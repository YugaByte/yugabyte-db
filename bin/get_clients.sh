#!/bin/sh
# Download yugabyte client binaries locally

set -e

VERSION=2.2
PKG_PREFIX=yugabyte-client

uname=$(uname | tr '[:upper:]' '[:lower:]')
pkg="${PKG_PREFIX}-${VERSION}-$uname.tar.gz"

printf "Downloading %s ... \r" "$pkg"
wget -q "https://downloads.yugabyte.com/${pkg}" -O "$pkg"

printf "Extracting %s ... \r" $pkg
tar -zxf "$pkg"

if test "$uname" = "linux"; then
   printf "Setting up %s ... \r" $pkg
  "${PKG_PREFIX}-${VERSION}/bin/post_install.sh" > /dev/null 2>&1
fi

echo "ysqlsh is at ${PKG_PREFIX}-${VERSION}/bin/ysqlsh                                      "
echo "ycqlsh is at ${PKG_PREFIX}-${VERSION}/bin/ycqlsh"
echo You may want to run
echo " sudo mv ${PKG_PREFIX}-${VERSION}/bin/ysqlsh /usr/local/bin/"
echo " sudo mv ${PKG_PREFIX}-${VERSION}/bin/ycqlsh /usr/local/bin/"
