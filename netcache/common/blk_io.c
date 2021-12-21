/*
 *
 *	block read 흐름
 *
 *			(1) block이 메모리에 캐싱되어있는지 확인, 캐싱되었으면 해당 블럭복사
 *			(2) block이 메로리에 캐싱되지 않은 경우
 *					(2.1) 대상 block에 대한 IO가 이미 진행중인지 확인. 
 * 						  진행중이면 비동기 block copy  요청 생성 후 EWOULDBLOCK과 리턴
 *					(2.2) (2.1)이 아닌 경우 
 *						  대상 블럭을 loading(onto memory)하기 위한 disk/origin IO 요청 실행 후 
 * 						  비동기 block copy 요청 생성 및 inode 대기열에 등록 후 EWOULDBLOCK과 함께 리턴
 *
 */

#include <config.h>
#include <netcache_config.h>


#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#include <dirent.h>
#include <sys/param.h>
#include <errno.h>
#include <pthread.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>

#endif /* HAVE_SYS_TIME_H */



#include <trace.h>
#include <hash.h>
#include <netcache.h>
#include <block.h>
#include <asio.h>
#include <util.h>
#include <rdtsc.h>
#include <bt_timer.h>
#if 	GCC_VERSION > 40000
#include <mm_malloc.h>
#endif
#include <bitmap.h>
#include <snprintf.h>

#define 		FAST_CRC_DATA_SIZE 			128 /* bytes */
#define 		BLK_INTRA_OFFSET(o) 		((o) % NC_BLOCK_SIZE)
#define 		OFFSET_LESS_THAN_LEN(o, _i)	((nc_size_t)(o) < (_i)->size)

extern int						g__per_block_pages;
extern mavg_t					*g__chi;
extern mavg_t					*g__netblkr;
extern mavg_t					*g__prepblk;
extern mavg_t					*g__readi;
extern mavg_t					*g__preppage;
extern mavg_t					*g__rapc; /* readapc 시간*/
extern mavg_t					*g__wapc; /* blk_ri_wait 시간*/
extern mavg_t					*g__nri; /* origin read 시간*/
extern mavg_t					*g__dri; /* disk read io 시간*/


extern __thread int 			__bc_locked;
extern int						g__origin_ra_blk_cnt;
extern int						g__cache_ra_blk_cnt;
extern nc_ulock_t 				s__blk_cache_lock;
extern int						g__need_fastcrc;
extern int						__terminating;
extern fc_blk_t					*s__blk_map;
extern fc_clfu_root_t			*g__blk_cache;
extern nc_int16_t				__memcache_mode;

extern unsigned int 				g__blk_rd_network; /* # of origin chunk read count */
extern unsigned int 				g__blk_rd_disk; /* # of chunk read count */
extern unsigned int 				g__blk_wr_disk; /* #of chunk write count */
extern unsigned int 				g__blk_rd_fault; /* 디스크 캐시를 로딩했으나, 사용되지 않고 사라진 청크 갯수*/

extern void *		__timer_wheel_base;
extern int 			g__max_asio_vector_count;
extern int			g__strict_crc_check;
extern long			__s_ref_blocks;
extern long			__s_ref_blocks_osd;
extern long			__s_ref_blocks_mem;
extern long			__s_ref_blocks_dsk;
extern char 		_str_iot[10][24]; 
extern int			__enable_compaction;
extern __thread int __nc_errno;


typedef int (*blk_iot_handler_proc_t)(nc_asio_type_t, fc_inode_t *, fc_blk_t *, nc_off_t, int, nc_asio_vector_t *, int *);

#if NOT_USED
static int blk_handle_io_osd_write_nolock(nc_asio_type_t, fc_inode_t *, fc_blk_t *, int , nc_asio_vector_t *, int *);
#endif
static int blk_handle_io_osd_read_nolock   (nc_asio_type_t, fc_inode_t *, fc_blk_t *, nc_off_t, int, nc_asio_vector_t *, int *);
static int blk_handle_io_cache_write_nolock(nc_asio_type_t, fc_inode_t *, fc_blk_t *, nc_off_t, int, nc_asio_vector_t *, int *);
static int blk_handle_io_cache_read_nolock(	nc_asio_type_t, fc_inode_t *, fc_blk_t *, nc_off_t, int, nc_asio_vector_t *, int *);

static int blk_schedule_io_nolock(	fc_inode_t 		*inode, 
									nc_xtra_options_t *kv,
									nc_asio_type_t 	cmd, 
									nc_blkno_t 		blkno, 
									int 			nbcnt/*blkno에서 부터 연속적인 io대상 블럭 갯수*/
									);
static int blk_scan_max_fault_nolock(fc_inode_t *inode, nc_asio_type_t cmd, nc_blkno_t blkno);
static int blk_allowable_request_nolock(fc_inode_t *inode, nc_off_t foffset, size_t size);
static inline void blk_ri_push(blk_apc_read_info_t *pri);


int 							g__stat_frees  = 0;
#define	NC_BLK_MINDUMP 			64
#ifndef NC_RELEASE_BUILD
extern __thread int 	__bc_locked;
#endif


char __str_blk_state[][24] = {
	"BS_INIT",
	"BS_FAULT", 
	"BS_CACHED",
	"BS_CACHED_DIRTY",
	"BS_WRITEBACK_DIRTY"
};







/*
 * LOCKS
 * 		inode lock
 * 		block lock
 * wait until the block of blkno becomes valid
 * if valid, make it refered and release locks
 */
#if NOT_USED
static void 
blk_report_hang(void *v)
{
	fc_blk_t 	*blk = (fc_blk_t *)v;
	fc_inode_t 	*inode = blk->file;
	TRACE((T_WARN, "!!!!!!!!!!! INODE[%u]/R[%d]/SIG[%lld] - too long waiting on blk#%ld/BUID[%d]/P[%d]/S[%d]/State[%s]/SIG[%lld]\n", 
		inode->uid, inode->refcnt, inode->signature, blk->blkno, BLOCK_UID(blk), blk->binprogress, (int)blk->stat, __str_blk_state[blk->bstate], (long long)blk->signature));
}
#endif

/*
 * inode 에 대해서 blkno 째 블럭 버퍼를 할당한다.
 * light-weight 버전. 무조건적인 할당
 */
fc_blk_t *
blk_prepare_for_inode_nolock(fc_inode_t *inode, nc_blkno_t blkno, int needmap)
{
	fc_blk_t 	*blk_ref = NULL;
	char 		dbuf[512];


	/*
	 * 신규 블럭 하나를 할당
	 */

	blk_ref = bcm_alloc(inode, blkno, needmap);

	blk_ref->binprogress = 1;
	/*
	 * 블럭 정보 갱신
	 */
#ifdef NC_BLOCK_TRACE
	blk_ref->inode		= inode;
#endif
	blk_ref->signature  = BLOCK_SIGNATURE(inode->signature, blkno);
	blk_ref->blkno 		= blkno;
	blk_ref->bblocked	= inode->blk_locked;

	blk_update_state(inode, blk_ref, BS_FAULT, __func__, __LINE__);

	TRACE((0, "BUID[%ld]/R[%d]/P[%d]/S[%llX] ALLOCED to INODE[%d]\n", BLOCK_UID(blk_ref), blk_ref->brefcnt, blk_ref->binprogress, blk_ref->signature, inode->uid));

	DEBUG_ASSERT_BLOCK(blk_ref, (blk_ref->brefcnt == 0));
	DEBUG_ASSERT_BLOCK(blk_ref, (blk_ref->binprogress != 0));
	TRACE((T_ASIO|T_BLOCK, "INODE[%u] - blk#%ld alloced/bound to {%s}\n",
									inode->uid, 
									blkno, 
									bcm_dump_lv1(dbuf, sizeof(dbuf), blk_ref)
									));

#ifndef NC_LAZY_BLOCK_CACHE
	blk_make_ref_nolock(blk_ref);
#endif
	return blk_ref;
}


/*
 * 
 * ba[0]에 저장된 bci와 동일한 연속적인 블럭만큼을
 * IO 스케줄링
 *
 */
int
blk_fault_batch_nolock(	fc_inode_t 	*inode,
						nc_blkno_t	baseno,
						bci_stat_t	*ba,
						int			nba,
						nc_xtra_options_t *kv)
{

	//int 				nfaulted = nba;
	int					maxr, res;
	nc_asio_type_t		at = IOT_UNKN;
	//nc_off_t			roff = 0;
	//nc_size_t			rlen = 1;
	int					i;


	if (ba[0].bci == DM_BLOCK_ONDISK)  {
		at 		= IOT_CACHE_READ;
		maxr 	= 1;

		for (i = 1; i < nba; i++) if (ba[i].bci == DM_BLOCK_ONDISK) maxr++;
	}
	else {
		ASSERT(ba[0].bci == DM_BLOCK_FAULT);
		at 		= (inode->ob_rangeable?IOT_OSD_READ:IOT_OSD_READ_ENTIRE) ;
		maxr 	= 1;
		for (i = 1; i < nba; i++) if (ba[i].bci == DM_BLOCK_FAULT) maxr++;
	}

	//maxr		= min(maxr, nba);




	if (at == IOT_OSD_READ_ENTIRE) {
		res = (blk_schedule_io_nolock(inode, kv, at, 0, -1) >= 0);
	}
	else {
#if 0
		if ((baseno + maxr) > inode->maxblkno) {
			maxr = inode->maxblkno - baseno + 1;
		}
		else 
			nfaulted = maxr;
#endif
		res = (blk_schedule_io_nolock(inode, kv, at, baseno, maxr) >= 0);
		
	}

	return res;
	
}



/*
 * TRUE : io invoked
 */
int
blk_fault_nolock(	fc_inode_t 			*inode, 
					nc_off_t 			offset, 
					nc_xtra_options_t 	*kv, 
					fc_block_status_t 	bs /* fault type */
					)
{

	/* CHG 2018-06-14 huibong 미사용 변수 및 goto 구문 미사용에 따른 label 구문 정리 (#32194) */
	nc_asio_type_t 		cmd = IOT_UNKN;
	char 				abuf[512];
	nc_blkno_t 			blkno = BLK_NO(offset, 0);
	nc_off_t			roff = 0;
	nc_size_t			rlen = 1;
	int					nfaulted = 0;
	int					res = 0;

#ifndef NC_RELEASE_BUILD
	ASSERT_FILE(inode, INODE_GET_REF(inode) > 0);
#endif


	TRACE((T_INODE, "Volume[%s].CKey[%s] - blk#%d faulted;%s\n", 
					inode->volume->signature,
					inode->q_id,
					blkno,
					dm_dump_lv1(abuf, sizeof(abuf), inode)
					));


	switch (bs) {
		case DM_BLOCK_ONDISK:
			cmd 		= IOT_CACHE_READ;
			nfaulted 	= 1 + blk_scan_max_fault_nolock(inode, cmd, blkno+1) ;
			res 		= (blk_schedule_io_nolock(inode, kv, cmd, blkno, nfaulted) >= 0);

			break;
		case DM_BLOCK_FAULT:
			cmd 	= (inode->ob_rangeable?IOT_OSD_READ:IOT_OSD_READ_ENTIRE) ;

			nfaulted = 1 + blk_scan_max_fault_nolock(inode, cmd, blkno+1) ;

			roff = FILE_OFFSET(blkno);
			rlen = NC_BLOCK_SIZE;

			if (cmd == IOT_OSD_READ) {
				if ((blkno == 0) && 
					 cfs_valid_session(inode->volume, inode->origin, inode->q_path, &roff, &rlen)) {
					/*
		 			 *  inode->origin 세션은 원본 객체의 roff에서 rlen만큼의 IO가 진행중인 세션임
					 *  이것을 blk #로 변환
		 			 */

					nc_blkno_t tbno;

					tbno = BLK_NO(roff+rlen-1, 0) + 1;
					TRACE((T_WARN, "VOLUME[%s]/O[%s]/INODE[%u] - reusing origin context;faulted[%ld:%ld], tbno=%ld (adjusted to %ld)\n", 
								inode->volume->signature, 
								inode->q_path, 
								inode->uid, 
								(long)blkno, 
								(long)(blkno + nfaulted), 
								(long)tbno,
								(long)(tbno - blkno + 1)

							));

					nfaulted = tbno - blkno + 1;
				}
				res = (blk_schedule_io_nolock(inode, kv, cmd, blkno, nfaulted) >= 0);
			}
			else {
				res = (blk_schedule_io_nolock(inode, kv, cmd, 0, -1) >= 0);
			}
			break;
		case DM_BLOCK_ONMEMORY:
		case DM_BLOCK_IOPROGRESS:
			/* never fall here!!!! Cases were added only for clang warning*/
			break;
	}

	return res;
}


PUBLIC_IF nc_crc_t
blk_make_crc(fc_inode_t *inode, fc_blk_t *blk, ssize_t len, int fastcrc)
{

	int 	 		real_blk_len = 0;
	blk_stream_t 	bs;
	nc_off_t 		blkoff = 0, blkend = 0;
	long long 			remained = 0;
	nc_crc_t 		crc = 0;
	uint32_t crc32_8bytes(const void *data, size_t length);
	uint32_t crc32_8bytes_stream(blk_stream_t *blkstream, int length);

	// CHG 2019-04-15 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	//ASSERT( len >= 0 );

	if (len >= 0) {

		// CHG 2019-04-01 huibong cache 파일 read 시 CRC 오류 발생 문제 해결(#32576)
		//                - blk->blkno * NC_BLOCK_SIZE 의 계산시 int * int = int 의 문제로
		//                - 계산식이 정상 동작하지 않을 가능성이 존재하여 형변환 처리 코드 추가함.
		// CHG 2020-01-20 huibong 서로 다른 signed, unsigned type casting 발생 코드 수정 (#32781)
		//                - 비교 대상 변수 type 에 맞도록 unsigned -> signed 로 변경 처리
		remained = min( len, dm_inode_size(inode, U_TRUE) - blk->blkno * (nc_int64_t)NC_BLOCK_SIZE );

	}
	else {
		remained = real_blk_len = NC_BLOCK_SIZE;
		blkoff = (long long)blk->blkno * (long long)NC_BLOCK_SIZE;
		if (inode->writable)
			blkend = min(blkoff + NC_BLOCK_SIZE, inode->fr_availsize);
		else
			blkend = min(blkoff + NC_BLOCK_SIZE,  dm_inode_size(inode, U_TRUE));
		remained = real_blk_len = (int)(blkend - blkoff);
	}

	// CHG 2020-01-20 huibong ASSERT 발생 관련 원인 분석을 위한 로깅 추가 (#32781)
	//                - ASSERT 발생 했으나.. len 값 음수로 인한 버그인지 아니면 실제 상황인지 파악 목적
	//DEBUG_ASSERT_BLOCK(blk, (remained >= 0 && remained <= NC_BLOCK_SIZE));
	if( !( remained >= 0 && remained <= NC_BLOCK_SIZE ) )
	{
		// remained 값이 0 ~ NC_BLOCK_SIZE (conf 의 chunk_size * 1024 ) 사이 값이 아닌 경우....
		// - 로깅 후 assert 
		TRACE(( T_ERROR, "remained[%lld] len[%lld] dm_inode_size[%lld] blkno[%u] NC_BLOCK_SIZE[%d] blkoff[%lld] blkend[%lld] inode[%s][%s]\n"
			, remained, len, dm_inode_size( inode, U_TRUE ), blk->blkno, NC_BLOCK_SIZE, blkoff, blkend, inode->volume->signature, inode->q_path ));

		DEBUG_ASSERT_BLOCK( blk, 0 );
	}

	/*
	 * 일단 simple하게 구현.
	 * remained 크기만큼 memory할당 후 blk_read_bytes를 통해서 memcpy후
	 * 기존 crc32_8bytes로 전달
	 */
	if (fastcrc) {
		if (remained <= fastcrc) {
			bs_open(&bs, inode, blk, remained);
			bs_lseek(&bs, 0);
			crc = crc32_8bytes_stream(&bs, remained);
		}
		else {
			nc_crc_t	crc2;

			bs_open(&bs, inode, blk, remained);
			bs_lseek(&bs, 0);
			crc = crc32_8bytes_stream(&bs, fastcrc);
			bs_lseek(&bs, max(0, remained-fastcrc));
			crc2 = crc32_8bytes_stream(&bs, fastcrc);
			crc = crc ^ crc2;
		}
	}
	else {
		bs_open(&bs, inode, blk, remained);
		crc = crc32_8bytes_stream(&bs, remained);
	}
	return crc;
}

/*
 * ASIO manager will be called back to the following function
 * whenever a block-io is completed.
 * tasks conducted in this functions
 * 		- if appropriate, add block to CLFU
 * 		- if error found, reset blk state and put it back to pool
 * 		- take some actions in each (current block state, IO type) pair
 * 		- update bitmap
 */
/*
 * 이 함수는 inode->refcnt가 0일 떄도 호출될 수 있음
 */
struct iot_handler_info {
	blk_iot_handler_proc_t	handler;
	int						rioq;
} s__iot_handler[] = {
{/* IOT_UNKN                        0*/		NULL,								U_FALSE},
{/* IOT_CACHE_READ                  1*/		blk_handle_io_cache_read_nolock,	U_TRUE},
{/* IOT_CACHE_WRITE                 2*/		blk_handle_io_cache_write_nolock,	U_FALSE},
{/* IOT_OSD_READ                    3*/		blk_handle_io_osd_read_nolock,		U_TRUE},
{/* IOT_OSD_WRITE                   4*/		NULL,								U_FALSE},
{/* IOT_OSD_READ_ENTIRE             5*/		blk_handle_io_osd_read_nolock,		U_TRUE},
{/* IOT_NULL    					 */		NULL,								U_FALSE}
};

nc_asio_type_t
blk_handle_io_event_nolock(nc_asio_type_t iot, fc_inode_t *inode, fc_blk_t *blk, nc_off_t len, int error, nc_asio_vector_t *v)
{
	int 			ret = BSTM_STOP;
	int 			result;
	int 			nxt_blkstate = BS_INIT;
	int				run_ioq = U_FALSE;
	char			abuf[256];
	char			ibuf[2048];
#ifdef __PROFILE
	//perf_val_t		s, d;
#endif

	
	//PROFILE_CHECK(s);

	TRACE((T_INODE, "Volume[%s].Key[%s]::INODE[%u]/R[%d].ASIO[%d] - %s(blk#%ld/BUID[%ld]/mapped[%d]/len[%lld]) got (error=%d);%s\n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							inode->refcnt,
							v->iov_id,
							_str_iot[iot],
							(long)blk->blkno,
							(long)BLOCK_UID(blk),
							(int)blk->mappedcnt, 
							(long long)len,
							error,
							asiov_dump_prefix(abuf, sizeof(abuf), v)
							));

#ifndef NC_RELEASE_BUILD
	ASSERT(blk->nioprogress == 0);
#endif
	result 	= s__iot_handler[iot].handler(iot, inode, blk, len, error, v, &nxt_blkstate);
	run_ioq = s__iot_handler[iot].rioq;


	/*
	 * 최대한 CPU 효율성을 고려한 로직
	 */

	if (result >= 0) {

		if (run_ioq) {
#ifdef NC_LAZY_BLOCK_CACHE
			/*
			 *  현재 block은 clfu cache manager에 등록도 안되어있는 상태
			 *  등록하자마자 refcnt 증가시켜두어야 reclaim안됨
			 */
			BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
				//TRACE((T_WARN, "BUID[%ld](%s) - cached(len=%d);%s\n", BLOCK_UID(blk), _str_iot[iot], len, bcm_dump_lv1(abuf, sizeof(abuf), blk)));
				blk->stat =  IS_CACHED;
				clfu_add(g__blk_cache, blk, &blk->node);
			}
			blk_make_ref_nolock(blk);
#endif
			INODE_LOCK(inode);
		}

		/*
		 * 아래 두 라인의 위치는 아주 중요함
		 * clfu_add 보다위에 있으면 절대 안됨
		 */
		blk->binprogress = 0;
		blk_update_state(inode, blk, nxt_blkstate,__func__, __LINE__);
#ifndef NC_RELEASE_BUILD
		if (run_ioq && !BLOCK_VALID_TO_READ(inode, blk, blk->blkno)) {
			TRACE((T_ERROR, "BLOCK_VALID_TO_READ: INODE[%d](runioq=%d,result=%d,nxt_blkstate=%d, iot=%s\n", 
							inode->uid, 
							(int)run_ioq, 
							(int)result, 
							(int)nxt_blkstate, 
							_str_iot[iot]
				  ));
			TRACE((T_ERROR, "  * INODE:  %s\n",dm_dump_lv1(ibuf, sizeof(ibuf), inode)));
			TRACE((T_ERROR, "  * BLOCK:  %s\n", bcm_dump_lv1(ibuf, sizeof(ibuf), blk)));
			TRAP;
		}
#endif

		/*
		 * clfu OP가 run_ioq보더 선행되어야함
		 */
		if (run_ioq) {
			if (test_bit(blk->blkno, inode->whint_map)) {
				dm_run_waitq(inode, blk, error);
			}
			INODE_UNLOCK(inode);

			blk_make_unref_nolock(blk);
		}

		if (result > 0) {
			ret = BSTM_CASCADE; 
		}
	}
	else {
		if (error == 0) error = EIO;

#ifdef NC_LAZY_BLOCK_CACHE
		/*
		 * CLFU 등록 안되어있음
		 * inode->blockmap[]에만 binding됨
		 */
		BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
			blk->binprogress 	= 0; 
			bcm_put_to_pool_nolock(blk, U_FALSE);
		}
#else
		/*
		 * 이미 clfu에 등록되어있음
		 * signature만 날려서 바로 victim처리 되도록만 해둠
		 */
		blk_update_state(inode, blk, BS_INIT,__FILE__, __LINE__);
		blk->binprogress 	= 0; 
		blk->signature		= 0; 
		TRACE((0, "BUID[%ld]/R[%d]/P[%d]/S[%llX] RESET\n", BLOCK_UID(blk), blk->brefcnt, blk->binprogress, blk->signature));
#endif

		if (run_ioq)  {
			INODE_LOCK(inode);
			dm_run_waitq(inode, NULL, error);
			INODE_UNLOCK(inode);
		}

	}



	//PROFILE_CHECK(d);
	//mavg_update(g__chi, PROFILE_GAP_MSEC(s, d));

	return ret;
}

/*
 * inode lock 획득된 상태에서 호출된다
 */


/*
 * negative return : error, need to free block
 * 0: done
 * 1: cascade needed
 */
static int
blk_handle_io_cache_read_nolock(nc_asio_type_t 		iot, 
								fc_inode_t *		inode, 
								fc_blk_t *			blk, 
								nc_off_t 			len, 
								int 				error, 
								nc_asio_vector_t *	v, 
								int *				nxt_blkstate)
{
#ifdef NC_ENABLE_CRC
	nc_crc_t 		crc;
#endif
	int 			res_code = 0;

	DEBUG_ASSERT_BLOCK(blk, blk->bstate == BS_FAULT);
	DEBUG_ASSERT_BLOCK(blk, blk->binprogress != 0);
	PROFILE_CHECK(blk->t_end);
	mavg_update(g__dri, PROFILE_GAP_MSEC(blk->t_submit, blk->t_end));


	//INODE_LOCK(inode);
	/* right state */
	if (error || len == 0) {
#if 0
		if (error != ECANCELED) {
			/* 
			 * 읽기 요청이 취소된 것이 아니고, 다른 에러가 발생.
			 * 디스크 내의 해당 블럭 캐싱 정보 삭제
			 *
			 */

			struct stat 	s_;
			stat(inode->c_path, &s_);
			TRACE((T_WARN|T_BLOCK, "INODE[%u]/REF[%d] : {%s} : got error code[%d], block redrawed(file.size=%lld) \n",
									inode->uid, 
									inode->refcnt,
									bcm_dump_lv1(dbuf, sizeof(dbuf), blk),
									error,
									(long long)s_.st_size
									));
		}
		if (error == ESTALE) {
			INODE_LOCK(inode);
			nvm_make_inode_stale(inode->volume, inode);
			INODE_UNLOCK(inode);
		}
#else
		inode->staleonclose = 1;
#endif

		*nxt_blkstate = BS_INIT;
		//blk->error 		= error;
		res_code 		= -1;
	}
	else {
		/*
		 *  디스크 캐시에서 지정 블럭을 성공적으로 읽어왔음
		 */
#ifdef NC_VALIDATE_ORIGIN_DATA
		bcm_verify_content_integrity(inode, blk, len);
#endif

#ifdef NC_ENABLE_CRC

		// CHG 2019-04-01 huibong cache 파일 read 시 CRC 오류 발생 문제 해결(#32576)
		//                - blk->blkno * NC_BLOCK_SIZE 의 계산시 int * int = int 의 문제로
		//                - 대용량 파일 마지막 block 에 대해 계산식이 정상 동작하지 않음.
		//                - 이를 수정하기 위해 형변환 처리함.
		// CHG 2020-01-20 huibong 서로 다른 signed, unsigned type casting 발생 코드 수정 (#32781)
		//                - 비교 대상 변수 type 에 맞도록 unsigned -> signed 로 변경 처리
		len = min( dm_inode_size( inode , U_TRUE ) - blk->blkno * (nc_int64_t)NC_BLOCK_SIZE, len );

		crc = blk_make_crc(inode, blk, len, g__need_fastcrc);

		if (!dm_verify_block_crc(inode, blk->blkno, crc)) {
			char	dbuf[512];
			char	ibuf[1024];
			/*
			 * CRC 에러 발생
			 */
			TRACE((T_WARN, "Volume[%s].CKey[%s].INODE[%u]/REF[%d] : %lld bytes read, {%s} :  CRC error, block redrawed;%s\n", 
						inode->volume->signature, 
						inode->q_id,
						inode->uid, 
						inode->refcnt,
						len,
						bcm_dump_lv1(dbuf, sizeof(dbuf), blk),
						dm_dump_lv0(ibuf, sizeof(ibuf), inode)

						));
			INODE_LOCK(inode);
			dm_reset_inode_nolock(inode) ;
			INODE_UNLOCK(inode);

			blk->error 		= EAGAIN;
			res_code 		= -1;
			_ATOMIC_ADD(inode->crc_errcnt,1);
			if (_ATOMIC_VAL(inode->crc_errcnt) > 10 )  {
				TRACE((T_WARN, "IOT_CACHE_READ:: INODE[%u]/REF[%d]/CACHE[%s] - serious error, need to recover\n", inode->uid, inode->refcnt, (inode->c_path?inode->c_path:"NULL")));
			}
		}
#endif
		*nxt_blkstate = BS_CACHED;
		_ATOMIC_ADD(g__blk_rd_disk, 1);
	}
	//	blk_update_state(inode, blk, nxt_blkstate,__func__, __LINE__);
	//INODE_UNLOCK(inode);	
	return res_code;
}
/*
 * 
 * 주의) 아래 함수의 경우 disk에 저장 실패의 경우에도
 * -1을 리턴하지 않음. 단지 대상 블럭만 reclaim으로 날라갈뿐이고
 * 이후 다시 참조될 때 원본에게 다시 요청되게됨
 * TODO) non-rangeable객체의 경우 일부가 저장 실패시, 
 * 		 처리 방법은?
 *
 * 이 함수의 실행은 INODE LOCK을 사용하지 않음
 *
 */
static int
blk_handle_io_cache_write_nolock(nc_asio_type_t iot, fc_inode_t *inode, fc_blk_t *blk, nc_off_t len, int error, nc_asio_vector_t *v, int *nxt_blkstate)
{
	int 		res_code = 0;
	char 		dbuf[1024];

	


	/* IO is done in right state */
	if (error || len == 0) {
		//
		// 2019.10.18 commented by weon
		//
		// dm_reset_inode_nolock(inode) ;
		//
		*nxt_blkstate = BS_CACHED;

		lpmap_free_nolock(inode, blk->blkno); /* reset allocation */
#if 0
		if (dm_disk_cacheable(inode, inode->origincode) && DM_FD_VALID(inode->fd) && !inode->ob_rangeable) {
			/*
			 *	디스크 객체임에도 저장실패
			 * 	close시점에 폐기되어야함
			 */
			IC_LOCK(CLFU_LM_EXCLUSIVE);
			nvm_isolate_inode_nolock(inode->volume, inode);
			inode->ob_doc = 1;
			IC_UNLOCK;
		}
#endif
		TRACE((T_WARN, "INODE[%u]/REF[%d] : {%s} :  got error code[%d], keeping on memory\n",
					inode->uid, 
					inode->refcnt,
					bcm_dump_lv1(dbuf, sizeof(dbuf), blk),
					error));
	}
	else {
		/*
		 * 정상적으로 블럭을 disk에 저장했음
		 */

#ifdef NC_ENABLE_CRC
		bcm_verify_block_crc(inode, blk);
#endif
		dm_commit_block_caching(inode, blk);
		*nxt_blkstate = BS_CACHED;
		_ATOMIC_ADD(g__blk_wr_disk, 1);
	}

	
	return res_code;
}

static int
blk_handle_io_osd_read_nolock(nc_asio_type_t iot, fc_inode_t *inode, fc_blk_t *blk, nc_off_t len, int error, nc_asio_vector_t *v, int *nxt_blkstate)
{
	char 		dbuf[1024];
	char 		ibuf[1024];
	int 		res_code  = 0;
#ifdef NC_ENABLE_CRC
	nc_crc_t 	crc;
#endif


	PROFILE_CHECK(blk->t_end);

	mavg_update(g__netblkr, PROFILE_GAP_MSEC(blk->t_submit, blk->t_end));
#ifndef NC_RELEASE_BUILD
	ASSERT(blk->nioprogress == 0);
#endif


	/* right state */
	if (error) {
		*nxt_blkstate 	= BS_INIT;
		//blk->signature = 0; 
#if 0
		INODE_LOCK(inode);
		if (!inode->ob_rangeable && inode->ob_staled == 0) {
			nvm_make_inode_stale(inode->volume, inode);
			TRACE((T_INODE|T_BLOCK, "Object[%s]/INODE[%u]/REF[%d] : {%s} :  marked 'staled' 'cause of error [%s]\n",
						inode->q_id, 
						inode->uid, 
						inode->refcnt,
						bcm_dump_lv1(dbuf, sizeof(dbuf), blk),
						nc_strerror(error, ibuf, sizeof(ibuf))));
		}
		res_code 		= -1; /* release 표시 */
		INODE_UNLOCK(inode);
#else
		res_code			= -1; /* release 표시 */
		if (iot == IOT_OSD_READ_ENTIRE) {
			inode->staleonclose = 1;
			TRACE((T_INODE|T_BLOCK, "Object[%s]/INODE[%u]/REF[%d] : staleonclose set\n",
									inode->q_id, 
									inode->uid, 
									inode->refcnt
				  ));
		}
#endif
	}
	else {
		/*
		 * 원본 스토리지에서 블럭읽어오기 성공
		 */
		if (len == 0) {
			/* 
			 * 블럭읽기 시도는 성공했으나, 데이타가 없음
			 */
			TRACE((T_INFO, "%s : {%s} :  read empty(error=%d)\n",
							dm_dump_lv0(ibuf, sizeof(ibuf), inode),
							bcm_dump_lv1(dbuf, sizeof(dbuf), blk),
							error));
			*nxt_blkstate = BS_INIT;
			res_code 	= -1;
			goto L_end_of_osd_read;
		}
#ifdef USE_VM_STAT
		else if (len > 0 && inode->volume) {
			_ATOMIC_ADD(inode->volume->cnt_origin_bytes,len);
		}
#endif

#ifdef NC_VALIDATE_ORIGIN_DATA
		bcm_verify_content_integrity(inode, blk, len);
#endif

#ifdef NC_ENABLE_CRC
		if (dm_disk_cacheable(inode, inode->origincode)) {
			crc = blk_make_crc(inode, blk, len, g__need_fastcrc);
			TRACE((T_BLOCK, "INODE[%u]/R[%d]/SZ[%lld] - blk#%ld, length=%lld bytes, CRC[0x%08lX]\n",
							inode->uid, 
							inode->refcnt, 
							(long long)inode->size, 
							(long)blk->blkno, 
							(long long)len, 
							(unsigned long)(0xFFFFFFFFl & crc)));
			dm_update_block_crc_nolock(inode, blk->blkno, crc);
		}
#endif
		_ATOMIC_ADD(g__blk_rd_network, 1);

		if ((asio_next_command(v, ASIO_CMD_CASCADE) != IOT_NULL) && dm_disk_cacheable(inode, inode->origincode) && DM_FD_VALID(inode->fd)) {
			*nxt_blkstate 	= BS_CACHED_DIRTY;
			res_code 		= 1; /* cascade IO */
		}
		else {
			/*
			 * 메모리 캐시 모드 - disk 저장할 필요가 없으므로
			 * asio 처리 종료 
			 */
			*nxt_blkstate 	= BS_CACHED;
			res_code 		= 0; /* 블럭 처리 종료 */
		}
	}
L_end_of_osd_read:
	TRACE((0, "INODE[%u]/REF[%d]/blk#[%d] : {%s} :  code [%d] done(res=%d)\n",
						inode->uid, 
						inode->refcnt,
						(int)blk->blkno,
						bcm_dump_lv1(dbuf, sizeof(dbuf), blk),
						error,
						res_code
						));
	
	return res_code;
}

void
blk_update_state(fc_inode_t *inode, fc_blk_t *blk, int state, const char *_f, int _l)
{
#if 0
	if (state != BS_FAULT && state != BS_INIT)
		TRACE((T_WARN, "INODE[%d] - blk#%ld:%s => %s\n", 
								(inode?inode->uid:-1), 
								blk->blkno, 
								__str_blk_state[blk->bstate],
								__str_blk_state[state]));

#endif
	blk->bstate =  state;
}
/*
 * blk내의 데이타를 offset위치에서 len 바이트 만큼 읽어서
 * data가 지정하는 위치에 복사
 * @offset : blk내의 offset
 */
int
blk_read_bytes(fc_inode_t *inode, fc_blk_t *blk, nc_off_t foffset, char *data, nc_size_t maxlen)
{
	register nc_off_t 	pageoff; 
	nc_off_t 			boffset;
	register int 		tocopy;
	nc_blkno_t			blkno;
	int 				pgno;
	int 				cpd = 0;
	int					remained ;
	char				ibuf[2048];
	char				bbuf[256];



	boffset		= BLK_INTRA_OFFSET(foffset);

	remained  	= min((NC_BLOCK_SIZE - boffset), maxlen);
	remained  	= min(remained, (dm_inode_size(inode, U_TRUE) - foffset));

	pageoff 	= boffset % NC_PAGE_SIZE;
	pgno 		= boffset / NC_PAGE_SIZE;

#ifdef NC_VALIDATE_ORIGIN_DATA
	bcm_verify_content_integrity(inode, blk, min(NC_BLOCK_SIZE, maxlen));
#endif

	blkno 		= BLK_NO(foffset, 0);
	DEBUG_ASSERT_BLOCK3(inode, blk, blkno, BLOCK_VALID_TO_READ(inode, blk, blkno));
	DEBUG_ASSERT(blk->nioprogress == 0);

	while  (remained > 0) {

		tocopy  	= min(remained, (NC_PAGE_SIZE - pageoff));
		if (blk->pages[pgno] == NULL || blk->mappedcnt <= pgno) {
			int					i = 0;
			nc_asio_vector_t	*v = NULL;
			TRACE((T_ERROR, "Volume[%s].Key[%s] - INODE[%u] mapping ERROR(blkno# %ld)(copied=%d, remained=%d)\n"
							"\t\t\t\t INODE:%s\n"
							"\t\t\t\t BLOCK:%s\n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							(long)BLK_NO(foffset,0),
							cpd,
							remained,
							dm_dump_lv1(ibuf, sizeof(ibuf), inode),
							bcm_dump_lv1(bbuf, sizeof(bbuf), blk)
							));
			
			INODE_LOCK(inode);
			v = (nc_asio_vector_t *)link_get_head_noremove(&inode->pending_ioq);
			while (v) {
				TRACE((T_ERROR, "\t\t\t\t %d-th IOV : %s\n",
								i+1,
								(char *)asio_dump_lv1(bbuf, sizeof(bbuf), v)
								));

				v = (nc_asio_vector_t *)link_get_next(&inode->pending_ioq, &v->node);
				i++;
			}
			INODE_UNLOCK(inode);
			TRAP;
		}
#ifndef NC_RELEASE_BUILD
		ASSERT(tocopy >= 0);
		ASSERT((remained - tocopy) >= 0);
#endif
		memcpy(data, BLOCK_PAGE_MEMORY(blk->pages[pgno]) + pageoff, tocopy);
		data		+= tocopy;
		cpd 		+= tocopy;
		foffset		+= tocopy;
		remained 	-= tocopy;


		pgno		+= 1;
		pageoff		= 0;
	}
	return cpd;
}

static inline int
blk_valid_range(fc_inode_t *inode, nc_off_t t)
{
	return (inode->ob_sizeknown == 0 || inode->ob_frio_inprogress != 0 || BETWEEN(0, dm_inode_size(inode, U_TRUE)-1, t));
}




nc_size_t
blk_read_apc(	fc_inode_t 				*inode, 
				char 					*apbuf, 
				nc_off_t 				foffset, 
				size_t 					size, 
				nc_xtra_options_t 		*inprop, 
				apc_block_ready_callback_t 	callback, 
				blk_apc_read_info_t 	*pri
				)
{
	nc_size_t			remained = size;
	int 				rcp;
	ssize_t				copied = 0;
	char				ibuf[2048];
	nc_blkno_t			baseno;
	fc_blk_t			*blk;
	size_t				in_off 	= foffset;
	size_t				in_size = size;
	int 				ascheduled = 0;

	fc_block_status_t		bci = DM_BLOCK_FAULT;
	int						hitupdate = -1;
	int						upgraded = 0;
#define		BATCH_BLK_CHECK		8
	bci_stat_t 				*bci_array = NULL;
	int						nbci = -1;
	int						ba_idx;
	int						ntor, maxr;
	int						locked = 0;
	int						blocked = 0;
	int						ntor_read = 0;



	__nc_errno = 0;

	INODE_LOCK(inode); locked = 1;


	if (!blk_allowable_request_nolock(inode, foffset, size)) {
		TRACE((T_INODE|T_BLOCK, "Volume[%s]:INODE[%u](%lld, %ld) : invalid request; {%s}\n", 
						inode->volume->signature, 
						inode->uid, 
						(long long)foffset, 
						(long)size, 
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)));
		locked = 0;
		INODE_UNLOCK(inode);
		__nc_errno = 0;
		goto L_read_not_allowed;
	}

	if (INODE_SIZE_DETERMINED(inode)) {
		remained = min(dm_inode_size(inode, U_FALSE) - foffset, size);
		TRACE((T_INODE|T_BLOCK, "Volume[%s]:INODE[%u](%lld/blk#%ld, %ld) : total to read=%ld;{%s}\n", 
						inode->volume->signature, 
						inode->uid, 
						(long long)foffset, 
						(long)BLK_NO(foffset, 0),
						(long)size, 
						(long)remained,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)));
	}
	if (remained <= 0) {
		locked = 0;
		__nc_errno = 0;
		INODE_UNLOCK(inode);
		goto L_read_not_allowed;
	}

	/*
	 * batch processing 
	 */

	ntor = BLK_NO(foffset, remained-1)  - BLK_NO(foffset, 0) + 1;
	maxr = g__origin_ra_blk_cnt;
	if (maxr < ntor)
		maxr = ntor;


	bci_array = (bci_stat_t *)alloca(maxr * sizeof(bci_stat_t));
	DEBUG_ASSERT(bci_array != NULL);

	/*
	 * 최대 maxr개의 block 상태를 한번에 읽기
	 * bci_array[]에 등록된 block의 상태정보 저장
	 * refcnt ++ 되어 있음(swap방지)
	 */
	baseno 		= BLK_NO(foffset, 0);
#if 0
/*
 * only for DEEP debugging
 * (DO NOT DELETE LINES)
 *
 */
		int 	i, n, r;
		char 	xb[2048];
		char	*p = xb;
		char 	c;
		BC_LOCK(CLFU_LM_SHARED);
		blocked = 1;
		nbci 	= dm_block_status_batch_nolock(inode, baseno, bci_array, ntor, g__cache_ra_blk_cnt, g__origin_ra_blk_cnt, maxr, U_TRUE);

		n = snprintf(p, r = sizeof(xb), "INODE[%d].base.blk#%d=%d(ntor=%d){", 
				(int)inode->uid,
				(unsigned)baseno,
				(int)nbci,
				ntor);
		p += n; r -= n;
		for (i = 0; i < nbci; i++) {
			switch (bci_array[i].bci) {
				case DM_BLOCK_ONDISK: c='D';break;
				case DM_BLOCK_ONMEMORY: c='M';break;
				case DM_BLOCK_IOPROGRESS: c='P';break;
				case DM_BLOCK_FAULT: c='F';break;
			}
			n = snprintf(p, r, "%c ", c);
			p += n;
		}
		n = snprintf(p, r, "}\n"); p += n; *p = 0;
		TRACE((T_WARN, "%s", xb));
#else
		BC_LOCK(CLFU_LM_SHARED);
		blocked = 1;
		nbci 	= dm_block_status_batch_nolock(inode, baseno, bci_array, ntor, g__cache_ra_blk_cnt, g__origin_ra_blk_cnt, maxr, U_TRUE);

		DEBUG_ASSERT(nbci > 0);
		DEBUG_ASSERT(nbci <= maxr);

#endif
		blocked = 0;
		BC_UNLOCK;

#if 0
	if (nbci >= ntor && bci_array[ntor-1].bci == DM_BLOCK_ONMEMORY) {
		/*
		 * 필요한 모든 블럭이 메모리 존재
		 * blk->refcnt++인 상태이므로 더이상 어떤 lock도 필요없음
		 */

		if (locked) {
			locked = 0;
			INODE_UNLOCK(inode);
		}
	}

#endif

	ba_idx 		= 0;
	ascheduled 	= 0;





	while ( !ascheduled && 
			(remained > 0) && 
			(ba_idx < nbci)  &&
			blk_allowable_request_nolock(inode, foffset, remained)) { 
		blk = bci_array[ba_idx].block;
		bci = bci_array[ba_idx].bci;

		switch (bci) {
			case DM_BLOCK_ONMEMORY:
				/*
				 * memory-caching된 상태임, 블럭 데이타 읽기
				 */
#ifndef NC_RELEASE_BUILD
				{
					nc_blkno_t	blkno;
					blkno = BLK_NO(foffset, 0);
					DEBUG_ASSERT_BLOCK3(inode, blk, blkno, BLOCK_VALID_TO_READ(inode, blk, blkno));
				}
#endif
				rcp = blk_read_bytes(inode, blk, foffset, apbuf, remained);
				hitupdate = ba_idx;
				ntor_read++;
				TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%d].(%lld, %lld) %d bytes copied\n",
								inode->volume->signature,
								inode->q_id,
								inode->uid,
								foffset,
								remained,
								rcp));



				if (rcp <= 0) goto L_blk_read_error;

				pri->boffset	+= rcp;
				pri->xferbytes	+= rcp;
				pri->foffset	+= rcp;

				remained 		-= rcp;
				apbuf 			+= rcp;
				copied			+= rcp;
				foffset 		+= rcp;
				break;

			case DM_BLOCK_FAULT:
			case DM_BLOCK_ONDISK:
				/*
				 * disk/network IO 실행 
				 * bci_array[]에 포함된 정보만을 이용해서 fault 스케줄링
				 *
				 */
				if (bci == DM_BLOCK_ONDISK ||  nvm_is_online(inode->volume, NCOF_READABLE)) {
					ascheduled++;
					dm_schedule_apc_read(inode, apbuf, foffset, remained, inprop, callback, pri);


					/*
					 * blk fault내에서 실제 block 할당이 일어나지 않음
					 */
					blk_fault_batch_nolock(inode, baseno + ba_idx, &bci_array[ba_idx], nbci - ba_idx, inprop);
					__nc_errno = EWOULDBLOCK;
				}
				else {
					__nc_errno = EREMOTEIO;
				}


				break;
			case DM_BLOCK_IOPROGRESS:


				dm_schedule_apc_read(inode, apbuf, foffset, remained, inprop, callback, pri);

				ascheduled++;
				__nc_errno = EWOULDBLOCK;
				break;
		}
		ba_idx++;
	}
L_blk_read_error:


	ba_idx = 0;
	if (hitupdate >= 0) {
		/*
		 *  적어도 하나이상의 블럭을 카피함
		 */
		if (!blocked) {
			BC_LOCK(CLFU_LM_EXCLUSIVE);
			blocked	 = 1;
			upgraded = 1;
		}


		for (ba_idx = 0; ba_idx <= hitupdate; ba_idx++) {
			bcm_hit_block_nolock(bci_array[ba_idx].block, 1); /* lock이 필요*/
		}
	}

	if (blocked) {
		blocked = 0;
		BC_UNLOCK;
	}

	if (locked) {
		locked = 0;
		INODE_UNLOCK(inode);
	}



	/*
	 * 아래의 경우 lock안사용함
	 */

	for ( ba_idx = 0; (ba_idx < nbci) && bci_array[ba_idx].block  ; ba_idx++)  {

		blk_make_unref_nolock(bci_array[ba_idx].block); /* block참조 종료 */
	}



L_read_not_allowed:
	if (!ascheduled && inode->ob_staled) __nc_errno = ESTALE;

	TRACE((T_INODE, 	"Volume[%s]:INODE[%u]:(%lld/blk#%ld,%ld) => *%lldB read(rem=%lld), ncerrno=%d;{%s}\n", 
						inode->volume->signature,
						inode->uid, 
						(long long)in_off,
						(long)BLK_NO(in_off,0),
						(long)in_size,
						(long long)copied, 
						(long long)remained, 
						__nc_errno,
						(char *)dm_dump_lv2(ibuf, sizeof(ibuf), inode)
						));
//	if (ntor != ntor_read) {
//		TRACE((T_WARN, "INODE[%d]/size[%lld]/max[%d] - (O:%lld, size:%lld)[%d:%d] ntor[%d], nbci[%d], ntor_read[%d]\n", 
//					inode->uid, inode->size, inode->maxblkno, (long long)in_off,  (long long)in_size, 
//					BLK_NO(in_off,0),
//					BLK_NO(in_off,in_size-1),
//					
//					
//					ntor, nbci, ntor_read));
//	}
	return copied;
}



void
blk_destroy_read_apc(apc_read_context_t *rctx)
{
	XFREE(rctx);
}

static link_list_t	s__free_ri_list = LIST_INITIALIZER;
static nc_lock_t	s__free_ri_lock	= NC_LOCK_INITIALIZER;
static int			s__free_ri_init	= 0;

#ifndef NC_RELEASE_BUILD

#define	RESET_RI(ri_)	{\
	(ri_)->__inuse = 0; \
	(ri_)->__heap = 1; \
	_nl_init(&(ri_)->hlock, NULL); \
	_nl_cond_init(&(ri_)->hsignal); \
	bt_init_timer(&(ri_)->t_read_timer, "async read timer", U_FALSE); \
	(ri_)->magic = 0xA55A5AA5; \
	}
#else
#define	RESET_RI(ri_)	{\
	(ri_)->__inuse = 0; \
	(ri_)->__heap = 1; \
	_nl_init(&(ri_)->hlock, NULL); \
	_nl_cond_init(&(ri_)->hsignal); \
	bt_init_timer(&(ri_)->t_read_timer, "async read timer", U_FALSE); \
	}
#endif

static inline blk_apc_read_info_t *
blk_ri_pop(int force_alloc)
{
	blk_apc_read_info_t		*pri = NULL;
	int						created = 0;
#define     MAX_PRE_RI_ALLOCATION   512


	_nl_lock(&s__free_ri_lock, __FILE__, __LINE__);
	if (!s__free_ri_init) {
		int						i = 0;
		blk_apc_read_info_t		*ria = NULL;
		ria = XCALLOC(MAX_PRE_RI_ALLOCATION, sizeof(blk_apc_read_info_t), AI_ETC);

		for  (i = 0; i < MAX_PRE_RI_ALLOCATION; i++) {
			RESET_RI(&ria[i]);
			link_append(&s__free_ri_list, &ria[i], &ria[i].__node);
		}
		s__free_ri_init ++;
	}

	if (!force_alloc) {
		pri = (blk_apc_read_info_t *)link_get_head(&s__free_ri_list);
	}

	if (!pri) {
		pri = (blk_apc_read_info_t *)XMALLOC(sizeof(blk_apc_read_info_t), AI_ETC);
		RESET_RI(pri);
		created++;
		TRACE((T_INFO, "**** new RI allocated\n"));
	}
#ifndef NC_RELEASE_BUILD
	ASSERT(pri->magic == 0xA55A5AA5);
#endif
	//TRACE((T_WARN, "RI[%p] - poped(%s)\n", pri, (created?"CREATED":"reuse")));
	pri->apc_op = 0;
	_nl_unlock(&s__free_ri_lock);
	return pri;
}
static inline void
blk_ri_push(blk_apc_read_info_t *pri)
{

#ifndef NC_RELEASE_BUILD
	ASSERT(pri->magic == 0xA55A5AA5);
	ASSERT(GET_ATOMIC_VAL(pri->__inuse) == 0);
#endif


	_nl_lock(&s__free_ri_lock, __FILE__, __LINE__);
	link_append(&s__free_ri_list, pri, &pri->__node);
	_nl_unlock(&s__free_ri_lock);
}


static nc_uint32_t	__ri_counter = 0;

blk_apc_read_info_t *
blk_ri_init(blk_apc_read_info_t *ri, nc_file_handle_t *fh, nc_off_t off, char *buffer, nc_size_t len)
{
	int	created = 0;



	if (!ri) {
		created = 1;
		ri = (blk_apc_read_info_t *)blk_ri_pop(U_FALSE);
	}
	else {
		bt_init_timer(&ri->t_read_timer, "async read timer", U_FALSE);
		_nl_init(&ri->hlock, NULL);
		_nl_cond_init(&ri->hsignal);
	}

	ri->__inuse		= 1;
	ri->seq			= _ATOMIC_ADD(__ri_counter, 1);

	ri->block		= NULL;
	ri->fhandle		= fh;
	ri->length 		= len;
	ri->completed 	= 0;
	ri->buffer		= buffer;
	ri->boffset		= 0;
	ri->waiter		= 0;

	ri->error		= 0;
	ri->xferbytes	= 0;
	ri->foffset		= off;
	ri->callback	= NULL;
	ri->cbdata		= NULL;
	PROFILE_CHECK(ri->start);

	
	

	return ri;
}

void
blk_ri_reuse(blk_apc_read_info_t *ri)
{
	ri->completed 	= 0;
	ri->error		= 0;
}
int 
blk_ri_wait(blk_apc_read_info_t *ri, long *wait_inmsec)
{
	int 					r 	= 0;
	struct 		timespec	ts	= {0,0};
	int						loop = 0;
	long					elapsed_mtime = 0;
	struct 		timespec	t_bf, t_af;
	long					torg = *wait_inmsec;



	_nl_lock(&ri->hlock, __FILE__, __LINE__);
	ri->waiter++;



	__nc_errno = 0;
	/*
	 * wait_inmsec <= 0이면 대기안함!!!!!!!!
	 */
	while (ri->completed == 0 && *wait_inmsec > 0) {
		make_timeout(&ts, *wait_inmsec, CLOCK_MONOTONIC);

		clock_gettime(CLOCK_MONOTONIC, &t_bf);
		r = _nl_cond_timedwait(&ri->hsignal, &ri->hlock, &ts);
		clock_gettime(CLOCK_MONOTONIC, &t_af);

		elapsed_mtime = (t_af.tv_sec - t_bf.tv_sec)*1000 + (t_af.tv_nsec - t_bf.tv_nsec)/1000000;
		TRACE((T_INODE, "INODE[%d] - to_wait[%ld], real wait=%ld, r=%d\n", ri->fhandle->inode->uid, *wait_inmsec, elapsed_mtime, r));


		/*
		 * ETIMEDOUT이면 *wait_inmsec 만큼 대기 끝난거임
		 * 하지만 system에서 elapsed_mtime값은 그보다 작은 수가 나올 수 있음
		 */
		if (r == ETIMEDOUT)
			*wait_inmsec = 0;
		else
			*wait_inmsec = *wait_inmsec - elapsed_mtime;
		

		if (r == ETIMEDOUT &&  ri->completed == 0)  {
			if (*wait_inmsec <= 0) {
				/*
				 * 지정된 대기시간 timeout
				 */
				__nc_errno = r;


				break;
			}
		}

		loop++;
	} 

	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u] returns (TO=%ld): error=%d, complete=%d(0 means timeout?), errno=%d, loop=%d\n",
					ri->fhandle->inode->volume->signature, 
					ri->fhandle->inode->q_id, 
					ri->fhandle->inode->uid,
					torg,
					ri->error,
					ri->completed,
					__nc_errno,
					loop));
	ri->waiter--;
	_nl_unlock(&ri->hlock); 
	return (ri->completed?0:-1);
}
void
blk_ri_destroy(blk_apc_read_info_t *ri)
{
	int		sleep = 0;


	if (_ATOMIC_CAS(ri->__inuse, 1, 0)) {
		DEBUG_ASSERT(ri->waiter == 0);
		ri->apc_op	= APC_NULL;
		TRACE((0, "FILE[%p] - RI[%p] TIMER unset/removed(%d)\n", ri->fhandle, ri, ri->t_read_timer.running));
		ri->fhandle = NULL;
		/*
		 * timer 내에서도 del_timer() 호출은 문제없음
		 */
		bt_del_timer_v2(__timer_wheel_base, &ri->t_read_timer) ;
		blk_ri_push(ri);
	}
}
int
blk_ri_valid(blk_apc_read_info_t *ri)
{
	return (ri->__inuse == 1);

}




char*
blk_apc_dump_RI(char *buf, int len, blk_apc_read_info_t*ri)
{
	if (ri)
		snprintf(buf, len, "RI[%p]{foff=%lld, len=%d, xfered=%d, remained=%d, error=%d}",
				 			ri, 
							ri->foffset, 
							ri->length, 
							ri->xferbytes, 
							(ri->length - ri->xferbytes), 
							ri->error
							);
	else
		snprintf(buf, len, "RI[%p]", ri);
	return buf;
}
char *
blk_apc_dump_context(char *buf, int len, apc_read_context_t *rctx) 
{
	if (rctx)
		snprintf(buf, len, "R-CTX[%p]{foff=%lld,len=%lld, rem=%d, W.blk#%u, error=%d}",
				 			rctx, 
							rctx->foffset, 
							rctx->len, 
							rctx->remained, 
							rctx->wanted,
							rctx->error
						);
	else
		snprintf(buf, len, "R-CTX[%p]", rctx);
	return buf;
}
/*
 * command type이 OSD_READ와 CACHE_READ에 대해서만
 * 호출된다
 */
static int
blk_scan_max_fault_nolock(fc_inode_t *inode, nc_asio_type_t cmd, nc_blkno_t blkno)
{
	nc_blkno_t					maxr, bli;	
	fc_blk_t					*blk;
	int							faulted = 0;
	register fc_block_status_t	bci 	= DM_BLOCK_FAULT;

	if (cmd == IOT_OSD_READ)
		maxr =  min(inode->maxblkno+1, (blkno + g__origin_ra_blk_cnt));
	else
		maxr =  min(inode->maxblkno+1, (blkno + g__cache_ra_blk_cnt));



	TRACE((T_INODE, "Volume[%s].CKey[%s] - INODE[%u], scan range [%ld:%ld-1]\n", 
					inode->volume->signature, 
					inode->q_id, 
					inode->uid,
					blkno, 
					maxr));
	for (bli = blkno; bli < maxr; bli++) {

			/*
		 * blkno에 대응하는 block의 fault판단, fault인 경우 다음 blkno에 대해 동일 체크 수행
		 *
		 * cmd = OSD_READ인경우 bci = DM_BLOCK_FAULT		
		 * cmd = CACHE_READ인 경우 bci = DM_BLOCK_ONDISK
		 *
		 */

			bci = dm_block_status_nolock(inode, &blk, bli, U_FALSE, __FILE__, __LINE__) ;
			if ( 	((cmd == IOT_OSD_READ) && (bci != DM_BLOCK_FAULT)) ||
					((cmd == IOT_CACHE_READ) && (bci != DM_BLOCK_ONDISK))
				)
				break;
			faulted++;
	}
	TRACE((T_INODE, "INODE[%u]/CMD[%s] - scanning from blk#%d results in # of fault blocks=%d\n", 
					inode->uid, 
					_str_iot[cmd], 
					blkno, 
					faulted));
	return faulted;
}


static int
blk_schedule_io_nolock(	fc_inode_t 		*inode, 
						nc_xtra_options_t *kv, 
						nc_asio_type_t 	cmd, 
						nc_blkno_t 		baseno, 
						int 			bcnt/*blkno에서 부터 연속적인 io대상 블럭 갯수*/
						)
{
	fc_blk_t			**ba = NULL;
	//fc_blk_t			*bs = NULL;
	int					i;
	//nc_blkno_t			bli;
	//int					nb_prepared = 0;
	int					ior = 0;
	nc_asio_type_t		cmd2;
	nc_asio_vector_t	*vector = NULL;
	char				abuf[256];
	//char				ibuf[512];


#ifndef NC_RELEASE_BUILD

	ASSERT_FILE(inode, ((inode) != 0));
	ASSERT_FILE(inode, INODE_GET_REF(inode) > 0);
#endif
	ASSERT_FILE(inode, (dm_inode_check_owned(inode) != 0));


	if (bcnt <= 0) bcnt = 1; /* OSD_READ_ENTIRE의 경우 bcnt == -1 */

	ba = (fc_blk_t **)alloca(bcnt * sizeof(fc_blk_t *));


	switch (cmd) {
		case IOT_OSD_READ:
		case IOT_CACHE_READ:
				/*
				 *
				 * ba[].block에 [baseno:baseno+bcnt-1]범위로 block할당
				 *
				 */
				for (i = 0; i < bcnt; i++) {
					inode->blockmap[baseno+i] = ba[i] = &g__NC_BLOCK_PG_MARKER;
				}
				TRACE((0, "INODE[%u].%s - blk#%u:%u(%d) scheduling\n", inode->uid, _str_iot[cmd], baseno, baseno+i-1, bcnt));
				break;
		case IOT_OSD_READ_ENTIRE:
				inode->blockmap[0] = ba[0] = &g__NC_BLOCK_PG_MARKER;
				bcnt			= 1;
				dm_need_frio(inode); /* inode에 대한 full-range IO진행 세팅 */

				break;
		default:
				TRACE((T_ERROR, "Fatal: Unknown command:%d, exiting\n", cmd));
				TRAP;
				break;
	}

	cmd2 = IOT_NULL;
	if 	((cmd == IOT_OSD_READ || cmd == IOT_OSD_READ_ENTIRE) && 
		 DM_FD_VALID(inode->fd) && 
		 dm_disk_cacheable(inode, inode->origincode))  {

		cmd2 	= IOT_CACHE_WRITE;
	}


	/*
	 * 	inode에 block이 binding되어 있고, 
	 *  block을 어디에서부터 읽어올 수 있는지까지 파악이 된 상태.
	 */
	vector = asio_new(inode, cmd, cmd2, baseno, bcnt, NULL, NULL, U_TRUE, kv, (inode->property == NULL));

	for (i = 0; i < bcnt;i++) {
		asio_append_block(vector, ba[i]);
	}


	TRACE((T_INODE, "Volume[%s].CKey[%s]:INODE[%u] - {%s} scheduling\n", 
					inode->volume->signature,
					inode->q_id,
					inode->uid, 
					asio_dump_lv1(abuf, sizeof(abuf), vector)));
	ior = asio_schedule(vector, ASIO_ASYNC);
	TRACE((0, "INODE[%ld].ASIO invoked:%s\n",
			inode->uid,
			(char *)asio_dump_lv1(abuf, sizeof(abuf), vector)));

	if (ior < 0) {
		TRACE((T_WARN|T_ASIO|T_BLOCK, "INODE[%u]/REF[%d] : ASIO{%s} - scheduling failed\n", 
					inode->uid, 
					(int)inode->refcnt, 
					(char *)asio_dump_lv1(abuf, sizeof(abuf), vector)));
		/* any IO handled in asio_schedule() */

		for (i = 0; i < bcnt; i++) {
			ba[i]->error 		= -ior;
			ba[i]->binprogress = 0;
		}
		asio_cancel_schedule(vector, -ior);
		asio_destroy(inode, vector);
	}

	return ior;
}


static int
blk_allowable_request_nolock(fc_inode_t *inode, nc_off_t foffset, size_t size)
{
	int 	vr = U_TRUE;
	char	ibuf[512];

	if (!blk_valid_range(inode, foffset)) {
		TRACE((T_INODE, "VOLUME[%s].CKey[%s]:INODE[%u]- read at offset %lld beyond EOF;{%s}\n",
						inode->volume->signature, 
						inode->q_id, 
						inode->uid, 
						foffset,
						dm_dump_lv1(ibuf, sizeof(ibuf), inode)
						));
		vr 			=  U_FALSE;
		return vr;
	}

    if (inode->staleonclose || inode->ob_staled) {
		TRACE((T_WARN|T_INODE, 	"VOLUME[%s].CKey[%s]:INODE[%u]- already staled;{%s}\n", 
							inode->volume->signature, 
							inode->q_id, 
							inode->uid, 
							dm_dump_lv1(ibuf, sizeof(ibuf), inode)));
		vr 			= U_FALSE;
		__nc_errno 	= ESTALE;
		return vr;
	}

	return vr;

}
