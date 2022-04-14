#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <regex.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <setjmp.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"

#include "voxio_def.h"
#include "scx_util.h"
#include "status.h"
#include "scx_glru.h"
#include "streaming.h"
#include "streaming_lru.h"
#include "meta_disk_cache.h"


extern __thread jmp_buf 	__thread_jmp_buf;
extern __thread int		__thread_jmp_working;

typedef struct tag_mdc_path_info {
	char 	filepath[MEDIA_METADATA_CACHE_PATH_MAX_LEN];
	off_t	st_size;    /* total size, in bytes */
	time_t	st_mtime;   /* time of last modification */
} mdc_path_info_t;

List		*scx__mdc_path_list = NULL;	/* media metadata disk cache file 경로를 저장하고 있는 리스트 */
pthread_t 	mdc_monitor_tid;
volatile int 		gscx__metadata_disk_read_interval_cnt = 0;			/* 단위 시간동안 disk에서 read된 media context 수 */
volatile uint64_t 	gscx__metadata_disk_read_interval_size = 0;			/* 단위 시간동안 disk에서 read된 media context의 byte 수*/
volatile int 		gscx__metadata_disk_write_interval_cnt = 0;			/* 단위 시간동안 disk에서 write된 media context 수 */
volatile uint64_t 	gscx__metadata_disk_write_interval_size = 0;		/* 단위 시간동안 disk에서 write된 media context의 byte 수*/
int			gscx__metadata_cache_write_enable = 1;	/* 캐시 파일 기록 여부, 캐시 파티션의 용량이 부족한 경우 이 값이 0으로 설정된다. */

static int mdc_list_compare(const void *left, const void *right, CompareInfo *arg);
static char *mdc_make_cache_path(char *key);
static int mdc_create_cache_dir();
static int64_t mdc_get_free_space();
int mdc_check_cache_dir_usage();
static int mdc_cal_dir_size(uint64_t *totalSize, char *path, int make_list);
static int mdc_clean_remove_dir();
void * mdc_monitor(void *d);

int
mdc_init()
{
	uint64_t totalSize = 0LL;
#if 0
	scx__mdc_path_list = iList.Create(sizeof (mdc_path_info_t *));
#else
	scx__mdc_path_list = iList.CreateWithAllocator(sizeof (mdc_path_info_t *), &iCCLMalloc);
#endif
	iList.SetCompareFunction(scx__mdc_path_list, mdc_list_compare);
	if (mdc_create_cache_dir() < 0) {
		return -1;
	}
	pthread_create(&mdc_monitor_tid, NULL, mdc_monitor, (void *)NULL);
	return 0;
}

int
mdc_deinit()
{
	int ret = 0;
	int i, lsize = 0;
	mdc_path_info_t *path_info = NULL;
	if (mdc_monitor_tid)
	{
		pthread_cancel(mdc_monitor_tid);
		pthread_join(mdc_monitor_tid, NULL);
	}

	if (scx__mdc_path_list) {
		lsize = iList.Size(scx__mdc_path_list);
		for (i = 0; i < lsize; i++) {
			path_info = *(mdc_path_info_t **)iList.GetElement(scx__mdc_path_list,i);
			SCX_FREE(path_info);
		}
		ret = iList.Finalize(scx__mdc_path_list);
		if (0 > ret) {
			TRACE((T_ERROR, "MDC path list remove failed.(reason : %s)\n", iError.StrError(ret)));
		}
		scx__mdc_path_list = NULL;
	}
	return 0;
}

/*
 * This type defines the function used to compare two elements.
 * The result should be less than zero if left is less than right,
 * zero if they are equal, and bigger than zero if left is bigger than right.
 */
static int
mdc_list_compare(const void *left, const void *right, CompareInfo *arg)
{
	mdc_path_info_t *left_info = *(mdc_path_info_t **)left;
	mdc_path_info_t *right_info = *(mdc_path_info_t **)right;
//	printf("left = %ld, right = %ld, left path = %s, right path = %s\n", left_info->st_mtime, right_info->st_mtime, left_info->filepath, right_info->filepath);
    if (left_info->st_mtime < right_info->st_mtime)
        return -1;
    else if (left_info->st_mtime > right_info->st_mtime)
        return 1;
    return 0;
}

int
mdc_write_func(unsigned char *block, size_t size, void *param)
{
	mdc_context_t *ctx = (mdc_context_t*)param;
	size_t write_size;

	write_size = fwrite((const void *)block, 1, size, ctx->fd);
    TRACE((T_DEBUG, "size = %d, write = %d\n", size, write_size));
    if(write_size != size) {
    	TRACE((T_INFO, "write error occured. to(%lld), writed(%lld). reason = %d(%s)\n", size, write_size, errno, strerror(errno)));
    	return zipper_err_io_handle;
    }

    return zipper_err_success;
}
ssize_t
mdc_reader_func(void *context, void *buf, size_t size, off_t offset, void *param)
{
	mdc_context_t *ctx = (mdc_context_t*)param;
	ssize_t 		copied = 0;
	int	ret = 0;

	TRACE((T_DEBUG, "offset = %lld, size = %lld\n", ctx->base_offset+offset, size));
	if(ctx->fd != NULL) {
		ret = fseek(ctx->fd, ctx->base_offset+offset, SEEK_SET);
		if (ret < 0) {
			TRACE((T_INFO, "seek(%lld) error occured. reason = %d(%s)\n", ctx->base_offset+offset, errno, strerror(errno)));
			return 0;
		}
		copied = fread( buf, 1, size, ctx->fd);
	}
    return copied;
}

/*
 * metadata disk cache 의 사용량을 계산한다.
 * 사용량이 지정량(disk_media_cache_high) 보다 많은 경우
 * 오래된 파일 순으로 삭제전용 디렉토리로 이동 시켜서 전체 용량이 disk_media_cache_low가 될때까지 반복한다.
 * 위 작업이 끝난후 삭제전용 디렉토리에 있는 파일들을 모두 삭제 한다.
 */
int
mdc_check_cache_dir_usage()
{
	uint64_t totalSize = 0LL;
	char	rename_path[MEDIA_METADATA_CACHE_DIR_MAX_LEN+30] = {'\0',};
	uint64_t disk_media_cache_high = gscx__config->disk_media_cache_high * 1048576LL;
	uint64_t disk_media_cache_low = gscx__config->disk_media_cache_low * 1048576LL;
	static int remove_cnt = 0;
	int	ret	= 0;

	if (!gscx__config->disk_media_cache_enable)  return 0;

	/* 해당 폴더의 파일 사용량을 계산해서 일정 용량을 초과하면 지운다. */
	mdc_clean_remove_dir(); /* 항상 삭제 전용 디렉토리를 파일을 비운다. */
	mdc_cal_dir_size(&totalSize, vs_data(gscx__config->disk_media_cache_dir), 0);
	TRACE((T_DAEMON, "meta data cache total size = %lld\n", totalSize));
	if (totalSize > disk_media_cache_high ) {
		TRACE((T_INFO, "meta data cache file reclaim start. total(%lld)\n", totalSize));
		totalSize = 0;
		mdc_cal_dir_size(&totalSize, vs_data(gscx__config->disk_media_cache_dir), 1);
		iList.Sort(scx__mdc_path_list);
		mdc_path_info_t *path_info = NULL;
		int lsize = iList.Size(scx__mdc_path_list);
		int i;
		for (i = 0; i < lsize; i++) {
			path_info = *(mdc_path_info_t **)iList.GetElement(scx__mdc_path_list,i);
			if (totalSize > disk_media_cache_low) {
				snprintf(rename_path, MEDIA_METADATA_CACHE_DIR_MAX_LEN+30, "%s/%s/removed_%d.mdc", vs_data(gscx__config->disk_media_cache_dir), MEDIA_METADATA_CACHE_REMOVE, remove_cnt++);
				//snprintf(rename_path, MEDIA_METADATA_CACHE_DIR_MAX_LEN+10, "%s/%s/", vs_data(gscx__config->disk_media_cache_dir), MEDIA_METADATA_CACHE_REMOVE, remove_cnt++);
				ret	= rename(path_info->filepath, rename_path);
				if (ret < 0) {
					TRACE((T_WARN, "cache file move failed. %s to %s. reason(%s)\n", path_info->filepath, rename_path, strerror(errno)));
				}
				else {
					TRACE((T_DAEMON, "cache file moved. %s to %s.\n", path_info->filepath, rename_path));
				}
				totalSize -= path_info->st_size;
			}
			SCX_FREE(path_info);
		}
		iList.Clear(scx__mdc_path_list);
		/* 삭제 전용 디렉토리에 있는 파일 들을 모두 삭제 한다. */
		mdc_clean_remove_dir();
	}

	return 0;
}

/*
 * key 값을 사용해서 디스크에 저장될 캐시파일 경로를 생성한다.
 */
static char *
mdc_make_cache_path(char *key)
{
	char		*buf = NULL;
	int			n;
	char		*cache_path = vs_data(gscx__config->disk_media_cache_dir);
	uint32_t 	hash = 0;

	hash = scx_hash(key);
	buf = (char *)SCX_MALLOC(strlen(cache_path) + 26);
	n  = sprintf(buf, "%s/%02X/%08x.%s",cache_path, hash >> 24, hash, MEDIA_METADATA_CACHE_EXT);
	return buf;
}

/*
 * 디스크에 저장된 meta data cache 파일을 읽는다.
 * 파일이 없는 경우나 문제가 있는 경우 NULL을 리턴한다.
 * 정상적으로 캐시파일을 읽은 경우 zipperCnt를 리턴
 */
zipperCnt
mdc_load_cache(nc_request_t *req, struct nc_stat *objstat, char *url)
{
	zipper_io_handle  *temp_ioh = NULL;
	size_t dumpsize;
	FILE * fd = NULL;
	char *key = NULL;
	char *cache_path = NULL;
	zipperCnt mcontext = NULL;
	double 			ts, te;
	mdc_context_t mdc_ctx;
	disk_minfo_t minfo;
	int	ret = 0;
	ssize_t copied = 0;

	if (!gscx__config->disk_media_cache_enable)  return NULL;

	key = strm_make_media_key(req, url);
	cache_path = mdc_make_cache_path(key);

	if (access(cache_path, F_OK) < 0) {
		/* cache file이 없는 경우는 skip 한다 */
		TRACE((T_DAEMON, "load mdc file not found(%s).\n", cache_path));
		ret = -1;
		goto end_load_cache;
	}
	fd = fopen(cache_path, "r");
	if(fd != NULL) {
		mdc_ctx.req = req;
		mdc_ctx.fd = fd;
		mdc_ctx.base_offset = 0;
		copied = mdc_reader_func(NULL, (void *)&minfo, sizeof(disk_minfo_t), 0, &mdc_ctx);
		if (copied != sizeof(disk_minfo_t)) {
			/* cache header를 읽지 못한 경우 */
			TRACE((T_WARN, "[%llu] failed to read header info. path(%s), read size = %d\n",	req->id, cache_path, copied));
			ret = -1;
			goto end_load_cache;
		}
		/* cache header가 정상적인지 확인한다. */
		if (minfo.magic != MEDIA_CACHE_HEADER_MAGIC) {
			TRACE((T_WARN, "[%llu] cache header magic corrupted. path(%s)\n", req->id, cache_path));
			ret = -1;
			goto end_load_cache;
		}
		if (minfo.version != (MEDIA_CACHE_VERSION+zipper_dump_current_ver)) {
			TRACE((T_WARN, "[%llu] cache version different. path(%s)\n", req->id, cache_path));
			ret = -1;
			goto end_load_cache;
		}
		/* hash가 같더라도 cache key이 다른 경우는 기존 캐시를 삭제하고 새로 만들도록 한다. */
		if (strcmp(key, minfo.key) != 0) {
			TRACE((T_WARN, "[%llu] cache key different. path(%s)\n", req->id, cache_path));
			ret = -1;
			goto end_load_cache;
		}
		strncpy(objstat->st_devid, minfo.st_devid, 128);
		objstat->st_vtime = minfo.vtime;
		objstat->st_mtime = minfo.mtime;
		objstat->st_size = minfo.media_size;
		/* MEDIA_CACHE_BASE_OFFSET은 zipper metadata가 들어 있는 위치임 */
		mdc_ctx.base_offset = MEDIA_CACHE_BASE_OFFSET;

		temp_ioh = strm_set_io_handle(NULL);
		temp_ioh->reader.readfp = mdc_reader_func;
		temp_ioh->reader.param = &mdc_ctx;
		temp_ioh->reader.sizefp = NULL;
		ts = sx_get_time();
		if  (sigsetjmp(__thread_jmp_buf, 1) == 0) {
			__thread_jmp_working = 1;
			ret = zipper_create_media_context(temp_ioh,&mcontext);
			__thread_jmp_working = 0;
		}
		else {
			/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
			ret = zipper_err_internal;
			TRACE((T_ERROR, "[%llu]zipper_create_media_context() SIGSEGV occured. path(%s). %s:%d\n", req->id, cache_path, __FILE__, __LINE__));
		}
		te = sx_get_time();
		req->t_zipper_build += (te - ts);
		strm_destroy_io_handle(temp_ioh);
		if (zipper_err_success != ret) {
			TRACE((T_WARN, "[%llu] zipper_create_media_context error. path(%s), reason\n", req->id, cache_path, zipper_err_msg(ret)));
			ret = -1;
			mcontext = NULL;
			goto end_load_cache;
		}
		ATOMIC_ADD(gscx__metadata_disk_read_interval_cnt, 1);
		ATOMIC_ADD(gscx__metadata_disk_read_interval_size, zipper_context_size(mcontext));
		TRACE((T_DEBUG, "[%llu] mdc load cache success. path(%s)\n", req->id, cache_path));
	}
	else {
		TRACE((T_WARN, "[%llu] mdc file open failed. path(%s), reason(%s)\n", req->id, cache_path, strerror(errno)));
		ret = -1;
		goto end_load_cache;
	}

end_load_cache:
	if(fd != NULL) {
		fclose(fd);
	}
	if (ret < 0) {
		/* cache file에 문제가 있는것으로 보고 삭제 한다. */
		mdc_disable_cache_file(req, url);
	}
	if (key != NULL) {
		SCX_FREE(key);
	}
	if (cache_path != NULL) {
		SCX_FREE(cache_path);
	}
	return mcontext;
}

/*
 * media metadata를 디스크에 저장한다.
 * is_update가 1인 경우는 기존 디스크 캐시에 TTL만 업데이트 하는 경우임
 */
int
mdc_save_cache(nc_request_t *req, media_info_t *media, int is_update)
{
	zipper_io_handle dump_io;
	size_t dumpsize;
	FILE *fd = NULL;
	char *key = NULL;
	char *cache_path = NULL;
	struct stat cache_stat;
	double 			ts, te;
	mdc_context_t mdc_ctx;
	disk_minfo_t minfo;

	int	ret = 0;

	if (!gscx__config->disk_media_cache_enable)  return 0;
	if (!gscx__metadata_cache_write_enable) return 0;

	key = strm_make_media_key(req, media->url);
	cache_path = mdc_make_cache_path(key);


	if(is_update) {
		fd = fopen(cache_path, "r+");
	}
	else {
		fd = fopen(cache_path, "w");
	}

	if(fd != NULL) {
		if(is_update) {
			memset(&cache_stat, 0,  sizeof(struct stat));
			ret = stat(cache_path, &cache_stat);
			if (ret < 0) {
				TRACE((T_WARN, "[%llu] stat failed, path[%s], reason(%s)\n", req->id, cache_path, strerror(errno)));
				ret = -1;
				goto end_save_cache;
			}

		}
		mdc_ctx.req = req;
		mdc_ctx.fd = fd;
		mdc_ctx.base_offset = MEDIA_CACHE_BASE_OFFSET;
		dump_io.writer.fp = mdc_write_func;
		dump_io.writer.param = &mdc_ctx;
		if(is_update) {
			/* is_update가 1이라도 st_size가 0인 경우는 캐시 파일이 지워진 상태이기 때문에 전체를 저장해야 한다. */
			if (cache_stat.st_size > 0) {
				/* 헤더만 저장하면 되기 때문에 아래 부분은 skip 한다 */
				goto save_cache_header;
			}
		}

		fseek(fd, MEDIA_CACHE_BASE_OFFSET, SEEK_SET);
		ts = sx_get_time();
		if  (sigsetjmp(__thread_jmp_buf, 1) == 0) {
			__thread_jmp_working = 1;
			ret = zipper_context_dump(&dump_io, media->mcontext, &dumpsize);
			__thread_jmp_working = 0;
		}
		else {
			/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
			ret = zipper_err_internal;
			TRACE((T_ERROR, "[%llu] zipper_rtmp_process() SIGSEGV occured. path(%s). %s:%d\n", req->id, cache_path, __FILE__, __LINE__));
		}
		te = sx_get_time();
		req->t_zipper_build += (te - ts);

		if (zipper_err_success != ret) {
			// file close후 파일 삭제 해야함
			TRACE((T_WARN, "[%llu] zipper_context_dump failed, URL[%s], reason(%s)\n",
					req->id, vs_data(req->url), zipper_err_msg(ret)));
			ret = -1;
			goto end_save_cache;
		}
		ATOMIC_ADD(gscx__metadata_disk_write_interval_cnt, 1);
		ATOMIC_ADD(gscx__metadata_disk_write_interval_size, dumpsize);
		TRACE((T_DEBUG, "[%llu] mdc save cache. path(%s), size(%lld)\n", req->id, cache_path, dumpsize));

	}
	else {
		TRACE((T_WARN, "[%llu] mdc file open failed. path(%s), reason(%s)\n",
							req->id, cache_path, strerror(errno)));
		ret = -1;
		goto end_save_cache;
	}
save_cache_header:
	/* header 를 저장 */
	memset(&minfo, 0,  sizeof(disk_minfo_t));
	minfo.magic = MEDIA_CACHE_HEADER_MAGIC;
	minfo.version = (MEDIA_CACHE_VERSION+zipper_dump_current_ver);	//SolProxy와 Zipper 라이브러리의 버전 변경을 모두 수용하기 위해 이렇게 한다.
	snprintf(minfo.key, 1024, "%s", key);
	snprintf(minfo.st_devid, 128, "%s", media->st_devid);
	minfo.vtime = media->vtime;
	minfo.mtime = media->mtime;
	minfo.media_size = media->msize;
	minfo.meta_size = dumpsize;
	ret = fseek(fd, 0, SEEK_SET);
	if (ret < 0) {
		TRACE((T_WARN, "[%llu] mdc file fseek failed. path(%s), reason(%s)\n",
									req->id, cache_path, strerror(errno)));
		goto end_save_cache;
	}
	mdc_ctx.base_offset = 0;
	mdc_write_func((char*)&minfo, sizeof(disk_minfo_t), &mdc_ctx);
	TRACE((T_DEBUG, "[%llu] mdc update cache header. path(%s)\n", req->id, cache_path));
	//printf("minfo.magic = %08X\n", minfo.magic);
end_save_cache:
	if(fd != NULL) {
		fclose(fd);

	}
	if (ret < 0) {
		/* 문제가 있는 경우 파일을 삭제 한다. */
		unlink(cache_path);
	}
	if (cache_path != NULL) {
		SCX_FREE(cache_path);
	}
	if (key != NULL) {
		SCX_FREE(key);
	}


	return 0;
}

/*
 * 캐시 파일에 문제가 있는 경우 바로 삭제 하지 않고 삭제 전용 디렉토리로 move시킨다.
 */
int
mdc_disable_cache_file(nc_request_t *req, char *url)
{
	char *key = NULL;
	char *cache_path = NULL;
	mdc_context_t mdc_ctx;
	disk_minfo_t minfo;
	int	ret = 0;
    char	rename_path[MEDIA_METADATA_CACHE_DIR_MAX_LEN+30] = {'\0',};
    static int disabled_cnt = 0;

    if (!gscx__config->disk_media_cache_enable)  return 0;

	key = strm_make_media_key(req, url);
	cache_path = mdc_make_cache_path(key);
	snprintf(rename_path, MEDIA_METADATA_CACHE_DIR_MAX_LEN+30, "%s/%s/disabled_%d.mdc", vs_data(gscx__config->disk_media_cache_dir), MEDIA_METADATA_CACHE_REMOVE, disabled_cnt++);
	//snprintf(rename_path, MEDIA_METADATA_CACHE_DIR_MAX_LEN+10, "%s/%s/", vs_data(gscx__config->disk_media_cache_dir), MEDIA_METADATA_CACHE_REMOVE, remove_cnt++);
	if (access(cache_path, F_OK) < 0) {
		/* cache file이 없는 경우는 skip 한다 */
		TRACE((T_DAEMON, "cache file not found(%s). %s. reason(%s)\n", url, cache_path,  strerror(errno)));
	}
	else {
		ret	= rename(cache_path, rename_path);
		if (ret < 0) {
			TRACE((T_WARN, "cache file move failed(%s). %s to %s. reason(%s)\n", url, cache_path, rename_path, strerror(errno)));
		}
		else {
			TRACE((T_DAEMON, "cache file moved(%s). %s to %s.\n", url, cache_path, rename_path));
		}
	}

	if (cache_path != NULL) {
		SCX_FREE(cache_path);
	}
	if (key != NULL) {
		SCX_FREE(key);
	}
	return 0;
}

static int64_t
mdc_get_free_space()
{
	int		ret = 0;
	struct statvfs stat;
	uint64_t  free_space = 0;

	char	*cache_path = vs_data(gscx__config->disk_media_cache_dir);

	ret = statvfs(cache_path, &stat);
	if (ret < 0) {
		TRACE((T_WARN, "Failed to get free space info, path(%s), reason(%s)\n", cache_path, strerror(errno)));
		return 0;
	}
	free_space = (int64_t)stat.f_bavail * stat.f_bsize;
	TRACE((T_DAEMON, "path(%s), free space = %lld\n", cache_path, free_space));
	return free_space;
}

/*
 * media metadata disk cache에 사용된 캐시 경로를 생성한다.
 * 전달되는 path는 [cachedir]/media로 되어야 한다.
 * media metadata disk cache 경로는 최대 128자 까지만 허용한다.
 */
static int
mdc_create_cache_dir()
{
	DIR		*pCacheDir = NULL;
	int		ret = 0;
	int		i = 0;
	int		max_sub_path_len = 0;
	char	sub_path[MEDIA_METADATA_CACHE_DIR_MAX_LEN+10] = {'\0',};
	int		cache_path_len = 0;
	char	*cache_path = vs_data(gscx__config->disk_media_cache_dir);
	max_sub_path_len = sizeof(sub_path);
	cache_path_len = strlen(cache_path);
	if (cache_path_len > MEDIA_METADATA_CACHE_DIR_MAX_LEN) {
		TRACE((T_ERROR, "Failed to create media metadata disk cache path length too long(%d), max length(%d)\n", cache_path_len, MEDIA_METADATA_CACHE_DIR_MAX_LEN));
		return -1;
	}

	if ((pCacheDir = opendir(cache_path)) == NULL) {
		if (errno == ENOENT) {
			/* 디렉토리가 없는 경우는 생성 */
			ret = mkdir(cache_path, 0755);
			if (ret < 0) {
				TRACE((T_ERROR, "Failed to create media metadata disk cache dir '%s', reason(%s)\n", cache_path, strerror(errno)));
				return -1;
			}
			TRACE((T_INFO, "Create media metadata disk cache dir '%s'\n", cache_path));
		}
		else {
			TRACE((T_ERROR, "couldn't open media metadata disk cache dir '%s', reason(%s)\n", cache_path, strerror(errno)));
			return -1;
		}
	}
	else  {
		closedir(pCacheDir);
	}

	/* sub directory들이 있는지 확인해서 없으면 생성 한다. */
	for (i = 0; i < 256; i++) {
		snprintf(sub_path, max_sub_path_len, "%s/%02X", cache_path, i);
		if ((pCacheDir = opendir(sub_path)) == NULL) {
			if (errno == ENOENT) {
				/* 디렉토리가 없는 경우는 생성 */
				ret = mkdir(sub_path, 0755);
				if (ret < 0) {
					TRACE((T_ERROR, "Failed to create media metadata disk cache dir '%s', reason(%s)\n", sub_path, strerror(errno)));
					return -1;
				}
				TRACE((T_DAEMON, "Create media metadata disk cache dir '%s'\n", cache_path));
			}
			else {
				TRACE((T_ERROR, "couldn't open media metadata disk cache dir '%s', reason(%s)\n", sub_path, strerror(errno)));
				return -1;
			}
		}
		else  {
			closedir(pCacheDir);
		}
	}
	/* 삭제 전용 디렉토리 생성 */
	snprintf(sub_path, max_sub_path_len, "%s/%s", cache_path, MEDIA_METADATA_CACHE_REMOVE);
	if ((pCacheDir = opendir(sub_path)) == NULL) {
		if (errno == ENOENT) {
			/* 디렉토리가 없는 경우는 생성 */
			ret = mkdir(sub_path, 0755);
			if (ret < 0) {
				TRACE((T_ERROR, "Failed to create media metadata disk cache dir '%s', reason(%s)\n", sub_path, strerror(errno)));
				return -1;
			}
			TRACE((T_DAEMON, "Create media metadata disk cache dir '%s'\n", cache_path));
		}
		else {
			TRACE((T_ERROR, "couldn't open media metadata disk cache dir '%s', reason(%s)\n", sub_path, strerror(errno)));
			return -1;
		}
	}
	else  {
		closedir(pCacheDir);
	}

	return 0;
}

/*
 * cache 디렉토리 용량을 계산
 * 단순 용량만 계산할 경우 make_list를 0으로 하고
 * 지우기 위해 파일 리스트를 만들 경우 make_list를 1로 한다.
 * make_list를 1로 해서 호출하는 경우 scx__mdc_path_list가 비어 있는 상태여야 한다.
 */
static int
mdc_cal_dir_size(uint64_t *totalSize, char *path, int make_list)
{
    char fullPath[256];
    struct dirent *dirData;
    struct stat file_stat;
    int exists;
    DIR *poDir;
	char*	szFileExt = NULL;
    int ret = 0;
    mdc_path_info_t	*path_info = NULL;

    poDir = opendir(path);
    if (poDir == NULL)
    {
        TRACE((T_WARN, "couldn't open '%s'\n", path));
        return -1;
    }

    while ((dirData = readdir(poDir)))
    {
        if (dirData == NULL)
        {
        	/* 디렉토리를 다 읽은 경우 */
            TRACE((T_DEBUG, "readdir '%s' ended.\n", path));
            break;
        }

        if (dirData->d_type == DT_DIR)
        {
        	/* 디렉토리인 경우 */
            if (dirData->d_name[0] != '.')
            {
            	if(strcmp(dirData->d_name, MEDIA_METADATA_CACHE_REMOVE) == 0) {
            		/* 삭제될 파일이 모인 디렉토리는 제외 */
            		continue;
            	}
                strcpy(fullPath,path);
                strcat(fullPath,"/");
                strcat(fullPath,dirData->d_name);

                if (mdc_cal_dir_size(totalSize,fullPath, make_list) < 0) {
                	/* sub directory에서 에러가 발생하는 경우는 무시한다. */
                }
            }
        }
        else if (dirData->d_type != DT_REG) {
        	/* 일반 파일이 아닌 경우는 skip */
        	continue;
        }
        else
        {
#if 1
			/* 확장자가 .mdc가 아닌 파일 제외 */
			szFileExt = strrchr(dirData->d_name, '.');
			if ( NULL == szFileExt || strncmp(szFileExt+1, MEDIA_METADATA_CACHE_EXT, 3) != 0) {
				continue;
			}
#endif
            strcpy(fullPath,path);
            strcat(fullPath,"/");
            strcat(fullPath,dirData->d_name);

            exists = stat(fullPath,&file_stat);
            //printf("mdc_cal_dir_size: file (%s): %d\n",fullPath,(int)file_stat.st_size);
            if (exists < 0)
            {
                TRACE((T_WARN, "Failed in stat file '%s', reason(%s)\n", fullPath, strerror(errno)));
                continue;
            }
            else
            {
                (*totalSize) += file_stat.st_size;
                //printf("path = %s, size = %lld, mtime = %ld\n", fullPath, file_stat.st_size, file_stat.st_mtim.tv_sec);
                if (make_list == 1) {
					/* 캐시 파일 정보를 리스트에 저장 */
					path_info = SCX_MALLOC(sizeof(mdc_path_info_t));
					snprintf(path_info->filepath, MEDIA_METADATA_CACHE_PATH_MAX_LEN,"%s", fullPath);
					path_info->st_mtime = file_stat.st_mtim.tv_sec;
					path_info->st_size = file_stat.st_size;
					iList.Add(scx__mdc_path_list, (void *)&path_info);

                }
            }
        }
    }

    closedir(poDir);

    return ret;
}

/*
 * 삭제전용 디렉토리에 있는 파일들을 모두 삭제 한다.
 */
int
mdc_clean_remove_dir()
{
    char fullPath[256];
    struct dirent *dirData;
    struct stat file_stat;
    int exists;
    DIR *poDir;
	char*	szFileExt = NULL;
    int ret = 0;
    char	remove_dir[MEDIA_METADATA_CACHE_DIR_MAX_LEN+10] = {'\0',};

    snprintf(remove_dir, MEDIA_METADATA_CACHE_DIR_MAX_LEN, "%s/%s", vs_data(gscx__config->disk_media_cache_dir), MEDIA_METADATA_CACHE_REMOVE);

    poDir = opendir(remove_dir);
    if (poDir == NULL)
    {
        TRACE((T_WARN, "couldn't open '%s'\n", remove_dir));
        return -1;
    }

    while ((dirData = readdir(poDir)))
    {
        if (dirData == NULL)
        {
        	/* 디렉토리를 다 읽은 경우 */
            TRACE((T_DEBUG, "readdir '%s' ended.\n", remove_dir));
            break;
        }

        if (dirData->d_type == DT_DIR)
        {
        	/* 디렉토리인 경우 */
        	continue;
        }
        else if (dirData->d_type != DT_REG) {
        	/* 일반 파일이 아닌 경우는 skip */
        	continue;
        }
        else
        {
			/* 확장자가 .mdc가 아닌 파일 제외 */
			szFileExt = strrchr(dirData->d_name, '.');
			if ( NULL == szFileExt || strncmp(szFileExt+1, MEDIA_METADATA_CACHE_EXT, 3) != 0) {
				continue;
			}
            strcpy(fullPath,remove_dir);
            strcat(fullPath,"/");
            strcat(fullPath,dirData->d_name);
            ret = unlink(fullPath);
            if (ret < 0) {
            	TRACE((T_WARN, "couldn't remove '%s'. reason(%s)\n", fullPath, strerror(errno)));
            }
   //         printf("%s removed\n", fullPath);
        }
    }
    closedir(poDir);
}

/*
 * 주기적으로 실행되면서 metadata disk cache 디렉토리를 감시한다.
 */
void *
mdc_monitor(void *d)
{
	int interval = gscx__config->disk_media_cache_monitor_period * 1000;
	const int64_t size_1GB = 1024*1024*1024;
	prctl(PR_SET_NAME, "mdc monitor");
	while (gscx__signal_shutdown == 0) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		/* cache partition이 여유 공간이 1GB 보다 작은 경우는 캐시파일을 생성하지 않는다. */
		if (mdc_get_free_space() < size_1GB) {
			gscx__metadata_cache_write_enable = 0;
		}
		else {
			gscx__metadata_cache_write_enable = 1;
		}
		mdc_check_cache_dir_usage();
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		bt_msleep(interval);
	}
	return NULL;
}

