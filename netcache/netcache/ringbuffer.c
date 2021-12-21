#include <config.h>
#ifndef MODULE_TEST
#include <netcache_config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_SYS_DIR_H
#include <sys/dir.h>
#endif
#ifdef HAVE_DIR_H
#include <dir.h>
#endif

#ifndef MODULE_TEST
#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#include <sys/vfs.h>
#include <ctype.h>

#include "disk_io.h"
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "bitmap.h"
#include "trace.h"
#include "bt_timer.h"
#include <mm_malloc.h>
#include <snprintf.h>


#else /* MODULE_TEST */
#include	<pthread.h>
#include	<sys/time.h>
#include 	<syscall.h>
#include 	<assert.h>

typedef int				nc_int32_t;
typedef unsigned char	nc_uint8_t;
typedef unsigned int	nc_uint32_t;
typedef unsigned long long	nc_uint64_t;
typedef struct tag_nc_ringbuffer nc_ringbuffer_t;


#define		ASSERT(x)	 	assert(x)
#define 	XMALLOC(_x,_y) 	malloc(_x)
#define 	XFREE(_x) 	free(_x)
#define		U_TRUE		1
#define		U_FALSE		0

static void condition_init(pthread_cond_t *cond);
static void make_timeout( struct timespec *to, long msec, clockid_t cid);
int rb_get_error(nc_ringbuffer_t *ring, int rw);
#define	MUTEX_INIT(_l, _x)	pthread_mutex_init(_l, _x)
#define	MUTEX_DESTROY(_l)	pthread_mutex_destroy(_l)

#ifndef max
#define 	max(a,b) 	(a>b?a:b)
#endif
#ifndef min
#define 	min(a,b) 	(a>b?b:a)
#endif

#endif /* MODULE_TEST */




/*
 * 	one producer/one consumer only support
 * 
 *  메모리 복사를 최소화하는 버전.
 *  (1) write 시
 *   	rinbuffer 사용자는 rb_get_writable을 이용해서 write 가능한 주소와, 크기를 받아온뒤
 *  	해당 주소에 최대 받아온 크기 또는 그보다 작은 바이트 수만큼을 저장 후
 * 		rb_commit_write 함수를 호출하여, 저장결과가 consumer와 공유되도록 한다
 * 
 */








struct tag_nc_ringbuffer {
	char			*buffer;
	char			*head; 
	char 			*tail;
	int				dataidx; 	/* 저장된 데이타의 첫번째 바이트 위치 */
	int				freeidx; 	/* write를 시작하는 위치 */
	nc_uint32_t		flag; 		/* error flag */
	int				wfx;		/* write offset */
	int				rfx;		/* read offset */
	int				datasiz;	/* 저장된 데이터 크기 */
	int				buffsiz;   	/* write가능영역 크기 */
	int 			size;


	/* house keeping */
	nc_int32_t		totread;
	nc_int32_t		totwrite;

	void * 			(*allocator)(void *cb, size_t sz);
	void * 			*allocator_cbdata;
#ifndef NC_RELEASE_BUILD
	fc_inode_t 		*inode;
#endif
	pthread_mutex_t lock;
	pthread_cond_t 	event[2];
};



static void rb_commit_update_nolock(nc_ringbuffer_t *ring);


nc_ringbuffer_t *
rb_create(size_t size, void *(*allocator)(void *cb, size_t sz), void *cbdata)
{
	nc_ringbuffer_t *ring = NULL;
	pthread_mutexattr_t	mattr;


	if (allocator) {
		ring 			= (nc_ringbuffer_t *)(*allocator)(cbdata, sizeof(nc_ringbuffer_t));
		ring->buffer 	= (char *)(*allocator)(cbdata,  size);
	}
	else {
		ring 			= (nc_ringbuffer_t *)XMALLOC(sizeof(nc_ringbuffer_t), AI_ETC);
		ring->buffer 	= (char *)XMALLOC(size, AI_ETC);
	}

	ring->size 		= size + 1;

	ring->buffsiz 	= ring->size - 1;
	ring->head 		= ring->tail = ring->buffer;
	ring->datasiz	= 0;
	ring->rfx		= ring->wfx	 = 0;


	ring->allocator = allocator;
	ring->allocator_cbdata = cbdata;

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&ring->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);
	condition_init(&ring->event[0]);
	condition_init(&ring->event[1]);

	return ring;
}
static  void
rb_commit_update_nolock(nc_ringbuffer_t *rb)
{
	if(rb->freeidx == rb->dataidx)
		rb->datasiz = 0;
	else if(rb->freeidx < rb->dataidx)
		rb->datasiz = rb->freeidx + (rb->size - rb->dataidx);
	else
		rb->datasiz = rb->freeidx - rb->dataidx;
	rb->buffsiz = rb->size - rb->datasiz - 1;
}
void
rb_bind_inode(nc_ringbuffer_t *ring, void *inode)
{
#ifndef NC_RELEASE_BUILD
	if (ring)
		ring->inode = (fc_inode_t *)inode;
#endif
}
void
rb_destroy(nc_ringbuffer_t *ring)
{
	pthread_mutex_destroy(&ring->lock);
	pthread_cond_destroy(&ring->event[0]);
	pthread_cond_destroy(&ring->event[1]);
	if (!ring->allocator) {
		XFREE(ring->buffer);
		XFREE(ring);
	}

#if 0
	close(ring->rfd);
	close(ring->wfd);
#endif
}
int
rb_lock(nc_ringbuffer_t *ring)
{
	return pthread_mutex_lock(&ring->lock);
}
int
rb_unlock(nc_ringbuffer_t *ring)
{
	return pthread_mutex_unlock(&ring->lock);
}

size_t
rb_size(const nc_ringbuffer_t *rb)
{
    return rb->size;
}

#if NOT_USED
static void
rb_clr_flag(nc_ringbuffer_t *rb, nc_uint32_t flg)
{
	rb->flag ^= flg;
}
#endif
void
rb_set_flag(nc_ringbuffer_t *rb, nc_uint32_t flg)
{
	int 		iot = 0;
	int			__t;
	char		__zflg[32]="";
	RB_TRANSACTION(rb, U_TRUE) {
		rb->flag |= flg;
		switch (flg) {
			case RBF_EOD: /* end of data: read finished */
				iot = RIOT_BUFFER; /* writer쪽에 신호보내야함*/
				strcpy(__zflg, "RIOT_EOD");
				break;
			case RBF_EOT: /* 일종의 data event */
				iot = RIOT_DATA;
				strcpy(__zflg, "RIOT_EOT");
				break;
			case RBF_READER_BROKEN:
				iot = RIOT_BUFFER; /* writer측에서 알아야할 buffer event*/
				strcpy(__zflg, "RIOT_READER_BROKEN");
				break;
			case RBF_WRITER_BROKEN:
				iot = RIOT_DATA; /* 일종의 data event */
				strcpy(__zflg, "RIOT_WRITER_BROKEN");
				break;
		}
		pthread_cond_signal(&rb->event[iot]);
	}
}
int
rb_flag(nc_ringbuffer_t *rb, nc_uint32_t flg)
{
	return ((rb->flag & flg) != 0);
}
void
rb_set_eot(nc_ringbuffer_t *rb)
{
	rb_set_flag(rb, RBF_EOT);
}
int
rb_eot(nc_ringbuffer_t *rb)
{
	return rb_flag(rb, RBF_EOT);
}
int
rb_exception(nc_ringbuffer_t *rb)
{
	return (rb->flag != 0);
}
/*
 *  주의) 이미  lock 획득상태라야함
 *  RETURN
 * 		> 0  : 데이타가 있거나, 버퍼가 있음
 *		< 0  : error occured
 *		== 0 : timeout
 */
int
rb_wait_for_nolock(nc_ringbuffer_t *rb, int iot, nc_int32_t msec)
{
	struct timespec		ts;
	int					available = 0;
	int					r = 0;
	
	

	if (iot == RIOT_BUFFER)  {
		if (rb_flag(rb, RBF_EOD|RBF_READER_BROKEN)) return -1; /* reader broken */
	}
	available = (iot == RIOT_DATA)?(rb->datasiz >  0 /* data avail*/):(rb->buffsiz > 0 /*buff avail*/);

	if (available) return available; /* data 또는 버퍼가 있음 */

	if ((iot == RIOT_DATA) && rb_flag(rb, RBF_EOT|RBF_WRITER_BROKEN)) return -1;


	if (msec > 0) {
		make_timeout( &ts, msec, CLOCK_MONOTONIC);
		r = pthread_cond_timedwait(&rb->event[iot], &rb->lock, &ts);
	}
	else if (msec < 0) {
		r = pthread_cond_wait(&rb->event[iot], &rb->lock);
	}

	available = (iot == RIOT_DATA)?(rb->datasiz >  0 /* data avail*/):(rb->buffsiz > 0 /*buff avail*/);

	if (available) 
		r =  available; /* data 또는 버퍼가 있음 */
//	else if ((iot == RIOT_DATA) && rb_flag(rb, RBF_EOT|RBF_WRITER_BROKEN)) 
//		r =  -1;
	else if (r == ETIMEDOUT) 
		r = 0; /* timeout */
	else
		r = -1;

	return r;
}


void
rb_free(nc_ringbuffer_t *rb)
{
    XFREE(rb->buffer);
    XFREE(rb);
}






size_t
rb_write(nc_ringbuffer_t * rb, char *buff, size_t count)
{
	int			__t;
	int 		write_size = 0;


	RB_TRANSACTION(rb, U_TRUE) {
		TRACE((0, ">>>>>{F/D=(%d, %d),count=%d \n", rb->freeidx, rb->dataidx, count));

		write_size = min(count, rb->buffsiz);

		if (rb->freeidx >= rb->dataidx) { /* write pointer가 앞서있음*/
			if (write_size < (rb->size - rb->freeidx)) {
				memcpy(&rb->buffer[rb->freeidx], buff, write_size);
				rb->freeidx += write_size;
			}
			else {
				/* 일부 write 후 앞으로 돌아가서 write재개 */
				int to_end = rb->size - rb->freeidx;
				memcpy(&rb->buffer[rb->freeidx], buff, to_end);
				memcpy(rb->buffer, buff + to_end, write_size - to_end);
				rb->freeidx = write_size - to_end;
			}
		}
		else { /* data pointer가 앞에 있어서 걍 write함 */
			memcpy(&rb->buffer[rb->freeidx], buff, write_size);
			rb->freeidx += write_size;
		}
		if (write_size > 0) {
			pthread_cond_signal(&rb->event[RIOT_DATA]);
			rb->totwrite += write_size;
			rb_commit_update_nolock(rb);
		}
	}
    return write_size;
}
size_t
rb_read(nc_ringbuffer_t *rb, char *zdata, size_t count)
{
	int			__t;
	int 		read_size = 0;
	int 		to_end = rb->size - rb->dataidx;


	RB_TRANSACTION(rb, U_TRUE) {
		read_size = min(count, rb->datasiz);
		if (read_size > 0) {
#if 0
        	if(rb->wfx > rb->dataidx || to_end >= read_size) {
           	 	memcpy(zdata, &rb->buffer[rb->dataidx], read_size);
           	 	rb->dataidx += read_size;
        	}
			else { // otherwise we have to wrap around the buffer and copy octest in two times
				memcpy(zdata, &rb->buffer[rb->dataidx], to_end);
				memcpy(zdata+to_end, &rb->buffer[0], read_size - to_end);
				rb->dataidx = read_size - to_end;
			}
#else
        	if(rb->freeidx > rb->dataidx || to_end >= read_size) {
           	 	memcpy(zdata, &rb->buffer[rb->dataidx], read_size);
           	 	rb->dataidx += read_size;
        	}
			else { // otherwise we have to wrap around the buffer and copy octest in two times
				memcpy(zdata, &rb->buffer[rb->dataidx], to_end);
				memcpy(zdata+to_end, &rb->buffer[0], read_size - to_end);
				rb->dataidx = read_size - to_end;
			}
#endif
		}
		if (read_size > 0) {
			rb_commit_update_nolock(rb);
			pthread_cond_signal(&rb->event[RIOT_BUFFER]);
		}
		rb->totread += read_size;
	}
    return read_size;
}
char *
rb_dump(nc_ringbuffer_t *rb, char *buf, int len)
{
	int 	n;
	int		rem = len-1;
	char 	*p = buf;

	n = snprintf(p, rem, "flag(");
	p += n;
	rem -= n;

	if (rb_flag(rb, RBF_EOT)) {
		n = snprintf(p, rem, "EOT ");
		p += n;
		rem -= n;
	}
	if (rb_flag(rb, RBF_EOD)) {
		n = snprintf(p, rem, "EOD ");
		p += n;
		rem -= n;
	}
	if (rb_flag(rb, RBF_WRITER_BROKEN)) {
		n = snprintf(p, rem, "W_BROKEN ");
		p += n;
		rem -= n;
	}
	if (rb_flag(rb, RBF_READER_BROKEN)) {
		n = snprintf(p, rem, "R_BROKEN ");
		p += n;
		rem -= n;
	}
	n = snprintf(p, rem, ")");
	p += n;
	rem -= n;

	n = snprintf(p, rem, "avail.data(%d), avail.buffer(%d)", rb->datasiz, rb->buffsiz);
	p += n;
	rem -= n;
	*p = 0;
	return buf;
}
