#ifndef __SCX_RTMP_H__
#define __SCX_RTMP_H__

#include "zipper.h"
#include "zlive.h"
#include "solCMAF.h"
#include "rtmp.h"



typedef struct rtmp_daemon_tag {
	struct rtmp_daemon_tag *master;
	struct rtmp_daemon_tag *worker_pool;	/* master 인 경우가 값이 있고 worker인 경우는 NULL이 들어 간다. */
	size_t pool_size;		/* rtmp daemon pool size, master인 경우가 아니면 0으로 설정된다. */
	int order;				/* thread number */
	pthread_t 	tid;
//	connection list
	int	listen_fd;
	int epoll_fd;
	int connection_count;	/* 현재 접속수 */
	void *connection_list;		/* rtmp_ctx_t * 의 리스트를 보관 */
} rtmp_daemon_t;

typedef struct rtmp_ctx_tag {
	int					fd;				/* hash key로 fd를 사용 */
	nc_request_t 		*req;
	zipperRtmp			handler;
	zipper_io_handle 	*ioh;
	double				connect_time; 	/* 최초 소켓 연결 시간 */
	int					last_event;		/* client로 부터 수신된 마지막 command */
	char 				client_ip[32];
	void 				*socket_timer;	/* 전송 제어에 사용되는 timer */
	rtmp_daemon_t		*daemon;
} rtmp_ctx_t;


int start_rtmp_service();
int stop_rtmp_service();


#endif /* __SCX_RTMP_H__ */
