#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <pthread.h>
/* Include the Lua API header files. */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
//#include <ncapi.h>
//#include <trace.h>
#include "common.h"
#include "luapool.h"
#include "scx_util.h"

pthread_mutex_t 		__lua_pool_lock;
pthread_cond_t			__has_free_elements;

lua_pool_t *
lua_pool_create(size_t alloc_cnt)
{
	lua_pool_t *pool = NULL;
	int i;
	TRC_BEGIN((__func__));
	pool = SCX_CALLOC(1, sizeof(lua_pool_t));
	if (!pool) {
		TRC_END;
		return NULL;
	}
	pool->max_cnt = 200;
	if (alloc_cnt > pool->max_cnt) {
		pool->max_cnt = alloc_cnt;
	}
	pool->alloc_cnt = alloc_cnt;
	pool->current = pool; /* 동적 pool 확장을 위해 사용 */
	pool->next = NULL;
	pool->start_loc = 0;

	pool->elements = SCX_CALLOC(1, pool->max_cnt * sizeof(void *)); /* pool의 크기를 최대 200개까지 확장 가능하도록 한다. */
	if (!pool->elements) {
		SCX_FREE(pool);
		TRC_END;
		return NULL;
	}

	pthread_mutex_init(&__lua_pool_lock, NULL);
	pthread_cond_init(&__has_free_elements, NULL);

	for(i = 0; i < pool->alloc_cnt; i++) {
		lua_elment_t *element = NULL;

		element = lua_pool_element_create(i);
		if (!element) {
			TRACE((T_ERROR, "lua pool element failed to create.\n"));
			lua_pool_destroy(pool);
			TRC_END;
			return NULL;
		}
		pool->current->elements[i] = element;
	}
	TRACE((T_INFO, "lua pool create success. count = %d\n", pool->alloc_cnt));
	TRC_END;
	return pool;
}

int
lua_expend_pool(lua_pool_t *pool, size_t expend_cnt)
{
	int i = 0;
	int alloc_cnt = pool->alloc_cnt;
	for(i = alloc_cnt; i < (alloc_cnt + expend_cnt); i++) {
		lua_elment_t *element = NULL;

		element = lua_pool_element_create(i);
		if (!element) {
			TRACE((T_ERROR, "lua pool element failed to create.\n"));
			lua_pool_destroy(pool);
			return 0;
		}
		pool->current->elements[i] = element;
	}
	pool->alloc_cnt += expend_cnt;
	TRACE((T_INFO, "lua pool expened to %d.\n", pool->alloc_cnt));
	return 1;
}

int
lua_pool_destroy(lua_pool_t *pool)
{
	if(pool == NULL)
		return 0;
	int i;
	for(i = 0; i < pool->alloc_cnt; i++) {
		lua_pool_element_destroy(pool->current->elements[i]);
	}

	pthread_cond_destroy(&__has_free_elements);
	pthread_mutex_destroy(&__lua_pool_lock);
	SCX_FREE(pool->elements);
	SCX_FREE(pool);
	pool = NULL;
}


lua_elment_t *
lua_pool_element_create(int locate)
{
	lua_elment_t *element = NULL;
	int i;
	element = SCX_CALLOC(1, sizeof(lua_elment_t));
	if (!element) {
		return NULL;
	}
	element->locate = locate;
	element->state = LUA_STATE_IDLE;
	lua_State *L = luaL_newstate();
	if(L == NULL) {
		SCX_FREE(element);
		return NULL;
	}
	// load the libs
	luaL_openlibs(L);
	element->data = (void *) L;
	return element;
}



int
lua_pool_element_destroy(lua_elment_t *element)
{
	if(element == NULL)
		return 0;

	lua_close((lua_State * )element->data);

	SCX_FREE(element);
	element = NULL;
	return 1;
}

lua_elment_t *
lua_pool_get_element(lua_pool_t *pool)
{
	if(pool == NULL)
		return NULL;

	lua_elment_t *element = NULL;
	int i;
	TRC_BEGIN((__func__));

	pthread_mutex_lock(&__lua_pool_lock);
retry_find_element :
	for (i = pool->start_loc; i < pool->alloc_cnt; i++) {
		if(((lua_elment_t *)pool->current->elements[i])->state == LUA_STATE_IDLE)
		{
			element = (lua_elment_t *)pool->current->elements[i];
			break;
		}
	}
	if (element == NULL){
		for(i = 0; i < pool->start_loc; i++) {
			if(((lua_elment_t *)pool->current->elements[i])->state == LUA_STATE_IDLE)
			{
				element = (lua_elment_t *)pool->current->elements[i];
				break;
			}
		}
	}
	if (element == NULL) {	/* 사용 가능한 pool이 없는 경우. */
		if (pool->alloc_cnt < pool->alloc_cnt) {
			/* pool에 10개씩  element를 추가로 생성한다. */
			lua_expend_pool(pool, 10);
		}
		else {
			/* pool의 최대 한도를 넘은 경우 비어 있는 pool이 생길때 까지 대기 한다 */
			TRACE((T_WARN, "Failed to find available lua state pool.\n"));
			pthread_cond_wait(&__has_free_elements, &__lua_pool_lock);
			goto retry_find_element;
		}
	}
	else {
		element->state = LUA_STATE_USING;
		if (element->locate == (pool->alloc_cnt - 1)) {	/* 할당 받은 element가 마지막인 경우 */
			pool->start_loc = 0;
		}
		else {
			pool->start_loc = element->locate + 1;
		}
//		TRACE((T_INFO, "get element(%d).\n", element->locate));
	}
	pthread_mutex_unlock(&__lua_pool_lock);
	TRC_END;
	return element;
}

int
lua_pool_release_element(lua_elment_t *element)
{
	TRC_BEGIN((__func__));
	pthread_mutex_lock(&__lua_pool_lock);
	element->state = LUA_STATE_IDLE;
	pthread_cond_signal(&__has_free_elements);
	pthread_mutex_unlock(&__lua_pool_lock);
//	TRACE((T_INFO, "lease element(%d).\n", element->locate));
	TRC_END;
	return 1;
}
