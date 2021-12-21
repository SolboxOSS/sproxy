#include <stdio.h>
#define		L_HEAD(l_)	(l_)->head
#define		L_TAIL(l_)	(l_)->tail
#define		SET_NODE(_x, _d) {(_x)->data = _d; (_x)->next = (_x)->prev = NULL; }

#define 	LIST_INITIALIZER	{NULL, NULL, 0, 0}
typedef struct tag_link_node {
#ifdef LL_DEBUG
	unsigned short 			magic;
#endif

	void					*data;
	struct tag_link_node	*next;
	struct tag_link_node	*prev;

#ifdef LL_DEBUG 
	char 				func[64];
	int 				line;
	unsigned short 			emagic;
#endif
} link_node_t;
typedef struct tag_link_list {
	struct tag_link_node	*head;
	struct tag_link_node	*tail;
	int 					count;
	int						ref;
} link_list_t;

void 
link_add_n_sort(link_list_t *list, void *data, link_node_t *m, int (*_comp)(void *a, void *b), const char *f, int l)
{
	SET_NODE(m, data);

    // Case 1: list is empty
    if (L_HEAD(list) == NULL) {
        m->next = NULL;
        m->prev = NULL;
		L_TAIL(list) = m;
		L_HEAD(list) = m;
        return;
    }

#if 0
    // Case 2: list is non-empty
    link_node_t *prev = L_HEAD(list);
    // we know list->head != NULL
    link_node_t *cur = L_HEAD(list)->next;
#else
    link_node_t *prev = NULL;
    link_node_t *cur  = L_HEAD(list);
#endif

    while (cur) {
        if (_comp(cur->data, m->data) < 0)
            break;

        prev = cur;
        cur = cur->next;
    }

    if (cur) {
        // Sub-case 1: insert before head
        if (cur == L_HEAD(list)) {
            m->next = L_HEAD(list);
            L_HEAD(list)->prev = m;
            L_HEAD(list) = m;
            return;
        }

        // Sub-case 2: insert after head
		prev->next 	= m;
        m->prev 	= prev;
		m->next		= cur;
        cur->prev 	= m;
    } else {
		// Case 3: List insertion is at end of list
		// so cur is empty
		m->next = NULL;
		m->prev = prev;

		if (prev) prev->next = m;

		L_TAIL(list) = m; 

    }
}
/* get data from head noremove */
void *
link_get_head_noremove(link_list_t *list)
{
	link_node_t	*node;
	void		*data = NULL;

	node = L_HEAD(list);
	if (node) {
		data = node->data;
	}
	return data;
}
void *
link_get_next(link_list_t *list, link_node_t *node)
{
	return (node->next)? (node->next)->data:NULL;
}
typedef struct {
	int			value;
	link_node_t	node;
} my_type_t;

int mycomp(void *a, void *b)
{
	my_type_t 	*ta = (my_type_t *)a;
	my_type_t 	*tb = (my_type_t *)b;

	return (int)(tb->value - ta->value);

}
	link_list_t		L = LIST_INITIALIZER;
main()
{
	my_type_t		*nav;

	my_type_t		v1, v2, v3, v4;

	v4.value = 4;
	v1.value = 1;
	v2.value = 2;
	v3.value = 3;

	link_add_n_sort(&L, &v3, &v3.node, mycomp, __FILE__, __LINE__);
	link_add_n_sort(&L, &v1, &v1.node, mycomp, __FILE__, __LINE__);
	link_add_n_sort(&L, &v4, &v4.node, mycomp, __FILE__, __LINE__);
	link_add_n_sort(&L, &v2, &v2.node, mycomp, __FILE__, __LINE__);

	nav = link_get_head_noremove(&L);
	while (nav) {
		fprintf(stderr, "* %d\n", nav->value);
		nav = link_get_next(&L, &nav->node);
	}

}
