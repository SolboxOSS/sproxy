AUTOMAKE_OPTIONS = foreign nostdinc

SVN_VERSION=$(shell svnversion -n)

lib_LTLIBRARIES = libnc.la

INCLUDES = -I$(top_srcdir)/include -I$(top_srcdir) -I$(top_builddir) -I$(top_builddir)/sqlite.current 

#
# SO version
VERSIONINFO=-version-info 3:0:0

#versioning rul
#version fields current[:revision[:age]]. So passing -version-info 3:12:1 sets current to 3, revision to 12, and 
# age to 1.
#
# simplifed rul guide on how to change -version-info:
# (current version is C:R:A)
# 1. if there are only source changes, use C:R+1:A
# 2. if interface were added, use C+1:0:A+1
# 3. if interfaces were removed, use C+1:0:0

# #if NO_UNDEFINED
UNDEF = -no-undefined
# #endif


NC_OS_LIBS = 
if NCM_ENABLE_GPERFPROF
	NC_OS_LIBS += -L/usr/local/lib -lprofiler 
endif

NC_OS_LIBS += -luuid

SQLITE3_CFLAGS=-I$(top_srcdir)/sqlite.current
SQLITE3_LIBS=-L$(top_builddir)/sqlite.current -lsqlite3 -lm

libnc_la_CFLAGS = -DREVISION="$(SVN_VERSION)" -D__DEBUG -DMT -D_REENTRANT -DBUILD_DLL -D__TRACE  $(SQLITE3_CFLAGS)
libnc_la_LDFLAGS = $(VERSIONINFO) -no-undefined -build-id $(NC_OS_LIBS) $(SQLITE3_LIBS) -lz
libnc_la_SOURCES = $(top_srcdir)/common/asio_mgr.c  bc_mgr.c  $(top_srcdir)/common/blk_io.c  cfs_apix.c  cfs_driver.c   \
					clfu.c lru.c log.c  threadpool.c  trace.c hash.c util.c tlc_queue.c \
					diskcache.c bt_timer.c disk_io_ops.c \
					blkio_lpmap.c volumeops.c mdb.c nc_rwlock.c\
					part_mgr.c fhops.c kv.c ringbuffer.c crc32.c cacheops.c \
					md5.c glru.c memmgr.c heaptrack.c mavg.c $(LIBNC_SOURCES_ADD)
release:
	touch cfs_apix.c
	make
