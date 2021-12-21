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
#include "ncapi.h"
//#include <duma.h>
#define	BLOCK_SIZE		(4096*64)
#ifdef WIN32
#define		TRAP	DebugBreak()
#else
#define		TRAP	abort()
#endif

mode_t 			gmode = O_RDONLY;
mode_t 			sleepmode = 0;
int __threads 	= 1;
int 			singlefile_mode = 0;
int 			verify = 0;
int 			forceclose = 0; /* force close mode */
int 			__term = 0;
int 			__c_ttl = 0;
int 			coldcaching = 0;
int 			ocmode = 0;
int 			burst_mode = 0;
int 			wmsmode = 0;
int 			statmode = 0;
int 			aiomode = 0;
int 			plmode = 0;
int 			wmamode = 0;
int 			loopcnt = 1000;
extern void 	*__timer_wheel_base; 
unsigned long 		IOSUM = 0;

int 			mnt_to_create = 15;
char			cache_path[] = "I:\\cache";
char			log_file[512] = "g:\\logs\\nc.log";
char			monitor_file[512] = "";
FILE *			monitor_pf = NULL;
#define 		MAX_FILE_COUNT 	30000
//#define 		MAX_FILE_COUNT 	5000
int __possible_files_count  = MAX_FILE_COUNT;
int __inode_count  = MAX_FILE_COUNT;
char 			**__files = NULL; //[MAX_FILE_COUNT];
char 			**__subdirs = NULL; //[MAX_FILE_COUNT];
int				__files_count = 0;
int				__dir_count = 0;
int				__dir_index = 0;
char 			xfiles[1000][128];
int 			__xfile_cnt = 0;
//char 			root[128]="/11945/VIDEO";
//char 			root[128]="/11957/test";
//char 			root[128]="/11385/SONG/WMA/080/673";
//char 			root[128]="/11385/SONG/WMA/080";
//char 			root[128]="/NetCache_WIN/1G";
//char 			root[128]="/NetCache_WIN/1K";
//char 			root[128]="/11957/test1"; /* BD-03*/
//char 			root[128]="/dav"; /* BD-03*/
//char 			root[128]="/dav/test/oracle/ora92";
//char 			root[128]="/dav/2801";
//char 			root[128]="/dav/4216/NetCache";
//char 			root[128]="/dav/2802";
//char 			root[128]="/dav/3274";
//char 			root[128]="/dav/4216";
//char 			root[128]="/dav/5102";
char 			root[128]="/"; /* BD-03*/
void * run_disk_io_burst_aio(void *d);
void * run_disk_io_1G(void *d);
void * run_disk_io_oc(void *d);
void * run_disk_io_wms(void *d);
static pthread_mutex_t 	s__io_lock;
void * run_disk_io_verify(void *d);
void * run_disk_io_wma(void *d);
void * run_stat_stress(void *d);
void * run_path_lock_stress(void *d);
void my_output(FILE *iof, char *fmt, ...);
void my_exit()
{
	__term = 1;
	//nc_shutdown();
	//odumpused();
}
void my_exit2(int a)
{
	fprintf(stderr, "*****************************8SIGNAL %d ----------signaled\n", a);
	__term = 1;
	//nc_shutdown();
	//odumpused();
}
int
is_in_xfile(char *path)
{
	int 	r = 0;
	int 	i;

	for (i = 0; i < __xfile_cnt;i++) {
		
		r = (strcasestr(path, xfiles[i])  != NULL);
		//my_output(stderr, "XFILE[%s], path(%s) = %d\n", xfiles[i], path, r);
		if (r == 1) 
			return 1;
	}
	return 0;
}
int 
fill_dir_proc(nc_volume_context_t *mc, void *buf, const char *name, const nc_stat_t *statp, nc_off_t off)
{
	char 		*cbuf= (char *)buf;
	nc_stat_t stbuf;
	size_t newlen;
	char 	nbuf[256];


	if (statp) {
		stbuf = *statp;
	}
	else {
		memset(&stbuf, 0,sizeof(stbuf));
	}

	if (__dir_count < max(__possible_files_count,10000) && __files_count < __possible_files_count) {
		if (!S_ISREG(statp->st_mode) ) {
			if ( name[0] != '\0' && name[0] != '.') {

				if (cbuf[strlen(cbuf)-1] == '/')
					snprintf(nbuf, sizeof(nbuf), "%s%s", (char *)buf, name);
				else
					snprintf(nbuf, sizeof(nbuf), "%s/%s", (char *)buf, name);
				if (is_in_xfile(nbuf)) {
					my_output(stderr, "file, '%s' - excluded\n", nbuf);
					//my_output(stderr, "file, '%s' - excluded, hit any key\n", nbuf);
					//getchar();
					return 0;
				}


				__subdirs[__dir_count++] = strdup(nbuf);
				//fprintf(stderr, "DIR: (%s) - found name(%s)\n", nbuf, name);
			}
		}
		else {
			char	*t = name;
			while (*t) {
				if (!isascii(*t)) return 0;
				t++;
			}
			if (statp->st_size > 1000000) {
				if (cbuf[strlen(cbuf)-1] == '/')
					snprintf(nbuf, sizeof(nbuf), "%s%s", (char *)buf, name);
				else
					snprintf(nbuf, sizeof(nbuf), "%s/%s", (char *)buf, name);
				if (is_in_xfile(nbuf)) {
					my_output(stderr, "file, '%s' - excluded, hit any key\n", nbuf);
					getchar();
					return 0;
				}
				__files[__files_count++] = strdup(nbuf);

#undef st_mtime
				fprintf(stderr, "FILE: (%s) - mtime(%lld) found\n", nbuf, statp->st_mtime);
			}
		}
	}
	else {
		return -1; /* break */
	}


	return 0;
}
void
open_monitor_file()
{
	if(0 == monitor_file[0]) return;

	monitor_pf = fopen(monitor_file, "a+");
	if(NULL == monitor_pf)
	{
		fprintf(stderr, "Failed to open monitor file %s\n", monitor_file);
		exit(1);
	}
}

void
close_monitor_file()
{
	if(NULL == monitor_pf) return;
	fclose(monitor_pf);
	monitor_pf = NULL;
}

void
write_monitor_file(void * cb, const char * method, const char * origin, const char * request, const char * reply, double elapsed, double sentb, double receivedb)
{
	FILE * pf = cb;
	time_t t;
	struct tm * tm;
	long sent = (long)sentb;
	long received = (long)receivedb;

	if(pf != monitor_pf)
	{
		fprintf(stderr, "Abnormal FILE POINTER %p(%p) of %s\n", pf, monitor_pf, monitor_file);
		exit(1);
	}
	t = time(NULL);
	tm = localtime(&t);
	fprintf(pf, "%04d%02d%02d%02d%02d%02d %-7s %s %s %lf %ld %ld %s\n"
			, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday
			, tm->tm_hour, tm->tm_min, tm->tm_sec
			, method, origin
			, reply, elapsed, sent, received
			, request);

	fflush(pf);
}

#define	DRIVER_CLASS	"ics_webdav 0.9"

char 			tfile[100000][256];
void * run_disk_io(void *d);
void * run_disk_io_burst(void *d);
char 			purge[256]="";
int 			purge_needed = 0;
nc_volume_context_t	*mnt = NULL;
nc_volume_context_t	*mnt_m[128];
int 			mnt_cnt = 0;


char 			cluster_ip[128]="";
char 			local_ip[128]="";
int 			cluster_ttl=1;
#define	TFILE_CNT 1000
#define	TFILE_1G_CNT 5
main(int argc, char *argv[])
{
	int 			zz;
	int 			valx;
	int 			pc = 0;
	char 			*tok;
	char 				v_sig[128] = "";
	//char 				encoding[]="euc-kr";
	//char 				encoding[]="utf-8";
	char 				encoding[]="CP949";
	//char 				encoding[]="euc-jp";
	char 				driver_class[128] = "WebDAV";
	char plugin_path[256] = "d:/works/netcache-1.0.3.work/lib";
#ifdef WIN32
	//char 			*driver_name_webdav = "libwebdav_driver-2.dll";
	char 			*driver_name_webdav = "msys-webdav_driver-2.dll";
	char 			*driver_name_swift = "libswift_driver-1.dll";
	char 			*driver_name_loopback = "libloopback_driver-2.dll";
#else
	//char 			*driver_name_webdav = "/root/libnetcache/2.0.0/lib/libwebdav_driver.so";
	char 			*driver_name_webdav = "/root/libnetcache/2.8.0/lib/libwebdav_driver.so";
	char 			*driver_name_swift = "/root/libnetcache/2.8.0/lib/libswift_driver-1.dll";
	char 			*driver_name_loopback = "/root/libnetcache/2.8.0/lib/libloopback_driver.so";
#endif
	char 	*opt_dup = NULL;
	mode_t			mode;
	char			newfile[256];
	int 			ncval;
	int 			ccf = 1;
	int				i,j;
	pthread_attr_t pattr;
	size_t stksiz;
	pthread_t 			tid[100];
	int 				zerob = 0;
	int 				T;
	int 				r;
	nc_stat_t	s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				buf[8192];
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	char			cache_path[10][128]; 
	int				cache_path_count = 0;
	long 			cache_mem = 512; /* MB */
	long 			inode_max = MAX_FILE_COUNT; /* MB 1500-150 = 1350*/
	LOG_HANDLE_t 		*LH = NULL;
	char				*stkaddr;
	//char 			tfile[256] = "/2802/ContentDistributor.rar";
	//char 			tfile[256] = "/2801/OS\SW_DVD5_Win_Pro_K_7_32BIT_Korean_Full_MLF_X15-72829.ISO";
	//cfs_origin_driver_t 	*test;
	int 				ctx_cnt = 0;
	nc_origin_info_t 	*ctx_to_use = NULL;
	char 				*driver_path = NULL;
	long 				loopcount = 0;




	nc_origin_info_t 	ctx_webdav[1] = {
#if 1
		{
        		.address = "http://nctest.ktsh.co.kr/dav",

			.prefix = "",
			.user = "nctest@nctest",
			.pass = "5952a1baec35b2b42ed4bd2efe8e447b",
			.encoding = "utf-8",
			.ioflag = NCOF_READABLE
		}
#else
		{
        		.address = "http://nctest2.ktsh.co.kr/dav",

			.prefix = "",
			.user = "nctest@nctest",
			.pass = "5952a1baec35b2b42ed4bd2efe8e447b",
			.encoding = "utf-8",
			.ioflag = NCOF_READABLE
		}
#endif
	};
	nc_origin_info_t 	ctx_swift[2] = {
		{
			"https://ssstg.ucloudbiz.olleh.com/auth/v1.0",
			"",
  			"solbox:solboxuser",
    		"solboxpass",
			"utf-8",
			1
		},
		{
			"https://ssstg.ucloudbiz.olleh.com/auth/v1.0",
			"",
  			"solbox:solboxuser",
    		"solboxpass",
			"utf-8",
			1

		}
	};
	nc_origin_info_t 	ctx_loopback[2] = {
		{
#if WIN32
			//"z:/KT ucloud storage",
			"C:/Windows",
#else
			"/usr/include",
#endif
			"",
			"sbfstest001@sbfstest",
			"07d6dab5c825f248e8f7d1f68ba0f1d6",
			"utf-8"
		},
	};


	//atexit(my_exit);
#ifdef WIN32
	//HANDLE htrace;
	//htrace = LoadLibraryA("backtrace.dll");
#endif



	//signal(SIGABRT, my_exit2);
	//signal(SIGTERM, my_exit2);
	//block_signal();
	//on_exit(my_exit2, NULL);
	fprintf(stderr, "started\n");
	while ((val = getopt(argc, argv, "AC:G:I:L:M:NOP:T:U:X:abcdi:l:m:op:st:z:vw1FW")) > 0) {
		switch (val) {
			case 'z':
				mnt_to_create = atoi(optarg);
				break;
			case 'W':
				sleepmode = 1;
				break;
			case 'F':
				forceclose = 1;
				break;
			case 'A':
				aiomode = 1;
				break;
			case 'd':
				coldcaching = 1;
				break;
			case 'U': /* cluster:local:ttl */
				opt_dup = strdup(optarg);
				fprintf(stderr, "************** cluster_info : %s\n", opt_dup);
				tok = strtok(opt_dup, ":");
				strcpy(cluster_ip, tok);
				tok = strtok(NULL, ":");
				strcpy(local_ip, tok);
				tok = strtok(NULL, ":");
				cluster_ttl = atoi(tok);
				break;
			case 'X':
				strcpy(xfiles[__xfile_cnt++], optarg);
				break;
			case 'G':
				strcpy(driver_class, optarg);
				if ((strcasecmp(driver_class, "loopback") != 0) &&
					(strcasecmp(driver_class, "Swift") != 0) &&
					(strcasecmp(driver_class, "webdav") != 0)) {
					fprintf(stderr, "driver class should be either 'WebDAV'/'Swift'/'loopback'\n");
					exit(1);
				}
				if (strcasecmp(driver_class, "loopback") == 0) {
					ctx_to_use = ctx_loopback;
					driver_path = driver_name_loopback;
					ctx_cnt = 1;
				}
				else if (strcasecmp(driver_class, "Swift") == 0) {
					ctx_to_use = ctx_swift;
					driver_path = driver_name_swift;
					ctx_cnt = 1;
				}
				else {
					driver_path = driver_name_webdav;
					ctx_to_use = ctx_webdav;
					ctx_cnt = sizeof(ctx_webdav)/sizeof(nc_origin_info_t);
				}
				break;
			case 'c':
				ccf = !ccf;
				break;
			case 'b': /* burst io mode */
				burst_mode=1;
				break;
			case 'P': /* plugin path */
				strcpy(plugin_path, optarg);
				break;
			case 'p':
				if (optarg) {
					strcpy(purge, optarg);
				}
				else {
					purge[0] = 0;
				}
				purge_needed++;
				break;
			case 'i':
				__inode_count  = atoi(optarg);
				break;
			case 'I':
				__possible_files_count  = atoi(optarg);
				break;
			case 'o':
				ocmode = 1;
				break;
			case 'v':
				//snprintf(root, sizeof(root), "/11499/TW");
				verify = 1;
				fprintf(stderr, "root dir = [%s]\n", root);
				break;
			case 'L':
				loopcnt =atoi(optarg);
				break;
			case 'w':
				wmsmode = 1;
				break;
			case 'O':
				plmode = 1;
				break;
			case 's':
				statmode = 1;
				break;
			case 'a':
				wmamode = 1;
				break;
			case '1':
				singlefile_mode = 1;
				break;
			case 'M':
				cache_mem = atoi(optarg);
				break;
			case 't':
				__threads = atoi(optarg);
				break;
			case 'T':
				__c_ttl = atoi(optarg);
				break;
			case 'C':
				strcpy(cache_path[cache_path_count++], optarg);
				break;
			case 'N':
				cache_path[0][0] = '\0';

				break;
			case 'l':
				strcpy(log_file, optarg);
				break;
			case 'm':
				strncpy(monitor_file, optarg, 512);
				break;
			default:
				fprintf(stderr, "unknown arg\n");
				exit(1);
				break;
		}
	}

	if (cache_path_count <= 0) {
		fprintf(stderr, "no cache path defined\n");
		exit(1);
	}
	__files = (char **)malloc(sizeof(char *) *__possible_files_count);
	__subdirs = (char **)malloc(sizeof(char *) * max(__possible_files_count, 10000)) ;
	for (i = 0; i < __possible_files_count; i++) {
		__files[i] = 0;
		__subdirs[i] = 0;
	}
	__files_count = 0;

	if (opt_dup)
		my_output(stderr, " cluster info is '%s' \n", opt_dup);


	if (pthread_mutex_init(&s__io_lock, NULL)) {
		fprintf(stderr, "mutex init failed\n");
		exit(1);
	}

	open_monitor_file();

	my_output(stderr, "Cache directory set to '%s' count=%d\n", cache_path[0]);
	my_output(stderr, "Log file set to '%s'\n", log_file);
	my_output(stderr, "Cache size set to '%d' MB\n", cache_mem);

/*
 * PREPARATION OF TRACE MODULE
 */
	//logmask = T_DEBUG	 | T_TRACE	 | T_INFO	 | T_WARN	 | T_ERROR | T_RA	 | T_INODE | T_BLOCK | T_DAV  ;
	//logmask = T_DEBUG	 | T_TRACE	 | T_INFO	 | T_WARN	 | T_ERROR | T_RA	 | T_INODE | T_BLOCK ;
	//logmask = NC_INFO	 | NC_WARN	 | NC_ERROR | NC_RA	 | NC_INODE | NC_BLOCK | NC_DAV  ;
	//logmask = NC_INFO	 | NC_WARN	 | NC_ERROR | NC_RA	 | NC_INODE | NC_DAV  | T_BLOCK;
	//logmask = NC_INFO	 | NC_ERROR | NC_RA	; 
	//logmask = NC_INODE | NC_INFO	 |NC_WARN | NC_ERROR | NC_MEM; 
	//logmask = NC_INODE |NC_INFO	 |NC_WARN | NC_ERROR ;
	//logmask =  NC_PERF | NC_INFO	 |NC_WARN | NC_ERROR ;
	//logmask =  NC_PERF|NC_INFO|NC_WARN|NC_ERROR;
	//logmask =   T_BLOCK	 | T_INFO	 | T_WARN	 | T_ERROR | T_RA	 | T_INODE ;
	//logmask =   T_BLOCK|T_INFO	 | T_WARN	 | T_ERROR | T_RA	 | T_INODE ;
	//logmask =   T_INFO	 | T_WARN	 | T_ERROR | T_RA	 | T_INODE ;

	//logmask =   NC_DAV|NC_INFO	 | NC_BLOCK |NC_WARN	 | NC_ERROR |NC_INODE | NC_DEBUG | NC_TRACE;
	//logmask =     NC_INFO	 | NC_WARN	 | NC_ERROR ;
	//logmask =     NC_ASIO|NC_WARN|NC_INFO	| NC_ERROR; 
	//logmask =     NC_WARN|NC_INFO	| NC_ERROR; 
	logmask =     NC_WARN|NC_ERROR|NC_INFO|T_STAT;
 	nc_setup_log(logmask, log_file, 50000, 10);

	//logmask =   NC_INFO	 | NC_ERROR ;
	//nc_change_mask(logmask);
	
	//logmask =   NC_WARN| NC_INFO	 | NC_ERROR | T_DEBUG | T_CACHE;
	//nc_change_mask(logmask);
	//exit(1);

	if (coldcaching) {
		nc_set_param(NC_GLOBAL_ENABLE_COLD_CACHING, &coldcaching, sizeof(int));
	}
	zz=120;
	//nc_set_param(NC_GLOBAL_BLOCK_SPIN_INTERVAL, (void *)&zz, sizeof(zz));
	nc_set_param(NC_GLOBAL_CLUSTER_IP, (void *)cluster_ip, strlen(cluster_ip));
	nc_set_param(NC_GLOBAL_LOCAL_IP, (void *)local_ip, strlen(local_ip));
	nc_set_param(NC_GLOBAL_CLUSTER_TTL, (void *)&cluster_ttl, sizeof(cluster_ttl));
	//cache_path[0] = 0;
	nc_set_param(NC_GLOBAL_CACHE_PATH, (void *)cache_path[0], strlen(cache_path));
	//nc_set_param(NC_GLOBAL_CACHE_PATH, (void *)"", 0);
	nc_set_param(NC_GLOBAL_CACHE_MEM_SIZE, (void *)&cache_mem, sizeof(cache_mem));
	nc_set_param(NC_GLOBAL_CACHE_MAX_INODE, (void *)&__inode_count, sizeof(__inode_count));
	valx = 70;
	nc_set_param(NC_GLOBAL_MAX_PATHLOCK, (void *)&valx, sizeof(valx));
	valx = 70;
	nc_set_param(NC_GLOBAL_MAX_STATCACHE, (void *)&valx, sizeof(valx));
	valx = 256;
	nc_set_param(NC_GLOBAL_CACHE_BLOCK_SIZE, (void *)&valx, sizeof(valx));
	valx = 128;
	nc_set_param(NC_GLOBAL_ENABLE_FASTCRC, (void *)&valx, sizeof(valx));

	//ncval=128;
	//nc_set_param(NC_GLOBAL_CACHE_MAX_ASIO, (void *)&ncval, sizeof(ncval));
	ncval=4;
	nc_set_param(NC_GLOBAL_CACHE_DISK_RA, (void *)&ncval, sizeof(ncval));
	ncval=8;
	nc_set_param(NC_GLOBAL_CACHE_NETWORK_RA, (void *)&ncval, sizeof(ncval));

	ncval=8;
	nc_set_param(NC_GLOBAL_CACHE_RA_MINTHREADS, (void *)&ncval, sizeof(ncval));
	ncval=128;
	nc_set_param(NC_GLOBAL_CACHE_RA_MAXTHREADS, (void *)&ncval, sizeof(ncval));

	ncval=60;
	nc_set_param(NC_GLOBAL_CACHE_POSITIVE_TTL, (void *)&ncval, sizeof(ncval));
	ncval=300;
	nc_set_param(NC_GLOBAL_CACHE_NEGATIVE_TTL, (void *)&ncval, sizeof(ncval));

	ncval=80; /* percent */
	nc_set_param(NC_GLOBAL_CACHE_DISK_LOW_WM, (void *)&ncval, sizeof(ncval));
	ncval=90; /* percent */
	nc_set_param(NC_GLOBAL_CACHE_DISK_HIGH_WM, (void *)&ncval, sizeof(ncval));
	nc_set_param(NC_GLOBAL_ENABLE_COMPACTION, (void *)&ccf, sizeof(ccf));
	fprintf(stderr, "enable_forceclose=%d\n", forceclose);
	nc_set_param(NC_GLOBAL_ENABLE_FORCECLOSE, (void *)&forceclose, sizeof(forceclose));


	//bt_msleep(5000);
	nc_init();
	//bt_msleep(5000);
	for (i = 1 ; i < cache_path_count; i++) {
		nc_add_partition(cache_path[i], 1);
	}

	if (nc_load_driver(driver_class, driver_path) < 0) {
		fprintf(stderr, "error in load driver, '%s'\n", driver_path);
		exit(1);
	}
	strcpy(v_sig, "//vol_default/");
	mnt = nc_create_volume_context(driver_class, v_sig, ctx_to_use, ctx_cnt);
	if (!mnt) {
		fprintf(stderr, "error in create\n");
	}
	mnt_m[mnt_cnt++] = mnt;
	nc_ioctl(mnt_m[0], NC_IOCTL_STORAGE_CHARSET, encoding, strlen(encoding));
	if (__c_ttl > 0) {
		nc_ioctl(mnt_m[0], NC_IOCTL_POSITIVE_TTL, &__c_ttl, sizeof(__c_ttl));
	}

	if(monitor_file[0])
	{
			nc_ioctl(mnt_m[0], NC_IOCTL_ORIGIN_MONITOR, write_monitor_file, sizeof(void *));
			nc_ioctl(mnt_m[0], NC_IOCTL_ORIGIN_MONITOR_CBD, monitor_pf, sizeof(FILE *));
	}

	for (i = mnt_cnt; i < mnt_to_create; i++) {
		sprintf(v_sig, "//vol_#%d/", i);
		mnt_m[mnt_cnt] 	 = nc_create_volume_context(driver_class, v_sig, ctx_to_use, ctx_cnt);
		pc = 0;
		nc_ioctl(mnt_m[mnt_cnt], NC_IOCTL_PRESERVE_CASE, &pc, sizeof(pc));
		nc_ioctl(mnt_m[mnt_cnt], NC_IOCTL_STORAGE_CHARSET, encoding, strlen(encoding));
		if (__c_ttl > 0) {
			nc_ioctl(mnt_m[mnt_cnt], NC_IOCTL_POSITIVE_TTL, &__c_ttl, sizeof(__c_ttl));
		}
		if(monitor_file[0])
		{
				nc_ioctl(mnt_m[mnt_cnt], NC_IOCTL_ORIGIN_MONITOR, write_monitor_file, sizeof(void *));
				nc_ioctl(mnt_m[mnt_cnt], NC_IOCTL_ORIGIN_MONITOR_CBD, monitor_pf, sizeof(FILE *));
		}
		mnt_cnt++;
	}
	nc_verify_storage(cache_path[0]);
	if (purge_needed) {
#if 0
		for (zz = 1; zz <= 5; zz++) {
			i = nc_purge(mnt_m[zz], (purge[0]?purge:"^/*"), U_TRUE, U_FALSE);
			if (i < 0) {
				do {
#ifndef WIN32
					sleep(1);
#else
					Sleep(1000);
#endif
					i = nc_purge(mnt, (purge[0]?purge:NULL), U_TRUE, U_FALSE);
				} while (i < 0);
			}
		}
#else
		i = nc_purge(mnt, "/*", U_TRUE, U_FALSE);
#endif

		fprintf(stderr, "%d entries purged\n", i);
#ifdef WIN32
		while (1) Sleep(1000);
#else
		while (1) sleep(1);
#endif
	}
#if 0
	snprintf(newfile, sizeof(newfile), "%s/%s", root, "BABO");
	mode = S_IFREG;
	if (nc_mknod(mnt, newfile, mode, 0) < 0) {
		my_output(stderr, "%s - create error\n", newfile);
	}
#endif
#if 1
	my_output(stderr, "SCANNING DIR:(%s)\n", root);
	//nc_readdir(mnt, "/NetCache_WIN/1G", fill_dir_proc);
	nc_readdir(mnt, root, fill_dir_proc, root);
	for (i = 0; i < __dir_count && __files_count < __possible_files_count; i++) {
		//my_output(stderr, "SCANNING SUB DIR:(%s)\n", __subdirs[i]);
		nc_readdir(mnt, __subdirs[i], fill_dir_proc, __subdirs[i]);
	}
	my_output(stderr, "%d files prepared\n", __files_count);
	getchar();

	ncval = 0;
	nc_ioctl(mnt, NC_IOCTL_PRESERVE_CASE, &ncval, sizeof(int));
//	getchar();
	nc_getattr(mnt, "/*", &s);

	//fprintf(stderr, "------------> size: %lld\n", s.st_size);
	//exit(1);
	for  (ii =  0; ii < TFILE_CNT; ii++) {
		sprintf(tfile[ii], "/NetCache_WIN/100M/%06d", ii+1);
	}
	//chdir("/stg/core");
#define N_THREADS	5
	pthread_attr_init(&pattr);
	pthread_attr_getstacksize(&pattr, &stksiz);
		stksiz = 5000000;
	pthread_attr_setstacksize(&pattr, stksiz);
	my_output(stderr, "** creating %d IO threads\n", __threads);
	for (T = 0; T < __threads; T++) {
		if (wmsmode) {
			pthread_create(&tid[T], &pattr, run_disk_io_wms, (void *)T);
		}
		else if (burst_mode) {
			if (aiomode) 
				pthread_create(&tid[T], &pattr, run_disk_io_burst_aio, (void *)T);
			else
				pthread_create(&tid[T], &pattr, run_disk_io_burst, (void *)T);
		}
		else if (ocmode) {
			pthread_create(&tid[T], &pattr, run_disk_io_oc, (void *)T);
		}
		else if (wmamode) {
			pthread_create(&tid[T], &pattr, run_disk_io_wma, (void *)T);
		}
		else if (statmode) {
			pthread_create(&tid[T], &pattr, run_stat_stress, (void *)T);
		}
		else if (plmode) {
			pthread_create(&tid[T], &pattr, run_path_lock_stress, (void *)T);
		}
		else if (singlefile_mode) {
			pthread_create(&tid[T], &pattr, run_disk_io_1G, (void *)T);
		}
		else if (verify) {
			pthread_create(&tid[T], &pattr, run_disk_io_verify, (void *)T);
		}
		else {
			pthread_create(&tid[T], &pattr, run_disk_io, (void *)T);
		}
	}
#ifdef TEST_SHUTDOWN
	Sleep(20000);
	my_output(stderr, "** %02d **  calling SHUTDOWN...\n");
	nc_destroy_volume_context(mnt);
	nc_shutdown();
	__term = 1;
#endif

	IOSUM = 1;
	//block_signal();
	my_output(stderr, "%d threads created\n", __threads);
	while (__threads > 0) {
		unsigned long 	Bytes = __sync_fetch_and_sub(&IOSUM, IOSUM);
		my_output(stderr, "IO SPEED = %.2f MB/sec (%d threads)\n", Bytes/5000000.0, __threads);
		fflush(stderr);
		if (Bytes == 0) {
			zerob++;
		}
		else {
			zerob = 0;
		}
#ifdef WIN32
		Sleep(5000);
#else
		bt_msleep(5000);
#endif
		if (loopcount && (loopcount % 6 == 0)) {
			;
			//mnt_purge_volume(mnt);
			//pm_purge(mnt, "*.mp4", U_TRUE, U_FALSE);
		}
		loopcount++;

	}
	my_output(stderr, "all workers finished, shutdown started...\n");
	for (i = 0; i < mnt_cnt; i++) {
		nc_destroy_volume_context(mnt_m[i]);
	}
	nc_shutdown();
	my_output(stderr, "FINISHED.....................\n");
#ifdef __MEMORY_DEBUG
	//odumpused();
#endif

#ifdef TEST_SHUTDOWN 
	my_output(stderr, "** %02d **  calling SHUTDOWN...DONE\n");
#endif

#endif
	//odumpused();

	close_monitor_file();
}

void *
run_disk_io(void *d)
{
	int 				ID = (int)d;
	int					sz;
	int 				R;
	int 				r;
	nc_stat_t			s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				buf[1024*1024];
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 150; /* MB */
	char 			testfile[256];



#ifdef WIN32
	my_output(stderr, "----- run_disk_io_burst_1 : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_burst_1 : %ld\n", pthread_self());
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii < loopcnt; ii++) {
		//snprintf(testfile, sizeof(testfile), "/11145/video/v%d.wmv", (rand() % TFILE_CNT)+1);

		R = rand() % __files_count;
		snprintf(testfile, sizeof(testfile), __files[R]);

		ra = rand() % 5;
		if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			bt_msleep(1000);
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error = %d)\n", ID, ii, testfile, r);
		}
		else {
#undef st_mtime
#undef st_ctime
			my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", 
						ID, 
						ii, 
						testfile, 
						s.st_size, 
						s.st_ctime, 
						s.st_mtime, 
						r);
		}
		if (ra) { /* 90% */
	
	
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				bt_msleep(5000);
				my_output(stderr, "** %02d ** S[%02d] OPEN - %s : failed(error= %d)\n", ID, ii, testfile,r);
				continue;
			}
			remained = s.st_size;
			my_output(stderr, "** %02d ** S[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			off 	 = 0;
			sz = 1024;
			while (!__term && remained > 0) {
				l = nc_read(F, buf, off, sz);
				if (l <= 0) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read (OFF[%lld], %d): %d returned\n", ID, off, sizeof(buf), l);
					break;
				}
				off += l;
				remained -= l;
				__sync_fetch_and_add(&IOSUM, l);
				sz = ((rand() % 1024) + 1)*1024;
			}
			if (remained != 0) {
				my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!** %02d ** S[%02d] SEQ.READ - %s : failed, remained[%lld]\n", ID, ii, testfile, remained);
			}
			else {
				my_output(stderr, "** %02d ** S[%02d] SEQ.READ - %s : ok, remained[%lld]\n", ID, ii, testfile, (long long)remained);
			}
	#if 0
			my_output(stderr, "++++++++++++++++++++++++++++++DATA+++++++++++++++++++\n\n"
					"%s"
					"\n\n++++++++++++++++++++++++++++++++++++++++++++++++++++++",
					buf);
	#endif
			nc_close(F);
		} /* sequencial */

		else {
			/*
			 *********************** RANDOM IO ****************
			 */
			my_output(stderr, "***   RANDOM IO TEST\n");
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** R[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			my_output(stderr, "** %02d ** R[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			for (rnd = 0; rnd < 500; rnd++) {
				off = (nc_size_t)rand() % (s.st_size-1000);
				off = max(0, off);
				//off = (off/BLOCK_SIZE)*BLOCK_SIZE;
				//remained = min(s.st_size, sizeof(buf));
				remained = 64*1024L;
				remained = min((s.st_size - off), remained);
				l = nc_read(F, buf, off, remained);
				if (l != remained) {
					my_output(stderr, "run_disk_io:: (%s) ERROR!! -> wrong read (off=%lld, size=%lld) file size=%lld => %ld\n", 
									testfile,
									(long long)off,
									(long long)remained,
									(long long)s.st_size,
									(long)l
									);
					//exit(1);
					bt_msleep(1000);
					break;
				}
				__sync_fetch_and_add(&IOSUM, l);
			}
			my_output(stderr, "** %02d ** R[%02d] RAND.READ - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			nc_close(F);
			my_output(stderr, "** %02d ** R[%02d] IO - %s : all done ***  \n\n\n", ID, ii, testfile);
		}
	//}

	}
	__threads--;
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****(%d)!\n", ID, __threads);

}
void *
run_disk_io_burst(void *d)
{
			struct tm ti;
			time_t 	  now;
	int 				ID = (int)d;
	int 				midx = 0, current = 0;
	int 				iocount;
	int					sz;
	int 				R;
	int 				r;
	long long 			rsize;
	long 				rd = 0;
	nc_stat_t			s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				buf[1024*1024];
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 150; /* MB */
	char 			testfile[8192];
	nc_volume_context_t 	*mnt = NULL;



#ifdef WIN32
	my_output(stderr, "----- run_disk_io_burst_2 : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_burst_2 : %ld\n", pthread_self());
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii < loopcnt; ii++) {
		//snprintf(testfile, sizeof(testfile), "/11145/video/v%d.wmv", (rand() % TFILE_CNT)+1);

		//R = ii % __files_count;
		R = rand() % __files_count;
		if ((R % 1000) == 0) {
			snprintf(testfile, sizeof(testfile), "%s.NOTFOUND", __files[R]);
		}
		else {
			snprintf(testfile, sizeof(testfile), __files[R]);
		}

		midx = rand() % (mnt_cnt+1);
		now = time(NULL);
		localtime_r(&now, &ti);
		current = ti.tm_hour % mnt_cnt;
		if (midx == mnt_cnt) {
			midx = current;
		}

		mnt = mnt_m[midx];

		ra = rand() % 5;
		if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			bt_msleep(1000);
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile,r );
			continue;
		}
		else {
#undef st_mtime
#undef st_ctime
#if 0
			my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", 
						ID, 
						ii, 
						testfile, 
						s.st_size, 
						s.st_ctime, 
						s.st_mtime, 
						r);
#endif
		}
		//ra = 0;

		if (ra) { /* 90% */
	
	
			//F = nc_open(mnt, testfile, gmode);
			F = nc_open_extended(mnt, testfile, testfile, gmode, NULL);
			if (F == NULL) {
				my_output(stderr, "** %02d ** %02d:S[%02d] OPEN - %s : failed\n", ID, midx, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			remained = s.st_size;
			my_output(stderr, "** %02d ** %02d/%02d:S[%02d] OPEN - %s : ok, size[%lld]\n", ID, current, midx, ii, testfile, s.st_size);
			off 	 = 0;
			sz = 1024;
			iocount = 3;
			while (!__term && remained > 0 && iocount > 0) {
				l = nc_read(F, buf, off, sz);
				if (l <= 0) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read (OFF[%lld], %d): %d returned\n", ID, off, sizeof(buf), l);
					break;
				}
				usleep(10000);
				off += l;
				remained -= l;
				__sync_fetch_and_add(&IOSUM, l);
				sz = ((rand() % 1024) + 1)*1024;
				iocount--;
			}

			//if (remained != 0) {
			//	my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!** %02d ** S[%02d] SEQ.READ - %s : failed, remained[%lld]\n", ID, ii, testfile, remained);
			//}
			//else {
			//	my_output(stderr, "** %02d ** S[%02d] SEQ.READ - %s : ok, remained[%lld]\n", (int)ID, (int)ii, (char *)testfile, (long long)remained);
			//}
	#if 0
			my_output(stderr, "++++++++++++++++++++++++++++++DATA+++++++++++++++++++\n\n"
					"%s"
					"\n\n++++++++++++++++++++++++++++++++++++++++++++++++++++++",
					buf);
	#endif
			nc_close(F);
		} /* sequencial */

		else {
			/*
			 *********************** RANDOM IO ****************
			 */
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** %02d:R[%02d] OPEN - %s : failed\n", ID, midx, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			my_output(stderr, "** %02d ** %02d/%02d:R[%02d] OPEN - %s : ok, size[%lld]\n", (int)ID, current, midx, (int)ii, (char *)testfile, (long long)s.st_size);
			rd = 0;
			iocount = 5;
			for (; (iocount > 0); iocount--) {
				rsize = s.st_size-s.st_size/2;
				if (rsize > 0) {
					off = (nc_size_t)rand() % (nc_size_t)(s.st_size-s.st_size/2);
					off = max(0, off);
				}
				else {
					off = 0;
				}
				//my_output(stderr, "size[%lld], offset[%lld]\n", s.st_size, off);
				assert(off <= s.st_size);
				//off = (off/BLOCK_SIZE)*BLOCK_SIZE;
				//remained = min(s.st_size, sizeof(buf));
				remained = 64*1024L;
				remained = (long) min((s.st_size - off), (long long)remained);
				remained = max(0, remained);
				l = nc_read(F, buf, off, remained);
				if (l != remained) {
					my_output(stderr, "###################### run_disk_io:: (%s) ERROR!! -> wrong read (off=%lld, size=%lld) file size=%lld => %ld\n", 
									testfile,
									(long long)off,
									(long long)remained,
									(long long)s.st_size,
									(long)l
									);
					//exit(1);
					bt_msleep(1000);
					break;
				}
				__sync_fetch_and_add(&IOSUM, l);
				rd += l;
			}
			//my_output(stderr, "** %02d ** R[%02d] RAND.READ - %s : %ld bytes, ok, size[%lld](iocount=%d)\n", (int)ID, (int)ii, (char *)testfile, rd, (long long)s.st_size, iocount);
			nc_close(F);
			//my_output(stderr, "** %02d ** R[%02d] IO - %s : all done ***  \n\n\n", ID, ii, testfile);
		}
		if (sleepmode) {
			bt_msleep(1000);
		}
	}
	__threads--;
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****(%d)!\n", ID, __threads);

}
void *
run_disk_io_burst_aio(void *d)
{
	int 				ID = (int)d;
	int 				iocount;
	int					sz;
	int 				R;
	int 				r;
	long long 			rsize;
	long 				rd = 0;
	nc_stat_t			s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				buf[1024*1024];
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 150; /* MB */
	char 			testfile[8192];
	nc_volume_context_t 	*mnt = NULL;



#ifdef WIN32
	my_output(stderr, "----- run_disk_io_burst_aio : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_burst_aio: %ld\n", pthread_self());
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii < loopcnt; ii++) {
		//snprintf(testfile, sizeof(testfile), "/11145/video/v%d.wmv", (rand() % TFILE_CNT)+1);

		//R = ii % __files_count;
		R = rand() % __files_count;
		if ((R % 1000) == 0) {
			snprintf(testfile, sizeof(testfile), "%s.NOTFOUND", __files[R]);
		}
		else {
			snprintf(testfile, sizeof(testfile), __files[R]);
		}
		mnt = mnt_m[rand() % mnt_cnt];

		ra = rand() % 5;
		if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			bt_msleep(1000);
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile,r );
			continue;
		}
		else {
#undef st_mtime
#undef st_ctime
#if 0
			my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", 
						ID, 
						ii, 
						testfile, 
						s.st_size, 
						s.st_ctime, 
						s.st_mtime, 
						r);
#endif
		}
		//ra = 0;

		if (ra) { /* 90% */
	
	
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** S[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			remained = s.st_size;
			my_output(stderr, "** %02d ** S[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			off 	 = 0;
			sz = 1024;
			iocount = 3;
			while (!__term && remained > 0 && iocount > 0) {
#ifndef WIN32
				l = fio_async_read(F, buf, off, sz);
#else
				l = -1;
#endif
				if (l == 0) {
					my_output(stderr, "** %02d ** S:fio_async_read (OFF[%lld], %d): %d returned(EOF)\n", ID, off, sizeof(buf), l);
					break;
				}
				else if (l < 0) {
					my_output(stderr, "** %02d ** S:fio_async_read (OFF[%lld], %d): %d returned(WOULDBLOCK)\n", ID, off, sizeof(buf), l);
					usleep(100000);
				}
				else {
					off += l;
					remained -= l;
					__sync_fetch_and_add(&IOSUM, l);
					sz = ((rand() % 1024) + 1)*1024;
					iocount--;
				}
			}

			//if (remained != 0) {
			//	my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!** %02d ** S[%02d] SEQ.READ - %s : failed, remained[%lld]\n", ID, ii, testfile, remained);
			//}
			//else {
			//	my_output(stderr, "** %02d ** S[%02d] SEQ.READ - %s : ok, remained[%lld]\n", (int)ID, (int)ii, (char *)testfile, (long long)remained);
			//}
	#if 0
			my_output(stderr, "++++++++++++++++++++++++++++++DATA+++++++++++++++++++\n\n"
					"%s"
					"\n\n++++++++++++++++++++++++++++++++++++++++++++++++++++++",
					buf);
	#endif
			nc_close(F);
		} /* sequencial */

		else {
			/*
			 *********************** RANDOM IO ****************
			 */
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** R[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			//my_output(stderr, "** %02d ** R[%02d] OPEN - %s : ok, size[%lld]\n", (int)ID, (int)ii, (char *)testfile, (long long)s.st_size);
			rd = 0;
			iocount = 5;
			for (; (iocount > 0); iocount--) {
				rsize = s.st_size-s.st_size/2;
				if (rsize > 0) {
					off = (nc_size_t)rand() % (nc_size_t)(s.st_size-s.st_size/2);
					off = max(0, off);
				}
				else {
					off = 0;
				}
				//my_output(stderr, "size[%lld], offset[%lld]\n", s.st_size, off);
				assert(off <= s.st_size);
				//off = (off/BLOCK_SIZE)*BLOCK_SIZE;
				//remained = min(s.st_size, sizeof(buf));
				remained = 64*1024L;
				remained = (long) min((s.st_size - off), (long long)remained);
				remained = max(0, remained);
				l = nc_read(F, buf, off, remained);
				if (l != remained) {
					my_output(stderr, "###################### run_disk_io:: (%s) ERROR!! -> wrong read (off=%lld, size=%lld) file size=%lld => %ld\n", 
									testfile,
									(long long)off,
									(long long)remained,
									(long long)s.st_size,
									(long)l
									);
					//exit(1);
					bt_msleep(1000);
					break;
				}
				__sync_fetch_and_add(&IOSUM, l);
				rd += l;
			}
			//my_output(stderr, "** %02d ** R[%02d] RAND.READ - %s : %ld bytes, ok, size[%lld](iocount=%d)\n", (int)ID, (int)ii, (char *)testfile, rd, (long long)s.st_size, iocount);
			nc_close(F);
			//my_output(stderr, "** %02d ** R[%02d] IO - %s : all done ***  \n\n\n", ID, ii, testfile);
		}
	//}

	}
	__threads--;
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****(%d)!\n", ID, __threads);

}
void *
run_stat_stress(void *d)
{
	int 				ID = (int)d;
	int 				iocount;
	int					sz;
	int 				R;
	int 				r;
	long long 			rsize;
	long 				rd = 0;
	nc_stat_t			s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				buf[1024*1024];
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 150; /* MB */
	char 			testfile[8192];



#ifdef WIN32
	my_output(stderr, "----- run stat stress : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run stat stress: %ld\n", pthread_self());
	srand(pthread_self()*time(NULL));
#endif
	for (ii = 0; !__term && ii < loopcnt; ii++) {
		R = rand() % __files_count;
		if ((R % 1000) == 0) {
			snprintf(testfile, sizeof(testfile), "%s.NOTFOUND", __files[R]);
		}
		else {
			snprintf(testfile, sizeof(testfile), __files[R]);
		}

		ra = rand() % 5;
#ifndef WIN32
		if (ra == 0) {
			sc_invalidate_entry_with_name(mnt, testfile);
		}
		else { 
			if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			bt_msleep(1000);
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile,r );
			continue;
			}
			else {
#undef st_mtime
#undef st_ctime
#if 1
				my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", 
						ID, 
						ii, 
						testfile, 
						s.st_size, 
						s.st_ctime, 
						s.st_mtime, 
						r);
#endif
			}
		}
#endif
		//ra = 0;
	}
	__threads--;
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****(%d)!\n", ID, __threads);

}
void *
run_path_lock_stress(void *d)
{
	void 				*pl[10];
	int 				ID = (int)d;
	int 				slt = 0;
	int 				midx = 0;
	int 				j;
	int 				iocount;
	int					sz;
	int 				R;
	int 				r;
	long long 			rsize;
	long 				rd = 0;
	nc_stat_t			s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				buf[1024*1024];
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 150; /* MB */
	char 			testfile[8192];



#ifdef WIN32
	my_output(stderr, "----- run stat stress : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run stat stress: %ld\n", pthread_self());
	srand(pthread_self()*time(NULL));
#endif
	for (ii = 0; !__term && ii < loopcnt; ii++) {
		R = rand() % __files_count;
		if ((R % 1000) == 0) {
			snprintf(testfile, sizeof(testfile), "%s.NOTFOUND", __files[R]);
		}
		else {
			snprintf(testfile, sizeof(testfile), __files[R]);
		}
		midx = rand() % 10;
		slt = (((int)rand()) % 10);

		for (j = 0; j < slt; j++) {
			pl[j] = nc_pl_lock(mnt_m[midx], testfile);
		}
		//usleep(slt);
		//nc_pl_unlock(mymnt, pl);
		for (j = 0; j < slt; j++) {
			nc_pl_unlock(mnt_m[midx], pl[j]);
		}
		if (ii && ((ii % 1000) == 0)) {
			my_output(stderr, "** %02d **  [%d loops] PATH-LOCK - done\n",
						ID, 
						ii);
		}


	}
	__threads--;
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****(%d)!\n", ID, __threads);

}
void *
run_disk_io_wms(void *d)
{
	long 				iomax;
	int 				ID = (int)d;
	int 				retry = 2;
	int 				r;
	int					R;
	nc_stat_t	s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				*buf = NULL;
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 10000; /* MB */
	char 			testfile[256]= "";
	int 			buflen = 8192;



	//block_signal();
	buf = (char *)malloc(buflen);
#ifdef WIN32
	my_output(stderr, "----- run_disk_io_wms : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_wms : %d(file count=%d), loop=%d, __term=%d\n", pthread_self(), __files_count, loopcnt, __term);
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii <loopcnt; ii++) {
#ifdef WIN32
		//Sleep(1000);
#else
		//bt_msleep(1000);
#endif
		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % TFILE_CNT)+1);
		//if (__files_count > 0)
			//R = rand() % __files_count;
		R = (time(NULL) + rand() % 1000)  % __files_count;
		snprintf(testfile, sizeof(testfile), __files[R]);

		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % 4)+1);
		//ra = rand() % 10;
		//
		//my_output(stderr, "target file = [%s]\n", testfile);
		ra = 1;
		if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile,r);
#ifdef WIN32
			Sleep(1000);
#else
			bt_msleep(1000);
#endif
			continue;
		}
//		else {
//			my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", ID, ii, testfile, s.st_size, s.st_ctime, s.st_mtime, r);
//		}
	
		if (ii && (ii % 100 == 0)) {
			my_output(stderr, "** %02d **  [%02d]  %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", ID, ii, testfile, s.st_size, s.st_ctime, s.st_mtime, r);
		}
	
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** S[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			if (s.st_size > 0) {
				//my_output(stderr, "** %02d ** R[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
				val = nc_read(F, buf, 0LL, 1024);
				if (val < 0) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read ('%s', 0, 1024): %d returned(File.size=%lld)\n", 
								(int)ID, (char *)testfile, (int)val, (long long)s.st_size);
#ifdef WIN32
					Sleep(5000);
#else
					sleep(5);
#endif
						//abort();
					goto L_file_finish;
				}
				__sync_fetch_and_add(&IOSUM, l);
	
				val = nc_read(F, buf, (off = max(0, s.st_size - 1024)), 1024);
				if (val < 1024) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read ('%s', OFF[%lld], %d): %d returned(should be 1024, file.size=%lld)\n", 
								(int)ID, (char *)testfile, (long long)off, 1024, (int)val, (long long)s.st_size);
						//abort();
#ifdef WIN32
					Sleep(5000);
#else
					sleep(5);
#endif
					goto L_file_finish;
				}
				__sync_fetch_and_add(&IOSUM, l);
			}
			nc_close(F);
			F = nc_open(mnt, testfile, gmode);
			retry = 2;
			do {


			iomax = remained = max(s.st_size, 10000000L);
#if 1

			//my_output(stderr, "** %02d ** S[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			if (s.st_size > 0)
				off 	 = rand() % s.st_size;
			else
				off 	 = 0;
			remained = min(iomax, s.st_size - off);

			while (!__term && F && remained > 0) {
				l = nc_read(F, buf, off, min(buflen, remained));
				if (l <= 0) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read ('%s', OFF[%lld], %d): %d returned(should be %ld)\n", ID, testfile, off, sizeof(buf), l, remained);
#ifdef WIN32
					Sleep(5000);
#else
					sleep(5);
#endif
					//abort();
					goto L_file_finish;
				}
				off += l;
				remained -= l;
				__sync_fetch_and_add(&IOSUM, l);
			}
			retry--;
			nc_close(F);
			F = nc_open(mnt, testfile, gmode);
			} while ((retry > 0) && (F != NULL));

			if (remained != 0) {
				my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!** %02d ** S[%02d] SEQ.READ - %s : failed, remained[%lld]\n", ID, ii, testfile, remained);
			}
//			else {
//				my_output(stderr, "** %02d ** S[%02d] SEQ.READ - %s : ok, remained[%lld]\n", ID, ii, testfile, remained);
//			}
#endif
L_file_finish:
			if (F) nc_close(F);
//			my_output(stderr, "** %02d ** R[%02d] CLOSE - %s : ok\n", ID, ii, testfile);

	}
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****!\n", ID);
	__threads--;

}
void *
run_disk_io_wma(void *d)
{
	int 				ID = (int)d;
	int 				r;
	int					R;
	nc_stat_t	s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				*buf = NULL;
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 10000; /* MB */
	char 			testfile[512]= "";
	int 			buflen = 1024*256;



	//block_signal();
	buf = (char *)malloc(buflen);
#ifdef WIN32
	my_output(stderr, "----- run_disk_io_wms : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_wms : %d(file count=%d), loop=%d, __term=%d\n", pthread_self(), __files_count, loopcnt, __term);
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii <loopcnt; ii++) {
#ifdef WIN32
		//Sleep(2);
#else
		//bt_msleep(1000);
#endif
		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % TFILE_CNT)+1);
		if (__files_count > 0)
			R = (time(NULL) + rand() % 3000)  % __files_count;
		else
			R = 0;
		strncpy(testfile, __files[R], sizeof(testfile));

		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % 4)+1);
		//ra = rand() % 10;
		//
		if (ii && (ii % 100 == 0)) {
			my_output(stderr, "** %02d **  [%02d]  %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", ID, ii, testfile, s.st_size, s.st_ctime, s.st_mtime, r);
		}
		//my_output(stderr, "target file = [%s]\n", testfile);
		ra = 1;
		if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile, r);
#ifdef WIN32
			Sleep(1000);
#else
			bt_msleep(1000);
#endif
			continue;
		}
//		else {
//			my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", ID, ii, testfile, s.st_size, s.st_ctime, s.st_mtime, r);
////		}
//		if (R % 5 == 0) {
//			Sleep(10000);
//		}
	
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** S[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
//			my_output(stderr, "** %02d ** R[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			if (s.st_size > 0) {
				l = nc_read(F, buf, 0LL, 4112);
				if (l < 0) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read (OFF[%lld], %d): %d returned\n", ID, off, sizeof(buf), l);
					nc_close(F);
#ifdef WIN32
					Sleep(1000);
#else
					bt_msleep(1000);
#endif
					continue;
				}
			}
			__sync_fetch_and_add(&IOSUM, l);

			if ((s.st_size - buflen) > 0) {
				l = nc_read(F, buf, s.st_size - buflen, buflen);
				if (l < 1024) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read (OFF[%lld], %d): %d returned\n", ID, off, sizeof(buf), l);
					goto L_file_finish;
				}
			}
			__sync_fetch_and_add(&IOSUM, l);


			remained = s.st_size;
#if 1

//			my_output(stderr, "** %02d ** S[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			off 	 = 0;
			//remained -= off;

			while (!__term && remained > 0) {
				l = nc_read(F, buf, off, buflen);
				if (l < 0) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read (OFF[%lld], %d): %d returned\n", ID, off, sizeof(buf), l);
					goto L_file_finish;
				}
				off += l;
				remained -= l;
				__sync_fetch_and_add(&IOSUM, l);
			}
			if (remained != 0) {
				my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!** %02d ** S[%02d] SEQ.READ - %s : failed, remained[%lld]\n", ID, ii, testfile, remained);
			}
//			else {
//				my_output(stderr, "** %02d ** S[%02d] SEQ.READ - %s : ok, remained[%lld]\n", ID, ii, testfile, remained);
//			}
#endif
L_file_finish:
			nc_close(F);
//			my_output(stderr, "** %02d ** R[%02d] CLOSE - %s : ok\n", ID, ii, testfile);

	}
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****!\n", ID);
	__threads--;

}
void *
run_disk_io_oc(void *d)
{
	int 				ID = (int)d;
	int 				r;
	int					R;
	nc_stat_t	s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				*buf = NULL;
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 10000; /* MB */
	char 			testfile[256]= "";
	int 			buflen = 1024*256;



	//block_signal();
	buf = (char *)malloc(buflen);
#ifdef WIN32
	my_output(stderr, "----- run_disk_io_wms : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_wms : %d(file count=%d), loop=%d, __term=%d\n", pthread_self(), __files_count, loopcnt, __term);
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii <loopcnt; ii++) {
#ifdef WIN32
		Sleep(10);
#else
		bt_msleep(1000);
#endif
		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % TFILE_CNT)+1);
		R = rand() % __files_count;
		snprintf(testfile, sizeof(testfile), __files[R]);

		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % 4)+1);
		//ra = rand() % 10;
		//
		my_output(stderr, "target file = [%s]\n", testfile);
		ra = 1;
		if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile, r);
			bt_msleep(1000);
		}
		else {
			my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", ID, ii, testfile, s.st_size, s.st_ctime, s.st_mtime, r);
		}
	
	
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** S[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			my_output(stderr, "** %02d ** R[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			nc_close(F);
			my_output(stderr, "** %02d ** R[%02d] CLOSE - %s : ok\n", ID, ii, testfile);

	}
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****!\n", ID);
	__threads--;

}
void *
run_disk_io_verify(void *d)
{
	int 				ID = (int)d;
	int 				r, br;
	int					R;
	nc_stat_t	s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd, J, linemax, lno, flno;
	int					ra = 0;
	char 				*buf = NULL;
	nc_off_t 			off;
	nc_ssize_t 			l1, l2, l;
	nc_ssize_t			remained;
	int 				val;
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 10000; /* MB */
	char 			testfile[1024]= "";
	int 			buflen = 8192;
	char 			*dig;



	//block_signal();
	buf = (char *)malloc(buflen);
#ifdef WIN32
	my_output(stderr, "----- run_disk_io_verify : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_verify : %d(file count=%d), loop=%d, __term=%d\n", pthread_self(), __files_count, loopcnt, __term);
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii <loopcnt; ii++) {
#ifdef WIN32
		//Sleep(1000);
#else
		bt_msleep(1000);
#endif
		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % TFILE_CNT)+1);
		R = rand() % __files_count + rand()%100;
		if (R < __files_count)
			snprintf(testfile, sizeof(testfile), __files[R]);
		else {
#ifdef WIN32
			snprintf(testfile, sizeof(testfile), "/%d/%ld.NOTFOUND", GetCurrentThreadId(), ii);
#else
			snprintf(testfile, sizeof(testfile), "/%d/%ld.NOTFOUND", time(NULL), ii);
#endif
		}

		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/100M/%06d", (rand() % 4)+1);
		//ra = rand() % 10;
		//
		my_output(stderr, "** %02d **  [%02d] TRGT - %s : trying\n", ID, ii, testfile);
		ra = 1;
		if ((r = nc_getattr(mnt, testfile, &s)) != 0) {
			bt_msleep(1000);
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile, r);
			continue;
		}
		else {
			my_output(stderr, "** %02d ** V[%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", ID, ii, testfile, s.st_size, s.st_ctime, s.st_mtime, r);
		}
#if 1	
		F = nc_open(mnt, testfile, gmode);
		if (F == NULL) {
			my_output(stderr, "** %02d ** V[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
			continue;
		}
		linemax = (s.st_size+15) / 16;
		linemax = linemax/10;
		my_output(stderr, "** %02d ** V[%02d] PREP - %s : %d lines\n", ID, ii, testfile, linemax);
		for (J = 0;  J < 10000; J++) {
			lno = (rand()) % linemax;
			lno = J*10;

			off = (long long)lno * 16LL;
			br = rand() % 16 + 1;
			l1 = nc_read(F, buf, off, br);
			if (l1 != br) {
				my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** V:nc_read (OFF[%lld], %d): %d returned\n", ID, lno*16, 16, l);
				TRAP;
				goto L_file_finish_v;
			}
			off += l1;
			if (16-l1 > 0) {
				l2 = nc_read(F, buf+l1, off, 16-l1);
				if (l2 != (16-l1)) {
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** V:nc_read (OFF[%lld], %d): %d returned\n", ID, lno*16, 16, l);
					TRAP;
					goto L_file_finish_v;
				}
			}

			if (J && (J %1000) == 0) {
				my_output(stderr, "** %02d ** V[%02d] VRFY - %s : %d-th loop (line #=%d)\n", ID, ii, testfile, J, lno);
			}
			l = l1+l2;
			buf[l] = 0;
			dig = buf; while (*dig == '0') dig++;
			flno = atoi(dig);
			//my_output(stderr, "** %02d ** V[%02d] PREP - %s : data at line %ld(%lld) => %ld\n", ID, ii, testfile, lno, off, flno);
			if (flno != lno) {
				my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** V:nc_read (OFF[%lld], %d): mismatch(E:%ld, R:%ld), BUF[%s]\n", ID, off, 16, lno, flno, buf);
				TRAP;
			}
			__sync_fetch_and_add(&IOSUM, l);
		}
L_file_finish_v:
		nc_close(F);
		my_output(stderr, "** %02d ** V[%02d] CLOSE - %s : ok\n", ID, ii, testfile);
#endif

	}
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****!\n", ID);
	__threads--;

}
void *
run_disk_io_1G(void *d)
{
	int 				ID = (int)d;
	int 				r;
	nc_stat_t	s;
	nc_file_handle_t 		*F = NULL;
	nc_uint32_t			logmask;
	int					ii, L, rnd;
	int					ra = 0;
	char 				buf[8192];
	nc_off_t 			off;
	nc_ssize_t 			l;
	nc_ssize_t			remained;
	int 				val;
	char			cache_path[] = "I:\\cache";
	long 			cache_mem = 128; /* MB */
	long 			inode_max = 10000; /* MB */
	char 			testfile[256];



#ifdef WIN32
	my_output(stderr, "----- run_disk_io_1G : %d\n", GetCurrentThreadId());
	srand(GetCurrentThreadId()*time(NULL));
#else
	my_output(stderr, "----- run_disk_io_1G : %d\n", pthread_self());
	srand(pthread_self()*time(NULL));
#endif
	//for (L=0; L < 10; L++) {

	for (ii = 0; !__term && ii <loopcnt; ii++) {
		//snprintf(testfile, sizeof(testfile), "/NetCache_WIN/1G/000001");
		snprintf(testfile, sizeof(testfile), "/11145/video/v1000.wmv");
		//ra = rand() % 2;
		ra = 0;
		if ((r = nc_getattr(mnt, testfile, &s)) < 0) {
			bt_msleep(1000);
			my_output(stderr, "** %02d **  [%02d] STAT - %s : failed(error=%d)\n", ID, ii, testfile, r);
		}
		else {
			my_output(stderr, "** %02d **  [%02d] STAT - %s : size=%lld bytes, ctime[%ld], mtime[%ld] [%d]\n", ID, ii, testfile, s.st_size, s.st_ctime, s.st_mtime, r);
		}
		if (!ra) {
	
	
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** S[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			remained = s.st_size;
			my_output(stderr, "** %02d ** S[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			off 	 = 0;
			while (!__term && remained > 0) {
				l = nc_read(F, buf, off, sizeof(buf));
				if (l < 0) {
					__term=1;
					my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ** %02d ** S:nc_read (OFF[%lld], %d): %d returned\n", ID, off, sizeof(buf), l);
					nc_close(F);
					goto L_end;
					break;
				}
				off += l;
				remained -= l;
				__sync_fetch_and_add(&IOSUM, l);
			}
			if (remained != 0) {
				my_output(stderr, "ERROR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!** %02d ** S[%02d] SEQ.READ - %s : failed, remained[%lld]\n", ID, ii, testfile, remained);
			}
			else {
				my_output(stderr, "** %02d ** S[%02d] SEQ.READ - %s : ok, remained[%lld]\n", ID, ii, testfile, remained);
			}
	#if 0
			my_output(stderr, "++++++++++++++++++++++++++++++DATA+++++++++++++++++++\n\n"
					"%s"
					"\n\n++++++++++++++++++++++++++++++++++++++++++++++++++++++",
					buf);
	#endif
			nc_close(F);
		} /* sequencial */

		else {
			/*
			 *********************** RANDOM IO ****************
			 */
			my_output(stderr, "***   RANDOM IO TEST\n");
			F = nc_open(mnt, testfile, gmode);
			if (F == NULL) {
				my_output(stderr, "** %02d ** R[%02d] OPEN - %s : failed\n", ID, ii, testfile);
				bt_msleep(5000);
				continue;
			}
			my_output(stderr, "** %02d ** R[%02d] OPEN - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			for (rnd = 0; rnd < 500; rnd++) {
				off = (nc_size_t)rand() % (s.st_size-1000);
				off = max(0, off);
				off = (off/BLOCK_SIZE)*BLOCK_SIZE;
				remained = min(s.st_size, sizeof(buf));
				l = nc_read(F, buf, off, remained);
				if (l != remained) {
					my_output(stderr, "run_disk_io_1G:: (%s) ERROR!! -> wrong read (off=%lld, size=%lld) file size=%lld => %ld\n", 
									testfile,
									off,
									remained,
									s.st_size,
									l
									);
					//exit(1);
					bt_msleep(1000);
					break;
				}
				__sync_fetch_and_add(&IOSUM, l);
			}
			my_output(stderr, "** %02d ** R[%02d] RAND.READ - %s : ok, size[%lld]\n", ID, ii, testfile, s.st_size);
			nc_close(F);
			my_output(stderr, "** %02d ** R[%02d] IO - %s : all done ***  \n\n\n", ID, ii, testfile);
		}
	//}
#ifdef WIN32
		Sleep(5000);
#endif
	}
L_end:
	my_output(stderr, "** %02d ** ** ** ** ** **************D O N E ! *****!\n", ID);
	__threads--;

}
void my_output(FILE *iof, char *fmt, ...)
{
	int				depth;
	struct timeval 	tp;
	va_list 		ap;
	char 			depth_str[1024] = "";
	int				depth_str_len;
	char 			*p, *p2;
	int 			n, i;
	int				uid;



	//depth_str = (char *)malloc(2048+1);
	depth_str_len = sizeof(depth_str);
#ifdef WIN32
	uid = GetCurrentThreadId();
#else
	uid = pthread_self();
#endif

	p = depth_str;

	n = snprintf(p, depth_str_len, "{%05d} ", uid);
	p += n;
	depth_str_len -= n;

	*p = 0;

	pthread_mutex_lock(&s__io_lock);
	va_start(ap, fmt);
	fprintf(iof, depth_str);
	vfprintf(iof, fmt, ap);
	va_end(ap);
	pthread_mutex_unlock(&s__io_lock);
	//free(depth_str);
}
