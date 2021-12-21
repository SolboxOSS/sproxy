#include <netcache_config.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include "trace.h"
#include <pthread.h>
#include <mm_malloc.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "ncapi.h"
#define __PROFILE
#include "rdtsc.h"
//#include <duma.h>
#define	BLOCK_SIZE		(4096*64)



nc_uint32_t		o_logmask = NC_WARN|NC_ERROR|NC_INFO;
char			o_log_file[128] = "./tp.log";

char 			o_driver_class[128] = "WebDAV";
char 			*o_driver_path = "/root/libnetcache/2.5.0/lib/libwebdav_driver.so";

struct tag_option {
	int 	command;
	union {
		int 	intval;
		char 	strval[128];
	} u;
#define	ival u.intval
#define	sval u.strval
	int	valsize;
} nc_global_options[] =  {
	{NC_GLOBAL_BLOCK_SPIN_INTERVAL, .ival = 120, 			sizeof(int)}, 
	{NC_GLOBAL_CLUSTER_IP, 			.sval = "", 			-1},
	{NC_GLOBAL_LOCAL_IP, 			.sval = "", 			-1},
	{NC_GLOBAL_CLUSTER_TTL, 		.ival = 0,  			sizeof(int)},
	{NC_GLOBAL_CACHE_PATH, 			.sval = "/stg/cache", 	-1},
	{NC_GLOBAL_CACHE_MEM_SIZE, 		.ival = 1024, 			sizeof(int)},
	{NC_GLOBAL_CACHE_MAX_INODE, 	.ival = 500, 			sizeof(int)},
	{NC_GLOBAL_MAX_PATHLOCK, 		.ival = 70, 			sizeof(int)},
	{NC_GLOBAL_MAX_STATCACHE, 		.ival = 70, 			sizeof(int)},
	{NC_GLOBAL_CACHE_BLOCK_SIZE, 	.ival = 256, 			sizeof(int)},
	{NC_GLOBAL_ENABLE_FASTCRC, 		.ival = 1, 				sizeof(int)},
	{NC_GLOBAL_CACHE_MAX_ASIO, 		.ival = 128, 			sizeof(int)},
	{NC_GLOBAL_CACHE_DISK_RA, 		.ival = 4, 				sizeof(int)},
	{NC_GLOBAL_CACHE_NETWORK_RA, 	.ival = 8, 				sizeof(int)},
	{NC_GLOBAL_CACHE_RA_MINTHREADS, .ival = 8 , 			sizeof(int)},
	{NC_GLOBAL_CACHE_RA_MAXTHREADS, .ival = 128, 			sizeof(int)},
	{NC_GLOBAL_CACHE_POSITIVE_TTL, 	.ival = 60 , 			sizeof(int)},
	{NC_GLOBAL_CACHE_NEGATIVE_TTL, 	.ival = 300, 			sizeof(int)},
	{NC_GLOBAL_CACHE_DISK_LOW_WM, 	.ival = 80, 			sizeof(int)},
	{NC_GLOBAL_CACHE_DISK_HIGH_WM, 	.ival = 90, 			sizeof(int)},
	{NC_IOCTL_STORAGE_CHARSET, 		.sval = "CP949", 		-1},
	{-1, 							.ival = 0, 				-1}

};


nc_origin_info_t 	o_origin_info_array[1] = {
		{
        	.address = "http://redeyestg012nd.x-cdn.com/dav",
			.prefix = "",
			.user = "redeyestg012nd@tstest",
			.pass = "b6f257e8d2339ee4ae4a813c541cefe0",
			.encoding = "utf-8",
			.ioflag = NCOF_READABLE
		}
	};



nc_volume_context_t 	*volume = NULL;


void my_exit()
{
}
int 
dir_list(nc_volume_context_t *vol, void *buf, const char *name, const nc_stat_t *statp, nc_off_t off)
{

	fprintf(stderr, ">> [%s]\n", name);
	return 0;
}
int
main(int argc, const char *argv[])
{
	nc_xtra_options_t 	*kv = NULL;
	nc_xtra_options_t 	*kv_c = NULL;
	char 			*v;
	//char 				key[128];
	char 				value[256];
	//char 				dumps[10240];
	int 				r;
	int 				uid=0;
	int 			i;	
	perf_val_t 	ms, me;
	long long	ud, sum = 0;
	char 				key_arr[20][64];


	o_logmask = NC_WARN|NC_ERROR|NC_INFO;
 	nc_setup_log(o_logmask, o_log_file, 50000, 10);
#if 0
	for (i = 0;  nc_global_options[i].command > 0; i++) {
		if (nc_global_options[i].valsize > 0) {
			nc_set_param(nc_global_options[i].command, nc_global_options[i].u.strval, nc_global_options[i].valsize);
		}
		else {
			nc_set_param(nc_global_options[i].command, nc_global_options[i].u.strval, strlen(nc_global_options[i].sval));
		}
	}


	if (nc_load_driver(o_driver_class, o_driver_path) < 0) {
		fprintf(stderr, "error in load driver, '%s'\n", o_driver_path);
		exit(1);
	}

	nc_init();
#endif
	for (i = 0; i < 20; i++) {
		sprintf(key_arr[i], "%c-KEY VALUE", 'A' + i);
	}
	fprintf(stderr, "starting...\n");


	kv = kv_create_d(__FILE__, __LINE__);
	srand(time(NULL));
	for (i = 0; i < 100000; i++) {
		uid = rand() % 20;
		r = rand() % 10;
		switch (r) {
			case 0:
			case 2:
				sprintf(value, "%d", uid);
				ASSERT(nc_check_memory(kv, 0) != 0);
				kv_add_val_d(kv, key_arr[uid], value, __FILE__, __LINE__);
				ASSERT(nc_check_memory(kv, 0) != 0);
				break;
			case 1:
			case 3:
				DO_PROFILE_USEC(ms, me, ud) {
					kv_remove(kv, key_arr[uid], 1);
				}
				sum += ud;
				ASSERT(nc_check_memory(kv, 0) != 0);
				break;
			case 4:
			case 5:
			case 6:
			case 7:
			case 8:
				DO_PROFILE_USEC(ms, me, ud) {
					v = kv_find_val(kv, key_arr[uid], 1);
				}
				sum += ud;
				if (v) {
					int 	tuid = atoi(v);
					ASSERT(tuid == uid);
				}
				break;
			case 9:
				kv_c = kv_clone_d(kv, __FILE__, __LINE__);
				ASSERT(kv_c != NULL);
				ASSERT(kv_c != kv);
				kv_destroy(kv_c);
				break;
		}
		
	}
#if 0
	nc_shutdown();
#endif
	fprintf(stderr, "Sum = %lld usec\n", sum);

}
