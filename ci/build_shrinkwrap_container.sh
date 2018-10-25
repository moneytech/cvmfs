#!/bin/bash

set -e

die() {
  echo "$1"
  exit 1
}

[ "x$CVMFS_SOURCE_LOCATION" != x ] || die "CVMFS_SOURCE_LOCATION missing"
[ "x$CVMFS_BUILD_LOCATION" != x ] || die "CVMFS_BUILD_LOCATION missing"
[ "x$SHRINKWRAP_PACKAGE" != x ] || die "SHRINKWRAP_PACKAGE missing"
[ "x$SHRINKWRAP_CONTAINER_RELEASE" != x ] || die "SHRINKWRAP_CONTAINER_RELEASE missing"

cd $CVMFS_BUILD_LOCATION
make -f $CVMFS_SOURCE_LOCATION/ci/shrinkwrap/Makefile \
  CVMFS_SRC=$CVMFS_SOURCE_LOCATION \
  SHRINKWRAP_PACKAGE=$SHRINKWRAP_PACKAGE \
  IMAGE_RELEASE=$SHRINKWRAP_CONTAINER_RELEASE
