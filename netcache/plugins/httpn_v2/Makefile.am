AUTOMAKE_OPTIONS = foreign nostdinc

lib_LTLIBRARIES = libhttpn_driver.la

INCLUDES = -I$(top_srcdir)/include -I$(top_srcdir) -I$(top_srcdir)/plugins/common -I$(top_builddir)

#
#REMARK!!!
#SO version
#version fields current[:revision[:age]]. So passing -version-info 3:12:1 sets current to 3, revision to 12, and # age to 1.
#
# simplifed rul guide on how to change -version-info:
# (current version is C:R:A)
# 1. if there are only source changes, use C:R+1:A
# 2. if interface were added, use C+1:0:A+1
# 3. if interfaces were removed, use C+1:0:0
VERSIONINFO=-version-info 2:9:0


#if NO_UNDEFINED
#UNDEF = -no-undefined
#endif



CURL_CFLAGS=-I$(top_srcdir)/curl.current/include -I$(top_builddir)/curl.current/include/curl
CURL_LIBS=-L$(top_builddir)/curl.current/lib -lcurl

libhttpn_driver_la_CPPFLAGS = -DMPX -DLIBICONV_PLUG $(OPENSSL_CFLAGS) $(XML_CFLAGS) $(CURL_CFLAGS) -D_MEMORY_DEBUG -DMT -DPHASE_HANDLER_DEFINED -D_REENTRANT -D__TRACE 
libhttpn_driver_la_LDFLAGS = $(VERSIONINFO) -L$(top_builddir)/netcache -lnc -no-undefined  -Wl,--export-dynamic -L$(top_builddir)/plugins/common -ldriver_common
libhttpn_driver_la_SOURCES = http_driver.c session_pool.c http_request.c base64.c http_codes.c iocb.c iocb_stm.c error.c $(SOURCES_ADDS)
libhttpn_driver_la_LIBADD = $(XML_LIBS) $(CURL_LIBS) $(OPENSSL_LIBS)
