#include <netcache_config.h>
#include <stdio.h>
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
#include "bt_timer.h"
#include "ncapi.h"
//#include <duma.h>
#define	BLOCK_SIZE		(4096*64)


mode_t 			gmode = O_RDONLY;
int __threads 	= 1;
int 			singlefile_mode = 0;
int 			verify = 0;
int 			__term = 0;
int 			ocmode = 0;
int 			burst_mode = 0;
int 			wmsmode = 0;
int 			wmamode = 0;
int 			loopcnt = 1000;
extern void 	*__timer_wheel_base; 
unsigned long 		IOSUM = 0;

char			cache_path[] = "/stg/cache";
char			log_file[512] = "./vt.log";
int 			__inode_count  = 1024;;
void my_exit();





















static void
print_line(char *buf)
{
	TRACE((T_ERROR, "%s\n", buf));
}
int
main(int argc, char *argv[])
{
	
	char 				driver_class[128] = "HTTPN";
	char				zsig[128];
	char 				*driver_path = "/usr/lib64/libhttpn_driver.so";
	//int 			ncval;
	int				i;//,j;
	//pthread_attr_t pattr;
	//int 				r;
	//nc_stat_t	s;
	nc_mount_context_t		*mnt[1000];
	nc_uint32_t			logmask;
	//int					ii, L, rnd;
	//int					ra = 0;
	//nc_ssize_t 			l;
	//int 				val;
	long 				cache_mem_size = 512; /* MB */
	nc_origin_info_t 		oi = {
		.address = "127.0.0.1",
		.prefix = ""
	};



	atexit(my_exit);

	fprintf(stderr, "Cache directory set to '%s'\n", cache_path);
	fprintf(stderr, "Log file set to '%s'\n", log_file);
	fprintf(stderr, "Cache size set to '%ld' MB\n", cache_mem_size);
	fprintf(stderr, "SIZEOF(bt_timer_t) = %d\n", sizeof(bt_timer_t));

/*
 * PREPARATION OF TRACE MODULE
 */
	logmask =     NC_WARN|NC_ERROR|NC_INFO;
 	nc_setup_log(logmask, log_file, 50000, 10);

	//logmask =   NC_INFO	 | NC_ERROR ;
	//nc_change_mask(logmask);
	
	//logmask =   NC_WARN| NC_INFO	 | NC_ERROR | T_DEBUG | T_CACHE;
	//nc_change_mask(logmask);
	//exit(1);

	nc_set_param(NC_GLOBAL_CACHE_PATH, (void *)cache_path, strlen(cache_path));
	nc_set_param(NC_GLOBAL_CACHE_MEM_SIZE, (void *)&cache_mem_size, sizeof(cache_mem_size));
	nc_set_param(NC_GLOBAL_CACHE_MAX_INODE, (void *)&__inode_count, sizeof(__inode_count));


	nc_init();

	if (nc_load_driver(driver_class, driver_path) < 0) {
		fprintf(stderr, "error in load driver, '%s'\n", driver_path);
		exit(1);
	}


	for (i = 0; i < 100; i++) {
		sprintf(zsig, "VOL[%d]", i);
		mnt[i] = nc_create_mount_context(driver_class, zsig, &oi, 1);
		if (!mnt[i]) {
			fprintf(stderr, "%d-th volume[%s]:error in create\n", i, zsig);
			exit(1);
		}
	}
	for (i = 0; i < 100; i++) {
		nc_destroy_mount_context(mnt[i]);
	}
	TRACE((T_WARN, "********************************************************************\n"));
	TRACE((T_WARN, "****************** DUMP(AFTER VOLUME DESTROY) **********************\n"));
	TRACE((T_WARN, "********************************************************************\n"));

	nc_shutdown();
	TRACE((T_WARN, "********************************************************************\n"));
	TRACE((T_WARN, "****************** DUMP(AFTER SHUTDOWN) ****************************\n"));
	TRACE((T_WARN, "********************************************************************\n"));

#ifdef NC_HEAPTRACK
	nc_raw_dump_report(print_line);
#endif

}
void my_exit()
{
	__term = 1;
	//nc_shutdown();
	//odumpused();
}
