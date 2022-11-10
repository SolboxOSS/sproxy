/*
 * solbox could storage accelerator
 *
 * KEEP IN MIND!
 * 	(1) 성능을 생각할 것!
 * 			- switch 문은 되도록 사용말 것
 * 			- memcpy, strcpy등은 되도록 사용하지 말것
 * 	(2) byte 단위 operation을 쓸 때, 4byte, 8byte operation으로 
 *      대치할 수 있는지 생각할것
 *  (3) lock은 되도록, 최선을 다해서 피할 것
 *  (4) strcmp, strcasestr, strcpy대신 strNxxx함수를 사용할 것
 * 	-- written by weon@solbox.com
 */


//#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <microhttpd.h>
#include <unistd.h>
#include <search.h>
#include <errno.h>
//#include <ncapi.h>
//#include <trace.h>
#include <getopt.h>
#include <gcrypt.h>
#include <curl/curl.h>
#include <setjmp.h>
#include <execinfo.h>

#include <dict.h>	/* libdict */

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "luaembed.h"
#include "status.h"
#include "httpd.h"
#include "luapool.h"
#include "scx_util.h"
#include "voxio_def.h"
#include "sessionmgr.h"
#include "scx_timer.h"
#include "standalonestat.h"
#include "limitrate.h"
#include "module.h"
#include "thpool.h"
#include "scx_rtmp.h"
#include "scx_list.h"
#include "streaming.h"
#include "meta_disk_cache.h"
#include "preload.h"
#include "shm_log.h"

#if GNUTLS_VERSION_MAJOR >= 3
#include <gnutls/abstract.h>
#pragma message("ENABLE SNI")
/**
 * A hostname, server key and certificate.
 */
/*
 * SNI사용 가능 최소 조건은 컴파일 옵션에 ENABLE_SNI가 있어야 하고 GNUTLS의 버전의 3.0이상이어야 함
 * SNI가 사용 가능할때 default 설정에만 인증서가 있으면 기존방식과 동일하게 동작함
 * */
#define SNI_PCRT_MAX	10
typedef struct st_snicert
{
  gnutls_pcert_st pcrt[SNI_PCRT_MAX];
  int	pcrt_cnt;	/* pcrt의 개수 */
  gnutls_privkey_t key;
} st_snicert_t;
static int scx_sni_handler (gnutls_session_t session, const gnutls_datum_t* req_ca_dn, int nreqs, const gnutls_pk_algorithm_t* pk_algos,
						int pk_algos_length, gnutls_pcert_st** pcert, unsigned int *pcert_length, gnutls_privkey_t * pkey);
#else
#pragma message("DISABLE SNI")
#endif
typedef struct st_cert
{
	char		*crt;
	char		*key;
} st_cert_t;


static char 	_default_mime_type[] = "text/plain";

static int hdr_comm_content_length(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_host(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_if_match(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_ims(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_ir(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_comm_cookie(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_ius(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_comm_pragma(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
//static int hdr_req_range(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_if_not_match(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_comm_connection(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_comm_accept_encoding(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_authorization(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_comm_cache_control(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_xff(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **, int ignore);
static int hdr_req_cache_id(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_pttl(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_nttl(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_origins(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_follow_redir(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_purge_key(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_purge_hard(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_purge_volume(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_preload(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);
static int hdr_req_pretty(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore);

static const vstring_t  * rr_lookup(vstring_t *field, vstring_t *condition);
static void scx_lookup_my_address();
static void scx_check_aesni();
static int scx_check_memory();

static int scx_make_tproxy_socket (int);
static int main_service(int load_count);
static int start_http_service(int load_count);
static int start_check_listening_service();
static void nce_ignore_signal();
static vstring_t * scx_update_via(nc_request_t *req, vstring_t *buf, char *v);
static int scx_check_stat_write_type();
static int nce_load_global_params(site_t * site, scx_config_t * config, int reload);
static int scx_is_secured(const char *uri);
static int scx_is_valid_host(nc_request_t *req);
static int scx_valid_uri(const char *uri);
static vstring_t * scx_pickup_url(mem_pool_t *pool, const char *uri);
static int scx_verify_and_adjust_headers(nc_request_t *req);
static vstring_t *nce_get_ip_str(const struct sockaddr *sa, vstring_t *s, size_t maxlen);
static int scx_check_method_authority(nc_request_t *req);

static vstring_t * scx_get_myaddr(nc_request_t *req, int cfd);
static int scx_log_rotate();
static int scx_skip_interpret(nc_request_t *req, vstring_t *vkey)  ;
static int scx_change_uid();
static int scx_check_cache_dir();
static void scx_get_session_info(nc_request_t *req);
static int scx_parent_kill(int clear);
static void scx_write_pidfile();
static void scx_remove_pidfile();
static int scx_create_child_msg_file();
static void scx_write_child_msg_file(pid_t ppid);
static int scx_check_child_msg_file();
static void scx_remove_child_msg_file();
static int scx_operate_range(nc_request_t *req);
static int scx_operate_condition(nc_request_t *req);
static void * scx_check_certitificate(site_t * site, scx_config_t * config, int mode);
static void scx_report_environment();
static void scx_desable_method(const char *method);
static void scx_wait_job_completed(scx_async_ctx_t * async_ctx);
static void scx_nc_open_completed(nc_file_handle_t *file, nc_stat_t *stat, int error, void *cr);
static int scx_handle_delete(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_get	(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_post	(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_put	(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_head	(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_purge	(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_stat	(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_preload(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_purgeall	(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_unknown(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_handle_options(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int handle_method_streaming_phase(nc_request_t *req);
static int handle_method_open_phase(nc_request_t *req);
static int scx_handle_method(nc_request_t *req);
static int scx_handle_async_method(const char *upload_data, size_t * upload_data_size, nc_request_t *req);
static int scx_sync_reader(void *cr, uint64_t pos, char *buf, size_t max, int reader_type);
static ssize_t scx_async_streaming_reader(void *cr, uint64_t pos, char *buf, size_t max);
static ssize_t scx_async_object_reader(void *cr, uint64_t pos, char *buf, size_t max);
static ssize_t scx_async_reader(void *cr, uint64_t pos, char *buf, size_t max, int reader_type);
static int scx_process_request(nc_request_t *req);
static int scx_create_thread_pool();
static int scx_destroy_thread_pool();
static int scx_sum_response_header_size(void *cr, enum MHD_ValueKind kind, const char *key, const char *value);
static char * nc_find_mime(const char *path);
static int nce_add_file_info(nc_request_t * response, nc_stat_t *buf);
int mm_load(const char *path);
char * mm_lookup(const char *key);
static int nce_inject_property(const char *key, const char *value, void *cb);
static int nce_build_header_dict();
static scx_config_t * scx_init_config(site_t * site);
static int scx_deinit_config();
static int nce_reload_default_cert(int reload);

static int scx_map_nc_error(int r);
static size_t scx_calc_xfer_buffer_size(nc_request_t *req, nc_ssize_t remained);
static int scx_permit_expired_request();
void scx_forbidden_expired_request(void *cr);

static void *signal_handler_thread(void *d);
void scx_dump_stack();
void scx_marking_tid_to_shm();
/*
 * 광역 변수들
 */

//#define	DEFAULT_ACCESSLOG_FORMAT "%h %{Host}i %c %x %T %E \"%t\" \"%r\" %b %s \"%{Referer}i\" \"%!200,206,302{User-agent}i\"" /* 이전 포맷 */
//#define	DEFAULT_ACCESSLOG_FORMAT "\"%t\" %h \"%{Host}i\" \"%{X-Forwarded-For}i\" %P %x %T %F %E %z %b %s \"%r\" \"%c\" \"%{Range}i\" \"%{Referer}i\" \"%{User-agent}i\""
//#define	DEFAULT_ACCESSLOG_FORMAT "\"%t\" %h \"%{Host}i\" \"%{X-Forwarded-For}i\" %P %x %T %F %E %z %b %s \"%r\" \"%c\" %H \"%{Range}i\" \"%{Referer}i\" \"%{User-agent}i\""
//#define	DEFAULT_ACCESSLOG_FORMAT "\"%t\" %h \"%{Host}i\" \"%{X-Forwarded-For}i\" %P %x %T %F %E %z %b %s \"%r\" \"%c\" %H \"%S\" \"%{Range}i\" \"%{Referer}i\" \"%{User-agent}i\""
//#define	DEFAULT_ACCESSLOG_FORMAT "[%l] %h \"%{Host}i\" \"%{X-Forwarded-For}i\" %P %x %T %F %E %z %b %B %s %C \"%r\" \"%c\" %H \"%S\" \"%{Range}i\" \"%{Referer}i\" \"%{User-agent}i\""
#define	DEFAULT_ACCESSLOG_FORMAT "[%l] %h \"%{Host}i\" \"%{X-Forwarded-For}i\" %P %x %T %F %E %z %b %B %s %C \"%r\" \"%c\" %H %X \"%S\" \"%{Range}i\" \"%{Referer}i\" \"%{User-agent}i\""

char 			gscx__default_conf_path[512] = { 0, };
scx_config_t	*gscx__config = NULL; /* 전역 설정 정보를 기억하는 구조체 */
pthread_mutex_t 	gscx__config_lock = PTHREAD_MUTEX_INITIALIZER;	/* 전역 설정 정보를 사용하거나 변경할때 LOCK을 사용 해야함 */
extern service_core_t	gscx__server_stat;
//const char 		gscx__url_pattern[] 		= {"^[hH][tT][tT][pP][sS]?:\\/\\/([a-zA-Z0-9\\-\\.]+):?[0-9]*(\\/.+)$"};
//const char 		gscx__url_pattern[] 			= {"^[hH][tT][tT][pP][sS]?:\\/\\/([a-zA-Z0-9\\-\\.]+):?[0-9]*(\\/?.+)?$"};
const char 		gscx__url_pattern[] 			= {"^[hH][tT][tT][pP][sS]?:\\/\\/([a-zA-Z0-9.\\-]+):?[0-9]*(/?.+)?$"};
regex_t 		*gscx__url_preg = NULL;
char 			gscx__log_file[256];
size_t		 	gscx__xfer_size = 64 * 1024;	/* MHD에서 reader callback시 요청되는 buffer 크기 */
uid_t 			gscx__real_uid = 0;
gid_t 			gscx__real_gid = 0;
vstring_t 		*gscx__real_group=NULL;
site_t 			*gscx__default_site = NULL;
mem_pool_t 		*gscx__global_mp = NULL;
size_t 			gscx__site_pool_size = 1024*10; /* virtual host(site)별 memory pool의 크기  */
vstring_t 		*gscx__local_ip = NULL;
char 			gscx__access_log_string[256] = {0};
char 			gscx__error_log_string[256] = {0};
char 			gscx__origin_log_string[256] = {0};
scx_logger_t 	*scx_access_logger 	= NULL;
scx_logger_t 	*scx_error_logger 	= NULL;
scx_logger_t 	*scx_origin_logger 	= NULL;
vstring_t		*gscx__nc_trace_mask = NULL;
uint32_t 		gscx__debug_mode = NC_WARN|NC_ERROR|NC_INFO;
vstring_t 		*gscx__config_root = NULL;
vstring_t 		*gscx__cache_part[10];
int 			gscx__cache_part_count = 0;
int 			gscx__min_rapool = 4;
int 			gscx__max_rapool = 128;
int 			gscx__run_standalone = 0;
int 			gscx__ignore_cookie = 0;
vstring_t 		*gscx__pid_file = NULL;
char	 		gscx__child_msg_file[256] = {0};
char	 		gscx__default_log_path[256] = {0};
int				gscx__lua_pool_size = 0;	//lua state pool의 크기, 0일 경우 state pool을 사용하지 않는다.
int 			gscx__signal_shutdown = 0;
int 			gscx__need_rerun_child = 1;
int 			gscx__main_service_pid = 0;
lua_pool_t 		*gscx__lua_pool = NULL;
int 			gscx__need2parent_kill = 0;  //1이면 종료시 부모 프로세스를 같이 죽인다.
int				gscx__nc_inited = 0;	//netcache core의 초기화 여부 확인용
int				gscx__use_aesni = 0;	//AES-NI 기능 사용가능 여부, 1이면 AES-NI가 지원 가능
int				gscx__service_available = 0;	/* 초기 상태(0)에서는 client의 요청에 대해 모두 거부 하다 초기화가 모두완료 되면 1로 셋팅된다. */
char			gscx__netcache_ver[128] = ""; //netcache core version 저장용
time_t			gscx__start_time = 0;	/* 서버 시작 시간을 기록 */
int				gscx__check_listen_port = 65221;	/* netcache core로 부터 들어온 check 요청 처리만 하는 MHD port */
int				gscx__socket_count = 0; /* MHD 데몬에 연결된 socket 수 */
threadpool 		gscx__thpool = NULL; 	/* thread pool 포인터 */
int				gscx__enable_async = 1;	/* 비동기 처리 여부, 1: 비동기, 0: 동기 */
int				gscx__max_job_queue_len = 0;	/* 비동기 동작시 job queue의 최대 크기 */
int				gscx__max_url_len = DEFAULT_POOL_SIZE/2; 	/* 초기 url max 값은 4K이다. */
int				gscx__max_header_val_len = DEFAULT_POOL_SIZE/2; 	/* 초기 헤더 length의 max 값은 4K이다. */
int				gscx__permit_expire = 1;	/* 이 값이 1이면 만료된 인증 요청이라도 허용이 된다. */
bt_timer_t 		gscx__timer_permit_expire;
rewrite_scripts_t	*gscx__previous_rewrite_scripts = NULL;	/* 이전에 사용된 rewrite phase에서 동작하는 lua script */
rewrite_scripts_t	*gscx__current_rewrite_scripts = NULL;		/* 현재 사용중인 rewrite phase에서 동작하는 lua script */
__thread jmp_buf 	__thread_jmp_buf;
__thread int		__thread_jmp_working = 0; /* sigsetjmp()를 호출후 1로 설정하고 해당 scope를 벗어 날때 0으로 설정 해야 한다.*/

static double 	gscx__mavg_dur = 0.0;
static double 	gscx__mavg_max = 0.0;
static double 	gscx__mavg_min = 0.0;
static double 	gscx__mavg_dur_read = 0.0;
static double 	gscx__mavg_dur_open = 0.0;
static double 	gscx__mavg_dur_close = 0.0;
static long 	gscx__mavg_cnt = 0;
static pthread_mutex_t 	gscx__mavg_dur_lock = PTHREAD_MUTEX_INITIALIZER;
volatile long long int 	gscx__id_seq = 0;

int 			gscx__vmsize = 0;
int 			gscx__vmrss = 0;

extern volatile long long int     	g__scx_dynamic_memory_sum ;	//전체 할당량
extern volatile long long int		g__scx_adynamic_memory_sum;	//g__scx_dynamic_memory_sum + __scx_meminfo_t 헤더 크기

#ifndef O_MEGA
#define O_MEGA(x)		(((uint64_t)x) << 20)
#endif


struct MHD_Daemon *gscx__http_servers[SCX_PORT_TYPE_CNT] = {NULL, NULL, NULL, NULL};

static struct option long_options[] =  {
	{"standalone",   optional_argument, &gscx__run_standalone, 0},
	{"debug",       required_argument, NULL, 'D'},
	{"help",    		0, NULL, 'h'},
	{"version",    		0, NULL, 'v'},		/* 버전 정보 표시 */
	{"config",    		0, NULL, 'C'},		/* 로드된 설정 파일 내용 표시 */
	{"pid",    			0, NULL, 'p'},		/* PID 파일 경로 지정 */
	{"repair", 			0, NULL, 'r'},		/* 캐시 정합성 검사 */
	{NULL,    0, NULL, 0}
};


/*
 * callback 함수들이 리턴하는 값들
 */
#define 	NCE_TAG_CONTINUE  		0
#define 	NCE_TAG_ACCEPT_CONTINUE  	1
#define 	NCE_TAG_FAILURE_STOP 	 	2
/*
 * 표준 태그는 www.w3.org/Protocols/rfc2616/rfc2616-sec14.html 참조
 * Solbox 추가 태그
 * 		SBX_HTTP_HEADER_CACHE_ID
 * 		SBX_HTTP_HEADER_SERVING : {MEM_HIT,DISK_HIT,MISS, BYPASS...}
 */
#define 	SBX_HTTP_HEADER_CACHE_ID 	"X-Cache-ID"
#define 	SBX_HTTP_HEADER_SERVING 	"X-Serving"
#define 	SBX_HTTP_HEADER_PTTL		"X-Positive-TTL"
#define 	SBX_HTTP_HEADER_NTTL 		"X-Negative-TTL"
#define 	SBX_HTTP_HEADER_ORIGIN_SERVER 		"X-Origin-Server"
#define 	SBX_HTTP_HEADER_FOLLOW_REDIR 		"X-Follow-Redir"
#define 	SBX_HTTP_HEADER_PURGE_KEY 			"X-Purge-Key"
#define 	SBX_HTTP_HEADER_PURGE_HARD	 		"X-Purge-Hard"
#define 	SBX_HTTP_HEADER_PURGE_VOLUME 		"X-Purge-Volume"
#define 	SBX_HTTP_HEADER_PRELOAD		 		"X-Preload"
#define 	SBX_HTTP_HEADER_PRELOAD_STATUS 		"X-Preload-Status"
#define 	SBX_HTTP_HEADER_PRETTY		 		"X-Pretty"
#define 	SBX_HTTP_HEADER_CACHE_STATUS	 		"X-Cache-Status"
header_element_t __header_tags[] = {
	{MHD_HTTP_HEADER_ACCEPT,  			HEADER_REQ,						NULL},
	{MHD_HTTP_HEADER_ACCEPT_CHARSET,	HEADER_REQ,						NULL},
	{MHD_HTTP_HEADER_ACCEPT_ENCODING,	HEADER_REQ,						hdr_comm_accept_encoding},
	{MHD_HTTP_HEADER_ACCEPT_LANGUAGE,	HEADER_REQ,						NULL},
	{MHD_HTTP_HEADER_ACCEPT_RANGES,		HEADER_RES|HEADER_SKIP,			NULL},
	{MHD_HTTP_HEADER_AGE, 				HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_ALLOW, 			HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_AUTHORIZATION,		HEADER_RES|HEADER_SKIP,			hdr_req_authorization},
	{MHD_HTTP_HEADER_CACHE_CONTROL, 	HEADER_COMMON|HEADER_SKIP,		hdr_comm_cache_control},
	{MHD_HTTP_HEADER_CONNECTION, 		HEADER_COMMON|HEADER_SKIP,		hdr_comm_connection},
	{MHD_HTTP_HEADER_CONTENT_ENCODING, 	HEADER_RES,					NULL},	/* 당분간 오리진에 Accept-Encoding 헤더를 보내지 않는다. */
	{MHD_HTTP_HEADER_CONTENT_LANGUAGE, 	HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_CONTENT_LENGTH,	HEADER_COMMON|HEADER_SKIP,		hdr_comm_content_length},
	{MHD_HTTP_HEADER_CONTENT_LOCATION, 	HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_CONTENT_MD5,		HEADER_COMMON,					NULL},
	{MHD_HTTP_HEADER_CONTENT_RANGE, 	HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_CONTENT_TYPE, 		HEADER_COMMON,					NULL},
	{MHD_HTTP_HEADER_COOKIE, 			HEADER_COMMON|HEADER_SKIP, 		hdr_comm_cookie},/* 응답도 Cookie전송 가능, 서블릿들 */
	{MHD_HTTP_HEADER_DATE, 				HEADER_COMMON,				NULL},
	{MHD_HTTP_HEADER_ETAG,				HEADER_RES,				NULL},
	{MHD_HTTP_HEADER_EXPECT,			HEADER_REQ|HEADER_SKIP,			NULL},
	{MHD_HTTP_HEADER_EXPIRES,			HEADER_RES,				NULL},
	{MHD_HTTP_HEADER_FROM, 				HEADER_REQ, 				NULL},
	{MHD_HTTP_HEADER_HOST,				HEADER_REQ,				hdr_req_host},
	/* CONDITIONAL GET */
	{MHD_HTTP_HEADER_IF_MATCH, 			HEADER_REQ|HEADER_SKIP,			hdr_req_if_match},
	{MHD_HTTP_HEADER_IF_MODIFIED_SINCE, 		HEADER_REQ|HEADER_SKIP,			hdr_req_ims},
	{MHD_HTTP_HEADER_IF_NONE_MATCH,			HEADER_REQ|HEADER_SKIP,			hdr_req_if_not_match},
	{MHD_HTTP_HEADER_IF_RANGE,			HEADER_REQ|HEADER_SKIP,			hdr_req_ir},
	{MHD_HTTP_HEADER_IF_UNMODIFIED_SINCE, 		HEADER_REQ|HEADER_SKIP,		hdr_req_ius},

	{MHD_HTTP_HEADER_LAST_MODIFIED, 		HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_LOCATION,			HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_MAX_FORWARDS,			HEADER_REQ,						NULL},
	{MHD_HTTP_HEADER_PRAGMA,			HEADER_COMMON|HEADER_SKIP,					hdr_comm_pragma},
	{MHD_HTTP_HEADER_PROXY_AUTHENTICATE, 		HEADER_RES,					NULL},
	{MHD_HTTP_HEADER_PROXY_AUTHORIZATION, 		HEADER_REQ,					NULL},
	{MHD_HTTP_HEADER_RANGE,				HEADER_REQ|HEADER_SKIP,		hdr_req_range},
	/* This is not a typo, see HTTP spec */
	{MHD_HTTP_HEADER_REFERER, 			HEADER_REQ,						NULL},
	//{MHD_HTTP_HEADER_RETRY_AFTER,		}
	{MHD_HTTP_HEADER_SERVER,			HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_SET_COOKIE,		HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_SET_COOKIE2,		HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_TE,				HEADER_REQ,						NULL},
	{MHD_HTTP_HEADER_TRAILER,			HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_TRANSFER_ENCODING, 	HEADER_RES,					NULL},
	{MHD_HTTP_HEADER_UPGRADE, 			HEADER_REQ,						NULL},
	{MHD_HTTP_HEADER_USER_AGENT,		HEADER_REQ,						NULL},
	{MHD_HTTP_HEADER_VARY,				HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_VIA,				HEADER_RES,						NULL},
	{MHD_HTTP_HEADER_WARNING,			HEADER_COMMON,					NULL},
	{MHD_HTTP_HEADER_WWW_AUTHENTICATE,  HEADER_REQ,						NULL},
	/*
	 * 표준이 아니지만 널리 알려진 태그
	 */
	{"X-Frame-Options", 				HEADER_COMMON,					NULL},
	{"X-Forwarded-For", 				HEADER_REQ,						hdr_req_xff},
	/* Cross-site scripting (XSS) filter */
	{"X-XSS-Protection", 				HEADER_COMMON,					NULL},
	/* Content Security Policy definition */
	{"X-WebKit-CSP", 					HEADER_COMMON,					NULL},
	{"Content-Security-Policy", 					HEADER_COMMON,		NULL},
	{"X-Content-Security-Policy", 					HEADER_COMMON,		NULL},
	/* IE/Chrome등에서 MIME sniffing을 방지하기위해서 사용 */
	{"X-Content-Type-Options", 			HEADER_COMMON,					NULL},
	/* 서비스를 제공하기위해서 사용된 기술적 정보 */
	{"X-Powered-By", 					HEADER_COMMON,					NULL},
	{"X-Runtime", 						HEADER_COMMON,					NULL},
	{"X-Version", 						HEADER_COMMON,					NULL},
	{"X-ASPNet-Version", 				HEADER_COMMON,					NULL},
	/* 선호하는 렌더링 엔진을 추천하기위해 사용 */
	{"X-UA-Compatible", 				HEADER_RES,						NULL},
	/*
	 * Solbox 확장 태그
	 */
	{SBX_HTTP_HEADER_CACHE_ID,			HEADER_REQ|HEADER_SKIP,			hdr_req_cache_id},
	{SBX_HTTP_HEADER_SERVING, 			HEADER_RES,						NULL},
	{SBX_HTTP_HEADER_PTTL, 				HEADER_REQ|HEADER_SKIP,			hdr_req_pttl},
	{SBX_HTTP_HEADER_NTTL, 				HEADER_REQ|HEADER_SKIP,			hdr_req_nttl},
	{SBX_HTTP_HEADER_ORIGIN_SERVER, 	HEADER_REQ|HEADER_SKIP,			hdr_req_origins},
	{SBX_HTTP_HEADER_FOLLOW_REDIR, 		HEADER_REQ|HEADER_SKIP,			hdr_req_follow_redir},
	{SBX_HTTP_HEADER_PURGE_KEY, 		HEADER_REQ|HEADER_SKIP,			hdr_req_purge_key},
	{SBX_HTTP_HEADER_PURGE_HARD, 		HEADER_REQ|HEADER_SKIP,			hdr_req_purge_hard},
	{SBX_HTTP_HEADER_PURGE_VOLUME, 		HEADER_REQ|HEADER_SKIP,			hdr_req_purge_volume},
	{SBX_HTTP_HEADER_PRELOAD, 			HEADER_REQ|HEADER_SKIP,			hdr_req_preload},
	{SBX_HTTP_HEADER_PRELOAD_STATUS, 	HEADER_RES,						NULL},
	{SBX_HTTP_HEADER_PRETTY, 			HEADER_REQ|HEADER_SKIP,			hdr_req_pretty},
	{"End-Of-Tag", 						0, 								NULL}
};
#define 		HEADER_DICT_COUNT 		howmany(sizeof(__header_tags), sizeof(header_element_t))

typedef int (*nc_method_handler_t)(
		 const char *upload_data,
		 size_t * upload_data_size,
		 nc_request_t *req);

struct method_map_tag {
	char 					method_str[32];
	nc_method_t 			method_id;
	nc_method_handler_t		method_proc;
	int						async;		/* async 처리 method인 경우 1, 아니면 0 */
} __method_map[] = {	/* 여기에 정의된 순서가 바뀌면 안된다. */
	{MHD_HTTP_METHOD_GET, 		HR_GET, 	scx_handle_get,			1},
	{MHD_HTTP_METHOD_HEAD, 		HR_HEAD, 	scx_handle_head,		1},
	{MHD_HTTP_METHOD_POST, 		HR_POST ,	scx_handle_post,		0},
	{MHD_HTTP_METHOD_PUT, 		HR_PUT ,	scx_handle_put,			0},
	{MHD_HTTP_METHOD_DELETE, 	HR_DELETE ,	scx_handle_delete,		0},
	{MHD_HTTP_METHOD_OPTIONS, 	HR_OPTIONS,	scx_handle_options,		0},
	{"PURGE", 					HR_PURGE,	scx_handle_purge,		1},
	{"PRELOAD", 				HR_PRELOAD, scx_handle_preload,		0},
	{"STAT", 					HR_STAT, 	scx_handle_stat,		0},
	{"PURGEALL", 				HR_PURGEALL, 	scx_handle_purgeall,1},
	{"-", 						HR_UNKNOWN, 	scx_handle_unknown,	0}
};
static struct hsearch_data 	__method_table;
#define 	__method_map_count 	sizeof(__method_map)/sizeof(struct method_map_tag)


#define PAGE "<html><head><title>File not found</title></head><body>File not found</body></html>"
#define NULL_PAGE ""


typedef struct {
	char 	class[64];
	char 	path1[256];
	char 	path2[256];
} driver_dict_t;
driver_dict_t 		__driver_dict[] = {
	{"HTTPN", 		"/usr/lib64/libhttpn_driver.so", "/usr/lib/libhttpn_driver.so"},
	{"", 			"", ""} /* end mark */
};


/*
 * 	분산캐싱으로 redirection된 요청의 경우: <service-domain>.<local-name>
 */

static vstring_t *
scx_check_internal_redirection(nc_request_t *req, vstring_t *tvalue)
{
	int 	tval_len = vs_length(tvalue);
	int 	substr_off = 0;

	if ((req->config->host_name == NULL) ||
		(tval_len < vs_length(req->config->host_name)+2))
		return tvalue;

	substr_off = tval_len - vs_length(req->config->host_name);

	if (strcasecmp(vs_data(tvalue) + substr_off , vs_data(req->config->host_name)) == 0) {
		substr_off -= 1; /* '.' 제거 */
		vs_truncate(tvalue, substr_off);
		TRACE((T_DAEMON, "modified string is '%s'(host='%s')\n", vs_data(tvalue), vs_data(req->config->host_name)));
	}
	return tvalue;
}


#define 	MAVG_TICK_COUNT 	200
static double 	__msum_v[MAVG_TICK_COUNT];
static double 	__msum_vread[MAVG_TICK_COUNT];
static double 	__msum_vopen[MAVG_TICK_COUNT];
static double 	__msum_vclose[MAVG_TICK_COUNT];
static double 	__msum = 0.0;
static double 	__msum_sq = 0.0;
static double 	__msum_read = 0.0;
static double 	__msum_open = 0.0;
static double 	__msum_close = 0.0;
static long 	__b1 = 0x5AA5A55A;
static int 		__msum_cnt = 0;
static int 		__msum_full = 0;
static long 	__b2 = 0x5AA5A55A;
static int 		__msum_vmax = -1;
static int 		__msum_vmin = -1;
static double
scx_make_stddev()
{
	double 	v_stddev = 0.0;
	double 	v_sum = 0.0;
	int 	i;
	if (__msum_cnt > 0) {
		//TRACE((T_WARN, "msum_sq=%.2f, mavg_dur=%.2f\n", __msum_sq, gscx__mavg_dur));
		v_stddev = __msum_sq/(double)__msum_cnt - gscx__mavg_dur*gscx__mavg_dur;
	}
	return sqrt(v_stddev);
}
void
scx_make_mavg(double dur, double ncopen, double ncread, double ncclose)
{
	double __msum_dc 		= 0.0;
	double __msum_dc_read 	= 0.0;
	double __msum_dc_open 	= 0.0;
	double __msum_dc_close 	= 0.0;
	int 			cur_tick_cnt = 0;

	pthread_mutex_lock(&gscx__mavg_dur_lock);

//	if (dur < (ncread+ncopen+ncclose)) {
//		TRACE((T_ERROR, "scx_make_mavg - got strange value(dur=%.2f, open=%.2f, read=%.2f, close=%.2f)\n", (float)dur, (float)ncopen, (float)ncread, (float)ncclose));
//	}
	if (__msum_full) {
		__msum_dc  		= __msum_v[0];
		__msum_dc_read  = __msum_vread[0];
		__msum_dc_open  = __msum_vopen[0];
		__msum_dc_close = __msum_vclose[0];
		__msum_sq -=  (__msum_dc * __msum_dc);
		memmove(&__msum_v[0], 		&__msum_v[1], 		(MAVG_TICK_COUNT-1)*sizeof(double));
		memmove(&__msum_vread[0], 	&__msum_vread[1], 	(MAVG_TICK_COUNT-1)*sizeof(double));
		memmove(&__msum_vopen[0], 	&__msum_vopen[1], 	(MAVG_TICK_COUNT-1)*sizeof(double));
		memmove(&__msum_vclose[0], 	&__msum_vclose[1], 	(MAVG_TICK_COUNT-1)*sizeof(double));
		__msum_v[MAVG_TICK_COUNT-1] 	= dur;
		__msum_vread[MAVG_TICK_COUNT-1] = ncread;
		__msum_vopen[MAVG_TICK_COUNT-1] = ncopen;
		__msum_vclose[MAVG_TICK_COUNT-1]= ncclose;
		__msum_sq +=  (dur * dur);
		__msum_vmax -= 1;
		__msum_vmin -= 1;
		if (__msum_vmax == -1) {
			/* 최대값 정보 날라갔음...갱신 필요*/
			int 	i;
			__msum_vmax = 0;
			for (i = 1; i < MAVG_TICK_COUNT; i++) {
				if (__msum_v[i] > __msum_v[__msum_vmax]) {
					__msum_vmax = i;
				}
			}
		}
		else if (__msum_v[MAVG_TICK_COUNT-1] > __msum_v[__msum_vmax]) {
			__msum_vmax = MAVG_TICK_COUNT-1;
		}

		if (__msum_vmin == -1) {
			/* 최소값 정보 날라갔음...갱신 필요*/
			int 	i;
			__msum_vmin = 0;
			for (i = 1; i < MAVG_TICK_COUNT; i++) {
				if (__msum_v[i] < __msum_v[__msum_vmin]) {
					__msum_vmin = i;
				}
			}
		}
		else if (__msum_v[MAVG_TICK_COUNT-1] < __msum_v[__msum_vmin]) {
			__msum_vmin = MAVG_TICK_COUNT-1;
		}



		cur_tick_cnt = MAVG_TICK_COUNT;

	}
	else {
		__msum_sq 					+=  (dur * dur);
		//TRACE((T_INFO, "%d-th : dur=%.2f, __msum_sq=%.2f\n", __msum_cnt, dur, __msum_sq));
		__msum_v[__msum_cnt] 		= dur;
		__msum_vread[__msum_cnt] 	= ncread;
		__msum_vopen[__msum_cnt] 	= ncopen;
		__msum_vclose[__msum_cnt] 	= ncclose;
		cur_tick_cnt = __msum_cnt;
		if (__msum_cnt == 1) {
			__msum_vmax = __msum_cnt;
			__msum_vmin = __msum_cnt;
		}
		else {
			if (__msum_vmax == -1 || __msum_v[__msum_cnt] > __msum_v[__msum_vmax]) __msum_vmax = __msum_cnt;
			if (__msum_vmin == -1 || __msum_v[__msum_cnt] < __msum_v[__msum_vmin]) __msum_vmin = __msum_cnt;

		}
		__msum_cnt++;
		if (__msum_cnt == MAVG_TICK_COUNT) __msum_full++;
	}
	ASSERT(__b1 == __b2);
	__msum 		+= (dur 	- __msum_dc);
	__msum_read += (ncread 	- __msum_dc_read);
	__msum_open += (ncopen 	- __msum_dc_open);
	__msum_close+= (ncclose	- __msum_dc_close);

	gscx__mavg_cnt ++;
	gscx__mavg_max 		= __msum_v[__msum_vmax];
	gscx__mavg_min 		= __msum_v[__msum_vmin];
	gscx__mavg_dur 		= __msum/cur_tick_cnt;
	gscx__mavg_dur_read 	= __msum_read/cur_tick_cnt;
	gscx__mavg_dur_open 	= __msum_open/cur_tick_cnt;
	gscx__mavg_dur_close 	= __msum_close/cur_tick_cnt;

	pthread_mutex_unlock(&gscx__mavg_dur_lock);
	return ;

}

struct enum_dump_i {
	char 	*buf;
	int 	remained;
	int 	bytes;
	nc_request_t	*req;
};

static int
header_comparei(const void *a, const void *b)
{
	header_element_t 	*e1 = (header_element_t *)a;
	header_element_t 	*e2 = (header_element_t *)b;

	return strcasecmp(e1->name, e2->name);

}
int
nce_build_header_dict()
{
	qsort(__header_tags, HEADER_DICT_COUNT, sizeof(header_element_t), header_comparei);
	TRACE((T_INFO, "Compiling %d defined-tags done\n", HEADER_DICT_COUNT));
	return 0;
}
static int
hd_enum_property(const char *key, const char *val, void *cb)
{
	struct enum_dump_i 	*ed = cb;
	int 				n;
	int 				len = 0;

	TRACE((T_DAEMON, "[%llu]('%s', '%s')\n", ed->req->id, key, val));
	len = strlen(val);
	if(len >= ed->remained) {
		//memory overflow 방지
		return -1;
	}
	n = scx_snprintf(ed->buf, ed->remained, "%s : %s\n", key, val);
	ed->buf += n;
	ed->remained -= n;
	ed->bytes += n;
}
static void *
hd_dump_property(const char *msg, nc_xtra_options_t *root, nc_request_t	*req)
{
	char 	pbuf[2048]="";
	struct enum_dump_i 	ed = {pbuf, sizeof(pbuf), 0, req};
	kv_for_each(root, hd_enum_property, &ed);
	TRACE((T_DAEMON, "[%llu] %s : propertyes(%d bytes)\n%s\n", req->id, msg, ed.bytes, pbuf));
}

nc_request_t *
nc_create_request(void *cls, char *uri)
{
	mem_pool_t 		*pool = NULL;
	nc_request_t	*req = NULL;
	int 			l = 0;
	scx_config_t * config = scx_get_config();	//호출후 scx_release_config()를 꼭 해주어야한다.
	pool = mp_create(config->pool_size);
	req = (nc_request_t *)mp_alloc(pool, sizeof(nc_request_t));
	req->method 	= HR_UNKNOWN;
	req->t_req_fba = sx_get_time();
	req->pool 	= pool;
	req->connect_type	= (int)cls;
	if (SCX_HTTPS == req->connect_type) {
		req->secured = 1;
	}
	else 	{
		req->secured = 0;
	}
	req->id		= ATOMIC_ADD(gscx__id_seq, 1);
	if (req->id < 0)
		ATOMIC_SET(gscx__id_seq, 1);
	if (uri == NULL) {
		/* uri가 NULL 인 경우가 발생할수 없어야 하지만 만약을 위해서 예외 처리 한다. */
		req->p1_error = MHD_HTTP_BAD_REQUEST;
		req->url 		= vs_allocate(pool, 3, "/", 1);/* 로그 기록을 위해 임의의 값을 넣는다. */
		scx_error_log(NULL, "Empty request url.\n");
		l = 1;
	}
	else {
#if 0	/* request url에 query의 시작을 알리는 ?는 인코딩이 안되어 있을거라고 생각하고 url coding을 하지 않는다. vnc player의 경우는 예외 */
#ifdef ZIPPER
	/* encoding URL일 경우 decoding 한다.
	 * MHD에서 넘어온 uri 파라미터를 변경하지 않으면 MHD에서 Argument를 찾을때 문제가 발생한다.*/
	//l = scx_url_decode(uri);

		l = MHD_http_unescape (uri);	/* MHD 0.9.39 버전 부터 지원 */
#else
		l = strlen(uri);
#endif
#else
		l = strlen(uri);
#endif

		if(l <= gscx__max_url_len ) { /* url이 최대 허용 크기 보다 큰 경우는 에러 처리 한다. 현재 gscx__max_url_len 기본이 4096임 */
			if(*uri == 'h' || *uri == 'H') {
				/* uri 가 http로 시작하면 AbsoluteURI(uri 내에 호스트 정보가 포함된 경우) 라고 판단한다. */
				size_t     nmatch = 3;
				regmatch_t pmatch[3];
				int 		ret = 0;
				ret = regexec(gscx__url_preg, uri, nmatch, pmatch, 0);
				if(ret == 0) { /* 패턴 일치 */
					req->host		= vs_allocate(pool, pmatch[1].rm_eo - pmatch[1].rm_so, uri + pmatch[1].rm_so, 1);
					req->url 		= vs_allocate(pool, pmatch[2].rm_eo - pmatch[2].rm_so, uri + pmatch[2].rm_so, 1);
				}
			}
			if(!req->url) {
				req->url 		= vs_allocate(pool, l, uri, 1);
			}
#if 0
			/* encoding 된 URL일 거라고 생각하고 무조건 decoding 한다. */
			l = scx_url_decode(vs_data(req->url));
		vs_update_length(req->url, l);	/* vstring_t의 data를 직접 수정한 경우는 update_length가 필수 임 */
#endif
		}
		else {
			req->p1_error = MHD_HTTP_REQUEST_URI_TOO_LONG;
			req->url 		= vs_allocate(pool, gscx__max_url_len, uri, 1);/* 로그 기록을 위해 일부만 사용한다. */
			scx_error_log(NULL, "Larger than uri limit(%d), URI(len = %d, %s)\n", gscx__max_url_len, l, vs_data(req->url));
		}
	}
	req->req_url_size = l;
	/*
	 * req->ori_url에는 실제 origin에 요청할 url이 들어간다.
	 */
	req->ori_url		= vs_strdup(pool, req->url);
	/* 오리진 요청 URL은 url decoding 된 상태여야 한다. */
#if 0
	l = scx_url_decode(vs_data(req->ori_url));
#else
	/* ?(%3F), #(%23)만 제외하고 decoding 한다. */
	l = scx_url_decode_e(vs_data(req->ori_url));
#endif
	/* url에 slash(/)가 연속으로 들어 오는 경우 한개의 slash로 만든다 */
	l = scx_remove_slash(vs_data(req->ori_url));
	/* vstring_t의 data를 직접 수정한 경우는 update_length가 필수 임 */
	vs_update_length(req->ori_url, l);
	req->ovalue 		= NULL;
	req->oversion 	= 0;
	req->config		= config;
	req->async_ctx.phase 		= SRP_INIT;

	req->streaming	= NULL;

	/* 비동기 context 초기화 */
	req->async_ctx.cr = (void *)req;
	req->async_ctx.pos = 0;
	req->async_ctx.max = 0;
	req->async_ctx.res = 0;
	req->async_ctx.buf = NULL;
	req->async_ctx.job_avail = 1;	/* 비정상 complete callback 처리용 flag */
	req->norandom = 0;
	req->resp_body_compressed = 0;
	req->step = STEP_START;
	req->shm_log_slot = NULL;
	shm_log_create_request(req);
	TRACE((T_DAEMON, "[%llu] %s() URI[%s]\n", req->id, __func__, uri));
#if 0
	req->session_id	= 0;
	req->session	= NULL;
#endif
	return req;
}

/*
 * ori_url에서 argument를 제외한 path만 따로 저장
 * 확장자가 있는 경우 확장자도 따로 저장
 */
int scx_make_path_to_url(nc_request_t *req)
{
	int	query_off;	//query parameter의 시작 부분의 offset. ? 포함
	int	ext_off;
	int off = vs_length(req->ori_url);
	int len = 0;
	mem_pool_t 		*pool = req->pool;
	vstring_t 	*v_token = NULL;

	query_off = vs_pickup_token_r(NULL, req->ori_url, off, "?", &v_token, DIRECTION_RIGHT);
	if (query_off != 0) { /* argument가 포함된 경우 path에 별도의 메모리를 할당한다. */
		req->path 		= vs_allocate(pool, query_off, vs_data(req->ori_url), TRUE); /* read-only*/
	}
	else {
		/* argument가 없더라도 url decode를 위해 메모리를 할당한다. */
		req->path 		= vs_allocate(pool, off, vs_data(req->ori_url), TRUE); /* read-only*/
	}
#if 0
	// req->url이 아닌 req->ori_url를 사용하기 때문에 디코딩이 필요 없다.
	len = scx_url_decode(vs_data(req->path));
	/* vstring_t의 data를 직접 수정한 경우는 update_length가 필수 임 */
	vs_update_length(req->path, len);
#endif
	/* 확장자 저장 부분 */
	off = vs_length(req->path);
	ext_off = vs_pickup_token_r(NULL, req->path, off, ".", &v_token, DIRECTION_RIGHT);
	if (ext_off != 0) { /* 요청 경로에 확장자가 있는 경우에는 따로 저장 한다. */
		req->ext = vs_allocate(pool, vs_length(req->path) - ext_off, vs_data(req->path)+ext_off+1, TRUE); /* read-only*/
	}

	return SCX_YES;
}

int
nc_build_request(nc_request_t *req)
{
	mem_pool_t 		*pool = req->pool;
	client_info_t 	*client = NULL;
	req->objlength = -1;
	scx_make_path_to_url(req);
#if 0
	req->options 		= kv_create_pool(pool, mp_alloc);
	req->scx_res.headers	= kv_create_pool(pool, mp_alloc);
#else
	req->options              = kv_create_pool_d(pool, mp_alloc, __FILE__, __LINE__);
	req->scx_res.headers      = kv_create_pool_d(pool, mp_alloc, __FILE__, __LINE__);
#endif
	if (req->connect_type == SCX_RTMP) {
		/* RTMP의 경우에는 이후 과정을 진행 할 필요가 없다. */
		return SCX_YES;
	}
	scx_get_session_info(req);
	if(req->connect_type == SCX_HTTP_TPROXY) {	/* tproxy port를 통해서 들어온 경우만 httpn 드라이버쪽에 아래의 값들을 넘겨 준다. */
		client = (client_info_t *)mp_alloc(pool, sizeof(client_info_t));
		memcpy(client->ip,vs_data(req->client_ip), 64);
		client->size = sizeof(client_info_t);
		kv_set_client(req->options, (void *)client, client->size);
	}
#if 0
	if (HR_GET == req->method) {	/* GET 이외의 method에서는 속도 제한이 필요 없다.*/
		limitrate_make((void *)req);
	}
#endif
	return SCX_YES;
}

static int
hdr_comm_accept_encoding(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	/* 요청에 Accept-Encoding 헤더가 포함된 경우 오리진에 Full GET 방식으로 요청을 하도록 한다. */
	if (!ignore) {
		TRACE((T_DAEMON, "[%llu] URL[%s] - marked norandom(%s) Accept-Encoding\n", req->id, vs_data(req->url), vs_data(value)));
//		req->norandom = 1;	/* 이 부분은 좀더 검증이 필요한 부분이라 일단 보류함 */
	}
	return NCE_TAG_CONTINUE;
}

static int
hdr_req_authorization(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	if (req->service->use_origin_authorization == 1) {
		/* 오리진 요청시 인증헤더를 추가해야하는 경우(양방향 오리진) Client로 부터 들어온 Authorization 헤더는 삭제 되도록 한다. */
		return NCE_TAG_CONTINUE;
	}
	return NCE_TAG_ACCEPT_CONTINUE;
}
static int
hdr_comm_cache_control(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	if (!ignore && vs_strcasestr_lg(value, "no-cache") != NULL) {
//		TRACE((T_DAEMON, "[%llu] URL[%s] - marked no-cache(%s) by cache-control\n", req->id, vs_data(req->url), vs_data(value)));
//		req->nocache = 1;
	}
	else if (!ignore && vs_strcasestr_lg(value, "max-age=0") != NULL) {
//		TRACE((T_DAEMON, "[%llu] URL[%s] - marked no-cache(%s) by cache-control\n", req->id,vs_data(req->url), vs_data(value)));
//		req->nocache = 1;
	}
	return NCE_TAG_CONTINUE;
}

static int
hdr_comm_content_length(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	int 	r = NCE_TAG_CONTINUE;

	if (req->method == HR_PUT || req->method == HR_POST) {
		/*
		 * PUT, POST의 경우 content-length를 다음  tier에 전달
		 * 아닌경우 tag 등록 matrix에 보다시피, content-length는 next-tier에게 전달되지 않음
		 */
		r = NCE_TAG_ACCEPT_CONTINUE;
	}
	if (!ignore)
		req->objlength = strtoll(vs_data(value), NULL, 10);
	return r;
}
static int
hdr_comm_connection(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	int 	r = NCE_TAG_CONTINUE;

	if (!ignore && vs_strcasestr_lg(value, "keep-alive")) {
		req->keepalive = 1;
	}
	return r;
}
/*
 * possible host-name
 * 	분산캐싱으로 redirection된 요청의 경우: <service-domain>.<local-name>
 *  client로부터 수신하는 경우 : <service-domain>
 */
/*
 *
 * 주의: host 태그는 ignore가 TRUE라도 무시하고 해석된다
 *
 */
static int
hdr_req_host(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	vstring_t 	*rep = NULL;
	int 		r = NCE_TAG_CONTINUE;
	vstring_t	*tvalue;


	/* req->host에 값이 할당된(absolute URI) 경우 여기를 skip 한다.*/
	if(req->host)
		return NCE_TAG_CONTINUE;
	/* 현재는 scx_request_handler()에서 req->host를 생성하므로 아래 부분은 동작 하지 않는다. */
/*
	if (strcasecmp(vs_data(value), "127.0.0.1") == 0) {
		req->p1_error = MHD_HTTP_BAD_REQUEST;
		return NCE_TAG_FAILURE_STOP;
	}
*/
	tvalue = vs_strdup(req->pool, value);

	/*
	 * 분산 캐싱이 살아있는 경우, 그로 인해 host
	 * 정보가 만들어졌을 개연성이 있음.
	 * 그런경우 host정보 재 구축이 필요
	 */
	tvalue = scx_check_internal_redirection(req, tvalue);

	req->host = vs_strdup(req->pool, tvalue);
	return r;
}
static int
hdr_req_if_match(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	if (req->method == HR_POST) {
		return NCE_TAG_ACCEPT_CONTINUE;
	}
	if (!ignore) {
		req->etag = vs_strdup(req->pool, value);
		req->condition = HTTP_IF_MATCH;
	}
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_if_not_match(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	if (req->method == HR_POST) {
		return NCE_TAG_ACCEPT_CONTINUE;
	}
	if (!ignore) {
		req->etag = vs_strdup(req->pool, value);
		req->condition = HTTP_IF_NOT_MATCH;
	}
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_xff(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	vstring_t 	*vs_newval;

	if (!ignore) {
		vs_newval = vs_allocate(req->pool, 32 + (value?vs_length(value):0), NULL, FALSE);
		if (value) {
			vs_strcat(vs_newval, value);
			vs_strcat_lg(vs_newval, ",");
		}
		vs_strcat(vs_newval, req->client_ip);
		*replaced = vs_newval;
	}
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_ims(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	/* ims message */
	time_t 				ov = 0;
	if (req->method == HR_POST) {
		/*
		 * POST의 경우 no-cache로 동작하므로 If-Modified-Since를 오리진에 제공해도 문제는 없음
		 * 또한 PRG (Post-Redirection-Get)형태의 사이트의 오퍼레이션을 완벽히 지원하려면
		 * POST에서 client에서 제공된 해당 태그가 원본에 전송될 필요가 있음
		 * 참조:http://en.wikipedia.org/wiki/File:PostRedirectGet_DoubleSubmitSolution.png
		 */
		return NCE_TAG_ACCEPT_CONTINUE;
	}
	if (!ignore) {
		ov = curl_getdate(vs_data(value), NULL);
		if (ov < 0x7FFFFFFF && ov >= 0) {
			req->condition = HTTP_IF_IMS;
			req->timeanchor  = ov;
		}
		else {
			TRACE((T_DAEMON, "[%llu] URL[%s] - invalid tag, ('%s', '%s')\n", req->id, vs_data(req->url), vs_data(tag), vs_data(value)));
			return NCE_TAG_FAILURE_STOP;
		}
	}
	return NCE_TAG_CONTINUE;

}
static int
hdr_req_ir(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	struct tm			tm_ims;
	time_t 				ov = 0;
	if (req->method == HR_POST) {
		return NCE_TAG_ACCEPT_CONTINUE;
	}
	if (!ignore) {
		ov = curl_getdate(vs_data(value), NULL);
		if (ov < 0x7FFFFFFF && ov >= 0) {
			req->timeanchor  = ov;
		}
		else {
			req->etag = vs_strdup(req->pool, value);
		}
		req->condition = HTTP_IF_IR;
	}
//	TRACE((T_WARN, "URL[%s] : tag, '%s' - not implemented\n", vs_data(req->url), vs_data(tag)));
	return NCE_TAG_CONTINUE;
}
static int
hdr_comm_cookie(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	/*
	 * COOKIE가 설정되어 있는 경우 private|nocache로 간주
	 */
	if (!ignore) {
		req->private = 1;
		TRACE((T_DAEMON, "[%llu] URL[%s] - marked no-cache(%s) by cookie\n", req->id, vs_data(req->url), vs_data(value)));
	}
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_ius(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	time_t 				ov = 0;
	if (req->method == HR_POST) {
		return NCE_TAG_ACCEPT_CONTINUE;
	}
	if (!ignore) {
		ov = curl_getdate(vs_data(value), NULL);
		if (ov < 0x7FFFFFFF && ov >= 0) {
			req->condition = HTTP_IF_IUS;
			req->timeanchor  = ov;
		}
		else {
			TRACE((T_DAEMON, "[%llu] URL[%s] - invalid tag, ('%s', '%s')\n", req->id, vs_data(req->url), vs_data(tag), vs_data(value)));
			return NCE_TAG_FAILURE_STOP;
		}
	}

	return NCE_TAG_CONTINUE;
}
static int
hdr_comm_pragma(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	/* 
	 * RFC 2615에 달랑 하나의 값만 정의됨, "no-cache" 
	 * 그러므로 값을 비교히자않는다. 당연이 no-cache라고 가정
	 */
	if (!ignore) {
//		req->nocache = 1;
//		TRACE((T_DAEMON, "[%llu] URL[%s] - marked no-cache(%s) by pragma\n", req->id, vs_data(req->url), vs_data(value)));
		if (req->method == HR_POST) {
			return NCE_TAG_ACCEPT_CONTINUE;
		}
	}

	return NCE_TAG_CONTINUE;
}
int
hdr_req_range(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	char 				*eq_pos;
	char 				*sep_pos;
	int 				data_len = vs_length(value);

	TRC_BEGIN((__func__));

	if (!(req->method == HR_GET || req->method == HR_HEAD)) {	/* HEAD일 경우도 Range 요청이 가능 해야 할것 같음 */
		TRC_END;
		return NCE_TAG_FAILURE_STOP;
	}

	if (!ignore) {

		req->range.begin 	= RANGE_UNDEFINED;
		req->range.end 		= RANGE_UNDEFINED; /* EOF */


		TRACE((T_DEBUG, "[%llu] Range = {%s}\n", req->id, vs_data(value)));
		if ((eq_pos = vs_strchr_lg(value, '=')) == NULL) {
			TRACE((T_DAEMON, "[%llu] '%s' - no '=' found\n", req->id, vs_data(value)));
			TRC_END;
			return NCE_TAG_FAILURE_STOP;
		}

		if (vs_strncasecmp_lg(value, "Bytes", ((long long)eq_pos - (long long)vs_data(value))) != 0) {
			TRACE((T_DAEMON, "[%llu] '%s' - byte-unit not found\n", req->id, vs_data(value)));
			TRC_END;
			return NCE_TAG_FAILURE_STOP;
		}
		if ((sep_pos = vs_strchr_lg(value, '-')) == NULL) {
			TRACE((T_DAEMON, "[%llu] '%s' - range sep '-' not found\n", req->id, vs_data(value)));
			TRC_END;
			return NCE_TAG_FAILURE_STOP;
		}

		if ((eq_pos + 1) == sep_pos) {
			/* Ranges: bytes=-???? 파일의 마지막 n byte*/
//			req->range.begin 	= RANGE_UNDEFINED;
			req->range.end 		= atoll(sep_pos + 1);
			if (req->range.end < 0LL) {
				TRC_END;
				return NCE_TAG_FAILURE_STOP;
			}
			req->range.size		= req->range.end;	/* range.size는 'bytes=-????'와 같이 마지막 몇바이트식으로 요청되는 경우만 설정이 된다. */
		}
		if (((eq_pos + 1) != sep_pos) && ((char *)vs_data(value) + data_len > sep_pos + 1)) {
			/* Ranges: bytes=????-???? */
			req->range.begin 	= atoll(eq_pos + 1);
			req->range.end 		= atoll(sep_pos + 1);

			if (req->range.end < 0LL || (req->range.begin > req->range.end)) {
				TRC_END;
				return NCE_TAG_FAILURE_STOP;
			}
		}
		if (((eq_pos + 1) != sep_pos) && ((char *)vs_data(value) + data_len == sep_pos + 1)) {
			/* Range : bytes=????- n byte부터 파일의 끝까지*/
			req->range.begin 	= atoll(eq_pos + 1);
			req->range.end 		= RANGE_UNDEFINED; /* EOF */
		}
		/*
		 * IO를 시작하는 시점, 완료 시점 정보 준비
		 */
		//이계산을 여기서 하면 안될것 같다.
		req->remained 	= req->range.end - req->range.begin + 1;
		req->cursor 	= req->range.begin;
		req->subrange = 1;
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_cache_id(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	TRC_BEGIN((__func__));
	if (!ignore) {
		req->object_key = vs_strdup(req->pool, value);
		TRACE((T_DAEMON, "[%llu] URL[%s] - Cache ID set to %s\n", req->id, vs_data(req->url), vs_data(value)));
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_pttl(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	nc_time_t 	pttl = 0;
	TRC_BEGIN((__func__));
	if (!ignore) {
		pttl = atoll(vs_data(value));
		kv_set_pttl(req->options, pttl);
		TRACE((T_DAEMON, "[%llu] URL[%s] - PTTL set to %lld\n", req->id, vs_data(req->url), pttl));
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_nttl(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	nc_time_t 	nttl = 0;
	TRC_BEGIN((__func__));

	if (!ignore) {
		nttl = atoll(vs_data(value));
		kv_set_nttl(req->options, nttl);
		TRACE((T_DAEMON, "[%llu] URL[%s] - NTTL set to %lld\n", req->id, vs_data(req->url), nttl));
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}
static int
hdr_req_origins(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
/*
 * value format: 'v=%lld;s=<space-separated string>
 */
	nc_time_t 	nttl = 0;
	off_t 		toffset = 0;
	vstring_t 	*v_o = NULL;
	TRC_BEGIN((__func__));


	if (!ignore) {
		vstring_t 	*token = NULL;
		toffset = vs_pickup_token(req->pool, value, toffset, ";", &token);
		if (!token) {
			TRACE((T_DAEMON, "[%llu] Invalid tag syntax for %s = %s\n", req->id, vs_data(tag), vs_data(value)));
			TRC_END;
			return NCE_TAG_FAILURE_STOP;
		}
		req->oversion = strtoll(vs_data(token)+2, NULL, 10);
		if (req->oversion <= 0) {
			TRACE((T_DAEMON, "[%llu] Invalid tag syntax for %s = %s\n", req->id, vs_data(tag), vs_data(value)));
			TRC_END;
			return NCE_TAG_FAILURE_STOP;
		}
		toffset = vs_pickup_token(req->pool, value, toffset, ";", &token);
		if (!token) {
			TRACE((T_DAEMON, "[%llu] Invalid tag syntax for %s = %s\n", req->id, vs_data(tag), vs_data(value)));
			TRC_END;
			return NCE_TAG_FAILURE_STOP;
		}
		toffset = 0;
		v_o = token;
		toffset = vs_pickup_token(req->pool, v_o, toffset, "=", &token);
		/* 실제 오리진 정보  */
		toffset = vs_pickup_token(req->pool, v_o, toffset, "=", &token);
		if (!token) {
			TRACE((T_DAEMON, "[%llu] Invalid tag syntax for %s = %s\n", req->id, vs_data(tag), vs_data(value)));
			TRC_END;
			return NCE_TAG_FAILURE_STOP;
		}
		req->ovalue = token;
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}

static int
hdr_req_follow_redir(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	int 	follow_redir = 0;
	TRC_BEGIN((__func__));

	if (!ignore && req->service) {
		if(vs_strcasestr_lg(value, "yes") != NULL) {
			follow_redir = 1;
		}
		else if(vs_strcasestr_lg(value, "no") != NULL) {
			follow_redir = 0;
		}
		else {
			TRC_END;
			return NCE_TAG_CONTINUE;
		}
		if(follow_redir != 1) follow_redir = 0; /* X-Follow-Redir 헤더에 'yes'를 제외한 다른 값이 오는 경우는 무조건 0로 셋팅된다. */
		pthread_mutex_lock(&req->service->lock);
		if(follow_redir != req->service->follow_redir) {
			req->service->follow_redir = follow_redir;
			nc_ioctl(req->service->core->mnt, NC_IOCTL_FOLLOW_REDIRECTION, &follow_redir, sizeof(follow_redir));
			TRACE((T_DAEMON, "[%llu] URL[%s] - FOLLOW_REDIRECTION set to %d\n", req->id, vs_data(req->url), req->service->follow_redir));
		}
		pthread_mutex_unlock(&req->service->lock);
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}

static int
hdr_req_purge_key(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	int 	purge_key = 0;
	TRC_BEGIN((__func__));

	if (!ignore) {
		if(vs_strcasestr_lg(value, "yes") != NULL) {
			purge_key = 1;
		}
		else if(vs_strcasestr_lg(value, "no") != NULL) {
			purge_key = 0;
		}
		else {
			TRC_END;
			return NCE_TAG_CONTINUE;
		}
		req->purge_key = purge_key;
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}

static int
hdr_req_purge_hard(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	int 	purge_hard = 0;
	TRC_BEGIN((__func__));

	if (!ignore) {
		if(vs_strcasestr_lg(value, "yes") != NULL) {
			purge_hard = 1;
		}
		else if(vs_strcasestr_lg(value, "no") != NULL) {
			purge_hard = 0;
		}
		else {
			TRC_END;
			return NCE_TAG_CONTINUE;
		}
		req->purge_hard = purge_hard;
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}

static int
hdr_req_purge_volume(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{
	int 	purge_volume = 0;
	TRC_BEGIN((__func__));

	if (!ignore) {
		if(vs_strcasestr_lg(value, "yes") != NULL) {
			purge_volume = 1;
		}
		else if(vs_strcasestr_lg(value, "no") != NULL) {
			purge_volume = 0;
		}
		else {
			TRC_END;
			return NCE_TAG_CONTINUE;
		}
		req->purge_volume = purge_volume;
	}
	TRC_END;
	return NCE_TAG_CONTINUE;
}

/*
 * HEAD Method로 들어온 요청중에 "X-Preload: yes" 헤더나 "X-Preload: status" 헤더가 포함 되어 있는 경우
 * Method를 PRELOAD로 변경해서 preload 로 동작 하도록 한다.
 */
static int
hdr_req_preload(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{

	if (!ignore) {
		if(vs_strcasestr_lg(value, "yes") != NULL || vs_strcasestr_lg(value, "status") != NULL) {
			if (SCX_MANAGEMENT != req->connect_type) {
				/* management port로 들어온 preload 요청이 아닌 경우에는 해당 preload 헤더를 무시한다. */
				return NCE_TAG_CONTINUE;
			}
			if (HR_HEAD == req->method) {
				req->method = HR_PRELOAD;
				/* access log에 기록될 method로 PRELOAD로 변경한다. */
				req->zmethod = vs_allocate(req->pool, strlen(__method_map[req->method].method_str), __method_map[req->method].method_str, 1);
				TRACE((T_DAEMON|T_DEBUG, "[%llu] set preload.\n", req->id));
			}
		}
	}
	return NCE_TAG_CONTINUE;
}

/*
 * 상태 조회를 위한 restapi 요청에 헤더에  "X-Pretty: yes" 가 들어 있는 경우
 * json 응답시 공백과 개행문자를 사용해서 보기 좋게 응답한다.
 */
static int
hdr_req_pretty(vstring_t *tag, vstring_t *value, unsigned int flag, nc_request_t *req, vstring_t **replaced, int ignore)
{

	if (!ignore) {
		if(vs_strcasestr_lg(value, "yes") != NULL) {
			if (SCX_MANAGEMENT != req->connect_type) {
				/* management port로 들어온 요청이 아닌 경우에는 해당 헤더를 무시한다. */
				return NCE_TAG_CONTINUE;
			}
			req->pretty = 1;
		}
	}
	return NCE_TAG_CONTINUE;
}

static int
nce_is_range_satisfiable(nc_request_t *req)
{
	if (!req->streaming) {	/* streaming 기능 사용시에는 req->file이 NULL 경우가 있음 */
		if (!req->file && !req->scx_res.body) { /* req->file이 NULL이라도 body를 동적 생성한 경우는 rangable함. */
			TRACE((T_INFO, "[%llu] URL[%s] - null object\n", req->id, vs_data(req->url)));
			return 0;
		}
	}
	if (!req->subrange) {
		TRACE((T_INFO, "[%llu] URL[%s] - not sub-range request\n", req->id, vs_data(req->url)));
		return 0;
	}

	if (req->range.begin > req->range.end && req->range.end != RANGE_UNDEFINED) {
		TRACE((T_INFO, "[%llu] URL[%s] - begin(%lld) > end(%lld)\n", req->id, vs_data(req->url), req->range.begin, req->range.end));
		return 0;
	}

	if (!req->objstat.st_sizedecled) {
		TRACE((T_INFO, "[%llu] URL[%s] - no-random object\n", req->id, vs_data(req->url)));
		return 0;
	}

	if (req->range.end >= req->objstat.st_size) {
		TRACE((T_INFO, "[%llu] URL[%s] - end(%lld) > obj.size(%lld)\n", req->id, vs_data(req->url), req->range.end, req->objstat.st_size));
		return 0;
	}


	return 1;
}

/*
 * client의 range요청및 full get 요청에 대해 전송해야할 위치와 크기를 계산한다.
 * req->remained 에는 전송해야 할 크기가 들어가고 req->cursor에는 전송할 위치가 들어 간다.
 * HLS의 경우는 remained와 cursor에는 실제 파일에 대한 정보가 아닌
 * 만들어진 미디어 파일의 정보가 들어간다.
 */
static int
scx_operate_range(nc_request_t 	*req)
{
	uint64_t content_size;
	ASSERT(!req->range.operated);	/* 논리적 오류로 중복 계산을 방지하기 위해 추가 */
	req->range.operated = 1;
	content_size = req->objstat.st_size;

	if (req->subrange) { /* range 요청 처리 */
		//begin과 end의 값이 맞는지 검사
		if(req->range.begin < 0 ||
			req->range.begin > (content_size-1)) {
			if(req->range.begin != RANGE_UNDEFINED) {
				//req->range.begin = 0;
				return MHD_HTTP_REQUESTED_RANGE_NOT_SATISFIABLE;
			}
		}
		if (req->range.begin == 0 && content_size == 0) {
			// 컨텐츠 크기가 0인 파일에 대해 0-xxx나 0-0, 0- 의 형태로 요청이 들어온 경우 200으로 응답한다.
			req->remained = 0;
			req->cursor = 0;
			req->subrange = 0;
			return 0;
		}
		if(	req->range.end > (content_size-1)) {
			req->range.end = content_size-1;
		}
		//end가 정의 되지 않은 경우  파일 크기로 셋팅한다.
		/* Range : bytes=????- n byte부터 파일의 끝까지*/
		if (req->range.end == RANGE_UNDEFINED ) {
			req->range.end = content_size-1;
		}
		else if (req->range.end < 0) {	/* 현재 이 경우는 없음 */
			req->range.end = content_size-1;
		}
		//begin이 정의 되지 않은 경우 동작
		/* Ranges: bytes=-???? 파일의 마지막  n byte만 전송하는 경우*/
		if (req->range.begin == RANGE_UNDEFINED ) {
			if (req->range.size > content_size) {
				req->range.size = content_size;
			}
			req->range.begin = content_size - req->range.size;
			req->remained = req->range.end;
			req->range.end = content_size-1;
			req->cursor = req->range.begin;
		}
		if(req->range.begin > req->range.end) {
			if(req->streaming) {
				req->range.begin = 0;
				req->range.end = content_size-1;
			}
			else {
				return MHD_HTTP_REQUESTED_RANGE_NOT_SATISFIABLE;
			}
		}
		req->remained 	= req->range.end - req->range.begin + 1;
		req->cursor 	= req->range.begin;

	}
	else {  /* full get 요청 처리  */
		req->remained = content_size;
		req->cursor = 0;
	}
	return 0;
}

static int
scx_operate_condition(nc_request_t 	*req)
{
	int ret = 1;
	switch (req->condition) {
		case HTTP_IF_NOT_MATCH:
			if (vs_strcasecmp_lg(req->etag, req->objstat.st_devid) == 0) {
				/* etag 일치, 일치하는 경우 진행할 수 없음 */
				req->resultcode = MHD_HTTP_NOT_MODIFIED;
				nce_create_response_from_buffer(req, 0, NULL);
				nce_add_basic_header(req, req->response);
				nce_add_file_info(req, &req->objstat);
			}
			break;
		case HTTP_IF_MATCH:
			if (vs_strcasecmp_lg(req->etag, req->objstat.st_devid) != 0) {
				/* etag 불일치, 불일치의 경우 진행할 수 없음 */
				req->resultcode = MHD_HTTP_PRECONDITION_FAILED;
				nce_create_response_from_buffer(req, 0, NULL);
				nce_add_basic_header(req, req->response);
				nce_add_file_info(req, &req->objstat);
			}
			break;
		case HTTP_IF_IMS:
			if (req->timeanchor == req->objstat.st_mtime) {
				/* 해당 객체가 기준 시간과 동일함*/

				req->resultcode = MHD_HTTP_NOT_MODIFIED;
				nce_create_response_from_buffer(req, 0, NULL);
				nce_add_basic_header(req, req->response);
				nce_add_file_info(req, &req->objstat);
			}
			break;
		case HTTP_IF_IUS:
			if (req->timeanchor < req->objstat.st_mtime) {
				/* 해당 객체가 기준 시간 이후에 변경되었음 */

				req->resultcode = MHD_HTTP_PRECONDITION_FAILED;
				nce_create_response_from_buffer(req, 0, NULL);
				nce_add_basic_header(req, req->response);
				nce_add_file_info(req, &req->objstat);
			}
			break;
		case HTTP_IF_IR:
			if(req->subrange != 1) {
				/* If-Range는 Range 헤더가 들어올때만 동작을 한다. */
				break;
			}
			/* 비교 기준과 맞지 않을 경우 range 응답을 하지 않도록 함 */
			if(req->timeanchor) {	/* Last-Modified Time 기준 비교*/
				if (req->timeanchor == req->objstat.st_mtime) {
					break;
				}
			}
			else if(req->etag) {	/* ETag 기준 비교 */
				if (vs_strcasecmp_lg(req->etag, req->objstat.st_devid) == 0) {
					break;
				}
			}
			else {

			}
			req->subrange = 0;
			req->remained = req->objstat.st_size;
			req->cursor = 0;
			break;
	}
	return ret;
}

static int
scx_interpret_request_header(void *cr, enum MHD_ValueKind kind, const char *key, const char *value)
{
	nc_request_t 	*req = cr;
	header_element_t qry ;
	header_element_t *res = NULL;
	int 			r = 0;
	vstring_t 		*replaced = NULL;
	int 			ignore = 0;
	int				key_len = 0, value_len = 0;
	vstring_t 		*vkey = NULL;
	vstring_t 		*vvalue = NULL;

	key_len = strlen(key);
	value_len = strlen(value);
	req->req_hdr_size += strlen(key) + strlen(value) + 2; //\r\n 길이 포함
	if (key_len >= MAX_HEADER_NAME_LENGTH ) {
		req->p1_error = MHD_HTTP_REQUEST_ENTITY_TOO_LARGE;
		scx_error_log(req, "URL[%s]: header tag(len=%d) Too Large(max=%d). header : '%s'\n", vs_data(req->url), strlen(key), MAX_HEADER_NAME_LENGTH, key);
		return MHD_NO;
	}
	else if (value_len >= gscx__max_header_val_len) {
		req->p1_error = MHD_HTTP_REQUEST_ENTITY_TOO_LARGE;
		scx_error_log(req, "URL[%s]: header value(len=%d) Too Large(max=%d). header : '%s', value : '%s'\n", vs_data(req->url), strlen(value), gscx__max_header_val_len, key, value);
		return MHD_NO;
	}
	strncpy(qry.name, key, MAX_HEADER_NAME_LENGTH - 1);
	qry.name[MAX_HEADER_NAME_LENGTH] = '\0';


	/*
	 * key, data에 대해서 strdup대신, 포인터만 설정
	 * MHD에서 이미 key, value를 별도로 메모리 할당해서 저장하고 있는 상태이므로
	 * 또한번 중복해서 저장할 필요없이, 그에 대한 포인터만 관리
	 */
	vkey 	= vs_allocate(req->pool, key_len, key, FALSE);
	vvalue 	= vs_allocate(req->pool, value_len, value, FALSE);
	r = NCE_TAG_CONTINUE;
	ignore = 0;
	/*
	 * 동작 로직:
	 * 	1. scx_skip_interpret()는 ignore_headers 설정에 지정된 헤더의 경우 해석및 오리진으로 전달을 하지 않는다.
	 * 	2. __header_tags에 정의 되지 않은 헤더의 경우 ignore가 1이면 오리진에 헤더를 전달하지 않음
	 * 	3. __header_tags에 헤더 별로 지정된 핸들러 함수(res->proc)에서 ignore가 1로 설정된 경우 해당 헤더에 대한 처리(IMS, rang등)를 하지 않고 skip함.
	 * 	  3.1 해당 헤더에 HEADER_SKIP이 설정되고 핸들러 함수에서 NCE_TAG_CONTINUE나 NCE_TAG_FAILURE_STOP를 리턴 하는 경우는 오리진에 헤더를 전달하지 않음.
	 * 	  3.2 NCE_TAG_ACCEPT_CONTINUE가 리턴되는 경우라도 ignore가 1이면 오리진에 헤더를 전달하지 않음
	 */
	if (scx_skip_interpret(req, vkey))  {
		/*
		 * ignore_headers에 지정된 헤더는 어떤 경우에든 오리진에 헤더를 전달 하지 않는다.
		 */
		ignore = 1;
		TRACE((T_DAEMON, "[%llu] URL[%s]: header '%s': '%s' is ignored\n", req->id, vs_data(req->url), vs_data(vkey), vs_data(vvalue)));
	}

	res = bsearch(&qry, __header_tags, HEADER_DICT_COUNT, sizeof(header_element_t), header_comparei);
	if (res == NULL) {
		/* __header_tags 에 정의 되지 않은 헤더인 경우 */

		if (ignore == 0) {
			/* 오리진에 헤더 전달 */
			TRACE((T_DAEMON, "[%llu] URL[%s]: header tag '%s' not find in tag dict, added\n", req->id, vs_data( req->url), key));
			kv_add_val_d(req->options, (const char *)vs_data(vkey), (const char *)(replaced == NULL? vs_data(vvalue): vs_data(replaced)), __func__, __LINE__);
		}
		else {
			/* req->options의 key list 저장은 되지만 netcache Core에서 origin 요청 하는 헤더에는 사용되지 않는다. */
			TRACE((T_DAEMON, "[%llu] URL[%s]: header tag '%s' not find in tag dict, skipped\n", req->id, vs_data( req->url), key));
			kv_add_val_extended(req->options, (const char *)vs_data(vkey), (const char *)(replaced == NULL? vs_data(vvalue): vs_data(replaced)), __func__, __LINE__, 1 /* ignore */);
		}
	}
	else {
		/* __header_tags 에 정의 되어 있는 헤더인 경우 */
		TRACE((T_DAEMON, "[%llu] URL[%s]: header '%s': '%s' checking\n", req->id, vs_data(req->url), vs_data(vkey), vs_data(vvalue)));
		if (res->proc) {
			/* 핸들러 함수가 정의된 경우 */
			r = (*res->proc)(vkey, vvalue, res->flag, req, &replaced, ignore);
			if (r == NCE_TAG_FAILURE_STOP) {
				/* inproper tag value */
				scx_error_log(req, "[%llu] URL[%s]: header tag '%s' has a value of '%s', invalid\n", req->id, vs_data(req->url), key, value);
			}
		}
		if ( (!HEADER_IS(res, HEADER_SKIP) || (r == NCE_TAG_ACCEPT_CONTINUE))
				&& (ignore == 0) ) {
			/* 오리진에 헤더 전달 */
			kv_add_val_d(req->options, (const char *)vs_data(vkey), (const char *)(replaced == NULL? vs_data(vvalue): vs_data(replaced)), __func__, __LINE__);
			TRACE((T_DAEMON, "[%llu] URL[%s]: header tag '%s' find in tag dict, added\n", req->id, vs_data( req->url), key));
		}
		else {
			/* req->options의 key list 저장은 되지만 netcache Core에서 origin 요청 하는 헤더에는 사용되지 않는다. */
			kv_add_val_extended(req->options, (const char *)vs_data(vkey), (const char *)(replaced == NULL? vs_data(vvalue): vs_data(replaced)), __func__, __LINE__, 1 /* ignore */);
			TRACE((T_DAEMON, "[%llu] URL[%s]: header tag '%s' find in tag dict, skipped\n", req->id, vs_data( req->url), key));
		}
	}

    return MHD_YES;
}

/*
 * nc_read_apc() 에서 비동기로 동작중
 * 작업이 완료되면 callback 되는 함수
 */
void
scx_nc_read_complete(nc_file_handle_t *file, int xfered, int error, void *cbdata)
{
	nc_request_t 	*req = (nc_request_t *)cbdata;
	req->te = sx_get_time();
	if (error == 0) {
		req->async_ctx.xfered = xfered;
	}
	else {
		scx_error_log(req, "scx_nc_read_complete() error(%d, %s)\n", error, strerror(error));
		TRACE((T_DAEMON, "[%llu] read error(%d, %s)\n", req->id, error, strerror(error)));
		req->async_ctx.xfered = -1;
	}
	TRACE((T_DEBUG, "[%llu] read complete callback\n", req->id));
	scx_wait_job_completed(&req->async_ctx);
}

static ssize_t
object_reader(void *cr, uint64_t pos, char *buf, size_t max)
{
	nc_request_t 	*req = cr;
	ssize_t 			r = 0;

	size_t 				toread = 0;

	//TRACE((T_DAEMON, "URL[%s]/rangeable[%d] - cursor[%lld], try_read=%ld\n", req->url, req->objstat.st_rangeable, (long long)req->cursor, max));
	TRACE((T_DEBUG, "[%llu] object_reader URL[%s]/rangeable[%d] st_sizedecled[%d] st_sizeknown[%d] st_size = %lld , cursor[%lld], try_read=%ld, remained=%ld\n",
			req->id, vs_data(req->url), req->objstat.st_rangeable, req->objstat.st_sizedecled, req->objstat.st_sizeknown, req->objstat.st_size,
			(long long)req->cursor, max, req->remained ));
	if(max == 0) {
		return 0;
	}
	if (!gscx__service_available) {	/* shutdown 과정에서 들어오는 요청의 경우 파일 읽기가 끝난걸로 처리한다. */
		TRACE((T_DAEMON, "[%llu] object_reader request rejected. (reason : shutdown progress)\n", req->id));
		return MHD_CONTENT_READER_END_OF_STREAM;
	}

	if (req->step <= STEP_PRE_READ) {
		req->copied = 0;
		if (req->objstat.st_sizedecled) {
			/*
			 * random 읽기가 허용되는 객체에 대한 읽기임
			 * 객체의 읽기 요청이 어떤 범위에 대한 것인지 정보가 있다면 (range)
			 * 해당 정보를 참고하여 전달해야함
			 */
			if (req->remained > 0) {
				toread = min(max, req->remained);
			}
		}
		else {
			/*
			 * 해당 객체는 subrange 읽기가 제공되지 않음
			 * 객체의 offset=0~end까지 모두 읽어서 전달해야함
			 */
			toread = max;
		}
		if (toread > 0) {
			if (req->scx_res.body) {
				memcpy(buf, req->scx_res.body+req->cursor, toread);
				req->copied = toread;
			}

			else {
				ASSERT(nc_check_memory(req->file, 0));
				req->ts = sx_get_time();

				if (req->is_suspeneded == 1 && gscx__config->enable_async_ncapi == 1){
					req->copied =  nc_read_apc(req->file, buf, (nc_off_t )req->cursor, (nc_size_t)toread, scx_nc_read_complete, (void *)req);
					TRACE((T_DEBUG, "[%llu] nc_read_apc() errno = %d\n", req->id, nc_errno()));
					if (nc_errno() != EWOULDBLOCK) {
					    /* 기존대로 진행 */
						req->te = sx_get_time();
						TRACE((T_DEBUG, "[%llu] nc_read_apc() complete\n", req->id));
					}
					else {
					    /*
					     * 비동기 open 진행 중
					     * 더이상 진행 하지 않고 바로 리턴한다.
					     * 이후 scx_nc_open_completed()가 callback되면 이후 과정부터 진행하면 된다.
					     */
						req->step = STEP_DOING_READ;
						TRACE((T_DEBUG, "[%llu] nc_read_apc() return EWOULDBLOCK.\n", req->id));
						return 0;
					}
				}
				else {
					req->copied =  nc_read(req->file, buf, (nc_off_t )req->cursor, (nc_size_t)toread);
					req->te = sx_get_time();

				}
			}
		}
		req->step = STEP_POST_READ;
	}
	if (req->step == STEP_POST_READ) {
		req->t_nc_read += (req->te - req->ts);
		if (req->copied < toread && nc_errno() == ETIMEDOUT){
			/* nc_read()에서 timeout이 걸린 경우 오리진이 서비스하기에 비정상적으로 느린것으로 판단해서 Client의 연결을 끊는다. */
			r = MHD_CONTENT_READER_END_OF_STREAM;
			TRACE((T_INFO, "[%llu] Read timeout, offset = %lld, toread = %lld\n", req->id, req->cursor, toread));
			scx_error_log(req, "object_reader() read timeout, URL[%s], offset = %lld, toread = %lld\n", vs_data(req->url), req->cursor, toread);
		}
		else if (req->copied <= 0) {
			TRACE((T_DEBUG, "[%llu] object_reader URL[%s], read = %d, toread = %d, error = %d(%s)\n",
												req->id, vs_data(req->url), req->copied, toread, nc_errno(), strerror(nc_errno())));
			r = MHD_CONTENT_READER_END_OF_STREAM;
		}
		else {
			TRACE((T_DEBUG, "[%llu] object_reader URL[%s], read = %d, toread = %d\n", req->id, vs_data(req->url), req->copied, toread));
			r = req->copied;
#if 0
			/* 캐시가 깨지는 현상 감지용 */
			int margin = 0;
			if (req->copied > 32 && req->streaming == NULL) {
				margin = 16 - (req->cursor % 16);
				if (*(buf+margin) != '0' || *(buf+margin+1) != '1' || *(buf+margin+2) != '2' || *(buf+margin+15) != 'F') {
					scx_error_log(req, "Cache file corrupted, url(%s), offset = %lld, margin = %d, char = %c\n", vs_data(req->url), req->cursor, margin, *(buf+margin));
				}
			}
#endif
		}


		if (req->copied > 0) {
			req->cursor 	+= req->copied;
			if (req->remained > 0)
				req->remained 	-= req->copied;

			if (req->objstat.st_sizedecled && req->remained == 0) {

			}

		}
		if (r != MHD_CONTENT_READER_END_OF_STREAM) {
			scx_update_res_body_size(req, r);
			//req->res_body_size += r;
		}
	}
	req->step = STEP_PRE_READ;
	//TRACE((T_INFO, "RES_BODY = %lld\n", req->res_body_size));

	return r;
}


static int
nce_encode_url(char *to, int bufl, char *from)
{
	static char reserved[]=":/?#[]@!$&'()*+,;="; /* rfc3986 */
	static char unreserved_xtra[]="-._~"; /* rfc3986 */
	int 	tol = 0;

	for (tol = 0; *from != '\0' && tol + 4 < bufl; from++) {
		if ( isalnum(*from) ||
			(strchr(reserved, *from) != NULL) ||
			(strchr(unreserved_xtra, *from))) {
			*to = *from; ++to;++tol;
		}
		else {
			sprintf(to, "%%%02x", (int)*from&0xFF);
			to +=3; tol +=3;
		}
		*to = 0;
	}
	return tol;
}

/*
 * referer 차단 기능
 * 차단 해야 하는 경우 -1을 리턴
 * referers_allow와 referers_deny는 동시에 사용되지 않으며
 * 같이 설정되는 경우 referers_allow만 적용된다.
 * referer_not_exist_allow는 referers_allow나 referers_deny가 설정 된 경우만 유효하다.
 */
static int
nc_check_referer(nc_request_t *req)
{

	service_info_t *service = req->service;
	const char 	*referer = NULL;
	int count = 0, i;
	char *token = NULL;
	char *match = NULL;
	if (service->referers_allow != NULL || service->referers_deny != NULL) {
		referer = MHD_lookup_connection_value(req->connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_REFERER);
		if (referer == NULL) {
			if (service->referer_not_exist_allow == 0 ) {
				scx_error_log(req, "URL[%s] %s header required.\n", vs_data(req->url), MHD_HTTP_HEADER_REFERER);
				return -1;
			}
			return 0;
		}
		if (service->referers_allow != NULL) {
			count = scx_list_get_size(service->referers_allow);
			for(i = 0; i < count; i++)	{
				token = (char *)scx_list_get_by_index_key(service->referers_allow, i);
				match = strcasestr(referer, token);
				TRACE((T_DEBUG, "[%llu] referers_allow, referer(%s), token(%s), match(%s)\n", req->id, referer, token, match?match:"NULL"));
				if (match != NULL) {
					return 0;
				}
			}
			scx_error_log(req, "URL[%s] %s header(%s) not allowed.\n", vs_data(req->url), MHD_HTTP_HEADER_REFERER, referer);
			return -1;
		}
		else if(service->referers_deny != NULL) {
			count = scx_list_get_size(service->referers_deny);
			for(i = 0; i < count; i++)	{
				token = (char *)scx_list_get_by_index_key(service->referers_deny, i);
				match = strcasestr(referer, token);
				TRACE((T_DEBUG, "[%llu] referers_deny, referer(%s), token(%s), match(%s)\n", req->id, referer, token, match?match:"NULL"));
				if (match != NULL) {
					scx_error_log(req, "URL[%s] %s header(%s) not allowed.\n", vs_data(req->url), MHD_HTTP_HEADER_REFERER, referer);
					return -1;
				}
			}
			return 0;
		}
	}

	return 0;
}

static int
nc_parse_header(nc_request_t *req)
{
	int 	result;
	struct MHD_Connection *connection = req->connection;

	result = MHD_get_connection_values(connection, MHD_HEADER_KIND, scx_interpret_request_header, req);
	if (result > 0) {
		if(req->p1_error) {
			/* scx_interpret_request_header()에서 이미 에러를 정의한 경우에는 별도의 작업이 필요 없음 */
		}
		else if (req->host == NULL) {
			/* 요청에서 host정보가 빠져있음 */
			scx_error_log(req, "URL[%s] - Host not found in header\n", vs_data(req->url));
			req->p1_error = MHD_HTTP_BAD_REQUEST;
		}
	}
	else {
		/* header parsing 중 에러 발생 */
		if(req->p1_error) {
			/* scx_interpret_request_header()에서 이미 에러를 정의한 경우에는 별도의 작업이 필요 없음 */
		}
		else {
			req->p1_error = MHD_HTTP_BAD_REQUEST;
			scx_error_log(req, "URL[%s] - seems no header tags\n", vs_data(req->url));
		}
	}
	/*
	 * 여기서 referer 차단 기능을 처리한다.
	 */
	if (nc_check_referer(req) < 0) {
		req->p1_error = MHD_HTTP_FORBIDDEN;
	}

#if 0
	hdr_req_xff() 함수에서 처리
	/*
	 * header tag의 추가작업 요청
	 */
	//scx_verify_and_adjust_headers(cr);
#endif


	if (mp_consumed(req->pool) >1024*32) {
		/* 
		 * HTTP header의 크기가 너무 큼
		 */
		req->p1_error = MHD_HTTP_REQUEST_ENTITY_TOO_LARGE;
		scx_error_log(req, "URL[%s] - too large request(allocated=%ld)\n", vs_data(req->url), mp_consumed(req->pool));
	}
	return 0;

}

static vstring_t *
scx_update_via(nc_request_t *req, vstring_t *buf, char *v)
{
	int 	n = 0;
		//n = vsnprintf(vs_data(buf), vs_remained(buf),  "%s %s %s/%s", req->version?vs_data(req->version):MHD_HTTP_VERSION_1_1, vs_data(req->config->host_name), PROG_SHORT_NAME, PROG_VERSION);
	n = scx_snprintf(vs_data(buf), vs_remained(buf),  "%s/%s",  PROG_SHORT_NAME, PROG_VERSION);

	vs_update_length(buf, n);
	if (v) {
		size_t m;
		m = scx_snprintf(vs_offset(buf,n), vs_remained(buf), ", %s",  v);
		vs_update_length(buf, n+m);
	}

	return buf;
}
/*
 * kv_for_each 함수를 통해서 callback된 함수
 * 원본 또는 캐시 객체의 property를
 * 응답 메시지(헤더)에 추가
 */
static int
nce_inject_property(const char *key, const char *value, void *cb)
{
	int  r;
	char 			rewbuf[256];
	char 			*old = (char *)value;
	char 			*prew = rewbuf;
	int 			n, remained = sizeof(rewbuf);
	nc_request_t   *req = (nc_request_t *)cb;
	char 			*p;

	ASSERT(req->response != NULL);
	ASSERT(key != NULL);
	ASSERT(value != NULL);
	/* 
	 * 어쩌면 여기서 __header_tag의 SKIP여부를 보고,
	 * 추가 여부를 결정해야할지 모름
	 */
	if (strncasecmp(key, "via", 3) == 0) {
			vstring_t 	*via_str = NULL;
			via_str = vs_allocate(req->pool, 256, NULL, 0);
			req->respvia = 1;
			scx_update_via(req, via_str, (char *)value);
			r = MHD_add_response_header(req->response, key, vs_data(via_str));
			//req->res_hdr_size += strlen(key) + vs_length(via_str); //response header size 계산 방식 변경으로 이 부분은 더이상 사용하지 않는다.

	}
	else if (strncasecmp(key, "X-Cache", 7) == 0) {
		/* parent가 solproxy인 경우 X-Cache 헤더가 들어 올수 있는데 이 경우 origin(parent)에서 받은 헤더는 무시한다 */
	}
	else if (strncasecmp(key, "X-Edge-Request-ID", 17) == 0) {
		/* X-Cache의 경우와 동일한 이유임 */
	}
#if 0	/* 원본 offline시 캐싱된 컨텐츠의 st_vtime이 갱신 되지 않아서 max-age가 음수가 되는 문제가 발생함 */
	else if (strncasecmp(key, "cache-control", 13) == 0) {
		/* max-age는 현재 시간 기준으로 다시 계산해야함 */
		if ((p = strcasestr(old, "max-age")) != NULL && req->objstat.st_vtime > 0) {
			/* 
			 * max-age 항목이 포함되어 있음 
			 */

			/* max-age의 'm'위치 전까지 copy */
			while (old < p) {*prew++ = *old++;remained--;}
			memcpy(prew, p, 8); /* 'max-age='  복사 */
			old += 8; remained -= 8; prew += 8;

			nc_time_t 	v = req->objstat.st_vtime - time(NULL);
			n = scx_snprintf(prew, remained, "%ld", (long)v);
			prew += n; remained -= n;
			while (isdigit(*old)) old++; /* max-age 값 skip */
			strcpy(prew, old);
			r = MHD_add_response_header(req->response, key, rewbuf);
			TRACE(((req->verbosehdr?T_INFO:T_DAEMON), 	"[%llu] nce_inject_property for URL[%s]: '%s: %s' "
														"injected instead of '%s' (result=%d)(vtime=%lld, now=%lld)\n",
														req->id, vs_data(req->url),
														key,
														rewbuf,
														value,
														r,
														(long long)req->objstat.st_vtime, (long long)time(NULL)));
			//req->res_hdr_size += strlen(key) + strlen(rewbuf); //response header size 계산 방식 변경으로 이 부분은 더이상 사용하지 않는다.
		}
		else {
			/* max-age  0 일 것임 */
			r = MHD_add_response_header(req->response, key, value);
			TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "[%llu] nce_inject_property for URL[%s]: "
													  "'%s: %s' injected(nocache=%d)(result=%d)\n",
													  req->id, vs_data(req->url),
													  key,
													  value,
													  req->objstat.st_nocache,
													  r));
			//req->res_hdr_size += strlen(key) + strlen(value);	//response header size 계산 방식 변경으로 이 부분은 더이상 사용하지 않는다.
		}
	}
#endif
	else  {
		r = MHD_add_response_header(req->response, key, value);
		//req->res_hdr_size += strlen(key) + strlen(value);	//response header size 계산 방식 변경으로 이 부분은 더이상 사용하지 않는다.
		TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "[%llu] nce_inject_property for URL[%s]: '%s: %s' injected(result=%d)\n",
				req->id, vs_data(req->url), key, value, r));
	}
	return 0;
}

/*
 * kv_for_each 함수를 통해서 callback된 함수
 * 응답 헤더를 검사해서 없는 경우 추가 하고
 * 있는 경우에 replace 한다.
 */
int
scx_update_resp_header(const char *key, const char *value, void *cb)
{

	nc_request_t   *req = (nc_request_t *)cb;

	ASSERT(req->response != NULL);
	ASSERT(key != NULL);
	ASSERT(value != NULL);
	const char * val = MHD_get_response_header(req->response, key);

	if(val != NULL){
		//동일한 header가 중복으로 있는 경우 이전 header를 삭제 한후에 새로운 헤더를 넣는다.
		MHD_del_response_header(req->response, key, val);
	}

	MHD_add_response_header(req->response, key, value);

	return 0;
}

/*
 * worker thread에서 호출 되는 function
 */
void
scx_run_async_job(void * pctx)
{
	scx_async_ctx_t * async_ctx = NULL;
	nc_request_t *req = NULL;
	int res = MHD_YES;
	if (pctx == NULL)  {
		TRACE((T_ERROR, "Unexpected async_job. %s:%d\n", __FILE__, __LINE__));
		scx_dump_stack();
		scx_marking_tid_to_shm();
		return;
	}
	async_ctx = (scx_async_ctx_t *)pctx;
	req = (nc_request_t *)async_ctx->cr;
	if (req == NULL || req->pool == NULL) {
		/* 실제 여기로 들어올 가능성은 없어 보이고 문제가 생기는 경우 위의  첫 pctx를 넘겨 받는 부분이 문제가 발생할 가능성이 큼 */
		TRACE((T_ERROR, "Unexpected async_job. %s:%d\n", __FILE__, __LINE__));
		scx_dump_stack();
		scx_marking_tid_to_shm();
		return;
	}
	TRACE((T_DAEMON, "[%llu] scx_run_async_job phase[%d]\n", req->id, async_ctx->phase));
	if (SRP_READ_DONE >= async_ctx->phase) {
		if (0 == async_ctx->job_avail) {
			TRACE((T_DAEMON, "[%llu] scx_run_async_job canceled. phase[%d]\n", req->id, async_ctx->phase));
			goto ASYNC_JOB_END;

		}
	}
	if (!gscx__service_available) {	/* shutdown 과정에서는 큐에 들어 있는 모든 job을 바로 리턴한다. */
		TRACE((T_DAEMON, "[%llu] scx_run_async_job request rejected. (reason : shutdown progress)\n", req->id));
		if (async_ctx->phase == SRP_CLOSE || async_ctx->phase == SRP_DELAYED_CLOSE) {
			/*  complete callback이 호출 되었을때에는 데몬이 종료 중이더라도 nc_close()를 정상적으로 호출 해주어야 nc_shutdown()이 정상 동작 한다. */
			nc_request_completed((void *)req->connect_type, req->connection, (void *) &req, async_ctx->toe);
		}
		goto ASYNC_JOB_END;
	}
	async_ctx->is_working = 1;
	switch (async_ctx->phase) {	/* GET,HEAD 일때 SRP_INIT과 SRP_REQUEST 과정을 한번에 실행 할수 있으면 성능 향상에 도움이 될것 같음 */
	case SRP_INIT:
		res = (*__method_map[req->method].method_proc)(NULL, 0, req);
		async_ctx->phase = SRP_REQUEST;
		ASSERT(req->is_suspeneded == 1);	/* 비정상 상황 디버깅용 */
		if (req->step == STEP_DOING_OPEN) {
			/* nc_open이 비동기로 진행중인 경우는 resume을 하지 않고 그냥 리턴한다.*/
			async_ctx->is_working = 0;
			return;
		}

		break;
	case SRP_REQUEST:
		res = (*__method_map[req->method].method_proc)(NULL, 0, req);
		/* post나 put의 경우는 여기가 여러번 호출 되기 때문에 phase를  SRP_READ_PRE로 명시적으로 변경하지 않는다. */
		/************************** Client Response Phase Handler가 실행 되는 부분 *************************************/
		ASSERT(req->is_suspeneded == 1);	/* 비정상 상황 디버깅용 */
		if (req->step == STEP_DOING_OPEN) {
			/* nc_open이 비동기로 진행중인 경우는 resume을 하지 않고 그냥 리턴한다.*/
			async_ctx->is_working = 0;
			return;
		}
		break;
	case SRP_WAIT_JOB:
		scx_wait_job_completed(async_ctx);
		return;
	case SRP_READ_DOING:
		ASSERT(req->stay_suspend == 0);
		res = scx_sync_reader(async_ctx->cr, async_ctx->pos, async_ctx->buf, async_ctx->max, async_ctx->reader_type);
		if (req->step == STEP_DOING_READ) {
			/* nc_read_apc가 비동기로 진행중인 경우는 resume을 하지 않고 그냥 리턴한다.*/
			async_ctx->is_working = 0;
			return;
		}
		async_ctx->phase = SRP_READ_DONE;
		/* cmaf 의 경우에 특정한값이 marking되어 있으면 아래 suspend 상태를 유지하도록 해야 한다. */
		if(req->stay_suspend) {
			req->stay_suspend = 0;
			strm_cmaf_add_supend_list(req);
			async_ctx->is_working = 0;
			return;
		}

		ASSERT(req->is_suspeneded == 1);	/* 비정상 상황 디버깅용 */
		break;
	case SRP_CLOSE:
		nc_request_completed((void *)req->connect_type, req->connection, (void *) &req, async_ctx->toe);
		/*
		 * nc_request_completed() 내에서 memory 해제가 끝났기 때문에 더이상 req와, async_ctx는 존재 하지 않는다.
		 * 그래서 여기서 바로 리턴을 한다.
		 */
		return ;
	case SRP_DELAYED_CLOSE:
		TRACE((T_ERROR, "[%llu] Unexpected Connection close(%d).\n", req->id, async_ctx->phase));
		/* 이곳은 주로 비동기 job이 실행중에 client의 연결이 갑자기 끊어 져서
		 * complete callback이 호출되는 경우에 들어오는 부분임. */
		if (req->is_suspeneded) {
			/* 비동기 처리 job이 아직 끝나지 않은 상태
			 * job이 끝날때까지 대기하기 위해 job queue에 다시 등록한다. */
			/* 현재 여기서 바로 종료 되는 문제가 보이는데 디버깅 필요함 */
			thpool_add_work(gscx__thpool, (void *)scx_run_async_job, (void *)async_ctx);
		}
		else {
			nc_request_completed((void *)req->connect_type, req->connection, (void *) &req, async_ctx->toe);
		}
		return ;
	default :
		TRACE((T_ERROR, "[%llu] Undefined request phase(%d).\n", req->id, async_ctx->phase));
		ASSERT(0);
	}
//	printf("done %s(), connection = %llX\n", __func__, req->connection);
//	bt_msleep(1);
	async_ctx->res = res;
ASYNC_JOB_END:
	async_ctx->is_working = 0;
	if (req->is_suspeneded) {	/* SRP_CLOSE의 경우는 suspend 상태가 아니다 */
		/* scx_async_reader()에서 이 값을 기준으로 동작 하는 부분이 있어서 항상 resume 바로 전에 호출 되어야 함 */
		req->is_suspeneded = 0;
		TRACE((T_DAEMON, "[%llu] scx_run_async_job is_suspeneded[%d], connection is %s\n",
				req->id, req->is_suspeneded,
				req->connection?"not NULL":"NULL"));
		/* 비정상 complete callback 호출인 경우는 req->connection가 NULL이 들어 올수도 있다. */
		if (NULL != req->connection) {
			TRACE((T_DAEMON, "[%llu] MHD_resume_connection. phase[%d]\n", req->id, async_ctx->phase));
			MHD_resume_connection(req->connection);	/* 동기화 문제 때문에 resume는 제일 마지막에 호출되어야 한다. */
		}
		else {
			/* connection이 NULL인 경우는 complete callback이 호출 되어서 메모리가 해제된 경우로 추정됨 */
			TRACE((T_ERROR, "Unexpected ASYNC_JOB_END. %s:%d\n",  __FILE__, __LINE__));
			scx_dump_stack();
			scx_marking_tid_to_shm();
		}
	}
	return ;
}

/*
 * 비동기 method에 대한 처리를 하는 함수
 *
 */
static int
scx_handle_async_method(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int 	res = MHD_YES;
	int		enable_async_accesshandler_callback = 1;

	if(req->service != NULL) {
		enable_async_accesshandler_callback = req->service->enable_async_accesshandler_callback;
	}
	if (1 == __method_map[req->method].async
			&& SRP_REQUEST != req->async_ctx.phase
			&& 1 == thpool_available_jobqueue(gscx__thpool)
			&& 1 == enable_async_accesshandler_callback) { /* 비동기 기능을 사용하는 상태이더라도 jobqueue가 많이 차 있는 상태면 동기 방식으로 동작한다. */
		/* 비동기 처리용 method는 별도의 로직을 사용한다. */
		req->is_suspeneded = 1;	/* 비동기 flag 셋팅 */
		TRACE((T_DAEMON, "[%llu] MHD_suspend_connection. phase[%d]\n", req->id, req->async_ctx.phase));
		/* 여기에서 suspend를 건다 */
		MHD_suspend_connection(req->connection);
		/*
		 * is_working과 job_avail의 경우 MHD 라이브러리에서 suspend 상태일때에도 complete callback이 호출되는 경우가 있어서
		 * 이를 회피하기 위한 flag이므로 MHD에서 이 문제가 해결되면 이 두가지 flag는 삭제해도 된다.
		 */
		req->async_ctx.is_working = 0;
		req->async_ctx.job_avail = 1;
		thpool_add_work(gscx__thpool, (void *)scx_run_async_job, (void *)&req->async_ctx);
		res = MHD_YES;

	}
	else {
		res = (*__method_map[req->method].method_proc)(upload_data, upload_data_size, req);
		if (SRP_INIT == req->async_ctx.phase) {
			req->async_ctx.phase = SRP_REQUEST;
		}
	}

	return res;
}

static ssize_t
scx_async_object_reader(void *cr, uint64_t pos, char *buf, size_t max)
{
	ssize_t ret = 0;
	ret = scx_async_reader(cr, pos, buf, max, 0);
	return ret;
}

static ssize_t
scx_async_streaming_reader(void *cr, uint64_t pos, char *buf, size_t max)
{
	ssize_t ret = 0;
	ret = scx_async_reader(cr, pos, buf, max, 1);
	return ret;
}

/*
 * 주의 사항 : reader callback의 경우 suspend를 하고 리턴을 하더라도
 * 다시 호출 되는 경우가 간혹 있고 return값이 0이 아닌 상태에서는 suspend를 하더라도 무조건 한번씩 더 호출된다.
 * 이런 문제 때문데 suspend 상태에서 connection이 끊어 지는 경우 complete callback 이 호출 될수가 있다.
 * 그래서 suspend시에는 리턴을 항상 0으로 한다.
 */
static ssize_t
scx_async_reader(void *cr, uint64_t pos, char *buf, size_t max, int reader_type)
{
	ssize_t ret = 0;
	nc_request_t 	*req = (nc_request_t *)cr;


	if (1 == gscx__enable_async && req->service->enable_async_reader_callback == 1) {
		TRACE((T_DAEMON, "[%llu] scx_async_reader is_suspeneded[%d], phase[%d]\n", req->id, req->is_suspeneded, req->async_ctx.phase));
		/* 비동기 처리인 경우 */
		if (req->is_suspeneded == 0)  {	/* suspend를 호출 해도 이전 바로 안멈추고 한번 더 호출이 되는 경우가 있다. */
			if (SRP_READ_DONE == req->async_ctx.phase) {	/* worker thread의 job이 끝난 경우 */
				/* suspend 당시 요청 형태와 resume후 요청 형태가 다른 경우가 있으면 안된다. */
				ASSERT(pos == req->async_ctx.pos);
				ASSERT(buf == req->async_ctx.buf);	/* MHD에서 호출 되는 buf가 동일 하다는 전제로 코딩됨 */
				ret = req->async_ctx.res;
				req->async_ctx.phase = SRP_READ_PRE;
			}
			else {
				ASSERT(max > 0);
				ASSERT(req->async_ctx.phase != SRP_READ_DOING);
				max = limitrate_control(cr, max, reader_type);
				if (0 == max)  return 0; 	/* limitrate_control()에서 suspend된 경우 0으로 리턴을 해야지 다시 호출 되지 않는다 */

				if (0 == thpool_available_jobqueue(gscx__thpool)) {
					/*
					 * 비동기 기능을 사용하는 상태이더라도 jobqueue가 많이 차 있는 상태면 동기 방식으로 동작한다.
					 * scx_async_reader() 처음 부분에서 이 검사를 하지 않고 여기서 하는 이유는 suspend 상태에서 호출되는 경우에 예외처리를 위해서임
					 */
					req->async_ctx.phase = SRP_READ_PRE; /* 이렇게 해주어야 다시 콜백 되었을때 문제가 발생하지 않음 */
					ret = scx_sync_reader(cr, pos, buf, max, reader_type);
					return ret;
				}

				req->async_ctx.pos = pos;
				req->async_ctx.buf = buf;
				req->async_ctx.max = max;

				req->is_suspeneded = 1;	/* 비동기 flag 셋팅 */

				/* 여기에서 suspend를 건다 */
				MHD_suspend_connection(req->connection);
				req->async_ctx.reader_type = reader_type;
				req->async_ctx.res = 0;
				req->async_ctx.phase = SRP_READ_DOING;
				/*
				 * is_working과 job_avail의 경우 MHD 라이브러리에서 suspend 상태일때에도 complete callback이 호출되는 경우가 있어서
				 * 이를 회피하기 위한 flag이므로 MHD에서 이 문제가 해결되면 이 두가지 flag는 삭제해도 된다.
				 */
				req->async_ctx.is_working = 0;
				req->async_ctx.job_avail = 1;
//				printf("add job, connection = %llX\n",req->connection);
				thpool_add_work(gscx__thpool, (void *)scx_run_async_job, (void *)&req->async_ctx);
				ret = 0;
			}
		}
		else {
			/* suspend 상태에서 불필요하게 호출 되는 경우 이쪽으로 빠진다. */
			int *mhd_ret = (int *)MHD_get_connection_info(req->connection, MHD_CONNECTION_INFO_CONNECTION_SUSPENDED);
			TRACE((T_INFO, "[%llu] redundant call %s() is_suspeneded[%d], phase[%d], ret = %d\n", req->id, __func__, req->is_suspeneded, req->async_ctx.phase, *mhd_ret));
		}
	}
	else {
		max = limitrate_control(cr, max, reader_type);
		if (0 == max)  return 0; 	/* limitrate_control()에서 suspend된 경우 0으로 리턴을 해야지 다시 호출 되지 않는다 */
		/* 동기 방식 처리인 경우 */
		ret = scx_sync_reader(cr, pos, buf, max, reader_type);
	}
	return ret;
}

static int
scx_sync_reader(void *cr, uint64_t pos, char *buf, size_t max, int reader_type)
{
	nc_request_t 	*req = (nc_request_t *)cr;
	int ret;

	if (0 == reader_type) {
		ret = object_reader(cr, pos, buf, max);
	}
	else if (1 == reader_type) {
		ret = streaming_reader(cr, pos, buf, max);
	}
	else {
		TRACE((T_ERROR, "[%llu]  %s(), Undefined reader type(%d) called.\n", req->id, __func__, reader_type));
		/* 혹시 새로운 reader를 만들고 처리를 안해줬을 경우를 대비 scx_run_async_job()에도 동일 하게 추가가 필요*/
		ASSERT(0);
	}
	return ret;
}

static  void
scx_async_request_completed(void *cls,
					struct MHD_Connection *connection,
					void **ctx,
					enum MHD_RequestTerminationCode toe)
{
	nc_request_t 	*req = *ctx;
	int count = 0;
	scx_async_ctx_t	*async_ctx = &req->async_ctx;
	int	enable_async_complete_callback = 1;

	if (req->service != NULL) {
		/* service가 NULL로 들어 오는 경우가 있어서 이렇게 처리 한다. */
		enable_async_complete_callback = req->service->enable_async_complete_callback;
		if(req->service->keepalive_timeout)
			MHD_set_connection_option(connection, MHD_CONNECTION_OPTION_TIMEOUT, req->service->keepalive_timeout);
	}
	if (1 == gscx__enable_async && enable_async_complete_callback == 1) {
		/* 비동기 처리인 경우 */
		/* Complete Callback은 다른 callback과 다르게 suspend를 호출하지 않고 진행한다. */
		TRACE((T_DAEMON, "[%llu] scx_async_request_completed is_suspeneded[%d]\n", req->id, req->is_suspeneded));

		async_ctx->toe = (int)toe;
		req->connection = NULL; /* 예상치 못한 문제가 발생 할수 있어서 NULL로 변경한다.  (complete callback은 connection을 가지고 작업하지 않는 가정으로 만들어짐)*/


		/* 여기에 suspend 상태로 들어 올수 있는 가능성은 request handler가 호출된 상태에서 client가 연결을 끊는 경우이다.
		 * 이때에는 다른 thread에서 job이 완료되어 resume가 호출될때까지 기다려야 한다.
		 * 그렇지 않고 여기서 그냥 진행 하게 되면 req context의 메모리가 해제된 상태에서
		 * job thread의 작업이 진행되어 memory 관련 문제가 발생할수 있음.
		 */

		if (0 != req->is_suspeneded)  {
			async_ctx->job_avail = 0;
			/* 일단 is_working은 안써도 될것 같다 */
			if (1 == async_ctx->is_working) { /* job thread에서 처리가 들어간 상태 이므로 대기 할수 밖에 없다. */
				/*
				 * 가끔씩 request handler 호출 직후 바로 client의 연결이 끊어지면
				 * suspend 상태에서 complete callback이 호출 되는 경우가 있다.
				 * 이런 경우에는 thread pool의 job이 처리 완료 될때까지 기다리는수 밖에 없다.
				 * 또 여기서 가끔식 thread pool의 job 중에 작업시간이 길어 져서 이곳의 timeout을 초과하는 경우도 발생함
				 */
#if 0
				while (count++ < 1000  && 0 != req->is_suspeneded) {
					TRACE((T_WARN, "[%llu] unusual connection suspending [%d], count[%d]\n", req->id, req->is_suspeneded, count));
					bt_msleep(10);
				}
				ASSERT(req->is_suspeneded == 0);
#endif
			}
			/* req->async_ctx의 phase를 설정할 경우 비동기로 동작 중인 다른 thread에서 동일 async_ctx의
			 * phase를 변경할 수가 있어서 별도의 context를 생성해서 설정을 하고
			 * job 등록도 새로 만들어진 context를 넘겨준다.
			 */
			async_ctx = (scx_async_ctx_t *)mp_alloc(req->pool,sizeof(scx_async_ctx_t));
			memcpy(async_ctx, &req->async_ctx, sizeof(scx_async_ctx_t));
			async_ctx->phase = SRP_DELAYED_CLOSE;
		}
		else {
			if (0 == thpool_available_jobqueue(gscx__thpool)) {
				/*
				 * 비동기 기능을 사용하는 상태이더라도 jobqueue가 많이 차 있는 상태면 동기 방식으로 동작한다.
				 * async_ctx->is_working 가 1인 경우는 이미 비동기 처리가 진행 중인 상태 이기 때문에
				 * job queue의 상태에 관계 없이 job queue에 작업을 넣는다.
				 */
				goto do_sync_request_completed;
			}
			async_ctx->phase = SRP_CLOSE;
		}
		/*
		 * is_working과 job_avail의 경우 MHD 라이브러리에서 suspend 상태일때에도 complete callback이 호출되는 경우가 있어서
		 * 이를 회피하기 위한 flag이므로 MHD에서 이 문제가 해결되면 이 두가지 flag는 삭제해도 된다.
		 */
		async_ctx->is_working = 0;
		//req->async_ctx.job_avail = 1;
		thpool_add_work(gscx__thpool, (void *)scx_run_async_job, (void *)async_ctx);
	}
	else {
do_sync_request_completed:
		nc_request_completed(cls, connection, ctx, toe);
	}
}

static int
scx_handle_head(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	return scx_handle_method(req);
}

static int
scx_handle_get(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	return scx_handle_method(req);
}

/* 테스트용 임시 코드 */
void print_mem(nc_request_t *req, void const *vp, size_t n)
{
    unsigned long long *p = vp+8;
       char buf[2048] = {0};
       int pos = 0;
    for (size_t i=0; i<64; i++)
        pos += sprintf(buf+pos, "%x, ", p[i]);
       //printf("req->options dump(%x) kv = %s\n", vp, buf);
    TRACE((T_INFO, "[%llu] req->options dump(%x) kv = %s\n", req->id, vp, buf));
};

static int
handle_method_streaming_phase(nc_request_t *req)
{
	int 					ret = 0;
	double 					ts, te;
	nc_method_t 			method = req->method;

	if (!strm_check_protocol_permition(req))  {/* permition을 검사하는 부분 */
		if (!req->p1_error) req->p1_error = MHD_HTTP_UNSUPPORTED_MEDIA_TYPE;
		goto HANDLE_METHOD_STREAMING_END;
	}
	if ( (req->streaming->media_mode & O_STRM_TYPE_DIRECT) == 0 ) {
		if ( (req->streaming->media_mode & O_STRM_TYPE_ADAPTIVE) != 0 ) {
			/* smil파일을 받아서 파싱 하는 부분 */
			if (!strm_handle_adaptive(req)) {
				scx_error_log(req, "SMIL handler error. URL(%s)\n", vs_data(req->url));
				if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				goto HANDLE_METHOD_STREAMING_END;
			}
		}

		if ((req->streaming->protocol & O_PROTOCOL_CORS) == 0) {
#ifdef ZIPPER
			if (req->service->streaming_method == 1) {
				/* live ad stiching 기능으로 설정된 경우 */
				if (req->streaming->live_path == NULL) {
					scx_error_log(req, "live origin url required.\n", vs_data(req->url));
					if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				}
				if (strm_live_session_check(req) == 0) {
					if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				}
				if (strm_live_session_repack(req) == 0) {
					if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				}
				goto HANDLE_METHOD_STREAMING_END;
			}
#endif
			if (req->service->streaming_method == 2) {
				/* CMAF live 기능으로 설정된 경우 */
				if (strm_cmaf_build_response(req) == 0) {
					if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				}
				goto HANDLE_METHOD_STREAMING_END;
			}
			if (req->streaming->builder == NULL) {
				if (strm_create_builder(req, 0) == 0) {
#ifndef ZIPPER
					if (MHD_HTTP_NOT_FOUND == req->p1_error) {
						/*
						 * 여기에 들어오는 경우는 URL이 잘못 생성되었거나
						 * 고객의 Origin 파일 url이 Streaming시 사용하는 가상 파일의 URL과 같을 경우이다.
						 * 예) 고객의 origin 파일 경로가 /path/mbc.mp4/content.mp4 인 경우 이를 solproxy에 요청하면
						 * solproxy는 content.mp4를 가상 파일이라고 판단해서 origin에 /path/mbc.mp4로 요청을 하게 되는데
						 * 이렇게 되면 의도대로 동작하지 않는 경우가 발생한다.
						 * 이런 경우를 방지하기 위해 404(원본 파일 열기 실패의 경우 내부에서 p1_error에 404기록함)의 경우는
						 * streaming이 아닌 cache 방식으로 한번더 요청하도록 한다.
						 */
						if(req->streaming){
							strm_destroy_stream(req);
							req->streaming = NULL;
						}
						req->p1_error = 0;

						req->step = STEP_PRE_OPEN;
						shm_log_update_step(req);
						return 0;
					}
#endif
					if (!req->p1_error)	req->p1_error = MHD_HTTP_NOT_FOUND;
					goto HANDLE_METHOD_STREAMING_END;
				}
			}
		}

	}
	ret = strm_prepare_stream(req);
	if (!ret) {
		if (!req->p1_error)	req->p1_error = MHD_HTTP_NOT_FOUND;

	}
HANDLE_METHOD_STREAMING_END:
	req->step = STEP_VERIFY_CONDITION;
	shm_log_update_step(req);
	return 0;
}

static void
scx_wait_job_completed(scx_async_ctx_t * async_ctx)
{
	nc_request_t *req = (nc_request_t *)async_ctx->cr;
	if (req->async_ctx.is_working == 0) {
		/* nc_open_extended_apc()나 nc_read_apc()를 호출한 thread의 job이 완료된 경우 */
		req->async_ctx.job_avail = 1;
		if(req->step == STEP_DOING_OPEN) {
			req->async_ctx.phase = SRP_REQUEST;
			req->step = STEP_POST_OPEN;
			shm_log_update_step(req);
			if (req->async_ctx.file != NULL) {
				memcpy(&req->objstat, &async_ctx->objstat, sizeof(nc_stat_t));
				req->file = async_ctx->file;
			}
			else {
				req->objstat.st_origincode = async_ctx->objstat.st_origincode;
			}
		}
		else if (req->step == STEP_DOING_READ) {
			req->step = STEP_POST_READ;
			req->copied = async_ctx->xfered;
		}
		else {
			TRACE((T_ERROR, "[%llu] not expected step(%d)\n", req->id, req->step));
			scx_dump_stack();
			scx_marking_tid_to_shm();
		}
		/* job queue에 넣어서 이후 작업을 진행 할수 있도록 한다 */
		TRACE((T_DEBUG, "[%llu] add async job\n", req->id));
		thpool_add_work(gscx__thpool, (void *)scx_run_async_job, (void *)&req->async_ctx);

	}
	else {	// req->async_ctx.is_working == 1
		/*
		 * nc_open_extended_apc()를 호출한 thread의 job이 아직 완료되지 않은 경우
		 * 해당 job완료 될때까지 기다리기 위해 확인 전용 job을 추가한다.
		 */
		req->async_ctx_2th.phase = SRP_WAIT_JOB;
		req->async_ctx_2th.cr = (void *)req;
		req->async_ctx_2th.job_avail = 1;
		if(req->step < STEP_PRE_READ) {
			req->async_ctx_2th.file = req->async_ctx.file;
			memcpy(&req->async_ctx_2th.objstat, &req->async_ctx.objstat, sizeof(nc_stat_t));
		}
		else {
			req->async_ctx_2th.xfered = async_ctx->xfered;
		}
		/* https://jarvis.solbox.com/redmine/issues/33429의 이슈가 해결 될때 까지 아래의 로그 레벨을 유지한다. */
		TRACE((T_INFO, "[%llu] add async job, SRP_WAIT_JOB phase, step(%d)\n", req->id,  req->step));
//		printf("[%llu] add async job, phase(%d), step(%d)\n", req->id, req->async_ctx_2th.phase, req->step);
		bt_msleep(1);// 순간적으로 엄청나게 호출되는 현상을 방지 하기 위해 잠깐 쉰다. */
		thpool_add_work(gscx__thpool, (void *)scx_run_async_job, (void *)&req->async_ctx_2th);
	}

}
/*
 * nc_open_extended_apc() 에서 비동기로 동작(EWOULDBLOCK 응답)할때
 * 작업이 완료되면 callback 되는 함수
 * 경우에 따라 nc_open_extended_apc() 호출과 거의 동시에 이 함수가 callback 되는 경우가 있어서
 * 같은 context를 가지고 두개의 thread에서 작업을 하게 되는 문제가 발생할수 있음.
 * 이를 회피하기 위해 req->async_ctx.is_working의 상태를 기준으로
 * nc_open_extended_apc() 호출한 thread의 job이 진행중인지를 판단한다.
 * nc_open_extended_apc()가 리턴되기 전에 이 함수가 callback되는 경우
 *    req->file 을 여기서 갱신하게 되면 nc_open_extended_apc()가 리턴하면서 다시 null로 설정되는 문제가 발생해서
 *    nc_open_extended_apc() 호출한쪽이 끝날때까지 대기 했다가 req->file과 req->objstat을 갱신 하도록 한다.
 */
static void
scx_nc_open_completed(nc_file_handle_t *file, nc_stat_t *stat, int error, void *cr)
{
	nc_request_t *req = (nc_request_t *)cr;
	req->te = sx_get_time();
	TRACE((T_DEBUG, "[%llu] open complete callback\n", req->id));
	if (file != NULL) {
		memcpy(&req->async_ctx.objstat, stat, sizeof(nc_stat_t));
		req->async_ctx.file = file;

	}
	else {
		scx_error_log(req, "Service[%s]/URL[%s] - open error (%s)\n",vs_data(req->service->name), vs_data(req->ori_url), strerror(error));
		TRACE((T_DAEMON, "[%llu] open error(%d, %s)\n", req->id, error, strerror(error)));
		req->async_ctx.file = NULL;
		if (stat != NULL && stat->st_origincode > 0) {
			req->async_ctx.objstat.st_origincode = stat->st_origincode;
		}
		else {
			req->async_ctx.objstat.st_origincode = 0;
		}
	}
	scx_wait_job_completed(&req->async_ctx);

}



/*
 * 리턴값이 0이면 다음 과정 진행
 * -1이면 nc_open_extended_apc()에서EWOULDBLOCK을 응답한 상태이므로 다음 과정을 진행하지 않고 바로 리턴함.
 */
static int
handle_method_open_phase(nc_request_t *req)
{
	int 					ret = 0;
	nc_method_t 			method = req->method;

	if (req->p1_error != 0) return 0;
	/* 스트리밍 경우에는 nc_open을 여기서 호출할 필요가 없다. */
	if (req->step <= STEP_PRE_OPEN) {
		if (req->service->core->mnt == NULL) {
			/* 오리진 설정이 되지 않은 볼륨으로 요청이 들어 오는 경우는 에러 처리 한다. */
			scx_error_log(req, "%s undefined origin. url(%s)\n", __func__, vs_data(req->ori_url));
			if (!req->p1_error)	req->p1_error = MHD_HTTP_BAD_REQUEST;
			return 0;
		}
		req->mode = 0;
		if (req->property_key != NULL) {
			/*
			 * script에서 property key가 생성된 경우 stat_key를 netcache core에게 전달
			 * script엔진에서 따로 안만든 경우는 stat-cache key는 objectkey와 동일한 것으로 간주함
			 */
			kv_set_stat_key(req->options, vs_data(req->property_key));
			req->mode |= O_NCX_MUST_REVAL;
		}
		if (method == HR_HEAD || method == HR_GET) {
			req->mode |= O_RDONLY;

			if (1 == req->private) {
				req->mode |= O_NCX_PRIVATE;
			}
			if (1 == req->nocache) {
				req->mode |= O_NCX_NOCACHE;
			}
			if (1 == req->norandom ) {
				req->mode |= O_NCX_NORANDOM;
			}
			else if (2 == req->norandom ) {
				req->mode |= O_NCX_TRY_NORANDOM;
			}
			else {

			}
		}
		else {
			//custom Method는 캐싱이 되지 않도록 한다.
			req->mode = O_RDONLY|O_NCX_NOCACHE|O_NCX_NORANDOM|O_NCX_PRIVATE;
		}
		if (req->norandom == 0) {
			/*
			 * Accept-Encoding 헤더 가 들어 있는 경우 오리진으로 full GET으로 요청 되기 때문에 Range 요청 컨텐츠인 경우 Accept-Encoding 헤더를 제거 한다.
			 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/32639
			 */
			kv_remove(req->options, MHD_HTTP_HEADER_ACCEPT_ENCODING, 0);
		}
		req->ts = sx_get_time();
		if (method != HR_HEAD) {
			kv_oob_command(req->options, vs_data(req->zmethod), 0);    /* method 전달 */
		}
		kv_set_opaque(req->options, (void *)req); /*weon's addition*/
		//print_mem(req, req->options, 616);
	//				ret = nc_getattr_extended(req->service->core->mnt, vs_data(req->object_key), vs_data(req->ori_url), &req->objstat, req->options, req->mode);
		if (req->is_suspeneded == 1 && gscx__config->enable_async_ncapi == 1) {
			/* suspened 상태일때만 nc_open_extended_apc()를 사용한다. */

			req->step = STEP_DOING_OPEN; /* nc_open_extended_apc() 호출과 거의 동시에 callback()함수가 호출 될수가 있어서 먼저 값을 셋팅한다. */
			shm_log_update_step(req);
			/* netcache 비동기 API 호출 */
			req->file = nc_open_extended_apc(req->service->core->mnt, vs_data(req->object_key), vs_data(req->ori_url), req->mode, &req->objstat, req->options, scx_nc_open_completed, (void *)req);
			TRACE((T_DEBUG, "[%llu] nc_open_extended_apc() errno = %d\n", req->id, nc_errno()));
			if (nc_errno() != EWOULDBLOCK) {
			    /* 기존대로 진행 */
				req->te = sx_get_time();
				req->step = STEP_POST_OPEN;
				shm_log_update_step(req);
				TRACE((T_DEBUG, "[%llu] nc_open_extended_apc() complete\n", req->id));
			}
			else {
			    /*
			     * 비동기 open 진행 중
			     * 더이상 진행 하지 않고 바로 리턴한다.
			     * 이후 scx_nc_open_completed()가 callback되면 이후 과정부터 진행하면 된다.
			     * 이곳 까지 오기 전에 이미 scx_nc_open_completed() callback이 호출 되는 경우도 있기 때문에 이후에는 req의 값을 변경해서는 안된다.
			     */
				//req->step = STEP_DOING_OPEN;
				TRACE((T_DEBUG, "[%llu] nc_open_extended_apc() return EWOULDBLOCK.\n", req->id));
				return -1;
			}
		}
		else {
			CHECK_TIMER("nc_open_extended2", req, req->file = nc_open_extended2(req->service->core->mnt, vs_data(req->object_key), vs_data(req->ori_url), req->mode, &req->objstat, req->options))
			req->te = sx_get_time();
		}
	}
	/* 아래 부분은 비동기 테스트 용이므로 테스트 종료시 지워야함 */
	if (req->step == STEP_DOING_OPEN) {
		ASSERT(0);
		return -1;
	}

	req->t_nc_open += (req->te - req->ts);
	TRACE((T_DAEMON|T_DEBUG, "[%llu] URL[%s]: size=%llu, vtime=%lld, mtime=%lld, property=%p, code=%d\n",
			req->id, vs_data(req->ori_url),
			req->objstat.st_size,
			req->objstat.st_vtime,
			req->objstat.st_mtime,
			req->objstat.st_property,
			req->objstat.st_origincode));
	if (req->objstat.st_origincode == 0) {
		/* req->objstat.st_origincode가 0이 생기는 이유는 현재 파악이 안됨 */
		req->p1_error = MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	else if (req->file == NULL) {
		req->p1_error = req->objstat.st_origincode;
	}
	else if (req->objstat.st_origincode >= 500) {
		/* 500 이상의 에러 코드에 대해서는 NetCache 에서 body를 줄때도 있고 주지 않을때도 있어서 SolProxy에서 body를 만든다. */
		req->p1_error = req->objstat.st_origincode;

	}
	else {
		if (req->objstat.st_property) {
			hd_dump_property(vs_data(req->url), req->objstat.st_property, req);
		}
		/*
		 * 오리진 코드가 200이나 206인 경우를 제외 하고는 모두 그대로 전달 한다.
		 */
		if (req->objstat.st_origincode != 200  && req->objstat.st_origincode != 206) {
			req->resultcode = req->objstat.st_origincode;
			req->subrange = 0; /* 오리진에서 200이나 206인 경우 외에는 모두 range 요청을 무시한다. */
		}
	}
	return 0;
}

static int
scx_handle_method(nc_request_t *req)
{
	char 					dzbuf[256];
	int 					ret = MHD_YES;
	int 					enu = 0;
	double 					ts, te;
	scx_codepage_t 			*page;
	struct MHD_Connection 	*connection = req->connection;
	nc_method_t 			method = req->method;
	size_t					buffer_size = 0;
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};


	if (req->step == STEP_START) {
		/* first-phase call : client요청에 대한 첫번째 호출, 헤더 정보를 모두 수신했을 때 호출*/
		ret = scx_process_request(req);
		if(ret != MHD_HTTP_CONTINUE) {
			return ret;
		}
		/* openapi 처리 부분 */
		if (req->isopenapi == 1) {
			ret = rsapi_do_handler(req);
			return ret;
		}
		/************************** Content Verify Phase Handler 공통 부분 *************************************/
		if (NULL == req->file				/* 이전 단계에서 이미 파일이 open되어 있거나 */
				&& NULL == req->scx_res.body) { 		/* body를 만든 경우는 아래의 과정을 skip 한다. */
			scx_update_ignore_query_url(req);
			/*
			 * scripting에서 object_key 가 생성안된 경우
			 * 생성할 것
			 */
			if (req->object_key == NULL) {
				req->object_key = req->ori_url;
			}

			/* 스트리밍 기능 사용시에는 일반 요청에 대해서는 응답하지 않는다. */
			if (req->streaming) {
				ret = handle_method_streaming_phase(req);
			}

		}
		else {
			req->step = STEP_VERIFY_CONDITION;
			shm_log_update_step(req);
		}
	}
	if (req->step <= STEP_POST_OPEN) {
		/* 아래 부터는 streaming 경우는 실행 할 필요 없이 다음 step으로 넘어 간다.*/
		/*
		 * script에서 property key가 생성된 경우 stat_key를
		 * netcache core에게 전달
		 * script엔진에서 따로 안만든 경우는 stat-cache key는 objectkey와 동일한 것으로
		 * 간주함
		 */
		ret = handle_method_open_phase(req);
		if (ret < 0 ) {
			/* nc_open_extended_apc()이 완료 되지 않은 경우임 */
			return ret;
		}
	}
	//TRACE((T_WARN, "URL[%s] - got( header parsing done)\n", url));

	if (req->step <= STEP_VERIFY_CONDITION) {
		/************************** Content Verify Phase Handler가 실행 되는 부분 *************************************/
		phase_ctx.req = (void *)req;
		if (SCX_YES != scx_phase_handler(PHASE_CONTENT_VERIFY, &phase_ctx)) {
			/* uri parse phase에서 에러를 낼 경우 핸들러 함수 내에서 req->p1_error에 에러를 셋팅해 주어야 한다. */
			if (!req->p1_error)	req->p1_error = MHD_HTTP_NOT_FOUND;
		}
		if (0 == req->p1_error) {
			ret = scx_operate_range(req);
			if(ret) {
				req->p1_error = ret;
			}
		}

		/*
		 * GET 요청 수신 완료
		 */
	//		req->t_req_lba = sx_get_time();
		/* second-phase call : request 내의 모든 값을 수신 완료 */
		//TRACE((T_WARN, "******* file range: cursor=%lld, remained=%lld\n",
				//(long long)req->cursor, (long long)req->remained));
		if (req->p1_error != 0) {
			/*
			 * 해당 url에 대응하는 파일을 netcache 에서 찾지 못함
			 */
			scx_error_log(req, "[%s] origin error(%d).\n", vs_data(req->ori_url), req->p1_error);
			ret = nce_handle_error(req->p1_error, req);
			return ret;
		}
		if (S_ISDIR(req->objstat.st_mode)) {
			/*
			 * 해당 url이 파일이 아니고 directory 에 해당함
			 * 실제로는 이 코드는 실행이 안될 것임. 왜냐하면 오리진에서
			 * 주는대로 응답해야하는데, 요기는 그게 아닌 코드임
			 */
			scx_error_log(req, "[%s] - dir\n", vs_data(req->ori_url));
			ret = nce_handle_error(MHD_HTTP_FORBIDDEN, req);
			return ret;
		}

		TRACE((T_DAEMON, "[%llu] URL[%s] - got origin code, %d\n", req->id, vs_data(req->ori_url), req->objstat.st_origincode));
		/*
		 * 컨텐츠 객체에 대해서 응답 생성
		 * 응답을 적절히 생성하기위해서 아래와 같은 단계로 체크를 진행
		 * 1. conditional 요청인가?
		 * 2. sub-range 요청인가?
		 */
		if (!req->streaming /* streaming 기능 사용시에는 req->file이 NULL 경우가 있음 */
				&& NULL == req->scx_res.body) {	/* edited_body에 값이 들어 있는 경우는 동적 body 생성이 된 경우도  req->file이 NULL인 경우가 있을수 있음 */
			if (req->file == NULL) {
				scx_error_log(req, "Service[%s]/URL[%s] - open error (%s)\n",vs_data(req->service->name), vs_data(req->ori_url), strerror(nc_errno()));
				ret = nce_handle_error(MHD_HTTP_SERVICE_UNAVAILABLE, req);
				return ret;
			}
		}
		/*
		 * 조건부 요청에 대한 처리 진행
		 */
		if (req->condition != HTTP_IF_NULL) {
			ret = scx_operate_condition(req);
			if (req->response) {
				ret = scx_finish_response(req);
				return ret;
			}
		}
	} //end of if (req->step <= STEP_VERIFY_CONDITION)


	/*
	 * precondition 검사를 통과한 상태
	 * subrange 요청인지 확인
	 */
	/* 원본 컨텐츠의 크기를 모르는 경우 range 요청을 받지 않는다. */
	if (req->subrange && !req->objstat.st_sizedecled) {

		/* 비정상 적인 요청 */
		TRACE((T_DAEMON, "[%llu] URL[%s] - range request not supported content\n", req->id, vs_data(req->ori_url)));
		/* origin의 컨텐츠 크기를 모를 경우에는 range 요청이 들어오면 range 속성을 없앤다.*/
		req->subrange = 0;
		/* range 요청을 full get으로 바꾸는 경우 range관련 부분을 모두 초기화 한다. */
		req->range.operated = 0;
		ret = scx_operate_range(req);
	}

	if (req->subrange && !nce_is_range_satisfiable(req)) {
		req->resultcode = MHD_HTTP_REQUESTED_RANGE_NOT_SATISFIABLE;
		page = scx_codepage(req->resultcode);
		nce_create_response_from_buffer(req, page->len, (void *) page->page);
		nce_add_basic_header(req, req->response);
		goto L_handled;
	}

//#ifdef ZIPPER /* 여기서 모든 streaming의 처리를 끝낸다. */
	if (req->streaming) {

		TRACE((T_DAEMON, "[%llu] URL[%s] - condition[%d], subrange[%d], remained[%lld]\n",
				req->id,vs_data(req->url), req->condition, req->subrange, req->remained));


		if(req->objstat.st_sizedecled) {
			if(req->scx_res.body != NULL) {
				/* streaming 기능으로 동작시에는 scx_res.body에 할당된 크기가  scx_res.body_len 대신에 streaming->media_size에 들어가 있다 */
				ASSERT(req->streaming->media_size);
				strm_compress_manifest(req);
				if (req->resp_body_compressed == 1) {
					nce_create_response_from_buffer(req, req->streaming->media_size, (void *) req->scx_res.body);
				}
				else {
					nce_create_response_from_buffer(req, req->remained, (void *) req->scx_res.body+req->cursor);
				}
				sprintf(dzbuf, "%lld", req->streaming->media_size);
			}
			else {
				buffer_size = scx_calc_xfer_buffer_size(req, req->remained);
				// MHD 0.9.60 부터 Content-Length 직접 설정을 지원하지 않고 MHD_create_response()의 size 파라미터로 Content-Length 헤더가 결정 된다.
				req->response = MHD_create_response_from_callback(req->remained,
																	buffer_size,
																	&scx_async_streaming_reader,
																	(void *)req,
																	NULL);
				sprintf(dzbuf, "%lld", req->remained);
			}

		}
		else {
			/* CMAF live의 서비스에서 fragment가 생성 중인 경우 여기로 들어 온다. */
			req->response = MHD_create_response_from_callback(	MHD_SIZE_UNKNOWN, // (req->objstat.st_norandom?MHD_SIZE_UNKNOWN:req->remained),
																gscx__xfer_size,
																&scx_async_streaming_reader,
																req,
																NULL);
		}
		if (req->subrange) {
			req->resultcode = MHD_HTTP_PARTIAL_CONTENT;
			sprintf(dzbuf, "bytes %lld-%lld/%lld", req->range.begin, req->range.end,req->objstat.st_size);
			MHD_add_response_header(req->response, MHD_HTTP_HEADER_CONTENT_RANGE, dzbuf);
			TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "[%llu] URL[%s], header => ('%s', '%s')\n",
					req->id, vs_data(req->url), MHD_HTTP_HEADER_CONTENT_RANGE, dzbuf));
		}
		else
		{
			req->resultcode = MHD_HTTP_OK;
		}
		if(req->streaming->media_type == MEDIA_TYPE_DOWN) {
			/* 원본 파일을 그대로 받는 경우에는 오리진의 헤더를 그대로 전달한다. */
			if (req->streaming->source != NULL && req->streaming->source->file != NULL) {
				enu = nc_enum_file_property(req->streaming->source->file, nce_inject_property, req);
			}
		}
		else {
			strm_add_streaming_header(req);
		}

	}
	else {
//#endif
		if ( req->scx_res.body) {
			/* body가 편집 되는 경우는 아래의 두 값이 필수이다 */
		//	ASSERT(1 == req->objstat.st_rangeable);
			ASSERT(1 == req->objstat.st_sizedecled);
		}
		//if (req->objstat.st_sizeknown ) {
		if(req->objstat.st_sizedecled) {
			/* subrange 요청이 가능한 객체 */
			TRACE((T_DAEMON, "[%llu] URL[%s] - condition[%d], subrange[%d], remained[%lld]\n",
					req->id, vs_data(req->url), req->condition, req->subrange, req->remained));

			// MHD 0.9.60 부터 Content-Length 직접 설정을 지원하지 않고 MHD_create_response()의 size 파라미터로 Content-Length 헤더가 결정 된다. */
			buffer_size = scx_calc_xfer_buffer_size(req, req->remained);
			req->response = MHD_create_response_from_callback(req->remained,
																buffer_size,
																&scx_async_object_reader,
																req,
																NULL);

			sprintf(dzbuf, "%lld", req->remained);

			if (req->resultcode == 0) {
				/* 오리진 응답이 200이나 206인 경우 */
				if (req->subrange) {
					req->resultcode = MHD_HTTP_PARTIAL_CONTENT;
					sprintf(dzbuf, "bytes %lld-%lld/%lld", req->range.begin, req->range.end,req->objstat.st_size);
					MHD_add_response_header(req->response, MHD_HTTP_HEADER_CONTENT_RANGE, dzbuf);
					TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "[%llu] URL[%s], header => ('%s', '%s')\n",
							req->id, vs_data(req->url), MHD_HTTP_HEADER_CONTENT_RANGE, dzbuf));
				}
				else {
					req->resultcode = MHD_HTTP_OK;
				}
			}

		}
		else {
			TRACE((T_DAEMON, "[%llu] URL[%s]/rangeable[1 - condition[%d], subrange[%d], remained[%lld]\n",
					req->id, vs_data(req->url), req->condition, req->subrange, req->remained));

			// MHD 0.9.60 부터 Content-Length 직접 설정을 지원하지 않고 MHD_create_response()의 size 파라미터로 Content-Length 헤더가 결정 된다.
			req->response = MHD_create_response_from_callback(	MHD_SIZE_UNKNOWN, // (req->objstat.st_norandom?MHD_SIZE_UNKNOWN:req->remained),
																gscx__xfer_size,
																&scx_async_object_reader,
																req,
																NULL);


			if (req->resultcode == 0) {
				req->resultcode = MHD_HTTP_OK;
			}
		}
		nce_add_file_info(req, &req->objstat);
		if (req->file) {
			enu = nc_enum_file_property(req->file, nce_inject_property, req);
		}
//#ifdef ZIPPER
	}
//#endif
	kv_for_each(req->scx_res.headers, scx_update_resp_header, req); 	/* custom response header를 추가한다. */
	nce_add_basic_header(req, req->response);

	if (req->keepalive) { // && strstr(version, "1.1"))
		//MHD_add_response_header(req->response, MHD_HTTP_HEADER_CONNECTION, "Keep-Alive");
		scx_update_resp_header(MHD_HTTP_HEADER_CONNECTION, "Keep-Alive", (void *)req);
		TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "[%llu] URL[%s], header => ('%s', '%s')\n",
				req->id, vs_data(req->url), MHD_HTTP_HEADER_CONNECTION, "keep-alive"));
	}

	if (!req->subrange && req->objstat.st_sizedecled) {
		scx_update_resp_header(MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes", (void *)req);
	}

L_handled:
	/************************** Build Response Phase Handler가 실행 되는 부분 *************************************/
	if (req->response == NULL) {
		TRACE((T_ERROR, "[%llu] response NULL\n", req->id));
	}

	if (0 == req->async_ctx.job_avail) {
		TRACE((T_DAEMON, "[%llu] URL[%s] Unusual complete.\n", req->id, vs_data(req->url)));
		return ret;
	}
	ret = scx_finish_response(req);
	TRACE((T_DAEMON, "[%llu] URL[%s] MHD_destroy_response() called.\n", req->id, vs_data(req->url)));
	return ret;
}


static int
scx_handle_unknown(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int ret;
	if (req->service->bypass_method) {
		if (vs_strcasestr(req->service->bypass_method, req->zmethod) != NULL ) {
			//custom Method는 캐싱이 되지 않도록한다.
	//		req->mode = O_RDONLY|O_NCX_NOCACHE|O_NCX_NORANDOM|O_NCX_PRIVATE;
			return scx_handle_method(req);
		}
	}
	ret = nce_handle_error(MHD_HTTP_METHOD_NOT_ALLOWED, req);
	return ret;
}
static int
scx_handle_post(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int 					result = MHD_HTTP_OK;
	int 					ret;
	char 					t;
	time_t 					to, acc_tm = 0;
	char 					pbuf[2048];
	int 					o_code;
	scx_codepage_t 			*page;
	struct MHD_Connection 	*connection = req->connection;
	double 				ts, te;
	size_t 					buffer_size;

	TRACE((T_DAEMON, "[%llu] scx_handle_post URL[%s]/size = %ld, %s\n", req->id, vs_data(req->ori_url), (long)*upload_data_size, upload_data == '\0'?"NULL":"full" ));
	if (req->service->core->mnt == NULL) {
		/* 오리진 설정이 되지 않은 볼륨으로 요청이 들어 오는 경우는 에러 처리 한다. */
		scx_error_log(req, "%s undefined origin. url(%s)\n", __func__, vs_data(req->ori_url));
		if (!req->p1_error)	req->p1_error = MHD_HTTP_BAD_REQUEST;
		return MHD_YES;
	}
	if (req->async_ctx.phase == SRP_INIT) {
		/* first-phase call : client요청에 대한 첫번째 호출, 헤더 정보를 모두 수신했을 때 호출*/
		ret = scx_process_request(req);
		if(ret != MHD_HTTP_CONTINUE) {
			return ret;
		}
		if (req->objlength < 0) {
			scx_error_log(req, "Content-Length not exist[%s]\n", vs_data(req->ori_url));
			ret = nce_handle_error(MHD_HTTP_LENGTH_REQUIRED, req);
			return ret;
		}
#if 0
		if (req->objlength > 4096) {
			scx_error_log(req, "Content-Length too large(%d).\n", req->objlength);
			ret = nce_handle_error(MHD_HTTP_REQUEST_ENTITY_TOO_LARGE, req);
			return ret;
		}
#endif
		if (req->file == NULL) {
			TRACE((T_DAEMON, "[%llu] URL[%s]/kv_oob_write - KV[%p].RING, bytes to write=%d\n", req->id, vs_data(req->ori_url), req->options, (long)*upload_data_size));
			kv_setup_ring(req->options, min(gscx__config->pool_size/2, 4096));	/* pool에서 할당 가능한 최대 크기가 gscx__config->pool_size/2이다. */
			kv_oob_command(req->options, "POST", req->objlength);
			req->u.post.oob_ring_opened = 1;
			kv_set_opaque(req->options, (void *)req);
			req->mode = O_RDONLY|O_NCX_PRIVATE|O_NCX_NORANDOM|O_NCX_REFRESH_STAT;

			ts = sx_get_time();
			CHECK_TIMER("nc_open_extended2", req,
					req->file 	= nc_open_extended2(req->service->core->mnt, vs_data(req->ori_url), vs_data(req->ori_url), req->mode, &req->objstat, req->options));
			te = sx_get_time();
//			printf( "URL[%s] - req->file[%p] nc_error()=%d\n", vs_data(req->ori_url), req->file, nc_errno() );
			TRACE((T_DAEMON, "[%llu] URL[%s] - KV[%p].RING object status code=%d\n", req->id, vs_data(req->ori_url), req->options, req->objstat.st_origincode));
			req->t_nc_open += (te - ts);
			//		__sync_add_and_fetch(&gscx__open_count, 1);//임시 코딩
			if (req->file == NULL|| nc_errno() != 0) {
//				printf("nc error\n");
				int nce = nc_errno();
				TRACE((T_WARN|T_DAEMON, "[%llu] URL[%s] - request[%p]/INODE.PTR[%p]KV[%p].RING open failed for POST(data_len=%d)(stat=%d, nc_errno=%d)\n",
						req->id, vs_data(req->ori_url), req, req->file, req->options, req->objlength, req->objstat.st_origincode, nce));
				scx_error_log(req, "POST/URL[%s] - could not open(netcache error code=%s)\n", vs_data(req->ori_url), strerror(nce));

				ret = nce_handle_error(scx_map_nc_error(nce), req);
				return ret;
			}
		}
		//TRACE((T_WARN|T_DAEMON, "URL[%s] - KV[%p].RING invoking IO\n", vs_data(req->ori_url), req->options));
		//nc_invoke_io(req->file);
		return MHD_YES;
	}

	if ( *upload_data_size != 0) {
		/*
		 * message body에서 읽은 데이타가 있음, 처리해야함
		 */
		/* put to oob ring buffer */
		TRACE((T_DAEMON, "[%llu] URL[%s]/kv_oob_write - INODE[%d].request[%p]/KV[%p].RING writing  %ld bytes\n",
				req->id, vs_data(req->ori_url), nc_inode_uid(req->file), req, req->options, (long)*upload_data_size));
		req->req_body_size += *upload_data_size;
		if (req->kv_oob_write_error) {
			/* 이전에 kv_oob_write()에러 에러를 리턴한 경우는 upload_data을 읽어서 버린다. */
			*upload_data_size = 0;	/* upload_data_size를 0으로 설정 하지 않고 경우 중간에 에러를 리턴하려고 하면 MHD에서 client에 500 Internal Server Error를 리턴한다. */
			return MHD_YES;
		}
		else {
			ts = sx_get_time();
			do {
				ret = kv_oob_write(req->options, (char *)upload_data, *upload_data_size);
				if (ret <= 0) {
					TRACE((T_WARN|T_DAEMON, "[%llu] URL[%s]/kv_oob_write(%d) got error\n", req->id, vs_data(req->ori_url), ret));
				}
			} while (ret == 0); /* timeout(no buffer available) */

			te = sx_get_time();
			req->t_nc_read += (te -ts);
		}
		*upload_data_size = 0;
		/* 정상 처리, 추가로 처리해야할 데이타가 있으면 요청 */
		return MHD_YES;
	}
	/*
	 * POST 요청 수신 완료
	 */
	TRACE((T_DAEMON, "[%llu] URL[%s]/kv_oob_write - KV[%p].RING, post finished\n", req->id, vs_data(req->ori_url), req->options, (long)*upload_data_size));
	//	req->t_req_lba = sx_get_time();
	ASSERT(req->options != NULL);
	//kv_dump_property(req->options, pbuf, sizeof(pbuf)) ));
L_write_error:
	TRACE((T_DAEMON, "[%llu] URL[%s] - INODE[%d].request[%p]/KV[%p].RING marking EOT with property, content-length=%lld\n",
			req->id, (char *)vs_data(req->ori_url),
			nc_inode_uid(req->file),
			req,
			req->options,
			(long long)req->objlength));

	kv_oob_write_eot(req->options);
	req->u.post.oob_ring_opened = 0;
	TRACE((T_DAEMON, "[%llu] URL[%s] - waiting on property, KV[%p].RING\n", req->id, vs_data(req->ori_url), req->options));
	to = scx_get_cached_time_sec() + 30;
	do {
		if (to < scx_get_cached_time_sec()) {
			acc_tm += 30;
			TRACE((T_WARN, "[%llu] VOLUME[%s]/URL[%s] - too long time(%ld secs) wait on INODE[%d].KV[%p] in kv_get_raw_code()\n",
					req->id, vs_data(req->service->name), vs_data(req->ori_url), (long)acc_tm, nc_inode_uid(req->file), req->options));
			to = scx_get_cached_time_sec() + 30;
			/* 30초이상 걸린 경우 에러를 리턴하도록 한다. */
			req->resultcode = MHD_HTTP_GATEWAY_TIMEOUT;
			page = scx_codepage(req->resultcode);
			nce_create_response_from_buffer(req, page->len, (void *) page->page);
			if (req->response) {
				nce_add_basic_header(req, req->response);
			}
			goto L_handled_post;
		}
		o_code = kv_get_raw_code(req->options);
		if (o_code == 0) {
			TRACE((T_DAEMON, "[%llu] URL[%s] - waiting raw code on INODE[%d]/KV[%p].RING\n", req->id, vs_data(req->ori_url), nc_inode_uid(req->file), req->options));
			bt_msleep(100);
		}
	} while (o_code == 0);

	if (o_code >=  500) {
		TRACE((T_DAEMON, "[%llu] URL[%s] - POST got error[%d]\n", req->id, vs_data(req->ori_url), o_code));
		req->resultcode = o_code;
		page = scx_codepage(req->resultcode);
		nce_create_response_from_buffer(req, page->len, (void *) page->page);
		if (req->response) {
			nce_add_basic_header(req, req->response);
		}
		goto L_handled_post;
	}

	/* end of upload */
	if (req->file) {
		ASSERT(nc_check_memory(req->file, 0));
		nc_read(req->file, (char *)&t, (nc_off_t)0, (nc_size_t)1);
#if 1
		nc_fgetattr(req->file, &req->objstat);
		if(req->objstat.st_sizedecled) {
			req->remained = req->objstat.st_size;
			req->cursor = 0;
		}
		if (req->objstat.st_sizedecled) {
			buffer_size = scx_calc_xfer_buffer_size(req, req->remained);
		}
		else {
			buffer_size = gscx__xfer_size;
		}
		req->response = MHD_create_response_from_callback(	(req->objstat.st_sizedecled?req->remained:MHD_SIZE_UNKNOWN),
				buffer_size,
				&scx_async_object_reader,
				req,
				NULL);
#else
		const char *zlen;
		req->objstat.st_rangeable = 0;
		zlen = nc_find_property(req->file, (const char *)"content-length");
		if (zlen) {
			buffer_size = scx_calc_xfer_buffer_size(req, atol(zlen));
		}
		else {
			buffer_size = gscx__xfer_size;
		}
		req->response = MHD_create_response_from_callback(	(zlen?atol(zlen):MHD_SIZE_UNKNOWN),
				buffer_size,
				&scx_async_object_reader,
				req,
				NULL);
#endif
		nc_enum_file_property(req->file, nce_inject_property, req);
		nce_add_basic_header(req, req->response);
		req->resultcode = o_code;
#if 0
			// MHD 0.9.60 부터는 Content-Length 직접 설정을 지원하지 않는다.
		if (req->objstat.st_size) {	//Chunked data일 경우는 Content-Length를 주지 않기 위해 Content-Length를 제거한다
			MHD_del_response_header (req->response,MHD_HTTP_HEADER_CONTENT_LENGTH,NULL);
		}
#endif
	}
	else {
		req->resultcode = o_code;
		page = scx_codepage(req->resultcode);
		nce_create_response_from_buffer(req, page->len, (void *) page->page);
		if (req->response) {
			nce_add_basic_header(req, req->response);
		}
		TRACE((T_WARN, "[%llu] URL[%s] - null object\n", req->id, vs_data(req->ori_url)));
	}

L_handled_post:
	ret = scx_finish_response(req);
	return ret;
}
static int
scx_handle_put(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int 					result = MHD_HTTP_OK;
	int 					ret;
	char 					t;
	char 					*turl = NULL;
	char 					pbuf[2048];
	int 					o_code;
	nc_ssize_t 				wr;
	scx_codepage_t 			*page;
	struct MHD_Connection 	*connection = req->connection;
	size_t 					buffer_size;

	if (req->service->core->mnt == NULL) {
		/* 오리진 설정이 되지 않은 볼륨으로 요청이 들어 오는 경우는 에러 처리 한다. */
		scx_error_log(req, "%s undefined origin. url(%s)\n", __func__, vs_data(req->ori_url));
		if (!req->p1_error)	req->p1_error = MHD_HTTP_BAD_REQUEST;
		return MHD_YES;
	}
	TRC_BEGIN((__func__));
	if (req->async_ctx.phase == SRP_INIT) {
		/* first-phase call : client요청에 대한 첫번째 호출, 헤더 정보를 모두 수신했을 때 호출*/
		ret = scx_process_request(req);
		if(ret != MHD_HTTP_CONTINUE) {
			TRC_END;
			return ret;
		}

		/*
		 * write를 위한  inode 생성
		 */
		kv_set_opaque(req->options, (void *)req);
		//req->file = file = nc_open_extended(req->service->core->mnt, vs_data(req->ori_url), vs_data(req->ori_url), O_CREAT|O_WRONLY|O_NCX_PRIVATE|O_NCX_DONTCHECK_ORIGIN, req->options);
		req->mode = O_CREAT|O_WRONLY|O_NCX_PRIVATE|O_NCX_DONTCHECK_ORIGIN;
		CHECK_TIMER("nc_open_extended2", req, req->file 	= nc_open_extended2(req->service->core->mnt, vs_data(req->ori_url), vs_data(req->ori_url), req->mode, &req->objstat, req->options))
//		__sync_add_and_fetch(&gscx__open_count, 1);//임시 코딩
		req->u.put.offset  = 0;
		TRC_END;
		return MHD_YES;
	}

	if ( *upload_data_size != 0) {
		int 	r;
		/*
		 * message body에서 읽은 데이타가 있음, 처리해야함
		 */
		/* put to oob ring buffer */
		//TRACE((T_DAEMON, "URL[%s]/nc_write(off=%lld, size=%ld)\n", vs_data(req->ori_url), req->put.offset, (long)*upload_data_size));
		wr = nc_write(req->file, upload_data, req->u.put.offset, *upload_data_size);
		if (wr != *upload_data_size ) {
			scx_error_log(req, "URL[%s] - write error\n", vs_data(req->ori_url));
			/* handle error */
			goto L_write_error;
		}
		req->resultcode = kv_get_raw_code(req->options);
		if (req->resultcode > 0) {
			TRACE((T_WARN, "[%llu] URL[%s] - write error, %d\n", req->id, vs_data(req->ori_url), req->resultcode));
			goto L_write_error;
		}

		req->u.put.offset += wr;
		req->req_body_size += wr;
		*upload_data_size = 0;
		/* 정상 처리, 추가로 처리해야할 데이타가 있으면 요청 */
		TRC_END;
		return MHD_YES;
	}
	/*
	 * PUT 요청 수신 완료
	 */
//	req->t_req_lba = sx_get_time();
	ASSERT(req->options != NULL);
	ret = nc_flush(req->file);
#if 0
	if (req->file) {
		double 				ts, te;
		ts = sx_get_time();
		nc_close(req->file);
		req->file = NULL;
		te = sx_get_time();
		req->t_nc_close = (te - ts);
	}
#endif
L_write_error:
	TRACE((T_DAEMON, "[%llu] URL[%s] - waiting on property, %p\n", req->id, vs_data(req->ori_url), req->options));
	do {
		   o_code = kv_get_raw_code(req->options);
		   if (o_code == 0) {
				   bt_msleep(1);
		   }
		   TRACE((T_DAEMON, "kv_get_raw_code, %d\n",o_code));
	} while (o_code == 0);
	if (o_code >=  400) {
	   TRACE((T_DAEMON, "[%llu] URL[%s] - PUT got error[%d]\n", req->id, vs_data(req->ori_url), o_code));
	   req->resultcode = o_code;
	   page = scx_codepage(req->resultcode);
	   nce_create_response_from_buffer(req, page->len, (void *) page->page);
	   if (req->response) {
		   nce_add_basic_header(req, req->response);
	   }
	   goto L_handled_put;
	}
    /* end of upload */
    if (req->file) {
    	ASSERT(nc_check_memory(req->file, 0));
		nc_read(req->file, (char *)&t, (nc_off_t)0, (nc_size_t)1);
		nc_fgetattr(req->file, &req->objstat);
		if(req->objstat.st_sizedecled) {
			req->remained = req->objstat.st_size;
			req->cursor = 0;
		}
		if (req->objstat.st_sizedecled) {
			buffer_size = scx_calc_xfer_buffer_size(req, req->remained);
		}
		else {
			buffer_size = gscx__xfer_size;
		}
		req->response = MHD_create_response_from_callback((req->objstat.st_sizedecled?req->remained:MHD_SIZE_UNKNOWN),
															buffer_size,
															&scx_async_object_reader,
															req,
															NULL);
		nc_enum_file_property(req->file, nce_inject_property, req);
		nce_add_basic_header(req, req->response);
		req->resultcode = o_code;
    }


L_handled_put:
	ret = scx_finish_response(req);
	TRC_END;
	return ret;
}

/*
 * options method에 대해서 origin으로 bypass 하는 기능이 생길 경우
 * req->streaming이 null이 아닌 경우만 제외하고는 모두 origin으로 bypass를 할수 있어야한다.
 * 또 설정으로 options대해 origin을 가지 않고 직접 응답 하는 기능도 필요 할수도 있음(양방향 사용시등)
 */
static int
scx_handle_options(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int ret;
	struct MHD_Connection 	*connection = req->connection;

#ifndef ZIPPER
	/* OPITIONS를 오리진에서 받고 싶은 경우 bypass_method를 설정하면 된다. */
	if (req->service->bypass_method) {
		if (vs_strcasestr(req->service->bypass_method, req->zmethod) != NULL ) {
			return scx_handle_method(req);
		}
	}
#endif
	if (req->async_ctx.phase == SRP_INIT) {
		/* first-phase call : client요청에 대한 첫번째 호출, 헤더 정보를 모두 수신했을 때 호출*/
		ret = scx_process_request(req);
		if(ret != MHD_HTTP_CONTINUE) {
			return ret;
		}
	}

	req->resultcode = MHD_HTTP_OK;

	nce_create_response_from_buffer(req, 0, NULL);
	nce_add_basic_header(req, req->response);
	scx_add_cors_header(req);
	if (req->streaming) {	/* DASH 프로토콜의 경우 OPTIONS Method로 CORS를 확인 한다. */
		//strm_add_streaming_header(req);
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_ALLOW, "HEAD,GET,OPTIONS");
	}
	else {
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_ALLOW, "HEAD,GET,OPTIONS,POST");
	}
	ret = scx_finish_response(req);
	return ret;
}

static int
scx_handle_purge(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int 					r;
	int 					ret = MHD_YES;
	scx_codepage_t 			*page;
	int						cnt = 0;
	struct MHD_Connection 	*connection = req->connection;
	char					*pattern;
	char 					*purge_path = vs_data(req->ori_url);

	ret = scx_process_request(req);
	if(ret != MHD_HTTP_CONTINUE) {
		return ret;
	}
	scx_update_ignore_query_url(req);
	if (req->streaming != NULL) {
		if ( (req->streaming->media_mode & O_STRM_TYPE_ADAPTIVE) != 0 ) {
			/* adaptive mode 경우는 smil파일을 받아서 그 안에 있는 컨텐츠 까지 퍼지 대상임 */
			if (!strm_handle_adaptive(req)) {
				scx_error_log(req, "SMIL handler error. URL(%s)\n", vs_data(req->url));
				if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				return MHD_YES;;
			}
		}
	}


	/*
	 * int nc_purge(nc_mount_context_t *mnt, char *pattern, int iskey, int ishard)
			@mnt - mount volume context
			@pattern - 삭제 패턴 (예:/temp/babo.jpg, /temp/*, /temp/babo?.jpg 등)
			@iskey - TRUE이면 삭제 패턴의 비교는 객체 key에 대해서 적용, FALSE면 패턴비교가 객체의 원본 경로에 대해서 적용
			@ishard - TRUE면 캐싱 객체 파일까지 삭제, FALSE면 캐싱 객체의 유효시간만 초기화
			key 기준 purge일 경우는 url이 아닌 object_key를 패턴으로 넘겨준다.
	 */
#ifdef ZIPPER
	cnt = strm_purge(req);
	/* Zipper의 경우에도 패턴퍼지나 볼륨 퍼지를 할수 있지만 media index에 대해서는 할수 없기 때문에 효과가 없음 */
#else
	if (1 == req->purge_volume) {
		/* volume purge 인 경우 path를 무시한다. */
		cnt = nc_purge(req->service->core->mnt, (char *)"/*", FALSE, req->purge_hard?TRUE:FALSE);
	}
	else {
		if (1 == req->purge_key) {
			if (NULL == req->object_key) {
				req->purge_key = 0;	/* key에 대한 퍼지가 들어 왔더라도 object_key가 지정되어 있지 않으면 key 퍼지를 하지 않는다. */
				TRACE((T_DAEMON, "[%llu] Key Purge requested but object key was not set.\n", req->id));
			}
		}
		if (1 == req->service->streaming_enable) {
			/*
			 * streaming 기능을 사용할 경우는 media index 도 같이 퍼지 해주어야 하기 때문에 조건이 다소 복잡하다.
			 */
			if (NULL != req->streaming) {
				/* streaming url 형태의 퍼지 요청인 경우 */
				cnt = strm_purge(req);
			}
			else {
				cnt = nc_purge(req->service->core->mnt, (char *)vs_data(req->purge_key?req->object_key:req->ori_url),req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);
				/* streaming url 형태가 아니라도 media index에 캐싱 가능성도 있으므로 media index 퍼지도 같이 실행한다*/
#if 0
		// 2020/12/07 base_path를 netcache에 직접 설정 하도록 수정 하면서 이 부분은 필요없어짐
				if(req->service->base_path) {
					/*
					 * media cache에 들어 있는 컨텐츠 경로는 base_path가 들어 있지 않은 경로인데
					 * req->ori_url은 base_path가 붙은 경로라서 base_path를 빼고 퍼지를 하기 위해 아래의 동작을 한다.
					 */
					if (vs_length(req->ori_url)  > vs_length(req->service->base_path)) {
						purge_path  +=  vs_length(req->service->base_path);
					}
				}
#endif
				strm_purge_media_info(req, vs_data(req->service->name), purge_path);
			}
		}
		else {
			cnt = nc_purge(req->service->core->mnt, (char *)vs_data(req->purge_key?req->object_key:req->ori_url),req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);
		}
	}

#endif

	req->resultcode = MHD_HTTP_OK;
	page = scx_codepage(req->resultcode);
	nce_create_response_from_buffer(req, page->len, (void *) page->page);
	nce_add_basic_header(req, req->response);
	ret = scx_finish_response(req);

	return ret;
}

/*
 * 서비스 중인 모든 볼륨에 purge를 한다.
 * localhost로만 접근을 허용하고 Host는 others이고 path는 '/*'만을 허용한다.
 * 예: curl -v -o /dev/null -H "Host: others" 127.0.0.1/* -X "PURGEALL"
 */
static int
scx_handle_purgeall(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int 					r;
	int 					ret = MHD_YES;
	scx_codepage_t 			*page;
	int						cnt = 0;
	struct MHD_Connection 	*connection = req->connection;


	ret = scx_process_request(req);
	if(ret != MHD_HTTP_CONTINUE) {
		TRC_END;
		return ret;
	}

	req->resultcode = MHD_HTTP_UNAUTHORIZED;
	/*
	 * localhost로만 접근을 허용하고 Host는 others이고 path는 '/*'만을 허용한다.
	 * 예: curl -v -o /dev/null -H "Host: others" 127.0.0.1/* -X "PURGEALL"
	 *
	 */
	if ( vs_strcmp_lg(req->host, OTHERS_VOLUME_NAME) == 0 &&
			vs_strcmp_lg(req->client_ip, "127.0.0.1") == 0 &&
			vs_strcmp_lg(req->url, "/*") == 0 ) {
		vm_all_volume_purge();
		req->resultcode = MHD_HTTP_OK;
	}

	page = scx_codepage(req->resultcode);
	nce_create_response_from_buffer(req, page->len, (void *) page->page);
	nce_add_basic_header(req, req->response);
	ret = scx_finish_response(req);

	return ret;
}

/*
 * scx_process_request의 리턴값이 MHD_HTTP_CONTINUE를 제외한
 * MHD_YES나 MHD_NO가 리턴되면 호출한 지점에서 진행을 더이상 하지 않고 바로 리턴 해야 한다.
 */
static int
scx_process_request(nc_request_t *req)
{
	vstring_t 				*host_org = NULL;
	char 					*hostname = NULL;
	nc_method_t 			method = req->method;
	int						ret = 0;
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};
	/* first-phase call : client요청에 대한 첫번째 호출, 헤더 정보를 모두 수신했을 때 호출*/
	if (req->p1_error != 0) {
		nce_handle_error(req->p1_error, req);
		return ret;
	}
	/************************** Client Request Phase Handler 공통 부분 *************************************/
	host_org = req->host;
	site_run_trigger_client_request(req);
	if(req->scx_res.trigger == LUA_TRIGGER_RETURN){
		return scx_make_response(req);
	}
#ifndef ZIPPER	/* streaming 으로 동작 했을때에는 아래의 부분이 필요 없다 */
	if (req->host != host_org) {
		/* trigger가 host정보를 변경했음 */
		//vm_update_origin(req, req->host);
		kv_replace(req->options, MHD_HTTP_HEADER_HOST, vs_data(req->host), FALSE);
	}
	/* 설정 파일에 origin 연결시 사용할 hostname이 정의 되어 있는 경우 여기서 업데이트한다.
	 * 이 경우 lua script에서 바꿨더라도 설정이 우선한다.*/
	if(req->service->origin_hostname) {
		hostname = kv_find_val(req->options, MHD_HTTP_HEADER_HOST, FALSE);
		if (NULL == hostname) {
			kv_replace(req->options, MHD_HTTP_HEADER_HOST, vs_data(req->service->origin_hostname), FALSE);
		}
		else if (strcmp(hostname, vs_data(req->service->origin_hostname)) != 0) {
			kv_replace(req->options, MHD_HTTP_HEADER_HOST, vs_data(req->service->origin_hostname), FALSE);
		}
	}
#endif
	if (req->ovalue) {
		vm_update_versioned_origin(req, req->oversion, req->ovalue);
	}
	/************************** Client Request Phase Handler가 실행 되는 부분 *************************************/
	phase_ctx.req = (void *)req;
	if (SCX_YES != scx_phase_handler(PHASE_CLIENT_REQUEST, &phase_ctx)) {
		return MHD_YES;
	}
	return MHD_HTTP_CONTINUE;	/* 상황에 맞지 않는 리턴 코드이긴 하지만 바로 리턴 해야 되는 경우와 구분하기 위해 사용 */
}

static int
scx_handle_preload(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int 	ret = 0;
	char 	*preload_header = NULL;
#ifdef ZIPPER
	// ret = nce_handle_error(MHD_HTTP_METHOD_NOT_ALLOWED, req);
	req->scx_res.code = MHD_HTTP_METHOD_NOT_ALLOWED;
#else
	ret = scx_process_request(req);
	if(ret != MHD_HTTP_CONTINUE) {
		return ret;
	}
	scx_update_ignore_query_url(req);

	/* preload status 요청인지 확인 한다. */
	preload_header = (char *)MHD_lookup_connection_value(req->connection, MHD_HEADER_KIND, SBX_HTTP_HEADER_PRELOAD);
	if(preload_header) {
		if(strcasestr(preload_header, "status") != NULL) {
			ret = pl_preload_status((void *)req);
			switch(ret) {
			case 0 :	/* 아직 프리로드가 완료되지 않음 */
				kv_replace(req->scx_res.headers, SBX_HTTP_HEADER_PRELOAD_STATUS, "not-cached", 0);
				break;
			case -1 :	/* 프리로드(캐싱) 할수 없는 상태 */
				kv_replace(req->scx_res.headers, SBX_HTTP_HEADER_PRELOAD_STATUS, "nocache", 0);
				break;
			case 1 :	/* 프리로드가 완료됨*/
				kv_replace(req->scx_res.headers, SBX_HTTP_HEADER_PRELOAD_STATUS, "cached", 0);
				break;
			default : 	/* 이 경우가 발생하면 안됨 */
				kv_replace(req->scx_res.headers, SBX_HTTP_HEADER_PRELOAD_STATUS, "unknown", 0);
			}
			req->scx_res.code = MHD_HTTP_OK;
			goto scx_handle_preload_end;
		}
	}
	/* nce_handle_error() 보다 scx_make_response()를 사용 */
	ret = pl_add_preload((void *)req);
	if (0 > ret) {
		req->scx_res.code = MHD_HTTP_NOT_FOUND;
		//ret = nce_handle_error(MHD_HTTP_NOT_FOUND, req);
	}
	else {
		req->scx_res.code = MHD_HTTP_OK;
		//ret = nce_handle_error(MHD_HTTP_OK, req);
	}
#endif
scx_handle_preload_end:
	ret = scx_make_response(req);
	return ret;
}
static int
scx_handle_stat(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int r;
	r = nce_handle_error(MHD_HTTP_METHOD_NOT_ALLOWED, req);
	return r;
}

#if GNUTLS_VERSION_MAJOR >= 3
/**
 * @param session the session we are giving a cert for
 * @param req_ca_dn NULL on server side
 * @param nreqs length of req_ca_dn, and thus 0 on server side
 * @param pk_algos NULL on server side
 * @param pk_algos_length 0 on server side
 * @param pcert list of certificates (to be set)
 * @param pcert_length length of pcert (to be set)
 * @param pkey the private key (to be set)
 * 동적 볼륨의 경우 gscx__config의 인증서를 사용하고
 * static 볼륨의 경우는 site의 인증서를 사용한다.
 * 통합방법 고민 필요
 * static 볼륨이라도 해당 볼륨 설정에 인증서가 없는 경우는 default의 인증서를 사용한다.
 * 여기서 volume을 찾을수 없는 경우 volume을 생성하는 기능이 추가가 되어야 한다.
 * gnutls_privkey_t는 pointer이므로 malloc이 불필요 (typedef struct gnutls_privkey_st *gnutls_privkey_t;)
 * gnutls_pcert_st는 malloc이 필요함
 * typedef struct gnutls_pcert_st
{
  gnutls_pubkey_t pubkey;
  gnutls_datum_t cert;
  gnutls_certificate_type_t type;
} gnutls_pcert_st;
 * 해제시 gnutls_pubkey_deinit()와 gnutls_pcert_deinit ()을 해주어야함
 * 참고
 * http://www.gnutls.org/reference/gnutls-abstract.html
 * http://vega.frugalware.org/tmpgit/gnometesting.old2/source/lib/gnutls3/pkg/usr/include/gnutls/abstract.h
 *
 *
 */
static int
scx_sni_handler (gnutls_session_t session,
		const gnutls_datum_t* req_ca_dn,
		int nreqs,
		const gnutls_pk_algorithm_t* pk_algos,
		int pk_algos_length,
		gnutls_pcert_st** pcert,
		unsigned int *pcert_length,
		gnutls_privkey_t * pkey)
{
	char name[256];
	size_t name_len;

	unsigned int type;
	int ret;
	st_snicert_t *cert = NULL;

	name_len = sizeof (name);

	ret = gnutls_server_name_get (session, name, &name_len, &type, 0 /* index */);
	if (GNUTLS_E_SUCCESS != ret)
	{
		TRACE((T_INFO, "gnutls_server_name_get failed. reason : '%s'(%d)\n",  gnutls_strerror(ret), ret));
		if (GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE == ret) {
			/* SNI를 지원하지 못하는 client로 부터 ssl 요청이 들어온 경우는 default 인증서가 있는 경우 default 인증서를 내보낸다. */
			if(gscx__default_site->certificate) {
				cert = (st_snicert_t *)gscx__default_site->certificate;
				*pcert = &cert->pcrt;
				*pkey = cert->key;
				*pcert_length = cert->pcrt_cnt;
				return 0;
			}
		}
		return -1;
	}

	service_info_t		*service = NULL;
	/* 비정상 요청에 대해 도메인이 생성되는것을 막기위해 여기서는 static volume에 대해서만 추가 해야 한다. */
	service = vm_add(name, 1);
	if (service == NULL || service->site->certificate == NULL) {
		/* 해당 volume의 인증서가 없는 경우, gscx__config.certificate_crt 사용 */
		if(gscx__default_site->certificate) {
			cert = (st_snicert_t *)gscx__default_site->certificate;
		}
		else {
			/* default 인증서가 설정되지 않은 경우 */
			return -1;
		}
	}
	else {
		/* static voluem의 인증서가 있는 경우 */
		cert = (st_snicert_t *)service->site->certificate;
	}
	*pcert = &cert->pcrt;
	*pkey = cert->key;
	*pcert_length = cert->pcrt_cnt;
	return 0;
}
#endif

void *
scx_sni_load_keys(struct site_tag * site, const char *CERT_FILE, const char *KEY_FILE)
{
#if GNUTLS_VERSION_MAJOR >= 3
	int ret;
	gnutls_datum_t data;
	int	cert_max = SNI_PCRT_MAX;
	gnutls_x509_crt_t x509_cert;

	st_snicert_t *cert = NULL;
	cert = mp_alloc(site->mp, sizeof(st_snicert_t));
	if(!cert) {
		goto error_ret;
	}
	ret = gnutls_load_file (CERT_FILE, &data);
	if (ret < 0)
	{
		TRACE((T_WARN, "Error loading certificate file %s. reason : '%s'(%d)\n", CERT_FILE, gnutls_strerror(ret), ret));
		goto error_ret;
	}

	ret = gnutls_pcert_list_import_x509_raw(&cert->pcrt, &cert_max, &data, GNUTLS_X509_FMT_PEM, 0);
	cert->pcrt_cnt = cert_max;
	if (ret < 0)
	{
		TRACE((T_WARN, "Error import certificate file %s. reason : '%s'(%d)\n", CERT_FILE, gnutls_strerror(ret), ret));
		goto error_ret;
	}
	gnutls_free (data.data);

	ret = gnutls_load_file (KEY_FILE, &data);
	if (ret < 0)
	{
		TRACE((T_WARN, "Error loading key file %s. reason : '%s'(%d)\n", KEY_FILE, gnutls_strerror(ret), ret));
		goto error_ret;
	}
	gnutls_privkey_init (&cert->key);
	ret = gnutls_privkey_import_x509_raw (cert->key,
									&data, GNUTLS_X509_FMT_PEM,
									NULL, 0);
	if (ret < 0)
	{
		TRACE((T_WARN, "Error import key file  %s. reason : '%s'(%d)\n", KEY_FILE, gnutls_strerror(ret), ret));
		goto error_ret;
	}
	gnutls_free (data.data);
	return cert;
error_ret :
#endif
	return NULL;
}


int
scx_sni_unload_keys(void * pcert)
{
	if(!pcert) return 0;
#if GNUTLS_VERSION_MAJOR >= 3
	st_snicert_t *cert = (st_snicert_t *)pcert;
	for(int i = 0; i < cert->pcrt_cnt; i++) {
		gnutls_pcert_deinit(&cert->pcrt[i]);
	}
	gnutls_privkey_deinit(cert->key);

#endif
	return 0;
}

static void *
scx_check_certitificate(site_t * site, scx_config_t * config,  int mode)
{
	char 			*fpath_crt = NULL;
	char 			*fpath_key = NULL;
	int 			r;
	struct stat 	s;
	int 			fd;
	size_t 			crt_sz;
	size_t 			key_sz;
	st_cert_t 		*cert = NULL;;
	/* cert 파일과  private key 파일은  root 소유여야 하고 쓰기와 실행 권한도 root만 있어야 한다. */
	fpath_crt = vs_data(config->certificate_crt_path);
	fpath_key = vs_data(config->certificate_key_path);
	if (r = access(fpath_crt, mode)) {
		TRACE((T_ERROR, "certificate '%s' fail(%s)\n", fpath_crt, strerror(errno)));
		return NULL;
	}
	stat(fpath_crt, &s);
	if (s.st_uid != 0 || s.st_gid  != 0) {
		TRACE((T_ERROR, "certificate '%s' isn't root ownership\n", fpath_crt));
		return NULL;
	}

	if (s.st_mode & (S_IWGRP|S_IXGRP| S_IWOTH| S_IXOTH) != 0) {
		TRACE((T_ERROR, "certificate '%s' must have not excute,write permission except owner\n", fpath_crt));
		return NULL;
	}
	if ((s.st_mode & S_IFREG) == 0) {
		TRACE((T_ERROR, "certificate '%s' isn't a regular file\n", fpath_crt));
		return NULL;
	}
	crt_sz = s.st_size;

	if (r = access(fpath_key, mode)) {
		TRACE((T_ERROR, "certificate '%s' fail(%s)\n", fpath_key, strerror(errno)));
		return NULL;
	}
	stat(fpath_key, &s);
	if (s.st_uid != 0 || s.st_gid  != 0) {
		TRACE((T_ERROR, "certificate '%s' isn't root ownership\n", fpath_key));
		return NULL;
	}
	if (s.st_mode & (S_IWGRP|S_IXGRP| S_IWOTH| S_IXOTH) != 0) {
		TRACE((T_ERROR, "certificate '%s' must have not excute,write permission except owner\n", fpath_key));
		return NULL;
	}
	if ((s.st_mode & S_IFREG) == 0){
		TRACE((T_ERROR, "certificate '%s' isn't a regular file\n", fpath_key));
		return NULL;
	}
	key_sz = s.st_size;

	//open이나 read 실패시 gscx__certificate_key와 gscx__certificate_crt를 모두 NULL 만든다.
	if (r == 0) {
		cert = mp_alloc(site->mp, sizeof(st_cert_t));

		size_t 	rd = 0;
		fd = open(fpath_crt, O_RDONLY);
		if(fd < 0 ) {
			TRACE((T_ERROR, "certificate '%s' fail to open,(%s)\n", fpath_crt, strerror(errno)));
			return NULL;
		}
		cert->crt = mp_alloc(site->mp, crt_sz);
		rd = read(fd, (char *)cert->crt, crt_sz);
		if(rd <= 0) {
			close(fd);
			TRACE((T_ERROR, "certificate '%s' fail to read,(%s)\n", fpath_crt, strerror(errno)));
			return NULL;
		}
		close(fd);

		fd = open(fpath_key, O_RDONLY);
		if(fd < 0 ) {
			TRACE((T_ERROR, "certificate '%s' fail to open,(%s)\n", fpath_key, strerror(errno)));
			return NULL;
		}
		cert->key = mp_alloc(site->mp, key_sz);
		rd = read(fd, (char *)cert->key, key_sz);
		if(rd <= 0) {
			close(fd);
			TRACE((T_ERROR, "certificate '%s' fail to read,(%s)\n", fpath_key, strerror(errno)));
			return NULL;
		}
		close(fd);
	}
	return cert;
}

static char *
nc_find_mime(const char *path)
{
	int 		len;
	int 		j;

	j = len = strlen(path);


	while (path[j] != '.' && j >= 0) j--;

	if (j == 0) return NULL;

	return mm_lookup(path + j + 1);
}
static int
scx_handle_delete(const char *upload_data, size_t * upload_data_size, nc_request_t *req)
{
	int						ret = MHD_YES, enu, ncr;
	char 					dzbuf[128];
	nc_xtra_options_t 		*result = NULL;
	scx_codepage_t 			*page;
	struct MHD_Connection 	*connection = req->connection;

	TRC_BEGIN((__func__));
	if (req->async_ctx.phase == SRP_INIT) {
		/* first-phase call : client요청에 대한 첫번째 호출, 헤더 정보를 모두 수신했을 때 호출*/
		ret = scx_process_request(req);
		if(ret != MHD_HTTP_CONTINUE) {
			TRC_END;
			return ret;
		}
		hd_dump_property(vs_data(req->ori_url), req->options, req);
		TRC_END;
		return MHD_YES;

	}
		/*
	 	 * DELETE 요청 수신 완료
	 	 */
//	req->t_req_lba = sx_get_time();
		/* second-phase call : request 내의 모든 값을 수신 완료 */

	hd_dump_property(vs_data(req->ori_url), req->options, req);
	ncr = nc_unlink(req->service->core->mnt, vs_data(req->ori_url), req->options, &result);
	hd_dump_property(vs_data(req->ori_url), result, req);
	req->resultcode = kv_get_raw_code(result);
	page = scx_codepage(req->resultcode);
	nce_create_response_from_buffer(req, page->len, (void *) page->page);
	kv_for_each(result, nce_inject_property, req);
	nce_add_basic_header(req, req->response);
	ret = scx_finish_response(req);
	TRC_END;
	return ret;
}

/* 비동기 처리가 가능한 method들을 모두 동기 방식으로 바꾼다 */
static void
scx_disable_async_method()
{
	int 		i;
	for (i = 0; i < __method_map_count; i++) {
		__method_map[i].async = 0;
	}
	gscx__enable_async = 0;
}

static void
scx_init_method_map()
{
	ENTRY 		it;
	ENTRY 		*found = NULL;
	int 		i;

	hcreate_r(__method_map_count, &__method_table);
	for (i = 0; i < __method_map_count; i++) {
		found = NULL;
		if (__method_map[i].method_str[0] != 0) {
			//it.key 	= SCX_STRDUP(__method_map[i].method_str);
			it.key 	= __method_map[i].method_str;
			it.data = (void *)&__method_map[i];
			hsearch_r(it, ENTER, &found, &__method_table);
		}
	}
#ifdef ZIPPER /* streaming 일때는 다음의 method는 인식 하지 않아야 한다. */
	scx_desable_method(MHD_HTTP_METHOD_POST);
	scx_desable_method(MHD_HTTP_METHOD_PUT);
	scx_desable_method(MHD_HTTP_METHOD_DELETE);
//	scx_desable_method(MHD_HTTP_METHOD_OPTIONS);
#else
	/* netcache Core 3.0에서 더이상 PUT method를 지원하지 않는다. */
	scx_desable_method(MHD_HTTP_METHOD_PUT);
	scx_desable_method(MHD_HTTP_METHOD_DELETE);
#endif
	//qsort(__method_map, __method_map_count, sizeof(struct method_map_tag), nc_method_compare);	
}

static void
scx_deinit_method_map()
{
	hdestroy_r(&__method_table);
}

static nc_method_t
scx_find_method(const char *method)
{
	ENTRY 					query = {(char *)method, NULL};
	ENTRY 					*found = NULL;
	struct method_map_tag 	*mr;
	nc_method_t 			method_id;



	if (!method || method[0] == '\0') return HR_UNKNOWN;

    hsearch_r(query, FIND, &found, &__method_table);
	if (!found)
		method_id = HR_UNKNOWN;
	else {
		mr  = (struct method_map_tag *)found->data;
		method_id = mr->method_id;
	}
	return method_id;
}

static void
scx_desable_method(const char *method)
{
	nc_method_t 			method_id;
	method_id = scx_find_method(method);
	if (HR_UNKNOWN != method_id) {
		__method_map[method_id].method_proc = scx_handle_unknown;
	}
}

int
nce_handle_error(int code, nc_request_t *req)
{
	int 					ret;
	scx_codepage_t 			*page;

	req->resultcode = code;


	page = scx_codepage(code);
	nce_create_response_from_buffer(req, page->len, (void *) page->page);
	if (req->response) {
		nce_add_basic_header(req,  req->response);
		ret = scx_finish_response(req);
		return ret;
	}
	return MHD_NO;
}

int
scx_make_response(nc_request_t *req)
{

	int 					ret;
	scx_codepage_t 			*page;
	struct MHD_Connection *connection = req->connection;

	if(req->scx_res.body != NULL) {
		nce_create_response_from_buffer(req, req->scx_res.body_len, (void *) req->scx_res.body);
	}
	else if (300 <= req->scx_res.code) {
		page = scx_codepage(req->scx_res.code);
		nce_create_response_from_buffer(req, page->len, (void *) page->page);
	}
	else {
		nce_create_response_from_buffer(req, 0, NULL);
	}

	if (req->response)
		nce_add_basic_header(req, req->response);


	kv_for_each(req->scx_res.headers, scx_update_resp_header, req);
	req->resultcode = req->scx_res.code; //access log에 반영 되기 위해  resultcode에 값을 넣어준다.
	ret = scx_finish_response(req);

	return ret;
}

int
scx_finish_response(struct request_tag *req)
{
	int ret = MHD_NO;
	ASSERT(req);
	if (req->response) {
		if(req->service) {
			site_run_trigger_client_response(req);
		}

		/*
		 * 여기에서 lua phase handler가 돌면 됨
		 */
		ret = MHD_queue_response(req->connection, req->resultcode, req->response);
		if(MHD_YES != ret) {
			TRACE((T_INFO, "[%llu] MHD_queue_response() failed(%d).\n", req->id, ret));
			req->step = STEP_REQUEST_DONE; /* 여기의 리턴값을 MHD 라이브러리에 알려줄 방법이 없어서 req->step에 설정 */
			shm_log_update_step(req);
			return ret;
		}

		MHD_get_response_headers(req->response, scx_sum_response_header_size, req);
		MHD_destroy_response(req->response);
		req->response = NULL;
	}
	req->t_res_fbs = sx_get_time();		/* 이 시점을 response header 전송시작 시간(first byte response time)으로 한다. */
	return ret;
}

static vstring_t *nce_get_ip_str(const struct sockaddr *sa, vstring_t *s, size_t maxlen)
{
    switch(sa->sa_family) {
        case AF_INET:
            inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), vs_data(s), maxlen);
			vs_update_length(s, -1);
            break;

        case AF_INET6:
            inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), vs_data(s), maxlen);
			vs_update_length(s, -1);
            break;

        default:
            strncpy(vs_data(s), "Unknown AF", maxlen);
			vs_update_length(s, -1);
            return NULL;
    }

    return s;
}

/*
 * 요청이 들어온 포트에 허용된 method인지 검사하는 함수
 * Managment port로 들어온 경우 Purge, Preload, OpenApi만 허용된다.
 * HTTP와 HTTPS, TProxy로 들어 온 경우는 현재는 Purge와, Preload, OpenApi도 허용 되지만
 * 추후에는 이 기능들은 모두 Management Port에서만 가능 하도록 한다.
 */
static int
scx_check_method_authority(nc_request_t *req)
{

	int 					ret = SCX_YES;
	/*
	 * Management port로 요청이 들어온 경우는
	 * Purge, Preload method와 OpenApi 만 허용이 된다.
	 * OpenApi 요청의 경우 아직 확실하게 정의 된것이 없어서 1차 개발에서는 제외 한다.
	 */
	if (SCX_MANAGEMENT == req->connect_type) {
		switch(req->method) {
		case HR_PURGE:
		case HR_PURGEALL:
		case HR_PRELOAD:
		case HR_STAT:
			break;
		default:
			if (req->isopenapi == 1) break;
			req->p1_error = MHD_HTTP_UNAUTHORIZED;
			ret = SCX_NO;
			break;
		}
	}
	else if(SCX_HTTPS == req->connect_type ||
			SCX_HTTP == req->connect_type||
			SCX_HTTP_TPROXY == req->connect_type) {
		/* 서비스 port로 Purge와 Preload, openapi 요청이 들어 오는 경우 차단한다. */
		switch(req->method) {
		case HR_PURGE:
		case HR_PURGEALL:
		case HR_PRELOAD:
		case HR_STAT:
			/* management_enable_with_service_port가 1로 설정 된 경우에는 service port로도 managemnt 명령이 들어 올수 있다 */
			if (gscx__config->management_enable_with_service_port != 1) {
				req->p1_error = MHD_HTTP_UNAUTHORIZED;
				ret = SCX_NO;
			}
			break;
		default:
			break;
		}
	}

	return ret;
}

/*
 * FHS 도메인으로 들어온 요청에 대해 request url에 포함된 첫번째 디렉토리로 볼륨을 찾아서 리턴한다.
 */
int
scx_redirect_fhsdomain_to_volume(nc_request_t *req)
{

	int 		ret = SCX_YES;
	vstring_t 	*volume_name = NULL;
	vstring_t 	*volume_domain = NULL;
	int			start_off = 0;
	int			end_off = 0;
	int			modi_url_len = 0;
	service_info_t	*find_service = NULL;

	if (req->service->cdn_domain_postfix == NULL) {
		/* FHS 도메인 기능을 사용하기 위해서는 default.conf에 cdn_domain_postfix가 필수적으로 설정 되어 있어야 한다. */
		TRACE((T_WARN, "[%llu] %s is not defined. host(%s)\n", req->id, SV_CDN_DOMAIN_POSTFIX, vs_data(req->host)));
		return SCX_NO;
	}
	if (*vs_data(req->ori_url) == '/') {
		/* url이 /로 시작하는 경우 /는 제외 한다. */
		start_off = 1;
	}
	/* 첫번째 디렉토리를 추출 */
	end_off = vs_pickup_token(req->pool, req->ori_url, start_off, "/", &volume_name); /* 여기서는 v_token과 DIRECTION_RIGHT은 의미 없이 형식만 맞춰주기 위함임 */
	if (volume_name == NULL || end_off >= vs_length(req->ori_url) ) {//end_off이 ori_url보다 큰 경우는 요청 url에 디렉토리가 포함되지 않은 경우이다.
		scx_error_log(req, "%s first directory not found. url(%s)\n", __func__, vs_data(req->ori_url));
		return SCX_NO;
	}
//	printf("token = %s, off = %d, url len = %d\n", vs_data(volume_name), end_off, vs_length(req->ori_url));


	volume_domain = vs_allocate(req->pool, vs_length(volume_name) + vs_length(req->service->cdn_domain_postfix) + 2, NULL, 1);
	snprintf(vs_data(volume_domain), vs_remained(volume_domain), "%s.%s", vs_data(volume_name), vs_data(req->service->cdn_domain_postfix));

//	printf("volume domain = %s\n", vs_data(volume_domain));

	/*  vm_add()를 해서 정상적인 볼륨인지 확인 */
	find_service = vm_add(vs_data(volume_domain), 0);
	if (find_service == NULL) {
		scx_error_log(req, "%s virtual host(%s) not found. url(%s)\n", __func__, vs_data(volume_domain), vs_data(req->ori_url));
		return SCX_NO;
	}

	/* req->ori_url를 첫번째 디렉토리를 뺀 경로로 변경한다. */
	req->ori_url = vs_allocate(req->pool, 0, vs_data(req->ori_url)+end_off-1, 0);
	/* 첫번째 디렉토리가 url 인코딩 되는 경우는 없기 때문에 req->ori_url와 동일한 기준으로 req->url의 첫번째 경로도 제거 한다. */
	req->url = vs_allocate(req->pool, 0, vs_data(req->url)+end_off-1, 0);
	/* req->path 경로도 업데이트 한다. */
	scx_make_path_to_url(req);

//	printf("edited url len(%d) %s\n", vs_length(req->ori_url), vs_data(req->ori_url) );
	/* req->host도 업데이트 한다. */
	req->host = volume_domain;
	/* FHS 도메인 볼륨을 해제하고 새로 찾은 볼륨을 할당한다. */
	scx_update_volume_service_end_stat(req);
	req->service = find_service;
	scx_update_volume_service_start_stat(req);

	return ret;
}

/*
 * 서비스 도메인으로 들어온 요청에 대해 request url에 포함된 첫번째 디렉토리로 볼륨을 찾아서 리턴한다.
 */
int
scx_redirect_servicedomain_to_volume(nc_request_t *req)
{

	int 		ret = SCX_YES;
	vstring_t 	*volume_name = NULL;
	char	 	*volume_domain = NULL;
	int			start_off = 0;
	int			end_off = 0;
	int			modi_url_len = 0;
	service_info_t	*find_service = NULL;

	/* CORS 요청인지 확인 한다. */
	strm_check_policy_file(req);
	if (req->streaming != NULL) {
		/* CORS 요청인 경우 대표 도메인 기능을 확인하지 않고 바로 리턴한다. */
		return SCX_YES;
	}

	if (*vs_data(req->ori_url) == '/') {
		/* url이 /로 시작하는 경우 /는 제외 한다. */
		start_off = 1;
	}
	/* 첫번째 디렉토리를 추출 */
	end_off = vs_pickup_token(req->pool, req->ori_url, start_off, "/", &volume_name); /* 여기서는 v_token과 DIRECTION_RIGHT은 의미 없이 형식만 맞춰주기 위함임 */
	if (volume_name == NULL || end_off >= vs_length(req->ori_url) ) {//end_off이 ori_url보다 큰 경우는 요청 url에 디렉토리가 포함되지 않은 경우이다.
		scx_error_log(req, "%s first directory not found. url(%s)\n", __func__, vs_data(req->ori_url));
		return SCX_NO;
	}
//	printf("token = %s, off = %d, url len = %d\n", vs_data(volume_name), end_off, vs_length(req->ori_url));


	volume_domain = vm_find_voldomain_from_sub_domain_list(req, vs_data(volume_name));
	if (volume_domain == NULL) {
		scx_error_log(req, "%s domain name(%s) not found from sub domain list. url(%s)\n", __func__, vs_data(volume_name), vs_data(req->ori_url));
		return SCX_NO;
	}

	/*  vm_add()를 해서 정상적인 볼륨인지 확인 */
	find_service = vm_add(volume_domain, 0);
	if (find_service == NULL) {
		scx_error_log(req, "%s virtual host(%s) not found. url(%s)\n", __func__, volume_domain, vs_data(req->ori_url));
		return SCX_NO;
	}
	if (req->service->remove_dir_level >= 1) {
		/* req->ori_url를 첫번째 디렉토리를 뺀 경로로 변경한다. */
		req->ori_url = vs_allocate(req->pool, 0, vs_data(req->ori_url)+end_off-1, 0);
		/* 첫번째 디렉토리가 url 인코딩 되는 경우는 없기 때문에 req->ori_url와 동일한 기준으로 req->url의 첫번째 경로도 제거 한다. */
		req->url = vs_allocate(req->pool, 0, vs_data(req->url)+end_off-1, 0);
		/* req->path 경로도 업데이트 한다. */
		scx_make_path_to_url(req);
	}


//	printf("edited url len(%d) %s\n", vs_length(req->ori_url), vs_data(req->ori_url) );
	/* req->host도 업데이트 한다. */
	req->host = vs_allocate(req->pool, strlen(volume_domain), volume_domain, 0);
	/* 대표 도메인 볼륨을 해제하고 새로 찾은 볼륨을 할당한다. */
	scx_update_volume_service_end_stat(req);
	req->service = find_service;
	scx_update_volume_service_start_stat(req);

	return ret;
}

static int
scx_request_handler(void *cls,
		 struct MHD_Connection *connection,
		 const char *url,
		 const char *method,
		 const char *version,
		 const char *upload_data,
		 size_t * upload_data_size,
		 void **ptr)
{
	static int aptr;
	int ret;
	nc_file_handle_t 	*file;
	nc_request_t		*req = (nc_request_t *)*ptr;
	nc_stat_t buf;
	nc_method_t 		m = HR_UNKNOWN;
	int 				code;
	int 				res = MHD_YES;
	char 				*host_val = NULL;
	char				*start_port_val = NULL;
	int					length = 0;
	vstring_t			*url_val = NULL;
	gnutls_session_t 	*tls_session = NULL;
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};

	if (!gscx__service_available) {	/* shutdown 과정에서 들어오는 요청의 경우 모두 에러를 리턴한다. */
		TRACE((T_DAEMON, "[%llu] scx_request_handler request rejected. (reason : shutdown progress)\n", req->id));
		return MHD_NO;
	}
	if (req->step == STEP_REQUEST_DONE) {
		/*
		 * scx_request_handler()가 callback 되기 전에 client의 연결이 끊긴 경우 주로 발생하고
		 * 이때 scx_finish_response()에서 MHD_queue_response() 호출시 MHD_NO가 리턴되는데
		 * 정상적인 경우라면 그 상태에서 더이상 scx_request_handler()가 callback 되지 않아야 하지만
		 * 어쩌다가 호출 되는 경우가 있음
		 * 이 경우 예외 처리를 위해 이 부분을 추가함
		 * https://jarvis.solbox.com/redmine/issues/33172
		 */
		TRACE((T_INFO, "[%llu] Unexpected closed connection callback\n", req->id));
		return MHD_NO;
	}
#ifdef DEBUG
	int *argnum = NULL;
	pthread_t 	tid;

	/* signal 대신 url로 reload 할 경우 사용 */
	if (vs_strcmp_lg(req->url,"/reload") == 0 &&  SCX_MANAGEMENT == req->connect_type) {
		argnum = SCX_CALLOC(1, sizeof(int));
		*argnum = SIGUSR1;
		pthread_create(&tid, NULL, signal_handler_thread, (void *) argnum);
		pthread_detach(tid);
		req->p1_error = 200;
		req->connection = connection;
		ret = nce_handle_error(req->p1_error, req);
		return ret;
	}
#endif
#if 0
/*
 * MHD는 현재 RFC규격상의 <scheme>을 포함하는 FULL URI를 전달하고 있지 않음
 */
	if (!scx_valid_uri(uri)) {
		struct MHD_Response *response = NULL;
		req->connection = connection;
		TRACE((T_ERROR, "URI[%s] - invalid\n", url));
		ret = nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
		TRC_END;
		return ret;
	}
#endif
//	printf("call %s(), is_suspeneded(%d), phase(%d)\n", __func__, req->is_suspeneded, req->async_ctx.phase);

#if 1
	/* 아래에서 비동기인 경우 첫번째는 바로 리턴하고 이후 비동기 처리를 했기때문에 suspend 상태에서 callback이 오지 않는다고 가정한다. */
	ASSERT(1 != req->is_suspeneded);
#else
	if (1 == req->is_suspeneded) {
		/* thread에서 처리중인 경우에는 바로 리턴 한다 */
		return MHD_YES;
	}
#endif
	if (req->p1_error != 0) {
		req->connection = connection;
		ret = nce_handle_error(req->p1_error, req);
		return ret;
	}
	TRACE((T_DAEMON, "[%llu] scx_request_handler URL[%s], phase = %d, upload size = %ld, %s\n",
			req->id, vs_data(req->url),  req->async_ctx.phase, (long)*upload_data_size, upload_data == '\0'?"NULL":"full" ));
	if (SRP_INIT == req->async_ctx.phase || SRP_RETRY == req->async_ctx.phase) {	/* 1st-phase */

		/* 비동기 처리하는 요청의 경우 일단 한번 리턴한후에 이후 과정(suspend)를 한다.
		 * 이렇게 하지 않고 처음 부터 suspend를 하는 경우 MHD 내부에서 동기화 문제로 죽는 현상 발생 */
		if (SRP_INIT == req->async_ctx.phase) {
			/*
			 * Method가 없거나 두글자 이하인 경우는 비정상 요청으로 보고 Response를 보내지 않고 연결을 끊는다.
			 * 이 경우 response code가 없기 때문에 access 로그에 기록이 안됨
			 */
			if (method == NULL) {
				/* 비정상 요청으로 보고 응답도 하지 않는다. */
				scx_error_log(req, "Request Method is NULL, URL(%s)\n", vs_data(req->url));
				return MHD_NO;
			}
			else if (strlen(method) <= 2) {
				scx_error_log(req, "Invalid Method(%s), URL(%s)\n", method, vs_data(req->url));
				return MHD_NO;
			}
			req->method = scx_find_method(method);
			if (1 == __method_map[req->method].async) {
				/* 비동기 method인 경우 바로 리턴함 */
				req->async_ctx.phase = SRP_RETRY;
				return MHD_YES;
			}
		}
		else {
			req->async_ctx.phase = SRP_INIT;
		}
		req->connection = connection;
		req->method = scx_find_method(method);
		/* complete phase에서 MHD connection이 close된 뒤에도 사용을 해야 해서 복사 방식으로 변경 */
		req->zmethod = vs_allocate(req->pool, strlen(method), method, 1);
		req->version = vs_allocate(req->pool, strlen(version), version, 1);
		req->req_hdr_size += strlen(method) + strlen(version); /* 요청 method와 HTTP version string도 트래픽에 반영한다. */
		req->t_req_lba = sx_get_time();
		/*
		 * Host 태그 처리를 가장먼저 실행하도록 변경
		 * host태그를 분석하여, request가 요청하는 URL이 속한 content 볼륨을 확인해야
		 * content volume의 설정값에 따른 응답처리 액션을 실행할 수 있음
		 * 2013.06.09 by weon
		 */


		if (req->host) {
			/* absoluteURI인 경우 헤더에 있는 host tag를 무시하고 uri에 포함된 host 정보를 사용한다.
			 * absoluteURI인 경우 req->host에는 port가 제거된   host만 들어간다.*/
			//host_val = vs_data(req->host);
		}
		else {
			host_val = (char *)MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_HOST);
			if(!host_val) {
				scx_error_log(req, "Host invalid, Method(%s), URL(%s)\n", method, vs_data(req->url));
				ret = nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
				return ret;
			}
			/*
			 * HOST 헤더에 port가 포함된 경우 port는 제거한다.
			 */
			start_port_val =  strchr(host_val, ':');
			if (start_port_val) {
				host_val[start_port_val - host_val] = '\0'; 	//직접 host_val을 수정하는것 만으로도 MHD의 header가 수정된다.
			}
			if (strlen(host_val) >= gscx__max_header_val_len) {
				scx_error_log(req, "URL[%s]: Host '%s' Too Large\n", vs_data(req->url),host_val);
				ret = nce_handle_error(MHD_HTTP_REQUEST_ENTITY_TOO_LARGE, req);
				return ret;
			}
			req->host = vs_allocate(req->pool, strlen(host_val), host_val, 1);
		}
		if (req->host == NULL || vs_length(req->host) <= 1) { /* host가 1자리 이하일때는 잘못된것이라고 판단한다. */
			scx_error_log(req, "Host invalid, Method(%s), URL(%s)\n", method, vs_data(req->url));
			ret = nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
			return ret;
		}
		TRACE((T_DAEMON, "[%llu] Host[%s], Method[%s], URL[%s]\n", req->id, vs_data(req->host), method, vs_data(req->url) ));
		if (SCX_YES != nc_build_request(req)) {
			scx_error_log(req, "URI[%s] - invalid\n", url);
			ret = nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
			return ret;
		}
		if(vs_length(req->url) == 0) {
			/* url 길이가 0인 경우 에러 처리한다. */
			scx_error_log(NULL, "[%llu] Empty URI, Host[%s], Method[%s]\n", req->id, vs_data(req->host), method);
			ret = nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
			return ret;
		}
		phase_ctx.req = (void *)req;
		site_run_host_rewrite(req); /* 이 단계에서만 host(volume)의 변경이 적용된다. */
		if (req->scx_res.trigger == LUA_TRIGGER_RETURN){
			return scx_make_response(req);
		}

		/************************** Host Rewrite Phase Handler가 실행 되는 부분 *************************************/
		if (SCX_YES != scx_phase_handler(PHASE_HOST_REWRITE, &phase_ctx)) {
			if(!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
			return MHD_YES;
		}
		if (rsapi_is_local_object(req) == 1) {
			/* openapi 인 경우에는 others 볼륨으로 강제로 선택한다. */
			req->host = vs_strdup_lg(req->pool, OTHERS_VOLUME_NAME);
		}
		else {
#ifdef ZIPPER
			strm_check_policy_file(req);	/* 여기서는 에러가 난다고 해도 다음과정을 진행하는게 정상임 */
			/* strm_host_parser()를 통해서 req->host 정보를  변경한다. */
			if ( strm_host_parser(req) == 0) {
				scx_error_log(req, "Host parse failed. URL(%s)\n",  vs_data(req->ori_url));
				ret = nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
				return ret;
			}
#endif
			if (scx_is_valid_host(req) == 0) {	/* streaming 기능 사용시에는 host에 대한 검사가 필요 없음*/
				/* host가 solproxy 서버의 ip이거나 loopback ip(127.0.0.1)인 경우 400에러를 리턴한다.
				 * recursive connection 방지 목적 */
				scx_error_log(req, "Host invalid, Method(%s), URL(%s)\n", method, vs_data(req->url));
				ret = nce_handle_error(MHD_HTTP_FORBIDDEN, req);
				return ret;
			}
		}
		req->service = vm_add(vs_data(req->host), 0);	//동기화 처리때문에 vm_add() 한번 호출로 합친다.
		if (req->service == NULL) {
			scx_error_log(req, "URL[%s] - virtual host not found in header(host='%s')\n", vs_data(req->url), vs_data(req->host));
			req->p1_error = MHD_HTTP_FORBIDDEN;
			return MHD_YES;
		}

		scx_update_volume_service_start_stat(req);
		/* 여기에서 vol_service_type에 따라 동작이 구분 되어야 한다. */
		/////////////// 대표 or FHS 도메인 기능 시작 ////////////////////////
		if (req->service->vol_service_type == VOL_SERVICE_TYPE_SERVICE) {
			/* 대표 도메인 기능 */
			if (scx_redirect_servicedomain_to_volume(req) != SCX_YES) {
				if(!req->p1_error) req->p1_error = MHD_HTTP_FORBIDDEN;
				return MHD_YES;
			}
		}
		else if (req->service->vol_service_type == VOL_SERVICE_TYPE_FHS) {
			/* FHS 도메인 기능 */
			if (scx_redirect_fhsdomain_to_volume(req) != SCX_YES) {
				if(!req->p1_error) req->p1_error = MHD_HTTP_FORBIDDEN;
				return MHD_YES;
			}
		}
		/////////////// 대표 or FHS 도메인 기능 끝 //////////////////////////

		req->limit_rate 		= req->service->limit_rate;
		req->limit_rate_after 	= req->service->limit_rate_after;
		req->limit_traffic_rate = req->service->limit_traffic_rate;
		req->norandom = req->service->origin_request_type;

		if (UNDEFINED_PROTOCOL != req->service->hls_playlist_protocol) {
			/* service->hls_playlist_protocol이 지정된 경우는 지정된 프로토콜로 변경한다. */
			req->hls_playlist_protocol = req->service->hls_playlist_protocol;
		}
		else {
			/* service->hls_playlist_protocol이 UNDEFINED_PROTOCOL인 경우는 설정이 되지 않은 경우이므로 요청이 들어온 프로토콜은 그대로 사용한다. */
			req->hls_playlist_protocol = req->secured;
		}
		if (UNDEFINED_PROTOCOL != req->service->dash_manifest_protocol) {
			/* service->hls_playlist_protocol이 지정된 경우는 지정된 프로토콜로 변경한다. */
			req->dash_manifest_protocol = req->service->dash_manifest_protocol;
		}
		else {
			/* service->hls_playlist_protocol이 UNDEFINED_PROTOCOL인 경우는 설정이 되지 않은 경우이므로 요청이 들어온 프로토콜은 그대로 사용한다. */
			req->dash_manifest_protocol = req->secured;
		}
#ifdef ZIPPER
		if(req->isopenapi != 1) {
			if ( strm_url_parser(req) == 0) {
				scx_error_log(req, "URL parse failed. URL(%s)\n",  vs_data(req->ori_url));
				if(!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				return MHD_YES;
			}
		}
#else
//#ifndef ZIPPER
		if (req->service->enable_crossdomain == 1) {
			strm_check_policy_file(req);	/* crossdomain 요청인지 확인 한다. */
		}
		/* enable_wowza_url이 1로 설정된 경우 여기서 ori_url을 application/instance를 뺀 경로로 바꿔준다. */
		if(req->service->enable_wowza_url == 1) {
			strm_wowza_url_manage(req);
		}
		if (req->service->hls_modify_manifest_enable == 1) {
			strm_check_manifest(req);
		}
		/* cache mode에서 streaming 기능으로 동작 할때에는 url parsing을 여기서 한다. */
		if (1 == req->service->streaming_enable) {
			strm_url_parser(req);	/* url parsing을 해서 streaming 형식이 아니라도 그냥 진행한다. */
			if (req->streaming == NULL && req->service->streaming_media_ext != NULL) {
				/* 요청 URL에 포함된 확장자가 streaming_media_ext에 지정된 확장자와 동일한 경우 streaming 컨텐츠로 인식하도록 한다. */
				strm_check_streaming_media_ext(req);

			}
			if (req->service->cache_request_enable == 0 && req->streaming == NULL) {
				scx_error_log(req, "Not allowed cache request, url(%s)\n", vs_data(req->ori_url));
				if(!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
				return MHD_YES;
			}
		}
#if 0
		// 2020/12/07 base_path를 netcache에 직접 설정 하도록 수정 하면서 이 부분은 필요없어짐
		/*
		 * streaming 기능으로 동작하는 상태에서 streamig 요청인 경우 strm_make_path()에서 base_path를 붙이는 동작을 하고
		 * streaming url 형태가 아닌 경우 여기서 base_path를 붙이는 동작을 해야한다.
		 * 현재로서는 streaming 요청에 ori_url에 base_path를 추가해도 문제가 없는 걸로 보이기 때문에
		 * streaming 요청이던 일반 요청이던 관계 없이 base_path를 ori_url에 추가 한다.
		 * 2020/11/23 hls manifest 수정 기능 동작시 base_path가 중복적으로 들어가는 문제가 확인 되어서 streaming 요청시 여기서는 ori_url에 base_bath를 추가하지 않도록 수정함
		 */
		if (req->service->base_path && req->streaming == NULL) {
			length = vs_length(req->ori_url) + vs_length(req->service->base_path) + 1;
			url_val = vs_allocate(req->pool, length, NULL, 0);
			snprintf(vs_data(url_val), length, "%s%s", vs_data(req->service->base_path), vs_data(req->ori_url));
			vs_update_length(url_val, strlen(vs_data(url_val)));
			req->ori_url = url_val;
		}
#endif
#endif
		if (req->connect_type == SCX_HTTPS) {
			if (req->service->enable_https != 1) {
				/* https 서비스가 허용되지 않은 경우 */
				TRACE((T_DAEMON, "Not allowed https service '%s'\n", vs_data(req->service->name) ));
				if(!req->p1_error) req->p1_error = MHD_HTTP_FORBIDDEN;
				return MHD_YES;
			}
			tls_session = (gnutls_session_t *)MHD_get_connection_info(req->connection, MHD_CONNECTION_INFO_GNUTLS_SESSION);
			if (tls_session != NULL) {
				/* tls 정보를 tls_info에 저장 */
				req->tls_info = (tls_info_t *)mp_alloc(req->pool, sizeof(tls_info_t));
				req->tls_info->proto_ver = gnutls_protocol_get_version(*tls_session);
				req->tls_info->kx_algorithm = gnutls_kx_get(*tls_session);
				req->tls_info->cipher_algorithm = gnutls_cipher_get(*tls_session);
				req->tls_info->mac_algorithm = gnutls_mac_get(*tls_session);
				TRACE((T_DEBUG, "[%llu] cipher suite = %s, ssl version = %s\n"
						, req->id
						, gnutls_cipher_suite_get_name(req->tls_info->kx_algorithm, req->tls_info->cipher_algorithm, req->tls_info->mac_algorithm)
						, gnutls_protocol_get_name(req->tls_info->proto_ver) ));
			}
		}

		/************************** Volume Lookup Phase Handler가 실행 되는 부분 *************************************/
		if (SCX_YES != scx_phase_handler(PHASE_VOLUME_LOOKUP, &phase_ctx)) {
			if(!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
			return MHD_YES;
		}
		if(req->service->send_timeout) {
			/* Client가 데이터를 받는 도중 pause를 하거나 Client의 네트웍이 갑자기 단절 되거나 했을때 적용되는 timeout을 설정 */
			MHD_set_connection_option(connection, MHD_CONNECTION_OPTION_TIMEOUT, req->service->send_timeout);
		}

		nc_parse_header(req);
		if (req->p1_error != 0) {
			ret = nce_handle_error(req->p1_error, req);
			return ret;
		}
		/* 요청이 들어온 포트별 권한이 맞는지 확인한다. */
		if (SCX_YES != scx_check_method_authority(req)) {
			if(!req->p1_error) req->p1_error = MHD_HTTP_UNAUTHORIZED;
			return MHD_YES;
		}
		/************************** Header Parse Phase Handler가 실행 되는 부분 *************************************/
		if (SCX_YES != scx_phase_handler(PHASE_HEADER_PARSE, &phase_ctx)) {
			if(!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
			return MHD_YES;
		}
		shm_log_update_host(req);
	}
	req->step = STEP_START;
	res = scx_handle_async_method(upload_data, upload_data_size, req);

	return res;
}


int
nce_init_sp_log()
{
	gscx__config->log_dir = scx_get_vstring_lg(gscx__default_site, SV_LOG_DIR, gscx__default_log_path);
	if (NULL == gscx__nc_trace_mask) {
		gscx__config->nc_trace_mask = scx_get_vstring_lg(gscx__default_site, SV_LOG_LEVEL, "info,stat,warn,error");
	}
	else {
		gscx__config->nc_trace_mask = gscx__nc_trace_mask;
	}

	if (scx_check_dir(vs_data(gscx__config->log_dir)) < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		return -1;
	}

	snprintf(gscx__log_file, sizeof(gscx__log_file), "%s/sp.log", vs_data(gscx__config->log_dir));
 	nc_setup_log(gscx__debug_mode, gscx__log_file, 100, 10);
	if (gscx__config->nc_trace_mask)
		trc_set_mask(vs_data(gscx__config->nc_trace_mask));
	return 0;
}

int
nce_init()
{
	int 			i;
	int 			ccf = 1;
	int 			disable = 0;
	char 			fpath[512];
	unsigned int 	intval;
	vstring_t 		*vstrval;
	struct passwd 	*pw;
	int 			ret = 0;
	uint32_t		log_flag = 0;


	/* geoip 핸들러 로딩 */
	gip_load();

	/*
	 * 로그 준비
	 */

	scx_report_environment();

	if (scx_check_cache_dir() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		return -1;
	}

	TRACE((T_INFO, "Public-IP of this system is %s\n", vs_data(gscx__local_ip)));
	if (gscx__cache_part_count > 0) {
		nc_set_param(NC_GLOBAL_CACHE_PATH, 			(void *)vs_data(gscx__cache_part[0]), 	vs_length(gscx__cache_part[0]));
	}
#if 0	
	for (i = 1; i < gscx__cache_part_count; i++) {
		nc_add_partition(vs_data(gscx__cache_part[i]), 100);
	}
#endif
	//intval = scx_get_uint(gscx_default_site, SV_MEMORY_CACHE, 1024);
	nc_set_param(NC_GLOBAL_CACHE_MEM_SIZE, 		(void *)&gscx__config->cache_mem, 		sizeof(gscx__config->cache_mem));

	//intval = scx_get_uint(gscx_default_site, SV_INMEMORY_CACHE, 10000);
#if 0
	nc_set_param(NC_GLOBAL_CACHE_MAX_INODE, 	(void *)&gscx__config->max_inodes, 	sizeof(gscx__config->max_inodes));
#else
	nc_set_param(NC_GLOBAL_INODE_MEMSIZE, 	(void *)&gscx__config->inode_mem, 	sizeof(gscx__config->inode_mem));
#endif
	nc_set_param(NC_GLOBAL_ENABLE_FASTCRC,		(void *)&gscx__config->fastcrc,		sizeof(gscx__config->fastcrc));
	nc_set_param(NC_GLOBAL_ENABLE_FORCECLOSE,	(void *)&gscx__config->force_close,	sizeof(gscx__config->force_close));
	nc_set_param(NC_GLOBAL_READAHEAD_MB, 		(void *)&gscx__config->ra_mb, 	sizeof(gscx__config->ra_mb));
	nc_set_param(NC_GLOBAL_CACHE_RA_MINTHREADS, (void *)&gscx__min_rapool, 	sizeof(gscx__min_rapool));
	nc_set_param(NC_GLOBAL_CACHE_RA_MAXTHREADS, (void *)&gscx__max_rapool, 	sizeof(gscx__max_rapool));
	nc_set_param(NC_GLOBAL_CACHE_POSITIVE_TTL, 	(void *)&gscx__config->positive_ttl,sizeof(gscx__config->positive_ttl));
	nc_set_param(NC_GLOBAL_CACHE_NEGATIVE_TTL, 	(void *)&gscx__config->negative_ttl,sizeof(gscx__config->negative_ttl));
	nc_set_param(NC_GLOBAL_CACHE_DISK_HIGH_WM,	(void *)&gscx__config->disk_high_wm,sizeof(gscx__config->disk_high_wm));
	nc_set_param(NC_GLOBAL_CACHE_DISK_LOW_WM,	(void *)&gscx__config->disk_low_wm,	sizeof(gscx__config->disk_low_wm));
	nc_set_param(NC_GLOBAL_MAX_PATHLOCK,		(void *)&gscx__config->max_path_lock_size,	sizeof(gscx__config->max_path_lock_size));
	nc_set_param(NC_GLOBAL_ENABLE_COMPACTION, 	(void *)&ccf, 				sizeof(ccf));
	nc_set_param(NC_GLOBAL_CACHE_BLOCK_SIZE, 	(void *)&gscx__config->block_size, 	sizeof(gscx__config->block_size));
	/*
	 * solproxy는 객체의 속성을 공유 메모리로 백업할 필요가 없음
	 */
	nc_set_param(NC_GLOBAL_BACKUP_PROPERTYCACHE,(void *)&disable, 	sizeof(disable));
	nc_set_param(NC_GLOBAL_STAT_BACKUP,			(void *)&disable, 	sizeof(disable));	/* 볼륨별 메모리(가상메모리) 사용량 감소 목적 */
	if (gscx__config->cluster_ip) {
		nc_set_param(NC_GLOBAL_CLUSTER_IP, 			(void *)vs_data(gscx__config->cluster_ip), 	vs_length(gscx__config->cluster_ip));
		nc_set_param(NC_GLOBAL_CLUSTER_TTL,			(void *)&gscx__config->cluster_ttl,	sizeof(gscx__config->cluster_ttl));
	}
	if (gscx__local_ip)
		nc_set_param(NC_GLOBAL_LOCAL_IP, 			(void *)vs_data(gscx__local_ip), 		vs_length(gscx__local_ip));
	if (gscx__config->cold_cache_enable) {
		nc_set_param(NC_GLOBAL_ENABLE_COLD_CACHING,	(void *)&gscx__config->cold_cache_enable,	sizeof(gscx__config->cold_cache_enable));
	}
	if (gscx__config->cold_cache_ratio) {
		nc_set_param(NC_GLOBAL_COLD_RATIO,			(void *)&gscx__config->cold_cache_ratio,	sizeof(gscx__config->cold_cache_ratio));
	}
	for (i = 0; __driver_dict[i].class[0] != 0; i++) {
		//sprintf(fpath, "%s%s", vs_data(gscx__config_root), __driver_dict[i].path1);
		snprintf(fpath, sizeof(fpath), "%s",  __driver_dict[i].path1);
		if (nc_load_driver(__driver_dict[i].class, fpath) < 0) {
			TRACE((T_INFO|T_ERROR, "The loadable driver, '%s' - found error in loading\n", fpath));
			if (__driver_dict[i].path2[0]) {
				//sprintf(fpath, "%s%s", vs_data(gscx__config_root), __driver_dict[i].path2);
				snprintf(fpath, sizeof(fpath), "%s", __driver_dict[i].path2);
				if (nc_load_driver(__driver_dict[i].class, fpath) < 0) {
					TRACE((T_INFO|T_ERROR, "The loadable driver, '%s' - found error in loading\n", fpath));
					syslog(LOG_ERR|LOG_PID, "error in loading driver, '%s'\n", fpath);
					gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
					return -1;
				}
				else {
					TRACE((T_INFO, "The loadable driver, '%s' - loaded at the second trial\n", fpath));
				}
			}
			else {
				TRACE((T_INFO|T_ERROR, "error in loading driver, '%s'\n", fpath));
				syslog(LOG_ERR|LOG_PID, "error in loading driver, '%s'\n", fpath);
				gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
				return -1;
			}
		}
	}
	pw = getpwnam(vs_data(gscx__config->real_user));
	if (!pw) {
		TRACE((T_ERROR, "real user id for '%s' not found\n", vs_data(gscx__config->real_user)));
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		return -1;
	}
	else {
		gscx__real_uid = pw->pw_uid;
		gscx__real_gid = pw->pw_gid;
	}
	if (0 > scx_change_uid() ) {	/* uid를 solproxy로 변경하는 경우 log나 cache directory 접근에 문제가 되는 경우가 많다.*/
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		return -1;
	}

	gscx__access_log_string[0] = 0;
	gscx__error_log_string[0] = 0;
	gscx__origin_log_string[0] = 0;
	strncpy(gscx__access_log_string,  vs_data(gscx__config->log_dir), sizeof(gscx__access_log_string) - 1);
	strncpy(gscx__error_log_string,  vs_data(gscx__config->log_dir), sizeof(gscx__error_log_string) - 1);
	strncpy(gscx__origin_log_string,  vs_data(gscx__config->log_dir), sizeof(gscx__origin_log_string) - 1);
	/* logrotate와 연동을 하는 경우에는 자체적인 log ratate 기능을 사용하지 않는다. */
	if (1 == gscx__config->logrotate_signal_enable) {
		log_flag = SCX_LOGF_SIGNAL|SCX_LOGF_FILE;
		strncat(gscx__access_log_string, "/access.log", sizeof(gscx__access_log_string) - 1);
		strncat(gscx__error_log_string, "/error.log", sizeof(gscx__error_log_string) - 1);
		strncat(gscx__origin_log_string, "/origin.log", sizeof(gscx__origin_log_string) - 1);
	}
	else
	{
		log_flag = SCX_LOGF_ROTATE|SCX_LOGF_FILE;
		strncat(gscx__access_log_string, "/access_%Y%m%d.log", sizeof(gscx__access_log_string) - 1);
		strncat(gscx__error_log_string, "/error_%Y%m%d.log", sizeof(gscx__error_log_string) - 1);
		strncat(gscx__origin_log_string, "/origin_%Y%m%d.log", sizeof(gscx__origin_log_string) - 1);
	}
	scx_access_logger 	= logger_open(gscx__access_log_string, O_MEGA(gscx__config->log_size), log_flag, 100);
	scx_error_logger 	= logger_open(gscx__error_log_string, O_MEGA(gscx__config->log_size), log_flag, 100);
	scx_origin_logger 	= logger_open(gscx__origin_log_string, O_MEGA(gscx__config->log_size), log_flag, 100);

	nce_ignore_signal(); //netcache core에서도 signal 핸들러를 등록하기 때문에 여기서 먼저 선언한다.
	if(nc_init() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		return -1;
	}
	gscx__nc_inited++;
	for (i = 1 ; i < gscx__cache_part_count; i++) {
		nc_add_partition(vs_data(gscx__cache_part[i]), 100);
	}
	vm_init();

	gscx__url_preg = (regex_t *)SCX_MALLOC(sizeof(regex_t));
	if(regcomp(gscx__url_preg, gscx__url_pattern, REG_EXTENDED)) {
		char buf[128];
		regerror(ret,gscx__url_preg, buf, sizeof(buf) );
		TRACE((T_ERROR, "uri regex compile error(%s), %s\n",gscx__url_pattern, buf));
		SCX_FREE(gscx__url_preg);
		gscx__url_preg = NULL;
	}

	return 0;
}

static int
nce_add_file_info(nc_request_t *req, nc_stat_t *buf)
{
	time_t 		utime = buf->st_mtime;
	struct tm 	result;
	struct tm	*gmt_tm = NULL;
	char 		xbuf[128];
	char * hlf_get_hit(nc_request_t *req, char *buf, int size);

	/*
	 * TODO : mime encoding 정보 추가
	 * mime encoding을 추가하기위해서는 mime dictionary 모듈 필요
	 */

	TRACE((T_DAEMON, "[%llu] URL[%s] - mtime[%ld], st_mtime[%ld]\n", req->id, vs_data(req->url), (long long)utime, buf->st_mtime));
	if (req->hi > 0) {	/* cache hit 상태가 지정되지 않은 경우는 X-Cache 헤더를 응답하지 않는다. */
		MHD_add_response_header(req->response, "X-Cache", hlf_get_hit(req, xbuf, sizeof(xbuf)));
	}

#if 0		//kv list에 들어있기 때문에 아래의 ETag,Last-Modified는 추가할 필요가 없다.
	if (buf->st_devid[0] != 0 && isprint(buf->st_devid[0])) {
		/* nc_stat_t 필드에 devid가 유효할때만 etag 추가 */
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_ETAG, buf->st_devid);
		TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "URL[%s], header => ('%s', '%s')\n", vs_data(req->url), MHD_HTTP_HEADER_ETAG, buf->st_devid));
	}
	if (buf->st_mtime != 0) {
		/* nc_stat_t 필드에 vtime이 유효할때만 expires 추가 */
		utime 	= buf->st_mtime;
		gmt_tm 	= localtime_r(&utime, &result);
		if (gmt_tm) {
			strftime(xbuf, sizeof(xbuf), GMT_DATE_FORMAT, gmt_tm);
			MHD_add_response_header(req->response, MHD_HTTP_HEADER_LAST_MODIFIED, xbuf);
			TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "[%llu] URL[%s], header => ('%s', '%s')\n",
					 req->id, vs_data(req->url), MHD_HTTP_HEADER_LAST_MODIFIED, xbuf));
		}
		else {
			TRACE((T_ERROR, "[%llu] URL[%s] - mtime[%ld], gmtime conversion failed(%s)\n",
					req->id, vs_data(req->url), req->objstat.st_mtime, (char *)ctime_r(&utime, xbuf)));
		}
	}
#endif
#if 0
	if (bputlength) {
		sprintf(xbuf, "%lld", buf->st_size);
		TRACE((T_WARN, "content-length: %s\n", xbuf));
		MHD_add_response_header(response, MHD_HTTP_HEADER_EXPIRES, xbuf);
	}
#endif
	return 0;
}

void
nce_create_response_from_buffer(nc_request_t *req, size_t size,  void *buffer)
{
	req->response = MHD_create_response_from_buffer(size,
			(void *) buffer,
			MHD_RESPMEM_PERSISTENT);
	scx_update_res_body_size(req,size);
	req->remained = 0;	/* access 로그 기록 때문에 0으로 설정 */
	TRACE((T_DEBUG, "[%llu] size = %lld'\n", req->id, size));
	return;
}



int
nce_add_basic_header(nc_request_t *req, struct MHD_Response *response)
{
	char 		xbuf[128];
	time_t 		utime;
	struct tm 	result;
	struct tm	*gmt_tm = NULL;
	int 		s = 0;

	utime 	= scx_get_cached_time_sec();
	TRACE((T_DEBUG, "[%llu] called '%s'\n", req->id, __func__));
	gmt_tm 	= gmtime_r(&utime, &result);
	s += strftime(xbuf, sizeof(xbuf), GMT_DATE_FORMAT, gmt_tm);
	s += strlen(MHD_HTTP_HEADER_DATE);
	MHD_add_response_header(response, MHD_HTTP_HEADER_DATE, xbuf);
	s += sprintf(xbuf, "%llu",req->id);
	MHD_add_response_header(req->response, "X-Edge-Request-ID", xbuf);
	if (req == NULL || req->respvia == 0) {
		char 		buff[128];
		vstring_t 	via_str = {sizeof(buff), 0, buff};
		//via_str = vs_allocate(req->pool, 256, NULL, 0);
		scx_update_via(req, &via_str, NULL);
#ifdef ZIPPER
		MHD_add_response_header(response, MHD_HTTP_HEADER_SERVER, vs_data(&via_str));
#else
		if(NULL == req->file) {
			/* origin에서 에러를 받아온 경우가 아니면 via 대신 server 헤더를 준다. */
			//MHD_add_response_header(response, MHD_HTTP_HEADER_SERVER, vs_data(&via_str));
			scx_update_resp_header(MHD_HTTP_HEADER_SERVER, vs_data(&via_str), (void *)req);
		}
		else
			//MHD_add_response_header(response, MHD_HTTP_HEADER_VIA, vs_data(&via_str));
			scx_update_resp_header(MHD_HTTP_HEADER_VIA, vs_data(&via_str), (void *)req);
#endif
	}
	if (req->resultcode == MHD_HTTP_REQUESTED_RANGE_NOT_SATISFIABLE &&
			req->objstat.st_sizedecled) {
		/* Range 요청이 잘못된 경우에 'Content-Range'에 전체 크기를 리턴한다. */
		s += sprintf(xbuf, "bytes */%lld", req->objstat.st_size);
		s += strlen(MHD_HTTP_HEADER_CONTENT_RANGE);
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_CONTENT_RANGE, xbuf);
	}
	if (req->resp_body_compressed == 1) {
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_CONTENT_ENCODING, "gzip");
	}
#if 0
	//response header size 계산 방식 변경으로 이 부분은 더이상 사용하지 않는다.
	if (req)
		req->res_hdr_size += s;
#endif
	req->t_res_sent = scx_update_cached_time_usec();	/* 응답을 보낸시간 */
	return 0;
}


int
scx_add_cors_header(nc_request_t *req)
{

	/* DASH 혹은 java script 플레이어의 경우는 "Access-Control-Allow-Origin: *" response header가 필수 이다 . CORS*/
	MHD_add_response_header(req->response, "Access-Control-Allow-Origin", "*");
	MHD_add_response_header(req->response, "Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
	MHD_add_response_header(req->response, "Access-Control-Allow-Credentials", "true");
//	if (HR_OPTIONS == req->method) {	/* 현재 확인된 바로는 OPTIONS Method 요청에 대해서만 Access-Control-Allow-Headers를 리턴하면 되지만 혹시 몰라서 항상 리턴하도록 함 */
		MHD_add_response_header(req->response, "Access-Control-Allow-Headers", "origin,range");
//	}

	return 0;
}
/*
 * request header size 계산 방식 변경으로 더 이상 이 함수를 사용하지 않는다.
 */
static int
scx_sum_property(const char *key, const char *val, void *cb)
{
	size_t 	*si = (size_t *)cb;

	*si += strlen(key) + strlen(val);
	return 0;
}

static int
scx_sum_response_header_size(void *cr, enum MHD_ValueKind kind, const char *key, const char *value)
{
	nc_request_t 	*req = cr;
	int				key_len = 0, value_len = 0;

	key_len = strlen(key);
	value_len = strlen(value);
	req->res_hdr_size += key_len + value_len + 2; //\r\n 길이 포함
    return MHD_YES;
}

void
nc_request_completed(void *cls,
					struct MHD_Connection *connection,
					void **ctx,
					enum MHD_RequestTerminationCode toe)
{
	int 		sidx = (int)(long long)cls;

	nc_request_t 	*req = *ctx;
	char 			buf[10240];
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};
	int 	len;
	mem_pool_t 		*pool = NULL;

	ASSERT(req);
	ASSERT(req->pool);
	limitrate_clean((void *)req);
#if 0
	/* request header size 계산 방식 변경으로 더 이상 이 부분을 사용하지 않는다. */
	if (req->options) {
		kv_for_each(req->options, scx_sum_property, &req->req_hdr_size);
		//TRACE((T_INFO, "REQ_HDR_SIZE=%ld\n", (long)req->req_hdr_size));
	}
	printf("request header req_hdr_size2 = %d, req_hdr_size = %d\n", req->req_hdr_size2, req->req_hdr_size);
	printf("response header res_hdr_size2 = %d, req_size = %d\n", req->res_hdr_size2, req->res_hdr_size);
#endif
/*
	if(req->session) {
		sm_release_session(req);
	}
*/
	if (HR_HEAD == req->method) {
		req->remained = 0;	/* access 로그 기록시 전송해야 할 양이 HEAD method의 경우는 0이다 */
	}
	req->toe = (uint32_t)toe;

	ASSERT(req->t_req_fba > 0.0);
	req->t_res_compl = sx_get_time();
	//ASSERT(req->t_req_fba <= req->t_res_compl);
	if(req->t_req_fba > req->t_res_compl)	//서버의 시간을 조정한 경우 에러가 발생 할수 있음
	{
		TRACE((T_WARN, "[%llu] Time reversed 'req->t_req_fba <= req->t_res_compl'\n", req->id));
		req->t_req_fba = req->t_res_compl;
		req->t_req_lba = req->t_res_compl;
	}
	//TRACE((T_INFO, "completed, freeing...\n"));
	if (req->file) {
		/*
		 * 응답속도 계산을 위해서 이 곳에 추가됨
		 */
		double 				ts, te;
		ts = sx_get_time();
		TRACE((T_DAEMON, "[%llu] URL[%s] - INODE[%d].KV[%p].RING closing\n", req->id, vs_data(req->url), nc_inode_uid(req->file), req->options));
		if (req->options && req->method == HR_POST && req->u.post.oob_ring_opened) {
			TRACE((T_WARN, "[%llu] URL[%s] - INODE[%d].request[%p]/KV[%p].RING; oob_ring still opened, marking EOT\n",
					req->id, vs_data(req->url), nc_inode_uid(req->file), req, req->options));
			kv_oob_write_eot(req->options);
		}
		ASSERT(nc_check_memory(req->file, 0));
		CHECK_TIMER("nc_close", req, nc_close(req->file, 0))
		te = sx_get_time();
		req->t_nc_close = (te - ts);
		req->file = NULL;
	}
#ifdef DEBUG
	/* nc_close()가 호출 되지 않은 상태에서 메모리 해제가 발생하는 경우 디버깅을 위한 코드 */
	ASSERT(req->step != STEP_DOING_OPEN);
#endif
	TRACE((T_DEBUG, "[%llu] response time status : open(%.2f), read(%.2f), close(%.2f), zipper_build(%.2f)\n",
			req->id, req->t_nc_open/1000000.0, req->t_nc_read/1000000.0, req->t_nc_close/1000000.0, req->t_zipper_build/1000000.0));
	scx_make_mavg(req->t_res_compl - req->t_req_fba, req->t_nc_open, req->t_nc_read, req->t_nc_close);
//	if (req->resultcode != 0) { /* resultcode 가 0인 경우는 정상 요청이 아닌것으로 판단해서 로그에 기록하지 않는다. */
	len = hlf_log(buf, sizeof(buf)-1, (const char *)vs_data(req->config->accesslog_format), req);
	if (req->service) {
		if(req->service->log_access){
			logger_put(scx_access_logger, buf, len);
		}
		/* content(http) 통계를 기록한다. */
		standalonestat_write_http(req);
	}
	else { /* service가 결정되지 않은 경우는 default 설정을 보고 log의 on/off를 결정한다. */
		scx_config_t * config = scx_get_config(); //호출후 scx_release_config()를 꼭 해주어야한다.
		if(config->log_access) {
			logger_put(scx_access_logger, buf, len);
		}
		scx_release_config(config);
	}
//	}
	/************************** Complete Phase Handler가 실행 되는 부분 *************************************/
	phase_ctx.req = (void *)req;
	if (SCX_YES != scx_phase_handler(PHASE_COMPLETE, &phase_ctx)) {

	}

	if (req->scx_res.body) {
		SCX_FREE(req->scx_res.body);
		req->scx_res.body = NULL;
	}
	shm_log_complete_request(req);

	/* Complete Phase Handler의 위치와 strm_destroy_stream()의 실행 위치에 대한 검토가 필요하다. */
	if(req->streaming){
		strm_destroy_stream(req);
	}
	scx_update_volume_service_end_stat(req);
	scx_release_config(req->config);

	req->connection = NULL;
	pool = req->pool;
	req->pool = NULL;
	if (req->is_suspeneded == 1) {
		/*
		 * suspended 상태의 연결은 resume가 호출 될때 까지는 callback이 되면 안된다.
		 * 여기에 들어오는 경우가 어떤 조건인지를 모르기 때문에 자세한 정보를 기록한다.
		 * 0.1초를 sleep하는 이유는 다른 worker thread에서 suspend된 job를 처리 했을수도 있어서임
		 *
		 */
		TRACE((T_ERROR, "[%llu] Unexpected suspened connection close event. phase(%d), is_working(%d), job_avail(%)\n", req->id, req->async_ctx.phase, req->async_ctx.is_working, req->async_ctx.job_avail));
		scx_dump_stack();
		scx_marking_tid_to_shm();
		bt_msleep(100);
	}
	req->async_ctx.cr = NULL;
	req->async_ctx_2th.cr = NULL;
	mp_free(pool);
}

static  void
scx_notify_connection(void *cls,
				struct MHD_Connection *connection,
				void **socket_data,
				enum MHD_ConnectionNotificationCode code)
{
	int 		sidx = (int)(long long)cls;

	if (MHD_CONNECTION_NOTIFY_STARTED == code) {
		ATOMIC_ADD(gscx__socket_count, 1);
	}
	else if (MHD_CONNECTION_NOTIFY_CLOSED == code) {
		ATOMIC_SUB(gscx__socket_count, 1);
	}

}

void
scx_get_memory_usage(int pid)
{
	FILE*           file            = fopen("/proc/self/status", "r");
	int             result          = -1;
	char                line[128];
	char            *tok;//, *L;
	int VmSize = 0, VmLck = 0, VmRSS = 0, VmData = 0, VmStk = 0, VmExe = 0, VmLib = 0;

	if (file == NULL) {
		return ;
	}
	while (fgets(line, 128, file) != NULL){
		if (strncmp(line, "VmSize:", 7) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) VmSize = atoi(tok);
		}
		else if (strncmp(line, "VmLck:", 6) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) VmLck = atoi(tok);
		}
		else if (strncmp(line, "VmRSS:", 6) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) VmRSS = atoi(tok);
		}
		else if (strncmp(line, "VmData:", 7) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) VmData = atoi(tok);
		}
		else if (strncmp(line, "VmStk:", 6) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) VmStk = atoi(tok);
		}
		else if (strncmp(line, "VmExe:", 6) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) VmExe = atoi(tok);
		}
		else if (strncmp(line, "VmLib:", 6) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) VmLib = atoi(tok);
		}
	}
	fclose(file);
	TRACE((T_STAT|T_INFO, "**** Process memory status Pid(%d), VmSize(%d), VmLck(%d), VmRss(%d), VmData(%d), VmStk(%d), VmExe(%d), VmLib(%d)\n",
							pid, VmSize, VmLck, VmRSS, VmData, VmStk, VmExe, VmLib));
	gscx__vmsize = VmSize;
	gscx__vmrss = VmRSS;
	return ;
}

int
nce_shutdown()
{
	scx_conf_close_shm();
	cr_stop_service();
	scx_destroy_thread_pool();
//#ifdef ZIPPER
	strm_deinit();
//#endif

	pl_deinit();	/* 프리로드 쓰레드 종료 */

	vm_deinit();
	if(gscx__lua_pool != NULL)	lua_pool_destroy(gscx__lua_pool);
	scx_deinit_module();
	scx_deinit_method_map();

	standalonestat_deinit();
	scx_timer_deinit();
	mdc_deinit();
	scx_deinit_rsapi();
	if(gscx__nc_inited) {
		TRACE((T_INFO, "shutdowning netcache core\n"));
		nc_shutdown();
	}
	gip_close();
	shm_log_deinit();
	//mm_destroy();
	if (scx_access_logger)
		logger_close(scx_access_logger);
	if (scx_error_logger)
		logger_close(scx_error_logger);
	if (scx_origin_logger)
		logger_close(scx_origin_logger);
	scx_deinit_config();
	if (gscx__default_site)
		scx_site_destroy(gscx__default_site);
	scx_deinit_status();
	if(gscx__url_preg != NULL) {
		regfree(gscx__url_preg);
		SCX_FREE(gscx__url_preg);
		gscx__url_preg = NULL;
	}
	if (gscx__previous_rewrite_scripts != NULL) {
		mp_free(gscx__previous_rewrite_scripts->mp);
	}
	if (gscx__current_rewrite_scripts != NULL) {
		mp_free(gscx__current_rewrite_scripts->mp);
	}


	if (NULL != gscx__global_mp) {
		mp_free(gscx__global_mp);
		gscx__global_mp = NULL;
	}
}


int
nce_check_dir_exist(char *path)
{
	struct stat 	s;
	int 			r;

	r = stat(path, &s);
	if (r != 0) {
		return -1;
	}
	if (access(path, R_OK|X_OK|W_OK) != 0) {
		return -1;
	}
	return 0;
}

/*
 * MHD에서 발생한 에러 로그를 기록하는 function
 * MHD 라이브러리에서 callback으로 호출 된다.
 */
volatile static int g__scx_mhd_msg_size = 0;	/* MHD에서 발생하는 에러 메시지의 중복 처리 방지를 위해 사용 */
volatile static int g__scx_mhd_msg_count = 0;	/* 마지막 에러 로그를 기록후 반복된 에러 메시지 count */
volatile static unsigned long g__scx_mhd_msg_time = 0; /* 마지막 에러 로그 기록 시간 */
void
sx_logger(void *cb, const char *format, va_list va)
{
	char	buf[1024];
	time_t 		utime;
	int		len = 0, old_len;
	utime 	= time(NULL);

	vsnprintf(buf, 1024, format, va);
	len = strlen(buf);
	old_len = ATOMIC_VAL(g__scx_mhd_msg_size);
	if(old_len != len) {
		/* 마지막 기록 로그와 다른 메시지 일 경우 기록 한다. */
		ATOMIC_SET(g__scx_mhd_msg_size, len);
		ATOMIC_SET(g__scx_mhd_msg_count, 0);
		ATOMIC_SET(g__scx_mhd_msg_time, utime);
		TRACE((T_INFO|T_DAEMON, "MHD: %s", buf));
	}
	else if (utime > (ATOMIC_VAL(g__scx_mhd_msg_time) + 10)) {
		/* 마지막 에러 로그 기록후 10초 이상 지난 경우 동일 메시지 라도 로그를 출력한다. */
		ATOMIC_SET(g__scx_mhd_msg_time, utime);
		TRACE((T_INFO|T_DAEMON, "message repeated %d times, MHD: %s", ATOMIC_VAL(g__scx_mhd_msg_count),buf));
		ATOMIC_SET(g__scx_mhd_msg_count, 0);
	}
	else {
		ATOMIC_ADD(g__scx_mhd_msg_count, 1);
	}
}

/*
 * error 로그 파일에 기록 하는  function
 * 로그의 기록 여부는 default 설정만 참고 한다.
 * 추후 service별 설정참고 부분도 넣어야 한다.
 */
void
scx_error_log(nc_request_t *req, const char *format, ...)
{
	va_list va;
	const int bufsize = 1023;
	char buf[1023+1] = "";
	time_t 		utime;
	struct tm	*c_tm = NULL;
	int len, tlen, timelen;
	struct tm 	result;
	int log_error, log_gmt_time;
	uint64_t id = 0;
	if(req != NULL) {
		log_gmt_time = req->config->log_gmt_time;
		if (req->service) {
			log_error = req->service->log_error;
		}
		else {
			log_error = req->config->log_error;
		}
		id = req->id;
	}
	else {
		log_error = gscx__config->log_error;
		log_gmt_time = gscx__config->log_gmt_time;
	}
	if(log_error) {

		utime 	= time(NULL);
#if 0
		if(log_gmt_time) {
			c_tm 	= gmtime_r(&utime, &result);
			strftime(buf, bufsize, GMT_DATE_FORMAT, c_tm);
		}
		else {
			c_tm	= localtime_r(&utime, &result);
			strftime(buf, bufsize, LOCAL_DATE_FORMAT, c_tm);
		}
		timelen = strlen(buf);
#else
		if(log_gmt_time) {
			c_tm 	= gmtime_r(&utime, &result);
		}
		else {
			c_tm	= localtime_r(&utime, &result);
		}
		/* access log와 포맷을 맞추기 위해서 아래처럼 한다. */
		timelen = scx_snprintf(buf, bufsize, "[%4d-%02d-%02dT%02d:%02d:%02d%c%02i:%02i]",
	                       c_tm->tm_year+1900, c_tm->tm_mon+1,
						   c_tm->tm_mday, c_tm->tm_hour,
						   c_tm->tm_min, c_tm->tm_sec,
						   c_tm->tm_gmtoff < 0 ? '-' : '+',
	                       abs(c_tm->tm_gmtoff / 3600), abs(c_tm->tm_gmtoff % 3600));
#endif
		*(buf+timelen++) = ' '; /* 시간뒤에 공백 추가 */
		snprintf(buf+timelen, bufsize - timelen, "[%llu] ", id) ;
		tlen = strlen(buf);
		va_start (va, format);
		len = vsnprintf(buf+tlen, bufsize - tlen,format, va) + tlen ;
		va_end (va);
		if (len < 0)	return ;
		len = strlen(buf); /* vsnprintf에서 리턴된 값은 buffer size의 제한을 초과할 경우 초과하는 값을 포함 해서 리턴이 되기때문에 실제의 크기를 다시 계산 해야 한다. */
		if (buf[len-1] != '\n' ) {	/* 기록 해야 하는 로그의 크기가 buffer 크기를 초과해서 마지막의 '\n'이 없어 질수 있어서 다시 넣어 준다.*/
			buf[len-1] = '\n';
		}
		TRACE((T_DEBUG, "%s", buf+timelen)); /* sp로그에 시간 부분만 제외하고 동일하게 출력 한다. */
		logger_put(scx_error_logger, buf, len);
	}
}

void daemon_shutdown()
{
}
void
nce_sig_shutdown_handler(int sig)
{
	pid_t 		pid;
	int 		status;

	gscx__need_rerun_child = 0;
	if (gscx__main_service_pid > 0) {
		syslog(LOG_INFO|LOG_PID, "signal %d got, shutdowning child %u", sig, gscx__main_service_pid);
		kill(gscx__main_service_pid, SIGTERM);
		waitpid(gscx__main_service_pid, &status, 0);
		syslog(LOG_INFO|LOG_PID, "child %u shutdowned",gscx__main_service_pid);
		scx_remove_pidfile();
	}
}
static void
nce_ignore(int sig)
{
	TRACE((T_INFO|T_STAT, "************* SIGNAL (%d) ignored.\n", sig));
}
static void
nce_ignore_signal()
{
	sigset_t 	new_sigset;
	struct sigaction	new_action;
	struct sigaction	old_action;
	sighandler_t 				r;
	new_action.sa_handler = nce_ignore;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = SA_RESTART;
	sigaction(SIGPIPE, &new_action, &old_action);
//	sigaction(SIGABRT, &new_action, &old_action); 	//시그널 허용을 해준다.
	sigaction(SIGALRM, &new_action, &old_action);
#if 0
	r = signal(SIGPIPE, SIG_IGN);
	if (r == SIG_ERR) TRACE((T_ERROR, "Blocking SIGPIPE failed\n"));
	r = signal(SIGABRT, SIG_IGN);
	if (r == SIG_ERR) TRACE((T_ERROR, "Blocking SIGABRT failed\n"));
	r = signal(SIGALRM, SIG_IGN);
	if (r == SIG_ERR) TRACE((T_ERROR, "Blocking SIGALRM failed\n"));
#endif
}
pthread_mutex_t 	__signal_handler_lock = PTHREAD_MUTEX_INITIALIZER;
static void *
signal_handler_thread(void *d)
{
	int			sig = *((int *)d);
	site_t 		*temp_site = NULL;
	uint32_t	hash;
	off_t			st_size;	/* 설정 파일의 크기 */
	static 		st_mask_toggle = 0;
	static char debug_log_mask[] = "info,warn,stat,error,plugin,trigger,debug,daemon,inode";
	SCX_FREE(d);

	TRACE((T_INFO|T_STAT, "(%d) signal received\n", sig));
	/* 서비스가 초기화 중일때 아래의 동작들을 실행하면 여러가지 문제(deadlock등)가 발생할수 있어서 초기화과 완료되기 이전에는 시그널을 무시한다. */
	if (1 != gscx__service_available) {
		TRACE((T_INFO|T_STAT, "child (%d) signal ignored(service initialization not completed.)\n", sig));
	}
	else {
		/* signal 처리는 동시에 한 session 되도록 한다. */
		pthread_mutex_lock(&__signal_handler_lock);
		if(sig == SIGUSR2) {	//로그 레벨 변경
			if(st_mask_toggle) {
				TRACE((T_INFO|T_STAT, "Toggle log mask '%s'\n", vs_data(gscx__config->nc_trace_mask)));
				trc_set_mask(vs_data(gscx__config->nc_trace_mask));
			}
			else {
				TRACE((T_INFO|T_STAT, "Toggle log mask '%s'\n", debug_log_mask));
				trc_set_mask(debug_log_mask);
			}
			st_mask_toggle = !st_mask_toggle;
		}
		else if(sig == SIGUSR1) {	//설정 reload
			TRACE((T_INFO|T_STAT, "Reload called\n"));
			/* 동시에 call 될 경우 두번째 부터는 reload가 끝날때까지 무시 되어야 한다.
			 * reloading이 진행중인지 확인 할수 있는 방법 필요 */

			/*
			 *  default.conf 파일의 hash를 만든후
			 *  이전 설정의 hash 와 비교 해서 틀린 경우 scx_site_create()를 사용해서 설정을 읽는다.
			 *  임시로 생성된 site는 작업이 끝나고 메모리 해제를 해주어야 한다.
			 */
			hash = vm_make_config_hash(gscx__default_conf_path, &st_size);
			if (hash != gscx__config->hash) {
				temp_site = scx_site_create(gscx__default_conf_path);
				if(temp_site == NULL) {
					TRACE((T_ERROR, "%s(%d) Failed to open %s", gscx__default_conf_path));
					goto volume_reload;
				}
				if(nce_load_global_params(temp_site, gscx__config, TRUE) < 0) {
					goto volume_reload;
				}
				gscx__config->hash = hash;
			}
			nce_reload_default_cert(TRUE);
volume_reload:
			if (temp_site)
				scx_site_destroy(temp_site);
			//void __nc_dump_heap();
			//__nc_dump_heap();
			vm_create_config(1);
		}
		else if(sig == SIGHUP) { /* log rotate */

			//로그 Rotate
			if (1 == gscx__config->logrotate_signal_enable) {
				TRACE((T_INFO|T_STAT, "Log Rotate Start.\n"));
				scx_log_rotate();
				TRACE((T_INFO|T_STAT, "Log Rotate End.\n"));
			}
			else {
				/* LogRotate 연동 설정이 없는 경우는 signal이 들어 오더라도 무시한다. */
				TRACE((T_INFO|T_STAT, "Log Rotate signal(%d) ignored.\n", sig));
			}
			TRACE((T_INFO|T_STAT, "SIGHUP(%d) signal received\n", sig));
		}
		pthread_mutex_unlock(&__signal_handler_lock);
	}

	return NULL;
}

static int           __in_signal = 0;
void
scx_dump_stack()
{
    int         j, nptrs;
    void        *frames[100];
    char        **strings;

    if (__in_signal) return;
    __in_signal++;

    nptrs = backtrace(frames, 100);

   /*
    *   The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
    *  would produce similar output to the following:
    */

    strings = backtrace_symbols(frames, nptrs);

    TRACE((T_ERROR, "CRITICAL : call stack dump information(%d frames)\n", nptrs));
    for (j = 0; j < nptrs; j++) {
         backtrace_lineinfo(j+1, frames[j], strings[j]);
    }
    TRACE((T_ERROR, "CRITICAL : end of stack dump information\n"));
    free(strings);
    __in_signal = 0;
}

/*
 * SIGSEGV signal이 발생시 재기동을 하지 않고 예외 처리하는 경우
 * 예외 처리 이력을 확인 할수 없어서 공유 메모리(/dev/shm/solproxy_critical.tid)에
 * 현재 thread의 ID를 기록
 * 모니터링 스크립트(solproxy_restart_check.sh)에서 이 파일을 확인해서 관련 로그를 이메일로 전송 하도록 한다.
 * https://jarvis.solbox.com/redmine/issues/33308
 */
void
scx_marking_tid_to_shm()
{
	unsigned char buf[20] = {'\0'};
	size_t len;
	int shf_fd = 0;
	char shm_name[64] = {'\0'};

	snprintf(shm_name, 256, "%s_critical.tid", PROG_SHORT_NAME);
	shf_fd = shm_open(shm_name, O_CREAT|O_TRUNC|O_RDWR, 0666);

	if(shf_fd < 0)
		return;

	len = snprintf(buf, sizeof(buf)-1, "%d", gettid());
	write(shf_fd, buf, len);

	close(shf_fd);
	return;
}

void
nce_signal_handler(int sig)
{

	pid_t pid = getpid();
	pthread_t 	tid;
	int *argnum = NULL;
	if(gscx__main_service_pid != 0){
		/* parent process */
		syslog(LOG_PID|LOG_INFO, "%d %d signal received", pid, sig);
		kill(gscx__main_service_pid, sig);	/* parent에서는 하는 일이 없으므로 child process에 전달 한다.*/

	}
	else if (sig == SIGSEGV) {
	    TRACE((T_ERROR, "signal %d caught!\n", sig));
		scx_dump_stack();
		/*
		 * 현재 zipper 라이브러리에서 SIGSEGV가 발생한 경우 재기동을 하지 않고 sigsetjmp 호출 시점 원복하도록 한다.
		 * force_sigjmp_restart가 1로 설정된 경우는 예외 없이 재기동 한다.
		 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/33307
		 */
		if (__thread_jmp_working == 1 && gscx__config->force_sigjmp_restart == 0) {
			scx_marking_tid_to_shm();
			siglongjmp(__thread_jmp_buf, 1);
		}
		else {
			_exit(1);
		}
	}
	else {
        argnum = SCX_CALLOC(1, sizeof(int));
        ASSERT(argnum);
        /* thread 전달 도중 이 함수가 종료되면 signal값이 변경될 가능성이 있어서 이렇게 처리함 */
        *argnum = sig;
		/* child process */
		syslog(LOG_PID|LOG_INFO, "%d %d signal received", pid, sig);
		pthread_create(&tid, NULL, signal_handler_thread, (void *) argnum);
		pthread_detach(tid);
	}
	return;
}

void
nce_setup_signals()
{
	struct sigaction new_sigaction;
	sigset_t 	new_sigset;

	sigemptyset(&new_sigset);
	//sigaddset(&new_sigset, SIGCHLD);
	sigaddset(&new_sigset, SIGSTOP);
	sigaddset(&new_sigset, SIGTTOU);
	sigaddset(&new_sigset, SIGTTIN);
	sigaddset(&new_sigset, SIGPIPE);
	sigprocmask(SIG_BLOCK, &new_sigset, NULL);

	new_sigaction.sa_handler = nce_sig_shutdown_handler;
	sigemptyset(&new_sigaction.sa_mask);
	new_sigaction.sa_flags = 0;
	//sigaction(SIGCHLD, &new_sigaction, NULL);
	//sigaction(SIGHUP, &new_sigaction, NULL);
	sigaction(SIGTERM, &new_sigaction, NULL);
	//sigaction(SIGINT, &new_sigaction, NULL);

	new_sigaction.sa_handler = nce_signal_handler;
	sigemptyset(&new_sigaction.sa_mask);
	new_sigaction.sa_flags = 0;
	sigaction(SIGUSR1, &new_sigaction, NULL);
	sigaction(SIGUSR2, &new_sigaction, NULL);
//	sigaction(SIGHUP, &new_sigaction, NULL);

	new_sigaction.sa_handler = nce_signal_handler;
	sigemptyset(&new_sigaction.sa_mask);
	new_sigaction.sa_flags = 0;
	sigaction(SIGHUP, &new_sigaction, NULL);


}

struct hang_check_tag {
	CURL 				*c;
	struct curl_slist 	*chunk;
};
pthread_t 	gscx__hang_check_tid;
int			gscx__hang_check_enable;
static void *hang_check_thread(void *d);
static void gdb_stack_dump(int pid, char *log_path);
static void hang_check_clean_up(void *arg);
static size_t hang_check_body_read_callback( void *source , size_t size , size_t nmemb , void *userData );

void
nce_monitor_service()
{
	pid_t 	waited_pid;
	int 	i;
	int 	status = 0;
	int		load_count = 0; /* child process 를 시작할 때마다 1씩 증가한다. */
	/*
	 * 현재 모니터링 프로세스 상태
	 */

	nce_setup_signals();

	close(0);
	close(1);
	close(2);
	open("/dev/null", O_RDWR);
	open("/dev/null", O_RDWR);
	open("/dev/null", O_RDWR);
	scx_set_proctitle(PROG_SHORT_NAME, "monitoring process");
	/* 여기에 .solproxy_active 파일을 생성하고 권한을 user로 바꿔야 한다. */

	do {
		load_count++;
		/* 여기에 .solproxy_active를 검사하는 부분이 들어 가야 한다. */
		if (gscx__need_rerun_child != 1)
			break;
		gscx__main_service_pid = fork();

		if (gscx__main_service_pid < 0) {
			syslog(LOG_PID|LOG_ERR, "%s creating the main service failed", PROG_SHORT_NAME);
			exit(1);
		}
		else if (gscx__main_service_pid == 0) {
			scx_set_proctitle(PROG_SHORT_NAME, "worker process");
			/* 
			 * child process, 여기에서 메인 서비스 실행.
			 * 종료시 자연스럽게 빠져나가도록 한다
			 * 재기동은 gscx__need_rerun_child == 1이면 재기동하도록 한다
			 */
			main_service(load_count);
			break;
		}
		/* 여기서 monitoring thread 생성 */
		gscx__hang_check_enable = 1;
		gscx__hang_check_tid = 0;

//#ifndef ZIPPER /* streaming(zipper)으로 동작 할때에는 별도의 체크 로직이 필요해서 우선 zipper에서는 check 기능을 사용하지 않는다. */
		pthread_create(&gscx__hang_check_tid, NULL, hang_check_thread, (void *) &gscx__main_service_pid);
//#endif
		/* monitoring 상태로 진입 */
		syslog(LOG_PID|LOG_INFO, "%s monitoring the child process %d begins", PROG_SHORT_NAME, gscx__main_service_pid);
		do {
			/*
			 * 지정된  child process, gscx__main_service_pid에 대해서 종료될 때까지 대기
			 */
			waited_pid = waitpid(gscx__main_service_pid, &status, 0);
			if (waited_pid == -1) {
				if (errno == EINTR) {
					continue;
				}
				break;
			}
			if (WIFEXITED(status)) {
				syslog(LOG_PID|LOG_ERR, "%s child process, %d exited", PROG_SHORT_NAME, waited_pid, gscx__need_rerun_child);
			}
			else if (WIFSIGNALED(status)) {
				syslog(LOG_PID|LOG_ERR, "%s child process, %d signal(%d) received", PROG_SHORT_NAME, waited_pid, WSTOPSIG(status));
			}
			else if (WCOREDUMP(status)) {
				syslog(LOG_PID|LOG_ERR, "%s child process, %d core dumped", PROG_SHORT_NAME, waited_pid);
			}
		} while (waited_pid <= 0);
#ifndef USE_SIGTERM_SIGNAL
		if (0 != gscx__need_rerun_child)
			gscx__need_rerun_child = scx_check_child_msg_file();
#endif
		if (gscx__hang_check_tid)
		{
			gscx__hang_check_enable = 0;
			pthread_cancel(gscx__hang_check_tid);
			pthread_join(gscx__hang_check_tid, NULL);
			gscx__hang_check_tid = 0;
		}

	} while (gscx__need_rerun_child == 1);
	if (gscx__hang_check_tid) {
		gscx__hang_check_enable = 0;
		pthread_cancel(gscx__hang_check_tid);
		pthread_join(gscx__hang_check_tid, NULL);
	}
	if (gscx__main_service_pid != 0) {
		syslog(LOG_PID|LOG_INFO, "%s monitoring process shutdowned(rerun=%u)", PROG_SHORT_NAME, gscx__need_rerun_child);
#ifndef USE_SIGTERM_SIGNAL
		scx_remove_pidfile();
#endif
	}
}

/*
 * Monitoring thread 동작
 * thread 실행후 5분동안은 아무런 동작을 하지 않고 대기
 * 5분이 지난후 10초 간격으로 solproxy(zipper)의 서비스 포트로 요청
 * 요청후 10초가 지나면 현재 요청을 종료하고 다시 요청
 * 연속 3회동안 timeout이 지나거나 비정상 응답을 하는 경우 child process 강제 종료.
 * 이때 thread도 같이 종료함.
 *
 * http port를 사용하지 않는 경우 어떻게 할까?
 */
static void *
hang_check_thread(void *d)
{

	/* gscx__main_service_pid를 사용하지 않고 pid를 매개 변수로 넘겨 받는 이유는 모니터링 쪽에서 worker process를 재시작 했을 경우 새로 생성된 worker의 pid에 대해 작업을 할 가능성이 있어서임 */
	int			service_pid = *(int *)d;
	char		szPathDefaultFile[512] = { 0, };
	site_t 		*default_site = NULL;
	vstring_t 	*listen_ip = NULL;
	int			listen_port = 80;
	char		check_url[256];
	char		url[100] = { 0, };
	char		host_header[40] = { 0, };
	CURL 		*c = NULL;
	struct curl_slist *chunk = NULL;
	int 		body_size = 0;
	long		ret_code = 0;
	double 		length;
	CURLcode 	errornum;
	int			error_count = 0;
	int 		hang_detect = 1;
	int			timeout	= 10;
	vstring_t 	*log_path_vs = NULL;
	char 		log_path[256] ={ 0, };
	char 		errbuf[CURL_ERROR_SIZE];


	struct hang_check_tag hang_check_ctx = {NULL, NULL};

	prctl(PR_SET_NAME, "Hang check thread");


	bt_msleep(300000); /* 5분동안 대기 */
	snprintf(szPathDefaultFile, sizeof(szPathDefaultFile), "%s/default.conf", vs_data(gscx__config_root));

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);	/* 아래 부분 실행하는 동안 pthread_cancel()이 호출되면 memory leak 가능성이 있어서 */
	/* 설정파일에서 listen_ip와 http_port 정보를 가져 오기 위해 scx_site_create() 호출하고 바로 해제 한다. */
	default_site = scx_site_create(szPathDefaultFile);
	if(default_site == NULL) {
		/* 문제가 있는 경우 그냥 리턴한다. */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		goto hang_check_end;

    }

	listen_ip = scx_get_vstring(default_site, SV_HTTP_LISTEN_IP, NULL);
	listen_port = scx_get_uint(default_site, SV_HTTP_PORT, 80);
	hang_detect = scx_get_uint(default_site, SV_HANG_DETECT, 1);	/* hang_detect가 0으로 설정되는 경우 hang 감지를 수행하지 않는다. */
	timeout = scx_get_uint(default_site, SV_HANG_DETECT_TIMEOUT, 20);
	log_path_vs = scx_get_vstring_lg(default_site, SV_LOG_DIR, gscx__default_log_path);

#ifdef ZIPPER
	snprintf(url, 100, "http://%s:%d/%s/_definst_/single/eng/0/%s/content.mp3",
			listen_ip?vs_data(listen_ip):"127.0.0.1", listen_port, CHECK_VOLUME_NAME, CHECK_FILE_PATH);
#else
	snprintf(url, 100, "http://%s:%d/%s", listen_ip?vs_data(listen_ip):"127.0.0.1", listen_port,CHECK_FILE_PATH);
#endif
	snprintf(log_path, sizeof(log_path), "%s", vs_data(log_path_vs));

	/*
	 * 여기서 위의 site를 지워야 메모리 릭이 발생하지 않는다.
	 * site에서 할당 받은  listen_ip 와 log_path_vs 등도 이후에는 사용하지 못하기 때문에
	 * 위의 log_path 변수 처럼 별도의 변수에다 복사를 해야 한다.
	 */
	scx_site_destroy(default_site);

	if (!hang_detect) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		goto hang_check_end;
	}
	c = curl_easy_init ();	/* thread 강제 종료에 의한 메모리릭 가능성 때문에 여기서 curl context를 할당해서 thread에 넘겨 준다. */
	snprintf(host_header, 40, "Host: %s", CHECK_VOLUME_NAME);
	chunk = curl_slist_append(chunk, host_header);	// "Host: contents.check" 헤더 추가
    // 쓰레드 종료시 호출될 함수 등록

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, chunk);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, hang_check_body_read_callback);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &body_size);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1);	/* Fail on HTTP 4xx errors. */
	curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);	/* 20초 time out */
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
	/*
	 * http 1.0으로 하는 경우는 서버와 클라이언트중 어느쪽이 먼저 끊을지 알수 없기 때문에 1.1로 설정해서 client(monitoring)가 먼저 끊도록 한다.
	 * https://jarvis.solbox.com/redmine/issues/31946 참조
	 */
	curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 1L);	/* socket 재사용 금지, curl_easy_perform()수행후 연결을 종료한다. */
	hang_check_ctx.c = c;
	hang_check_ctx.chunk = chunk;
	pthread_cleanup_push(hang_check_clean_up, (void *)&hang_check_ctx);	/* pthread_cancel()로 thread가 종료 될때 호출될 함수 지정 */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);


	while (gscx__hang_check_enable) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);	/* curl_easy_perform()이 실행되는 동안 thread가 강제 종료 되면 메모리 릭이 발생할수도 있을것 같아서 */
		errornum = curl_easy_perform(c);
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		if (CURLE_OK != errornum) {
		  /* error 처리 */
			syslog(LOG_PID|LOG_ERR, "%s(%d) %s curl error, '%s'(%d)", PROG_SHORT_NAME, service_pid, __func__, errbuf, errornum);
			error_count++;
		}
		else {
			curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, (long *)&ret_code);
			curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD,  (double *)&length);
			if (200 != (int)ret_code) {
				/* return code가 200이 나와야함, 이외의 코드는 무조건 에러처리 */
				syslog(LOG_PID|LOG_ERR, "%s(%d) %s return code(%ld) error", PROG_SHORT_NAME, service_pid, __func__, ret_code);
				error_count++;
			}
			else {
				/* 정상 응답을 받은 경우 error_count를 초기화 */
				error_count = 0;
			}
		}

		if (3 <= error_count) {
			break;

		}
		bt_msleep(10000);	/* 10초 간격으로 모니터링 */
	}

hang_check_end:
	pthread_cleanup_pop(0);
	if (chunk) {
		curl_slist_free_all(chunk);
	}
	if (c) {
		curl_easy_cleanup (c);
	}
	if (3 <= error_count) {
		/* 3회 연속 에러가 발생하면 child process를 강제 종료한다. */
		syslog(LOG_PID|LOG_ERR, "%s(%d) service not respond", PROG_SHORT_NAME, service_pid);
		/* 프로세스를 죽이기 전에 gdb를 사용한 stack dump를 시도한다. */
		gdb_stack_dump(service_pid, log_path);
		scx_parent_kill(1);	/* 자식 프로세스가 부팅 과정에서 hang 걸려 있는 경우에 부모 프로세스까지 같이 죽지 않도록 하기 위해 설정 한다.*/
		kill(service_pid, SIGKILL);
	}
	return NULL;
}

/*
 * 모니터링(부모) 프로세스에서 hang 감지 모니터링 중에 hang이 발생하는 경우에
 * 서비스(자식) 프로세스를 재기동 시키기 전에 GDB를 사용해서 파일에 전체 stack dump 결과를 남기도록 한다.
 * 이 기능은 gdb가 설치 되어 있는 경우에만 동작하고 설치 되어 있지 않은 경우에는 서비스 프로세스를 재기동만 시킨다.
 ** gdb 설치는 solproxy(zipper) 프로파일의 rpm 설치 기능을 사용한다.
 ** solproxy rpm 설치시에 rpm 의존성을 설정해서 할수도 있지만 gdb 설치가 불가한 경우가 있을수가 있어서 ACT의 프로파일을 사용함.
 ** 현재는 실 서비스에서 gdb를 사용하는 경우가 solproxy 밖에 없어서 ACT의 프로파일을 사용해서 설치가 가능하지만 다른 데몬에서도 gdb를 사용하는 경우가 생기면 rpm 의존성을 설정해서 설치 할수도 있음.
 * stack dump는 다음의 명령을 사용
 ** gdb -batch -n -ex 'set pagination off' -ex 'thread apply all bt full' -p (solproxy 자식 프로세스번호)
 * stack dump 결과는 log 디렉토리에 stack_날짜_pid.log로 기록한다.
 * 용량을 줄이기 위해 gzip을 사용해서 압축을 한다.
 * sudo를 사용해서 solproxy를 기동시키는 경우 gdb 권한을 확인할 필요가 있다.
 */
static void
gdb_stack_dump(int pid, char *log_path)
{
	const char gdb_path[] = {"/usr/bin/gdb"};
	const char gzip_path[] = {"/usr/bin/gzip"};
	char gdb_dump_cmd[512] = {'\0'};
	char dump_file_path[256] = {'\0'};
	char msg[1024] = {'\0'};
	time_t	now;
	struct tm 	tm_now;
	time_t buildtime = BUILD_DATE;
	struct tm *t;
	t = localtime(&buildtime);

	now = time(NULL);
	localtime_r(&now, &tm_now);



	/* '/usr/service/logs/solproxy/stack_20161024_15491.log'의 형태로 로그 파일을 만든다. */
	snprintf(dump_file_path, sizeof(dump_file_path), "%s/stack_%04d%02d%02d_%d.log",
			log_path, tm_now.tm_year+1900, tm_now.tm_mon+1, tm_now.tm_mday, pid );


	/* gdb 파일이 없는 경우 skip 한다. */
	if( access( gdb_path, F_OK ) != -1 ) {
		// file exists
		/* 디버깅을 위해 rpm 정보를 덤프 파일의 앞쪽에 넣는다. */
		if (0 == gscx__netcache_ver[0])
			nc_version(gscx__netcache_ver, sizeof(gscx__netcache_ver));
		snprintf(msg, sizeof(msg), "Core Version            : %s(rev %d)\n%s Version        : %s-%d in %s\n",
				gscx__netcache_ver, CORE_SVN_REVISION, PROG_SHORT_NAME, PROG_VERSION, SVN_REVISION, asctime(t));
		snprintf(gdb_dump_cmd, sizeof(gdb_dump_cmd),
					"echo '%s'> %s",
					msg, dump_file_path);
		system(gdb_dump_cmd);

		/* gdb를 사용한 backtrace full dump를 한다. */
		snprintf(gdb_dump_cmd, sizeof(gdb_dump_cmd),
				"%s -batch -n -ex 'set pagination off' -ex 'thread apply all bt full' -p %d >> %s",
				gdb_path, pid, dump_file_path);
		syslog(LOG_PID|LOG_ERR, "%s gdb stack dump(%d)", PROG_SHORT_NAME, pid);
		system(gdb_dump_cmd);

		/* 덤프 파일을 gzip으로 압축 */
		snprintf(gdb_dump_cmd, sizeof(gdb_dump_cmd),
				"%s %s", gzip_path, dump_file_path);
		system(gdb_dump_cmd);
	} else {
		// file doesn't exist
		syslog(LOG_PID|LOG_ERR, "%s gdb(%s) not found", PROG_SHORT_NAME, gdb_path);
	}
}

// 쓰레드 종료시 호출될 함수
// 메모리릭을 최소화 하기 위해 사용.
static void
hang_check_clean_up(void *arg)
{
	struct hang_check_tag *pctx = (struct hang_check_tag *)arg;

	if (pctx->chunk) {
		curl_slist_free_all(pctx->chunk);
		pctx->chunk = NULL;
	}
	if (pctx->c) {
		curl_easy_cleanup(pctx->c);
		pctx->c = NULL;
	}
	return;
}

static size_t
hang_check_body_read_callback( void *source , size_t size , size_t nmemb , void *userData )
{

	const int buffersize = size * nmemb ;
	int	 *body_size = userData;
#if 0	/* 실제 body를 읽을 필요는 없고 크기만 정상인지 받는다. */
	char * buf = malloc(buffersize);
	*body_size = buffersize;
	memcpy(buf, (char *)source, buffersize);
	printf ("message = \n %s\n", buf);
#endif
	return( buffersize ) ;

}




static void
scx_lookup_my_address()
{
	char 				*dns_server = "8.8.8.8";
	int 				dns_port = 53;

	struct sockaddr_in 	ep_addr;

	int ep = socket(AF_INET, SOCK_DGRAM, 0);

	if (ep < 0) {
		perror("socket error");
		exit(1);
	}
	memset(&ep_addr, 0, sizeof(ep_addr));
	ep_addr.sin_family = AF_INET;
	ep_addr.sin_addr.s_addr = inet_addr(dns_server);
	ep_addr.sin_port 		= htons(dns_port);

	int 	err = connect(ep, (const struct sockaddr *)&ep_addr, sizeof(ep_addr));

	struct sockaddr_in 	name;
	socklen_t 			namelen = sizeof(name);
	err = getsockname(ep, (struct sockaddr *)&name, &namelen);
	close(ep);
	char 	ipaddr_buf[128];
	const char 	*p = inet_ntop(AF_INET, &name.sin_addr, ipaddr_buf, sizeof(ipaddr_buf));
	if ( p != NULL) {
		gscx__local_ip = vs_strdup_lg(gscx__global_mp, ipaddr_buf);
	}
	else {
		perror("inet_ntop error");
		exit(1);
	}
}

/*
 * solproxy가 실행되는 서버가 하드웨어 적으로 AES-NI를 지원하는지 확인 한다.
 * 확인 방식은 /proc/cpuinfo의 flags값에 aes가 있는 경우 AES-NI를 지원 할수 있다고 판단한다.
 */
#if 0
static void
scx_check_aesni()
{
	FILE *cmdFp = NULL;
	char cmdBuf[256];
	char buf[5];
	int	 cpucount = 0;


	snprintf(cmdBuf, 256, "grep flags /proc/cpuinfo|grep aes|wc -l");

	cmdFp = popen(cmdBuf, "r");

	if (!cmdFp)
	{
		fprintf(stderr, "/proc/cpuinfo file open Fail!(%s)\n", strerror(errno));
		return;
	}

	if(fgets(buf, 5, cmdFp) == NULL)
	{
		pclose(cmdFp);
		fprintf(stderr, "%s : unknown Error\n", __FUNCTION__);
		return ;
	}
	pclose(cmdFp);
	cpucount = atoi(buf);
	if(cpucount > 0) {
		gscx__use_aesni = 1;
		fprintf(stdout, "AES-NI enabled\n");
	}
	else
	{
		gscx__use_aesni = 0;
		fprintf(stdout, "AES-NI disabled\n");
	}
}
#else
static void
scx_check_aesni()
{
	/*
	 * 수정 이유
	 * popen으로 해당 정보를 가져오는 경우 
	 * GDB의 follow_child 옵션 모드를 사용할 수 없게됨
	 * child process가 계속 재기동해야하는 경우 gdb가 follow해줄 수 없음
	 * 2020.7.25 by weon@solbox.com
	 */
	FILE			*fcpuinfo = fopen("/proc/cpuinfo", "rb");
	char			*info_line = NULL;
	size_t			info_len = 0;
	char			*tok, *p;
	int				aes_flg = 0;

	while (getline(&info_line, &info_len, fcpuinfo) != -1) {
		if (strncmp(info_line, "flags", 5) == 0) {
			p = info_line + 5;
			while (*p && *p != ':') p++;
			if (*p == 0) continue;
			p++;
			tok = strtok(p, " ");
			do {
				if (tok && strcasecmp(tok, "aes") == 0) {
					aes_flg = 1;
					break;
				}
			} while (tok = strtok(NULL, " "));

		}
	}
	fclose(fcpuinfo);

	gscx__use_aesni = aes_flg; 
	fprintf(stdout, "AES-NI %s\n", aes_flg?"enabled":"disabled");
}
#endif

/*
 * 설정 파일에 정의된 메모리 관련 용량만큼 실제 시스템의 가용 메모리가 있는지 확인 한다.
 * 시스템의 가용 메모리는 free 명령어 실행시 나오는 값중에 freeram + bufferram + cached 이다.
 * inmemory_objects 1개 마다 약 600 byte정도씩 증가한다.
 * 정상 구동을 위해서는 (cache_size * 1024 * 1024) + (inmemory_objects * 600) 보다
 * 가용 메모리가 100메가 이상 커야 한다.
 * 즉 free_size > (cache_size * 1024 * 1024) + (inmemory_objects * 600) + (100 * 1024 * 1024) 이어야함.
 */
static int
scx_check_memory()
{
	struct sysinfo si;
	FILE *fp = NULL;
	const int max_len = 100;
	char buf[max_len];
	unsigned long cached_size = 0UL;
	unsigned long free_size = 0UL;
	unsigned long require_size = 0UL;
	if(sysinfo(&si) < 0) {
//	  printf("error! couldn't get sin");
	  TRACE((T_WARN, "couldn't get sysinfo\n"));
	  return SCX_YES;
	}


	fp = fopen("/proc/meminfo", "r");
	ASSERT(fp);
	for(int i = 0; i <= 3; i++) {
	  fgets(buf, max_len, fp);
	}
	fclose(fp);
	/* buf에 "Cached:            71492 kB"가 들어옴 */
	char *p1 = strchr(buf, ':');
	/* p1에 ":            71492 kB"가 들어옴 */
	p1++;
	/* p1에 "            71492 kB"가 들어옴 */
	cached_size = strtoull(p1, NULL, 10) * 1024UL;
	free_size = si.freeram + si.bufferram + cached_size;
#if 0
	require_size = (gscx__config->cache_mem * 1024 * 1024) + (gscx__config->max_inodes * 600) + (100 * 1024 * 1024);
#else
	require_size = (gscx__config->cache_mem * 1024 * 1024) + (gscx__config->inode_mem * 1024 * 1024) + (100 * 1024 * 1024);
#endif
	if (free_size < require_size) {	/* 가용 메모리 용량이 필요 메모리 용량 보다 작은 경우 종료 한다. */
		TRACE((T_ERROR, "Insufficent memory.(free : %llu, required : %llu)\n", free_size, require_size));
		syslog(LOG_ERR|LOG_PID, "Insufficent memory.(free : %llu, required : %llu)\n", free_size, require_size);
		return SCX_NO;
	}
	return SCX_YES;
}

int
usage(char *prog)
{
	fprintf(stdout, "%s - usage:\n"
					"   --standalone={0|1}      : when 1, start as stand alone(not daemon)\n"
					"   -D, --debug             : Set log level.(default:info,warn,error)\n"
					"   -h, --help              : Print this message and exit.\n"
					"   -v, --version           : Print the version number.\n"
					"   -C, --config            : Print configuration file information.\n"
					"   -p, --pid               : Set pid file path.\n"
					, prog);
	return 0;
}

static void
scx_report_environment()
{
	TRACE((T_INFO|T_STAT, "***************** Start Service *********************\n"));
	TRACE((T_INFO|T_STAT, "%s Running Environment:\n", PROG_NAME));
	TRACE((T_INFO|T_STAT, "* Configuration directory : %s\n", vs_data(gscx__config_root)));
	TRACE((T_INFO|T_STAT, "* Log directory           : %s\n", vs_data(gscx__config->log_dir )));
	TRACE((T_INFO|T_STAT, "* HTTP port               : %d\n", gscx__config->http_port));
	TRACE((T_INFO|T_STAT, "* HTTPS port              : %d\n", gscx__config->https_port));
	if (gscx__config->tproxy_port)
		TRACE((T_INFO|T_STAT, "* TPROXY port             : %d\n", gscx__config->tproxy_port));
	TRACE((T_INFO|T_STAT, "* Certificate             : %s\n",  (gscx__config->certificate_crt_path == NULL?"(not-defined)":(char *)vs_data(gscx__config->certificate_crt_path))));
	TRACE((T_INFO|T_STAT, "*                         : %s\n",  (gscx__config->certificate_key_path == NULL?"(not-defined)":(char *)vs_data(gscx__config->certificate_key_path))));
	TRACE((T_INFO|T_STAT, "* Minimum concurrent AIO  : %d\n", gscx__min_rapool));
	TRACE((T_INFO|T_STAT, "* Maximum concurrent AIO  : %d\n", gscx__max_rapool));
#if 0
	TRACE((T_INFO|T_STAT, "* In-memory cache objects : %d\n", gscx__config->max_inodes));
#else
	TRACE((T_INFO|T_STAT, "* Inode memory size(MB)   : %d\n", gscx__config->inode_mem));
#endif
	TRACE((T_INFO|T_STAT, "* read-ahead(MB)          : %d\n", gscx__config->ra_mb));
	TRACE((T_INFO|T_STAT, "* Cache memory size(MB)   : %lu\n", gscx__config->cache_mem));
	TRACE((T_INFO|T_STAT, "* Chunk size (KB)         : %d\n",  gscx__config->block_size));
	TRACE((T_INFO|T_STAT, "* Host name               : %s\n",  (gscx__config->host_name?(char *)vs_data(gscx__config->host_name):"(not-defined)")));
	TRACE((T_INFO|T_STAT, "* # of Workers            : %d\n",  gscx__config->workers));
	TRACE((T_INFO|T_STAT, "* Positive TTL(sec)       : %d\n",  gscx__config->positive_ttl));
	TRACE((T_INFO|T_STAT, "* Negative TTL(sec)       : %d\n",  gscx__config->negative_ttl));
	TRACE((T_INFO|T_STAT, "* Cache Disk High WM      : %d\n",  gscx__config->disk_high_wm));
	TRACE((T_INFO|T_STAT, "* Cache Disk Low WM       : %d\n",  gscx__config->disk_low_wm));
	TRACE((T_INFO|T_STAT, "* Max Path-Lock size      : %d\n",  gscx__config->max_path_lock_size));
	TRACE((T_INFO|T_STAT, "* Access Log Format       : '%s'\n",  vs_data(gscx__config->accesslog_format)));
	TRACE((T_INFO|T_STAT, "* Trace information       : '%s'\n",  vs_data(gscx__config->nc_trace_mask)));
	TRACE((T_INFO|T_STAT, "* Pid file path           : '%s'\n", vs_data(gscx__pid_file)));
	TRACE((T_INFO|T_STAT, "* Force Close             : '%s'\n", (gscx__config->force_close == 1) ? "Enabled":"Disabled"));
	TRACE((T_INFO|T_STAT, "* I/O event               : %s\n", (gscx__config->polling_policy == 1) ? "SELECT":((gscx__config->polling_policy == 2) ?"POLL": "EPOLL"   )));
	TRACE((T_INFO|T_STAT, "* CDN Domain PostFIX      : '%s'\n",  (gscx__config->cdn_domain_postfix?(char *)vs_data(gscx__config->cdn_domain_postfix):"(not-defined)")));

	//TRACE((T_INFO|T_STAT, "* Follow Redirection      : %d(%s)\n", gscx__config->follow_redir, gscx__config->follow_redir==1?"enabled":"disabled"));
	nc_version(gscx__netcache_ver, sizeof(gscx__netcache_ver));
	TRACE((T_INFO|T_STAT, "* Core Version            : %s(rev %d)\n", gscx__netcache_ver, CORE_SVN_REVISION));
	time_t buildtime = BUILD_DATE;
	struct tm *t;
	t = localtime(&buildtime);
	TRACE((T_INFO|T_STAT, "* %s Version        : %s-%d in %s\n", PROG_SHORT_NAME, PROG_VERSION, SVN_REVISION, asctime(t)));

	if (gscx__lua_pool_size > 0){
		TRACE((T_INFO|T_STAT, "* LUA Pool size           : %d\n", gscx__lua_pool_size));
	}
	if (gscx__config->cold_cache_enable) {
		TRACE((T_INFO|T_STAT, "* Cold Cache enable       : %d\n", gscx__config->cold_cache_enable));
	}
	if (gscx__config->cold_cache_ratio) {
		TRACE((T_INFO|T_STAT, "* Cold Cache ratio        : %d\n", gscx__config->cold_cache_ratio));
	}
	if (gscx__config->fastcrc) {
		TRACE((T_INFO|T_STAT, "* FastCRC size            : %d\n",  gscx__config->fastcrc));
	}
	if (gscx__config->core_file_path) {
//		TRACE((T_INFO|T_STAT, "* Core file path          : %s\n", vs_data(gscx__config->core_file_path)));
	}
	if (gscx__config->listen_ip) {
		TRACE((T_INFO|T_STAT, "* Listen IP    : %s \n", vs_data(gscx__config->listen_ip)));
	}
	if (gscx__config->cluster_ip) {
		TRACE((T_INFO|T_STAT, "* Multicast cluster IP    : %s (/w TTL=%d)\n", vs_data(gscx__config->cluster_ip), gscx__config->cluster_ttl));
	}
	else {
		TRACE((T_INFO|T_STAT, "* Multicast cluster IP    : (not-defined)\n"));
	}
	for (int i = 0; i <gscx__cache_part_count; i++) {
		TRACE((T_INFO|T_STAT, "* Caching disk partition  : %s\n", vs_data(gscx__cache_part[i])));
	}

	TRACE((T_INFO|T_STAT, "*****************************************************\n"));
}

#if 0
static int
scx_check_permission(char * path, int mode)
{
	struct stat 	s;
	int 			r;
	if (r = access(fpath_crt, mode)) {
		TRACE((T_ERROR, "certificate '%s' fail(%s)\n", fpath_crt, strerror(errno)));
		return -1;
	}
	stat(fpath_crt, &s);
	if (s.st_uid != 0 || s.st_gid  != 0) {
		TRACE((T_ERROR, "certificate '%s' isn't root ownership\n", fpath_crt));
		return -1;
	}

	if (s.st_mode & (S_IWGRP|S_IXGRP| S_IWOTH| S_IXOTH) != 0) {
		TRACE((T_ERROR, "certificate '%s' must have not excute,write permission except owner\n", fpath_crt));
		return -1;
	}
	if ((s.st_mode & S_IFREG) == 0) {
		TRACE((T_ERROR, "certificate '%s' isn't a regular file\n", fpath_crt));
		return -1;
	}
	crt_sz = s.st_size;
}
#endif
#include "lib_solbox_jwt.h"
int main(int argc, char *const *argv)
{
	int 	option_index = 0;
	int 	c;
	int 	v;
	time_t 	now;
	struct tm 	*tm;
	int		child_msg_len = 0;
	char 	strpath[256] = {'0',};

#if 0
	now = time(NULL);
	tm = localtime(&now);
	if (tm->tm_zone) {
		fprintf(stderr, "CURRENT TIME ZONE = '%s'\n", tm->tm_zone);
		setenv("TZ", tm->tm_zone, 1);
	}
	tzset();
	getchar();
#endif

	char 	*confpath = getenv("CSA_CONFPATH");
	char 	*pidfile = getenv("CSA_PIDFILE");

	gscx__global_mp = mp_create(1024*1024); /* 전역설정 저장용 메모리를 1메가로 할당 */
	if (confpath) {
		gscx__config_root = vs_strdup_lg(gscx__global_mp, confpath);
	}
	else {
		sprintf(strpath, "/usr/service/etc/%s", PROG_SHORT_NAME);
		gscx__config_root = vs_strdup_lg(gscx__global_mp, strpath);
	}

	if (pidfile) {
		gscx__pid_file = vs_strdup_lg(gscx__global_mp, pidfile);
	}
	else {
		sprintf(strpath, "/var/run/%s.pid", PROG_SHORT_NAME);
		gscx__pid_file = vs_strdup_lg(gscx__global_mp, strpath);
	}

	memset(gscx__child_msg_file, 0, 256);
	snprintf(gscx__child_msg_file, 256, "%s/.%s_msg_file", vs_data(gscx__config_root), PROG_SHORT_NAME);
	memset(gscx__default_log_path, 0, 256);
	snprintf(gscx__default_log_path, 256, "/usr/service/logs/%s", PROG_SHORT_NAME);

	while (1) {
		c = getopt_long(argc, argv, "D:p:vhrC", long_options, &option_index);
		//fprintf(stderr, "option, '%c' (%s)\n", c, optarg);
		if (c == -1) break;
		switch (c) {
			case 0:
				if (optarg) {
					*long_options[option_index].flag = atoi(optarg);
				}
				break;
			case 'h':
				usage(argv[0]);
				exit(0);
				break;
			case 'C': /* Print configuration file information */
				scx_conf_dump_from_shm();
				exit(0);
				break;
			case 'D':
				gscx__nc_trace_mask = vs_strdup_lg(gscx__global_mp, optarg);
				break;
			case 'p':
				gscx__pid_file = vs_strdup_lg(gscx__global_mp, optarg);
				break;
			case 'v': /* version */
				trc_set_mask("warn,error"); //이렇게 안하면 nc_version을 실행 할때 메시지가 나온다.
				nc_version(gscx__netcache_ver, sizeof(gscx__netcache_ver));
				fprintf(stderr, "Core Version : %s(rev %d)\n", gscx__netcache_ver, CORE_SVN_REVISION);
//				fprintf(stderr, "%s Version : %s-%d\n", PROG_SHORT_NAME, PROG_VERSION,SVN_REVISION);
				time_t buildtime = BUILD_DATE;
				struct tm *t;
				t = localtime(&buildtime);
				fprintf(stderr, "%s Version : %s-%d in %s\n", PROG_SHORT_NAME, PROG_VERSION,SVN_REVISION, asctime(t));

				exit(0);
				break;
			default:
				fprintf(stderr, "option '%c' - unknown\n", c);
				usage(argv[0]);
				exit(1);
				break;

		}

	}
	if (access(vs_data(gscx__config_root), R_OK|X_OK)) {
		fprintf(stderr, "%s conf directory is '%s' is invalid(%s)\n", PROG_SHORT_NAME, vs_data(gscx__config_root), strerror(errno));
		exit(1);
	}
	scx_init_proctitle(argc, argv);
/*
 * 글로벌 변수들 설정 (from default site)
 */
	scx_lookup_my_address();

	scx_check_aesni();

//	scx_report_environment();	//여기서는 동작 안함
	fprintf(stdout, "running %s.\n", PROG_SHORT_NAME);
	fflush(stdout);
#if 0
/* jwt 임시 테스트용 코드 */
	jwt_t *jwt = NULL;
//	char token_str[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ.ajB221IVz7aggEfTp3jUPc7UBw5xemmp-LmrmEgFETU";
//	char psk[] = "testsecret";
	char token_str[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ.UiBNUsddJ5kp1UHphIt0V4C_3YnrX21kFoFXrBRSNwE";
	char psk[] = "anothersecret";
	int psk_len = strlen(psk);
	int result = 0;
	jwt = jwt_new();
	jwt_set_opt(jwt, JWT_OPT_VERIFY_HEADER | JWT_OPT_VERIFY_SIG);

	result = jwt_decode(jwt, (char *)token_str, psk, psk_len);

	printf("jwt result = %d\n", result);

return 0;
#endif
	if (gscx__run_standalone) {
		main_service(1);
		fprintf(stderr, "main service gracefully shutdowned\n");
	}
	else {
		int 	pid;

		setsid();
		daemon(0, 1);
		scx_write_pidfile();//daemon으로 동작할 때만 pid 파일을 생성하도록 한다.
		nce_monitor_service();

	}
	if (NULL != gscx__global_mp) {
		mp_free(gscx__global_mp);
		gscx__global_mp = NULL;
	}
//	scx_site_destroy(gscx__default_site); /* main process에서는 gscx__default_site를 다시 사용 하는 경우가 없으므로 여기서 삭제한다.*/
	return 0;

}


void
nce_shutdown_handler(int sig)
{
#if 0	/* main 처리 루틴을 thread로 분리 하면서 로딩 중에도 종료를 할수 있도록 변경 되었음 */
	/*
	 * 서비스 초기화가 끝났을때에만 아래의 기능이 동작하도록 한다.
	 * nc_init()도중에 syslog()를 호출하면 deadlock이 발생한다.
	 */
	if (1 == gscx__service_available) {
#endif
		syslog(LOG_PID|LOG_INFO, "signal %d caught\n", sig);
		TRACE((T_INFO|T_STAT, "signal %d caught\n", sig));
		gscx__signal_shutdown = 1;
#if 0
	}
#endif
}

static void *
scx_make_request(void *cls, const char *uri)
{
	void 	*req = NULL;
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};
	req = (void *)nc_create_request(cls, (char *)uri);
	/************************** Uri Parse Phase Handler가 실행 되는 부분 *************************************/
	phase_ctx.req = (void *)req;
	if (SCX_YES != scx_phase_handler(PHASE_URI_PARSE, &phase_ctx)) {

	}
	return req;
}

/*
 * client의 IP에 따른 접속차단(ACL, access control list)이 필요할 경우 scx_accept_policy function에서 하면 된다.
 * MHD_AcceptPolicyCallback은 socket 이 처음 연결 될때 호출 되므로 Keep-Alive를 사용하는 경우에는 처음 한번만 호출이 되고
 * 이후 부터는 MHD_OPTION_URI_LOG_CALLBACK만 호출 된다.
 */
static int
scx_accept_policy(void *cls, const struct sockaddr *addr,  socklen_t addrlen)
{
	int 	sidx = (int)cls;
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};
	char 	client_ip[32] = {"\0"};
#if 0
	static count = 0;
	if ((count++ % 20) == 0) {
		printf("return\n");
		return MHD_NO;
	}
	else printf("pass\n");
#endif
	if (!gscx__service_available) {	/* http port가 열린 상태라도 초기화 과정이 끝나지 않았으면 요청을 거부한다. */
		return MHD_NO;
	}

    switch(addr->sa_family) {
        case AF_INET:
            inet_ntop(AF_INET, &(((struct sockaddr_in *)addr)->sin_addr), client_ip, 32);
            break;
        case AF_INET6:
            inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)addr)->sin6_addr), client_ip, 32);
            break;
        default:
            strncpy(client_ip, "Unknown AF", 32);
    }
	TRACE((T_DAEMON, "%s() called. IP = %s\n", __func__, client_ip));


	/************************** Accept Policy Phase Handler가 실행 되는 부분 *************************************/
	/* IP차단을 할 경우 MHD_NO를 리턴하면 된다. */
	phase_ctx.addr = (void *)addr;
	if (SCX_YES != scx_phase_handler(PHASE_ACCEPT_POLICY, &phase_ctx)) {
		return MHD_NO;
	}
	return MHD_YES;
}

static void leakdump(char *line)
{
    TRACE((T_ERROR, "%s\n", line));
}

void scx_dict_init()
{
	/* dict 라이브러리의 메모리 할당 function 지정 */
	dict_malloc_func = scx_dict_malloc;
	dict_free_func = scx_dict_free;
}

static void *
main_service_thread(void *d)
{
	int	service_pid = -1;
	int	load_count = *(int *)d;
	int	i = 0;
	uint32_t 	concurrent_count;
	int loop_count = 0;
	service_pid = getpid();
	off_t			st_size;	/* 설정 파일의 크기 */
	srand ((unsigned long)scx_mix(clock(), time(NULL), service_pid));

	ATOMIC_SET(gscx__id_seq, (uint64_t)rand()*(uint64_t)1000000000);	/* unique ID의 시작을 random 하게 만든다. */

	snprintf(gscx__default_conf_path, sizeof(gscx__default_conf_path), "%s/default.conf", vs_data(gscx__config_root));
	//gscx__default_site = scx_site_create("default");
	gscx__default_site = scx_site_create(gscx__default_conf_path);
	if(gscx__default_site == NULL) {
		//fprintf(stderr, "Failed to open %s/etc/solproxy.d/%s\n", vs_data(gscx__config_root),"default");
		fprintf(stderr, "Failed to open %s\n", gscx__default_conf_path);
		syslog(LOG_PID|LOG_ERR, "%s(%d) Failed to open %s", PROG_SHORT_NAME, service_pid,gscx__default_conf_path);
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	gscx__config = scx_init_config(gscx__default_site);
	gscx__config->hash = vm_make_config_hash(gscx__default_conf_path, &st_size); /* default.conf 파일의 hash 값을 저장한다. */

	/*
	 * sp 로그를 사용할수 있게 한다.
	 * 여기 부터 로그가 기록됨
	 */
	if(nce_init_sp_log() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	/* content router 서비스 시작 */
	if (cr_start_service() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	/* streaming type 통계를 사용하는지 확인 한다. */
	scx_check_stat_write_type();

	if(nce_load_global_params(gscx__default_site, gscx__config, FALSE) < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	nce_reload_default_cert(FALSE);

	/* 시스템에 충분한 메모리가 있는지 확인한다. */
	if(scx_check_memory() != SCX_YES) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	scx_dict_init();

	/* user 권한으로 변경 하기 이전에 service port를 열어야지 permition 문제에 걸리지 않는다. */
	if(start_http_service(load_count) < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	if (0 != gscx__config->polling_policy) {
		/* epoll 방식이 아닌 경우에는 비동기 동작을 하지 않는다. */
		scx_disable_async_method();
	}
	else if (0 == scx_get_uint(gscx__default_site, SV_ENABLE_ASYNC_CALLBACK, 1)) {
		/*
		 * 1.5 버전 부터는 비동기를 기본으로 한다.
		 */
		scx_disable_async_method();
	}
	if (start_rtmp_service() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}

	/*
	 * netcache core에서 localhost로 들어온 요청을 처리하는 서비스를 생성한다.
	 * 이 부분은 solproxy의 비동기 처리가 가능해지면 필요가 없다.
	 */
	if (start_check_listening_service() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}

	if(nce_init() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	if(shm_log_init() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	scx_timer_init();
	if(standalonestat_init() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	if(mdc_init() < 0) {
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	if (SCX_YES != scx_init_module()) {
		TRACE((T_ERROR, "Failed to init module.\n"));
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	/* 데몬 시작시 모든 볼륨 로딩 */
	if (vm_create_config(0) < 0)
	{
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		/* nce_shutdown() 에서 resource 를 정리해 주므로, 여기서 따로 정리 안해도 된다. */
		goto L_shutdown;
	}

	if (1 == load_count) {	/* 정상 시작 시에만 호출이 되고 비정상 종료로 인한 재시작 시에는 호출 되어서는 안된다. */
		int clean_start = 0;
		clean_start = scx_get_uint(gscx__default_site, SV_CLEAN_START, 0);
		if (1 == clean_start) {
			vm_all_volume_purge();
		}
	}
	/* allow_dynamic_vhost가 0으로 된 경우에도 아래의 특수 볼륨을 생성하기 위해 임시로 설정을 바꾼다. */
	int old_val = gscx__config->allow_dynamic_vhost;
	gscx__config->allow_dynamic_vhost = 1;
	vm_add(OTHERS_VOLUME_NAME, 0);	/* crossdomain.xml 요청 처리를 위한 가상 볼륨 생성 */
	vm_add(CHECK_VOLUME_NAME, 0);	/* 모니터링 요청을 처리하기 위한 가상 볼륨 생성 */
	gscx__config->allow_dynamic_vhost = old_val;

	init_log_format();
	nce_build_header_dict();
	scx_lua_build_header_dict();
	if(gscx__lua_pool_size > 0) {
		gscx__lua_pool = lua_pool_create(gscx__lua_pool_size);
	}
	pl_init();
	scx_init_rsapi();
	scx_init_status();
	if(strm_init() == 0) {
		TRACE((T_ERROR, "streaming init failed.\n"));
		gscx__need2parent_kill = 1; //부모 프로세스까지 같이 죽인다.
		goto L_shutdown;
	}
	if (gscx__enable_async) /* 비동기 처리를 하지 않을때에는 worker thread pool을 별도로 생성할 필요가 없다. */
		scx_create_thread_pool();
	signal(SIGSEGV, nce_signal_handler);	/* netcache에서  stack dump를 남기지 않고 SolProxy 에서 남기기 위해 SIGSEGV을 따로 분리 */

	gscx__start_time = scx_update_cached_time_sec();
	scx_permit_expired_request();	/* 인증이 만료된 요청에 대해 일정시간 동안 허용하도록 설정한다. */
	scx_parent_kill(1);	/* 초기화 과정이 끝났으로 이후 과정에서 죽는 현상이 발생하면 재시작 할수 있도록 마킹을 한다.*/
	gscx__service_available = 1; /* 초기화가 완료된 시점 부터 client의 request를 받는다. */
	i = 1;
	while (gscx__signal_shutdown == 0) {
		logger_flush(scx_access_logger);
		logger_flush(scx_error_logger);
		logger_flush(scx_origin_logger);
		if(i % 10 == 0) {
			scx_write_status();
		}
		if(i % 60 == 0) {
			i = 0;
			TRACE((T_STAT|T_INFO, "**** Response time moving average(%lld ticks) : stddev=%.2f mavg=%.2f(max=%.2f,min=%.2f), open=%.2f, read=%.2f, close=%.2f \n",
						gscx__mavg_cnt, (float)scx_make_stddev(), gscx__mavg_dur, gscx__mavg_max, gscx__mavg_min, gscx__mavg_dur_open, gscx__mavg_dur_read, gscx__mavg_dur_close));
			TRACE((T_STAT|T_INFO, "**** Total Allocated Memory %.2f MB \n", ATOMIC_VAL(g__scx_adynamic_memory_sum) / 1048576.0 ));
			scx_get_memory_usage(service_pid);
			scx_write_concurrent_status();
			scx_write_system_status();
			if (gscx__thpool)
				thread_pool_status(gscx__thpool);
			strm_write_cache_status();
		}
		scx_update_system_status();
#if 0	//30초후 정상종료
		if(i > 30) {
			goto L_shutdown;
		}
#endif
		i++;
		bt_msleep(1000);
	}
L_shutdown:
	TRACE((T_INFO|T_STAT, "Graceful shutdown started\n"));
	/* 여기까지 오는 경우는 서비스 종료가 필요한 경우라고 판단 한다. */
	scx_parent_kill(0); //parent를 같이 종료
	scx_used_memory_list();
	gscx__service_available = 0;
	gscx__signal_shutdown = 1; /* 초기 시작시에 에러가 발생한 경우 이 값이 marking이 되지 않는 경우도 있어서 이렇게 처리함 */
	/* 종료 전에 더이상 비동기 처리가 되지 않도록 한다 */
	scx_disable_async_method();
	/* 연결된 client가 모두 끊어질 때까지 대기 한다. */
	while( (concurrent_count = ATOMIC_VAL(gscx__server_stat.concurrent_count)) > 0) {
		TRACE((T_INFO|T_STAT, "Waiting for the connection to close. concurrent(%d)\n", concurrent_count));
		bt_msleep(200);
		if (loop_count++ > 50) break; /* 10초 이상 기다려도 사용자 정리가 되지 않는 경우는 그냥 종료한다. */
	}
	stop_rtmp_service();
	scx_write_concurrent_status();
	for (i = 0; i < SCX_PORT_TYPE_CNT; i++) {
		if (gscx__http_servers[i]) {
			TRACE((T_INFO|T_STAT, "shutdowning http server, %d\n", i));
			MHD_stop_daemon(gscx__http_servers[i]);
		}
	}
#if 0
	/* 혹시라도 종료 과정에 문제가 생겨서 무한 재시작 하는걸 방지 하기 위해 parent process에서 재시작 하지 못하도록 한 후 나머지 종료 과정을 진행한다. */
	if(gscx__need2parent_kill == 1) { //parent를 같이 종료 해야 하는 경우
		scx_parent_kill(0);
	}
#endif
	nce_shutdown();
	nc_raw_dump_report(leakdump);
	return NULL;
}

static int
main_service(int load_count)
{
	struct sigaction 	sa_old, sa_new;

//	char szPathDefaultFile[512] = { 0, };

	umask(0);
	gscx__service_available = 0;
	set_resource_limits();
	scx_setup_codepage();
	scx_init_method_map();
	sa_new.sa_handler = nce_shutdown_handler;
	sigemptyset(&sa_new.sa_mask);
	sa_new.sa_flags = 0;
	sigaction(SIGINT, &sa_new, &sa_old);
	sigaction(SIGTERM, &sa_new, &sa_old);

#ifndef USE_SIGTERM_SIGNAL	/* signal을 사용는 경우 아래 처럼 파라미터가 0로 들어가면 바로 부모 프로세스를 죽인다. */
	scx_parent_kill(0);	/* 초기화 과정에서 죽는 경우가 발생하면 parent process에서 감지 해서 무한 재시작이 될수 있어서 재시작이 안되도록 마킹을 한다. */
#endif

	pthread_t 	main_service_thread_tid;
	int 		status;
	pthread_create(&main_service_thread_tid, NULL, (void *)main_service_thread, (void *) &load_count);
	pthread_join(main_service_thread_tid, (void **)&status);

	TRACE((T_INFO|T_STAT, "Graceful shutdown is checking if parent needs to be kill(%d)\n", gscx__need2parent_kill));

}

#define MAX_MHD_OPTION_SIZE	20	/* array로 미리 만들어야 하기때문에 최대 20개로 한다. */

/*
 * 지정된 옵션 Array에 추가 및 변경 한다.
 * 값(option)이 opt_array에 없는 경우는 추가를 하고 있는 경우는 업데이트를 한다.
 */
static int
mhd_option_addnreplace(struct MHD_OptionItem *opt_array, int option, intptr_t value, void *ptr_value)
{
	int i = 0;

	for (i = 0; i < MAX_MHD_OPTION_SIZE; i++) {
		if (MHD_OPTION_END == opt_array[i].option) {
			/* 추가 */
			if (i == (MAX_MHD_OPTION_SIZE-1) ) {
				/* array를 모두 사용한 경우 */
				TRACE((T_ERROR, "Exceed Max mhd option count(%d).option(%d)\n", MAX_MHD_OPTION_SIZE, option));
				return SCX_NO;
			}
			opt_array[i].option = option;
			opt_array[i].value = value;
			opt_array[i].ptr_value = ptr_value;
			opt_array[i+1].option = MHD_OPTION_END;
			opt_array[i+1].value = 0;
			opt_array[i+1].ptr_value = NULL;
			break;

		}
		else if (option == opt_array[i].option) {
			/* 변경(업데이트) */
			opt_array[i].value = value;
			opt_array[i].ptr_value = ptr_value;
			break;
		}
	}
	return SCX_YES;
}

/*
 * 지정된 옵션을 Array로 부터 삭제한다.
 * 값(option)이 opt_array에 있는 경우는 삭제 하고 없는 경우에도 정상 리턴한다.
 */
static int
mhd_option_remove(struct MHD_OptionItem *opt_array, int option)
{
	int i = 0, j = 0;

	for (i = 0; i < MAX_MHD_OPTION_SIZE; i++) {
		if (option == opt_array[i].option) {
			/* 옵션을 찾은 위치 뒤에 있는 것들을 모두 한칸씩 앞당긴다 */
			for (j = i; MHD_OPTION_END != opt_array[j].option; j++) {
				opt_array[j].option = opt_array[j+1].option;
				opt_array[j].value = opt_array[j+1].value;
				opt_array[j].ptr_value = opt_array[j+1].ptr_value;
			}
			break;
		}
	}
	return SCX_YES;
}
#if 0
char dhparms[] = {"-----BEGIN DH PARAMETERS-----\
MIICCAKCAgEAlWHPUp8n5rI9pzvn1SWOFvHurPUcilmAc0K0dHbpjXNx8i9PJr9W\
13P5ByfoS1EIVgOnGei6GoTw+akl5rSjWEFQL+yr+uOjgprCIWCFKh7U/9Fe3Ht9\
eRFFV+o6fBPj3cuZFfg7hxj+nYNY0F5W/QLoYV3EzZ/b7U0c/f4ZElXV0GAyBdkS\
Myuenhger29i56G8X0Q3Ir1qIZDI6wJsVlozY0Z3vI27D9L2olk/VyDZFyyYwgzE\
/u1ZqY3JIuo4jE//JLWc0PcOMg/OoYN5URyGXSzcQy0jR4cEmZo8ycovMkqY8dxq\
x1tE8VoPD7fnhtc4b4M9UhtRhfqRJj/ngc6VeLxV7J9dyK4AMeZAWsyTPFuYsciA\
jjzzz7XFbhEoi+OXtwFNWqsXG8r2LcxLtjFQT6/qDlqBObHR7T2d8GRw7/6vQLc5\
WAqDPZ3aKjRGZlm0ECX6F6vhpugQUBZOOpaXqI4tzCYEqcwx3i5eyBzqoIQ7H62c\
K0kDaOgscmV96m6Wo8MOTaH7jIfXYjvbxNvx9VHqAORG0dawamyiAY12jfyT9ZGM\
0r/0Nv0DLGTQqDdzTjm+Nyp1+cAxnk+nrRyGYpDzLC88PK7FbkbJanRT1pcxnlYb\
uKVPF2q1pWJym9ebmAnC9M3Sv/IZHTNKsC17ZGql2Oh6y+3gP9bEx7MCAQI=\
-----END DH PARAMETERS-----"};
#endif
static int
start_http_service(int load_count)
{
	struct sockaddr_in http_addr, https_addr;
	int i;
	int	retry_count = 0;
	struct MHD_OptionItem options[MAX_MHD_OPTION_SIZE] = {
		{MHD_OPTION_CONNECTION_LIMIT, 60000, NULL},
//		{MHD_OPTION_CONNECTION_MEMORY_LIMIT, 32768, NULL},
		{MHD_OPTION_CONNECTION_TIMEOUT, gscx__config->request_timeout, NULL},
		{MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)&scx_make_request, (void *)SCX_HTTP},
		{MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)&scx_async_request_completed, (void *)SCX_HTTP},
		{MHD_OPTION_NOTIFY_CONNECTION, (intptr_t)&scx_notify_connection, (void *)SCX_HTTP},	/* socket이 연결이나 해제될때 호출되는 function 등록 */
		{MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)&sx_logger, 0},
		{MHD_OPTION_THREAD_POOL_SIZE, gscx__config->workers, 0},
		{MHD_OPTION_SOCK_ADDR, 0, NULL},
		{MHD_OPTION_LISTENING_ADDRESS_REUSE, 1, NULL},	/* TIME_WAIT 상태의 소켓을 재사용, kernel 3.0이상 부터 사용가능, centos 6.x에는 사용 불가 */
		{MHD_OPTION_END, 0, NULL}
	};
	struct MHD_OptionItem s_options[MAX_MHD_OPTION_SIZE] = {	//HTTPS용 옵션이 들어간다.
		{MHD_OPTION_CONNECTION_LIMIT, 60000, NULL},
//		{MHD_OPTION_CONNECTION_MEMORY_LIMIT, 32768, NULL},
		{MHD_OPTION_CONNECTION_TIMEOUT, gscx__config->request_timeout, NULL},
		{MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)&scx_make_request, (void *)SCX_HTTPS},
		{MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)&scx_async_request_completed, (void *)SCX_HTTPS},
		{MHD_OPTION_NOTIFY_CONNECTION, (intptr_t)&scx_notify_connection, (void *)SCX_HTTPS},	/* socket이 연결이나 해제될때 호출되는 function 등록 */
		{MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)&sx_logger, 0},
		{MHD_OPTION_THREAD_POOL_SIZE, gscx__config->workers, 0},
		{MHD_OPTION_HTTPS_MEM_KEY, 0, NULL},
		{MHD_OPTION_HTTPS_MEM_CERT, 0, NULL},
//		{MHD_OPTION_HTTPS_MEM_DHPARAMS, 0, dhparms},
#if GNUTLS_VERSION_MAJOR >= 3
		{MHD_OPTION_HTTPS_CERT_CALLBACK, NULL, NULL},
//		{MHD_OPTION_HTTPS_PRIORITIES, 0, "NONE:+AES-256-GCM:+SHA384"},
#endif
		{MHD_OPTION_SOCK_ADDR, 0, NULL},
		{MHD_OPTION_LISTENING_ADDRESS_REUSE, 1, NULL},	/* TIME_WAIT 상태의 소켓을 재사용 */
		{MHD_OPTION_END, 0, NULL}
	};
	struct MHD_OptionItem t_options[MAX_MHD_OPTION_SIZE] = {	//TPROXY관련 옵션이 들어간다.
		{MHD_OPTION_CONNECTION_LIMIT, 20000, NULL},
		{MHD_OPTION_CONNECTION_TIMEOUT, gscx__config->request_timeout, NULL},
		{MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)&scx_make_request, (void *)SCX_HTTP_TPROXY},
		{MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)&scx_async_request_completed, (void *)SCX_HTTP_TPROXY},
		{MHD_OPTION_NOTIFY_CONNECTION, (intptr_t)&scx_notify_connection, (void *)SCX_HTTP_TPROXY},	/* socket이 연결이나 해제될때 호출되는 function 등록 */
		{MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)&sx_logger, 0},
		{MHD_OPTION_LISTENING_ADDRESS_REUSE, 1, NULL},	/* TIME_WAIT 상태의 소켓을 재사용 */
		{MHD_OPTION_END, 0, NULL}
	};
	struct MHD_OptionItem m_options[MAX_MHD_OPTION_SIZE] = { 	// Management Port 관련 옵션이 들어간다.
		{MHD_OPTION_CONNECTION_LIMIT, 2000, NULL},
		{MHD_OPTION_CONNECTION_TIMEOUT, gscx__config->request_timeout, NULL},	/* streaming 기능 때문에 대기 시간을 길게 가져간다. client에서 TCP Window Full이 발생하면 전송이 안되기 때문에  */
		{MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)&scx_make_request, (void *)SCX_MANAGEMENT},
		{MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)&scx_async_request_completed, (void *)SCX_MANAGEMENT},
		{MHD_OPTION_NOTIFY_CONNECTION, (intptr_t)&scx_notify_connection, (void *)SCX_MANAGEMENT},	/* socket이 연결이나 해제될때 호출되는 function 등록 */
		{MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)&sx_logger, 0},
		{MHD_OPTION_THREAD_POOL_SIZE, 4, 0},		/* management Port는 worker용 thread가 많을 필요가 없다 */
		{MHD_OPTION_SOCK_ADDR, 0, NULL},
		{MHD_OPTION_LISTENING_ADDRESS_REUSE, 1, NULL},	/* TIME_WAIT 상태의 소켓을 재사용, kernel 3.0이상 부터 사용가능, centos 6.x에는 사용 불가 */
		{MHD_OPTION_END, 0, NULL}
	};
	int tsock_fd;
	unsigned int run_http_options, run_https_options;

	if(gscx__config->polling_policy == 1)
	{	/* centos 5.x일때는 select만 가능하다. */
		run_http_options = MHD_USE_SELECT_INTERNALLY  | MHD_USE_DEBUG | MHD_ALLOW_SUSPEND_RESUME;
		run_https_options = MHD_USE_SELECT_INTERNALLY | MHD_USE_DEBUG | MHD_USE_TLS |MHD_ALLOW_SUSPEND_RESUME;
	}
	else if(gscx__config->polling_policy == 2)
	{
//		run_http_options = MHD_USE_POLL_INTERNALLY  | MHD_USE_DEBUG;
		run_http_options = MHD_USE_POLL_INTERNALLY | MHD_USE_THREAD_PER_CONNECTION| MHD_USE_DEBUG;
//		run_https_options = MHD_USE_POLL_INTERNALLY | MHD_USE_DEBUG | MHD_USE_TLS;
		run_https_options = MHD_USE_POLL_INTERNALLY | MHD_USE_THREAD_PER_CONNECTION| MHD_USE_DEBUG|MHD_USE_TLS;
		/* MHD_USE_THREAD_PER_CONNECTION에서는 notify callback이 정상동작을 하지 않아서 제거한다. */
		mhd_option_remove(options, MHD_OPTION_NOTIFY_CONNECTION);
		mhd_option_remove(s_options, MHD_OPTION_NOTIFY_CONNECTION);
		mhd_option_remove(t_options, MHD_OPTION_NOTIFY_CONNECTION);
		mhd_option_remove(m_options, MHD_OPTION_NOTIFY_CONNECTION);
		/* MHD_OPTION_THREAD_POOL_SIZE은 MHD_USE_THREAD_PER_CONNECTION 옵션과 같이 사용이 불가능하다 */
		mhd_option_remove(options, MHD_OPTION_THREAD_POOL_SIZE);
		mhd_option_remove(s_options, MHD_OPTION_THREAD_POOL_SIZE);
		mhd_option_remove(m_options, MHD_OPTION_THREAD_POOL_SIZE);
	}
	else {	/* MHD 0.9.51에서 MHD_USE_EPOLL_LINUX_ONLY가 MHD_USE_EPOLL로 바뀜 */
		//run_http_options = MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL_LINUX_ONLY | MHD_USE_EPOLL_TURBO | MHD_USE_DEBUG;
//		run_http_options = MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL_LINUX_ONLY | MHD_USE_EPOLL_TURBO | MHD_USE_DEBUG|MHD_ALLOW_SUSPEND_RESUME|MHD_USE_IPv6;
		run_http_options = MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL_LINUX_ONLY | MHD_USE_EPOLL_TURBO | MHD_USE_DEBUG|MHD_ALLOW_SUSPEND_RESUME;
		/*
		 * https인 경우에는 MHD_USE_EPOLL_TURBO 옵션을 사용하는 경우 비정상 요청이 들어 오는 경우 CPU를 100% 사용하는 문제가 있어서 해당 옵션을 사용하지 않는다.
		 * https://jarvis.solbox.com/redmine/issues/33495 일감 참고
		 */
		run_https_options = MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL_LINUX_ONLY | MHD_USE_DEBUG | MHD_USE_TLS |MHD_ALLOW_SUSPEND_RESUME;
	}

	if (gscx__config->listen_ip) {
		//MHD_OPTION_SOCK_ADDR을 사용할 경우 MHD_start_daemon시의 port 옵션이 무시되므로
		//여기에 port 옵션을 같이 설정해 주어야 한다.
		http_addr.sin_family = AF_INET;
		http_addr.sin_port = htons(gscx__config->http_port);
		inet_pton(AF_INET,  (const char *)vs_data(gscx__config->listen_ip), (void *)&(http_addr.sin_addr.s_addr));
		if (mhd_option_addnreplace(options, MHD_OPTION_SOCK_ADDR, 0,  (struct sockaddr *)&(http_addr)) != SCX_YES) {
			return -1;
		}
	}
	retry_count = 0;
retry_http:
	gscx__http_servers[SCX_HTTP] = MHD_start_daemon(
						run_http_options,
						gscx__config->http_port,
						scx_accept_policy,	/* MHD_AcceptPolicyCallback */
						(void *)SCX_HTTP,	/* apc_cls extra argument */
						&scx_request_handler,
						(void *)SCX_HTTP,
						MHD_OPTION_ARRAY, options,
						MHD_OPTION_END);

	if (gscx__http_servers[SCX_HTTP] == NULL) {
		retry_count++;
		/* binding 실패할 경우 시간을 두고 몇번(300초) 더 시도 한다. */
		if (EADDRINUSE == errno && 300 > retry_count) {
			TRACE((T_WARN, "failed to bind http port %d(%s), retry count %d.\n", gscx__config->http_port, strerror(errno), retry_count));
			bt_msleep(1000);
			goto retry_http;
		}
		TRACE((T_ERROR, "failed to start http server on port %d(%s), shutdowning\n", gscx__config->http_port, strerror(errno)));
		if ( ((errno == ENOPROTOOPT) || (errno == ENOENT))
				&& 2 > retry_count) {
			/*
			* SO_REUSEPORT를 지원하지 않는 커널의 경우 MHD_OPTION_LISTENING_ADDRESS_REUSE를 1로 설정하면
			* Protocol not available에러가 발생하면서 port binding이 되지 않아서 데몬이 기동되지 않는다.
			* 이런 문제를 회피하기 위해 MHD_OPTION_LISTENING_ADDRESS_REUSE 옵션을 빼고 기동을 다시 시도한다.
			* 이 경우 MHD 버전에 따라 errno에 ENOPROTOOPT가 설정(0.9.59이전)되거나 ENOEN가 설정(0.9.60)된다.
			*/
			mhd_option_remove(options, MHD_OPTION_LISTENING_ADDRESS_REUSE);
			mhd_option_remove(s_options, MHD_OPTION_LISTENING_ADDRESS_REUSE);
			mhd_option_remove(t_options, MHD_OPTION_LISTENING_ADDRESS_REUSE);
			mhd_option_remove(m_options, MHD_OPTION_LISTENING_ADDRESS_REUSE);
			TRACE((T_WARN, "Remove SO_REUSEPORT socket option, retry count %d.\n", retry_count));
			goto retry_http;
		}
		return -1;
	}
	TRACE((T_INFO|T_STAT, "HTTP service successfully launched on port %d\n", gscx__config->http_port));

	if(gscx__config->tproxy_port) {
		tsock_fd = scx_make_tproxy_socket(gscx__config->tproxy_port);
		if(!tsock_fd)
		{
			return -1;
		}
		retry_count = 0;
retry_tproxy:
		gscx__http_servers[SCX_HTTP_TPROXY] = MHD_start_daemon(
								run_http_options,
								gscx__config->tproxy_port,
								scx_accept_policy,	/* MHD_AcceptPolicyCallback */
								(void *)SCX_HTTP_TPROXY,	/* apc_cls extra argument */
								&scx_request_handler,
								(void *)SCX_HTTP_TPROXY,
								MHD_OPTION_ARRAY, t_options,
								MHD_OPTION_THREAD_POOL_SIZE, gscx__config->workers,
								MHD_OPTION_LISTEN_SOCKET, tsock_fd,
								MHD_OPTION_END);

		if (gscx__http_servers[SCX_HTTP_TPROXY] == NULL) {
			/* binding 실패할 경우 시간을 두고 몇번(300초) 더 시도 한다. */
			if (EADDRINUSE == errno && 300 > retry_count++) {
				TRACE((T_WARN, "failed to bind TPROXY port %d(%s), retry count %d.\n", gscx__config->tproxy_port, strerror(errno), retry_count));
				bt_msleep(1000);
				goto retry_tproxy;
			}
			TRACE((T_ERROR, "failed to start TPROXY server on port %d(%s), shutdowning\n", gscx__config->tproxy_port, strerror(errno)));
			return -1;
		}
	}
#if GNUTLS_VERSION_MAJOR >= 3

#if 0	/* 아래의 기능들이 필요한지 모르겠음 */
	gcry_control (GCRYCTL_ENABLE_QUICK_RANDOM, 0);
#ifdef GCRYCTL_INITIALIZATION_FINISHED
	gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif
#endif
#endif
	/* sni 기능 사용시는 default 인증서가 없어도 https port open이 가능 하다. */
	int enable_https = 0;
	if (gscx__config->https_port > 0 ) {
		if (gscx__config->enable_sni == 1 ||
					gscx__default_site->certificate != NULL) {
			enable_https = 1;
		}
	}
	if (enable_https) {

		if (gscx__config->listen_ip) {
			//MHD_OPTION_SOCK_ADDR을 사용할 경우 MHD_start_daemon시의 port 옵션이 무시되므로
			//여기에 port 옵션을 같이 설정해 주어야 한다.
			https_addr.sin_family = AF_INET;
			https_addr.sin_port = htons(gscx__config->https_port);
			inet_pton(AF_INET,  vs_data(gscx__config->listen_ip), &(https_addr.sin_addr.s_addr));
			if (mhd_option_addnreplace(s_options, MHD_OPTION_SOCK_ADDR, 0,  (struct sockaddr *)&(https_addr)) != SCX_YES) {
				return -1;
			}
		}

#if GNUTLS_VERSION_MAJOR >= 3
		if(gscx__config->enable_sni == 1) {
			if (mhd_option_addnreplace(s_options, MHD_OPTION_HTTPS_CERT_CALLBACK, 0, &scx_sni_handler) != SCX_YES) {
				return -1;
			}
		}
		else {
#else
		{
#endif
			if (mhd_option_addnreplace(s_options, MHD_OPTION_HTTPS_MEM_KEY, 0, ((st_cert_t *)gscx__default_site->certificate)->key) != SCX_YES) {
				return -1;
			}
			if (mhd_option_addnreplace(s_options, MHD_OPTION_HTTPS_MEM_CERT, 0, ((st_cert_t *)gscx__default_site->certificate)->crt) != SCX_YES) {
				return -1;
			}
		}
		if (gscx__config->ssl_priorities) {
			if (mhd_option_addnreplace(s_options, MHD_OPTION_HTTPS_PRIORITIES, 0, vs_data(gscx__config->ssl_priorities)) != SCX_YES) {
				return -1;
			}
		}
		retry_count = 0;
retry_https:
		gscx__http_servers[SCX_HTTPS] = MHD_start_daemon(
						 run_https_options,
						 gscx__config->https_port,
						 scx_accept_policy,	/* MHD_AcceptPolicyCallback */
						 (void *)SCX_HTTPS,	/* apc_cls extra argument */
						 &scx_request_handler,
						 (void *)SCX_HTTPS,
						 MHD_OPTION_ARRAY, s_options,
						 MHD_OPTION_END);

		if (gscx__http_servers[SCX_HTTPS] == NULL) {
			/* binding 실패할 경우 시간을 두고 몇번(300초) 더 시도 한다. */
			if (EADDRINUSE == errno && 300 > retry_count++) {
				TRACE((T_WARN, "failed to bind https port %d(%s), retry count %d.\n", gscx__config->https_port, strerror(errno), retry_count));
				bt_msleep(1000);
				goto retry_https;
			}
			TRACE((T_ERROR, "failed to start https server on port %d(%s), shutdowning\n", gscx__config->https_port, strerror(errno)));
			return -1;
		}
		TRACE((T_INFO|T_STAT, "HTTPS service successfully launched on port %d\n", gscx__config->https_port));
	}	/* end of if (enable_https) */

/* Management Port open */
	if (gscx__config->management_listen_ip) {
		//MHD_OPTION_SOCK_ADDR을 사용할 경우 MHD_start_daemon시의 port 옵션이 무시되므로
		//여기에 port 옵션을 같이 설정해 주어야 한다.
		http_addr.sin_family = AF_INET;
		http_addr.sin_port = htons(gscx__config->management_port);
		inet_pton(AF_INET,  (const char *)vs_data(gscx__config->management_listen_ip), (void *)&(http_addr.sin_addr.s_addr));
		if (mhd_option_addnreplace(m_options, MHD_OPTION_SOCK_ADDR, 0,  (struct sockaddr *)&(http_addr)) != SCX_YES) {
			return -1;
		}
	}
	retry_count = 0;
retry_management_http:
	gscx__http_servers[SCX_MANAGEMENT] = MHD_start_daemon(
						run_http_options,
						gscx__config->management_port,
						scx_accept_policy,	/* MHD_AcceptPolicyCallback */
						(void *)SCX_MANAGEMENT,	/* apc_cls extra argument */
						&scx_request_handler,
						(void *)SCX_MANAGEMENT,
						MHD_OPTION_ARRAY, m_options,
						MHD_OPTION_END);

	if (gscx__http_servers[SCX_MANAGEMENT] == NULL) {
		/* binding 실패할 경우 시간을 두고 몇번(300초) 더 시도 한다. */
		if (EADDRINUSE == errno && 300 > retry_count++) {
			TRACE((T_WARN, "failed to bind management port %d(%s), retry count %d.\n", gscx__config->management_port, strerror(errno), retry_count));
			bt_msleep(1000);
			goto retry_management_http;
		}
		TRACE((T_ERROR, "failed to start management server on port %d(%s), shutdowning\n", gscx__config->management_port, strerror(errno)));
		return -1;
	}
	TRACE((T_INFO|T_STAT, "Management service successfully launched on port %d\n", gscx__config->management_port));

	return 0;
}

static int
start_check_listening_service()
{
	struct sockaddr_in http_addr;
	int i;
	int port_cnt = 0;
	struct MHD_OptionItem options[] = {
		{MHD_OPTION_CONNECTION_LIMIT, 200, NULL},
		{MHD_OPTION_CONNECTION_TIMEOUT, 60, NULL},	/* streaming 기능 때문에 10초에서 60초로 늘림, client에서 TCP Window Full이 발생하면 전송이 안되기 때문에  */
		{MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)&scx_make_request, (void *)SCX_HTTP},
		{MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)&scx_async_request_completed, (void *)SCX_HTTP},
		{MHD_OPTION_NOTIFY_CONNECTION, (intptr_t)&scx_notify_connection, (void *)SCX_HTTP},	/* socket이 연결이나 해제될때 호출되는 function 등록 */
		{MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)&sx_logger, 0},
		{MHD_OPTION_SOCK_ADDR, 0, NULL},
		{MHD_OPTION_END, 0, NULL}
	};

	int tsock_fd;
	unsigned int run_http_options;


	run_http_options = MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL_LINUX_ONLY | MHD_USE_EPOLL_TURBO | MHD_USE_DEBUG | MHD_ALLOW_SUSPEND_RESUME;




	for (port_cnt = 0; port_cnt < 200; port_cnt++ ) {
		//MHD_OPTION_SOCK_ADDR을 사용할 경우 MHD_start_daemon시의 port 옵션이 무시되므로
		//여기에 port 옵션을 같이 설정해 주어야 한다.
		/* port를 65221 부터 65420 까지 열수 있는 포트를 사용한다. */
		http_addr.sin_family = AF_INET;
		http_addr.sin_port = htons(gscx__check_listen_port);
		inet_pton(AF_INET,  "127.0.0.1", &(http_addr.sin_addr.s_addr));
		for (i = 0; options[i].option != MHD_OPTION_END; i++) {
			if (options[i].option == MHD_OPTION_SOCK_ADDR) {
				options[i].ptr_value = (struct sockaddr *) &(http_addr);
			}
		}

		gscx__http_servers[SCX_CHECK_SERVICE] = MHD_start_daemon(
							run_http_options,
							gscx__check_listen_port,
							NULL,	/* MHD_AcceptPolicyCallback */
							(void *)SCX_HTTP,	/* apc_cls extra argument */
							&scx_request_handler,
							(void *)SCX_HTTP,
							MHD_OPTION_ARRAY, options,
							MHD_OPTION_THREAD_POOL_SIZE, 1,
							MHD_OPTION_END);

		if (gscx__http_servers[SCX_CHECK_SERVICE] == NULL) {
			TRACE((T_INFO|T_STAT, "failed to open check listening service port %d.\n", gscx__check_listen_port));
			gscx__check_listen_port++;
			continue;

		}
		break;

	}
	if (200 <= port_cnt) {
		TRACE((T_ERROR, "failed to start check listening service on port %d, shutdowning\n", gscx__check_listen_port));
		return -1;
	}

	else {

	TRACE((T_INFO|T_STAT, "check listening service successfully launched on port %d\n", gscx__check_listen_port));
	}

	return 0;
}
void
nce_object_monitor(void *opt, nc_hit_status_t hs)
{
	nc_request_t *req = (nc_request_t *)kv_get_opaque(opt);
	if (req) {	/* preload에서 호출 되는 경우에는 req에 NULL이 들어 온다. */
		req->hi = hs;
		if(!req->hi) req->hi = hs; /* hi값이 셋팅 되지 않은 경우만 hit 상태를 업데이트 한다. */
	}
#if 0
	char 		zcc[128];
	char * 	hlf_get_hit(nc_request_t *req, char *buf);
	hlf_get_hit(req, zcc);
#endif
}

void
scx_origin_monitor(void *cb, const char *method, const char *origin, const char *request, const char *reply, double elapsed, double sentb, double receivedb)
{
	char 			log_line[4096] = "";
	int 			n;
	nc_method_t 	m;
	service_core_t *service_core = (service_core_t *)cb;
	service_info_t 		*service = (service_info_t *)service_core->master_service;
	char 		timestring[128];
	time_t 		utime;
	struct tm 	result;
	struct tm	*c_tm = NULL;
	int 		s = 0;

	if (!gscx__service_available) return ;
	m = scx_find_method(method);
	scx_update_volume_origin_stat(service, m, sentb, receivedb, reply);
	utime 	= time(NULL);
#if 0
	if(service->log_gmt_time) {
		c_tm 	= gmtime_r(&utime, &result);
		strftime(timestring, sizeof(timestring), GMT_DATE_FORMAT, c_tm);
	}
	else {
		c_tm	= localtime_r(&utime, &result);
		strftime(timestring, sizeof(timestring), LOCAL_DATE_FORMAT, c_tm);
	}
#else
		if(service->log_gmt_time) {
			c_tm 	= gmtime_r(&utime, &result);
		}
		else {
			c_tm	= localtime_r(&utime, &result);
		}
		/* access log와 포맷을 맞추기 위해서 아래처럼 한다. */
		scx_snprintf(timestring, sizeof(timestring), "[%4d-%02d-%02dT%02d:%02d:%02d%c%02i:%02i]",
	                       c_tm->tm_year+1900, c_tm->tm_mon+1,
						   c_tm->tm_mday, c_tm->tm_hour,
						   c_tm->tm_min, c_tm->tm_sec,
						   c_tm->tm_gmtoff < 0 ? '-' : '+',
	                       abs(c_tm->tm_gmtoff / 3600), abs(c_tm->tm_gmtoff % 3600));
#endif
	n = scx_snprintf(log_line, sizeof(log_line), "%s %s %s %s %s \"%s\" %s %.2f %.0f %.0f\n",
					timestring,
					vs_data(gscx__local_ip),
					vs_data(service->name),
					*origin==NULL?"0.0.0.0":origin,	/* origin에 값이 없는 경우가 있음 */
					method,
					request,
					reply,
					elapsed,
					sentb,
					receivedb);


#if 0	
	{
		int 	i;
		for (i = 0; i < n; i++)  {
			if (!isprint(log_line[i])) {
				TRACE((T_ERROR, "LOG:'%s' - contains binary char\n", log_line));
				TRAP;
			}
		}

	}
#endif
	if(service->log_origin){
		logger_put(scx_origin_logger, log_line, n);
	}
}

void
scx_origin_monitor2(nc_uint32_t txid, nc_xtra_options_t *req_headers, void *cb, const char *method, const char *origin, const char *request, const char *reply, double elapsed, double sentb, double receivedb, const char * infostr)
{
	char 			log_line[4096] = "";
	int 			n;
	nc_method_t 	m;
	service_core_t *service_core = (service_core_t *)cb;
	service_info_t 		*service = (service_info_t *)service_core->master_service;
	char 		timestring[128];
	time_t 		utime;
	struct tm 	result;
	struct tm	*c_tm = NULL;
	int 		s = 0;
	const char *range_header = NULL;
	if (!gscx__service_available) return ;


	range_header = kv_find_val(req_headers, "Range", 0);	/* get일 때는 대소 문자 구분을 하지 않는다. */

	m = scx_find_method(method);
	scx_update_volume_origin_stat(service, m, sentb, receivedb, reply);
	utime 	= time(NULL);
	if(service->log_gmt_time) {
		c_tm 	= gmtime_r(&utime, &result);
	}
	else {
		c_tm	= localtime_r(&utime, &result);
	}
	/* access log와 포맷을 맞추기 위해서 아래처럼 한다. */
	scx_snprintf(timestring, sizeof(timestring), "[%4d-%02d-%02dT%02d:%02d:%02d%c%02i:%02i]",
					   c_tm->tm_year+1900, c_tm->tm_mon+1,
					   c_tm->tm_mday, c_tm->tm_hour,
					   c_tm->tm_min, c_tm->tm_sec,
					   c_tm->tm_gmtoff < 0 ? '-' : '+',
					   abs(c_tm->tm_gmtoff / 3600), abs(c_tm->tm_gmtoff % 3600));

	n = scx_snprintf(log_line, sizeof(log_line), "%s %s %s %s %s \"%s\" %s %.2f %.0f %.0f %u \"%s\" \"%s\"\n",
					timestring,
					vs_data(gscx__local_ip),
					vs_data(service->name),
					*origin==NULL?"0.0.0.0":origin,	/* origin에 값이 없는 경우가 있음 */
					method,
					request,
					reply,
					elapsed,
					sentb,
					receivedb,
					txid,
					range_header?range_header:"-",
					(infostr[0]!= NULL)?infostr:"-");	/* infostr은 오리진에 ERROR가 발생 했을때만 기록 된다. */


#if 0
	{
		int 	i;
		for (i = 0; i < n; i++)  {
			if (!isprint(log_line[i])) {
				TRACE((T_ERROR, "LOG:'%s' - contains binary char\n", log_line));
				TRAP;
			}
		}

	}
#endif
	if(service->log_origin){
		logger_put(scx_origin_logger, log_line, n);
	}
}


 int
header_parser(const char *key, const char *value, void *cb)
{
	 TRACE((T_INFO, "request : key(%s) = value(%s)\n", key, value ));

	return 0;
}

void
scx_origin_request_handler(int phase_id, nc_origin_io_command_t *command)
{
	site_t 		*site = (site_t *)command->cbdata;
	nc_xtra_options_t	*properties = command->properties;
	service_info_t 	*service = NULL;
	int count = 0;
	int i;
	char *header = NULL;
	if (!site) return;
	service = (service_info_t *) site->service;
	if (!service) return;
	if (service->custom_user_agent) {
		/* 오리진 요청 User-Agent 헤더를 변경 */
		ASSERT(properties);
		kv_replace(properties, MHD_HTTP_HEADER_USER_AGENT, vs_data(service->custom_user_agent), 1);
	}
	scx_check_url_encoding(command->url);
	if (service->remove_origin_request_headers) {
		/* 오리진에 요청되는 헤더를 제거 */
		count = scx_list_get_size(service->remove_origin_request_headers);
		for(i = 0; i < count; i++)	{
			header = (char *)scx_list_get_by_index_key(service->remove_origin_request_headers, i);
			TRACE((T_DEBUG, "host(%s) remove header[%s]\n", vs_data(service->name), header));
			kv_remove(properties, header, 0);
		}
	}
//	TRACE((T_DAEMON, "request : phase id(%d) method = %s, url(%s)\n", phase_id, command->method, command->url));
//	TRACE((T_DAEMON, "request : host(%s)\n", vs_data(site->name) ));
//	kv_for_each(command->properties, header_parser, (void *)NULL);
	site_run_origin_trigger(command, phase_id);
	return;
}

void
scx_origin_response_handler(int phase_id, nc_origin_io_command_t *command)
{
	site_t 		*site = (site_t *)command->cbdata;
	nc_xtra_options_t	*properties = command->properties;
	service_info_t 	*service = NULL;
	int count = 0;
	int i;
	char *header = NULL;
	if(!site) return;
	service = (service_info_t *) site->service;
	if (!service) return;
//	TRACE((T_DAEMON, "response : phase id(%d) method = %s, url(%s)\n", phase_id, command->method, command->url));
//	TRACE((T_DAEMON, "response : host(%s)\n", vs_data(site->name) ));
//	kv_for_each(command->properties, header_parser, (void *)NULL);
	if (service->remove_origin_response_headers) {
		/* 오리진에서 응답되는 헤더를 제거 */
		count = scx_list_get_size(service->remove_origin_response_headers);
		for(i = 0; i < count; i++)	{
			header = (char *)scx_list_get_by_index_key(service->remove_origin_response_headers, i);
			TRACE((T_DEBUG, "host(%s) remove header[%s]\n", vs_data(service->name), header));
			kv_remove(properties, header, 0);
		}
	}
	site_run_origin_trigger(command, phase_id);
	return;
}

/*
 * NetCache Core에서 Origin의 on/offline 확인시 불려지는 callback 함수
 * 리턴이 TRUE이면 OFFLINE, FALSE이면 ONLINE으로 처리된다.
 */
int
scx_origin_online_handler(char *url, int httpcode, void *usercontext)
{
	service_core_t		*core = (service_core_t *) usercontext;
	service_info_t 		*service = NULL;
	char str_code[10] = {0,};
	int ret = TRUE;
	if (core == NULL) {
		return FALSE;
	}
	service = (service_info_t *)core->master_service;
	ASSERT(service);
	snprintf(str_code, sizeof(str_code), "%d", httpcode);
	if (500 > httpcode ) {
		ret = FALSE;
	}
	else if(service->origin_online_code == NULL) {
		ret = FALSE;
	}
	/*
	 * 기존(netcache 1671 이전) : origin_online_code에 정의가 되어 있는 경우 ONLINE
	 * 변경(netcache 1671 이후) : origin_online_code에 정의가 되어 있는 경우 OFFLINE
	 */
	else if (NULL == strstr(vs_data(service->origin_online_code), str_code)) {
		ret = FALSE;
	}
	TRACE((T_DEBUG|T_DAEMON, "Volume[%s] origin online handler url(%s), online_code(%s), origin code(%d) is %s.)\n",
						vs_data(service->name), url, service->origin_online_code?vs_data(service->origin_online_code):"NULL" , httpcode, ret?"offline":"online"));
	return ret;
}

static char gscx__stat_traffic_prefix[32] = {'\0'};
static char gscx__stat_nos_prefix[32] = {'\0'};
static char gscx__stat_http_prefix[32] = {'\0'};
static char gscx__stat_http_path[32] = {'\0'};
/* streaming type 통계를 사용하는지 확인 한다. */
static int
scx_check_stat_write_type()
{
	int stat_write_type = 0;
	stat_write_type = scx_get_uint(gscx__default_site, SV_STAT_WRITE_TYPE, STAT_TYPE_DELIVERY);
	TRACE((T_INFO|T_STAT, "%s = %d\n", SV_STAT_WRITE_TYPE, stat_write_type));
	if (stat_write_type == STAT_TYPE_STREAMING) {
		/* streaming type 통계 인 경우 */
		gscx__config->stat_write_period = 5;
		snprintf(gscx__stat_traffic_prefix, 32, "traffic_stat_wowza_");
		snprintf(gscx__stat_nos_prefix, 32, "nos_stat_wowza_");
		snprintf(gscx__stat_http_prefix, 32, "mms_stat_wowza_");
		snprintf(gscx__stat_http_path, 32, "/usr/service/stat/mms_stat");
		gscx__config->stat_origin_enable = 0;
		gscx__config->rtmp_enable = 1;
		gscx__config->session_enable = 1;
	}
	else {
		/* delivery type 통계 인 경우 */
		gscx__config->stat_write_period = 10;
		snprintf(gscx__stat_traffic_prefix, 32, "traffic_stat_statd_");
		snprintf(gscx__stat_nos_prefix, 32, "nos_stat_statd_");
		snprintf(gscx__stat_http_prefix, 32, "http_stat_statd_");
		snprintf(gscx__stat_http_path, 32, "/usr/service/stat/http_stat");
		gscx__config->stat_origin_enable = 1;
		gscx__config->rtmp_enable = 0;
		gscx__config->session_enable = 0;
		stat_write_type = STAT_TYPE_DELIVERY;
	}
	gscx__config->stat_write_type = stat_write_type;
	return 0;
}

static int
nce_reload_default_cert(int reload)
{
	uint32_t	hash_crt; 				/* crt 인증서 파일의 hash값. 설정 파일의 변경 여부 확인에 사용된다. */
	uint32_t	hash_key; 				/* key 인증서 파일의 hash값. 설정 파일의 변경 여부 확인에 사용된다. */
	void		*certificate;
	off_t 		st_size; // 인증서 파일의 크기를 저장
	if (gscx__config->certificate_crt_path != NULL && gscx__config->certificate_key_path != NULL) {	/* 인증서 로딩 */
		hash_crt  = vm_make_config_hash(vs_data(gscx__config->certificate_crt_path), &st_size);
		hash_key  = vm_make_config_hash(vs_data(gscx__config->certificate_key_path), &st_size);
		if (gscx__config->hash_crt != hash_crt || gscx__config->hash_key != hash_key) {
			// default 인증서 파일이 변경 된 경우(인증서 파일의 hash 값 변경)만 load 한다.
			TRACE((T_INFO|T_STAT, "certificate file changed.\n"));
			if(gscx__config->enable_sni == 1) {
				/* SNI 기능이 사용가능 하더라도 enable_sni 설정이 되어 있지 않으면 동작 하지 않는다. */
				certificate = scx_sni_load_keys(gscx__default_site, vs_data(gscx__config->certificate_crt_path),
									vs_data(gscx__config->certificate_key_path));
			}
			else {
				certificate =  scx_check_certitificate(gscx__default_site, gscx__config, R_OK);
			}
			if (certificate == NULL) {
				TRACE((T_ERROR, "certificate loading failed.\n"));
			}
			else {
				gscx__default_site->certificate = certificate;
				gscx__config->hash_crt  = hash_crt;
				gscx__config->hash_key  = hash_key;
				TRACE((T_INFO|T_STAT, "certificate loading done\n"));
			}
		}
	} // end of if (config->certificate_crt_path != NULL && config->certificate_key_path != NULL)

}


static int
nce_load_global_params(site_t * site, scx_config_t * config, int reload)
{
#define 	VT_VSTRING 	0
#define 	VT_INT 		1
#define 	VT_UINT 	2
#define 	VT_INT64 	3
#define 	RELOAD_NOT_AVAIL	0
#define 	RELOAD_AVAIL		1
	struct var_info {
		char 	var_name[64];
		int 	var_type;
		int		reload;
		void 	*var_ptr;
		union {
			vstring_t 		*vs;
			long long 		ll;
			unsigned int 	ui;
		} ud;
	} variables[] = {
	{SV_FOLLOW_REDIR, 		VT_UINT,	RELOAD_AVAIL,		&config->follow_redir, 	.ud.ui = config->follow_redir},	/* reload 가능*/
	{SV_NEGATIVE_TTL,		VT_UINT,	RELOAD_AVAIL,		&config->negative_ttl,	.ud.ui = config->negative_ttl},	/* reload 가능*/
	{SV_POSITIVE_TTL, 		VT_UINT,	RELOAD_AVAIL,		&config->positive_ttl, 	.ud.ui = config->positive_ttl},	/* reload 가능*/
	{SV_READAHEAD_MB, 		VT_UINT,	RELOAD_NOT_AVAIL,	&config->ra_mb, 		.ud.ui = config->ra_mb},
	{SV_ENABLE_FASTCRC, 	VT_UINT,	RELOAD_NOT_AVAIL,	&config->fastcrc, 		.ud.ui = config->fastcrc},
	{SV_ENABLE_FORCE_CLOSE, VT_UINT,	RELOAD_NOT_AVAIL,	&config->force_close, 		.ud.ui = config->force_close},
	{SV_ALLOW_DYNAMIC_VHOST,VT_UINT,	RELOAD_AVAIL,		&config->allow_dynamic_vhost, 	.ud.ui = config->allow_dynamic_vhost},
	{SV_FORCE_SIGJMP_RESTART,VT_UINT,	RELOAD_AVAIL,		&config->force_sigjmp_restart, 	.ud.ui = config->force_sigjmp_restart},
	{SV_CERTIFICATE_CRT, 	VT_VSTRING,	RELOAD_AVAIL,	&config->certificate_crt_path, 	.ud.vs = config->certificate_crt_path?config->certificate_crt_path:NULL},
	{SV_CERTIFICATE_KEY, 	VT_VSTRING,	RELOAD_AVAIL,	&config->certificate_key_path, 	.ud.vs = config->certificate_key_path?config->certificate_key_path:NULL},
	{SV_SSL_PRIORITIES,		VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->ssl_priorities, 		.ud.vs = NULL},
#if GNUTLS_VERSION_MAJOR >= 3	 /* SNI가 지원 되지 않는 버전에서는 enable_sni가 설정 되어 있어도 무시한다. */
	{SV_ENABLE_SNI,			VT_UINT,	RELOAD_NOT_AVAIL,	&config->enable_sni, 		.ud.ui = config->enable_sni},
#endif
#if 0
	{SV_INMEMORY_OBJECT ,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->max_inodes, 		.ud.ui = config->max_inodes},
#else
	{SV_INODE_SIZE ,		VT_UINT,	RELOAD_NOT_AVAIL,	&config->inode_mem, 		.ud.ui = config->inode_mem},
#endif
	{SV_MEMORY_CACHE,		VT_UINT,	RELOAD_NOT_AVAIL,	&config->cache_mem, 		.ud.ui = config->cache_mem},
	{SV_CHUNK_SIZE,			VT_UINT,	RELOAD_NOT_AVAIL,	&config->block_size, 		.ud.ui = config->block_size},
	{SV_WRITEBACK_CHUNKS,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->writeback_blocks,.ud.ui = config->writeback_blocks},
	{SV_CACHE_PATH,			VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->cache_path_list, .ud.vs = NULL},
	{SV_WORKERS,			VT_UINT,	RELOAD_NOT_AVAIL,	&config->workers, 		.ud.ui = config->workers},
	{SV_POOL_SIZE,			VT_UINT,	RELOAD_NOT_AVAIL,	&config->pool_size, 		.ud.ui = config->pool_size},
	{SV_MAX_VIRTUAL_HOST,	VT_UINT,	RELOAD_AVAIL,		&config->max_virtual_host, 	.ud.ui = config->max_virtual_host},
	{SV_CLUSTER_IP,			VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->cluster_ip, 		.ud.vs = config->cluster_ip?config->cluster_ip:NULL},
	{SV_CLUSTER_TTL,		VT_UINT,	RELOAD_NOT_AVAIL,	&config->cluster_ttl,		.ud.ui = config->cluster_ttl},
	{SV_LOG_FORMAT,			VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->accesslog_format,	.ud.vs = vs_allocate(site->mp, 0, DEFAULT_ACCESSLOG_FORMAT, 0)},
//	{SV_LOG_DIR,			VT_VSTRING,	RELOAD_AVAIL,		&config->log_dir , 		.ud.vs = config->log_dir ?config->log_dir :vs_allocate(site->mp, 0, gscx__default_log_path, 0)},
	{SV_LOG_GMT_TIME,	 	VT_UINT,	RELOAD_NOT_AVAIL,	&config->log_gmt_time, 	.ud.ui = config->log_gmt_time},	/* reload 가능*/
	{SV_LOG_SIZE,			VT_UINT,	RELOAD_NOT_AVAIL,	&config->log_size,	 	.ud.ui = config->log_size},
	{SV_LOG_ACCESS,			VT_UINT,	RELOAD_AVAIL,		&config->log_access,	 .ud.ui = config->log_access},		/* reload 가능*/
	{SV_LOG_ERROR,			VT_UINT,	RELOAD_AVAIL,		&config->log_error,	 	.ud.ui = config->log_error},		/* reload 가능*/
	{SV_LOG_ORIGIN,			VT_UINT,	RELOAD_AVAIL,		&config->log_origin,	 	.ud.ui = config->log_origin},		/* reload 가능*/
	{SV_LOGROTATE_SIGNAL_ENABLE,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->logrotate_signal_enable,	 	.ud.ui = config->logrotate_signal_enable},		/* reload 가능*/
	{SV_HOST_NAME,			VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->host_name, 		.ud.vs = NULL},
	{SV_HTTPS_PORT, 		VT_UINT,	RELOAD_NOT_AVAIL,	&config->https_port, 		.ud.ui = config->https_port},
	{SV_HTTP_PORT, 			VT_UINT,	RELOAD_NOT_AVAIL,	&config->http_port, 		.ud.ui = config->http_port},
	{SV_TPROXY_PORT, 		VT_UINT,	RELOAD_NOT_AVAIL,	&config->tproxy_port, 	.ud.ui = config->tproxy_port},
	{SV_HTTP_LISTEN_IP,		VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->listen_ip, 		.ud.vs = NULL},
	{SV_MANAGEMENT_PORT, 		VT_UINT,	RELOAD_NOT_AVAIL,	&config->management_port, 		.ud.ui = config->management_port},
	{SV_MANAGEMENT_LISTEN_IP,	VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->management_listen_ip, 	.ud.vs = NULL},
	{SV_MANAGEMENT_ENABLE_WITH_SERVICE_PORT, 	VT_UINT,	RELOAD_AVAIL,	&config->management_enable_with_service_port, 	.ud.ui = config->management_enable_with_service_port},
	{SV_REQUEST_TIMEOUT,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->request_timeout, .ud.vs = config->request_timeout},
	{SV_REAL_USER, 			VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->real_user, 		.ud.vs = vs_allocate(site->mp, 0, "root", 0)},
	{SV_COLD_CACHE_ENABLE,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->cold_cache_enable, .ud.ui = config->cold_cache_enable},
	{SV_COLD_CACHE_RATIO,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->cold_cache_ratio, 	.ud.ui = config->cold_cache_ratio},
	{SV_DISK_HIGH_WM,	 	VT_UINT,	RELOAD_NOT_AVAIL,	&config->disk_high_wm,	.ud.ui = config->disk_high_wm},
	{SV_DISK_LOW_WM,	 	VT_UINT,	RELOAD_NOT_AVAIL,	&config->disk_low_wm, 	.ud.ui = config->disk_low_wm},
	{SV_MAX_PATH_LOCK_SIZE,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->max_path_lock_size, 	.ud.ui = config->max_path_lock_size},
	{SV_DISK_MEDIA_CACHE_DIR,			VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->disk_media_cache_dir, 		.ud.vs = NULL},
	{SV_DISK_MEDIA_CACHE_HIGH,	 		VT_UINT,	RELOAD_AVAIL,	&config->disk_media_cache_high, 	.ud.ui = config->disk_media_cache_high},
	{SV_DISK_MEDIA_CACHE_LOW,	 		VT_UINT,	RELOAD_AVAIL,	&config->disk_media_cache_low, 		.ud.ui = config->disk_media_cache_low},
	{SV_DISK_MEDIA_CACHE_MONITOR_PERIOD,VT_UINT,	RELOAD_NOT_AVAIL,	&config->disk_media_cache_monitor_period, 	.ud.ui = config->disk_media_cache_monitor_period},
	{SV_DISK_MEDIA_CACHE_ENABLE,		VT_UINT,	RELOAD_AVAIL,	&config->disk_media_cache_enable, 	.ud.ui = config->disk_media_cache_enable},
	{SV_POLLING_POLICY,					VT_UINT,	RELOAD_NOT_AVAIL,	&config->polling_policy,					.ud.ui = config->polling_policy},
	{SV_ENABLE_ASYNC_ACCESSHANDLER_CALLBACK,VT_UINT,	RELOAD_AVAIL,	&config->enable_async_accesshandler_callback,	.ud.ui = config->enable_async_accesshandler_callback},
	{SV_ENABLE_ASYNC_READER_CALLBACK,		VT_UINT,	RELOAD_AVAIL,	&config->enable_async_reader_callback,	.ud.ui = config->enable_async_reader_callback},
	{SV_ENABLE_ASYNC_COMPLETE_CALLBACK,		VT_UINT,	RELOAD_AVAIL,	&config->enable_async_complete_callback,	.ud.ui = config->enable_async_complete_callback},
	{SV_ENABLE_ASYNC_NCAPI,					VT_UINT,	RELOAD_NOT_AVAIL,	&config->enable_async_ncapi,	.ud.ui = config->enable_async_ncapi},
	{SV_IO_BUFFER_SIZE,		VT_UINT,	RELOAD_NOT_AVAIL,	&config->io_buffer_size,	.ud.ui = config->io_buffer_size},
	{SV_MEDIA_CACHE_SIZE,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->media_cache_size,	.ud.ui = config->media_cache_size},
	{SV_BUILDER_CACHE_SIZE,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->builder_cache_size,.ud.ui = config->builder_cache_size},
	{SV_SESSION_ENABLE,		VT_UINT,	RELOAD_NOT_AVAIL,	&config->session_enable,	.ud.ui = config->session_enable},
	{SV_SESSION_TIMEOUT,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->session_timeout,	.ud.ui = config->session_timeout},
	{SV_USE_LOCAL_FILE,		VT_UINT,	RELOAD_NOT_AVAIL,	&config->use_local_file,	.ud.ui = config->use_local_file},
	{SV_CORE_FILE_PATH,		VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->core_file_path,	.ud.vs = NULL},
	{SV_NCAPI_HANG_DETECT,			VT_UINT,	RELOAD_AVAIL,	&config->ncapi_hang_detect,				.ud.ui = config->ncapi_hang_detect},
	{SV_NCAPI_HANG_DETECT_TIMEOUT,	VT_UINT,	RELOAD_AVAIL,	&config->ncapi_hang_detect_timeout,		.ud.ui = config->ncapi_hang_detect_timeout},
	{SV_CDN_DOMAIN_POSTFIX,			VT_VSTRING,	RELOAD_AVAIL,	&config->cdn_domain_postfix,			.ud.vs = NULL},
	{SV_PERMIT_INIT_EXPIRE_DURATION,VT_UINT,	RELOAD_NOT_AVAIL,	&config->permit_init_expire_duration,	.ud.ui = config->permit_init_expire_duration},
	{SV_CR_ENABLE,			VT_UINT,	RELOAD_AVAIL,		&config->cr_enable,			.ud.ui = config->cr_enable},
	{SV_STAT_RC_SEQ,		VT_UINT,	RELOAD_AVAIL,		&config->stat_rc_seq,			.ud.ui = config->stat_rc_seq},
	{SV_STAT_VRC_SEQ,		VT_UINT,	RELOAD_AVAIL,		&config->stat_vrc_seq,			.ud.ui = config->stat_vrc_seq},
	{SV_STAT_SVR_SEQ,		VT_UINT,	RELOAD_AVAIL,		&config->stat_svr_seq,			.ud.ui = config->stat_svr_seq},
	{SV_STAT_IDC_NODE_ID,	VT_VSTRING,	RELOAD_AVAIL,		&config->stat_idc_node_id,		.ud.vs = vs_allocate(site->mp, 0, "-", 0)}, /* IDC_NODE_ID,RC_ID,VRC_ID,SVR_ID 는 없어도 통계 기록이 가능하기 때문에 기본값을 -로 처리한다. */
	{SV_STAT_RC_ID,			VT_VSTRING,	RELOAD_AVAIL,		&config->stat_rc_id,			.ud.vs = vs_allocate(site->mp, 0, "-", 0)},
	{SV_STAT_VRC_ID,		VT_VSTRING,	RELOAD_AVAIL,		&config->stat_vrc_id,			.ud.vs = vs_allocate(site->mp, 0, "-", 0)},
	{SV_STAT_SVR_ID,		VT_VSTRING,	RELOAD_AVAIL,		&config->stat_svr_id,			.ud.vs = vs_allocate(site->mp, 0, "-", 0)},
	{SV_STAT_WRITE_PERIOD,	VT_UINT,	RELOAD_AVAIL,		&config->stat_write_period,		.ud.ui = config->stat_write_period},
	{SV_STAT_ROTATE_PERIOD,	VT_UINT,	RELOAD_AVAIL,		&config->stat_rotate_period,	.ud.ui = config->stat_rotate_period},
	{SV_STAT_ORIGIN_PATH,	VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_origin_path,		.ud.vs = vs_allocate(site->mp, 0, "/usr/service/stat/origin_stat", 0)},
	{SV_STAT_ORIGIN_PREFIX,	VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_origin_prefix,	.ud.vs = vs_allocate(site->mp, 0, "origin_stat_statd_", 0)},
	{SV_STAT_TRAFFIC_PATH,	VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_traffic_path,		.ud.vs = vs_allocate(site->mp, 0, "/usr/service/stat/traffic_stat", 0)},
	{SV_STAT_TRAFFIC_PREFIX,VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_traffic_prefix,	.ud.vs = vs_allocate(site->mp, 0, gscx__stat_traffic_prefix, 0)},
	{SV_STAT_NOS_PATH,		VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_nos_path,			.ud.vs = vs_allocate(site->mp, 0, "/usr/service/stat/nos_stat", 0)},
	{SV_STAT_NOS_PREFIX,	VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_nos_prefix,		.ud.vs = vs_allocate(site->mp, 0, gscx__stat_nos_prefix, 0)},
	{SV_STAT_HTTP_PATH,		VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_http_path,		.ud.vs = vs_allocate(site->mp, 0, gscx__stat_http_path, 0)},
	{SV_STAT_HTTP_PREFIX,	VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_http_prefix,		.ud.vs = vs_allocate(site->mp, 0, gscx__stat_http_prefix, 0)},
	{SV_STAT_DOING_DIR_NAME,VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_doing_dir_name,	.ud.vs = vs_allocate(site->mp, 0, "doing", 0)},
	{SV_STAT_DONE_DIR_NAME,	VT_VSTRING,	RELOAD_NOT_AVAIL,	&config->stat_done_dir_name,	.ud.vs = vs_allocate(site->mp, 0, "done", 0)},
	{SV_STAT_WRITE_ENABLE,	VT_UINT,	RELOAD_AVAIL,		&config->stat_write_enable,		.ud.ui = config->stat_write_enable},
	{SV_STAT_ORIGIN_ENABLE,	VT_UINT,	RELOAD_AVAIL,		&config->stat_origin_enable,	.ud.ui = config->stat_origin_enable},
	{SV_STAT_TRAFFIC_ENABLE,VT_UINT,	RELOAD_AVAIL,		&config->stat_traffic_enable,	.ud.ui = config->stat_traffic_enable},
	{SV_STAT_NOS_ENABLE,	VT_UINT,	RELOAD_AVAIL,		&config->stat_nos_enable,		.ud.ui = config->stat_nos_enable},
	{SV_STAT_HTTP_ENABLE,	VT_UINT,	RELOAD_AVAIL,		&config->stat_http_enable,		.ud.ui = config->stat_http_enable},
	{SV_RTMP_PORT,				VT_UINT,	RELOAD_NOT_AVAIL,	&config->rtmp_port,				.ud.ui = config->rtmp_port},
	{SV_RTMP_ENABLE,			VT_UINT,	RELOAD_NOT_AVAIL,	&config->rtmp_enable,			.ud.ui = config->rtmp_enable},
	{SV_RTMP_WORKER_POOL_SIZE,	VT_UINT,	RELOAD_NOT_AVAIL,	&config->rtmp_worker_pool_size,	.ud.ui = config->rtmp_worker_pool_size},
	{SV_LOG_LEVEL,			VT_VSTRING,	RELOAD_AVAIL,		&config->nc_trace_mask,		.ud.vs = gscx__nc_trace_mask?gscx__nc_trace_mask:vs_allocate(site->mp, 0, "info,stat,warn,error", 0)},	/* reload 가능*/
	{"", 					VT_VSTRING,	RELOAD_NOT_AVAIL,	NULL, 					.ud.vs = NULL}
	};
#define 	gv_count 	sizeof(variables)/sizeof(struct var_info)
	int 			i;
	vstring_t		*tvs = NULL;
	int 	ret;
	int 	factor = 40;
	unsigned int temp_int;
	unsigned long long temp_int64;
	int		alloc_size = 0;
	int 	require_size = 0;

	TRACE((T_INFO|T_STAT, "***************** Load global parameter started *********************\n"));
	for (i = 0; i < gv_count; i++) {
		if (variables[i].var_name[0] == '\0') continue;
		if (reload && variables[i].reload != RELOAD_AVAIL) continue; // reload시에는 RELOAD_AVAIL로 된것 들만 처리한다.
		switch (variables[i].var_type) {
			/*
			 * reload시에는 기존 설정과 비교해서 틀린 경우 변경사항을 업데이트 한다.
			 * 문자열의 경우 gscx__default_site로 부터 메모리를 추가로 할당 받아서 업데이트 하도록 한다.
			 * 이로 인해 문자열로 된 설정이 자주 변경되는 경우 메모리 낭비가 많이 되는 문제가 발생할수 있다.
			 */
			case VT_VSTRING:
				if (reload) {
					tvs = scx_get_vstring(site, variables[i].var_name, variables[i].ud.vs);
					if (tvs) {
						if (*((vstring_t **) variables[i].var_ptr) == NULL) {
							/* 기존에 값이 없었다가 추가된 경우는 메모리를 새로 할당한다. */
							*((vstring_t **) variables[i].var_ptr) = vs_strdup(gscx__default_site->mp, tvs);
						}
						else if (vs_strcasecmp(tvs, *((vstring_t **) variables[i].var_ptr)) != 0) {
							/* 설정이 변경된 경우 gscx__default_site의 memory pool을 사용해서 변경 사항을 업데이트 한다. */
							if (strcmp(variables[i].var_name, SV_LOG_LEVEL) == 0) {
								/*
								 * log level 변경의 경우 자주 있을거라고 판단되어 가능하면 메모리를 재할당 하지 않도록 아래처럼 구현
								 * 다른 값들의 경우 변경 도중에 사용될 경우 문제가 될수 있어서 매번 메모리를 할당하는 방식으로 했음.
								 */
								alloc_size = vs_alloc_size(*((vstring_t **) variables[i].var_ptr));
								require_size = vs_length(tvs)+1;
								TRACE((T_DEBUG, "%s alloc size(%d) require size(%d)\n", variables[i].var_name, alloc_size, require_size));
								if (alloc_size < require_size) {
									/* 기존에 할당된 메모리 보다 새로운 문자열의 크기가 큰 경우 메모리를 새로 할당한후 복사 한다. */
									*((vstring_t **) variables[i].var_ptr) = vs_strdup(gscx__default_site->mp, tvs);
								}
								else {
									/* 기존에 할당된 메모리가 새로운 문자열의 크기보다 큰 경우에는 기존 메모리를 재사용한다. */
									vs_update_length(*((vstring_t **) variables[i].var_ptr), 0);
									vs_strcat(*((vstring_t **) variables[i].var_ptr), tvs);
								}
								alloc_size = vs_alloc_size(*((vstring_t **) variables[i].var_ptr));

								/* loglevel이 변경된 경우 여기서 바로 처리 한다. */
								trc_set_mask(vs_data(config->nc_trace_mask));
							}
							else {
								/* 다른 설정들은 변경시 마다 메모리를 재할당 한다.*/
								*((vstring_t **) variables[i].var_ptr) = vs_strdup(gscx__default_site->mp, tvs);
							}
							TRACE((T_DAEMON, "%s variable changed.\n", variables[i].var_name, alloc_size, require_size));
						}
					}
					else {
						// 해당 설정이 없는 경우
						*((vstring_t **) variables[i].var_ptr) = NULL;
					}
				} /* end of if (reload) */
				else {
					tvs = *((vstring_t **) variables[i].var_ptr) = scx_get_vstring(site, variables[i].var_name, variables[i].ud.vs);
				}
				TRACE((T_INFO|T_STAT, "global.%s = %s\n", variables[i].var_name, (tvs?(char *)vs_data(tvs):"null")));
				break;
			case VT_INT:
			case VT_UINT:
				if (reload) {
					temp_int = scx_get_uint(site, variables[i].var_name, variables[i].ud.ui);
					if (temp_int != *((unsigned int *) variables[i].var_ptr)) {
						*((unsigned int *) variables[i].var_ptr) = temp_int;
					}
				}
				else {
					*((unsigned int *) variables[i].var_ptr) = scx_get_uint(site, variables[i].var_name, variables[i].ud.ui);
				}
				TRACE((T_INFO|T_STAT, "global.%s = %u\n", variables[i].var_name, *((unsigned int *)variables[i].var_ptr)));
				break;
			case VT_INT64:
				if (reload) {
					temp_int64 = scx_get_uint64(site, variables[i].var_name, variables[i].ud.ll);
					if ( temp_int64 != *((long long *) variables[i].var_ptr) ) {
						*((long long *) variables[i].var_ptr) = temp_int64;
					}
				}
				else {
					*((long long *) variables[i].var_ptr) = scx_get_uint64(site, variables[i].var_name, variables[i].ud.ll);
				}
				TRACE((T_INFO|T_STAT, "global.%s = %llu\n", variables[i].var_name, *((unsigned long long *)variables[i].var_ptr)));
				break;
		}
	}
	TRACE((T_INFO|T_STAT, "***************** Load global parameter finished ********************\n"));
	if (reload == FALSE) {	// 데몬 기동시에만 실행되는 부분
		gscx__cache_part_count = 0;
		if (config->cache_path_list && vs_length(config->cache_path_list) > 0) {
			/* cache 파티션이 설정되어 있음, ',' 또는 ':'로 복수 경로가 설정되어있을 확율이 있음*/
			off_t 			toffset = 0;
			vstring_t 		*partition = NULL;
			//toffset = vs_pickup_token(site->mp, config->cache_path_list, toffset, ",:", &partition);
			/* cache 파티션은 변경을 하지 못하므로 reloading 대상이 아니다. */
			toffset = vs_pickup_token(gscx__global_mp, config->cache_path_list, toffset, ",:", &partition);
			while (partition) {
				gscx__cache_part[gscx__cache_part_count++] = partition;
				//toffset = vs_pickup_token(site->mp, config->cache_path_list, toffset, ",:", &partition);
				toffset = vs_pickup_token(gscx__global_mp, config->cache_path_list, toffset, ",:", &partition);
			}
			if (config->disk_media_cache_dir == NULL) {
				/* 지정하지 않을 경우 cache 경로 아래에 media 폴더로 생성 */
				config->disk_media_cache_dir = vs_allocate(site->mp, (vs_length(gscx__cache_part[0])+7), NULL, FALSE);
				sprintf(vs_data(config->disk_media_cache_dir), "%s/media", vs_data(gscx__cache_part[0]));
				vs_update_length(config->disk_media_cache_dir, strlen(vs_data(config->disk_media_cache_dir)) );
			}
		}


		if (config->io_buffer_size < 16 ) {
			config->io_buffer_size = 16;
		}
		if (config->io_buffer_size > 1024) {
			config->io_buffer_size = 1024;
		}
		if (config->fastcrc > 0 ) { /* fastcrc는 4의 배수여야 하고 block size보다 작거나 같아야 한다. */
			if (config->fastcrc > config->block_size * 1024)
			{
				config->fastcrc = config->block_size * 1024;
			}
			else if ( (config->fastcrc % 4) > 0) {
				config->fastcrc = (config->fastcrc / 4) * 4;
			}

		}
		if (config->block_size < 4 || config->block_size > 10240) config->block_size = 128;
		if(config->positive_ttl > MAX_POSITIVE_TTL)
			config->positive_ttl = MAX_POSITIVE_TTL;
		if(config->negative_ttl > MAX_NEGATIVE_TTL)
			config->negative_ttl = MAX_NEGATIVE_TTL;
		gscx__lua_pool_size = 8;
		/*
		 * 이 부분은 필요한 경우 설정으로 받을수 있다.
		 * 설정으로 받게 되더라도 config->pool_size보다 작아야 한다.
		 */
		gscx__max_url_len = config->pool_size/2;
		gscx__max_header_val_len = config->pool_size/2;
		if (config->host_name == NULL) {
			char 	t_host[128] = "";
			char 	t_domain[128] = "";
			gethostname(t_host, sizeof(t_host));
			getdomainname(t_domain, sizeof(t_domain));
			config->host_name = vs_allocate(site->mp, (strlen(t_host) + strlen(t_domain)+4), NULL, FALSE);
			if ((t_domain[0] == '\0')  || strchr(t_domain, '(')) {
				strcpy(vs_data(config->host_name), t_host);
			}
			else {
				sprintf(vs_data(config->host_name), "%s.%s", t_host, t_domain);
			}
			vs_update_length(config->host_name, -1);
		}
		/*
		 * inode 메모리의 크기를 계산한다.
		 * 캐시 메모리의 2.5%를 inode 메모리 크기로 할당한다.
		 * chunk_size가 64KB이고 캐시 메모리가 20GB인 경우 inode 메모리는 512MB가 되고
		 * 약 22만개 정도의 inode를 생성할수 있다.
		 * chunk_size가 64KB 보다 작은 경우는 inode 메모리를 더많이 할당 하기 위해 factor가 변한다.
		 */
		if (config->inode_mem == 0) {
			if(config->block_size < 64) {
				factor = (40 * config->block_size) / 64;
			}
			config->inode_mem = config->cache_mem / factor;
			if(config->inode_mem < 32) {
				/* 별도의 설정이 없는 경우에는 최소 값이 32MB임 */
				config->inode_mem = 32;
			}
		}
		else if(config->inode_mem < 4) {
			/* 직접 지정할수 있는 최소 값은 4MB임 */
			config->inode_mem = 4;
		}
		if (config->inode_mem > 6144 ) {
			config->inode_mem = 6144;
		}
		if(gscx__config->cr_enable == 1) {
			if (cr_update_config(site) < 0) {
				TRACE((T_ERROR, "Content Router Service configuration failed.\n"));
				return 0;
			}
		}
	}	// end of if (reload == FALSE)
	else {
		// reload시에만 실행 되는 부분
		for (i = 0; i < gv_count; i++) {
			if (reload) {
				if (variables[i].var_type == VT_VSTRING) {

				}
				else {

				}
			}
		}
		cr_update_config(site);	// reload 시에는 content router 설정에 문제가 있더라도 기존 처럼 데몬을 종료 하지 않는다. */
	}

	if (config->max_virtual_host > 1000000) {
		config->max_virtual_host = 1000000;
	}
	else if(config->max_virtual_host < 10) {
		config->max_virtual_host = 10;
	}


	/* host rewrite용 lua 스크립트 reload 처리 부분 시작 */
	/* phase handler용 lua script는 별도의 메모리 pool을 사용한다. */
	rewrite_scripts_t *temp_scrips;
	temp_scrips = site_copy_rewrite_script(site);
	if (gscx__previous_rewrite_scripts != NULL) {
		mp_free(gscx__previous_rewrite_scripts->mp);
		gscx__previous_rewrite_scripts = NULL;
	}
	gscx__previous_rewrite_scripts = gscx__current_rewrite_scripts;
	gscx__current_rewrite_scripts = temp_scrips;
	/* host rewrite용 lua 스크립트 reload 처리 부분 끝 */

	return 0;
}

scx_config_t	*
scx_get_config()
{
	scx_config_t * config = NULL;
	pthread_mutex_lock(&gscx__config_lock);
	config = gscx__config;
	config->refcount++;
	pthread_mutex_unlock(&gscx__config_lock);
	return config;
}

int
scx_release_config(scx_config_t	* config)
{
	ASSERT(config);
	pthread_mutex_lock(&gscx__config_lock);
	config->refcount--;
	pthread_mutex_unlock(&gscx__config_lock);
	return 0;
}

static vstring_t *
scx_pickup_url(mem_pool_t *pool, const char *uri)
{
	int 	n;
	char 	*ptok = NULL;
	char 	*ptok2 = NULL;

	ptok = strchr(uri, ':'); /* uri syntax체크를 완료했으므로 NULL아님 */
	ptok++; /* ':' 넘어가기*/
	ptok2 = ptok;
	while (*ptok2 == '/') ptok2++;
	if ((ptok2 - ptok) != 2) {
		/* ':' 다음에 '//'가 있어야하는데 부족하거나 더 많은 상태 */
		scx_error_log(NULL, "URI[%s] - invalid <hierachical part> syntax\n", uri);
		return  NULL;
	}

	ptok = ptok2;

	/*
	 * url는 trigger action에 의해서 변경될 수 있으므로 clone해야함
	 */
	return vs_allocate(pool, 0, ptok, 1);
}
static int
scx_valid_uri(const char *uri)
{
	const char 	*scheme;

	if (strlen(uri) > MAX_URI_SIZE) {
		scx_error_log(NULL, "URI[%s] - larger than the limit(%d)\n", uri, MAX_URI_SIZE);
		return 0;
	}

	scheme = strchr(uri, ':');
	if (scheme == NULL) {
		scx_error_log(NULL, "URI[%s] - no scheme specified\n", uri);
		return 0; /* no scheme */
	}
	if ((strncasecmp(uri, "http", 4) == 0) ||
		(strncasecmp(uri, "https", 5) == 0))
		return 1; /* valid scheme part */
	return 0;
}
static int
scx_is_secured(const char *uri)
{
	return (strncasecmp(uri, "https", 5) == 0);
}
/*
 * host에 값이 solproxy 서버의 공인 IP이거나 loopback IP(127.0.0.1)인 경우는
 * 계속 자신의 서버에 요청을 하는 문제가 생김으로 차단한다.
 */
static int
scx_is_valid_host(nc_request_t *req)
{
	vstring_t *host = req->host;
	if (host == NULL)
		return 0;
#ifdef ZIPPER
	/* 스트리밍 기능 사용시는 ip 기반 요청이 들어 오기때문에 host에 server의 ip가 들어오기 때문에 아래의 과정이 필요 없다 */
#else
	if (strcmp(vs_data(host), vs_data(gscx__local_ip)) == 0) {
		return 0;
	}
	if (strcmp(vs_data(host), "127.0.0.1") == 0) {
		return 0;
	}
	if (strcmp(vs_data(host), "localhost") == 0) {
		return 0;
	}
#endif
	return 1;

}

static int
scx_verify_and_adjust_headers(nc_request_t *req)
{

	char 			*value = NULL;
	vstring_t		*vs_newval = NULL;

	/*
	 * XFF adjustment
	 */
	value = kv_find_val_extened(req->options, "X-Forwarded-For", FALSE, 1);
	vs_newval = vs_allocate(req->pool, 32 + (value?strlen(value):0), NULL, FALSE);
	if (value) {
		vs_strcat_lg(vs_newval, value);
		vs_strcat_lg(vs_newval, ",");
	}
	vs_strcat(vs_newval, req->client_ip);
	if (value)
		kv_replace(req->options, "X-Forwarded-For", vs_data(vs_newval), FALSE);
	return 0;
}

static vstring_t *
scx_get_myaddr(nc_request_t *req, int cfd)
{
	struct sockaddr_in 	name;
	socklen_t 			namelen = sizeof(name);
	char 				ipaddr_buf[128];
	vstring_t 			*v_addr = NULL;
	int 				err;

	err = getsockname(cfd, (struct sockaddr *)&name, &namelen);
	if (!err) {
		const char 	*p = inet_ntop(AF_INET, &name.sin_addr, ipaddr_buf, sizeof(ipaddr_buf));
		if ( p != NULL) {
			v_addr = vs_allocate(req->pool, 0, p, 1);
		}
	}
	return v_addr;
}

static int
scx_log_rotate()
{
	/* 외부에서 Log Rotate signal을 받은 경우 log 파일을 close 후에 다시 open한다. */
	logger_reopen(scx_access_logger);
	logger_reopen(scx_error_logger);
	logger_reopen(scx_origin_logger);

	return 0;
}

static int
scx_skip_interpret(nc_request_t *req, vstring_t *vkey)
{
	int 	needskip = 0;

	if (vkey && req->service && scx_contains(req->service->site, SV_IGNORE_HEADERS, vkey)) {
		needskip = 1;
	}
	return needskip;
}

/*
 * 사용하는 파일들의 소유권을 바꾸고 난후에 gid와 uid를 바꾼다.
 */
static int
scx_change_uid()
{
	/* cache directory의 owner id를 바꾼다. */
	for (int i = 0 ; i < gscx__cache_part_count; i++) {
		if( 0 != chown(vs_data(gscx__cache_part[i]), gscx__real_uid, gscx__real_gid)) {
			TRACE((T_ERROR, "chown for '%s' failed:'%s'\n", vs_data(gscx__cache_part[i]), strerror(errno)));
			return -1;
		}
		else {
			TRACE((T_INFO|T_STAT, "chown for '%s' done\n", vs_data(gscx__cache_part[i])));
		}
	}
	if( 0 != chown(vs_data(gscx__config->log_dir), gscx__real_uid, gscx__real_gid)) {
		TRACE((T_ERROR, "chown for '%s' failed:'%s'\n", vs_data(gscx__config->log_dir), strerror(errno)));
		return -1;
	}
	else {
		TRACE((T_INFO|T_STAT, "chown for '%s' done\n", vs_data(gscx__config->log_dir)));
	}

	if (0 != setgid(gscx__real_gid)) {
			TRACE((T_ERROR, "setgid for '%s' failed:'%s'\n", vs_data(gscx__config->real_user), strerror(errno)));
			return -1;
	}
	else {
		TRACE((T_INFO|T_STAT, "setgid for '%s' done\n", vs_data(gscx__config->real_user)));
	}
	if (0 != setuid(gscx__real_uid)) {
			TRACE((T_ERROR, "setuid for '%s' failed:'%s'\n", vs_data(gscx__config->real_user), strerror(errno)));
			return -1;
	}
	else {
		TRACE((T_INFO|T_STAT, "setuid for '%s' done\n", vs_data(gscx__config->real_user)));
	}


	return 0;
}

/*
 * 지정된 캐시 디렉토리가 있는지 검사 해서 없는 경우 새로 생성한다.
 */
static int
scx_check_cache_dir()
{
	int i, ret;
	struct stat  dirstat;
	for(i = 0; i < gscx__cache_part_count; i++) {
		ret = scx_check_dir(vs_data(gscx__cache_part[i]));
		if (ret < 0) {
			return -1;
		}
	}
	return 0;
}

static void
scx_get_session_info(nc_request_t *req)
{

	struct sockaddr **addr = NULL;
	struct MHD_Connection *connection = req->connection;

	/* get client information */
	addr = (struct sockaddr **)MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
	if (addr) {
		switch ((*addr)->sa_family) {
			case AF_INET6:
			case AF_INET: {
					req->client_ip = vs_allocate(req->pool, 64, NULL, FALSE);
					if (req->client_ip)
						nce_get_ip_str(*addr, req->client_ip, 64-1);
				}
				if ((*addr)->sa_family == AF_INET) {
					struct sockaddr_in *addr_in = (struct sockaddr_in *)*addr;
					req->client_port = ntohs(addr_in->sin_port);
				}
				else {
					struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)*addr;
					req->client_port = ntohs(addr_in->sin6_port);
				}
				break;
			default:
				scx_error_log(req,"URL[%s] - could not identify the remote(addr family=%ld)\n", vs_data(req->url), (*addr)->sa_family);
				break;
		}
	}

	if (!req->server_ip) {
		int 				cfd;
		struct sockaddr 	saddr;
		socklen_t 			addrlen = sizeof(saddr);
		int 				err;

		memset(&saddr, 0, sizeof(saddr));
#ifdef MHD_CONNECTION_INFO_CONNECTION_FD
		cfd = (int)(long long)MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CONNECTION_FD);


		err = getsockname(cfd, (struct sockaddr *)&saddr, &addrlen);
		if (!err) {
			req->server_ip = vs_allocate(req->pool, 64, NULL, FALSE);
			if (req->server_ip)
				nce_get_ip_str(&saddr, req->server_ip, 64);
		}

		if (saddr.sa_family == AF_INET) {
			struct sockaddr_in *addr_in = (struct sockaddr_in *)&saddr;
			req->server_port = ntohs(addr_in->sin_port);
		}
		else {
			struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)&saddr;
			req->server_port = ntohs(addr_in->sin6_port);
		}
#else
		req->server_ip = vs_strdup_lg(req->pool, "");
		req->server_port = -1;
#endif


	}
}

/*
 * parent process에 종료 신호를 보낸다.
 * signal을 통해 SIGTERM을 보내는 방법(signal은 root로 실행 했을때만 가능하다)과
 * 파일에 종료 marking을 하는 두가지 방법이 있다.
 * clear가 1인 경우 scx_write_child_msg_file()을 리셋하고
 * 0인 경우 parent가 종료 될수 있도록 scx_write_child_msg_file()에 ppid를 설정한다.
 */
static int
scx_parent_kill(int clear)
{
	pid_t ppid = getppid();
#ifdef USE_SIGTERM_SIGNAL
	if (1 == clear) return;	/* signal 방식을 사용할 경우 리셋 기능이 없기 때문에 그냥 리턴한다. */
	if ( 0 != kill(ppid, SIGTERM)) {
		TRACE((T_ERROR, "Failed to send signal. reason(%s)\n", strerror(errno)));
	}
#else
	if (1 == clear) ppid = -1;
	scx_write_child_msg_file(ppid);
#endif
	return 0;
}

/*
 * 함수 호출 시점은 pid 검사가 끝나서 중복 실행이 되지 않은 상태이어야 한다.
 * 빈파일을 설정 파일 경로에 만들고 child process에서 쓸수 있도록 권한 설정을 변경한다.
 */
static int
scx_create_child_msg_file()
{
	FILE *fptr;

	/* 파일이 있는 경우 먼저 삭제 한다 */
	scx_remove_child_msg_file();

	if ((fptr = fopen(gscx__child_msg_file, "w")) == NULL) {
		TRACE((T_ERROR, "Failed to create message file('%s'). reason(%s)\n", gscx__child_msg_file, strerror(errno)));
		return -1;
	}
	fclose(fptr);

	if( 0 != chmod(gscx__child_msg_file,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)) {
		TRACE((T_ERROR, "chmod for '%s' failed:'%s'\n", gscx__child_msg_file, strerror(errno)));
		return -1;
	}
	else {
		TRACE((T_INFO|T_STAT, "chmod for '%s' done\n", gscx__child_msg_file));
	}

	return 0;
}

/*
 * client process에서 종료시에 parent process에서 다시 client process를 생성하지 못하도록
 * msg_file에 marking을 한다.
 */
static void
scx_write_child_msg_file(pid_t ppid)
{
	FILE *fptr;
	if ((fptr = fopen(gscx__child_msg_file, "w")) == NULL) {
		TRACE((T_ERROR, "Failed to write message file('%s'). reason(%s)\n", gscx__child_msg_file, strerror(errno)));
		return;
	}
	fprintf(fptr, "%d", (int)ppid);
	fclose(fptr);
}

/*
 * child process에서 보낸 parent kill signal이 이 있는지 파일에서 확인한다.
 * signal 확인 기준은 file에 자신의 pid가 기록 되어 있는 경우 signal이 온 걸로 판단한다.
 * child process를 실행 시킬 필요가 있는 경우 1을 리턴하고
 * 그냥 종료 해야 하는 경우는 0을 리턴한다.
 */
static int
scx_check_child_msg_file()
{
	FILE *fptr;
	if (0 == gscx__need_rerun_child) return 0;	/* 이전에 종료 셋팅이 된 경우에는 아래 과정을 진행 하지 않는다. */
	if (access(gscx__child_msg_file, F_OK) == 0) {
		int fd;
		pid_t pid;
		char spid[11] = {'\0'};

		if ((fd = open(gscx__child_msg_file, O_RDONLY)) < -1) {
			TRACE((T_INFO, "Failed to open '%s'. reason(%s)\n",gscx__child_msg_file, strerror(errno)));
			return 1;
		}

		if (read(fd, spid, 10) < 0) {
			TRACE((T_INFO, "Failed to read '%s'. reason(%s)\n", gscx__child_msg_file, strerror(errno)));
			return 1;
		}
		//msg file에 있는 pid와  자신의 pid를  비교 해서 동일하면 종료를 해야 하는 경우임
		if(atoi(spid) == (int)getpid()) {
			return 0;
		}

	}
	else {
		/* 파일이 실행 도중 삭제가 되거나 한 경우에는 새로 생성한다. */
		scx_create_child_msg_file();
	}
	return 1;
}

static void
scx_remove_child_msg_file()
{
	if (access(gscx__child_msg_file, F_OK) == 0) {
		unlink(gscx__child_msg_file);
	}
}


static void
scx_write_pidfile()
{
	FILE *fptr;

	/* 화일이 이미 존재하면... */
	if (access(vs_data(gscx__pid_file), F_OK) == 0) {
		int fd;
		pid_t pid;
		char spid[11] = {'\0'};


		if ((fd = open(vs_data(gscx__pid_file), O_RDONLY)) < -1) {
				TRACE((T_ERROR, "Failed to open '%s'. reason(%s)\n", vs_data(gscx__pid_file), strerror(errno)));
				exit(EXIT_FAILURE);
		}

		if (read(fd, spid, 10) < 0) {
			TRACE((T_ERROR, "Failed to read '%s'. reason(%s)\n", vs_data(gscx__pid_file), strerror(errno)));
			exit(EXIT_FAILURE);
		}

		pid = atoi(spid);
		if(getpgid (pid) > -1) //해당 pid로 동작중인 프로그램이 있는지 검사한다.
		{
			fprintf(stderr, "%s server(#%s) is already started.\n", PROG_SHORT_NAME, spid);
			close(fd);
			exit(EXIT_SUCCESS);
		}
		unlink(vs_data(gscx__pid_file));
	}

	if ((fptr = fopen(vs_data(gscx__pid_file), "w")) == NULL) {
		TRACE((T_ERROR, "Failed to create '%s'. reason(%s)\n", vs_data(gscx__pid_file), strerror(errno)));
		exit(EXIT_FAILURE);
	}

	fprintf(fptr, "%d", (int)getpid());

	fclose(fptr);
#ifndef USE_SIGTERM_SIGNAL
	if (0 > scx_create_child_msg_file()) {
		exit(EXIT_FAILURE);
	}
#endif
}

static void
scx_remove_pidfile()
{
	FILE *fptr;

	if (access(vs_data(gscx__pid_file), F_OK) == 0) {
		int fd;
		pid_t pid;
		char spid[11] = {'\0'};

		if ((fd = open(vs_data(gscx__pid_file), O_RDONLY)) < -1) {
			TRACE((T_WARN, "Failed to open '%s'. reason(%s)\n", vs_data(gscx__pid_file), strerror(errno)));
			return;
		}

		if (read(fd, spid, 10) < 0) {
			TRACE((T_WARN, "Failed to read '%s'. reason(%s)\n", vs_data(gscx__pid_file), strerror(errno)));
			return;
		}
		//pid file에 있는 pid와  자신의 pid를  비교 해서 동일 할때만  pid파일을 삭제한다.
		if(atoi(spid) == (int)getpid()) {
			unlink(vs_data(gscx__pid_file));
#ifndef USE_SIGTERM_SIGNAL
			scx_remove_child_msg_file();
#endif
		}

	}

}


/*
 * tproxy 기능에 필요한 listening socket을 생성한다.
 */
static int
scx_make_tproxy_socket (int port)
{
	int tsock_fd = 0;
	int yes = 1;
	struct sockaddr_in tsvr_addr;
#ifdef 	IP_TRANSPARENT
	/* get the listener */
	if((tsock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		TRACE((T_ERROR, "failed to create TPROXY socket.(%s)\n", strerror(errno)));
		return 0;
	}

	/*"address already in use" error message */
	if(setsockopt(tsock_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
	{
		TRACE((T_ERROR, "setsockopt SO_REUSEADDR error(%s)\n", strerror(errno)));
		return 0;
	}
	if(setsockopt(tsock_fd, SOL_IP, IP_TRANSPARENT, &yes, sizeof(int)) == -1)
	{
		TRACE((T_ERROR, "setsockopt IP_TRANSPARENT error(%s)\n", strerror(errno)));
		return 0;
	}


	/* bind */
	tsvr_addr.sin_family = AF_INET;
	tsvr_addr.sin_addr.s_addr = INADDR_ANY;
	tsvr_addr.sin_port = htons(port);
	memset(&(tsvr_addr.sin_zero), '\0', 8);

	if(bind(tsock_fd, (struct sockaddr *)&tsvr_addr, sizeof(tsvr_addr)) == -1)
	{
		TRACE((T_ERROR, "failed to bind TPROXY socket\n", strerror(errno)));
		return 0;
	}
	/* listen */
	if(listen(tsock_fd, 32) == -1)
	{
		TRACE((T_ERROR, "failed to listen TPROXY socket\n", strerror(errno)));
		return 0;
	}
#else
	TRACE((T_ERROR, "TPROXY unsupported version.\n"));
#endif
	return tsock_fd;
}

/* gscx__config를 포인터로 리턴 *
 * gscx__default_site를 파라미터로 받음 */
static scx_config_t *
scx_init_config(site_t * site) {
	scx_config_t *config = NULL;

	config = (scx_config_t *)mp_alloc(site->mp, sizeof(scx_config_t));
	ASSERT(config);
	config->hash = 0;
	config->follow_redir = 0;
	config->negative_ttl = 30;
	config->positive_ttl = 300;
	config->fastcrc = 0;
	config->force_close = 0;
	config->ra_mb = 4;
	config->allow_dynamic_vhost = 1;
	config->force_sigjmp_restart = 0;
	config->certificate_crt_path = NULL;
	config->certificate_key_path = NULL;
#if GNUTLS_VERSION_MAJOR >= 3
	config->enable_sni = 1;
#else
	config->enable_sni = 0;
#endif
	config->cache_mem	= scx_get_cache_size(); /* MB */
#if 0
	config->max_inodes = 10000;
#else
	config->inode_mem = 0;
#endif
	config->block_size = 128; /*KB*/
	config->writeback_blocks = 32;
	config->cache_path_list = NULL;
	config->workers = 32;
	config->pool_size = DEFAULT_POOL_SIZE;
	config->max_virtual_host = 2500;
	config->cluster_ip = NULL;
	config->cluster_ttl = 4;
	config->accesslog_format = NULL;
	config->log_dir = NULL;
	config->log_gmt_time = 0;
	config->log_size	= 100;
	config->log_access	= 1;
	config->log_error	= 1;
	config->log_origin	= 1;
	config->logrotate_signal_enable = 1;
	config->host_name = NULL;
	config->https_port = 0;
	config->http_port = 80;
	config->tproxy_port = 0;
	config->listen_ip = NULL;
	config->management_port = 61800;
	config->management_listen_ip = NULL;
	config->management_enable_with_service_port = 0;
	config->request_timeout = 5;
	config->cdn_domain_postfix = NULL;
	config->permit_init_expire_duration = 60;
	config->cr_enable = 0;
	config->real_user = NULL;
	config->cold_cache_enable = 0;
	config->cold_cache_ratio = 0;
	config->disk_high_wm = 90;
	config->disk_low_wm = 80;
	config->max_path_lock_size = 10000;
	config->disk_media_cache_dir = NULL;
	config->disk_media_cache_high = 51200;
	config->disk_media_cache_low = 46080;
	config->disk_media_cache_monitor_period = 60;
	config->disk_media_cache_enable = 1;
#ifdef ZIPPER
	/* streaming 기능이 enable 되는 경우에는 MHD_USE_POLL_INTERNALLY | MHD_USE_THREAD_PER_CONNECTION를 기본으로 한다. */
	config->polling_policy = 0;
#else
	config->polling_policy = 0;
#endif
	config->enable_async_accesshandler_callback = 1;
	config->enable_async_reader_callback = 1;
	config->enable_async_complete_callback = 1;
	config->enable_async_ncapi = 1;
	config->ssl_priorities = NULL;
	config->core_file_path = NULL;
	config->nc_trace_mask = NULL;
	config->io_buffer_size = 256;
	config->media_cache_size = 1000;
	config->builder_cache_size = 2000;
	config->session_timeout = 30;
	config->session_enable = 0;
	config->ncapi_hang_detect = 1;
	config->ncapi_hang_detect_timeout = 300;

	config->stat_rc_seq = 0;
	config->stat_vrc_seq = 0;
	config->stat_svr_seq = 0;
	config->stat_idc_node_id = NULL;
	config->stat_rc_id = NULL;
	config->stat_vrc_id = NULL;
	config->stat_svr_id = NULL;
	config->stat_write_period = 10;
	config->stat_rotate_period = 60;
	config->stat_origin_path = NULL;
	config->stat_origin_prefix = NULL;
	config->stat_traffic_path = NULL;
	config->stat_traffic_prefix = NULL;
	config->stat_nos_path = NULL;
	config->stat_nos_prefix = NULL;
	config->stat_http_path = NULL;
	config->stat_http_prefix = NULL;
	config->stat_doing_dir_name = NULL;
	config->stat_done_dir_name = NULL;
	config->stat_write_enable = 0;
	config->stat_write_type = STAT_TYPE_DELIVERY;
	config->stat_origin_enable = 1;
	config->stat_traffic_enable = 1;
	config->stat_nos_enable = 1;
	config->stat_http_enable = 1;
	config->rtmp_port = 1935;
	config->rtmp_enable = 0;
	config->rtmp_worker_pool_size = 10;
	return config;
}

static int
scx_create_thread_pool ()
{
#if 0
	int poolsize = gscx__config->workers * 2; /* 일단 MHD worker의 2배수로 한다. 나중에 자동 확장이나 축소가 필요함 */
	if (poolsize > 128) {
		/* 자동으로 설정되는 thread의 최대 크기는 128개로 제한 한다. */
		poolsize = 128;
	}
	poolsize = scx_get_uint(gscx__default_site, SV_JOB_THREAD_COUNT, poolsize);
#else
	int poolsize;
	poolsize = scx_get_uint(gscx__default_site, SV_JOB_THREAD_COUNT, 512);
#endif
	if (4 > poolsize) poolsize = 4;
	gscx__thpool = thpool_init(poolsize);
	if (NULL == gscx__thpool) {
		TRACE((T_ERROR, "Thread pool create failed.\n"));
		return -1;
	}
	/* 별도의 설정이 없을 경우 job queue의 크기는 thread pool의 크기와 동일하게 설정한다. */
	gscx__max_job_queue_len = scx_get_uint(gscx__default_site, SV_MAX_JOB_QUEUE_LEN , poolsize);
	TRACE((T_INFO|T_STAT, "Thread pool created. count = %d\n", poolsize));
	TRACE((T_INFO|T_STAT, "Max job queue length = %d\n", gscx__max_job_queue_len));
	return 0;
}


static int
scx_destroy_thread_pool ()
{
	if (NULL == gscx__thpool) {
		return 0;
	}

	thpool_destroy(gscx__thpool);
	gscx__thpool = NULL;

	return 0;
}

static int
scx_deinit_config() {
	return 0;
}

static int
scx_map_nc_error(int r)
{
	int 	httpcode = MHD_HTTP_INTERNAL_SERVER_ERROR;
	switch (r) {
		case ENOENT:
			httpcode = MHD_HTTP_NOT_FOUND;
			break;
		case EREMOTEIO:
			httpcode = MHD_HTTP_SERVICE_UNAVAILABLE;
			break;
	}
	return httpcode;
}


/*
 * hang 발생시 request 헤더를 로그파일에 기록한다.
 */
static int
expire_dump_header(const char *key, const char *val, void *cr)
{
	nc_request_t    *req = (nc_request_t *)cr;
	char 			pbuf[2048]="";

	if (NULL == key) {
		TRACE((T_WARN, "[%llu] %s invalid key.\n", req->id, __func__));
		return 0;
	}
	else if (NULL == val) {
		TRACE((T_WARN, "[%llu] %s invalid value.(key : %s)\n", req->id, __func__, key));
		return 0;
	}
	TRACE((T_WARN, "[%llu] Header('%s', '%s')\n",req->id, key, val));

	return 0;
}

/*
 * CHECK_TIMER에서 설정한 시간이 지났을때 callback으로 호출 되는 function
 */
void
expire_timer_callback(void *cr)
{
	nc_request_t    *req = (nc_request_t *)cr;
	pid_t pid = getpid();
	/*
	 * callback을 호출하는 timer가 single thread이기 때문에 동시에 callback이 호출될 가능성은 없지만
	 * 나중에 timer가 교체 되는 경우를 대비해서 lock을 건다.
	 */
	pthread_mutex_lock(&__signal_handler_lock);
	TRACE((T_ERROR, "[%llu] %s, pid(%d)\n", req->id, req->msg, pid));
	TRACE((T_WARN, "[%llu] Host[%s], URL[%s]\n", req->id, vs_data(req->host),vs_data(req->url)));
	TRACE((T_WARN, "[%llu] Dump request headers.\n", req->id));
	kv_for_each(req->options, expire_dump_header, (void *)req);	/* request header를 sp로그에 기록한다. */
	if (req->objstat.st_property) {
		TRACE((T_WARN, "[%llu] Dump response headers.\n", req->id));
		kv_for_each(req->objstat.st_property, expire_dump_header, (void *)req);
	}
	TRACE((T_WARN, "[%llu] Dump start.\n", req->id));
	gdb_stack_dump(pid, gscx__default_log_path);
	TRACE((T_WARN, "[%llu] Dump end.\n", req->id));
	kill(pid, SIGKILL);
	pthread_mutex_unlock(&__signal_handler_lock);

}

int
scx_update_ignore_query_url(nc_request_t *req)
{
	char 					*query_pos = NULL;
	/* query 무시 기능이 설정 된 경우만 동작한다. */
	if (1 != req->service->ignore_query) {
		return 0;
	}
	query_pos = vs_strchr_lg(req->ori_url, '?');
	if (NULL != query_pos) {	/* request에 query parameter가 포함된 경우 query를 잘라내고 ori_url의 length를 갱신한다 */
		query_pos[0] = '\0';
		vs_update_length(req->ori_url, -1);
	}
	return 0;
}

/*
 * MHD_create_response_from_callback() 호출시 사용되는 buffer_size를 계산후 리턴
 * buffer_size를 크게 지정하면 MHD 라이브러리에서 할당하는 buffer의 크기가 커지므로 buffer 초기화 부하로 인해 성능 저하가 발생한다.
 * 반대로 buffer_size가 작게 지정하면 buffer의 크기는 작아지게 되어 buffer 초기화 부하 문제는 적어지지만 자주 read callback이 발생하게 되어 netcache쪽에서 부하가 많이 발생하게 된다.
 * 최소 크기 : 4KB
 * 최대 크기 : 256KB
 */
static size_t
scx_calc_xfer_buffer_size(nc_request_t *req, nc_ssize_t remained)
{
	size_t buffer_size = 262144;
	return buffer_size;
	if (remained <= 4096) {
		buffer_size = 4096;
	}
	else if (remained <= 16384) {
		buffer_size = 16384;
	}
	else if (remained <= 65536) {
		buffer_size = 65536;
	}

	if (req->connect_type == SCX_HTTPS && MHD_VERSION < 0x00095900) {
		/*
		 * MHD 0.9.55 버전에서는 HTTPS일때 16KB보다 buffer가 큰경우 reader callback이 한번만 호출되는 문제가 발생
		 * 0.9.59 버전을 사용하게 되면 이부분은 필요 없음
		 */
		buffer_size = 16384;
	}
	TRACE((T_DAEMON|T_DEBUG, "[%llu] read buffer size = %u, remained(%lld)\n", req->id, buffer_size, remained));
	TRACE((T_INFO, "[%llu] read buffer size = %u, remained(%lld), url = %s\n", req->id, buffer_size, remained, vs_data(req->url)));
	return buffer_size;
}




/*
 * 인증이 만료된 요청에 대해 일정시간 동안 허용하도록 설정한다.
 */
static int
scx_permit_expired_request()
{
	gscx__permit_expire = 1;
	bt_init_timer(&gscx__timer_permit_expire, "permit expired timer", 0);
	bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)&gscx__timer_permit_expire, gscx__config->permit_init_expire_duration * 1000 , scx_forbidden_expired_request, NULL);
	TRACE((T_INFO|T_STAT, "Permit expired request set for %d seconds.\n", gscx__config->permit_init_expire_duration));
	return 0;
}
/*
 * 인증이 만료된 요청에 대해 인증 실패하도록 바꾼다.
 */
void
scx_forbidden_expired_request(void *cr)
{
	gscx__permit_expire = 0;
#ifdef BT_TIMER_VER2
	while (bt_destroy_timer_v2(gscx__timer_wheel_base, &gscx__timer_permit_expire) < 0) {
		bt_msleep(10);
	}
#else
	bt_destroy_timer(gscx__timer_wheel_base, &gscx__timer_permit_expire);
#endif
	TRACE((T_INFO|T_STAT, "Forbidden expired request.\n"));
	return;
}
