#ifndef __GLRU_H__
#define __GLRU_H__
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
	int				(*dump)(char *d, int l, void *udata);
} glru_operation_map_t;

#define 		GLRU_FMAGIC_VALUE 	0x0F0F0F0F
#define 		GLRU_RMAGIC_VALUE 	0xF0F0F0F0
#ifndef NC_RELEASE_BUILD
#define 		GLRU_CHECK_CORRUPT(_g_) 	ASSERT(((_g_)->fmagic | (_g_)->rmagic) == 0xFFFFFFFF)
#else
#define 		GLRU_CHECK_CORRUPT(_g_) 	
#endif
typedef struct tag_glru {
	char 				name[64];
#ifndef NC_RELEASE_BUILD
	nc_uint32_t 		fmagic;		
#endif
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
	int 			(*dump)(char *d, int l, void *udata);
#ifndef NC_RELEASE_BUILD
	nc_uint32_t 		rmagic;		
#endif
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

glru_t * glru_init(int max_entries, glru_operation_map_t *map, char *name) ;
int glru_destroy(glru_t *glru);
int glru_unregister(glru_t *glru, void *map, glru_node_t *gnode);
int glru_ref(glru_t *glru, glru_node_t **gnode, const char *key, void *map, u_boolean_t allocifnotexist, u_boolean_t setprogress, const char *, int l);
void glru_commit(glru_t *glru, glru_node_t *);
int glru_unref(glru_t *glru, glru_node_t *gnode);
int glru_getref(glru_t *glru, glru_node_t *gnode);
int glru_makeref(glru_t *glru, glru_node_t *gnode);
pthread_mutex_t * glru_get_lock(glru_t *glru);
void glru_unlock(glru_t *glru);
void glru_lock(glru_t *glru);
int glru_progress_nolock(glru_t *glru, glru_node_t *gnode);
char * glru_dump(char *b, int l, glru_node_t *gnode);
void glru_ref_direct(glru_t *glru, glru_node_t *gnode);
void glru_for_each(glru_t *glru, int (*proc)(void *glru_node, void *), void *proc_data);
int glru_check_idle(void *glru);
#endif
