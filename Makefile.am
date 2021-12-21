AUTOMAKE_OPTIONS = foreign nostdinc
BUILT_SOURCES = $(builddir)/siteconfig_grammar.h


bin_PROGRAMS = solproxy

INCLUDES = -I$(NC_INCLUDE) -I$(srcdir) -I$(top_builddir) -I/usr/include/libxml2  -I$(NC_LIBS)/../include

ZIPPER_INCLUDES = -I$(srcdir)/libzipper/include/zipper

SOLCMAF_INCLUDES = -I$(srcdir)/libzipper/include/solcmaf

RTMP_INCLUDES = -I$(srcdir)/libzipper/include/rtmp

#
# SO version
VERSIONINFO=-release 1:0:2

#if NO_UNDEFINED
#UNDEF = -no-undefined
#endif
#
DLL_CFLAGS= -DNOHM -DCLFU_AGE_SPIN -D_GNU_SOURCE -DMT -D_REENTRANT -pthread -D__USE_UNIX98 -DBUILD_DLL $(OPT) $(ARCH) $(MEM_LEAK_CHECK) $(INCLUDE) -DHAVE_CONFIG_H -DHYBRID_CACHE -D_GNU_SOURCE -DMT -D_REENTRANT -D__PROFILE -D__TRACE -D$(PLATFORM) -D__DEBUG -fPIC

if NC_LIBRT
NC_ADD_LIBS=-lrt
endif
#AM_YFLAGS = -t -d --defines=$(builddir)/siteconfig_grammar.h 
AM_YFLAGS = -t -d 
AM_LFLAGS = -d -i
ACLOCAL_AMFLAGS = -I config
#CFLAGS = -O0 -std=c99



#SVN_REVISION이 없는 경우는 0으로 셋팅한다.(일반 디버깅용)
#automake에서 ifndef를 사용할 경우 에러가 발생해서 아래와 같은 function 선언을 했다

@protect@ifndef SVN_REVISION
@protect@SVN_REVISION @equal@ 0
@protect@endif

@protect@ifndef CORE_SVN_REVISION
@protect@CORE_SVN_REVISION @equal@ 0
@protect@endif

@protect@ifndef BUILD_DATE
@protect@BUILD_DATE @equal@ 0
@protect@endif

MHD_VER=0.9.73
SUBDIRS=MHD/0.9.73/src/microhttpd
MHD_INCLUDE=-I$(srcdir)/MHD/$(MHD_VER)/src/include 
MHD_LIBS=-L$(builddir)/MHD/$(MHD_VER)/src/microhttpd/.libs -lmicrohttpd
LUA_ROOT=$(srcdir)/lua-5.1.5/src
DICT_ROOT=$(srcdir)/libdict
CCL_ROOT=$(srcdir)/ccl
JANSSON_ROOT=$(builddir)/jansson
JANSSON_INCLUDE=-I$(srcdir)/jansson/src -I$(JANSSON_ROOT)/src
#solproxy_CFLAGS = -g -D__TRACE   -I$(top_srcdir)/httpd -I$(srcdir)/lua-5.1.5/include -I$(srcdir)/geoip/libGeoIP -I$(srcdir)/MHD/$(MHD_VER)/src/include -D_GNU_SOURCE 
solproxy_CFLAGS = $(CFLAGS) -D__TRACE -I$(top_srcdir)/httpd -I$(srcdir)/lua-5.1.5/include\
					 $(MHD_INCLUDE) -DSUPPORT_NOT_INTERLEAVED\
					 -I$(DICT_ROOT)/include $(ZIPPER_INCLUDES) $(SOLCMAF_INCLUDES) $(RTMP_INCLUDES) -I$(CCL_ROOT)\
					 -D_GNU_SOURCE -DSVN_REVISION="$(SVN_REVISION)" -DCORE_SVN_REVISION="$(CORE_SVN_REVISION)" -DBUILD_DATE="$(BUILD_DATE)"\
					 $(JANSSON_INCLUDE)
solproxy_SOURCES = httpd.c mime_dict.c volumemgr.c logformat.c logger.c http_pages.c vstring.c reqpool.c symboltable.c luaembed.c scx_util.c\
					siteconfig_grammar.y siteconfig_token.l site.c lua_bit.c geoip_handler.c rest_api.c luapool.c streaming.c\
					sessionmgr.c smilparser.c status.c streaming_lru.c preload.c scx_timer.c limitrate.c scx_list.c\
					soluri2.c md5.c module.c test_module.c check_module.c thpool.c originstat.c lib_solbox_jwt.c soljwt.c\
					standalonestat.c scx_rtmp.c meta_disk_cache.c content_router.c shm_log.c
solproxy_LDADD = -L$(NC_LIBS) -lnc $(LIBS) $(NC_ADD_LIBS) $(MHD_LIBS) -lgcrypt ./liblua.a ./libdict/libdict.a -lz \
                 -lgnutls @LEXLIB@ -lGeoIP -lpthread -lxml2 $(srcdir)/libzipper/lib/zipper.a $(srcdir)/libzipper/lib/libcmaf.a \
				 $(srcdir)/libzipper/lib/librtmp.a -lcrypto ./libccl.a ./jansson/src/.libs/libjansson.a -lcurl

solproxy_LDFLAGS = -static

if ENABLE_AESNI
solproxy_CFLAGS += -DENABLE_AESNI
endif 

	
liblua.a:
	$(MAKE) -f $(LUA_ROOT)/Makefile $(AM_MAKEFLAGS) CC='$(CC)' RANLIB='$(RANLIB)' CFLAGS='$(AM_CFLAGS) $(CFLAGS)' VPATH=$(LUA_ROOT) PLAT='linux' $(builddir)/liblua.a
#	cp -p $(LUA_DIR)/liblua.a .
	$(RANLIB) $@
	
libdict/libdict.a: 
	$(MAKE) -f $(DICT_ROOT)/Makefile MYCFLAGS='$(AM_CFLAGS) $(CFLAGS)' VPATH=$(DICT_ROOT) static

libccl.a: 
	$(MAKE) -f $(CCL_ROOT)/Makefile MYCFLAGS='$(AM_CFLAGS) $(CFLAGS)' VPATH=$(CCL_ROOT) libccl.a 	

jansson/src/.libs/libjansson.a: 
	$(MAKE) -f Makefile -C $(JANSSON_ROOT)

clean-local:
	rm -f libcmaf.a zipper.a liblua.a zipper clicmaf *.so librtmp.a libccl.a
	$(MAKE) -f Makefile -C $(JANSSON_ROOT) clean


