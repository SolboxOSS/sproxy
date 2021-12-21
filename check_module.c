#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <microhttpd.h>

#include "common.h"
#include "module.h"
#include "httpd.h"
#include "streaming.h"
#include "scx_util.h"
#include "setup_vars.h"
#include "sample_mp3.h"

/*
 *                    hang 감지 모니터링 모듈
 *
 * 별도의 모니터링용 가상 볼륨(CHECK_VOLUME_NAME) 생성.
 * 로그/통계 : 모니터링 볼륨은 로그나 통계에 기록하지 않음.
 * TTL : TTL을 짧게 해야 원본 검사를 자주 한다.
 * origin : 모니터링 볼륨의 origin은 solproxy 자신이 된다. (origin 서버 주소가 127.0.0.1:solproxy_port가 됨)
 *          listen ip 바인딩을 하는 경우 loopback ip가 아니라 바인딩된 ip로 origin 주소를 설정한다.
 * content : 모드에 관계없이 특정파일에 대해 동일 하게 반응한다.
 * 원본 파일 정책 : 테스트용 원본 파일은 메모리에서 관리하고 다음 처럼 짦은 시간 캐싱을 허용(Cache-Control: max-age=60)할지
 *                아니면 아래처럼 요청시 마다 매번 확인 하도록 할건지 결정해야함
 *                Cache-Control: no-cache,no-store,must-revalidate
 *                Pragma: no-cache
 * 응답지연 발생 : nc_open이나 nc_read에서 일정시간 동안 응답이 없는 경우에 SP로그에 warning 메시지를 기록한다.
 * 요청 처리 : client로부터 들어온 모니터링 요청을 원본에 전달할때 "X-Contents-Check: 1" 헤더를 추가해서 전달하고
 *            client로부터 들어온 헤더에 X-Contents-Check가 있는지 검사 해서 있는 경우
 *            netcache core에서 loopback으로 들어 온거라고 판단하고 이에 따른 동작을 한다.
 * 볼륨 설정 :
 *
 * 예:
 * zipper일때
 * curl -v -o /dev/null 127.0.0.1:80/contents.check/_definst_/single/eng/0/check.mp3/content.mp3
 * solproxy일때
 * curl -v -o /dev/null 127.0.0.1:80/check.txt -H "Host: contents.check"
 *
 */


module_driver_t *a_load();
int check_module_init();
int check_module_deinit();
int check_module_volume_create(phase_context_t *ctx);
int check_module_volume_destroy(phase_context_t *ctx);
int check_module_volume_lookup(phase_context_t *ctx);


extern int	gscx__check_listen_port;

module_driver_t gscx__check__module = {
	.name 				= "check_module",	/* name */
	.desc				= "check module",
	.version			= 1,				/* version */
	.init				= check_module_init,	/* init func */
	.deinit				= check_module_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};

/* 현재의 모듈에서 사용할 전용 volume context */
typedef struct check_module_vol_ctx_tag {
	int	is_check_volume;		/* check 용 볼륨인 경우만 1로 셋팅 */
} check_module_vol_ctx_t;

char check_file_buf[] = {"The Solbox Web Acceleration offers you a cost-effective solution for quickly and reliably delivering content to audiences by applying multi-core based high performance caching — even during peak traffic and flash crowds — so you can provide the performance they want while reducing digital delivery costs."};

int
check_module_init()
{
	/* 설정파일에 check_module 활성화 설정이 있는지 확인 해서 없는 경우 phase function을 등록하지 않는다.
	 * 아직 구체적인 설정에 대한 계획은 없음 */
	scx_reg_phase_func(PHASE_VOLUME_CREATE, check_module_volume_create);
	scx_reg_phase_func(PHASE_VOLUME_DESTROY, check_module_volume_destroy);
	scx_reg_phase_func(PHASE_VOLUME_LOOKUP, check_module_volume_lookup);

	return SCX_YES;
}

int
check_module_deinit()
{
	return SCX_YES;
}

int
check_module_volume_create(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	/* 아래의 과정을 한다.
	 * 로그/통계 : 모니터링 볼륨은 로그나 통계에 기록하지 않음. (에러 로그만 빼고 모두 off)
	 * TTL : TTL을 짧게 해야 원본 검사를 자주 한다.
	 * origin : 모니터링 볼륨의 origin은 solproxy 자신이 된다. (origin 서버 주소가 127.0.0.1:solproxy_port가 됨)
	 *          listen ip 바인딩을 하는 경우 loopback ip가 아니라 바인딩된 ip로 origin 주소를 설정한다.
	*/
	int		index = gscx__check__module.index;	/* 현재 모듈이 로딩된 position */
	check_module_vol_ctx_t *vol_ctx  = NULL;
	nc_origin_info_t 	o;
	int 	positive_ttl = 5;
	int 	negative_ttl = 1;
	int 	ret = 0;

	/* 모니터링 전용 볼륨(CHECK_VOLUME_NAME)이 생성되는 경우만 아래의 과정을 진행한다. */
	if (vs_strcasecmp_lg(service->name, CHECK_VOLUME_NAME) == 0) {
		vol_ctx = (check_module_vol_ctx_t *) SCX_CALLOC(1, sizeof(check_module_vol_ctx_t));
		vol_ctx->is_check_volume = 1;
		service->core->module_ctx[index] = vol_ctx;	/* service context에 모듈 전용 context를 추가 한다 */
		/* check 관련 요청은 로그에 기록하지 않는다. */
		service->log_access = 0;
		service->log_error = 1;	/* 혹시 모를 에러 상황을 대비해서 에러 로그만 켠다 */
		service->log_origin = 0;
		service->stat_write_enable = 0; /* 통계 기록을 하지 않는다. */
		service->session_enable = 0;	/* 스트리밍시 session 기능의 오동작 발생 가능성 때문에 끄는걸 명시한다. */

		/* 원본 관련 설정을 변경 한다. 원본은 현재 실행중인 자신의 http port이다. */
		memset(&o, 0, sizeof(nc_origin_info_t));
		o.ioflag = NCOF_READABLE|NCOF_WRITABLE;
		strcpy(o.encoding, "utf-8");
		strcpy(o.prefix, CHECK_VOLUME_NAME);
#if 0
		sprintf(o.address, "http://%s:%d",
				gscx__config->listen_ip?vs_data(gscx__config->listen_ip):"127.0.0.1",	/* ip 바인딩 된 경우 바인딩 된 IP를 오리진 주소로 한다 */
				gscx__config->http_port);
#else
		sprintf(o.address, "http://127.0.0.1:%d", gscx__check_listen_port);
#endif
		ret = nc_ioctl(service->core->mnt, NC_IOCTL_UPDATE_ORIGIN, (void *)&o, 1/*오리진 정보 갯수*/);
		/* TTL을 캐싱이 되지 않을정도로 짧게 설정한다. */
		nc_ioctl(service->core->mnt, NC_IOCTL_POSITIVE_TTL, &positive_ttl, sizeof(positive_ttl));
		nc_ioctl(service->core->mnt, NC_IOCTL_NEGATIVE_TTL, &negative_ttl, sizeof(negative_ttl));

		TRACE((T_INFO, "Monitoring volume(%s) module configuration success.\n", CHECK_VOLUME_NAME));
	}
	return SCX_YES;
}

int
check_module_volume_destroy(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	int		index = gscx__check__module.index;
	/*
	 * 전용 context의 제거 필수
	 */
	if (NULL != service->core->module_ctx[index]) {
		SCX_FREE(service->core->module_ctx[index]);
		service->core->module_ctx[index] = NULL;
	}
	return SCX_YES;
}


/*
 * check 볼륨 요청일 때만 동작 한다.
 * check 볼륨으로 들어오는 요청은 아래의 두가지 경우가 있다.
 *           client로부터 들어온 요청 : "X-Contents-Check" 헤더 없음
 *           netcache core에서 들어온 요청 : "X-Contents-Check" 헤더 값이 있음
 */
int
check_module_volume_lookup(phase_context_t *ctx)
{
	nc_request_t *req = ctx->req;
	service_info_t *service = req->service;
	char *header = NULL;
	check_module_vol_ctx_t *vol_ctx  = NULL;
	char 	time_string[128] = "Tue, 14 Jan 2020 05:05:38 GMT";
	int		index = gscx__check__module.index;	/* 현재 모듈이 로딩된 position */
	if (service->core->module_ctx[index]) {	/* check 볼륨으로 요청이 들어 온 경우만 아래 루틴이 실행된다. */
		vol_ctx = service->core->module_ctx[index];
		if (vol_ctx->is_check_volume) {
			if (vs_strstr_lg(req->ori_url, CHECK_FILE_PATH) != NULL) {
				req->disable_status = 1; /* status에 기록되지 않도록 한다. */
				header = MHD_lookup_connection_value(req->connection, MHD_HEADER_KIND, CHECK_HEADER_NAME);
				if (NULL != header) {
					/* netcache core로 부터 요청이 들어 온 경우 */

					if (req->scx_res.body) {
						/*
						 * req->scx_res.body 할당 전에 항상 검사를 해서 이미 다른 내용이 있으면 해제후 재할당 해야 한다.
						 * module 간의 문제 때문에 이부분이 지켜 지지 않으면 메모리릭이 발생하는 원인이된다.
						 */
						SCX_FREE(req->scx_res.body);
						req->scx_res.body = NULL;
					}
#ifdef ZIPPER
					req->scx_res.body = SCX_MALLOC(sample_mp3_len+1);
					memcpy(req->scx_res.body, sample_mp3_buf, sample_mp3_len);
					/* edited_body를 사용하는 경우는 아래의 값들을 필수로 셋팅 해주어야 한다. */

					req->objstat.st_size = sample_mp3_len;
#else
					req->scx_res.body = SCX_MALLOC(strlen(check_file_buf)+1);
					strcpy(req->scx_res.body, check_file_buf);
					/* edited_body를 사용하는 경우는 아래의 값들을 필수로 셋팅 해주어야 한다. */
					req->objstat.st_size = strlen(check_file_buf);
#endif
					req->objstat.st_rangeable = 1;
					req->objstat.st_sizedecled = 1;
					req->objstat.st_private = 0;
					req->objstat.st_sizeknown = 1;
					req->objstat.st_chunked = 0;
#ifdef ZIPPER
						/* streaming 기능을 사용하는 경우에는 body를 편집 하면 req->streaming->media_size를 업데이트 해주어야 한다. */
					req->streaming->media_size = req->objstat.st_size;
					kv_add_val(req->scx_res.headers, MHD_HTTP_HEADER_CONTENT_TYPE, "video/MP2T");
#else
					kv_add_val(req->scx_res.headers, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
#endif
#if 0				/* 원본에서 nocache 관련 헤더를 리턴하는 경우 NetCache Core에서 inode를 재사용하지 못하고 새로 할당 받는 문제가 생김 */
					 /*
					  * netcache core에서 캐싱이 되지 않도록 아래의 no-cache 헤더를 리턴한다.
					  *  Cache-Control: no-cache,no-store,must-revalidate
					  *  Pragma: no-cache
					  */
					kv_add_val(req->scx_res.headers, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache,no-store,must-revalidate");
					kv_add_val(req->scx_res.headers, MHD_HTTP_HEADER_PRAGMA, "no-cache");
#endif
					kv_add_val(req->scx_res.headers, MHD_HTTP_HEADER_LAST_MODIFIED, time_string);
					TRACE((T_DEBUG, "[%llu] check origin request(%s).\n", req->id, vs_data(req->ori_url)));
					return SCX_YES;
				}	/* end of if (NULL != header) */
				else {
					/* "X-Contents-Check: 1" 요청 헤더를 추가해서 netcache core로 부터 들어 온 요청이라는 것을 명시한다. */
#ifdef ZIPPER
					/* 요청이 다시 zipper로 들어 가야 하므로 처음 들어온 url로 netcache에 요청한다. */
					//req->streaming->media_path = vs_data(req->ori_url);
					ASSERT(req->streaming);
					if (NULL == req->streaming->content) {
						/* 여기로 들어 오는 경우는 비정상 요청인 경우라고 봐야 한다. */
						scx_error_log(req, "Unexpected check request('%s').\n", vs_data(req->url));
						req->p1_error = MHD_HTTP_FORBIDDEN;
						return SCX_NO;	/* check 볼륨으로 다른 요청이 들어 온 경우는 무조건 에러를 리턴한다. */
					}
					/*
					 * 원본 파일을 경로를 여기서 다시 한번 바꾼다.
					 * content->path에 대해 메모리를 중복 할당 하게 되지만 memory pool에서의 할당이므로 pool 해제시 정상적으로 free가 된다.
					 */
					req->streaming->content->path = (char *) mp_alloc(req->streaming->mpool,strlen(CHECK_VOLUME_NAME)+strlen(CHECK_FILE_PATH)+50);
					sprintf(req->streaming->content->path , "/%s/_definst_/single/eng/0/%s", CHECK_VOLUME_NAME, CHECK_FILE_PATH);
					/* 스트리밍 기능 사용시 origin에 요청되는 헤더를 추가 하기 위해서는 req->options이 아닌 req->streaming->options에 추가해야 한다. */
					kv_add_val(req->streaming->options, CHECK_HEADER_NAME, "1");
#else
					kv_add_val(req->options, CHECK_HEADER_NAME, "1");
#endif
					TRACE((T_DEBUG, "[%llu] check client request(%s).\n", req->id, vs_data(req->ori_url)));
					return SCX_YES;
				}
			}	/* end of if (vs_strcmp_lg(req->ori_url, CHECK_FILE_PATH) == 0) */
			scx_error_log(req, "Unexpected check request('%s').\n", vs_data(req->url));
			req->p1_error = MHD_HTTP_FORBIDDEN;
			return SCX_NO;	/* check 볼륨으로 다른 요청이 들어 온 경우는 무조건 에러를 리턴한다. */
		}	/* end of if (vol_ctx->is_check_volume) */
	}	/* end of if (service->core->module_ctx[index]) */
	return SCX_YES;
}

