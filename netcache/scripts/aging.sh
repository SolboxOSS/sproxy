#!/bin/bash --norc
# DESCRIPTION
# 	- 대상 파일 크기 			: FILE_SIZE
# 	- 대상 파일 이름 			: FILE_NAME_PATTERN에서 'X' 문자를 랜덤 숫자로 생성 
#   - 대상 파일 갯수 			: FILE_COUNT
#	- 동시 접속 클라이언트 수 	: NTASKS
#	- 시험 타겟 주소			: PROXY
#	- ORIGIN 호스트 명 			: ORIGIN
# 	- 파일 읽기 최대 크기 		: SZLIMIT
# 	- 시험 횟수   				: MAXLOOP
# 
#	위의 설명의 설정값을 지정하면 random 으로 생성된 파일 명에 대해서
# 	random으로 읽기 영역을 생성해서 HTTP GET 실행
#
#	written by weon@solbox.com
#
#
#FILE_COUNT=500
FILE_COUNT=400
FILE_SIZE=10000000
#FILE_SIZE=100000
SUBRANGE=0
#CURL_OPT="--limit-rate 10K"

#FILE_NAME_PATTERN="100KB_XXXXXXX"
FILE_NAME_PATTERN="100MB_XXXXXXX"
#FILE_NAME_PATTERN="10MB_XXXXXXX"
#FILE_NAME_PATTERN="1MB_XXXXXXX"
#FILE_NAME_PATTERN="1KB_XXXXXXX"
#PROXY=192.168.40.249:8080
PROXY=127.0.0.1:8080
ORIGIN=origin.media.com
SZLIMIT=1000000
MAXLOOP=10000000
URL=""
MYID=0
NTASKS=1


trap 'cleanup' INT

cleanup()
{
	pids=`jobs -p`
	for pid in $pids
	do
		kill $pid 2>/dev/null;
	done
	echo "cleaned up"
}

do_loop() 
{
	RLOOP=0
	LOOP=$1
	MYID=$2
	while [ $LOOP -gt 0 ]
	do
		#FILE_NUMBER=$(( ( RANDOM % $FILE_COUNT )  + 1 ))
		my_RANDOM=`od -vAn -N7 -tu8 < /dev/urandom`
		FILE_NUMBER=$(( my_RANDOM % $FILE_COUNT ))
		
		#FILE_NUMBER=`date +%s`
		#FILE_NUMBER=$((FILE_NUMBER  + MYID)) 
		#FILE_NUMBER=$((FILE_NUMBER  % FILE_COUNT)) 

		LOOP=$((LOOP - 1))
		RLOOP=$((RLOOP + 1))
		if [ $SUBRANGE != 0 ]
		then
			URANDOM=`od -vAn -N7 -tu8 < /dev/urandom`
			O1=$(( (URANDOM % $FILE_SIZE) ))
	
			URANDOM=`od -vAn -N7 -tu8 < /dev/urandom`
			O2=$(( (URANDOM % $FILE_SIZE) ))
			if [ $O1 -gt $O2 ]
			then
				T=$O1 O1=$O2 O2=$T
			fi
			OSIZ=$((O2 - O1))
			if [ $OSIZ -gt $SZLIMIT ]
			then
				O2=$((O1 + $SZLIMIT))
			fi
		fi
		FILE_NAME="${FILE_NAME_PATTERN/XXXXXXX/$FILE_NUMBER}"
#		URL="http://$PROXY/bysize/100KB/$FILE_NAME"
#		URL="http://$PROXY/bysize/10MB/$FILE_NAME"
		URL="http://$PROXY/bysize/100MB/$FILE_NAME"
#      	curl  -o /dev/null  -v -H "Host: $ORIGIN" -H "Range: bytes=$O1-$O2" $URL
		if [ $SUBRANGE != 0 ]
		then
        	echo $MYID[$RLOOP]:: curl  -o /dev/null  $CURL_OPT -v -H "Host: $ORIGIN" -H "Range: bytes=$O1-$O2" $URL
      		curl -s -o /dev/null  $CURL_OPT  -H "Host: $ORIGIN" -H "Range: bytes=$O1-$O2" $URL
		else
        	echo $MYID[$RLOOP]:: curl  -o /dev/null  $CURL_OPT -v -H "Host: $ORIGIN" $URL
      		curl -s -o /dev/null   $CURL_OPT -H "Host: $ORIGIN" $URL
		fi
#        echo $URL \( $O1-$O2 \) "..... done"
	done
}


while [ $NTASKS -gt 0 ]
do
	do_loop $MAXLOOP $NTASKS &
	NTASKS=$((NTASKS - 1));
done


for t in `jobs -p`
do
	wait $t
done
