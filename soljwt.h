#ifndef __SOLJWT_H__
#define __SOLJWT_H__

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
struct soljwt_context_s
{
	int start;
	int duration;
};
typedef struct soljwt_context_s soljwt_context_t;



/*
 * solstoken이 들어 있는 경우에는 stoken의 내용만을 사용하고 argument에 있는 keyword들은 모두 무시해야 하기 때문에
 * 제일 먼제 solstoken을 실행 하기 위에 제일 앞에 위치 해야 한다.
 */
enum soljwt_reserved_keyword_e {
	E_SOLJWT_UNKNOWN = -1,
	E_SOLJWT_EXPIRE,
	E_SOLJWT_PATH	,
	E_SOLJWT_ALOWIP,
	E_SOLJWT_ALOWCC,
	E_SOLJWT_DURATION,
	E_SOLJWT_PLAYSTART,
	E_SOLJWT_TOKEN_MAX
};


#define SOLJWT_EXPIRE		"exp"
#define SOLJWT_PATH			"path"
#define SOLJWT_ALOWIP		"alowip"
#define SOLJWT_ALOWCC		"alowcc"
#define SOLJWT_DURATION		"duration"
#define SOLJWT_PLAYSTART	"playstart"


#define SOLJWT_TOKEN		"token"

int soljwt_init();
int soljwt_deinit();
int soljwt_module_init();
int soljwt_module_deinit();
int soljwt_handler(nc_request_t *req);
int soljwt_check_streaming_claim(nc_request_t *req);



#endif /* __SOLJWT_H__ */
