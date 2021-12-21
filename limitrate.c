#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory.h>
#include <pthread.h>
#include <stdlib.h>
#include <fcntl.h>
//#include <ncapi.h>
#include <microhttpd.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
#include "scx_util.h"
#include "scx_timer.h"
#include "limitrate.h"
#include "streaming.h"



void 	wakeup_connection(void *cr);


int
limitrate_make(void *cr)
{
	nc_request_t 	*req = cr;
	bt_timer_t *timer = NULL;
	ASSERT(gscx__timer_wheel_base);
	timer = SCX_CALLOC(1, sizeof(bt_timer_t));
	req->limit_rate_timer = (void *)timer;
	bt_init_timer(req->limit_rate_timer, "traffic control timer", 0);
	TRACE((T_DEBUG, "[%llu] limit rate init timer.\n", req->id));

	return SCX_YES;
}

void
limitrate_clean(void *cr)
{
	nc_request_t 	*req = cr;
	if (req->limit_rate_timer) {
#ifdef BT_TIMER_VER2
	while (bt_del_timer_v2(gscx__timer_wheel_base, (bt_timer_t *)req->limit_rate_timer) < 0) {
		bt_msleep(1);
	}
	while (bt_destroy_timer_v2(gscx__timer_wheel_base, (bt_timer_t *)req->limit_rate_timer) < 0) {
		bt_msleep(1);
	}
#else
	bt_del_timer(gscx__timer_wheel_base, (bt_timer_t *)req->limit_rate_timer);
	bt_destroy_timer(gscx__timer_wheel_base, (bt_timer_t *)req->limit_rate_timer);
#endif
		SCX_FREE(req->limit_rate_timer);
		req->limit_rate_timer = NULL;
		TRACE((T_DEBUG, "[%llu] limit rate clean timer.\n", req->id));
	}
}

size_t
limitrate_control(void *cr, size_t max, int reader_type)
{
	nc_request_t 	*req = cr;
	/* 설정에 limit rate가 있는 경우 아래 동작 */
	if (0 < req->limit_rate || 0 < req->limit_traffic_rate) {
		if (req->async_ctx.skip_mtime > 0) {
			limitrate_suspend(cr);
			/* suspend 시에는 return을 0으로 해야지만 다시 호출 되지 않는다. */
			return 0;
		}
		if (1 == reader_type) {
			if (O_PROTOCOL_PROGRESSIVE != req->streaming->protocol) {
				/* 속도 제한은 계속 연결을 해서 받는 progressive download에만 건다.
				 * HLS, DASH등은 session 기반의 속도 제한 로직이 필요함. */
					return 0;
			}
		}
		max = limitrate_compute(cr, max);
	}
	return max;
}

/*
 * 전송 속도를 계산해서 설정된 전송속도 보다 빠르게 전송한 경우 MHD_suspend_connection()를 호출 한후
 * bt_set_timer()를 호출해서 일정시간 이후에 MHD_resume_connection()으로 깨어나도록 한다.
 */
size_t
limitrate_compute(void *cr, size_t max)
{
	nc_request_t 	*req = cr;
	double 			ts = 0.0;
	double 			elapsed = 0.0;
	double			cur_trans_Bps = 0.0; /* 현재의 전송 속도 */
	double			cal_trans_Bps = 0.0; /* 설정에 따른 전송속도 */
	ssize_t			res_trans_size = 0;  /* limit_rate_after를 제외한 현재 전송량 */
	ssize_t			cal_trans_size = 0;  /* 설정에 따른 예상 전송량 */
	ssize_t			exceed_size = 0;	/* 초과 전송량 */
	unsigned int	skip_time = 0;			/* 쉬어야 하는 시간 */
	int				limit_rate = 0;
	ssize_t			limit_rate_after = 0;

#if 0
	if (!req->limit_rate_timer) goto limitrate_compute_end;
#else
	if (!req->limit_rate_timer) limitrate_make(cr);
#endif

	/* 처음 패킷 전송 전이나 읽기 실패인 경우, 전송완료인 경우에는 더 진행 할 필요가 없다 */
	if (0 == req->res_body_size || 0 >= max || 0 >= req->remained) goto limitrate_compute_end;

	limit_rate = req->limit_rate * 1024; /* 단위를 Byte/sec 로 변경 */
	limit_rate_after = req->limit_rate_after * 1024;

	if (0 < limit_rate_after) {
		if (req->res_body_size < limit_rate_after)  {

			/*
			 * limit_rate_after가 설정된 상태에서 전송량이 limit_rate_after 보다 작은 경우는
			 * traffic 제한 없이 최대 속도로 전송 한다.
			 */
			if ((req->res_body_size + max) >= limit_rate_after) {
				/* 이번 턴을 전송하고 나면 limit_rate_after를 초과하는 경우 */
				req->t_res_sent = scx_update_cached_time_usec();

			}
			goto limitrate_compute_end;
		}
	}


	res_trans_size = req->res_body_size - limit_rate_after; /* limit_rate_after의 크기를 제외한 전송량을 재계산 한다. */
	if (res_trans_size <= (max*2)) {
		/* 처음 속도 제한이 없는 구간에서는 시간을 항상 새로 구한다. */
		ts = scx_update_cached_time_usec();
	}
	else {
		ts = scx_get_cached_time_usec();
	}

	/* req->t_res_sent를 기준 시간으로 해서 현재의 전송된 량이 설정된 전송속도보다 많을 경우 일정시간동안 쉬도록 한다. */

	elapsed = (ts - req->t_res_sent) / 1000000.0;
	if (0.0 >= elapsed) goto limitrate_compute_end; /* 너무 작은 시간은 의미가 없으므로 skip 한다. */
	cur_trans_Bps = (double)res_trans_size / elapsed;
	cal_trans_size = limit_rate * elapsed;
	if (cal_trans_size < res_trans_size) {
		exceed_size = res_trans_size - cal_trans_size;
		/* 계산식 :  쉬어야 하는 시간 = (초과용량 * 1000(msec로 변환)) / (설정속도 * 1024(KBps로 변환)) */
		skip_time = (exceed_size * 1000) / limit_rate ;
		if(TIMER_RESOLUTION < skip_time) { /* timer thread의 해상도 보다 작은 경우에는 그냥 넘어 간다. */
			if (skip_time > 1000) {		/* 전송속도를 초과하더라도 1초에는 한번은 전송하도록 한다. */
				max = 4096;
				skip_time = 1000;
			}
#if 0
			MHD_suspend_connection(req->connection);
			bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)req->limit_rate_timer, skip_time , wakeup_connection, cr);
			TRACE((T_DEBUG, "[%llu] url(%s), %f KBps, exceed  %llu byte, sleep %d msec\n",
					req->id, vs_data(req->url), cur_trans_Bps/1024.0 , exceed_size, skip_time));
#else
			TRACE((T_DEBUG, "[%llu] url(%s), %f KBps, exceed  %llu byte\n",
					req->id, vs_data(req->url), cur_trans_Bps/1024.0 , exceed_size));
#endif
		}
	}
limitrate_compute_end:
	req->async_ctx.skip_mtime = skip_time;
	return max;
}

/* connection에 대해 suspend를 하고 timer에 이벤트 등록을 한다. */
int
limitrate_suspend(void *cr)
{
	nc_request_t 	*req = cr;
	ASSERT(req->connection);
	req->is_suspeneded = 1;	/* 비동기 flag 셋팅 */
	MHD_suspend_connection(req->connection);
	bt_set_timer(gscx__timer_wheel_base, (bt_timer_t *)req->limit_rate_timer, req->async_ctx.skip_mtime , wakeup_connection, cr);
	TRACE((T_DEBUG, "[%llu] url(%s), sleep %d msec\n", req->id, vs_data(req->url), req->async_ctx.skip_mtime));
	req->async_ctx.skip_mtime = 0;
	return SCX_YES;
}

void
wakeup_connection(void *cr)
{
	/* connection이 끊기고 난 후에 호출 되는 경우 문제가 발생함 */
	nc_request_t 	*req = cr;
	ASSERT(req->connection);
	TRACE((T_DEBUG, "[%llu] limit rate wakeup timer.\n", req->id));
	req->is_suspeneded = 0;
	MHD_resume_connection(req->connection);
}


