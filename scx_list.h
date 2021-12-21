#ifndef __SCX_LIST_H__
#define __SCX_LIST_H__

#define SCX_CHECK_LIST(list) ASSERT(((list)->head == NULL && (list)->tail == NULL) || ((list)->head != NULL && (list)->tail != NULL))

#define	O_LIST_TYPE_DATA	0x0001
#define	O_LIST_TYPE_KEY		0x0002
typedef enum {
	LIST_TYPE_UNDEFINED			= 0,
	LIST_TYPE_SIMPLE_DATA		= O_LIST_TYPE_DATA, /* data만 저장 */
	LIST_TYPE_SIMPLE_KEY		= O_LIST_TYPE_KEY, /* key만 저장, key 검색용 */
	LIST_TYPE_KEY_DATA			= O_LIST_TYPE_DATA | O_LIST_TYPE_KEY/* key, data 모두 저장 */
} scx_list_type_t;





struct request_mem_pool;

typedef struct tag_scx_list_node{
	char 			*key;	/* type이 LIST_TYPE_SIMPLE_LIST인 경우는 key에는 값이 없을수도 있음, key는 항상 내부적으로 복사가 이루어진다. */
	void 			*data;
	struct tag_scx_list_node *next;
	struct tag_scx_list_node *prev;
} scx_list_node_t;

typedef struct tag_scx_list {
	struct request_mem_pool		*mpool;	/* 값이 설정되면 해당 memory pool을 사용해서 할당한다. mpool이 NULL이면 SCX_MALLOC를 사용한다. */
	scx_list_node_t				*head;
	scx_list_node_t				*tail;
	int 						count;	/* node의 수 */
	scx_list_type_t				type;
	int							copy;	/* copy가 1인 경우에는 memory allocation을 한후 복사 한다. 0인 경우는 할당하지않고 assign을 한다. */
} scx_list_t;


typedef struct tag_scx_list_iter {
	scx_list_t *list;
	scx_list_node_t *node;
} scx_list_iter_t;


scx_list_t * scx_list_create(struct request_mem_pool	*mpool, scx_list_type_t type, int copy);
int scx_list_destroy(scx_list_t *list);
int scx_list_append(scx_list_t *list, char *key, void *data, int data_len);
int scx_list_get_size(scx_list_t *list);
void *scx_list_get_by_index_data(scx_list_t *list, int index);
char *scx_list_get_by_index_key(scx_list_t *list, int index);
void *scx_list_find_data(scx_list_t *list, const char *key, int bcase); 	/* 해당 키가 리스트에 있는지 확인 해서 있는 경우 해당 data를 리턴한다.*/
char *scx_list_find_key(scx_list_t *list, const char *key, int bcase); 	/* 해당 키가 리스트에 있는지 확인 해서 있는 경우 해당 key를 리턴한다. */
scx_list_node_t *scx_list_next(scx_list_node_t *node);
scx_list_node_t *scx_list_prev(scx_list_node_t *node);
scx_list_iter_t *scx_list_iter_new(scx_list_t *list);
void scx_list_iter_free(scx_list_iter_t *iter);
int scx_list_iter_next(scx_list_iter_t *iter);
int scx_list_iter_prev(scx_list_iter_t *iter);
int scx_list_iter_first(scx_list_iter_t *iter);
int scx_list_iter_last(scx_list_iter_t *iter);
int scx_list_iter_valid(scx_list_iter_t *iter);
void *scx_list_iter_data(scx_list_iter_t *iter);
char *scx_list_iter_key(scx_list_iter_t *iter);

int test_list_func(); /* 테스트용 func */
#endif /* __SCX_LIST_H__ */
