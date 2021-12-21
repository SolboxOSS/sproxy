#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <trace.h>
#include <hash.h>
#include <disk_io.h>
#include "util.h"
#include "hash.h"
#include "disk_io.h"
#include "bt_timer.h"
#include "tlc_queue.h"
#include "threadpool.h"
#define MAX_IDLE_TIME 		(60) /* sec */

extern int __terminating;

#define 	TP_MAGIC_BARRIER 	0x5AA5A55A5AA5A55A
typedef struct tag_threadpool  {

	nc_uint64_t 		magic;
	char 				*name;
	int					intg;	/* 1: thread-pool에서 queue에서 job 자체스케줄링, 0: 자체스케줄링 안함*/
	int					min_threads;
	int					max_threads;
	tp_job_handler_t 	handler;
	tp_isidle_t 		isidle;
	tp_epilog_t			epilog;
	tp_prolog_t			prolog;
	tlc_queue_t			wait_queue;
	int 				running;	/* # of running threads */
	int 				busy;		/* # of running threads */
	int 				maxbusy;	/* # of running threads */
	int 				terminating;
	void 				*cbdata;
	pthread_mutex_t		lock;
} threadpool_t;
extern void  *	__timer_wheel_base;

static void  * tp_worker_nq(void *d);
static void  * tp_worker(void *d);


tp_handle_t	
tp_init(const char *name, int mint, int maxt, tp_job_handler_t proc, tp_prolog_t prolog_proc, tp_epilog_t epilog_proc)
{
	threadpool_t 		*p = NULL;
	pthread_mutexattr_t	mattr;

	p = (threadpool_t *)XMALLOC(sizeof(threadpool_t), AI_ETC);
	DEBUG_ASSERT(p != NULL);

	p->name 		= XSTRDUP((char *)name, AI_ETC);
	p->magic 		= TP_MAGIC_BARRIER;
	DEBUG_ASSERT(p->name != NULL);
	p->max_threads 	= maxt;
	p->min_threads 	= mint;
	p->handler 		= proc;
	DEBUG_ASSERT(p->handler != NULL);
	p->epilog 		= epilog_proc;
	p->prolog 		= prolog_proc;
	p->running		= 0;
	p->busy			= 0;
	p->maxbusy		= 0;
	p->terminating	= 0;
	p->intg			= U_TRUE;

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&p->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);

	p->wait_queue 	= tlcq_init_with_lock(U_TRUE, &p->lock); /* waitable concurrent queue */


	return p;

}

/*
 * create threadpool without queue
 */
tp_handle_t	
tp_init_nq(const char *name, int mint, int maxt, tp_job_handler_t proc, tp_isidle_t isidle, tp_prolog_t prolog_proc, tp_epilog_t epilog_proc, void *cxt, int needq)
{
	threadpool_t 		*p = NULL;
	pthread_mutexattr_t	mattr;

	p = (threadpool_t *)XMALLOC(sizeof(threadpool_t), AI_ETC);
	DEBUG_ASSERT(p != NULL);

	p->name 		= XSTRDUP((char *)name, AI_ETC);
	p->magic 		= TP_MAGIC_BARRIER;
	DEBUG_ASSERT(p->name != NULL);
	p->max_threads 	= maxt;
	p->min_threads 	= mint;
	p->handler 		= proc;
	p->isidle 		= isidle;
	DEBUG_ASSERT(p->handler != NULL);
	p->epilog 		= epilog_proc;
	p->prolog 		= prolog_proc;
	p->running		= 0;
	p->busy			= 0;
	p->maxbusy		= 0;
	p->cbdata		= cxt;
	p->terminating	= 0;
	p->intg			= U_FALSE;

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&p->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);

	p->wait_queue 	= NULL;
	if (needq)
		p->wait_queue 	= tlcq_init_with_lock(U_TRUE, &p->lock); /* waitable concurrent queue */


	return p;

}


void  *
tp_worker(void *d)
{
	tp_handle_t 	handle = (tp_handle_t)d;
	void 			*job;
	int 			idle = 0;
	time_t 			idle_start = 0;
	tp_data_t 		tcb = NULL;
#define		NEEDSTOP		(__terminating && handle->terminating && (tlcq_length(handle->wait_queue) == 0))

	TRC_BEGIN((__func__));

	TRACE((T_DEBUG, "thread %ld begins (running=%d, the count may be shortly wrong)\n", trc_thread_self(), handle->running));
	if (handle->prolog) {
		(*handle->prolog)(&tcb);
	}

	while (!NEEDSTOP) {
		job = (void *)tlcq_dequeue(handle->wait_queue, 1000);
		if (!job) {
			/* no job available, indicates idle */
			if (idle == 0) idle_start = nc_cached_clock();

			idle++;
			if (idle > 0 && (nc_cached_clock() - idle_start) > MAX_IDLE_TIME) {
				/*
				 *	idle continued for 60 secs or more
				 *  we need to exit this thread
				 */
				TRACE((T_DEBUG, "thread %ld is idle for more than 60secs(running=%d, busy=%d)\n", trc_thread_self(), handle->running, handle->busy));
				if (handle->running > handle->min_threads) {
					TRACE((0, "idle thread %d, min %d, is going to be shutdowned (current busy = %d/%d)\n", handle->running, handle->min_threads, handle->busy, handle->running));
					break;
				}
				idle = 0;
			}
			continue;
		}
		idle = 0;
		if (!handle->epilog)
			pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		_ATOMIC_ADD(handle->busy, 1);
		handle->maxbusy = max(handle->maxbusy, handle->busy);
		(*handle->handler)(job, tcb);
		_ATOMIC_SUB(handle->busy, 1);
		if (!handle->epilog)
			pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	}


	/*
	 * we need to call epilog handler if available
	 */
	if (handle->epilog) {
		(*handle->epilog)(tcb);
	}
	_ATOMIC_SUB(handle->running, 1);

	TRACE((T_DEBUG, "thread-pool(%p) : thread %ld terminating(running = %d)\n", handle, trc_thread_self(), handle->running));
	TRC_END;
	return NULL;
}
void
tp_set_busy(tp_handle_t h, int busy)
{
	if (busy) 
		_ATOMIC_ADD(h->busy, 1);
	else
		_ATOMIC_SUB(h->busy, 1);
}
static void  *
tp_worker_nq(void *d)
{
	tp_handle_t 	handle = (tp_handle_t)d;
	int 			idle = 0;
	tp_data_t 		tcb = NULL;
	int				processed;
	int 			needexit;
#define		NEEDSTOP		(__terminating && handle->terminating && (tlcq_length(handle->wait_queue) == 0))

	TRC_BEGIN((__func__));

	TRACE((T_DEBUG, "thread %ld begins (running=%d, the count may be shortly wrong)\n", trc_thread_self(), handle->running));
	if (handle->prolog) {
		(*handle->prolog)(&tcb);
	}

	do {
		idle = 0;
		processed 	= (*handle->handler)(handle->cbdata, tcb);
		if (handle->isidle)
			idle 		= (*handle->isidle)(handle->cbdata, tcb);
		needexit = (handle->running > handle->min_threads); 
		TRACE((T_DEBUG, "%s:thread %ld is idle (running=%d, busy=%d, min=%d, max=%d) need_break=%d\n", 
						handle->name, 
						trc_thread_self(), 
						handle->running, 
						handle->busy,
						handle->min_threads,
						handle->max_threads,
						needexit));
	} while ((handle->terminating == 0) && ((needexit == 0) || (processed > 0) || (idle == 0)));

	if (!handle->epilog)
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	if (!handle->epilog)
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);


	/*
	 * we need to call epilog handler if available
	 */
	if (handle->epilog) {
		(*handle->epilog)(tcb);
	}
	_ATOMIC_SUB(handle->running, 1);

	TRACE((T_DEBUG, "%s:thread %ld going to be terminated(running=%d, busy=%d, min=%d, max=%d)\n", 
						handle->name, 
						trc_thread_self(), 
						handle->running, 
						handle->busy,
						handle->min_threads,
						handle->max_threads
						));
	TRC_END;
	return NULL;
}
int 
tp_start_worker(tp_handle_t handle)
{
	int 			vc;
	pthread_attr_t	tattr;
	pthread_t 		tid;

	DEBUG_ASSERT(handle->magic == TP_MAGIC_BARRIER);
	pthread_attr_init(&tattr);

	/* 
	 * making it as detached means that the thread would not be joined any more
	 */
	pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);

	if (handle->intg)
		vc = my_pthread_create(&tid, &tattr, tp_worker, (void *)handle);
	else
		vc = my_pthread_create(&tid, &tattr, tp_worker_nq, (void *)handle);

	pthread_attr_destroy(&tattr);
	if (vc == 0) {
		pthread_setname_np(tid, handle->name);
		pthread_detach(tid);
	}

	return ((vc != 0)? -1:0); /* thread creation failed */

}
int 
tp_length(tp_handle_t handle)
{
	return tlcq_length(handle->wait_queue);
}
void *
tp_fetch(tp_handle_t handle)
{
	void	*p = NULL;
	if (__terminating) return NULL;


	p =  tlcq_dequeue(handle->wait_queue, 0);
	return p;

}
void 
tp_submit(tp_handle_t handle, void *data)
{
	int		runthr, busythr;
	if (__terminating) return;

	DEBUG_ASSERT(handle->magic == TP_MAGIC_BARRIER);

	pthread_mutex_lock(&handle->lock);

	tlcq_enqueue(handle->wait_queue, (void *)data);
	if ( handle->running >= handle->max_threads)
		goto L_submit_done;

	runthr 	= _ATOMIC_ADD(handle->running, 0);
	busythr = _ATOMIC_ADD(handle->busy, 0);
	if (runthr - busythr <= 1)  {
		/* start one more thread */
		if (tp_start_worker(handle) < 0) {
			TRACE((T_ERROR, "failed to create a worker thread:%s\n", strerror(errno)));
		}
		else {
			TRACE((T_TRACE, "starting a worker thread ok\n"));
			_ATOMIC_ADD(handle->running, 1);
		}
	}
L_submit_done:
	pthread_mutex_unlock(&handle->lock);
}

void 
tp_check_busy_nq(tp_handle_t handle)
{
	int		runthr, busythr;
	if (__terminating) return;

	DEBUG_ASSERT(handle->magic == TP_MAGIC_BARRIER);

	pthread_mutex_lock(&handle->lock);

	if (handle->running >= handle->max_threads)
		goto L_submit_done;

	runthr 	= _ATOMIC_ADD(handle->running, 0);
	busythr = _ATOMIC_ADD(handle->busy, 0);
	if (runthr - busythr <= 4)  {
		/* start one more thread */
		if (tp_start_worker(handle) < 0) {
			TRACE((T_ERROR, "failed to create a worker thread:%s\n", strerror(errno)));
		}
		else {
			TRACE((T_TRACE, "starting a worker thread ok\n"));
			_ATOMIC_ADD(handle->running, 1);
		}
	}
L_submit_done:
	pthread_mutex_unlock(&handle->lock);
}


int
tp_start(tp_handle_t handle)
{
	int 	i;
	TRC_BEGIN((__func__));
	
	// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	for( i = 0; i < handle->min_threads; i++ ) {
		if (tp_start_worker(handle) < 0) {
			TRACE((T_ERROR, "failed to create %d-th/%d thread\n", i, handle->min_threads));
		}
		else {
			TRACE((T_TRACE, "starting %d-th/%d thread ok\n", i, handle->min_threads));
			_ATOMIC_ADD(handle->running, 1);
		}
	}
	TRC_END;
	return 0;
}
void
tp_stop(tp_handle_t handle)
{
	nc_time_t		to = nc_cached_clock() + 5;
	handle->terminating = U_TRUE;

	while ((handle->running > 0) && (to > nc_cached_clock())) {
		bt_msleep(1);
	}
	if (handle->wait_queue)
		tlcq_destroy(handle->wait_queue);
	if (handle->name) XFREE(handle->name);
	TRACE((T_INFO, "thread_pool %p - confirmed all threads shutdowned(%s)\n", 
					handle,
					((to <= nc_cached_clock())?"TIMEOUT":"OK")
					));
	XFREE(handle);
}
int
tp_stop_signalled(tp_handle_t handle)
{
	return handle->terminating;
}
int
tp_get_workers(tp_handle_t handle)
{
	return _ATOMIC_ADD(handle->running, 0);
}

// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
//                - 원부사장님 수정 소스 적용
int tp_get_configured( tp_handle_t handle )
{
	return handle->max_threads;
}

pthread_mutex_t *
tp_lock(tp_handle_t handle)
{
	return &handle->lock;
}

