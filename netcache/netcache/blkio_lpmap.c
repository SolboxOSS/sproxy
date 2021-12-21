/*
 *	LPmap[logial blk #] = physical blk #
 *	valid logical  blk# : 0:inode->maxblkno
 *	valid physical blk# : 1:inode->maxblkno+1
 * 	LP[amp[logical blk#] == NULL_PBLKNO : empty not yet allocated
 *			
 *
 */

#include <config.h>
#include <netcache_config.h>

#ifdef WIN32
#include <windows.h>
#endif

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
#endif
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
#include <disk_io.h>
#include <snprintf.h>

#define 		BLK_INTRA_OFFSET(o) 		((o) % NC_BLOCK_SIZE)
#define 		BLK_NO(o, h) 				(((o) + (h)) / NC_BLOCK_SIZE)
#define			IS_SERIAL(_i)				(_i->ob_rangeable == 0)


/*
 * Main function:
 *	LPMAP[logical block #] => (pblk#+1)
 *
 *	physical file offset[physial block #] =>  header_size + (pblk#)* NC_BLOCK_SIZE 
 *
 *  LPMAP[lblkno] -1 indicates (phsyical block # + 1) 
 *  
 *
 *
 */


extern int						__terminating;
extern fc_clfu_root_t			*g__blk_cache;
extern nc_int16_t				__memcache_mode;

extern long			__s_ref_blocks;
extern long			__s_ref_blocks_osd;
extern long			__s_ref_blocks_mem;
extern long			__s_ref_blocks_dsk;
extern char 		_str_iot[10][24]; 
extern memmgr_heap_t		g__page_heap;
extern memmgr_heap_t		g__inode_heap;


#define 	LPMAP_LOCK(i_)			INODE_LOCK(i_)
#define 	LPMAP_UNLOCK(i_)		INODE_UNLOCK(i_)

/*
 * 
 * input param
 * 	blkno : logical block #
 * return
 * 	pblk#
 */

void
lpmap_free_nolock(fc_inode_t *inode, nc_blkno_t blkno)
{


	if (IS_SERIAL(inode)) return;

	LPMAP_LOCK(inode);
	/*
	 * clear
	 */
	if (inode->LPmap[blkno] != NULL_PBLKNO) {
		/* 기존에 할당된 것이 있음, clear */
		clear_bit(LPMAP_PBLKNO2BIT(inode->LPmap[blkno]), inode->pbitmap);
	}

	inode->LPmap[blkno] = NULL_PBLKNO;
	LPMAP_UNLOCK(inode);

    TRACE((T_INODE, "Volume[%s].Key[%s] :: INODE[%u].blk#%u == NULL\n",
                inode->volume->signature,
				inode->q_id,
                inode->uid,
                blkno
                ));
    return;
}

int
LPmap_verify(fc_inode_t *inode)
{
	return 0;
}



/* 
 * file offset calc'ed with the given phsysical block no
 */
nc_off_t 
lpmap_physical_offset(fc_inode_t *inode, nc_blkno_t blkno, int alloc)
{
	nc_off_t		off = 0;
	nc_blkno_t		pblkno;


	if (IS_SERIAL(inode)) {
		pblkno = blkno + 1;
	}
	else {
		if (alloc) {
			DEBUG_ASSERT(inode->LPmap[blkno] == NULL_PBLKNO);
			pblkno = find_first_zero_bit(inode->pbitmap, inode->bitmaplen) ;
			if (pblkno == inode->bitmaplen)
				pblkno = NULL_PBLKNO;
			else {
				/* 이중 set요청 검증*/
				DEBUG_ASSERT(test_bit(pblkno, inode->pbitmap) == 0);
				set_bit(pblkno, inode->pbitmap);

				pblkno = pblkno+1;

				inode->LPmap[blkno] = pblkno;

				TRACE((0, "Volume[%s].Key[%s] - Inode[%d] LPmap[blk#%u] <= pblk#%u set\n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							blkno,
							pblkno));

			}
		}
		else {
			pblkno 		= inode->LPmap[blkno];

			if (pblkno == NULL_PBLKNO) {
				char	ibuf[1024];
				TRACE((T_ERROR, "INODE[%u]/%s - blk#%ld mapped NULL;%s\n", 
								inode->uid, 
								(alloc?"alloc":"lookup"),
								(long)blkno, 
								dm_dump_lv1(ibuf, sizeof(ibuf), inode)));
				return (nc_off_t)-1;
			}
			TRACE((0, "Volume[%s].Key[%s] - Inode[%d] LPmap[blk#%u] => pblk#%u get\n",
							inode->volume->signature,
							inode->q_id,
							inode->uid,
							blkno,
							pblkno));
		}
	}
	
	off = (nc_uint64_t)inode->doffset + (pblkno-1)*(nc_uint64_t)NC_BLOCK_SIZE;
	TRACE((0, "INODE[%d]/BL[%d]%s:LPmap[blk#%u] -> pblk#%u(OFF=%lld/%lld)\n", 
							inode->uid, 
							inode->bitmaplen, 
							(alloc?"/ALLOC":""),
							blkno, 
							inode->LPmap[blkno],
							off,
							inode->size
							));

	TRACE((T_INODE|T_BLOCK, "INODE[%u]/Max[%ld]/%s - blk#%u => pblk#%u(cache file offset %lld)(D.O=%ld)\n", 
							inode->uid, 
							inode->maxblkno, 
							(alloc?"alloc":"lookup"),
							blkno, 
							pblkno, 
							(long long)off,
							(long)inode->doffset));
	
	return off;
}
void
lpmap_dump(fc_inode_t *inode, int tflag, const char *caller)
{
}

int
LPmap_recover_pbitmap_nolock(fc_inode_t *inode)
{
    int         restored = 0;
    nc_blkno_t  blkno;

	DEBUG_ASSERT(inode->LPmap != NULL);
    if (inode->mapped < (inode->maxblkno+1)) {
		TRACE((T_INODE, "INODE[%d] - block max info mismatch\n", inode->uid));
		return -1;
	}

	if (IS_SERIAL(inode))
		return inode->bitmaplen;

	/*
	 * 함수 호출 시점이 단독 접근일테니까 spinlock안씀
	 */
    for (blkno = 0; blkno <= inode->maxblkno; blkno++) {
		/*
		 *
		 * header 전체에 대해서 CRC 무결성을 통과한 상태라서
		 * 이중 삼중의 체크 필요없음
		 *
		 */
		if (test_bit(blkno, inode->bitmap) != 0) {
			set_bit(inode->LPmap[blkno]-1, inode->pbitmap);
			restored++;
		}
    }
	TRACE((T_INODE, "Volume[%s].Key[%s] - Inode[%d] %d blocks restored\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					restored));


    return restored;
}
void
lpmap_clear(fc_inode_t *inode)
{
    int     LPsize = -1;
    int     Bsize = -1;

	LPsize = NC_CANNED_LPMAP_SIZE(inode->bitmaplen);

    if (inode->LPmap) {
        memset((char *)inode->LPmap, NULL_PBLKNO, LPsize);
    }
    if (inode->pbitmap) {
        Bsize = NC_CANNED_BITMAP_SIZE(inode->bitmaplen);
        memset(inode->pbitmap, 0, Bsize);
    }

}

