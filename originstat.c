#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <memory.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>

//#include <ncapi.h>
#include <microhttpd.h>
#include <dict.h>	/* libdict */

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "scx_timer.h"
#include "scx_util.h"
#include "scx_list.h"
#include "module.h"
#include "originstat.h"
#include "stat_protocol.h"


module_driver_t gscx__originstat_module = {
	.name 				= "originstat",	/* name */
	.desc				= "origin status processing module",	/* description */
	.version			= 1,				/* version */
	.init				= originstat_module_init,	/* init func */
	.deinit				= originstat_module_deinit,	/* deinit func */
	.index				= -1,
	.callback			= NULL,
};


#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

#define SOCKET_STATUS_OK			0
#define SOCKET_STATUS_FAIL			1
#define SOCKET_STATUS_DEFAULT		2

#define SOLBOX_UDP_SIZE_MAX		65507
#define STAT_DOMAIN_MAX			256

#define ORIGIN_STAT_PERIOD			5		/* Origin 전송 간격, 초단위 */

typedef struct service_origin_stat_tag {
	char 		domain[STAT_DOMAIN_MAX];
	service_core_t 		*core;		/* 볼륨의 실제 통계가 업데이트 되는 부분은 core 부분에 들어 있음 */
	uint64_t 	prev_origin_request;
	uint64_t 	current_origin_request;
	uint64_t	period_origin_request;
	uint64_t 	prev_origin_response;
	uint64_t 	current_origin_response;
	uint64_t	period_origin_response;
	time_t		prev_time;	/* 마지막 전송 시간 */
	int			enable; /* 0인 경우 통계 전송 안함, 1인 경우 통계 전송 */
} service_origin_stat_t;


#if 0
struct _hdr_t {
	int size; // 바디의 크기, ics_real_pkt_type, ics_http_pkt_type, ics_isp_pkt_type와 같은 구 프로토콜들은 헤더의 크기를 포함한다.
	int type;
} __attribute((packed));
#endif

typedef struct {
	struct _hdr_t header;
	char buffer[SOLBOX_UDP_SIZE_MAX];
} __attribute((packed)) bulk_packet;

typedef struct origin_stat_ctx_tag {
	struct sockaddr_un		addr;
	socklen_t				addr_len;
	int						count;
	int						offset;
	int						sock;
	int						status;
	bulk_packet				packet;
} __attribute((packed)) origin_stat_ctx_t;

typedef struct {
	struct _hdr_t header;
	struct _origin_pkt_t body;
} __attribute((packed)) origin_packet_t;

pthread_mutex_t __oirginstat_lock = PTHREAD_MUTEX_INITIALIZER;
bt_timer_t 	gscx__timer_originstat;
int			gscx__timer_originstat_inited = 0;	/* 모듈이 최화된 상태인지 여부 */
vstring_t 	*gscx__stat_socket_path = NULL;
origin_stat_ctx_t gscx__stat_ctx;
dict 		*gscx__originstat_dct = NULL;
mem_pool_t 	*gscx__originstat_mpool = NULL;

int originstat_module_volume_create(phase_context_t *ctx);
int originstat_module_volume_reload(phase_context_t *ctx);
int originstat_module_volume_destroy(phase_context_t *ctx);

int originstat_module_insert_domain_to_hash(service_origin_stat_t * origin_stat, const char *domain);
int originstat_module__delete_domain_from_hash(const char *domain);
void originstat_timer_func(void *cr);
static void originstat_make_bulkpacket(service_origin_stat_t *origin_stat);
static void originstat_set_nonblock(int socket);
void originstat_send(origin_stat_ctx_t *stat_ctx);
void originstat_key_val_free(void *key, void *datum);

int
originstat_init()
{

	vstring_t 		*rv_chars = NULL;
	int stat_enable = 1;
	int standalone_stat_enable = 0;
	ASSERT(gscx__timer_wheel_base);
	standalone_stat_enable = scx_get_uint(gscx__default_site, SV_STAT_WRITE_ENABLE, 0);
	if (1 == standalone_stat_enable) {
		/*
		 * default.conf에서 standalone_stat_enable이 1인 경우는 오리진 통계를 따로 전송 statd에 전송할 필요가 없기때문에
		 * 통계 모듈을 아예 로드 하지 않는다.
		 */
		TRACE((T_INFO, "%s module disabled.(reason : StandAlone stat enabled)\n", gscx__originstat_module.name));
		return SCX_NO;
	}

	stat_enable = scx_get_uint(gscx__default_site, SV_STAT_ENABLE, 1);
	if (0 == stat_enable) {
		/*
		 * default.conf에서 통계 OFF가 설정 된 경우
		 * 통계 모듈을 아예 로드 하지 않는다.
		 */
		TRACE((T_INFO, "%s module disabled.\n", gscx__originstat_module.name));
		return SCX_NO;
	}

	TRACE((T_INFO, "%s module enabled.\n", gscx__originstat_module.name));

	bt_init_timer(&gscx__timer_originstat, "origin status timer", 0);
	gscx__stat_socket_path = scx_get_vstring_lg(gscx__default_site, SV_STAT_SOCKET_PATH, "/tmp/statd-org.sock");

	gscx__timer_originstat_inited = 1;

	gscx__stat_ctx.count		= 0;
	gscx__stat_ctx.offset		= 0;
	gscx__stat_ctx.sock			= -1;
	gscx__stat_ctx.status		= SOCKET_STATUS_DEFAULT;

	gscx__stat_ctx.addr.sun_family = AF_UNIX;
	gscx__stat_ctx.addr_len = sizeof(struct sockaddr_un);
	strncpy(gscx__stat_ctx.addr.sun_path, vs_data(gscx__stat_socket_path), min(vs_length(gscx__stat_socket_path) + 1, UNIX_PATH_MAX-1));
	TRACE((T_INFO, "Stat socket path : '%s'\n", gscx__stat_ctx.addr.sun_path));


	gscx__originstat_dct = hashtable_dict_new((dict_compare_func)strcmp, dict_str_hash, DICT_HSIZE);
	ASSERT(dict_verify(gscx__originstat_dct));

	bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)&gscx__timer_originstat, 10000 , originstat_timer_func, NULL);
	return SCX_YES;
}

int
originstat_deinit()
{
	if (gscx__timer_originstat_inited) {
#ifdef BT_TIMER_VER2
		while (bt_del_timer_v2(gscx__timer_wheel_base, &gscx__timer_originstat) < 0) {
			bt_msleep(1);
		}
		while (bt_destroy_timer_v2(gscx__timer_wheel_base, &gscx__timer_originstat) < 0) {
			bt_msleep(1);
		}
#else
		bt_del_timer(gscx__timer_wheel_base, &gscx__timer_originstat);
		bt_destroy_timer(gscx__timer_wheel_base, &gscx__timer_originstat);
#endif
		gscx__timer_originstat_inited = 0;
	}

	if (gscx__originstat_dct) {
		dict_free(gscx__originstat_dct, originstat_key_val_free);
		gscx__originstat_dct = NULL;
	}
	if (gscx__originstat_mpool) {
		mp_free(gscx__originstat_mpool);
		gscx__originstat_mpool = NULL;
	}

	return SCX_YES;
}


int
originstat_module_init()
{
	int ret = 0;
	ret = originstat_init();
	if (SCX_YES != ret) return SCX_YES; /* 모듈 동작이 필요 없는 경우 phase handler를 등록할 필요가 없다. */

	scx_reg_phase_func(PHASE_VOLUME_CREATE, originstat_module_volume_create);
	scx_reg_phase_func(PHASE_VOLUME_RELOAD, originstat_module_volume_reload);
	scx_reg_phase_func(PHASE_VOLUME_DESTROY, originstat_module_volume_destroy);
	/* 여기서 통계 관련 default 설정을 읽고 socket을 연다. */


	return SCX_YES;
}

int
originstat_module_deinit()
{

	originstat_deinit();

	return SCX_YES;
}

void
originstat_key_val_free(void *key, void *datum)
{

}


int
originstat_module_volume_create(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	int	stat_enable = 0;
	service_origin_stat_t * origin_stat = NULL;
	int index = gscx__originstat_module.index;
	/* 특수 볼륨은 통계를 제외한다. 아니면 전송 시에만 제거?, 이경우 아래의 origin_stat->enable에만 marking, reload시 동일 */
	if (0 == vs_strcmp_lg(service->name, OTHERS_VOLUME_NAME))
		return SCX_YES;
	else if (0 == vs_strcmp_lg(service->name, CHECK_VOLUME_NAME))
		return SCX_YES;
	stat_enable = scx_get_uint(service->site, SV_STAT_ENABLE, 1);
	origin_stat = (service_origin_stat_t *)SCX_CALLOC(1, sizeof(service_origin_stat_t));
	service->core->module_ctx[index] = (void *)origin_stat;
	origin_stat->core = service->core;
	origin_stat->enable = stat_enable;
	TRACE((T_DAEMON|T_DEBUG, "'%s' service origin stat context created.(%d)\n",  vs_data(service->name), index ));
	snprintf(origin_stat->domain, STAT_DOMAIN_MAX, "%s", vs_data(service->name));
	originstat_module_insert_domain_to_hash(origin_stat, origin_stat->domain);

	/* gscx__originstat_dct 사용 하는 모든 부분에 lock을 넣으면 별도의 동기화 문제는 고민 하지 않아도 될것 같음
	 * lock을 거는 범위에 대한 검토 필요 */

	return SCX_YES;
}

int
originstat_module_volume_reload(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	int index = gscx__originstat_module.index;
	int	stat_enable = 0;
	service_origin_stat_t * origin_stat = (service_origin_stat_t *)service->core->module_ctx[index];

	/* 특수 볼륨은 통계를 제외한다. */
	if (0 == vs_strcmp_lg(service->name, OTHERS_VOLUME_NAME))
		return SCX_YES;
	else if (0 == vs_strcmp_lg(service->name, CHECK_VOLUME_NAME))
		return SCX_YES;
	ASSERT(origin_stat);
	stat_enable = scx_get_uint(service->site, SV_STAT_ENABLE, 1);
	if (stat_enable != origin_stat->enable) {
		/* 통계 수집상태가 변경 된 경우 모든 값을 리셋한다 */
		origin_stat->prev_origin_request = 0;
		origin_stat->current_origin_request = 0;
		origin_stat->period_origin_request = 0;
		origin_stat->prev_origin_response = 0;
		origin_stat->current_origin_response = 0;
		origin_stat->period_origin_response = 0;
		origin_stat->prev_time = 0;
	}
	origin_stat->enable = stat_enable;
	return SCX_YES;
}

int
originstat_module_volume_destroy(phase_context_t *ctx)
{
	service_info_t *service = ctx->service;
	int index = gscx__originstat_module.index;
	service_origin_stat_t * origin_stat = (service_origin_stat_t *)service->core->module_ctx[index];
	/* 특수 볼륨은 통계를 제외한다. */
	if (0 == vs_strcmp_lg(service->name, OTHERS_VOLUME_NAME))
		return SCX_YES;
	else if (0 == vs_strcmp_lg(service->name, CHECK_VOLUME_NAME))
		return SCX_YES;
	if (NULL != origin_stat) {
		originstat_module__delete_domain_from_hash(origin_stat->domain);
		SCX_FREE(service->core->module_ctx[index]);
		service->core->module_ctx[index] = NULL;
	}

	return SCX_YES;
}



/*
 * 중요 : domain 파라미터의 포인터는 해당 dict가 삭제될때 까지 유지 되어야 한다.
 */
int
originstat_module_insert_domain_to_hash(service_origin_stat_t * origin_stat, const char *domain)
{
	void **datum_location = NULL;
	pthread_mutex_lock(&__oirginstat_lock);

	dict_insert_result result = dict_insert(gscx__originstat_dct, (void *)domain);
	pthread_mutex_unlock(&__oirginstat_lock);
	if (!result.inserted) {
		TRACE((T_WARN, "Already registered stat domain(%s).\n", domain));
		return -1;
	}
	ASSERT(result.datum_ptr != NULL);
	ASSERT(*result.datum_ptr == NULL);
	*result.datum_ptr = origin_stat;


	TRACE((T_INFO, "stat domain for '%s' - successfully added.\n", domain));
	return 0;
}

int
originstat_module__delete_domain_from_hash(const char *domain)
{
	pthread_mutex_lock(&__oirginstat_lock);
	dict_remove(gscx__originstat_dct, (void *)domain);
	pthread_mutex_unlock(&__oirginstat_lock);
	TRACE((T_INFO, "stat domain for '%s' - successfully removed.\n", domain));
	return 0;
}

void
originstat_timer_func(void *cr)
{
	TRACE((T_DAEMON|T_DEBUG, "call %s func\n", __func__ ));
	/* 여기에 statd 전송 로직이 들어 가야 한다 */
	/* 먼저 timer 등록을 하고 이후에 다른 작업을 한다. */
	bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)&gscx__timer_originstat, ORIGIN_STAT_PERIOD * 1000 , originstat_timer_func, NULL); /* 5초 마다 실행 */

	{
		dict_itor *itor = NULL;
		service_origin_stat_t * origin_stat = NULL;
		service_core_t 		*core = NULL;
		pthread_mutex_lock(&__oirginstat_lock);
		if (gscx__originstat_dct) {
			itor = dict_itor_new(gscx__originstat_dct);
			dict_itor_first(itor);
			while (dict_itor_valid(itor)) {
				origin_stat = (service_origin_stat_t *)*dict_itor_datum(itor);
				ASSERT(origin_stat);
				core = origin_stat->core;
				ASSERT(core);
				origin_stat->prev_origin_request = origin_stat->current_origin_request;
				origin_stat->prev_origin_response = origin_stat->current_origin_response;
				origin_stat->current_origin_request = core->counter_outbound_request;
				origin_stat->current_origin_response = core->counter_outbound_response;
				origin_stat->period_origin_request = origin_stat->current_origin_request - origin_stat->prev_origin_request;
				origin_stat->period_origin_response = origin_stat->current_origin_response - origin_stat->prev_origin_response;
				origin_stat->prev_time = scx_update_cached_time_sec();	/* 이부분은 실제 전송 부분에서 넣으면 될듯 */
//				printf("origin stat(%s), response = %lld\n", origin_stat->domain, origin_stat->period_origin_response);
				/* 통계가 on 되는 경우에만 전송한다. */
				if (origin_stat->enable)
					originstat_make_bulkpacket(origin_stat);
				dict_itor_next(itor);
			}
			dict_itor_free(itor);
		}
		pthread_mutex_unlock(&__oirginstat_lock);
		/* bulk buffer쪽에 전송할 데이터가 남아 있으면 전송 한다 */
		if ( 0 < gscx__stat_ctx.offset) {
			originstat_send(&gscx__stat_ctx);
		}
	}
}


static void
originstat_make_bulkpacket(service_origin_stat_t *origin_stat)
{
	origin_packet_t *origin_pkt = NULL;
	int bound = SOLBOX_UDP_SIZE_MAX - sizeof(origin_packet_t);

	/* 버퍼에 패킷을 담을 공간이 남아 있지 않으면 버퍼의 내용을 소켓을 통해서 전송한후  버퍼를 비우고 다음 작업을 진행한다. */
	if ( gscx__stat_ctx.offset > bound ) {
		originstat_send(&gscx__stat_ctx);
	}
	origin_pkt = (origin_packet_t *)&gscx__stat_ctx.packet.buffer[gscx__stat_ctx.offset];
	/* origin_pkt->body.domain을 STAT_DOMAIN_MAX 크기로 사용하는게 아니라 domain length+1로 만들어서 전송한다. */
	origin_pkt->header.size	= sizeof(struct _origin_pkt_t) - STAT_DOMAIN_MAX +  strlen(origin_stat->domain) + 1;
	origin_pkt->header.type	= origin_pkt_type;
	origin_pkt->body.stattime = origin_stat->prev_time;
	origin_pkt->body.period = ORIGIN_STAT_PERIOD * 1000;
	origin_pkt->body.tx = origin_stat->period_origin_request;
	origin_pkt->body.rx = origin_stat->period_origin_response;
	origin_pkt->body.protocol = PROTOCOL_HTTP;
	snprintf(origin_pkt->body.domain, STAT_DOMAIN_MAX, "%s", origin_stat->domain);

	//gscx__stat_ctx.offset		+= sizeof(origin_packet_t);
	gscx__stat_ctx.offset		+= sizeof(struct _hdr_t) + origin_pkt->header.size;
	gscx__stat_ctx.count++;

	TRACE((T_DAEMON|T_DEBUG, "%s, offset = %d, count = %d, size = %d, type = %d, tx = %lld, rx = %lld, domain = %s\n",
			__func__ , gscx__stat_ctx.offset, gscx__stat_ctx.count-1,
			origin_pkt->header.size, origin_pkt->header.type,
			origin_pkt->body.tx, origin_pkt->body.rx, origin_pkt->body.domain));

}


static void
originstat_set_nonblock(int socket)
{
    int flags;

    flags = fcntl(socket,F_GETFL,0);
    if(flags == -1)
    {
    	TRACE((T_ERROR, "%s, failed to get socket(%s)\n", __func__ , strerror(errno)));
    	return;
    }
    fcntl(socket, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(socket,F_GETFL,0);
    if(flags == -1)
    {
    	TRACE((T_ERROR, "%s,  failed to set socket(%s)\n", __func__ , strerror(errno)));
    }
    return;
}

void
originstat_send(origin_stat_ctx_t *stat_ctx)
{
	int retry_count = 2;
	if (stat_ctx->sock == -1)
	{
		stat_ctx->sock= socket(AF_UNIX, SOCK_DGRAM, 0);

		if (stat_ctx->sock== -1)
		{
			TRACE((T_ERROR, "%s, failed to init socket, %s\n", __func__ , strerror(errno)));

		}
		originstat_set_nonblock(stat_ctx->sock);
	}

	stat_ctx->packet.header.size = stat_ctx->offset;
	stat_ctx->packet.header.type = bulk_pkt_type;
	TRACE((T_DAEMON|T_DEBUG, "%s, bulk packet size: %d\n",
								__func__ , stat_ctx->packet.header.size));

	if (stat_ctx->sock != -1)
	{
#if 0  //전송 시간 검증용
		double 				ts, te, elapsed;
		ts = sx_get_time();
#endif
		TRACE((T_DAEMON|T_DEBUG, "%s, packet count %d going to send\n", __func__ , stat_ctx->count));
		do
		{
			if (sendto(stat_ctx->sock, (void*)&(stat_ctx->packet), stat_ctx->packet.header.size + sizeof(struct _hdr_t),
						0, (struct sockaddr*)&(stat_ctx->addr), stat_ctx->addr_len) < 0)
			{
				if(errno == EAGAIN)
				{	//socket buffer가 다 찬 경우
					TRACE((T_ERROR, "%s, origin stat(%d) packet buffer full (%d, %s)\n", __func__ , stat_ctx->packet.header.type, errno, strerror(errno)));
				}
				else
				{
					if (stat_ctx->status == SOCKET_STATUS_OK
							|| stat_ctx->status == SOCKET_STATUS_DEFAULT)
						TRACE((T_ERROR, "%s, origin stat(%d) packet send fail(%d, %s)\n",
								__func__ , stat_ctx->packet.header.type , errno, strerror(errno)));


					//socket은 process가 종료시에만 닫는다.
					//				close(stat->sock);
					//				stat->sock = -1;
					stat_ctx->status = SOCKET_STATUS_FAIL;
				}
				retry_count--;
				continue;
			}
			else
			{
				if (stat_ctx->status == SOCKET_STATUS_FAIL
						|| stat_ctx->status == SOCKET_STATUS_DEFAULT)
					TRACE((T_DAEMON|T_DEBUG, "%s, origin stat(%d) packet send ok\n",
							__func__ , stat_ctx->packet.header.type));


				stat_ctx->status = SOCKET_STATUS_OK;
				break;
			}
		}
		while( retry_count > 0 );
#if 0
		te = sx_get_time();
		elapsed = te - ts;
		if (1000 < elapesed) {
			TRACE((T_WARN, "%s, %lld elapsed, size = %d", __func__,  elapsed, stat_ctx->packet.header.size));
		}

#endif
	}

	stat_ctx->count = 0;
	stat_ctx->offset = 0;
}

