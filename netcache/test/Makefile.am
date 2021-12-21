AUTOMAKE_OPTIONS = foreign nostdinc

bin_PROGRAMS = test_kv volume_test  vstr_test rb_test mem_test uuid_test list_test integrity_test
#sort_test

INCLUDES = -I$(top_srcdir)/include -I$(top_builddir)

#
# SO version
VERSIONINFO=-release 1:0:2

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
#UNDEF = -no-undefined
#endif
#
DLL_CFLAGS= -DNOHM -DCLFU_AGE_SPIN -D_GNU_SOURCE -DMT -D_REENTRANT -pthread -D__USE_UNIX98 -DBUILD_DLL $(OPT) $(ARCH) $(MEM_LEAK_CHECK) $(INCLUDE) -DHAVE_CONFIG_H -DHYBRID_CACHE -D_GNU_SOURCE -DMT -D_REENTRANT -D__PROFILE -D__TRACE -D$(PLATFORM) -D__DEBUG -fPIC

if NC_LIBRT
NC_ADD_LIBS=-lrt
endif



#test_path_CFLAGS = -g -D__TRACE  
#test_path_SOURCES = test_path.c
#test_path_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)

test_kv_CFLAGS = -g -D__TRACE  
test_kv_SOURCES = test_kv.c
test_kv_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
test_kv_LDFLAGS = -g

volume_test_CFLAGS = -g -D__TRACE  
volume_test_SOURCES = volume_test.c
volume_test_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
volume_test_LDFLAGS = -g

vstr_test_CFLAGS = -g -D__TRACE  
vstr_test_SOURCES = vstr_test.c
vstr_test_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
vstr_test_LDFLAGS = -g

rb_test_CFLAGS = -g -D__TRACE  
rb_test_SOURCES = rb_test.c
rb_test_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
rb_test_LDFLAGS = -g

mem_test_CFLAGS = -g -D__TRACE  
mem_test_SOURCES = mem_test.c
mem_test_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
mem_test_LDFLAGS = -g

uuid_test_CFLAGS = -g -D__TRACE  
uuid_test_SOURCES = uuid_test.c
uuid_test_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
uuid_test_LDFLAGS = -g



list_test_CFLAGS = -g -D__TRACE  
list_test_SOURCES = list_test.c
list_test_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
list_test_LDFLAGS = -g

integrity_test_CFLAGS = -g -D__TRACE  -I$(top_builddir)/sqlite.current
integrity_test_SOURCES = integrity_test.c
integrity_test_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
integrity_test_LDFLAGS = -g
