#include <netcache_config.h>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <curl/curl.h>
#include <curl/easy.h>


#include <netcache.h>
#include <block.h>
#include "util.h"
#include "disk_io.h"
#include "trace.h"
#include "hash.h"
#include "httpn_driver.h"
#include "httpn_request.h"
#include "http_codes.h"



extern char	__zowner[][32]; 
extern char	_zmpxstate[][32];
extern void *__timer_wheel_base;
extern __thread httpn_mux_info_t	*g__current_mpx;

static char	__zevents[][32] = {
	"0:IOCB_EVENT_EXECUTE",
	"1:IOCB_EVENT_BEGIN_TRY",
	"2:IOCB_EVENT_PROPERTY_DONE",
	"3:IOCB_EVENT_PAUSE_TRY",
	"4:IOCB_EVENT_RESUME_TRY",
	"5:IOCB_EVENT_TIMEOUT_TRY",
	"6:IOCB_EVENT_END_TRY",
	"7:IOCB_EVENT_FINISH"
};


int
mpx_STM(httpn_io_context_t *iocb, httpn_iocb_event_t event, int finish, char *f, int l)
{
	int				r;
	char			dbuf[2048];
	int				tries = -1;
	int				de;
	nc_uint32_t		keptid = 0;
	int				res = 0;

	TRACE((T_PLUGIN, ">>>>>>>>>>>>>>>>>>>>>> IOCB[%d] : EVENT[%s]%s::: %s %d@%s\n", 
						iocb->id, 
						__zevents[event], 
						(finish?"/FINISH":""), 
						httpn_dump_iocb(dbuf, sizeof(dbuf), iocb),
						l, 
						f));
	DEBUG_ASSERT_IOCB(iocb, (iocb->mpx ==  g__current_mpx));
	tries	= iocb->tries;
	keptid	= iocb->keptid;
	switch (event) {
		case IOCB_EVENT_PROPERTY_DONE:
			//DEBUG_ASSERT_IOCB(iocb, iocb->state == IOCB_MPX_TRY_RUN);

			httpn_migrate_http_state(iocb, HS_EOH, __FILE__, __LINE__);

			if (httpn_handle_response_headers(iocb) < 0) {
				TRACE((T_PLUGIN, "***** IOCB[%d] - got error condition from inspecting response\n", iocb->id));
				iocb->last_errno = httpn_handle_error(iocb, &iocb->last_curle, &iocb->last_httpcode, &de);
				httpn_set_raw_code_ifpossible(iocb, iocb->last_httpcode); /*정상처리*/
				iocb->canceled = 1;
				res = -1; /* need abort */
				break;
			}
#if 0
			/*
			 * wait for application's next order
			 */
			if (httpn_is_state(iocb->session, iocb->target_action)) {
				/*
				 * application이 지정한 목표 상태 도달
				 * 다음 요청 때 까지 대기해야함(application과 동기 시점)
				 * 참고로 pause는 target_action이 HS_EOH가 아니라 HS_BODY라서
				 * 이 조건은 절대 불가능
				 */
				if (iocb->method != HTTP_POST) {
					iocb->last_errno = httpn_handle_error(iocb, &iocb->last_curle, &iocb->last_httpcode, &de);
					httpn_set_raw_code_ifpossible(iocb, iocb->last_httpcode); /*정상처리*/
					TRACE((T_PLUGIN, "IOCB[%d] : reached to the target action;%s\n", iocb->id, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
					apc_overlapped_signal(&iocb->event);
				}
			}
#endif
			break;
		case IOCB_EVENT_BEGIN_TRY:

			if (httpn_prepare_try(iocb) < 0) {
				/*
				 * preparation fail: multi에 add도 안된 상태?
				 * 여기에서 result세팅을 해야함
				 */
				TRACE((T_PLUGIN, "IOCB[%d] - preparion failed:%s\n", iocb->id, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
				iocb->last_curle	= 0;
				iocb->last_httpcode	= HTTP_BAD_GATEWAY;
				iocb->last_errno	= EREMOTEIO;
				httpn_set_raw_code_ifpossible(iocb, iocb->last_httpcode); 
				mpx_STM(iocb, IOCB_EVENT_FINISH, 0, __FILE__, __LINE__);
			}
			else {
				TRACE((T_PLUGIN, "IOCB[%d] - preparion ok:%s\n", iocb->id, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
				httpn_mpx_state_set(iocb, IOCB_MPX_TRY_RUN, __FILE__, __LINE__);
				httpn_mpx_timer_restart(iocb, __FILE__, __LINE__);
			}
			tries = iocb->tries; /* 변했음 */

			break;
		case IOCB_EVENT_PAUSE_TRY:
			httpn_mpx_timer_pause(iocb, __FILE__, __LINE__);
			httpn_mpx_state_set(iocb, IOCB_MPX_PAUSED, __FILE__, __LINE__);
			break;
		case IOCB_EVENT_RESUME_TRY:
			DEBUG_ASSERT_IOCB(iocb, iocb->state == IOCB_MPX_PAUSED);

			httpn_mpx_timer_restart(iocb, __FILE__, __LINE__);
			//httpn_mpx_state_set(iocb, IOCB_MPX_TRY_RUN, __FILE__, __LINE__);
			//
			break;
		case IOCB_EVENT_END_TRY:
			/*
			 * 이 event처리에서 curle와 httpcode는 확정되어야함
			 */
			DEBUG_ASSERT_IOCB(iocb, iocb->state == IOCB_MPX_TRY_RUN||iocb->state == IOCB_MPX_PAUSED);
			DEBUG_ASSERT_IOCB(iocb, iocb->last_curle >= 0);

			httpn_mpx_state_set(iocb, IOCB_MPX_TRY_DONE, __FILE__, __LINE__);

			r =  httpn_handle_try_result(iocb);


			if (finish == U_FALSE && r == HTTP_TRY_CONTINUE) {
				TRACE((T_PLUGIN, "IOCB[%d] - try result continue:%s\n", iocb->id, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
				httpn_mpx_state_set(iocb, IOCB_MPX_READY, __FILE__, __LINE__);
				mpx_STM(iocb, IOCB_EVENT_BEGIN_TRY, 0, __FILE__, __LINE__);
			}
			else  {
				TRACE((T_PLUGIN, "IOCB[%d] - try result finish:%s\n", iocb->id, httpn_dump_iocb(dbuf, sizeof(dbuf), iocb)));
				mpx_STM(iocb, IOCB_EVENT_FINISH, 0, __FILE__, __LINE__);
			}
			break;
		case IOCB_EVENT_TIMEOUT_TRY:
			//DEBUG_ASSERT_IOCB(iocb, iocb->state == IOCB_MPX_TRY_RUN);
			/* make result code*/
			iocb->last_curle	= CURLE_OPERATION_TIMEDOUT;
			iocb->last_httpcode = HTTP_GATEWAY_TIMEOUT;
			TRACE((T_PLUGIN, "IOCB[%d] - curle changed to OPERATION_TIMEOUT, httpcode set to GATEWAY_TIMEOUT\n", iocb->id));

			mpx_STM(iocb, IOCB_EVENT_END_TRY, 0, __FILE__, __LINE__);
			break;
		case IOCB_EVENT_FINISH:
			/*
			 * final state, cleanup and free memories
			 */
			apc_overlapped_signal(&iocb->event);
#ifndef NC_RELEASE_BUILD
			link_del(&g__current_mpx->q_run, &iocb->mpx_node);
#endif
			httpn_mpx_state_set(iocb, IOCB_MPX_FINISHED, __FILE__, __LINE__);
			httpn_cleanup_try(iocb);
			_ATOMIC_SUB(g__current_mpx->run, 1);
			break;
	}
	TRACE((T_PLUGIN, "«««««««««««««««««««««««« IOCB[%d] : EVENT[%s]%s DONE(%d-th(st) attempt)\n", keptid, __zevents[event], (finish?"/FINISH":""), tries));
	return res;
}

