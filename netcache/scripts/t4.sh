#!/bin/sh
alias rm='rm'
PID=$$
procs=0
N=0
threshold=64
tosched=0
while true
do
    if [ "$procs" -le $threshold ]; then
       procs=`ps auxw|grep t4run.sh |wc -l`
       tosched=$(($threshold - $procs))
       N=$(($N+1))
       D=`date`
       echo test4 $D \| $procs \| ${N} \| $tosched
       while [ $tosched -gt 0 ];do
           ./t4run.sh &
           tosched=$(($tosched - 1))
#           echo "tosched=$tosched"
       done
    else
       procs=`ps auxw|grep t4run.sh |wc -l`
       D=`date`
       echo test4 $D $procs "waiting..."
       sleep 0.5
    fi
done
