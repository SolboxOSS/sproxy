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
#include "lib_solbox_jwt.h"
#include "soljwt.h"

/* 관련 개발 내역 : https://jarvis.solbox.com/redmine/issues/32472 참고 */


module_driver_t gscx__soljwt_module = {
	.name 				= "soljwt",	/* name */
	.desc				= "soljwt authorization module",	/* description */
	.version			= 1,				/* version */
	.init				= soljwt_module_init,	/* init func */
	.deinit				= soljwt_module_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};

int soljwt_module_volume_load(phase_context_t *ctx);
#if 0
int soljwt_module_volume_lookup(phase_context_t *ctx);
#else
int soljwt_module_client_request(phase_context_t *ctx);
#endif
int soljwt_module_content_verify(phase_context_t *ctx);
int soljwt_init_claim_validator(service_info_t *service);
int soljwt_change_claim_validator(service_info_t *service, vstring_t *claim_validator, vstring_t *claim_name, vstring_t *claim_opt);

int soljwt_claim_validator_exp(nc_request_t *req, char *claim, jwt_t *jwt, int opt);
int soljwt_claim_validator_path(nc_request_t *req, char *claim, jwt_t *jwt, int opt);
int soljwt_claim_validator_alowip(nc_request_t *req, char *claim, jwt_t *jwt, int opt);
int soljwt_claim_validator_alowcc(nc_request_t *req, char *claim, jwt_t *jwt, int opt);
int soljwt_claim_validator_duration(nc_request_t *req, char *claim, jwt_t *jwt, int opt);
int soljwt_claim_validator_playstart(nc_request_t *req, char *claim, jwt_t *jwt, int opt);

static int soljwt_validator(nc_request_t *req);
static soljwt_context_t *soljwt_context_init(nc_request_t *req);




extern time_t			gscx__start_time;

static struct {
	int idx;
	int	required;		/* 필수 파라미터인 경우 1, 생략 가능한 파라미터인 경우 0 */
	char str[20];
} g_reserved_keys[] = {
	{ E_SOLJWT_EXPIRE, 		1,	SOLJWT_EXPIRE },
	{ E_SOLJWT_PATH, 		1,	SOLJWT_PATH },
	{ E_SOLJWT_ALOWIP, 		0, 	SOLJWT_ALOWIP },
	{ E_SOLJWT_ALOWCC, 		0,	SOLJWT_ALOWCC },
	{ E_SOLJWT_DURATION, 	0, 	SOLJWT_DURATION },
	{ E_SOLJWT_PLAYSTART, 	0,	SOLJWT_PLAYSTART },
	{ E_SOLJWT_TOKEN_MAX, 	0,	'\0' }
};

typedef struct soljwt_claim_op_s
{
	int use;
	char *claim;
	int (*handler)(nc_request_t *req, char *claim, jwt_t *jwt, int opt);
	int opt;
} soljwt_claim_op_t;

enum opt_t {
	opt_unknown = -1,
	opt_deny_not_exist,
	opt_allow_not_exist, /* default */
	opt_allow_empty_value,
	opt_count
};

static soljwt_claim_op_t default_claim_validators[] = {
	{ TRUE, SOLJWT_EXPIRE,  soljwt_claim_validator_exp, opt_deny_not_exist },
	{ TRUE, SOLJWT_PATH,  soljwt_claim_validator_path, opt_deny_not_exist },
	{ FALSE, SOLJWT_ALOWIP,  soljwt_claim_validator_alowip, opt_allow_not_exist },
	{ FALSE, SOLJWT_ALOWCC,  soljwt_claim_validator_alowcc, opt_allow_not_exist },
	{ FALSE, SOLJWT_DURATION, soljwt_claim_validator_duration, opt_allow_not_exist },
	{ FALSE, SOLJWT_PLAYSTART, soljwt_claim_validator_playstart, opt_allow_not_exist },
	{ FALSE, NULL, NULL, -1 }
};

const int gscx__count_soljwt_claim_op = howmany(sizeof(default_claim_validators), sizeof(soljwt_claim_op_t));

int
soljwt_init()
{
	return SCX_YES;
}

int
soljwt_deinit()
{

	return SCX_YES;
}


int
soljwt_module_init()
{
	int ret = 0;
	ret = soljwt_init();
	if (SCX_YES != ret) return ret;

	scx_reg_phase_func(PHASE_VOLUME_CREATE, soljwt_module_volume_load);
	scx_reg_phase_func(PHASE_VOLUME_RELOAD, soljwt_module_volume_load);
#if 0
	scx_reg_phase_func(PHASE_VOLUME_LOOKUP, soljwt_module_volume_lookup);
#else
	scx_reg_phase_func(PHASE_CLIENT_REQUEST, soljwt_module_client_request);
#endif
	scx_reg_phase_func(PHASE_CONTENT_VERIFY, soljwt_module_content_verify);

	return SCX_YES;
}

int
soljwt_module_deinit()
{

	soljwt_deinit();

	return SCX_YES;
}


/* 볼륨 생성시나 리로드시 호출 */
int
soljwt_module_volume_load(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	soljwt_claim_op_t *claim_op = NULL;
	vstring_t 		*claims = NULL;
	off_t 			off = 0;
	vstring_t 		*vs_o = NULL;
	vstring_t 		*claim_validator = NULL;	/* 기본으로 지정된 claim name */
	vstring_t 		*claim_name = NULL; /* claim name 변경을 원하는거나 OFF등 지정 */
	vstring_t 		*claim_opt = NULL;	/* claim에 대한 option */
	int	name_off, opt_off;	//: 포함
	mem_pool_t 			*tmp_mpool = NULL;

	if(NULL == service->soljwt_secret_list)
	{
		/* jwt 인증을 사용하지 않는 경우 */
		return SCX_YES;
	}
	service->soljwt_claim_list = scx_list_create(service->site->mp, LIST_TYPE_SIMPLE_DATA, 0);
	soljwt_init_claim_validator(service);
	tmp_mpool = mp_create(4096);	//이 위치 고민 필요
	claims = scx_get_vstring(service->site, SV_SOLJWT_CLAIMS, NULL);
	if (claims) {
		/*
		 * soljwt_claims는 컴마(,)로 구분된 여러개의 키(claim)가 설정될수 있다.
		 * 형식 : soljwt_claims = claim_validator:claim_name@claim_opt
		 * 예:
		 *     soljwt_claims = alowip:clientip@deny_not_exist, alowcc:alowcc (alowip 대신 clientip로 claim을 변경해서 인식, alowcc는 그대로 사용)
		 *     soljwt_claims = alowip:off (allowip 사용안함. 이 경우 token에 allowip)
		 */

		off = vs_pickup_token(tmp_mpool, claims, off, ",", &vs_o);
		while (vs_o)
		{
			vs_trim_string(vs_o);

			/* 문자열 뒷부분 부터 거꾸로 작업 한다. */
			opt_off = vs_pickup_token_r(tmp_mpool, vs_o, vs_length(vs_o), "@", &claim_opt, DIRECTION_RIGHT);
			if (opt_off == 0) {
				/* option이 지정되지 않은 경우 */
				claim_opt = NULL;
			}
			else {
				/* option이 지정된 경우 option은 claim_opt에 복사가 되고 vs_o에서 해당 부분은 잘라낸다. */
				vs_trim_string(claim_opt);
				vs_truncate(vs_o, opt_off);
			}
			name_off = vs_pickup_token_r(tmp_mpool, vs_o, vs_length(vs_o), ":", &claim_name, DIRECTION_RIGHT);
			if(claim_name == NULL) {
				/* validator와 validator:claim_name이 같이 지정되지 않는 경우는 skip한다.*/
				continue;
			}
			else {
				vs_truncate(vs_o, name_off);
				claim_validator = vs_strdup(tmp_mpool, vs_o);
			}

			/* claim의 동작을 변경 한다. */
			soljwt_change_claim_validator(service, claim_validator, claim_name, claim_opt);


			/* 다음 claim이 있으면 읽는다. */
			off = vs_pickup_token(tmp_mpool, claims, off, ",", &vs_o);
		}

	}


	/*
	 * 여기서 사용할 클레임을 설정파일로 부터 읽는 부분이 들어가야 한다.
	 * solbox JWT의 기본 클레임 정의
	 * 현재는 exp, path
	 * 기본 클레임 외에는 각 validator는 기본적으로 off이며, 설정으로 on
	 * solbox_jwt_soluri_claim_validator allowip=clientip, allowcountry=allowCountry,<validator>=<클레임이름>...
	 * soljwt_enable_claim 으로 기본 claim인 exp와 path외의 인식할 클레임을 지정
	 * 예: soljwt_claims = allowip:clientip, allowcountry:allowcountry (allowip 대신 clientip로 claim을 변경해서 인식, allowcountry는 그대로 사용)
	 */
	if (tmp_mpool) mp_free(tmp_mpool);
	return SCX_YES;
}
#if 0
int
soljwt_module_volume_lookup(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;

	if(NULL == req->service->soljwt_secret_list)
	{
		/* jwt 인증을 사용하지 않는 경우 */
		return SCX_YES;
	}
	/* 여기서 특정 요청에 대해 인증을 skip 할수 있는 방법을 찾아야함. */
	if (SCX_YES != soljwt_handler(req)) {
		scx_error_log(req, "URL[%s] - soljwt secure check failed.\n", vs_data(req->url), vs_data(req->host));
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
int
soljwt_module_client_request(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;

	if (req->method != HR_GET && req->method != HR_HEAD) {
		/* 인증은 HEAD와 GET method일때만 한다. */
		return SCX_YES;
	}
	if(NULL == req->service->soljwt_secret_list || 1 == req->skip_auth)
	{
		/* jwt 인증을 사용하지 않는 경우 */
		return SCX_YES;
	}
	/* 여기서 특정 요청에 대해 인증을 skip 할수 있는 방법을 찾아야함. */
	if (SCX_YES != soljwt_handler(req)) {
		scx_error_log(req, "URL[%s] - soljwt secure check failed.\n", vs_data(req->url), vs_data(req->host));
		if (!req->p1_error) req->p1_error = MHD_HTTP_UNAUTHORIZED;
		return SCX_NO;
	}
	return SCX_YES;
}
#endif
int
soljwt_module_content_verify(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;
	if(NULL == req->service->soljwt_secret_list)
	{
		/* jwt 인증을 사용하지 않는 경우 */
		return SCX_YES;
	}
#if 1
	if (1 == req->expired) {
		scx_error_log(req, "request denied cause of expired token.\n");
		if (!req->p1_error) req->p1_error = MHD_HTTP_UNAUTHORIZED;
	}
#endif
	return SCX_YES;
}


/*
 * default로 지정된 각 클레임의 속성을 list에 넣는다.
 */
int
soljwt_init_claim_validator(service_info_t *service)
{
	soljwt_claim_op_t *claim_op = NULL;
	int i;
	ASSERT(service->soljwt_claim_list);
    for( i = 0; default_claim_validators[i].claim != NULL; i++ ) {
    	claim_op = mp_alloc(service->site->mp, sizeof(soljwt_claim_op_t));
    	claim_op->use = default_claim_validators[i].use;
    	claim_op->claim = mp_strdup(service->site->mp,  default_claim_validators[i].claim);
    	claim_op->handler = default_claim_validators[i].handler;
    	claim_op->opt = default_claim_validators[i].opt;
    	scx_list_append(service->soljwt_claim_list, NULL, (void *)claim_op, 0);
    }
	return SCX_YES;
}

/*
 * 아래의 조건으로 claim의 name 및 옵션을 변경한다.
 * 기본 클레임 외에는 각 validator는 기본적으로 off이며, 설정으로 on
 *   solbox_jwt_soluri_claim_validator allowip=clientip:deny_not_exist, allowcountry=allowCountry,<validator>=<클레임이름>...
 * soljwt_claims 으로 기본 claim인 exp와 path외의 인식할 클레임을 지정
 *   형식 : soljwt_claims = claim_validator:claim_name@claim_opt
 *      claim_name에 off가 들어가는 경우 해당 validator는 동작 안함
 *      예
 *         soljwt_claims = alowip:clientip@deny_not_exist, alowcc:off, duration:duration (alowip 대신 clientip로 claim을 변경해서 인식, alowcc는 사용 안함, duration 사용)
 *         soljwt_claims = alowip:off (allowip 사용안함. 이 경우 token에 allowip)
 */
int
soljwt_change_claim_validator(service_info_t *service, vstring_t *claim_validator, vstring_t *claim_name, vstring_t *claim_opt)
{
	soljwt_claim_op_t *claim_op = NULL;
	int i;
	int list_cnt = 0;
	ASSERT(service->soljwt_claim_list);

	list_cnt = scx_list_get_size(service->soljwt_claim_list);

    for( i = 0; i < list_cnt; i++ ) {
    	claim_op = (soljwt_claim_op_t *)scx_list_get_by_index_data(service->soljwt_claim_list, i);
    	if (strncmp(vs_data(claim_validator), claim_op->claim, vs_length(claim_validator)) == 0) {
    		if (claim_name) {
    			if (strncasecmp(vs_data(claim_name), "off", 3) == 0) {
    				/* 해당 claim을 사용하지 않는 경우 */
    				claim_op->use = FALSE;
    				TRACE((T_INFO, "SERVICE[%s] claim(%s) disable.\n", vs_data(service->name), vs_data(claim_validator)));
    				return SCX_YES;
    			}
    			else if (strncmp(vs_data(claim_validator), vs_data(claim_name), vs_length(claim_validator)) != 0) {
					/* claim name을 변경하는 경우 */
					claim_op->claim = mp_strdup(service->site->mp, vs_data(claim_name));
					TRACE((T_INFO, "SERVICE[%s] claim(%s) name changed(%s).\n", vs_data(service->name), vs_data(claim_validator), vs_data(claim_name)));
    			}
    			/* 설정에 off로 명시적으로 지정되지 않고 해당 claim에 대한 정의가 있으면 사용하는걸로 인식한다. */
    			claim_op->use = TRUE;
    			TRACE((T_INFO, "SERVICE[%s] claim(%s) enabled.\n", vs_data(service->name), claim_op->claim));
    		}
    		if (claim_opt) {
    			/* 해당 claim의 옵션을 변경한다. */
    			if (strncasecmp(vs_data(claim_opt), "deny_not_exist", sizeof("deny_not_exist")) == 0) {
    				claim_op->opt = opt_deny_not_exist;
    				TRACE((T_INFO, "SERVICE[%s] claim(%s) option changed(deny_not_exist).\n", vs_data(service->name), claim_op->claim));
    			}
    			else if(strncasecmp(vs_data(claim_opt), "allow_not_exist", sizeof("allow_not_exist")) == 0) {
    				claim_op->opt = opt_allow_not_exist;
    				TRACE((T_INFO, "SERVICE[%s] claim(%s) option changed(allow_not_exist).\n", vs_data(service->name), claim_op->claim));
    			}
    			else if(strncasecmp(vs_data(claim_opt), "allow_empty_value", sizeof("allow_empty_value")) == 0) {
    				claim_op->opt = opt_allow_empty_value;
    				TRACE((T_INFO, "SERVICE[%s] claim(%s) option changed(allow_empty_value).\n", vs_data(service->name), claim_op->claim));
    			}
    		}
    		return SCX_YES;
    	}
    }
    TRACE((T_INFO, "SERVICE[%s] claim(%s) not found.\n", vs_data(service->name), vs_data(claim_validator)));
	return SCX_YES;
}

static int
soljwt_solbox_jwt_claim_opt_checker(nc_request_t *req, char *claim, jwt_t *jwt, int opt)
{

	/*
	 * 클레임이 비어있는 경우, JWT 에러 코드는 JWT_ERR_NONE이다.
	 */
	if( jwt_errno(jwt) != JWT_ERR_NONE && opt == opt_allow_not_exist )
	{
		TRACE((T_DEBUG, "[%llu] '%s' claim seems to empty, but 'allow_not_exist' option was enabled\n", req->id, claim));
		return SCX_YES;
	}
	if( opt == opt_allow_empty_value )
	{
		TRACE((T_DEBUG, "[%llu] '%s' claim seems to empty, but 'allow_empty_value' option was enabled\n", req->id, claim));
		return SCX_YES;
	}

	return SCX_NO;
}

int
soljwt_claim_validator_exp(nc_request_t *req, char *claim, jwt_t *jwt, int opt)
{
	long long val = 0;
	val = jwt_get_claim_number(jwt, claim);
	if( jwt_errno(jwt) == JWT_ERR_CLAIM && val != -1 ) {
		/* exp claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim does not exist.\n", claim);
			return SCX_NO;
		}
		return SCX_YES;
	}
	if( val == 0 )	{
		/* exp claim의 value가 없는 경우. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim value is empty.\n", claim);
			return SCX_NO;

		}
		return SCX_YES;
	}
	if( (time_t)val < scx_get_cached_time_sec() ) {
		//scx_error_log(req, "already has been expired 'now:%d' '%s:%d'\n", scx_get_cached_time_sec(), claim, (int)val);
		TRACE((T_DEBUG, "[%llu] Request has been expired, 'now:%d' '%s:%d' \n",
									req->id, scx_get_cached_time_sec(), claim, (int)val));
		req->expired = 1;
		return SCX_YES;
	}
	return SCX_YES;
}

int
soljwt_claim_validator_path(nc_request_t *req, char *claim, jwt_t *jwt, int opt)
{
	char *val = NULL;
	int val_len = 0;

	ASSERT(req->path);

	val_len = jwt_get_claim_string_length(jwt, claim);
	if( val_len < 0 )	{
		/* path claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim does not exist.\n", claim);
			return SCX_NO;
		}
		return SCX_YES;
	}
	if( val_len == 0 )	{
		/* path claim의 value가 없는 경우. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim value is empty.\n", claim);
			return SCX_NO;
		}
		return SCX_YES;
	}

	val = (char *)alloca(val_len + 1);
	ASSERT(val);
	memset(val, 0, val_len + 1);
	if( jwt_get_claim_string(jwt, claim, val, val_len) != 0 ) {
		/* path claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim get failed.\n", claim);
			return SCX_NO;
		}
		/* 이 경우가 발생하면 안될듯 */
		return SCX_YES;
	}
	/*
	 * path claim 기준으로 path와 비교한다.
	 */
	if( strncmp(vs_data(req->path), val, val_len) != 0 ) {
		scx_error_log(req, "request path and JWT path are does not match, 'request path:%s', '%s claim:%s'\n", vs_data(req->path), claim, val);
		return SCX_NO;
	}
	TRACE((T_DEBUG, "[%llu] request path and JWT path are match,'request path:%s', '%s claim:%s'\n",req->id, vs_data(req->path), claim, val));

	return SCX_YES;
}

int
soljwt_claim_validator_alowip(nc_request_t *req, char *claim, jwt_t *jwt, int opt)
{
	char *val = NULL;
	int val_len = 0;

	ASSERT(req->path);

	val_len = jwt_get_claim_string_length(jwt, claim);
	if( val_len < 0 )	{
		/* path claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim does not exist.\n", claim);
			return SCX_NO;
		}
		return SCX_YES;
	}
	if( val_len == 0 )	{
		/* path claim의 value가 없는 경우. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim value is empty.\n", claim);
			return SCX_NO;
		}
		return SCX_YES;
	}

	val = (char *)alloca(val_len + 1);
	ASSERT(val);
	memset(val, 0, val_len + 1);
	if( jwt_get_claim_string(jwt, claim, val, val_len) != 0 ) {
		/* path claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			scx_error_log(req, "'%s' claim get failed.\n", claim);
			return SCX_NO;
		}
		/* 이 경우가 발생하면 안될듯 */
		return SCX_YES;
	}
	/*
	 * alowip claim 기준으로 client의 ip와 비교한다.
	 * 앞단에 nginx가 있는 경우 X-Forwarded-For를 통해서 client의 ip가 들어 올수 있는데 이 경우에 대한 처리는 하지 않는다.
	 */
	if( strncmp(vs_data(req->client_ip), val, val_len) != 0 ) {
		scx_error_log(req, "request ip and JWT ip are does not match, 'request ip:%s', '%s claim:%s'\n", vs_data(req->client_ip), claim, val);
		return SCX_NO;
	}
	TRACE((T_DEBUG, "[%llu] request ip and JWT ip are match,'request ip:%s', '%s claim:%s'\n",req->id, vs_data(req->client_ip), claim, val));

	return SCX_YES;
}

int
soljwt_claim_validator_alowcc(nc_request_t *req, char *claim, jwt_t *jwt, int opt)
{
	char *val = NULL;
	int val_len = 0;
	char country_code[5] = {0};
	int i, cnt;
	if( jwt_is_claim_type_string(jwt, claim) )	{
		/* 클레임이 문자열인 경우 */
		val_len = jwt_get_claim_string_length(jwt, claim);
		if( val_len < 0 )	{
			/* 해당 claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
			if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
				scx_error_log(req, "'%s' claim does not exist.\n", claim);
				return SCX_NO;
			}
			return SCX_YES;
		}
		if( val_len == 0 )	{
			/* 해당 claim의 value가 없는 경우. */
			if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
				scx_error_log(req, "'%s' claim value is empty.\n", claim);
				return SCX_NO;
			}
			return SCX_YES;
		}

		val = (char *)alloca(val_len + 1);
		ASSERT(val);
		memset(val, 0, val_len + 1);
		if( jwt_get_claim_string(jwt, claim, val, val_len) != 0 ) {
			/* path claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
			if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
				scx_error_log(req, "'%s' claim get failed.\n", claim);
				return SCX_NO;
			}
			/* 이 경우가 발생하면 안될듯 */
			return SCX_YES;
		}

		if( strncasecmp(val, (u_char *)"ALL", (size_t)3) == 0 )
		{
			TRACE((T_DEBUG, "[%llu] 'ALL' requested, allow all country codes\n", req->id));
			return SCX_YES;
		}
#if 0
		//테스트용 코드
		//sprintf(vs_data(req->client_ip), "211.38.137.42");
		sprintf(vs_data(req->client_ip), "104.17.126.180");
		vs_update_length(req->client_ip, -1);
#endif

		if (gip_lookup_country2(vs_data(req->client_ip), country_code) == NULL) {
			scx_error_log(req, "'%s' unable to proceed, cause of lack of country code information.\n", vs_data(req->client_ip));
			return SCX_NO;
		}
		if (strncasecmp(country_code, val, 2) == 0) {
			TRACE((T_DEBUG, "[%llu] '%s' country code match\n", req->id, val));
			return SCX_YES;
		}
		scx_error_log(req, "'%s:%s' country code not match\n", val, country_code);
		return SCX_NO;
	}
	else if( jwt_is_claim_type_array(jwt, claim) )	{
		cnt = jwt_get_claim_array_size(jwt, claim);
		if( cnt < 0 ) {
			if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
				scx_error_log(req, "'%s' claim array does not exist.\n", claim);
				return SCX_NO;
			}
			return SCX_YES;
		}
		else if( cnt == 0 ) {
			if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
				scx_error_log(req, "'%s' claim value is empty.\n", claim);
				return SCX_NO;
			}
			return SCX_YES;
		}
		for( i = 0; i < cnt; i++ )		{
			val_len = jwt_get_claim_array_n_string_length(jwt, claim, i);
			if( val_len <= 0 )	{
				/* 클레임이 없는 경우 처리 */
				if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
					continue;
				}
				return SCX_YES;
			}
			val = (char *)alloca(val_len + 1);
			ASSERT(val);
			memset(val, 0, val_len + 1);
			if( jwt_get_claim_array_n_string(jwt, claim, i, val, val_len) != 0 ) {
				/* path claim이 없는 경우 허용 옵션으로 되어 있는지 확인한다. */
				if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
					scx_error_log(req, "'%s' claim get failed.\n", claim);
					return SCX_NO;
				}
				/* 이 경우가 발생하면 안될듯 */
				return SCX_YES;
			}
			if( strncasecmp(val, (u_char *)"ALL", (size_t)3) == 0 )
			{
				TRACE((T_DEBUG, "[%llu] 'ALL' requested, allow all country codes\n", req->id));
				return SCX_YES;
			}
#if 0
		//테스트용 코드
		sprintf(vs_data(req->client_ip), "211.38.137.42");
		//sprintf(vs_data(req->client_ip), "104.17.126.180");
		vs_update_length(req->client_ip, -1);
#endif

			if (gip_lookup_country2(vs_data(req->client_ip), country_code) == NULL) {
				/* 배열 형태로 들어 온 경우 여기서 문제가 발생하더라도 에러처리하지 않고 다음으로 넘긴다. */
				TRACE((T_DEBUG, "[%llu] '%s' unable to proceed, cause of lack of country code information.\n", req->id, vs_data(req->client_ip)));
				continue;
			}
			if (strncasecmp(country_code, val, 2) == 0) {
				TRACE((T_DEBUG, "[%llu] '%s' country code match\n", req->id, val));
				return SCX_YES;
			}
			TRACE((T_DEBUG, "[%llu] '%s:%s' country code not match\n", req->id, val, country_code));
			continue;

		}
		scx_error_log(req, "tried to validate %d of '%s' claim's value, but nothing match\n", i, claim);
		return SCX_NO;
	}
	else if( jwt_is_claim_type_null(jwt, claim) ) {
		if( soljwt_solbox_jwt_claim_opt_checker(req, claim, jwt, opt) != SCX_YES ) {
			return SCX_NO;
		}
		return SCX_YES;

	}
	return SCX_YES;
}

int
soljwt_claim_validator_duration(nc_request_t *req, char *claim, jwt_t *jwt, int opt)
{
	soljwt_context_t *ctx  = NULL;
	long long val = 0;

	ctx = (soljwt_context_t *)req->module_ctx[gscx__soljwt_module.index];

	val = jwt_get_claim_number(jwt, claim);
	if (val <= 0) {
		/* duration claim은 지정되지 않은 경우 옵션에 관계 없이 skip 한다. */
		return SCX_YES;
	}
	ctx->duration = (int)val;
	TRACE((T_DEBUG, "[%llu] %s = %d\n", req->id, claim, ctx->duration));
	return SCX_YES;
}

int
soljwt_claim_validator_playstart(nc_request_t *req, char *claim, jwt_t *jwt, int opt)
{
	soljwt_context_t *ctx  = NULL;
	long long val = 0;

	ctx = (soljwt_context_t *)req->module_ctx[gscx__soljwt_module.index];

	val = jwt_get_claim_number(jwt, claim);
	if (val <= 0) {
		/* playstart claim은 지정되지 않은 경우 옵션에 관계 없이 skip 한다. */
		return SCX_YES;
	}
	ctx->start = (int)val;
	TRACE((T_DEBUG, "[%llu] %s = %d\n", req->id, claim, ctx->start));
	return SCX_YES;
}


/*
 * soljwt 모듈에서 uri 파싱 및 해석, 검증을 위해 사용하는 컨텍스트를 초기화한다.
 */
static soljwt_context_t *
soljwt_context_init(nc_request_t *req)
{
	soljwt_context_t *ctx;
	/* soljwt_check_streaming_claim()에서 먼저 생성 하는 경우가 있어서 NULL인 경우만 할당하도록 한다. */
	if (req->module_ctx[gscx__soljwt_module.index] == NULL) {
		ctx = mp_alloc(req->pool, sizeof(soljwt_context_t));
		ASSERT(ctx);
		req->module_ctx[gscx__soljwt_module.index] = (void *)ctx;
	}
	else {
		ctx = (soljwt_context_t *)req->module_ctx[gscx__soljwt_module.index];
	}
	ctx->start = 0;
	ctx->duration = 0;
	return ctx;
}

int
soljwt_handler(nc_request_t *req)
{
	streaming_t *streaming = req->streaming;
	if (streaming) {
		if ((req->streaming->protocol & O_PROTOCOL_CORS) != 0) {
			/* crossdomain.xml요청은 인증을 하지 않는다. */
			return SCX_YES;
		}
	}
	if (SCX_YES != soljwt_validator(req)) {
		return SCX_NO;
	}
	return SCX_YES;
}

/*
 * JWT 검증
 * soljwt_secret에 설정된 PSK 수 만큼 반복해서 verify를 시도.
 * JWT spec상 PSK가 mandtory가 아니므로 NULL인 케이스도 고려해야함.
 * 추가로, base64 인코딩한 키에 대한 케이스도 고려해야함.
 */
static int
soljwt_verify(nc_request_t *req, const char *token, int token_len, jwt_t **jwt)
{
	char *secret = NULL;
	int count = 0;
	int ret = SCX_NO;
	char token_str[token_len + 1];
	int i;

	ASSERT(token);

	memset(token_str, 0, token_len + 1);
	memcpy(token_str, token, token_len);

	count = scx_list_get_size(req->service->soljwt_secret_list);
	/* secret 여러개인 경우 성공하는게 나올때 까지 모두 검사한다. */
	for( i = 0; i < count; i++ )	{
		*jwt = jwt_new();
		ASSERT(*jwt);
		secret = scx_list_get_by_index_key(req->service->soljwt_secret_list, i);
		TRACE((T_DEBUG, "[%llu] soljwt secret key[%s], token[%s]\n", req->id, secret, token_str));
		jwt_set_opt(*jwt, JWT_OPT_VERIFY_HEADER | JWT_OPT_VERIFY_SIG);
		if( jwt_decode(*jwt, (char *)token_str, secret, strlen(secret)) == 0 ) {
			TRACE((T_DEBUG, "[%llu] token is secure.\n", req->id));
			ret = SCX_YES;
			break;
		}
		/*
		* 실패 이유가 expire인 경우 더 이상 진행 하지 않는다.
		* JWT_OPT_VERIFY_CLAIM_EXP를 설정하지 않았기 때문에 expire 검사를 하지 않아서 실제로 여기로 들어오지는 않는다.
		* expire 검사는 여기서 하지 않고 claim_validator에서 한다.
		*/
		if( jwt_errno(*jwt) == JWT_ERR_EXPIRED )
		{
			TRACE((T_DEBUG, "[%llu] request denied cause of expired token.(%s)\n", req->id, token));
//			scx_error_log(req, "request denied cause of expired token.(%s)\n", token));
			ret = SCX_YES;
			/* 인증이 만료가 된 경우 여기서 에러를 리턴하지 않고 다음 phase handler에서 req->expired의 값을 검사후 에러 처리를 한다.
			 * 이렇게 하는 이유는 중간에 세션 검사 로직이 들어가서 만료가 됬더라도 세션이 있는 경우 만료가 되지 않은 것으로 하기 위함임. */
			req->expired = 1;
			break;
		}
		else {
			TRACE((T_DEBUG, "[%llu] failed to decode token(%s), reason : '%s'\n", req->id, token, jwt_err_str(*jwt)));

		}
		/* 다음 secret에 대해 jwt_decode를 실행하려면 jwt를 초기화해야 한다. */
		/* jwt context를 포인터를 넘겨 받을때 더블 포인터로 받아야 하는지 확인 필요 */
		jwt_free(*jwt);
		*jwt = NULL;
	}

	return ret;
}

static int
soljwt_claim_validator_handlers(nc_request_t *req, jwt_t *jwt)
{
	soljwt_claim_op_t *op = NULL;
	int i;
	int list_cnt = 0;
	ASSERT(req->service->soljwt_claim_list);
	ASSERT(jwt);
	list_cnt = scx_list_get_size(req->service->soljwt_claim_list);

	for( i = 0; i < list_cnt; i++ ) {
	    op = (soljwt_claim_op_t *)scx_list_get_by_index_data(req->service->soljwt_claim_list, i);
		if( op->use == FALSE )
		{
			TRACE((T_DEBUG, "[%llu] '%s' claim validator disabled.\n", req->id, op->claim));
			continue;
		}
		if( op->handler(req, op->claim, jwt, op->opt) != SCX_YES )
		{
			TRACE((T_DEBUG, "[%llu] invaild '%s' claim\n", req->id, op->claim));
			return SCX_NO;
		}
		else
		{
			TRACE((T_DEBUG, "[%llu] Vaild '%s' claim\n", req->id, op->claim));

		}
	}
	TRACE((T_DEBUG, "[%llu] claim validation succeed\n", req->id));
	return SCX_YES;
}

static int
soljwt_validator(nc_request_t *req)
{
	int	ret = SCX_YES;
	jwt_t *jwt_ctx = NULL;
	char *token = NULL;
	soljwt_context_t *ctx = NULL;

	TRACE((T_DEBUG, "[%llu] soljwt enabled.\n", req->id));


	ctx = soljwt_context_init(req);
	if(NULL == ctx) {
		TRACE((T_ERROR, "[%llu] Failed to initialized jwt context(%s).\n", req->id, vs_data(req->url) ));
		/* FIXME: 여기서 모듈이 실패하면 답이 없다!!! T^T */
		goto soljwt_handler_error;
	}

	if (req->connect_type == SCX_RTMP) {
		/* RTMP 요청인 경우에는 jwt 토큰을 다른 곳에서 가져온다.  */
		if (req->jwt_token != NULL) {
			token = vs_data(req->jwt_token);
		}
	}
	else {
		token = (char *)MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, SOLJWT_TOKEN);
		if (token == NULL) {
			TRACE((T_DAEMON, "[%llu] empty jwt token\n", req->id));
			scx_error_log(req, "empty jwt token\n");
			goto soljwt_handler_error;
		}
	}

	/* 케이스 2: JWT 검증  */
	/*
	 * secret_array에 설정된 PSK 수 만큼 반복해서 verify를 시도.
	 *  JWT spec상 PSK가 mandatory가 아니므로 NULL인 케이스도 고려해야함.
	 *  추가로, base64 인코딩한 키에 대한 케이스도 고려해야함.
	 */
	if( soljwt_verify(req, token, strlen(token), &jwt_ctx) != SCX_YES )
	{
		scx_error_log(req, "Failed to verify token(%s)\n", token);
		goto soljwt_handler_error;
	}

	if( soljwt_claim_validator_handlers(req, jwt_ctx) != SCX_YES ) {
		scx_error_log(req, "Failed to validate token(%s)\n", token);
		goto soljwt_handler_error;
	}
	if (ctx->start > 0 || ctx->duration > 0) {
		if (req->streaming == NULL) {
			goto soljwt_handler_success;
		}
		if ( (O_STRM_TYPE_SINGLE & req->streaming->media_mode) != 0) {
			if (req->streaming->content == NULL) {
				goto soljwt_handler_success;
			}
			/* claim에 playstart나 duration이 지정된 경우에는 argument에 값이 지정 되어 있더라도 무시하고 claim의 값을 우선 적용한다. */
			if (ctx->start > 0 && ctx->duration > 0) {
				/* milisecond로 변환 */
				req->streaming->content->start = ctx->start * 1000;
				req->streaming->content->end = ctx->start * 1000 + ctx->duration * 1000;
			}
			else if (ctx->start > 0 ) {
				/* playstart만 지정된 경우 해당 지점부터 끝까지 지정 한다. */
				req->streaming->content->start = ctx->start * 1000;
				req->streaming->content->end = EOF;
			}
			else if (ctx->duration > 0 ) {
				/* duration만 지정된 경우 처음부터 duration만큼만 지정한다. */
				req->streaming->content->start = 0;
				req->streaming->content->end = ctx->duration * 1000;
			}
		}
		else if( (O_STRM_TYPE_ADAPTIVE & req->streaming->media_mode)  != 0) {
			/*
			 * 여기서의 Adaptive는 Zipper의 Adaptive mode로 요청이 들어온게 아니라
			 * single 모드나 SolProxy에서 smil 파일로 요청이 들어와서 adaptive mode로 변경된 경우이다.
			 * 처음 부터 Zipper의 Adaptive mode로 들어 오는 경우는 기존 path에 같이 지정된 시간이 있는 경우
			 * 해당 값이 무시가 되기 때문에 문제가 발생할수 있다.
			 */
			if (req->streaming->adaptive_info == NULL) {
				goto soljwt_handler_success;
			}
			/* claim에 playstart나 duration이 지정된 경우에는 argument에 값이 지정 되어 있더라도 무시하고 claim의 값을 우선 적용한다. */
			if (ctx->start > 0 && ctx->duration > 0) {
				/* milisecond로 변환 */
				req->streaming->adaptive_info->start = ctx->start * 1000;
				req->streaming->adaptive_info->end = ctx->start * 1000 + ctx->duration * 1000;
			}
			else if (ctx->start > 0 ) {
				/* playstart만 지정된 경우 해당 지점부터 끝까지 지정 한다. */
				req->streaming->adaptive_info->start = ctx->start * 1000;
				req->streaming->adaptive_info->end = EOF;
			}
			else if (ctx->duration > 0 ) {
				/* duration만 지정된 경우 처음부터 duration만큼만 지정한다. */
				req->streaming->adaptive_info->start = 0;
				req->streaming->adaptive_info->end = ctx->duration * 1000;
			}
		}
	}
	goto soljwt_handler_success;
soljwt_handler_error:
	ret = SCX_NO;
soljwt_handler_success:
	if( jwt_ctx )
	{
		jwt_free(jwt_ctx);
	}
	return ret;
}


/*
 * jwt token에 포함된 스트리밍 관련 claim(start, duration)이 있는지 확인 해서
 * 있는 경우 SCX_YES를 리턴, 없는 경우는 SCX_NO를 리턴
 *
 */
int
soljwt_check_streaming_claim(nc_request_t *req)
{
	int	ret = SCX_NO;
	jwt_t *jwt_ctx = NULL;
	char *token = NULL;
	soljwt_context_t *ctx = NULL;

	if(NULL == req->service->soljwt_secret_list || 1 == req->skip_auth)
	{
		/* jwt 인증을 사용하지 않는 경우는 바로 리턴 */
		return SCX_NO;
	}
	if (req->connect_type == SCX_RTMP) {
		/* RTMP 요청인 경우에는 이 함수가 호출 되지 않는다. */
		TRACE((T_ERROR, "[%llu] Unexpected call.(%s).\n", req->id, vs_data(req->url) ));
		ASSERT(0);

	}
	token = (char *)MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, SOLJWT_TOKEN);
	if (token == NULL) {
		TRACE((T_DAEMON, "[%llu] empty jwt token\n", req->id));
		return SCX_NO;
	}
	ctx = soljwt_context_init(req);
	if(NULL == ctx) {
		TRACE((T_ERROR, "[%llu] Failed to initialized jwt context(%s).\n", req->id, vs_data(req->url) ));
		return SCX_NO;
	}
	/* 케이스 2: JWT 검증  */
	/*
	 * secret_array에 설정된 PSK 수 만큼 반복해서 verify를 시도.
	 *  JWT spec상 PSK가 mandatory가 아니므로 NULL인 케이스도 고려해야함.
	 *  추가로, base64 인코딩한 키에 대한 케이스도 고려해야함.
	 */
	if( soljwt_verify(req, token, strlen(token), &jwt_ctx) != SCX_YES )
	{
		scx_error_log(req, "Failed to verify token(%s)\n", token);
		goto soljwt_streaming_claim_error;
	}
	if( soljwt_claim_validator_handlers(req, jwt_ctx) != SCX_YES ) {
		scx_error_log(req, "Failed to validate token(%s)\n", token);
		goto soljwt_streaming_claim_error;
	}
	if (ctx->start > 0 || ctx->duration > 0) {
		/* 두 claim중 한개라도 0보다 큰값이 들어 있는 경우에 미리 보기로 판단해서 SCS_YES를 리턴한다. */
		ret = SCX_YES;
	}
	goto soljwt_streaming_claim_success;
soljwt_streaming_claim_error:
	ret = SCX_NO;
soljwt_streaming_claim_success:
	if( jwt_ctx )
	{
		jwt_free(jwt_ctx);
	}
	return ret;
}


