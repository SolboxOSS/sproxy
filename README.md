# solproxy

## package required for build
Only CentOS 6 is supported
install epel repo
db4 db4-utils db4-devel db4-cxx
libaio libaio-devel
curl libcurl libcurl-devel
gdb bison flex  zlib-devel
openssl-devel libxml2-devel libuuid-devel.x86_64 sqlite-devel.x86_64 sqlite.x86_64 
libgcrypt-devel GeoIP-devel gnutls-devel.x86_64
ffmpeg-devel.x86_64 libjpeg-turbo-devel.x86_64 fdk-aac-devel
tcl.x86_64 tcl-devel.x86_64 rpm-build.x86_64 gcc-c++
gnutls30.x86_64  gnutls30-devel GeoIP 

## Building solproxy
Move to source directory
- Build netcache
./netcache_build.sh
- Build solproxy
./solproxy_build.sh

## Environment
- make directory
-- mkdir -p /usr/service/etc/solproxy
-- mkdir -p /usr/service/sbin
- copy binary 
-- cp solproxy_build/solproxy /usr/service/sbin/
-- cp solproxy_build/MHD/0.9.73/src/microhttpd/.libs/libmicrohttpd.so* /usr/lib64/ 
-- cp libnetcache/lib/libnc.so* /usr/lib64/
-- cp libnetcache/lib/libhttpn_driver.so* /usr/lib64/
- set env


## Example configuration
- /usr/service/etc/solproxy/default.conf
```
server {
        http_port = 80
        negative_ttl = 10
        positive_ttl = 3600
        chunk_size = 16
        cache_size = 1024
        nwra = 128
        dra = 16
        cache_dir = /var/cache/solproxy
        log_directory = /var/log/solproxy
        pool_size = 8192
        workers = 8
        logrotate_signal_enable = 1
}
```
- /usr/service/etc/solproxy/service.conf
```
server {
        domain = vod.origin.com
        origin = 13.188.35.147
        streaming_enable = 1
}
```
## How to run
- export CSA_PIDFILE=/var/run/solproxy.pid
- export CSA_CONFPATH=/usr/service/etc/solproxy
- solproxy_build/solproxy

## License
solproxy is is dual-licensed under the GNU Lesser General PublicLicense (LGPLv2.1+) 
