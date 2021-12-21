#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

typedef int (*tp_job_handler_t)(void *data, void *tcb);
typedef void (*tp_stop_handler_t)(void);
typedef void (*tp_epilog_t)(void *p);
typedef void (*tp_prolog_t)(void *p);
typedef int (*tp_isidle_t)(void *cddata, void *tcb);
struct tag_threadpool;
typedef struct tag_threadpool * tp_handle_t;

tp_handle_t	tp_init(const char *name, int mint, int maxt, tp_job_handler_t proc, tp_prolog_t prolog_proc, tp_epilog_t epilog_proc);
tp_handle_t	tp_init_nq(const char *name, int mint, int maxt, tp_job_handler_t proc, tp_isidle_t idle_proc, tp_prolog_t prolog_proc, tp_epilog_t epilog_proc, void *cxt, int needq);
int tp_start_worker(tp_handle_t handle);
void tp_stop(tp_handle_t handle);
void tp_submit(tp_handle_t handle, void *data);
int tp_start(tp_handle_t handle);
int tp_length(tp_handle_t handle);
int tp_get_workers(tp_handle_t handle);
void tp_check_busy_nq(tp_handle_t handle);
void tp_set_busy(tp_handle_t h, int busy);
void * tp_fetch(tp_handle_t handle);
int tp_stop_signalled(tp_handle_t handle);
pthread_mutex_t * tp_lock(tp_handle_t handle);

// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
//                - 원부사장님 수정 소스 적용
int tp_get_configured( tp_handle_t handle );

#endif /* __THREADPOOL_H__*/
