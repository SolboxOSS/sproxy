#ifndef __HTTPD_H__
#define __HTTPD_H__

#include "vstring.h"

//#define		ZIPPER	/* zipper로 동작하는 경우 컴파일시 파라미터로 받는다. */
//#define 	USE_SIGTERM_SIGNAL			/* root 권한으로 실행 되는 경우가 아니면 signal 방식을 사용할수 없다. */
#define		PROG_VERSION "1.5.2"	/* rpm 버전에도 사용된다 */

#ifdef ZIPPER
#pragma message("Enable AdZipper")
#define		PROG_SHORT_NAME "zipper"
#define 	PROG_NAME "Solbox Cloud Streaming Server"
#else
#define		PROG_SHORT_NAME "solproxy"
#define 	PROG_NAME "Solbox Cloud Storage Accelerator"
#endif
#define 	INSTALL_PREFIX 	"/usr"
#define 	MAX_URI_SIZE 	4096
#define 	MAX_HEADER_NAME_LENGTH	64
#define 	NAME_SHM_PROG	"SolProxy_config"

#define		OTHERS_VOLUME_NAME 	"others"			/* 특정 볼륨에 속하지 않는 요청을 처리하기 위한 가상 볼륨 */
#define		CHECK_VOLUME_NAME 	"contents.check"	/* 모니터링 요청을 처리하기 위한 가상 볼륨 */
#ifdef ZIPPER
#define 	CHECK_FILE_PATH		"check.mp3"
#else
#define 	CHECK_FILE_PATH		"check.txt"
#endif
#define 	CHECK_HEADER_NAME	"X-Contents-Check"
#define 	UNDEFINED_PROTOCOL		1000

//#define 	BT_TIMER_VER2	/* bt_timer GPL 라이선스 제거 버전 사용시 체크 */
#define 	USE_CHECK_TIMER
#ifdef		USE_CHECK_TIMER
#pragma message("enable check timer")
extern void *gscx__timer_wheel_base; //bt_timer용
#ifdef BT_TIMER_VER2
#define 	CHECK_TIMER(func,req,x) {\
	if (1 == gscx__config->ncapi_hang_detect) { \
		bt_timer_t timer_remove; \
		snprintf(req->msg, sizeof(req->msg)-1, "NCAPI(%s) response delayed(%s,%d). tid(%d)", func, __FILE__, __LINE__, gettid());\
		bt_init_timer(&timer_remove, "init timer", 0); \
		bt_set_timer(gscx__timer_wheel_base, &timer_remove, gscx__config->ncapi_hang_detect_timeout * 1000, expire_timer_callback, (void *)req); \
		x; \
		while (bt_del_timer_v2(gscx__timer_wheel_base, &timer_remove) < 0) bt_msleep(1); \
		while (bt_destroy_timer_v2(gscx__timer_wheel_base, &timer_remove) < 0) bt_msleep(1); \
	} \
	else { \
		x; \
	} \
}
#else	//BT_TIMER_VER2
#define 	CHECK_TIMER(func,req,x) {\
	if (1 == gscx__config->ncapi_hang_detect) { \
		bt_timer_t timer_remove; \
		snprintf(req->msg, sizeof(req->msg)-1, "NCAPI(%s) response delayed(%s,%d). tid(%d)", func, __FILE__, __LINE__, gettid());\
		bt_init_timer(&timer_remove, "init timer", 0); \
		bt_set_timer(gscx__timer_wheel_base, &timer_remove, gscx__config->ncapi_hang_detect_timeout * 1000, expire_timer_callback, (void *)req); \
		x; \
		bt_del_timer(gscx__timer_wheel_base, &timer_remove);\
		bt_destroy_timer(gscx__timer_wheel_base, &timer_remove);\
	} \
	else { \
		x; \
	} \
}
#endif	//BT_TIMER_VER2



#else
#pragma message("disable check timer")
#define 	CHECK_TIMER(func,req,x) {\
	x;\
}
#endif

typedef enum {
	SCX_HTTP 	= 0,
	SCX_HTTPS,
	SCX_HTTP_TPROXY,
	SCX_CHECK_SERVICE,
	SCX_MANAGEMENT,
	SCX_RTMP,
	SCX_PORT_TYPE_CNT
} scx_port_type_t;

static const char *_scx_port_name[] = {
    "http",			/* SCX_HTTP */
    "https",		/* SCX_HTTPS */
	"tproxy",		/* SCX_HTTP_TPROXY */
	"check",		/* SCX_CHECK_SERVICE */
	"mgmt",			/* SCX_MANAGEMENT */
	"rtmp",			/* SCX_RTMP */
	"null",			/* SCX_PORT_TYPE_CNT */
};


struct MHD_Connection;
struct streaming_tag;
struct site_tag;

struct service_core_tag;

typedef enum {
	LUA_TRIGGER_CONTINUE	= 0, 	/* 다음 trigger를 실행한다. */
	LUA_TRIGGER_BREAK 		= 1,	/* trigger를 더이상 실행하지 않고 다음 단계를 진행한다. */
	LUA_TRIGGER_RETURN 		= 2 	/* 더이상 진행하지 않고 client에게 response를 전달한다. */
} scx_trigger_ret_t;

typedef enum {
	STAT_TYPE_DELIVERY		= 0, 	/* Delivery type으로 통계 기록 */
	STAT_TYPE_STREAMING 	= 1 	/* Streaming Type으로 통계 기록 */
} scx_stat_write_type_t;

typedef struct {
	char 	code[8];
	char 	page[512];
	int 	http11_only;
	int		len;
} scx_codepage_t;
typedef struct {
	char 				host[256]; /* serving domain */
	char 				class[64]; /* origin's driver class: HTTP, WebDAV, etc */
	nc_origin_info_t 	origin;   /* 오리진 서버 정보, 여러개 입력 가능 */
	nc_mount_context_t	*volume;
} vhost_t;


struct request_tag;
/* 전역설정(default)파일의 내용을 저장하는 구조체 */
typedef struct scx_config_tag {
	uint32_t	hash; 				/* default.conf 파일의 전체 hash값.  hash는 설정 파일의 변경 여부 확인에 사용된다. */
	int			refcount;	/* 현재 config를 사용중인 유저수 */
	int 		follow_redir;		/* 1로 셋팅되면 양뱡향에서 301을 return할때 netcache core에서 자동으로 redirect된다.*/
	int 		negative_ttl;
	int 		positive_ttl;
	int			fastcrc;		/* 1로 셋팅되면 동작, 0이면 동작안함 */
	int			force_close;		/* 1로 셋팅되면 동작, 0이면 동작안함 */
	int 		ra_mb;
	vstring_t 	*certificate_crt_path;
	vstring_t 	*certificate_key_path;
	uint32_t	hash_crt; 				/* crt 인증서 파일의 hash값. 설정 파일의 변경 여부 확인에 사용된다. */
	uint32_t	hash_key; 				/* key 인증서 파일의 hash값. 설정 파일의 변경 여부 확인에 사용된다. */
	int			enable_sni; 	/* 1인 경우에만 sni 기능이 동작한다, 설정하지 않는 경우는 0임 , 설정이 되었어더라도 시스템에서 지원하지 않으면 동작 안함 */
#if 0
	long 		max_inodes;
#else
	long 		inode_mem;
#endif
	long 		cache_mem;
	int 		block_size;
	int 		writeback_blocks;
	vstring_t 	*cache_path_list;
	int 		workers;
	size_t 		pool_size;
	size_t 		max_virtual_host;
	vstring_t	*cluster_ip;
	int 		cluster_ttl;
	vstring_t 	*accesslog_format;
	vstring_t 	*log_dir;
	int			log_gmt_time;	/* 1인 경우 GMT time, 0인 경우 local time */
	int			log_size;
	int			log_access;
	int			log_error;
	int			log_origin;
	int			logrotate_signal_enable;
	vstring_t 	*host_name;
	int 		https_port;
	int 		http_port;
	int 		tproxy_port;
	vstring_t	*listen_ip;
	int			management_port;
	vstring_t	*management_listen_ip;
	int			management_enable_with_service_port;	/* 서비스 포트로 PURGE, Preload, OpenAPI 요청 허용, 기본값:0(허용안함), 0:허용안함, 1:허용 */
	int			keepalive_timeout;
	vstring_t 	*real_user;
	unsigned int	xfer_size;
	int 		cold_cache_enable;
	int 		cold_cache_ratio;
	int 		disk_high_wm;	/* disk cache 사용량이 이 비율을 넘으면 정리(삭제)를 시작한다.*/
	int 		disk_low_wm; 	/* disk cache 삭제시 이 비율까지만 정리한다. */
	int 		max_path_lock_size; 	/* path lock의 최대 크기를 지정. */
	vstring_t 	*disk_media_cache_dir;	/* metadata disk cache 경로 */
	int 		disk_media_cache_high;	/* metadata disk cache 최대 용량 */
	int 		disk_media_cache_low;	/* metadata disk cache 최소 용량 */
	int 		disk_media_cache_monitor_period;	/* metadata disk cache manager 동작 주기 */
	int			disk_media_cache_enable;/* metadata disk cache 사용 여부 */
	int			polling_policy;	/* 1인 경우만 select 사용, 그외에는 모두 epoll을 사용한다. centos 5에서는 select만 사용가능 하다. */
	vstring_t 	*core_file_path;	/* core dump file이 생성될 위치를 지정한다. 현재 미사용 */
	vstring_t	*nc_trace_mask;
	int			io_buffer_size; 	/* zipper 라이브러리에서 파일을 읽을때 사용되는 버퍼 크기 */
	int			media_cache_size; 	/* media index cache 크기 지정 */
	int			session_timeout;	/* 마지막 요청후 세션 유지 시간 . 단위:초*/
	int			session_enable;		/* streaming시 session 기능을 사용 여부. 1이면 사용, 0이면 사용 안함, default 1 */
	int			builder_cache_size; /* builder index cache 크기 지정 */
	int			allow_dynamic_vhost; /* 동적 볼륨 생성 여부, 1이면 허용, 0이면 차단, default : 1 */
	int			force_sigjmp_restart; /* sigsetjmp()를 사용한 구간에서 SIGSEGV 예외가 발생 했을때 재기동 여부, 1:재기동,0:재기동안함,기본값:0(재기동안함)*/
	int			use_local_file;	/* origin 서버를 사용하지 않고 local file을 사용함. 0:origin사용, 1:local file 사용 */
	int 		ncapi_hang_detect; /* API 행감지 기능 동작 여부, default : 1, 1:동작, 0:동작안함 */
	int 		ncapi_hang_detect_timeout; /*API 행감지 timeout, default : 120, 단위 : 초 */
	int			enable_async_accesshandler_callback;	/* 1이면 AccessHandlerCallback시 비동기(별도의 job thread 사용) 기능을 사용 */
	int			enable_async_reader_callback;			/* 1이면 ContentReaderCallback시 비동기(별도의 job thread 사용) 기능을 사용 */
	int			enable_async_complete_callback;			/* 1이면 RequestCompletedCallback시 비동기(별도의 job thread 사용) 기능을 사용 */
	int			enable_async_ncapi;			/* netcache 비동기 api(open, read) 사용 여부, 1:사용, 0:사용안함 */
	vstring_t 	*ssl_priorities;		/* ssl priorities 설정, default:NORMAL */
	uint32_t	request_timeout;		/* Client가 접속후에 request를 보내기까지의 허용 시간 */
	vstring_t	*cdn_domain_postfix;		/* cdn 별 main 도메인 */
	uint32_t	permit_init_expire_duration;	/* 인증을 사용하는 경우 서비스 데몬 기동후 설정된 시간 내에 들어 오는 요청에 대해서 expire된 요청이라도 일정시간 동안 허용해준다. default:60(초) */
	////////////////////////////////// Content Router 기능 관련 설정 시작 //////////////////////////////////////////////
	int			cr_enable;				/* content router 기능 사용 여부, 기본값:0(사용안함), 1:사용, 0:사용안함, reload 가능 */
	////////////////////////////////// Content Router 기능 관련 설정 끝   //////////////////////////////////////////////
	////////////////////////////////// 통계 관련 설정 시작 /////////////////////////////////////////////////////////////
	int			stat_rc_seq;			/* 통계 기록시 사용 되는 RC_SEQ */
	int			stat_vrc_seq;			/* 통계 기록시 사용 되는 VRC_SEQ */
	int			stat_svr_seq;			/* 통계 기록시 사용 되는 SVR_SEQ */
	vstring_t 	*stat_idc_node_id;		/* 통계 기록시 사용 되는 IDC_NODE_ID */
	vstring_t 	*stat_rc_id;			/* 통계 기록시 사용 되는 RCID */
	vstring_t 	*stat_vrc_id;			/* 통계 기록시 사용 되는 VRCID */
	vstring_t 	*stat_svr_id;			/* 통계 기록시 사용 되는 SVRID */
	int			stat_write_period;		/* 통계 기록 주기 */
	int			stat_rotate_period;		/* 통계 파일 rotate (파일명 변경) 주기 */
	vstring_t 	*stat_origin_path;		/* origin 통계 파일이 기록될 경로 */
	vstring_t 	*stat_origin_prefix;	/* origin 통계 파일명의 prefix */
	vstring_t 	*stat_traffic_path;		/* traffic 통계 파일이 기록될 경로 */
	vstring_t 	*stat_traffic_prefix;	/* traffic 통계 파일명의 prefix */
	vstring_t 	*stat_nos_path;			/* nos 통계 파일이 기록될 경로 */
	vstring_t 	*stat_nos_prefix;		/* nos 통계 파일명의 prefix */
	vstring_t 	*stat_http_path;		/* http 통계 파일이 기록될 경로 */
	vstring_t 	*stat_http_prefix;		/* http 통계 파일명의 prefix */
	vstring_t 	*stat_doing_dir_name;	/* 생성중인 통계파일이 저장될 directory 명 */
	vstring_t 	*stat_done_dir_name;	/* 생성이 완료된 통계파일이 저장될 directory 명 */
	int			stat_write_enable;		/* 서비스 데몬의 통계 기록 여부 설정 */
	scx_stat_write_type_t	stat_write_type;	/* 통계 기록 방식 설정 */
	int			stat_origin_enable;		/* 오리진 통계 기록 여부 설정 */
	int			stat_traffic_enable;	/* 트래픽 통계 기록 여부 설정 */
	int			stat_nos_enable;		/* NOS 통계 기록 여부 설정 */
	int			stat_http_enable;		/* HTTP(Content) 통계 기록 여부 */
	////////////////////////////////// 통계 관련 설정 끝 ///////////////////////////////////////////////////////////////
	int			rtmp_port;	/* rtmp 서비스를 위한 포트, default : 1935 */
	int			rtmp_enable; 		/* rtmp 서비스를 하는 경우 설정, 1:사용, 0:사용안함, default : 0(사용안함) */
	int			rtmp_worker_pool_size;		/* rtmp 처리용 work thread 수 설정, default : 10 */
} scx_config_t;

#ifndef howmany
# define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#define GMT_DATE_FORMAT "%a, %d %b %Y %H:%M:%S GMT"
#define LOCAL_DATE_FORMAT "%a, %d %b %Y %H:%M:%S %z"
#define STAT_DATE_FORMAT "%Y%m%d%H%M%S"	/* 20190410172840의 형태 */

#define MAX_POSITIVE_TTL 		2592000 /* 30*24*3600 한달 */
#define MAX_NEGATIVE_TTL 		86400 /* 24*3600 하루 */
#define MIN_KEEPALIVE_TIMEOUT	3		/* keepalive timeout을 3보다 작게 설정하는 경우 클라이언트의 연결이 바로 끊어지는 문제가 발생한다. 현재 이부분 사용안함 */
#define MAX_XFER_SIZE			256		/* MHD의 response call buffer의 최대크기는 256K를 넘지 않도록 한다. */
/* memory pool의 기본 크기로 사용된다. */
#ifdef ZIPPER
/* streaming의 경우는 url의 길이가 길어서 기본 pool size를 크게 한다. */
#define DEFAULT_POOL_SIZE		16384
#else
#define DEFAULT_POOL_SIZE		8192
#endif

#define DICT_HSIZE 		9973	/* libdict의 hash table 새성 부분에 사용 */

#define 	HEADER_REQ 				0x10000000
#define 	HEADER_RES 				0x20000000

#define 	HEADER_SKIP 			0x00000001
#define 	HEADER_COMMON 	(HEADER_RES|HEADER_REQ)

#define 	HEADER_IS(_h, _f) 	(((_h)->flag & (_f)) == (_f))
typedef int (*header_handler_proc_t)(vstring_t *tag, vstring_t *value, unsigned int flag, struct request_tag *req, vstring_t **replaced, int);
typedef struct {
	char 					name[MAX_HEADER_NAME_LENGTH];
	unsigned int			flag;
	header_handler_proc_t 	proc;
} header_element_t;


#define	SCX_LOGF_FILE		(1 << 0)
#define	SCX_LOGF_PIPE		(1 << 1)
#define	SCX_LOGF_ROTATE		(1 << 2)
#define	SCX_LOGF_SIGNAL		(1 << 3)	/* logrotate와 연동 하는 경우 */
#define	SCX_LOGF_STAT		(1 << 4)	/* 통계 파일 특정 정의 */
typedef struct tag_scx_ringbuffer {
	unsigned short 	magic_b;
	char 			*buffer;
	unsigned short 	magic_a;
	int 			eot;
	int 			size;
	int 			write;
	int 			read;
	pthread_mutex_t lock;
	pthread_cond_t 	cond;
} scx_ringbuffer_t;

typedef struct {
	int 			log_fd;
	uint32_t 		log_flags;
	int 			log_maxkepts;
	char 			log_template[512];	/* %Y, %y, %m, %d allowed */
	uint64_t		log_threshold; 		/* -1 : unlimit, SCX_LOGF_ROTATTE가 있는 경우 값이 valid  */
	uint64_t 		log_written;   		/* total written bytes to log_fd */
	uint64_t 		log_flushsize;   	/* 실제 디스크로 flush가 일어나야하는 크기*/
	int 			log_yday;       		/* day in the year, log를 일별로 rotate할 때 사용 */
	char			log_path[1024];			/* 로그(통계) 파일 경로 */
	char			stat_done_path [1024]; 	/* 기록 완료된 로그(통계) 파일이 이동될 경로 */
	pthread_mutex_t log_writerlock;
	scx_ringbuffer_t	*ring; 			/* optimized for 1:1 reader/writer, no lock needed */
} scx_logger_t;


int nce_handle_error(int code, struct request_tag *req);
int nce_add_basic_header(struct request_tag *req, struct MHD_Response *res);
int scx_add_cors_header(struct request_tag *req);
int nc_build_request(struct request_tag *req);
void nce_create_response_from_buffer(struct request_tag *req, size_t size,  void *buffer);
int scx_finish_response(struct request_tag *req);
int hdr_req_range(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
nc_request_t * nc_create_request(void *cls, char *uri);
void nc_request_completed(void *cls, struct MHD_Connection *connection, void **ctx,	enum MHD_RequestTerminationCode toe);
int scx_make_response(nc_request_t *req);
int scx_update_resp_header(const char *key, const char *value, void *cb);
int scx_update_ignore_query_url(nc_request_t *req);
int scx_make_path_to_url(nc_request_t *req);

int vm_init();
int vm_deinit();
struct service_info_tag * vm_lookup(const char *host);
struct service_info_tag * vm_add(const char *host, int only_static);
int vm_update_versioned_origin(struct request_tag *req, uint64_t version, vstring_t *origins);
int vm_create_config(int is_reload);
int vm_all_volume_purge();
char *vm_find_voldomain_from_sub_domain_list(struct request_tag *req, const char *volname);
uint32_t vm_make_config_hash(const char *path, off_t *st_size);


int hlf_log(char *buf, int buflen, const char *fmt, struct request_tag *req);
void scx_origin_monitor(void *cb, const char *method, const char *origin, const char *request, const char *reply, double elapsed, double sentb, double receivedb);
void scx_origin_monitor2(nc_uint32_t txid, nc_xtra_options_t *req_headers, void *cb, const char *method, const char *origin, const char *request, const char *reply, double elapsed, double sentb, double receivedb, const char * infostr);
void scx_origin_request_handler(int phase_id, nc_origin_io_command_t *command);
void scx_origin_response_handler(int phase_id, nc_origin_io_command_t *command);
int scx_origin_online_handler(char *url, int httpcode, void *usercontext);
void scx_run_async_job(void * pctx);

scx_config_t	*scx_get_config();
int	scx_release_config(scx_config_t	* config);
void *scx_sni_load_keys(struct site_tag * site, const char *CERT_FILE, const char *KEY_FILE);
int scx_sni_unload_keys(void * pcert);
void expire_timer_callback(void *cr);	//CHECK_TIMER에서 설정한 시간이 지났을때 callback됨
void nce_object_monitor(void *, nc_hit_status_t);
scx_logger_t * logger_open(char *, uint64_t, uint32_t , int );
void logger_close(scx_logger_t *logger);
void logger_flush(scx_logger_t *logger);
void logger_set_done_path(scx_logger_t *logger, char *path);
int logger_reopen(scx_logger_t *logger);
void scx_error_log(struct request_tag *req, const char *format, ...);
void init_log_format();
void scx_setup_codepage();
scx_codepage_t * scx_codepage(int code);
int logger_put(scx_logger_t *logger, const char *log, int len);
int gip_load();
char * gip_lookup_country2(const char *ip, char *buf);
char * gip_lookup_country3(const char *ip, char *buf);
char * gip_lookup_isp(const char *ip, char *);
char * gip_lookup_city(const char *ip, char *city);
char * gip_lookup_country_name(const char *ip, char *buf);
char * gip_lookup_region(const char *ip, char *buf);
void gip_close();

void scx_init_rsapi();
void scx_deinit_rsapi();
int rsapi_is_local_object(struct request_tag *req);
int rsapi_do_handler(struct request_tag *req);

extern scx_config_t	*gscx__config;
extern pthread_mutex_t 	gscx__config_lock;
extern int		gscx__signal_shutdown;
extern struct site_tag 	*gscx__default_site;
extern int		gscx__permit_expire;


#endif /* __HTTPD_H__ */
