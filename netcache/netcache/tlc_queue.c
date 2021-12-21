#include <config.h>
#include <netcache_config.h>
#include <sys/time.h>
#include <stdio.h>
#include <time.h>
#include <trace.h>
#include <snprintf.h>
#include <util.h>
#include <tlc_queue.h>


tlc_queue_t tlcq_init(int waitable)
{
	tlc_queue_t   			q;
	pthread_mutexattr_t 	mattr;


    q = (tlc_queue_t)XMALLOC(sizeof(tlc_queue_info_t), AI_ETC);
    if(q == NULL) return (tlc_queue_t)NULL;

    q->waitable 	= waitable;


	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_NORMAL);
	q->lock 		= (pthread_mutex_t *)XMALLOC(sizeof(pthread_mutex_t), AI_ETC);
    pthread_mutex_init(q->lock, &mattr);


	if (waitable) {
		condition_init(&q->waitcond);
	}

	pthread_mutexattr_destroy(&mattr);

    return q;

} /* end of function */
tlc_queue_t tlcq_init_with_lock(int waitable, pthread_mutex_t *lock)
{
	tlc_queue_t   			q;
	pthread_mutexattr_t 	mattr;


    q = (tlc_queue_t)XMALLOC(sizeof(tlc_queue_info_t), AI_ETC);
    if(q == NULL) return (tlc_queue_t)NULL;

    q->waitable 	= waitable;


    q->lock = lock;


	if (waitable) {
		condition_init(&q->waitcond);
	}

	pthread_mutexattr_destroy(&mattr);

    return q;

} /* end of function */
void
tlcq_destroy(tlc_queue_t q)
{

	link_node_t     *node;

	pthread_mutex_lock(q->lock);

	while ((node = link_get_head_node(&q->list)) != NULL) {
		XFREE(node);
	}
	if (q->waitable) {
		pthread_cond_destroy(&q->waitcond);
	}
	pthread_mutex_destroy(q->lock);

	XFREE(q);
}
int tlcq_enqueue(tlc_queue_t q, void *data)
{
	link_node_t		*node = NULL;

    node = (link_node_t *)XMALLOC(sizeof(link_node_t), AI_ETC);
	XVERIFY(node);

	pthread_mutex_lock(q->lock);
	link_append(&q->list, data, node);
	if (q->waitable) {
		pthread_cond_signal(&q->waitcond);
	}
	pthread_mutex_unlock(q->lock);
	return 0;
}
void * tlcq_dequeue(tlc_queue_t q, int msec)
{
	link_node_t 	*node = NULL;
	void 			*data = NULL;

	pthread_mutex_lock(q->lock);

	if (q->waitable && (msec > 0) && (link_count(&q->list, U_TRUE) == 0)) {
		condition_wait(q->lock, &q->waitcond, msec, NULL, NULL, U_FALSE);
	}

	node = link_get_head_node(&q->list);
	if (node) {
		XVERIFY(node);
		data = node->data;
		XFREE(node);
	}
	
	pthread_mutex_unlock(q->lock);

	return data;
}
int 
tlcq_dequeue_batch(tlc_queue_t q, void *va[], int array_len, int msec)
{
	link_node_t 	*node = NULL;
	int				i = 0;

	pthread_mutex_lock(q->lock);

	if (q->waitable && (msec > 0) && (link_count(&q->list, U_TRUE) == 0)) {
		condition_wait(q->lock, &q->waitcond, msec, NULL, NULL, U_FALSE);
	}

	do {
		node = link_get_head_node(&q->list);
		if (node) {
			XVERIFY(node);
			va[i++] = node->data;
			XFREE(node);
		}
	} while (node && array_len > 0);
	
	pthread_mutex_unlock(q->lock);

	return i;
}

int
tlcq_length(tlc_queue_t q)
{
	int 		ql = 0;


	pthread_mutex_lock(q->lock);
	
	ql = link_count(&q->list, U_TRUE);

	pthread_mutex_unlock(q->lock);

	return ql;
}
