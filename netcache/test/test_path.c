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
//#include <duma.h>
#define	BLOCK_SIZE		(4096*64)
#ifdef WIN32
#define		TRAP	DebugBreak()
#else
#define		TRAP	abort()
#endif



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
main(const char *argv[], int argc)
{
	char 		vsignature[] = "default";
	nc_stat_t	ss;
	nc_file_handle_t 	*f = NULL;
	char		dpath[] = "/4780/11226/kculod/sample_estream/042004/20152/so100701/1/1/viewer/images";
	char		tpath[1024] = "/4780/11226/kculod/sample_estream/042004/20152/so100701/1/1/viewer/images/index_bg.gif";
	char		buf[1024];
	size_t 		rs = 0;
	int 		loop = 3;
	int 		i;




	o_logmask = NC_WARN|NC_ERROR|NC_INFO;
 	nc_setup_log(o_logmask, o_log_file, 50000, 10);

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

	volume = nc_create_volume_context(o_driver_class, vsignature, o_origin_info_array, sizeof(o_origin_info_array)/sizeof(o_origin_info_array));
	if (!volume) {
		fprintf(stderr, "error in create\n");
	}

	while (loop > 0) {
		nc_readdir(volume, dpath, dir_list, dpath);
		loop--;
		fprintf(stderr, "[%d] remained.\n", loop);
		bt_msleep(1000);
	}

	if (nc_getattr(volume, tpath, &ss) < 0) {
		fprintf(stderr, "error in getattr(%s) :%d\n", tpath,errno);
		exit(1);
	}
	fprintf(stderr, "[%s]'s size=%lld\n", tpath, ss.st_size);
	f = nc_open(volume, tpath, O_RDONLY);
	if (!f) {
		fprintf(stderr, "error in open(%s) :%d\n", tpath,errno);
		exit(1);
	}
	rs = nc_read(f, buf, 0, sizeof(buf));
	fprintf(stderr, "[%s]'s read=%ld\n", tpath, rs);
	nc_close(f, U_TRUE);
	nc_destroy_volume_context(volume);
	nc_shutdown();

}
