#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <regex.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/stat.h>

#include <dict.h>
//#include <ncapi.h>
#include <microhttpd.h>

#include "common.h"
#include "scx_glru.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"

#include "voxio_def.h"
#include "scx_util.h"
#include "smilparser.h"
#include "sessionmgr.h"
#include "status.h"
#include "streaming.h"
#include "streaming_lru.h"

pthread_mutex_t 	gscx__glru_lock = PTHREAD_MUTEX_INITIALIZER;

volatile int 	gscx__media_seq = 0;	/* media 생성 sequence */
volatile int 	gscx__media_ref_cnt = 0;
volatile int 	gscx__media_using_cnt = 0;	/* 현재 사용가능한 media의 숫자, 이게 부족하면 builder를 강제로 삭제 해야함 */
volatile uint64_t gscx__media_context_size = 0;
volatile int 	gscx__media_context_create_interval_cnt = 0;	/* 단위 시간동안 생성된 media context 수 */
volatile uint64_t gscx__media_context_create_interval_size = 0;/* 단위 시간동안 생성된 media context의 메모리 사용량 */
extern int				gscx__max_url_len;
extern volatile int 		gscx__metadata_disk_read_interval_cnt;			/* 단위 시간동안 disk에서 read된 media context 수 */
extern volatile uint64_t 	gscx__metadata_disk_read_interval_size;			/* 단위 시간동안 disk에서 read된 media context의 byte 수*/
extern volatile int 		gscx__metadata_disk_write_interval_cnt;			/* 단위 시간동안 disk에서 write된 media context 수 */
extern volatile uint64_t 	gscx__metadata_disk_write_interval_size;		/* 단위 시간동안 disk에서 write된 media context의 byte 수*/

glru_t 				*g__glru_media_cache = NULL ;
nc_int32_t 			g__media_cachesize = 2000;
dict 	*scx__glru_media_dct = NULL;

static void key_val_free(void *key, void *datum);
static void media_key_val_free(void *key, void *datum);
static int strm_dict_strcmp(const void *k1, const void *k2);
static unsigned strm_dict_create_hash(const void *p);
static void *gnode_allocate(glru_node_t **gnodep);
static void *gnode_free(glru_node_t *gnode, void *data);
static void gnode_reset(void *data);
static void gnode_fill_key(void *data, const char * key);
static void *gnode_lookup_media(void *map, const char *key);
static int gnode_enroll_media(void *map, glru_node_t *gnode);
static int gnode_unroll(void *map, glru_node_t *gnode);
static int gnode_isidle(void *d);

int
scx_lru_init()
{

	glru_operation_map_t 			media_map = {
		gnode_allocate,
		gnode_free,
		gnode_reset,
		gnode_fill_key,
		gnode_lookup_media,
		gnode_enroll_media,
		gnode_unroll,
		gnode_isidle,
		NULL
	};

    dict_compare_func cmp_func = strm_dict_strcmp;
    dict_hash_func hash_func = strm_dict_create_hash;

    g__media_cachesize = gscx__config->media_cache_size;
	g__glru_media_cache = glru_init(g__media_cachesize + 3, &media_map, "media-cache"); /* 혹시라도 glru 부족으로 대기 하는 상황을 방지 하기 위해 설정값 보다 조금더 크게 생성한다. */
    scx__glru_media_dct = hashtable_dict_new(strcmp, dict_str_hash, DICT_HSIZE);
    ASSERT(dict_verify(scx__glru_media_dct));

}

void
scx_lru_destroy()
{
	int 				n = 0;
	gnode_info_t 	*ginfo = NULL;
	media_info_t 	*media = NULL;

	dict_itor *itor = NULL ;
	pthread_mutex_lock(&gscx__glru_lock);

	if (scx__glru_media_dct) {
		itor = dict_itor_new(scx__glru_media_dct);
		dict_itor_first(itor);
		while (dict_itor_valid(itor)) {
			ginfo = (gnode_info_t *)*dict_itor_datum(itor);
			media = (media_info_t *)ginfo->context;
			if (media)
				strm_destroy_media(media);
			dict_itor_next(itor);
		}
		dict_itor_free(itor);
		n = dict_free(scx__glru_media_dct, key_val_free);
		TRACE((T_INFO, "media cache destroyed - %d entries\n", n));
	}
	if(g__glru_media_cache) {
		n = glru_destroy(g__glru_media_cache);
		TRACE((T_INFO, "media glru cache destroyed - %d entries\n", n));
	}
	pthread_mutex_unlock(&gscx__glru_lock);
}

static void
key_val_free(void *key, void *datum)
{

}

static void
media_key_val_free(void *key, void *datum)
{
//	strm_destroy_media(datum);
}

static int
strm_dict_strcmp(const void *k1, const void *k2)
{
    return dict_ptr_cmp(k1, k2);
}

static unsigned
strm_dict_create_hash(const void *p)
{
    // return dict_str_hash(p); // session id가 random int를 사용하기 때문에 별도의 hashing은 필요 없다.
	//unsigned hash = atoi(p);
	unsigned hash = (unsigned )p;
	return hash;
}

static void *
gnode_allocate(glru_node_t **gnodep)
{
	gnode_info_t 	*ginfo = NULL;

	ginfo = (gnode_info_t *)SCX_CALLOC(1, sizeof(gnode_info_t));

	ginfo->type = GNODE_TYPE_NOTDEFINE;
	ginfo->key = NULL;

	*gnodep = &ginfo->gnode;
	return ginfo;
}

static void *
gnode_free(glru_node_t *gnode, void *data)
{
	ASSERT(data);
	gnode_info_t 	*ginfo = (gnode_info_t *)data;
	if (ginfo->key != NULL) {
		SCX_FREE(ginfo->key);
	}
	SCX_FREE(data)
}


static void
gnode_reset(void *data)
{
	gnode_info_t 	*ginfo = (gnode_info_t *)data;

	ginfo->type = GNODE_TYPE_NOTDEFINE;

	if (ginfo->key != NULL) {
		SCX_FREE(ginfo->key);
	}

	ginfo->key = NULL;
	ginfo->id = 0;
	ginfo->context = NULL;
}


static void
gnode_fill_key(void *data, const char * key)
{
	gnode_info_t 	*ginfo = (gnode_info_t *)data;
	int				key_len;

	key_len = strlen(key);
	ginfo->key = SCX_MALLOC(key_len+1);
	snprintf(ginfo->key, key_len+1, "%s", key);
}

static void *
gnode_lookup_media(void *map, const char *key)
{
	glru_node_t 	*gnode = NULL;
	gnode_info_t 	*ginfo = NULL;

	void** search = dict_search(scx__glru_media_dct, (void *)key);
	if (search == NULL) {
		   return NULL;
	}
	ginfo = *(gnode_info_t **)search;

	gnode = (void *)&ginfo->gnode;
	return gnode;
}

static int
gnode_enroll_media(void *map, glru_node_t *gnode)
{
	gnode_info_t 	*ginfo = (gnode_info_t *)gnode->gdata;
	media_info_t 	*media = NULL;
	void 			**datum_location = NULL;
	bool inserted = false;

	media = allocate_media(gnode->gdata, ginfo->key);
	/* gnode 셋팅및 glru hash 등록 */
	ginfo->id = media->id;
	ginfo->context = (void *)media;
	ginfo->type = GNODE_TYPE_MEDIA;

	dict_insert_result result = dict_insert(scx__glru_media_dct, (void *)ginfo->key);
	if (result.inserted) {
		ASSERT(result.datum_ptr != NULL);
		ASSERT(*result.datum_ptr == NULL);
		*result.datum_ptr = ginfo;
	}


	return GLRUR_FOUND;
}


/*
 * glru hash에서 삭제 available을 0, deleted을 1으로 셋팅, builder의 refcount가 0이면 builder_info_t를 hash 에서 삭제
 */
static int
gnode_unroll(void *map, glru_node_t *gnode)
{
	gnode_info_t 	*ginfo = (gnode_info_t *)gnode->gdata;
	builder_info_t 	*builder = NULL;
	media_info_t 	*media = NULL;

	if (ginfo->type == GNODE_TYPE_MEDIA) {
		media = (media_info_t *)ginfo->context;
		delete_media(media);
		dict_remove(scx__glru_media_dct, ginfo->key);
	}
	else {
		TRACE((T_ERROR, "Unexpected Error(%s)\n", __func__));
		ASSERT(0);
	}
	return GLRUR_FOUND;
}

static int
gnode_isidle(void *d)
{
	/* 이곳이 호출 되는 경우는 사용자가 한명도 없는 media 에 대해서만 호출 되기 때문에 항상 1로 리턴하면 된다. */
	return 1;
}

builder_info_t *
allocate_builder()
{
	mem_pool_t 		*mpool = NULL;
	builder_info_t 	*builder = NULL;
	bool inserted = false;

	mpool = mp_create(sizeof(builder_info_t) + gscx__max_url_len);
	ASSERT(mpool);
	builder = (builder_info_t *)mp_alloc(mpool, sizeof(builder_info_t));
	ASSERT(builder);
	builder->mpool = mpool;


	builder->bcontext = NULL;
	builder->ctime = scx_get_cached_time_sec();

	builder->bandwidth = 1280000;
	builder->duration = 0.0;

	return builder;
}

media_info_t *
allocate_media(void *data, const char * key)
{
	gnode_info_t 	*ginfo = (gnode_info_t *)data;
	mem_pool_t 		*mpool = NULL;
	media_info_t 	*media = NULL;
	int				key_len = 0;
	bool inserted = false;
	if (NULL != key) {
		key_len = strlen(key);
	}
	mpool = mp_create(key_len +  sizeof(media_info_t) + gscx__max_url_len);
	ASSERT(mpool);
	media = (media_info_t *)mp_alloc(mpool, sizeof(media_info_t));
	ASSERT(media);
	media->mpool = mpool;
	media->seq_id = ATOMIC_ADD(gscx__media_seq, 1);
	media->id = media->seq_id;
	media->ginfo = (void *)ginfo;
	media->available = 1;


	return media;
}

int
delete_media(media_info_t *media)
{
	media->available = 0;
	media->ginfo = NULL;

	strm_destroy_media(media);

}

/*
 * media metadata cache에 사용될 key를 만든후 리턴한다.
 * 리턴된 key는 사용후 반드시 SCX_FREE()를 해주어야함.
 */
char *
strm_make_media_key(nc_request_t *req, char *url)
{
	char 			*key = NULL;
	int				keylen = 0;
	if (req->service->master_cache_domain == NULL) {
		keylen = vs_length(req->service->name) + strlen(url) + 1;
		key = SCX_MALLOC(keylen);
		snprintf(key, keylen, "%s%s", vs_data(req->service->name), url);
	}
	else {
		/* 여러 볼륨이 cache를 공유 하는 기능 사용시 master cache의 domain이 key가 된다. */
		keylen = vs_length(req->service->master_cache_domain) + strlen(url) + 1;
		key = SCX_MALLOC(keylen);
		snprintf(key, keylen, "%s%s", vs_data(req->service->master_cache_domain), url);
	}
	return key;
}


/*
 * key는 (service name)+url로 한다.
 * 처음 할당하는 경우(리턴이 GLRUR_ALLOCATED)인 경우와 GLRUR_FOUND인 경우를 구분해서 저장할수 있어야함.
 * allocifnotexist :  찾기만 하는 경우에는 U_FALSE, 찾아서 없는 경우 할당을 하는 경우에는 U_TRUE(이 경우 lock이 걸림)를 설정해야 한다.
 * setprogress : false 인 경우 처음 한번만 전체 대기 하고  처음 lock이 풀리면 전체가 풀림
 * gres : glru_ref()의 리턴값을 저장. 새로 할당된 경우 GLRUR_ALLOCATED가 설정됨.
 * gres가 GLRUR_ALLOCATED로 리턴되는 경우는 반드시 glru_commit()이 필요함.
 */
media_info_t *
strm_find_media(nc_request_t *req, int type, char *url, int allocifnotexist, int setprogress, int *gres)
{
	media_info_t 	*find_media = NULL;
	builder_info_t 	*find_builder = NULL;
	gnode_info_t 	*ginfo = NULL;
	glru_node_t 	*gnode = NULL;
	char 			*key = NULL;
	int				i = 0;
	int				use_cnt = 0;
	int				n_allocifnotexist = allocifnotexist;
	int				media_using_cnt = ATOMIC_VAL(gscx__media_using_cnt);

	/* media cache가 모두 사용중인 경우 기존에 캐싱된게 있는지 확인(allocifnotexist를 U_FALSE로)만 하고 없는 경우 glru를 사용하지 않고 새로 생성한다. */
	if (media_using_cnt >= g__media_cachesize ) {
		n_allocifnotexist = U_FALSE;
		//return NULL;

	}
	TRACE((T_DEBUG, "[%llu] medicache use = %d, max = %d, allocifnotexist = %d\n", req->id, media_using_cnt, g__media_cachesize, n_allocifnotexist));
	key = strm_make_media_key(req, url);
	*gres = glru_ref(g__glru_media_cache, &gnode, key, &g__glru_media_cache, n_allocifnotexist, setprogress, __func__, __LINE__);
	SCX_FREE(key);

	if (*gres == GLRUR_NOTFOUND) {
		ASSERT(NULL == gnode); 	/* gres가 NOTFOUND인 상황에서 gnode 값이 NULL이 아닌 경우는 없어야 하지만 혹시 모르니까.. */
		if (n_allocifnotexist) {
			/*
			 * 여유가 있는 상태에서 할당 요청을 했기 때문에 할당 하지 못하는 경우가 있어서는 안되지만
			 * glru_ref() 호출부분까지 동시성 문제로 발생 가능성이 있음.
			 * 동작에는 문제가 없을걸로 판단 되므로 그냥 NULL을 리턴한다0.
			 */
			TRACE((T_ERROR, "[%llu] Unexpected Media Cache status. use = %d, max = %d\n", req->id,media_using_cnt, g__media_cachesize));
			goto allocate_uncached_media;


		}
		else  {
			if(allocifnotexist) {
				TRACE((T_INFO, "[%llu] Insufficient Media Cache. use = %d, max = %d\n", req->id, media_using_cnt, g__media_cachesize));
			}

			return NULL;
		}

	}
	else {
		ATOMIC_ADD(gscx__media_ref_cnt,1);
		ginfo = (gnode_info_t *)gnode->gdata;

		find_media = (media_info_t *)ginfo->context;
		ASSERT(find_media);

		use_cnt = ATOMIC_ADD(find_media->refcount,1);
		if (use_cnt == 1) {
			/* 여기에 들어 오는 경우는 처음 할당 상태이거나 사용중이 아니던 media를 다시 사용하는 경우임 */
			ATOMIC_ADD(gscx__media_using_cnt,1);
		}
		if (U_FALSE == allocifnotexist) {
			/*
			 * 여기에 들어 오는 경우는 strm_purge_media_info()에서 호출되는 경우만 있기 때문에 이후의 과정을 진행 하면 안된다.
			 * Cache의 경우 strm_get_media_attr() 호출시에 죽는 현상이 발생한다.
			 */
			return find_media;
		}
		if (*gres == GLRUR_FOUND ) {
			/*
			 * 새로 할당이 아닌 캐싱 되어 있는 media index의 경우에는
			 * 해당 media가 사용 가능한 상태인지 확인 해야 하고
			 * TTL이 만료된 경우 변경 사항이 있는지 확인 해야 한다.
			 */
			if (find_media->available == 0) {
				/* media에 문제가 있는 경우 */
				if (n_allocifnotexist) {
					if( use_cnt == 1) { /* glru_getref가 1이면 현재 할당 받은 세션외에 다른 사용은 없다 */
						/* 사용자가 없으면 새로 media를 할당해서 리턴한다. */
						strm_reset_media(find_media);
						*gres = GRLUR_RESETTED;
						TRACE((T_INFO, "[%llu] Reset cached media info. path(%s)\n", req->id, url));
						mdc_disable_cache_file(req, url);
					}
					else {
						/* 사용자가 없어질때까지 캐싱되지 않는 media를 새로 생성한다.(glru 사용안함) */
						TRACE((T_INFO, "[%llu] Cached media info found but not available. using(%d), path(%s)\n", req->id, use_cnt, url));
						goto allocate_uncached_media;
					}
				}
				else {
					/*
					 * 여기에 들어 올수 있는 경우는 strm_find_media() 호출시 allocifnotexist가 U_TRUE였다가
					 * glru가 모두 사용중이라서 allocifnotexist가 U_FALSE로 바뀐 상태에서
					 * find_media->available이 0인 상태이다.
					 * 이 경우 find_media를 release하고 NULL을 리턴하면 될것 같음.
					 */
					TRACE((T_INFO, "[%llu] Cached media info found but not available. path(%s)\n", req->id, url));
					goto allocate_uncached_media;
				}
			}
			else {
				/* 캐싱된 media 정보가 있는 경우 만료 시간이 지났는지 확인 한다. */

#if 0
				time_t			timenow = time(NULL);`
				struct nc_stat 		objstat;
				if (find_media->vtime < timenow) {
					/*
					 * 유효 시간이 지난 경우 원본을 다시 열어서 갱신 여부를 확인한다.
					 * 이때 media->st_devid ID가 변경된 경우는 해당 media의 available을 0로 표시하고
					 * media를 찾는 과정을 반복한다.
					 */
					/* 만료 시간이 지난 컨텐츠 들은 시간을 다시 확인 한다. */

					if (strm_get_media_attr(req, type, find_media->url, &objstat) == 0 ) {
						TRACE((T_INFO, "[%llu] Failed to get media attribute. path(%s)\n", req->id, url));
						/* 원본 파일에 문제가 있는 경우 */
						goto allocate_uncached_media;
					}

					if (find_media->mtime != objstat.st_mtime) {
						/*
						 * 원본 파일이 변경된 경우 available을 0로 marking 한 후에
						 * 캐싱되지 않는 media를 새로 생성한다.
						 */
						TRACE((T_INFO, "[%llu] Cached media info mtime updated. cache(%ld), update(%ld), path(%s)\n", req->id, find_media->mtime, objstat.st_mtime, url));
						goto allocate_uncached_media;
					}
					else if (find_media->msize != objstat.st_size) {
						/*
						 * 원본 파일의 크기가 변경된 경우도 파일이 변경 된걸로 판단.
						 */
						TRACE((T_INFO, "[%llu] Cached media size changed. cache(%lld), change(%lld), path(%s)\n", req->id, find_media->msize, objstat.st_size, url));
						goto allocate_uncached_media;
					}
					else if(strncmp(find_media->st_devid, objstat.st_devid, 128) != 0) {
						/*
						 * ETag가 바뀌는 경우에도 파일이 변경된걸로 판단한다.
						 * 이 검사를 skip 하는 기능이 필요할수도 있지만 설정이 너무 복잡해지는것 같아서 제외함.
						 */
						TRACE((T_INFO, "[%llu] Cached media info ETag updated. cache(%s), update(%s), path(%s)\n", req->id, find_media->st_devid, objstat.st_devid, url));
						goto allocate_uncached_media;
					}
					else {
						/* 여기 까지 오는 경우는 원본 파일이 변경되지 않고 netcache core에서 만료 시간이 갱신된 경우라고 봐야  */
						TRACE((T_DAEMON|T_DEBUG, "[%llu] Cached media info expire time update. cache(%ld), update(%ld), path(%s)\n", req->id, find_media->vtime, objstat.st_mtime, url));
						find_media->vtime = objstat.st_vtime;
					}
				}
#else
				time_t			timenow = scx_get_cached_time_sec();
				if (find_media->vtime < timenow) {
					/*
					 * 유효 시간이 지난 경우 원본을 다시 열어서 갱신 여부를 확인한다.
					 * 이때 media->st_devid ID가 변경된 경우는 해당 media의 available을 0로 표시하고
					 * media를 찾는 과정을 반복한다.
					 */
					/* 만료 시간이 지난 컨텐츠 들은 시간을 다시 확인 한다. */
					if (strm_check_media_info(req, type, find_media) < 0) {
						goto allocate_uncached_media;
					}

				}
#endif

			}
		}
		else if (*gres == GLRUR_ALLOCATED )  {
			TRACE((T_DAEMON|T_DEBUG, "[%llu] New media info allocated from glru. path(%s)\n", req->id, url));
		}
		else {
			/* 현재까지의 로직으로는 여기에 들어오는 경우가 없다. */
			TRACE((T_ERROR, "[%llu] Unexpected GLRU return code(%d)\n", req->id, *gres));
			ASSERT(0);
		}
	}

	return find_media;

allocate_uncached_media :
	if (find_media) {
		find_media->available = 0;
		strm_release_media(find_media);
		if (setprogress) {
			strm_commit_media(req, find_media);
		}
		/* metadata disk cache를 삭제한다. */
		mdc_disable_cache_file(req, url);
	}

	*gres = GLRUR_NOTFOUND;
	return NULL;

}

media_info_t *
strm_allocate_media(nc_request_t *req, int *gres)
{
	media_info_t 	*media = NULL;
	ATOMIC_ADD(gscx__media_ref_cnt,1);
	media = allocate_media(NULL, NULL);
	*gres = GLRUR_ALLOCATED;
	TRACE((T_INFO, "[%llu] Allocate temporary media info. use = %d, max = %d\n", req->id, ATOMIC_VAL(gscx__media_using_cnt), g__media_cachesize));
	return media;
}


int
strm_commit_media(nc_request_t *req, media_info_t *media)
{
	if (NULL == media->ginfo) {
		return 1;
	}

	gnode_info_t 	*ginfo = (gnode_info_t *)media->ginfo;
	glru_commit(g__glru_media_cache, &ginfo->gnode);
	return 1;
}

int
strm_release_media(media_info_t *media)
{
	gnode_info_t 	*ginfo = NULL;
	glru_node_t 	*gnode = NULL;
	int 	available = 0;
	int		refcount = 0;
	int 	count = 0;
	ATOMIC_SUB(gscx__media_ref_cnt,1);

	if (NULL == media->ginfo) {
		/* glru에 등록되지 않은 미디어의 경우 바로 삭제 한다. */
		TRACE((T_DAEMON, "NULL media release(%s).\n", media->url));
		strm_destroy_media(media);
		return 1;
	}

	ginfo = (gnode_info_t *)media->ginfo;
	refcount = ATOMIC_SUB(media->refcount,1);
	glru_unref(g__glru_media_cache, &ginfo->gnode);
	if (refcount == 0) {
		ATOMIC_SUB(gscx__media_using_cnt,1);
	}

	return 1;
}

void
strm_write_cache_status()
{
	int	media_cnt = 0;
	int builder_cnt = 0;
	int	glru_media_cnt = 0;
	int glru_builder_cnt = 0;
	int	media_ref_cnt = ATOMIC_VAL(gscx__media_ref_cnt);

	media_cnt = ATOMIC_VAL(gscx__media_using_cnt);
	glru_media_cnt = dict_count(scx__glru_media_dct);

	TRACE((T_STAT|T_INFO, "**** media cache : using(%d), ref(%d), memory(%d, %lld), create(%d, %lld), disk read(%d, %lld), disk write(%d, %lld)\n",
							media_cnt, media_ref_cnt,
							glru_media_cnt, ATOMIC_VAL(gscx__media_context_size),
							ATOMIC_SET(gscx__media_context_create_interval_cnt, 0), ATOMIC_SET(gscx__media_context_create_interval_size, 0),
							ATOMIC_SET(gscx__metadata_disk_read_interval_cnt, 0), ATOMIC_SET(gscx__metadata_disk_read_interval_size, 0),
							ATOMIC_SET(gscx__metadata_disk_write_interval_cnt, 0), ATOMIC_SET(gscx__metadata_disk_write_interval_size, 0)));

	return ;
}
/* 이 부분은 실제 사용할 경우 deadlock이나 segfault 가 발생한다.
 * 오직 테스트용으로만 사용해야 함*/
void
strm_write_cache_dict()
{
	FILE *fp = NULL;
	fp = fopen("/root/sol_cache.log", "a");
	fprintf(fp, "start log.\n");
	dict_itor *itor = NULL ;
	builder_info_t  *builder = NULL;
	media_info_t 	*media = NULL;
	gnode_info_t 	*ginfo = NULL;
	pthread_mutex_lock(&gscx__glru_lock);

	itor = dict_itor_new(scx__glru_media_dct);
	dict_itor_first(itor);
	while (dict_itor_valid(itor)) {
		ginfo = (gnode_info_t *)*dict_itor_datum(itor);
		media = (media_info_t *)ginfo->context;
		fprintf(fp, "media url = %s, avail = %d, id = %d, gref = %d\n",
				(media->url == NULL? "NULL":media->url) ,  media->available, media->id, (ginfo == NULL? 0:glru_getref(g__glru_media_cache, &ginfo->gnode)));
		dict_itor_next(itor);
	}
	dict_itor_free(itor);

	pthread_mutex_unlock(&gscx__glru_lock);
	fclose(fp);
}
