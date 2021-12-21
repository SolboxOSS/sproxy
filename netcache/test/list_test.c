#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <netcache_types.h>
#include <util.h>
#include <ncapi.h>
#include <assert.h>

link_list_t 			L = LIST_INITIALIZER;

typedef struct N{
	int					value;
	link_node_t			node;
} my_data_t;
int 
my_compare(void *a, void *b)
{
	my_data_t		*ma = a;
	my_data_t		*mb = b;

	return mb->value - ma->value;
}
main()
{
	my_data_t		*arr;
	int				i;
	my_data_t		*cursor;

	arr = calloc(100, sizeof(my_data_t));

	for (i = 0; i < 100; i++) {
		arr[i].value = rand() % 10000;
		link_add_n_sort(&L, &arr[i], &arr[i].node,  my_compare);
	}

	i = 0;

	cursor = (my_data_t *)link_get_head(&L);

	while (cursor) {
		printf("[%2d] => %d\n", i++, cursor->value);
		cursor = (my_data_t *)link_get_head(&L);
	}
}
