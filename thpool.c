/* ********************************
 * Author:       Johan Hanssen Seferidis
 * License:	     MIT
 * Description:  Library providing a threading pool where you can add
 *               work. For usage, check the thpool.h file or README.md
 *
 *//** @file thpool.h *//*
 * 
 ********************************/


#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h> 
#include <sys/prctl.h>

#include "common.h"
#include "scx_util.h"
#include "thpool.h"
#include "httpd.h"

#define MAX_NANOSEC 999999999
#define CEIL(X) ((X-(int)(X)) > 0 ? (int)(X+1) : (int)(X))

static volatile int threads_keepalive;
static volatile int threads_on_hold;

extern int	gscx__max_job_queue_len;



/* ========================== STRUCTURES ============================ */


/* Binary semaphore */
typedef struct bsem {
	pthread_mutex_t mutex;
	pthread_cond_t   cond;
	int v;
} bsem;


/* Job */
typedef struct job{
	struct job*  prev;                   /* pointer to previous job   */
	void*  (*function)(void* arg);       /* function pointer          */
	void*  arg;                          /* function's argument       */
} job;


/* Job queue */
typedef struct jobqueue{
	pthread_mutex_t rwmutex;             /* used for queue r/w access */
	job  *front;                         /* pointer to front of queue */
	job  *rear;                          /* pointer to rear  of queue */
	bsem *has_jobs;                      /* flag as binary semaphore  */
	int   len;                           /* number of jobs in queue   */
} jobqueue;


/* Thread */
typedef struct thread{
	int       id;                        /* friendly id               */
	pthread_t pthread;                   /* pointer to actual thread  */
	struct thpool_* thpool_p;            /* access to thpool          */
} thread;


/* Threadpool */
typedef struct thpool_{
	thread**   threads;                  /* pointer to threads        */
	volatile int num_threads_alive;      /* threads currently alive   */
	volatile int num_threads_working;    /* threads currently working */
	pthread_mutex_t  thcount_lock;       /* used for thread count etc */
	jobqueue*  jobqueue_p;               /* pointer to the job queue  */    
} thpool_;





/* ========================== PROTOTYPES ============================ */


static void  thread_init(thpool_* thpool_p, struct thread** thread_p, int id);
static void* thread_do(struct thread* thread_p);
static void  thread_destroy(struct thread* thread_p);

static int   jobqueue_init(thpool_* thpool_p);
static void  jobqueue_clear(thpool_* thpool_p);
static void  jobqueue_push(thpool_* thpool_p, struct job* newjob_p);
static struct job* jobqueue_pull(thpool_* thpool_p);
static void  jobqueue_destroy(thpool_* thpool_p);

static int  bsem_init(struct bsem *bsem_p, int value);
static int  bsem_reset(struct bsem *bsem_p);
static void  bsem_post(struct bsem *bsem_p);
static void  bsem_post_all(struct bsem *bsem_p);
static void  bsem_wait(struct bsem *bsem_p);





/* ========================== THREADPOOL ============================ */


/* Initialise thread pool */
struct thpool_* thpool_init(int num_threads)
{

	threads_on_hold   = 0;
	threads_keepalive = 1;

	if ( num_threads < 0){
		num_threads = 0;
	}

	/* Make new thread pool */
	thpool_* thpool_p;
	thpool_p = (struct thpool_*)SCX_MALLOC(sizeof(struct thpool_));
	if (thpool_p == NULL){
		TRACE((T_ERROR, "thpool_init(): Could not allocate memory for thread pool\n"));
		return NULL;
	}
	thpool_p->num_threads_alive   = 0;
	thpool_p->num_threads_working = 0;

	/* Initialise the job queue */
	if (jobqueue_init(thpool_p) == -1){
		TRACE((T_ERROR, "thpool_init(): Could not allocate memory for job queue\n"));
		SCX_FREE(thpool_p);
		return NULL;
	}

	/* Make threads in pool */
	thpool_p->threads = (struct thread**)SCX_MALLOC(num_threads * sizeof(struct thread));
	if (thpool_p->threads == NULL){
		TRACE((T_ERROR, "thpool_init(): Could not allocate memory for threads\n"));
		jobqueue_destroy(thpool_p);
		SCX_FREE(thpool_p->jobqueue_p);
		SCX_FREE(thpool_p);
		return NULL;
	}

	pthread_mutex_init(&(thpool_p->thcount_lock), NULL);
	
	/* Thread init */
	int n;
	for (n=0; n<num_threads; n++){
		thread_init(thpool_p, &thpool_p->threads[n], n);
		TRACE((T_DEBUG, "%s() Created thread %d in pool \n", __func__, n));

	}
	
	/* Wait for threads to initialize */
	while (thpool_p->num_threads_alive != num_threads) {}
	TRACE((T_INFO, "Thread Pool inited.\n"));
	return thpool_p;
}


/* Add work to the thread pool */
int thpool_add_work(thpool_* thpool_p, void *(*function_p)(void*), void* arg_p)
{
	job* newjob;
	/* newjob 도 req->pool에서 할당 하도록 변경 필요 */
	newjob=(struct job*)SCX_MALLOC(sizeof(struct job));
	if (newjob==NULL){
		TRACE((T_ERROR, "%s() Could not allocate memory for new job\n", __func__));
		return -1;
	}

	/* add function and argument */
	newjob->function=function_p;
	newjob->arg=arg_p;

	/* add job to queue */
	pthread_mutex_lock(&thpool_p->jobqueue_p->rwmutex);
	jobqueue_push(thpool_p, newjob);
	pthread_mutex_unlock(&thpool_p->jobqueue_p->rwmutex);

	return 0;
}


/* Wait until all jobs have finished */
int thpool_wait(thpool_* thpool_p)
{

	/* Continuous polling */
	double timeout = 1.0;
	time_t start, end;
	double tpassed = 0.0;
	int queue_len = thpool_p->jobqueue_p->len;
	time (&start);
	while (tpassed < timeout && 
			(thpool_p->jobqueue_p->len || thpool_p->num_threads_working))
	{
		time (&end);
		tpassed = difftime(end,start);
	}

	/* Exponential polling */
	long init_nano =  1; /* MUST be above 0 */
	long new_nano;
	double multiplier =  1.01;
	int  max_secs   = 20;
	
	struct timespec polling_interval;
	polling_interval.tv_sec  = 0;
	polling_interval.tv_nsec = init_nano;
	
	while (thpool_p->jobqueue_p->len || thpool_p->num_threads_working)
	{
		nanosleep(&polling_interval, NULL);
		if ( polling_interval.tv_sec < max_secs ){
			new_nano = CEIL(polling_interval.tv_nsec * multiplier);
			polling_interval.tv_nsec = new_nano % MAX_NANOSEC;
			if ( new_nano > MAX_NANOSEC ) {
				polling_interval.tv_sec ++;
			}
		}
		else break;
	}
	
	/* Fall back to max polling */
	while (thpool_p->jobqueue_p->len || thpool_p->num_threads_working){
		sleep(max_secs);
	}
	return queue_len;
}


/* Destroy the threadpool */
void thpool_destroy(thpool_* thpool_p)
{
	
	volatile int threads_total = thpool_p->num_threads_alive;

	/* End each thread 's infinite loop */
	threads_keepalive = 0;
	
	/* Give one second to kill idle threads */
	double TIMEOUT = 1.0;
	time_t start, end;
	double tpassed = 0.0;
	time (&start);
	while (tpassed < TIMEOUT && thpool_p->num_threads_alive){
		bsem_post_all(thpool_p->jobqueue_p->has_jobs);
		time (&end);
		tpassed = difftime(end,start);
	}
	
	/* Poll remaining threads */
	while (thpool_p->num_threads_alive){
		bsem_post_all(thpool_p->jobqueue_p->has_jobs);
		sleep(1);
	}

	/* Job queue cleanup */
	jobqueue_destroy(thpool_p);
	SCX_FREE(thpool_p->jobqueue_p);
	
	/* Deallocs */
	int n;
	for (n=0; n < threads_total; n++){
		thread_destroy(thpool_p->threads[n]);
	}
	SCX_FREE(thpool_p->threads);
	SCX_FREE(thpool_p);
	TRACE((T_INFO, "Thread Pool destroyed.\n"));
}

/* thread pool의 상태를 로그에 출력 */
void thread_pool_status (thpool_* thpool_p)
{
	pthread_mutex_lock(&thpool_p->thcount_lock);
	TRACE((T_INFO|T_STAT, "**** Working thread = %d(%d), job queue = %d\n",
				thpool_p->num_threads_working, thpool_p->num_threads_alive, thpool_p->jobqueue_p->len));
	pthread_mutex_unlock(&thpool_p->thcount_lock);
}

/*
 * jobqueue의 크기를 확인해서 job thread의 수보다 많은 경우 0을 리턴한다.
 * 이 경우 더 이상 jobqueue에 job을 넣지 말고 동기 방식으로 처리를 해야한다.
 * jobqueue의 상태가 정상(queue에 여유가 많은 경우)인 경우는 1을 리턴한다.
 * 이 함수에서 접근하는 변수들의 경우 참조용으로만 사용하기 때문에 따로 lock을 하지는 않는다.
 */
int thpool_available_jobqueue (thpool_* thpool_p)
{
	if (gscx__max_job_queue_len < thpool_p->jobqueue_p->len)
	{
		TRACE((T_DAEMON, "Max job queue length(%d) exceeded!. current queue(%d)\n", gscx__max_job_queue_len, thpool_p->jobqueue_p->len));
		return 0;
	}
	return 1;
}

/* ============================ THREAD ============================== */


/* Initialize a thread in the thread pool
 * 
 * @param thread        address to the pointer of the thread to be created
 * @param id            id to be given to the thread
 * 
 */
static void thread_init (thpool_* thpool_p, struct thread** thread_p, int id)
{
	
	*thread_p = (struct thread*)SCX_MALLOC(sizeof(struct thread));
	if (thread_p == NULL){
		fprintf(stderr, "thpool_init(): Could not allocate memory for thread\n");
		exit(1);
	}

	(*thread_p)->thpool_p = thpool_p;
	(*thread_p)->id       = id;

	pthread_create(&(*thread_p)->pthread, NULL, (void *)thread_do, (*thread_p));
	pthread_detach((*thread_p)->pthread);
	
}

/* What each thread is doing
* 
* In principle this is an endless loop. The only time this loop gets interuppted is once
* thpool_destroy() is invoked or the program exits.
* 
* @param  thread        thread that will run this function
* @return nothing
*/
static void* thread_do(struct thread* thread_p)
{
	/* Set thread name for profiling and debuging */
	char thread_name[128] = {0};
	sprintf(thread_name, "thread-pool-%d", thread_p->id);
	prctl(PR_SET_NAME, thread_name);

	/* Assure all threads have been created before starting serving */
	thpool_* thpool_p = thread_p->thpool_p;
	

	/* Mark thread as alive (initialized) */
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive += 1;
	pthread_mutex_unlock(&thpool_p->thcount_lock);

	while(threads_keepalive){

		bsem_wait(thpool_p->jobqueue_p->has_jobs);

		if (threads_keepalive){
			
			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working++;
			pthread_mutex_unlock(&thpool_p->thcount_lock);
			
			/* Read job from queue and execute it */
			job* job_p;
			pthread_mutex_lock(&thpool_p->jobqueue_p->rwmutex);
			job_p = jobqueue_pull(thpool_p);
			pthread_mutex_unlock(&thpool_p->jobqueue_p->rwmutex);
			void (*func_buff)(void*);
			void*  arg_buff;
			if (job_p) {
				func_buff = job_p->function;
				arg_buff  = job_p->arg;
				func_buff(arg_buff);
				/* 여기에서 working function을 호출 해야 한다. */
			//	scx_run_async_job((void *)job_p->arg);
				SCX_FREE(job_p);
			}
			
			pthread_mutex_lock(&thpool_p->thcount_lock);
			thpool_p->num_threads_working--;
			pthread_mutex_unlock(&thpool_p->thcount_lock);

		}
	}
	pthread_mutex_lock(&thpool_p->thcount_lock);
	thpool_p->num_threads_alive --;
	pthread_mutex_unlock(&thpool_p->thcount_lock);

	return NULL;
}


/* Frees a thread  */
static void thread_destroy (thread* thread_p)
{
	SCX_FREE(thread_p);
}


/* ============================ JOB QUEUE =========================== */


/* Initialize queue */
static int jobqueue_init(thpool_* thpool_p)
{
	
	thpool_p->jobqueue_p = (struct jobqueue*)SCX_MALLOC(sizeof(struct jobqueue));
	if (thpool_p->jobqueue_p == NULL){
		return -1;
	}
	thpool_p->jobqueue_p->len = 0;
	thpool_p->jobqueue_p->front = NULL;
	thpool_p->jobqueue_p->rear  = NULL;

	thpool_p->jobqueue_p->has_jobs = (struct bsem*)SCX_MALLOC(sizeof(struct bsem));
	if (thpool_p->jobqueue_p->has_jobs == NULL){
		return -1;
	}

	pthread_mutex_init(&(thpool_p->jobqueue_p->rwmutex), NULL);
	if (bsem_init(thpool_p->jobqueue_p->has_jobs, 0) < 0)
		return -1;

	return 0;
}


/* Clear the queue */
static void jobqueue_clear(thpool_* thpool_p)
{
	void *queue = NULL;
	while(thpool_p->jobqueue_p->len){
		queue = (void *)jobqueue_pull(thpool_p);
		SCX_FREE(queue);
	}

	thpool_p->jobqueue_p->front = NULL;
	thpool_p->jobqueue_p->rear  = NULL;
	bsem_reset(thpool_p->jobqueue_p->has_jobs);
	thpool_p->jobqueue_p->len = 0;

}


/* Add (allocated) job to queue
 *
 * Notice: Caller MUST hold a mutex
 */
static void jobqueue_push(thpool_* thpool_p, struct job* newjob)
{

	newjob->prev = NULL;

	switch(thpool_p->jobqueue_p->len){

		case 0:  /* if no jobs in queue */
					thpool_p->jobqueue_p->front = newjob;
					thpool_p->jobqueue_p->rear  = newjob;
					break;

		default: /* if jobs in queue */
					thpool_p->jobqueue_p->rear->prev = newjob;
					thpool_p->jobqueue_p->rear = newjob;
					
	}
	thpool_p->jobqueue_p->len++;
	
	bsem_post(thpool_p->jobqueue_p->has_jobs);
	TRACE((T_DEBUG, "%s() remain job, queue length(%d) \n", __func__, thpool_p->jobqueue_p->len));
}


/* Get first job from queue(removes it from queue)
 * 
 * Notice: Caller MUST hold a mutex
 */
static struct job* jobqueue_pull(thpool_* thpool_p)
{

	job* job_p;
	job_p = thpool_p->jobqueue_p->front;

	switch(thpool_p->jobqueue_p->len){
		
		case 0:  /* if no jobs in queue */
		  			break;
		
		case 1:  /* if one job in queue */
					thpool_p->jobqueue_p->front = NULL;
					thpool_p->jobqueue_p->rear  = NULL;
					thpool_p->jobqueue_p->len = 0;
					break;
		
		default: /* if >1 jobs in queue */
					thpool_p->jobqueue_p->front = job_p->prev;
					thpool_p->jobqueue_p->len--;
					/* more than one job in queue -> post it */
					bsem_post(thpool_p->jobqueue_p->has_jobs);
					
	}
	TRACE((T_DEBUG, "%s() remain job queue length(%d) \n", __func__, thpool_p->jobqueue_p->len));
	return job_p;
}


/* Free all queue resources back to the system */
static void jobqueue_destroy(thpool_* thpool_p)
{
	jobqueue_clear(thpool_p);
	SCX_FREE(thpool_p->jobqueue_p->has_jobs);
}





/* ======================== SYNCHRONISATION ========================= */


/* Init semaphore to 1 or 0 */
static int bsem_init(bsem *bsem_p, int value)
{
	if (value < 0 || value > 1) {
		TRACE((T_ERROR, "bsem_init(): Binary semaphore can take only values 1 or 0\n"));
		return -1;
	}
	pthread_mutex_init(&(bsem_p->mutex), NULL);
	pthread_cond_init(&(bsem_p->cond), NULL);
	bsem_p->v = value;
	return 0;
}


/* Reset semaphore to 0 */
static int bsem_reset(bsem *bsem_p)
{
	int ret = bsem_init(bsem_p, 0);
	return ret;
}


/* Post to at least one thread */
static void bsem_post(bsem *bsem_p)
{
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_signal(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}


/* Post to all threads */
static void bsem_post_all(bsem *bsem_p)
{
	pthread_mutex_lock(&bsem_p->mutex);
	bsem_p->v = 1;
	pthread_cond_broadcast(&bsem_p->cond);
	pthread_mutex_unlock(&bsem_p->mutex);
}


/* Wait on semaphore until semaphore has value 0 */
static void bsem_wait(bsem* bsem_p)
{
	pthread_mutex_lock(&bsem_p->mutex);
	while (bsem_p->v != 1) {
		pthread_cond_wait(&bsem_p->cond, &bsem_p->mutex);
	}
	bsem_p->v = 0;
	pthread_mutex_unlock(&bsem_p->mutex);
}
