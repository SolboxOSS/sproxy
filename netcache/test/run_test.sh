rm -f dt_*.log* check.result


#CACHEPATH=f:/cache
CACHEPATH=/root/ext4

MEMSIZ=1024
THREADS=4
MAXLOOP=100000
TARGETFILES=300
MAXINODES=290
VALCMD="valgrind --track-origins=yes --show-reachable=yes --log-file=./check.result --leak-check=full" 
#windows
#./driver_test -t 20 -M 1024 -C f:/cache -G loopback -l ./dt.log -b  -I 1000 -L 100000
#./driver_test -t 1 -M 1024 -C f:/cache -G WebDAV -l ./dt.log -b  -I 200 -L 10
#linux
#$VALCMD ./driver_test -t 30 -M 1024 -C /stg/weon_cache -G WebDAV -l `pwd`/dt.log -i 5000 -I 10000 -a -L 10000
#$VALCMD ./driver_test -t 30 -M 1024 -C /stg/cache -G WebDAV -l `pwd`/dt.log -i 5000 -I 10000 -a -L 10000
#echo "running $VALCMD ./driver_test -t 1 -M 1024 -C /stg/cache -G WebDAV -l `pwd`/dt.log -I 10000 -I 1000 -b -L 10000000"
#$VALCMD ./driver_test -t 5 -M 4092 -C /stg/cache -G WebDAV -l `pwd`/dt.log -I 10000 -I 200 -b -L 10000000
#$VALCMD ./driver_test -t 5 -M 4092 -C f:/cache -G WebDAV -l `pwd`/dt.log -I 10000 -I 200 -b -L 10000000
#$VALCMD ./driver_test -t 2 -M 2048 -C /stg/cache -G WebDAV -l `pwd`/dt.log -I 10000 -I 500 -b -L 10000000
#$VALCMD ./driver_test -t 30 -M 4092 -C /stg/cache -G WebDAV -l `pwd`/dt.log -I 10000 -I 500 -w -L 10000000
echo "running $VALCMD .libs/driver_test -t $THREADS -M $MEMSIZ -C $CACHEPATH -G WebDAV -l `pwd`/dt.log -I 10000 -I 500 -b -L 10000000 "
#
$VALCMD ./driver_test -t $THREADS -M $MEMSIZ -C $CACHEPATH -G WebDAV -l `pwd`/dt.log -i $MAXINODES -I $TARGETFILES  -b -L $MAXLOOP
