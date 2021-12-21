#include <config.h>
#include <netcache_config.h>

#include <error.h>
#include <fcntl.h>
#include <iconv.h>
#include <langinfo.h>
#include <libintl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>


#include <netcache.h>
#include <block.h>
#include <tlc_queue.h>
#include "util.h"
#include "disk_io.h"
#include "threadpool.h"
#include "trace.h"
#include "hash.h"
#include "httpn_driver.h"
#include "httpn_request.h"
#include "http_codes.h"
#define ATTRIBUTE_CONSTRUCTOR __attribute__ ((constructor))
#define ATTRIBUTE_DESTRUCTOR __attribute__ ((destructor))


#define GMT_DATEFORMAT "%a, %d %b %Y %H:%M:%S"

extern __thread httpn_mux_info_t	*g__current_mpx; 
extern char							httpn__agent[];
extern cfs_origin_driver_t		__httpn_native;
extern char						__zowner[][32];
extern char						_zmpxstate[][32];

/*
 *	errno maintained in thread-local storage
 */
extern mode_t default_file_mode;
extern mode_t default_dir_mode;

extern int		g__per_block_pages;
extern void  *	__timer_wheel_base;
extern int 		__terminating;
__thread int 	__httpn_errno = 0;
__thread int 	__httpn_httpcode = 0;
static int 		__driver_instance_id = 0;
static int 		__max_read_tries = 3;
mode_t default_file_mode;
mode_t default_dir_mode;
static int    __curl_inited = 0;
link_list_t		g__mpx_q = LIST_INITIALIZER;
pthread_t		g__iocb_cleaner;
tp_handle_t		g__netpool;


extern bt_timer_t	__timer_am;

extern tlc_queue_t	g__iocb_free_queue;
extern int			g__iocb_cleaner_shutdown;


/*
 * 캐싱할 때 빼고 캐싱해야하는 기본적인 태그들
 * 나머지 태그들은 조건/상황에 따라 뺄 수 도 넣을 수 도 있음
 *
 */
const char 	*__default_filter="Connection;Keep-Alive;Content-Length;Content-Range";



/* NEW 2018-10-25 huibong origin 관련 connect, transfer timeout default 값 정의 (#32401) */

#define DEFAULT_ORIGIN_CONNECT_TIMEOUT		 5	/* origin 연결에 성공하기까지 대기 최대 시간 (sec) */
#define DEFAULT_ORIGIN_TRANSFER_TIMEOUT		10	/* origin 으로 부터 data 수신 처리 중 대기 가능한 최대 시간 (sec)*/

// NEW 2020-01-22 huibong DNS cache timeout 설정 기능 추가 (#32867)
//                - curl 상의 dns cache timeout 관련 설정 default 값 (sec)
//                - curl default 값은 60 sec
//                - 운영팀 의견 수렴 결과.. 60 sec 정도면 충분할 것으로 판단됨.
#define DEFAULT_DNS_CACHE_TIMEOUT			60


static	int httpn_destroy_instance(cfs_origin_driver_t *driver);
static	cfs_origin_driver_t * httpn_create_instance(	char *signature, 
													cfs_update_hint_callback_t sc_update_hint_proc, 
													cfs_update_result_callback_t sc_update_result_proc);
static	int		httpn_unbind_context(cfs_origin_driver_t *drv, int ctxtype);
static	int		httpn_bind_context(cfs_origin_driver_t *drv, char *prefix, nc_origin_info_t *ctx, int ctxcnt, int ctxtype);
static httpn_session_pool_t * httpn_prepare_pool(cfs_origin_driver_t *drv, nc_origin_info_t *ctx, int ctxtype);

static void 			httpn_init_try(httpn_io_context_t *iocb, int allowretry);

static void		httpn_init_once(void);
static void		httpn_init_encoding(cfs_origin_driver_t *drv, const char *lencoding, const char *sencoding);
static	int httpn_rebind_context(cfs_origin_driver_t *drv, char *prefix, nc_origin_info_t *ctx, int ctxcnt, int ctxtype);
void httpn_dump_phase_command(nc_origin_io_command_t *cmd, int pid, const char *msg);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static void * httpn_curl_malloc(size_t siz);
static void httpn_curl_free(void *ptr);
static void * httpn_curl_realloc(void *ptr, size_t siz);
static char * httpn_curl_strdup(const char *str);
static void * httpn_curl_calloc(size_t nmemb, size_t siz);
#pragma GCC diagnostic pop

static	int httpn_unbind_context_i(cfs_origin_driver_t *drv, int ctxtype);
static void httpn_destroy_pool(httpn_session_pool_t *pool);
static void httpn_update_probe_url(cfs_origin_driver_t *drv, httpn_session_pool_t *pool, char *url, int);
static void httpn_update_probe_url_ALL(cfs_origin_driver_t *drv, char *url);
static int httpn_post_handler(httpn_io_context_t *iocb);

static int httpn_read_mpx_epilog(void *u);
static void httpn_put_defered_queue(cfs_origin_driver_t *drv, int ctxtype);


static void httpn_origin_monitor(lb_t *lb, char *id, void *ud, int online);
#ifndef  NC_RELEASE_BUILD
void httpn_activity_monitor(void *u);
static unsigned long	__running = 0;
#endif

static char *
httpn_convert_or_not(iconv_t eh, const char *s)
{
#ifndef MB_LEN_MAX 
#define MB_LEN_MAX 	3
#endif
	size_t 			insize = strlen(s);
	char 			*in = (char *)s;
	size_t 			outsize = MB_LEN_MAX * (insize + 1);
	char 			*buf = XMALLOC(outsize, AI_DRIVER);

	if (!buf)
		abort();

	char *out = buf;

	if (eh == (iconv_t)-1) {
		TRACE((T_PLUGIN, "path(%s) - conversion bypassed\n", s));
		XFREE(buf);
		return XSTRDUP((char *)s, AI_DRIVER);
	}
		/* calc sizeof buff */
	iconv(eh, NULL, NULL, &out, &outsize);
	if (iconv(eh, (char **)&in, &insize, &out, &outsize) >= 0 && 
		(insize == 0) && 
		(outsize >= MB_LEN_MAX)) {
		*out = 0;
		out = XSTRDUP(buf, AI_DRIVER);
	}
	else {
		out = NULL;
	}
	XFREE(buf);


	return out;
}
/*
 * translate code set without escaping
 */
int
httpn_make_fullpath(httpn_driver_info_t *driverX, char **urlpath, const char *path, int need_esc)
{

		char 				*en_qpath = NULL;
		char 				*ex_qpath = NULL;


		en_qpath 	= httpn_convert_or_not(driverX->to_server_enc, path);
		if (!en_qpath) {
			TRACE((T_WARN, "encoding error(%s)\n", path));
			return -1;
		}
		if (need_esc) {
			ex_qpath = curl_escape(en_qpath, 0);
			TRACE((T_PLUGIN, "map0(%s), map.convert(%s), map.esc(%s)\n", path, en_qpath, ex_qpath));
			*urlpath = XSTRDUP(ex_qpath, AI_DRIVER);
		}
		else {
			TRACE((T_PLUGIN, "map0(%s), map.convert(%s), \n", path, en_qpath));
			*urlpath = XSTRDUP(en_qpath, AI_DRIVER);
		}
		if (en_qpath)
			XFREE(en_qpath);
		if (ex_qpath)
			curl_free(ex_qpath);

		return 0;
}

static  int 
httpn_errno(void)
{
	return __httpn_errno;
}
int
httpn_set_errno(int err)
{
	__httpn_errno = err;
	return err;
}

static char *
httpn_trim_string(char *str)
{
	char 	*end;
	while (isspace(*str)) str++;
	if (*str == 0) return str;
	end = str + strlen(str) -1;
	while (end > str && isspace(*end)) end--;
	*(end+1) = 0;
	return str;
}
typedef struct tag_cc_resp_element {
	char 	control[32];
} cc_resp_element_t;
static cc_resp_element_t 	__cc_resp_el[] = {
#define 	CCI_PUBLIC 				0
	{"public"},
#define 	CCI_PRIVATE 			1
	{"private"},
#define 	CCI_NO_CACHE 			2
	{"no-cache"},
#define 	CCI_NO_STORE 			3
	{"no-store"},
#define 	CCI_NO_TRANSFORM 		4
	{"no-transform"},
#define 	CCI_MUST_REVALIDATE 	5
	{"must-revalidate"},
#define 	CCI_PROXY_REVALIDATE 	6
	{"proxy-revalidate"},
#define 	CCI_MAX_AGE 			7
	{"max-age"},
#define 	CCI_S_MAXAGE 			8
	{"s-maxage"},
#define 	CCI_END 				9
	{""},
};
static int
httpn_handle_tag_cache_control(char *input, httpn_io_context_t *iocb)
{
	char 		*kept = NULL;
	char 		*token = NULL;
	char 		*value =NULL;
	int 		i;
	char 		dbuf[128]="";
	char 		*p = dbuf;
	int 		n;
	int 		np = 0;
	/* ir # 24721, max-age의 '=' 이후 값 없어지는 현상 수정 */
	char 		*winput = XSTRDUP(input, AI_DRIVER);

	token = strtok_r(winput, ",", &kept);
	while (token) {
		httpn_trim_string(token);
		if ((value = strchr(token, '=')) != 0) {
			*value = 0; /* mark termination */
			/* find value position if any */
			value += 1;
			while (*value && isspace(*value)) value++;
			if (*value == '"') {
				char 	*ep;
				ep = value + strlen(value) -1;
				value++;
				if (*ep == '"') *ep = 0;
			}
		}
		for (i = 0; i < CCI_END; i++ ) {
			if (strcasecmp(token,__cc_resp_el[i].control) == 0)
				break;
		}
		switch (i) {
			case 	CCI_PUBLIC:
				iocb->stat.st_public = 1;
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					n = sprintf(p, "public"); p+= n;
				}
				np++;
				break;
			case 	CCI_PRIVATE:
				iocb->stat.st_private = 1;
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					n = sprintf(p, "private"); p+= n;
				}
				np++;
				break;
			case 	CCI_NO_CACHE:
				iocb->stat.st_nocache = 1;
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					n = sprintf(p, "no-cache"); p+= n;
				}
				np++;
				TRACE((T_PLUGIN, "iocb->stat.st_nocache[%u]\n", iocb->stat.st_nocache));
				break;
			case 	CCI_NO_STORE:
				iocb->stat.st_nostore = 1;
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					n = sprintf(p, "no-store"); p+= n;
				}
				np++;
				break;
			case 	CCI_NO_TRANSFORM:
				iocb->stat.st_noxform = 1;
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					n = sprintf(p, "no-xform"); p+= n;
				}
				np++;
				break;
			case 	CCI_MUST_REVALIDATE:
				iocb->stat.st_mustreval = 1;
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					n = sprintf(p, "must-revalite"); p+= n;
				}
				np++;
				break;
			case 	CCI_PROXY_REVALIDATE:
				iocb->stat.st_pxyreval = 1;
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					n = sprintf(p, "proxy-must-revalite"); p+= n;
				}
				np++;
				break;
			case 	CCI_MAX_AGE: {
				long long 	lla = 0;

				if (value) 
					lla = strtoll(value, NULL, 10);

				if (p != dbuf) {strcat(p, ",");p++;}

				if (iocb->stat.st_vtime == 0) { 
					iocb->stat.st_vtime = time(NULL) + lla;

					if (TRACE_ON(T_PLUGIN)) {
						if (value) {
							n = sprintf(p, "max-age=%s ok", value); p+= n;
						}
						else {
							n = sprintf(p, "max-age=<undefined> ok"); p+= n;
						}
					}
					TRACE((T_PLUGIN, "** maxage=%lld accepted 'cause not set or override, (vtime=%lld)\n", lla, iocb->stat.st_vtime));
				}
				else {
					;
					if (TRACE_ON(T_PLUGIN)) {
						TRACE((T_PLUGIN, "** maxage=%lld overrided (vtime=%lld)\n", lla, iocb->stat.st_vtime));
						if (value) {
							n = sprintf(p, "max-age=%s discarded", value); p+= n;
						}
						else {
							n = sprintf(p, "max-age=<undefined>"); p+= n;
						}
					}
				}
				np++;
				}
				break;
			case 	CCI_S_MAXAGE: {
				long long 	llma = 0;
				if (value) {
					llma = strtoll(value, NULL, 10);
					iocb->stat.st_vtime = time(NULL) + llma;
				}
				if (TRACE_ON(T_PLUGIN)) {
					if (p != dbuf) {strcat(p, ",");p++;}
					if (value) {
						n = sprintf(p, "s-maxage=%s", value); p+= n;
					}
					else {
						n = sprintf(p, "s-maxage=<undefined>"); p+= n;
					}
				}
				np++;
				TRACE((T_PLUGIN, "***s-maxage=%s, vtime=%lld\n", (value?value:"<undefined>"), iocb->stat.st_vtime));
				}
				break;
		}

		token = strtok_r(NULL, ",", &kept);
	}
	*p = 0;

	XFREE(winput);
	TRACE((T_PLUGIN, "Cache-control={%s}\n", dbuf));
	return np;
}
static int
httpn_hit_on_filter(httpn_driver_info_t *driver, char *tag)
{
	char 	*p = NULL;
	int 	l;

	if (!driver->filter) return 0;
	p = strcasestr(driver->filter, tag);

	return (p && ( *(p + (l = strlen(p))) == ';' || *(p + l) == '\0'));

}
int
httpn_is_redirection(int c)
{
	return 	(c == HTTP_MOVED_PERMANANTLY) || 	/* http 1.0; get/post */
		(c == HTTP_SEE_OTHER) || 		/* temporary, http 1.0 not cacheable, get/post*/

		(c == HTTP_MOVED_TEMPORARILY) || 	/* temporary, http 1.1 never, always get*/
		(c == HTTP_TEMPORAL_REDIRECT) ||	/* temporary, http 1.1 not cacheable by default */
		(c == HTTP_PERMANENT_REDIRECT); 	/* permanent, http 1.1 cacheable by default */
}

/*
 * 기존 stat값이 존재하는 경우 아래와 같은 추가 태그를 붙여야한다.
 * client(solhttpd)에서 내려오는 property에서는 기본적으로 아래의 값이 없다고 가정
 * 		- If-Not-Modified-Since
 * 		- If-Match
 * 		- If-Not-Match
 *  주의) 본 함수는 대상 객체의 속성을 조회할 때만 사용해야한다.
 *  요청 시 태그 추가 순서
 * 		1. objstat가 존재하는 경우, 해당 필드 값 기준으로 생성. req_prop의 필드에도 존재하는 경우엔
 * 		   req_prop property 값 무시
 * 		2. old_s가 없는 경우, req_prop기준으로 생성
 */
static int httpn_map_property_stat(char *key, char *value, void *cb)
{
	httpn_io_context_t	*iocb = (httpn_io_context_t *)cb; 
	int 				need_remove = 0;
	time_t 				ov = 0;


	if (!strcasecmp ((const char *) key, "last-modified")) {
		ov = curl_getdate(value, NULL);
		if (ov > 0 && ov < 0x7FFFFFFFL) {
			iocb->stat.st_mtime = ov;
		}
		TRACE((T_PLUGIN, "st_mtime=%ld by %s\n", (long long)iocb->stat.st_mtime, key));
	}
	else if (!strcasecmp ((const char *) key, "Cookie")) {
		iocb->stat.st_private = 1;
		TRACE((T_PLUGIN, "st_private=%d by Cookie\n", iocb->stat.st_private));
	}
/* 
 * 2015.6.10 추가
 */
	else if (!strcasecmp ((const char *) key, "Transfer-Encoding")) {
		if (strcasestr(value, "chunked")) {
			iocb->stat.st_chunked = 1;
		}

	} /* 2015.6.10 st_chunked 세팅 추가 */
	else if (!strcasecmp ((const char *) key, "content-length")) {
		if (iocb->last_httpcode != HTTP_PARTIAL_CONTENT ) {
			/*
			 * 2016.8.31
			 * 응답이 206이 아니라면 Content-Length가 객체 전체의 크기임
			 */
			iocb->stat.st_sizeknown 	= 1; /*객체 크기 알 수 있음 */
			iocb->stat.st_sizedecled 	= 1; /*객체 크기 알 수 있음 */
			iocb->stat.st_size 		= strtoll(value, NULL, 10);
			TRACE((T_PLUGIN, "content-length = %lld, sizeknown,declaed\n", iocb->stat.st_size));
		}
	}
	else if (!strcasecmp ((const char *) key, "etag")) {
		strncpy(iocb->stat.st_devid, value, sizeof(iocb->stat.st_devid)-1);
		iocb->stat.st_devid[min(strlen(value), sizeof(iocb->stat.st_devid)-1)] = '\0';
	}
	else if (!strcasecmp ((const char *) key, "expires")) {
		if (iocb->stat.st_vtime == 0) {
		    char *zdate = (char *)kv_find_val(iocb->stat.st_property, "Date", U_FALSE);
			time_t 		t_date;
			time_t 		t_expire;
			if (zdate) {
				t_date = curl_getdate(zdate, NULL);
				t_expire = curl_getdate(value, NULL);
				if ((t_date > 0) && 
				    (t_expire > 0) &&
					(t_date < 0x7FFFFFFF) && 
					(t_expire < 0x7FFFFFFF)) {
					ov = (t_expire - t_date);
					if (ov >= 0) {
						iocb->stat.st_vtime = nc_cached_clock() + ov;
					}
				}

				else {
					goto L_rollback_normal_expire;
				}
			}
			else {
L_rollback_normal_expire:
			
				ov = curl_getdate(value, NULL);
				if (ov > 0 && ov < 0x7FFFFFFFL) {
					iocb->stat.st_vtime = ov;
				}
				else {
					TRACE((T_PLUGIN, "URL[%s] improper value  'expires : %s', too large value\n", iocb->request->url, value));
					iocb->stat.st_vtime = 0x7FFFFFFFL; 
				}
			}
		}
		else {
			TRACE((T_PLUGIN, "**** expires at : '%s' discarded (vtime=%lld, already set)\n", value, iocb->stat.st_vtime));
			/* the field already set by max-age or s-maxage */
		}
	}
	else if (!strcasecmp ((const char *) key, "Content-Range")) {
		char *psep = NULL;
		psep = strchr(value, '/');
		if (psep && isdigit(psep[1])) {
			iocb->stat.st_sizeknown = 1;
			iocb->stat.st_sizedecled = 1;
			iocb->stat.st_size = atoll(psep+1);
			TRACE((T_PLUGIN, "** Content-Size(rangeable) : %lld\n", iocb->stat.st_size));
		}
		else {
			iocb->stat.st_size = 0;
			iocb->stat.st_sizeknown  = 0;
			iocb->stat.st_sizedecled = 0;
		}
	}
	else if (!strcasecmp ((const char *) key, "cache-control")) {
		/* max-age has the more higher priority to "expires" value */
		/* s-max-age has the more higher priority to "max-age" value */
		httpn_handle_tag_cache_control((char *)value, iocb);
	}
	else if (!strcasecmp ((const char *) key, "pragma")) {
		if (strcasestr(value, "no-cache")) {
			iocb->stat.st_nocache = 1;
		}
	}
	else if (!strcasecmp ((const char *) key, "set-cookie") ||
			 !strcasecmp ((const char *) key, "set-cookie2")) {
		/*
		 *
		 * REMARK!!!!!)
		 * cookie가 있는 경우에는 private으로 강제로 전환한다
		 * 또한 cookie에 의해서 쪼개서 요청해서 가져오는 range-io는 cookie가 중간에 달라질 수 있어서
		 * 사용하지 않도록 한다
		 */
		iocb->stat.st_cookie = 1;
		/* ir #25994 관련 구현 정책 */
		iocb->stat.st_private	= 1;
		iocb->stat.st_rangeable = 0; /* 쿠키 객체는 non-rangeable로 처리*/
		TRACE((T_PLUGIN, "IOCB[%d] - st_private=1, cookie=1, rangeable=0 by set-cookie\n", iocb->id));
	}
	else if (!strcasecmp ((const char *) key, "Accept-Ranges")) {
		if (!IS_ON(iocb->mode, O_NCX_NORANDOM)) {
			if (strcasestr(value, "bytes") && !iocb->stat.st_vary)
				iocb->stat.st_rangeable = 1;
		}
		TRACE((T_PLUGIN, "IOCB[%d] - st_rangealbe=%d by AcceptRange(value[%s],vary[%d])\n", iocb->id, iocb->stat.st_rangeable == 1, value, iocb->stat.st_vary == 1));	
	}
	else if (!strcasecmp ((const char *) key, "vary")) {
		iocb->stat.st_vary = 1;
		/*
		 * vary의 경우 content-length가 head요청과 get요청에서 달라질 수 있으므로
		 * no-random 처리
		 */
		iocb->stat.st_rangeable = 0;
		TRACE((T_PLUGIN, "IOCB[%d] - set st_rangealbe=%d by Vary[%d]\n", iocb->id, iocb->stat.st_rangeable, iocb->stat.st_vary));	
	}

	if (httpn_hit_on_filter(iocb->driverX, (char *)key)) {
		need_remove = 1;
	}
	return need_remove;
}
/*
 * 현재까지 수신한 헤더 정보를 parsing하기 위해서 호출
 *
 * ir# 27068 vary 태그 관련 개선 추가
 */
static int
httpn_post_handler(httpn_io_context_t *iocb)
{
	
	char 	*vary_val = NULL;
	char 	*ce_val = NULL;
	char	zobi[256];
	//int		derror = 0;



	if (iocb->imson) 
		iocb->stat.st_originchanged 	= (iocb->last_httpcode != HTTP_NOT_MODIFIED);

	iocb->stat.st_mtime 		= 0;
	iocb->stat.st_origincode 	= iocb->last_httpcode;
	/*
	 * HS_EOH에서 property 전달할 때 필요
	 */
	iocb->last_errno = 0;
	iocb->last_curle = 0;
	httpn_set_raw_code_ifpossible(iocb, iocb->last_httpcode); /*정상처리*/
	TRACE((T_PLUGIN, "IOCB[%u] - post handling after reading header(ocode=%d,errno=%d)\n", iocb->id, iocb->last_httpcode, iocb->last_errno));

	iocb->stat.st_ifon = iocb->imson;
	if (iocb->stat.st_property) {

		/*
		 * ir# 27068
		 */
		ce_val = (char *)kv_find_val(iocb->stat.st_property, "Content-Encoding", U_FALSE);
		if (ce_val) {
			/*
			 * 응답에 Content-Encoding 존재, Vary의 대한 조정 추가
			 */
			vary_val = (char *)kv_find_val(iocb->stat.st_property, "Vary", U_FALSE);
			if (vary_val) {
				/*
				 * 서버가 vary 태그 추가했음. Accept-Encoding이 추가되어있는지 확인
				 */
				if (!strcasestr(vary_val, "Accept-Encoding")) {
					char 	strbuf[1024]="";
					sprintf(strbuf, "%s,%s", vary_val, "Accept-Encoding");
					kv_replace(iocb->stat.st_property, "Vary", strbuf, U_FALSE);
					TRACE((T_PLUGIN, "URL[%s] - Vary tag adjusted to '%s' for content-encoding\n", iocb->request->url, strbuf));
				}
			}
			else {
				kv_add_val_d(iocb->stat.st_property, "Vary", "Accept-Encoding", __FILE__, __LINE__);
				TRACE((T_PLUGIN, "URL[%s] - Vary tag added for content-encoding\n", iocb->request->url));
			}
		}
		/*
		 * end of ir# 27068 patch
		 */

		kv_for_each_and_remove(iocb->stat.st_property, httpn_map_property_stat, iocb);

		/*
		 * rangeable의 조건
		 *
		 */ 
		iocb->stat.st_rangeable = ((iocb->stat.st_rangeable == 1) &&  
										(iocb->stat.st_vary  == 0) &&
										(iocb->stat.st_chunked  == 0) 
										);
		if (iocb->rangeop && iocb->stat.st_origincode != HTTP_PARTIAL_CONTENT) {
			TRACE((T_PLUGIN, "IOCB[%d] - changed non-rangeable, 'cause status=%d\n", iocb->id, iocb->stat.st_origincode));
			iocb->stat.st_rangeable = 0;
		}


		if (iocb->inode) {
			iocb->stat.st_mode |= iocb->inode->imode;
		}
		kv_remove(iocb->stat.st_property, "date", U_FALSE);
		if (iocb->stat.st_size < 0) {
			TRACE((T_PLUGIN, "object[%s] - size not known yet\n", iocb->wpath));
			iocb->stat.st_size = 0;
			iocb->stat.st_sizeknown = 0;
			iocb->stat.st_sizedecled = 0;
		}

		TRACE(((iocb->verbose?T_WARN:0)|T_PLUGIN, "IOCB[%u].URL{%s} - RESPONSE\n"
						 " size      = %lld\n"
						 " mode      = 0x%08X\n"
						 " mtime     = %ld\n"
						 " properties= %s\n"
						 " originupchanged%s= %d\n"
						 " origincode= %d\n",
						 iocb->id,
						 iocb->request->url,
						 iocb->stat.st_size,
						 iocb->stat.st_mode,
						 (long long)iocb->stat.st_mtime,
						 obi_dump(zobi, sizeof(zobi), &iocb->stat.obi),
						 (iocb->imson?"(IMS on)":""), iocb->stat.st_originchanged,
						 iocb->stat.st_origincode));
						 
	}
#if 0
	switch (iocb->owner) {
		case HTTP_API_READ:
			if ((iocb->method == HTTP_POST) &&
				(iocb->driverX->io_update_property_proc && iocb->inode)) {
					(*iocb->driverX->io_update_property_proc)(iocb->inode,  &iocb->stat.st_stat);
			}
			break;

		case HTTP_API_GETATTR:
			/*
			 * 0-0 범위 요청이든 아니든 원본에서 
			 * non-rangeable이라고 알려옴
			 */
			if (iocb->stat.st_rangeable == 0)
				httpn_set_readinfo(iocb, 0, -1);

			break;
#if defined(__clang__)
		default:
			break;
#endif
	}
#else
	if ((iocb->owner == HTTP_API_READ) &&
		(iocb->method == HTTP_POST) &&
		(iocb->driverX->io_update_property_proc && iocb->inode)) {

		(*iocb->driverX->io_update_property_proc)(iocb->inode,  &iocb->stat);

	}
#endif
	return 0;
}

static size_t
httpn_head_dispatch(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	char				*line;
	char				*tag;
	char				*value;
	httpn_io_context_t	*iocb = (httpn_io_context_t *)userdata; 
	int 				asiz = 0;

	char				dbuf[1024];


	asiz = size *nmemb;
	DEBUG_ASSERT_IOCB (iocb, httpn_mpx_state_is(iocb, IOCB_MPX_TRY_RUN));

	iocb->post_avail  = 0; /* header 수신중이면 post_avail 은 ALWAYS 0 */

	if (iocb->inredirection) return asiz; /* bypass */

	if (iocb->stat.st_property == NULL) {
		TRACE((T_WARN, "IOCB[%u] - property is NULL;%s\n", iocb->id, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
		return 0;
	}

	if (iocb->last_httpcode == HTTP_PARTIAL_CONTENT) {
		iocb->entire = 0;
		iocb->stat.st_rangeable = 1;
	}

	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u] - %s", iocb->id, ptr)); /* '\n' 필요없음 */

	switch (httpn_state(iocb->session)) {
		case HS_HEADER_IN:
			line 	= (char *)alloca(asiz + 1);
			tag 	= (char *)alloca(asiz + 1);
			value 	= (char *)alloca(asiz + 1);
			memcpy(line, ptr, asiz);
			line[asiz] = 0;

			if (sscanf(line, "%[^:]: %[^\r\n]", tag, value) == 2) {
				kv_add_val_d(iocb->stat.st_property, tag, value, __FILE__, __LINE__);
			}
			else if (strncmp(line, "\r\n", 2) == 0) {
				DEBUG_ASSERT_IOCB(iocb, httpn_is_state(iocb->session, HS_HEADER_IN));

				/* --> HS_EOH로 전이 */
				mpx_STM(iocb, IOCB_EVENT_PROPERTY_DONE, 0, __FILE__, __LINE__); 

				/*
				 * target_action == HS_EOH인지 확인
				 */
				if (iocb->session && httpn_is_state(iocb->session, iocb->target_action)) {
					/*
					 * buffer정보가 없으므로
					 * read 요청이 올때까지 대기
					 */
					/* MPX_RUN --> MPX_PAUSED */
					httpn_mpx_pause_iocb_nolock(iocb);
					/*
					 * application이 지정한 목표 상태 도달
					 * 다음 요청 때 까지 대기해야함(application과 동기 시점)
					 * 참고로 pause는 target_action이 HS_EOH가 아니라 HS_BODY라서
					 * 이 조건은 절대 불가능
					 */
					if (iocb->method != HTTP_POST) {
						int		de;
						iocb->last_errno = httpn_handle_error(iocb, &iocb->last_curle, &iocb->last_httpcode, &de);
						httpn_set_raw_code_ifpossible(iocb, iocb->last_httpcode); /*정상처리*/
						TRACE((T_PLUGIN, "IOCB[%d] : reached to the target action;%s\n", iocb->id, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
						apc_overlapped_signal(&iocb->event);
					}
				}
			}
			/*
			 * 필요하면 여기에서 2 x CRLF 체크해서 EOH 확인 가능
			 */
			break;
		case HS_EOH:
			TRACE((iocb->tflg|T_PLUGIN|T_WARN, "IOCB[%u].URL[%s] - invalid line, ['%s'], skipped 'cause in HS_EOH\n",
							iocb->id,
							iocb->request->url,
							ptr));
			kv_add_val_d(iocb->stat.st_property, (char *)ptr, "", __FILE__, __LINE__);
			break;
		case HS_BODY:
			TRACE((iocb->tflg|T_PLUGIN|T_WARN, "IOCB[%u].URL[%s] - invalid line, ['%s'], skipped 'cause in HS_BODY\n",
							iocb->id,
							iocb->request->url,
							ptr));
			kv_add_val_d(iocb->stat.st_property, (char *)ptr, "", __FILE__, __LINE__);

			break;
		case HS_DONE:
			TRACE((iocb->tflg|T_PLUGIN|T_WARN, "IOCB[%u].URL[%s] - invalid line, ['%s'], skipped 'cause in HS_DONE\n",
							iocb->id,
							iocb->request->url,
							ptr));
			kv_add_val_d(iocb->stat.st_property, (char *)ptr, "", __FILE__, __LINE__);
			break;

	}



	return asiz;
}


#if NOT_USED
/*
 * 원본 객체의 크기를 미리 알고 있는 경우 사용
 */
static nc_off_t
httpn_endof_xfer(httpn_io_context_t *iocb)
{
	nc_off_t 	o1, o2;

	
	o1 = NC_ORG_BLOCK_OFFSET(ASIO_CONTEXT(iocb->vector, iocb->vector->iov_cnt-1)->blkno) + NC_BLOCK_SIZE ;
	o1 = min((long long)(o1), (long long)iocb->vector->iov_inode->size);
	o2 = (nc_off_t)ASIO_CONTEXT(iocb->vector, iocb->u.r.blk_cursor)->blkno * NC_BLOCK_SIZE + iocb->u.r.blk_off;
	
	return (nc_off_t)(o1 - o2) == 0;
}
#endif

/*
 * 원본 객체의 크기를 미리 알고 있는 경우 사용
 */
static void
httpn_make_range(nc_asio_vector_t *vector, int for_extend, int cursor, cfs_off_t *range_begin, cfs_off_t *range_end)
{
	
	DEBUG_ASSERT_ASIO(vector, vector->iov_inode, (cursor < vector->iov_cnt));
	DEBUG_ASSERT_ASIO(vector, vector->iov_inode, vector->iov_cnt > cursor);
	DEBUG_ASSERT_ASIO(vector, vector->iov_inode, ASIO_CONTEXT(vector, cursor)->block != NULL);
	DEBUG_ASSERT_ASIO(vector, vector->iov_inode, ASIO_CONTEXT(vector, vector->iov_cnt-1)->block != NULL);

	*range_begin = NC_ORG_BLOCK_OFFSET(ASIO_CONTEXT(vector, cursor)->block->blkno);

	*range_end = NC_ORG_BLOCK_OFFSET(ASIO_CONTEXT(vector, vector->iov_cnt-1)->block->blkno) + NC_BLOCK_SIZE ;


	*range_end = min((long long)(*range_end), (long long)dm_inode_size(vector->iov_inode, U_FALSE));
	*range_end = *range_end - 1LL;

	if (*range_end < 0 || *range_begin < 0) {
		char	ibuf[1024];
		TRACE((T_PLUGIN, "VOLUME[%s]/K[%s]/INODE[%u].{%s} - invalid range made (begin=%lld, end=%lld)\n",
				vector->iov_inode->volume->signature,
				vector->iov_inode->q_id,
				vector->iov_inode->uid,
				dm_dump_lv1(ibuf, sizeof(ibuf), vector->iov_inode),
				*range_begin,
				*range_end));

		*range_end = max(0, *range_end);
		*range_begin = max(0, *range_begin);
	}
	return;
}


/*
 * 	IO 요청 허용 범위 내인지 확인
 */
static int
httpn_buffer_avail(httpn_io_context_t *iocb, nc_off_t objoff)
{
	int avail = 0;

	/*
	 * IOCB에 명시된 아래 두 필드를 기준의 버퍼 준비여부 파악
	 * (httpn_set_readinfo()에 의해 설정)
	 * 		iocb->reqoffset	= off;
	 * 		iocb->reqsize 	= siz;
	 */

	/*
	 * reqsize가 0보다 작으면 항상 buffer는 있다고 가정
	 */
	avail = (iocb->reqsize < 0)?U_TRUE: (objoff < (iocb->reqoffset + iocb->reqsize));
	return avail;
}
size_t 
httpn_block_reader(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	int					prepared 		= 0;
	int					length 			= size*nmemb;
	httpn_io_context_t	*iocb 			= (httpn_io_context_t *)userdata;
	int 				blkremained 	= 0;
	cfs_size_t			eof;
	int 				tocopy 			= 0;
	int					copied 			= 0;
	char				*inbuffer 		= (char *)ptr;
	int					remained 		= 0;
	int					ba 				= 0;
	int 				loop 			= 1;
	char 				tbuf[4092];
	fc_inode_t			*inode			= NULL;

	DEBUG_ASSERT(inbuffer != NULL);
	DEBUG_ASSERT(length >= 0);
	inode = iocb->inode;
	
	/*
	 *
	 * 현재 redirection 메시지의 응답 데이타 처리
	 * inredirection이 TRUE이면 모든 데이타 버림
	 */

	if (iocb->inredirection) return length; /* bypass */

#if 0
	if (iocb->needpause != 0 && iocb->target_action == HS_EOH) {
		/*
		 * method=POST 일 때 아직 read_mpx() 호출이 들어오지 않아서
		 * block_reader() 내부 진입을 할 수 없음
		 * httpn_read_mpx()호출이 진행될 때까지 input 처리는 대기함
		 */
		TRAP;
		iocb->needpause		= 0;
		iocb->needresume	= 0;
		mpx_STM(iocb, IOCB_EVENT_PAUSE_TRY, 0, __FILE__, __LINE__);
		copied = CURL_WRITEFUNC_PAUSE;
		TRACE((T_PLUGIN, "IOCB[%d] - going to sleep until client signals\n", iocb->id));
		return copied;
	}
#endif


	DEBUG_ASSERT_IOCB(iocb, (iocb->vector != NULL));
	httpn_migrate_http_state(iocb, HS_BODY, __FILE__, __LINE__);

	TRACE((0, ">>>IOCB[%u]/(%d-th): received=%d, iocb->accepted=%ld(so far)\n", 
					iocb->id,
					iocb->tries,
					length,
					iocb->accepted));

	iocb->received += length;
	remained 		= length;

#if 1
	if (iocb->canceled) {
		TRACE((T_PLUGIN|T_ASIO, "IOCB[%u] - cancel detected\n", iocb->id));
		
		return -1; /* ABORT network read */

	}
#endif

	if (__terminating || length < 0) {
		TRACE((0, "[%d].Volume[%s].URL[%s] - IOCB[%u] so_far filled[%d], block.cursor=%d, received[%d] - got length < 0\n",
								loop,
								iocb->driver->signature,
								iocb->request->url,
								iocb->id,
								(int)iocb->u.r.filled, 
								(int)iocb->u.r.blk_cursor, 
								(int)length));
		iocb->canceled = 1;
		
		
		return -1; /* quit network read */
	}


	/*
	 * OSD_READ_ENTIRE이고 재시도 상황이면,
	 * 이전에 받은 위치까지의 데이타는 함(왜냐하면 이전에 이미 버퍼에 복사해두었음)
	 */
	if (asio_is_command_type(iocb->vector, 0, IOT_OSD_READ_ENTIRE) && iocb->tries > 1) {
		int			toskip = 0;
		cfs_off_t	fofforg = 0;
		remained = length;
		toskip = (int)(iocb->u.r.file_off_prev - iocb->u.r.file_off); 
		if (toskip > 0) {
			toskip				= min(toskip, length); /* real skip bytes */
			remained			-= toskip;
			fofforg				= iocb->u.r.file_off;
			iocb->u.r.file_off 	= iocb->u.r.file_off + toskip;
			TRACE((0, "Volume[%s].Key[%s] - IOCB[%u]{f.off=%lld) == {length=%d, skip=%d] => {f.off=%lld}\n",
										iocb->inode->volume->signature, 
										iocb->request->url,
										iocb->id,
										fofforg,
										length,
										toskip,
										iocb->u.r.file_off
										));
		}

		if (remained == 0) {
			/*
			 * 이미 받았던 데이타뿐임, 여기서 리턴
			 */
			TRACE((0, "Volume[%s].Key[%s] - IOCB[%u] {f.off=%lld, blk_cursor=%d}, nothing to do, return\n",
										iocb->inode->volume->signature, 
										iocb->request->url,
										iocb->id,
										iocb->u.r.file_off,
										iocb->u.r.blk_cursor));
			return length;
		}
	}
	if (remained <= 0) {
		TRACE((T_PLUGIN|T_ASIO, "[%d]INODE[%ld].ASIO[%d] : IOCB[%u]/CNT[%d/%d] blk # %ld BUID[%ld]/S[%d]/P[%d] - EOT\n",
								loop,
								(long)iocb->inode->uid,
								(int)iocb->vector->iov_id, 
								iocb->id,
								(int)iocb->u.r.blk_cursor,
								(int)iocb->vector->iov_cnt,
								(long)ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->blkno,
								(long)ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->buid,
								(int)ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->stat,
								(int)ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->binprogress));
		if (iocb->vector->iov_asio_callback) {
			TRACE((0, "INODE[%u].blk#%u - done\n", inode->uid, ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->blkno));
			iocb->vector->iov_asio_callback(iocb->vector, iocb->u.r.blk_cursor, iocb->u.r.file_off - iocb->u.r.blk_off , NULL, iocb->u.r.blk_off, 0);
		}
		iocb->u.r.blk_fini = iocb->u.r.blk_cursor;

		iocb->u.r.blk_cursor++;
		iocb->u.r.blk_off = 0;
		iocb->u.r.blk_filled++;
	}
	else {

		ba = httpn_buffer_avail(iocb, iocb->u.r.file_off);

		while (remained > 0 && ba) {

				/*
				 * r.blk_cursor로 지정한 블럭의 준비
				 */
				if (iocb->vector->iov_asio_prep_context_callback) {
					/*
					 * blk 가 준비되지 않은 경우 준비하도록 콜백 호출
					 * blk가 준비되었다고 하더라도 실제 데이타 저장을 위한 page buffer할당은 별개임
					 */
				
					prepared = (*iocb->vector->iov_asio_prep_context_callback)(iocb->vector, iocb->u.r.blk_cursor, iocb->u.r.file_off, U_TRUE, U_TRUE);
	
					if (prepared < 0) {
						TRACE((T_WARN, "[%d]Volume[%s].URL[%s] - content length %lld defined but contains more than that, aborting\n",
										loop,
										iocb->inode->volume->signature, 
										iocb->request->url,
										dm_inode_size(iocb->inode, U_FALSE)));
						ba = 0; /* buffer is not avail */
	
						break;
					}
				}


				/*
				 * r.blk_cursor 블럭에 copy할 바이트 수 계산
				 */
				blkremained  	= NC_BLOCK_SIZE - iocb->u.r.blk_off;
				tocopy 			= min(remained, blkremained);

				/*
				 * 준비된 blk의 적정한 page buffer에[ 데이타 복사
				 * 만약 page buffer가 할당되지 않은 상태라면 page buffer 할당은 자동으로 실행
				 */
				iocb->vector->iov_asio_bio_callback(iocb->vector, iocb->u.r.blk_cursor, iocb->u.r.file_off, inbuffer, tocopy);
				TRACE((T_PLUGIN, "[%d]INODE[%ld].ASIO[%d] : IOCB[%u]/BIDX[%d/%d] blk # %ld - %d bytes filled;%s\n",
										loop,
										(long)iocb->inode->uid,
										(int)iocb->vector->iov_id, 
										iocb->id,
										(int)iocb->u.r.blk_cursor,
										(int)iocb->vector->iov_cnt,
										(long)ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->blkno,
										tocopy,
										httpn_dump_iocb(tbuf, sizeof(tbuf), iocb)
										));
				/*
				 * 복사한 바이트 수(tocopy)만큼 각종 컨텍스트에 반영
				 */
				iocb->accepted 					= iocb->accepted+tocopy;
				iocb->vector->iov_xfer_bytes 	= iocb->vector->iov_xfer_bytes + tocopy;
				iocb->u.r.blk_off 				= iocb->u.r.blk_off + tocopy;
				iocb->u.r.file_off 				= iocb->u.r.file_off + tocopy;
				iocb->u.r.filled 				= iocb->u.r.filled + tocopy;
				remained 						= remained - tocopy;
				blkremained 					= blkremained - tocopy;

				inbuffer 						= (char *)inbuffer + tocopy;


				eof = 0; /* IOT_OSD_READ_ENTIRE의 경우 전송의 끝을 알 수 없다고 가정*/
				if (iocb->vector->iov_inprogress && (iocb->vector->iov_inode->ob_sizeknown && iocb->vector->iov_inode->ob_sizedecled)) {
					eof = (iocb->u.r.file_off == iocb->vector->iov_inode->size)? ASIO_EOV:0;
				}
				/* 
				 * check if the block completed with data
				 */
				if (blkremained == 0) {
					TRACE((0, "[%d]Volume[%s].URL[%s].INODE[%ld] : IOCB[%u], ASIO[%d].BIDX[%d/%d](blk# %ld) - "
														"OK, done(%d,%d) EOT(EOF)[%08X]\n",
														loop,

														iocb->driver->signature,
														iocb->request->url, 
														iocb->inode->uid,

														iocb->id,

														(int)iocb->vector->iov_id,
														(int)iocb->u.r.blk_cursor,
														(int)iocb->vector->iov_cnt,
														(long)ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->blkno,
														(int)iocb->vector->iov_donecnt[0],
														(int)iocb->vector->iov_donecnt[1],
														(unsigned int)eof
														));
														
					fc_blk_t 	*blk = ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor);
	
					/*
					 * 해당 blk에 대한 모든 데이터 카피 완료. 
					 * 완료 통보
					 */
					TRACE((T_PLUGIN, "INODE[%u].ASIO[%d] - blk#%u filled completely(httpcode=%d, curle=%d)\n", 
									inode->uid, 
									iocb->vector->iov_id, 
									ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->blkno,
									iocb->last_httpcode,
									iocb->last_curle
									));
#ifndef NC_RELEASE_BUILD
			        TRACE((0, "INODE[%d] - blk#%d/BUID[%d] NIO cleared\n", inode->uid, blk->blkno, BLOCK_UID(blk)));

					ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->nioprogress = 0;
#endif					
					(*iocb->vector->iov_asio_callback)(	iocb->vector, 
														iocb->u.r.blk_cursor, 
														iocb->u.r.file_off - iocb->u.r.blk_off, 
														NULL, 
														iocb->u.r.blk_off, 
														0) ;
					iocb->u.r.blk_fini 		= iocb->u.r.blk_cursor;
					iocb->u.r.blk_off 		= 0;
					iocb->u.r.blk_cursor++;
					iocb->u.r.blk_filled++;
				}


				TRACE((0, 	"[%d]IOCB[%u]/INODE[%u] - ASIO[%d]/CURSOR[%d/%d];remained=%d, bufavail=%d(so far accepted=%lld)\n", 
									loop, 
									iocb->id, 
									iocb->inode->uid, 
									iocb->vector->iov_id, 
									iocb->u.r.blk_cursor, 
									iocb->vector->iov_cnt, 
									remained, 
									ba, 
									(long long)iocb->accepted
									));



				copied 	+= tocopy;
				ba 		= httpn_buffer_avail(iocb, iocb->u.r.file_off);
				loop++;
		}
	}
L_finish:
	if (ba == 0 && remained > 0) {
		iocb->last_curle 	= CURLE_OK;
		iocb->premature 	= 1;

		TRACE((T_PLUGIN,	"[%d]Volume[%s].URL[%s] - IOCB[%u] operation prematually done ;buf.avil=%d,httpcode=%d(%s)\n", 
								loop,
								iocb->driver->signature, 
								iocb->request->url, 
								iocb->id,
								ba,
								iocb->last_httpcode, 
								hcd_errstring(iocb->last_httpcode)
								));

		httpn_migrate_http_state(iocb, HS_DONE, __FILE__, __LINE__);
	}
	httpn_mpx_timer_restart(iocb, __FILE__, __LINE__);
	TRACE((T_PLUGIN,	"[%d]Volume[%s].URL[%s] - IOCB[%d] copied=%d;httpcode=%d(%s)\n", 
						loop,
						iocb->driver->signature, 
						iocb->request->url, 
						iocb->id,
						copied,
						iocb->last_httpcode, 
						hcd_errstring(iocb->last_httpcode)
						));
	return copied;
}
struct tag_post_info {
	char 			*opaque;
	nc_size_t		len;
	nc_size_t		remained;
};

int
httpn_getattr_completion(void *u)
{
	httpn_io_context_t	*iocb 		= (httpn_io_context_t *)u;
	apc_open_context_t	*oc 		= NULL;
	nc_uint32_t			tflg 		= 0;

	char				ibuf[4096]	= "";


	

	TRACE((T_PLUGIN, "IOCB[%d] - getattr completed;%s\n", iocb->id, httpn_dump_iocb(ibuf, sizeof(ibuf), iocb)));
	if ((oc = (apc_open_context_t *)iocb->private) != NULL) {
		memcpy(&oc->query_result.stat, &iocb->stat, sizeof(nc_stat_t));
		iocb->stat.st_property = NULL; /* iocb free 과정에서 st_property가 free되지 않도록*/
		oc->query_result.result 	= iocb->last_errno;
		TRACE((tflg|iocb->tflg|T_PLUGIN, "IOCB[%u] - sending the completion event(O-CTX[%p].result=%d, st_origincode=%d);%s\n",
							iocb->id, 
							oc,
							oc->query_result.result,
							oc->query_result.stat.st_origincode,
							httpn_dump_iocb(ibuf, sizeof(ibuf), iocb)
							));
		asio_post_apc(oc, __FILE__, __LINE__);
		iocb->private = NULL;
	}


	
	return 0;

}
static int 			
httpn_getattr_mpx(	struct cfs_origin_driver 	*drv, 
					char 						*path, 
					nc_stat_t 					*objstat, 
					nc_stat_t 					*new_s, 
					nc_kv_list_t 				*req_prop, 
					nc_mode_t 					mode, 
					apc_open_context_t 			*oc)
{
	httpn_io_context_t	*iocb = oc->origin;
	int					cached 		= (objstat != NULL); /* 객체가 full-caching상태 의미 */
	char				dbuf[2048];
	httpn_pin_state_t	target_action = HS_EOH;

	



	if (drv->shutdown) {
		httpn_set_errno(EPERM);
		
		return -EPERM;
	}

	DEBUG_ASSERT_IOCB(iocb, httpn_mpx_state_is(iocb, IOCB_MPX_INIT));
	TRACE((iocb->tflg|T_PLUGIN, "PATH[%s],mode(0x%08X),alreadycached(%d) \n", path, mode, cached));
	httpn_set_owner(iocb, HTTP_API_GETATTR);

	if (oc && oc->inode) {
		iocb->rangeable 	= oc->inode->ob_rangeable;
		iocb->sizeknown 	= oc->inode->ob_sizeknown;
		if (iocb->sizeknown)
			iocb->knownsize 	= oc->inode->size;
	}

	httpn_set_private(iocb, oc);
	apc_overlapped_switch(&iocb->event, iocb, httpn_getattr_completion);
	TRACE((T_PLUGIN, "IOCB[%d] - completion callback to 'httpn_getattr_completion' set\n", iocb->id));
	if (iocb->method == HTTP_POST) {
		/*
		 * psuedo 속성 리턴
		 */
		if (httpn_online(drv, NCOF_READABLE)) {
			TRACE((iocb->tflg|T_PLUGIN, "POST on URL[%s], pseudo property made(O-CTX=%p) \n", path, oc));
			iocb->stat.st_private 		= 1;
			iocb->stat.st_nocache 		= 1;
			iocb->stat.st_pseudoprop 	= 1;
		}
		else {
			iocb->last_errno			= EREMOTEIO;
			iocb->last_httpcode			= HTTP_BAD_GATEWAY;
			TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u]/PATH[%s] found the origin down(O-CTX=%p)\n", iocb->id, path, oc));
			httpn_set_raw_code_ifpossible(iocb, HTTP_BAD_GATEWAY);
		}
		//target_action = HS_BODY;
		apc_overlapped_signal(&iocb->event);
		return 0;
	}



	httpn_init_try(iocb, U_TRUE);

	if (cached && (!iocb->driverX->useheadforattr)) {
		httpn_request_add_ims_header(iocb, objstat, oc);
	}
	/*
	 * 새로운 헤더 정보와 설정을 이용해서 try 정보 update
	 */


	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u] - PATH[%s] dispatching(target=%d) ;%s\n", iocb->id, path, target_action, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
	httpn_mpx_execute_iocb(iocb, target_action);

	return 0;
}


static int 						
httpn_read_mpx(cfs_origin_driver_t *driver, nc_asio_vector_t *vector, nc_origin_session_t octx)
{
	cfs_off_t			range_begin, range_end;
	char 				byte_range[256]="";
	httpn_io_context_t	*iocb = octx;
	dev_file_handle_t	*dev_handle;
	int 				needresume = 0;
	char				buf[2048];
	nc_mode_t			tmode;
	nc_uint32_t			kept;
	httpn_mux_info_t 	*mpx = NULL;

	DEBUG_ASSERT_FILE(vector->iov_inode, INODE_GET_REF(vector->iov_inode) > 0);
	

	cfs_lock_driver(driver, cfs_lock_shared);

	if (driver->shutdown) {
		TRACE((0, "Driver[%s] - read tried while shutdowning\n", driver->name));
		httpn_set_errno(EPERM);
		cfs_unlock_driver(driver, cfs_lock_shared);
		
		return -EPERM;
	}



	dev_handle = (dev_file_handle_t *)vector->iov_inode->driver_file_data;


	if (iocb == NULL) {
		/*
		 * caller did not provide an origin context
		 */

		DEBUG_ASSERT_FILE(vector->iov_inode, INODE_GET_REF(vector->iov_inode) > 0);
		tmode = (vector->iov_inode->imode & ~O_NCX_TRY_NORANDOM);
		iocb = httpn_create_iocb(driver, dev_handle->path, vector->iov_kv_out, tmode, vector, U_TRUE, NULL, &kept, __FILE__, __LINE__);
		if (iocb) {
			httpn_set_owner(iocb, HTTP_API_READ);
			iocb->keptid = kept;
		}
		DEBUG_ASSERT_FILE(vector->iov_inode, INODE_GET_REF(vector->iov_inode) > 0);
		TRACE((T_PLUGIN, "%s/%s - IOCB[%d] set(created)\n", driver->signature, dev_handle->path, (iocb?iocb->id:-1)));
	}
	else {
		/*
		 * 아래 두개의 순서 지킬것:owner바뀌면  vector가 그순간부터 유효하다고 가정함
		 */
		httpn_bind_vector(iocb, vector);
		httpn_set_owner(iocb, HTTP_API_READ);

		if (iocb->mpx) {
			/*
			 * iocb->mpx != NULL
			 */
	
			TRACE((T_PLUGIN, "%s/%s - IOCB[%d] set(resume)\n", driver->signature, dev_handle->path, (iocb?iocb->id:-1)));
			mpx = iocb->mpx;


			if (httpn_mpx_state_is(iocb, IOCB_MPX_FINISHED)) {
				TRACE((T_PLUGIN, "IOCB[%d] *** early finished;%s\n", iocb->id, httpn_dump_iocb(buf, sizeof(buf), iocb)));
				httpn_read_mpx_epilog(iocb);
				cfs_unlock_driver(driver, cfs_lock_shared);
				return 0;
			}
			else {
				needresume++;
			}

		}
	}
	cfs_unlock_driver(driver, cfs_lock_shared);


	/*
	 * after binding vector 
	 */
	TRACE((T_PLUGIN, "IOCB[%d] - completion callback to 'httpn_read_mpx_epilog' set\n", iocb->id));
	apc_overlapped_switch(&iocb->event, iocb, httpn_read_mpx_epilog);



	if (asio_is_command_type(vector, 0, IOT_OSD_READ)) {
		httpn_make_range(vector, U_FALSE, 0, &range_begin, &range_end);
		snprintf(byte_range, sizeof(byte_range), "bytes=%lld-%lld", range_begin, range_end);
		iocb->u.r.total 		= (range_end - range_begin + 1);
	}
	else {
		iocb->u.r.total 		= -1;
	}

	vector->state 	= ASIOS_RUNNING;

	/* EOT mark를 전송해야하는지 확인 */

	httpn_init_try(iocb, U_TRUE);



	if (needresume) {

		/*
		 * IMPORTANT!
		 *	read_mpx()가 준비되어서 HS_EOH에서 pause하지 않고
		 *	body processing으로 넘어갈 수 있도록 target_action을 변경
		 */
		iocb->target_action = HS_BODY;

		DEBUG_ASSERT(iocb->mpx != NULL);
		DEBUG_ASSERT(iocb->session != NULL);

		LO_CHECK_ORDER(LON_NET_MUX);
		LO_PUSH_ORDER_FL(LON_NET_MUX, __FILE__, __LINE__);
		_nl_lock(&mpx->mpxlock, __FILE__, __LINE__ );

		TRACE((iocb->tflg|T_PLUGIN, "IOCB[%d] - read invoked(post_avail=%d);%s\n", 
							iocb->id,
							iocb->post_avail,
							httpn_dump_iocb(buf, sizeof(buf), iocb)

			));

		DEBUG_ASSERT (HTTPN_NEED_RESUME(iocb)) 
		httpn_mpx_resume_iocb_nolock(iocb, U_TRUE);

		_nl_unlock(&mpx->mpxlock);
		LO_POP_ORDER(LON_NET_MUX);

	}
	else {
		httpn_mpx_execute_iocb(iocb, HS_BODY);
	}



	return 0;
}



static	dev_file_handle_t * 
httpn_open(cfs_origin_driver_t *drv, char *path, void *hint, int hint_len, int mode, nc_xtra_options_t *opt)
{
	dev_file_handle_t		*devhandle = NULL;
	httpn_driver_info_t 	*driverX;


	

	driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	if (drv->shutdown) {
		httpn_set_errno(EINVAL);
		
		return NULL;
	}

	devhandle = (dev_file_handle_t *)XMALLOC(sizeof(dev_file_handle_t), AI_DRIVER);
	devhandle->path = path;
	devhandle->mode = mode;
	devhandle->driver = drv;
	
	return devhandle;
}
static int
httpn_iserror(int code)
{
	return (code < 200 || code >= 400);
}
static int
httpn_issamecode(int cached, int newcode)
{
	int c_1, c_2;
	int 	same = 0;
	c_1 = cached / 100;
	c_2 = newcode / 100;

	if (newcode == HTTP_NOT_MODIFIED)
		return U_TRUE;

	same =  ( (c_1 == 2 && c_1 == c_2)     /* 2xx response */ 
				|| (c_1 == 2 && c_2 == 3)  /* 301 rediection */
				|| (cached == newcode) );


	if (!same) {
		same = httpn_is_redirection(cached) && httpn_is_redirection(newcode); 
	}

	return same;
}


#if NOT_USED
static int	
httpn_flush(struct tag_fc_inode_info *handle)
{
	return 0;
}
#endif
static int 
httpn_close(dev_file_handle_t *handle)
{
	httpn_file_handle_t		*httphandle;
	cfs_origin_driver_t		*drv;
	int						r = 0;

	

	drv		= handle->driver;
	httphandle	= (httpn_file_handle_t *)handle->driver_data;

#if 0
	TRACE((T_PLUGIN, "handle[%p] - closing\n", handle));

	if (httphandle) {
		if (httphandle->session) {
			/* 해당 handle이 write용으로 사용된 상태임 */

			/*
			 * chunked transfer인 경우 아직 EOT를 모르고 있을 확율이 높음
			 */
			if (httphandle->tid_valid) {
				pthread_mutex_lock(&httphandle->lock);
				if (httphandle->state == HTTP_IO_WAIT_INPUT) {
					TRACE((0, "URL[%s] - still waiting for input data, marking EOT\n", httphandle->path));
					httphandle->eot = 1;
					pthread_cond_signal(&httphandle->cond_data_avail);
				}
				pthread_mutex_unlock(&httphandle->lock);
				/* 전송 thread 종료 대기 */
				if (pthread_join(httphandle->tid, NULL)) {
					TRACE((0, "URL[%s] - thread join failed(%s)\n", httphandle->path, strerror(errno)));
				}
				TRACE((T_PLUGIN, "URL[%s] - %lld bytes xfered, httpcode=%d\n",  httphandle->path, httphandle->xfered, kv_get_raw_code(httphandle->req_options)));

				if (httphandle->req) {
					TRACE((T_PLUGIN, "URL[%s] - request %p destroying...\n", httphandle->path, httphandle->req));
					httpn_request_destroy(httphandle->req);
				}

			}
			if (httphandle->session) {
				TRACE((T_PLUGIN, "IOCB[%u] - removing EIF[%p]\n", httphandle->session->eif));
				httpn_cleanup_session(httphandle->session);
				httpn_free_session(httphandle->session);
				httphandle->session = NULL;
			}
		}
		/*
		 * res_options는 APP에서 넘어온 것이므로 여기서 프리하면
		 * 안됨.
		 */
		if (httphandle->res_options) {
			kv_destroy(httphandle->res_options);
		}
	}

#endif
	XFREE(handle);


	
	return r;
}

static int
httpn_ioctl(cfs_origin_driver_t *drv, int cmd, void *val, int vallen)
{
	int 		result = -1;
	char 		tl_encoding[128];
	char 		ts_encoding[128];

	
	httpn_driver_info_t *driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	if (!driverX) {
		httpn_set_errno(EINVAL);
		TRACE((T_ERROR, "no bound context, should be bound to context before\n"));
		
		return -1;
	}
	switch (cmd) {
		case NC_IOCTL_SET_IO_VALIDATOR:
			driverX->io_validator_proc = (cfs_io_validator_callback_t)val;
			TRACE((T_PLUGIN,  "driver[%s] - io validator set to %p\n",
						drv->name, driverX->io_validator_proc));
			result = 0;
			break;
		case NC_IOCTL_SET_LAZY_PROPERTY_UPDATE:
			driverX->io_update_property_proc = (cfs_io_property_updator_t)val;
			TRACE((T_PLUGIN,  "driver[%s] - io lazy property updator set to %p\n",
						drv->name, driverX->io_update_property_proc));
				result = 0;
			break;
		case NC_IOCTL_HTTP_FILTER: {
					if (driverX->filter) {
						XFREE(driverX->filter);
						driverX->filter = NULL;
					}
					driverX->filter = XSTRDUP(val, AI_DRIVER);
					TRACE((T_PLUGIN,  "driver[%s] - '%s' added as filter string\n", drv->name, driverX->filter));
				}
				result = 0;
				break;
		case NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER:
				driverX->on_receive_response_proc = (nc_origin_phase_handler_t)val;
				result = 0;
				break;
		case NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER:
				driverX->on_send_request_proc = (nc_origin_phase_handler_t)val;
				result = 0;
				break;
		case NC_IOCTL_SET_ORIGIN_PHASE_RES_CBDATA:
				driverX->on_receive_response_cbdata = (void *)val;
				TRACE((0, "DRIVER[%s] - NC_IOCTL_SET_ORIGIN_PHASE_RES_CBDATA: value=%p\n",  drv->signature, (void *)val));
				result = 0;
				break;
		case NC_IOCTL_SET_ORIGIN_PHASE_REQ_CBDATA:
				driverX->on_send_request_cbdata 	= (void *)val;
				TRACE((0, "DRIVER[%s] - NC_IOCTL_SET_ORIGIN_PHASE_REQ_CBDATA: value=%p\n",  drv->signature, (void *)val));
				result = 0;
				break;
		case NC_IOCTL_WEBDAV_ALWAYS_LOWER:
				driverX->always_lower = (*(int *)val); 
				TRACE((T_PLUGIN,  "driver[%s] - char conversion to lower one %s\n",
								drv->name, 
								(driverX->always_lower?"enabled":"disabled")));
				result = 0;
				break;
		case NC_IOCTL_LOCAL_CHARSET: {
				strncpy(tl_encoding, (char *)val, sizeof(tl_encoding));
				if (strcmp(tl_encoding, drv->l_encoding) != 0) {
					strcpy(ts_encoding, (char *)drv->s_encoding);
					httpn_init_encoding(drv, tl_encoding, ts_encoding);
					result = 0;
					if (driverX->from_server_enc == (iconv_t)-1 || driverX->to_server_enc == (iconv_t)-1) {
						TRACE((T_ERROR,  "driver[%s] - storage charset set to '%s', conversion disabled!\n", drv->name, drv->s_encoding));
						result = -1;
					}
					TRACE((T_PLUGIN,  "driver[%s] - local char-set changed to '%s', encoding map between (L:S) = ('%s':'%s')(%p:%p)\n", 
							drv->name, 
							tl_encoding,
							drv->l_encoding, 
							drv->s_encoding, 
							(void *)driverX->from_server_enc, 
							(void *)driverX->to_server_enc));
				}
				result = 0;
			}
			break;
		case NC_IOCTL_STORAGE_CHARSET: {
					strncpy(ts_encoding, (char *)val, sizeof(ts_encoding));
					if (strcmp(ts_encoding, drv->s_encoding) != 0) {
	
						strncpy(tl_encoding, (char *)drv->l_encoding, sizeof(tl_encoding));
						httpn_init_encoding(drv, tl_encoding, ts_encoding);
						result = 0;
						if (driverX->from_server_enc == (iconv_t)-1 || driverX->to_server_enc == (iconv_t)-1) {
							TRACE((T_ERROR,  "driver[%s] - storage charset set to '%s', conversion disabled!\n", drv->name, drv->s_encoding));
							result = -1;
						}
						TRACE((T_PLUGIN,  "driver[%s] - storge charset changed to '%s', encoding map between (L:S) = ('%s':'%s')(%p:%p)\n", 
								drv->name, 
								ts_encoding,
								drv->l_encoding, 
								drv->s_encoding, 
								(void *)driverX->from_server_enc, 
								(void *)driverX->to_server_enc));
		
					}
					result = 0;
				}
			break;
		case NC_IOCTL_STORAGE_PROBE_INTERVAL:
			if ((*(int *)val) <= 5) 
				result = -EINVAL;
			else {
				driverX->probe_interval = (*(int *)val); 
				TRACE((T_PLUGIN,  "driver[%s] - probing interval set to %d secs\n",
								drv->name, driverX->probe_interval));
				driverX->probe_interval = driverX->probe_interval *1000; /* sec to msec */
				result = 0;
			}
			break;
		case NC_IOCTL_FOLLOW_REDIRECTION:
			driverX->follow_redirection		= (*(int *)val) != 0;
			TRACE((T_PLUGIN, "driver[%s] - redirection followup %s.\n",
						drv->name,
						(driverX->follow_redirection?"enabled":"disabled")));
			result = 0;
			break;
		case NC_IOCTL_STORAGE_PROBE_COUNT:
			if (((*(int *)val) <= 0)  ||
				((*(int *)val) > 50))  {
				result = -EINVAL;
			}
			else {
				driverX->probe_count =  *(int *)val;
				TRACE((T_PLUGIN,  "driver[%s] - probing count set to %d secs\n",
								drv->name, driverX->probe_count));
				result = 0;
			}
			break;
		case NC_IOCTL_STORAGE_TIMEOUT: 
		case NC_IOCTL_CONNECT_TIMEOUT: {
				driverX->timeout = *(int *)(val);
				TRACE((T_PLUGIN,  "driver[%s] - connection timeout set to %d secs\n",
						drv->name, driverX->timeout));
				result = 0;
			}
			break;
		case NC_IOCTL_TRANSFER_TIMEOUT: {
				/* CHG 2018-10-22 huibong result 값 설정 및 잘못된 TRACE 구문 출력 수정 (#32395) */
				driverX->xfer_timeout = *(int *)(val);
				TRACE((T_PLUGIN,  "driver[%s] - transfer timeout set to %d secs\n",
						drv->name, driverX->xfer_timeout));
				result = 0;
			}
			break;
		case NC_IOCTL_SET_DNS_CACHE_TIMEOUT: {
				// NEW 2020-01-22 huibong DNS cache timeout 설정 기능 추가 (#32867)
				//                - curl 상의 dns cache timeout 관련 설정 (sec)
				driverX->dns_cache_timeout = *(int *)(val);
				TRACE((T_PLUGIN,  "driver[%s] - dns cache timeout set to %d sec.\n", drv->name, driverX->dns_cache_timeout ));
				result = 0;
			}
			break;
		case NC_IOCTL_STORAGE_RETRY: {
				driverX->max_tries = *(int *)(val);
				TRACE((T_PLUGIN,  "driver[%s] - storage max retries set to %d \n",
						drv->name, driverX->max_tries));
				result = 0;
			}
			break;
		case NC_IOCTL_PROXY: {
				/* host:port */
				char 	*tval = XSTRDUP(val, AI_DRIVER);
				char 	*sep = NULL;
				sep = strchr((char *)tval, ':');
				if (sep) {
					*sep = 0;
					sep++; /* port에 대한 포인터 */
				}
				driverX->proxy 		= XSTRDUP(tval, AI_DRIVER);
				driverX->proxy_port 	= 80;
				if (sep) {
					driverX->proxy_port = atoi(sep);
				}
				TRACE((T_PLUGIN, "driver[%s] - got a proxy, '%s:%d'\n", drv->name, driverX->proxy, driverX->proxy_port));
				XFREE(tval);
				result = 0;
			}
			break;
		case NC_IOCTL_ORIGIN_MONITOR:
			driverX->origin_monitor_proc = (nc_origin_monitor_t )val;
			result = 0;
			break;
		case NC_IOCTL_ORIGIN_MONITOR2:
			driverX->origin_monitor2_proc = (nc_origin_monitor2_t )val;
			result = 0;
			break;
		case NC_IOCTL_ORIGIN_MONITOR_CBD:
			driverX->origin_monitor_cbd = (void *)val;
			TRACE((0, "DRIVER[%s] - NC_IOCTL_ORIGIN_MONITOR: value=%p\n",  drv->signature, (void *)val));
			result = 0;
			break;
		case NC_IOCTL_USE_HEAD_FOR_ATTR:
			driverX->useheadforattr = *(int *)val;
			if (driverX->useheadforattr) {
				TRACE((T_PLUGIN, "driver[%s] - will use HEAD method to obtain object's property\n", drv->name));
			}
			result = 0;
			break;
		case NC_IOCTL_UPDATE_PARENT:
			_nl_lock(&driverX->lock, __FILE__, __LINE__);
			result = httpn_rebind_context(drv, NULL, (nc_origin_info_t *) val, vallen, NC_CT_PARENT);
			_nl_unlock(&driverX->lock);
			break;
		case NC_IOCTL_UPDATE_ORIGIN:
			_nl_lock(&driverX->lock, __FILE__, __LINE__);
			result = httpn_rebind_context(drv, NULL, (nc_origin_info_t *) val, vallen, NC_CT_ORIGIN);
			_nl_unlock(&driverX->lock);
			break;
		case NC_IOCTL_SET_LB_PARENT_POLICY:
			DEBUG_ASSERT(driverX->LB[NC_CT_PARENT] != NULL);
			_nl_lock(&driverX->lock, __FILE__, __LINE__);
			lb_set_policy(driverX->LB[NC_CT_PARENT], (nc_policy_type_t)*(nc_uint64_t *)val);
			_nl_unlock(&driverX->lock);
			result = 0;
			break;
		case NC_IOCTL_SET_LB_ORIGIN_POLICY:
			DEBUG_ASSERT(driverX->LB[NC_CT_ORIGIN] != NULL);
			_nl_lock(&driverX->lock, __FILE__, __LINE__);
			lb_set_policy(driverX->LB[NC_CT_ORIGIN], (nc_policy_type_t)*(nc_uint64_t *)val);
			_nl_unlock(&driverX->lock);
			result = 0;
			break;

			// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
			// - 전달받은 값을 origin, parent 모두에 설정 처리한다.
		case NC_IOCTL_SET_LB_FAIL_COUNT_TO_OFFLINE:
			_nl_lock(&driverX->lock, __FILE__, __LINE__);
			if( driverX->LB[NC_CT_ORIGIN] != NULL )
				driverX->LB[NC_CT_ORIGIN]->request_fail_count_to_offline = *(int *)val;
			if( driverX->LB[NC_CT_PARENT] != NULL )
				driverX->LB[NC_CT_PARENT]->request_fail_count_to_offline = *(int *)val;
			_nl_unlock(&driverX->lock);
			result = 0;
			break;

		case NC_IOCTL_HTTPS_SECURE:
			driverX->opt_https_secure = (*(int *)val? U_TRUE:U_FALSE);
			break;
		case NC_IOCTL_HTTPS_CERT:
			driverX->opt_https_cert = XSTRDUP((char *)val, AI_DRIVER);
			break;
		case NC_IOCTL_HTTPS_CERT_TYPE:
			driverX->opt_https_cert_type = XSTRDUP((char *)val, AI_DRIVER);
			break;
		case NC_IOCTL_HTTPS_SSLKEY:
			driverX->opt_https_sslkey = XSTRDUP((char *)val, AI_DRIVER);
			break;
		case NC_IOCTL_HTTPS_SSLKEY_TYPE:
			driverX->opt_https_sslkey_type = XSTRDUP((char *)val, AI_DRIVER);
			break;
		case NC_IOCTL_HTTPS_CRLFILE:
			driverX->opt_https_crlfile = XSTRDUP((char *)val, AI_DRIVER);
			break;
		case NC_IOCTL_HTTPS_CAINFO:
			driverX->opt_https_cainfo = XSTRDUP((char *)val, AI_DRIVER);
			break;
		case NC_IOCTL_HTTPS_CAPATH:
			driverX->opt_https_capath = XSTRDUP((char *)val, AI_DRIVER);
			break;
		case NC_IOCTL_HTTPS_FALSESTART:
			driverX->opt_https_falsestart = (*(int *)val? U_TRUE:U_FALSE);
			break;
		case NC_IOCTL_HTTPS_TLSVERSION: {
				char *sp = (char *)val;
				result =  0;
				if (strcmp(sp, "1.0") == 0) {
					driverX->opt_https_tlsversion = CURL_SSLVERSION_TLSv1_0;
				}
				else if (strcmp(sp, "1.1") == 0) {
					driverX->opt_https_tlsversion = CURL_SSLVERSION_TLSv1_1;
				}
				else if (strcmp(sp, "1.2") == 0) {
					driverX->opt_https_tlsversion = CURL_SSLVERSION_TLSv1_2;
				}
#ifdef CURL_SSLVERSION_TLSv1_3
				else if (strcmp(sp, "1.3") == 0) {
					driverX->opt_https_tlsversion = CURL_SSLVERSION_TLSv1_3;
				}
#endif
				else {
					TRACE((T_INFO, "Specified TLS version '%s' : unrecognised\n", sp));
					result =  -1;
				}
			}
			break;
		case NC_IOCTL_SET_OFFLINE_POLICY_FUNCTION:
			driverX->origin_is_down = (nc_offline_policy_proc_t)val;
			TRACE((T_PLUGIN, "driver[%s] - OFFLINE_POLICY set to %p\n", drv->name, driverX->origin_is_down));
			break;
		case NC_IOCTL_SET_OFFLINE_POLICY_DATA:
			driverX->usercontext = (void *)val;
			break;
		case NC_IOCTL_SET_DEFAULT_READ_SIZE:
			driverX->max_read_size = *(int *)val;
			break;
		case NC_IOCTL_SET_PROBING_URL:
			httpn_update_probe_url_ALL(drv, val);
			break;
		default:
			TRACE((T_PLUGIN, "unknown command:%d, ignored\n", cmd));
			result = -EINVAL;
			break;
	}
	
	return result;
}
struct tag_stream_data_info {
	char 		*buf;
	nc_off_t 	cursor;
	nc_size_t 	remained;
} ;





PUBLIC_IF CALLSYNTAX cfs_origin_driver_t * 
load()
{
	cfs_origin_driver_t *drvclass = NULL;
	drvclass = (cfs_origin_driver_t *)XMALLOC(sizeof(cfs_origin_driver_t), AI_DRIVER);
	memcpy(drvclass, &__httpn_native, sizeof(cfs_origin_driver_t));
	TRACE((T_INFO, "NetCache Pluggable Driver, '%s' loaded\n", httpn__agent));
	TRACE((T_INFO, "This driver is using the following libraries in form of shared or static library\n"
				   "\t\t\t\t\t\t * Libcurl are licensed under a MIT/X derivate license\n"
				   "\t\t\t\t\t\t * OpenSSL is Copyright (C) 1998-2011 The OpenSSL Project. All rights reserved.\n"
				   "\t\t\t\t\t\t * OpenSSL is Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)\n"));
	httpn_init_once();
	return drvclass;
}
static void
httpn_curl_version_print(void)
{
	char 				features[256];
	char 				*pout = features;
	int 				remained = sizeof(features);
	int 				n;
#define 	CHECK_BIT(f_, v_)  if (((f_) & (v_)) != 0) { \
		if (pout != features)  { \
			n = snprintf(pout, remained, ","); \
			pout++; \
			remained--; \
		} \
		n = snprintf(pout, remained, "%s", "F." #v_); \
		pout += n; \
		remained -= n; \
		*pout = 0; \
	}

	struct curl_version_info_short {
		CURLversion 	age;
		const char 		*version;
		unsigned int 	version_num;
		const 			char *host;
		int 			features;
		char 			*ssl_version;
		long 			ssl_version_num;
		const char 		*libz_version;
		const char 		**protocols;
		/* age 0 */
		/* other fields intentionally ignored */
	} *vd;
	vd = (struct curl_version_info_short *)curl_version_info(CURLVERSION_NOW);

	TRACE((T_INFO, "libcurl version : '%s'\n", vd->version));

#ifdef CURL_VERSION_IPV6
	CHECK_BIT(vd->features, CURL_VERSION_IPV6);
#endif
#ifdef CURL_VERSION_KERBEROS4
	CHECK_BIT(vd->features, CURL_VERSION_KERBEROS4);
#endif
#ifdef CURL_VERSION_SSL
	CHECK_BIT(vd->features, CURL_VERSION_SSL);
#endif
#ifdef CURL_VERSION_LIBZ
	CHECK_BIT(vd->features, CURL_VERSION_LIBZ);
#endif
#ifdef CURL_VERSION_NTLM
	CHECK_BIT(vd->features, CURL_VERSION_NTLM);
#endif
#ifdef CURL_VERSION_GSSNEGOTIATE
	CHECK_BIT(vd->features, CURL_VERSION_GSSNEGOTIATE);
#endif
#ifdef CURL_VERSION_DEBUG
	CHECK_BIT(vd->features, CURL_VERSION_DEBUG);
#endif
#ifdef CURL_VERSION_CURLDEBUG
	CHECK_BIT(vd->features, CURL_VERSION_CURLDEBUG);
#endif
#ifdef CURL_VERSION_ASYNCDNS
	CHECK_BIT(vd->features, CURL_VERSION_ASYNCDNS);
#endif
#ifdef CURL_VERSION_SPNEGO
	CHECK_BIT(vd->features, CURL_VERSION_SPNEGO);
#endif
#ifdef CURL_VERSION_LARGEFILE
	CHECK_BIT(vd->features, CURL_VERSION_LARGEFILE);
#endif
#ifdef CURL_VERSION_IDN
	CHECK_BIT(vd->features, CURL_VERSION_IDN);
#endif
#ifdef CURL_VERSION_SSPI
	CHECK_BIT(vd->features, CURL_VERSION_SSPI);
#endif
#ifdef CURL_VERSION_CONV
	CHECK_BIT(vd->features, CURL_VERSION_CONV);
#endif
#ifdef CURL_VERSION_TLSAUTH_SRP
	CHECK_BIT(vd->features, CURL_VERSION_TLSAUTH_SRP);
#endif
#ifdef CURL_VERSION_NTLM_WB
	CHECK_BIT(vd->features, CURL_VERSION_NTLM_WB);
#endif

	TRACE((T_INFO, "libcurl features: %s\n", features));
}
#if NOT_USED
static void
httpn_init_mod()
{
#if defined(S_ISUID) && defined(S_ISGID) && defined(MS_NOSUID)
	if (mopts & MS_NOSUID)
		dir_umask = S_ISUID | S_ISGID;
#endif

#if defined(MS_RDONLY)
	if (mopts & MS_RDONLY)
		dir_umask |= S_IWUSR | S_IWGRP | S_IWOTH;
#endif
	file_umask = dir_umask;

#if defined(MS_NOEXEC)
	if (mopts & MS_NOEXEC)
		file_umask |= S_IXUSR | S_IXGRP | S_IXOTH;
#endif

	mode_t 	default_mode = umask(0);
	umask(default_mode);

	default_mode = ~ default_mode;
	default_mode &= S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;

	default_dir_mode = default_mode;
#if defined(S_IRUSR)
	default_dir_mode |= (default_dir_mode & S_IRUSR) ? S_IXUSR : 0;
#endif
#if defined(S_IRGRP)
	default_dir_mode |= (default_dir_mode & S_IRGRP) ? S_IXGRP : 0;
#endif
#if defined(S_IROTH)
	default_dir_mode |= (default_dir_mode & S_IROTH) ? S_IXOTH : 0;
#endif
    default_dir_mode &= ~dir_umask;
    default_dir_mode |= S_IFDIR;

	default_file_mode = default_mode;
    default_file_mode &= ~file_umask;
    default_file_mode |= S_IFREG;


    httpn_init_locks();
}
#endif

#ifdef PROFILE_PERFORM_TIME
mavg_t						*g__perform;
#endif

static void
httpn_init_once()
{
	// CHG 2019-04-25 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용

	if (__curl_inited == 0) { 

#if defined(NC_MEM_DEBUG) && !defined(NC_HEAPTRACK)
		TRACE((T_INFO, "LIBCURL - memory allocatotor replaced\n"));
#pragma message(REMIND("replacing curl memory-callbacks"))
    	curl_global_init_mem(	CURL_GLOBAL_ALL, 
								httpn_curl_malloc, 
								httpn_curl_free,
								httpn_curl_realloc, 
								httpn_curl_strdup, 
								httpn_curl_calloc);
#endif
		__curl_inited = 1;
		httpn_curl_version_print();
	}

	{
		int		min_thr = max(4, (nr_cpu()/4));
		g__netpool = tp_init_nq("MPX", min_thr, 4*min_thr, &httpn_mpx_handler, &httpn_mpx_idle, &httpn_mpx_handler_prolog/*prolog*/, &httpn_mpx_handler_epilog/*epilog*/, (void *)NULL/*ctx*/, U_TRUE/*need queue*/);
#ifdef PROFILE_PERFORM_TIME
		g__perform = mavg_create("mpx_perform");
#endif
		pthread_create(&g__iocb_cleaner, NULL , httpn_iocb_cleaner, (void *)NULL);



	}


	DEBUG_ASSERT(g__netpool != NULL);
    httpn_init_locks();
	httpn_iocb_prepare_pool(256);
}

static httpn_session_pool_t * 
httpn_prepare_pool(cfs_origin_driver_t *drv, nc_origin_info_t *ctx, int ctxtype)
{
	int 				ret = 0;
	httpn_session_pool_t 	*new_pool; 
	lb_pool_t 		 	*lb_pool; 
	

	TRACE((T_DEBUG, "given context is \n"
					"\t\t\t - host : (%s)\n"
					"\t\t\t - prefix : (%s)\n"
					"\t\t\t - user : (%s)\n"
					"\t\t\t - password : (%s)\n",
					ctx->address,
					ctx->prefix,
					ctx->user,
					ctx->pass
					));


	new_pool 			= (httpn_session_pool_t *)XMALLOC(sizeof(httpn_session_pool_t), AI_DRIVER);
	lb_pool				= lb_create_pool(ctx->prefix, new_pool);
	new_pool->pooltype 	= ctxtype;
	new_pool->probe_url_utime = 0;
	new_pool->lb_pool 	= lb_pool;
	new_pool->cfc 		= 0;
	new_pool->uri 		= XSTRDUP(ctx->address, AI_DRIVER);
	new_pool->driver 	= (void *)drv;
	strncpy(new_pool->key, new_pool->uri, sizeof(new_pool->key)-1);
	strcpy(new_pool->probe_url, "/");


#if 1
	ret = httpn_setup_pool(new_pool, new_pool->uri, ctx->user, ctx->pass);
	if (ret < 0) {
		TRACE((T_INFO|T_PLUGIN, "pool[%s] - failed to authenticate\n", new_pool->key));
		httpn_destroy_pool(new_pool);
		
		return NULL;
	}
#endif



	INIT_LIST(&new_pool->probing_url);
	

	return new_pool;

}
static void
httpn_destroy_pool(httpn_session_pool_t *pool)
{

	if (pool) {
		XFREE(pool->storage_url);
		XFREE(pool->pass);
		XFREE(pool->user);
		XFREE(pool->uri);
		XFREE(pool); 
	}

}
static	int 
httpn_bind_context(cfs_origin_driver_t *drv, char *prefix, nc_origin_info_t *ctx, int ctxcnt, int ctxtype)
{
	void httpn_expire_session(void *d);
	int http_check_alive(lb_t *lb, lb_pool_t *pool, void *ud) ;
	int						i;
	httpn_session_pool_t 	*hsp = NULL;

	httpn_driver_info_t 	*driverX = NULL;
	
	driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);



	_nl_lock(&driverX->lock, __FILE__, __LINE__);

	driverX->LB[ctxtype] 			= lb_create(drv, &driverX->lock, httpn_expire_session, NULL, http_check_alive);
	driverX->pool[ctxtype] 			= (httpn_session_pool_t **)XCALLOC(ctxcnt, sizeof(httpn_session_pool_t *), AI_DRIVER);
	lb_set_monitor(driverX->LB[ctxtype], httpn_origin_monitor, (void *)drv);
	LB_REF(driverX->LB[ctxtype]);
	for (i = 0;i < ctxcnt; i++) {
		if ((hsp = httpn_prepare_pool(drv, &ctx[i], ctxtype)) == NULL) {
			TRACE((T_WARN, "driver instance [%s] : can not add context info: user(%s), pass(%s), path(%s)\n",
					drv->name, 
					ctx[i].user, 
					ctx[i].pass, 
					ctx[i].prefix));
			LB_UNREF(driverX->LB[ctxtype]);
			goto L_bind_error;
		}
		else {
			/*
			 * pool이 overflow 날거라고는 생각안함. 버그 아닌 경우에.
			 */
			driverX->pool[ctxtype][i] = hsp;
			if (lb_add_pool(driverX->LB[ctxtype], hsp->lb_pool, 0) >= 0) {
				lb_pool_set_online(driverX->LB[ctxtype], hsp->lb_pool, U_TRUE/*online*/, U_FALSE/*don't recover*/, U_FALSE);
				driverX->pool_cnt[ctxtype]++;
				TRACE((T_PLUGIN,  "driver[%s] - : pool# %d - ioflag, remote addr='%s'\n", drv->name, (int)driverX->pool_cnt[ctxtype], hsp->uri));
			}
			else {
				// CHG 2019-05-08 huibong 로깅 level 수정
				//               - 중복 등록된 origin 에 대해 무시 처리하므로...ERROR -> WARN 으로 log level 변경
				TRACE( (T_WARN, "driver[%s] - : pool# %d - ioflag, remote addr='%s' duplicate, ignored\n"
					, drv->name, (int)driverX->pool_cnt[ctxtype], hsp->uri) );
			}
		}
	}

	// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
	// - origin 등록 완료 후 후처리 작업 call 기능 추가
	lb_add_pool_complete( driverX->LB[ctxtype] );
	LB_UNREF(driverX->LB[ctxtype]);

L_bind_error:
	_nl_unlock(&driverX->lock);
	
	return driverX->pool_cnt[ctxtype];
}
/*
 * 2020-4-29 by weon@solbox.com
 *		cfs_driver에서 exclusive lock후 호출
 * 수정: operation을 실제 진행하기전에 대상 LB가 busy인지 확인, busy인 경우에는
 *       -EBUSY 리턴하도록 변경
 *  
 */
static	int 
httpn_rebind_context(cfs_origin_driver_t *drv, char *prefix, nc_origin_info_t *ctx, int ctxcnt, int ctxtype)
{
	int 					ret = 0;
	httpn_driver_info_t 	*driverX = NULL;
	static char				zctxtype[2][32] = {"ORIGIN", "PARENT"};


	/* CHG 2018-10-30 huibong conf reload 처리시 기존 등록된 parent 삭제만 하는 기능 추가 (#32410) 
	*                         - parent 정보를 conf 에서 삭제 후 reload 처리하면... 기존 parent 정보가 삭제되지 않고 그대로 유지됨.
	*                         - 이를 해결하기 위해 conf 에 parent 정보 유무에 상관없이 reload 처리시 parent 정보 갱신 요청 예정 (김현호)
	*                         - 이에 ctxcnt 가 0 인 경우.. unbind 만 처리하고...
	*                         - 그 외에는 기존과 동일하게 unbind -> bind 처리하도록 수정
	*                         - origin 인 경우.. 전체 삭제하는 상황은 운영자 실수 이므로.... 기존과 같이 오류처리.
	*/

	driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);


	if (drv->shutdown) {
		httpn_set_errno(EPERM);
		
		return -EPERM;
	}

	/*
	 * driver 형상 정보를 이용하는 모든 operation들에게
	 * 정보가 변경되었음을 알림
	 * 예) lb_set_online()등
	 */
	_ATOMIC_ADD(driverX->confver, 1);


	if( ctxcnt == 0 && ctxtype == NC_CT_PARENT )	// parent 정보 삭제 후 reload 처리 지원용
	{
		ret = httpn_unbind_context_i(drv, ctxtype);
	}
	else
	{

		ret = httpn_unbind_context_i(drv, ctxtype);
		if (ret == 0)
			ret = httpn_bind_context(drv, prefix, ctx, ctxcnt, ctxtype);
	}
	
	TRACE((T_INFO, "Driver[%s] - updating '%s' (cnt=%d), returns %d\n", drv->signature, zctxtype[ctxtype], ret, ctxcnt));
	return ret;
}



static	int 
httpn_unbind_context_i(cfs_origin_driver_t *drv, int ctxtype)
{
	int 					i;
	httpn_driver_info_t 	*driverX = NULL;
	static char					zctx[][32]={
			"ORIGIN",
			"PARENT"
	};

	TRACE((drv->trace|T_PLUGIN, "DRIVER[%s]/ref[%d] - unbinding zctx[%s]\n",drv->signature, drv->refcnt, zctx[ctxtype]));
	driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	/* CHG 2018-10-29 huibong #30586 에서 V28 에 적용된 내역을 V30 에도 적용 (#32410)
	*                 - parent 구성시 죽는 문제 확인 결과... #30586 에서 V28 에 수정 적용됨.
	*                 - 해당 내역을 V30 에서 적용 처리함.
	*/
	if (driverX) {
		_nl_lock(&driverX->lock, __FILE__, __LINE__);

		if( driverX->LB[ctxtype] )
		{
			httpn_put_defered_queue(drv, ctxtype);
			TRACE((drv->trace|T_PLUGIN, "DRIVER[%s]/ref[%d] -  zctx[%s].LB busy, scheduling defered free\n", drv->signature, LB_COUNT(driverX->LB[ctxtype]), zctx[ctxtype]));
			lb_signal_shutdown(driverX->LB[ctxtype]);
			driverX->LB[ctxtype] 		= NULL;
			driverX->pool[ctxtype] 		= NULL;
			driverX->pool_cnt[ctxtype] 	= 0;
		}
		_nl_unlock(&driverX->lock);
	}

	return 0;
}
static	int 
httpn_unbind_context(cfs_origin_driver_t *drv, int ctxtype)
{
	int		r;
	httpn_driver_info_t	*driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	_nl_lock(&driverX->lock, __FILE__, __LINE__);
	TRACE((T_PLUGIN, "DRIVER[%s] unbinding...(refcnt=%d, drv.shutdown=%d)\n", drv->signature, drv->refcnt, drv->shutdown));
	r = httpn_unbind_context_i(drv, ctxtype);

	TRACE((T_PLUGIN, "driver[%s] unbinding...(refcnt=%d, drv.shutdown=%d) done(res=%d)\n", drv->signature, drv->refcnt, drv->shutdown, r));
	_nl_unlock(&driverX->lock);

	return r;
}
static link_list_t			s__defered_free_queue 		= LIST_INITIALIZER;
static int 					s__defered_free_init  	= 0;
static pthread_mutex_t		s__defered_free_queue_lock 	= PTHREAD_MUTEX_INITIALIZER;
typedef struct {
	nc_uint32_t				magic;
	cfs_origin_driver_t 	*driver;
	int						ctxtype;
	lb_t					*LB;
	httpn_session_pool_t    **pool;
	int    					pool_cnt;

	nc_time_t				registered;
	link_node_t				node;
} httpn_defered_free_t;

static void
httpn_put_defered_queue(cfs_origin_driver_t *drv, int ctxtype)
{
	httpn_defered_free_t	*j = NULL;
	httpn_driver_info_t		*driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);


	lb_signal_shutdown(driverX->LB[ctxtype]);

	pthread_mutex_lock(&s__defered_free_queue_lock);

	j = (httpn_defered_free_t *)XMALLOC(sizeof(httpn_defered_free_t), AI_DRIVER);

	j->magic		= 0x5aa5a55a;
	j->driver 		= drv;
	j->ctxtype 		= ctxtype;
	j->LB 			= driverX->LB[ctxtype];
	j->pool 		= driverX->pool[ctxtype];
	j->pool_cnt 	= driverX->pool_cnt[ctxtype];
	j->registered 	= nc_cached_clock();


	link_append(&s__defered_free_queue, j, &j->node);
	pthread_mutex_unlock(&s__defered_free_queue_lock);
	TRACE((T_PLUGIN, "!-!-!-!-!-!-!-!-!-!-!-!-! DRIVER[%p].LB[%p] - scheduled to free[refcnt=%d]\n",
					j->driver,
					j->LB,
					LB_COUNT(j->LB)
					));
}
int
httpn_handle_defered_queue(cfs_origin_driver_t *drv, int force)
{

	httpn_defered_free_t	*j = NULL;
	httpn_defered_free_t	*next_j = NULL;
	httpn_driver_info_t 	*driverX = NULL;
	int						i, n;
	perf_val_t				s,e;
	long 					d;
	int						scanned = 0;
	int						match_to = 0, match_drv = 0;
#define		DEFER_TIME		600

	n = 0;


	PROFILE_CHECK(s);
	pthread_mutex_lock(&s__defered_free_queue_lock);
L_retry_queue:
	j = (httpn_defered_free_t *)link_get_head_noremove(&s__defered_free_queue);

	/*
	 * 진입조건
	 * 	C#1 j && j->driver == drv  
	 * 	C#2 j && force = TRUE
	 * 	C#2 j && (now - j->registered) > DEFER_TIME
	 */
	match_to  = (j? ((nc_cached_clock() - j->registered) > DEFER_TIME):0);

	while (j && (force ||  drv || match_to)) {
#ifndef NC_RELEASE_BUILD
		DEBUG_ASSERT(nc_check_memory(j, 0));
		if (!nc_check_memory(j->driver, 0)) {
			TRACE((T_ERROR, "DRIVER[%p] - invalid memory\n", j->driver));
			TRAP;
		}
		if (!nc_check_memory(j->LB, 0)) {
			TRACE((T_ERROR, "DRIVER[%p].LB[%p] - invalid memory\n", j->driver, j->LB));
			TRAP;
		}
#endif
		match_drv = (j?(drv && (j->driver == drv)):0);


		driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(j->driver);
		next_j 	= link_get_next(&s__defered_free_queue, &j->node);

		/*
		 * 아래 조건은 destroy_instance()실행 중에 호출인지
		 * 확인하는 것으로
		 * iocb cleaner thread에서 주기적인 호출 시에는 해당되지 않음
		 */
		if (drv && match_drv == 0) {
			j = next_j;	
			continue;
		}


		driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(j->driver);

		_nl_lock(&driverX->lock, __FILE__, __LINE__);
		TRACE((T_PLUGIN, "DRIVER[%p].LB[%p] - going to free (force=%d, match_drv=%d,match_to=%d)\n", j->driver, j->LB, force, match_drv, match_to));
		if (!force && !LB_IDLE(j->LB)) {
			/*
			 * 종료단계는 아니지만, LB가 여전히 idle이 아님
			 * 이 LB는 남겨두고 나음 LB로 진행하도록 함
			 */
			TRACE((T_ERROR, "!+!+!+!+!+!+!!+!+!+!+!+!!+!+!+!+!!+!+DRIVER[%s].LB[%p] - still busy[refcnt=%d, shutdown=%d, timer.run=%d, timer.set=%d](force=%d),retrying\n",
                        	j->driver->signature,
	                        j->LB,
							LB_COUNT(j->LB),
							j->LB->shutdown,
							j->LB->timer_run,
							j->LB->timer_set,
							force));
			j = next_j;	
			_nl_unlock(&driverX->lock);
			bt_msleep(500);
			goto L_retry_queue;
		}

		/*
		 * free할 수 있는 LB임, defered queue에서 제거
		 */
		link_del(&s__defered_free_queue, &j->node);

		
		lb_destroy(j->LB);
		for (i = 0; i < j->pool_cnt; i++) {
			httpn_destroy_pool(j->pool[i]);
		}
		_nl_unlock(&driverX->lock);

		XFREE(j->pool);
		XFREE(j);
		n++;

		j = next_j;
	} 
	PROFILE_CHECK(e);
	d = PROFILE_GAP_MSEC(s,e);
	TRACE((T_PLUGIN, "%d LBs left in the defered-free queue(%d handled, %.2f sec)\n", link_count(&s__defered_free_queue, U_TRUE), n, (1.0 *d)/1000.0));
	pthread_mutex_unlock(&s__defered_free_queue_lock);

	return n;

}

#if NOTUSED
static void
httpn_tp_monitor(void *u)
{
		int	 config;
		int	 running;
		int	 qlen;

		config 	= tp_get_configured(g__netpool);
		running = tp_get_workers(g__netpool);
		qlen 	= tp_length(g__netpool);
		TRACE((T_PLUGIN, "NETPOOL: %d/%d running (queue=%d)\n", running, config, qlen));
		bt_set_timer( __timer_wheel_base, &__timer_am, 10 * 1000 /*10 sec*/, httpn_tp_monitor, NULL );
}
#endif

/*
 * 이 기능을 타이머로 밖에 구현하기 힘든 이유
 * 	health-check기능이 동기식으로 돌고, network 환경에 따라 오래 시간 걸릴 수 있음
 *  이걸 하나의 thread에서 순차적으로 점검한다는것은 얼마나 시간 오래걸릴지 예측 불가
 *  그러므로 timer를 통해서 주기적으로 기동하며 timer에서 실행시 독립 thread생성후 거기서
 * 	실행하는 방식으로 함
 */
static void 
httpn_healthcheck_monitor(void *u)
{
	int					i = 0;
	int					scanned = 0;
	lb_t				*lb;
	cfs_origin_driver_t	*drv = u;
	httpn_driver_info_t	*driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	if (driverX->shutdown) {
		goto L_heahcheck_finish;
	}

	_nl_lock(&driverX->lock, __FILE__, __LINE__);
	for (i = 0; i < 2 && drv->shutdown == 0 && driverX->shutdown == 0; i++) {
		/*
		 * driverX->LB[i]에 대한 참조부터 lock획득 후 실행
		 */
		lb = driverX->LB[i];
		if( lb )  {
			if (lb_shutdown_signalled(lb)) {
				driverX->LB[i] = NULL;
				continue; /* repeat with the next LB */
			}

			scanned++;
			LB_REF(lb);

			_nl_unlock(&driverX->lock);
			DEBUG_ASSERT(_nl_owned(&driverX->lock) == 0);
			//
			// healthy_nolock()에서 시간 걸리므로 lock해제
			//
			if (!lb_healthy_nolock(lb)) {
				TRACE((T_PLUGIN, "timer[%p] - lb[%p] probing\n", &driverX->t_healthcheck, lb));
				lb_probe_pools(lb); /* driverX->lock획득상태에서 호출*/
			}
			LB_UNREF(lb);



			_nl_lock(&driverX->lock, __FILE__, __LINE__);
			DEBUG_ASSERT(_nl_owned(&driverX->lock) != 0);
		}
	}
	DEBUG_ASSERT(_nl_owned(&driverX->lock) != 0);
	_nl_unlock(&driverX->lock);
	DEBUG_ASSERT(_nl_owned(&driverX->lock) == 0);
L_heahcheck_finish:

	if (drv->shutdown == 0 && driverX->shutdown == 0) 
		bt_set_timer( __timer_wheel_base, &driverX->t_healthcheck, 2000 /*2 sec*/, httpn_healthcheck_monitor, u );
	else
		TRACE((T_PLUGIN, "driver[%s] - healthcheck(%d) shutdown found\n", drv->signature, scanned));

}
static	cfs_origin_driver_t * 
httpn_create_instance(char *signature, 
					cfs_update_hint_callback_t sc_update_hint_proc, 
					cfs_update_result_callback_t sc_update_result_proc)
{
	cfs_origin_driver_t *driver = NULL;
	httpn_driver_info_t	*driverX = NULL;
	pthread_mutexattr_t		mattr;
	char					tname[256];


	// CHG 2019-04-25 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	static int			_am = 0;


#ifndef  NC_RELEASE_BUILD
	if (_ATOMIC_ADD(__running, 1) == 1) {
		/*
		 *  first run
		 */
		bt_init_timer(&__timer_am, "activity monitor", U_FALSE);
		bt_set_timer( __timer_wheel_base, &__timer_am, 10 * 1000 /*10 sec*/, httpn_activity_monitor, NULL );
	}

#endif
	driver = (cfs_origin_driver_t *)XMALLOC(sizeof(cfs_origin_driver_t), AI_DRIVER);



	memcpy(driver, &__httpn_native, sizeof(cfs_origin_driver_t));
	if (signature) {
		snprintf(	driver->name, 
					sizeof(driver->name), 
					"#%d[%s]", 
					__sync_fetch_and_add(&__driver_instance_id, 1),
					signature);
		strcpy(driver->signature, signature);
	}
	else {
		snprintf(driver->name, 
					sizeof(driver->name), 
					"%s #%d", 
					DRIVER_NAME, 
					__sync_fetch_and_add(&__driver_instance_id, 1));

		strcpy(driver->signature, driver->name);
	}

	driver->refcnt 		= 0;
	driver->hint_proc 	= sc_update_hint_proc;
	driver->result_proc = sc_update_result_proc;


	driverX = (httpn_driver_info_t *)XMALLOC(sizeof(httpn_driver_info_t), AI_DRIVER);
	strcpy(driver->s_encoding, "UTF-8");
	driver->shutdown			= 0;
   	driverX->from_server_enc= (iconv_t)-1;
   	driverX->to_server_enc 	= (iconv_t)-1;
	driverX->always_lower 	= 0;
	driverX->confver 		= 0;
	driverX->probe_interval = 10*1000;
	driverX->probe_count 	= 2;
	driverX->max_read_size	= 1024*1024;

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
	_nl_init(&driverX->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);

	/* CHG 2018-10-25 huibong transfer timeout 처리 방식 결정에 따른 default 값 조종 (#32401) */
	driverX->timeout		= DEFAULT_ORIGIN_CONNECT_TIMEOUT;	/* default 5 sec */
	driverX->xfer_timeout	= DEFAULT_ORIGIN_TRANSFER_TIMEOUT;	/* transfer timeout sec */

	// NEW 2020-01-22 huibong DNS cache timeout 설정 기능 추가 (#32867)
	//                - curl 상의 dns cache timeout 관련 설정값 관련 default 값 적용
	driverX->dns_cache_timeout = DEFAULT_DNS_CACHE_TIMEOUT;

	driverX->filter			= XSTRDUP((char *)__default_filter, AI_DRIVER); 
	driverX->max_tries		= __max_read_tries;
	driverX->proxy			= NULL;
	driverX->proxy_port		= 0;
	driverX->pool[0]		= NULL;
	driverX->pool[1]		= NULL;
	driverX->follow_redirection		= 0;
	driverX->opt_use_https			= U_FALSE;/* default 0 */
	driverX->opt_https_secure 		= U_TRUE;  			/* default 1 */
	driverX->opt_https_falsestart 	= U_FALSE; 		/* default : FALSE */
	driverX->opt_https_tlsversion 	= CURL_SSLVERSION_TLSv1_1;
	driver->driver_data 			= (void *)driverX;
	snprintf(tname, sizeof(tname), "healthcheck.%s", signature);
	bt_init_timer(&driverX->t_healthcheck, tname, U_TRUE);
	bt_set_timer( __timer_wheel_base, &driverX->t_healthcheck, 2000 /*2 sec*/, httpn_healthcheck_monitor, driver );



	if (_am == 0) {
		TRACE((T_PLUGIN, "thread-pool starting...\n"));
		tp_start(g__netpool);

		_am++;
	}
#ifndef NC_RELEASE_BUILD
	TRACE((driver->trace|T_PLUGIN, "DRIVER[%s] created(refs=%d, addr=%p)\n", driver->signature, driver->refcnt, driver));
#endif
	return driver;
}

/*
 * cfs_driver에서 exclusive lock이후 호출
 */
static	int 
httpn_destroy_instance(cfs_origin_driver_t *driver)
{
	int 		i;
	int 		r;
	/*
	 * destroy driver instance
	 * 	- relinquish allocated memories
	 */
	httpn_driver_info_t	*driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(driver);
	

	driverX->shutdown = 1;
	TRACE((driver->trace|T_PLUGIN, "DRIVER[%s] destroying(refs=%d, addr=%p)\n", driver->signature, driver->refcnt, driver));

	_nl_lock(&driverX->lock, __FILE__, __LINE__);
	while (bt_del_timer_v2(__timer_wheel_base,  &driverX->t_healthcheck) < 0)  {
		_nl_unlock(&driverX->lock);
		bt_msleep(500);
		_nl_lock(&driverX->lock, __FILE__, __LINE__);
	}
	for (i = 0; i < 2; i++) {
		if (driverX->pool[i]) {
			r = httpn_unbind_context_i(driver, i);

			if (r == -EBUSY) return r;

			driverX->pool[i] = NULL;
		}
	}
	i = httpn_handle_defered_queue(driver, U_FALSE);
	TRACE((T_PLUGIN, "DRIVER[%s] - %d LB freed\n", driver->signature, i));
	_nl_unlock(&driverX->lock);

	DEBUG_ASSERT(CFS_DRIVER_IDLE(driver));

	driverX->confver 	= (nc_uint32_t)-1; /* mark removed */
	driver->driver_data	= NULL;
	driver->magic		= 0;
	XFREE(driverX->filter);
	XFREE(driverX);
	XFREE(driver);


#if 0
	if (__curl_inited) { 
		__curl_inited = 0;
    	curl_global_cleanup();
	}
#endif
	
	return 0;
}

static void
httpn_init_try(httpn_io_context_t *iocb, int allowretry)
{

	/*
	 * request 준비
	 *
	 */

	if (iocb->request == NULL) {
		iocb->request = httpn_request_create(iocb, iocb->zmethod, NULL, iocb->in_property);
		httpn_request_set_head_callback(iocb->request, httpn_head_dispatch, iocb);
		iocb->allow_retry 	= allowretry;
		iocb->tries 		= 0;
		TRACE((T_PLUGIN, "IOCB[%u] request created\n", iocb->id));

	}
}
int
httpn_prepare_try(httpn_io_context_t *iocb)
{
	cfs_off_t		range_begin, range_end;
	char			byte_range[256];
	int				r;



	iocb->tries++;	/* 생성 후 0임, 첫 시도는 1부터 시작*/
	iocb->reported = 0;

	DEBUG_ASSERT_IOCB(iocb, iocb->mpx == g__current_mpx);
	if (!http_retry_allowed(iocb)) {
		TRACE((iocb->tflg|T_PLUGIN, "%d/%d:: URL[%s].IOCB[%u] did our best, gave up\n",
							iocb->tries,
							iocb->driverX->max_tries,
							iocb->wpath, 
							iocb->id
			  ));
		return -1;
	}
	iocb->pool 		= httpn_next_pool(iocb->driver, iocb->wpath, U_TRUE);
	if (!iocb->pool) {
		TRACE((iocb->tflg|T_PLUGIN, "%d/%d:: URL[%s].IOCB[%u] %d-th attempt found no ONLINE pool\n",
							iocb->tries,
							iocb->driverX->max_tries,
							iocb->wpath, 
							iocb->id,
							iocb->tries
			  ));
		return -1;
	}



	httpn_cleanup_request(iocb->request);

	httpn_reset_response(iocb);
	httpn_migrate_http_state(iocb, HS_INIT, __FILE__, __LINE__);



	if (iocb->tries > 1) {
		char		ziocb[512];
		TRACE((iocb->tflg|T_PLUGIN, "IOCB[%d] : preparing %d-th (context:%s)\n", iocb->id, iocb->tries, httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)));
		iocb->u.r.file_off_prev = iocb->u.r.file_off;
		iocb->u.r.file_off		= 0;
	}
	else {
		memset(&iocb->u.r, 0, sizeof(iocb->u.r));
	}

	if (iocb->owner == HTTP_API_GETATTR) { 
		if (iocb->assume_rangeable != 0 || iocb->rangeable != 0) { 


			/*
			 * 내부 회의 결과 0-0오로 고정
			 */
			if (iocb->knownsize == -1 || iocb->knownsize != 0) {
				/*
				 * 이미 캐싱되어있고 size=0이면 예외
				 */
				iocb->rangeop = 1;
				snprintf(byte_range, sizeof(byte_range), "bytes=0-0");
				httpn_set_readinfo(iocb, 0, 1);
				httpn_request_add_header(iocb->request, "Range", byte_range, U_TRUE);
			}
		}
	}
	else {

		iocb->vector->iov_xfer_bytes = 0;
		if (asio_is_command_type(iocb->vector, 0, IOT_OSD_READ_ENTIRE)) {
			httpn_set_readinfo(iocb, 0, -1);
		}
		else if (asio_is_command_type(iocb->vector, 0, IOT_OSD_READ)) {
			/* 
			 * 현재까지 읽은 내용 정리
			 */
			iocb->vector->iov_xfer_bytes = iocb->accepted 	= iocb->u.r.blk_cursor * NC_BLOCK_SIZE;

			/*
			 * 새로 읽기 요청할 준비
			 */
			httpn_make_range(iocb->vector, U_FALSE, iocb->u.r.blk_cursor, &range_begin, &range_end);
			iocb->u.r.file_off 	= range_begin;
			iocb->u.r.blk_off 	= 0;
			iocb->rangeop 		= 1;
			snprintf(byte_range, sizeof(byte_range), "bytes=%lld-%lld", range_begin, range_end);

			httpn_request_add_header(iocb->request, "Range", byte_range, U_TRUE);
			httpn_set_readinfo(iocb, range_begin, range_end - range_begin +1);
			iocb->u.r.total = (range_end - range_begin + 1);
			TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u].request range[%s] prepared\n", iocb->id, byte_range));
		}
	}

	/*
	 * prepare the next startup
	 */


	/* 2018-08-20 huibong Origin Request callback 함수의 추가된 request header 반영 오류 수정 (#32301) */
	/* httpn_commit_headers(iocb->request); */

	iocb->request->url = httpn_setup_url(iocb->pool, iocb->pool->storage_url, iocb->wpath);

	iocb->session = httpn_prepare_session(iocb->driver, iocb, iocb->pool);
   	CHK_OPT(r, curl_multi_add_handle(iocb->mpx->mif, iocb->session->eif));

	httpn_bind_request_to_session(iocb->driver, iocb->session, iocb->request);


	return 0;
}
void
httpn_cleanup_try(httpn_io_context_t *iocb)
{
	if (iocb->autocreat) {
		httpn_free_iocb(iocb);
	}
	else 
		TRACE((T_PLUGIN, "IOCB[%d] - went to idle, waitint for free\n", iocb->id));
}
int
httpn_forcely_down(httpn_driver_info_t *driverX, char *url, int curle, int httpcode)
{
	int 	fd = 0;

	if (curle == CURLE_OK && driverX->origin_is_down) {
		fd = (*driverX->origin_is_down)(url, httpcode, driverX->usercontext);
	}
	else {
		fd = 	(curle == CURLE_COULDNT_RESOLVE_PROXY) ||
				(curle == CURLE_COULDNT_RESOLVE_PROXY) ||
				(curle == CURLE_COULDNT_CONNECT) ||
				(curle == CURLE_OPERATION_TIMEDOUT) ||
				(curle == CURLE_SSL_CONNECT_ERROR) ||
				(curle == CURLE_SEND_ERROR) ||
				(curle == CURLE_RECV_ERROR) ||
				(httpcode >= 500);
	}
	return fd;
}

/*
 *
 * iocb->last_curle & iocb->last_httpcode is the last recent values
 *
 */
int 
httpn_handle_try_result(httpn_io_context_t *iocb)
{
	httpn_session_pool_t 	*old_pool = NULL;
	httpn_session_pool_t 	*new_pool = NULL;
	int						trycode = HTTP_TRY_CONTINUE;
	int						need_force_down = U_FALSE;
	int						derror = 0;
	int						r;
	char					dbuf[2048];
	static char				zdtc[][32] = {
		"HTTPDE_OK",
		"HTTPDE_RETRY",
		"HTTPDE_CRITICAL",
		"HTTPDE_USER"
	};
	int						tflg = 0;

	/*
	 * ***************************************************************
	 * result processing : 실행 결과의 최종 처리
	 * ***************************************************************
	 */
	
	iocb->last_errno = httpn_handle_error(iocb, &iocb->last_curle, &iocb->last_httpcode, &derror);



	httpn_set_raw_code_ifpossible(iocb, iocb->last_httpcode);

	httpn_set_status(iocb);
	if (iocb->session && iocb->last_httpcode > 0) {
		/*
		 * report할만상 상태
		 * last_httpcode = 0인 경우는 socket전송 도중이거나 강제로 close된 상태
		 */
		httpn_mpx_handle_done_iocb(iocb);
		httpn_report_origin_log(iocb);
	}
	TRACE((T_PLUGIN, "%d/%d: IOCB[%d] - last_curle=%d, last_http=%d, last_errno=%d(derror=%s), stat.st_origincode=%d\n",
				iocb->tries,
				iocb->driverX->max_tries,
				iocb->id,
				iocb->last_curle,
				iocb->last_httpcode,
				iocb->last_errno,
				zdtc[-derror],
				iocb->stat.st_origincode
				));


	old_pool = iocb->pool;

	if (iocb->tries > 1) tflg = T_INFO;

	switch (derror) {
		case HTTPDE_RETRY:

			need_force_down = httpn_forcely_down(iocb->driverX, iocb->request->url, iocb->last_curle, iocb->last_httpcode);
			trycode = HTTP_TRY_CONTINUE;
			if (httpn_valid_config(iocb->driver, iocb->confver)) {
				r = httpn_mark_down_pool_x(iocb->driver, iocb->pool, (char *)iocb->wpath, need_force_down);
			}
			break;
		case HTTPDE_CRITICAL:
			/*
			 * 재시도 없이 오리진 offline후 종료
			 */

			if (httpn_valid_config(iocb->driver, iocb->confver)) { 

				need_force_down = httpn_forcely_down(iocb->driverX, iocb->request->url, iocb->last_curle, iocb->last_httpcode);

				httpn_mark_down_pool_x(iocb->driver, iocb->pool, (char *)iocb->wpath, need_force_down);
				TRACE((T_INFO,"%d/%d DRIVER[%s].POOL[%s] - (curle=%d(%s), httpcode=%d, next=%s, stderrno=%d):goes OFFLINE=CRITICAL;%s\n",
									iocb->tries,
									iocb->driverX->max_tries,
									iocb->driver->name,
									(old_pool?old_pool->key:"NULL"),
									iocb->last_curle,
									curl_easy_strerror(iocb->last_curle),
									iocb->last_httpcode,
									zdtc[-derror],
									iocb->last_errno,
									httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)

					  ));
			}
			trycode = HTTP_TRY_STOP;
			break;

		case HTTPDE_USER:
			// CHG 2019-03-25 huibong origin 응답코드에 대해 운영자 설정에 따른 OFFLINE 처리 기능 개선 (#32565)
			//                - origin 으로 부터 응답은 수신했지만.... HTTP 500 이상의 응답인 경우
			//                - 운영자가 지정한 OFFLINE 조건에 해당하는지 check 를 위한 기능 추가
			//                - HTTPE_OK 와 동일한 정상, retry 필요 없는 상황이지만... 응답코드가 OFFLINE 처리해야 하는지만 check.
			trycode = HTTP_TRY_STOP;

			// 사용자 offline 처리 함수가 설정된 경우..
			need_force_down = httpn_forcely_down(iocb->driverX, iocb->request->url, iocb->last_curle, iocb->last_httpcode);

			// 강제로 OFFLINE 처리 해야 하는 경우..
			if( need_force_down != 0 )
			{
				if (httpn_valid_config(iocb->driver, iocb->confver)) {
					// origin offline 처리
					httpn_mark_down_pool_x( iocb->driver, iocb->pool, (char *)iocb->wpath, need_force_down );
				}
				break;
			}

			// 위의 사용자 강제 offline 상황이 아닌 경우... 아래의 정상 default 상황으로 처리되도록 
			// 일부러 break 구문을 주석 처리함.
			//break;

		default:
			/* 
			 * 정상 종료 코드
			 */
			trycode = HTTP_TRY_STOP;

			// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
			//     - origin 정상 응답인 경우.... online 상태 정보 update.
			if (httpn_valid_config(iocb->driver, iocb->confver)) {
				if (iocb->method == HTTP_GET && BETWEEN(100, 499, iocb->last_httpcode)) {
					httpn_update_probe_url(iocb->driver, iocb->pool, iocb->wpath, U_FALSE);
				}
				httpn_mark_ok_pool_x( iocb->driver, iocb->pool );
			}

			break;
	}

	//httpn_cleanup_session(iocb->session);
	TRACE((tflg | T_PLUGIN,"%d/%d IOCB[%u]:POOL[%s] - trial result (%s);%s\n",
							iocb->tries,
							iocb->driverX->max_tries,
							iocb->id,
							(old_pool?old_pool->key:"NULL"), 
							zdtc[-derror],
							httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)
				));

	httpn_mpx_unregister_nolock_self( iocb->mpx, iocb );
	httpn_release_session(iocb, iocb->session);
	iocb->session = NULL;


	if (old_pool) {
		LB_UNREF(LB_GET(old_pool->lb_pool));
	}

	return trycode;
}
void 
httpn_report_origin_log(httpn_io_context_t *iocb)
{
	char 	zcode[32] = "";
	lb_t	*lb = NULL;
	char	infostr[512]="";

#ifndef NC_RELEASE_BUILD
	lb = httpn_get_LB(iocb);
	DEBUG_ASSERT(iocb->session == NULL || lb != NULL);
	DEBUG_ASSERT(CFS_DRIVER_VALID(iocb->driver));
	DEBUG_ASSERT(CFS_DRIVER_INUSE(iocb->driver));
	DEBUG_ASSERT(iocb->volume == NULL || (iocb->volume && iocb->volume->magic == NC_MAGIC_KEY));
	TRACE((0, "DRIVER[%s]->refcnt[%d].shutdown[%d]: LB[%p].shutdown[%d] reporting\n",
					iocb->driver->signature,
					iocb->driver->refcnt,
					iocb->driver->shutdown,
					lb, 
					lb->shutdown));

	if (iocb->driver->shutdown != 0) {
		TRACE((0, "DRIVER[%s]->refcnt[%d]: LB[%p] driver's shutdown marked(cbd=%p)\n",
					iocb->driver->signature,
					iocb->driver->refcnt,
					lb, 
					iocb->driverX->origin_monitor_cbd
					));
		return; /* reporting 포기 */
	}
	if (lb->shutdown != 0 ) {
		TRACE((0, "DRIVER[%s]->refcnt[%d]: LB[%p] shutdown marked(cbd=%p)\n",
					iocb->driver->signature,
					iocb->driver->refcnt,
					lb,
					iocb->driverX->origin_monitor_cbd
					));

		return; /* reporting 포기 */
	}
#else
	lb = httpn_get_LB(iocb);
	DEBUG_ASSERT(iocb->session == NULL || lb != NULL);
	DEBUG_ASSERT(iocb->driver->refcnt > 0);
	if (iocb->driver->shutdown != 0) {
		return; /* reporting 포기 */
	}
	if (lb->shutdown != 0 ) {
		return; /* reporting 포기 */
	}
#endif
	if (_ATOMIC_CAS(iocb->reported,0, 1) && (iocb->driverX->origin_monitor_proc || iocb->driverX->origin_monitor2_proc)) {
		TRACE((T_PLUGIN|T_INODE, "DRIVER[%s]->refcnt[%d]: IOCB[%d] LB[%p]/Ref[%d] reporting (cbd=%p)\n",
						iocb->driver->signature,
						iocb->driver->refcnt,
						iocb->id,
						lb,
						lb->refcnt,
						iocb->driverX->origin_monitor_cbd
			  ));

		DEBUG_ASSERT(CFS_DRIVER_INUSE(iocb->driver));
		DEBUG_ASSERT(CFS_DRIVER_VALID(iocb->driver));

		if (iocb->request && iocb->pool && iocb->session) {
			sprintf(zcode, "%u", iocb->last_httpcode);
			TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u]/PATH[%s] reporting an origin activity log\n", iocb->id, iocb->wpath));
			if (iocb->driverX->origin_monitor2_proc) {
				if (iocb->last_errno != 0) {
					sprintf(infostr, "%d-th Attempt:%s(httpcode=%d, errno=%d, CURLE=%d, timeout=%d, cancel=%d, premature=%d)",
									iocb->tries,
									iocb->last_curle_str,
									iocb->last_httpcode,
									iocb->last_errno,
									iocb->last_curle,
									iocb->timedout,
									iocb->canceled,
									iocb->premature
						   );
				}
				(*iocb->driverX->origin_monitor2_proc)(
										iocb->id,
										iocb->request->inject_property, 
										iocb->driverX->origin_monitor_cbd, 
										iocb->zmethod, 
										(iocb->request->origin?iocb->request->origin:iocb->pool->key),
										(iocb->request->eurl?iocb->request->eurl:iocb->request->url), 
										zcode, 
										iocb->request->t_elapsed, 
										iocb->request->upload_bytes, 
										iocb->request->download_bytes,
										infostr
										);


			}
			else
				(*iocb->driverX->origin_monitor_proc)(iocb->driverX->origin_monitor_cbd, 
										iocb->zmethod, 
										(iocb->request->origin?iocb->request->origin:iocb->pool->key),
										(iocb->request->eurl?iocb->request->eurl:iocb->request->url), 
										zcode, 
										iocb->request->t_elapsed, 
										iocb->request->upload_bytes, 
										iocb->request->download_bytes);
		}
	}
}
static void
httpn_init_encoding(cfs_origin_driver_t *drv, const char *lencoding, const char *sencoding)
{
	char 				utf8[] = "UTF-8";
    char 				*lc_charset = NULL; //nl_langinfo(CODESET);
	iconv_t 			from_utf_8;
	iconv_t 			to_utf_8;
	httpn_driver_info_t 	*driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	
	if (!sencoding) {
		sencoding = utf8;
	}
	strcpy(drv->s_encoding, sencoding);

	if (lencoding && lencoding[0] != 0) {
		lc_charset = (char *)lencoding;
		strcpy(drv->l_encoding, lencoding);
	}
	else {
    	lc_charset = nl_langinfo(CODESET);
		strcpy(drv->l_encoding, lc_charset);
	}

    if (strcasecmp(drv->l_encoding, "UTF-8") != 0) {
        from_utf_8 = iconv_open(drv->l_encoding, "UTF-8");
        if (from_utf_8 == (iconv_t) -1)
            from_utf_8 = 0;
        to_utf_8 = iconv_open("UTF-8", drv->l_encoding);
        if (to_utf_8 == (iconv_t) -1)
            to_utf_8 = 0;
    }

    if (strcasecmp(drv->s_encoding, drv->l_encoding) != 0) {
        if (strcasecmp(drv->s_encoding, "UTF-8") == 0) {
            driverX->from_server_enc = from_utf_8;
            driverX->to_server_enc = to_utf_8;
        } else {
            driverX->from_server_enc = iconv_open(drv->l_encoding, drv->s_encoding);
            driverX->to_server_enc = iconv_open(drv->s_encoding, drv->l_encoding);
        }
    }
	else {
		/* source and target is in same encoding scheme */
		driverX->from_server_enc = (iconv_t)-1;
		driverX->to_server_enc = (iconv_t)-1;

	}
	TRACE((T_PLUGIN, "encoding map(%s <=> %s) = (L->S=%p, S->L=%p)\n",
					drv->l_encoding,
					drv->s_encoding,
					driverX->to_server_enc,
					driverX->from_server_enc));
	
}








int
httpn_map_httpcode(httpn_io_context_t *iocb, int httpcode)
{
	int		   mapped_err = -999;



	

	switch (httpcode) {
	case 200:					/* OK */
	case 201:					/* Created */
	case 202:					/* Accepted */
	case 203:					/* Non-Authoritative Information */
	case 204:					/* No Content */
	case 205:					/* Reset Content */
	case 206:					/* Partial Content */
	case 207:					/* Multi-Status */
	case 304:					/* Not Modified */
	case 401:					/* Unauthorized */
	case 402:					/* Payment Required */
	case 407:					/* Proxy Authentication Required */
	case 301:					/* Moved Permanently */
	case 302:					/* Moved Permanently */
	case 303:					/* See Other */
	case 404:					/* Not Found */
	case 410:					/* Gone */
	case 423:					/* Locked */
	case 505:					/* HTTP Version Not Supported */
		mapped_err= 0;
		break;
	case 501:					/* Not Implemented */
		mapped_err= 0;
		break;
	case 408:					/* Request Timeout */
	case 500:					/* Internal Server Error */
	case 502:					/* Bad Gateway */
	case 503:					/* Service Unavailable */
	case 504:					/* Gateway Timeout */
		mapped_err= EREMOTEIO;
		break;
	case 400:					/* Bad Request */
	case 403:					/* Forbidden */
	case 405:					/* Method Not Allowed */
	case 409:					/* Conflict */
	case 411:					/* Length Required */
	case 412:					/* Precondition Failed */
	case 414:					/* Request-URI Too Long */
	case 415:					/* Unsupported Media Type */
	case 424:					/* Failed Dependency */
	case 413:					/* Request Entity Too Large */
	case 507:					/* Insufficient Storage */
	case 300:					/* Multiple Choices */
	case 305:					/* Use Proxy */
	case 306:					/* (Unused) */
	case 406:					/* Not Acceptable */
	case 416:					/* Requested Range Not Satisfiable */
	case 417:					/* Expectation Failed */
	case 422:					/* Unprocessable Entity */
	case 307:					/* Temporary Redirect */
	case 308:					/* Perm Redirect */
		mapped_err = 0;
		break;
	case HTTP_IM_TEAPOT:
		mapped_err = ECANCELED;
		break;
	default:
		mapped_err = 0;
		break;
	}
	TRACE((T_PLUGIN, "IOCB[%d] - http=%d -> stderrno=%d mapped\n", iocb->id, httpcode,mapped_err));
	
	return mapped_err;
}

size_t 
httpn_post_data_proc(char *ptr, size_t size, size_t nmemb, void *cb)
{
	httpn_io_context_t 			*iocb 		= (httpn_io_context_t *)cb;
	nc_xtra_options_t 			*opt 		= (nc_xtra_options_t *)iocb->in_property;
	fc_inode_t					*inode 		= iocb->inode;
	size_t 						tocpy 		= 0;
	size_t 						buflen 		= size*nmemb;
	//char						reason[128] = "";
	char						ibuf[1024];
#ifdef __PROFILE
	perf_val_t					ms, me;
	long 						ud;
#endif
	int							or = 0;
	char						zcp[32]="OK";

	DEBUG_ASSERT_IOCB(iocb, http_iocb_valid(iocb));
	kv_oob_lock(opt);

	iocb->post_avail = 0;

	if (httpn_is_state(iocb->session, HS_POSTING) && httpn_is_state(iocb->session, HS_HEADER_OUT)) {
		/*
		 * POST 중인데 오리진에서 오류나 기타 예외적인 응답을 하는 경우
		 * solproxy에게 EOD 설정을 통해서 더이상 oob_write가 진행되지 않도록 해야함
		 */
		TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%d] origin response (httpcode=%d) while posting data;%s\n",
						(inode?inode->volume->signature:"?"),
						(inode?inode->q_id:"?"),
						(inode?inode->uid:-1),
						iocb->last_httpcode,
						httpn_dump_iocb(ibuf, sizeof(ibuf), iocb)
						));
		kv_oob_set_error(opt, RBF_EOD); /* end of data */
		kv_oob_unlock(opt);
		return 0; /* post done */
	}
	PROFILE_CHECK(ms);
	httpn_migrate_http_state(iocb, HS_POSTING, __FILE__, __LINE__);



	or = tocpy = (int)kv_oob_read(opt, ptr, buflen, 10/* msec */);

	if (or > 0)  {
		iocb->postlen  += tocpy;
	}
	else if (or == 0) {
		/*
		 * read timeout or no data available
		 */


		if (kv_oob_error(opt, RBF_EOT)) {
			tocpy = 0;
			TRACE((T_PLUGIN, "IOCB[%u]/(bufsiz=%d): RING[%p] found EOT?\n",
						iocb->id, 
						(int)buflen, 
						opt->oob_ring
						));
		}
		else {
			strcpy(zcp, "READ-PAUSE");
			tocpy = CURL_READFUNC_PAUSE;
			/*
			 * POST 중의 PAUSE는 상태변환을 하지 않고
			 * iocb->needresume과 session_monitor를 통해 처리
			 */
			httpn_mpx_pause_iocb_nolock(iocb);
			TRACE((T_PLUGIN, "IOCB[%d] - no data available, going to sleep\n", iocb->id));
		}
	}
	else {
		tocpy 			= 0;

	}
	kv_oob_unlock(opt);


	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%d]/RING[%p] - copied=%d(%s);%s\n", 
						iocb->id, 
						opt,
						(int)tocpy,
						zcp,
						httpn_dump_iocb_s(ibuf, sizeof(ibuf), iocb)
						));
	PROFILE_CHECK(me);
#ifdef __PROFILE
	ud = PROFILE_GAP_MSEC(ms, me);
	if (ud > 1000) {
		TRACE((T_PLUGIN, "Volume[%s].%s(%s)::IOCB[%u] RING[%p] TOO slow(%ld msec)\n",
						iocb->driver->signature,
						iocb->zmethod,
						iocb->wpath,
						iocb->id, 
						opt->oob_ring,
						ud
						));
	}
#endif
	return tocpy;
}
int
httpn_post_notification_callback(nc_xtra_options_t *opt, void *ud)
{
	httpn_io_context_t *iocb = (httpn_io_context_t *)ud;
	char				ibuf[1024];
	httpn_mux_info_t	*mpx;

	mpx				= iocb->mpx;



	kv_oob_lock(opt);
	if (iocb->session == NULL || !(httpn_is_state(iocb->session, HS_INIT) || httpn_is_state(iocb->session, HS_POSTING))) {
		/*
		 * session이 NULL이거나
		 * session state가 header나 body 받는 상태라면 바로 리턴
		 */
		TRACE((T_PLUGIN, "IOCB[%u] - not in proper state[%s], returning\n", iocb->id, httpn_zstate(iocb->session)));
		kv_oob_unlock(opt);
		return 0;
	}


	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%d] - %s(%s) : INODE[%d] got notified that data is available(%s%s%s);{%s}\n",
						iocb->id,
						iocb->zmethod,
						iocb->wpath,
						(iocb->inode?iocb->inode->uid:-1),
						(kv_oob_error(opt, RBF_READER_BROKEN)?"R_BRK ":""),
						(kv_oob_error(opt, RBF_WRITER_BROKEN)?"W_BRK ":""),
						(kv_oob_error(opt, RBF_EOT)?"EOT ":""),
						httpn_dump_iocb(ibuf, sizeof(ibuf), iocb)
						));

	if (kv_oob_error(opt, RBF_READER_BROKEN|RBF_EOD)) {
		/*
		 * netcache쪽에서 종료
		 */
		TRACE((T_PLUGIN, "IOCB[%d] - RING-IO closed by the netcache driver(READER_BROKEN)\n", iocb->id));
		kv_oob_unlock(opt);
		goto L_resume;
	}


	kv_oob_unlock(opt);

L_resume:
	iocb->post_avail = 1;
	TRACE((T_PLUGIN, "IOCB[%d] - data written, resume required\n", iocb->id));

	return 0;
}

void
httpn_set_status(httpn_io_context_t *iocb)
{
	__httpn_httpcode 	= iocb->last_httpcode;
	__httpn_errno 		= iocb->last_errno;
}
#if NOT_USED
static int
httpn_get_errno(void)
{
	return __httpn_errno;
}
#endif


#if NOT_USED
static int 
httpn_wait_on(httpn_file_handle_t *hfh, pthread_cond_t *cond, long waitmsec)
{
	struct timeval		tv;
	struct timespec		ts;
	int 				delta_sec;
	int 				r;

	gettimeofday(&tv, NULL);
	ts.tv_sec  	= tv.tv_sec + (delta_sec = (waitmsec/1000L));
	ts.tv_nsec 	= tv.tv_usec*1000L + ((waitmsec - delta_sec*1000L)*1000000L);
	r = pthread_cond_timedwait(cond, &hfh->lock, &ts);
	return (r == 0);
}
static void
httpn_report_error(nc_asio_vector_t *vector, int cursor, int err)
{
	int 		i;

	for (i = cursor; i < vector->iov_cnt; i++) {
		if (vector->iov_asio_callback) {
			TRACE((T_WARN, "ASIO[%d] - %d-th blk reported with error %d\n", vector->iov_id, i));
			vector->iov_asio_callback(vector, i, FILE_OFFSET(ASIO_CONTEXT(vector, i)->blkno), NULL, 0, err);
		}
	}
	return ;
}
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static void *
httpn_curl_malloc(size_t siz)
{
	return __nc_malloc(siz, AI_DRIVERLIB, __FILE__, __LINE__);
}
static void
httpn_curl_free(void *ptr)
{
	return __nc_free(ptr, __FILE__, __LINE__);
}
static void *
httpn_curl_realloc(void *ptr, size_t siz)
{
	return __nc_realloc(ptr, siz, AI_DRIVERLIB, __FILE__, __LINE__);
}
static char *
httpn_curl_strdup(const char *str)
{
	return __nc_strdup(str, AI_DRIVERLIB, __FILE__, __LINE__);
}
static void *
httpn_curl_calloc(size_t nmemb, size_t siz)
{
	return __nc_calloc(nmemb, siz, AI_DRIVERLIB, __FILE__, __LINE__);
}
#pragma GCC diagnostic pop

static nc_origin_session_t 
httpn_allocate_context(cfs_origin_driver_t *drv, apc_open_context_t *aoc)
{
	httpn_io_context_t		*iocb = NULL;
	nc_uint32_t				kept;

	iocb = httpn_create_iocb(drv, aoc->origin_path, aoc->in_prop, aoc->mode, NULL, U_FALSE, aoc->open_cc, &kept, __FILE__, __LINE__);
	if (iocb) {
		httpn_set_owner(iocb, HTTP_API_GLOBAL);
		iocb->keptid = kept;
		TRACE((T_PLUGIN, "IOCB[%u]/PATH[%s] allocated by client\n", iocb->id, iocb->wpath));
	}
	return iocb;
}
static int
httpn_valid_context(cfs_origin_driver_t *drv, nc_origin_session_t vctx, char *path, nc_off_t *off, nc_size_t *len)
{
	int 				v = U_TRUE;
	httpn_io_context_t 	*ctx = (httpn_io_context_t *)vctx;
	nc_off_t 			orgoff = *off;
	nc_size_t 			orglen = *len;

	if (!ctx) return U_FALSE;

	if (ctx->method != HTTP_POST) {
		v = v && (ctx->session && httpn_is_state(ctx->session, HS_EOH));
	}
	if (ctx->request) {
		v = v && httpn_request_contains(ctx, (cfs_off_t *)off, (cfs_size_t *)len);
	}
	TRACE((T_PLUGIN, "IOCB[%u]/PATH[%s]/[%lld,%lld] %s\n", ctx->id, ctx->wpath, orgoff, orglen, (v?"VALID":"INVALID")));
	return v;

}
static void
httpn_free_context(cfs_origin_driver_t *drv, nc_origin_session_t vctx)
{
	httpn_io_context_t 	*ctx = (httpn_io_context_t *)vctx;

	TRACE((T_PLUGIN, "IOCB[%u] destroying from client\n", ctx->id));
	DEBUG_ASSERT( ctx->cmd_id != HMC_DESTROY);

	tlcq_enqueue(g__iocb_free_queue, vctx);
	return;
}
static void
httpn_update_probe_url_ALL(cfs_origin_driver_t *drv, char *url)
{
	int					i,j;
	httpn_driver_info_t *driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	for (i = 0; i < 2; i++) {
		if (driverX->pool[i]) {
			for (j = 0; j < driverX->pool_cnt[i]; j++) {
				if (driverX->pool[i][j])
					httpn_update_probe_url(drv, driverX->pool[i][j], url, U_TRUE);
			}
		}
	}
}
static void
httpn_update_probe_url(cfs_origin_driver_t *drv, httpn_session_pool_t *pool, char *url, int force)
{

	long	td;
	int		old_fv = 0;
	/*
	 * frozen이 아니면 1시간에 한번만 수정
	 */
	td = (pool->probe_url_frozen?-1:(nc_cached_clock() - pool->probe_url_utime));
	if ((url == NULL || strlen(url) < sizeof(pool->probe_url)) && 
		(force || td > 3600)) {
		TRACE((T_PLUGIN, "VOLUME[%s] - POOL[%s] updated probe URL to '%s'(%s)\n",
					drv->signature, pool->key, url, ((force && url)?"SET":"")));
		if (url) {
			strcpy(pool->probe_url, url);
			pool->probe_url_utime	= nc_cached_clock();
		}
		old_fv = pool->probe_url_frozen;	
		pool->probe_url_frozen	= (force && url != NULL);

		if (old_fv == 1 && pool->probe_url_frozen == 0) {
			pool->probe_url_utime	= 0;
		}
	}
}


int
httpn_handle_response_headers(httpn_io_context_t *iocb) 
{
	int		r = 0;
	char	ziocb[4096];


	TRACE((iocb->tflg|T_PLUGIN, 	"Volume[%s].URL[%s] - IOCB[%u].owner[%s] got final response '%d'(PROP[%p], state[%s])\n", 
						iocb->driver->signature,
						iocb->request->url, 
						iocb->id, 
						__zowner[iocb->owner],
						iocb->last_httpcode, 
						iocb->stat.st_property, 
						_zmpxstate[iocb->state]));


#ifdef	HTTPN_MEASURE_IOCB
	PROFILE_CHECK(iocb->t_hdrend);
#endif
	if ( iocb->driverX->on_receive_response_proc) {
		httpn_PH_on_receive_response(iocb);
	}


	httpn_post_handler(iocb);


	/*
	 * READ이고 신선도 검사단계가 아닌경우 호출
	 */
	if (!iocb->imson && iocb->owner == HTTP_API_READ) {
		if (iocb->driverX->io_validator_proc && iocb->inode) {
			if ((*iocb->driverX->io_validator_proc)(iocb->driver, iocb->inode,  &iocb->stat) < 0) {
				/*
				 * detected the cache staled while reading.
				 */
				r = -1;
				TRACE((iocb->tflg|T_PLUGIN, "IOCB[%d].URL[%s] - notified staled from netcache core;%s\n", 
										iocb->id, 
										iocb->wpath, 
										httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)
										));
			}
		}

	}
	return r;

}
static void 
httpn_origin_monitor(lb_t *lb, char *id, void *ud, int online)
{
	cfs_origin_driver_t 	*drv = (cfs_origin_driver_t *)ud;

	drv->online = (httpn_next_pool(drv, "/", U_FALSE) != NULL);


	TRACE((T_INFO, "VOLUME[%s] - one of origin, \"%s\" goes %s (driver online[%d])\n", drv->signature, id, (online?"ONLINE":"OFFLINE"), drv->online));
}

static void
httpn_unload()
{

	g__iocb_cleaner_shutdown = 1;
	pthread_join(g__iocb_cleaner, NULL); 
	tp_stop(g__netpool);

}

static char *
httpn_dump_session(char *buf, int siz, void *u)
{
	return httpn_dump_iocb(buf, siz, (httpn_io_context_t *)u);

}


static int
httpn_read_mpx_epilog(void *u)
{
	int							need_fini;
	nc_asio_vector_t * 			vector = NULL;
	httpn_io_context_t *		iocb = (httpn_io_context_t *)u;
	char						ziocb[2048];
	int							xtracb = 0;
	fc_blk_t					*blk = NULL;
	


	vector = iocb->vector;



	/*
	 * 응답 코드 저장
	 */
	need_fini				= asio_need_EOV(vector);
	vector->iov_xfer_blks 	= (int)iocb->u.r.blk_filled;

	xtracb 					= ((iocb->u.r.blk_off > 0) && (iocb->u.r.blk_filled <= iocb->u.r.blk_cursor));
	TRACE((iocb->tflg|T_PLUGIN, 	
						"INODE[%u]/ASIO[%ld] - IOCB[%u] NEED_FINI=%d, xfer_blks=%d(would +1), asio_callback=%d;%s\n",
						(int)iocb->inode->uid, 
						(long)iocb->vector->iov_id, 
						iocb->id,
						(int)need_fini, 
						vector->iov_xfer_blks,
						xtracb,
						httpn_dump_iocb(ziocb,sizeof(ziocb), iocb)
						));
	//if ((iocb->canceled == 0  && iocb->timedout == 0) && (iocb->u.r.blk_off > 0) && (iocb->u.r.blk_filled <= iocb->u.r.blk_cursor)) {
	if (iocb->canceled == 0  && (iocb->u.r.blk_off > 0) && (iocb->u.r.blk_filled <= iocb->u.r.blk_cursor)) {
		/* 
		 * 블럭을 채우지 못하고 수신완료됨
		 * 여기에서 강제로 context handler가 호출되어야함
		 * cancel또는 timeout 경우는 제외하기로 함(2020.6.18 by weon@solbox.com)
		 */
#ifndef NC_RELEASE_BUILD
		blk = ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor);
		if (iocb->last_errno == 0) {
			DEBUG_ASSERT_FILE(iocb->inode, 
					((iocb->inode->maxblkno > blk->blkno) && (iocb->u.r.blk_off == NC_BLOCK_SIZE)) ||
					((iocb->inode->maxblkno == blk->blkno) && (iocb->u.r.blk_off <= NC_BLOCK_SIZE))
					);
		}
		TRACE((0, "INODE[%d] - blk#%d/BUID[%d] NIO cleared\n", iocb->inode->uid, blk->blkno, BLOCK_UID(blk)));
		ASIO_BLOCK(iocb->vector, iocb->u.r.blk_cursor)->nioprogress = 0;
#endif
		if (iocb->last_curle != 0) {
			TRACE((T_WARN, "Volume[%s].CKey[%s] - IO error(curle=%d) while reading\n",
						iocb->inode->volume->signature,
						iocb->inode->q_id,
						iocb->last_curle
				  ));
		}
		iocb->vector->iov_asio_callback(iocb->vector, iocb->u.r.blk_cursor, iocb->u.r.file_off, NULL, iocb->u.r.blk_off, iocb->last_errno);
		vector->iov_xfer_blks++; 	
	}
	vector->iov_error = iocb->last_errno;

	TRACE((T_PLUGIN, "Volume[%s].CKey[%s] - DONE[iov_error=%d]\n",
					iocb->inode->volume->signature,
					iocb->inode->q_id,
					vector->iov_error
			  ));

	vector->type 		= APC_READ_VECTOR_DONE;
	asio_post_apc(vector, __FILE__, __LINE__);
	return 0;
}

int
httpn_PH_on_receive_response(httpn_io_context_t *iocb)
{
	nc_origin_io_command_t *rcv_cmd = (nc_origin_io_command_t *)alloca(sizeof(nc_origin_io_command_t));
	httpn_request_t 		*req = iocb->request;



	rcv_cmd->status 	= iocb->last_httpcode;
	rcv_cmd->method 	= iocb->zmethod;
	rcv_cmd->url 		= iocb->request->url;
	rcv_cmd->cbdata 	= iocb->driverX->on_receive_response_cbdata;
	rcv_cmd->properties = iocb->stat.st_property;
	(*iocb->driverX->on_receive_response_proc)(NC_ORIGIN_PHASE_RESPONSE, rcv_cmd);
	if (iocb->request->url != rcv_cmd->url) {
		/* URL이 메모리 포인터가 변경되었음 */
		free(req->url);
		iocb->request->url = rcv_cmd->url;
	}
	return 0;
}
static int
httpn_set_range_info(cfs_origin_driver_t *drv, nc_origin_session_t vctx, nc_int64_t off, nc_int64_t len)
{
	httpn_set_readinfo(vctx, off, len);
	return 0;
}
int
httpn_valid_config(cfs_origin_driver_t *drv, nc_uint32_t confver)
{
	httpn_driver_info_t 	*driverX; 

	driverX = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	return (driverX->confver == confver);
}

cfs_origin_driver_t __httpn_native = {
	.name 			= DRIVER_NAME,
	.driver_data 	= NULL,
	.ctx 			= NULL,
	.open			= httpn_open,
	.read			= httpn_read_mpx,
	.write_stream	= NULL,
	.close			= httpn_close,
	.getattr		= httpn_getattr_mpx,
	.readdir		= NULL,
	.lioctl			= httpn_ioctl,
	.statfs 		= NULL,


	.write			= NULL,
	.truncate 		= NULL,
	.ftruncate 		= NULL,
	.create			= NULL,
	.mkdir			= NULL,
	.rmdir			= NULL,
	.unlink			= NULL,
	.rename			= NULL,
	.flush			= NULL,
	//.online 		= httpn_online,
	.iserror 		= httpn_iserror,
	.set_lasterror 	= httpn_set_errno,
	.lasterror		= httpn_errno,
	.issamecode		= httpn_issamecode,
	.bind_context 		= httpn_bind_context,
	.unbind_context 	= httpn_unbind_context,
	.create_instance 	= httpn_create_instance,
	.destroy_instance 	= httpn_destroy_instance,
	.allocate_context 	= httpn_allocate_context,
	.valid_context 		= httpn_valid_context,
	.free_context 		= httpn_free_context,
	.set_read_range     = httpn_set_range_info,

	.dump_session 		= httpn_dump_session,
	.unload 		= httpn_unload

};


