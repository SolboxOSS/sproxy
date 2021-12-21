#ifndef 	__KV_H__
#define 	__KV_H__

typedef struct {
	link_list_t 	list;
	size_t 			size; /* total size */
} nc_kv_list_t;
typedef struct {
	char 			*key;
	char 			*value;
	link_node_t 	node;
} nc_kv_t;
nc_xtra_options_t * kv_create();
nc_xtra_options_t * kv_create_d(const char *f, int l);
nc_xtra_options_t * kv_create_pool(void *cb, void *(allocator *)(void *cb, size_t sz));
nc_xtra_options_t * kv_clone_d(nc_xtra_options_t *root, const char *file, int l);
int kv_valid(nc_xtra_options_t *opt);
nc_xtra_options_t * kv_clone(nc_xtra_options_t *root);
void *kv_add_val(nc_kv_list_t *root, char *key, char *val);
nc_kv_t * kv_add(nc_kv_t *root, nc_kv_t *toadd);
nc_kv_t * kv_find_key(nc_kv_t *root, const char *key, int bcase);
void kv_destroy(nc_kv_list_t *root) ;
char * kv_dump_oob(char *buf, int l, nc_xtra_options_t *opt);

#endif
