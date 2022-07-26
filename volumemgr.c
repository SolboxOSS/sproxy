#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/stat.h>
#include <search.h>
#include <gnutls/gnutls.h>
#include <pthread.h>
#include <gcrypt.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
//#include <ncapi.h>
//#include <trace.h>
#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include <search.h>
#include "scx_util.h"
#include "streaming.h"
#include "scx_list.h"
#include "module.h"
#include "scx_timer.h"
#include "standalonestat.h"

#include <dict.h>	/* libdict */

extern vstring_t	*gscx__config_root;
extern int			gscx__enable_async;
extern int			gscx__service_available;
/*
 * netcache core를 통해서 생성되는 volume의 관리
 * 생성, 
 * lookup
 */
int 		gscx__load_revision = 0;	/* 설정 loading 시마다 1씩 증가 */
uint32_t	gscx__purge_revision = 0; 	/* vm_all_volume_purge가 호출된 수 */
dict 		*scx__service_dct = NULL;
List		*scx__past_service_list = NULL;	/* reload 이전의 service 정보들을 가지고 있는 리스트 */
char		gscx__custom_user_agent[256];

/*
 * 일단 mutex로 구현
 * 향후, 성능 이슈가 있는 것 같으면, rwlock으로 변경 필요
 * (request한번 받을 때 마다 vm_lookup이 실행되는 것을 고려해야함,
 * rwlock의 readlock으로 vm_lookup을 실행하는 경우 parallel search
 * 가 가능할지도 모름.안되면 glibc의 hash table말고
 * 별도 구현 필요
 */
//pthread_mutex_t 		__services_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t  		__services_lock;
pthread_mutex_t 		__delay_config_lock;

int  vm_create_service(service_info_t *service, struct site_tag *site, int volume_type, service_core_t *core);
int  vm_destroy_service(service_info_t *service);
void vm_reset_applied_conf(service_core_t *core);
void vm_key_val_free(void *key, void *datum);
int  vm_service_init(service_info_t *service, struct site_tag *site, char *signature, int volume_type, service_core_t *core);
int  vm_netcache_config(service_info_t *service, char* signature);
int  vm_ad_netcache_config(service_info_t *service, char* signature);
int  vm_base_config(service_info_t *service);
void vm_update_origin(nc_request_t *req, vstring_t *origins);
int  vm_insert_domain_to_hash(service_info_t *service, const char *host);
int  vm_delete_domain_from_hash(service_info_t *service, const char *host);
scx_list_t *vm_parse_domain(site_t *site);
scx_list_t *vm_parse_string_by_token(mem_pool_t *mpool, vstring_t *string, const char *sep);
scx_list_t *vm_parse_sub_domain_list(service_info_t *service, vstring_t *string);
int vm_move_domain_hash_to_list(service_info_t *service);
int vm_restore_domain_hash_from_list(service_info_t *service);
int vm_clean_past_service(int revision);
void vm_delayed_site_destroy(void *data);

int
vm_init()
{
	pthread_rwlock_init(&__services_lock, NULL);
	pthread_mutex_init(&__delay_config_lock, NULL);
	scx__service_dct = hashtable_dict_new((dict_compare_func)strcmp, dict_str_hash, DICT_HSIZE);
    ASSERT(dict_verify(scx__service_dct));
    CurrentAllocator = &iCCLMalloc;	/* C Container Library의 memory 관련 callback function을 할당한다. */
#if 0
    scx__past_service_list = iList.Create(sizeof (service_info_t *));
#else
    scx__past_service_list = iList.CreateWithAllocator(sizeof (service_info_t *), &iCCLMalloc);
#endif
    snprintf(gscx__custom_user_agent, 256, "NetCache(%s-%s-%d_%d)", PROG_SHORT_NAME, PROG_VERSION, SVN_REVISION, CORE_SVN_REVISION);
	return 0;
}

int
vm_deinit()
{

	service_info_t *service = NULL;
	char	*host;
	dict_itor *itor = NULL;
	int ret = 0;
	int i, lsize = 0;
	int	alias_count = 0;
	pthread_rwlock_wrlock(&__services_lock);
	if (scx__service_dct) {
		TRACE((T_DAEMON, "%d running volumes, destroying...\n", dict_count(scx__service_dct)));
		itor = dict_itor_new(scx__service_dct);
		dict_itor_first(itor);
		while (dict_itor_valid(itor)) {
			service = (service_info_t *)*dict_itor_datum(itor);
			host = (char *)dict_itor_key(itor);
			dict_itor_next(itor);
			alias_count = vm_delete_domain_from_hash(service, host);
			if (0 == alias_count)
				vm_destroy_service(service);
		}
		dict_itor_free(itor);
		dict_free(scx__service_dct, vm_key_val_free);
		scx__service_dct = NULL;
		TRACE((T_INFO, "Virtual host destroyed complete\n"));
	}
	if (scx__past_service_list) {
		lsize = iList.Size(scx__past_service_list);
		for (i = 0; i < lsize; i++) {
			service = *(service_info_t **)iList.GetElement(scx__past_service_list,i);
			vm_destroy_service(service);
		}
		ret = iList.Finalize(scx__past_service_list);
		if (0 > ret) {
			TRACE((T_ERROR, "Service List remove failed.(reason : %s)\n", iError.StrError(ret)));
		}
		scx__past_service_list = NULL;
	}
	pthread_rwlock_unlock(&__services_lock);

	pthread_rwlock_destroy(&__services_lock);
	pthread_mutex_destroy(&__delay_config_lock);
	return 0;
}

/*
 * hash 리스트에서 현재의 revision과 다른 service(동적 도메인은 제외)들(service 설정이 없어진 경우)을
 * 백업 리스트로 옮긴후
 * 백업 리스트 중에 using_count가 0인 service 들을 삭제한다.
 */
int
vm_clean_past_service(int revision)
{

	service_info_t *service = NULL;
	char	*host;
	dict_itor *itor = NULL;
	int ret = 0;
	int i, lsize = 0;
	TRACE((T_DAEMON, "Clean Old Service info started.\n"));
	/* hash 리스트에서 현재의 revision과 다른 service들을 백업 리스트로 옮긴다.
	 * 이때 동적 service와 사용자가 있는 서비스들은 제외한다.
	 * 사용자가 있는 서비스를 백업 리스트로 옮기지 않는 이유는 사용자가 있는 상태에서 백업리스트로 옮기고
	 * 이후 reloading 시 동일한 service 설정이 생기는 경우 중복된 netcache mount가 생길수 있기 때문임 */
	if (scx__service_dct) {
		itor = dict_itor_new(scx__service_dct);
		dict_itor_first(itor);
		while (dict_itor_valid(itor)) {
			service = (service_info_t *)*dict_itor_datum(itor);
			dict_itor_next(itor);
			/* 동적 도메인이나 정적 도메인이라도 check_revision과 revision이 같은 경우(설정 파일이 바뀌지 않은 경우)는  경우는 옮기지 않는다. */
			if (service->revision != revision					/* 최신 도메인 제외 */
					&& VOLUME_STATIC == service->volume_type	/* 동적 도메인 제외 */
					&& revision != service->check_revision)	/* 설정 변경이 없는 서비스 제외 */
			{
				/* 여기에 들어 오는 경우는 설정이 삭제된 경우이다. */
				if (0 < service->using_count) {
					/* 설정이 삭제된 service라도 사용자가 연결 되어 있는 경우에는 백업 리스트로 옮기지 않고
					 * disable marking만 한다.
					 * 이렇게 번거로운 처리는 하는 이유는 사용중인 상태에서 백업리스트로 옮기면 바로 삭제 되지 않고
					 * netcache mount를 가지고 있게 되는데 이때 동일 설정이 새로 생성되는 경우
					 * netcache mount를 두군데서 가지게 되고 이후 백업리스트의 service에 연결된 사용자가 종료 되는 경우
					 * netcache mount를 삭제 하는데 이렇게 되면 새로 생성된 서비스의 mount도 삭제가 되어 문제가 발생한다.
					 * 이를 피하기 위해 disable marking만 하고 혹시 삭제 이전에 동일 service가 새로 생성되면
					 * disable만 enable로 갱신 하여 주면 된다.
					 */
					service->available = 0;
					service->st_size = 0; /* 동일 설정이 새로 생성되었을때 현재 service를 백업으로 보내기 위해 size 를 리셋 */
				}
				else {
					if (vm_move_domain_hash_to_list(service) > 1) {
						/*
						 * vm_move_domain_hash_to_list()의 리턴값이 1 보다 큰 경우는 해당 서비스의 도메인이 두개 이상인 경우라서
						 * 나머지 도메인도 scx__service_dct에서 제거가 된 상태임
						 * 이 상태에서 dict_itor_next()로 가지고온 itor값은 이미 scx__service_dct에서 빠진 값일수도 있기 때문에 처음부터 다시 한다.
						 */
						dict_itor_first(itor);
					}
				}
			}
		}
		dict_itor_free(itor);
	}

	/* 백업 리스트에 있는 service 중에 사용중이 아닌것들을 삭제 한다. */
	lsize = iList.Size(scx__past_service_list);
	for (i = 0; i < lsize;) {
		service = *(service_info_t **)iList.GetElement(scx__past_service_list,i);
#if 1
		if (0 == service->using_count) {
#else
		if (0 == service->using_count && service->revision < (revision-1)) {
			/*
			 * 설정 reload 과정에서 통계 모듈에서 삭제되는 볼륨에 대한 통계를 기록중 일수 있기 때문에
			 * 삭제가 된 볼륨이라도 바로전 revision의 볼륨들은 vm_destroy_service()를 호출하지 않는다.
			 */
#endif
			vm_destroy_service(service);
			ret = iList.EraseAt(scx__past_service_list, i);
			if (0 > ret) {
				TRACE((T_ERROR, "past list Erase() failed.(%s)\n", iError.StrError(ret)));
				i++;
				ASSERT(0); /* 어떤때 발생하는지 모르므로 일단 멈춘다. */
			}
			lsize = iList.Size(scx__past_service_list);
		}
		else {
			TRACE((T_DAEMON, "service(%s) clean delayed. using(%d)\n", vs_data(service->name), service->using_count));
			i++;
		}
	}
	TRACE((T_DAEMON, "Clean Old Service info finished.\n"));
	return 0;
}


/*
	core : 처음 볼륨이 생성되는 경우 NULL, reload되는 경우는 기존 service->core
*/
int
vm_create_service(service_info_t *service, struct site_tag *site, int volume_type, service_core_t *core)
{
	int result = 0;
	int domain_cnt = 0;
	char *domain = NULL;
	int	i = 0;
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};

	domain = scx_list_get_by_index_key(service->domain_list, 0);	/* domain list의 첫 domain을 service key로 사용한다. */
	result = vm_service_init(service, site, domain, volume_type, core);
	if (result < 0)	{
		// 여기서 에러가 리턴 되는 경우는 없음.
		TRACE((T_ERROR, "service configuration for '%s' - failed to init.\n", domain));
		return SCX_NO;
	}
	result = vm_base_config(service);
	if (result < 0)	{
		TRACE((T_ERROR, "service configuration for '%s' - failed to base configuration.\n", domain));
		return SCX_NO;
	}

	if (scx_get_vstring(service->site, SV_ORIGIN, NULL) != NULL ) {
		result = vm_netcache_config(service, domain);
		if(result < 0) {
			TRACE((T_ERROR, "service configuration for '%s' - failed to netcache create.\n", domain));
			return SCX_NO;
		}
	}
	/*
	 * 오리진 설정이 없어도 되는 경우는 현재 아래 3가지 경우이다.
	 * 1. CMAF 기능 사용시
	 * 2. local file 기능 사용시
	 * 3. 대표 도메인이나 FHS 도메인인 경우
	 * 이 외에는 에러 처리한다.
	 */
	else if (service->streaming_method == 2 ) {
		/* CMAF 기능 사용시는 cmaf library에서 오리진 연결을 진행 하기 때문에 NetCache 관련 동작을 하지 않는다. */
		// 여기에서 호출하는 함수에서 solCMAF_create()와 solCMAF_connect()를 실행해야 한다.
		// 볼륨 삭제시의 처리 방안도 같이 만들어야 한다.
	}
	else if (1 == service->use_local_file) {
		/*
		 * local file 기능 사용시는 vm_netcache_config()는 실행할 필요가 없다.
		 * local file에서 netcache 사용으로 설정 변경시는 ?
		 * */
	}
	else if (service->vol_service_type > 0) {
		/*
		 * 대표 도메인이나 FHS 도메인인 경우에도 오리진을 사용하지 않는다.
		 */
	}
	else {
		result = vm_netcache_config(service, domain);
		if(result < 0) {
			TRACE((T_ERROR, "service configuration for '%s' - failed to netcache create.\n", domain));
			return SCX_NO;
		}
	}

	/************************** Volume Create Phase Handler가 실행 되는 부분 *************************************/
	phase_ctx.service = (void *)service;
	if (NULL == core) {	/* 볼륨이 reload가 아닌 처음 생성되는 경우만 핸들러가 실행된다. */
		if (SCX_YES != scx_phase_handler(PHASE_VOLUME_CREATE, &phase_ctx)) {
			TRACE((T_ERROR, "service configuration for '%s' - volume create phase handler error.\n", domain));
			return SCX_NO;
		}
	}
	else {	/* 볼륨이 reload 되는 경우 */
		if (SCX_YES != scx_phase_handler(PHASE_VOLUME_RELOAD, &phase_ctx)) {
			TRACE((T_ERROR, "service configuration for '%s' - volume create phase handler error.\n", domain));
			return SCX_NO;
		}
	}
	return SCX_YES;
}

int
vm_destroy_service(service_info_t *service)
{
	phase_context_t phase_ctx = {NULL/* service */, NULL/* req */, NULL/* addr */};
	site_t *site = NULL;
	ASSERT(service);
#if 0
	if (NULL == service->site) /* alias 볼륨 때문에 이전에 이미 삭제가 된 경우는 그냥 리턴한다. */
		return 0;
#else
	ASSERT(service->site);
#endif
	if (service->core) {	/* reload시 service 정보가 갱신만 되는 경우는 core정보가 지워지면 안된다. */
		service->core->service_count -= 1;
		ASSERT(0 <= service->core->service_count);
		if (0 == service->core->service_count){
	/************************** Volume Destroy Phase Handler가 실행 되는 부분 *************************************/
			/* reload 되는 경우에 phase handler가 호출 되는것을 막기 위해 이부분에서 핸들러를 호출한다. */
			phase_ctx.service = (void *)service;
			if (SCX_YES != scx_phase_handler(PHASE_VOLUME_DESTROY, &phase_ctx)) {

			}
			if(service->core->mnt && service->core->is_alias_mnt == 0) {
				nc_ioctl(service->core->mnt, NC_IOCTL_CACHE_MONITOR, NULL, sizeof(void *));
				nc_ioctl(service->core->mnt, NC_IOCTL_ORIGIN_MONITOR, NULL, sizeof(void *));
				nc_ioctl(service->core->mnt, NC_IOCTL_ORIGIN_MONITOR2, NULL, sizeof(void *));
				nc_ioctl(service->core->mnt, NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER, NULL, sizeof(void *));
				nc_ioctl(service->core->mnt, NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER, NULL, sizeof(void *));

				TRACE((T_DAEMON, "service(%s) unmounted.\n", vs_data(service->name)));
				//WEON
				ASSERT(__sync_sub_and_fetch(&service->opened, 0) == 0);
				nc_destroy_mount_context(service->core->mnt);
				if (nc_errno() < 0)	{
					TRACE((T_WARN, "virtual host connection for '%s' - failed to destroy mount(%d)\n", vs_data(service->name), nc_errno()));
				}
				service->core->mnt = NULL;
			}
#ifdef ZIPPER
			if(service->core->ad_mnt) {
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_CACHE_MONITOR, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_ORIGIN_MONITOR, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_ORIGIN_MONITOR2, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER, NULL, sizeof(void *));

				TRACE((T_DAEMON, "service(%s) Advertizement origin unmounted.\n", vs_data(service->name)));
				//WEON
				ASSERT(__sync_sub_and_fetch(&service->opened, 0) == 0);
				nc_destroy_mount_context(service->core->ad_mnt);
				if (nc_errno() < 0)	{
					TRACE((T_WARN, "virtual host for '%s' - failed to destroy Advertizement mount(%d)\n", vs_data(service->name), nc_errno()));
				}
				service->core->ad_mnt = NULL;
			}
#endif
			if(service->core->cmaf) {
				strm_cmaf_destroy(service);
				service->core->cmaf = NULL;
			}
			SCX_FREE(service->core->conf.origins);
			SCX_FREE(service->core->conf.parents);
			SCX_FREE(service->core->conf.base_path);
			SCX_FREE(service->core->conf.proxy_address);
			SCX_FREE(service->core->conf.origin_health_check_url);
			SCX_FREE(service->core->conf.ad_origins);
			SCX_FREE(service->core->conf.ad_base_path);
			SCX_FREE(service->core);
			service->core = NULL;
		}
	}

	TRACE((T_DAEMON, "service(%s) released. alias count(%d)\n", vs_data(service->name), service->alias_count));

	if (service->site) {
		site = service->site;
		if (gscx__service_available == 0) {
			/* 데몬 종료시는 바로 메모리를 해제 한다. */
			site->service = NULL;
			service->site = NULL; /* memory pool이 지워지면 service가 이미 메모리에서 해제가 되기 때문에 만약을 대비해서 여기서 NULL을 설정한다. */
			scx_site_destroy(site);
		}
		else {
			service->timer = (void *)mp_alloc(site->mp, sizeof(bt_timer_t));
			bt_init_timer((bt_timer_t *)service->timer, "site free timer", 0);
			bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)service->timer, 60 * 1000 , vm_delayed_site_destroy, (void *)service);
			TRACE((T_DAEMON, "delayed site(%s) set timer.\n", vs_data(service->name)));
		}
	}
	return 0;
}

/*
 * site memory를 바로 해제하는 경우 netcache에서 callback이 늦게 호출 되는 경우 죽는 문제가 발생해서
 * 바로 해제를 하지 않고 일정 시간이 지난후 해제 하도록 한다.
 */
void
vm_delayed_site_destroy(void *data)
{
	service_info_t *service = (service_info_t *)data;
	site_t *site = service->site;

#ifdef BT_TIMER_VER2
		while (bt_destroy_timer_v2(gscx__timer_wheel_base, (bt_timer_t *)service->timer) < 0) {
			bt_msleep(10);
		}
#else
	bt_destroy_timer(gscx__timer_wheel_base, (bt_timer_t *)service->timer);
#endif
	TRACE((T_DAEMON, "delayed site(%s) destroy called.\n", vs_data(service->name)));
	site->service = NULL;
	service->site = NULL;
	scx_site_destroy(site);
}

/*
 * NetCache Core에서 Origin 주소를 변경하는 경우 기존 설정이 모두 초기화 되는 문제가 발생한다.
 * 이를 해결하기 위해 오리진 주소(origin, parent, ad_origin)가 변경 되는 경우 모든 설정을 다시 하도록 한다.
 * 오리진 주소 변경시 모든 설정을 다시 하기 위해 기존 설정들을 모두 초기화 한다.
 * 주소 변경시 마다 이 함수가 호출 되어야 한다.
 * origin, parent, ad_origin은 제외
 * base_path를 초기화 하면 설정이 있다가 없어지는 경우에 대응이 되지 않아서 초기화를 하지 않는다.
 */
void
vm_reset_applied_conf(service_core_t *core)
{
	SCX_FREE(core->conf.parents);
	core->conf.parents = NULL;
	SCX_FREE(core->conf.proxy_address);
	core->conf.proxy_address = NULL;
	SCX_FREE(core->conf.origin_health_check_url);
	core->conf.origin_health_check_url = NULL;
	core->conf.is_base_path_changed = 1;
	core->conf.origin_protocol = -1;
	core->conf.origin_policy = -1;
	core->conf.parent_policy = -1;
	core->conf.writeback_chunks = 0;
	core->conf.origin_connect_timeout = 0;
	core->conf.origin_transfer_timeout = 0;
	core->conf.origin_max_fail_count = 0;
	core->conf.origin_online_code_rev = 0;
	core->conf.negative_ttl = 0;
	core->conf.positive_ttl = 0;
	core->conf.follow_redir = -1;
	core->conf.dns_cache_ttl = 0;
	core->conf.ad_origin_protocol = -1;
	core->conf.is_ad_base_path_changed = 1;
	core->conf.ad_writeback_chunks = 0;
	core->conf.ad_origin_connect_timeout = 0;
	core->conf.ad_origin_transfer_timeout = 0;
	core->conf.ad_nc_read_timeout = 0;
	core->conf.ad_origin_policy = -1;
	core->conf.ad_follow_redir = -1;
	core->conf.ad_origin_online_code_rev = 0;
	core->conf.ad_negative_ttl = 0;
	core->conf.ad_positive_ttl = 0;
	return;
}

void
vm_key_val_free(void *key, void *datum)
{

}


/*
 * service 객체 init 처리
 * 호출전에 반드시 service, site 객체가 생성되어 있어야 한다. 
*/
int
vm_service_init(service_info_t *service, struct site_tag *site, char *signature, int volume_type, service_core_t *core)
{
	off_t st_size; // 인증서 파일의 크기를 저장
	if (!service || !site || !signature)
	{
		TRACE((T_ERROR, "service configuration for '%s' - failed to apply\n", signature));
		return -1;
	}

	service->name = vs_strdup_lg(site->mp, signature);
	service->site = site;
	service->volume_type = volume_type;
	pthread_mutex_init(&service->lock, NULL);
	service->current_origin_ver = 1;
	service->keepalive_timeout = 0;
	service->hls_target_duration = 10;
	service->quick_start_seg = 0;
	service->session_enable = 1;
	service->session_timeout = 30;
	service->cmaf_origin_timeout = 5;
	service->cmaf_origin_disconnect_time = 300;
	service->cmaf_origin_connect_onload = 0;
	service->cmaf_origin_retry_interval = 2;
	service->cmaf_origin_retry_max_count = 0;
	service->cmaf_fragment_cache_count = 10;
	service->cmaf_prefetch_fragment_count = 0;
	service->cmaf_manifest_fragment_count = 5;
	service->dash_manifest_use_absolute_path = 0;
	service->dash_manifest_protocol = UNDEFINED_PROTOCOL;
	service->hls_manifest_compress_enable = 0;
	service->available = 1;
	if (NULL != core) {
		/* reload시 기존 설정이 있는 경우에는 core의 할당이 필요 없다. */
		service->core = core;
	}
	else {
		service->core = SCX_CALLOC(1, sizeof(service_core_t));
	}
	service->core->service_count += 1;
	service->revision = gscx__load_revision;
	ASSERT(service->core);
#if GNUTLS_VERSION_MAJOR >= 3
	/* static 볼륨에 대해서만 인증서를 loading 한다. */
	if (service->volume_type == VOLUME_STATIC)
	{
		service->certificate_crt_path = scx_get_vstring(service->site, SV_CERTIFICATE_CRT, NULL);
		service->certificate_key_path = scx_get_vstring(service->site, SV_CERTIFICATE_KEY, NULL);
		if (service->certificate_crt_path && service->certificate_key_path) {
			service->site->certificate = scx_sni_load_keys(service->site, vs_data(service->certificate_crt_path), vs_data(service->certificate_key_path));
			if (service->site->certificate != NULL) {
				service->hash_crt  = vm_make_config_hash(vs_data(service->certificate_crt_path), &st_size);
				service->hash_key  = vm_make_config_hash(vs_data(service->certificate_key_path), &st_size);
			}
		}
		else {
			service->certificate_crt_path = NULL;
			service->certificate_key_path = NULL;
		}
	}
#endif

	return 0;
}

/*
 *
 * vm_create_config() : volumn 에 대한 설정 디렉토리의 모든 파일 대상인지 확인한다.
 * 확인 제외 대상은 검사하지 않는다. (설정 파일이 아닌 것은 굳이 검사할 필요가 없기 때문) 
 * 실행 결과가 비 정상적인 경우에는 종료한다.
 *
 *  확인 제외 대상 
 *  1. 파일명이 "default.conf" 는 확인 안함 (domain 생략가능하기 때문) 
 *
 * return value
 *  null : error
 *  not null : success (service point)
 *
 * FLOW
 * 0. 최초 기동시에만 호출된다.
 * 1. 설정파일을 모두 읽는다.
 * 2. site 객체를 파싱한다.
 * 3. 파싱된 객체를 검증한다. (중복등 체크, 에러발생시 종료처리)
 * 4. site create 파싱된 객체를 이용해 초기화를 진행한다. (모든 볼륨을 loading)
 * 5. 기동완료
 *
 * is_reload가 0인 경우 함수 내부에서 lock을 사용
*/

/*
 * reload 동작 정리
 * default.conf를 제외한 다른 설정들을 모두 reload 한다.
1. service lock
2. revision 카운터 증가
3. 설정파일 한개를 읽음
3-1. 새로 읽은 설정파일의 도메인과 동일한 service가 있는 경우(설정 변경)
- 기존 service를 백업 list로 옮김(백업 list는 alias 도메인이 여러개라도 한개의 service만 만든다. )
- site_tag에서 sub 도메인이 있으면 main list에서 모두 삭제(domain 항목의 순서가 바뀌는 상황 고려해야함).
- 새로 생성된 서비스는 service_core_t를 새로 생성하지 않고 기존의 포인터를 그대로 사용.
- nc_create_mount_context() 대신 nc_ioctl(NC_IOCTL_UPDATE_ORIGIN) 사용
3-2. 기존 설정이 없는 경우는(새로 추가된 경우) 기존과 동일 하게 service 생성
3-3. 에러가 발생하면  main list에서 최신 revision의 service를 모두 삭제하고 백업 list에서 이전 revision의 service를 main list로 옮김(rollback)
- 이때 vm_netcache_config()을 다시 실행.
- nc_create_mount_context() 대신 nc_ioctl(NC_IOCTL_UPDATE_ORIGIN) 사용
- 모든 alias 도메인을 hash에 다시 등록한다.
4. 모든 파일을 읽은 경우 main list를 순차적으로 검사해서 static 볼륨이면서 revision이 이전인 service들(service가 삭제된)을 모두  백업 list로 옮김. 이때도 subdomain에 대한 처리를 해야함.
5. 정상적으로 모든 볼륨을 loading이 끝난 상태이면 delete list에서 사용자가 0인 서비스들을 찾아서 모두 삭제함.
6. service unlock
*/
int
vm_create_config(int is_reload)
{
#define FILENAME_LENGTH 512

	int				ret = 0;

	char			szFileName[FILENAME_LENGTH] = { 0, };
	char*			szFileExt = NULL;
	char			szFileDir[FILENAME_LENGTH] = { 0, };
	char			szFilePath[FILENAME_LENGTH] = { 0, };

	DIR*			pConfDir = NULL;
	struct dirent*	pEntryDir = NULL;
	struct stat		st;

	site_t*			site = NULL;
	service_info_t	*service = NULL;
	service_info_t	*found_service = NULL;
	service_info_t	*last_found_service = NULL;
	off_t 			off = 0;
	int 			result = 0;

	scx_list_t 		*domain_root = NULL;
	int 			domain_cnt = 0, i;
	char	 		*domain = NULL;
	uint32_t		hash;		/* 설정 파일의 hash값 */
	uint32_t		cert_hash;	/* 인증서 파일의 hash값 */
	off_t			st_size;	/* 설정 파일의 크기 */
	off_t			cert_size;	/* 인증서 파일의 크기 */
	scx_list_t 		*alias_vol_list = NULL;	/* alias origin을 사용하는 볼륨의 path를 저장 */
	int				phase = 1;
	int				alias_cnt = 0;
	char 			*tmp_path = NULL;
	dict_itor 		*itor = NULL;

	static volatile int loading_progress = 0;
	int		prev_revision = 0;
	pthread_rwlock_wrlock(&__services_lock);
	if (0 != loading_progress) {
		TRACE((T_WARN, "Another reload process is working!\n"));
		pthread_rwlock_unlock(&__services_lock);
		return 0;
	}
	loading_progress = 1;
	pthread_rwlock_unlock(&__services_lock);
	TRACE((T_INFO|T_STAT, "***************** Configuration %s started. *********************\n", is_reload?"reload":"load" ));

	prev_revision = gscx__load_revision++;	/* lock 상태에서 revision 연산을 하기 때문에 atomic function을 사용하지 않는데 lock을 벗어 나는 경우가 생기면 atomic을 변환 해야 한다. */

	snprintf(szFileDir, sizeof(szFileDir), "%s", vs_data(gscx__config_root));

	if ((pConfDir = opendir(szFileDir)) == NULL) {
		TRACE((T_ERROR, "couldn't open '%s'\n", szFileDir));
		loading_progress = 0;
		return -1;
	}
	/* alias origin을 사용하는 볼륨을 임시로 저장할 리스트를 생성한다. */
	alias_vol_list = scx_list_create(NULL, LIST_TYPE_SIMPLE_DATA, 1);
	/* 1. 설정 파일을 모두 읽는다. */
	while (1) {
read_next_config:
		service = NULL;
		last_found_service = NULL;
		site = NULL;
		if (phase == 1) {
			/* service 를 추가 등록할 수 있는지 개 수 확인 */
			if (0 == is_reload) {
				/* reload인 경우에는 service 개수 확인을 하지 않는다. */
				if (dict_count(scx__service_dct) >= gscx__config->max_virtual_host)
				{
					/* 이미 허용 볼륨 수를 초과한 상태이기 때문에 더 이상 볼륨을 load하지 않고 중단 한다 */
					TRACE((T_ERROR, "reaches the allowable max(%d) service count!\n", gscx__config->max_virtual_host));
					break;
				}
			}
			if ((pEntryDir = readdir(pConfDir)) == NULL) {
				/* 읽을 파일이나 디렉토리가 없는 경우 */
				//break;
				/* alias 볼륨 처리를 하기 위해 phase를 변경한다 */
				phase = 2;
				alias_cnt = scx_list_get_size(alias_vol_list);
				continue;
			}

			memset(&st, 0x00, sizeof(st));

			/* 디렉토리 제외 */
			result = stat(pEntryDir->d_name, &st);
			if ((result >= 0 && S_ISDIR(st.st_mode) == 1)) {
				continue;
			}

			/* "default.conf" 파일 제외 */
			if (strncmp(pEntryDir->d_name, "default.conf", 12) == 0) {
				continue;
			}
			/* ".conf" 파일 제외 */
			else if (strncmp(pEntryDir->d_name, ".conf", 5) == 0) {
				continue;
			}
			/* 확장자가 .conf가 아닌 파일 제외 */
			szFileExt = strrchr(pEntryDir->d_name, '.');
			if ( NULL == szFileExt || strncmp(szFileExt, ".conf", 5) != 0) {
				continue;
			}

			memset(szFilePath, 0x00, FILENAME_LENGTH);
			snprintf(szFilePath, FILENAME_LENGTH, "%s/%s", szFileDir, pEntryDir->d_name);
		}
		else if (phase == 2) {
			if (alias_cnt <= 0) {
				/* alias origin을 사용하는 볼륨이 다 처리된 상태이거나 한개도 없는 경우임 */
				break;
			}
			/* alias_vol_list의 마지막 볼륨 부터 처리한다. */
			tmp_path = (char *)scx_list_get_by_index_data(alias_vol_list, alias_cnt - 1);
			alias_cnt -= 1;
			memset(szFilePath, 0x00, FILENAME_LENGTH);
			sprintf(szFilePath,  "%s", tmp_path);
		}
		hash = vm_make_config_hash(szFilePath, &st_size);
		if (0 == hash) {
			/* 설정 파일에 대한 hash 생성 실패시 해당 설정 파일은 skip 한다. */
			TRACE((T_DAEMON, "Configuration file('%s') make hash failed.\n", szFilePath));
			continue;
		}
		else if (st_size < 20)
		{
			/* 설정 파일의 크기가 20 byte 보다 적은 경우는 비정상 파일로 판단해서 skip 한다. */
			TRACE((T_ERROR, "Configuration file('%s') size too small(%d) \n",szFilePath, st_size));
			continue;
		}
		TRACE((T_DEBUG, "Configuration file('%s') hash = %u.\n", szFilePath, hash));
		/* 설정파일에서 site 객체를 파싱한다. */
		site = scx_site_create(szFilePath);
		if (site == NULL) {
			/*
			 * 설정 파일이 없는 경우
			 * 1. 초기 실행시
			 *   - 종료 처리한다.
			 * 2. 운영중에는
			 *   - 이런 경우를 dynamic 볼륨이라고 하며, scx_site_clone() 으로 생성한다.
			 *   - 단, scx_sni_handle() 에서 호출 되는 dynamic 볼륨인 경우에는 site 를 생성하지 않는다.
			*/
			TRACE((T_ERROR, "site(%s) configuration failed.\n", pEntryDir->d_name));
			continue;
		}
		if (phase == 1) {
			if (scx_get_vstring(site, SV_MASTER_CACHE_DOMAIN, NULL) != NULL) {
				/* alias origin이 지정된 볼륨의 경우는 리스트에 해당 파일의 경로를 저장하고 scx_site_destroy()를 호출한다. */
				scx_site_destroy(site);
				scx_list_append(alias_vol_list, NULL, szFilePath, strlen(szFilePath)+1);
				continue;
			}
		}

		domain_root = vm_parse_domain(site);
		if (NULL == domain_root) {
			TRACE((T_ERROR, "domain configuration for '%s' - not found\n", vs_data(site->name)));
			scx_site_destroy(site);
			continue;
		}
		/* 해당 서비스에 있는 도메인중 이미 등록되어 있는 도메인이 있는지 먼저 확인 한다. */

		domain_cnt = scx_list_get_size(domain_root);
		for(i = 0; i < domain_cnt; i++) {
			domain = scx_list_get_by_index_key(domain_root,i);

			found_service = vm_lookup(domain);
			if (found_service != NULL) {
				/*
				 * 이미 해당 도메인이 등록되어 있는 경우
				 * 처음 load : 에러발생.(도메인 중복 상황)
				 * reload :
				 *        파일이 변경되지 않았으면 : site를 지우고 다음 설정 파일을 읽음.
				 *        found_service가 현재 revision : 에러 발생.(도메인 중복 상황)
				 *        found_service가 이전 revision : 해당 service에 속해 있는 hash들을 모두 지우고 service를 백업 리스트로 이동
				 *
				 * TODO : 동적 도메인이 생성되어 있는 상태에서 reload를 할 경우에 대한 처리가 필요하긴 하지만 이건 나중에 함..
				 */
				TRACE((T_DEBUG, "Service(%s) old(size = %lld, hash = %u), new(size = %lld, hash = %u).\n",
							vs_data(found_service->name), found_service->st_size, found_service->hash, st_size, hash));
				if (found_service->st_size == st_size && found_service->hash == hash) {
					/* 설정 파일이 변경되지 않은 경우 */
					if (found_service->certificate_crt_path && found_service->certificate_key_path) {
						/* 인증서를 사용하는 사용하는 경우 인증서 파일의 변경 여부를 따로 확인 한다. */
						cert_hash = vm_make_config_hash(vs_data(found_service->certificate_crt_path), &cert_size);
						if (cert_hash != found_service->hash_crt) {
							/* 인증서 변경도 볼륨 설정 변경과 동일하게 처리한다. */
							last_found_service = found_service;
							break;
						}
						cert_hash = vm_make_config_hash(vs_data(found_service->certificate_key_path), &cert_size);
						if (cert_hash != found_service->hash_key) {
							/* 인증서 변경도 볼륨 설정 변경과 동일하게 처리한다. */
							last_found_service = found_service;
							break;
						}
					}

					scx_site_destroy(site);
					found_service->check_revision = gscx__load_revision;
					TRACE((T_DAEMON, "service(%s) not changed.\n", vs_data(found_service->name)));
					goto read_next_config;
				}
				else if (found_service->revision == gscx__load_revision) {
					/* 처음 load시점이나 현재의 revision과 동일한 버전의 서비스가 있는 경우(도메인 중복) */
					TRACE((T_ERROR, "'%s' in '%s' is already registered domain.\n", domain, vs_data(site->name)));
					scx_site_destroy(site);
					goto read_next_config;
				}
				else {
					/* 볼름 설정이 변경되는 경우임 */
					last_found_service = found_service;
					/*
					 * 현재는 도메인이 서비스를 옮겨 다니는 경우에 죽는 현상이 발생함
					 * 아래의 break를 지우면 도메인이 다른 볼륨으로 옮겨 갔을때 죽는 현상중 일부는 해결이 되지만 전체가 되는것은 아니기 때문에 현 상태를 유지한다.
					 * https://jarvis.solbox.com/redmine/issues/32882 일감 참고
					 */
					break;

				}
			}
		}	/* end of for(i = 0; i < domain_cnt; i++) */
		/* 여기에 들어 오는 경우는 처음 로딩시나 볼륨이 새로 추가되는 경우 여야 한다. */
		service = (service_info_t *)mp_alloc(site->mp, sizeof(service_info_t));
		service->domain_list = domain_root;
		site->service = (void *) service;	/* origin callback에서 사용하기 위한 목적 */
		if (SCX_YES != vm_create_service(service, site, VOLUME_STATIC, last_found_service?last_found_service->core:NULL)) {
			TRACE((T_ERROR, " failed to create service (%s)\n", vs_data(site->name)));
			goto L_failed_create_service;
		}

		service->st_size = st_size;
		service->hash = hash;

		pthread_rwlock_wrlock(&__services_lock);
		if (last_found_service != NULL) {
			/* 설정이 변경되는 경우 */
			vm_move_domain_hash_to_list(last_found_service);
		}
			/* 도메인들을 hash에 등록한다. */
		for(i = 0; i < domain_cnt; i++) {
			domain = scx_list_get_by_index_key(service->domain_list,i);
			ASSERT(domain);
			result = vm_insert_domain_to_hash(service, domain);
			ASSERT(0 == result);	/* 위에서 도메인이 hash에 없는것을 확인 했기 때문에 result가 -1이 나오면 논리 오류임 */
		}
		pthread_rwlock_unlock(&__services_lock);

		continue;
		/* 아래는 vm_create_service()에서 실패를 리턴하는 경우 호출 되는 부분임 */
L_failed_create_service:
		pthread_rwlock_wrlock(&__services_lock);
		if (service) {
			vm_destroy_service(service);
			service = NULL;
		}
		if(last_found_service != NULL) {
			// 이전에 해당 볼륨이 서비스 중인 경우는 원상태로 롤백한다.
			vm_restore_domain_hash_from_list(last_found_service);
			last_found_service = NULL;
		}
		pthread_rwlock_unlock(&__services_lock);
				//////////////////////////////////////////////////////////////////////////
	}	/* end of while (1) loop */

L_create_context_done:
	standalonestat_wlock();
	pthread_rwlock_wrlock(&__services_lock);
	/* 정상적으로 설정 loading이 끝난 경우 */
	if (1 == is_reload) {
		/* service 설정이 있다가 없어지나 갱신이 된 service들을 정리 한다. */
		vm_clean_past_service(gscx__load_revision);
	}

	if (pConfDir) closedir(pConfDir);
	if (alias_vol_list != NULL) {
		scx_list_destroy(alias_vol_list);
	}
	/* 통계에 사용될 볼륨 목록을 복사하는 부분 */
//	if (gscx__config->stat_write_enable == 1 ) { //list를 openapi에서도 사용하기 때문에 통계를 기록하지 않아도 list를 만든다.
		/* 기존 통계 목록을 지운다. */
		standalonestat_service_clear_all();

		itor = dict_itor_new(scx__service_dct);
		dict_itor_first(itor);
		while (dict_itor_valid(itor)) {
			service = (service_info_t *)*dict_itor_datum(itor);
			domain = (char *)dict_itor_key(itor);
			dict_itor_next(itor);

			if (service->alias_count > 1) {
				/*
				 * service->alias_count가 2개 이상인 경우(복수의 domain으로 서비스 하는 경우)에는 domain 설정에 두개 이상의 domain이 등록된 경우이다.
				 * 이 경우 첫번째 도메인이 service->name으로 지정되기 때문에 dict_itor_key()나온 host와 같은 것들만 통계용 서비스 목록에 넣는다.
				 */
				if (strcmp(domain, vs_data(service->name)) != 0) {
					TRACE((T_DAEMON, "domain(%s) has been skiped to the stat list.\n", domain));
//					printf("domain(%s) has been skiped to the stat list.\n", domain);
					continue;
				}
			}
			standalonestat_service_push(service);

		}
		dict_itor_free(itor);
//	}
	pthread_rwlock_unlock(&__services_lock);
	standalonestat_unlock();
	/*
	 * 모든 과정이 끝나야 다음 reload를 허용한다.
	 * 여기까지 진행이 안된 상태에서 reload가 반복적으로 들어 오는 경우 해당 reload 요청은 무시한다.
	 */
	loading_progress = 0;
			TRACE((T_INFO|T_STAT, "***************** Configuration %s finished. *********************\n", is_reload?"reload":"load" ));


	return ret;
}


/*
 * 	originaddr 오리진 서버 주소, signature로도 사용
 *  devclass NULL인 경우 HTTP를 기본으로 가정
 *  pttl positive TTL (초)
 *  nttl negative TTL (초)
 *  mnt가 있는지 찾아 보고 있으면 해당 mnt를 리턴하고 없는 경우 새로 생성한다.
 *  only_static는 static 볼륨과 dynamic 볼륨에대해 다른 동작이 필요 할때 정의 한다.
 *
 * 실제 domain 작업을 수행한다. (VOLUME_STATIC and VOLUME_DYNAMIC)
 * 기동시 vm_create_config() 를 통해 모든 도메인 및 alias가 이미 등록되었다.
 * 호출시 도메인 및 alias 가 등록되어 있지 않는 경우를  dynamic 볼륨이라고 한다.
 */
service_info_t *
vm_add(const char *host, int only_static)
{
	int				ret = 0;

	site_t*			site = NULL;
	service_info_t	*service = NULL;

	int 			result = 0;
	int				volume_type = VOLUME_DYNAMIC;
	scx_list_t 		*domain_root = NULL;
	int 			domain_cnt = 0, i;
	char	 		*domain = NULL;


	//pthread_rwlock_rdlock(&__services_lock);



	if (host)
	{
		/* 여기에만 lock을 거는 방식으로 변경 */
		pthread_rwlock_rdlock(&__services_lock);
		service = vm_lookup(host);
		pthread_rwlock_unlock(&__services_lock);
		if (service) {
			/* host에 해당하는 volume을 찾은 경우. */
//			TRACE((T_ERROR, "'%s' is already registered (type=%d)\n", host, volume_type));
//			ret = -1;
			return service;
		}

		if (only_static) {
			/* scx_sni_handle()에서 호출 되는 경우에는 dynamic 볼륨인 경우 site 생성을 하지 않는다. */
			return NULL;
		}

		if (gscx__config->allow_dynamic_vhost == 0) {
			TRACE((T_DAEMON, "Dynamic VHost(%s) not allowed.\n", host));
			return NULL;
		}
		/* 이후 부분은 동적 VOLUME을 생성하는 부분임
		 * 설정에 따라 동적 Volume의 생성 가능 여부를 선택 할수 있어야 한다.
		 */

		/* service를 추가 등록할 수 있는지 개수 확인 */

		if (dict_count(scx__service_dct) >= gscx__config->max_virtual_host)
		{
			TRACE((T_WARN, "reaches the allowable max(%d) service count. current(%d)!\n", gscx__config->max_virtual_host, dict_count(scx__service_dct)));
			return NULL;
		}
		/*
		 * 동적 볼륨 추가시 동작
		 * rwlock
		 * lock을 걸기 이전에 다른 thread에서 동일 볼륨 추가가 있었는지 확인
		 * vm_insert_domain_to_hash() 호출 해서 도메인을 hash에 등록
		 * vm_create_service()호출
		 * unlock
		 */
		/* 동적 볼륨 추가를 위해 write lock을 건다. */
		pthread_rwlock_wrlock(&__services_lock);
		/* 신규 볼륨 등록 전에 다시 확인 해서 그사이에 동일 볼륨이 생성된게 있는지 확인 한다. */
		service = vm_lookup(host);
		if (service != NULL) {
			/* 다른 thread에서 볼륨이 등록 된 경우는 그냥 리턴한다. */
			goto L_add_domain_done;
		}

		site = scx_site_clone((char *)host, gscx__default_site);

		if (site == NULL) {
			TRACE((T_ERROR, "create site for '%s' - failed to clone (type=%d)\n", host, volume_type));
			ret = -1;
			goto L_add_domain_done;
		}

		service = (service_info_t *)mp_alloc(site->mp, sizeof(service_info_t));

		domain_cnt = 1;
		domain_root = scx_list_create(site->mp, LIST_TYPE_SIMPLE_KEY, 0);
		scx_list_append(domain_root, (char *)host, NULL, 0);
		service->domain_list = domain_root;

		result = vm_insert_domain_to_hash(service, host);
		ASSERT(0 == result);	/* 위에서 도메인이 hash에 없는것을 확인 했기 때문에 result가 -1이 나오면 논리 오류임 */

		if (SCX_YES != vm_create_service(service, site, VOLUME_DYNAMIC, NULL)) {
			TRACE((T_ERROR, " failed to create service (%s)\n", host));
			ret = -1;
			goto L_add_domain_done;
		}
	}
	else {	/* end of if (host) */
		/* host가 지정되지 않은 경우가 있으면 안된다. */
		TRACE((T_ERROR, "Unexpected empty host\n"));
		return NULL;
	}

L_add_domain_done:

	if (0 > ret) {
		/* 에러가 발생 한 경우 */
		if (service) {
			/*
			 * hash에서 도메인을 지우는 경우는 service가 생성 된 상태이어야만 한다.
			 * 추가 하려는 도메인이 기존에 사용중이어서 에러가 나는 경우는 service가 NULL임.
			 * (이때 hash를 지우게 되면 엉뚱한 service가 문제가 발생할수 있음)
			 */
			for(i = 0; i < domain_cnt; i++) {	/* vm_create_config()와 코드를 동일하게 유지하기 위해 for 문을 사용한다 */
				domain = scx_list_get_by_index_key(domain_root,i);
				ASSERT(domain);
				vm_delete_domain_from_hash(service, domain);
			}
			vm_destroy_service(service);
			service = NULL;
		}
		else if (NULL != site) {
			scx_site_destroy(site);
			site = NULL;
		}
	}
	pthread_rwlock_unlock(&__services_lock);
	return service;
}

/*
 * 중요 : host 파라미터의 포인터는 해당 dict가 삭제될때 까지 유지 되어야 한다.
 *       가능하면 service 구조체 내의 포인터를 사용해야 한다.
 */
int
vm_insert_domain_to_hash(service_info_t *service, const char *host)
{
	dict_insert_result result = dict_insert(scx__service_dct, (void *)host);
	if (!result.inserted) {
		TRACE((T_WARN, "Already registered virtual host(%s).\n", host));
		return -1;
	}
	ASSERT(result.datum_ptr != NULL);
	ASSERT(*result.datum_ptr == NULL);
	*result.datum_ptr = service;
	service->alias_count++;
	TRACE((T_INFO, "Virtual host for '%s' - successfully added to service hash.\n", host));
	return 0;

}

int
vm_delete_domain_from_hash(service_info_t *service, const char *host)
{
	dict_remove(scx__service_dct, (void *)host);
	TRACE((T_INFO, "Virtual host for '%s' - successfully removed from service hash.\n", host));
	return --service->alias_count;
}

/*
 * vm_lookup을 직접 호출 하는 경우는 호출 이전에 반드시 lock을 해주어야만 한다.
 */
service_info_t *
vm_lookup(const char *host)
{
	service_info_t		*service = NULL;
	void** search = dict_search(scx__service_dct, host);
	if (search == NULL) {
		return NULL;
	}
	service = *(service_info_t **)search;
	return service;

}

/*
 * origin/parent policy를 문자열로 리턴한다.
 * 성능이 필요한 부분이 아니라서 간단히 구현함.
 */
const char *
vm_get_policy_name(int policy)
{
	static char __policy_name_rr[] = "RoundRobin";
	static char __policy_name_ps[] = "Primary/Secondary";
	static char __policy_name_hash[] = "Hash";
	static char __policy_name_unknown[] = "Unknown";
	char * policy_name = __policy_name_rr;
	switch(policy) {
	case NC_LBP_POLICY_RR:
		policy_name = __policy_name_rr;
		break;
	case NC_LBP_POLICY_PS:
		policy_name = __policy_name_ps;
		break;
	case NC_LBP_POLICY_HASH:
		policy_name = __policy_name_hash;
		break;
	default:
		policy_name = __policy_name_unknown;
		break;
	}
	return policy_name;
}

/*
 * netcache 의 환경설정은 기본적으로 설정하지 않을 경우에 netcache core 의 설정에 따른다.
 * is_rollback이 1이며 rollback 과정이므로 netcache core쪽의 설정만 해주면 된다.
 */
int
vm_netcache_config(service_info_t *service, char* signature)
{
	int ret = -1;
	if (!service || !signature) {
		TRACE((T_ERROR, "service configuration for '%s' - failed to netcache apply\n", signature));
		return -1;
	}

	/* 공통 변수 */
	int 				rv_int = 0;
	int 				rv_uint = 0;
	int64_t				rv_int64 = 0;
	vstring_t 			*rv_chars = NULL;

	/* solproxy 관련 변수 */
	site_t 				*site = service->site;

	/* netcache 관련 변수 */
	nc_mount_context_t*	m = NULL;
	nc_origin_info_t	o[32] = { 0, };
	int 				ocnt = 0;
	vstring_t 			*drv_class = NULL;
	vstring_t 			*origins = NULL;
	vstring_t 			*parents = NULL;
	vstring_t 			*vs_o = NULL;
	off_t 				off = 0;
	vstring_t 			*o_host = NULL;
	vstring_t 			*o_userinfo = NULL;
	vstring_t 			*o_userid = NULL;
	vstring_t 			*o_password = NULL;
	int 	 			protocol = 0;
	int					negative_ttl = 0;
	int 				positive_ttl = 0;
	vstring_t 			def_drvclass = { 0, 0, "HTTPN" };
	char 				signature_tmp[128] = "";
	int 				ll = strlen(signature);
	scx_config_t		*config = NULL;
	vstring_t 			def_origin = { ll, ll, (char *)signature };
	int 				result = 0;
	vstring_t			str_alwaysexpire = { 0, 0, "alwaysexpire"};	/* manifest 및 TS 컨텐츠는 TTL 만료시 갱신을 확인 하지 않고 새로 받아온다.*/
	int					origin_policy = NC_LBP_POLICY_HASH;
	int					parent_policy = NC_LBP_POLICY_HASH;
	int 				transfer_timeout = 0;

	mem_pool_t 			*tmp_mpool = NULL;
	vstring_t			*cache_domain = NULL;
	service_info_t		*found_service = NULL;
	int					alias_done = 0;
	char				temp_url[512] = "";

	/* alias domain 이 지정된 경우는 해당 도메인의 mnt 정보를 상속 한다. */
	cache_domain = scx_get_vstring(site, SV_MASTER_CACHE_DOMAIN, NULL);
	if (cache_domain != NULL) {
		if (service->core->is_alias_mnt == 0 && service->core->mnt) {
			/* 기존에 alias 볼륨을 사용하지 않다가 사용하는 경우에는 기존 mount를 해제 해주어야 한다.*/
			nc_ioctl(service->core->mnt, NC_IOCTL_CACHE_MONITOR, NULL, sizeof(void *));
			nc_ioctl(service->core->mnt, NC_IOCTL_ORIGIN_MONITOR, NULL, sizeof(void *));
			nc_ioctl(service->core->mnt, NC_IOCTL_ORIGIN_MONITOR2, NULL, sizeof(void *));
			nc_ioctl(service->core->mnt, NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER, NULL, sizeof(void *));
			nc_ioctl(service->core->mnt, NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER, NULL, sizeof(void *));
			TRACE((T_INFO, "service(%s) unmounted.\n", vs_data(service->name)));
			nc_destroy_mount_context(service->core->mnt);
			//WEON
			ASSERT(__sync_sub_and_fetch(&service->opened, 0) == 0);
			if (nc_errno() < 0)	{
				TRACE((T_WARN, "virtual host connection for '%s' - failed to destroy mount(%d)\n", vs_data(service->name), nc_errno()));
			}
			service->core->mnt = NULL;
			SCX_FREE(service->core->conf.origins);
		}
		found_service = vm_lookup(vs_data(cache_domain));
		if (found_service != NULL) {
			/* alias 도메인을 찾은 경우 찾은 도메인의 mnt 정보를 상속 받고 is_alias_mnt을 1로 설정한다. */
			service->core->mnt = found_service->core->mnt;
			service->follow_redir = found_service->follow_redir; /* 아래에 custom_user_agent 처리 항목 때문에 follow_redir도 같이 상속 받는다. */
			service->core->is_alias_mnt = 1;
			alias_done = 1;
			service->master_cache_domain = cache_domain;
			TRACE((T_INFO, "[%s] alias volume[%s] founded.\n", signature, vs_data(cache_domain)));
		}
		else {
			/* alias 도메인으로 지정된 도메인이 없는 경우 현재 도메인에 설정된 오리진 정보를 사용한다. */
			TRACE((T_WARN, "[%s] alias volume[%s] not exist.\n", signature, vs_data(cache_domain)));
		}
	}
	if (alias_done == 0) {
		if (service->core->is_alias_mnt == 1) {
			/* alias 도메인을 사용하다가 사용하지 않는 경우 현재 볼륨의 오리진을 사용해야 하기 때문에 mnt를 NULL로 명시적으로 초기화한다. */
			service->core->mnt = NULL;
			service->core->is_alias_mnt = 0;
		}

		if (ll > 128) {
			/* host의 크기가 signagure의 최대 크기인 127을 넘을 경우에는 앞에서 127byte만을 사용한다. */
			strncpy(signature_tmp, signature, 127);
			signature = signature_tmp;
			ll = strlen(signature);
		}
		tmp_mpool = mp_create(4096);

		config = scx_get_config();
		service->follow_redir= scx_get_uint(site, SV_FOLLOW_REDIR, config->follow_redir);
		scx_release_config(config);

		/* 설정을 적용한다. */
		drv_class = scx_get_vstring(site, SV_DRIVER_CLASS, &def_drvclass);
		if (config->allow_dynamic_vhost == 1) {
			/* 동적 볼륨을 사용할 때에만 signature를 origin으로 사용한다. */
			origins = scx_get_vstring(site, SV_ORIGIN, &def_origin);
		}
		else {
			origins = scx_get_vstring(site, SV_ORIGIN, NULL);
		}
		protocol = scx_get_uint(site, SV_ORIGIN_PROTOCOL, 0); /* default http */
		service->protocol = protocol;
		if (origins)
		{
			/* origin info를 origin  설정해야함 */
			off = vs_pickup_token(tmp_mpool, origins, off, ",", &vs_o);
			if (NULL != vs_o) vs_trim_string(vs_o);
			while (vs_o)
			{
				memset(&o[ocnt], 0, sizeof(nc_origin_info_t));
				o[ocnt].ioflag = NCOF_READABLE|NCOF_WRITABLE;
				strncpy(o[ocnt].encoding, "utf-8", NC_MAX_STRINGLEN);
				int	query_off;	//query parameter의 시작 부분의 offset. ? 포함
				vstring_t 	*v_token = NULL;
				query_off = vs_pickup_token_r(tmp_mpool, vs_o, vs_length(vs_o), "@", &o_host, DIRECTION_RIGHT);
				vs_trim_string(o_host);
				if (query_off != 0) {
					/* 양방향 스토리지 연결을 위한 인증정보 파싱 부분
					 * 양방향을 사용할 경우 origin 설정은 id:password@domain:port,id:password@domain:port의 형태임*/
					o_userinfo 	= vs_allocate(tmp_mpool, query_off, vs_data(vs_o), TRUE);
					vs_trim_string(o_userinfo);
					query_off 	= vs_pickup_token_r(tmp_mpool, o_userinfo, vs_length(o_userinfo), ":", &o_password, DIRECTION_RIGHT);
					vs_trim_string(o_password);
					if (query_off != 0) {
						o_userid 	= vs_allocate(tmp_mpool, query_off, vs_data(o_userinfo), TRUE);
						//printf("userid = '%s', password = '%s', host = '%s'\n", vs_data(o_userid), vs_data(o_password), vs_data(o_host));
						strncpy(o[ocnt].user, vs_data(o_userid), NC_MAX_STRINGLEN-1);
						strncpy(o[ocnt].pass, vs_data(o_password), NC_MAX_STRINGLEN-1);
						if ( 0 < vs_length(o_userid) && 0 < vs_length(o_password)) {
							/* id와 password가 지정된 경우 origin 연결에 인증을 사용하도록 한다. */
							o[ocnt].ioflag |= NCOF_BASIC_AUTH_FROM_USER;
						}
					}
					/* 양방향 인증을 사용하는 경우 client request header에 들어 있는 Authorization 헤더를 제거 하기 위해 use_origin_authorization로 marking 한다. */
					service->use_origin_authorization = 1;
				}
				else {
					o_host 		= vs_o;
				}
				strncpy(o[ocnt].prefix, vs_data(o_host), NC_MAX_STRINGLEN-1); 			/* 시그너처: 수정 필요 */
				snprintf(o[ocnt].address, NC_MAX_STRINGLEN, "%s://%s%s", (protocol?"https":"http"), vs_data(o_host), service->base_path!=NULL?vs_data(service->base_path):"");
				ocnt++;
				off = vs_pickup_token(tmp_mpool, origins, off, ",", &vs_o);
				if (NULL != vs_o) vs_trim_string(vs_o);

			}
		}
		else {
			TRACE((T_WARN, "Volume[%s] origin required.\n", signature));
			ret = -1;
			goto L_netcache_create_done;
		}

		service->core->conf.is_base_path_changed = 0;
		if (service->base_path == NULL && service->core->conf.base_path != NULL) {
			/* base_path가 있다가  삭제 된 경우 */
			service->core->conf.is_base_path_changed = 1;
		}
		else if(service->base_path != NULL && service->core->conf.base_path != NULL) {
			if (strcmp(vs_data(service->base_path), service->core->conf.base_path) != 0) {
				/* base_path 설정이 변경 된 경우 */
				service->core->conf.is_base_path_changed = 1;
			}
		}
		else if (service->base_path != NULL && service->core->conf.base_path == NULL) {
			/* base_path가 새로 추가된 경우 */
			service->core->conf.is_base_path_changed = 1;
		}
		if (service->base_path) {
			/* 설정이 정상적으로 적용된 경우 적용 이력을 남기기 위해 아래 처럼 설정 정보를 core->conf에 저장한다. */
			if (service->core->conf.base_path != NULL) {
				SCX_FREE(service->core->conf.base_path);
			}
			service->core->conf.base_path = SCX_MALLOC(vs_length(service->base_path)+1);
			snprintf(service->core->conf.base_path, vs_length(service->base_path)+1, "%s", vs_data(service->base_path));
		}
		else {
			if (service->core->conf.base_path != NULL) {
				SCX_FREE(service->core->conf.base_path);
				service->core->conf.base_path = NULL;
			}
		}

		if (NULL == service->core->mnt) {
			/* host의 크기가 128을 넘을 경우 signature을 재정의 해야함 */
			m = nc_create_mount_context((char *)vs_data(drv_class), signature, o, ocnt/*오리진 정보 갯수*/);
			if (!m || nc_errno() < 0)
			{
				TRACE((T_ERROR, "virtual host connection for '%s' - failed to create (%d)\n", signature, nc_errno()));
				ret = -1;
				goto L_netcache_create_done;
			}
			service->core->mnt = m;
			/* netcache core의 default는 대소문자 무시이기 때문에 대소문자를 구분하도록 설정 해주어야 한다. */
			rv_int = 1;
			nc_ioctl(m, NC_IOCTL_PRESERVE_CASE, &rv_int, sizeof(rv_int));
			nc_ioctl(m, NC_IOCTL_SET_LAZY_PROPERTY_UPDATE, NULL, 0);	/* property 요청시와 nc_read시의 파일 크기가 다를 경우처리를 하기 위해 추가 */
			nc_ioctl(m, NC_IOCTL_ORIGIN_MONITOR_CBD, (void *)service->core, sizeof(void *));
			TRACE((T_DAEMON, "SERVICE[%s] - set origin_moitor_cbd as %p\n", vs_data(service->name), service->core));

			nc_ioctl(m, NC_IOCTL_CACHE_MONITOR, nce_object_monitor, sizeof(void *));
			nc_ioctl(m, NC_IOCTL_ORIGIN_MONITOR, scx_origin_monitor, sizeof(void *));
			nc_ioctl(m, NC_IOCTL_ORIGIN_MONITOR2, scx_origin_monitor2, sizeof(void *));

			nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER, scx_origin_request_handler, sizeof(void *));
			nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER, scx_origin_response_handler, sizeof(void *));
			/* origin 주소가 변경 되는 경우 설정을 다시 적용 해야 되기 때문에 기존에 기억한 설정을 모두 초기화 한다. */
			vm_reset_applied_conf(service->core);
			/* 설정이 정상적으로 적용된 경우 적용 이력을 남기기 위해 아래 처럼 설정 정보를 core->conf에 저장한다. */
			service->core->conf.origins = SCX_MALLOC(vs_length(origins)+1);
			snprintf(service->core->conf.origins, vs_length(origins)+1, "%s", vs_data(origins));

		}
		else {
			 /* 이전의 서비스 정보를 갱신하는 경우는 mnt가 이미 생성된 상태이므로 nc_ioctl()로 갱신만 해준다. */
			if (service->core->conf.origins == NULL ||
					strcmp(service->core->conf.origins, vs_data(origins)) != 0 ||
					service->core->conf.is_base_path_changed == 1 ||
					service->core->conf.origin_protocol != protocol) {	/* 설정이 변경 된 경우만 아래의 과정을 진행 */
				ret = nc_ioctl(service->core->mnt, NC_IOCTL_UPDATE_ORIGIN, (void *)o, ocnt/*오리진 정보 갯수*/);
				if (0 > ret)
				{
					TRACE((T_ERROR, "virtual host connection for '%s' - failed to set (%d)\n", signature, nc_errno()));
					ret = -1;
					goto L_netcache_create_done;
				}
				/* origin 주소가 변경 되는 경우 설정을 다시 적용 해야 되기 때문에 기존에 기억한 설정을 모두 초기화 한다. */
				vm_reset_applied_conf(service->core);
				/* 설정이 정상적으로 적용된 경우 적용 이력을 남기기 위해 아래 처럼 설정 정보를 core->conf에 저장한다. */
				SCX_FREE(service->core->conf.origins);
				service->core->conf.origins = SCX_MALLOC(vs_length(origins)+1);
				snprintf(service->core->conf.origins, vs_length(origins)+1, "%s", vs_data(origins));
				service->core->conf.origin_protocol = protocol;
			}
			m = service->core->mnt;
		}

		parents = scx_get_vstring(site, SV_PARENT, NULL);
		if (parents)
		{
			vs_o = NULL;
			off = 0;
			o_host = NULL;
			o_userinfo = NULL;
			o_userid = NULL;
			o_password = NULL;
			ocnt = 0;
			/* origin info를 origin  설정해야함 */
			off = vs_pickup_token(tmp_mpool, parents, off, ",", &vs_o);
			if (NULL != vs_o) vs_trim_string(vs_o);
			while (vs_o)
			{
				memset(&o[ocnt], 0, sizeof(nc_origin_info_t));
				o[ocnt].ioflag = NCOF_READABLE|NCOF_WRITABLE;
				strncpy(o[ocnt].encoding, "utf-8", NC_MAX_STRINGLEN);
				int	query_off;	//query parameter의 시작 부분의 offset. ? 포함
				vstring_t 	*v_token = NULL;
				query_off = vs_pickup_token_r(tmp_mpool, vs_o, vs_length(vs_o), "@", &o_host, DIRECTION_RIGHT);
				vs_trim_string(o_host);
				if (query_off != 0) {
					/* 양방향 스토리지 연결을 위한 인증정보 파싱 부분
					 * 양방향을 사용할 경우 origin 설정은 id:password@domain:port,id:password@domain:port의 형태임*/
					o_userinfo 	= vs_allocate(tmp_mpool, query_off, vs_data(vs_o), TRUE);
					vs_trim_string(o_userinfo);
					query_off 	= vs_pickup_token_r(tmp_mpool, o_userinfo, vs_length(o_userinfo), ":", &o_password, DIRECTION_RIGHT);
					vs_trim_string(o_password);
					if (query_off != 0) {
						o_userid 	= vs_allocate(tmp_mpool, query_off, vs_data(o_userinfo), TRUE);
						//printf("userid = '%s', password = '%s', host = '%s'\n", vs_data(o_userid), vs_data(o_password), vs_data(o_host));
						strncpy(o[ocnt].user, vs_data(o_userid), NC_MAX_STRINGLEN-1);
						strncpy(o[ocnt].pass, vs_data(o_password), NC_MAX_STRINGLEN-1);
						if ( 0 < vs_length(o_userid) && 0 < vs_length(o_password)) {
							/* id와 password가 지정된 경우 origin 연결에 인증을 사용하도록 한다. */
							o[ocnt].ioflag |= NCOF_BASIC_AUTH_FROM_USER;
						}
					}
				}
				else {
					o_host 		= vs_o;
				}
				strncpy(o[ocnt].prefix, vs_data(o_host), NC_MAX_STRINGLEN-1); 			/* 시그너처: 수정 필요 */
				snprintf(o[ocnt].address, NC_MAX_STRINGLEN, "%s://%s%s", (protocol?"https":"http"), vs_data(o_host), service->base_path!=NULL?vs_data(service->base_path):"");
				ocnt++;
				off = vs_pickup_token(tmp_mpool, parents, off, ",", &vs_o);
				if (NULL != vs_o) vs_trim_string(vs_o);
			}
			if (service->core->conf.parents == NULL ||
					strcmp(service->core->conf.parents, vs_data(parents)) != 0 ||
					service->core->conf.is_base_path_changed == 1 ||
					service->core->conf.origin_protocol != protocol) {	/* 설정이 변경 된 경우만 아래의 과정을 진행 */
				ret = nc_ioctl(service->core->mnt, NC_IOCTL_UPDATE_PARENT, (void *)o, ocnt/*Parent 정보 갯수*/);
				if (0 > ret)
				{
					TRACE((T_ERROR, "virtual host connection for '%s' - failed to set parent (%d)\n", signature, nc_errno()));
					ret = -1;
					goto L_netcache_create_done;
				}
				/* origin 주소가 변경 되는 경우 설정을 다시 적용 해야 되기 때문에 기존에 기억한 설정을 모두 초기화 한다. */
				vm_reset_applied_conf(service->core);
				/* 설정이 정상적으로 적용된 경우 적용 이력을 남기기 위해 아래 처럼 설정 정보를 core->conf에 저장한다. */
				SCX_FREE(service->core->conf.parents);
				service->core->conf.parents = SCX_MALLOC(vs_length(parents)+1);
				snprintf(service->core->conf.parents, vs_length(parents)+1, "%s", vs_data(parents));
			}
			rv_chars = scx_get_vstring(site, SV_PARENT_POLICY, NULL);
			if (rv_chars) {
				if (0 == vs_strncasecmp_lg(rv_chars, "rr", 2)) {
					parent_policy = NC_LBP_POLICY_RR;
				}
				else if (0 == vs_strncasecmp_lg(rv_chars, "ps", 2)) {
					parent_policy = NC_LBP_POLICY_PS;
				}
				else if (0 == vs_strncasecmp_lg(rv_chars, "hash", 4)) {
					parent_policy = NC_LBP_POLICY_HASH;
				}
			}
			if (0 != parent_policy && parent_policy != service->core->conf.parent_policy) {
				rv_int64 = parent_policy;
				result = nc_ioctl(m, NC_IOCTL_SET_LB_PARENT_POLICY, &rv_int64, sizeof(rv_int64));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Parent Policy '%d(%s)', result(%d)\n",
						signature, parent_policy, vm_get_policy_name(parent_policy), result));
				service->core->conf.parent_policy = parent_policy;
			}

		}
		else {
			/* parent가 없는 경우는 count에 0을 전달한다. (https://jarvis.solbox.com/redmine/issues/32410 참조) */
			ret = nc_ioctl(service->core->mnt, NC_IOCTL_UPDATE_PARENT, NULL, 0/*Parent 정보 갯수*/);
			if (0 > ret)
			{
				TRACE((T_ERROR, "virtual host connection for '%s' - failed to set parent (%d)\n", signature, nc_errno()));
				ret = -1;
				goto L_netcache_create_done;
			}
			SCX_FREE(service->core->conf.parents);
		}


		rv_int = scx_get_uint(site, SV_WRITEBACK_CHUNKS, 10);
		if (service->core->conf.writeback_chunks != rv_int) {
			nc_ioctl(m, NC_IOCTL_WRITEBACK_BLOCKS, &rv_int, sizeof(rv_int));
			service->core->conf.writeback_chunks = rv_int;
		}
		rv_chars = scx_get_vstring(site, SV_PROXY, NULL);
		if (rv_chars) {
			/* proxy는 한번 설정 되면 해제할 방법은 없는것 같음. */
			if (service->core->conf.proxy_address == NULL ||
					strcmp(service->core->conf.proxy_address, vs_data(rv_chars)) != 0) {
				result = nc_ioctl(m, NC_IOCTL_PROXY, vs_data(rv_chars), vs_length(rv_chars));
				if (result >= 0) {
					SCX_FREE(service->core->conf.proxy_address);
					service->core->conf.proxy_address = SCX_MALLOC(vs_length(rv_chars)+1);
					snprintf(service->core->conf.proxy_address, vs_length(rv_chars)+1, "%s", vs_data(rv_chars));
				}
			}
		}
		nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_REQ_CBDATA, site, sizeof(void *));
		nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_RES_CBDATA, site, sizeof(void *));

#if 0	/* 현재 NetCache 3.0에서 NC_IOCTL_USE_HEAD_FOR_ATTR를 1로 설정할 경우 full GET 컨텐츠에 대해서 IMS를 사용하지 않는 문제가 있어서 사용하지 못하도록 한다. */
		rv_int = scx_get_uint(site, SV_USE_HEAD, -1); /* default GET */
		if (rv_int >= 0) {
			result = nc_ioctl(m, NC_IOCTL_USE_HEAD_FOR_ATTR, &rv_int, sizeof(rv_int));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Use Head for atrribute '%d', result(%d)\n", signature, rv_int, result));
		}
#endif
		rv_uint = scx_get_uint(site, SV_ORIGIN_TIMEOUT, 0); /* 설정하지 않은 경우 plugin에 30초 기본으로 적용됨 */
		if (rv_uint > 0) {
			/*
			* SV_ORIGIN_TIMEOUT은
			* SV_ORIGIN_CONNECT_TIMEOUT과 동일, 이전버전과 호환성 유지용, 삭제 예정
			*/
			if (service->core->conf.origin_connect_timeout != rv_uint) {
				result = nc_ioctl(m, NC_IOCTL_CONNECT_TIMEOUT, &rv_uint, sizeof(rv_uint));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Origin Connection Timeout '%d', result(%d)\n", signature, rv_uint, result));
				service->core->conf.origin_connect_timeout = rv_uint;
			}
		}
		else {
			rv_uint = scx_get_uint(site, SV_ORIGIN_CONNECT_TIMEOUT, 3); /* 설정하지 않은 경우 NetCache에서 10초 기본으로 적용됨 */
			if (rv_uint > 0) {
				/*
				* 원본의 연결 timeout
				*/
				if (service->core->conf.origin_connect_timeout != rv_uint) {
					result = nc_ioctl(m, NC_IOCTL_CONNECT_TIMEOUT, &rv_uint, sizeof(rv_uint));
					TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Origin Connection Timeout '%d', result(%d)\n", signature, rv_uint, result));
					service->core->conf.origin_connect_timeout = rv_uint;
				}
			}
		}
		transfer_timeout = scx_get_uint(site, SV_ORIGIN_TRANSFER_TIMEOUT, 5); /* 설정하지 않은 경우 NetCache에서 30초 기본으로 적용됨 */
		if (transfer_timeout > 0) {
			/*
			* 원본의 receive timeout
			*/
			if (service->core->conf.origin_transfer_timeout != transfer_timeout) {
				result = nc_ioctl(m, NC_IOCTL_TRANSFER_TIMEOUT, &transfer_timeout, sizeof(transfer_timeout));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Origin Transfer Timeout '%d', result(%d)\n", signature, transfer_timeout, result));
				service->core->conf.origin_transfer_timeout = transfer_timeout;
			}
		}
		rv_uint = scx_get_uint(site, SV_NC_READ_TIMEOUT, transfer_timeout); /* 설정하지 않은 경우 origin_transfer_timeout을 상속 받는다. */
		if (rv_uint > 0) {
			/*
			* nc_read() timeout
			*/
			if (service->core->conf.nc_read_timeout != rv_uint) {
				result = nc_ioctl(m, NC_IOCTL_SET_NCREAD_TIMEOUT, &rv_uint, sizeof(rv_uint));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set nc_read() Timeout '%d', result(%d)\n", signature, rv_uint, result));
				service->core->conf.nc_read_timeout = rv_uint;
			}
		}
		/*
		 * Origin 또는 Parent 에서 접속실패, 잘못된 응답 등의 상황이 연속으로 발생 하여 설정값을 초과 하는 경우 OFFLINE 으로 처리함.
		 * 예를 들어 2로 설정한 경우 연속으로 2번 실패가 나면 OFFLINE 처리 된다.
		 */
		rv_uint = scx_get_uint(site, SV_ORIGIN_MAX_FAIL_COUNT, 2);
		if (rv_uint > 100000) rv_uint = 100000;
		else if (rv_uint < 1) rv_uint = 1;
		if (service->core->conf.origin_max_fail_count != rv_uint) {
			result = nc_ioctl(m, NC_IOCTL_SET_LB_FAIL_COUNT_TO_OFFLINE, &rv_uint, sizeof(rv_uint));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Origin Max Fail Count '%d', result(%d)\n", signature, rv_uint, result));
			service->core->conf.origin_max_fail_count = rv_uint;
		}

		if (service->streaming_method == 1) {
			/* 라이브 캐싱 컨텐츠는 TTL이 만료 되면 캐싱된것을 버리고 새로 받는다. */
			rv_chars = scx_get_vstring(site, SV_FRESH_CHECK_POLICY, &str_alwaysexpire);
		}
		else {
			rv_chars = scx_get_vstring(site, SV_FRESH_CHECK_POLICY, NULL);
		}
		if (rv_chars) {
			if (service->core->conf.fresh_check_policy[0] == '\0' ||
					 strcmp(service->core->conf.fresh_check_policy, vs_data(rv_chars)) != 0) {
				result = nc_ioctl(m, NC_IOCTL_FRESHNESS_CHECK, vs_data(rv_chars), vs_length(rv_chars));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - freshness check policy defined as '%s', result(%d)\n", signature, vs_data(rv_chars), result));
				snprintf(service->core->conf.fresh_check_policy, 30, "%s", vs_data(rv_chars));
			}
		}
		if (service->follow_redir) {
			/*
			 * follow_redirection이 설정 되어 있는 경우를 양방향 연결이라고 보고 아래의 정책을 설정한다.
			 */
			origin_policy = NC_LBP_POLICY_PS;
		}
		if (service->streaming_method == 1) {
			/* live ad stitching 기능으로 동작하는 경우 오리진 서버는 master쪽만 바라봐야 한다. */
			origin_policy = NC_LBP_POLICY_PS;
			/* live ad stitching 기능으로 동작하는 경우 오리진으로 302응답이 있는 경우 location헤더에 지정된 서버로 접속해서 데이터를 받아 와야 한다. */
			service->follow_redir = 1;
		}
		rv_chars = scx_get_vstring(site, SV_ORIGIN_POLICY, NULL);
		if (rv_chars) {
			if (0 == vs_strncasecmp_lg(rv_chars, "rr", 2)) {
				origin_policy = NC_LBP_POLICY_RR;
			}
			else if (0 == vs_strncasecmp_lg(rv_chars, "ps", 2)) {
				origin_policy = NC_LBP_POLICY_PS;
			}
			else if (0 == vs_strncasecmp_lg(rv_chars, "hash", 4)) {
				origin_policy = NC_LBP_POLICY_HASH;
			}
		}

		if (0 != origin_policy && service->core->conf.origin_policy != origin_policy) {
			rv_int64 = origin_policy;
			if (service->core->conf.origin_policy != origin_policy) {
				result = nc_ioctl(m, NC_IOCTL_SET_LB_ORIGIN_POLICY, &rv_int64, sizeof(rv_int64));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Origin Policy '%d(%s)', result(%d)\n",
						signature, origin_policy, vm_get_policy_name(origin_policy), result));
				service->core->conf.origin_policy = origin_policy;
			}
		}

		rv_chars = scx_get_vstring(site, SV_ORIGIN_ONLINE_CODE, NULL);
		if (rv_chars) {
			service->origin_online_code = vs_strdup(site->mp, rv_chars);
			/*
			 * callback 함수 등록 여부 기준
			 ** origin_online_code_rev 값이 0보다 큰 경우 함수가 등록 되어있는 상태
			 ** origin_online_code_rev 값이 0인 경우  함수가 등록 되지 않은 상태로 본다.
			 */
			if (service->core->conf.origin_online_code_rev == 0) {
				nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_FUNCTION , scx_origin_online_handler, sizeof(void *));
				nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_DATA , service->core, sizeof(void *));
				service->core->conf.origin_online_code_rev = gscx__load_revision;
			}
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set origin online code : '%s'\n", signature, vs_data(rv_chars)));

		}
		else {
			if (service->core->conf.origin_online_code_rev > 0) {
				result = nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_FUNCTION , NULL, sizeof(void *));
				result = nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_DATA , NULL, sizeof(void *));
				service->core->conf.origin_online_code_rev = 0;
			}

		}

		/*
		* 생성된 볼륨을 등록, 복수 등록 고려해야함
		* site profile에 domain=가 여러 개 있을 수 있음
		*/

		negative_ttl = scx_get_uint(site, SV_NEGATIVE_TTL, negative_ttl);
		positive_ttl = scx_get_uint(site, SV_POSITIVE_TTL, positive_ttl);

		if (negative_ttl) {
			if (service->core->conf.negative_ttl != negative_ttl) {
				result = nc_ioctl(m, NC_IOCTL_NEGATIVE_TTL, &negative_ttl, sizeof(negative_ttl));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Negative TTL '%d', result(%d)\n", signature, negative_ttl, result));
				service->core->conf.negative_ttl = negative_ttl;
			}
		}
		if (positive_ttl) {
			if (service->core->conf.positive_ttl != positive_ttl) {
				result = nc_ioctl(m, NC_IOCTL_POSITIVE_TTL, &positive_ttl, sizeof(positive_ttl));
				TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Positive TTL '%d', result(%d)\n", signature, positive_ttl, result));
				service->core->conf.positive_ttl = positive_ttl;
			}
		}

		rv_int = service->follow_redir;
		if (service->core->conf.follow_redir != rv_int) {
			result = nc_ioctl(m, NC_IOCTL_FOLLOW_REDIRECTION, &rv_int, sizeof(rv_int));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Follow Redirection '%d', result(%d)\n", signature, rv_int, result));
			service->core->conf.follow_redir = rv_int;
		}

		service->core->master_service = (void *)service;

		rv_int = scx_get_int(site, SV_DNS_CACHE_TTL, 60);
		if (service->core->conf.dns_cache_ttl != rv_int) {
			result = nc_ioctl(m, NC_IOCTL_SET_DNS_CACHE_TIMEOUT, &rv_int, sizeof(rv_int));
			TRACE((T_DAEMON, "SERVICE[%s] - set DNS Cache TTL '%d', result(%d)\n", vs_data(service->name), rv_int, result));
			service->core->conf.dns_cache_ttl = rv_int;
		}

		rv_chars = scx_get_vstring(site, SV_ORIGIN_HEALTH_CHECK_URL, NULL);
		if (rv_chars) {
			/* base_path가 지정된 경우 base_path/origin_health_check_url로 설정 되어야 한다.*/
			if (service->base_path != NULL) {
				snprintf(temp_url, 512, "%s/%s", vs_data(service->base_path), vs_data(rv_chars));
				if (service->core->conf.origin_health_check_url == NULL ||
						strcmp(service->core->conf.origin_health_check_url, temp_url) != 0) {
					result = nc_ioctl(m, NC_IOCTL_SET_PROBING_URL , temp_url, -1);
					SCX_FREE(service->core->conf.origin_health_check_url);
					service->core->conf.origin_health_check_url = SCX_MALLOC(strlen(temp_url)+1);
					snprintf(service->core->conf.origin_health_check_url, strlen(temp_url)+1, "%s", temp_url);
				}

			}
			else {
				if (service->core->conf.origin_health_check_url == NULL ||
						strcmp(service->core->conf.origin_health_check_url, vs_data(rv_chars)) != 0) {
					result = nc_ioctl(m, NC_IOCTL_SET_PROBING_URL , vs_data(rv_chars), -1);
					SCX_FREE(service->core->conf.origin_health_check_url);
					service->core->conf.origin_health_check_url = SCX_MALLOC(vs_length(rv_chars)+1);
					snprintf(service->core->conf.origin_health_check_url, vs_length(rv_chars)+1, "%s", vs_data(rv_chars));
				}
			}

		}
		else {
			if (service->core->conf.origin_health_check_url != NULL) {
				result = nc_ioctl(m, NC_IOCTL_SET_PROBING_URL , NULL, -1);
				SCX_FREE(service->core->conf.origin_health_check_url);
			}

		}

	} /* end of if (alias_done == 0) */


	/*
	 * User-Agent 헤더 변경 우선순위
	 * 1. custom_user_agent이 0으로 설정되는 경우 Client request에 들어 있는 User-Agent헤더를 오리진에 전달
	 * 2. custom_user_agent에 설정이 되는 경우 해당 값을 오리진 요청 User-Agent 헤더에 사용
	 * 3. 오리진이 양방향 서버인 경우(follow_redir이 1로 설정되는 경우)는 NetCache로 시작하는 custom User-Agent 헤더를 사용
	 * 4. 이외의 경우는 Client의 request에 들어 있는 User-Agent헤더를 오리진에 전달
	 * https://jarvis.solbox.com/redmine/issues/31783 참조
	 */
	rv_chars = scx_get_vstring(site, SV_CUSTOM_USER_AGENT, NULL);
	if (rv_chars) {
		if (vs_strcasecmp_lg(rv_chars, "0") == 0) {
			service->custom_user_agent = NULL;
		}
		else {
			service->custom_user_agent = vs_strdup(site->mp, rv_chars);
		}
	}
	else if (service->follow_redir) {
		service->custom_user_agent = vs_strdup_lg(site->mp, gscx__custom_user_agent);
	}
	else {
		service->custom_user_agent = NULL;
	}

	ret = 0;

L_netcache_create_done:
	if (tmp_mpool) mp_free(tmp_mpool);
	/* 광고 Stitching 기능을 사용하는 경우는 광고용 오리진을 한개더 생성한다. */
#ifdef ZIPPER
	ret = vm_ad_netcache_config(service, signature);
#endif
	return ret;
}

#ifdef ZIPPER
/*
 * 광고 전용 오리진 설정
 */
int
vm_ad_netcache_config(service_info_t *service, char* signature)
{
	int ret = -1;
	/* 공통 변수 */
	int 				rv_int = 0;
	int 				rv_uint = 0;
	int64_t				rv_int64 = 0;
	vstring_t 			*rv_chars = NULL;

	/* solproxy 관련 변수 */
	site_t 				*site = service->site;

	/* netcache 관련 변수 */
	nc_mount_context_t*	m = NULL;
	nc_origin_info_t	o[32] = { 0, };
	int 				ocnt = 0;
	vstring_t 			*drv_class = NULL;
	vstring_t 			*origins = NULL;
	vstring_t 			*parents = NULL;
	vstring_t 			*vs_o = NULL;
	off_t 				off = 0;
	vstring_t 			*o_host = NULL;
	vstring_t 			*o_userinfo = NULL;
	vstring_t 			*o_userid = NULL;
	vstring_t 			*o_password = NULL;
	int 	 			protocol = 0;
	int					negative_ttl = 0;
	int 				positive_ttl = 0;
	vstring_t 			def_drvclass = { 0, 0, "HTTPN" };
	char 				signature_tmp[128] = "";
	int 				ll = strlen(signature);
	scx_config_t		*config = NULL;
	int 				result = 0;
	int					follow_redir = 0;
	int 				transfer_timeout = 0;

	int					origin_policy = NC_LBP_POLICY_PS;
	char				fress_check_policy[] = {"alwaysexpire"};	/* manifest 및 TS 컨텐츠는 TTL 만료시 갱신을 확인 하지 않고 새로 받아온다.*/

	mem_pool_t 			*tmp_mpool = NULL;

	{
		/* signature를 host 명 앞에 live_를 붙여서 광고(vod)용 오리진과 구분하도록 한다. */
		snprintf(signature_tmp, 127, "live_%s", signature);
		signature = signature_tmp;
	}
	tmp_mpool = mp_create(4096);


	/* 설정을 적용한다. */
	drv_class = scx_get_vstring(site, SV_DRIVER_CLASS, &def_drvclass);
	origins = scx_get_vstring(site, SV_AD_ORIGIN, NULL);
	if (origins)
	{
		service->ad_origin_hostname = scx_get_vstring(site, SV_AD_ORIGIN_HOSTNAME, NULL);
		service->ad_base_path = scx_get_vstring(site, SV_AD_BASE_PATH, NULL);
		protocol = scx_get_uint(site, SV_AD_ORIGIN_PROTOCOL, 0); /* default http */
		/* origin info를 origin  설정해야함 */
		off = vs_pickup_token(tmp_mpool, origins, off, ",", &vs_o);
		if (NULL != vs_o) vs_trim_string(vs_o);
		while (vs_o)
		{
			memset(&o[ocnt], 0, sizeof(nc_origin_info_t));
			o[ocnt].ioflag = NCOF_READABLE|NCOF_WRITABLE;
			strncpy(o[ocnt].encoding, "utf-8", NC_MAX_STRINGLEN);
			int	query_off;	//query parameter의 시작 부분의 offset. ? 포함
			vstring_t 	*v_token = NULL;
			query_off = vs_pickup_token_r(tmp_mpool, vs_o, vs_length(vs_o), "@", &o_host, DIRECTION_RIGHT);
			vs_trim_string(o_host);
			if (query_off != 0) {
				/* 양방향 스토리지 연결을 위한 인증정보 파싱 부분
				 * 양방향을 사용할 경우 origin 설정은 id:password@domain:port,id:password@domain:port의 형태임*/
				o_userinfo 	= vs_allocate(tmp_mpool, query_off, vs_data(vs_o), TRUE);
				vs_trim_string(o_userinfo);
				query_off 	= vs_pickup_token_r(tmp_mpool, o_userinfo, vs_length(o_userinfo), ":", &o_password, DIRECTION_RIGHT);
				vs_trim_string(o_password);
				if (query_off != 0) {
					o_userid 	= vs_allocate(tmp_mpool, query_off, vs_data(o_userinfo), TRUE);
					//printf("userid = '%s', password = '%s', host = '%s'\n", vs_data(o_userid), vs_data(o_password), vs_data(o_host));
					strncpy(o[ocnt].user, vs_data(o_userid), NC_MAX_STRINGLEN-1);
					strncpy(o[ocnt].pass, vs_data(o_password), NC_MAX_STRINGLEN-1);
					if ( 0 < vs_length(o_userid) && 0 < vs_length(o_password)) {
						/* id와 password가 지정된 경우 origin 연결에 인증을 사용하도록 한다. */
						o[ocnt].ioflag |= NCOF_BASIC_AUTH_FROM_USER;
					}
				}
			}
			else {
				o_host 		= vs_o;
			}
			strncpy(o[ocnt].prefix, vs_data(o_host), NC_MAX_STRINGLEN-1); 			/* 시그너처: 수정 필요 */
			snprintf(o[ocnt].address, NC_MAX_STRINGLEN, "%s://%s%s", (protocol?"https":"http"), vs_data(o_host), service->ad_base_path!=NULL?vs_data(service->ad_base_path):"");
			ocnt++;
			off = vs_pickup_token(tmp_mpool, origins, off, ",", &vs_o);
			if (NULL != vs_o) vs_trim_string(vs_o);
		}
	}
	else {
		/*
		 * 광고 VOD 전용 오리진 설정이 되지 않은 경우는 다른 설정들은 필요가 없으므로 여기서 리턴한다.
		 * 광고 VOD 전용 오리진 설정이 되지 않은 경우에는 기본 오리진의 origin_hostname과 base_path를 상속 받는다.
		 */
		service->ad_origin_hostname = service->origin_hostname;
		service->ad_base_path = service->base_path;
		if (NULL != service->core->ad_mnt) {
			/* 광고 오리진이 설정 되었다 없어 지는 경우에는 광고 오리진의 mount를 해제 한다. */
			if(service->core->ad_mnt) {
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_CACHE_MONITOR, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_ORIGIN_MONITOR, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_ORIGIN_MONITOR2, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER, NULL, sizeof(void *));
				nc_ioctl(service->core->ad_mnt, NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER, NULL, sizeof(void *));
				TRACE((T_DAEMON, "service(%s) Advertizement origin unmounted.\n", vs_data(service->name)));

				//WEON
				ASSERT(__sync_sub_and_fetch(&service->opened, 0) == 0);

				nc_destroy_mount_context(service->core->ad_mnt);
				if (nc_errno() < 0)	{
					TRACE((T_WARN, "virtual host for '%s' - failed to destroy Advertizement mount(%d)\n", vs_data(service->name), nc_errno()));
				}
				service->core->ad_mnt = NULL;
				SCX_FREE(service->core->conf.ad_origins);
			}
		}
		ret = 0;
		goto ad_netcache_confige_done;
	}
	service->core->conf.is_ad_base_path_changed = 0;
	if (service->ad_base_path == NULL && service->core->conf.ad_base_path != NULL) {
		/* base_path가 있다가  삭제 된 경우 */
		service->core->conf.is_ad_base_path_changed = 1;
	}
	else if(service->ad_base_path != NULL && service->core->conf.ad_base_path != NULL) {
		if (strcmp(vs_data(service->ad_base_path), service->core->conf.ad_base_path) != 0) {
			/* base_path 설정이 변경 된 경우 */
			service->core->conf.is_ad_base_path_changed = 1;
		}
	}
	else if (service->ad_base_path != NULL && service->core->conf.ad_base_path == NULL) {
		/* base_path가 새로 추가된 경우 */
		service->core->conf.is_ad_base_path_changed = 1;
	}
	if (service->ad_base_path) {
		/* 설정이 정상적으로 적용된 경우 적용 이력을 남기기 위해 아래 처럼 설정 정보를 core->conf에 저장한다. */
		if (service->core->conf.ad_base_path != NULL) {
			SCX_FREE(service->core->conf.ad_base_path);
		}
		service->core->conf.ad_base_path = SCX_MALLOC(vs_length(service->ad_base_path)+1);
		snprintf(service->core->conf.ad_base_path, vs_length(service->ad_base_path)+1, "%s", vs_data(service->ad_base_path));
	}
	else {
		if (service->core->conf.ad_base_path != NULL) {
			SCX_FREE(service->core->conf.ad_base_path);
			service->core->conf.ad_base_path = NULL;
		}
	}

	if (NULL == service->core->ad_mnt) {
		m = nc_create_mount_context((char *)vs_data(drv_class), signature, o, ocnt/*오리진 정보 갯수*/);
		if (!m || nc_errno() < 0)
		{
			TRACE((T_ERROR, "virtual host Advertizement connection for '%s' - failed to create (%d)\n", signature, nc_errno()));
			ret = -1;
			goto ad_netcache_confige_done;
		}
		service->core->ad_mnt = m;

		/* netcache core의 default는 대소문자 무시이기 때문에 대소문자를 구분하도록 설정 해주어야 한다. */
		rv_int = 1;
		nc_ioctl(m, NC_IOCTL_PRESERVE_CASE, &rv_int, sizeof(rv_int));
		nc_ioctl(m, NC_IOCTL_SET_LAZY_PROPERTY_UPDATE, NULL, 0);	/* property 요청시와 nc_read시의 파일 크기가 다를 경우처리를 하기 위해 추가 */
		nc_ioctl(m, NC_IOCTL_ORIGIN_MONITOR_CBD, (void *)service->core, sizeof(void *));
		TRACE((T_DAEMON, "SERVICE[%s] - Set Advertizement origin_moitor_cbd as %p\n", vs_data(service->name), service));

		nc_ioctl(m, NC_IOCTL_CACHE_MONITOR, nce_object_monitor, sizeof(void *));
		nc_ioctl(m, NC_IOCTL_ORIGIN_MONITOR, scx_origin_monitor, sizeof(void *));
		nc_ioctl(m, NC_IOCTL_ORIGIN_MONITOR2, scx_origin_monitor2, sizeof(void *));

		nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_REQ_HANDLER, scx_origin_request_handler, sizeof(void *));
		nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_RES_HANDLER, scx_origin_response_handler, sizeof(void *));

		/* origin 주소가 변경 되는 경우 설정을 다시 적용 해야 되기 때문에 기존에 기억한 설정을 모두 초기화 한다. */
		vm_reset_applied_conf(service->core);
		service->core->conf.ad_origins = SCX_MALLOC(vs_length(origins)+1);
		snprintf(service->core->conf.ad_origins, vs_length(origins)+1, "%s", vs_data(origins));

	}
	else {
		 /* 이전의 서비스 정보를 갱신하는 경우는 mnt가 이미 생성된 상태이므로 nc_ioctl()로 갱신만 해준다. */
		if (service->core->conf.ad_origins == NULL ||
				strcmp(service->core->conf.ad_origins, vs_data(origins)) != 0 ||
				service->core->conf.is_ad_base_path_changed == 1 ||
				service->core->conf.ad_origin_protocol != protocol) {	/* 설정이 변경 된 경우만 아래의 과정을 진행 */
			ret = nc_ioctl(service->core->ad_mnt, NC_IOCTL_UPDATE_ORIGIN, (void *)o, ocnt/*오리진 정보 갯수*/);
			if (0 > ret)
			{
				TRACE((T_ERROR, "virtual host Advertizement connection for '%s' - failed to set (%d)\n", signature, nc_errno()));
				ret = -1;
				goto ad_netcache_confige_done;
			}
			/* origin 주소가 변경 되는 경우 설정을 다시 적용 해야 되기 때문에 기존에 기억한 설정을 모두 초기화 한다. */
			vm_reset_applied_conf(service->core);
			/* 설정이 정상적으로 적용된 경우 적용 이력을 남기기 위해 아래 처럼 설정 정보를 core->conf에 저장한다. */
			SCX_FREE(service->core->conf.ad_origins);
			service->core->conf.ad_origins = SCX_MALLOC(vs_length(origins)+1);
			snprintf(service->core->conf.ad_origins, vs_length(origins)+1, "%s", vs_data(origins));
			service->core->conf.ad_origin_protocol = protocol;
		}
		m = service->core->ad_mnt;
	}

	rv_int = scx_get_uint(site, SV_WRITEBACK_CHUNKS, 10);
	if (service->core->conf.ad_writeback_chunks != rv_int) {
		nc_ioctl(m, NC_IOCTL_WRITEBACK_BLOCKS, &rv_int, sizeof(rv_int));
		service->core->conf.ad_writeback_chunks = rv_int;
	}
	nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_REQ_CBDATA, site, sizeof(void *));
	nc_ioctl(m, NC_IOCTL_SET_ORIGIN_PHASE_RES_CBDATA, site, sizeof(void *));

	rv_uint = scx_get_uint(site, SV_ORIGIN_CONNECT_TIMEOUT, 3); /* 설정하지 않은 경우 NetCache에서 5초 기본으로 적용됨 */
	if (rv_uint > 0) {
		/*
		* 원본의 연결 timeout
		*/
		if (service->core->conf.ad_origin_connect_timeout != rv_uint) {
			result = nc_ioctl(m, NC_IOCTL_CONNECT_TIMEOUT, &rv_uint, sizeof(rv_uint));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Advertizement Origin Connection Timeout '%d', result(%d)\n", signature, rv_uint, result));
			service->core->conf.ad_origin_connect_timeout = rv_uint;
		}
	}

	transfer_timeout = scx_get_uint(site, SV_ORIGIN_TRANSFER_TIMEOUT, 5); /* 설정하지 않은 경우 NetCache에서 30초 기본으로 적용됨 */
	if (transfer_timeout > 0) {
		/*
		* 원본의 receive timeout
		*/
		if (service->core->conf.ad_origin_transfer_timeout != transfer_timeout) {
			result = nc_ioctl(m, NC_IOCTL_TRANSFER_TIMEOUT, &transfer_timeout, sizeof(transfer_timeout));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Advertizement Origin Transfer Timeout '%d', result(%d)\n", signature, transfer_timeout, result));
			service->core->conf.ad_origin_transfer_timeout = transfer_timeout;
		}
	}
	rv_uint = scx_get_uint(site, SV_NC_READ_TIMEOUT, transfer_timeout); /* 설정하지 않은 경우 origin_transfer_timeout을 상속 받는다. */
	if (rv_uint > 0) {
		/*
		* nc_read() timeout
		*/
		if (service->core->conf.ad_nc_read_timeout != rv_uint) {
			result = nc_ioctl(m, NC_IOCTL_SET_NCREAD_TIMEOUT, &rv_uint, sizeof(rv_uint));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set nc_read() Timeout '%d', result(%d)\n", signature, rv_uint, result));
			service->core->conf.ad_nc_read_timeout = rv_uint;
		}
	}
	rv_chars = scx_get_vstring(site, SV_AD_FRESH_CHECK_POLICY, NULL);
	if (rv_chars) {
		if (service->core->conf.ad_fresh_check_policy[0] == '\0' ||
				strcmp(service->core->conf.ad_fresh_check_policy, vs_data(rv_chars)) != 0) {
			result = nc_ioctl(m, NC_IOCTL_FRESHNESS_CHECK, vs_data(rv_chars), vs_length(rv_chars));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Advertizement freshness check policy defined as '%s', result(%d)\n", signature, vs_data(rv_chars), result));
			snprintf(service->core->conf.ad_fresh_check_policy, 30, "%s", vs_data(rv_chars));
		}
	}

	rv_chars = scx_get_vstring(site, SV_AD_ORIGIN_POLICY, NULL);
	if (rv_chars) {
		if (0 == vs_strncasecmp_lg(rv_chars, "rr", 2)) {
			origin_policy = NC_LBP_POLICY_RR;
		}
		else if (0 == vs_strncasecmp_lg(rv_chars, "ps", 2)) {
			origin_policy = NC_LBP_POLICY_PS;
		}
		else if (0 == vs_strncasecmp_lg(rv_chars, "hash", 4)) {
			origin_policy = NC_LBP_POLICY_HASH;
		}
	}

	follow_redir = scx_get_uint(site, SV_AD_FOLLOW_REDIR, 0);
	rv_int = follow_redir;
	result = nc_ioctl(m, NC_IOCTL_FOLLOW_REDIRECTION, &rv_int, sizeof(rv_int));
	TRACE((T_INFO | T_DAEMON, "Volume[%s] - Advertizement Set Follow Redirection '%d', result(%d)\n", signature, rv_int, result));

	if (follow_redir) {
		/*
		 * follow_redirection이 설정 되어 있는 경우를 양방향 연결이라고 보고 아래의 정책을 설정한다.
		 */
		origin_policy = NC_LBP_POLICY_PS;

	}
	if (0 != origin_policy && service->core->conf.ad_origin_policy != origin_policy) {
		rv_int64 = origin_policy;
		if (service->core->conf.ad_origin_policy != origin_policy) {
			result = nc_ioctl(m, NC_IOCTL_SET_LB_ORIGIN_POLICY, &rv_int64, sizeof(rv_int64));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Advertizement Origin Policy '%d(%s)', result(%d)\n",
					signature, origin_policy, vm_get_policy_name(origin_policy), result));
			service->core->conf.ad_origin_policy = origin_policy;
		}
	}

	rv_chars = scx_get_vstring(site, SV_ORIGIN_ONLINE_CODE, NULL);
	if (rv_chars) {
		service->origin_online_code = vs_strdup(site->mp, rv_chars);
		/*
		 * callback 함수 등록 여부 기준
		 ** origin_online_code_rev 값이 0보다 큰 경우 함수가 등록 되어있는 상태
		 ** origin_online_code_rev 값이 0인 경우  함수가 등록 되지 않은 상태로 본다.
		 */
		if (service->core->conf.ad_origin_online_code_rev == 0) {
			result = nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_FUNCTION , scx_origin_online_handler, sizeof(void *));
			result = nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_DATA , service->core, sizeof(void *));
			service->core->conf.ad_origin_online_code_rev = gscx__load_revision;
		}
		TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Advertizement online code : '%s', result(%d)\n", signature, vs_data(rv_chars), result));
	}
	else {
		if (service->core->conf.ad_origin_online_code_rev > 0) {
			result = nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_FUNCTION , NULL, sizeof(void *));
			result = nc_ioctl(m, NC_IOCTL_SET_OFFLINE_POLICY_DATA , NULL, sizeof(void *));
			service->core->conf.ad_origin_online_code_rev = 0;
		}
	}

	negative_ttl = scx_get_uint(site, SV_AD_NEGATIVE_TTL, negative_ttl);
	positive_ttl = scx_get_uint(site, SV_AD_POSITIVE_TTL, positive_ttl);

	if (negative_ttl) {
		if (service->core->conf.ad_negative_ttl != negative_ttl) {
			result = nc_ioctl(m, NC_IOCTL_NEGATIVE_TTL, &negative_ttl, sizeof(negative_ttl));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Advertizement Negative TTL '%d', result(%d)\n", signature, negative_ttl, result));
			service->core->conf.ad_negative_ttl = negative_ttl;
		}
	}
	if (positive_ttl) {
		if (service->core->conf.ad_positive_ttl != positive_ttl) {
			result = nc_ioctl(m, NC_IOCTL_POSITIVE_TTL, &positive_ttl, sizeof(positive_ttl));
			TRACE((T_INFO | T_DAEMON, "Volume[%s] - Set Advertizement Positive TTL '%d', result(%d)\n", signature, positive_ttl, result));
			service->core->conf.ad_positive_ttl = positive_ttl;
		}
	}


	ret = 0;

	ad_netcache_confige_done:
	if (tmp_mpool) mp_free(tmp_mpool);
	return ret;
}
#endif

int
vm_base_config(service_info_t *service)
{
	int ret = -1;

	/* 공통 변수 */
	int 				rv_int = 0;
	int 				rv_uint = 0;
	vstring_t 			*rv_chars = NULL;
	vstring_t 			*vs_o = NULL;
	off_t 				off = 0;

	/* solproxy 관련 변수 */
	site_t*				site = NULL;
	scx_config_t		*config = NULL;
	vstring_t 			*permitted = NULL;

	int					hls_target_duration = 10;
	int					quick_start_seg = 0;
	int					session_enable = 1;
	int					session_timeout = 30;
	int					streaming_enable = 0;

	ASSERT(service);
	site = service->site;
	
	/* default config를 사용하는 부분은 모두 여기에 넣는다. */
	config = scx_get_config();
	service->log_gmt_time = config->log_gmt_time;
	service->log_access = scx_get_uint(site, SV_LOG_ACCESS, config->log_access);
	service->log_error = scx_get_uint(site, SV_LOG_ERROR, config->log_error);
	service->log_origin = scx_get_uint(site, SV_LOG_ORIGIN, config->log_origin);
	session_enable = scx_get_uint(site, SV_SESSION_ENABLE, config->session_enable);
	if (session_enable == 1) {
		if (config->stat_write_type == STAT_TYPE_STREAMING) {
			service->stat_session_type_enable = scx_get_uint(site, SV_STAT_SESSION_TYPE_ENABLE, 1);
		}
		else {
			service->stat_session_type_enable = scx_get_uint(site, SV_STAT_SESSION_TYPE_ENABLE, 0);
		}
	}
	else
	{	/* session 기능을 사용하지 않는 경우에는 stat_session_type_enable은 자동으로 0으로 설정된다. */
		service->stat_session_type_enable = 0;
	}
	session_timeout = scx_get_uint(site, SV_SESSION_TIMEOUT, config->session_timeout);
	service->use_local_file = scx_get_uint(site, SV_USE_LOCAL_FILE, config->use_local_file);
	if (2 != config->polling_policy) {
		/*
		 * polling_policy가 2인 경우(poll+connection per thread)에는 limit rate기능이 동작 하지 않음.
		 */
		service->limit_rate = scx_get_uint(site, SV_LIMIT_RATE, 0);
		service->limit_rate_after = scx_get_uint(site, SV_LIMIT_RATE_AFTER, 0);
		service->limit_traffic_rate = scx_get_uint(site, SV_LIMIT_TRAFFIC_AFTER, 0);
	}
	service->enable_async_accesshandler_callback = scx_get_uint(site, SV_ENABLE_ASYNC_ACCESSHANDLER_CALLBACK, config->enable_async_accesshandler_callback);
	service->enable_async_reader_callback = scx_get_uint(site, SV_ENABLE_ASYNC_READER_CALLBACK, config->enable_async_reader_callback);
	service->enable_async_complete_callback = scx_get_uint(site, SV_ENABLE_ASYNC_COMPLETE_CALLBACK, config->enable_async_complete_callback);

	service->stat_write_enable = scx_get_uint(site, SV_STAT_SERVICE_WRITE_ENABLE, config->stat_write_enable);
	service->stat_origin_enable = scx_get_uint(site, SV_STAT_SERVICE_ORIGIN_ENABLE, config->stat_origin_enable);
	service->stat_traffic_enable = scx_get_uint(site, SV_STAT_SERVICE_TRAFFIC_ENABLE, config->stat_traffic_enable);
	service->stat_nos_enable = scx_get_uint(site, SV_STAT_SERVICE_NOS_ENABLE, config->stat_nos_enable);
	service->stat_http_enable = scx_get_uint(site, SV_STAT_SERVICE_HTTP_ENABLE, config->stat_http_enable);
	service->cdn_domain_postfix = scx_get_vstring(site, SV_CDN_DOMAIN_POSTFIX, config->cdn_domain_postfix);


	scx_release_config(config);

	service->stat_sp_seq = scx_get_uint(site, SV_STAT_SP_SEQ, 0);
	service->stat_cont_seq = scx_get_uint(site, SV_STAT_CONT_SEQ, 0);
	service->stat_vol_seq = scx_get_uint(site, SV_STAT_VOL_SEQ, 0);
	if (service->stat_sp_seq == 0 || service->stat_cont_seq == 0 || service->stat_vol_seq == 0) {
		/* 통계 관련 sequence가 한개라도 없는 경우는 통게를 기록하지 않는다. */
		service->stat_write_enable = 0;
	}

	service->stat_http_ignore_path = scx_get_uint(site, SV_STAT_HTTP_IGNORE_PATH, 0);

	service->send_timeout = scx_get_uint(site, SV_SEND_TIMEOUT, 120);
	service->keepalive_timeout = scx_get_uint(site, SV_KEEPALIVE_TIMEOUT, 10);


	/* jwt 인증과 soluri2 인증이 동시에 설정되는 경우 jwt 인증만을 사용한다.*/
	rv_chars = scx_get_vstring(site, SV_SOLJWT_SECRET, NULL);
	if (rv_chars) {	/* SV_SOLJWT_SECRET은 컴마(,)로 구분된 여러개의 키가 설정될수 있다. */
		service->soljwt_secret_list = vm_parse_string_by_token(site->mp, rv_chars, ",");
	}
	else {
		rv_chars = scx_get_vstring(site, SV_SOLURI2_SECRET, NULL);
		if (rv_chars) {	/* SV_SOLURI2_SECRET은 컴마(,)로 구분된 여러개의 키가 설정될수 있다. */
			service->soluri2_secret = vm_parse_string_by_token(site->mp, rv_chars, ",");
		}
	}
	if (service->soljwt_secret_list != NULL || service->soluri2_secret != NULL) {
		/* 인증 기능을 사용하는 경우 별도의 설정이 없는 경우 query를 무시하도록 한다. */
		rv_int = scx_get_uint(site, SV_IGNORE_QUERY, 1);
	}
	else {
		rv_int = scx_get_uint(site, SV_IGNORE_QUERY, 0);
	}
	if (1 == rv_int)
		service->ignore_query = 1;

	service->enable_https = scx_get_uint(site, SV_ENABLE_HTTPS, 0);
	if (service->enable_https != 0 && service->enable_https != 1)
		service->enable_https = 0;

	rv_int = scx_get_uint(site, SV_AUTH_START_TIME, 60);
	if (0 < rv_int)
		service->auth_start_time = rv_int;

	service->origin_request_type = scx_get_uint(site, SV_ORIGIN_REQUEST_TYPE, 0);
	if(service->origin_request_type < 0 || service->origin_request_type > 2) {
		service->origin_request_type = 0;
	}

	rv_chars = scx_get_vstring(site, SV_BYPASS_METHOD, NULL);
	if (rv_chars) {
		service->bypass_method = vs_strdup(site->mp, rv_chars);
	}
	else {
		service->bypass_method = NULL;
	}

	rv_chars = scx_get_vstring(site, SV_REMOVE_ORIGIN_REQEUEST_HEADERS, NULL);
	if (rv_chars) {	/* SV_SOLJWT_SECRET은 컴마(,)로 구분된 여러개의 키가 설정될수 있다. */
		service->remove_origin_request_headers = vm_parse_string_by_token(site->mp, rv_chars, ",");
	}
	else {
		service->remove_origin_request_headers = NULL;
	}
	rv_chars = scx_get_vstring(site, SV_REMOVE_ORIGIN_RESPONSE_HEADERS, NULL);
	if (rv_chars) {	/* SV_SOLJWT_SECRET은 컴마(,)로 구분된 여러개의 키가 설정될수 있다. */
		service->remove_origin_response_headers = vm_parse_string_by_token(site->mp, rv_chars, ",");
	}
	else {
		service->remove_origin_response_headers = NULL;
	}

	/*
	 * referers_allow와 referers_deny는 동시에 사용되지 않으며
	 * 같이 설정되는 경우 referers_allow만 적용된다.
	 * referer_not_exist_allow는 referers_allow나 referers_deny가 설정 된 경우만 유효하다.
	 */
	rv_chars = scx_get_vstring(site, SV_REFERERS_ALLOW, NULL);
	if (rv_chars) {	/* SV_SOLJWT_SECRET은 컴마(,)로 구분된 여러개의 키가 설정될수 있다. */
		service->referers_allow = vm_parse_string_by_token(site->mp, rv_chars, ",");
	}
	else {
		service->referers_allow = NULL;
		rv_chars = scx_get_vstring(site, SV_REFERERS_DENY, NULL);
		if (rv_chars) {	/* SV_SOLJWT_SECRET은 컴마(,)로 구분된 여러개의 키가 설정될수 있다. */
			service->referers_deny = vm_parse_string_by_token(site->mp, rv_chars, ",");
		}
		else {
			service->referers_deny = NULL;
		}
	}
	service->referer_not_exist_allow = scx_get_uint(site, SV_REFERER_NOT_EXIST_ALLOW, 1);


	//////////////////////	Streaming 관련 설정 시작	///////////////////////////////


	quick_start_seg = scx_get_uint(site, SV_QUICK_START_SEG, quick_start_seg);
	service->quick_start_seg = quick_start_seg;
	if (session_timeout < 5) {
		session_timeout = 5;
	}
	service->session_timeout = session_timeout;
	if (session_enable != 0) {
		session_enable = 1;
	}
	service->session_enable = session_enable;
#ifdef ZIPPER
	/* zipper(streaming 모드)로 빌드시에는 streaming_enable이 항상 1이다. */
	service->streaming_enable = 1;
	service->streaming_method = scx_get_uint(site, SV_STREAMING_METHOD, 0); /* 기본 설정은 0(VOD)로 동작 */
	if (service->streaming_method > 2) service->streaming_method = 0;
	if (service->streaming_method == 1) {
		/* Live ad stitching 모드로 동작하는 경우는 session 기능이 필수 이기 때문에 자동으로 설정 된다. */
		service->session_enable = 1;
		service->quick_start_seg = 0;	/* Live Ad Stitching 모드에서는 quick_start_seg를 사용하지 않는다. */
	}

#else
	if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
		service->streaming_enable = scx_get_uint(site, SV_STREAMING_ENABLE, 1);
		service->rtmp_service_enable = scx_get_uint(site, SV_RTMP_SERVICE_ENABLE, 1);
	}
	else {
		service->streaming_enable = scx_get_uint(site, SV_STREAMING_ENABLE, 0);
		service->rtmp_service_enable = scx_get_uint(site, SV_RTMP_SERVICE_ENABLE, 0);
	}
	if (service->streaming_enable == 1) {
		/* 스트리밍 기능 사용시는 별도의 설정이 없어도 CORS 응답 기능이 활성화 된다. */
		service->enable_crossdomain = 1;
	}
	service->enable_wowza_url = scx_get_uint(site, SV_EANBLE_WOWZA_URL, 0);
	service->cache_request_enable = scx_get_uint(site, SV_CACHE_REQUEST_ENABLE, 1);
	service->streaming_media_ext = scx_get_vstring(site, SV_STREAMING_MEDIA_EXT, NULL);

	/* solproxy에서는 RTMP LIVE로 동작하는 경우(streaming_method가 2)를 제외 하고는 streaming_method가 항상 0(vod)임 */
	service->streaming_method = scx_get_uint(site, SV_STREAMING_METHOD, 0); /* 기본 설정은 0(VOD)로 동작 */
	if (service->streaming_method != 2) service->streaming_method = 0;
#endif

	service->progressive_convert_enable = scx_get_uint(site, SV_PROGRESSIVE_CONVERT_ENABLE, 1);
	hls_target_duration = scx_get_uint(site, SV_STREAMING_TARGET_DURATION, hls_target_duration); /* 설정하지 않는 경우 10초임 */
	if (hls_target_duration < 1) {
		hls_target_duration = 1;
	}
	service->hls_target_duration = hls_target_duration;

	service->subtitle_target_duration = scx_get_uint(site, SV_SUBTITLE_TARGET_DURATION, 60); /* 설정하지 않는 경우 60초임 */
	if (service->subtitle_target_duration < 1) {
		service->subtitle_target_duration = 1;
	}

	service->origin_hostname = scx_get_vstring(site, SV_ORIGIN_HOSTNAME, NULL);

	service->base_path = scx_get_vstring(site, SV_BASE_PATH, NULL);

#if 0
	rv_chars = scx_get_vstring(site, SV_SMIL_ORIGIN, NULL);
	if (rv_chars) {
		service->smil_origin = vs_strdup(site->mp, rv_chars);
	}
#endif
	service->hls_playlist_host_domain = scx_get_vstring(site, SV_HLS_PLAYLIST_HOST_DOMAIN, NULL);
	service->hls_master_playlist_use_absolute_path = scx_get_uint(site, SV_HLS_MASTER_PLAYLIST_USE_ABSOLUTE_PATH, 0);
	service->hls_media_playlist_use_absolute_path = scx_get_uint(site, SV_HLS_MEDIA_PLAYLIST_USE_ABSOLUTE_PATH, 0);
	service->hls_permit_discontinuty = scx_get_uint(site, SV_HLS_PERMIT_DISCONTINUTY, 0);
	service->hls_discontinuty_all = scx_get_uint(site, SV_HLS_DISCONTINUTY_ALL, 0);
	rv_chars = scx_get_vstring(site, SV_HLS_ENCRYPT_METHOD, NULL);
	if (rv_chars) {
		if (vs_strcasecmp_lg(rv_chars, "AES-128") == 0) {
			service->hls_encrypt_method = ENCRYPT_METHOD_AES_128;
		}
		else if (vs_strcasecmp_lg(rv_chars, "SAMPLE-AES") == 0) {
			service->hls_encrypt_method = ENCRYPT_METHOD_SAMPLE_AES;
		}
		else
		{
			service->hls_encrypt_method = ENCRYPT_METHOD_NONE;
		}
	}
	rv_chars = scx_get_vstring(site, SV_HLS_ENCRYPT_KEY, NULL);
	if (rv_chars) {
		service->hls_encrypt_key = vs_strdup(site->mp, rv_chars);
	}
	else  {	/* ENCRYPT_METHOD가 설정 되어 있어도 ENCRYPT_KEY 설정 되지 않은 경우는 무시된다. */
		service->hls_encrypt_method = ENCRYPT_METHOD_NONE;
	}
	service->manifest_prefix_path = scx_get_vstring(site, SV_MANIFEST_PREFIX_PATH, NULL);
	service->vol_service_type = scx_get_uint(site, SV_VOL_SERVICE_TYPE, VOL_SERVICE_TYPE_COMMON);
	if (service->vol_service_type == VOL_SERVICE_TYPE_SERVICE) {
		service->remove_dir_level = scx_get_uint(site, SV_REMOVE_DIR_LEVEL, 0);
		/* jwt 인증과 soluri2 인증이 동시에 설정되는 경우 jwt 인증만을 사용한다.*/
		rv_chars = scx_get_vstring(site, SV_SUB_DOMAIN_LIST, NULL);
		if (rv_chars) {	/* SV_SUB_DOMAIN_LIST는  "볼륨명:볼륨도메인,볼륨명:볼륨도메인" 형식으로 들어오기 때문에 파싱을 해서 sub_domain_list에 넣는다. */
			service->sub_domain_list = vm_parse_sub_domain_list(service, rv_chars);
		}
		else {
			service->sub_domain_list = NULL;
			TRACE((T_WARN, "%s '%s' are required to use '%s' = %d.\n", vs_data(service->name), SV_SUB_DOMAIN_LIST, SV_VOL_SERVICE_TYPE, VOL_SERVICE_TYPE_SERVICE));
		}
	}
	service->media_negative_ttl = scx_get_int(site, SV_MEDIA_NEGATIVE_TTL, 1);

	service->cr_service_enable = scx_get_uint(site, SV_CR_SERVICE_ENABLE, 0);
	service->cr_hot_content_day = scx_get_uint(site, SV_CR_HOT_CONTENT_DAY, 10);

	service->hls_playlist_protocol = UNDEFINED_PROTOCOL;
	rv_chars = scx_get_vstring(site, SV_HLS_PLAYLIST_PROTOCOL, NULL);
	if (rv_chars) {
		if (vs_strcmp_lg(rv_chars, "https") == 0) {
			service->hls_playlist_protocol = 1;
		}
		else if (vs_strcmp_lg(rv_chars, "http") == 0) {
			service->hls_playlist_protocol = 0;
		}
	}
	rv_chars = scx_get_vstring_lg(site, SV_HLS_ENCRYPT_KEY_PROTOCOL, "http");
	if (vs_strcmp_lg(rv_chars, "https") == 0) {
		service->hls_encrypt_key_protocol = 1;
	}
	else {
		service->hls_encrypt_key_protocol = 0;
	}
#if 0
	rv_chars = scx_get_vstring_lg(site, SV_HLS_SEGMENT_PROTOCOL, "http");
	if (vs_strcmp_lg(rv_chars, "https") == 0) {
		service->hls_segment_protocol = 1;
	}
	else {
		service->hls_segment_protocol = 0;
	}
#endif
	service->allow_different_protocol = scx_get_uint(site, SV_ALLOW_DIFFERENT_PROTOCOL, 1); /* 설정하지 않은 경우 https, http 모두 허용 */

	rv_chars = scx_get_vstring_lg(site, SV_STREAMING_ALLOW_PROTOCOL, "progressive,hls,dash,mss"); /* 기본 설정은 모두 허용임 */
	off = 0;
	off = vs_pickup_token(site->mp, rv_chars, off, ",", &permitted);
	while (permitted)
	{
		vs_trim_string(permitted);
		if (vs_strcmp_lg(permitted, "progressive") == 0) {
			service->permitted_protocol |= O_PROTOCOL_PROGRESSIVE;
		}
		else if (vs_strcmp_lg(permitted, "hls") == 0) {
			service->permitted_protocol |= O_PROTOCOL_HLS;
		}
		else if (vs_strcmp_lg(permitted, "dash") == 0) {
			service->permitted_protocol |= O_PROTOCOL_DASH;
		}
		else if (vs_strcmp_lg(permitted, "mss") == 0) {
			service->permitted_protocol |= O_PROTOCOL_MSS;
		}
		off = vs_pickup_token(site->mp, rv_chars, off, ",", &permitted);
	}
	service->permitted_protocol |= O_PROTOCOL_CORS;
	service->permitted_protocol |= O_PROTOCOL_TUNNEL;

	rv_chars = scx_get_vstring_lg(site, SV_STREAMING_MODE, "single,multi,adaptive"); /* 기본 설정은 모두 허용임 */
	off = 0;
	off = vs_pickup_token(site->mp, rv_chars, off, ",", &permitted);
	while (permitted) {
		vs_trim_string(permitted);
		if (vs_strcmp_lg(permitted, "single") == 0) {
			service->permitted_mode |= O_STRM_TYPE_SINGLE | O_STRM_TYPE_DIRECT; /* 현재 single와 direct에 대한 구분은 따로 없다. */
		}
		else if (vs_strcmp_lg(permitted, "multi") == 0) {
			service->permitted_mode |= O_STRM_TYPE_MULTI;
		}
		else if (vs_strcmp_lg(permitted, "adaptive") == 0) {
			service->permitted_mode |= O_STRM_TYPE_ADAPTIVE;
		}
		off = vs_pickup_token(site->mp, rv_chars, off, ",", &permitted);
	}
	service->id3_retain = scx_get_uint(site, SV_ID3_RETAIN, 0); /* 설정하지 않은 경우 ID3 정보를 무시하도록 한다. */

#ifndef ZIPPER
	/* manifest 수정 기능은 SolProxy에서만 동작하고 Zipper에서는 동작 하지 않아야 한다. */
	service->hls_modify_manifest_enable = scx_get_uint(site, SV_HLS_MODIFY_MANIFEST_ENABLE, 0);
	if (service->hls_modify_manifest_enable == 1) {
		service->hls_modify_manifest_session_enable = scx_get_uint(site, SV_HLS_MODIFY_MANIFEST_SESSION_ENABLE, 0);
		service->hls_modify_manifest_redirect_enable = scx_get_uint(site, SV_HLS_MODIFY_MANIFEST_REDIRECT_ENABLE, 0);
#if 0
		/* 302 redirect를 사용할때에는 hostdomain이 필수 이지만 manifest body를 만들어서 redirect 하는 방식에서는 hostdomain이 없어도 된다.*/
		if (service->hls_modify_manifest_redirect_enable == 1) {
			/* manifest redirect 기능을 사용하기 위해서는 hls_playlist_host_domain이 필수이다. */
			if (service->hls_playlist_host_domain == NULL) {
				TRACE((T_WARN, "%s hls_modify_manifest_redirect_enable set failed.(reason : hls_playlist_host_domain not set)\n", vs_data(service->name)));
				service->hls_modify_manifest_redirect_enable = 0;
			}
		}
#endif
		service->hls_modify_manifest_ttl = scx_get_uint(site, SV_HLS_MODIFY_MANIFEST_TTL, 0);
		service->hls_modify_manifest_ext = scx_get_vstring_lg(site, SV_HLS_MODIFY_MANIFEST_EXT, "m3u8");
		service->origin_request_type = 1; /* 오리진 요청은 full GET으로 자동 설정 된다. */
		service->enable_crossdomain = 1;
		service->ignore_query = 1;
	}
#endif
	rv_int = scx_get_uint(site, SV_HLS_MANIFEST_COMPRESS_ENABLE, 0);
	if (1 == rv_int) {
		service->hls_manifest_compress_enable = 1;
	}
	rv_chars = scx_get_vstring(site, SV_ADDITIONAL_MANIFEST_CACHE_KEY, NULL);
	if (rv_chars) {	/* additional_manifest_cache_key은 컴마(,)로 구분해서 여러개를 설정 가능. */
		service->additional_manifest_cache_key_list = vm_parse_string_by_token(site->mp, rv_chars, ",");
	}
	else {
		service->additional_manifest_cache_key_list = NULL;
	}

	service->enable_crossdomain = scx_get_uint(site, SV_ENABLE_CROSSDOMAIN, service->enable_crossdomain);
	service->session_id_name = scx_get_vstring_lg(site, SV_SESSION_ID_NAME, "solsessionid");

	rv_int = scx_get_uint(site, SV_DASH_MANIFEST_USE_ABSOLUTE_PATH, service->dash_manifest_use_absolute_path);
	if (1 == rv_int) {
		/* dash_manifest_use_absolute_path이 1로 설정된 경우 dash_manifest_host_domain도 같이 설정되어 있어야만 정상 동작 한다. */
		rv_chars = scx_get_vstring(site, SV_DASH_MANIFEST_HOST_DOMAIN, NULL);
		if (rv_chars) {
			service->dash_manifest_host_domain = vs_strdup(site->mp, rv_chars);
			service->dash_manifest_use_absolute_path = 1;
		}
		else {
			TRACE((T_WARN, "%s '%s' settings are required to use '%s' settings .\n", vs_data(service->name), SV_DASH_MANIFEST_USE_ABSOLUTE_PATH, SV_DASH_MANIFEST_HOST_DOMAIN));
			service->dash_manifest_use_absolute_path = 0;
		}
	}
	rv_chars = scx_get_vstring(site, SV_DASH_MANIFEST_PROTOCOL, NULL);
	if (rv_chars) {
		if (vs_strcmp_lg(rv_chars, "https") == 0) {
			service->dash_manifest_protocol = 1;
		}
		else if (vs_strcmp_lg(rv_chars, "http") == 0) {
			service->dash_manifest_protocol = 0;
		}
	}
#ifdef ZIPPER
	service->ad_allow_media_fault = scx_get_uint(site, SV_AD_ALLOW_MEDIA_FAULT, 1); /* 설정하지 않는 경우 광고에 문제가 있어도 zipping 됨 */
#endif
	//////////////////////	  CMAF Live 설정 시작		///////////////////////////////
	if (service->streaming_method == 2 && service->streaming_enable == 1) {
		if (gscx__enable_async != 1) {
			/* 비동기 모드에서만 동작 가능하기 때문에 여기에서 ERROR 로그를 기록 한다. */
			TRACE((T_ERROR, "%s configuration failed, CMAF Live function works only in a Async mode.\n", vs_data(service->name)));
			ret = -1;
			goto L_base_config;
		}
		off = 0;
		rv_chars = scx_get_vstring(site, SV_CMAF_ORIGIN, NULL);	/* 복수 설정이 필요한 경우 컴마(,)로 구분 하고 두개까지 설정 가능 */
		if (rv_chars) {
			off = vs_pickup_token(site->mp, rv_chars, off, ",", &vs_o);
			if (NULL != vs_o) vs_trim_string(vs_o);
			service->cmaf_origin[0] = vs_strdup(site->mp, vs_o);
			off = vs_pickup_token(site->mp, rv_chars, off, ",", &vs_o);
			if (NULL != vs_o) {
				vs_trim_string(vs_o);
				service->cmaf_origin[1] = vs_strdup(site->mp, vs_o);
			}
			else {
				service->cmaf_origin[1] = NULL;
			}
		}
		else {
			TRACE((T_ERROR, "%s configuration failed, '%s' is required.\n", vs_data(service->name), SV_CMAF_ORIGIN));
			ret = -1;
			goto L_base_config;
		}
		rv_chars = scx_get_vstring(site, SV_CMAF_ORIGIN_BASE_PATH, NULL);
		if (rv_chars) {
			service->cmaf_origin_base_path = vs_strdup(site->mp, rv_chars);
		}
		service->cmaf_origin_path = scx_get_vstring(site, SV_CMAF_ORIGIN_PATH, NULL);
		if (service->cmaf_origin_path == NULL) {
			TRACE((T_ERROR, "%s configuration failed, '%s' is required.\n", vs_data(service->name), SV_CMAF_ORIGIN_PATH));
			ret = -1;
			goto L_base_config;
		}
		rv_int = scx_get_uint(site, SV_CMAF_ORIGIN_TIMEOUT, service->cmaf_origin_timeout);
		if (0 < rv_int) {
			service->cmaf_origin_timeout = rv_int;
		}
		rv_int = scx_get_uint(site, SV_CMAF_ORIGIN_DISCONNECT_TIME, service->cmaf_origin_disconnect_time);
		if (0 <= rv_int) {
			service->cmaf_origin_disconnect_time = rv_int;
		}
		rv_int = scx_get_uint(site, SV_CMAF_ORIGIN_CONNECT_ONLOAD, service->cmaf_origin_connect_onload);
		if (0 == rv_int || 1 == rv_int) {
			service->cmaf_origin_connect_onload = rv_int;
		}
		rv_int = scx_get_uint(site, SV_CMAF_FRAGMENT_CACHE_COUNT, service->cmaf_fragment_cache_count);
		if (3 < rv_int) {
			service->cmaf_fragment_cache_count = rv_int;
		}
		rv_int = scx_get_uint(site, SV_CMAF_PREFETCH_FRAGMENT_COUNT, service->cmaf_prefetch_fragment_count);
		if (0 <= rv_int) {
			service->cmaf_prefetch_fragment_count = rv_int;
		}
		rv_int = scx_get_uint(site, SV_CMAF_MANIFEST_FRAGMENT_COUNT, service->cmaf_manifest_fragment_count);
		if (2 < rv_int) {
			service->cmaf_manifest_fragment_count = rv_int;
		}
		if (strm_cmaf_check(service) != 1) {
			ret = -1;
			goto L_base_config;
		}
		service->enable_async_reader_callback = 0;	/* CMAF live 기능으로 동작할 때에는 reader callback은 비동기 동작을 하지 않도록 한다. https://jarvis.solbox.com/redmine/issues/32498 참조*/
	}
	else if(service->core->cmaf != NULL) {
		/* 해당 서비스가 CMAF Live 서비스를 하다가 중지(더이상 사용을 안함)하는 경우라고 본다.*/
		//strm_cmaf_destroy()를 호출 해야하지만 여기서 바로 호출 하게 되는 경우 서비스 지연이나 연결된 사용자들에 대한 처리 문제가 발생할수 있음
#pragma message("cmaf를 사용하다 사용하지 않는 경우에 대해 이부분 보강 해야함")
		strm_cmaf_stop(service);
	}
	//////////////////////	  CMAF Live 설정 끝		///////////////////////////////

	//////////////////////	Streaming 관련 설정 끝	///////////////////////////////



	ret = 0;

L_base_config:
	return ret;
}


void
vm_update_origin(nc_request_t *req, vstring_t *origins)
{

	vstring_t 			*v;
	nc_origin_info_t 	o[32];
	int 				ocnt = 0;
	int 				r;

	v = scx_get_vstring_pool(req->pool, req->service->site, SV_ORIGIN, NULL);
	if (!v) {
		/* 
		 * 설정파일에 origin 설정이 없음 
		 * 이런 경우에는 HTTP request에 포함된 host정보를 이용하여, 
		 * 오리진 설정이 잡힌 경우라고 봐야하며, 
		 * 이 경우에만 origin 설정을 바꿀 수 있음
		 */
		vstring_t 	*vs_o = NULL;
		off_t 		off = 0;
		/* origin info를 origin  설정해야함 */
		off = vs_pickup_token(req->pool, origins, off, ",", &vs_o);
		while (vs_o) {
			vs_trim_string(vs_o);
			memset(&o[ocnt], 0, sizeof(nc_origin_info_t));
			o[ocnt].ioflag = NCOF_READABLE|NCOF_WRITABLE;
			strncpy(o[ocnt].encoding, "utf-8", NC_MAX_STRINGLEN);
			strncpy(o[ocnt].prefix, vs_data(vs_o), NC_MAX_STRINGLEN-1); 			/* 시그너처: 수정 필요 */
			snprintf(o[ocnt].address, NC_MAX_STRINGLEN, "%s://%s%s", (req->service->protocol?"https":"http"), vs_data(vs_o), req->service->base_path!=NULL?vs_data(req->service->base_path):"");
			ocnt++;
			off = vs_pickup_token(req->pool, origins, off, ",", &vs_o);
		}
		if (ocnt > 0) {
			r = nc_ioctl(req->service->core->mnt, NC_IOCTL_UPDATE_ORIGIN, (void *)o, ocnt/*오리진 정보 갯수*/);
			scx_set_vstring(req->service->site, SV_ORIGIN, origins);
			scx_site_commit(req->service->site);
		}
	}
	return;
}
/*
 * request header에 
 * X-Origin-Info가 있는 경우 호출
 */
int
vm_update_versioned_origin(nc_request_t *req, uint64_t version, vstring_t *origins)
{
	vstring_t 	*v = NULL;
	int 		result = 0; /* no-op */
	nc_origin_info_t 	o[32];
	int 				ocnt = 0;
	v = scx_get_vstring_pool(req->pool, req->service->site, SV_ORIGIN, NULL);
	if (v) {
		/*
		 * origin 설정이 되어 있는 경우엔 적용안함 그냥 리턴
		 */
		return -1; /* improper op */
	}
	pthread_mutex_lock(&req->service->lock);
	if (req->service->current_origin_ver < version) {
		/* 
		 * 요청에 있는 오리진이 버전이 더 높음, 변경 필요
		 */
		vstring_t 	*vs_o = NULL;
		off_t 		off = 0;
		/* origin info를 origin  설정해야함 */
		TRACE((T_INFO, "Volume[%s] - got new origin string, '%s'\n", vs_data(req->service->name), vs_data(origins)));
		off = vs_pickup_token(req->pool, origins, off, " ", &vs_o);
		while (vs_o) {
			vs_trim_string(vs_o);
			memset(&o[ocnt], 0, sizeof(nc_origin_info_t));
			o[ocnt].ioflag = NCOF_READABLE|NCOF_WRITABLE;
			strncpy(o[ocnt].encoding, "utf-8", NC_MAX_STRINGLEN);
			strncpy(o[ocnt].prefix, vs_data(vs_o), NC_MAX_STRINGLEN-1); 			/* 시그너처: 수정 필요 */
			snprintf(o[ocnt].address, NC_MAX_STRINGLEN, "%s://%s%s", (req->connect_type == SCX_HTTPS?"https":"http"), vs_data(vs_o), req->service->base_path!=NULL?vs_data(req->service->base_path):"");
			ocnt++;
			off = vs_pickup_token(req->pool, origins, off, " ", &vs_o);
		}
		if (ocnt > 0) {
			int r;
			r = nc_ioctl(req->service->core->mnt, NC_IOCTL_UPDATE_ORIGIN, (void *)o, ocnt/*오리진 정보 갯수*/);
			if ( r >= 0) {
				req->service->current_origin_ver = version;
				TRACE((T_INFO, "Volume[%s] - origin version updated to %lld\n", vs_data(req->service->name), version));
				result = 1;
			}
			else {
				TRACE((T_INFO, "Volume[%s] - origin version failed to update(current version=%lld, wanted=%lld\n", 
						vs_data(req->service->name), req->service->current_origin_ver, version));
			}
		}
	}
	pthread_mutex_unlock(&req->service->lock);
	return result;
}

scx_list_t *
vm_parse_domain(site_t *site)
{
	scx_list_t *domain_root = NULL;
	off_t 	off = 0;

	vstring_t	*domain_s = NULL;
	vstring_t	*domain = NULL;

	/* domain 파라미터 체크, SV_DOMAIN에는 여러개의 domain이 ,(컴마)로 구분되서 들어올수 있다. */
	domain_s = scx_get_vstring_lg(site, SV_DOMAIN, "");
	if (vs_length(domain_s) == 0) {
		return NULL;
	}
	off = 0;
	off = vs_pickup_token(site->mp, domain_s, off, ",", &domain);
	ASSERT(domain);

	vs_trim_string(domain);

	domain_root = scx_list_create(site->mp, LIST_TYPE_SIMPLE_KEY, 0);
	scx_list_append(domain_root, vs_data(domain), NULL, 0);

	/* (메인 domain 에 연결될) hash 에 alias 도메인 등록 (위의 vs_pickup_token() 에서 셋팅된 off 값을 초기화 하면 안된다.) */
	while (domain) {
		off = vs_pickup_token(site->mp, domain_s, off, ",", &domain);
		/* alias 도메인이 있나? */
		if (domain)
		{
			vs_trim_string(domain);
			if (scx_list_find_key(domain_root, vs_data(domain), 1) != NULL) {
				/* domain_list에 중복 도메인이 있는 경우는 warning 메시지를 출력한다. */
				TRACE((T_WARN, "domain(%s) configuration was duplicated.\n", vs_data(domain_s)));
				continue;
			}
			scx_list_append(domain_root, vs_data(domain), NULL, 0);
		}
	}

	return domain_root;
}

/*
 * 지정된 토큰 문자열(sep)을 사용해서 string을 파싱한후
 * list의 key를 생성한다.
  */
scx_list_t *
vm_parse_string_by_token(mem_pool_t *mpool, vstring_t *string, const char *sep)
{
	scx_list_t *list_root = NULL;
	off_t 	off = 0;
	vstring_t	*token = NULL;

	ASSERT(mpool);
	off = 0;
	off = vs_pickup_token(mpool, string, off, sep, &token);
	if (token == NULL) {
		TRACE((T_WARN, "empty token\n"));
		return NULL;
	}


	vs_trim_string(token);

	list_root = scx_list_create(mpool, LIST_TYPE_SIMPLE_KEY, 0);
	scx_list_append(list_root, vs_data(token), NULL, 0);

	while (token) {
		off = vs_pickup_token(mpool, string, off, ",", &token);

		if (token)
		{
			vs_trim_string(token);
			scx_list_append(list_root, vs_data(token), NULL, 0);
		}
	}

	return list_root;
}

/*
 * 설정에 지정 문자열을 파싱해서 sub_domain_list에 넣는다.
 * 입력 형태 : vol1:vol1-solsvc.lgucdn.com, vol2:vol2-solsvc.lgucdn.com, vol3:vol3-solsvc.lgucdn.com
  */
scx_list_t *
vm_parse_sub_domain_list(service_info_t *service, vstring_t *string)
{
	scx_list_t *list_root = NULL;
	off_t 	off = 0;
	vstring_t	*token = NULL;
	site_t*		site = service->site;
	vstring_t 		*vs_o = NULL;
	mem_pool_t 			*mpool = service->site->mp;
	int	colon_off; /* :포함 */
	vstring_t 		*voldomain = NULL;
	vstring_t 		*volname = NULL;


	vs_trim_string(string);

	list_root = scx_list_create(mpool, LIST_TYPE_KEY_DATA, 0);	/* list에 추가시 data는 copy를 하지 않고 포인터만 저장한다. */
	off = vs_pickup_token(mpool, string, off, ",", &vs_o);
	while (vs_o)
	{
		vs_trim_string(vs_o);

		/* 문자열 뒷부분 부터 거꾸로 작업 한다. */
		colon_off = vs_pickup_token_r(mpool, vs_o, vs_length(vs_o), ":", &voldomain, DIRECTION_RIGHT);
		if(voldomain == NULL) {
			/* volname과 voldomain이 같이(volname:voldomain 형식) 지정되지 않는 경우는 skip한다.*/
			continue;
		}
		else {
			vs_truncate(vs_o, colon_off);
			volname = vs_strdup(mpool, vs_o);
		}
		/* 리스트에 해당 볼륨을 추가 한다. */
		scx_list_append(list_root, vs_data(volname), (void *)vs_data(voldomain), 0);

		/* 다음 set이 있으면 읽는다. */
		off = vs_pickup_token(mpool, string, off, ",", &vs_o);
	}

	return list_root;
}

char *
vm_find_voldomain_from_sub_domain_list(nc_request_t *req, const char *volname)
{
	char *voldomain = NULL;
	scx_list_t *list_root = req->service->sub_domain_list;
	if (list_root == NULL) return NULL;
	voldomain = (char *) scx_list_find_data(req->service->sub_domain_list, volname, 0);
	return voldomain;

}

/*
 * service에 연결된 domain들을 hash table에서 지우고
 * service를 백업 list로 옮긴다.
 */
int
vm_move_domain_hash_to_list(service_info_t *service)
{
	scx_list_t *root = NULL;

	int count = 0, i;
	char *domain = NULL;

	root = service->domain_list;
	count = scx_list_get_size(root);

	for(i = 0; i < count; i++) {
		domain = scx_list_get_by_index_key(root,i);
		ASSERT(domain);
		vm_delete_domain_from_hash(service, domain);
	}

	iList.Add(scx__past_service_list, (void *)&service);
	TRACE((T_DAEMON, "service(%s) was moved to backup list.\n", vs_data(service->name)));
	return count;
}

/*
 * 백업되었던 설정을 복구 한다.
 */
int
vm_restore_domain_hash_from_list(service_info_t *service)
{
	scx_list_t *root = NULL;

	int count = 0, i;
	char *domain = NULL;
	scx_list_t *domain_root = NULL;
	int ret = 0;

	domain_root = service->domain_list;
	count = scx_list_get_size(domain_root);
#if 0
	for(i = 0; i < count; i++) {
		domain = scx_list_get_by_index_key(domain_root,i);
		ASSERT(domain);
		ret = vm_insert_domain_to_hash(service, domain);
		ASSERT(0 == ret);
	}
#endif
	domain = scx_list_get_by_index_key(domain_root,0);
	/* netcache 쪽 rollback 을 한다.. */
	ret = vm_netcache_config(service, domain);
	if(0 > ret) {
		TRACE((T_WARN, "service(%s) core rollback failed.\n", domain));
	}
#if 0
	ret = iList.Erase(scx__past_service_list, (void *)&service);
	if (0 > ret) {
		TRACE((T_ERROR, "past list Erase() failed.(%s)\n", iError.StrError(ret)));
		i++;
		ASSERT(0); /* 어떤때 발생하는지 모르므로 일단 멈춘다. */
	}
#endif
	service->revision = gscx__load_revision;	/* revision을 최신으로 갱신한다. */
	TRACE((T_WARN, "service(%s) was restored.\n", vs_data(service->name)));
	return count;
}

/*
 * 지정된 파일을 읽어서 앞쪽 10KB부분의 hash를 만든다.
 * 문제가 발생한 경우 0을 리턴한다.
 */
uint32_t
vm_make_config_hash(const char *path, off_t *st_size)
{
	char *buf = NULL;
	int	buffer_size = 0;
	ssize_t copied = 0;
	uint32_t hash = 2166136261U;
	const uint8_t* ptr = NULL;
	int file = 0;
	int to_read = 0;
	int	pos = 0;
	struct stat	st;

	file = open(path, O_RDONLY);
	if (0 > file) {
		//ASSERT(0);
		TRACE((T_WARN, "Configuration file(%s) open fail.(%s)\n", path, strerror(errno)));;
		return 0;
	}
	fstat(file, &st);
	*st_size = st.st_size;
	if (10240 < st.st_size) {
		/* 설정 파일의 크기가 10kB보다 큰 경우에는 10kB까지만 hash를 만든다. */
		buffer_size = 10240;
	}
	else {
		buffer_size = (int) st.st_size;
	}

	buf = SCX_MALLOC(buffer_size+1);
	*(buf+buffer_size) = '\0';

	to_read = buffer_size;

	while(pos < buffer_size) {
		copied = read((int)file, buf+pos, to_read);
		ASSERT(0 <= copied);
		to_read -= copied;
		pos += copied;
	}

    for (ptr = (uint8_t *)buf; *ptr;) {
        hash = (hash ^ *ptr++) * 16777619U;
    }
    if (0 == hash) hash = 1; /* 0인 경우는 에러를 표시하므로 혹시 hash 값이 0이 나온 경우는 1로 변환해서 리턴 한다. */
    SCX_FREE(buf);
    close(file);
    return hash;
}

/*
 * 로딩된 모든 볼륨들에 volume purge 명령을 보낸다. *
 */
int
vm_all_volume_purge()
{
	service_info_t *service = NULL;
	char		*host;
	dict_itor 	*itor = NULL;
	int 		ret = 0;
	char 		purge_path[3] = {"/*"};

	pthread_rwlock_wrlock(&__services_lock);
	gscx__purge_revision++;
	if (scx__service_dct) {
		itor = dict_itor_new(scx__service_dct);
		dict_itor_first(itor);
		while (dict_itor_valid(itor)) {
			service = (service_info_t *)*dict_itor_datum(itor);
			if (NULL != service->core->mnt &&
					gscx__purge_revision != service->core->purge_revision) {	/* domain을 여러개 사용하는 경우 purge가 여러번 호출되는것을 방지하기 위해 사용 */
				ret = nc_purge(service->core->mnt, purge_path, FALSE /* iskey */, FALSE /* ishard */);
				TRACE((T_INFO, "'%s' volume purged.\n", vs_data(service->name)));
				service->core->purge_revision = gscx__purge_revision;
			}
			dict_itor_next(itor);
		}
		dict_itor_free(itor);
	}

	pthread_rwlock_unlock(&__services_lock);
	return SCX_YES;
}



