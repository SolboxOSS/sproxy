#rm -f dt_*.log* check.result
#VALCMD="/usr/bin/valgrind --tool=callgrind --log-file=./soldrive.prof "
VALCMD="/usr/bin/valgrind --tool=memcheck --log-file=./soldrive.prof --trace-children=yes"
#echo "running $VALCMD ./driver_test -t 1 -M 1024 -C /stg/cache -G WebDAV -l `pwd`/dt.log -I 10000 -I 1000 -b -L 10000000"
$VALCMD  /usr/service/sbin/solproxy 
