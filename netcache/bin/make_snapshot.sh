ROOT=/stg/work/weon
SUFFIX=`date +%F`
TARGET=$ROOT/snapshot/netcache-2.5.$SUFFIX
SOURCE=`pwd`/..
XCLUDE="--exclude=build.win32 --exclude=build.win32.x86_64 --exclude=build.linux.x86_64 --exclude=*.log --exclude=*.org --exclude=*.dll"
mkdir -p $TARGET
if [ $? == 0 ]
then
	(cd $SOURCE; tar czvf - * $XCLUDE) | (cd $TARGET; tar xzvf -) 
	echo "snapshoting to $TARGET done"
else
	echo "mkdir -p $TARGET failed"
fi
