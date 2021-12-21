/*
 *	
 * 	netcache 2.7 수정 방향
 * 		OS별 서로 다른 implementation이 일부 코드는 공유하고 일부는
 * 		ifdef를 통해서 서로 다르게 구현되어 있음
 * 		
 *		LINUX - libaio를 통한 비동기 vector 방식의 IO 구현
 *
 *	TODO
 * 		NULL terminating page의 처리 방법,어디서 할당하나? 
 */
#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include "bitmap.h"

#ifdef HAVE_FCNTL_H
#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <fcntl.h>

#endif /* HAVE_FCNTL_H*/

#include <sys/stat.h>
#ifdef HAVE_SYS_TIME_H 
#include <sys/time.h>
#endif


#include <unistd.h>
#ifdef HAVE_LIBAIO
#include <libaio.h>
#else
#error "Package libaio-devel should be installed"
#endif

#include <time.h>
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <netcache.h>
#include <trace.h>
#include <hash.h>
#include <disk_io.h>
#include "util.h"
#include "hash.h"
#include "disk_io.h"
#include "block.h"
#include "asio.h"
#include "bt_timer.h"
#include "ncapi.h"
#include "threadpool.h"

#define		IO_MAGIC	0x5AA5


/* statistics */
extern int				g__per_block_pages;
extern int				__terminating ;
extern mavg_t			*g__dskio; /* disk block io 시간*/




typedef struct io_thread_info {
	pthread_t 	tid;
	int			idle;
	int		shutdown; /* 2 indicates the corresponding thread should be gracefull shutdowned */
					  /* 1 shutdowned ok */
					  /* 0 running */
} io_thread_info_t;




// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603)
//                - 원부사장님 수정 소스 적용
//                - 해당 값을 512 -> 4*1024 로 변경
#define	DIO_MAX_AIO			(32*1024)


int							s__wait_ios			= 0;
int							s__wait_read_ios 	= 0;
int							s__wait_write_ios 	= 0;


extern void *				__timer_wheel_base;
extern char 				_str_iot[][24];
extern int 					__enable_compaction;

struct nc_iocb;

typedef void (*dio_io_callback_t)(struct nc_iocb *iocb, size_t len, nc_int32_t err);


#pragma pack(4)
typedef struct nc_iocb_xtra {
	unsigned short		magic;
	unsigned short 		bidx;
	int					iot;
	nc_asio_vector_t 	*vector;
	fc_inode_t			*file;
	nc_part_element_t	*part;
	void				*cb;
	struct iovec		iov[0];
} iocb_xtra_t;
#pragma pack()

typedef struct nc_iocb {
	nc_apcop_t		apc_op;
	struct iocb		iocb;
	link_node_t 	node;
	perf_val_t		t_s;
	perf_val_t		t_e;

	int				xfered;
	int				error;

	iocb_xtra_t		iocb_xtra; /* iov때문에 아래 확장됨, node위치는 위에 놓아야함 */
} nc_iocb_t;
static void __dio_io_done(io_context_t ctx, struct iocb *aio_iocb, long transfered, long error);
static int dio_handle_completed(void *, void *);
static void __dio_prepare_iocb_pool();




static io_context_t		s__libaio_queue;
/*
 * OS에서 요구하는 요청 구조체 할당 관리
 */

static nc_iocb_t 		*s__iocb_pool_array = NULL;
static link_list_t  	s__iocb_pool;
static pthread_mutex_t  s__iocb_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static int				s__max_iocbs	= 0;
static pthread_mutex_t  s__iox_lock = PTHREAD_MUTEX_INITIALIZER;
static tp_handle_t		s__kaio_tp;



int
dio_get_wait(int *maxios, int *pending)
{
	*maxios = s__max_iocbs;
	//*wait_io = s__wait_ios;
	pthread_mutex_lock(&s__iocb_pool_lock);
	*pending = s__max_iocbs - link_count(&s__iocb_pool, U_TRUE);
	pthread_mutex_unlock(&s__iocb_pool_lock);
	return *pending;
}

/*
 * 현재 진행 중인 io 카운터 재조정
 */
static void
dio_count_ioq(nc_part_element_t *part, int opcode, int isdone, int cnt)
{
	switch (opcode) {
		case IOT_CACHE_READ:
			if (isdone) {
				_ATOMIC_SUB(s__wait_ios, cnt);
				pm_update_ios(part, -cnt);
				_ATOMIC_SUB(s__wait_read_ios, cnt);
			}
			else {
				_ATOMIC_ADD(s__wait_ios, cnt);
				pm_update_ios(part, cnt);
				_ATOMIC_ADD(s__wait_read_ios, cnt);
			}
			break;
		case IOT_CACHE_WRITE:
			if (isdone) {
				_ATOMIC_SUB(s__wait_ios, cnt);
				pm_update_ios(part, -cnt);
				_ATOMIC_SUB(s__wait_write_ios, cnt);
			}
			else {
				pm_update_ios(part, cnt);
				_ATOMIC_ADD(s__wait_ios, cnt);
				_ATOMIC_ADD(s__wait_write_ios, cnt);
			}
			break;
		default:
			TRACE((T_ERROR, "DIO - UNKNOWN OPCODE\n"));
			break;
	}
}


static void
__dio_prepare_iocb_pool()
{
	long				perunit = 0;
	long				perunita = 0;
	int					i;
	nc_iocb_t 			*nciocb = NULL;
	


	perunit	= (NC_BLOCK_SIZE / NC_PAGE_SIZE) * sizeof(struct iovec) + sizeof(nc_iocb_t);
	perunita= NC_ALIGNED_SIZE(perunit, 8);

	TRACE((T_INFO, "NCIOCB=%d B, pages/blk=%d, sizeof(iovec)=%d B, perunit=%d B, perunit(align)=%d B\n", 
				sizeof(nc_iocb_t), (NC_BLOCK_SIZE/NC_PAGE_SIZE), sizeof(struct iovec), perunit, perunita));

	s__max_iocbs = 2*DIO_MAX_AIO;
	s__iocb_pool_array = (nc_iocb_t *)XCALLOC(s__max_iocbs, perunit, AI_ETC);
	DEBUG_ASSERT(s__iocb_pool_array != NULL);
	INIT_LIST(&s__iocb_pool);
	for (i = 0; i < s__max_iocbs;i++) {
		nciocb = (nc_iocb_t *)((char *)s__iocb_pool_array + i * perunit);
		nciocb->iocb_xtra.magic = IO_MAGIC;
		__dio_put_free(nciocb);
	}



}
static nc_iocb_t *
__dio_get_free(tp_data_t tcb)
{
	nc_iocb_t	*nciocb = NULL;
	pthread_mutex_lock(&s__iocb_pool_lock);
	nciocb = (nc_iocb_t *)link_get_head(&s__iocb_pool);
	pthread_mutex_unlock(&s__iocb_pool_lock);
	if (nciocb == NULL) {
		int 		max_diocb, pending_diocb;

		dio_get_wait(&max_diocb, &pending_diocb);
		TRACE((T_ERROR, "CRITICAL:no free iocb available (%d/%d used)\n", pending_diocb, max_diocb));
		exit(1);
	}
	memset(nciocb, 0, sizeof(nc_iocb_t));
	nciocb->iocb_xtra.magic = IO_MAGIC;
	return nciocb;
}
void
__dio_put_free(void *v_nciocb)
{
	nc_iocb_t *nciocb = (nc_iocb_t *)v_nciocb;
#if 0
	DEBUG_ASSERT(nc_check_memory(nciocb, sizeof(nc_iocb_t)) != 0);
	DEBUG_ASSERT(nciocb->iocb_xtra.magic == IO_MAGIC);
	nciocb->iocb_xtra.magic = 0;
	XFREE(nciocb);
#else
	DEBUG_ASSERT(nciocb->iocb_xtra.magic == IO_MAGIC);
	nciocb->iocb_xtra.magic = ~IO_MAGIC;
	pthread_mutex_lock(&s__iocb_pool_lock);
	link_append(&s__iocb_pool, nciocb, &nciocb->node);
	pthread_mutex_unlock(&s__iocb_pool_lock);
#endif
}
/*
 *  @off : 캐시 파일 내의 옵셋
 */
static void
dio_io_prep_pread(nc_iocb_t *nciocb, fc_inode_t *inode, fc_blk_t *block, nc_off_t off, void *data)
{
	int 	i;

	if (!BLOCK_VALID(inode, block, block->blkno)) {
		char	bbuf[256];
		char	ibuf[512];
		TRACE((T_ERROR, 	"INODE[%u].Offset[%lld] - BLOCK{%s} invalid;INODE{%s}\n",
							inode->uid, 
							off,
							bcm_dump_lv1(bbuf, sizeof(bbuf), block),
							dm_dump_lv1(ibuf, sizeof(ibuf), inode)
							)); 
		TRAP;
	}

	nciocb->iocb_xtra.cb			= data;
	nciocb->iocb_xtra.file			= inode;



	for (i = 0; i < block->mappedcnt;i++) {
		nciocb->iocb_xtra.iov[i].iov_base = (char *)BLOCK_PAGE_MEMORY(block->pages[i]);
		nciocb->iocb_xtra.iov[i].iov_len  = NC_PAGE_SIZE;
	}
	TRACE((T_INODE|T_BLOCK, "INODE[%u] - blk#%u[#P:%d] prepared for disk-io(off=%lld)\n", 
							inode->uid, 
							block->blkno, 
							block->mappedcnt,
							(long long)off
							)); 
	io_prep_preadv(&nciocb->iocb, inode->fd, nciocb->iocb_xtra.iov, block->mappedcnt, off);
	nciocb->iocb.aio_reqprio = 0;

}
static void
dio_io_prep_pwrite(nc_iocb_t *nciocb, fc_inode_t *inode, fc_blk_t *block, nc_off_t off, void *data)
{
	int 		i;
	DEBUG_ASSERT_FILE(inode, (off >= 0));
	DEBUG_ASSERT_FILE(inode, DM_FD_VALID(inode->fd));
	DEBUG_ASSERT_FILE(inode, BLOCK_VALID(inode, block, block->blkno));
	DEBUG_ASSERT(block->mappedcnt > 0);
	DEBUG_ASSERT(off > 0); /* because doffset > 0 */
	nciocb->iocb_xtra.cb			= data;
	nciocb->iocb_xtra.file			= inode;


	for (i = 0; (i < block->mappedcnt) && block->pages[i] ;i++) {
		nciocb->iocb_xtra.iov[i].iov_base = BLOCK_PAGE_MEMORY(block->pages[i]);
		nciocb->iocb_xtra.iov[i].iov_len  = NC_PAGE_SIZE;
	}
	io_prep_pwritev(&nciocb->iocb, inode->fd, nciocb->iocb_xtra.iov, block->mappedcnt, off);
	nciocb->iocb.aio_reqprio = 0;
	TRACE((T_INODE, "INODE[%d] - blk#%u, mapped[%d], aio_prep done(disk offset=%lld)\n",
					inode->uid,
					block->blkno,
					block->mappedcnt,
					off
					));




}


int
ds__libaio_queue_length()
{
	return s__wait_ios;
}




void
dio_shutdown()
{
	int	r;

	tp_stop(s__kaio_tp);
	r = io_queue_release(s__libaio_queue);
	TRACE((T_INFO, "shutdown requested(%d)\n", r));
	XFREE(s__iocb_pool_array);
	
}

int
dio_init(int queuelen)
{
	/* set DIO read callback */
	int						e;

	

	__dio_prepare_iocb_pool();

	if ((e = io_queue_init(DIO_MAX_AIO, &s__libaio_queue)) != 0) {
		TRACE((T_ERROR, "s__libaio_queue_init (%ld) - returned %d\n", DIO_MAX_AIO, e));
		exit(1);
	}
	TRACE((T_INFO, "MAX AIO configued to %ld\n", DIO_MAX_AIO));



	s__kaio_tp = tp_init_nq("KAIO", 2, 4, dio_handle_completed, NULL, NULL, NULL, NULL, U_FALSE);
	tp_start(s__kaio_tp);
	return 0;
}

#if NOTUSED
static void
dio_submit_done_event(nc_iocb_t *iocb)
{
	asio_post_apc(iocb, __FILE__, __LINE__);

}
#endif
void
dio_get_params(void *viocb, nc_asio_vector_t **v, int *bidx, long *transfered, long *error)
{
	nc_iocb_t * iocb = (nc_iocb_t *)viocb;

	*v 			= iocb->iocb_xtra.vector;
	*bidx		= iocb->iocb_xtra.bidx;
	*transfered	= iocb->xfered;
	*error		= iocb->error;
}

static void 
__dio_io_done(io_context_t ctx, struct iocb *aio_iocb, long transfered, long error)
{
	nc_iocb_t *					iocb	= (nc_iocb_t *)((char *)aio_iocb - offsetof(nc_iocb_t, iocb));

	nc_off_t					off;


	
	/* 2018-11-28 CHG huibong nc_asio_vector_t 객체의 iob 포인터 변수 접근시 SIGSEGV 발생 수정 (#32444)
	*                 - 본 함수는 dio_aio_event_handler thread 에서 호출
	*                 - 하지만 httpn_mpx_handler thread 에 의해 호출되는 asio_prepare_context() 함수에서
	*                   nc_asio_vector_t 객체의 iob 포인터 변수 memory realloc 처리 수행
	*                 - 이로 인해 nc_asio_vector_t 객체의 iob 포인터 변수에 대해 동시 접근이 발생하여 SIGSEGV 발생
	*                 - 본 함수에서 asioc_dump_prefix() 를 호출하지 않도록 수정 처리. 
	*/

	PROFILE_CHECK(iocb->t_e);

	mavg_update(g__dskio, (double)(PROFILE_GAP_USEC(iocb->t_s, iocb->t_e)/1000.0));

	dio_count_ioq( iocb->iocb_xtra.part, iocb->iocb_xtra.iot, U_TRUE, 1 );
#if 0
	if (iocb->iocb_xtra.iot == IOT_CACHE_WRITE) {
		/*
		 * direct call
		 */
		iocb->iocb_xtra.vector->iov_asio_callback(	iocb->iocb_xtra.vector,
													iocb->iocb_xtra.bidx,
													off,
													NULL,
													transfered,
													error);
		__dio_put_free(iocb);
	}
	else {
		iocb->apc_op	= APC_CHUNK;
		iocb->xfered 	= transfered;
		iocb->error 	= error;
		dio_submit_done_event(iocb);
	}
#else
	/*
	 * 있을 수 없는 상황이긴 한데 
	 * 1년 이상 운용상황 본뒤 지울것 (until 2020.12)
	 */
#pragma message(REMIND("libaio bug test?"))
	if (transfered == 0 && error == 0) {
		TRACE((T_ERROR, "********** LIBAIO return strange value(0,0), recovering\n"));
		error = ESTALE;
	}
	iocb->iocb_xtra.vector->iov_asio_callback(	iocb->iocb_xtra.vector,
												iocb->iocb_xtra.bidx,
												off,
												NULL,
												transfered,
												error);
	__dio_put_free(iocb);
#endif
	return;
}

/*
 * 주의 : 이 함수는 inode-lock확보된 후 호출됨(asio_context_handler내에서)
 */
int PUBLIC_IF
dio_block_io(nc_asio_vector_t *asiov, int ctxidx, nc_asio_type_t type, tp_data_t tcb)
{
	struct iocb *		iocb[10]; 
	char				abuf[128];
	char				bbuf[128];
	
	nc_part_element_t *	part 			= NULL;
	nc_iocb_t *			tiocb 			= NULL;
	nc_asio_context_t *	asioc 			= NULL; /* asioc는 asio_lock 상황에서만 valid함 !!*/
	int					ior 			= 0; 
	nc_asio_type_t 		iot 			= IOT_NULL;
	fc_blk_t *			asioc_block 	= NULL;			/* memory  where data copied to */
	fc_inode_t *		inode 			= NULL;
	nc_volume_context_t *volume 		= NULL;
	nc_path_lock_t      *pl 			= NULL;
	int					attempts		= 0;
	nc_size_t			off;
	int					needlock;



	tp_check_busy_nq(s__kaio_tp);

	/*
	 * LOCK 확보 필요 (pathlock == INODE LOCK)
	 *		- asioc 포인터는 LOCK내에서반 valid함(왜냐하면 realloc되면서 invalid화 가능
	 */
	inode 			= asiov->iov_inode;
	part 			= inode->part;
	volume			= inode->volume;
	pl				= inode->PL;


	needlock = (type == IOT_CACHE_WRITE);

	/*
	 *
	 ************************************ B E G I N N I N G O F   L O C K ********************************************
	 *
	 */

	if (needlock) {
#ifdef EXPERIMENTAL_USE_SLOCK
		pthread_spin_lock(&inode->slock);

#else
		INODE_LOCK(inode);
#endif /* experiment */
	}




	asioc 			= ASIO_CONTEXT(asiov, ctxidx);
	DEBUG_ASSERT_BLOCK(asioc->block, BLOCK_VALID(inode, asioc->block, asioc->blkno));
	asioc->completed= 0;
	asioc->offset 	= lpmap_physical_offset(asiov->iov_inode, asioc->block->blkno, type == IOT_CACHE_WRITE);
	if (ASIO_CONTEXT(asiov, ctxidx)->offset  <= 0){
		TRACE((T_WARN, "Volume[%s].Key[%s]-Inode[%d] found lpmapping inconsistancy(alloc=%d):blk#%d=>NULL, redrawing\n" 
					   "\t\t\t\tASIO vector:%s \n"
					   "\t\t\t\tBLOCK :%s \n",
						inode->volume->signature,
						inode->q_id,
						inode->uid,
						(type == IOT_CACHE_WRITE),
						ASIO_BLOCK(asiov, ctxidx)->blkno,
						asio_dump_lv1(abuf, sizeof(abuf), asiov),
						bcm_dump_lv1(bbuf, sizeof(bbuf), ASIO_BLOCK(asiov, ctxidx))

					));
		lpmap_free_nolock(inode, ASIO_BLOCK(asiov, ctxidx)->blkno);
		asiov->iov_asio_callback(	asiov,
									ctxidx,
									-1,
									NULL,
									0,
									EIO);
		if (needlock) {
#ifdef EXPERIMENTAL_USE_SLOCK
			pthread_spin_unlock(&inode->slock);
#else
			INODE_UNLOCK(inode);
#endif
		}
		return -1;
	}
	asioc->length 	= NC_BLOCK_SIZE;
	asioc_block 	= asioc->block;
	iot 			= (asioc->cascade?asioc->vector->iot[ASIO_CMD_CASCADE]:asioc->vector->iot[ASIO_CMD_FIRST]);

#ifdef NC_ENABLE_CRC
	if (type == IOT_CACHE_WRITE)
		bcm_verify_block_crc(inode, asioc_block);
#endif

#ifdef NC_MEASURE_BLOCKIO
	PROFILE_CHECK(asioc->block->t_done);
#endif
	off = asioc->offset;

	if (needlock) {
#ifdef EXPERIMENTAL_USE_SLOCK
		pthread_spin_unlock(&inode->slock);
#else
		INODE_UNLOCK(inode);
#endif
	}
	/*
	 *
	 ***************************************** E N D    O F   L O C K ********************************************
	 *
	 */



	dio_count_ioq(part, iot, U_FALSE, 1);


	/*
	 * iocb 할당을 inode lock 범위 밖에서 실행
	 */
	tiocb 						= __dio_get_free(tcb);
	tiocb->iocb_xtra.bidx   	= ctxidx;
	tiocb->iocb_xtra.part   	= part;
	tiocb->iocb_xtra.iot   		= iot;
	tiocb->iocb_xtra.vector 	= asiov;
	PROFILE_CHECK(tiocb->t_s);

	if (type == IOT_CACHE_READ) {
		dio_io_prep_pread(tiocb, inode, asioc_block, off, NULL);
	}
	else {
		dio_io_prep_pwrite(tiocb, inode, asioc_block, off, NULL);
	}
	
	io_set_callback(&tiocb->iocb, __dio_io_done); 


	iocb[0] = &tiocb->iocb;

#if 0
	/*
	 *
	 */
	if ((asiov->canceled) && (type == IOT_CACHE_READ) ) {
		__dio_io_done(s__libaio_queue, &tiocb->iocb, 0, ECANCELED);
	}
	else 
#endif
	{
		int 	nc = 0;

		attempts		= 0;
		do {
			nc = 0;
			ior = io_submit(s__libaio_queue, 1, iocb);

			if (ior == -EAGAIN) {

				bt_msleep(rand()%100+1); /* 실제는 10msec이상임*/
				nc = 1;
				attempts++;

			}
			else if (ior < 0) {
				TRACE(( T_WARN, "ASIO{%s}/IDX[%d] -  io_submit returned error %d\n",
								asio_dump_lv1(abuf, sizeof(abuf), asiov),
								ctxidx,
								ior ));
			}
		} while (nc);
#if 0
		if (attempts > 1) {
				int 		max_diocb, pending_diocb;
				dio_get_wait(&max_diocb, &pending_diocb);
				TRACE(( T_INFO, "INODE[%d] ASIO{%s}/IDX[%d] -  io_submit suceeded after %d attempts(%d/%d pending) \n",
								inode->uid, 
								asio_dump_lv1(abuf, sizeof(abuf), asiov),
								ctxidx,
								attempts,
								pending_diocb,
								max_diocb));
		}
#endif
		if (ior != 1) {
			/*
			 *
			 * ior != 1 인 경우에만 asiov에 대한 정보가 정상이고,
			 * 이외의 경우 asiov가 모두 완료되어 free되었을 수 있음
			 *
			 */
			__dio_io_done(s__libaio_queue, &tiocb->iocb, 0, -ior);
		}

		/*
		 * 주의!!!)
		 * dio_io_done() 호출 이후 asio에 대한 참조는 모조리 불가!
		 * asio의 모든 필드가 이미 free된 상태일 확율이 있음
		 */
	}
	TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%d].{%s} - IO scheduled with file offset=%lld (attempts=%d)\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					asioc_dump_prefix(abuf, sizeof(abuf), asiov, ctxidx),
					asioc->offset,
					attempts
					));


	return 0;
}
int PUBLIC_IF
dio_block_io_vector(nc_asio_vector_t *asiov, nc_asio_type_t type, tp_data_t tcb)
{
	char				abuf[128];
	char				bbuf[128];
	struct iocb *		*iocb = NULL; 
	
	nc_part_element_t *	part 			= NULL;
	nc_iocb_t *			tiocb 			= NULL;
	int					ior 			= 0; 
	fc_inode_t *		inode 			= NULL;
	nc_volume_context_t *volume 		= NULL;
	nc_path_lock_t      *pl 			= NULL;
	int					attempts		= 0;
	int					i;
	int					oidx;
	int					ncnt = 0;
	int					needlock = 0;




	tp_check_busy_nq(s__kaio_tp);

	iocb = XCALLOC(asiov->iov_cnt, sizeof(struct iocb *), AI_ETC);
	/*
	 * LOCK 확보 필요 (pathlock == INODE LOCK)
	 *		- asioc 포인터는 LOCK내에서반 valid함(왜냐하면 realloc되면서 invalid화 가능
	 */
	inode 			= asiov->iov_inode;
	part 			= inode->part;
	volume			= inode->volume;
	pl				= inode->PL;






	/*
	 *
	 ***************************************** E N D    O F   L O C K ********************************************
	 *
	 */




	needlock = (type == IOT_CACHE_WRITE || inode->ob_frio_inprogress);


	if (needlock) {
#ifdef EXPERIMENTAL_USE_SLOCK
		pthread_spin_lock(&inode->slock);
#else
		INODE_LOCK(inode);
#endif
	}
	for (ncnt = 0, i = 0; i < asiov->iov_cnt; i++) {

		/*
		 * iocb 할당을 inode lock 범위 밖에서 실행
		 */

		ASIO_CONTEXT(asiov, i)->offset 	= lpmap_physical_offset(inode, asiov->iov_sblkno + i, type == IOT_CACHE_WRITE);

		TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%d].{%s} - IO scheduled with file offset=%lld(RBOD=%d)\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					asioc_dump_prefix(abuf, sizeof(abuf), asiov, i),
					ASIO_CONTEXT(asiov,i)->offset,
					DM_CHECK_RBOD(inode, asiov->iov_sblkno+i)
					));



		if (ASIO_CONTEXT(asiov, i)->offset  <= 0){
			TRACE((T_WARN, "Volume[%s].Key[%s]-Inode[%d] found lpmapping inconsistancy(alloc=%d):blk#%d=>NULL, redrawing\n" 
					   		"\t\t\t\tASIO vector:%s \n"
					   		"\t\t\t\tBLOCK :%s \n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							type == IOT_CACHE_WRITE,
							ASIO_BLOCK(asiov, i)->blkno,
							asio_dump_lv1(abuf, sizeof(abuf), asiov),
							bcm_dump_lv1(bbuf, sizeof(bbuf), ASIO_BLOCK(asiov, i))
							));

			lpmap_free_nolock(inode, ASIO_BLOCK(asiov, i)->blkno);
			asiov->iov_asio_callback(	asiov,
										i,
										-1,
										NULL,
										0,
										EIO);
			continue;
		}
		dio_count_ioq(part, type, U_FALSE, 1);
		ASIO_CONTEXT(asiov, i)->length 	= NC_BLOCK_SIZE;
#ifdef NC_MEASURE_BLOCKIO
		PROFILE_CHECK(ASIO_CONTEXT(asiov, i)->block->t_done);
#endif

		tiocb 						= __dio_get_free(tcb);
		tiocb->iocb_xtra.bidx   	= i;
		tiocb->iocb_xtra.part   	= part;
		tiocb->iocb_xtra.iot   		= type;
		tiocb->iocb_xtra.vector 	= asiov;
		PROFILE_CHECK(tiocb->t_s);
		if (type == IOT_CACHE_READ)
			dio_io_prep_pread(tiocb, inode, ASIO_BLOCK(asiov, i), ASIO_CONTEXT(asiov, i)->offset, NULL);
		else
			dio_io_prep_pwrite(tiocb, inode, ASIO_BLOCK(asiov, i), ASIO_CONTEXT(asiov, i)->offset, NULL);


		io_set_callback(&tiocb->iocb, __dio_io_done); 

		iocb[ncnt++] = &tiocb->iocb;

	}
	if (needlock) {
#ifdef EXPERIMENTAL_USE_SLOCK
		pthread_spin_unlock(&inode->slock);
#else
		INODE_UNLOCK(inode);
#endif
	}

	if ((asiov->canceled) && (type == IOT_CACHE_READ) ) {
		for (i = 0; i < asiov->iov_cnt; i++) {
			__dio_io_done(s__libaio_queue, (struct iocb *)iocb[i], 0, ECANCELED);
		}
	}
	else {

		srand(time(NULL));
		attempts		= 0;

		oidx			= 0;
		do {
			ior = io_submit(s__libaio_queue, ncnt, &iocb[oidx]);

			if (ior == -EAGAIN) {
				bt_msleep( (rand()%10 + 1)*2 );
			}
			else if (ior > 0) {
				/*
				 * 주의!!!!!!!) undocumented io_submit 동작 방식
				 * vector submit의 경우 submit이 all or nothing이 아님
				 * 10개를 submit한 경우 submit성공 갯수는 10개보다 작은 수 일 수있음
				 */
				oidx 	+= ior;

				if (ior != ncnt) 
					TRACE(( T_INFO, "INODE[%d] ASIO{%s} -  io_submit returned error %d(%d/%d done, %d done int this call)\n",
								inode->uid,
								asio_dump_lv1(abuf, sizeof(abuf), asiov),
								ior,
								oidx,
								asiov->iov_cnt,
								ior
								));
				ncnt	-= ior;
			}
			attempts++;
		} while (ncnt > 0);

		if (attempts > 1) {
				int 		max_diocb, pending_diocb;
				dio_get_wait(&max_diocb, &pending_diocb);
				TRACE(( T_INFO, "INODE[%d] ASIO{%s} -  io_submit suceeded after %d attempts(%d/%d pending) \n",
								inode->uid, 
								asio_dump_lv1(abuf, sizeof(abuf), asiov),
								attempts,
								pending_diocb,
								max_diocb));
		}

		/*
		 * 주의!!!)
		 * dio_io_done() 호출 이후 asio에 대한 참조는 모조리 불가!
		 * asio의 모든 필드가 이미 free된 상태일 확율이 있음
		 */
	}

	XFREE(iocb);


	return 0;
}

nc_fio_handle_t PUBLIC_IF
dio_open_file_direct(char *filename, long long fsize, int bcreat, int brdonly)
{
	int	oflag ;
	int	fd;


	oflag = O_RDWR | O_DIRECT | O_LARGEFILE;
	if (bcreat) oflag |= O_CREAT;

	fd = open(filename, oflag, 0666);
	if (fd < 0) {
		char	ebuf[128];
		TRACE((T_INFO|T_INODE, "PATH['%s'](create=%d, rdonly=%d) - %s\n", 
						filename, 
						bcreat,
						brdonly,
						strerror_r(errno, ebuf, sizeof(ebuf))
						));
	}
	return fd;
}
int PUBLIC_IF
dio_open_file(char *filename, long long fsize, int bcreat, int brdonly)
{
	return dio_open_file_direct(filename, fsize, bcreat, brdonly);
}
int 
dio_file_size(int fd, nc_size_t *sz)
{
	int 			r = -1;
	struct stat 	fs;
	if (fstat(fd, &fs) == 0) {
		r = 0;
		*sz = fs.st_size;
	}
	return r;
}
PUBLIC_IF int 
dio_close_file(int fd)
{
	/*\
	 * 	ISSUES)
	 *		we don't know what would be happed if we close a file while writing to it
	\*/
	close(fd);
	return 0;
}



int PUBLIC_IF
dio_valid(int fd)
{
    return (fd >= 0);
}

#if 1
static int
dio_handle_completed(void *ud, void *tcb) 
{
	// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	//                - timeout 변수 nanoseconds 초기값 0 -> 100,000 으로 변경
	struct timespec to;
	struct timespec tv   = { 0, 100000000}; /* 100 msecs */
	struct timespec tv_n = { 0,  1000000}; /* 1 msecs */
	perf_val_t		s, e;
	long			d;

//#define	NR_MAX_EVENTS	max(DIO_MAX_AIO/4, 1024)
#define	NR_MAX_EVENTS	256
	struct io_event event[NR_MAX_EVENTS];
	struct iocb *iocb = NULL;
	int				handled = 0;
	int ret, acc_ioc;
	int i;

	do {
		to = tv;
		//pthread_mutex_lock(&s__iox_lock);
		PROFILE_CHECK(s);
		/*
		 * 2021.2.26 by weon@solbox.com
		 * To get more performance from NEW hardware, 
		 * the following code sniffets revised to get more completion events
		 * from kernel
		 */
#if 1
		acc_ioc = 0;
		do {
			ret = io_getevents(s__libaio_queue, 1, NR_MAX_EVENTS-acc_ioc, &event[acc_ioc], &to);
			if (ret > 0) acc_ioc += ret;
			to = tv_n; /* shorter msec */
		} while ((ret > 0) && (NR_MAX_EVENTS - acc_ioc) > 0);
#else
		/*
		 * OLD implementation, deprecated!
		 */
		acc_ioc = io_getevents(s__libaio_queue, 1, NR_MAX_EVENTS, event, &to);
#endif
		//pthread_mutex_unlock(&s__iox_lock);
		if (acc_ioc > 0)  {
			
			tp_set_busy(s__kaio_tp, U_TRUE);
			for (i = 0; i < acc_ioc; i++) {
				io_callback_t cb 	= (io_callback_t)event[i].data;
				iocb 				= event[i].obj;
	
				cb(s__libaio_queue, iocb, event[i].res, event[i].res2);
				handled++;
			}
			tp_set_busy(s__kaio_tp, U_FALSE);
			PROFILE_CHECK(e);
			d = PROFILE_GAP_MSEC(s, e);
			TRACE((T_INODE, "%d IO.CMPL events handled(%.2f sec)\n", acc_ioc, (float)(d/1000.0)));
		}
	} while (acc_ioc > 0);

	return handled;
}
#endif
