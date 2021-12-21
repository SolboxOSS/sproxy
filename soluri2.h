#ifndef __SOLURI2_H__
#define __SOLURI2_H__

/*
 * ***** 중요 *****
 * streaming 일때에는 nc_create_request()에서 url decode를 하지만
 * cache일 때에는 url decode를 하지 않기 때문에 soluri가 정상 동작 하지 않음.
 * cache일때 url decode를 하는 문제는 고민이 필요함
 * cache일때 %XX문자를 만나면 url decoding하고 argument를 수동 파싱 하는 형태로 한다면 될듯
 */
struct tag_scx_list;
struct request_mem_pool;
/*
 * 각 HTTP 요청별로 다음과 같은 SolURI2 컨텍스트를 생성한다.
 */
struct soluri2_context_s
{
	struct request_mem_pool	*mpool;
	int 	soluriver;
	char 	*soltoken;			/* requested soltoken value */
	struct tag_scx_list *token_item;	/* soluri2_nv_t */
	struct tag_scx_list *rule_item;	/* 파싱한 soltokenrule을 배열에 soluri2_nv_t 타입으로 추가 */
	char 	*rule_string; 			/* soltokenrule 아이템으로 작성한 문자열 */
	char 	*path;				/* generated contents path */
	struct tag_scx_list *args;		/* SOLSTOKEN이 존재할 경우 사용, SOLSTOKEN에 들어 있는 값을 디코딩후 key/value로 파싱해서 넣는다. */
};
typedef struct soluri2_context_s soluri2_context_t;

struct soluri2_nv_s{
	int idx;
	char *name;
	char *value;
};
typedef struct soluri2_nv_s soluri2_nv_t;

/*
 * solstoken이 들어 있는 경우에는 stoken의 내용만을 사용하고 argument에 있는 keyword들은 모두 무시해야 하기 때문에
 * 제일 먼제 solstoken을 실행 하기 위에 제일 앞에 위치 해야 한다.
 */
enum soluri2_reserved_keyword_e {
	E_SOLURI2_UNKNOWN = -1,
	E_SOLURI2_SOLSTOKEN,
	E_SOLURI2_SOLTOKEN,
	E_SOLURI2_SOLTOKENRULE,
	E_SOLURI2_SOLEXPIRE,
	E_SOLURI2_SOLUUID,
	E_SOLURI2_SOLPATH,
	E_SOLURI2_SOLURIVER,
	E_SOLURI2_SOLPATHLEN,
	E_SOLURI2_TOKEN_MAX
};

#define SOLURI2_SOLSTOKEN		"solstoken"  /* added by SolURI 2.1 spec */
#define SOLURI2_SOLTOKEN		"soltoken"
#define SOLURI2_SOLTOKENRULE	"soltokenrule"
#define SOLURI2_SOLEXPIRE		"solexpire"
#define SOLURI2_SOLUUID			"soluuid"
#define SOLURI2_SOLPATH			"solpath"    /* deprecated by SolURI 2.2 spec */
#define SOLURI2_SOLURIVER		"soluriver"  /* added by SolURI 2.2 spec */
#define SOLURI2_SOLPATHLEN		"solpathlen" /* added by SolURI 2.2 spec */
#define SOLURI2_SOLTOKENRULE_DELIM 	"|"
#define SOLURI2_SOLTOKENRULE_DELIM_CHAR	'|'

int soluri2_init();
int soluri2_deinit();
int soluri2_module_init();
int soluri2_module_deinit();
int soluri2_handler(nc_request_t *req);



#endif /* __SOLURI2_H__ */
