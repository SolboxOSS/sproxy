#ifndef __BLOCK_H__
#define __BLOCK_H__


typedef enum {
BS_INIT 			= 0,
BS_FAULT 			= 1,
BS_CACHED 			= 2,
BS_CACHED_DIRTY 	= 3,
BS_WRITEBACK_DIRTY 	= 4
} __attribute__((packed)) nc_block_state_t;


#define	NC_BLOCK_SIZE		nc_block_size()


#define 		BLK_NO(o, h) 				(((o) + (h)) / NC_BLOCK_SIZE)
#define 		FILE_OFFSET(_b) 				(nc_off_t)(((nc_off_t)_b)*NC_BLOCK_SIZE)
#define	NC_VALID_OFFSET(_v_) 		(((int)NC_BITMAP_LEN((_v_)) < 0)?0:1)
#define	NC_BLOCK_MAP_SIZE			(NC_MAX_FILE_SIZE/NC_BLOCK_SIZE)
#define	NC_BLOCK_BIT_COUNT			((NC_MAX_FILE_SIZE+NC_BLOCK_SIZE-1)/NC_BLOCK_SIZE)
#define	NC_DEFAULT_BITMAP_LEN		BITS_TO_LONGS(1000000/NC_BLOCK_SIZE)
#define	NC_BITMAP_LEN(sz)			((sz+NC_BLOCK_SIZE-1)/NC_BLOCK_SIZE)
#define	NC_BUFFER_ALIGN_BOUND		512LL
//#define	NC_BUFFER_ALIGN(s)			((long long)(((long)(s) -1 +NC_BUFFER_ALIGN_BOUND)/ NC_BUFFER_ALIGN_BOUND)* NC_BUFFER_ALIGN_BOUND)
#define	NC_BUFFER_ALIGN(s)			((NC_BITMAP_LEN(s) + 1LL /* header */) * NC_BLOCK_SIZE)
#define NC_BITMAP_ALLOC(_n)			XMALLOC((_n)


#define		BC_LOCK(lm_)			clfu_lock(g__blk_cache, lm_, __FILE__, __LINE__);__bc_locked=lm_;
#define		BC_UPGRADE				clfu_upgradelock(g__blk_cache, __FILE__, __LINE__); __bc_locked = CLFU_LM_EXCLUSIVE;
#define		BC_UNLOCK				__bc_locked = 0; clfu_unlock(g__blk_cache, __FILE__, __LINE__)

/*
 * C99 또는 GNU99에서도
 * 변수 선언과 함수 호출은 동시 호출 불가
 * condition check파트로 함수 호출 이동
 */
//#define 	BC_TRANSACTION(lm_)		for (int i__ = 0, __bc_locked = lm_; \
//										(i__ == 0) && clfu_lock(g__blk_cache, lm_, __FILE__, __LINE__); \
//										clfu_unlock(g__blk_cache, __FILE__, __LINE__), i__ = i__+1)

#define 	BC_TRANSACTION(lm_)		for (clfu_lock(g__blk_cache, lm_, __FILE__, __LINE__), __bc_locked = lm_; \
										(__bc_locked == lm_) ; \
										clfu_unlock(g__blk_cache, __FILE__, __LINE__), __bc_locked = 0)

#define 	BE_OK 				0
#define 	BE_OSD_DOWN 		-1
#define 	BE_CACHE_READ_IO 	-2
#define 	BE_RANGE_ERROR 		-3
#define 	BE_CACHE_FULL 		-4
#define 	BE_CANCELED 		-5
#define 	BE_INVALID_IO 		-6
#define 	BE_OSD_BAD_IO 		-7


#define 	BSTM_CASCADE 		0
#define 	BSTM_STOP 			-1

/*
 * remark!!) (b)->mappedcnt는 reset하지 않음
 * mappedcnt는 현재 대상 block에 binding된 memory page 정보를 
 * 실제로 가지고 있음
 */
 // NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
 //                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
 //                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
 //                  -- disk-read-ahead 크기 동적 조절시 차이 발생
#define BLOCK_RESET(b) { \
			(b)->error 		= 0; \
			(b)->binfo		= 0; \
			(b)->blkno 		= -1; \
  			(b)->error		= 0; \
			(b)->signature	= 0; \
			}

#define BLOCK_STATE(_b, _s) 			((_b)->bstate == (_s))


#ifndef NC_RELEASE_BUILD

#define	blk_make_ref_nolock(blk_) { \
								int 	r; \
								DEBUG_ASSERT(((blk_)->brefcnt >= 0)); \
								r = _ATOMIC_ADD((blk_)->brefcnt, 1); \
								TRACE((0, "BUID[%ld] - ref[%d] at %d@%s\n", (blk_)->buid, r, __LINE__, __FILE__)); \
							}
#define	blk_make_unref_nolock(blk_) { \
									int 	r; \
									DEBUG_ASSERT(((blk_)->brefcnt > 0)); \
									r = _ATOMIC_SUB((blk_)->brefcnt, 1); \
									TRACE((0, "BUID[%ld] - UNref[%d] at %d@%s\n", (blk_)->buid, r, __LINE__, __FILE__)); \
								}
#else

//#define	blk_make_ref_nolock(blk_, f_, l_) 	{TRACE((T_WARN, "+%ld [%d] at %d@%s\n", (blk_)->buid, _ATOMIC_ADD((blk_)->brefcnt, 1), l_, strrchr(f_, '/'))); }
//#define	blk_make_unref_nolock(blk_, f_, l_) {TRACE((T_WARN, "-%ld [%d] at %d@%s\n", (blk_)->buid, _ATOMIC_SUB((blk_)->brefcnt, 1), l_, strrchr(f_, '/'))); }
#define	blk_make_ref_nolock(blk_) 	_ATOMIC_ADD((blk_)->brefcnt, 1)
#define	blk_make_unref_nolock(blk_) _ATOMIC_SUB((blk_)->brefcnt, 1)

#endif
/* 
 * BLOCK_VALID() 가 0이 아니면, 해당 블럭은 소속된 inode에 유효한 블럭이라고 간주
 * reclaimed != 0 : 해당 블럭이 현재 빼앗기는 도중을 의미
 * stat == 0 :  블럭이 free된 상태
 * b->signature != inode->signature :  블럭이 다른 inode에 빼앗겨서 바인딩됨.
 * b->blkno != blkno : 블럭이 빼앗겨서 동일 또는 다른 inode의 다른 block에 할당된 상태
 * 제거(임시)	: (((_in)->maxblkno == (_b)->blkno) || ((_b)->mappedcnt == g__per_block_pages)) && \
 */


#define BLOCK_VALID_FOR_UPDATE(_in, _b, _n)  	(BLOCK_VALID(_in, _b, _n) && (!BLOCK_STATE((_b), BS_INIT))) 
#define BLOCK_VALID_TO_READ(_in, _b, _n)		BLOCK_VALID(_in, _b, _n)


#define BLOCK_VALID_TO_WRITEBACK(_in, _b, _n)  	(BLOCK_VALID((_in),_b, _n) && ((_b)->bstate == BS_WRITEBACK_DIRTY) && ((_b)->inprogress == 0))

#define BLOCK_VALID_TO_WAIT(_in, _b, _n)		(BLOCK_VALID((_in),_b, _n) && BLOCK_STATE(_b, BS_FAULT) && ((_b)->binprogress == 1))


#define	BLOCK_PAGE_MEMORY(p_)		(p_)->memory


#define	BLOCK_MARK_INVALID(_b)		{(_b)->signature = 0; (_b)->bstate = BS_INIT;}

/* 
 * special block unique ID means that the block is just NULL  data
 */
#define 	BLOCK_EMPTY 0x7FFFFFFF

#define 	NULL_BLKNO	0
#define 	NULL_PBLKNO	(nc_blkno_t)0
#define 	ZERO_BLKNO	0


// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
//#define		BLOCK_IDLE_MARK		0x00200


typedef struct tag_page_ctrl {
	nc_uint8_t	*memory; /* 실제 page크기 메모리에 대한 포인터 */
	link_node_t	node;
} nc_page_ctrl_t;

#define	BLOCK_MAGIC		0x5AA5A55A
struct tag_fc_blk {
#ifdef NC_BLOCK_TRACE
	nc_uint32_t	bmagic;
#endif
	/*
	 * for cache-coherency
	 *
	 * Intel CPU에서 sb_brefcnt가 가장 하위 바이트임
	 */
	union {
		struct {
			nc_uint16_t			sb_brefcnt;  	/* 1 byte */
		  	nc_block_state_t	sb_bstate;		/* 1 byte */
			/* bitwise value */
	  		nc_uint8_t 			sb_bblocked:1;	  // don't swap 						---
	  		nc_uint8_t       	sb_binprogress:1; // IO inprogrss                        ^
			nc_uint8_t 			sb_bosdfault:1;	  // loaded from origin not yet used,    1 byte
			nc_uint8_t 			sb_bdskfault:1;	  // loaded from disk                    |
	  		nc_uint8_t       	sb_bcanceled:1;	  // block-IO canceled                   V
	  		nc_uint8_t 			sb_bhit:1;		  // used block							---
#ifndef NC_RELEASE_BUILD
	  		nc_uint8_t 			sb_bnioprogress:1;		  // 
#endif
		} sb_;
		nc_uint32_t	sb_scalar;
	} u_;
	/* end of bit value */
#define	brefcnt			u_.sb_.sb_brefcnt
#define	bstate			u_.sb_.sb_bstate
#define	bosdfault		u_.sb_.sb_bosdfault
#define	bdskfault		u_.sb_.sb_bdskfault
#define	binprogress		u_.sb_.sb_binprogress
#define	bblocked		u_.sb_.sb_bblocked
#define	bcanceled		u_.sb_.sb_bcanceled
#define	bhit			u_.sb_.sb_bhit
#define	nioprogress		u_.sb_.sb_bnioprogress
#define	binfo			u_.sb_scalar


  	nc_int16_t			error;		/* block error code */

  	ic_cache_state_t   	stat;


	nc_blkno_t			blkno;
	nc_uint32_t 		buid;
   	lru_node_t			node;



	perf_val_t		t_begin; /* allocation time */
	perf_val_t		t_submit; /* submit time */
	perf_val_t 		t_end;   /* filled up time */
	nc_uint64_t		signature; /* 속한 inode's signature */
	/*
	 * 4KB page 관리
	 */
	nc_uint16_t		mappedcnt; 		/* total mapped page count */
#ifndef NC_RELEASE_BUILD
	char			*file;
	int				line;
	nc_uint32_t		tid;
#endif

#ifdef NC_BLOCK_TRACE
	fc_inode_t		*inode;
#endif
	nc_page_ctrl_t	*pages[0];


};

#ifdef NC_RELEASE_BUILD
#define	BLOCK_SIGNATURE(i_, b_)		((((nc_uint64_t)((i_) & 0x7FFFFFFFLL)) << 32) | (b_))
#define BLOCK_VALID(_in, _b, _n)				((_b) &&  ((_b)->signature == BLOCK_SIGNATURE((_in)->signature, _n))) 
#else
static inline nc_uint64_t BLOCK_SIGNATURE(nc_uint64_t is, nc_blkno_t blkno) { return ((nc_uint64_t)(is & 0x7FFFFFFFLL)) << 32 | (nc_uint64_t)blkno; } 
static inline int BLOCK_VALID(fc_inode_t *inode, struct tag_fc_blk *blk, nc_blkno_t blkno) { return blk &&  (blk->signature == BLOCK_SIGNATURE(inode->signature, blkno)); }
#endif
#pragma pack()
struct tag_blk_apc_read_info {
	nc_apcop_t			apc_op;
#ifndef NC_RELEASE_BUILD
	nc_uint32_t			magic;
#endif
	nc_uint32_t			seq;
	nc_file_handle_t	*fhandle;
	nc_off_t			foffset;	/* 다음 읽기 위치: 객체 내의 바이트 옵셋 */
	int					length;		/* 읽기 요청 총 바이트 수*/

	char				*buffer;
	int					boffset; /* buffer offset */

	int					xferbytes;	/* 총 copy된 바이트 수 */
	int					waiter;
	int					completed;
	int					error;
	fc_blk_t			*block;
	apc_read_context_t	*rctx;
	nc_lock_t			hlock;
	nc_cond_t			hsignal;

	bt_timer_t				t_read_timer;
	nc_apc_read_callback_t	callback;
	void					*cbdata;

	perf_val_t				start;

	nc_uint8_t				__heap;
	nc_uint8_t				__inuse;
	link_node_t				__node;
} ;

#ifndef		NC_RELEASE_BUILD
#define 	DEBUG_ASSERT_BLOCK(b_, x_)	 \
								if (!(x_)) { \
									char bdbuf[512]; \
									TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::{%s}... \n", \
															#x_, \
															__LINE__,  \
															__FILE__, \
															(char *)bcm_dump_lv1(bdbuf, sizeof(bdbuf), (b_)) \
										));  \
									__builtin_trap(); \
								}

#define 	DEBUG_ASSERT_BLOCK2(__bn, __b, __x)	if (!(__x)) { \
									char bdbuf[512]; \
	TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::block_info{%s}..,current blk # %ld,. blk->blk#%ld.BUID=%ld, VALID=%d\n", \
													#__x, __LINE__, __FILE__, \
													bcm_dump_lv1(bdbuf, sizeof(bdbuf), (__b)), \
													(long)__bn, \
													(int)((__b)?(__b)->blkno:-1), \
													(long)BLOCK_UID(__b), \
													(int)BLOCK_VALID(__f, __b, __bn) \
													));  \
												__builtin_trap(); \
								}

#define 	DEBUG_ASSERT_BLOCK3(i_, b_, n_, x_)	 \
								if (!(x_)) { \
									char bdbuf[512]; \
									TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::SIG.VALID(%d),B.VALID(%d),{%s}... \n", \
															#x_, \
															__LINE__,  \
															__FILE__, \
															((b_)->signature == BLOCK_SIGNATURE((i_)->signature, n_)),  \
															(BLOCK_VALID(i_, b_, n_) != 0), \
															(char *)bcm_dump_lv1(bdbuf, sizeof(bdbuf), (b_)) \
										));  \
									__builtin_trap(); \
								}
#else
#define 	DEBUG_ASSERT_BLOCK(b, e)	
#define 	DEBUG_ASSERT_BLOCK2(b_, e_, x_)	
#define 	DEBUG_ASSERT_BLOCK3(i_, b, n, e)	
#endif

#ifndef NC_RELEASE_BUILD
extern __thread int __bc_locked;
#endif


extern fc_blk_t		g__NC_BLOCK_PG_MARKER; /* IO가 진행중임을 표시하는 특별한 blk */
#define		BLOCK_IS_PG_MARKED(b_)		((b_) == &g__NC_BLOCK_PG_MARKER)


#define 	NC_MAX_PBLOCKNO(_i) 		((_i)->maxblkno + 1)

#define	BLK_INDICATE_ERROR(b_)	((blk == NULL) || (blk == (fc_blk_t *)EOF_BLKNO))



int bcm_init(nc_size_t max_cache_size, nc_size_t max_object_in_mem, char *cache_path);
int bcm_get_free_page_count();
//fc_blk_t * bcm_alloc(fc_inode_t *inode, long blkno, nc_size_t allocsiz, int needmap);
fc_blk_t * bcm_alloc(fc_inode_t *inode, long blkno, int needmap);
int bcm_track(int stat);
void bcm_release(fc_inode_t *inode, fc_blk_t *blk, nc_blkno_t blkno, int error);
void bcm_shutdown();
fc_blk_t * bcm_lookup_light_nolock(fc_inode_t *inode, long blkno/* ligical blk # */, int dolock);
fc_blk_t * bcm_lookup(fc_inode_t *inode, long blkno/* ligical blk # */, int dolock, char *f, int l);
void bcm_hit_block(fc_blk_t 	*blk, int ntimes);
void bcm_hit_block_nolock(fc_blk_t 	*blk, int ntimes);

void blk_update_state(fc_inode_t *inode, fc_blk_t *blk, int state, const char *_f, int _l);
int blk_prepare_batch_for_inode_nolock(fc_inode_t *inode, nc_blkno_t baseno, fc_blk_t **ba, int bcnt, int needmap);
fc_blk_t * blk_prepare_for_inode(fc_inode_t *inode, nc_blkno_t blkno, int *inprogress);
int blk_fault_batch_nolock(	fc_inode_t 	*inode,
							nc_blkno_t	baseno,
							bci_stat_t	*ba,
							int			nba,
							nc_xtra_options_t *kv);
int blk_fault_nolock(	fc_inode_t 			*inode, 
					nc_off_t 			offset, 
					nc_xtra_options_t 	*kv, 
					fc_block_status_t 	bs 
					);
nc_size_t blk_read(fc_inode_t *inode, char *apbuf, nc_off_t offset, size_t size, nc_kv_list_t *kv, long *towait_insec);
fc_blk_t * blk_prepare_for_inode_nolock(fc_inode_t *inode, nc_blkno_t blkno, int needmap);
int blk_schedule_writeback(fc_inode_t *inode, int need_flush, nc_kv_list_t *kv);
nc_size_t blk_write(fc_inode_t *inode, char *apbuf, nc_off_t offset, size_t size, nc_kv_list_t *kv);
PUBLIC_IF nc_crc_t blk_make_crc(fc_inode_t *inode, fc_blk_t *blk, ssize_t len, int need_fastcrc);
char *bcm_dump_lv1(char *buf, int len, fc_blk_t *blk);
void bcm_change_ownership(fc_inode_t *newowner, fc_inode_t *prevowner);
int blk_lock_owned(fc_blk_t *blk) ;
nc_asio_type_t blk_handle_io_event_nolock(nc_asio_type_t iot, fc_inode_t *inode, fc_blk_t *blk, nc_off_t len, int error, nc_asio_vector_t *v);
int blk_write_bytes(fc_blk_t *blk, nc_off_t offset, char *data, nc_size_t len);
//int blk_read_bytes(fc_blk_t *blk, nc_off_t offset, char *data, nc_size_t len);
int blk_read_bytes(fc_inode_t *inode, fc_blk_t *blk, nc_off_t foffset, char *data, nc_size_t maxlen);
nc_uint8_t * bcm_check_page_avail(fc_inode_t *inode, fc_blk_t *block, int pageno, int allocifnull);
int bcm_get_free_blks();
void blk_schedule_io(fc_inode_t *inode, nc_off_t off, nc_size_t len, nc_kv_list_t *req_prop);
void blk_signal(fc_blk_t *blk);
fc_blk_t * blk_get(fc_inode_t *inode, long blkno, int *ioerror, int debug);
blk_apc_read_info_t * blk_ri_init(blk_apc_read_info_t *ri, nc_file_handle_t * fh, nc_off_t off, char *buffer, nc_size_t len);
int blk_ri_wait(blk_apc_read_info_t *ri, long *wait_inmsec);
void blk_ri_destroy(blk_apc_read_info_t *ri);
void blk_ri_reuse(blk_apc_read_info_t *ri);
char* blk_apc_dump_RI(char *buf, int len, blk_apc_read_info_t*ri);

nc_size_t blk_read_apc(	fc_inode_t *, char *, nc_off_t, size_t , nc_xtra_options_t *, apc_block_ready_callback_t ,  blk_apc_read_info_t *);
void blk_destroy_read_apc(apc_read_context_t *rctx);
char * blk_apc_dump_context(char *buf, int len, apc_read_context_t *rctx) ;
void bcm_free_page(void *page);
int bcm_get_free_page_count();
nc_page_ctrl_t * bcm_allocate_page();
fc_blk_t * bcm_alloc_nolock(fc_inode_t *inode, long blkno, nc_size_t blksize, int needmap);
int blk_apc_resume_read(apc_read_context_t *rctx, fc_blk_t * blk);
void bcm_update_ref_stat(fc_blk_t *blk, int ntimes);
int bcm_valid_block_nolock(fc_inode_t *inode, fc_blk_t *blk, nc_blkno_t blkno);
fc_blk_t * bcm_lookup_LIGHT(fc_inode_t *inode, long blkno/* ligical blk # */, int dolock, char *f, int l);
fc_blk_t * bcm_get_block_from_pool(int npages);
void bcm_put_to_pool(fc_blk_t *blk, int pagefree);
void bcm_put_to_pool_nolock(fc_blk_t *blk, int pagefree);


int ra_invoke_readahead(fc_inode_t *inode, nc_off_t cursor, void *cbdata, nc_kv_list_t *kv, int scanfromcursor);


char * lpmap_dump_string(fc_inode_t *inode, char *buf, int len);
PUBLIC_IF nc_blkno_t lpmap_get_l2p_nolock(fc_inode_t *inode, nc_blkno_t blkno) ;
PUBLIC_IF void lpmap_reset_nolock(fc_inode_t *inode) ;
PUBLIC_IF int lpmap_prepare_nolock(fc_inode_t *inode) ;
nc_blkno_t lpmap_allocate_physical_block_nolock(fc_inode_t *inode, nc_blkno_t blkno);
nc_off_t lpmap_physical_offset(fc_inode_t *inode, nc_blkno_t blkno, int alloc);
PUBLIC_IF int lpmap_recover_error_nolock(fc_inode_t *inode, nc_blkno_t blkno);
PUBLIC_IF nc_blkno_t lpmap_valid_nolock(fc_inode_t *inode, nc_blkno_t blkno);
PUBLIC_IF int LPmap_recover_pbitmap_nolock(fc_inode_t *inode);
nc_blkno_t LPmap_restore_cursor(fc_inode_t *inode);
PUBLIC_IF nc_blkno_t LPmap_find_cursor(fc_inode_t *inode) ;
PUBLIC_IF void lpmap_clear(fc_inode_t *inode);
int lpmap_resize_nolock(fc_inode_t *inode, long sz);
void lpmap_dump(fc_inode_t *inode, int tflag, const char *caller);
int LPmap_verify(fc_inode_t *inode);
int LPmap_copy(fc_inode_t *inode, nc_uint32_t *lpmap);
void lpmap_free_nolock(fc_inode_t *inode, nc_blkno_t blkno);


/*
 * The following function might be changed 
 */
void bcm_verify_content_integrity(fc_inode_t *inode, fc_blk_t *blk, int blklength);
void bcm_verify_block_crc(fc_inode_t *inode, fc_blk_t *blk);

#endif /* __BLOCK_H__*/

