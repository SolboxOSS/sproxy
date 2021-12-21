#!/bin/bash
export PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/share/pkgconfig
export CFLAGS='-g -O2 -pthread -std=gnu99 -D_GNU_SOURCE -D_REENTRANT'

C_DIR=`pwd`

mkdir -p libnetcache/lib/
rm -rf libnetcache/lib/*
rm -f /usr/lib64/libhttpn_driver.so*
rm -f /usr/lib64/libnc.so*



#bootstrap

rm -rf netcache_build/*


touch netcache/sqlite.current/*
mkdir -p netcache_build
cd netcache_build
$C_DIR/netcache/configure --enable-static=libconv --enable-shared --prefix=$C_DIR/libnetcache

make
make install
cd $C_DIR

cp -a libnetcache/lib/libhttpn_driver.so* /usr/lib64
cp -a libnetcache/lib/libnc.so* /usr/lib64


