#ifndef __httpn_DRIVER_H__
#define __httpn_DRIVER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#include <pthread.h>
#include <sys/stat.h>
#include "hash.h"
#include "bt_timer.h"
#include "trace.h"
#include "lb.h"
#include "cfs_driver.h"

#ifndef NC_RELEASE_BUILD
#define		DEBUG_IOCB
#define		PROFILE_PERFORM_TIME
#endif


//#define	HTTPN_MEASURE_IOCB

/*
 * http driver error class
 */

#define	HTTPDE_OK				0		/* 정상, 더이상 재시도 필요없음*/
#define	HTTPDE_RETRY			(-1) 	/* 비정상, retry 할 필요 있음 */
#define	HTTPDE_CRITICAL			(-2)	/* 비정상, retry 할 필요 없음 */

// CHG 2019-03-25 huibong oringin 응답코드에 대해 운영자 설정에 따른 OFFLINE 처리 기능 개선 (#32565)
//                 - Origin 응답은 정상이지만.. 응답코드에 따른 OFFLINE 처리 기능 지원을 위해 추가
#define HTTPDE_USER				(-3)	/* 정상, retry 필요 없음, OFFLINE 처리만 추가로 check. */



#ifndef NC_RELEASE_BUILD
#define DEBUG_ASSERT_IOCB(_iocb, _e)		if (!(_e)) { \
												char 	_bufz[1024];\
												TRACE((g__trace_error, (char *)"IOCB[%d] Assertion '%s' ERROR at %d@%s\n" \
																			   "\t\t\t\tIOCB DUMP: %s\n", \
													(_iocb)->id, \
													#_e,  __LINE__, __FILE__,\
													httpn_dump_iocb(_bufz, sizeof(_bufz), _iocb))); \
												raise(SIGSEGV); \
										    }
#else
#define DEBUG_ASSERT_IOCB(_iocb, _e)		
#endif

#define	SET_OPT(_s, _o, _v) 		if (curl_easy_setopt(_s, _o, _v) != CURLE_OK) { \
										TRACE((T_ERROR, "operation '%s' failed at %d@%s\n", #_o, __LINE__, __FILE__)); \
									}
#define	CHK_OPT(_r, _o) 			{ \
										_r = (_o); \
										if (_r != CURLE_OK) { \
											TRACE((T_ERROR, "operation '%s' failed(r=%d:%s) at %d@%s\n", #_o, _r, curl_easy_strerror(_r), __LINE__, __FILE__)); \
										} \
									}
#define	CHK_IOCB_OPT(_i, _o) 		{ \
										int		_r; \
										char	_buf[2048]; \
										_r = (_o); \
										if (_r != CURLE_OK) { \
											TRACE((T_ERROR, "IOCB[%s].operation '%s' failed(r=%d:%s) at %d@%s\n", #_o, httpn_dump_iocb(_buf, sizeof(_buf), _i), _r, curl_easy_strerror(_r), __LINE__, __FILE__)); \
										} \
									}


/* 
 * 	IO context
 */
#define 	HTTP_IO_READ 	(1 << 0)
#define 	HTTP_IO_WRITE	(1 << 1)

#define		MAX_HEADER_SIZE		4096
#define		MAX_PATH_SIZE		1024
#define		MAX_URL_SIZE		(MAX_PATH_SIZE*3)

struct httpn_session_pool;
struct httpn_session;
struct tag_httpn_mux_info;


struct tag_httpn_io_context; 
typedef enum {
	MPXOP_PROLOG=0,
	MPXOP_EPILOG=1
} httpn_mpxop_t;

#define		MPX_TASK_RESULT_COMPLETE	0
#define		MPX_TASK_RESULT_PAUSED	1

#define 	MPX_TASK_CONTINUE	0
#define 	MPX_TASK_STOP		1
typedef int (*httpn_task_proc_t)(struct tag_httpn_io_context *, httpn_mpxop_t, int);

struct tag_getattr_param ;
typedef enum {HTTP_API_NULL = 0, HTTP_API_GLOBAL=1, HTTP_API_GETATTR=2, HTTP_API_READ=3, HTTP_API_WRITE=4, HTTP_API_PROBE = 5} httpn_api_t;

typedef enum {HTTP_NULL=0, HTTP_HEAD=1, HTTP_PUT=2, HTTP_GET=3, HTTP_MKDIR=4, HTTP_DELETE=5, HTTP_POST=6, HTTP_CUSTOM=7} httpn_method_t;
typedef enum {
	HS_INIT			= 0,
	HS_HEADER_OUT 	= 1,
	HS_POSTING		= 2,
	HS_HEADER_IN 	= 3,
	HS_EOH			= 4, /* end of header */
	HS_BODY			= 5,
	HS_DONE			= 6,
	HS_MAX			= 7
} httpn_pin_state_t;
typedef enum {
	IOCB_MPX_INIT		= 0,
	IOCB_MPX_READY		= 1,
	IOCB_MPX_TRY_RUN		= 2,
	IOCB_MPX_PAUSED		= 3,
	IOCB_MPX_TRY_DONE	= 4,
	IOCB_MPX_FINISHED	= 5
} httpn_mpx_state_t;
typedef enum {
	IOCB_EVENT_EXECUTE		= 0,
	IOCB_EVENT_BEGIN_TRY	= 1,
	IOCB_EVENT_PROPERTY_DONE	= 2,
	IOCB_EVENT_PAUSE_TRY	= 3,
	IOCB_EVENT_RESUME_TRY	= 4,
	IOCB_EVENT_TIMEOUT_TRY	= 5,
	IOCB_EVENT_END_TRY		= 6,
	IOCB_EVENT_FINISH		= 7
} httpn_iocb_event_t;


#define 	HTTP_MPX_COUNT	(HTTP_MPX_CLOSED+1)


typedef enum {
	HMC_NONE 		= 0,
	HMC_SCHEDULE 	= 1,
	HMC_RESUME 		= 2,
	HMC_REMOVE 		= 3,
	HMC_DESTROY 	= 4,
	HMC_FREED 		= 5
} httpn_mpx_cmd_t;

#define 	HS_VALID(_s)	(_s >= 0 && _s < HS_MAX)
struct httpn_driver_info; 
struct httpn_request;
#define		IOCB_VALID_MAGIC 	0x5AA5A55A

struct tag_httpn_io_context_exc {
	nc_uint32_t					id;
	httpn_mpx_cmd_t				cmd_id;	/* valid only when dequeued */
	httpn_pin_state_t			cmd_target;	/* valid only when dequeued */
	nc_cond_t					cmd_signal; /* 필요할때만 할당 */
};
struct tag_httpn_io_context {

	/*
	 * client input
	 */
	nc_uint32_t					id;


	httpn_mpx_cmd_t				cmd_id;	/* valid only when dequeued */
	httpn_pin_state_t			cmd_target;	/* valid only when dequeued */
	nc_cond_t					cmd_signal; /* 필요할때만 할당 */

	//
	// 2020-01-27 by weon
	// 
	// driver의 현재 버전 번호를 keep해두고
	// 이후 LB등 drvctx에 속한 메모리 접근 시
	// 접근안하고 회피할 수 있도록 함
	nc_uint32_t					confver;

	nc_uint32_t					magic;
	struct tag_httpn_mux_info	*mpx;


	// CHG 2019-04-24 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용

	nc_uint8_t					mpxassoc;	
	nc_uint8_t					canceled;
	void 						*private;
	nc_asio_vector_t 			*vector;
	fc_inode_t 					*inode;
	nc_mount_context_t			*volume;
	cfs_origin_driver_t 		*driver;
	struct httpn_driver_info	*driverX;
	nc_xtra_options_t 			*in_property; /* input property provided by caller */
	httpn_method_t				method;
	char						zmethod[32];
	nc_mode_t					mode;
	void *						udata;
	char						*wpath; /* working path */
	int							postlen; /* post data len so far */

	nc_size_t					knownsize;  	/* size가 알려진 경우 */
	nc_uint32_t					autocreat:1;
	nc_uint32_t					timedout:1;
	nc_uint32_t					rangeop:1;	/* sub-range operation sent */
	nc_uint32_t					rangeable:1; /* caching된 객체 속석이 rangeable */
	nc_uint32_t					sizeknown:1; /* caching된 객체 속성이 sizeknown 객체 */
	nc_uint32_t					imson:1;
	nc_uint32_t					entire:1;	/* 1 if FULL-GET */
	nc_uint32_t					verbose:1;
	int							cachedhttpcode; /* getattr()시 캐싱되어있던 오리진 응답코드 */
	/*
	 * valid IO range 설정 (이 설정된 범위까지만 읽음)
	 */
	nc_off_t					reqoffset;
	nc_size_t					reqsize;

	httpn_api_t					owner;

	/*
	 * origin's response
	 */
	long long 			received; 	/* curl로부터 받은 데이타 */
	long long 			accepted;	/* 받은 데이타 중 실제로 파싱된 데이타 */

	/*
	 * internal operation
	 */
	httpn_mpx_state_t			state;
	lb_t						*assoc_lb;		/* iocb가 할당받은 세션이 속하는 LB */
	struct httpn_session_pool 	*pool;			/* if any */
	struct httpn_session 		*session;
	struct httpn_request 		*request;
	CURLcode					last_curle; 	/* last curl error code */
	char						last_curle_str[CURL_ERROR_SIZE+1];						
	int							last_httpcode;  /* last httpcode code */
	int							last_errno;  	/* last errno code */

	/*
	 * retry information
	 */
	nc_int8_t 		allow_retry;
	nc_int8_t 		tries;
	nc_int8_t		reqsent;
	nc_time_t		ctime;	/* creation time in sec*/
	/*
	 * performance-related
	 */
#ifdef	HTTPN_MEASURE_IOCB
	perf_val_t 		t_create; /* creation time */
	perf_val_t 		t_send;
	perf_val_t 		t_fba;
	perf_val_t 		t_hdrend;
	perf_val_t 		t_pause;
	perf_val_t 		t_resume;
	perf_val_t 		t_done;
#endif
	

	/* 
	 * check why this fields needed
	 */
	union {
		struct {
			int					blk_cursor;
			int					blk_off; /* written bytes in current block */
			cfs_off_t 			file_off;
			cfs_size_t			flushed; /* written bytes so far */
			cfs_size_t			total; /* total bytes to send */
			int					blk_flushed; /* # of blocks sent */
		} w;
		struct {

			int					blk_cursor;	/* 현재 write중인 blk index */
			int					blk_fini; 	/* fill 후 callback호출 완료된 idx */
			int 				blk_off;	/* 현재 write중인 block내의 데이타 옵셋*/
			cfs_off_t 			file_off;	/* 현재 저장중인 데이타 옵셋*/
			cfs_off_t 			file_off_prev;	/* 이전 시도에서 정상적으로 수신한 데이타의 옵셋*/
			cfs_size_t 			filled;		/* IOCB 생성 이후 현재까지 받은 데이타 */
			cfs_size_t 			total;
			int 				blk_filled;
		} r;
	} u;

	nc_stat_t					stat;
	httpn_task_proc_t			task;
	apc_overlapped_t 			event;
	httpn_pin_state_t 			target_action;;
	int 						refs;
	nc_uint32_t					keptid;
	nc_uint32_t					tflg; /* default 0, dynamically set to T_WARN */

	nc_uint64_t					iotimeout;	/* NOW + timeout 값 */
	nc_uint32_t					post_avail:1;
	nc_uint32_t					needpause:1;
	nc_uint32_t					inredirection:1;
	nc_uint32_t					assume_rangeable:1;
	nc_uint32_t					premature:1;
	nc_uint8_t					reported;

#ifdef DEBUG_IOCB
	char						*csource;
	int							cline;
	char						*dsource;
	int							dline;
	link_node_t 				dbg_node;
#endif
#ifndef NC_RELEASE_BUILD
	link_node_t					mpx_node;
#endif
	link_node_t 				node;
};
typedef struct tag_httpn_io_context httpn_io_context_t;



typedef struct {
	CURL				*curl; 		/* non-rangeable객체에만 valid, != NULL */
	httpn_pin_state_t	state;	
	int 				usagelimit;	/* 위의 경우 1로 설정, 사용 후 0이 되면 curl 핸들 release */
} httpn_pin_t;


typedef enum {
	DPS_DOWN = 0,
	DPS_ONLINE = 1,
	DPS_PROBING = 2,
} httpn_pool_status_t;



/*
 * 	 session pool structures
 */
typedef struct httpn_session {
	char						key[64];
	cfs_origin_driver_t*		driver;

	httpn_pin_state_t			state;
	struct httpn_session_pool*	pool;
	/*
	 * poo_handle은 반드시 필요. 
	 * LB내의 pool 형상이 바뀌었을 때
	 * 유효성 검사에 필요
	 */
	nc_pool_handle_t 			pool_handle;
	CURL						*eif; /* easy interface */
	nc_uint32_t					paused:1; /* 1 if session is in paused state */
	int 						stderrno;
	link_node_t					node;
} httpn_session_t;
typedef struct {
	char			host[128];
	int				probed;
	link_node_t		node;
} httpn_host_t;
typedef struct tag_httpn_url {
	link_list_t		host_list;
	char 			path[1024];
	int				count; /* host count */
	int				probe_count;
	link_node_t 	node;
} httpn_url_t;
#define 	POOL_READABLE 	NCOF_READABLE
#define 	POOL_WRITABLE 	NCOF_WRITABLE
typedef struct httpn_session_pool {
	char 					key[256];
	char					probe_url[256];
	nc_time_t				probe_url_utime;
	nc_int8_t 				pooltype; 	/* NC_CT_XXXX */
	nc_int8_t				https:1; 	/* 1 if https required */
	nc_int8_t				probe_url_frozen:1; 	/* 1 if https required */
	nc_int32_t				cfc;		/* consecutive failure count:연속 원본 오류 횟수 */
	char					*uri;
	char 					*user;
	char 					*pass;
	char					*storage_url;
	lb_pool_t				*lb_pool;
	nc_pool_handle_t 		pool_handle;
	httpn_pool_status_t 	status;
	/* maintained in LIFO way */
	cfs_origin_driver_t		*driver;
	link_list_t 	probing_url; /* failed URL */
} httpn_session_pool_t;

typedef struct tag_httpn_mux_info {
	CURL					*mif;
	int						shutdown;
	pthread_t				thr_mpx;
	pid_t					tid;

	int						run; 		/* 현재 mux에 add 된 iocb 갯수 */
	nc_time_t				idle; 		/* idle 시작 시간 */
	nc_lock_t				mpxlock;

#ifndef NC_RELEASE_BUILD
	link_list_t				q_run;			/* pending iocb queue */
#endif

	link_list_t					q_direct;	/* mux에 할당된 queue */

	link_node_t					node;
} httpn_mux_info_t;

typedef struct httpn_driver_info {
	httpn_session_pool_t    **pool[2];
	int    					pool_cnt[2];
	nc_lock_t				lock;
	char 					*filter;
	char 					*proxy; 		/*향후 복수개의 proxy 잡을 수 있도록 */
	long 					proxy_port; 	/*향후 복수개의 proxy 잡을 수 있도록 */
	nc_uint32_t 			pool_size; 		// # of pool
	int 					probe_interval;
	int 					probe_count;
	int						max_tries;
	//
	// 2020-01-27 by weon
	// 
	// driver의 현재 버전 번호
	// 0부터 시작해서 LB등 정보가 변경될 때마다 1씩 증가함
	nc_uint32_t				confver;

	/*
	 *
	 */
	nc_int32_t				max_read_size; /* origin_request_type=2 일때 사용됨*/

	nc_int16_t 				always_lower:1;
	nc_int16_t 				shutdown:1;
	nc_int16_t 				useheadforattr:1; /* 1: getattr에서 HEAD 사용*/
	nc_int16_t 				follow_redirection:1;
	nc_int16_t 				timeout;
	nc_int16_t 				xfer_timeout;
	bt_timer_t				t_healthcheck;	/* health-check timer */

	// NEW 2020-01-22 huibong DNS cache timeout 설정 기능 추가 (#32867)
	//                - curl 상의 dns cache timeout 관련 설정값
	//                - 단위: sec
	int						dns_cache_timeout;

	nc_origin_monitor_t 	origin_monitor_proc;
	nc_origin_monitor2_t 	origin_monitor2_proc;
	cfs_io_property_updator_t 	io_update_property_proc;
	cfs_io_validator_callback_t  io_validator_proc;

	void 				*origin_monitor_cbd;
#ifdef PHASE_HANDLER_DEFINED
	nc_origin_phase_handler_t on_send_request_proc;
	nc_origin_phase_handler_t on_receive_response_proc;
	nc_origin_phase_handler_t on_send_request_cbdata;
	nc_origin_phase_handler_t on_receive_response_cbdata;
#endif
	/*
	 * converation related stuff
	 */
	iconv_t 			from_utf_8;
	iconv_t				to_utf_8;
	iconv_t				from_server_enc;
	iconv_t				to_server_enc;
	/*
 	 * ir # 26831 관련 추가
	 */
	lb_t 				 *LB[2]; 		/* 0:NC_CT_ORIGIN, 1:NC_CT_PARENT */
	int 				active_lb; 		/* [0,1]: 현재 사용 중인 LB index */
	/* 
	 * ir # 26831 
	 */

	int 		opt_use_https;				/* default 0 */
	int			opt_https_secure;  			/* default 0 */
	char		*opt_https_cert;			/* default NULL */
	char		*opt_https_cert_type;		/* default NULL */
	char		*opt_https_sslkey;			/* default NULL */
	char		*opt_https_sslkey_type;		/* default NULL */
	char		*opt_https_crlfile;			/* default NULL */
	char		*opt_https_cainfo;			/* default NULL */
	char		*opt_https_capath;			/* default NULL */
	int			opt_https_falsestart; 		/* default : FALSE */
	int			opt_https_tlsversion; 	/* example : deafult "1.1" */
		
	nc_offline_policy_proc_t		origin_is_down;
	void 							*usercontext;
 
} httpn_driver_info_t;



typedef enum {
	HTTP_IO_WAIT_INPUT = 0,
	HTTP_IO_DATA_AVAILABLE = 1,
	HTTP_IO_INPROGRESS = 2,
	HTTP_IO_EOT = 3
} httpn_io_state_t;

struct tag_httpn_request;
typedef struct httpn_request httpn_request_t;
typedef struct httpn_file_info {
	pthread_t 			tid;
	int					tid_valid;
	pthread_mutex_t 	lock;
	pthread_cond_t 		cond_data_avail;
	pthread_cond_t 		cond_data_done;
	httpn_io_state_t 	state;
	char 				*path;
	nc_xtra_options_t 	*req_options;
	nc_xtra_options_t 	*res_options;
	httpn_io_context_t 	*iocb;
	int 				eot;
	httpn_session_t 	*session;
	httpn_request_t		*req;
	cfs_size_t 			xfered;
	cfs_size_t 			expected;
	int 				write_throttle; /* reader/writer에 따라 증감 */
	link_list_t 		write_queue;
	int 				httpcode; /* worker threadrk 설정 */
	int 				stderrno; /* worker threadrk 설정 */
} httpn_file_handle_t;
typedef struct tag_node {
		char			*tag;
		char			*value;
		link_node_t		node;
} httpn_tag_t;

typedef enum {
	DP_CURRENT 	= 0,
	DP_NEXT 	= 0,
} httpn_pool_cmd_t;

#define 	HTTP_TRY_STOP 		0 	/* break loop */
#define 	HTTP_TRY_CONTINUE 	1 	/* break loop */
#define 	HTTP_TRY_FAILED 		2 	/* remote pool down */

#define	DRIVER_NAME	"http native driver v2.5 (" __DATE__ " " __TIME__ " )"

#define	HTTPN_NEED_RESUME(i_) (httpn_is_state((i_)->session, HS_EOH) && httpn_mpx_state_is(i_, IOCB_MPX_PAUSED))

httpn_session_pool_t * httpn_find_active_pool(cfs_origin_driver_t *drv);
int httpn_online(cfs_origin_driver_t *drv, int ioflag);
void httpn_clear_session_table(void *v);
int httpn_mark_down_pool_x(cfs_origin_driver_t *drv, httpn_session_pool_t * sp, char *path, int force);

// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
// - origin 이 정상 상태임을 설정하기 위한 함수.
void httpn_mark_ok_pool_x( cfs_origin_driver_t * drv, httpn_session_pool_t * sp );

int httpn_setup_pool(httpn_session_pool_t *pool, const char *docroot, const char *id, const char *pass);
httpn_session_pool_t * httpn_next_pool(cfs_origin_driver_t *driver, const char *path, int makeref);
void httpn_init_locks();
//void httpn_lock_driver(httpn_driver_info_t *drv_x);
//void httpn_unlock_driver(httpn_driver_info_t *drv_x);
int httpn_map_2_http(httpn_io_context_t *iocb, CURLcode response);
void httpn_migrate_state(httpn_session_t *s, httpn_pin_state_t ps, const char *msg);
int httpn_is_state(httpn_session_t *s, httpn_pin_state_t state);
int httpn_state(httpn_session_t *s);
int hcd_init();
int mpx_STM(httpn_io_context_t *iocb, httpn_iocb_event_t event, int finish, char *f, int l);
const char * hcd_errstring(int code);
void httpn_set_owner(httpn_io_context_t *iocb, httpn_api_t api);
lb_t *httpn_get_LB(httpn_io_context_t *iocb);
httpn_io_context_t *httpn_create_iocb(	cfs_origin_driver_t *drv, 
										const char *path, 
										nc_kv_list_t *in_prop, 
										nc_mode_t mode, 
										nc_asio_vector_t *v, 
										int aut, 
										nc_open_cc_t *aoc,
										nc_uint32_t	*kept,
										char *f, 
										int l);
int http_retry_allowed(httpn_io_context_t *iocb);
void httpn_bind_request(httpn_io_context_t *iocb, httpn_request_t *req);
int httpn_update_operation(httpn_io_context_t *iocb, char *method);
int httpn_bind_vector(httpn_io_context_t *iocb, nc_asio_vector_t *v);
void httpn_destroy_iocb(httpn_io_context_t *iocb, int force, char *f, int l);
void httpn_free_iocb(httpn_io_context_t *iocb);
int httpn_set_result_httpcode(httpn_io_context_t *iocb, int httpcode);
void httpn_reset_response(httpn_io_context_t *iocb);
int httpn_is_secure(const char *path);
int httpn_make_fullpath(httpn_driver_info_t *drvctx, char **urlpath, const char *path, int need_esc);
void httpn_status_code_callback(int httpcode, void *cb);
int httpn_is_redirection(int c);
httpn_session_t * httpn_pop_session(cfs_origin_driver_t *drv, httpn_session_pool_t *pool, httpn_session_t *, char *f, int l);
httpn_method_t httpn_map_method(char *method_string);
void httpn_set_status(httpn_io_context_t *);
int httpn_set_errno(int err);
int httpn_map_httpcode(httpn_io_context_t *, int httpcode); 
void httpn_report_origin_log(httpn_io_context_t *iocb);
size_t httpn_block_null_reader(void *ptr, size_t size, size_t nmemb, void *userdata);
size_t httpn_block_reader(void *ptr, size_t size, size_t nmemb, void *userdata);
size_t httpn_post_data_proc(char *ptr, size_t size, size_t nmemb, void *cb);
int httpn_request_add_ims_header(httpn_io_context_t *iocb, nc_stat_t *old_s, apc_open_context_t *);
void httpn_set_raw_code_ifpossible(httpn_io_context_t *iocb, int code);
void httpn_set_task(httpn_io_context_t *iocb, httpn_task_proc_t proc);
void httpn_setup_session(httpn_io_context_t *iocb, httpn_session_t *session, httpn_session_pool_t *pool);
int httpn_refresh_session(httpn_io_context_t *iocb);
int httpn_bind_RS(httpn_io_context_t *iocb);
int httpn_mpx_task_unregister(httpn_io_context_t *iocb);
int httpn_mpx_task_remove_from_paused(httpn_io_context_t *iocb);
void httpn_mpx_state_set(httpn_io_context_t *iocb, httpn_mpx_state_t mpxs, char *,int);
httpn_mpx_state_t httpn_mpx_state(httpn_io_context_t *iocb);
int httpn_mpx_state_is(httpn_io_context_t *iocb, httpn_mpx_state_t mpxs);
void httpn_set_private(httpn_io_context_t *iocb, void *u);
int httpn_handle_response_headers(httpn_io_context_t *iocb) ;
int httpn_forcely_down(httpn_driver_info_t *drvctx, char *url, int curle, int httpcode);
char * httpn_dump_iocb(char *buf, int len, httpn_io_context_t *iocb);
char * httpn_dump_iocb_s(char *buf, int len, httpn_io_context_t *iocb);
void httpn_get_stat(int 	stat[]);
int httpn_getattr_completion(void *u);
int httpn_PH_on_receive_response(httpn_io_context_t *iocb);
void httpn_migrate_http_state(httpn_io_context_t *iocb, httpn_pin_state_t ps, char *f, int l);
char * httpn_dump_info(char *buf, int len, CURL *handle);
int httpn_post_notification_callback(nc_xtra_options_t *opt, void *ud);
int http_iocb_valid(httpn_io_context_t *iocb);
int httpn_valid_config(cfs_origin_driver_t *drv, nc_uint32_t confver);
int httpn_prepare_try(httpn_io_context_t *iocb);
void httpn_cleanup_try(httpn_io_context_t *iocb);
int httpn_handle_try_result(httpn_io_context_t *iocb);
int httpn_bind_request_to_session(cfs_origin_driver_t *drv, httpn_session_t *session, httpn_request_t *request);
httpn_session_t * httpn_prepare_session(cfs_origin_driver_t *drv, httpn_io_context_t *iocb, httpn_session_pool_t *pool);
void httpn_release_session(httpn_io_context_t *iocb, httpn_session_t *session);
void httpn_init_session(httpn_session_t *session, httpn_io_context_t *iocb);
int httpn_handle_error(httpn_io_context_t *iocb, CURLcode *curle, int *httpcode, int *dtc);
void * httpn_iocb_cleaner(void *notused);
int httpn_iocb_prepare_pool(int n);
int httpn_handle_defered_queue(cfs_origin_driver_t *drv, int);
#endif /* __httpn_DRIVER_H__*/
