/*
 * solbox JWT decode & verify library
 * <sd@solbox.com>
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <gnutls/x509.h>
#include <gnutls/abstract.h>

#include <jansson.h>

//#include <execinfo.h>
#include "lib_solbox_jwt.h"
#include "common.h"
#include "scx_util.h"

/*
 * TODO: 해시관련한 내부 로직을 변경해야한다.
 */
struct jwt_part_s {
	char *token;
	json_t *claims;
};
/*
 * Header.Payload.Signature로 되어 있는 token을
 * jwt->head->token에 Header가 jwt->body->token에 Payload, 그리고 jwt->sig에 Signature가 들어간다.
 */
struct jwt_s {
	jwt_alg_t alg;
	jwt_part_t *head;	/* token의 Header가 들어감 */
	jwt_part_t *body;	/* token의 Payload가 들어감 */
	char *sig;			/* token의 Signature가 들어감 */
	char *key;			/* 설정의 secret Key 값이 들어감 */
	int key_len;
	int flags;
	char *err_str;
	int err;
};

#define jwt_set_no_error(jwt) { jwt->err = 0; jwt->err_str = NULL; }
#define jwt_set_error(jwt, n) { jwt->err = n; jwt->err_str = NULL; }
#define jwt_set_error_str(jwt, n, str) { jwt->err = n; jwt->err_str = str; }
#define jwt_is_opt_set(jwt, opt) (jwt->flags & opt)
#define jwt_has_error(jwt) ( (jwt->err != 0 || jwt->err_str != NULL ) ? 1 : 0 )


/*
 * json operations
 */
static const char *get_js_string(json_t *js, const char *key)
{
	const char *val = NULL;
	json_t *js_val;

	js_val = json_object_get(js, key);
	if (js_val)
		val = json_string_value(js_val);

	return val;
}
static long long get_js_number(json_t *js, const char *key)
{
	/* ir #29586  -1 -> 0*/
	long long val = 0;
	json_t *js_val;

	js_val = json_object_get(js, key);
	if( js_val )
		val = json_integer_value(js_val);
	return val;
}
static int get_js_type(json_t *js, const char *key)
{
	json_t *js_val;

	js_val = json_object_get(js, key);
	if( js_val )
		return json_typeof(js_val);
	return JSON_NULL;
}

const char *jwt_err_str(jwt_t *jwt)
{
	switch( jwt->err )
	{
		case JWT_ERR_EXPIRED:
			return (jwt->err_str ? jwt->err_str : "expired JWT");
		case JWT_ERR_NOT_SUPPORTED:
			return (jwt->err_str ? jwt->err_str : "not supported yet");
		case JWT_ERR_OPTIONAL:
			return (jwt->err_str ? jwt->err_str : "Not error, this is optional");
		case JWT_ERR_INVALID:
			return (jwt->err_str ? jwt->err_str : "invalid JWT");
		case JWT_ERR_NOMEM:
			return "Unable to allocate memory";
		case JWT_ERR_HEADER:
			return (jwt->err_str ? jwt->err_str : "invalid JWT header");
		case JWT_ERR_BODY:
			return (jwt->err_str ? jwt->err_str : "invalid JWT body" );
		case JWT_ERR_SIG:
			return (jwt->err_str ? jwt->err_str : "invalid JWT signature");
		case JWT_ERR_CLAIM:
			return (jwt->err_str ? jwt->err_str : "invalid claim");
		default:
			return (jwt->err_str ? jwt->err_str : "unknown error code");
	}
	return NULL;
}
static int jwt_str_alg(jwt_t *jwt, const char *alg)
{ 
	jwt_set_no_error(jwt); 
	if (!strcasecmp(alg, "none"))
	{
		jwt->alg = JWT_ALG_NONE;
	}
	else if (!strcasecmp(alg, "HS256"))
	{
		jwt->alg = JWT_ALG_HS256;
	}
	else if (!strcasecmp(alg, "HS384"))
	{
		jwt->alg = JWT_ALG_HS384;
	}
	else if (!strcasecmp(alg, "HS512"))
	{
		jwt->alg = JWT_ALG_HS512;
	}
	else
	{
		jwt_set_error_str(jwt, JWT_ERR_NOT_SUPPORTED, "not supported alg");
		errno = EINVAL;
		return -1;
	}

	return 0;
}
static void jwt_scrub_key(jwt_t *jwt)
{
	if (jwt->key) {
		/* Overwrite it so it's gone from memory. */
		memset(jwt->key, 0, jwt->key_len);

		SCX_FREE(jwt->key);
		jwt->key = NULL;
	}

	jwt->key_len = 0;
	jwt->alg = JWT_ALG_NONE;
}

jwt_alg_t jwt_get_alg(jwt_t *jwt)
{
	return jwt->alg;
}
static jwt_part_t *jwt_part_new(void)
{
	jwt_part_t *part;
	part = SCX_MALLOC(sizeof(jwt_part_t)); // 16
	if( !part )
	{
		errno = ENOMEM;
		return NULL;
	}
	memset(part, 0, sizeof(jwt_part_t));
#if 0
	/* 
	 * #ir 29072, jwt_decode() -> jwt_parse_part() 까지 들어가면, base64 디코딩 후, 해당 문자열로 json 객체를 생성한다. 
	 * 즉, 여기서 각 클레임들을 위한 json 객체를 만들어 둘 필요가 없다
	 * (우리는 jwt 인코딩이 아니라 디코딩이므로..)
	 */
	part->claims = json_object();
	if( !part->claims )
	{
		SCX_FREE(part);
		errno = ENOMEM;
		return NULL;
	}
#endif
	errno = 0;
	return part;
	
}
jwt_t *jwt_new(void)
{
	jwt_t *jwt;
	jwt = SCX_MALLOC(sizeof(jwt_t)); // 64
	if(!jwt)
	{	
		//errno = ENOMEM; 
		return NULL;
	}

	memset(jwt, 0, sizeof(jwt_t));

	jwt->head = jwt_part_new();
	if( !jwt->head )
	{
		SCX_FREE(jwt);
		//errno = ENOMEM;
		return NULL;
	}
	jwt->body = jwt_part_new();
	if( !jwt->body )
	{
		SCX_FREE(jwt->head);
		SCX_FREE(jwt);
		//errno = ENOMEM;
		return NULL;
	}
	jwt->flags = JWT_OPT_VERIFY_NONE;
	return jwt;
}

void jwt_part_free(jwt_part_t *part)
{
	if(!part)
		return;
	json_decref(part->claims);
	SCX_FREE(part);
}
void jwt_free(jwt_t *jwt)
{
	if (!jwt)
		return;

	jwt_scrub_key(jwt);
	/*
	 * token을 strdup하여 head의 token으로 할당하므로, head의 token만 free
	 */
	if(jwt->head->token)
		SCX_FREE(jwt->head->token);
	jwt->head->token = NULL;
	jwt_part_free(jwt->head);
	jwt_part_free(jwt->body);

	SCX_FREE(jwt);
}
void jwt_part_wipe(jwt_part_t *part)
{
	if(!part)
		return;
	json_decref(part->claims);
}
/*
 * jwt 오브젝트와 flags를 제외한 모든 멤버를 삭제한다.
 * jwt 파싱 + 디코딩을 진행할 수 있도록 하기 위하여, jwt_new() 결과와 같은 상태를 반환해야한다.
 * FIXME:
 * 	장기적으로는 jwt_wipe() 함수를 없애자
 */
void jwt_wipe(jwt_t *jwt)
{
	if (!jwt)
		return;

	jwt_scrub_key(jwt);
	/*
	 * token을 strdup하여 head의 token으로 할당하므로, head의 token만 free
	 */
	if(jwt->head->token)
		SCX_FREE(jwt->head->token);
	jwt->head->token = NULL;
	jwt_part_free(jwt->head);
	jwt_part_free(jwt->body);
	jwt->head = jwt_part_new();
	if( !jwt->head )
	{
		SCX_FREE(jwt);
		return;
	}
	jwt->body = jwt_part_new();
	if( !jwt->body )
	{
		SCX_FREE(jwt->head);
		SCX_FREE(jwt);
		return;
	}
	jwt_set_no_error(jwt);

}
static json_t *jwt_b64_decode(char *src)
{
#if 0
	char *decode, *input;
	int z, len;

	len = strlen(src);
	decode = alloca(BASE64_DECODED_LENGTH(strlen(src)+4));
	input = alloca(len+4);

//	printf("len = %d, decode len = %d\n", strlen(src)+4, BASE64_DECODED_LENGTH(strlen(src)+4));
	sprintf(input, "%s",src);
	z = 4 - (len % 4);
	if (z < 4) {
		while (z--)
			input[len++] = '=';
	}
	input[len] = '\0';
	if( scx_base64_decode(decode, input) == SCX_NO )
	{
		TRACE((T_DAEMON|T_DEBUG,"Failed to decoded base64: %s'.\n", src));
		return NULL;
	}
#else
	char *decode;
	decode = alloca(BASE64_DECODED_LENGTH(strlen(src)+4));
	if( scx_base64_decode(decode, src) == SCX_NO )
	{
		TRACE((T_DAEMON|T_DEBUG,"Failed to decoded base64: %s'.\n", src));
		return NULL;
	}
#endif
	return json_loads(decode, 0, NULL);

}

static pthread_mutex_t 	gscx__gnutls_hmac_lock = PTHREAD_MUTEX_INITIALIZER;
int jwt_sign_sha_hmac(jwt_t *jwt, char **out, unsigned int *len, const char *str)
{
	gnutls_mac_algorithm_t  alg;
	int ret;

	switch (jwt->alg) {
	case JWT_ALG_HS256:
		alg = GNUTLS_DIG_SHA256;
		break;
	case JWT_ALG_HS384:
		alg = GNUTLS_DIG_SHA384;
		break;
	case JWT_ALG_HS512:
		alg = GNUTLS_DIG_SHA512;
		break;
	default:
		return EINVAL;
	}

	*len = gnutls_hmac_get_len(alg);
	*out = malloc(*len + 1);	/* 이부분을 SCX_MALLOC()로 대체 할 경우 gnutls_hmac_fast()의 결과가 달라진다. */
	*(*out+*len) = '\0'; /* 문자열의 마지막 부분에 '\0'가 들어 가지 않는 경우 scx_encode_base64_url() 호출시 sign의 길이를 잘못 인식하는 문제가 발생한다. */
	if (*out == NULL)
		return ENOMEM;
//	printf("len = %d, alg = %d, key = %s, keylen = %d, str = %s, strlen = %d\n", *len, alg, jwt->key, jwt->key_len, str, strlen(str));
	pthread_mutex_lock(&gscx__gnutls_hmac_lock);
	/* gnutls_hmac_fast()를 동시에 여러 thread에서 호출하는 경우 죽는 문제가 발생해서 lock으로 처리함 */
	ret = gnutls_hmac_fast(alg, jwt->key, jwt->key_len, str, strlen(str), *out);
	pthread_mutex_unlock(&gscx__gnutls_hmac_lock);
	if (ret != 0) {
		free(*out);
		*out = NULL;
		return EINVAL;
	}

	return 0;
}

static int jwt_parse_part(jwt_part_t *part, char *token)
{
	part->claims = jwt_b64_decode(token);
	if (!part->claims)
	{	
		return -1;
	}
	return 0;
}
static int jwt_verify_alg(jwt_t *jwt)
{
	json_t *js = NULL;
	const char *val;
	int alg = 0;

	if( !jwt || !jwt->head || !jwt->head->claims )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	jwt_set_no_error(jwt); 
	js = jwt->head->claims;

	val = get_js_string(js, "alg");
	if( !val )
	{
		jwt_set_error_str(jwt, JWT_ERR_HEADER, "unable to found alg");
		errno = EACCES;
		return -1;
	}
	alg = jwt_str_alg(jwt, val);
	if( alg != 0 )
		return -1;
	jwt_set_no_error(jwt);
	return 0;
}
static int jwt_verify_cty(jwt_t *jwt)
{
	/* TODO: implement...this is optional claim */
	jwt_set_error_str(jwt, JWT_ERR_NOT_SUPPORTED, "cty parameter is not supported yet");
	return 0;
}
static int jwt_verify_typ(jwt_t *jwt)
{
	json_t *js = NULL;
	const char *val;

	if( !jwt || !jwt->head || !jwt->head->claims )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	jwt_set_no_error(jwt); 
	js = jwt->head->claims;
	
	val = get_js_string(js, "typ");
	if( !val )
	{
		jwt_set_error_str(jwt, JWT_ERR_OPTIONAL, "typ parameter is optional");
		return 1; /* typ is optional */
	}
	if( strcasecmp(val, "JWT") != 0 )
	{
		jwt_set_error_str(jwt, JWT_ERR_INVALID, "only \"JWT\" allowed in here"); /* cause of not implemented.. */
		errno = EACCES;
		return -1;
	}
	jwt_set_no_error(jwt);
	return 0;

}
int jwt_verify_head(jwt_t *jwt)
{
	if( !jwt || !jwt->head )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}	
	if( jwt_parse_part(jwt->head, jwt->head->token) != 0 )
	{
		jwt_set_error(jwt, JWT_ERR_HEADER);
		return -1;
	}
	if( jwt_is_opt_set( jwt, JWT_OPT_VERIFY_HEADER ) )
	{
		if( jwt_verify_alg(jwt) < 0 )
		{
			if( !jwt_has_error(jwt) ) 
				jwt_set_error(jwt, JWT_ERR_HEADER);
			return -1;
		}
		if( jwt_verify_cty(jwt) < 0 )
		{
			if( !jwt_has_error(jwt) ) 
				jwt_set_error(jwt, JWT_ERR_HEADER);
			return -1;
		}
		/* If alg is not NONE, there should be a typ. */
		if (jwt->alg != JWT_ALG_NONE)
		{
			if( jwt_verify_typ(jwt) != 0 )
			{
				if( !jwt_has_error(jwt) ) 
					jwt_set_error(jwt, JWT_ERR_HEADER);
				return -1;
			}
		}
	}
	jwt_set_no_error(jwt);
	return 0;
}
int jwt_verify_exp(jwt_t *jwt)
{
	json_t *js = NULL;
	time_t exp;
	jwt_set_no_error(jwt); 
	if( !jwt || !jwt->body || !jwt->body->claims )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	js = jwt->body->claims;

	exp = get_js_number(js, "exp");
	if( !exp || exp < time(NULL) )
	{
		jwt_set_error_str(jwt, JWT_ERR_EXPIRED, "\"exp\" already has been expired");
		errno = EACCES;
		return -1;
	}
	jwt_set_no_error(jwt);
	return 0;

}
int jwt_verify_body(jwt_t *jwt)
{
	if( !jwt || !jwt->body )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}	
	jwt_set_no_error(jwt); 
	if( jwt_parse_part(jwt->body, jwt->body->token) != 0 )
	{
		if( !jwt_has_error(jwt) ) 
			jwt_set_error(jwt, JWT_ERR_BODY);
		return -1;
	}
	if( jwt_is_opt_set(jwt, JWT_OPT_VERIFY_BODY) 
		|| jwt_is_opt_set(jwt, JWT_OPT_VERIFY_CLAIM_EXP) )
	{
		if( jwt_verify_exp(jwt) != 0 )
		{
			if( !jwt_has_error(jwt) ) 
				jwt_set_error(jwt, JWT_ERR_BODY);
			return -1;
		}
	}
	jwt_set_no_error(jwt);
	return 0;
}

/*
 * Header.Payload.Signature로 되어 있는 token을 파싱해서
 * jwt->head->token에 header가 jwt->body->token에 Payload, 그리고 jwt->sig에 Signature가 들어간다.
 */
int jwt_parse_token(jwt_t *jwt, const char *token)
{
	char *head = NULL; 
	char *body, *sig;
	if( !jwt || !token )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		goto failed;
	}	
	head = SCX_STRDUP(token);
	if (!head)
	{
		jwt_set_error(jwt, JWT_ERR_NOMEM);
		errno = ENOMEM;
		goto failed;
	}

	for (body = head; body[0] != '.'; body++) {
		if (body[0] == '\0')
		{
			errno = EINVAL;
			goto failed;
		}
	}

	body[0] = '\0';
	body++;

	/* 
	 * RFC 7519에서는 unsecured JWT의 경우 signature가 없을 수 있다고 명시되어 있음
	 */
	for (sig = body; sig[0] != '.'; sig++) {
		if (sig[0] == '\0')
			break;
	}
	/*
	 * unsecured JWT 또는 invalid JWT의 경우 signature가 없을 수 있다.
	 * 파싱 이후 위 케이스들에 대하여 결과를 반환한다.
	 */
	if( sig[0] != '\0' ) {
		sig[0] = '\0';
		sig++;
	}
	jwt->head->token = head;
	jwt->body->token = body;
	jwt->sig = sig;
	jwt_set_no_error(jwt);
	return 0;
failed:
	if( head )
		SCX_FREE(head);
	/* head 를 free 했으므로.. */
	jwt->head->token = NULL;
	jwt->body->token = NULL;
	jwt->sig = NULL;	
	if( !jwt_has_error(jwt) )
		jwt_set_error(jwt, JWT_ERR_INVALID);
	return -1;

}
int jwt_set_psk(jwt_t *jwt, const unsigned char *psk, int psk_len)
{
	if( !jwt || !psk || psk_len <= 0 )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	if( jwt->key )
	{
		memset(jwt->key, 0, jwt->key_len);

		SCX_FREE(jwt->key);
		jwt->key = NULL;

	}
	jwt->key = SCX_CALLOC(1, psk_len+1);
	if( !jwt->key )
	{
		jwt_set_error(jwt, JWT_ERR_NOMEM);
		errno = ENOMEM;
		return -1; 
	}
	memcpy(jwt->key, psk, psk_len);
	jwt->key_len = psk_len;
	jwt_set_no_error(jwt);
	return 0;
}
int jwt_decode(jwt_t *jwt, const char *token, const unsigned char *psk, int psk_len)
{
	int ret = EINVAL;

	if( !jwt )
	{
		errno = EINVAL;
		return -1;
	}

	ret = jwt_parse_token(jwt, token);
	if( ret != 0 )
		goto done; 

	if( psk_len && psk )
	{
		if( jwt_set_psk(jwt, psk, psk_len) != 0 )
			goto done;
	}

	ret = jwt_verify_head(jwt);
	if (ret)
		goto done;

	ret = jwt_verify_body(jwt);
	if (ret)
		goto done;

	/*
	 * signature가 없는 케이스에 대한 처리
	 * 1. "alg" == "none"
	 * 	PSK가 설정되어 있다면, 실패처리
	 * 	PSK가 설정되어 있지 않다면...?
	 * 2. "alg" != "none"
	 * 	무조건 실패 처리
	 */
	if( jwt->alg == JWT_ALG_NONE )
	{
		if( psk || psk_len > 0 )
		{
			jwt_set_error_str(jwt, JWT_ERR_INVALID, "\"alg\" claim is \"none\"");
			ret = -1;
			goto done;
		}
	}
	if( jwt->alg != JWT_ALG_NONE 
		&& psk 
		&& psk_len > 0
		&& jwt_is_opt_set(jwt, JWT_OPT_VERIFY_SIG) ) {
		ret = jwt_verify_sig(jwt, psk, psk_len); 
	}
	else {
		ret = 0;
	}

done:
	if( !ret )
		jwt_set_no_error(jwt);

	return ret;
}
int jwt_get_claim_string(jwt_t *jwt, const char *claim, char *val, unsigned int val_len)
{
	int type;
	long long val_num;
	const char *val_str;
	if (!jwt || !claim || !strlen(claim) || !val || !val_len )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	jwt_set_no_error(jwt); 
	type = get_js_type(jwt->body->claims, claim);
	if( type == JSON_NULL )
	{
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		errno = EINVAL;
		return -1;
	}
	if( type == JSON_STRING )
	{
		val_str = get_js_string(jwt->body->claims, claim);
		if( !val_str )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
			errno = EINVAL;
			return -1;
		}
		if( strlen(val_str) > val_len )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "claim's value buffer is too small");
			errno = EINVAL;
			return -1;
		}
		strncpy(val, val_str, (strlen(val_str) > val_len ? val_len : strlen(val_str)) );
		val[strlen(val)] = '\0';
		
	}
	else if( type == JSON_INTEGER )
	{
		val_num = get_js_number(jwt->body->claims, claim);
		if( !val_num )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
			errno = EINVAL;
			return -1;
		} 
		snprintf(val, val_len, "%lld", val_num);
		val[strlen(val)] = '\0';
	}
	else
	{
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "not supported claim's value type");
		errno = EINVAL;
		return -1;
	}
	jwt_set_no_error(jwt);
	return 0;
}
static int get_number_len(long long value){
	int l = 1;
	int minus = 0;
	
	if( value < 0 )
	{
		value *= -1;
		minus = 1;	
	}
	while( value > 9 )
	{ 
		l++; 
		value /= 10;
	}
	if( minus )	
		l++;
	
	return l;
}
int jwt_get_claim_string_length(jwt_t *jwt, const char *claim)
{
	int type, len = -1;
	long long val_num;
	const char *val_str;
	if (!jwt || !claim || !strlen(claim) )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	/* set default error code */
	jwt_set_error(jwt, JWT_ERR_INVALID);
	errno = EINVAL;

	type = get_js_type(jwt->body->claims, claim);
	if( type == JSON_NULL )
	{
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		errno = EINVAL;
		return -1;
	}
	if( type == JSON_STRING )
	{
		val_str = get_js_string(jwt->body->claims, claim);
		if( !val_str )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
			errno = EINVAL;
			return -1;
		}
		/* 문자열의 길이만 반환 */ 
		len = strlen(val_str);	
		jwt_set_no_error(jwt);
	}
	else if( type == JSON_INTEGER )
	{
		val_num = get_js_number(jwt->body->claims, claim);
		if( !val_num )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
			errno = EINVAL;
			return -1;
		} 
		/* 숫자의 길이만 반환 */
		len = get_number_len(val_num);
		jwt_set_no_error(jwt);
	}
	else
	{
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "not supported claim's value type");
		errno = EINVAL;
		return -1;
	}
	return len;

}
int jwt_get_parameter(jwt_t *jwt, const char *param, char *value, unsigned int value_len)
{
	int type;
	long long val_num;
	const char *val_str;
	if (!jwt || !param|| !strlen(param) || !value || !value_len )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	jwt_set_no_error(jwt); 
	type = get_js_type(jwt->head->claims, param);
	if( type == JSON_NULL )
	{
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target parameter does not exist");
		errno = EINVAL;
		return -1;
	}
	if( type == JSON_STRING )
	{
		val_str = get_js_string(jwt->head->claims, param);
		if( !val_str )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target parameter does not exist");
			errno = EINVAL;
			return -1;
		}
		if( strlen(val_str) > value_len )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "value buffer is too small");
			errno = EINVAL;
			return -1;
		}
		strncpy(value, val_str, value_len);
		
	}
	else if( type == JSON_INTEGER )
	{
		val_num = get_js_number(jwt->head->claims, param);
		if( !val_num )
		{
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target parameter does not exist");
			errno = EINVAL;
			return -1;
		} 
		snprintf(value, value_len, "%lld", val_num);
	}
	else
	{
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "not supported parameter's value type");
		errno = EINVAL;
		return -1;
	}
	return 0;
}
int jwt_get_claim_type(jwt_t *jwt, const char *claim)
{
	if (!jwt || !claim || !strlen(claim)) {
		errno = EINVAL;
		if( jwt )
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		return -1;
	}
	jwt_set_no_error(jwt); 
	errno = 0;

	return get_js_type(jwt->body->claims, claim);

} 

long long jwt_get_claim_number(jwt_t *jwt, const char *claim)
{
	long long num;
	if (!jwt || !claim || !strlen(claim)) {
		errno = EINVAL;
		if( jwt )
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		return 0;
	}

	errno = 0;
	jwt_set_no_error(jwt); 
	num = get_js_number(jwt->body->claims, claim);
	if( num <= 0 )
	{
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist"); 
	}
	return num;
}

void jwt_base64uri_encode(char *str)
{
	int len = strlen(str);
	int i, t;

	for (i = t = 0; i < len; i++) {
		switch (str[i]) {
		case '+':
			str[t++] = '-';
			break;
		case '/':
			str[t++] = '_';
			break;
		case '=':
			break;
		default:
			str[t++] = str[i];
		}
	}

	str[t] = '\0';
}
static const char basis_64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int jwt_Base64encode(char *encoded, const char *string, int len)
{
    int i;
    char *p;

    p = encoded;
    for (i = 0; i < len - 2; i += 3) {
    *p++ = basis_64[(string[i] >> 2) & 0x3F];
    *p++ = basis_64[((string[i] & 0x3) << 4) |
                    ((int) (string[i + 1] & 0xF0) >> 4)];
    *p++ = basis_64[((string[i + 1] & 0xF) << 2) |
                    ((int) (string[i + 2] & 0xC0) >> 6)];
    *p++ = basis_64[string[i + 2] & 0x3F];
    }
    if (i < len) {
    *p++ = basis_64[(string[i] >> 2) & 0x3F];
    if (i == (len - 1)) {
        *p++ = basis_64[((string[i] & 0x3) << 4)];
        *p++ = '=';
    }
    else {
        *p++ = basis_64[((string[i] & 0x3) << 4) |
                        ((int) (string[i + 1] & 0xF0) >> 4)];
        *p++ = basis_64[((string[i + 1] & 0xF) << 2)];
    }
    *p++ = '=';
    }

    *p++ = '\0';
    return p - encoded;
}

int jwt_verify_sig(jwt_t *jwt, const unsigned char *psk, int psk_len)
{
	const char *sig;
	char *sig_check = NULL;
	unsigned int len;
	char *head;
	char *buf;
	int	ret = 0;

	if( !jwt || !jwt->sig || !psk )
	{
		if( jwt )
			jwt_set_error(jwt, JWT_ERR_INVALID);
		errno = EINVAL;
		return -1;
	}
	if( jwt->alg == JWT_ALG_NONE )
	{
		return 0;
	}
	sig = jwt->sig;	//jwt token에 들어 있던 signature
	head = jwt->head->token;

	/*
	 * 체크섬을 위해, 파싱 후 "\0" 넣은 구문을 원래대로 "."으로 되돌린다.
	 */
	jwt->body->token[-1] = '.';
	ret = jwt_sign_sha_hmac(jwt, &sig_check, &len, head);
	jwt->body->token[-1] = '\0';
	if( ret != 0 )
	{
		return -1;
	}
	buf = alloca(len * 2);
	memset(buf, 0, len *2);
	jwt_Base64encode(buf, sig_check, len);
	jwt_base64uri_encode(buf);	/* jwt에서는 padding으로 쓰이는 =를 사용하지 않기 때문에 여기서 제거한다. */
//	printf("buf = %s\nsig = %s\nhead = %s\n", buf, sig, head);
	if( strcmp(buf, sig) != 0 )
	{
		jwt_set_error_str(jwt, JWT_ERR_SIG, "failed due to invalid signature");
		errno = EACCES;
		ret = -1;
	}
	if (sig_check != NULL ) {
		free(sig_check);
		sig_check = NULL;
	}
	return ret;
}


int jwt_set_opt(jwt_t *jwt, int flags)
{
	if( !jwt )
	{
		errno = EINVAL;
		return -1;
	}
	/*
	 * TODO: flags range check
	 */
	jwt->flags = flags;
	jwt_set_no_error(jwt);
	return 0;
}
int jwt_errno(jwt_t *t)
{
	return t->err;
}
int jwt_is_claim_type_string(jwt_t *jwt, const char *claim)
{
	if( jwt_get_claim_type(jwt, claim) == JSON_STRING )
		return 1;
	return 0;
}
int jwt_is_claim_type_array(jwt_t *jwt, const char *claim)
{
	if( jwt_get_claim_type(jwt, claim) == JSON_ARRAY )
		return 1;
	return 0;
}
int jwt_is_claim_type_number(jwt_t *jwt, const char *claim)
{
	if( jwt_get_claim_type(jwt, claim) == JSON_INTEGER )
		return 1;
	return 0;
}
int jwt_is_claim_type_null(jwt_t *jwt, const char *claim)
{
	if( jwt_get_claim_type(jwt, claim) == JSON_NULL )
	{
		/* 
		 * JSON_NULL이면 클레임이 없는 것이므로, err 설정
		 */
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		return 1;
	}
	return 0;
}
size_t jwt_get_claim_array_size(jwt_t *jwt, const char *claim)
{
	if (!jwt || !claim || !strlen(claim)) {
		errno = EINVAL;
		if( jwt )
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		return -1;
	}
	json_t *json;
	size_t size;
	errno = 0;
	json = json_object_get(jwt->body->claims, claim);
	if( !json )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object");
		return -1;
	}
	size = json_array_size(json);
/*
 * 빈 오브젝트인 경우 에러 처리 안한다.
 */
#if 0
	if( size <= 0 )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object size");
		return -1;
	}
	jwt_set_no_error(jwt); 
	return size;
#else
	jwt_set_no_error(jwt); 
	return size;
#endif
}

int jwt_get_claim_array_n_string_length(jwt_t *jwt, const char *claim, int nelm)
{
	if (!jwt || !claim || !strlen(claim)) {
		errno = EINVAL;
		if( jwt )
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		return -1;
	}
	json_t *json;
	size_t size;
	errno = 0;
	json = json_object_get(jwt->body->claims, claim);
	if( !json )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object");
		return -1;
	}
	size = json_array_size(json);
	if( size == 0 )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object size");
		return -1;
	}
	if( size < (unsigned int)nelm )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object, requested element number was greater than array size");
		return -1;
	}
	json = json_array_get(json, nelm);
	if( !json )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get requested claim object");
		return -1;

	}
	/*
	 * 빈 값을 허용해야 하므로, 문자열 길이가 0이라도 성공 처리한다.
	 */
#if 0
	size = json_string_length(json);
	if( size <= 0 )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get requested claim object");
		return -1;
	}
	return size;
#else
	jwt_set_no_error(jwt); 
	return  json_string_length(json);
#endif 
}
int jwt_get_claim_array_n_string(jwt_t *jwt, const char *claim, int nelm, char *val, unsigned int val_len)
{
	if (!jwt || !claim || !strlen(claim) || !val) {
		errno = EINVAL;
		if( jwt )
			jwt_set_error_str(jwt, JWT_ERR_CLAIM, "target claim does not exist");
		return -1;
	}
	json_t *json;
	size_t size;
	const char *ptr;
	errno = 0;
	json = json_object_get(jwt->body->claims, claim);
	if( !json )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object");
		return -1;
	}
	size = json_array_size(json);
	if( size == 0 )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object size");
		return -1;
	}
	if( size < (unsigned int)nelm )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get claim object, requested element number was greater than array size");
		return -1;
	}
	json = json_array_get(json, nelm);
	if( !json )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get requested claim object");
		return -1;

	}
	size = json_string_length(json);
	if( size <= 0 )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get requested claim object");
		return -1;
	}
	if( size > val_len )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "claim value length length larger than buffer size");
		return -1;
	}
	ptr = json_string_value(json);
	if( !ptr )
	{
		errno = EINVAL;
		jwt_set_error_str(jwt, JWT_ERR_CLAIM, "unable to get requested claim value");
		return -1;

	}
	strncpy(val, ptr, val_len);
	jwt_set_no_error(jwt); 
	return 0;
}
