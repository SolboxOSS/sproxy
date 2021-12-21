/*
 * path-lock의 관리
 * 		- pool - free path lock 자원을 광역으로 관리
 * 		- pl_tbl - 각 volume의 name-space에서 사용중인 또는 사용되고, 아직 뺏기지 않고 등록된 path-lock
 * 						   을 관리하기위한 hash table
 * 		- LRU - 광역 구조체로써, 모든 할당된 path-lock을 LRU 형태로 할당관리
 */
#include <config.h>
#include <stdio.h>
#include <stddef.h>
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
#include <regex.h>
#ifndef WIN32
#include <fnmatch.h>
#endif
#include <search.h>
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
#include "util.h"
#include "glru.h"
#define	PL_MAGIC	0xA55A5AA4

#define		__ASSERT_LOCK_OWNED(l) 		DEBUG_ASSERT((l)->__data.__owner == trc_thread_self())
#define		PATHLOCK_OWNED(l) 				((l)->__data.__owner == trc_thread_self())
#define		__ASSERT_LOCK_RELEASED(l) 	DEBUG_ASSERT(!((l)->__data.__owner == trc_thread_self() && (l)->__data.__count > 0))


#define 	MAX_STAT_LIVE_TIME 		(60*60)

#ifdef NC_DEBUG_PATHLOCK
nc_int32_t 					g__pl_poolsize = 256;
#else
nc_int32_t 					g__pl_poolsize = 5000;
#endif

nc_int32_t					g__pl_refs 	   = 0; /* for debugging*/




extern nc_ulock_t 			g__inode_cache_lock;
extern fc_clfu_root_t		*g__inode_cache;
extern long					g__cm_interval;
extern void  *				__timer_wheel_base; 
extern int					__positive_TTL;
extern int					__negative_TTL;
extern time_t				__nc_clock;
extern int 					g__pending_inodeop;
extern int 					g__pending_open;
extern int 					g__pending_close;
extern int 					g__pending_io;
extern int 					g__pending_stat;
extern nc_uint64_t 			g__write_count;
extern nc_uint64_t 			g__write_bytes;
extern int 					g__need_stat_backup;
extern char 				g__fresh_result[][32];
extern nc_int32_t 			g__pl_poolsize;
extern int                  g__readahead_MB;
extern __thread int          __nc_errno;



#ifdef NC_MEASURE_PATHLOCK
//static  pthread_spinlock_t s__pls_lock;
static  pthread_mutex_t s__pls_lock = PTHREAD_MUTEX_INITIALIZER;
#endif



/*
 * 
 * 
 */
static u_hash_table_t  				*s__volume_dictionary = NULL;
static pthread_mutex_t				s__volume_dictionary_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static link_list_t					s__volume_pool = LIST_INITIALIZER;

/*
 *
 * 할당된 path-lock을 광역 LRU 캐쉬로 관리하기 위한 변수
 * 검색 범위는 volume->PL_tbl 한정임
 *
 */
glru_t 								*g__path_cache = NULL ;
static char __z_fresh_str[][32] = {
		"NC_FRESH:0", 				/* 아직 사용가능한 상태 */
		"NC_REFRESH_FRESH:1", 		/* 유효기간 지나서 체크해본 결과 사용가능한 상태*/
		"NC_STALE_BY_ETAG:2", 		/* 캐싱의 ETAG값이 상이하여 STALE처리 됨*/
		"NC_STALE_BY_MTIME:3", 		/* 객체 변경시간이 상이하여 STALE처리 됨 */
		"NC_STALE_BY_OTHER:4", 		/* 여타 다른 이유로 STALE처리됨 */
		"NC_STALE_BY_OFFLINE:5", 	/* 여타 다른 이유로 STALE처리됨 */
		"NC_STALE_BY_SIZE:6",
		"NC_STALE_BY_ALWAYS:7",
		"NC_STALE_BY_ORIGIN_OTEHR:8",
		"NC_VALIDATION_CODENEXT:9",
		"NC_VNONE:10"
};

static char __z_ps[][32] = {
	"PL_RESOURCE_IDLE",
	"PL_RESOURCE_INPROGRESS"
};

static char							s__default_fc_policy[]="mtime,devid,size";



typedef int (*nc_validation_step_t)(	nc_volume_context_t 	*volume, 
										fc_inode_t 				*inode, 
										nc_stat_t 				*stat, 
										int 					*fresh, 
										int 					*reason, 
										nc_validation_result_t 	*,
										int						fcid
										);
typedef struct tag_freshcheck_entry {
	char 					fcname[32];
	int 					fcid;
	nc_validation_step_t	step;
	link_node_t 			node;
} nc_freshcheck_entry_t;

#define	FCG_STOP			0	/* 신선도 검사를 여기에서 종료 */
#define	FCG_CONTINUE		1	/* 신선도 검사를 계속 진행 */



static int nvm_parse_freshness_policy(nc_volume_context_t *volume, char *policy);
static unsigned long nvm_hash_inode(void *v1, void *ud);
static int nvm_check_freshness_prolog(						nc_volume_context_t *	volume, 
															fc_inode_t*				inode, 
															nc_stat_t *				stat, 
															int *					fresh, 
															int *					checked, 
															nc_validation_result_t*	reason,
															int 					fcid);
static int nvm_check_freshness_by_iftag(					nc_volume_context_t *volume, 
															fc_inode_t *inode, 
															nc_stat_t *stat, 
															int *fresh, 
															int *checked, 
															nc_validation_result_t *reason,
															int	fcid);
static nc_validation_result_t nvm_check_freshness_by_mtime(	nc_volume_context_t *volume, 
															fc_inode_t *inode, 
															nc_stat_t *stat, 
															int *fresh, 
															int *checked, 
															nc_validation_result_t *reason,
															int fcid);
static nc_validation_result_t nvm_check_freshness_by_etag(	nc_volume_context_t *volume, 
															fc_inode_t *inode, 
															nc_stat_t *stat, 
															int *fresh, 
															int *checked, 
															nc_validation_result_t *reason,
															int fcid);
static int nvm_check_freshness_by_size(						nc_volume_context_t *volume, 
															fc_inode_t *inode, 
															nc_stat_t *stat, 
															int *fresh, 
															int *checked, 
															nc_validation_result_t *reason,
															int fcid);
static int nvm_check_freshness_by_always_expire(			nc_volume_context_t *volume, 
															fc_inode_t *inode, 
															nc_stat_t *stat, 
															int *fresh, 
															int *checked, 
															nc_validation_result_t *reason,
															int fcid);


static void * 	nvm_pc_allocate(glru_node_t **gnodep);
static void *	nvm_pc_free(glru_node_t *gnode, void *data);
static void 	nvm_pc_reset(void *d);
static void 	nvm_pc_fill_key(void *d, const char * key);
static int 		nvm_pc_dump(char *b, int l, void *d);
static void *	nvm_pc_lookup(void *map, const char *key);
static int 		nvm_pc_enroll(void *map, glru_node_t *node);
static int 		nvm_pc_unroll(void *map, glru_node_t *node);
static int 		nvm_pc_isidle(void *d);
static void 	nvm_origin_path_change_proc(fc_inode_t *inode, char *new_path);
static int 		nvm_lazy_property_updater(fc_inode_t *inode, nc_stat_t *runtime_stat);
static void * 	nvm_destroy_internal(void * u);;
static int 		nvm_check_if_same_object(cfs_origin_driver_t *drv, fc_inode_t *inode, nc_stat_t *stat);
static nc_volume_context_t * nvm_create_nolock(	char *drvname,  char *signature, nc_origin_info_t *oi, int oicnt, int ignore_error);

#ifdef NC_MEASURE_PATHLOCK
static pls_t * nvm_lookup_pl_stat(char *f, int l);
static void nvm_update_pl_stat(pls_t *found, double msec);
#endif

static link_list_t		s__nvm_dthread_list = LIST_INITIALIZER;
static pthread_mutex_t	s__nvm_lock 		= PTHREAD_MUTEX_INITIALIZER;


/*
 * volume 생성 관리를 위한 path-lock의 할당을 위한
 * 특별한 volume
 */
static nc_volume_context_t	*s__reserved_vol = NULL;

typedef struct {
	pthread_t		tid;
	link_node_t		node;
} nvm_dti_t;



#ifdef USE_VM_STAT
static void 
nvm_activity_monitor(void *d)
{
	nc_volume_context_t	*mc = (nc_volume_context_t *)d;
	char 			obuf[128];
	char 			cbuf[128];
	nc_uint32_t		ios = 0;
	long long 		wc = 0;
	long long 		wb = 0;
	nc_uint64_t 	o_bytes;
	nc_uint64_t 	c_bytes;
	nc_uint32_t		interv = g__cm_interval/1000;
	ios =	(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_getattr)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_open)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_close)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_readdir)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_read)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_create)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_unlink)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_rename)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_mkdir)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_rmdir)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_utimens)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_write)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_truncate)+
			(nc_uint32_t)GET_ATOMIC_VAL(mc->cnt_mknod);
	wc = GET_ATOMIC_VAL(g__write_count);
	wb = GET_ATOMIC_VAL(g__write_bytes);
	o_bytes = GET_ATOMIC_VAL(mc->cnt_origin_bytes);
	c_bytes = GET_ATOMIC_VAL(mc->cnt_client_bytes);
	if (ios > 0) {
		TRACE((T_PERF, "VOLUME : '%s' activity monitor : interval : %d secs ***** \n"
					"\t\t\t\t\t + getattr : %8d + open    : %8d + close  : %8d\n"
					"\t\t\t\t\t + readdir : %8d + read    : %8d\n"
					"\t\t\t\t\t + create  : %8d + unlink  : %8d + rename : %8d\n"
					"\t\t\t\t\t + mkdir   : %8d + rmdir   : %8d + utimens: %8d\n"
					"\t\t\t\t\t + write   : %8d + truncat : %8d\n"
					"\t\t\t\t\t + IO speed: %ld/sec     avg write size: %.2fB/op total: %ld\n"
					"\t\t\t\t\t + Traffic Saving: %.2f %% (origin io bytes: %s  user io bytes: %s)\n",
					mc->signature,
					interv,
					mc->cnt_getattr,
					mc->cnt_open, 
					mc->cnt_close,
					mc->cnt_readdir,
					mc->cnt_read,
					mc->cnt_create,
					mc->cnt_unlink,
					mc->cnt_rename,
					mc->cnt_mkdir,
					mc->cnt_rmdir,
					mc->cnt_utimens,
					mc->cnt_write,
					mc->cnt_truncate,
					(long)(ios/interv),
					(float)((wc == 0)?wb*1.0:wb*1.0/wc),
					(long)ios,
					(float)(c_bytes == 0? 0.0:(100.0 - (o_bytes*100.0)/c_bytes)),
					hz_bytes2string(obuf, sizeof(obuf), o_bytes), 
					hz_bytes2string(cbuf, sizeof(cbuf), c_bytes)
					));
	}
	_ATOMIC_SUB(mc->cnt_getattr, mc->cnt_getattr);
	_ATOMIC_SUB(mc->cnt_open, mc->cnt_open);
	_ATOMIC_SUB(mc->cnt_close, mc->cnt_close);
	_ATOMIC_SUB(mc->cnt_readdir, mc->cnt_readdir);
	_ATOMIC_SUB(mc->cnt_read, mc->cnt_read);
	_ATOMIC_SUB(mc->cnt_create, mc->cnt_create);
	_ATOMIC_SUB(mc->cnt_unlink, mc->cnt_unlink);
	_ATOMIC_SUB(mc->cnt_rename, mc->cnt_rename);
	_ATOMIC_SUB(mc->cnt_mkdir, mc->cnt_mkdir);
	_ATOMIC_SUB(mc->cnt_rmdir, mc->cnt_rmdir);
	_ATOMIC_SUB(mc->cnt_utimens, mc->cnt_utimens);
	_ATOMIC_SUB(mc->cnt_write, mc->cnt_write);
	_ATOMIC_SUB(mc->cnt_truncate, mc->cnt_truncate);
	_ATOMIC_SUB(mc->cnt_mknod, mc->cnt_mknod);
	_ATOMIC_SUB(g__write_bytes, g__write_bytes);
	_ATOMIC_SUB(g__write_count, g__write_count);
	_ATOMIC_SUB(mc->cnt_origin_bytes, o_bytes);
	_ATOMIC_SUB(mc->cnt_client_bytes, c_bytes);
	if (mc->online)  {
		bt_set_timer(__timer_wheel_base,  &mc->t_activity_monitor, g__cm_interval/* 60 sec*/, nvm_activity_monitor, (void *)mc);
	}
}
#endif
static void
nvm_key_free(void *d)
{
	XFREE(d);
}

/*
 * NOTICE!!!) 이 함수는 s__volume_dictionary 테이블이 
 * free될 때 등록되어있는(사용중인) 모든 볼륨에 대해서 호출됨
 * 
 * Shutdown 중이므로 함수의 실행은 동기식으로 이루어져야함
 *
 */

#if NOT_USED
static void
nvm_free_volume_ht(void *d)
{
	nvm_destroy_internal(d);
}
#endif
int
nvm_init()
{
	glru_operation_map_t 			pc_map = {
		nvm_pc_allocate,
		nvm_pc_free,
		nvm_pc_reset,
		nvm_pc_fill_key,
		nvm_pc_lookup,
		nvm_pc_enroll,
		nvm_pc_unroll,
		nvm_pc_isidle,
		NULL,
		nvm_pc_dump
	};
	
	/* CHG 2018-07-04 huibong 불필요 사항 정리 및 return 구문 추가 (#32230) */

#ifdef PATHLOCK_USE_RWLOCK
	TRACE((T_INFO, "RWLOCK enabled for path-lock\n"));
#endif


#if defined(NC_MEASURE_PATHLOCK)
//	{	
//		int	e;
//		DEBUG_ASSERT ((e = pthread_spin_init(&s__pls_lock, PTHREAD_PROCESS_PRIVATE)) == 0);
//	}
#endif


	if (!s__volume_dictionary) {
		/*
		 * 볼륨 context삭제는 수작업으로 진행
		 */
		s__volume_dictionary = (u_hash_table_t *)u_ht_table_new_withlock(&s__volume_dictionary_lock, 
																		 path_hash, 
																		 path_equal, 
																		 nvm_key_free, 
																		 NULL); 
		u_ht_set_alloc_category(s__volume_dictionary, AI_VOLUME);

	}
#ifndef NC_RELEASE_BUILD
	g__path_cache = glru_init(g__pl_poolsize, &pc_map, "path-lock-cache");
#else
	g__path_cache = glru_init(g__pl_poolsize, &pc_map, "path-lock-cache");
#endif
	TRACE((T_INODE, "path-lock unit size=%ld(max = %d)\n", sizeof(nc_path_lock_t), g__pl_poolsize));

	if (!s__reserved_vol) {
		int	on = U_TRUE;
		s__reserved_vol = nvm_create_nolock(NULL, "@meta_volume", NULL, 0, U_TRUE /*ignore error*/);
		nvm_ioctl(s__reserved_vol, NC_IOCTL_PRESERVE_CASE, &on, sizeof(on));
	}

	return 0;
}
static int 
nvm_ioctl_notifier(struct cfs_origin_driver *drv, int cmd, void *val, int len, struct nc_volume_context *mc)
{
	int		iv;
	
	switch (cmd) {
		case NC_IOCTL_PRESERVE_CASE:
			 iv = mc->preservecase;
			 mc->preservecase = (*(int *)val != 0);
			 if (iv != mc->preservecase) {
				TRACE((T_INFO, "VOLUME[%s] - volume case sentivity changed to '%s'\n",
					mc->signature, (mc->preservecase?"case preserved":"case ignored")));
			}
			break;
		default:
			break;
	}
	return 0;
	
}
static int
nvm_make_online(nc_volume_context_t *volume, char *drvname, nc_origin_info_t *oi, int oicnt)
{
	cfs_origin_driver_t			*drv;
	int							r;
	int							mr = 0;

	
	
	drv = cfs_create_driver_instance( drvname, volume->signature);
	if (!drv) {
		TRACE((T_ERROR, "requested driver class '%s' - not loaded\n", drvname));
		
		return -1;
	}
	volume->osd 	= drv;

	r = cfs_bind_context(volume, (char *)volume->signature, oi, oicnt);
	if (r <= 0) {
		TRACE((T_ERROR, "VOLUME[%s] - binding origin_info to the driver instance failed\n", volume->signature));
		
		return -1;
	}

	/*
	 *
	 * 기본 콜백 설정
	 *
	 */
	cfs_ioctl(volume, NC_IOCTL_SET_IO_VALIDATOR, &nvm_check_if_same_object, sizeof(void *));
	cfs_ioctl(volume, NC_IOCTL_SET_LAZY_PROPERTY_UPDATE, &nvm_lazy_property_updater, sizeof(void *));
	cfs_ioctl(volume, NC_IOCTL_SET_ORIGIN_PATH_CHANGE_CALLBACK, &nvm_origin_path_change_proc, sizeof(void *));
	cfs_set_notifier(drv, nvm_ioctl_notifier, volume);

    /*
	 * 12.6 by weon
	 */
	mr = g__readahead_MB * 1024*1024;
	cfs_ioctl(volume, NC_IOCTL_SET_DEFAULT_READ_SIZE, &mr, sizeof(int));



#ifdef USE_VM_STAT
	snprintf(timer_nam, sizeof(timer_nam)-1, "activity_monitor:%s", volume->signature);
	bt_init_timer(&volume->t_activity_monitor, timer_nam, U_FALSE);
	bt_set_timer(__timer_wheel_base,  &volume->t_activity_monitor, g__cm_interval/* 60 sec*/, nvm_activity_monitor, (void *)volume);
#endif	

	return 0;
}
nc_volume_context_t * 
nvm_create( char 				*drvname,  
			char 				*signature,
			nc_origin_info_t 	*oi,
			int 				oicnt)
{
	nc_volume_context_t		*volume = NULL;



	pthread_mutex_lock(&s__volume_dictionary_lock);

	volume =  nvm_create_nolock(drvname, signature, oi, oicnt, U_FALSE);

	pthread_mutex_unlock(&s__volume_dictionary_lock);

	return volume;
}


static nc_volume_context_t * 
nvm_create_nolock(	char 				*drvname,  
					char 				*signature,
					nc_origin_info_t 	*oi,
					int 				oicnt,
					int					ignore_error)
{
	nc_volume_context_t		*volume = NULL;
	int						need_destroy = U_TRUE;



	/*
	 * solproxy에서 볼륨 존재 체크 안하고 생성 요청들어오는 경우
	 * 존재함
	 * 이미 대상 볼륨이 존재하는 경우 생성 실패 return(nc_errno=EEXIST)
	 */

	if ((volume = nvm_lookup_volume(signature)) != NULL) {
		TRACE((T_WARN, "Volume[%s] - exist\n", signature));
		__nc_errno = EEXIST;
		return NULL;
	}
	
	/*
	 * allocate volume context 
	 * s__volume_pool에 아무것도 없거나, 또는 있더라도 10분 이내로
	 * pool에 등록되었다면 사용하지 않음
	 */
    pthread_mutex_lock(&s__volume_dictionary_lock);
    volume = (nc_volume_context_t *)link_get_head_noremove(&s__volume_pool);
	if (volume == NULL || (nc_cached_clock() - volume->pooledtime) < 10*60) {
		volume = (nc_volume_context_t *)XMALLOC(sizeof(nc_volume_context_t), AI_VOLUME);
		nc_ulock_init(&volume->lock);
	}
	else {
		/*
		 * get & remove from s__volume_pool(lock필드 부분까지 초기화 제외)
		 */
		volume = (nc_volume_context_t *)link_get_head(&s__volume_pool);

		memset((char *)&volume->__barrier__, 
				0, 
				sizeof(nc_volume_context_t) - offsetof(nc_volume_context_t, __barrier__) 
				);
	}
    pthread_mutex_unlock(&s__volume_dictionary_lock);


	/*
	 * initialization
	 */
#if 1
	nc_ulock_init(&volume->lock);
#else
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&volume->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);
#endif

	strcpy(volume->signature, signature);



	/*
	 * caching content version 정보 load if exist
	 */
	volume->version	= mdb_read_volumes_version(signature);
	if (volume->version == 0 || volume->version == VOLUME_VERSION_NULL) {
		volume->version = 1;
	}

	

	volume->magic				= NC_MAGIC_KEY;
	volume->refcnt				= 0;
	volume->writeback_blocks	= 4; /* not used ?? */
	volume->preservecase		= 0; /* default, case-ignore */

#ifdef USE_VM_STAT
	volume->cnt_getattr			= 0; /* # of calls of driver->getattr */
	volume->cnt_read			= 0; /* # of calls of driver->read */
	volume->cnt_write			= 0; /* # of calls of driver->write */
	volume->cnt_readdir			= 0; /* # of calls of driver->readdir */
	volume->cnt_mkdir			= 0; /* # of calls of driver->mkdir */
	volume->cnt_truncate		= 0; /* # of calls of driver->truncate */
	volume->cnt_rename			= 0; /* # of calls of driver->rename */
	volume->cnt_unlink			= 0; /* # of calls of driver->unlink */
	volume->cnt_rmdir			= 0; /* # of calls of driver->rmdir */
	volume->cnt_utimens			= 0; /* # of calls of driver->utimens */
	volume->cnt_utimens			= 0; /* # of calls of driver->utimens */
	volume->cnt_open			= 0; /* # of calls of driver->open */
	volume->cnt_create			= 0; /* # of calls of driver->create */
	volume->cnt_close			= 0; /* # of calls of driver->close */
	volume->cnt_mknod			= 0; /* # of calls of driver->mknod */
#endif


	volume->fc_policy			= 0;
	volume->adaptivereadahead	= 0;



	/*
	 * 신선도 검사용 변수 초기화
	 */
	INIT_LIST(&volume->fc_graph);
	volume->fc_alwayscheck 	= 0;
	volume->fc_accepted[0] 	= 0;
	volume->fc_policy 		= 0;
	volume->to_ncread		= DEFAULT_NCREAD_TIMEOUT;
	nvm_parse_freshness_policy(volume, s__default_fc_policy);

	/*
	 * 캐싱 유효시간 설정
	 */
	volume->p_ttl			= __positive_TTL; /* global default value */
	volume->n_ttl			= __negative_TTL; /* global default value */

	/*
	 * qpath_hash : key= inode->q_id, data = inode itself
	 * uuid_hash :  key= inode->uuid, data = inode itself
	 */
	volume->object_tbl		= u_ht_table_new_withlock_x(	NULL,
															(void *)volume, 
															nvm_hash_inode, 
															nvm_hash_equal, 
															NULL, 
															NULL /*fc_qpath_hash_destructor*/);
	u_ht_table_lock_mode(volume->object_tbl, U_FALSE);
	DEBUG_ASSERT(volume->object_tbl != NULL);

#ifdef ENABLE_UUID_LOOKUP
	volume->uuid_hash		= u_ht_table_new_withlock_x(	NULL,
															(void *)volume, 
															nvm_hash_uuid,  
															nvm_hash_uuid_equal, 
															NULL, 
															NULL /*fc_qpath_hash_destructor*/);
	u_ht_set_alloc_category(volume->uuid_hash, AI_VOLUME);
	u_ht_table_lock_mode(volume->uuid_hash, U_FALSE);
#endif

	volume->PL_tbl			= u_ht_table_new_withlock_x(	glru_get_lock(g__path_cache), 
															(void *)volume, 
															nvm_hash_inode, 
															nvm_hash_equal, 
															NULL, 
															NULL );

	volume->readable		= 0;
	volume->writable		= 0;
	volume->freed			= 0;

	/*
	 * hash테이블에서 할당하는 heap메모리의 카테고리 설정
	 */
	u_ht_set_alloc_category(volume->PL_tbl, AI_VOLUME);
	u_ht_set_alloc_category(volume->object_tbl, AI_VOLUME);

	/*
	 * driver instance를 생성하고 연동
	 */
	if ((oicnt > 0) && (nvm_make_online(volume, drvname, oi, oicnt) >=0)) { 
		char 	*vk = NULL;


		DEBUG_ASSERT(volume->osd != NULL);
		volume->online		= U_TRUE;
		volume->readable	= 1; /* default: active */
	
		/*
		 * 생성된 볼륨을 향후 volume signature로 검색할 수 있도록 
		 * volume dict에 등록
		 */

		vk = XSTRDUP(signature, AI_VOLUME);
		if (!u_ht_insert(s__volume_dictionary, vk,  (void *)volume, U_FALSE)) {
			nc_volume_context_t *dv = nvm_lookup_volume(volume->signature);
			TRACE((T_ERROR, "FATAL: inserting volume[%s] failed, duplicated?(dup=[%p:%s])\n", vk, dv, dv->signature));
			TRAP;
		}

		TRACE((T_INODE, "Volume[%s] : created with caching version, %lu \n", signature, volume->version));
		need_destroy = U_FALSE;
	}
	if (ignore_error == 0) {
		if (need_destroy) {
			u_ht_remove(s__volume_dictionary, volume->signature);
			nvm_destroy_internal(volume); /* sync-type nvm_destroy() */
			volume = NULL;
		}
	}
	
	return volume;
}


struct orphanize_info {
	nc_uint32_t				cv;
	nc_volume_context_t 	*volume;
	int 					no; /* # of orphanized inodes */
	int						busyfound;
};

/*
 * inode를 볼륨에서 떼어버림
 * 		***** 필요하면 disk swapout 해야함
 */
static int
nvm_orphanize_inodes_cb(void *key, void *value, void *ud)
{
	struct orphanize_info 	*poi 	= (struct orphanize_info *)ud;
	fc_inode_t 				*inode 		= (fc_inode_t *)value;


	DEBUG_ASSERT(poi->cv == 0x5AA5A55A);
	DEBUG_ASSERT_FILE(inode, INODE_GET_REF(inode) == 0);
	if (INODE_BUSY(inode)) {
		poi->busyfound = 1;
		return HT_STOP;
	}

	/*
	 * 객체 정보를 필요하면 저장
	 */
	dm_swapout_inode_nolock(inode);

	dm_detach_driver(inode, U_FALSE);
	inode->volume = NULL;
	poi->no = poi->no + 1;
	if (poi->no < 0) {
		TRAP;
	}


	return HT_CONTINUE;
}

/*
 *
 *	inode들이 victim 선택될 수 있도록 만들어 둠
 * 	free된 메모리가 참조되어 SEGV발생하지 않도록 하는 부분도 포함
 *
 */
static int
nvm_orphanize_inodes(nc_volume_context_t *volume)
{
	struct orphanize_info		oi;	
	int							attempts = 0;
#define	DO_ORPHANIZE { \
	oi.busyfound = 0; \
	attempts++; \
	IC_LOCK(CLFU_LM_EXCLUSIVE); /* table에서 매칭 엔트리 삭제*/ \
	u_ht_foreach(volume->object_tbl, nvm_orphanize_inodes_cb, &oi); \
	IC_UNLOCK; \
	}

	oi.cv 		= 0x5AA5A55A;
	oi.volume 	= volume;
	oi.no		= 0;

	DO_ORPHANIZE;
	while  (oi.busyfound != 0) {
		bt_msleep(10);
		DO_ORPHANIZE;
	} 
	TRACE((T_MONITOR, "VOLUME[%s] - %d inodes orphanized(%d times tried)\n", volume->signature, oi.no, attempts));
	return 0;
}
u_boolean_t 
nvm_free_path_lock(void *key, void *value, void *userdata)
{
	nc_path_lock_t 	*pl = (nc_path_lock_t *)value;
	if (pl && pl->path) {
		XFREE(pl->path);
	}
	return U_TRUE;
}
/*
 * This function make an existing online volume context to 
 * offline one.
 * WARN: THIS WOULD CAUSE VOLUME CONTEXT OVERRUN!
 */


int
nvm_destroy(nc_volume_context_t *volume, int bsync)
{
	nvm_dti_t			*dti = NULL;
	int					r = 0;


	pthread_mutex_lock(&s__volume_dictionary_lock);
#ifndef NC_RELEASE_DEBUG
	u_ht_remove(s__volume_dictionary, volume->signature);
#else
	if (!u_ht_remove(s__volume_dictionary, volume->signature)) {
		TRACE((T_ERROR, "Volume[%s] - not registered\n", volume->signature));
	}
	DEBUG_ASSERT(nvm_lookup_volume(volume->signature) == NULL);
#endif
	pthread_mutex_unlock(&s__volume_dictionary_lock);

	volume->online 	= U_FALSE;

	if (bsync) {

		r = 0;
		nvm_destroy_internal(volume);

	}
	else {

		/*
		 * dti는 thread id를 받기 전에 미리 dti list에 등록
		 */
		dti = XMALLOC(sizeof(nvm_dti_t), AI_ETC);
		pthread_mutex_lock(&s__nvm_lock);
		/*
		 * join 대상등록
		 */
		link_append(&s__nvm_dthread_list, dti, &dti->node);
		pthread_mutex_unlock(&s__nvm_lock);
	
		pthread_create(&dti->tid, NULL, nvm_destroy_internal, volume);
		r = 0;
	}

	return r;
	
}
static void *
nvm_destroy_internal(void * u)
{
	nc_volume_context_t		*volume = u;
	cfs_origin_driver_t 	*drv = volume->osd;
	time_t 					timeout = time(NULL)  + 10;
	int						ni = 0;
	nc_freshcheck_entry_t	*fe = NULL;


	TRACE((T_INODE, "VOLUME[%s] - destroying ...\n", volume->signature));
	DEBUG_ASSERT(volume->magic == NC_MAGIC_KEY);

	drv->shutdown = 1;


#ifdef USE_VM_STAT
	if (!bt_is_valid(&volume->t_activity_monitor)) {
		TRACE((T_INFO, "VOLUME[%s] - got invalid timer, destroying skipped\n", 
					volume->signature));
	}
	else {
		while (bt_destroy_timer(__timer_wheel_base, &volume->t_activity_monitor) < 0) {
			TRACE((T_INFO, "VOLUME[%s] : waiting for timer idle...\n", volume->signature));
			bt_msleep(1000);
		}
	}
#endif

	if  (GET_ATOMIC_VAL(volume->refcnt) > 0 ) {
		TRACE((T_INFO, "VOLUME[%s] : waiting for idle...[%d]\n", volume->signature, (int)GET_ATOMIC_VAL(volume->refcnt)));
		timeout = nc_cached_clock() + 30;
		do {

			bt_msleep(10);

		} while ((GET_ATOMIC_VAL(volume->refcnt) > 0) && timeout >= nc_cached_clock());

		if (timeout < nc_cached_clock(NULL)) {
			int		n = 0;

			TRACE((T_ERROR, "VOLUME[%s] - volume context for driver instance, '%s' still busy, emergency closing, %d inodes scannned:refcnt=%d\n",
							volume->signature, 
							volume->osd->name, 
							n,
							volume->refcnt
							));
			n = nvm_for_all_inodes(volume, dm_forcely_close_inode, NULL);
		}
	}
	ni 	= nvm_orphanize_inodes(volume);
	u_ht_table_free(volume->object_tbl);
	volume->object_tbl = NULL;
	XFREE(volume->zpolicy);
	nvm_orphanize_PL(volume);

	/*
	 * driver 연동 삭제
	 */

	// CHG 2019-06-12 huibong 잘못된 로깅 내역 수정 (#32701)
	TRACE( (T_INODE, "Volume[%s]: unbind to the driver[%s] complete.\n", volume->signature, drv->signature) );
	cfs_unbind_context( volume );
	cfs_destroy_driver_instance( drv );


#ifdef ENABLE_UUID_LOOKUP
	u_ht_table_free(volume->uuid_hash);
#endif
	u_ht_table_free(volume->PL_tbl);



	while ((fe = link_get_head(&volume->fc_graph)) != NULL) {
		XFREE(fe);
	}

	volume->freed 	= 1; /* 사실 의미 없음*/
	volume->magic 	= 0; 

    pthread_mutex_lock(&s__volume_dictionary_lock);
	volume->pooledtime = nc_cached_clock();
	link_append(&s__volume_pool, volume, &volume->node);
	pthread_mutex_unlock(&s__volume_dictionary_lock);



	return 0;
}
#if NOT_USED
static int
nvm_destroy_path_lock(void *d)
{
	nc_path_lock_t 	*pl = (nc_path_lock_t *)d;
#ifdef PATHLOCK_USE_RWLOCK
	pthread_rwlock_destroy(&pl->lock);
#else
	pthread_mutex_destroy(&pl->lock);
#endif
	XFREE(pl->path);
	XFREE(d);
	return 0;
}
#endif
void
nvm_free_all()
{
	int 			n = 0;
	nvm_dti_t		*dti = NULL;

	n = glru_destroy(g__path_cache);
	TRACE((T_INODE, "Path-lock cache destroyed - %d entries\n", n));
#if 0
	/*
	 * 당분간 사용안함
	 * solproxy의 virtual host관리쪽에서 모든 볼륨에 대해서 삭제처리하는중
	 *
	 */

	u_ht_table_free(s__volume_dictionary);
#endif

	/*
	 * dti 정보 free안함
	 */
	TRACE((T_INFO, "Path-lock cache destroyed..., waiting %d threads\n", link_count(&s__nvm_dthread_list, U_TRUE)));
	pthread_mutex_lock(&s__nvm_lock);
	dti = link_get_head(&s__nvm_dthread_list);
	while (dti) { 
		n++;
		pthread_join(dti->tid, NULL);
		XFREE(dti);
		
		dti = link_get_head(&s__nvm_dthread_list);
	}
	pthread_mutex_unlock(&s__nvm_lock);
	TRACE((T_INODE, "%d volumes destroying threads joined ok\n", n));

}
int
nvm_is_online(nc_volume_context_t * volume, int flag)
{
	int ionline = 0;


	if (!volume)  {
		TRACE((T_INFO, "VOLUME[%s] - NOT AVAILABLE\n", volume->signature));
		ionline = 0;
		goto L_offline;
	}
	else {
		ionline = volume->online;
		if ((flag & NCOF_READABLE) != 0) 
			ionline	= ionline && cfs_is_online(volume->osd, NCOF_READABLE);
		else
			ionline	= ionline && cfs_is_online(volume->osd, NCOF_WRITABLE);
	}
L_offline:
	return ionline;
}
int
nvm_is_online2(nc_volume_context_t * volume)
{
	return (volume && volume->online);
}
struct tag_volume_context_fi {
	char				*qr_path;
	nc_volume_context_t	*mc;
};
#if NOT_USED
static  int
nvm_find_volume_context(void *key, void *value, void *ud)
{
	struct tag_volume_context_fi	*fi = (struct tag_volume_context_fi *)ud;
	nc_volume_context_t		*volume = (nc_volume_context_t *)value;
	int						n;

	n = strlen(fi->mc->signature);
	/* match only the signature part from qr_path */
	if (strncmp(fi->qr_path, volume->signature, n) == 0) {
		fi->mc = volume;
		return HT_STOP;
	}
	return HT_CONTINUE;
}
#endif
nc_volume_context_t *
nvm_lookup_volume(char *signature)
{
	nc_volume_context_t *m = NULL;

	m = (nc_volume_context_t *)u_ht_lookup(s__volume_dictionary, (void *)signature);
	return m;
}
int
nvm_unregister_cache_object_nolock(nc_volume_context_t *volume, fc_inode_t *inode)
{
	u_boolean_t 	br_1; /*, br_2;*/
	int				r = 0;
	char			dbuf[8192];
	
	

	br_1 = ic_unregister_cache_object_nolock(volume, inode);

	if (!br_1 ) {
		TRACE((T_INODE, "INODE {%s} - failed to remove(not found)\n", dm_dump_lv1(dbuf, sizeof(dbuf), inode)));
		r = -1;
	}
	else {
		TRACE((T_INODE, "INODE {%s} - removed OK\n", dm_dump_lv1(dbuf, sizeof(dbuf), inode)));
	}
	
	return r;
}
fc_inode_t *
nvm_register_cache_object_nolock(nc_volume_context_t *volume, fc_inode_t *f)
{
	fc_inode_t		*lookupi = NULL;
	
	lookupi = ic_register_cache_object_nolock(volume, f);
	return lookupi;
}
static unsigned long 
nvm_hash_inode(void *vstr, void *ud)
{
	char 			*str = (char *)vstr;
	nc_volume_context_t	*volume = (nc_volume_context_t *)ud;
	register int 	pc;
	unsigned long	hv;
	int 			wrdlen = strlen(str);
	register char 	*a_str;
	nc_uint32_t FNV1A_Hash_Yoshimura(char *str, nc_uint32_t wrdlen);
	pc = volume->preservecase;
	if (!pc) {
		a_str = alloca(wrdlen+1) ;
		strcpy(a_str, str);
		str = a_str;
		while (*str) { *str = tolower(*str);str++;}
		str = a_str;
	}
	hv = FNV1A_Hash_Yoshimura((char *)str, wrdlen);
	return hv;
	
}
u_boolean_t
nvm_hash_equal(void *v1, void *v2, void *ud)
{
	int					r;
	nc_volume_context_t	*volume = (nc_volume_context_t *)ud;
	r =  volume->preservecase?strcmp((char *)v1, (char *)v2):strcasecmp((char *)v1, (char *)v2);
	if (r == 0) {
		TRACE((0, "VOLUME[%s]/C[%d]:: ['%s', '%s'] = %d\n", 
				volume->signature, volume->preservecase, 
				(char *)v1, (char *)v2, r));
	}
	return r == 0;
}
int
nvm_ioctl(nc_volume_context_t *mc, int cmd, void *val, int vallen)
{
	int						result = 0;
	int 					tval;

	switch (cmd) {
		case NC_IOCTL_SET_LAZY_PROPERTY_UPDATE:
			/*
			 * 이 ioctl 설정은, 객체가 동적이고, 크기가 호출될 때마다 변하는 경우
			 * 설정되어야한다.
			 * NC_IOCTL_SET_LAZY_XXX가 명령으로 내려오는 경우 무조건 설정으로 간주하고,
			 * driver에게 callback을 등록한다
			 */
			result = cfs_ioctl(mc, cmd, nvm_lazy_property_updater, sizeof(cfs_io_validator_callback_t));
			break;

		case NC_IOCTL_NEGATIVE_TTL:
			tval = *(int *)val;
			if (tval > 0) {
				mc->n_ttl			= tval;
				TRACE((T_INFO, "volume[%s] : volume's negative TTL changed to %lld\n", 
								mc->signature, mc->n_ttl));
			}
			else {
				TRACE((T_ERROR, "volume[%s] : negative TTL value should be a positive integter in sec.\n", 
								mc->signature, mc->n_ttl));
			}
			break;
		case NC_IOCTL_POSITIVE_TTL:
			tval = *(int *)val;
			if (tval > 0) {
				mc->p_ttl			= tval;
				TRACE((T_INFO, "volume[%s] : volume's positive TTL changed to %lld\n", 
								mc->signature, mc->p_ttl));
			}
			else {
				TRACE((T_ERROR, "volume[%s] : positive TTL value should be a positive integter in sec.\n", 
								mc->signature, mc->p_ttl));
			}
			break;
		case NC_IOCTL_PRESERVE_CASE: {
				tval = *(int *)val;
				if (tval != mc->preservecase) {
					if  (tval) {
						TRACE((T_DEBUG, "volume[%s] : context changed to preserve the file name case\n", 
								mc->signature));
					}
					else {
						TRACE((T_DEBUG, "volume[%s] : context changed to ignore the file name case\n",
								mc->signature));
					}
					mc->preservecase = tval;
				}
			}
			break;
		case NC_IOCTL_SET_VALIDATOR:
			break;
		case NC_IOCTL_WRITEBACK_BLOCKS: {
			int 		tval;
				tval = *(int *)val;
				if (mc->writeback_blocks != tval) {
					if  (tval > 0 && tval <= 256) {
						mc->writeback_blocks = tval;
					}
					else {
						TRACE((T_ERROR, "volume[%s] : invalid value, %d for writeback blocks, ignored \n", 
										mc->signature, tval));
					}
				}
				TRACE((T_DEBUG, "volume[%s] : writeback blocks for driver[%s] is %d\n", mc->signature, mc->osd->name, mc->writeback_blocks));
			}
			break;
		case NC_IOCTL_ADAPTIVE_READAHEAD: {
			int		tval;
				tval = *(int *)val;
				if (tval) {
					mc->adaptivereadahead =  1;
				}
				else {
					mc->adaptivereadahead =  0;
				}
				TRACE((T_INFO, "volume[%s] : adaptive readahead is '%s'\n", 
								mc->signature, (mc->adaptivereadahead?"enabled":"disabled")));
			}
			break;
		case NC_IOCTL_FRESHNESS_CHECK: 
			/*
			 * upgrade to x-lock
			 */
			nc_ulock_upgrade(&mc->lock);
			if (nvm_parse_freshness_policy(mc, (char *)val) < 0) {
				TRACE((T_ERROR, "VOLUME[%s] - policy {%s} invalid\n", mc->signature,val));
				result = -EINVAL;
			}
			else {
				TRACE((T_INFO, "VOLUME[%s] - policy {%s} accepted\n", mc->signature,val));
				result = 0;
			}
			break;
		case NC_IOCTL_CACHE_MONITOR:
			/* val 음 함수임, void proc(void *cb, hit-type) */
			mc->monitor = val;
			break;
		case NC_IOCTL_SET_LB_PARENT_POLICY: 
		case NC_IOCTL_SET_LB_ORIGIN_POLICY: {
				result = cfs_ioctl(mc, cmd, val, vallen);
			}
			break;
		case NC_IOCTL_WEBDAV_ALWAYS_LOWER: {
#if 0
				int		tval;
				tval = *(int *)val;
				if (mc->alwayslower != tval) {
					mc->alwayslower=tval;
				}
#else
				TRACE((T_INFO, "VOLUME[%s] - NC_IOCTL_WEBDAV_ALWAYS_LOWER deprecated!\n", mc->signature));
#endif
			}
			/*  브레이크 없이 아래 default 처리 */

		case NC_IOCTL_SET_NCREAD_TIMEOUT: {
				int		tval;
				tval = *(int *)val;
				if (tval <= 0 || tval >= 3600) {
					TRACE((T_ERROR, "VOLUME[%s] - NC_IOCTL_SET_NCREAD_TIMEOUT : invalid parameter value '%d'(current:%d)\n", mc->signature, tval, mc->to_ncread));
				}
				else {
					mc->to_ncread		= tval;
					TRACE((T_TRACE, "VOLUME[%s] - timeof of nc_read() set to %d secs\n", mc->signature, tval));
				}
			}
			break;
		default: {
				result = cfs_ioctl(mc, cmd, val, vallen);
			}
			break;
	}
	return result;
}


fc_inode_t *
nvm_lookup_inode(nc_volume_context_t *volume, int lookup_type, char *key, int bremove, int makeref, int markbusy, int *busy, char *file, int line)
{
	fc_inode_t	*lookup = NULL;
	void 		* (*remove_op)(u_hash_table_t *ht, void *key) = u_ht_lookup;


	DEBUG_ASSERT(nvm_volume_valid(volume));

	if (bremove) {
		remove_op = u_ht_remove_II;
	}
#ifdef ENABLE_UUID_LOOKUP
	if (lookup_type == DM_LOOKUP_WITH_KEY)
		lookup = (fc_inode_t *)(*remove_op)(volume->object_tbl, (void *)key);
	else
		lookup = (fc_inode_t *)(*remove_op)(volume->uuid_hash, (void *)key);
#else
	lookup = (fc_inode_t *)(*remove_op)(volume->object_tbl, (void *)key);
#endif

	if (lookup) {
		if (markbusy) {
			if (ic_is_busy(lookup))  {
				if (busy) *busy  = 1;
				lookup = NULL;
				goto L_lookup_done;
			}
			ic_set_busy(lookup, U_TRUE, file, line);
		}

		if (bremove) 
			clfu_remove(g__inode_cache, &lookup->node);
		

		if (makeref) {
   			INODE_REF(lookup); 
		}

	}
L_lookup_done:
	TRACE((T_INODE, 			"Volume[%s]/Key[%s] - %s(INODE[%d]) %s(makeref=%d,markbusy=%d %s ) \n", 
								volume->signature, 
								key,
								(bremove?"REMOVE":"LOOKUP"),
								(lookup?lookup->uid:-1),
								(lookup?"found":"not found"),
								makeref,
								markbusy,
								(char *)((busy&&*busy)?"BUSY":"")
								));
	return lookup;
}

int
nvm_lock(nc_volume_context_t *mc, int exclusive, char *f, int l)
{
/*
 * lock을 살리면 지연발생
 */
#ifdef NC_VOLUME_LOCK 
	int		r;

	r = (exclusive?nc_ulock_wrlock(&mc->lock):nc_ulock_rdlock(&mc->lock));
	return (r == 0);

#else
	return 1;
#endif
}
void
nvm_unlock(nc_volume_context_t *mc, char *f, int l)
{
#ifdef NC_VOLUME_LOCK 
	if (mc) {
		nc_ulock_unlock(&mc->lock);
		//TRACE((T_WARN, "[%s] UNLOCK %d@%s\n", mc->signature, l, f));
	}
#else
	return ;
#endif
}
/*
 * volume context, volume에 속한 모든 inode에 대해서 cbfunc(key, value, cb)를 호출 실행
 * cbfunc : callback function
 * cb     : callback data
 */
typedef struct nvm_operation_ctx {
	int (*cbfunc)(void *, void *, void *);
	void  *cb;
} nvm_operation_ctx_t;



static int
nvm_do_operation_wrapper(void *a, void *b, void *ud)
{
	nvm_operation_ctx_t *poctx = (nvm_operation_ctx_t *)ud;

	return (poctx->cbfunc(a, b, poctx->cb) >= 0)? HT_CONTINUE:HT_STOP;
}
int
nvm_do_operation(nc_volume_context_t *volume, int (*cbfunc)(void *, void *, void*), void *cb)
{
	struct nvm_operation_ctx 	octx = {NULL, NULL};

	octx.cbfunc 	= cbfunc;
	octx.cb 		= cb;

	u_ht_foreach(volume->object_tbl, nvm_do_operation_wrapper, &octx);
	return 0;
}
int
nvm_space_avail(nc_volume_context_t *volume, char *path)
{
	return U_TRUE;
}
static int
nvm_parse_freshness_policy(nc_volume_context_t *volume, char *policy)
{
	struct {
		char 					name[16];
		nc_uint16_t 			val;
		nc_validation_step_t	step;
	} policy_dict [] = {
		{"mtime", 			VOL_FC_MTIME, 			nvm_check_freshness_by_iftag},
		{"size", 			VOL_FC_SIZE, 			nvm_check_freshness_by_size},
		{"devid", 			VOL_FC_DEVID, 			nvm_check_freshness_by_iftag},
		{"alwaysexpire", 	VOL_FC_ALWAYSEXPIRE, 	nvm_check_freshness_by_always_expire},
		{"", 0}
	};
	char 					*t_policy = NULL; 
	char 					*token = NULL;
	char 					*last = NULL;
	nc_uint16_t				policy_tag = 0;
	int 					i;
	int 					error = 0;
	nc_uint16_t				npolicy = 0;
	nc_freshcheck_entry_t	*entry = NULL;
	char 					*pa = volume->fc_accepted;
	int 					n = 0;


	if (!policy) return -1;

	if (link_count(&volume->fc_graph, U_TRUE) > 0) {
		/*
		 * 기존 설정 삭제
		 */
		while ((entry = link_get_head(&volume->fc_graph)) != NULL) {
			XFREE(entry);
		}
	}
	INIT_LIST(&volume->fc_graph);
	volume->fc_alwayscheck	= 0;
	XFREE(volume->zpolicy);
	/*
	 * prolog add
	 */
	entry 				= XMALLOC(sizeof(nc_freshcheck_entry_t), AI_ETC);
	strcpy(entry->fcname, "Prolog");
	n = sprintf(pa, "prolog");pa += n;

	entry->fcid 		= VOL_FC_PROLOG;
	entry->step 		= nvm_check_freshness_prolog;
	INIT_NODE(&entry->node);
	link_append(&volume->fc_graph, entry, &entry->node);
	volume->fc_alwayscheck++; /* prolog함수는 항상 check됨 */

	volume->alwaysexpire = 0; /* 일단 reset */


	volume->zpolicy = XSTRDUP(policy, AI_VOLUME);

	t_policy = (char *)alloca(strlen(policy) + 1);
	strcpy(t_policy, policy);

	token = strtok_r(t_policy, ",&", &last);
	while (token) {
		token = trim_string(token);
		if (token && *token) {
			policy_tag = 0;
			i = 0;
			while (policy_dict[i].name[0]) {
				if (strcasecmp(policy_dict[i].name, token) == 0) {
					policy_tag = policy_dict[i].val;
					entry = XMALLOC(sizeof(nc_freshcheck_entry_t), AI_ETC);
					entry->fcid = policy_dict[i].val;
					entry->step = policy_dict[i].step;
					strcpy(entry->fcname, policy_dict[i].name);
					n = sprintf(pa, ",%s", policy_dict[i].name);pa += n;
					INIT_NODE(&entry->node);
					/*
					 * 먼저 나온 항목이 더상위 우선순위를 가짐
					 */
					link_append(&volume->fc_graph, entry, &entry->node);

					break;
				}
				if (strcasecmp(token, "alwaysexpire") == 0) {
					volume->alwaysexpire = 1;
				}
				i++;
			}
			if (policy_tag == 0) {
				TRACE((T_ERROR, "VOLUME[%s] - freshnesscheck policy, '%s'  unknown, skipped\n", volume->signature, token));
				error++;
			}
			else {
				npolicy |= policy_tag;
			}
		}
		token = strtok_r(NULL, ",&", &last); /* SEP=','|'&' 을 사용 */
	}
	if (npolicy != 0) {
		if((npolicy & VOL_FC_ALWAYSEXPIRE) && (npolicy != VOL_FC_ALWAYSEXPIRE)) {
			TRACE((T_ERROR, "VOLUME[%s] - freshness check policy, (%s' denied (should use without others).\n", volume->signature, "alwaysexpire"));
			error++;
		}
		else {
			volume->fc_policy = npolicy;
		}
	}
	TRACE((T_INFO, "Volume[%s] - freshness check policies are {%s}[total=%d](error=%d)\n", 
					volume->signature, 
					volume->fc_accepted, 
					link_count(&volume->fc_graph, U_TRUE), 
					error));
	return (error?-1:0);
}
/*
 * ir #24809 : 신선도 검사 flag 정리
 */
#if NOT_USED
static int
nvm_need_to_apply(nc_volume_context_t *volume, const nc_uint16_t policy)
{
	int 	applicable = 0;

	if (policy == VOL_FC_ALWAYSEXPIRE) {
		/*
		 * 배타적 단독 사용 flag
		 */

		applicable = (volume->fc_policy == policy);
	}
	else {
		/*
		 * 조합가능한 flag
		 */
		applicable = ((volume->fc_policy & policy) != 0) ;
	}
	return applicable;
}
#endif
/*
 * 필드 항목 별 신선도 검사 함수
 */
static int
nvm_check_freshness_by_iftag(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int *fresh, int *checked, nc_validation_result_t *reason, int fcid)
{

	int		res = FCG_CONTINUE;
	*checked  	= U_FALSE;
	*fresh 	 	= U_TRUE;
	*reason 	= NC_FRESH;
	if (stat->st_ifon) {
		/*
		 * IMS/INM으로 실행된 요청에 대한 응답
		 * 신선도 검사시는 여기서 멈춰야함
		 */
		*checked 	= U_TRUE;
		res  		= FCG_STOP;
		if (stat->st_originchanged) {
				/*
		 		 * 위에서 비교한것과 다른 이유로 원본에서 변경되었다고 알려옴
	        	 */
			*reason = NC_STALE_BY_ORIGIN_OTHER;
			*fresh 	= U_FALSE;
		}
		TRACE((T_INODE, "VOLUME[%s]/CKey[%s] - INODE[%u] %s(code=%d, st_originchanged=%d)\n",
						volume->signature, 
						inode->q_id, 
						inode->uid, 
						(*fresh? "fresh":"staled by origin's content change"),
						stat->st_origincode,
						stat->st_originchanged
						));
	}
	/*
	 * rangeable객체가 full-caching 상태가 아닌 경우 유효성 체크를 위해서 추가된 코드
	 */
	else if (fcid == VOL_FC_MTIME) {
		res = nvm_check_freshness_by_mtime(volume, inode, stat, fresh, checked, reason, fcid);
	}
	else if (fcid == VOL_FC_DEVID) {
		res = nvm_check_freshness_by_etag(volume, inode, stat, fresh, checked, reason, fcid);
	}

	return res;
}
static nc_validation_result_t
nvm_check_freshness_by_mtime(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int *fresh, int *checked, nc_validation_result_t *reason, int fcid)
{
	int 	res = FCG_CONTINUE;

	*checked 	= U_FALSE;
	*fresh 		= U_TRUE;
	*reason 	= NC_FRESH;

	if (stat->st_mtime != 0) {
		*checked = U_TRUE;
		/*
		 * 원본 응답이 valid한 mtime을 가지고 있음
		 */
		if (inode->mtime != stat->st_mtime) {
			/*
			 *
			 * 시간 비교는 최신상태 여부보다 원본과 다른지 같은지 비교하는게 정확
			 *
			 */
			TRACE((T_INODE, "VOLUME[%s]/CKey[%s] - INODE[%u] staled by the modification time\n", 
							volume->signature, 
							inode->q_id, 
							inode->uid, 
							stat->st_origincode));

			*reason 	= NC_STALE_BY_MTIME;
			*fresh 		= U_FALSE;
			res 		= FCG_STOP;
		}
	}
	return res;
}
static nc_validation_result_t
nvm_check_freshness_by_etag(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int *fresh, int *checked, nc_validation_result_t *reason, int fcid)
{
	int 	res = FCG_CONTINUE;

	*checked 	= U_FALSE;
	*fresh 		= U_TRUE;
	*reason 	= NC_FRESH;

	if (isprint(stat->st_devid[0])) {
		*checked = U_TRUE;
		/*
		 * 원본 응답이 valid한 mtime을 가지고 있음
		 */
		if (isprint(inode->devid[0]) == 0 || strcmp(inode->devid, stat->st_devid) != 0) {
			/*
			 *
			 * etag 불일치
			 *
			 */
			TRACE((T_INODE, "VOLUME[%s]/CKey[%s] - INODE[%u] staled by the etag[%s, %s]\n", 
							volume->signature, 
							inode->q_id, 
							inode->uid, 
							stat->st_devid,
							inode->devid
							));

			*reason 	= NC_STALE_BY_ETAG;
			*fresh 		= U_FALSE;
			res 		= FCG_STOP;
		}
	}
	return res;
}

static int
nvm_check_freshness_by_size(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int *fresh, int *checked, nc_validation_result_t *reason, int fcid)
{
	int 	res = FCG_CONTINUE;


	*checked 	= U_FALSE;
	*fresh 		= U_TRUE;
	*reason 	= NC_FRESH;

	if (inode->ob_sizeknown == 0) {
		*checked = U_TRUE;
		return res;
	}
	if ((stat->st_sizedecled != 0)  && (inode->ob_sizedecled != 0)) {
		*checked 	= U_TRUE;
		if (inode->ob_template)  {
			/*
		 	*  vary의 경우 응답 길이는 client요청에 따라 달라지므로,
		 	*  vary여부 정보만 간직한 template에겐 사이즈 비교가 의미 없음.
		 	*/ 
			*fresh 		= U_TRUE;
			*reason 	= NC_FRESH;
			TRACE((T_INODE, "VOLUME[%s]/CKey[%s] - INODE[%u] template, supposed to be fresh by size\n", volume->signature, inode->q_id, inode->uid));
		}
		else {
			/*
			 * 객체의 크기가 명확히 Content-Length 태그를 명시됨
			 */
			*fresh 		= (inode->size == stat->st_size);
			res 		= FCG_CONTINUE;
	
			if (!*fresh) {
				TRACE((T_INODE, "INODE[%u]/O[%s] - size mismatch (origin=%lld, caching=%lld){st_sd=%d,ob_sd=%d)\n", 
								inode->uid, 
								inode->q_path, 
								stat->st_size, 
								inode->size,
								stat->st_sizedecled,
								inode->ob_sizedecled
								));
				*reason = NC_STALE_BY_SIZE;
				res 	= FCG_STOP;
			}
		}
	}
	return res;
}

static int
nvm_check_freshness_by_always_expire(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int *fresh, int *checked, nc_validation_result_t *reason, int fcid)
{
	*checked 	= U_TRUE;
	*reason 	= NC_STALE_BY_ALWAYS;
	*fresh 		= U_FALSE;

	TRACE((T_INODE, "VOLUME[%s]/CKey[%s] - INODE[%u] Always-Expire set\n", volume->signature, inode->q_id, inode->uid));

	return FCG_STOP;
}
static int
nvm_check_freshness_prolog(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int *fresh, int *checked, nc_validation_result_t *reason, int fcid)
{
	*checked 	= U_TRUE;


	*fresh 		= U_TRUE;
	*reason 	= NC_FRESH;

	if (inode->ob_mustexpire) {
		TRACE((T_INODE, "VOLUME[%s]/CKey[%s] - INODE[%u] expired by MUST-EXPIRE\n", volume->signature, inode->q_id, inode->uid));
		*reason 	= NC_STALE_BY_ORIGIN_OTHER;
		*fresh 		= U_FALSE;
	}
	return FCG_CONTINUE;
}


/*
 *
 * 원본에 객에 대한 신선도 검사 query 요청 후 응답을 수신한 뒤 호출됨
 * 
 */
nc_validation_result_t
nvm_check_freshness_by_policy(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int skipalways, int origindown, nc_validation_result_t defval)
{

#if 1
	nc_validation_result_t  vr 		= NC_STALE_BY_ORIGIN_OTHER;
	int 					fresh 	= U_TRUE;
#else
	nc_validation_result_t 	vr 		= NC_FRESH;
	int 					fresh 	= U_TRUE;
#endif
	char					ibuf[2048];
	int						sc		= FCG_CONTINUE; /* stop or continue */
	int 					skip_count = 0; /* fresh-check step들이 적용되지 않고 skip된 갯수 */
	int 					fc_count = 0; 	/* 사용된 fresh-check step */
	int 					checked = 0;
	nc_freshcheck_entry_t	*fe = NULL;
	int						tocheck = 0;



	TRACE((T_INODE, "Volume[%s].CKey[%s](skip=%d,origindown=%d) - INODE[%u] begins\n",
					volume->signature,
					inode->q_id,
					skipalways,
					origindown,
					inode->uid
					));
	if (origindown) {
		return NC_STALE_BY_OFFLINE;
	}

	fe = nvm_get_fcg_first(volume);
	while (fe) {
		fc_count++;

		fresh	= U_TRUE;
		sc		= FCG_CONTINUE; /* stop or continue */
		if (!skipalways)
			sc = fe->step(volume, inode, stat, &fresh, &checked, &vr, fe->fcid);
		if (!checked)  {
			/*
			 * 신선도 정책에 명시되어있으나
			 * 실제 응답 데이타에 해당 정책 속성이 포함되어있지않음
			 */
			skip_count++; 
		}
		if (!fresh || (sc == FCG_STOP)) 
			goto L_fc_break; /* stop 조건이거나 !fresh이면 더이상 진행필요 없음 */

		fe = nvm_get_fcg_next(volume, fe);
	}
#if 1
	tocheck = link_count(&volume->fc_graph, U_TRUE) - volume->fc_alwayscheck;
	if (tocheck == skip_count)  {
		/*
		 * 등록된 모든 신선도 검토 정책이 검토되었음에도
		 * 정책에 명시된 속성 검토를 시도했으나 모두 skip됨
		 * 2021.2.7 by weon 
		 * 기존의 경우 stale로 처리했으나, fresh로 판단하기로 변경
		 */
		//vr 		= NC_FRESH;
		vr 		= defval;
		TRACE((T_INFO, "Volume[%s].CKey[%s](skip=%d,origindown=%d) - INODE[%u] all specified freshnesscheck policies(%s) skipped because origin has no such properties(regarded %s)\n",
						volume->signature,
						inode->q_id,
						skipalways,
						origindown,
						inode->uid,
						volume->zpolicy,
						__z_fresh_str[vr]
						));
	}
#endif


L_fc_break:
	TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - %s;"
					"INODE{%s},STAT{ifon[%d],OChg[%d],Sz[%lld],MTIME[%ld],Etag[%s]}\n", 
					inode->volume->signature,
					inode->q_id, 
					(long)inode->uid, 
					__z_fresh_str[vr],
					dm_dump_lv1(ibuf, sizeof(ibuf), inode),
					stat->st_ifon,
					stat->st_originchanged,
					stat->st_size,
					stat->st_mtime,
					stat->st_devid
					));
	return vr;
}



fc_cache_status_t
nvm_need_freshness_check_nolock(nc_volume_context_t *volume, fc_inode_t *inode)
{
	fc_cache_status_t	cs  = CS_FRESH;
	char				zobi[256];
	static char			*_cs[] = {
		"FRESH",
		"NEED_REVAL",
		"MARK_STALE"
	};

	


	if ((cs == CS_FRESH) && inode->ob_nocache)
		cs = CS_NEED_REVAL;


	if (cs == CS_FRESH)  {
		cs = (inode->vtime >= nc_cached_clock()) ? CS_FRESH:CS_NEED_REVAL;
		if (cs == CS_NEED_REVAL && (inode->origincode != 0 && cfs_iserror(volume, inode->origincode))) {
			/* valid time expired */
			cs = CS_MARK_STALE;
		}
	}

	if ((cs == CS_NEED_REVAL) && inode->volume->alwaysexpire) {
		/*
		 * 2020.1.4 by weon
		 * NEED_REVAL이지만 alwaysexpire가 설정된 경우엔 STALE 처리 해버림
		 */
		cs = CS_MARK_STALE;
	}


	TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u].%s - %s;vtime[%lld] =?= now[%lld]\n",
					volume->signature,
					inode->q_id,
					inode->uid,
					obi_dump(zobi,sizeof(zobi), &inode->obi),
					(char *)_cs[cs],
					(long long)inode->vtime,
					(long long)nc_cached_clock()));

	return cs;
}

int
nvm_is_usable_state(fc_inode_t *inode, nc_validation_result_t flag)
{
	int 	r = 0;

	r = 	(flag == NC_FRESH) ||
			(flag == NC_REFRESH_FRESH); 

//	if  (!inode->ob_mustreval) {
//		r = r || 
//			(flag == NC_STALE_BY_OFFLINE);
//	}
	return r;
}
static nc_uint64_t 
nvm_calc_signature(nc_volume_context_t *volume, char *path)
{
	nc_uint64_t 	val = 0;
	if (path)
		val = nvm_hash_inode((void *)path, (void *)volume) ;
	if (volume) 
		val |= (((nc_uint64_t)nvm_hash_inode(volume->signature, (void *)volume)) << 32);
	return val;
}
void
nvm_path_lock_unref_reuse(nc_volume_context_t *volume, nc_path_lock_t *pl, char *f, int l)
{
	char		zpath[2048];
	DEBUG_ASSERT_PATHLOCK(pl, glru_getref(g__path_cache, &pl->node) > 0); 

	glru_unref(g__path_cache, &pl->node);

	TRACE((T_INODE, "Volume[%s] - (%s) unrefered at %d@%s\n", 
						volume->signature,
						nvm_path_lock_dump(zpath, sizeof(zpath), pl),
						l,
						f));
	//DEBUG_ASSERT_PATHLOCK(pl, glru_getref(g__path_cache, &pl->node) >= 0); 

}
void
nvm_path_lock_ref_reuse(nc_volume_context_t *volume, nc_path_lock_t *pl, char *f, int l)
{
	char	zpath[2048];
	/*
	 * reuse 를 실행하기 전에, 대상 객체는 refcnt > 0  이어야함
	 * 그렇지 않으면, steal되었을 가능성이 있음
	 */
	DEBUG_ASSERT_PATHLOCK(pl,glru_getref(g__path_cache, &pl->node) > 0); 

	glru_makeref(g__path_cache, &pl->node);
	//DEBUG_ASSERT_PATHLOCK(pl, glru_getref(g__path_cache, &pl->node) > 0); 
	TRACE((T_INODE, "Volume[%s] - (%s) refered at %d@%s\n", 
					volume->signature,
					nvm_path_lock_dump(zpath, sizeof(zpath), pl),
					l,
					f
					));
}
nc_path_lock_t *
nvm_path_lock_ref(nc_volume_context_t *volume, char *path, char *file, int line)
{
	nc_path_lock_t 		*pl;
	int 				res;
	glru_node_t 		*gnode = NULL;
	char				zpath[2048];

	res = glru_ref(g__path_cache, &gnode, path/*key*/, volume/*map*/, U_TRUE/*alloc*/, U_FALSE, __func__, __LINE__);
	DEBUG_ASSERT(gnode != NULL);

	if (res == GLRUR_ALLOCATED) {
		pl = (nc_path_lock_t *)gnode->gdata;
		/*
		 * fill
		 */
		pl->volume 		= volume;
		pl->private 	= 0;
		pl->state 		= PLS_RESOURCE_IDLE;
		pl->signature 	= nvm_calc_signature(volume, path);
		INIT_LIST(&pl->waiters);
		glru_commit(g__path_cache, gnode);
	}
	/*
	 * path lock을 획득했다고 보장되는 시점
	 */
	pl = (nc_path_lock_t *)gnode->gdata;

	//DEBUG_ASSERT_PATHLOCK(pl, glru_getref(g__path_cache, &pl->node) > 0); 
	TRACE((T_INODE, "Volume[%s] - (%s), refered at %d@%s\n", 
					volume->signature,
					nvm_path_lock_dump(zpath, sizeof(zpath), pl),
					line,
					file
					));

	return pl;
}


int
nvm_path_lock_unref(nc_volume_context_t *volume, nc_path_lock_t *plck, char *f, int l)
{
	char	zpath[2048];
	glru_unref(g__path_cache, &plck->node);
	TRACE((T_INODE, "Volume[%s] - (%s),  unrefed at %d@%s\n", 
					volume->signature,
					nvm_path_lock_dump(zpath, sizeof(zpath), plck),
					l,
					f
					));

	//DEBUG_ASSERT_PATHLOCK(plck, glru_getref(g__path_cache, &plck->node) >= 0); 

	return 0;
}


int
nvm_path_lock_cond_timedwait( pthread_cond_t *hsignal, nc_path_lock_t *pl, struct timespec *ts)
{
	int		r;

	r = pthread_cond_timedwait(hsignal, &pl->lock, ts);
}
int
nvm_path_lock(nc_volume_context_t *volume, nc_path_lock_t *pl, char *f, int l)
{
	int 				res;
	char				zpath[2048];
#if defined(NC_MEASURE_PATHLOCK)
	int					owned = 0;

	owned = ((pl->lock).__data.__owner == trc_thread_self());
#endif


	DEBUG_ASSERT_PATHLOCK(pl, glru_getref(g__path_cache, &pl->node) > 0); 

	LO_CHECK_ORDER(LON_PATHLOCK);
	LO_PUSH_ORDER_FL(LON_PATHLOCK, f, l);
	/*
	 * thread context-switching회피
	 */
	if ((res = pthread_mutex_trylock(&pl->lock)) != 0) {
		pthread_mutex_lock(&pl->lock);
	}


	TRACE((T_INODE, "Volume[%s] - (%s)  acquired lock at %d@%s\n", 
					volume->signature,
					nvm_path_lock_dump(zpath, sizeof(zpath), pl),
					l,
					f
					));
#if defined(NC_MEASURE_PATHLOCK)
	if (!owned) {
		pl->pls = nvm_lookup_pl_stat(f, l);
		PROFILE_CHECK(pl->t_s);
		PROFILE_CHECK(pl->t_e);
	}
#endif



	return res;
}

int
nvm_path_is_private_nolock(nc_path_lock_t *pl)
{
	DEBUG_ASSERT(pl != NULL);
	return (int)(pl->private != 0);
}
void
nvm_path_set_private_nolock(nc_path_lock_t *pl, int priv)
{
	DEBUG_ASSERT(pl != NULL);
	pl->private = priv;
}
void
nvm_path_inprogress_nolock(nc_path_lock_t *pl, int set, char *f, int l)
{
	int		old, new, casr; 
#ifdef NC_DEBUG_PATHLOCK
	nc_uint32_t	tflg = T_WARN;
#else
	nc_uint32_t	tflg = 0;
#endif
	char		zpath[2048];

	DEBUG_ASSERT_PATHLOCK(pl,glru_getref(g__path_cache, &pl->node) > 0); 
	DEBUG_ASSERT(nvm_path_lock_owned(NULL, pl));
#ifdef NC_RELEASE_BUILD 
	old = pl->state;
	new = (set?PLS_RESOURCE_INPROGRESS:PLS_RESOURCE_IDLE); 
	DEBUG_ASSERT(pl->state != new);
	// 호출이 set/unset각각 한군데 밖에 없어서 line# 불필요
	
	//pl->state = new;
	casr = _ATOMIC_CAS(pl->state, old, new);
	DEBUG_ASSERT(casr);
	TRACE((T_INODE, "Volume[%s] - (%s) at %d@%s\n", 
					pl->volume->signature,
					nvm_path_lock_dump(zpath, sizeof(zpath), pl),
					l,
					f
					));
#else
	old = pl->state;
	new = (set?_ATOMIC_ADD(pl->state, 1):_ATOMIC_SUB(pl->state, 1));
	DEBUG_ASSERT(new >= 0 && new < 2);
	
	TRACE((T_INODE, "Volume[%s] - (%s) at %d@%s\n", 
					pl->volume->signature,
					nvm_path_lock_dump(zpath, sizeof(zpath), pl),
					l,
					f
					));
#endif
}
/*
 * path에서 대기중인 job들 wakeup
 */

int 
nvm_post_wakeup(nc_volume_context_t *volume, fc_inode_t *inode, nc_path_lock_t *pl, nc_stat_t *stat, int error)
{
	int 	scheduled  = 0;
	char	cbuf[1024];
	char	zpath[2048];
	apc_open_context_t *apc = NULL;
	fc_file_t			*fh;

	if (error && error != EWOULDBLOCK) {
		while ((apc = (apc_open_context_t *)link_get_head(&pl->waiters)) != 0) {
			TRACE((T_INODE, "VOLUME[%s]:CKey[%s] - got error %d, finishing a pending context{%s} (ctxs left in pending q=%d, stat.oc=%d)\n", 
								volume->signature, 
								apc->cache_key, 
								error,
								dm_dump_open_context(cbuf, sizeof(cbuf), apc),
								link_count(&pl->waiters, U_TRUE),
								stat->st_origincode
								));
			(*apc->callback)(NULL,  stat, error, apc->callback_data);
			apc_destroy_open_context(apc);
			VOLUME_UNREF(volume);
		}
	}
	else {
		/*
		 *
		 * APC_OS_WAIT_FOR_COMPLETION 상태로 대기중인 context 중
		 * LIFO 순으로 하나를 선택해서 event-queue에 넣고 실행
		 * 
		 */
#if 1
		if ((apc = (apc_open_context_t *)link_get_head(&pl->waiters)) != NULL) {
			volume = apc->volume;
			TRACE((T_INODE, "VOLUME[%s] - resuming a O-CTX{%p:%s} on (%s)\n",
								volume->signature, 
								apc,
								dm_dump_open_context(cbuf, sizeof(cbuf), apc),
								nvm_path_lock_dump(zpath, sizeof(zpath), pl)
								));
			asio_post_apc(apc, __FILE__, __LINE__);
			scheduled++;
		}
#else
		/*
		 * The following code is to reduce context switching overhead if deployed
		 * If inode is NOT NULL
		 *		(1) inode->refcnt > 0 
		 *		(2) inode is not a VARY template object
		 *		(3) inode is already onlined (through step 1)
		 *
		 */
		DEBUG_ASSERT(inode == NULL || INODE_GET_REF(inode) > 0);

		if (inode && dm_is_inode_reusable(inode) && (nvm_need_freshness_check_nolock(volume, inode)== CS_FRESH)) {

			while ((apc = (apc_open_context_t *)link_get_head(&pl->waiters)) != 0) {
				fh = dm_apc_fast_reopen_internal(inode, apc);
				(*apc->callback)(fh,  stat, error, apc->callback_data);
				TRACE((T_INODE, "APC[%p]- INODE[%d]/R[%d] fast reopened\n", apc, inode->uid, INODE_GET_REF(inode)));
				apc_destroy_open_context(apc);
				VOLUME_UNREF(volume);
			}
			/*
			 * no async-scheduling necessary
			 */
		}
		else {
			if ((apc = (apc_open_context_t *)link_get_head(&pl->waiters)) != NULL) {
				TRACE((T_INODE, "VOLUME[%s]:CKey[%s] - resuming a pending context{%s} (pending ctx(s)=%d, stat.oc=%d, error=%d)\n", 
									volume->signature, 
									apc->cache_key, 
									dm_dump_open_context(cbuf, sizeof(cbuf), apc),
									link_count(&pl->waiters, U_TRUE),
									stat->st_origincode,
									error
									));
				asio_post_apc(apc, __FILE__, __LINE__);
				scheduled++;
			}
		}
#endif
	}
	return scheduled;
}
int
nvm_path_lock_is_nolock(nc_path_lock_t *pl, nc_path_lock_state_t s, char *f, int l)
{
	int		r = 0;
	r = (pl->state == s);
	return r;
}
int
nvm_path_lock_is(nc_path_lock_t *pl, nc_path_lock_state_t s, char *f, int l)
{
	int					r = 0;

	nvm_path_lock(pl->volume, pl, f, l);
	r 	= nvm_path_lock_is_nolock(pl, s, f, l);
	nvm_path_unlock(pl->volume, pl, f, l);

	return r;
}


char *
nvm_path_lock_dump(char *buf, int len, nc_path_lock_t *pl)
{
#ifdef NC_RELEASE_BUILD
	snprintf(	buf, 
				len, 
				"PL[%p(%s)].%llu/R[%d]/S[%s]/queue[%d]", 
				pl, 
				pl->path, 
				pl->signature,
				glru_getref(g__path_cache, &pl->node),
				__z_ps[pl->state], 
				link_count(&pl->waiters, U_TRUE));
#else
	snprintf(	buf, 
				len, 
				"PL[%p(%s)].%llu/R[%d]/S[%d]/queue[%d]", 
				pl, 
				pl->path, 
				pl->signature,
				glru_getref(g__path_cache, &pl->node),
				pl->state,
				link_count(&pl->waiters, U_TRUE));
#endif
	return buf;
}
int
nvm_path_busy_nolock(nc_path_lock_t *pl)
{
#ifdef NC_RELEASE_BUILD
	return nvm_path_lock_is_nolock(pl, PLS_RESOURCE_INPROGRESS, __FILE__, __LINE__);
#else
	return pl->state;
#endif
}
int
nvm_path_put_wait(nc_path_lock_t *pl, apc_open_context_t  *oc, char *f, int l)
{
#ifdef NC_DEBUG_PATHLOCK
	int		tflg = 0;
#else
	int		tflg = 0;
#endif
	char	zpath[2048];

	nvm_path_lock(pl->volume,  pl, f, l);
	link_append(&pl->waiters, oc, &oc->node);
	TRACE((tflg|T_INODE, "Volume[%s] -  put WAIT(O-CTX[%p]) on (%s)  at %d@%s\n", 
					pl->volume->signature,
					oc,
					nvm_path_lock_dump(zpath, sizeof(zpath), pl),
					l,  
					f));
	nvm_path_unlock(pl->volume, pl,f, l);
	return 0;
}

void
nvm_path_set_state(nc_path_lock_t *pl, nc_path_lock_state_t state)
{
	DEBUG_ASSERT(pl != NULL);
	DEBUG_ASSERT(pl->lock.__data.__owner == trc_thread_self());
	pl->state = state;
}
nc_path_lock_state_t
nvm_path_get_state(nc_path_lock_t *pl)
{
	DEBUG_ASSERT(pl != NULL);
	DEBUG_ASSERT(pl->lock.__data.__owner == trc_thread_self());
	return pl->state;
}

/*
 * path_lock을 획득하고 있는 상태에서의 호출이므로
 * volume lock을 획득할 필요 없음
 */
int
nvm_path_unlock(nc_volume_context_t *volume, nc_path_lock_t *plck, char *f, int l)
{
	int 			r;
#if defined(NC_MEASURE_PATHLOCK)
	int				owned = 0;
	perf_val_t		s, e;
#endif
	char			zpath[2048];


	DEBUG_ASSERT_PATHLOCK(plck, plck != NULL);
	DEBUG_ASSERT_PATHLOCK(plck, volume != NULL);
	DEBUG_ASSERT_PATHLOCK(plck, nvm_path_lock_owned(volume, plck));
	DEBUG_ASSERT_PATHLOCK(plck, glru_getref(g__path_cache, &plck->node) > 0); 
	TRACE((T_INODE, "Volume[%s] - (%s), lock release at %d@%s\n", 
					volume->signature,
					nvm_path_lock_dump(zpath, sizeof(zpath), plck),
					l,
					f
					));


#if defined(NC_MEASURE_PATHLOCK)
	s = plck->t_s;
	PROFILE_CHECK(e);
#endif


	r = pthread_mutex_unlock(&plck->lock);
	DEBUG_ASSERT_PATHLOCK(plck, r == 0);
	//

#if defined(NC_MEASURE_PATHLOCK)
	owned = ((plck->lock).__data.__owner == trc_thread_self());
	if (!owned) {
		double msec;
		msec = (double)(PROFILE_GAP_USEC(s, e)/1000.0);
		nvm_update_pl_stat(plck->pls, msec);
	}
#endif
	LO_POP_ORDER(LON_PATHLOCK);

	//
	// 아래 assert는 향후 remind용으로 삭제하지 않고 코멘트로 남겨둠
	// unlock이 실제 실행된 이우 refcnt의 값은 0이 될 수 있음
	// 2021.2.21 by weon@solbox.com
	//
	// DEBUG_ASSERT_PATHLOCK(plck, glru_getref(g__path_cache, &plck->node) > 0); 

	return r;
}
int
nvm_path_lock_owned(nc_volume_context_t *volume, nc_path_lock_t *plck)
{
	return (plck && plck->lock.__data.__owner == trc_thread_self());
}
int
nvm_path_lock_owned_n_healthy(nc_volume_context_t *volume, nc_path_lock_t *plck, char *path)
{
	/*
	 * requirement
	 * 		1. path should be valid
	 *		2. the calling thread should own the lock
	 *		3. the lock has reference count larger than 0
	 */

	return ((plck->lock.__data.__owner == trc_thread_self()) && 
			(strcmp(plck->path, path) == 0) && 
			(glru_getref(g__path_cache, &plck->node) >0));
}
void
nvm_report_hit_info(nc_volume_context_t *volume, nc_xtra_options_t *opt, nc_hit_status_t hi)
{
	if (volume->monitor) (*volume->monitor)(opt, hi);
	return;
}
int
nvm_need_backup(nc_volume_context_t *volume)
{
	return g__need_stat_backup;
}

#if NOT_USED
static unsigned long 
nvm_hash_uuid(void *vstr, void *ud)
{
	unsigned long 			*uuidp = (unsigned long *)vstr;
	register int 			i;
	unsigned long	hv = 0;

	for (i = 0; i < sizeof(uuid_t)/sizeof(unsigned long); i++) {
		hv = (hv << 1) + *uuidp;
		uuidp++;
	}
	return hv;
}
static u_boolean_t
nvm_hash_uuid_equal(void *v1, void *v2, void *ud)
{
	return  memcmp(v1, v2, sizeof(uuid_t));
}
#endif

static void * nvm_pc_allocate(glru_node_t **gnodep)
{
	nc_path_lock_t 		*pl = NULL;
	pthread_mutexattr_t	mattr;

	pl = (nc_path_lock_t *)XMALLOC(sizeof(nc_path_lock_t), AI_VOLUME);

#ifndef PATHLOCK_USE_RWLOCK
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&pl->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);
#endif
	pl->path 		= NULL; 
	pl->signature 	= 0;

	*gnodep = &pl->node;
	return pl;
}
static void *nvm_pc_free(glru_node_t *gnode, void *data)
{
	nc_path_lock_t *plck = (nc_path_lock_t *)data;

	XVERIFY(data);
#ifdef PATHLOCK_USE_RWLOCK
	pthread_rwlock_destroy(&plck->lock);
#else
	pthread_mutex_destroy(&plck->lock);
#endif
	XFREE(plck->path);
	XFREE(plck);
	return NULL;
}
static void nvm_pc_reset(void *d)
{
	nc_path_lock_t *pl;

	XVERIFY(d);
	pl 				= (nc_path_lock_t *)d;
	XVERIFY(pl->path);
	pl->volume 		= NULL;
	pl->signature 	= 0;
	XFREE(pl->path);
}
static void nvm_pc_fill_key(void *d, const char * key)
{
	nc_path_lock_t *pl = (nc_path_lock_t *)d;
	XVERIFY(pl);
	pl->path = XSTRDUP(key, AI_VOLUME);
}
static void *nvm_pc_lookup(void *map, const char *key)
{
	glru_node_t 			*gnode = NULL;
	nc_volume_context_t 	*volume 	= (nc_volume_context_t *)map;
	gnode = (glru_node_t *)u_ht_lookup((u_hash_table_t *)volume->PL_tbl, (void *)key);
	return gnode;
}
static int nvm_pc_enroll(void *map, glru_node_t *gnode)
{
	nc_path_lock_t 		*plck = (nc_path_lock_t *)gnode->gdata;
	nc_volume_context_t 	*volume = (nc_volume_context_t *)map;

#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(plck != NULL);
	DEBUG_ASSERT(plck->path != NULL);
	DEBUG_ASSERT(volume != NULL);
	DEBUG_ASSERT(volume->PL_tbl != NULL);
#endif

	if (!u_ht_insert((u_hash_table_t *)volume->PL_tbl, (void *)plck->path, gnode, U_FALSE)) {
		TRACE((T_ERROR, "VOLUME[%s]/PATH('%s') - enroll failed, exist\n", volume->signature, plck->path ));
		TRAP;
	}
	TRACE((0, "VOLUME[%s]/PATH('%s') - enroll done\n", volume->signature, plck->path ));
	return GLRUR_FOUND;
}
static int nvm_pc_unroll(void *map, glru_node_t *gnode)
{
	nc_path_lock_t 			*plck = (nc_path_lock_t *)gnode->gdata;
	nc_volume_context_t 	*volume;
#ifndef NC_RELEASE_BUILD
	XVERIFY(plck);
	XVERIFY(plck->path);
#endif

	volume = (nc_volume_context_t *)plck->volume;
	if (!nvm_is_valid(volume)) { 
		/*
	 	 * 이미 해당 volume 삭제됨
		 */
		return GLRUR_FOUND;
	}
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(nvm_pc_isidle(plck) >= 0);
#endif
	if (!u_ht_remove((u_hash_table_t *)volume->PL_tbl, plck->path)) {
		TRACE((T_ERROR, "VOLUME[%s]/PATH('%s') - unroll failed, not exist?\n", 
						volume->signature, 
						plck->path
						));
		TRAP;
	}
	return GLRUR_FOUND;
}
static int nvm_pc_isidle(void *d)
{
	nc_path_lock_t 	*plck = (nc_path_lock_t *)d;

	XVERIFY(plck);

	return (plck->state == PLS_RESOURCE_IDLE);
}
static int nvm_pc_dump(char *b, int l, void *d)
{
	char				li[512]="";
	char				xb[512]="";
	glru_node_t 		*gnode = (glru_node_t *)d;
	nc_path_lock_t 		*plck = (nc_path_lock_t *)gnode->gdata;

#ifdef KV_TRACE
	if (plck->file)
		sprintf(li, "%d@%s", plck->line, plck->file);
#endif
	TRACE((T_MONITOR, "[%d] %4d   V[%s]:K[%s]:%s [%s];%s\n", 
					glru_check_idle(gnode),
					glru_getref(g__path_cache, gnode),
					plck->volume->signature, 
					plck->path, 
					__z_ps[plck->state],
					li,
					glru_dump(xb, sizeof(xb), gnode)
					));
	return 0;
}
/*
 * 이 함수는 read를 요청했을 때 실제 데이타를 fill하기 전에 driver에서 호출함
 * 호출의 조건은 객체에 대한 요청이 entire(sub-range요청이 아닌)일 때
 * 
 */
static int
nvm_lazy_property_updater(fc_inode_t *inode, nc_stat_t *rstat)
{
	char 		ibuf[2048];

	INODE_LOCK(inode);
#ifndef NC_RELEASE_BUILD
	dm_verify_allocation(inode);
#endif

		/*
		 * 동적인 컨텐츠이고, 요청 시마다 크기가 달라지는 객체임
		 */
	
		if (inode->ob_needoverwrite || inode->ob_pseudoprop) {
			TRACE((T_INODE, "INODE[%u]/O[%s] - new stat arrived\n"
				   			"%8s :%8s %8s\n" /* fields, current,  new */
						   	"size     :%8lld %8lld\n"
						   	"size_knwn:%8d %8d\n"
				   			"size_decl:%8d %8d\n"
				   			"rangeable:%8d %8d\n"
				   			"chunked  :%8d %8d\n"
				   			"nocache  :%8d %8d\n"
				   			"private  :%8d %8d\n",
							inode->uid, inode->q_path,	
				   			"fields", "current", "new",
				   			(long long)inode->size, (long long)rstat->st_size,
				   			inode->ob_sizeknown, rstat->st_sizeknown,
				   			inode->ob_sizedecled, rstat->st_sizedecled,
				   			inode->ob_rangeable, rstat->st_rangeable,
				   			inode->ob_chunked, rstat->st_chunked,
				   			inode->ob_nocache, rstat->st_nocache,
				   			inode->ob_priv, rstat->st_private));
				dm_set_inode_stat(inode, rstat, inode->imode, inode->ob_pseudoprop != 0);
				inode->ob_needoverwrite = 0;
				inode->ob_pseudoprop = 0;
				TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] properties updated;%s\n", 
								inode->volume->signature, 
								inode->q_id, 
								inode->uid,
								(inode?dm_dump_lv1(ibuf, sizeof(ibuf), inode):"")
								));
		}
	INODE_UNLOCK(inode);
	return 0;
}
#if NOT_USED
static int 
nvm_check_if_match(nc_volume_context_t *volume, regex_t *reg_pat, char *target)
{
	int 			res = 0;	
	regmatch_t 		pmatch;
	res = regexec(reg_pat, target, 1, &pmatch, 0);
	return res; 
}
#endif
void *
nvm_create_pattern(nc_volume_context_t *volume, char *pat_string)
{
	regex_t  *reg_pat = (regex_t *)XMALLOC(sizeof(regex_t), AI_ETC);
	int 	caseflag = 0;
	int 	status = 0;


	if (!volume->preservecase) {
		/*
		 * 해당 볼륨이 대소문자 무시
		 */
		caseflag = REG_ICASE; 
	}
	status = regcomp(reg_pat, pat_string, REG_EXTENDED|REG_NEWLINE|caseflag);
	if (status != 0) {

		// BUG 2019-03-12 huibong 변수 미초기화 및 출력 인자 count mismatch 로 인한 SIGSEGV 발생 수정 (#32543)
		//                - 본 함수는 #32386 에서 더이상 호출되지 않도록 수정되었으나....
		//                - 구형 버전이 적용된 장비에서 SIGSEGV 발생
		//                - 원인은 error_msg 미초기화 및 TRACE 구문 출력인자 mismatch 로 발생.
		//                - 향후 본 함수를 다시 사용할 가능성이 있어 수정 처리함.
		char error_msg[1024];
		memset( error_msg, 0x00, sizeof( error_msg ) );

		regerror(status, reg_pat,  error_msg, sizeof(error_msg));
		TRACE((T_ERROR, "VOLUME[%s] - pattern[%s] got error : '%s'\n", volume->signature, (pat_string ? pat_string : "NULL"), error_msg));
		XFREE(reg_pat);
		return NULL;
	}
	return reg_pat;
}
static int
nvm_hard_purge_target(void *key, void *value, void *ud)
{
	purge_info_t 		*pi = (purge_info_t *)ud;
	fc_inode_t 			*inode = (fc_inode_t *)value;
	match_inode_t 		*pnode = NULL;
	int 				eq;
	int 				flag = 0;
	nc_volume_context_t *volume = inode->volume;
	int					res = HT_CONTINUE;

    if (volume && volume->preservecase == 0)
		flag = FNM_CASEFOLD;

	if (pi->iskey) {
		eq = (fnmatch(pi->pattern, key, flag) == 0);
	}
	else {
		eq = (fnmatch(pi->pattern, inode->q_path, flag) == 0);
	}

	if (eq) {
		/*
		 * 이미 mdb기준으로 disk 캐시들은 모두 퍼지되었으므로
		 */
		pnode = (match_inode_t *)XMALLOC(sizeof(match_inode_t), AI_ETC);

		INIT_NODE(&(pnode->node));

		pnode->inode 	= inode;
		pnode->qid 		= XSTRDUP(inode->q_id, AI_ETC);
		pnode->qpath 	= XSTRDUP(inode->q_path, AI_ETC);
		pnode->inode_signature = dm_get_signature(inode);
		pi->count++;
		link_append(&pi->list, (void *)pnode, &pnode->node);
		res = HT_CONTINUE;
	}
	return res;
}
static int
nvm_soft_purge_target(void *key, void *value, void *ud)
{
	purge_info_t 		*pi = (purge_info_t *)ud;
	fc_inode_t 			*inode = (fc_inode_t *)value;
	//match_inode_t 		*pnode = NULL;
	int 				eq;
	int 				flag = 0;
	nc_volume_context_t *volume = inode->volume;

    if (volume && volume->preservecase == 0)
		flag = FNM_CASEFOLD;

	if (pi->iskey) {
		eq = (fnmatch(pi->pattern, key, flag) == 0);
	}
	else {
		eq = (fnmatch(pi->pattern, inode->q_path, flag) == 0);
	}

	if (eq)  { 
		/*
		 * 이미 mdb기준으로 disk 캐시들은 모두 퍼지되었으므로
		 */
		eq 				= 0; /* hash table에서 entry 삭제 못하도록 */
		DM_UPDATE_METAINFO(inode,  inode->vtime = 0);
	}
	return HT_CONTINUE;
}
void
nvm_destroy_pattern(nc_volume_context_t *volume, void *reg_pat)
{
	DEBUG_ASSERT(reg_pat != NULL);
	XVERIFY(reg_pat);
	regfree(reg_pat);
	XFREE(reg_pat);
	return;
}

/*
 *
 * 주의) 아래 함수는 enum만 실행하는 것이 아니라 maching된 inode에 대해서 다음과 같은 작업이 실제 이루어짐
 * (1)pi->ishard=TRUE 인 경우 CLFU에서 삭제됨
 * (2)pi->ishard=FALSE 인 경우 vtime=0로 초기화하여 재사용 시점에 신선도 검사가 강제되도록 변경
 * (3)soft/hard상관없이 VIT table 에서 매칭된 inode를 제외
 *
 */
static int
nvm_enum_purge_pattern(purge_info_t *pi)
{
	/* CHG 2018-10-16 huibong 불필요한 정규식 처리관련 코드 제거 (#32386) */


	/*
	 * inode cache에서 pattern에 부합하는 모든 inode를 제거
	 */
	if (strchr(pi->pattern, '*')) {
		if (pi->ishard) 
			u_ht_remove_foreach(pi->volume->object_tbl, nvm_hard_purge_target, pi);
		else
			u_ht_foreach(pi->volume->object_tbl, nvm_soft_purge_target, pi);
	}
	else {
		match_inode_t 		*pnode = NULL;
		fc_inode_t			*inode = NULL;

		inode = nvm_lookup_inode(	pi->volume, 
									DM_LOOKUP_WITH_KEY, 
									pi->pattern, 
									U_FALSE,  /* hard일경우 제거*/
									U_FALSE,  /* refcnt 그대로 */
									U_FALSE,  /* DON't mark busy */
									NULL,
									__FILE__, 
									__LINE__);


		if (inode) {
			if (pi->ishard ) {
				pnode = (match_inode_t *)XMALLOC(sizeof(match_inode_t), AI_ETC);
				pnode->inode_signature = dm_get_signature(inode);
				pnode->qid 		= XSTRDUP(inode->q_id, AI_ETC);
				pnode->qpath 	= XSTRDUP(inode->q_path, AI_ETC);
				pnode->inode 	= inode;
				link_append(&pi->list, (void *)pnode, &pnode->node);

				pi->count++;
			}
			else
				DM_UPDATE_METAINFO(inode,  inode->vtime = 0);

		}
	}
L_skip_pick:
	TRACE((T_INODE, "VOLUME[%s]/PATTERN(%s) - %d purge candidates found on VIT\n", pi->volume->signature, pi->pattern, pi->count));
	return pi->count;
}
/*
 * ir #24803 : 해당 볼륨에 속한 모든 inode에 대해서 아래 함수가 호출됨
 *  	cversion : 현재 갱신된 최근 볼륨 캐싱 version과 동기화
 *  	vtime    : 반드시 원본과 비교해야할 대상으로 마킹
 * 		mdbsynctime 수정 : inode를 close 하는 시점에 헤더 정보는 disk에 저장되어야함(MDB에도)
 */
struct tag_purge_volume_context {
	nc_uint32_t 			purged;
	nc_volume_context_t 	*volume;
};
static int
nvm_softpurge_inode_callback(void *key, void *value, void *ud)
{
	fc_inode_t 		*inode = (fc_inode_t *)value;
	int 			*pcount = (int *)ud;



	DM_UPDATE_METAINFO(inode, inode->vtime = 0);

	TRACE((T_INODE, "Volume[%s].CKey[%s]: INODE[%u] - vtime cleared\n", 
					inode->volume->signature,
					inode->q_id,
					inode->uid));
	if (pcount) *pcount += 1;
	return HT_CONTINUE;
}
int
nvm_purge_volume(nc_volume_context_t *volume)
{

	int 	x;
	nc_uint32_t 	ov;
	struct tag_purge_volume_context	pc = {0, volume};

	/*
	 * 실제 table entry를 넣고 빼는 오퍼레이션이 아니라
	 * 읽는 오퍼레이션이므로 shared로 가능
	 */
	IC_LOCK(CLFU_LM_SHARED);

	ov = volume->version;
	volume->version++;

	/*
	 * phase-I : VIT내의 inode 버전 변경
	 */
	nvm_do_operation(volume, nvm_softpurge_inode_callback, (void *)&pc);
	TRACE((0, "Volume-Purge/Phase-I: VOLUME[%s] - %d inodes purged from VIT\n", volume->signature, pc.purged));
	IC_UNLOCK;

	/*
	 * phase-II : MDB의 version 변경
	 */
	x = mdb_purge_volume(volume->signature, volume->version);
	TRACE((T_INFO, "Volume-Purge/Phase-II: VOLUME[%s] - volume age updated(version;%d->%d)\n", 
					volume->signature, 
					ov, 
					volume->version));


	return x;

}

/*
 * purge 대상은 disk-cache이거나 memory-only cache 일 수 있음
 */
int
nvm_purge(nc_volume_context_t *volume, char *pattern, int ishard, int iskey)
{
	/* CHG 2018-10-16 huibong 잘못된 로깅 내역 수정 및 정리, 불필요 코드 정리  (#32386) */

	/*regex_t 			*reg_pattern = NULL; */
	int 				purgecnt1 = 0, purgecnt2 = 0;
	int					retry = 0;
	nc_path_lock_t 		*pl_parent;
	purge_info_t		pi = {
			.volume		= volume,
			.pattern	= pattern,
			.comp_pattern	= NULL,
			.ishard		= ishard,
			.iskey		= iskey,
			.count		= 0,
			.list		= LIST_INITIALIZER
	};
	char			lock_info[64];
	mdb_tx_info_t 	txno;
	match_inode_t	*pnode;
	nc_path_lock_t	*pl;
	fc_inode_t		*inode;
	int				r, stolen;
#ifdef __PROFILE
	perf_val_t		ts, te;
	long			wd;
#endif


	/* NEW 2018-10-16 huibong purge 처리 관련 각 단계별 수행시간 로깅을 위한 기능 추가 (#32386) */
	long sec_perform_p1 = 0;
	long sec_perform_p2 = 0;
#ifdef __PROFILE
	perf_val_t time_start, time_end;
#endif


	/*
	 * pattern에 '/'가 없을 경우 '/'를 default로 사용토록 해야함
	 */
	/*
	 * 툭수 경로 (purge serialization)
	 */
	sprintf(lock_info, "@PURGE.%s", volume->signature);
	pl_parent = nvm_path_lock_ref(volume, lock_info, __FILE__, __LINE__);
	nvm_path_lock(volume, pl_parent, __FILE__, __LINE__);

#ifdef __PROFILE
	PROFILE_CHECK(time_start);
#endif
	/*
	 ************************************************************************************
	 * P H A S E   I : MDB 기반 purge
	 * 		- mdb에서 추출된 key, path등의 정보를 이용해서 inode까지 purge 실행
	 ************************************************************************************
	 */
	purgecnt1 = pm_purge(volume, pattern, iskey, ishard);

#ifdef __PROFILE
	PROFILE_CHECK(time_end);
	sec_perform_p1 = PROFILE_GAP_SEC(time_start, time_end);
#endif




#ifdef __PROFILE
	PROFILE_CHECK(time_start);
#endif
	IC_LOCK(CLFU_LM_EXCLUSIVE);
	/*
	 * 퍼지 패턴으로 퍼지 대상을 VIT에서 검색
	 * 하드퍼지의 경우 대상 inode가 추가 오퍼레이션 중 reclaim되지않도록
	 * ic_set_busy()를 통해서 마킹해둠
	 * !!!!!!!! 주의: 또한 busy marking이 된 객체는 퍼지 대상에서 제외한다
	 */
	nvm_enum_purge_pattern(&pi);
	IC_UNLOCK;
	/*
	 * 아래 로직에서 list에 포함된 객체들은 hard-purge 대상들임
	 * soft-purge의 경우 위의 enum operation동안 처리완료됨
	 */
	pnode = (match_inode_t *)link_get_head(&pi.list);
	while (pnode) {
		pl = nvm_path_lock_ref(volume, pnode->qpath, __FILE__, __LINE__);
		nvm_path_lock(volume, pl, __FILE__,__LINE__);
		inode = pnode->inode;

		while (nvm_path_busy_nolock(pl))  {
			nvm_path_unlock(volume, pl, __FILE__, __LINE__);
			bt_msleep(10);
			nvm_path_lock(volume, pl, __FILE__, __LINE__);
		}
		stolen = 0;
		IC_LOCK(CLFU_LM_SHARED);
		while (ic_is_busy(inode)) {
			IC_UNLOCK;
			nvm_path_unlock(volume, pl, __FILE__, __LINE__);
			bt_msleep(10);
			stolen++;
			nvm_path_lock(volume, pl, __FILE__, __LINE__);
			IC_LOCK(CLFU_LM_SHARED);
		}
		ic_set_busy(inode, U_TRUE, __FILE__, __LINE__);
		IC_UNLOCK;
#ifndef NC_RELEASE_BUILD
		if (stolen > 0)
			TRACE((T_INFO, "Volume[%s].CKey[%s] - INODE[%d] stolen(%d) while purging\n", volume->signature, pnode->qpath, inode->uid, stolen));
#endif
		if (pnode->inode_signature != dm_get_signature(inode)) {
			/* swapped or reclaimed already*/
			TRACE((T_INFO, "Volume[%s].CKey[%s] - INODE[%d] reclaimed while purging\n", volume->signature, pnode->qpath, inode->uid));
			ic_set_busy(inode, U_FALSE, __FILE__, __LINE__);
			goto L_skip;
		}
#if 1
		/*
		 *  pl에지정된 객체에 대한 operation이 진행 중인 경우
		 *  대기해야함
		 */
		retry = 0;
		PROFILE_CHECK(ts);
		if  (nvm_path_busy_nolock(pl))  {
			nvm_path_unlock(volume, pl, __FILE__, __LINE__);
			bt_msleep(10);
		}

		while (nvm_path_busy_nolock(pl))  {
			/*
			 * work on the path, pi[i].path in progress
			 * need to wait
			 */
			nvm_path_unlock(volume, pl, __FILE__, __LINE__);
			retry++;
			bt_msleep(10);

			nvm_path_lock(volume, pl, __FILE__, __LINE__);
		}
		if (retry  > 0) {
			PROFILE_CHECK(te);
			wd = PROFILE_GAP_MSEC(ts, te);
			TRACE((T_INFO, "Volume[%s].Key[%s] - INODE[%d] purged after %.2f sec(s)\n",  volume->signature,  inode->q_path, inode->uid, (float)(wd/1000.0)));
		}
#else
		/* inprogress는 enum_pattern()에[서 설정됨*/
#endif
		DEBUG_ASSERT_PATHLOCK(pl, (nvm_path_lock_ref_count(pl) > 0));
		DEBUG_ASSERT_PATHLOCK(pl, (nvm_path_lock_is_for(pl, inode->q_path)));

		r = nvm_purge_inode(volume, inode, U_TRUE, NULL);

		if (r == -EBUSY) {
			/* 
			 * 현재 사용중인 객체
			 */
			inode->ob_doc = 1;
		}
		purgecnt2++;
L_skip:
		nvm_path_unlock(volume, pl, __FILE__,__LINE__);
		nvm_path_lock_unref(volume, pl, __FILE__, __LINE__);
		pnode = (match_inode_t *)link_get_head(&pi.list);
	}

#ifdef __PROFILE
	PROFILE_CHECK(time_end);
	sec_perform_p2 = PROFILE_GAP_SEC(time_start, time_end);
#endif

	nvm_path_unlock(volume, pl_parent, __FILE__, __LINE__) ;
	nvm_path_lock_unref(volume, pl_parent, __FILE__, __LINE__) ;

	TRACE((T_INFO, "* PURGE[%s] : VOLUME[%s]/PAT[%s]/%s/%s - (%d/%.2fmsec , %d/%.2fmsec) total %.2fmsec\n", 
					(ishard?"hard":"soft"),
					volume->signature, 
					pattern, 
					(ishard?"hard":"soft"), 
					(iskey?"key":"path"), 
					purgecnt1, (float)(sec_perform_p1/1000.0),
					purgecnt2, (float)(sec_perform_p2/1000.0),
					(float)((sec_perform_p1+sec_perform_p2)/1000.0)
					));

	return (purgecnt1 + purgecnt2); /* 주의 이 숫자가 퍼지된 서로다른 객체의 갯수를 의미하지 않음!! */
}

static void
nvm_origin_path_change_proc(fc_inode_t *inode, char *new_path)
{
	TRACE((T_INFO, "VOLUME[%s]/INODE[%u] - origin path[%s] changed to '%s'\n", 
					inode->volume->signature, 
					inode->uid, 
					inode->q_id, 
					new_path));
	XFREE(inode->q_id);
	inode->q_path = XSTRDUP(new_path, AI_INODE);
}
int
nvm_is_valid(nc_volume_context_t *volume)
{
	return (volume && volume->online && volume->PL_tbl);
}
void *
nvm_get_fcg_first(nc_volume_context_t *volume)
{
	nc_freshcheck_entry_t	*fe = NULL;
	fe = link_get_head_noremove(&volume->fc_graph);
	return fe;
}
void *
nvm_get_fcg_next(nc_volume_context_t *volume, void *anc)
{
	nc_freshcheck_entry_t	*fe = (nc_freshcheck_entry_t	*)anc;
	fe = link_get_next(&volume->fc_graph, &fe->node);
	return fe;
}
int
nvm_fcg_id(void *anc)
{
	nc_freshcheck_entry_t	*fe = (nc_freshcheck_entry_t	*)anc;
	return fe->fcid;
}
struct tag_PL_oi {
	nc_volume_context_t *volume;
	int					count;		
};
static int
nvm_orphanize_PL_callback(void *glru_node, void *uc)
{
	glru_node_t 		*gnode = (glru_node_t *)glru_node;
	nc_path_lock_t 		*pl = (nc_path_lock_t *)gnode->gdata;
	struct tag_PL_oi 	*pl_oi = (struct tag_PL_oi *)uc;

	if (pl->volume == pl_oi->volume) {
		pl_oi->count++;
		pl->volume = NULL;
	}
	return 0;
}
int
nvm_orphanize_PL(nc_volume_context_t *volume)
{
	struct tag_PL_oi 	pl_oi =  {volume, 0};

	glru_for_each(g__path_cache, nvm_orphanize_PL_callback, (void *)&pl_oi);
	TRACE((T_INFO, "Volume[%s] - %d active path-locks orphanized\n", volume->signature, pl_oi.count));
	return 0;
}
char *
nvm_dump_PL(char *buf, int l, nc_path_lock_t *p)
{

	snprintf(	buf, 
					l, 
					"{Volume[%s].PATH[%s]/S[%s]",
					p->volume->signature,
					p->path,
					__z_ps[p->state]
					);
	return buf;
}
int
nvm_path_lock_is_for(nc_path_lock_t *pl, char *key)
{
	return (strcmp(pl->path, key) == 0);
}
int
nvm_path_lock_ref_count(nc_path_lock_t *pl)
{
	return glru_getref(g__path_cache, &pl->node);
}


/*
 * 
 * 동일 객체에 대한 연속읽기시 driver에서 호출됨
 * o setup
 *  cfs_ioctl(mc, NC_IOCTL_SET_IO_VALIDATOR, &nvm_check_if_same_object, sizeof(void *));
 * o 주의
 *   - Get 요청에 대한 응답의 헤더를 받은 경우 호출
 * 	 - 신선도 검사단계에서는  이 함수는 호출되지 않도록 해야함
 *     (안그러면 중복호출되고 이 함수가 우선 호출됨)
 * 
 */
static int
nvm_check_if_same_object(cfs_origin_driver_t *drv, fc_inode_t *inode, nc_stat_t *stat)
{
	int							issame = U_FALSE;
	nc_validation_result_t 		vr = NC_VNONE;
#if 0
	/*
	 * 2020.7.17 by weon
	 * 아래 코드는 막음
	 * stale 또는 purge로 인해 doc세팅이 되었다고 하더라도
	 * client가 사용중이면 IO는 계속되어야함
	 */
	/*
	 * 전제조건 확인
	 */
	if (!inode->volume  || inode->ob_staled || inode->ob_doc) {
		/*
		 * 호출될리는 없더라도 더 진행하지 말아야할 객체
		 * -1 return은 객체 정보의 불일치와 함께 IO abort를 요구함
		 */
		return -1;
	}
#endif
	issame =  dm_check_if_same_prop(inode, stat);
	if (issame && (inode->volume->alwaysexpire == 0)) {

		nvm_lock(inode->volume, U_FALSE, __FILE__, __LINE__);
		vr = nvm_check_freshness_by_policy(	inode->volume, 
										inode, 
										stat, 
										U_FALSE, 
										U_FALSE,
										NC_FRESH /*매칭 정책이 없는 경우는 FRESH로 판단*/
										); 
		nvm_unlock(inode->volume, __FILE__, __LINE__);
		/*
		 * usable_state = U_TRUE도 issame=TRUE로 해석
		 */
		issame = nvm_is_usable_state(inode, vr);
		if (!issame) {
			TRACE((T_INODE, "Volume[%s].CKey[%s].INODE[%u] - object modified while reading\n", 
							inode->volume->signature, 
							inode->q_id, 
							inode->uid
							));
		}
	}

	INODE_LOCK(inode);


	if (!issame)  {
		TRACE((T_INFO, "Volume[%s]/Key[%s] - the caching object INODE[%d] is stale(inode->origin=%d,stat->origin=%d:(%s)) while reading\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					inode->origincode,
					stat->st_origincode,
					__z_fresh_str[vr]
					));
		inode->staleonclose = 1;
	}
	else {
		TRACE((T_INODE, "Volume[%s]/Key[%s] - the caching object INODE[%d] is same with origin(inode->origin=%d,stat->origin=%d) \n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					inode->origincode,
					stat->st_origincode
					));
	}
	INODE_UNLOCK(inode);

	return (issame? 0:-1);

}
/*
 * 	
 *	해당 inode를 cache에서 제거 후 close때 제거되도록 처리
 *	이 함수는 nc_open으로 열려있는 inode에 대해서만 호출됨
 *	즉 inode->PL은 정상이며 PL->refcnt도 >0 임
 * 
 */
void
nvm_make_inode_stale(nc_volume_context_t *volume, fc_inode_t *inode)
{
	char			ibuf[1024];

	DEBUG_ASSERT_FILE(inode, (nvm_path_lock_ref_count(inode->PL) > 0));


	nvm_path_lock(volume, inode->PL, __FILE__, __LINE__);
	DEBUG_ASSERT(nvm_path_lock_owned(volume, inode->PL));

	IC_LOCK(CLFU_LM_EXCLUSIVE);
	nvm_isolate_inode_nolock(volume, inode);
	IC_UNLOCK;

	inode->ob_staled = 1;
	inode->ob_doc	 = 1;

	nvm_path_unlock(volume, inode->PL, __FILE__, __LINE__);

	TRACE((inode->traceflag|T_INODE, "Volume[%s].Key[%s] : INODE[%d]/R[%d] staled;%s\n", 
									volume->signature,
									inode->q_id,
									inode->uid, 
									inode->refcnt, 
									dm_dump_lv1(ibuf, sizeof(ibuf), inode)));

	if (INODE_GET_REF(inode) > 0) {
		INODE_LOCK(inode);
		dm_run_waitq(inode, (void *)NULL, ESTALE);
		INODE_UNLOCK(inode);
	}

	return;
}


/*
 * 	함수 호출의 결과
 *		- CLFU에서 대상 inode제거
 *		- VIT 테이블에서 제거
 *		- MDB에서 제거
 *
 * 주의) ob_priv = 1인 경우 VIT table과 CLFU에 등록되지는 않지만, 
 *   	 PARTITION/.private 경로아래 디스크 파일은 생성되어있음
 *		 
 */
int
nvm_isolate_inode_nolock(nc_volume_context_t *volume, fc_inode_t *inode)
{
    char        dbuf[1024];
	int			r = 0;



	if (inode->cstat != IS_ORPHAN) {

		/*
		 * VIT & CLFU에서 inode제거
		 */
		nvm_unregister_cache_object_nolock(volume, inode);
	}
#ifndef NC_RELEASE_BUILD
	else {
		DEBUG_ASSERT_FILE(inode, clfu_cached(&inode->node) == 0);
	}
#endif

	/*
	 * 추가: mdb에서도 지워줘야 새로운 meta정보 생성가능
	 */
	if (inode->rowid >= 0) {
		r = pm_remove_metainfo(inode);
		inode->rowid		= -1LL;
		TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u]/R[%d] meta info removal returned %s;%s\n",
					volume->signature,
					inode->q_id,
					inode->uid,
					inode->refcnt,
                   	((r >= 0)?"OK":"FAIL"),
                   	dm_dump_lv1(dbuf, sizeof(dbuf), inode)
                   	));
		/*
		 * r 이 0이라도 상관없음
		 */
	}
	inode->cstat 		= IS_ORPHAN;


	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u]/R[%d] isolation %s;%s\n",
					volume->signature,
					inode->q_id,
					inode->uid,
					inode->refcnt,
                   	((r >= 0)?"OK":"FAIL"),
                   	dm_dump_lv1(dbuf, sizeof(dbuf), inode)
                   	));


	return r;
}
fc_inode_t *
nvm_isolate_inode_with_key(nc_volume_context_t *volume, char *key)
{
    char        dbuf[1024];
	fc_inode_t	*inode = NULL;
	


	/*
	 * VIT 및 CLFU에서 제외
	 */
	inode = nvm_lookup_inode(	volume, 
								DM_LOOKUP_WITH_KEY, 
								key, 
								U_TRUE,  /* cache에서 제외*/
								U_FALSE,  /* refcnt 그대로 */
								U_FALSE,  /* DON't mark busy */
								NULL,
								__FILE__, 
								__LINE__);

	if (inode) {
		/*
		 * inode는 VIT및 CLFU에서 제거됨
		 */
		inode->cstat = IS_ORPHAN;

		/*
		 * 추가: mdb에서도 지워줘야 새로운 meta정보 생성가능
		 */
		if (inode->rowid >= 0)  {
			pm_remove_metainfo(inode);
			inode->rowid	 = -1LL;
			TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u]/R[%d] meta info removal returned;%s\n",
						volume->signature,
						inode->q_id,
						inode->uid,
						inode->refcnt,
       	            	dm_dump_lv1(dbuf, sizeof(dbuf), inode)
       	            	));
		}
	}

	return inode;
}


/*
 * 이 함수 호출의 전제
 * 		- CLFU cache에서 제거되어있어야함
 *		- VIT 에서 등록해제되어 있어야함
 *		- MDB 에서 제거되어 있어야함
 *	@umode : operation의 OR 조합
 *		VPM_FILE 
 *		VPM_MDB
 *
 */ 
int
nvm_purge_inode(nc_volume_context_t *volume, fc_inode_t *inode, int resetbusy, mdb_tx_info_t *txno)
{

	int 			e = 0;
	char			dbuf[1024];
	char			zuuid[128];
	char			ebuf[128];

	

	IC_LOCK(CLFU_LM_EXCLUSIVE);

	if (nvm_isolate_inode_nolock(volume, inode) < 0) {
		TRACE((T_ERROR, "Volume[%s].CKey[%s] - INODE[%u] ******* STOLEN, skipping;%s\n",
						volume->signature,
						inode->q_id,
						inode->uid,
						dm_dump_lv1(dbuf, sizeof(dbuf), inode)
						));
		IC_UNLOCK;
		return  -EINVAL;
	}
	if (resetbusy && ic_is_busy(inode)) {
		ic_set_busy(inode, U_FALSE, __FILE__, __LINE__);
	}

	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u] purging in progress;%s\n",
					volume->signature,
					inode->q_id,
					inode->uid,
					dm_dump_lv1(dbuf, sizeof(dbuf), inode)
					));
	if (INODE_BUSY(inode)) {
		TRACE((T_WARN|T_INODE, "Volume[%s].CKey[%s] - INODE[%u]/R[%d]/IOB[%d] in use\n",
						volume->signature,
						inode->q_id,
						inode->uid,
						INODE_GET_REF(inode),
						inode->iobusy
						));
		IC_UNLOCK;
		return -EBUSY;
	}


	IC_UNLOCK;

	if (inode->ob_validuuid != 0) {
		int		tflg = 0;

		e = pm_unlink_uuid(volume->signature, inode->q_id, inode->ob_priv != 0, inode->part, inode->uuid, inode->c_path);
		if (e < 0) {
			TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned %d:%s\n",
							volume->signature,
							inode->q_id,
							uuid2string(inode->uuid, zuuid, sizeof(zuuid)),
							-e,
							strerror_r(-e, ebuf, sizeof(ebuf))
							));
		}
		else {
			TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned OK\n",
							volume->signature,
							inode->q_id,
							uuid2string(inode->uuid, zuuid, sizeof(zuuid))
							));
		}

		inode->ob_validuuid = 0;
		inode->part			= NULL;

	}



    dm_free_inode(inode, U_TRUE/*recycle*/);

	return e;

}

void 
nvm_dump_all_pathlocks()
{
void glru_dump_all(glru_t *glru);
	glru_dump_all(g__path_cache);
};

struct inode_callback_info_tag {
	int				(*callback)(fc_inode_t *, void *);
	void			*cbdata;
	int				scanned; /* # of scanned count */
};
static int 
nvm_inode_callback_wrapper(void *key, void *value, void *ud)
{
	fc_inode_t						*inode = (fc_inode_t *)value;
	struct inode_callback_info_tag	*ici = (struct inode_callback_info_tag	*)ud;
	int								r;

	r = (*ici->callback)(inode, ici->cbdata);
	ici->scanned++;

	return (r>=0?HT_CONTINUE:HT_STOP);
}
int
nvm_volume_valid(nc_volume_context_t *v)
{
	int	valid = 0;

	valid = (v->magic == NC_MAGIC_KEY) &&
			nc_check_memory(v->object_tbl, 0) && 
			nc_check_memory(v->PL_tbl, 0);

	return	valid;
			
}

int
nvm_for_all_inodes(nc_volume_context_t *volume, int (*callback)(fc_inode_t *, void *), void *cbdata)
{
	struct inode_callback_info_tag	ici = {callback, cbdata, 0};


	u_ht_foreach(volume->object_tbl, nvm_inode_callback_wrapper, &ici);

	return ici.scanned;
}
#ifdef NC_MEASURE_PATHLOCK
/*
 * path-lock이 다양한 위치에서 호출되는데 각 위체에서 획득된 lock이
 * release될 떄까지 걸린 시간을 측정하고, 나중에
 * lock의 평균 유지 시간, 호출된 횟수 등으로 분석하기위해서 사용
 *
 */




struct tag_pls_t {
	char		*file;
	int			line;
	double		sum;
	double		vmin;
	double		vmax;
	double		vavg;
	long long	count;
};

static 	void 			*s__pls_tbl = NULL;
static  int 	 		s__pls_array_cnt = 0;
static  int 	 		s__pls_array_alloc = 0;
static  pls_t 	 		**s__pls_array = NULL;

/*
 * @f : source file name
 * @l : source line #
 * @msec : l@f에서 획득한 lock이 해제될 떄 까지 걸린 시간(msec)
 *
 * 소스에서 l@f에서 획득한 lock의 보유시간을 모두 합산
 */
static int
nvm_compare_pl_stat(const void *a, const void *b)
{
    pls_t   *pa = (pls_t *)a;
    pls_t   *pb = (pls_t *)b;
    int     r;

    r =  strcmp(pa->file, pb->file);
    if (r) return -r;
    return -1.0 * (pa->line - pb->line);

}
static pls_t *
nvm_lookup_pl_stat(char *f, int l)
{
	pls_t			key = {f, l, 0, 0};
	pls_t			**ap 	= NULL;
	pls_t			*found 	= NULL;

	pthread_mutex_lock(&s__pls_lock); 
	//DEBUG_ASSERT(pthread_spin_lock(&s__pls_lock) == 0);
	if (s__pls_tbl)
		ap = (pls_t **)tfind(&key, &s__pls_tbl, nvm_compare_pl_stat);

	if (ap) {
		found 		= *ap;
	}
	else {
		found 		= (pls_t *)malloc(sizeof(pls_t));
		found->file = f; /* symbol table에 static하게 할당된 스트링 */
		found->line = l;
		found->sum  = 0;
		found->count= 0;

		if (s__pls_array_cnt >= s__pls_array_alloc) {
			s__pls_array_alloc += 128;
			s__pls_array = realloc(s__pls_array, s__pls_array_alloc*sizeof(pls_t *));
		}
		s__pls_array[s__pls_array_cnt] = found;
		ap = (pls_t **)tsearch(found, &s__pls_tbl,  nvm_compare_pl_stat);
		s__pls_array_cnt++;
	}
	pthread_mutex_unlock(&s__pls_lock);
	//pthread_spin_unlock(&s__pls_lock);
	return found;
}
static void
nvm_update_pl_stat(pls_t *found, double msec)
{

	//pthread_spin_lock(&s__pls_lock);
	pthread_mutex_lock(&s__pls_lock); 
	DEBUG_ASSERT(msec >= 0);
	found->sum += msec;

	if (found->count == 0) {
		found->vmin =
		found->vmax = msec;
	}
	else {
		found->vmin = min(found->vmin, msec);
		found->vmax = max(found->vmax, msec);
	}
	found->count++;
	//pthread_spin_unlock(&s__pls_lock);
	pthread_mutex_unlock(&s__pls_lock);
}
static int
nvm_sort_by_holdtime(const void *a, const void *b)
{
	pls_t 	*pa = *(pls_t **)a;
	pls_t 	*pb = *(pls_t **)b;
	double 	r;

	r =  (pa->vavg - pb->vavg);
	if (!r) return r;

	return (int)(r > 0.0? 1:-1);
}
void
nvm_report_pl_stat()
{
	int		i ;
	char	zc[64];

	//pthread_spin_lock(&s__pls_lock);
	pthread_mutex_lock(&s__pls_lock);
	for (i = 0; i < s__pls_array_cnt; i++) {
		if (s__pls_array[i]->count > 0)
			s__pls_array[i]->vavg = (double)s__pls_array[i]->sum/(1.0 * s__pls_array[i]->count);
		else
			s__pls_array[i]->vavg = 0.0;
	}

	qsort(s__pls_array, s__pls_array_cnt, sizeof(pls_t *), nvm_sort_by_holdtime);

	TRACE((T_MONITOR, "%15s | %6s | %6s | %6s | %64s\n", "count", "avg", "min", "max", "source"));
	for (i = 0; i < s__pls_array_cnt; i++) {
		TRACE((T_MONITOR, "%15s | %6.2f | %6.2f | %6.2f | %s:%d\n",
						ll2dp(zc, sizeof(zc), s__pls_array[i]->count), 
						s__pls_array[i]->vavg, 
						s__pls_array[i]->vmin, 
						s__pls_array[i]->vmax, 
						s__pls_array[i]->file, 
						s__pls_array[i]->line));
		s__pls_array[i]->vavg = 0.0; 
		s__pls_array[i]->vmin = 0.0; 
		s__pls_array[i]->vmax = 0.0; 
		s__pls_array[i]->count= 0; 
						
	}
	//pthread_spin_unlock(&s__pls_lock);
	pthread_mutex_unlock(&s__pls_lock);

}
#endif

