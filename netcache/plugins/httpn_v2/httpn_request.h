#ifndef __HTTP_REQUEST_H__
#define __HTTP_REQUEST_H__
#include <curl/curl.h>
#include <curl/easy.h>



typedef void (*httpn_result_code_proc_t)(int status, void *cb);
typedef size_t (*httpn_write_data_proc_t)(char *ptr, size_t size, size_t nmemb, void *userdata);
typedef size_t (*httpn_read_data_proc_t)(void *ptr, size_t size, size_t nmemb, void *userdata);
typedef size_t (*httpn_head_data_proc_t)(void *ptr, size_t size, size_t nmemb, void *userdata);
struct httpn_session;



struct httpn_request {
	httpn_method_t		method;
	char				zmethod[32];
	char				lastline[512];
	nc_xtra_options_t 	*inject_property;
	struct curl_slist	*headers;
	struct curl_slist	*dynheaders;
	char 				*outfilter;
	char				*url;
	int					code;  /* HTTP status code */
	int					error;   /* CURL error code */
	char				error_string[128]; /* CURL error string */
	void				*read_cb; /* callback data which passwd to the following procs */
	void				*head_cb; /* callback data which passwd to the following procs */
	void				*write_cb; /* callback data which passwd to the following procs */
	void 				*status_cb;

	cfs_off_t			reqoffset;
	cfs_size_t			reqsize;

	int					write_fp:1;
	int					entire_read:1; /*전체 읽기인 경우 1 */
	nc_size_t 			datalen;
	httpn_write_data_proc_t	write_proc;
	httpn_read_data_proc_t	read_proc;
	httpn_head_data_proc_t	head_proc;
	httpn_result_code_proc_t status_proc;
	struct {
#ifdef WIN32
		HANDLE 		fd;
#else
		int 		fd;
#endif
		nc_off_t	size;
	} ufp;


	/* bookeeping */
	char 			*origin; 	 /* static pointer, don't free it */
	char 			*eurl; 		 /* recently-used effective url */
	double 			t_elapsed;
	double 			upload_bytes;
	double 			download_bytes;
};

#define		MPX_TRANSACTION(_lx, _r,_thr)	for (_r = 0, _nl_lock(_lx, __FILE__, __LINE__);  \
											_r == 0;  \
											_r = 1, _nl_unlock(_lx))
#define		MPX_RELAX(_lx, _r,_thr)	for (_r = 0, _nl_unlock(_lx);  \
											_r == 0;  \
											_r = 1, _nl_lock(_lx, __FILE__, __LINE__))

/* tproxy 기능에 사용되는 구조체임, httpn_request.h에도 동일한 내용이 들어있어야 한다. */
typedef struct client_info_tag {
	int		size;	/* client_info_tag의 크기가 들어 간다. solproxy와 httpn간의 같은 헤더를 쓰는지 검증용 */
	char  	ip[64];
} client_info_t;


struct httpn_io_context;

httpn_request_t * httpn_request_create(httpn_io_context_t *iocb, char *method, char *outfilter, nc_kv_list_t *inprop);
void httpn_request_destroy(httpn_request_t *req);
void httpn_request_set_reader_callback(httpn_request_t *req, httpn_read_data_proc_t proc, void *data);
void httpn_request_set_head_callback(httpn_request_t *req, httpn_head_data_proc_t proc, void *data);
void httpn_request_set_writer_callback(httpn_request_t *req, httpn_write_data_proc_t proc, void *data);
int  httpn_request_get_statinfo(httpn_request_t *req, nc_stat_t *stat);
void httpn_request_set_status_callback(httpn_request_t *req, httpn_result_code_proc_t proc, void *);
#ifdef WIN32
void httpn_request_set_writer_fd(httpn_request_t *req, HANDLE fp);
#else
void httpn_request_set_writer_fd(httpn_request_t *req, int fd);
#endif
int httpn_request_exec_single(httpn_io_context_t *iocb) ;
int httpn_request_exec_multi(httpn_io_context_t *iocb, int action); 
void httpn_request_add_header(httpn_request_t *req, const char *name, const char *value, int needreplace);
void httpn_request_post_data(httpn_request_t *req, char * data, nc_size_t len);
int httpn_response_code(httpn_io_context_t *iocb, int curlres);
void httpn_unlock(httpn_session_pool_t *pool);
void httpn_lock(httpn_session_pool_t *pool);
int httpn_get_location(httpn_request_t *req, char *locbuf);
void httpn_request_set_length(httpn_request_t *req, nc_size_t len);
int httpn_setup_curl_sock(void *clientp, curl_socket_t curlfd, curlsocktype purpose);
int Base64encode(char *encoded, const char *string, int len);
int Base64encode_len(int len);
int httpn_expect_redirection(httpn_io_context_t *iocb);
void httpn_reset_headers(httpn_io_context_t *iocb);
char * httpn_setup_url(httpn_session_pool_t *pool, char *pool_url, const char *path);
void httpn_commit_headers(httpn_request_t *req);
int httpn_request_exec_resume(httpn_io_context_t *iocb, int action) ;
void httpn_set_readinfo(httpn_io_context_t *iocb, cfs_off_t off, cfs_size_t siz);
int httpn_request_contains(httpn_io_context_t *iocb, cfs_off_t *off, cfs_size_t *len);
nc_uint32_t httpn_task_id();
int httpn_mpx_handle_done_iocb(httpn_io_context_t *iocb);
int httpn_mpx_run_iocb(httpn_io_context_t *iocb);
int httpn_mpx_execute_iocb(httpn_io_context_t *iocb, httpn_pin_state_t action);
int httpn_mpx_pause_iocb_nolock(httpn_io_context_t *iocb);
int httpn_mpx_EOH_iocb(httpn_io_context_t *iocb);
int httpn_mpx_handle_paused_context(httpn_driver_info_t *instance);
int httpn_mpx_task(httpn_io_context_t *iocb, httpn_mpxop_t op, int curle, int self);
int httpn_mpx_wait_completion(httpn_io_context_t *iocb);
int httpn_mpx_handler (void *udrv, void *);
int httpn_mpx_idle (void *udrv, void *);
int httpn_mpx_remove_iocb_nolock(httpn_mux_info_t *mpx, httpn_io_context_t *iocb, int needack);
int httpn_mpx_resume(httpn_io_context_t *iocb);
int httpn_mpx_resume_iocb(httpn_io_context_t *iocb, int needack);
int httpn_mpx_resume_iocb_nolock(httpn_io_context_t *iocb, int needack);
int httpn_mpx_resume_iocb_self(httpn_io_context_t *iocb);
const char * httpn_zstate(httpn_session_t *session);
void httpn_mpx_timer_restart(httpn_io_context_t *iocb, char *f, int l);
void httpn_mpx_timer_pause(httpn_io_context_t *iocb, char *f, int l);
int httpn_is_buggy(char *url);
int httpn_add_buggy(char *url);
int httpn_mpx_bound(httpn_io_context_t *iocb);
int httpn_mpx_unregister_nolock_self( httpn_mux_info_t *mux, httpn_io_context_t *iocb );
int httpn_mpx_task_unregister(httpn_io_context_t *iocb);
void httpn_mpx_handler_prolog(void *u);
void httpn_mpx_handler_epilog(void *u);
int httpn_mpx_finish_iocb(httpn_io_context_t *iocb);
void httpn_cleanup_request(httpn_request_t *request);
void httpn_mpx_put_iocb_direct(httpn_mux_info_t *mpx, httpn_io_context_t *iocb);
#endif /* __REQUEST_H__ */
