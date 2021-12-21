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
#include <ctype.h>
#include <curl/curl.h>
#include <curl/easy.h>


#include <netcache.h>
#include <block.h>
#include <tlc_queue.h>
#include "util.h"
#include "disk_io.h"
#include "trace.h"
#include "hash.h"
#include "httpn_driver.h"
#include "httpn_request.h"
#include "http_codes.h"

extern char	__zowner[][32]; 
extern char	_zmpxstate[][32];
extern void *__timer_wheel_base;

#ifndef NC_RELEASE_BUILD
extern long __cnt_paused;
extern long __cnt_running;
#endif
long		g__iocb_count = 0;

int			g__iocb_cleaner_shutdown = 0;
tlc_queue_t	g__iocb_free_queue;


static link_list_t 		s__dbg_iocb_list = LIST_INITIALIZER;
static pthread_mutex_t 	s__iocb_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static link_list_t 		s__iocb_pool 	 	= LIST_INITIALIZER;


httpn_io_context_t *
httpn_create_iocb(	cfs_origin_driver_t *drv, 
					const char 			*path, 
					nc_kv_list_t 		*inprop, 
					nc_mode_t 			mode, 
					nc_asio_vector_t 	*v, 
					int 				autoctx, 
					nc_open_cc_t		*occ,
					nc_uint32_t			*fsignature, 
					char 				*file,
					int 				line
					)
{
	httpn_io_context_t 	*iocb 				= NULL;
	int					use_subrange 		= U_TRUE;
	char 				*cc_method 			= NULL;
	int					kept = 0;
	char				*ptr = NULL;


	cfs_lock_driver(drv, cfs_lock_shared);
	if (drv->shutdown) {

		cfs_unlock_driver(drv, cfs_lock_shared);
		return NULL;
	}

	pthread_mutex_lock(&s__iocb_pool_lock);
	iocb				= (httpn_io_context_t *)link_get_head(&s__iocb_pool);
	pthread_mutex_unlock(&s__iocb_pool_lock);

	if (!iocb) {
		iocb 			= (httpn_io_context_t *)XMALLOC(sizeof(httpn_io_context_t), AI_DRIVER);
		iocb->cmd_id 	= HMC_FREED;
		_nl_cond_init(&iocb->cmd_signal);

	}
	else {
		kept = iocb->keptid;
		DEBUG_ASSERT(iocb->cmd_id == HMC_FREED);
		memset((char *)&iocb->confver, 0, sizeof(httpn_io_context_t) - sizeof(struct tag_httpn_io_context_exc)); 
		/* memset이 제대로 실행되었다면...*/
		DEBUG_ASSERT_IOCB(iocb, iocb->keptid == 0);
		iocb->keptid = kept;
	}

	DEBUG_ASSERT(CFS_DRIVER_VALID(drv));

	CFS_DRIVER_REF(drv);
	cfs_unlock_driver(drv, cfs_lock_shared);

	iocb->cmd_id		= HMC_NONE;
	iocb->id 			= httpn_task_id();



//#pragma message(REMIND("DEBUG"))
	//iocb->tflg = T_WARN;

	//TRACE((T_ERROR, "IOCB[%d] - tried at %d@%s(kept=%d)\n", iocb->id, line, file, kept));

#ifdef DEBUG_IOCB
	iocb->csource		= file;
	iocb->cline			= line;
#endif




	iocb->magic			= IOCB_VALID_MAGIC;

	if (fsignature)
		*fsignature = iocb->id;

	iocb->knownsize 	= -1;
	iocb->driver		= drv;
	iocb->driverX		= drv->driver_data;
	//
	// IOCB가 현재 생성시 사용된 driver confver를 기록
	// 이후 이 cofver를 이용해서 LB등 접근시
	// 참조 lb_set_online()등의 호출은 회피함(왜냐하면 memory pointer등이 변경되었을 수 있음)
	//
	iocb->confver		= ((httpn_driver_info_t *)drv->driver_data)->confver;
	iocb->cachedhttpcode= 0;
	iocb->imson 		= 0;
	iocb->ctime 		= nc_cached_clock();
	iocb->mpxassoc 		= 1;
	iocb->state 		= IOCB_MPX_INIT;
	iocb->verbose 		= (drv->trace?U_TRUE:U_FALSE);

	if (httpn_make_fullpath(iocb->driverX, &iocb->wpath, path, U_FALSE) < 0) {
		TRACE((T_ERROR, "path(%s) - could not re-encode to one in storage charset\n", path));
		XFREE(iocb);
		return NULL;
	}

	if (occ && (cc_method = (char *)nc_lookup_cc(occ, NC_OCC_OPERATION)) ) {
		iocb->method = httpn_map_method(cc_method);
		strcpy(iocb->zmethod, cc_method);
	}
/*
 * 호환성 관련 코드
 */
	else if (inprop && inprop->opaque_command) {
		iocb->method = httpn_map_method(inprop->opaque_command);
		strcpy(iocb->zmethod, inprop->opaque_command);
	}
	else {
		/*
		 * fill with default operation
		 */
		iocb->method = httpn_map_method("GET");
		strcpy(iocb->zmethod, "GET");
	}
	if (iocb->method == HTTP_POST) {
		kv_oob_set_notifier(inprop, httpn_post_notification_callback, iocb);
	}
	if (v)
		httpn_bind_vector(iocb, v);
	iocb->in_property 	= inprop;
	iocb->mode 			= mode;
	iocb->autocreat 	= autoctx;
	iocb->received		= 0;
	iocb->accepted		= 0;


	


	if (use_subrange && iocb->driverX->useheadforattr) 
		use_subrange = 0;
	if (use_subrange && (
							IS_ON(mode, O_NCX_NORANDOM) ||
							IS_ON(mode, O_NCX_TRY_NORANDOM) 
						)) { 
		TRACE((T_PLUGIN, "IOCB[%d] - request mode NORANDOME(%d)|TRY_NORANDOM(%d), assume_rangeable went to 0\n", 
							iocb->id, 
							IS_ON(mode, O_NCX_NORANDOM),
							IS_ON(mode, O_NCX_TRY_NORANDOM)
							));
		use_subrange = 0;
	}
	if (use_subrange && inprop && (ptr = kv_find_val(inprop, "Accept-Encoding", U_FALSE))) {
		TRACE((T_PLUGIN, "IOCB[%d] - Property Accept-Encoding, %s found, assume_rangeable went to 0\n", iocb->id, ptr));
		use_subrange = 0;
	}
	/*
	 * 원본에 응답을 받기전에 요청에 포함돈 정보를 기반으로 subrange GET 가능여부 유추
	 */
	iocb->assume_rangeable 	= (use_subrange != 0);
	if (iocb->assume_rangeable == 0) {
#if 1
		/*
		 * default readinfo 
		 */
		if (IS_ON(mode, O_NCX_TRY_NORANDOM))
			httpn_set_readinfo(iocb, 0, iocb->driverX->max_read_size);
		else
			httpn_set_readinfo(iocb, 0, -1);
#else
		httpn_set_readinfo(iocb, 0, -1);
#endif
	}

	iocb->task 			= NULL;
#if 0
	iocb->event 		= apc_overlapped_new(NULL, NULL);
#endif
	iocb->target_action = HS_INIT;
	INIT_NODE(&iocb->node);
	SET_NODE(&iocb->node, NULL);
	if (iocb->autocreat == 0) {
		TRACE((T_PLUGIN, 	"Volume[%s].URL[%s]/mode[0x%08X] - IOCB[%u] allocated\n",
						drv->signature,
						path, 
						mode, 
						iocb->id
						));

	}
	DEBUG_ASSERT(_ATOMIC_ADD(g__iocb_count, 1)>=0);

	DEBUG_ASSERT_IOCB(iocb, httpn_mpx_state_is(iocb, IOCB_MPX_INIT));

#ifdef DEBUG_IOCB
	pthread_mutex_lock(&s__iocb_pool_lock);
	link_append(&s__dbg_iocb_list, iocb, &iocb->dbg_node);
	pthread_mutex_unlock(&s__iocb_pool_lock);
#endif



#ifdef	HTTPN_MEASURE_IOCB
	PROFILE_CHECK(iocb->t_create);
	iocb->state 	= IOCB_MPX_INIT;
	iocb->t_send 	= iocb->t_create;
	iocb->t_fba 	= iocb->t_create;
	iocb->t_hdrend 	= iocb->t_create;
	iocb->t_pause 	= iocb->t_create;
	iocb->t_resume 	= iocb->t_create;
	iocb->t_done 	= iocb->t_create;
#endif

#if 0
	{
	char	ziocb[4096];
	TRACE((T_WARN, "IOCB[%u] - allocated;%s\n", iocb->id, httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)));
	}
#endif
	return iocb;
}
void
httpn_set_private(httpn_io_context_t *iocb, void *u)
{
	iocb->private = u;
}
void
httpn_set_owner(httpn_io_context_t *iocb, httpn_api_t api)
{
	iocb->owner = api;
	TRACE((T_PLUGIN, "IOCB[%d] - set owner[%s]\n", iocb->id, __zowner[api]));
	
}
void
httpn_set_task(httpn_io_context_t *iocb, httpn_task_proc_t proc)
{
	iocb->task = proc;
}

void
httpn_bind_request(httpn_io_context_t *iocb, httpn_request_t *req) {
	iocb->request = req;
}
int
httpn_update_operation(httpn_io_context_t *iocb, char *method)
{
	iocb->method = httpn_map_method(method);
	strcpy(iocb->zmethod, method);
	return 0;
}

int
httpn_bind_vector(httpn_io_context_t *iocb, nc_asio_vector_t *v)
{
	extern char _str_iot[][24]; /* 주의 libnc.so에 있는 광역변수*/
	iocb->vector = NULL;
	if (v) {
		iocb->vector 	= v;
		iocb->inode		= v->iov_inode;
		v->priv			= iocb; /* for tracing */
		if (iocb->inode)
			iocb->volume 	= iocb->inode->volume;
		TRACE((T_PLUGIN, "IOCB[%u] - bound to INODE[%u].ASIO[%ld].%s blk#[%u:%u]\n",
						iocb->id,
						iocb->inode->uid,
						iocb->vector->iov_id,
						_str_iot[v->iot[0]],
						((iocb->vector->iov_cnt > 0)?ASIO_BLOCK(iocb->vector, 0)->blkno:-1),
						((iocb->vector->iov_cnt > 0)?ASIO_BLOCK(iocb->vector, (iocb->vector->iov_cnt-1))->blkno:-1)
						));
						
	}
	return 0;	
}
void
httpn_get_stat(int 	stat[])
{
#ifdef DEBUG_IOCB
	httpn_io_context_t 	*iocb;

	pthread_mutex_lock(&s__iocb_pool_lock);
	iocb = (httpn_io_context_t *) link_get_head_noremove(&s__dbg_iocb_list);

	while (iocb) {
		stat[iocb->state]++;
		iocb = (httpn_io_context_t *)link_get_next(&s__dbg_iocb_list, &iocb->dbg_node);
	}
	pthread_mutex_unlock(&s__iocb_pool_lock);
#endif

}
char * 
httpn_dump_info(char *buf, int len, CURL *handle)
{
	int 		n;
	int			remained = len-10;
	char		*cursor = buf;
	curl_off_t 	t_dns=-1, t_conn=-1, t_total=-1;
	curl_off_t	dnspeed = -1;
	long		code = -1;
	char 		*rip = NULL;
	curl_off_t	reqsiz = -1, ressiz = -1;
	char		zspeed[32];
	long		ose;
	char		zose[64]="";
#define	CURLOFF_FLOAT(n_)		(float)((n_*1.0)/1000000.0)


	if (handle) {
		curl_easy_getinfo(handle, CURLINFO_NAMELOOKUP_TIME_T, 	&t_dns);
		curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME_T, 		&t_conn);
		curl_easy_getinfo(handle, CURLINFO_TOTAL_TIME_T, 		&t_total);

		curl_easy_getinfo(handle, CURLINFO_SIZE_UPLOAD_T, 		&reqsiz);
		curl_easy_getinfo(handle, CURLINFO_SIZE_DOWNLOAD_T, 	&ressiz);
		curl_easy_getinfo(handle, CURLINFO_SPEED_DOWNLOAD_T, 	&dnspeed);
		curl_easy_getinfo(handle, CURLINFO_OS_ERRNO, 			&ose);
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_getinfo(handle, CURLINFO_PRIMARY_IP, &rip);
		if (ose != 0) strerror_r(ose, zose, sizeof(zose));

		n = snprintf(cursor, remained, "IP[%s] TIME{DNS[%.2f] CONN[%.2f] TOTAL[%.2f]} D.SPEED[%sBps] SIZE(U/D){%ld, %ld}, HTTPCODE[%ld], OS.ERR[%s]",
					rip, CURLOFF_FLOAT(t_dns), CURLOFF_FLOAT(t_conn), CURLOFF_FLOAT(t_total), unitdump(zspeed, sizeof(zspeed), dnspeed), reqsiz, ressiz, code, zose);
	
		cursor += n;
		remained -= n;
	}
	*cursor = '\0';
	return buf;
}
static void
httpn_delegate_destroy_iocb(httpn_io_context_t *iocb, int force, char *f, int l)
{
	char					dbginfo[1024];

	if (iocb->autocreat == 0 && force == 0)  {
		return; /* force=TRUE요청일때만 실행하고 이외의 경우 바로 리턴 */
	}
	
	TRACE((iocb->tflg, "IOCB[%u] destroying tried ;%s at %d@%s\n", 
						iocb->id, 
						httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb),
						l, 
						f));

	if (iocb->id != iocb->keptid) { 
		TRACE((T_ERROR, "IOCB[%u] double destroy(force=%d,auto=%d) at %d@%s\n" 
						"IOCB-INFO:%s\n",
						iocb->id, 
						force, 
						iocb->autocreat,
						l,
						f,
						httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb)
						));
		//_nl_unlock(&iocb->lock);
		return; /* force=TRUE요청일때만 실행하고 이외의 경우 바로 리턴 */
	}


#ifdef DEBUG_IOCB
	iocb->dsource 	= f;
	iocb->dline 	= l;
#endif

	if (iocb->mpx) {
		iocb->cmd_id = HMC_DESTROY;
		httpn_mpx_put_iocb_direct(iocb->mpx, iocb);
	}
	else 
		httpn_free_iocb(iocb);
	return;
}
void
httpn_destroy_iocb(httpn_io_context_t *iocb, int force, char *f, int l)
{
	char					dbginfo[1024];
	httpn_mux_info_t		*mpx;
	int						tx;

	if (iocb->autocreat == 0 && force == 0)  {
		return; /* force=TRUE요청일때만 실행하고 이외의 경우 바로 리턴 */
	}
	
	TRACE((T_PLUGIN|iocb->tflg, "IOCB[%u] destroying tried(%d) ;%s at %d@%s\n", 
						iocb->id, 
						force,
						httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb),
						l, 
						f));

	if (iocb->id != iocb->keptid) { 
		TRACE((T_ERROR, "IOCB[%u] double destroy(force=%d,auto=%d) at %d@%s\n" 
						"IOCB-INFO:%s\n",
						iocb->id, 
						force, 
						iocb->autocreat,
						l,
						f,
						httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb)
						));
		//_nl_unlock(&iocb->lock);
		return; /* force=TRUE요청일때만 실행하고 이외의 경우 바로 리턴 */
	}


#ifdef DEBUG_IOCB
	iocb->dsource 	= f;
	iocb->dline 	= l;
#endif

	mpx = iocb->mpx;
	if (mpx) {
		LO_CHECK_ORDER(LON_NET_MUX);
		LO_PUSH_ORDER_FL(LON_NET_MUX,__FILE__, __LINE__);
		MPX_TRANSACTION( &mpx->mpxlock, tx, 10 ) {
			if (httpn_mpx_bound(iocb)) {
				TRACE((T_PLUGIN, "*** >>>>>>>>>> IOCB[%d] - found bound, request to unregister from mpx;%s\n", iocb->id, httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb)));
				httpn_mpx_remove_iocb_nolock(mpx, iocb, U_TRUE);
				TRACE((T_PLUGIN, "*** <<<<<<<<<< IOCB[%d] - found bound, request to unregister from mpx\n", iocb->id, httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb)));
			}
		}
		LO_POP_ORDER(LON_NET_MUX);
	}
	httpn_free_iocb(iocb);
	return;
}
void
httpn_free_iocb(httpn_io_context_t *iocb)
{

	char				dbginfo[2048]="";
	int					needwarn = 0;
	cfs_origin_driver_t	*driver = iocb->driver;

	TRACE((T_PLUGIN, "VOLUME[%s]/%s - IOCB[%d] freeing;%s\n",
					iocb->driver->signature,
					iocb->wpath,
					iocb->id,
					httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb)
					));

	if (iocb->autocreat == 0) {
		TRACE((T_PLUGIN, 	"Volume[%s].URL[%s] - IOCB[%u] freeing;%s\n",
						iocb->driver->signature,
						iocb->wpath, 
						iocb->id,
						httpn_dump_iocb(dbginfo, sizeof(dbginfo), iocb)
						));

	}

	//{
	//char	ziocb[2048];
	//TRACE((T_WARN, "IOCB[%u] - freed;%s\n", iocb->id, httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)));
	//}


	if (iocb->request) {
		httpn_request_destroy(iocb->request);
		iocb->request = NULL;
	}
	if (iocb->session) {
		if (needwarn) {
			httpn_dump_info(dbginfo, sizeof(dbginfo), iocb->session->eif);
		}
		TRACE((0, "LB[%p] - session %p destroying\n", LB_GET(iocb->session->pool->lb_pool), iocb->session));
		httpn_release_session(iocb, iocb->session);
		iocb->session = NULL;
	}
	if (iocb->stat.st_property)  {
		TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u]/auto[%d] destroying;PROP[%p]\n", iocb->id, iocb->autocreat, iocb->stat.st_property));
		kv_destroy(iocb->stat.st_property);
		iocb->stat.st_property = NULL;
	}



	if (needwarn) {
		TRACE((0, "VOLUME[%s]/%s.URL[%s] - slow [%.2f sec] [%d tries, curle=%d,http=%d]{%s}\n",
					iocb->driver->signature,
					iocb->zmethod,
					iocb->wpath,
#ifdef HTTPN_MEASURE_IOCB
					(float)PROFILE_GAP_MSEC(iocb->t_create, iocb->t_done),
#else
					-0.0,
#endif
					iocb->tries,
					iocb->last_curle,
					iocb->last_httpcode,
					dbginfo));
	}
	iocb->cmd_id 	= HMC_FREED;
	iocb->session 	= NULL;
	iocb->stat.st_property = NULL;
	iocb->request 	= NULL;
	if (iocb->wpath) 
		XFREE(iocb->wpath);


	iocb->id	= 0;
	iocb->magic	= 0;


	pthread_mutex_lock(&s__iocb_pool_lock);
#ifdef DEBUG_IOCB
	link_del(&s__dbg_iocb_list, &iocb->dbg_node);
#endif

	link_append(&s__iocb_pool, iocb, &iocb->node);
	pthread_mutex_unlock(&s__iocb_pool_lock);

	DEBUG_ASSERT(_ATOMIC_SUB(g__iocb_count, 1) >= 0);

	CFS_DRIVER_UNREF(driver);

	return;

}
#ifdef DEBUG_IOCB
void
httpn_dump_iocbs()
{
	httpn_io_context_t 	*iocb;
	int					idx = 0;
	char				ziocb[2048];


	pthread_mutex_lock(&s__iocb_pool_lock);
	iocb = link_get_head_noremove(&s__dbg_iocb_list);
	while (iocb) {
		TRACE((T_WARN, "[%03d] - %s\n", ++idx, httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)));
		iocb = (httpn_io_context_t *)link_get_next(&s__dbg_iocb_list, &iocb->dbg_node);
	}
	pthread_mutex_unlock(&s__iocb_pool_lock);

	
}
#endif

httpn_method_t
httpn_map_method(char *method)
{
	httpn_method_t	mid = HTTP_NULL;

	if (!strcasecmp(method, "GET")) {
		mid = HTTP_GET;
	}
	else if (!strcasecmp(method, "PUT")) {
		mid = HTTP_PUT;
	}
	else if (!strcasecmp(method, "MKDIR")) {
		mid = HTTP_MKDIR;
	}
	else if (!strcasecmp(method, "DELETE")) {
		mid = HTTP_DELETE;
	}
	else if (!strcasecmp(method, "HEAD")) {
		mid = HTTP_HEAD;
	}
	else if (!strcasecmp(method, "POST")) {
		mid = HTTP_POST;
	}
	else if (!strcasecmp(method, "PUT")) {
		mid = HTTP_PUT;
	}
	else  {
		mid = HTTP_CUSTOM;
	}
	return mid;
}
void
httpn_reset_response(httpn_io_context_t *iocb)
{
	nc_kv_list_t 	*old = NULL;
	if (iocb->stat.st_property) {
		old = iocb->stat.st_property;
		kv_destroy(iocb->stat.st_property);
		TRACE((T_INODE, "IOCB[%u]/PROP[%p] - destroying\n", iocb->id, iocb->stat.st_property));
		iocb->stat.st_property = NULL;
	}
	memset(&iocb->stat, 0, sizeof(iocb->stat));

	iocb->stat.st_property = kv_create_d(__FILE__, __LINE__);
	iocb->timedout			= 0;
	iocb->rangeop			= 0;
	iocb->needpause			= 0;
	iocb->entire			= 0;
	iocb->post_avail		= 0;
	iocb->inredirection		= 0;
	iocb->iotimeout			= 0;
	iocb->received			= 0;
	iocb->accepted			= 0;
	iocb->last_curle 		= 0;
	iocb->timedout			= 0;
	iocb->premature			= 0;
	iocb->last_httpcode 	= 0;
	iocb->last_errno 		= 0;
	iocb->last_curle		= 0;
	iocb->last_curle_str[0] = 0;

	TRACE((T_PLUGIN, "IOCB[%u]/auto[%d] reset;property[%p] -> [%p]\n", iocb->id, iocb->autocreat, old, iocb->stat.st_property));
}
void
httpn_set_raw_code_ifpossible(httpn_io_context_t *iocb, int code)
{
	nc_xtra_options_t	*opt = iocb->in_property;
	nc_asio_vector_t	*v = iocb->vector;

	if (iocb->method == HTTP_POST && opt) {

		/*
		 * cancel된 operation이면 패스
		 */
		if (v && v->canceled) goto L_dead_vector;

		DEBUG_ASSERT(iocb->inode == NULL || iocb->inode && INODE_GET_REF(iocb->inode)  > 0);
		kv_oob_lock(opt);
		if (kv_get_raw_code(opt) == 0)  {
			kv_set_raw_code(opt, code);
			kv_oob_set_error(opt, RBF_EOD); /* end of data */
			DEBUG_ASSERT(kv_oob_error(opt, RBF_EOD) != 0);
			TRACE((0, "IOCB[%d.%d] - EOD set with result[%d]\n", iocb->id, iocb->keptid, code));
			iocb->in_property = NULL;
		}
		kv_oob_unlock(opt);
	}
L_dead_vector:
	iocb->stat.st_origincode	= code;
}
char *
httpn_dump_iocb(char *buf, int len, httpn_io_context_t *iocb)
{
	char		xbuf[64]	= "";
	char		obuf[128]	= "";
	extern char _zs[][32];
#ifndef NC_RELEASE_BUILD
	char		*po = obuf;
	int			n;
#endif	

	switch (iocb->method)
	{
		case HTTP_POST:
			/*
			 * nothing
			 * mpx-lock이 걸려있을지도모르는 상황에서 rb-lock을 획득시도하면 절대 안됨
			 */
#ifndef NC_RELEASE_BUILD
			po = obuf;
			if (iocb->inode)
				n = sprintf(po, "INODE[%d]", iocb->inode->uid);
			else
				n = sprintf(po, "No IOV");

			po = po + n;
			if (iocb->in_property)
				n = sprintf(po, ".KV[%p]", iocb->in_property);
			*(po + n) = 0;
#endif


			break;
		case HTTP_GET:
			if (iocb->owner == HTTP_API_READ) {
				/*
				 * iocb에 대한 lock이 없으므로 
				 * 동시에 여러개의 thread(netcache core쪽)에서 접근되어 write될 수 있음
				 * 즉 정보의 일부는 부정확할 수 있음
				 */
				snprintf(obuf, sizeof(obuf)-1, "RI[blk_cur=%d,blk_off=%d, filled=%lld, blk_filled=%d]", 
												(int)iocb->u.r.blk_cursor, 
												(int)iocb->u.r.blk_off, 
												(long long)iocb->u.r.filled, 
												iocb->u.r.blk_filled);
			}
			break;
		default:
			/* do nothing*/
			break;
	}

	if (iocb->inode)
		sprintf(xbuf, ",INODE[%u]", iocb->inode->uid);

	/*
	 * IOCB[nnn] 형태의 grep tracing을 하기위해서 형식 변경
	 */
	snprintf(buf, len, "IOCB[%u]([%d/%d]-th) %s::{%s(%s/%s):ca[%d],S[%s/%s],ARA[%d],IMS[%d],mpx[x%p.%s],curle[%d],http[%d],stderrno[%d],TO[%d],auto[%d],R[%lld,%lld],Life[%d]%s/%s)}", 
						iocb->id, 
						iocb->tries,
						iocb->driverX->max_tries,
						(char *)__zowner[iocb->owner], 
						(char *)iocb->zmethod,
						(char *)iocb->driver->signature,
						(char *)iocb->wpath,
						(int)(iocb->canceled != 0),
						(char *)(iocb->session?httpn_zstate(iocb->session):"NULL"), 
						_zs[iocb->target_action],
						(iocb->assume_rangeable!=0),
						iocb->imson,
						iocb->mpx,
						(char *)_zmpxstate[iocb->state],
						iocb->last_curle,
						iocb->last_httpcode,
						iocb->last_errno,
						(int)(iocb->timedout?1:0),
						iocb->autocreat,
						iocb->reqoffset,
						iocb->reqsize,
						(int)(nc_cached_clock() - iocb->ctime),
						xbuf,
						obuf
						);
	return buf;
}
char *
httpn_dump_iocb_s(char *buf, int len, httpn_io_context_t *iocb)
{
	char	xbuf[64]	= "";
	char	obuf[128]	= "";


	DEBUG_ASSERT_IOCB(iocb, http_iocb_valid(iocb));
	if (iocb->inode)
		sprintf(xbuf, ",INODE[%u]", iocb->inode->uid);

	snprintf(buf, len, "IOCB[%u](%d-th) %s::session[%s],cancel[%d],mpx[%s],ARA[%d],curle[%d],http[%d],stderrno[%d],TO[%d],auto[%d],R[%lld,%lld],Life[%d]%s /%s}", 
						iocb->id, 
						iocb->tries, 
						(char *)__zowner[iocb->owner], 
						(char *)(iocb->session?httpn_zstate(iocb->session):"NULL"), 
						(int)(iocb->canceled != 0),
						_zmpxstate[iocb->state],
						(iocb->assume_rangeable!=0), 
						iocb->last_curle,
						iocb->last_httpcode,
						iocb->last_errno,
						(int)(iocb->timedout?1:0),
						iocb->autocreat,
						iocb->reqoffset,
						iocb->reqsize,
						(int)(nc_cached_clock() - iocb->ctime),
						xbuf,
						obuf
						);

	return buf;
}
int
http_iocb_valid(httpn_io_context_t *iocb)
{
	return (iocb && iocb->magic == IOCB_VALID_MAGIC) && (iocb->id == iocb->keptid);
}
int
http_retry_allowed(httpn_io_context_t *iocb)
{
	int		allowed = 0, n_o = 0;

	if (iocb->method == HTTP_POST) {
		allowed = (iocb->tries == 1);
	}
	else {
		allowed = (iocb->allow_retry && (iocb->canceled == 0) && (iocb->tries <= iocb->driverX->max_tries));
		_nl_lock(&iocb->driverX->lock, __FILE__, __LINE__);
		if (iocb->driverX->LB[NC_CT_PARENT])
			n_o = lb_pool_count_online(iocb->driverX->LB[NC_CT_PARENT]);
		if (iocb->driverX->LB[NC_CT_ORIGIN])
			n_o += lb_pool_count_online(iocb->driverX->LB[NC_CT_ORIGIN]);
		_nl_unlock(&iocb->driverX->lock);

		allowed = (iocb->tries== 1 || (allowed && (n_o > 1)));
	}

	return allowed;
}
lb_t *
httpn_get_LB(httpn_io_context_t *iocb)
{
	if (iocb->session == NULL) return NULL;
	return LB_GET(iocb->session->pool->lb_pool);
}

void *
httpn_iocb_cleaner(void *notused)
{
#define	MAX_IOCB_TOCLEAN		100
	httpn_io_context_t	*iocb[MAX_IOCB_TOCLEAN];
	int					i, n = 0;
	int					niocb = 0;
	perf_val_t			s,e;
	long 				d;
	nc_time_t			tdq_last_run = 0;

	g__iocb_free_queue = tlcq_init(U_TRUE);
	DEBUG_ASSERT(g__iocb_free_queue != NULL);

	while (g__iocb_cleaner_shutdown == 0) {
		n = 0;
		PROFILE_CHECK(s);
		niocb = tlcq_dequeue_batch(g__iocb_free_queue, iocb, MAX_IOCB_TOCLEAN, 1000);
		if (niocb > 0) {
			for (i = 0; i < niocb; i++) {
				n++;
				httpn_delegate_destroy_iocb(iocb[i], U_TRUE, __FILE__, __LINE__);
			}
		} 
		PROFILE_CHECK(e);
		d = PROFILE_GAP_MSEC(s,e);
#ifndef NC_RELEASE_BUILD
		if (n > 0 && d > 1000) {
			TRACE((T_INFO, "%d iocb freed (%.2f sec)\n", n, (1.0*d)/1000.0));
		}
#endif
		if ((nc_cached_clock() - tdq_last_run) > 5) {
			httpn_handle_defered_queue(NULL, U_FALSE);
			tdq_last_run = nc_cached_clock();
		}
	}
	tlcq_destroy(g__iocb_free_queue);
	httpn_handle_defered_queue(NULL, U_TRUE);
	return NULL;
}
int
httpn_iocb_prepare_pool(int n)
{
	int					i;
	httpn_io_context_t *arr = NULL;

	arr = (httpn_io_context_t *)XCALLOC(n, sizeof(httpn_io_context_t), AI_DRIVER);
	for (i = 0; i < n; i++) {
		_nl_cond_init(&arr[i].cmd_signal);
		arr[i].cmd_id = HMC_FREED;
		link_append(&s__iocb_pool, &arr[i], &arr[i].node);
	}

	return n;
}
