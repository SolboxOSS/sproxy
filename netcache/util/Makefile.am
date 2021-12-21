AUTOMAKE_OPTIONS = foreign nostdinc

bin_PROGRAMS = nc_checkfs

INCLUDES = -I$(top_srcdir)/include -I$(top_builddir)


#if NO_UNDEFINED
UNDEF = -no-undefined
#endif

if NC_LIBRT
NC_ADD_LIBS=-lrt
endif

#DLL_CFLAGS= -DNOHM -DCLFU_AGE_SPIN -D_GNU_SOURCE -DMT -D_REENTRANT -pthread -D__USE_UNIX98 -DBUILD_DLL $(OPT) $(ARCH) $(MEM_LEAK_CHECK) $(INCLUDE) -DHAVE_CONFIG_H -DHYBRID_CACHE -D_GNU_SOURCE -DMT -D_REENTRANT -D__PROFILE -D__TRACE -D$(PLATFORM) -D__DEBUG -fPIC

nc_checkfs_CFLAGS = -g -D__DEBUG -DNOHM -DCLFU_AGE_SPIN -DMT -D_REENTRANT -DHYBRID_CACHE 

nc_checkfs_SOURCES = nc_checkfs.c
nc_checkfs_LDADD = -L$(top_builddir)/netcache -lnc $(LIBS) $(NC_ADD_LIBS)
nc_checkfs_LDFLAGS = -g
LIB_ADDS=


