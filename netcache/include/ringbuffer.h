#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__

struct tag_nc_ringbuffer;
typedef struct tag_nc_ringbuffer nc_ringbuffer_t;

#define	 RB_TRANSACTION(_r,_n)		for (__t=0, _n && rb_lock(_r);__t == 0; __t++, _n && rb_unlock(_r))
#define	 RB_UNLOCK(_r,_n)			_n && rb_unlock(_r)


/*
 * ringbuffer flags
 */
#define		RBF_EOT					(1 << 0)
#define		RBF_EOD					(1 << 1)
#define		RBF_READER_BROKEN		(1 << 2)
#define		RBF_WRITER_BROKEN		(1 << 3)



#define		RIOT_BUFFER			0
#define		RIOT_DATA			1


nc_ringbuffer_t * rb_create(size_t size, void *(*allocator)(void *cb, size_t sz), void *cbdata );
size_t rb_write(nc_ringbuffer_t * rb, char *buff, size_t count);
size_t rb_read(nc_ringbuffer_t *rb, char *zdata, size_t count);
int rb_lock(nc_ringbuffer_t *ring);
int rb_unlock(nc_ringbuffer_t *ring);
void rb_destroy(nc_ringbuffer_t *ring);
int rb_set_error(nc_ringbuffer_t *ring, int);
int rb_get_error(nc_ringbuffer_t *ring);
size_t rb_free_size(const nc_ringbuffer_t *rb);
void rb_set_flag(nc_ringbuffer_t *ring, nc_uint32_t v);
int rb_eot(nc_ringbuffer_t *ring);
size_t rb_data_size(const nc_ringbuffer_t *rb);
int rb_wait_for_nolock(nc_ringbuffer_t *rb, int iot, nc_int32_t msec);
int rb_exception(nc_ringbuffer_t *rb);
size_t rb_capacity(const nc_ringbuffer_t * rb);
int rb_flag(nc_ringbuffer_t *rb, nc_uint32_t flg);
void rb_set_eot(nc_ringbuffer_t *rb);
void rb_bind_inode(nc_ringbuffer_t *ring, void *inode);
char * rb_dump(nc_ringbuffer_t *rb, char *buf, int len);

#endif /* __RINGBUFFER_H__ */
