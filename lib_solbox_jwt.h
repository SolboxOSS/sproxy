/*
 * solbox JWT decode & verify library
 * <sd@solbox.com>
 */
#ifndef __LIB_SOLBOX_JWT_H__
#define __LIB_SOLBOX_JWT_H__

/*
 * TODO: 해시관련한 내부 로직을 변경해야한다.
 */
#define JWT_OPT_VERIFY_NONE		0x00000000
#define JWT_OPT_VERIFY_HEADER		0x00000001
#define JWT_OPT_VERIFY_BODY		0x00000002
#define JWT_OPT_VERIFY_SIG		0x00000004
#define JWT_OPT_VERIFY_CLAIM_EXP	0x00000008
#define JWT_OPT_VERIFY_ALL		0x0000FFFF

#define jwt_error(jwt) ( jwt && jwt->err != 0 )
typedef struct jwt_part_s jwt_part_t;
typedef struct jwt_s jwt_t;

typedef enum jwt_err {
	JWT_ERR_NONE = 0,	/* not error */
	JWT_ERR_OPTIONAL, 	/* not error */
	JWT_ERR_NOT_SUPPORTED,	/* not error */
	JWT_ERR_EXPIRED,
	JWT_ERR_INVALID,
	JWT_ERR_NOMEM,
	JWT_ERR_HEADER,
	JWT_ERR_BODY,
	JWT_ERR_SIG,
	JWT_ERR_CLAIM	
} jwt_err_t;
typedef enum jwt_alg {
	JWT_ALG_NONE = 0,
	JWT_ALG_HS256,
	JWT_ALG_HS384,
	JWT_ALG_HS512
} jwt_alg_t;

/*
 * jwt 오브젝트를 생성한다.
 */
jwt_t *jwt_new(void);
void jwt_free(jwt_t *jwt);
/*
 * jwt 오브젝트의 토큰과 head, body, PSK를 모두 삭제(!)한다
 * 단, flags는 유지한다.
 */
void jwt_wipe(jwt_t *jwt);


/*
 * jwt 오브젝트의 직전 오퍼레이션에 대한 에러 문자열을 반환한다.
 * 직전 오퍼레이션이 성공했다면, NULL을 반환한다.
 */
const char *jwt_err_str(jwt_t *jwt);
int jwt_errno(jwt_t *t);
/*
 * jwt를 디코딩하여 검증한 뒤 jwt 오브젝트를 생성, 그 결과를 반환한다.
 * psk가 NULL 이거나, psk_len이 0이면 시그니쳐에 대한 검증을 수행하지 않는다.
 */
int jwt_decode(jwt_t *jwt, const char *token, const unsigned char *psk, int psk_len);

/*
 * body의 클레임을 조회하여 문자열로 변환하여 val에 val_len 길이 만큼 복사한다.
 * 성공하면 0, 실패하면 -1을 반환한다.
 * 클레임의 값이 val_len보다 길다면 실패를 반환한다.
 *  JSON_INTEGER, JSON_STRING 타입의 값들만 지원한다.
 */
int jwt_get_claim_string(jwt_t *jwt, const char *claim, char *val, unsigned int val_len);
/*
 * body 클레임을 조회하여 문자열 길이를 구한뒤 돌려준다.
 *  JSON_INTEGER, JSON_STRING 타입의 값들만 지원한다.
 */
int jwt_get_claim_string_length(jwt_t *jwt, const char *claim);
/*
 * body의 클레임을 조회하여 long long 타입의 숫자를 반환한다.
 * 해당 값이 JSON_INTEGER 타입이 아니라면 jwt 오브젝트에 실패를 기록하고, 0을 반환한다.
 */
long long jwt_get_claim_number(jwt_t *jwt, const char *claim);
/*
 * body의 클레임을 조회하여,  JSON_ARRAY 타입인 경우 배열 크기를 반환한다.
 * 해당 값이 JSON_ARRAY 타입이 아니라면 jwt 오브젝트에 실패를 기록하고, -1을 반환 한다.
 */
size_t jwt_get_claim_array_size(jwt_t *jwt, const char *claim);

/*
 * body 클레임을 조회하여 N번째 배열 값의 크기를 돌려 준다
 */
int jwt_get_claim_array_n_string_length(jwt_t *jwt, const char *claim, int nelm);

/*
 * body 클레임을 조회하여 N번째 배열 값을 val에 val_len 만큼 복사 한다.
 * 성공하면 0, 실패하면 -1을 반환한다.
 * 클레임의 값이 val_len보다 길다면 실패를 반환한다.
 */
int jwt_get_claim_array_n_string(jwt_t *jwt, const char *claim, int nelm, char *val, unsigned int val_len);
/*
 * body의 클레임 타입을 조회하여 반환한다.
 * 클레임이 없다면 실패 (-1)를 반환하고, jwt errno은 JWT_ERR_CLAIM을 설정한다.
 */
int jwt_get_claim_type(jwt_t *jwt, const char *claim);

int jwt_is_claim_type_string(jwt_t *jwt, const char *claim);
int jwt_is_claim_type_array(jwt_t *jwt, const char *claim);
int jwt_is_claim_type_number(jwt_t *jwt, const char *claim);
int jwt_is_claim_type_null(jwt_t *jwt, const char *claim);
/*
 * head의 파라미터를 조회하여 그 값을 value에 value_len 길이 만큼 복사한다.
 */
int jwt_get_parameter(jwt_t *jwt, const char *param, char *value, unsigned int value_len);

char *jwt_dump_str(jwt_t *jwt, int pretty);
jwt_alg_t jwt_get_alg(jwt_t *jwt);

int jwt_set_opt(jwt_t *jwt, int flags);
/*
 * 디코딩된 토큰에 대한 sig만 검증을 수행한다.
 */
int jwt_verify_sig(jwt_t *jwt, const unsigned char *psk, int psk_len);

#endif /* end of #ifndef __LIB_SOLBOX_JWT_H__ */
