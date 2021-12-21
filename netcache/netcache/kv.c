/* 
 * 키/값 매니지먼트
 */

/*
 * 	File Handle Operations
 *
 * 
 */
#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_SYS_DIR_H
#include <sys/dir.h>
#endif
#ifdef HAVE_DIR_H
#include <dir.h>
#endif
#include <dirent.h>
#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#ifdef WIN32
#include <statfs.h>
#endif
#ifndef WIN32
#include <sys/vfs.h>
#endif
#include <ctype.h>
#include "disk_io.h"
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "bitmap.h"
#include "trace.h"
#include "bt_timer.h"
#include <mm_malloc.h>
#include <snprintf.h>

#define 	USE_MEMMGR


#define	KV_MAGIC 	0xA55A5AA5
#if defined(KV_TRACE)
static link_list_t 		__debug_kv_list = LIST_INITIALIZER;
static pthread_mutex_t 	__debug_kv_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

extern memmgr_heap_t			g__inode_heap;
extern memmgr_heap_t			g__page_heap;
static int 						s__kv_alloced = 0;

#define KV_HASH_VAL(_k_)        ((char)tolower(_k_) & (KV_HASH_SIZE-1))


static void
kv_cleanup(nc_xtra_options_t *kv)
{
	memset(kv->root, 0, KV_HASH_SIZE*sizeof(void *));
	kv->magic				= KV_MAGIC;
	kv->raw_result 			= 0;
	kv->pttl 				= 0;
	kv->nttl 				= 0;
	kv->opaque_command 		= NULL;
	kv->stat_keyid 			= NULL;
	kv->opaque_data_len 	= 0;
	kv->oob_ring 			= NULL;
	kv->opaque 				= NULL;
	kv->client 				= NULL;
	kv->client_data_len 	= 0;
	kv->oob_callback_data 	= NULL;
	kv->oob_callback		= NULL;

	kv->pool_cb = NULL;
	kv->pool_allocator = NULL;

#ifdef KV_TRACE
	INIT_NODE(&kv->node);
#endif
}
PUBLIC_IF CALLSYNTAX
nc_xtra_options_t *
kv_create(char *f, int l)
{
	nc_xtra_options_t * o;

#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif
#ifdef USE_MEMMGR
	o = mem_alloc(&g__inode_heap, sizeof(nc_kv_list_t), U_FALSE, AI_INODE, U_TRUE, __FILE__, __LINE__);
#else
	o = XMALLOC(sizeof(nc_kv_list_t), AI_ETC);
#endif
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif

	kv_cleanup(o);
#if defined(KV_TRACE)
	o->file = f;
	o->line = l;
	pthread_mutex_lock(&__debug_kv_lock);
	link_append(&__debug_kv_list, o, &o->node);
	pthread_mutex_unlock(&__debug_kv_lock);
#endif
	TRACE((T_INODE, "RING[%p] - created \n", o));
	_ATOMIC_ADD(s__kv_alloced,1); 
	return o;
}
nc_xtra_options_t *
kv_create_d(char *f, int l)
{
	nc_xtra_options_t * o = NULL; 

#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif

#ifdef USE_MEMMGR
	o = mem_alloc(&g__inode_heap, sizeof(nc_kv_list_t), U_FALSE, AI_INODE, U_TRUE, f, l);
#else
	o = XMALLOC_FL(sizeof(nc_kv_list_t), AI_ETC, f, l);
#endif

#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif

	kv_cleanup(o);
#if defined(KV_TRACE)
	o->file	= f;
	o->line = l;
	pthread_mutex_lock(&__debug_kv_lock);
	link_append(&__debug_kv_list, o, &o->node);
	pthread_mutex_unlock(&__debug_kv_lock);
#endif
	_ATOMIC_ADD(s__kv_alloced,1); 
	return o;
}
PUBLIC_IF CALLSYNTAX
nc_xtra_options_t *
kv_create_pool_d(void *cb, void *(*allocator )(void *cb, size_t sz), const char *f, int l)
{
	nc_xtra_options_t * o = (nc_xtra_options_t *)(*allocator)(cb, sizeof(nc_kv_list_t));

	kv_cleanup(o);
	o->pool_cb 			= cb;
	o->pool_allocator 	= allocator;
	if (allocator)
		o->custom_allocator 	= 1;
	return o;
}
int
kv_setup_ring(nc_xtra_options_t *kv, size_t bufsiz)
{
	if (!kv) return -1;
	if (bufsiz < 128) 
		return -1; /*버퍼 크기가 너무 작음*/

	if (kv->oob_ring) 
		return 0; /* 이미 만들어있음 */
	DEBUG_ASSERT(kv->magic == KV_MAGIC);
	kv->oob_ring = rb_create(bufsiz, kv->pool_allocator, kv->pool_cb);
	kv->raw_result = 0;
	TRACE((T_INODE, "RING[%p] - created oob[%p]\n", kv, kv->oob_ring));
	DEBUG_ASSERT(kv->oob_ring != NULL);

	return 0; 
}
void
kv_oob_command(nc_xtra_options_t *kv, char *cmd, nc_size_t len)
{
	if (kv->pool_allocator) {
		kv->opaque_command = (char *)(*kv->pool_allocator)(kv->pool_cb, strlen(cmd)+1);
		strcpy(kv->opaque_command, cmd);
	}
	else {
#ifdef USE_MEMMGR
		kv->opaque_command = mem_strdup(&g__inode_heap, cmd, AI_INODE, __FILE__, __LINE__);
#else
		kv->opaque_command = XSTRDUP(cmd, AI_ETC);
#endif
	}
	kv->opaque_data_len = len;
	TRACE((T_INODE, "OOB data len = %ld set\n", len));
}
/*
 *  본 함수는 data가 가르키는 주소에서 len 길이만큼을
 *  ringbuffer에 저장하여, reader가 읽을 수 있도록 한다.
 *  rb_get_writable()함수는 아래와 같은 두 가지 경우에만 리턴된다.
 * 		(1) ring buffer가 full이 아니어서 저장공간이 있는 경우
 * 		(2) ring buffer의 reader에서 에러를 표기한 경우
 * 
 * 	return value
 * 		정상 적인 경우 IN 파라미터에 지정된 len과 동일 값이 전송된다.
 * 	     len과 동일 값이 전송되는 경우, 전송이 성공적으로 이루어졌다고 간주한다.
 * 		 비정상/오류 발생시 -1을 리턴한다. 음수를 반환받았을 때 실제 값의 확인은
 * 		 nc_get_raw_code()함수를 통해 확인한다.
 */
ssize_t
kv_oob_write(nc_xtra_options_t *kv, char *data, size_t len)
{
	int 	wlen;
	int		__t;
	size_t	written = 0;
	size_t	remained = len;
	int 	need_notify = 0;
	int		try = 0;
	int     (*cb)(nc_xtra_options_t *kv, void *) = NULL;
	void	*cbdata = NULL;
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(kv->magic == KV_MAGIC);
#endif
	while (remained > 0 && kv->raw_result == 0) {
		RB_TRANSACTION(kv->oob_ring, U_TRUE) {
			try++;
			if (kv_oob_error(kv, RBF_READER_BROKEN|RBF_EOD)) {
				TRACE((T_PLUGIN, "RING[%p] - got exception(reader broken)(%ld/%ld sent),notify=%d\n", kv, written, len, need_notify));
				RB_UNLOCK(kv->oob_ring, U_TRUE);
				break; /* UNLOCK 포함됨 */
			}

			wlen = rb_wait_for_nolock(kv->oob_ring, RIOT_BUFFER, 100);
	
			/*
			 * data 
			 */
			TRACE((T_PLUGIN, "RING[%p]::%d - rb_wait_for_nolock returned %d\n", kv->oob_ring, try, wlen));
			if (wlen < 0) {
				if (kv_oob_error(kv, RBF_READER_BROKEN|RBF_EOD))  {
					TRACE((T_PLUGIN, "RING[%p] - got exception(reader broken)(%ld/%ld sent),notify=%d\n", kv, written, len, need_notify));
					RB_UNLOCK(kv->oob_ring, U_TRUE);
					break; /* UNLOCK 포함됨 */
				}
			}
			else if (wlen == 0) {
				/*
				 * buffer not available
				 */
				TRACE((T_PLUGIN, "RING[%p]::%d - got TIMEOUT(%ld/%ld sent)\n", kv->oob_ring, try, written, len));
				RB_UNLOCK(kv->oob_ring, U_TRUE);
				if (kv->oob_callback) {
					need_notify = 1;
					cb			= kv->oob_callback;
					cbdata		= kv->oob_callback_data;
				}
				break; /* UNLOCK 포함됨 */
			}
			else {
				wlen 		= rb_write(kv->oob_ring, data, remained);
				remained 	-= wlen;
				written 	+= wlen;
				data 		+= wlen;

				if (kv->oob_callback) {
					need_notify = 1;
					cb			= kv->oob_callback;
					cbdata		= kv->oob_callback_data;
				}
				TRACE((T_PLUGIN, "RING[%p]::%d - got %d bytes written(%ld/%ld sent),notify=%d\n", kv->oob_ring, try, wlen, written, len, need_notify));
			}
		}
		if (need_notify) 
			(*cb)(kv, cbdata);	
	}
	TRACE((T_PLUGIN, "RING[%p]::* - TOTALLY %d bytes written(%ld expected), raw_code=%d notify=%d\n", kv->oob_ring, written, len, kv->raw_result, need_notify));
	if (written <= 0)  {
		if (kv->raw_result != 0) written = -1; /* not timeout, end of transfer*/
	}
	TRACE((T_API, "(KV[%p], %p, %ld) = %d(raw_code=%d)\n", kv, data, len, written, kv->raw_result));

	return written;
}
void
kv_oob_set_error(nc_xtra_options_t *kv, int err)
{

	int			notify = 0;
	int     (*cb)(nc_xtra_options_t *kv, void *) = NULL;
	void	*cbdata = NULL;
	DEBUG_ASSERT(kv->magic == KV_MAGIC);
	rb_lock(kv->oob_ring);
	TRACE((T_INODE, "RING[%p] - set error(%d) set\n", kv->oob_ring, err));
	rb_set_flag(kv->oob_ring, err);
	if (kv->oob_callback && !rb_flag(kv->oob_ring, RBF_READER_BROKEN|RBF_EOD)) {
		/* notify that data available*/
		notify	= 1;
		cb		= kv->oob_callback;
		cbdata	= kv->oob_callback_data;
	}
	TRACE((0, "KV[%p].RING[%p] - set error(%d) set\n", kv, kv->oob_ring, err));
	rb_unlock(kv->oob_ring);

	if (notify) (*cb)(kv, cbdata);	
}
int
kv_oob_error(nc_xtra_options_t *kv, int err)
{
	int r;

	DEBUG_ASSERT(kv->magic == KV_MAGIC);
	r = rb_flag(kv->oob_ring, err);
	return r;
}
void
kv_oob_write_eot(nc_xtra_options_t *kv)
{
	int		notify = 0;
	int     (*cb)(nc_xtra_options_t *kv, void *) = NULL;
	void	*cbdata = NULL;

	DEBUG_ASSERT(kv->magic == KV_MAGIC);
	rb_lock(kv->oob_ring);
	if (kv_oob_error(kv, RBF_READER_BROKEN|RBF_EOD)) {
		/* writing invalid */
		rb_unlock(kv->oob_ring);
		return;
	}
	TRACE((T_PLUGIN, "RING[%p] - EOT set\n", kv->oob_ring));
	rb_set_flag(kv->oob_ring, RBF_EOT);
	if (kv->oob_callback) {
		/* notify that data available*/
		cb		= kv->oob_callback;
		cbdata	= kv->oob_callback_data;
		notify = 1;
	}
	rb_unlock(kv->oob_ring);

	if (notify)
		(*cb)(kv, cbdata);	
	TRACE((T_API, "(KV[%p]) \n", kv));
}
int
kv_oob_endofdata(nc_xtra_options_t *kv)
{
	DEBUG_ASSERT(kv->magic == KV_MAGIC);
	return rb_eot(kv->oob_ring);
}
ssize_t 
kv_oob_read(nc_xtra_options_t *kv, char  *data, size_t len, long tomsec)
{
	int		__t;
	int 	rlen = -1, waitres = 0;
	char	buf[128];

	DEBUG_ASSERT(kv->magic == KV_MAGIC);
	RB_TRANSACTION(kv->oob_ring, U_TRUE) {
		waitres = rb_wait_for_nolock(kv->oob_ring, RIOT_DATA, tomsec);
		/*
		 * rlen == 0 : timeout
		 * rlen >  1 : data available
		 * rlen <  0 : error (including timeout)
		 */
		if (waitres > 0) {
			rlen = rb_read(kv->oob_ring, data, len);
		}
		else if (waitres == 0) {
			rlen = 0; /* means timeout */
		}
		TRACE((T_INODE|T_PLUGIN, "RING(%p) waitres=%d, got %d while waiting for DATA);{%s}\n", kv->oob_ring, waitres, rlen, rb_dump(kv->oob_ring, buf, sizeof(buf))));
	}
	TRACE((T_API, "(KV[%p], %p, %ld, %ld) = %d(waitres=%d)\n", kv, data, (long)len, tomsec, rlen, waitres));
	return rlen;
}
PUBLIC_IF CALLSYNTAX 
void *
kv_add_val(nc_kv_list_t *root, char *key, char *val)
{
	void *p;
#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif
	p = kv_add_val_extended(root, key, val, NULL, 0, 0);
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
	return p;
}

PUBLIC_IF CALLSYNTAX 
void *
kv_add_val_d(nc_kv_list_t *root, char *key, char *val, const char *f, int l)
{
	void *p;
#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif
	p = kv_add_val_extended(root, key, val, f, l, 0);
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
	return p;
}

PUBLIC_IF CALLSYNTAX
void *
kv_add_val_extended(nc_kv_list_t *root, char *key, char *val, const char *f, int l, int ignore)
{
	nc_kv_t 	*new = NULL;

	register    int hv = (int) KV_HASH_VAL(*key);

	if (root->pool_allocator) {
		int 	n;

		DEBUG_ASSERT(root->magic == KV_MAGIC);
		DEBUG_ASSERT(root->custom_allocator != 0);
		new = (nc_kv_t *)(*root->pool_allocator)(root->pool_cb, sizeof(nc_kv_t));
		n = strlen(key);
		new->key 	= (*root->pool_allocator)(root->pool_cb, n+1);
		strcpy(new->key, key);

		n = strlen(val);
		new->value 	= (*root->pool_allocator)(root->pool_cb, n+1);
		strcpy(new->value, val);
	}
	else {
		XVERIFY(root);
		DEBUG_ASSERT(root->magic == KV_MAGIC);
		DEBUG_ASSERT(root->custom_allocator == 0);
		/*
		 * property는 inode bind용이므로
		 * AI_INODE로 카테고리 변경
		 */
#ifdef NC_HEAPTRACK
		___trace_upper++;
#endif
#ifdef USE_MEMMGR
		new 		= (nc_kv_t *)mem_alloc(&g__inode_heap, sizeof(nc_kv_t), U_FALSE, AI_INODE, U_TRUE, __FILE__, __LINE__);
		new->key 	= mem_strdup(&g__inode_heap, key, AI_INODE, __FILE__, __LINE__);
		new->value 	= mem_strdup(&g__inode_heap, val, AI_INODE, __FILE__, __LINE__);
#else
		new 		= (nc_kv_t *)XMALLOC(sizeof(nc_kv_t), AI_ETC);
		new->key 	= XSTRDUP(key, AI_ETC);
		new->value 	= XSTRDUP(val, AI_ETC);
#endif
#ifdef NC_HEAPTRACK
		___trace_upper--;
#endif


	}
	new->ignore = ignore;
        if (!root->root[hv]) {
				if (root->pool_allocator) {
                	root->root[hv] = (link_list_t *) (*root->pool_allocator)(root->pool_cb, sizeof(link_list_t));
				}
				else {
#ifdef USE_MEMMGR
                	root->root[hv] = (link_list_t *) mem_alloc(&g__inode_heap, sizeof(link_list_t), U_FALSE, AI_INODE, U_TRUE, __FILE__, __LINE__);
#else
                	root->root[hv] = (link_list_t *) XMALLOC(sizeof(link_list_t), AI_ETC);
#endif
				}
                INIT_LIST(root->root[hv]);
        }
		DEBUG_ASSERT(root->root[hv] != NULL);
        INIT_NODE(&new->node);
        link_append(root->root[hv], new, &new->node);

	return root;
}
char * kv_find_val_extened(nc_kv_list_t *root, char *key, int bcase, int ignore);
char *
kv_find_val(nc_kv_list_t *root, char *key, int bcase)
{
	return kv_find_val_extened(root, key, bcase, 0);
}

char *
kv_find_val_extened(nc_kv_list_t *root, char *key, int bcase, int ignore)
{
	nc_kv_t 	*iter;
	nc_kv_t 	*found = NULL;
	int 		r;
	register    int hv = (int) KV_HASH_VAL(*key);

	DEBUG_ASSERT(root->magic == KV_MAGIC);

	iter = root->root[hv]?link_get_head_noremove(root->root[hv]):NULL;

	while (iter) {
		r = (bcase?strcmp(key, iter->key):strcasecmp(key, iter->key));
		if (r == 0) {
			if (ignore == 1  || iter->ignore == 0) {
				/* key와 일치하는 항목 발견 */
				found = iter;
				break;
			}
		}
		iter = (nc_kv_t *)link_get_next(root->root[hv], &iter->node);
	}
	return (found?found->value:NULL);
}

PUBLIC_IF CALLSYNTAX int
kv_remove(nc_kv_list_t *root, char *key, int casesensitive)
{
	nc_kv_t 	*iter;
	nc_kv_t 	*found = NULL;
	int 		r;

    register    int hv = (int) KV_HASH_VAL(*key);
	DEBUG_ASSERT(root->magic == KV_MAGIC);

	iter = root->root[hv]?link_get_head_noremove(root->root[hv]):NULL;

	while (iter) {
		r = (casesensitive?strcmp(key, iter->key):strcasecmp(key, iter->key));
		if (r == 0) {
			/* key와 일치하는 항목 발견 */
			found = iter;
			break;
		}
		iter = (nc_kv_t *)link_get_next(root->root[hv], &iter->node);

	}
	if (found) {
		link_del(root->root[hv], &found->node);

		if (root->pool_allocator == NULL) {
#ifdef USE_MEMMGR
			MEM_FREE(&g__inode_heap, found->key, U_FALSE);
			MEM_FREE(&g__inode_heap, found->value, U_FALSE);
			MEM_FREE(&g__inode_heap, found, U_FALSE);
#else
			XFREE(found->key);
			XFREE(found->value);
			XFREE(found);
#endif
		}
	}
	return (found!=NULL);
}
PUBLIC_IF CALLSYNTAX int
kv_replace(nc_kv_list_t *root, char *key, char *value, int bcase)
{
	nc_kv_t 	*iter;
	nc_kv_t 	*found = NULL;
	int 		r;
	register    int hv = (int) KV_HASH_VAL(*key);

	DEBUG_ASSERT(root->magic == KV_MAGIC);
	iter = root->root[hv]?link_get_head_noremove(root->root[hv]):NULL;

	while (iter) {
		r = (bcase?strcmp(key, iter->key):strcasecmp(key, iter->key));
		if (r == 0) {
			/* key와 일치하는 항목 발견 */
			found = iter;
			break;
		}
		iter = (nc_kv_t *)link_get_next(root->root[hv], &iter->node);

	}
	if (found) {
		link_del(root->root[hv], &found->node);

		if (root->pool_allocator == NULL) {
#ifdef USE_MEMMGR
			MEM_FREE(&g__inode_heap, found->key, U_FALSE);
			MEM_FREE(&g__inode_heap, found->value, U_FALSE);
			MEM_FREE(&g__inode_heap, found, U_FALSE);
#else
			XFREE(found->key);
			XFREE(found->value);
			XFREE(found);
#endif
		}
	}
	if (value && strlen(value) > 0)
		kv_add_val_d(root, key, value, __FILE__, __LINE__);
	return 0;
}
void
kv_update_trace(nc_kv_list_t *root, char *f, int l)
{
#ifdef KV_TRACE
	if (root) {
		root->file = f;
		root->line = l;
	}
#endif
}

PUBLIC_IF CALLSYNTAX 
void
kv_destroy(nc_kv_list_t *root) 
{
	
	nc_kv_t 	*iter;
	int 		hvi = 0;
	link_list_t	*troot = NULL;


#if defined(KV_TRACE)
	pthread_mutex_lock(&__debug_kv_lock);
	link_del(&__debug_kv_list, &root->node);
	pthread_mutex_unlock(&__debug_kv_lock);
#endif
	if (!__sync_bool_compare_and_swap(&root->magic, KV_MAGIC, 0)) {
		TRACE((T_ERROR, "******** FREE attempt to already freed kv-list!(%p)\n", root));
		TRAP;
	}

	TRACE((T_INODE, "KV[%p].RING[%p] - destroying\n", root, root->oob_ring));

	if  (root->oob_ring) {
		rb_lock(root->oob_ring);
		kv_oob_set_error(root, RBF_WRITER_BROKEN);
		rb_unlock(root->oob_ring);

		if (root->oob_callback) {
			(*root->oob_callback)(root, root->oob_callback_data);	
		}
	}

	if (root->pool_allocator == NULL) {
		DEBUG_ASSERT(nc_check_memory(root, 0));
		DEBUG_ASSERT(root->custom_allocator == 0);

		for (hvi = 0; hvi < KV_HASH_SIZE; hvi++) {
			troot = __sync_val_compare_and_swap(&root->root[hvi], root->root[hvi], NULL);
			if (troot) {
				
				while ((iter = (nc_kv_t *)link_get_head(troot)) != NULL) {
					DEBUG_ASSERT(nc_check_memory(iter->key, 0));
					DEBUG_ASSERT(nc_check_memory(iter->value, 0));
					DEBUG_ASSERT(nc_check_memory(iter, 0));

#ifdef USE_MEMMGR
					MEM_FREE(&g__inode_heap, iter->key, U_FALSE);
					MEM_FREE(&g__inode_heap, iter->value, U_FALSE);
					MEM_FREE(&g__inode_heap, iter, U_FALSE);
#else
					XFREE(iter->key);
					XFREE(iter->value);
					XFREE(iter);
#endif
				}
#ifdef USE_MEMMGR
				MEM_FREE(&g__inode_heap, troot, U_FALSE);
#else
				XFREE(troot);
#endif
			}
		}
		if (root->opaque_command) {
			XFREE(root->opaque_command);
#ifdef USE_MEMMGR
			MEM_FREE(&g__inode_heap, root->opaque_command, U_FALSE);
#else
			XFREE(root->opaque_command);
#endif
			root->opaque_command = NULL;
		}
		if (root->stat_keyid) {
#ifdef USE_MEMMGR
			MEM_FREE(&g__inode_heap, root->stat_keyid, U_FALSE);
#else
			XFREE(root->stat_keyid);
#endif
			root->stat_keyid = NULL;
		}
		if (root->oob_ring) {
			rb_destroy(root->oob_ring);
		}
		if(root->client) {
#ifdef USE_MEMMGR
			MEM_FREE(&g__inode_heap, root->client, U_FALSE);
#else
			XFREE(root->client);
#endif
			root->client = NULL;
		}
		root->magic = 0x12345678;
#ifdef USE_MEMMGR
		MEM_FREE(&g__inode_heap, root, U_FALSE);
#else
		XFREE(root);
#endif
		_ATOMIC_SUB(s__kv_alloced,1); 

	}
	else {
		DEBUG_ASSERT(root->custom_allocator != 0);
		DEBUG_ASSERT(root->pool_allocator != NULL);
		/* pool_allocator가 있는 경우, free하지 않음! */
	}
}

PUBLIC_IF CALLSYNTAX int
kv_for_each(nc_kv_list_t *root, int (*do_it)(char *key, char *val, void *cb), void *cb)
{
	int 		r = 0;
	nc_kv_t 	*iter, *iter_n;
	register    int hv; 

	if (!root) {
		return -1;
	}


	if (root->pool_allocator == NULL) {
		XVERIFY(root);
	}

	DEBUG_ASSERT(root->magic == KV_MAGIC);


	for (hv = 0; hv < KV_HASH_SIZE; hv++) {

		if (!root->root[hv]) continue;


		if (root->pool_allocator == NULL) {
			XVERIFY(root->root[hv]);
		}

		iter = root->root[hv]?link_get_head_noremove(root->root[hv]):NULL;
		while (iter) {
			r++;

			if (root->pool_allocator == NULL) {
				XVERIFY(iter);
			}

			iter_n = (nc_kv_t *)link_get_next(root->root[hv], &iter->node);
			if (!iter->ignore) {
				if ((*do_it)((char *)iter->key, (char *)iter->value, cb) < 0)
					break;
			}
			iter = iter_n;
		}
	}


	return r;
}
PUBLIC_IF CALLSYNTAX int
kv_for_each_and_remove(nc_kv_list_t *root, int (*do_it)(char *key, char *val, void *cb), void *cb)
{
	int 		r = 0;
	nc_kv_t 	*iter, *next;
	register    int hv = 0;

	DEBUG_ASSERT(root->magic == KV_MAGIC);



	for (hv = 0; hv < KV_HASH_SIZE; hv++) {
		iter = root->root[hv]?link_get_head_noremove(root->root[hv]):NULL;
		while (iter) {
			r++;
			next = (nc_kv_t *)link_get_next(root->root[hv], &iter->node);
			if (!iter->ignore) {
				r = (*do_it)((char *)iter->key, (char *)iter->value, cb);
			}
			if (r) {
				link_del(root->root[hv], &iter->node);
				if (!root->pool_allocator) {
#ifdef USE_MEMMGR
					MEM_FREE(&g__inode_heap, iter->key, U_FALSE);
					MEM_FREE(&g__inode_heap, iter->value, U_FALSE);
					MEM_FREE(&g__inode_heap, iter, U_FALSE);
#else
					XFREE(iter->key);
					XFREE(iter->value);
					XFREE(iter);
#endif
				}
			}

			iter = next;
		}
	}

	return r;
}

/*
 * 아래 함수는 netcache core에서만 사용하는 함수
 * 그러므로  pool_allocator에 따른 할당은 하지 않음
 */
PUBLIC_IF CALLSYNTAX
nc_xtra_options_t *
kv_clone(nc_xtra_options_t *root, char *f, int l)
{
	nc_xtra_options_t 	*clone; 
	nc_kv_t 			*iter;
	int 				hvi = 0; 

#ifdef NC_HEAPTRACK
	___trace_upper+=2;
#endif
	clone = kv_create(f, l);
#ifdef NC_HEAPTRACK
	___trace_upper-=2;
#endif

	for (hvi = 0; hvi < KV_HASH_SIZE; hvi++) {
		if (root->root[hvi]) {
			iter = (nc_kv_t *)link_get_head_noremove(root->root[hvi]);
			while (iter) {
				if (!iter->ignore) {
#ifdef NC_HEAPTRACK
					___trace_upper+=1;
#endif
					kv_add_val_d(clone, iter->key, iter->value, __FILE__, __LINE__);
#ifdef NC_HEAPTRACK
					___trace_upper-=1;
#endif
				}
				iter = (nc_kv_t *)link_get_next(root->root[hvi], &iter->node);
			}
		}
	}
	if (root->opaque_command) {
#ifdef USE_MEMMGR
		clone->opaque_command = mem_strdup(&g__inode_heap, root->opaque_command, AI_INODE, __FILE__, __LINE__);
#else
		clone->opaque_command = XSTRDUP(root->opaque_command, AI_ETC);
#endif
	}
	/* tproxy 기능을 위해 추가된 부분 */
	if(root->client_data_len && root->client) {
#ifdef USE_MEMMGR
		clone->client = mem_alloc(&g__inode_heap, root->client_data_len, U_FALSE, AI_INODE, U_TRUE, __FILE__, __LINE__);
#else
		clone->client = XMALLOC(root->client_data_len, AI_ETC);
#endif
		memcpy(clone->client, root->client, root->client_data_len);
		clone->client_data_len = root->client_data_len;
	}

	DEBUG_ASSERT(clone->magic == KV_MAGIC);
	return clone;

}
PUBLIC_IF CALLSYNTAX
nc_xtra_options_t *
kv_clone_d(nc_xtra_options_t *root, const char *file, int l)
{
	nc_xtra_options_t 	*clone; 
	nc_kv_t 			*iter;

	int 				hvi = 0; 

#ifdef NC_HEAPTRACK
	___trace_upper+=2;
#endif
	clone = kv_create_d((char *)file, l);
#ifdef NC_HEAPTRACK
	___trace_upper-=2;
#endif


	for (hvi = 0; hvi < KV_HASH_SIZE; hvi++) {
		if (root->root[hvi]) {
			iter = (nc_kv_t *)link_get_head_noremove(root->root[hvi]);
			while (iter) {
				if (!iter->ignore) {
#ifdef NC_HEAPTRACK
					___trace_upper+=2;
#endif
					kv_add_val_d(clone, iter->key, iter->value, file, l);
#ifdef NC_HEAPTRACK
					___trace_upper-=2;
#endif
				}
				iter = (nc_kv_t *)link_get_next(root->root[hvi], &iter->node);
			}
		}
	}
	if (root->opaque_command) {
#ifdef USE_MEMMGR
		clone->opaque_command = mem_strdup(&g__inode_heap, root->opaque_command, AI_INODE, __FILE__, __LINE__);
#else
		clone->opaque_command = XSTRDUP(root->opaque_command, AI_ETC);
#endif
	}
	/* tproxy 기능을 위해 추가된 부분 */
	if(root->client_data_len && root->client) {
#ifdef USE_MEMMGR
		clone->client = mem_alloc(&g__inode_heap, root->client_data_len, U_FALSE, AI_INODE, U_TRUE, __FILE__, __LINE__);
#else
		clone->client = XMALLOC(root->client_data_len, AI_ETC);
#endif
		memcpy(clone->client, root->client, root->client_data_len);
		clone->client_data_len = root->client_data_len;
	}

	//TRACE((T_WARN, "property %p, cloned to a new property %p\n", root, clone));
	DEBUG_ASSERT(clone->magic == KV_MAGIC);
	return clone;

}
char *
kv_dump_property(nc_xtra_options_t *root, char *buf, int len)
{
	nc_kv_t 			*iter;
	int 				n, rem = len;
	char 				*cursor = buf;
	int 				hvi;
	if (root == NULL) {
		n = snprintf(cursor, rem, "(nil)");
		cursor += n;
		*cursor = 0;
		return buf;
	}
#if 1

	for (hvi = 0; hvi < KV_HASH_SIZE; hvi++) {
		if (root->root[hvi]) {
			iter = (nc_kv_t *)link_get_head_noremove(root->root[hvi]);
			while (iter) {
				if (rem > 1) {
					n = snprintf(cursor, rem, "(%s='%s') ", iter->key, iter->value);
					cursor += n;
					rem -= n;
				}
				iter = (nc_kv_t *)link_get_next(root->root[hvi], &iter->node);
			}
		}
	}

	if (root->opaque_command && rem > 1) {
		n = snprintf(cursor, rem, "(OP_CMD='%s')(LEN=%lld) ", root->opaque_command, root->opaque_data_len);
		cursor += n;
		rem -= n;
	}
#endif
	*cursor = 0;
	return buf;
}
void
kv_dump_property_debugger(nc_xtra_options_t *root)
{
	char	buf[10240];

	kv_dump_property(root, buf, sizeof(buf));
	TRACE((T_MONITOR, "%s\n", buf));
}
void
kv_set_raw_code(nc_xtra_options_t *root, int code)
{
	TRACE((T_INODE, "kv_set_raw_code: RING[%p] - set raw code %d\n", root->oob_ring, code));
	DEBUG_ASSERT(root->magic == KV_MAGIC);
	root->raw_result = code;

	TRACE((T_API, "(KV[%p], %d)\n", root, code));
}
int
kv_get_raw_code(nc_xtra_options_t *root)
{
	TRACE((T_API, "(KV[%p]) = %d\n", root, root->raw_result));
	DEBUG_ASSERT(root->magic == KV_MAGIC);
	return root->raw_result;
}
void
kv_set_opaque(nc_xtra_options_t *root, void *opaque)
{
	root->opaque = opaque;
}
PUBLIC_IF CALLSYNTAX
void
kv_set_stat_key(nc_xtra_options_t *root, void *keyid)
{
#if 1
	if (root->pool_allocator) {
		root->stat_keyid = (char *)(*root->pool_allocator)(root->pool_cb, strlen(keyid)+1);
		strcpy(root->stat_keyid, keyid);
	}
	else {
#ifdef USE_MEMMGR
		root->stat_keyid = mem_strdup(&g__inode_heap, root->stat_keyid, AI_INODE, __FILE__, __LINE__);
#else
		root->stat_keyid = XSTRDUP(keyid, AI_ETC);
#endif
	}
#else
	root->stat_keyid = keyid;
#endif
}
char *
kv_get_stat_key(nc_xtra_options_t *root)
{
	return root->stat_keyid;
}
void *
kv_get_opaque(nc_xtra_options_t *root)
{
	return root->opaque;
}
PUBLIC_IF CALLSYNTAX void
kv_set_pttl(nc_xtra_options_t *root, nc_time_t pttl)
{
	root->pttl = pttl;
}
PUBLIC_IF CALLSYNTAX void
kv_set_nttl(nc_xtra_options_t *root, nc_time_t nttl)
{
	root->nttl = nttl;
}
void
kv_set_client(nc_xtra_options_t *kv, void *client, int len)
{
	/*
	 * 1바이트 추가는 client에서 string copy할때
	 * null byte 한바이트를 뺴먹을 경우가 있으므로
	 * 그에 대한 대처
	 */
	if (kv->pool_allocator) {
		kv->client = (*kv->pool_allocator)(kv->pool_cb, len+1);
	}
	else {
#ifdef USE_MEMMGR
		kv->client = mem_alloc(&g__inode_heap, len + 1, U_FALSE, AI_INODE, U_TRUE, __FILE__, __LINE__);
#else
		kv->client = XMALLOC(len+1, AI_ETC);
#endif
	}
	memcpy(kv->client, client, len);
	kv->client_data_len = len;
}
void *
kv_get_client(nc_xtra_options_t *kv)
{
	return kv->client;
}
int
kv_is_pooled(nc_xtra_options_t *kv)
{
	return kv->pool_allocator != NULL;
}
void
kv_dump_allocated()
{
	//nc_xtra_options_t *iter;
#if defined(KV_TRACE)
	TRACE((T_MONITOR, "************************* REMAINED KV-LIST ********************************\n"));
	pthread_mutex_lock(&__debug_kv_lock);
	iter = (nc_xtra_options_t *)link_get_head_noremove(&__debug_kv_list);
	while (iter) {
		TRACE((T_WARN, "KV[%p] - left at %d@%s\n", iter, iter->line, iter->file));
		iter = (nc_xtra_options_t *)link_get_next(&__debug_kv_list, &iter->node);
	}
	pthread_mutex_unlock(&__debug_kv_lock);
	TRACE((T_MONITOR, "************************* KV-LIST DUMP DONE ********************************\n"));
#endif
}
int
kv_valid(nc_xtra_options_t *opt)
{
	return (opt->magic == KV_MAGIC);
}
int
kv_oob_set_notifier(nc_xtra_options_t *kv, int (*callback)(nc_xtra_options_t *kv, void*), void *callback_data)
{
	int		__t;

	DEBUG_ASSERT(kv->magic == KV_MAGIC);
	if (kv->oob_ring) {
		RB_TRANSACTION(kv->oob_ring, U_TRUE) {
			DEBUG_ASSERT(kv->magic == KV_MAGIC);
			kv->oob_callback 		= callback;
			kv->oob_callback_data	= callback_data;
		}
	}
	return 0;	
}
int
kv_oob_valid(nc_xtra_options_t *opt)
{
	return (opt->magic == KV_MAGIC) && (opt->oob_ring != NULL);
}
void
kv_oob_lock(nc_xtra_options_t *opt)
{
	DEBUG_ASSERT(opt->magic == KV_MAGIC);
	rb_lock(opt->oob_ring);	
}
void
kv_oob_unlock(nc_xtra_options_t *opt)
{
	DEBUG_ASSERT(opt->magic == KV_MAGIC);
	rb_unlock(opt->oob_ring);	
}
int
kv_count_allocated()
{
	return _ATOMIC_ADD(s__kv_alloced,0); 
}
char *
kv_dump_oob(char *buf, int l, nc_xtra_options_t *opt)
{
	return rb_dump(opt->oob_ring, buf, l);
}
