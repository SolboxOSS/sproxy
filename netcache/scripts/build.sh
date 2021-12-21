#!/bin/sh
# 사용법
# 	[1] 본 스크립트를 BUILD 디렉토리 아래에 복사
#   [2] 아래에서 SOURCE 경로 수정
#   [3] devtool-set-6가 설치되었다면 scripts/setup_denv.sh 실행 
#   [4] cd BUILD;./build.sh 실행
#   [5] 화면 출력 파라미터가 맞으면 <enter>
#
#
# development tool change commands
# %scl enable devtoolset-8 bash
#

# written by weon@solbox.com
# remarks(centos 6):
# 	- yum install devtoolset-8
# 	- yum install devtoolset-8-valgrind
# 	- yum install centos-release-scl-rh
# 	- %scl enable devtoolset-8 bash
#
#
export PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/share/pkgconfig
SOURCE=/root/T/trunk
#SOURCE=/root/TT/trunk
#SOURCE=/root/netcache.svn/trunk
#SOURCE=/root/slock.netcache
#CC=gcc
CC=clang
CCPATH=`which $CC`
#CCVER="8.x"
BUILD_TYPE="debug"
#BUILD_TYPE="release"


#
# configuration options
#
#OPTS="-enable-memleak --enable-checkoverflow --enable-silent-rules --enable-heaptrack"
#OPTS="-enable-memleak --enable-checkoverflow --enable-silent-rules"
#OPTS="-enable-silent-rules --enable-heaptrack --enable-overflow"
#OPTS="-enable-silent-rules --enable-heaptrack --enable-memleak"
#OPTS="-enable-silent-rules --enable-heaptrack"
#OPTS="-enable-silent-rules --enable-checkoverflow"
#OPTS="-enable-silent-rules --enable-memleak"
#OPTS="-enable-silent-rules --enable-heaptrack"
#OPTS="-enable-silent-rules --enable-overflow" 
#OPTS="-enable-silent-rules --enable-asan" 
OPTS="-disable-silent-rules" 



if [ $BUILD_TYPE == "debug" ]
then
	CPPFLAGS='-Wunused-variable -Wunused-function -D_GNU_SOURCE -D_REENTRANT -DNC_BUILD_FOR_DEVELOP'
	LDFLAGS='-rdynamic -ldl'
	CFLAGS='-O0 -pthread'
else
	CPPFLAGS='-g -D_GNU_SOURCE -D_REENTRANT -DNC_BUILD_FOR_DEVELOP'
	LDFLAGS='-g -rdynamic -ldl'
	#CFLAGS='-g -Ofast -pthread'
	CFLAGS='-g -O2 -pthread'

fi

if [ $CC == "clang" -a $BUILD_TYPE == "debug" ]
then
	CPPFLAGS+=' -O0 -ggdb'
	LDFLAGS+=' -O0 -ggdb'
	CFLAGS+=' -O0 -ggdb'
fi

#if [ $CCVER == "4.x" ]
#then
#	#nothing
#	CFLAGS='$CFLAGS' 
#elif [ $CCVER == "8.x" ] 
#then
#CFLAGS="$CFLAGS  -fsanitize=address"
#fi
CFLAGS+=" -std=gnu99" 



# for 4.8.x
#export PATH=/opt/tools/gcc/4.8.3/rtf/bin:$PATH
#export CFLAGS='-g -DENABLE_POOL_ALLOCATOR -D__MEMORY_DEBUG'
#export CC=/opt/tools/gcc/4.8.3/rtf/bin/gcc
#export LD_LIBRARY_PATH=/opt/tools/gcc/4.8.3/rtf/lib:/opt/tools/gcc/4.8.3/rtf/lib64:$LD_LIBRARY_PATH

#export CFLAGS='-gcoff'
#export CFLAGS='-g -O2'
#export CFLAGS='-g'
#DT-4
#export CFLAGS='-g -O -D_GNU_SOURCE -D_REENTRANT  -DNC_DEBUG -fsanitize=address'
#export CFLAGS='-g -O -D_GNU_SOURCE -D_REENTRANT -fstack-protector-all'  
#gcc 5
#-fstack-protector-all : Check for stack smashing in every function.
#export CFLAGS='-g -fno-inline -fno-omit-frame-pointer -D_GNU_SOURCE -D_REENTRANT -fstack-protector-all'  

#
#DevToolSet-6
# default set if 'scl enable devtoolset-6 bash' executed
#export CC=/opt/rh/devtoolset-6/root/usr/bin/gcc
#export CPPFLAGS='-g -Wunused-variable -Wunused-function -D_GNU_SOURCE -D_REENTRANT -DNC_BUILD_FOR_DEVELOP'
#export CFLAGS='-g -fstack-protector-all'
#export LDFLAGS='-g -rdynamic'

#export CFLAGS='-g -O0 -fstack-protector-all' 
#export CPPFLAGS='-D_GNU_SOURCE -D_REENTRANT -DNC_DEBUG'
export CC
export CFLAGS
export CPPFLAGS
export LDFLAGS

clear
echo "--------------------- BUILD PARAMETERS -----------------------"
echo "SOURCE DIR  : $SOURCE"
echo "COMPILER    : $CCPATH"
echo "CFLAGS      : $CFLAGS"
echo "CPPFLAGS    : $CPPFLAGS"
echo "BUILD OPTION: $OPTS"
echo "--------------------------------------------------------------"

read -p "Hit any key to proceed " inp

echo "$SOURCE/configure $OPTS --enable-static=libconv --enable-shared --prefix=/root/libnetcache/2.5.0 2>&1 | tee /tmp/c.log"
$SOURCE/configure $OPTS --enable-static=libconv --enable-shared --prefix=/root/libnetcache/2.5.0 2>&1 | tee /tmp/c.log
