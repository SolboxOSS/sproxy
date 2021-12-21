#!/bin/sh
#DR=/root/tools/DrMemory-Linux-1.11.0-2
DR=/root/tools/dr.current
export CSA_PIDFILE=/var/run/solproxy.pid
export CSA_CONFPATH=/usr/service/etc/solproxy
export PATH=$PATH:$DR/bin64:$DR/dynamorio/bin64

#drmemory -crash_at_unaddressable   -crash_at_error   -ignore_early_leaks -light -report_leak_max 300000 -- /usr/service/sbin/solproxy --standalone=1
#drmemory -crash_at_error   -ignore_early_leaks -light -report_leak_max 300000 -- /usr/service/sbin/solproxy --standalone=1
drmemory -ignore_early_leaks -light -report_leak_max 300000 -logdir /usr/service/logs/solproxy -- /usr/service/sbin/solproxy --standalone=1
#drmemory -crash_at_unaddressable   -light -crash_at_error   -ignore_early_leaks -logdir /usr/service/logs/solproxy -- /usr/service/sbin/solproxy --standalone=1
#drmemory -dr_ops "-vm_size 30000M" -light -leaks_only -logdir /usr/service/logs/solproxy -- /usr/service/sbin/solproxy --standalone=1
#drmemory -leaks_only -logdir /usr/service/logs/solproxy -- /usr/service/sbin/solproxy --standalone=1
#drmemory -dr_ops "-vm_size 10000M" -light -logdir /usr/service/logs/solproxy -- /usr/service/sbin/solproxy --standalone=1
