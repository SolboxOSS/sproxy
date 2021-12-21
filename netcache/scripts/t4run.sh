#!/bin/sh
PID=$$
NUM=${RANDOM}
#SIZE=$((${NUM}*${PID}))
SIZE=$((${RANDOM}%100))
NUM=$((${SIZE}%20))
SIZE=$((${SIZE}*1000000))
FSIZE=$SIZE;
FILE=/vfs/$((${NUM}))/test.${SIZE}1
REM=$((${NUM}%3))
#ROFF=$((${RANDOM}%${SIZE}))
#START=$((${SIZE}%${RANDOM}))
#START=$ROFF
#END=$((${START} + 4000000))
#if [ $END -gt $SIZE ];then
# 	END=$(($SIZE -1))
#fi
#END=$((${SIZE}/4*2-1))
#if [ $REM == 0 ]; then
#   ./t4curl -s -m 300 -o /dev/null -r ${START}-${END}  -A SOLBOX "http://218.145.46.4${FILE}" >/dev/null 2>&1 
#fi
#START=$((${SIZE}/4-1))
#END=$((${SIZE}/4*3-1))
#if [ $REM == 1 ]; then
#   ./t4curl -s -m 300 -o /dev/null -r ${START}-${END}  -A SOLBOX "http://218.145.46.4${FILE}" >/dev/null 2>&1 
#fi
#START=$((${SIZE}/2-1))
END=$((${SIZE}-1))
#if [ $REM == 2 ]; then
#   ./t4curl -s -m 300 -o /dev/null -r ${START}-${END}  -A SOLBOX "http://218.145.46.4${FILE}" >/dev/null 2>&1 
#fi

UNIT=100000
OFF=0
remained=$FSIZE
#echo  ./t4curl -s -m 300 -o /dev/null -r 0-10000 -H "Host: localhost.localdomain:8080"   -A SOLBOX "http://218.145.46.4${FILE}" 
#echo "$FILE" 
while [ $remained -gt 0 ]
do

   if [ ${UNIT} -gt ${remained} ];then
	UNIT=$remained
   fi
   END=$((${OFF} + ${UNIT}-1)) 
  
#   echo "${FILE} :: ${OFF}-${END}" 
   curl -s -m 300 -r ${OFF}-${END} -o /dev/null -H 'Host: 218.145.46.4:8080'  -A SOLBOX "http://127.0.0.1${FILE}" >/dev/null 2>&1 
   OFF=$END
   remained=$((${remained}-${UNIT}))
   sleep 0.1
done
