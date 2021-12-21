#ifndef __SCX_GLRU_H__
#define __SCX_GLRU_H__
#if 0
struct tag_link_list;
//typedef int  (*node_lock_proc_t)(void *data, int enforce_lock);
#pragma pack(4)
typedef struct tag_link_list {
        struct tag_link_node    *head;
        struct tag_link_node    *tail;
        int                                     count;
        int                                             ref;
} link_list_t;
typedef struct {
        link_node_t                     bnode; /* 위치는 반드시 처음 */
        nc_uint8_t                              state;
        nc_uint8_t                              hitcount; /* useful only for LRU */
        //nc_int16_t                              signature;  /* invalid = -1 */
        nc_uint32_t                             clockstamp; /* refer to clfu */
    void                                        *root;
} lru_node_t;
#pragma pack()
typedef enum {
        U_TRUE=1,
        U_FALSE=0
} u_boolean_t;
typedef int (*fc_check_idle_t)(void *);
typedef int (*fc_destructor_t)(void *);
typedef int (*fc_dump_t)(void *);
typedef struct tag_lru_root {
//        unsigned short          magic;
        int                                     id;
        time_t                          signature;
//        int                             debug;
        void                            *root;    /* points to the LFU band */
        int                                     lru_cap;  /* # of max elements should be in */
        int                                     lru_cnt;  /*# of elements, used to trigger cache reclaim */
      	fc_check_idle_t		check_idle;
      	fc_dump_t 			dump;
//        pthread_mutex_t         *lock;
//        int                             foreignlock;
//        int                 locked;
        link_list_t             list;
} fc_lru_root_t;


typedef struct tag_glru_node glru_node_t;
typedef struct tag_glru_operations {
	void *			(*allocate)(glru_node_t **);
	void *			(*free)(glru_node_t *gnode, void *data);
	void 			(*reset)(void *);
	void 			(*fill_key)(void *, const char *);
	void *			(*lookup)(void *, const char *);
	int 			(*enroll)(void *map, glru_node_t *node);
	int 			(*unroll)(void *map, glru_node_t *node);
	int 			(*isidle)(void *);
	int 			(*continue_ifrunout)(int alloced); /* TRUE 리턴시 지정갯수 초과할당 상황이라도 추가할당 */
	int             (*dump)(char *d, int l, void *udata);
} glru_operation_map_t;

#define 		GLRU_FMAGIC_VALUE 	0x0F0F0F0F
#define 		GLRU_RMAGIC_VALUE 	0xF0F0F0F0
#define 		GLRU_CHECK_CORRUPT(_g_) 	ASSERT(((_g_)->fmagic | (_g_)->rmagic) == 0xFFFFFFFF)
typedef struct tag_glru {
	char 				name[64];
	nc_uint32_t 		fmagic;		
	int 				max_count;
	int 				count;
	pthread_mutex_t 	lock;
	pthread_cond_t 		signal;
	link_list_t 		pool;
	fc_lru_root_t 		*LRU;
	void * 			(*allocate)(glru_node_t **);
	void *			(*free)(glru_node_t *gnode, void *data);
	void 			(*reset)(void *);
	void 			(*fill_key)(void *, const char *);
	void *			(*lookup)(void *, const char *);
	int 			(*enroll)(void *map, glru_node_t *node);
	int 			(*unroll)(void *map, glru_node_t *node);
	int 			(*isidle)(void  *);
	int 			(*continue_ifrunout)(int alloced);
	int             (*dump)(char *d, int l, void *udata);
	nc_uint32_t 		rmagic;		
} glru_t;
struct tag_glru_node {
	lru_node_t 	node;
	void 			*groot;
	void 			*gdata;
	int 			refcnt;
	int 			waiter;
	unsigned char	progress:1;
	unsigned char	cached:1;
#ifdef GLRU_DEBUG
	int 			__id;
	char 			__sig[1024];
	int 			__line;
#endif
}; 

#define 	GLRUR_FOUND 		0
#define 	GLRUR_NOTFOUND 		-1
#define 	GLRUR_ALLOCATED 	1
#define 	GRLUR_RESETTED		5			/* glru에 정의 되지 않은 값을 임의로 정의 */

glru_t * glru_init(int max_entries, glru_operation_map_t *map, char *name) ;
int glru_destroy(glru_t *glru);
int glru_unregister(glru_t *glru, void *map, glru_node_t *gnode);
int glru_ref(glru_t *glru, glru_node_t **gnode, const char *key, void *map, u_boolean_t allocifnotexist, u_boolean_t setprogress, const char *, int l);
void glru_commit(glru_t *glru, glru_node_t *);
int glru_unref(glru_t *glru, glru_node_t *gnode);
int glru_getref(glru_t *glru, glru_node_t *gnode);
void glru_makeref(glru_t *glru, glru_node_t *gnode);
pthread_mutex_t * glru_get_lock(glru_t *glru);
void glru_unlock(glru_t *glru);
void glru_lock(glru_t *glru);
int glru_progress_nolock(glru_t *glru, glru_node_t *gnode);
char * glru_dump(char *b, int l, glru_node_t *gnode);
void glru_ref_direct(glru_t *glru, glru_node_t *gnode);
void glru_for_each(glru_t *glru, int (*proc)(void *glru_node, void *), void *proc_data);
int glru_check_idle(void *glru);

#else

#include "util.h"
#include "lru.h"
#include "glru.h"
#define 	GRLUR_RESETTED		5			/* glru.h에 정의 되지 않은 값을 임의로 정의 */

#endif
#endif
