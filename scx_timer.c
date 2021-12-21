#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory.h>
#include <pthread.h>
#include <sys/prctl.h>
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


void  		*gscx__timer_wheel_base = NULL;
pthread_t 		gscx__timer_thread_tid;
int				gscx__timer_working = 1;



void 	*scx_timer_thread(void *d);


int
scx_timer_init()
{
	if (!gscx__timer_wheel_base) {
		TRACE((T_DAEMON, "scx timer initializing\n"));
		gscx__timer_wheel_base = bt_init_timers(); /* bt_timer 라이브러리에 gscx__timer_wheel_base의 메모리를 해제하는 api가 없다 ㅠ.ㅠ */
	}

	pthread_create(&gscx__timer_thread_tid, NULL, scx_timer_thread, (void *)NULL);
	return 0;
}

void
scx_timer_deinit()
{
	if (gscx__timer_wheel_base) {
		gscx__timer_working = 0;
		pthread_join(gscx__timer_thread_tid, NULL);

		gscx__timer_wheel_base = NULL; /* gscx__timer_wheel_base를 초기화 하는 명령이 없다. */
		TRACE((T_DAEMON, "scx timer deinitialized.\n"));
	}
}

/*
 * loop을 돌면서 지정된 시간(100msec)마다 timer에 등록된 job들을 실행 시키고
 * cached time을 갱신 한다.
 */
void *
scx_timer_thread(void *d)
{
	prctl(PR_SET_NAME, "scx timer thread");
	TRACE((T_DAEMON, "%s timer scheduler thread started.\n",PROG_SHORT_NAME));
	while (gscx__timer_working) {
		bt_msleep(TIMER_RESOLUTION);
		scx_update_cached_time_usec();
		bt_run_timers(gscx__timer_wheel_base);
	}
	TRACE((T_DAEMON, "%s timer scheduler thread stoped.\n", PROG_SHORT_NAME));
}


