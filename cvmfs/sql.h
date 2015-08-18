/**
 * This file is part of the CernVM file system.
 */

#ifndef CVMFS_SQL_H_
#define CVMFS_SQL_H_

#include <string>

#include "duplex_sqlite3.h"
#include "util.h"

namespace sqlite {

class Sql;

/**
 * Encapsulates an SQlite connection.
 *
 * This is an abstract base class for different SQLite database flavours used
 * throughout CernVM-FS. It provides a general interface for creating, opening,
 * compacting and migrating an SQLite database.
 * Furthermore it manages a `properties` table in each database, to store simple
 * key-value style information in a common fashion. For that, it offers the
 * templated methods SetProperty(), GetProperty<>() and HasProperty() that take
 * all common data types and persist it in the database.
 *
 * By default Database<> objects do not take ownership of the underlying SQLite
 * database file and hence do not unlink it on database closure. If the using
 * code calls Database<>::TakeFileOwnership() the SQLite file will be unlinked
 * in the destructor of the Database<> object.
 *
 * Note: This implements a Curiously Recurring Template Pattern in order to
 *       implement Database::Create and Database::Open as a static polymorphism
 *
 *  The following methods need to be implemented by each subclass:
 *  (Database<> assumes 'true' as a return value on success)
 *
 *  -> bool CreateEmptyDatabase()
 *              creates all necessary SQLite tables for the concrete Database
 *              implementation. Furthermore it can insert default data into the
 *              newly created database tables.
 *  -> bool CheckSchemaCompatibility()
 *              checks a database for compatibility directly after opening it.
 *              Database<> provides schema_version() and schema_revision() to
 *              access the compatibility stored in the `properties` table
 *  -> bool LiveSchemaUpgradeIfNecessary()
 *              this allows for on-the-fly schema updates and is always called
 *              when a database is opened read/write. It assumes 'true' both on
 *              successful migration and if no migration was necessary
 *  -> bool CompactDatabase()
 *              here implementation specific cleanup actions can take place on
 *              databases opened as read/write. It is invoked by the `Vacuum()`
 *              method, that can be used by higher level user code
 *
 *  Furthermore Database<> expects two static constants to be defined:
 *
 *  -> kLatestSchema  - the newest schema version generated by invoking
 *                      DerivedT::CreateEmptyDatabase()
 *  -> kLatestSchemaRevision - same as kLatestSchema, however different schema
 *                             revisions are supposed to be backward compatible
 *                             or on-the-fly updateable by
 *                             DerivedT::LiveSchemaUpgradeIfNecessary()
 *
 * @param DerivedT  the name of the inheriting Database implementation class
 *                  (Curiously Recurring Template Pattern)
 *
 * TODO(rmeusel): C++11 Move Constructors to allow for stack allocated databases
 */
template <class DerivedT>
class Database : SingleCopy {
 public:
  enum OpenMode {
    kOpenReadOnly,
    kOpenReadWrite,
  };

  static const float kSchemaEpsilon;  // floats get imprecise in SQlite

  /**
   * Creates a new database file of the type implemented by DerivedT. During the
   * invocation of this static method DerivedT::CreateEmptyDatabase() is called.
   *
   * @param filename  the file location of the newly created database
   *                  (file does not need to exist)
   * @return          an empty database of type DerivedT (or NULL on failure)
   */
  static DerivedT* Create(const std::string  &filename);

  /**
   * Opens a database file and assumes it to be of type DerivedT. This method
   * will call DerivedT::CheckSchemaCompatibility() to figure out readability of
   * the contained schema revision. Furthermore, if the database was opened in
   * read/write mode, it calls DerivedT::LiveSchemaUpgradeIfNecessary() to allow
   * for on-the-fly schema upgrades of the underlying database file.
   *
   * @param filename   path to the SQLite file to be opened as DerivedT
   * @param open_mode  kOpenReadOnly or kOpenReadWrite open modes
   * @return           a database of type DerivedT (or NULL on failure)
   */
  static DerivedT* Open(const std::string  &filename,
                        const OpenMode      open_mode);

  bool IsEqualSchema(const float value, const float compare) const {
    return (value > compare - kSchemaEpsilon &&
            value < compare + kSchemaEpsilon);
  }

  bool BeginTransaction() const;
  bool CommitTransaction() const;

  template <typename T>
  T GetProperty(const std::string &key) const;
  template <typename T>
  T GetPropertyDefault(const std::string &key, const T default_value) const;
  template <typename T>
  bool SetProperty(const std::string &key, const T value);
  bool HasProperty(const std::string &key) const;

  sqlite3*            sqlite_db()       const { return database_.database();  }
  const std::string&  filename()        const { return database_.filename();  }
  float               schema_version()  const { return schema_version_;       }
  unsigned            schema_revision() const { return schema_revision_;      }
  bool                read_write()      const { return read_write_;           }

  /**
   * Figures out the ratio of free SQLite memory pages in the SQLite database
   * file. A high ratio can be an indication of a necessary call to Vacuum().
   * Note: This is not done automatically and the decision is left to the using
   *       code!
   * @return  the free-page ratio in the opened database file (free pages/pages)
   */
  double GetFreePageRatio() const;

  /**
   * Performs a VACUUM call on the opened database file to compacts the database.
   * As a first step it runs DerivedT::CompactDatabase() to allow for implement-
   * ation dependent cleanup actions. Vacuum() assumes that the SQLite database
   * was opened in read/write mode.
   * @return  true on success
   */
  bool Vacuum() const;

  /**
   * Prints the given error message, together with the last encountered SQLite
   * error of this database.
   * @param error_msg  an error message to be printed along with the SQL error
   */
  void PrintSqlError(const std::string &error_msg);

  /**
   * Returns the english language error description of the last error
   * happened in the context of the encapsulated sqlite3 database object.
   * Note: In a multithreaded context it might be unpredictable which
   *       the actual last error is.
   * @return   english language error description of last error
   */
  std::string GetLastErrorMsg() const;


  /**
   * Transfers the ownership of the SQLite database file to the Database<> ob-
   * ject. Hence, it will automatically unlink the file, once the Database<>
   * object goes out of scope or is deleted.
   */
  void TakeFileOwnership();

  /**
   * Resigns from the ownership of the SQLite database file underlying this
   * Database<> object. After calling this the using code is responsible of
   * managing the database file.
   */
  void DropFileOwnership();

  /**
   * Check if the SQLite database file is managed by the Database<> object
   * Note: unmanaged means, that the using code needs to take care of the file
   *       management (i.e. delete the file after usage)
   *
   * @return  false if file is unmanaged
   */
  bool OwnsFile() const { return database_.OwnsFile(); }

 protected:
  /**
   * Private constructor! Use the factory methods DerivedT::Create() or
   * DerivedT::Open() to instantiate a database object of type DerivedT.
   */
  Database(const std::string  &filename,
           const OpenMode      open_mode);

  bool Initialize();

  bool CreatePropertiesTable();
  bool PrepareCommonQueries();

  bool OpenDatabase(const int sqlite_open_flags);
  bool Configure();
  bool FileReadAhead();

  void ReadSchemaRevision();
  bool StoreSchemaRevision();

  void set_schema_version(const float ver)     { schema_version_  = ver; }
  void set_schema_revision(const unsigned rev) { schema_revision_ = rev; }

 private:
  /**
   * This wraps the opaque SQLite database object along with a file unlink guard
   * to control the life time of the database connection and the database file
   * in an RAII fashion.
   */
  struct DatabaseRaiiWrapper {
    DatabaseRaiiWrapper(const std::string   &filename,
                        Database<DerivedT>  *delegate)
      : sqlite_db(NULL)
      , db_file_guard(filename, UnlinkGuard::kDisabled)
      , delegate_(delegate) {}
    ~DatabaseRaiiWrapper();

    sqlite3*           database() const { return sqlite_db;            }
    const std::string& filename() const { return db_file_guard.path(); }

    void TakeFileOwnership() { db_file_guard.Enable();           }
    void DropFileOwnership() { db_file_guard.Disable();          }
    bool OwnsFile() const    { return db_file_guard.IsEnabled(); }

    sqlite3             *sqlite_db;
    UnlinkGuard          db_file_guard;
    Database<DerivedT>  *delegate_;
  };

  static const std::string kSchemaVersionKey;
  static const std::string kSchemaRevisionKey;

  DatabaseRaiiWrapper database_;

  const bool          read_write_;
  float               schema_version_;
  unsigned            schema_revision_;

  UniquePtr<Sql>      begin_transaction_;
  UniquePtr<Sql>      commit_transaction_;

  UniquePtr<Sql>      has_property_;
  UniquePtr<Sql>      set_property_;
  UniquePtr<Sql>      get_property_;
};


//
// # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
//


/**
 * Base class for all SQL statement classes.  It wraps a single SQL statement
 * and all neccessary calls of the sqlite3 API to deal with this statement.
 */
class Sql {
 public:
  /**
   * Basic constructor to use this class for a specific statement.
   * @param database the database to use the query on
   * @param statement the statement to prepare
   */
  Sql(sqlite3 *sqlite_db, const std::string &statement);
  virtual ~Sql();

  bool Execute();
  bool FetchRow();
  std::string DebugResultTable();
  bool Reset();
  inline int GetLastError() const { return last_error_code_; }

  /**
   * Returns the english language error description of the last error
   * happened in the context of the sqlite3 database object this statement is
   * registered to.
   * Note: In a multithreaded context it might be unpredictable which
   *       the actual last error is.
   * @return   english language error description of last error
   */
  std::string GetLastErrorMsg() const;

  bool BindBlob(const int index, const void* value, const int size) {
    last_error_code_ = sqlite3_bind_blob(statement_, index, value, size,
                                         SQLITE_STATIC);
    return Successful();
  }
  bool BindBlobTransient(const int index, const void* value, const int size) {
    last_error_code_ = sqlite3_bind_blob(statement_, index, value, size,
                                         SQLITE_TRANSIENT);
    return Successful();
  }
  bool BindDouble(const int index, const double value) {
    last_error_code_ = sqlite3_bind_double(statement_, index, value);
    return Successful();
  }
  bool BindInt(const int index, const int value) {
    last_error_code_ = sqlite3_bind_int(statement_, index, value);
    return Successful();
  }
  bool BindInt64(const int index, const sqlite3_int64 value) {
    last_error_code_ = sqlite3_bind_int64(statement_, index, value);
    return Successful();
  }
  bool BindNull(const int index) {
    last_error_code_ = sqlite3_bind_null(statement_, index);
    return Successful();
  }
  bool BindTextTransient(const int index, const std::string &value) {
    return BindTextTransient(index, value.data(), value.length());
  }
  bool BindTextTransient(const int index, const char *value, const int size) {
    return BindText(index, value, size, SQLITE_TRANSIENT);
  }
  bool BindText(const int index, const std::string &value) {
    return BindText(index, value.data(), value.length(), SQLITE_STATIC);
  }
  bool BindText(const int   index,
                const char* value,
                const int   size,
                void(*dtor)(void*) = SQLITE_STATIC) {
    last_error_code_ = sqlite3_bind_text(statement_, index, value, size, dtor);
    return Successful();
  }

  /**
   * Figures out the type to be bound by template parameter deduction
   * NOTE: For strings or char* buffers this is suboptimal, since it needs to
   *       assume that the provided buffer is transient and copy it to be sure.
   *       Furthermore, for char* buffers we need to assume a null-terminated
   *       C-like string to obtain its length using strlen().
   */
  template <typename T>
  inline bool Bind(const int index, const T value);


  int RetrieveType(const int idx_column) const {
    return sqlite3_column_type(statement_, idx_column);
  }

  /**
   * Determines the number of bytes necessary to store the column's data as a
   * string. This might involve type conversions and depends on which other
   * RetrieveXXX methods were called on the same column index before!
   *
   * See SQLite documentation for sqlite_column_bytes() for details:
   *   https://www.sqlite.org/c3ref/column_blob.html
   */
  int RetrieveBytes(const int idx_column) const {
    return sqlite3_column_bytes(statement_, idx_column);
  }
  const void *RetrieveBlob(const int idx_column) const {
    return sqlite3_column_blob(statement_, idx_column);
  }
  double RetrieveDouble(const int idx_column) const {
    return sqlite3_column_double(statement_, idx_column);
  }
  int RetrieveInt(const int idx_column) const {
    return sqlite3_column_int(statement_, idx_column);
  }
  sqlite3_int64 RetrieveInt64(const int idx_column) const {
    return sqlite3_column_int64(statement_, idx_column);
  }
  const unsigned char *RetrieveText(const int idx_column) const {
    return sqlite3_column_text(statement_, idx_column);
  }
  std::string RetrieveString(const int idx_column) const {
    return reinterpret_cast<const char*>(RetrieveText(idx_column));
  }
  template <typename T>
  inline T Retrieve(const int index);

 protected:
  Sql() : statement_(NULL), last_error_code_(0) { }
  bool Init(const sqlite3 *database, const std::string &statement);

  /**
   * Checks the last action for success
   * @return true if last action succeeded otherwise false
   */
  inline bool Successful() const {
    return SQLITE_OK   == last_error_code_ ||
           SQLITE_ROW  == last_error_code_ ||
           SQLITE_DONE == last_error_code_;
  }

  sqlite3_stmt *statement_;
  int last_error_code_;
};

}  // namespace sqlite

#include "sql_impl.h"

#endif  // CVMFS_SQL_H_
