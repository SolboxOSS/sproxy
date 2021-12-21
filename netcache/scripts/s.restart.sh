#!/bin/bash
#
#
# written by weon for easy restart
#
#

CONFIG=/usr/service/etc/solproxy/default.conf
LOGDIR=/usr/service/logs/solproxy
CACHEDIR=`grep cache_dir $CONFIG | egrep -v "^[ ]*#.*" | cut -d'=' -f2`

service solproxy stop

ans="N"
read -p "** Cleaning up the cache dir '$CACHEDIR', proceed(y/N)?" ans

if [ "X$ans" == "XY" -o "X$ans" == "Xy" ]
then
	find $CACHEDIR -type f -print -delete
fi

echo ""
echo ""
ans="N"
read -p "**Cleaning up the log dir '$LOGDIR', proceed(y/N)?" ans

if [ "X$ans" == "XY" -o "X$ans" == "Xy" ]
then
	rm -f $LOGDIR/*.log* $LOGDIR/*.gz
fi


service solproxy start
