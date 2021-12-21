#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


#include "common.h"
#include "module.h"


module_driver_t *a_load();
int a_init();
int a_deinit();
int a_volume_create(phase_context_t *ctx);
int a_volume_destroy(phase_context_t *ctx);
int a_accept_policy(phase_context_t *ctx);
int a_uri_parse(phase_context_t *ctx);
int a_header_parse(phase_context_t *ctx);
int a_host_rewrite(phase_context_t *ctx);
int a_volume_lookup(phase_context_t *ctx);
int a_client_request(phase_context_t *ctx);
int a_content_verify(phase_context_t *ctx);
int a_build_response(phase_context_t *ctx);
int a_client_response(phase_context_t *ctx);
int a_complete(phase_context_t *ctx);

int b_init();
int b_deinit();
int b_volume_create(phase_context_t *ctx);
int b_volume_destroy(phase_context_t *ctx);
int b_accept_policy(phase_context_t *ctx);
int b_uri_parse(phase_context_t *ctx);
int b_header_parse(phase_context_t *ctx);
int b_host_rewrite(phase_context_t *ctx);
int b_volume_lookup(phase_context_t *ctx);
int b_client_request(phase_context_t *ctx);
int b_content_verify(phase_context_t *ctx);
int b_build_response(phase_context_t *ctx);
int b_client_response(phase_context_t *ctx);
int b_complete(phase_context_t *ctx);



module_driver_t gscx__a_module = {
	.name 				= "a_module",	/* name */
	.desc				= "test a module description",
	.version			= 1,				/* version */
	.init				= a_init,	/* init func */
	.deinit				= a_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};

module_driver_t gscx__b_module = {
	.name 				= "b_module",	/* name */
	.desc				= "test b module description",
	.version			= 1,				/* version */
	.init				= b_init,	/* init func */
	.deinit				= b_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};



int
a_init()
{
	printf("call %s\n", __func__);
	scx_reg_phase_func(PHASE_VOLUME_CREATE, a_volume_create);
	scx_reg_phase_func(PHASE_VOLUME_DESTROY, a_volume_destroy);
	scx_reg_phase_func(PHASE_ACCEPT_POLICY, a_accept_policy);
	scx_reg_phase_func(PHASE_URI_PARSE, a_uri_parse);
	scx_reg_phase_func(PHASE_HEADER_PARSE, a_header_parse);
	scx_reg_phase_func(PHASE_HOST_REWRITE, a_host_rewrite);
	scx_reg_phase_func(PHASE_VOLUME_LOOKUP, a_volume_lookup);
	scx_reg_phase_func(PHASE_CLIENT_REQUEST, a_client_request);
	scx_reg_phase_func(PHASE_CONTENT_VERIFY, a_content_verify);
	scx_reg_phase_func(PHASE_BUILD_RESPONSE, a_client_response);
	scx_reg_phase_func(PHASE_CLIENT_RESPONSE, a_client_response);
	scx_reg_phase_func(PHASE_COMPLETE, a_complete);


	return SCX_YES;
}

int
a_deinit()
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
a_volume_create(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	printf("call %s\n", __func__);
	printf("volume name = %s\n", vs_data(service->name));

	return SCX_YES;
}

int
a_volume_destroy(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	printf("call %s\n", __func__);
	printf("volume name = %s\n", vs_data(service->name));
	return SCX_YES;
}

int
a_accept_policy(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
a_uri_parse(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_header_parse(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_host_rewrite(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_volume_lookup(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_client_request(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_content_verify(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_build_response(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_client_response(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}

int
a_complete(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("url path = %s\n", vs_data(req->url));
	return SCX_YES;
}



int
b_init()
{
	printf("call %s\n", __func__);

	scx_reg_phase_func(PHASE_ACCEPT_POLICY, b_accept_policy);
	scx_reg_phase_func(PHASE_URI_PARSE, b_uri_parse);
	scx_reg_phase_func(PHASE_HEADER_PARSE, b_header_parse);
	scx_reg_phase_func(PHASE_HOST_REWRITE, b_host_rewrite);
	scx_reg_phase_func(PHASE_VOLUME_LOOKUP, b_volume_lookup);
	scx_reg_phase_func(PHASE_CLIENT_REQUEST, b_client_request);
	scx_reg_phase_func(PHASE_CONTENT_VERIFY, b_content_verify);
	scx_reg_phase_func(PHASE_BUILD_RESPONSE, b_client_response);
	scx_reg_phase_func(PHASE_CLIENT_RESPONSE, b_client_response);
	scx_reg_phase_func(PHASE_COMPLETE, b_complete);
	return SCX_YES;
}

int
b_deinit()
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_volume_create(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_volume_destroy(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_accept_policy(phase_context_t *ctx)
{
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_uri_parse(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	printf("path = %s\n", vs_data(req->url));

	return SCX_YES;
}

int
b_header_parse(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_host_rewrite(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_volume_lookup(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_client_request(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_content_verify(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_build_response(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_client_response(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

int
b_complete(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	printf("call %s\n", __func__);
	return SCX_YES;
}

