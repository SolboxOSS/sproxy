#!/bin/bash
#export PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/share/pkgconfig
#export CFLAGS='-g -O2 -pthread -std=gnu99 -D_GNU_SOURCE -D_REENTRANT'

C_DIR=`pwd`

rm -rf solproxy_build


touch configure.ac aclocal.m4 configure Makefile.am Makefile.in
cd $C_DIR/MHD/0.9.73/
touch configure.ac aclocal.m4 configure Makefile.am Makefile.in
find  $C_DIR/jansson/  -type f -print0 | xargs -0 touch


#touch  $C_DIR/aclocal.m4
#time issue로 빌드가 안되는 문제때문에 아래의 touch부분 필요(http://192.168.0.247/redmine/issues/21183)
#touch  $C_DIR/MHD/0.9.73/*

#touch  $C_DIR/MHD/0.9.73/doc/*

#touch  $C_DIR/jansson/*

mkdir -p $C_DIR/solproxy_build
cd $C_DIR/solproxy_build
$C_DIR/configure --with-mdh-pkgconf=/usr/lib64/gnutls30/pkgconfig --enable-aesni --with-nc-include=$C_DIR/netcache/include --with-nc-libs=$C_DIR/libnetcache/lib

make -d

cd $C_DIR


