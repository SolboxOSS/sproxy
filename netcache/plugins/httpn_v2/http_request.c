#include <netcache_config.h>
#include <error.h>
#include <fcntl.h>
#include <iconv.h>
#include <langinfo.h>
#include <libintl.h>
#include <netinet/in.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <curl/easy.h>

#include <netcache.h>
#include <block.h>
#include <threadpool.h>
#include "util.h"
#include "trace.h"
#include "hash.h"

/*
 * 다른 곳에서 enable되었어도 여기선 무시
 */
#include "httpn_driver.h"
#include "httpn_request.h"
#include "http_codes.h"


#define		HTTPN_AGENT	"NetCache : http proxy driver for" $VERSION "(" $DATE " " $TIME ")"

void httpn_dump_phase_command(nc_origin_io_command_t *cmd, int pid, const char *msg);
static char * httpn_encode_url(const char *path);
static int httpn_add_user_headers(char *key, char *value, void *cb);
static int httpn_commit_user_headers(char *key, char *value, void *cb);
static int httpn_mpx_handle_direct_queue_nolock(httpn_mux_info_t *mpx);
static int httpn_mpx_handle_completed_nolock(httpn_mux_info_t *mpx);
static void httpn_mpx_waitlist(httpn_mux_info_t *mpx, int put);
static int httpn_mpx_handle_system_queue_nolock(httpn_mux_info_t *mpx, int maxsched);

extern void 		*__timer_wheel_base;
extern char			_zmpxstate[][32];
extern mode_t 		default_file_mode;
extern int			__terminating;
extern long			g__iocb_count;
extern tp_handle_t	g__netpool;
extern link_list_t	g__mpx_q;
static pthread_mutex_t	__mpx_q_lock = PTHREAD_MUTEX_INITIALIZER;

#ifndef NC_RELEASE_BUILD
bt_timer_t	__timer_am;
long __cnt_paused		= 0;
long __cnt_running		= 0;
#endif

__thread httpn_mux_info_t	*g__current_mpx = NULL;

/*
 *
 * thread-local-storage var
 */




char	__zowner[][32] = {
	"0:HTTP_API_NULL" , 
	"1:HTTP_API_GLOBAL",
	"2:HTTP_API_GETATTR",
	"3:HTTP_API_READ",
	"4:HTTP_API_WRITE",
	"5:HTTP_API_PROBE"
};
char	__zresult[][32]={
	"0:MPX_TASK_RESULT_COMPLETE",
	"1:MPX_TASK_RESULT_PAUSED"
};
#if 0
#define 		CHECK_BUGGY_URL(_i) 		if (httpn_is_buggy((_i)->request->url)) { \
												(_i)->verbose = 1; \
											}
#define			KEEP_BUGGY_URL(_u) 			httpn_add_buggy(_u)
#else
#define			CHECK_BUGGY_URL(_i)
#define			KEEP_BUGGY_URL(_u) 			
#endif

#ifdef PROFILE_PERFORM_TIME
extern  mavg_t						*g__perform;
#endif


#define	HTTPN_WAIT_CMD_DONE(mpx_, iocb_, enq_) { \
		int	tx;\
		struct timespec ts; \
		(iocb_)->cmd_signal = (pthread_cond_t *)XMALLOC(sizeof(nc_cond_t), AI_ETC); \
		_nl_cond_init((iocb_)->cmd_signal); \
		enq_; \
		if (mpx_) curl_multi_wakeup((mpx_)->mif); \
		DEBUG_ASSERT(dm_inode_locked() == 0); \
		MPX_TRANSACTION( &(mpx_)->mpxlock, tx, 10 ) { \
			while (((iocb_)->cmd_id != HMC_NONE) && http_iocb_valid(iocb)) { \
				make_timeout( &ts, 50, CLOCK_MONOTONIC); \
				_nl_cond_timedwait((iocb_)->cmd_signal, &mpx->mpxlock, &ts); \
			} \
		} \
		_nl_cond_destroy((iocb_)->cmd_signal); \
		XFREE((iocb_)->cmd_signal); \
		}



httpn_request_t *
httpn_request_create(httpn_io_context_t *iocb, char *method, char *outfilter, nc_kv_list_t *inprop)
{
	httpn_request_t	*req = (httpn_request_t *)XMALLOC(sizeof(httpn_request_t), AI_DRIVER);
	client_info_t	*client = NULL; //, *new_client = NULL;

	DEBUG_ASSERT(req != NULL);

	memset(req, 0, sizeof(httpn_request_t));

	req->headers				= NULL; /*extra header list */
	req->outfilter  			= outfilter;
	req->inject_property    	= kv_create_d(__FILE__, __LINE__);
	req->method					= iocb->method;
	strcpy(req->zmethod,iocb->zmethod);
	if (inprop) {
		kv_for_each(inprop, httpn_add_user_headers, (void *)req);

		client = (client_info_t *)kv_get_client(inprop);
		if (client) {
			/* 2018-08-09 CHG huibong memory leak 수정 */
			//new_client = XMALLOC(client->size, AI_DRIVER);
			//memcpy(new_client, client, client->size);
			//kv_set_client(req->inject_property, (void *)new_client, new_client->size);
			kv_set_client(req->inject_property, (void *)client, client->size);
		}
	}
	
	httpn_request_add_header(req, "Connection", "Keep-Alive", U_TRUE);
	httpn_request_set_reader_callback(req, httpn_block_reader, iocb);

	switch (iocb->method) {
		case HTTP_MKDIR:
			httpn_request_add_header(req, "Expect", "", U_FALSE);
			httpn_request_add_header(req, "Transfer-Encoding", "", U_FALSE);
			httpn_request_add_header(req, "Content-Type", "application/directory", U_FALSE);
			break;
		case HTTP_GET:
			//if (iocb->owner == HTTP_API_GETATTR) {
			//	httpn_request_set_reader_callback(req, httpn_block_null_reader, iocb);
			//	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u] - GET, setting null_reader\n",
			//					iocb->id
			//					));
			//}
			break;
		case HTTP_POST:
			TRACE((iocb->tflg|T_PLUGIN, "URL[%s] - preparing post_data_proc\n", iocb->wpath));
			//httpn_request_set_reader_callback(req, httpn_block_null_reader, iocb);
			httpn_request_set_writer_callback(req, httpn_post_data_proc, iocb);
			httpn_request_add_header(req, "Expect", "", U_FALSE);
			//TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u] - POST, setting null_reader\n",
			//					iocb->id
			//					));
			break;
		case HTTP_DELETE:
			httpn_request_add_header(req, "Expect", "", U_FALSE);
			httpn_request_add_header(req, "Transfer-Encoding", "", U_FALSE);
			httpn_request_add_header(req, "Content-Length", "", U_FALSE);
			break;
		case HTTP_CUSTOM:
			//if (iocb->owner == HTTP_API_GETATTR) {
			//	httpn_request_set_reader_callback(req, httpn_block_null_reader, iocb);
			//	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u] - %s, setting null_reader\n",
			//					iocb->id,
			//					iocb->zmethod
			//					));
			//}
			break;
		default:
			/* ERROR */
			break;
	}


	return req;
}

int
httpn_request_contains(httpn_io_context_t *iocb, cfs_off_t *off, cfs_size_t *len)
{
	int 	in = U_TRUE;
	int 	in_b, in_e, in_e1, in_e2;

	in_b = in = in && (iocb->reqoffset == *off); /* 시작점 */

	in_e = in = in && (	(in_e1=(iocb->reqsize < 0)) /*infinite*/ || 
						(in_e2=(iocb->reqoffset + iocb->reqsize -1) >= (*off + *len -1))
					  );
	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u].preset[O:%lld,L:%lld], requested range[O:%lld,L:%lld] ==> %d (b=%d,e=%d(%d,%d))\n",
						iocb->id,
						iocb->reqoffset, 
						iocb->reqsize, 
						*off, 
						*len, 
						in,
						in_b,
						in_e,
						in_e1,
						in_e2
						));
	if (in) {
		*off = iocb->reqoffset;
		*len = iocb->reqsize;
	}
	return in;
}
void
httpn_set_readinfo(httpn_io_context_t *iocb, cfs_off_t off, cfs_size_t siz)
{
	iocb->reqoffset	= off;
	iocb->reqsize 	= siz;
	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u]- set range[Off:%lld,Len:%lld](%lld:%lld)\n", 
					iocb->id, 
					iocb->reqoffset, 
					iocb->reqsize,

					iocb->reqoffset,
					((iocb->reqsize > 0)? (iocb->reqoffset + iocb->reqsize-1):-1LL)
					));
}
static int
httpn_add_user_headers(char *key, char *value, void *cb)
{
        httpn_request_t         *req = cb;

        httpn_request_add_header(req, key, value, U_FALSE);
        return 0;
}

void
httpn_request_destroy(httpn_request_t *req)
{
	if (req->url) {
		DEBUG_ASSERT(nc_check_memory(req->url, 0));
	}
	if (req->inject_property) {
		
		kv_destroy(req->inject_property);
		req->inject_property = NULL;
	}
	if (req->headers)
		curl_slist_free_all(req->headers);
	if (req->dynheaders) {
		curl_slist_free_all(req->dynheaders);
		req->dynheaders = NULL;
	}

	if (req->url) XFREE(req->url);
	XFREE(req);

	return;
}
void
httpn_request_set_reader_callback(httpn_request_t *req, httpn_read_data_proc_t proc, void *data)
{
	req->read_proc	= proc;
	req->read_cb	= data;
}
void
httpn_request_set_head_callback(httpn_request_t *req, httpn_read_data_proc_t proc, void *data)
{
	req->head_proc	= proc;
	req->head_cb	= data;
}
void 
httpn_request_set_status_callback(httpn_request_t *req, httpn_result_code_proc_t proc, void *data)
{
	req->status_proc = proc;
	req->status_cb = data;
}
void
httpn_request_set_writer_callback(httpn_request_t *req, httpn_write_data_proc_t proc, void *data)
{
	req->write_proc	= proc;
	req->write_cb			= data;
}
void
httpn_request_set_writer_fd(httpn_request_t *req, int fd)
{
	struct stat		s;
	req->ufp.fd	= fd;
	fstat(fd, &s);
	req->ufp.size	= s.st_size;
	req->write_fp   = 1;
	TRACE((T_PLUGIN, "request upload file size : %lld bytes, FD(%ld)\n", (long long)req->ufp.size, (long)fd));
}
typedef struct {
	int 	fd;
	off_t 	offset;
} httpn_file_read_info_t;

size_t 
httpn_file_reader( void *ptr, size_t size, size_t nmemb, void *userdata)
{
	httpn_file_read_info_t *ri = (httpn_file_read_info_t *)userdata;
	size_t 	l = 0;

	l = pread(ri->fd, ptr, size*nmemb, ri->offset);
	TRACE((T_PLUGIN, "FILE[%ld] at offset %lld (size=%lld, nmemb=%lld) - read %ld bytes\n", 
			(long)ri->fd, 
			(long long)ri->offset, 
			(long long)size,
			(long long)nmemb,
			(long)l));
	ri->offset += l;

	return l;
}

int httpn_trace_io(CURL *curl, curl_infotype itype, char *msg, size_t msglen, void *ud);
/* run command */

int
httpn_request_exec_single(httpn_io_context_t *iocb)
{
	CURLcode	result;
	nc_time_t	now = nc_cached_clock();
	long 		upsiz;
	cfs_origin_driver_t	*driver = NULL;
	httpn_driver_info_t	*hi = NULL;
	long 		hdrsiz;
	client_info_t	*client = NULL;




	


	TRACE((iocb->tflg|T_PLUGIN, "given URL(%s)\n", iocb->request->url));
	driver = iocb->driver;
	hi = (httpn_driver_info_t *)CFS_DRIVER_PRIVATE(driver);


	curl_easy_setopt(iocb->session->eif, CURLOPT_DEBUGFUNCTION, httpn_trace_io);
	curl_easy_setopt(iocb->session->eif, CURLOPT_DEBUGDATA, 	(void *)iocb);
	curl_easy_setopt(iocb->session->eif, CURLOPT_VERBOSE, 3);


	if (hi->proxy) {
		curl_easy_setopt(iocb->session->eif, CURLOPT_PROXY, 		hi->proxy);
		curl_easy_setopt(iocb->session->eif, CURLOPT_PROXYPORT, 	hi->proxy_port);
		curl_easy_setopt(iocb->session->eif, CURLOPT_PROXYTYPE, 	CURLPROXY_HTTP);
		httpn_request_add_header(iocb->request, "Proxy-Connection", "", U_TRUE);
	}

	client = (client_info_t *)kv_get_client(iocb->request->inject_property);

	if(client != NULL && client->ip[0] != 0) {	//TPROXY 동작을 위한 부분
		 curl_easy_setopt(iocb->session->eif, CURLOPT_SOCKOPTFUNCTION, httpn_setup_curl_sock);
		 curl_easy_setopt(iocb->session->eif, CURLOPT_INTERFACE, client->ip);
		 TRACE((iocb->tflg|T_PLUGIN, "Connection type : TPROXY\n"));
	}

	if (iocb->pool->https) {
		if (!hi->opt_https_secure) {
			SET_OPT(iocb->session->eif, CURLOPT_SSL_VERIFYPEER, 0L);
			SET_OPT(iocb->session->eif, CURLOPT_SSL_VERIFYHOST, 0L);
		}
		else {
			SET_OPT(iocb->session->eif, CURLOPT_SSL_VERIFYPEER, 1L);
		}
		if (hi->opt_https_falsestart) {
			SET_OPT(iocb->session->eif, CURLOPT_SSL_FALSESTART, 1L);
		}
		if (hi->opt_https_crlfile)
			SET_OPT(iocb->session->eif, CURLOPT_CRLFILE, hi->opt_https_crlfile);
		
		if (hi->opt_https_cert)
			SET_OPT(iocb->session->eif, CURLOPT_SSLCERT, hi->opt_https_cert);
		if (hi->opt_https_cert_type)
			SET_OPT(iocb->session->eif, CURLOPT_SSLCERTTYPE, hi->opt_https_cert_type);
		if (hi->opt_https_sslkey)
			SET_OPT(iocb->session->eif, CURLOPT_SSLKEY, hi->opt_https_sslkey);
		if (hi->opt_https_sslkey_type)
			SET_OPT(iocb->session->eif, CURLOPT_SSLKEYTYPE, hi->opt_https_sslkey_type);

		if (hi->opt_https_cainfo)
			SET_OPT(iocb->session->eif, CURLOPT_CAINFO, hi->opt_https_cainfo);
		if (hi->opt_https_capath)
			SET_OPT(iocb->session->eif, CURLOPT_CAPATH, hi->opt_https_capath);

		SET_OPT(iocb->session->eif, CURLOPT_SSLVERSION, hi->opt_https_tlsversion);

	}
	TRACE((iocb->tflg|T_PLUGIN, "trying the constructed url[%s]... \n", iocb->request->url));
	curl_easy_setopt(iocb->session->eif, CURLOPT_URL, iocb->request->url);
    curl_easy_setopt(iocb->session->eif, CURLOPT_VERBOSE, 1);


	switch (iocb->method) {
		case HTTP_HEAD:
			curl_easy_setopt(iocb->session->eif, CURLOPT_FILETIME, 1);
			curl_easy_setopt(iocb->session->eif, CURLOPT_NOBODY, 1);
			curl_easy_setopt(iocb->session->eif, CURLOPT_HEADER, 0);
			if (iocb->request && iocb->request->head_proc) {
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERFUNCTION, iocb->request->head_proc);
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERDATA, iocb->request->head_cb);
			}
			break;
		case HTTP_MKDIR:
			curl_easy_setopt(iocb->session->eif, CURLOPT_UPLOAD, 1);
			curl_easy_setopt(iocb->session->eif, CURLOPT_INFILESIZE, 0);
			httpn_request_add_header(iocb->request, "Expect", "", U_TRUE);
			httpn_request_add_header(iocb->request, "Transfer-Encoding", "", U_TRUE);

			httpn_request_add_header(iocb->request, "Content-Type", "application/directory", U_TRUE);
			break;
		case HTTP_PUT:
			curl_easy_setopt(iocb->session->eif, CURLOPT_UPLOAD, 1);
			if (iocb->request->datalen > 0)
				curl_easy_setopt(iocb->session->eif, CURLOPT_INFILESIZE_LARGE, (curl_off_t)iocb->request->datalen);
			TRACE((iocb->tflg|T_PLUGIN, "PUT[%s] - length[%lld] set\n", iocb->request->url, iocb->request->datalen));
			if (iocb->request->write_proc) {
				curl_easy_setopt(iocb->session->eif, CURLOPT_READDATA, iocb->request->write_cb);
				curl_easy_setopt(iocb->session->eif, CURLOPT_READFUNCTION, iocb->request->write_proc);
			}
			if (iocb->request->head_proc) {
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERFUNCTION, iocb->request->head_proc);
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERDATA, iocb->request->head_cb);
			}
			curl_easy_setopt(iocb->session->eif, CURLOPT_TIMEOUT, 0);
			break;
		case HTTP_GET:
			if (iocb->request->head_proc) {
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERFUNCTION, iocb->request->head_proc);
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERDATA, iocb->request->head_cb);
			}

			/* 
			 * 2013.5.7
			 * FOLLOW 옵션 제거, 사용하는 경우 redirection을 하면서 property
			 *들이 이중으로 중첩 누적되는 현상을 피할 수 없음
			 */
			curl_easy_setopt(iocb->session->eif, CURLOPT_WRITEDATA, iocb->request->read_cb);
			curl_easy_setopt(iocb->session->eif, CURLOPT_WRITEFUNCTION, iocb->request->read_proc);
			break;
		case HTTP_POST:
			curl_easy_setopt(iocb->session->eif, CURLOPT_POST, 1L);

			if (iocb->request->head_proc) {
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERFUNCTION, iocb->request->head_proc);
				curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERDATA, iocb->request->head_cb);
			}
			/*
			 * POST의 결과로 수신할 데이타가 있을 때 설정
			 */
			if (iocb->request->read_proc) {
				curl_easy_setopt(iocb->session->eif, CURLOPT_WRITEDATA, iocb->request->read_cb);
				curl_easy_setopt(iocb->session->eif, CURLOPT_WRITEFUNCTION, iocb->request->read_proc);
			}

			/*
			 * 서버로 전송할 데이타가 있을 때 설정됨
			 */
			if (iocb->request->write_proc) {
				curl_easy_setopt(iocb->session->eif, CURLOPT_READDATA, iocb->request->write_cb);
				curl_easy_setopt(iocb->session->eif, CURLOPT_READFUNCTION, iocb->request->write_proc);
			}

                httpn_request_add_header(iocb->request, "Expect", "", U_TRUE);

			break;
		case HTTP_DELETE:
			httpn_request_add_header(iocb->request, "Expect", "", U_TRUE);
			httpn_request_add_header(iocb->request, "Transfer-Encoding", "", U_TRUE);
			httpn_request_add_header(iocb->request, "Content-Length", "", U_TRUE);
			curl_easy_setopt(iocb->session->eif, CURLOPT_CUSTOMREQUEST, "DELETE");
			break;
		default:
			/* ERROR */
			break;
	}
    curl_easy_setopt(iocb->session->eif, CURLOPT_HTTPHEADER, iocb->request->headers);

	TRACE((iocb->tflg|T_PLUGIN, "%s %s - performing\n", iocb->zmethod, iocb->request->url));

    result = iocb->last_curle 	= curl_easy_perform(iocb->session->eif);

	TRACE((iocb->tflg|T_PLUGIN, "%s %s - CURL[%ld], HTTP[%d]\n", iocb->zmethod, iocb->request->url, iocb->last_curle, iocb->last_httpcode));
	curl_easy_getinfo(iocb->session->eif, CURLINFO_EFFECTIVE_URL, 	&iocb->request->eurl); 	/* last-used effective url */
	curl_easy_getinfo(iocb->session->eif, CURLINFO_PRIMARY_IP, 		&iocb->request->origin); 	/* origin IP */
	curl_easy_getinfo(iocb->session->eif, CURLINFO_TOTAL_TIME,  	&iocb->request->t_elapsed);
	iocb->request->upload_bytes = 0;
	iocb->request->download_bytes = 0;
	iocb->request->t_elapsed = nc_cached_clock() - now;
	curl_easy_getinfo(iocb->session->eif, CURLINFO_REQUEST_SIZE, 	&upsiz);
	iocb->request->upload_bytes = (double)upsiz;
	curl_easy_getinfo(iocb->session->eif, CURLINFO_SIZE_DOWNLOAD, 	&iocb->request->download_bytes);
	curl_easy_getinfo(iocb->session->eif, CURLINFO_HEADER_SIZE, 	&hdrsiz);
	iocb->request->download_bytes += (double)hdrsiz;
#if 1
	/*
	 * 디버그용
	 *     	|--NAMELOOKUP
	 *     	|--|--CONNECT
	 *	   	|--|--|--APPCONNECT
	 *	   	|--|--|--|--PRETRANSFER
	 *		|--|--|--|--|--STARTTRANSFER
	 *		|--|--|--|--|--|--TOTAL
	 *		|--|--|--|--|--|--REDIRECT
	 */
	 curl_off_t		t_total;
	curl_easy_getinfo(iocb->session->eif, CURLINFO_TOTAL_TIME_T, 	&t_total);
	 TRACE((iocb->tflg|T_PLUGIN, "URL[%s] - %ld bytes %.2f msec\n", iocb->request->eurl,  (long)iocb->request->download_bytes, (float)(t_total/1000.0)));
	 
#endif	
	return result;
}
static int
httpn_commit_user_headers(char *key, char *value, void *cb)
{
	httpn_request_t 	*req = (httpn_request_t *)cb;
	int 				l;
	char 				*z_tag;
	struct curl_slist  *result = NULL;

	l = strlen(value) + strlen(key) + 4;
	z_tag = (char  *)alloca(l);
	sprintf(z_tag, "%s: %s", key, value);
	
	/* 2018-08-20 huibong curl_slist_append() 호출 실패시 반환값 check 기능 추가 (#32301) */
	//req->headers = curl_slist_append(req->headers, z_tag);
	result = curl_slist_append(req->headers, z_tag);
	if (result != NULL)
	{
		if (result != req->headers)
			req->headers = result;

		TRACE((T_PLUGIN, "injecting '%s'\n", z_tag));
	}
	else
	{
		TRACE((T_WARN, "request header injecting '%s' failed.\n", z_tag));
	}
	return 0;
}
void
httpn_commit_headers(httpn_request_t *req)
{
	kv_for_each(req->inject_property, httpn_commit_user_headers, (void *)req); 
}
void 
httpn_request_add_header(httpn_request_t *req, const char *name, const char *value, int replace)
{
	if (req->outfilter && strcasestr(req->outfilter, name)) 
		return;
 
	/*
	 * 중복체크 하지 않음
	 */
	if (replace) {
		kv_replace(req->inject_property, (char *)name, (char *)value, U_FALSE);
	}
	else {
		kv_add_val_d(req->inject_property, (char *)name, (char *)value, __FILE__, __LINE__);
	}
}

void 
httpn_request_set_length(httpn_request_t *req, nc_size_t len)
{
	req->datalen = len;
}

int
httpn_setup_curl_sock(void *clientp, curl_socket_t curlfd, curlsocktype purpose)
{
	(void) clientp;
	(void) purpose;

	int       optval = 1;
	socklen_t optlen = sizeof(optval);
#ifdef IP_TRANSPARENT
	setsockopt(curlfd, SOL_IP, IP_TRANSPARENT, (void *) &optval, optlen);
#endif
	return 0;
}
int
httpn_is_secure(const char *path)
{
	return strncmp(path, "https:", 6) == 0;
}
int
httpn_request_add_ims_header(httpn_io_context_t *iocb, nc_stat_t *old_s, apc_open_context_t *oc)
{
	char 		*val 			= NULL;
	void 		*iter 			= NULL;
	int 		needcontinue 	= U_TRUE;



	if (old_s->st_property == NULL)  {
		TRACE((T_INODE|iocb->tflg|T_PLUGIN, "Volume[%s].CKey[%s] - INODE[%u] adding IMS header required, but old_s->st_property is NULL\n",
								  iocb->driver->signature,
								  oc->cache_key,
								  (oc->inode?oc->inode->uid:-1)
								  ));
		return iocb->imson;
	}

	iter = nvm_get_fcg_first(oc->volume);

	while (iter && needcontinue) {
		needcontinue = U_TRUE;
		switch (nvm_fcg_id(iter))  {
			case VOL_FC_MTIME:
				val = kv_find_val(old_s->st_property, "Last-Modified", U_FALSE);
				if (val) {
					httpn_request_add_header(iocb->request, "If-Modified-Since", val, U_TRUE);
					TRACE((iocb->tflg|T_PLUGIN, "OBJECT[%s] - adding 'If-Modified-Since: %s'\n", iocb->wpath, val));
					needcontinue = U_FALSE;
				}
				break;

			case VOL_FC_DEVID:
				val = kv_find_val(old_s->st_property, "ETag", U_FALSE);
				if (val) { 
					httpn_request_add_header(iocb->request, "If-None-Match", val, U_TRUE);
					TRACE((iocb->tflg|T_PLUGIN, "OBJECT[%s] - adding 'If-None-Match: %s'\n", iocb->wpath, val));
					needcontinue = U_FALSE;
				}
				break;
			default:
				break;
		}
		iter = nvm_get_fcg_next(oc->volume, iter);
	}
	if (needcontinue == 0) {
		iocb->imson 			=  U_TRUE;
		iocb->knownsize			= old_s->st_size;
		iocb->cachedhttpcode 	= old_s->st_origincode;
		TRACE((T_INODE|iocb->tflg|T_PLUGIN, "Volume[%s].CKey[%s] - INODE[%u] imson[%s], cachedhttpcode=%d\n",
								  iocb->driver->signature,
								  oc->cache_key,
								  (oc->inode?oc->inode->uid:-1),
								  val,
								  old_s->st_origincode
								  ));
	}
	else {
		TRACE((T_INFO|T_INODE|iocb->tflg|T_PLUGIN, "Volume[%s].CKey[%s] - INODE[%u] IMS required but no info. contained in inode's properties\n",
								  iocb->driver->signature,
								  oc->cache_key,
								  (oc->inode?oc->inode->uid:-1)
								  ));
	}
	return iocb->imson;
}

char *
httpn_setup_url(httpn_session_pool_t *pool, char *pool_url, const char *path)
{
	char 	*t_path = (char *)path;
	char 	*url;
	char 	*tofree = NULL;


	tofree = t_path = httpn_encode_url(path);

	if (pool_url && strlen(pool_url) > 0) {
		/*
		 * 아래 로직은 path에 처리과정 문제가 있음
		 */
		while (*t_path == '/') t_path++;
		url = (char *)XMALLOC(strlen(pool_url) + strlen(t_path) + 2, AI_DRIVER);
		sprintf(url, "%s/%s", pool_url, t_path);
		XFREE(tofree);
		TRACE((T_PLUGIN, "(pool[%s],path[%s]) => [%s]\n", pool_url, path, url));
	}
	else {
		url = tofree;
	}

	return url;
}
void 
httpn_dump_phase_command(nc_origin_io_command_t *cmd, int pid, const char *msg)
{
#if 0
	char 	zphase[][32] = {
			"NULL",
			"ORIGIN_PHASE_REQUEST",
			"ORIGIN_PHASE_RESPONSE"
	};
	char 		zpbuf[1024]="";
	TRACE((iocb->tflg|T_PLUGIN, "%s: %s - %s %s\n"
					 "\t - status : %d\n"
					 "\t - properties : {%s}\n",
					 msg, zphase[pid], cmd->method, cmd->url,
					 cmd->status,
					kv_dump_property(cmd->properties, zpbuf, sizeof(zpbuf))));
#endif
}
#define 	RFC3986_RESERVED(c)		(strchr("!*'();:@&=+$,/?#[]", (c)) != NULL)
#define 	RFC3986_UNRESERVED(c)		(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~", (c)) != NULL)
static char 
_tohex(char c)
{
	static char 	_h[] = "0123456789abcdef";
	return _h[c&15];
}
static char *
httpn_encode_url(const char *path)
{
	char 	*tpath_anchor = XMALLOC(strlen(path)*3 + 1, AI_DRIVER);
	char	*tpath = tpath_anchor;

	while (*path) {
		if (RFC3986_UNRESERVED(*path) || RFC3986_RESERVED(*path)) {
			*tpath++ = *path;
		}
		else {
			*tpath ++ = '%';
			*tpath ++ = _tohex(*path >> 4);
			*tpath ++ = _tohex(*path & 0xF);
		}
		path++;
	}
	*tpath = '\0';
	return tpath_anchor;
}
/*
 * 	0: upto HEADER
 *  1: from HEADER to BODY
 *  2: ALL
 */


char 	_zs[][32]={
		"0:HS_INIT",
		"1:HS_HEADER_OUT",
		"2:HS_POSTING",
		"3:HS_HEADER_IN",
		"4:HS_EOH",
		"5:HS_BODY",
		"6:HS_DONE",
		"7:HS_MAX"
};
const char *
httpn_zstate(httpn_session_t *session)
{
	if (!session) return "NULL";

	return _zs[session->state];
}
#ifdef NOT_USED
static int
httpn_need_break(httpn_session_t *session, int action)
{
	extern int __terminating;
	return ((session->state == HS_MAX) || (session->state >= action) || __terminating);
}
#endif

int
httpn_expect_redirection(httpn_io_context_t *iocb)
{
	int httpn_is_redirection(int c);

	return (
			iocb->driverX->follow_redirection  &&
			httpn_is_state(iocb->session, HS_EOH) &&
			httpn_is_redirection(iocb->last_httpcode)
			);
}
/*
 * phase-handler를 통해서 변경되는 부분만 reset
 */
void
httpn_cleanup_request(httpn_request_t *request)
{
	if (request && request->headers) {
		curl_slist_free_all(request->headers);
		request->headers = NULL;
	}
}




static nc_uint32_t 	___schedule_id = 0;
nc_uint32_t
httpn_task_id()
{
	nc_uint32_t 	id = 0;
	id = _ATOMIC_ADD(___schedule_id, 1);

	if (id == 0) 
		id = _ATOMIC_ADD(___schedule_id, 1);
		
	return id;
}


int
httpn_mpx_handle_done_iocb(httpn_io_context_t *iocb)
{
	long 		req_reqsiz;
	double		req_upsiz;
	long 		res_hdrsiz;
	double 		res_downsiz;
#ifdef PHASE_HANDLER_DEFINED
#endif
	



	



	/*
	 * read callback update 
	 */

	httpn_set_raw_code_ifpossible(iocb, iocb->last_httpcode); /*정상처리*/
	iocb->request->eurl = NULL;
	curl_easy_getinfo(iocb->session->eif, CURLINFO_EFFECTIVE_URL,	&iocb->request->eurl);
	curl_easy_getinfo(iocb->session->eif, CURLINFO_PRIMARY_IP, 		&iocb->request->origin);
	curl_easy_getinfo(iocb->session->eif, CURLINFO_TOTAL_TIME,  	&iocb->request->t_elapsed);

	curl_easy_getinfo(iocb->session->eif, CURLINFO_REQUEST_SIZE, 	&req_reqsiz);
	curl_easy_getinfo(iocb->session->eif, CURLINFO_SIZE_UPLOAD, 	&req_upsiz);
	curl_easy_getinfo(iocb->session->eif, CURLINFO_HEADER_SIZE, 	&res_hdrsiz);
	curl_easy_getinfo(iocb->session->eif, CURLINFO_SIZE_DOWNLOAD, 	&res_downsiz);
	iocb->request->upload_bytes 	= (double)req_reqsiz + req_upsiz;
	iocb->request->download_bytes 	= (double)res_downsiz + res_hdrsiz;

	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%d].%s(%s):%s - REQ SIZE[%.2f, %.2f] ==> RES SIZE[%.2f, %.2f]\n",
				iocb->id,
				iocb->zmethod,
				iocb->wpath,
				_zmpxstate[iocb->state], 
				(float)req_reqsiz,
				(float)req_upsiz,
				(float)res_hdrsiz,
				(float)res_downsiz
				));
	
#ifdef	HTTPN_MEASURE_IOCB
	PROFILE_CHECK(iocb->t_done);
	/*
	 * 디버그용
	 *     	|--NAMELOOKUP
	 *     	|--|--CONNECT
	 *	   	|--|--|--APPCONNECT
	 *	   	|--|--|--|--PRETRANSFER
	 *		|--|--|--|--|--STARTTRANSFER
	 *		|--|--|--|--|--|--TOTAL
	 *		|--|--|--|--|--|--REDIRECT
	 */
	curl_off_t		t_total;
	curl_easy_getinfo(iocb->session->eif, CURLINFO_TOTAL_TIME_T, 	&t_total);

	TRACE((T_WARN, "IOCB[%u].URL[%s] - %ld bytes %.2f msec;(send=%.2f,fba=%.2f,hdrend=%.2f, pause=%.2f, resume=%.2f, done=%.2f\n", 
				iocb->id, 
				iocb->request->eurl,  
				(long)iocb->request->download_bytes, 
				(float)(t_total/1000.0),

				(float)PROFILE_GAP_MSEC(iocb->t_create, iocb->t_send),
				(float)PROFILE_GAP_MSEC(iocb->t_create, iocb->t_fba),
				(float)PROFILE_GAP_MSEC(iocb->t_create, iocb->t_hdrend),
				(float)PROFILE_GAP_MSEC(iocb->t_create, iocb->t_pause),
				(float)PROFILE_GAP_MSEC(iocb->t_create, iocb->t_resume),
				(float)PROFILE_GAP_MSEC(iocb->t_create, iocb->t_done)
	));
#endif	
	

	return 0;
}


int
httpn_mpx_execute_iocb(httpn_io_context_t *iocb, httpn_pin_state_t action)
{
	//int 				tx;
	char				ziocb[2048];
	struct timespec		ts;
	pthread_mutex_t		*lock;

	/*
	 * 준비된 io context를 scheduler에 pending queued에 등록
	 */


	DEBUG_ASSERT_IOCB(iocb, iocb->mpx == NULL);
	DEBUG_ASSERT_IOCB(iocb, httpn_mpx_state_is(iocb, IOCB_MPX_INIT));
	


	/*
	 * enqueue onto the MUX shared queue
	 */
	iocb->target_action = action;
	iocb->cmd_id = HMC_SCHEDULE;


	TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u]/ADDR[%p]:scheduled to RUN for target action[%s];%s\n", 
						iocb->id,
						iocb, 
						_zs[action],
						httpn_dump_iocb(ziocb,sizeof(ziocb), iocb)
						));

	lock = tp_lock(g__netpool);
	pthread_mutex_lock(lock);
	tp_submit(g__netpool, iocb);
	while (iocb->cmd_id == HMC_SCHEDULE) {
		make_timeout( &ts, 50, CLOCK_MONOTONIC); 
		_nl_cond_timedwait2(&iocb->cmd_signal, lock, &ts); 
			
	}
	pthread_mutex_unlock(lock);

	return 0;
}

/*
 *
 * 구현방식에 대한 설명
 * 	- 현재 http driver는 libcurl의 multi-interface에 easy-interface를 추가하는 방식으로 동작
 *  - multi-interface는 epoll처름 운영할 수 있으나, 치명적으로 다른 점은
 *	  일부 데이타 수신 후 epoll에서 socket을 일시 제거하는 것과 같은 기능 구현이 안됨
 *  - 즉, http header까지 수신 후, multi-interface에서 easy interface를 제거하는 경우
 *    libcurl내부에서 socket close가 발생함
 *  - 그러므로 mpx_handler()에 IOCB(easy-interface)가 바인딩되고 나면 pause한다고
 *    multi-interface엣 제거하고 mpx_handler()가 thread-pool에 회수되는 방식으로 구현 불가능함
 */
int
httpn_mpx_pause_iocb_nolock(httpn_io_context_t *iocb)
{
#ifdef	HTTPN_MEASURE_IOCB
	PROFILE_CHECK(iocb->t_pause);
#endif
	DEBUG_ASSERT_IOCB (iocb, httpn_mpx_state_is(iocb, IOCB_MPX_PAUSED) == U_FALSE);

	mpx_STM(iocb, IOCB_EVENT_PAUSE_TRY, 0, __FILE__, __LINE__);
	CHK_IOCB_OPT( iocb, curl_easy_pause( iocb->session->eif, CURLPAUSE_ALL ) );
	
	return 0;
}
void
httpn_mpx_put_iocb_direct(httpn_mux_info_t *mpx, httpn_io_context_t *iocb)
{
	int		tx;

	DEBUG_ASSERT(nc_check_memory(mpx,0));
	DEBUG_ASSERT(http_iocb_valid(iocb));

	LO_CHECK_ORDER(LON_NET_MUX);
	LO_PUSH_ORDER_FL(LON_NET_MUX,__FILE__, __LINE__);

	DEBUG_ASSERT(nc_check_memory(mpx,0));
	DEBUG_ASSERT(http_iocb_valid(iocb));

	MPX_TRANSACTION( &mpx->mpxlock, tx, 10 ) {
		link_append(&mpx->q_direct, iocb, &iocb->node); 
		curl_multi_wakeup(mpx->mif); 
	}
	LO_POP_ORDER(LON_NET_MUX);
}
/*
 * netcache또는 APP쪽에서 호출
 */
int
httpn_mpx_resume_iocb_nolock(httpn_io_context_t *iocb, int needack)
{
	httpn_mux_info_t	*mpx;
	struct timespec		ts; 
	int					resumed = 0;

	DEBUG_ASSERT_IOCB(iocb,  httpn_mpx_bound(iocb)); 




	mpx					= iocb->mpx;

	if (httpn_mpx_state_is(iocb, IOCB_MPX_PAUSED)) { 

		iocb->cmd_id	= HMC_RESUME;
		iocb->needpause	= 0;
		link_append(&mpx->q_direct, iocb, &iocb->node); 
		curl_multi_wakeup(mpx->mif); 
		TRACE((T_PLUGIN, "IOCB[%u] - sedning RESUME request\n", iocb->id));
		if (needack) {
			while ((iocb->cmd_id == HMC_RESUME) && httpn_mpx_state_is(iocb, IOCB_MPX_PAUSED)) {
				make_timeout( &ts, 50, CLOCK_MONOTONIC); 
				_nl_cond_timedwait(&iocb->cmd_signal, &mpx->mpxlock, &ts); 
			} 
		}
		resumed++;
	} 


	return resumed;
}
int
httpn_mpx_resume_iocb(httpn_io_context_t *iocb, int needack)
{
	httpn_mux_info_t	*mpx;
	int					tx;
	int					resumed = 0;

	DEBUG_ASSERT_IOCB(iocb,  httpn_mpx_bound(iocb)); 

	mpx					= iocb->mpx;

	DEBUG_ASSERT(dm_inode_locked() == 0);
	LO_CHECK_ORDER(LON_NET_MUX);
	LO_PUSH_ORDER_FL(LON_NET_MUX,__FILE__, __LINE__);
	MPX_TRANSACTION( &mpx->mpxlock, tx, 10 ) {
		resumed = httpn_mpx_resume_iocb_nolock(iocb, needack);
	}
	LO_POP_ORDER(LON_NET_MUX);


	return resumed;
}
int
httpn_mpx_remove_iocb_nolock(httpn_mux_info_t *mpx, httpn_io_context_t *iocb, int needack)
{
	struct timespec		ts; 

	iocb->cmd_id		= HMC_REMOVE;

	if (!httpn_mpx_state_is(iocb, IOCB_MPX_FINISHED)) {
		link_append(&mpx->q_direct, iocb, &iocb->node); 
		curl_multi_wakeup(mpx->mif); 

		if (needack) {
			while (!httpn_mpx_state_is(iocb, IOCB_MPX_FINISHED)) {
				make_timeout( &ts, 50, CLOCK_MONOTONIC); 
				_nl_cond_timedwait(&iocb->cmd_signal, &mpx->mpxlock, &ts); 
			} 
		}
	}
	DEBUG_ASSERT_IOCB(iocb,  httpn_mpx_state_is(iocb, IOCB_MPX_FINISHED)) ;


	return 0;
}
/*
 * mpx_handler 내부에서 호출
 */
int
httpn_mpx_resume_iocb_self(httpn_io_context_t *iocb)
{
	char				ziocb[512];
	int					r;
	httpn_mux_info_t    *mpx;


#ifdef	HTTPN_MEASURE_IOCB
	PROFILE_CHECK(iocb->t_pause);
#endif

	DEBUG_ASSERT_IOCB(iocb,  httpn_mpx_bound(iocb)); 


	TRACE( (iocb->tflg|T_PLUGIN, "IOCB[%u] - resuming;{%s}\n",
					iocb->id,
					httpn_dump_iocb( ziocb, sizeof( ziocb ), iocb )
					) );
	mpx = iocb->mpx;


	iocb->session->paused = 0;

	/*
	 * 주의!! *****
	 *		현재 libcurl 구현에서는pause상태인 easy interface에 대해서 resume을 실행하면
	 *		함수가 리턴되지 않고, 현재 대기중인 데이타를 처리하기위한 기존 설정에 따란
	 *		콜백이 처리 진행됨
	 *		그러므로 아래 함수 httpn_mpx_resume_iocb_self()가 호출되기 전에 
	 *		일단 iocb->state를 RUN으로 바꿔야함
	 */
	httpn_mpx_timer_restart(iocb, __FILE__, __LINE__);

	httpn_mpx_state_set(iocb, IOCB_MPX_TRY_RUN, __FILE__, __LINE__);
	TRACE((T_PLUGIN, ">>>>>>>>>>>>>>>>>> IOCB[%d] - resuming, buffered data handling may start heare !!!!\n", iocb->id));
	CHK_IOCB_OPT( iocb, curl_easy_pause( iocb->session->eif, CURLPAUSE_CONT ) );
	TRACE((T_PLUGIN, "<<<<<<<<<<<<<<<<<< IOCB[%d] - resuming completed\n", iocb->id));



	return r;
}


/*
 * handler 내부에서 호출
 */
int
httpn_mpx_unregister_nolock_self( httpn_mux_info_t *mux, httpn_io_context_t *iocb )
{
	httpn_io_context_t	*reged = NULL;
	int 				r;
	char				ziocb[2048];


	if (httpn_mpx_bound(iocb)) {
		
		httpn_mpx_timer_pause(iocb, __FILE__, __LINE__);
    	CHK_OPT(r, curl_easy_setopt(iocb->session->eif, CURLOPT_PROGRESSFUNCTION, NULL));
#if LIBCURL_VERSION_NUM >= 0x072000
    	CHK_OPT(r, curl_easy_setopt(iocb->session->eif, CURLOPT_XFERINFOFUNCTION, NULL));
#endif
		CHK_OPT(r, curl_easy_setopt(iocb->session->eif, CURLOPT_DEBUGFUNCTION, NULL));
		CHK_OPT(r, curl_easy_setopt(iocb->session->eif, CURLOPT_HEADERFUNCTION, NULL));
		CHK_OPT(r, curl_easy_setopt(iocb->session->eif, CURLOPT_READFUNCTION, NULL));
		CHK_OPT(r, curl_easy_setopt(iocb->session->eif, CURLOPT_WRITEFUNCTION, NULL));
		CHK_OPT(r, curl_easy_getinfo(iocb->session->eif, CURLINFO_PRIVATE, (char **)&reged));
	}
	if (reged) {
		TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u]/state[%s] - removed EIF[%p] from MPX[%p];%s\n", 
					iocb->id, 
					_zmpxstate[iocb->state], 
					iocb->session->eif,
					iocb->mpx,
					httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)
					));

    	SET_OPT(iocb->session->eif, CURLOPT_PRIVATE, NULL);

		CHK_OPT( r, curl_multi_remove_handle( mux->mif, iocb->session->eif ) );

	
	}
#ifndef NC_RELEASE_BUILD
	else {
		TRACE((T_WARN, "IOCB[%u] - NOT mpx bound;%s\n",
						iocb->id,
						httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)
			  ));
	}
#endif
	return 0;
}




void 
httpn_mpx_timer_restart(httpn_io_context_t *iocb, char *f, int l)
{
#if 1

	/* CHG 2018-10-25 huibong Origin transfer timeout 처리 방식 결정에 따른 수정 (#32401) 
	*                 - timer 를 이용한 transfer timeout 처리 방식 사용하기로 결정
	*                 - 이에 conf 의 설정값이 timer 에 적용되도록 수정 처리
	*/


	/* 참고사항 
	* 0 보다 큰 경우만 timer set 처리... default 값은 10 sec 임.
	* 만약 0 인 경우.. conf 에서 임의로 disable 처리를 위해 설정한 값이므로.. timer 가 동작하지 않도록 한다.
	* iocb 인자 유효성 검사는 본 함수 호출 측에서 수행하므로... 제외
	* 본 함수는 호출 횟수가 상당히 많으므로... 로깅 등의 기능은 추가하지 않는다.
	*/
	if( iocb->driverX->xfer_timeout > 0 )
	{
		iocb->iotimeout =  sys_gettickcount() + iocb->driverX->xfer_timeout * 1000LL;
		TRACE((0, "IOCB[%d] - *set timeout [%lld](xfer=%d)\n", iocb->id, iocb->iotimeout, iocb->driverX->xfer_timeout));
	}

#endif
}

void 
httpn_mpx_timer_pause(httpn_io_context_t *iocb, char *f, int l)
{
	iocb->iotimeout = 0;
}


#ifndef NC_RELEASE_BUILD

void
httpn_activity_monitor(void *u)
{

	long		r, p,i;
	long		v_cnt = 0;
	double		v_min, v_max, v_avg;

	r = GET_ATOMIC_VAL( __cnt_running);
	p = GET_ATOMIC_VAL( __cnt_paused);
	i = GET_ATOMIC_VAL( g__iocb_count);
	pthread_mutex_lock(&__mpx_q_lock);
	mavg_stat(g__perform, &v_cnt, &v_min, &v_max, &v_avg);
	pthread_mutex_unlock(&__mpx_q_lock);
	TRACE((T_MONITOR, "IOCB : (run,pause,created)=(%ld,%ld,%ld), avg perform(%ld)=(%.2f, %.2f, %.2f) msec\n", 
						r, p, i, 
						v_cnt, v_min, v_max, v_avg));

	bt_set_timer( __timer_wheel_base, &__timer_am, 10 * 1000 /*10 sec*/, httpn_activity_monitor, NULL );
}
#else
void
httpn_activity_monitor(void *u)
{
}
#endif

/*
 * thread-pool의 callback 함수
 * 모든 작업이 완료되면 리턴해야함
 */

void
httpn_mpx_handler_prolog(void *u)
{
	httpn_mux_info_t ** pmpx = (httpn_mux_info_t **)u;
	pthread_mutexattr_t	mattr;

	*pmpx 		= (httpn_mux_info_t *)XMALLOC(sizeof(httpn_mux_info_t), AI_DRIVER);
	(*pmpx)->mif = curl_multi_init();
	(*pmpx)->tid = gettid();
	(*pmpx)->run = 0;
	INIT_LIST(&(*pmpx)->q_direct);
#ifndef NC_RELEASE_BUILD
	INIT_LIST(&(*pmpx)->q_run);
#endif
	pthread_mutexattr_init( &mattr );
	pthread_mutexattr_settype( &mattr, PTHREAD_MUTEX_RECURSIVE );
	_nl_init( &(*pmpx)->mpxlock, &mattr );
	pthread_mutexattr_destroy( &mattr );
	TRACE((T_INFO, "thread %ld(mpx=%p) created\n", (*pmpx)->tid, *pmpx));
}

void
httpn_mpx_handler_epilog(void * u)
{
	httpn_mux_info_t	*mpx = (httpn_mux_info_t *)u;

	DEBUG_ASSERT(mpx->run == 0);
	DEBUG_ASSERT(link_count(&mpx->q_direct, U_TRUE) == 0);
	TRACE((T_INFO, "thread %ld(mpx=%p) destroyed(run=%d, direct_q=%d)\n", mpx->tid, mpx, mpx->run, link_count(&mpx->q_direct, U_TRUE)));
	curl_multi_cleanup(mpx->mif);
	_nl_destroy(&mpx->mpxlock);
	XFREE(mpx);
}
int
httpn_mpx_idle(void *u, void *tcb)
{
	httpn_mux_info_t 	*mpx = (httpn_mux_info_t *)tcb;
	int					idle = 0;
	int					tx;

	/*
	 * 현재 mpx에 이미 binding되어서 실행 중인 iocb 갯수
	 */
	LO_CHECK_ORDER(LON_NET_MUX);
	LO_PUSH_ORDER_FL(LON_NET_MUX,__FILE__, __LINE__);
	MPX_TRANSACTION( &mpx->mpxlock, tx, 10 ) {
		idle = (GET_ATOMIC_VAL(mpx->run) == 0) && (link_count(&mpx->q_direct, U_TRUE) == 0);
		if (idle) {
			if (mpx->idle == 0) mpx->idle = nc_cached_clock();
		}
		else {
			mpx->idle = 0;
		}

	}
	LO_POP_ORDER(LON_NET_MUX);

	/*
	 * 60초 이상 idle 지속되는 경우에
	 */
	return (mpx->idle > 0 && ((nc_cached_clock() - mpx->idle) > 60));
}

int
httpn_mpx_handler(void *v, void *tcb)
{
	httpn_mux_info_t 	*t__mpx = (httpn_mux_info_t *)tcb;
	int				  	runnings = 0;
	int					n_tosched;
	int					n_sched = 0;
	int					n_resumed = 0;
	int					n = 0, cres, nfd;
	nc_time_t			idlebegin = 0;
	int					dd = 0;
#ifdef PROFILE_PERFORM_TIME
	perf_val_t			s, e;
	long				d;
#endif





	g__current_mpx = t__mpx;
	do {
		n_tosched 	= 10;
		n			= 0;

		tp_set_busy(g__netpool, U_TRUE);


		if (GET_ATOMIC_VAL(t__mpx->run) > 0 || n_sched > 0) {
#ifdef PROFILE_PERFORM_TIME
			DO_PROFILE_USEC(s, e, d) {
#endif
			CHK_OPT(cres, curl_multi_perform(t__mpx->mif, &runnings));
#ifdef PROFILE_PERFORM_TIME
			}
			pthread_mutex_lock(&__mpx_q_lock);
			mavg_update(g__perform, ((double)d/1000.0));
			pthread_mutex_unlock(&__mpx_q_lock);
#endif

		}

		DEBUG_ASSERT(dm_inode_locked() == 0);
		LO_CHECK_ORDER(LON_NET_MUX);
		LO_PUSH_ORDER_FL(LON_NET_MUX,__FILE__, __LINE__);
		_nl_lock( &g__current_mpx->mpxlock, __FILE__, __LINE__ );
		n_resumed = httpn_mpx_handle_direct_queue_nolock(t__mpx);

		n = httpn_mpx_handle_completed_nolock(t__mpx);

		n_sched = httpn_mpx_handle_system_queue_nolock(t__mpx, n_tosched); 


		_nl_unlock( &g__current_mpx->mpxlock);
		LO_POP_ORDER(LON_NET_MUX);


		tp_set_busy(g__netpool, U_FALSE);


		if (n_sched == 0 && n_resumed == 0) {
			nfd	 = 0;
				

				/*
				 * mpx-handler가 추가 iocb 처리 요청을 받을 수 있는 상태
				 */
				httpn_mpx_waitlist(t__mpx, U_TRUE);
    			CHK_OPT(cres, curl_multi_poll(t__mpx->mif, NULL, 0, 50, &nfd));
				httpn_mpx_waitlist(t__mpx, U_FALSE);
				/*
				 * 이후 추가 schedule요청 안받음
				 */
		}

		if (t__mpx->run != 0)
			idlebegin = 0;
		else if (t__mpx->run == 0) {
			if (idlebegin == 0) 
				idlebegin = nc_cached_clock();

			if (tp_stop_signalled(g__netpool))
				break;/* thread-pool stop signalled */
		}

		if ((idlebegin != 0) && ((dd = (nc_cached_clock() - idlebegin)) > 30)) {
			break;
		}
		
	} while (!t__mpx->shutdown );

	return n;
}

int
httpn_mpx_bound(httpn_io_context_t *iocb)
{
	httpn_io_context_t *b = NULL;

	if (iocb->session && iocb->session->eif)
		curl_easy_getinfo(iocb->session->eif, CURLINFO_PRIVATE, (char **)&b);
	return (b != NULL);
}



static int
httpn_mpx_handle_system_queue_nolock(httpn_mux_info_t *mpx, int maxsched)
{
	int					nsched = 0;
	httpn_io_context_t *iocb;
	pthread_mutex_t		*lock;
	httpn_io_context_t	*toschedule = NULL;


	lock = tp_lock(g__netpool);
	iocb = (httpn_io_context_t *)tp_fetch(g__netpool);
	while (iocb) {
		DEBUG_ASSERT(CFS_DRIVER_INUSE(iocb->driver)); 
		DEBUG_ASSERT(iocb->cmd_id == HMC_SCHEDULE);

		iocb->mpx = mpx;
		_ATOMIC_ADD(mpx->run, 1);
		/*
		 * signal을 먼저 실행함
		 */
		pthread_mutex_lock(lock);
		iocb->cmd_id = HMC_NONE;
		TRACE((0, "IOCB[%u] - scheduled to exec\n", iocb->id));
		_nl_cond_signal(&iocb->cmd_signal);
		toschedule = iocb;
		iocb = (httpn_io_context_t *)tp_fetch(g__netpool);
		pthread_mutex_unlock(lock);

		httpn_mpx_state_set(toschedule, IOCB_MPX_READY, __FILE__, __LINE__);
#ifndef NC_RELEASE_BUILD
		link_append(&mpx->q_run, toschedule, &toschedule->mpx_node);
#endif
		nsched++;

		mpx_STM(toschedule, IOCB_EVENT_BEGIN_TRY, 0, __FILE__, __LINE__);
	}
	return nsched;
		
}

static int 
httpn_mpx_handle_direct_queue_nolock(httpn_mux_info_t 	*mpx)
{
	httpn_io_context_t 	*iocb;
	int					tx;
	int					handled = 0;
	char				ziocb[2048];
	char				z_cmd[][32]={ "HMC_NONE", "HMC_SCHEDULE", "HMC_RESUME", "HMC_REMOVE", "HMC_DESTROY", "HMC_FREED" };
	int					resume = 0;

	iocb = (httpn_io_context_t *)link_get_head(&mpx->q_direct);
	while (iocb) {
		resume = 0;
		TRACE((iocb->tflg|T_PLUGIN, "IOCB[%u] - command[%s] got;%s\n", iocb->id, z_cmd[iocb->cmd_id], httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)));
		switch (iocb->cmd_id)  {
			case HMC_RESUME:
				if (httpn_mpx_state_is(iocb, IOCB_MPX_PAUSED)) {
					mpx_STM(iocb, IOCB_EVENT_RESUME_TRY, 0, __FILE__, __LINE__);
					MPX_RELAX( &mpx->mpxlock, tx, 10 ) {
						httpn_mpx_resume_iocb_self(iocb);
					}
					resume++;
				}
				TRACE((0, "IOCB[%d] - HTTP[%s], MPX[%s], resume=%d\n", iocb->id, httpn_zstate(iocb->session), _zmpxstate[iocb->state], resume));
				iocb->cmd_id = HMC_NONE;
				_nl_cond_signal(&iocb->cmd_signal);
				break;
			case HMC_REMOVE:
				if (!httpn_mpx_state_is(iocb, IOCB_MPX_FINISHED))
					mpx_STM(iocb, IOCB_EVENT_END_TRY, U_TRUE, __FILE__, __LINE__);
				iocb->cmd_id = HMC_NONE;
				_nl_cond_signal(&iocb->cmd_signal);
				break;
			case HMC_DESTROY:
				if (httpn_mpx_bound(iocb) && !httpn_mpx_state_is(iocb, IOCB_MPX_FINISHED)) {
					mpx_STM(iocb, IOCB_EVENT_END_TRY, U_TRUE, __FILE__, __LINE__);
				}
				httpn_destroy_iocb(iocb, U_TRUE, __FILE__, __LINE__);
				
				break;
			default:
				TRACE((T_WARN, "IOCB[%u] - on queue with unknown cmd(%d);%s\n", 
						iocb->id, 
						iocb->cmd_id,
						httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)
						));
				break;
		}
		handled++;
		/*
		 * iocb_stm()내에서 resume하면 state가 다 꼬임
		 * 왜냐하면 CHK_OPT( r, curl_easy_pause( iocb->session->eif, CURLPAUSE_CONT ) );
		 * 의 호출내에서 이미 받은 데이타에 대해서 head_dispatch, block_reader()등의 호출을 해버리기 때문임
		 */
		iocb = (httpn_io_context_t *)link_get_head(&mpx->q_direct);
	}
	return handled;
}
static int 
httpn_mpx_handle_completed_nolock(httpn_mux_info_t *mpx)
{
	CURLMsg 			*msg;
	httpn_io_context_t	*iocb;
	int					cnt_msg, msgcnt;
	int					loop;


	cnt_msg	= 0;
	do {
		loop++;

		msg = curl_multi_info_read( mpx->mif, &msgcnt );

		if (msg && (msg->msg == CURLMSG_DONE)) {
			cnt_msg++;
			/*
			 * we reached the end of transfer
			 */
			iocb = NULL;
			curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&iocb);
			DEBUG_ASSERT(CFS_DRIVER_INUSE(iocb->driver)); 
			TRACE((T_PLUGIN, "IOCB[%d] - ******* TRY COMPLETED(curle=%d) ******\n", iocb->id, msg->data.result));
			iocb->last_curle	= msg->data.result;
#if 0
			if (msg->data.result == CURLE_OPERATION_TIMEDOUT) {
				iocb->timedout		= 1;
			}
			if (iocb->last_curle == CURLE_ABORTED_BY_CALLBACK) {
				if (iocb->premature) 
					iocb->last_curle = CURLE_OK;
			}
#endif
			if (iocb->timedout) 
				mpx_STM(iocb, IOCB_EVENT_TIMEOUT_TRY, 0, __FILE__, __LINE__); /* retry if possible */
			else 
				mpx_STM(iocb, IOCB_EVENT_END_TRY, iocb->canceled, __FILE__, __LINE__); /* canceled */
		}
	} while (msg);
	return cnt_msg;
}
/*
 * called within mpx_handler()
 */
static void
httpn_mpx_waitlist(httpn_mux_info_t *mpx, int put)
{
	pthread_mutex_lock(&__mpx_q_lock);
	if (put) {
		link_append(&g__mpx_q, mpx, &mpx->node);
	}
	else {
		link_del(&g__mpx_q, &mpx->node);
	}
	pthread_mutex_unlock(&__mpx_q_lock);
}

/*
 * called from netcache thread
 */
httpn_mux_info_t *
httpn_idle_mpx()
{
	httpn_mux_info_t *mpx;

	pthread_mutex_lock(&__mpx_q_lock);
	mpx = link_get_head_noremove(&g__mpx_q);
	pthread_mutex_unlock(&__mpx_q_lock);

	return mpx;
}
