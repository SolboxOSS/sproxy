/** @file netcache.h */
#ifndef __NETCACHE_H__
#define __NETCACHE_H__
#include <netcache_config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include "netcache_types.h"
#include "rdtsc.h"
#include "hash.h"
#if defined(NETCACHE)
#include "bitmap.h"
#endif
#include "lock.h"
#include "trace.h"
#include "util.h"
#define		LRUW_MAGIC	0xA55A
#include "cyclic_lfu.h"
#include "glru.h"
#include "bt_timer.h"
#include "cfs_driver.h"
#include "snprintf.h"



/*
 * LRU/Cyclic-LFU 구조체의 scan의 정상 허용 범위 
 * scan 횟수 - 이 이상의 scan이 발생하면 비정상적으로 본다
 *
 */
#define 	NC_NODESCAN_THRESHOLD		50

#define 	KBYTES			*1024
#define		MBYTES			*(1024*1024)

/*
 *
 * PURGE OBJECT TYPE
 *
 */
#define	VPM_FILE 		(1<<0)
#define	VPM_MDB			(1<<1)

#define		NC_PAGE_SIZE			g__page_size
#define 	NC_CACHE_DIR_COUNT		256
#define		NC_ALIGN_SIZE	pm_get_align_size()


#define		NC_READAHEAD_BLOCKS		5

#define 	BLOCK_NULL 				(0)
#define   	BLOCK_UID(__blk__) 		((__blk__ == NULL)?-1:((__blk__)->buid))

#define 	IL_SET_PROGRESS_IDLE 		(0)
#define 	IL_SET_PROGRESS_SHARED 		(1 << 0)
#define 	IL_SET_PROGRESS_EXCLUSIVE 		(1 << 1)
#define 	IL_SET_PROGRESS_MANAGEMENT 		(1 << 2)
#define		IL_SET(_b, _v)				((_b & _v) != 0)



#define  	BETWEEN(_m, _M, _v) 	(((_v) >= (_m)) && ((_v) <= (_M)))

#define		VOLUME_VERSION_NULL		0xFFFFFFFF



#define	INODE_SIZE_DETERMINED(_i)		((_i)->ob_sizeknown) 
#define	INODE_SIZE_DECLED(_i)			((_i)->ob_sizeknown && (_i)->ob_sizedecled) 
#define	INODE_VALID_TO_READ(_i)			((_i)->ob_staled == 0)


#ifdef WIN32
#define 	DM_NULL_FD 	INVALID_HANDLE_VALUE
#define 	DM_CLOSE_FD(fd_)   if (fd_ != DM_NULL_FD) { dio_close_file(fd_); fd_ = DM_NULL_FD; }
#define 	DM_FD_VALID(_h) 	((_h) != NULL && (_h) != INVALID_HANDLE_VALUE)

#else
#define 	DM_NULL_FD 	 -1
#define 	DM_CLOSE_FD(fd_)   if (fd_ != DM_NULL_FD) { dio_close_file(fd_); fd_ = DM_NULL_FD; }
#define 	DM_FD_VALID(_h) 	(((int)_h) >= 0)
#endif

/*
 * 대상 블럭이 디스크에 저장되어있고, 접근가능한지 여부
 */
#define		DM_CHECK_RBOD(i_, b_)	(DM_FD_VALID(i_->fd) && (test_bit(b_, i_->bitmap) != 0))

#define 	PM_CHECK_LOW 				1
#define 	PM_CHECK_HIGH 				2


#define		IC_LOCK(lm_)			{ \
				LO_CHECK_ORDER(LON_INODE); \
				LO_PUSH_ORDER_FL(LON_INODE,__FILE__, __LINE__); \
				clfu_lock(g__inode_cache, lm_, __FILE__, __LINE__); \
		}
#define		IC_UPGRADE				clfu_upgradelock(g__inode_cache, __FILE__, __LINE__)
#define		IC_UNLOCK				{ \
			clfu_unlock(g__inode_cache, __FILE__, __LINE__); \
			LO_POP_ORDER(LON_CLFU_INODE); \
		} 

typedef struct {
	nc_int64_t	decade;
	nc_int64_t	txno;
} mdb_tx_info_t;

#ifdef NC_MEASURE_PATHLOCK
struct tag_pls_t; 
typedef struct tag_pls_t pls_t ;
#endif
#ifdef NC_MEASURE_CLFU
struct tag_bl_t; 
typedef struct tag_bl_t bl_t ;
#endif
typedef struct tag_blk_apc_read_info blk_apc_read_info_t;

typedef enum {
	DM_BLOCK_FAULT 		= 0,
	DM_BLOCK_ONMEMORY 	= 1,
	DM_BLOCK_ONDISK		= 2,
	DM_BLOCK_IOPROGRESS	= 3
} fc_block_status_t;
typedef enum {
	CS_FRESH		= 0,	/* 규약에 따라 신선하다고 가정*/
	CS_NEED_REVAL	= 1,	/* 서빙 전에 원본에게 확인 필요*/
	CS_MARK_STALE	= 2		/* 원본에 물을 필요없이 유효기간 만료로 간주*/
} fc_cache_status_t;


typedef struct tag_expire_info {
	DIR		*cursor;			/* opendir*/
	long	expired;			/* # of expired files */
	time_t	started;			/* expiration starting time */
	time_t	lastrun;			/* last expiration time */
} expire_info_t;
typedef struct tag_memmgr_heap {
	nc_int64_t	heapsize;
	nc_int64_t	heapalloc;
	nc_int64_t	heapalloc_aligned;
} memmgr_heap_t;
typedef struct tag_bci_stat {
	fc_blk_t				*block;
	fc_block_status_t		bci;
} bci_stat_t;


#define 	KV_VER_2
#define		KV_HASH_SIZE			0x40
typedef struct tag_nc_kv_list {
	nc_uint32_t 				magic;
#ifdef KV_VER_2
	link_list_t 				*root[KV_HASH_SIZE]; /* char를 0x3F로 해시해서 root 선택*/
#else
	link_list_t 				root;
#endif
	int 						raw_result; /* property가 원본에서 응답으로 만들어 진 경우 code값 */
	nc_time_t 					pttl; /* 0이면 무시 */
	nc_time_t 					nttl; /* 0이면 무시*/
	char 						*opaque_command;
	char 						*stat_keyid; /* NULL이 아닌 경우 속성 캐시의 key값 */
	nc_size_t 					opaque_data_len;
	struct tag_nc_ringbuffer 	*oob_ring;
	void 						*opaque;
	void						*client; /* tproxy 기능에 필요한 client의 정보 */
	nc_size_t					client_data_len;
	void 						*pool_cb;
	void 						*(*pool_allocator)(void *cb, size_t sz);
	int 						(*oob_callback)(nc_xtra_options_t *kv, void *);
	void 						*oob_callback_data;

	int 						custom_allocator;
#ifdef KV_TRACE
	int 						line;
	char						*file;
	link_node_t 				node;
#endif
} nc_kv_list_t;

typedef struct tag_nc_kv_node {
	char 			*key;
	char 			*value;
	int				ignore; 	/* 1로 설정되면 해당 키는 kv_for_each()에서 무시 */
	link_node_t 	node;
} nc_kv_t;

struct tag_fc_inode_info ;
struct tag_fc_stream_info ;


typedef struct tag_nc_file_ref {
	nc_uint32_t 				magic;
	nc_volume_context_t 		*volume;
	nc_path_lock_t				*lock;
	struct tag_fc_stream_info 	*stream; /* non-buffered writing용 */
	struct tag_fc_inode_info 	*inode;
	nc_kv_list_t 				*list; /*extended key-value list if any*/
	nc_mode_t 					mode;
} fc_file_t;
typedef struct tag_fc_stream_info {
	int     property_injected;
  	void	*session;
	char 	buffer[8192];
	char 	buffered;
} fc_stream_t;



#if 1
#define		INODE_REF(i_) 			_ATOMIC_ADD((i_)->refcnt, 1)
#define		INODE_UNREF(i_) 		_ATOMIC_SUB((i_)->refcnt, 1)
#else
#define		INODE_REF(i_) 			TRACE((T_WARN, "%s/%s:: REFed[%d] at %d@%s\n", \
													i_->volume->signature,  \
													i_->q_id,  \
													_ATOMIC_ADD((i_)->refcnt, 1), \
													__LINE__, \
													__FILE__)); 
#define		INODE_UNREF(i_) 		TRACE((T_WARN, "%s/%s:: **UN**REFed[%d] at %d@%s\n", \
													i_->volume->signature,  \
													i_->q_id,  \
													_ATOMIC_SUB((i_)->refcnt, 1), \
													__LINE__, \
													__FILE__)); 
#endif

#define		INODE_GET_REF(i_) 		_ATOMIC_VAL((i_)->refcnt)
#define		INODE_BUSY(i_)			((INODE_GET_REF(i_) > 0) || ((i_)->iobusy != 0) || ((i_)->inodebusy != 0))


#pragma pack(8)

#define 	INODE_HDR_VERSION_V30 		7
typedef struct tag_nc_mdb_handle nc_mdb_handle_t;

#define		PS_ROK			(1 << 0)
#define		PS_WOK			(1 << 1)
typedef struct tag_part_element {
	char			*path; /* partition path */
	dev_t			devid;
	float			fs_usedper; /* used percent */
	size_t			fs_bsize;
	size_t			fs_bfree;
	int				weight;
	int				valid;
	int				ios;
	int				r_s; /* range start */
	int				r_e; /* range end */
	link_node_t 	node;
	nc_mdb_handle_t		*mdb;
	int 				needstop;
	int 				stopped;
	pthread_t			reclaimer;
	pthread_rwlock_t 	rwlock;

} nc_part_element_t;

typedef struct tag_fc_header_info_v30 fc_header_info_v30_t;
typedef enum { IS_FREED = 0, IS_ORPHAN = 1, IS_CACHED = 2} ic_cache_state_t;
typedef nc_uint32_t 	nc_crc_t;
//#ifndef offsetof
//#define	offsetof(t_, m_)		((size_t)&((t_ *)0)->m_)
//#endif

#define		INODE_RESET(_i) 	{ \
	/*			\
	 * inode->uid는 reset되면 안됨 \
	 */ \
	offsetof(struct tag_fc_inode_info, __barrier__);  \
	memset(  &(_i)->__barrier__, 0,  sizeof(struct tag_fc_inode_info) - offsetof(struct tag_fc_inode_info, __barrier__) );  \
  	(_i)->fd 			= DM_NULL_FD;\
  	(_i)->rowid 		= -1LL;\
  	(_i)->cstat			= IS_ORPHAN;\
}


#ifdef EXPERIMENTAL_USE_SLOCK
#define 	INODE_SPINLOCK_TRANSACTION(in_)		for (int i__ = 0;  \
												(i__ == 0) && pthread_spin_lock(&(in_)->slock) == 0; \
												pthread_spin_unlock(&(in_)->slock), i__ = i__+1)
#else
#define 	INODE_SPINLOCK_TRANSACTION(in_)		

#endif


#ifdef WIN32
#include <win_uuidwrap.h>
#else
#include <uuid/uuid.h>
#endif


/*
 * ir #28261 : opaque 정의 추가
 */
typedef struct tag_opaque {
	nc_uint32_t	fmagic;
	int 	len; 	/* opaque length */
	union {
		nc_uint8_t 		_b[8];
		nc_uint64_t 	_b64;
		nc_uint32_t 	_b32;
		nc_uint16_t 	_b16;
		void 		*   _bp;
	} u;
#define		ub 		u._b
#define		ub64 	u._b64
#define		ub32 	u._b32
#define		ub16 	u._b16
#define		uptr 	u._bp
	nc_uint32_t	rmagic;
} nc_opaque_t;




/** inode_info */
typedef struct tag_fc_inode_info {

  	nc_uint32_t						uid; 				/* unique id for tracing */
#ifdef EXPERIMENTAL_USE_SLOCK
	/*
	 * mutex를 사용하지않고 작은 오퍼레이션의 순차화만을 위해서
	 * sizeof(spinlock) = 4 bytes
	 * 사용 (2019.10.25 by weon)
	 */
	pthread_spinlock_t				slock;				/* fast spin lock:spinlock내에서 lock, sleep, IO등 절대 사용말것 */
#endif
	link_node_t						nav_node; 			/* 생성된 inode 추적용 */
	pthread_mutex_t					lock;
	nc_int64_t						rowid;				/* MDB unique ID */
	bt_timer_t						t_origin;			/* origin expiration timer */
	bt_timer_t						t_defer;			/* deferring timer */
	nc_uint64_t						__barrier__;


	nc_volume_context_t * 			volume;		
  	void *							driver_file_data;	/**<driver/device specific IO를 수행할 때 필요한 정보 블럭에 대한 포인터 */
	nc_part_element_t *				part;				/**<디스크 캐싱모드일 때, 대상 디스크 파티션, NULL when in memory only or write mode */
	char							*c_path;
	char *							q_id;				/* 객체의 key */
	char *							q_path;				/* 객체 URL중 경로 part */
	uuid_t							uuid;				/* disk캐싱 객체에 대해서만 uuid생성. 이 값을 이용해서 cache file명 생성*/
	nc_uint64_t 					hid;				/* hash id(MDB 저장시 locality(결국 성능) 향상용) */
	nc_uint32_t 					doffset;			/* 캐시 파일 내에서 실제 객체 데이타가 시작되는 옵셋*/
	/*
	 * inode의 blockmap의 할당된 갯수
	 */
	nc_uint32_t 					mapped;
	/* adaptive read-ahead factors */
#ifdef NC_ADAPTIVE_READAHEAD
	nc_int16_t 						current_nra;
	nc_int16_t 						current_dra;
	float 							missratio;
	nc_uint64_t 					totalhit; 		/* # of block io */
	nc_uint64_t 					totalmisshit; 		/* # of block loaded but not used */
#endif

  	nc_uint64_t						signature;

	/*
	 * object properties
	 */

	nc_mode_t						imode;			/**<객체의 read/write등 현재 open mode */
  	nc_time_t      					atime;			/**last access time */
  	nc_time_t      					vtime;			/**<cache 가 유효한 시간, xfv=1이면 무조건 이후에 삭제됨 */
  	nc_time_t      					ctime; 			/**<cache 생성 시간*/
  	nc_time_t      					mtime;			/**<last update time */
  	nc_size_t      					size;			/**<객체의 크기 (단위:바이트)*/
  	nc_size_t      					rsize;			/**<객체의 실제크기 (단위:바이트)*/
	nc_devid_t 						devid;			/**<특정 deivce에 저장된 객체의 ID */
	nc_obitinfo_t					obi;			/* 각각의 속성 bitwise정보 */

	nc_int32_t 						origincode; 	/**<원본에서 제공한 응답 코드 */
	nc_uint32_t 					viewcount;		/**<캐싱된 이후 view count (즉,  open 횟수)*/
	nc_xtra_options_t 				*property;
	/*
	 * end of object properties
	 */
  	nc_blkno_t     					maxblkno;
  	nc_uint32_t						cversion; 		/* ir #24803 : caching version */
  	nc_int32_t						bitmaplen; 		/* bit count :number of blocks*/
  	nc_int32_t 						header_size; 	/* header size in bytes */
  	nc_int16_t 						refcnt; 		/* no cache */
#ifdef NC_ENABLE_CRC
	nc_int8_t 						crc_errcnt;		/* crc error count */
#endif
  	nc_int32_t						traceflag;  		/* NEW)2020-2-12 by weon, inode추적*/
	nc_int8_t 						hdr_version:4;
	nc_int8_t						blk_locked:1;
	nc_int8_t						iobusy:1;			/* NEW) 2020-2-10 by weon 1 if io busy*/
	nc_int8_t						staleonclose:1;		/* NEW) 2021-2-22 by weon 1 stale처리 필요*/
	nc_int8_t						inodebusy;			/* 1:현재 대상객체에 대한 비동기 open 진생 중, reclaim하면 안됨 */
  	nc_uint8_t      				writable;			/* >= 1 if the file opened with write mode*/
  	nc_uint32_t						headerversion;  	/* monotonic increase by 1 */
  	nc_uint32_t						contentversion; 	/* the same as above */
  	nc_uint32_t						mdbversion; 	 	/* meta 정보가 바뀔때마다 1씩 증가*/

  	unsigned long 					*bitmap; 		/* cached block information, size of (unsigned long) depends on platform arch */
  	unsigned long 					*pbitmap; 		/* cached block information, size of (unsigned long) depends on platform arch */
	nc_blkno_t						*LPmap;			/* Logical-Physical block map */

  	fc_blk_t 						**blockmap;  	/* 65535 * 256KB => more than 120GB */
#ifdef NC_ENABLE_CRC
	nc_crc_t 						*blockcrc; 		/* 1-byte block CRC */
	nc_uint32_t 					crcsize;
#endif



	/*
	 *********************************************
	 * Cache-Manager maintained fields
	 *********************************************
	 */
#if NOT_USED
  	nc_int32_t 						progress;			/* CLFU 작업중인 경우 +1, reclaim하면 안됨을 표시*/
#endif

	nc_fio_handle_t					fd;

  /*  buffer cache management */
  	lru_node_t						node;



	nc_uint64_t 					disksize; /* 디스크 객체 크기*/
	nc_time_t 						lastsynctime; /* 마지막으로 동기된 시점 */

	nc_path_lock_t					*PL;






	/*
	 * progress lock 디버깅
	 */
#if defined(NC_DEBUG_PROGRESS)
	char 				pl_path[128];
	int 				pl_line;
	long				pl_owner;
	int					pl_sig;
#endif
	nc_int64_t						fr_availsize; /* non-rangeable IO중에 현재까지 가져온 크기 */
	nc_origin_session_t				origin;
	nc_uint8_t						cstat; /* cache stat */

	link_list_t						pending_ioq; /* 대상 객체에 현재 진행된는 IO(ASIO) 들의 list*/
	unsigned long					*whint_map; /* wait hint */
	link_list_t						ctx_waitq; /*nc_read요청 중 IO완료를 대기하는 대기열 */
	nc_uint32_t						ctx_waitq_age;
} fc_inode_t;


typedef enum {
	APC_OS_INIT = 0,
	APC_OS_QUERY_FOR_MISS=1,
	APC_OS_QUERY_FOR_REFRESHCHECK=2,
	APC_OS_WAIT_FOR_COMPLETION=3,
	APC_OS_FREE=4
} apc_open_state_t;
typedef int (*apc_completion_callback_t)(void *userdata, int error);

typedef struct apc_remove_context {
	nc_apcop_t 		type; 			
	nc_int32_t		hard_remove;		/* 1: mdb record 제거 & 캐시파일 삭제, 0이면 메모리 free만*/	
	fc_inode_t		*inode;
} apc_remove_context_t;

typedef struct apc_open_context {
	nc_apcop_t 				type; 			

	/*
	 * input params given from application
	 */
	nc_volume_context_t		*volume;
	char 					*origin_path;
	char        			*cache_key_given;
	char        			*cache_key;
	nc_mode_t   			mode;
	fc_inode_t  			*inode;
	fc_inode_t  			*inodetofree;
	nc_open_cc_t			*open_cc; /* 일반적인 경우 NULL */

	nc_uint32_t 			need_create_vary_meta:1;
	int         			open_result;
	nc_uint32_t     		force_nocache:1;
	nc_uint32_t				force_private:1;
	nc_opaque_t 			opaque;
	nc_xtra_options_t       *in_prop;
	nc_apc_open_callback_t	callback; 
	void 					*callback_data;


	/*
	 * data structure used in operation
	 */
	union {
		nc_uint32_t		_u32;
		struct {
			nc_uint32_t    			vary:1;			/* URL should be vary */
			nc_uint32_t    			stalled:1;
			nc_uint32_t         	keychanged:1;
			nc_uint32_t         	queried:1;
			nc_uint32_t         	offcache:1;
			nc_uint32_t         	activating:1;
		} _ubi;
	} u;
	nc_origin_session_t		origin; /* origin 요청이 필요할 때 할당됨 */
	nc_uint64_t 			id;
	apc_open_state_t		state;
	bt_timer_t				t_hang;
	nc_path_lock_t			*pathlock;

	struct {
		nc_stat_t 			stat;	/* filled from driver */
		int 				result; /* filled from driver */
	} query_result;

	apc_overlapped_t		*overlapped; 
	link_node_t				node;
} apc_open_context_t;
typedef struct tag_close_context  {
	nc_apcop_t 			type; 			
	fc_inode_t			*inode;
	nc_time_t			firstattempt; /* close 시도가 시작된 시점*/
} close_context_t;
typedef struct tag_dio_done_context {
	nc_apcop_t 			type; 			
	nc_asio_vector_t	*asio;
	int					bidx;
	nc_off_t			offset;
	size_t				transfered;
	int					error;
} dio_done_context_t;


typedef struct tag_nio_done_context {
	nc_apcop_t 			type; 			
	nc_asio_vector_t	*asio;
	int					bidx;
	nc_off_t			offset;
	size_t				transfered;
	unsigned long		error;
} nio_done_context_t;




typedef struct apc_read_context {
	nc_apcop_t 				type; 			
	/*
	 * input  params
	 */
	nc_uint32_t				fmagic;
	//char				    *buffer;
	nc_off_t				foffset;
	nc_size_t				len;
	nc_xtra_options_t 		*inprop;

	/*
	 * working
	 */
	char					*cursor;
	int 					remained;
	int 					error;
	nc_uint32_t				age;
	nc_blkno_t				wanted;

	fc_inode_t 				*inode;
	nc_uint64_t				signature;

	nc_origin_session_t		origin;
	bt_timer_t				t_hang;


	apc_block_ready_callback_t	cb_complete; 
	void						*cb_data;
	link_node_t				node;
	nc_uint32_t				rmagic;
} apc_read_context_t;
/* disk meta INFO */
typedef struct tag_nc_mdb_inode_info {
	nc_uint16_t 		magic;
	uuid_t				uuid;
	nc_int64_t			rowid;
	nc_uint64_t			hid;
	nc_int64_t			size;
	nc_int64_t			rsize; /* real data size */
	nc_mode_t			mode;
	nc_uint8_t 			casesensitive;
	nc_time_t			ctime;
	nc_time_t			mtime;
	nc_time_t			vtime;
	nc_time_t			xtime;
	nc_devid_t			devid;
	nc_uint32_t			viewcount;
	nc_int32_t			viewindex;
	nc_uint16_t 		origincode; 	/*원본에서 제공한 응답 코드 */
	nc_uint32_t 		cversion; 		/* ir #24803 : caching version */

	nc_obitinfo_t		obi;
#if 0
	union {
		nc_uint64_t 	m_obits_s;
		struct {
			nc_uint64_t 		rangeable:1;
			nc_uint64_t 		chunked:1;
			nc_uint64_t			vary:1;
			nc_uint64_t			priv:1;
			nc_uint64_t      	complete:1; 	/* 1 if all blocks are cached */
			nc_uint64_t      	mustreval:1; 	/* 항상 원본에 체크 */
			nc_uint64_t      	sizeknown:1; 	/* 객체크기가 알려진경우,  norandom=1인경우, io가 끝나야 알 수 있음 */
			nc_uint64_t      	sizedecled:1; 	/* 객체크기가 알려진경우,  norandom=1인경우, io가 끝나야 알 수 있음 */
			nc_uint64_t      	nocache:1; 	/* */
		} m_obits;
	} u;
#endif
	char				*vol_signature;
	char				*q_path;
	char				*q_id;
	nc_part_element_t	*part;
	nc_xtra_options_t 	*property;
} nc_mdb_inode_info_t; 

struct purge_info {
	char				*vol_signature;
	char 				*path;
	char 				*key;
	nc_int64_t 			rowid;
	uuid_t 				uuid;
	nc_obitinfo_t		obi;
	nc_uint32_t			cversion;
	int					delok;
};


#define		DM_UPDATE_METAINFO(i_, xp_)	{xp_; dm_need_mdb_update(i_, "");}



#define	MEM_FREE_CLEAR(h_, p_, b_)		if (p_) { mem_free(h_, p_, b_, 0, __FILE__, __LINE__); (p_) = NULL;}
#define	MEM_FREE(h_, p_, b_)			if (p_) mem_free(h_, p_, b_, 0, __FILE__, __LINE__)



int mdb_purge_rowid(nc_mdb_handle_t *mdb, int ishard, int iskey, uuid_t uuid, nc_int64_t rowid, mdb_tx_info_t *txno);
void mdb_get_counter(nc_uint64_t *r, nc_uint64_t *w, nc_uint64_t *m);
int mdb_get_primary(char *buf);
void mdb_close_lru_cursor(void *v_cursor);
void * mdb_prepare_lru_cursor(nc_mdb_handle_t *mdb);
int mdb_get_lru_entries(nc_mdb_handle_t *mdb, struct purge_info *pi, int maxcount);
nc_mdb_inode_info_t ** mdb_cursor_entries(void *v_cursor, int maxcount, int *returned);
nc_mdb_handle_t *mdb_open(nc_part_element_t *part, char *partition);
void mdb_close(nc_mdb_handle_t *tdb);
int mdb_check_status(char *buf, int len);
int mdb_wait_update_done(nc_mdb_handle_t *mdb, nc_uint64_t myseq);
int mdb_update(nc_mdb_handle_t *mdb, fc_inode_t *inode, int binsert, mdb_tx_info_t *txno);
int mdb_remove(nc_mdb_handle_t *mdb, nc_uint64_t, int casepreserve, char *signature, char *path, nc_uint32_t, uuid_t);
int mdb_remove_uuid(nc_mdb_handle_t	*mdb, uuid_t, nc_int64_t rowid);
int mdb_remove_uuid_direct(nc_mdb_handle_t	*mdb, uuid_t, nc_int64_t rowid);
int mdb_remove_tx(nc_mdb_handle_t *mdb, nc_uint64_t, int casepreserve, char *signature, char *path, nc_uint32_t, uuid_t, mdb_tx_info_t *txno);
int mdb_reuse(nc_mdb_handle_t *mdb, uuid_t uuid, nc_int64_t rowid, nc_time_t vtime, nc_uint32_t nver);
nc_mdb_inode_info_t * mdb_load(nc_mdb_handle_t *mdb, int casepreserve, char *signature, char *path);
int mdb_invalidate(nc_mdb_handle_t *mdb, int preservecase, char *signature, char *qid);
int mdb_purge(	nc_volume_context_t *volume, 	
				nc_mdb_handle_t *mdb, 
				char *pattern, 
				int ishard, 
				int iskey, 
				int (*postaction)(nc_volume_context_t *vol, char *path, char *key, uuid_t uuid, nc_int64_t rowid, int ishard, int istempl, nc_path_lock_t *, nc_part_element_t *, void *), 
				void *postaction_ctx,
				int limitcount);
int mdb_check_valid( nc_mdb_handle_t *mdb);
void mdb_commit(nc_mdb_handle_t *handle);
nc_int64_t mdb_insert(nc_mdb_handle_t *mdb, fc_inode_t *inode);
nc_uint32_t mdb_read_volume_age(nc_mdb_handle_t *mdb, char *volume);
int mdb_purge_volume(char *volume, nc_uint32_t);
int mdb_read_volumes_version(char *volume);
void mdb_check_leak(void *u);
int mdb_wait_done(nc_mdb_handle_t *mdb, mdb_tx_info_t * myseq);
int mdb_txno_compare(mdb_tx_info_t *a, mdb_tx_info_t *b);
void mdb_txno_null(mdb_tx_info_t *z);
int mdb_txno_isnull(mdb_tx_info_t *z);
int mdb_update_rowid(nc_mdb_handle_t 	 *mdb,
				 uuid_t				uuid,
				 nc_int64_t			rowid,
				 nc_size_t			size,
				 nc_size_t			rsize,
				 nc_time_t			vtime,
				 nc_mode_t			mode,
				 nc_uint64_t		obi,
				 mdb_tx_info_t		*txno
				 );
int mdb_remove_rowid_direct(nc_mdb_handle_t *mdb, uuid_t uuid, nc_int64_t rowid);

nc_mdb_inode_info_t * pm_map_header_to_minode(nc_part_element_t *part, void *header, char * msignature, char *qpath, char *qid);
nc_int64_t pm_verify_partition(char *partition);
int pm_check_partition();
nc_mdb_inode_info_t * pm_load_disk_inode2(nc_part_element_t *part, int casepreserve, char *signature, char *qid);
int pm_init(int high, int low);
size_t pm_get_align_size();
void pm_stop();
int pm_destroy();
void pm_get_winfo(int *config, int *running);
nc_part_element_t * pm_add_partition(char *path, int weight);
int pm_remove_metainfo(fc_inode_t *inode);
int pm_wait_mdb_done(nc_part_element_t *p, mdb_tx_info_t *txno);
int pm_unlink_uuid(char *vol_signature, char *key, int ispriv, nc_part_element_t *part, uuid_t uuid, char *cpath_ifany);
int pm_recover(nc_part_element_t *part, nc_mdb_inode_info_t *minode,char *signature, char *qpath, char *qid);
nc_part_element_t * pm_elect_part(nc_size_t size, int);
nc_mdb_inode_info_t * pm_load_disk_inode(int casepreserve, char *signature, char *qpath);
char * pm_create_cache_path(nc_part_element_t *part, int priv, uuid_t uuid, nc_uint32_t);
int pm_remove_cache_inode(fc_inode_t *inode, int bdelmdb);
int pm_reuse_inode(nc_part_element_t *part,  uuid_t uuid, nc_int64_t rowid, nc_time_t vtime, nc_uint32_t );
nc_int64_t pm_add_cache(nc_part_element_t *part, fc_inode_t *inode, int binsert);
int pm_check_under_mark(nc_part_element_t *part, int markv, int bupdate);
void pm_update_ios(nc_part_element_t *pe, int nio);
int pm_check_available (nc_part_element_t *part, uuid_t uuid);
int pm_invalidate_node(int preservecase, char *signature, char *qid);
int pm_purge(nc_volume_context_t *volume, char *pattern, int iskey, int ishard);
void pm_free_minode(nc_mdb_inode_info_t *mi);

typedef int (*fc_file_destructor_t)(fc_inode_t *fi);

#define	NC_MAGIC 		('X'<<24|'O'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V20 	('2'<<24|'0'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V21 	('2'<<24|'1'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V22 	('2'<<24|'2'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V23 	('3'<<24|'2'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V24 	('4'<<24|'2'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V25 	('5'<<24|'2'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V26 	('6'<<24|'2'<<16|'B'<<8|'S')	
#define	NC_MAGIC_V30 	('0'<<24|'3'<<16|'B'<<8|'S')	


typedef struct tag_vstring {
	nc_int16_t		len;
	char			data[4]; /* for alignement purpose */
} nc_vstring_t;
#define		NC_VSTRING_SIZE(_l)	(sizeof(nc_uint16_t) + ((_l + sizeof(nc_uint16_t)-1)/sizeof(nc_uint16_t))*sizeof(nc_uint16_t))

#define	NC_HEADER_FLAG_COMPRESSED		0x10000000






typedef struct tag_fc_common_header_info {
  	nc_uint32_t			magic;
  	nc_int32_t			disk_header_size;	/* 압축된 경우, 압축되어 저장된 디스크 헤더 크기 */
  	nc_int32_t			header_size;	 	/* total header size, 압축되지 않은 상태에서의 크기 */
  	nc_uint32_t 		flag;				/* 이전 버전과 호환성 문제로 위치고정, 향후 다용도로 사용 */
} fc_common_header_t;

struct tag_fc_header_info_v30 {
	fc_common_header_t	chdr;
  	nc_crc_t			crc; 				/* 32bit header CRC */
  	nc_int16_t			block_size;			/* unit : 1024, multiples of 1024 bytes */
  	nc_uint32_t 		size;
  	uuid_t				uuid;
	nc_mode_t			mode;
	nc_time_t			ctime; 				/* 원본 생성 시각*/
	nc_time_t			mtime;				/* 원본 최종 변경 시각*/
	nc_time_t			vtime; 				/* 다음 원본 신선도 체크 시각(UTC?) */
	nc_devid_t			devid;				/* device id, ex: etag */
  	nc_int16_t 			bitmaplen;			/* block bit count*/
	nc_uint16_t 		origincode; 		/*원본에서 제공한 응답 코드 */

	nc_obitinfo_t		obi;
	/*
	 * reserved 영역의 사용
	 */
	nc_uint32_t			cversion;		/* ir #24803 : 0인경우 1로 간주 , */
	nc_uint8_t  		dummy[56]; 		/* 예약된 영역, 향후 확장시 이 영역 사용*/


  	nc_uint32_t	     	vlen; 			/* 가변 헤더 정보 크기, 압축된 경우엔 압축된 크기  */
  	char	      		vbase[0];
  /* #1:vstring	vol_signature; */
  /* #2:vstring object path */
  /* #3:vstring object id */
  /* #4:valid chunk bitmap array */
  /* #5:block CRC array */
  /* #6:LP map array*/
  /* propert list */
};


#pragma pack()

/*
 * purge-related type definitions
 */
typedef struct {
	fc_inode_t 		*inode;
	char 			*qid;
	char 			*qpath;
	long long 		inode_signature;
	link_node_t 	node;
} match_inode_t;
typedef struct {
	nc_volume_context_t *volume;
	char 				*pattern;
	int 				pattern_len;
	nc_uint8_t 			match_target; /* 0: key, 1: path */
	nc_uint8_t 			prefix_match:1;
	nc_uint8_t 			setprogress:1;
	//int 				*purge_count;
	int 				count;
	link_list_t 		list;
} match_info_t;
typedef struct {
	nc_volume_context_t *volume;
	char 				*pattern; 		/* 패턴 문자열 */
	void 				*comp_pattern; 	/* 컴파일된 패턴 핸들*/
	int  				ishard; 	/* hard purge이면 1 */
	int  				iskey; 		/* key match이면 1, 0이면 path match */
	int 				count; 		/* purge 갯수 */
	link_list_t 		list; 		/* inode 리스트*/
} purge_info_t;

void kv_dump_allocated();
int kv_is_pooled(nc_xtra_options_t *kv);
int kv_oob_set_notifier(nc_xtra_options_t *kv, int (*callback)(nc_xtra_options_t *kv, void *), void *callback_data);
void kv_update_trace(nc_kv_list_t *root, char *f, int l);
nc_xtra_options_t * kv_create_d(char *f, int l);
nc_xtra_options_t * kv_create_pool_d(void *cb, void *(*allocator )(void *cb, size_t sz), const char *f, int l);
nc_xtra_options_t * kv_create_pool(void *cb, void *(*allocator )(void *cb, size_t sz));
char * kv_dump_oob(char *buf, int l, nc_xtra_options_t *opt);
int kv_setup_ring(nc_xtra_options_t *kv, size_t bufsiz);
void kv_set_raw_code(nc_xtra_options_t *root, int code);
int kv_get_raw_code(nc_xtra_options_t *root);
void kv_oob_write_eot(nc_xtra_options_t *kv);
void kv_oob_set_error(nc_xtra_options_t *kv, int err);
int kv_oob_get_error(nc_xtra_options_t *kv);
void *kv_add_val(nc_kv_list_t *root, char *key, char *val);
nc_kv_list_t * kv_add(nc_kv_list_t *root, nc_kv_t *toadd);
char * kv_find_val(nc_kv_list_t *root, char *key, int bcase);
void kv_destroy(nc_kv_list_t *root) ;
int  kv_for_each(nc_kv_list_t *root, int (*do_it)(char *key, char *val, void *cb), void *cb);
int kv_for_each_and_remove(nc_kv_list_t *root, int (*do_it)(char *key, char *val, void *cb), void *cb);
char * kv_dump_property(nc_xtra_options_t *root, char *buf, int len);
void kv_set_opaque(nc_xtra_options_t *kv, void *);
void kv_oob_command(nc_xtra_options_t *kv, char *cmd, nc_size_t len);
ssize_t kv_oob_write(nc_xtra_options_t *kv, char *data, size_t len);
int kv_oob_endofdata(nc_xtra_options_t *kv);
ssize_t kv_oob_read(nc_xtra_options_t *kv, char *data, size_t len, long timeout_msec);
void kv_set_stat_key(nc_xtra_options_t *root, void *keyid);
char *kv_get_stat_key(nc_xtra_options_t *root);
void kv_oob_lock(nc_xtra_options_t *opt);
void kv_oob_unlock(nc_xtra_options_t *opt);
int kv_oob_valid(nc_xtra_options_t *opt);
int kv_count_allocated(); 
void kv_dump_property_debugger(nc_xtra_options_t *root);



/* DISK-cache related macros 
 *		bc : bit count: bitmaplen
 *		bn : block number starting 0 which excludes header blocks
 * 
 */
#define		NC_BLOCK_OFFSET(f, bc, bn)		((f)->doffset + bn * NC_BLOCK_SIZE)
/* Origin related macros */
#define		NC_ORG_BLOCK_OFFSET(bn)		((long long)bn*NC_BLOCK_SIZE)
#define		NC_ORG_OFFSET_BLOCK(off)	(offset/NC_BLOCK_SIZE)
#define 	NC_SIZE2BLOCK(sz) 			((sz + NC_BLOCK_SIZE-1)/NC_BLOCK_SIZE)


#define 	NC_CANNED_BITMAP_SIZE(_bl)      	NC_ALIGNED_SIZE(BITS_TO_LONGS(_bl)*sizeof(unsigned long), sizeof(nc_int64_t))
#define		NC_CANNED_CRC_SIZE(_bl)				NC_ALIGNED_SIZE(_bl*sizeof(nc_crc_t), sizeof(nc_int64_t))
#define		NC_CANNED_BLOCKMAP_SIZE(_bl)		NC_ALIGNED_SIZE(_bl*sizeof(fc_blk_t *), sizeof(fc_blk_t *))
#define 	NC_CANNED_LPMAP_SIZE(_bl) 			NC_ALIGNED_SIZE((_bl)*sizeof(nc_blkno_t), sizeof(nc_int64_t))

#define		NC_CANNED_VLPMAP_SIZE(_i)			NC_CANNED_LPMAP_SIZE((_i)->bitmaplen, sizeof(nc_uint32_t))
#define		NC_CANNED_VLPMAP_SIZE_R(_bt, _bl)	NC_CANNED_LPMAP_SIZE((_bl), sizeof(nc_uint32_t))

#define 		LPMAP_BASE_BLKNO 				1
#define 		LPMAP_MAX_PBLKNO(_i) 			((_i)->maxblkno + LPMAP_BASE_BLKNO)
#define 		LPMAP_PBLKNO2BIT(_n) 			(_n-1)
#define 		LPMAP_BIT2PBLKNO(_n) 			(_n+1)


#define			INODE_LOCK(i_)					dm_inode_lock(i_, __FILE__, __LINE__)
#define			INODE_UNLOCK(i_)				dm_inode_unlock(i_, __FILE__, __LINE__)


/* called when fc_read is called and does not find the corresponding block from cache */
/*
 * return : # of blocks filled
 */
typedef void (*fc_done_handler)(char *buffer, int len, int err);
typedef int (*fc_fault_handler)(void *cbdata, fc_inode_t *f, long blkno, fc_blk_t *block[], int blkcnt, fc_done_handler cb);


#include "asio.h"

/* exported APIs */

/*
 * DM API
 */
int dm_is_mdb_dirty(fc_inode_t *inode);
void dm_commit_block_caching(fc_inode_t *inode, fc_blk_t *blk);
fc_inode_t * dm_new_inode(nc_int32_t uid);
void dm_free_origin_session(nc_volume_context_t *volume, nc_origin_session_t origin, int bound, char *f, int l);
nc_origin_session_t dm_allocate_origin_session(apc_open_context_t *aoc);
nc_origin_session_t dm_steal_origin_session(fc_inode_t *inode, cfs_off_t off, cfs_size_t siz);
int dm_is_inode_reusable(fc_inode_t *inode);
fc_file_t * dm_apc_fast_reopen_internal(fc_inode_t *inode, apc_open_context_t *oc);


int dm_inode_check_public(fc_inode_t *inode);
int dm_check_cache_completeness(fc_inode_t *inode, int bupdate);
nc_uint64_t dm_make_hash_key(int casesensitive, char *key);
void dm_clear_uuid(uuid_t uuid);
int dm_resize_extent(fc_inode_t *inode, nc_off_t off);
int dm_close_allowable(fc_inode_t *inode);
int dm_io_in_progress_nolock(fc_inode_t *inode);
int dm_io_add_vector_nolock(fc_inode_t *inode, nc_asio_vector_t *v);
int dm_io_count_vector_nolock(fc_inode_t *inode);
int dm_io_remove_vector_nolock(fc_inode_t *inode, nc_asio_vector_t *v);
int mdb_update_with_minode(nc_mdb_handle_t *mdb, nc_mdb_inode_info_t *minode, char *signature, char *qpath, char *qid);
void dm_update_content_version(fc_inode_t *inode);
void dm_update_disk_object_size_nolock(fc_inode_t *inode, nc_blkno_t blkno);
int dm_swapout_inode_nolock(void *d);
int dm_evict_inode_nolock(fc_inode_t *inode, int);
u_boolean_t dm_is_mdb_required(fc_inode_t *inode);
int dm_check_magic(unsigned long mc);
int dm_verify_if_orphaned(nc_part_element_t *part, char *cachepath);
void dm_minode_to_stat(nc_mdb_inode_info_t *minode, nc_stat_t *stat, char *hint, int *hint_len);
int dm_is_storable(fc_inode_t *inode);
int dm_is_writable(nc_mode_t mode);
nc_size_t dm_calc_header_size(fc_inode_t *inode);
nc_size_t dm_calc_header_size_v30(fc_inode_t *inode);
long dm_max_blkno(nc_size_t sz);
void dm_make_uuid(uuid_t uuid);
unsigned long dm_make_signature();
int dm_check_public(char *path, nc_stat_t *stat, nc_mode_t mode);
int dm_inode_idle(void *vf);
//int dm_check_idle_extent(void *iv);
void dm_reset_inode_nolock(fc_inode_t *inode) ;
int dm_check_concurrent_limit_nolock(fc_inode_t *inode);
int dm_is_loaded(char *signature, char *path);
void dm_shutdown(void);
typedef int (*dm_reclaim_inode_callback_t)(char *rfname);
int dm_init(char *path, size_t cap, dm_reclaim_inode_callback_t proc);
nc_size_t dm_inode_size(fc_inode_t *inode, int realdatasize);
fc_inode_t * dm_open_inode(nc_volume_context_t *volume, char *cache_id, char *origin_path, nc_mode_t mode, nc_xtra_options_t *kv);
void dm_update_viewcount(fc_inode_t *inode);
fc_blk_t * dm_get_block_nolock(fc_inode_t *inode, nc_blkno_t blkno);
int dm_close_inode(fc_inode_t *inode, int write_close, int async_close);
int dm_check_if_same_prop(fc_inode_t *inode, nc_stat_t *stat);
int dm_update_inode(fc_inode_t *f);
ssize_t dm_direct_read(fc_inode_t *f, void *p,  size_t len, nc_off_t off);
int dm_flush_inode_v20(fc_inode_t *inode);
int dm_flush_inode_v21(fc_inode_t *inode);
void dm_unlink_inode(struct cfs_origin_driver *drv, char *path, nc_xtra_options_t **opt, nc_xtra_options_t **res);
void dm_setwtimer(fc_inode_t *inode);
int dm_enough_space(int bupdate);
int dm_add_inode(fc_inode_t *f, u_hash_table_t *tbl);
void dm_add_defer_close(fc_inode_t *inode);
void dm_del_defer_close(fc_inode_t *inode);
int dm_valid_file(fc_inode_t *inode);
int dm_stat(struct cfs_origin_driver *drv, char *path, nc_stat_t *s);
int dm_online(void);
nc_uint64_t dm_get_signature(fc_inode_t *inode);
void dm_finish(void);
fc_blk_t * dm_ref_cached_block(fc_inode_t *inode, long blkno, int *cached);
void dm_unref_cached_block(fc_inode_t *inode, fc_blk_t *blk);
int dm_purge_inode(nc_volume_context_t *volume, char *zid, int ishard, int iskey, mdb_tx_info_t *txno);
int dm_isolate_cached_object(nc_volume_context_t *volume, fc_inode_t *inode);
void dm_dump_inodes(void);
void dm_signal_opened_inodes(int eno);
//int dm_count_valid_blocks(unsigned long *bitmap, int bitlen);
int dm_copy_statcache(nc_volume_context_t *volume, int isonline, char *cache_id, char *origin_path, nc_stat_t *stat, nc_kv_list_t *kv, nc_hit_status_t *stathit, nc_mode_t, int needclone);
void block_signal_2(int enabled);
void dm_update_block_crc_nolock(fc_inode_t *inode, long blkno, nc_crc_t crc);
PUBLIC_IF int dm_verify_block_crc(fc_inode_t *inode, long blkno, nc_crc_t);
int dm_get_cache_path(nc_volume_context_t *volume, char *path, char *cpath, int len);
void dm_cancel_cached_unlock(fc_inode_t *inode, long blkno);
int dm_redraw_block_nolock(fc_inode_t *inode, const char *);
int dm_inode_stat(nc_volume_context_t *volume, char *cache_id, char *origin_path, nc_stat_t *stat, void *stat_hint, int *hint_len, nc_kv_list_t *kv, nc_mode_t mode, nc_hit_status_t *hit, int enable_monitor);
int dm_is_MR(fc_inode_t *inode);
int dm_resize_inode(fc_inode_t *inode, nc_size_t len, int bforce);
int dm_utimens(nc_volume_context_t *volume, char *rpath, struct nc_timespec tv[2]);
int dm_rename(nc_volume_context_t *volume, char *src, char *dest);
int dm_unlink(nc_volume_context_t *volume, char *path, nc_xtra_options_t *req, nc_xtra_options_t  **res);
int dm_rmdir(nc_volume_context_t *volume, char *path);
int dm_mkdir(nc_volume_context_t *volume, char *path, nc_mode_t mode);
int dm_truncate(nc_volume_context_t *volume, char *path, nc_off_t off);
int dm_ftruncate(fc_inode_t *inode, nc_off_t length);
int dm_mknod(nc_volume_context_t *volume, char *path, nc_mode_t mode);
fc_inode_t * dm_create(nc_volume_context_t *volume, char *rpath, nc_mode_t mode);
void dm_update_size(fc_inode_t *inode, nc_size_t sz);
int dm_remove_cache_inode_with_path(nc_volume_context_t *volume, char *path);
int dm_flush_inode(fc_inode_t *inode);
int dm_detach_driver(fc_inode_t *inode, int needlock);
int dm_chmod(nc_volume_context_t *mc, char *path, nc_mode_t mode);
char * dm_dump_mode(char *buf, int len, nc_mode_t mode);
void dm_need_header_update(fc_inode_t *inode);
void dm_need_content_update(fc_inode_t *inode);
char * dm_dump_open_context(char *buf, int len, apc_open_context_t *o);
char * dm_dump_waitq(char *buf, int len, fc_inode_t *inode);

int dm_offset_eof(fc_inode_t *inode, nc_off_t offset);
char * dm_dump_lv2(char *dbuf, int remained, fc_inode_t *inode);
PUBLIC_IF CALLSYNTAX char * dm_dump_lv1(char *dbuf, int remained, fc_inode_t *inode);
PUBLIC_IF CALLSYNTAX int dm_inode_try_lock(fc_inode_t *inode, char *str);
PUBLIC_IF CALLSYNTAX void dm_inode_lock(fc_inode_t *inode, char *f, int l);
PUBLIC_IF CALLSYNTAX void dm_inode_unlock(fc_inode_t *inode, char  *, int l);
void dm_commit_length(fc_inode_t *inode, nc_size_t sz);
PUBLIC_IF CALLSYNTAX char * dm_dump_mode(char *buf, int len, nc_mode_t mode);
PUBLIC_IF CALLSYNTAX int dm_inode_locked();
void dm_update_misshit(fc_inode_t *inode, int hit);
void dm_update_hit(fc_inode_t *inode);
#ifndef WIN32
int dm_inode_check_owned(fc_inode_t *inode);
void dm_inode_unlock_all(fc_inode_t *inode) ;
#endif


u_boolean_t dm_disk_cacheable(fc_inode_t *inode, int ocode);
u_boolean_t dm_is_valid_bind_nolock(fc_inode_t *inode, fc_blk_t *blk, nc_blkno_t blkno);
char * dm_dump_lv0(char *dbuf, int remained, fc_inode_t *inode);
void dm_bind_block_nolock(fc_inode_t *inode, nc_blkno_t, fc_blk_t *blk);
int dm_unbind_block_nolock(fc_inode_t *inode, nc_blkno_t blkno);
int dm_reclaim_inode_ifnot_refered(nc_volume_context_t *vol, int , char *, uuid_t , char *, char *, nc_path_lock_t *, nc_uint32_t) ;
int dm_inode_for_each_property(fc_inode_t *inode, int (*do_it)(char *key, char *val, void *cb), void *cb);
void dm_fix_length(fc_inode_t *inode, nc_size_t sz);
void dm_copy_stat(nc_stat_t *cobj, fc_inode_t *inode, int needclone);
void dm_copy_stat_light(nc_stat_t *cobj, fc_inode_t *inode);
void dm_copy_object_info_minode(nc_object_info_t *obj, nc_mdb_inode_info_t *minode);
int dm_fini_size(fc_inode_t *inode);
int dm_prepare_internal_inode_nolock(fc_inode_t *inode, nc_off_t off);
int dm_end_of_file(fc_inode_t *inode, nc_off_t off);
void dm_copy_object_info(nc_object_info_t *obj, fc_inode_t *inode);
void dm_update_origin_block_no_nolock(fc_inode_t *inode, nc_blkno_t blkno);
int dm_need_origin_block(fc_inode_t *inode, nc_blkno_t blkno);
int dm_unmap_block_nolock(fc_inode_t *inode, fc_blk_t *blk);
void dm_recover_inode(fc_inode_t *inode) ;
int dm_dump_or_count_inode_opened(long tflag, int *total, int *free, int need_dump);
//void dm_free_extent_memory(fc_inode_t *inode, fc_inode_extent_t *ext, const char *sig);
int dm_need_iowait_nolock(fc_inode_t *inode);
void dm_update_stat(fc_inode_t *inode, nc_stat_t *cobj);
int dm_handle_obsolete_inode_nolock(nc_volume_context_t *volume, fc_inode_t *obsolete, u_boolean_t remove_onlyif_notused);
void dm_free_inode(fc_inode_t *inode, int recycle);
void dm_remove_cache(fc_inode_t *inode);
void dm_update_property(fc_inode_t *inode, nc_stat_t *stat);
int dm_reflect_size_nolock(fc_inode_t *inode, nc_off_t off);
void dm_update_ttl(fc_inode_t *inode, nc_stat_t *stat, nc_kv_list_t *reqkv, int error, int);
//int dm_resize_extent(fc_inode_t *inode, nc_off_t off);



void nvm_free_all();
void nvm_report_pl_stat();
int nvm_orphanize_PL(nc_volume_context_t *volume);
int nvm_path_lock_owned(nc_volume_context_t *volume, nc_path_lock_t *plck);
int nvm_is_valid(nc_volume_context_t *volume);
int nvm_unregister_cache_object_nolock(nc_volume_context_t *volume, fc_inode_t *inode);
int nvm_purge(nc_volume_context_t *volume, char *pattern, int ishard, int iskey);
int nvm_space_avail(nc_volume_context_t *volume, char *path);
int nvm_need_backup(nc_volume_context_t *volume);
int nvm_purge_volume(nc_volume_context_t *volume);
nc_volume_context_t * nvm_lookup_volume(char *signature);
nc_volume_context_t * nvm_create( char *drvname,  char *prefix, nc_origin_info_t *oi, int oicnt);
int nvm_destroy(nc_volume_context_t *volume, int bsync);
int nvm_is_usable_state(fc_inode_t *inode, nc_validation_result_t flag);
int nvm_is_online(nc_volume_context_t * volume, int flag);
int nvm_is_online2(nc_volume_context_t * volume);
int nvm_is_accessible(nc_volume_context_t *volume, int makeref);
int nvm_init();
fc_inode_t * nvm_register_cache_object(nc_volume_context_t *volume, fc_inode_t *f);
int nvm_unregister_cache_object(nc_volume_context_t *volume, fc_inode_t *inode);
int nvm_ioctl(nc_volume_context_t *mc, int cmd, void *val, int vallen);
fc_inode_t * nvm_lookup_inode(nc_volume_context_t *volume, int lookup_type, char *qid, int bremove, int makeref, int markbusy, int *busy, char *file, int line);
int nvm_lock(nc_volume_context_t *mc, int xclusive, char *f, int l);
void nvm_unlock(nc_volume_context_t *mc, char *f, int l);
int nvm_enum_matches(nc_volume_context_t *volume, match_info_t *matchinfo);
int nvm_statfs(nc_volume_context_t *volume, char *path, nc_statvfs_t *fs);
void nvm_free();
char * nvm_dump_PL(char *buf, int l, nc_path_lock_t *p);
int nvm_path_lock_owned_n_healthy(nc_volume_context_t *volume, nc_path_lock_t *plck, char *path);
void nvm_path_lock_unref_reuse(nc_volume_context_t *volume, nc_path_lock_t *pl, char *f, int l);
void nvm_path_lock_ref_reuse(nc_volume_context_t *volume, nc_path_lock_t *pl, char *f, int l);
void nvm_make_inode_stale(nc_volume_context_t *volume, fc_inode_t *inode);
int nvm_isolate_inode_nolock(nc_volume_context_t *volume, fc_inode_t *inode);
fc_inode_t * nvm_isolate_inode_with_key(nc_volume_context_t *volume, char *key);
int nvm_purge_inode_with_key(nc_volume_context_t *volume, char *key, uuid_t uuid, int ishard, nc_path_lock_t *pl, nc_part_element_t *part, mdb_tx_info_t *txno);
int nvm_purge_inode(nc_volume_context_t *volume, fc_inode_t *inode, int resetbusy, mdb_tx_info_t *txno);
void nvm_update_read_stat();
int nvm_for_all_inodes(nc_volume_context_t *volume, int (callback)(fc_inode_t *, void *), void *cbdata);

void nvm_path_G_lock(char *f);
void nvm_path_G_unlock();



int 			nvm_path_lock_cond_timedwait( pthread_cond_t *hsignal, nc_path_lock_t *pl, struct timespec *ts);
nc_path_lock_t *nvm_path_lock_ref(nc_volume_context_t *volume, char *path, char *, int);
int 			nvm_path_lock_unref(nc_volume_context_t *volume, nc_path_lock_t *pl, char *, int);

int 			nvm_path_lock(nc_volume_context_t *volume, nc_path_lock_t *pl, char *, int);
int 			nvm_path_unlock(nc_volume_context_t *volume, nc_path_lock_t *plck, char *f, int l);

int 			nvm_path_put_wait(nc_path_lock_t *, apc_open_context_t  *oc, char *, int);
int 			nvm_path_lock_is(nc_path_lock_t *, nc_path_lock_state_t s, char *f, int l);
void 			nvm_path_inprogress_nolock(nc_path_lock_t *pl, int, char *,int);
nc_uint32_t		nvm_path_scheduled_left(nc_path_lock_t *pl);
nc_uint32_t		nvm_path_schedule(nc_path_lock_t *pl, int run_or_done, char *f, int l);

void 			nvm_path_set_private_nolock(nc_path_lock_t *pl, int priv);
int 			nvm_path_is_private_nolock(nc_path_lock_t *pl);

nc_validation_result_t nvm_check_freshness_nolock(nc_volume_context_t *volume, fc_inode_t *inode, nc_xtra_options_t *kv, nc_origin_session_t octx, nc_stat_t *stat);
fc_cache_status_t nvm_need_freshness_check_nolock(nc_volume_context_t *volume, fc_inode_t *inode);
nc_validation_result_t nvm_check_freshness_by_policy(nc_volume_context_t *volume, fc_inode_t *inode, nc_stat_t *stat, int skipalways, int origindown, nc_validation_result_t defval);
u_boolean_t nvm_hash_equal(void *v1, void *v2, void *ud);
void * nvm_create_pattern(nc_volume_context_t *volume, char *pat_string);
void nvm_destroy_pattern(nc_volume_context_t *volume, void *reg_pat);
void apc_destroy_open_context(apc_open_context_t *oc);

/* stat cluster */
int scc_init();
int scc_setup(char *maddr, int port, char *localaddr, int ttl);
//int scc_query_stat(char *signature, char *qpath, int msec, nc_stat_t *s);
int scc_query_stat(char *signature, char *qpath, int msec, nc_stat_t *s, nc_xtra_options_t *req_prop);
int scc_shutdown();

fc_file_t * fio_open_extended_apc_internal(		nc_volume_context_t 	*volume, 
										char 					*cachepath, 
										char 					*path, 
										nc_mode_t 				mode, 
										nc_stat_t 				*ns, 
										nc_xtra_options_t 		*req_prop, 
										nc_apc_open_callback_t 	proc, 
										void 					*userdata);
fc_file_t * fio_open_extended_apc_wrap(		nc_volume_context_t 	*volume, 
										char 					*cachepath, 
										char 					*path, 
										nc_mode_t 				mode, 
										nc_stat_t 				*ns, 
										nc_xtra_options_t 		*req_prop, 
										nc_apc_open_callback_t 	proc, 
										void 					*userdata);
nc_ssize_t fio_read_apc(fc_file_t *fi, char *buf, nc_off_t off, nc_size_t len, nc_apc_read_callback_t callback, void *callbackdata);
int fio_close_allowable(fc_file_t *fh);
nc_ssize_t fio_async_read(fc_file_t *fi, char *buf, nc_off_t off, nc_size_t len);
fc_file_t * fio_open_extended(nc_volume_context_t *volume, char *cachepath, char *path, nc_mode_t mode, nc_stat_t *ns, nc_xtra_options_t *req_prop);
int fio_getattr(nc_volume_context_t *volume, char *path, nc_stat_t *s);
int fio_fgetattr(fc_file_t *fh, nc_stat_t *s);
fc_file_t * fio_open(nc_volume_context_t *volume, char *cachepath, char *path, nc_mode_t mode, nc_kv_list_t *list);
int fio_close(fc_file_t *fh, int force);
nc_ssize_t fio_read(fc_file_t *fi, char *buf, nc_off_t off, nc_size_t len);
nc_ssize_t  fio_write(fc_file_t *fi, char *buf, nc_off_t off, size_t len);
fc_file_t * fio_create(nc_volume_context_t *volume, char *path, nc_mode_t mode, nc_kv_list_t *list);
int fio_ftruncate(fc_file_t *fi, nc_size_t len);
int fio_flush(nc_file_handle_t *fi);
int fio_for_each_property(fc_file_t *fi, int (*do_it)(char *key, char *val, void *cb), void *cb);
int fio_invoke_io(fc_file_t *fh);
int fio_result_code(fc_file_t *fi);
char * fio_find_property(nc_file_handle_t *fi, char *tag);
void nc_add_dm(nc_int32_t asz, nc_int32_t sz);
void nc_sub_dm(nc_int32_t asz, nc_int32_t sz);



int lm_init_lock_pool(int max_lcks);
void lm_destroy_lock_pool();
void lm_release_lock(nc_lock_t *lock);
nc_lock_t * lm_acquire_lock();
int nc_mutex_lock(nc_lock_t *lock);
int nc_mutex_unlock(nc_lock_t *lock);
int _sync_file(char *src, char *dest);

char * hz_string(char *buf, int len, char *ibuf);

/*
 * cache operations
 */

nc_size_t ra_size_bytes();

//void ic_hit_extent_nolock(fc_inode_t *inode);
void ic_set_busy(fc_inode_t *inode, int su, char *f, int l);
int ic_is_busy(fc_inode_t *inode) ;
pthread_mutex_t * ic_get_global_lock();
int ic_is_hot(fc_inode_t *inode);
int ic_shutdown();
int ic_init();
int ic_is_cached_nolock(fc_inode_t *inode) ;
int ic_hit_nolock(fc_inode_t *inode, int ntimes);
fc_inode_t *ic_lookup_cache_object(nc_volume_context_t *volume, int lookup_type, char *qid, int makeref, int setprogress, char *file, int line);
fc_inode_t * ic_prepare_inode_raw_nolock( 	nc_volume_context_t *volume, 
											char 				* cache_id,
											char 				* origin_path,  
											nc_mode_t 			mode,  /* stat 구조내의 mode 값에 우선한다 */
											nc_stat_t 			*stat,
											nc_xtra_options_t 	*property,
											int 				complete,
											nc_uint32_t			cversion,
											u_boolean_t 		template,
											int 				setprogress,
											char 				*f,
											int					l
											);
void ic_wait_on_progress(fc_inode_t *inode, int alradylocked);
fc_inode_t * ic_prepare_inode_with_minode_nolock (nc_volume_context_t *volume, nc_mdb_inode_info_t *minode, int progress);
void ic_get_stat(int *total, int *cached, int *freed, int *xtotal, int *xcached, int *xfreed);
#ifndef WIN32
int ic_lock_owned();
#endif

/* CHG 2018-04-11 huibong 함수 반환 값 void 변경 처리 (#32013) */ 
//void ic_bind_extent_nolock(fc_inode_t *inode, fc_inode_extent_t *ext);

int ic_free_count_nolock();
int ic_free_object_nolock(fc_inode_t *inode);
int ic_free_object(fc_inode_t *inode);
fc_inode_t * ic_register_cache_object_nolock(nc_volume_context_t *volume, fc_inode_t *inode);
int ic_unregister_cache_object_nolock(nc_volume_context_t *volume, fc_inode_t *inode);
int ic_set_online(fc_inode_t *inode, char *path, int line);
int ic_set_progress_nolock(fc_inode_t *inode, int pflag, char *path, int line);
int ic_is_progress(fc_inode_t *inode);
int ic_progress_assert(fc_inode_t *inode, int v);
int ic_set_progress(fc_inode_t *inode, int bexclusive, char *path, int line);
int ic_free_object(fc_inode_t *inode);
void ic_update_signature(fc_inode_t *inode);
int ic_free_inode();
long bcm_page_size(void);
int opq_valid(nc_opaque_t *o);
void opq_reset(nc_opaque_t *o);
void opq_copy(nc_opaque_t *dest, nc_opaque_t *src, int reset);
void opq_set(nc_opaque_t *dest, void *p);

nc_path_lock_state_t nvm_path_get_state(nc_path_lock_t *pl);
int nvm_post_wakeup(nc_volume_context_t *volume, fc_inode_t *inode, nc_path_lock_t *pl, nc_stat_t *stat, int err);
nc_path_lock_t * nvm_path_lock_status(nc_volume_context_t *volume, char *path, nc_path_lock_state_t s, char *, int);
int nvm_path_lock_is_nolock(nc_path_lock_t *pl, nc_path_lock_state_t s, char *f, int l);
int apcs_run_action(apc_context_t  *apc);
int apcs_close_action(apc_context_t *apc);
int apcs_open_action_LOCK(apc_context_t *apc);
int dm_apcs_remove_action(apc_remove_context_t *ri);
fc_inode_t * 
dm_apc_open_inode(		nc_volume_context_t 		*volume, 
						char 						*cache_id, 
						char 						*origin_path, 
						nc_mode_t 					mode, 
						nc_xtra_options_t 			*kv, 
						nc_path_lock_t  			*pl,
						nc_apc_open_callback_t 		callback, 
						void * callback_data);
apc_open_context_t *
dm_apc_prepare_context_raw(		nc_volume_context_t 	*volume, 
								fc_inode_t 				*inode,
								char 					*originpath, 
								char 					*cachekey_given, 
								char 					*cachekey, 
								nc_mode_t 				mode, 
								int 					state, 
								nc_xtra_options_t 		*inprop, 
								nc_path_lock_t 			*pl,
								nc_uint32_t				ubi, 
								nc_apc_open_callback_t 	callback, 
								void *callback_data);
fc_file_t * fio_make_fhandle(fc_inode_t *inode, nc_path_lock_t *, nc_mode_t mode, nc_xtra_options_t *kv);
int nvm_path_put_wait_nolock(nc_volume_context_t *volume, char *path, apc_open_context_t  *oc);

char * nvm_path_lock_dump(char *buf, int len, nc_path_lock_t *pl);

void * nvm_get_fcg_first(nc_volume_context_t *volume);
void *nvm_get_fcg_next(nc_volume_context_t *volume, void *anc);
int nvm_fcg_id(void *anc);
int nvm_path_lock_is_for(nc_path_lock_t *pl, char *key);
int nvm_path_lock_ref_count(nc_path_lock_t *pl);
int nvm_path_busy_nolock(nc_path_lock_t *pl);
int nvm_volume_valid(nc_volume_context_t *v);

nc_off_t dm_cache_offset(fc_inode_t *inode, nc_blkno_t blkno);
int dm_open_iowait(fc_inode_t *inode);
int dm_wait_iowait(fc_inode_t *inode, int (*completed)(void *), void *cb, int msec);
int dm_finish_frio_nolock(fc_inode_t *inode, int error);
int dm_close_iowait(fc_inode_t *inode);
int dm_forcely_close_inode(fc_inode_t *inode, void *ud);
int dm_check_concurrent_limit_nolock(fc_inode_t *inode);
void dm_remove_refered_cache(fc_inode_t *inode);
int dm_remove_cache_raw(fc_inode_t *inode);
int dm_signal_iowait(fc_inode_t *inode);
int dm_add_ctx_waitq(fc_inode_t *inode, apc_read_context_t *rctx);
int dm_run_waitq(fc_inode_t *inode, fc_blk_t * blk, int error);
void dm_cancel_waitq(fc_inode_t *inode, apc_read_context_t *rc);
apc_read_context_t * dm_schedule_apc_read(	fc_inode_t 				*inode, 
						char 					*buffer, 
						nc_off_t 				foffset, 
						nc_size_t 				size, 
						nc_xtra_options_t 		*inprop,
						apc_block_ready_callback_t 	callback, 
						blk_apc_read_info_t 	*pri
		);
void dm_need_mdb_update(fc_inode_t *inode, const char *);
int dm_update_size_with_RI_nolock(fc_inode_t *inode, nc_off_t off, nc_size_t len);
void dm_finish_frio(fc_inode_t *inode, int error);
void dm_set_inode_stat(fc_inode_t *inode, nc_stat_t *stat, nc_mode_t mode, int update_cc_for_pseudoprop);
int dm_apcs_remove_eaction(apc_remove_context_t *ri);
int dm_need_frio(fc_inode_t *inode);
fc_block_status_t dm_block_status_nolock(fc_inode_t *inode, fc_blk_t **blk, nc_blkno_t blkno, int makeref, char *f, int l);
int dm_block_status_batch_nolock(fc_inode_t *inode, nc_blkno_t baseno, bci_stat_t *ba, int maxm, int maxd, int maxf, int maxe, int makeref);
void dm_swap_cacheobject_ifneededed(fc_inode_t *inode);
void dm_inode_make_stale(fc_inode_t *inode);
void dm_verify_allocation(fc_inode_t *inode);


int 	mem_init(memmgr_heap_t *, long long memsz);
void * 	mem_alloc(memmgr_heap_t *, nc_size_t memsz, int aligned, int category, int force, const char *f, int l);
void 	mem_stat(memmgr_heap_t *heap, nc_int64_t *used, nc_int64_t *total);
char * 	mem_strdup(memmgr_heap_t *heap, char *str, int category, const char *f, int l);
void 	mem_free(memmgr_heap_t *heap, void *p, int aligned, nc_size_t memsz, const char *f, int l);
void * 	mem_realloc(memmgr_heap_t *heap, void *p, nc_size_t memsz, int category, const char *f, int l);



#define 	DM_LOOKUP_WITH_KEY 		0
#define 	DM_LOOKUP_WITH_CO		1


#ifdef __TRACE
#define 	ASSERT_FILE(f_, x_)	if (!(x_)) { \
									char dbuf_[2048]; \
	TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::%s\n", \
													#x_, __LINE__, __FILE__, \
													dm_dump_lv1(dbuf_, sizeof(dbuf_), f_))); \
													TRAP; \
								}
#define 	ASSERT_PATHLOCK(f, x)	if (!(x)) { \
											char _dbuf[1024]; \
	TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::%s\n", \
													#x, __LINE__, __FILE__, \
													nvm_path_lock_dump(_dbuf, sizeof(_dbuf), f))); \
													TRAP; \
								}
#else
#define 	ASSERT_FILE(b, e)	
#endif

#ifdef NC_DEBUG_BUILD
#define 	DEBUG_ASSERT_PATHLOCK(f, x)	if (!(x)) { \
											char _dbuf[1024]; \
											TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::%s\n", \
													#x, __LINE__, __FILE__, \
													nvm_path_lock_dump(_dbuf, sizeof(_dbuf), f))); \
													TRAP; \
								}
#define 	DEBUG_ASSERT_FILE(f_, x_)	if (!(x_)) { \
									char dbuf_[2048]; \
									TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::%s\n", \
													#x_, __LINE__, __FILE__, \
													dm_dump_lv1(dbuf_, sizeof(dbuf_), f_))); \
													TRAP; \
								}
#else
#define 	DEBUG_ASSERT_FILE(b, e)	
#define 	DEBUG_ASSERT_PATHLOCK(f, x)	
#endif




#define NC_VALID_BLOCK(_i, _b)		(_i && (_b) && ((_b)->stat > 0) && (!(_b)->victim ) && (_i->signature == (_b)->signature) && ((_b)->magic == BLOCK_MAGIC) && (_i->blockmap) ) 
#define NC_VALID_INODE(_i, _s)		(_i && (_i->cstat > 0) && (_i->refcnt > 0) && (_i->blockmap || _i->ob_complete) && (_i->signature == _s))

#define NC_VALID_INODE_FD(_i)		(_i && (_i->cstat > 0) && (_i->refcnt > 0) && (_i->blockmap != NULL || _i->ob_complete) && (_i->memres ||__memcache_mode || (DM_FD_VALID(_i->fd))))
//#define NC_VALID_INODE_2(_i)		(_i && (_i->cstat > 0) && (_i->refcnt > 0) && (_i->memres ||__memcache_mode || (DM_FD_VALID(_i->fd))))
#define NC_VALID_INODE_2(_i)		(_i && (_i->cstat > 0) && (_i->refcnt > 0))



#define 	DM_CLOSE_ALLOWED(_i) 	(dm_io_in_progress_nolock(_i)== 0)

#define 	NC_INODE_ID(_f_) 		((_f_)? (_f_)->inode->uid:-1)

extern int				__block_count ; /*calced from __cache_size later */
extern int				g__page_size ; 
extern fc_clfu_root_t	*g__inode_cache; 

#ifdef NC_HEAPTRACK
extern __thread int 	___trace_upper;
#endif















#endif /* __NETCACHE_H__ */
