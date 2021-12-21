#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <malloc.h>
#include <ctype.h>
#include <string.h>
//#include <ncapi.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
//#include "trace.h"
#include "scx_util.h"
#include "streaming.h"
#include "sessionmgr.h"
/*
 * log formatting
 * format specification : like apache

 * %a : the remote client IP
 * %A : the local client IP
 * %B : The remaining byte to transfer
 * %b : # of bytes transmitted, excluding HTTP headers, '-' = no bytes transmitted
 * %c : request's cache control values if any
 * %C : Session ID
 * %D : client의 request를 받아서 response처리를 끝내기까지의 시간, micro second 단위
 * %E : origin 요청에 사용된 시간, 초단위
 * %f : requested file name
 * %F : first byte send time, client의 request를 받아서 response header를 보내기까지의 시간, 초단위
 * %H : requested protocol
 * %h : remote host name or IP
 * %{var}i : the content of the HTTP header line named var, %{User-agent}i = Mozilla/...
 * %I : client request size(header+body). 단위 : byte
 * %l : ISO 8601 시간 포맷 (예: 2019-10-31T17:32:51+09:00)
 * %m : requested method
 * %O : client response size(header+body). 단위 : byte
 * %P : unique ID
 * %{remote}p, %{local}p: client 혹은 서버의 port
 * %q : the query string prepended with a '?'
 * %r : the first of the request. example : GET / HTTP/1.1
 * %s : server response code
 * %S : ssl version + cipher suite info
 * %T : client의 request를 받아서 response처리를 끝내기까지의 시간, 초단위
 * %t : Local time (예: Thu, 21 Nov 2013 10:32:58 +0900), UTC time (예: Wed, 20 Nov 2013 14:59:36 GMT)
 * %U : request URL path
 * %v : host name
 * %x : hit status
 * %X : client termination code
 * %z : zipper build에 사용된 시간, 초단위

 * %[condition][param of info-specifier]<info-specifier>
 * examples)
 * 	%400,501{User-agent}i  	=> status code가 400 또는 501인 경우 User-Agent 출력
 *	%{Cache-Control}i  		=> request의 User-Agent 출력
 *	%{%D %T}t				=> strftime의 포맷 스트링에 따라 시간 표시
 * 
 * 정의된 state 
 *

 * 	S_INIT : 초기 상태
 *  S_FMT_BEGIN : 
 *  S_FMT_COND : 
 *  S_FMT_INFOP : 
 *  S_FMT_INFOE : 
 *  S_FMT_ESCAPE : 


 *  %200,206,302,303h
 */

typedef enum {
	S_FMT_INIT 	= 0, 
	S_FMT_BEGIN = 1, 
	S_FMT_COND 	= 2, 
	S_FMT_INFOP = 3,
	S_FMT_INFOE = 4
} fmt_state_t;
typedef struct {
	int 	alloced;
	int 	offset; /* offset & length */
	char 	*buffer;
} fmt_string_t;
typedef struct {
	fmt_state_t 	state;
	char 			*buffer;
	int 			remained;
	int 			offset;
	const char 		*fmt;
	int 			fmtidx;
	fmt_string_t 	*condition;
	fmt_string_t 	*symbol;
	nc_request_t 	*req;
} fmt_context_t;

typedef int (*hlf_state_proc_t)(fmt_context_t *, char x);

static int hlf_proc_init(fmt_context_t *ctx, char x);
static int hlf_proc_begin(fmt_context_t *ctx, char x);
static int hlf_proc_infop(fmt_context_t *ctx, char x);
static int hlf_proc_infoe(fmt_context_t *ctx, char x);
static int hlf_proc_cond(fmt_context_t *ctx, char x);
static int hlf_handle_quark(fmt_context_t *ctx, char x); 
char * hlf_get_hit(nc_request_t *req, char *buf, int size);

static hlf_state_proc_t 		__proc_dict[] = {
	hlf_proc_init,
	hlf_proc_begin,
	hlf_proc_cond,
	hlf_proc_infop,
	hlf_proc_infoe
	//hlf_proc_escape  
};

char g__method_string[][8] = {
		"GET",
		"HEAD",
		"POST",
		"PUT",
		"OPTIONS",
		"UNKNWN"
};



static fmt_string_t *
fmt_string_create(mem_pool_t *pool)
{
	fmt_string_t 	*ft = NULL;

	ft = (fmt_string_t *)mp_alloc(pool, sizeof(fmt_string_t));
	ft->buffer 	= NULL;
	ft->offset 	= 0;
	ft->alloced = 0;
	return ft;
}
static void
fmt_string_destroy(fmt_string_t *s)
{
//	if (s->buffer) SCX_FREE(s->buffer);
//	SCX_FREE(s);
}
static void
fmt_string_append_char(mem_pool_t *pool, fmt_string_t *fs, char x)
{
	if (fs->alloced <= fs->offset) {
		fs->alloced += 64;
		fs->buffer = (char *)mp_realloc(pool, fs->buffer, fs->alloced);
	}
	fs->buffer[fs->offset++] = x;
	fs->buffer[fs->offset] 	 = 0;
	return ;
}
static char *
fmt_get_buffer(fmt_string_t *fs)
{
	return fs->buffer;
}

static int
hlf_proc_init(fmt_context_t *ctx, char x)
{
	if (x == '%') {
		ctx->state = S_FMT_BEGIN;
	}
	else {
		ctx->buffer[ctx->offset++] = x;
		ctx->remained--;
	}
	return 0;
}

static int
hlf_proc_begin(fmt_context_t *ctx, char x)
{
	int r = -1;
	if (isdigit(x) || x == '!') {
		ctx->state = S_FMT_COND;
		r = hlf_proc_cond(ctx, x);
	}
	else if (isalpha(x)) {
		r = hlf_handle_quark(ctx, x);
		ctx->state = S_FMT_INIT;
	}
	else if (x == '{') {
		ctx->state = S_FMT_INFOP;
		r = 0;
	}

	return r;
}
static int
hlf_proc_cond(fmt_context_t *ctx, char x)
{
	int r = -1;
	if (ctx->condition == NULL) {
		ctx->condition = fmt_string_create(ctx->req->pool);
	}
	if (isdigit(x) || x == '!' || x == ',')  {
		fmt_string_append_char(ctx->req->pool, ctx->condition, x);
		r = 0;
	}
	else if (x == '{') {
		ctx->state = S_FMT_INFOP;
		r = 0;
	}
	return r;
}
static int
hlf_proc_infop(fmt_context_t *ctx, char x)
{
	int 	r = -1;
	char 	spc[] = " \"'\t:;/.<>?[]!@#$%^&*()_+|-=";
	if (isalnum(x) || strchr(spc, x )) {
		if (ctx->symbol == NULL) {
			ctx->symbol = fmt_string_create(ctx->req->pool);
		}
		fmt_string_append_char(ctx->req->pool, ctx->symbol, x);
		r = 0;
	}
	else if (x == '}') {
		ctx->state = S_FMT_INFOE;
		r = 0;
	}
	return r;
}
static int
hlf_proc_infoe(fmt_context_t *ctx, char x)
{
	int 	r = -1;
	if (isalpha(x)) {
		r = hlf_handle_quark(ctx, x);
		ctx->state = S_FMT_INIT;
	}
	else  {
		fprintf(stderr, "error in infoe \n");
		ctx->state = S_FMT_INIT;
		r = -1;
	}
	return r;
}
char *
hlf_find_value(nc_request_t *req, char *symbol)
{
	static char __null_v[]="-";
	char 		*pfind = NULL;
	int			pos = 0;
	char		*found = NULL;
	if (strncasecmp(symbol, "host", 5) == 0) {
		/*
		 * client request의 host헤더는 req->host 값을 사용 해야 한다.
		 * req->options에 있는 host는 오리진에 요청한 host 헤더임
		 */
		if (req->host) {
			pfind = vs_data(req->host);
		}

	}
	else if (req->options) {
		pfind = (char *)kv_find_val_extened(req->options, symbol, 0/*ignore case */, 1);
		if (strncasecmp(symbol, "X-Forwarded-For", 15) == 0	&& pfind) {
#if 0
			/* "192.168.10.1,127.0.0.1"으로 들어 왔을때 "192.168.10.1"로 출력 하도록함
			 * 이때 192.168.10.1는 client의 IP이고 127.0.0.1는 중간에 있는 proxy(nginx)의 IP이다.*/
			found = strstr(pfind, vs_data(req->client_ip));
			if (NULL != found && pfind != found) {
				pos = found - pfind - 1;
				if (0 < pos) {
					*(pfind+pos) = '\0';
				}
			}
#else
			/*
			 * X-Forwarded-For로 지정된 경우에는 client IP만 로그에 기록하도록 한다.
			 * 현재 구현에서는 앞단에 nginx가 proxy로 있는 경우 "X-Forwarded-For: 123.142.45.228, 127.0.0.1, 127.0.0.1"의 형태로 들어 온다
			 * "X-Forwarded-For: 123.142.45.228 , 127.0.0.1"와 "X-Forwarded-For: 123.142.45.228, 127.0.0.1" 같이 컴마(,)앞에 공백이 포함된 경우까지 지원하기 위해 dilimiter를 ", "로 사용한다.
			 */
			found = strtok(pfind, ", ");
			if (NULL != found && pfind != found) {
				pos = found - pfind;
				if (0 < pos) {
					*(pfind+pos) = '\0';
				}
			}
#endif
		}
	}
	return (pfind?pfind:__null_v);
}
#if 0
char *
hlf_find_response_value(nc_request_t *req, char *symbol)
{
	static char __null_v[]="";
	char 		*pfind = NULL;
	if (req->req_options)
		pfind = (char *)kv_find_val_extened(req->res_options, symbol, 0/*ignore case */, 1);
	return (pfind?pfind:__null_v);
}
#endif

char *
hlf_get_client_cc(nc_request_t *req, char *zbuf, int size)
{
	const char 		*v ;
	int 		n;
	char 		*buforg = zbuf;
	int 		copied = 0, zl;
//	size -= 1;
	if (req->options) {
		v = (char *)kv_find_val_extened(req->options, "Cache-Control", 0, 1);
		if (v) {
			zl = scx_snprintf(zbuf, size, "%s: %s", "Cache-Control", v);
			size -= zl;
			zbuf += zl;
			copied += zl;

		}
		v = kv_find_val_extened(req->options, "Pragma", 0, 1);
		if (v) {
			if (copied != 0 && size > 1) {
				strncat(zbuf, ";", size);
				size -= 1;
				zbuf++;
			}
			zl = scx_snprintf(zbuf, size, "%s: %s", "Pragma", v);
			size -= zl;
			copied += zl;
			zbuf += zl;
		}
	}
	if (copied == 0) {
		*zbuf++ = '-';
	}
	else {
		TRACE((T_DEBUG, "hlf_get_client_cc : '%s' - found\n", buforg));
	}
	*zbuf = 0;
	return buforg;
}
/* ncapi.h의 nc_hit_status_t 정의 참조 */
char __z_hit_code[][32] = {
	"-",			/* 0, 초기 상태 */
	"MISS",					/* 1, 객체가 캐싱되어 있지 않음 */
	"HIT", 					/* 2, 객체가 캐싱되어 있지 않음 */
	"MISS", 				/* 3, 객체가 캐싱되어 있지 않음 */
	"HIT", 				/* 4, 객체가 캐싱되어 있지 않음 */
	"REFRESH_HIT",			/* 5, 원본 객체가 nocache 요청임 */
	"REFRESH_MISS",			/* 6, 원본 객체가 nocache 요청임 */
	"OFF_HIT",				/* 7, 원본 장애라서 캐싱된 객체 정보 전달 */
	"NEG_HIT",				/* 8, 객체 에러(notfound등)가 계속 유효함 */
	"NEG_MISS",				/* 9, 객체 에러이지만 캐싱되지 않은 상태일 때 */
	"NEG_REFRESH_HIT",		/* 10,객체 에러가 캐싱되어있으며 유효기간 지나서 다시 체크했더니 동일 */
	"NEG_OFF_HIT",				/* 11, 객체 에러가 캐싱되어있으며 원본이 다운된 상태 */
	"NOTFOUND",				/* 12, 원본 장애라서 캐싱된 객체 정보 전달 */
	"NEG_REFRESH_MISS",		/* 13, 캐시를 사용하지않고 원본 조회 */
	"BYPASS",			/* 14, nocache또는 private인 경우 향후 private인 경우 제외해야함 */
};
char *
hlf_get_hit(nc_request_t *req, char *buf, int size)
{

	strncpy(buf, __z_hit_code[req->hi], size);
	return buf;
}
/* microhttpd.h의 MHD_RequestTerminationCode 정의 참조 */
char __z_toe_code[][32] = {
	"OK",	/* 0, MHD_REQUEST_TERMINATED_COMPLETED_OK  */
	"WE",	/* 1, MHD_REQUEST_TERMINATED_WITH_ERROR */
	"TO", 	/* 2, MHD_REQUEST_TERMINATED_TIMEOUT_REACHED */
	"SH", 	/* 3, MHD_REQUEST_TERMINATED_DAEMON_SHUTDOWN */
	"RE", 	/* 4, MHD_REQUEST_TERMINATED_READ_ERROR */
	"CA",	/* 5, MHD_REQUEST_TERMINATED_CLIENT_ABORT */
};
char *
hlf_get_toe(nc_request_t *req, char *buf, int size)
{
	if (req->toe > 5) {
		strncpy(buf, "UNDEFINED", size);
	}
	else {
		strncpy(buf, __z_toe_code[req->toe], size);
	}
	return buf;
}

int a(fmt_context_t *ctx)
{
	int 		n;
	/* the remote client IP */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "remote(a)");
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int A(fmt_context_t *ctx)
{
	int 		n;
	/* 서버 IP */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, vs_data(ctx->req->server_ip));
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

int b(fmt_context_t *ctx)
{
	int 		n;
	/* # of bytes transmitted, excluding HTTP headers, '-' = no bytes transmitted */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%lld", (long long)ctx->req->res_body_size);
	//fprintf(stderr, "[%c] => %lld\n", x, (long long)ctx->req->xfered);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int B(fmt_context_t *ctx)
{
	int 		n;
	/* The remaining byte to transfer(전송하지 못한 byte수, 이 값이 0보다 큰경우 연결이 중간에 끊어진 경우라고 볼수 있다) */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%lld", (long long)ctx->req->remained );
	//fprintf(stderr, "[%c] => %lld\n", x, (long long)ctx->req->xfered);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

int c(fmt_context_t *ctx)
{
	int 		n;
	char 		zcc[128];
	/* client's cache-control tags */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", (char *)hlf_get_client_cc(ctx->req, zcc, sizeof(zcc)));
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int C(fmt_context_t *ctx)
{
	/* Session ID 기록 */
	int 		n;
	char 		zcc[128];
	if(ctx->req->streaming)
	{
		if (ctx->req->streaming->session != NULL) {
			n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%d", ctx->req->streaming->session->id);
		}
		else {
			n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "-");
		}
	}
	else {
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "-");
	}
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int D(fmt_context_t *ctx)
{	/* client의 request를 받아서 response처리를 끝내기까지의 시간, micro second 단위 */
	int 		n;
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%.2f", (float)(ctx->req->t_res_compl - ctx->req->t_req_fba));
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int E(fmt_context_t *ctx)
{	/* origin 요청에 사용된 시간, 초단위 */
	int 		n;
	double 		sum = 0;
	sum = ctx->req->t_nc_open + ctx->req->t_nc_read + ctx->req->t_nc_close;
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%.2f", (float)(sum/1000000.0));
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int f(fmt_context_t *ctx)
{
	int 		n;
	/* requested file name */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "file");
	//fprintf(stderr, "[%c] => file\n", x);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int F(fmt_context_t *ctx)
{	/* first byte send time, client의 request를 받아서 response header를 보내기까지의 시간, 초단위*/
	int 		n;
	if(ctx->req->t_req_fba > ctx->req->t_res_fbs) {
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%.2f", 0.0);
	}
	else {
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%.2f", (float)((ctx->req->t_res_fbs - ctx->req->t_req_fba)/1000000.0));
	}

	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int H(fmt_context_t *ctx)
{
	int n;
	int type;
	type = ctx->req->connect_type;
	if (type > SCX_PORT_TYPE_CNT) {
		type = SCX_PORT_TYPE_CNT;
	}
	/* requested protocol */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", _scx_port_name[type]);

	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
/* 아래의 알파벳으로 된 function들은 로그 포맷시 사용하는 설정값과 동일한 이름으로 하기 위해서 한글자로만 naming을 했음 */
int h(fmt_context_t *ctx)
{
	int 		n;
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, (ctx->req->client_ip?(char *)vs_data(ctx->req->client_ip):"0.0.0.0"));
	//fprintf(stderr, "[%c] => %s\n", x, &ctx->buffer[ctx->offset] );
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

int I(fmt_context_t *ctx)
{
	int 		n;
	/* 요청과 헤더를 포함한 수신 바이트 수 */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%lld", ctx->req->req_hdr_size + ctx->req->req_body_size);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int i(fmt_context_t *ctx)
{
	int 		n,r=0;
	/* the content of the HTTP header line named var, %{User-agent}i = Mozilla/...*/
	if (!ctx->symbol) {
		char *field = NULL;
		/* invalid syntax */

		r = -1;
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "i(?)");
		ctx->remained 	-= n;
		ctx->offset 	+= n;
	}
	else {
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", (char *)hlf_find_value(ctx->req, fmt_get_buffer(ctx->symbol)));
		//fprintf(stderr, "[%c]/{%s} => %s\n",  x, fmt_get_buffer(ctx->symbol), (char *)hlf_find_value(ctx->req, fmt_get_buffer(ctx->symbol)));
		ctx->remained 	-= n;
		ctx->offset 	+= n;
	}
}


#if 0
/usr/include/time.h
struct tm
{
  int tm_sec;           /* Seconds. [0-60] (1 leap second) */
  int tm_min;           /* Minutes. [0-59] */
  int tm_hour;          /* Hours.   [0-23] */
  int tm_mday;          /* Day.     [1-31] */
  int tm_mon;           /* Month.   [0-11] */
  int tm_year;          /* Year - 1900.  */
  int tm_wday;          /* Day of week. [0-6] */
  int tm_yday;          /* Days in year.[0-365] */
  int tm_isdst;         /* DST.     [-1/0/1]*/
  long int tm_gmtoff;       /* Seconds east of UTC.  */
  __const char *tm_zone;    /* Timezone abbreviation.  */
};
#endif
int l(fmt_context_t *ctx)
{
	int 		n, r = 0;
	time_t 		t_val = 0;
	struct tm 	tm_res;;
	struct tm	*c_tm = NULL;
	struct tm	*ptm_local = NULL;
	/* time */
	t_val 	= time(NULL);

	if(ctx->req->config->log_gmt_time) { /* gmt time */
		c_tm	= gmtime_r(&t_val, &tm_res);
	}
	else {  /* local time */
		c_tm	= localtime_r(&t_val, &tm_res);
	}
	if (c_tm) {
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%4d-%02d-%02dT%02d:%02d:%02d%c%02i:%02i",
	                       c_tm->tm_year+1900, c_tm->tm_mon+1,
						   c_tm->tm_mday, c_tm->tm_hour,
						   c_tm->tm_min, c_tm->tm_sec,
						   c_tm->tm_gmtoff < 0 ? '-' : '+',
	                       abs(c_tm->tm_gmtoff / 3600), abs(c_tm->tm_gmtoff % 3600));
		//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
		ctx->remained 	-= n;
		ctx->offset 	+= n;
		//TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "URL[%s], header => ('%s', '%s')\n", req->url, MHD_HTTP_HEADER_LAST_MODIFIED, time_string));
	}
	else {
		//fprintf(stderr, "[%c] => ??\n",  x);
		r = -1;
	}
}
int m(fmt_context_t *ctx)
{
	int 		n;
	/* requested method */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", (char *)vs_data(ctx->req->zmethod));
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int O(fmt_context_t *ctx)
{
	int 		n;
	/* 요청과 헤더를 포함한 송신 바이트 수 */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%lld", ctx->req->res_hdr_size + ctx->req->res_body_size);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int P(fmt_context_t *ctx)
{
	int 		n;
	/* Client Unique ID */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%llu", ctx->req->id);

	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int p(fmt_context_t *ctx)
{
	int 		n;

	if (ctx->symbol) {
		char 	*psym = fmt_get_buffer(ctx->symbol);
		if (strcasecmp(psym, "remote") == 0)
			n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%ld", (long)ctx->req->client_port);
		else if (strcasecmp(psym, "local") == 0)
			n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%ld", (long)ctx->req->server_port);
		else
			n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "-");
	}
	else {
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "-");
	}
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int q(fmt_context_t *ctx)
{
	int 		n;
	/* the query string prepended with a '?' */
	char 	*pparam = NULL;
	pparam = vs_strchr_lg(ctx->req->url, '?');
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", (char *)(pparam?pparam:"-"));
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int r(fmt_context_t *ctx)
{	/* 요청 URL(method와 Protocol 버전 포함)을 출력 */
	int 		n;
	nc_request_t 	*tr = ctx->req ;
	/* example : GET /url HTTP/1.1 */

#if 1
	n = scx_snprintf(&ctx->buffer[ctx->offset], (4096<ctx->remained)?4096:ctx->remained, "%s %s %s",
			(tr->zmethod?(char *)vs_data(tr->zmethod):""), (tr->url?(char *)vs_data(tr->url):"") , (tr->version?(char *)vs_data(tr->version):""));
#else
	n = scx_snprintf(&ctx->buffer[ctx->offset], (4096<ctx->remained)?4096:ctx->remained, "%s", (ctx->req->first?(char *)vs_data(ctx->req->first):""));
#endif
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int s(fmt_context_t *ctx)
{
	int 		n;
	/* server response code */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%d", ctx->req->resultcode);
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

/*
 * ssl version + cipher suite info
 * 예 : "TLS1.2 ECDHE_RSA_AES_128_GCM_SHA256", SSL이 아닌 경우 "-"
 */
int S(fmt_context_t *ctx)
{
	int 		n;
	tls_info_t *tls_info = NULL;
	tls_info = ctx->req->tls_info;
	if (tls_info == NULL) {
		/* ssl을 사용하지 않은 경우 */
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "-");
	}
	else {
		/* ssl을 사용한 경우 */
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s %s",
									gnutls_protocol_get_name(tls_info->proto_ver),
									gnutls_cipher_suite_get_name(tls_info->kx_algorithm, tls_info->cipher_algorithm, tls_info->mac_algorithm)
									);
	}
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

int T(fmt_context_t *ctx)
{	/* client의 request를 받아서 response처리를 끝내기까지의 시간, 초단위 */
	int 		n;
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%.2f", (float)((ctx->req->t_res_compl - ctx->req->t_req_fba)/1000000.0));
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int t(fmt_context_t *ctx)
{
	int 		n, r = 0;
	time_t 		t_val = 0;
	struct tm 	tm_res;;
	struct tm	*c_tm = NULL;
	struct tm	*ptm_local = NULL;
	char 		*ptime_fmt = NULL;
	/* time */
	t_val 	= time(NULL);

//	if(gscx__config->log_gmt_time){
	if(ctx->req->config->log_gmt_time) { /* gmt time */
		ptime_fmt = GMT_DATE_FORMAT;
		c_tm	= gmtime_r(&t_val, &tm_res);
	}
	else {  /* local time */
		ptime_fmt = LOCAL_DATE_FORMAT;
		c_tm	= localtime_r(&t_val, &tm_res);
	}
	if (c_tm) {
		if (ctx->symbol) ptime_fmt = fmt_get_buffer(ctx->symbol);
		n = strftime(&ctx->buffer[ctx->offset], ctx->remained, ptime_fmt, c_tm);
		//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
		ctx->remained 	-= n;
		ctx->offset 	+= n;
		//TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "URL[%s], header => ('%s', '%s')\n", req->url, MHD_HTTP_HEADER_LAST_MODIFIED, time_string));
	}
	else {
		//fprintf(stderr, "[%c] => ??\n",  x);
		r = -1;
	}
}

int U(fmt_context_t *ctx)
{
	int 		n;
	/* request URL path */
	n = scx_snprintf(&ctx->buffer[ctx->offset], ((4096<ctx->remained)?4096:ctx->remained), "%s", vs_data(ctx->req->url));
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

int v(fmt_context_t *ctx)
{
	int 		n;
	/* host 명*/
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", vs_data(ctx->req->config->host_name));
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
int x(fmt_context_t *ctx)
{
	int 		n;
	char 		zcc[128];
	/* hit status */
#ifdef ZIPPER
		/* 스트리밍에서는 캐시 상태가 필요 없음 */
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "-");
#else
		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", (char *)hlf_get_hit(ctx->req, zcc, sizeof(zcc)));
#endif
	//fprintf(stderr, "[%c] => %s\n",  x, &ctx->buffer[ctx->offset]);
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}
/* Client의 연결 종료 상태 */
int X(fmt_context_t *ctx)
{
	int 		n;
	char 		zcc[128];
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%s", (char *)hlf_get_toe(ctx->req, zcc, sizeof(zcc)));
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

int z(fmt_context_t *ctx)
{	/* zipper build에 사용된 시간, 초단위 */
	int 		n;
	double 		sum = 0;
	sum = ctx->req->t_zipper_build;
	n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "%.2f", (float)(sum/1000000.0));
	ctx->remained 	-= n;
	ctx->offset 	+= n;
}

typedef int (*fmt_handler_t)(fmt_context_t *ctx);
typedef struct fmt_handler_table_tag {
	char 		x;
	fmt_handler_t 	func;
} fmt_handler_table_t;

fmt_handler_table_t handler_list[] = {
	{'a', &a},{'A', &A},{'B', &B},{'b', &b},{'c', &c},{'C', &C},{'D', &D},{'E', &E},{'f', &f},{'F', &F},{'h', &h},{'H', &H},
	{'i', &i},{'I', &I},{'l', &l},{'m', &m},{'O', &O},{'P', &P},{'p', &p},{'q', &q},{'r', &r},{'s', &s},{'S', &S},{'t', &t},
	{'T', &T},{'U', &U},{'v', &v},{'x', &x},{'X', &X},{'z', &z}
};
fmt_handler_t handler_vector[128] = {0};
#define 	LOG_FMT_LIST_COUNT 		howmany(sizeof(handler_list), sizeof(fmt_handler_table_t))

void init_log_format()
{
	int i;
	for(i = 0; i < LOG_FMT_LIST_COUNT; i++) {
		handler_vector[handler_list[i].x] = handler_list[i].func;
	}
}

static int
hlf_handle_quark(fmt_context_t *ctx, char x) {
	int 		httpcode;
	char 		zhttpcode[32];
	char 		zcc[128];
	char 		*	pcond;
	int 		neg = 0;
	int 		condition_asserted = 1;
	time_t 		t_val = 0;
	int 		n, r = 0;
	struct tm 	tm_res;;
	struct tm	*c_tm = NULL;
	struct tm	*ptm_local = NULL;
	char 		*ptime_fmt = NULL;



	/* check condition if exist */
	if (ctx->condition) {
		httpcode = ctx->req->resultcode;
		sprintf(zhttpcode, "%d", httpcode);
		pcond = fmt_get_buffer(ctx->condition);
		if (*pcond == '!') {
			neg++;
			pcond++;
		}
		if(strstr(pcond, zhttpcode)) 
			condition_asserted = (neg?0:1);
		else
			condition_asserted = (neg?1:0);
	}
	if (!condition_asserted)  {

		n = scx_snprintf(&ctx->buffer[ctx->offset], ctx->remained, "-");
		ctx->remained 	-= n;
		ctx->offset 	+= n;
		return 0; /* 조건 불만족, 출력 생력 */
	}
	ASSERT(handler_vector[0] == 0);	//handler_vector가 변경되는 시점을 확인하기 위해 코드 추가
	if(handler_vector[x]) {
		handler_vector[x](ctx);
	}
	ASSERT(handler_vector[0] == 0);	//handler_vector가 변경되는 시점을 확인하기 위해 코드 추가
	/*
	 * cleanup context 
	 */
	ctx->buffer[ctx->offset] = 0;
	if (ctx->symbol) {
		fmt_string_destroy(ctx->symbol);
		ctx->symbol = NULL;
	}
	if (ctx->condition) {
		fmt_string_destroy(ctx->condition);
		ctx->condition = NULL;
	}
	return r;
}

int
hlf_log(char *buf, int buflen, const char *fmt, nc_request_t *req)
{
	fmt_context_t 	ctx = {S_FMT_INIT, buf, buflen, 0, fmt, 0};
	int 			r;


	ctx.req = req;
	ctx.offset 		= 0;
	while (*fmt != 0) {
		r =  __proc_dict[ctx.state](&ctx, *fmt);
		if (r < 0) {
			fprintf(stderr, "error at '%c'\n", *fmt);
			break;
		}
		fmt++;
	}
	ctx.buffer[ctx.offset++] = '\n';
	ctx.buffer[ctx.offset] = '\0';
	return ctx.offset;
}

