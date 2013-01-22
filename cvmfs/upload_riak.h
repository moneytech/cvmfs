/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_UPLOAD_RIAK_H_
#define CVMFS_UPLOAD_RIAK_H_

#include <vector>

#include "upload.h"

#include "util_concurrency.h"

typedef void CURL;
struct curl_slist;

struct json_value;
typedef struct json_value JSON;

namespace upload {
  /**
   * The RiakSpooler implements an upstream backend adapter for a Riak key/value
   * storage (see http://basho.com/products/riak-overview/ for details).
   * It implements both the Process() and the Copy() functionality concurrently
   * using the ConcurrentWorkers template. Process() will first compress and hash
   * the given file and afterwards schedule an upload job based on the results
   * while Copy() will directly schedule an upload job. See upload_parameters for
   * more details.
   *
   * For a detailed interface description of this class please have a look at the
   * AbstractSpooler class which it is derived from.
   */
  class RiakSpooler : public AbstractSpooler {
   public:
    /**
     * Encapsulates an extendable memory buffer.
     * consecutive calls to Copy() will copy the given memory into the buffer
     * without overwriting the previously copied data. This is very handy for
     * cURL-style data handling callbacks.
     *
     * Note: this class might be useful for other parts of the code as well,
     *       and might end up as a Util somewhen.
     */
    struct DataBuffer {
      DataBuffer() : data(NULL), size_(0), offset_(0) {}
      ~DataBuffer() { free(data); data = NULL; size_ = 0; offset_ = 0; }

      bool           Reserve(const size_t bytes);
      unsigned char* Position() const;
      void           Copy(const unsigned char* ptr, const size_t bytes);

      unsigned char* data;
      size_t         size_;
      unsigned int   offset_;
    };

    //
    // -------------------------------------------------------------------------
    //

   protected:
    /**
     * Encapsulates the input data structure for the concurrent upload worker
     * with a number of different constructors for a variety of situations.
     * This structure might be generated by a call to Copy(), meaning that it
     * describes a direct copy operation from local storage to Riak.
     * Or it might be the result of a Process() operation, in which case it
     * contains information on how to upload a compressed and hashed file into
     * a Riak storage.
     * See the different constructors of this structure for more details.
     *
     * Note: This struct is derived from SpoolerResult and can therefore be
     *       directly used as asynchronous output through the NotifyListeners
     *       method of RiakSpooler (see the Observable template for details)
     */
    struct upload_parameters : public SpoolerResult {
      /**
       * Describes the differnt states of the upload_parameters structure, that
       * makes the upload worker behave differently depending on the way the
       * upload_parameters were generated
       *
       * Detailed Description:
       *   kPlainUpload:      The parameter structure was created by the Copy()
       *                      method of the RiakSpooler and therefore requests
       *                      to be uploaded to Riak directly. In particular it
       *                      does NOT contain a valid content_hash.
       *   kCompressedUpload: This parameter structure was created after the
       *                      file to be uploaded was piped through the Process()
       *                      method. It therefore is compressed and comes with
       *                      a content_hash that can be used for key creation.
       *   kEmpty:            This parameter is invalid and should be handled as
       *                      error state.
       */
      enum JobType {
        kPlainUpload,      //!< This file was not compressed (no content_hash)
        kCompressedUpload, //!< This file was compressed before (use content_hash)
        kEmpty             //!< THis is an invalid parameter structure (crash!)
      };

      /**
       * Constructor that creates a plain upload parameter structure, telling
       * the concurrent upload worker to simply push the file from local_path
       * into Riak using a key determined from remote_path.
       *
       * @param local_path   the path to the file to be uploaded
       * @param remote_path  the path the file should become visible in the
       *                     backend storage (actual Riak key will be derived
       *                     from that remote_path)
       * @param move         describes if the file should be moved
       *                     (currently not implemented)
       */
      upload_parameters(const std::string  &local_path,
                        const std::string  &remote_path,
                        const bool          move) :
        SpoolerResult(0, local_path),
        type(kPlainUpload),
        upload_source_path(local_path),
        remote_path(remote_path),
        move(move) {}

      /**
       * Constructor that creates an upload_parameter structure indicating an
       * error condition. Usually the return_code will be set different from
       * zero to indicate the type of error.
       *
       * @param return_code  the error code to be returned
       * @param local_path   the path to the file that produced the error state
       *                     while being processed
       */
      upload_parameters(const int           return_code,
                        const std::string  &local_path) :
        SpoolerResult(return_code, local_path),
        type(kEmpty),
        move(false) {}

      /**
       * This type of upload_parameter is generated after a Process() was per-
       * formed. It will contain information about a recently compressed file
       * that needs to be uploaded into the Riak backend storage.
       * It therefore is the handover-structure used to connect the Concurrent-
       * CompressionWorker with the ConcurrentUploadWorker.
       *
       * @param return_code      the resulting return code of the Concurrent-
       *                         CompressionWorker whose result is handed over
       *                         to the ConcurrentUploadWorker.
       * @param local_path       the local_path of the previously compressed file
       *                         Note: this is not needed for the upload itself
       *                               in this case but it identifies the job
       *                               when returned to the user
       * @param compressed_path  path to the compressed file which will be up-
       *                         loaded into the backend storage
       * @param content_hash     the content hash of the compressed data. This
       *                         will be used to derive a Riak key
       * @param file_suffx       suffix that is appended to the Riak key in
       *                         order to mark special files (such as catalogs)
       * @param move             describes if the file should be moved
       *                         (currently not implemented)
       */
      upload_parameters(const int           return_code,
                        const std::string  &local_path,
                        const std::string  &compressed_path,
                        const std::string  &remote_dir,
                        const hash::Any    &content_hash,
                        const std::string  &file_suffix,
                        const bool          move) :
        SpoolerResult(return_code, local_path, content_hash),
        type(kCompressedUpload),
        upload_source_path(compressed_path),
        remote_dir(remote_dir),
        file_suffix(file_suffix),
        move(move) {}

      /**
       * This constructor produces an empty upload_parameters structure which
       * is required by the implementation of the ConcurrentWorkers template
       * See the documentation of the ConcurrentWorkers template for details.
       */
      upload_parameters() :
        SpoolerResult(),
        type(kEmpty),
        move(false) {}

      /**
       * Generates a Riak key out of the information encapsulated inside the
       * upload_parameters structure. Based on the type of upload, a Riak key
       * might be derived from the content_hash or the given remote_path.
       *
       * @return  the Riak key to be used to store the file described in this
       *          upload_paramters structure
       */
      std::string GetRiakKey() const;

      const JobType     type;               //!< type specifier for this upload_parameters
      const std::string upload_source_path; //!< path to the source file to be uploaded
      const std::string remote_path;        //!< path where the final file should be found (only filled for type == kPlainUpload)
      const std::string remote_dir;         //!< path where to put the compressed file     (only filled for type == kCompressedUpload)
      const std::string file_suffix;        //!< suffix to append to the Riak key          (only filled for type == kCompressedUpload)
      const bool        move;               //!< should the file just be moved?
    };

    //
    // -------------------------------------------------------------------------
    //


    /**
     * The CompressionWorker implements the ConcurrentWorker interface and will
     * be concurrently executed by the ConcurrentWorkers template.
     * It allows for concurrent compression and hashing of files.
     *
     * See the documentation of the ConcurrentWorker base class for a detailed
     * interface description.
     */
    class CompressionWorker : public ConcurrentWorker<CompressionWorker> {
     public:
      typedef compression_parameters expected_data;
      typedef upload_parameters      returned_data;

      struct worker_context {
        worker_context(const std::string &temp_directory) :
          temp_directory(temp_directory) {}

        const std::string temp_directory; //!< where to store compression results (preferrable on a RAM disk)
      };

     public:
      CompressionWorker(const worker_context *context);
      void operator()(const expected_data &input);

     protected:
      /**
       * Compresses the given source file to a temporary file location. Add-
       * itionally it computes a rolling hash of the compressed data. The
       * results are provided through output parameters.
       *
       * @param source_file_path  path to the file to be compressed
       * @param destination_dir   path to the directory where to put the
       *                          compressed file
       * @param tmp_file_path     output parameter: will contain the path to
       *                                            the compressed file
       * @param content_hash      output parameter: will contain the computed
       *                                            content has of the compres-
       *                                            sed data.
       * @return  true on success
       */
      bool CompressToTempFile(const std::string &source_file_path,
                              const std::string &destination_dir,
                              std::string       *tmp_file_path,
                              hash::Any         *content_hash) const;

     private:
      StopWatch         compression_stopwatch_;       //!< time measurement for single file compression time
      double            compression_time_aggregated_; //!< aggregates time measurements for all files
      const std::string temp_directory_;              //!< temporary results storage location
    };

    //
    // -------------------------------------------------------------------------
    //

    /**
     * Implementatino of ConcurrentWorker that pushes files into a Riak storage
     * Currently this worker is based on cURL and uses the HTTP interface of
     * Riak. Possibly this will be extended by a Protocol Buffer implementation.
     */
    class UploadWorker : public ConcurrentWorker<UploadWorker> {
     public:
      typedef upload_parameters      expected_data;
      typedef SpoolerResult          returned_data;

      /**
       * Concurrent UploadWorkers need to communicate through this context
       * object, therefore it implements the Lockable interface to be used as a
       * mutex object in conjuction with the LockGuard template.
       */
      struct worker_context : public Lockable {
        worker_context(const std::vector<std::string> &upstream_urls) :
          upstream_urls(upstream_urls),
          next_upstream_url_(0) {}

        /**
         * Provides each concurrent UploadWorker with an upstream URL to one of
         * the configured Riak cluster instances. Upstream URLs are handed to the
         * upload workers in a round robin scheme.
         *
         * @return  an URL to a Riak cluster instance
         */
        const std::string& AcquireUpstreamUrl() const;

        const std::vector<std::string> upstream_urls;      //!< list of available upstream URLs
        mutable unsigned int           next_upstream_url_; //!< state variable for the round robin allocation
      };

     public:
      UploadWorker(const worker_context *context);
      void operator()(const expected_data &input);

      bool Initialize();
      void TearDown();

     protected:
      bool InitUploadHandle();
      bool InitDownloadHandle();

      /**
       * Performs a read action to a Riak cluster and obtains the vector clock
       * for a already present key. If the key is not present no vector clock
       * is returned.
       *
       * @param key           the Riak key to be queried
       * @param vector_clock  output parameter: is set to the vector clock string
       *                                        after a successful read
       * @return  true if the key was found (and a vector clock was set), other-
       *          wise the vector clock is not set. False could also mean failure
       */
      bool GetVectorClock(const std::string &key, std::string &vector_clock);

      /**
       * Pushes a file into the Riak data store under a given key. Furthermore
       * uploads can be marked as 'critical' meaning that they are ensured to be
       * consistent after the upload finished (w=all, dw=all)
       *
       * @param key          the key which should reference the data in the file
       * @param file_path    the path to the file to be stored into Riak
       * @param is_critical  a flag marking files as 'critical' (default = false)
       * @return             0 on success, > 0 otherwise
       */
      int PushFileToRiak(const std::string &key,
                         const std::string &file_path,
                         const bool         is_critical = false);

      /*
       * Generates a request URL out of the known Riak base URL and the given key.
       * Additionally it can set the W-value to 'all' if a consistent write must
       * be ensured. (see http://docs.basho.com/riak/1.2.1/tutorials/
       *                  fast-track/Tunable-CAP-Controls-in-Riak/ for details)
       * @param key          the key where the request URL should point to
       * @param is_critical  set to true if a consistent write is desired
       *                     (sets Riak's w_val to 'all')
       * @return             the final request URL string
       */
      std::string CreateRequestUrl(const std::string &key,
                                   const bool         is_critical = false) const;

      typedef size_t (*UploadCallback)(void*, size_t, size_t, void*);

      /**
       * Configures the cURL_easy_handle for upload a planned upload of a file
       * into the Riak storage. This might include a read operation to a Riak
       * node in order to obtain the current vector clock of the entry to be up-
       * loaded.
       *
       * @param key        the Riak key to be created (or updated)
       * @param url        the full-blown url to send the request to
       * @param headers    a cURL list of headers to be sent
       * @param data_size  the file size of the file to be uploaded
       * @param callback   a pointer to a callback function to obtain data from
       *                   (can be used to upload a memory buffer)
       *                   can be set to NULL to upload a file
       * @param userdata   a pointer to the file handle to be uploaded or to
       *                   some user data to be passed to the callback
       * @return  true on successful configuration
       */
      bool ConfigureUpload(const std::string   &key,
                           const std::string   &url,
                           struct curl_slist   *headers,
                           const size_t         data_size,
                           const UploadCallback callback,
                           const void*          userdata);
      bool CheckUploadSuccess(const int file_size);

      bool CollectUploadStatistics();
      bool CollectVclockFetchStatistics();

      /**
       * cURL callback to extract the vector clock header from a received list
       * of headers.
       */
      static size_t ObtainVclockCallback(void *ptr,
                                         size_t size,
                                         size_t nmemb,
                                         void *userdata);

     private:
      // general state information
      const std::string upstream_url_;

      // CURL state
      CURL              *curl_upload_;
      CURL              *curl_download_;
      struct curl_slist *http_headers_download_;

      // instrumentation
      StopWatch upload_stopwatch_;
      double    upload_time_aggregated_;
      double    curl_upload_time_aggregated_;
      double    curl_get_vclock_time_aggregated_;
      double    curl_connection_time_aggregated_;
      int       curl_connections_;
      double    curl_upload_speed_aggregated_;
    };


   public:
    virtual ~RiakSpooler();

    /**
     * Schedules an asynchronous upload to a Riak storage.
     *
     * @param local_path   path to the file to be directly uploaded into Riak
     * @param remote_path  used to determine the Riak key to make the file avail-
     *                     able under a certain remote_path in Riak
     */
    void Copy(const std::string &local_path,
              const std::string &remote_path);

    /**
     * Schedules an asynchronous compression and hashing job, which in turn will
     * schedule an asynchronous upload job when successfully finished. The final
     * result is, that the given file will be stored under its content hash into
     * the Riak backend storage.
     *
     * @param local_path   path to the file to be processed
     * @param remote_dir   remote directory where the file should end up in
     * @param file_suffix  suffix to be attached to the final Riak key
     */
    virtual void ProcessChunk(const std::string   &local_path,
                              const std::string   &remote_dir,
                              const unsigned long  offset,
                              const unsigned long  length);

    void EndOfTransaction();
    void WaitForUpload() const;
    void WaitForTermination() const;

    unsigned int GetNumberOfErrors() const;

   protected:
    friend class AbstractSpooler;
    RiakSpooler(const SpoolerDefinition &spooler_definition);

    bool Initialize();
    void TearDown();

    /**
     * Callback method for the concurrent CompressionWorker
     * Will schedule an upload job to push the results of the compression into
     * Riak.
     */
    void CompressionWorkerCallback(const CompressionWorker::returned_data &data);

    /**
     * Callback method for the concurrent UploadWorker
     * Will inform the user about the outcome of a scheduled job.
     */
    void UploadWorkerCallback(const UploadWorker::returned_data &data);

   private:
    /**
     * Checks if the configuration of the Riak cluster conforms to our require-
     * ments.
     *
     * @param url  the URL of one of the cluster nodes to be checked
     * @return     true if the configuration is sound
     */
    static bool  CheckRiakConfiguration(const std::string &url);

    /**
     * Downloads the configuration information from the Riak cluster.
     *
     * @param url     the URL to one of the Riak cluster nodes to be checked
     * @param buffer  a DataBuffer to store the results into
     * @return  true on successful download
     */
    static bool  DownloadRiakConfiguration(const std::string &url,
                                           DataBuffer& buffer);

    /**
     * Parses the obtained configuration information as a JSON string and pro-
     * vides a simple JSON structure for further investigation. We use the slim
     * JSON library vjson for parsing (see: http://code.google.com/p/vjson/)
     *
     * @param buffer  the DataBuffer object containing the JSON string
     * @return  a pointer to a vjson JSON structure
     */
    static JSON* ParseJsonConfiguration(DataBuffer& buffer);

    /**
     * Checks the JSON configuration obtained from Riak and makes sure that it
     * is configured as we expect it to be.
     *
     * @param json_root  the JSON structure obtained from ParseJsonConfiguration
     * @return  true if the configuration matches or requirements
     */
    static bool CheckJsonConfiguration(const JSON *json_root);

   private:
    // concurrency objects
    UniquePtr<ConcurrentWorkers<CompressionWorker> > concurrent_compression_;
    UniquePtr<ConcurrentWorkers<UploadWorker> >      concurrent_upload_;

    UniquePtr<CompressionWorker::worker_context>     compression_context_;
    UniquePtr<UploadWorker::worker_context>          upload_context_;
  };
}

#endif /* CVMFS_UPLOAD_RIAK_H_ */
