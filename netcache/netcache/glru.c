/*
 *
 * 사용시 주의 사항
 * 1. 호출 순서(progress set)
 * 		1.1. glru_ref 
 * 		1.2  glru_unref
 * 		1.3  glru_commit
 * 2. 호출 순서(no setprogress)	
 * 		1.1 glru_ref
 * 		1.2 glru_unref
 * TO BE RESOLVED
 * 	- thread 1에서 glru_ref가 setprogress=0로 호출된 후, glru_ref가 thread 2에 의해서 setprogress와 함께 호출됐을때
 * 	  refcnt=2가 되고 thread 2에서 unref호출 후 commit시 refcnt=1로 남는 상황
 * 	  전통적인 rw lock방식인 경우에는 thread2는 glru_ref 호출에서 대기해야하는 상황이나, 병렬처리 효과가 떨어짐
 */

#include <ctype.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#ifndef WIN32
#include <pwd.h>
#include <grp.h>
#endif

#include <sys/param.h>
#include <sys/stat.h>

#include <config.h>
#include <netcache_config.h>
#include <trace.h>
#include <hash.h>
#include <netcache.h>
#include <block.h>
#include <disk_io.h>
#include <threadpool.h>
#include "util.h"
#include "hash.h"
#include "tlc_queue.h"
#include "asio.h"
#include "bt_timer.h"
#include "snprintf.h"
#include "util.h"
#include "glru.h"


#ifdef GLRU_DEBUG
static int __glru_id = 0;
#endif
int glru_check_idle(void *glru);
static int glru_free_entry(void *vgn);
static void glru_waiton_progress(glru_t *glru, glru_node_t *gnode, const char *key);
static int glru_hit(glru_t *glru, glru_node_t *gnode);

glru_t *
glru_init(int max_entries, glru_operation_map_t *map, char *name) 
{
	glru_t 					*n = NULL;
	pthread_mutexattr_t		mattr;

	n = (glru_t *)XMALLOC(sizeof(glru_t), AI_ETC);
	n->name[0] = 0;
	if (name) strcpy(n->name, name);
	n->max_count = max_entries;
	n->count 	 = 0;
	INIT_LIST(&n->pool);
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
	MUTEX_INIT(&n->lock, &mattr);
	n->LRU 		 = LRU_create(123, 100, glru_check_idle, NULL);
	n->allocate  = map->allocate;
	n->reset  	 = map->reset;
	n->fill_key  = map->fill_key;
	n->enroll  	= map->enroll;
	n->unroll	= map->unroll;
	n->isidle 	= map->isidle;
	n->free 	= map->free;
	n->lookup 	= map->lookup;
	n->continue_ifrunout  	= map->continue_ifrunout;
	n->dump 	= map->dump;
#ifndef NC_RELEASE_BUILD
	n->fmagic 	= GLRU_FMAGIC_VALUE;
	n->rmagic 	= GLRU_RMAGIC_VALUE;
#endif

	condition_init(&n->signal);

	return n;
}

int
glru_check_idle(void *vgn)
{
	glru_node_t	*gnode = (glru_node_t *)vgn;
	glru_t 		*glru = (glru_t *)gnode->groot;

	if (gnode->refcnt > 0 || gnode->progress)
		return U_FALSE; /* not idle */
	return glru->isidle(gnode->gdata);
}
void 
glru_lock(glru_t *glru)
{
	pthread_mutex_lock(&glru->lock);
}
void 
glru_unlock(glru_t *glru)
{
	pthread_mutex_unlock(&glru->lock);
}
static int
glru_hit(glru_t *glru, glru_node_t *gnode)
{
	LRU_hit(glru->LRU, (lru_node_t *)&gnode->node);
	return 0;
}
void
glru_dump_all(glru_t *glru)
{
	int 	cnt = 0;
	LRU_dump(glru->LRU, glru->dump, U_TRUE, &cnt);
}
static glru_node_t *
glru_find_victim(glru_t *glru)
{
	int 			navic = 0;
	link_node_t 	*node;
	node = (link_node_t *)LRU_reclaim_node(glru->LRU, &navic);
	if (!node) {
		TRACE((T_WARN, "GLRU - %d nodes scanned to find a victim\n", navic));
	}
	return (glru_node_t *)(node?node->data:NULL);
}
static glru_node_t *
glru_get_from_pool_internal(glru_t *glru)
{
	glru_node_t *gnode;
	gnode = link_get_head(&glru->pool);
	return gnode;
}
/*
 * setprogress - progress=0이면 1로 변경하고, 1인 경우는 progress가 풀릴때까지 대기
 * 				 신규 할당된 엔트리는 setprogress값과 무관하게 progress가 1로 설정됨
 */
int
glru_ref(glru_t *glru, glru_node_t **gnode, const char *key, void *map, u_boolean_t allocifnotexist, u_boolean_t setprogress, const char *f, int l)
{

	int					res = GLRUR_FOUND;
	void 				*data = NULL;
	int 				retry = 0;
L_get_retry:
	if (retry > 2000) { /* more than 5 secs */
		TRACE((T_WARN, "GLRU - infinite loop for more than 3 secs\n"));
		glru_dump_all(glru);
		TRAP;
		res = GLRUR_NOTFOUND;
		return res;
	}
	GLRU_CHECK_CORRUPT(glru);
	glru_lock(glru);
	*gnode = (glru->lookup)(map, key);
	if (!*gnode) {
		res = GLRUR_NOTFOUND;
		if (allocifnotexist)  {
			*gnode = glru_get_from_pool_internal(glru);
			if (!*gnode) {
				if (glru->count < glru->max_count) {
L_force_alloc:
					res = GLRUR_ALLOCATED;
					data = (glru->allocate)(gnode);
#ifdef GLRU_DEBUG
					(*gnode)->__id 		= __glru_id++;
#endif

					(*gnode)->gdata 	= data;
					(*gnode)->groot  	= glru;
					(*gnode)->progress  = 0;
					(*gnode)->cached	= 0;
					(*gnode)->refcnt	= 0;
					LRU_init_node( &(*gnode)->node, __FILE__, __LINE__);
					glru->count++;
				} 
				else {
					*gnode = glru_find_victim(glru);
					if (*gnode == NULL) {
						if (glru->continue_ifrunout && (glru->continue_ifrunout)(glru->count)) {
							/*
							 * continue_ifrunout함수가 추가 할당을 요청한 상태
							 */ 
							 goto L_force_alloc;
						}
						TRACE((T_WARN, "GLRU - all cached entries(total=%d) busy, waiting 1 msec\n", (int)glru->count));
						glru_unlock(glru);
						retry++;
						bt_msleep(1);
						goto L_get_retry;
					}
					res = GLRUR_ALLOCATED;
					DEBUG_ASSERT(*gnode != NULL);
					DEBUG_ASSERT((*gnode)->cached != 0);
					DEBUG_ASSERT((*gnode)->refcnt == 0);
					DEBUG_ASSERT((*gnode)->progress == 0);
					(glru->unroll)(map, *gnode);
					(glru->reset)((*gnode)->gdata);
					(*gnode)->refcnt 	= 0;
					(*gnode)->cached = 0;
				}
			}
			else {
					res = GLRUR_ALLOCATED;
			}
			if (*gnode) {
				DEBUG_ASSERT((*gnode)->refcnt == 0);
				DEBUG_ASSERT((*gnode)->progress == 0);
				(*gnode)->progress = 1;
#ifdef GLRU_DEBUG		
				strcpy((*gnode)->__sig, f);
				(*gnode)->__line = l;
#endif
				(glru->fill_key)((*gnode)->gdata, key);
				LRU_add(glru->LRU, *gnode, &(*gnode)->node);
				(*gnode)->cached = 1;
				if (map) {
					(glru->enroll)(map, (*gnode));
				}
			}
		}
	}
	else {
	
		res = GLRUR_FOUND;
		if (setprogress) {
			glru_waiton_progress(glru, *gnode, key);
			(*gnode)->progress=1;
#ifdef GLRU_DEBUG		
			strcpy((*gnode)->__sig, f);
			(*gnode)->__line = l;
#endif
		}
		else {
			glru_waiton_progress(glru, *gnode, key);
		}

	}
	if (*gnode) {
		glru_makeref(glru, *gnode);
	}
	glru_unlock(glru);
	GLRU_CHECK_CORRUPT(glru);
	return res;
}
int
glru_unref(glru_t *glru, glru_node_t *gnode)
{
	int 	r;
	GLRU_CHECK_CORRUPT(glru);
	glru_lock(glru);
	r = _ATOMIC_SUB(gnode->refcnt,1);
	glru_unlock(glru);
	return r;
}
int
glru_getref(glru_t *glru, glru_node_t *gnode)
{
	int 	r;
	GLRU_CHECK_CORRUPT(glru);
	glru_lock(glru);
	r = GET_ATOMIC_VAL(gnode->refcnt);
	glru_unlock(glru);
	return r;
}

void
glru_for_each(glru_t *glru, int (*proc)(void *glru_node, void *), void *uc)
{
	GLRU_CHECK_CORRUPT(glru);
	glru_lock(glru);

	if (glru->LRU)
		LRU_for_each(glru->LRU, proc, uc, U_TRUE);


	glru_unlock(glru);
	GLRU_CHECK_CORRUPT(glru);
}
int
glru_makeref(glru_t *glru, glru_node_t *gnode)
{
	int 	r;
	GLRU_CHECK_CORRUPT(glru);
	glru_lock(glru);
	r = _ATOMIC_ADD(gnode->refcnt, 1);
	glru_hit(glru, gnode);
	glru_unlock(glru);
	return r;
}
int
glru_unregister(glru_t *glru, void *map, glru_node_t *gnode)
{
	int 		r = GLRUR_FOUND;

	GLRU_CHECK_CORRUPT(glru);
	glru_lock(glru);
	if (map) {
		(glru->unroll)(map, gnode);
	}
	LRU_remove(glru->LRU, &gnode->node);
	(glru->reset)(gnode->gdata);
	gnode->cached = 0;
	link_append(&glru->pool, gnode, (link_node_t *)&gnode->node);
	
	glru_unlock(glru);
	GLRU_CHECK_CORRUPT(glru);

	return r;

}

static int
glru_free_entry(void *vgn)
{
	glru_node_t 	*gnode = (glru_node_t *)vgn;
	glru_t 		*glru = (glru_t *)gnode->groot;
	(glru->reset)(gnode->gdata);
	(glru->free)(gnode, gnode->gdata);
	return 0;
}
/*
 * return : # of entries freed
 */
int
glru_destroy(glru_t *glru)
{
	glru_node_t 	*gnode = NULL;
	int 			freed = 0;

	GLRU_CHECK_CORRUPT(glru);
	freed = LRU_destroy(glru->LRU, glru_free_entry);
	glru->LRU = NULL;
	TRACE((T_INFO, "GLRU(%s) - %d LRU object freed\n", glru->name, freed)); freed = 0;

	gnode = glru_get_from_pool_internal(glru);
	while (gnode) {
		glru_free_entry(gnode);
		freed++;
		gnode = glru_get_from_pool_internal(glru);
	}
	TRACE((T_INFO, "GLRU(%s) - %d pooled objects freed\n", glru->name, freed));
	pthread_mutex_destroy(&glru->lock);
	pthread_cond_destroy(&glru->signal);
	XFREE(glru);
	return freed;
}
void
glru_commit(glru_t *glru, glru_node_t *gnode)
{
	GLRU_CHECK_CORRUPT(glru);
	glru_lock(glru);
	DEBUG_ASSERT(gnode->cached != 0);
	DEBUG_ASSERT(gnode->progress == 1);
	gnode->progress = 0;
#ifdef GLRU_DEBUG
	gnode->__sig[0] = 0;
	gnode->__line = 0;
#endif
	glru_unlock(glru);
}
pthread_mutex_t * glru_get_lock(glru_t *glru)
{
	GLRU_CHECK_CORRUPT(glru);
	return &glru->lock;
}
int
glru_progress_nolock(glru_t *glru, glru_node_t *gnode)
{
	return (int)gnode->progress; 
}
static void
glru_waiton_progress(glru_t *glru, glru_node_t *gnode, const char *key )
{
	long 				tslice = 31; /* msec */
	struct timespec  	tslice_ts;
	int 				rc;
#ifdef GLRU_DEBUG
	int 				loop = 0;
#endif

	GLRU_CHECK_CORRUPT(glru);

	gnode->waiter++;
	while ((gnode)->progress) {
		tslice = min(tslice, tslice + 17);
		make_timeout(&tslice_ts, tslice, CLOCK_MONOTONIC); /* tslice : msec */
		rc = pthread_cond_timedwait(&glru->signal, &glru->lock, &tslice_ts);
#ifdef GLRU_DEBUG
		DEBUG_ASSERT(rc == ETIMEDOUT || rc == 0);
		loop++;
		if (rc == ETIMEDOUT && (loop&& loop % 100 == 0) ) {
			TRACE((T_WARN, "%s : id=%d, progress=%d, ref=%d, waiter=%d (%d@%s)- timeout\n", key, gnode->__id, gnode->progress, gnode->refcnt, gnode->waiter, gnode->__line, gnode->__sig));
		}
#endif
	}
	gnode->waiter--;
}
char *
glru_dump(char *b, int l, glru_node_t *gnode)
{
#ifdef GLRU_DEBUG
	snprintf(b, l, "id = %d, ref=%d,progress=%d,waiter=%d", gnode->__id, gnode->refcnt, gnode->progress, 0);
#else
	snprintf(b, l, "ref=%d,progress=%d,waiter=%d", gnode->refcnt, gnode->progress, 0);
#endif
	return b;
}
