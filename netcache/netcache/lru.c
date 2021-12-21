#include <config.h>
#include <stdio.h>
#include <netcache_config.h>
#include <netcache_types.h>
#include <pthread.h>
#include <errno.h>
#include <lru.h>
#include <util.h>



int
LRU_count(fc_lru_root_t *root)
{
	return root->lru_cnt;
}


fc_lru_root_t * 
LRU_create(int signature, int cap, fc_check_idle_t idle_proc, fc_dump_t dump_proc)
{
	fc_lru_root_t	*root = NULL;


	root = (fc_lru_root_t *)XMALLOC(sizeof(fc_lru_root_t), AI_ETC);



	root->id 			= signature;
	root->signature 	= signature; /*시그너처는 band간에 서로 다른값이면 충분*/
	root->lru_cap 		= cap;
	root->lru_cnt 		= 0;
	root->check_idle 	= idle_proc;
	root->dump  	 	= dump_proc;

	INIT_LIST(&root->list);

	return root;
}

int
LRU_destroy(fc_lru_root_t *root, fc_destructor_t free_proc)
{

	void 	*data;
	int 	freed = 0;


	data = link_get_head(&root->list);

	while (data) {
		if (free_proc) free_proc(data);
		data = link_get_head(&root->list);
		freed++;
	}

	XFREE(root);

	return freed;
}
int 
LRU_add(fc_lru_root_t *root, void *data, lru_node_t *node)
{
  int 	r;

  LRU_assign_node(node, root);
  r = link_prepend(&root->list, data, &node->bnode);
  root->lru_cnt++;



  return r;
}

int 
LRU_remove(fc_lru_root_t *root, lru_node_t *ln)
{


  link_del(&root->list, &ln->bnode);
  root->lru_cnt--;


  return 0;
}

int 
LRU_hit(fc_lru_root_t *root, lru_node_t *node)
{
	
	int 	r;

	r = link_move_head(&root->list, &(node->bnode));

	
	return r;
}

lru_node_t *
LRU_reclaim_node(fc_lru_root_t *root, int *navi)
{
  link_node_t	*node;
  lru_node_t 	*ln = NULL, *victim = NULL;




  	node = root->list.tail;

  	while (victim == NULL && node != NULL) {

		(*navi)++;

		ln = (lru_node_t *)node; /* ln은 node를 포함 */

        if (root->check_idle && (*root->check_idle)(node->data) == 0) {
			/*
			 * check_idle함수가대상 노드가 busy라고 리턴
			 */
			node = link_get_node_prev(&root->list, node);
		}
		else {
			/*
			 * idle
			 */
			if (ln->clockstamp != root->signature) {
				/* node 정보 보정*/
				ln->root 	 	= (void *)root;
				ln->clockstamp 	= root->signature;
				ln->hitcount   	= 1;  /* 0일 수도 있음...하지만 일단 1로 보정함 */
			}
			LRU_remove(root, ln);
			victim = ln;
		}
  	}
  	
  	return victim;
}
int
LRU_join(fc_lru_root_t *dest, fc_lru_root_t *src)
{
	int	n;

	n = link_join(&dest->list, &src->list);

	dest->lru_cnt 	=  dest->lru_cnt + src->lru_cnt;
	src->lru_cnt   	= 0;

	src->signature = time(NULL); /* 조인 후 조인된 node들이 signature비교시 root갱신을 알리도록 */

	return n;
}

char * 
LRU_dump(fc_lru_root_t *root, fc_dump_node_proc_t proc, int forward, int *cnt)
{
#define	DUMP_BUF_SIZ	20480
  link_node_t	*node;
  char			*buf = NULL, *pbuf;
  int			n, remained = DUMP_BUF_SIZ;

  int			nodecnt = 0;

  if (cnt) *cnt = 0;
  if (proc) {

	  if (!buf)
	  	pbuf = buf = (char *)XMALLOC(DUMP_BUF_SIZ+1, AI_ETC);
	  else
	  	pbuf = buf;
	  node = (forward?root->list.head:root->list.tail);
	  n = snprintf(pbuf, remained, "(");
	  pbuf += n; remained -= n;
	  while (node && remained > 0) {
		if (proc) n = (*proc)(pbuf, remained, node->data);
		pbuf += n; remained -= n;
		*pbuf++ = ' '; remained--;
		node = (forward?node->next:node->prev);
		if (cnt) (*cnt)++;
		nodecnt++;
		if (nodecnt > 0 && (nodecnt % 12 == 0)) {
			*pbuf++ = '\n'; remained -= 1;
		}

	  }
	  *pbuf++ = ')';
	  *pbuf = 0;
	}
	 return buf;
}
char * 
LRU_for_each(fc_lru_root_t *root, int (*proc)(void *data, void *), void *uc, int forward)
{
	link_node_t	*node, *nextnode;

	if (proc) {
		node = (forward?root->list.head:root->list.tail);
			while (node) {
				nextnode = (forward?node->next:node->prev);
				if (proc) (*proc)(node->data, uc);
				node = nextnode;
			}
	}
	return NULL;
}

