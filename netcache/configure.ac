# Process this file with autoconf to produce a configure script.

dnl AC_PREREQ([2.69])
dnl AC_PREREQ([2.69])
dnl
dnl we extract version from config
dnl
define([svnversion], esyscmd([sh -c "svnversion -n"]))
AC_INIT([NetCache],[3.1],[weon@solbox.com])
AC_CONFIG_SRCDIR([netcache/cfs_driver.c])

AC_DEFINE(SVN_REVISION, "svnversion", [SVN Revision])

dnl LM_INIT

AC_PATH_PROG([SED],[sed],[not_found], [$PATH:/usr/bin:/usr/sbin])
if test -z "$SED" || test "$SED" = "not_found"; then
	AC_MSG_ERROR([sed not found in PATH, could not configue without sed.])
fi
AC_SUBST([SED])


AC_CONFIG_AUX_DIR([config])
dnl figure out the libcurl version
VERSION=`$SED -ne 's/^#define LIBNC_VERSION "\(.*\)"/\1/p' ${srcdir}/include/ncconfig.h`

AM_INIT_AUTOMAKE(NetCache, $VERSION)
AM_SILENT_RULES([yes])
dnl AM_SILENT_RULES([yes])
AC_MSG_CHECKING([NetCache Version])
AC_MSG_RESULT($VERSION)
AC_CONFIG_HEADER([config.h])

AC_CANONICAL_HOST
dnl get system canonical name
AC_DEFINE_UNQUOTED(OS, "${host}", [cpu-machine-OS])

dnl extend
AC_USE_SYSTEM_EXTENSIONS

dnl support building of Windows DLLs
dnl AC_REQUIRE(AC_CANONICAL_HOST)_LT_SET_OPTION([LT_INIT],[win32-dll])
dnl AC_DIAGNOSE([obsolete],
dnl [AC_REQUIRE(AC_CANONICAL_HOST)_LT_SET_OPTION([LT_INIT],[win32-dll])

AC_CANONICAL_HOST


dnl determine target system
case "$host_os" in
	*mingw32*|*cygwin*|msys) os_type=win32
				echo "###############################################" ;;
	darwin*) os_type=mac ;;
	*) os_type=unix ;;
esac

echo "HOST OS=" $host_os "OS_TYPE=" $os_type
AM_CONDITIONAL([OS_WIN32], [test "$os_type" = "win32"])
AM_CONDITIONAL([OS_UNIX], [test "$os_type" != "win32"])
AM_CONDITIONAL([OS_LINUX], [test "$os_type" = "linux"])


#
#
DEVELOPER_CFLAGS="-Wall -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wreturn-type -Wcast-qual -Wwrite-strings -Wswitch -Wshadow -Wcast-align -Wunused-parameter -Wchar-subscripts -Winline -Wnested-externs -Wredundant-decls -Wformat" 
EXTRA_DEVELOPER_CFLAGS="-Wextra -Wno-missing-field-initializers -Wno-sign-compare"

# Checks for programs.
dnl We need to check for the compiler up here to avoid anything else
dnl starting with a different one.
AC_PROG_CC(clang gcc)
AC_PROG_CXX(clang++ g++)
AC_PROG_CPP
LT_INIT
AC_PROG_LEX
AC_PROG_YACC
AC_PROG_INSTALL

LT_INIT
AC_PROG_LN_S
#AC_PROG_CC_C99
#AX_GCC_ARCHFLAG([no], [CFLAGS="$CFLAGS $ax_cv_gcc_archflag"], AC_MSG_ERROR([can not detect architecture of CPU]));
#AX_GCC_ARCHFLAG(no)

VERSIONNUM=`$SED -ne 's/^#define LIBNC_VERSION "\(.*\)"/\1/p' ${srcdir}/include/ncconfig.h`
AC_MSG_RESULT($VERSIONNUM)
AC_SUBST(VERSIONNUM)


dnl
dnl fossil for updating sqlite
dnl
dnl echo "updating sqlite3 via fossil..."
dnl
dnl $srcdir/bin/update.sqlite3 $srcdir
dnl rm -f $srcdir/sqlite.current
dnl ln -s $srcdir/sqlite3.fossil $srcdir/sqlite.current


# large file support


#GCC_STACK_PROTECT_CC
#GCC_STACK_PROTECT_LIB

AC_SYS_LARGEFILE

# Checks for libraries.
AM_GNU_GETTEXT_VERSION(0.14.4)
#AM_GNU_GETTEXT([external])
#NE_REQUIRE_VERSIONS([0], [25 26 27 28 29])
#NC_CHECK_NEON

#PKG_CHECK_MODULES(XML, libxml-2.0, , AC_MSG_ERROR('Unable to find libxml2. Please make sure library and header files are installed.'))
#PKG_CHECK_MODULES(CURL, libcurl, , AC_MSG_ERROR('Unable to find libcurl.  Please make sure library and header files are installed.'))
#PKG_CHECK_MODULES(OPENSSL, openssl, [HAVE_OPENSSL=1], [HAVE_OPENSSL=0]])
AC_SUBST(HAVE_OPENSSL)


case $host in
	*-*-mingw32*)
		TARGET_PSPACE="-DPLATFORM_32BIT"
		NC_LIB_ADDS="-lwinsock"
		;;
	*-*-mingw64*)
		TARGET_PSPACE="-DPLATFORM_64BIT"
		NC_LIB_ADDS="-lwinsock32"
		;;
	x86_64*|amd64*)
		TARGET_PSPACE="-DPLATFORM_64BIT"
		NC_LIB_ADDS="-lnsl"
		;;
	*)
		TARGET_PSPACE="-DPLATFORM_32BIT"
		NC_LIB_ADDS="-lnsl"
		;;
esac
AC_SUBST(TARGET_PSPACE, $TARGET_PSPACE)
AC_SUBST(NC_LIB_ADDS, $NC_LIB_ADDS)

dnl GCC_STACK_PROTECTOR



# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
#AC_CHECK_HEADERS([argz.h fcntl.h libintl.h langinfo.h limits.h locale.h mntent.h stddef.h stdint.h stdlib.h string.h sys/mount.h sys/time.h syslog.h termios.h unistd.h utime.h])
AC_CHECK_HEADERS([fcntl.h libintl.h limits.h mntent.h stddef.h stdint.h stdlib.h signal.h string.h sys/time.h syslog.h unistd.h utime.h execinfo.h dlfcn.h sys/statfs.h sys/resource.h])
# We have to do this here so that the test below can compile
AC_CHECK_HEADERS([sys/uuid.h uuid/uuid.h uuid.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_UID_T
AC_C_INLINE
#AC_TYPE_INT16_T
#AC_TYPE_INT32_T
#AC_TYPE_INT8_T
#AC_TYPE_MODE_T
#AC_TYPE_OFF_T
#AC_TYPE_PID_T
#AC_TYPE_SIZE_T
#AC_TYPE_SSIZE_T
AC_CHECK_MEMBERS([struct stat.st_blksize])
AC_HEADER_TIME
AC_STRUCT_TM
#AC_TYPE_UINT32_T
#AC_TYPE_UINT64_T
AC_C_VOLATILE
AC_CHECK_SIZEOF([void *])

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
#AC_PROG_GCC_TRADITIONAL
AC_HEADER_MAJOR
AC_FUNC_MALLOC
AC_FUNC_SELECT_ARGTYPES
AC_FUNC_STRFTIME
AC_FUNC_STAT
AC_FUNC_MKTIME
AC_FUNC_MKTIME
AC_FUNC_STRERROR_R
AC_FUNC_STRFTIME
AC_FUNC_UTIME_NULL
AC_FUNC_UTIME_NULL
AC_CHECK_FUNCS([gettid endpwent ftruncate getmntent memset mkdir nl_langinfo rpmatch select setlocale strcasestr strcasecmp strchr strdup strerror strpbrk strrchr strstr strtol utime])
AC_CHECK_LIB(pthreads, pthread_create, [], [AC_CHECK_LIB(pthread, pthread_create)])
AC_CHECK_LIB(rt, pthread_create, [], [AC_CHECK_LIB(pthread, pthread_create)])
AC_SEARCH_LIBS([shm_open], [rt], [AM_CONDITIONAL([NC_LIBRT], [true])],[AM_CONDITIONAL([NC_LIBRT], [false])]
)
dnl
dnl check snprintf replacement
dnl
HW_FUNC_VSNPRINTF
HW_FUNC_SNPRINTF
HW_FUNC_VASPRINTF
HW_FUNC_ASPRINTF

dnl
dnl check strptime
dnl
AC_CHECK_FUNC(strptime, 
				[AM_CONDITIONAL([NC_ADD_STRPTIME], [false])],
				[AM_CONDITIONAL([NC_ADD_STRPTIME], [true])]

)

AC_CHECK_LIB(dl, dlopen, , missing_functions="yes")
if test  "$missing_functions" = "yes"; then
		AC_MSG_ERROR([Required libdl library is not available])
fi 



dnl AC_CHECK_LIB([c], [iconv], [NEED_LIBICONV=],
dnl 				AC_CHECK_LIB([iconv], [iconv], [NEED_LIBICONV=Y], [NO_ICONV=Y]))

dnl AC_CHECK_LIB(iconv, libiconv_open, , missing_functions="yes")
dnl if test  "$missing_functions" = "yes"; then
dnl 	missing_functions="no"
dnl 	AC_CHECK_LIB(iconv, libiconv_open, LDFLAGS="$LDFLAGS --static -liconv", missing_functions="yes")
dnl 	if test "$missing_functions" = yes; then
dnl 		AC_MSG_ERROR([Required libiconv library is not available])
dnl 	fi
dnl 
dnl fi 

old_CFLAGS="$CLFAGS"
old_LIBS="$LIBS"
AC_CHECK_LIB([c], [iconv_open], , missing_function="yes")
if test  "$missing_functions" = "yes"; then
	AC_CHECK_LIB([iconv], [libiconv_open], , missing_function="yes")
fi



AC_CHECK_LIB(rt, clock_gettime, missing_functions="no", missing_functions="yes")
if test  "$missing_functions" = yes; then
	echo "babooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
	missing_functions="no"
	missing_functions_clock_get_time="no"
	AC_CHECK_LIB(rt, clock_gettime, LDFLAGS="$LDFLAGS -lrt", missing_functions="yes")
	if test "$missing_functions" = yes; then
		AC_DEFINE(HAVE_CLOCK_GETTIME, 0, [Define if you have clock_gettime()])
		missing_functions_clock_get_time="yes"
	else
		AC_DEFINE(USE_MONOTONIC_CONDWAIT, 1, [Define if you have clock_gettime()])
		AC_DEFINE(HAVE_CLOCK_GETTIME, 1, [Define if you have clock_gettime()])
		missing_functions_clock_get_time="no"
	fi
else
	AC_DEFINE(USE_MONOTONIC_CONDWAIT, 1, [Define if you have clock_gettime()])
	AC_DEFINE(HAVE_CLOCK_GETTIME, 1, [Define if you have clock_gettime()])
	missing_functions_clock_get_time="no"
fi 
#AC_CHECK_LIB(rt, sys_clock_gettime, , missing_functions="yes")
#if test  "$missing_functions_clock_get_time" = yes; then
#	missing_functions="no"
#	AC_CHECK_LIB(rt, sys_clock_gettime, LDFLAGS="$LDFLAGS -lrt", missing_functions="yes")
#	if test "$missing_functions" = yes; then
#		AC_DEFINE(HAVE_SYS_CLOCK_GETTIME, 0, [Define if sys_you have clock_gettime()])
#	else
#		AC_DEFINE(HAVE_SYS_CLOCK_GETTIME, 1, [Define if you have sys_clock_gettime()])
#		AC_DEFINE(USE_MONOTONIC_CONDWAIT, 1, [Define if you have clock_gettime()])
#	fi
#
#fi 

AC_MSG_CHECKING(if pthread_rwlock_t has '__writers')
AC_COMPILE_IFELSE( [
		#include <pthread.h>
		pthread_rwlock_t __T;
		int main() {
		        __T.__data.__writers = 0;
				return 0;
		}
	], [
		AC_DEFINE(NC_RWLOCK_HAVE_WRITERS, 1, [.__writers exists])
		AC_MSG_RESULT(Yes)
	], AC_MSG_RESULT(No))

AC_MSG_CHECKING(if pthread_rwlock_t has '__writer')
AC_COMPILE_IFELSE( [
		#include <pthread.h>
		pthread_rwlock_t __T;
		int main() {
		        __T.__data.__writer = 0;
				return 0;
		}
	], [
		AC_DEFINE(NC_RWLOCK_HAVE_WRITER,1, [.__writer exists])
		AC_MSG_RESULT(Yes)
	], AC_MSG_RESULT(No))

AC_CHECK_LIB(aio, io_submit, , missing_functions="yes")
if test  "$missing_functions" = yes; then
	missing_functions="no"
	AC_CHECK_LIB(aio, io_submit, LDFLAGS="$LDFLAGS -laio", missing_functions="yes")
dnl	if test "$missing_functions" = yes; then
dnl		AC_MSG_ERROR([KAIO is not available, using Windows native IO])
dnl 	dnl 	NC_SOURCE_ADD=disk_io_win.c
dnl 	else
dnl 		NC_SOURCE_ADD=disk_io_unix.c
dnl 		NC_LIB_ADDS="-laio"
dnl 	fi

fi 

AC_CHECK_FUNCS(preadv pwritev)


AC_CHECK_FUNCS(localtime_r strtok_r pthread_mutex_init)
dnl if test x"$ac_cv_func_pthread_mutex_init" = "xyes"; then
dnl      AC_COMPILE_IFELSE(AC_LANG_PROGRAM([[#include <pthread.h>]],
dnl  	[[int i = PTHREAD_MUTEX_RECURSIVE;]]),
dnl  	[AC_DEFINE(NC_PTHREAD_MUTEX_RECURSIVE, PTHREAD_MUTEX_RECURSIVE,
dnl  	[Define if you have PTHREAD_MUTEX_RECURSIVE])],
dnl  	[AC_COMPILE_IFELSE(AC_LANG_PROGRAM([[#include <pthread.h>]],
dnl  	[[int i = PTHREAD_MUTEX_RECURSIVE_NP;]]),
dnl  	[AC_DEFINE(NC_PTHREAD_MUTEX_RECURSIVE, PTHREAD_MUTEX_RECURSIVE_NP,
dnl  	[Define if you have PTHREAD_MUTEX_RECURSIVE_NP])])])

dnl PTHREAD_MUTEX_RECURSIVE
AC_MSG_CHECKING([pthread mutex recursive])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[#include <pthread.h>]],
                   [[int i = PTHREAD_MUTEX_RECURSIVE;]])
  ],
  [AC_DEFINE(NC_PTHREAD_MUTEX_RECURSIVE, PTHREAD_MUTEX_RECURSIVE, [Define if you have PTHREAD_MUTEX_RECURSIVE])
   AC_MSG_RESULT(PTHREAD_MUTEX_RECURSIVE)
  ],
  [AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[#include <pthread.h>]],
                     [[int i = PTHREAD_MUTEX_RECURSIVE_NP;]])
    ],
    [AC_DEFINE(NC_PTHREAD_MUTEX_RECURSIVE, PTHREAD_MUTEX_RECURSIVE_NP, [Define if you have PTHREAD_MUTEX_RECURSIVE_NP])
     AC_MSG_RESULT(PTHREAD_MUTEX_RECURSIVE_NP)
    ],
    [AC_MSG_ERROR([do not have PTHREAD_MUTEX_RECURSIVE symbol])])
  ]
)


AC_ARG_ENABLE(developer-warnings, 
         AS_HELP_STRING([--enable-developer-warnings],[enable strict warnings (default is NO)]), 
         CFLAGS="${CFLAGS} ${DEVELOPER_CFLAGS}") 

AC_ARG_ENABLE(extra-developer-warnings, 
         AS_HELP_STRING([--enable-extra-developer-warnings],[enable even stricter warnings (default is NO)]), 
         [], 
         [enable_extra_developer_warnings=no]) 
 

#if test "x$enable_stack_protector" != "xno"; then 
#         save_CFLAGS="$CFLAGS" 
#         CFLAGS="${CFLAGS} ${EXTRA_DEVELOPER_CFLAGS}" 
#         AC_COMPILE_IFELSE( 
#                 [AC_LANG_PROGRAM([],[],[])], 
#                 [], 
#                 [AC_MSG_WARN([All of ${EXTRA_DEVELOPER_CFLAGS} not supported, disabling]) 
#                     CFLAGS="$save_CFLAGS"]) 
#fi 

_nc_md="no"
AC_ARG_ENABLE(memleak, 
         AS_HELP_STRING([--enable-memleak],[enable checking memory leakage (default is NO)]), 
         [enable_memleak=yes], 
         [enable_memleak=no]) 

if test "x$enable_memleak" != "xno"; then 
	_nc_md="yes"
	echo "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ MEMLEAK-CHECK"
	read babo
	AC_DEFINE(NC_MEMLEAK_CHECK, 1, [enable memory leakage debugging])
fi 

_nc_md="no"
AC_ARG_ENABLE(asan, 
         AS_HELP_STRING([--enable-asan],[enable address sanitizer (default is NO)]), 
         [enable_asan=yes], 
         [enable_asan=no]) 

if test "x$enable_asan" != "xno"; then 
	CFLAGS="${CFLAGS} -O0 -fsanitize=address -fno-omit-frame-pointer"
fi 


_nc_md="no"
AC_ARG_ENABLE(checkoverflow, 
         AS_HELP_STRING([--enable-checkoverflow],[enable checking the allocation boundary overflow(default is NO)]), 
         [], 
         [overflowcheck=no]) 
  
if test "x$overflowcheck" != "xno"; then 
	_nc_md="yes"
	read babo
	AC_DEFINE(NC_OVERFLOW_CHECK, 1, [enable allocation overflow check])
fi 

_nc_md="no"
AC_ARG_ENABLE(debugpathlock, 
         AS_HELP_STRING([--enable-debugpathlock],[enable debugging pathlock(default is NO)]), 
         [], 
         [debugpathlock=no]) 
  
if test "x$debugpathlock" != "xno"; then 
	_nc_md="yes"
	AC_DEFINE(NC_DEBUG_PATHLOCK, 1, [enable pathlock debugging])
fi 

_nc_md="no"
AC_ARG_ENABLE(heaptrack, 
         AS_HELP_STRING([--enable-heaptrack],[enable tracking heap allocation (default is NO)]), 
         [], 
         [enable_heaptrack=no]) 

if test "x$enable_heaptrack" != "xno"; then 
	_nc_md="yes"
	AC_DEFINE(NC_HEAPTRACK, 1, [enable tracking heap allocation])
fi 

_nc_md="no"
enable_gperfprof=no
AC_ARG_ENABLE(gperfprof, 
         AS_HELP_STRING([--enable-gperfprof],[enable google perf  profling (default is NO)]), 
         [], 
         [enable_gperfprof=no]) 

if test "x$enable_gperfprof" != "xno"; then 
	_nc_md="yes"
	AC_DEFINE(NC_ENABLE_GPERFPROF, 1, [enable google perf profiling])
fi 
AM_CONDITIONAL(NCM_ENABLE_GPERFPROF, test "x${enable_gperfprof}" = "xyes")


#AC_ARG_ENABLE(stack-protector, 
#         AS_HELP_STRING([--enable-stack-protector],[enable stack protector (default is NO)]), 
#         [], 
#         [enable_stack_protector=no]) 
#  
# if test "x$enable_stack_protector" != "xno"; then 
#         save_CFLAGS="$CFLAGS" 
#         CFLAGS="${CFLAGS} -fstack-protector-all" 
#         AC_COMPILE_IFELSE( 
#                 [AC_LANG_PROGRAM([],[],[])], 
#                 [], 
#                 [AC_MSG_WARN([-fstack-protector not supported, disabling]) 
#                     CFLAGS="$save_CFLAGS"]) 
#  fi 



# Misc.
#NC_DEFAULTS
#NC_LFS
#NC_LINGUAS([ko])
#AC_DEFINE([_GNU_SOURCE], 1, [Define to enable GNU extensions])

AC_CONFIG_FILES([Makefile
                 common/Makefile
                 include/Makefile
                 plugins/Makefile
                 plugins/httpn_v2/Makefile
                 plugins/common/Makefile
                 netcache/Makefile
                 test/Makefile
				 ])
AC_OUTPUT
NC_MESSAGE
#ac_configure_args="--prefix=$ac_abs_top_builddir ---test=BABO"
dnl AX_CONFIG_DIR([httpd/geoip])

dnl ac_configure_args="--enable-shared --enable-static --enable-https --enable-bauth --enable-dauth"
dnl AX_CONFIG_DIR([httpd/MHD/0.9.28])

dnl
dnl static libunwind
dnl
dnl ac_configure_args="--disable-shared --enable-static"
dnl AX_CONFIG_DIR([libunwind-1.1])

dnl
dnl jemalloc configuration
dnl
dnl ac_configure_args="--with-jemalloc-prefix=je --disable-experimental --enable-mremap --disable-valgrind --enable-prof-libunwind --enable-xmalloc -enable-ivsalloc --with-static-libunwind=../libunwind-1.1/src/.libs/libunwind.a"
dnl AX_CONFIG_DIR([jemalloc-3.4.0])

#ac_configure_args="--with-ssl=openssl --with-libxml --enable-static --enable-nls --enable-threadsafe-ssl=posix --prefix=/usr --libdir=/usr/lib64"
#ac_configure_args="--without-ssl --with-libexpat --enable-static --enable-nls --prefix=/usr --libdir=/usr/lib64"
#AX_CONFIG_DIR([neon-0.30.1])

#ac_configure_args="--disable-shared --enable-static -enable-optimize --prefix=/usr --libdir=/usr/lib64"
#AX_CONFIG_DIR([c-ares-1.10.0])

#NEW addition(2020.4.14 by weon)
#  --disable-curldebug     Disable curl debug memory tracking
#	--enable-optimize
#	--disable-debug
#  --disable-progress-meter
#  --disable-mime          Disable mime API support

ac_configure_args="--enable-verbose --disable-mime --disable-progress-meter --disable-debug --enable-optimize --disable-curldebug --disable-shared --enable-static --enable-nls --disable-ftp --disable-pop3 --disable-smb --disable-smbs --disable-tftp --disable-ldap --disable-file --disable-ldaps --disable-rtsp --disable-telnet --disable-dict --disable-imap --disable-smtp --disable-gopher --disable-manual --disable-optimize --enable-curldebug --enable-threaded-resolver --with-ssl --without-gnutls --with-nss --without-libidn2 --without-librtmp --without-libssh2"
AX_CONFIG_DIR([curl.current])

#
#sqlite3
export CFLAGS="" CPPFLAGS="-DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_DEFAULT_WORKER_THREADS=4 -DSQLITE_THREADSAFE=2"
ac_configure_args="--disable-shared --enable-static --enable-threadsafe --with-pic"
AX_CONFIG_DIR([sqlite.current])
