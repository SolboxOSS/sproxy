#!/bin/sh
LIBS="libnc.so.2.0.0 libwebdav_driver.so.1.0.0 libloopback_driver.so.1.0.0 libnc.a libwebdav_driver.a libloopback_driver.a"
HDR="ncapi.h netcache_types.h"
SDIR=/root/weon/netcache
TDIR=/root/libnetcache/1.5.0

for f in $LIBS
do
	cp $SDIR/lib/$f $TDIR/lib/.
done
for f in $HDR
do
	cp $SDIR/include/$f $TDIR/include/.
done
