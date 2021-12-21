#include <config.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#ifdef HAVE_SIGNAL_H
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <signal.h>
#endif

#include <ctype.h>

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#ifdef HAVE_DLFCN_H 

#include <dlfcn.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#ifndef WIN32
#include <pwd.h>
#include <grp.h>
#endif

#include <sys/param.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef RLIMIT_DEFINED
#include <sys/resource.h>
#endif



#include <config.h>
#include <netcache_config.h>
#include <trace.h>
#include <hash.h>
#include <netcache.h>
#include <block.h>
#include <disk_io.h>
#include <threadpool.h>
#include "util.h"
#include "hash.h"
#include "tlc_queue.h"
#include "asio.h"
#include "bt_timer.h"
#include "snprintf.h"



int							g__origin_ra_blk_cnt= 0; /* nc_init에서 초기화됨 */
int							g__cache_ra_blk_cnt = 1; /* 상동 */
int 						g__nc_inited 		= 0;
int 						g__readahead_MB 	= 2; /*기본 2MB */
int 						g__forceclose 		= 0;
int							g__busy_asio		= 0;
extern nc_size_t			g__inode_heap_size;
extern long 				g__cache_size; 
extern long 				g__inode_count;
extern nc_int16_t 			__memcache_mode;
extern nc_int64_t 			g__tb_seed;
extern tp_handle_t 			g__asio_thread_pool;
nc_int32_t 					__nc_block_size = 256*1024;
nc_int64_t 					g__dynamic_memory_usage = 0LL;
nc_int64_t 					g__adynamic_memory_usage = 0LL;
int							g__need_fastcrc = 0; /* 0:entire chunk, posivie in bytes */
int 						g__enable_cold_caching = U_FALSE;
int 						g__cold_ratio = 30; /* percent */
char						g__default_group[128] = "nobody";
char						g__default_user[128] = "nobody"; /* nobody */
int							g__default_uid = 0;
int							g__oapc_count = 0;
int							g__default_gid = 0;
int							g__strict_crc_check = 0;
long						g__cm_interval = 60000; /* 60 secs */
extern nc_int32_t 					g__pl_poolsize;
extern int					g__clfu_inode_base; 
extern int					g__clfu_inode_base_percent; 
extern int					g__clfu_inode_total;
extern int					g__clfu_inode_kept;
extern int					g__clfu_blk_base; 
extern int					g__clfu_blk_base_percent; 
extern int					g__clfu_blk_total;
extern int					g__clfu_blk_kept;
extern int 					g__max_asio_vector_count;
extern long 				g__inode_opened;
extern __thread int 		__bc_locked;
extern fc_clfu_root_t	*	g__inode_cache;
extern fc_clfu_root_t	*	g__blk_cache;
char 						g__cluster_ip[128]="";
char 						g__local_ip[128]="";
int 						g__cluster_ttl = 1;
nc_uint64_t 				g__write_bytes = 0;
nc_uint64_t 				g__write_count = 0;
int 						g__need_stat_backup = 1;
int 						g__block_spin_interval = 60;
char 						__build_info[] 		= PACKAGE_NAME " " PACKAGE_VERSION " in " __DATE__ " " __TIME__;
char 						__charset[128]		= "UTF-8";
char						__cache_dir[256]	= "";
int 						__terminating = 0;
int 						__terminating_timer = 0;

//int 						__task_cnt = 0;
int 						__ms_allocated = 0; 
char 						__stat_path[256]="./netcache_stat.txt";
char 						__plugin_path[256]="/usr/service/lib/plugin";

int 						g__pending_inodeop = 0;
int 						g__pending_open = 0;
int 						g__pending_close = 0;
int 						g__pending_io = 0;
int 						g__pending_stat = 0;
int 						g__pending_purge = 0;
int 						g__intelligent_cold_caching = 0; /* 1: cold content에 대한 과도한 disk부하 회피*/

unsigned int 				g__blk_rd_network  = 0; /* # of origin chunk read count */
unsigned int 				g__blk_rd_disk = 0; /* # of chunk read count */
unsigned int 				g__blk_wr_disk = 0; /* #of chunk write count */
unsigned int 				g__blk_rd_fault = 0; /* 디스크 캐시를 로딩했으나, 사용되지 않고 사라진 청크 갯수*/
float						g__false_read_limit = 0.5; 	/* default 0.1% */



time_t 						netcache_signature = 0;
bt_timer_t 					s__timer_memory_reclaim;
bt_timer_t 					s__timer_check_heap;
bt_timer_t 					s__timer_cache_monitor;

bt_timer_t 					s__timer_RSS_monitor; /* for debugging */

int 						__s__max_ios = 512;
int							__positive_TTL = 30;  /* 30초 */
int							__negative_TTL = 5;  /* 5초 */
time_t						__nc_clock = 0;


long						__s_ref_blocks = 0;
long						__s_ref_blocks_osd = 0;
long						__s_ref_blocks_mem = 0;
long						__s_ref_blocks_dsk = 0;

int 						__enable_compaction = 1; /* default enable */
int							__enable_asio_timer = 0;

//#define						CFS_MAX_TASKS		100
//pthread_t 				 	__tasks[CFS_MAX_TASKS];
pthread_t 				 	__tasks_timer; /* 나중에 종료하기위해 특별 처리 필요 */
mavg_t						*g__readi;
mavg_t						*g__openi;
mavg_t						*g__closei;
mavg_t						*g__dskio; /* disk block io 시간(read/write 모두)*/
mavg_t						*g__netblkr; /* IO schedule후 block데이타를 origin에서 수신할 때까지 걸린 시간 */
mavg_t						*g__nri; /* origin read 시간*/
mavg_t						*g__dri; /* disk read io 시간*/
mavg_t						*g__chi; /* context handling*/

mavg_t						*g__rapc; /* readapc 시간*/
mavg_t						*g__wapc; /* blk_ri_wait 시간*/
/*
 *
 * for debug-only
 *
 */
mavg_t						*g__prepblk;
mavg_t						*g__preppage;


static float				s__mhit_ratio = 0.0;


extern int 					__min_asio_threads;
extern int					__max_asio_threads;
extern nc_size_t 			g__allocated;
extern memmgr_heap_t		g__inode_heap;
extern memmgr_heap_t		g__page_heap;
extern int 					g__dm_reclaim_highmark;
extern int 					g__dm_reclaim_lowmark;


static void nc_dump_param();
void dump_clfu(int sig);
static int nc_get_revision();
void emergency_unvolume();
int g_maskarray[6] =
	{ (T_INFO | T_WARN | T_ERROR), T_TRACE, T_DAV, T_BLOCK, T_INODE,
T_DEBUG };
int g_maskidx = 0;
int g_debugmask = 0;

static int nc_check_path_limit(const char *path);


__thread int __nc_errno = 0;


/*weon's addition 2009/08/02 */
void  *	PUBLIC_IF __timer_wheel_base = NULL;
long long	stat_bytes_read = 0;
typedef struct {
	char	name[32];
	int		id;
	int		scale;
	int		type;
} nc_api_stat_t;
nc_api_stat_t nc_api_stat[] = 
{
#define 								SBFS_STAT_OPEN 				0
	{"OPEN(c/s)", -1, 1, STATT_REL},
#define 								SBFS_STAT_OPENRT 			1
	{"OPEN(msec)", -1, 1, STATT_ABS},
#define 								SBFS_STAT_READ 				2
	{"READ(MB/s)", -1, 1000000, STATT_REL},
#define 								SBFS_STAT_WRITE 			3
	{"WRITE(MB/s)", -1, 1000000, STATT_REL},
#define 								SBFS_STAT_CLOSE 			4
	{"CLOSE(c/s)", -1, 1, STATT_REL},
#define 								SBFS_STAT_CLOSERT 			5
	{"CLOSE(msec)", -1, 1, STATT_ABS},
#define 								SBFS_STAT_READRT 			6
	{"READ(msec)", -1, 1000, STATT_ABS},
#define 								SBFS_STAT_STAT 				7
	{"STAT(c/s)", -1, 1, STATT_REL},
#define 								SBFS_GETATTR				8
	{"GETATTR(ms)",	-1, 1, STATT_ABS},
#define 								SBFS_READDIR 				9
	{"READDIR(ms)",	-1, 1, STATT_REL},
#define 								SBFS_MKNOD 	    			10
	{"MKNOD(ms)", 	-1, 1, STATT_REL},
#define 								SBFS_WRITE 	    			11
	{"WRITE(ms)", 	-1, 1, STATT_REL},
#define 								SBFS_FLUSH 	    			12
	{"FLUSH(ms)", 	-1, 1, STATT_REL}
};
#define 	SBFS_STAT_COUNT		howmany(sizeof(nc_api_stat), sizeof(nc_api_stat_t))


#define	VOLUME_OP_BEGIN(v_, xl_, chkonline_, r_)		 \
		if (v_ && nvm_lock(v_, xl_, __FILE__, __LINE__)) { \
			if (chkonline_) { \
				if (v_->online == 0) { \
					__nc_errno = ENODEV; \
					nvm_unlock(v_, __FILE__, __LINE__); \
					return r_; \
				} \
			} \
			VOLUME_REF(v_); \
		} \
		else { \
			__nc_errno = ENODEV; \
			return r_; \
		}

#define	VOLUME_RETURN_ERROR(v_, r_) { \
					VOLUME_UNREF(v_); \
					nvm_unlock(v_, __FILE__, __LINE__); \
					return r_; \
				}
#define VOLUME_OP_END(v_, r_) { \
				VOLUME_UNREF(v_); \
				nvm_unlock(v_, __FILE__, __LINE__); \
				return r_; \
			}

static void nc_adjust_ra();

#ifdef 	NC_HEAPTRACK
static void LEAK(char *l);
#endif


void 
cfs_monitor()
{

#if 0
	cnt++;
	if (cnt && (cnt % 10) == 0) {
		TRACE((0, "# of DISK-IO pending : %d(R:%d, S:%d), LRU.kept=%d, Allocated=%d, Total=%d : POOL_SIZE: %.4f MB\n", 
						s__wait_ios, s__wait_read_ios, s__wait_write_ios, 
						clfu_kept_count(__blk_cache),
						__ms_allocated,
						__block_count,
						(omemusage())/1000000.0
						
						));
	}
#endif	
}
void *timer_scheduler_thread(void *d)
{

#ifdef __PROFILE
	perf_val_t 	ms, me;
	long long 	ud;
#endif
	void nc_udpate_cached_clock();
	int			ticks = 0;

	


	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	/* run timer scheduling in 500msec granularity */
	nc_update_cached_clock();
	while (!__terminating_timer) {
		bt_msleep(10);
		ticks++;
		nc_update_cached_clock();
		DO_PROFILE_USEC(ms, me, ud) {
			bt_run_timers(__timer_wheel_base);
		}
#ifdef __PROFILE
		if (ud > 100000) {
			TRACE((T_WARN, "*** too slow timers(%.2f msec)\n", (float)ud/1000.0));
		}
#endif
#if 0
		if (ticks > 100) {/* every 10 secs */
			nc_adjust_ra();
			ticks = 0;
		}
#endif
	}
	TRACE((T_INFO, "timer scheduler thread gracefully shutdowned\n"));
	
	return NULL;
}

#define		mhit_ratio (float)min(100.0, (((__s_ref_blocks_mem + __s_ref_blocks_dsk) * 100.0)/max(__s_ref_blocks,1)))
static void 
cache_monitor(void *d)
{
#ifdef __PROFILE
	perf_val_t  ms, me;
	long long 	ud;
#endif
	extern nc_int32_t g__total_blks_allocated;
	extern nc_int32_t g__page_count;
	extern int g__octx_count;
	//extern int g__oapc_count;
	extern int g__octx_bind_count;
	extern nc_int32_t g__pl_refs;

	//extern int  s__wait_ios;

	unsigned int disk_rd;
	unsigned int origin_rd;
	unsigned int disk_wr;
	unsigned int fault_rd; 
	int		ifree = 0;
#ifndef NC_RELEASE_BUILD
	int 	total_inode = 0;
	int 	free_inode = 0;
#endif
	nc_uint64_t	mdbr, mdbw, mdbm;
	char		d1[128], d2[128], d3[128], d4[128];
	nc_int64_t	pused,ptotal,iused,itotal;
#if defined(NC_MEM_DEBUG)
	char		abuf[256];
#endif
	int 		max_diocb, pending_diocb;
	int			dirty_blks = 0;


#ifndef NC_RELEASE_BUILD
	int		blk_count, page_count;
	void bcm_count_cache_info(int *blk_count, int *page_count);

	bcm_count_cache_info(&blk_count, &page_count);
#endif
	int			qw_configed, qw_running;

	float		c_mhit_ratio;




	disk_rd 	= GET_ATOMIC_VAL(g__blk_rd_disk);
	origin_rd 	= GET_ATOMIC_VAL(g__blk_rd_network);
	disk_wr 	= GET_ATOMIC_VAL(g__blk_wr_disk);
	fault_rd 	= GET_ATOMIC_VAL(g__blk_rd_fault);

	/*
	 * Experiment : 동적으로 disk read-ahead  갯수 조정 
	 */


	IC_LOCK(CLFU_LM_SHARED);
	ic_get_stat(NULL, NULL, &ifree, NULL, NULL, NULL);
	clfu_get_stat((fc_clfu_root_t *)g__inode_cache, &g__clfu_inode_base_percent, &g__clfu_inode_base, &g__clfu_inode_kept, &g__clfu_inode_total);
	IC_UNLOCK;


	BC_LOCK(CLFU_LM_SHARED);
	clfu_get_stat((fc_clfu_root_t *)g__blk_cache, &g__clfu_blk_base_percent, &g__clfu_blk_base, &g__clfu_blk_kept, &g__clfu_blk_total);
	BC_UNLOCK;


	mdb_get_counter(&mdbr, &mdbw, &mdbm);

#ifndef NC_RELEASE_BUILD
	g__inode_opened = dm_dump_or_count_inode_opened(T_ERROR, &total_inode, &free_inode, U_FALSE);
#endif
	mem_stat(&g__inode_heap, &iused,&itotal);
	mem_stat(&g__page_heap, &pused,&ptotal);


	mavg_t		*ind_arr[] = {
					g__dskio, g__netblkr, NULL,
					g__openi, g__readi, g__closei,
				};
	int			ind_arr_cnt = 6;
	char		zind_info[1024];
	char		*pzind = zind_info;
	int			i, n, remained = sizeof(zind_info);

	double		ind_min, ind_max, ind_avg;
	long		ind_cnt;
	char		zind_cnt[32];


	for (i = 0; i < ind_arr_cnt; i++) {
		if ((i % 3) == 0) {
			n = snprintf(pzind, remained, "\t\t\t\t\t *                          : ");
			pzind 		+= n;
			remained 	-= n;
		}
		if (ind_arr[i]) {
			mavg_stat(ind_arr[i], &ind_cnt, &ind_min, &ind_max, &ind_avg);
			ll2dp(zind_cnt, sizeof(zind_cnt), ind_cnt);
			n = snprintf(pzind, remained, "%s(%s)=%.2f/%.2f/%.2f ", mavg_name(ind_arr[i]), zind_cnt, ind_min, ind_max, ind_avg);
			pzind 		+= n;
			remained 	-= n;
		}
		if ((i + 1) % 3 == 0) {
			n = snprintf(pzind, remained, "\n");
			pzind 		+= n;
			remained 	-= n;

		}
	}
	*pzind = 0;



	pm_get_winfo(&qw_configed, &qw_running);

	dio_get_wait(&max_diocb, &pending_diocb);
	dirty_blks = bcm_track(BS_CACHED_DIRTY);
	TRACE((T_MONITOR|T_INFO|T_STAT, 
					"Cache Monitor (per %d secs): ****************\n"
				   "\t\t\t\t\t * Cache Hit  : %.2f %% == (Mem=%.2f %% + Disk=%.2f %%)\n"
				   "\t\t\t\t\t * Block Refs : Total=%ld == (Mem=%ld + Disk=%ld + Origin=%ld) \n"
				   "\t\t\t\t\t * Inode Cache: Base Count=%d, Base Population=%d%%, Cached Total=%d(kept=%d)(%.2f %%)(%d free, %d KVs(%d pl refs))\n"
				   "\t\t\t\t\t * Block Cache: Base Count=%d, Base Population=%d%%, Cached Total=%d(kept=%d)(%.2f %%)(%d/%d pages free, total %d blks)\n"
 
#if defined(NC_HEAPTRACK) || defined(NC_MEM_DEBUG)
				   "\t\t\t\t\t * Total Heap Usage : %.2f MB (%s) \n"
#endif
				   "\t\t\t\t\t * Total Cache Memory Usage : Page=%s MB/%s MB, Inode=%s MB/%s MB \n"
				   "\t\t\t\t\t * Total MDB IO : total %lld (Read %lld, Write %lld, Remove %lld) (%d/%d running) \n"
#ifndef NC_RELEASE_BUILD
				   "\t\t\t\t\t * In-use(opened) Inode Count : %ld/%d (%d free)\n"
#endif
					// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603) 
					//                - 원부사장님 수정 소스 적용
				   "\t\t\t\t\t * Async IO event handler   : %d(busy=%d)/%d (pending IOs=%d/%d), dirty blks=%d\n"
#ifndef NC_RELEASE_BUILD
				   "\t\t\t\t\t * Block Cache Diagnostics  : blks=%d/%d pages=%d/%d(%d missing)\n"
				   "\t\t\t\t\t * Session Alloc Status     : bound/allocated=%d/%d\n" 
#endif
				   "\t\t\t\t\t * Statistics(min/max/avg)  : IO latency measurement\n"
				   "%s"
				   "\t\t\t\t\t * Cache Chunk Read=%u,  Orign Chunk Read=%u, Cache Chunk Write=%u, Cache Chunk Fault Read(RA=%.2fMB)=%u\n"
				   "\t\t\t\t\t * pending OP(s): open=%d read/write=%d close=%d purge=%d stat=%d inodeops=%d\n", 




/*per-interval*/					(int)(g__cm_interval/1000L),

/*cache hit*/		(float)min(100.0, (((__s_ref_blocks_mem + __s_ref_blocks_dsk) * 100.0)/max(__s_ref_blocks,1))),
/*cache hit*/		(float)(__s_ref_blocks_mem  * 100.0)/max(__s_ref_blocks, 1),
/*cache hit*/		(float)(__s_ref_blocks_dsk  * 100.0)/max(__s_ref_blocks, 1),

/*block refs*/		(long)__s_ref_blocks, 
						(long)__s_ref_blocks_mem, 
						(long)__s_ref_blocks_dsk, 
						(long)__s_ref_blocks_osd, /* block cache access stat */

/*inode cache*/		g__clfu_inode_base, 
						g__clfu_inode_base_percent, 
						g__clfu_inode_total, 
						g__clfu_inode_kept,
						(float)((100.0*g__clfu_inode_kept)/g__clfu_inode_total),
						(int)ifree,
						(int)kv_count_allocated(), 
						g__pl_refs,

/*block cache*/		g__clfu_blk_base, 
						g__clfu_blk_base_percent, 
						g__clfu_blk_total,
						g__clfu_blk_kept,
						(float)((100.0*g__clfu_blk_kept)/g__clfu_blk_total),
						(int)bcm_get_free_page_count(), 
						g__page_count, 
						g__total_blks_allocated,

#ifdef NC_HEAPTRACK
/* heap usage */	(float)((float)nc_get_heap_allocated()/1000000.0),
						nc_get_heap_alloc_counter_x(),
#elif defined(NC_MEM_DEBUG)
/* heap usage */	(float)(_ATOMIC_ADD(g__dynamic_memory_usage, 0)/1000000.0),
						(char *)ai_dump(abuf, sizeof(abuf)),
#endif
					ll2dp(d1, sizeof(d1), pused/1000000),
					ll2dp(d2, sizeof(d2), ptotal/1000000),

					ll2dp(d3, sizeof(d1), iused/1000000),
					ll2dp(d4, sizeof(d2), itotal/1000000),


					(mdbr+mdbw+mdbm), mdbr, mdbw, mdbm, qw_running, qw_configed, /* MDB conter */
#ifndef NC_RELEASE_BUILD
					(long)g__inode_opened, (int)total_inode,  free_inode,
#endif
					tp_get_workers( g__asio_thread_pool ), (int)GET_ATOMIC_VAL(g__busy_asio), tp_get_configured( g__asio_thread_pool ), pending_diocb, max_diocb, dirty_blks,
#ifndef NC_RELEASE_BUILD
					blk_count, g__clfu_blk_total,
					page_count, g__page_count, (g__page_count - page_count),
					GET_ATOMIC_VAL(g__octx_bind_count), GET_ATOMIC_VAL(g__octx_count),
#endif
					zind_info,
					disk_rd,
					origin_rd,
					disk_wr,
					(float)(g__cache_ra_blk_cnt*NC_BLOCK_SIZE)/1000000.0,
					fault_rd,
					GET_ATOMIC_VAL(g__pending_open), 
					GET_ATOMIC_VAL(g__pending_io), 
					GET_ATOMIC_VAL(g__pending_close), 
					GET_ATOMIC_VAL(g__pending_purge), 
					GET_ATOMIC_VAL(g__pending_stat), 
					GET_ATOMIC_VAL(g__pending_inodeop)
					));
	nc_adjust_ra();
	c_mhit_ratio = mhit_ratio;
	if (s__mhit_ratio > c_mhit_ratio) {
		/*
		 * memory hit 감소
		 */
		g__cache_ra_blk_cnt 	= max(g__cache_ra_blk_cnt - 1, 2); 

	}

	_ATOMIC_SUB(g__blk_rd_disk, disk_rd);
	_ATOMIC_SUB(g__blk_rd_network, origin_rd);
	_ATOMIC_SUB(g__blk_wr_disk, disk_wr);
	_ATOMIC_SUB(g__blk_rd_fault, fault_rd);
	__s_ref_blocks = 0;
	__s_ref_blocks_dsk = 0;
	__s_ref_blocks_osd = 0;
	__s_ref_blocks_mem = 0;


#if 0
	nvm_update_read_stat();
#endif
	DO_PROFILE_USEC(ms, me, ud) {
		bt_set_timer(__timer_wheel_base,  &s__timer_cache_monitor, g__cm_interval, cache_monitor, NULL);
	}
#ifdef __PROFILE
	if (ud > 100000) {
		TRACE((T_PERF, "bt_set_timer (cache_monitor) too slow: %.2f msec\n", (float)ud/1000.0));
	}
#endif
}
#if NOT_USED
static void 
memory_monitor(void *d)
{
	char	zn[64];
	int 	rss = get_memory_usage(); /* KB */

	TRACE((T_WARN, "FOR-DEBUG: VmRSS=%s KB\n", ll2dp(zn, sizeof(zn), (long long)rss)));
	if ((rss / 1000) > g__cache_size*4) {
		TRACE((T_ERROR, "FOR-DEBUG: VmRSS=%d KB > 4 * %ld)\n", rss, (long)g__cache_size));
		__nc_dump_heap(AI_ALL);
		exit(1);
	}

	bt_set_timer(__timer_wheel_base,  &s__timer_RSS_monitor, g__cm_interval, memory_monitor, NULL);
}
#endif

char * 
PUBLIC_IF nc_version(char *buf, int len)
{
	
 	snprintf(buf, len-1, "%s %s.%d in " __DATE__ " " __TIME__, 
			 PACKAGE_NAME,
			 PACKAGE_VERSION,
			 nc_get_revision());
	TRACE((T_API, "version : '%s'\n", buf));
	
	return buf;
} 


void backtrace_lineinfo( int number, void *address, char *symbol )
{
#ifdef HAVE_DLFCN_H
	int			rc;
	Dl_info		info;
	const void	*addr;
	FILE		*output;
	char		cmd_line[1024];
	char		line[1024], *ptr_line, *pos;
	char		function_name[1024];
	int			file_line;

	/* stack frame address 정보를 이용하여 pathname, symbol name 등의 세부 정보 추출 */
	rc = dladdr( address, &info );
	if( ( rc == 0 ) || !info.dli_fname || !info.dli_fname[0] ) {
		TRACE( ( T_ERROR, "%03d  %s\n", number, symbol ) );
		return;
	}

	addr = address;
	if( info.dli_fbase >= ( const void * ) 0x40000000 )
		addr = ( void * ) ( ( unsigned long ) ( ( const char * ) addr ) - ( unsigned long ) info.dli_fbase );

	/* addr2line 명령어를 이용하여 함수명, 파일명:파일 line number 정보 추출
	   - addr2line --functions --demangle -e $(which /root/libnetcache/2.5.0/lib/libhttpn_driver.so) 0x11699 */
	snprintf( cmd_line, sizeof( cmd_line ),
			  "addr2line --functions --demangle -e $(which %s) %p",
			  info.dli_fname, addr );

#if 1
	output = popen( cmd_line, "r" );
	if( !output ) {
		TRACE( ( T_ERROR, "%03d  %s\n", number, symbol ) );
		return;
	}

	function_name[0] = '\0';
	file_line = 0;

	while( !feof( output ) )
	{
		ptr_line = fgets( line, sizeof( line ) - 1, output );
		/* addr2line 호출 결과
		1st line : 함수명 (ex: httpn_open 또는 ?? )
		2nd line : 소스파일명:소스line (ex:  /root/netcache/trunk/plugins/httpn_v2/http_driver.c:1487 또는 ??:0 )
		*/

		if( ptr_line && ptr_line[0] )
		{
			pos = strchr( ptr_line, '\n' );

			if( pos )
				pos[0] = '\0';

			/* 2nd line 인 경우 - 소스파일명:소스 line number 정보 존재 */
			if( strchr( ptr_line, ':' ) ) 
			{
				file_line = 1;

				/* addr2line 호출 결과.. 소스 파일명 추출 실패시 */
				/* CHG 2018-04-26 huibong addr2line 에서 정보 추출 실패시 로깅 개선 (#32077) */
				if( strlen( ptr_line ) > 2 && ptr_line[0] == '?' && ptr_line[1] == '?' )
				{
					TRACE( ( T_ERROR, "%03d  %s%s%s%s\n"
							 , number
							 , symbol
							 , ( function_name[0] ) ? " [function " : ""
							 , function_name
							 , ( function_name[0] ) ? "]" : "" ));
				}
				else
				{
					TRACE( ( T_ERROR, "%03d  %s%s%s%s [0x%llx]\n",
							 number,
							 ptr_line,
							 ( function_name[0] ) ? " [function "
							 : "", function_name,
							 ( function_name[0] ) ? "]" : "",
							 address ) );	/* ??:0 [function ??] 이런식으로 symbol이 깨지더라도 주소는 기록되게한다. */
				}

				function_name[0] = '\0';
			}
			else /* 1st line 인 경우 - 함수명 또는 ?? 정보 */
			{
				if( function_name[0] ) {
					TRACE( ( T_ERROR, "%03d  %s", number, function_name ) );
				}

				snprintf( function_name, sizeof( function_name ), "%s", ptr_line );
			}
		}
	}

	/* CHG 2018-04-26 huibong popen 사용에 따른 pclose 처리 누락 수정 (#32018) */
	pclose( output );

	if( function_name[0] ) 
	{
		TRACE( ( T_ERROR, "%03d  %s\n", number, function_name ) );
	}    /* {} 표시를 하지 않으면 TRACE define에서 if 문이 포함 되어 있어서 아래의 else if 가 정상 동작 하지 않는다. */
	else if( 0 == file_line ) /* popen으로 정보를 못가져 오는 경우 symbol 정보라도 기록을 한다.*/
	{
		TRACE( ( T_ERROR, "%03d  %s\n", number, symbol ) );
	}

#else
		/* jemalloc 사용시 popen을 사용할 수 없음 */
	TRACE( ( T_ERROR, "CMD:%s\n", cmd_line ) );
#endif

#endif
}

static sighandler_t  __sig_segv_handler[64];
static int 			 __in_signal = 0;


#ifdef UNITTEST
#ifdef SIGRTMIN
static void
nc_dump_report(int sig)
{
void nvm_dump_all_pathlocks();
void report_ml_stat();
void dm_dump_inodes();
void report_bcm_stat();

// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
//                  -- disk-read-ahead 크기 동적 조절시 차이 발생

	TRACE((T_MONITOR, "**** resouce usage dump\n"));

#ifdef NC_MEASURE_PATHLOCK
#pragma message (REMIND("PATHLOCK mesaurement enabled"))
	nvm_report_pl_stat();
#endif
#ifdef NC_MEASURE_CLFU
#pragma message (REMIND("CLFU mesaurement enabled"))
	clfu_report_bl_stat();
#endif

#ifdef NC_MEASURE_MUTEX
#pragma message (REMIND("MUTEX mesaurement enabled"))
	report_ml_stat();
#endif

// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
#ifdef NC_DEBUG_BCM
	report_bcm_stat();
#endif

#ifdef 	NC_HEAPTRACK
#pragma message (REMIND("HEAPTRACK reporting enabled"))
	nc_raw_dump_report(LEAK);
#endif

#ifdef NC_DEBUG_PATHLOCK
	{
	nvm_dump_all_pathlocks();
	}
#endif


	dm_dump_inodes();
}
#endif
#endif


void
nc_dump_stack(int sig)
{
	int 		j, nptrs;
#define FRAME_COUNT 	100
	void 		*frames[FRAME_COUNT];
	char 		**strings;

	if (__in_signal) return;
	__in_signal++;
	TRACE((T_ERROR, "signal %d caught!\n", sig));
   	nptrs = backtrace(frames, FRAME_COUNT);

   /* 
    * 	The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
	*  would produce similar output to the following: 
	*/

	strings = backtrace_symbols(frames, nptrs);

	TRACE((T_ERROR, "CRITICAL : call stack dump information(%d frames)\n", nptrs));
	for (j = 0; j < nptrs; j++) {
	     backtrace_lineinfo(j+1, frames[j], strings[j]);
	}
	TRACE((T_ERROR, "CRITICAL : end of stack dump information\n"));
	free(strings);
	if (sig) _exit(1);
	__in_signal--;
}


int PUBLIC_IF nc_init()
{
	struct passwd	*	pw_default = NULL;
	struct group	*	gr_default = NULL;
	pthread_attr_t 		pattr;
	size_t 				stksiz;
	int 				res;
#define	DEFINE_CONFIG(c_) 	{ \
								n 		= sprintf(pcap, #c_ "| "); \
								pcap 	+= n; \
							}
#ifdef WIN32
	HINSTANCE 	htrace = INVALID_HANDLE_VALUE;
#endif

	// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603) 
	//                - 원부사장님 수정 소스 적용
	//                - 불필요 함수 선언 제거
	//void bcm_setup_page_size();

#ifdef __PROFILE
	perf_val_t 	t_b, t_e;
#endif


#ifdef RLIMIT_DEFINED
//typedef void (CALLBACK *SetTraceOutput)(FILE *);
	struct rlimit rlim;
#endif
	PROFILE_CHECK(t_b);

	clrtid();


	g__tb_seed				= nc_cached_clock() + (rand() % 10000);
	g__origin_ra_blk_cnt 	= g__readahead_MB*1024*1024/NC_BLOCK_SIZE;
#if 0
	g__cache_ra_blk_cnt 	= 0; /*disk read-ahead 안함*/
#else
	g__cache_ra_blk_cnt 	= max(1, g__origin_ra_blk_cnt/4); 
#endif

	netcache_signature = time(NULL);
	
	nc_dump_param();



	crc_init();


#ifdef WIN32
	void backtrace_register(void);

#else

	/* CHG 2018-05-17 huibong 중복 및 잘못 mapping 된 signal handler 수정 (#32120) */
#ifdef SIGSEGV
	__sig_segv_handler[SIGSEGV] = signal(SIGSEGV, nc_dump_stack);
#endif

#ifdef SIGBUS
	__sig_segv_handler[SIGBUS] = signal(SIGBUS, nc_dump_stack);
#endif 

#ifdef SIGABRT
	__sig_segv_handler[SIGABRT] = signal(SIGABRT, nc_dump_stack);
#endif 

#ifdef SIGFPE
	__sig_segv_handler[SIGFPE] = signal(SIGFPE, nc_dump_stack);
#endif

// CHG 2019-12-04 huibong 원부사장님 요청으로 Release 버전에서는 사용되지 않도록 수정 처리
// UNIT-TEST에서만 enable
#ifdef UNITTEST

#ifdef SIGRTMIN
#pragma message(REMIND("SIGRTMIN enabled for netcache debugging(should be disabled in release time)"))
	__sig_segv_handler[SIGRTMIN] = signal(SIGRTMIN, nc_dump_report);
	TRACE((T_MONITOR, "SIGRTMIN registered\n"));
#endif 
#endif

#endif





	if (!__timer_wheel_base) {
		TRACE((T_DEBUG, "timer wheel initializing\n"));
		__timer_wheel_base = bt_init_timers();
	}
	if (!__timer_wheel_base) {
		TRACE((T_ERROR, "init_timer - initialization failed, exiting\n"));
		exit(1);
	}

	TRACE((T_DEBUG, "timer wheel initialized:%p\n", __timer_wheel_base));

	bt_init_timer(&s__timer_cache_monitor, "cache_monitor", U_TRUE);
	bt_set_timer(__timer_wheel_base,  &s__timer_cache_monitor, g__cm_interval, cache_monitor, NULL);
#if NOT_USED
	bt_init_timer(&s__timer_RSS_monitor, "memory_monitor", U_TRUE);
	bt_set_timer(__timer_wheel_base,  &s__timer_RSS_monitor, g__cm_interval, memory_monitor, NULL);
#endif
	nvm_init();
	dio_init(__s__max_ios);





#ifdef RLIMIT_DEFINED
	getrlimit(RLIMIT_NOFILE, &rlim);
	TRACE((T_INFO, "File MAX : %lu/%lu\n", rlim.rlim_cur, rlim.rlim_max));
#endif

#ifndef VALGRIND
#ifdef RLIMIT_DEFINED
	rlim.rlim_cur = 100000;	//100000;
	rlim.rlim_max = 100000;	//100000;
	if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
		TRACE((T_ERROR, "Cannot set file descriptor [%lu/%lu]:'%s'\n",
			   rlim.rlim_cur, rlim.rlim_max, strerror(errno)));
		
		PROFILE_CHECK(t_e);
		return -1;
	}
#endif
#endif

#ifdef RLIMIT_DEFINED 
	getrlimit(RLIMIT_CORE, &rlim);
	TRACE((T_INFO, "Core Enabled\n"));
	rlim.rlim_cur = RLIM_INFINITY;
	if (setrlimit(RLIMIT_CORE, &rlim) < 0)
		TRACE((T_INFO, "Failed to 'Core Enabled'\n"));
	TRACE((T_INFO, "CORE dump directory changed to /stg/core\n"));
#endif

	pm_init(g__dm_reclaim_highmark, g__dm_reclaim_lowmark);

	res = bcm_init(g__cache_size, -1, __cache_dir);
	if (res < 0) {
		/* CHG 2018-06-21 huibong 로깅 구문 수정 (#32205) */
		TRACE((T_ERROR, "bcm_init failed in nc_init, exit needed\n"));
		TRAP;
	}
	/*
	 * volume 별로 하는게 더 타당함
	 */
	g__chi 		= mavg_create("blk.context_handler");
	g__readi 	= mavg_create("read");
	g__openi 	= mavg_create("open");
	g__closei 	= mavg_create("close");

	g__dskio	= mavg_create("disk.IO"); 		/* disk block io 시간(read/write포함)*/
	g__netblkr	= mavg_create("network.read");  /* network block read에 걸린 시간*/
	g__dri		= mavg_create("B.stat"); 		/* network block io 시간*/
	g__nri		= mavg_create("B.read"); /* network block io 시간*/

	g__prepblk		= mavg_create("Alloc.blk"); /* blk allocation 시간*/
	g__preppage		= mavg_create("Alloc.pag"); /* page allocation 시간*/

	g__rapc		= mavg_create("B.readapc"); /* network block io 시간*/
	g__wapc		= mavg_create("B.riwait"); /* network block io 시간*/

	pthread_attr_init(&pattr);
	pthread_attr_getstacksize(&pattr, &stksiz);
	pthread_attr_setscope(&pattr, PTHREAD_SCOPE_PROCESS);

	asio_init();



	res = my_pthread_create(&__tasks_timer, &pattr, timer_scheduler_thread, (void *)NULL) ;
	if (res) {
		TRACE((T_ERROR, "failed to create timer scheduler, exiting\n"));
	}
	pthread_attr_destroy(&pattr);
	
#ifndef WIN32
	pw_default = getpwnam((const char *)g__default_user);
	if (pw_default) {
		g__default_uid = pw_default->pw_uid;
	}
	else {
		TRACE((T_WARN, "default user, '%s' - not found, using root as default\n", g__default_user));
	}
	gr_default = (struct group *)getgrnam((const char *)g__default_group);
	if (gr_default) {
		g__default_gid = gr_default->gr_gid;
	}
	else
		TRACE((T_WARN, "default user, '%s' - not found, using root as default\n", g__default_group));
#endif
	/* setup stat-cluster */


	g__nc_inited++; /* netcache core engine initialized */

	TRACE((T_DEBUG, "NetCache Default Permission : uid=%d, gid=%d\n", g__default_uid, g__default_gid));
	TRACE((T_DEBUG, "NetCache successfully initialized\n"));
	{
		char	zcap[512] = "";
		char	*pcap = zcap;
		int		n;

#ifdef 	NC_HEAPTRACK
		DEFINE_CONFIG(NC_HEAPTRACK);
#endif
#ifdef 	NC_MEMLEAK_CHECK
		DEFINE_CONFIG(NC_MEMLEAK_CHECK);
#endif
#ifdef 	NC_OVERFLOW_CHECK
		DEFINE_CONFIG(NC_OVERFLOW_CHECK);
#endif
#ifdef 	NC_MEM_DEBUG
		DEFINE_CONFIG(NC_MEM_DEBUG);
#endif
#ifdef 	__PROFILE
		DEFINE_CONFIG(__PROFILE);
#endif
#ifdef	NC_RELEASE_BUILD
		DEFINE_CONFIG(NC_RELEASE_BUILD);
#endif
#ifdef	NC_DEBUG_BUILD
		DEFINE_CONFIG(NC_DEBUG_BUILD);
#endif
#ifdef 	KV_TRACE
		DEFINE_CONFIG(KV_TRACE);
#endif
#ifdef 	UNLIMIT_MAX_INODES
		DEFINE_CONFIG(UNLIMIT_MAX_INODES)
#endif
#ifdef 	EXPERIMENTAL_USE_SLOCK
		DEFINE_CONFIG(EXPERIMENTAL_USE_SLOCK)
#endif
#ifdef 	NC_DEBUG_PATHLOCK
		DEFINE_CONFIG(NC_DEBUG_PATHLOCK)
#endif
#ifdef 	NC_ENABLE_WRITE_VECTOR
		DEFINE_CONFIG(NC_ENABLE_WRITE_VECTOR);
#endif
#ifdef	NC_ENABLE_CRC
		DEFINE_CONFIG(NC_ENABLE_CRC);
#endif
#ifdef	NC_PASS_BLOCK_WQ
		DEFINE_CONFIG(NC_PASS_BLOCK_WQ);
#endif
#ifdef	NC_LAZY_BLOCK_CACHE
		DEFINE_CONFIG(NC_LAZY_BLOCK_CACHE);
#endif
#ifdef	NC_MULTIPLE_PARTITIONS
		DEFINE_CONFIG(NC_MULTIPLE_PARTITIONS);
#endif

#ifdef 	NC_ENABLE_MDB_WAIT
		DEFINE_CONFIG(NC_ENABLE_MDB_WAIT);
#endif
#ifdef 	NC_MEASURE_CLFU
		DEFINE_CONFIG(NC_MEASURE_CLFU);
#endif
#ifdef 	NC_MEASURE_PATHLOCK
		DEFINE_CONFIG(NC_MEASURE_PATHLOCK);
#endif
#ifdef 	NC_MEASURE_MUTEX
		DEFINE_CONFIG(NC_MEASURE_MUTEX);
#endif
#ifdef 	NC_DEBUG_CLFU
		DEFINE_CONFIG(NC_DEBUG_CLFU);
#endif
#ifdef 	NC_ENABLE_LAZY_HEADER_SYNC
		DEFINE_CONFIG(NC_ENABLE_LAZY_HEADER_SYNC);
#endif
#ifdef 	PATHLOCK_USE_RWLOCK
		DEFINE_CONFIG(PATHLOCK_USE_RWLOCK);
#endif
#ifdef 	NC_VOLUME_LOCK
		DEFINE_CONFIG(NC_VOLUME_LOCK);
#endif

		n 		= sprintf(pcap, "Max_Path_Lock=%d", g__pl_poolsize);
		pcap 	+= n;
		*pcap 	= '\0';
		TRACE((T_MONITOR, "NetCache Build Params:{%s}\n", zcap));
	}
	

	PROFILE_CHECK(t_e);
#ifdef __PROFILE
	TRACE((T_INFO, "NetCache took %.2f msec to be online\n", PROFILE_GAP_MSEC(t_b, t_e)));
#endif
	return 0;
}


int PUBLIC_IF
nc_load_driver(char *name, char *path)
{
	int		r;
	
	r =  ((cfs_load_driver(name, path) != NULL)? 1: -1);
	TRACE((T_API, "('%s', '%s') = %d(nc_errno=%d)\n", name, path, r, __nc_errno));
	
	return r;
}

void __cfs_dump_clfu(int sig)
{
	clfu_dump(g__blk_cache, NULL, 1);
}

#if 0
void __cfs_toggle_debug(int sig)
{
	g_debugmask = g_maskarray[0];
	g_maskidx = (g_maskidx + 1) % 6;
	g_debugmask |= g_maskarray[g_maskidx];
	TRC_DEPTH((g_debugmask));
}

void reload_mask(int sig)
{
	char *env;
	long m = 0;

	env = getenv("DEBUG_MASK");
	if (env) {
		fprintf(stderr, "TRACE MASK is '%s'\n", env);
		while (*env && strchr("0123456789", *env)) {
			m = 10 * m + *env - '0';
		}
	}
	fprintf(stderr, "TRACE MASK changed to 0x%04X\n", m);
}

pid_t g_cid = 0;
time_t last_launching = 0;
int too_fast = 0;

void sig_error(int code)
{
	TRACE((T_ERROR, "signal %d caught!!!!\n", code));
	kill(g_cid, SIGKILL);
	exit(0);
}

void init_signal()
{
	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, sig_error);
	signal(SIGILL, sig_error);
	signal(SIGFPE, sig_error);
	signal(SIGPIPE, SIG_IGN);

	signal(SIGTERM, sig_error);
	signal(SIGRTMIN, sig_error);

}
#endif								/* end of function */

static char __z_ioctl[NC_IOCTL_MAX][48] = {
	"NC_IOCTL_PRESERVE_CASE:5000",
	"NC_IOCTL_STORAGE_CHARSET:5001",
	"NC_IOCTL_LOCAL_CHARSET:5002",
	"NC_IOCTL_STORAGE_TIMEOUT:5003",
	"NC_IOCTL_STORAGE_PROBE_INTERVAL:5004",
	"NC_IOCTL_STORAGE_PROBE_COUNT:5005",
	"NC_IOCTL_WRITEBACK_BLOCKS:5006",
	"NC_IOCTL_STORAGE_RETRY:5007",
	"NC_IOCTL_WEBDAV_USE_PROPPATCH:5008",
	"NC_IOCTL_WEBDAV_ALWAYS_LOWER:5009",
	"NC_IOCTL_NEGATIVE_TTL:5010",
	"NC_IOCTL_POSITIVE_TTL:5011",
	"NC_IOCTL_ADAPTIVE_READAHEAD:5012",
	"NC_IOCTL_FRESHNESS_CHECK:5013",
	"NC_IOCTL_HTTP_FILTER:5014",
	"NC_IOCTL_SET_VALIDATOR:5015",
	"NC_IOCTL_CACHE_MONITOR:5016",
	"NC_IOCTL_PROXY:5017",
	"NC_IOCTL_CACHE_VALIDATOR:5018",
	"NC_IOCTL_ORIGIN_MONITOR:5019",
	"NC_IOCTL_FOLLOW_REDIRECTION:5020",
	"NC_IOCTL_ORIGIN_MONITOR_CBD:5021",
	"NC_IOCTL_USE_HEAD_FOR_ATTR:5022",
	"NC_IOCTL_UPDATE_ORIGIN:5023",
	"NC_IOCTL_SET_IO_VALIDATOR:5024",
	"NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER:5025",
	"NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER:5026",
	"NC_IOCTL_SET_LAZY_PROPERTY_UPDATE:5027",
	"NC_IOCTL_SET_ORIGIN_PHASE_REQ_CBDATA:5028",
	"NC_IOCTL_SET_ORIGIN_PHASE_RES_CBDATA:5029",
	"NC_IOCTL_SET_ORIGIN_PATH_CHANGE_CALLBACK:5030",
	"NC_IOCTL_SET_MAKE_REDIR_ABSOLUTE:5031",
	"NC_IOCTL_SET_LB_ORIGIN_POLICY:5032",
	"NC_IOCTL_SET_LB_PARENT_POLICY:5033",
	"NC_IOCTL_UPDATE_PARENT:5034",
	"NC_IOCTL_CONNECT_TIMEOUT:5035",
	"NC_IOCTL_TRANSFER_TIMEOUT:5036",
	"NC_IOCTL_HTTPS_SECURE:5037",
	"NC_IOCTL_HTTPS_CERT:5038",
	"NC_IOCTL_HTTPS_CERT_TYPE:5039",
	"NC_IOCTL_HTTPS_SSLKEY:5040",
	"NC_IOCTL_HTTPS_SSLKEY_TYPE:5041",
	"NC_IOCTL_HTTPS_CRLFILE:5042",
	"NC_IOCTL_HTTPS_CAINFO:5043",
	"NC_IOCTL_HTTPS_CAPATH:5044",
	"NC_IOCTL_HTTPS_FALSESTART:5045",
	"NC_IOCTL_HTTPS_TLSVERSION:5046",
	"NC_IOCTL_SET_OFFLINE_POLICY_FUNCTION:5047",
	"NC_IOCTL_SET_OFFLINE_POLICY_DATA:5048",
	"NC_IOCTL_SET_LB_FAIL_COUNT_TO_OFFLINE:5049",
	"NC_IOCTL_SET_DEFAULT_READ_SIZE:5050",
	"NC_IOCTL_SET_DNS_CACHE_TIMEOUT:5051",
	"NC_IOCTL_SET_NCREAD_TIMEOUT:5052"
};


/*
 * NetCache Manager APIs
 */


PUBLIC_IF CALLSYNTAX int   
nc_ioctl(nc_volume_context_t *volume, int cmd, void *val, int vallen)
{
	int 	result = 0;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float			d1=0;
#endif
	clrtid();
	

	/*
	 * shared-lock으로 시작
	 * 필요하면 xlock으로 upgrade
	 */
	PROFILE_CHECK(ps);
	VOLUME_OP_BEGIN(volume, U_FALSE, U_TRUE, -ENODEV);
	__nc_errno = 0;

	_ATOMIC_ADD(g__pending_inodeop, 1);
	result = nvm_ioctl(volume, cmd, val, vallen);
	if (result < 0)
		__nc_errno = -result;
	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif
	TRACE((T_API|T_API_VOLUME, "('%s', %d[%s], %p, %d) = %d(nc_errno=%d) %.2f msec elapsed\n", (volume?volume->signature:"NULL"), cmd, __z_ioctl[cmd - NC_IOCTL_BASE], val, vallen, result, __nc_errno, d1));
	_ATOMIC_SUB(g__pending_inodeop, 1);

	VOLUME_OP_END(volume, result);

}
/*
 *  @drvclass - name of origin storage driver
 * 	@prefix - on Windows, it is always '\0'.
 */
PUBLIC_IF CALLSYNTAX nc_volume_context_t * 
nc_create_volume_context(	char *drvname,  
							char *prefix,
							nc_origin_info_t *oi,
							int oicnt)
{
	nc_volume_context_t *m;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	long long		d=0;
#endif
	clrtid();
	
	__nc_errno = 0;
	DO_PROFILE_USEC(ps, pe, d) {
		m = nvm_create(drvname,  prefix, oi, oicnt);
	}
	if (!m) __nc_errno = errno;

	TRACE((T_API|T_API_VOLUME, "('%s', '%s', %p, %d) = %p(nc_errno=%d, %.2f msec(s) elapsed)\n", drvname, prefix, oi, oicnt, m, __nc_errno, (float)(d/1000.0)));
	
	return m;
}
#if 0
PUBLIC_IF CALLSYNTAX nc_volume_context_t * 
nc_create_mount_context(	const char *drvname,  
							const char *prefix,
							nc_origin_info_t *oi,
							int oicnt)
{
	return nc_create_volume_context(	drvname, prefix, oi, oicnt);
	
}
#endif

PUBLIC_IF CALLSYNTAX int  
nc_destroy_volume_context(nc_volume_context_t *volume)
{

	int		r;
	char	signature[1024]="";
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float			d=0;
#endif

	strcpy(signature, volume->signature);
		
	
	__nc_errno = 0;
	DO_PROFILE_USEC(ps, pe, d) {
		r = nvm_destroy(volume, U_TRUE); /* sync */
	}
	TRACE((T_API|T_API_VOLUME, "('%s') = %d(nc_errno=%d, %.2f msec(s) elapsed)\n", signature, r, __nc_errno, (float)(d/1000.0)));
	
	return r;
}
PUBLIC_IF CALLSYNTAX int
nc_ref_count(nc_file_handle_t *fi)
{
	int		r;
	
	__nc_errno = 0;
	r = (NC_VALID_INODE_2(fi->inode)?INODE_GET_REF(fi->inode):0);
	TRACE((T_API, "(INODE[%d]) = %d(errno=%d)\n", NC_INODE_ID(fi), r, __nc_errno));

	return r;
}
#if 0
PUBLIC_IF CALLSYNTAX nc_file_handle_t *  
nc_handle_of(nc_volume_context_t *volume, const char *path) 
{
	nc_file_handle_t *h;
	
	__nc_errno = 0;
	h = dm_lookup_inode(volume, DM_LOOKUP_WITH_KEY, path, NULL, U_FALSE, U_FALSE);
	if (!h) __nc_errno = EBADF;
	TRACE((T_API, "returns %p(errno=%d)\n", h, __nc_errno));
	
}
#endif
PUBLIC_IF CALLSYNTAX nc_file_handle_t *  
nc_open(nc_volume_context_t *volume, char *path, int mode)
{
	nc_file_handle_t		*handle = NULL;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float			d1=0;
#endif

	clrtid();

	PROFILE_CHECK(ps);

	VOLUME_OP_BEGIN(volume,  U_FALSE, U_TRUE, NULL);
	
	__nc_errno = 0;
	TRACE((0, "NC_OPEN(%s) - BEGIN\n", path));
	if ( nc_check_path_limit(path) < 0) {
		__nc_errno = errno = ENAMETOOLONG;
		
		TRACE((T_API|T_API_OPEN, "('%s', '%s', 0x%08X) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), path, mode, NULL, __nc_errno));
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if ( __terminating )  {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_API|T_API_OPEN, "('%s', '%s', 0x%08X) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), path, mode, NULL, __nc_errno));
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if (! nvm_is_online2(volume)) {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_WARN|T_API|T_API_OPEN, "('%s', '%s', 0x%08X) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), path, mode, NULL, __nc_errno));
		VOLUME_RETURN_ERROR(volume, NULL);
	}


	_ATOMIC_ADD(g__pending_open, 1);
	handle = fio_open(volume, path, path, mode, NULL);
	mavg_update(g__openi, (double)d1/1000.0);

	_ATOMIC_SUB(g__pending_open, 1);

	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps,pe);
#endif
	
	TRACE((T_API|T_API_OPEN, "('%s', '%s', 0x%08X) = INODE[%d](VREF=%d) %.2f msec elapsed\n", 
							(volume?volume->signature:"NULL"), 
							path, 
							mode, 
							NC_INODE_ID(handle),
							GET_ATOMIC_VAL(volume->refcnt)-1,
							(float)d1
							));

	DEBUG_ASSERT(handle == NULL || handle && (dm_inode_check_owned(handle->inode) == 0));

	VOLUME_OP_END(volume, handle); /* unref해도 이전에 volume_ref()해둠*/

}

PUBLIC_IF CALLSYNTAX nc_file_handle_t *  
nc_open_extended(nc_volume_context_t *volume, char *cache_id, char *path, int mode, nc_xtra_options_t *kv)
{
	nc_file_handle_t		*handle = NULL;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float		d1=0;
#endif


	
	PROFILE_CHECK(ps);
	VOLUME_OP_BEGIN(volume,  U_FALSE, U_TRUE, NULL);

	__nc_errno = 0;
	TRACE((0, "NC_OPEN(%s) - BEGIN\n", path));
	if ( nc_check_path_limit(path) < 0) {
		__nc_errno = errno = ENAMETOOLONG;
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), cache_id, path, mode, kv, NULL, __nc_errno));
		
		VOLUME_RETURN_ERROR(volume, NULL);
		return NULL;
	}
	if ( __terminating )  {
		__nc_errno = errno = EINVAL;
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), cache_id, path, mode, kv, NULL, __nc_errno));
		
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if (strlen(path) > NC_VISIBLE_MAXPATHLEN)  {
		__nc_errno = errno = ENAMETOOLONG;
		
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), cache_id, path, mode, kv, NULL, __nc_errno));
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if (! nvm_is_online2(volume)) {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_WARN|T_API|T_API_OPEN, "('%s', '%s', 0x%08X) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), path, mode, NULL, __nc_errno));
		VOLUME_RETURN_ERROR(volume, NULL);
	}


	_ATOMIC_ADD(g__pending_open, 1);
	handle = fio_open(volume, cache_id, path, mode, kv);
	mavg_update(g__openi, (double)d1/1000.0);

		//ASSERT_FILE(handle->inode, (dm_inode_check_owned(handle->inode) == 0));
		//dm_inode_unlock_all(handle->inode);
	_ATOMIC_SUB(g__pending_open, 1);


	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps,pe);
#endif
	TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p) = INODE[%d](VREF=%d) %.2f msec elapsed\n", 
							(volume?volume->signature:"NULL"), 
							cache_id, 
							path, 
							mode, 
							kv, 
							NC_INODE_ID(handle),
							GET_ATOMIC_VAL(volume->refcnt)-1,
							(float)d1
							));
	
	DEBUG_ASSERT(handle == NULL || handle && (dm_inode_check_owned(handle->inode) == 0));
	VOLUME_OP_END(volume, handle);


}

/*
 * volume의 online여부와 무관하게 실행되록
 * 현상태 유지 - nc_open_XXX에서 volume의 refcnt가 이미
 * 증가된 상태에므로 안전하게 nc_read 실행 가능
 */
PUBLIC_IF CALLSYNTAX nc_ssize_t  
nc_read(nc_file_handle_t *fi, char *buf, nc_off_t off, nc_size_t len)
{
	nc_ssize_t		s  = 0;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float			d1;
#endif
	nc_time_t		t_s;

	clrtid();


	PROFILE_CHECK(ps);
	t_s = nc_cached_clock();
	VOLUME_OP_BEGIN(fi->volume,  U_FALSE, U_TRUE, -ENODEV);
	//VOLUME_REF(fi->volume); 

	__nc_errno = 0;
	if ( __terminating )  {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), buf, off, len, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(fi->volume, -__nc_errno);
	}
	if (!NC_VALID_OFFSET(off+len)) {
		errno = __nc_errno =  EINVAL;
		
		TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), buf, off, len, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(fi->volume, -__nc_errno);
	}
	_ATOMIC_ADD(g__pending_io, 1);

	if (off < 0) {
		_ATOMIC_SUB(g__pending_io, 1);
		__nc_errno = errno = EINVAL;
		
		TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), buf, off, len, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(fi->volume, -__nc_errno);
	}

	DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);

	s = fio_read(fi, buf, off, len);

	DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);
#if 0
	if (s > 0) {
		nc_volume_context_t 	*vol = fi->inode->volume;
		/*
		 * 요청의 크기 기준, 실제 읽기 기준아님
		 */
		vol->tot_readsiz_1m = _ATOMIC_ADD(vol->tot_readsiz_1m, len);
		vol->tot_readcnt_1m = _ATOMIC_ADD(vol->tot_readcnt_1m, 1);
	}
#endif

	_ATOMIC_SUB(g__pending_io, 1);
	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif 

	if (((nc_cached_clock() - t_s) > fi->volume->to_ncread) && ( s !=  len))
		__nc_errno = ETIMEDOUT;
	
	TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld) = %d(nc_errno=%d) elapsed %.2f msec\n", NC_INODE_ID(fi), buf, off, len, s, __nc_errno, d1));

	VOLUME_OP_END(fi->volume, s);
	//VOLUME_UNREF(fi->volume);
}


PUBLIC_IF CALLSYNTAX nc_ssize_t  
nc_read_apc(nc_file_handle_t *fi, char *buf, nc_off_t off, nc_size_t len, nc_apc_read_callback_t callback, void *callbackdata)
{
	nc_ssize_t		s  = 0;
	nc_volume_context_t 	*volume = fi->volume;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float		d1=0;
#endif

	clrtid();


	
	PROFILE_CHECK(ps);
	__nc_errno = 0;
	VOLUME_OP_BEGIN(fi->volume,  U_FALSE, U_TRUE, -ENODEV);

	if ( __terminating )  {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld, %p, %p) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), buf, off, len, callback, callbackdata, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(fi->volume, -__nc_errno);
	}
	if (!NC_VALID_OFFSET(off+len)) {
		errno = __nc_errno =  EINVAL;
		
		TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld, %p, %p) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), buf, off, len, callback, callbackdata, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(fi->volume, -__nc_errno);

	}
	if (fi->inode == NULL) {
		__nc_errno = EBADF;
		
		TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld, %p, %p) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), buf, off, len, callback, callbackdata, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(fi->volume, -__nc_errno);
	}
	if (off < 0) {
		__nc_errno = errno = EINVAL;

		TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld, %p, %p) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), buf, off, len, callback, callbackdata, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(fi->volume, -__nc_errno);
	}

	_ATOMIC_ADD(g__pending_io, 1);
	DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);

	s = fio_read_apc(fi, buf, off, len, callback, callbackdata);

	DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);
	if (__nc_errno != EWOULDBLOCK) {
		/*
		 * 아래 카운터도 역시 fio_read_apc_blk_prepared()에서 완료시 실행됨
		 */
		_ATOMIC_SUB(g__pending_io, 1);
	}
	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif
	
	TRACE((T_API|T_API_READ, "(INODE[%d], %p, %lld, %lld, %p, %p) = %d %.2f msec elapsed(nc_errno=%d)\n", 
							NC_INODE_ID(fi), 
							buf, 
							off, 
							len, 
							callback, 
							callbackdata, 
							s, 
							(float)d1, 
							__nc_errno));
	DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);

	VOLUME_OP_END(fi->volume, s);
}
PUBLIC_IF CALLSYNTAX nc_ssize_t  
nc_write(nc_file_handle_t *fi, char *buf, nc_off_t off, size_t len)
{
	nc_ssize_t		s = -1;
#if 0
#ifdef __PROFILE
	float		d1=0,d2=0;
#endif
	fc_inode_t 		*inode = fi->inode;
	nc_volume_context_t 	*volume = NULL;

	
	__nc_errno = 0;
	if ( __terminating )  {
		errno = __nc_errno  = EINVAL;
		TRACE((T_API|T_API_WRITE, "('%s', %lld, %lld) (terminating) returns %d(errno=%d)\n", 
					(char *)(inode?inode->q_path:"nul"), 
					(long long)off, 
					(long long)len, 
					-__nc_errno,
					__nc_errno));
		
		return -__nc_errno;
	}
	if (inode && inode->volume && !nvm_is_online(inode->volume, NCOF_WRITABLE)) {
		errno = __nc_errno =  EINVAL;
		TRACE((T_API|T_API_WRITE, "('%s', %lld, %lld) (terminating) returns %d(errno=%d)\n", 
					(char *)(fi?inode->q_path:"nul"), 
					(long long)off, 
					(long long)len, 
					__nc_errno));
		
		return -__nc_errno;
	}
	if (!NC_VALID_OFFSET(off+len)) {
		errno = __nc_errno =  EINVAL;
		TRACE((T_API|T_API_WRITE, "('%s', %lld, %lld) (invalid value) returns %d(errno=%d)\n", 
					(char *)(fi?inode->q_path:"nul"), 
					(long long)off, 
					(long long)len, 
					__nc_errno));
		
		return -__nc_errno;

	}
	volume = inode->volume;

	_ATOMIC_ADD(g__pending_io, 1);
	_ATOMIC_ADD(g__write_bytes, len);
	_ATOMIC_ADD(g__write_count, 1);


		s = blk_write(inode, (char *)buf, off, len, NULL);

#ifdef __PROFILE
	if ((d1+d2) > 5000000) {
		TRACE((T_PERF, "INODE[%u]/REF[%d]: offset %lld, req.sz[%lld] : %lld bytes slow(d1:%.2f msec,d2:%.2f msec)\n",
				(int)NC_INODE_ID(fi),
				inode->refcnt, 
				off,
				len, s, 
				(float)(d1/1000.0),
				(float)(d2/1000.0)
				));
	}
#endif
	TRACE((T_INODE, "nc_write(INODE[%u], %lld, %ld) - %ld DONE\n", inode->uid, off, (long)len, (long)s));
	_ATOMIC_SUB(g__pending_io, 1);
	TRACE((T_API|T_API_WRITE, "('%s', %lld, %lld)/INODE[%u] returns %d(errno=%d)(size=%lld, fr_size=%lld)\n", 
					(char *)(fi?inode->q_path:"nul"), 
					(long long)off, 
					(long long)len, 
					(int)NC_INODE_ID(fi),
					(int)s,
					(int)__nc_errno,
					inode->size,
					inode->fr_availsize));
	
#endif
	return s;
}


PUBLIC_IF CALLSYNTAX int  
nc_close(nc_file_handle_t *fi, int force)
{
	int 				r;
#ifdef __PROFILE
	perf_val_t 			ps, pe;
	float			d1=0;
#endif
	int					tid;
	char				volsig[128]="";
	char				*key = NULL;

	nc_volume_context_t 	*volume = fi->volume;


	clrtid();

	
	__nc_errno = 0;


	
	PROFILE_CHECK(ps);

	VOLUME_OP_BEGIN(volume,  U_FALSE, U_FALSE, -ENODEV);


	if (!fi) {
		TRACE((T_ERROR, "********* NULL netcache file handle\n"));
#ifndef NC_RELEASE_BUILD
		TRAP;
#endif
		__nc_errno = errno = EBADF;
		
		TRACE((T_ERROR|T_API|T_API_CLOSE, "(INODE[%d], %d) = %d(nc_errno=%d)\n", NC_INODE_ID(fi), force, -__nc_errno, __nc_errno));
		VOLUME_RETURN_ERROR(volume, -__nc_errno);
	}

	if (TRACE_ON(T_API_OPEN|T_API))
		strcpy(volsig, fi->inode->volume->signature);
	if (TRACE_ON(T_API_OPEN|T_API)) {
		key = alloca(strlen(fi->inode->q_id) + 1);
		strcpy(key,  fi->inode->q_id);
	}

	
	_ATOMIC_ADD(g__pending_close, 1);
	tid = NC_INODE_ID(fi);  /* close이후는 memory free됨 */


	r = fio_close(fi, force);
	mavg_update(g__closei, (double)d1/1000.0);

	if (r == 0) {
		_ATOMIC_SUB(g__pending_close, 1);
	}

	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif
	TRACE((T_API|T_API_CLOSE, "(INODE[%d], %d) = %d (volume='%s', Key='%s')(VREF=%d) %.2f msec elapsed\n", 
								tid, 
								force, 
								r, 
								volsig, 
								key,
								GET_ATOMIC_VAL(volume->refcnt),
								(float)d1
								));

	VOLUME_OP_END(volume, 0);
}
PUBLIC_IF CALLSYNTAX int
nc_purge(nc_volume_context_t *volume, char *pattern, int iskey, int ishard)
{
	int 	cnt;

	/* NEW 2018-10-16 huibong purge 요청에 대한 수행시간 로깅을 위한 기능 추가 (#32386) */
#ifdef __PROFILE
	perf_val_t time_start, time_end;
	float				d;
#endif

	PROFILE_CHECK(time_start);
	VOLUME_OP_BEGIN(volume,  U_FALSE, U_TRUE, -ENODEV);

	__nc_errno = 0;
	if (strlen(pattern) > NC_VISIBLE_MAXPATHLEN)  {
		__nc_errno  = ENAMETOOLONG;
		
		TRACE((T_API|T_API_PURGE, "('%s', '%s', %d, %d) = %d\n", (volume?volume->signature:"NULL"), pattern, iskey, ishard, -__nc_errno));
		VOLUME_RETURN_ERROR(volume, -__nc_errno);
	}

	/* CHG 2018-10-16 huibong 앞의 legnth check 와 코드 중복으로 인한 정리 (#32386) */
	/*
	if ( nc_check_path_limit(pattern) < 0 ) {
		__nc_errno = errno = ENAMETOOLONG;
		
		TRACE((T_API|T_API_CLOSE, "('%s', '%s', %d, %d) = %d\n", (volume?volume->signature:"NULL"), pattern, iskey, ishard, -__nc_errno));
		VOLUME_RETURN_ERROR(volume, -__nc_errno);
	}
	*/


	_ATOMIC_ADD(g__pending_purge, 1);
#ifndef WIN32
	if ((strcmp(pattern, "/*") == 0) && (ishard == 0)) {
		/*
		 * only for soft volume purge operation !!!
		 */
		cnt = nvm_purge_volume(volume);
	}
	else {
		cnt = nvm_purge(volume, pattern, ishard, iskey);;
	}
#endif
	_ATOMIC_SUB(g__pending_purge, 1);


#ifdef __PROFILE
	PROFILE_CHECK(time_end);
	d = PROFILE_GAP_MSEC(time_start, time_end);
#endif

	/* CHG 2018-10-16 huibong PURGE 최종 응답 관련 log level, 수행시간, 결과 항목 추가 (#32386) */
	TRACE((T_API|T_API_PURGE, "('%s', '%s', %d, %d) = %d(%.2f msecs elapsed)\n", (volume?volume->signature:"NULL"), pattern, iskey, ishard, cnt, d));
	VOLUME_OP_END(volume, cnt);
}

PUBLIC_IF CALLSYNTAX int 
nc_getattr(nc_volume_context_t *volume, char *path, nc_stat_t *s)
{
	int 					r;
#ifdef __PROFILE
	perf_val_t 	_s, _e;
	float 	d = 0;
#endif

	
	VOLUME_OP_BEGIN(volume,  U_FALSE, U_TRUE, -ENODEV);

	__nc_errno = 0;
	TRACE((0, "NC_GETATTR(%s) - BEGIN\n", path));
	if ( __terminating )  {
		__nc_errno = EINVAL;
		
		TRACE((T_API|T_API_GETATTR, "('%s', '%s', %p) = %d\n", (volume?volume->signature:"NULL"), path, s, -__nc_errno));
		VOLUME_RETURN_ERROR(volume, -__nc_errno);
	}
	if ( nc_check_path_limit(path) < 0 ) {
		__nc_errno = errno = ENAMETOOLONG;
		
		TRACE((T_API|T_API_GETATTR, "('%s', '%s', %p) = %d\n", (volume?volume->signature:"NULL"), path, s, -__nc_errno));
		VOLUME_RETURN_ERROR(volume, -__nc_errno);
	}

	_ATOMIC_ADD(g__pending_stat, 1);
	DO_PROFILE_USEC(_s, _e, d) {
		r = fio_getattr(volume, path, s);
	}
	_ATOMIC_SUB(g__pending_stat, 1);
	
	__nc_errno = ((r < 0)?-r:0);

	TRACE((T_API|T_API_GETATTR, "('%s', '%s', %p) = %d\n", (volume?volume->signature:"NULL"), path, s, r));
	VOLUME_OP_END(volume, r);
}
PUBLIC_IF CALLSYNTAX int 
nc_getattr_extended(nc_volume_context_t *volume, char *cache_id, char *path, nc_stat_t *s, nc_xtra_options_t *kv, mode_t mode)
{
	int 					r;
#ifdef __PROFILE
	perf_val_t 	_s, _e;
	float 	d = 0;
#endif
	nc_hit_status_t 	hs;

	
	VOLUME_OP_BEGIN(volume,  U_FALSE, U_TRUE, -ENODEV);
	__nc_errno = 0;
	TRACE((0, "NC_GETATTR(%s) - BEGIN\n", path));
	if ( __terminating )  {
		__nc_errno = EINVAL;
		
		TRACE((T_API|T_API_GETATTR, "('%s', '%s', '%s', %p, %p, 0X%08X) = %d\n", (volume?volume->signature:"NULL"), cache_id, path, s, kv, mode, -__nc_errno));
		VOLUME_RETURN_ERROR(volume, -__nc_errno);
	}
	if ( nc_check_path_limit(path) < 0 ) {
		__nc_errno = errno = ENAMETOOLONG;
		
		TRACE((T_API|T_API_GETATTR, "('%s', '%s', '%s', %p, %p, 0X%08X) = %d\n", (volume?volume->signature:"NULL"), cache_id, path, s, kv, mode, -__nc_errno));
		VOLUME_RETURN_ERROR(volume, -__nc_errno);
	}


	_ATOMIC_ADD(g__pending_stat, 1);
	DO_PROFILE_USEC(_s, _e, d) {
		r = dm_inode_stat(volume, cache_id, path, s, NULL, NULL, kv, mode, &hs, U_TRUE);
	}
	_ATOMIC_SUB(g__pending_stat, 1);
	
	__nc_errno = ((r < 0)?-r:0);

	
	TRACE((T_API|T_API_GETATTR, "('%s', '%s', '%s', %p, %p, 0X%08X) = %d\n", (volume?volume->signature:"NULL"), cache_id, path, s, kv, mode, r));
	VOLUME_OP_END(volume, r);
}
PUBLIC_IF CALLSYNTAX int   
nc_set_param(int cmd, void *val, int vallen)
{
	int 	result = 0;
	int 	v;

	// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	static int	_premature_init = 0;
	void bcm_setup_page_size();


	clrtid();

	
	nc_update_cached_clock();
	__nc_errno = 0;

	// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	if( !_premature_init ) {
		bcm_setup_page_size();
		_premature_init = 1;
	}

	switch (cmd) {
		case NC_GLOBAL_ENABLE_COLD_CACHING:
			g__enable_cold_caching = ((*(int *)val) != 0);
			break;
		case NC_GLOBAL_CLUSTER_IP:
			strcpy(g__cluster_ip, (char *)val);
			break;
		case NC_GLOBAL_LOCAL_IP: 
			strcpy(g__local_ip, (char *)val);
			break;
		case NC_GLOBAL_MAX_PATHLOCK:
			g__pl_poolsize = *(int *)val;
			break;
		case NC_GLOBAL_CLUSTER_TTL:
			g__cluster_ttl = *(int *)val;
			break;
		case NC_GLOBAL_ASIO_TIMER: {
				int 	tval = 0;
				tval = *(int *)val;
				__enable_asio_timer = tval;
			}
			break;
		case NC_GLOBAL_BLOCK_SPIN_INTERVAL: {
				int 	tval = 0;
				tval = *(int *)val;
				if (tval >= 60 && tval <= (24*3600)) {
					g__block_spin_interval = tval;
				}
				else {
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
			}
			break;
#if 0
		case NC_GLOBAL_ENABLE_COMPACTION: {
				int 	tval = 0;
				tval = *(int *)val;
				if (vallen > 0) {
					__enable_compaction = tval;
				}
				else {
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
			}
			break;
#endif /* ir #25835 : 고정값으로 변경. 수정 불가 */
		case NC_GLOBAL_STRICT_CRC: {
				int 	tval = 0;
				tval = *(int *)val;
				g__strict_crc_check = tval;
			}
			break;
		case NC_GLOBAL_CACHE_RA_MAXTHREADS: {
				int 		tval;
				tval = *(int *)val;

				// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
				//                - 원부사장님 수정 소스 적용
				//                - default 값 256 -> 512 변경에 따라 최대값을 300 -> 512 로 변경 처리.
				if  (tval < 0 || tval > 512) {
					TRACE((T_ERROR, "max # of eead-ahead threads should be inbetween 1 and 512\n"));
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
				else {
					__max_asio_threads = tval;
				}
			}
			break;
		case NC_GLOBAL_CACHE_BLOCK_SIZE:
			__nc_block_size = *(int *)val*1024;

			// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603)
			//                - 원부사장님 수정 소스 적용
			//                - 설정값을 기준으로... g__page_size 값의 정수배율로 조정하도록 기능 추가.
			__nc_block_size = ( (__nc_block_size + g__page_size - 1) / g__page_size) * g__page_size;

			if (g__need_fastcrc > 0 && g__need_fastcrc >= __nc_block_size) {
				TRACE((T_WARN, "Fast CRC enabled but larger than the chunk size (%d), FastCRC changed to be disabled\n",
							__nc_block_size));
				g__need_fastcrc = 0;
			}
			break;
		case NC_GLOBAL_CACHE_RA_MINTHREADS: {
				int 		tval;
				tval = *(int *)val;
				if  (tval < 0 || tval > 64) {
					TRACE((T_ERROR, "min # of read-ahead  threads should be inbetween 1 and 64\n"));
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
				else {
					__min_asio_threads = tval;
				}
			}
			break;
		case NC_GLOBAL_CACHE_DISK_LOW_WM: {
				int 		tval;
				tval = *(int *)val;
				if  (tval >= 100) {
					TRACE((T_ERROR, "disk-reclaiming low water mark invalid, %d\n", tval));
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
				else {
					//extern int __dm_reclaim_lowmark;
					g__dm_reclaim_lowmark = tval;
				}
			}
			break;
		case NC_GLOBAL_CACHE_DISK_HIGH_WM: {
				int 		tval;
				tval = *(int *)val;
				if  (tval >= 100) {
					TRACE((T_ERROR, "disk-reclaiming high water mark invalid, %d\n", tval));
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
				else {
					//extern int __dm_reclaim_highmark;
					g__dm_reclaim_highmark = tval;
				}
			}
			break;
#if 0
		case NC_GLOBAL_CACHE_DISK_RA: {
				g__cache_ra_blk_cnt = tval;
			}
			break;
#endif
		case NC_GLOBAL_ENABLE_FORCECLOSE: {
					g__forceclose = *(int *)val;
				}
				break;
		case NC_GLOBAL_BACKUP_PROPERTYCACHE: {
					g__need_stat_backup = *(int *)val != 0;
				}
				break;
#if 0
		case NC_GLOBAL_CACHE_NETWORK_RA: {
				int 		tval;
				tval = *(int *)val;
				if  (tval < 0 || tval > 512) {
					TRACE((T_ERROR, "remote read-ahead  blocks should be inbetween 1 and 512\n"));
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
				else {
					g__max_asio_vector_count = max(g__max_asio_vector_count, tval);
					__remote_ra_blk_cnt = tval;
				}
			}
			break;
#endif
		case NC_GLOBAL_CACHE_MAX_ASIO: {
				int 		tval;
				tval = *(int *)val;
				if  (tval < 0 || tval > 2048) {
					TRACE((T_ERROR, "max concurrent async IO should be inbetween 1 and 2048\n"));
					result = -1;
				}
				else {
					__s__max_ios = tval;
				}
			}
			break;
		case NC_GLOBAL_CACHE_POSITIVE_TTL: {

				int 		tval;
				tval = *(int *)val;
				if  (tval <= 0 ) {
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
				else {
					__positive_TTL = tval;
				}
			}
			break;
		case NC_GLOBAL_CACHE_NEGATIVE_TTL: {
				int 		tval;
				tval = *(int *)val;
				if  (tval <= 0) {
					TRACE((T_ERROR, "Negative cache TTL should be inbetween 1 and %d secs\n", 24*3600));
					__nc_errno = EINVAL;
					result = -__nc_errno;
				}
				else {
					__negative_TTL = tval;
				}
			}
			break;
		case NC_GLOBAL_CACHE_PATH:
			strncpy(__cache_dir, (char *)val, vallen);
			break;
		case NC_GLOBAL_CACHE_MAX_INODE:
			g__inode_count = (long)*(int *)val;
			break;
		//case NC_GLOBAL_CACHE_MAX_EXTENT:
		//	ic_update_extent_count(g__inode_count, (long)*(int *)val);
		//	break;
		case NC_GLOBAL_CACHE_MEM_SIZE:
			g__cache_size = (long long) *(int *)val;
			break;
#ifndef WIN32 
		case NC_GLOBAL_DEFAULT_USER: 
			if (!getpwnam((char *)val)) {
				TRACE((T_WARN, "user name, '%s' not found in this system\n", (char *)val));
				__nc_errno = EINVAL;
				result = -__nc_errno;
			}
			else {
				strcpy(g__default_user, (char *)val);
			}
			break;
		case NC_GLOBAL_DEFAULT_GROUP: 
			if (!getgrnam((char *)val)) {
				TRACE((T_WARN, "group name, '%s' not found in this system\n", (char *)val));
				__nc_errno = EINVAL;
				result = -__nc_errno;
			}
			else {
				strcpy(g__default_group, (char *)val);
			}
			break;
#endif
		case NC_GLOBAL_COLD_RATIO: /* 캐싱된 객체 중 cold 객체 비율 */
			v = (int)*(int *)val;
			if (v < 0 || v > 100) {
				TRACE((T_ERROR, "This value should be between 0 and 100\n"));
			}
			else {
				g__cold_ratio = v;
			}
			break;
		case NC_GLOBAL_ENABLE_FASTCRC:
			v = (int)*(int *)val;
			if (v < 0 || v > __nc_block_size) { 
				TRACE((T_ERROR, "This value shoud be zero or positive value less than chunk size(chunk size=%d)\n", __nc_block_size));
				__nc_errno = EINVAL;
				result = -__nc_errno;
			}
			else if ((v % 4) != 0) {
				TRACE((T_ERROR, "This value shoud be multiple of 4\n"));
				__nc_errno = EINVAL;
				result = -__nc_errno;
			}
			else {
				g__need_fastcrc = v;

			}

			break;
		case NC_GLOBAL_STAT_BACKUP:
			g__need_stat_backup = *(int *)val;
			break;
		case NC_GLOBAL_READAHEAD_MB:
			g__readahead_MB 	= *(int *)val;
			break;
		case NC_GLOBAL_INODE_MEMSIZE:
			g__inode_heap_size = max(4, (long long)*(int *)val) * 1024LL * 1024LL ;
			break;
		case NC_GLOBAL_FALSE_READ_LIMIT:
			g__false_read_limit = (float) *(float *)val;
		default:
			TRACE((T_WARN, "command %d - unknown\n", cmd));
			__nc_errno = EINVAL;
			result = -__nc_errno;
			break;
	}

	

	TRACE((T_API, "(%d, %p, %d) = %d\n", cmd, val, vallen, result));
	return result;
}

PUBLIC_IF CALLSYNTAX void
nc_change_mask(nc_uint32_t mask)
{
	clrtid();
	trc_depth(mask);
	TRACE((T_WARN, "0.log mask change test output in WARN\n"));
	TRACE((T_TRACE, "1. log mask change test output in TRACE\n"));
	TRACE((T_DEBUG, "2. log mask change test output in DEBUG\n"));
	TRACE((T_ERROR, "3. log mask change test output in ERROR\n"));
	TRACE((T_CACHE, "4. log mask change test output in CACHE\n"));
	TRACE((T_INFO, "**** END OF TEST ****\n"));
}

PUBLIC_IF CALLSYNTAX int  
nc_setup_log(nc_uint32_t mask, char *path, nc_int32_t szMB, nc_int32_t rotate)
{
	LOG_HANDLE_t	*LH = NULL;

	nc_update_cached_clock();
	clrtid();
#ifdef NC_RELEASE_BUILD
	LH = _init_log(path, szMB*1024*1024, rotate);
#else
	LH = _init_log(path, 3*szMB*1024*1024, rotate*2);
#endif
	if (!LH) {
		return -1;
	}
	TRC_LOG_HANDLE((LH));
	TRC_DEPTH((mask));
	return 0;
}
#ifdef 	NC_HEAPTRACK
static void
LEAK(char *l)
{
	TRACE((T_MONITOR, "%s\n", l));
}
#endif
PUBLIC_IF CALLSYNTAX int  
nc_shutdown()
{
	int 		try = 5;
	int			itotal = 0;
	int			ifree = 0;

	
	__nc_errno = 0;
	TRACE((T_INFO, "freeing all resources..., wait\n"));
	TRACE((0, "NC_SHUTDOWN - BEGIN\n"));

	if (__terminating) {
		/* nc_shutdown called again */
		return 0;
		
	}
#ifdef NC_MEASURE_PATHLOCK
	nvm_report_pl_stat();
#endif
#ifdef NC_MEASURE_CLFU
	clfu_report_bl_stat();
#endif
	__terminating = 1;
	__terminating_timer = 1;
	TRACE((T_INFO, "Shutdown signalled\n"));
	
#if NOT_USED
	/* CHG 2018-07-04 huibong 미사용 전역변수, 로컬변수, loop 제거, return 구문 추가 (#32238)*/
	/*
	for (i = 0; i < __task_cnt; i++) {
		if (pthread_cancel(__tasks[i]) == 0) {
			pthread_join(__tasks[i], NULL);
		}
	}
	*/
#endif

#ifdef NEON_MEMLEAK
	ne_alloc_dump(stdout);
#endif
	TRACE((T_INFO, "freeing inodes\n"));
	dm_finish();
	g__inode_opened = dm_dump_or_count_inode_opened(T_ERROR, &itotal, &ifree, U_FALSE);
	while (g__inode_opened > 0 && try > 0) {
		TRACE((T_INFO, "In-use inode count  = %d/%d, waiting\n", g__inode_opened, itotal));
		bt_msleep(10);
		try--;
		g__inode_opened = dm_dump_or_count_inode_opened(T_ERROR, &itotal, &ifree, U_FALSE);
	}
	if (g__inode_opened > 0) {
		TRACE((T_ERROR, "%d inodes still opened, check your logic\n", g__inode_opened));
		dm_dump_or_count_inode_opened(T_ERROR, &itotal, &ifree, U_TRUE);
	}
	TRACE((T_INFO, "freeing buffers\n"));
	pthread_join(__tasks_timer, NULL);
	asio_shutdown();
	bcm_shutdown();
	dm_shutdown();
	nvm_free_all();
	pm_destroy();
	dio_shutdown();

	DUMP_RESOURCES;

	cfs_unload_all();

	__nc_dump_heap(AI_ALL);

	TRACE((T_INFO, "freeing all resources..., done ok\n"));
	TRACE((T_INFO, "Not-freed dynamic memory : %.2f MB\n", (float)(_ATOMIC_ADD(g__dynamic_memory_usage, 0)/1000000.0)));
	TRACE((0, "NC_SHUTDOWN - END\n"));

	return 0;
}
PUBLIC_IF CALLSYNTAX int
nc_add_partition(char *path, int weight)
{
	nc_part_element_t *p;

	p  = pm_add_partition(path, weight);

	return (p != NULL)?1:-1;
}

PUBLIC_IF CALLSYNTAX int
nc_enum_file_property(nc_file_handle_t *fi, int (*do_it)(char *key, char *val, void *cb), void *cb)
{
	return fio_for_each_property(fi, do_it, cb);
}
#if 0
PUBLIC_IF CALLSYNTAX int  
nc_invoke_io(nc_file_handle_t *fi)
{
	return fio_invoke_io(fi);
}
#endif
PUBLIC_IF CALLSYNTAX char *
nc_find_property(nc_file_handle_t *fi, char *tag)
{
	return fio_find_property(fi, tag);
}

PUBLIC_IF CALLSYNTAX nc_int32_t
nc_block_size()
{
	return __nc_block_size;
}
PUBLIC_IF CALLSYNTAX  int
nc_errno()
{
	return __nc_errno;
}
PUBLIC_IF CALLSYNTAX  int
nc_result_code(nc_file_handle_t * fi)
{
	return fio_result_code(fi);
}

void
nc_add_dm(nc_int32_t category, nc_int32_t sz)
{
#ifdef NC_MEM_DEBUG
		_ATOMIC_ADD(g__dynamic_memory_usage, sz);
		_ATOMIC_ADD(g__adynamic_memory_usage, sz);
#endif
	
}
void
nc_sub_dm(nc_int32_t category, nc_int32_t sz)
{
#ifdef NC_MEM_DEBUG
		_ATOMIC_SUB(g__dynamic_memory_usage, sz);
		_ATOMIC_SUB(g__adynamic_memory_usage, sz);
#endif
}

static int
nc_check_path_limit(const char *path)
{
	return (strlen(path) > NC_VISIBLE_MAXPATHLEN)? -1:0 ;
}

/*
 * 새로 solproxy를 위해 추가된 API
 * (2019.10.3일 현재 사용중인 open API)
 */
PUBLIC_IF CALLSYNTAX nc_file_handle_t *
nc_open_extended2(nc_volume_context_t *volume, char *cache_id, char *path, int mode, nc_stat_t *s, nc_kv_list_t *kv)
{
	nc_file_handle_t		*handle = NULL;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float		d1=0;
#endif


	clrtid();
	
	PROFILE_CHECK(ps);
	VOLUME_OP_BEGIN(volume, U_FALSE, U_TRUE, NULL);
	__nc_errno = 0;
	if ( nc_check_path_limit(path) < 0) {
		__nc_errno = errno = ENAMETOOLONG;
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p) = %p\n", (volume?volume->signature:"NULL"), cache_id, path, mode, s, kv, NULL));
		
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if ( __terminating )  {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p) = %p\n", (volume?volume->signature:"NULL"), cache_id, path, mode, s, kv, NULL));
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if (! nvm_is_online2(volume)) {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_WARN|T_API|T_API_OPEN, "('%s', '%s', 0x%08X) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), path, mode, NULL, __nc_errno));
		VOLUME_RETURN_ERROR(volume, NULL);
	}


	_ATOMIC_ADD(g__pending_open, 1);
	handle = fio_open_extended(volume, cache_id, path, mode, s, kv);
	mavg_update(g__openi, (double)d1/1000.0);

	_ATOMIC_SUB(g__pending_open, 1);

	PROFILE_CHECK(pe);
	
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif
	TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p) = INODE[%d](nc_errno=%d)(VREF=%d) elapsed %.2f msec\n", 
					(volume?volume->signature:"NULL"), 
					cache_id, 
					path, 
					mode, 
					s, 
					kv, 
					NC_INODE_ID(handle), 
					__nc_errno,
					GET_ATOMIC_VAL(volume->refcnt)-1,
					(float)d1
					));

	VOLUME_OP_END(volume, handle);
}
static void
nc_dump_param()
{
	char 	pbuf[2048]="";
	char 	vbuf[2048]="";
	//char 	zout[128]="";

	memset(pbuf, 'P', sizeof(pbuf));
	memset(vbuf, 'V', sizeof(vbuf));
#ifdef NC_RELEASE_BUILD
	snprintf(pbuf, sizeof(pbuf), "NetCache core global parameters: RELEASE VERSION\n"
#else
	snprintf(pbuf, sizeof(pbuf), "NetCache core global parameters: DEVELOPMENT VERSION\n"
#endif
				"\t\t - max in-memory caching objects : %ld \n"
				"\t\t - INODE heap size               : %.2f MB \n"
				"\t\t - chunk size(B)                 : %d\n"
				"\t\t - chunk CRC policy              : %d bytes (%s)\n"
				"\t\t - cache memory size(MB)         : %ld MB\n"
				"\t\t - access permission             : u='%s', g='%s'\n"
				"\t\t - async. force close            : %s\n"
				"\t\t - cold caching ratio(%%)        : %s (ratio=%d) \n"
				"\t\t - read-ahead(in MB)             : %d MB \n"
				"\t\t - working ASIO threads          : %d~%d\n"
				"\t\t - ASIO hang timeout             : '%s'\n"
				"\t\t - object caching TTL(sec)       : positive=%d, negative=%d\n"
				"\t\t - chunk data CRC check mode     : '%s'\n"
				"\t\t - primary caching partition     : '%s'\n"
				"\t\t - partition reclaiming ratio(%%) : %d~%d\n"
				"\t\t - clustering infomation         : %s (multicast='%s', local='%s', ttl=%d)\n"
				"\t\t - path-lock pool size           : %d\n",
				g__inode_count,
				(float)g__inode_heap_size/1000000.0,
				__nc_block_size,
				((g__need_fastcrc == 0)?__nc_block_size:g__need_fastcrc),
				((g__need_fastcrc == 0)?"disabled":"enabled"),
				g__cache_size,
				g__default_user, g__default_group,		
				(g__forceclose?"enabled":"disabled"),
				(g__enable_cold_caching?"enabled":"disabled"), g__cold_ratio,
				g__readahead_MB,
				__min_asio_threads, __max_asio_threads,
				(__enable_asio_timer?"enabled":"disabled"),
				__positive_TTL, __negative_TTL,
				(g__strict_crc_check?"strict":"loose") ,
				__cache_dir,
				g__dm_reclaim_lowmark, g__dm_reclaim_highmark,
				(isdigit(g__cluster_ip[0])?"enabled":"disabled"), g__cluster_ip, g__local_ip,g__cluster_ttl,
				(int)g__pl_poolsize
				);

	TRACE((T_INFO, "%s", pbuf));
	snprintf(pbuf, sizeof(pbuf),    "Copyright information:\n"
					"\t\t** Solbox NetCache Core engine version: (%s)\n"
					"\t\t** NetCache is Copyright(C) 2010-2012, Solbox,Inc. <support@solbox.com>\n"
					"\t\t** MD5 is Copyright(C) 1990, RSA Data Security, Inc. \n",
					nc_version(vbuf, sizeof(vbuf)));
	TRACE((T_MONITOR|T_INFO, "%s", pbuf));
}
PUBLIC_IF CALLSYNTAX int 
nc_fgetattr(nc_file_handle_t *fh, nc_stat_t *s)
{
	int 					r;
#ifdef __PROFILE
	perf_val_t 	ps, pe;
	float 	d1 = 0;
#endif

	clrtid();
	PROFILE_CHECK(ps);
	VOLUME_OP_BEGIN(fh->volume, U_FALSE, U_TRUE, -ENODEV);
	
	__nc_errno = 0;
	if ( __terminating )  {
		__nc_errno = EINVAL;

		TRACE((T_API|T_API_GETATTR, "(INODE[%d], %p) = %d(nc_errno=%d)\n", NC_INODE_ID(fh), s, -__nc_errno, __nc_errno));
		
		VOLUME_RETURN_ERROR(fh->volume, -__nc_errno);
	}


	_ATOMIC_ADD(g__pending_stat, 1);
	r = fio_fgetattr(fh, s);
	_ATOMIC_SUB(g__pending_stat, 1);
	
	__nc_errno = ((r < 0)?-r:0);

	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif
	TRACE((T_API|T_API_GETATTR, "(INODE[%d], '%s') = %d(st_size=%lld) (nc_errno=%d) elapsed %.2f msec\n", 
					NC_INODE_ID(fh),
					s,
					r,
					(long long)s->st_size,
					__nc_errno,
					d1));
	
	VOLUME_OP_END(fh->volume, r);
}
nc_int32_t
nc_inode_uid(nc_file_handle_t *fh)
{
	return (fh->inode?fh->inode->uid:-1);
}


static int
nc_get_revision()
{
	char 	*svnversion = "$Revision: 2344 $";

	return atol(svnversion+11);
}


PUBLIC_IF CALLSYNTAX int 
nc_close_allowable(nc_file_handle_t *fh)
{
	int 					r;
#ifdef __PROFILE
	perf_val_t 	ps, pe;
	float 	d1 = 0;
#endif
	char					volsig[128]="";

	clrtid();
	
	PROFILE_CHECK(ps);
	__nc_errno = 0;
	if ( __terminating )  {
		__nc_errno = EINVAL;
		TRACE((T_API, "(INODE[%d]) = %d(nc_errno=%d, volume='%s')\n", NC_INODE_ID(fh), -__nc_errno, __nc_errno, volsig));
		return -__nc_errno;
	}



	_ATOMIC_ADD(g__pending_stat, 1);
	if (TRACE_ON(T_API_OPEN|T_API))
		strcpy(volsig, fh->inode->volume->signature);

	r = fio_close_allowable(fh);
	_ATOMIC_SUB(g__pending_stat, 1);
	
	__nc_errno = ((r < 0)?-r:0);
	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif

	TRACE((T_API, "(INODE[%d]) = %d (nc_errno=%d, volume='%s') elapsed %.2f msec\n", NC_INODE_ID(fh), r, __nc_errno, volsig, d1));
	
	return r;
}
PUBLIC_IF CALLSYNTAX nc_file_handle_t *
nc_open_extended_apc(nc_volume_context_t *volume, char *cache_id, char *path, int mode, nc_stat_t *s, nc_kv_list_t *kv, nc_apc_open_callback_t proc, void *userdata)
{
	nc_file_handle_t		*handle = NULL;
#ifdef __PROFILE
	perf_val_t 		ps, pe;
	float		d1=0;
#endif


	clrtid();

	PROFILE_CHECK(ps);
	VOLUME_OP_BEGIN(volume, U_FALSE, U_TRUE, NULL);
	
	__nc_errno = 0;
	if ( nc_check_path_limit(path) < 0) {
		__nc_errno = errno = ENAMETOOLONG;
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p, %p, %p) = %p(nc_errno=%d)\n", 
						(volume?volume->signature:"NULL"), 
						cache_id, 
						path, 
						mode, 
						kv, 
						proc,
						userdata,
						NULL, 
						__nc_errno));
		
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if ( __terminating )  {
		__nc_errno = errno = EINVAL;
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p, %p, %p) = %p(nc_errno=%d)\n", 
						(volume?volume->signature:"NULL"), 
						cache_id, 
						path, 
						mode, 
						kv, 
						proc,
						userdata,
						NULL, 
						__nc_errno));
		
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if (strlen(path) > NC_VISIBLE_MAXPATHLEN)  {
		__nc_errno = errno = ENAMETOOLONG;
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p, %p, %p) = %p(nc_errno=%d)\n", 
						(volume?volume->signature:"NULL"), 
						cache_id, 
						path, 
						mode, 
						kv, 
						proc,
						userdata,
						NULL, 
						__nc_errno));
		
		VOLUME_RETURN_ERROR(volume, NULL);
	}
	if (! nvm_is_online2(volume)) {
		__nc_errno = errno = EINVAL;
		
		TRACE((T_WARN|T_API|T_API_OPEN, "('%s', '%s', 0x%08X) = %p(nc_errno=%d)\n", (volume?volume->signature:"NULL"), path, mode, NULL, __nc_errno));
		VOLUME_RETURN_ERROR(volume, NULL);
	}


	_ATOMIC_ADD(g__pending_open, 1);
	handle = fio_open_extended_apc_wrap(volume, cache_id, path, mode, s, kv, proc, userdata);
	mavg_update(g__openi, (double)d1/1000.0);

	

	if (__nc_errno != EWOULDBLOCK) {
		/* operation finished */
		_ATOMIC_SUB(g__pending_open, 1);
	}

	PROFILE_CHECK(pe);
#ifdef __PROFILE
	d1 = PROFILE_GAP_MSEC(ps, pe);
#endif
	TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p, %p, %p) = INODE[%d].R[%d](nc_errno=%d)(VREF=%d) %.2f msec elapsed\n", 
						(volume?volume->signature:"NULL"), 
						cache_id, 
						path, 
						mode, 
						s,
						kv, 
						proc,
						userdata,
						NC_INODE_ID(handle), 
						(handle?INODE_GET_REF(handle->inode):-1),
						__nc_errno,
						GET_ATOMIC_VAL(volume->refcnt) -1,
						d1
						));

	VOLUME_OP_END(volume, handle);
}


PUBLIC_IF CALLSYNTAX int
nc_unlink(nc_volume_context_t *volume, char *file, nc_xtra_options_t *req, nc_xtra_options_t **res)
{
#if 0
	int 	r;
	/*
	 * stat(path)
	 * if exist, issue to remove it
	 * remove it from stat-cache
	 */
	TRC_BEGIN((__func__));
	__nc_errno = 0;
	if (!volume) {
		__nc_errno = EINVAL;
		TRACE((T_API, "%s('%s') (no volume) returns %d(errno=%d)\n", 
					__func__,
					path,
					-__nc_errno,
					__nc_errno));
		TRC_END;
		return -__nc_errno;
	}
	if (!nvm_is_online(volume, NCOF_WRITABLE)) {
		__nc_errno = EINVAL;
		TRACE((T_API, "%s('%s', '%s') (no writable) returns %d(errno=%d)\n", 
					__func__,
					volume->signature, 
					path,
					-__nc_errno,
					__nc_errno));
		TRC_END;
		return -__nc_errno;
	}
	if ( nc_check_path_limit(path) < 0) {
		__nc_errno = errno = ENAMETOOLONG;
		TRC_END;
		return -__nc_errno;
	}
	_ATOMIC_ADD(g__pending_inodeop, 1);
	r = dm_unlink(volume, path, req, res);
	if (r < 0) {
		TRACE((T_WARN, "nc_unlink(%s) = %d\n", path, r));
	}
	__nc_errno = errno = ((r < 0)?-r:0);
	_ATOMIC_SUB(g__pending_inodeop, 1);
	TRACE((T_API, "%s('%s','%s')  returns %d(errno=%d)\n", 
					__func__,
					volume->signature,
					path,
					r,
					__nc_errno));
	TRC_END;
	return r;
#else
	return 0;
#endif
}

PUBLIC_IF CALLSYNTAX int 
nc_flush(nc_file_handle_t *fi)
{
#if 0
	int 	r = 0;
	nc_mount_context_t 	*volume;
	fc_inode_t 			*inode = fi->inode;
	TRC_BEGIN((__func__));
	__nc_errno = 0;
	if (fi && inode->volume && !nvm_is_online(inode->volume, NCOF_WRITABLE)) {
		__nc_errno = EINVAL;
		TRACE((T_API, "%s('%s') (no writable) returns %d(errno=%d)\n", 
					__func__,
					(char *)(inode?inode->q_path:"null"),
					-__nc_errno,
					__nc_errno));
		TRC_END;
		return -__nc_errno;
	}
	volume = inode->volume;
	r = fio_flush(fi);
	__nc_errno = errno = ((r < 0)?-r:0);
	TRACE((T_API, "%s('%s') returns %d(errno=%d)\n", 
					__func__,
					(char *)(inode?inode->q_path:"null"),
					-r,
					__nc_errno));
	TRC_END;
	return -r;
#endif
	return 0;
}


PUBLIC_IF CALLSYNTAX nc_open_cc_t *  		
nc_open_cc(		nc_open_cc_t * 		pcc,
				nc_cc_command_t 	command,
				void 				*value,
				int   				vallen)
{

	struct tag_nc_open_cc_quark 	*pq = NULL;


	if (pcc != NULL) {
		pcc = XMALLOC(sizeof(nc_open_cc_t), AI_ETC);
	}

	pq = XMALLOC(sizeof(nc_open_cc_quark_t) + vallen, AI_ETC);
	pq->command = command;
	pq->len 	= vallen;
	memcpy(pq->u._data, value, vallen);
	link_append(&pcc->root, pq, &pq->node);
	return pcc;
}
PUBLIC_IF CALLSYNTAX void
nc_close_cc	(nc_open_cc_t * 		pcc)
{
	struct tag_nc_open_cc_quark 	*pq = NULL;

	XVERIFY(pcc);
	while ((pq = link_get_head(&pcc->root)) != NULL) {
		XVERIFY(pq);
		XFREE(pq);
	}
	XFREE(pcc);
}

PUBLIC_IF CALLSYNTAX void *
nc_lookup_cc(nc_open_cc_t * pcc, nc_cc_command_t command)
{

	struct tag_nc_open_cc_quark 	*pq 	= NULL;
	void 							*val 	= NULL;
	XVERIFY(pcc);

	pq = link_get_head_noremove(&pcc->root);
	while (pq) {
		if (pq->command == command) {
			val = pq->u._data;
			break;
		}
		pq = link_get_next(&pcc->root, &pq->node);
	}
	return val;



}
static void
nc_adjust_ra()
{
	float 			fratio = 0.0;
	int				ra_org;
	unsigned int disk_rd;
	unsigned int fault_rd; 

	disk_rd 	= GET_ATOMIC_VAL(g__blk_rd_disk);
	fault_rd 	= GET_ATOMIC_VAL(g__blk_rd_fault);
	if (disk_rd > 0) {
		ra_org = g__cache_ra_blk_cnt;
		fratio = (fault_rd*100.0)/disk_rd;

		if (fratio > g__false_read_limit) {
			g__cache_ra_blk_cnt 	= max(g__cache_ra_blk_cnt - 2, 2); 
		}
		else {
			g__cache_ra_blk_cnt 	= min(g__cache_ra_blk_cnt + 1, (2*1024.0*1024.0)/NC_BLOCK_SIZE ); 
		}

		// if (ra_org != g__cache_ra_blk_cnt) {
		// TRACE((T_INFO, "Disk read-ahead changed to %d to %d(disk false-read ratio=%.2f)\n", ra_org, g__cache_ra_blk_cnt, fratio));
		TRACE((T_INFO, "Disk read-ahead changed to %d(disk false-read ratio=%.2f <--%u/%u)\n", 
						g__cache_ra_blk_cnt, 
						fratio,
						fault_rd,
						disk_rd
						));
							
		//}
	}
}
nc_size_t
nc_readahead_bytes()
{
	return 0;
}
