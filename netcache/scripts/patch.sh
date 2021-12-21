B=/root/netcache.build
I=/usr/lib64
#ucloud-proxy (centos 6)
#TARGET_IP=14.63.226.198 

#ucloud-proxy (centos 7)
#TARGET_IP=211.253.28.61

#문제:hyunho
#TARGET_IP=192.168.110.134
#
#cent_6_aging
#TARGET_IP=192.168.40.236

#perf
TARGET_IP=192.168.10.37

#snp
#TARGET_IP=121.156.59.87

#hyunho
#TARGET_IP=192.168.40.240
#TARGET_IP=192.168.40.236
##
## ngrinder target ip
#TARGET_IP=192.168.10.37

#
#test-suite
#TARGET_IP=192.168.110.211

#TARGET_IP=192.168.130.204
TARGET_IP=192.168.130.63
echo "Target IP is '" $TARGET_IP "'"

patch()
{
	scp $1 root@$TARGET_IP:$2
	echo "patch $1 done"
}
check_same()
{
	RMD=`ssh root@$TARGET_IP md5sum $2 | cut -d' ' -f1`
	LMD=`md5sum $1 | cut -d' ' -f1`
	if [ "X$RMD" != "X$LMD" ]
	then
		echo "*patch fail: the copy mismatched with the origin"
	fi
}
patch $B/netcache/.libs/libnc.so.3.0.0 /usr/lib64
check_same $B/netcache/.libs/libnc.so.3.0.0 /usr/lib64/libnc.so.3.0.0

patch $B/plugins/httpn_v2/.libs/libhttpn_driver.so.2.0.9 /usr/lib64
check_same $B/plugins/httpn_v2/.libs/libhttpn_driver.so.2.0.9 /usr/lib64/libhttpn_driver.so.2.0.9

