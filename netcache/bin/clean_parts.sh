#!/bin/bash

usage()
{
cat<<EOF
usage: $0 options
OPTIONS:
	-h : show usage
	-q : quiet mode
	-f : scan and remove all files
EOF
}

PARTS=""
ERROR=0

QUIET=0
FINDRM=0
while getopts "fqh" OPTION
do
	case $OPTION in
		q)
			QUIET=1
			;;
		h)
			usage
			exit 1
			;;
		f)
			FINDRM=1
			;;
	esac
done

read_dom() 
{
	local 	IFS=\>
	read -d \< ENTITY CONTENT
}
NEED_SEP=0

#
#strip of comment lines
#
awk '$0 !~ /<!--.*-->/ {print $0;}' < /usr/service/etc/nc.xml > /tmp/nc$$.tmp
while read_dom
do
	if [[ $NEED_SEP = 1 ]]
	then
		PARTS+=" "
		NEED_SEP=0
	fi
	if [[ $ENTITY = "CacheDir" ]]
	then
		PARTS+=$CONTENT
		NEED_SEP=1
	fi
done  < /tmp/nc$$.tmp

rm -f /tmp/nc$$.tmp
echo "parts=$PARTS"


cleanup()
{
# $1 : mount point
# $2 : device path
	if [[ -z $1 ]]
	then
		return -1
	fi
	if [[ -z $2 ]]
	then
		return -1
	fi
	DEV=$2
	MP=$1
	ERROR=0

	echo "formatting partition, $DEV..."
		umount $DEV
		ERROR="$?"
		mkfs.ext4 -q $DEV
		ERROR="$?"
		if [ $ERROR != "0" ] 
		then
			echo "mkfs $MP failed: $ERROR";
			exit 1;
		fi
		mount $MP
		ERROR="$?"
		if [ $ERROR != "0" ] 
		then
			echo "mount $MP failed: $ERROR";
			exit 1;
		fi
	return 0
}


for i in $PARTS
do
	DEV=`cat /etc/fstab | awk -v mp="$i" 'BEGIN {FS=" ";} { if ( $2 == mp ) {print $1;}}'`
	if [[ $QUIET == 0 ]]
	then
		echo -n "confirm to clean partition, '$DEV' for cache partition $i(Y/N)";
		read ans
	else
		ans="Y"
	fi
	if [[ "${ans}" = "Y" ]] || [[ "${ans}" = "y" ]]
	then
		if [[ $FINDRM = 1 ]]
		then
			echo $i - cleaning...
			find $i -type f -exec rm -f {} \;
			echo $i - cleaned
		else
			cleanup $i $DEV
		fi
	fi
done
