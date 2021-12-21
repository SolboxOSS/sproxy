#!/bin/bash 
LOGROOT=/usr/service/logs


cleanup_logs()
{
	cd $LOGROOT/solproxy
	cat /dev/null > access.log
	cat /dev/null > error.log
	cat /dev/null > origin.log
	rm -f sp*.log.*
	ls -l
	
	cd $LOGROOT/nginx
	rm -f *.log-2020*
	rm -f *.gz*

	cat /dev/null > access.log
	cat /dev/null > error.log
	ls -l

	find /usr/service/stat -type f -delete

	df -h
	echo "cleanup done"
}

while [ 1 ]
do
cleanup_logs
sleep 60
done
