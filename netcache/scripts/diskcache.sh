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
FILE_COUNT=2000
FILE_SIZE=100000000

#FILE_NAME_PATTERN="100KB_XXXXXX"

FILE_NAME_PATTERN="100MB_XXXXXXX"
#PROXY=192.168.40.249:8080
PROXY=127.0.0.1:8080
ORIGIN=origin.media.com
SZLIMIT=10000000
MAXLOOP=10
URL=""
MYID=0
NTASKS=10


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

do_loop_GET() 
{
	LOOP_COUNT=$1
	MYID=$2
	FILE_COUNT=$3
	for l in $(seq 1 $LOOP_COUNT)
	do
		for f in $(seq 1 $FILE_COUNT)
		do
			URANDOM=`od -vAn -N7 -tu8 < /dev/urandom`
			O1=$(( (URANDOM % $FILE_SIZE) ))
	
			URANDOM=`od -vAn -N7 -tu8 < /dev/urandom`
			O2=$(( (URANDOM % $FILE_SIZE) ))
			FILE_NAME="${FILE_NAME_PATTERN/XXXXXXX/$f}"

			if [ $O1 -gt $O2 ]
			then
				T=$O1 O1=$O2 O2=$T
			fi

			OSIZ=$((O2 - O1))

			if [ $OSIZ -gt $SZLIMIT ]
			then
				O2=$((O1 + $SZLIMIT))
			fi
	
			URL="http://$PROXY/bysize/100MB/$FILE_NAME"
        		echo $MYID[$l:$f]:: curl  -o /dev/null  -v -H "Host: $ORIGIN" -H "Range: bytes=$O1-$O2" $URL
      			curl -s -o /dev/null   -H "Host: $ORIGIN" -H "Range: bytes=$O1-$O2" $URL
		done
	done
}
do_hard_purge()
{
	curl -v -m 10 -o /dev/null -H "Host: $ORIGIN"  http://$PROXY/* -X PURGE -H "X-Purge-Hard: yes"

}


while [ $NTASKS -gt 0 ]
do
#
#	@1 : loop
#	@2 : task-id
#	@3 : file count
#
	do_loop_GET 5 $NTASKS 2000 &
	NTASKS=$((NTASKS - 1));
done



for t in `jobs -p`
do
	wait $t
done

do_hard_purge
