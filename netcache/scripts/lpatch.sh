B=/root/netcache.build

cp $B/netcache/.libs/libnc.so.3.0.0 /root/libnetcache/2.5.0/lib/
cp $B/plugins/httpn_v2/.libs/libhttpn_driver.so.2.0.9 /root/libnetcache/2.5.0/lib/

cp $B/netcache/.libs/libnc.so.3.0.0 /usr/lib64/.
cp $B/plugins/httpn_v2/.libs/libhttpn_driver.so.2.0.9 /usr/lib64/.

md5sum $B/netcache/.libs/libnc.so.3.0.0
md5sum /usr/lib64/libnc.so
md5sum $B/plugins/httpn_v2/.libs/libhttpn_driver.so.2.0.9 
md5sum /usr/lib64/libhttpn_driver.so
rm -f /usr/lib64/libhttpn_driver.so
rm -f /usr/lib64/libhttpn_driver.so.2
ln -s /usr/lib64/libhttpn_driver.so.2.0.9 /usr/lib64/libhttpn_driver.so 
ln -s /usr/lib64/libhttpn_driver.so.2.0.9 /usr/lib64/libhttpn_driver.so.2
rm -f /usr/lib64/libnc.so 
rm -f /usr/lib64/libnc.so.2 
rm -f /usr/lib64/libnc.so.3 
ln -s /usr/lib64/libnc.so.3.0.0 /usr/lib64/libnc.so 
ln -s /usr/lib64/libnc.so.3.0.0 /usr/lib64/libnc.so.2 
ln -s /usr/lib64/libnc.so.3.0.0 /usr/lib64/libnc.so.3 


check_same()
{
	m1=`md5sum $1 | cut -f1 -d' '`
	m2=`md5sum $2 | cut -f1 -d' '`

	if [ "$m1" != "$m2" ]
	then
		echo "MD5 different ($1, $2)"
	fi
}

check_same /usr/lib64/libnc.so.3.0.0 $B/netcache/.libs/libnc.so.3.0.0
check_same /usr/lib64/libhttpn_driver.so.2.0.9 $B/plugins/httpn_v2/.libs/libhttpn_driver.so.2.0.9

rm -f /usr/service/logs/solproxy/*.log*
