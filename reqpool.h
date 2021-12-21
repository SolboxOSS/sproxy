#ifndef __REQ_POOL_H__
#define __REQ_POOL_H__

struct request_mem_pool;
struct request_mem_pool * mp_create(size_t size);
void * mp_alloc(struct request_mem_pool *mp, size_t size);
void * mp_realloc(struct request_mem_pool *mp, void *p, size_t size);
void mp_free(struct request_mem_pool *mp);
size_t mp_consumed(struct request_mem_pool *pool);
char * mp_strdup(struct request_mem_pool *mp, const char *string);

#endif /* __REQ_POOL_H__ */
