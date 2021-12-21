#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <fcntl.h>
//#include <ncapi.h>
#include <containers.h> /* c container library */
#include <microhttpd.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
#include "status.h"
#include "preload.h"
#include "scx_util.h"



pthread_mutex_t gscx__preload_lock;
pthread_cond_t 	gscx__preload_cond;


List			*scx__preload_list = NULL;	/* preload가 될 대상을 가지고 있는 리스트 */
pthread_t 		gscx__preload_thread_tid;
int				gscx__pl_working = 1;
int				gscx__pl_inited = FALSE;
typedef struct tag_preload_info {
	mem_pool_t 	*mpool;
	char 		*host; /* 변수 명*/
	char 		*url; /* 값 */
	uint64_t	id;		/* unique ID */
	nc_xtra_options_t	*options;
} preload_info_t;


void *pl_working_thread(void *d);
int pl_destroy_info(preload_info_t *pinfo);
int pl_do_preload(preload_info_t *pinfo, char *buf, int bufsize);
static int pl_clone_options(nc_xtra_options_t *root, preload_info_t *pinfo);

/*
 * preload 시의 object_key는 요청이 들어온 url을 사용한다.
 * 만약 lua script등에서 object_key를 바꾸는 경우는 preload가 정상 동작 하지 않는다.
 * streaming 기능 사용시의 preload는 별도 구현예정.
 */
/*
 * loop을 돌면서 lock을 걸고 상태에서 preload 할 파일이 있는지 확인
 * 있으면 해당 파일을 nc_read로 읽음.
 * 없으면 시그널을 기다리면 무한 대기
 */
void *
pl_working_thread(void *d)
{
#define PRELOAD_BUF_SIZE 	262144
	preload_info_t 	*pinfo = NULL;
	int ret;
	char buf[PRELOAD_BUF_SIZE];
	prctl(PR_SET_NAME, "Preload working thread");
	while (gscx__pl_working && gscx__signal_shutdown == 0) {
		pthread_mutex_lock(&gscx__preload_lock);
		ret = iList.PopFront(scx__preload_list,&pinfo);
		pthread_mutex_unlock(&gscx__preload_lock);
		if (0 == ret) {
			/*
			 * 리스트를 검사해서 읽을 파일이 없으면 무한 대기
			 */
			pthread_mutex_lock(&gscx__preload_lock);
			pthread_cond_wait(&gscx__preload_cond, &gscx__preload_lock);
			pthread_mutex_unlock(&gscx__preload_lock);
			continue;
		}
		else if (0 > ret) {
			/* 어떤 경우에 발생 할지 모르므로 일단 프로그램을 중단하도록 한다. */
			ASSERT(0);
			TRACE((T_ERROR, "Preload working error.(where:%s, reason:%d)\n", __func__, ret));
		}
		ASSERT(pinfo);

		ret = pl_do_preload(pinfo, buf, PRELOAD_BUF_SIZE);
		/* preload가 성공한 후에는 정보를 지운다. */
		pl_destroy_info(pinfo);
	}

}

void
pl_init()
{
#if 0
	scx__preload_list = iList.Create(sizeof (preload_info_t *));
#else
	scx__preload_list = iList.CreateWithAllocator(sizeof (preload_info_t *), &iCCLMalloc);
#endif
	gscx__pl_working = 1;
	pthread_mutex_init(&gscx__preload_lock, NULL);
	pthread_cond_init(&gscx__preload_cond, NULL);
	pthread_create(&gscx__preload_thread_tid, NULL, pl_working_thread, (void *)NULL);
	gscx__pl_inited = TRUE;

}

void
pl_deinit()
{
	int i, lsize = 0;
	preload_info_t 	*pinfo = NULL;
	int ret;
	if (FALSE == gscx__pl_inited) return ;
	gscx__pl_working = 0;
	/* pl_working_thread에 signal을 보내서 깨운후 thread가 종료 되기를 기다린다. */
    pthread_mutex_lock(&gscx__preload_lock);
    pthread_cond_signal(&gscx__preload_cond);
    pthread_mutex_unlock(&gscx__preload_lock);
    pthread_join(gscx__preload_thread_tid, NULL);

	pthread_cond_destroy(&gscx__preload_cond);
	pthread_mutex_destroy(&gscx__preload_lock);

	lsize = iList.Size(scx__preload_list);
	for (i = 0; i < lsize; i++) {
		pinfo = *(preload_info_t **)iList.GetElement(scx__preload_list,i);
		pl_destroy_info(pinfo);
	}
	ret = iList.Finalize(scx__preload_list);

}

int
pl_add_preload(void *preq)
{
	nc_request_t 	*req = (nc_request_t *) preq;
	preload_info_t 	*pinfo = NULL;
	mem_pool_t 		*mpool = NULL;
	int				pool_size = gscx__config->pool_size;

	mpool = mp_create(pool_size);
	ASSERT(mpool);
	pinfo = (preload_info_t *)mp_alloc(mpool,sizeof(preload_info_t));
	ASSERT(pinfo);
	pinfo->mpool = mpool;
	pinfo->host = mp_alloc(mpool, vs_length(req->host) + 1);
	strncpy(pinfo->host, vs_data(req->host), vs_length(req->host));
	pinfo->url = mp_alloc(mpool, vs_length(req->ori_url) + 1);
	strncpy(pinfo->url, vs_data(req->ori_url), vs_length(req->ori_url));
	pinfo->id = req->id;
	pinfo->options = kv_create_pool_d(pinfo->mpool, mp_alloc, __FILE__, __LINE__);
	pl_clone_options(req->options, pinfo);
	/* 리스트에 추가 */
	pthread_mutex_lock(&gscx__preload_lock);
	iList.Add(scx__preload_list, (void *)&pinfo);
	pthread_cond_signal(&gscx__preload_cond);
	pthread_mutex_unlock(&gscx__preload_lock);
	TRACE((T_INFO, "[%llu] Add content(%s) to preload list.\n", req->id, vs_data(req->ori_url)));
	return 0;
}

/*
 * preload 상태를 확인 한다.
 * 리턴값
 *     -1 : preload를 할수 없는 상태(원본에 해당 파일이 없거나 오리진 응답코드가 300 이상인 경우)
 *     0 : 아직 프리로드를 시작 하지 않았거나 진행중임
 *     1 : 프리로드가 완료됨
 */
int
pl_preload_status(void *preq)
{
	nc_request_t 	*req = (nc_request_t *) preq;
	preload_info_t 	*pinfo = NULL;
	mem_pool_t 		*mpool = NULL;
	nc_file_handle_t 	*file = NULL;
	nc_stat_t 			objstat;
	mode_t	mode = O_RDONLY;
	service_info_t *service = NULL;
	int				pool_size = gscx__config->pool_size;
	int ret = 0;


	mpool = mp_create(pool_size);
	ASSERT(mpool);
	pinfo = (preload_info_t *)mp_alloc(mpool,sizeof(preload_info_t));
	ASSERT(pinfo);
	pinfo->mpool = mpool;
	pinfo->host = mp_alloc(mpool, vs_length(req->host) + 1);
	strncpy(pinfo->host, vs_data(req->host), vs_length(req->host));
	pinfo->url = mp_alloc(mpool, vs_length(req->ori_url) + 1);
	strncpy(pinfo->url, vs_data(req->ori_url), vs_length(req->ori_url));
	pinfo->id = req->id;
	pinfo->options = kv_create_pool_d(pinfo->mpool, mp_alloc, __FILE__, __LINE__);
	pl_clone_options(req->options, pinfo);
	TRACE((T_INFO, "[%llu] Preload status,  content(%s).\n", pinfo->id, pinfo->url));

	service = vm_add(pinfo->host, 1);
	if (service == NULL) {
		TRACE((T_INFO, "[%llu] Preload failed to find service(%s).\n", pinfo->id, pinfo->host));
		ret = -1;
		goto pl_preload_status_end;
	}
	internal_update_service_start_stat(service);
	if (service->core->mnt == NULL) {
		/* 오리진 설정이 되지 않은 볼륨으로 요청이 들어 오는 경우는 에러 처리 한다. */
		scx_error_log(req, "%s undefined origin. url(%s)\n", __func__, vs_data(req->ori_url));
		ret = -1;
		goto pl_preload_status_end;
	}
	if (0 == service->origin_request_type ) {
		mode &= !O_NCX_NORANDOM;
	}
	else {
		mode |= O_NCX_NORANDOM;
	}
	file = nc_open_extended2(service->core->mnt, pinfo->url, pinfo->url, mode, &objstat, pinfo->options);
	//WEON
	if (file) {
		__sync_add_and_fetch(&service->opened, 1);
	}
	if (NULL == file) {
		TRACE((T_INFO, "[%llu] Preload failed to open file. reason(%d).\n", pinfo->id, nc_errno()));
		ret = -1;
		goto pl_preload_status_end;
	}
	/* 오리진 코드가 200이나 206이 아닌 경우는 preload 실패로 판단한다. */
	if (objstat.st_origincode != 200 && objstat.st_origincode != 206) {
		TRACE((T_INFO, "[%llu] Preload failed. origin code(%d).\n", pinfo->id, objstat.st_origincode));
		ret = -1;
		goto pl_preload_status_end;
	}
	/* nc_open_extended2()에서 nc_stat.st_cached가 1인 경우는 full caching 상태이다. */
	if (1 == objstat.st_cached) {
		TRACE((T_INFO, "[%llu] Already cached file(%s).\n", pinfo->id, pinfo->url));
		ret = 1;
		goto pl_preload_status_end;
	}
	/* 여기 까지 진행된 경우는 아직 preload가 완료되지 않은 상태이다. */
	ret = 0;
pl_preload_status_end:
	if (file) nc_close(file, 0);
	//WEON
	if (file) {
		__sync_sub_and_fetch(&service->opened, 1);
	}
	if (service) internal_update_service_end_stat(service);
	/* 임시 할당 memory pool 삭제 */
	mp_free(mpool);
	return ret;
}

int
pl_destroy_info(preload_info_t *pinfo)
{
	mem_pool_t 		*mpool = NULL;
	mpool = pinfo->mpool;
	pinfo->mpool = NULL;
	mp_free(mpool);
	return 0;
}

int
pl_do_preload(preload_info_t *pinfo, char *buf, int bufsize)
{
	nc_file_handle_t 	*file = NULL;
	nc_stat_t 			objstat;
	mode_t	mode = O_RDONLY;
	service_info_t *service = NULL;
	int64_t copied = 0;
	int64_t remained = 0;
	off_t 	offset = 0;

	TRACE((T_INFO, "[%llu] '%s' Preload start.\n", pinfo->id, pinfo->url));
	service = vm_add(pinfo->host, 1);
	if (service == NULL) {
		TRACE((T_INFO, "[%llu] Preload failed to find service(%s).\n", pinfo->id, pinfo->host));
		goto pl_do_preload_end;
	}
	internal_update_service_start_stat(service);
	if (0 == service->origin_request_type ) {
		mode &= !O_NCX_NORANDOM;
	}
	else if (2 == service->origin_request_type ) {
		mode |= O_NCX_TRY_NORANDOM;
	}
	else {
		mode |= O_NCX_NORANDOM;
	}
	file = nc_open_extended2(service->core->mnt, pinfo->url, pinfo->url, mode, &objstat, pinfo->options);
	//WEON
	if (file) {
		__sync_add_and_fetch(&service->opened, 1);
	}
	if (NULL == file) {
		TRACE((T_INFO, "[%llu] Preload failed to open file. reason(%d).\n", pinfo->id, nc_errno()));
		goto pl_do_preload_end;
	}
	/* nc_open_extended2()에서 nc_stat.st_cached가 1인 경우는 full caching 상태이므로 skip 하도록 한다. */
	if (1 == objstat.st_cached) {
		TRACE((T_INFO, "[%llu] Already cached file(%s).\n", pinfo->id, pinfo->url));
		goto pl_do_preload_end;
	}
	remained = objstat.st_size;
	while (0 < remained && gscx__pl_working) {
		copied = nc_read(file, buf, (nc_off_t)offset, (uint64_t)bufsize);
		if (0 >= copied) {
			TRACE((T_INFO, "[%llu] Preload failed to read file(%s). reason(%d).\n", pinfo->id, pinfo->url, nc_errno()));
			goto pl_do_preload_end;
		}
		remained -= copied;
		offset += copied;
	}
	TRACE((T_INFO, "[%llu] '%s' Preload complete.\n", pinfo->id, pinfo->url));
pl_do_preload_end:
	if (file) nc_close(file, 0);
	//WEON
	if (file) {
		__sync_sub_and_fetch(&service->opened, 1);
	}
	if (service) internal_update_service_end_stat(service);
	return 0;
}

static int
pl_clone_property(const char *key, const char *val, void *cb)
{
	preload_info_t *pinfo = cb;
	int 			n;
	int 			len = 0;

	if (NULL == key) {
		TRACE((T_WARN, "[%llu] %s invalid key.\n", pinfo->id, __func__));
		return 0;
	}
	else if (NULL == val) {
		TRACE((T_WARN, "[%llu] %s invalid value.(key : %s)\n", pinfo->id, __func__, key));
		return 0;
	}
	TRACE((T_DAEMON, "[%llu] %s('%s', '%s')\n", pinfo->id,  __func__, key, val));

	kv_add_val_d(pinfo->options, (const char *)key, (const char *)val, __func__, __LINE__);

	return 0;
}

/*
 * req->options의 header 값을 pinfo->options에 복사한다.
 * req->options에 들어 있는 header중 ignore가 1인 header들은 kv_for_each()로는 복사가 되지 않지만
 * ignore가 1인 값들은 origin에 전달이 되지 않는 header이므로 상관이 없다.
 */
static int
pl_clone_options(nc_xtra_options_t *root, preload_info_t *pinfo)
{
	kv_for_each(root, pl_clone_property, pinfo);
	TRACE((T_DAEMON, "[%llu] %s()\n", pinfo->id, __func__));
	return 0;
}
