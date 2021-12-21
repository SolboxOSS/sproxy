#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <errno.h>
#include <microhttpd.h>
//#include <ncapi.h>

#include "common.h"
#include "scx_util.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
#include "site.h"
#include "scx_list.h"
#include "setup_vars.h"
#include "module.h"


//#define MODULE_TEST 1


/* 만들어진 모듈에 대한 선언을 여기에 해주어야 한다. */
extern module_driver_t gscx__a_module;
extern module_driver_t gscx__b_module;
extern module_driver_t gscx__soljwt_module;
extern module_driver_t gscx__soluri2_module;
extern module_driver_t gscx__session_module;
extern module_driver_t gscx__check__module;
extern module_driver_t gscx__originstat_module;
/* 모듈들은 아래에 정의된 순서에 따라 실행이 된다. */
scx_static_module_t gscx__static_modules[] = {
/*		{name,				is_base,	driver}	*/
#ifdef MODULE_TEST		/* 테스트용 모듈 삽입 */
		{"a_module",		1, &gscx__a_module},
		{"b_module",		1, &gscx__b_module},
#endif
		{"soljwt",			1, &gscx__soljwt_module},	/* soluri2와 session 모듈의 순서가 바뀌면 안된다. */
		{"soluri2",			1, &gscx__soluri2_module},	/* soluri2와 session 모듈의 순서가 바뀌면 안된다. */
//#ifdef ZIPPER
		{"session",			1, &gscx__session_module},	/* session 모듈의 경우 streaming 에서만 사용 가능하다 */
//#endif
#if 1	/* 처음 요청한 worker와 netcache core에서 접속한 worker가 동일한 경우 blocking되는 문제가 있어서 당분간 사용안한다. */
		{"check_module",	1, &gscx__check__module},
#endif
		{"originstat",	1, &gscx__originstat_module},
		{"", 				0,	NULL}
};


scx_module_t 	*gscx__module_list[MAX_MODULE_COUNT] = {NULL};
int				gscx__module_cnt = 0; 	/* 현재 load된 모듈의 크기 */

scx_list_t 		*gscx__phase_func_list[PHASE_COUNT]  = {NULL};	/* 모듈의 phase 함수들이 등록될 리스트 */



int scx_reg_static_module();
int scx_reg_dynamic_module(char *path);
int scx_reg_module(scx_module_t *module);
int scx_active_phase_func(char *module_name);
int scx_active_all_phase_func();
int check_phase_parameter(phase_type_t phase, phase_context_t *ctx);
void * module_malloc(size_t size);
void * module_calloc(size_t size, size_t multiply);
void * module_realloc(void *buf, size_t newsize);
void module_free(void *buf);
int get_module_handler_version();


module_callback_t 	gscx__module_callback = {	/* 동적 모듈에서 사용이 필요한 함수들을 여기에 콜백 함수로 등록한다. */
		.get_module_handler_version = get_module_handler_version,
		.reg_phase_func = scx_reg_phase_func,
		.malloc = module_malloc,
		.calloc = module_calloc,
		.realloc = module_realloc,
		.free = module_free,
		.mp_create = mp_create,
		.mp_alloc = mp_alloc,
		.mp_free = mp_free,
};

/*
 * scx_init_module()은 default 설정을 읽고나서 vm_create_config()가 호출 되기 이전에 실행 되어야 한다.
 * vm_create_config()호출 이후에 실행이 될 경우 volume create phase handler가 정상 동작 하지 못한다.
 */
int
scx_init_module()
{
	mem_pool_t 	*mpool = NULL;
	vstring_t 	*module = NULL;
	vstring_t 	*module_list = NULL;
	off_t 		off = 0;
	mpool = mp_create(4096);
	int ret = 0;
	int i = 0;

	for (i = 0; i < PHASE_COUNT; i++) {
		//gscx__phase_func_list[i] = scx_list_create(NULL, O_LIST_TYPE_DATA, 0);
		gscx__phase_func_list[i] = NULL;
	}


	if (scx_reg_static_module() == SCX_NO) {
		goto init_module_error;
	}

	module_list = scx_get_vstring(gscx__default_site, SV_MODULE_PATH, NULL);
	off = 0;
	if (NULL != module_list) {
		off = vs_pickup_token(mpool, module_list, off, ",", &module);
		while (module) {
			vs_trim_string(module);
			ret = scx_reg_dynamic_module(vs_data(module));
			if (SCX_NO == ret) {
				goto init_module_error;
			}
			off = vs_pickup_token(mpool, module_list, off, ",", &module);
		}
	}

	module_list = scx_get_vstring(gscx__default_site, SV_MODULE_ORDER, NULL);
	off = 0;
	if (NULL != module_list) {
		/* module_order에 지정된 순서로 각 phase handler function을 gscx__module_list에 등록 한다. */
		off = vs_pickup_token(mpool, module_list, off, ",", &module);
		while (module) {
			vs_trim_string(module);
			if (scx_active_phase_func(vs_data(module)) == SCX_NO) {
				goto init_module_error;
			}
			off = vs_pickup_token(mpool, module_list, off, ",", &module);
		}
	}
	else {	/* module_order가 별도로 지정되지 않은 경우 gscx__module_list에 들어 있는 순서대로 실행이 되고 이후
				module_path에 모듈들이 실행 된다. */
		if (scx_active_all_phase_func() == SCX_NO) {
			goto init_module_error;
		}
	}

	if (mpool) {
		mp_free(mpool);
		mpool = NULL;
	}
	return SCX_YES;
init_module_error:
	if (mpool) mp_free(mpool);
	TRACE((T_ERROR, "Module init failed.\n"));
	return SCX_NO;
}

int
scx_deinit_module()
{
	int i;
	for (i = 0; i < gscx__module_cnt; i++) {
		if (gscx__module_list[i]->driver->deinit == NULL) {
			TRACE((T_ERROR, "'%s' Module init function not found.\n", gscx__module_list[i]->name));
			return SCX_NO;
		}
		if (gscx__module_list[i]->driver->deinit() == SCX_NO) {	/* 모듈별 init 함수에 phase 별로 scx_reg_phase_func()를 호출 하는 부분이 있어야 한다. */
			TRACE((T_ERROR, "'%s' Module init failed.\n", gscx__module_list[i]->name));
			return SCX_NO;
		}
        if (gscx__module_list[i]->type == MODULE_TYPE_DYNAMIC) {
        	/* 동적 라이브러리 모듈인 경우는 먼저 열린 라이브러리를 닫는다. */
        	if (NULL != gscx__module_list[i]->d_handle) {
        		dlclose(gscx__module_list[i]->d_handle);
        		gscx__module_list[i]->d_handle = NULL;
        	}
        }
        SCX_FREE(gscx__module_list[i]);
    }
	for (i = 0; i < PHASE_COUNT; i++) {
		if (gscx__phase_func_list[i] != NULL) {
			scx_list_destroy(gscx__phase_func_list[i]);
			gscx__phase_func_list[i] = NULL;
		}

	}
	return SCX_YES;
}

int
scx_reg_static_module()
{
	int i;
	scx_module_t *module = NULL;
	for (i = 0; gscx__static_modules[i].driver != NULL; i++) {
		module = (scx_module_t *)SCX_CALLOC(1, sizeof(scx_module_t));
		module->type = MODULE_TYPE_STATIC;
		strncpy(module->name, gscx__static_modules[i].name, MAX_MODULE_NAME_LEN);
		module->driver = gscx__static_modules[i].driver;
		module->d_handle = NULL;
		module->is_base = gscx__static_modules[i].is_base;
		if (scx_reg_module(module) == SCX_NO) {
			return SCX_NO;
		}
    }
	return SCX_YES;
}

int
scx_reg_dynamic_module(char *path)
{
	int i;
	scx_module_t *module = NULL;
	module_driver_t *(*load)() = NULL;
	char *error;
	void 	*d_handle = NULL;
	d_handle = dlopen(path, RTLD_LAZY|RTLD_GLOBAL);
	if (d_handle == NULL) {
		TRACE((T_ERROR, "failed to load module(%s): std(%s), dlerror(%s)\n", path, strerror(errno), dlerror()));
		return SCX_NO;
	}
	load = dlsym(d_handle, "load");
	if ((error = dlerror()) != NULL) {
		TRACE((T_ERROR, "dlsym(%s, 'load') failed:%s\n", path, error));
		return SCX_NO;
	}

	module = (scx_module_t *)SCX_CALLOC(1, sizeof(scx_module_t));
	module->type = MODULE_TYPE_DYNAMIC;

	module->driver = (*load)();
	strncpy(module->name, module->driver->name, MAX_MODULE_NAME_LEN);
	module->d_handle = d_handle;
	module->is_base = 1; /* 동적 모듈을 loading 한다는것은 기본 모듈로 사용하겠다는것으로 판단한다. */
	if (scx_reg_module(module) == SCX_NO) {
		return SCX_NO;
	}
	return SCX_YES;
}

/*
 * 로딩된 모듈을 모듈리스트에 등록한다.
 * 모듈이 정상 동작을 하기 위해서는 이후에 active_phase_func 등록 과정을 거쳐야 한다.
 */
int
scx_reg_module(scx_module_t *module)
{
	if (gscx__module_cnt >= MAX_MODULE_COUNT) {
		TRACE((T_ERROR, "Module max count(%d) exceeded.\n", MAX_MODULE_COUNT));
		return SCX_NO;
	}
	module->driver->index = gscx__module_cnt;
	module->driver->callback = &gscx__module_callback;
	gscx__module_list[gscx__module_cnt] = module;
	TRACE((T_INFO, "%s module added(%d).\n", module->name, gscx__module_cnt));
	gscx__module_cnt++;
	return SCX_YES;
}

/* 지정된 모듈에 있는 phase function들을 phase handler에 등록한다. */
int
scx_active_phase_func(char *module_name)
{
	int i;
	scx_module_t *module = NULL;
	for (i = 0; i < gscx__module_cnt; i++) {
		if ( strncmp (gscx__module_list[i]->name, module_name, MAX_MODULE_NAME_LEN) == 0 ) {
			if (gscx__module_list[i]->driver->init == NULL) {
				TRACE((T_ERROR, "'%s' Module active function not found.\n", gscx__module_list[i]->name));
				return SCX_NO;
			}
			if (gscx__module_list[i]->driver->init() == SCX_NO) {	/* 모듈별 init 함수에 phase 별로 scx_reg_phase_func()를 호출 하는 부분이 있어야 한다. */
				TRACE((T_ERROR, "'%s' Module active failed.\n", gscx__module_list[i]->name));
				return SCX_NO;
			}
			TRACE((T_INFO,"'%s' Module active completed.\n", gscx__module_list[i]->name));
			return SCX_YES;
		}
    }
	TRACE((T_ERROR, "'%s' Module failed to find from module list.\n", module_name));
	return SCX_NO;
}

/* 기본 모듈(동적모듈 포함)로 등록된 모든 모듈의 phase function들을 phase handler에 등록한다. */
int
scx_active_all_phase_func()
{
	int i;
	scx_module_t *module = NULL;
	for (i = 0; i < gscx__module_cnt; i++) {
		if (0 == gscx__module_list[i]->is_base) {
			continue; /* 기본 모듈이 아닌 경우는 skip 한다. */
		}
		if (gscx__module_list[i]->driver->init == NULL) {
			TRACE((T_ERROR, "'%s' Module active function not found.\n", gscx__module_list[i]->name));
			return SCX_NO;
		}
		if (gscx__module_list[i]->driver->init() == SCX_NO) {	/* 모듈별 init 함수에 phase 별로 scx_reg_phase_func()를 호출 하는 부분이 있어야 한다. */
			TRACE((T_ERROR, "'%s' Module active failed.\n", gscx__module_list[i]->name));
			return SCX_NO;
		}
		TRACE((T_INFO,"'%s' Module active completed.\n", gscx__module_list[i]->name));

    }

	return SCX_YES;
}

/* 각 모듈의 phase 함수들을 phase handler list에 등록 하는 함수 */
int
scx_reg_phase_func(phase_type_t phase, module_func func)
{
	ASSERT(phase < PHASE_COUNT);
	if (NULL == gscx__phase_func_list[phase]) {
		gscx__phase_func_list[phase] = scx_list_create(NULL, O_LIST_TYPE_DATA, 0);
	}

	scx_list_append(gscx__phase_func_list[phase], NULL, (void *)func, 0);
	return SCX_YES;
}

/* phase 별로 지정된 phase handler function들을 실행 function*/

int
scx_phase_handler(phase_type_t phase, phase_context_t *ctx)
{
	int ret = 0;
	module_func func = NULL;
	ASSERT(phase < PHASE_COUNT);
	if (NULL == gscx__phase_func_list[phase]) {
		return SCX_YES;	/* 해당 phase에 등록된 모듈이 없을 경우 아래 과정이 필요 없음으로 바로 리턴한다. */
	}
	ASSERT(ctx);
	ASSERT(check_phase_parameter(phase, ctx) == SCX_YES);
	scx_list_iter_t *iter = scx_list_iter_new(gscx__phase_func_list[phase]);
	scx_list_iter_first(iter);
	for(;scx_list_iter_valid(iter) == SCX_YES;scx_list_iter_next(iter)) {
		func = (module_func)scx_list_iter_data(iter);
		ASSERT(func);
		ret = func(ctx);
		if (SCX_YES != ret) {
			scx_list_iter_free(iter);
			return SCX_NO;
		}
	}
	scx_list_iter_free(iter);
	return SCX_YES;
}

/*
 * phase handler별 필수 파라미터가 전달 되는지 확인 한다.
 */
int
check_phase_parameter(phase_type_t phase, phase_context_t *ctx)
{
	int ret = SCX_YES;
	switch(phase) {
	case PHASE_VOLUME_CREATE:
	case PHASE_VOLUME_RELOAD:
	case PHASE_VOLUME_DESTROY:
		if (NULL == ctx->service) {
			ret = SCX_NO;
		}
		break;
	case PHASE_ACCEPT_POLICY:
		if (NULL == ctx->addr) {
			ret = SCX_NO;
		}
		break;
	case PHASE_URI_PARSE:
	case PHASE_HEADER_PARSE:
	case PHASE_HOST_REWRITE:
	case PHASE_VOLUME_LOOKUP:
	case PHASE_CLIENT_REQUEST:
	case PHASE_CONTENT_VERIFY:
	case PHASE_BUILD_RESPONSE:
	case PHASE_CLIENT_RESPONSE:
		break;
		if (NULL == ctx->req) {
			ret = SCX_NO;
		}
	default :
		break;
	}
	if (SCX_NO == ret) {
		TRACE((T_ERROR, "NULL module parameter. phase(%d)\n", phase));
	}
	return ret;
}

void *
module_malloc(size_t size)
{
	return SCX_MALLOC(size);
}

void *
module_calloc(size_t size, size_t multiply)
{
    return SCX_CALLOC(size, multiply);
}

void *
module_realloc(void *buf, size_t newsize)
{
    return SCX_REALLOC(buf, newsize);
}

void
module_free(void *buf)
{
	SCX_FREE(buf);
}

int
get_module_handler_version()
{
	return __MODULE_HANDLER_VERSION;
}

#if 0	/* MHD 라이브러리를 so 형태로 링크 하면서 아래의 함수가 필요가 없어 졌음 */
/*
 * client request에 포함된 header이나 argument를 찾아서 리턴한다.
 * type에는 REQUEST_ARGUMENT_KIND와  REQUEST_HEADER_KIND 두가지가 사용가능하다.
 */
const char *
get_request_connection_value(phase_context_t *ctx, int type, char *key)
{
	const char *val = NULL;
	nc_request_t *req = ctx->req;
	int kind;
	ASSERT(req);
	ASSERT(req->connection);
	if(REQUEST_ARGUMENT_KIND == type) {
		kind = MHD_GET_ARGUMENT_KIND;
	}
	else {
		kind = MHD_HEADER_KIND;
	}

	val = MHD_lookup_connection_value(req->connection, kind, key);
	if(val == NULL){
		TRACE((T_DEBUG, "[%llu][%s] failed to find value '%s', type(%d)\n", req->id, __func__, key, type));
	}
	else {
		TRACE((T_DEBUG, "[%llu][%s] find value '%s' is '%s'\n", req->id,  __func__,key, val));
	}
	return val;
}
#endif
