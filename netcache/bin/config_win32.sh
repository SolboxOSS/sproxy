export PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/share/pkgconfig
#export CFLAGS='-g -DENABLE_POOL_ALLOCATOR -D__MEMORY_DEBUG'
#export CFLAGS='-gcoff'
#export CFLAGS='-g -O2'
export CFLAGS='-g' 
#set PATH='/usr/bin:${PATH}' 
#export PATH
#./configure --enable-shared --prefix=`pwd` --enable-stack-protector
# mount d:/mingw64/mingw64 /mingw
# mount d:/mingw64/mingw64 /mingw64
../tools/set32.sh
../configure --enable-shared --prefix=/root/libnetcache/2.0.0
