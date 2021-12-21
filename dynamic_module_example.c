#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <module.h>
#include <stdint.h>
#include <microhttpd.h>

#include "common.h"
#include "module.h"


int dynamic_init();
int dynamic_deinit();
int dynamic_volume_create(phase_context_t *ctx);
int dynamic_volume_destroy(phase_context_t *ctx);
int dynamic_accept_policy(phase_context_t *ctx);
int dynamic_uri_parse(phase_context_t *ctx);
int dynamic_header_parse(phase_context_t *ctx);
int dynamic_host_rewrite(phase_context_t *ctx);
int dynamic_volume_lookup(phase_context_t *ctx);
int dynamic_client_request(phase_context_t *ctx);
int dynamic_content_verify(phase_context_t *ctx);
int dynamic_build_response(phase_context_t *ctx);
int dynamic_client_response(phase_context_t *ctx);
int dynamic_complete(phase_context_t *ctx);



module_driver_t gscx__dynamic_example_module = {
	.name 				= "demo_dmodule",	/* name */
	.desc				= "dynamic module demo",	/* description */
	.version			= 1,				/* version */
	.init				= dynamic_init,	/* init func */
	.deinit				= dynamic_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};

/* so 방식을 사용하는 경우.
 * main module handler쪽에서 (module_driver_t)dlsym(d_handle, "load") 형태로 호출한다. */
module_driver_t *
load()
{
	TRACE((T_INFO, "Demo dynamic module(%s %s) loaded.\n", __DATE__, __TIME__));
	return &gscx__dynamic_example_module;

}

int
dynamic_init()
{
	/* 모듈 초기화 단계에서 항상 모듈 callback function에 대한 유효성 검사를 해야한다. */
	MODULE_CONSISTENCY_CHECK(gscx__dynamic_example_module, 1);
	printf("call %s\n", __func__);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_VOLUME_CREATE, dynamic_volume_create);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_VOLUME_DESTROY, dynamic_volume_destroy);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_ACCEPT_POLICY, dynamic_accept_policy);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_URI_PARSE, dynamic_uri_parse);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_HEADER_PARSE, dynamic_header_parse);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_HOST_REWRITE, dynamic_host_rewrite);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_VOLUME_LOOKUP, dynamic_volume_lookup);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_CLIENT_REQUEST, dynamic_client_request);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_CONTENT_VERIFY, dynamic_content_verify);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_BUILD_RESPONSE, dynamic_client_response);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_CLIENT_RESPONSE, dynamic_client_response);
	gscx__dynamic_example_module.callback->reg_phase_func(PHASE_COMPLETE, dynamic_complete);

	char * mem_test = NULL;
	mem_test = gscx__dynamic_example_module.callback->malloc(30);
	if (NULL == mem_test) {
		printf("memory alloc failed.\n");
	}
	else {
		printf("memory alloc success.\n");
		sprintf(mem_test, "ok");
	}
	void * mem_pool = NULL;
	mem_pool = gscx__dynamic_example_module.callback->mp_create(10240);
	if (NULL == mem_pool) {
		printf("memory pool create failed.\n");
	}
	else {
		printf("memorypool create success.\n");
	}
	char * mem_pool_alloc = NULL;
	mem_pool_alloc = (char *)gscx__dynamic_example_module.callback->mp_alloc(mem_pool,100);
	if (NULL == mem_pool_alloc) {
		printf("mem_pool_alloc alloc failed.\n");
	}
	else {
		printf("mem_pool_alloc alloc success.\n");
		sprintf(mem_pool_alloc, "ok");
	}
	return SCX_YES;
}

int
dynamic_deinit()
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_volume_create(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	module_callback_t *cb = gscx__dynamic_example_module.callback;
	int	index = gscx__dynamic_example_module.index;
	char *service_ctx = cb->malloc(100);
	service->core->module_ctx[index] = (void *)service_ctx;	/* module_ctx에는 구조체 포인터도 사용 가능 */
	sprintf(service_ctx, "volume = %s", vs_data(service->name));


	printf("call %s\n", __func__);

	return SCX_YES;
}

int
dynamic_volume_destroy(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	module_callback_t *cb = gscx__dynamic_example_module.callback;
	int	index = gscx__dynamic_example_module.index;
	char *service_ctx = (char *)service->core->module_ctx[index];
	printf("call %s\n", __func__);
	printf("servicedata = '%s'\n", service_ctx);
	cb->free(service_ctx);
	service->core->module_ctx[index] = NULL;
	return SCX_YES;
}

int
dynamic_accept_policy(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_uri_parse(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_header_parse(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_host_rewrite(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_volume_lookup(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_client_request(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	module_callback_t *cb = gscx__dynamic_example_module.callback;
	printf("call %s\n", __func__);
	/* 원본에서 파일을 읽지 않고 동적으로 body를 생성 하는 예제
	 * client에서 http://127.0.0.1/editbody.txt 이런식으로 요청 할때 동작 */
	if (strncmp(vs_data(req->ori_url),"/editbody.txt", 13 ) == 0 ) {
		req->scx_res.body = cb->malloc(100);
		sprintf(req->scx_res.body, "new file !!\n*\n*\n*\n123");
		/* edited_body를 사용하는 경우는 아래의 값들을 필수로 셋팅 해주어야 한다. */
		req->objstat.st_size = strlen(req->scx_res.body);
		req->objstat.st_rangeable = 1;
		req->objstat.st_sizedecled = 1;
		req->objstat.st_private = 0;
		req->objstat.st_sizeknown = 1;
		req->objstat.st_chunked = 0;
	}

	return SCX_YES;
}

int
dynamic_content_verify(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	module_callback_t *cb = gscx__dynamic_example_module.callback;
	int	pos = 0, toread = 0, copied = 0;
	char * read_buf = NULL;
	int alloc_size = 0;
	int edited_body_size = 0;
	const char *header = MHD_lookup_connection_value(req->connection, MHD_HEADER_KIND, "User-Agent");
	if (NULL != header) {
		printf("User-Agent = %s\n", header);
	}
	/***************** 원본 파일 편집 기능 예제 시작 ***************/
	if (NULL == req->scx_res.body) {
		if (0 != req->p1_error )
			return SCX_YES;
		if (NULL == req->file)
			return SCX_YES;
		if (10240 < req->objstat.st_size)
			return SCX_YES;
		alloc_size = req->objstat.st_size + 100;

		read_buf = cb->malloc(req->objstat.st_size+1);
		printf("req->objstat.st_size = %d\n", req->objstat.st_size);
		pos = 0;
		toread = req->objstat.st_size;

		while(0 < toread) {
			copied = nc_read(req->file, read_buf+pos, (nc_off_t)pos, (nc_size_t)toread);
			if (0 >= copied) {
				break;
			}
			pos += copied;
			toread -= copied;
		}
		if (pos != req->objstat.st_size) {
			cb->free(read_buf);
			return SCX_YES;
		}
		if (req->scx_res.body) {
			/*
			 * req->scx_res.body 할당 전에 항상 검사를 해서 이미 다른 내용이 있으면 해제후 재할당 해야 한다.
			 * module 간의 문제 때문에 이부분이 지켜 지지 않으면 메모리릭이 발생하는 원인이된다.
			 */

			cb->free(req->scx_res.body);
			req->scx_res.body = NULL;
		}
		req->scx_res.body = cb->malloc(alloc_size);
		edited_body_size = sprintf(req->scx_res.body, "this file edited!!\n*\n*\n*\n%s", read_buf);
		/* edited_body를 사용하는 경우는 아래의 값들을 필수로 셋팅 해주어야 한다. */
		req->objstat.st_size = edited_body_size;
		req->objstat.st_rangeable = 1;
		req->objstat.st_sizedecled = 1;
		req->objstat.st_private = 0;
		req->objstat.st_sizeknown = 1;
		req->objstat.st_chunked = 0;
		printf("edited_body_size= %d\n", edited_body_size);
		cb->free(read_buf);
	}
	/***************** 원본 파일 편집 기능 예제 끝  ***************/
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_build_response(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_client_response(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
dynamic_complete(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	module_callback_t *cb = gscx__dynamic_example_module.callback;
	/***************** 원본 파일 편집 기능 예제 시작 ***************/

	/***************** 원본 파일 편집 기능 예제 끝  ***************/
	printf("call %s\n", __func__);
	return SCX_YES;
}




