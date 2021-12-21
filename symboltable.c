#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

//#include <ncapi.h>
#include "common.h"
#include "reqpool.h"
#include "vstring.h"

#include "../include/rdtsc.h"
#include "site.h"


typedef struct symbol_data_tag {
	vstring_t 		*var; 
	void 			*value;
	size_t 			value_size;
} i_symbol_data_t;
typedef struct symbol_table_tag {
	int 			alloced;
	int 			count;
	i_symbol_data_t *table;
} i_symbol_table_t;

static int st_comp(const void *m1, const void *m2);

i_symbol_table_t *
st_create(mem_pool_t *pool, int tblsize)
{
	i_symbol_table_t *T;

	T = (i_symbol_table_t *)mp_alloc(pool, sizeof(i_symbol_table_t));
	T->table = (i_symbol_data_t *)mp_alloc(pool, tblsize*sizeof(i_symbol_data_t));
	T->alloced = tblsize;
	T->count = 0;
	return T;
}
//overwrite가 0이 아닌 경우는 해당 key를 overwrite한다.
//trigger의  경우는 overwrite를 하지 않고 add를 해야 한다.
int
st_put(mem_pool_t *pool, i_symbol_table_t *T, const char *var, void *data, size_t datalen, int overwrite)
{
	int 	found = -1;
	int 	replaced = 1;
	int 	i;
	/* lookup */
	for (i = 0; i < T->count; i++) {
		/* 구축 중이므로, table scan밖에 사용할 수 없음 */
		if (strcasecmp(vs_data(T->table[i].var), var) == 0) {
			found = i;
			break;
		}
	}
	if (found < 0 || overwrite == SITE_KEY_ADD) {
		if (T->count >= T->alloced) {
			T->alloced += 5;
			T->table = (i_symbol_data_t *)mp_realloc(pool, T->table, T->alloced*sizeof(i_symbol_data_t));
		}
		found = T->count;
		T->count++;
	}
	if (T->table[found].var == NULL) {
		/* add mode */
		replaced = 0;
		T->table[found].var = vs_strdup_lg(pool, var);
	}
	T->table[found].value = mp_alloc(pool, datalen);
	memcpy(T->table[found].value, data, datalen);
	T->table[found].value_size = datalen;
	return replaced;
}
void *
st_get(i_symbol_table_t *T, const char *var)
{
	void *data = NULL;
	vstring_t 	key = {strlen(var), strlen(var), (char *)var};
	i_symbol_data_t 	keysym = {&key, NULL, 0};
	i_symbol_data_t 	*found = NULL;
	/* 구축이 끝났다고 가정, bsearch를 사용할 수 있음 */
	found = (i_symbol_data_t *)bsearch(&keysym, T->table, T->count, sizeof(i_symbol_data_t), st_comp);
	return (found?found->value:NULL);
	
}

void *
st_get_by_index(i_symbol_table_t *T, int idx)
{
	if (idx >= T->count) return NULL;

	return (void *)T->table[idx].value;
}
static int
st_comp(const void *m1, const void *m2)
{
	i_symbol_data_t 	*v1 = (i_symbol_data_t *)m1;
	i_symbol_data_t 	*v2 = (i_symbol_data_t *)m2;
	return strcasecmp((const char *)vs_data(v1->var), (const char *)vs_data(v2->var));
}


void 
st_commit(i_symbol_table_t *T)
{
	qsort(T->table, T->count, sizeof(i_symbol_data_t), st_comp);
}
i_symbol_table_t *
st_clone_table(mem_pool_t *pool, i_symbol_table_t *T)
{
	i_symbol_table_t *	clone;
	int 				i;
	clone = (i_symbol_table_t *)mp_alloc(pool, sizeof(i_symbol_table_t));
	clone->table = (i_symbol_data_t *)mp_alloc(pool, T->alloced*sizeof(i_symbol_data_t));
	clone->alloced = T->alloced;
	clone->count = 0;

	for (i = 0; i < T->count; i++) {
		st_put(pool, clone, vs_data(T->table[i].var), T->table[i].value, T->table[i].value_size, SITE_KEY_OVERWRITE);
	}
	return clone;
}
