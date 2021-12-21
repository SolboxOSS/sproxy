AUTOMAKE_OPTIONS = foreign nostdinc

#lib_LTLIBRARIES = libnc_common.la
lib_LIBRARIES = libnc_common.a

INCLUDES = -I$(top_builddir)/include

#
# SO version
VERSIONINFO=-version-info 1:0:2

#versioning rul
#version fields current[:revision[:age]]. So passing -version-info 3:12:1 sets current to 3, revision to 12, and 
# age to 1.
#
# simplifed rul guide on how to change -version-info:
# (current version is C:R:A)
# 1. if there are only source changes, use C:R+1:A
# 2. if interface were added, use C+1:0:A+1
# 3. if interfaces were removed, use C+1:0:0

#if NO_UNDEFINED
UNDEF = -no-undefined
#endif


libnc_common_a_CFLAGS = -DNOHM -DMT -D_REENTRANT -pthread -DBUILD_DLL -D__TRACE -I../include `$(NEON_CONFIG) --cflags` 
#libnc_common_a_LDFLAGS = $(UNDEF) 
libnc_common_a_SOURCES = asio_mgr.c  blk_io.c  cache_stat.c  lock.c log.c  threadpool.c 
