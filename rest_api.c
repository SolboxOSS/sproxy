#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
//#include <ncapi.h>
#include <microhttpd.h>
#include <jansson.h>
#include <dict.h>	/* libdict */

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
#include "status.h"
//#include "trace.h"
#include "scx_util.h"
#include "scx_list.h"
#include "standalonestat.h"

#define RSAPI_SIGNATURE 	"/__restapi/"


extern 	service_core_t	gscx__server_stat;
extern 	time_t			gscx__start_time;

extern 	int 	gscx__ncore;
extern 	double 	gscx__uusage;
extern 	double 	gscx__susage;
extern 	double 	gscx__tusage;
extern 	int 	gscx__vmsize;
extern 	int 	gscx__vmrss;

extern	code_element_t gscx__code_el_def[];
extern	int		gscx__code_element_count;

extern 	List	*gscx__standalonestat_service_list;

pthread_mutex_t 		__rest_api_lock = PTHREAD_MUTEX_INITIALIZER;
int	 gscx__rasapi_signature_len	= 0;

static void *rsapi_malloc(size_t size);
static void rsapi_free(void *buf);
static int rsapi_get_server_status(struct request_tag *req, json_t* j_ststus);
static int rsapi_get_all_service_status(struct request_tag *req, json_t* j_ststus);
static int rsapi_get_volume_status(struct request_tag *req, json_t* j_service, service_info_t *service);


void
scx_init_rsapi()
{
	gscx__rasapi_signature_len = strlen(RSAPI_SIGNATURE);
	json_set_alloc_funcs(rsapi_malloc, rsapi_free);
}

void
scx_deinit_rsapi()
{

}
static int rsapi_malloc_cnt = 0;	//memory leak 테스트용

static void *
rsapi_malloc(size_t size)
{
//	rsapi_malloc_cnt++;
	return SCX_MALLOC(size);
}

static void
rsapi_free(void *buf)
{
//	rsapi_malloc_cnt--;
	SCX_FREE(buf);
}


/*
 * 형식 예:__restapi/<version>/status/<service_domain>
 *
 * 	<version> :: = 1
 *	<service_domain> ::= (all | domain)
 * openapi 요청인 경우 1로 리턴, 일반요청이면 0을 리턴
 */
int
rsapi_is_local_object(nc_request_t *req)
{
	int ret = 0;
	/* management port로 들어온 경우에 url을 검사한다. */
	if (SCX_MANAGEMENT != req->connect_type) return 0;
	if( strncasecmp(RSAPI_SIGNATURE, vs_data(req->url), gscx__rasapi_signature_len) != 0) return 0;

	//TRACE((T_INFO, "strncasecmp(%s, %s) == %d\n", RSAPI_LOCAL_SIGNATURE, url, r));
	/*  OpenAPI는 GET Method 지원한다. */
	if (req->method != HR_GET) {
		scx_error_log(req, "open api only support GET method\n");
		ret = nce_handle_error(MHD_HTTP_METHOD_NOT_ACCEPTABLE, req);
		return 0;
	}
	req->isopenapi = 1;
	return 1;
}

int
rsapi_parse_objects(nc_request_t *req, vstring_t **version, vstring_t **command, vstring_t **domain)
{
	off_t 	off = gscx__rasapi_signature_len; /* offset of RSAPI_LOCAL_SIGNATURE */
	vstring_t 	*v_version = NULL;
	vstring_t 	*v_command = NULL;
	vstring_t 	*v_domain = NULL;

	off = vs_pickup_token(req->pool, req->url, off, "/", &v_version);
	off = vs_pickup_token(req->pool, req->url, off, "/", &v_command);
	off = vs_pickup_token(req->pool, req->url, off, "/", &v_domain);


	*version = v_version;
	*command 	= v_command;
	*domain 	= v_domain;

	return 1;
}


int
rsapi_do_handler(struct request_tag *req)
{
	vstring_t 	*version = NULL;
	vstring_t 	*command = NULL;
	vstring_t 	*domain = NULL;
	int 		ret = MHD_YES;
	size_t 		dump_flag = JSON_ENCODE_ANY|JSON_REAL_PRECISION(5);	// json 출력시 정수를 포함해서 최대 5자리까지만 표현한다.
	service_info_t *service = NULL;
    json_t* j_ststus = NULL;
    char* json_dump_buff = NULL;


	if (rsapi_parse_objects(req, &version, &command, &domain) == 0) {
		/* bad request */
		scx_error_log(req, "Not supported open api url format(%s).\n", vs_data(req->url));
		nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
		return MHD_YES;
	}

	if (version == NULL|| command == NULL || domain == NULL) {
		/* bad request */
		scx_error_log(req, "Not supported open api url format(%s).\n", vs_data(req->url));
		nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
		return MHD_YES;
	}
	if (vs_strcasecmp_lg(version, "1") != 0) {
		scx_error_log(req, "Not supported open api version(%s).\n", vs_data(req->url));
		nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
		return MHD_YES;
	}
	if (vs_strcasecmp_lg(command, "status") != 0) {
		/* 현재는 status 만 지원한다. */
		scx_error_log(req, "Not supported open api command(%s).\n", vs_data(req->url));
		nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
		return MHD_YES;
	}
	/* open api 요청은 동시에 한세션만 가능하도록 한다. */
	pthread_mutex_lock(&__rest_api_lock);

    j_ststus = json_object();
	if (vs_strcasecmp_lg(domain, "all") == 0) {
		/* 전체 서버 정보 요청시 */
	    if (rsapi_get_server_status(req, j_ststus) == 0) {
			scx_error_log(req, "Failed to build api server response(%s).\n", vs_data(req->url));
			nce_handle_error(MHD_HTTP_NOT_FOUND, req);
	    	goto rsapi_do_handler_done;
	    }

	    if (rsapi_get_all_service_status(req, j_ststus) == 0) {
			scx_error_log(req, "Failed to build api service response(%s).\n", vs_data(req->url));
			nce_handle_error(MHD_HTTP_NOT_FOUND, req);
	    	goto rsapi_do_handler_done;
	    }

	}
	else {
		/*
		 * volume 별 정보 요청시
		 * 이전 단계에서 reload에 대한 lock 걸려 있는 상태서 도중에 service 정보가 사라지는 경우가 없기 때문에
		 * scx_update_volume_service_start_stat()나 internal_update_service_start_stat()등을 호출 하지 않는다.
		 */
		service = vm_add(vs_data(domain), 1);
		if (service == NULL) {
			scx_error_log(req, "%s virtual host(%s) not found. url(%s)\n", __func__, vs_data(domain), vs_data(req->url));
			nce_handle_error(MHD_HTTP_NOT_FOUND, req);
	    	goto rsapi_do_handler_done;
		}
	    if (rsapi_get_volume_status(req, j_ststus, service) == 0) {
			scx_error_log(req, "Failed to build api service(%s) response. url(%s).\n", vs_data(domain), vs_data(req->url));
			nce_handle_error(MHD_HTTP_NOT_FOUND, req);
	    	goto rsapi_do_handler_done;
	    }
	}

    if (req->pretty) {
    	dump_flag |= JSON_INDENT(4);	/* Pretty-print the result */
    }
    /* buffer에 json data를 write 한다. */
    json_dump_buff = json_dumps(j_ststus, dump_flag);
    if (json_dump_buff == NULL) {
		scx_error_log(req, "json_dumps() Failed(%s). \n", vs_data(req->url));
		nce_handle_error(MHD_HTTP_NOT_FOUND, req);
    	goto rsapi_do_handler_done;
    }
//    printf("result = \n%s\n", json_dump_buff);



	req->scx_res.code = MHD_HTTP_OK;
	req->scx_res.body_len = strlen(json_dump_buff);
	req->scx_res.body = (void *)SCX_MALLOC(req->scx_res.body_len+1);
	strncpy(req->scx_res.body, json_dump_buff, req->scx_res.body_len);
	req->scx_res.body_len = strlen(json_dump_buff);
#if 0
	ret = scx_make_response(req);
#else
	req->resultcode = req->scx_res.code;
	nce_create_response_from_buffer(req, req->scx_res.body_len, req->scx_res.body);
	nce_add_basic_header(req, req->response);
	MHD_add_response_header(req->response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
	ret = scx_finish_response(req);
#endif

rsapi_do_handler_done:

	if (req->t_res_fbs == 0) {
		/*
		 * t_res_fbs가 0인 경우는 에러등으로 아직 response가 만들어지지 않은 상태라고 본다.
		 * 예외 처리가 되지 않은 부분이 있을때 여기에 들어 온다.
		 */
		scx_error_log(req, "Response not completed(%s).\n", vs_data(req->url));
		nce_handle_error(MHD_HTTP_BAD_REQUEST, req);
	}

	if (json_dump_buff != NULL) {
		rsapi_free(json_dump_buff);
		json_dump_buff = NULL;
	}
	if (j_ststus != NULL) {
		json_decref(j_ststus);
		j_ststus = NULL;
	}
	pthread_mutex_unlock(&__rest_api_lock);
//	printf("call malloc cnt = %d\n", rsapi_malloc_cnt);
	return ret;

}

/*
 *
 */
static int
rsapi_get_server_status(struct request_tag *req, json_t* j_ststus)
{
	char str_key[64];
	char str_value[256];
	time_t cur_time;
	service_core_t		*core = NULL;
    json_t* j_cpu = NULL;
    json_t* j_memory = NULL;
    json_t* j_response = NULL;
    json_t* j_origin_response = NULL;
    json_t* j_method = NULL;
    json_t* j_value = NULL;
	double uusage = gscx__uusage;
	double susage = gscx__susage;
	double tusage = uusage+susage;	//출력 도중 값이 변경 될수 있어서 여기서 직접 계산한다.
    int i;

	json_object_set_new(j_ststus, "HostName", json_string(vs_data(gscx__config->host_name)));
	snprintf(str_value, 256, "%s-%d.%d", PROG_VERSION, SVN_REVISION, CORE_SVN_REVISION);
	json_object_set_new(j_ststus, "Version", json_string(str_value));


	snprintf(str_value, 256, "%u", gscx__start_time);
	json_object_set_new(j_ststus, "StartTime", json_integer(gscx__start_time));
	cur_time = scx_update_cached_time_sec();
	snprintf(str_value, 256, "%u", cur_time);
	json_object_set_new(j_ststus, "CurrentTime", json_integer(cur_time));
	snprintf(str_value, 256, "%u", cur_time - gscx__start_time);
	json_object_set_new(j_ststus, "UpTime", json_integer(cur_time - gscx__start_time));


    j_cpu = json_object();	// hash를 위해서 object 생성
	json_object_set_new(j_cpu, "Cores", json_integer(gscx__ncore));
	json_object_set_new(j_cpu, "User", json_real(uusage));
	json_object_set_new(j_cpu, "System", json_real(susage));
	json_object_set_new(j_cpu, "Total", json_real(tusage));
    json_object_set_new(j_ststus, "Cpu", j_cpu);


    j_memory = json_object();
	json_object_set_new(j_memory, "VmSize", json_integer(gscx__vmsize * 1024L));
	json_object_set_new(j_memory, "VmRSS", json_integer(gscx__vmrss * 1024LL));
    json_object_set_new(j_ststus, "Memory", j_memory);

	core = &gscx__server_stat;
	json_object_set_new(j_ststus, "ServiceInBytes", json_integer(ATOMIC_VAL(core->counter_inbound_request)));
	json_object_set_new(j_ststus, "ServiceOutBytes", json_integer(ATOMIC_VAL(core->counter_inbound_response)));
	json_object_set_new(j_ststus, "OriginInBytes", json_integer(ATOMIC_VAL(core->counter_outbound_response)));
	json_object_set_new(j_ststus, "OriginOutBytes", json_integer(ATOMIC_VAL(core->counter_outbound_request)));
	json_object_set_new(j_ststus, "ServiceCounter", json_integer(ATOMIC_VAL(core->counter_inbound_counter)));
	json_object_set_new(j_ststus, "OriginCounter", json_integer(ATOMIC_VAL(core->counter_outbound_counter)));
	json_object_set_new(j_ststus, "Concurrent", json_integer(ATOMIC_VAL(core->concurrent_count)));

    j_method = json_object();
	json_object_set_new(j_method, "GET", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_GET])));
	json_object_set_new(j_method, "HEAD", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_HEAD])));
	json_object_set_new(j_method, "POST", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_POST])));
	json_object_set_new(j_method, "OPTIONS", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_OPTIONS])));
	json_object_set_new(j_method, "PURGE", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_PURGE])));
	json_object_set_new(j_method, "PRELOAD", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_PRELOAD])));
	json_object_set_new(j_method, "STATUS", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_STAT])));
    json_object_set_new(j_ststus, "Method", j_method);


	j_response = json_object();
	for (i = 0; i < gscx__code_element_count; i++) {
		json_object_set_new(j_response, gscx__code_el_def[i].code, json_integer(ATOMIC_VAL(core->counter_inbound_code[i])));
	}
    json_object_set_new(j_ststus, "ResponseCode", j_response);

    j_origin_response = json_object();
	for (i = 0; i < gscx__code_element_count; i++) {
		json_object_set_new(j_origin_response, gscx__code_el_def[i].code, json_integer(ATOMIC_VAL(core->counter_outbound_code[i])));
	}
    json_object_set_new(j_ststus, "OriginResponseCode", j_origin_response);

	return 1;
}

/*
 * 전체 서비스 목록을 가져와서 기록
 */
static int
rsapi_get_all_service_status(struct request_tag *req, json_t* j_ststus)
{

    int i;
	int lsize = 0;
	int	ret = 1;
	service_info_t *service = NULL;
	json_t* j_service = NULL;

	j_service = json_object();
	standalonestat_rlock();
	lsize = iList.Size(gscx__standalonestat_service_list);
	for (i = 0; i < lsize; i++) {
		service = *(service_info_t **)iList.GetElement(gscx__standalonestat_service_list,i);
	    if (rsapi_get_volume_status(req, j_service, service) == 0) {
			ret = 0;
	    	break;
	    }

	}
	standalonestat_unlock();

    json_object_set_new(j_ststus, "Service", j_service);
	return ret;
}

/*
 *
 */
static int
rsapi_get_volume_status(struct request_tag *req, json_t* j_service, service_info_t *service)
{
	char str_key[64];
	char str_value[256];
	service_core_t		*core = NULL;
    json_t* j_vol = NULL;
    json_t* j_domain = NULL;
    json_t* j_response = NULL;
    json_t* j_origin_response = NULL;
    json_t* j_method = NULL;
    int i;
	int lsize = 0;
	int	ret = 1;
	scx_list_t *root = NULL;
	int count = 0;
	char *domain = NULL;



	j_vol = json_object();

	/* Domains 처리 부분 */
	j_domain = json_array();
	root = service->domain_list;
	count = scx_list_get_size(root);
	for(i = 0; i < count; i++) {
		domain = scx_list_get_by_index_key(root,i);
		snprintf(str_value, 256, "%s", domain);
		json_array_append_new(j_domain, json_string(str_value));
	}
	json_object_set_new(j_vol, "Domains", j_domain);

	/* Service status 처리 부분 */
	core = service->core;
	json_object_set_new(j_vol, "ServiceInBytes", json_integer( ATOMIC_VAL(core->counter_inbound_request)));
	json_object_set_new(j_vol, "ServiceOutBytes", json_integer( ATOMIC_VAL(core->counter_inbound_response)));
	json_object_set_new(j_vol, "OriginInBytes", json_integer( ATOMIC_VAL(core->counter_outbound_response)));
	json_object_set_new(j_vol, "OriginOutBytes", json_integer( ATOMIC_VAL(core->counter_outbound_request)));
	json_object_set_new(j_vol, "ServiceCounter", json_integer( ATOMIC_VAL(core->counter_inbound_counter)));
	json_object_set_new(j_vol, "OriginCounter", json_integer( ATOMIC_VAL(core->counter_outbound_counter)));
	json_object_set_new(j_vol, "Concurrent", json_integer( ATOMIC_VAL(core->concurrent_count)));

    j_method = json_object();
	json_object_set_new(j_method, "GET", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_GET])));
	json_object_set_new(j_method, "HEAD", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_HEAD])));
	json_object_set_new(j_method, "POST", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_POST])));
	json_object_set_new(j_method, "OPTIONS", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_OPTIONS])));
	json_object_set_new(j_method, "PURGE", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_PURGE])));
	json_object_set_new(j_method, "PRELOAD", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_PRELOAD])));
	json_object_set_new(j_method, "STATUS", json_integer(ATOMIC_VAL(core->counter_inbound_method[HR_STAT])));
    json_object_set_new(j_vol, "Method", j_method);

	j_response = json_object();
	for (i = 0; i < gscx__code_element_count; i++) {
		json_object_set_new(j_response, gscx__code_el_def[i].code, json_integer( ATOMIC_VAL(core->counter_inbound_code[i])));
	}
    json_object_set_new(j_vol, "ResponseCode", j_response);

    j_origin_response = json_object();
	for (i = 0; i < gscx__code_element_count; i++) {
		json_object_set_new(j_origin_response, gscx__code_el_def[i].code, json_integer( ATOMIC_VAL(core->counter_outbound_code[i])));
	}
    json_object_set_new(j_vol, "OriginResponseCode", j_origin_response);

	json_object_set_new(j_service, vs_data(service->name), j_vol);
	return 1;
}


#if 0
// 기본 구조
{
	"HostName" : "GN-61-FHS3005",
	"Version" : "1.5.2-1341.1942",
	"StartTime" : 1577253600,
	"CurrentTime" : 1581573600,
	"UpTime" : 4320000,
	"ServiceInBytes" : 1278533789,
	"ServiceOutBytes" : 55845073627,
	"OriginInBytes" : 4258633789,
	"OriginOutBytes" : 19073627,
	"ServiceCounter" : 25613219,
	"OriginCounter" : 1323433,
	"Concurrent" : 10,
	"Cpu": {
		"Cores": "4",
		"User": "4.46",
		"System": "5.03",
		"Total": "9.49"
	},
	"Memory"  : {
		"VmSize" : 45765111808,
		"VmRSS" : 26712240128
	}
	"ResponseCode" : {
		"0" : 0,
		"100" : 0,
		"101" : 0,
		"102" : 0,
		"200" : 18032873,
		"201" : 0,
		"202" : 0,
		.
		.
		.
		"206" : 862417,
		"400" : 6770,
		"401" : 3450,
		.
		.
		.
		"507" : 0
	},
	"Service" : {
		"imgmedianet-sbsinmcache0.lgucdn.com" : {
			"Domains" : ["imgmedianet-sbsinmcache0.lgucdn.com", "imgmedianet.sbs.co.kr"],
			"ServiceInBytes" : 278533789,
			"ServiceOutBytes" : 845073627,
			"OriginInBytes" : 258633789,
			"OriginOutBytes" : 15073627,
			"ServiceCounter" : 2561119,
			"OriginCounter" : 323433,
			"Concurrent" : 3,
			"ResponseCode" : {
				"0" : 0,
				"100" : 0,
				"101" : 0,
				"102" : 0,
				"200" : 18032873,
				"201" : 0,
				"202" : 0,
				.
				.
				.
				"206" : 862417,
				"400" : 6770,
				"401" : 3450,
				.
				.
				.
				"507" : 0
			},
		},
		.
		.
		.
	}
}
#endif
