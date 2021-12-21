service soldrive stop
ROOT=/stg/work/weon/netcache-2.2.work/build.linux.x86_64
cp  $ROOT/netcache/.libs/libnc.so.2.0.3 /usr/lib64
cp  $ROOT/plugins/webdav/.libs/libwebdav_driver.so.2.0.2 /usr/service/lib
cp  $ROOT/plugins/http/.libs/libhttp_driver.so.1.0.1 /usr/service/lib

