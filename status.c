#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <memory.h>
//#include <ncapi.h>
//#include <trace.h>
#include <microhttpd.h>
#include "common.h"
#include "scx_util.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "scx_util.h"
#include "status.h"

service_core_t	gscx__server_stat = {0};	/* 전체 통계를 저장 */
extern struct MHD_Daemon *gscx__http_servers[3];
extern int		gscx__socket_count; /* MHD 데몬에 연결된 socket 수 */
extern int      gscx__session_count; /* 현재 연결 되어 있는 streaming session 수 */

int gscx__ncore = 1;
double gscx__uusage = 0;
double gscx__susage = 0;
double gscx__tusage = 0;


code_element_t gscx__code_el_def[] = {
		{"0", 	"NULL Page", 0},	/* 아래에 정의되지 않은 코드가 들어 왔을때 지정됨 */
		{"100", "Continue", 0},
		{"101", "Switching Protocols", 0},
		{"102", "Processing", 0},
		{"200", "OK", 0},
		{"201", "Created", 0},
		{"202", "Accepted", 0},
		{"203", "Non-Authoritative Information", 0},
		{"204", "No Content", 0},
		{"205", "Reset Content", 0},
		{"206", "Partial Content", 0},
		{"300", "Multiple Choices", 0},
		{"301", "Moved Permanently", 0},
		{"302", "Moved Temporarily", 0},
		{"303", "See Other", 0},
		{"304", "Not Modified", 0},
		{"305", "Use Proxy", 0},
		{"306", "unused", 0},
		{"307", "Temporary Redirect", 0},
		{"400", "Bad Request", 0},
		{"401", "Unauthorized", 0},
		{"402", "Payment Required", 0},
		{"403", "Forbidden", 0},
		{"404", "Not Found", 0},
		{"405", "Not Allowed", 0},
		{"406", "Not Acceptable", 0},
		{"407", "Proxy Authentication Required", 0},
		{"408", "Request Time-out", 0},
		{"409", "Conflict", 0},
		{"410", "Gone", 0},
		{"411", "Length Required", 0},
		{"412", "Precondition Failed", 0},
		{"413", "Request Entity Too Large", 0},
		{"414", "Request-URI Too Large", 0},
		{"415", "Unsupported Media Type", 0},
		{"416", "Requested Range Not Satisfiable", 0},
		{"500", "Internal Server Error", 0},
		{"501", "Not Implemented", 0},
		{"502", "Bad Gateway", 0},
		{"503", "Service Temporarily Unavailable", 0},
		{"504", "Gateway Time-out", 0},
		{"505", "HTTP Version Not Supported", 0},
		{"506", "Variant Also Negotiates" , 0},
		{"507", "Insufficient Storage", 0}
};

int		gscx__code_element_count = 0;

static struct hsearch_data 	__code_el_dict; /* 리턴 코드 메시지 저장용 */

void
scx_init_status()
{
	status_setup_element();
	memset(&gscx__server_stat, 0, sizeof(service_core_t) );
}

void
scx_deinit_status()
{
	status_destory_element();
}

void
status_setup_element()
{
	ENTRY 		ii;
	ENTRY 		*found;
	gscx__code_element_count = sizeof(gscx__code_el_def)/sizeof(code_element_t);
	hcreate_r(gscx__code_element_count, &__code_el_dict);
	for  (int i = 0; i < gscx__code_element_count; i++) {
		gscx__code_el_def[i].pos = i;	/* 배열에서의 위치를 재지정한다 */
		ii.key 	= gscx__code_el_def[i].code;
		ii.data = &gscx__code_el_def[i];
	    hsearch_r(ii, ENTER, &found, &__code_el_dict);
	}
}

/*
 * return code(int)로
 * service_core_t->counter_inbound_code[]나 service_core_t->counter_outbound_code[]의 위치를 반환한다.
 */
int
status_get_pos_by_int(int code)
{
	ENTRY 		query;
	ENTRY 		*found = NULL;
	int 		r;
	code_element_t	*page_found = NULL;
	char 		zcode[16];

	sprintf(zcode, "%d", code);
	query.key 	= zcode;
	query.data 	= NULL;
    r = hsearch_r(query, FIND, &found, &__code_el_dict);
	if (!found) {
		page_found = &gscx__code_el_def[0];
	}
	else {
		page_found = (code_element_t *)found->data;
	}
	return page_found->pos;
}

/*
 * return code(char*)로
 * service_core_t->counter_inbound_code[]나 service_core_t->counter_outbound_code[]의 위치를 반환한다.
 */
int
status_get_pos_by_char(const char *zcode)
{
	ENTRY 		query;
	ENTRY 		*found = NULL;
	int 		r;
	code_element_t	*page_found = NULL;

	query.key 	= (char *)zcode;
	query.data 	= NULL;
    r = hsearch_r(query, FIND, &found, &__code_el_dict);
	if (!found) {
		page_found = &gscx__code_el_def[0];
	}
	else {
		page_found = (code_element_t *)found->data;
	}
	return page_found->pos;
}

void
status_destory_element()
{
	hdestroy_r(&__code_el_dict);
}

/*
 * volume 내의 counter 업데이트
 */
void
scx_update_volume_service_start_stat(nc_request_t *req)
{
	if (req->service) {
		ATOMIC_ADD(req->service->core->counter_inbound_counter, 1);
		ATOMIC_ADD(req->service->core->concurrent_count, 1);
		ATOMIC_ADD(req->service->using_count, 1);
		ATOMIC_ADD(gscx__server_stat.concurrent_count, 1);
	}
	ATOMIC_ADD(gscx__server_stat.counter_inbound_counter, 1);
}

uint32_t
scx_get_volume_concurrent_count(service_info_t *service)
{
	ASSERT(service);
	return  ATOMIC_VAL(service->core->concurrent_count);
}

uint32_t
scx_get_volume_session_count(service_info_t *service)
{
	ASSERT(service);
	return  ATOMIC_VAL(service->core->session_count);
}

/*
 * volume 내의 counter 업데이트
 */
void
scx_update_volume_service_end_stat(nc_request_t *req)
{
	int	pos = 0;
	pos = status_get_pos_by_int(req->resultcode);
	if (req->service) {
		//TRACE((T_INFO, "SERVICE[%s]/SERVICE -  method %d, sent=%lld, rcvd=%lld\n", req->service->site->name, req->method, req->res_hdr_size, req->req_hdr_size));

		//ATOMIC_ADD(req->service->counter_inbound_response, (req->res_hdr_size + req->res_body_size));
//		ATOMIC_ADD(req->service->core->counter_inbound_counter, 1);
		ATOMIC_ADD(req->service->core->counter_inbound_response, req->res_hdr_size); /* response body size는 reader callback 호출될때마다 실시간으로 누적되고 있음 */
		ATOMIC_ADD(req->service->core->counter_inbound_request, (req->req_hdr_size+req->req_body_size+req->req_url_size));
		if (req->isopenapi == 1) {
			/* openapi 요청은 GET으로 들어 왔더라도 STAT으로 따로 counting 한다. */
			ATOMIC_ADD(req->service->core->counter_inbound_method[HR_STAT], 1);
		}
		else {
			ATOMIC_ADD(req->service->core->counter_inbound_method[req->method], 1);
		}
		ATOMIC_ADD(req->service->core->counter_inbound_code[pos], 1);
		ATOMIC_SUB(req->service->core->concurrent_count, 1);
		ATOMIC_SUB(req->service->using_count, 1);

		ATOMIC_SUB(gscx__server_stat.concurrent_count, 1);
	/*
	 * 통계 기록 off 요청에 대해서는 이후부터의 작업을 하지 않는다
	 * 통계 기록 off 요청에 대해 작업이 필요한 값들은 이전에 처리 해야 한다.
	 */
		if (req->disable_status) return ;
		ATOMIC_ADD(gscx__server_stat.counter_inbound_response, req->res_hdr_size); /* response body size는 reader callback 호출될때마다 실시간으로 누적되고 있음 */
		ATOMIC_ADD(gscx__server_stat.counter_inbound_request, (req->req_hdr_size+req->req_body_size+req->req_url_size));
		if (req->isopenapi == 1) {
			/* openapi 요청은 GET으로 들어 왔더라도 STAT으로 따로 counting 한다. */
			ATOMIC_ADD(gscx__server_stat.counter_inbound_method[HR_STAT], 1);
		}
		else {
			ATOMIC_ADD(gscx__server_stat.counter_inbound_method[req->method], 1);
		}

	}
	/* 아래의 두가지 항목은 에러 발생시에도 누적이 가능한 항목임 */
//	ATOMIC_ADD(gscx__server_stat.counter_inbound_counter, 1);
	ATOMIC_ADD(gscx__server_stat.counter_inbound_code[pos], 1);

	//scx_dump_service_stat(req->service);

//	printf("client ret = %d, pos = %d, count = %lld\n", req->resultcode, pos, ATOMIC_VAL(gscx__server_stat.counter_inbound_code[pos]));
}

void
scx_update_res_body_size(nc_request_t *req, size_t size)
{
	if (req->disable_status) return ;
	if(size <= 0) {
		return;
	}
	req->res_body_size += size;
	if (req->service) {
		/* service 별 전송량을 누적한다 */
		ATOMIC_ADD(req->service->core->counter_inbound_response, size);
	}
	ATOMIC_ADD(gscx__server_stat.counter_inbound_response, size);
}

uint64_t
scx_get_volume_send_size(service_info_t *service)
{
	ASSERT(service);
	return  ATOMIC_VAL(service->core->counter_inbound_response);
}

void
scx_update_volume_origin_stat(service_info_t *service, nc_method_t m, double sentb, double receivedb, const char *zcode)
{
	int	pos = 0;
	if (service) {
		pos = status_get_pos_by_char(zcode);
		TRACE((T_DAEMON, "SERVICE[%s]/ORIGIN -  method %d, sent=%.2f, rcvd=%.2f\n", vs_data(service->name), m, sentb, receivedb));
#ifdef DEBUG
		ASSERT(service->core->mnt);     //core의 memory가 해제된후 호출되는 경우 디버깅을 위해 추가
#endif
		ATOMIC_ADD(service->core->counter_outbound_response, (size_t)(receivedb));
		ATOMIC_ADD(service->core->counter_outbound_request, (size_t)(sentb));
		ATOMIC_ADD(service->core->counter_outbound_method[m], 1);
		ATOMIC_ADD(service->core->counter_outbound_counter, 1);
		ATOMIC_ADD(service->core->counter_outbound_code[pos], 1);
		//이후 부분 추가
		ATOMIC_ADD(gscx__server_stat.counter_outbound_response, (size_t)(receivedb));
		ATOMIC_ADD(gscx__server_stat.counter_outbound_request, (size_t)(sentb));
		ATOMIC_ADD(gscx__server_stat.counter_outbound_method[m], 1);
		ATOMIC_ADD(gscx__server_stat.counter_outbound_counter, 1);
		ATOMIC_ADD(gscx__server_stat.counter_outbound_code[pos], 1);
		//printf("origin ret = %s, pos = %d, count = %d\n", zcode, pos, ATOMIC_VAL(gscx__server_stat.counter_outbound_code[pos]));
		//scx_dump_service_stat(service);
	}
}

/* client의 요청과 별도로 내부의 필요 때문에 service를 여는 경우 호출
 * streaming의 경우나 preload에서 사용*/
void
internal_update_service_start_stat(service_info_t *service)
{

	ATOMIC_ADD(service->core->local_using_count, 1);
	ATOMIC_ADD(service->using_count, 1);

}

/* client의 요청과 별도로 내부의 필요 때문에 service를 닫는 경우 호출 */
void
internal_update_service_end_stat(service_info_t *service)
{
	ATOMIC_SUB(service->core->local_using_count, 1);
	ATOMIC_SUB(service->using_count, 1);
}

/* session이 생성되는 경우 호출 */
void
session_update_service_start_stat(service_info_t *service)
{
	internal_update_service_start_stat(service);
	ATOMIC_ADD(service->core->session_count, 1);
	ATOMIC_ADD(service->core->counter_session_total, 1);

}

/* session이 삭제되는 경우 호출 */
void
session_update_service_end_stat(service_info_t *service)
{
	internal_update_service_end_stat(service);
	ATOMIC_SUB(service->core->session_count, 1);
}



void
scx_write_status()
{
	static uint64_t 	before_inbound_counter = 0;
	static uint64_t 	before_inbound_request = 0;
	static uint64_t 	before_inbound_response = 0;
	static uint64_t 	before_outbound_counter = 0;
	static uint64_t 	before_outbound_request = 0;
	static uint64_t 	before_outbound_response = 0;
	static double		before_time = 0;

	uint64_t current_inbound_counter = 0;
	uint64_t current_inbound_request = 0;
	uint64_t current_inbound_response = 0;
	uint64_t current_outbound_counter = 0;
	uint64_t current_outbound_request = 0;
	uint64_t current_outbound_response = 0;
	double		current_time = 0;

	float	in_cps = 0.0;
	float	in_req_kbps = 0;
	float	in_resp_kbps = 0;
	float	out_cps = 0.0;
	float	out_req_kbps = 0;
	float	out_resp_kbps = 0;
	double	diff_time = 0.0;

	current_time = sx_get_time();
	current_inbound_counter = ATOMIC_VAL(gscx__server_stat.counter_inbound_counter);
	current_inbound_request = ATOMIC_VAL(gscx__server_stat.counter_inbound_request);
	current_inbound_response = ATOMIC_VAL(gscx__server_stat.counter_inbound_response);
	current_outbound_counter = ATOMIC_VAL(gscx__server_stat.counter_outbound_counter);
	current_outbound_request = ATOMIC_VAL(gscx__server_stat.counter_outbound_request);
	current_outbound_response = ATOMIC_VAL(gscx__server_stat.counter_outbound_response);

	/* 처음 한번 skip */
	if(before_inbound_counter != 0) {
		diff_time = (current_time - before_time) / 1000000.0;
		in_cps = (current_inbound_counter - before_inbound_counter) / diff_time;
		in_req_kbps = (current_inbound_request - before_inbound_request) * 8.0 / (diff_time * 1024.0);
		in_resp_kbps = (current_inbound_response - before_inbound_response) * 8.0 / (diff_time * 1024.0);
		out_cps = (current_outbound_counter - before_outbound_counter) / diff_time;
		out_req_kbps = (current_outbound_request - before_outbound_request) * 8.0 / (diff_time * 1024.0);
		out_resp_kbps = (current_outbound_response - before_outbound_response) * 8.0 / (diff_time * 1024.0);
		TRACE((T_STAT|T_INFO, "**** Client : tps = %.1f, req = %.2f kbps, resp = %.2f kbps; Origin : tps = %.1f, req = %.2f kbps, resp = %.2f kbps\n",
				in_cps, in_req_kbps, in_resp_kbps, out_cps, out_req_kbps, out_resp_kbps));
	}
	before_inbound_counter = current_inbound_counter;
	before_inbound_request = current_inbound_request;
	before_inbound_response = current_inbound_response;
	before_outbound_counter = current_outbound_counter;
	before_outbound_request = current_outbound_request;
	before_outbound_response = current_outbound_response;
	before_time = current_time;

	return ;
}

void
scx_write_concurrent_status()
{
	int i, socket_connections = 0;
	int session_count = ATOMIC_VAL(gscx__session_count);

#if 0
	union MHD_DaemonInfo *daemon_info = NULL;
	/* MHD_DAEMON_INFO_CURRENT_CONNECTIONS에 버그가 있어서 동시 접속이 많고 연결이 자주 끊어 지는 경우 SIGABRT가 발생한다. */
	for (int i = 0; i < 3; i++) {
		if (gscx__http_servers[i]) {
			daemon_info = MHD_get_daemon_info(gscx__http_servers[i], MHD_DAEMON_INFO_CURRENT_CONNECTIONS);
			socket_connections += daemon_info->num_connections;
		}
	}
#else
	socket_connections = ATOMIC_VAL(gscx__socket_count);
#endif
	TRACE((T_INFO|T_STAT, "**** Connection : concurrent = %d(%d), session = %d, client = %lld, origin = %lld\n",
					ATOMIC_VAL(gscx__server_stat.concurrent_count),
					socket_connections,	/* MHD daemon에 연결된 socket 수 */
					session_count, /* 연결 되어 있는 session 수 */
					ATOMIC_VAL(gscx__server_stat.counter_inbound_counter),
					ATOMIC_VAL(gscx__server_stat.counter_outbound_counter)));
	return ;
}

void
scx_dump_service_stat(service_info_t *service)
{
	TRACE((T_INFO|T_STAT, "Volume[%s] - SERVICE[%lld, %lld], ORIGIN[%lld, %lld]\n",
					vs_data(service->site->name),
					ATOMIC_VAL(service->core->counter_inbound_response), ATOMIC_VAL(service->core->counter_inbound_request),
					ATOMIC_VAL(service->core->counter_outbound_response), ATOMIC_VAL(service->core->counter_outbound_request)));
}



/*
 * 단위 시간동안의 평균 CPU 사용률을 출력.
 * CPU core 수를 알아 낼수 없을 때에는 CPU Core 수 * 사용률의 형태로 나온다. (예를 들어 CPU Core가 4개인 경우 400까지 나올수 있음)
 * 결과치는 atop과는 차이가 있고 top과 동일하게 나옴.
 */
void
scx_write_system_status()
{
	TRACE((T_INFO|T_STAT, "**** CPU(%d) : user = %.1f%, system = %.1f%, total = %.1f%\n",
			gscx__ncore, gscx__uusage, gscx__susage, gscx__tusage));
	return ;
}

void
scx_update_system_status()
{
	struct rusage usage;
	static double	before_time = 0;
	double			current_time = 0;
	double			diff_time = 0.0;
	static uint64_t utime_before = 0, stime_before = 0, time_total_before = 0;
	uint64_t utime_after, stime_after, time_total_after;


	current_time = sx_get_time();


	if (0 == getrusage(RUSAGE_SELF, &usage)) {
		utime_after = usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec;
		stime_after = usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
		time_total_after = utime_after + stime_after;

		if (0 != time_total_before) {
			diff_time = (current_time - before_time);
			gscx__uusage = (utime_after - utime_before) / diff_time * 100.0 / (double)gscx__ncore;
			gscx__susage = (stime_after - stime_before) / diff_time * 100.0 / (double)gscx__ncore;
			gscx__tusage = gscx__uusage + gscx__susage;
			TRACE((T_DEBUG, "**** CPU(%d) : user = %.1f%, system = %.1f%, total = %.1f%\n",
					gscx__ncore, gscx__uusage, gscx__susage, gscx__tusage));
		}
		else {
			/* 처음 실행시 */
#ifdef _SC_NPROCESSORS_ONLN
			/* cpu core의 수를 알아 낸다. */
			gscx__ncore = sysconf(_SC_NPROCESSORS_ONLN);
#else
			gscx__ncore = 1;
#endif
		}
		before_time = current_time;
		utime_before = utime_after;
		stime_before = stime_after;
		time_total_before = time_total_after;

	}
	return;
}

