/*
 * 	DISK-CACHE MANAGER
 *
 *  ASSUMPTION
 *  	a partition should be dedicated to DISK-CACHE MANAGER for easy calculation
 * 			of disk utilization
 * LOCK 순서
 * 		1. IC-lock과 inode-lock은 상호 독립적으로 구현됨
 *
 * 		ic-lock을 획득한 상태에서 inode lock사용하지 말것
 * 		ic-lock 후 inode-lock의 획득을 위해서는 trylock을 활용한다.
 * 
 */
#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#include <alloca.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <zlib.h>
#ifdef HAVE_SYS_DIR_H
#include <sys/dir.h>
#endif
#ifdef HAVE_DIR_H
#include <dir.h>
#endif
#include <dirent.h>
#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#ifdef WIN32
#include <statfs.h>
#endif
#ifndef WIN32
#include <sys/vfs.h>
#endif
#include <ctype.h>
#include "disk_io.h"
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "bitmap.h"
#include "trace.h"
#include "bt_timer.h"
#include "md5.h"
#include <mm_malloc.h>
#include <snprintf.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-value"
#endif
#define	MDB_UPDATE_RESET(_i,_f) { \
	if ((_i)->part && ((_f) || dm_is_mdb_dirty(_i))) {\
		nc_int32_t		tv; \
		tv = inode->mdbversion; \
		pm_add_cache((_i)->part, (_i), U_FALSE);   \
		inode->mdbversion = inode->mdbversion - tv; \
		TRACE((T_INODE, "INODE[%u] - partition(%s), force_update(%d), mdb_dirty(%d)/%u\n", (_i)->uid, ((_i)->part?"Y":"N"), _f, dm_is_mdb_dirty(_i), (_i)->mdbversion)); \
	} \
}






#define	INODE_FREE_SIGNATURE 	0x1234
#define	INODE_CACHED_SIGNATURE 	0xA55A

#if 0
#define		DEBUG_TRAP		{TRACE((T_ERROR, "TRAP FOR DEBUGGING!!, removed prior to release\n"));TRAP;}
#else
#define		DEBUG_TRAP		
#endif
 /* CHG 2018-06-14 huibong ibuf, xbuf 변수 관련 실제 사용함수에서 중복 선언 warning 발생 관련 수정 (#32194) */
#define		ASSERT_OC(o_, e_) if (!(e_)) { \
									; \
								char 	ibuf_dump[1024]; \
								char 	xbuf_dump[512]; \
								TRACE((T_ERROR, "Error in Expr '%s' at %d@%s;INODE{%s}.X{%s}\n", \
												#e_,  \
												__LINE__,  \
												__FILE__, \
												((o_)->inode?dm_dump_lv2(ibuf_dump, sizeof(ibuf_dump), (o_)->inode):""), \
												dm_dump_open_context(xbuf_dump, sizeof(xbuf_dump),o_) \
												));  \
							}

#define DM_CHUNKMAP_EXPAND_UNIT 	10


typedef struct {
	nc_part_element_t 	*part;
	uuid_t				uuid;
	char				*cache_path;
} fc_cache_file_info_t;

/*
 * APS
 *
 *
 *	dm_init(char *path, size_t capability)
 *			- initialize diskcache_mgr and scan cache_directory
 *
 *	dm_add_new_file(char *path, fc_inode_t *inode)
 *			- cache-missed file, add it to diskcache_mgr DB
 *	dm_refer(char *path)
 *			- update last access time
 *
 *
 */

#define	   	DM_CACHE_DIR_COUNT	NC_CACHE_DIR_COUNT
/* max background disk-rebuilding threads */
#if 1
#define 	DIRS_PER_THREAD		64
#else
#define 	DIRS_PER_THREAD		256
#endif/*DEBUG */
#define 	MAX_BACKGROUND_THREADS 	(DM_CACHE_DIR_COUNT/DIRS_PER_THREAD)


#define 	DM_RECLAIM_LOWMARK 			80
#define 	DM_RECLAIM_HIGHMARK 		90

int 		g__dm_reclaim_lowmark 		= DM_RECLAIM_LOWMARK;
int 		g__dm_reclaim_highmark 		= DM_RECLAIM_HIGHMARK;
int 		g__enable_offline_cache 	= 1;


/* disk rebuilding status */
#define 	DMS_INIT 			0
#define 	DMS_BUILDING 		1
#define 	DMS_CLEAN   		2

#define 	NC_MIN_HEADER_SIZE_V30 	NC_ALIGNED_SIZE(sizeof(fc_header_info_v30_t), NC_ALIGN_SIZE)

#define DM_NEED_FLUSH(_i_)		((_i_)->writable) 

/*
 * open error code
 */
#define 	DM_OPENR_FRESH  			NC_FRESH
#define 	DM_OPENR_REFRESH_FRESH  	NC_REFRESH_FRESH
#define 	DM_OPENR_STALLED_ETAG 		NC_STALE_BY_ETAG
#define 	DM_OPENR_STALLED_MTIME 		NC_STALE_BY_MTIME
#define 	DM_OPENR_STALLED_OTHER 		NC_STALE_BY_OTHER
#define 	DM_OPENR_STALLED_OFFLINE 	NC_STALE_BY_OFFLINE
#define 	DM_OPENR_STALLED_SIZE 		NC_STALE_BY_SIZE
#define 	DM_OPENR_STALLED_ALWAYS 	NC_STALE_BY_ALWAYS
#define 	DM_OPENR_STALLED_BY_ORIGIN 	NC_STALE_BY_ORIGIN_OTHER
#define 	DM_OPENR_AGAIN 				NC_VALIDATION_CODENEXT /* 9 */
#define 	DM_OPENR_ALLOCFAILED		(NC_VALIDATION_CODENEXT+1) /*10*/
#define 	DM_OPENR_NOTFOUND 			(NC_VALIDATION_CODENEXT+2) /*11*/
#define 	DM_OPENR_ALLOCATED 			(NC_VALIDATION_CODENEXT+3) /*12*/
#define 	DM_OPENR_KEYCHANGED 		(NC_VALIDATION_CODENEXT+4) /*13*/
#define 	DM_OPENR_SCHEDULED 			(NC_VALIDATION_CODENEXT+5) /*14*/
#define 	DM_OPENR_BUSYWAIT			(NC_VALIDATION_CODENEXT+6) /*14*/



typedef struct tag_cachedir_state {
	char							cachedir[NC_MAXPATHLEN];
	int								nodisk; /* 1 when memcache_mode is 1 */
	dm_reclaim_inode_callback_t		proc;
	int								notrace;
	long							alloc;
	long							count;
	float							fs_usedper; /* used percent */
	size_t							fs_bsize;
	size_t							fs_bfree;
	long long						cap;
	pthread_t						background_thread[MAX_BACKGROUND_THREADS];
	pthread_mutex_t					lock;
	pthread_t						tid;
	fc_clfu_root_t					*inode_cache;
	fc_lru_root_t					*inode_free;
	int								state;	/* DMS_INIT, DMS_BUILDING, DMS_CLEAN */
	int								reclaiming; /* 1 if reclaiming in progress */
	bt_timer_t						timer_inode_spinner;
	bt_timer_t						timer_check_statfs;
} cachedir_state_t;



char 		g__fresh_result[][32] = {
		"NC_FRESH:0", 			/* 아직 사용가능한 상태 */
		"NC_REFRESH_FRESH:1", 			/* 유효기간 지나서 체크해본 결과 사용가능한 상태*/
		"NC_STALE_BY_ETAG:2", 			/* 캐싱의 ETAG값이 상이하여 STALE처리 됨*/
		"NC_STALE_BY_MTIME:3", 			/* 객체 변경시간이 상이하여 STALE처리 됨 */
		"NC_STALE_BY_OTHER:4", 			/* 여타 다른 이유로 STALE처리됨 */
		"NC_STALE_BY_OFFLINE:5", 			/* 여타 다른 이유로 STALE처리됨 */
		"NC_STALE_BY_SIZE:6",
		"NC_STALE_BY_ALWAYS:7",
		"NC_STALE_BY_ORIGIN_OTHER:8",
		"NC_VALIDATION_CODENEXT:9"
	};
char _str_block_status[][32] = {
	"DM_BLOCK_FAULT",
	"DM_BLOCK_ONMEMORY",
	"DM_BLOCK_ONDISK",
	"DM_BLOCK_IOPROGRESS"
};

static nc_uint32_t 		__dm_signature = 1;

nc_uint64_t				g__tb_seq		= 0;
nc_uint64_t				g__tb_seed		= 0;
int 					g__delayed_sync = 1;	/* 헤더 싱크 delay */
int						g__clfu_inode_base = 0;
int						g__clfu_inode_base_percent = 0;
int						g__clfu_inode_total = 0;
int						g__clfu_inode_kept = 0;

int				g__octx_count = 0;
int 				g__octx_bind_count = 0;

static cachedir_state_t __dm_state;


extern int				g__pending_close;
extern int				g__per_block_pages;
extern __thread int 	__bc_locked;
extern fc_clfu_root_t	*g__blk_cache;
extern memmgr_heap_t	g__inode_heap;
extern memmgr_heap_t	g__page_heap;
extern glru_t 			*g__path_cache ;
extern int 				g__cold_ratio;
extern int 				g__oapc_count;
extern int 				g__forceclose;
extern int 				g__enable_cold_caching;
extern int 				g__intelligent_cold_caching;
extern 	fc_blk_t		*s__blk_map;
extern __thread int		__nc_errno;
extern int				g__default_uid;
extern int				g__default_gid;
long					g__inode_count = 0;
long					g__inode_max_count = 100000;
extern time_t			netcache_signature;
extern nc_int16_t 		__memcache_mode;
extern void *			__timer_wheel_base;
extern int				__terminating;
extern int 				__enable_compaction;
extern nc_size_t 		__max_cachable_object_size;
long 					g__inode_opened = 0;
link_list_t				__inode_pool  = LIST_INITIALIZER;

static int dm_update_iobusy(fc_inode_t *inode);
static int dm_is_iobusy(fc_inode_t *inode);
static void dm_copy_uuid(uuid_t dest, uuid_t src);
static ssize_t dm_read_bytes(nc_fio_handle_t fd, char *ptr, ssize_t sz, nc_off_t base);
static ssize_t dm_write_bytes(fc_inode_t *inode, char *ptr, ssize_t sz, nc_off_t base);
static fc_inode_t * dm_load_disk_cache(nc_volume_context_t *volume, char *cachekey);
static int dm_sync_disk_object(fc_inode_t *inode, int bforce, size_t *hdrsiz, char *msg);
static int dm_update_partition_info(char *path);
#if NOTUSED
static void dm_hit_clfu(fc_inode_t *inode, int ntimes) ;
#endif
static void dm_hit_clfu_nolock(fc_inode_t *inode, int ntimes) ;
static char * dm_create_cache_key(char *cache_id, char *property_list, nc_xtra_options_t *prop);
static int dm_upgrade_if_hot(fc_inode_t *inode);
static int dm_compress_vheader(nc_uint8_t *outbuf, nc_int32_t *outlen, nc_uint8_t *inbuf, nc_int32_t inlen);
static int dm_decompress_vheader(nc_uint8_t *outbuf, nc_int32_t *outlen, nc_uint8_t *inbuf, nc_int32_t inlen);
static nc_crc_t do_crc32(char *data, ssize_t len);
static int dm_verify_header(void *header);
static int dm_restore_inode_extent_v30(fc_inode_t *inode, void *header, size_t memsiz, char *errmsg, int checkcomplete);
static int dm_load_inode_extent(nc_volume_context_t *volume, fc_inode_t *inode, nc_size_t sz, int checkcomplete);
static int dm_need_sync(fc_inode_t *inode) ;
static int dm_is_header_dirty(fc_inode_t *inode) ;
static apc_open_context_t * dm_apc_prepare_context(apc_open_context_t *soc, int state);
static int dm_create_key_pair(	nc_volume_context_t *, 
								char *opath, 
								nc_xtra_options_t *property, 
								char *cache_key_given, 
								char **cache_key, 
								nc_xtra_options_t *req );
static int dm_make_report(fc_inode_t *inode, nc_xtra_options_t *kv, nc_hit_status_t 	hs);
static int dm_apc_schedule_async_open(apc_open_context_t *soc, int state, char *f, int l);

#ifdef WIN32
size_t dio_read_bytes(HANDLE fd, BYTE *buffer, nc_off_t offset, nc_size_t sz);
size_t dio_write_bytes(HANDLE fd, BYTE *buffer, nc_off_t offset, nc_size_t sz);
#endif
void apc_destroy_open_context(apc_open_context_t *oc);
static int dm_make_inode_online(fc_inode_t *inode, nc_path_lock_t *pl, nc_xtra_options_t *kv);
static void dm_update_recent_nolock(nc_volume_context_t *volume, fc_inode_t *inode);
static int dm_update_caching_completeness_nolock(fc_inode_t *inode);
static void apc_open_context_init(	apc_open_context_t 			*soc, 
									nc_volume_context_t 		*volume, 
									char 						*origin_path, 
									char 						*cache_id, 
									nc_mode_t 					mode, 
									nc_xtra_options_t 			*kv, 
									nc_path_lock_t 				*pl,
									int							pl_makeref,
									nc_uint32_t					ubi,
									nc_apc_open_callback_t 		callback, 
									void 						* callback_data);
static int dm_is_vary_meta_object_nolock(fc_inode_t *inode);
static fc_inode_t * dm_create_cache_object(	nc_volume_context_t *volume, 
											nc_path_lock_t		*lock, 
											char				**cache_key,
											char				*object_path,
											nc_mode_t			mode,
											nc_stat_t			*stat,
											nc_xtra_options_t 	*req_prop,
											nc_uint32_t 		cversion,		/*cache version */
											u_boolean_t			makeref, 
											int					markbusy, 
											char				*f, 
											int					l
											);
/*
 * asio event handling functions
*/
static int dm_upsert_template(apc_open_context_t *openx);
static fc_inode_t *dm_apc_handle_event_for_miss(apc_open_context_t *oc);
static fc_inode_t *dm_apc_handle_event_for_freshcheck(apc_open_context_t *oc);
static fc_inode_t *dm_apc_handle_event_for_miss_internal(apc_open_context_t *oc);
static fc_inode_t *dm_apc_handle_event_for_freshcheck_internal(apc_open_context_t *oc);
static int dm_open_file_ifnot(int fd, char *fname, nc_size_t fsize, int bcreat, int bronly);
static fc_inode_t * dm_apc_open_inode_internal(apc_open_context_t *poc);
static int dm_invalidate_blks(fc_inode_t *inode);
static void dm_remove_inode_while_close(fc_inode_t *inode);
static nc_origin_session_t* dm_bind_origin_session(fc_inode_t *inode, nc_origin_session_t origin, char *f, int l);
static fc_inode_t * dm_duplicate_inode(fc_inode_t *oi, apc_open_context_t *oc);
static fc_inode_t * dm_lookup_cache(nc_volume_context_t *volume, int lookup_type, char *cache_key, u_boolean_t makeref, u_boolean_t markbusy, nc_path_lock_t *lock, const char *f, int l);

#if NOT_USED
static char __p_fresh[][32] = {
	"0:NC_FRESH",
	"1:NC_REFRESH_FRESH",
	"2:NC_STALE_BY_ETAG",
	"3:NC_STALE_BY_MTIME",
	"4:NC_STALE_BY_OTHER",
	"5:NC_STALE_BY_OFFLINE",
	"6:NC_STALE_BY_SIZE",
	"7:NC_STALE_BY_ALWAYS",
	"8:NC_STALE_BY_ORIGIN_OTHER",
	"9:NC_VALIDATION_CODENEXT"
};
#endif


static char 	__p_stat[][32]= {
		"0:NC_OS_UNDEFINED",
		"1:NC_OS_MISS",
		"2:NC_OS_HIT",
		"3:NC_OS_NC_MISS",
		"4:NC_OS_NC_HIT",
		"5:NC_OS_REFRESH_HIT",
		"6:NC_OS_REFRESH_MISS",
		"7:NC_OS_OFF_HIT",
		"8:NC_OS_PROPERTY_HIT",
		"9:NC_OS_PROPERTY_MISS",
		"10:NC_OS_PROPERTY_REFRESH_HIT",
		"11:NC_OS_PROPERTY_OFF_HIT",
		"12:NC_OS_NOTFOUND",
		"13:NC_OS_PROPERTY_REFRESH_MISS",
		"14:NC_OS_BYPASS"
	};
static char 	__z_apc_open_str[][32] = {
	"APC_OS_INIT = 0",
	"APC_OS_QUERY_FOR_MISS=1",
	"APC_OS_QUERY_FOR_REFRESHCHECK=2",
	"APC_OS_WAIT_FOR_COMPLETION=3"
};
nc_uint32_t FNV1A_Hash_Yoshimura(char *str, nc_uint32_t wrdlen);

long 
dm_max_blkno(nc_size_t sz)
{
	long 	mb = 0;
	register nc_off_t 	loff = max(sz -1, 0);
	
	DEBUG_ASSERT(sz >= 0);
	mb =  BLK_NO(loff, 0);
	return mb;
}
int
dm_is_writable(nc_mode_t mode)
{
	return IS_ON(mode,  (O_WRONLY|O_CREAT|O_RDWR)) ; 
}
PUBLIC_IF CALLSYNTAX char *
dm_dump_mode(char *buf, int len, nc_mode_t mode)
{
	char 	*p = buf;
	int 	n;
	int 	remained = len;
	int 	need_sep = 0;
#define 	MAKE_SEP 	if (need_sep) { n = snprintf(p, remained, "|"); p += n; remained -= n; need_sep = 0; }
#define 	CHECK_MODE(_m, _v) 	if ((_m & _v) != 0) { MAKE_SEP; n = snprintf(p, remained, #_v); p += n; remained -= n; need_sep++;}

	if ((mode & O_ACCMODE) == 0) {
		MAKE_SEP;
		n = snprintf(p, remained, "O_RDONLY");
		p += n;
		remained -= n;
		need_sep++;
	}
	CHECK_MODE(mode, O_CREAT);
	CHECK_MODE(mode, O_WRONLY);
	CHECK_MODE(mode, O_RDWR);
	CHECK_MODE(mode, O_APPEND);
#ifdef O_NOCTTY
	CHECK_MODE(mode, O_NOCTTY);
#endif
#ifdef O_TRUNC
	CHECK_MODE(mode, O_TRUNC);
#endif
#ifdef O_NONBLOCK
	CHECK_MODE(mode, O_NONBLOCK);
#endif
#ifdef O_SYNC
	CHECK_MODE(mode, O_SYNC);
#endif
#ifdef O_LARGEFILE
	CHECK_MODE(mode, O_LARGEFILE);
#endif
#ifdef O_NOATIME
	CHECK_MODE(mode, O_NOATIME);
#endif
	return buf;
}
unsigned long
dm_make_signature()
{
	 nc_uint32_t 	v;
	 /* do we have to lock ? */
	 v = _ATOMIC_ADD(__dm_signature,1);
	 if (v == 0) {
	 	v = _ATOMIC_ADD(__dm_signature,1);
	}
	return v;
}
nc_uint64_t
dm_get_signature(fc_inode_t *inode)
{
	nc_uint64_t t = inode->signature;
	return t;
}
int
dm_online()
{
	return (__dm_state.state == DMS_CLEAN );
}
/*
 * 주의 이 함수는 ic-lock 획득 후 호출됨
 */

int 
dm_inode_idle(void *vf)
{
	fc_inode_t	*inode = (fc_inode_t *)vf;
	int 		idle = U_FALSE;

	/*
	 * refcnt만 봐야하므로
	 * diskcache에서 refcnt를 감소시킨 순간 부터 이후 코드에서 inode
	 * 참조는 모두 부적절함
	 */
	idle = !(ic_is_busy(inode) || INODE_BUSY(inode));

	return idle;
}


/*
 * NOTE!!!) this function should be called without any lock except clfu lock
 */
static void
dm_hit_clfu_nolock(fc_inode_t *inode, int ntimes) 
{
	if (clfu_cached(&inode->node)) {
		ic_hit_nolock(inode, ntimes) ;
	}
}
#if NOTUSED
static void
dm_hit_clfu(fc_inode_t *inode, int ntimes) 
{
	IC_LOCK(CLFU_LM_EXCLUSIVE);
	dm_hit_clfu_nolock(inode, ntimes) ;
	IC_UNLOCK;
}
#endif
/*
 * 이 함수의 호출 시 
 * inode->bitmap 메모리가 resize되고 있을 확율은
 * 확정적으로 0.0%임
 * 고로 inode lock 필요없음
 */
static int
dm_update_caching_completeness_nolock(fc_inode_t *inode)
{
	int	old_compl = inode->ob_complete; 

	/*
	 *
	 * disk캐싱 여부 정보를 bitmap스캔만으로 파악
	 *
	 */
	if (dm_is_vary_meta_object_nolock(inode)) return 1;


#ifdef EXPERIMENTAL_USE_SLOCK
	pthread_spin_lock(&inode->slock);
#endif

	inode->ob_complete = bitmap_full(inode->bitmap, inode->bitmaplen);

#ifdef EXPERIMENTAL_USE_SLOCK
	pthread_spin_unlock(&inode->slock);
#endif

	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%d] caching complete = %d(maxblkno=%d, bitmap=%d)\n",
				inode->volume->signature,
				inode->q_id,
				inode->uid,
				(inode->ob_complete != 0),
				inode->maxblkno,
				inode->bitmaplen
		 ));
	if (old_compl != inode->ob_complete) {
	 	/*
		 * 전체 청크가 모두 캐싱됨
		 */
		DM_UPDATE_METAINFO(inode, U_TRUE);
		dm_need_header_update(inode);
	}
	return inode->ob_complete == 1;
}



/*
 * cacheops에서 callback으로 호출됨.
 * 파라미터로 제공된 inode에서 extent가 reclaim이 가능한 상태인지
 * 체크 후, 불가능한 경우는 음수를 리턴하고, 
 * 가능하다면 0 또는 양수를 리턴
 * 	
 */

static int
dm_update_partition_info(char *path)
{
	struct statfs 	sf;
	static int 		__pinfo_init = 0;
	long long 		block_used = 0;

	if (statfs(path, &sf) != 0) {
		TRACE((T_INFO, "statfs('%s') - error\n", path));
		return -1;
	}
	if (!__pinfo_init) {
		TRACE((T_INODE, "partition info:\n"
				   "\t\t\t\t\t file system magic info      : 0x%08X\n"
				   "\t\t\t\t\t optimal transfer block size : %ld\n"
				   "\t\t\t\t\t # of total data blocks      : %lld\n"
				   "\t\t\t\t\t # of free data blocks       : %lld\n"
				   "\t\t\t\t\t # of availble data blocks   : %lld\n"
				   "\t\t\t\t\t # of total inodes           : %lld\n"
				   "\t\t\t\t\t # of free inodes            : %lld\n",
				   "\t\t\t\t\t available free space        : %.2f %%\n",
				   sf.f_type,
				   sf.f_bsize,
				   sf.f_blocks,
				   sf.f_bfree,
				   sf.f_bavail,
				   sf.f_files,
				   sf.f_ffree));

	}
	if (sf.f_blocks > 0) {
		block_used = sf.f_blocks - sf.f_bfree;
		if (block_used == 0) {
			__dm_state.fs_usedper = 0.0;
		}
	}
	__dm_state.fs_bfree = sf.f_bfree;
	return 0;
}

int
dm_check_magic(unsigned long mc)
{
	mc = 0xFFFFFFFF & mc;

	if (mc == NC_MAGIC_V30) {
		return INODE_HDR_VERSION_V30;
	}
	return 0;
}




/*
 * 헤더 정보를 파싱하지 않고 메모리 상에 로딩
 * v2.5의 경우, 압축되어있는 경우엔 압축된 상태로 메모리에 로딩됨
 */
static char *
dm_read_header(nc_fio_handle_t *fd/*IN*/, char *cpath/*IN*/, size_t *memsiz/*OUT*/, char *errmsg)
{

	int 				hv = -1;
	fc_common_header_t 	*pcommon;
	char 				ebuf[256];
	//char 				*base;
	//int 				len;
	int 				toread;
	int 				real_align_hdr_size;
	int 				diskhdrsize;
	int 				hdrcopied, copied;
	char 				*H = NULL;
	char 				*pbuffer;
	size_t 				allocated = 0;

	

	if (!dio_valid(*fd)) {
		*fd = dio_open_file((char *)cpath, 0, U_FALSE/*only open if exist */, U_TRUE/*readonly*/);
	}

	if (!dio_valid(*fd)) {
		sprintf(errmsg, "caching file[%s] - open error(%s)\n", cpath, strerror(errno));
		__nc_errno = errno = ENOENT;
		
		return NULL;
	}

	toread = NC_ALIGN_SIZE; /* 헤더 최소 단위만큼 읽도록 지정 */

	allocated = real_align_hdr_size = NC_ALIGN_SIZE;
	H = XMALLOC(real_align_hdr_size, AI_ETC);
	*memsiz = real_align_hdr_size;
	pbuffer = (char *)H;
	hdrcopied = 0;

	do {

#ifdef WIN32
		copied = dio_read_bytes(*fd, pbuffer + hdrcopied, hdrcopied, toread);
#else
		copied = dm_read_bytes(*fd, pbuffer+hdrcopied, toread, hdrcopied);

#endif


		TRACE((0, "pread(%ld, %ld, %ld) => %d\n", (long)hdrcopied, (long)toread, (long)hdrcopied, copied));
		if (copied < toread) 
		{
			/* CHG 2018-04-11 huibong nc_lasterror() 삭제에 따른 수정 처리 (#32013) */
			sprintf(errmsg, "caching file['%s'] FD(%d) - error, buffer=%p (copied=%d, toread=%d):%s\n"
					, cpath, *fd, (char *)(pbuffer+hdrcopied), copied, toread, nc_strerror(errno, ebuf, sizeof(ebuf)));
			XFREE(H);
			H = NULL;
			goto L_end_header;
		}

		if (hdrcopied == 0) {
			/*
			 * we successfully read the min header infomation
			 */
			pcommon 	= (fc_common_header_t *)H;
			hv 			= dm_check_magic(pcommon->magic);


			if (hv == INODE_HDR_VERSION_V30) {
				diskhdrsize 			= pcommon->disk_header_size;
				diskhdrsize 			=  NC_ALIGNED_SIZE(diskhdrsize, NC_ALIGN_SIZE);
				/*
				 * version 2.5의 경우 압축되었을 수 있으므로, 압축풀린 상태의 헤더크기 정보를
				 * 이용하여 재할당 메모리 크기를 계산
				 */
				real_align_hdr_size 	=  NC_ALIGNED_SIZE(pcommon->header_size, NC_ALIGN_SIZE);
			}
			else
			{
				/* CHG 2018-04-11 huibong nc_lasterror() 삭제에 따른 수정 처리 (#32013) */
				sprintf( errmsg, "CACHE['%s'] FD(%d) - header magic corrupt(%s)\n"
						 , cpath, *fd, nc_strerror( errno, ebuf, sizeof( ebuf ) ) ) ;
				XFREE( H );
				H = NULL;
				goto L_end_header;
				break;
			}
			/*
			 *
			 * align total size with pm_get_align_size
			 */

			if (allocated < real_align_hdr_size) {
				/* need to realign */
				H = XREALLOC(H, real_align_hdr_size, AI_ETC);
				*memsiz = real_align_hdr_size;
				pbuffer = (char *)H;
				allocated = real_align_hdr_size;
			}
		}
		hdrcopied += copied;
		toread 		= diskhdrsize - hdrcopied;
	} while (diskhdrsize > hdrcopied);

L_end_header:

	if (hv == INODE_HDR_VERSION_V30) {
		fc_header_info_v30_t	*h30 = (fc_header_info_v30_t *)H;
		/*
		 * header 규격이 version 2.5인 경우 압축 여부에 따른 압축 해제
		 */
		if ((h30->chdr.flag & NC_HEADER_FLAG_COMPRESSED) != 0) {
			/*
			 * 압축된 헤더
			 */
			nc_uint8_t 		*t_mem = NULL; 
			nc_int32_t		t_siz = h30->chdr.header_size - sizeof(fc_common_header_t); /* 압축되지않는 크기*/
			nc_uint32_t 	t_compsiz = h30->chdr.disk_header_size - sizeof(fc_common_header_t);/*압축영역 크기*/
			/*
			 * 압축된 영역을 t_mem으로 이동
			 */
			t_mem = XMALLOC(t_siz, AI_ETC);
			memcpy(t_mem, (nc_uint8_t *)h30 + sizeof(fc_common_header_t), t_compsiz);
	
			/*
			 * t_mem에 저장된 t_siz 크기의 압축데이타를 풀어서 h30 + sizeof(fc_common_Header_t)
			 * 위치에 저장
			 */
			if (dm_decompress_vheader((nc_uint8_t *)h30 + sizeof(fc_common_header_t), &t_siz, t_mem, t_compsiz) < 0) {
				sprintf(errmsg, "Caching file[%s] - decompression failed(CRC verified);magic[0x%8X], header size[%d], disk header size[%d], flag[0x%08X]\n",
									cpath,
									h30->chdr.magic,
									h30->chdr.header_size,
									h30->chdr.disk_header_size,
									h30->chdr.flag);

				XFREE(H);
				H = NULL;
			}
			XFREE(t_mem);
		}
	}

	return H;
}
static int
dm_restore_inode_extent_v30(fc_inode_t *inode, void *header, size_t memsiz, char *errmsg, int checkcomplete)
{
	int						Bsize;
	int						Psize;
	int						Msize;
	int						LPsize;
	int 					len1,len2,len3;
	int						r = 0;
	char					*bitmap_base = NULL, *vb1, *vb2, *vb3;
	char 					*vsignature = NULL;
	char 					*vpath = NULL;
	char 					*vid = NULL;
	fc_header_info_v30_t	*h30 = (fc_header_info_v30_t *)header;

	



	/*
	 * 압축여부에 따라 압축 해제
	 */


	Bsize 	= NC_CANNED_BITMAP_SIZE(inode->bitmaplen);
#ifdef NC_ENABLE_CRC
	Psize 	= NC_CANNED_CRC_SIZE(inode->bitmaplen);
#else
	Psize 	= 0;
#endif
	LPsize 	= NC_CANNED_LPMAP_SIZE(inode->bitmaplen);

	Msize 	= NC_CANNED_BLOCKMAP_SIZE(inode->bitmaplen);




	bitmap_base = h30->vbase;
	/* signature */
	bitmap_base 	= vb1 = vstring_unpack(bitmap_base, &vsignature, &len1);
	if (vsignature == NULL || strcasecmp(vsignature, inode->volume->signature) != 0) {
		strcpy(errmsg, "invalid volume signature(mismatch)");
		r = -1;
		goto L_load_extent_failed_v30;
	}


	/* object-path */
	bitmap_base 	= vb2 = vstring_unpack(bitmap_base, &vpath, &len2);
	if (vpath == NULL || strcasecmp(vpath, inode->q_path) != 0) {
		strcpy(errmsg, "invalid object path(mismatch) ");
		r = -1;
		goto L_load_extent_failed_v30;
	}


	/* object-id */
	bitmap_base 	= vb3 = vstring_unpack(bitmap_base, &vid, &len3);
	if (vid == NULL || strcasecmp(vid, inode->q_id) != 0) {
		strcpy(errmsg, "invalid object key(mismatch) ");
		r = -1;
		goto L_load_extent_failed_v30;
	}

	if ((bitmap_base + Bsize) > ((char *)h30+h30->chdr.header_size)) {
		/* CHG 2018-04-12 huibong 출력 인자 갯수 불일치 수정 (#32018) */
		strcpy(errmsg, "meta-info corrupt(bitmap)");

		TRACE((0, "Volume[%s].CKey[%s]/Sz[%lld] - seems corrupt(base+bsize) ; header(%p), bitmap_base[%p]+Bsize[%d](%p) >= h30[%p] + header_size[%d](%p)(vbase=%p)\n" 
					   "\t\t static header     = %d (common and fields)\n" 
					   "\t\t variable strings  = %d (signature=%d, path=%d, id=%d) \n"
					   "\t\t map size          = %d (bitmap=%d, blockmap=%d, CRCmap=%d) <=> %d\n"
					   "\t\t ____________________________________________________________________\n"
					   "\t\t common header info= %d (compressed=%d)\n"
						, (vsignature?vsignature:"NULL")
						, inode->q_path
						, inode->size
						, (char *)h30
						, bitmap_base
						, Bsize
						, (char *)bitmap_base + Bsize
						, h30
						, h30->chdr.header_size
						, (char *)h30 + h30->chdr.header_size
						, h30->vbase
						, sizeof(struct tag_fc_header_info_v30)
						, (len1+len2+len3)
						, len1
						, len2
						, len3
						, (Bsize+Psize+Msize)
						, Bsize
						, Msize
						, Psize
						, h30->chdr.header_size
						, h30->chdr.header_size
						, h30->chdr.disk_header_size ));
		r = -1;
		goto L_load_extent_failed_v30;
	}

	/*
	 * bitmap_base => bitmap start offset
	 */
	if (checkcomplete && !bitmap_full((unsigned long *)bitmap_base, inode->bitmaplen)) {
		/*
		 * not completely cached, some data missing
		 */
		strcpy(errmsg, "object not completed cached(but expected)");
		r = -1;
		goto L_load_extent_failed_v30;
	}
	/*
	 * bitmaplen개의 bit를 복사
	 */
	bitmap_copy(inode->bitmap, (unsigned long *)bitmap_base, inode->bitmaplen);




#ifdef NC_ENABLE_CRC
	DEBUG_ASSERT_FILE(inode, (inode->crcsize >= Psize));
#endif
	if ((bitmap_base + Bsize + Psize) > ((char *)h30+h30->chdr.header_size)) {
		strcpy(errmsg, "meta-info corrupt(blockcrc)");

		TRACE((0, "Volume[%s].CKey[%s]/Sz[%lld] - seems corrupt(base + bsize + psize) ; header(%p), bitmap_base[%p]+Bsize[%d]+Psize[%d](%p) >= h30[%p] + header_size[%d](%p)(vbase=%p)\n" 
					   "\t\t static header     = %d (common and fields)\n"
					   "\t\t variable strings  = %d (signature=%d, path=%d, id=%d)\n"
					   "\t\t map size          = %d (bitmap=%d, blockmap=%d, CRCmap=%d)\n"
					   "\t\t ____________________________________________________________________\n"
					   "\t\t common header info= %d (compressed=%d)\n", 
						(vsignature?vsignature:"NULL"),
						inode->q_id,
						inode->size,
						(char *)h30,
						bitmap_base,
						Bsize,
						Psize,
						(char *)bitmap_base + Bsize + Psize,
						h30,
						h30->chdr.header_size,
						(char *)h30 + h30->chdr.header_size,
						h30->vbase,
						sizeof(struct tag_fc_header_info_v30),
						(len1+len2+len3), 
						len1, 
						len2, 
						len3,
						(Bsize+Psize+Msize), 
						Bsize, 
						Msize, 
						Psize, 
						h30->chdr.header_size, 
						h30->chdr.disk_header_size
						));

		r = -1;
		goto L_load_extent_failed_v30;
	}
#ifdef NC_ENABLE_CRC
	memcpy(inode->blockcrc, (char *)bitmap_base +  Bsize, Psize);
#endif


	if (inode->size > 0) {
        memcpy(inode->LPmap, (char *)bitmap_base + Psize + Bsize , LPsize);
        /* rebuild pbitmap */
        if (LPmap_recover_pbitmap_nolock(inode) < 0) {
			strcpy(errmsg, "restoring LPmap failed");
			r = -1;
            goto L_load_extent_failed_v30;
        }
		dm_update_caching_completeness_nolock(inode);

	}
	TRACE((T_INODE, "INODE[%u]/maxblk[%ld] - extend loaded, h30 size=%ld\n", 
					inode->uid, 
					inode->maxblkno, 
					(long)inode->header_size));
L_load_extent_failed_v30:
	XFREE(vsignature);
	XFREE(vid);
	XFREE(vpath);
	
	return r;
}




static int
dm_load_inode_extent(nc_volume_context_t *volume, fc_inode_t *inode, nc_size_t sz, int checkcomplete)
{
	int 				result = 0;
 	size_t 				memsiz = 0;
	fc_header_info_v30_t *header = NULL;
	char				ibuf[2048];
	char				errmsg[4096]="";

	

	inode->c_path		= pm_create_cache_path(inode->part, inode->ob_priv != 0, inode->uuid, AI_ETC);
	/*
	 * read caching file header and decompress if compressed
	 *
	 */
	header = (fc_header_info_v30_t *)dm_read_header(&inode->fd, inode->c_path, &memsiz, errmsg);
	if (!header) {
		TRACE((T_INFO, "Volume[%s].CKey[%s] - disk cache corrupt(%s)\n", volume->signature, inode->q_id, errmsg ));
		result = -1; /* 로드 실패, 새로 생성 필요*/
		goto L_load_extent_failed;
	}

	if (!dm_verify_header(header)) {
		TRACE((T_INFO, "Volume[%s].CKey[%s] - found header CRC error\n", volume->signature, inode->q_id));
		result = -1;
		goto L_load_extent_failed;
	}

	/*
	 *  check if size info is different from that in MDB
	 */
	if (header->size != sz) {
		TRACE((T_INFO, "Volume[%s].CKey[%s] - disk cache[%s] corrupt(size mismatch;MDB:DISK=%lld:%lld)\n", 
						volume->signature, inode->q_id, inode->c_path, sz, header->size));
		result = -1;
		goto L_load_extent_failed;
	}


	if (dm_restore_inode_extent_v30(inode, header, memsiz, errmsg, checkcomplete) < 0) {
		TRACE((T_INFO, "Volume[%s].CKey[%s] - restore failed(%s);{%s}\n", 
						volume->signature, 
						inode->q_id, 
						errmsg, 
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)));
		result = -1; /* 로드 실패, 새로 생성 필요*/
		goto L_load_extent_failed;
	}


L_load_extent_failed:
	if (header) XFREE(header);

	return result;
}


/*
 * disk-caching object swap-in (load & restore)
 */
static fc_inode_t *
dm_load_disk_cache(nc_volume_context_t *volume, char *cachekey)
{
	fc_inode_t 			*inode = NULL;
	int					r;
#ifdef __PROFILE
	perf_val_t			ms, me;
	long				ud;
#endif
	nc_mdb_inode_info_t *minode = NULL;
	fc_inode_t 			*lookup;
	int					check_complete = 0;
	char				zuuid[128];
	char				ebuf[128];



	PROFILE_CHECK(ms);
	

	if (__dm_state.nodisk) {
		__nc_errno = errno = EACCES;
		
		return NULL;
	}
	/* find if it stored on disk */
	minode = pm_load_disk_inode(volume->preservecase, volume->signature, cachekey);
	if (!minode) {
		/* not cached on disk */
		__nc_errno = errno = ENOENT;
		
		return NULL;
	}


	inode 	= ic_prepare_inode_with_minode_nolock (volume, minode, 0/*setprogress*/);
	if (!inode) {
		TRACE((T_ERROR, "Volume[%s].CKey[%s] - inode allocation failed\n", volume->signature, cachekey));

		return NULL;
	}

	/*
	 * inode allocated and prepared
	 */

	if(minode->ob_validuuid != 0) {

		/* caching file on disk */
		check_complete = (minode->ob_rangeable == 0); /* non-rangeable 객체는 full-caching되어 있어야함*/
		/*
		 * load inode extent
		 * @to be filled
		 *		(1) inode->fd
		 *		(2) inode->{bitmap, pbitmap, LPmap, crc}
		 *		(3) inode->c_path
		 *
		 */
		r = dm_load_inode_extent(volume, inode, minode->size/*object real size in bytes */, check_complete);

		if (r < 0) {
			/*
			 * 캐싱된 객체 정보가 불완전하거나, 또는 무결성이 깨진상태임
			 * meta info 삭제
			 */
			r = mdb_remove_rowid_direct(minode->part->mdb, minode->uuid, minode->rowid);
			/*
			 * 디스크 캐시 삭제 if exist
			 */
			r = pm_unlink_uuid(volume->signature, cachekey, minode->ob_priv != 0, minode->part, minode->uuid, inode->c_path);
			if (r < 0) {
				TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned %d:%s\n",
								volume->signature,
								cachekey,
								uuid2string(minode->uuid, zuuid, sizeof(zuuid)),
								-r,
								strerror_r(-r, ebuf, sizeof(ebuf))
								));
			}
			else {
				TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned OK\n",
								volume->signature,
								cachekey,
								uuid2string(minode->uuid, zuuid, sizeof(zuuid))
								));
			}
			pm_free_minode(minode);
			dm_free_inode(inode, U_TRUE);
			__nc_errno = errno = ENOENT;
			return NULL;
		}
		inode->doffset = NC_ALIGNED_SIZE(inode->header_size, NC_ALIGN_SIZE);
		inode->lastsynctime = nc_cached_clock();
	}
	DEBUG_ASSERT_FILE(inode, inode->origincode != 304);


	/*
	 * 읽어온 객체 정보(minode)를 이용해서 inode를 생성 및 속성 정보에 따라
	 * cache-manager에 등록
	 */


	if (inode && inode->ob_priv == 0) {
		IC_LOCK(CLFU_LM_EXCLUSIVE);
		lookup = ic_register_cache_object_nolock(volume, inode); /* CLFU에 등록 후, reclaim되지 않도록. */
		DEBUG_ASSERT(inode == lookup);
		IC_UNLOCK;

	}
	

	PROFILE_CHECK(me);
#ifdef __PROFILE
	ud = PROFILE_GAP_MSEC(ms, me);
	if (ud > 5000) {
		TRACE((T_INFO, "Cache CKey '%s' load slow %.2f msec\n", cachekey, (ud/1000.0) ));
	}
#endif


	if (minode) {
		/*
		 * 할당된 메모리만 free
		 */
		pm_free_minode(minode);
	}
	
	return inode;
}



nc_size_t
dm_calc_header_size(fc_inode_t *inode)
{
	nc_size_t			hisize = 0;
	
	DEBUG_ASSERT (inode->hdr_version == INODE_HDR_VERSION_V30); 
	hisize = dm_calc_header_size_v30(inode);
	return hisize;
}
nc_size_t
dm_calc_header_size_v30(fc_inode_t *inode)
{
	nc_size_t			hisize = 0;
	int 				Bsize;
	int 				Psize;;
	int 				LPsize;;
	int 				Ssize;;
	int 				Basesize;;
	int 				pathsize;;

	
	hisize = (Basesize = sizeof(fc_header_info_v30_t)); 
	if (INODE_SIZE_DECLED(inode)) {
#ifdef DM_FLATFORMAT
		if (inode->ob_sizedecled) {
			hisize += (Bsize = NC_CANNED_BITMAP_SIZE(inode->bitmaplen));
			hisize += (Psize = NC_CANNED_CRC_SIZE(inode->bitmaplen));
		}
#else
		hisize += (Bsize = NC_CANNED_BITMAP_SIZE(inode->bitmaplen));
		hisize += (LPsize = NC_CANNED_LPMAP_SIZE(inode->bitmaplen));

#ifdef NC_ENABLE_CRC
		hisize += (Psize = NC_CANNED_CRC_SIZE(inode->bitmaplen));
#else
		hisize += (Psize = 0);
#endif


#endif
		/* vol_signature */
		DEBUG_ASSERT_FILE(inode, (inode->volume != NULL));
		hisize += (Ssize 	= vstring_size(inode->volume->signature));
		/* qpath */
		DEBUG_ASSERT_FILE(inode, (inode->q_path != NULL));
		hisize += (pathsize	= 	vstring_size(inode->q_path)); 
		/* qid */
		DEBUG_ASSERT_FILE(inode, (inode->q_id != NULL));
		hisize += (pathsize	= 	vstring_size(inode->q_id));
	}	
	else {
		hisize = NC_BLOCK_SIZE;
	}

	TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u]-Base=%d+Bsize=%d+LPsize=%d+CRC =%d+SIG=%d+path=%d => header=%lld\n",
					(long)inode->volume->signature, 
					(char *)inode->q_id, 
					inode->uid, 
					Basesize, 
					Bsize, 
					LPsize, 
					Psize,  
					Ssize, 
					pathsize, 
					(long long)hisize));

	
	return hisize;
}


static int 
dm_sync_disk_object_v30(fc_inode_t *inode, size_t *hdrsiz, char *msg, nc_time_t sytime)
{
	int								hisize = 0;
	int								a_hisize = 0;
	fc_header_info_v30_t			*hi_v30 = NULL, *hi_org = NULL, *hi_org2 = NULL;
	int								n;
	int 							Bsize 	= 0;
	int 							Psize 	= 0;
	int 							LPsize 	= 0;
	char							dbuf[8192]="";
	char							*vbase = NULL;
	char							*bitmap = NULL;
	char 							*nextptr = NULL;
	int 							compressed = 0;
	size_t 							disk_writing_size = 0;
	int 							result = 0;
	nc_crc_t						crc = 0;
	char 							ibuf[1024];
	char							*endp;
	char							*LPmap = NULL;

	


	if (!DM_FD_VALID(inode->fd) || inode->ob_memres) {
		TRACE((T_WARN, "Volume[%s].CKey[%s].INODE[%u] - not valid FD[%d];%s\n", 
						inode->volume->signature,
						inode->q_id, 
						inode->uid, 
						inode->fd,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)
						));
		
		return 0;
	}

	/*
	 * 메모리 할당을 위해서 header정보 크기 파악
	 * 주의) v2.4대비 hisize 리턴값은 NC_ALIGN_SIZE 단위로 align된 값이 아님
	 */
	hisize = dm_calc_header_size_v30(inode);

	/*
	 * 각각의 bitmap 필드의 크기 계산
	 */
#ifdef DM_FLATFORMAT
	Psize 	= 0;
	Bsize 	= 0;
	Msize 	= 0;
	LPsize 	= 0;
#else
#ifdef NC_ENABLE_CRC
	Psize 	= NC_CANNED_CRC_SIZE(inode->bitmaplen);
#else
	Psize 	= 0;
#endif
	Bsize 	= NC_CANNED_BITMAP_SIZE(inode->bitmaplen);
	LPsize 	= NC_CANNED_LPMAP_SIZE(inode->bitmaplen);
#endif



#ifdef NC_ENABLE_CRC
	DEBUG_ASSERT_FILE(inode, (Psize  <= inode->crcsize));
#endif

	a_hisize = NC_ALIGNED_SIZE(hisize, NC_ALIGN_SIZE);


	hi_org2 = hi_org = hi_v30 = XMALLOC(a_hisize, AI_ETC);
	endp 	= (char *)hi_org  + a_hisize;

  	hi_v30->chdr.magic				= NC_MAGIC_V30;
	hi_v30->chdr.header_size		= hisize;
	hi_v30->chdr.flag  				= 0;
	hi_v30->chdr.disk_header_size 	= 0;

	hi_v30->block_size				= NC_BLOCK_SIZE;
  	hi_v30->size					= inode->size;
  	hi_v30->bitmaplen				= inode->bitmaplen;	
	hi_v30->obi.op_bit_s			= inode->obi.op_bit_s;
	hi_v30->mode 					= inode->imode;
	hi_v30->ctime 					= inode->ctime; 				/* 원본 생성 시각*/
	hi_v30->mtime 					= inode->mtime;				/* 원본 최종 변경 시각*/
	hi_v30->vtime 					= inode->vtime; 				/* 다음 원본 신선도 체크 시각(UTC?) */
	hi_v30->origincode				= inode->origincode; 		/*원본에서 제공한 응답 코드 */
	hi_v30->cversion				= inode->cversion; 		/* 1 : expire after st_vtime < now */

	memcpy(hi_v30->devid, inode->devid, sizeof(hi_v30->devid));				/* device id, ex: etag */
	dm_copy_uuid(hi_v30->uuid, inode->uuid);

	nextptr = bitmap = vbase =  (char *)hi_v30->vbase;

	/* #1: volume volume signature 저장 */
	nextptr = vstring_pack(nextptr, (int)(endp - nextptr), inode->volume->signature, -1);
	/* #2: 원본 경로 저장 */
	nextptr = vstring_pack(nextptr, (int)(endp - nextptr), inode->q_path, -1);
	/* #3: 캐싱 객체 key-id 저장 */
	nextptr = vstring_pack(nextptr, (int)(endp - nextptr), inode->q_id, -1);

	/*
	 *****************************************************************************
	 * VARIABLE CHUNK #1 : BITMAP
	 * 청크의 디스크 캐싱 여부를 관리하는 bitmap 정보의 저장 포인터 설정
	 *****************************************************************************
	 */
	vbase = bitmap = nextptr;
	/*
	 * overflow 체크
	 */
	if (inode->ob_complete || inode->bitmap == NULL) {
		/*
		 * full-caching이므로 그냥 FF로 저장
		 */
		memset((char *)bitmap, 0xFF, Bsize);
	}
	else {
		/*
		 * 부분 캐싱이므로 bitmap copy
		 */
		bitmap_copy((unsigned long *)bitmap, (unsigned long *)inode->bitmap, inode->bitmaplen);
	}

#ifdef NC_ENABLE_CRC

	/*
	 *****************************************************************************
	 * VARIABLE CHUNK #2 : CRC
	 * CRC 저장 위치 : vbase + Bsize(비트맵 크기)
	 *****************************************************************************
	 */
	{

		char	*crcp;

		crcp = (char *)vbase + Bsize; /* calculate the start offset of parity block */
		DEBUG_ASSERT_FILE(inode, (Psize  <= inode->crcsize));
		memcpy(crcp, inode->blockcrc, Psize);
	}

#endif

    /*
     *****************************************************************************
     * VARIABLE CHUNK #3 : LPMAP(Logical->Physical Block Mapping Table)
     * LPMAP의 위치는 vbase + Bsize(비트맵 크기) + CRC 크기
     *****************************************************************************
     */
    int     mapped = 0;
    LPmap = (char *)vbase + Bsize + Psize;

    /*
     * overflow 체크
     */
    if ((nc_uint64_t)((char *)LPmap + LPsize) > (nc_uint64_t)((char *)hi_v30 + a_hisize)) {
        TRACE((T_WARN, "{%s] - bitmaplen=%d, header_size=%d, Bsize=%d, Psize=%d, LPmap size=%d, but LPmap copy overflow!(0x%p + %d > 0x%p + %d(hisize)  \n",
                        dm_dump_lv1(dbuf, sizeof(dbuf), inode),
                        inode->bitmaplen,
                        a_hisize,
                        Bsize,
                        Psize,
                        LPsize,
                        LPmap,
                        LPsize,
                        hi_v30,
                        a_hisize));
    }
	memcpy(LPmap, inode->LPmap, LPsize);

    DEBUG_ASSERT((nc_uint8_t *)LPmap + sizeof(nc_uint32_t)*mapped <= (nc_uint8_t *)hi_org + a_hisize);




	/*
	 * header 정보의 무결성 체크를 위한 CRC 생성 및 저장
	 */
#ifdef DM_FLATFORMAT
	if (inode->ob_sizedecled)
		crc = hi_v30->crc = do_crc32((char *)hi_v30, hisize);
#else
	crc = hi_v30->crc = do_crc32((char *)hi_v30, hisize);
#endif /* FLATFORMAT */


	if (hi_v30->chdr.header_size > pm_get_align_size())  {
		/*
		 * 한 페이지 이상 크기일 경우에만 압축 저장
		 */
		nc_uint8_t 	*t_mem = XMALLOC(a_hisize, AI_ETC);
		nc_int32_t 	 t_size = hi_v30->chdr.header_size - sizeof(fc_common_header_t);
		nc_int32_t 	 t_compsize = a_hisize;
		/*
		 * 임시 메모리로, 패킹된 데이타의 이동
		 */
		memcpy(t_mem, (nc_uint8_t *)hi_v30 + sizeof(fc_common_header_t), t_size);
		if (dm_compress_vheader((nc_uint8_t *)hi_v30 + sizeof(fc_common_header_t), &t_compsize, t_mem, t_size) < 0) {
			TRACE((T_ERROR, "INODE[%u]/R[%d] - header size=%ld failed to deflate\n", inode->uid, inode->refcnt, inode->header_size));
			TRAP;
		}
		t_compsize += sizeof(fc_common_header_t);
		TRACE((T_INODE, "INODE[%u]/R[%d] - header size=%ld, compress info:%ld => %d bytes (CRC=%08X)\n", 
							(int)inode->uid, 
							(int)inode->refcnt, 
							(long)hisize, 
							(long)t_size,
							(long)t_compsize,
							(nc_crc_t)crc
							));
		hi_v30->chdr.disk_header_size 	= t_compsize;
		disk_writing_size 				= t_compsize;
		hi_v30->chdr.flag  				|= NC_HEADER_FLAG_COMPRESSED;
		XFREE(t_mem);
		compressed ++;
	}
	else {
		hi_v30->chdr.disk_header_size = hi_v30->chdr.header_size = hisize;
		disk_writing_size = hi_v30->chdr.header_size;
	}
	disk_writing_size = NC_ALIGNED_SIZE(disk_writing_size, NC_ALIGN_SIZE);
	*hdrsiz = disk_writing_size;


	/*
	 * header정보를 caching 파일의 헤더에 저장 (옵셋 0)
	 */
	n = dm_write_bytes(inode, (char *)hi_v30, disk_writing_size, 0/*offset*/);
	if (n < disk_writing_size) {
		TRACE((T_ERROR, "Volume[%s].CKey[%s].Cache[%s]: header_size:%d, return:%d, writing header  returns ERROR=%d:%s\n", 
						inode->volume->signature, 
						inode->q_id, 
						inode->c_path, 
						disk_writing_size, 
						n, 
						errno, 
						dm_dump_lv0(dbuf, sizeof(dbuf), inode)));
		result = -1;
	}

	XFREE(hi_org);

	
	return result;
}




static int
dm_sync_disk_object(fc_inode_t *inode, int bforce, size_t *hdrsiz, char *msg)
{
	int		r = 0;
	nc_time_t 	oldver = 0;


	DEBUG_ASSERT_FILE(inode, (inode->ob_doc == 0));
	DEBUG_ASSERT_FILE(inode, (inode->ob_memres == 0));
	DEBUG_ASSERT_FILE(inode, (inode->ob_validuuid != 0));
	if (inode->ob_staled != 0) {
		char 	dbuf[2048];
		TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%d] already staled;%s\n",
						inode->volume->signature,
						inode->q_id,
						inode->uid,
						dm_dump_lv1(dbuf, sizeof(dbuf), inode)
						));
		TRAP;
	}
#ifndef WIN32
	DEBUG_ASSERT_FILE(inode, (uuid_is_null(inode->uuid) == U_FALSE));
#endif
	DEBUG_ASSERT_FILE(inode, (inode->hdr_version == INODE_HDR_VERSION_V30));
	oldver = inode->headerversion;
	r = dm_sync_disk_object_v30(inode, hdrsiz, msg, oldver);
	
	if (r >= 0) {
		/* 최근 동기화 시간 저장*/
		inode->lastsynctime 	= nc_cached_clock();
		inode->headerversion 	-= oldver;
		TRACE((T_INODE, "INODE[%u]/R[%d]/Cache[%s] - header_size=%ld B(compressed=%ld B), synced on disk(upgrade=%d)\n", 
						inode->uid, 
						inode->refcnt, 
						inode->c_path, 
						(long)inode->header_size, 
						(long)*hdrsiz, 
						inode->ob_upgraded));
	}
	return r;
}
/*
 * 
 * rename operation flow
 * 주의 
 * 	path-lock이 src, dest 양쪽에 모두 걸려있다는 전제하에 호출 및 수행됨
 *
 * Concerned infomation
 *  - inode : 
 * 		- memres & writable : used to determine if meta db operation is necessary.
 * 		- refcnt : used to know if it's already opened.
 * 		- loaded : used to if we conduct mdb operation.
 * 		- X : extent info  needed to call dm_sync_object
 * 		- 
 * Flow
 * 		if (inode found in VIT)
 * 			if (refcnt > 0)
 *				update VIT
 * 				inode->qpath update
 *				remove old mdb record
 *				add renamed mdb record
 *			else
 *				remove all cache objects
 *
 *
 */



#if 0
static void
dm_fill_white(fc_inode_t *inode, nc_off_t 	offset, nc_off_t size)
{
	char 		buff[4] = {0,0,0,0};
	nc_off_t 	markoff ;
	int 		copied = 0;

	markoff 	= size - 1;
	TRACE((T_INODE, "INODE[%u] - writing EOF mark by 1 byte 0x00\n", inode->uid));
	copied = blk_write(inode, buff, markoff, 1, NULL);
	if (copied != 1) {
		TRACE((T_ERROR, "INODE[%u]/REF[%d] - write error at offset %lld\n", inode->uid, inode->refcnt, markoff));
	}

}
#endif
void
dm_update_viewcount(fc_inode_t *inode)
{
	DEBUG_ASSERT(inode != NULL);
	_ATOMIC_ADD(inode->viewcount, 1);
}
char *
dm_dump_lv2(char *dbuf, int remained, fc_inode_t *inode)
{
	char 	bbuf[128]="";
	char	zuuid[64];
	char	*dbuf_org = dbuf;

	remained-=10; /* to reserve some bytes for null terminating char */
	snprintf(dbuf, 
				remained, 
				"INODE[%u]/R[%d]/Sz[%lld]/UUID[%s]/O.S[%d]/CVer[%u]/FD[%d]/IO[%d]/bits{%s}",
				inode->uid,
				(int)inode->refcnt,
				(long long)inode->size,
				uuid2string(inode->uuid, zuuid, sizeof(zuuid)),
				(int)(inode->origin?1:0),
				inode->cversion,
				inode->fd,
				link_count(&inode->pending_ioq, U_TRUE),
				(char *)obi_dump(bbuf, sizeof(bbuf), &inode->obi)
				);
	return dbuf_org;
}
char *
dm_dump_lv1(char *dbuf, int remained, fc_inode_t *inode)
{
	int					fm = 0x5AA55AA5;
	char 				bbuf[128]="";
	char 				zuuid[64]="";
	char 				mbuf[128]="";
	char				*dbuf_org = dbuf;
	int					rm = 0x5AA55AA5;
	static 	char 		_istat[][32] = {"IS_FREED:0", 	
										"IS_ORPHAN:1", 
										"IS_CACHED:2"
										};


	remained-=10; /* to reserve some bytes for null terminating char */
	snprintf(dbuf, 
				remained, 
				"INODE[%u]/V.K[%s/%s]/R[%d].(%s)/MaxBlk[%u]/Sz[%lld,%lld]/CVer[%u]/ROW[%lld]/FD[%d]:(%s.%s/O[%d]/IOB[%d]/VT[%lld]/MT[%lld]/DEV[%s]/UUID[%s]/[%s]/PIO[%d]/O.S[%d]/D.O[%lld]/Sig[%llu]/P[%s]/WQ[%d]",
				inode->uid,
				(inode->volume?inode->volume->signature:"NULL"),
				inode->q_id,
				(int)inode->refcnt,
				((inode->inodebusy!=0)?"BUSY":""),
				inode->maxblkno,
				(long long)inode->size,
				(long long)inode->rsize,
				(nc_uint32_t)inode->cversion,
				(long long)inode->rowid,
				(int)inode->fd,
				(char *)mode_dump(mbuf, sizeof(mbuf), inode->imode),
				(char *)obi_dump(bbuf, sizeof(bbuf), &inode->obi),
				inode->origincode,
				(inode->iobusy!=0),
				(long long)inode->vtime,
				(long long)inode->mtime,
				(char *)(isprint(inode->devid[0])?inode->devid:""),
				uuid2string(inode->uuid, zuuid, sizeof(zuuid)),
				(char *)_istat[inode->cstat],
				(int)link_count(&inode->pending_ioq, U_TRUE), /* lock이 없어서 부정확할 수도*/
				(int)(inode->origin?1:0),
				(long long)inode->doffset,
				(long long)inode->signature,
				(inode->part?inode->part->path:"NULL"),
				(int)link_count(&inode->ctx_waitq, U_TRUE)

				);
	DEBUG_ASSERT(fm == rm);
	return dbuf_org;
}
char *
dm_dump_lv0(char *dbuf, int remained, fc_inode_t *inode)
{
	int					fm = 0x5AA55AA5;
	char 				bbuf[128]="";
	char 				zuuid[64]="";
	char				*dbuf_org = dbuf;
	int					rm = 0x5AA55AA5;
	static 	char 		_istat[][32] = {"IS_FREED:0", 	
										"IS_ORPHAN:1", 
										"IS_CACHED:2"
										};

	remained-=10; /* to reserve some bytes for null terminating char */
	snprintf(dbuf, 
				remained, 
				"INODE[%u]/R[%d].(%s)/Sz[%lld,%lld]/CVer[%u]/ROW[%lld]/FD[%d]:%s/Code[%d]/HID[%llu]/UUID[%s]/VT[%lld]/MT[%lld]/DEV[%s]/[%s]/PIO[%d]/O.S[%d]/D.O[%u]",
				inode->uid,
				(int)inode->refcnt,
				((inode->inodebusy!=0)?"BUSY":""),
				(long long)inode->size,
				(long long)inode->rsize,
				(int)inode->cversion,
				inode->rowid,
				(int)inode->fd,
				(char *)obi_dump(bbuf, sizeof(bbuf), &inode->obi),
				(int)inode->origincode,
				(long long)inode->hid,
				(char *)uuid2string(inode->uuid, zuuid, sizeof(zuuid)),
				(long long)inode->vtime,
				(long long)inode->mtime,
				(char *)(isprint(inode->devid[0])?inode->devid:""),
				(char *)_istat[inode->cstat],
				(int)link_count(&inode->pending_ioq, U_TRUE), /* lock이 없어서 부정확할 수도*/
				(inode->origin?1:0),
				inode->doffset
				);
	DEBUG_ASSERT(fm == rm);
	return dbuf_org;
}
int
dm_detach_driver(fc_inode_t *inode, int needlock)
{
	void				*dp = inode->driver_file_data;

	
	if (inode->volume && inode->driver_file_data) {
		cfs_close(inode->volume, inode->driver_file_data);
		inode->driver_file_data = NULL;
	}
	TRACE((T_INODE, "INODE[%d] - driver closed(%p=>%p):(volume=%p)\n", inode->uid, dp, inode->driver_file_data, inode->volume));
	return  0;
}
/*
 * 수정 필요
 */

/*
 * *********************************************************************************8
 * *********************************************************************************8
 * *********************************************************************************8
 * P U B L L I C      O P E R A T I O N S
 * *********************************************************************************8
 * *********************************************************************************8
 */
#ifndef NC_RELEASE_BUILD
__thread int __inode_locked__ = 0;
#endif

PUBLIC_IF CALLSYNTAX void
dm_inode_lock(fc_inode_t *inode, char *f, int l)
{
	LO_CHECK_ORDER(LON_INODE);
	LO_PUSH_ORDER_FL(LON_INODE,f, l);

	pthread_mutex_lock(&inode->lock);
#ifndef NC_RELEASE_BUILD
	_ATOMIC_ADD(__inode_locked__, 1);
#endif

	TRACE((0, "INODE[%d] - locked at %d@%s\n", inode->uid, l, f));

}
PUBLIC_IF CALLSYNTAX int
dm_inode_check_owned(fc_inode_t *inode)
{
	return ((&inode->lock)->__data.__owner == trc_thread_self());

}
PUBLIC_IF CALLSYNTAX int
dm_inode_locked()
{
#ifndef NC_RELEASE_BUILD
	return _ATOMIC_ADD(__inode_locked__, 0);
#else
	return 0;
#endif
}

PUBLIC_IF CALLSYNTAX void
dm_inode_unlock(fc_inode_t *inode, char *f, int l)
{

#ifndef NC_RELEASE_BUILD
	_ATOMIC_SUB(__inode_locked__, 1);
#endif
	LO_POP_ORDER(LON_INODE);
	pthread_mutex_unlock(&inode->lock); 
	TRACE((0, "INODE[%d] - *UN*locked at %d@%s(locked=%d)\n", inode->uid, l, f, dm_inode_check_owned(inode)));
}

/*
 * applied only to disk-cache manager
 * block manager can lock the inode at any time.
 * yielding lock is necessary to allow block-io manager to freely 
 * access inode. The other operations of disk-cacache manager not
 * permitted in the lock yielding state.
 */




/*
 *
 * 항상 IC_LOCK내에서 호출됨
 *
 */
static nc_uint32_t 			___id_counter = 1; 
fc_inode_t *
dm_new_inode(nc_int32_t uid)
{
	fc_inode_t		*inode = NULL;

#ifndef UNLIMIT_MAX_INODES
	if (link_count(&__inode_pool, U_TRUE) >= g__inode_max_count)
		return NULL; /* no more allocation */
#endif
	inode = (fc_inode_t *)mem_alloc(&g__inode_heap, sizeof(fc_inode_t), U_FALSE, AI_INODE, U_FALSE, __FILE__, __LINE__);
	if (inode) {

		inode->uid  = ___id_counter++;
		if (___id_counter == 0) ___id_counter = 1;

		memset(&inode->nav_node, 0, sizeof(inode->nav_node));
		INODE_RESET(inode);
		bt_init_timer(&inode->t_defer, "deferring timer", U_FALSE);
		bt_init_timer(&inode->t_origin, "origin expiration", U_FALSE);
		LRU_init_node(&inode->node, __func__, __LINE__);
		{
			pthread_mutexattr_t	mattr;
			pthread_mutexattr_init(&mattr);
			pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
			pthread_mutex_init(&inode->lock, &mattr);
		}


#ifdef EXPERIMENTAL_USE_SLOCK
		pthread_spin_init(&inode->slock, PTHREAD_PROCESS_PRIVATE);
#endif
		/*
		 * 관리용 list에 등록
		 */
		link_append(&__inode_pool, inode, &inode->nav_node);
		g__inode_count++;
	}
	return inode;
}
int
dm_init(char *path, size_t cap, dm_reclaim_inode_callback_t proc)
{
	int 		i;
	pthread_mutexattr_t	mattr;
	int		ia = 0;

	


	if (!__memcache_mode && pm_check_partition()) {
		__dm_state.nodisk = 0;
	}
	else {

		TRACE((T_INFO, "Disk cache storage not specified, running in memory-only caching mode\n"));
		__dm_state.nodisk = 1;
	}
	pthread_mutexattr_init(&mattr);
	if (pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE)) {
		TRACE((T_ERROR,"mutex set to RECURSIVE failed:'%s'\n", strerror(errno)));
	}

	__dm_state.fs_usedper	= 0.0;
	__dm_state.reclaiming 	= 0;
	__dm_state.count		= 0;
	__dm_state.cap			= cap;
	__dm_state.state		= DMS_INIT;
	__dm_state.proc			= proc;
	ic_init();
	pthread_mutexattr_init(&mattr);
	if (pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE)) {
		TRACE((T_ERROR,"mutex set to RECURSIVE failed:'%s'\n", strerror(errno)));
	}

	__dm_state.notrace=1;
	ia = i;
	pthread_mutexattr_destroy(&mattr);



	__dm_state.notrace=0;

	if (!__dm_state.nodisk) {
		dm_update_partition_info(path);;
	}
	
	return 0;
}
int
dm_forcely_close_inode(fc_inode_t *inode, void *cbdata)
{
	int			i = 0;
	int			wri = 0;
	char		buf[4096];


	if (INODE_GET_REF(inode) > 0) {
		TRACE((T_WARN, "Volume[%s].Key[%s] - still opened, forcely closing...:%s\n",
						inode->volume->signature,
						inode->q_id,
						dm_dump_lv1(buf, sizeof(buf), inode)
			  ));

		wri = dm_is_writable(inode->imode);

		for (i = 0; i < INODE_GET_REF(inode); i++) {
			dm_close_inode(inode, wri, U_FALSE);
		}
	}
	return 0;
}

void
dm_dump_inodes()
{
	fc_inode_t 		*inode = NULL;
	char 			ibuf[1024];
	long			cnt_busy = 0;
	long			cnt_cached = 0;
	int				cnt_free = 0;
	long			cnt_allocated = 0;

	IC_LOCK(CLFU_LM_EXCLUSIVE);
	inode = link_get_head_noremove(&__inode_pool);
	while  (inode) {
		cnt_allocated++;
		if (dm_inode_idle(inode) == 0) {
			TRACE((T_MONITOR, "INODE[%u] - CKey[%s];{%s} might be leak\n", 
							inode->uid,
							inode->q_id,
							dm_dump_lv1(ibuf, sizeof(ibuf)-10, inode)
							));
			cnt_busy++;
		}

		if (ic_is_cached_nolock(inode))
			cnt_cached++;
		inode = link_get_next(&__inode_pool, &inode->nav_node);
	}
	ic_get_stat(NULL, NULL, NULL, NULL, NULL, &cnt_free);
	TRACE((T_MONITOR, "INODE info summary: allocated=%ld, cached=%ld, free=%d, busy=%ld\n", cnt_allocated, cnt_cached, cnt_free, cnt_busy));
	IC_UNLOCK;
}

/*
 * inode를 새로 할당하고, 필요한 속성을 모두 설정한 뒤 리턴한다.
 * 리턴 시 조건
 * 	inode - 
 * 		lock이 해제된 상태, 필요 시 VIT에 등록 및 CLFU에 등록된 상태
 *		refcnt가 1만큼 증가한 상태
 */ 
int
dm_check_public(char *path, nc_stat_t *stat, nc_mode_t mode)
{
	int 	r;
	if (!stat) return U_TRUE; /* null stat의 경우 공유 캐시로 가정*/

	r = !((stat->st_private != 0) || (IS_ON(mode, O_NCX_PRIVATE) != 0));

	return r;
}
static char *
dm_size_info(fc_inode_t *inode, char *buf, int bufsiz)
{
	int			n;
	int			remained = bufsiz;
	char		dbuf[128];
	char		*buforg = buf;

	*buf = 0;

	if (inode->ob_sizedecled) {
		n = snprintf(buf, remained, "%s", ll2dp(dbuf, sizeof(dbuf)-10, (long long)inode->size));
		remained -= n;
		buf += n;
		if (inode->ob_sizeknown) {
			n = snprintf(buf, remained, "(real: %s)",  ll2dp(dbuf, sizeof(dbuf)-10, (long long)dm_inode_size(inode, U_FALSE)));
			remained -= n;
			buf += n;
		}
	}
	else {
		n = snprintf(buf, remained, "%s", "not decled");
		remained -= n;
		buf += n;

		if (inode->ob_sizeknown) {
			n = snprintf(buf, remained, "(real: %s)",  ll2dp(dbuf, sizeof(dbuf)-10, (long long)dm_inode_size(inode, U_FALSE)));
			remained -= n;
			buf += n;
		}
	}
	*buf = 0;
	return buforg;
}
/*
 * 신규로 캐싱 객체 생성. 
 * 이 함수는 객체가 캐싱되지 않은 경우에만 해야함
 * assumption) path-lock이 걸려있음
 */

/*
 * 객체에 대한 캐싱 객체를 생성하거나 존재하면, 해당 객체를 리턴
 * 주의
 * 		- 해당 객체에 대한 open요청 전에 객체에 대한 경로에 대해서 path-lock을 무조건할당받아야한다
 * 		  path-lock없이 진행하는 경우 racing 컨디션 등 문제 발생
 * NOTE!)
 *		PERMISSION ERROR:
 *			(1) if an inode is already opened in read mode, 
 *				opening with write flag is not permitted
 */
/*
 * 이버전은 vary처리 과정에서 vary template에 대한 meta inode를 생성한다.
 */
static int
dm_make_report(fc_inode_t *inode, nc_xtra_options_t *kv, nc_hit_status_t 	hs)
{
	char 		hq_path[1024];
	char 		hq_id[1024];
	char 		mtime[64]="";
	char 		vtime[64]="";
	char 		mbuf[64];
	char 		pbuf[1024];
	char 		rbuf[256];

	if (hs ==  NC_OS_MISS ||
		hs == NC_OS_BYPASS ||
		TRACE_ON(T_INODE)) {
		filetime(mtime, sizeof(mtime), inode->mtime);
		filetime(vtime, sizeof(vtime), inode->vtime);
		if (isprint(inode->devid[0])) {
			TRACE((T_INODE|T_INFO, "Volume[%s]/Origin[%s] - %s\n"
						   "\t\t\t ID(KEY)           : %s\n"
						   "\t\t\t Modification time : %s\n"
						   "\t\t\t Valid until       : %s\n"
						   "\t\t\t Dev-ID            : %s\n"
						   "\t\t\t Size              : %s\n"
						   "\t\t\t Object-properties : {%s}\n"
						   "\t\t\t Opening mode      : %s\n"
						   "\t\t\t Caching mode      : %s\n"
						   "\t\t\t Response code     : %d\n"
						   "\t\t\t Allocated inode   : %d\n",
							inode->volume->signature, 
							(char *)hz_string(hq_path, sizeof(hq_path)-10,inode->q_path), 
							__p_stat[hs],
							(char *)hz_string(hq_id, sizeof(hq_id)-10,inode->q_id), 
							(char *)((inode->mtime != 0)?(char *)mtime:"unknown"),
							(char *)((inode->vtime != 0)?(char *)vtime:"unknown"),
							inode->devid,
							dm_size_info(inode, rbuf, sizeof(rbuf)),
							(char *)obi_dump(pbuf, sizeof(pbuf)-10, &inode->obi),
							(char *)mode_dump(mbuf, sizeof(mbuf), inode->imode),
							(char *)(dm_disk_cacheable(inode, inode->origincode)?"disk":"memory-only"),
							inode->origincode,
							inode->uid
							));
		}
		else {
			TRACE((T_INODE|T_INFO, "Volume[%s]/Origin[%s] - %s\n"
						   "\t\t\t ID(KEY)           : %s\n"
						   "\t\t\t Modification time : %s\n"
						   "\t\t\t Valid until       : %s\n"
						   "\t\t\t Size              : %s\n"
						   "\t\t\t Object-properties : {%s}\n"
						   "\t\t\t Caching mode      : %s\n"
						   "\t\t\t Response code     : %d\n"
						   "\t\t\t Allocated inode   : %d\n",
							inode->volume->signature, 
							(char *)hz_string(hq_path, sizeof(hq_path)-10,inode->q_path), 
							__p_stat[hs],
							(char *)hz_string(hq_id, sizeof(hq_id)-10,inode->q_id), 
							(char *)((inode->mtime != 0)?(char *)mtime:"unknown"),
							(char *)((inode->vtime != 0)?(char *)vtime:"unknown"),
							dm_size_info(inode, rbuf, sizeof(rbuf)),
							(char *)obi_dump(pbuf, sizeof(pbuf)-10, &inode->obi),
							(char *)(dm_disk_cacheable(inode, inode->origincode)?"disk":"memory-only"),
							inode->origincode,
							inode->uid
							));
		}
	}
	nvm_report_hit_info(inode->volume, kv, hs);
	return 0;
}
static int
dm_inquiry_property(apc_open_context_t *oc)
{
	nc_stat_t 	orgstat;
	char 		ibuf[1024];
	char 		cbuf[1024];
	int 		ifon = 0;
	int			r = 0;


	if (oc->inode &&  oc->inode->ob_complete) {
		/*
		 * IMS(If-Modified-Since) 또는 If-Match를
		 * 사용할 조건인지 확인하고
		 * 맞다면 IMS 태그 생성을 위한 정보를 복사
		 *
		 */
		TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%u] IMS on\n", oc->inode->volume->signature, oc->inode->q_id, oc->inode->uid));
		dm_copy_stat(&orgstat, oc->inode, U_FALSE); /* copy the cached property for IMS */
		ifon++;
	}

	/*
	 * cfs_getattr 함수 호출
	 * 주의) 이함수는 이전 버전과 달리 순수 비동기 함수임.
	 * context를 넘겨주고 완료 후에 메시지로 결과 받음
	 */
	/* CHG 2018-05-10 huibong cfs_getattr() 호출 결과를 local 변수 r 에 저장하도록 수정 (#32089) */

	r = cfs_getattr(	oc->volume
						, oc->origin_path 
						, (ifon ? (&orgstat) : NULL)
						, &oc->query_result.stat
						, oc->in_prop
						, oc->mode
						, oc);
	if (r < 0) {
		oc->query_result.result 	= -r;
		asio_post_apc(oc, __FILE__, __LINE__);
	}

	TRACE((T_INODE, "INODE{%s}/CONTEXT{%s}/ifon[%d] - got %d from cfs_getattr()\n", 
					(oc->inode?dm_dump_lv2(ibuf, sizeof(ibuf), oc->inode):"NULL"),
					dm_dump_open_context(cbuf, sizeof(cbuf),oc),
					ifon,
					r));

	return r;
}

#if NOT_USED
/*
 * driver에서 호출됨
 */
static int
dm_apc_done(void *ud, int error)
{
	apc_open_context_t 	*oc = (apc_open_context_t *)ud;	

	oc->query_result.result = error;
	TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - OC[%p]/[%s]/OSI[%p] posting event(prop=%p,error=%d)\n", 
					oc->volume->signature, 
					oc->origin_path, 
					(oc->inode?oc->inode->uid:-1), 
					oc, 
					__z_apc_open_str[oc->state],  
					oc->callback_data, 
					oc->query_result.stat.st_property, 
					oc->query_result.result));
	asio_post_apc(ud, __FILE__, __LINE__);
	return 0;
}
static void *
dm_apc_context_hang(void *u)
{
	apc_open_context_t *oc = (apc_open_context_t *)u;
	nc_path_lock_t 	*pl = NULL;
	TRACE((T_ERROR, "Volume[%s].CKey[%s]/OC[%p]:[%s]/INODE[%u]/PRIV[%d] - TIMEOUT(pathlock=%p, state=%d)\n", 
					oc->volume->signature, 
					oc->cache_key, 
					oc, 
					__z_apc_open_str[oc->state], 
					(oc->inode?oc->inode->uid:-1), 
					(IS_ON(oc->mode, O_NCX_PRIVATE)?1:0), pl, (pl?pl->state:-1)
					));
	TRAP;
}
#endif
static int
dm_apc_schedule_async_open(apc_open_context_t *soc, int state, char *f, int l)
{
	apc_open_context_t *		aoc = NULL;
	char						ibuf[2048];
	char						zpath[2048];

	DEBUG_ASSERT(VOLUME_INUSE(soc->volume));
	DEBUG_ASSERT(nvm_path_lock_is_for(soc->pathlock, soc->origin_path));
	DEBUG_ASSERT(nvm_path_lock_ref_count(soc->pathlock) > 0);
	DEBUG_ASSERT(nvm_path_lock_owned(soc->volume, soc->pathlock));


	aoc 			= dm_apc_prepare_context(soc, state);

	DEBUG_ASSERT(aoc->origin  == NULL);

	aoc->origin 	= dm_allocate_origin_session(aoc);

	if (!nvm_path_busy_nolock(aoc->pathlock))
		nvm_path_inprogress_nolock(aoc->pathlock, U_TRUE, __FILE__, __LINE__);

	TRACE((T_INODE, "Volume[%s].CKey[%s].INODE{%s}/CONTEXT[%p] (inherit from OC[%p]) on (%s) - scheduling for inquiry(%s) at %d@%s\n", 
					aoc->volume->signature,
					(aoc->inode?aoc->inode->q_id:aoc->cache_key),
					(aoc->inode?dm_dump_lv2(ibuf, sizeof(ibuf), aoc->inode):"NULL"),
					aoc,
					soc,
					nvm_path_lock_dump(zpath, sizeof(zpath), aoc->pathlock),
					__z_apc_open_str[state],
					l,
					f
					));

	VOLUME_REF(soc->volume);

	dm_inquiry_property(aoc);/* send of a inquiry and returned */

#ifndef NC_RELEASE_BUILD
	{
		extern __thread int	t_scheduled; 
		t_scheduled++;
		DEBUG_ASSERT(t_scheduled <= 1);
	}
#endif

	return DM_OPENR_SCHEDULED;
}
static void
apc_open_context_init(	apc_open_context_t 			*soc, 
						nc_volume_context_t 		*volume, 
						char 						*origin_path, 
						char 						*cache_id, 
						nc_mode_t 					mode, 
						nc_xtra_options_t 			*kv, 
						nc_path_lock_t				*pl,
						int							pl_makeref, 
						nc_uint32_t					ubi,
						nc_apc_open_callback_t 		callback, 
						void 						* osi)
{

	char		cbuf[1024];


	//WEON:::(try) memset(soc, 0, sizeof(apc_open_context_t));
	soc->volume 			= volume;
	soc->origin_path		= (char *)origin_path;
	soc->in_prop			= kv;
	soc->state				= APC_OS_INIT;
	soc->mode				= mode;
	soc->inode				= NULL;
	soc->u._u32				= ubi;
	soc->open_result		= DM_OPENR_FRESH;
	soc->force_private		= IS_ON(mode, O_NCX_PRIVATE);
	soc->force_nocache		= IS_ON(mode, O_NCX_NOCACHE);
	soc->callback			= callback; 
	soc->callback_data		= osi;
	soc->cache_key_given 	= cache_id;
	soc->cache_key 			= cache_id;
	soc->pathlock 			= pl;
	memset(&soc->query_result,0, sizeof(soc->query_result));


	TRACE((T_INODE, "Volume[%s].CKey{%s] - context inited;{%s}\n", 
					volume->signature, 
					cache_id, 
					dm_dump_open_context(cbuf, sizeof(cbuf), soc)));
}

/*
 * clone context for async op
 */
static apc_open_context_t *
dm_apc_prepare_context(apc_open_context_t *soc, int state)
{
	apc_open_context_t *		aoc = NULL;


	aoc = dm_apc_prepare_context_raw(	soc->volume,
										soc->inode,
										soc->origin_path,
										soc->cache_key_given,
										soc->cache_key,
										soc->mode,
										state,
										soc->in_prop,
										soc->pathlock,
										soc->u._u32,
										soc->callback,
										soc->callback_data);


	return aoc;
}
apc_open_context_t *
dm_apc_prepare_context_raw(		nc_volume_context_t 	*volume, 
								fc_inode_t				*inode,
								char 					*origin_path, 
								char 					*cachekey_given, 
								char 					*cachekey, 
								nc_mode_t 				mode, 
								int 					state,
								nc_xtra_options_t 		*inprop, 
								nc_path_lock_t			*pl, 
								nc_uint32_t				ubi,
								nc_apc_open_callback_t 	callback, 
								void 					*osi)
{
	apc_open_context_t 		*aoc;


	aoc = (apc_open_context_t *)XMALLOC(sizeof(apc_open_context_t), AI_OPENCTX);

	aoc->type				= APC_OPEN;


	apc_open_context_init(	aoc, 
							volume, 
							origin_path, 
							aoc->cache_key, /* 아래에서 설정, debugging 출력용 set */
							mode, 
							inprop, 
							pl,
							U_TRUE,
							ubi,
							callback, 
							osi);

	/*
	 * 비동기 operation이므로 function call때 주어진 string pointer는 
	 * return 이후 free될 수 있음. dup 필요
	 */
	aoc->inode				= inode;
	aoc->origin_path		= XSTRDUP(origin_path, AI_OPENCTX);
	aoc->cache_key_given 	= XSTRDUP(cachekey_given, AI_OPENCTX);
	aoc->cache_key 			= XSTRDUP(cachekey, AI_OPENCTX);
	aoc->open_result      	= DM_OPENR_FRESH;
	aoc->state				= state;
	aoc->pathlock			= pl;

	bt_init_timer(&aoc->t_hang, "open context hang timere", U_FALSE);

	/*
	 * add it to path-lock's waiter list
	 */
	_ATOMIC_ADD(g__oapc_count, 1);
	nvm_path_lock_ref_reuse(aoc->volume, aoc->pathlock, __FILE__, __LINE__);
	return aoc;
}

void
apc_destroy_open_context(apc_open_context_t *oc)
{
	char	lbuf[1024];
	char	xbuf[1024];
	int		sleep = 0;
	TRACE((T_INODE, "Volume[%s].Key[%s] - OC{%s} done;%s\n", 
					oc->volume->signature, 
					oc->cache_key, 
					dm_dump_open_context(xbuf, sizeof(xbuf), oc),
					nvm_path_lock_dump(lbuf, sizeof(lbuf), oc->pathlock)
					));
	nvm_path_lock_unref_reuse(oc->volume, oc->pathlock, __FILE__, __LINE__);


	while (bt_del_timer_v2(__timer_wheel_base,  &oc->t_hang) < 0) {
		bt_msleep(1000);
		sleep++;
	}
	while (bt_destroy_timer_v2(__timer_wheel_base,  &oc->t_hang) < 0) {
		bt_msleep(1);
	}

	if (sleep > 0) {
		TRACE((T_WARN, "Volume[%s].Key[%s] - OC{%s} timer busy for %d secs;%s\n", 
					oc->volume->signature, 
					oc->cache_key, 
					sleep,
					dm_dump_open_context(xbuf, sizeof(xbuf), oc),
					nvm_path_lock_dump(lbuf, sizeof(lbuf), oc->pathlock)
					));
	}

	if (oc->origin) {
		dm_free_origin_session(oc->volume, oc->origin, U_FALSE /*'cuase not bound */, __FILE__, __LINE__);
		oc->origin = NULL;
	}

	if (oc->query_result.stat.st_property) {
		kv_destroy(oc->query_result.stat.st_property);
		TRACE((T_INODE,  "PROP[%p] - destroying\n", oc->query_result.stat.st_property));
		oc->query_result.stat.st_property = NULL;
	}
	oc->state = APC_OS_FREE;

	if (oc->cache_key != oc->cache_key_given)
		XFREE(oc->cache_key);
	XFREE(oc->cache_key_given);


	XFREE(oc->origin_path);
	_ATOMIC_SUB(g__oapc_count, 1);


	XFREE(oc);
}


/*
 * 	offline 시 객체의 사용여부 판단 조건
 * 
 *	객체에 private, nocache,must-revalidate,must-expire가 있는 경우 : FALSE
 *	
 *	
 */
static int
dm_inode_offline_accessible(nc_volume_context_t *volume, fc_inode_t *inode)
{
	
	u_boolean_t 	oa = U_FALSE;

	if (g__enable_offline_cache) {
		oa = (	!inode->ob_priv && 
					!inode->ob_nocache &&
					!inode->ob_mustreval  &&
					!inode->ob_mustexpire 
					);
		oa = inode->ob_complete && oa;
	}
	return oa;
}

static fc_inode_t *
dm_lookup_cache(nc_volume_context_t *volume, 
				int lookup_type, 
				char *cache_key, 
				u_boolean_t makeref, 
				u_boolean_t markbusy, 
				nc_path_lock_t *lock, 
				const char *f, 
				int l)
{
	fc_inode_t      *inode;
	int				dsk = 0;
	int				busy;
	int				load_if_need = U_TRUE;
	char 			ibuf[2048];
	perf_val_t		ts,te;
	long			ud;
	int				try = 0;
	int				busywait = 0;

	TRACE((T_INODE, "Volume[%s].CKey[%s] - makeref=%d trying %d@%s\n",
					volume->signature,
					cache_key, 
					makeref,
					l,
					f));
	DEBUG_ASSERT(nvm_path_lock_ref_count(lock) > 0);
	DEBUG_ASSERT(nvm_path_lock_owned(volume, lock) != 0);

	PROFILE_CHECK(ts);
L_try_lookup_again:
	try++;
	IC_LOCK(CLFU_LM_SHARED);
	do {
		busy = 0;
		inode = nvm_lookup_inode(volume, DM_LOOKUP_WITH_KEY, cache_key, U_FALSE, makeref, markbusy, &busy, (char *)f, l);
		if (busy) {
			busywait++;
			IC_UNLOCK;
			nvm_path_unlock(volume, lock, __FILE__, __LINE__);
			bt_msleep(10); /* retry after 10 ms without any lock */
			nvm_path_lock(volume, lock, __FILE__, __LINE__);
			IC_LOCK(CLFU_LM_SHARED);
		}
	} while (busy);
	IC_UNLOCK;

	if (!inode && load_if_need) {
		load_if_need = 0; /* to avoid re-entrance */
		inode = dm_load_disk_cache(volume, cache_key);
		dsk++;
		if (inode) {
			goto L_try_lookup_again;
		}
	}
	PROFILE_CHECK(te);

	ud = PROFILE_GAP_MSEC(ts, te);
	TRACE((T_INODE, "Volume[%s].CKey[%s]/makeref[%d] - INODE[%d] found(locked=%d, loaded=%d,busywait=%d) took %.2f sec;%s at %d@%s\n", 
					volume->signature, 
					cache_key, 
					makeref,
					(inode?inode->uid:-1),
					(inode?dm_inode_check_owned(inode):0),
					dsk,
					busywait,
					(float)(ud/1000.0),
					(inode?dm_dump_lv1(ibuf, sizeof(ibuf), inode):""),
					l,
					f
					));
	return inode;
}



static fc_inode_t *
dm_apc_open_inode_internal(apc_open_context_t *poc)
{
	int							ret;
	fc_inode_t					*inode_tofree 	= NULL;
	fc_inode_t					*inode_toreturn	= NULL;
	char						ibuf[1024];
	char						xbuf[1024];
	fc_cache_status_t			cs  = CS_FRESH;
	
	






	do {
		/*@@ M.1 */
		if (!poc->force_private) {
			/*
			 * lookup 과정에서 inode의 progress 설정하여, 다른 CLFU 캐시 op에서 reclaim 방지
			 */
			poc->inode = dm_lookup_cache(	poc->volume, 
											DM_LOOKUP_WITH_KEY, 
											poc->cache_key, 
											U_FALSE, 
											U_TRUE, 
											poc->pathlock, 
											__FILE__,
											__LINE__);
		}
		TRACE((T_INODE, "Volume[%s].CKey[%s] - fPV[%d], INODE[%u] found;{%s}\n",
						poc->volume->signature,
						poc->cache_key,
						poc->force_private,
						(poc->inode?poc->inode->uid:-1),
						(poc->inode?dm_dump_lv2(ibuf, sizeof(ibuf), poc->inode):"")
						));


		if (!poc->inode) { 
			/*
			 * @@ M.2 
			 * caching되어있지 않음
			 * 원본에 요청하면서 상태값을 저장
			 */
L_do_miss_operation:
			poc->open_result = dm_apc_schedule_async_open(poc, APC_OS_QUERY_FOR_MISS, __FILE__, __LINE__);
			if (poc->open_result == DM_OPENR_SCHEDULED) {
				goto L_scheduled;
			}
		}
		else {

			/*
			 * poc->inode의 현재 상태
			 * 		- inode locked
			 * 		- refcnt + 1
			 *		- ic_set_busy(U_TRUE) 
			 */
			TRACE((T_INODE, "Volume[%s].CKey[%s] - found INODE[%u];CTX{%s}\n", 
							poc->volume->signature, 
							poc->cache_key,
							poc->inode->uid,
							dm_dump_open_context(xbuf, sizeof(xbuf), poc)
							));
			/* 
			 * @@ M.3 
			 * 신선도 검사가 필요한지 기본 확인
			 */
            if (dm_is_vary_meta_object_nolock(poc->inode)) {
                /*
                 * @@ M.5
                 * 조회된 객체가 vary meta 객체임.
                 * 캐시 key 새로 생성필요 (새로운 캐시키로 실제 캐시 객체조회 준비)
                 */
				

				if (poc->cache_key != poc->cache_key_given)
					XFREE(poc->cache_key);
                dm_create_key_pair( poc->volume,
                                    poc->origin_path,
                                    poc->inode->property,
                                    poc->cache_key_given,
                                    &poc->cache_key,
                                    poc->in_prop);
                TRACE((T_INODE, "O[%s]: VARY, new KEY[%s] created, returning\n", poc->origin_path, poc->cache_key));
                poc->u._ubi.keychanged  = 1;
                poc->u._ubi.vary        = 1;
                poc->open_result        = DM_OPENR_KEYCHANGED;

				ic_set_busy(poc->inode, U_FALSE, __FILE__, __LINE__);

                poc->inode              = NULL;
            }
            else {
                cs = nvm_need_freshness_check_nolock(poc->volume, poc->inode);

				switch (cs) {
					case CS_MARK_STALE:

						IC_LOCK(CLFU_LM_EXCLUSIVE);
						ic_set_busy(poc->inode, U_FALSE, __FILE__, __LINE__);
						nvm_isolate_inode_nolock(poc->volume, poc->inode);
						IC_UNLOCK;
						inode_tofree 		= poc->inode;
						poc->inode 			= NULL;
						poc->open_result 	= DM_OPENR_ALLOCFAILED;
						poc->u._ubi.stalled = 1;
						goto L_do_miss_operation;
						break;
					case CS_NEED_REVAL:
						/*
	                     * @@ M.6
	                     * 신선도 검사가 필요한 객체
						 * 원본에 요청하면서 상태값 저장
						 */


						poc->open_result = dm_apc_schedule_async_open(poc, APC_OS_QUERY_FOR_REFRESHCHECK, __FILE__, __LINE__);

						/*
						 * 비동기 스케줄링 후 poc->inode는 NULL로 만들어야함
						 */
						poc->inode = NULL;
						if (poc->open_result == DM_OPENR_SCHEDULED) {
							goto L_scheduled;
						}
						break;
					case CS_FRESH:
						/*
						 * @@ M.4
						 * 신선도 검사 필요없음
						 * vary 객체 여부 체크
						 */
						poc->open_result     = DM_OPENR_FRESH;
						break;
				}
            }


		}
	} while (poc->open_result == DM_OPENR_AGAIN || poc->open_result == DM_OPENR_KEYCHANGED);




	if (poc->inode) {
		/*
		 * @@ M.7
		 * 현재상태
		 * 		- refcnt+1
		 * 		- inode lock acquired
		 *		- PROGRESS acquired
		 *
		 */

		TRACE((T_INODE, "Volume[%s].CKey[%s]/INODE[%u]/R[%d] - %s cache object found\n", 
						poc->volume->signature, 
						poc->inode->q_id, 
						poc->inode->uid, 
						poc->inode->refcnt,  
						(poc->u._ubi.offcache?"OFFLINE":"FRESH")
						));
		/* 
		 * 신선도 검사할 필요가 없음
		 * 즉, 신선하다고 판단
		 */

		dm_update_recent_nolock(poc->volume, poc->inode);
		dm_update_viewcount(poc->inode);


		if (INODE_GET_REF(poc->inode) == 0) { 
			/*
			 * inode를 활성화
			 */
			poc->u._ubi.activating = 1;

			ret = dm_make_inode_online(poc->inode, poc->pathlock, poc->in_prop);
			if (ret < 0) {
				/*
				 * free 
				 */
			
L_remove_and_allocate:
				TRACE((T_INFO, "Volume[%s].CKey[%s] - INODE[%u] activation failed{%s}:errno=%d\n", 
								poc->volume->signature, 
								poc->inode->q_id, 
								poc->inode->uid, 
								dm_dump_lv1(ibuf, sizeof(ibuf), poc->inode),
								errno
								));
				/*
				 * return값 관심없음
				 */
				IC_LOCK(CLFU_LM_EXCLUSIVE);
				ic_set_busy(poc->inode, U_FALSE, __FILE__, __LINE__);
				nvm_isolate_inode_nolock(poc->volume, poc->inode);
				IC_UNLOCK;

				inode_tofree 		= poc->inode;
				poc->inode 			= NULL;
				poc->open_result 	= DM_OPENR_ALLOCFAILED;
				goto L_do_miss_operation;
			}
			else { 
				TRACE((T_INODE, "INODE[%u]/CKey[%s]/R[%d] - successfully activated\n", 
								poc->inode->uid, 
								poc->inode->q_id, 
								poc->inode->refcnt));
				_ATOMIC_ADD(g__inode_opened, 1);


			}
		}

#if NOT_USED
		if (poc->inode->ob_memres && poc->inode->ob_upgradable) {
			dm_upgrade_if_hot(poc->inode);
		}
#endif

		if ((INODE_GET_REF(poc->inode) == 0) && 
			(poc->inode->c_path) && 
			(poc->inode->ob_memres == 0)) {
			poc->inode->fd = dm_open_file_ifnot(poc->inode->fd, poc->inode->c_path, 0, poc->inode->ob_created, U_FALSE);
			if (!DM_FD_VALID(poc->inode->fd)) {
				TRACE((T_INFO, "Volume[%s].CKey[%s] - INODE[%u] open(not create) error(would be removed) ;{%s}\n",
								poc->inode->volume->signature,
								poc->inode->q_id,
								poc->inode->uid,
								dm_dump_lv2(ibuf, sizeof(ibuf), poc->inode)
								));
				
				DEBUG_TRAP;
				goto L_remove_and_allocate;
			}
			TRACE((0, "INODE[%u]/R[%d] - fd [%d]\n", poc->inode->uid, poc->inode->refcnt, poc->inode->fd));
		}


		/* open 성공 */
		IC_LOCK(CLFU_LM_EXCLUSIVE);
		dm_hit_clfu_nolock(poc->inode, 1) ;
		TRACE((T_INODE|poc->inode->traceflag, "INODE[%d]/R[%d] opened ok\n", poc->inode->uid, INODE_GET_REF(poc->inode)));
		IC_UNLOCK;

		/*
		 * return 준비
		 */
		__nc_errno = errno = 0;
		dm_make_report(poc->inode, poc->in_prop, (poc->u._ubi.offcache?NC_OS_OFF_HIT:NC_OS_NC_HIT));
#ifndef NC_RELEASE_BUILD
		DEBUG_ASSERT( (strcmp(poc->inode->q_id, poc->cache_key) == 0) ||(strcasecmp(poc->inode->q_id, poc->cache_key) == 0)) 
#endif
		TRACE((T_INODE, "INODE[%u]/CKey[%s]/R[%d] - ok in blocking call mode;%s\n", 
						poc->inode->uid, 
						poc->inode->q_id, 
						poc->inode->refcnt, 
						dm_dump_lv1(ibuf, sizeof(ibuf), poc->inode)
						));

		inode_toreturn = poc->inode;
	} 




L_scheduled:
	if (poc->open_result == DM_OPENR_SCHEDULED)
		__nc_errno = errno = EWOULDBLOCK;


	if (inode_tofree && nvm_purge_inode(poc->volume, inode_tofree, U_TRUE,  NULL) == -EBUSY)
		inode_tofree->ob_doc 	= 1;

	TRACE((T_INODE, "Volume[%s]/Key[%s] - open_result=%d(toreturn=INODE[%d])\n", 
					poc->volume->signature, 
					poc->cache_key, 
					poc->open_result,
					(inode_toreturn?inode_toreturn->uid:-1)
					));

	return inode_toreturn;
}

fc_inode_t *
dm_apc_open_inode(		nc_volume_context_t 	*volume, 
						char 					*cache_id, 
						char 					*origin_path, 
						nc_mode_t 				mode, 
						nc_xtra_options_t 		*kv, 
						nc_path_lock_t 			*pl, 
						nc_apc_open_callback_t 	callback, 
						void 					* callback_data)
{

	apc_open_context_t			soc;





	apc_open_context_init(&soc, volume, origin_path, cache_id, mode, kv, pl, U_FALSE, 0, callback, callback_data);
	soc.inode = dm_apc_open_inode_internal(&soc);
	if (soc.cache_key != soc.cache_key_given)
		XFREE(soc.cache_key);

	return soc.inode;
}



static int
dm_need_to_remove(fc_inode_t *inode)
{
	return ((inode->ob_doc || inode->ob_priv)); 
}

/*
 * inode LOCK 상태로 호출
 */
static void
dm_delegate_close(fc_inode_t *inode, nc_time_t attempt, int needref)
{
	char		zpl[1024];
	close_context_t			*cc = XMALLOC(sizeof(close_context_t), AI_ETC);

	/*
	 * fio_close에서 unref를 하므로, reclaim되는것을 막기위해서
	 * 여기에서 refcnt를 증가시켜둬야함
	 */


	if (needref)
		nvm_path_lock_ref_reuse(inode->volume, inode->PL, __FILE__, __LINE__);

	cc->type		= APC_CLOSE_INODE;
	cc->inode 		= inode;
	if (cc->firstattempt == 0) {
		if (attempt != 0)
			cc->firstattempt= attempt;
		else
			cc->firstattempt= nc_cached_clock();
	}
	TRACE((T_INODE, "INODE[%d]/R[%d] - delegating(PL=%s)\n", inode->uid, INODE_GET_REF(inode), nvm_path_lock_dump(zpl, sizeof(zpl), inode->PL)));
	asio_post_apc(cc, __FILE__, __LINE__);
}
//
// 가정 inode에 대한 lock이 획득된 상태에서 호출됨
//
static int
dm_wait_io_complete(fc_inode_t *inode)
{
	char				buf[2048];
	nc_asio_vector_t	*v;
	nc_time_t			to_begin;
	nc_time_t			to_start;
	int					waiting = 0, nc;


	v		 = NULL;
	to_start = to_begin = nc_cached_clock();
	INODE_LOCK(inode);

L_restart:
	while ((waiting = link_count(&inode->pending_ioq, U_TRUE)) > 0) {
		if ((nc_cached_clock() - to_begin) > inode->volume->to_ncread) {
			nc = asio_signal_cancel(inode);
			TRACE((T_INODE, "Volume[%s].Key[%s] - too long time IO pending(%d wait, %d canceled)(%d secs elapsed),canceling ***WHATEVER***;%s\n",
						inode->volume->signature,
						inode->q_id,
						waiting,nc,
						(int)(nc_cached_clock() - to_start),
						dm_dump_lv1(buf, sizeof(buf), inode)
				  ));
			to_begin = nc_cached_clock();
			goto L_restart;
		}
		INODE_UNLOCK(inode);
		bt_msleep(1);
		if (INODE_GET_REF(inode) > 1) {
			goto L_wait_abort;
		}
		INODE_LOCK(inode);
	}

	if (waiting) {
		TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%d] IO complete after %d secs;%s\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					(int)(nc_cached_clock() - to_start),
					dm_dump_lv1(buf, sizeof(buf), inode)
			  ));
	}
	INODE_UNLOCK(inode);
L_wait_abort:
	return (INODE_GET_REF(inode) > 1)?-1:0;
}
/*
 * PRE-REQUISTE
 * 	path-lock acquired
 * 	refcnt > 0
 */
int
dm_close_inode(fc_inode_t *inode, int write_close, int forceclose)
{

	char				dbuf[1024];
	nc_origin_session_t	*origin;
	int					cancelable = 0;



	

	DEBUG_ASSERT_FILE(inode, (INODE_GET_REF(inode) > 0));
	DEBUG_ASSERT_FILE(inode, inode->volume->refcnt > 0);
	DEBUG_ASSERT(dm_inode_check_owned(inode) == 0);

	/*
	 ***************************************************************************************
	 *
	 * atime은 일단 막음 (2019.8.26)
	 * by weon@solbox.com
	 * reclaim시 대상 선택에 atime정보 사용할 수 없게됨
	 *
	 ***************************************************************************************
	 */


	TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u]/R[%d];{%s} -  begins\n", 
					inode->volume->signature, 
					inode->q_id, 
					inode->uid, 
					inode->refcnt, 
					(char *)dm_dump_lv1(dbuf, sizeof(dbuf), inode)
					));

	DM_UPDATE_METAINFO(inode, inode->atime = nc_cached_clock());
	if (inode->staleonclose) {
		inode->staleonclose = 0;
		nvm_make_inode_stale(inode->volume, inode);
		TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u]/R[%d];{%s} -  staled by staleonclose\n", 
					inode->volume->signature, 
					inode->q_id, 
					inode->uid, 
					inode->refcnt, 
					(char *)dm_dump_lv1(dbuf, sizeof(dbuf), inode)
					));
	}

L_retry_close:

	origin = inode->origin;

	/*
	 * rangeable객체에 대한 cancel을 생략함.
	 *		양방향 스토리지의 경우 cancel 후 새로운 요청(소켓할당)을 하는경우 부하 증가(from 유희곤)
	 */
	INODE_LOCK(inode);
	cancelable = (INODE_GET_REF(inode) == 1) &&
				 (inode->iobusy && (inode->ob_priv || inode->ob_staled));
	if (cancelable)
	{
		/*
		 * asio가 진행중이면 취소 
		 *
		 * 캐싱 객체 자체가 폐기될 내용임.
		 * 현재 inode에서  대기하는 모든 IO 요청도 취소되어야함
		 * 이부분은 dm_need_remove()의 조건과 다름. dm_need_remove()는 
		 * 디스크 공간 절감 단계에서도 TRUE 가 됨
		 */
		int 	nc = 0;

		nc = asio_signal_cancel(inode);
		TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - cancelling ASIO(by forceclose) returned %d(IOs cancel signalled) \n", 
								inode->volume->signature,
								inode->q_id,
								inode->uid, 
								nc));
	}
	INODE_UNLOCK(inode);

	if (INODE_GET_REF(inode) == 1) {
		TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u]/R[%d];{%s} -  delegating\n", 
						inode->volume->signature, 
						inode->q_id, 
						inode->uid, 
						inode->refcnt, 
						(char *)dm_dump_lv1(dbuf, sizeof(dbuf), inode)
						));

		DEBUG_ASSERT(dm_inode_check_owned(inode) == 0);

		dm_delegate_close(inode, nc_cached_clock(), U_TRUE); /* first attempt */

		DEBUG_ASSERT(dm_inode_check_owned(inode) == 0);
		return -EWOULDBLOCK;

	}
	INODE_UNREF(inode);
	DEBUG_ASSERT(dm_inode_check_owned(inode) == 0);

	return 0; /* write error value */
}

int
dm_flush_inode(fc_inode_t *inode)
{
/*
 * 본 루틴은 inode->writable == 1 인 경우엔 대기해야함
 * issue : asio count를 세기때문에 read가 진행중인 경우에도 대기할 수 있음.
 * 향후 시간날 때 세분화
 */
#if 0
	int		r;

	DEBUG_ASSERT_FILE(inode, INODE_GET_REF(inode) > 0);
	r = blk_schedule_writeback(inode, U_TRUE, NULL);
	while (dm_check_concurrent_limit_nolock(inode) > 0) {
		bt_msleep(5);
	}

	cfs_flush(inode->volume, inode);

	return r;
#else
	return 0;
#endif
}





/*
 * REMARK)
 * 이 함수에서 호출되는 dm_free_inode()는 IC_LOCK불필요함
 * 왜냐하면 recycle = FALSE이므로  free-list에 등록되지 않음
 */
int
dm_evict_inode_nolock(fc_inode_t *inode, int checkswap)
{
	char	ibuf[1024];

	TRACE((inode->traceflag|T_INODE, "Volume[%s].CKey[%s] - INODE[%d](with %s swap) evicted;%s\n",
			(inode->volume?inode->volume->signature:"NULL"),
			inode->q_id,
			inode->uid,
			(checkswap?"":"no"),
			dm_dump_lv1(ibuf, sizeof(ibuf), inode)
			));


	dm_free_inode(inode, U_FALSE);
	return 0;
}

/*
 *
 * 이 함수는 CLFU 생성시 destroy 콜백으로 등록되어있음
 * CLFU destroy시 등록되어있는 모든 객체에 대해서 이 함수가 호출됨
 *
 */
int
dm_swapout_inode_nolock(void *ui)
{
	fc_inode_t 	*inode = (fc_inode_t *)ui;
//	mdb_tx_info_t	myseq;


	if ((inode->volume == NULL) || dm_need_to_remove(inode)) {
		/*
		 * 이미 volume정보 제거되었거나, 삭제 대상임
		 */
		return 0;
	}
	/*
	 * mdb에 객체 메타정보가 저장된 경우 필요하면  update
	 */
	if (dm_is_mdb_dirty(inode)) { 
		dm_update_caching_completeness_nolock(inode);
		pm_add_cache(inode->part, inode, U_FALSE);
		inode->mdbversion = 0;
	}

	/*
	 * 디스크 캐시를 가진 객체는 필요하면 디스 객체 파일에 저장
	 */
	if (inode->c_path) {
		/*
		 * vary meta는 c_path없음
		 */
		dm_swap_cacheobject_ifneededed(inode);
	}
	return 0;
}
void
dm_shutdown()
{
	int		freed = 0;
	TRACE((T_INFO, "Disk-cache manager - freeing all inodes\n"));

	ic_shutdown();
	TRACE((T_INFO, "Disk-cache manager - %d inodes freed(total allocated=%d)\n", freed, g__inode_max_count));
}


void
dm_copy_stat_light(nc_stat_t *cobj, fc_inode_t *inode)
{
	cobj->st_property 		= NULL;
	if (inode->writable)
		cobj->st_size 		= inode->fr_availsize;
	else
		cobj->st_size 		= inode->size;

	cobj->st_uid  		= g__default_uid;
	cobj->st_gid  		= g__default_gid;
	cobj->st_ctime 		= inode->ctime;
	cobj->st_mtime 		= inode->mtime;
	cobj->st_atime 		= 0;
	cobj->st_vtime 		= inode->vtime;
	cobj->st_origincode	= inode->origincode;

	cobj->obi			= inode->obi;


	cobj->st_ino  		= inode->uid; /* 2015.8.27 ir# 24098 */
	if (inode->devid[0] != 0) 
		memcpy(cobj->st_devid, inode->devid, sizeof(inode->devid));
}
void
dm_copy_stat(nc_stat_t *cobj, fc_inode_t *inode, int needclone)
{
	dm_copy_stat_light(cobj, inode);

	cobj->st_property	= inode->property;
	if (needclone) {
		cobj->st_property	= kv_clone_d(inode->property, __FILE__, __LINE__);
	}
}
int
dm_inode_stat(nc_volume_context_t *volume, char *cache_id, char *origin_path, nc_stat_t *stat, void *stat_hint, int *hint_len, nc_kv_list_t *kv, nc_mode_t mode, nc_hit_status_t *stathit, int enablemonitor)
{
	return 0;
}

/*
 * 지정된 블럭이 디스크에 저장완료되었음을
 * 비트맵에 기록
 */
void
dm_commit_block_caching(fc_inode_t *inode, fc_blk_t * blk)
{
	char	bbuf[256]="";

#ifdef EXPERIMENTAL_USE_SLOCK
	pthread_spin_lock(&inode->slock);
	/*
	 * LOCK이 없으면 bit정보 유실됨(MP-machie에서여러 thread동시 접근시 )
	 */
	TRACE((T_ASIO, "Volume[%s].Key[%s] : INODE[%u] - blk#%u COMMITED;%s\n", 
				inode->volume->signature, 
				inode->q_id, 
				inode->uid, 
				blk->blkno,
				bcm_dump_lv1(bbuf, sizeof(bbuf), blk)
				));
	set_bit(blk->blkno, inode->bitmap);

	pthread_spin_unlock(&inode->slock);
#else
	INODE_LOCK(inode);
	/*
	 * LOCK이 없으면 bit정보 유실됨(MP-machie에서)
	 */

	TRACE((T_INODE|T_ASIO, "Volume[%s].Key[%s] : INODE[%u] - blk#%u COMMITED;%s\n", 
				inode->volume->signature, 
				inode->q_id, 
				inode->uid, 
				blk->blkno,
				bcm_dump_lv1(bbuf, sizeof(bbuf), blk)
				));
	set_bit(blk->blkno, inode->bitmap);

	INODE_UNLOCK(inode);
#endif
	dm_need_header_update(inode);
}
void
dm_update_block_crc_nolock(fc_inode_t *inode, long blkno, nc_crc_t crc)
{

#ifdef NC_ENABLE_CRC
	char	ibuf[8192];

	
	DEBUG_ASSERT_FILE(inode, (blkno < 0xFFFFFFFFL));
	if ((blkno >= inode->mapped))  {
		TRACE((T_ERROR, "INODE[%u] - {%s} - extent invalid or, %ld is larger than size\n",
						inode->uid, 
						(char *)dm_dump_lv1(ibuf, sizeof(ibuf), inode), blkno));
		
		return;
	}

	if (inode->crcsize > (blkno*sizeof(nc_crc_t))) {
		inode->blockcrc[blkno] = crc;
		TRACE((T_INODE|T_BLOCK, "Volume[%s].CKey[%s].INODE[%u]:blk#%u CRC[0x%08X] - updated,(0x%08X)\n",
								inode->volume->signature, 
								inode->q_id, 
								inode->uid, 
								blkno, 
								crc, 
								inode->blockcrc[blkno]));
	}
	else {
		TRACE((T_ERROR, "INODE[%u] - %ld crc size error, crcsize=%ld\n", 
						inode->uid, 
						blkno, 
						inode->crcsize));
	}
#endif
}


void
dm_finish()
{

	TRACE((T_INFO, "Disk-cache manager - closing timers\n"));
	while (bt_destroy_timer_v2(__timer_wheel_base, &__dm_state.timer_inode_spinner) < 0) {
		bt_msleep(1);
	}
	while (bt_destroy_timer_v2(__timer_wheel_base, &__dm_state.timer_check_statfs) < 0) {
		bt_msleep(1);
	}

}
u_boolean_t
dm_disk_cacheable(fc_inode_t *inode, int ocode)
{
#if 0
	return (inode->ob_memres 	== 0 &&  
			inode->ob_priv 		== 0 &&
			inode->ob_template 	== 0 &&
			inode->writable 	== 0 &&  
			__memcache_mode 	== 0 
			);
#else
	/*
	 * 캐시 효율성 상향을 위해서  priv도 일단 디스크 캐시
	 */
	int		res = 0;


	/*
	 * check property
	 */
	res =  (inode->ob_memres 	== 0 &&  
			inode->ob_template 	== 0 &&
			inode->ob_doc 		== 0 &&
			inode->ob_staled 	== 0 &&
			inode->writable 	== 0 &&  
			__memcache_mode 	== 0 
			);
	//DEBUG_ASSERT(ocode > 0);
	//res = res || (cfs_iserror(inode->volume, ocode) == 0);
	return res;
#endif
}
u_boolean_t
dm_is_mdb_required(fc_inode_t *inode)
{
	return		(inode->part != NULL) && 
				(inode->ob_memres == 0) &&
				(inode->ob_priv == 0) &&
				(inode->writable == 0) &&
				(__memcache_mode == 0)
				;
}

/*
 * writable인 경우에만?
 */
int
dm_verify_block_crc(fc_inode_t *inode, long blkno, nc_crc_t crc)
{
#ifdef NC_ENABLE_CRC
	int 	r = 1;
	char 	ibuf[1024];
	DEBUG_ASSERT_FILE(inode, (blkno <= inode->maxblkno));
	r =  (inode->blockcrc[blkno] == crc);
	if (!r) {
		TRACE((T_INODE, "INODE[%u]/CKey[%s] - %ld, crc error (0x%08X, 0x%08X)\n\t\tINODE-INFO:%s\n", 
						inode->uid, 
						inode->q_id, 
						(long)blkno, 
						inode->blockcrc[blkno], 
						crc,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)

						));
	}
	return r;
#else
	return U_TRUE;
#endif
}


//static	int				__s_uuid_warned = 0;
void
dm_make_uuid(uuid_t uuid)
{
	//while (uuid_generate_time_safe(uuid) != 0) bt_msleep(1);
	uuid_generate_random(uuid);
	return ;
}

#ifdef NC_ADAPTIVE_READAHEAD
void
dm_update_misshit(fc_inode_t *inode, int hit)
{
	if ((hit == 0) &&
		inode && 
		(INODE_GET_REF(inode) > 0) && 
		inode->volume) {
		if (inode->volume->adaptivereadahead) _ATOMIC_ADD(inode->totalmisshit,1);
	}
}
void
dm_update_hit(fc_inode_t *inode)
{
	if (inode->volume->adaptivereadahead) {
		inode->totalhit++;
		if (inode->totalhit >= 0x0FFFFFFF) {
			inode->totalhit = 0;
			inode->totalmisshit = 0;
		}
	}
}
#endif
/*
 * 함수의 호출 시점
 * inode의 refcnt == 0일 때도 호출가능.
 * inode의 refcnt == 0 이면 이 함수 호출 결과는 FALSE로 리턴
 * refcnt > 0일 때만 의미있는 체크 실행
 * 	 2014.8.2 by weon
 * 호출 전제조건
 * 		block not locked AND
 * 		inode not locked
 */
u_boolean_t 
dm_is_valid_bind_nolock(fc_inode_t *inode, fc_blk_t *blk, nc_blkno_t blkno)
{
	u_boolean_t 			res = U_FALSE;



	if (inode->ob_rangeable)
		DEBUG_ASSERT_FILE(inode, (blkno <= inode->maxblkno));

	DEBUG_ASSERT_FILE(inode, (blkno < inode->mapped));
	DEBUG_ASSERT_FILE(inode, (inode->blockmap != NULL));
	DEBUG_ASSERT_FILE(inode, (INODE_GET_REF(inode) > 0));

	if (inode->writable) {
		/*
		 * writable인 경우 blockmap의 비교는 제외한다.
		 * blockmap은 지속적으로 resizing되고 있는 상태이므로,
		 * SEGV발생 가능성 있음
		 * 또한 실제로도 비교의 의미가 없음
		 */
		res = 	blk->signature == BLOCK_SIGNATURE(inode->signature, blkno);
	}
	else {
		res = 	(blk->signature == BLOCK_SIGNATURE(inode->signature, blkno)) &&
				((inode->blockmap[blkno]) == blk); 
	}


	if (!res) {
		TRACE((T_BLOCK, "INODE[%u]/R[%d] - (blockmap[%ld]=%p != blk=%p) dangled\n", 
						inode->uid, 
						inode->refcnt, 
						(long)blkno,
						inode->blockmap[blkno],
						blk
						));
	}
	return res;
}
int
dm_unbind_block_nolock(fc_inode_t *inode, nc_blkno_t blkno)
{

	int ubb = 0;
	fc_blk_t 	*blk;
	if (blkno <= inode->maxblkno) {

		blk = inode->blockmap[blkno];
		inode->blockmap[blkno] = NULL_BLKNO;
		TRACE((T_INODE, "INODE[%u]/R[%d] - blk#%u(%p) unbound\n", inode->uid, inode->refcnt, blkno, blk));
		ubb = 1;
	}
	return ubb;
}
fc_blk_t *
dm_get_block_nolock(fc_inode_t *inode, nc_blkno_t blkno)
{
	fc_blk_t 	*blk = NULL;

	/*
	 * inode lock은 획득되지않은 상태에서 호출되어야함
	 */
	if (blkno <= inode->maxblkno)  {
		blk = inode->blockmap[blkno];
	}
	return blk;
}
void
dm_bind_block_nolock(fc_inode_t *inode, nc_blkno_t blkno, fc_blk_t * blk)
{
	char	dbuf[1024];
	inode->blockmap[blkno] = blk;
	TRACE((T_BLOCK, "INODE[%u] : blk#%u/BUID[%ld] - bound {%s}\n",
					inode->uid, 
					blkno,
					(long)BLOCK_UID(blk),
					bcm_dump_lv1(dbuf, sizeof(dbuf), blk)
					));

}

static char *
dm_create_cache_key(char *cache_id, char *property_list, nc_xtra_options_t *prop)
{
	int 	i, n;
	char 	*prop_key_buf = NULL;
	char 	*tok, *cursor = NULL;
	char 	*val = NULL;
	char 	*prop_val[40];
	char 	*ck = NULL, *ok = NULL;
	int 	 prop_val_idx = 0;
	int 	 al = strlen(cache_id), xl = 0;
	nc_MD5_CTX			mdContext;

	
	//memset(prop_val, 0, sizeof(prop_val));
	if (property_list)
		prop_key_buf = XSTRDUP(property_list, AI_ETC);
	else  {
		TRACE((T_INFO, "cache_id '%s' - no property set\n", cache_id));
		prop_key_buf = XSTRDUP("", AI_ETC);
	}

	tok = strtok_r(prop_key_buf, ",", &cursor);
	while (tok) {
		while (strchr("\t ", *tok)) tok++;
		val = kv_find_val(prop, (char *)tok, U_FALSE);
		if (val) {
			prop_val[prop_val_idx++] = (char *)val;
			xl += strlen(val) + 1;
		}
		tok = strtok_r(NULL, ",", &cursor);
	}
	XFREE(prop_key_buf);

	al = al + max(32, xl) + 128;

	ok = (char *)XMALLOC(al, AI_ETC);

	n = sprintf(ok, "%s;", cache_id);
	ck = cursor = ok + n;

	cursor = ck;
	for (xl = 0, i = 0; i < prop_val_idx;i++) {
		n = sprintf(cursor, "/%s", prop_val[i]);
		cursor += n;
		xl += n;
	}
	*cursor = 0; xl++;
	/* nc_MD5 문자열 생성 */
	nc_MD5Init (&mdContext);
	nc_MD5Update (&mdContext, (unsigned char *)ck, xl);
	nc_MD5Final (&mdContext);

	for (cursor = ck, al = 0; al < 16 ; al++) {
		n = sprintf(cursor, "%02x", mdContext.digest[al]);
		cursor += n;
	}
	*cursor = 0;
	DEBUG_ASSERT(strlen(ok) > 0);

	/*
	 * 파라미터로 주어진 cache_id + md5 스트링을 합친 문자열을 리턴
	 */
	
	return ok;
}
int
dm_inode_for_each_property(fc_inode_t *inode, int (*do_it)(char *key, char *val, void *cb), void *cb)
{
	int 	r = -1;
	if (inode && inode->property) {
		r = kv_for_each(inode->property, do_it, cb);
	}
	return r;
}
int
dm_check_concurrent_limit_nolock(fc_inode_t *inode)
{
	int 	piocnt;

	piocnt = link_count(&inode->pending_ioq, U_TRUE);
	return piocnt;
}





int
dm_unmap_block_nolock(fc_inode_t *inode, fc_blk_t *blk)
{
	int	um = 0;
	if (dm_is_valid_bind_nolock(inode, blk, blk->blkno)) {
		um = dm_unbind_block_nolock(inode, blk->blkno);
	}
	return um;
}


/*
 * 캐싱된 객체의 내용 중에 메모리에 아직 올라오지 않은
 * 디스크 상에 존재하는 캐싱 블럭 정보는 모두 리셋
 */
void
dm_reset_inode_nolock(fc_inode_t *inode) 
{
	if (!inode->ob_staled) {
		TRACE((T_INFO|T_INODE, "INODE[%u]/R[%d]/maxblk[%d] - marking 'staled' cause some block(s) redrawed(%s)\n", 
						inode->uid, 
						inode->refcnt, 
						inode->maxblkno));
		inode->staleonclose = 1;
 		bitmap_zero(inode->bitmap, inode->bitmaplen ); 
		DM_UPDATE_METAINFO(inode, inode->ob_complete = 0);
	}

}
fc_file_t *
dm_apc_fast_reopen_internal(fc_inode_t *inode, apc_open_context_t *oc)
{
	fc_file_t	*fh;

	fh = fio_make_fhandle(inode, oc->pathlock, oc->mode, oc->in_prop);

	return  fh;
}
int
dm_is_inode_reusable(fc_inode_t *inode)
{
	return (
			(inode->ob_priv == 0) &&
			(inode->ob_staled == 0) &&
			(inode->ob_doc == 0)
		   );
}
int
dm_dump_or_count_inode_opened(long tflag, int *total, int *free, int need_dump)
{
#ifndef NC_RELEASE_BUILD
	char 			ibuf[1024];
	int 			opened = 0;
	fc_inode_t		*inode = NULL;

	IC_LOCK(CLFU_LM_SHARED);

	*total = 0;
	*free = ic_free_count_nolock();
	inode = link_get_head_noremove(&__inode_pool);
	while (inode) {
		XVERIFY(inode);
		if (INODE_GET_REF(inode) > 0) {
			opened++;
			if (need_dump && TRACE_ON(tflag))
				TRACE((tflag, "CKey[%s]/INODE[%u] - not yet closed{%s}\n", 
								inode->q_id, 
								inode->uid, 
								dm_dump_lv1(ibuf, sizeof(ibuf), inode)
								));
		}
		*total = *total + 1;
		inode = link_get_next(&__inode_pool, &inode->nav_node);
	}

	IC_UNLOCK;
	return opened;
#else
	return 0;
#endif
}



static int
dm_compress_vheader(nc_uint8_t *outbuf, nc_int32_t *outlen, nc_uint8_t *inbuf, nc_int32_t inlen)
{
	z_stream 	zdef;
	int 		err = Z_OK;

	memset(&zdef, 0, sizeof(zdef));
	/*
	 * setup input
	 */
	zdef.zalloc =  Z_NULL;
	zdef.zfree 	=  Z_NULL;
	zdef.opaque =  Z_NULL;
	zdef.avail_in = 0;
	zdef.next_in  = Z_NULL;
	deflateInit(&zdef, Z_DEFAULT_COMPRESSION);



	zdef.avail_in = inlen;
	zdef.next_in  = inbuf;

	/*
	 * setup output
	 */
	zdef.next_out = outbuf;
	zdef.avail_out = *outlen;
	err = deflate(&zdef, Z_SYNC_FLUSH);
	err = deflate(&zdef, Z_FINISH);
	*outlen = zdef.total_out;
	deflateEnd(&zdef);
	return err;
}
static int
dm_decompress_vheader(nc_uint8_t *outbuf, nc_int32_t *outlen, nc_uint8_t *inbuf, nc_int32_t inlen)
{
	z_stream 	zinf;
	int 		err = Z_OK;

	memset(&zinf, 0, sizeof(zinf));
	/*
	 * setup input
	 */
	zinf.zalloc =  Z_NULL;
	zinf.zfree 	=  Z_NULL;
	zinf.opaque =  Z_NULL;
	zinf.avail_in = 0;
	zinf.next_in  = Z_NULL;

	inflateInit(&zinf);

	/*
	 * setup output
	 */
	zinf.avail_in = inlen;
	zinf.next_in  = inbuf;
	zinf.avail_out = *outlen;
	zinf.next_out  = outbuf;

	err = inflate(&zinf, Z_SYNC_FLUSH);
	if (err < 0) {
		return err;
	}

	err = inflate(&zinf, Z_FINISH);
	if (err < 0) {
		return err;
	}
	*outlen = zinf.total_out;
	inflateEnd(&zinf);
	return err;
}
static nc_crc_t
do_crc32(char *data, ssize_t len)
{
	nc_crc_t 		crc = 0;
	uint32_t crc32_8bytes(const void *data, size_t length);

	crc = crc32_8bytes((const void *)data, len);
	return crc;
}
#if NOT_USED
static int
dm_get_case_element(void *header)
{
	fc_common_header_t 		*pcommon = (fc_common_header_t *)header;
	fc_header_info_v30_t 	*h30 = (fc_header_info_v30_t *)header;

	dm_check_magic(pcommon->magic);
	return h30->ob_preservecase;
}
#endif
static int
dm_verify_header(void *header)
{
	fc_common_header_t 		*pcommon = (fc_common_header_t *)header;
	fc_header_info_v30_t 	*h30 = (fc_header_info_v30_t *)header;
	int 					r = 1;
	int 					hv;
	nc_crc_t 				crc, org_crc;
	nc_uint32_t 			org_dhs;
	nc_uint32_t 			org_flag;
	
	hv = dm_check_magic(pcommon->magic);

	if (!hv) return 0;
	/*
	 * v2.5에서는 header_size는 실제 헤더 크기
	 */
	org_crc 		= h30->crc;
	h30->crc		= 0;
	org_dhs 		= h30->chdr.disk_header_size;
	org_flag 		= h30->chdr.flag;

	h30->chdr.disk_header_size = 0;
	h30->chdr.flag  = 0;

	crc = do_crc32((char *)h30, h30->chdr.header_size);
	r = (crc == org_crc);
	h30->chdr.disk_header_size = org_dhs;
	h30->chdr.flag  = org_flag;
	h30->crc = org_crc;
	return r;
}
void
dm_update_disk_object_size_nolock(fc_inode_t *inode, nc_blkno_t pblkno)
{
	inode->disksize = max(inode->disksize, (long long)(inode->doffset + ((long long)pblkno -1LL)*NC_BLOCK_SIZE));
}

/*
 * sync 정책 
 * 		data 저장 이후 헤더가 동기화 안되었을 때
 * 		data 저장 이후 한 시간이상 지났을 때
 */
static int
dm_need_sync(fc_inode_t *inode) 
{
	/* CHG 2018-06-21 huibong 불필요 변수 제거 (#32205) */
	return dm_is_header_dirty(inode);
}
int
dm_is_mdb_dirty(fc_inode_t *inode)
{
	return (inode->rowid >= 0) && (inode->mdbversion != 0); 
}
static int
dm_is_header_dirty(fc_inode_t *inode)
{
	 return inode->headerversion; 
}
#if NOT_USED
static int
dm_is_dirty(fc_inode_t *inode)
{
	return dm_is_mdb_dirty(inode) || dm_is_header_dirty(inode);
}
#endif
void
dm_update_content_version(fc_inode_t *inode)
{
	inode->contentversion++;
}
static int
dm_create_key_pair(	nc_volume_context_t *volume, 
					char 				*opath, 
					nc_xtra_options_t 	*property, 
					char 				*cache_key_given, 
					char 				**cache_key, 
					nc_xtra_options_t 	*req )
{
	char 	*vf = NULL;
	int 	r = 0;
	if (property) {
		/* 
		 * working cache id 및 stat id가 prefix와 다른 메모리에 대한 포인터인 경우
		 * 새로 할당된 메모리임. free할 필요있음
		 */
		if (*cache_key != cache_key_given)
			XFREE(*cache_key);

		vf 			= (char *)kv_find_val(property, "Vary", U_FALSE);
		*cache_key 	= dm_create_cache_key(cache_key_given, vf, req);


	}
	else {
		TRACE((T_INFO, "Volume[%s].CKey[%s]- no vary property in prev object id, '%s'\n",  volume->signature, opath, *cache_key));
		/* property 가 NULL임 */
		r = -1;
	}
	TRACE((T_INODE, "Volume[%s].CKey[%s]:O[%s] - res[%d], NEW cache-key[%s] created(condition[%s],prop[%p])\n",
					volume->signature,
					cache_key_given,
					opath,
					r,
					(*cache_key? *cache_key:"NULL"),
					(vf?vf:"NULL"),
					req
					));
	return r;
}


void
dm_need_mdb_update(fc_inode_t *inode, const char *reason)
{
	nc_uint32_t	v; 
	v = _ATOMIC_ADD(inode->mdbversion, 1);
	TRACE((T_INODE, "INODE[%d] - mdb version set to %u\n", inode->uid, v));
}
void
dm_need_header_update(fc_inode_t *inode)
{
	nc_uint32_t	v; 
	v= _ATOMIC_ADD(inode->headerversion, 1);
	TRACE((T_INODE, "INODE[%d] - header version set to %u\n", inode->uid, v));
}
int
dm_need_iowait_nolock(fc_inode_t *inode)
{
	register int 	iow = 0;
	iow = inode->ob_frio_inprogress;
	return iow;
}
int
dm_check_if_same_prop(fc_inode_t *inode, nc_stat_t *stat)
{
	int 	same = U_TRUE;

	same =  cfs_issameresponse(inode->volume, inode->origincode, stat->st_origincode);
	if (!same) {
		TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - response changed, cached.code=%d, origin.code=%d\n", 
						inode->volume->signature, 
						inode->q_id, 
						inode->uid, 
						stat->st_origincode,
						inode->origincode));
	}

	return same;
}
int
dm_io_add_vector_nolock(fc_inode_t *inode, nc_asio_vector_t *v)
{
	/* CHG 2018-05-17 huibong 초기화되지 않은 return flow 수정 (#32120) */
	int 	r;
	r = link_append( &inode->pending_ioq, ( void * )v, &v->node);
	TRACE((T_INODE, "INODE[%d] - ASIO[%d] added, PID[%d]\n", inode->uid, v->iov_id, (int)link_count(&inode->pending_ioq, U_TRUE)));
	inode->iobusy = U_TRUE;
	return r;
}
int
dm_io_remove_vector_nolock(fc_inode_t *inode, nc_asio_vector_t *v)
{
	/* CHG 2018-05-17 huibong 초기화되지 않은 return flow 수정 (#32120) */
	int 	r;

	r = link_del( &inode->pending_ioq, &v->node);
	TRACE((T_INODE, "INODE[%d] - ASIO[%ld] done, removed, PIO[%d]\n",
					inode->uid,
					v->iov_id,
					(int)link_count(&inode->pending_ioq, U_TRUE)
					));
	dm_update_iobusy(inode);
	return r;
}


static ssize_t
dm_write_bytes(fc_inode_t *inode, char *ptr, ssize_t sz, nc_off_t base)
{
	ssize_t 			n;
	ssize_t 			tw = 0;
	char				*abuf = NULL, *abuf_org = NULL;
	int					tlen = 0;
	char				ibuf[2048];

	if (!DM_FD_VALID(inode->fd)) {
		TRACE((T_ERROR, "Volume[%s].CKey[%s] - INODE[%d] Invalid FD;%s\n",
						inode->volume->signature,
						inode->q_id,
						inode->uid,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)
			  ));
		return -1;
	}

	abuf_org = abuf = (char *)nc_aligned_malloc(NC_PAGE_SIZE, getpagesize(), AI_ETC, __FILE__, __LINE__);

	while (sz > 0) {
		tlen = min(NC_PAGE_SIZE,sz);
		memcpy(abuf, ptr, tlen);
		n = pwrite(inode->fd, (char *)abuf, (size_t)tlen, (off_t)base);

		if (n < 0)  {
			TRACE((T_ERROR, "Volume[%s].CKey[%s] - INODE[%d] pwrite(offset=%lld, len=%d) returns %d(errono=%d)\n", 
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							base, 
							tlen, 
							n, 
							errno
						));
			break;
		}
		ptr 	+= n;
		base 	+= n;
		sz		-= n;
		tw 		+= n;
	}
	nc_aligned_free(abuf);
	return tw;
}
static ssize_t
dm_read_bytes(nc_fio_handle_t fd, char *ptr, ssize_t sz, nc_off_t base)
{
	char 			*aptr = NULL;
	char 			*aptr_org = NULL;
	nc_off_t 		woff = 0;
	nc_off_t 		wsiz = 0;
	ssize_t 		n, an = 0;
	int				bs = pm_get_align_size();

	aptr_org = aptr = (char *)nc_aligned_malloc(bs /* size */, getpagesize(), AI_ETC, __FILE__, __LINE__);
	do {
		wsiz = min(bs, sz);
		n = pread(fd, (char *)aptr, wsiz/*count*/, base + woff/* offset */);
		if (n > 0) {
			memcpy(ptr + woff, aptr, n);
			sz -= n;
			woff += n;
			an += n;
		}
	} while (n > 0 && sz > 0);
	nc_aligned_free(aptr);
	return an;
}
static void
dm_copy_uuid(uuid_t dest, uuid_t src)
{
	memcpy(dest, src, sizeof(uuid_t));
}
void 
dm_clear_uuid(uuid_t uuid)
{
	uuid_clear(uuid);
}
nc_uint64_t
dm_make_hash_key(int casesensitive, char *key)
{
	char		*temp;
	nc_uint64_t	hv = 0;
	if (!casesensitive) {
		temp = tolowerz(key);
		hv = FNV1A_Hash_Yoshimura((char *)temp, strlen(temp));
		XFREE(temp);
	}
	else {
		hv = FNV1A_Hash_Yoshimura((char *)key, strlen(key));
	}
	return hv;
}
void
dm_set_inode_stat(fc_inode_t *inode, nc_stat_t *stat, nc_mode_t mode, int update_cc_for_pseudoprop)
{

	if (inode->property) {
		TRACE((T_INODE, "INODE[%u]/PROP[%p] - destroying\n", inode->uid, inode->property));
		kv_destroy(inode->property);
	}
	if (stat->st_property) {
		inode->property 	= stat->st_property;
		stat->st_property 	= NULL;
		kv_update_trace(inode->property, __FILE__, __LINE__);
		TRACE((T_INODE, "INODE[%u]/PROP[%p] - cloned\n", inode->uid, inode->property));
	}
	inode->imode		= stat->st_mode;
	inode->size			= stat->st_size;
	inode->ctime		= stat->st_ctime;
	inode->mtime		= stat->st_mtime;
	inode->vtime		= stat->st_vtime;
	inode->origincode 	= stat->st_origincode;

	inode->ob_vary			= stat->st_vary;
	inode->ob_rangeable 	= stat->st_rangeable;
	inode->ob_chunked 		= stat->st_chunked;
	inode->ob_sizeknown 	= stat->st_sizeknown;
	inode->ob_sizedecled 	= stat->st_sizedecled;
	inode->ob_nocache 		= stat->st_nocache;
	inode->ob_onlyifcached 		= stat->st_onlyifcached;
	inode->ob_mustreval    		= stat->st_mustreval;
	inode->ob_pxyreval    		= stat->st_pxyreval;
	inode->ob_immutable    		= stat->st_immutable;
	inode->ob_noxform    		= stat->st_noxform;
	inode->ob_nostore    		= stat->st_nostore;
	inode->ob_chunked    		= stat->st_chunked;
	inode->ob_cookie    		= stat->st_cookie;
	inode->ob_rangeable    		= stat->st_rangeable;


	if (!update_cc_for_pseudoprop) {
		/*
		 * POST와 같은 pseudo property에 대한 overwrite를 위한 호출인 경우는
		 * ob_priv=1로 그대로 유지되어야함.
		 * 그 이외에는 ob_priv는 application(solproxy)에서 제공한 mode값이
		 * 반드시 반영되어야함. (OR로 반영함)
		 */
		inode->ob_priv 			= (stat->st_private || (IS_ON(mode,  O_NCX_PRIVATE) != 0)); 
	}

	inode->bitmaplen		= NC_BITMAP_LEN(inode->size);
	inode->maxblkno			= dm_max_blkno(inode->size);
	inode->header_size		= dm_calc_header_size_v30(inode);

	memcpy(inode->devid, stat->st_devid, sizeof(inode->devid));
	DM_UPDATE_METAINFO(inode, U_TRUE); /* POST일때이므로 실제는 필요 없음 */

}
nc_origin_session_t 
dm_allocate_origin_session(apc_open_context_t *aoc)
{
	nc_origin_session_t s = NULL;
	s = cfs_allocate_session(aoc->volume, aoc);
	TRACE((T_INODE, "KEY[%s] - ORIGIN %p allocated\n", aoc->cache_key, s));

	if (s) {
		_ATOMIC_ADD(g__octx_count, 1);
	}

	return s;
}
static void dm_expire_origin_session(void *ud);

static nc_origin_session_t *
dm_bind_origin_session(fc_inode_t *inode, nc_origin_session_t origin, char *f, int l)
{
	nc_origin_session_t		*kept = NULL;

	kept = __sync_val_compare_and_swap(&inode->origin, inode->origin, origin);

	if (kept) {
		//
		// inode lock상황에서 cfs 함수호출 안됨. dead-lock 발생
		//
		_ATOMIC_SUB(g__octx_bind_count, 1);
		while (bt_del_timer_v2(__timer_wheel_base, &inode->t_origin) < 0) {
			bt_msleep(100);
		}
	}
	if (origin) {
		/*
		 * need reset timer with a new valid session info
		 */
		bt_set_timer(__timer_wheel_base,  &inode->t_origin, 5000, dm_expire_origin_session, inode);
		_ATOMIC_ADD(g__octx_bind_count, 1);

	
	}


	return kept;
}

nc_origin_session_t 
dm_steal_origin_session(fc_inode_t *inode, cfs_off_t off, cfs_size_t siz)
{
	nc_origin_session_t     origin = NULL;


	/* CHG 2018-06-21 huibong 초기화되지 않은 미사용 변수 사용 코드 수정 (#32205) */


	origin = dm_bind_origin_session(inode, NULL,__FILE__, __LINE__);
	if (origin) {
		if (!cfs_valid_session(inode->volume, origin, inode->q_path, (nc_off_t *)&off, (nc_size_t *)&siz) ) {
			dm_free_origin_session(inode->volume, origin, U_FALSE, __FILE__, __LINE__);
			origin  = NULL;
		}
	}
	return origin;
}
static void
dm_expire_origin_session(void *ud)
{
	fc_inode_t			*inode = (fc_inode_t *)ud;
	nc_origin_session_t	*origin = NULL;

	origin = dm_bind_origin_session(inode, NULL,__FILE__, __LINE__);

	if (origin) {
		TRACE((T_INODE, "Volume[%s].KEY[%s] - ORIGIN[%p] EXPIRED from INODE[%u]\n", 
								inode->volume->signature,
								inode->q_id, 
								origin, 
								inode->uid));

		dm_free_origin_session(inode->volume, origin, U_FALSE, __FILE__, __LINE__);
	}

}
void
dm_free_origin_session(nc_volume_context_t *volume, nc_origin_session_t origin, int bound, char *f, int l)
{

	if (origin) { 
		cfs_free_session(volume, origin);
		_ATOMIC_SUB(g__octx_count, 1);


		TRACE((T_INODE, "Volume[%s] - origin session '%p' FREED %d@%s\n", 
								volume->signature,
								origin,
								l, 
								f
								));

		if (bound) _ATOMIC_SUB(g__octx_bind_count, 1);
	}
}

/*
 * call scenario
 *
 *		0. createEventQueue(volume, key-pair, origin-path, mode)
 *		1. open_A(...., key)
 * 		2. if (inode) proceed to the odinary action
 *		3. nc_errno = EWOUDBLOCK, 
 * 			suspend
 *		
 *		#1 getEvent()
 *			(key, errorcode, OPEN(file-handle))
 *			(key, errorcode)
 *			(key, errorcode, READ(length))
 *
 */

static void
dm_update_recent_nolock(nc_volume_context_t *volume, fc_inode_t *inode)
{
	char	zuuid[64];
	char	ibuf[2048];
	int		r;
	int		tflg = T_WARN;
	if (inode->cversion != volume->version) {
		if (inode->part) {
			r = pm_reuse_inode(	inode->part,  
							inode->uuid, 
							inode->rowid, 
							inode->vtime, 
							volume->version);
			if (r == 1) {
				tflg = 0;
				inode->cversion = volume->version;
			}
		}
		else {
			tflg = 0;
			inode->cversion = volume->version;
		}

		TRACE((tflg|T_INODE, "Volume[%s].CKey[%s] - INODE[%u]/UUID[%s]/ROWID[%lld] refreshed %s upto current volume caching version(%d->%d);result=%d;%s\n", 
						volume->signature, 
						inode->q_id, 
						inode->uid, 
						uuid2string(inode->uuid, zuuid, sizeof(zuuid)),
						inode->rowid,
						((r == 1)?"OK":"FAILED"),
						inode->cversion,
						volume->version,
						r,
						dm_dump_lv1(ibuf, sizeof(ibuf),inode)
						));
	}
}

/*
 *  * 이 함수는 driver에서 객체에 대한 GET/HEAD등의 요청의 결과 중
 *    property를 받은뒤 property 수신완료 이벤트를 asio scheduler로 전송했고,
 *    asio thread에 의해서 이 함수가 호출된 상태
 * 	* calling thread는 아래 작업의 수행완료를 기다리고 있는 상태
 * 	* query에 대한 원본의 응답이 성공적이면
 * 		- query_result필드의 값이 채워져 있음
 *
 */
static fc_inode_t *
dm_apc_handle_event_for_miss(apc_open_context_t *oc)
{
	oc->open_result =  0;
	oc->inode = dm_apc_handle_event_for_miss_internal(oc);

	return oc->inode;
}

static fc_inode_t *
dm_apc_handle_event_for_miss_internal(apc_open_context_t *oc)
{
	char				ibuf[4096];
	fc_inode_t			*inode_tofree = NULL,
						*inode 	= NULL;

	int					result = 0;
	int 				o_ = 0;
	nc_hit_status_t 	hs = NC_OS_MISS;

	XVERIFY(oc);
	oc->open_result = DM_OPENR_FRESH;

	/*
	 * path-lock이 확보된 상태
	 */
	if (IS_ON(oc->mode, O_NCX_DONTCHECK_ORIGIN)) {
		/* 
		 * application에서 원본에 해당 객체의 존재 유무, 속성 조회를 하지말 것을 요청
		 */
		if (!dm_is_writable(oc->mode) && IS_ON(oc->mode,  O_NCX_PRIVATE)) {
			TRACE((T_INODE|T_INFO, "Volume[%s].CKey[%s]/O[%s] - O_NCX_DONTCHECK_ORIGIN defined, but not writable or no-cache\n", 
							oc->volume->signature, 
							oc->cache_key,
							oc->origin_path));
			oc->open_result = DM_OPENR_ALLOCFAILED;
			goto L_open_alloc_done;
		}
		oc->open_result = DM_OPENR_NOTFOUND;
	}
	else  {
		if (oc->query_result.result == 0) { 
			if (IS_ON(oc->mode, O_NCX_NORANDOM) && 
				oc->query_result.stat.st_rangeable) {
				/*
				 * 오리진은 rangeable 객체라고 리포트 했지만,
				 * solproxy쪽에서 강제로 NORANDOM으로 override함
				 */
				TRACE((T_INODE, "Volume[%s].CKey[%s] - O[%s] rangeable overrided\n", 
								oc->volume->signature, 
								oc->cache_key, 
								oc->origin_path));
				oc->query_result.stat.st_rangeable = 0; /*강제로 rangeable=0처리 */
			}
		}
	}



	if (oc->query_result.result != 0) {

		TRACE((T_INODE, "Volume[%s].Key[%s] - returned Open Context:%s\n",
					oc->volume->signature,
					oc->cache_key,
					dm_dump_open_context(ibuf, sizeof(ibuf), oc)
			  ));
		/*
		 * error 발생
		 */
		errno = __nc_errno = oc->query_result.result;
		/* exception case */
		switch (oc->query_result.result) {
			case ENOENT:
				/* we didn't found it from stat-cache */
				if (!IS_ON(oc->mode, O_CREAT)) { 
					TRACE((T_INODE, "Volume[%s].CKey[%s]/O[%s] - no such object in the origin\n", 
									oc->volume->signature, 
									oc->cache_key, 
									oc->origin_path));
					oc->open_result = DM_OPENR_NOTFOUND;
					oc->inode = NULL;
				}
				else {
					TRACE((T_INODE, "NEED TO CREATE\n"));
					goto L_need_create;
				}
				break;
			default:
				/* other error */
				oc->open_result = DM_OPENR_ALLOCFAILED;
				TRACE((T_INODE, "Volume[%s].CKey['%s'] could not be created because the origin has error(%d)\n", oc->volume->signature, oc->origin_path, errno));
				break;
	
		}
	}
	else {
L_need_create:
		TRACE((T_INODE, 	"Volume[%s].CKey[%s]/O[%s] - query ok, creating a cache object(st_vary=%d, key-changed=%d)\n", 
							oc->volume->signature, 
							oc->cache_key, 
							oc->origin_path, 
							(int)oc->query_result.stat.st_vary, 
							(int)oc->u._ubi.keychanged));
		/* 
		 * 원본 스토리지에 해당 경로의 객체가 존재하지만, caching 되어 있지 않은 상태 
		 * vary 객체의 경우는 cache-key를 새로 생성
		 */

		if (oc->u._ubi.keychanged) { 
			/*
			 *  M.10
			 */
			dm_upsert_template(oc);	/* vtime 갱신*/
			TRACE((T_INODE,	"Volume[%s].CKey[%s]/O[%s] - key-changed=%d, updating template ok\n", 
							oc->volume->signature, 
							oc->cache_key, 
							oc->origin_path, 
							(int)oc->u._ubi.keychanged
							));

			goto L_instance_creation;


		}
		else {

			if (oc->query_result.stat.st_vary) {
				/*
				 * M.8
				 * 
				 * 1. upsert(template)
				 * 2. key 변경 필요
				 * 3. key_changed=1
				 */
				dm_upsert_template(oc);
				if (oc->cache_key != oc->cache_key_given)
					XFREE(oc->cache_key);
				dm_create_key_pair( oc->volume,
									oc->origin_path,
									oc->query_result.stat.st_property,
									oc->cache_key_given,
									&oc->cache_key,
									oc->in_prop);
	

				oc->u._ubi.keychanged 	= U_TRUE;
				oc->open_result 		= DM_OPENR_AGAIN;
				TRACE((T_INODE, "Volume[%s].CKey[%s] - path[%s] is vary, new instance key gen'ed\n", 
								oc->volume->signature, 
								oc->cache_key,
								oc->origin_path
								));

		
				/*
				 * vary cache에 대해서 path-lock.
				 * 이  lock은 nc_close로 들어오는 요청과 serialization
				 */

				if (oc->inode) {
					ic_set_busy(oc->inode, U_FALSE, __FILE__, __LINE__);
				}

				oc->inode = dm_lookup_cache(oc->volume, 
											DM_LOOKUP_WITH_KEY, 
											oc->cache_key, 
											U_FALSE, 
											U_TRUE, 
											oc->pathlock, 
											__FILE__,
											__LINE__);

				TRACE((T_INODE, "Volume[%s].CKey[%s] - %s.{keychanged=%d}, INODE[%u] found;{%s}\n",
								oc->volume->signature,
								oc->cache_key,
								__z_apc_open_str[oc->state],
								oc->u._ubi.keychanged,
								(oc->inode?oc->inode->uid:-1),
								(oc->inode?dm_dump_lv2(ibuf, sizeof(ibuf), oc->inode):"")
								));
				if (oc->inode) {
					oc->inode = dm_apc_handle_event_for_freshcheck_internal(oc);
				}
				else {
					/* 
					 * M. 9 
					 * instance not exist in cache
					 */
L_instance_creation:
					
					oc->open_result = DM_OPENR_ALLOCFAILED;
					oc->inode = dm_create_cache_object(	oc->volume, 
														oc->pathlock, 
														&oc->cache_key,
														oc->origin_path,
														oc->mode,
														&oc->query_result.stat,
														oc->in_prop,
														oc->volume->version,	/* cache version */
														U_FALSE,				/* DONOT make ref */
														U_TRUE,					/* mark BUSY */
														__FILE__,
														__LINE__
														);
					if (oc->inode) {
						oc->open_result = DM_OPENR_ALLOCATED;
					}
				}
			}
			else {
				/*
				 * goto M.9
				 * normal object (not vary)
				 */
				goto L_instance_creation;
			}
		}


		if (oc->inode && (oc->open_result == DM_OPENR_ALLOCATED)) {
			inode = oc->inode;

			result = 0;
			if (INODE_GET_REF(oc->inode) == 0) {
				result = dm_make_inode_online(oc->inode, oc->pathlock, oc->in_prop);
				o_++;
			}


			if (result >= 0) {
				__nc_errno = errno = 0;
				hs = (oc->inode->ob_priv==0)?NC_OS_MISS:NC_OS_BYPASS;
				if (oc->u._ubi.stalled)
					hs = NC_OS_REFRESH_MISS;


				dm_make_report(oc->inode, oc->in_prop, hs);
			}
			else {
				/*
				 * activation 중 에러
				 */
				IC_LOCK(CLFU_LM_EXCLUSIVE);
				ic_set_busy(oc->inode, U_FALSE, __FILE__, __LINE__);
				nvm_isolate_inode_nolock(oc->volume, oc->inode);
				IC_UNLOCK;
				inode_tofree	= oc->inode;
				oc->inode		= NULL;
			}
		}
	}
L_open_alloc_done:
	
	if (inode_tofree) {
		dm_free_inode(inode_tofree, U_TRUE/*recycle*/);
	}

	return oc->inode;
}

/*
 * inode를 vit에서 찾고 refresh 검사를 위해서 
 * 원본 조회 요청 후 결과 받음
 * 		- open_context에서 지정된 inode는 open/close에 대해서는 
 *		  path-lock을 통해 배타적 소유권확보 (단 purge/disk정리
 * 		  가 실행되는 경우 progress flag를 통해서 사용중을 알려야함
 * 		
 */
static fc_inode_t *
dm_apc_handle_event_for_freshcheck(apc_open_context_t *oc)
{
	oc->open_result =  0;
	oc->inode = dm_apc_handle_event_for_freshcheck_internal(oc);
	return oc->inode;
}
static fc_inode_t *
dm_apc_handle_event_for_freshcheck_internal(apc_open_context_t *oc)
{
	/* 
	 * inode에 대해서 다른 thread들은 lookup에서 pending걸리는 상태
	 */
	nc_validation_result_t 			vr;
	char							ibuf[1024];
	fc_inode_t						*inode_free = NULL;
	int								ret = 0;;
	nc_hit_status_t					hs  = NC_OS_REFRESH_HIT;
	fc_inode_t						*pinode = NULL;
	nc_origin_session_t				porigin = NULL;




	DEBUG_ASSERT(oc->volume != NULL);
	DEBUG_ASSERT(oc->inode != NULL);
	DEBUG_ASSERT(nvm_path_lock_is_for(oc->pathlock, oc->origin_path));
	DEBUG_ASSERT(nvm_path_lock_ref_count(oc->pathlock) > 0);
	DEBUG_ASSERT(nvm_path_lock_owned(oc->volume, oc->pathlock));
	DEBUG_ASSERT(nvm_path_busy_nolock(oc->pathlock));

	oc->open_result =  0;

L_freshcheck:
	pinode = oc->inode; /* keep previous inode */
	DEBUG_ASSERT_FILE(pinode, pinode == NULL || ic_is_busy(pinode));

	vr = nvm_check_freshness_by_policy(oc->volume, oc->inode, &oc->query_result.stat, U_FALSE, oc->query_result.result == EREMOTEIO, NC_STALE_BY_ORIGIN_OTHER); 
	TRACE((oc->inode->traceflag|T_INODE, "Volume[%s].CKey[%s].INODE[%u](query_result=%d, down=%d) - freshness check result %d\n",
					oc->inode->volume->signature,
					oc->inode->q_id,
					oc->inode->uid,
					oc->query_result.result,
					oc->query_result.result == EREMOTEIO,
					vr));


	ret = -1;
	if (nvm_is_usable_state(oc->inode, vr)) {
		/*
		 * fresh
		 * activate it if not - path-lock에 
		 */

L_offline_access:
		ret = 0;
		if (oc->inode->ob_template)  {

			/*
			 * M.11
			 *
			 */
			pinode = oc->inode;

			if (oc->cache_key != oc->cache_key_given)
				XFREE(oc->cache_key);
			dm_create_key_pair( oc->volume,
								oc->origin_path,
								oc->inode->property, /* template prop 사용 */
								oc->cache_key_given,
								&oc->cache_key,
								oc->in_prop);
			TRACE((oc->inode->traceflag|T_INODE, "Volume[%s].CKey[%s].INODE[%u] - template cache, a Key[%s] created\n",
							oc->volume->signature,
							oc->inode->q_id, 
							oc->inode->uid, 
							oc->cache_key));



			ic_set_busy(oc->inode, U_FALSE, __FILE__, __LINE__);

			oc->u._ubi.keychanged 	= 1;
			oc->u._ubi.vary 		= 1;
			oc->inode 				= NULL;

			/*
			 * 새로운 key로 처음부터 다시 시작
			 */
			oc->inode = dm_lookup_cache(	oc->volume, 
											DM_LOOKUP_WITH_KEY, 
											oc->cache_key, 
											U_FALSE, 
											U_TRUE, 
											oc->pathlock, 
											__FILE__,
											__LINE__);
			TRACE((T_INODE, "Volume[%s].CKey[%s] - %s.{keychanged=%d}, INODE[%u] found;{%s}\n",
							oc->volume->signature,
							oc->cache_key,
							__z_apc_open_str[oc->state],
							oc->u._ubi.keychanged,
							(oc->inode?oc->inode->uid:-1),
							(oc->inode?dm_dump_lv2(ibuf, sizeof(ibuf), oc->inode):"")
							));
			if (!oc->inode) {
				/* 
				 *
				 * instance는 캐싱안됨, oc에 있는 정보(query_result)를 이용해서 생성
				 * 
				 */
				/*
				 * Exception 처리 추가:(2020.7.26 by weon)
				 *		- 신선도 검사는 통과했는데 여기서 객체 로딩중 에러 발생하는 경우
				 *		  query_result의 origincode가 3XX라서 엉뚱한 값이 캐싱될 수 있음
				 *
				 */
				if (!BETWEEN(oc->query_result.stat.st_origincode, 300, 399)) {
					oc->inode 	= dm_apc_handle_event_for_miss_internal(oc);
					TRACE((T_INODE, "Volume[%s].CKey[%s] -  loading disk-cache failed, INODE[%d] created from origin response\n",
								oc->volume->signature,
								oc->cache_key,
								oc->inode->uid
						  ));
				}
				else {
					/*
					 * 응답이 IMS응답임
					 * 이 경우 현재 원본과의 연결 세션을 통해서 가져온 정보를 사용할 수 없음
					 * template inode를 이용해서 temporal inode 생성하고 다음 읽깅요청에서
					 * property update
					 */
					porigin		= oc->origin;
					oc->origin	= NULL;
					oc->inode 	= dm_duplicate_inode(pinode, oc);
					/*
					 * template을 통해서 만들어진 객체라서 overwrite해야함
					 * 일부 정보가 날라간 상태
					 */
					oc->inode->ob_needoverwrite = 1;
					TRACE((T_INODE, "Volume[%s].CKey[%s] -  loading disk-cache failed, temporarily INODE[%d] created from template object(INODE[%d])\n",
								oc->volume->signature,
								oc->cache_key,
								oc->inode->uid,
								pinode->uid
						  ));
				}
				goto L_freshcheck_open_done;
			}
			else {
				/*
				 * M.13
				 * 바뀐 key로 재시도
				 */
				dm_upsert_template(oc);	/* vtime 갱신*/
				goto L_freshcheck;
			}
		}


		/*
		 *
		 * 필요하면 캐싱 겍체의 activation
		 */
		ret  = 0;
		if (INODE_GET_REF(oc->inode) == 0) {
			/*
			 *
			 * 
			 */
			ret = dm_make_inode_online(oc->inode, oc->pathlock, oc->in_prop);
			if (ret < 0) {
				TRACE((oc->inode->traceflag|T_INODE, "Volume[%s].CKey[%s] - INODE[%u] activation failed{%s}:errno=%d\n", 
								oc->volume->signature, 
								oc->inode->q_id, 
								oc->inode->uid, 
								dm_dump_lv1(ibuf, sizeof(ibuf), oc->inode),
								errno
								));
			}


		}


		if (ret >= 0) {

			IC_LOCK(CLFU_LM_EXCLUSIVE);
			dm_hit_clfu_nolock(oc->inode, 1) ;
			IC_UNLOCK;
			TRACE((T_INODE|oc->inode->traceflag, "INODE[%d]/R[%d] opened ok\n", oc->inode->uid, INODE_GET_REF(oc->inode)));
			/*
			 * activation success or refcnt > 0
			 */
			if (!oc->inode->ob_priv) {
				dm_update_ttl(oc->inode, &oc->query_result.stat, oc->in_prop, oc->query_result.result, oc->u._ubi.offcache != 0);
				dm_update_viewcount(oc->inode);
			}
			hs = (oc->u._ubi.offcache? NC_OS_OFF_HIT:NC_OS_REFRESH_HIT);

			dm_make_report(oc->inode, oc->in_prop, hs);
		}
	}
	else {
		/*
		 * 신선하지 않은 객체
		 */
		if ( vr == NC_STALE_BY_OFFLINE) {
			oc->u._ubi.offcache = dm_inode_offline_accessible(oc->volume, oc->inode);
			if (oc->u._ubi.offcache) {
				/*
				 * 아래와 같은 조건을 모두 만족하는 상황
				 * 	1. 광역 설정으로 원본 오프라인시 캐싱 객체 서비스 허용 설정
				 *  2. 원본이 OFFLINE 상태
				 *  3. 캐싱 객체가 full caching된 상태
				 */
				oc->open_result 	= DM_OPENR_STALLED_OFFLINE;
				TRACE((T_INODE|T_INFO, "Volume[%s].CKey[%s] - INODE[%u] going to be served in offline mode;%s\n", 
										oc->inode->volume->signature, 
										oc->inode->q_id, 
										oc->inode->uid,
										dm_dump_lv1(ibuf, sizeof(ibuf), oc->inode)
										));
				ret = 0;
				goto L_offline_access;

			}
		}

		ret = -1;
	}

	if (ret < 0) {
		/* 
		 * 1. 신선하지만 make_online 도중 에러난 객체
		 * 2. 신선도 검사에서 신선하지 않은걸로 판명된 객체
		 */

		inode_free = oc->inode;

		dm_remove_inode_while_close(oc->inode);

		oc->u._ubi.stalled 	= 1;
		oc->inode 			= NULL;
		oc->open_result 	= dm_apc_schedule_async_open(oc, APC_OS_QUERY_FOR_MISS, __FILE__, __LINE__);
	}
L_freshcheck_open_done:
	if (porigin) {
		cfs_free_session(oc->volume, porigin);
	}
	return oc->inode;
}
static void
dm_defer_and_delegate_close(void *v)
{
	dm_delegate_close((fc_inode_t *)v, 0, U_FALSE);
}
/*
 * application thread가 아닌 ASIO thread에서 
 * 단독 실행되는 함수,
 * ASIO event queue에 요청을 큐잉하기 전에 이미 path-lock의
 * refcount는 1 증가한 상태
 * 
 * 비동기 inode close 함수
 */
int
apcs_close_action(apc_context_t *apc)
{
	int						result 	= 0;
	close_context_t 		*cc 	= (close_context_t *)apc;
	fc_inode_t				*inode 	= cc->inode;
	nc_time_t				attempt = cc->firstattempt;
	nc_path_lock_t			*lock 	= inode->PL;
	nc_volume_context_t		*volume = inode->volume;
	char					dbuf[2048];
	nc_origin_session_t		origin = NULL;
	int						postpond = 0;


	XFREE(cc);

	DEBUG_ASSERT_FILE(inode, INODE_GET_REF(inode) > 0);
	DEBUG_ASSERT_FILE(inode, inode->volume != NULL);
	DEBUG_ASSERT_FILE(inode, inode->volume->refcnt > 0);
	DEBUG_ASSERT_FILE(inode, (nvm_path_lock_is_for(lock, inode->q_path)));
	DEBUG_ASSERT_FILE(inode, (nvm_path_lock_ref_count(lock) > 0));

	nvm_path_lock(volume, lock, __FILE__, __LINE__);
	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%d]/R[%d] async-part close started\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					INODE_GET_REF(inode)
					));

	if ((nc_cached_clock() - attempt) > 120) {

		nc_asio_vector_t	*v;
		char				vbuf[256];
		int					ino = 0;

		/*
		 * close가 60초 이상 지연되고 있음
		 */
		INODE_LOCK(inode);
		asio_signal_cancel(inode);

		TRACE((T_ERROR, "Volume[%s].CKey[%s].INODE[%u] - close postponded more than for 60 secs;{%s}\n", 
						inode->volume->signature, 
						inode->q_id, 
						inode->uid, 
						dm_dump_lv1( dbuf, sizeof( dbuf ), inode )
						));
		// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
		//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
		//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
		//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
		v = (nc_asio_vector_t *)link_get_head_noremove( &inode->pending_ioq );
		while( v ) {
			TRACE(( T_ERROR, "Volume[%s].CKey[%s].INODE[%u] - %d-th(st) IO vector:%s\n", 
							 inode->volume->signature,
							 inode->q_id,
							 inode->uid,
							 ++ino,
							 asio_dump_lv1( vbuf, sizeof( vbuf ), v )
							 ));
			v = (nc_asio_vector_t *)link_get_next( &inode->pending_ioq, &v->node );
		}
		INODE_UNLOCK(inode);

		attempt = nc_cached_clock();
	}

L_start_again:
	IC_LOCK(CLFU_LM_SHARED);
	if (INODE_GET_REF(inode) > 1) {
		TRACE((inode->traceflag|T_INODE, "INODE[%d] - skip closing\n", inode->uid));
   		INODE_UNREF(inode);
		IC_UNLOCK;
		goto L_fast_close_done;
	}



	if (dm_is_iobusy(inode)) {
		/*
		 * 
		 * 여기에서 wait하느라고 무한대기하는 경우
		 * asio event handling thread가 부족해져서 queue에 
		 * 대기중인 다른 event처리 못함. 결국 hang으로 이어질 수 있음
		 * (2020.1.3 by weon@solbox.com)
		 */
		IC_UNLOCK;
		TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%d]/R[%d] - IO busy, close posponded again\n",
						inode->volume->signature,
						inode->q_id,
						inode->uid,
						INODE_GET_REF(inode)
						));

		bt_set_timer(__timer_wheel_base,  &inode->t_defer, 1000, dm_defer_and_delegate_close, inode);
		postpond = 1;
		goto L_fast_close_done;
	}
	IC_UNLOCK;

	/*
	 * real close; no body refers to this object
	 */

#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(nvm_path_lock_owned(inode->volume, inode->PL));
	DEBUG_ASSERT_FILE(inode, nvm_volume_valid(inode->volume));
#endif
	dm_detach_driver(inode, U_FALSE);


	if (dm_need_to_remove(inode)) {
		/*
		 * inode locked
		 * refcnt == 1
		 * odoc == 1 || private == 1
		 * isolated from VIT
		 */

		INODE_UNREF(inode);

		TRACE((inode->traceflag|T_INODE, "Volume[%s].CKey[%s].INODE[%u] - doc[%d]/priv[%d] going to be removed;{%s}\n", 
						inode->volume->signature, 
						inode->q_id, 
						inode->uid, 
						(inode->ob_doc != 0), 
						(inode->ob_priv != 0),
						dm_dump_lv2(dbuf, sizeof(dbuf), inode)
						));

		nvm_purge_inode(inode->volume, inode, U_FALSE, NULL);
		_ATOMIC_SUB(g__inode_opened, 1);

	}
	else {
		/*
		 * valid close (refcnt == 1)
		 */
		DEBUG_ASSERT_FILE (inode,
						((INODE_GET_REF(inode) == 1) ||
						((INODE_GET_REF(inode) == 1) && (dm_check_concurrent_limit_nolock(inode) == 0))  
						));


		if (dm_disk_cacheable(inode, inode->origincode) && dm_need_sync(inode)) { 
			dm_update_caching_completeness_nolock(inode);
#ifdef NC_ENABLE_LAZY_HEADER_SYNC
			lazysync = (nc_cached_clock() - inode->lastsynctime) > 1800; /* 마지막 동기 후 30분 지났는지 확인 */
			if ((synced == 0) && lazysync) {
	
				dm_swap_cacheobject_ifneededed(inode);
	
			}
#else
			dm_swap_cacheobject_ifneededed(inode);
#endif
		}
		MDB_UPDATE_RESET(inode, U_FALSE);

		if (DM_FD_VALID(inode->fd))  {
			DM_CLOSE_FD(inode->fd);
			inode->ob_created 	= 0;
		}

   		INODE_UNREF(inode); /*위치 변경 절대 안됨 */
		if (INODE_GET_REF(inode) == 0)
			_ATOMIC_SUB(g__inode_opened, 1);
		
		inode->PL = NULL;
		TRACE((inode->traceflag|T_INODE, "INODE[%d]/R[%d] - finally unrefred\n", inode->uid, INODE_GET_REF(inode)));
	}/* normal valid inode */

	origin = dm_bind_origin_session(inode, NULL,__FILE__, __LINE__);

L_fast_close_done:
	if (!postpond) {
		/*
		 * nc_close()때 비동기 close로 인해 유예된 카운터
		 * 정상 감소시킴
		 */
		_ATOMIC_SUB(g__pending_close, 1);
		DEBUG_ASSERT(VOLUME_INUSE(volume));
		VOLUME_UNREF(volume);
	}

	DEBUG_ASSERT(nvm_path_lock_owned(volume, lock));
	nvm_path_unlock(volume, lock, __FILE__, __LINE__);

	//
	// delegate() 에서  path_lock ref함(왜냐하면 app은 이미 close()완료하고 리턴함)
	//

	if (postpond == 0)
		nvm_path_lock_unref_reuse(volume, lock, __FILE__, __LINE__);

	dm_free_origin_session(volume, origin, U_FALSE,  __FILE__, __LINE__);

	return result;
}

/*
 * event handler(asio thread)에서 호출됨(user-thread에서 호출되는 함수아님)
 * 완료 event를 수신
 * open context가 inode 정보를 가지고 있다면 대상 inode의 progress != 0임
 */
#ifndef NC_RELEASE_BUILD
__thread int	t_scheduled = 0;
#endif
int
apcs_open_action_LOCK(apc_context_t *apc)
{
	apc_open_context_t 			*oc = (apc_open_context_t *)apc;
	fc_inode_t					*inode = NULL;
	fc_file_t					*fh = NULL;
	nc_stat_t					stat;
	char 						ibuf[2048];
	char 						cbuf[2048];
	char 						sbuf[128];
	int							res = 0;
	nc_origin_session_t 		porigin = NULL;
	int							scheduled = 0;

#ifndef NC_RELEASE_BUILD
	t_scheduled = 0;
#endif




	XVERIFY(oc);

	DEBUG_ASSERT(nvm_volume_valid(oc->volume));
	DEBUG_ASSERT(VOLUME_INUSE(oc->volume));

	nvm_path_lock(oc->volume, oc->pathlock, __FILE__, __LINE__);

	DEBUG_ASSERT_PATHLOCK(oc->pathlock, nvm_path_lock_ref_count(oc->pathlock) > 0);
	DEBUG_ASSERT_PATHLOCK(oc->pathlock, nvm_path_lock_is_for(oc->pathlock, oc->origin_path));
	DEBUG_ASSERT_PATHLOCK(oc->pathlock, nvm_path_lock_owned(oc->volume, oc->pathlock));
	DEBUG_ASSERT_PATHLOCK(oc->pathlock, nvm_path_busy_nolock(oc->pathlock)); 




	TRACE((T_INODE, "Volume[%s].CKey[%s] - event arrived for O-CTX[%p]\n"
					"\t\t\t O-CTX: {%s}\n"
					"\t\t\t INODE: {%s}\n"
					"\t\t\t STAT : result=%d, {%s}\n", 
					oc->volume->signature, 
					oc->cache_key, 
					oc,
					dm_dump_open_context(cbuf, sizeof(cbuf), oc),
					(oc->inode?dm_dump_lv1(ibuf, sizeof(ibuf), oc->inode):""),
					oc->query_result.result, 
					stat_dump(sbuf, sizeof(sbuf), &oc->query_result.stat)
					));

	

	switch (oc->state) {
		case APC_OS_QUERY_FOR_MISS:
			inode = oc->inode = dm_apc_handle_event_for_miss(oc);

			
			break;
		case APC_OS_QUERY_FOR_REFRESHCHECK:
			inode = oc->inode = dm_apc_handle_event_for_freshcheck(oc);
			break;
		case APC_OS_WAIT_FOR_COMPLETION: /* 다른 thread에서 완료 후 post해준 event*/
			__nc_errno = 0;
			TRACE((T_INODE, "Volume[%s].CKey[%s] - wake up CONTEXT{%s}\n",
								oc->volume, 
								oc->cache_key, 
								dm_dump_open_context(cbuf, sizeof(cbuf), oc)));

			inode = dm_apc_open_inode_internal(oc);


			if (inode == NULL && __nc_errno == EWOULDBLOCK) {
					/* 비동기 스케줄됨 */
					TRACE((T_INODE, "Volume[%s].CKey[%s] - wake up but found would block, scheduing CONTEXT{%s}\n",
								oc->volume, 
								oc->cache_key, 
								dm_dump_open_context(cbuf, sizeof(cbuf), oc)));
					/*
					 * 새로운 context 가 dm_apc_open_inode_oc()에서 생성되어 스케줄링됨
					 */
					res = 0;

				goto L_inprogress_continued;
			}
				/*
				 * 에러 종료 처리 시작
				 */
			oc->query_result.result = __nc_errno;
			break;
#if defined(__clang__)
		case APC_OS_INIT:
		case APC_OS_FREE:
			break;
#endif
	}



	memset(&stat, 0, sizeof(stat));

	if (inode) {

		if (inode->ob_complete == 0 && oc->origin) {
			porigin = dm_bind_origin_session(inode, oc->origin,__FILE__, __LINE__);
			oc->origin = NULL;
		}
		fh = fio_make_fhandle(inode, oc->pathlock, oc->mode, oc->in_prop);

		/* CHG 2018-07-04 huibong ASSERT 구문 뒤 ';' 누락 수정 (#32230) */
		//ASSERT( ( strcmp( inode->q_id, oc->cache_key ) == 0 ) || ( strcasecmp( inode->q_id, oc->cache_key ) == 0 ) );

		TRACE((inode->traceflag|T_INODE, "Volume[%s].CKey[%s] - INODE[%u] async'ly opened for {%s} CTX{%s} \n",
								oc->volume->signature,
								oc->cache_key,
								inode->uid, 
								dm_dump_lv2(ibuf, sizeof(ibuf), inode),
								dm_dump_open_context(cbuf, sizeof(cbuf), oc)
								));
		dm_copy_stat(&stat, inode, U_FALSE);
	}
	else {
		fh = NULL;
		memcpy(&stat, &oc->query_result.stat, sizeof(nc_stat_t));
		stat.st_property = NULL;
	}


#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT((t_scheduled == 0 && oc->open_result != DM_OPENR_SCHEDULED)||
				 (t_scheduled != 0 && oc->open_result == DM_OPENR_SCHEDULED)
			);
#endif
	if(oc->open_result != DM_OPENR_SCHEDULED) {
		scheduled = nvm_post_wakeup(oc->volume, inode, oc->pathlock, &stat, __nc_errno);
		if (scheduled == 0) {
			/*
			 * 추가 scheduling이 없는 상태, progress를 reset해도 됨
			 */
			TRACE((T_INODE, "Volume[%s].CKey[%s] - progress to IDLE;%s\n",
								oc->volume->signature,
								oc->cache_key,
								nvm_path_lock_dump(cbuf, sizeof(cbuf), oc->pathlock)
				  ));

			nvm_path_inprogress_nolock(oc->pathlock, U_FALSE, __FILE__, __LINE__);
		}
		(*oc->callback)(fh, &stat, oc->query_result.result, oc->callback_data);
		DEBUG_ASSERT_PATHLOCK(oc->pathlock, nvm_path_lock_ref_count(oc->pathlock) > 0);
		DEBUG_ASSERT_PATHLOCK(oc->pathlock, nvm_path_lock_is_for(oc->pathlock, oc->origin_path));
		DEBUG_ASSERT_PATHLOCK(oc->pathlock, nvm_path_lock_owned(oc->volume, oc->pathlock));
	}
L_inprogress_continued:
	/*
	 *  L_inprogress_continued로 점프해온 경우 oc->volume 의 refcnt가 유지됨을 주목
	 *  도한 아직도 path-lock의 inprogress가 유지됨도 주목
	 */
	DEBUG_ASSERT(VOLUME_INUSE(oc->volume));
	VOLUME_UNREF(oc->volume);
	nvm_path_unlock(oc->volume, oc->pathlock, __FILE__, __LINE__);

	dm_free_origin_session(oc->volume, porigin, U_FALSE, __FILE__, __LINE__);
	DEBUG_ASSERT_PATHLOCK(oc->pathlock, !nvm_path_lock_owned(oc->volume, oc->pathlock));
	apc_destroy_open_context(oc);
	return res;
}

/*
 *
 * data structure의 메모리 할당
 *
 * @off : offset or size
 * 
 */

int
dm_resize_extent(fc_inode_t *inode, nc_off_t off)
{
	long 			t_required_blks; 
	int 			Bsize;
	int 			Psize;
	int 			Msize;
	int 			LPsize;
	int				t_mapped;



	 /* 
	  * 할당이 필요한 실제 블럭 수 
	  * 항상 한 바이트 만큼 더 수용할 수 있도록 블럭 할당 필요 
	  */


	t_required_blks 	= NC_BITMAP_LEN(off + 1);  /* 한 바이트만큼  더 */;

	if (inode->mapped > t_required_blks) {
		/* 충분히 확보되어서, 재조정 필요없음*/
		return 0;
	}
	t_mapped = t_required_blks + 5;


	/*
	 * 각 구조체의 크기 계산
	 */
	Bsize 	= NC_CANNED_BITMAP_SIZE(t_mapped); 
#ifdef NC_ENABLE_CRC
	Psize 	= NC_CANNED_CRC_SIZE(t_mapped);/* in bytes */
#else
	Psize 	= 0;
#endif
	Msize 	= NC_CANNED_BLOCKMAP_SIZE(t_mapped);/* in bytes */

	/*
	 * LPmap size
	 */
	LPsize 	= NC_CANNED_LPMAP_SIZE(t_mapped);

	/* bitmap */
	inode->bitmap   	= (unsigned long *) mem_realloc(&g__inode_heap, inode->bitmap, Bsize, AI_INODE, __FILE__, __LINE__);

	/* pbitmap */
	inode->pbitmap   	= (unsigned long *) mem_realloc(&g__inode_heap, inode->pbitmap, Bsize, AI_INODE, __FILE__, __LINE__);

	inode->whint_map	= (unsigned long *) mem_realloc(&g__inode_heap, inode->whint_map, Bsize, AI_INODE, __FILE__, __LINE__);

#ifdef NC_ENABLE_CRC
	/* block crc */
	inode->crcsize 		= Psize;
	inode->blockcrc   	= (nc_crc_t *) mem_realloc(&g__inode_heap, inode->blockcrc, Psize, AI_INODE, __FILE__, __LINE__);
#endif
	inode->LPmap   		= (nc_blkno_t *) mem_realloc(&g__inode_heap, inode->LPmap, LPsize, AI_INODE, __FILE__, __LINE__);


	BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
		/*
		 * blockmap은  clfu 락 내에서 관리
		 */
		inode->blockmap = (fc_blk_t **) mem_realloc(&g__inode_heap, inode->blockmap, Msize, AI_INODE, __FILE__, __LINE__);
	}


	inode->mapped = t_mapped;
	TRACE((T_INODE, "INODE[%u] - mapped size increased to %d blks(might be larger than the real size)\n", inode->uid, t_mapped));
	return 0;
}



static int
dm_compare_blkno(void *a, void *b)
{
	apc_read_context_t	*ra  = ( apc_read_context_t	*)a;
	apc_read_context_t	*rb  = ( apc_read_context_t	*)b;

	/*
	 * ctx_waitq내의 대기 request들이 wanted에 명시된 block no 오름순으로
	 * 정렬되도록 함
	 */
	return (int)(rb->wanted - ra->wanted);

}
int
dm_add_ctx_waitq(fc_inode_t *inode, apc_read_context_t *rctx)
{
	char				ibuf[1024];
	char				zrctx[128];


	DEBUG_ASSERT(dm_inode_check_owned(inode));
	//TRACE((T_WARN, "RC[%p] - scheduled\n", rctx));
	set_bit(rctx->wanted, inode->whint_map);
	rctx->age = GET_ATOMIC_VAL(inode->ctx_waitq_age);
	link_add_n_sort(&inode->ctx_waitq, (void *)rctx, &rctx->node, dm_compare_blkno);

	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u]/R[%d] - (%s) added to wait-queue;%s\n", 
					inode->volume->signature,
					inode->q_id, 
					inode->uid, 
					INODE_GET_REF(inode), 
					blk_apc_dump_context(zrctx, sizeof(zrctx), rctx),
					dm_dump_lv1(ibuf, sizeof(ibuf), inode)
					));
	
	return 0;
}
apc_read_context_t *
dm_schedule_apc_read(	fc_inode_t 				*inode, 
						char 					*buffer, 
						nc_off_t 				foffset, 
						nc_size_t 				size, 
						nc_xtra_options_t 		*inprop,
						apc_block_ready_callback_t 	callback, 
						blk_apc_read_info_t 	*pri
		)
{

	apc_read_context_t	*rctx = NULL;
	char				zrctx[128];
	char				zri[128];

	rctx = (apc_read_context_t *)XMALLOC(sizeof(apc_read_context_t), AI_IOCTX);

	rctx->fmagic 	= rctx->rmagic = 0x5aa5a55a;
	rctx->type 		= APC_APP_READ;
	rctx->wanted 	= BLK_NO(foffset, 0);
	rctx->foffset	= foffset;
	rctx->len		= size;
	rctx->cursor 	= buffer;
	rctx->remained 	= size;
	rctx->error 	= 0;
	rctx->inprop 	= inprop;
	rctx->inode 	= inode;
	rctx->signature = inode->signature;


	rctx->cb_complete 	= callback; 
	rctx->cb_data 		= pri; 

	if (pri) 
		pri->rctx				= rctx;

	TRACE((T_BLOCK|T_INODE, "Volume[%s]:INODE[%u]/R[%d] - {%s}/{%s} prepared\n", 
							inode->volume->signature,
							inode->uid, 
							INODE_GET_REF(inode), 
							blk_apc_dump_context(zrctx, sizeof(zrctx), rctx),
							blk_apc_dump_RI(zri, sizeof(zri), pri)
							));


	/*
	 *
	 * 이미 inode lock이 걸린 상태임
	 *
	 */
	dm_add_ctx_waitq(inode, rctx);
	return rctx;
}
/*
 * blkno == -1 
 */
struct dump_wait_info {
	char 	*buf;
	int		off;
	int		len;
};
static int
dm_dump_rctx(void *ud, void *cbd)
{
	apc_read_context_t		*rctx = (apc_read_context_t	*)ud;
	struct dump_wait_info 	*dwi = (struct dump_wait_info *)cbd;
	char					zri[128];
	int						n;
	if (dwi->off != 0)
		n = snprintf(dwi->buf+dwi->off, dwi->len - dwi->off, ",{%s}", blk_apc_dump_context(zri,sizeof(zri), rctx));
	else
		n = snprintf(dwi->buf+dwi->off, dwi->len - dwi->off, "{%s}", blk_apc_dump_context(zri,sizeof(zri), rctx));

	dwi->off += n;
	return 0;
}
char *
dm_dump_waitq(char *buf, int len, fc_inode_t *inode)
{
	struct dump_wait_info	dwi = {buf, 0, len};

	link_foreach(&inode->ctx_waitq, dm_dump_rctx, &dwi);

	return buf;
}
void
dm_cancel_waitq(fc_inode_t *inode, apc_read_context_t *rc)
{
	DEBUG_ASSERT(dm_inode_check_owned(inode));
	link_del(&inode->ctx_waitq, &rc->node);
	//TRACE((T_WARN,"RC[%p] - cancel&destroyed\n", rc));
	blk_destroy_read_apc(rc);

}
int 
dm_run_waitq(fc_inode_t *inode, fc_blk_t * blk, int error)
{
	apc_read_context_t 		*rctx = NULL;
	apc_read_context_t 		*rctx_next = NULL;
	int						hitcnt = 0;
	char					wbuf[1024]="";
	blk_apc_read_info_t 	*pri;

	DEBUG_ASSERT(dm_inode_check_owned(inode));

	_ATOMIC_ADD(inode->ctx_waitq_age, 1);

	rctx 	= link_get_head_noremove(&inode->ctx_waitq);



	TRACE((inode->traceflag|T_INODE, "Volume[%s].Key{%s]:: INODE[%u]/R[%d]/FRIO[%d] - got {blk#%u, error[%d:%s]} ;; %d ctx on wait-queue(%s)\n", 
					inode->volume->signature,
					inode->q_id,
					(unsigned)inode->uid, 
					(int)inode->refcnt, 
					(int)(inode->ob_frio_inprogress?1:0), 
					(blk?blk->blkno:-1),
					(int)error, 
					(char *)strerror(error),
					(int)link_count(&inode->ctx_waitq, U_TRUE),
					dm_dump_waitq(wbuf, sizeof(wbuf), inode)
					));

	while (rctx && (error || blk->blkno >= rctx->wanted)) { 

		rctx_next 	= link_get_next(&inode->ctx_waitq, &rctx->node);

		if ((rctx->age == GET_ATOMIC_VAL(inode->ctx_waitq_age)) || 
			 (blk && (blk->blkno != rctx->wanted))) {

			rctx = rctx_next;
			continue;
		}


		link_del(&inode->ctx_waitq, &rctx->node);

		rctx->error = error;


		TRACE((inode->traceflag||T_BLOCK|T_INODE, 
						"Volume[%s].CKey[%s].INODE[%u]/RI[%p] - got blk#%u(error=%d:%s), completing callback(waitq = %d remained) \n", 
						rctx->inode->volume->signature,
						rctx->inode->q_id,
						rctx->inode->uid,
						rctx->cb_data,
						(blk?blk->blkno:-1L),
						error,
						(char *)strerror(error),
						(int)link_count(&inode->ctx_waitq, U_TRUE)));
		pri = (blk_apc_read_info_t *)__sync_val_compare_and_swap(&rctx->cb_data, rctx->cb_data, NULL);
		if (rctx->cb_complete && pri) {
			/* 
			 * blk_ri_timeout 실행이 리턴된뒤 이함수가 호출되는 경우
			 * pri->rctx는 invalid memory 주소를 가질 수 있음
			 */
			pri->rctx = NULL;
#ifdef NC_PASS_BLOCK_WQ
			(*rctx->cb_complete)((void *)blk, (int)(rctx->len - rctx->remained), (int)error, (void *)pri);
#else
			(*rctx->cb_complete)(NULL, (int)(rctx->len - rctx->remained), (int)error, (void *)pri);
#endif
		}
		hitcnt++;
		//TRACE((T_WARN,"RC[%p] - run&destroyed\n", rctx));
		blk_destroy_read_apc(rctx);
		rctx = rctx_next;
	}


L_run_finish:

	if (blk) {
		clear_bit(blk->blkno, inode->whint_map); 
#ifdef NC_PASS_BLOCK_WQ
		BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
			bcm_hit_block_nolock(blk, hitcnt); 
		}
#endif
	}
	dm_update_iobusy(inode);
	return hitcnt;
}


/*
 * inode의 크기 정보가 원본과 통신하면서 계속 변경
 * 수정가능한 필드
 * 		rsize : 초기에 0, 데이타를 수신하면서 크기 변경
 */
int
dm_update_size_with_RI_nolock(fc_inode_t *inode, nc_off_t off, nc_size_t len)
{
	long 				tflg = T_INODE;
	nc_blkno_t			tblkno;
	char				ibuf[2048];
	int					updated = 0;
	nc_size_t			new_availsize;
	int					toobig = 0;


	if (inode->ob_frio_inprogress && (inode->fr_availsize < off+len)) {
		
		/*
		 * update real maxblkno/bitmaplen
		 */
		new_availsize			= max(inode->fr_availsize, off+len);
		tblkno					= dm_max_blkno(new_availsize); /* size */
		DEBUG_ASSERT(new_availsize != inode->fr_availsize);
		if (new_availsize > inode->fr_availsize) {
			/*
			 * resize는 커지는 방향만 고려
			 */
			dm_resize_extent(inode, new_availsize);
			toobig++;
		}
		if (inode->maxblkno < tblkno) {
			TRACE((T_INODE, "INODE[%d]/R[%d] - maxblkno changed : %d -> %d\n", inode->uid, INODE_GET_REF(inode), inode->maxblkno, tblkno));
			inode->maxblkno		= tblkno;
			inode->bitmaplen	= inode->maxblkno+1;
		}
		inode->fr_availsize 	= new_availsize;
		updated++;
	
		TRACE((tflg, 	"Volume[%s].CKey[%s].INODE[%u]/(%lld,%lld) - frsize[%lld], maxblkno[%u] %s;{%s} \n", 
						inode->volume->signature, 
						inode->q_id, 
						inode->uid, 
						(long long)off,
						(long long)len,
						inode->fr_availsize, 
						inode->maxblkno,
						((toobig?"(EXCEEDED beyond the declared size)":"")),
						dm_dump_lv2(ibuf, sizeof(ibuf), inode)
						));
	}
	return updated;
}
/*
 * 호출전에 inode는 lock이 걸려있음
 * @return values: 
 *		0 : ok, proceed to the next
 *		-1: ERROR, this inode should not be reused(cached)
 */
int
dm_finish_frio_nolock(fc_inode_t *inode, int error)
{
	char 	ibuf[1024];
	int		r = 0;
	int		tflg = T_INFO;

	if (inode->ob_frio_inprogress) {
		inode->ob_sizeknown			= 1;
		inode->ob_frio_inprogress	= 0;
		inode->rsize				= inode->fr_availsize; /*  실제 데이타 크기 갱신 */
		inode->maxblkno				= dm_max_blkno(inode->rsize);

		/*
		 * IO 종료 후 재검토할 작업 처리
		 */
		inode->header_size			= dm_calc_header_size(inode);
		dm_run_waitq(inode, NULL, EAGAIN); /* pending된 작업들에대해서 EOF 확정 후 재시도 */

		DM_UPDATE_METAINFO(inode, U_TRUE);

		dm_need_header_update(inode);
		if (error) {
			/*
			 * 2021.3.4: 에러처리
			 */
			inode->staleonclose = 1;
			r		= -1;
			tflg	= 0;
		}
		if (inode->ob_sizedecled && (inode->size != inode->rsize)) {
			TRACE((tflg|T_INODE, "Volume[%s].CKey[%s]/ERROR[%d] - caching size(%lld) is different from the declared one(%lld):%s\n",
							inode->volume->signature, 
							inode->q_id, 
							error,
							(long long)inode->rsize,
							(long long)inode->size,
							dm_dump_lv1(ibuf, sizeof(ibuf), inode)
							));
		}
		TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - FRIO finish(rsize=%lld, header_size=%d);{%s}\n", 
						inode->volume->signature, 
						inode->q_id, 
						inode->uid, 
						(long long)inode->rsize,
						(int)inode->header_size,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)
						));
	}
	return r;
}
/*
 * inode 크기
 * (1) 메모리할당과 같은 경우에는  dm_inode_size(inode, U_FALSE)를 사용해야함
 * (2) blk_read..()의 경우에는 dm_inode_size(inode, U_TRUE)로 사용해야함
 */
nc_size_t 
dm_inode_size(fc_inode_t *inode, int realdatasize)
{
	nc_size_t		tsiz;
	/*
	 * 오리진에서 size 정보로 준 숫자와
	 * 실제 수신한 데이타 크기와 다를 수 있음
	 */
#if 0
	if (!realdatasize) {
		if (inode->ob_sizedecled) 
			tsiz = inode->size;
		else
			tsiz = inode->fr_availsize;
	}
	else {
		tsiz = (inode->ob_rangeable?inode->size:inode->fr_availsize);
	}
#else
	if (inode->ob_sizedecled) {
		if (inode->ob_frio_inprogress) {
			tsiz =  (realdatasize?inode->fr_availsize:inode->size);
		}
		else {
			tsiz =  (realdatasize?inode->rsize:inode->size);
		}
	}
	else if (inode->ob_sizeknown) 
		tsiz = inode->rsize;
	else if (inode->ob_frio_inprogress)
		tsiz = inode->fr_availsize;
	else
		tsiz = 0; /* size unknown 이고 IO 시작도 안됨 */
#endif
	TRACE((T_INODE,  "Volume[%s].Key[%s]/R[%d] - INODE[%d].%s size %lld\n",
					inode->volume->signature,
					inode->q_id,
					INODE_GET_REF(inode),
					inode->uid,
					(char *)(realdatasize?"REAL":"DEF'd"),
					tsiz));
	return tsiz;
}
void
dm_update_ttl(fc_inode_t *inode, nc_stat_t *stat, nc_kv_list_t *reqkv, int result, int offline)
{
	int					def = 0;



	if (cfs_iserror(inode->volume, stat->st_origincode) || offline || result > 0) {

		if (result > 0)
			TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u]: result[%d], negative ttl will be applied(offline=%d, origincode=%d)\n", 
								inode->volume->signature,
								inode->q_id,
								inode->uid, 
								result, 
								offline, 
								stat->st_origincode));

		/* error case, negative ttl applied */
		if (reqkv && reqkv->nttl > 0) {
			def			 	= 1;
			DM_UPDATE_METAINFO(inode, inode->vtime 	= nc_cached_clock() + reqkv->nttl);
			TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - st.origincode[%d], result[%d], reqkv.nttl[%d] applied to vtime[%u]\n", 
							inode->volume->signature,
							inode->q_id, 
							inode->uid, 
							stat->st_origincode, 
							result, 
							reqkv->nttl, 
							inode->vtime));
		}
		else {
			DM_UPDATE_METAINFO(inode, (inode->vtime 	= (stat->st_vtime != 0)? stat->st_vtime:(nc_cached_clock() + inode->volume->n_ttl)));
			def			 	= 1;
		}
	}
	else {
		if (reqkv && reqkv->pttl > 0) {
			DM_UPDATE_METAINFO(inode, (inode->vtime = nc_cached_clock() + reqkv->pttl));

			TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - st.origincode[%d], result[%d], reqkv.pttl[%d] applied to vtime[%u]\n", 
							inode->volume->signature,
							inode->q_id, 
							inode->uid, 
							stat->st_origincode, 
							result, 
							reqkv->pttl, 
							inode->vtime));
		}
		else {
			inode->vtime = stat->st_vtime;
			if (inode->vtime == 0) {
				def = 1;
				inode->vtime = nc_cached_clock() + inode->volume->p_ttl;
			}
			DM_UPDATE_METAINFO(inode, U_TRUE);
		}

	}
	TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - st.origincode[%d], result[%d] set to vtime[%u](by_config[%d])\n", 
					inode->volume->signature,
					inode->q_id, 
					inode->uid, 
					stat->st_origincode, 
					result, 
					inode->vtime,
					def));
}
/*
 * need full-range IO
 */
int
dm_need_frio(fc_inode_t *inode)
{
	inode->ob_frio_inprogress	= 1;
	inode->fr_availsize			= 0;
	return 0;
}

char *
dm_dump_open_context(char *buf, int len, apc_open_context_t *o)
{
	/* CHG 2018-04-17 huibong 문자열 출력인자 1개 누락 수정 (#32018) */
	snprintf(buf, len, "O-CTX[%p].{%s:R[%d],fNC[%d],fPV[%d],xbits{%s%s%s%s%s},O.R[%d],O.Code[%d]}",
						o,
						__z_apc_open_str[o->state],
						o->open_result, 
						o->force_nocache,
						o->force_private,
						(o->u._ubi.vary?"Va ":""),
						(o->u._ubi.stalled?"ST ":""),
						(o->u._ubi.keychanged?"KC ":""),
						(o->u._ubi.offcache?"OFF ":""),
						(o->u._ubi.activating?"A ":""),
						o->query_result.result,
						o->query_result.stat.st_origincode
						);
	return buf;
}
static int
dm_is_vary_meta_object_nolock(fc_inode_t *inode)
{
	return (inode->ob_template);
}






/*
 * FOR HISTORY
 *
 * 		IC_LOCK의 사용을 최소한의 영역(VIT table한정)으로 변경한 이유는
 * 		이미 대상 URL영역에 대해서 path-lock이 확보되어 있으므로
 */

static int
dm_upsert_template(apc_open_context_t *openx)
{
	int 					result = DM_OPENR_ALLOCATED,
							created = 0;
	fc_inode_t 				*inode = NULL, 
							*lookup  = NULL;
	nc_stat_t				tstat;
	int						upgraded = 0;

	XVERIFY(openx);
	DEBUG_ASSERT(nvm_path_lock_is_for(openx->pathlock, openx->origin_path));
	DEBUG_ASSERT(nvm_path_lock_ref_count(openx->pathlock) > 0);
	DEBUG_ASSERT(nvm_path_lock_owned(openx->volume, openx->pathlock));

	lookup =
	inode = dm_lookup_cache(	openx->volume, 
								DM_LOOKUP_WITH_KEY, 
								openx->cache_key_given, 
								U_FALSE, 
								U_FALSE, 
								openx->pathlock, 
								__FILE__,
								__LINE__);

	if (!inode) {
		/*
		 * template(meta) inode 등록
		 * 		- template 등록시 st_property가 사용되어버리지 않도록 NULL로 임시
		 *		  바꿔치기해야함
		 */
		memcpy(&tstat, &openx->query_result.stat, sizeof(nc_stat_t));
		tstat.st_property 	= kv_clone_d(openx->query_result.stat.st_property, __FILE__, __LINE__);

		inode 	= ic_prepare_inode_raw_nolock(	openx->volume,  
												openx->cache_key_given, 
												openx->origin_path,  
												openx->mode,
												&tstat,  /* tstat 사용 */
												openx->in_prop, 
												U_TRUE, /*complete*/
												openx->volume->version,
												U_TRUE, /*TEMPLATE*/
												U_TRUE,		/*set progress*/
												__FILE__,
												__LINE__
												);
		/*
		 * private 객체라고 하더라도 MDB에 일단 등록
		 */

		inode->ob_priv 		= 0; /*2014.10.25 강제로 ob_priv제거, 안그러면 삭제됨*/
		inode->ob_nocache 	= 0; /*2014.10.25 강제로 nocache제거, 안그러면 삭제됨*/
		inode->ob_memres 	= 0; 

		/*
		 * VIT 및 CLFU에 등록
		 */
		IC_LOCK(CLFU_LM_EXCLUSIVE);
		lookup = ic_register_cache_object_nolock(openx->volume, inode); /* CLFU에 등록 후, reclaim되지 않도록. */
		ic_set_busy(inode, U_FALSE, __FILE__, __LINE__);
		DEBUG_ASSERT(inode == lookup);
		IC_UNLOCK; upgraded++;
		created++;
	}

	if (lookup != inode) {
		TRACE((T_WARN, "meta INODE[%u] - registration failed, already INODE[%u] exists\n", inode->uid, lookup->uid));

		dm_free_inode(inode, U_TRUE/*recycle*/); /* xclusive ic-lock필요*/
	}
	else {


		if (!inode->part)  {
			/*
			 * 새로 생성된 경우 inode->part = NULL임
			 * 아래에서 mdb 갱신을 위해서는 partition 선택되어 있어야함
			 */
			inode->part			= pm_elect_part(0, U_FALSE); /* MDB 위치 선택 */
		}
		dm_update_ttl(inode, &openx->query_result.stat, openx->in_prop, openx->query_result.result, openx->u._ubi.offcache != 0);
		if (inode->part) {
			if(inode->rowid < 0) {
				inode->rowid	= pm_add_cache(inode->part, inode, U_TRUE);
				if (inode->rowid < 0) {
					TRACE((T_ERROR, "VOlume[%s].Key[%s] - INODE[%d[ template adding failed\n", openx->volume->signature, inode->q_id, inode->uid));
				}
			}
			else
				pm_add_cache(inode->part, inode, U_FALSE);
		}
		TRACE((T_INODE, "INODE[%u]/CKey[%s]/O[%s] - vary meta %s\n", inode->uid, inode->q_id, inode->q_path, (created?"created":"updated")));
	}


	return result;

}
nc_off_t
dm_cache_offset(fc_inode_t *inode, nc_blkno_t blkno)
{
	nc_off_t	off = 0;

	off = inode->doffset + blkno*NC_BLOCK_SIZE;
	return off;
}


/*
 * 대상 inode의 구조체에 연결된 모든 heap 할당을 free하고
 * extent가 할당되어있었다면 extent까지 free
 * inode는 free-list에 전달
 * 주의 사항) 
 *		1. MDB update나 캐시 파일에 관련된 변경은 이 함수에서 실행되지 않음
 *		2. 이 함수에서 volume에 대한 어떤 참조도 valid하지 않음
 *	
 *
 * 호출 조건
 * 		- clfu_cached_nolock() == 0
 * 		- refcount == 0
 *
 */
void
dm_free_inode(fc_inode_t *inode, int need_recycle)
{
	nc_origin_session_t	origin = NULL;

	DEBUG_ASSERT_FILE(inode, (ic_is_cached_nolock(inode) == 0));
	DEBUG_ASSERT_FILE(inode, (INODE_GET_REF(inode) <=  1));
	DEBUG_ASSERT_FILE(inode, (dm_check_concurrent_limit_nolock(inode) == 0));
	DEBUG_ASSERT_FILE(inode, (link_count(&inode->pending_ioq, U_TRUE) == 0));
	DEBUG_ASSERT_FILE(inode, (link_count(&inode->ctx_waitq, U_TRUE) == 0));
	DEBUG_ASSERT_FILE(inode, inode->cstat == IS_ORPHAN);


	inode->signature = 0;
	_ATOMIC_CAS(inode->cstat, IS_ORPHAN, IS_FREED);

	MEM_FREE_CLEAR(&g__inode_heap, inode->q_id, U_FALSE);
	MEM_FREE_CLEAR(&g__inode_heap, inode->q_path, U_FALSE);

	dm_invalidate_blks(inode);

	inode->ctime = 0; /* 중복free검사용*/

	if (inode->property)  {
		TRACE((T_INODE, "PROP[%p] destroying from INODE[%u]\n", inode->property, inode->uid));
		kv_destroy(inode->property);
		inode->property = NULL;
	}


	while (bt_del_timer_v2(__timer_wheel_base, &inode->t_origin) < 0) {
		bt_msleep(1);
	}
	origin = dm_bind_origin_session(inode, NULL,__FILE__, __LINE__);
	if (origin)
		dm_free_origin_session(inode->volume, origin, U_FALSE, __FILE__, __LINE__);

	XFREE(inode->c_path);

	MEM_FREE_CLEAR(&g__inode_heap, inode->bitmap, U_FALSE);
	MEM_FREE_CLEAR(&g__inode_heap, inode->pbitmap, U_FALSE);
	MEM_FREE_CLEAR(&g__inode_heap, inode->whint_map, U_FALSE);

	MEM_FREE_CLEAR(&g__inode_heap, inode->blockmap, U_FALSE);
	MEM_FREE_CLEAR(&g__inode_heap, inode->LPmap, U_FALSE);
#ifdef NC_ENABLE_CRC
	MEM_FREE_CLEAR(&g__inode_heap, inode->blockcrc, U_FALSE);
#endif

	DEBUG_ASSERT_FILE(inode, inode->driver_file_data == NULL);
	DM_CLOSE_FD(inode->fd);


	if (need_recycle) {
		/*
		 * free-list에 등록
		 */
		IC_LOCK(CLFU_LM_EXCLUSIVE);
		ic_free_object_nolock(inode);
		IC_UNLOCK;
	}
}
/*
 * inode's current state
 *
 * inode->refcnt = 0
 */ 
static void
dm_remove_inode_while_close(fc_inode_t *inode)
{


	char 	ibuf[1024];
	//int		recheck = 0;
	int		r;


	TRACE((inode->traceflag|T_INODE, "Volume[%s].CKey[%s].INODE[%u]/R[%d] - removing(CVersion[%u])\n", 
					inode->volume->signature,
					inode->q_id, 
					inode->uid, 
					inode->refcnt, 
					inode->cversion
					));

	DEBUG_ASSERT_FILE(inode, (INODE_GET_REF(inode) >= 0));



	IC_LOCK(CLFU_LM_EXCLUSIVE);

	ic_set_busy(inode, U_FALSE, __FILE__, __LINE__);

	if ((r = nvm_isolate_inode_nolock(inode->volume, inode)) < 0) {
		TRACE((inode->traceflag|T_INODE, "Volume[%s].CKey[%s] - INODE[%u]/R[%d] isolation %s;%s\n",
						inode->volume->signature,
						inode->q_id,
						inode->uid,
						inode->refcnt,
       	            	((r >= 0)?"OK":"FAIL"),
       	            	dm_dump_lv1(ibuf, sizeof(ibuf), inode)
       	            	));
	}
	IC_UNLOCK;

L_recheck:

	if (INODE_GET_REF(inode) == 0) {
		//int 	cur_ref = 0;


		DEBUG_ASSERT_FILE(inode, !dm_is_iobusy(inode));
		TRACE((inode->traceflag, "INODE[%d] - purging...\n", inode->uid));

		nvm_purge_inode(inode->volume, inode, U_TRUE, NULL);
	}
	else {
L_fast_return:
		TRACE((inode->traceflag|T_INODE, "Volume[%s].CKey[%s] : INODE[%d]/R[%d] in use, setting DoC;%s\n", 
						inode->volume->signature,
						inode->q_id,
						inode->uid, 
						inode->refcnt,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)
						));
		inode->ob_doc = 1;
	}
	return ;
}
static int
dm_open_file_ifnot(int fd, char *fname, nc_size_t fsize, int bcreat, int bronly)
{
	DEBUG_ASSERT(fname != NULL);
	if (!DM_FD_VALID(fd)) {
		fd = dio_open_file(fname, 0, bcreat, bronly);
	}
	return fd;
}

/*
 * 'blkno'에서 부터 최대 maxe 갯수만큼의 블럭의 상태 스캔
 *		- bci가 INPROGRESS이면 거기서 중단 후 리턴
 *		- 
 *
 *		@baseno	 : 기준 블럭 번호
 *		@maxm	 : 최대 연속된 BCI_BLOCK_ONMEMORY 갯수
 *		@maxf	 : 최대 연속된 BCI_BLOCK_FAULT 갯수
 *		@maxd	 : 최대 연속된 BCI_BLOCK_ONDISK 갯수
 *		@ba		 : array of bci_stat_t
 *		@maxe	 : array 크기(갯수) 
 */
int
dm_block_status_batch_nolock(fc_inode_t *inode, nc_blkno_t baseno, bci_stat_t *ba, int maxm, int maxd, int maxf, int maxe, int makeref)
{

	int			i = 0;
	int			nbci = 0;
	int			c_m = 0, c_f = 0, c_d = 0;
	nc_blkno_t	blkno_limit = inode->maxblkno;

	DEBUG_ASSERT(__bc_locked);
	if (!INODE_SIZE_DETERMINED(inode)) {
		blkno_limit = inode->mapped;
	}
	TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%u]/max[%u], base=%u, maxm=%d, maxd=%d, maxf=%d, maxe=%d(size detemined=%d)\n", 
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					inode->maxblkno,
					baseno,
					maxm,
					maxd,
					maxf,
					maxe,
					(int)INODE_SIZE_DETERMINED(inode)
					));



	while ( (i < maxe) &&
			((baseno + i) <= blkno_limit) &&
			(c_m <= maxm) && 
			(c_d <= maxf) &&
			(c_f <= maxf)) { 
		ba[i].block = NULL;
		ba[i].bci 	= dm_block_status_nolock(inode, &ba[i].block, baseno + i, makeref, __FILE__, __LINE__);


		switch (ba[i].bci) {
			case DM_BLOCK_IOPROGRESS:
				/*
				 * 더이상 진행안함, 이 값을 받은 caller는 다시 대기 상태로 가야함
				 */
				nbci++;
				goto L_stop_scan;
				break;
			case DM_BLOCK_ONMEMORY:
				if (c_f > 0 || c_d > 0) {
					/*
					 * 연속된 network-fault나 disk-fault 블럭 스캔 중에
					 * memory-caching된 블럭 발견
					 * nbci에 현재 값은 포함되지 않도록 함
					 */
					blk_make_unref_nolock(ba[i].block);
					goto L_stop_scan;
				}
				c_m++;
				break;
			case DM_BLOCK_ONDISK:
				if (c_f > 0) {
					/*
					 * 연속된 network-fault난 블럭을 스캔중인 상태에서
					 * disk-caching된 블럭 발견
					 * nbci에 현재 값은 포함되지 않도록 함
					 * nbci값은 이후 blk_fault_batch_nolock()에서 사용	
					 */
					goto L_stop_scan;
				}
				c_d++;
				break;
			case DM_BLOCK_FAULT:
				if (c_d > 0) {
					/*
					 * 연속된 disk-caching된 블럭 스캔중인데
					 * network fault발생
					 * nbci에 현재 값은 포함되지 않도록 함
					 * nbci값은 이후 blk_fault_batch_nolock()에서 사용	
					 */
					goto L_stop_scan;
				}
				c_f++;
				break;
		}

		nbci++;
		i++;


		if (c_m > maxm) {
			/*
			 * 연속 onmemory block이 최대 갯수에 도달
			 */
			break;
		}
		if (c_d > maxd) {
			/*
			 * 연속 disk cached block이 최대 갯수에 도달
			 */
			break;
		}
		if (c_f > maxf) {
			/*
			 * 연속 fault block이 최대 갯수에 도달
			 */
			break;
		}

	}
L_stop_scan:
	return nbci;
}
/*
 *
 */
__thread int 	__bc_locked = 0;

fc_block_status_t
dm_block_status_nolock(fc_inode_t *inode, fc_blk_t **blk, nc_blkno_t blkno, int makeref, char *f, int l)
{
	fc_block_status_t	bci 	= DM_BLOCK_FAULT;
	register nc_block_state_t	tbstate;
	char						vbuf[128]="";


	DEBUG_ASSERT_FILE(inode, INODE_GET_REF(inode) > 0);
	DEBUG_ASSERT(__bc_locked);

	*blk = bcm_lookup_LIGHT(inode, blkno, U_FALSE, f, l);

	if (*blk) {
		/*
		 * lock은 걸려있지 않음
		 */
#ifdef NC_BLOCK_TRACE
		DEBUG_ASSERT((*blk)->bmagic == BLOCK_MAGIC);
#endif
		if (BLOCK_VALID(inode, *blk, blkno)) {
			tbstate = (*blk)->bstate;

			if (((*blk)->binprogress == 0) &&((tbstate == BS_CACHED) || (tbstate == BS_CACHED_DIRTY)))
			{

				if (makeref) blk_make_ref_nolock(*blk);

				bci = DM_BLOCK_ONMEMORY;
			}
			else if ((tbstate == BS_FAULT)  && ((*blk)->binprogress == 1)) {
				bci 	= DM_BLOCK_IOPROGRESS;
				*blk	= NULL;
			}
			else {
				char bbuf[256];
				TRACE((T_INFO|T_INODE, "INODE[%u] - blk#%u INVALID(%d);%s(ORG:%s)\n", 
								inode->uid, 
								blkno, 
								BLOCK_VALID(inode, *blk, blkno),
								bcm_dump_lv1(bbuf, sizeof(bbuf), *blk),
								vbuf));
				*blk	= NULL;
				bci = DM_BLOCK_FAULT;
			}
		}
		else if (BLOCK_IS_PG_MARKED(*blk)) {
			/*refcnt 없음*/
			bci 	= DM_BLOCK_IOPROGRESS;
			*blk	= NULL;
		}
		else {
			bci = DM_BLOCK_FAULT;
			*blk	= NULL;
		}
	}

	/*
	 *
	 * 위의 조건 검사에서 해당 사항이 없는 경우
	 *
	 */
	if (bci == DM_BLOCK_FAULT) {
		int		dc;
		dc = DM_CHECK_RBOD(inode, blkno);
		if (dc)  {
			bci = DM_BLOCK_ONDISK;
		}
		else if (inode->ob_frio_inprogress) 
			bci = DM_BLOCK_IOPROGRESS;

	}
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT( ((bci == DM_BLOCK_ONMEMORY) && (*blk != NULL)) ||
			((bci == DM_BLOCK_IOPROGRESS||bci == DM_BLOCK_ONDISK||bci == DM_BLOCK_FAULT) &&
			 (*blk == NULL))
		  );
	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u].blk#%u is '%s'(maxblkno=%u, size=%lld, fr_availsize=%lld)\n",  
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					blkno,
					_str_block_status[bci],
					inode->maxblkno,
					inode->size,
					inode->fr_availsize
					));
#endif
	return bci;
}

/* CHG 2018-06-21 huibong return type 을 int -> void 로 변경 (#32205) */
void
dm_swap_cacheobject_ifneededed(fc_inode_t *inode)
{
	char 	hq_path[1024];
	char 	hq_id[1024];
	size_t 	hdrsiz = 0;
	char	dbuf[1024];
	char	pbuf[1024];
	int		oldfd = DM_NULL_FD;
	int		cmpl = 0;

	if (dm_disk_cacheable(inode, inode->origincode)) {

		if (dm_need_sync(inode)) { 
			cmpl = dm_update_caching_completeness_nolock(inode);

			/*
			 * upgrade 여부는 reset해도 됨(이미 diskcaching 상태이므로)
			 */
			oldfd = inode->fd;

			inode->fd = dm_open_file_ifnot(inode->fd, inode->c_path, 0, inode->ob_created, U_FALSE);

			inode->ob_upgraded 			= 0;
			inode->ob_upgradable 		= 0;


			dm_sync_disk_object(inode, U_TRUE, &hdrsiz, "periodic sync");
			TRACE((0, "Volume[%s].CKey[%s] - %s\n"
					   "\t\t\t ID                : %s\n"
					   "\t\t\t Size              : %s\n"
					   "\t\t\t Object-properties : {%s}\n"
					   "\t\t\t Header size       : normal=%ld B, compressed=%ld B\n"
					   "\t\t\t Allocated inode   : %d\n",
						inode->volume->signature, 
						(char *)hz_string(hq_path, sizeof(hq_path),inode->q_path), 
						"header written to disk 'cause too long time elapsed",
						(char *)hz_string(hq_id, sizeof(hq_id),inode->q_id), 
						(inode->ob_sizeknown?ll2dp(dbuf, sizeof(dbuf), (long long)inode->size):"unknown"), 
						(char *)obi_dump(pbuf, sizeof(pbuf), &inode->obi),
						(long)inode->header_size, 
						(long)hdrsiz,
						(long)(inode->maxblkno+1),
						inode->uid
						));
			if (inode->fd != oldfd) {
				DM_CLOSE_FD(inode->fd);
				inode->fd = oldfd;
			}
		}
	}
}



void
dm_verify_allocation(fc_inode_t *inode)
{
#ifndef NC_RELEASE_BUILD
	XVERIFY(inode->blockmap);
#ifdef NC_ENABLE_CRC
	XVERIFY(inode->blockcrc);
#endif
	XVERIFY(inode->bitmap);
#endif
}
/*
 * 이 함수는 inode의 refcnt=1이고
 * cache에서 제외된 상태에서 
 * 다른 Thread에 간섭받지 않고 호출됨
 */
static int
dm_invalidate_blks(fc_inode_t *inode)
{
	fc_blk_t 					*blk;
	nc_blkno_t					blkno;
	int							n = 0;



	/*
	 * 굳이 exclusive lock필요없음 
	 * bcm_alloc(reclaim)은 exclusive lock이 획됙되어야하는데, 
	 * 아래에서 shared lock을 사용하는 동안 exclusive lock 획득 시도는
	 * 은 어차피 대기해야함
	 */
	if (inode->blk_locked) {
		for (blkno = 0; blkno <= inode->maxblkno; blkno++) {
			BC_TRANSACTION(CLFU_LM_SHARED) {
				blk = inode->blockmap[blkno];
				if (BLOCK_VALID(inode, blk, blkno)) {
					blk->bblocked 	= 0;
					blk->signature = 0;
				}
			}
		}
	}
	return n;
}
static int
dm_update_iobusy(fc_inode_t *inode)
{
	inode->iobusy = (link_count(&inode->pending_ioq, U_TRUE) > 0) ||
					(link_count(&inode->ctx_waitq, U_TRUE) > 0);
	return 0;
}
static int
dm_is_iobusy(fc_inode_t *inode)
{
	return inode->iobusy != 0;
}
static fc_inode_t *
dm_duplicate_inode(fc_inode_t *oi, apc_open_context_t *oc)
{
	nc_stat_t	ns;
	fc_inode_t	*inode, *lookup;

	dm_copy_stat(&ns, oi, U_TRUE);

	inode 	= ic_prepare_inode_raw_nolock(	oc->volume,  
											oc->cache_key, 
											oc->origin_path,  
											oc->mode,
											&ns,
											oc->in_prop,
											U_FALSE/*complete*/, 
											oi->cversion,
											U_FALSE, /*template*/
											0, /*progress flag */
											__FILE__,
											__LINE__
											);

	IC_LOCK(CLFU_LM_EXCLUSIVE);
	lookup  = ic_register_cache_object_nolock(oc->volume, inode); 

	DEBUG_ASSERT(inode == lookup);
	IC_UNLOCK; 

	return inode;
}

static fc_inode_t *
dm_create_cache_object(	nc_volume_context_t *volume, 
						nc_path_lock_t		*pathlock, 
						char				**cache_key,
						char				*object_path,
						nc_mode_t			mode,
						nc_stat_t			*cstat,
						nc_xtra_options_t 	*in_prop,
						nc_uint32_t 		cversion,		/*cache version */
						u_boolean_t			makeref, 
						int					markbusy, 
						char			*f, 
						int					l
						)
{
	fc_inode_t			*inode = NULL;
	fc_inode_t			*lookup = NULL;
	char				ibuf[2048];
	char				zuuid[128];
	struct stat			_s;
	int					tflg = 0;

	DEBUG_ASSERT(VOLUME_INUSE(volume));
	DEBUG_ASSERT(nvm_path_lock_is_for(pathlock, object_path));
	DEBUG_ASSERT(nvm_path_lock_ref_count(pathlock) > 0);
	DEBUG_ASSERT(nvm_path_lock_owned(volume, pathlock));


	TRACE((T_INODE, "Volume[%s].CKey[%s] - creating a cache object\n",
					volume->signature,
					*cache_key
		  ));


/*
 ******************************************************************************
 *
 * INODE 생성
 *
 ******************************************************************************
 */

#if 0
	if (!dm_check_public(origin_path, stat, mode)) {
		/*
		 * public이 아닌 객체에 대한 key 생성: 어차피 한번만 사용됨
		 */
		ol = strlen(*cache_key);
		*cache_key = XREALLOC(*cache_key, strlen(*cache_key) + 64 + 1, AI_OPENCTX);
		(*cache_key)[ol] = ';';
		sprintf(*cache_key+ol+1, "%llu.%llu", g__tb_seed, _ATOMIC_ADD(g__tb_seq, 1));
	}
#endif

	inode 	= ic_prepare_inode_raw_nolock(	volume,  
											*cache_key, 
											object_path,  
											mode,
											cstat, 
											in_prop,
											U_FALSE/*complete*/, 
											cversion,
											U_FALSE, /*template*/
											markbusy, /*progress flag */
											f,
											l
											);

	DEBUG_ASSERT(inode != NULL); /*  정상이면 무조건 생성되어야함 */

	lookup = inode;
	if (inode->ob_priv == 0) {
		/*
		 * cache-manager에 의해서 관리되고,검색되도록 등록
		 */
		IC_LOCK(CLFU_LM_EXCLUSIVE);
		lookup 	= ic_register_cache_object_nolock(volume, inode); /* CLFU에 등록 */
		DEBUG_ASSERT(inode == lookup);
		IC_UNLOCK;
	}
	DEBUG_ASSERT(lookup == inode); /* 정상 생성이면 생성과 등록이 같아야함 */

/*
 ******************************************************************************
 *
 * 디스크 상에 캐싱 객체 파일 설정 및 생성
 *
 ******************************************************************************
 */
			


	if (dm_disk_cacheable(inode, cstat->st_origincode)) { 

		inode->part	= pm_elect_part(inode->size, U_FALSE);
		if (inode->part == NULL) {
			/*
			 * 강제 선택 모드,대신 priv으로 변경
			 */
			inode->part	= pm_elect_part(inode->size, U_TRUE);

			/*
			 * inode를 VIT에서 제거
			 * private으로 cache 생성하도록 전략 변경
			 */
			IC_LOCK(CLFU_LM_EXCLUSIVE);
			nvm_isolate_inode_nolock(inode->volume, inode); 
			IC_UNLOCK;
			inode->ob_priv = 1;
			tflg		   = T_INFO;
		}
		DEBUG_ASSERT(inode->part != NULL);



		/*
		 * disk cache 경로 생성
		 */
		dm_make_uuid(inode->uuid);
		inode->ob_validuuid	= 1; /* uuid is valid */

		inode->c_path		= pm_create_cache_path(inode->part, inode->ob_priv != 0, inode->uuid, AI_ETC);

		if (dm_is_mdb_required(inode)) {
			inode->rowid		= pm_add_cache(inode->part, inode, U_TRUE);
			TRACE((T_INODE, "INODE[%d] - UUID[%s] created(ROWID[%lld])\n",
							inode->uid, 
							uuid2string(inode->uuid, zuuid, sizeof(zuuid)),
							inode->rowid
				  ));
		}


		inode->fd			= dio_open_file(inode->c_path, 0, U_TRUE, U_FALSE); 

		DEBUG_ASSERT(DM_FD_VALID(inode->fd) );

		TRACE((tflg|T_INODE, "Volume[%s].CKey[%s].INODE[%u] - cache[%s] created(stat=%d):%s\n", 
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					inode->c_path,
					stat(inode->c_path, &_s),
					dm_dump_lv1(ibuf, sizeof(ibuf), inode)
					));
	}
	else {
		TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - cache[%s] created(stat=%d):%s\n", 
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					inode->c_path,
					stat(inode->c_path, &_s),
					dm_dump_lv1(ibuf, sizeof(ibuf), inode)
					));
		inode->ob_memres = 1; 
	}
	inode->ob_created 	= 0;

	if (makeref) 
		INODE_REF(inode);

	return inode;
}
/*
 *  VIT 테이블에서 검색된 inode에 대해서 IO 가능하도록 추가 setup
 *
 */
static int
dm_make_inode_online(fc_inode_t *inode, nc_path_lock_t *pl, nc_xtra_options_t *kv)
{
	int 		ret = 0;
	char		ibuf[2048];


	DEBUG_ASSERT(nvm_path_lock_is_for(pl, inode->q_path));
	DEBUG_ASSERT(nvm_path_lock_ref_count(pl) > 0);
	DEBUG_ASSERT(nvm_path_lock_owned(inode->volume, pl));
	DEBUG_ASSERT_FILE(inode, INODE_GET_REF(inode) == 0);


	if (inode->ob_validuuid != 0) {
		/*
		 * inode->fd 가 invalid 일때만 실제 open 실행 
		 */
		inode->fd = dm_open_file_ifnot(inode->fd, inode->c_path, 0, U_FALSE, U_FALSE);
		TRACE((T_INODE, "volume[%s].CKey[%s] - making INODE[%u] online FD=%d ;%s\n", 
						inode->volume->signature,
						inode->q_id,
						inode->uid,
						inode->fd,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)
						));

		if (!DM_FD_VALID(inode->fd)) {
			/*
			 * 아마 파일 존재 안함 
			 */
			return -1;
		}
	}

	inode->PL = pl;

	if(!inode->driver_file_data)
		inode->driver_file_data	= cfs_open(inode->volume, inode->q_path, NULL, 0, inode->imode, inode->property);

	_ATOMIC_ADD(g__inode_opened, 1);
	
#if NOT_USED
#pragma message(REMIND("바깥으로 이동:시험"))
	if (inode->ob_memres && inode->ob_upgradable) {
		dm_upgrade_if_hot(inode);
	}

	inode->blk_locked = (inode->ob_memres && inode->ob_rangeable == 0);
#endif

	TRACE((T_INODE, "volume[%s].CKey[%s] - making INODE[%u] online returns %d;%s\n", 
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					ret,
					dm_dump_lv1(ibuf, sizeof(ibuf), inode)
					));
	return ret;
}
