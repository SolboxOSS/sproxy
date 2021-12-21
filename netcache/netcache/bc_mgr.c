/*
 * LOCKS:
 * 		- POOL
 * 		- CLFU
 *      - INODE
 *  	- block
 *
 * 
 */

#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#ifndef WIN32
#include <sys/resource.h>
#include <sys/mman.h>
#else
int getpagesize(void);
#endif
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#include <dirent.h>
#include <sys/param.h>
#include <errno.h>
#include <pthread.h>

#include <config.h>
#include <netcache_config.h>
#include <trace.h>
#include <hash.h>
#include <netcache.h>
#include <block.h>
#include <asio.h>
#include <util.h>
#include <bitmap.h>
#include <rdtsc.h>
#include <bt_timer.h>
#if 	GCC_VERSION > 40000
#include <mm_malloc.h>
#endif
#include <snprintf.h>


extern unsigned int g__blk_rd_fault;
extern __thread int __bc_locked;


memmgr_heap_t			g__page_heap;
memmgr_heap_t			g__inode_heap;


fc_blk_t					g__NC_BLOCK_PG_MARKER = {
	.signature	= 0xFFFFFFFFFFFFFFFF,
#ifdef NC_BLOCK_TRACE
	.bmagic 	= BLOCK_MAGIC,
#endif
	.buid		= BLOCK_EMPTY
}; /* IO진행을 표시하는 특별한 블럭 */
#ifdef NC_RELEASE_BUILD
nc_size_t					g__inode_heap_size = 512*1024*1024;
#else
nc_size_t					g__inode_heap_size = 24*1024*1024;
#endif


int							g__clfu_blk_base = 0;
int							g__clfu_blk_base_percent = 0;
int							g__clfu_blk_total = 0;
int							g__clfu_blk_kept = 0;
int 						g__page_size = 0;

// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
int 						g__blk_count = 0;

nc_size_t					g__cache_size 			= 1024; 
nc_uint32_t 				__max_buid 			= 0;
nc_int32_t					g__page_count 			= 0;
nc_int32_t					g__bulk_count 			= 0;
nc_int32_t					g__total_blks_allocated 			= 0;
fc_clfu_root_t			*	g__blk_cache  			= NULL;
link_list_t					*__free_blk_pool		= NULL;
nc_int16_t 					__memcache_mode 		= 0;
int 						g__per_block_pages = 0;
nc_size_t 					__max_cachable_object_size  = 0; //(65535LL-1LL)*NC_BLOCK_SIZE;
extern int 					g__block_spin_interval;
extern int 					g__cold_ratio;

nc_ulock_t 					s__blk_cache_lock;

static link_list_t 			__blk_track_list = LIST_INITIALIZER;
static						bt_timer_t 			s__bcm_timer_block_spinner;
static int 					__bcache_spin_interval = 1; /* 기본값 60초*/
static int 					__bcache_spin_interval_cur = 1;
static link_list_t 			s__page_pool;
static link_list_t 			s__bulk_list;

extern long 				g__inode_count;
extern void * 		 		__timer_wheel_base;
extern long					__s_ref_blocks;
extern long					__s_ref_blocks_osd;
extern long					__s_ref_blocks_mem;
extern long					__s_ref_blocks_dsk;

extern char __str_blk_state[][24];
static void bcm_free_all_pages_nolock(fc_blk_t *blk);
static int bcm_allocate_page_batch(nc_page_ctrl_t *map[], int alreadymap, int mapcount);
static fc_blk_t * bcm_malloc_blk_nolock();
static int bcm_is_orphaned(fc_blk_t *blk) ;
static int bcm_free_after_pages_nolock(fc_blk_t *blk, int cntvalid);
static inline int bcm_get_free_page_count_nolock();

#define		BCM_FATAL(msg_) { \
		TRACE((T_ERROR, "*************************************************************\n")); \
		TRACE((T_ERROR, "%s\n", msg_)); \
		TRACE((T_ERROR, "*************************************************************\n"));\
		exit(1); \
	}

#define		BCM_FREE_PAGE(_p_)	{link_append(&s__page_pool, (_p_), &(_p_)->node); (_p_) = NULL; }

#define		BCM_CHECK_FALSE_READ(blk__) { \
											if ((blk__)->bhit == 0 && (blk__)->bdskfault == 1) {\
												(blk__)->bdskfault = 0;\
												_ATOMIC_ADD(g__blk_rd_fault, 1);\
											}\
										}
#if 0
typedef struct {
	char	name[32];
	int		id;
	float	scale;
	int		type;
} bcm_stat_t;
bcm_stat_t bcm_stat[] = 
{
#define NC_STAT_OSDRT				0
	{"OSD.IOT(msec)", 	-1, 1000.0, STATT_ABS},
#define NC_STAT_OSDIO				1
	{"OSD.IO(Mbps)", 	-1, 1000000.0, STATT_REL},
#define NC_STAT_LRUWPUSH 			2
	{"LRUW.push(b/s)", 	-1, 1, STATT_REL},
#define NC_STAT_CACHEFAULT 			3
	{"FAULT(MB/s)", 	-1, 1000000, STATT_REL},
#define NC_STAT_SWAPIN				4
	{"SWAP.IN(b/s)", 	-1, 1, STATT_REL},
#define NC_STAT_SWAPOUT 			5
	{"SWAP.OUT(b/s)", 	-1, 1, STATT_REL},
#define NC_STAT_BLOCKCNT			6
	{"BLK USAGE(%)",	-1, 1, STATT_ABS}, /* percent */
#define NC_STAT_SCATPER				7
	{"Scatter(%)",		-1, 1, STATT_ABS}, /* percent */
#define NC_STAT_CFAIL				8
	{"C.FAIL(%)",		-1, 1, STATT_ABS}, /* percent */
#define NC_STAT_READAIO				9
	{"DSKRD(b/s)",		-1, 1, STATT_REL}, /* percent */
#define NC_STAT_CCUR				10
	{"C.CURR(%)",		-1, 1, STATT_ABS} /* count(current)/__block_count, percent */
};
#define 	NC_STAT_COUNT		(howmany(sizeof(bcm_stat), sizeof(bcm_stat_t)))
#endif

extern mavg_t						*g__prepblk;
extern mavg_t						*g__preppage;



static void bcm_dump_blks();
static void bcm_spin_block_lfu(void *d);
static int bcm_expand_cache_pages_nolock();



char *
bcm_dump_lv1(char *buf, int len, fc_blk_t *blk)
{
	char		*buf_org = buf;
	int			n;
	int 		off, rl;
	static char __is_state[][32] = {
		"IS_FREED",
		"IS_ORPHAN",
		"IS_CACHED" 
	};

#ifdef NC_BLOCK_TRACE
	n = snprintf(buf, len, "%s(blk#%u, BUID[%u],S[%s],R[%d],P[%d],s[%s],C[%d],BS[%llu],E[%d],m[%d],HIT(%c,%c,%c)", 
							((blk->bmagic == BLOCK_MAGIC)?"VALID":"CORRUPT"),
								(unsigned int)blk->blkno,
								(unsigned int)BLOCK_UID(blk),
								__is_state[blk->stat],
								(int)blk->brefcnt,
								(int)(blk->binprogress == 1),
								__str_blk_state[blk->bstate],
								(int)(blk->bcanceled == 1),
								(long long)blk->signature,
								(int)blk->error,
								(int)blk->mappedcnt,
								(blk->bhit==1?'H':' '),
								(blk->bdskfault==1?'D':' '),
								(blk->bosdfault==1?'O':' ')
						);
#else
	n = snprintf(buf, len, "(blk#%u, BUID[%u],S[%s],R[%d],P[%d],s[%s],C[%d],S[%llu],E[%d],m[%d],HIT(%c,%c,%c)", 
								(unsigned int)blk->blkno,
								(unsigned int)BLOCK_UID(blk),
								__is_state[blk->stat],
								(int)blk->brefcnt,
								(int)(blk->binprogress == 1),
								__str_blk_state[blk->bstate],
								(int)(blk->bcanceled == 1),
								(long long)blk->signature,
								(int)blk->error,
								(int)blk->mappedcnt,
								(blk->bhit==1?'H':' '),
								(blk->bdskfault==1?'D':' '),
								(blk->bosdfault==1?'O':' ')
						);
#endif
	if (SNPRINTF_OVERFLOW(len, n)) {

		off = max(0, len - 4);
		rl = min(len - off -1, 3);
		rl = max(0, rl);
		strncpy(&buf[off], "...", rl);
	}

	return buf_org;
}
char *
bcm_dump_lv2(char *buf, int len, fc_blk_t *blk)
{
	char	*buf_org = buf;
	int		n;
	int		remained = len;


	n = snprintf(buf, remained, "(REF[%d],BUID[%ld],IOP[%d],S[%s],%c)",
								blk->brefcnt,
								(long)BLOCK_UID(blk),
								blk->binprogress,
								__str_blk_state[blk->bstate],
								(char)(bcm_is_orphaned(blk)?'O':'V')
								);
	if (SNPRINTF_OVERFLOW(remained, n)) {
		SNPRINTF_WRAP(buf, len) ;
		return buf_org;
	}
	remained 	-= n;
	buf 		+= n;
	*buf = 0;

	return buf_org;
}
static int
bcm_is_orphaned(fc_blk_t *blk) 
{
	return  (blk->signature == 0);
}

/*
 * chunk global lock을 획득한 상태
 * lru에서 해당 block의 lock을 획득한 상태
 * 체크하여, idle한 상태 - 즉, victim으로 선정될 수 있는 상태 -
 * 라면 U_TRUE를 리턴한다.
 *
 * rangeable inode
 * 		- simple check
 * non-rangeable
 * 		- orphan check 필요
 */
#if NOT_USED
static int
bcm_block_busy_nostate(fc_blk_t *blk)
{
	int	isbusy;

	isbusy = (blk->signature != 0) &&
			 (
			 	(blk->bblocked) ||
				(blk->refcnt > 0) ||
			 	(blk->binprogress != 0) ||
			 	(blk->bstate != BS_CACHED)
			 )
			 ;
	return isbusy;
}
/*
 * 이 함수에서 IDLE이면 victim으로 선택될 수 있음
 */
static int
bcm_block_idle(fc_blk_t *blk, int updatestat)
{
	int 			isbusy = U_TRUE;


	isbusy = bcm_block_busy_nostate(blk);

	return !isbusy;
}
#endif
/*
 * block을 사용할 때 패턴
 *	- block read : refcnt +-
 *	- block fault handling시:  (inprogress, state={fault, cached_dirty)
 *	- block load fail : (?)
 *	- dibs on : bcm에서 reclaim하지 못하도록 마킹
 *
 * 해당 entry가 reclaim 가능 여부를 체크하고
 * 가능한 상태인경우에는 0또는 0보다 큰 값을
 * 불가능한 상태인 경우에는 음수를 리턴
 * blk->brefcnt가 0이된 순간 이후 blk의 valid여부는
 * **절대** 보장안됨!!
 */
static int 
bcm_find_victim(void *vd)
{
	fc_blk_t		*blk = (fc_blk_t *)vd;
	register 		int idle = U_FALSE;


	if (blk->bblocked == 0 && BLOCK_STATE(blk,BS_CACHED) && GET_ATOMIC_VAL(blk->brefcnt) == 0) {
		blk->signature 	= 0;
		idle 			= U_TRUE;
	}
	return idle;
}
#if NOT_USED
static int
bcm_dump_block(char *buf, int len, void *d)
{
	fc_blk_t	*blk = (fc_blk_t *)d;
	int 		n;

	bcm_dump_lv2(buf, len, blk);
	n = strlen(buf);
	return n;
}
#endif
static int
bcm_free_block_mem(void *d)
{
	return 0;
}
void
bcm_shutdown()
{

	fc_blk_t 				*blk;
	nc_page_ctrl_t 			*pc;
	int 					pf = 0;
	int						bcount = 0, bf = 0;

	link_node_t 	*track = NULL;

	bt_destroy_timer(__timer_wheel_base, &s__bcm_timer_block_spinner);
	//LRU_destroy(__free_blk_pool, bcm_free_block_mem);
	clfu_clean(g__blk_cache, bcm_free_block_mem);

	while ((track = link_get_head_node(&__blk_track_list)) != NULL) {
		blk = track->data;
		mem_free(&g__page_heap, blk, 		0, 0, __FILE__, __LINE__);
		XFREE(track);
		pf++;
	}
	bcount = link_count(&s__bulk_list, U_TRUE);
	while ((track = (link_node_t *)link_get_head_node(&s__bulk_list)) != NULL) {
			pc = (nc_page_ctrl_t *)track->data;
			nc_aligned_free(pc);
			XFREE(track);
			bf++;
	}

//	if (pf > 0) {
//		TRACE((T_BLOCK, "Block Cache Manager shutdowned ok(%d pages freed, %d/%d(%d) bulks freed)\n", pf, bf, bcount, g__bulk_count));
//	}

}

int
bcm_init(nc_size_t max_cache_size, nc_size_t max_object_in_mem, char *cache_path)
{
	char					cdir[NC_MAXPATHLEN];
	char					*cdir_p, *ep;
	char 					dbuf[128];
	int						l; 

	

	__max_cachable_object_size  = (0x7FFFFFFFLL)*NC_BLOCK_SIZE;
	g__cache_size 	= max_cache_size*1000000L;
	mem_init(&g__inode_heap, g__inode_heap_size);
	mem_init(&g__page_heap, g__cache_size);



	TRACE((T_INFO, "Size of per-block container = %d bytes, cache_size=%lld bytes \n", sizeof(fc_blk_t), g__cache_size));
	if (max_object_in_mem > 0LL)
		__max_cachable_object_size = max_object_in_mem;
	TRACE((T_INFO, "Max size of in-memory object = %s\n", 
					(char *)ll2dp(dbuf, sizeof(dbuf)-1, __max_cachable_object_size)));



	/*
	 * blk_t cache manager 초기화
	 */
	nc_ulock_init(&s__blk_cache_lock);

	g__blk_cache   = clfu_create(&s__blk_cache_lock, CLFU_LT_UPGRADABLE, 60, bcm_find_victim, NULL, -1, 30/*percent*/);
	clfu_set_debug(g__blk_cache);

	clfu_set_signature(g__blk_cache, "BUFFER");

	if (cache_path) {
		strcpy(cdir, cache_path);
		cdir_p = cdir;
		while (*cdir_p == ' ') cdir_p++;
		ep = cdir_p + strlen(cdir_p);
		while (*ep == ' ' && ep != cdir_p) {
			*ep = 0; ep--;
		}
		l = strlen(cdir_p);
		ep = cdir_p;
		while (*ep != 0) {
			if (*ep == '\\') *ep = '/';
			ep++;
		}
	}
	else {
		l = 0;
	}

	if  (l == 0) {
		TRACE((T_INFO, "cache storage not defined, running in memory-cache mode only\n"));
		__memcache_mode = 1;
	}
	else {
		if (pm_add_partition(cdir_p, 1) == NULL ) {
			TRACE((T_ERROR, "FATAL: checking cache disk failed: disk path was '%s'\n",cdir_p));
			errno = ENOENT;
			
			return -1;
		}
	}


	/*
	 * @@@@ do we need to move dm_init call to nc_init()???
	 */
	if (dm_init(cdir_p, 0 /*cap */, NULL /* reclaim */) < 0) {
		TRACE((T_ERROR, "FATAL:disk manager initialization failed!,exiting\n"));
		
		return -1;
	}




	bt_init_timer(&s__bcm_timer_block_spinner, "block_spinner", U_TRUE);

	/*
	 *************************************************************************************
	 * free page list 생성 및 pool에 등록
	 *************************************************************************************
	 */
	INIT_LIST(&s__page_pool);
	INIT_LIST(&s__bulk_list);
	/*
	 * page별로 malloc하는 대신 100MB 단위로 할당후 쪼개기.
	 */



	/*
	 *************************************************************************************
	 * free blk_t list 준비
	 *************************************************************************************
	 */
	g__per_block_pages  = (NC_BLOCK_SIZE + NC_PAGE_SIZE-1)/NC_PAGE_SIZE;
	__free_blk_pool 	= (link_list_t *)XCALLOC(g__per_block_pages+1, sizeof(link_list_t), AI_ETC);
	for (l = 0; l <= g__per_block_pages; l++) {
		INIT_LIST(&__free_blk_pool[l]);
	}

	
	bt_set_timer(__timer_wheel_base,  &s__bcm_timer_block_spinner, g__block_spin_interval*1000/* 60 sec*/, bcm_spin_block_lfu, g__blk_cache);
	
	return 0;
}

static void
bcm_spin_block_lfu(void *d)
{




	int 	cold_percent;
	int 	base_count;
	int 	chunk_kept;
	int 	chunk_total;

	__bcache_spin_interval_cur--;
	if (__bcache_spin_interval_cur == 0) {
		BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
			clfu_spin((fc_clfu_root_t *)g__blk_cache);
		}
		__bcache_spin_interval_cur = __bcache_spin_interval;
		//TRACE((T_INFO, "*** BLOCK CACHE(experimental) - spined(interval=%d)\n", __bcache_spin_interval));
	}
		/*
		 * 현재 cold 비율을 보고 interval재조정
		 */
	BC_TRANSACTION(CLFU_LM_SHARED) {
		clfu_get_stat((fc_clfu_root_t *)d, &cold_percent, &base_count, &chunk_kept, &chunk_total);
	}
	if (chunk_kept > chunk_total/2) {
 		if (cold_percent > g__cold_ratio) 
			__bcache_spin_interval = min(__bcache_spin_interval+1, 60); 
		else if (cold_percent < g__cold_ratio) 
			__bcache_spin_interval = max(__bcache_spin_interval-1, 1); 
		TRACE((T_INFO, "***BLOCK CACHE(experimental) base_count=%d, cold_percent=%d(target cold ratio=%d): spin interval = %d mins(cur=%d)\n",
				base_count, 
				cold_percent, 
				g__cold_ratio, 
				__bcache_spin_interval, 
				__bcache_spin_interval_cur
				));
	}
	else {
		TRACE((T_INFO, "*** BLOCK CACHE(experimental) - spin_interval=%d mins, spin_interval_cur(remained)=%d, total=%d, kept=%d\n", __bcache_spin_interval, __bcache_spin_interval_cur, chunk_total, chunk_kept));
	}
	bt_set_timer(__timer_wheel_base,  &s__bcm_timer_block_spinner, g__block_spin_interval*1000/* msec*/, bcm_spin_block_lfu, g__blk_cache);




}

void
bcm_put_to_pool_nolock(fc_blk_t *blk, int pagefree)
{


	DEBUG_ASSERT(__bc_locked == CLFU_LM_EXCLUSIVE);
	DEBUG_ASSERT_BLOCK(blk, (blk->stat == IS_ORPHAN));
	DEBUG_ASSERT_BLOCK(blk, (blk->brefcnt == 0));
	TRACE((0, "BUID[%ld]/R[%d]/P[%d] --> FREE-POOL\n", BLOCK_UID(blk), blk->brefcnt, blk->binprogress));

	BCM_CHECK_FALSE_READ(blk); 

	blk_update_state(NULL, blk, BS_INIT, __FILE__, __LINE__);
	blk->stat 		= IS_FREED;
	blk->signature 	= 0;
	if (pagefree) {
		bcm_free_all_pages_nolock(blk);
		blk->mappedcnt 	= 0;
	}

	/*
	 * pages수를 기준으로 free_blk_pool index에 
	 * 등록
	 * node 구조의 상위 필드는 lru_node_t와 호환됨
	 */
	DEBUG_ASSERT(clfu_cached(&blk->node) == 0);
	link_append(&__free_blk_pool[blk->mappedcnt], blk, (link_node_t *)&blk->node);
	//TRACE((T_BLOCK, "BUID[%ld]/%d pages on LIST.FREE_BLOCK\n", BLOCK_UID(blk), blk->mappedcnt));
	return;
}

void
bcm_put_to_pool(fc_blk_t *blk, int pagefree)
{
	DEBUG_ASSERT_BLOCK(blk, (blk->stat == IS_ORPHAN));

	BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
		bcm_put_to_pool_nolock(blk, pagefree);
	}
}

/*
 * DESCRIPTION
 *		npages 갯수의 pages를 가진 블럭 할당
 *		이미 __free-blk_pool은 0~g__per_block_pages까지의 page를 가진
 *		블럭이 free-list를 array형태로 관리하는 중
 */
fc_blk_t *
bcm_get_block_from_pool(int npages)
{

	fc_blk_t 		*b 		= NULL;
	int				pidx 	= npages;
	int				lowm;
	

	b = (fc_blk_t *)link_get_head(&__free_blk_pool[pidx]);

	while (!b && pidx < g__per_block_pages) {
		pidx++;

		b = (fc_blk_t *)link_get_head(&__free_blk_pool[pidx]);
	}

	if (b) {
		if (b && b->mappedcnt > npages) {
			for  (pidx = npages; pidx < b->mappedcnt; pidx++) {
				BCM_FREE_PAGE(b->pages[pidx]);
			}
		}
		TRACE((0, "%d-pages block required: BUID[%ld]/%d pages chosen, %d pages returned to free page list\n",
						npages,
						b->buid,
						b->mappedcnt,
						(b->mappedcnt - npages)
						));
		b->mappedcnt = npages;
	}
	else {
		lowm = (npages > 0? 1:0);
		for  (pidx = npages-1; b == NULL && pidx >= lowm; pidx--) {
			b = (fc_blk_t *)link_get_head(&__free_blk_pool[pidx]);
		}

#if 0
		if (b) {
			TRACE((T_BLOCK, "%d-pages block required:BUID[%ld]/%d pages chosen\n",
							npages,
							b->buid,
							b->mappedcnt
							));
		}
#endif
	}
	if (b) {
		b->stat 	= IS_ORPHAN;
		b->blkno 	= -1;
	}

	
	return b;
}

int 
bcm_alloc_page_from_blks(int npages)
{
	fc_blk_t	*blk = NULL;
	int			i, j;
	int			tofree = 0;
	int			nf = 0;
	int			nfblks = 0;
	//char		bbuf[256];


	do {
		blk = bcm_get_block_from_pool(npages);

		if (!blk) {

			blk = clfu_reclaim_node(g__blk_cache);

			if (!blk) break;
			nfblks++;

			BCM_CHECK_FALSE_READ(blk); 
			blk->stat = IS_ORPHAN;
			//TRACE((T_BLOCK, "BUID[%ld] RECLAIMED;%s\n", BLOCK_UID(blk), bcm_dump_lv1(bbuf, sizeof(bbuf), blk)));

		}
		BLOCK_MARK_INVALID(blk);


		tofree		= max(0, (npages - bcm_get_free_page_count_nolock()));
		tofree 		= min(tofree, blk->mappedcnt);
	
		j = blk->mappedcnt - 1;
		for (i = 0; i < tofree; i++) {
			BCM_FREE_PAGE(blk->pages[j]); /* j--를 사용하면 절대 안됨!!*/

			j--;
			blk->mappedcnt--;
		}
		bcm_put_to_pool_nolock(blk, U_FALSE);
	} while ((nf = bcm_get_free_page_count_nolock()) < npages); /* free 된 page가 하나도 없으면 반복 */
	//TRACE((T_BLOCK, "%d pages freed (required #pages=%d, nfblks=%d)\n", nf, npages, nfblks));


	return nf;
}

static int
bcm_calc_page_count(fc_inode_t *inode, long blkno)
{
	int		page_cnt;

	if (inode->writable)
		page_cnt 		= NC_BLOCK_SIZE/NC_PAGE_SIZE;
	else {
		if (INODE_SIZE_DETERMINED(inode)) {

			page_cnt = min(NC_BLOCK_SIZE, (dm_inode_size(inode, U_FALSE) - (long long)blkno * NC_BLOCK_SIZE));
			page_cnt = NC_ALIGNED_SIZE(page_cnt, NC_PAGE_SIZE) / NC_PAGE_SIZE;

		}
		else {
			if (blkno < inode->maxblkno) {
				/*
				 * maxblkno보다 작으면 무조건 full mapping
				 * frio_inprogress =1 인 상태이지만, 일부 데이타가
				 * 디스크에 존재하는 경우고, 이를 읽어들일 block할당의 경우임
				 */
				page_cnt 		= NC_BLOCK_SIZE/NC_PAGE_SIZE;
			}
			else
				page_cnt 		= 1;
		}
	}

	return page_cnt;

}
fc_blk_t *
bcm_alloc(fc_inode_t *inode, long blkno, int needmap)
{
	fc_blk_t 					*blk 	= NULL;
	int 						retries = 0;
	int							reclaimed = 0;
	//char						dbuf[512];
	nc_int64_t 					page_cnt = 0;
	int							frompool = 0;
	int							alloced = 0;
	int							retry = 5;

#ifdef __PROFILE
	perf_val_t 		ps, pe;
	perf_val_t 		pbs, pbe;
	long long		d1=0, d1_sum = 0;
#endif

	

	PROFILE_CHECK(pbs);
	if (needmap) page_cnt = bcm_calc_page_count(inode, blkno);

L_retry:
	reclaimed = 0;

	do {
		retries++;
		reclaimed = 0;
		/*
		 * free-list에 idle buffer가 있으면 그곳으로 부터 할당 시도
		 */
		blk = bcm_get_block_from_pool(page_cnt);

		if (!blk) {
			blk = bcm_malloc_blk_nolock();
			if (blk) {
				g__total_blks_allocated++;
				clfu_set_max(g__blk_cache, g__total_blks_allocated);
				alloced++;
				//TRACE((T_BLOCK|T_INODE, "BUID[%ld] - from heap to INODE[%d] in progress\n", (long)BLOCK_UID(blk), inode->uid));
			}
			else {
				/*
				 * clfu 리스트 내에서 현재 idle인 희생양 선택
				 */
				blk = clfu_reclaim_node(g__blk_cache);
				if (!blk) {
					if (retry > 0) {
						BC_UNLOCK;
						bt_msleep(10);
						BC_LOCK(CLFU_LM_EXCLUSIVE);
						retry--;
						goto L_retry;
					}

					bcm_dump_blks();
				}
				/*
				 * block from CLFU list
				 */
				reclaimed 	= 1;
				blk->blkno 	=  -2;
				blk->stat 	=  IS_ORPHAN;
				blk_update_state(NULL, blk, BS_INIT, __FILE__, __LINE__);
				BCM_CHECK_FALSE_READ(blk); 


			}
			BLOCK_MARK_INVALID(blk);
		}
		else {
			//TRACE((T_BLOCK|T_INODE, "BUID[%ld] - from pool to INODE[%d] in progress\n", (long)BLOCK_UID(blk), inode->uid));
			frompool++;
		}
	} while (blk == NULL && retries < 10);


	/*
	 * BC LOCK 바깥 영역
	 */

	if (blk) {
		/*
		 * blk는 더이상 다른 thread들에 의해서 접근되지 않는 블럭
		 */
		DEBUG_ASSERT_BLOCK(blk, (blk->stat == IS_ORPHAN) && (blk->brefcnt == 0));

		if (reclaimed) {
			DO_PROFILE_USEC(ps,pe, d1) {
				/* 
				 * 필요하면 page free 
				 * 	- neemap == U_FALSE인 경우
				 *  - page갯수가 다른 경우
				 */
				if (!needmap) {
					bcm_free_all_pages_nolock(blk);
				}
				else {
					bcm_free_after_pages_nolock(blk, page_cnt);
				}
			}

			// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
			//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
			//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
			//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
			//BLOCK_RESET(blk);

			d1_sum += d1;
		}

		if (needmap && (page_cnt - blk->mappedcnt) > 0 ) {
			DO_PROFILE_USEC(ps,pe, d1) {
				blk->mappedcnt = bcm_allocate_page_batch(blk->pages, blk->mappedcnt, (page_cnt - blk->mappedcnt));
			}
			d1_sum += d1;
		}

		// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
		//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
		//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
		//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
		BLOCK_RESET( blk );

		mavg_update(g__preppage, (double)d1_sum/1000.0);
		/* block의 소속 inode의 signature 승계 */

	}
	
	PROFILE_CHECK(pbe);
	mavg_update(g__prepblk, (double)PROFILE_GAP_USEC(pbs, pbe)/1000.0);
	return blk;
}


/*
 * inode내의 blockmap에 block에 binding되어 있고, valid한 상태 
 * 즉, reclaim되지 않은 상태인지 확인 후 
 * 해당 블럭을 리턴
 * dolock 파라미터에 따라 lock을 획득한상태로 리턴될 수 있다.
 * makeref 파라미터 값에 따라 block의 refcount가 증가할 수 있다
 * inode lock내에서 호출되어야함, blockmap이 realloc되므로
 */
fc_blk_t *
bcm_lookup_LIGHT(fc_inode_t *inode, long blkno/* ligical blk # */, int dolock, char *f, int l)
{
	//fc_blk_t 	*blk;
	//int			prog_marked = 0;

	

//	if (!(blk = dm_get_block_nolock(inode, blkno))) {
//		return NULL;
//	}
//
//	prog_marked = BLOCK_IS_PG_MARKED(blk); 
//
//
//	if (prog_marked) return blk; /* don't make_ref for PG_MARK */
//	return blk;

	return dm_get_block_nolock(inode, blkno);


}
void
bcm_hit_block_nolock(fc_blk_t 	*blk, int ntimes)
{
	bcm_update_ref_stat(blk, ntimes);
	clfu_hit(g__blk_cache, &blk->node, ntimes);
}
void
bcm_hit_block(fc_blk_t 	*blk, int ntimes)
{
	bcm_hit_block_nolock(blk, ntimes);
}
/*
 * cntvalid만큼의 page만 valid하고 나머지 page는 free 시킴
 */

static int
bcm_free_after_pages_nolock(fc_blk_t *blk, int cntvalid)
{
	int 	i, cnt=0;
	if (blk->mappedcnt > cntvalid)  {
		for (i = cntvalid; i < blk->mappedcnt; i++) {
			BCM_FREE_PAGE(blk->pages[i]);	
			cnt++;
		}
		blk->mappedcnt -= cnt;
	}
	return blk->mappedcnt;
}
/*
 * blk에 할당된 모든 page의 free
 */
static void
bcm_free_all_pages_nolock(fc_blk_t *blk)
{
	int 		i;

	for (i = 0; i < blk->mappedcnt; i++) {
		BCM_FREE_PAGE(blk->pages[i]);	
	}
	memset(blk->pages, 0, g__per_block_pages*sizeof(nc_page_ctrl_t *)); 
	blk->mappedcnt =  0;
}
void
bcm_free_page(void *page)
{
	nc_page_ctrl_t 	*p = (nc_page_ctrl_t *)page;
	BC_LOCK(CLFU_LM_EXCLUSIVE);
	link_append(&s__page_pool, p, &p->node);
	BC_UNLOCK;
}
static nc_page_ctrl_t *
bcm_get_page_nolock()
{
	nc_page_ctrl_t 	*p;


	p = (nc_page_ctrl_t *)link_get_head(&s__page_pool);

		
	return p;
}
#ifdef NOT_USED
static int
bcm_free_all_pages(fc_blk_t *blk)
{
	int	r;
	BC_LOCK(CLFU_LM_EXCLUSIVE);
	r = bcm_free_all_pages_nolock(blk);
	BC_UNLOCK;
	return r;
}
#endif
/*
 * pool에서 page memory 할당
 * pool에 free된 page가 없다면 bllk를 reclaim해야함
 */
nc_page_ctrl_t *
bcm_allocate_page()
{
	nc_page_ctrl_t 		*page = NULL;
	fc_blk_t 			*blk = NULL;




	do {
		blk = NULL;

		page = bcm_get_page_nolock();

		if (page == NULL) {
			bcm_expand_cache_pages_nolock();
			page = bcm_get_page_nolock();
			if (!page && (bcm_alloc_page_from_blks(1) > 0)) { 
				page = bcm_get_page_nolock();
			}
		}

	} while (page == NULL);
	return page;
}
static inline int
bcm_get_free_page_count_nolock()
{
	return link_count(&s__page_pool, U_TRUE);
}
int
bcm_get_free_page_count()
{
	int 	r;
	BC_LOCK(CLFU_LM_SHARED);
	r = link_count(&s__page_pool, U_TRUE);
	BC_UNLOCK;
	return r;
}

/*
 * alreadymapped: map[] array내에서 valid한 mapping 갯수
 * mapcount: 추가로 필요한 페이지 수
 * clfu global lock의 사용을 최소화
 */
static int
bcm_allocate_page_batch(nc_page_ctrl_t *map[], int alreadymapped, int mapcount)
{
	int				realmapped = 0;
	nc_page_ctrl_t 	*page = NULL;
	int				remained_toalloc  = mapcount;
	int				blkfired = 0; /* debugging을 위한 변수 */
	int				nf = 0;
	int				nexpand = 0;
	int				loop = 0;


	realmapped = alreadymapped;

	/*
	 * 상황에 부적절할 수 있음!!!
	 */
	if (link_count(&s__page_pool, U_TRUE) < mapcount) {
		/*
		 * not enough free pages 
		 */
		nexpand = bcm_expand_cache_pages_nolock();
	}

	while (remained_toalloc > 0) {
		loop++;
		page = NULL;



		/************** CLFU LOCK의 범위 ************/

		blkfired 	= 0;
		nf			= 0;
		//
		// free page-list에 노는 memory page 확인
		//
		page = bcm_get_page_nolock();


		//
		// free page-list에 노는 page가 없어서 block cache에 확인
		// 필요하면 block을 reclaim해서 block에속한 페이지들을 free page-list에 강제 편입
		//
		if (!page && ((nf = bcm_alloc_page_from_blks(remained_toalloc)) > 0)) { 
			blkfired++;
			//
			// remained_toalloc개 이상의 free page 갯수(nf)가 확보된 상태
			//
			page = bcm_get_page_nolock();
		}
		//ASSERT(page != NULL);

		if (page) {
			map[alreadymapped ++ ] = page;
			realmapped++;
			remained_toalloc--;
		}
		else {
			if (nexpand == 0 && bcm_expand_cache_pages_nolock() == 0) {
				TRACE((T_ERROR, "******* page allocation failed!!!!!:remained_toalloc=%d, blkfired=%d, nf=%d, free_pages=%d\n",
						remained_toalloc,
						blkfired, 
						nf, 
						bcm_get_free_page_count_nolock()
						));
				bcm_dump_blks();
				TRAP;
			}
		}
		/************** CLFU LOCK의 범위 ************/
	}
	return realmapped;
}
/*
 * 지정 pageno에 해당하는 page가 없으면 할당
 *
 */
nc_uint8_t *
bcm_check_page_avail(fc_inode_t *inode, fc_blk_t *block, int pageno, int allocifnull)
{
	//char	bbuf[256];
	/*
	 * page buffer의 할당이 필요한 경우에만 lock 실행
	 */



	if (allocifnull) {
		if (!block->pages[pageno]) {
			BC_TRANSACTION(CLFU_LM_EXCLUSIVE) {
				block->pages[pageno] = bcm_allocate_page();
				block->mappedcnt++;
			}
		}
	}
	return BLOCK_PAGE_MEMORY(block->pages[pageno]);

}
blk_stream_t  *
bs_open(blk_stream_t *bs, void *inode, fc_blk_t *blk, int blksiz)
{
	bs->inode 	= inode;
	bs->buffer 	= blk;
	bs->offset 	= 0;
	bs->buffersiz = blksiz;
	return bs;
}
int
bs_lseek(blk_stream_t *bs, off_t off)
{
	int		ro = -1;
	if (off >= 0) {
		ro = bs->offset = off;
	}
	return ro;
}
int
bs_read(blk_stream_t *bs, unsigned char *buf, size_t len)
{
	int 	r = 0;
	
	if (bs->offset < bs->buffersiz) {
		r =  blk_read_bytes((fc_inode_t *)bs->inode, bs->buffer, bs->offset, (char *)buf, min((bs->buffersiz - bs->offset), len));
		bs->offset += r;
	}
	return r;
}
unsigned char
bs_char(blk_stream_t *bs)
{
	int 			r;
	unsigned char 	d = 0xFF;
	DEBUG_ASSERT(bs->offset < bs->buffersiz);
	if (bs->offset < bs->buffersiz) {
		r = blk_read_bytes((fc_inode_t *)bs->inode, bs->buffer, bs->offset, (char *)&d, sizeof(d));
		bs->offset += r;
	}
	return d;
}

int
bcm_track(int stat)
{
#ifndef NC_RELEASE_BUILD
	fc_blk_t	*blk = NULL;
	int			match = 0;
	BC_LOCK(CLFU_LM_EXCLUSIVE);
	blk= link_get_head_noremove(&__blk_track_list);
	while (blk) {
		if (blk->bstate == stat) match++;

		blk = link_get_next(&__blk_track_list, &blk->node);
	}
	BC_UNLOCK;
	return match;
#else
	return 0;
#endif
}
static fc_blk_t *
bcm_malloc_blk_nolock()
{
	fc_blk_t 		*blk = NULL;
	link_node_t 	*track = NULL;
	blk = (fc_blk_t *) mem_alloc(	&g__page_heap, 
									sizeof(fc_blk_t) + g__per_block_pages*sizeof(nc_page_ctrl_t *), 
									0, 
									AI_BLOCK, 
									U_FALSE, 
									__FILE__, 
									__LINE__);
	if (!blk) return blk;


	BLOCK_RESET(blk);

#ifdef NC_DEBUG_BLOCK
	blk->magic	= BLOCK_MAGIC;
#endif
	blk->buid = _ATOMIC_ADD(__max_buid, 1);


	LRU_init_node(&blk->node, __FILE__, __LINE__);


	/*
	 * blk_t 구조체 house-keeping
	 * ic-lock범위 내이므로 lock은 별도로 필요없음
	 */
	track = (link_node_t *)XMALLOC(sizeof(link_node_t), AI_ETC);
	link_append(&__blk_track_list, (void *)blk, track);
	blk->stat = IS_ORPHAN;
#ifdef NC_BLOCK_TRACE
	blk->bmagic = BLOCK_MAGIC;
#endif

	// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
	//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
	//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
	//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
	g__blk_count++;

	TRACE((0, "block - BUID[%ld] mem-allocated(total=%d)\n", BLOCK_UID(blk), link_count(&__blk_track_list, U_TRUE)));


	return blk;
}
#if NOT_USED
static int
bcm_check_if_page_avail_nolock(int thrpages)
{
	long 	r;
	r = link_count(&s__page_pool, U_TRUE);
	return r >= thrpages;

}
#endif
void
bcm_setup_page_size()
{
#ifdef WIN32
	SYSTEM_INFO 	si;
	GetSystemInfo(&si);
	g__page_size = si.dwPageSize;
#else

	// CHG 2019-04-23 huibong origin 처리성능 개선 (#32603)
	//                - 원부사장님 수정 소스 적용
	//                - 원부사장님 지시대로 기본 4,096 의 4 배수 = 16,384 로 설정 처리.
	//g__page_size =  getpagesize();
	//
	// streaming 과 같은 대용량 객체의 경우 효율성 좋음
	g__page_size = 4 * getpagesize();
	//g__page_size = 8 * getpagesize();

#endif
}
/*
 * allocate real memory to increase cache pages
 */
static int
bcm_expand_cache_pages_nolock()
{
	nc_uint8_t 		*pbulk = NULL;	
	int				pi;
	link_node_t		*pnode = NULL;
	int 			ralloc = 0;
	nc_page_ctrl_t 		*pc;


#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif
	pbulk = (nc_uint8_t *) mem_alloc(&g__page_heap, g__per_block_pages * NC_PAGE_SIZE, 4096/*align boundary*/, AI_BLOCK, U_FALSE, __FILE__, __LINE__);

#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
	if (pbulk) {
		pnode = (link_node_t *)XMALLOC(sizeof(link_node_t), AI_ETC);
		link_append(&s__bulk_list, pbulk, pnode);
		g__bulk_count++;

		for (pi = 0; pi < g__per_block_pages; pi++) {
			pc = (nc_page_ctrl_t *)XMALLOC(sizeof(nc_page_ctrl_t), AI_ETC);
			pc->memory = (pbulk + pi*NC_PAGE_SIZE);
			link_append(&s__page_pool, pc, &pc->node);
			g__page_count++;
			ralloc++;
		}
	}
	TRACE((0, "%d pages allocated(total=%ld)\n", ralloc, g__page_count));
	return  ralloc;
}
void
bcm_update_ref_stat(fc_blk_t *blk, int ntimes)
{
	if (blk->bdskfault) {
		blk->bdskfault = 0;

		_ATOMIC_ADD(__s_ref_blocks_dsk, 1);

		_ATOMIC_ADD(__s_ref_blocks_mem, ntimes-1);

	}
	else if (blk->bosdfault) {
		blk->bosdfault = 0;
		_ATOMIC_ADD(__s_ref_blocks_osd, 1);

		_ATOMIC_ADD(__s_ref_blocks_mem, ntimes-1);

	}
	else {
		_ATOMIC_ADD(__s_ref_blocks_mem, ntimes);
	}
	blk->bhit 	= 1;  	  			/* 사용한 상태로 변경 */
	_ATOMIC_ADD(__s_ref_blocks, ntimes); 
}



/********************** DIAG function *****************************/
/*
 * CLFU cache에 등록된 block들에 할당된 memory page의 수를 모두
 * 확인
 */

struct tag_bcm_info {
	int 	page_count;
	int 	blk_count;
};
static int 
bcm_count_blk_pages_cb(void *data, void *ud)
{
	fc_blk_t 				*blk= (fc_blk_t *)data;
	struct tag_bcm_info 	*bi = (struct tag_bcm_info *)ud;

	bi->blk_count++;
	bi->page_count += blk->mappedcnt;
	return HT_CONTINUE;
}
void
bcm_count_cache_info(int *blk_count, int *page_count)
{
	struct tag_bcm_info bi = {0,0}; 	

	BC_TRANSACTION(CLFU_LM_SHARED) {
		clfu_for_each(g__blk_cache, bcm_count_blk_pages_cb, &bi, U_TRUE);
	}

	*blk_count 	= bi.blk_count;
	*page_count = bi.page_count;
}

// NEW 2020-01-20 huibong 원부사장님 수정소스 반영 (#32924)
//                - POST 처리과정에서 solproxy에서 free된 메모리 읽기 쓰기 시도가 나중에 발생하는 문제 수정
//                - Page-fault시 disk fault, origin fault의 구분 플래그 세팅제대로 안되는 문제 수정
//                  -- disk-read-ahead 크기 동적 조절시 차이 발생
//                - #ifdef NC_DEBUG_BCM 구문부터 #endif 구문까지 추가.
#ifdef NC_DEBUG_BCM
struct bcm_stat {
	int		total; /*inuse+idle*/
	int		inuse;
	int		idle;
	int		locked;
};
int
bcm_scan_callback( void * data, void * cb_data )
{
#if 0
	struct bcm_stat * bs = ( struct bcm_stat * )cb_data;
	fc_blk_t * blk = (fc_blk_t *)data;

	bs->total++;

	if( ( blk->signature == 0 ) || BLOCK_IDLE( blk ) )
		bs->idle++;
	else {
		if( ( blk->binprogress != 0 ) || blk->brefcnt > 0 )
			bs->inuse++;
		else
			bs->idle++;
	}

	if( blk->bblocked ) bs->locked++;
#endif
	return 0; /* return value not used */
}
int
bcm_check_idle_callback( void * d, void * cb_data )
{

}
void
report_bcm_stat()
{
	struct bcm_stat 	bs;
	int					freed = 0;
	int					i;
#if 1
	TRACE(( T_MONITOR, "BCM buffer/page usage report: total %d blks\n", g__blk_count ));
	memset( &bs, 0, sizeof( bs ) );
	BC_TRANSACTION( CLFU_LM_SHARED ) {
		TRACE(( T_MONITOR, "\t\t\t\t - BLOCK.alloc : %d\n", link_count( &__blk_track_list, U_TRUE ) ));
		clfu_for_each( g__blk_cache, bcm_scan_callback, &bs, U_TRUE );
		for( i = 0; i < g__per_block_pages; i++ ) {
			freed += link_count( &__free_blk_pool[i], U_TRUE );
		}
		TRACE(( T_MONITOR, "\t\t\t\t - BLOCK.cached: total %d, busy %d, idle %d, free %d (locked=%d)\n", bs.total, bs.inuse, bs.idle, freed, bs.locked ));

		if( g__blk_count != ( bs.inuse + bs.idle + freed ) ) {
			TRACE(( T_MONITOR, "\t\t\t\t - !!!!!! some blocks missing!(%d blocks lost)\n", ( g__blk_count - ( bs.inuse + bs.idle + freed ) ) ));
			//link_foreach(g__blk_cache, int (*proc)(void *, void *), void *ud)
		}

	}
#endif
}
#endif

#ifdef NC_VALIDATE_ORIGIN_DATA
/*
 *
 * foffset을 통해서 원본 데이타의 저장된 값을 
 * 결정
 *
 * 00:0F  0 1 2 3 4 5 6 7 8 9 A B C D E F
 * 10:1F  0 1 2 3 4 5 6 7 8 9 A B C D E F
 * 
 */
void
bcm_verify_content_integrity(fc_inode_t *inode, fc_blk_t *blk, int blklength)
{
	int	 				i = 0; 
	nc_uint8_t			exp_val;
	char				dbytes[256] = "";
	static char			hv[16]="0123456789ABCDEF";


	exp_val = 0; /* 현재 chunk크기는 16의 정수배이므로 */

	blklength = min(16, blklength); /* 최대 16바이트만 검증*/

	while (blklength > 0) {
		if (hv[exp_val]  != *(nc_uint8_t *)(BLOCK_PAGE_MEMORY(blk->pages[0])+i)) {
			trc_hex(dbytes, BLOCK_PAGE_MEMORY(blk->pages[0]), 16);
			TRACE((T_ERROR, "Volume[%s].Key[%s] - INODE[%d].blk# %ld verification error:%s", 
							inode->volume->signature, 
							inode->q_id, 
							inode->uid, 
							blk->blkno, 
							dbytes));
			TRAP;
		}

		blklength--; /* next expected byte value*/
		exp_val++;
		i++;
	}


}
#endif


#ifdef NC_ENABLE_CRC
void
bcm_verify_block_crc(fc_inode_t *inode, fc_blk_t *blk)
{
	nc_size_t		len;
	nc_crc_t 		crc;
extern int						g__need_fastcrc;
	char			xbytes_1[128];
	char			xbytes_2[128];

	len = min( dm_inode_size( inode, U_TRUE ) - blk->blkno * (nc_int64_t)NC_BLOCK_SIZE, NC_BLOCK_SIZE );
	DEBUG_ASSERT(len >= 0);

	crc = blk_make_crc(inode, blk, len, g__need_fastcrc);

	DEBUG_ASSERT(dm_verify_block_crc(inode, blk->blkno, crc));

	TRACE((T_WARN,  "Volume[%s].Key[%s] - INODE[%u] blk#%u CRC[0x%08X] verified==> %02d'[%s]:::%02d'[%s]\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					blk->blkno,
					crc,
					0, trc_hex(xbytes_1, BLOCK_PAGE_MEMORY(blk->pages[0]), 16),
					blk->mappedcnt-1, trc_hex(xbytes_2, BLOCK_PAGE_MEMORY(blk->pages[blk->mappedcnt-1]), 16)
					));

}
#endif

static int
_bcm_dump_blk_info(void *u_a, void *u_b)
{
	fc_blk_t	*blk = (fc_blk_t *)u_a;
	int			*id = (int *)u_b;

	TRACE((T_ERROR, "%06d-th BLOCK: R[%d]/BUID[%d], stat[%s], mapped[%d];%s\n", 
					*id,
					blk->brefcnt,
					blk->buid,
					__str_blk_state[blk->bstate],
					blk->mappedcnt,
					(clfu_cached(&blk->node)?"CACHED":"")
					));

	(*id)++;

	return 0;
}
static void
bcm_dump_blks()
{
	int			id = 0;
	link_node_t	*cursor;
	//clfu_for_each(g__blk_cache, _bcm_dump_blk_info, &id, U_TRUE);

	
	TRACE((T_ERROR, "BLOCK DUMP : total %d blocks\n", link_count(&__blk_track_list, U_TRUE)));
	cursor = __blk_track_list.head;
	while (cursor) {
		_bcm_dump_blk_info(cursor->data, &id);
		cursor = cursor->next;
	}
}
