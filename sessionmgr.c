#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/stat.h>


//#include <ncapi.h>
//#include <trace.h>
#include <microhttpd.h>
#include <dict.h>
#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
#include "scx_util.h"
#include "streaming.h"

#include "sessionmgr.h"
#include "module.h"

/*
 * session기능에 사용되는 hash key는 rand에서 나오는 session을 사용한다.
 * Wowza의 경우는 client에서 session id가 들어 오지 않는 경우는 새로운 ID를 생성한다(crossdomain.xml 요청은 제외).
 * solproxy는 menifest에 대해서만 session id가 없을 경우 생성을 한다.
 * session id의 파라미터는 solsessionid으로 한다.
 * session id가 동일하면 같은 무조건 같은 session이라고 판단한다. (session id의 중복 가능성 무시)
 * lock관련 기능 추가 해야함
 */


static pthread_mutex_t 	gscx__session_list_lock = PTHREAD_MUTEX_INITIALIZER;
dict 	*scx_session_dct = NULL;
int		gscx__session_monitoring_interval = 5;	/* session의 만료를 검사하는 주기. 단위 : 초 */
int		gscx__session_timeout = 600;	/* 마지막 사용후 최대 session 유지(만료)시간. 단위 : 초 */
pthread_t 	sm_monitor_tid;
int		gscx__session_count = 0;

module_driver_t gscx__session_module = {
	.name 				= "session",	/* name */
	.desc				= "session manager module",	/* description */
	.version			= 1,				/* version */
	.init				= sm_module_init,	/* init func */
	.deinit				= sm_module_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};

#if 0
int sm_module_volume_lookup(phase_context_t *ctx);
#else
int sm_module_client_request(phase_context_t *ctx);
#endif

int sm_module_complete(phase_context_t *ctx);
void *sm_monitor(void *d);
int sm_strcmp(const void *k1, const void *k2);
unsigned sm_create_hash(const void *p);
void sm_key_val_free(void *key, void *datum);
int sm_remove_element(session_info_t * session);

void
sm_init()
{
    dict_compare_func cmp_func = sm_strcmp;
    dict_hash_func hash_func = sm_create_hash;

    scx_session_dct = hashtable_dict_new(cmp_func, hash_func, DICT_HSIZE);
    ASSERT(dict_verify(scx_session_dct));

	pthread_create(&sm_monitor_tid, NULL, sm_monitor, (void *)scx_session_dct);

}

void
sm_deinit()
{
//	pthread_mutex_lock(&gscx__session_list_lock);
//	pthread_mutex_unlock(&gscx__session_list_lock);
	/* 아래 부분이 필요 할수도 있음 */
	if (sm_monitor_tid)
	{
		pthread_cancel(sm_monitor_tid);
		pthread_join(sm_monitor_tid, NULL);
	}

	if (scx_session_dct)
		dict_free(scx_session_dct, sm_key_val_free);
}

int
sm_module_init()
{

//#ifdef ZIPPER		/* session은 streaming에서만 사용하도록 한다. */
	sm_init();
#if 0
	scx_reg_phase_func(PHASE_VOLUME_LOOKUP, sm_module_volume_lookup);
#else
	scx_reg_phase_func(PHASE_CLIENT_REQUEST, sm_module_client_request);
#endif
	scx_reg_phase_func(PHASE_COMPLETE, sm_module_complete);
//#endif

	return SCX_YES;
}

int
sm_module_deinit()
{

//#ifdef ZIPPER
	sm_deinit();
//#endif
	return SCX_YES;
}


#if 0
int
sm_module_volume_lookup(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;

	if (req->streaming == NULL) return SCX_YES;
//#ifdef ZIPPER
	if (O_PROTOCOL_DASH == req->streaming->protocol
			|| O_PROTOCOL_HLS == req->streaming->protocol
			|| O_PROTOCOL_MSS == req->streaming->protocol) {
		/* session은 dash나 hls에서만 사용이 가능하다. */
		if (!sm_handle_session(req)) {
			scx_error_log(req, "Session handler error. URL(%s)\n", vs_data(req->url));
			if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
			return SCX_NO;
		}
	}
//#endif
	return SCX_YES;
}
#else
/*
 * soluri나 jwt 인증 사용시 특정 요청에 대해 무인증 처리를 하기 위해서 PHASE_VOLUME_LOOKUP에서 PHASE_CLIENT_REQUEST phase로 변경
 */
int
sm_module_client_request(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;

	if (req->streaming == NULL) return SCX_YES;
//#ifdef ZIPPER
	if (O_PROTOCOL_DASH == req->streaming->protocol
			|| O_PROTOCOL_HLS == req->streaming->protocol
			|| O_PROTOCOL_MSS == req->streaming->protocol) {
		/* session은 dash나 hls에서만 사용이 가능하다. */
		if (!sm_handle_session(req)) {
			scx_error_log(req, "Session handler error. URL(%s)\n", vs_data(req->url));
			if (!req->p1_error) req->p1_error = MHD_HTTP_BAD_REQUEST;
			return SCX_NO;
		}
	}
//#endif
	return SCX_YES;
}
#endif

int
sm_module_complete(phase_context_t *ctx)
{
	nc_request_t *req = (nc_request_t *)ctx->req;

	if (req->streaming == NULL) return SCX_YES;
//#ifdef ZIPPER
//	sm_release_session(req);
//#endif
	return SCX_YES;
}


int
sm_handle_session(nc_request_t *req)
{

	streaming_t *streaming = req->streaming;
	ASSERT(streaming);
	if (req->service->session_enable == 0) {
		return 1;
	}
#if 0
	if (streaming->media_type == MEDIA_TYPE_CROSSDOMAIN) {
		/* crossdomain.xml요청은 session과 관계 없이 동작한다. */
		return 1;
	}
#endif
	streaming->session = sm_check_session(req);
	if(streaming->session == NULL) {

		return 0;
	}
	return 1;
}


/*
 * 해당 session이 리스트에 있는지 확인하고 없는 경우 새로운 session을 생성한다.
 * session이 없는 경우 새로 생성할건지를 옵션으로 받을수 있어야함.
 */
session_info_t *
sm_check_session(nc_request_t *req)
{
	streaming_t 	*streaming = req->streaming;
	session_info_t * session = NULL;
	//char key[MAX_SESSION_KEY_LEN] = {'\0'};
	int	key = 0;
	const char *val = NULL;
	int	id = 0;
	ASSERT(streaming);
	val = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, vs_data(req->service->session_id_name));
	if (val) {
		/* argument에 들어 있는 session id가 숫자로 되어 있는지 확인 한다. */
		if (scx_is_valid_number(val) != 1) {
			scx_error_log(req, "Session key not valid URL(%s)\n", vs_data(req->url));
			req->p1_error = MHD_HTTP_UNAUTHORIZED;
			return NULL;
		}
		else if (strlen(val) > 10 ){
			scx_error_log(req, "Session key not valid URL(%s). too large number(%s)\n", vs_data(req->url), val);
		}
		streaming->session_id = atoi(val);
	}
	pthread_mutex_lock(&gscx__session_list_lock);
	if (streaming->session_id) {
		/* session_id가 request에 포함 되어 있는 경우에는 먼저 해당 session id가 리스트에 있는지 확인 한다.
		 * 리스트에서 확인할때 컨텐츠 경로(url)가 동일한지도 확인 해야함.
		 * 리스트에서 찾지 못한 경우 해당 ID를 사용해서 새로운 session을 생성 한다.
		 * 이 과정에서 해당 ID의 형식이 맞는것인지는 확인 해야 할까?
		 * 예를 들어 악의적으로 session id를 동일한것으로 요청하게 되면 문제가 발생할수 있음 */
		//snprintf(key, MAX_SESSION_KEY_LEN, "%d", req->session_id);
		key = streaming->session_id;
		void** search = dict_search(scx_session_dct, key);
		if(search != NULL) {
			session = *(session_info_t **)search;
			if (strcmp(session->key, streaming->key) != 0) {
				/* session 생성시의 요청 컨텐츠와 현재의 컨텐츠가 다른 경우는 에러 처리한다.*/
				pthread_mutex_unlock(&gscx__session_list_lock);
				scx_error_log(req, "Session key not matched URL(%s)\n", vs_data(req->url));
				req->p1_error = MHD_HTTP_UNAUTHORIZED;
				return NULL;
			}
			req->expired = 0;
		}
		else {	/* session이 리스트에 없는 경우 */
			if (gscx__permit_expire == 1 && req->expired == 1) {
				/* 데몬이 재기동 된 상태에서 이전 세션으로 요청이 들어온 경우라고 보고 만료가 되었더라도 허용해준다. */
				req->expired = 0;
			}

		}
	}
	if (1 == req->expired) {
		/* 만료된 인증을 사용한 경우 이기 때문에 session을 생성하지 않고 바로 리턴한다. */
		pthread_mutex_unlock(&gscx__session_list_lock);
		scx_error_log(req, "Expired request, Session not created(%s)\n", vs_data(req->url));
		req->p1_error = MHD_HTTP_UNAUTHORIZED;
		return NULL;
	}
	if (!session) {
		/* 리스트에서 session을 찾지 못한 경우나 요청에 session id가 포함되어 있지 않은 경우는  새로운 session을 생성한다. */
		session = sm_create_session(req);
	}
	/*
	 * 동기화 문제 때문에 pthread_mutex_unlock(&gscx__session_list_lock) 이전에 아래를 처리한다.
	 * dead lock 발생 할 경우 전체적인 로직 재 점검 필요
	 */
	pthread_mutex_lock(&session->lock);
	session->refcount++;
	session->account++;
	session->end_time = time(NULL);
	pthread_mutex_unlock(&session->lock);

	pthread_mutex_unlock(&gscx__session_list_lock);


	return session;
}

session_info_t *
sm_create_session(nc_request_t *req)
{
	streaming_t 	*streaming = req->streaming;
	int session_id = streaming->session_id;
	mem_pool_t 		*mpool = NULL;
	session_info_t * session = NULL;

	if(session_id == 0)	/* session id 가 지정되지 않은 경우는 새로 생성한다. */
		session_id = sm_create_id();
	ASSERT(session_id);
	/* pool부터 생성 */
	mpool = mp_create(strlen(streaming->key) + sizeof(session_info_t) + 1024);
	ASSERT(mpool);
	session = (session_info_t *)mp_alloc(mpool, sizeof(session_info_t));
	ASSERT(session);
	session->mpool = mpool;
	//snprintf(session->id, MAX_SESSION_KEY_LEN, "%d", session_id);
	session->id = session_id;
	session->key = mp_strdup(session->mpool, streaming->key);
	session->host = mp_strdup(session->mpool, vs_data(req->host));
	session->client_ip = mp_strdup(session->mpool, vs_data(req->client_ip));
	session->timeout = req->service->session_timeout;
	session->start_time = session->end_time = req->t_req_lba / 1000000; /* start_time의 기준은 client의 요청이 완료된 시간이다. 시간의 단위가 틀리므로 변환을 한다.*/

	/* streaming type 통계를 사용하는 경우에만 아래 부분이 실행 된다. */
	if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
		if (req->service->streaming_method == 1 || req->service->hls_modify_manifest_enable == 1) {
			/*
			 * mms 통계기록시 live 구분을 위해서 사용
			 * dash live 기능이 추가 되는 경우 여기에 추가 해야 한다.
			 */
			session->is_live = 1;
		}
		else {
			/* VOD 인 경우 */
			session->is_live = 0;
		}
	}

	if (streaming->real_path_len > 0) {
		/*
		 * 스트리밍 기능사용시(zipper로 동작이나 solproxy에서 streaming_enable가 1일 경우)에 HTTP(Content) 통계에 남는 FilePath  부분에 Client의 요청 경로에서 가상 파일 경로를 제외하고 남기도록 한다
		 * 여기서  real_path_len에 +1을 하지 않는 이유는 real_path_len에 마지막 / 까지 포함 되어 있기 때문에 /를 제외 하기 위함임
		 */
		session->path = mp_alloc(session->mpool,streaming->real_path_len);
		snprintf(session->path, streaming->real_path_len, "%s", vs_data(req->ori_url));
	}
	else {
		/* client로 부터 요청된 경로를 그대로 기록 */
		session->path = mp_strdup(session->mpool, vs_data(req->path));
	}
#ifdef DEBUG
	printf("session->path = '%s'\n", session->path);
#endif
	session->service = req->service;
	session_update_service_start_stat(req->service);

	session->req_hdr_size = 0;
	session->req_body_size = 0;
	session->res_hdr_size = 0;
	session->res_body_size = 0;
	session->refcount = 0;
	session->account = 0;
#ifdef ZIPPER
	session->l_info.z_ctx = NULL;
	session->l_info.ad_finished = 0;
#endif
	pthread_mutex_init(&session->lock, NULL);
	/* 필요한 정보들을 session에 넣는다. */
	dict_insert_result result = dict_insert(scx_session_dct, session->id);
	if (result.inserted) {
		ASSERT(result.datum_ptr != NULL);
		ASSERT(*result.datum_ptr == NULL);
		*result.datum_ptr = session;
	}
	ATOMIC_ADD(gscx__session_count, 1);
	TRACE((T_DEBUG, "Session created. ID(%d), key(%s)\n", session->id, session->key));
	/************************** Session Create Phase Handler가 실행 되는 부분 *************************************/
	return session;
}

/*
 * 모듈 구조 이기 때문에 PHASE_COMPLETE handler에서 동작 해야 하지만
 * 이렇게 하는 경우 중간에 req->streaming이 사라지는 경우에 대해 처리가 안되는 문제가 있어서
 * strm_destroy_stream()에서 직접 호출 하도록 한다.
 */
void
sm_release_session(nc_request_t *req)
{
	streaming_t 	*streaming = req->streaming;
	ASSERT(streaming);
	session_info_t * session = streaming->session;
	int	remove = 0;
	if (NULL == session)
		return;
	time_t 		t_val = time(NULL);
	pthread_mutex_lock(&session->lock);
	session->end_time = t_val;
	--session->refcount;
	session->req_hdr_size += req->req_hdr_size;
	session->req_body_size += req->req_body_size;
	session->res_hdr_size += req->res_hdr_size;
	session->res_body_size += req->res_body_size;
	ASSERT(session->refcount >= 0);
	if (400 <= req->p1_error && 1 == session->account) {
		/* 세션 생성후 바로 에러가 발생한 경우에만 session을 삭제하도록 한다. */
		remove = 1;
	}
	if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
		/*
		 * streaming type 통계를 사용하면서 VOD인 경우
		 * 처음 한번 content size와 duration을 업데이트 해준다.
		 */
		if (session->is_live == 0 && session->duration == 0) {
			if (streaming->builder != NULL) {
				session->duration = (int) req->streaming->builder->duration;
				if (streaming->builder->media_list != NULL) {
					/* 전송 해야할 파일 크기는 첫번째 파일의 크기로 한다. */
					session->file_size = streaming->builder->media_list->media->msize;
				}
			}
		}
	}
	pthread_mutex_unlock(&session->lock);

	TRACE((T_DEBUG, "Session released. ID(%d), key(%s), end time(%ld)\n", session->id, session->key, session->end_time));
	if (1 == remove) {	/* 동기화 문제가 발생할 수 있어서 위에서 바로 삭제 하지 않고 여기에서 한다. */
		sm_destroy_session(session, 1/* lock */, 0/* http 통계 기록안함 */);
	}
	streaming->session = NULL;
}

/*
 * width_lock이 1인 경우 lock을 사용
 */
void
sm_destroy_session(session_info_t * session, int width_lock, int is_write_http_stat)
{

	TRACE((T_DEBUG, "Session destroyed. ID(%d), key(%s)\n", session->id, session->key));
#ifdef ZIPPER
	if (session->l_info.z_ctx != NULL) {
		/* live session 기능을 사용중인 경우 live session 먼저 삭제 한다. */
		strm_live_session_destory((void *)session);
	}
#endif
	/************************** Session Destroy Phase Handler가 실행 되는 부분 *************************************/
	if (is_write_http_stat) {
		/*
		 * session 생성후 컨텐츠 문제로 바로 session이 삭제되는 경우는
		 * 기존 방식으로 content 통계가 기록 되므로 이중 기록을 막기 위해
		 * session기준 content 통계를  기록하지 않는다.
		 * */
		standalonestat_write_http_session(session);
	}
	session_update_service_end_stat(session->service);
	if (width_lock) pthread_mutex_lock(&gscx__session_list_lock);
	dict_remove(scx_session_dct, session->id);
	if (width_lock) pthread_mutex_unlock(&gscx__session_list_lock);
	sm_remove_element(session);
	ATOMIC_SUB(gscx__session_count, 1);
}

/*
 * session ID를 생성한다.
 * session ID는 rand()에서 나오는 값을 그대로 사용한다.(리턴값이 hash function에서 리턴하는 값과 크기가 동일함)
 * session ID 생성후에 기존의 리스트에 동일한 값이 있는지 확인 한다.
 * 검사한후에 바로 리스트에 넣어야 하지만 상위 function에서 lock을 걸고 있으므로 동기화 문제는 신경 쓰지 않아도됨
 */
int
sm_create_id()
{
	int session_id;
	//char key[MAX_SESSION_KEY_LEN] = {'\0'};
	int key	= 0;
	int	retry = 1000;
	/* 이부분에 대한 lock이 필요함 */
	while(retry-- > 0) {
		session_id = rand();
		/* 해당 session_id 가 사용중인지 확인 하는 부분. */
		//snprintf(key, MAX_SESSION_KEY_LEN, "%d", session_id);
		if(dict_search(scx_session_dct, session_id) == NULL)
			break;
	}
	return session_id;
}

/*
 * 주기적으로 실행되면서 session의 만료를 판단한다.
 */
void *
sm_monitor(void *d)
{
	int interval = gscx__session_monitoring_interval * 1000;
	dict_itor *itor = NULL ;
	char * key = NULL;
	session_info_t * session = NULL;
	time_t 		t_val;
	prctl(PR_SET_NAME, "session manager monitor");
	while (gscx__signal_shutdown == 0) {
		t_val = time(NULL);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		pthread_mutex_lock(&gscx__session_list_lock);

		/* 아래에 검사 부분이 들어간다.
		 * 검사시에 현재 연결중인 session은 검사 대상에서 제외 한다.
		 * */
		TRACE((T_DEBUG|T_DAEMON, "check expired session. count(%d)\n", ATOMIC_VAL(gscx__session_count)));
		itor = dict_itor_new(scx_session_dct);
		dict_itor_first(itor);
		while(dict_itor_valid(itor)) {
			key = (char *)dict_itor_key(itor);
			session = (session_info_t *)*dict_itor_datum(itor);
			dict_itor_next(itor);
			if ( (t_val > (session->end_time + session->timeout)) && (session->refcount == 0) ) {
				/* session 시간 만료 */
				TRACE((T_DEBUG, "Session Expired. ID(%d), key(%s)\n", session->id, session->key));
				sm_destroy_session(session, 0/* no lock */, 1 /* http 통계 기록*/);
			}
			else if ( (t_val > (session->end_time + 600))  && (session->refcount > 0) ) {
				/* 접속되어 있는 상태로 10분이 지난 경우는 정상이라고 보기 어렵다. */
				TRACE((T_WARN, "Session time problem . ID(%d), key(%s), time exceed(%d), refcount(%d)\n", session->id, session->key, t_val - session->end_time, session->refcount));
				/* 이런 현상이 발생했을때의 조치는 나중에 추가 해야함. */
			}
		}
		dict_itor_free(itor);
		pthread_mutex_unlock(&gscx__session_list_lock);
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		bt_msleep(interval);
	}
}


int
sm_strcmp(const void *k1, const void *k2)
{
    return dict_ptr_cmp(k1, k2);
}


unsigned
sm_create_hash(const void *p)
{
    // return dict_str_hash(p); // session id가 random int를 사용하기 때문에 별도의 hashing은 필요 없다.
	//unsigned hash = atoi(p);
	unsigned hash = (unsigned)p;
	return hash;
}

void
sm_key_val_free(void *key, void *datum)
{
	sm_remove_element(datum);
}

int
sm_remove_element(session_info_t * session)
{
	mem_pool_t 		*mpool = NULL;
	pthread_mutex_destroy(&session->lock);
	if (session->mpool)  {
		mpool = session->mpool;
		session->mpool = NULL;
		mp_free(mpool);
	}
}
