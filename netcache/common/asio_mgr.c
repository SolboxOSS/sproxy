/*\
 *	simple read-ahead handling module
 *
 * 	ASIO LOCK ORDER
 * 		(1) when asio API called, no inode lock maintained.
 * 		(2) locking order
 * 				asio lock
 * 				inode lock
 * 				block lock
 *
 * asio operation
 *		- iov_inprogress : OSD_ENTIRE방식의 요청시 생성될 떄 1로 설정
 * 		- driver.read
 * 			return value : 실제로 정상적으로 처리된(read) 청크 수
 *						   iov_cnt보다 작다면 나머지 이미 할당된 chunk에 대한 io cancel 처리
 *						  
 *							
 *			driver에서 chunk 채워질 때 마다 context_handler 호출
 * 					error : EOV flag | error code (여기서 EOV flag는 iov_inprogress=1일 때 세팅되어야함
\*/
#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#include <trace.h>
#include <hash.h>
#include <netcache.h>
#include <block.h>
#include <disk_io.h>
#include <threadpool.h>
#include <util.h>
#include "hash.h"
#include "tlc_queue.h"
#include "asio.h"
#include "bitmap.h"
#include "bt_timer.h"
#include <snprintf.h>


#ifdef __TRACE
#define 	ASSERT_ASIO_BLOCK(_f, _v, _bidx, x)	if (!(x)) { \
									char bdbuf[512]; \
									char adbuf[512]; \
									char *p = NULL; \
									fc_blk_t *b; \
									b = ASIO_BLOCK(_v, _bidx); \
			TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::\n" \
													"\t\t\t\tASIO={%s}\n" \
													"\t\t\t\t%dth-BLOCK={%s} \n" \
													"\t\t\t\tINODE INFO={INODE[%u]/REF[%d]/S[%d]/blkmap[%d].BUID=%ld}\n", \
													#x, __LINE__, __FILE__, \
													(char *)asio_dump_lv1(adbuf, sizeof(adbuf), _v), \
													(int)_bidx, \
													(char *)bcm_dump_lv1(bdbuf, sizeof(bdbuf), b), \
													(int)((_f)?(_f)->uid:-1), \
													(int)((_f)?(_f)->refcnt:-1), \
													(int)((_f)?(_f)->signature:-1), \
													(int)((ASIO_BLOCK(_v, _bidx))?(b)->blkno:-1), \
													(long)BLOCK_UID(b) \
													));  \
												*p = 0; \
												TRAP; \
								}

#else
#define 	ASSERT_ASIO_BLOCK(_f, _v, b, x)	
#endif



#define	MIN_ASIO_THREADS	4

// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
//                - 원부사장님 수정 소스 적용
//                - 256 -> 512 로 변경 
#define	MAX_ASIO_THREADS	512	

// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
//                - 원부사장님 수정 소스 적용
//                - 512 -> 1024 로 변경 
//                - 4*1024 로 변경 (2019/8/8)
#define ASIO_LIMITS			1024*4

#define	ASIO_HANG_TIMEOUT	180



extern __thread int __bc_locked;
extern mavg_t	*g__chi;
extern char _str_iot[][24];
extern fc_clfu_root_t	*	g__blk_cache;
extern int 			__s__max_ios; 
extern void  *		__timer_wheel_base;
extern int 			__terminating;
extern int 			g__octx_bind_count;
extern int 			g__busy_asio;
extern int 			__enable_compaction;
extern int			__enable_asio_timer;
extern nc_int16_t 	__memcache_mode;
int 				__min_asio_threads = MIN_ASIO_THREADS;
int					__max_asio_threads = MAX_ASIO_THREADS;
int 				__per_thread_var_enabled = 0;
int 				g__max_asio_vector_count = NC_MAX_VECTORIO;
tp_handle_t 		g__asio_thread_pool = NULL;

static nc_asio_vector_t		**__asio_ptr = NULL;
static int asio_run_io(void *d, void *tcb);

struct  {
	link_list_t		list;
	//pthread_mutex_t lock;
	pthread_spinlock_t lock;
} __asio_frees;



static void asio_context_handler(nc_asio_vector_t *asiov, int ctxidx, nc_off_t, void *tcb, nc_off_t len, unsigned long error);

#ifndef NC_RELEASE_BUILD
static void asio_verify_iob(nc_asio_vector_t *biov);
#endif
static int asio_remained_act(nc_asio_vector_t *biov);
static int asio_fini_vector(fc_inode_t *inode, nc_asio_vector_t *v, void *tcb/*not used */);
static int asio_set_block_nolock(nc_asio_vector_t *biov, int bidx, fc_blk_t *blk);
static void asio_replace_block_nolock(nc_asio_vector_t *biov, int bidx, fc_blk_t *blk);
static int asio_bio_fill(nc_asio_vector_t *biov, int bidx, nc_off_t offset, char *buffer, int len); 
static int asio_cascade_io_internal_nolock(nc_asio_vector_t *v, int ctxidx);
static int asio_read_vector_done_LOCK(nc_asio_vector_t *asiov, void *tcb/*not used*/);
static int asio_chunk_done_LOCK(void *ddc, void *tcb/*not used*/);
static void asio_push(fc_inode_t *inode, nc_asio_vector_t *v);
static int asio_run_vector(nc_asio_vector_t *asiov, void *tcb);

char _str_iot[10][24] = {
	"UNKN", 
	"CACHE_READ", 
	"CACHE_WRITE",
	"ORIGIN_READ", 
	"ORIGIN_WRITE",
	"ORIGIN_READ_ENTIRE",
	"NULL"
};
static char _str_asio_state[10][24] = {
	"0:ASIO_FREE", 
	"1:ASIO_INIT", 
	"2:ASIO_SCHEDULE", 
	"3:ASIO_DISPATCHED",
	"4:ASIO_RUNNING",
	"5:ASIOS_FRARRIVED", 
	"6:ASIOS_EOT",
	"7:ASIO_FINISHED"
};
extern char __str_blk_state[][24]; 
static int 	__asio_limit = ASIO_LIMITS;



void
asio_init(void)
{
	int		i;
	nc_asio_vector_t	*v = NULL;

	

	/*
	 * prepare asio resources
	 */
	INIT_LIST(&__asio_frees.list);
	//pthread_mutex_lock(&__asio_frees.lock);

	pthread_spin_init(&__asio_frees.lock, U_FALSE);
	pthread_spin_lock(&__asio_frees.lock);

	// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	//__asio_limit = min(512, __asio_limit);
	__asio_limit = min( ASIO_LIMITS, __asio_limit );

	__asio_ptr = (nc_asio_vector_t **)XCALLOC(__asio_limit+10, sizeof (nc_asio_vector_t *), AI_ASIO);
	for (i = 0; i < __asio_limit; i++) {
		__asio_ptr[i] = v = (nc_asio_vector_t *)XMALLOC(sizeof(nc_asio_vector_t), AI_ASIO);
		//memset(v, 0, sizeof(nc_asio_vector_t));
		INIT_NODE(&v->node);
		v->iov_id = i+1;
		v->iov_freed = 1;
		link_append(&__asio_frees.list, (void *)v, &v->node);
	}
	TRACE((T_INFO|T_ASIO, "%d asio resources prepared\n", __asio_limit));
	TRACE((T_ASIO, "starting %d read-ahead threads, would reach upto %d threads\n", __min_asio_threads, __max_asio_threads));
	g__asio_thread_pool = tp_init("ASIO", __min_asio_threads, __max_asio_threads, asio_run_io, NULL, NULL);
	ASSERT(g__asio_thread_pool != NULL);
	tp_start(g__asio_thread_pool);
	//pthread_mutex_unlock(&__asio_frees.lock);
	pthread_spin_unlock(&__asio_frees.lock);
	
}

static nc_uint32_t __asio_id_counter = 1;

static nc_asio_vector_t *
asio_pop(fc_inode_t *inode, int bwait)
{
	nc_asio_vector_t *v;

	ASSERT(inode != NULL);

L_retry_allocation:
	//pthread_mutex_lock(&__asio_frees.lock);
	pthread_spin_lock(&__asio_frees.lock);
	v = (nc_asio_vector_t *)link_get_head(&__asio_frees.list);
	while (bwait && !v) {
		/* no more asio resouce, release lock and wait */
		//pthread_mutex_unlock(&__asio_frees.lock);
		pthread_spin_unlock(&__asio_frees.lock);
		bt_msleep(10);

		//pthread_mutex_lock(&__asio_frees.lock);
		pthread_spin_lock(&__asio_frees.lock);
		v = (nc_asio_vector_t *)link_get_head(&__asio_frees.list);
	
		// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
		//                - 원부사장님 수정 소스 적용
		//                - 기존 소스 disable 처리후 else 구문 상의 TRACE 구문 추가.
		TRACE( (T_WARN, "ASIO resource run out, retired allocation =%p\n", v) );

	}
	//pthread_mutex_unlock(&__asio_frees.lock);
	pthread_spin_unlock(&__asio_frees.lock);
	if (!v ) {
		return NULL;
	}


	if (v->state != ASIOS_FREE) {
		/* restore it */
		TRACE((T_ASIO, "ASIO[%ld] - restoring onto freelist 'cause in use\n", (long)v->iov_id));
		pthread_spin_lock(&__asio_frees.lock);
		//pthread_mutex_lock(&__asio_frees.lock);
		link_append(&__asio_frees.list, (void *)v, &v->node);
		//pthread_mutex_unlock(&__asio_frees.lock);
		pthread_spin_unlock(&__asio_frees.lock);
		goto L_retry_allocation;
	}
	ASSERT(v->state == ASIOS_FREE); /* assure that the vector is not in use */

	XFREE(v->iob);
	v->iob = NULL;
	v->state 				 = ASIOS_INIT;
	v->canceled 			 = 0;
	v->iov_allocated_context = 0;
	v->iov_cnt 				 = 0;
	v->iov_id				 = _ATOMIC_ADD(__asio_id_counter, 1);
	if (v->iov_id == 0)
		v->iov_id = _ATOMIC_ADD(__asio_id_counter, 1);

	TRACE((T_ASIO, "ASIO[%d] - allocated for INODE[%u]/R[%d]/STL[%d]/FRIO[%d]\n", 
					v->iov_id, 
					inode->uid, 
					inode->refcnt, 
					inode->ob_staled, 
					inode->ob_frio_inprogress));
	/* we don't have to lock v */
	v->iov_inode = inode;
	v->iov_signature = dm_get_signature(inode);

	INODE_LOCK(inode);
	dm_io_add_vector_nolock(inode, v);
	INODE_UNLOCK(inode);

	TRACE((T_ASIO, "Volume[%s]/CKey[%s] - INODE[%u].ASIO[%d] added(count=%d)(%d,%d)\n", 
					inode->volume->signature,
					inode->q_id, 
					inode->uid, 
					v->iov_id, 
					link_count(&inode->pending_ioq, U_TRUE), 
					v->iov_cnt, 
					v->iov_allocated_context));

	return v;
}
static void
asio_push(fc_inode_t *inode, nc_asio_vector_t *v)
{
	int			i;
	fc_blk_t	*blk;

	/* 
	 * v->node will be used
	 */
	DEBUG_ASSERT_ASIO(v, inode, v->iov_freed == 0);
	DEBUG_ASSERT_ASIO(v, inode, v->state != ASIOS_FREE);
	
	v->iov_freed 	= 1;
	v->iov_priv  	= NULL;
	v->iov_inode	= NULL;
	v->state 		= ASIOS_FREE;
	v->iov_allocated_context = 0;
	for (i = 0; i < v->iov_cnt;i++) {
		blk = ASIO_BLOCK(v, i);
#ifndef NC_LAZY_BLOCK_CACHE
		if (blk && !BLOCK_IS_PG_MARKED(blk) ) {
			blk_make_unref_nolock(blk);
		}
#endif
	}
	XFREE(v->iob);


	pthread_spin_lock(&__asio_frees.lock);
	link_append(&__asio_frees.list, (void *)v, &v->node);
	pthread_spin_unlock(&__asio_frees.lock);

}
void
asio_shutdown(void)
{
	int 			i, freed=0;

	
	if (g__asio_thread_pool)
		tp_stop(g__asio_thread_pool);
	for (i = 0; i < __asio_limit; i++) {
		if (__asio_ptr[i]) freed++;
		if (__asio_ptr[i]->iob) {
			XFREE(__asio_ptr[i]->iob);
			__asio_ptr[i]->iob = NULL;
		}
		XFREE(__asio_ptr[i]);
	}
	XFREE(__asio_ptr);
	//pthread_mutex_destroy(&__asio_frees.lock);
	pthread_spin_destroy(&__asio_frees.lock);
	TRACE((T_INFO, "ASIO resources(%d) freed ok\n", freed));
	
}
int
asio_vector_count(nc_asio_vector_t *biov)
{
	return biov->iov_cnt;
}
void
asio_set_inode(nc_asio_vector_t *biov, fc_inode_t *inode)
{
	biov->iov_inode		= inode;
	biov->iov_signature	= dm_get_signature(inode);
}
#if OLD
void
asio_set_startoffset(nc_asio_vector_t *biov, nc_off_t off)
{
	biov->iov_startoffset= off;
}
#endif


/*
 * prepchunk = 1 인 경우에는 chunk 블럭도 할당
 */
static int
asio_prepare_context(nc_asio_vector_t *v, int cidx, nc_off_t offset,  int prepchunk, int check_inode_lock)
{
	int 		ncnt = 0;
	long 		blkno;
	int 		known = 0;
	int			prep = 0;
	int			needlock = 0;
	fc_blk_t 	*blk = NULL;

	if (!v->iov_inode->ob_frio_inprogress) {
		return 0;
	}

	/*
	 * IOT_OSD_READ_ENTIRE요청이고 
	 * 객체 크기가 알려지지 않은 경우
	 * 아래 스텝 진행
	 */
	needlock = v->iot[ASIO_CMD_FIRST] == IOT_OSD_READ_ENTIRE; /*  일부가 writing중이라도 iob 등 realloc될 수 있음 */


	if (needlock)
		INODE_LOCK(v->iov_inode);

	if (cidx >= v->iov_cnt) {
		if (v->iov_cnt >= v->iov_allocated_context) {
			if (asio_is_command_type(v, 0, IOT_OSD_WRITE)) {
				ncnt = v->iov_allocated_context + g__max_asio_vector_count;
			}
			else {
				if (INODE_SIZE_DETERMINED(v->iov_inode)) {
					/*
					 *  선언크기를 기준으로
					 */
					ncnt = v->iov_allocated_context + min(g__max_asio_vector_count, ((dm_inode_size(v->iov_inode, U_FALSE) + NC_BLOCK_SIZE-1)/NC_BLOCK_SIZE));
				}
				else {
					ncnt = v->iov_allocated_context + g__max_asio_vector_count;
				}
				ncnt = max(ncnt, 1); /* 0이 나오는 것을 회피 */
			}
			v->iob 		= (nc_asio_context_t *)XREALLOC(v->iob,  ncnt * sizeof(nc_asio_context_t ), AI_ASIO);
			TRACE((T_ASIO, "ASIO[%d]/INODE[%u] - cidx=%d, %d iobs prepared"
									"(old iov_cnt=%d, old iov_alloc=%d, known=%d, entire=%d, rangeable=%d)\n", 
									v->iov_id, 
									v->iov_inode->uid, 
									cidx, 
									ncnt, 
									v->iov_cnt, 
									v->iov_allocated_context,
									known, 
									(int)asio_is_command_type(v, 0, IOT_OSD_READ_ENTIRE),
									v->iov_inode->ob_rangeable
									));
			v->iov_allocated_context = ncnt;
		}
	}

	dm_resize_extent(v->iov_inode, offset);


	if (!prepchunk) goto L_done;


	blk = NULL;
	BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
		if ((blk = ASIO_BLOCK(v, cidx)) == NULL) {
			/* chunk 할당 */
	
#ifdef __PROFILE
			v->f_prep++;
#endif
			blkno 	= BLK_NO(offset, 0);
			/*
			 * 2020.1.8 수정(by weon)
			 * non-rangeable IO중에 블럭할당임
			 * size보다 더 큰 데이타 요청도 있을 수 있으므로
			 * blk_prepare_for_inode_nolock(호출시 needmap은 FALSE로 바꿈
			 */
			blk = blk_prepare_for_inode_nolock(v->iov_inode, blkno, U_FALSE);
			ASSERT(blk != NULL);
#ifndef NC_LAZY_BLOCK_CACHE
			blk->stat =  IS_CACHED;
			clfu_add(g__blk_cache, blk, &blk->node);
#endif
			dm_bind_block_nolock(v->iov_inode, blkno, blk);
			TRACE((T_ASIO, "INODE[%u]/ASIO[%ld] : %d-th iob = {BUID[%ld],blk # %ld} bound\n", 
									v->iov_inode->uid, 
									v->iov_id, 
									cidx, 
									BLOCK_UID(blk), 
									blk->blkno));
			asio_set_block_nolock(v, cidx, blk);
		}
		
	}

L_done:

	if (needlock)
		INODE_UNLOCK(v->iov_inode);
	return prep;
}
nc_asio_vector_t *
asio_new(fc_inode_t *inode, nc_asio_type_t first_type, nc_asio_type_t cas_type,  nc_blkno_t sblkno, int niobs, nc_user_callback_t callback, void *cb, int bwait, nc_kv_list_t *kv, int need_property)
{

	nc_asio_vector_t 	*biov = NULL;
	long				added_flag =0;
	char				pxbuf[128];


	

	biov = (nc_asio_vector_t *)asio_pop(inode, bwait);
	if (biov == NULL) {
		
		return NULL;
	}

	biov->type = APC_RUN_VECTOR;
	biov->iov_complex = 0;
	if ( (int)cas_type != (int)IOT_NULL) {
		biov->iov_complex = 1;
	}


	biov->iov_freed 		= 0;
	biov->iov_needproperty 		= need_property;
	biov->iot[ASIO_CMD_FIRST]	= first_type;					/* io target type */
	biov->iot[ASIO_CMD_CASCADE]	= cas_type;						/* io target type */
	biov->iov_async				= (callback != NULL?1:0);		/* async io  flag */
	biov->iov_cnt				= 0;			/* # of blocks necessary to do IO */
	biov->iov_sblkno			= sblkno;		/* start blkno */
	biov->iov_kv_out			= NULL;
	biov->iov_kv_out_nf			= 0;
	if (kv && ((first_type == IOT_OSD_READ) || (first_type == IOT_OSD_READ_ENTIRE))) {
		/*
		 * 
		 * asio가 active라는 말은 activation 시킨 client가 inode close를 안했다는 말이고
		 * 그전까지는 제공한 property pointer가 유지된다고 볼 수 있음
		 *
		 */
		biov->iov_kv_out_nf		= 1;
		biov->iov_kv_out		= kv_clone(kv, __FILE__, __LINE__);
#ifndef NC_RELEASE_BUILD
		rb_bind_inode(kv->oob_ring,	inode);						
#endif
		DEBUG_ASSERT_ASIO(biov, inode, (kv_valid(biov->iov_kv_out)));
		TRACE((T_ASIO, "ASIO[%d] - iov_kv_out property '%s'\n", biov->iov_id, (first_type == IOT_OSD_READ_ENTIRE)?"shared":"cloned"));
	}
	biov->iov_kv_in				= NULL;			/* key/value 리스트 */
	biov->iov_donecnt[ASIO_CMD_FIRST]			= 0;			/* # of blocks handled so far */
	biov->iov_donecnt[ASIO_CMD_CASCADE]			= 0;			/* # of blocks handled so far */
#if OLD
	biov->iov_stamp				= time(NULL);
#endif

	if (asio_is_command_type(biov, ASIO_CMD_FIRST, IOT_OSD_READ) ||
		asio_is_command_type(biov, ASIO_CMD_FIRST, IOT_OSD_READ_ENTIRE)) {
		biov->iov_inprogress =  U_TRUE;
	}

	biov->iov_asio_bio_callback				= asio_bio_fill;
	biov->iov_asio_callback					= asio_context_handler;
	//biov->iov_asio_callback					= asio_bio_fill_trigger;
	biov->iov_asio_prep_context_callback 	= asio_prepare_context;
	biov->iov_done_callback					= NULL;
	biov->iov_done_cb						= NULL;
	biov->iov_user_callback					= callback;
	biov->iov_cb							= cb;
	biov->locked							= 0;
	biov->iov_xfer_bytes					= 0;
	biov->canceled							= 0;
	if (niobs > 0) {
		biov->iob 	= (nc_asio_context_t *)XREALLOC(NULL,  niobs * sizeof(nc_asio_context_t ), AI_ASIO);
		//memset(biov->iob, 0, niobs * sizeof(nc_asio_context_t ));
		biov->iov_allocated_context = niobs;
	}



	/*
	 * context array만 생성 - 이때 inode에 대한 lock할당 여부를 확인할 필요 없음
	 */
	PROFILE_CHECK(biov->t_begin);
	PROFILE_CHECK(biov->t_end);
	PROFILE_CHECK(biov->t_schedule);
#ifdef __PROFILE
	biov->f_prep 	= 0;
#endif


	biov->state 			= ASIOS_INIT;
	biov->priv 				= NULL;



	TRACE((added_flag|T_ASIO|T_DEBUG, 
				"ASIO{%s} - complex=%d, property=%p allocated\n", 
				asiov_dump_prefix(pxbuf, sizeof(pxbuf), biov),
				(int)biov->iov_complex,
				(void *)biov->iov_kv_out));

#ifdef ASIO_DEBUG 
	ai = (asio_debug_t *)XMALLOC(sizeof(asio_debug_t), AI_ASIO);
	ai->id = biov->iov_id;
	pending_asio_count++;
	pthread_mutex_lock(&pending_asio_lock);
	link_append(&pending_asio, (void *)ai, &ai->node);
	pthread_mutex_unlock(&pending_asio_lock);
#endif
	
	return biov;
}

/*
 * signal pending ASIO jobs to be canceled
 * NOTICE) ensure that inode and asio vector should not be locked in the same time
 */
int
asio_signal_cancel(fc_inode_t *inode)
{
	int 				ncanceled = 0;
	char				pxbuf[128];
	nc_asio_vector_t*	v = NULL; 

	v = (nc_asio_vector_t *)link_get_head_noremove(&inode->pending_ioq);
	while (v && v->iov_inode) {
		if (v->canceled == 0) {
			TRACE((inode->traceflag|T_INODE|T_ASIO, "Volume[%s].CKey[%s] : INODE[%u].ASIO{%s} - cancel set\n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							asiov_dump_prefix(pxbuf, sizeof(pxbuf), v)
							));
			v->canceled = 1;
		}
		v = (nc_asio_vector_t *)link_get_next(&inode->pending_ioq, &v->node);
		ncanceled++;

	}
	/*
	 * inode lock 해제. 
	 * asio lock을 해야하므로, inode lock은 필요없음 */
	return ncanceled;
}

int
asio_append_block(nc_asio_vector_t *biov, fc_blk_t *blk)
{
	int 			bidx;
	int				err = -1;
	char			pxbuf[128];

	

	bidx = biov->iov_cnt;
	ASSERT(biov->iov_cnt <= biov->iov_allocated_context);
	if (biov->iov_cnt > biov->iov_allocated_context) {
		TRACE((T_ERROR, "ASIO[%d]/CNT[%d] - larger than allocatex context(%d)\n", biov->iov_id, biov->iov_cnt, biov->iov_allocated_context));
		TRAP;
	}
#ifdef NC_MEASURE_BLOCKIO
	PROFILE_CHECK(blk->t_begin);
#endif

#ifndef NC_RELEASE_BUILD
	ASSERT(ASIO_VALID_VECTOR(biov)); 
	ASSERT_FILE(biov->iov_inode, (bidx < biov->iov_allocated_context));
	ASSERT_FILE(biov->iov_inode, (blk->blkno <= biov->iov_inode->maxblkno));
#endif
	//asio_verify_iob(biov);
	asio_set_block_nolock(biov, bidx, blk);
	TRACE((T_ASIO|T_DEBUG, "ASIO{%s} : blk # %ld(BUID[%d]) bound\n", 
							asiov_dump_prefix(pxbuf, sizeof(pxbuf), biov),
							blk->blkno,
							BLOCK_UID(blk)
							));



#ifdef NC_MEASURE_BLOCKIO
	PROFILE_CHECK(blk->t_begin);
#endif
	err = 0;
	
	return err;
}

static int
asio_set_block_nolock(nc_asio_vector_t *biov, int bidx, fc_blk_t *blk)
{
	int				err = -1;
	char			pxbuf[128];

#ifdef NC_MEASURE_BLOCKIO
	PROFILE_CHECK( blk->t_begin );
#endif

	DEBUG_ASSERT_ASIO( biov, biov->iov_inode, bidx < biov->iov_allocated_context );

#ifdef NC_BLOCK_TRACE
	ASSERT( blk->bmagic == BLOCK_MAGIC );
#endif

	ASIO_CONTEXT( biov, bidx )->index		= bidx;				/* vector pointer */
	ASIO_CONTEXT( biov, bidx )->vector		= biov;				/* vector pointer */
	ASIO_CONTEXT( biov, bidx )->error		= 0; 				/* IO error code, in success, 0 */
	ASIO_CONTEXT( biov, bidx )->blkno		= blk->blkno;		/* IO error code, in success, 0 */
	ASIO_CONTEXT( biov, bidx )->block		= blk;				/* memory  where data copied to */
	ASIO_CONTEXT( biov, bidx )->completed	= 0;				/* 1 when finished */
	ASIO_CONTEXT( biov, bidx )->cascade		= 0;				/* 1 if the context was cascaded */
	ASIO_CONTEXT( biov, bidx )->offset		= 0;
	ASIO_CONTEXT( biov, bidx )->length		= NC_BLOCK_SIZE;

	// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
	//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
	//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
	//                  -- disk-read-ahead 크기 동적 조절시 차이 발생

	if (!BLOCK_IS_PG_MARKED(blk)) {
		if( biov->iot[ASIO_CMD_FIRST] == IOT_CACHE_READ )
			blk->bdskfault = 1;
		else {
			blk->bosdfault		= 1;
#ifndef NC_RELEASE_BUILD
			blk->nioprogress	= 1;
			TRACE((0, "INODE[%d] - blk#%d/BUID[%d] NIO set\n", biov->iov_inode->uid, blk->blkno, BLOCK_UID(blk)));
#endif
		}
	}

	TRACE(( T_INODE|T_ASIO, "ASIO[%s] : (blk#%u, BUID[%d]) prepared\n"
		, asiov_dump_prefix( pxbuf, sizeof( pxbuf ), biov )
		, blk->blkno
		, BLOCK_UID( blk ) ));

	biov->iov_cnt++;

#ifdef NC_MEASURE_BLOCKIO
	PROFILE_CHECK( blk->t_begin );
#endif

	PROFILE_CHECK( blk->t_submit );
	PROFILE_CHECK( blk->t_end );
	err = 0;

	return err;
}
static void
asio_replace_block_nolock(nc_asio_vector_t *biov, int bidx, fc_blk_t *blk)
{
#ifdef NC_BLOCK_TRACE
	ASSERT(blk->bmagic == BLOCK_MAGIC);
#endif

	// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
	//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
	//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
	//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
	if( biov->iot[ASIO_CMD_FIRST] == IOT_CACHE_READ )
		blk->bdskfault = 1;
	else {
		blk->bosdfault = 1;
#ifndef NC_RELEASE_BUILD
		blk->nioprogress	= 1;
		TRACE((0, "INODE[%d] - blk#%d/BUID[%d] NIO set\n", biov->iov_inode->uid, blk->blkno, BLOCK_UID(blk)));
#endif
	}

	ASIO_CONTEXT(biov,bidx)->blkno	= blk->blkno;		/* IO error code, in success, 0 */
	ASIO_CONTEXT(biov,bidx)->block	= blk;				/* memory  where data copied to */
	PROFILE_CHECK(blk->t_submit);
	PROFILE_CHECK(blk->t_end);
}

void
asio_hang_timer(void *d)
{
	nc_asio_vector_t 	*vector = (nc_asio_vector_t *)d;
	char				tbuf[256];
	char				ibuf[1024];
	char				sbuf[512];


	vector->iov_hung++;
	TRACE((T_WARN, "Volume[%s].CKey[%s]/ASIO[%d] : hang timer expired \n"
				   "ASIO-INFO : %s\n"
				   "INODE-INFO: %s\n"
				   "SESSION(if any): %s\n",
				   	vector->iov_inode->volume->signature,
				   	vector->iov_inode->q_id,
				   	vector->iov_id,
					asio_dump_lv1(tbuf, sizeof(tbuf), vector),
					dm_dump_lv1(ibuf, sizeof(ibuf), vector->iov_inode),
					(vector->iov_priv?cfs_dump_session(vector->iov_inode->volume, sbuf, (int)sizeof(sbuf), (void *)vector->iov_priv):"empty")
					));
	/*
	 * AIO를 커널에 전송후 event를 못받을 경우
	 * 복구시도
	 */
}

/*
 * when operation can not be performed
 * called in asio_schedule.
 */
int
asio_cancel_schedule(nc_asio_vector_t *v, int e)
{
	int		i;

	

	for (i = 0; i < v->iov_cnt && v->iob;i++) {
		asio_context_handler(v, i , -1, NULL, 0, e);
	}
	TRACE((		T_ASIO, 
				"ASIO[%lld] - %d blocks canceled\n",
				(long long)v->iov_id,
				(int)v->iov_cnt));
	
	return 0;
}
int
asio_schedule(nc_asio_vector_t *biov, nc_schedule_type_t stype)
{
	int 				ret = 0;
	char				abuf[1024];


	ASSERT(biov != NULL);
	ASSERT(biov->iov_inode != NULL);
	if (INODE_GET_REF(biov->iov_inode) <= 0) {
		TRACE((T_ERROR, "INODE[%u] - already closed\n", biov->iov_inode->uid));
		/* inode already closed */
		
		return -EINVAL;
	}

	if (biov->iov_cnt < 0) {
		TRACE((T_ERROR, "ASIO[%lld] - invalid IO count\n", (long long)biov->iov_id));
		
		return -EINVAL;
	}

	/*
	 * fill other scheduling-related fields
	 */
	biov->iov_error = 0;
	if (stype == ASIO_ASYNC) {
		biov->iov_async  = 1;
	}
	else {
		biov->iov_async  = 0;
	}

	TRACE((T_ASIO, "ASIO[%d] - setting hang timer\n", biov->iov_id));
	biov->state = ASIOS_SCHEDULED;
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT_ASIO(biov, biov->iov_inode, ASIO_VALID_VECTOR(biov));
	DEBUG_ASSERT_ASIO(biov, biov->iov_inode, (stype == ASIO_ASYNC || stype == ASIO_SYNC)); 
	DEBUG_ASSERT_ASIO(biov, biov->iov_inode, (biov->iov_cnt >= 0));
#endif
	ret = biov->iov_cnt; /* we have to keep it because biov may be processed before return */



	PROFILE_CHECK(biov->t_begin);
		//asio_run_io((void *)biov, NULL);
		TRACE((T_ASIO, "INODE[%u]/R[%d] - ASIO{%s} - executing...\n", 
								biov->iov_inode->uid, 
								biov->iov_inode->refcnt, 
								asio_dump_lv1(abuf, sizeof(abuf), biov)
							));
#if 1
		tp_submit(g__asio_thread_pool, (void *)biov);
#else
		asio_run_vector(biov, NULL);
#endif


	return ret;
}


static int
asio_cascade_io_internal_nolock(nc_asio_vector_t *asiov, int bidx)
{

	int 					r = 0;
	int 					i = 0;

	if (bidx >= 0) {
		ASIO_CONTEXT(asiov, bidx)->cascade = 1;
		r = dio_block_io(asiov, bidx, IOT_CACHE_WRITE, NULL);
	}
	else {
		/* batch */
		for (i = 0; i < asiov->iov_cnt; i++) {
			ASIO_CONTEXT(asiov, i)->cascade = 1;
		}
		r = dio_block_io_vector(asiov, IOT_CACHE_WRITE, NULL);
	}
		
	return r;
}
void
asio_destroy(fc_inode_t *inode, nc_asio_vector_t *biov)
{
	char			pxbuf[128];
	nc_uint32_t		tflg = (biov->iov_inode?biov->iov_inode->traceflag:0);


	

	PROFILE_CHECK(biov->t_end);
#ifdef __PROFILE
	TRACE((T_PERF, "ASIO[%d]: schedule[%.2f msec], finish[%.2f msc], prepare[%d calls]\n", 
			biov->iov_id, 
			(float)PROFILE_GAP_MSEC(biov->t_begin, biov->t_schedule),
			(float)PROFILE_GAP_MSEC(biov->t_schedule, biov->t_end),
			biov->f_prep
			));
#endif
	DEBUG_ASSERT_ASIO(biov, inode, biov->state != ASIOS_FREE);
	DEBUG_ASSERT_ASIO(biov, inode, biov->state != ASIOS_FINISHED);


	TRACE((tflg|T_ASIO, "ASIO[%s] - destroying (%s)\n", 
					asiov_dump_prefix(pxbuf, sizeof(pxbuf), biov),
					(biov->canceled?"canceled by signal":"")));


	biov->state = ASIOS_FINISHED;


	INODE_LOCK(inode);
	if (biov->iov_inode) {
		biov->iov_signature = 0;
		dm_io_remove_vector_nolock(inode, biov);
		biov->iov_inode		= NULL;
	}
	INODE_UNLOCK(inode);

	if (biov->iov_kv_out_nf) {
		kv_destroy(biov->iov_kv_out);
		biov->iov_kv_out = NULL;
		biov->iov_kv_out_nf = 0;
	}
	asio_push(inode, biov);
	
}


static __inline__ int
asio_remained_act(nc_asio_vector_t *biov)
{
	int remained = 0;

	
	remained = biov->iov_cnt - GET_ATOMIC_VAL(biov->iov_donecnt[ASIO_CMD_FIRST]); 

	if (biov->iov_complex)
		remained = remained + (biov->iov_cnt  - GET_ATOMIC_VAL(biov->iov_donecnt[ASIO_CMD_CASCADE])); 

	return remained; 
}

int
asio_need_EOV(nc_asio_vector_t *asiov)
{
	return (asiov->iov_inprogress);
}
int
asio_count_blocks(nc_asio_vector_t *asiov)
{
	return (asiov->iov_cnt);
}

#if 0
/*
 * origin으로부터 block fill이 완료되었을 때 호출
 */

static void
asio_bio_fill_trigger(nc_asio_vector_t *asiov, int ctxidx, nc_off_t foffset, void *tcb, nc_off_t len, unsigned long verror)
{
	nio_done_context_t	*ndc = XMALLOC(sizeof(nio_done_context_t), AI_ETC);

	ndc->type		= APC_READ_CHUNK; /* 원본에서 chunk fill됨*/
	ndc->asio 		= asiov;
	ndc->bidx 		= ctxidx;
	ndc->offset 	= foffset;
	ndc->transfered = len;
	ndc->error 		= verror;


	asio_post_apc(ndc, __FILE__, __LINE__);


}
#endif



static void
asio_context_handler(nc_asio_vector_t *asiov, int ctxidx, nc_off_t foffset, void *tcb, nc_off_t len, unsigned long verror)
{
	fc_inode_t 				*inode 		= asiov->iov_inode;
	char					pxbuf[256]	= "";
	char					tbuf[256]	= "";
	nc_asio_context_t 		*asioc 		= NULL;
	fc_blk_t				*blk 		= NULL;
	int						iot;
	int						nextstep;
	int						cascade;
	int						needlock;

#ifdef __PROFILE
	perf_val_t				s, d;
#endif
	int						cascade_stop = 0;


	PROFILE_CHECK(s);


#ifndef NC_RELEASE_BUILD
	ASSERT_FILE(inode, inode->ob_template == 0); /* lock없어도 됨 */
	ASSERT_FILE(inode, ((INODE_GET_REF(inode) > 0) && (dm_check_concurrent_limit_nolock(inode) > 0)));
#endif
	
	if (asiov->state ==  ASIOS_RUNNING)
		asiov->state =  ASIOS_FRARRIVED;


	needlock = asiov->iot[ASIO_CMD_FIRST] == IOT_OSD_READ_ENTIRE; /*  일부가 writing중이라도 iob 등 realloc될 수 있음 */


	/**********************************************************************************************************
	 *
	 *
	 * 아래 함수에서 lock은 asiov->iob와 inode->blockmap등이 계속 realloc될 때만
	 * 획득될 필요가 있으나 이외의 경우엔 필요가 없음
	 *
	 */
	
	if (needlock) {
		INODE_LOCK(inode);
		BC_LOCK(CLFU_LM_SHARED);
	}

	asioc 	  	= ASIO_CONTEXT(asiov, ctxidx);

	iot 		= (asioc->cascade? asiov->iot[ASIO_CMD_CASCADE]:asiov->iot[ASIO_CMD_FIRST]);
	blk			= asioc->block;
	cascade		= asioc->cascade;


	TRACE((inode->traceflag| T_INODE| T_ASIO, "[err=%d]INODE[%d].ASIO{%s} - %ld bytes\n", 
					verror,
					inode->uid,
					asioc_dump_prefix(pxbuf, sizeof(pxbuf), asiov, ctxidx),
					(long)len
					));


	if (needlock) {
		BC_UNLOCK;
		INODE_UNLOCK(inode);
	}

	/**********************************************************************************************************/


	/* 
	 * 주의)
	 * blk_handle_io_event_nolock()의 호출은 INODE에 대한 lock없이 호출
	 *
	 */
	nextstep = blk_handle_io_event_nolock(iot, asiov->iov_inode, blk, len, verror, asiov);

	TRACE((inode->traceflag| T_ASIO, "[err=%d]ASIO{%s} - block handler return %d:%s\n", 
					verror,
					asioc_dump_prefix(pxbuf, sizeof(pxbuf), asiov, ctxidx),
					nextstep,
					((nextstep==BSTM_CASCADE)?"CASCADE":"STOP")
					));

	/*
	 * 일단 block io handler가 한번 호출되었으므로 donecount 1 증가
	 */





#ifndef		NC_ENABLE_WRITE_VECTOR
	if ((nextstep == BSTM_CASCADE) && 
		(asiov->iot[ASIO_CMD_CASCADE] != IOT_NULL)  &&
		(!cascade)) {

		/*
		 * cascade IO 실행
		 */

		asio_cascade_io_internal_nolock(asiov, ctxidx); /* this call should be finished in async mode */
	}
	else {
		/*
		 * 더 이상 진행할게 없음
		 */
		if (asiov->iov_complex && cascade == 0 && nextstep == BSTM_STOP) {
			/*
			 * complex임에도, block io 모듈에서 STOP리턴을 했음.
			 * 비정상이므로, 여기에서 donecount 1증가 필요
			 */
			cascade_stop++;
			TRACE((T_ASIO, "INODE[%u]/R[%d] - ASIO{%s} - cascade donecnt increased by abort\n", 
							inode->uid, 
							inode->refcnt, 
							asio_dump_lv1(tbuf, sizeof(tbuf), asiov)));
		}
	}
#else
#pragma message("write-vector enabled")
	if (asiov->iov_complex && cascade == 0) {
		/*
		 * complex io request이고 두번째 명령이 아직 실행안됨
		 */
		if (nextstep == BSTM_STOP) {
			/*
			 * complex임에도, block io 모듈에서 STOP리턴을 했음.
			 * 비정상이므로, 여기에서 donecount 1증가 필요
			 */
			cascade_stop++;
			TRACE((T_ASIO, "INODE[%u]/R[%d] - ASIO{%s} - cascade donecnt increased by abort\n", 
							inode->uid, 
							inode->refcnt, 
							asio_dump_lv1(tbuf, sizeof(tbuf), asiov)));
		}
		else {
			
			if (iot == IOT_OSD_READ_ENTIRE) { 
				/*
				 * EOF를 알수 없는 경우 각 블럭마다 따로 실행
				 */
				asio_cascade_io_internal_nolock(asiov, ctxidx); /* this call should be finished in async mode */
			}
			else if ((iot == IOT_OSD_READ) && (asiov->iov_donecnt[ASIO_CMD_FIRST] == asiov->iov_cnt)) {
				/*
				 * 	batch invocation allowable
				 */
				asio_cascade_io_internal_nolock(asiov, -1); 
			}
		}
	}


#endif


	_ATOMIC_ADD(asiov->iov_donecnt[cascade], 1);
	_ATOMIC_ADD(asiov->iov_donecnt[ASIO_CMD_CASCADE], cascade_stop);

	INODE_LOCK(inode);
	if (asiov->iov_inprogress == 0) {
		asio_fini_vector(inode, asiov, tcb);
	}
	INODE_UNLOCK(inode);


	PROFILE_CHECK(d);
	mavg_update(g__chi, PROFILE_GAP_USEC(s, d)/1000.0);

	return;
	
}
void 
asio_set_done_callback(nc_asio_vector_t *biov, nc_done_callback_t c, void *cb)
{
	biov->iov_done_callback = c;
	biov->iov_done_cb = cb;
}

void
asio_cancel_io(nc_asio_vector_t *biov, int donecnt, void *tcb, int err)
{
	int  				i, iot = IOT_UNKN;
	int 				bs;
	int 				oerr = err;
	int 				canceled = 0;
	char 				dbuf[256];
	//fc_inode_t 			*inode = biov->iov_inode;	
	

	
	if (err == 0) err = ECANCELED;

	TRACE((T_ASIO, "ASIO[%d] - CNT[%d] : INODE[%u]/SIG[%ld]/FD[%u], cancelling...done=%d, (remained=%d), cause=%d(org_cause=%d)\n", 
							biov->iov_id,
							biov->iov_cnt,
							biov->iov_inode->uid,
							biov->iov_inode->signature,
							biov->iov_inode->fd,
							donecnt,
							asio_remained_act(biov),
							err,
							oerr
							));

	for  (i = max(0, donecnt); i < biov->iov_cnt; i++) {
		if (ASIO_CONTEXT(biov, i)->block == NULL) {
			TRACE((T_ERROR, "ASIO[%d] - IOB[%d]/CNT[%d] : %d-th IO completed, done1=%d,done2=%d(CHECK)\n",
							(int)biov->iov_id,
							(int)i,
							(int)biov->iov_cnt,
							(int)i,
							(int)biov->iov_donecnt[ASIO_CMD_FIRST],
							(int)biov->iov_donecnt[ASIO_CMD_CASCADE]
							));
			continue;
		}
		iot = (ASIO_CONTEXT(biov, i)->cascade? biov->iot[ASIO_CMD_CASCADE]:biov->iot[ASIO_CMD_FIRST]);
		bs  = ASIO_BLOCK(biov, i)->bstate;
			fc_blk_t *b;
			b = ASIO_BLOCK(biov, i);
#ifndef NC_RELEASE_BUILD
			TRACE((0, "INODE[%d].ASIO[%d] - blk#%d/BUID[%d] canceled(err=%d);%s\n", biov->iov_inode->uid, biov->iov_id, b->blkno, BLOCK_UID(b), err, asio_dump_lv1(dbuf, sizeof(dbuf), biov)));
			b->nioprogress = 0;
#endif
			asio_context_handler(biov, i, -1, tcb, 0, err);
			canceled++;
	}
	if ((canceled + donecnt) != biov->iov_cnt) {
			TRACE((T_ERROR, "%s ~ {INODE[%u]/SIG[%ld]/FD[%u]} : error(%d), canceled(%d), done(%d) found inconsistant\n",
							asio_dump_lv1(dbuf, sizeof(dbuf), biov),
							biov->iov_inode->uid,
							biov->iov_inode->signature,
							biov->iov_inode->fd,
							err,
							canceled,
							donecnt
							));
	}

	if (biov->iot[ASIO_CMD_FIRST] == IOT_OSD_READ_ENTIRE) {
		biov->iov_inode->staleonclose = 1;
	}
	
}

static int
asio_fini_vector(fc_inode_t *inode, nc_asio_vector_t *v, void *tcb/*not used */)
{
	int				n;
	int 			removed = 0;


	if ((v->state == ASIOS_FREE)  ||
		(v->iov_inode != inode))
		return 0; /*이미 free되거나 다른 작업에 할당됨 */
	

	n = asio_remained_act(v);

	if (n != 0) {
		return 0;
	}

	if (v->iov_async) { 
		/*
		 * CAS memory barrier로 동시 진입 불가
		 */
		asio_destroy(inode, v); /* called without locked */
		removed++;
	}
	
	return removed;;
}



/*
 * REMARK)
 *		이 함수의 실행에 LOCK은 필요 없음
 * 		왜냐하면 ASIO 생성 후 단독 실행이므로
 */
static int
asio_exec_cache_read(nc_asio_vector_t *asiov, int *error)
{
	fc_inode_t 	*inode = asiov->iov_inode;
	int 		r = 0;
	int			i;
	char		ibuf[1024];
	char		pxbuf[128];
	fc_blk_t	*blk = NULL;



#ifndef NC_RELEASE_BUILD
	ASSERT(inode != NULL);
	ASSERT_FILE(inode, INODE_GET_REF(inode) > 0);
	ASSERT_FILE(inode, link_count(&inode->pending_ioq, U_TRUE)>0);
	ASSERT_FILE(inode, DM_FD_VALID(inode->fd));
	asio_verify_iob(asiov);
#endif
	TRACE((T_INODE, "Volume[%s].CKey[%s]: ASIO{%s} running;%s\n",
					inode->volume->signature,
					inode->q_id,
					asiov_dump_prefix(pxbuf, sizeof(pxbuf), asiov),
					dm_dump_lv1(ibuf, sizeof(ibuf), inode)
					));


	*error = 0;
	asiov->state = ASIOS_RUNNING;


	BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
		for (i = 0; i < asiov->iov_cnt; i++) {
			blk = blk_prepare_for_inode_nolock(inode, asiov->iov_sblkno+i, U_TRUE);
#ifndef NC_LAZY_BLOCK_CACHE
			blk->stat =  IS_CACHED;
			clfu_add(g__blk_cache, blk, &blk->node);
#endif
			dm_bind_block_nolock(inode, asiov->iov_sblkno+i, blk);
			asio_replace_block_nolock(asiov, i, blk);

			// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924) 
			//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
			//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
			//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
			//ASIO_BLOCK(asiov, i)->bdskfault = 1;
		}
	}

	r = dio_block_io_vector(asiov, asiov->iot[ASIO_CMD_FIRST], NULL);
	return r;
}

static int
asio_exec_osd_read(nc_asio_vector_t *asiov, int *error)
{
	int 					r = 0;
	int						i;
	char					abuf[512];
	char					ibuf[512];
	fc_blk_t				*blk;


	fc_inode_t				*inode = asiov->iov_inode;
	nc_origin_session_t		origin = NULL;

	TRACE((T_INODE, "Volume[%s].CKey[%s]: preparing;%s\n",
					inode->volume->signature,
					inode->q_id,
					dm_dump_lv1(ibuf, sizeof(ibuf), inode)
					));

	*error = 0;


	BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
		for (i = 0; i < asiov->iov_cnt; i++) {
			blk = blk_prepare_for_inode_nolock(inode, asiov->iov_sblkno+i, U_TRUE);
#ifndef NC_LAZY_BLOCK_CACHE
			blk->stat =  IS_CACHED;
			clfu_add(g__blk_cache, blk, &blk->node);
#endif
			dm_bind_block_nolock(inode, asiov->iov_sblkno+i, blk);
			asio_replace_block_nolock(asiov, i, blk);

			// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
			//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
			//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
			//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
			//ASIO_BLOCK(asiov, i)->bosdfault = 1;
		}
	}
	TRACE((T_INODE, "Volume[%s].CKey[%s]:INODE[%u] - {%s} blocks prepared\n", 
					inode->volume->signature,
					inode->q_id,
					inode->uid, 
					asio_dump_lv1(abuf, sizeof(abuf), asiov)));

	{
		/*
		 * origin session을 이미 가지고 있는 경우
		 * 사용가능성 체크
		 */ 

		nc_int64_t	iosiz = -1LL;

		if (asiov->iot[ASIO_CMD_FIRST] == IOT_OSD_READ)
			iosiz = asiov->iov_cnt * NC_BLOCK_SIZE;

		origin = dm_steal_origin_session(inode, asiov->iov_sblkno*NC_BLOCK_SIZE, iosiz);
		if (origin) {
			cfs_set_read_range(inode->volume, origin, asiov->iov_sblkno * NC_BLOCK_SIZE, iosiz);
		}

	}

	asiov->iov_priv = origin;
	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u].%s invoking READ"
							"{%s}"
							"{%s}\n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							_str_iot[asiov->iot[ASIO_CMD_FIRST]],
							asio_dump_lv1(abuf, sizeof(abuf), asiov),
							dm_dump_lv1(ibuf, sizeof(ibuf), inode)
							));

	r = cfs_read_vector(inode->volume, asiov, origin);
	if (r < 0) {
		TRACE((T_WARN, "Volume[%s].CKey[%s] - INODE[%u].%s invoking READ failed(r=%d)"
							"{%s}"
							"{%s}\n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							_str_iot[asiov->iot[ASIO_CMD_FIRST]],
							r,
							asio_dump_lv1(abuf, sizeof(abuf), asiov),
							dm_dump_lv1(ibuf, sizeof(ibuf), inode)
							));
		asio_cancel_io(asiov, 0, NULL, -r); /* for each block, call the predefined callback with it*/
	}
	return r;
}

/*
 * 	ORIGIN에서 vector-IO가 완료되었을 때 호출
 */
static int
asio_read_vector_done_LOCK(nc_asio_vector_t *asiov, void *tcb/*not used*/)
{
	fc_inode_t *			inode = asiov->iov_inode;
	nc_volume_context_t *	volume = inode->volume;
	char					abuf[512];
	char					ibuf[512];
	void					*sess = NULL;
	int						fatal= 0;




	sess 	= asiov->iov_priv;
	volume 	= inode->volume;

	TRACE((T_ASIO, 	"Volume[%s]:CKey[%s].INODE[%u]/ASIO[%d] - cfs_read_vector() return %d;%s\n", 
					volume->signature, 
					inode->q_id,
					inode->uid,
					asiov->iov_id, 
					asiov->iov_xfer_blks,
					asio_dump_lv1(abuf, sizeof(abuf), asiov)
					));



	if (asiov->iov_xfer_blks < asiov->iov_cnt) {
		/* some blocks not finished in correct way */
		TRACE((T_INODE|T_ASIO, 	"Volume[%s]:CKey[%s].INODE[%u]/ASIO[%d]  got errno(done=%d/%d), cancelling remaining;{%s}\n", 
						inode->volume->signature, 
						inode->q_id,
						inode->uid,
						asiov->iov_id, 
						asiov->iov_xfer_blks,
						asiov->iov_cnt,
						asio_dump_lv1( abuf, sizeof(abuf), asiov)
						)); 
		asio_cancel_io(asiov, asiov->iov_xfer_blks, NULL, asiov->iov_error); /* for each block, call the predefined callback with it*/
	}

	if (sess) {
		dm_free_origin_session(volume, sess, U_FALSE, __FILE__, __LINE__);
	}

	/* context handler가 안불린 상태 */
	if (asiov->iov_xfer_blks == 0 || asio_need_EOV(asiov))  {
		TRACE((T_ASIO, 	"Volume[%s]:CKey[%s].INODE[%u]/ASIO[%d]  finished, running waitq;{%s}{%s}\n", 
						asiov->iov_inode->volume->signature, 
						asiov->iov_inode->q_id,
						asiov->iov_inode->uid,
						asiov->iov_id, 
						asio_dump_lv1( abuf, sizeof(abuf), asiov),
						dm_dump_lv1( ibuf, sizeof(ibuf), inode)
						)); 
		INODE_LOCK(inode);

		asiov->iov_inprogress = 0; 

		fatal = dm_finish_frio_nolock(inode, asiov->iov_error);

		asio_fini_vector(inode, asiov, NULL);
		INODE_UNLOCK(inode);
	}

	return 0;
}
/*
 * thread-pool에서 run-queue에 대기하던 asio를
 * 실행 시작
 */
static int
asio_run_vector(nc_asio_vector_t *asiov, void *tcb)
{
	fc_inode_t 			*inode = NULL;
	int					e = 0;
	int 				iot = 0;

	/*
	 * 아래 두 변수는 inode가 close되고 사라졌을 때를 대비해서
	 * inode의 path-lock, volume값의 저장용으로 사용
	 */
	nc_volume_context_t			*volume;

	inode	= asiov->iov_inode;

	volume 	= inode->volume;

#ifndef NC_RELEASE_BUILD
	dm_verify_allocation(inode);
#endif

	PROFILE_CHECK(asiov->t_schedule);
	iot = asiov->iot[ASIO_CMD_FIRST];

	DEBUG_ASSERT_ASIO(asiov, inode, asiov->state == ASIOS_SCHEDULED);

	asiov->state = ASIOS_DISPATCHED;

	switch (iot) {
		case IOT_OSD_READ:
		case IOT_OSD_READ_ENTIRE:
			asio_exec_osd_read(asiov, &e);
			break;
		case IOT_CACHE_READ:
			asio_exec_cache_read(asiov, &e);
			break;
		default:
			break;
	}
	return 0;
}
/*
 * called when it's time to run IO queued
 */

void
asio_post_apc(void *d, const char *f, int l)
{
	TRACE((0, "EVENT[%p] posted at %d%s\n", d, l, f));
	tp_submit(g__asio_thread_pool, (void *)d);
}

/*
 *
 **********************************************************************
 *
 * NETCACH CORE central event handler
 *
 **********************************************************************
 */
static int
asio_run_io(void *d, void *tcb)
{
	/* CHG 2018-06-07 huibong 불필요 변수 정리 (#32176) */

	apc_context_t		*apc = (apc_context_t *)d;
#ifndef NC_RELEASE_BUILD
	XVERIFY(apc);
#endif

	if  ( __terminating) {
		return 0;
	}
	_ATOMIC_ADD(g__busy_asio, 1);

	/*
	 *
	 * void *d의 구조체가 제각각 다르므로,
	 * 각 함수 시작점에서 INODE또는 path lock을 획득하도록함
	 *
	 */
	switch (apc->apc_op) {
		case APC_OPEN: /* driver에서 post한 객체의 property 요청에 대한 응답 이벤트 수신*/
			apcs_open_action_LOCK(d);
			break;

		case APC_READ_VECTOR_DONE:  /* driver에서 post한 vector-IO의 최종 완료 이벤트 수신 */
			asio_read_vector_done_LOCK(d, tcb);
			break;

		case APC_RUN_VECTOR: /* block module에서 vector IO 실행 요청 수신 */
			asio_run_vector(d, tcb);
			break;

		case APC_CHUNK:  /* disk_io 모듈에서 chunk단위의 disk-read/write완료 이벤트 수신 */
			asio_chunk_done_LOCK((void *)d, tcb);
			break;
		case APC_READ_CHUNK: {/* driver에서 chunk fill 완료 통보 */
			nio_done_context_t 	*nio = (nio_done_context_t *)d;
			XVERIFY(d);
			asio_context_handler(nio->asio, nio->bidx, nio->offset, NULL, (nc_off_t)nio->transfered, nio->error);
			XFREE(d);
			}
			break;
		case APC_CLOSE_INODE:
			apcs_close_action(d);
			break;
#ifdef APC_WRITE_CHUNK
		case APC_WRITE_CHUNK: {
			nc_asio_particle_t 	*p = (nc_asio_particle_t *)d;

			asio_cascade_io_internal_nolock(p->vector, p->cidx);

			XFREE(p);
				
			}
			break;
#endif
		case APC_READ_TIMEOUT:  {
				blk_apc_read_info_t		*pri  = (blk_apc_read_info_t *)d;
				fc_inode_t				*inode = pri->fhandle->inode;
				void fio_read_apc_blk_prepared(fc_blk_t *blk/*used*/, int xferbytes, int error, void *cbdata);
			
				_ATOMIC_SUB(pri->__inuse, 1);
			
				INODE_LOCK(inode);
				if (pri->rctx) {
					dm_cancel_waitq(pri->fhandle->inode, pri->rctx);
				}
				TRACE((0, "INODE[%d] - RI[%p] found read timeout\n", inode->uid, pri));
				fio_read_apc_blk_prepared(NULL/*used*/, 0, ETIMEDOUT, d);
				INODE_UNLOCK(inode);
			}
			break;
		default:
			TRACE((T_ERROR, "not implemented operation, %d\n", apc->apc_op));
			TRAP;
	}
	_ATOMIC_SUB(g__busy_asio, 1);

	return 0;	
}


static int 
asio_chunk_done_LOCK(void *iocb, void *tcb/*not used*/)
{
	nc_asio_vector_t 	*v;
	int					bidx;
	long				transfered;
	long				error;

	dio_get_params(iocb, &v, &bidx, &transfered, &error);

#ifdef NC_DEBUG_BLOCK
	DEBUG_ASSERT_ASIO(ddc->asio, ddc->asio->iov_inode, ASIO_CONTEXT(ddc->asio, ddc->bidx)->block->magic == BLOCK_MAGIC);
#endif
	asio_context_handler(v, bidx, 0, NULL, (nc_off_t)transfered, error);

	__dio_put_free(iocb);

	return 0;
}
char  *
asio_dump_lv1( char *buf, int buflen, nc_asio_vector_t	*vector)
{
	int 	n = 0;
	char 	*tp;
	int 	rem = buflen-1;
	tp = buf;
	n = snprintf(tp, rem, 	"ASIO[%d]/S[%s]/{%s+%s}/P[%d]/Cpx[%d]/cnt[%d]/PROP_NF[%d]/done{%d,%d}/blk#(%ld:%ld)/priv[%p]",
							(int)vector->iov_id,
							(char *)_str_asio_state[vector->state],
							(char *)&_str_iot[vector->iot[0]][0], 
							(char *)&_str_iot[vector->iot[1]][0], 
							(unsigned)vector->iov_inprogress,
							(unsigned)vector->iov_complex ,
							(int)vector->iov_cnt,
							(int)vector->iov_kv_out_nf,
							(int)_ATOMIC_ADD(vector->iov_donecnt[ASIO_CMD_FIRST], 0),
							(int)_ATOMIC_ADD(vector->iov_donecnt[ASIO_CMD_CASCADE], 0),
							(unsigned long)(vector->iov_sblkno),
							(unsigned long)(vector->iov_sblkno + vector->iov_cnt-1),
							vector->iov_priv
				);
	tp += n;	
	rem -= n;
	
#if 0
	n = snprintf(tp, rem, "["); tp+= n; rem -= n;
	for (int i = 0; i < vector->iov_cnt; i++) {
		if (ASIO_BLOCK(vector, i)->buid  != BLOCK_EMPTY) {
			n = snprintf(tp, rem, "%u ", ASIO_BLOCK(vector, i)->buid); 
			tp  += n;
			rem -= n;
		}
	}
	n = snprintf(tp, rem, "]"); tp+= n; rem -= n;
#endif
	if (rem > 0) {
		if (vector->iov_inode && (dm_get_signature(vector->iov_inode) == vector->iov_signature)) {
			n = snprintf(tp, rem, "{INODE[%u]/R[%d]/FD[%d]}", 
									vector->iov_inode->uid, 
									vector->iov_inode->refcnt, 
									vector->iov_inode->fd);
		}
		else {
			n = snprintf(tp, rem, "{INODE dangled}");
		}
		tp += n;
		rem -= n;
	}

	/* CHG 2018-05-17 huibong snprintf 반환값 특성상 잘못된 메모리 위치의 값 변경 가능 코드 제거 (#32120) */
	/* *tp = '\0'; */

	return buf;
}
#ifndef NC_RELEASE_BUILD
static void
asio_verify_iob(nc_asio_vector_t *biov)
{
	DEBUG_ASSERT_ASIO(biov, biov->iov_inode, nc_check_memory(biov->iob, biov->iov_allocated_context* sizeof(nc_asio_context_t)));
	DEBUG_ASSERT_ASIO(biov, biov->iov_inode, (biov->iob != NULL));
	DEBUG_ASSERT_ASIO(biov, biov->iov_inode, biov->iov_cnt >= 0);
	DEBUG_ASSERT_ASIO(biov, biov->iov_inode, biov->iov_cnt <= biov->iov_allocated_context);
}
#endif
int
asio_next_command(nc_asio_vector_t *v, int n)
{
	return v->iot[n];
}
static int
asio_bio_fill(nc_asio_vector_t *biov, int bidx, nc_off_t fileoffset, char *buffer, int len)
{
	nc_uint64_t		pageoff = 0;
	nc_uint64_t		pageno = 0;
	nc_uint64_t		tocopy = 0;
	nc_uint8_t	*	pagebuffer;
	nc_off_t 		intrablkoffset;
	int 			cpd = 0;
	fc_blk_t 	*	blk = NULL;
	fc_inode_t		*inode = NULL;
	char			pxbuf[128];
	int				locked = 0;
	int				updated = 0;



	
	inode 	= biov->iov_inode;
	ASSERT(fileoffset >= 0LL);



	if (inode->ob_frio_inprogress) {
		INODE_LOCK(inode); locked++;
		updated = dm_update_size_with_RI_nolock(inode, fileoffset, len);
		BC_LOCK(CLFU_LM_SHARED);
	}

	TRACE((T_ASIO, "ASIO{%s} - File.Offset=%lld, Length=%d\n", 
					asioc_dump_prefix(pxbuf, sizeof(pxbuf), biov, bidx),
					fileoffset,
					len
					));


	blk   			= ASIO_CONTEXT(biov, bidx)->block;

	if (locked) {
		BC_UNLOCK;
		INODE_UNLOCK(inode); /* ASIO_CONTEXT()에서 iob  realloc때문에 LOCK범위를 여기로 */
	}

	DEBUG_ASSERT_BLOCK(blk, blk->bstate == BS_FAULT);
	intrablkoffset = fileoffset - (nc_off_t)BLK_NO(fileoffset, 0) * (nc_off_t)NC_BLOCK_SIZE;

	while (len > 0) {
		pageoff = intrablkoffset % NC_PAGE_SIZE;
		pageno  = intrablkoffset / NC_PAGE_SIZE;
		tocopy  = min(len, (NC_PAGE_SIZE - pageoff));
		pagebuffer = bcm_check_page_avail(inode, blk, pageno, U_TRUE);
		if (asio_is_command_type(biov, ASIO_CMD_FIRST, IOT_OSD_WRITE)) {
			memcpy(buffer, pagebuffer+pageoff, tocopy);
		}
		else {
			memcpy(pagebuffer+pageoff, buffer, tocopy);
		}
		intrablkoffset 	+= tocopy;
		pageoff 		+= tocopy;
		buffer 			+= tocopy;
		len  			-= tocopy;
		cpd 			+= tocopy;
		fileoffset		+= tocopy;
	}

	return cpd;
}
char *
asioc_dump_prefix(char *buf, int buflen, nc_asio_vector_t *v, int idx)
{
	/* 주의사항 (#32444)
	*  - httpn_mpx_handler thread 에 의해 호출되는 asio_prepare_context() 함수에서
	*  - nc_asio_vector_t 객체의 iob 포인터 변수 memory realloc 처리 수행
	*  - 본 함수에서 nc_asio_vector_t 객체의 iob 포인터 변수 참조는 ASIO_BLOCK, ASIO_CONTEXT 에 의해 수행됨.
	*  - 따라서 httpn_mpx_handler thread 내에서 호출되는 함수가 아닌 경우
	*  - nc_asio_vector_t 객체의 iob 포인터 변수에 대해 동시 접근이 발생하여 SIGSEGV 발생 가능
	*  - 따라서 본 함수 호출시 주의 필요.
	*/

	snprintf(buf, buflen, "INODE[%u].ASIO[%u].[blk#%u:%d/%d][%s]",
							v->iov_inode->uid,
							v->iov_id,
							ASIO_BLOCK(v, idx)->blkno,
							(idx+1), /* not index # */
							v->iov_cnt,
							_str_iot[v->iot[ASIO_CONTEXT(v, idx)->cascade]]
							);

	return buf;
}
char *
asiov_dump_prefix(char *buf, int buflen, nc_asio_vector_t *v)
{
	if (v->iov_inode)
		snprintf(buf, buflen, "INODE[%u].ASIO[%u][#%d].[%s:%s]",
							v->iov_inode->uid,
							v->iov_id,
							v->iov_cnt,
							_str_iot[v->iot[0]],
							((v->iot[0] != IOT_NULL)?_str_iot[v->iot[1]]:"NULL")
							);
	else
		snprintf(buf, buflen, "INODE[NULL].ASIO[%u][#%d].[%s:%s]",
							v->iov_id,
							v->iov_cnt,
							_str_iot[v->iot[0]],
							((v->iot[0] != IOT_NULL)?_str_iot[v->iot[1]]:"NULL")
							);



	return buf;
}
