# Introduction

![GitHub Repo stars](https://img.shields.io/github/stars/SolboxOSS/sproxy?style=social) ![GitHub commit activity](https://img.shields.io/github/commit-activity/y/SolboxOSS/sproxy) ![GitHub forks](https://img.shields.io/github/forks/SolboxOSS/sproxy?style=social) ![GitHub contributors](https://img.shields.io/github/contributors-anon/SolboxOSS/sproxy)

## Introduction

Sproxy is a reverse proxy that acts as a proxy server in the edge section close to the user on behalf of various web servers.

* Github: [https://github.com/SolboxOSS/sproxy](https://github.com/SolboxOSS/sproxy)
* Project page: [https://sproxy.solbox.com/](https://sproxy.solbox.com)
*

### How to RUN

#### package required for build

Only CentOS 6 is supported

* install epel repo
* install rpm
  * db4 db4-utils db4-devel db4-cxx
  * libaio libaio-devel
  * curl libcurl libcurl-devel
  * gdb bison flex zlib-devel
  * openssl-devel libxml2-devel libuuid-devel.x86\_64 sqlite-devel.x86\_64 sqlite.x86\_64
  * libgcrypt-devel GeoIP-devel gnutls-devel.x86\_64
  * ffmpeg-devel.x86\_64 libjpeg-turbo-devel.x86\_64 fdk-aac-devel
  * tcl.x86\_64 tcl-devel.x86\_64 rpm-build.x86\_64 gcc-c++
  * gnutls30.x86\_64 gnutls30-devel GeoIP

#### Building solproxy

Move to source directory

* Build netcache
  * ./netcache\_build.sh
* Build solproxy
  * ./solproxy\_build.sh

#### Environment

* make directory
  * mkdir -p /usr/service/etc/solproxy
  * mkdir -p /usr/service/sbin
* copy binary
  * cp solproxy\_build/solproxy /usr/service/sbin/
  * cp solproxy\_build/MHD/0.9.73/src/microhttpd/.libs/libmicrohttpd.so\* /usr/lib64/
  * cp libnetcache/lib/libnc.so\* /usr/lib64/
  * cp libnetcache/lib/libhttpn\_driver.so\* /usr/lib64/
* set env

#### Example configuration

* /usr/service/etc/solproxy/default.conf

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

* /usr/service/etc/solproxy/service.conf

```
server {
        domain = vod.origin.com
        origin = 13.188.35.147
        streaming_enable = 1
}
```

#### How to run

* export CSA\_PIDFILE=/var/run/solproxy.pid
* export CSA\_CONFPATH=/usr/service/etc/solproxy
* solproxy\_build/solproxy

### License

solproxy is is dual-licensed under the GNU General PublicLicense (GPLv2.0) or commercial license
