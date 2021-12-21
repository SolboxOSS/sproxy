#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.59])
AC_INIT([solproxy], [1.0.1], [k20245@solbox.com])
AC_CONFIG_SRCDIR([logger.c])
AC_CONFIG_AUX_DIR([config])

VERSION="1.0.1"
AM_INIT_AUTOMAKE(solproxy, $VERSION)

echo "HOST OS=" $host_os "OS_TYPE=" $os_type
AM_CONDITIONAL([OS_WIN32], [test "$os_type" = "win32"])
AM_CONDITIONAL([OS_UNIX], [test "$os_type" != "win32"])
AM_CONDITIONAL([OS_LINUX], [test "$os_type" = "linux"])


# Checks for programs.
AC_PROG_AWK
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LEX
AC_PROG_YACC
AC_PROG_LIBTOOL
AC_PROG_LN_S
#AC_PROG_CC_C99


# Checks for libraries.
# FIXME: Replace `main' with a function in `-lGeoIP':
AC_CHECK_LIB([GeoIP], [main], , AC_MSG_ERROR([GeoIP not found!]))
# FIXME: Replace `main' with a function in `-ldl':
AC_CHECK_LIB([dl], [main])
# FIXME: Replace `main' with a function in `-lgcrypt':
AC_CHECK_LIB([gcrypt], [main])
# FIXME: Replace `main' with a function in `-lgnutls':
AC_CHECK_LIB([gnutls], [main])
# FIXME: Replace `main' with a function in `-lhistory':
AC_CHECK_LIB([history], [main])
# FIXME: Replace `main' with a function in `-llua':
#AC_CHECK_LIB([lua], [main])
# FIXME: Replace `main' with a function in `-lm':
AC_CHECK_LIB([m], [main])
# FIXME: Replace `main' with a function in `-lnc':
AC_CHECK_LIB([nc], [main])
# FIXME: Replace `main' with a function in `-lreadline':
AC_CHECK_LIB([readline], [main])
# FIXME: Replace `main' with a function in `-lrt':
AC_CHECK_LIB([rt], [main])
# FIXME: Replace `main' with a function in `-lstrict':
AC_CHECK_LIB([strict], [main])

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h limits.h locale.h malloc.h memory.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/socket.h sys/time.h syslog.h unistd.h])

AC_SEARCH_LIBS([shm_open], [rt], [AM_CONDITIONAL([NC_LIBRT], [true])],[AM_CONDITIONAL([NC_LIBRT], [false])]
)

#makefile.am에서 ifndef를 사용하기 위해 선언
#http://gnu-automake.7480.n7.nabble.com/ifdef-endif-issue-td2662.html 참조
AC_SUBST([protect], [""])
AC_SUBST([equal], ["="])


# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_UID_T
#AC_TYPE_INT32_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
#AC_TYPE_SSIZE_T
AC_STRUCT_TIMEZONE
#AC_TYPE_UINT32_T
#AC_TYPE_UINT64_T
#AC_TYPE_UINT8_T
AC_CHECK_TYPES([ptrdiff_t])

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
AC_FUNC_FORK
AC_FUNC_MKTIME
AC_FUNC_STRCOLL
AC_CHECK_FUNCS([floor gethostname getpagesize gettimeofday localeconv localtime_r memchr memmove memset modf pow regcomp setenv setlocale socket sqrt strcasecmp strchr strcspn strdup strerror strncasecmp strpbrk strrchr strstr strtol strtoul tzset])
AC_MSG_CHECKING(for nc-include nci-libs)
# for netcache library 
AC_ARG_WITH(nc-include,
        AC_HELP_STRING([--with-nc-include@<:@=DIR@:>@],[netcache core header path]),
        [NC_INCLUDE=$withval],[NC_INCLUDE=/root/netcache-2.5/include])

AC_ARG_WITH(nc-libs,
        AC_HELP_STRING([--with-nc-libs@<:@=DIR@:>@],[netcache core library path]),
        [NC_LIBS=$withval],[NC_LIBS=/root/libnetcache/2.5.0/lib])

AC_ARG_WITH(mdh_pkgconf,
        AC_HELP_STRING([--with-mdh-pkgconf@<:@=DIR@:>@],[mhd custom pkgconfig path for gnutls]),
        [pkg_conf_arg="PKG_CONFIG_PATH=$withval"],[])
        
AC_MSG_CHECKING(for netcache core library header)
AC_MSG_RESULT($NC_INCLUDE)
AC_MSG_CHECKING(for netcache core library binary)
AC_MSG_RESULT($NC_LIBS)

AC_SUBST(NC_LIBS)
AC_SUBST(NC_INCLUDE)


# for HLS segmentor jpeg image 
AC_ARG_ENABLE([thumbnail],
        AS_HELP_STRING([--enable-thumbnail], [Enable HLS thumbnail creation, with FFMPEG]),
        [AM_CONDITIONAL([DISABLE_THUMBNAIL],[false])],
        [AM_CONDITIONAL([DISABLE_THUMBNAIL],[true])])
AC_MSG_RESULT($DISABLE_THUMBNAIL)

# for HLS segmentor AES-NI  
AC_ARG_ENABLE([aesni],
        AS_HELP_STRING([--enable-aesni], [Enable HLS AES-NI packet encryption]),
        [AM_CONDITIONAL([ENABLE_AESNI],[true])],
        [AM_CONDITIONAL([ENABLE_AESNI],[false])])
AC_MSG_RESULT($ENABLE_AESNI)

# for Release Mode
AC_ARG_ENABLE([release],
                AS_HELP_STRING([--enable-release],
                        [Release Mode Build]),
                [release_build=yes],
                [release_build=no])
AC_MSG_RESULT($release_build)  
echo "release_build = $release_build"

if test "$release_build" = "no"
then
CFLAGS=" -std=c99 -g -O0 -DDEBUG -rdynamic"
else
CFLAGS=" -std=c99 -g -O2 -rdynamic"
fi

CFLAGS+=" -fstack-protector-all"

# for libmicrohttpd 
AC_ARG_ENABLE([epoll],
                AS_HELP_STRING([--disable-epoll],
                        [disable epoll]),
                [enable_epoll=${enableval}],
                [enable_epoll=yes])
AC_MSG_RESULT($enable_epoll)


echo "ac_configure_args = $ac_configure_args"
main_configure_args=$ac_configure_args

ac_configure_args="--enable-shared --disable-static --enable-https --enable-bauth --enable-dauth --enable-epoll --enable-messages $pkg_conf_arg"


AX_CONFIG_DIR([MHD/0.9.73])

ac_configure_args=""
AX_CONFIG_DIR([jansson])

#centos 6에서 gnutls 3.x 버전 사용을 위해 설정, https://jarvis.solbox.com/redmine/issues/32838 일감 참조
#centos 6를 사용하지 않게 되면 아래 부분은 제거 해도 된다.
if test -z "$pkg_conf_arg"
then
	echo "use default gnutls"
else
	echo "use custom gnutls"
	CFLAGS+=" -I/usr/include/gnutls30" 
fi

#reconf시 configure option이 바뀌지 않게 하기 위해 기억해 놓았던 arguments를 복원한다.
ac_configure_args=$main_configure_args
AC_CONFIG_FILES([Makefile])

AC_OUTPUT
