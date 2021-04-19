#!/bin/sh

#
# This script builds the CernVM-FS service container.
#

set -e

SCRIPT_LOCATION=$(cd "$(dirname "$0")"; pwd)
. ${SCRIPT_LOCATION}/../common.sh

if [ $# -lt 2 ]; then
  echo "Usage: $0 <CernVM-FS source directory> <build result location> [<nightly build number>]"
  echo "This script builds CernVM-FS service container"
  exit 1
fi

CVMFS_SOURCE_LOCATION="$1"
CVMFS_RESULT_LOCATION="$2"
CVMFS_BUSYBOX=/usr/bin/busybox
CVMFS_NIGHTLY_BUILD_NUMBER="${3-0}"

# For the time being, build with the host's docker until the builder nodes are
# new enough to support fuse in user namespaces.  That's a precondition to
# using buildah

if ! docker version; then
  echo "docker required to build container image"
  exit 1
fi

# if ! buildah version; then
#   echo "buildah required to build container image"
#   exit 1
# fi

if ! $CVMFS_BUSYBOX --help | head -5; then
  echo "functional busybox is required"
  exit 1
fi

if ! lsb_release -sicr; then
  echo "lsb_release required to build container image"
  exit 1
fi

# retrieve the upstream version string from CVMFS
cvmfs_version="$(get_cvmfs_version_from_cmake $CVMFS_SOURCE_LOCATION)"
echo "detected upstream version: $cvmfs_version"

git_hash="$(get_cvmfs_git_revision $CVMFS_SOURCE_LOCATION)"

# generate the release tag for either a nightly build or a release
CVMFS_TAG=
if [ $CVMFS_NIGHTLY_BUILD_NUMBER -gt 0 ]; then
  build_tag="git-${git_hash}"
  nightly_tag="0.${CVMFS_NIGHTLY_BUILD_NUMBER}.${git_hash}git"

  echo "creating nightly build '$nightly_tag'"
  CVMFS_TAG="${cvmfs_version}-$nightly_tag"
else
  echo "creating release: $cvmfs_version"
  CVMFS_TAG="${cvmfs_version}-1"
fi

# sanity checks
# TODO: add externals?
for d in build rootfs; do
  if [ -d ${CVMFS_RESULT_LOCATION}/${d} ]; then
    echo "build directory seems to be used before (${CVMFS_RESULT_LOCATION}/${d} exists)"
    exit 1
  fi
done
for d in build rootfs; do
  mkdir -p ${CVMFS_RESULT_LOCATION}/${d}
done

# build commands
cd ${CVMFS_RESULT_LOCATION}/build
cmake -DBUILD_SNAPSHOTTER=yes -DBUILD_CVMFS=no \
  -DBUILD_SERVER=no -DBUILD_RECEIVER=no -DBUILD_GEOAPI=no \
  -DBUILD_LIBCVMFS=no -DBUILD_LIBCVMFS_CACHE=no \
  -DINSTALL_BASH_COMPLETION=no \
  -DEXTERNALS_PREFIX=${CVMFS_RESULT_LOCATION}/externals \
  ${CVMFS_SOURCE_LOCATION}
make -j4

# generating package map section for specific platform
if [ ! -z $CVMFS_CI_PLATFORM_LABEL ]; then
  echo "generating package map section for ${CVMFS_CI_PLATFORM_LABEL}..."
  generate_package_map                                      \
    "$CVMFS_CI_PLATFORM_LABEL"                              \
    "cvmfs-service-${CVMFS_TAG}.$(uname -m).docker.tar.gz"  \
    ""  \
    ""  \
    ""  \
    ""  \
    ""
fi