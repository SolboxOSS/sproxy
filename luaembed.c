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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <microhttpd.h>
#include <unistd.h>
#include <search.h>
#include <errno.h>
//#include <ncapi.h>
//#include <trace.h>
#include <getopt.h>
/* Include the Lua API header files. */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "luaembed.h"
#include "setup_vars.h"
#include "luapool.h"
#include "scx_util.h"


/******************************************************
 * 여기부터 LUA 테스트 코드임
 ******************************************************/
//lua 콜백 function에서 request를 참조하기 위해서는 따로 선언을 해주어야 한다.
//lua에서 호출되는 api는 return값을 성공일때 1 실패 일때 0을 해주어야 정상 동작한다.

static void scx_lua_set_req_p( lua_State* L, lua_main_st_t *lua_main);
static lua_main_st_t * scx_lua_get_req_p( lua_State* L );

static int scx_lua_header_set(lua_State *L);
static int scx_lua_header_get(lua_State *L);
static int scx_lua_request_set(lua_State *L);
static int scx_lua_request_get(lua_State *L);
static int scx_lua_conn_argument_set(lua_State *L);
static int scx_lua_conn_argument_get(lua_State *L);
static int scx_lua_conn_header_set(lua_State *L);
static int scx_lua_conn_header_get(lua_State *L);
static int scx_lua_response_set(lua_State *L);
static int scx_lua_response_get(lua_State *L);
static int scx_lua_server_set(lua_State *L);
static int scx_lua_server_get(lua_State *L);
static int scx_lua_request_geoip(void *p, nc_request_t *req, lua_req_element_t *res);
static int scx_lua_util_nextaction(lua_State *L);
static int scx_lua_util_set_pttl(lua_State *L);
static int scx_lua_util_set_nttl(lua_State *L);
static int scx_lua_solproxy_get(lua_State *L);
static int scx_lua_solproxy_set(lua_State *L);
static int scx_hyphen_to_underbar(char * str);
static int scx_underbar_to_hyphen(char * str);
static int scx_lua_req_comparei(const void *a, const void *b);
static int scx_lua_svr_comparei(const void *a, const void *b);
static int scx_lua_bit_comparei(const void *a, const void *b);
static int scx_lua_print(lua_State* L);

static int scx_lua_origin_header_set(lua_State *L);
static int scx_lua_origin_header_get(lua_State *L);
static int scx_lua_origin_argument_set(lua_State *L);
static int scx_lua_origin_argument_get(lua_State *L);

LUALIB_API int luaopen_bit(lua_State *L);

extern lua_pool_t 		*gscx__lua_pool;

#define 	LUA_REQ_GLOBAL_NAME		"global_lua_request"

#define REQ_OFFSET( m) ((size_t)(&((nc_request_t *)0)->m))
lua_req_element_t __lua_req_tags[] = {
//		{name,				type, 				offset, 			readonly,	proc},
		{"zmethod",  		LUA_TYPE_VSTRING,	REQ_OFFSET(zmethod),		1, NULL},
		{"host", 			LUA_TYPE_VSTRING, 	REQ_OFFSET(host),			0, NULL},
		{"version",			LUA_TYPE_VSTRING, 	REQ_OFFSET(version),		1, NULL},
		{"path",			LUA_TYPE_VSTRING, 	REQ_OFFSET(path),			1, NULL},
		{"url",				LUA_TYPE_VSTRING, 	REQ_OFFSET(url),			1, NULL},
		{"ori_url",			LUA_TYPE_VSTRING, 	REQ_OFFSET(ori_url),		0, NULL},
		{"client_ip", 		LUA_TYPE_VSTRING, 	REQ_OFFSET(client_ip),		1, NULL},
		{"object_key", 		LUA_TYPE_VSTRING, 	REQ_OFFSET(object_key),		0, NULL},
		{"property_key",	LUA_TYPE_VSTRING, 	REQ_OFFSET(property_key),	0, NULL},
		{"nocache", 		LUA_TYPE_BIT, 		(size_t)0,					0, NULL},
		{"norandom", 		LUA_TYPE_BIT, 		(size_t)0,					0, NULL},
		{"private", 		LUA_TYPE_BIT, 		(size_t)0,					0, NULL},
		{"skip_auth", 		LUA_TYPE_BIT, 		(size_t)0,					0, NULL},
		{"connect_type", 	LUA_TYPE_INT, 		REQ_OFFSET(connect_type),	1, NULL},
		{"mode", 			LUA_TYPE_INT, 		REQ_OFFSET(mode),			0, NULL},
		{"limit_rate", 			LUA_TYPE_INT, 	REQ_OFFSET(limit_rate),			0, NULL},
		{"limit_rate_after", 	LUA_TYPE_INT, 	REQ_OFFSET(limit_rate_after),	0, NULL},
		{"limit_traffic_rate", 	LUA_TYPE_INT, 	REQ_OFFSET(limit_traffic_rate),	0, NULL},
		//여기부터는 geo ip handler function이다
		{"geoip_city_country_code",  	LUA_TYPE_GEOIP, 	(size_t)0,		1,&gip_lookup_country2},
		{"geoip_city_country_code3",   	LUA_TYPE_GEOIP, 	(size_t)0,		1,&gip_lookup_country3},
		{"geoip_city_country_name",   	LUA_TYPE_GEOIP, 	(size_t)0,		1,&gip_lookup_country_name},

/*		{"geoip_region",   				LUA_TYPE_GEOIP, 	(size_t)0,		1,&gip_lookup_region},
 		{"geoip_postal_code",   		LUA_TYPE_GEOIP, 	(size_t)0,		1,&scx_lua_request_postal_code},
		{"geoip_city_continent_code",   LUA_TYPE_GEOIP, 	(size_t)0,		1,&scx_lua_request_continent_code},
		{"geoip_latitude",   			LUA_TYPE_GEOIP, 	(size_t)0,		1,&scx_lua_request_latitude},
		{"geoip_longitude",   			LUA_TYPE_GEOIP, 	(size_t)0,		1,&scx_lua_request_longitude},
		{"geoip_dma_code",   			LUA_TYPE_GEOIP, 	(size_t)0,		1,&scx_lua_request_dma_code},
		{"geoip_area_code",  		 	LUA_TYPE_GEOIP, 	(size_t)0,		1,&scx_lua_request_area_code}

		{"geoip_city",   				LUA_TYPE_GEOIP, 	(size_t)0,		1,&gip_lookup_city}
*/
		{"geoip_isp",   				LUA_TYPE_GEOIP, 	(size_t)0,		1,&gip_lookup_isp}
};
#define 	LUA_REQ_DICT_COUNT 		howmany(sizeof(__lua_req_tags), sizeof(lua_req_element_t))
#define 	GET_REQ_FIELD(req,offset)  	((char*)req + offset)

lua_svr_element_t __lua_svr_tags[] = {
		{SV_ORIGIN, 			LUA_TYPE_VSTRING},
		{SV_DOMAIN, 			LUA_TYPE_VSTRING},
		{SV_PROXY,				LUA_TYPE_VSTRING},
		{SV_FOLLOW_REDIR,		LUA_TYPE_INT},
		{SV_NEGATIVE_TTL,		LUA_TYPE_INT},
		{SV_POSITIVE_TTL,		LUA_TYPE_INT},
		{SV_READAHEAD_MB,		LUA_TYPE_INT},
		{SV_CERTIFICATE_CRT,	LUA_TYPE_VSTRING},
		{SV_INODE_SIZE,			LUA_TYPE_INT},
		{SV_CHUNK_SIZE,			LUA_TYPE_INT},
		{SV_WRITEBACK_CHUNKS,	LUA_TYPE_INT},
		{SV_DRIVER_CLASS,		LUA_TYPE_VSTRING},
		{SV_CACHE_PATH,			LUA_TYPE_VSTRING},
		{SV_WORKERS,			LUA_TYPE_INT},
		{SV_POOL_SIZE,			LUA_TYPE_INT},
		{SV_CLUSTER_IP,			LUA_TYPE_VSTRING},
		{SV_CLUSTER_TTL,		LUA_TYPE_INT},
		{SV_LOG_DIR,			LUA_TYPE_VSTRING},
		{SV_HOST_NAME,			LUA_TYPE_VSTRING},
		{SV_HTTPS_PORT,			LUA_TYPE_INT},
		{SV_HTTP_PORT,			LUA_TYPE_INT},
		{SV_USE_HEAD,			LUA_TYPE_INT},
		{SV_LOG_FORMAT,			LUA_TYPE_INT},
		{SV_GEOIP_COUNTRY,		LUA_TYPE_INT},
		{SV_GEOIP_CITY,			LUA_TYPE_INT},
		{SV_GEOIP_ISP,			LUA_TYPE_INT}

};
#define 	LUA_SVR_DICT_COUNT 		howmany(sizeof(__lua_svr_tags), sizeof(lua_svr_element_t))

lua_bit_element_t  __lua_bit_tags[] = {
		{"NCX_NOCACHE",			O_NCX_NOCACHE},
		{"DONTCHECK_ORIGIN",	O_NCX_DONTCHECK_ORIGIN},
		{"SEQUENTIAL",			O_NCX_NORANDOM},
		{"NORANDOM",            O_NCX_NORANDOM},
		{"REFRESH_STAT",		O_NCX_REFRESH_STAT},
		{"MUST_REVAL",			O_NCX_MUST_REVAL}
};
#define 	LUA_BIT_DICT_COUNT 		howmany(sizeof(__lua_bit_tags), sizeof(lua_bit_element_t))


const struct luaL_Reg 	__request_symbols[] = {
	{"__index", scx_lua_request_get},
	{"__newindex", scx_lua_request_set},
	{NULL, NULL}
};
const struct luaL_Reg 	__response_symbols[] = {
	{"__index", scx_lua_response_get},
	{"__newindex", scx_lua_response_set},
	{NULL, NULL}
};
const struct luaL_Reg __solproxylib[] = {
  {"nextaction", scx_lua_util_nextaction},
  {"set_pttl", scx_lua_util_set_pttl},
  {"set_nttl", scx_lua_util_set_nttl},
  {NULL, NULL} /* end of array */
};
const struct luaL_Reg __printlib[] = {
  {"print", scx_lua_print},
  {NULL, NULL} /* end of array */
};


int scx_lua_client_request_phase(void *cls, const char *script)
{
	lua_main_st_t  main_st = {cls, LUA_PHASE_CLIENT_REQUEST, NULL} ;
	nc_request_t *req = (nc_request_t *)cls;
	lua_main_st_t * lua_main = &main_st;
	lua_State *L = NULL;
	lua_elment_t * element = NULL;
	int ret = 0;
	const struct luaL_Reg 	__header_symbols[] = {
		{"__index", scx_lua_header_get},
		{"__newindex", scx_lua_header_set},
		{NULL, NULL}
	};
	const struct luaL_Reg 	__argument_symbols[] = {
			{"__index", scx_lua_conn_argument_get},
			{"__newindex", scx_lua_conn_argument_set},
			{NULL, NULL}
	};

	const struct luaL_Reg 	__server_symbols[] = {
		{"__index", scx_lua_server_get},
		{"__newindex", scx_lua_server_set},
		{NULL, NULL}
	};

	const struct luaL_Reg __solproxyattr[] = {
		{"__index", scx_lua_solproxy_get},
		{"__newindex", scx_lua_solproxy_set},
		{NULL, NULL} /* end of array */
	};

	TRC_BEGIN((__func__));


//	TRACE((T_DEBUG, "[%llu] lua script = %s\n", req->id, script));


	if(gscx__lua_pool == NULL)
	{
		L = luaL_newstate();
		// load the libs
		luaL_openlibs(L);
	}
	else
	{
		element = lua_pool_get_element(gscx__lua_pool);
		if(element == NULL)
		{
			TRACE((T_ERROR|T_TRIGGER, "[%llu] failed to get free lua state\n", req->id));
			//printf("error : %s\n",lua_tostring(L, -1));
			ret = -1;
			goto close_state;
		}
		L = (lua_State * )element->data;
	}
//	lua_createtable(L, 0 /* narr */, 20 /* nrec */);
	lua_settop(L,0);
	scx_lua_set_req_p(L, lua_main); //lua_main 포인터를 global 변수 할당한다.


	//아래 부분은 순서를 바꾸면 동작 하지 않는다. -_-
	lua_settop(L,0);
	luaL_newmetatable(L, "BIT");
	luaopen_bit(L);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "BIT", __solproxyattr);
#else
	luaL_setfuncs(L, __solproxyattr, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "BIT");
	//여기까지 순서를 바꾸면 안됨


	lua_settop(L,0);
	luaL_newmetatable(L, "HEADER");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "HEADER", __header_symbols);
#else
	luaL_setfuncs(L, __header_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "HEADER");

	/* URI 에 포함된 GET 파라미터 */
	lua_settop(L,0);
	luaL_newmetatable(L, "ARGUMENT");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "ARGUMENT", __argument_symbols);
#else
	luaL_setfuncs(L, __argument_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "ARGUMENT");

	lua_settop(L,0);
	luaL_newmetatable(L, "REQUEST");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "REQUEST", __request_symbols);
#else
	luaL_setfuncs(L, __request_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "REQUEST");

	lua_settop(L,0);
	luaL_newmetatable(L, "RESPONSE");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "RESPONSE", __response_symbols);
#else
	luaL_setfuncs(L, __response_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "RESPONSE");

	lua_settop(L,0);
	luaL_newmetatable(L, "SERVER");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "SERVER", __server_symbols);
#else
	luaL_setfuncs(L, __server_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "SERVER");

	lua_settop(L,0);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "UTIL", __solproxylib);
#else
	luaL_setfuncs(L, __solproxylib, 0);
#endif


	lua_settop(L,0);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "_G", __printlib);
#else
	luaL_setfuncs(L, __printlib, 0);
#endif
	lua_settop(L,0);


//	ret = luaL_dostring(L, script); //luaL_loadstring()과 lua_pcall()를 합친 function
	ret = luaL_loadstring(L, script); //lua script를 메모리에 load하고  문법을 검사한다.
	if(ret > 0 ) //return이 0보다 큰 경우 script의 문법이 잘못됐거나 파일을 읽을수 없는 경우이다.
	{
		TRACE((T_ERROR|T_TRIGGER, "[%llu] error in lua script loading (%s)\n", req->id, lua_tostring(L, -1)));
		//printf("error : %s\n",lua_tostring(L, -1));
		ret = -1;
		goto close_state;
	}
	ret = lua_pcall(L, 0, LUA_MULTRET, 0); //lua를 실행한다.
	if(ret > 0 )
	{
		TRACE((T_ERROR|T_TRIGGER, "[%llu] error in lua script run (%s)\nscript = %s", req->id, lua_tostring(L, -1), script));
		ret = -1;
		goto close_state;
	}

	if(req->scx_res.trigger == LUA_TRIGGER_RETURN &&
			(req->scx_res.code < 100 || req->scx_res.code > 600 )) {
		//LUA_TRIGGER_RETURN이고  error code가 정상적으로 셋팅 되지 않은 경우는
		// 400에러를 리턴한다..
		TRACE((T_TRIGGER, "[%llu] invalid http return code(%d)\n", req->id, req->scx_res.code));
		req->scx_res.code = MHD_HTTP_BAD_REQUEST;
	}

	ret = 0;

close_state :
	if(element == NULL)
	{
		//lua state pool을 사용하지 않는 경우에는 직접 close 한다.
		if(L != NULL) lua_close(L);
	}
	else
	{
		lua_pool_release_element(element);
	}
	TRC_END;
	return ret;
}

//request struct pointer를 lua에서 call 되는 function들에 넘겨주기 위한  function임
static void scx_lua_set_req_p( lua_State* L, lua_main_st_t *lua_main)
{
	lua_pushlightuserdata(L, (void *)lua_main);  /* push address */
    lua_setglobal( L, LUA_REQ_GLOBAL_NAME );
}

static lua_main_st_t * scx_lua_get_req_p( lua_State* L )
{
    lua_getglobal( L, LUA_REQ_GLOBAL_NAME );
    lua_main_st_t * lua_main = (lua_main_st_t *)lua_touserdata(L, -1);
    lua_pop( L, 1 );
    return lua_main;
}

static int
scx_lua_header_set(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	char 	*key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다. lua에서 변수로  '-'를 허용하지 않기 때문에 '_' 대신 사용한다.
	const char  *val = NULL;
	//항상  nil인지 확인해야 한다.
	if (lua_isnil(L, 3)) {	//변수에 nil을 직접 할당 하거나 변수만 선언한 경우는 모두 nil이다
		//값이 할당 되지 않는 경우 해당 key를 hash에서 지운다.
		kv_remove(req->options, key, 0);
		TRC_END;
		return 1;
	}
	else {
		val = luaL_checkstring(L, 3); //lua로 부터 value 값을 읽어 온다.
	}

	kv_replace(req->options, key, val, 0);
	TRC_END;
	return 1;
}

static int
scx_lua_header_get(lua_State *L)
{
	//request 정보를 가져 온다
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다.

	const char *val = NULL;

	val = kv_find_val_extened(req->options, key, 0, 1);
	if(val == NULL){
		TRACE((T_TRIGGER, "[%llu] failed to find header tag '%s'\n", req->id, key));
		lua_pushnil(L);
		//lua_pushstring(L, "");
	}
	else {
		TRACE((T_TRIGGER, "[%llu] find header tag '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	TRC_END;
	return 1;
}


static int
scx_lua_conn_argument_set(lua_State *L)
{
	/*
	 * request URL에 포함된 GET 파라미터(argument)는 개별적으로 수정을 할수가 없다.
	 * 파라미터의 수정이 필요한 경우 REQUEST.ori_url을 새로 만드는 방식을 사용해야 한다.
	 * 사용 예 :
	 *  if (ARGUMENT.token ~= nil) and (ARGUMENT.expr ~= nil) then
	 *		  	REQUEST.ori_url=REQUEST.path.."?token="..ARGUMENT.token.."&expr="..ARGUMENT.expr.."&ARG=1234"
	 *  end
	 *
	 */
	return 1;
}

/* MHD의 connection으로 부터 직접 url에 포함된 argument를 읽는다. */
static int
scx_lua_conn_argument_get(lua_State *L)
{
	//request 정보를 가져 온다
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get argument data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다.

	const char *val = NULL;

	val = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, key);
	if(val == NULL){
		TRACE((T_TRIGGER, "[%llu] failed to find argument  '%s'\n", req->id, key));
		lua_pushnil(L);
		//lua_pushstring(L, "");
	}
	else {
		TRACE((T_TRIGGER, "[%llu] find argument '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	TRC_END;
	return 1;
}

/* MHD의 connection에 헤더를 직접 변경한다. */
static int
scx_lua_conn_header_set(lua_State *L)
{
	/*
	 * MHD_set_connection_value()를 사용해서 값이 변경가능.
	 * 변경은 MHD AccessHandlerCallback 단계에서만 유효하다.
	 * 변경시 넘겨주는 key와 value는 connection이 종료 될때까지 유효한 문자열이어야 한다.
	 * 현재는 설정이 불가능 하도록 한다.
	 */
	return 1;
}
/* MHD의 connection으로 부터 직접 헤더를 읽는다. */
static int
scx_lua_conn_header_get(lua_State *L)
{
	//request 정보를 가져 온다
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get argument data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다.

	const char *val = NULL;

	val = MHD_lookup_connection_value(req->connection, MHD_HEADER_KIND, key);
	if(val == NULL){
		TRACE((T_TRIGGER, "[%llu] failed to find header '%s'\n", req->id, key));
		lua_pushnil(L);
		//lua_pushstring(L, "");
	}
	else {
		TRACE((T_TRIGGER, "[%llu] find header '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	TRC_END;
	return 1;
}

static int
scx_lua_request_set(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	const char *key = luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.

	lua_req_element_t *res = NULL;
	lua_req_element_t qry ;
	strncpy(qry.name, key, MAX_LUA_ELEMENT_NAME_LEN);

	//res = bsearch(&qry, lua_main->elm, lua_main->elmcnt, sizeof(lua_req_element_t), scx_lua_req_comparei);
	res = bsearch(&qry, __lua_req_tags, LUA_REQ_DICT_COUNT, sizeof(lua_req_element_t), scx_lua_req_comparei);
	if (res == NULL) {
		TRACE((T_TRIGGER, "[%llu] failed to find request tag '%s'\n", req->id, key));
		TRC_END;
		return 0;
	}

	const char *val = NULL;
	int num;
	if(res->readonly == 0) {	/* 읽기 전용인 경우에는 아래의 과정을 skip한다. */
		//TRACE((T_TRIGGER, "REQUEST.%s being changed\n", key));
		switch (res->type) {
			case LUA_TYPE_VSTRING:
							//항상  nil인지 확인해야 한다.
				if (lua_isnil(L, 3)) {	//변수에 nil을 직접 할당 하거나 변수만 선언한 경우는 모두 nil이다
					*((vstring_t **)GET_REQ_FIELD(req, res->offset)) = vs_strdup_lg(req->pool, "");
					TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is deleted.\n", req->id, key));

				}
				else {
					val = luaL_checkstring(L, 3); //lua로 부터 value 값을 읽어 온다.
					*((vstring_t **)GET_REQ_FIELD(req, res->offset)) = vs_strdup_lg(req->pool, val);
					TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is set to '%s'\n", req->id, key, val));
				}

				break;
			case LUA_TYPE_INT:
				if (lua_isnil(L, 3)) {
					*((mode_t **)GET_REQ_FIELD(req, res->offset)) = 0;
					TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is deleted.\n", req->id, key));
				}
				else {
					num = luaL_checkint(L, 3);
					//*((mode_t **)GET_REQ_FIELD(lua_main->cls, res->offset)) = num;
					*((mode_t **)((char*)req + res->offset)) = num;
					TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is set to '%d'\n", req->id, key, num));
				}
				break;
			case LUA_TYPE_BIT:
				//bit형 변수인 경우 변수 포인터를 사용할수 없다.
				if(strcasecmp(key, "nocache") == 0) {
					if (lua_isnil(L, 3)) {
						req->nocache = 0;
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is deleted.\n", req->id, key));
					}
					else {
						num = luaL_checkint(L, 3);
						req->nocache = (num == 0)? 0 : 1; //값이 0이외에는 모두 1로 변환한다.
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is set to '%d' (num=%d)\n", req->id, key, req->nocache, num));
					}
				}
				else if (strcasecmp(key, "norandom") == 0) {
					if (lua_isnil(L, 3)) {
						req->norandom = 0;
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is deleted.\n", req->id, key));
					}
					else {
						num = luaL_checkint(L, 3);
						if(num < 0 || num > 2) num = 0;
						req->norandom = num;
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is set to '%d' (num=%d)\n", req->id, key, req->norandom, num));
					}
				}
				else if (strcasecmp(key, "private") == 0) {
					if (lua_isnil(L, 3)) {
						req->private = 0;
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is deleted.\n", req->id, key));
					}
					else {
						num = luaL_checkint(L, 3);
						req->private = (num == 0)? 0 : 1; //값이 0이외에는 모두 1로 변환한다.
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is set to '%d' (num=%d)\n", req->id, key, req->private, num));
					}
				}
				else if (strcasecmp(key, "skip_auth") == 0) {
					if (lua_isnil(L, 3)) {
						req->skip_auth = 0;
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is deleted.\n", req->id, key));
					}
					else {
						num = luaL_checkint(L, 3);
						req->skip_auth = (num == 0)? 0 : 1; //값이 0이외에는 모두 1로 변환한다.
						TRACE((T_DEBUG|T_TRIGGER, "[%llu] request tag '%s' is set to '%d' (num=%d)\n", req->id, key, req->skip_auth, num));
					}
				}
				break;
			default:
				TRACE((T_TRIGGER, "[%llu] failed to set request tag '%s'\n", req->id, key));
				break;
		}
	}
	TRC_END;
	return 1;

}

static int
scx_lua_request_get(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	const char *key = luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.

	lua_req_element_t *res = NULL;
	lua_req_element_t qry ;
	strncpy(qry.name, key, MAX_LUA_ELEMENT_NAME_LEN);

	res = bsearch(&qry, __lua_req_tags, LUA_REQ_DICT_COUNT, sizeof(lua_req_element_t), scx_lua_req_comparei);
	if (res == NULL) {
		TRACE((T_TRIGGER, "[%llu] failed to find request tag '%s'\n", req->id, key));
		TRC_END;
		return 0;
	}
	const char *val = NULL;
	vstring_t * tmpstr = NULL;
	int num = 0;

	switch (res->type) {
		case LUA_TYPE_VSTRING:

			tmpstr = *((vstring_t **)GET_REQ_FIELD(req, res->offset));

			if(tmpstr == NULL){
				lua_pushnil(L);	//nil을 script에서 출력하려고 할 경우 script가 멈춘다.
				//lua_pushstring(L, "");
				TRACE((T_TRIGGER, "[%llu] failed to find request tag '%s'\n", req->id, key));
				break;
			}
			val = vs_data(tmpstr);
			if(val == NULL){
				lua_pushnil(L);
				//lua_pushstring(L, "");
				TRACE((T_TRIGGER, "[%llu] failed to find request tag '%s'\n", req->id, key));
			}
			else {
//				TRACE((T_INFO, "======== get key= REQUEST.'%s' => value(%s)\n", key, val));
				TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%s'\n", req->id, key, val));
				lua_pushstring(L, val);
			}
			break;
		case LUA_TYPE_INT:
			num = *(mode_t **)GET_REQ_FIELD(req, res->offset);
			lua_pushinteger(L, num);
			TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%d'\n", req->id, key, num));
			break;
		case LUA_TYPE_BIT:
			//bit형 변수인 경우 변수 포인터를 사용할수 없다.
			if(strcasecmp(key, "nocache") == 0) {
				num = req->nocache;
			}
			else if(strcasecmp(key, "norandom") == 0) {
				num = req->norandom;
			}
			else if(strcasecmp(key, "private") == 0) {
				num = req->private;
			}
			else if(strcasecmp(key, "skip_auth") == 0) {
				num = req->skip_auth;
			}
			else {
				break;
			}
			lua_pushinteger(L, num);
			TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%d'\n", req->id, key, num));
			break;
		case LUA_TYPE_GEOIP:
			if (res->proc) {
					//(*res->proc)((void *)L, req);
				scx_lua_request_geoip((void *)L, req, res);
			}
			else {
				TRACE((T_TRIGGER, "[%llu] geoip tag '%s' handler not defined\n", req->id, key));
			}
			break;
		default :
			TRACE((T_TRIGGER, "[%llu] undefined key(%s) type(%d)\n", req->id, key, res->type));
			break;
	}
	TRC_END;
	return 1;
}


static int scx_lua_request_geoip(void *p, nc_request_t *req, lua_req_element_t *res)
{
	lua_State *L = (lua_State *)p;
	TRC_BEGIN((__func__));
	char buf[512] = {'\0'};
	char * retbuf = NULL;

	TRACE((T_TRIGGER, "client ip = '%s' \n", vs_data(req->client_ip) ));
	retbuf = (*res->proc)(vs_data(req->client_ip), buf);
	if(retbuf == NULL) {
//		TRACE((T_ERROR, "-------------- scx_lua_request_geoip is null\n"));
		TRACE((T_TRIGGER, "failed to find geoip tag '%s' \n", res->name));
		lua_pushnil(L);
		//lua_pushstring(L, "");
	}
	else {
//		TRACE((T_ERROR, "-------------- scx_lua_request_geoip = %s\n", retbuf));
		TRACE((T_TRIGGER, "find geoip tag '%s' is '%s'\n", res->name, retbuf));
		lua_pushstring(L, retbuf);
	}
	TRC_END;
	return 1;
}

/* RESPONSE에서 변경 가능한 header
		RESPONSE.location = "http://xxxx.yyy.zzz.com/give_special_gift.html"
		RESPONSE.body("축하합니다. 이벤트에 당첨되셨습니다")
		RESPONSE.code=301
 */
static int
scx_lua_response_set(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;
	//TRACE((T_TRIGGER, "%s, gettop = %d\n", __FUNCTION__,lua_gettop(L)));
	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.
	const char *val = NULL;

	if (lua_isnil(L, 3)) {	// nil을 할당한 경우는 response에서 삭제 해야 할까?
		TRACE((T_TRIGGER, "[%llu] %s, %s is NULL\n", req->id, __FUNCTION__, key));
	}
	else {
		val = luaL_checkstring(L, 3); //lua로 부터 value 값을 읽어 온다.
	}

	if(val == NULL) {
		TRC_END;
		return 0;

	}

	if(strcasecmp(key, "code") == 0) {
		req->scx_res.code = atoi(val);
	}

	else if(strcasecmp(key, "body") == 0) {
		if (req->scx_res.body) {	/* body가 이전에 기록되어 있는 경우 해당 메모리의 해제가 먼저 이루어 진다. */
			SCX_FREE(req->scx_res.body);
			req->scx_res.body = NULL;
			req->objstat.st_size = req->scx_res.body_len = 0;
		}
		req->objstat.st_size = req->scx_res.body_len = strlen(val);
		req->scx_res.body  = SCX_CALLOC(1, req->scx_res.body_len + 1);
		strncpy(req->scx_res.body, val, req->scx_res.body_len);
		TRACE((T_TRIGGER, "[%llu] %s, scx_res.body = %s\n", req->id, __FUNCTION__, req->scx_res.body));
	}


	else {
		scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다. lua에서 변수로  '-'를 허용하지 않기 때문에 '_' 대신 사용한다.
		kv_replace(req->scx_res.headers, key, val, 0);
	}


	TRC_END;
	return 0;

}

static int
scx_lua_response_get(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;
	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.

	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다.
	const char *val = NULL;

	if(strcasecmp(key, "code") == 0) {
		lua_pushinteger (L, req->scx_res.code);
	}
	else if(strcasecmp(key, "body") == 0) {
		if(req->scx_res.body == NULL)
		{
			lua_pushnil(L);
		}
		else {
			lua_pushstring(L, req->scx_res.body);
		}
	}
	else {
		val = kv_find_val_extened(req->scx_res.headers, key, 0, 1);
		if(val == NULL){
			TRACE((T_TRIGGER, "[%llu] failed to find header tag '%s'\n", req->id, key));
			lua_pushnil(L);
			//lua_pushstring(L, "");
		}
		else {
			TRACE((T_TRIGGER, "[%llu] find header tag '%s' is '%s'\n", req->id, key, val));
			lua_pushstring(L, val);
		}
	}


	TRC_END;
	return 1;
}


static int
scx_lua_server_set(lua_State *L)
{
	TRC_BEGIN((__func__));
	TRACE((T_TRIGGER, "SERVER set is disabled '%s'\n"));
	TRC_END;
	return 1;
}

static int
scx_lua_server_get(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;
	const char *key = luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.

	lua_svr_element_t *res = NULL;
	lua_svr_element_t qry ;
	strncpy(qry.name, key, MAX_LUA_ELEMENT_NAME_LEN);

	res = bsearch(&qry, __lua_svr_tags, LUA_SVR_DICT_COUNT, sizeof(lua_svr_element_t), scx_lua_svr_comparei);
	if (res == NULL) {
		TRACE((T_TRIGGER, "[%llu] failed to find SERVER tag '%s'\n", req->id, key));
		TRC_END;
		return 0;
	}

	char 			*val = NULL;
	int				num = 0;
	val = (char *)st_get(req->service->site->st_setup, key);
	switch (res->type) {
		case LUA_TYPE_VSTRING:
			if(val == NULL){
				lua_pushnil(L);
				TRACE((T_TRIGGER, "[%llu] failed to find server tag '%s'\n", req->id, key));
			}
			else {
				lua_pushstring(L, val);
				TRACE((T_TRIGGER, "[%llu] find server tag '%s' is '%s'\n", req->id, key, val));
			}
			break;
		case LUA_TYPE_INT:
			if(val == NULL){
				lua_pushinteger(L, 0);
				TRACE((T_TRIGGER, "[%llu] failed to find server tag '%s'\n", req->id, key));
			}
			else {
				num = atoi(val);
				lua_pushinteger(L, num);
				TRACE((T_TRIGGER, "[%llu] find server tag '%s' is '%d'\n", req->id, key, num));
			}
			break;
		default:
			TRACE((T_TRIGGER, "[%llu] undefined key(%s) type(%d)\n", req->id, key, res->type));
			break;

	}
	TRC_END;
	return 1;
}

static int
scx_lua_util_nextaction(lua_State *L)
{
	TRC_BEGIN((__func__));

	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	if(lua_gettop(L) != 1) {
		TRACE((T_TRIGGER, "[%llu] nextaction must require parameter\n", req->id));
		TRC_END;
		return 0;
	}
	if (lua_isnil(L, 1)) {
		TRACE((T_TRIGGER, "[%llu] nextaction not permit nil value\n", req->id));
		TRC_END;
		return 0;
	}

	const char *val = luaL_checkstring(L, 1); //lua로 부터 key 값을 읽어 온다.
	if(val == NULL) {
		TRACE((T_TRIGGER, "[%llu] nextaction not defined.\n", req->id));
		TRC_END;
		return 0;
	}

	if(strcasecmp(val, "continue") == 0) {
		req->scx_res.trigger = LUA_TRIGGER_CONTINUE;
	}
	else if(strcasecmp(val, "break") == 0) {
		req->scx_res.trigger = LUA_TRIGGER_BREAK;
	}
	else if(strcasecmp(val, "return") == 0) {
		req->scx_res.trigger = LUA_TRIGGER_RETURN;
	}
	TRACE((T_TRIGGER, "[%llu] action is set to %d \n", req->id, req->scx_res.trigger));

	TRC_END;
	return 0;
}

static int
scx_lua_util_set_pttl(lua_State *L)
{
	nc_time_t 	pttl = 0;
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		return 0;
	}

	nc_request_t *req = (nc_request_t *)lua_main->cls;

	if (LUA_PHASE_CLIENT_REQUEST != lua_main->phase) {
		/* TTL 설정은 client request phase에서만 동작이 가능하다 */
		TRACE((T_TRIGGER, "[%llu] set_pttl is only working at client request phase.\n", req->id));
		return 0;
	}
	if(lua_gettop(L) != 1) {
		TRACE((T_TRIGGER, "[%llu] set_pttl must require parameter\n", req->id));
		return 0;
	}
	if (lua_isnil(L, 1)) {
		TRACE((T_TRIGGER, "[%llu] set_pttl not permit nil value\n", req->id));
		return 0;
	}

	const char *val = luaL_checkstring(L, 1); //lua로 부터 TTL 값을 읽어 온다.
	if(val == NULL) {
		TRACE((T_TRIGGER, "[%llu] TTL not defined.\n", req->id));
		return 0;
	}

	pttl = atoll(val);
	if (0 > pttl) {
		TRACE((T_TRIGGER, "[%llu] Invalid TTL value(%s)\n", req->id, val));
		return 0;
	}
	kv_set_pttl(req->options, pttl);

	TRACE((T_TRIGGER, "[%llu] Positive TTL is set to %d \n", req->id, pttl));

	return 0;
}

static int
scx_lua_util_set_nttl(lua_State *L)
{
	nc_time_t 	nttl = 0;
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		return 0;
	}

	nc_request_t *req = (nc_request_t *)lua_main->cls;

	if (LUA_PHASE_CLIENT_REQUEST != lua_main->phase) {
		/* TTL 설정은 client request phase에서만 동작이 가능하다 */
		TRACE((T_TRIGGER, "[%llu] set_nttl is only working at client request phase.\n", req->id));
		return 0;
	}

	if(lua_gettop(L) != 1) {
		TRACE((T_TRIGGER, "[%llu] set_nttl must require parameter\n", req->id));
		return 0;
	}
	if (lua_isnil(L, 1)) {
		TRACE((T_TRIGGER, "[%llu] set_nttl not permit nil value\n", req->id));
		return 0;
	}

	const char *val = luaL_checkstring(L, 1); //lua로 부터 TTL 값을 읽어 온다.
	if(val == NULL) {
		TRACE((T_TRIGGER, "[%llu] TTL not defined.\n", req->id));
		return 0;
	}

	nttl = atoll(val);
	if (0 > nttl) {
		TRACE((T_TRIGGER, "[%llu] Invalid TTL value(%s)\n", req->id, val));
		return 0;
	}
	kv_set_nttl(req->options, nttl);

	TRACE((T_TRIGGER, "[%llu] Negative TTL is set to %d \n", req->id, nttl));

	return 0;
}


static int scx_lua_solproxy_get(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;

	const char *key = luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.
	TRACE((T_TRIGGER, "[%llu] %s key = '%s'\n", req->id, __FUNCTION__, key));
	lua_bit_element_t *res = NULL;
	lua_bit_element_t qry ;
	size_t count = LUA_BIT_DICT_COUNT;
	strncpy(qry.name, key, MAX_LUA_ELEMENT_NAME_LEN);

	//현재는 __lua_bit_tags에 5개만 들어 있어서 lfind를 사용하지만 더 많아 질 경우는 bsearch로 바꿔야 한다.
	res = lfind(&qry, __lua_bit_tags, &count, sizeof(lua_bit_element_t), scx_lua_bit_comparei);
	if(res == NULL){
		lua_pushnil(L);
		TRACE((T_TRIGGER, "[%llu] failed to find bit tag '%s'\n", req->id, key));
	}
	else {
		lua_pushinteger(L, res->val);
		TRACE((T_TRIGGER, "[%llu] find bit tag '%s' is '%d'\n", req->id, key, res->val));
	}
	TRC_END;
	return 1;
}

static int scx_lua_solproxy_set(lua_State *L)
{
	TRC_BEGIN((__func__));
	TRACE((T_TRIGGER, "set is disabled.\n"));
	TRC_END;
	return 1;
}

static int
scx_lua_print(lua_State* L) {
    int nargs = lua_gettop(L);
    for (int i=1; i <= nargs; i++) {
        if (lua_isstring(L, i)) {
            /* Pop the next arg using lua_tostring(L, i) and do your print */
        	TRACE((T_INFO|T_DEBUG|T_TRIGGER, "%s\n", lua_tostring(L, -1)));
        }
        else {
        /* Do something with non-strings if you like */
        }
    }

    return 1;
}

static int
scx_hyphen_to_underbar(char * str)
{
	for (char *p = str ; *p; ++p){
		if(*p == '-')
		*p = '_';
	}
	return 1;
}

static int
scx_underbar_to_hyphen(char * str)
{
	for (char *p = str ; *p; ++p){
		if(*p == '_')
		*p = '-';
	}
	return 1;
}

int
scx_lua_build_header_dict()
{
	qsort(__lua_req_tags, LUA_REQ_DICT_COUNT, sizeof(lua_req_element_t), scx_lua_req_comparei);
	qsort(__lua_svr_tags, LUA_SVR_DICT_COUNT, sizeof(lua_svr_element_t), scx_lua_svr_comparei);
	return 1;
}

static int
scx_lua_req_comparei(const void *a, const void *b)
{
	lua_req_element_t 	*e1 = (lua_req_element_t *)a;
	lua_req_element_t 	*e2 = (lua_req_element_t *)b;
	return strcasecmp(e1->name, e2->name);
}

static int
scx_lua_svr_comparei(const void *a, const void *b)
{
	lua_svr_element_t 	*e1 = (lua_svr_element_t *)a;
	lua_svr_element_t 	*e2 = (lua_svr_element_t *)b;

	return strcasecmp(e1->name, e2->name);
}

static int
scx_lua_bit_comparei(const void *a, const void *b)
{

	lua_bit_element_t 	*e1 = (lua_bit_element_t *)a;
	lua_bit_element_t 	*e2 = (lua_bit_element_t *)b;
//	TRACE((T_DEBUG|T_TRIGGER, "%s, %s, %s\n", __FUNCTION__, e1->name, e2->name));
	return strcasecmp(e1->name, e2->name);
}

int
scx_lua_rewrite_phase(void *cls, const char *script)
{
	lua_main_st_t  main_st = {cls , LUA_PHASE_HOST_REWRITE, NULL} ;
	nc_request_t *req = (nc_request_t *)cls;
	lua_main_st_t * lua_main = &main_st;
	lua_State *L = NULL;
	lua_elment_t * element = NULL;

	int ret = 0;
#if 1
	const struct luaL_Reg 	__header_symbols[] = {
		{"__index", scx_lua_conn_header_get},
		{"__newindex", scx_lua_conn_header_set},
		{NULL, NULL}
	};
	const struct luaL_Reg 	__argument_symbols[] = {
		{"__index", scx_lua_conn_argument_get},
		{"__newindex", scx_lua_conn_argument_set},
		{NULL, NULL}
	};

	TRC_BEGIN((__func__));



	TRACE((T_DEBUG, "lua script = %s\n", script));

	if (gscx__lua_pool == NULL)
	{
		L = luaL_newstate();
		// load the libs
		luaL_openlibs(L);
	}
	else
	{
		element = lua_pool_get_element(gscx__lua_pool);
		if (element == NULL)
		{
			TRACE((T_ERROR|T_TRIGGER, "[%llu] failed to get free lua state\n", req->id));
			//printf("error : %s\n",lua_tostring(L, -1));
			ret = -1;
			goto close_state;
		}
		L = (lua_State * )element->data;
	}

	lua_settop(L,0);
	scx_lua_set_req_p(L, lua_main); //lua_main 포인터를 global 변수 할당한다.



	lua_settop(L,0);
	luaL_newmetatable(L, "HEADER");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "HEADER", __header_symbols);
#else
	luaL_setfuncs(L, __header_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "HEADER");


	lua_settop(L,0);
	luaL_newmetatable(L, "ARGUMENT");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "ARGUMENT", __argument_symbols);
#else
	luaL_setfuncs(L, __argument_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "ARGUMENT");

	lua_settop(L,0);
	luaL_newmetatable(L, "REQUEST");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "REQUEST", __request_symbols);
#else
	luaL_setfuncs(L, __request_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "REQUEST");

	lua_settop(L,0);
	luaL_newmetatable(L, "RESPONSE");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "RESPONSE", __response_symbols);
#else
	luaL_setfuncs(L, __response_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "RESPONSE");

	lua_settop(L,0);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "UTIL", __solproxylib);
#else
	luaL_setfuncs(L, __solproxylib, 0);
#endif



	lua_settop(L,0);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "_G", __printlib);
#else
	luaL_setfuncs(L, __printlib, 0);
#endif
	lua_settop(L,0);

	ret = luaL_loadstring(L, script); //lua script를 메모리에 load하고  문법을 검사한다.
	if (ret > 0 ) //return이 0보다 큰 경우 script의 문법이 잘못됐거나 파일을 읽을수 없는 경우이다.
	{
		TRACE((T_ERROR|T_TRIGGER, "[%llu] error in lua script loading (%s)\n", req->id, lua_tostring(L, -1)));
		//printf("error : %s\n",lua_tostring(L, -1));
		ret = -1;
		goto close_state;
	}
	ret = lua_pcall(L, 0, LUA_MULTRET, 0); //lua를 실행한다.
	if (ret > 0 )
	{
		TRACE((T_ERROR|T_TRIGGER, "[%llu] error in lua script run (%s)\nscript = %s", req->id, lua_tostring(L, -1), script));
		ret = -1;
		goto close_state;
	}
	if(req->scx_res.trigger == LUA_TRIGGER_RETURN &&
			(req->scx_res.code < 100 || req->scx_res.code > 600 )) {
		//LUA_TRIGGER_RETURN이고  error code가 정상적으로 셋팅 되지 않은 경우는
		// 400에러를 리턴한다..
		TRACE((T_TRIGGER, "[%llu] invalid http return code(%d)\n", req->id, req->scx_res.code));
		req->scx_res.code = MHD_HTTP_BAD_REQUEST;
	}

	ret = 0;

close_state :
	if (element == NULL)
	{
		//lua state pool을 사용하지 않는 경우에는 직접 close 한다.
		if(L != NULL) lua_close(L);
	}
	else
	{
		lua_pool_release_element(element);
	}
	TRC_END;
#endif
	return ret;
}

static int
scx_lua_url_parser(char * url, void * v_context)
{
	mem_pool_t 	*mpool = NULL;
	lua_origin_context_t *context = (lua_origin_context_t *)v_context;
	vstring_t 			*oriurl = NULL;
	off_t		 		off = 0;
	off_t				arg_off = 0;
	vstring_t 			*token;
	vstring_t 			*key;
	vstring_t 			*value;


	context->mpool = mp_create(strlen(url)*3 + 1024);  /* url을 만들고 각 key를 한번 이상씩 변경 가능한 크기를 할당 해야 한다. */
	context->argument	= kv_create_d(__func__, __LINE__);

	oriurl = vs_strdup_lg(context->mpool, url);
	off = vs_pickup_token(context->mpool, oriurl, off, "?", &context->path);
	if(off >= vs_length(oriurl)) {
		/* argument가 없으므로 더이상 진행 할 필요가 없음 */
		return 0;
	}
	do {
		off = vs_pickup_token(context->mpool, oriurl, off, "&", &token);
		if(token)
		{
			arg_off = vs_pickup_token_r(context->mpool, token, vs_length(token), "=", &key, DIRECTION_LEFT);
			arg_off = vs_pickup_token_r(context->mpool, token, vs_length(token), "=", &value, DIRECTION_RIGHT);
			if(arg_off > 0 && key != NULL > 0 && value != NULL > 0) {/* key, value가 하나도 없는 경우에는 skip한다. */
				kv_add_val(context->argument, vs_data(key), vs_data(value));
			}
		}
	} while (off < vs_length(oriurl));
	return 0;
}

static int
scx_lua_commit_argument(const char *key, const char *value, void *cb)
{
	lua_origin_context_t *context = (lua_origin_context_t *)cb;
	if(context->url == NULL) { /* url의 전체 length 계산을 위한 부분 */
		context->url_len += strlen(key) + strlen(value) + 2;
	}
	else {
		/* url에 argument가 붙는 경우 처음은 ?로 시작 하고 두번째 부터는 &로 시작 해야 한다.
		 * 예)http://127.0.0.1/1k_2.data?test1=arg&test2=arg2&test3=arg3
		 */
		sprintf(context->url, "%s%s%s=%s", context->url,context->counter++==0?"?":"&", key, value);

	}
	return 0;
}


static int
scx_lua_url_rebuild(char ** url, void * v_context)
{
	lua_origin_context_t *context = (lua_origin_context_t *)v_context;
	/* url 생성에 필요한 전체 length를 계산 */
	context->url_len = vs_length(context->path)+1;
	kv_for_each(context->argument, scx_lua_commit_argument, (void *)context); /* argument들의 총 필요 length를 계산 */

	context->url = calloc(context->url_len, 1); /* url은 plugin에 넘겨줄 데이타라서 기본 allocation을 사용 한다. */
	sprintf(context->url,"%s",vs_data(context->path));
	kv_for_each(context->argument, scx_lua_commit_argument, (void *)context);

//	free(*url); url에 대한 free는 httpn plugin에서 한다.
	*url = context->url;
	return 0;
}
int
scx_lua_origin_phase(void *cls, const char *script, int trig_phase)
{
	lua_main_st_t  main_st = {cls , trig_phase, NULL} ;
	nc_origin_io_command_t *command = (nc_origin_io_command_t *)cls;
	lua_main_st_t * lua_main = &main_st;
	lua_State *L = NULL;
	lua_elment_t * element = NULL;
	lua_origin_context_t *context = NULL;
	int ret = 0;
	const struct luaL_Reg 	__header_symbols[] = {
		{"__index", scx_lua_origin_header_get},
		{"__newindex", scx_lua_origin_header_set},
		{NULL, NULL}
	};
	const struct luaL_Reg 	__argument_symbols[] = {
		{"__index", scx_lua_origin_argument_get},
		{"__newindex", scx_lua_origin_argument_set},
		{NULL, NULL}
	};

	TRC_BEGIN((__func__));
	context = (lua_origin_context_t *)SCX_CALLOC(1, sizeof(lua_origin_context_t));
	main_st.context = context;
	scx_lua_url_parser(command->url, context);



	TRACE((T_DEBUG, "lua script = %s\n", script));

	if (gscx__lua_pool == NULL)
	{
		L = luaL_newstate();
		// load the libs
		luaL_openlibs(L);
	}
	else
	{
		element = lua_pool_get_element(gscx__lua_pool);
		if (element == NULL)
		{
			TRACE((T_ERROR|T_TRIGGER, "failed to get free lua state\n"));
			//printf("error : %s\n",lua_tostring(L, -1));
			ret = -1;
			goto close_state;
		}
		L = (lua_State * )element->data;
	}

	lua_settop(L,0);
	scx_lua_set_req_p(L, lua_main); //lua_main 포인터를 global 변수 할당한다.


	lua_settop(L,0);
	luaL_newmetatable(L, "HEADER");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "HEADER", __header_symbols);
#else
	luaL_setfuncs(L, __header_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "HEADER");

	lua_settop(L,0);
	lua_pushstring(L, vs_data(context->path));
	lua_setglobal(L, "PATH");

	lua_settop(L,0);
	luaL_newmetatable(L, "ARGUMENT");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "ARGUMENT", __argument_symbols);
#else
	luaL_setfuncs(L, __argument_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "ARGUMENT");

	lua_settop(L,0);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "_G", __printlib);
#else
	luaL_setfuncs(L, __printlib, 0);
#endif
	lua_settop(L,0);

	ret = luaL_loadstring(L, script); //lua script를 메모리에 load하고  문법을 검사한다.
	if (ret > 0 ) //return이 0보다 큰 경우 script의 문법이 잘못됐거나 파일을 읽을수 없는 경우이다.
	{
		TRACE((T_ERROR|T_TRIGGER, "error in lua script loading (%s)\n", lua_tostring(L, -1)));
		//printf("error : %s\n",lua_tostring(L, -1));
		ret = -1;
		goto close_state;
	}
	ret = lua_pcall(L, 0, LUA_MULTRET, 0); //lua를 실행한다.
	if (ret > 0 )
	{
		TRACE((T_ERROR|T_TRIGGER, "error in lua script run (%s)\nscript = %s", lua_tostring(L, -1), script));
		ret = -1;
		goto close_state;
	}

	lua_getglobal(L, "PATH");
	char *key = (char *)luaL_checkstring(L, 1);
	if (vs_strcmp_lg(context->path, key) != 0 ) {
		/* path를 변경한 경우 */
		context->modified = 1;
		context->path = vs_strdup_lg(context->mpool, key);
	}
	/* path나 argument가 변경 되었으면 url을 새로 만든다. */
	if (context->modified == 1) {
		scx_lua_url_rebuild(&command->url, context);
	}

	ret = 0;

close_state :
	if (context) {
		if (context->argument) kv_destroy(context->argument);
		if (context->mpool) mp_free(context->mpool);

		SCX_FREE(context);
	}
	if (element == NULL)
	{
		//lua state pool을 사용하지 않는 경우에는 직접 close 한다.
		if(L != NULL) lua_close(L);
	}
	else
	{
		lua_pool_release_element(element);
	}
	TRC_END;
	return ret;
}


static int
scx_lua_origin_header_set(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_origin_io_command_t *cmd = (nc_origin_io_command_t *)lua_main->cls;

	char 	*key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다. lua에서 변수로  '-'를 허용하지 않기 때문에 '_' 대신 사용한다.
	const char  *val = NULL;
	//항상  nil인지 확인해야 한다.
	if (lua_isnil(L, 3)) {	//변수에 nil을 직접 할당 하거나 변수만 선언한 경우는 모두 nil이다
		//값이 할당 되지 않는 경우 해당 key를 hash에서 지운다.
		kv_remove(cmd->properties, key, 0);
		TRC_END;
		return 1;
	}
	else {
		val = luaL_checkstring(L, 3); //lua로 부터 value 값을 읽어 온다.
	}

	kv_replace(cmd->properties, key, val, 0);
	TRC_END;
	return 1;
}

static int
scx_lua_origin_header_get(lua_State *L)
{
	//request 정보를 가져 온다
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_origin_io_command_t *cmd = (nc_request_t *)lua_main->cls;

	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다.

	const char *val = NULL;

	val = kv_find_val_extened(cmd->properties, key, 0, 1);	/* get일 때는 대소 문자 구분을 하지 않는다. */
	if(val == NULL){
		TRACE((T_TRIGGER, "failed to find header tag '%s'\n", key));
		lua_pushnil(L);
		//lua_pushstring(L, "");
	}
	else {
		TRACE((T_TRIGGER, "find header tag '%s' is '%s'\n", key, val));
		lua_pushstring(L, val);
	}
	TRC_END;
	return 1;
}

static int
scx_lua_origin_argument_set(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	lua_origin_context_t *context = (lua_origin_context_t *)lua_main->context;

	char 	*key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
//	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다. lua에서 변수로  '-'를 허용하지 않기 때문에 '_' 대신 사용한다.
	const char  *val = NULL;
	context->modified = 1; 	/* argument의 변경이 있을때 설정 */
	//항상  nil인지 확인해야 한다.
	if (lua_isnil(L, 3)) {	//변수에 nil을 직접 할당 하거나 변수만 선언한 경우는 모두 nil이다
		//값이 할당 되지 않는 경우 해당 key를 hash에서 지운다.
		kv_remove(context->argument, key, 0);
		TRC_END;
		return 1;
	}
	else {
		val = luaL_checkstring(L, 3); //lua로 부터 value 값을 읽어 온다.
	}
	kv_replace(context->argument, key, val, 0);
	TRC_END;
	return 1;
}

static int
scx_lua_origin_argument_get(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	lua_origin_context_t *context = (lua_origin_context_t *)lua_main->context;

	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다. const char * 형이어야 하지만 scx_underbar_to_hyphen를 사용하기 위해 const를 사용하지 않는다.
//	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다.

	const char *val = NULL;

	val = kv_find_val_extened(context->argument, key, 0, 1);
	if(val == NULL){
		TRACE((T_TRIGGER, "failed to find header tag '%s'\n", key));
		lua_pushnil(L);
		//lua_pushstring(L, "");
	}
	else {
		TRACE((T_TRIGGER, "find header tag '%s' is '%s'\n", key, val));
		lua_pushstring(L, val);
	}
	TRC_END;
	return 1;
}


static int
scx_lua_not_implement(lua_State *L)
{

	return 1;
}

/*
 * LUA_PHASE_CLIENT_RESPONSE 에서  REQUEST 변수 조회시 호출
 */
static int
scx_lua_c_resp_request_get(lua_State *L)
{
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;
	const char *val = NULL;
	int num = 0;
	const char *key = luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.

	if (strcasecmp(key, "zmethod") == 0) {
		val = vs_data(req->zmethod);
		TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	else if (strcasecmp(key, "version") == 0) {
		val = vs_data(req->version);
		TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	else if (strcasecmp(key, "connect_type") == 0) {
		num = req->connect_type;
		TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%d'\n", req->id, key, num));
		lua_pushinteger(L, num);
	}
	else if (strcasecmp(key, "path") == 0) {
		val = vs_data(req->path);
		TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	else if (strcasecmp(key, "url") == 0) {
		val = vs_data(req->url);
		TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	else if (strcasecmp(key, "ori_url") == 0) {
		val = vs_data(req->ori_url);
		TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	else if (strcasecmp(key, "client_ip") == 0) {
		val = vs_data(req->client_ip);
		TRACE((T_TRIGGER, "[%llu] find request tag '%s' is '%s'\n", req->id, key, val));
		lua_pushstring(L, val);
	}
	return 1;
}


static int
scx_lua_c_resp_response_get(lua_State *L)
{
	TRC_BEGIN((__func__));
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		TRC_END;
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;
	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.

	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다.
	const char *val = NULL;

	if(strcasecmp(key, "code") == 0) {
		lua_pushinteger (L, req->scx_res.code);
	}
	else if(strcasecmp(key, "body") == 0) {
#if 0
		if(req->scx_res.body == NULL)
		{
			lua_pushnil(L);
		}
		else {
			lua_pushstring(L, req->scx_res.body);
		}
#endif
	}
	else {
		val = MHD_get_response_header(req->response, key);
		if(val == NULL){
			TRACE((T_TRIGGER, "[%llu] failed to find header tag '%s'\n", req->id, key));
			lua_pushnil(L);
			//lua_pushstring(L, "");
		}
		else {
			TRACE((T_TRIGGER, "[%llu] find header tag '%s' is '%s'\n", req->id, key, val));
			lua_pushstring(L, val);
		}
	}


	TRC_END;
	return 1;
}

/*
 * RESPONSE의 응답 코드 및 헤더를 삭제/변경/추가
 * body 수정 기능은 제외
 * 예)
 ** 특정 헤더 삭제 : RESPONSE.ETag = nil
 ** 응답코드 변경 : RESPONSE.code=301
 */
static int
scx_lua_c_resp_response_set(lua_State *L)
{
	lua_main_st_t * lua_main = (lua_main_st_t *)scx_lua_get_req_p(L);
	if (lua_main == NULL) {
		TRACE((T_ERROR|T_TRIGGER, "failed to get request data\n"));
		return 0;
	}
	nc_request_t *req = (nc_request_t *)lua_main->cls;
	//TRACE((T_TRIGGER, "%s, gettop = %d\n", __FUNCTION__,lua_gettop(L)));
	char *key = (char *)luaL_checkstring(L, 2); //lua로 부터 key 값을 읽어 온다.
	const char *val = NULL;
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다. lua에서 변수로  '-'를 허용하지 않기 때문에 '_' 대신 사용한다.
	if (lua_isnil(L, 3)) {	// nil을 할당한 경우는 response에서 삭제 해야 할까?
		TRACE((T_TRIGGER, "[%llu] %s, %s is NULL\n", req->id, __FUNCTION__, key));
	}
	else {
		val = luaL_checkstring(L, 3); //lua로 부터 value 값을 읽어 온다.
	}
	scx_underbar_to_hyphen(key); // '_'를 찾아서 '-'로 바꾼다. lua에서 변수로  '-'를 허용하지 않기 때문에 '_' 대신 사용한다.
	if(val == NULL) {
		/*
		 * RESPONSE.ETag = nil 와 같이 nil로 된 경우 이쪽으로 들어 온다
		 * 특정 헤더 삭제
		 */
		scx_update_resp_header(key, "", req);
		return 0;

	}

	if(strcasecmp(key, "code") == 0) {
		req->scx_res.code = atoi(val);
	}
	else if(strcasecmp(key, "body") == 0) {
#if 0
		if (req->scx_res.body) {	/* body가 이전에 기록되어 있는 경우 해당 메모리의 해제가 먼저 이루어 진다. */
			SCX_FREE(req->scx_res.body);
			req->scx_res.body = NULL;
			req->objstat.st_size = req->scx_res.body_len = 0;
		}
		req->objstat.st_size = req->scx_res.body_len = strlen(val);
		req->scx_res.body  = SCX_CALLOC(1, req->scx_res.body_len + 1);
		strncpy(req->scx_res.body, val, req->scx_res.body_len);
		TRACE((T_TRIGGER, "[%llu] %s, scx_res.body = %s\n", req->id, __FUNCTION__, req->scx_res.body));
#endif
	}
	else {
		scx_update_resp_header(key, val, req);
	}

	return 0;

}

int scx_lua_client_response_phase(void *cls, const char *script)
{
	lua_main_st_t  main_st = {cls, LUA_PHASE_CLIENT_RESPONSE, NULL} ;
	nc_request_t *req = (nc_request_t *)cls;
	lua_main_st_t * lua_main = &main_st;
	lua_State *L = NULL;
	lua_elment_t * element = NULL;
	int ret = 0;
	const struct luaL_Reg 	__c_resp_header_symbols[] = {
		{"__index", scx_lua_header_get},
		{"__newindex", scx_lua_not_implement},
		{NULL, NULL}
	};
	const struct luaL_Reg 	__c_resp_argument_symbols[] = {
			{"__index", scx_lua_conn_argument_get},
			{"__newindex", scx_lua_not_implement},
			{NULL, NULL}
	};
	const struct luaL_Reg 	__c_resp_request_symbols[] = {
		{"__index", scx_lua_c_resp_request_get},
		{"__newindex", scx_lua_not_implement},
		{NULL, NULL}
	};
	const struct luaL_Reg 	__c_resp_response_symbols[] = {
		{"__index", scx_lua_c_resp_response_get},
		{"__newindex", scx_lua_c_resp_response_set},
		{NULL, NULL}
	};
	const struct luaL_Reg __c_resp_solproxylib[] = {
	  {"nextaction", scx_lua_util_nextaction},
	  {NULL, NULL} /* end of array */
	};
	req->scx_res.code = req->resultcode;


//	TRACE((T_DEBUG, "[%llu] lua script = %s\n", req->id, script));


	if(gscx__lua_pool == NULL)
	{
		L = luaL_newstate();
		// load the libs
		luaL_openlibs(L);
	}
	else
	{
		element = lua_pool_get_element(gscx__lua_pool);
		if(element == NULL)
		{
			TRACE((T_ERROR|T_TRIGGER, "[%llu] failed to get free lua state\n", req->id));
			//printf("error : %s\n",lua_tostring(L, -1));
			ret = -1;
			goto response_close_state;
		}
		L = (lua_State * )element->data;
	}
//	lua_createtable(L, 0 /* narr */, 20 /* nrec */);
	lua_settop(L,0);
	scx_lua_set_req_p(L, lua_main); //lua_main 포인터를 global 변수 할당한다.



	//여기까지 순서를 바꾸면 안됨


	lua_settop(L,0);
	luaL_newmetatable(L, "HEADER");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "HEADER", __c_resp_header_symbols);
#else
	luaL_setfuncs(L, __c_resp_header_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "HEADER");

	/* URI 에 포함된 GET 파라미터 */
	lua_settop(L,0);
	luaL_newmetatable(L, "ARGUMENT");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "ARGUMENT", __c_resp_argument_symbols);
#else
	luaL_setfuncs(L, __c_resp_argument_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "ARGUMENT");

	lua_settop(L,0);
	luaL_newmetatable(L, "REQUEST");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "REQUEST", __c_resp_request_symbols);
#else
	luaL_setfuncs(L, __c_resp_request_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "REQUEST");

	lua_settop(L,0);
	luaL_newmetatable(L, "RESPONSE");
#if LUA_VERSION_NUM < 502
	luaL_register(L, "RESPONSE", __c_resp_response_symbols);
#else
	luaL_setfuncs(L, __c_resp_response_symbols, 0);
#endif
	lua_setmetatable(L, -2);
	lua_setglobal(L, "RESPONSE");

	lua_settop(L,0);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "UTIL", __c_resp_solproxylib);
#else
	luaL_setfuncs(L, __c_resp_solproxylib, 0);
#endif


	lua_settop(L,0);
#if LUA_VERSION_NUM < 502
	luaL_register(L, "_G", __printlib);
#else
	luaL_setfuncs(L, __printlib, 0);
#endif
	lua_settop(L,0);


//	ret = luaL_dostring(L, script); //luaL_loadstring()과 lua_pcall()를 합친 function
	ret = luaL_loadstring(L, script); //lua script를 메모리에 load하고  문법을 검사한다.
	if(ret > 0 ) //return이 0보다 큰 경우 script의 문법이 잘못됐거나 파일을 읽을수 없는 경우이다.
	{
		TRACE((T_ERROR|T_TRIGGER, "[%llu] error in lua script loading (%s)\n", req->id, lua_tostring(L, -1)));
		//printf("error : %s\n",lua_tostring(L, -1));
		ret = -1;
		goto response_close_state;
	}
	ret = lua_pcall(L, 0, LUA_MULTRET, 0); //lua를 실행한다.
	if(ret > 0 )
	{
		TRACE((T_ERROR|T_TRIGGER, "[%llu] error in lua script run (%s)\nscript = %s", req->id, lua_tostring(L, -1), script));
		ret = -1;
		goto response_close_state;
	}

	/*
	 * response code의 경우 이전단계에 이미 req->resultcode 값이 설정 되어 있기 때문에 req->scx_res.code 대신 req->resultcode 값을 사용한다.
	 */
	if(req->scx_res.trigger == LUA_TRIGGER_RETURN &&
			(req->scx_res.code < 100 || req->scx_res.code > 600 )) {
		//LUA_TRIGGER_RETURN이고  error code가 정상적으로 셋팅 되지 않은 경우는 응답코드를 원래의 코드로 원복한다.
		TRACE((T_TRIGGER, "[%llu] invalid http return code(%d)\n", req->id, req->scx_res.code));
		req->scx_res.code = req->resultcode;
	}
	else {
		req->resultcode = req->scx_res.code;
	}
	ret = 0;

response_close_state :
	if(element == NULL)
	{
		//lua state pool을 사용하지 않는 경우에는 직접 close 한다.
		if(L != NULL) lua_close(L);
	}
	else
	{
		lua_pool_release_element(element);
	}

	return ret;
}

