#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <memory.h>
#include <fcntl.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <setjmp.h>

#include <microhttpd.h>
#include <dict.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "soljwt.h"
#include "streaming.h"
#include "status.h"
#include "scx_util.h"
#include "scx_timer.h"
#include "scx_rtmp.h"
#include "thpool.h"



#define MAX_LISTEN_BACKLOG 1024

extern int gscx__service_available;
extern int gscx__signal_shutdown;
extern int gscx__io_buffer_size;
extern int gscx__max_url_len;
extern __thread jmp_buf 	__thread_jmp_buf;
extern __thread int		__thread_jmp_working;

static pthread_mutex_t 	gscx__rtmp_list_lock = PTHREAD_MUTEX_INITIALIZER;
threadpool 	gscx__rtmp_thpool = NULL; 	/* rtmp 메모리 해제 전용 thread pool 포인터 */

int		gscx__rtmp_service_inited = 0;		/* rtmp service가 동작중인 경우 1로 설정 된다. */
rtmp_daemon_t	*gscx_rtmp_daemon = NULL;

static void *rtmp_poling_thread(void *d);
int rtmp_strcmp(const void *k1, const void *k2);
unsigned rtmp_create_hash(const void *p);
void rtmp_key_val_free(void *key, void *datum);
rtmp_ctx_t * rtmp_create_session_ctx(int fd, rtmp_daemon_t *daemon);
static zstr * rtmp_get_argument(nc_request_t *req, rtmpUrl *url, char *name);
int rtmp_destroy_session_ctx(rtmp_ctx_t * ctx);
void rtmp_epoll_add_write_event(void *cr);
void rtmp_epoll_del_write_event(void *cr);
static int rtmp_session_write(unsigned char *block, size_t size, void *param);
static int rtmp_session_before_call(zipperRtmp handler, zipper_rtmp_call call, uint32_t callarg, rtmp_ctx_t *ctx);
static int rtmp_session_set_source(zipperRtmp handler, rtmpUrl *url, zipper_rtmp_source *src, rtmp_ctx_t *ctx);
void rtmp_free_ctx_timer_callback(void *cr);
void rtmp_free_ctx(void *cr);
static int rtmp_create_thread_pool();
static int rtmp_destroy_thread_pool();


int
start_rtmp_service()
{
	int on = 1;
	struct sockaddr_in rtmp_addr;
	const struct sockaddr *servaddr = NULL;
	socklen_t addrlen;
	int	ret = 0;
	int flags;
	pthread_t 	tid;
	pthread_attr_t attr;
	int		listen_fd = -1;
	int 	i;
	rtmp_daemon_t *daemon = NULL;


	if (gscx__config->rtmp_enable != 1) {
		return 0;
	}
	// 소켓 생성
	listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		TRACE((T_ERROR, "RTMP socket() failed, reason : %d(%s)\n", errno, strerror(errno)));
		goto init_socket_fail;
	}
	gscx_rtmp_daemon = SCX_CALLOC(1, sizeof (struct rtmp_daemon_tag));
	gscx_rtmp_daemon->master = NULL;
	gscx_rtmp_daemon->pool_size = gscx__config->rtmp_worker_pool_size;
	gscx_rtmp_daemon->listen_fd = listen_fd;

	gscx_rtmp_daemon->worker_pool = SCX_CALLOC(1, (sizeof (struct rtmp_daemon_tag)) * gscx_rtmp_daemon->pool_size);


	on = 1;
	/* 성능 향상을 위해서 SO_REUSEPORT로 옵션을 변경할수도 있음 */
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&on, sizeof(on)) < 0 ) {
		TRACE((T_WARN, "RTMP setsockopt(SO_REUSEADDR) failed, reason : %d(%s)\n", errno, strerror(errno)));
	}

	on = 1;
    if (setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, (void*)&on, sizeof(on)) < 0 ) {
		TRACE((T_WARN, "RTMP setsockopt(TCP_NODELAY) failed, reason : %d(%s)\n", errno, strerror(errno)));
	}

#ifdef SO_NOSIGPIPE
	on = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&on, sizeof(on)) < 0 ) {
		TRACE((T_WARN, "RTMP setsockopt(SO_NOSIGPIPE) failed, reason : %d(%s)\n", errno, strerror(errno)));
	}
#endif
	rtmp_addr.sin_family = AF_INET;
	rtmp_addr.sin_port = htons(gscx__config->rtmp_port);
	if (gscx__config->listen_ip) {
		//MHD_OPTION_SOCK_ADDR을 사용할 경우 MHD_start_daemon시의 port 옵션이 무시되므로
		//여기에 port 옵션을 같이 설정해 주어야 한다.
		inet_pton(AF_INET,  vs_data(gscx__config->listen_ip), &(rtmp_addr.sin_addr.s_addr));
	}
	else {
		rtmp_addr.sin_addr.s_addr = htonl (INADDR_ANY);
	}
	addrlen = sizeof (struct sockaddr_in);
	servaddr = (struct sockaddr *) &rtmp_addr;

	if (bind (listen_fd, servaddr, addrlen) < 0) {
		TRACE((T_ERROR, "RTMP bind() failed, port : %d, reason : %d(%s)\n", gscx__config->rtmp_port, errno, strerror(errno)));
		goto init_socket_fail;
	}

    if (listen (listen_fd, MAX_LISTEN_BACKLOG) < 0) {
		TRACE((T_ERROR, "RTMP listen() failed, port : %d, reason : %d(%s)\n", gscx__config->rtmp_port, errno, strerror(errno)));
		goto init_socket_fail;
	}
	// server fd Non-Blocking Socket으로 설정. Edge Trigger 사용하기 위해 설정.
	flags = fcntl(listen_fd, F_GETFL);
	flags |= O_NONBLOCK;
	if (fcntl(listen_fd, F_SETFL, flags) < 0) {
		TRACE((T_ERROR, "RTMP fcntl() failed, port : %d, reason : %d(%s)\n", gscx__config->rtmp_port, errno, strerror(errno)));
		goto init_socket_fail;
	}
	rtmp_create_thread_pool();
	gscx__rtmp_service_inited = 1;
	TRACE((T_INFO|T_STAT, "RTMP service successfully launched on port %d\n", gscx__config->rtmp_port));


	/* Start the workers in the pool */
	for (i = 0; i < gscx_rtmp_daemon->pool_size; i++) {
		/* Create copy of the Daemon object for each daemon */
		daemon = (rtmp_daemon_t *)&gscx_rtmp_daemon->worker_pool[i];
		memcpy (daemon, gscx_rtmp_daemon, sizeof (struct rtmp_daemon_tag));
		daemon->master = gscx_rtmp_daemon;
		daemon->pool_size = 0;
		daemon->worker_pool = NULL;
		daemon->order = i;
		daemon->listen_fd = listen_fd;
		daemon->epoll_fd = -1;
		daemon->connection_list = NULL;
		ret = pthread_attr_init (&attr);
		if (ret != 0) {
			TRACE((T_ERROR, "thread_attr_init() failed, reason : %d(%s)\n", errno, strerror(errno)));
			goto init_socket_fail;
		}
		ret = pthread_attr_setstacksize (&attr, 4*1024*1024);
		if (ret != 0) {
			TRACE((T_ERROR, "pthread_attr_setstacksize() failed, reason : %d(%s)\n", errno, strerror(errno)));
			goto init_socket_fail;
		}
		pthread_create(&tid, &attr, &rtmp_poling_thread, (void *)daemon);
		daemon->tid = tid;
	}
	TRACE((T_INFO|T_STAT, "RTMP service Started.\n"));
	return 0;
init_socket_fail:
	stop_rtmp_service();

	TRACE((T_INFO|T_STAT, "RTMP service start Failed.\n"));
	return -1;
}

int
stop_rtmp_service()
{
	int i;
	rtmp_daemon_t *daemon = NULL;

	if (gscx__rtmp_service_inited != 1)	return 0;
	rtmp_destroy_thread_pool();
	if (gscx_rtmp_daemon != NULL) {
		if (gscx_rtmp_daemon->worker_pool != NULL) {
			for (i = 0; i < gscx_rtmp_daemon->pool_size; i++) {
				daemon = (rtmp_daemon_t *)&gscx_rtmp_daemon->worker_pool[i];
				if (daemon->tid != NULL) {
					pthread_join(daemon->tid, NULL);
					daemon->tid = NULL;
				}
			}
			SCX_FREE(gscx_rtmp_daemon->worker_pool);
			gscx_rtmp_daemon->worker_pool = NULL;
		}
		if (gscx_rtmp_daemon->listen_fd > 0) {
			close(gscx_rtmp_daemon->listen_fd);
			gscx_rtmp_daemon->listen_fd = -1;
		}
		SCX_FREE(gscx_rtmp_daemon);
		gscx_rtmp_daemon = NULL;
	}

	TRACE((T_INFO|T_STAT, "RTMP service Stoped.\n"));
	gscx__rtmp_service_inited = 0;
	return 0;
}


/*
 * 주요 epoll event
EPOLLIN 	= 0x001		//수신할 데이터가 존재하는 상황 (EPOLL_CTL의 입력, WAIT 의 출력에 모두 사용됨)
EPOLLPRI = 0x002		//OOB 데이터가 수신된 상황 (EPOLL_CTL의 입력, WAIT 의 출력에 모두 사용됨)
EPOLLOUT = 0x004		//출력버퍼가 비워져서 당장 데이터를 전송할 수 있는 상황 (EPOLL_CTL의 입력, WAIT 의 출력에 모두 사용됨)
EPOLLERR = 0x008		//에러가 발생한 상황 (EPOLL_WAIT의 출력으로만 사용됨)
EPOLLHUP = 0x010		//장애발생 (hangup) (EPOLL_WAIT의 출력으로만 사용됨)
EPOLLRDHUP = 0x2000		//연결이 종료되거나 Half-close 가 진행된 상황, 이는 엣지 트리거 방식에서 유용하게 사용될 수 있다. 상대편 소켓 셧다운 (EPOLL_CTL의 입력, WAIT 의 출력에 모두 사용됨)
EPOLLONESHOT = (1 << 30)	//이벤트가 한번 감지되면, 해당 파일 디스크립터에서는 더 이상 이벤트를 발생시키지 않는다. 따라서 epoll_ctl 함수의 두번째 인자로 EPOLL_CTL_MOD을 전달해서 이벤트를 재설정해야 한다. (EPOLL_CTL의 입력에만 사용됨)
EPOLLET 	= (1 << 31)	//엣지 트리거 방식으로 설정 (EPOLL_CTL의 입력에만 사용됨)
 */
void *
rtmp_poling_thread(void *d)
{
	int num_events;
	int i;
	int tid = gettid();
	struct epoll_event events[MAX_LISTEN_BACKLOG];
	struct epoll_event event;
	int flags;
	int			client_fd;
	socklen_t 	client_addrlen;
	struct sockaddr_in	client_addr;
	rtmp_ctx_t * ctx = NULL;
	int		ret = 0;
	char		data[4096];
	int			read_len;
	int		epoll_fd = -1;
	int		listen_fd = -1;
	int		order = 0;
	dict 	*connection_list = NULL;

	rtmp_daemon_t *daemon = (rtmp_daemon_t *)d;
//	daemon->tid = tid;
	order = daemon->order;
	listen_fd = daemon->listen_fd;
	/*
	 * EPOLL 용 file descriptor를 만든다.
	 * FD_CLOEXEC 플래그는 하나의 프로세스에서 새로운 프로세스를 실행시킬 때 열려있는 fd 를 그대로 넘겨준다.
	 * 새로운 프로세스에 열린 fd 값을 상속시키는 것이 디폴트로 FD_CLOEXEC 플래그를 해제하는 것이고, 0 으로 사용할 수도 있다.
	 * 반대로 새로운 프로세스에 열린 fd 값을 상속시키지 않게 하는 것은 FD_CLOEXEC 플래그를 설정하는 것이고, 1로 사용할 수도 있다.
	 */
	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd  < 0) {
		TRACE((T_ERROR, "epoll_create1() failed, order(%d), reason : %d(%s)\n", order, errno, strerror(errno)));
		return NULL;
	}
	daemon->epoll_fd = epoll_fd;
	// EPOLLET : Edge Trigger 사용
	// 레벨트리거와 에지 트리거를 소켓 버퍼에 대응하면, 소켓 버퍼의 레벨 즉 데이터의 존재 유무를 가지고 판단하는 것이
	// 레벨트리거 입니다.즉 읽어서 해당 처리를 하였다 하더라도 덜 읽었으면 계속 이벤트가 발생하겠지요.예를 들어
	// 1000바이트가 도착했는데 600바이트만 읽었다면 레벨 트리거에서는 계속 이벤트를 발생시킵니다.그러나
	// 에지트리거에서는 600바이트만 읽었다 하더라도 더 이상 이벤트가 발생하지 않습니다.왜냐하면 읽은 시점을 기준으로
	// 보면 더이상의 상태 변화가 없기 때문이죠..LT 또는 ET는 쉽게 옵션으로 설정 가능합니다.
	// 참고로 select / poll은 레벨트리거만 지원합니다.
	event.events			= EPOLLIN ;
	event.data.ptr			= daemon;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) < 0)
	{
		TRACE((T_ERROR, "epoll_ctl() failed, order(%d), reason : %d(%s)\n", order, errno, strerror(errno)));
		return NULL;
	}

    dict_compare_func cmp_func = rtmp_strcmp;
    dict_hash_func hash_func = rtmp_create_hash;

    connection_list = hashtable_dict_new(cmp_func, hash_func, DICT_HSIZE);
    ASSERT(dict_verify(connection_list));
    daemon->connection_list = (void *) connection_list;


	TRACE((T_INFO, "rtmp poling thread started(%d). \n", daemon->order));
	while (gscx__signal_shutdown == 0) {
		num_events = MAX_LISTEN_BACKLOG;
		while (MAX_LISTEN_BACKLOG == num_events) {
			/* update event masks */
			num_events = epoll_wait (epoll_fd,  events, MAX_LISTEN_BACKLOG, 100);	/* 마지막 파라미터를 -1로 해서 무한 대기로 했을 경우 종료시 이곳에서 blocking 되는 경우가 발생한다. */
			if (num_events < 0) {
				if (errno == EINTR) {
					break;
				}
				/* 실패가 발생한 경우 서버 재기동을 해야 하는게 아닌지 모르겠음 */
				TRACE((T_WARN, "epoll_wait() failed, order(%d), reason : %d(%s)\n", order, errno, strerror(errno)));
				break;
			}
		}
		//printf("num_events = %d\n", num_events);
		if (num_events <= 0) {
			continue;
		}
		if (!gscx__service_available) {	/* rtmp port가 열린 상태라도 초기화 과정이 끝나지 않았으면 요청을 거부한다. */
			continue;
		}
		for (i = 0; i < num_events; i++) {
			if (NULL == events[i].data.ptr) {
//					printf("event ptr is NULL\n");
					// shutdown signal ?
				    continue;
			}

			if (daemon == events[i].data.ptr)  {

//				printf("num_events = %d, count = %d, fd(%d), event(0X%X)\n", num_events, i, events[i].data.fd, events[i].events );
//				printf("listen event\n");
				/* RTMP listen socket인 경우(Client socket 연결시)
				/* Check for error conditions on listen socket. */
				if (0 == (events[i].events & (EPOLLERR | EPOLLHUP))) {

					client_addrlen	= sizeof(client_addr);
					client_fd	= accept(listen_fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addrlen);
					if ( (client_fd < 0 ) || (client_addrlen <= 0) ) {
						TRACE((T_DAEMON, "accept() failed, order(%d), reason : %d(%s)\n", order, errno, strerror(errno)));
						/* multi thread에서 동시에 accept를 실행하기 때문에 아래의 에러들이 발생할수 있다. */
						if (EINVAL == errno) {	/* can happen during shutdown */
							continue;
						}
						if (EAGAIN == errno) {
							continue;
						}
						if (ECONNABORTED == errno) {	/* do not print error if client just disconnected early */
							continue;
						}

						if (client_fd > 0) {
							close(client_fd);
						}
						TRACE((T_INFO, "accept() failed, order(%d), reason : %d(%s)\n", order, errno, strerror(errno)));
						continue;
					}
//					printf("User Accept(%d) fd(%d), order(%d)\n", tid, client_fd, order);
					flags = fcntl (client_fd, F_GETFL);
					if (-1 == flags) {
						TRACE((T_WARN, "fcntl(F_GETFL) failed, order(%d), reason : %d(%s)\n", order, errno, strerror(errno)));
						close(client_fd);
						continue;
					}

					flags |= O_NONBLOCK;
					if (fcntl(client_fd, F_SETFL, flags) < 0) {
						TRACE((T_WARN, "fcntl(F_SETFL) failed, order(%d), reason : %d(%s)\n", order, errno, strerror(errno)));
						close(client_fd);
						continue;
					}
					ctx = rtmp_create_session_ctx(client_fd, daemon);
					if (ctx == NULL) {
						/* fd는 rtmp_create_session_ctx()내에서 이미 close가 호출 되기 때문에 여기서 따로 호출 하지 않는다. */
						continue;
					}
					inet_ntop(AF_INET, &client_addr.sin_addr, ctx->client_ip, 32);

					// 클라이언트 fd, epoll 에 등록
					//event.events	= EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLET | EPOLLRDHUP |EPOLLHUP;
					event.events	= EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDHUP |EPOLLHUP;
					//event.data.fd	= client_fd;
					event.data.ptr 	= (void *)ctx;

					if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0)
					{
						printf("client epoll_ctl() error\n");
						rtmp_destroy_session_ctx(ctx);
						continue;
					}
				}
				continue;
			}
			else {

		//		printf("client event\n");


				ctx = (rtmp_ctx_t *)events[i].data.ptr;
				/* listen socket이 아닌 경우는 모두 client와 연결된 socket이라고 본다. */
				client_fd = ctx->fd;
				//printf("num_events = %d, count = %d, fd(%d), event(0X%X)\n", num_events, i, ctx->fd, events[i].events );
				if (0 != (events[i].events & (EPOLLPRI | EPOLLERR | EPOLLHUP))) {
					/* socket 문제가 생긴걸로 판단해서 연결을 종료 시킨다.	 */
//					printf("socket exception event(0X%X), fd(%d)\n", events[i].events);
					TRACE((T_DAEMON, "socket exception event(0X%X)\n", events[i].events, ctx->fd));
					rtmp_destroy_session_ctx(ctx);
				}
				else if (0 != (events[i].events & EPOLLRDHUP)) {
//					printf("socket disconnect event(%d)\n", events[i].events);
					TRACE((T_DAEMON, "socket disconnect event(0X%X)\, fd(%d)n", events[i].events, ctx->fd));
					rtmp_destroy_session_ctx(ctx);
				}
				else if (0 != (events[i].events & EPOLLIN)) {
					//						printf("socket read event(0X%X)\n", events[i].events);
					TRACE((T_DAEMON, "socket read event(0X%X)\n", events[i].events));
					// 입력 버퍼에 데이터가 남아 있는 동안,...
					while((read_len = read(client_fd, &data, sizeof(data)-1)) > 0) {
						//					    	printf("read size = %d\n",  read_len);
						if  (sigsetjmp(__thread_jmp_buf, 1) == 0) {
							__thread_jmp_working = 1;
							// zipperRtmp 컨텍스트에 넘겨준다.
							ret = zipper_rtmp_process(ctx->ioh, ctx->handler, data, read_len);
							__thread_jmp_working = 0;
						}
						else {
							/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
							ret = zipper_err_internal;
							TRACE((T_ERROR, " zipper_rtmp_process() SIGSEGV occured.%s:%d\n", __FILE__, __LINE__));
						}
						if(ret!= zipper_err_success) {
#ifdef DEBUG
							//printf("zipper_rtmp_process error ret = %d\n", ret);
#endif
							rtmp_destroy_session_ctx(ctx);
							ctx = NULL;
							break;
						}

						//printf("zipper_rtmp_process ret = %d\n", ret);

					}
					/* read event가 발생한 경우에는 write가 항상 필요하기 때문에 rtmp_epoll_add_write_event()를 호출 한다. */
					if (ctx != NULL) rtmp_epoll_add_write_event((void *)ctx);
				}
				else if (0 != (events[i].events & EPOLLOUT)) {
					//printf("socket write event(%d) \n", events[i].events);
					TRACE((T_DAEMON, "socket write event(0X%X)\n", events[i].events));
					if (ctx->last_event < zipper_rtmp_call_play) {
						/* socket이 연결되고 2초내에 play 요청이 들어오지 않는다면 문제가 있는것으로 보고 소켓을 강제로 끊도록 한다. */
						if ((ctx->connect_time + 2000000.0) < scx_update_cached_time_usec()) {
							TRACE((T_DAEMON, "Delayed rtmp play command. fd(%d)\n", ctx->fd));
							rtmp_destroy_session_ctx(ctx);
							continue;
						}
					}
					/*  라이브 미리보기 기능 처리 부분 */
					if (ctx->req != NULL) {
						if (ctx->req->service->streaming_method == 2 && ctx->req->streaming->content->end != EOF) {
							if ((ctx->connect_time + (ctx->req->streaming->content->end * 1000.0)) < scx_update_cached_time_usec()) {
								TRACE((T_DAEMON, "[%llu] '%s' Preview finished.\n", ctx->req->id, vs_data(ctx->req->url)));
								rtmp_destroy_session_ctx(ctx);
								continue;
							}
						}
					}
					if  (sigsetjmp(__thread_jmp_buf, 1) == 0) {
						__thread_jmp_working = 1;
						ret = zipper_rtmp_process(ctx->ioh, ctx->handler, NULL, 0);
						__thread_jmp_working = 0;
					}
					else {
						/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
						ret = zipper_err_internal;
						TRACE((T_ERROR, " zipper_rtmp_process() SIGSEGV occured.%s:%d\n", __FILE__, __LINE__));
					}
					TRACE((T_DEBUG, "zipper_rtmp_process write event return(%d), fd(%d) \n", ret, ctx->fd));
					// zipperRtmp 컨텍스트에 넘겨준다.
					if(ret != zipper_err_success) {
						if (ret == zipper_err_need_more) {
							/*
							 * 속도 제어나 pause 요청이 들어온 경우
							 * socket은 write 가능 */
						//	bt_msleep(100);
#if 0
							event.events	= EPOLLIN  | EPOLLPRI | EPOLLRDHUP |EPOLLHUP;
							//event.data.fd	= client_fd;
							event.data.ptr 	= (void *)ctx;

							if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->fd, &event) < 0)
							{
								printf("client epoll_ctl() error\n");
								rtmp_destroy_session_ctx(ctx);
								continue;
							}
#else
							rtmp_epoll_del_write_event((void *)ctx);
#endif
							bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)ctx->socket_timer, 1 , rtmp_epoll_add_write_event, ctx);

//							printf("zipper_rtmp_process write event return zipper_err_need_more. fd(%d)\n", ctx->fd);
							continue;
						}
						else if (ret == zipper_err_eof) {
							/*
							 * VOD애서 socket은 연결 되어 있지만 전송은 모두 끝낸 경우나
							 * 라이브에서 오리진에 문제가 있는 경우이다.
							 * write event만 무시하도록 하고 timer 에 등록을 하지는 않는다.
							 */
							rtmp_epoll_del_write_event((void *)ctx);
							if (ctx->req->service->streaming_method == 2) {
								/* rtmp 라이브 서비스 인 경우 client와의 연결을 강제 종료 한다. */
								rtmp_destroy_session_ctx(ctx);
							}
							TRACE((T_DEBUG, "zipper_rtmp_process write event return zipper_err_eof. fd(%d)\n", ctx->fd));
//							rtmp_destroy_session_ctx(ctx);
							continue;
						}
						TRACE((T_DEBUG, "zipper_rtmp_process write event ret = %d\n", ret));
						rtmp_destroy_session_ctx(ctx);
						continue;
					}
				}
				else {
//					printf("unknown socket event(0X%X)\n", events[i].events);
					TRACE((T_WARN, "unknown socket event(0X%X)\n", events[i].events));
					/*
					 * 현재 여기 까지 오는 조건이 어떤 경우인지를 몰라서 이렇게 처리함.
					 * 개발 완료후 아래 부분 제거 해야함.
					 */
#pragma message("RTMP fix me.!!")
					ASSERT(0);

				}

			}
		}	// endof 'for (i = 0; i < num_events; i++)'
	}

	if (connection_list) {
		dict_itor 	*itor = NULL;
		int	dict_count = 0;
		TRACE((T_DAEMON, "rtmp session list delete start, thread(%d), list count(%d), session count(%d) \n", daemon->order, dict_count(connection_list), daemon->connection_count));
		itor = dict_itor_new(connection_list);
		dict_itor_first(itor);
		/* 여기서 list에 있는 모든 client가 모두 종료 되는지 확인해야됨. */
		while (dict_itor_valid(itor)) {
			ctx = (rtmp_ctx_t *)*dict_itor_datum(itor);
			dict_itor_next(itor);	/* rtmp_destroy_session_ctx()에서 list에서 삭제하는 부분이 있어서 미리 다음 itor의 포인터를 받아온다. */
			rtmp_destroy_session_ctx(ctx);
			dict_count++;

		}
		dict_itor_free(itor);

		dict_free(connection_list, rtmp_key_val_free);
		connection_list = NULL;
		TRACE((T_DAEMON, "rtmp session list deleted, thread(%d), list count(%d), session count(%d). \n", daemon->order, dict_count, daemon->connection_count));
	}
	if (daemon->epoll_fd != -1) {
		close(daemon->epoll_fd);
		daemon->epoll_fd = -1;
	}
	TRACE((T_INFO, "rtmp poling thread stoped(%d). \n", daemon->order));

	return NULL;
}


/*
 * epoll socket서 write event를 인식하도록 추가
 * 이 함수가 호출되는 경우 :
 * 	   read socket event가 발생한 경우
 * 	   전송 속도 제어에서 일정시간동안 sleep한 후 다시 전송 하는 경우
 */

void
rtmp_epoll_add_write_event(void *cr)
{
	rtmp_ctx_t * ctx = (rtmp_ctx_t *)cr;
	struct epoll_event event;
	event.events	= EPOLLIN  | EPOLLOUT | EPOLLPRI | EPOLLRDHUP |EPOLLHUP;
	event.data.ptr 	= (void *)ctx;
//	printf("rtmp_epoll_add_write_event calling. fd(%d)\n", ctx->fd);
	if (epoll_ctl(ctx->daemon->epoll_fd, EPOLL_CTL_MOD, ctx->fd, &event) < 0)
	{

		TRACE((T_DAEMON, "epoll_ctl() failed(%d), reason(%s)\n", ctx->fd, strerror(errno)));

	}
//	printf("rtmp_epoll_add_write_event called. fd(%d)\n", ctx->fd);
}


/*
 * epoll socket에서 write event가 발생하지 않도록 제거
 * 이 함수가 호출되는 경우 :
 * 	   플레이어에서 재생이 완료된후 socket만 연결된 경우
 * 	   전송 속도 제어후에 한동안 전송이 불필요한 경우
 */
void
rtmp_epoll_del_write_event(void *cr)
{
	rtmp_ctx_t * ctx = (rtmp_ctx_t *)cr;
	struct epoll_event event;
	event.events	= EPOLLIN  | EPOLLPRI | EPOLLRDHUP |EPOLLHUP;	/* EPOLLOUT을 제거 */
	event.data.ptr 	= (void *)ctx;

	if (epoll_ctl(ctx->daemon->epoll_fd, EPOLL_CTL_MOD, ctx->fd, &event) < 0)
	{

		TRACE((T_DAEMON, "epoll_ctl() failed(%d), reason(%s)\n", ctx->fd, strerror(errno)));

	}
//	printf("rtmp_epoll_del_write_event called. fd(%d)\n", ctx->fd);
}


int
rtmp_strcmp(const void *k1, const void *k2)
{
    return dict_ptr_cmp(k1, k2);
}


unsigned
rtmp_create_hash(const void *p)
{
    // return dict_str_hash(p); // session id가 random int를 사용하기 때문에 별도의 hashing은 필요 없다.
	//unsigned hash = atoi(p);
	unsigned hash = (unsigned)p;
	return hash;
}


void
rtmp_key_val_free(void *key, void *datum)
{

}



/*
 *
 */
rtmp_ctx_t *
rtmp_create_session_ctx(int fd, rtmp_daemon_t *daemon)
{
	rtmp_ctx_t	*ctx = NULL;
	void **datum_location = NULL;
	int ret = 0;

	ctx = SCX_CALLOC(1, sizeof(rtmp_ctx_t));

	ctx->fd = fd;
	ctx->daemon = daemon;
	ctx->req = NULL;
	ctx->ioh = strm_set_io_handle(NULL);
	ctx->ioh->bufsize = gscx__io_buffer_size;
	ctx->ioh->readbuf.data = (unsigned char *)SCX_MALLOC(ctx->ioh->bufsize);
	ctx->ioh->reader.param = NULL;
	ctx->ioh->writer.fp = rtmp_session_write;
	ctx->ioh->writer.param = ctx;
	ctx->connect_time = sx_get_time();
	ctx->last_event = -1;

	ctx->socket_timer = (void *)SCX_CALLOC(1, sizeof(bt_timer_t));
	bt_init_timer(ctx->socket_timer, "socket control timer", 0);

    // zipperRtmp 컨텍스트를 생성한다. /////////////////
    zipper_create_rtmp_param zcrp;

    memset(&zcrp, 0, sizeof(zipper_create_rtmp_param));
    zcrp.callback.param = ctx;
    zcrp.callback.src = rtmp_session_set_source;
    zcrp.callback.call = rtmp_session_before_call;
    zcrp.config.rc = 1;		// 1이면, 전송 Rate Control 동작(VOD)
    zcrp.config.chunksize = 4096;	// RTMP 패킷 단위 전송 크기(최대값 > 4096), bytes 기본값은 4096
    if  (sigsetjmp(__thread_jmp_buf, 1) == 0) {
    	__thread_jmp_working = 1;
    	ret = zipper_create_rtmp_context(ctx->ioh, &ctx->handler, &zcrp);
    	__thread_jmp_working = 0;
    }
    else {
    	/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
    	ret = zipper_err_internal;
    	TRACE((T_ERROR, "zipper_create_rtmp_context() SIGSEGV occured.%s:%d\n", __FILE__, __LINE__));
    }
    if(ret != zipper_err_success) {

        TRACE((T_WARN, "zipper_create_rtmp_context() failed)\n"));
        rtmp_destroy_session_ctx(ctx);
        return NULL;
    }

	dict_insert_result result = dict_insert((dict *)daemon->connection_list, (void *)ctx->fd);
	if (result.inserted) {
		ASSERT(result.datum_ptr != NULL);
		ASSERT(*result.datum_ptr == NULL);
		*result.datum_ptr =  (void *)ctx;
	}

	daemon->connection_count++;

    TRACE((T_DAEMON, "rtmp session created fd(%d)\n", ctx->fd));
	return ctx;

}


int
rtmp_destroy_session_ctx(rtmp_ctx_t * ctx)
{
	ASSERT(ctx);
	rtmp_daemon_t *daemon = ctx->daemon;
	nc_request_t *req = ctx->req;
	int	connect_type = SCX_RTMP;

	TRACE((T_DAEMON, "rtmp session destroy fd(%d)\n", ctx->fd));
	dict_remove((dict *)ctx->daemon->connection_list, (void *)ctx->fd);
	if (ctx->socket_timer) {

#ifdef BT_TIMER_VER2
		while (bt_del_timer_v2(gscx__timer_wheel_base, (bt_timer_t *)ctx->socket_timer) < 0) {
			bt_msleep(1);
		}
#else
		bt_del_timer(gscx__timer_wheel_base, (bt_timer_t *)ctx->socket_timer);
#endif
	}
	if(ctx->ioh) {
		if(ctx->handler) {
			zipper_free_rtmp_context(ctx->ioh, &ctx->handler);
			ctx->handler = NULL;
		}
		strm_destroy_io_handle(ctx->ioh);
		ctx->ioh = NULL;
	}
	if (ctx->fd > 0) {
//		printf("socket close(%d)\n", ctx->fd);
		TRACE((T_DAEMON, "socket close(%d)\n", ctx->fd));

	    epoll_ctl(ctx->daemon->epoll_fd , EPOLL_CTL_DEL, ctx->fd, NULL);
		close(ctx->fd);
		ctx->fd = -1;
	}

	if (ctx->req != NULL) {
		if (ctx->req->resultcode == 0) ctx->req->resultcode = MHD_HTTP_BAD_REQUEST;
		if (ctx->req->service != NULL) {
			session_update_service_end_stat(ctx->req->service);
		}
		nc_request_completed((void *)connect_type, NULL, &ctx->req, 0);
		ctx->req = NULL;
	}


	/*
	 * timer에 job이 등록된 경우 bt_del_timer()를 호출 하더라도 timer에 등록된 함수가 callback 되는 경우가 발생한다.
	 * 이를 회피하기 위해 timer에 등록된 상태이면 메모리 free를 바로 하지 않고 timer에 다시 등록해서 지연후 free 되도록 한다.
	 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/33126
	 */
	bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)ctx->socket_timer, 1 , rtmp_free_ctx, ctx);
	daemon->connection_count--;
	return 0;

}

/*
 * 메모리 해제를 위해서 timer에 등록된 경우 호출 되는 함수
 * 여기서 바로 bt_destroy_timer()를 호출하면 timer callback에서 자신의 timer를 삭제 하는 경우이기 때문에
 * 문제가 발생할수 있어서 thread pool에 job만 등록하고 바로 리턴한다.
 */
void
rtmp_free_ctx_timer_callback(void *cr)
{
	thpool_add_work(gscx__rtmp_thpool, (void *)rtmp_free_ctx, cr);
	return;
}

void
rtmp_free_ctx(void *cr)
{
	rtmp_ctx_t * ctx = (rtmp_ctx_t *)cr;
	rtmp_daemon_t *daemon = ctx->daemon;
	if (ctx->socket_timer) {

#ifdef BT_TIMER_VER2
		while (bt_destroy_timer_v2(gscx__timer_wheel_base, (bt_timer_t *)ctx->socket_timer) < 0) {
			bt_msleep(1);
		}
#else
		bt_destroy_timer(gscx__timer_wheel_base, (bt_timer_t *)ctx->socket_timer);
#endif
		SCX_FREE(ctx->socket_timer);
		ctx->socket_timer = NULL;
	}
	SCX_FREE(ctx);

	return;
}

static int
rtmp_create_thread_pool()
{
	int poolsize = 1; /* 현재 connection 종료시 memory 용도로 사용하기 때문에 1개의 thread만 있어도 된다. */
	gscx__rtmp_thpool = thpool_init(poolsize);
	if (NULL == gscx__rtmp_thpool) {
		TRACE((T_ERROR, "RTMP Thread pool create failed.\n"));
		return -1;
	}
	else {
		TRACE((T_INFO, "RTMP Thread pool created.\n"));
	}

	return 0;
}


static int
rtmp_destroy_thread_pool()
{
	if (NULL == gscx__rtmp_thpool) {
		return 0;
	}
	thpool_destroy(gscx__rtmp_thpool);
	gscx__rtmp_thpool = NULL;
	TRACE((T_INFO, "RTMP Thread pool destoryed.\n"));
	return 0;
}

//////// 라이브러리 패킷 송신 요청 콜백 ////////////////////
static int
rtmp_session_write(unsigned char *block, size_t size, void *param)
{
	rtmp_ctx_t	*ctx = (rtmp_ctx_t	*) param;
	int	ret = zipper_err_success;
	int	write_ret = 0;
	int	using_j_enable = 0;
	nc_request_t *req = ctx->req;
	ssize_t 			copied = 0;
#ifdef DEBUG
	if (size > 10000) {
		printf("size too large(%d)\n", size);
	}

//	printf("[%llu] rtmp_session_write() called(%d), fd = (%d)\n", req?req->id:0LL, size, ctx->fd);
#endif
	if (__thread_jmp_working == 1) {
		/* zipper library를 벗어나서 SIGSEGV 발생하는 경우는 죽어야 하므로 gscx__j_enable을 0으로 바꾼다. */
		using_j_enable = 1;
		__thread_jmp_working = 0;
	}

	while (copied < size ) {
		write_ret = write (ctx->fd, block+copied, size);
		if (write_ret <= 0) {
			// 실패 시 오류 리턴
			// 호출 되는 시점에 따라 ctx->req가 NULL인 경우가 있어서 아래 처럼 예외 처리 추가
			TRACE((T_DAEMON, "[%llu] rtmp_session_write() failed, reason : %d(%s)\n", req?req->id:0LL, errno, strerror(errno)));
#ifdef DEBUG
			printf("rtmp_session_write() failed, reason : %d(%s)\n", errno, strerror(errno));
#endif
			ret = zipper_err_io_handle;
			break;
		}
		if (write_ret != size) {
			// 호출 되는 시점에 따라 ctx->req가 NULL인 경우가 있어서 아래 처럼 예외 처리 추가
			TRACE((T_INFO, "[%llu] rtmp_session_write() write size mismatch. size(%d), sendto(%d)\n", req->id, size, write_ret));
#ifdef DEBUG
			printf("[%llu] rtmp_session_write() write size mismatch. size(%d), sendto(%d)\n", req?req->id:0LL, size, write_ret);
#endif
		}
		copied += write_ret;
		if (req != NULL) {
			scx_update_res_body_size(req, write_ret);
		}

	}
	if(using_j_enable == 1) {
		__thread_jmp_working = 1;
	}
    // 패킷 전송을 수행한다.
    return ret;
}

/*
 * client로 부터 event(command)를 수신 했을때 콜백 되는 함수.
 */
//////// 라이브러리 RPC 호출 콜백 ////////////////////////
static int
rtmp_session_before_call(zipperRtmp handler, zipper_rtmp_call call, uint32_t pos, rtmp_ctx_t *ctx)
{
	nc_request_t *req = NULL;
	if (call == zipper_rtmp_call_play) {
		req = ctx->req;
		if (req->t_res_fbs == 0) {
			req->t_res_fbs = sx_get_time();		/* play event를 처음 받은 시점을 response header 전송시작 시간(first byte response time)으로 한다. */
		}
	}
	ctx->last_event = (int)call;
	TRACE((T_DEBUG, "rtmp_session_before_call() called(%s), arg(pos) = %u, fd = (%d)\n", zipper_rtmp_call_string(call), pos, ctx->fd));
#ifdef DEBUG
	//printf("rtmp_session_before_call() called(%s), arg(pos) = %u, fd = (%d)\n", zipper_rtmp_call_string(call), pos, ctx->fd);
#endif
    return zipper_err_success;
}


/*
 * 라이브러리 패킷 소스 설정 콜백
 * 여기에서 nc_create_request()을 호출 해서 nc_request_t * 생성하고 streaming_t* 생성 및 볼륨 결정까지 해야 한다.
 * referer 차단기능 구현시 url->pageUrl을 사용해서 구현하면 됨
url 파라미터 예
(gdb) p *url
$1 = {playUrl = {src = {
      val = 0x7fffa0003718 "rtmp://192.168.110.203:1935/rtmp/mp4/mp4:Big_Buck_Bunny_HD?myarg=arg1&extarg=arg2", len = 81}, host = {val = 0x0, len = 0}, app = {
      val = 0x7fffa0003734 "rtmp/mp4/mp4:Big_Buck_Bunny_HD?myarg=arg1&extarg=arg2",
      len = 4}, path = {val = 0x7fffa00037e8 "mp4/Big_Buck_Bunny_HD.mp4", len = 25},
    param = {{name = {val = 0x7fffa0003753 "myarg=arg1&extarg=arg2", len = 5}, value = {
          val = 0x7fffa0003759 "arg1&extarg=arg2", len = 4}}, {name = {
          val = 0x7fffa000375e "extarg=arg2", len = 6}, value = {
          val = 0x7fffa0003765 "arg2", len = 4}}, {name = {val = 0x0, len = 0}, value = {
          val = 0x0, len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0,
          len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0, len = 0}}, {
        name = {val = 0x0, len = 0}, value = {val = 0x0, len = 0}}, {name = {val = 0x0,
          len = 0}, value = {val = 0x0, len = 0}}, {name = {val = 0x0, len = 0}, value = {
          val = 0x0, len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0,
          len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0, len = 0}}}},
  swfUrl = {val = 0x0, len = 0}, pageUrl = {val = 0x0, len = 0}}

(gdb) p *url
$3 = {playUrl = {src = {
      val = 0x7fffb80e6cc8 "rtmp://192.168.110.203:1935/rtmp/mp4/mp4:Big_Buck_Bunny_HD.mp4", len = 62}, host = {val = 0x0, len = 0}, app = {
      val = 0x7fffb80e6ce4 "rtmp/mp4/mp4:Big_Buck_Bunny_HD.mp4", len = 4}, path = {
      val = 0x7fffb80d09c8 "mp4/Big_Buck_Bunny_HD.mp4", len = 25}, param = {{name = {
          val = 0x0, len = 0}, value = {val = 0x0, len = 0}}, {name = {val = 0x0,
          len = 0}, value = {val = 0x0, len = 0}}, {name = {val = 0x0, len = 0}, value = {
          val = 0x0, len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0,
          len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0, len = 0}}, {
        name = {val = 0x0, len = 0}, value = {val = 0x0, len = 0}}, {name = {val = 0x0,
          len = 0}, value = {val = 0x0, len = 0}}, {name = {val = 0x0, len = 0}, value = {
          val = 0x0, len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0,
          len = 0}}, {name = {val = 0x0, len = 0}, value = {val = 0x0, len = 0}}}},
    swfUrl = {
          val = 0x7fff70043108 "https://www.wowza.com/demos/testplayers/FlashRTMPPlayer/live.swf", len = 64},
    pageUrl = {val = 0x7fff70043178 "https://www.wowza.com/testplayers",len = 33}
    }
 */
static int
rtmp_session_set_source(zipperRtmp handler, rtmpUrl *url, zipper_rtmp_source *src, rtmp_ctx_t *ctx)
{
	char * tempstr;
	int	connect_type = SCX_RTMP;
	nc_request_t *req = NULL;
	static const char method[] = "GET";
	static const char protocol[] = "RTMP/3";
	streaming_t	 *streaming = NULL;
	content_info_t 	*content = NULL;
	media_info_t 	*media = NULL;
	char * req_full_uri = NULL;	/* RTMP 요청시 사용된 원래 URL(주소 포함) */
	char * req_uri = NULL;	/* RTMP 요청시 사용된 URL(주소를 제외한 path+argument) */
	char * req_volume = NULL;	/* 요청에 들어 있는 첫번째 디렉토리 경로(볼륨) */
	char * req_path = NULL; 	/* 요청에 들어 있는 content path */
	int	param_len = 0;
	int	alloc_len = 0;
	zstr *arg_zstr = NULL;
	char *dur_str = NULL;
	int	 arg_dur = 0;
	int ret = 0;
	int	using_j_enable = 0;
	size_t     nmatch = 10;
	regmatch_t pmatch[10];
	unsigned char *arg_start = NULL;


	if (__thread_jmp_working == 1) {
		/* zipper library를 벗어나서 SIGSEGV 발생하는 경우는 죽어야 하므로 gscx__j_enable을 0으로 바꾼다. */
		using_j_enable = 1;
		__thread_jmp_working = 0;
	}
	req_full_uri = alloca(url->playUrl.src.len + 1);
	memcpy(req_full_uri, url->playUrl.src.val, url->playUrl.src.len);
	req_full_uri[url->playUrl.src.len] = '\0';
	TRACE((T_DAEMON, "RTMP request URL(%s)\n", url->playUrl.src.val));
//	printf("request URL(%s)\n swfUrl(%s)\n", url->playUrl.src.val, url->swfUrl.val);

	req_volume = alloca(url->playUrl.app.len + 1);
	memcpy(req_volume, url->playUrl.app.val, url->playUrl.app.len);
	req_volume[url->playUrl.app.len] = '\0';
	TRACE((T_DAEMON, "RTMP request application(%s)\n", req_volume));
//	printf("req_volume = %s\n", req_volume);
	req_path = alloca(url->playUrl.path.len + 1);
	memcpy(req_path, url->playUrl.path.val, url->playUrl.path.len);
	req_path[url->playUrl.path.len] = '\0';

	if (url->playUrl.param[0].name.len > 0) {
		/* argument가 있는 경우에는 /url->app/url->path?url->param의 형태로 사용한다.
		 * 이 경우url->param[0].name.val 값은 NULL terminate 되지 않은 값이기 때문에
		 * url->param[0].name.len을 무시하고 strlen을 사용해서문자열의 크기를 알아낸후 사용하면 된다.
		 */
		param_len = strlen(url->playUrl.param[0].name.val);
		alloc_len = url->playUrl.app.len + url->playUrl.path.len + param_len + 4; /* /? 포함 */
		req_uri = alloca(alloc_len);
		snprintf(req_uri, alloc_len, "/%s/%s?%s", req_volume, req_path, url->playUrl.param[0].name.val);
	}
	else
	{
		/* argument가 없는 경우에는 /url->app/url->path의 형태로 사용한다. */
		alloc_len = url->playUrl.app.len + url->playUrl.path.len + 3;  /* / 포함 */
		req_uri = alloca(alloc_len);
		snprintf(req_uri, alloc_len, "/%s/%s", req_volume, req_path);
	}

	/* 아래에서 URL 전달시 실제 경로를 넘겨줘야 할듯. */
	req = nc_create_request((void *)connect_type, req_uri);
	ctx->req = req;
	req->zmethod = vs_allocate(req->pool, strlen(method), method, 1);
	req->method = HR_UNKNOWN;
	req->host = vs_allocate(req->pool, strlen(req_volume), req_volume, 1);
	req->client_ip = vs_allocate(req->pool, strlen(ctx->client_ip),ctx->client_ip, TRUE);
	req->version = vs_allocate(req->pool, strlen(protocol), protocol, 1);
	TRACE((T_DEBUG, "[%llu] rtmp socket fd(%d), uri(%s)\n", req->id, ctx->fd, req_uri));
	/* jwt 인증에서 사용할 req->path 를 생성 */
	if (SCX_YES != nc_build_request(req)) {
		scx_error_log(req, "URI[%s] - invalid\n", vs_data(req->url));
		req->resultcode = MHD_HTTP_BAD_REQUEST;
		goto session_set_source_error;
	}

	req->service = vm_add(req_volume, 0);
	if (req->service == NULL) {
		scx_error_log(req, "URL[%s] - virtual host not found in header(volume='%s')\n", vs_data(req->url), vs_data(req->host));
		req->resultcode = MHD_HTTP_FORBIDDEN;
		goto session_set_source_error;
	}
	scx_update_volume_service_start_stat(req);
	session_update_service_start_stat(req->service);	//session 통계에 반영을 위해서 호출

	if (req->service->streaming_enable != 1 || req->service->rtmp_service_enable != 1) {
		scx_error_log(req, "URL[%s] rtmp service not allowed.\n", vs_data(req->url));
		req->resultcode = MHD_HTTP_UNAUTHORIZED;
		goto session_set_source_error;
	}
	/* enable_wowza_url이 1로 설정된 경우 여기서 ori_url을 application/instance를 뺀 경로로 바꿔준다. */
	if(req->service->enable_wowza_url == 1) {
		if (strm_wowza_url_manage(req) == 0) {
			/* parameter가 붙은 경우 따로 처리 해주어야 한다. */
			arg_start = strchr(vs_data(req->ori_url), '?');
			if(arg_start != NULL) {
				req_path = alloca((arg_start - vs_data(req->ori_url)) + 1);
				memcpy(req_path , vs_data(req->ori_url), (arg_start - vs_data(req->ori_url)));
				req_path[(arg_start - vs_data(req->ori_url))] = '\0';
			}
			else {
				req_path = alloca(vs_length(req->ori_url)+1);
				memcpy(req_path , vs_data(req->ori_url), vs_length(req->ori_url));
				req_path[vs_length(req->ori_url)] = '\0';
			}
			req->path 		= vs_allocate(req->pool, strlen(req_path), req_path, TRUE); /* read-only*/
#ifdef DEBUG
			printf("rtmp real path = '%s',oriurl = %s\n", req_path, vs_data(req->ori_url));
#endif
		}
	}

	/* base_path 기능도 여기에 들어 가야 한다. */
	streaming = strm_create_streaming(req);

	streaming->protocol = O_PROTOCOL_RTMP;
	streaming->media_mode = O_STRM_TYPE_SINGLE;
	streaming->media_type = MEDIA_TYPE_RTMP;

	streaming->key = (char *) mp_alloc(streaming->mpool, strlen(req_path) + 1);
	sprintf(streaming->key , "%s", req_path);

	/* 미리 보기 요청 확인 */
	arg_zstr = rtmp_get_argument(req, url, ARGUMENT_NAME_DURATION);
	if (arg_zstr != NULL) {
		/* duration argument의 요청 단위는 초(second)임 */
		dur_str = alloca(arg_zstr->len + 1);
		strncpy(dur_str, arg_zstr->val, arg_zstr->len);
		dur_str[arg_zstr->len] = '\0';
		arg_dur = (int)(atof(dur_str) * 1000.0);
		TRACE((T_DEBUG, "[%llu] '%s' duration(%d)\n", req->id, req_uri, arg_dur));
	}

	// 소스 타입 설정
	// src->type = zipper_rtmp_source_vodctx;	// 미디어 컨텍스트 기반 VOD
	//src->type = zipper_rtmp_source_bldr; 		// 빌드 컨텍스트 기반 VOD
	//src->type = zipper_rtmp_source_solCMAF;	// solCMAF 라이브 소스
	if (req->service->streaming_method == 2) {
		/* rtmp 라이브 서비스 인 경우 */
		src->type = zipper_rtmp_source_solCMAF;
		ASSERT(req->service->core->cmaf);
	}
	else {
		/* rtmp VOD 인 경우 */
		src->type = zipper_rtmp_source_bldr;
	}

	// 트랙 출력 설정
	src->bldflag = BLDFLAG_INCLUDE_ALL;
//	if(src->type == zipper_rtmp_source_bldr) {	// VOD 인 경우
		content = (content_info_t *)mp_alloc(streaming->mpool,sizeof(content_info_t));
		ASSERT(content);
		streaming->content = content;
		content->path = (char *) mp_alloc(streaming->mpool,strlen(req_path)+1);
		sprintf(content->path, "%s", req_path);
		content->start = 0;
		if (arg_dur > 0) {
			content->end = arg_dur;
		}
		else {
			content->end = EOF;
		}
//	}
	if(NULL != req->service->soljwt_secret_list)
	{
		/* jwt 인증을 이 위치에서 실행 해야만 jwt 토큰에 duration이 들어 있는 경우 반영이 될수 있다. */
		arg_zstr = rtmp_get_argument(req, url, SOLJWT_TOKEN);
		if (arg_zstr != NULL) {
			req->jwt_token = vs_strndup_lg(req->pool, arg_zstr->val, arg_zstr->len);
			TRACE((T_DEBUG, "[%llu] '%s' jwt token(%s)\n", req->id, req_uri, vs_data(req->jwt_token)));
		}
		else {
			/* jwt 사용으로 설정된 상태에서 요청 url에 token이 없는 경우에는 에러 처리 한다 */
			TRACE((T_DAEMON, "[%llu] empty jwt token(%s)\n", req->id, req_uri));
			scx_error_log(req, "empty jwt token(%s)\n", req_uri);
			req->resultcode = MHD_HTTP_UNAUTHORIZED;
			goto session_set_source_error;
		}

		if (SCX_YES != soljwt_handler(req)) {
			scx_error_log(req, "URL[%s] - soljwt secure check failed.\n", vs_data(req->url));
			req->resultcode = MHD_HTTP_UNAUTHORIZED;
			goto session_set_source_error;
		}
		if (1 == req->expired) {
			scx_error_log(req, "URL[%s] request denied cause of expired token.\n", vs_data(req->url));
			req->resultcode = MHD_HTTP_UNAUTHORIZED;
			goto session_set_source_error;
		}
	}
	if(src->type == zipper_rtmp_source_bldr) {	// VOD 인 경우
		if (strm_create_builder(req, 0) == 0) {
			req->resultcode = MHD_HTTP_NOT_FOUND;
			goto session_set_source_error;
		}
		// 미디어 컨텍스트(zipperCnt) 지정
		src->src.vodctx = streaming->builder->media_list->media->mcontext;
		// 빌더 컨텍스트(zipperBldr) 지정 (overwrite)
		src->src.bldr = streaming->builder->bcontext;
        src->track.audio = 0;
        src->track.video = 0;
	}
	else {	// LIVE 인 경우
		if (strm_cmaf_check_ready(req) == 0) {
			/* cmaf 오리진과 연결이 준비 되지 않은 경우 */
			scx_error_log(req, "CMAF origin not ready. URL[%s]\n", vs_data(req->url));
			goto session_set_source_error;
		}
		/* live인 경우에는 cmaf context만 넘겨준다. */
		src->src.cmaf = req->service->core->cmaf->ctx;

        int i;

        solCMAFMediaInfo minfo;

        for(i = cmafAdaptiveIndexBase; i < cmafAdaptiveIndexMax; i++) {


            ret = solCMAF_alloc_mediaInfo(src->src.cmaf, i, &minfo);
            if(ret == zipper_err_success) {
            	/* streaming->content->path와 각 stream 명과 strstr로 비교 하기 때문에 요청 url이 짧게 들어 오는 경우 엉뚱한 스트림이 나갈수도 있음 */
            	TRACE((T_DAEMON, "[%llu] path = %s, adpat[%u-%s]: %s\n", req->id, streaming->content->path,  i, minfo.trk == MP4_TRACK_VIDEO ? "video" : "audio", minfo.idstr));
                if(strstr(minfo.idstr, streaming->content->path)) {
                    if(minfo.trk == MP4_TRACK_VIDEO) src->track.video = i;
                    else if(minfo.trk == MP4_TRACK_SOUND) src->track.audio = i;
                }
                solCMAF_free_mediaInfo(src->src.cmaf, &minfo);
            }
        }

        if (src->track.video == 0 && src->track.audio == 0) {
        	/*
        	 * 해당 stream을 찾지 못한 경우는 에러를 리턴해야 한다.
        	 */
			scx_error_log(req, "request stream not found. volume='%s', path='%s'\n", vs_data(req->host), streaming->content->path);
			goto session_set_source_error;
        }

	}

	ctx->ioh->reader.param = streaming; /* rtmp_create_session_ctx()에서 strm_set_io_handle()호출시 parameter를 NULL로 했기 때문에 여기서 파라미터를 다시 설정한다. */
	req->resultcode = MHD_HTTP_OK;
	if(using_j_enable == 1) {
		__thread_jmp_working = 1;
	}
	//printf("rtmp_session_set_source() done\n");
	return zipper_err_success;
session_set_source_error:
	if(req->streaming){
		strm_destroy_stream(req);
		req->streaming = NULL;
	}
	if(using_j_enable == 1) {
		__thread_jmp_working = 1;
	}
	return zipper_err_io_handle;
}

/*
 * rtmp source callback 호출시 넘어 오는 rtmpUrl 정보에서 지정된 argument를 검색
 */
static zstr *
rtmp_get_argument(nc_request_t *req, rtmpUrl *url, char *name)
{
	int i;
	int len;

	len = strlen(name);
	for (i = 0; i < RTMP_URL_MAX_PARAM; i++) {
		if (url->playUrl.param[i].name.val == NULL || url->playUrl.param[i].value.val == NULL) continue;
		else if(strncasecmp(name, url->playUrl.param[i].name.val, len) == 0){
			return &url->playUrl.param[i].value.val;
		}
	}

	return NULL;
}

