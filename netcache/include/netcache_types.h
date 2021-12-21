#ifndef __NETCACHE_TYPES_H__
#define __NETCACHE_TYPES_H__

#include <unistd.h>
#ifndef GCC_VERSION
#define GCC_VERSION	(__GNUC__*10000 + __GNUC_MINOR__*100 + __GNUC_PATCHLEVEL__)
#endif
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <netcache_config.h>


#if UINTPTR_MAX == 0xffffffff
#define 	PLATFORM_32BIT 	1
#define 	PLATFORM_64BIT 	0
#else
#define 	PLATFORM_64BIT 	1
#define 	PLATFORM_32BIT 	0
#endif

#ifdef WIN32

#include <windows.h>
/* the value comes from LINUX fcntl.h */



#ifndef O_DIRECT
#define		O_DIRECT 	00040000
#endif
#endif


#ifndef ESTALE
#define ESTALE 	116
#endif /* ESTALE */
#ifndef EREMOTEIO
#define EREMOTEIO 	121
#endif /* EREMOTEIO */
#ifndef ECANCELED 	
#define ECANCELED 	125
#endif /* ECANCELED */



#define NC_MAXPATHLEN_V20   	256
#define NC_MAXPATHLEN_V21   	1024
#define NC_MAXPATHLEN_V24   	4096
#define NC_MAXPATHLEN   		NC_MAXPATHLEN_V24
#define NC_VISIBLE_MAXPATHLEN  (NC_MAXPATHLEN_V24-64)
#define	NC_MAX_FILE_SIZE	(2000000000LL)


#define 	NC_ALIGNED_SIZE(_s, _u) 	((long long)(((long long)(_s) + (_u) -1)/(_u))*((long long)_u))
#define 	NC_ALIGNED_PTR(_s, _u) 		((long long)((_s)+(_u)-1ULL) & (long long)~((_u)-1ULL))

#define		GET_ATOMIC_VAL(i)	__sync_add_and_fetch(&(i), 0)
#define		GET_ATOMIC_VAL_P(i)	__sync_add_and_fetch((i), 0)
#define		_ATOMIC_ADD(i, v)	__sync_add_and_fetch(&(i), (v))
#define		_ATOMIC_SUB(i, v)	__sync_sub_and_fetch(&(i), (v))
#define		_ATOMIC_ADD_P(i, v)	__sync_add_and_fetch((i), (v))
#define		_ATOMIC_SUB_P(i, v)	__sync_sub_and_fetch((i), (v))
#define		_ATOMIC_VAL(i)		__sync_sub_and_fetch(&(i), 0)
#define		_ATOMIC_CAS(i, o, v)	__sync_bool_compare_and_swap(&(i), o, v)


typedef struct timespec	perf_val_t;

typedef long long 			nc_ssize_t;
typedef long long 			nc_size_t;
typedef long long 			nc_off_t;
typedef unsigned long long	nc_uint64_t;
typedef long long int			nc_int64_t;


typedef unsigned char 		nc_uint8_t;
typedef unsigned short 		nc_uint16_t;
typedef unsigned int		nc_uint32_t;

typedef char 				nc_int8_t;
typedef short				nc_int16_t;
typedef int					nc_int32_t;
typedef nc_uint32_t			nc_time_t;
typedef nc_uint32_t			nc_blkno_t;
typedef unsigned int		nc_buid_t;
#ifdef WIN32
typedef HANDLE				nc_fio_handle_t;
#else
typedef int					nc_fio_handle_t;
#endif
#define						EOF_BLKNO		0xFFFFFFFF




struct nc_asio_context;
struct nc_asio_vector;

typedef struct nc_asio_context 		nc_asio_context_t;
typedef struct nc_asio_vector 		nc_asio_vector_t;



struct cfs_origin_driver;
struct nc_volume_context; 
struct tag_fc_blk; 
struct tag_fc_inode_info;
struct nc_stat;
struct tag_fc_file_info;
struct nc_volume_context;
struct _log_handle;
struct tag_origin_info; 


typedef struct tag_fc_blk			fc_blk_t;

typedef struct tag_nc_file_ref 	nc_file_handle_t;

typedef struct nc_volume_context 	nc_volume_context_t;
typedef struct _log_handle 			LOG_HANDLE_t;
typedef void * tp_data_t;
//typedef int nc_asio_type_t;
typedef enum {IOT_UNKN=0, IOT_CACHE_READ=1, IOT_CACHE_WRITE=2, IOT_OSD_READ=3, IOT_OSD_WRITE=4, IOT_OSD_READ_ENTIRE=5, IOT_NULL=6 } nc_asio_type_t;

typedef nc_int32_t 	nc_uid_t;
typedef nc_int32_t 	nc_gid_t;
typedef nc_int32_t 	nc_mode_t;

typedef struct tag_origin_info 		nc_origin_info_t;

typedef struct tag_statvfs {
	nc_uint32_t		f_bsize;
	nc_uint32_t		f_frsize;
	nc_int64_t 		f_blocks;
	nc_int64_t 		f_bfree;
	nc_int64_t 		f_bavail;
	nc_int64_t 		f_files;

	nc_int64_t 		f_ffree;
	nc_int64_t 		f_favail;
	nc_int32_t 		f_fsid;
	nc_int32_t 		f_flag;
	nc_int32_t 		f_namemax;
} nc_statvfs_t;

struct nc_timespec {
        nc_time_t 	tv_sec;
        long 		tv_nsec;
};



typedef struct tag_link_node {
	union {
		nc_uint64_t			xbyte;
		struct {
			nc_int8_t				inlist;
			/* 확장될수있음 최대 64bit */
		} ns;
	} u;
	void					*data;
	struct tag_link_node	*next;
	struct tag_link_node	*prev;

} link_node_t;
#define	NODE_CONTAINS(_node, _data)		((_node) && ((_node)->data == _data))
/*
 * 아래 구조체는 aligned memory allocation에 대한
 * 추적관리를 위한 house-keeping용 메모리 포인터임
 * 각 필드는 nc_aligned_alloc에서 세팅됨
 * nc_aligned_free에서 아래 구조체를 내려주면 모두 free
 */
typedef struct {
	nc_uint64_t	rsize;
	void 		*rptr;
	link_node_t	node;
} nc_aligned_ptr_t;

typedef enum {
	APC_OPEN 				= 0,  /* open context에 대한 event */
	APC_APP_READ          	= 1,  /* application read 요청 */
	APC_READ_VECTOR_DONE 	= 1,  /* origin read요청 완료, 완료 post-action실행 필요 */
	APC_WRITE_VECTOR 		= 2,
	APC_RUN_VECTOR 			= 3,  /* run-queued, vector실행 요청*/
	APC_CHUNK 				= 4,
	APC_READ_CHUNK 			= 5,
	APC_CLOSE_INODE 		= 6,
	APC_READ_TIMEOUT		= 7,	  /* read-timeout */
	APC_NULL				= 0xFF	  /* NULL */
} nc_apcop_t;
typedef struct {
	nc_apcop_t		apc_op;
	nc_uint8_t		apc_data[1];
} apc_context_t;

typedef int (*apc_completion_proc_t)(void *);
typedef enum {
	OT_CALLBACK	= 1,
	OT_EVENT 	= 2
} apc_overlapped_type_t; 


typedef struct tag_apc_overlapped {
	nc_uint16_t 			fmagic;
	apc_overlapped_type_t		type;
	union {
		struct {
			void 					*ud;
			apc_completion_proc_t	func;
		} ucallback;
		struct {
			nc_int8_t				finished;
			pthread_cond_t			signal;
			pthread_mutex_t			lock;
		} uevent;
	} u;
	nc_uint16_t 			rmagic;
} apc_overlapped_t;


struct tag_nc_kv_list;
typedef struct tag_nc_kv_list 	nc_xtra_options_t;
typedef struct tag_nc_kv_node 	nc_option_t;
typedef struct nc_path_lock_tag nc_path_lock_t;
typedef void * 					nc_lb_handle_t;




/*
 * 주의!!!!
 * 		- bit information의 순서는 반드시 유지해야함: 순서가 수정되거나 중간 필드가 삭제
 * 		  되는 경우 MDB의 bit정보의 해석이 바뀌게 되어 장애가 발생함
 * 		- 새로운 bit의 추가는 반드시 뒤에 append하거나, 기존 필드 중 안쓰는 bit의 재사용
 */
typedef union {
		nc_uint64_t		op_bit_s;
		struct bitefields {
				/*
				 * REMARK!
				 *  bit-field추가 사용시 nc_mdb_inode_info_t의 
				 */
				nc_uint8_t 			obsolete__;



				/*
				 * RFC specified info (keep the order for compatibility)
				 */
			  	nc_uint64_t 		rangeable:1;
			  	nc_uint64_t 		chunked:1;			/* 객체의 transfer-encoding: chunked 였던 경우 */
			  	nc_uint64_t			vary:1;				/* 대상 객체가 vary 객체임 */
			  	nc_uint64_t			priv:1;
			  	nc_uint64_t      	complete:1; 		/* 1 if all blocks are cached */
			  	nc_uint64_t      	mustreval:1; 		/* 대상 객체에 접근할 때 마다 반드시 원본에 신선도 검사 필요 */
			  	nc_uint64_t      	sizeknown:1; 		/* 객체크기가 알려진경우,  norandom=1인경우, io가 끝나야 알 수 있음 */
			  	nc_uint64_t      	sizedecled:1; 		/* 객체크기가 알려진경우,  norandom=1인경우, io가 끝나야 알 수 있음 */
				nc_uint64_t 		nocache:1; 			/* no cache */
				nc_uint64_t			template:1;			/* vary 객체 template. 대상 URL이 vary객체인지 여부만 판단하기위해 존재하는 inode*/
				nc_uint64_t			mustexpire:1;		/* ir #31909; 1: vtime까지만 유효, 이후엔 무조건 폐기 */
				nc_uint64_t 		refreshstat:1;
				nc_uint64_t 		xfv:1; 				/* 1 : expire after st_vtime < now */


	
				nc_uint64_t      	preservecase:1;   	/* volume에서 가져온 값인데 무결성 문제 발생가능. 삭제가 요구됨 */
				/*
				 * cold cache features
				 */
				nc_uint64_t      	upgraded:1;			/* cold caching일 때만 해석 */
				nc_uint64_t      	upgradable:1;		/* cold caching일 때만 해석 */

				nc_uint64_t 		frio_inprogress:1; 	/* norandom_io가 진행 중이면 1 */
				nc_uint64_t 		validuuid:1;		/* UUID 가  valid && caching-file 존재 */

				nc_uint64_t 		orphan:1;
				nc_uint64_t 		doc:1; 				/* delete on close of cache object */
				nc_uint64_t      	staled:1;			/* 1: 원본 객체가 변경된 상태.캐싱된 inode 폐기 필요*/

			  	nc_uint64_t      	created:1; 			/* inode가 신규로 생성된 경우 1 */
				nc_uint64_t      	__notused__1:1;		/* NOT USED:inode가 disk MDB에서 로딩되어 생성된 경우 */
				nc_uint64_t 		memres:1; 			/* 1 if the file data cached only on RAM */
				nc_uint64_t			__notused__2:1;		/* NOT USED:inode가 disk에서 로딩됨 */

				nc_uint64_t      	pseudoprop:1;		/* 1: pseudo property , POST operatoin 실행 초기 단계에서 1로 세팅됨*/
				nc_uint64_t 		needoverwrite:1; 	/* 1 : 해당 객체 정보가 리셋되어 원본에서 수신한 데이타로 overwrite필요 */

				/*
				 * added by weon(2020.8.24)
				 */
			  	nc_uint64_t			public:1;
				nc_uint64_t 		onlyifcached:1; 			/* no cache */
			  	nc_uint64_t      	pxyreval:1; 		/* proxy reval */
			  	nc_uint64_t      	immutable:1; 		/* immutable */
			  	nc_uint64_t      	nostore:1;			/* nostore */
			  	nc_uint64_t      	noxform:1;			/* noxform */

			  	nc_uint64_t			cookie:1;			/* cookie presents */
				/*
				 * end of addition(2020.8.24)
				 */

		} op_bits;
} nc_obitinfo_t; /* cache object bit info */
#define		ob_obit					obi.op_bit_s

#define		ob_public 			obi.op_bits.public
#define		ob_priv 			obi.op_bits.priv
#define		ob_nocache 			obi.op_bits.nocache
#define		ob_onlyifcached 	obi.op_bits.onlyifcached
#define		ob_mustreval 		obi.op_bits.mustreval
#define		ob_pxyreval 		obi.op_bits.pxyreval
#define		ob_immutable 		obi.op_bits.immutable
#define		ob_nostore 			obi.op_bits.nostore
#define		ob_noxform 			obi.op_bits.noxform



#define 	ob_rangeable 			obi.op_bits.rangeable
#define 	ob_cookie				obi.op_bits.cookie
#define 	ob_complete 			obi.op_bits.complete
#define		ob_chunked				obi.op_bits.chunked
#define		ob_vary					obi.op_bits.vary
#define		ob_complete				obi.op_bits.complete
#define		ob_sizeknown			obi.op_bits.sizeknown
#define		ob_sizedecled			obi.op_bits.sizedecled
#define		ob_template				obi.op_bits.template

#define		ob_preservecase 		obi.op_bits.preservecase
#define		ob_upgraded 			obi.op_bits.upgraded
#define		ob_upgradable 			obi.op_bits.upgradable
#define		ob_needoverwrite 		obi.op_bits.needoverwrite
#define		ob_frio_inprogress 		obi.op_bits.frio_inprogress
//#define		ob_stat 				obi.op_bits.stat
#define		ob_validuuid 			obi.op_bits.validuuid
#define		ob_victim 				obi.op_bits.victim
#define		ob_onmdb 				obi.op_bits.onmdb
#define		ob_isolated 			obi.op_bits.isolated
#define		ob_doc 					obi.op_bits.doc
#define		ob_odoc 				obi.op_bits.odoc
#define		ob_loaded 				obi.op_bits.loaded
#define		ob_created 				obi.op_bits.created
#define		ob_memres 				obi.op_bits.memres
#define		ob_refreshstat 			obi.op_bits.refreshstat
#define		ob_trace 				obi.op_bits.trace
#define		ob_xfv 					obi.op_bits.xfv
#define		ob_occ 					obi.op_bits.occ
#define		ob_ondisk 				obi.op_bits.ondisk
#define		ob_pseudoprop 			obi.op_bits.pseudoprop
#define		ob_staled 				obi.op_bits.staled
#define		ob_mustexpire 			obi.op_bits.mustexpire
#define		ob_setbyexpires 		obi.op_bits.setbyexpires
#define		ob_swappedin 			obi.op_bits.swappedin


/*
 * Since version 3.0
 * nc_open_XXX related additions
 */
struct tag_nc_open_cc ; /* NetCache Open Custom Context: nc_open_XXXX와이후 operation에 필요한 파라미터 구축용 구조체 */
typedef struct tag_nc_open_cc 	nc_open_cc_t;

typedef enum {
	NC_OCC_OPERATION 		= 0,
	NC_OCC_READ_FUNCTION	= 1, 
	NC_OCC_READ_DATA		= 2
} nc_cc_command_t;

#include <ringbuffer.h>


#endif /* __NETCACHE_TYPES_H__ */
