/*
 * 기능 개요
 ** SolProxy 자체적으로 Content Router 로 동작할수 있는 기능을 개발한다.
 ** SolProxy 에서 Main manifest 응답시 적당한 서버를 선택해서 응답
 * 관련일감 : https://jarvis.solbox.com/redmine/issues/33182
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <sys/prctl.h>
#include <errno.h>
#include <microhttpd.h>
#include <curl/curl.h>
#include <jansson.h>

#include "common.h"
#include "scx_util.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "status.h"
#include "scx_timer.h"
#include "content_router.h"



extern 	double 	gscx__tusage;
extern 	service_core_t	gscx__server_stat;

#define CR_MAX_URL_SIZE				8192
#define CR_MAX_READ_BUFFER_SIZE		512000
#define CR_MAX_FHS_STAT_ARRAY_SIZE	512
#define CR_SIZE_OF_VIRTUAL_NODE_PER_FHS		20		/* fhs 당 가상 노드 생성 수 */
#define CR_MAX_VIRTUAL_NODE_SIZE	CR_MAX_FHS_STAT_ARRAY_SIZE * CR_SIZE_OF_VIRTUAL_NODE_PER_FHS
/*
 * redis proxy 서버로 부터 받아온 json body를 저장하는 구조체
 */
typedef struct {
	char 	buffer[CR_MAX_READ_BUFFER_SIZE];
	size_t	cursor;		// 현재까지 읽은 위치
} cr_body_info_t;

/*
 * consistent hash를 만들기 위한 가상 node의 정보를 저장하는 구조체
 * 가상 node 이름은 중요하지 않기 때문에 따로 기록하지 않는다.
 */
typedef struct {
	unsigned int	hash;	/* 가상 node의 이름을 사용해서 만든 hash */
	cr_fhs_stat_info_t *info;
} cr_node_hash_t;

int			gscx__cr_thread_working = 0;
int			gscx__cr_working = 0;
int			gscx__cr_inited = FALSE;
pthread_mutex_t gscx__cr_lock;
char		gscx__cr_server[5][64] = {'\0',};	/* redis proxy 서버의 주소와 port, 복수의 서버 설정시 세미콜론(;)으로 구분, ip:port, reload 안됨 */
int			gscx__cr_server_count = 0;
int			gscx__cr_server_cur = 0;			/* 현재 사용중인 redis proxy 서버 */
int			gscx__cr_enable = 0;				/* content router 기능 사용 여부, 기본값:0(사용안함), 1:사용, 0:사용안함, reload 가능 */
int			gscx__cr_update_enable = 1;		/* redis proxy 서버에 자신의 정보를 업데이트 할지 여부, 기본값:1(기능사용), 0:업데이트안함, 1:업데이트함, reload 가능 */
int			gscx__cr_update_duration = 2;		/* redis proxy 서버에 정보를 업데이트 하는 주기, 단위:초, 기본값:2(초), reload 가능 */
int			gscx__cr_threshold_traffic = 5120;	/* 지정된 트래픽을 넘기는 경우 해당 서버가 선정되더라도 content routing을 하지 않는다. 기본값:5120(Mbps), 단위:Mbps, reload 가능 */
int			gscx__cr_threshold_cpu = 50;		/* 지정된 cpu 사용률을 넘기는 경우 해당 서버가 선정되더라도 content routing을 하지 않는다. 기본값:50(%), 단위:퍼센트(100%가 최고임), reload 가능 */
int			gscx__cr_ttl = 3;					/* redis 에 서버 정보 기록시 설정되는 TTL, 단위:초, reload 가능 */
int			gscx__cr_alive_check_timeout = 1;	/* 자신의 hang 상태를 확인하기 위해 자신의 서비스 포트로 요청을 할때 사용되는 timeout 시간, 단위:초, default:1(초), reload 가능 */
int			gscx__cr_policy = CR_POLICY_ROUTE_NEXT_SERVER;		/* content routing 과정에서 traffic이나 CPU 사용률을 넘긴 서버가 선정되는 경우 정책, 기본값:0(다른 서버 선정),0:다른 서버 선정, 1:routing 안함 */
char		gscx__cr_group[64] = {'\0'}	;				/* redis proxy에 등록시 key 앞에 붙는  prefix, reload 가능 */
char		gscx__cr_server_domain[64] = {'\0'}	;		/* 서버에 접근 가능한 domain 이나 IP, reload 가능 */
cr_body_info_t gscx__cr_body_info;
cr_fhs_stat_info_t	gscx__cr_fhs_stat_array[2][CR_MAX_FHS_STAT_ARRAY_SIZE];		/* redis proxy로부터 받아온 서버 정보를 기록, 두개의 array를 번갈아서 사용 */
cr_node_hash_t		gscx__cr_node_hash_array[2][CR_MAX_VIRTUAL_NODE_SIZE];	/* cr_node_hash_t를 매번할당 하지 않기 위해  미리 array로 생성해 둔다  */
int			gscx__cr_fhs_stat_array_using_size[2] = {0,0};	/* gscx__cr_fhs_stat_list에서 실제 사용중인 fhs의 수 */
int			gscx__cr_fhs_stat_array_cur = 0; 				/* 현재 서비스에 사용중인 gscx__cr_fhs_stat_array의 번호, 0 or 1 */
static cr_node_hash_t *find_node_from_consistent_hash_ring(nc_request_t *req, const unsigned int path_hash, int service_array);


pthread_t 	gscx_cr_thread_tid = 0;

static void *cr_working_thread(void *d);
static int cr_update_server_status_to_redis();
static int cr_check_local_service();
static int cr_get_server_status_from_redis();
static uint32_t cr_make_hash_from_fhs_stat_array(int working_array);
static int cr_request_get_to_redis(char *path, cr_body_info_t *body_info);
static int cr_parse_server_list(char *body, char *path, int path_max);
static int cr_parse_servers_info_list(char *body, int	working_array);
static int cr_make_server_info(const char *info_str, cr_fhs_stat_info_t *fhs_info);
static int cr_make_virtual_node(cr_fhs_stat_info_t *fhs_info, int working_array, int pos, int node_per_fhs);
static int cr_check_available_node(cr_fhs_stat_info_t *fhs_info);
static int cr_make_consistent_hash_ring(int working_array);
static int cr_list_compare(const void *left, const void *right, CompareInfo *arg);
static int cr_compare_consistent_hash_node(const void *left, const void *right);
int
cr_start_service ()
{
	pthread_mutex_init(&gscx__cr_lock, NULL);
	gscx__cr_body_info.cursor = 0;
	gscx__cr_inited = TRUE;
	TRACE((T_INFO, "Content Router service inited.\n"));
	return 0;
}

int
cr_stop_service ()
{
	int i, lsize = 0;
	int	ret = 0;
	if (FALSE == gscx__cr_inited) return 0;
	gscx__cr_thread_working = 0;
	if (gscx_cr_thread_tid)
	{
		pthread_join(gscx_cr_thread_tid, NULL);
		gscx_cr_thread_tid = 0;
	}
	pthread_mutex_destroy(&gscx__cr_lock);

	TRACE((T_INFO, "Content Router service stopped.\n"));
	return 0;
}

/*
 * 설정 reload시 호출 되는 함수
 * 기존 설정과 reload된 설정에 변경사항이 있는지 확인 해서
 * 변경사항을 반영
 */
int
cr_update_config(site_t * site)
{
	vstring_t		*rv_chars = NULL;
	int				rv_int = 0;
	int 	ret;
	off_t 			toffset = 0;
	vstring_t 		*addr = NULL;
	int				server_cnt = 0;
	if(gscx__config->cr_enable == 1) {
		pthread_mutex_lock(&gscx__cr_lock);
		/* 설정 로딩중에는 혹시 모를 문제를 회피하기 위해 lock을 사용한다. */
		rv_chars = scx_get_vstring(site, SV_CR_SERVER, NULL);
		if (rv_chars != NULL) {
			toffset = vs_pickup_token(site->mp, rv_chars, toffset, ",", &addr);
			while (addr && server_cnt <= 5) {
				snprintf(gscx__cr_server[server_cnt++], 64, "%s", vs_data(addr));
//				printf("%s = %s, count = %d\n", SV_CR_SERVER,  gscx__cr_server[gscx__cr_server_count-1],gscx__cr_server_count-1);
				toffset = vs_pickup_token(site->mp, rv_chars, toffset, ",", &addr);
			}
			gscx__cr_server_count = server_cnt;
		}
		else {
			TRACE((T_WARN, "%s not configured.\n", SV_CR_SERVER));
			pthread_mutex_unlock(&gscx__cr_lock);
			goto failed_update_config;
			/* 여기서 에러 처리 방안 고민 해야됨 */
		}
		gscx__cr_server_cur = 0;	/* 설정 변경시 처음 서버 부터 다시 시도하도록 한다. */
		gscx__cr_update_enable = scx_get_uint(site, SV_CR_UPDATE_ENABLE, 1);
		gscx__cr_update_duration = scx_get_uint(site, SV_CR_UPDATE_DURATION, 2);
		gscx__cr_threshold_traffic = scx_get_uint(site, SV_CR_THRESHOLD_TRAFFIC, 5120);
		gscx__cr_threshold_cpu = scx_get_uint(site, SV_CR_THRESHOLD_CPU, 50);
		gscx__cr_ttl = scx_get_uint(site, SV_CR_TTL, gscx__cr_update_duration+1);
		gscx__cr_alive_check_timeout = scx_get_uint(site, SV_CR_ALIVE_CHECK_TIMEOUT, 1);
		gscx__cr_policy = scx_get_uint(site, SV_CR_POLICY, CR_POLICY_ROUTE_NEXT_SERVER);
		if (gscx__cr_policy != CR_POLICY_NO_ROUTE) gscx__cr_policy = CR_POLICY_ROUTE_NEXT_SERVER;
		rv_chars = scx_get_vstring(site, SV_CR_GROUP, vs_allocate(site->mp, 0, "cr", 0));
		snprintf(gscx__cr_group, 64, "%s", vs_data(rv_chars));
		rv_chars = scx_get_vstring(site, SV_CR_SERVER_DOMAIN, NULL);
		if (rv_chars != NULL) {
			snprintf(gscx__cr_server_domain, 64, "%s", vs_data(rv_chars));
		}

		pthread_mutex_unlock(&gscx__cr_lock);

		if (gscx__cr_thread_working == 1) {
			/* reload 이전에도 content router 기능을 사용하던 경우 */

		}
		else {
			/* content router 기능을 사용하지 않다가 사용하는 경우 */
			gscx__cr_thread_working = 1;
			pthread_create(&gscx_cr_thread_tid, NULL, cr_working_thread, (void *)NULL);
			TRACE((T_INFO, "Content Router service started.\n"));
		}

	}	// end of 'if(gscx__config->cr_enable == 1)'
	else if (gscx__cr_thread_working == 1) {
		/* content route기능을 사용하다가 사용하지 않는 경우에 여기로 들어 온다 */
		gscx__cr_thread_working = 0;
		if (gscx_cr_thread_tid)
		{
			pthread_join(gscx_cr_thread_tid, NULL);
			gscx_cr_thread_tid = 0;
		}
	}
	return 0;
failed_update_config:
	/* 문제가 생긴 경우 content router 기능이 동작 하지 않게 한다. */
	gscx__cr_thread_working = 0;
	if (gscx_cr_thread_tid)
	{
		pthread_join(gscx_cr_thread_tid, NULL);
		gscx_cr_thread_tid = 0;
	}
	return -1;
}

static void *
cr_working_thread(void *d)
{
	int ret;
	prctl(PR_SET_NAME, "content router working thread");
	TRACE((T_INFO, "Content Router working thread started.\n"));
	while (gscx__cr_thread_working  && gscx__signal_shutdown == 0) {
		bt_msleep(1000);	/* sleep 시간을 보정하는 부분도 들어가야함, 단 동기화 문제 때문에 최소 0.5초 이상은 sleep을 하도록 한다.*/
		if (gscx__cr_update_enable == 1) {
			if (cr_check_local_service() == 0 ) {
				/* local curl check에 통과 한 경우만 서버 상태를 업데이트 한다. */
				cr_update_server_status_to_redis();
			}
		}
		ret = cr_get_server_status_from_redis();
		if (ret < 0) {
			/* 서버 목록 업데이트에 실패한 경우에는 content router 동작을 하지 않도록 한다. */
			gscx__cr_working = 0;
		}
		else {
			gscx__cr_working = 1;
			gscx__cr_fhs_stat_array_cur = !gscx__cr_fhs_stat_array_cur;
		}

	}
	TRACE((T_INFO, "Content Router working thread stopped.\n"));

}




static size_t
cr_null_body_read_callback( void *source , size_t size , size_t nmemb , void *userData )
{

	const int buffersize = size * nmemb ;
	int	 *body_size = userData;
#if 0	/* 실제 body를 읽을 필요는 없고 크기만 정상인지 받는다. */
	char * buf = malloc(buffersize);
	*body_size = buffersize;
	memcpy(buf, (char *)source, buffersize);
	printf ("message = \n %s\n", buf);
#endif
	return buffersize;
}

/*
 * 자신의 서비스 포트로 test 요청을 해봐서 정상적으로 응답하는지 확인
 * 1초 이내에 200 응답을 해야지만 정상이고
 * 그 외에는 모두 비정상 처리 한다.
 */
static int
cr_check_local_service()
{
	char		url[128] = { 0, };
	CURL 		*c = NULL;
	int 		body_size = 0;
	long		ret_code = 0;
	double 		length;
	CURLcode 	errornum;
	char		host_header[40] = { 0, };
	struct curl_slist *header_list = NULL;
	int 		ret = 0;
	char 		errbuf[CURL_ERROR_SIZE];

#ifdef ZIPPER
	snprintf(url, 128, "http://127.0.0.1:%d/%s/_definst_/single/eng/0/%s/content.mp3", gscx__config->http_port, CHECK_VOLUME_NAME, CHECK_FILE_PATH);
#else
	snprintf(url, 128, "http://127.0.0.1:%d/%s", gscx__config->http_port,CHECK_FILE_PATH);
#endif

	c = curl_easy_init ();	/* thread 강제 종료에 의한 메모리릭 가능성 때문에 여기서 curl context를 할당서 thread에 넘겨 준다. */
	snprintf(host_header, 40, "Host: %s", CHECK_VOLUME_NAME);
	header_list = curl_slist_append(header_list, host_header);	// "Host: contents.check" 헤더 추가
    // 쓰레드 종료시 호출될 함수 등록

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, header_list);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cr_null_body_read_callback);	/* callback을 등록하지 않으면 consol로 output이 출력 된다. */
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &body_size);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1);	/* Fail on HTTP 4xx errors. */
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 1);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 1);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
	/*
	 * http 1.0으로 하는 경우는 서버와 클라이언트중 어느쪽이 먼저 끊을지 알수 없기 때문에 1.1로 설정해서 client(monitoring)가 먼저 끊도록 한다.
	 * https://jarvis.solbox.com/redmine/issues/31946 참조
	 */
	curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 1L);	/* socket 재사용 금지, curl_easy_perform()수행후 연결을 종료한다. */

	errornum = curl_easy_perform(c);
	if (CURLE_OK != errornum) {
	  /* error 처리 */
		TRACE((T_INFO, "check url error, '%s'(%d)\n", errbuf, errornum));
		ret = -1;
	}
	else {
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, (long *)&ret_code);
		curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD,  (double *)&length);
		if (200 != (int)ret_code) {
			/* return code가 200이 나와야함, 이외의 코드는 무조건 에러처리 */
			TRACE((T_INFO, "check url return code(%ld) error\n", ret_code));
			ret = -1;
		}

	}
	if (header_list) {
		curl_slist_free_all(header_list);
	}
	if (c) {
		curl_easy_cleanup (c);
	}
	return ret;
}


static int
cr_update_server_status_to_redis()
{
	char			path[1280] = { 0, };
	double 			length;
	cr_fhs_stat_info_t 	fhs_info;
	static uint64_t 	before_inbound_response = 0;
	uint64_t 		current_inbound_response = 0;
	uint64_t 		diff_inbound_response;
	char			value[1024] = { 0, };
	static double	before_time = 0;
	double			current_time = 0;
	double			diff_time = 0.0;
	int				ret = 0;

	current_time = sx_get_time();

	pthread_mutex_lock(&gscx__cr_lock);
	fhs_info.version = CR_VERSION;
	snprintf(fhs_info.domain, 40, "%s", gscx__cr_server_domain);
	current_inbound_response = gscx__server_stat.counter_inbound_response;

	if (before_inbound_response > 0LL) {
		diff_inbound_response = current_inbound_response - before_inbound_response;
		if (diff_inbound_response > 0) {
			diff_time = (current_time - before_time) / 1000000.0;
			fhs_info.current_traffic = (int)(diff_inbound_response/(1048576.0 * diff_time)) * 8;
		}
		else {
			/* 트래픽이 없는 경우 */
			fhs_info.current_traffic = 0;
		}
	}
	else {
		/* 처음 시작의 경우 */
		fhs_info.current_traffic = 0;
	}
	before_inbound_response = current_inbound_response;
	fhs_info.threshold_traffic = gscx__cr_threshold_traffic;
	fhs_info.cpu_usage = (int) gscx__tusage;
	fhs_info.threshold_cpu_usage = gscx__cr_threshold_cpu;
	fhs_info.update_time = scx_get_cached_time_sec();


	/*
	 * 저장 순서 : version, domain(IP), current traffic, threshold traffic, 현재 cpu 사용률, threshold cpu 사용률 , 현재 시간(unix timestamp)
	 * 필요에 따라서 url encode를 해야 될 수도 음. | 대신 %%7C로 사용 해야 할수 도 있음 */
	snprintf(value, 1024,"%d|%s|%d|%d|%d|%d|%ld",
			fhs_info.version, fhs_info.domain,
			fhs_info.current_traffic, fhs_info.threshold_traffic,
			fhs_info.cpu_usage, fhs_info.threshold_cpu_usage,
			fhs_info.update_time);

	before_time = current_time;
	snprintf(path, 1280, "SET/%s_%s/%s/ex/%d", gscx__cr_group, fhs_info.domain, value, gscx__cr_ttl);
	TRACE((T_DEBUG, "path = %s\n", path));
	pthread_mutex_unlock(&gscx__cr_lock);

	ret = cr_request_get_to_redis(path, &gscx__cr_body_info);

	return ret;
}



static int
cr_get_server_status_from_redis()
{
	int		ret = 0;
	int		MAX_PATH_LEN = CR_MAX_URL_SIZE-50;
	char	path[CR_MAX_URL_SIZE-50] = { 0, };
	static uint32_t prev_hash[2] = {0,0};
	uint32_t 		curr_hash = 0;
	int		working_array; 	/* 현재 업데이트 준비 중인 array의 번호, 0 or 1 */
	working_array = !gscx__cr_fhs_stat_array_cur;

	snprintf(path, MAX_PATH_LEN, "KEYS/%s_*", gscx__cr_group);

	/*
	 * redis proxy 서버에 그룹에 속해 있는 fhs의  목록을 요청
	 * 서버 요청 url 예
	 ** http://192.168.110.138:7379/KEYS/solvod_*
	 * 서버로 부터 return 되는 body는 아래의 형태임
	 ** {"KEYS":["solvod_k20245-063","solvod_server_1","solvod_server_2","solvod_server_3","solvod_server_4"]}
	 **
	 */
	ret = cr_request_get_to_redis(path, &gscx__cr_body_info);
	if(ret < 0) {
		return ret;
	}
	snprintf(path, MAX_PATH_LEN, "MGET");
	ret = cr_parse_server_list(gscx__cr_body_info.buffer, path+strlen(path), MAX_PATH_LEN - strlen(path) - 1);
	if(ret < 0) {
		return ret;
	}
	/*
	 * redis proxy 서버에 각 fhs들의 상태 정보들을 MGET을 사용해서 한번에 요청
	 * 서버 요청 url 예
	 ** http://192.168.110.138:7379/MGET/solvod_k20245-063/solvod_server_1/solvod_server_2/solvod_server_3/solvod_server_4
	 * 서버로 부터 return 되는 body는 아래의 형태임
	 ** {"MGET":["1|fhs-domain.solbox.com|0|5120|0|50|1600306850","1|server1.solbox.com|1903|5120|2|50|1600237883","1|server2.solbox.com|1533|5120|23|50|1600237853","1|server3.solbox.com|1347|5120|55|50|1600236852","1|server4.solbox.com|5347|5120|5|50|1600234856"]}
	 **
	 */
	ret = cr_request_get_to_redis(path, &gscx__cr_body_info);
	if(ret < 0) {
		return ret;
	}
	ret = cr_parse_servers_info_list(gscx__cr_body_info.buffer, working_array);
	if(ret < 0) {
		return ret;
	}
	curr_hash = cr_make_hash_from_fhs_stat_array(working_array);

	/*
	 * 이전에 받아온 서버 목록과 현재 받아온 서버 목록이 같은 경우 가상노드를 새로생성 하지 않고 기존의 가상 노드 정보를 그대로 사용한다.
	 * 서버 목록의 hash를 만들어서 이전 hash와 비교 하면 구분이 가능함
	 * gscx__cr_policy이 0(CR_POLICY_ROUTE_NEXT_SERVER)인 경우에는
	 * 서버의 목록이 같아도 각서버의 threshold를 초과하는 경우가 있기 때문에 매번 consistent hash ring을 만든다.
	 * 현재 두개의 lock 사용을 최소화 하기 위해 array를 번갈아 가며 사용 하도록 되어 있기 때문에 array 별로 hash를 따로 만들어야 한다.
	 */

	if (prev_hash[working_array] != curr_hash) {
		/* hash값이 변경된 경우(목록의 변화가 있는 경우) consistent hash를 다시 만든다. */
		ret = cr_make_consistent_hash_ring(working_array);
		if(ret < 0) {
			return ret;
		}
	}

	prev_hash[working_array] = curr_hash;
	return ret;
}


/*
 * callback 되는 nmemb의 최대 크기가 16384 byte임
 * 이보다 body가 큰 경우 나눠서 읽어야 한다.
 */
static size_t
cr_webdis_body_read_callback( void *source , size_t size , size_t nmemb , void *userData )
{

	int bodysize = size * nmemb ;
	cr_body_info_t *body_info = userData;

	if ((body_info->cursor + bodysize) > (CR_MAX_READ_BUFFER_SIZE-1)) {
		/*
		 * 저장할수 있는 최대 크기는 CR_MAX_READ_BUFFER_SIZE 보다 작아야 한다.
		 * 이 크기를 넘는 경우는 최대 크기까지만 읽도록 한다.
		 */
		bodysize = CR_MAX_READ_BUFFER_SIZE - body_info->cursor - 1;
	}
	TRACE((T_DEBUG,"read callback size = %lld, nmemb = %lld, body size = %d\n", size, nmemb, bodysize));
	memcpy(body_info->buffer+body_info->cursor, (char *)source, bodysize);
	body_info->cursor += bodysize;
	return  bodysize;
}

/*
 * gscx__cr_fhs_stat_array에 들어 있는 fhs의 name을 모두 붙여서 문자열을 만든후 이를 사용해서 hash를 생성한다.
 */
static uint32_t
cr_make_hash_from_fhs_stat_array(int working_array)
{
	uint32_t hash;
	int		i;
	int		fhs_stat_size = gscx__cr_fhs_stat_array_using_size[working_array];
	cr_fhs_stat_info_t *fhs_info = NULL;
	char	fhs_str[CR_MAX_URL_SIZE] = { 0, };
	int		str_pos = 0;	/* fhs_str의 마지막 문자열 위치 */
	int		domain_len = 0;

	/* FHS별 가상 노드 생성 */
	for (i = 0; i < fhs_stat_size; i++) {
		fhs_info = (cr_fhs_stat_info_t *)(&gscx__cr_fhs_stat_array[working_array][i]);
		domain_len = strlen(fhs_info->domain);
		memcpy(fhs_str + str_pos, fhs_info->domain, domain_len);
		str_pos += domain_len;
	}
	fhs_str[str_pos] = '\0';
	hash = scx_hash(fhs_str);
	TRACE((T_DEBUG, "hash = %u, string = %s\n", hash, fhs_str));
	return hash;
}

/*
 * 첫번째 서버에서 실패(400이상)가 발생하는 경우 다음 서버에 요청을 한다.
 * 마지막 성공한 서버를 기억하고 있다가 시도 해보고 다음 부터는 성공한 서버(gscx__cr_server_cur)로 요청?
 * reload시는 다시 첫 서버 부터 요청
 */
static int
cr_request_get_to_redis(char *path, cr_body_info_t *body_info)
{
	int	ret = 0;
	char		url[CR_MAX_URL_SIZE] = { 0, };

	CURL 		*c = NULL;
	int 		body_size = 0;
	long		ret_code = 0;
	double 		length;
	CURLcode 	errornum;
	char 		errbuf[CURL_ERROR_SIZE];
	int			retry_cnt = gscx__cr_server_count + 1;
	int			i;

//	snprintf(url, 1280, "http://192.168.130.63:8080/test4.txt");

	if (gscx__cr_server_count == 0) {
		return -1;
	}
	while (retry_cnt--) {
		body_info->buffer[0] = '\0';
		body_info->cursor = 0;

		snprintf(url, CR_MAX_URL_SIZE, "http://%s/%s", gscx__cr_server[gscx__cr_server_cur], path);

		c = curl_easy_init ();	/* thread 강제 종료에 의한 메모리릭 가능성 때문에 여기서 curl context를 할당서 thread에 넘겨 준다. */

		curl_easy_setopt(c, CURLOPT_URL, url);

		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cr_webdis_body_read_callback);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, body_info);
		curl_easy_setopt(c, CURLOPT_FAILONERROR, 1);	/* Fail on HTTP 4xx errors. */
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 1);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 1);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
		/*
		 * http 1.0으로 하는 경우는 서버와 클라이언트중 어느쪽이 먼저 끊을지 알수 없기 때문에 1.1로 설정해서 client(monitoring)가 먼저 끊도록 한다.
		 * https://jarvis.solbox.com/redmine/issues/31946 참조
		 */
		curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	//	curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 1L);	/* socket 재사용 금지, curl_easy_perform()수행후 연결을 종료한다. */

		errornum = curl_easy_perform(c);
		if (CURLE_OK != errornum) {
			/* curl에서 error를 리턴하는 경우는 해당 서버를 제외하고 다른 서버로 다시 요청 한다. */
			gscx__cr_server_cur++;
			if(gscx__cr_server_cur >= gscx__cr_server_count ) {
				gscx__cr_server_cur = 0;
			}
			TRACE((T_WARN, "update redis curl error, '%s'(%d), url(%s)\n", errbuf, errornum, url));
			/*
			 * webdis 접속 실패시
			 * 16:34:22.290 17397 [ WARN] cr_request_get_to_redis: update redis curl error, 'Failed to connect to 192.168.110.138 port 7379: Connection refused'(7), url(http://192.168.110.138:7379/SET/solvod_fhs-domain.solbox.com/1|fhs-domain.solbox.com|0|5120|1|50|1602056062/ex/3)
			 * webdis hang 발생시, redis hang 발생시
			 * 16:30:18.665 17048 [ WARN] cr_request_get_to_redis: update redis curl error, 'Operation timed out after 1001 milliseconds with 0 bytes received'(28), url(http://192.168.110.139:7379/KEYS/solvod_*)
			 * redis 서버 down시
			 * 16:34:50.007 17397 [ WARN] cr_request_get_to_redis: update redis curl error, 'The requested URL returned error: 503 Service Unavailable'(22), url(http://192.168.110.138:7379/KEYS/solvod_*)
			 */
			curl_easy_cleanup (c);
			c = NULL;
			ret = -1;
			continue;
		}
		else {
			curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, (long *)&ret_code);
			curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD,  (double *)&length);
			/* return code가 200이 나와야함, 이외의 코드는 무조건 에러처리 */
			if (ret >= 500) {
				/* return code가 5xx인 경우는 해당 서버를 제외하고 다른 서버로 다시 요청한다. */
				gscx__cr_server_cur++;
				if(gscx__cr_server_cur >= gscx__cr_server_count ) {
					gscx__cr_server_cur = 0;
				}
				TRACE((T_WARN, "update redis curl return code(%ld) error, url(%s)\n", ret_code, url));
				curl_easy_cleanup (c);
				c = NULL;
				ret = -1;
				continue;
			}
			else if (200 != (int)ret_code) {
				/* 200 이 아닌 500 미만의 에러코드가 나오는 경우는 재시도를 하지 않고 바로 에러 처리 한다. */
				TRACE((T_INFO, "update redis curl return code(%ld) error, url(%s)\n", ret_code, url));
				ret = -1;
				goto end_cr_request_get_to_redis;
			}
			body_info->buffer[body_info->cursor] = '\0';
			TRACE((T_DEBUG,"curl update ret = %d, length = %f, body = %s\n", ret_code, length, body_info->buffer));
			ret = 0;
			break;
		}
	}
end_cr_request_get_to_redis:
	if (c) {
		curl_easy_cleanup (c);
	}
	return ret;
}


/*
 * json 형태
 ** {"KEYS":["solvod_k20245-063","solvod_server_1","solvod_server_2","solvod_server_4","solvod_server_3"]}
 ** {"KEYS":["solvod_k20245-063"]}  --> key가 한개인 경우
 ** {"KEYS":[]}	--> 한개도 없는 경우
 * json으로 되어 있는 key 목록을 /key1/key2/key3... 의 형태로 만들어서 path에 넣어 준다.
 */
static int
cr_parse_server_list(char *body, char *path, int path_max)
{
	int	ret = 0;
	json_error_t error;
	json_t *root = NULL;
	json_t *array;
	size_t index;
	json_t *element;
	const char * value;
	size_t size = 0;
	int	path_pos = 0;

	root = json_loads(body, 0, &error);
	if(!root){
		TRACE((T_INFO, "json load failed. %d: %s\n", error.line, error.text));
		ret = -1;
		goto end_cr_parse_server_list;
	}
	array = json_object_get(root, "KEYS");
	if(!array){
		TRACE((T_INFO, "json get root keys failed.\n"));
		ret = -1;
		goto end_cr_parse_server_list;
	}
	if(!json_is_array(array)){
		TRACE((T_INFO, "Invalid json root keys type.\n"));
		ret = -1;
		goto end_cr_parse_server_list;
	}
	size = json_array_size(array);

	if (size == 0) {
		TRACE((T_INFO, "Empty server list.\n"));
		ret = -1;
		goto end_cr_parse_server_list;
	}
	/* 여기서 목록을 string으로 생성한다. */
	json_array_foreach(array, index, element){
		if (json_is_string(element)) {
			value = json_string_value(element);
			path_pos += snprintf(path+path_pos, path_max - path_pos, "/%s", value);
		}
	}
//	path_pos += snprintf(path+path_pos, path_max - path_pos, "/notfound"); // 테스트를 위해 없는 서버도 추가
end_cr_parse_server_list:
	if (root != NULL) {
		json_decref(root);
	}

	return ret;
}


/*
 * 서버들의 상태 정보가 들어 있는 json을 parsing 한다.
 * json 형태
 ** {"MGET":["1|fhs-domain.solbox.com|0|5120|0|50|1600306850","1|server1.solbox.com|1903|5120|2|50|1600237883","1|server2.solbox.com|1533|5120|23|50|1600237853","1|server3.solbox.com|1347|5120|55|50|1600236852","1|server4.solbox.com|5347|5120|5|50|1600234856"]}
 ** {"MGET":["1|fhs-domain.solbox.com|0|5120|0|50|1600310215"]}  --> key가 한개인 경우
 ** {"MGET":["1|fhs-domain.solbox.com|0|5120|0|50|1600310215",null]}--> 요청한 key중 마지막 key가 없는 경우
 */
static int
cr_parse_servers_info_list(char *body, int	working_array)
{
	int	ret = 0;
	json_error_t error;
	json_t *root = NULL;
	json_t *array;
	size_t index;
	json_t *element;
	const char * value;
	size_t size = 0;
	int	path_pos = 0;
	cr_fhs_stat_info_t *fhs_info = NULL;
	int	array_num = 0;


	gscx__cr_fhs_stat_array_using_size[working_array] = 0;

	root = json_loads(body, 0, &error);
	if(!root){
		TRACE((T_INFO, "json load failed. %d: %s\n", error.line, error.text));
		ret = -1;
		goto end_cr_parse_servers_info;
	}
	array = json_object_get(root, "MGET");
	if(!array){
		TRACE((T_INFO, "json get root keys failed.\n"));
		ret = -1;
		goto end_cr_parse_servers_info;
	}
	if(!json_is_array(array)){
		TRACE((T_INFO, "Invalid json root keys type.\n"));
		ret = -1;
		goto end_cr_parse_servers_info;
	}
	size = json_array_size(array);

	if (size == 0) {
		TRACE((T_INFO, "Empty server list.\n"));
		ret = -1;
		goto end_cr_parse_servers_info;
	}
	/* 여기서 목록을 string으로 생성한다. */
	json_array_foreach(array, index, element){
		if (json_is_null(element)) {
			/*
			 * 요청한 key를 찾치 못한 경우는 null이 들어 있다.
			 * 이 경우는 그냥 skip 한다.
			 */
		}
		else {
			value = json_string_value(element);
			fhs_info = (cr_fhs_stat_info_t *)(&gscx__cr_fhs_stat_array[working_array][array_num]);
			if (cr_make_server_info(value, fhs_info) < 0 ) {
				TRACE((T_DAEMON, "server status string parse failed(%s)\n", value));
				continue;
			}
			/*
			 * cr_policy가  CR_POLICY_ROUTE_NEXT_SERVER 인 경우는
			 * content routing이 가능한 서버인지 확인해서 문제가 있는 경우에는 서버 목록(gscx__cr_fhs_stat_array)에 추가 하지 않는다
			 */
			if (gscx__cr_policy == CR_POLICY_ROUTE_NEXT_SERVER) {
				if (cr_check_available_node(fhs_info) == 0) {
					TRACE((T_DEBUG, "Invalid fhs(%s) skipped.\n", fhs_info->domain));
					continue;
				}

			}
			array_num++;
			if (array_num >= CR_MAX_FHS_STAT_ARRAY_SIZE) {
				TRACE((T_INFO, "fhs info list count over %d\n", CR_MAX_FHS_STAT_ARRAY_SIZE));
				goto end_cr_parse_servers_info;
			}
//			printf("value = %s\n", value);
		}
	}
end_cr_parse_servers_info:
	if (root != NULL) {
		json_decref(root);
	}
	if (ret == 0 && array_num > 0) {
		gscx__cr_fhs_stat_array_using_size[working_array] = array_num;
	}

	return ret;
}

/*
 * 문자열 형태
 * 저장 순서 : version|domain(IP)|current traffic|threshold traffic|현재 cpu 사용률|threshold cpu 사용률|현재 시간(unix timestamp)
 * 예 : 1|fhs3017.gn-21.lgucdn.com|1847|5120|5|50|1600234856
 */
static int
cr_make_server_info(const char *info_str, cr_fhs_stat_info_t *fhs_info)
{
	int		ret = 0;
	char	*tok;
	int 	version;
	char	temp_str[120];

	snprintf(temp_str, 120, "%s", info_str);
	tok = strtok(temp_str, "|");
	if (tok == NULL) {
		return -1;
	}
	version = atoi(tok);
	// 버전이 틀린 경우 바로 리턴
	if (version != CR_VERSION) {
		TRACE((T_DAEMON, "different content router version(%d). (%s)\n", CR_VERSION, info_str));
		return -1;
	}
	fhs_info->version = version;
	tok = strtok(NULL, "|");
	if (tok == NULL) {
		return -1;
	}
	snprintf(fhs_info->domain, 40, "%s", tok);
	tok = strtok(NULL, "|");
	if (tok == NULL) {
		return -1;
	}
	fhs_info->current_traffic = atoi(tok);
	tok = strtok(NULL, "|");
	if (tok == NULL) {
		return -1;
	}
	fhs_info->threshold_traffic = atoi(tok);
	tok = strtok(NULL, "|");
	if (tok == NULL) {
		return -1;
	}
	fhs_info->cpu_usage = atoi(tok);
	tok = strtok(NULL, "|");
	if (tok == NULL) {
		return -1;
	}
	fhs_info->threshold_cpu_usage = atoi(tok);
	tok = strtok(NULL, "|");
	if (tok == NULL) {
		return -1;
	}
	fhs_info->update_time = atoi(tok);

	return ret;
}

/*
 * 가상 노드를 지정된 수(node_per_fhs) 만큼 만든다
 */
static int
cr_make_virtual_node(cr_fhs_stat_info_t *fhs_info, int working_array, int pos, int node_per_fhs)
{
	int		ret = 0;
	int		i;
	int		array_pos = pos * node_per_fhs;
	char 	node_name[128];
	cr_node_hash_t *node;
	unsigned int hash;


	for(i = 0; i < node_per_fhs; i++) {
		node = (cr_node_hash_t *)(&gscx__cr_node_hash_array[working_array][array_pos]);

		/*
		 * 숫자를 뒤에다 넣을 경우 hash 함수 특성상 hash 값이 비슷하게 나오는 문제가 있어서 숫자를 앞에다 배치 한다.
		 * 필요한 경우 여기서 나온 hash 값으로 google consistent hash 함수를 한번 더 돌리면 되지만 이정도면 충분 할것 같아서 여기 까지만 함
		 */
		node->info = fhs_info;
		snprintf(node_name, 128, "%d_%s", i,fhs_info->domain);
		hash = scx_hash(node_name);
		node->hash = hash;
	//	printf("node_name = %s, hash = %ld, node num = %d\n", node_name, hash, array_pos);
		TRACE((T_DEBUG, "node_name = %s, hash = %ld, node num = %d\n", node_name, hash, array_pos));

		array_pos++;
	}

	return ret;

}

/*
 * 가상 노드를 만들고 consistent hash ring을 구성한다.
 */
static int
cr_make_consistent_hash_ring(int working_array)
{
	int		ret = 0;
	int 	i;
	int		fhs_stat_size = gscx__cr_fhs_stat_array_using_size[working_array];
	cr_fhs_stat_info_t *fhs_info = NULL;
	cr_node_hash_t *node;
	int		fhs_pos = 0;	/* 유효한 FHS 수 */

	/* FHS별 가상 노드 생성 */
	for (i = 0; i < fhs_stat_size; i++) {
		fhs_info = (cr_fhs_stat_info_t *)(&gscx__cr_fhs_stat_array[working_array][i]);
		if (cr_make_virtual_node(fhs_info, working_array, fhs_pos++, CR_SIZE_OF_VIRTUAL_NODE_PER_FHS) < 0) {
			TRACE((T_WARN, "make virtual node failed(%s)\n", fhs_info->domain));
			ret = -1;
			goto end_cr_make_consistent_hash_ring;
		}
	}
	gscx__cr_fhs_stat_array_using_size[working_array] = fhs_pos;
	qsort( gscx__cr_node_hash_array[working_array], fhs_pos*CR_SIZE_OF_VIRTUAL_NODE_PER_FHS, sizeof(cr_node_hash_t), cr_compare_consistent_hash_node);
	for (i = 0; i < fhs_pos*CR_SIZE_OF_VIRTUAL_NODE_PER_FHS; i++) {
		node = (cr_node_hash_t *)(&gscx__cr_node_hash_array[working_array][i]);
		//printf("get node_name = %s, hash = %ld, node num = %d\n", node->info->domain, node->hash, i);

	}

end_cr_make_consistent_hash_ring:

	return ret;
}


/*
 * This type defines the function used to compare two elements.
 * The result should be less than zero if left is less than right,
 * zero if they are equal, and bigger than zero if left is bigger than right.
 */
static int
cr_list_compare(const void *left, const void *right, CompareInfo *arg)
{
	cr_node_hash_t *left_info = *(cr_node_hash_t **)left;
	cr_node_hash_t *right_info = *(cr_node_hash_t **)right;
	//printf("left = %ld, right = %ld, left path = %s, right path = %s\n", left_info->hash, right_info->hash, left_info->info->domain, right_info->info->domain);
    if (left_info->hash < right_info->hash)
        return -1;
    else if (left_info->hash > right_info->hash)
        return 1;
    return 0;
}

static int
cr_compare_consistent_hash_node(const void *left, const void *right)
{
	cr_node_hash_t *left_info = (cr_node_hash_t *)left;
	cr_node_hash_t *right_info = (cr_node_hash_t *)right;
	//printf("left = %ld, right = %ld, left path = %s, right path = %s\n", left_info->hash, right_info->hash, left_info->info->domain, right_info->info->domain);
    if (left_info->hash < right_info->hash)
        return -1;
    else if (left_info->hash > right_info->hash)
        return 1;
    return 0;
}



/*
 * 요청된 경로 기준으로 생성한 hash값으로 node의 hash 값보다 작으면서 가장 가까운 node를 찾는다.
 * 모든 노드의 hash 값보다 경로의 hash값이 큰 경우는 처음 노드가 선택 된다.
 * 선택된 노드의 상태를 확인해서 문제가 있는 경우 NULL 을 리턴하고 문제가 없는 경우는 서버의 주소를 리턴한다.
 ** NULL이 아니라 다른 노드를 찾아서 리턴해야 하는 경우 문제가 있는 노드 앞에 노드를 계속 확인 하는 방식으로 구현하면 됨
 * 현재의 트래픽이나 CPU 사용률이 threshold 값을 초과 하는 경우 노드에 문제가 있다고 판단한다.
 * 현재 사용중인 array 확인은 gscx__cr_fhs_stat_array_cur를 통해서 한다.
 ** 확인시에 lock을 사용하지 않는다.
 ** 사용중이 아닌 array를 사용한다고 해도 전체가 정적 array로 구성 되어 있기 때문에 죽는 현상이 발생하거나 하지는 않는다.
 * 리턴값 : 선택된 노드의 IP 나 domain, 선택된 노드에 문제가 있는 경우 NULL을 리턴
 */
char *
cr_find_node(nc_request_t *req, const char *path)
{
	int service_array = gscx__cr_fhs_stat_array_cur; 	/* 현재 사용중인 array의 번호, 0 or 1 */
	unsigned int path_hash;
	cr_node_hash_t *node;

	if (path == NULL || gscx__cr_working == 0) {
		return NULL;
	}
	if (gscx__cr_fhs_stat_array_using_size[service_array] <= 0) {
		/* consistent hash ring에 등록된 FHS가 한개도 없는 경우 */
		return NULL;
	}
	path_hash = scx_hash(path);
	TRACE((T_DEBUG,"[%llu] path(%s), hash(%u)\n", req->id, path, path_hash));
	node = find_node_from_consistent_hash_ring(req, path_hash, service_array);
	if (node == NULL) {
		return NULL;
	}

	if (gscx__cr_policy == CR_POLICY_NO_ROUTE) {
		/* 선정된 서버가 threshold 값을 넘는 경우인지 확인 */
		if (cr_check_available_node(node->info) == 0) {
			TRACE((T_DEBUG, "Invalid fhs(%s)\n", node->info->domain));
			return NULL;
		}
	}

	//printf("path %s, hash = %ld, select node hash = %ld, domain = %s\n", path, path_hash, node->hash, node->info->domain);
	return node->info->domain;
}

/*
 * 해당 FHS의 현재 상태가 threshold를 넘겼는지와
 * 마지막 업데이트 시간을 확인해서 서비스 가능한 서버 인지를 확인한다.
 * 서비스 가능한 경우 1을 리턴하고
 * 서비스가 불가능한 서버의 경우 0을 리턴한다.
 */
static int
cr_check_available_node(cr_fhs_stat_info_t *fhs_info)
{
	TRACE((T_DEBUG, "fhs(%s), traffic(%d:%d), cpu(%d:%d), time(%d:%d)\n"
			, fhs_info->domain
			, fhs_info->current_traffic, fhs_info->threshold_traffic
			, fhs_info->cpu_usage, fhs_info->threshold_cpu_usage
			, fhs_info->update_time, (scx_get_cached_time_sec() - 60) ));
	if (fhs_info->current_traffic > fhs_info->threshold_traffic) {
		return 0;
	}
	if (fhs_info->cpu_usage > fhs_info->threshold_cpu_usage) {
		return 0;
	}
	if ( fhs_info->update_time < (scx_get_cached_time_sec() - 60) ) {
		/* 서버의 마지막 업데이트 시간이 60초이상 지난 경우에도 content routing을 시키지 않는다. */
		return 0;
	}
	return 1;
}

/*
 * consistent hash 알고리즘으로 node를 찾는 함수
 * consistent hash ring 검색 로직
 ** 지정된 hash 값과 가장 가까우면서 더 작거나 같은 값을 가지는 노드를 선택
 ** hash ring의 가장 작은 hash 값을 가지는 노드 보다  path_hash가 더 작은 경우 hash값이 가장 큰 노드가 선택됨
 * 찾는 방식은 binary search 알고리즘을 응용
 ** https://www.programmingsimplified.com/c/source-code/c-program-binary-search
 */

static cr_node_hash_t *
find_node_from_consistent_hash_ring(nc_request_t *req, const unsigned int path_hash, int service_array)
{
	cr_node_hash_t *node;
	int	array_size = gscx__cr_fhs_stat_array_using_size[service_array] * CR_SIZE_OF_VIRTUAL_NODE_PER_FHS;
	int first, last, middle;

	first = 0;
	last = array_size-1;
	middle = (first+last)/2;

	while (1) {
		TRACE((T_DEBUG,"[%llu] pre first(%d), middle(%d) last(%d)\n", req->id, first, middle, last));
		node = (cr_node_hash_t *)(&gscx__cr_node_hash_array[service_array][middle]);
		TRACE((T_DEBUG,"[%llu] first(%d), middle(%d), last(%d), node(%d) hash(%u), fhs(%s)\n", req->id, first, middle, last, middle, node->hash, node->info->domain));
		if (node->hash < path_hash)
			first = middle + 1;
		else if (node->hash == path_hash) {
			/* hash 값이 정확하게 일치 하는 노드를 찾은 경우 */
			TRACE((T_DEBUG,"[%llu] choose node %d, hash(%u), fhs(%s)\n", req->id, middle, node->hash, node->info->domain));
			break;
		}
		else {
			last = middle - 1;
		}
		if (last < 0) {
			/* 찾는 hash 값이 제일 작은 노드 hash 값 보다 작은 경우는 consistent hash ring 구조상  hash 값이 가장 큰 마지막 노드를 리턴한다. */
			node = (cr_node_hash_t *)(&gscx__cr_node_hash_array[service_array][array_size-1]);
			TRACE((T_DEBUG,"[%llu] choose node %d, hash(%u)\n", req->id, array_size-1, node->hash, node->info->domain));
			break;
		}
		else if (first > last) {
			node = (cr_node_hash_t *)(&gscx__cr_node_hash_array[service_array][last]);
			TRACE((T_DEBUG,"[%llu] choose node %d, hash(%u), fhs(%s)\n", req->id, last, node->hash, node->info->domain));
			break;
		}
		middle = (first + last)/2;


	}

	return node;
}

