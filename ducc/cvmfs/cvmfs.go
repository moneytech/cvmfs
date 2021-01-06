package cvmfs

import (
	"archive/tar"
	"encoding/json"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/sys/unix"

	copy "github.com/otiai10/copy"
	"github.com/pkg/xattr"
	log "github.com/sirupsen/logrus"

	constants "github.com/cvmfs/ducc/constants"
	da "github.com/cvmfs/ducc/docker-api"
	fs "github.com/cvmfs/ducc/filesystem"
	l "github.com/cvmfs/ducc/log"
	temp "github.com/cvmfs/ducc/temp"
)

// ingest into the repository, inside the subpath, the target (directory or file) object
// CVMFSRepo: just the name of the repository (ex: unpacked.cern.ch)
// path: the path inside the repository, without the prefix (ex: .foo/bar/baz), where to put the ingested target
// target: the path of the target in the normal FS, the thing to ingest
// if no error is returned, we remove the target from the FS
func PublishToCVMFS(CVMFSRepo string, path string, target string) (err error) {
	defer func() {
		if err == nil {
			l.Log().WithFields(log.Fields{"target": target, "action": "ingesting"}).Info("Deleting temporary directory")
			os.RemoveAll(target)
		}
	}()
	l.Log().WithFields(log.Fields{"target": target, "action": "ingesting"}).Info("Start ingesting")

	path = filepath.Join("/", "cvmfs", CVMFSRepo, path)

	l.Log().WithFields(log.Fields{"target": target, "action": "ingesting"}).Info("Start transaction")

	err = WithinTransaction(CVMFSRepo, func() error {
		l.Log().WithFields(log.Fields{"target": target, "path": path, "action": "ingesting"}).Info("Copying target into path")

		targetStat, err := os.Stat(target)
		if err != nil {
			l.LogE(err).WithFields(log.Fields{"target": target}).Error("Impossible to obtain information about the target")
			return err
		}

		if targetStat.Mode().IsDir() {
			os.RemoveAll(path)
			err = os.MkdirAll(path, constants.DirPermision)
			if err != nil {
				l.LogE(err).WithFields(log.Fields{"repo": CVMFSRepo}).Warning("Error in creating the directory where to copy the singularity")
			}
			err = copy.Copy(target, path, copy.Options{PreserveTimes: true})

		} else if targetStat.Mode().IsRegular() {
			err = func() error {
				os.MkdirAll(filepath.Dir(path), constants.DirPermision)
				os.Remove(path)

				from, err := os.Open(target)
				if err != nil {
					return err
				}
				defer from.Close()

				to, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE, constants.FilePermision)
				if err != nil {
					return err
				}
				defer to.Close()

				_, err = io.Copy(to, from)
				return err
			}()
		} else {
			err = fmt.Errorf("Trying to ingest neither a file nor a directory")
		}

		if err != nil {
			l.LogE(err).WithFields(log.Fields{"repo": CVMFSRepo, "target": target}).Error("Error in moving the target inside the CVMFS repo")
			return err
		}
		return nil
	})

	return err
}

// create a symbolic link inside the repository called `newLinkName`, the symlink will point to `toLinkPath`
// newLinkName: comes without the /cvmfs/$REPO/ prefix
// toLinkPath: comes without the /cvmfs/$REPO/ prefix
func CreateSymlinkIntoCVMFS(CVMFSRepo, newLinkName, toLinkPath string) (err error) {
	// add the necessary prefix
	newLinkName = filepath.Join("/", "cvmfs", CVMFSRepo, newLinkName)
	toLinkPath = filepath.Join("/", "cvmfs", CVMFSRepo, toLinkPath)

	llog := func(l *log.Entry) *log.Entry {
		return l.WithFields(log.Fields{"action": "save backlink",
			"repo":           CVMFSRepo,
			"link name":      newLinkName,
			"target to link": toLinkPath})
	}

	// check if the file we want to link actually exists
	if _, err := os.Stat(toLinkPath); os.IsNotExist(err) {
		return err
	}

	relativePath, err := filepath.Rel(newLinkName, toLinkPath)
	if err != nil {
		llog(l.LogE(err)).Error("Error in find the relative path")
		return err
	}
	// from the relativePath we remove the first part of the path.
	// The part we remove reprensent the same directory where is the target.
	linkChunks := strings.Split(relativePath, string(os.PathSeparator))
	link := filepath.Join(linkChunks[1:]...)

	err = WithinTransaction(CVMFSRepo, func() error {
		linkDir := filepath.Dir(newLinkName)
		err = os.MkdirAll(linkDir, constants.DirPermision)
		if err != nil {
			llog(l.LogE(err)).WithFields(log.Fields{
				"directory": linkDir}).Error(
				"Error in creating the directory where to store the symlink")
			return err
		}

		// the symlink exists already, we delete it and replace it
		if lstat, err := os.Lstat(newLinkName); !os.IsNotExist(err) {
			if lstat.Mode()&os.ModeSymlink != 0 {
				// the file exists and it is a symlink, we overwrite it
				err = os.Remove(newLinkName)
				if err != nil {
					err = fmt.Errorf("Error in removing existsing symlink: %s", err)
					llog(l.LogE(err)).Error("Error in removing previous symlink")
					return err
				}
			} else {
				// the file exists but is not a symlink
				err = fmt.Errorf(
					"Error, trying to overwrite with a symlink something that is not a symlink")
				llog(l.LogE(err)).Error("Error in creating a symlink")
				return err
			}
		}

		err = os.Symlink(link, newLinkName)
		if err != nil {
			llog(l.LogE(err)).Error(
				"Error in creating the symlink")
			return err
		}
		return nil
	})

	return err
}

type Backlink struct {
	Origin []string `json:"origin"`
}

func GetBacklinkPath(CVMFSRepo, layerDigest string) string {
	return filepath.Join(LayerMetadataPath(CVMFSRepo, layerDigest), "origin.json")
}

func GetBacklinkFromLayer(CVMFSRepo, layerDigest string) (backlink Backlink, err error) {
	backlinkPath := GetBacklinkPath(CVMFSRepo, layerDigest)
	llog := func(l *log.Entry) *log.Entry {
		return l.WithFields(log.Fields{"action": "get backlink from layer",
			"repo":         CVMFSRepo,
			"layer":        layerDigest,
			"backlinkPath": backlinkPath})
	}

	if _, err := os.Stat(backlinkPath); os.IsNotExist(err) {
		return Backlink{Origin: []string{}}, nil
	}

	backlinkFile, err := os.Open(backlinkPath)
	if err != nil {
		llog(l.LogE(err)).Error(
			"Error in opening the file for writing the backlinks, skipping...")
		return
	}

	byteBackLink, err := ioutil.ReadAll(backlinkFile)
	if err != nil {
		llog(l.LogE(err)).Error(
			"Error in reading the bytes from the origin file, skipping...")
		return
	}

	err = backlinkFile.Close()
	if err != nil {
		llog(l.LogE(err)).Error(
			"Error in closing the file after reading, moving on...")
		return
	}

	err = json.Unmarshal(byteBackLink, &backlink)
	if err != nil {
		llog(l.LogE(err)).Error(
			"Error in unmarshaling the files, skipping...")
		return
	}
	return
}

func SaveLayersBacklink(CVMFSRepo string, imgManifest da.Manifest, imageName string, layerDigest []string) error {
	llog := func(l *log.Entry) *log.Entry {
		return l.WithFields(log.Fields{"action": "save backlink",
			"repo":  CVMFSRepo,
			"image": imageName})
	}

	llog(l.Log()).Info("Start saving backlinks")

	backlinks := make(map[string][]byte)

	for _, layerDigest := range layerDigest {
		imgDigest := imgManifest.Config.Digest

		backlink, err := GetBacklinkFromLayer(CVMFSRepo, layerDigest)
		if err != nil {
			llog(l.LogE(err)).WithFields(log.Fields{"layer": layerDigest}).Error(
				"Error in obtaining the backlink from a layer digest, skipping...")
			continue
		}
		backlink.Origin = append(backlink.Origin, imgDigest)

		backlinkBytesMarshal, err := json.Marshal(backlink)
		if err != nil {
			llog(l.LogE(err)).WithFields(log.Fields{"layer": layerDigest}).Error(
				"Error in Marshaling back the files, skipping...")
			continue
		}

		backlinkPath := GetBacklinkPath(CVMFSRepo, layerDigest)
		backlinks[backlinkPath] = backlinkBytesMarshal
	}

	llog(l.Log()).Info("Start transaction")
	err := WithinTransaction(CVMFSRepo, func() error {

		for path, fileContent := range backlinks {
			// the path may not be there, check,
			// and if it doesn't exists create it
			dir := filepath.Dir(path)
			if _, err := os.Stat(dir); os.IsNotExist(err) {
				err = os.MkdirAll(dir, constants.DirPermision)
				if err != nil {
					llog(l.LogE(err)).WithFields(
						log.Fields{"file": path}).
						Error("Error in creating the directory for the backlinks file, skipping...")
					continue
				}
			}
			err := ioutil.WriteFile(path, fileContent, constants.FilePermision)
			if err != nil {
				llog(l.LogE(err)).WithFields(log.Fields{"file": path}).Error(
					"Error in writing the backlink file, skipping...")
				continue
			}
			llog(l.LogE(err)).WithFields(log.Fields{"file": path}).Info(
				"Wrote backlink")
		}
		return nil
	})

	return err
}

func RemoveScheduleLocation(CVMFSRepo string) string {
	return filepath.Join("/", "cvmfs", CVMFSRepo, ".metadata", "remove-schedule.json")
}

func AddManifestToRemoveScheduler(CVMFSRepo string, manifest da.Manifest) error {
	schedulePath := RemoveScheduleLocation(CVMFSRepo)
	llog := func(l *log.Entry) *log.Entry {
		return l.WithFields(log.Fields{
			"action": "add manifest to remove schedule",
			"file":   schedulePath})
	}
	var schedule []da.Manifest

	// if the file exist, load from it
	if _, err := os.Stat(schedulePath); !os.IsNotExist(err) {

		scheduleFileRO, err := os.OpenFile(schedulePath, os.O_RDONLY, constants.FilePermision)
		if err != nil {
			llog(l.LogE(err)).Error("Impossible to open the schedule file")
			return err
		}

		scheduleBytes, err := ioutil.ReadAll(scheduleFileRO)
		if err != nil {
			llog(l.LogE(err)).Error("Impossible to read the schedule file")
			return err
		}

		err = scheduleFileRO.Close()
		if err != nil {
			llog(l.LogE(err)).Error("Impossible to close the schedule file")
			return err
		}

		err = json.Unmarshal(scheduleBytes, &schedule)
		if err != nil {
			llog(l.LogE(err)).Error("Impossible to unmarshal the schedule file")
			return err
		}
	}

	schedule = func() []da.Manifest {
		for _, m := range schedule {
			if m.Config.Digest == manifest.Config.Digest {
				return schedule
			}
		}
		schedule = append(schedule, manifest)
		return schedule
	}()

	err := WithinTransaction(CVMFSRepo, func() error {
		if _, err := os.Stat(schedulePath); os.IsNotExist(err) {
			err = os.MkdirAll(filepath.Dir(schedulePath), constants.DirPermision)
			if err != nil {
				llog(l.LogE(err)).Error("Error in creating the directory where save the schedule")
			}
		}

		bytes, err := json.Marshal(schedule)
		if err != nil {
			llog(l.LogE(err)).Error("Error in marshaling the new schedule")
		} else {

			err = ioutil.WriteFile(schedulePath, bytes, constants.FilePermision)
			if err != nil {
				llog(l.LogE(err)).Error("Error in writing the new schedule")
			} else {
				llog(l.Log()).Info("Wrote new remove schedule")
			}
		}
		return nil
	})

	return err
}

func RemoveSingularityImageFromManifest(CVMFSRepo string, manifest da.Manifest) error {
	dir := filepath.Join("/", "cvmfs", CVMFSRepo, manifest.GetSingularityPath())
	llog := func(l *log.Entry) *log.Entry {
		return l.WithFields(log.Fields{
			"action": "removing singularity directory", "directory": dir})
	}
	err := RemoveDirectory(CVMFSRepo, manifest.GetSingularityPath())
	if err != nil {
		llog(l.LogE(err)).Error("Error in removing singularity direcotry")
		return err
	}
	return nil
}

func LayerPath(CVMFSRepo, layerDigest string) string {
	return filepath.Join("/", "cvmfs", CVMFSRepo, constants.SubDirInsideRepo, layerDigest[0:2], layerDigest)
}

func ChainPath(CVMFSRepo, layerDigest string) string {
	layerDigest = removeHashMarkerIfPresent(layerDigest)
	return filepath.Join("/", "cvmfs", CVMFSRepo, constants.ChainSubDir, layerDigest[0:2], layerDigest)
}

func LayerRootfsPath(CVMFSRepo, layerDigest string) string {
	return filepath.Join(LayerPath(CVMFSRepo, layerDigest), "layerfs")
}

func LayerMetadataPath(CVMFSRepo, layerDigest string) string {
	return filepath.Join(LayerPath(CVMFSRepo, layerDigest), ".metadata")
}

//from /cvmfs/$REPO/foo/bar -> foo/bar
func TrimCVMFSRepoPrefix(path string) string {
	return strings.Join(strings.Split(path, string(os.PathSeparator))[3:], string(os.PathSeparator))
}

func RemoveLayer(CVMFSRepo, layerDigest string) error {
	llog := func(l *log.Entry) *log.Entry {
		return l.WithFields(log.Fields{
			"action": "removing layer", "layer": layerDigest})
	}
	err := RemoveDirectory(CVMFSRepo, constants.SubDirInsideRepo, layerDigest[0:2], layerDigest)
	if err != nil {
		llog(l.LogE(err)).Error("Error in deleting a layer")
		return err
	}
	return nil
}

func RemoveDirectory(CVMFSRepo string, dirPath ...string) error {
	path := []string{"/cvmfs", CVMFSRepo}
	for _, p := range dirPath {
		path = append(path, p)
	}
	directory := filepath.Join(path...)
	llog := func(l *log.Entry) *log.Entry {
		return l.WithFields(log.Fields{
			"action": "removing directory", "directory": directory})
	}
	stat, err := os.Stat(directory)
	if err != nil {
		if os.IsNotExist(err) {
			llog(l.LogE(err)).Warning("Directory not existing")
			return nil
		}
		llog(l.LogE(err)).Error("Error in stating the directory")
		return err
	}
	if !stat.Mode().IsDir() {
		err = fmt.Errorf("Trying to remove something different from a directory")
		llog(l.LogE(err)).Error("Error, input is not a directory")
		return err
	}

	dirsSplitted := strings.Split(directory, string(os.PathSeparator))
	if len(dirsSplitted) <= 3 || dirsSplitted[1] != "cvmfs" {
		err := fmt.Errorf("Directory not in the CVMFS repo")
		llog(l.LogE(err)).Error("Error in opening the transaction")
		return err
	}
	err = WithinTransaction(CVMFSRepo, func() error {
		err := os.RemoveAll(directory)
		if err != nil {
			llog(l.LogE(err)).Error("Error in publishing after adding the backlinks")
		}
		return err
	})

	return err
}

func CreateCatalogIntoDir(CVMFSRepo, dir string) (err error) {
	catalogPath := filepath.Join("/", "cvmfs", CVMFSRepo, dir, ".cvmfscatalog")
	if _, err := os.Stat(catalogPath); os.IsNotExist(err) {
		tmpFile, err := ioutil.TempFile("", "tempCatalog")
		tmpFile.Close()
		if err != nil {
			return err
		}
		err = PublishToCVMFS(CVMFSRepo, TrimCVMFSRepoPrefix(catalogPath), tmpFile.Name())
		if err != nil {
			return err
		}
		return err
	}
	return nil
}

//writes data to file and publish in cvmfs repo path
func WriteDataToCvmfs(CVMFSRepo, path string, data []byte) (err error) {
	tmpFile, err := temp.UserDefinedTempFile()
	if err != nil {
		l.LogE(err).Error("Error in creating temporary file for writing data")
		return err
	}
	defer os.RemoveAll(tmpFile.Name())
	err = ioutil.WriteFile(tmpFile.Name(), data, 0644)
	if err != nil {
		l.LogE(err).Error("Error in writing data to file")
		return err
	}
	err = tmpFile.Close()
	if err != nil {
		l.LogE(err).Error("Error in closing temporary file")
		return err
	}
	return PublishToCVMFS(CVMFSRepo, path, tmpFile.Name())
}

func removeHashMarkerIfPresent(digest string) string {
	t := strings.Split(digest, ":")
	if len(t) == 2 {
		return t[1]
	}
	return digest
}

func CreateChain(CVMFSRepo, chain, previous, layer string) error {
	newChainPath := ChainPath(CVMFSRepo, chain)
	layerPath := LayerRootfsPath(CVMFSRepo, removeHashMarkerIfPresent(layer))

	opt := TemplateTransaction{
		source:      TrimCVMFSRepoPrefix(layerPath),
		destination: TrimCVMFSRepoPrefix(newChainPath)}

	if previous != "" {
		baseChainPath := ChainPath(CVMFSRepo, previous)
		opt.source = TrimCVMFSRepoPrefix(baseChainPath)

		return WithinTransaction(CVMFSRepo, func() error {
			err := fs.ApplyDirectory(newChainPath, layerPath)
			if err != nil {
				l.LogE(err).Error("Error in Applying the layer on top of the chain")
			}
			return err
		}, opt)
	}

	return WithinTransaction(CVMFSRepo, func() error {
		if err := os.MkdirAll(newChainPath, constants.DirPermision); err != nil {
			l.LogE(err).Error("Error in creating the first directory of the chain")
			return err
		}
		err := fs.ApplyDirectory(newChainPath, layerPath)
		if err != nil {
			l.LogE(err).Error("Error in Applying the layer on top of the chain")
		}
		return err
	})
}

func CreateSneakyChain(CVMFSRepo, newChainId, previousChainId string, layer tar.Reader) error {
	sneakyPath := filepath.Join("/", "var", "spool", "cvmfs", CVMFSRepo, "scratch", "current")
	newChainPath := ChainPath(CVMFSRepo, newChainId)
	sneakyChainPath := filepath.Join(sneakyPath, TrimCVMFSRepoPrefix(newChainPath))
	// we need to create the directory were to do the template transaction
	dir := filepath.Dir(newChainPath)
	if _, err := os.Stat(dir); os.IsNotExist(err) {
		// if the directory does not exists, we create it

		if err := WithinTransaction(CVMFSRepo, func() error {
			return os.MkdirAll(dir, constants.DirPermision)
		}); err != nil {
			return nil
		}
	}
	// then we need the template transaction to populate it
	if previousChainId != "" {
		// if it is the very first chain, we don't need the template transaction
		opt := TemplateTransaction{
			source:      TrimCVMFSRepoPrefix(ChainPath(CVMFSRepo, previousChainId)),
			destination: TrimCVMFSRepoPrefix(newChainPath),
		}
		if err := WithinTransaction(CVMFSRepo, func() error { return nil }, opt); err != nil {
			return err
		}
	}
	// finally we need the sneaky transaction to create the chain
	if err := ExecuteAndOpenTransaction(CVMFSRepo, func() error {
		for {
			header, err := layer.Next()
			if err != nil {
				return err
			}
			if err == io.EOF {
				return nil
			}
			if header == nil {
				continue
			}

			path := filepath.Join(sneakyChainPath, header.Name)
			dir := filepath.Dir(path)

			os.MkdirAll(dir, constants.DirPermision)
			if isWhiteout(path) {
				// this will be an empty file
				// check if it is an opaque directory or a standard whiteout file
				base := filepath.Base(path)
				if base == ".wh..wh..opq" {
					// an opaque directory
					if err := makeOpaqueDir(dir); err != nil {
						return err
					}
				} else {
					// a whiteout file
					base = base[4:]
					path := filepath.Join(path, base)
					if err := makeWhiteoutFile(path); err != nil {
						return err
					}
				}
				// create the relative file or set the
				continue
			}

			permissionMask := int64(0)
			switch header.Typeflag {

			case tar.TypeDir:
				{
					err := os.MkdirAll(path, constants.DirPermision)
					if err != nil {
						return err
					}
					permissionMask |= 0700
				}
			case tar.TypeReg, tar.TypeRegA:
				{
					f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, constants.FilePermision)
					if err != nil {
						return err
					}
					defer f.Close()
					if _, err = io.Copy(f, &layer); err != nil {
						return err
					}
					permissionMask |= 0600
				}
			case tar.TypeLink:
				{
					// hardlink
					// maybe we should just copy the file
					if err := os.Link(header.Linkname, path); err != nil {
						return err
					}
				}
			case tar.TypeSymlink:
				{
					// symlink
					if err := os.Symlink(header.Linkname, path); err != nil {
						return err
					}

				}
			case tar.TypeChar, tar.TypeBlock, tar.TypeFifo:
				{
					// char device
					var mode uint32
					switch header.Typeflag {
					case tar.TypeChar:
						mode = unix.S_IFCHR
					case tar.TypeBlock:
						mode = unix.S_IFBLK
					case tar.TypeFifo:
						mode = unix.S_IFIFO
					}
					dev := unix.Mkdev(uint32(header.Devmajor), uint32(header.Devminor))
					if err := unix.Mknod(path, uint32(os.FileMode(int64(mode)|header.Mode)), int(dev)); err != nil {
						return err
					}
				}
			default:
				{
					// unclear what to do here, just skip it
					continue
				}
			}

			// these are common to everything
			if err := os.Chmod(path, os.FileMode(header.Mode|permissionMask)); err != nil {
				return err
			}
			if err := os.Chown(path, header.Uid, header.Gid); err != nil {
				return err
			}
			if err := os.Chtimes(path, header.AccessTime, header.ModTime); err != nil {
				return err
			}
		}
	}); err != nil {
		return err
	}
	// no the transaction is open and the sneaky overlay is populated
	// we don't need to do anything else at this point and we can close the transaction
	return Publish(CVMFSRepo)
}

func isWhiteout(path string) bool {
	base := filepath.Base(path)
	if len(base) <= 3 {
		return false
	}
	return base[0:4] == ".wh."
}

func makeWhiteoutFile(path string) error {
	dev := unix.Mkdev(0, 0)
	mode := os.FileMode(int64(unix.S_IFCHR) | 0000)
	return unix.Mknod(path, uint32(mode), int(dev))
}

func makeOpaqueDir(path string) error {
	if _, err := os.Stat(path); os.IsNotExist(err) {
		if err := os.MkdirAll(path, constants.DirPermision); err != nil {
			return err
		}
	}
	return xattr.Set(path, "trusted.overlay.opaque", []byte("y"))
}
