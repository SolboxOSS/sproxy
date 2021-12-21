#ifndef __VT_H__
#define __VT_H__

#include <sys/types.h>

typedef enum {
	U_TRUE=1,
	U_FALSE=0
} u_boolean_t;
#include <pthread.h>
#ifdef WINDOWS
#include <string.h>

#define	INLINE	__inline

#endif

#define	HT_STOP		1
#define	HT_CONTINUE		0

#define	FONT_BUF_SIZ	1024
#ifndef min
#define	min(x,y) (x > y? y:x)
#endif
#ifndef max
#define	max(x,y) (x > y? x:y)
#endif


typedef struct u_hash_table	u_hash_table_t;
typedef unsigned long 		(*u_hash_func_t)(void *);
typedef unsigned long 		(*u_hash_func_cb_t)(void *, void *);
typedef u_boolean_t 		(*u_hash_equal_func_t)(void *, void *);
typedef u_boolean_t 		(*u_hash_equal_func_cb_t)(void *, void *, void *cb);

typedef void 				(*u_hash_destroy_func_t)(void *);

typedef u_boolean_t 		(*u_hash_remove_callback_t)(void *key, void *value, void *userdata);
typedef int 				(*u_hash_node_callback_t)(void *key, void *value, void *userdata);
u_hash_table_t * 
u_ht_table_new_withlock(pthread_mutex_t *lock, u_hash_func_t proc_hash, u_hash_equal_func_t proc_key_equal, 
				u_hash_destroy_func_t proc_key_destroy, u_hash_destroy_func_t proc_value_destroy);
u_hash_table_t * 
u_ht_table_new_withlock_x(	pthread_mutex_t *lock, 
							void *cb,
							u_hash_func_cb_t proc_hash, 
							u_hash_equal_func_cb_t proc_key_equal, 
							u_hash_destroy_func_t proc_key_destroy, 
							u_hash_destroy_func_t proc_value_destroy);
u_hash_table_t * 
u_ht_table_new_withrwlock_x(	pthread_rwlock_t *lock, 
							void *cb,
							u_hash_func_cb_t proc_hash, 
							u_hash_equal_func_cb_t proc_key_equal, 
							u_hash_destroy_func_t proc_key_destroy, 
							u_hash_destroy_func_t proc_value_destroy);

u_hash_table_t * u_ht_table_new_x(	void *cb, 
					u_hash_func_cb_t proc_hash, 
					u_hash_equal_func_cb_t proc_key_equal, 
					u_hash_destroy_func_t proc_key_destroy, 
					u_hash_destroy_func_t proc_value_destroy);
u_hash_table_t * u_ht_table_new_raw(u_hash_func_t proc_hash, u_hash_equal_func_t proc_key_equal, 
				u_hash_destroy_func_t proc_key_destroy, u_hash_destroy_func_t proc_value_destroy);
u_hash_table_t * u_ht_table_new(u_hash_func_t , u_hash_equal_func_t,
	u_hash_destroy_func_t, u_hash_destroy_func_t );
u_boolean_t u_ht_insert(u_hash_table_t *ht, void *key, void *value, int replacable);
u_boolean_t u_ht_insert_dbg(u_hash_table_t *ht, void *key, void *value, int replacable, const char *, int);
void * u_ht_insert2(u_hash_table_t *ht, void *key, void *value, int replacable);
void * u_ht_insert2_dbg(u_hash_table_t *ht, void *key, void *value, int replacable, const char *f, int l);
void u_ht_table_free(u_hash_table_t *ht);
void * u_ht_lookup(u_hash_table_t *ht, void *key);
void * u_ht_lookup_and_run(u_hash_table_t *ht, void *key, void (*func)(void *v));
void * u_ht_lookup_hint(u_hash_table_t *ht, void *key, void *d);
u_boolean_t u_ht_lookup_extended(	u_hash_table_t *ht,
					void *lookup_key,
					void **org_key,
					void **value);

void u_ht_replace(	u_hash_table_t *ht,
			void *		key,
			void *		value);
u_boolean_t u_ht_remove(u_hash_table_t *ht, void *key);
void * u_ht_remove_II(u_hash_table_t *ht, void *key);
u_boolean_t u_ht_steal(u_hash_table_t *ht, void *key);
int u_ht_remove_foreach(	u_hash_table_t *ht, 
					u_hash_remove_callback_t func,
					void *user_data);
int u_ht_steal_foreach(	u_hash_table_t *ht, 
					u_hash_remove_callback_t func,
					void *user_data);
void u_ht_foreach(	u_hash_table_t *ht,
			u_hash_node_callback_t func,	
			void * user_data);
unsigned int u_ht_size(u_hash_table_t *ht);
unsigned long u_ht_direct_hash(void *v);
u_boolean_t u_ht_direct_equal(void *v1, void *v2);
void u_ht_table_lock_mode(u_hash_table_t *ht, u_boolean_t b);
void u_ht_set_alloc_category(u_hash_table_t *ht, int cat);
unsigned long u_ht_direct_hash64(void *v);
u_boolean_t u_ht_direct_equal64(void *v1, void *v2);

void * ft_lock_create(void);
void u_ht_table_debug(u_hash_table_t *ht, int onoff);
unsigned long u_ht_direct_hash_x(void *v, void *ud);
u_boolean_t u_ht_direct_equal_x(void *v1, void *v2, void *ud);
#endif

