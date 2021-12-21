#ifndef __LUA_EMBED_H__
#define __LUA_EMBED_H__

#include "httpd.h"

#define MAX_LUA_ELEMENT_NAME_LEN	64
typedef enum {
	LUA_TYPE_VSTRING 	= 0,
	LUA_TYPE_BIT,
	LUA_TYPE_INT,
	LUA_TYPE_GEOIP
} scx_lua_type_t;


//REQUEST namespace에 들어갈 변수들을 handling하기 위한 구조체
typedef struct {
	char 			name[MAX_LUA_ELEMENT_NAME_LEN+1];
	int				type; //scx_lua_type_t에 정의된 값이 들어간다.
	size_t 			offset;
	int				readonly;	//real only인 경우는 1, 0이면 r/w가능
	char * 			(*proc)();	//type이 LUA_TYPE_GEOIP일 경우 실행된다.
} lua_req_element_t;


//SERVER namespace에 들어갈 변수들을 handling하기 위한 구조체
typedef struct {
	char 			name[MAX_LUA_ELEMENT_NAME_LEN+1];
	int				type;	//scx_lua_type_t에 정의된 값이 들어간다.
} lua_svr_element_t;

//netcache core에 header요청에 필요한  bit값 정의를 위한 구조체
typedef struct {
	char 			name[MAX_LUA_ELEMENT_NAME_LEN+1];
	unsigned int	val;	//bit값을 정수형으로 변환한 값이 들어간다.
} lua_bit_element_t;

typedef struct {	//lua callback function이 호출될때 넘겨줘야할 변수들을 여기에 정의한다.
	void		 	*cls;
    int				phase; /* site.h에 정의된 phasetype_t 값이 들어 간다. */
    void			*context;	/* phase 별로 별도로 정의된 context를 사용한다. */
} lua_main_st_t;

typedef struct {
	vstring_t 			*path; 		/* url에서의 path부분만 저장됨 */
	nc_xtra_options_t 	*argument;	/* url에 포함된 argument가 key,value 형태로 저장된다. */
	mem_pool_t 			*mpool;
	int					modified;	/* path나 argument가 수정된 경우 1을 셋팅 */
	char	 			*url;		/* url이 수정되는 경우에만 여기에 값이 들어간다. */
	int					url_len;	/* url 생성에 필요한 buffer size */
	int					counter;	/* url 생성시 사용하는 counter */
} lua_origin_context_t;


int scx_lua_client_request_phase(void *cls, const char *script);
int scx_lua_client_response_phase(void *cls, const char *script);
int scx_lua_build_header_dict();
int scx_lua_rewrite_phase(void *cls, const char *script);
int scx_lua_origin_phase(void *cls, const char *script, int trig_phase);





#endif /* __LUA_EMBED_H__ */
