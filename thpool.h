/********************************** 
 * @author      Johan Hanssen Seferidis
 * License:     MIT
 * 
 **********************************/

#ifndef _THPOOL_
#define _THPOOL_





/* =================================== API ======================================= */


typedef struct thpool_* threadpool;


/**
 * @brief  Initialize threadpool
 * 
 * Initializes a threadpool. This function will not return untill all
 * threads have initialized successfully.
 * 
 * @example
 * 
 *    ..
 *    threadpool thpool;                     //First we declare a threadpool
 *    thpool = thpool_init(4);               //then we initialize it to 4 threads
 *    ..
 * 
 * @param  num_threads   number of threads to be created in the threadpool
 * @return threadpool    created threadpool on success,
 *                       NULL on error
 */
threadpool thpool_init(int num_threads);


/**
 * @brief Add work to the job queue
 * 
 * Takes an action and its argument and adds it to the threadpool's job queue.
 * If you want to add to work a function with more than one arguments then
 * a way to implement this is by passing a pointer to a structure.
 * 
 * NOTICE: You have to cast both the function and argument to not get warnings.
 * 
 * @example
 * 
 *    void print_num(int num){
 *       printf("%d\n", num);
 *    }
 * 
 *    int main() {
 *       ..
 *       int a = 10;
 *       thpool_add_work(thpool, (void*)print_num, (void*)a);
 *       ..
 *    }
 * 
 * @param  threadpool    threadpool to which the work will be added
 * @param  function_p    pointer to function to add as work
 * @param  arg_p         pointer to an argument
 * @return nothing
 */
int thpool_add_work(threadpool, void *(*function_p)(void*), void* arg_p);


/**
 * @brief Wait for all queued jobs to finish
 * 
 * Will wait for all jobs - both queued and currently running to finish.
 * Once the queue is empty and all work has completed, the calling thread
 * (probably the main program) will continue.
 * 
 * Smart polling is used in wait. The polling is initially 0 - meaning that
 * there is virtually no polling at all. If after 1 seconds the threads
 * haven't finished, the polling interval starts growing exponentially 
 * untill it reaches max_secs seconds. Then it jumps down to a maximum polling
 * interval assuming that heavy processing is being used in the threadpool.
 *
 * @example
 * 
 *    ..
 *    threadpool thpool = thpool_init(4);
 *    ..
 *    // Add a bunch of work
 *    ..
 *    thpool_wait(thpool);
 *    puts("All added work has finished");
 *    ..
 * 
 * @param threadpool     the threadpool to wait for
 * @return waited worker queue count
 */
int thpool_wait(threadpool);



/**
 * @brief Destroy the threadpool
 * 
 * This will wait for the currently active threads to finish and then 'kill'
 * the whole threadpool to free up memory.
 * 
 * @example
 * int main() {
 *    threadpool thpool1 = thpool_init(2);
 *    threadpool thpool2 = thpool_init(2);
 *    ..
 *    thpool_destroy(thpool1);
 *    ..
 *    return 0;
 * }
 * 
 * @param threadpool     the threadpool to destroy
 * @return nothing
 */
void thpool_destroy(threadpool);

/* thread pool의 상태를 로그에 출력 */
void thread_pool_status(threadpool);

/* jobqueue의 사용가능 상태를 리턴 */
int thpool_available_jobqueue (threadpool);

#endif
