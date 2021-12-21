AUTOMAKE_OPTIONS = foreign nostdinc

SVN_VERSION=$(shell svnversion -n)

noinst_LTLIBRARIES = libdriver_common.la

#INCLUDES = -I$(top_srcdir)/include -I$(top_srcdir) -I$(top_builddir) -I$(srcdir)
INCLUDES = -I$(top_srcdir)/include -I$(top_srcdir) -I$(top_srcdir)/plugins/common -I$(top_builddir)

#
# SO version
#VERSIONINFO=-release 1.5.0
VERSIONINFO=-version-info 1:0:0

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

libdriver_common_la_CFLAGS = $(CFLAGS_ADDS) -DNOHM -DMT -D_REENTRANT -DBUILD_DLL -D__TRACE 
libdriver_common_la_LDFLAGS = $(VERSIONINFO) -L/lib -L$(top_builddir)/netcache -lnc -no-undefined $(LIB_ADDS)
libdriver_common_la_LIBS = $(NC_LIB_ADDS) 
libdriver_common_la_SOURCES = lb.c
