#ifndef __SESSION_MANAGER_H__
#define __SESSION_MANAGER_H__

#include "zlive.h"

struct service_info_tag;

typedef struct live_info_tag {
	zlive 			z_ctx;		/* live 관리용 context */
	void			*req;		/* 매번 접속시 마다 갱신해야함. */
	void		 	*builder;	/* 광고가 종료 되는 경우(zlive_expire_preroll_callback 호출 경우)에는 해당 빌더를 삭제 한다. */
	int				ad_finished;	/* 광고가 종료된 경우 1이 셋팅된다. */
} live_info_t;
//struct _zipper_rangeget_context;
typedef struct session_info_tag {
	mem_pool_t 		*mpool;
	int				id; 		/*random으로 생성한 unique ID */
	int				timeout;		/* session 만료시간 */
	time_t			start_time;		/* 처음 접속시간  */
	time_t			end_time;		/* 마지막 접속 시간, 접속시 마다 갱신 */
//	_zipper_rangeget_context *rcontext; 	/* mp42http기능 사용시 필요함 */
	char 			*client_ip;
	char 			*host;
	char 			*key;
	char			*path;			/* 통계 기록시 사용 */
	char			*enc_key;		/* AES-128 암호화화 사용 */
	struct service_info_tag 	*service;
	int				protocol;	/* hls, dash, progresive download 등 */
	uint64_t 		req_hdr_size;
	uint64_t 		req_body_size;
	uint64_t 		res_hdr_size;
	uint64_t 		res_body_size;
	int				refcount;	/* 현재 해당 session을 사용중인 연결수 */
	unsigned long	account;	/* 총 요청수 */
	live_info_t 	l_info;		/* live 관리용 session */
	/* streaming type 통게 관련 추가 부분 시작 */
	int				is_live; 	/* mms 통계에서 live와 vod 구분을 위해서 사용, 1:live, 0:vod */
	float      		duration;   /* 전체 재생시간  (초), vod 인 경우 사용 */
	uint64_t		file_size;	/* 미디어 원본 사이즈 */
	/* streaming type 통게 관련 추가 부분 끝 */
	pthread_mutex_t lock;
} session_info_t;


void sm_init();
void sm_deinit();
int sm_module_init();
int sm_module_deinit();
int sm_handle_session(nc_request_t *req);
session_info_t * sm_check_session(nc_request_t *req);
session_info_t * sm_create_session(nc_request_t *req);
void sm_release_session(nc_request_t *req);
void sm_destroy_session(session_info_t * session, int width_lock, int is_write_http_stat);
int sm_create_id();

#endif /*__SESSION_MANAGER_H__ */
