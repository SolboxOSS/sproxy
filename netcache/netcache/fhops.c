/*
 * 	File Handle Operations
 *
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

#define 	FIO_MAGIC 	(0x5AA5A55A)
extern __thread int		__nc_errno;
extern glru_t 			*g__path_cache; 

extern mavg_t					*g__rapc; /* readapc 시간*/
extern mavg_t					*g__wapc; /* blk_ri_wait 시간*/
extern mavg_t					*g__readi;
extern void *					__timer_wheel_base;
extern int 						g__pending_io;
extern int 						g__pending_open;

typedef struct fio_open_sync_info {
	nc_volume_context_t	*volume;
	char 				*key;
	char 				*path;
	int 				mode;
	nc_xtra_options_t 	*prop;
	fc_file_t 			*fh;
	int 				error;
	int 				origincode;
	nc_stat_t			*stat;
	nc_path_lock_t		*pathlock;
	pthread_cond_t		hsignal;
	int 				completed;
#ifndef NC_RELEASE_BUILD
	void 				*user_data_back; /* 김현호 차장의 user_data corrupt주장에 대한 검증용 */
#endif
	nc_apc_open_callback_t 			user_callback; 
	void 							*user_data;
} fio_osi_t;
extern fc_clfu_root_t			*	g__blk_cache ;


static void fio_on_complete_apc_open(nc_file_handle_t *fhandle, nc_stat_t *stat, int error, void *callback_data);

static int
fio_is_writable(nc_mode_t mode)
{
        return ((mode & (O_WRONLY|O_CREAT|O_RDWR)) != 0);
}

static fio_osi_t *
fio_osi_init(fio_osi_t *osi, nc_volume_context_t *volume, char *key, char *path, int mode, nc_stat_t *sp, nc_xtra_options_t *prop)
{

	memset(osi, 0, sizeof(fio_osi_t));
	osi->fh 		= NULL;
	osi->error 		= 0;
	osi->stat  		= sp;
	osi->mode  		= mode;
	osi->prop  		= prop;
	osi->completed  = 0;
	osi->volume  	= volume;
	osi->key  		= key;
	osi->path  		= key;


	condition_init(&osi->hsignal);
	/*
	 * path-lock으로 대체함
	 */
	osi->pathlock = nvm_path_lock_ref(volume, path, __FILE__, __LINE__); 

	return osi;
}
static int 
fio_osi_wait(fio_osi_t *osi)
{
	int 				r = 0;
	nc_time_t 			to, tanc;
	int 				i = 0;
	nc_uint32_t			tflg = 0;
	char				ibuf[2048];
	char				pbuf[2048];


	struct 		timespec	ts={0,0};

	TRACE((T_INODE, "Volume[%s].Key[%s] - OSI[%p] waiting (r=%d,, completed=%d)(unlocked while waiting)\n", osi->volume->signature, osi->key, osi, r, osi->completed));	

	tanc 	= nc_cached_clock();
	to 		= tanc + 60;
	do {
		make_timeout(&ts, 10, CLOCK_MONOTONIC);
		r = nvm_path_lock_cond_timedwait(&osi->hsignal, osi->pathlock, &ts);
		if (nc_cached_clock() > to) {
			i++;
			TRACE((T_WARN, "Volume[%s].CKey[%s] - too long wait on COND[%p] (%d time(s)).PL{%s}\n", 
							osi->volume->signature, 
							osi->key, 
							&osi->hsignal,
							i,
							(char *)nvm_path_lock_dump(pbuf, sizeof(pbuf), osi->pathlock)
							));
			to 		= nc_cached_clock() + 60;
			tflg 	= T_WARN;
			break;
		}
	} while (osi->completed == 0);
	TRACE((tflg|T_INODE, "Volume[%s].CKey[%s] - OSI[%p] wait done[%d secs] (r=%d,, completed=%d, error=%d, code=%d);%s\n", 
					osi->volume->signature, 
					osi->key, 
					osi, 
					(nc_cached_clock() - tanc),
					r, 
					osi->completed,
					osi->error,
					(osi->stat?osi->stat->st_origincode:-1),
					(osi->fh?dm_dump_lv1(ibuf, sizeof(ibuf), osi->fh->inode):"NULL")
					));	
	return r;
}
static void
fio_osi_destroy(fio_osi_t *osi, char *f, int l)
{

	osi->completed = -1; /* for easier debugging */
	nvm_path_lock_unref(osi->volume, osi->pathlock, f, l); 
	pthread_cond_destroy(&osi->hsignal);
}


fc_file_t *
fio_open(nc_volume_context_t *volume, char *cachepath, char *originpath, nc_mode_t mode, nc_xtra_options_t *list)
{
	fc_inode_t 	*inode;
	fc_file_t 	*fh = NULL;
	fio_osi_t		osi;

	
	if (fio_is_writable(mode) && !nvm_space_avail(volume, originpath)) {
		TRACE((T_WARN, "VOLUME[%s] - not enough space avilable\n", volume->signature));
		__nc_errno = ENOSPC;
		
		return NULL;
	}

	/*
	 * path_lock_ref()를 통해서 증가한 refcnt는 close까지 유지
	 */

	fio_osi_init(&osi, volume, cachepath, originpath, mode, NULL, list);
	nvm_path_lock(volume, osi.pathlock, __FILE__, __LINE__);
	inode = dm_apc_open_inode(volume, cachepath, originpath, mode, list, osi.pathlock, fio_on_complete_apc_open,  &osi);

	if (inode) {
		fh = fio_make_fhandle(inode, osi.pathlock, mode, list);
		TRACE((T_INODE, "file[%p]/INODE[%u] - opened\n", fh, fh->inode->uid));
		if (inode) {
		}
	}
	else {
		/*
		 * 객체가 존재하지 않거나, 에러인 경우
		 */
		if (nc_errno() == EWOULDBLOCK) {
			fio_osi_wait(&osi);
			fh = osi.fh;
			__nc_errno = osi.error;
			/*
			 * wait중에 에러가 발생하면 lock resource unref는 해당 모듈에서처리
			 */
		}

	}
	nvm_path_unlock(volume, osi.pathlock, __FILE__, __LINE__);
	fio_osi_destroy(&osi, __FILE__, __LINE__);

	
	return fh;
}

int
fio_getattr(nc_volume_context_t *volume, char *originpath, nc_stat_t *s)
{
	int 				r;
	nc_hit_status_t 	hs;
	nc_path_lock_t *pl = NULL;
#ifdef __PROFILE
	perf_val_t	ms, me;
	long long	ud;
	char 		perfbuf[1024];
	char 		hinfo[1024];
#endif

	DO_PROFILE_USEC(ms, me, ud) {

		pl = nvm_path_lock_ref(volume, originpath, __FILE__, __LINE__); 
		nvm_path_lock(volume, pl, __FILE__, __LINE__);

		r = dm_inode_stat(volume, originpath, originpath, s, NULL, NULL, NULL, 0, &hs, U_TRUE);

		nvm_path_unlock(volume, pl, __FILE__, __LINE__);
		nvm_path_lock_unref(volume, pl, __FILE__, __LINE__); 
	}
#ifdef  __PROFILE
	snprintf(perfbuf, sizeof(perfbuf)-3, "VOLUME[%s]:fio_getattr{%s) = %d\n",
		volume->signature, hz_string(hinfo, sizeof(hinfo), originpath), r);
	CHECK_WRAP_TIME(ms, me, 10000.0, perfbuf);
#endif

	return r;
}
int
fio_fgetattr(fc_file_t *fh, nc_stat_t *s)
{
	dm_copy_stat(s, fh->inode, U_FALSE);
	return 0;
}
#if 0
fc_file_t *
fio_create(nc_volume_context_t *volume, const char *path, nc_mode_t mode, nc_xtra_options_t *list)
{
	fc_file_t 	*fh = NULL;
	nc_path_lock_t *pl = NULL;
	fc_inode_t 	*inode;

	if (!nvm_space_avail(volume, path)) {
		TRACE((T_WARN, "VOLUME[%s] - not enough space avilable\n", volume->signature));
		__nc_errno = ENOSPC;
		return NULL;
	}

#ifdef PATHLOCK_USE_RWLOCK
	pl = nvm_path_lock(volume, path, U_FALSE);
#else
	pl = nvm_path_lock(volume, path, __FILE__, __LINE__);
#endif
		inode = dm_create(volume, path, mode|O_CREAT);
		if (inode) {
			fh = XMALLOC(sizeof(fc_file_t), AI_ETC);
			fh->magic = FIO_MAGIC;
			fh->volume = volume;
			fh->inode = inode;
			fh->mode = mode;
			fh->list = list;
	
		}
	nvm_path_unlock(volume, pl, __FILE__, __LINE__);

	return fh;
}
#endif
int
fio_close(fc_file_t *fh, int force)
{
	int 			r, wri = 0;
	int				iid;
	nc_volume_context_t	*volume;

	
	DEBUG_ASSERT(fh->magic == FIO_MAGIC); 
	DEBUG_ASSERT(dm_inode_check_owned(fh->inode) == 0);
	DEBUG_ASSERT_FILE(fh->inode, (nvm_path_lock_is_for(fh->lock, fh->inode->q_path)));
	DEBUG_ASSERT_PATHLOCK(fh->lock, (nvm_path_lock_ref_count(fh->lock) > 0));
	DEBUG_ASSERT(fh->inode->PL== fh->lock); 

	if (fh->magic != FIO_MAGIC) {
		TRACE((T_ERROR, "%p : redundant close operation\n", fh));
		__nc_errno = EBADF;
		return -EBADF;
	}
	iid			= fh->inode->uid;
	volume		= fh->volume;
	wri			= fio_is_writable(fh->mode);


	nvm_path_lock(fh->volume, fh->lock, __FILE__, __LINE__);

	fh->magic = 0; /* 이중 close 감지 */

	r = dm_close_inode(fh->inode, wri, force); /* close 이후엔 inode의 구조체 상태가 free되었을 개연성있으므로 디버그 출력 제거 */

	nvm_path_unlock(fh->volume, fh->lock, __FILE__, __LINE__);

	DEBUG_ASSERT(dm_inode_check_owned(fh->inode) == 0);

	if (r != -EWOULDBLOCK) {
		/* dm_close_inode() 가 동기식으로 실행완료됨 */
		VOLUME_UNREF(volume); /* fio_make_handle()때 증가한 volume refcnt 1감소 */
	}

	TRACE((0, "(INODE[%d], %d) = %d (volume='%s')(VREF=%d)\n", 
								iid, 
								0, 
								r, 
								volume->signature, 
								GET_ATOMIC_VAL(volume->refcnt)
								));

	/*
	 * fio_make_fhandle()에서 ref된 count 1 감소
	 */
	nvm_path_lock_unref_reuse(fh->volume, fh->lock, __FILE__, __LINE__);
	XFREE(fh);
	
	return r;
}

static void 
fio_apc_dummy_complete(fc_blk_t *buffer, int xferbytes, int error, void *ud)
{
	// dummy
}
static char ___dummy[1];
int
fio_invoke_io(fc_file_t *fh)
{

	/*
	 * ___dummy[]는 global var임
	 * 아래 함수는 호출후 리턴이므로 auto var이면
	 * return순간 메모리 날라감
	 */
	blk_read_apc(fh->inode, ___dummy, 0, 1, fh->list, fio_apc_dummy_complete, NULL);

	return 0;
}

/*
 * remark!)
 * 	이 함수는 dm_run_waiq() 실행 thread에서
 *	호출된다
 */
static void 
fio_apc_read_complete(fc_blk_t *blk/*used*/, int xferbytes, int error, void *cbdata)
{
	blk_apc_read_info_t 	*ri = (blk_apc_read_info_t *)cbdata;

	TRACE((T_INODE|T_BLOCK, "INODE[%u]/R[%u] - RI[%p] complete, xfered[%d], error[%d] signalling\n", 
							ri->fhandle->inode->uid, 
							ri->fhandle->inode->refcnt, 
							ri, 
							xferbytes,
							error));
	_nl_lock(&ri->hlock, __FILE__, __LINE__);
	ri->error 		= error;
	ri->block 		= blk;
	ri->completed 	= 1;

#ifdef NC_PASS_BLOCK_WQ
	if (blk)  {
		/*
		 * ri_wait()호출 후 대기 중엔 thread
		 * 에 block을 전달하기 전에 refcnt를 증가시켜둬야
		 * reclaim되는 상황을 방지할 수 있음
		 */
		blk_make_ref_nolock(blk);
	}
#endif
	_nl_cond_signal(&ri->hsignal);

	_nl_unlock(&ri->hlock);
}

nc_ssize_t
fio_read(fc_file_t *fi, char *buf, nc_off_t off, nc_size_t len)
{
	int 					nread = 0;
	blk_apc_read_info_t 	*ri = NULL;
	int						remained = len;
	int						tread = 0;
#ifdef __PROFILE
	perf_val_t				s, d;
	perf_val_t				st, dt;
	double					ud;
#endif
	int						nw = 0;
	int						timeout = 0;
	long					to_wait = fi->volume->to_ncread*1000;
#ifndef NC_RELEASE_BUILD
	nc_time_t				t_start = nc_cached_clock(), t_end = 0;
#endif
	nc_ssize_t				xb = 0;


	PROFILE_CHECK(st);
	ri = blk_ri_init(NULL, fi, off, buf, len);
	TRACE((T_INODE, ">>>>>>>>>>>>*INODE[%u]/R[%u] - RI[%p] init, file.offset=%lld, length=%d, timeout=%ld\n",
					(nc_uint32_t)ri->fhandle->inode->uid, 
					(int)ri->fhandle->inode->refcnt, 
					(void *)ri, 
					(nc_uint64_t)ri->foffset,
					(int)ri->length,
					(long)to_wait));

	while (remained > 0 && to_wait > 0) {
		__nc_errno = 0;

		PROFILE_CHECK(s);
		blk_ri_reuse(ri);
		DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);
		nread = blk_read_apc(ri->fhandle->inode, ri->buffer+ri->boffset, ri->foffset, remained, fi->list, fio_apc_read_complete, ri);
		DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);

		if (nread > 0) {
			//ri->boffset		+= nread;
			//ri->xferbytes	+= nread;
			//ri->foffset		+= nread;
			remained		= ri->length- ri->xferbytes;

			if (remained == 0) {
				PROFILE_CHECK(d);
				ud = (double)(PROFILE_GAP_USEC(s, d)/1000.0);
				mavg_update(g__rapc, ud);
				continue;
			}

		}
		PROFILE_CHECK(d);
		ud = (double)(PROFILE_GAP_USEC(s, d)/1000.0);
		mavg_update(g__rapc, ud);
		if (__nc_errno == EWOULDBLOCK) {

			/* 
			 * IO가 background 실행으로 스케줄링됨
			 * IO가 완료될 떄까지 대기
			 */
			TRACE((T_INODE, "INODE[%u]/R[%u] - RI[%p] prepared,waiting!(%ld msecs)\n",
								ri->fhandle->inode->uid, 
								ri->fhandle->inode->refcnt, 
								ri,
								to_wait
					  ));
			PROFILE_CHECK(s);

			if (blk_ri_wait(ri, &to_wait) < 0) {
				/*
				 * 대기 중 timeout 발생
				 */
				INODE_LOCK(ri->fhandle->inode);
				if (ri->rctx) {
					/*
					 * 아직 run_waiq에서 rctx가 destroy되지 않음
					 */
					dm_cancel_waitq(ri->fhandle->inode, ri->rctx);
				}
				INODE_UNLOCK(ri->fhandle->inode);
				//
				// blk_ri_wait()가 timeout처리됨
				// 반드시 nc_errno=ETIMEDOUT이 넘어가야함
				// tread는 > 0 일 수 있음
				timeout = 1;
				TRACE((T_INODE, "INODE[%u]/R[%u] - RI[%p] timeout!(IO req canceled)\n",
								ri->fhandle->inode->uid, 
								ri->fhandle->inode->refcnt, 
								ri
					  ));
				break;
			}
			nw++;

			/*
			 * 아래 코드는 NC_PASS_BLOCK_WQ가 정의된경우에만 실행
			 */
			if (ri->block) {
				/*
				 * block already refered in run_waitq
				 */
				nread = 0;

				DEBUG_ASSERT (BLK_NO(ri->foffset, 0) == ri->block->blkno); 

				DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);
				nread = blk_read_bytes(ri->fhandle->inode, ri->block, ri->foffset, ri->buffer + ri->boffset, remained);
				DEBUG_ASSERT(dm_inode_check_owned(fi->inode) == 0);

				blk_make_unref_nolock(ri->block);
	
				ri->boffset 	+= nread;
				ri->foffset 	+= nread;
				ri->xferbytes	+= nread;
				tread			+= nread;
				remained		-= nread;

				TRACE((T_INODE, "INODE[%u]/R[%u] - RI[%p] signalled, block[%p]/blk#%d, %d bytes copied(xfered=%d) NEXT(foffset=%lld, boffset=%d, remained=%d)\n", 
								ri->fhandle->inode->uid, 
								ri->fhandle->inode->refcnt, 
								ri, 
								ri->block,
								(ri->block?ri->block->blkno:-1),
								nread,
								ri->xferbytes,
								ri->foffset,
								ri->boffset,
								remained));



				if (remained == 0)  {
					__nc_errno = 0; /* reset error code */
					PROFILE_CHECK(d);
					ud = (double)(PROFILE_GAP_USEC(s, d)/1000.0);
					mavg_update(g__wapc, ud);
					break;
				}
			}
			PROFILE_CHECK(d);
			ud = (double)(PROFILE_GAP_USEC(s, d)/1000.0);
			mavg_update(g__wapc, ud);

		}
		else {
			if (tread == 0) tread =  - __nc_errno;
			break;
		}
		TRACE((T_INODE, "INODE[%u]/R[%u] - xfered[%d] remained[%d], to_wait[%ld]\n",
						ri->fhandle->inode->uid, 
						ri->fhandle->inode->refcnt, 
						ri->xferbytes,
						remained,
						to_wait));

	}

	/*
	 * return 값 보정
	 */
	if (remained == 0) __nc_errno = 0;

	if (to_wait <= 0) __nc_errno = ETIMEDOUT;

#ifndef NC_RELEASE_BUILD
	t_end = nc_cached_clock();
	if ((t_end - t_start) > fi->volume->to_ncread) {
		TRACE((T_WARN, "*****%s/%s - INODE[%d] elapsed(%d secs/%d sec)\n",
						fi->volume->signature,
						fi->inode->q_path, 
						fi->inode->uid, 
						(t_end - t_start),
						fi->volume->to_ncread
			  ));
	}
#endif
	TRACE((T_INODE, "<<<<<<<<<<<<<<<<*INODE[%u]/R[%u] - offset=%lld, xfered[%d] remained[%d], to_wait[%ld], ncerrno=%d\n",
					ri->fhandle->inode->uid, 
					ri->fhandle->inode->refcnt, 
					off,
					ri->xferbytes,
					remained,
					to_wait,
					__nc_errno));
	xb =  ri->xferbytes;
	blk_ri_destroy(ri);
	PROFILE_CHECK(dt);
	mavg_update(g__readi, (double)(PROFILE_GAP_USEC(st, dt)/1000.0));
	return xb;
}

/*
 *	************** REMARK ****************
 *		- 이 함수의 호출은 inode에 대한 lock  범위 내에서 이루어 짐
 *		  시간지연이 발생하는 operation은 절대 여기서 실행되면 안됨
 * dm_dun_waitq()에서 callback됨
 *		- 정상적인 경우 block buffer가 데이타로 fillup된 후 바로 호출됨
 *
 */
void 
fio_read_apc_blk_prepared(fc_blk_t *blk/*used*/, int xferbytes, int error, void *cbdata)
{
	int						rem		= 0;
	int						nread	= 0;
	blk_apc_read_info_t 	*ri		= (blk_apc_read_info_t *)cbdata;
	char					zri[128];


	DEBUG_ASSERT(ri != NULL);
	DEBUG_ASSERT_FILE(ri->fhandle->inode, INODE_GET_REF(ri->fhandle->inode) > 0);
	TRACE((T_INODE, "Volume[%s]/Key[%s] - INODE[%d] signalled:blk#%d,error=%d,xfered=%d RI:%s\n",
					ri->fhandle->inode->volume->signature,
					ri->fhandle->inode->q_id,
					ri->fhandle->inode->uid,
					(blk?blk->blkno:-1),
					error,
					xferbytes,
					blk_apc_dump_RI(zri, sizeof(zri), ri)
			  ));

	DEBUG_ASSERT(ri->apc_op != APC_NULL); /* free되었는지 체크 */
	/*
	 * EAGAIN은 크기를 알수없는 객체의 network read가 완료된 시점에 
	 * 지정되어 호출됨.
	 * EAGAIN의 경우 blk_read를 다시한번 호출 실행해야함
	 */
	if (error && error != EAGAIN) {

		TRACE((T_INODE, "Volume[%s]/Key[%s] - INODE[%d] got error %d\n", 
						ri->fhandle->inode->volume->signature,
						ri->fhandle->inode->q_id,
						ri->fhandle->inode->uid,
						error
				  ));

		(*ri->callback)(ri->fhandle, ri->xferbytes, error, ri->cbdata);
		blk_ri_destroy(ri);
		_ATOMIC_SUB(g__pending_io, 1);	
		return;
	}
	/*
	 * callback 함수의 blk가 실제 값임
	 */
	if (ri->block) {
		/*
		 * ri->block은 데이타로 채워져 있는 상태이고
		 * dm_run_waitq()에서 이미 대상 블럭의 refcnt는 1증가해서
		 * reclaim될 일도 없음
		 */
#ifndef NC_RELEASE_BUILD
		{
			nc_blkno_t blkno;
			blkno = BLK_NO(ri->foffset, 0);
			DEBUG_ASSERT(ri->__inuse == 1);
			DEBUG_ASSERT_BLOCK3(ri->fhandle->inode, ri->block, blkno, BLOCK_VALID_TO_READ(ri->fhandle->inode, ri->block, blkno));
			DEBUG_ASSERT_BLOCK(ri->block, blkno == ri->block->blkno); 
		}
#endif
		nread = blk_read_bytes(ri->fhandle->inode, ri->block, ri->foffset, ri->buffer + ri->boffset, ri->length - ri->xferbytes);

		DEBUG_ASSERT_BLOCK(ri->block, nread > 0);

		blk_make_unref_nolock(ri->block);
	
		ri->foffset		+= nread;
		ri->boffset		+= nread;
		ri->xferbytes	+= nread;
		TRACE((T_INODE, "INODE[%u]/R[%u] - %d bytes copied, RI:%s\n",
						ri->fhandle->inode->uid, 
						ri->fhandle->inode->refcnt, 
						nread,
						blk_apc_dump_RI(zri, sizeof(zri), ri)
			  ));
	}

	rem = ri->length - ri->xferbytes;
	DEBUG_ASSERT(rem >= 0);


	__nc_errno = 0;


	if (rem > 0) {
		perf_val_t		now;
		PROFILE_CHECK(now);
		if (PROFILE_GAP_MSEC(ri->start, now) > (ri->fhandle->volume->to_ncread*1000.0)) {
			goto L_RI_finish;
		}
		else {
			blk_ri_reuse(ri);
			nread = blk_read_apc(ri->fhandle->inode, ri->buffer + ri->boffset, ri->foffset, rem, ri->fhandle->list, fio_read_apc_blk_prepared, ri);
	
			if (__nc_errno != EWOULDBLOCK) {
			//	if (nread > 0) {
			//		ri->foffset		+= nread;
			//		ri->boffset		+= nread;
			//		ri->xferbytes	+= nread;
			//	}
				TRACE((T_INODE, "Volume[%s]/Key[%s] - INODE[%d] finished(nread=%d):{%s}\n",
								ri->fhandle->inode->volume->signature,
								ri->fhandle->inode->q_id,
								ri->fhandle->inode->uid,
								nread,
								blk_apc_dump_RI(zri,sizeof(zri), ri)
						  ));
				(*ri->callback)(ri->fhandle, ri->xferbytes, __nc_errno, ri->cbdata);
				blk_ri_destroy(ri);
				_ATOMIC_SUB(g__pending_io, 1);	
			}
		}
	}
	else {
L_RI_finish:
		ri->rctx = NULL;
		/* finished */
		DEBUG_ASSERT_FILE(ri->fhandle->inode, INODE_GET_REF(ri->fhandle->inode) > 0);
		TRACE((T_INODE, "Volume[%s]/Key[%s] - INODE[%d] finished(rem <=0):xfered=%d, error=%d\n",
						ri->fhandle->inode->volume->signature,
						ri->fhandle->inode->q_id,
						ri->fhandle->inode->uid,
						ri->xferbytes,
						error
				  ));
		(*ri->callback)(ri->fhandle, ri->xferbytes, __nc_errno, ri->cbdata);
		blk_ri_destroy(ri);
		_ATOMIC_SUB(g__pending_io, 1);	
	}

}
static void
fio_read_apc_timeout(void *ud)
{
	blk_apc_read_info_t		*pri  = (blk_apc_read_info_t *)ud;
	fc_inode_t				*inode = NULL;
	
	if (!pri->fhandle)	 {
#ifndef NC_RELEASE_BUILD
		TRACE((T_WARN, "FILE[%p]- RI[%p] TIMEOUT(already done)\n", pri->fhandle, ud));
#endif
		return;
	}

	inode = pri->fhandle->inode;


	INODE_LOCK(inode);
	if (!pri->completed) {
		if (pri->rctx) {
			TRACE((0, "VOLUME[%s]/KEY[%s] - INODE[%d] TIMEOUT(RI[%u](%lld, %d), xfered=%d)\n",
							pri->fhandle->volume->signature,
							pri->fhandle->inode->q_id,
							pri->fhandle->inode->uid,
							pri->seq,
							pri->foffset,
							pri->length,
							pri->xferbytes
				  ));
			dm_cancel_waitq(pri->fhandle->inode, pri->rctx);
			fio_read_apc_blk_prepared(NULL/*used*/, 0, ETIMEDOUT, ud);
		}
	}
	INODE_UNLOCK(inode);
	DEBUG_ASSERT(dm_inode_check_owned(inode) == 0);
}

nc_ssize_t
fio_read_apc(fc_file_t *fi, char *buf, nc_off_t off, nc_size_t len, nc_apc_read_callback_t callback, void *callbackdata)
{
	nc_ssize_t 	s;

	blk_apc_read_info_t 	*pri; 
	

#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(fi != NULL);
	DEBUG_ASSERT(fi->inode != NULL);
	DEBUG_ASSERT(fi->magic == FIO_MAGIC);
	DEBUG_ASSERT(kv_valid(fi->list));
#endif
	pri = blk_ri_init(NULL, fi, off, buf, len);
	pri->callback	= callback;
	pri->cbdata		= callbackdata;
	bt_set_timer(__timer_wheel_base,  &pri->t_read_timer, fi->volume->to_ncread*1000, fio_read_apc_timeout, (void *)pri);


	s = blk_read_apc(fi->inode, buf, off, len, fi->list, fio_read_apc_blk_prepared, pri);

	if (__nc_errno != EWOULDBLOCK) {
		/*
		 * sync operation finished
		 */
		TRACE((0, "INODE[%d] - RI[%p] timer removed 'cause of sync OP\n", fi->inode->uid, pri));

		blk_ri_destroy(pri);
	}
#ifndef NC_RELEASE_BUILD
	else {
		TRACE((0, "FILE[%p] - RI[%p] scheduled\n", fi, pri));
	}
#endif
	
	return s;
}
nc_ssize_t  
fio_write(fc_file_t *fi, char *buf, nc_off_t off, size_t len)
{
#if 0
	nc_ssize_t 	s;

	if (fi->inode) {
		s = blk_write(fi->inode, buf, off, len, fi->list);
	}
	return s;
#else
	return 0;
#endif
}
#if 0
nc_ssize_t
fio_write_stream(fc_file_t *fi, char *buf, size_t len)
{
	nc_ssize_t 	s;
	DEBUG_ASSERT(fi->stream != NULL);
	DEBUG_ASSERT(fi->stream->session != NULL);
	if (fi->stream->property_injected == 0) {
		s = cfs_write_stream(fi->stream->session, buf, len, fi->list);
		fi->stream->property_injected++;
	}
	else {
		s = cfs_write_stream(fi->stream->session, buf, len, NULL);
	}

	return s;
}
#endif
int 
fio_flush(nc_file_handle_t *fi)
{
#if 0
	int 	r;
	r = blk_schedule_writeback(fi->inode, U_TRUE, fi->list);
	return r;
#else
	return 0;
#endif
}
int
fio_ftruncate(fc_file_t *fi, nc_size_t len)
{
#if 0
	return dm_ftruncate(fi->inode, len);
#else
	__nc_errno = ENOSYS;
	return -ENOSYS;
#endif
}
int
fio_for_each_property(fc_file_t *fi, int (*do_it)(char *key, char *val, void *cb), void *cb)
{
	int 	r;
	


	INODE_LOCK(fi->inode);

	r = dm_inode_for_each_property(fi->inode, do_it, cb);
	INODE_UNLOCK(fi->inode);

	return r;


}
int
fio_result_code(fc_file_t *fi)
{
	if (!fi->inode->property)
		return -1;

	return kv_get_raw_code(fi->inode->property);
}
char * 
fio_find_property(nc_file_handle_t *fi, char *tag)
{
	if (!fi->inode->property) return NULL;

	return (char *)kv_find_val(fi->inode->property, tag, U_FALSE);
}

fc_file_t *
fio_make_fhandle(fc_inode_t *inode, nc_path_lock_t *pl, nc_mode_t mode, nc_xtra_options_t *req_prop)
{
	fc_file_t 	*fh = NULL;

	DEBUG_ASSERT((req_prop == NULL) || (req_prop && kv_valid(req_prop)));
	DEBUG_ASSERT(nvm_path_lock_is_for(pl, inode->q_path));
	DEBUG_ASSERT_PATHLOCK(pl, nvm_path_lock_ref_count(pl) > 0);
	DEBUG_ASSERT(nvm_path_lock_owned(inode->volume, pl));

	fh = XMALLOC(sizeof(fc_file_t), AI_ETC);
	fh->magic 	= FIO_MAGIC;
	fh->volume 	= inode->volume;
	fh->inode 	= inode;
	fh->lock 	= pl;
	fh->mode 	= inode->imode;
	fh->list 	= req_prop;

	nvm_path_lock_ref_reuse(inode->volume, pl, __FILE__, __LINE__);

	/*
	 * update last access time 
	 */
	INODE_REF(inode);

	if (ic_is_busy(inode)) 
		ic_set_busy(inode, U_FALSE, __FILE__, __LINE__);

	DM_UPDATE_METAINFO(inode, inode->atime = nc_cached_clock());

	VOLUME_REF(fh->volume);

	/*
	 * 동적으로 변하는 컨텐츠 때문에 실행
	 */
	if (!inode->ob_rangeable && !fio_is_writable(mode)) {
		/*
		 * range가 가능한 객체는 static으로 봐야한다
		 */
		if (req_prop && req_prop->opaque_command && strcasecmp(req_prop->opaque_command, "POST") == 0) {
			fio_invoke_io(fh);
			TRACE((T_INODE, "INODE[%u]/O[%s] - not rangeable, trying to get the real object\n", inode->uid, inode->q_path));
		}

	}
	else {
		TRACE((T_INODE, "INODE[%u]/O[%s] - rangeable[%d] or writable[%d]\n", inode->uid, inode->q_path, inode->ob_rangeable, fio_is_writable(mode)));
	}
	return fh;
}
/*
 * OUT: inode, stat,errno
 */


static void 
fio_on_complete_apc_open(nc_file_handle_t *fhandle, nc_stat_t *stat, int error, void *callback_data)
{
	struct fio_open_sync_info	*osi = (struct fio_open_sync_info*)callback_data;
	nc_volume_context_t			*volume = osi->volume;

	TRACE((T_INODE, "Volume[%s].CKey[%s]- OSI[%p] got a completion event, signalling;fhandle=%p,stat=%p,errno=%d,origincode=%d\n", 
							osi->volume->signature,
							osi->key,
							osi, 
							fhandle, 
							stat,
							error,
							(stat?stat->st_origincode:-1)));

	if (osi->user_callback) {
#ifndef NC_RELEASE_BUILD
		/* 김현호 차장의 user_data corrupt주장에 대한 검증용 */
		DEBUG_ASSERT(osi->user_data == osi->user_data_back);
#endif
		TRACE((T_API|T_API_OPEN, "('%s', '%s', '%s', 0x%08X, %p, %p, %p, %p) = INODE[%d].R[%d](nc_errno=%d)(VREF=%d)\n", 
							(volume?volume->signature:"NULL"), 
							osi->key, 
							osi->path, 
							osi->mode, 
							osi->stat, 
							osi->prop, 
							osi->user_callback,
							osi->user_data,
							(fhandle?fhandle->inode->uid:-1), 
							(fhandle?INODE_GET_REF(fhandle->inode):-1),
							__nc_errno,
							GET_ATOMIC_VAL(volume->refcnt)-1
							));
		(*osi->user_callback)(fhandle, stat, error, osi->user_data);
		/*
		 * fhandle == NULL 인 경우에만 pathlock unref함
		 */
		fio_osi_destroy(osi, __FILE__, __LINE__);
		XFREE(osi); /*user_callback != NULL인 경우엔 heap alloc임*/
		//VOLUME_UNREF(volume); /* 비동기 요청으로 전환될 때 1증가시켜둔 refcnt를 다시 1 감소 */
		_ATOMIC_SUB(g__pending_open, 1);
	}
	else {
		osi->fh 		= fhandle;
		osi->error 		= error;
		__nc_errno		= error;

		if (stat && osi->stat) {
			memcpy(osi->stat, stat, sizeof(nc_stat_t));
		}
		osi->completed = 1;
		pthread_cond_signal(&osi->hsignal);
	}
}
/*
 * nc_open_extended_apc에서 호출
 */
fc_file_t *
fio_open_extended_apc_wrap(	nc_volume_context_t 			*volume, 
							char 							*cachepath, 
							char 							*originpath, 
							nc_mode_t 						mode,
							nc_stat_t 						*ns, 
							nc_xtra_options_t 				*req_prop, 
							nc_apc_open_callback_t 			proc, 
							void 							*userdata)

{

	nc_file_handle_t			*fh;
	fio_osi_t 					*osi = XMALLOC(sizeof(fio_osi_t), AI_ETC);
	nc_path_lock_t				*pl = NULL;

	/*
	 * remark:: osi가 heap에서 할당됨
	 */
	fio_osi_init(osi, volume, cachepath, originpath, mode, ns, req_prop);
	osi->user_callback	= proc;
	osi->user_data		= userdata;
#ifndef NC_RELEASE_BUILD
	/* 김현호 차장의 user_data corrupt주장에 대한 검증용 */
	osi->user_data_back	= userdata;
#endif

	/*
	 *
	 * @ wired nvm_path_lock_ref_reuse() by weon@solbox.com(2021.2.21)
	 *
	 * 다른 thread에서 fio_osi_destroy()가 호출되면서
	 * path_lock_unref()가  path_unlock() 호출보다 먼저일어날 수 있음
	 * 이럴경우  nvm_path_unlock()은 의미불명의 unlock()을 실행하게됨
	 * 이를 방지하기위해서 ref()/unref()함수를 한번 더 호출해둠
	 */

	pl = osi->pathlock; /* keep for later unrefer */
	nvm_path_lock_ref_reuse(volume, osi->pathlock, __FILE__, __LINE__);

	nvm_path_lock(volume, osi->pathlock, __FILE__, __LINE__);

	fh  = fio_open_extended_apc_internal(volume, cachepath, originpath, mode, ns, req_prop, fio_on_complete_apc_open, osi);


	/*
	 * wouldblock이 아닌 NULL return의 경우
	 * 여기서 모두 cleanup 함
	 */
	if (__nc_errno != EWOULDBLOCK) {
		/*
		 * 동기식 처리로 끝남(error여부와 상관없이)
		 */
		fio_osi_destroy(osi, __FILE__, __LINE__);
		XFREE(osi);
	}

	nvm_path_unlock(volume, pl, __FILE__, __LINE__);
	nvm_path_lock_unref_reuse(volume, pl, __FILE__, __LINE__);
	return fh;

}

fc_file_t *
fio_open_extended_apc_internal(	nc_volume_context_t 			*volume, 
						char 							*cachepath, 
						char 							*originpath, 
						nc_mode_t 						mode,
						nc_stat_t 						*ns, 
						nc_xtra_options_t 				*req_prop, 
						nc_apc_open_callback_t 			proc, 
						void 							*userdata)
{

	fc_inode_t 					*inode = NULL;
	fc_file_t 					*fh = NULL;
	fio_osi_t 					* osi = (fio_osi_t *)userdata;
	apc_open_context_t			*oc = NULL;
	int 						busy = 0;
	char 						ebuf[128];


	
#ifndef NC_RELEASE_BUILD
	{
	extern __thread int	t_scheduled;
	t_scheduled = 0;
	}
#endif

#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(kv_valid(req_prop));

	if (req_prop && req_prop->opaque_command && strcasecmp(req_prop->opaque_command, "post") == 0) {
		DEBUG_ASSERT(req_prop->oob_ring != NULL);
	}
#endif
	if (dm_is_writable(mode) && !nvm_space_avail(volume, originpath)) {
		TRACE((T_WARN, "VOLUME[%s] - not enough space avilable\n", volume->signature));
		__nc_errno = ENOSPC;
		
		return NULL;
	}

	__nc_errno = 0;



	busy = nvm_path_busy_nolock(osi->pathlock);
	if (busy) {
		/*
		 * busy
		 */
		
		oc = dm_apc_prepare_context_raw(	volume, 
											NULL, 
											originpath, 
											cachepath, 
											cachepath, 
											mode, 
											APC_OS_WAIT_FOR_COMPLETION, 
											req_prop, 
											osi->pathlock, 
											0, 
											proc, 
											userdata);

		TRACE((T_INODE,"Volume[%s].CKey[%s] - O-CTX[%p] waiting(already in progress)\n", volume->signature, cachepath, oc));
		nvm_path_put_wait(osi->pathlock, oc, __FILE__, __LINE__);
		__nc_errno = EWOULDBLOCK;
		inode = NULL;
		VOLUME_REF(volume);
	}
	else {
		DEBUG_ASSERT(req_prop == NULL || kv_valid(req_prop));
		/*
		 * 비동기 요청 invoke시에 path에 inprogres설정
		 */
		inode = dm_apc_open_inode(volume, cachepath, originpath, mode, req_prop, osi->pathlock, proc,  userdata);
		if (__nc_errno == EWOULDBLOCK)
			TRACE((T_INODE,"Volume[%s].CKey[%s] - O-CTX[%p] return EWOULDBLOCK\n", volume->signature, cachepath, oc));
	}



	if (inode) {
		
		/*
		 * 캐싱되어있는 inode 확보
		 */
		fh = fio_make_fhandle(inode, osi->pathlock, mode, req_prop);
		if (ns) dm_copy_stat(ns, inode, U_FALSE);

	}
	else {
		/*
		 * 
		 * path-lock 확보된 상태
		 * error값이 remote error아니면 wouldblock
		 */


		if (__nc_errno != EWOULDBLOCK) {
			TRACE((T_WARN|T_INODE, "VOLUME[%s]/Object[%s]/OSI[%p] got error(nc_errno=%d)\n", volume->signature, originpath, osi, __nc_errno));
		}

		TRACE((T_INODE, "VOLUME[%s]/Object[%s] got error(nc_errno=%d(%s), origin_code=%d)\n", 
				volume->signature, originpath, __nc_errno, strerror_r(__nc_errno, ebuf,sizeof(ebuf)), ns->st_origincode));
	}
	
	return fh;
}
/*
 * fio_osi_init()에서 할당된 pathlock은 refcnt를 증가시켜서 
 * 리턴해둠.
 * 이렇게 함으로써 close까지 대상 pathlock은 GLRU에서 다른 path에 재할당되지않음
 */
fc_file_t *
fio_open_extended(nc_volume_context_t *volume, char *cachepath, char *originpath, nc_mode_t mode, nc_stat_t *ns, nc_xtra_options_t *req_prop)
{
	fc_file_t 	*fh = NULL;
	fio_osi_t 	osi; 


	fio_osi_init(&osi, volume, cachepath, originpath, mode, ns, req_prop);
	nvm_path_lock(volume, osi.pathlock, __FILE__, __LINE__);

	fh  = fio_open_extended_apc_internal(volume, cachepath, originpath, mode, ns, req_prop, fio_on_complete_apc_open, &osi);

	/*
	 * async open in progress
	 */
	if (fh == NULL && __nc_errno == EWOULDBLOCK) {
		fio_osi_wait(&osi);
		fh = osi.fh;
		__nc_errno = osi.error;
		TRACE((T_INODE, "Volume[%s].CKey[%s] - handle[%p], nc_errno=%d, (st_origin=%d)\n",
						volume, cachepath, fh, __nc_errno, (ns?ns->st_origincode:-1)));
	}
	nvm_path_unlock(volume, osi.pathlock, __FILE__, __LINE__);
	fio_osi_destroy(&osi, __FILE__, __LINE__);
	return fh;
}
int
fio_close_allowable(fc_file_t *fh)
{
	return 0;
}
