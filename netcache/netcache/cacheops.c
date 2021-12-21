/*
 * inode 매니지먼트
 * - 최대한 병렬 처리 능력을 유지하면서, base의 효율적인 관리를 위해서
 *   base의 특정 필드의 관리에 대해서만 본 모듈에서 접근관리한다.
 * - 본 모듈에서 사용하는 필드들 (lock 획득 후)
 * 		(1) base
 * 				- node
 * 				- inode.stat : 0: orphan, 1: freed, 2:cached
 *				- X
 * - cache-manager global lock을 획득 상태에서 다른 lock 호출 절대 불허
 *
 *
 * - CLFU victim election 가능 여부 판단
 * 		refcnt = 0
 * - Disk-reclaim 가능여부 판단
 *		refcnt = 0 
 *				then remove-cache
 *		otherwise 
 *				stalled = 1
 *		
 */
#include <config.h>
#include <netcache_config.h>
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
#include <dirent.h>
#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#ifdef WIN32
#include <statfs.h>
#endif
#ifndef WIN32
#include <sys/vfs.h>
#endif
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

#define	INODE_FREE_SIGNATURE 	0x1234
#define	INODE_CACHED_SIGNATURE 	0xA55A

/*
 * global environment values
 */
extern link_list_t			__inode_pool;
extern memmgr_heap_t		g__page_heap;
extern memmgr_heap_t		g__inode_heap;
extern int 				g__cold_ratio;
extern void *			__timer_wheel_base;
extern __thread int		__nc_errno;
extern nc_size_t 		__max_cachable_object_size;
extern nc_int16_t 			__pagesize;
extern int 				g__enable_cold_caching;
extern int 				g__intelligent_cold_caching;
extern int				g__origin_ra_blk_cnt;
extern int				g__cache_ra_blk_cnt;
extern int				g__default_uid;
extern int				g__default_gid;
extern long				g__inode_count;
extern long				g__inode_max_count;
extern time_t			netcache_signature;
extern nc_int16_t 		__memcache_mode;
extern int				__terminating;
extern int 				__enable_compaction;
extern int 				g__delayed_sync;
/*
 * 
 *
 */
nc_ulock_t 			g__inode_cache_lock;


static link_list_t 			__inode_free;
fc_clfu_root_t				*g__inode_cache;
static bt_timer_t			__timer_inode_spinner;
#define	INODE_SPIN_PERIOD 	60000
static int 					__inode_cache_spin_interval = 1; /* 기본값 60초*/
static int 					__inode_cache_spin_interval_cur = 1;

static fc_inode_t * ic_allocate_inode(nc_volume_context_t *volume, int alloc_ext);
static void ic_spin_cache_ring(void *d);
static void ic_push_to_freelist_nolock(fc_inode_t *inode);
static fc_inode_t *ic_pop_freelist();

int
ic_shutdown()
{
	TRACE((T_INFO, "Inode Cache Manager going to be shutdown\n"));
	clfu_clean(g__inode_cache, dm_swapout_inode_nolock);
	TRACE((T_INFO, "Inode Cache Manager shutdowned\n"));
	return 0;
}
void
ic_get_stat(int *total, int *cached, int *freed, int *xtotal, int *xcached, int *xfreed)
{

	clfu_get_stat((fc_clfu_root_t *)g__inode_cache, NULL, NULL, cached, total);
	if (freed)
		*freed = link_count(&__inode_free, U_TRUE); 

	if (xtotal)
		*xtotal = 0;
	return;
}
int
ic_init()
{



	g__inode_count 		= 0;

	nc_ulock_init(&g__inode_cache_lock);
	g__inode_cache 		= clfu_create(&g__inode_cache_lock, CLFU_LT_UPGRADABLE, 60, dm_inode_idle, NULL/*dm_dump_inode_callback*/, 0, g__cold_ratio);
	DEBUG_ASSERT(g__inode_cache != NULL);
	clfu_set_signature(g__inode_cache, "INODE");

	INIT_LIST(&__inode_free); 

	bt_init_timer(&__timer_inode_spinner, "inode_spinner", U_TRUE);
	bt_set_timer(__timer_wheel_base,  &__timer_inode_spinner, 60000/* 60 sec*/, ic_spin_cache_ring, (void *)g__inode_cache);

	return 0;

}
static fc_inode_t *
ic_allocate_inode(nc_volume_context_t *volume, int alloc_ext)
{
	fc_inode_t 	*inode = NULL;
	int 		reclaimed = 0;
	int 		kept = 0;
	int 		total = 0;
	static char __is_state[][32] = {
		"IS_FREED",
		"IS_ORPHAN",
		"IS_CACHED" 
	};


/*
 *********************************************************************************************
 *
 * EXCLUSIVE LOCK 영역 시작
 *
 *********************************************************************************************
 */
	IC_LOCK(CLFU_LM_EXCLUSIVE);

	inode = ic_pop_freelist();
	if (!inode) {
		inode = dm_new_inode(0);
		if (!inode) {
			clfu_get_stat((fc_clfu_root_t *)g__inode_cache, NULL, NULL, &kept, &total);
			inode = (fc_inode_t *)clfu_reclaim_node(g__inode_cache);
			if (inode) {
				reclaimed++;
				inode->signature 	= 0;
				/*
				 * lock 범위 내에서 VIT에서 inode 조회 안되도록 등록해제
				 */
				if (inode->volume) {
					DEBUG_ASSERT(inode->volume != NULL);
					DEBUG_ASSERT(inode->volume->freed == 0);
					DEBUG_ASSERT(inode->volume->object_tbl != NULL);
					DEBUG_ASSERT(inode->q_id != NULL);
					DEBUG_ASSERT(inode->q_id[0] != 0);
					u_ht_remove(inode->volume->object_tbl, (void *)inode->q_id);
				}
				/*
				 * 여러가지 이유로 CAS를 통해서 cstat의 값이 관리되고 있는지 확인
				 */
				_ATOMIC_CAS(inode->cstat, IS_CACHED, IS_ORPHAN);
			}
			else {
				TRACE((T_ERROR, "no idle inode-base found: inode.free=%d, kept=%d\n", link_count(&__inode_free, U_TRUE), kept));
				clfu_get_stat((fc_clfu_root_t *)g__inode_cache, NULL, NULL, &kept, &total);
				dm_dump_inodes();
				TRAP;
			}
		}
		else {
			clfu_set_max(g__inode_cache, g__inode_count);
		}
	}

	IC_UNLOCK;
/*
 *********************************************************************************************
 *
 * EXCLUSIVE LOCK 영역 끝
 *
 *********************************************************************************************
 */


	if (inode && reclaimed) {
		DEBUG_ASSERT_FILE(inode, (INODE_GET_REF(inode) == 0));

		dm_evict_inode_nolock(inode, (inode->volume != NULL));
	}
	INODE_RESET(inode); /* 몇개의 필드가 ic-lock을 필요로함 */
	inode->signature = dm_make_signature();
	DEBUG_ASSERT(inode->cstat == IS_ORPHAN);

	return inode;
}
/*
 * 
 * stat 및 주어진 파라미터를 이용해서
 * inode 생성
 * 가정 : path-lock을 이미 획득 후 호출되는 것을 가정
 *
 */
fc_inode_t *
ic_prepare_inode_raw_nolock( 	nc_volume_context_t 	*volume, 
								char 					*cache_key,
								char 					*origin_path,  
								nc_mode_t 				mode,  /* stat 구조내의 mode 값에 우선한다 */
								nc_stat_t 				*stat,
								nc_xtra_options_t 		*reqkv,
								int 					complete,
								nc_uint32_t 			cversion,
								u_boolean_t				template,
								int 					markbusy,
								char					*f,
								int						l
							)
{

	fc_inode_t 			*inode = NULL;
	char 				ibuf[1024]	= "";
	nc_uint32_t			tflg 		= T_INODE;



	errno = 0;


	if (stat && stat->st_size < 0LL) {
		TRACE((T_ERROR, "Volume[%s].CKey[%s]: st_size of origin[%s] has wrong information(%lld)!\n", 
						volume->signature,
						cache_key,
						origin_path, 
						stat->st_size));
		inode = NULL;
		__nc_errno = errno = EFBIG; 
		return inode;
	}


	inode = ic_allocate_inode(volume, template == 0);


	if (!inode) {
		inode = NULL;
		__nc_errno = errno = ENOMEM;
		TRACE((T_ERROR, "FILE['%s'] failed to allocate a new inode(no available inode)\n", origin_path));
		return inode;
	}


	inode->volume				= volume;
	inode->q_path 				= (char *)mem_strdup(&g__inode_heap, origin_path, AI_INODE, __FILE__, __LINE__);
	inode->q_id 				= (char *)mem_strdup(&g__inode_heap, cache_key, AI_INODE, __FILE__, __LINE__);


	inode->writable				= 0;
	if (cversion != VOLUME_VERSION_NULL) {
		inode->cversion = cversion;
	}

	

	/* init stat properties */
	if (stat == NULL) {
		/*
		 * TODO)))
		 * solproxy의 경우 PUT operation 구현에 사용될 수 있음
		 * 코드 재검토 필요 
		 * write를 위해서 신규로 생성되는 객체
		 * 기본가정 :inode의 모든 필드는 초기화되었다고 가정
		 */
		inode->ctime 		=
		inode->vtime 		= 
		inode->mtime 		= nc_cached_clock(NULL);

		inode->imode 		= mode|S_IFREG|0444; /*  구시대의 유물.  맞는지?*/
		inode->size 		= inode->rsize = 0;
		inode->ob_priv 		= 0;
		inode->ob_vary 		= 0;
		inode->ob_chunked 	= 0;
		inode->ob_sizeknown	= 1;
		inode->ob_rangeable	= 1;
		inode->ob_memres	= 1;

		inode->cversion 	= 1;
		inode->devid[0] 	= 0;
	}
	else {
		/*
		 * stat정보를 반영하여 객체 정보 일부 생성
		 */



		inode->ob_sizeknown			= stat->st_sizeknown;
		inode->ob_sizedecled		= stat->st_sizedecled;
		if (inode->ob_sizeknown) {
			inode->size		= stat->st_size;
			inode->rsize 	= stat->st_size; /* 처음 캐싱 시작할 때는 오리진 크기 정보 사용 */
			TRACE((T_INODE, "Volume[%s]/INODE[%u] - origin reports szknown[%d]/szdecled[%d], size=%lld\n", 
							inode->volume->signature, 
							inode->uid, 
							inode->ob_sizeknown, 
							inode->ob_sizedecled, 
							inode->size));
		}
		inode->imode				= mode | stat->st_mode; /* 이 부분 확인 필요 */
		inode->ctime				= nc_cached_clock();
		inode->mtime				= stat->st_mtime;
		inode->obi					= stat->obi;

		inode->ob_rangeable 		= stat->st_rangeable && (IS_ON(mode, O_NCX_NORANDOM) == 0);
		inode->ob_priv 				= stat->st_private || (IS_ON(mode,  O_NCX_PRIVATE) != 0); 
		inode->ob_nocache 			= stat->st_nocache || (IS_ON(mode,  O_NCX_NOCACHE) != 0); 
		inode->origincode			= stat->st_origincode;
		DEBUG_ASSERT_FILE(inode, inode->origincode != 304);

		/*
		 * 캐시 유효시간 설정은  볼륨별 positive/negative 설정과
		 * kv 구조에 설정된 값등을 참고하여 결정
		 */
		dm_update_ttl(inode, stat, reqkv, 0, U_FALSE);

		inode->viewcount			= 1;

		/*
		 * property의 복사는 아래와 같은 조건에서만 실행한다.
		 * 왜냐하면 httpn_driver의 경우 lazy property update procedure를
		 * 콜백설정하므로, nvm_lazy_update_property_proc에서 
		 * property 업데이트가 또한번 일어남
		 * 조건
		 * 	- 해당 객체가 sub-range접근이 가능한 객체 
		 * 2014.8.28 재수정
		 * inode->property는 rangeable이 아니더라도 업데이트가능하도록 재수정
		 */
		if (stat->st_property)  {
			inode->property 		= stat->st_property;
			stat->st_property 		= NULL;
			kv_update_trace(inode->property, __FILE__, __LINE__);
			XVERIFY(inode->property);
			TRACE((T_INODE,  "PROP[%p] - handed over and kept into inode[%u]\n", inode->property, inode->uid));
		}


		if (isprint(stat->st_devid[0]))
			memcpy(inode->devid, stat->st_devid, sizeof(nc_devid_t));

		inode->bitmaplen			= NC_BITMAP_LEN(dm_inode_size(inode, U_FALSE));
		inode->maxblkno				= dm_max_blkno(dm_inode_size(inode, U_FALSE));
		inode->header_size			= dm_calc_header_size_v30(inode);//hi_v24->header_size;
		/*
		 * doffset의 경우 disk에 캐싱될 때 유의미
		 */
		inode->doffset 				= NC_ALIGNED_SIZE(inode->header_size, NC_ALIGN_SIZE);
		inode->headerversion 		= inode->contentversion  = 1; /* disk저장은 반드시 필요*/
		inode->mdbversion = 0;
		inode->fd					= DM_NULL_FD;
		inode->ob_upgradable 		= 0;
		inode->ob_upgraded 			= 0;
		inode->ob_created			= 1; /* 새로 생성된 캐싱 객체 */
		inode->ctime 				= nc_cached_clock(NULL); /* caching time */
		inode->ob_upgradable 		= g__enable_cold_caching;
		inode->ob_memres 			= (pm_check_partition() == 0 || g__enable_cold_caching || (stat->st_origincode != 0 && cfs_iserror(volume, stat->st_origincode)));

	}
	inode->ob_refreshstat 		= IS_ON(mode, O_NCX_REFRESH_STAT) != 0;
	inode->ob_preservecase 		= volume->preservecase;
	inode->ob_template 			= template; 

	/*
	 * ir # 31909
	 */
	inode->ob_mustexpire 		= IS_ON(mode, O_NCX_MUST_EXPIRE);



	inode->ob_complete 			= complete;
	if (INODE_SIZE_DETERMINED(inode)) {
		inode->ob_complete 		= (inode->ob_complete != 0) || (inode->size == 0);

		if (inode->size > __max_cachable_object_size) {
			inode->ob_memres 	= 1;
		}
	}

	inode->hid = dm_make_hash_key(volume->preservecase, cache_key);

	inode->hdr_version 			= INODE_HDR_VERSION_V30;
#ifdef NC_ADAPTIVE_READAHEAD
	inode->current_nra 			= g__origin_ra_blk_cnt;
	inode->current_dra 			= g__cache_ra_blk_cnt;
	inode->missratio 			= 0.0;
#endif
	inode->ob_needoverwrite 	= 0;
	inode->disksize 			= 0;
	inode->rowid				= -1LL;
	
	if (inode->cversion != volume->version) {
		/*
		 * 디스크에 저장된 객체가 volume의 컨텐츠 버전과 다름.
		 * 디스크 캐싱된 객체는 반드시 원본 조회로 유효성 검증되어야함
		 */
		inode->vtime = 0;
	}


	



	if (!template) {
		dm_resize_extent(inode, dm_inode_size(inode, U_FALSE));
	}
	inode->lastsynctime = nc_cached_clock();
	ic_set_busy(inode, markbusy, __FILE__, __LINE__);
	TRACE((0, "INODE[%d] - open_inprogress to %d at %d@%s\n", inode->uid, markbusy, l, f));


	if (!inode) tflg = T_WARN;
	TRACE((tflg|T_INODE, 	"Volume[%s].Ckey[%s] - INODE[%u] allocated {%s}\n", 
							volume->signature,
							cache_key, 
							(inode?inode->uid:-1),
							(inode?dm_dump_lv1(ibuf, sizeof(ibuf), inode):"NULL")
					));
	if (stat)
		TRACE((tflg, 	"Volume[%s].Ckey[%s] - INODE[%u] allocated by the following properties {%s}\n", 
						volume->signature,
						cache_key, 
						(inode?inode->uid:-1),
						stat_dump(ibuf, sizeof(ibuf), stat)
						));


	return inode; /* remember the inode's refcnt added by 1*/

}

fc_inode_t *
ic_prepare_inode_with_minode_nolock (nc_volume_context_t *volume, nc_mdb_inode_info_t *minode, int progress)
{
	fc_inode_t 			*inode = NULL;
	char 				ibuf[1024]="";
	char 				zuuid[1024]="";
	nc_uint32_t			tflg = T_INODE;



	inode = ic_allocate_inode(volume, minode->ob_template == 0);



	inode->hid					= minode->hid;
	inode->volume				= volume;
	inode->obi					= minode->obi; /* bit information을 고대로 한번에 복사 */
	inode->ob_needoverwrite 	= 0;
	inode->ob_frio_inprogress 	= 0;
	inode->ob_memres 			= 0;
	inode->ob_refreshstat 		= 0;
	inode->ob_pseudoprop 		= 0;





	inode->q_path 				= (char *)mem_strdup(&g__inode_heap, minode->q_path, AI_INODE, __FILE__,__LINE__);
	inode->q_id 				= (char *)mem_strdup(&g__inode_heap, minode->q_id, AI_INODE, __FILE__, __LINE__);
	inode->signature			= dm_make_signature();
	inode->writable				= 0;
	inode->cversion 			= minode->cversion;
	inode->size 				= minode->size;
	inode->rsize				= minode->rsize;
	memcpy(&inode->uuid,  minode->uuid, sizeof(uuid_t));
	inode->imode 				= minode->mode;
	inode->ctime 				= minode->ctime;
	inode->mtime 				= minode->mtime;
	inode->vtime 				= minode->vtime;
	inode->origincode 			= minode->origincode;
	memcpy(inode->devid, minode->devid, sizeof(nc_devid_t));
	inode->viewcount 			= minode->viewcount;
	inode->part 				= minode->part;
	inode->property 			= minode->property; /* property는 heap할당이였으므로 포인터만 copy */
	inode->rowid				= minode->rowid; 
	minode->property 			= NULL;
	kv_update_trace(inode->property, __FILE__, __LINE__);
	inode->hdr_version 			= INODE_HDR_VERSION_V30;

#ifdef NC_ADAPTIVE_READAHEAD
	inode->current_nra 			= g__origin_ra_blk_cnt;
	inode->current_dra 			= g__cache_ra_blk_cnt;
	inode->missratio 			= 0.0;
#endif
	inode->bitmaplen			= NC_BITMAP_LEN(minode->size);
	inode->maxblkno				= dm_max_blkno(minode->rsize);
	inode->header_size			= dm_calc_header_size_v30(inode);
	/*
	 * doffset의 경우 disk에 캐싱될 때 유의미
	 */
	inode->doffset 				= NC_ALIGNED_SIZE(inode->header_size, NC_ALIGN_SIZE);

	
	if (inode->cversion != volume->version) {
		/*
		 * 디스크에 저장된 객체가 volume의 컨텐츠 버전과 다름.
		 * 디스크 캐싱된 객체는 반드시 원본 조회로 유효성 검증되어야함
		 */
		inode->vtime 	= 0;
	}
	
	inode->ob_created 	= 0;
	inode->ob_validuuid = (uuid_is_null(inode->uuid) == 0); 
	inode->cstat		= IS_ORPHAN; /* 이전 상태과 무관하게 overwrite, disk에서 obi가 로딩되었으므로 포함된 cstat무시되어야함*/



	if (!minode->ob_template) {
		/*
		 * driver-specific open call
		 */
		dm_resize_extent(inode, dm_inode_size(inode, U_FALSE));
	}

	inode->lastsynctime = nc_cached_clock();


	if (!inode) tflg = T_WARN;

	TRACE((tflg, 	"Volume[%s].Ckey[%s] - INODE[%u].UUID[%s] restored {%s}\n", 
					volume->signature,
					minode->q_id, 
					(inode?inode->uid:-1),
					(char *)((inode && inode->ob_template==0)?uuid2string(inode->uuid, zuuid, sizeof(zuuid)):""),
					(inode?dm_dump_lv1(ibuf, sizeof(ibuf), inode):"NULL")
					));

	
	return inode;
}
/*
 * 매분 spin
 */
static void
ic_spin_cache_ring(void *d)
{
	int 	cold_percent;
	int 	base_count;
	int 	inode_kept;
	int 	inode_total;
	
	__inode_cache_spin_interval_cur--;

	IC_LOCK(CLFU_LM_EXCLUSIVE);

	if (__inode_cache_spin_interval_cur == 0) {
		/* inode clfu spin 실행*/
		clfu_spin((fc_clfu_root_t *)d);
		__inode_cache_spin_interval_cur = __inode_cache_spin_interval;
	}

	/*
	 * 현재 cold 비율을 보고 interval재조정
	 */
	if (g__enable_cold_caching) {
		clfu_get_stat((fc_clfu_root_t *)d, &cold_percent, &base_count, &inode_kept, &inode_total);
		if (cold_percent > g__cold_ratio) 
			__inode_cache_spin_interval = min(__inode_cache_spin_interval+1, 30); 
		else if (cold_percent < g__cold_ratio) 
			__inode_cache_spin_interval = max(__inode_cache_spin_interval-1, 1); 
		TRACE((T_INFO, "***[INODE]*** base_count=%d, cold_percent=%d(target cold ratio=%d): spin interval = %d sec(cur=%d)\n",
						base_count, cold_percent, g__cold_ratio, 
						__inode_cache_spin_interval, 
						__inode_cache_spin_interval_cur
						));
	}

	IC_UNLOCK;


	bt_set_timer(__timer_wheel_base,  &__timer_inode_spinner, 60000/*60초*/, ic_spin_cache_ring, (void *)g__inode_cache);
	
}
int
ic_is_hot(fc_inode_t *inode)
{
	return clfu_is_hot(g__inode_cache, &inode->node);
	
}

int
ic_hit_nolock(fc_inode_t *inode, int ntimes) 
{
	int 	r = -1;

	if (ic_is_cached_nolock(inode)) {
		/*
		 * inode base hit
		 */
		clfu_hit(g__inode_cache, &inode->node, ntimes);
		r = 0;
	}
	return r;
}
int
ic_is_cached_nolock(fc_inode_t *inode) 
{
	return (inode->cstat == IS_CACHED) && clfu_cached(&inode->node);
}
int
ic_free_object_nolock(fc_inode_t *inode)
{
	ic_push_to_freelist_nolock(inode);
	return 0;
}

int
ic_free_object(fc_inode_t *inode)
{
	int 	e;
	IC_LOCK(CLFU_LM_EXCLUSIVE);
	e = ic_free_object_nolock(inode);
	IC_UNLOCK;
	return e;
}
int
ic_free_count_nolock()
{
	return link_count(&__inode_free, U_TRUE);
}
static void
ic_push_to_freelist_nolock(fc_inode_t *inode)
{
	DEBUG_ASSERT_FILE(inode, inode->cstat == IS_FREED);
	DEBUG_ASSERT_FILE(inode, ic_is_busy(inode) == 0);
	RESET_NODE(&inode->node);
	TRACE((T_INODE, "INODE[%u] - pushed back to free list(total=%d)\n", inode->uid, link_count(&__inode_free, U_TRUE)));
	link_append(&__inode_free, (void *)inode, (link_node_t *)&(inode->node));
}
static fc_inode_t *
ic_pop_freelist()
{
	fc_inode_t 	*inode;

	inode = link_get_head(&__inode_free);

	if (inode) {
		_ATOMIC_CAS(inode->cstat, IS_FREED, IS_ORPHAN);
	}

	return inode;
}

fc_inode_t *
ic_register_cache_object_nolock(nc_volume_context_t *volume, fc_inode_t *inode)
{
	fc_inode_t 		*lookupi 	= NULL;
	nc_uint32_t		flg 		= T_ERROR;
	char			ibuf[2048];

	/*
	 * 등록전 마지막 점검
	 */
	DEBUG_ASSERT(inode->cstat == IS_ORPHAN);

	lookupi = (fc_inode_t *)u_ht_insert2_dbg(volume->object_tbl, (void *)(inode->q_id), inode, U_FALSE, __FILE__, __LINE__);
	if (lookupi == inode) {
		flg 			= T_INODE;
		inode->cstat = IS_CACHED;
#ifdef ENABLE_UUID_LOOKUP
		u_ht_insert(volume->uuid_hash, (void *)(inode->uuid), inode, U_FALSE);
#endif
		clfu_add(g__inode_cache, inode, &inode->node);

		
	}
#ifndef NC_RELEASE_BUILD
	else {
		TRACE((T_ERROR, "++ object with the same key already exist:%s\n", dm_dump_lv1(ibuf, sizeof(ibuf), lookupi)));
		TRACE((T_ERROR, "++ object to register:%s\n", dm_dump_lv1(ibuf, sizeof(ibuf), inode)));
	}
#endif

	TRACE((flg, "Volume[%s].CKey[%s] : INODE[%u] %s to register;{%s}\n", 
				inode->volume->signature,
				inode->q_id,
				inode->uid,
				((inode != lookupi)?"FAILED":"OK"),
				dm_dump_lv1(ibuf, sizeof(ibuf), inode)
				));


	return lookupi;
}
int
ic_unregister_cache_object_nolock(nc_volume_context_t *volume, fc_inode_t *inode)
{
	int 			r 		= 0;
	static char __is_state[][32] = {
		"IS_FREED",
		"IS_ORPHAN",
		"IS_CACHED" 
	};


	if (inode->cstat == IS_CACHED) {
		u_ht_remove(volume->object_tbl, (void *)inode->q_id);
		clfu_remove(g__inode_cache, &inode->node);
		inode->cstat = IS_ORPHAN;
		r = 1;
	}
	else {
		TRACE((T_INODE, "Volume[%s]/Key[%s] - INODE[%d] cache state '%s'(!IS_CACHED)\n",
						volume->signature,
						inode->q_id,
						inode->uid,
						__is_state[inode->cstat]
						));
	}
	return r;
}






/*
 * ic_set_busy()의 호출은 inode open하는 calller들임
 * path-lock으로 순차화 하므로 실제 단 하나의 thread에서만 호출됨
 * 그러므로 inode->inodebusy값의 접근을 경쟁하는 thread는
 *		(1) inode open을 실행하는 thread
 *		(2) VIT에서 reclaim할 inode를 찾는 thread
 * 두 개임
 * 
 */


void
ic_set_busy(fc_inode_t *inode, int su, char *f, int l)
{

	int		o = inode->inodebusy;

	inode->inodebusy = (su != 0?1:0);
	DEBUG_ASSERT_FILE(inode, inode->inodebusy != o);
	TRACE((T_INODE, "INODE[%d] - %s(<-%d) at %d@%s\n", inode->uid, (su?"set":"UNset"), o, l, f));
}
int
ic_is_busy(fc_inode_t *inode) 
{
	return (inode->inodebusy != 0);
}
