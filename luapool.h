#ifndef __LUA_POOL_H__
#define __LUA_POOL_H__


typedef enum
{
		LUA_STATUS_INVALID,
        LUA_STATE_IDLE,
        LUA_STATE_USING,
}LUA_STATE;


typedef struct lua_pool lua_pool_t;
struct lua_pool {
	size_t 					alloc_cnt; 		/* allocated pool size */
	size_t					start_loc;		/* free element 검색 시작 위치  */
	size_t					max_cnt;		/* 최대 할당 가능할 pool size */
	void 					**elements;
	struct lua_pool 		*next; 		/* 현재 pool의 추가 할당된 pool의 포인터*/
	struct lua_pool		    *current; 	/* 현재 할당이 진행되고 있는 pool */
} ;

typedef struct  lua_elment lua_elment_t;
struct lua_elment {
	int 		locate;
	LUA_STATE 	state;
	void 		*data;
};



lua_pool_t * lua_pool_create(size_t alloc_cnt);
int lua_expend_pool(lua_pool_t *pool, size_t expend_cnt);
int lua_pool_destroy(lua_pool_t *pool);
lua_elment_t * lua_pool_element_create(int locate);
int lua_pool_element_destroy(lua_elment_t *element);
lua_elment_t * lua_pool_get_element(lua_pool_t *pool);
int lua_pool_release_element(lua_elment_t *element);

#endif//end of __LUA_POOL_H__
