#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <getopt.h>


#ifndef WIN32
#include <error.h>
#endif
#include <fcntl.h>
#include <iconv.h>
#ifndef WIN32
#include <langinfo.h>
#endif
#ifndef WIN32
#include <libintl.h>
#endif
#include <limits.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>

#ifdef NEON_MEMLEAK
#include <memleak.h>
#endif
#include <pthread.h>
#include <util.h>
#include <netcache.h>
#include <block.h>
#include <bitmap.h>
#include <disk_io.h>

#include "trace.h"
#include "hash.h"




char 	__cache_dir[256]="/stg/weon_cache";
int 	__repair = 0;
int 	__scanned = 0;
int 	__enable_compaction = 1;
static int nck_check_file(const char *fpath, int __repair);
static int nck_verify_file(char *cpath, char *msg, int repair, int *needflush);
//FILE 	*__output_file = stderr;
int PUBLIC_IF dm_count_valid_blocks(unsigned long *bitmap, unsigned long *ebitmap, int bitlen);
void nck_free_inode(fc_file_t *inode)	;
void nck_flush_inode(fc_file_t *inode);
fc_file_t 	__tinode;
int		__thread_count = 1;


//char	*__t_block_buf = NULL;
int		 __block_size = 1024*256;

pthread_mutex_t __msg_lock = PTHREAD_MUTEX_INITIALIZER;
void
msg_out(const char *fmt, ...)
{
	va_list 		ap;

	pthread_mutex_lock(&__msg_lock);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	pthread_mutex_unlock(&__msg_lock);
}
static int 
nck_scandir(int i, int j)
{
	DIR			*dh;
	char		dpath[NC_MAXPATHLEN];
	char		fpath[NC_MAXPATHLEN];
	char 		ebuf[128];
	struct		dirent *fh;
	int 		oh_notused;
	int			cnt = 0;
	fc_file_t 	*f, *lookup;
	long		sleepcnt = 0;
	int			dcnt = 0;
#ifndef WIN32
	struct dirent	l_de;
	struct dirent	*dir_cursor = NULL;
#endif

	snprintf(dpath, sizeof(dpath), "%s/%02X/%02X", __cache_dir, i, j);
	TRACE((T_DEBUG, "scanning directory, '%s'\n", dpath));
	dh = opendir(dpath);
	if (dh == NULL) {
		return cnt;
	}
#ifdef WIN32
	fh = readdir(dh);
	while (fh != NULL) 
#else
	fh = &l_de;
	dir_cursor = NULL;
	readdir_r(dh, fh, &dir_cursor);
	while (dir_cursor != NULL) 
#endif
	{
		/* skip built-in entries, '.', '..' */
		if ((strcmp(fh->d_name, ".") == 0) || (strcmp(fh->d_name, "..") == 0)) {
			goto L_next_entry;
		}
		TRACE((T_DEBUG, "verifying cache file, '%s'\n", fh->d_name));

		snprintf(fpath, sizeof(fpath), "%s/%s", dpath, fh->d_name);
		nck_check_file(fpath, __repair);
L_next_entry:
#ifdef WIN32
		fh = readdir(dh);
#else
		readdir_r(dh, fh, &dir_cursor);
#endif
		dcnt++;
	}
	if (dh) closedir(dh);
	TRACE((T_DEBUG, "scanning directory '%s' done, %d files\n", dpath, dcnt));
	return cnt;
}
void * PUBLIC_IF nc_aligned_malloc(nc_size_t size, int alignment);
void PUBLIC_IF nc_aligned_free(void *p);
int
nck_move_block(fc_file_t *inode, nc_off_t orgoff, nc_off_t newoff)
{
	char 		*p;
	ssize_t 	readl;
	ssize_t 	writel;
	char 		*buf = NULL;



	buf = nc_aligned_malloc(NC_BLOCK_SIZE, 1024);
	readl = pread(inode->fd, buf, NC_BLOCK_SIZE, orgoff);
	if (readl != NC_BLOCK_SIZE) {
		msg_out("pread - %s(%s)\n", strerror(errno));
		nc_aligned_free(buf);
		return -1;
	}
	writel = pwrite(inode->fd, buf, NC_BLOCK_SIZE, newoff);
	if (writel != NC_BLOCK_SIZE) {
		msg_out("pwrite - %s\n", strerror(errno));
		nc_aligned_free(buf);
		return -1;
	}
	nc_aligned_free(buf);
	return 0;
}
int
nck_remap_blocks(fc_file_t *inode)
{
	int 		i;
	int 		org_pblkno = 0;
	int 		new_pblkno = 0;
	int 		r;
	int 		cnt_l;
	int 		cnt_p;
	int 		new_physical_cursor = 0;

	cnt_l = dm_count_valid_blocks((unsigned long *)inode->bitmap, NULL, inode->bitmaplen);
	cnt_p = dm_count_valid_blocks((unsigned long *)inode->pbitmap, NULL, inode->bitmaplen);
	inode->physical_cursor = 0;
	for (i = 0; i <= inode->maxblkno; i++) {
		if (dm_check_cached_unlock(inode, i)) {
			if (inode->LPmap[i] == NULL_BLKNO) {
				/* marked cached but no data! */
				msg_out( "remap failed\n");
				return -1;
			}
			if (inode->LPmap[i] > (cnt_l)) {
				/* need to remap the block */
				org_pblkno = inode->LPmap[i];
				new_pblkno = lpmap_allocate_physical_block_nolock(inode, i, U_FALSE);
				if (new_pblkno == NULL_BLKNO) {
					msg_out( "CACHE[%s] - blk # %d relocation failed, removing the cache file\n",
						inode->c_path,
						i);
					return -1;
				}
				r = nck_move_block(inode, NC_ORG_BLOCK_OFFSET(org_pblkno), NC_ORG_BLOCK_OFFSET(new_pblkno)); 
				if (r < 0) {
					return r;
				}
				msg_out( "CACHE[%s] - blk # %d relocated to physical blk # %d(from %d)\n",
						inode->c_path, 
						i,
						new_pblkno,
						org_pblkno);
			}
		}
	}
	return 0;
}
int
nck_check_disk_cached(fc_file_t *inode, long blkno)
{
	if (inode->complete) {
		return 1;
	}
	return (test_bit(blkno, inode->bitmap) != 0); 
}
int
nck_mark_uncached(fc_file_t *inode, long blkno)
{
	int Bsize;
	if (inode->complete) {
		/* need create bitmap */
		Bsize = NC_CANNED_BITMAP_SIZE(inode->bitmaplen);
		inode->bitmap = (unsigned long *)XMALLOC(Bsize);
		memset((char *)inode->bitmap, 0xFF, sizeof(Bsize));
		inode->complete = 0;
	}
	clear_bit(blkno, inode->bitmap);
	lpmap_recover_error_nolock(inode, blkno);
	return 0;
}
int
nck_verify_block_internal(fc_file_t *inode, long blkno, char *block_ptr)
{
	nc_blkno_t 		mapped_blkno = 0;
	nc_off_t		offset;
	long			length;
	long			copied;
	unsigned char	tcrc;
	fc_blk_t		tb;

	if (__enable_compaction) {
		mapped_blkno 		= lpmap_get_l2p_nolock(inode, blkno);
	}
	else {
		mapped_blkno 		= blkno;
	}
	offset = NC_ORG_BLOCK_OFFSET(mapped_blkno);
	length = NC_BLOCK_SIZE;

	copied = pread(inode->fd, block_ptr, length, offset);
	if (copied < length) {
		msg_out( "\t * cache '%s' got error in pread(%d) - %s\n", inode->c_path, copied, strerror(errno));
		return -1; /* IO error */
	}

	tb.buffer = block_ptr;
	tb.blkno  = blkno;
	tcrc = blk_make_crc(inode, &tb);
	if (dm_verify_block_crc(inode, blkno, tcrc)) {
		return 0; /* OK */
	}
	TRACE((T_WARN, "cache '%s' blk # %ld got CRC mismatch (0x%02X, 0x%02X)\n",
				inode->c_path,
				blkno,
				(unsigned char)inode->blockcrc[blkno],
				(unsigned char)tcrc));
	return -1; /* CRC error */
}

int
nck_verify_blocks(fc_file_t *inode)
{
	long				blkno = 0;
	int					r;
	int					failed_blocks = 0;
	int					delete_threshold = 0;
	char				*block_buf = NULL;

	delete_threshold = 10;
	block_buf = (char *)nc_aligned_malloc(__block_size, 1024);
	while (blkno <= inode->maxblkno) {
		if (nck_check_disk_cached(inode, blkno)) {
			/* block cached in disk*/
			r = nck_verify_block_internal(inode, blkno, block_buf);
			if (r < 0) {
				/* verify failed */
				failed_blocks++;
				TRACE((T_WARN, "cache, '%s', blk # %ld - block marked uncached\n", inode->c_path, blkno));
				nck_mark_uncached(inode, blkno);
			}
		}
		if (failed_blocks > delete_threshold) {
			TRACE((T_WARN, "cache, '%s', too many CRC error, %d, give up\n", inode->c_path, blkno, failed_blocks));
			nc_aligned_free(block_buf);
			return -1;
		}
		blkno++;
	}
	nc_aligned_free(block_buf);
	return failed_blocks; /* if positive value, need to recover LPmap again */
}



#ifdef WIN32
int
nck_recover_v20(HANDLE fd, const char *cpath, char *msg)
#else
int
nck_recover_v20(int fd, const char *cpath, char *msg)
#endif
{
	int 				bl;
	int 				cnt;
	int 				r, vblk = 0;
	long long 			rsize ;
	struct stat 		s;

	fc_header_info_v20_t 	*hi;
	fc_file_t 			*f = &__tinode;
	int 				nr, br;
	int					vblocks=0;
	int					hisize;
	time_t 				now = time(NULL);
	char				sctime[64];
	char				smtime[64];
	struct tm			ltm;
	char 				ebuf[128];
	int 				Bsize;
	int 				Psize;
	int 				LPsize;
	int					need_delete = 0;

	hi = dm_read_header_v20(fd, cpath);

	if (hi == NULL) {
		sprintf(msg, "version 2.0 meta-info parsing failed, removed");
		TRACE((T_WARN, "CACHE['%s'] : header  corrupt \n", cpath));
		return -1;
	}
	if (hi->temporal) {
		TRACE((T_WARN, "CACHE['%s'] : temporal file, removing \n", cpath));
		sprintf(msg, "temporal file, removed");
		return -1;
	}
	Bsize = NC_CANNED_BITMAP_SIZE(hi->bitmaplen);
	Psize = NC_CANNED_PARITY_SIZE(hi->bitmaplen);
	if (__enable_compaction) {
		LPsize = NC_CANNED_LPMAP_SIZE(hi->bitmaplen);
	}
	else
		LPsize = 0;



	f->hdr_version 	= INODE_HDR_VERSION_V20;
	f->ctime 	= hi->ctime;
	f->mtime 	= hi->mtime;
	f->size 	= hi->size;
	f->temporal	= (0x0001 & hi->temporal);
	f->fd		= fd;
	f->complete = 0;
	f->dirtyblocks  	= 0;
	f->headersynctime  	= 0;
	f->blocksynctime  	= 0;
	f->physical_cursor 	= hi->physical_cursor;
	f->qr_key = XSTRDUP(hi->qr_key);

	f->qr_path = XSTRDUP(hi->qr_path);
	f->c_path = XSTRDUP(cpath);
	f->bitmaplen= NC_BITMAP_LEN(hi->size);
	f->maxblkno = max(0, f->bitmaplen-1);

	cnt = dm_count_valid_blocks((unsigned long *)hi->bitmap, NULL, hi->bitmaplen);
	if (cnt != hi->bitmaplen) {
		f->bitmap		= (unsigned long *)XMALLOC(NC_CANNED_BITMAP_SIZE(hi->bitmaplen)); 
		f->dbitmap		= (unsigned long *)XMALLOC(NC_CANNED_BITMAP_SIZE(hi->bitmaplen)); 
		ASSERT(f->bitmap != NULL);
	}
	else {
		if (cnt == hi->bitmaplen) {
			f->complete 		= 1;
			f->bitmap 		= NULL;
			f->dbitmap 		= NULL;
		}
	}
	f->blockcrc		= (unsigned char *)XMALLOC(NC_CANNED_PARITY_SIZE(f->bitmaplen));
	f->blockmap 	= (unsigned int *)XCALLOC(f->bitmaplen, sizeof(unsigned int));
	if (__enable_compaction) {
		lpmap_prepare_nolock(f);
	}

	if (f->bitmap)
		bitmap_copy(f->bitmap, (unsigned long *)hi->bitmap, hi->bitmaplen);


	memcpy(f->blockcrc, (char *)hi->bitmap +  Bsize, Psize);


	if (__enable_compaction) {
		memcpy(f->LPmap, (char *)hi->bitmap + Psize + Bsize , LPsize);
		r = LPmap_recover_pbitmap_nolock(f);
		TRACE((T_WARN, "cache, '%s' - LPmap result=%d\n", cpath, r));

		switch (r) {
			case 0: /* no problem */
				break;
			case -1: /* error, unrecoverable */
				sprintf(msg, "block recovery failed");
				nck_free_inode(f);
				need_delete++;
				break;
			case 1: /* error, but recoverable */
				if (nck_remap_blocks(f) < 0) {
					need_delete++;
					sprintf(msg, "block remapping failed, recovery failed");
					nck_free_inode(f);
					r = -1;
				}
				else {
					r = 1; /* flush needed */
					f->physical_cursor = LPmap_find_cursor(f);
					rsize = (f->physical_cursor+1)*NC_BLOCK_SIZE;
					ftruncate(f->fd, rsize);
					sprintf(msg, "block remapping done, cursor repositioned to %d", f->physical_cursor);
				}
				break;
		}
	}

	TRACE((T_WARN, "cache '%s' - r=%d, need_delete=%d\n", cpath, r, need_delete));
	if (!need_delete) {
		if ((r = nck_verify_blocks(f)) < 0) {
			sprintf(msg, "block recovery failed, need to be removed");
			need_delete++;
			r = -1;
		}
	}
	if (r == 1) {/* need flush */
		vblk = dm_count_valid_blocks((unsigned long *)f->bitmap, NULL, f->bitmaplen);
		if (vblk > 0) {
			nck_flush_inode(f);
			TRACE((T_WARN, "cache '%s' - %d blocks are recovered\n", cpath, vblk));
			sprintf(msg, "%d blocks recovered", vblk);
		}
		else {
			r = -1; /* need to remove */
			TRACE((T_WARN, "cache '%s' - valid block is 0, need to remove\n", cpath));
			sprintf(msg, "valid block is 0, need to remove", cpath);
		}
	}
	nck_free_inode(f);
	return r;
}

#ifdef WIN32
int
nck_recover_v21(HANDLE fd, const char *cpath, char *msg)
#else
int
nck_recover_v21(int fd, const char *cpath, char *msg)
#endif
{
	int 				bl, vblk;
	int 				cnt;
	int 				r;
	long long 			rsize ;
	struct stat 		s;

	fc_header_info_v21_t 	*hi;
	fc_file_t 			t_inode;// = &__tinode;
	fc_file_t 			*f = &t_inode;
	int 				nr, br;
	int					vblocks=0;
	int					hisize;
	time_t 				now = time(NULL);
	char				sctime[64];
	char				smtime[64];
	struct tm			ltm;
	char 				ebuf[128];
	int 				Bsize;
	int 				Psize;
	int 				LPsize;
	int					need_delete = 0;

	hi = dm_read_header_v21(fd, cpath);

	if (hi == NULL) {
		sprintf(msg, "version 2.1 meta-info parsing failed, removed");
		return -1;
	}
	if (hi->temporal) {
		//TRACE((T_WARN, "CACHE['%s'] : temporal file, removing :%s\n", cpath, nc_lasterror(ebuf, sizeof(ebuf))));
		sprintf(msg, "temporal file, removed");
		return -1;
	}
	Bsize = NC_CANNED_BITMAP_SIZE(hi->bitmaplen);
	Psize = NC_CANNED_PARITY_SIZE(hi->bitmaplen);
	if (__enable_compaction) {
		LPsize = NC_CANNED_LPMAP_SIZE(hi->bitmaplen);
	}
	else
		LPsize = 0;



	f->hdr_version 	= INODE_HDR_VERSION_V21;
	f->ctime 	= hi->ctime;
	f->mtime 	= hi->mtime;
	f->size 	= hi->size;
	f->temporal	= (0x0001 & hi->temporal);
	f->fd		= fd;
	f->complete = 0;
	f->dirtyblocks  	= 0;
	f->headersynctime  	= 0;
	f->blocksynctime  	= 0;
	f->physical_cursor 	= hi->physical_cursor;
	f->qr_key = XSTRDUP(hi->qr_key);

	f->qr_path = XSTRDUP(hi->qr_path);
	f->c_path = XSTRDUP(cpath);
	f->bitmaplen= NC_BITMAP_LEN(hi->size);
	f->maxblkno = max(0, f->bitmaplen-1);

	cnt = dm_count_valid_blocks((unsigned long *)hi->bitmap, NULL, hi->bitmaplen);
	if (cnt != hi->bitmaplen) {
		f->bitmap		= (unsigned long *)XMALLOC(NC_CANNED_BITMAP_SIZE(hi->bitmaplen)); 
		f->dbitmap		= (unsigned long *)XMALLOC(NC_CANNED_BITMAP_SIZE(hi->bitmaplen)); 
		ASSERT(f->bitmap != NULL);
	}
	else {
		if (cnt == hi->bitmaplen) {
			f->complete 		= 1;
			f->bitmap 		= NULL;
			f->dbitmap 		= NULL;
		}
	}
	f->blockcrc		= (unsigned char *)XMALLOC(NC_CANNED_PARITY_SIZE(f->bitmaplen));
	f->blockmap 	= (unsigned int *)XCALLOC(f->bitmaplen, sizeof(unsigned int));
	if (__enable_compaction) {
		lpmap_prepare_nolock(f);
	}

	if (f->bitmap)
		bitmap_copy(f->bitmap, (unsigned long *)hi->bitmap, hi->bitmaplen);


	memcpy(f->blockcrc, (char *)hi->bitmap +  Bsize, Psize);


	if (__enable_compaction) {
		memcpy(f->LPmap, (char *)hi->bitmap + Psize + Bsize , LPsize);
		r = LPmap_recover_pbitmap_nolock(f);

		switch (r) {
			case 0: /* no problem */
				break;
			case -1: /* error, unrecoverable */
				sprintf(msg, "block recovery failed");
				nck_free_inode(f);
				need_delete++;
				break;
			case 1: /* error, but recoverable */
				if (nck_remap_blocks(f) < 0) {
					need_delete++;
					sprintf(msg, "block remapping failed, recovery failed");
					nck_free_inode(f);
					r = -1;
				}
				else {
					r = 1; /* flush needed */
					f->physical_cursor = LPmap_find_cursor(f);
					rsize = (f->physical_cursor+1)*NC_BLOCK_SIZE;
					ftruncate(f->fd, rsize);
					sprintf(msg, "block remapping done, cursor repositioned to %d", f->physical_cursor);
				}
				break;
		}
	}

	if (!need_delete) {
		if (nck_verify_blocks(f) < 0) {
			sprintf(msg, "block recovery failed");
			need_delete++;
			r = -1;
		}
	}
	if (r == 1) {/* need flush */
		vblk = dm_count_valid_blocks((unsigned long *)f->bitmap, NULL, f->bitmaplen);
		if (vblk > 0) {
			nck_flush_inode(f);
			TRACE((T_WARN, "cache '%s' - %d blocks are recovered\n", cpath, vblk));
			sprintf(msg, "%d blocks recovered", vblk);
		}
		else {
			r = -1; /* need to remove */
			TRACE((T_WARN, "cache '%s' - valid block is 0, need to remove\n", cpath));
			sprintf(msg, "valid block is 0, need to remove", cpath);
		}
	}
	nck_free_inode(f);


	return -1;
}

int
nck_verify_file(char *cpath, char *msg, int repair, int *needflush)
{
	int 				bl;
	int 				cnt;
	int 				r;
	long long 			rsize ;
#ifdef WIN32
	HANDLE			fd;
	DWORD			err;
#else
	int 			fd;
#endif
	struct stat 		s;
	int					vblk = 0;

#if 0
	fc_header_info_t 	*hi;
	fc_file_t 			*f = &__tinode;
	int 				nr, br;
	//char				outputbuf[10240];
	int					vblocks=0;
	int					hisize;
	time_t 				now = time(NULL);
	char				sctime[64];
	char				smtime[64];
	struct tm			ltm;
	int 				Bsize;
	int 				Psize;
	int 				LPsize;
#endif
	int					hdr_version = -1;

	*needflush = 0;
	fd = dio_open_file(cpath, 0, U_FALSE, U_FALSE);
	if (!DM_FD_VALID(fd)) {
		return -1;
	}
	hdr_version = dm_read_magic(fd, cpath);

	sprintf(msg, "verified ok");

	switch (hdr_version) {
		case INODE_HDR_VERSION_V20:
			r = nck_recover_v20(fd, cpath, msg);
			if (r == 1) *needflush = 1;
			break;
		case INODE_HDR_VERSION_V21:
			r = nck_recover_v21(fd, cpath, msg);
			if (r == 1) *needflush = 1;
			break;
		default:
			r = -1; /* magic corrupt */
			TRACE((T_WARN, "cache '%s' - header corrupt\n", cpath, msg));
			break;
	}

	TRACE((T_WARN, "nck_recover_v2x - %d\n",r));
	if (r < 0 ) {
		TRACE((T_WARN, "cache '%s' - %s\n", cpath, msg));
		msg_out( "cache '%s' - %s\n", cpath, msg);
		dio_close_file(fd);
		unlink(cpath);
	}
	else {
		msg_out( "cache '%s' - %s\n", cpath, msg);
		dio_close_file(fd);
	}
	return r;
}
void
nck_free_inode(fc_file_t *inode)	
{
	int		r;

	DM_CLOSE_FD(inode);
	inode->orphan 		= 0;
	inode->isolated 		= 0;
	inode->doc			= 0;
	inode->dirtyblocks 	= 0;
	inode->blocksynctime = 0;
	inode->headersynctime = 0;
	inode->nocache 		= 0;
	inode->asio_cnt	= 0;
	inode->ctime 		= 0;
	inode->mtime 		= 0;
	inode->victim		= 0;
	inode->size 		= 0;
	inode->temporal		= 0;
	inode->closesyncing	= 0;

	inode->t_path		= NULL;
	inode->bitmaplen 	= 0;
	if (inode->blockmap) XFREE(inode->blockmap);
	if (inode->blockcrc) XFREE(inode->blockcrc);
	if (inode->bitmap) XFREE(inode->bitmap);
	if (inode->dbitmap) XFREE(inode->dbitmap);
	if (__enable_compaction) 
		lpmap_reset_nolock(inode);

	inode->stat 		= 0;
	inode->c_path[0] 	= 0;
	inode->size 		= 0;

	XFREE(inode->qr_key);
	XFREE(inode->qr_path);
	XFREE(inode->c_path);

}
void
nck_flush_inode(fc_file_t *inode)
{
	int		vblk = 0;
		switch (inode->hdr_version) {
			case INODE_HDR_VERSION_V20:
				vblk = dm_count_valid_blocks((unsigned long *)inode->bitmap, NULL, inode->bitmaplen);
				dm_flush_inode_v20(inode);
				TRACE((T_WARN, "cache, '%s' - finished(%d blocks)\n", inode->c_path, vblk));
				break;
			case INODE_HDR_VERSION_V21:
				vblk = dm_count_valid_blocks((unsigned long *)inode->bitmap, NULL, inode->bitmaplen);
				dm_flush_inode_v21(inode);
				TRACE((T_WARN, "cache, '%s' - finished(%d blocks)\n", inode->c_path, vblk));
				break;
			default:
				break;
		}
}


static int 
nck_check_file(const char *cpath, int repair)
{
	int			r;
	int 		recovered = 0;
	char 		msg[1024] = "";
	long long 	rsize ;

	TRACE((T_DEBUG, "cache, '%s' - checking\n", cpath));
	sprintf(msg, "verified ok");
	r = nck_verify_file(cpath, msg, repair, &recovered);
	_ATOMIC_ADD(__scanned, 1);
//	if (strlen(msg) > 0)
//		fprintf(stderr, "CPATH[%s] - %s\n", cpath, msg);
	return 0;
}
typedef struct {
	int		begin;
	int		end;
} thread_param_t;
#define 	NC_MAX_DIR 	256
void * 
nck_dir_scanner(void *d)
{
	int		i, j;
	int		old_scanned;
	int		t;
	thread_param_t	*param = (thread_param_t *)d;
	for (i = param->begin; i <= param->end ; i++) {
		old_scanned = GET_ATOMIC_VAL(__scanned);
		for (j = 0; j < NC_MAX_DIR; j++) {
			nck_scandir(i, j);
		}
		t = GET_ATOMIC_VAL(__scanned);
		if (old_scanned != t) {
			msg_out( "%d cache file(s) scanned\n", t);
		}
	}
}
int
nck_run_threads(int thr_cnt)
{
	pthread_t		*thr = NULL;
	int				i, r;
	thread_param_t	*param;
	int				unit;

	thr = (pthread_t *)malloc(thr_cnt * sizeof(pthread_t));
	param = (thread_param_t *)malloc(thr_cnt * sizeof(thread_param_t));
	unit = NC_MAX_DIR/thr_cnt;
	for (i = 0; i < thr_cnt; i++) {
		param[i].begin = i* unit;
		param[i].end   = (i+1)*unit -1;

		if (i == thr_cnt-1) {
			/* the last thread */
			param[i].end = NC_MAX_DIR-1;
		}
		r = pthread_create(&thr[i], NULL, nck_dir_scanner, &param[i]);
		if (r != 0) {
			msg_out( "%d-th thread invocation failed\n", i);
			exit(1);
		}
	}
	for (i = 0; i < thr_cnt; i++) {
		pthread_join(thr[i], NULL);
	}

}
main(int argc, char *argv[])
{
	int 		i;
	int 		j;
	int 		old_scanned;
	int 		val;
	int 		debug=0;
	char		*log_file="ncc.log";
	unsigned long logmask;
	pthread_mutexattr_t 	mattr;
	//trc_depth(0xFFFF);
	memset(&__tinode, 0, sizeof(__tinode)); 
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);

	logmask =     0xFFFFF;
 	nc_setup_log(logmask, log_file, 50000, 10);
	TRC_DEPTH((0xFFFFFF));
	while ((val = getopt(argc, argv, "C:t:d")) > 0) {
		switch (val) {
			case 'C':
				strcpy(__cache_dir, optarg);
				break;
			case 't':
				__thread_count = atoi(optarg);
#ifdef WIN32
				if (__thread_count != 1) { 
					msg_out( "thread count is invalid, %d, Win32 platform allows single thread\n", __thread_count);
					exit(1);
				}
#else
				if (__thread_count <= 0 || __thread_count > 10) {
					msg_out( "thread count is invalid, %d\n", __thread_count);
					exit(1);
				}
#endif
				break;
			case 'd':
				debug++;
				break;
			default:
				msg_out( "%s [-C <cache root>] \n", (char *)argv[0]);
				exit(1);
		}
	}
#if 0
	if (debug)
		trc_depth(T_INFO|T_WARN|T_ERROR);
	else
		trc_depth(T_ERROR);
#endif
	//__t_block_buf = nc_aligned_malloc(__block_size, 1024);

	msg_out( "NetCache storage repair tool begins to check the files under path '%s'\n", __cache_dir);
	msg_out( "If there are so many files, it will take long time to complete.\n\n");
	msg_out( "Configured working thread is %d\n", __thread_count);

	pthread_mutex_init(&__tinode.lock, &mattr);

	nck_run_threads(__thread_count);
#if 0
	for (i = 0; i < NC_MAX_DIR ; i++) {
		old_scanned = __scanned;
		for (j = 0; j < NC_MAX_DIR; j++) {
			nck_scandir(i, j);
		}
		if (old_scanned != __scanned) {
			fprintf(stderr, "%d cache file(s) scanned\n", __scanned);
		}
	}
#endif
	msg_out( "*** completed, %d cache file(s) verified\n", __scanned);
}
