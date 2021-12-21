#include <netcache_config.h>
#ifndef WIN32
#include <error.h>
#endif
#include <fcntl.h>
#include <iconv.h>
#ifndef WIN32
#include <langinfo.h>
#endif
#ifndef WIN32
#include <libintl.h>
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>
#include <malloc.h>

#include <curl/curl.h>
#include <curl/easy.h>

#include <netcache.h>
#include <block.h>
#include "util.h"
#include "trace.h"
#include "hash.h"
#include "httpn_driver.h"
#include "httpn_request.h"
#include "http_codes.h"


#define HAVE_SSL 1


#define DAV_POOLDOWN_RETURN(_s_, _url_)  if ((__s) == NULL) { \
											TRACE((T_ERROR, "%s - no available pool\n", _url_)); \
											httpn_seterrno(EREMOTEIO); \
											 \
											return -EREMOTEIO; \
										  }
#define DAV_POOLDOWN_BREAK(_s_, _url_, _e_)  if ((__s) == NULL) { \
											TRACE((T_ERROR, "%s - no available pool\n", _url_)); \
											(_e_) = EREMOTEIO); \
											break; \
										  }

#define			MAX_SESSION_REUSE_LIMIT		10000
#define			REQUEST_RETRIES				4


// NEW 2020-01-20 huibong Redirection 무한 반복을 방지하기 위해 Redirection 최대 횟수 설정 (#32861)
//                - 최대 5회까지만 허용, 6회부터는 오류로 처리되도록 한다.
#define			ALLOW_MAX_REDIRECTION_COUNT		5	

extern pthread_mutex_t		g__iocb_lock;
extern long 				g__mpx_run;
extern int 					__terminating;
extern void  *				__timer_wheel_base;
extern char					_zs[][32];
char						httpn__agent[128] = "NetCache plugin : " DRIVER_NAME;
char	_zmpxstate[][32] = {			
	"0:IOCB_MPX_INIT",
	"1:IOCB_MPX_READY",
	"2:IOCB_MPX_TRY_RUN",
	"3:IOCB_MPX_PAUSED",
	"4:IOCB_MPX_TRY_DONE",
	"5:IOCB_MPX_FINISHED"
};




#ifndef NC_RELEASE_BUILD
extern long __cnt_paused;
extern long __cnt_running;
#endif
extern long g__iocb_count;



#ifdef HAVE_SSL
#include <openssl/crypto.h>
static pthread_mutex_t *ssl_lockarray;
static void 
lock_callback(int mode, int type, char *file, int line)
{
	if (mode & CRYPTO_LOCK)
		pthread_mutex_lock(&(ssl_lockarray[type]));
	else
		pthread_mutex_unlock(&(ssl_lockarray[type]));
}

static unsigned long 
thread_id()
{
  return trc_thread_self(); // (unsigned long)pthread_self();
}
#endif

void 
httpn_init_locks()
{
#ifdef HAVE_SSL
  int i;
  ssl_lockarray = (pthread_mutex_t *)OPENSSL_malloc(CRYPTO_num_locks() *
                                            sizeof(pthread_mutex_t));
  for (i = 0; i < CRYPTO_num_locks(); i++)
    pthread_mutex_init(&(ssl_lockarray[i]), NULL);
  CRYPTO_set_id_callback((unsigned long (*)())thread_id);
  CRYPTO_set_locking_callback((void (*)())lock_callback);
#endif
}

#if 0
static void rewrite_url_snet(char *url)
{
  char protocol[MAX_URL_SIZE];
  char rest[MAX_URL_SIZE];
  sscanf(url, "%[a-z]://%s", protocol, rest);
  if (strncasecmp(rest, "snet-", 5))
    sprintf(url, "%s://snet-%s", protocol, rest);
}
#endif

#if 0
static size_t xml_dispatch(void *ptr, size_t size, size_t nmemb, void *stream)
{
  xmlParseChunk((xmlParserCtxtPtr)stream, (char *)ptr, size * nmemb, 0);
  return size * nmemb;
}
#endif



/*
 ***************************************************************************************************
 * session pool management
 ***************************************************************************************************
 */

void
httpn_bind_pool(httpn_session_t *session, httpn_session_pool_t *pool)
{

	if (pool) {
		LB_REF(LB_GET(pool->lb_pool));
	}
	session->pool = pool;
}
void
httpn_unbind_pool(httpn_session_t *session)
{
	if (session->pool) {
		LB_UNREF(LB_GET(session->pool->lb_pool));
		session->pool = NULL;
	}
}

/*
 * allocate a new session 
 * In this allocation stage, we don't set expire time.
 * which will be set when pushing back to pool
 */
void
httpn_expire_session(void *d)
{
#if NOTUSED
	httpn_session_t*		session = (httpn_session_t *)d;
	cfs_origin_driver_t *	driver 	= session->driver;

	

	driver = session->driver;

	if (driver->shutdown) {
		
		return ;
	}
	/*
	 * 세션이 이전 version pool에서 할당됨.
	 * 그냥 삭제, timer는 이미 삭제됐을꺼임
	 */
	TRACE((T_PLUGIN, "session expired, destroying...\n"));
	httpn_cleanup_session(session);
	httpn_free_session(session);
#else
	TRAP;
#endif
	
}
int
httpn_trace_io(CURL *curl, curl_infotype itype, char *msg, size_t msglen, void *ud)
{
	httpn_io_context_t 	*iocb = (httpn_io_context_t *)ud;
	char 				*_t = NULL;
	int					tflag = 0;
	int 				status;

	
	//tflag = (iocb->verbose?T_WARN:0);


	switch (itype) {
		case CURLINFO_TEXT:
			if (TRACE_ON(T_PLUGIN)) {
				_t = alloca(msglen+4);
				memcpy(_t, msg, msglen);
				_t[msglen] = 0;
				
				/* CHG 2018-05-17 huibong TRACE '{}' add (#32089) */
				if ( msglen > 0 && _t[msglen - 1] == '\n' )
				{
					TRACE( ( tflag | T_PLUGIN, "Volume[%s].IOCB[%u].INFO :: %s", iocb->pool->key, iocb->id, _t ) );
				}
				else
				{
					TRACE( ( tflag | T_PLUGIN, "Volume[%s].IOCB[%u].INFO :: %s\n", iocb->pool->key, iocb->id, _t ) );
				}
			}
			break;
		case CURLINFO_HEADER_OUT:
			if (TRACE_ON(T_PLUGIN)) {
				_t = alloca(msglen+1);
				memcpy(_t, msg, msglen);
				_t[msglen] = 0;
				TRACE((tflag|T_PLUGIN, "Volume[%s].IOCB[%u].HEADER OUT :: %s", iocb->pool->key, iocb->id, _t));
			}
			httpn_migrate_http_state(iocb, HS_HEADER_OUT, __FILE__, __LINE__);
			httpn_mpx_timer_restart(iocb, __FILE__, __LINE__);
#ifdef HTTPN_MEASURE_IOCB
			PROFILE_CHECK(iocb->t_send);
#endif
			break;
		case CURLINFO_HEADER_IN:

			if (iocb->session->state >= HS_EOH) {
				return 0;
			}

			_t = alloca(msglen+1);
	
			memcpy(_t, msg, msglen);
			_t[msglen] = 0;
			TRACE((T_PLUGIN, "IOCB[%u].%s::: HEADER IN: |%s", iocb->id, httpn_zstate(iocb->session), _t));

			if (iocb->method == HTTP_POST) { 
				kv_oob_lock(iocb->in_property);
			}
			httpn_migrate_http_state(iocb, HS_HEADER_IN, __FILE__, __LINE__);
			httpn_mpx_timer_restart(iocb, __FILE__, __LINE__);
			TRACE((0, "Volume[%s].IOCB[%u].HEADER IN :: %s", iocb->pool->key, iocb->id, _t));

			if (msg[0] == 'H' && (sscanf(msg, "HTTP/1.1 %d", &status) == 1 ||
			     				  sscanf(msg, "HTTP/1.0 %d", &status) == 1)) {
#ifdef HTTPN_MEASURE_IOCB
				PROFILE_CHECK(iocb->t_fba);
#endif
				iocb->last_httpcode = status;

				if (iocb->driverX->follow_redirection && httpn_is_redirection(status)) {
					/*
					 * NULLize all callbacks
					 */
					TRACE((T_PLUGIN, "IOCB[%d] - redirection code '%d' found, skipping all incoming bytes\n", iocb->id, status));
					//httpn_reset_response(iocb);
					iocb->inredirection = 1;
				}
				else {
					/*
					 * restore all callbacks
					 */
					iocb->inredirection = 0;
				}
				// BUG 2019-04-22 huibong HTTP 301 응답시 서로 다른 content 로 인식하는 문제점 수정 (#32626) 
				//                - 연관일감 #32617 수정관련 응답코드 변경시 서로 다른 content 로 처리하도록 수정 
				//                - 하지만 양방향의 HTTP 301 rediection  후 206 응답 수신시 
				//                - http_drive.c 의 httpn_issamecode() 에 인자로 전달되는 newcode 값이 0 인 상황 발생
				//                - 이를 해결하기 위해 원부사장님이 아래 코드 추가함.
				iocb->last_httpcode = status;
				iocb->last_curle	= 0;

				if (iocb->method == HTTP_POST) { 
					/*
					 * POST의 경우 ASAP status code set하도록 함
					 */
					kv_set_raw_code(iocb->in_property, status);
					kv_oob_set_error(iocb->in_property, RBF_EOD); /* end of data */
				}
			}
			if (iocb->method == HTTP_POST) { 
				kv_oob_unlock(iocb->in_property);
			}
			break;
		default:
			/*
			 * to avaoid clang warning
			 * */
			break;
	}

	
	return 0;
}
void 
httpn_cleanup_session(httpn_session_t *session)
{
	session->state		= HS_INIT;
	session->stderrno 	= 0;
	session->pool_handle= 0;
	session->paused		= 0;
	session->key[0]		= 0;
	session->pool		= NULL;
}

/*
 * 세션 생성, pool등록 등
 */
httpn_session_t *
httpn_prepare_session(cfs_origin_driver_t *drv, httpn_io_context_t *iocb, httpn_session_pool_t *pool)
{
	httpn_session_t	*		session;

	session = (httpn_session_t *)XMALLOC_FL(sizeof(httpn_session_t), AI_DRIVER, __FILE__, __LINE__);
	session->eif	= curl_easy_init();
	session->driver = drv;
	DEBUG_ASSERT(session->eif != NULL);

	httpn_init_session(session, iocb);
	httpn_bind_pool(session, pool);
	httpn_migrate_http_state(iocb, HS_INIT, __FILE__, __LINE__);

	return session;
}
void
httpn_release_session(httpn_io_context_t *iocb, httpn_session_t *session)
{
	httpn_unbind_pool(session);
	httpn_cleanup_session(session);
	if (session->eif)  {
		curl_easy_cleanup(session->eif);
		session->eif = NULL;
	}
	XFREE(session);

}


int
httpn_setup_pool(httpn_session_pool_t *pool, const char *docroot, const char *id, const char *pass)
{
	int						result = 0;


	if (httpn_is_secure(docroot)) {
		TRACE((T_PLUGIN, "POOL[%s] - docroot[%s] secure\n", pool->key, docroot));
		pool->https = U_TRUE;
	}
	if (docroot) 
		pool->storage_url = XSTRDUP(docroot, AI_DRIVER);
	else
		pool->storage_url = XSTRDUP("", AI_DRIVER);

	/* BUG 2018-11-07 huibong 인증정보가 없는 경우에도 Authorization Header 가 생성됨 (#32424) 
	*                 - parent 구성시 conf 상에 origin 인증정보 설정 안해도.. 
	*                 - 자동으로 Authorization heaer 가 붙지만.. 실제 data 없어 인증 실패 발생
	*                 - 설정된 인증 정보가 없는 경우...Authorization header 가 붙지 않도록 수정. 
	*/

	// 본 함수는 httpn_prepare_pool() 에서만 호출되며  매개변수는 nc_origin_info_t 객체의 user[256], pass[256] 배열 address 이므로
	// 포인터인 id, pass 변수는 항상 유효, 이로 인해 실제 data 가 저장되어 있지 않아도 메모리 할당, data 복사가 발생하여
	// 빈 Authoriation 정보가 생성됨.
	// 이를 방지하기 위해  향후.. 다른 곳에서도 호출될 수 있으므로... 정석으로
	// id, pass 포인터가 유효하고.. id, pass 에 저장된 data 가 유효한 경우에만 새로운 메모리 할당 후 data 복사되도록 수정한다.
	if( id && strlen(id) > 0 )
	{
		pool->user = XSTRDUP(id, AI_DRIVER);
	}

	if( pass && strlen(pass) > 0 )
	{
		pool->pass =  XSTRDUP(pass, AI_DRIVER);
	}

	return result;
}
/*
 * just destroy memory block
 * removal from pool list should be done before calling this routine.
 */
void 
httpn_free_session(httpn_session_t *session)
{

	DEBUG_ASSERT(session->pool == NULL);

	if (session->eif)  {
		curl_easy_cleanup(session->eif);
		session->eif = NULL;
	}
	XFREE(session);
}
int
httpn_online(cfs_origin_driver_t *drv, int ioflag)
{
#if 0
	int			n = 0;
	httpn_driver_info_t 	*drvctx = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);

	if (drvctx && drvctx->LB[drvctx->active_lb])
		n =  lb_pool_count_online(drvctx->LB[drvctx->active_lb]);
	return n;
#else
	return drv->online;
#endif
}

int
httpn_mark_down_pool_x(cfs_origin_driver_t *drv, httpn_session_pool_t * sp, char *path, int force)
{

	int		r = 0;
	
	if (sp->lb_pool) {
		r = lb_pool_set_online(LB_GET(sp->lb_pool), sp->lb_pool, U_FALSE/* offline */, U_TRUE/*activate recovery*/, force); 
	}
	return r;
	
}

// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
// - origin 이 정상 상태임을 처리하기 위한 함수.
void httpn_mark_ok_pool_x( cfs_origin_driver_t * drv, httpn_session_pool_t * sp )
{

	// origin 이 이미 offline 상태인 경우.. alive check 를 수행해야 하므로.. 
	// 해당 origin 이 online 상태인 경우에만 호출 처리한다.
	if( sp && sp->lb_pool && sp->lb_pool->online == 1 )
	{
		lb_pool_set_online( LB_GET(sp->lb_pool), sp->lb_pool, U_TRUE/* offline */, U_FALSE/*activate recovery*/, U_FALSE /*force*/ );
	}
}

static size_t 
httpn_block_probe_reader(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	httpn_io_context_t	*iocb 			= (httpn_io_context_t *)userdata;
	iocb->premature = 1;
	return 0; /* 2020-09-04 by weon@solbox.com */
}
/*
 * LB에 의해서 호출됨
 * TODO : nc_ioctl에 의해서 active probing할지 결정하도록 되면
 * 다시 수정되어야함
 */
int
http_check_alive(lb_t *lb, lb_pool_t *pool, void *ud) 
{
	httpn_session_pool_t*	http_pool 	= (httpn_session_pool_t *)ud;;  
	cfs_origin_driver_t*	driver 		= http_pool->driver;
	httpn_io_context_t*		iocb 		= NULL;	
	httpn_driver_info_t*	drvctx 		= NULL;
	int 					r 			= 0;
	char					pu[256]		= "";
	CURLcode                response;

	char					byte_range[16];					// range GET 처리를 위한 변수
	long                    origin_response_code = -1;		// 실제 origin 응답 내역을 저장하기 위한 변수
	int                     force_up = 0;					// timeout 발생했지만.. origin 응답이 정상인 경우.. 강제 UP 처리 관련 flag
	int						tdtc = 0;
	nc_uint32_t				kept;


	cfs_lock_driver(driver, cfs_lock_shared);
	if (driver->shutdown) {
		cfs_unlock_driver(driver, cfs_lock_shared);
		return r;
	}

	DEBUG_ASSERT(driver->magic == NC_MAGIC_KEY);
	DEBUG_ASSERT(lb->magic == 0x5aa5a55a);

	drvctx = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(driver);

	strcpy(pu, http_pool->probe_url);

	iocb = httpn_create_iocb(http_pool->driver, (char *)pu, NULL, 0, NULL, U_TRUE, NULL, &kept, __FILE__, __LINE__);
	if (iocb == NULL) {
		/*
		 * iocb 생성 실패
		 * driver 종료중
		 */
		cfs_unlock_driver(driver, cfs_lock_shared);
		return r;
	}
	cfs_unlock_driver(driver, cfs_lock_shared);
	
	httpn_set_owner(iocb, HTTP_API_PROBE);
	httpn_update_operation(iocb, "GET");

	iocb->keptid 		= kept;
	iocb->mpxassoc 	  	= 0;
	iocb->pool 	  		= http_pool;
	iocb->tries++;
	iocb->session 		= httpn_prepare_session(driver, iocb, http_pool); 

	iocb->request 		= httpn_request_create(iocb, iocb->zmethod, NULL, NULL);
	iocb->request->url 	= httpn_setup_url(iocb->pool, http_pool->storage_url, iocb->wpath);

	httpn_bind_request_to_session(iocb->driver, iocb->session, iocb->request);

	// CHG 2019-05-03 huibong origin health check 방식 개선 (#32652, #32705)
	//                - 대용량 contnet인 경우 opeation timeout 따른 alive check 실패 개선 목적
	//                - connection timeout, transfer timeout 값을 서비스 설정값 적용하도록 수정
	SET_OPT( iocb->session->eif, CURLOPT_CONNECTTIMEOUT, drvctx->timeout );
	SET_OPT( iocb->session->eif, CURLOPT_TIMEOUT, drvctx->xfer_timeout );


	httpn_request_set_reader_callback(iocb->request, httpn_block_probe_reader, iocb);
	httpn_request_set_head_callback(iocb->request, NULL, NULL);

	iocb->last_curle 	= response = httpn_request_exec_single(iocb);
	iocb->last_errno	= httpn_handle_error(iocb, &iocb->last_curle, &iocb->last_httpcode, &tdtc);

	// origin 의 원래 reponse code 확인.
	curl_easy_getinfo( iocb->session->eif, CURLINFO_RESPONSE_CODE, &origin_response_code );

	// NEW 2019-05-03 huibong origin health check 정보를 origin.log 에 logging 처리 (#32651, #32705)
	//                - 문제 상황의 빠른 파악을 위한 운영 편의 제공 목적
	//                - iocb 의 session, request 객체 destory 수행 전에 호출하여 로깅 처리
	TRACE((0, "Volume[%s].URL[%s] - probing got %d\n", driver->signature, iocb->wpath,  iocb->last_curle));
	httpn_report_origin_log( iocb );
	TRACE((0, "Volume[%s].URL[%s] - reported OK\n", driver->signature, iocb->wpath,  iocb->last_curle));

	httpn_release_session(iocb, iocb->session);
	iocb->session = NULL;

	httpn_request_destroy(iocb->request); iocb->request = NULL;

	// down 상황이면 r 값은 0 , 정상 상태면 r값은 양의 정수
	r =  !httpn_forcely_down(drvctx, pu, iocb->last_curle, iocb->last_httpcode);

	// CHG 2019-05-03 huibong origin health check 방식 개선 (#32652, #32705)
	//                - 대용량 contnet인 경우 operation timeout(28) 따른 alive check 실패 개선 목적
	//                - range GET 미지원 등으로 operation timeout 발생시 (curle == 28)
	//                - 실제 origin 응답코드가 정상(100 이상 500 미만)이면 alive 상태로 처리
	if( r == 0 && iocb->last_curle == CURLE_OPERATION_TIMEDOUT )
	{
		if( origin_response_code < 500 && origin_response_code >= 100 )
		{
			r = 1;
			force_up = 1;

			TRACE( (T_INFO, "Volume[%s] alive check [%s][%s]=> curl error[%d: %s] op_timeout[%d], curl_http_response[%d]. but origin_response[%ld], forced to return up state\n"
				, driver->signature
				, http_pool->key
				, pu
				, iocb->last_curle
				, iocb->last_curle_str
				, drvctx->xfer_timeout
				, iocb->last_httpcode
				, origin_response_code) );
		}
	}

	// CHG 2019-03-25 huibong oringin 응답코드에 대해 운영자 설정에 따른 OFFLINE 처리 기능 개선 (#32565)
	//                - alive check 관련 로깅 level 및 내역 보강
	//                - alive check 진행 내역을 운영자가 확인하지 못해서... 로깅 처리하기로 함.
	// CHG 2019-05-03 huibong origin health check 방식 개선 (#32652, #32705)
	//                - 대용량 contnet인 경우 opeation timeout 따른 alive check 실패 개선 목적
	//                - 실제 origin 응답코드 및 강제로 up 처리 내역을 로깅하도록 추가
	TRACE( (T_INFO, "Volume[%s] probing pool [%s][%s]=> curl error[%d: %s], curl_http_response[%d], origin_response[%ld]=> return [%d: %s][%s]\n"
		, driver->signature
		, http_pool->key
		, pu
		, iocb->last_curle
		, iocb->last_curle_str
		, iocb->last_httpcode
		, origin_response_code
		, r, (r == 0 ? "down" : "up")
		, (force_up == 1 ? "force up" : "-")) );

	httpn_destroy_iocb( iocb, U_TRUE, __FILE__, __LINE__ );

	return r;
}


/*
 * 이 함수는 데이타 전송 중에는 빠르게 반복 호출됨
 * 데이타 전송이 느려지면 이 함수 호출빈도는 1초에 1번 횟수까지로 
 * 느려짐
 */
static int
httpn_session_monitor(void *clp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	httpn_session_t	*		session = (httpn_session_t *)clp;
	cfs_origin_driver_t	*	driver 	= (cfs_origin_driver_t *)session->driver;
	int						needabort = 0;
	httpn_io_context_t	*	iocb = NULL;
	int						volume_abort = 0;
	int						nc_abort = __terminating;
	char					ziocb[4096];
	char					zcurl[256];



	XVERIFY(session);


	curl_easy_getinfo(session->eif, CURLINFO_PRIVATE, (char **)&iocb);

	if (iocb->iotimeout != 0 && iocb->iotimeout <= sys_gettickcount()) {
		TRACE((T_PLUGIN, "IOCB[%d] - iotimeout[%lld] <= sys_gettickcount[%lld]\n", iocb->id, iocb->iotimeout,  sys_gettickcount()));
		iocb->timedout = 1;
	}
#if 0
	/*
	 * LB shutdown때에도 abort 되도록함
	 * 아니면 느린 read가 진행중이면 오래 걸림
	 */
	volume_abort = driver->shutdown ||  lb_shutdown_signalled(LB_GET(session->pool->lb_pool));
#else
	// driver shutdown(volume destroy때)만 
	volume_abort = driver->shutdown; 
#endif
	needabort = (volume_abort || nc_abort|| (iocb->vector && iocb->vector->canceled));

	if (iocb && iocb->post_avail && httpn_mpx_state_is(iocb, IOCB_MPX_PAUSED)) {
		TRACE((T_PLUGIN, "IOCB[%u] - resuming\n", iocb->id));
		httpn_mpx_resume_iocb_self(iocb);
	}



	if (needabort || iocb->timedout) {
		//TRACE((0, 	"IOCB[%ld] Volume[%s] session %p : UP: %" CURL_FORMAT_CURL_OFF_T " of %" CURL_FORMAT_CURL_OFF_T " "
	    //     		"DOWN: %" CURL_FORMAT_CURL_OFF_T " of %" CURL_FORMAT_CURL_OFF_T
		//	        "\n",
		//			iocb->id, 
		//			driver->signature,
		//			session,
		//			ulnow, ultotal, dlnow, dltotal));
		if (TRACE_ON(T_PLUGIN)) {
			httpn_dump_iocb_s(ziocb, sizeof(ziocb), iocb);
			httpn_dump_info(zcurl, sizeof(zcurl), session->eif);
			TRACE((T_PLUGIN, "Volume[%s].%s(%s) - IOCB[%d] got stop signal(volume_abort=%d, nc_abort=%d, cancel=%d, timeout=%d)\n"
							"\t\t+ IOCB  : %s\n"
							"\t\t+ CURL  : %s\n",
							driver->signature, 
							iocb->zmethod,
							iocb->wpath,
							iocb->id,
							volume_abort,
							nc_abort,
							(iocb->vector && iocb->vector->canceled),
							iocb->timedout,
							ziocb,
							zcurl
							));
		}
		if (iocb->timedout) {
			/*
			 * timeout set by timer routine
			 */
			iocb->last_httpcode =  HTTP_GATEWAY_TIMEOUT;
		}
		if (needabort) {
			iocb->canceled = 1;
		}
		return -1; /* remained processinged left for mpx_handle_completed() */
	}
	return 0;
}
static int
httpn_session_monitor_old(void *clp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	return httpn_session_monitor(clp,
						 			(curl_off_t)dltotal,
						 			(curl_off_t)dlnow,
						 			(curl_off_t)ultotal,
						 			(curl_off_t)ulnow
								);
}

void
httpn_init_session(httpn_session_t *session, httpn_io_context_t *iocb)
{
	cfs_origin_driver_t	*		driver;
	httpn_driver_info_t * 		drvctx;

	driver = (cfs_origin_driver_t *)session->driver;
	drvctx = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(driver);

	SET_OPT(session->eif, CURLOPT_DEBUGDATA, 		(void *)session);
	SET_OPT(session->eif, CURLOPT_VERBOSE, 			3);
	SET_OPT(session->eif, CURLOPT_HEADER, 			0);
	SET_OPT(session->eif, CURLOPT_NOPROGRESS, 		(long)0);
	SET_OPT(session->eif, CURLOPT_CONNECTTIMEOUT, 	drvctx->timeout);
	SET_OPT(session->eif, CURLOPT_COOKIELIST, 		"ALL");
    SET_OPT(session->eif, CURLOPT_MAXREDIRS, 		5L);

	// NEW 2020-01-22 huibong DNS cache timeout 설정 기능 추가 (#32867)
	//                - curl 에 dns cache timeout 값 설정
	SET_OPT( session->eif, CURLOPT_DNS_CACHE_TIMEOUT, drvctx->dns_cache_timeout );

    SET_OPT(session->eif, CURLOPT_PROGRESSFUNCTION, httpn_session_monitor_old);
    SET_OPT(session->eif, CURLOPT_PROGRESSDATA, 	session);

	SET_OPT(session->eif, CURLOPT_DEBUGFUNCTION, httpn_trace_io);
	SET_OPT(session->eif, CURLOPT_DEBUGDATA, 	(void *)iocb);
	SET_OPT(session->eif, CURLOPT_PRIVATE, iocb );
	//
	// TCP KEEP-ALIVE 관련 설정
	//		20초 동안 주고받는 데이타가 없으면 이후 5초마다 체크
	//		횟수는 지정없음
	//
	SET_OPT(session->eif, CURLOPT_TCP_KEEPALIVE, 1L );
	SET_OPT(session->eif, CURLOPT_TCP_KEEPIDLE, 20L );
	SET_OPT(session->eif, CURLOPT_TCP_KEEPINTVL, 5L );

	//long						sp_time;
	//long						sp_limit;
	//sp_time		=  5;
	//sp_limit	= 10000; /* 10KBps */
    //SET_OPT(session->eif, CURLOPT_LOW_SPEED_TIME, 	sp_time);
    //SET_OPT(session->eif, CURLOPT_LOW_SPEED_LIMIT, 	sp_limit);


	SET_OPT(session->eif, CURLOPT_ERRORBUFFER, iocb->last_curle_str );

#if LIBCURL_VERSION_NUM >= 0x072000
    SET_OPT(session->eif, CURLOPT_XFERINFOFUNCTION, httpn_session_monitor);
    SET_OPT(session->eif, CURLOPT_XFERINFODATA, 	session);
#endif

	// CHG 2019-04-24 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	//                - receive buffer size 를 16kB * 2 = 32kB 로 설정
	//                - default: CURL_MAX_WRITE_SIZE (16kB), max: CURL_MAX_READ_SIZE (512kB) 
	SET_OPT(session->eif, CURLOPT_BUFFERSIZE, NC_BLOCK_SIZE);

    SET_OPT(session->eif, CURLOPT_NOSIGNAL, 1);
}
/*
 * ir# 26831
 * 
 *	active_lb 가 parent(1)인 상태에서 OFFLINE 이면 origin(0) 에 대한 try
 *  active_lb 가 origin(0) 이면, 호출 시 마다 parent(1)번이 살았는지 확인 후, parent(1)이 ONLINE이면 parent(1)으로 자동절체
 */
httpn_session_pool_t *
httpn_next_pool(cfs_origin_driver_t *driver, const char *hint, int makeref)
{
	httpn_driver_info_t*	driverX 	= (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(driver);
	nc_pool_handle_t		ph 		= LB_INVALID_HANDLE;
	httpn_session_pool_t*	hsp 	= NULL;
	int 					ac 		= 0;
	int 					total;
	lb_t					*alb	= NULL;

	/* CHG 2018-10-30 huibong parent 존재하나.. parent로 요청이 전송되지 않는 코드 수정 (#32410) 
	*                         - parent 가 구성되었으나.. active_lb 가 무조건 0 으로 Origin 만 가르켜.. parent 사용이 안됨.
	*                         - active_lb 가 parent, origin 상태 및 업무 flow 따라 선택되로록 수정 처리.
	*                         - 불필요한 코드 제거
	*/

	_nl_lock(&driverX->lock, __FILE__, __LINE__);


	// origin 만 있으면.. total 값은 1, parent 까지 구성된 경우.. total 값은 2.
	total = 1;
	// reload 등을 통해 기존 존재하던 parent 정보가 삭제될 수 있어 매번 check...
	total += ( driverX->LB[NC_CT_PARENT] ? 1 : 0 );

	// 마지막 active LB
	ac = driverX->active_lb;

	if( total > 1 )	// 현재 parent 가 존재하는 경우.
	{
		if( ac == NC_CT_PARENT )	// 마지막 active lb 가 parent 인 경우..
		{
			// parent 상태 check.
			if( lb_pool_count_online(driverX->LB[NC_CT_PARENT]) <= 0 )	// parent 가 모두 죽은 경우..
			{
				if( lb_pool_count_online(driverX->LB[NC_CT_ORIGIN]) > 0 )
				{
					// parent 가 모두 죽고... Origin 은 유효한 경우...
					// - origin 으로 변경 처리...
					ac = NC_CT_ORIGIN;
					driverX->active_lb = ac;

					TRACE((T_INFO, "VOLUME[%s], LB parent->origin changed by parent all dead. active_lb=%d\n", driver->signature, driverX->active_lb));
				}
				else
				{
					// parent 죽고.. origin 까지 모두 죽은 경우...
					// - 모두 죽은 경우...  parent 가 우선 순위가 가장 높으므로.. parent 로 시도. 
					TRACE((T_INFO, "VOLUME[%s], parent & origin all dead. LB default parent set. active_lb=%d\n", driver->signature, driverX->active_lb));
				}
			}
			else
			{
				// 마지막 active LB 가 parent 인데.. parent 가 alive 상태인 경우..
				// - 기존 값 변경 없이 그대로 사용.
			}
		}
		else	// 마지막 active LB 가 origin 인데.. parent 가 존재하는 경우...
		{
			if( lb_pool_count_online(driverX->LB[NC_CT_PARENT]) > 0 )
			{
				// parent 살았는지 check 하여 정상이 parent 존재하는 경우..
				// - parent 사용하도록 변경 처리
				ac = NC_CT_PARENT;
				driverX->active_lb = ac;

				TRACE((T_INFO, "VOLUME[%s], LB origin->parent changed by parent alive ok. active_lb=%d\n", 
								driver->signature, 
								driverX->active_lb));
			}
			else
			{
				// 정상인 parent 가 없는 경우..
				// - 기존에 설정된 active LB = origin 값을 그냥 사용하도록...
			}
		}
	}
	else	// 현재 origin 만 존재하는 경우..
	{
		// parent 가 존재하다가.. reload 되면서 없어질 수 있으므로.. check

		if( ac == NC_CT_PARENT )	// 마지막 active lb 가 parent 인 경우 origin 으로 변경 처리
		{
			ac = NC_CT_ORIGIN;
			driverX->active_lb = ac;
					
			TRACE((T_WARN, "VOLUME[%s], LB parent->origin changed by parent deleted. active_lb=%d\n", driver->signature, driverX->active_lb));
		}
		else
		{
			// 마지막 active LB 가 origin 인 경우..
			// - origin 만 존재하고.. 기존 값도 origin 이므로.. 기존값 그냥 사용.
		}
	}

	alb = driverX->LB[ac];
	ph = lb_get_policy_pool(alb, hint);
	TRACE((T_PLUGIN, "VOLUME[%s], total_pool=%d, driver->active_lb=%d, ac=%d, pool_handle=0x%llx\n", driver->signature, total, driverX->active_lb, ac, ph));

	if (ph != LB_INVALID_HANDLE) {
		hsp 	= (httpn_session_pool_t *)lb_pool_userdata(alb, ph);
		hsp->pool_handle = ph;
	}
#if 0
	else
	{
		// session 획득 실패시 .. 로깅
		TRACE((T_ERROR, "VOLUME[%s], session get(lb_get_policy_pool) failed. ac=%d, hint=%s \n", driver->signature, ac, hint));
	}
#endif
	if (makeref && hsp) {
		LB_REF(LB_GET(hsp->lb_pool));
	}
	_nl_unlock(&driverX->lock);



	return hsp;
}

/*
 *
 * HTTP session state management
 */

void
httpn_migrate_http_state(httpn_io_context_t *iocb, httpn_pin_state_t ps, char *f, int l)
{

	if (iocb->session && iocb->session->state != ps) {
		TRACE((T_PLUGIN, "IOCB[%lu].STD:[%s] --> [%s] : %d@%s\n", iocb->id, _zs[iocb->session->state], _zs[ps], l, f));
		iocb->session->state = ps;
	}
}
int
httpn_is_state(httpn_session_t *s, httpn_pin_state_t state)
{
	return (s->state == state);
}
int
httpn_state(httpn_session_t *s)
{
	return s->state;
}


int
httpn_bind_request_to_session(cfs_origin_driver_t *drv, httpn_session_t *session, httpn_request_t *request)
{
	//int 						secure 		= 0;
	httpn_driver_info_t	*		http_instance = NULL;
	client_info_t	*			client 		= NULL;
	httpn_session_pool_t*		pool 		= NULL;
#ifdef PHASE_HANDLER_DEFINED
	nc_origin_io_command_t 	*	req_cmd 	= NULL;
#endif
	char					* 	up 			= NULL;


	http_instance 	= (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(drv);
	pool 			= session->pool;





	if (pool->user) {
		asprintf(&up, "%s:%s", pool->user, (pool->pass?pool->pass:""));
		SET_OPT(session->eif, CURLOPT_USERPWD, up);
		free(up);
	}


	if (http_instance->proxy) {
		SET_OPT(session->eif, CURLOPT_PROXY, 		http_instance->proxy);
		SET_OPT(session->eif, CURLOPT_PROXYPORT, 	http_instance->proxy_port);
		SET_OPT(session->eif, CURLOPT_PROXYTYPE, 	CURLPROXY_HTTP);
		httpn_request_add_header(request, "Proxy-Connection", "", U_FALSE);
	}

#ifdef PHASE_HANDLER_DEFINED
	/*
	 *
	 * request를 전송하기 전에 phase handler가 정의되어있다면 호출
	 *
	 */
	if (http_instance->on_send_request_proc) {
		/*
		 * command 생성
		 * 주의 : 이때 표준 malloc API를 통해서 할당한다
		 */
		req_cmd = (nc_origin_io_command_t *)alloca(sizeof(nc_origin_io_command_t)); /* free할 필요없음*/
		req_cmd->method		=  request->zmethod; /* just point to the string */
		req_cmd->url 		= request->url;
		req_cmd->properties = request->inject_property;
		req_cmd->cbdata 	= http_instance->on_receive_response_cbdata;
		(*http_instance->on_send_request_proc)(NC_ORIGIN_PHASE_REQUEST, req_cmd);
		if (request->url != req_cmd->url) {
			/* URL이 메모리 포인터가 변경되었음 */
			XFREE(request->url);
			request->url = XSTRDUP(req_cmd->url, AI_DRIVER);
			free(req_cmd->url); /* raw malloc/free 제거 */
		}
	}
	client = (client_info_t *)kv_get_client(request->inject_property);

	if(client != NULL && client->ip[0] != 0) {	//TPROXY 동작을 위한 부분
		 SET_OPT(session->eif, CURLOPT_SOCKOPTFUNCTION, httpn_setup_curl_sock);
		 SET_OPT(session->eif, CURLOPT_INTERFACE, client->ip);
		 TRACE((T_PLUGIN, "Connection type : TPROXY\n"));
	}
#if 0
	if (secure) {
		SET_OPT(session->eif, CURLOPT_SSL_VERIFYPEER, 0);
		SET_OPT(session->eif, CURLOPT_SSL_VERIFYHOST, 0);
	}
#endif
    SET_OPT(session->eif, CURLOPT_VERBOSE, 3);

	if (http_instance->follow_redirection) {
		SET_OPT(session->eif, CURLOPT_FOLLOWLOCATION, 1);

		// NEW 2020-01-20 huibong Redirection 무한 반복을 방지하기 위해 Redirection 최대 횟수 설정 (#32861)
		//                - redirection 을 허용하는 경우.. 허용가능 최대횟수 지정 처리
		SET_OPT( session->eif, CURLOPT_MAXREDIRS, ALLOW_MAX_REDIRECTION_COUNT );

		/*
		 *
		 * IR#32131
		 * FOLLOW_REDIRECTION 설정이 있고, user/pass 설정이 있는 경우에만 아래 옵션 적용
		 *
		 */
		if (pool->user) 
			SET_OPT(session->eif, CURLOPT_UNRESTRICTED_AUTH, 1);
	}


	switch (request->method) {
		case HTTP_HEAD:
			SET_OPT(session->eif, CURLOPT_FILETIME, 1);
			SET_OPT(session->eif, CURLOPT_NOBODY, 1);
			SET_OPT(session->eif, CURLOPT_HEADER, 0);
			SET_OPT(session->eif, CURLOPT_HEADERFUNCTION, request->head_proc);
			SET_OPT(session->eif, CURLOPT_HEADERDATA, request->head_cb);
			break;
		case HTTP_MKDIR:
			SET_OPT(session->eif, CURLOPT_UPLOAD, 1);
			SET_OPT(session->eif, CURLOPT_INFILESIZE, 0);
			break;
		case HTTP_PUT:
			SET_OPT(session->eif, CURLOPT_UPLOAD, 1);
			if (request->datalen > 0)
				SET_OPT(session->eif, CURLOPT_INFILESIZE_LARGE, (curl_off_t)request->datalen);
			if (request->write_proc) {
				SET_OPT(session->eif, CURLOPT_READDATA, request->write_cb);
				SET_OPT(session->eif, CURLOPT_READFUNCTION, request->write_proc);
			}
			if (request->head_proc) {
				SET_OPT(session->eif, CURLOPT_HEADERFUNCTION, request->head_proc);
				SET_OPT(session->eif, CURLOPT_HEADERDATA, request->head_cb);
			}
			SET_OPT(session->eif, CURLOPT_TIMEOUT, 0);
			break;
		case HTTP_GET:
			if (request->head_proc) {
				SET_OPT(session->eif, CURLOPT_HEADERFUNCTION, request->head_proc);
				SET_OPT(session->eif, CURLOPT_HEADERDATA, request->head_cb);
			}

			/* 
			 * 2013.5.7
			 * FOLLOW 옵션 제거, 사용하는 경우 redirection을 하면서 property
			 *들이 이중으로 중첩 누적되는 현상을 피할 수 없음
			 */
			SET_OPT(session->eif, CURLOPT_WRITEDATA, request->read_cb);
			SET_OPT(session->eif, CURLOPT_WRITEFUNCTION, request->read_proc);
			break;
		case HTTP_POST:
			SET_OPT(session->eif, CURLOPT_POST, 1L);

			if (request->head_proc) {
				SET_OPT(session->eif, CURLOPT_HEADERFUNCTION, request->head_proc);
				SET_OPT(session->eif, CURLOPT_HEADERDATA, request->head_cb);
			}
			/*
			 * POST의 결과로 수신할 데이타가 있을 때 설정
			 */
			if (request->read_proc) {
				SET_OPT(session->eif, CURLOPT_WRITEDATA, request->read_cb);
				SET_OPT(session->eif, CURLOPT_WRITEFUNCTION, request->read_proc);
			}

			/*
			 * 서버로 전송할 데이타가 있을 때 설정됨
			 */
			if (request->write_proc) {
				SET_OPT(session->eif, CURLOPT_READDATA, request->write_cb);
				SET_OPT(session->eif, CURLOPT_READFUNCTION, request->write_proc);
			}

			break;
		case HTTP_DELETE:
			SET_OPT(session->eif, CURLOPT_CUSTOMREQUEST, "DELETE");
			break;
		case HTTP_CUSTOM:
			SET_OPT(session->eif, CURLOPT_CUSTOMREQUEST, request->zmethod);
			if (request->head_proc) {
				SET_OPT(session->eif, CURLOPT_HEADERFUNCTION, request->head_proc);
				SET_OPT(session->eif, CURLOPT_HEADERDATA, request->head_cb);
			}

			SET_OPT(session->eif, CURLOPT_WRITEDATA, request->read_cb);
			SET_OPT(session->eif, CURLOPT_WRITEFUNCTION, request->read_proc);
			break;
		default:
			/* ERROR */
			break;
	}

	//
	// CHG 2019-03-27 huibong solproxy에서 URL 경로 임의 변경시 반영안되는 문제점 수정 (#32574)
	//                - URL 정보 설정을 solproxy callback 함수 호출 이후에 이루어지도록 순서 변경	
	SET_OPT(session->eif, CURLOPT_URL, request->url);
	SET_OPT(session->eif, CURLOPT_BUFFERSIZE, NC_BLOCK_SIZE);

	/* 2018-08-20 huibong Origin Request callback 함수의 추가된 request header 반영 오류 수정 (#32301) */
	/* on_send_request_proc() 호출 이후 heaer commit -> SET_OPT 호출되도록 순서 수정 */
	httpn_commit_headers(request);
	SET_OPT(session->eif, CURLOPT_HTTPHEADER, request->headers);

	return 0;
}

void 
httpn_mpx_state_set(httpn_io_context_t *iocb, httpn_mpx_state_t mpxs, char *f, int l)
{
#ifndef NC_RELEASE_BUILD
	int		tflg = 0;
	char	buf[2048];
	switch (mpxs) {
		case IOCB_MPX_TRY_RUN:
			if (iocb->state == IOCB_MPX_PAUSED) {
				/* PAUSED -> RUN */
				_ATOMIC_SUB(__cnt_paused, 1);
				_ATOMIC_ADD(__cnt_running, 1);
			}
			else if (iocb->state == IOCB_MPX_READY) {
				/* READY -> RUN */
				_ATOMIC_ADD(__cnt_running, 1);
			}
			else {
				TRACE((T_ERROR, "state error:%s at %d@%s\n", httpn_dump_iocb(buf, sizeof(buf), iocb), l, f));
				TRAP;

			}
			break;
		case IOCB_MPX_PAUSED:
			if (iocb->state == IOCB_MPX_TRY_RUN) {
				/* RUN->PAUSED */
				_ATOMIC_ADD(__cnt_paused, 1);
				_ATOMIC_SUB(__cnt_running, 1);
			}
			break;
		case IOCB_MPX_TRY_DONE:	
			if (iocb->state == IOCB_MPX_TRY_RUN) {
				/* RUN -> DONE */
				_ATOMIC_SUB(__cnt_running, 1);
			}
			else if (iocb->state == IOCB_MPX_PAUSED) {
				/* PAUSED -> DONE */
				_ATOMIC_SUB(__cnt_paused, 1);
			}

			break;
		case IOCB_MPX_FINISHED:
			break;
		default: /* do nothing*/
			break;

	}
	TRACE((tflg|T_PLUGIN, "IOCB[%u](kept IOCB[%d]) (%ld,%ld,%ld) - state [%s] -> [%s](ASIO[%d]) at %d@%s\n", 
						iocb->id, 
						iocb->keptid, 
						GET_ATOMIC_VAL(__cnt_running),
						GET_ATOMIC_VAL(__cnt_paused),
						GET_ATOMIC_VAL(g__iocb_count),
						_zmpxstate[iocb->state],
						_zmpxstate[mpxs],
						(iocb->vector?iocb->vector->iov_id:-1),
						l, 
						f
						));
#else
	TRACE((T_PLUGIN, "IOCB[%u](kept IOCB[%d]) - state [%s] -> [%s](ASIO[%d]) at %d@%s\n", 
						iocb->id, 
						iocb->keptid, 
						_zmpxstate[iocb->state],
						_zmpxstate[mpxs],
						(iocb->vector?iocb->vector->iov_id:-1),
						l, 
						f
						));
#endif
	iocb->state = mpxs;
}

int
httpn_mpx_state_is(httpn_io_context_t *iocb, httpn_mpx_state_t mpxs)
{
	return (iocb->state == mpxs);
}

httpn_mpx_state_t 
httpn_mpx_state(httpn_io_context_t *iocb)
{
	return iocb->state;
}
#endif

