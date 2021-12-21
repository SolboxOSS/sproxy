#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <memory.h>
//#include <ncapi.h>
#include "common.h"
#include "scx_util.h"
#include "reqpool.h"
#include "vstring.h"
#include "common.h"
#include "httpd.h"
#include "scx_list.h"

int
test_list_func()
{
	scx_list_t * root = scx_list_create(NULL, LIST_TYPE_KEY_DATA, 1);
	scx_list_append(root, "1", (void *)"data1", 5);
	scx_list_append(root, "2", (void *)"data2", 5);
	scx_list_append(root, "3", (void *)"data3", 5);
	scx_list_append(root, "4", (void *)"data4", 5);
	scx_list_append(root, "5", (void *)"data5", 5);
	scx_list_append(root, "local", (void *)"local", 5);
	scx_list_append(root, "local2", (void *)"local2", 6);
	scx_list_append(root, "local3", (void *)"local3", 6);
	scx_list_append(root, "exte", (void *)"myest", 5);
	int count = scx_list_get_size(root);
	printf ("link size = %d\n", count);

	for(int i = 0; i < count; i++) {
		printf ("%d, key = %s, data = %s\n", i, scx_list_get_by_index_key(root,i), (char *)scx_list_get_by_index_data(root,i));
	}

	scx_list_iter_t *iter = scx_list_iter_new(root);
	scx_list_iter_first(iter);
	for(;scx_list_iter_valid(iter) == SCX_YES;scx_list_iter_next(iter)) {
		printf ("iter key = %s, data = %s\n",scx_list_iter_key(iter), (char *)scx_list_iter_data(iter));
	}
	scx_list_iter_last(iter);
	for(;scx_list_iter_valid(iter) == SCX_YES;scx_list_iter_prev(iter)) {

		printf ("iter key = %s, data = %s\n",scx_list_iter_key(iter), (char *)scx_list_iter_data(iter));
	}

	scx_list_iter_free(iter);

	printf ("find key(1) = %s\n", scx_list_find_data(root, "1", 0));
	printf ("find key(local) = %s\n", scx_list_find_data(root, "local", 0));
	printf ("find key(local3) = %s\n", scx_list_find_data(root, "local3", 0));
	scx_list_destroy(root);

}

/*
 * mpool이 설정되면 해당 memory pool을 사용해서 할당한다.
 * mpool이 NULL이면 SCX_MALLOC를 사용해서 memory를 할당한다.
 */
scx_list_t *
scx_list_create(struct request_mem_pool	*mpool, scx_list_type_t type, int copy)
{
	scx_list_t *root = NULL;
	if (mpool) {
		root = mp_alloc(mpool, sizeof(scx_list_t));
	}
	else {
		root = SCX_CALLOC(1, sizeof(scx_list_t));
	}

	ASSERT(root);
	root->mpool = mpool;
	root->head = root->tail = NULL;
	root->type = type;
	root->copy = copy;
	return root;
}

int
scx_list_destroy(scx_list_t *list)
{
	scx_list_node_t * node = list->head;
	scx_list_node_t * next = NULL;
	if (!list->mpool) {
		/* memory pool을 사용하는 경우는 여기서 메모리 정리가 필요 없다 */
		while(node) {
			next = node->next;
			if (list->copy) {
				SCX_FREE(node->data);
			}
			SCX_FREE(node);
			node = next;
		}
		SCX_FREE(list);
	}
	return 0;
}


int
scx_list_append(scx_list_t *list, char *key, void *data, int data_len)
{
	char *new_key = NULL;
	scx_list_node_t * node = NULL;
	void *new_data = NULL;
	int	key_len = 0;
	SCX_CHECK_LIST(list);
	if ((list->type & O_LIST_TYPE_KEY) != 0) {
		if (NULL == key) {		/* type이 LIST_TYPE_SIMPLE_KEY나 LIST_TYPE_KEY_DATA 인 경우는 key가 필수 파라미터임 */
			ASSERT(0);
		}
	}
	else if (NULL != key) {		/* type이 LIST_TYPE_KEY_DATA 일때 key에 값이 들어오는 경우 로직 에러라고 본다. */
		ASSERT(0);
	}
	if ((list->type & O_LIST_TYPE_DATA) != 0) {
		if (NULL == data) {		/* type이 LIST_TYPE_SIMPLE_DATA나 LIST_TYPE_KEY_DATA 인 경우는 key가 필수 파라미터임 */
			ASSERT(0);
		}
	}
	else if (NULL != data) {	/* type이 LIST_TYPE_SIMPLE_KEY 일때 key에 값이 들어오는 경우 로직 에러라고 본다. */
		ASSERT(0);
	}


	if (list->mpool) {
		node =  mp_alloc(list->mpool, sizeof(scx_list_node_t));
	}
	else {
		node = SCX_CALLOC(1, sizeof(scx_list_node_t));
	}
	if (NULL != key) {
		key_len = strlen(key) + 1;
		if (list->mpool) {

			new_key = mp_alloc(list->mpool, key_len);
		}
		else {
			new_key = SCX_MALLOC(key_len);
		}
		strncpy(new_key, key, key_len);
	}
	node->key = new_key;
	if (NULL != data) {
		if (list->copy) {
			if (list->mpool) {
				new_data = mp_alloc(list->mpool, data_len);
			}
			else {
				new_data = SCX_MALLOC(data_len);
			}
			memcpy(new_data, data, data_len);
			node->data = new_data;
		}
		else {
			node->data = data;
		}
	}
	else {
		node->data = NULL;
	}
	node->prev = list->tail;

	if (list->tail)
		list->tail->next = node;

	if (list->head == NULL)
		list->head = node;
	list->tail = node;
	list->count++;

	return 0;
}

int
scx_list_get_size(scx_list_t *list)
{
	ASSERT(list);
	return list->count;
}

void *
scx_list_get_by_index_data(scx_list_t *list, int index)
{
	ASSERT(list->count >= index);
	scx_list_node_t * node = list->head;
	int			i;
	for (i = 0; i < index; i++) {
		node = node->next;
	}
	ASSERT(node);
	return node->data;
}

char *
scx_list_get_by_index_key(scx_list_t *list, int index)
{
	ASSERT(list->count >= index);
	scx_list_node_t * node = list->head;
	int			i;
	for (i = 0; i < index; i++) {
		node = node->next;
	}
	ASSERT(node);
	return node->key;
}

/*
 * 해당 키가 리스트에 있는지 확인 해서
 * 있는 경우 해당 data를 리턴한다.
 * bcase가 1이면 대소 문자 구분하지 않음.
 */
void *
scx_list_find_data(scx_list_t *list, const char *key, int bcase)
{
	scx_list_node_t * node = list->head;
	scx_list_node_t *found = NULL;
	int 		r;
	while(node) {
		r = (bcase?strcmp(key, node->key):strcasecmp(key, node->key));
		if (r == 0) {
			/* key와 일치하는 항목 발견 */
			found = node;
			break;
		}
		node = node->next;
	}
	return (found?found->data:NULL);
}

/*
 * 해당 키가 리스트에 있는지 확인 해서
 * 있는 경우 해당 key를 리턴한다.
 * bcase가 1이면 대소 문자 구분하지 않음.
 */
char *
scx_list_find_key(scx_list_t *list, const char *key, int bcase)
{
	scx_list_node_t * node = list->head;
	scx_list_node_t *found = NULL;
	int 		r;
	while(node) {
		r = (bcase?strcmp(key, node->key):strcasecmp(key, node->key));

		if (r == 0) {
			/* key와 일치하는 항목 발견 */
			found = node;
			break;
		}
		node = node->next;
	}
	return (found?found->key:NULL);
}

scx_list_node_t *
scx_list_next(scx_list_node_t *node)
{
	return node->next;
}

scx_list_node_t *
scx_list_prev(scx_list_node_t *node)
{
	return node->prev;
}

scx_list_iter_t *
scx_list_iter_new(scx_list_t *list)
{
	ASSERT(list != NULL);

	scx_list_iter_t *iter = SCX_CALLOC(1, sizeof(scx_list_iter_t));
	ASSERT(iter);
	iter->list = list;
	iter->node = NULL;
	return iter;
}

void
scx_list_iter_free(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);

	SCX_FREE(iter);
	return;
}

int
scx_list_iter_next(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);

	if (!iter->node) {
		scx_list_iter_first(iter);
	}
	else {
		iter->node = scx_list_next(iter->node);
	}
	return SCX_YES;
}

int
scx_list_iter_prev(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);

	if (!iter->node) {
		scx_list_iter_last(iter);
	}
	else {
		iter->node = scx_list_prev(iter->node);
	}
	return SCX_YES;
}

int
scx_list_iter_first(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);

    if (NULL == iter->list->head) {
    	iter->node = NULL;
    	return SCX_NO;
    }
    iter->node = iter->list->head;
    return SCX_YES;
}

int
scx_list_iter_last(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);

    if (NULL == iter->list->tail) {
    	iter->node = NULL;
    	return SCX_NO;
    }
    iter->node = iter->list->tail;
    return SCX_YES;
}



int
scx_list_iter_valid(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);
	if (NULL == iter->node)
		return SCX_NO;
	return SCX_YES;
}

void *
scx_list_iter_data(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);
	if (NULL == iter->node) {
		return NULL;
	}
	return iter->node->data;
}

char *
scx_list_iter_key(scx_list_iter_t *iter)
{
	ASSERT(iter != NULL);
	if (NULL == iter->node) {
		return NULL;
	}
	return iter->node->key;
}

