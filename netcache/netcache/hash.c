#include <config.h>
#include <netcache_config.h>
#ifdef WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <errno.h>

#ifdef DXMALLOC
#include "dmalloc.h"
#endif
//#include <duma.h>
#include "hash.h"
#include "trace.h"
#include "util.h"
#include <snprintf.h>

/*
 * 2014.11.9 MIN_SIZE 수정
 * 2018.6.29 재수정
 */
#define	HASH_TABLE_MIN_SIZE	1237
#define	HASH_TABLE_MAX_SIZE	1237*50
#define CLAMP(x, low, high)  (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))


#define	HASH_TABLE_RESIZE(ht) \
	if ((ht->size >=  4 * ht->nnodes && ht->size > HASH_TABLE_MIN_SIZE) || \
		(4 * ht->size <= ht->nnodes  && ht->size < HASH_TABLE_MAX_SIZE)) {\
			u_ht_resize(ht);  \
	}
#define	HASH_TABLE_SHRINK(ht) \
	if ((ht->size >=  4 * ht->nnodes && ht->size > HASH_TABLE_MIN_SIZE) || \
		(4 * ht->size <= ht->nnodes  && ht->size < HASH_TABLE_MAX_SIZE)) {\
			u_ht_resize(ht);  \
	}
typedef struct u_hash_node	u_hash_node_t;
struct u_hash_node {
	void *			key;
	void *			value;
	u_hash_node_t	*	next;
};
#pragma pack(8)
struct u_hash_table {
	int 				lockmode; 	/* 1 : lock used for every operation */
	int 				isrwlock; 	/* 1 : rw lock would be used */
	int 				rawmode; 	/* 1 : lock used for low-level malloc operation */
	int				    lockshared;
	int				    mcategory;
	int				    debug;
	pthread_mutex_t		*lock;
	pthread_rwlock_t	*rwlock;
	int					size;
	int					nnodes;
	void 				*cb;
	u_hash_node_t		**nodes;
	int					*nels;
	u_hash_func_t			proc_key_hash;
	u_hash_equal_func_t		proc_key_equal;
	u_hash_func_cb_t			proc_key_hash_x;
	u_hash_equal_func_cb_t		proc_key_equal_x;
	u_hash_destroy_func_t	proc_value_destroy;
	u_hash_destroy_func_t	proc_key_destroy;
};
#pragma pack()
/* static function prototypes */

static int u_hash_table_lock(struct u_hash_table *tbl, int bexcl)
{
	register int 	r;

	if (!tbl->lockmode)
		return 0;


	if (tbl->isrwlock) 
		r = (bexcl?pthread_rwlock_wrlock(tbl->rwlock):pthread_rwlock_rdlock(tbl->rwlock));
	else
		r = pthread_mutex_lock(tbl->lock);
	return r;
}
static int u_hash_table_unlock(struct u_hash_table *tbl)
{
	register int 	r;

	if (!tbl->lockmode)
		return 0;

	if (tbl->isrwlock) 
		r = pthread_rwlock_unlock(tbl->rwlock);
	else
		r = pthread_mutex_unlock(tbl->lock);
	return r;
}
unsigned long u_ht_direct_hash(void *v);
static u_hash_node_t *u_ht_node_new(void *key, void *value,int raw, int category);
static u_hash_node_t *u_ht_node_new_dbg(void *key, void *value,int raw, int category, const char *, int );
static void u_ht_node_free(	u_hash_node_t *u_hash_node,
				u_hash_destroy_func_t key_proc,
				u_hash_destroy_func_t value_proc, 
				int raw);
static int u_ht_nodes_free(	u_hash_table_t *, u_hash_node_t *u_hash_node,
				u_hash_destroy_func_t key_proc,
				u_hash_destroy_func_t value_proc,
				int raw);
static u_hash_node_t ** u_ht_lookup_node_internal(u_hash_table_t *ht, void *key);
static void u_ht_resize(u_hash_table_t *ht);
unsigned long u_ht_direct_hash_x(void *v, void *ud);
u_boolean_t u_ht_direct_equal_x(void *v1, void *v2, void *ud);



void PUBLIC_IF
u_ht_table_lock_mode(u_hash_table_t *ht, u_boolean_t b)
{
	ht->lockmode = b;
}
void 
u_ht_table_debug(u_hash_table_t *ht, int onoff)
{
	ht->debug = onoff;
	TRACE((T_WARN, "Table[%p] - debug mode %s\n", ht, (onoff?"ON":"OFF")));
}
u_hash_table_t * PUBLIC_IF
u_ht_table_new(u_hash_func_t proc_hash, u_hash_equal_func_t proc_key_equal, 
				u_hash_destroy_func_t proc_key_destroy, u_hash_destroy_func_t proc_value_destroy)
{
	u_hash_table_t		*ht;
	int					 i;
	pthread_mutexattr_t		attr;


#ifndef HAVE_LCAPI_H
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#else
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#endif
	memset(ht, 0, sizeof(u_hash_table_t));

	ht->lock = (pthread_mutex_t *)XMALLOC(sizeof(pthread_mutex_t), AI_ETC);
	pthread_mutexattr_init(&attr);
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		TRACE((T_ERROR,"ht_table_new- mutex set to RECURSIVE failed:'%s'\n", strerror(errno)));
	}
	pthread_mutex_init(ht->lock, &attr);
	pthread_mutexattr_destroy(&attr);

	ht->cb 			= NULL;
	ht->mcategory 	= AI_ETC;
	ht->lockmode 	= U_TRUE;
	ht->rawmode 	= U_FALSE;
	ht->size 		= HASH_TABLE_MIN_SIZE;
	ht->nnodes		= 0;
	ht->proc_key_hash		= (u_hash_func_t)(proc_hash ? proc_hash: u_ht_direct_hash);
	ht->proc_key_equal		= proc_key_equal;
	ht->proc_key_hash_x		= NULL;
	ht->proc_key_equal_x	= NULL;
	ht->proc_key_destroy	= proc_key_destroy;
	ht->proc_value_destroy	= proc_value_destroy;
#ifndef HAVE_LCAPI_H
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#else
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#endif

	for (i = 0; i < ht->size; i++) {
		ht->nodes[i] = NULL;
		ht->nels[i]	 = 0;
	}
	return ht;
}
u_hash_table_t * PUBLIC_IF
u_ht_table_new_withlock(pthread_mutex_t *lock, 
						u_hash_func_t proc_hash, 
						u_hash_equal_func_t proc_key_equal, 
						u_hash_destroy_func_t proc_key_destroy, 
						u_hash_destroy_func_t proc_value_destroy)
{
	u_hash_table_t		*ht;
	int					 i;
	//pthread_mutexattr_t		attr;


#ifndef HAVE_LCAPI_H
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#else
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#endif
	memset(ht, 0, sizeof(u_hash_table_t));

	ht->lock = lock;
	ht->lockshared = 1;

	ht->cb 			= NULL;
	ht->mcategory 	= AI_ETC;
	ht->lockmode 	= U_TRUE;
	ht->rawmode 	= U_FALSE;
	ht->size 		= HASH_TABLE_MIN_SIZE;
	ht->nnodes		= 0;
	ht->proc_key_hash		= (u_hash_func_t)(proc_hash ? proc_hash: u_ht_direct_hash);
	ht->proc_key_equal		= proc_key_equal;
	ht->proc_key_hash_x		= NULL;
	ht->proc_key_equal_x	= NULL;
	ht->proc_key_destroy	= proc_key_destroy;
	ht->proc_value_destroy	= proc_value_destroy;
#ifndef HAVE_LCAPI_H
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#else
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#endif

	for (i = 0; i < ht->size; i++) {
		ht->nodes[i] = NULL;
		ht->nels[i]	 = 0;
	}
	return ht;
}
u_hash_table_t * PUBLIC_IF
u_ht_table_new_withlock_x(	pthread_mutex_t *lock, 
							void *cb,
							u_hash_func_cb_t proc_hash, 
							u_hash_equal_func_cb_t proc_key_equal, 
							u_hash_destroy_func_t proc_key_destroy, 
							u_hash_destroy_func_t proc_value_destroy)
{
	u_hash_table_t		*ht;
	int					 i;
	//pthread_mutexattr_t		attr;


#ifndef HAVE_LCAPI_H
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#else
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#endif
	memset(ht, 0, sizeof(u_hash_table_t));

	ht->lock = lock;
	ht->lockshared = 1;

	ht->cb 			= cb;
	ht->mcategory 	= AI_ETC;
	ht->lockmode 	= U_TRUE;
	ht->rawmode 	= U_FALSE;
	ht->size 		= HASH_TABLE_MIN_SIZE;
	ht->nnodes		= 0;
	ht->proc_key_hash_x		= (u_hash_func_cb_t)(proc_hash ? proc_hash: u_ht_direct_hash_x);
	ht->proc_key_equal_x	= proc_key_equal;

	ht->proc_key_hash		= NULL;
	ht->proc_key_equal		= NULL;
	ht->proc_key_destroy	= proc_key_destroy;
	ht->proc_value_destroy	= proc_value_destroy;
#ifndef HAVE_LCAPI_H
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#else
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#endif

	for (i = 0; i < ht->size; i++) {
		ht->nodes[i] = NULL;
		ht->nels[i]	 = 0;
	}
	return ht;
}
u_hash_table_t * PUBLIC_IF
u_ht_table_new_withrwlock_x(	pthread_rwlock_t *lock, 
							void *cb,
							u_hash_func_cb_t proc_hash, 
							u_hash_equal_func_cb_t proc_key_equal, 
							u_hash_destroy_func_t proc_key_destroy, 
							u_hash_destroy_func_t proc_value_destroy)
{
	u_hash_table_t		*ht;
	int					 i;
	//pthread_mutexattr_t		attr;


#ifndef HAVE_LCAPI_H
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#else
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#endif
	memset(ht, 0, sizeof(u_hash_table_t));

	ht->rwlock = lock;
	ht->lockshared = 1;
	ht->isrwlock = 1;

	ht->cb 			= cb;
	ht->mcategory 	= AI_ETC;
	ht->lockmode 	= U_TRUE;
	ht->rawmode 	= U_FALSE;
	ht->size 		= HASH_TABLE_MIN_SIZE;
	ht->nnodes		= 0;
	ht->proc_key_hash_x		= (u_hash_func_cb_t)(proc_hash ? proc_hash: u_ht_direct_hash_x);
	ht->proc_key_equal_x	= proc_key_equal;

	ht->proc_key_hash		= NULL;
	ht->proc_key_equal		= NULL;
	ht->proc_key_destroy	= proc_key_destroy;
	ht->proc_value_destroy	= proc_value_destroy;
#ifndef HAVE_LCAPI_H
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#else
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#endif

	for (i = 0; i < ht->size; i++) {
		ht->nodes[i] = NULL;
		ht->nels[i]	 = 0;
	}
	return ht;
}



u_hash_table_t * PUBLIC_IF
u_ht_table_new_x(	void *cb, 
					u_hash_func_cb_t proc_hash, 
					u_hash_equal_func_cb_t proc_key_equal, 
					u_hash_destroy_func_t proc_key_destroy, 
					u_hash_destroy_func_t proc_value_destroy)
{
	u_hash_table_t		*ht;
	int					 i;
	pthread_mutexattr_t		attr;


#ifndef HAVE_LCAPI_H
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#else
	ht = (u_hash_table_t *)XMALLOC(sizeof(u_hash_table_t), AI_ETC);
#endif
	memset(ht, 0, sizeof(u_hash_table_t));

	ht->lock = (pthread_mutex_t *)XMALLOC(sizeof(pthread_mutex_t), AI_ETC);
	pthread_mutexattr_init(&attr);
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		TRACE((T_ERROR,"ht_table_new- mutex set to RECURSIVE failed:'%s'\n", strerror(errno)));
	}
	pthread_mutex_init(ht->lock, &attr);
	pthread_mutexattr_destroy(&attr);

	ht->cb 			= NULL;
	ht->mcategory 	= AI_ETC;
	ht->lockmode 	= U_TRUE;
	ht->rawmode 	= U_FALSE;
	ht->size 		= HASH_TABLE_MIN_SIZE;
	ht->nnodes		= 0;
	ht->proc_key_hash_x		= (u_hash_func_cb_t)(proc_hash ? proc_hash: u_ht_direct_hash_x);
	ht->proc_key_equal_x	= proc_key_equal;
	ht->proc_key_hash		= NULL;
	ht->proc_key_equal		= NULL;
	ht->proc_key_destroy	= proc_key_destroy;
	ht->proc_value_destroy	= proc_value_destroy;
#ifndef HAVE_LCAPI_H
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#else
	ht->nodes				= (u_hash_node_t**)XCALLOC(ht->size, sizeof(u_hash_node_t *), AI_ETC);
	ht->nels				= (int*)XCALLOC(ht->size, sizeof(int), AI_ETC);
#endif

	for (i = 0; i < ht->size; i++) {
		ht->nodes[i] = NULL;
		ht->nels[i]	 = 0;
	}
	return ht;
}
u_hash_table_t * PUBLIC_IF
u_ht_table_new_raw(u_hash_func_t proc_hash, u_hash_equal_func_t proc_key_equal, 
				u_hash_destroy_func_t proc_key_destroy, u_hash_destroy_func_t proc_value_destroy)
{
	u_hash_table_t		*ht;
	int					 i;
	pthread_mutexattr_t  attr;


	ht = (u_hash_table_t *)malloc(sizeof(u_hash_table_t));
	memset(ht, 0, sizeof(u_hash_table_t));

	ht->lock = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutexattr_init(&attr);
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		TRACE((T_ERROR,"ht_table_new- mutex set to RECURSIVE failed:'%s'\n", strerror(errno)));
	}
	pthread_mutex_init(ht->lock, &attr);
	pthread_mutexattr_destroy(&attr);

	ht->lockmode 	= U_TRUE;
	ht->mcategory 	= AI_ETC;
	ht->rawmode 	= U_TRUE;
	ht->size 		= HASH_TABLE_MIN_SIZE;
	ht->nnodes		= 0;
	ht->proc_key_hash		= (u_hash_func_t)(proc_hash ? proc_hash: u_ht_direct_hash);
	ht->proc_key_equal		= proc_key_equal;
	ht->proc_key_hash_x		= NULL;
	ht->proc_key_equal_x	= NULL;
	ht->proc_key_destroy	= proc_key_destroy;
	ht->proc_value_destroy	= proc_value_destroy;
	ht->nodes				= (u_hash_node_t**)calloc(ht->size, sizeof(u_hash_node_t *));
	ht->nels				= (int*)calloc(ht->size, sizeof(int));

	for (i = 0; i < ht->size; i++) {
		ht->nodes[i] = NULL;
		ht->nels[i]	 = 0;
	}
	return ht;
}
void PUBLIC_IF
u_ht_table_free(u_hash_table_t *ht)
{
	int			i;
	long 		total = 0;

	for (i = 0; i < ht->size; i++) {
		total += u_ht_nodes_free(ht, ht->nodes[i], ht->proc_key_destroy, ht->proc_value_destroy, ht->rawmode);
	}
	TRACE((T_DEBUG, "TABLE:%p (raw=%d)- %ld nodes freed\n", ht, ht->rawmode, total));
	if (!ht->lockshared) {
		pthread_mutex_destroy(ht->lock);
		XFREE(ht->lock);
	}
	if (ht->rawmode) {
		free(ht->nodes);
		free(ht->nels);
		free(ht);
	}
	else {
		XFREE(ht->nodes);
		XFREE(ht->nels);
		XFREE(ht);
	}
}
static u_hash_node_t **
u_ht_lookup_node_internal(u_hash_table_t *ht, void *key)
{
	u_hash_node_t		**node = NULL;

		if (ht->cb)
			node = &ht->nodes[ (*ht->proc_key_hash_x)(key, ht->cb) % ht->size ];
		else
			node = &ht->nodes[ (*ht->proc_key_hash)(key) % ht->size ];

		/*\
		 *	hash table lookup needs to be fast.
		 *	we therefore remove the extra conditional of testing
		 *	whether to call the proc_key_equal or not from the
		 *  inner loop
		\*/

#if 0
		if (ht->proc_key_equal) {
			while (*node && !(*ht->proc_key_equal)((*node)->key, key))
				node = &(*node)->next;
		}
		else
			while (*node && (*node)->key != key)
				node = &(*node)->next;
#else
		if (ht->proc_key_equal||ht->proc_key_equal_x) {
			if (ht->cb)
				while (*node && !(*ht->proc_key_equal_x)((*node)->key, key, ht->cb))
					node = &(*node)->next;
			else
				while (*node && !(*ht->proc_key_equal)((*node)->key, key))
					node = &(*node)->next;
		}
		else
			while (*node && (*node)->key != key)
				node = &(*node)->next;
#endif
	return node;
}

void * PUBLIC_IF
u_ht_lookup(u_hash_table_t *ht, void *key)
{
	//char			sig[56];
	u_hash_node_t	 *node = NULL;
	//int				r, c;
	void 			*v ;

	u_hash_table_lock(ht, U_FALSE);
		node = *u_ht_lookup_node_internal(ht, key);
		v = (node ? node->value:NULL);
	u_hash_table_unlock(ht);
	return v;
}
void * PUBLIC_IF
u_ht_lookup_and_run(u_hash_table_t *ht, void *key, void (*func)(void *v))
{
	//char			sig[56];
	u_hash_node_t	 *node = NULL;
	//int				r, c;
	void 			*v ;

	u_hash_table_lock(ht, U_FALSE);

	node = *u_ht_lookup_node_internal(ht, key);
	v = (node ? node->value:NULL);
	if (v) {
		(*func)(v);
	}

	u_hash_table_unlock(ht);
	return v;
}
/*\
 *	lookup_key 	: the key to lookup
 *  org_key		: returns the original key
 *	value		: returns the value associated with the key
 *
 *	DESCRIPTIONS
 *	Looks up a key in the ht, returning the original key and
 *  the associated value and a u_boolean_t value which is U_TRUE if the key
 *	was found. This is useful if you need to free the memory allocated
 *	for the original key, for example before calling u_ht_table_free().
\*/
u_boolean_t
u_ht_lookup_extended(	u_hash_table_t *ht,
					void *lookup_key,
					void **org_key,
					void **value)
{
	u_hash_node_t	*node = NULL;
	node = *u_ht_lookup_node_internal(ht, lookup_key);
	if (node) {
		if (org_key) 
			*org_key = node->key;
		if (value)
			*value = node->value;
		return U_TRUE;
	}
	return U_FALSE;
}
/*\
 *	DESCRIPTION
 *
 *	insert a new key and value into a 'ht'
 *	If the key already exists in the ht, its current value is
 *	replaced with the new value.  The previous value would be
 *	freed automatically.
 * 
\*/	
u_boolean_t PUBLIC_IF
u_ht_insert(u_hash_table_t *ht, void *key, void *value, int replacable)
{

	u_hash_node_t		**node;
	//int						r,c;
	u_boolean_t			result=U_FALSE;
	//char			sig[56];

		u_hash_table_lock(ht, U_TRUE);
			node = u_ht_lookup_node_internal(ht, key);
			if (*node) {
				if (!replacable) {
					result = U_FALSE;
					goto L_insert_error;
				}
				/*\
				 *	do not reset node->key in this place, keeping
				 *	the old key is the intended behaviour.
				 *	u_ht_replace(..) can be used instead
				\*/
		
				/*\
				 * free the passed key, because caller doesn't know
				 * if inserted or replaced
				\*/ 
				//fprintf(stderr, "key[%s] replaced\n", (char *)key);
				if (ht->proc_key_destroy)
					ht->proc_key_destroy(key);
		
				if (ht->proc_value_destroy)
					ht->proc_value_destroy((*node)->value);
				(*node)->value = value;
				result = U_TRUE;
			}
			else {
				int		hi;
				if (ht->cb)
					hi = (*ht->proc_key_hash_x)(key, ht->cb) % ht->size;
				else
					hi = (*ht->proc_key_hash)(key) % ht->size;
				ht->nels[hi]++;
		
				*node = u_ht_node_new(key, value, ht->rawmode, ht->mcategory);
				ht->nnodes++;
				HASH_TABLE_RESIZE(ht);
				//fprintf(stderr, "key[%s] added in slot[%d]\n", (char *)key, hi);
				result = U_TRUE;
			}
L_insert_error:
		u_hash_table_unlock(ht);
	return result;
}
u_boolean_t PUBLIC_IF
u_ht_insert_dbg(u_hash_table_t *ht, void *key, void *value, int replacable, const char *f, int l)
{

	u_hash_node_t		**node;
	//int						r,c;
	u_boolean_t			result = U_FALSE;
	//char			sig[56];

	u_hash_table_lock( ht, U_TRUE );
	node = u_ht_lookup_node_internal( ht, key );
	if( *node ) {
		if( !replacable ) {
			result = U_FALSE;
			goto L_insert_error;
		}
		/*\
		 *	do not reset node->key in this place, keeping
		 *	the old key is the intended behaviour.
		 *	u_ht_replace(..) can be used instead
		\*/

		/*\
		 * free the passed key, because caller doesn't know
		 * if inserted or replaced
		\*/
		//fprintf(stderr, "key[%s] replaced\n", (char *)key);
		if( ht->proc_key_destroy )
			ht->proc_key_destroy( key );

		if( ht->proc_value_destroy )
			ht->proc_value_destroy( (*node)->value );
		(*node)->value = value;
		result = U_TRUE;
	}
	else {
		int		hi;
		if( ht->cb )
			hi = (*ht->proc_key_hash_x)(key, ht->cb) % ht->size;
		else
			hi = (*ht->proc_key_hash)(key) % ht->size;
		ht->nels[hi]++;

		*node = u_ht_node_new_dbg( key, value, ht->rawmode, ht->mcategory, f, l );
		if( ht->debug ) {
			TRACE( (T_WARN, "Table[%p] - node %p added\n", ht, *node) );
		}
		ht->nnodes++;
		HASH_TABLE_RESIZE( ht );
		//fprintf(stderr, "key[%s] added in slot[%d]\n", (char *)key, hi);
		result = U_TRUE;
	}
L_insert_error:
	u_hash_table_unlock( ht );
	return result;
}
void * PUBLIC_IF
u_ht_insert2(u_hash_table_t *ht, void *key, void *value, int replacable)
{

	u_hash_node_t		**node;
	void 				*lkvalue = value;

		u_hash_table_lock(ht, U_TRUE);
			node = u_ht_lookup_node_internal(ht, key);
			if (*node) {
				if (!replacable) {
					lkvalue = (*node)->value;
					goto L_insert_error;
				}
				/*\
				 *	do not reset node->key in this place, keeping
				 *	the old key is the intended behaviour.
				 *	u_ht_replace(..) can be used instead
				\*/
		
				/*\
				 * free the passed key, because caller doesn't know
				 * if inserted or replaced
				\*/ 
				//fprintf(stderr, "key[%s] replaced\n", (char *)key);
				if (ht->proc_key_destroy)
					ht->proc_key_destroy(key);
		
				if (ht->proc_value_destroy)
					ht->proc_value_destroy((*node)->value);
				(*node)->value = value;
			}
			else {
				int		hi;
				if (ht->cb)
					hi = (*ht->proc_key_hash_x)(key, ht->cb) % ht->size;
				else
					hi = (*ht->proc_key_hash)(key) % ht->size;
				ht->nels[hi]++;
		
				*node = u_ht_node_new(key, value, ht->rawmode, ht->mcategory);
				ht->nnodes++;
				HASH_TABLE_RESIZE(ht);
				//fprintf(stderr, "key[%s] added in slot[%d]\n", (char *)key, hi);
			}
L_insert_error:
		u_hash_table_unlock(ht);
	return lkvalue;
}
void * PUBLIC_IF
u_ht_insert2_dbg(u_hash_table_t *ht, void *key, void *value, int replacable, const char *f, int l)
{

	u_hash_node_t		**node;
	void 				*lkvalue = value;
	//int						r,c;
	//char			sig[56];

		u_hash_table_lock(ht, U_TRUE);
			node = u_ht_lookup_node_internal(ht, key);
			if (*node) {
				if (!replacable) {
					lkvalue = (*node)->value;
					goto L_insert_error;
				}
				/*\
				 *	do not reset node->key in this place, keeping
				 *	the old key is the intended behaviour.
				 *	u_ht_replace(..) can be used instead
				\*/
		
				/*\
				 * free the passed key, because caller doesn't know
				 * if inserted or replaced
				\*/ 
				//fprintf(stderr, "key[%s] replaced\n", (char *)key);
				if (ht->proc_key_destroy)
					ht->proc_key_destroy(key);
		
				if (ht->proc_value_destroy)
					ht->proc_value_destroy((*node)->value);
				(*node)->value = value;
			}
			else {
				int		hi;
				if (ht->cb)
					hi = (*ht->proc_key_hash_x)(key, ht->cb) % ht->size;
				else
					hi = (*ht->proc_key_hash)(key) % ht->size;
				ht->nels[hi]++;
		
				*node = u_ht_node_new_dbg(key, value, ht->rawmode, ht->mcategory, f, l);
				if (ht->debug) {
					TRACE((T_WARN, "Table[%p] - node %p added\n", ht, *node));
				}
				ht->nnodes++;
				HASH_TABLE_RESIZE(ht);
				//fprintf(stderr, "key[%s] added in slot[%d]\n", (char *)key, hi);
			}
L_insert_error:
		u_hash_table_unlock(ht);
	return lkvalue;
}
void
u_ht_replace(	u_hash_table_t *ht,
			void *		key,
			void *		value)
{
	u_hash_node_t 	**node;
	//int					r,c;
	//char			sig[56];

	u_hash_table_lock(ht, U_TRUE);
		node = u_ht_lookup_node_internal(ht, key);

		if (*node) {
			if (ht->proc_value_destroy)
				(*ht->proc_value_destroy)((*node)->value);
			(*node)->value 	= value;
		}
		else {
			int		hi;
			//fprintf(stderr, "key[%s] added by u_ht_replace()\n", (char *)key);
			if (ht->cb)
				hi = (*ht->proc_key_hash_x)(key, ht->cb) % ht->size;
			else
				hi = (*ht->proc_key_hash)(key) % ht->size;
			ht->nels[hi]++;
			*node = u_ht_node_new(key, value, ht->rawmode, ht->mcategory);
			ht->nnodes++;
			HASH_TABLE_RESIZE(ht);
		}
	u_hash_table_unlock(ht);
}

/*\
 *	returns U_TRUE if key found and removed
\*/
u_boolean_t
u_ht_remove(u_hash_table_t *ht, void *key)
{
	u_hash_node_t		**node, *dest;
	//int						r,c;
	int					result = U_FALSE;
	//char			sig[56];


	u_hash_table_lock(ht, U_TRUE);
		node = u_ht_lookup_node_internal(ht, key);
		if (*node) {
			dest = *node;
			(*node) = dest->next;
#if 0 /*def __DEBUG */
			if (*node && !ovalid(*node)) {
				TRACE((T_ERROR, "u_ht_remove : 1-st stage: next node corrupt!\n"));
			}
#endif

			u_ht_node_free(dest, ht->proc_key_destroy, ht->proc_value_destroy, ht->rawmode);
			ht->nnodes--;
			HASH_TABLE_SHRINK(ht);
			result = U_TRUE;
#if 0 /*def __DEBUG*/
			if (*node && !ovalid(*node)) {
				TRACE((T_ERROR, "u_ht_remove : 1-st stage: next node corrupt!\n"));
			}
#endif
		}
	u_hash_table_unlock(ht);
	return result;
}
/*\
 *	returns U_TRUE if key found and removed
\*/
void *
u_ht_remove_II(u_hash_table_t *ht, void *key)
{
	u_hash_node_t		**node, *dest;
	int					result = U_FALSE;
	void 				*v = NULL;


	u_hash_table_lock(ht, U_TRUE);
		node = u_ht_lookup_node_internal(ht, key);


		if (*node) {
			dest = *node;
			v = (dest ? dest->value:NULL);
			(*node) = dest->next;
#if 0 /*def __DEBUG */
			if (*node && !ovalid(*node)) {
				TRACE((T_ERROR, "u_ht_remove : 1-st stage: next node corrupt!\n"));
			}
#endif

			u_ht_node_free(dest, ht->proc_key_destroy, ht->proc_value_destroy, ht->rawmode);
			ht->nnodes--;
			HASH_TABLE_SHRINK(ht);
			result = U_TRUE;
#if 0 /*def __DEBUG*/
			if (*node && !ovalid(*node)) {
				TRACE((T_ERROR, "u_ht_remove : 1-st stage: next node corrupt!\n"));
			}
#endif
		}
	u_hash_table_unlock(ht);
	return v;
}
u_boolean_t
u_ht_steal(u_hash_table_t *ht, void *key)
{
	u_hash_node_t		**node, *dest;
	//int					r,c;
	int					result = U_FALSE;
	//char			sig[56];

		u_hash_table_lock(ht, U_TRUE);

		node = u_ht_lookup_node_internal(ht, key);
		if (*node) {
			dest = *node;
			(*node) = dest->next;
	
			u_ht_node_free(dest, NULL, NULL, ht->rawmode);
			ht->nnodes--;
			HASH_TABLE_SHRINK(ht);
			result = U_TRUE;
		}
		u_hash_table_unlock(ht);
	return result;
}

static unsigned int
u_ht_remove_or_steal_foreach_internal(u_hash_table_t *ht, 
									u_hash_remove_callback_t func,
									void *user_data,
									u_boolean_t notify)
{
	u_hash_node_t		*node, *prev, *nodenext;
	unsigned int	i;
	unsigned int	deleted = 0;
	//int				r,c;


		for (i = 0; i < (unsigned int)ht->size; i++) {
L_restart:
			prev = NULL;

			for (node = ht->nodes[i]; node; prev = node, node = node->next) {
				nodenext = node->next;
				/* we keep 'nodenext because the 'func' would free the node itself */
				if ((*func)(node->key, node->value, user_data)) {
					deleted ++;
					ht->nnodes--;
					if(prev) {
						prev->next = nodenext;
						u_ht_node_free(node, 
										(notify?ht->proc_key_destroy:NULL),
										(notify?ht->proc_value_destroy:NULL), ht->rawmode);
						node = prev;
					}
					else {
						ht->nodes[i] = nodenext;
						u_ht_node_free(node, 
										(notify?ht->proc_key_destroy:NULL),
										(notify?ht->proc_value_destroy:NULL), ht->rawmode);
						goto L_restart;
					}
				}
			}
		}
		if (func) {
			HASH_TABLE_RESIZE(ht);
		}

	return deleted;


}
int
u_ht_remove_foreach(	u_hash_table_t *ht, 
					u_hash_remove_callback_t func,
					void *user_data)
{
	int		n;
	//int		l1,l2;
	//char			sig[56];
	u_hash_table_lock(ht, U_TRUE);
		n =  u_ht_remove_or_steal_foreach_internal(ht, func, user_data, U_TRUE);
	u_hash_table_unlock(ht);
	return n;
}

int
u_ht_steal_foreach(	u_hash_table_t *ht, 
					u_hash_remove_callback_t func,
					void *user_data)
{
	int		n;
	//int		l1,l2;
	//char			sig[56];
	u_hash_table_lock(ht, U_TRUE);
		n = u_ht_remove_or_steal_foreach_internal(ht, func, user_data, U_FALSE);
	u_hash_table_unlock(ht);
	return n;
}

void
u_ht_foreach(	u_hash_table_t *ht,
			u_hash_node_callback_t func,	
			void * user_data)
{
	u_hash_node_t		*node;
	int				i, rc;//, r,c;
	//char			sig[56];


	u_hash_table_lock(ht, U_TRUE);
		for (i = 0; i < ht->size; i++) {
			for (node = ht->nodes[i]; node; node=node->next) {
				rc = (*func)(node->key, node->value, user_data);
				if (rc == HT_STOP) goto L_end;
			}
		}
L_end: ;
	u_hash_table_unlock(ht);
}
unsigned int
u_ht_size(u_hash_table_t *ht)
{
	return ht->nnodes;
}

static void
u_ht_resize(u_hash_table_t *ht)
{
	u_hash_node_t		**new_nodes;
	int				*new_els;
	u_hash_node_t		*node;
	u_hash_node_t		*next;
	unsigned int	hash_val;
	int				new_size;
	int				i;


	new_size = ht->nnodes * 3;
	new_size = CLAMP(new_size, HASH_TABLE_MIN_SIZE, HASH_TABLE_MAX_SIZE);

	if (ht->rawmode) {
		new_nodes= (u_hash_node_t **)calloc(new_size, sizeof(u_hash_node_t *));
		new_els	 = (int*)calloc(new_size, sizeof(int));
	}
	else {
		new_nodes= (u_hash_node_t **)XCALLOC(new_size, sizeof(u_hash_node_t *), AI_ETC);
		new_els	 = (int*)XCALLOC(new_size, sizeof(int), AI_ETC);
	}


	for (i = 0; i < ht->size; i++) {
		for (node = ht->nodes[i]; node; node = next) {
			next = node->next;
			if (ht->cb)
				hash_val = (*ht->proc_key_hash_x)(node->key, ht->cb) % new_size;
			else
				hash_val = (*ht->proc_key_hash)(node->key) % new_size;
			node->next = new_nodes[hash_val];

			new_nodes[hash_val] = node; /* move all linked list to new_nodes */
			new_els[hash_val]++ ; 

//			fprintf(stderr, "\t NODE_ROOT[key[%s] :NODE[%d] => NODE[%d] \n", 
//							(char *)node->key, 
//							i,
//							hash_val);
		}
	}
	if (ht->rawmode) {
		free(ht->nodes);
		free(ht->nels);
	}
	else {
		XFREE(ht->nodes);
		XFREE(ht->nels);
	}
	ht->nodes = new_nodes;
	ht->size  = new_size;
	ht->nels  = new_els;
	TRACE((T_INFO, "Table[%p] - resized to %ld\n", ht, ht->size));
}
static u_hash_node_t *
u_ht_node_new(void *key, void *value, int raw, int category)
{
	u_hash_node_t		*u_hash_node;

	if (raw) {
		u_hash_node  = (u_hash_node_t *)malloc(sizeof(u_hash_node_t));
		memset(u_hash_node, 0, sizeof(*u_hash_node));
	}
	else {
		u_hash_node  = (u_hash_node_t *)XMALLOC(sizeof(u_hash_node_t), category);
	}
	u_hash_node->key 		= key;
	u_hash_node->value 	= value;
	u_hash_node->next 	= NULL;

	return u_hash_node;
}
static u_hash_node_t *
u_ht_node_new_dbg(void *key, void *value, int raw, int category, const char *f, int l)
{
	u_hash_node_t		*u_hash_node;

	if (raw) {
		u_hash_node  = (u_hash_node_t *)malloc(sizeof(u_hash_node_t));
		memset(u_hash_node, 0, sizeof(*u_hash_node));
	}
	else {
		u_hash_node  = (u_hash_node_t *)XMALLOC_FL(sizeof(u_hash_node_t), category, f, l);
	}
	u_hash_node->key 		= key;
	u_hash_node->value 	= value;
	u_hash_node->next 	= NULL;

	return u_hash_node;
}
static void
u_ht_node_free(	u_hash_node_t *u_hash_node,
				u_hash_destroy_func_t key_proc,
				u_hash_destroy_func_t value_proc,
				int raw)
{
	if (key_proc)
		key_proc(u_hash_node->key);
	if (value_proc)
		value_proc(u_hash_node->value);

	if (raw) {
		free(u_hash_node);
	}
	else {
		XFREE(u_hash_node);
	}
}
/*\
 *	free all nodes which has the same hashed key value
\*/
static int
u_ht_nodes_free(u_hash_table_t *ht, 
			    u_hash_node_t *u_hash_node,
				u_hash_destroy_func_t key_proc,
				u_hash_destroy_func_t value_proc, 
				int rawmode)
{
	int 	cnt = 0;
	if (u_hash_node) {
		u_hash_node_t	*node = u_hash_node;
		u_hash_node_t	*tofree = NULL;

		while (node->next) {
			if (key_proc) key_proc(node->key);
			if (value_proc) value_proc(node->value);
			tofree = node;
			node = node->next;

			if (ht->debug) {
				TRACE((T_WARN, "Table[%p - node %p freed\n", ht, tofree));
			}

			if (rawmode) {
				free(tofree);
			}
			else {
				XFREE(tofree);
			}
			cnt++;
		}
		if (key_proc) key_proc(node->key);
		if (value_proc) value_proc(node->value);
		if (ht->debug) {
			TRACE((T_WARN, "Table[%p - node %p freed\n", ht, node));
		}
		if (rawmode) {
			free(node);
		}
		else {
			XFREE(node);
		}
		cnt++;
	}
	return cnt;
}
unsigned long
u_ht_direct_hash(void *v)
{
	return (unsigned long)v;
}
u_boolean_t
u_ht_direct_equal(void *v1, void *v2)
{
	return v1 == v2;
}

unsigned long
u_ht_direct_hash_x(void *v, void *ud)
{
	return (unsigned long)v;
}
u_boolean_t
u_ht_direct_equal_x(void *v1, void *v2, void *ud)
{
	return v1 == v2;
}

void 
u_ht_set_alloc_category(u_hash_table_t *ht, int cat)
{
	ht->mcategory = cat;
}

