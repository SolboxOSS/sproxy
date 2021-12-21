#!/bin/sh
LF=/tmp/
TD=/stg/netcache_mount/346
CT=0

while [ true ]
do

	dd if=/dev/urandom if=
	cp $F $TD/T
	diff $F $TD/T
	if [ $? != 0 ]
	then
		echo "overwrite error found"
		break;
	fi
	let $CT=$CT+1
	sleep 1
done
