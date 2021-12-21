#ifndef __NCAPIX_H__
#define __NCAPIX_H__
#ifdef WIN32
#include <win_uuidwrap.h>
#else
#include <uuid/uuid.h>
#endif


#ifndef EREMCHG
#define 	EREMCHG 	78
#endif

#ifndef EREMOTEIO
#define 	EREMOTEIO 	121
#endif

#ifndef ECONNREFUSED
#define 	ECONNREFUSED 	111
#endif

#ifndef EHOSTDOWN
#define 	EHOSTDOWN 	112
#endif

#ifndef ESHUTDOWN 	
#define ESHUTDOWN 	 	108
#endif

#ifndef EINPROGRESS 	
#define EINPROGRESS 	 	115
#endif

#ifndef EFRESH 	 		
#define EFRESH 	 		 	200
#endif


/*
 * 	NetCache API header file
 *		Indentation rule: tab=4 char width
 * 		Style : KR
 */


#include <netcache_types.h>
#include <sys/statvfs.h>

/*
 * to use NetCache manager, use the following structure
 */
#undef st_atime
#undef st_ctime
#undef st_mtime

#pragma pack(4)
/*
 * NOTE!
 * in HTTP spec. etag limited upto 8KB
 *
 */



typedef		char	nc_devid_t[128];
struct nc_stat {
	nc_uint32_t			st_mode;
	nc_size_t			st_size;
	nc_int32_t			st_ino;
	nc_uid_t			st_uid;
	nc_gid_t			st_gid;
	nc_time_t			st_atime;
	nc_time_t			st_ctime; /*객체가 생성된 시간,초*/
	nc_time_t			st_mtime; /* 객체가 변경된 시간,초 */
	nc_int32_t			st_blksize;
	nc_int32_t			st_blocks;
	nc_time_t			st_vtime; 			/* 해당 캐시가 유효한 시간, expires, max-age등으로 설정되거나, default값으로 설정 */
	nc_time_t			st_xtime; 			/* > 0:valid, 해당 속성이 삭제되는 시간 */
	nc_devid_t			st_devid; 			/* devid indicates etag when http_driver bound */
	nc_obitinfo_t				obi;
	nc_uint32_t					st_originchanged:1;
	nc_uint32_t					st_ifon:1;
	nc_xtra_options_t 			*st_property; 		/* 해당 리스트는 값 (key, value)가 있을 때 사용됨 */
	nc_int16_t  				st_origincode; 		/* 원본 코드 */
	nc_uint8_t 					st_stathit; 		/* nc_hit_status_t의 값 중 PROPERTY관련 hit정보 */
/*
 * 2.8추가
 */
 	nc_uint32_t 			st_viewcount; 		/* 캐싱된 이후 open 횟수*/
 	nc_int32_t 				st_viewindex; 		/* caching의 hotness 척도 (only on disk)*/
};

#define		st_public 			obi.op_bits.public
#define		st_private 			obi.op_bits.priv
#define		st_nocache 			obi.op_bits.nocache
#define		st_onlyifcached 	obi.op_bits.onlyifcached
#define		st_mustreval 		obi.op_bits.mustreval
#define		st_pxyreval 		obi.op_bits.pxyreval
#define		st_immutable 		obi.op_bits.immutable
#define		st_nostore 			obi.op_bits.nostore
#define		st_noxform 			obi.op_bits.noxform



#define 	st_rangeable 			obi.op_bits.rangeable
#define 	st_cookie				obi.op_bits.cookie
#define		st_chunked				obi.op_bits.chunked
#define		st_vary					obi.op_bits.vary
#define		st_cached				obi.op_bits.complete
#define		st_sizeknown			obi.op_bits.sizeknown
#define		st_sizedecled			obi.op_bits.sizedecled
#define		st_template				obi.op_bits.template

#define		st_preservecase 		obi.op_bits.preservecase
#define		st_upgraded 			obi.op_bits.upgraded
#define		st_upgradable 			obi.op_bits.upgradable
#define		st_needoverwrite 		obi.op_bits.needoverwrite
#define		st_frio_inprogress 		obi.op_bits.frio_inprogress
#define		st_stat 				obi.op_bits.stat
#define		st_validuuid 			obi.op_bits.validuuid
#define		st_victim 				obi.op_bits.victim
#define		st_onmdb 				obi.op_bits.onmdb
#define		st_isolated 			obi.op_bits.isolated
#define		st_doc 					obi.op_bits.doc
#define		st_odoc 				obi.op_bits.odoc
#define		st_loaded 				obi.op_bits.loaded
#define		st_created 				obi.op_bits.created
#define		st_memres 				obi.op_bits.memres
#define		st_refreshstat 			obi.op_bits.refreshstat
#define		st_trace 				obi.op_bits.trace
#define		st_xfv 					obi.op_bits.xfv
#define		st_occ 					obi.op_bits.occ
#define		st_ondisk 				obi.op_bits.ondisk
#define		st_pseudoprop 			obi.op_bits.pseudoprop
#define		st_staled 				obi.op_bits.staled
#define		st_mustexpire 			obi.op_bits.mustexpire
#define		st_setbyexpires 		obi.op_bits.setbyexpires
#define		st_swappedin 			obi.op_bits.swappedin


typedef struct nc_volume_stat {
	nc_uint16_t		mode;   /* bitwise comination, (1<<0):READ, (1<<1):WRITE */
	nc_uint16_t		status; /* 0: OFFLINE, 1:ONLINE */	
	nc_uint16_t		collection_time; /* time when this statistis collection started */
	nc_uint32_t		operations;  /* # of operations in 1 mins */
	nc_uint16_t		asio_workers;  /* # of concurrent ASIO threads */
	nc_uint32_t		object_count; /* # of caching objects for this volume */
} nc_volume_stat_t ;


#pragma pack()
typedef struct nc_stat 	nc_stat_t;


/*
 * Extended IO Flags
 */

/* nocache->private로 이름 변경 */
#define 	O_NCX_PRIVATE 				0x10000000
#define 	O_NCX_DONTCHECK_ORIGIN 		0x20000000
#define 	O_NCX_NORANDOM 				0x40000000
#define 	O_NCX_REFRESH_STAT 			0x80000000
/* added */
#define 	O_NCX_NOCACHE 				0x01000000
/*
 * swift/amazone때문에 추가
 * 원본 요청의 결과는 stat-cache에 잠시 보관되나 재사용되지 않음
 */
#define 	O_NCX_MUST_REVAL 			0x02000000
/* ir #31909 */
#define 	O_NCX_MUST_EXPIRE 			0x04000000

/*
 * origin_request_type=2일 때 solproxy가 nc_open할때 mode
 * 값으로 전달하는 값
 */
#define 	O_NCX_TRY_NORANDOM 			0x08000000





/*
 * ORIGIN IO customization phase handler definitions
 */
#define		NC_ORIGIN_PHASE_REQUEST 		1
#define		NC_ORIGIN_PHASE_RESPONSE 		2
typedef struct {
	char  				*method;
	char 				*url;
	int 				status; /* 응답일 경우만 유의미한 값*/
	nc_xtra_options_t 	*properties;
	void 				*cbdata; /* ioctl로 지정한 콜백 데이타*/
} nc_origin_io_command_t;
typedef int (*nc_origin_phase_handler_t)(	int phase_id, 
											nc_origin_io_command_t *command);



#define 	NC_MAX_STRINGLEN 	256
#define 	NCOF_READABLE 				0x0001
#define 	NCOF_WRITABLE 				0x0002

/* introduced from 2.7 */
#define		NCOF_BASIC_AUTH_FROM_USER 	0x0010
struct tag_origin_info {
	char			address[NC_MAX_STRINGLEN];
	char 			prefix[NC_MAX_STRINGLEN];
	char 			user[NC_MAX_STRINGLEN];
	char			pass[NC_MAX_STRINGLEN];
	char			encoding[NC_MAX_STRINGLEN]; /* utf-8? */
	/* introduced in version 2.0.0 */
	unsigned short 	ioflag;
} ;
/*
 * driver-specific LB policy
 */
#define		NC_LBP_POLICY_RR 	1
#define		NC_LBP_POLICY_PS 	2
#define		NC_LBP_POLICY_HASH 	3

#define	nc_mount_context_t		nc_volume_context_t

typedef int (*nc_fill_dir_proc_t) (	
									nc_volume_context_t *mc,
									void *cb, 
									char *name, 
									const nc_stat_t *stbuf,
									nc_off_t off);

/*
 * 요청을 할 때, 실제 데이타를 읽어보기 전에, hit여부를 알 수 없음
 * 다만, inode의 존재 여부를 통해서 hit 가능성만 알 수 있음
 * 그러므로, 객체 존재 유무에 대한 cache hit상태만 정리
 */
typedef enum {
	NC_OS_UNDEFINED 		= 0,  /* 객체가 캐싱되어 있지 않음 */
	NC_OS_MISS 			= 1,  /* 객체가 캐싱되어 있지 않음 */
	NC_OS_HIT 			= 2,  /* 객체가 캐싱되어 있지 않음 */
	NC_OS_NC_MISS 			= 3,  /* 객체가 캐싱되어 있지 않음 */
	NC_OS_NC_HIT 			= 4,  /* 객체가 캐싱되어 있지 않음 */
	NC_OS_REFRESH_HIT 		= 5, /* 원본 객체가 nocache 요청임 */
	NC_OS_REFRESH_MISS 		= 6, /* 원본 객체가 nocache 요청임 */
	NC_OS_OFF_HIT 			= 7, /* 원본 장애라서 캐싱된 객체 정보 전달 */
	NC_OS_PROPERTY_HIT		= 8, /* 객체 에러(notfound등)가 계속 유효함 */
	NC_OS_PROPERTY_MISS		= 9, /* 객체 에러이지만 캐싱되지 않은 상태일 때 */
	NC_OS_PROPERTY_REFRESH_HIT	= 10, /* 객체 에러가 캐싱되어있으며 유효기간 지나서 다시 체크했더니 동일 */
	NC_OS_PROPERTY_OFF_HIT		= 11, /* 객체 에러가 캐싱되어있으며 원본이 다운된 상태 */
	NC_OS_NOTFOUND 			= 12, /* 원본 장애라서 캐싱된 객체 정보 전달 */
	NC_OS_PROPERTY_REFRESH_MISS     = 13, /* 캐시를 사용하지않고 원본 조회 */
	NC_OS_BYPASS     		= 14  /* ir #25994 : 추가, nocache또는 private인 경우 향후 private인 경우 제외해야함 */
} nc_hit_status_t;
typedef void 	(*nc_cache_monitor_t)(void *cb, nc_hit_status_t hs);

/*
 * 하나의 request를 처리하기위해서 여러 번의 원본 요청이 발생할 수 있으므로
 * 감안하여 monitor 콜백함수를 구현해야함
 */
typedef void 	(*nc_origin_monitor_t)(void *cb, char *method, char *origin, char *request, char *reply, double elapsed, double sentb, double receivedb);
typedef void 	(*nc_origin_monitor2_t)(nc_uint32_t txid, nc_xtra_options_t *req_headers, void *cb, char *method, char *origin, char *request, char *reply, double elapsed, double sentb, double receivedb, char *infostring);

/*
 * 2.5 feature
 */

typedef enum {
	NC_FRESH 			= 0, 			/* 아직 사용가능한 상태 */
	NC_REFRESH_FRESH 	= 1, 			/* 유효기간 지나서 체크해본 결과 사용가능한 상태*/
	NC_STALE_BY_ETAG 	= 2, 			/* 캐싱의 ETAG값이 상이하여 STALE처리 됨*/
	NC_STALE_BY_MTIME 	= 3, 			/* 객체 변경시간이 상이하여 STALE처리 됨 */
	NC_STALE_BY_OTHER 	= 4, 			/* 여타 다른 이유로 STALE처리됨 */
	NC_STALE_BY_OFFLINE = 5, 			/* 여타 다른 이유로 STALE처리됨 */
	NC_STALE_BY_SIZE  	= 6,
	NC_STALE_BY_ALWAYS  = 7,
	NC_STALE_BY_ORIGIN_OTHER  = 8,
	NC_VALIDATION_CODENEXT  = 9,
	NC_VNONE				= 10
} nc_validation_result_t;

/*
 * 캐시된 객체에 대한 모든 정보
 * 해당 정보는 오직 조회용으로만 사용되어야하며, 변경되지 말아야함
 */
typedef struct nc_object_info_tag {
	char 				*cache_path; 		/* 원본 객체의 URL 경로 */
	char  				*cache_key;			/* 객체에 대한 URL */
	nc_uint32_t 		vary:1;				/* 객체가 vary 객체인 경우 1, 아니면 0 */
	nc_uint32_t 		rangeable:1;			/* 객체가 range요청을 허용하는 경우엔 1, 아니면 0 */
	nc_uint32_t 		chunked:1;			/* 객체가 chunked transfer-encoding인 경우 */
	nc_uint32_t 		complete:1;			/* 객체가 풀 캐싱된 경우엔 1, 아니면 0 */
	nc_uint32_t 		sizeknown:1	;		/* 객체의 크기가 확정된 경우엔 1, 아니면 0 */
	nc_uint32_t 		sizedecled:1;		/* 1: 원본이 객체크기를 알려줌 */
	nc_uint32_t 		mustrevalidate:1;	/* 객체가 항상 revalidation이 필요하면 1, 아니면 0 */
	nc_uint32_t 		priv:1;			/* 객체가 private 객체 */
	nc_uint64_t 		size; 				/* 객체의 크기 */
	nc_time_t 	 		modification_time; 	/* 객체가 변경된 시간, 0이면 해당 필드 무효, UTC time */
	nc_time_t 	 		valid_time; 		/* 캐시 만료시간 , UTC time*/
	nc_time_t 	 		caching_time; 		/* 캐시가 생성된 시간 , UTC time*/
	nc_uint32_t 	 	origin_result; 		/* 원본에서 객체를 가져올 때 받은 응답 코드 */
	char 				*device_id;			/* 객체의 unique ID string, HTTP의 경우 etag, 존재하지 않으면 NULL */
	nc_xtra_options_t 	*property;			/* 객체의 속성 정보, 즉 헤더 정보 */
} nc_object_info_t;
typedef nc_validation_result_t (*nc_cache_validator_t)(nc_object_info_t *object, nc_stat_t *stat, void *cbdata);

/*
 * NetCache Parameter Control Command
 * NC_GLOBAL_XXX means the command affects NetCache globally.
 */

#define 	NC_GLOBAL_CACHE_PATH		1000
#define 	NC_GLOBAL_CACHE_MAX_INODE	1001
#define 	NC_GLOBAL_CACHE_MEM_SIZE	1002
#define 	NC_GLOBAL_CACHE_MAX_ASIO	1003
#define 	NC_GLOBAL_CACHE_DISK_RA		1004		/* origin read ahead block 갯수 */
#define 	NC_GLOBAL_CACHE_NETWORK_RA	1005		/* disk-cache read ahead block 갯수 */
#define 	NC_GLOBAL_CACHE_RA_MAXTHREADS	1006
#define 	NC_GLOBAL_CACHE_RA_MINTHREADS	1007
#define 	NC_GLOBAL_CACHE_POSITIVE_TTL	1008
#define 	NC_GLOBAL_CACHE_NEGATIVE_TTL	1009
#define 	NC_GLOBAL_CACHE_DISK_HIGH_WM	1010
#define 	NC_GLOBAL_CACHE_DISK_LOW_WM		1011
#define 	NC_GLOBAL_ENABLE_COMPACTION		1013
#define 	NC_GLOBAL_CACHE_BLOCK_SIZE		1014
#define 	NC_GLOBAL_DEFAULT_USER			1015
#define 	NC_GLOBAL_DEFAULT_GROUP			1016
#define 	NC_GLOBAL_STRICT_CRC			1017
#define 	NC_GLOBAL_ASIO_TIMER			1018
#define 	NC_GLOBAL_CLUSTER_IP 			1019
#define 	NC_GLOBAL_LOCAL_IP 				1020
#define 	NC_GLOBAL_CLUSTER_TTL 			1021
/*since 2.5*/
#define 	NC_GLOBAL_BACKUP_PROPERTYCACHE	1022
/*since 2.5 revision 17 */
#define 	NC_GLOBAL_ENABLE_COLD_CACHING	1023
#define 	NC_GLOBAL_COLD_RATIO		1024
#define 	NC_GLOBAL_MAX_STATCACHE 	1025	/* obsolete since 3.0 */
#define 	NC_GLOBAL_MAX_PATHLOCK		1026

/*since 2.6 revision 372 */
#define		NC_GLOBAL_BLOCK_SPIN_INTERVAL 	1027
/*
 * NC_GLOBAL_CACHE_MAX_STAT affects all instances of driver
 * because the stat cache kept in driver instance wise.
 */
//#define		NC_GLOBAL_CACHE_MAX_STAT	1008
/*
 * 2014.12.3
 * value : CRC 생성에 필요한 바이트 수
 * 0인 경우는 기존 방식의 전체 chunk 데이타에 대한 CRC생성이고
 * 0보다 큰 경우는 해당 크기 chunk의 앞에서 계산하고, 
 * 또한 뒤에서 계산하여 그 둘을 조합하여 생성
 */
#define			NC_GLOBAL_ENABLE_FASTCRC	1028

/*
 * ir #27231
 */
#define 	NC_GLOBAL_CACHE_MAX_EXTENT	1029		/* obsolete in 3.0 */
/*
 * force close option
 */
#define 	NC_GLOBAL_ENABLE_FORCECLOSE	1030
#define 	NC_GLOBAL_STAT_BACKUP		1031		/* obsolete since 3.0 */

/*
 * read-ahead (3.0에서 신규 도입, range-io가 가능한 객체에 한정)
 */
#define 	NC_GLOBAL_READAHEAD_MB		1032		/* read-ahead크기 (단위:MB), obsolete*/
#define		NC_GLOBAL_INODE_MEMSIZE		1033		/* inode 정보 할당관리에서 사용할 메모리 크기 (MB) */
/* 2019.10.22 added */
#define		NC_GLOBAL_FALSE_READ_LIMIT	1034		/* disk false read 한계치, false read 비율이 이 이상증가하면 read-ahead 감소*/

/*
 * param:
 * 	1 : preseve case
 *  2 : ignore case
 */
#define		NC_IOCTL_PRESERVE_CASE			5000
#define		NC_IOCTL_STORAGE_CHARSET		5001
#define		NC_IOCTL_LOCAL_CHARSET			5002
#define 	NC_IOCTL_STORAGE_TIMEOUT 		5003
/* unit : seconds*/

#define 	NC_IOCTL_STORAGE_PROBE_INTERVAL 5004
/*  0 < freq <= 50 */

#define 	NC_IOCTL_STORAGE_PROBE_COUNT 	5005
/* introduced in version 2.0.0 */
#define 	NC_IOCTL_WRITEBACK_BLOCKS 		5006
#define 	NC_IOCTL_STORAGE_RETRY			5007
#define 	NC_IOCTL_WEBDAV_USE_PROPPATCH	5008
#define 	NC_IOCTL_WEBDAV_ALWAYS_LOWER	5009
/* introduced in version 2.2 */
#define		NC_IOCTL_NEGATIVE_TTL 		5010
#define		NC_IOCTL_POSITIVE_TTL 		5011
/* introduced since version 2.2.0 */
#define		NC_IOCTL_ADAPTIVE_READAHEAD 5012
#define		NC_IOCTL_FRESHNESS_CHECK 	5013
/* since 2.5 */
#define		NC_IOCTL_HTTP_FILTER 		5014
#define		NC_IOCTL_SET_VALIDATOR 		5015
#define 	NC_IOCTL_CACHE_MONITOR 		5016
#define 	NC_IOCTL_PROXY 				5017
#define 	NC_IOCTL_CACHE_VALIDATOR 	5018
#define 	NC_IOCTL_ORIGIN_MONITOR  	5019
#define 	NC_IOCTL_FOLLOW_REDIRECTION 5020 /* HTTPN driver용 */
#define 	NC_IOCTL_ORIGIN_MONITOR_CBD 5021 /* origin montior호출시 전달할 data */
#define 	NC_IOCTL_ORIGIN_MONITOR_CBD 5021 /* origin montior호출시 전달할 data */
#define 	NC_IOCTL_USE_HEAD_FOR_ATTR 	5022 /* property요청시 HEAD 사용 여부*/
#define 	NC_IOCTL_UPDATE_ORIGIN 		5023 /* parent 주소 변경 요청 */
#define 	NC_IOCTL_SET_IO_VALIDATOR 	5024 /* 읽기 요청 중에 원본 요청이 유효한지 확인하기위한 콜백등록*/
#define 	NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER 	5025 /* netcache core가 원본에 요청을 전송하기 전에, 
							      					  *	URL및 헤더 정보를 APP에 전달하는 콜백등록*/
#define 	NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER 	5026 /* 오리진의 요청을 수신 후, 수신된 
							      					      *	URL및 헤더 정보를 APP에 전달하는 콜백등록*/
#define		NC_IOCTL_SET_LAZY_PROPERTY_UPDATE 	5027 	/* inode 생성 후 객체 속성 변경 콜백 */
#define 	NC_IOCTL_SET_ORIGIN_PHASE_REQ_CBDATA 	5028 /* request의 callback 호출시 전달할 callback-data */
#define 	NC_IOCTL_SET_ORIGIN_PHASE_RES_CBDATA 	5029 /* response의 callback 호출시 전달할 callback-data */
/*
 * added after 2.6
 */
#define	NC_IOCTL_SET_ORIGIN_PATH_CHANGE_CALLBACK 	5030 	/* redirection시 수신한 경로명에 대한 콜백요청*/
#define	NC_IOCTL_SET_MAKE_REDIR_ABSOLUTE 			5031 	/* redirection응답 수신시 경로명을 절대 경로로 인식*/
/*
 * added after 2.7
 */

#define NC_IOCTL_SET_LB_POLICY                   	5032	/* 2.7이전 버전과의 호환성 유지용 */
#define	NC_IOCTL_SET_LB_ORIGIN_POLICY			5032	/* origin LB의 정책 결정 */
#define	NC_IOCTL_SET_LB_PARENT_POLICY			5033	/* parent LB의 정책 결정 */


#define NC_IOCTL_UPDATE_PARENT 					5034 	/* 원본 주소 변경 요청 */

/*
 * 2.8 timeout
 */

#define NC_IOCTL_CONNECT_TIMEOUT 					5035
#define NC_IOCTL_TRANSFER_TIMEOUT					5036

/*
 * 2016/12/23 additions
 */
#define	NC_IOCTL_HTTPS_SECURE						5037 /* 0: insecure */
#define	NC_IOCTL_HTTPS_CERT							5038 /* cert file path */
#define	NC_IOCTL_HTTPS_CERT_TYPE					5039 /* cert file format: default "PEM" */
#define	NC_IOCTL_HTTPS_SSLKEY						5040 /* private key: default format "PEM" */
#define	NC_IOCTL_HTTPS_SSLKEY_TYPE					5041 /* private key file format: default "PEM" */
#define	NC_IOCTL_HTTPS_CRLFILE						5042 /* specify a Certificate Revocation List file : default NULL */
#define	NC_IOCTL_HTTPS_CAINFO						5043 /* ca path : default NULL(built-in system specific) */
#define	NC_IOCTL_HTTPS_CAPATH						5044 /* Path to CA cert bundle: default NULL */
#define NC_IOCTL_HTTPS_FALSESTART					5045 /* TLS fault start option: default FALSE */
#define NC_IOCTL_HTTPS_TLSVERSION					5046  /* in string : example) 1.1 */

/*
 * 2017/4/11 addition
 */
typedef int (*nc_offline_policy_proc_t)(char *url,  int httpcode, void *usercontext);
#define NC_IOCTL_SET_OFFLINE_POLICY_FUNCTION		5047  
#define NC_IOCTL_SET_OFFLINE_POLICY_DATA			5048 


// NEW 2019-11-01 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
// - origin, parent 가 offline 상태로 변경시킬 연속 오류 횟수 설정
#define NC_IOCTL_SET_LB_FAIL_COUNT_TO_OFFLINE		5049

//
// origin_request_type = 2인 경우 FULL-GET을 요청하고
// 읽어들이는 최소 단위
//
#define NC_IOCTL_SET_DEFAULT_READ_SIZE              5050

// NEW 2020-01-22 huibong DNS cache timeout 설정 기능 추가 (#32867)
//                - curl 상의 dns cache timeout 설정 (단위: sec)
#define NC_IOCTL_SET_DNS_CACHE_TIMEOUT				5051
//
//
// nc_read timeout(in sec)
// default : 30 secs
// nc_read ()호출후 요청한 읽기 바이트보다 작은 값이 리턴되는 경우
// nc_errno를 반드시 체크해서 ETIMEDOUT이 세팅되었나 확인해야함
//
#define NC_IOCTL_SET_NCREAD_TIMEOUT					5052
#	define		DEFAULT_NCREAD_TIMEOUT								30

#define 	NC_IOCTL_ORIGIN_MONITOR2				5053

/*
 * 2020.9.4
 */
#define		NC_IOCTL_SET_PROBING_URL				5054

#define	NC_IOCTL_MAX								NC_IOCTL_SET_PROBING_URL
#define	NC_IOCTL_BASE								5000


/*
 * EASY log mask for nc_setup_log
 */
#define	NC_DEBUG	((nc_uint32_t)1<<0)
#define	NC_TRACE	((nc_uint32_t)1<<1)
#define	NC_INFO		((nc_uint32_t)1<<2)
#define	NC_WARN		((nc_uint32_t)1<<3)
#define	NC_ERROR	((nc_uint32_t)1<<4)
#define	NC_TRIGGER	((nc_uint32_t)1<<5)
#define	NC_INODE 	((nc_uint32_t)1<<6)
#define	NC_BLOCK 	((nc_uint32_t)1<<7)
#define	NC_PLUGIN  	((nc_uint32_t)1<<8)
#define	NC_CACHE	((nc_uint32_t)1<<9)
#define	NC_ASIO		((nc_uint32_t)1<<10)
#define	NC_PERF		((nc_uint32_t)1<<11)
#define	NC_DAEMON	((nc_uint32_t)1<<12)
#define	NC_API		((nc_uint32_t)1<<13)
#define	NC_JOB		((nc_uint32_t)1<<14)
#define	NC_CLUSTER	((nc_uint32_t)1<<15)
#define	NC_STAT		((nc_uint32_t)1<<16)


/*
 * VERSION 2.2 extentions
 */


#ifdef WIN32

#define 	CALLSYNTAX	

#ifdef BUILD_DLL
#define	PUBLIC_IF	__declspec(dllexport)
#else
#define	PUBLIC_IF	__declspec(dllimport)
#endif

#else /* UNIX */

#define 	CALLSYNTAX	
#define PUBLIC_IF
#endif



#ifdef __cplusplus
extern "C" {
#endif
/*
 * exported public interface definitions
 */
PUBLIC_IF CALLSYNTAX char * 				nc_version(char *buf, int len);
PUBLIC_IF CALLSYNTAX int  					nc_init(void);
PUBLIC_IF CALLSYNTAX void 					nc_change_mask(nc_uint32_t mask);


#define	nc_create_mount_context				nc_create_volume_context
#define	nc_destroy_mount_context			nc_destroy_volume_context
PUBLIC_IF CALLSYNTAX nc_volume_context_t * 	nc_create_volume_context(char *, char *prefix, nc_origin_info_t *oi, int oicnt);
PUBLIC_IF CALLSYNTAX int 					nc_add_origin_cluster(nc_volume_context_t *, char *prefix, nc_origin_info_t *oi, int oicnt);
PUBLIC_IF CALLSYNTAX nc_file_handle_t *  	nc_open_extended(nc_volume_context_t *volume, char *cache_id, char *path, int mode, nc_xtra_options_t *kv);
PUBLIC_IF CALLSYNTAX int 					nc_destroy_volume_context(nc_volume_context_t *);
PUBLIC_IF CALLSYNTAX nc_file_handle_t * 	nc_open(nc_volume_context_t *, char *, int );
PUBLIC_IF CALLSYNTAX int 					nc_ref_count(nc_file_handle_t *fi);
PUBLIC_IF CALLSYNTAX nc_ssize_t 			nc_read(nc_file_handle_t *fi, char *buf, nc_off_t off, nc_size_t len);
PUBLIC_IF CALLSYNTAX int 					nc_close(nc_file_handle_t *fi, int force);
PUBLIC_IF CALLSYNTAX int 					nc_getattr(nc_volume_context_t *volume, char *path, nc_stat_t *s);
PUBLIC_IF CALLSYNTAX int 					nc_getattr_extended(nc_volume_context_t *volume, char *cache_id, char *path, nc_stat_t *s, nc_xtra_options_t *kv, mode_t );
PUBLIC_IF CALLSYNTAX int 					nc_set_param(int cmd, void *val, int vallen);
PUBLIC_IF CALLSYNTAX int 					nc_setup_log(nc_uint32_t mask, char *path, nc_int32_t szMB, nc_int32_t rotate);
PUBLIC_IF CALLSYNTAX int 					nc_unlink(nc_volume_context_t *volume, char *file, nc_xtra_options_t *req, nc_xtra_options_t **res);
PUBLIC_IF CALLSYNTAX int 					nc_lock(nc_volume_context_t *volume, char *src, char *dest);
PUBLIC_IF CALLSYNTAX nc_ssize_t				nc_write(nc_file_handle_t *handle, char *buf, nc_off_t offset, size_t size);
PUBLIC_IF CALLSYNTAX int 					nc_ioctl(nc_volume_context_t *volume, int cmd, void *valptr, int vallen);
PUBLIC_IF CALLSYNTAX int 					nc_purge(nc_volume_context_t *volume, char *pattern, int iskey, int ishard);
PUBLIC_IF CALLSYNTAX int					nc_map_to_cache_name(nc_volume_context_t *volume, char *path, char *buffer, int len);
PUBLIC_IF CALLSYNTAX int 					nc_shutdown(void);
PUBLIC_IF CALLSYNTAX int 					nc_load_driver(char *name, char *path);
PUBLIC_IF CALLSYNTAX nc_file_handle_t *  	nc_handle_of(nc_volume_context_t *volume, char *path) ;
PUBLIC_IF CALLSYNTAX nc_file_handle_t * 	nc_create(nc_volume_context_t *volume, char *path, mode_t mode);
PUBLIC_IF CALLSYNTAX int 					nc_flush(nc_file_handle_t *fi);
#ifndef WIN32
PUBLIC_IF CALLSYNTAX int 					nc_utimens(nc_volume_context_t *volume, char *path, const struct nc_timespec tv[]);
#endif
PUBLIC_IF CALLSYNTAX int 					nc_truncate(nc_volume_context_t *volume, char *path, nc_off_t offset);
PUBLIC_IF CALLSYNTAX int  					nc_ftruncate(nc_file_handle_t *inode , nc_off_t off);
PUBLIC_IF CALLSYNTAX int 					nc_enum_file_property(nc_file_handle_t *fi, int (*do_it)(char *key, char *val, void *cb), void *cb);
PUBLIC_IF CALLSYNTAX int 					nc_errno(void);
/*
 * nc_check_memory(void *p, size_t sz)
 * 	@p : netcache core에서 할당해서 리턴한 메모리 포인터
 * 	@sz : 현재 사용안함
 * RETURN
 * 	메모리가 정상이면 TRUE 리턴 비정상이면 FALSE 리턴
 */
PUBLIC_IF CALLSYNTAX int 					nc_check_memory(void *memp, size_t sz);




/*********************************************************************************************
 * VERSION 2.2 addition 
 *
 */
#ifdef WIN32
#pragma pack(8)
#else
#pragma pack(4)
#endif
typedef struct {
	uuid_t 			jobid;
	nc_uint8_t 		priority; /* JP_HIGHER, JP_NORMAL, JP_LOWER */
	nc_uint8_t 		command;  /* JC_PURGE/JC_PRELOAD */
	nc_int32_t 		total;
	nc_int32_t 		completed;  /* success count= completed - error, inprogress count=total - complete */
	nc_int32_t 		error;
	nc_uint8_t 		**path;  	/* string arrary pointer, max index = error-1 */
} nc_job_report_t;
#pragma pack()

PUBLIC_IF CALLSYNTAX int					nc_add_partition(char *path, int weight);

/* after 2.2: 2.5 */
PUBLIC_IF CALLSYNTAX nc_xtra_options_t * 	kv_create_pool(void *cb, void *(*allocator)(void *cb, size_t sz));
PUBLIC_IF CALLSYNTAX nc_xtra_options_t * 	kv_create_pool_d(void *cb, void *(*allocator )(void *, size_t), const char *f, int l);
PUBLIC_IF CALLSYNTAX char *					kv_dump_oob(char *buf, int l, nc_xtra_options_t *opt);
PUBLIC_IF CALLSYNTAX int 					kv_for_each(nc_xtra_options_t *, int (*do_it)(char *, char *, void *), void *);
PUBLIC_IF CALLSYNTAX int 					kv_for_each_and_remove(nc_xtra_options_t *root, int (*do_it)(char *key, char *val, void *cb), void *cb);
PUBLIC_IF CALLSYNTAX void 					kv_destroy(nc_xtra_options_t *root);
PUBLIC_IF CALLSYNTAX void * 				kv_add_val(nc_xtra_options_t *root, char *key, char *val);
PUBLIC_IF CALLSYNTAX void * 				kv_add_val_d(nc_xtra_options_t *root, char *key, char *val, const char *f, int l);
PUBLIC_IF CALLSYNTAX void * 				kv_add_val_extended(nc_xtra_options_t *, char *, char *, const char *f, int l, int );
PUBLIC_IF CALLSYNTAX nc_xtra_options_t * 	kv_create(char *f, int l);
PUBLIC_IF CALLSYNTAX nc_xtra_options_t * 	kv_create_d(char *, int);
PUBLIC_IF CALLSYNTAX nc_xtra_options_t * 	kv_clone(nc_xtra_options_t *root, char *f, int l);
PUBLIC_IF CALLSYNTAX nc_xtra_options_t * 	kv_clone_d(nc_xtra_options_t *root, const char *, int);
PUBLIC_IF CALLSYNTAX int 					kv_replace(nc_xtra_options_t *root, char *key, char *value, int bcase);
PUBLIC_IF CALLSYNTAX void 					kv_set_stat_key(nc_xtra_options_t *root, void *keyid);
PUBLIC_IF CALLSYNTAX int 					kv_remove(nc_xtra_options_t *root, char *key, int casesensitive);
PUBLIC_IF CALLSYNTAX void 					kv_set_pttl(nc_xtra_options_t *root, nc_time_t pttl);
PUBLIC_IF CALLSYNTAX void 					kv_set_nttl(nc_xtra_options_t *root, nc_time_t nttl);

PUBLIC_IF CALLSYNTAX int  					nc_invoke_io(nc_file_handle_t *fi);
PUBLIC_IF CALLSYNTAX int 					nc_result_code(nc_file_handle_t * fi);
PUBLIC_IF CALLSYNTAX char * 				nc_find_property(nc_file_handle_t *fi, char *tag);
PUBLIC_IF CALLSYNTAX nc_int32_t 			nc_block_size();
PUBLIC_IF CALLSYNTAX int 					nc_fgetattr(nc_file_handle_t *fh, nc_stat_t *s);

/*
 *
 * KV 구조 manipulation API
 *
 */
PUBLIC_IF CALLSYNTAX int 					kv_valid(nc_xtra_options_t *opt);
PUBLIC_IF CALLSYNTAX void 					kv_set_raw_code(nc_xtra_options_t *root, int code);
PUBLIC_IF CALLSYNTAX int 					kv_get_raw_code(nc_xtra_options_t *root);
PUBLIC_IF CALLSYNTAX void 					kv_oob_write_eot(nc_xtra_options_t *kv);
PUBLIC_IF CALLSYNTAX void 					kv_oob_set_error(nc_xtra_options_t *kv, int err);
PUBLIC_IF CALLSYNTAX int 					kv_oob_error(nc_xtra_options_t *kv, int error);
PUBLIC_IF CALLSYNTAX void *					kv_add_val(nc_xtra_options_t *root, char *key, char *val);
PUBLIC_IF CALLSYNTAX char * 				kv_find_val(nc_xtra_options_t *root, char *key, int bcase);
PUBLIC_IF CALLSYNTAX char * 				kv_find_val_extened(nc_xtra_options_t *root, char *key, int bcase, int ignore);
PUBLIC_IF CALLSYNTAX void 					kv_destroy(nc_xtra_options_t *root) ;
PUBLIC_IF CALLSYNTAX int  					kv_for_each(nc_xtra_options_t *root, int (*do_it)(char *key, char *val, void *cb), void *cb);
PUBLIC_IF CALLSYNTAX char * 				kv_dump_property(nc_xtra_options_t *root, char *buf, int len);
PUBLIC_IF CALLSYNTAX void 					kv_oob_command(nc_xtra_options_t *kv, char *cmd, nc_size_t len);
PUBLIC_IF CALLSYNTAX ssize_t 				kv_oob_write(nc_xtra_options_t *kv, char *data, size_t len);
PUBLIC_IF CALLSYNTAX ssize_t 				kv_oob_read(nc_xtra_options_t *kv, char *data, size_t len, long);
PUBLIC_IF CALLSYNTAX int 					kv_setup_ring(nc_xtra_options_t *kv, size_t bufsiz);
PUBLIC_IF CALLSYNTAX void 					kv_set_opaque(nc_xtra_options_t *root, void *opaque);
PUBLIC_IF CALLSYNTAX void *					kv_get_opaque(nc_xtra_options_t *root);
PUBLIC_IF CALLSYNTAX void 					kv_set_client(nc_xtra_options_t *kv, void *client, int len);
PUBLIC_IF CALLSYNTAX void * 				kv_get_client(nc_xtra_options_t *kv);
PUBLIC_IF CALLSYNTAX void 					kv_oob_lock(nc_xtra_options_t *opt);
PUBLIC_IF CALLSYNTAX void 					kv_oob_unlock(nc_xtra_options_t *opt);


/*
 * (obsolete)
 * PUBLIC_IF CALLSYNTAX int 					nc_verify_storage(char *partition);
 *
 */
PUBLIC_IF CALLSYNTAX nc_file_handle_t * 	nc_open_extended2(	nc_volume_context_t *,  /* [IN] volume */
																char *, 				/* [IN] object path in origin */
																char *, 				/* [IN] cache - key */
																int , 					/* [IN] mode */
																nc_stat_t *s, 			/* [OUT] stat */
																nc_xtra_options_t *kv);	/* [IN] properties */
PUBLIC_IF CALLSYNTAX nc_int32_t 			nc_inode_uid(nc_file_handle_t *fh);

/* solproxy에서 참조하는 trace.h, util.h의 일부분 추가 */

PUBLIC_IF CALLSYNTAX void * 	__nc_malloc(size_t n, int category, const char *f, int l);
PUBLIC_IF CALLSYNTAX void * 	__nc_calloc(size_t n, size_t m, int category, const char *f, int l);
PUBLIC_IF CALLSYNTAX void 		__nc_free(void *p, const char *file, int lno);
PUBLIC_IF CALLSYNTAX void * 	__nc_realloc(void * old_, size_t reqsiz, int category, const char *f, int l);
PUBLIC_IF CALLSYNTAX char * 	__nc_strdup(const char *s, int category, const char *f, int l);



/* 2.9 추가 */
PUBLIC_IF CALLSYNTAX int nc_close_allowable(nc_file_handle_t *fh);

/* 3.0 추가 */
typedef void (*nc_apc_open_callback_t)(nc_file_handle_t *, nc_stat_t *, int , void *);
typedef void (*nc_apc_read_callback_t)(nc_file_handle_t *handle, int xfered, int error, void *cbdata);
typedef void (*apc_block_ready_callback_t)(fc_blk_t *block, int xfered, int error, void *cbdata);
PUBLIC_IF CALLSYNTAX nc_file_handle_t *	nc_open_extended_apc(	nc_volume_context_t *, 
																char *, 
																char *, 
																int , 
																nc_stat_t *, 
																nc_xtra_options_t *, 
																nc_apc_open_callback_t callback, 
																void *callback_data);
PUBLIC_IF CALLSYNTAX nc_ssize_t  		nc_read_apc(			nc_file_handle_t *fi, 
																char *buf, 
																nc_off_t off, 
																nc_size_t len, 
																nc_apc_read_callback_t callback, 
																void *callbackdata);

PUBLIC_IF CALLSYNTAX nc_size_t 			nc_readahead_bytes();
PUBLIC_IF CALLSYNTAX int 				nc_raw_dump_report(void  (*dump_proc)(char *));


 
/*
 * Since 3.0
 * nc_open()함수의 동작 control용
 * for command, look at NC_OCC_XXXXX
 *  command 				value type 				len
 *	NC_OCC_OPERATION 		string					0 (not used)
 *	NC_OCC_READ_FUNCTION 	int (*)(buf,len)		0 (not used)
 *	NC_OCC_READ_DATA 		void *					0 (not used)
 */

PUBLIC_IF CALLSYNTAX nc_open_cc_t *  		nc_open_cc(		nc_open_cc_t * 		ifexist,
															nc_cc_command_t 	command,
															void 				*value,
															int   				vallen);
PUBLIC_IF CALLSYNTAX void  					nc_close_cc(	nc_open_cc_t *);
PUBLIC_IF CALLSYNTAX void * 				nc_lookup_cc(nc_open_cc_t * pcc, nc_cc_command_t command);

PUBLIC_IF CALLSYNTAX nc_file_handle_t * 	nc_open_extended_oc(nc_volume_context_t *,  /* [IN] volume */
																char *, 				/* [IN] object path in origin */
																char *, 				/* [IN] cache - key */
																int , 					/* [IN] mode */
																nc_stat_t *s, 			/* [OUT] stat */
																nc_xtra_options_t *kv,	/* [IN] properties */
																nc_open_cc_t *cc);		/* [IN] Custom Open Context */





PUBLIC_IF CALLSYNTAX int nc_raw_dump_report(void  (*dump_proc)(char *));
PUBLIC_IF CALLSYNTAX long long nc_get_heap_allocated();
PUBLIC_IF CALLSYNTAX char* nc_get_heap_alloc_counter_x();

#define T_DEBUG 	(1<<0)
#define T_TRACE		(1<<1)
#define T_INFO  	(1<<2)
#define T_WARN  	(1<<3)
#define T_ERROR 	(1<<4)
#define T_TRIGGER  	(1<<5)
#define T_PERF 		(1<<11)
#define T_DAEMON 	(1<<12)
#define T_STAT		(1<<16)

int trc_is_on(nc_uint32_t x);
void trace(nc_uint32_t leve, char *fmt, ...);
void trace_f(nc_uint32_t level, const char *func, char *fmt, ...);

#ifndef TRC_BEGIN
#define TRC_BEGIN(x)
#endif

#ifndef TRC_END
#define TRC_END
#endif

#if !defined(TRACE)

#define TRACE_ON(x) 		trc_is_on(x)

#define	TRACE(x)		TRACE_NF x

#define	TRACE_N(mask, ...)	if (TRACE_ON(mask)) trace(mask, __VA_ARGS__)
#define	TRACE_NF(mask, ...)	if (TRACE_ON(mask)) trace_f(mask, __func__, __VA_ARGS__)
#define	TRACE_X(mask, sign, ...)	if (TRACE_ON(mask)) trace_f(mask, sign, __VA_ARGS__)
#endif /*TRACE*/

#ifndef ASSERT

/* CHG 2018-06-21 huibong 로컬변수 사용으로 인한 중복 선언 warning 발생 코드 제거 (#32205) */
#define	ASSERT(x)  	if (!(x)) { \
						TRACE((g__trace_error, (char *)"Error in Expr '%s' at %d@%s\n", \
									#x, __LINE__, __FILE__));  \
						__builtin_trap(); \
					}
#endif /*end of assert*/

#ifndef DEBUG_ASSERT
#ifdef NC_RELEASE_BUILD

#define	DEBUG_ASSERT(x)

#else
#define	DEBUG_ASSERT(x)  	if (!(x)) { \
								TRACE((g__trace_error, (char *)"Error in Expr '%s' at %d@%s\n", \
									#x, __LINE__, __FILE__));  \
						TRAP; \
					}
#endif
#endif



extern int   	g__trace_error;


#ifdef __cplusplus
};
#endif

#endif /* __NCAPIX_H__ */
