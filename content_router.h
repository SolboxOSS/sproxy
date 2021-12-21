#ifndef __CONTENT_ROUTER_H__
#define __CONTENT_ROUTER_H__

#define CR_VERSION		1

typedef enum {
	CR_POLICY_ROUTE_NEXT_SERVER	= 0, 	/* 다른 서버 선정 */
	CR_POLICY_NO_ROUTE			= 1 	/* routing 안함 */
} cr_policy_t;

/* redis에 저장되는  FHS 상태 정보 */
typedef struct {
	int		version; 	/* CR_VERSION과 틀린 경우 해당 FHS는 무시한다. */
	char 	domain[40];	/* FHS 도메인이나 IP */
	int		current_traffic;	/* 현재 traffic (단위 : Mbps) */
	int		threshold_traffic;	/* 서비스 가능 최대 트래픽 (단위 : Mbps) */
	int		cpu_usage;		/* CPU 사용률 (100% 기준) */
	int		threshold_cpu_usage;/*서비스 가능 최대 CPU 사용률 (100% 기준) */
	time_t	update_time;	/* 마지막 업데이트시간 */
} cr_fhs_stat_info_t;


int cr_start_service();
int cr_stop_service();
int cr_update_config(site_t * site);
char *cr_find_node(nc_request_t *req, const char *path);

#endif /* __CONTENT_ROUTER_H__ */
