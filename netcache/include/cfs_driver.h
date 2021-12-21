#ifndef __CFS_DRIVER_H__
#define __CFS_DRIVER_H__
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/statvfs.h>
#endif
#include <sys/stat.h>
#include <pthread.h>
#include <netcache_config.h>
#include "util.h"
#include "rdtsc.h"
#include "bt_timer.h"

/* CHG 2018-07-04 huibong Header 파일 상호 교차 include 수정 (#32238) */
/* #include <netcache.h> */

#ifndef howmany
#define howmany(x,y) 	(((x)+((y)-1))/(y))
#endif

#define MAX_DISKIO_VECTOR	10

#define	VOL_FC_PROLOG 		(1 << 0)
#define	VOL_FC_MTIME 		(1 << 1)
#define	VOL_FC_SIZE 		(1 << 2)
#define	VOL_FC_DEVID 		(1 << 3)
#define	VOL_FC_ALWAYSEXPIRE	(1 << 4)

typedef enum {
cfs_lock_exclusive = 0,
cfs_lock_shared = 1
} cfs_lock_t;


typedef enum {
	PLS_RESOURCE_IDLE = 0,
        PLS_RESOURCE_INPROGRESS=1
} nc_path_lock_state_t;
struct apc_open_context; 
struct cfs_origin_driver; 
struct nc_volume_context ;
typedef void * nc_origin_session_t;
struct nc_path_lock_tag {
	nc_uint32_t 				private; /* 1 for private path */
	char 						*path;
	struct nc_volume_context 	*volume;
	nc_uint64_t 				signature;

#ifdef PATHLOCK_USE_RWLOCK
	pthread_rwlock_t 			lock;
#else
	pthread_mutex_t 			lock;
#endif

#ifdef NC_RELEASE_BUILD
	nc_path_lock_state_t		state;
#else
	int							state;
#endif
	link_list_t					waiters;
	glru_node_t 				node;
#if defined(NC_MEASURE_PATHLOCK)
	perf_val_t					t_s;
	perf_val_t					t_e;
	void						*pls;
#endif
};

#define	USE_RWLOCK

struct tag_lru_root; 
struct tag_freshcheck_graph;
struct nc_volume_context {
	nc_uint32_t						magic;
	char							signature[128];
	nc_ulock_t						lock;

	nc_uint64_t						__barrier__;
	struct cfs_origin_driver		*osd;

	/*
	 *
	 * cache-key로 lookup가능한 캐시 객체의 테이블
	 */
	u_hash_table_t					*object_tbl;   /* object table(P.KEY= cache-id) */

	/*
	 * 해당 볼륨 범위의 path-lock 테이블
	 */
	u_hash_table_t 					*PL_tbl;

	/*
	 * nc_open을 호출할 때 마다, 대상 객체가 hit인지 miss인지 등의 정보를 
	 * 리턴
	 */
	void 							(*monitor)(void *cb, nc_hit_status_t hs);

	/* any other will be filled later */
	nc_int32_t						refcnt;
	nc_uint16_t 					writeback_blocks;
	nc_uint32_t 					adaptivereadahead:1; /* default 0(disabled) */


	nc_uint32_t 					preservecase:1; /* 대소문자 구분 여부 */
	nc_uint32_t 					alwaysexpire:1; /* 신선도 검사에 alwaysexpire존재*/

	/*
	 * 대상 볼륨의 원본/접근  상태
	 */
	nc_uint32_t 					freed:1; /* 1:더이상 접근할 수 없는 볼륨 */
	nc_uint32_t 					online:1;
	nc_uint32_t 					readable:1;
	nc_uint32_t 					writable:1;

#if USE_VM_STAT
	nc_uint16_t						cnt_getattr; /* # of calls of driver->getattr */
	nc_uint16_t						cnt_read; /* # of calls of driver->read */
	nc_uint16_t						cnt_write; /* # of calls of driver->write */
	nc_uint16_t						cnt_readdir; /* # of calls of driver->readdir */
	nc_uint16_t						cnt_mkdir; /* # of calls of driver->mkdir */
	nc_uint16_t						cnt_truncate; /* # of calls of driver->truncate */
	nc_uint16_t						cnt_rename; /* # of calls of driver->rename */
	nc_uint16_t						cnt_unlink; /* # of calls of driver->unlink */
	nc_uint16_t						cnt_rmdir; /* # of calls of driver->rmdir */
	nc_uint16_t						cnt_utimens; /* # of calls of driver->utimens */
	nc_uint16_t						cnt_open; /* # of calls of driver->open */
	nc_uint16_t						cnt_create; /* # of calls of driver->create */
	nc_uint16_t						cnt_close; /* # of calls of driver->close */
	nc_uint16_t						cnt_mknod; /* # of calls of driver->mknod */
	nc_uint64_t 					cnt_origin_bytes;
	nc_uint64_t 					cnt_client_bytes;
	bt_timer_t						t_activity_monitor;
#endif

	nc_uint32_t						version; 		/* caching version(age) */

#if 0
	float							avg_readsiz_1m; /* avg read size in every 1 min */
	nc_uint64_t						tot_readsiz_1m;
	nc_uint32_t						tot_readcnt_1m;
#endif
	/*
	 * ir # 31910: 신선도 검사를 위한
	 * 필드들
	 */


	char							*zpolicy;
	nc_uint16_t						fc_policy;
	char							fc_accepted[128];
	link_list_t 					fc_graph;;
	int 							fc_alwayscheck; 	/* 항상 check되는 항목 갯수*/
	/*
	 * TTLs
	 */
	int 							p_ttl; /* positive TTL in secs */
	int 							n_ttl; /* negative TTL in secs */

	//
	// nc_read_timeout
	int								to_ncread;

	//
	// volume pooling (2021.1.6 by weon)
	nc_time_t						pooledtime;
	link_node_t						node;
};


#if 1
#define 	VOLUME_REF(_m)		DEBUG_ASSERT(_ATOMIC_ADD((_m)->refcnt, 1)  > 0)
#define 	VOLUME_UNREF(_m)	DEBUG_ASSERT(_ATOMIC_SUB((_m)->refcnt, 1)  >= 0)
#else
#define 	VOLUME_REF(_m)			{ \
			int 	c_ = _ATOMIC_ADD((_m)->refcnt, 1); \
			TRACE((T_WARN, "VOLUME[%s] - REFed[%d] at %d@%s\n", (_m)->signature, c_, __LINE__, __FILE__)); \
		}
#define 	VOLUME_UNREF(_m)		{ \
			int 	c_ = _ATOMIC_SUB((_m)->refcnt, 1); \
			TRACE((T_WARN, "VOLUME[%s] - *UN*REFed[%d] at %d@%s\n", (_m)->signature, c_, __LINE__, __FILE__)); \
		}
#endif

#define 	VOLUME_INUSE(_m)	(GET_ATOMIC_VAL((_m)->refcnt)  > 0)
#define 	VOLUME_IDLE(_m)		(GET_ATOMIC_VAL((_m)->refcnt) == 0)

struct tag_nc_open_cc {
	link_list_t 	root;
};
typedef struct tag_nc_open_cc_quark {
	link_node_t		node;
	nc_cc_command_t	command;
	int				len;
	union {
		nc_int32_t		_cc_32int;
		nc_int32_t		_cc_32uint;
		char 			_data[1]; /* 가변 할당임 */
	} u;
} nc_open_cc_quark_t;


typedef unsigned long long 	cfs_size_t;
typedef long long			cfs_off_t;
typedef struct dev_file_handle {
	char 						*path;
	int 						mode;
	struct cfs_origin_driver	*driver;
	void 						*driver_data; /* driver-specific data (dynamically allocated) */
} dev_file_handle_t;
typedef struct cfs_iov {
	nc_uint64_t 		signature;	
	struct tag_fc_blk 	*block;
	char 		*buffer;
#ifdef __PROFILE
	perf_val_t 	_s, _e;
#endif
	nc_off_t	foff; 	/* file offset */
	int			off;	/* buffer size */
	int			len;	/* buffer size */
	int			filled; /* real read bytes */
	void *		cb; 	/* callback data key */
	int			wcnt;   /* write count */
} cfs_iov_t;
struct nc_volume_context; 


typedef void (*cfs_stream_read_done_handler_t)(cfs_iov_t *iov, int result);
typedef void (*cfs_io_callback_t)(cfs_iov_t *iov, int result);
typedef int (*cfs_dir_callback_t)(char *entry_name,  char *path, nc_stat_t *nstat, void *hint, int hint_len, nc_off_t off, void *cb);
typedef int (*cfs_update_hint_callback_t)(struct nc_volume_context *msc, char *path, void *hint, int hint_len);
typedef int (*cfs_update_result_callback_t)(struct nc_volume_context *msc, char *path, int errcode);
typedef struct cfs_origin_driver * (*cfs_load_class_proc_t)(void);
typedef int (*cfs_notifier_callback_t)(struct cfs_origin_driver *drv, int cmd, void *val, int len, struct nc_volume_context *mc);
typedef int (*cfs_io_validator_callback_t)(struct cfs_origin_driver *drv, struct tag_fc_inode_info *inode, nc_stat_t *stat);
typedef int (*cfs_io_property_updator_t)(struct tag_fc_inode_info *inode , nc_stat_t *stat);
typedef void (*cfs_io_origin_path_change_callback_t)(struct tag_fc_inode_info *inode, char *path);

/*
 * driver-neutral IOCTL commands
 */
#define 		IOCTL_SET_INODECACHE_COUNT 			1000
#define 		IOCTL_SET_INODECACHE_NEGTTL			1001
#define 		IOCTL_SET_INODECACHE_TTL			1002
#define 		IOCTL_SET_INODECACHE_PURGE			1003

/*
 * bind context type
 */
#define			NC_CT_ORIGIN		0
#define			NC_CT_PARENT		1

#define	CFS_DRIVER_REF(d_)		{	\
									_ATOMIC_ADD((d_)->refcnt, 1) ; \
									TRACE(((d_)->trace, "**REF:::DRIVER[%s] - refcnt[%d] at %d@%s\n", (d_)->signature, GET_ATOMIC_VAL((d_)->refcnt), __LINE__, __FILE__)); \
								}

#define	CFS_DRIVER_UNREF(d_)	{	\
									_ATOMIC_SUB((d_)->refcnt, 1) ; \
									TRACE(((d_)->trace, "UNREF::DRIVER[%s] - refcnt[%d] at %d@%s\n", (d_)->signature, GET_ATOMIC_VAL((d_)->refcnt), __LINE__, __FILE__)); \
								}
							

#define	CFS_DRIVER_IDLE(d_)			(GET_ATOMIC_VAL((d_)->refcnt) == 0)
#define	CFS_DRIVER_INUSE(d_)		(GET_ATOMIC_VAL((d_)->refcnt) > 0)
#define	CFS_DRIVER_VALID(d_)		((d_)->magic  == NC_MAGIC_KEY)



#define	NC_MAGIC_KEY				0x5CC5A55A

typedef struct cfs_origin_driver {
	nc_uint32_t						magic;
	char 							name[NC_MAX_STRINGLEN];
	char 							signature[NC_MAX_STRINGLEN];
	struct cfs_origin_driver		*devclass;
	nc_uint32_t						trace; 
	struct dev_file_handle *		(*open)(struct cfs_origin_driver *drv, char *path, void *hint, int hint_len, int mode, nc_xtra_options_t *);
	struct dev_file_handle *		(*create)(struct cfs_origin_driver *drv, char *path, int mode, void *hint, int hint_len, nc_xtra_options_t *);
	int 							(*read)(struct cfs_origin_driver *drv, nc_asio_vector_t *vector, nc_origin_session_t ctx);
	int 							(*write)(nc_asio_vector_t *vector);
	int 							(*mknod)(struct cfs_origin_driver *drv, char *path, mode_t mode, void *hint, int hint_len);
	int 							(*truncate)(struct cfs_origin_driver *drv, char *path, nc_size_t len);
	int 							(*ftruncate)(struct cfs_origin_driver *drv, struct dev_file_handle *, nc_size_t len);
	int 							(*mkdir)(struct cfs_origin_driver *drv, char *path, mode_t mode);
	int 							(*utimens)(struct cfs_origin_driver *drv, char *path, struct nc_timespec ts[]);
	int 							(*write_stream)(struct dev_file_handle *, char *buf, nc_size_t len);
	int								(*statfs)(struct cfs_origin_driver *drv, char *path, nc_statvfs_t  *s);
	int								(*close)(dev_file_handle_t *handle);
	/*
	 * 2.5 (2013.05.18)
	 */
	int								(*getattr)(struct cfs_origin_driver *drv, char *path, nc_stat_t *old_s, nc_stat_t *new_s, nc_xtra_options_t *kv, nc_mode_t, struct apc_open_context *oc);
	int								(*readdir)(struct cfs_origin_driver *drv, char *path, void *cb, cfs_dir_callback_t proc, cfs_off_t off, void *userdata);
	int								(*lioctl)(struct cfs_origin_driver *drv, int cmd, void *val, int vallen);
	int 							(*bind_context)(struct cfs_origin_driver *drv, char *prefix, nc_origin_info_t *ctx, int ctxcnt, int ctx_type);
	int 							(*unbind_context)(struct cfs_origin_driver *drv, int ctx_type);
	int 							(*set_notifier)(struct cfs_origin_driver *drv, cfs_notifier_callback_t cb, struct nc_volume_context *mc);
	struct cfs_origin_driver *		(*create_instance)(char *signature, cfs_update_hint_callback_t hint_proc, cfs_update_result_callback_t result_proc); 
	int 							(*destroy_instance)(struct cfs_origin_driver *driver);
	int 							(*lasterror)(void);
	int 							(*set_lasterror)(int);
	int								(*unlink)(struct cfs_origin_driver *drv, char *path, nc_xtra_options_t *req_option, nc_xtra_options_t **res);
	int								(*link)(struct cfs_origin_driver *drv, char *path1, char *path2);
	int								(*rmdir)(struct cfs_origin_driver *drv, char *path);
	int								(*rename)(struct cfs_origin_driver *drv, dev_file_handle_t *fi, char *path1, char *path2);
	int								(*flush)(struct tag_fc_inode_info *handle);
    //int 							(*online)(struct cfs_origin_driver *drv, int ioflag);
	int								(*iserror)(int code);
	int								(*issamecode)(int cachedcode, int newcode);
	nc_origin_session_t 	 		(*allocate_context)(struct cfs_origin_driver *drv, struct apc_open_context *aoc); 
	int 							(*valid_context)(struct cfs_origin_driver *drv, nc_origin_session_t ctx, char *path, nc_off_t *off, nc_size_t *len);
	void 							(*free_context)(struct cfs_origin_driver *drv, nc_origin_session_t ctx);
	int                             (*set_read_range)(struct cfs_origin_driver *drv, nc_origin_session_t ctx, nc_int64_t offset, nc_int64_t size);
	char * 							(*dump_session)(char *, int, void *);

	/*
	 * 2017.8.9 added by weon
	 */
	void 							(*unload)(void); /* 드라이버 모듈 전체가 unload되기 전에 실행해야할 작업 호출 */
	/*
	 * 2.7 : ir #25884 rwlock 추가
	 */
	pthread_rwlock_t 				*rwlock;



	struct nc_volume_context 		*mc;
	nc_origin_info_t 				*ctx;
	cfs_update_hint_callback_t 		hint_proc;
	cfs_update_result_callback_t 	result_proc;
	char 							prefix[256];
	char 							s_encoding[128];
	char 							l_encoding[128];
	int 							preservecase;
	int								shutdown;
	int								online;
	int								refcnt;
	int								prefix_len;
	int 							ctx_cnt; /* # of context for a volume point */
	void 							*driver_data;	/* driver specific data here */
} cfs_origin_driver_t;
#define	CFS_DRIVER_PRIVATE(_cfs)		((_cfs)->driver_data)



cfs_origin_driver_t * cfs_create_driver_instance(char *class, char *signature);
cfs_origin_driver_t * cfs_find_driver_class(char *class);
int cfs_destroy_driver_instance(cfs_origin_driver_t *drv);
cfs_origin_driver_t * cfs_load_driver(char *name, char *path);

int cfs_is_online(struct cfs_origin_driver *drv, int ioflag);
int cfs_read_vector(nc_volume_context_t *mc, nc_asio_vector_t *vector, nc_origin_session_t session);
int cfs_getattr(nc_volume_context_t *mc, char *path , nc_stat_t *old, nc_stat_t *s, nc_xtra_options_t *kv, nc_mode_t mode, struct apc_open_context *);
int cfs_getattr_apc(nc_volume_context_t *mc, char *path , nc_stat_t *old, nc_stat_t *s, nc_xtra_options_t *kv, nc_mode_t mode, nc_origin_session_t session, struct apc_open_context *oc);
int cfs_readdir(	nc_volume_context_t 	*mc, cfs_origin_driver_t *drv, char *path, void *cb,  nc_fill_dir_proc_t dir_proc, cfs_off_t offset, void *ud );
int cfs_errno(	cfs_origin_driver_t *drv);
/* 0-th: access time, 1st: modification time */
int cfs_utimens(nc_volume_context_t *mc, char *path, struct nc_timespec tv[2]);
int cfs_write_vector(nc_volume_context_t *mc, nc_asio_vector_t *vector);
int cfs_mkdir(nc_volume_context_t *mc, char *path, mode_t mode); 
int cfs_mknod(nc_volume_context_t *mc, char *path, mode_t mode, void *hint, int hint_len); 
int cfs_unlink(nc_volume_context_t *mc, char *path, nc_xtra_options_t *req, nc_xtra_options_t **res);
int cfs_rmdir(nc_volume_context_t *mc, char *path);
int cfs_truncate(nc_volume_context_t *mc, char *path, nc_size_t len);
int cfs_ftruncate(nc_volume_context_t *mc, struct dev_file_handle *dh, nc_size_t len);
int cfs_rename(nc_volume_context_t *mc, dev_file_handle_t *fi, char *src, char *dest);
struct dev_file_handle * cfs_open(nc_volume_context_t *mc, char *path, void *hint, int hint_len, int mode, nc_xtra_options_t *);
struct dev_file_handle * cfs_create(nc_volume_context_t *mc, char *path, int mode, void *hint, int hint_len, nc_xtra_options_t *);
int cfs_close(nc_volume_context_t *mc, dev_file_handle_t *handle);
int cfs_flush(nc_volume_context_t *mc, struct tag_fc_inode_info *inode);
int cfs_statfs(nc_volume_context_t *mc, char *path, nc_statvfs_t *fs);
void cfs_unload_all();
int cfs_is_online_x(char *qr_path, int ioflag);
int cfs_set_notifier(struct cfs_origin_driver *drv, cfs_notifier_callback_t cb, struct nc_volume_context *mc);
int cfs_set_io_validator(struct cfs_origin_driver *drv, cfs_io_validator_callback_t cb);
int cfs_ioctl(nc_volume_context_t *mc, int cmd, void *val, int vallen);
int cfs_bind_context(nc_volume_context_t *mc, char *prefix, nc_origin_info_t *ctx, int ctxcnt);
int cfs_unbind_context(nc_volume_context_t *mc);
int cfs_set_lasterror(nc_volume_context_t *mc, int v);
int cfs_bind_context_x(nc_volume_context_t *mc, char *prefix, nc_origin_info_t *ctx, int ctxcnt, int ctxtype);
int cfs_unbind_context_x(nc_volume_context_t *mc, int ctxtype);
int	cfs_iserror(nc_volume_context_t *mc, int code);
int	cfs_issameresponse(nc_volume_context_t *mc, int c1, int c2);
nc_origin_session_t  cfs_allocate_session(nc_volume_context_t *mc, struct apc_open_context *);
int cfs_valid_session(nc_volume_context_t *mc, nc_origin_session_t ctx, char *path, nc_off_t *off, nc_size_t *len);
void cfs_free_session(nc_volume_context_t *mc, nc_origin_session_t ctx);
void cfs_set_trace(nc_volume_context_t *mc, int onoff);
char *cfs_dump_session(nc_volume_context_t *, char *, int l, void *d);
int cfs_set_read_range(nc_volume_context_t *mc, nc_origin_session_t ctx, nc_int64_t offset, nc_int64_t length);
int cfs_lock_driver(cfs_origin_driver_t *drv, cfs_lock_t shared);
int cfs_unlock_driver(cfs_origin_driver_t *drv, cfs_lock_t shared);

#endif /* __CFS_DRIVER_H__ */
