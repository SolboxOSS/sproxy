#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <memory.h>
//#include <ncapi.h>

#include <microhttpd.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "streaming.h"
#include "scx_util.h"
#include "scx_list.h"
#include "md5.h"
#include "module.h"
#include "soluri2.h"


module_driver_t gscx__soluri2_module = {
	.name 				= "soluri2",	/* name */
	.desc				= "soluri2 authorization module",	/* description */
	.version			= 1,				/* version */
	.init				= soluri2_module_init,	/* init func */
	.deinit				= soluri2_module_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};

#if 0
int soluri2_module_volume_lookup(phase_context_t *ctx);
#else
int soluri2_module_client_request(phase_context_t *ctx);
#endif
int soluri2_module_content_verify(phase_context_t *ctx);
static soluri2_nv_t *soluri2_find_nv_by_idx(nc_request_t *req, int idx, struct tag_scx_list *key_array);
static char *soluri2_get_str_by_idx(int idx);
static int soluri2_validator(nc_request_t *req);
static int soluri2_parsing_request(nc_request_t *req, soluri2_context_t *ctx);
static soluri2_context_t *soluri2_context_init(nc_request_t *req);
static inline int soluri2_context_free(nc_request_t *req, soluri2_context_t *ctx);
static int soluri2_check_expire(nc_request_t *req, soluri2_context_t *ctx);
static int soluri2_check_version(nc_request_t *req, soluri2_context_t *ctx);
static int soluri2_generate_path(nc_request_t *req, soluri2_context_t *ctx);
static char * soluri2_http_arg(nc_request_t *req, soluri2_context_t *ctx, char *name);
static int soluri2_processing_tokenrule(nc_request_t *req, soluri2_context_t *ctx);
static int soluri2_decode_token_rule(nc_request_t *req, soluri2_context_t *ctx);
static int soluri2_parse_token_value(nc_request_t *req, soluri2_context_t *ctx);
static int soluri2_is_token_secure(nc_request_t *req, soluri2_context_t *ctx, char *secret);
static int soluri2_parse_stoken(nc_request_t *req, soluri2_context_t *ctx, char *stoken);


extern time_t			gscx__start_time;

static struct {
	int idx;
	int	required;		/* 필수 파라미터인 경우 1, 생략 가능한 파라미터인 경우 0 */
	char str[20];
} g_reserved_keys[] = {
	{ E_SOLURI2_SOLSTOKEN, 		0,	SOLURI2_SOLSTOKEN },
	{ E_SOLURI2_SOLTOKEN, 		1,	SOLURI2_SOLTOKEN },
	{ E_SOLURI2_SOLTOKENRULE, 	1, 	SOLURI2_SOLTOKENRULE },
	{ E_SOLURI2_SOLEXPIRE, 		0,	SOLURI2_SOLEXPIRE },
	{ E_SOLURI2_SOLUUID, 		0, 	SOLURI2_SOLUUID },
	{ E_SOLURI2_SOLPATH, 		0,	SOLURI2_SOLPATH },
	{ E_SOLURI2_SOLURIVER, 		0,	SOLURI2_SOLURIVER },
	{ E_SOLURI2_SOLPATHLEN, 	0,	SOLURI2_SOLPATHLEN },
	{ E_SOLURI2_TOKEN_MAX, 		0,	'\0' }
};


int
soluri2_init()
{
	return SCX_YES;
}

int
soluri2_deinit()
{

	return SCX_YES;
}


int
soluri2_module_init()
{
	int ret = 0;
	ret = soluri2_init();
	if (SCX_YES != ret) return ret;
#if 0
	scx_reg_phase_func(PHASE_VOLUME_LOOKUP, soluri2_module_volume_lookup);
#else
	scx_reg_phase_func(PHASE_VOLUME_LOOKUP, soluri2_module_client_request);
#endif
	scx_reg_phase_func(PHASE_CONTENT_VERIFY, soluri2_module_content_verify);

	return SCX_YES;
}

int
soluri2_module_deinit()
{

	soluri2_deinit();

	return SCX_YES;
}
#if 0
int
soluri2_module_volume_lookup(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;

	if(NULL == req->service->soluri2_secret)
	{
		/* soluri 인증을 사용하지 않는 경우 */
		return SCX_YES;
	}
	if (SCX_YES != soluri2_handler(req)) {
		scx_error_log(req, "URL[%s] - soluri2 secure check failed.\n", vs_data(req->url), vs_data(req->host));
		if (!req->p1_error) req->p1_error = MHD_HTTP_UNAUTHORIZED;
		return SCX_NO;
	}
	return SCX_YES;
}
#else
/*
 * lua client_request phase handler script를 통해서 특정 컨텐츠에 대한 무인증이 가능하도록 하기 위해서
 * PHASE_VOLUME_LOOKUP에서 PHASE_CLIENT_REQUEST phase로 변경
 */
soluri2_module_client_request(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;
	if (req->method != HR_GET && req->method != HR_HEAD) {
		/* 인증은 HEAD와 GET method일때만 한다. */
		return SCX_YES;
	}
	if(NULL == req->service->soluri2_secret || 1 == req->skip_auth)
	{
		/* soluri 인증을 사용하지 않는 경우 */
		return SCX_YES;
	}
	if (SCX_YES != soluri2_handler(req)) {
		scx_error_log(req, "URL[%s] - soluri2 secure check failed.\n", vs_data(req->url), vs_data(req->host));
		if (!req->p1_error) req->p1_error = MHD_HTTP_UNAUTHORIZED;
		return SCX_NO;
	}
	return SCX_YES;
}
#endif
int
soluri2_module_content_verify(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;
	if(NULL == req->service->soluri2_secret)
	{
		/* soluri 인증을 사용하지 않는 경우 */
		return SCX_YES;
	}
	if (1 == req->expired) {
		scx_error_log(req, "Request has been expired\n");
		if (!req->p1_error) req->p1_error = MHD_HTTP_UNAUTHORIZED;
	}
	return SCX_YES;
}

static char *
soluri2_get_str_by_idx(int idx)
{
	int i;

	for( i = 0; i < E_SOLURI2_TOKEN_MAX || g_reserved_keys[i].str[0] != '\0'; i ++ )
	{
		if( g_reserved_keys[i].idx == idx )
			return g_reserved_keys[i].str;
	}
	return g_reserved_keys[E_SOLURI2_TOKEN_MAX].str;
}

static soluri2_nv_t *
soluri2_find_nv_by_idx(nc_request_t *req, int idx, struct tag_scx_list *key_array)
{
	int i;
	soluri2_nv_t *ptr = NULL;
	int	array_size = 0;
	char *name = NULL;
	ASSERT(idx > E_SOLURI2_UNKNOWN  && idx <= E_SOLURI2_TOKEN_MAX  );
	ASSERT(key_array);
	name = soluri2_get_str_by_idx(idx);
	array_size = scx_list_get_size(key_array);
	if( array_size <= 0 ) {
		scx_error_log(req, "Empty soluri2 array, url(%s)\n", vs_data(req->url));
		return NULL;
	}
	ptr = (soluri2_nv_t *)scx_list_find_data(key_array, name, 0);
	if (NULL == ptr) {
		TRACE((T_DEBUG, "[%llu] Empty soluri2 array.\n", req->id));
	}
	return ptr;
}

/*
 * soluri2 모듈에서 uri 파싱 및 해석, 검증을 위해 사용하는 컨텍스트를 초기화한다.
 */
static soluri2_context_t *
soluri2_context_init(nc_request_t *req)
{
	soluri2_context_t *ctx;
	mem_pool_t *mpool = NULL;
	mpool = mp_create(gscx__config->pool_size);	/* request context의 memory pool 크기와 동일한 pool을 할당한다. */
	ASSERT(mpool);
	ctx =  mp_alloc(mpool, sizeof(soluri2_context_t));
	ASSERT(ctx);
	ctx->mpool = mpool;
	ctx->soluriver = -1;
	ctx->soltoken = NULL;
	ctx->rule_string = NULL;
	ctx->path = NULL;
	ctx->args = NULL;
	ctx->token_item = scx_list_create(ctx->mpool, LIST_TYPE_KEY_DATA, 0);
	ctx->rule_item = scx_list_create(ctx->mpool, LIST_TYPE_KEY_DATA, 0);

	return ctx;
}

static inline int
soluri2_context_free(nc_request_t *req, soluri2_context_t *ctx)
{
	if (ctx) mp_free(ctx->mpool);

	return SCX_YES;
}

int
soluri2_handler(nc_request_t *req)
{
	streaming_t *streaming = req->streaming;
	if (streaming) {
		if ((req->streaming->protocol & O_PROTOCOL_CORS) != 0) {
			/* crossdomain.xml요청은 인증을 하지 않는다. */
			return SCX_YES;
		}
	}
	if (SCX_YES != soluri2_validator(req)) {
		return SCX_NO;
	}
	return SCX_YES;
}

static int
soluri2_validator(nc_request_t *req)
{
	int i;
	soluri2_context_t *ctx = NULL;
	int count = 0;
	int	ret = SCX_YES;
	char *secret = NULL;

	TRACE((T_DEBUG, "[%llu] soluri2 enabled.\n", req->id));

	ctx = soluri2_context_init(req);
	if(NULL == ctx) {
		TRACE((T_ERROR, "[%llu] Failed to initialized SolURI2 context(%s).\n", req->id, vs_data(req->url) ));
		/* FIXME: 여기서 모듈이 실패하면 답이 없다!!! T^T */
		goto soluri2_handler_error;
	}

	if(soluri2_parsing_request(req, ctx) == SCX_NO) {

		goto soluri2_handler_error;
	}

	/*
	 * 현재 로직상 실패할 이유는 없는 함수지만...차후를 위해...
	 */
	if( soluri2_check_version(req, ctx) == SCX_NO )	{
		scx_error_log(req, "Request was invalid SolURI2 version, don't know how to process.\n");
		goto soluri2_handler_error;
	}
	if( soluri2_generate_path(req, ctx) == SCX_NO)	{
		scx_error_log(req, "Secure contents path of request was invaild.");
		goto soluri2_handler_error;
	}
	if( soluri2_processing_tokenrule(req, ctx) == SCX_NO )	{
		scx_error_log(req, "Request has invalid SolURI2 syntax.");

		goto soluri2_handler_error;
	}

	ret = SCX_NO;
	count = scx_list_get_size(req->service->soluri2_secret);
	for( i = 0; i < count; i++ )	{
		secret = scx_list_get_by_index_key(req->service->soluri2_secret, i);
		TRACE((T_DEBUG, "[%llu] SolURI2 secret key[%d]: %s\n", req->id, i, secret));

		if( soluri2_is_token_secure(req, ctx, secret) == SCX_YES ) {
			TRACE((T_DEBUG, "[%llu] token is secure.\n", req->id));
			ret = SCX_YES;
			break;
		}
	}
	if (SCX_NO == ret) {
		scx_error_log(req, "Token is not secure. url(%s)\n", vs_data(req->url));
		goto soluri2_handler_error;
	}
	/*
	 * 컨텐츠 만료시 단순 에러가 아니라 session 기능과 연동을 위해서
	 * 만료 검사는 마지막에 한다.
	 */
	if(soluri2_check_expire(req, ctx) == SCX_NO) {
//		scx_error_log(req, "Request has been expired.\n");
		TRACE((T_DEBUG, "[%llu] Request has been expired.\n", req->id));
		goto soluri2_handler_error;
	}
	TRACE((T_DEBUG, "[%llu] Request was secured.\n", req->id));
	ret = SCX_YES;
	goto soluri2_handler_success;
soluri2_handler_error:
	ret = SCX_NO;
soluri2_handler_success:
	if (ctx) {
		soluri2_context_free(req, ctx);
	}
	return ret;

}

static int
soluri2_check_version(nc_request_t *req, soluri2_context_t *ctx)
{
	soluri2_nv_t *ptr = NULL;
	ASSERT(req && ctx);
	if( ctx->soluriver >= 2 )
	{
		TRACE((T_DEBUG, "[%llu] Valid request for SolURI 2.%d\n", req->id, ctx->soluriver));
		return SCX_YES;
	}
	ptr = soluri2_find_nv_by_idx(req, E_SOLURI2_SOLSTOKEN, ctx->token_item);
	if( ptr )
	{
		/*
		 * soluriver이 존재하지 않지만, solstoken이 존재하면 SolURI 2.1
		 */
		ctx->soluriver = 1;
		TRACE((T_DEBUG, "[%llu] Valid request for SolURI 2.1\n", req->id));
		return SCX_YES;
	}
	/*
	 * 파싱에는 성공했으므로, 여기까지 도달하면 SolURI 2.0으로 간주한다.
	 */
	ctx->soluriver = 0;
	TRACE((T_DEBUG, "[%llu] Valid request for SolURI 2.0\n", req->id));

	return SCX_YES;
}
/*
 * 요청을 파싱하여 soluri2_context_t의 token_item를 채운다.
 */
static int
soluri2_parsing_request(nc_request_t *req, soluri2_context_t *ctx)
{
	int i;
	char *buf = NULL, *decode = NULL;
	soluri2_nv_t *ptr = NULL;
	ASSERT(req && ctx);
	for( i = 0; i < E_SOLURI2_TOKEN_MAX || g_reserved_keys[i].str[0] != '\0'; i++ ) {
#if 0
		buf = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, g_reserved_keys[i].str);
#else
		buf = soluri2_http_arg(req, ctx, g_reserved_keys[i].str);
#endif
		if(NULL == buf)	{
			if (1 == g_reserved_keys[i].required) { /* 필수 파라미터가 없는 경우는 에러 처리한다. */
				scx_error_log(req, "Unable to found HTTP argument '%s' on request query.\n", g_reserved_keys[i].str);
				return SCX_NO;
			}
			TRACE((T_DEBUG, "[%llu] Unable to found HTTP argument '%s' on request query.\n", req->id, g_reserved_keys[i].str));
		}
		else
		{
			switch( g_reserved_keys[i].idx )
			{
				case E_SOLURI2_SOLURIVER:
					ctx->soluriver = atoi(buf);
					break;
				case E_SOLURI2_SOLSTOKEN:
				case E_SOLURI2_SOLTOKENRULE:
					/*
					 * base64 인코딩된 인자는 디코딩한 값을 저장한다.
					 */
					decode = mp_alloc(ctx->mpool, BASE64_DECODED_LENGTH(strlen(buf)));

					if( scx_base64_decode(decode, buf) == SCX_NO )
					{
						scx_error_log(req, "Failed to decoded base64: '%s:%s'.\n",
								g_reserved_keys[i].str, buf);
						return SCX_NO;
					}
					TRACE((T_DEBUG, "[%llu] '%s' (base64): '%s', (decoded): '%s'\n",
										req->id, g_reserved_keys[i].str, buf, decode));
					buf = decode;
					/*
					 * solstoken의 경우, context의 args는 solstoken을 디코딩한 값으로 지정
					 */
					if( g_reserved_keys[i].idx == E_SOLURI2_SOLSTOKEN )
					{
						if (soluri2_parse_stoken(req, ctx, buf) == SCX_NO) {
							scx_error_log(req, "Invalid SOLSTOKEN(%s).\n", buf);
							return SCX_NO;
						}
					}
					break;
				case E_SOLURI2_SOLTOKEN:
					/* 편의상 두군데에 파싱함, 아래 부분에서 중복 메모리 할당이 필요한지 모르겠음 */
#if 1
					ctx->soltoken = buf;
#else
					ctx->soltoken = mp_alloc(ctx->mpool, strlen(buf)+1);
					strcpy(ctx->soltoken, buf);
#endif

					break;
				default:
					break;
			} /* end of switch */
			ptr =  mp_alloc(ctx->mpool, sizeof(soluri2_nv_t));
			ptr->idx = g_reserved_keys[i].idx;
			ptr->name =  mp_alloc(ctx->mpool, strlen(g_reserved_keys[i].str)+1);
			strcpy(ptr->name, g_reserved_keys[i].str);
#if 1			/* MHD_lookup_connection_value()에서 리턴된 값을 복사하지 않고 그냥 사용해도 될듯함. */
			ptr->value = buf;
#else
			if (E_SOLURI2_SOLSTOKEN == g_reserved_keys[i].idx ||
					E_SOLURI2_SOLTOKENRULE == g_reserved_keys[i].idx) {
				ptr->value = buf; /* 이 경우는 위에서 memory pool에서 할당한 값이기 때문에 복사할 필요가 없다. */
			}
			else {
				ptr->value =  mp_alloc(ctx->mpool, strlen(buf)+1);
				strcpy(ptr->value, buf);
			}
#endif
			scx_list_append(ctx->token_item, ptr->name, (void *)ptr, 0);
			TRACE((T_DEBUG, "[%llu] '%s' exist on query, value: '%s'\n",
					req->id, ptr->name, ptr->value));
		}

	} /* end of g_reserved_keys for loop  */
	TRACE((T_DEBUG, "[%llu] Total SolURI2 arguments count: %d\n", req->id, scx_list_get_size(ctx->token_item)));
	return SCX_YES;
}

/*
 * stoken에 들어 있는 argument를 파싱한다.
 * soluri2_http_arg() 호출 이전에 stoken의 base64 디코딩을 먼저 해야한다.
 * stoken은 아래의 형태로 구성 되어 있다.
 * solstoken=base64(solexpire=a&solpath=b&soluuid=c&soltokenrule=d)

 */
static int
soluri2_parse_stoken(nc_request_t *req, soluri2_context_t *ctx, char *stoken)
{
	char *ptok1 = NULL;
	char *key = NULL, *value = NULL;
	char *saveptr1 = NULL, *saveptr2 = NULL;
	char *stoken_string = NULL;
	stoken_string = mp_alloc(ctx->mpool, strlen(stoken)+1);
	strcpy(stoken_string, stoken);
	ctx->args = scx_list_create(ctx->mpool, LIST_TYPE_KEY_DATA, 0);
	ptok1 = strtok_r(stoken_string, "&", &saveptr1);
	while (ptok1) {

		key = strtok_r(ptok1, "=", &saveptr2);
		value = strtok_r(NULL, "=", &saveptr2);
		if(!key || !value) {
			/* key/value의 형태가 아닌 경우 에러 처리한다. */
			return SCX_NO;
		}
		scx_list_append(ctx->args, key, (void *)value, 0);
		ptok1 = strtok_r(NULL, "&", &saveptr1);
	}
	return SCX_YES;

}

/*
 * stolen from ngx_http_parse.c:ngx_http_arg()
 *  soluri2_context_t.args를 파싱하도록 ngx_http_arg()를 변경한 함수
 */
static char *
soluri2_http_arg(nc_request_t *req, soluri2_context_t *ctx, char *name)
{
	char  *p, *last;
	char *value = NULL;

	if (ctx->args) {
		/*
		 * solstoken이 정의 되어 있는 경우는 solstoken 파싱한 결과를 가지고 있는 ctx->args의 값만을 사용한다.
		 */
		value = (char *)scx_list_find_data(ctx->args, name, 0);

	}
	else {
		/*
		 * solstoken을 사용하는 경우를 제외하고는
		 * MHD_lookup_connection_value()를 사용해서 argument를 읽는다.
		 */
		value = (char *)MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, name);

	}

	return value;
}

/*
 * soluri2 context와 요청 메시지를 이용하여, 인증할 경로를 구한다.
 */
static int
soluri2_generate_path(nc_request_t *req, soluri2_context_t *ctx)
{
	soluri2_nv_t *ptr = NULL;
	char *path_token = NULL;
	char *buf = NULL;
	char *mpsn = NULL;
	char *url_path = NULL;
	int path_len = 0;

	ASSERT(req->path);
	url_path = vs_data(req->path);
	switch( ctx->soluriver ) {
		case 0:
		case 1:	/* solpath는 버전 2.1까지만 지원 되고 2.2부터는 지원 안함 */
			path_token = soluri2_get_str_by_idx(E_SOLURI2_SOLPATH);
			ctx->path = soluri2_http_arg(req, ctx, path_token);

			path_len = strlen(url_path);
			/*
			 * path 는 url 앞쪽의 "/"를 제외한 나머지 경로이다.
			 */
			for(; url_path[0] == '/' && path_len-- > 0; url_path++)
				;

			if(NULL == ctx->path)	{
				/* argument에 solpath가 없는 경우는 요청 url을 사용한다. */
				ctx->path = url_path;
				TRACE((T_DEBUG, "[%llu] Unable to found '%s' from argument of SolURI2 context. Secure contents path: '%s'\n", req->id, path_token, ctx->path));
			}
			else
			{
				TRACE((T_DEBUG, "[%llu] Found '%s' on SolURI2 context. Secure contents path: '%s'\n",
						req->id, path_token, ctx->path));
				/* argument에 solpath가 있는 경우 요청 url의 앞쪽 경로와 동일한지 비교 한다.*/
				if ( 0 != strncmp(url_path, ctx->path, strlen(url_path)) ) {
					scx_error_log(req, "Secure contents path is not matched, %s(%s), url(%s)\n", path_token, ctx->path, vs_data(req->url));
					return SCX_NO;
				}

			}

			break;
		case 2:
		default:
			path_token = soluri2_get_str_by_idx(E_SOLURI2_SOLPATHLEN);
			ptr = soluri2_find_nv_by_idx(req, E_SOLURI2_SOLPATHLEN, ctx->token_item);
			if( !ptr )
			{	/* solpathlen을 사용하지 않는 경우, 전체 경로를 사용한다. */
				ctx->path = url_path;
				TRACE((T_DEBUG, "[%llu] Unable to found '%s' from argument of SolURI2 context. Secure contents path '%s'\n", req->id, path_token, ctx->path));
			}
			else
			{

				/* solpathlen을 사용하는 경우 */
				path_len = atoi(ptr->value);
				ctx->path = mp_alloc(ctx->mpool, path_len+1);
				strncpy(ctx->path, url_path, path_len);
				ctx->path[path_len] = '\0';
				TRACE((T_DEBUG, "[%llu] Found '%s' from argument of SolURI2 context. Secured contents path: '%s'\n", req->id, path_token, ctx->path));
			}
			break;
	}/* end of switch( ctx->soluriver ) */

	return SCX_YES;
}

/*
 * soltokenrule의 값들 '|' 기준으로 파싱하여 rule_item에 soluri2_nv_t.name을 추가한다.
 */
static int
soluri2_decode_token_rule(nc_request_t *req, soluri2_context_t *ctx)
{
	soluri2_nv_t *tokenrule = NULL;
	char *ptok = NULL;
	char *saveptr = NULL;
	char *rule_string = NULL;
	soluri2_nv_t *ptr = NULL;

	tokenrule = soluri2_find_nv_by_idx(req, E_SOLURI2_SOLTOKENRULE, ctx->token_item);
	if( !tokenrule ) {
		/*
		 * 여기에 들어오는 경우는 예상치 못한 논리 오류임.
		 * ASSERT를 넣을까 고민 하다 일단 에러만 출력함
		 */
		TRACE((T_ERROR, "[%llu] Unable to found mandatory argument '%s' on SolURI2 context.\n",
				req->id, SOLURI2_SOLTOKENRULE));
		scx_error_log(req, "Unable to found mandatory argument '%s' on SolURI2 context.\n", SOLURI2_SOLTOKENRULE);
		return SCX_NO;
	}
	rule_string = mp_alloc(ctx->mpool, strlen(tokenrule->value)+1);
	strcpy(rule_string, tokenrule->value);
	ptok = strtok_r(rule_string, SOLURI2_SOLTOKENRULE_DELIM, &saveptr);
	while (ptok) {
		ptr =  mp_alloc(ctx->mpool, sizeof(soluri2_nv_t));
		ptr->idx = 0;
		ptr->name = ptok;
		ptr->value = NULL;
		scx_list_append(ctx->rule_item, ptok, (void *)ptr, 0);
		TRACE((T_DEBUG, "[%llu] %s name: '%s' \n", req->id, tokenrule->name, ptok));
		ptok = strtok_r(NULL, SOLURI2_SOLTOKENRULE_DELIM, &saveptr);
	}

	return SCX_YES;
}
/*
 * soluri2 context를 뒤져서 rule_item에 soluri2_nv_t.value를 추가한다.
 */
static int
soluri2_parse_token_value(nc_request_t *req, soluri2_context_t *ctx)
{
	int i;
	int length = 0;
	soluri2_nv_t *item = NULL;
	int count = scx_list_get_size(ctx->rule_item);
	for (i = 0; i < count; i++) {
		item = (soluri2_nv_t *) scx_list_get_by_index_data(ctx->rule_item,i);
		if (strcmp(item->name, SOLURI2_SOLPATHLEN) == 0
				|| strcmp(item->name, SOLURI2_SOLPATH) == 0) {
			/* solpath나 solpathlen은 argument에서 찾지 않고 ctx->path를 사용한다. */
			item->value = ctx->path;
			TRACE((T_DEBUG, "[%llu] Found argument '%s', use URI(%s)\n", req->id, item->name, item->value));
		}
		else {
			item->value = soluri2_http_arg(req, ctx, item->name);
			if (NULL == item->value) {
				scx_error_log(req, "HTTP query argument '%s' does not exist\n", item->name);
				return -1;
			}
			else {
				TRACE((T_DEBUG, "[%llu] Found argument '%s', use URI(%s)\n", req->id, item->name, item->value));
			}
		}
		length += strlen(item->value);
	}
	TRACE((T_DEBUG, "[%llu] Total length of '%s' items value: %d\n", req->id, SOLURI2_SOLTOKENRULE, length));
	return length;
}

static int
soluri2_processing_tokenrule(nc_request_t *req, soluri2_context_t *ctx)
{
	int i;
	int length = 0;
	soluri2_nv_t *item = NULL;
	int count = 0;
	char *soltokenrule = soluri2_get_str_by_idx(E_SOLURI2_SOLTOKENRULE);

	if ( soluri2_decode_token_rule(req, ctx) == SCX_NO )	{
		scx_error_log(req, "Failed to parsing '%s'\n", soltokenrule);
		return SCX_NO;
	}
	length = soluri2_parse_token_value(req, ctx);
	if ( length <= 0 )	{
		scx_error_log(req, "Failed to parsing arguments of '%s'\n", soltokenrule);
		return SCX_NO;
	}

	ctx->rule_string = mp_alloc(ctx->mpool, length+1);
	ASSERT(ctx->rule_string);
	count = scx_list_get_size(ctx->rule_item);
	for (i = 0; i < count; i++) {
		item = (soluri2_nv_t *) scx_list_get_by_index_data(ctx->rule_item,i);
		ASSERT(item);
		strcat(ctx->rule_string, item->value);
	}
	TRACE((T_DEBUG, "[%llu] Generated token items string: '%s'\n", req->id, ctx->rule_string));
	return SCX_YES;
}


/* md5라이브리는 MHD에 있는것을 사용한다. */
static int
soluri2_is_token_secure(nc_request_t *req, soluri2_context_t *ctx, char *secret)
{
	struct MD5Context md5;
	char digest[16], *token = NULL;
	char hash_hexadecimal[33];
	int token_len = 0;

	token_len = strlen(secret) + strlen(ctx->rule_string);
	token = mp_alloc(ctx->mpool, token_len+1);
	ASSERT(token);
	snprintf(token, token_len+1, "%s%s", secret, ctx->rule_string);
	TRACE((T_DEBUG, "[%llu] generated string: '%s'\n", req->id, token));

	MD5Init(&md5);
	MD5Update(&md5, token, token_len);
	MD5Final(digest, &md5);
	ascii2hex(hash_hexadecimal, digest, 16);

	hash_hexadecimal[32] = '\0';
	TRACE((T_DEBUG, "[%llu] '%s': '%s', genereated token: '%s'\n", req->id, SOLURI2_SOLTOKEN, ctx->soltoken, hash_hexadecimal));

	if( strncmp(ctx->soltoken, hash_hexadecimal, token_len) == 0 )
	{
		TRACE((T_DEBUG, "[%llu] secured.\n", req->id));
		return SCX_YES;
	}
	scx_error_log(req, "secure token does not matched, '%s': '%s', generated token: '%s'\n", SOLURI2_SOLTOKEN, ctx->soltoken, hash_hexadecimal);

	return SCX_NO;
}

static int
soluri2_check_expire(nc_request_t *req, soluri2_context_t *ctx)
{
	soluri2_nv_t *solexpire = NULL;
	int expire;
	time_t 		cur_time;

	ASSERT(req && ctx);
	solexpire = soluri2_find_nv_by_idx(req, E_SOLURI2_SOLEXPIRE, ctx->token_item);
	if( !solexpire )
	{
		/*
		 * solexpire가 없는 경우는 유효시간이 없는 경우로 간주하여 허용 처리
		 */
		TRACE((T_DEBUG, "[%llu] HTTP query argument '%s' does not exist.\n", req->id, SOLURI2_SOLEXPIRE));
		return SCX_YES;
	}
	expire = atoi(solexpire->value);
	if( expire < 0 )
	{
		scx_error_log(req, "Failed to converting HTTP query argument '%s'\n", solexpire->value);
		return SCX_NO;
	}
	cur_time 	= time(NULL);
	TRACE((T_DEBUG, "[%llu] %s: %d current time: %d\n",
					req->id, SOLURI2_SOLEXPIRE, expire, cur_time));

	if( expire < cur_time )
	{
		if( 0 < req->service->auth_start_time
				&& (gscx__start_time + req->service->auth_start_time) > cur_time ) {
			/* 인증 시간을 넘겼더라도 서버가 재시작 하고 나서 일정 시간이 지나지 않은 경우는 인증 성공으로 한다. */
		}
		else {
			/* 인증이 만료가 된 경우 여기서 에러를 리턴하지 않고 다음 phase handler에서 req->expired의 값을 검사후 에러 처리를 한다.
			 * 이렇게 하는 이유는 중간에 세션 검사 로직이 들어가서 만료가 됬더라도 세션이 있는 경우 만료가 되지 않은 것으로 하기 위함임. */
			req->expired = 1;
	//		scx_error_log(req, "Request has been expired, %s: %d current time: %d\n",SOLURI2_SOLEXPIRE, expire, cur_time);
			TRACE((T_DEBUG, "[%llu] Request has been expired, %s: %d current time: %d\n",
							req->id, SOLURI2_SOLEXPIRE, expire, cur_time));
			return SCX_YES;
		}
	}
	TRACE((T_DEBUG, "[%llu] Request has not expired.\n",	req->id));

	return SCX_YES;
}

