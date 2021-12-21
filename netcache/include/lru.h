#ifndef __LRU_H__
#define __LRU_H__
#include <pthread.h>
#include <errno.h>
#include <util.h>


#ifdef PTHREAD_MUTEX_RECUSIVE
#undef NC_PTHREAD_MUTEX_RECURSIVE
#define NC_PTHREAD_MUTEX_RECURSIVE PTHREAD_MUTEX_RECUSIVE
#endif

#define LRU_assign_node(ln_, root_) { \
									(ln_)->root 	 = root_; \
									(ln_)->hitcount = 0; \
									}
#define LRU_init_node(n_, f_, l_) 	{ \
									INIT_NODE((&(n_)->bnode)); \
									(n_)->clockstamp 	= 0xFFFFFFFF; \
									(n_)->hitcount 		= 0; \
									(n_)->state 		= 0; \
									}

typedef int (*fc_check_idle_t)(void *);
typedef int (*fc_destructor_t)(void *);
typedef int (*fc_dump_t)(void *);


/*
 * random prime number less than 0xFF
 */
#define		LRU_CACHED		173


#pragma pack(4)
typedef struct {
	link_node_t 			bnode; /* 위치는 반드시 처음 */
	nc_uint8_t 				dummy__;
	nc_uint8_t 				state;
	nc_uint16_t				hitcount; /* useful only for LRU */
	nc_uint32_t				clockstamp; /* refer to clfu */
    void 					*root;
} lru_node_t;
#pragma pack()

typedef struct tag_lru_root {
	int					id;
	time_t				signature;
	void				*root;	  /* points to the LFU band */
  	int					lru_cap;  /* # of max elements should be in */
  	int					lru_cnt;  /*# of elements, used to trigger cache reclaim */
  	fc_check_idle_t		check_idle;
  	fc_dump_t 			dump;
  	link_list_t        	list;
} fc_lru_root_t;
typedef int (*fc_dump_node_proc_t)(char *, int n, void *data);

int LRU_count(fc_lru_root_t *root);
int LRU_valid(lru_node_t *node);
fc_lru_root_t * LRU_create(int signature, int cap, fc_destructor_t dest_proc, fc_dump_t dump_proc);
void LRU_set_debug(fc_lru_root_t *lru);
int LRU_destroy(fc_lru_root_t *root, fc_destructor_t free_proc);
lru_node_t * LRU_reclaim_node(fc_lru_root_t *root, int *navicnt);
int LRU_join(fc_lru_root_t *dest, fc_lru_root_t *src);
char * LRU_dump(fc_lru_root_t *root, fc_dump_node_proc_t proc, int forward, int *cnt);
char * LRU_for_each(fc_lru_root_t *root, int (*proc)(void *data, void *), void *uc, int forward);
void LRU_destroy_node(link_node_t *n);
int LRU_hit(fc_lru_root_t *root, lru_node_t *node);
int LRU_add(fc_lru_root_t *root, void *data, lru_node_t *node);
int LRU_remove(fc_lru_root_t *root, lru_node_t *ln);
int LRU_hit(fc_lru_root_t *root, lru_node_t *node);



#endif /*__LRU_H__ */
