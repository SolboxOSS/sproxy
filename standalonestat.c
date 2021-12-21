#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <memory.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <setjmp.h>

//#include <ncapi.h>
#include <microhttpd.h>
#include <dict.h>	/* libdict */
#include <bt_timer.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "scx_util.h"
#include "status.h"
#include "scx_list.h"
#include "thpool.h"
#include "streaming.h"
#include "sessionmgr.h"
#include "standalonestat.h"

#define 	STAND_ALONE_TIMER_RESOLUTION	100
#define 	MAX_STAT_PATH_LENGTH			255

extern int				gscx__service_available;
extern __thread jmp_buf 	__thread_jmp_buf;
extern __thread int		__thread_jmp_working;

void  		*gscx__standalonestat_timer_wheel_base = NULL;
pthread_t 	gscx__standalonestat_timer_tid;
int			gscx__standalonestat_timer_working = 1;
bt_timer_t 	gscx__timer_standalonestat;
int			gscx__timer_standalonestat_inited = 0;	/* 모듈이 최화된 상태인지 여부 */
threadpool 	gscx__standalonestat_thpool = NULL; 	/* 통계 전용 thread pool 포인터 */
List		*gscx__standalonestat_service_list = NULL;	/* 통계에 사용되는 서비스들의 리스트를 가지고 있는 포인터 */

scx_logger_t 	*scx_origin_stat_logger 	= NULL;		/* origin 통계 기록용 log pointer */
scx_logger_t 	*scx_traffic_stat_logger 	= NULL;		/* traffic 통계 기록용 log pointer */
scx_logger_t 	*scx_nos_stat_logger 		= NULL;		/* nos 통계 기록용 log pointer */
scx_logger_t 	*scx_http_stat_logger 		= NULL;		/* content 통계 기록용 log pointer */

pthread_rwlock_t __standalonestat_lock = PTHREAD_RWLOCK_INITIALIZER;

static int standalonestat_create_thread_pool();
static int standalonestat_destroy_thread_pool();
void 	*standalonestat_timer_thread(void *d);
void 	standalonestat_timer_func(void *cr);
int	    standalonestat_check_directory();
void 	standalonestat_write_job(void * ptype);
void	standalonestat_write_http();
int		standalonestat_file_init();
int		standalonestat_file_deinit();
void	standalonestat_write_origin();
void	standalonestat_write_traffic();
void	standalonestat_write_nos();
void	standalonestat_nos_sec_peekcon();

typedef enum {

	STAT_TYPE_ERROR = 0,
	STAT_TYPE_WRITE,
	STAT_TYPE_NOS_PEEKCON,
	STAT_TYPE_ROTATE
} standalonestat_type_t;

int
standalonestat_init()
{
	pthread_rwlock_init(&__standalonestat_lock, NULL);	//통계를 사용하지 않는 경우에도 lock은 활성화 시키기 위해서 여기서 초기화를 한다.
#if 0
	gscx__standalonestat_service_list = iList.Create(sizeof (service_info_t *));
#else
	gscx__standalonestat_service_list = iList.CreateWithAllocator(sizeof (service_info_t *), &iCCLMalloc);
#endif
	if (gscx__config->stat_write_enable != 1) {

		return 0;
	}
	if (gscx__config->stat_write_enable == 1) {
		if (gscx__config->stat_rc_seq == 0 || gscx__config->stat_vrc_seq == 0 || gscx__config->stat_svr_seq == 0 ) {
			/* 통계 기록에 필요한 sequence들이 설정되지 않은 경우 통계 기록을 하지 않는다. */
			TRACE((T_ERROR, "Stat sequence not set. disable stat write.\n"));
			gscx__config->stat_write_enable = 0;
		}
	}

	if (!gscx__standalonestat_timer_wheel_base) {
		TRACE((T_INFO|T_STAT, "standalonestat timer initializing\n"));
		gscx__standalonestat_timer_wheel_base = bt_init_timers(); /* bt_timer 라이브러리에 gscx__timer_wheel_base의 메모리를 해제하는 api가 없다 ㅠ.ㅠ */
	}
	bt_init_timer(&gscx__timer_standalonestat, "origin status timer", 0);
	gscx__timer_standalonestat_inited = 1;

	pthread_create(&gscx__standalonestat_timer_tid, NULL, standalonestat_timer_thread, (void *)NULL);
	bt_set_timer(gscx__standalonestat_timer_wheel_base, (bt_timer_t *)&gscx__timer_standalonestat, 1000 , standalonestat_timer_func, NULL); /* 10 초마다 실행, 임시 */

	if (standalonestat_file_init() < 0) {
		return -1;
	}


	standalonestat_create_thread_pool();

	return 0;
}

void
standalonestat_deinit()
{
	int i, lsize = 0;
	if (gscx__timer_standalonestat_inited) {
#ifdef BT_TIMER_VER2
		while (bt_del_timer_v2(gscx__standalonestat_timer_wheel_base, &gscx__timer_standalonestat) < 0) {
			bt_msleep(10);
		}
		while (bt_destroy_timer_v2(gscx__standalonestat_timer_wheel_base, &gscx__timer_standalonestat) < 0) {
			bt_msleep(10);
		}
#else
		bt_del_timer(gscx__standalonestat_timer_wheel_base, &gscx__timer_standalonestat);
		bt_destroy_timer(gscx__standalonestat_timer_wheel_base, &gscx__timer_standalonestat);
#endif
		gscx__timer_standalonestat_inited = 0;
	}

	if (gscx__standalonestat_timer_wheel_base) {
		gscx__standalonestat_timer_working = 0;
		pthread_join(gscx__standalonestat_timer_tid, NULL);

		gscx__standalonestat_timer_wheel_base = NULL; /* gscx__standalonestat_timer_wheel_base를 초기화 하는 명령이 없다. */
		TRACE((T_INFO|T_STAT, "standalonestat timer deinitialized.\n"));
	}
	standalonestat_destroy_thread_pool();
	standalonestat_file_deinit();

	standalonestat_wlock();
	if (gscx__standalonestat_service_list) {
		iList.Clear(gscx__standalonestat_service_list);
		iList.Finalize(gscx__standalonestat_service_list);
		gscx__standalonestat_service_list = NULL;
	}
	standalonestat_unlock();
	pthread_rwlock_destroy(&__standalonestat_lock);
}


int
standalonestat_file_init()
{
	char 		path_stat_string[MAX_STAT_PATH_LENGTH] = {0};

	uint32_t		log_flag = SCX_LOGF_SIGNAL|SCX_LOGF_FILE|SCX_LOGF_STAT;

	TRACE((T_DAEMON, "%s called.\n", __func__));

	if (standalonestat_check_directory() < 0) {
		return -1;
	}

	/* 동계 기록중(doing) 파일의 생성 경로 및 file format 지정 */
	if (gscx__config->stat_write_type == STAT_TYPE_DELIVERY) {
		/* origin log는 delivery type에만 기록 한다. */
		snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s/%s%d_%%Y%%m%%d_%%h%%M",
							vs_data(gscx__config->stat_origin_path), vs_data(gscx__config->stat_doing_dir_name),  vs_data(gscx__config->stat_origin_prefix), gscx__config->stat_svr_seq);
		scx_origin_stat_logger = logger_open(path_stat_string, 0, log_flag, 100);
		/* 통계 기록후 done로 이동될 경로를 지정 */
		snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
							vs_data(gscx__config->stat_origin_path), vs_data(gscx__config->stat_done_dir_name));
		logger_set_done_path(scx_origin_stat_logger, path_stat_string);
	}
	/* 동계 기록중(doing) 파일의 생성 경로 및 file format 지정 */
	snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s/%s%d_%%Y%%m%%d_%%h%%M",
						vs_data(gscx__config->stat_traffic_path), vs_data(gscx__config->stat_doing_dir_name),  vs_data(gscx__config->stat_traffic_prefix), gscx__config->stat_svr_seq);
	scx_traffic_stat_logger = logger_open(path_stat_string, 0, log_flag, 100);
	/* 통계 기록후 done로 이동될 경로를 지정 */
	snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_traffic_path), vs_data(gscx__config->stat_done_dir_name));
	logger_set_done_path(scx_traffic_stat_logger, path_stat_string);

	/* 동계 기록중(doing) 파일의 생성 경로 및 file format 지정 */
	snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s/%s%d_%%Y%%m%%d_%%h%%M",
						vs_data(gscx__config->stat_nos_path), vs_data(gscx__config->stat_doing_dir_name),  vs_data(gscx__config->stat_nos_prefix), gscx__config->stat_svr_seq);
	scx_nos_stat_logger = logger_open(path_stat_string, 0, log_flag, 100);
	/* 통계 기록후 done로 이동될 경로를 지정 */
	snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_nos_path), vs_data(gscx__config->stat_done_dir_name));
	logger_set_done_path(scx_nos_stat_logger, path_stat_string);

	/* 동계 기록중(doing) 파일의 생성 경로 및 file format 지정 */
	snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s/%s%d_%%Y%%m%%d_%%h%%M",
						vs_data(gscx__config->stat_http_path), vs_data(gscx__config->stat_doing_dir_name),  vs_data(gscx__config->stat_http_prefix), gscx__config->stat_svr_seq);
	scx_http_stat_logger = logger_open(path_stat_string, 0, log_flag, 100);
	/* 통계 기록후 done로 이동될 경로를 지정 */
	snprintf(path_stat_string, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_http_path), vs_data(gscx__config->stat_done_dir_name));
	logger_set_done_path(scx_http_stat_logger, path_stat_string);
	return 0;
}

int
standalonestat_file_deinit()
{
	TRACE((T_DAEMON, "%s called.\n", __func__));
	if (gscx__config->stat_write_type == STAT_TYPE_DELIVERY) {
		if (scx_origin_stat_logger)
			logger_close(scx_origin_stat_logger);
	}
	if (scx_traffic_stat_logger)
		logger_close(scx_traffic_stat_logger);
	if (scx_nos_stat_logger)
		logger_close(scx_nos_stat_logger);
	if (scx_http_stat_logger)
		logger_close(scx_http_stat_logger);
	return 0;
}

/* 임시 통계 디렉토리(doing)에 잔여 파일이 있는 경우 done 디렉토리로 옮긴다. */
int
standalonestat_move_remaining_statfile(char *doing_path, char *done_path)
{
	DIR				*pDoingDir = NULL;
	struct dirent	*pEntryDir = NULL;
	struct stat		st;
	int				result = 0;
	char			*file_name = NULL;
	char 			target_path[MAX_STAT_PATH_LENGTH] = {0};
	char			origin_path[MAX_STAT_PATH_LENGTH] = {0};
	if ((pDoingDir = opendir(doing_path)) == NULL) {
		TRACE((T_WARN, "couldn't open '%s'\n", doing_path));
		return -1;
	}
	while (1) {
		if ((pEntryDir = readdir(pDoingDir)) == NULL) {
			/* 읽을 파일이나 디렉토리가 없는 경우 */
			break;
		}

		/* 디렉토리 제외 */
		result = stat(pEntryDir->d_name, &st);
		if ((result >= 0 && S_ISDIR(st.st_mode) == 1)) {
			continue;
		}

		snprintf(origin_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s", doing_path, pEntryDir->d_name);
		snprintf(target_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s", done_path, pEntryDir->d_name);

		if (rename(origin_path, target_path) < 0) {
			TRACE((T_WARN, "Stat file(%s) to (%s) move failed, reason = %d'%s'\n", origin_path, target_path, errno, strerror(errno)));
		}
		TRACE((T_DAEMON, "Stat file(%s) to (%s) move success.\n", origin_path, target_path));
	}
	closedir(pDoingDir);
	return 0;
}

/*
 * 통계가 기록될 디렉토리들을 확인해서 없는 경우 새로 생성
 * 임시 디렉토리인 doing directory에 있는 파일을 done directory로 이동 시킨다.
 */
int
standalonestat_check_directory()
{
	char 		stat_doing_path[MAX_STAT_PATH_LENGTH] = {0};
	char 		stat_done_path[MAX_STAT_PATH_LENGTH] = {0};
	snprintf(stat_doing_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_origin_path), vs_data(gscx__config->stat_doing_dir_name));
	if (scx_check_dir(stat_doing_path) < 0) {
		TRACE((T_WARN, "Origin Stat doing directory check failed(%s).\n", stat_doing_path));
		return -1;
	}
	snprintf(stat_done_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_origin_path), vs_data(gscx__config->stat_done_dir_name));
	if (scx_check_dir(stat_done_path) < 0) {
		TRACE((T_WARN, "Origin Stat done directory check failed(%s).\n", stat_done_path));
		return -1;
	}
	standalonestat_move_remaining_statfile(stat_doing_path, stat_done_path);

	snprintf(stat_doing_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_traffic_path), vs_data(gscx__config->stat_doing_dir_name));
	if (scx_check_dir(stat_doing_path) < 0) {
		TRACE((T_WARN, "Traffic Stat doing directory check failed(%s).\n", stat_doing_path));
		return -1;
	}
	snprintf(stat_done_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_traffic_path), vs_data(gscx__config->stat_done_dir_name));
	if (scx_check_dir(stat_done_path) < 0) {
		TRACE((T_WARN, "Traffic Stat done directory check failed(%s).\n", stat_done_path));
		return -1;
	}
	standalonestat_move_remaining_statfile(stat_doing_path, stat_done_path);

	snprintf(stat_doing_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_nos_path), vs_data(gscx__config->stat_doing_dir_name));
	if (scx_check_dir(stat_doing_path) < 0) {
		TRACE((T_WARN, "NOS Stat doing directory check failed(%s).\n", stat_doing_path));
		return -1;
	}
	snprintf(stat_done_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_nos_path), vs_data(gscx__config->stat_done_dir_name));
	if (scx_check_dir(stat_done_path) < 0) {
		TRACE((T_WARN, "NOS Stat done directory check failed(%s).\n", stat_done_path));
		return -1;
	}
	standalonestat_move_remaining_statfile(stat_doing_path, stat_done_path);

	snprintf(stat_doing_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_http_path), vs_data(gscx__config->stat_doing_dir_name));
	if (scx_check_dir(stat_doing_path) < 0) {
		TRACE((T_WARN, "HTTP Stat doing directory check failed(%s).\n", stat_doing_path));
		return -1;
	}
	snprintf(stat_done_path, MAX_STAT_PATH_LENGTH - 1, "%s/%s",
						vs_data(gscx__config->stat_http_path), vs_data(gscx__config->stat_done_dir_name));
	if (scx_check_dir(stat_done_path) < 0) {
		TRACE((T_WARN, "HTTP Stat done directory check failed(%s).\n", stat_done_path));
		return -1;
	}
	standalonestat_move_remaining_statfile(stat_doing_path, stat_done_path);

	return 0;

}

static int
standalonestat_create_thread_pool()
{
	int poolsize = 4; /* 통계 종류와 동일한 수의 thread를 생성한다. */
	gscx__standalonestat_thpool = thpool_init(poolsize);
	if (NULL == gscx__standalonestat_thpool) {
		TRACE((T_ERROR, "Stand Alone Stat Thread pool create failed.\n"));
		return -1;
	}

	return 0;
}


static int
standalonestat_destroy_thread_pool()
{
	if (NULL == gscx__standalonestat_thpool) {
		return 0;
	}

	thpool_destroy(gscx__standalonestat_thpool);
	gscx__standalonestat_thpool = NULL;

	return 0;
}

/*
 * loop을 돌면서 지정된 시간(100msec)마다 timer에 등록된 job들을 실행 시킨다.
 */
void *
standalonestat_timer_thread(void *d)
{
	struct timeval current;
	time_t nexttimer = 0;
	time_t curtime;
	int  delaytime = 0;
	prctl(PR_SET_NAME, "standalonestat timer thread");
	TRACE((T_INFO|T_STAT, "standalonestat timer scheduler thread started.\n"));
	while (gscx__standalonestat_timer_working) {
		/* timer가 100 milisecond 마다 정확하게 호출 되게 하기 위해 sleep time을 계산한다. */
		gettimeofday(&current, NULL);
		curtime = (uint64_t)current.tv_sec * 1000000 + (uint64_t)current.tv_usec ;
		if(nexttimer == 0) {
			nexttimer = curtime;
		}
		nexttimer = nexttimer + STAND_ALONE_TIMER_RESOLUTION * 1000;
		if(nexttimer > curtime) {
			delaytime = (int) (nexttimer - curtime) / 1000 ;
		}
		else {	/* 서버 시간이 변경 된 경우 여기로 들어 올수 있음.*/
			delaytime = STAND_ALONE_TIMER_RESOLUTION;
		}
//		printf ("stat called. %10u, %10u, delay = %d\n", current.tv_sec, current.tv_usec , delaytime);
		bt_msleep(delaytime);	/* 100 milisec 마다 호출 */

		bt_run_timers(gscx__standalonestat_timer_wheel_base);
	}
	TRACE((T_INFO|T_STAT, "standalonestat timer scheduler thread stoped.\n"));
}



void
standalonestat_timer_func(void *cr)
{
	struct timeval current;
	time_t curtime;
	int  delaytime = 0;
	static time_t prevtime = 0;
	static time_t nexttimer = 0;
	static int rotate_cnt = 0;
	static int write_cnt = 0;

	gettimeofday(&current, NULL);

	////////////////////////////// timer 관련 부분 시작 //////////////////////////////////////////
	curtime = (uint64_t)current.tv_sec * 1000000 + (uint64_t)current.tv_usec ;
	if(nexttimer == 0) {
		/* 처음 실행에만 이부분이 실행 된다. */
		nexttimer = (uint64_t)current.tv_sec * 1000000 + (uint64_t)100000 ;	/* *.1 초에 호출 되도록 하기 위해서 이렇게 계산한다. */
		prevtime = curtime - 1000000;
	}

	nexttimer = nexttimer + 1000000;
	if(nexttimer > curtime) {
		delaytime = (int) (nexttimer - curtime) / 1000 ;
	}
	else {	/* 시간 계산이 잘못된 경우 계산 없이 1초 후에 호출 되도록 한다. */
		delaytime = 1000;
		nexttimer = (uint64_t)current.tv_sec * 1000000 + (uint64_t)100000 ;	/* *.1 초에 호출 되도록 하기 위해서 이렇게 계산한다. */
	}
	TRACE((T_DEBUG, "%s called. %10u, %10u, %d\n", __func__, current.tv_sec, current.tv_usec, delaytime ));
//	printf("delay = %.3f sec\n", (curtime - prevtime) / 1000000.0);
//	printf ("stat called. %10u, %10u, %d\n", current.tv_sec, current.tv_usec, delaytime);
	if ((prevtime + 1200000) < curtime) {
		TRACE((T_INFO, "%s time diff to large(%.3f sec)\n", __func__, (curtime - (prevtime + 1000000)) / 1000000.0));
//		printf("%s time diff to large(%.3f sec)\n", __func__, (curtime - (prevtime + 1000000)) / 1000000.0);

	}

	prevtime = curtime;

	bt_set_timer(gscx__standalonestat_timer_wheel_base, (bt_timer_t *)&gscx__timer_standalonestat, delaytime , standalonestat_timer_func, NULL); /* 다음에 실행할 timer를 등록 */
	////////////////////////////// timer 관련 부분 끝 //////////////////////////////////////////
	if (!gscx__service_available) return ;	// 서비스가 준비 되지 않은 경우에는 그냥 리턴한다.
	/* STAT_TYPE_NOS_PEEKCON는 1초 마다 호출 되도록 한다. */
	thpool_add_work(gscx__standalonestat_thpool, (void *)standalonestat_write_job, (void *)STAT_TYPE_NOS_PEEKCON);

	/*
	 * 10초마다 통계를 파일로 기록한다.
	 * timer가 정확하게 10초에 호출되지 않는 경우를 대비해서 write_cnt가 10초를 넘으면 강제로 기록하도록 한다.
	 */
	if ((current.tv_sec % gscx__config->stat_write_period) == 0 || write_cnt >= gscx__config->stat_write_period) {
		thpool_add_work(gscx__standalonestat_thpool, (void *)standalonestat_write_job, (void *)STAT_TYPE_WRITE);
		write_cnt = 0;
	}

	/*
	 * write와 rotate가 동시에 일어나는 것이 문제가 있을 수도 있음 이런 경우 매분 1초에 rotate를 할수도 있다.
	 * timer 문제 때문에 정확한 시간에 호출 안되는 경우를 대비 해서 rotate_cnt가 60을 넘는 경우에 강제로 rotate하도록 한다.
	 */
	if ((current.tv_sec % gscx__config->stat_rotate_period) == 1 || rotate_cnt >= gscx__config->stat_rotate_period) {
		thpool_add_work(gscx__standalonestat_thpool, (void *)standalonestat_write_job, (void *)STAT_TYPE_ROTATE);
		rotate_cnt = 0;
	}

	rotate_cnt++;
	write_cnt++;
}



/*
 * worker thread에서 호출 되는 function
 */
void
standalonestat_write_job(void * ptype)
{
	standalonestat_type_t  stat_type = (standalonestat_type_t )ptype;
//	printf("stat type = %d\n", stat_type);
	TRACE((T_DEBUG, "%s stat type = %d.\n", __func__, stat_type));
	switch (stat_type) {
	case STAT_TYPE_WRITE:
		if (gscx__config->stat_write_type == STAT_TYPE_DELIVERY) {
			standalonestat_write_origin();
			logger_flush(scx_origin_stat_logger);
		}
		standalonestat_write_traffic();
		logger_flush(scx_traffic_stat_logger);
		standalonestat_write_nos();
		logger_flush(scx_nos_stat_logger);

		/* http의 통계를 client 종료시 마다 buffer에 쓰고 있기 때문에 여기서는 buffer에 있는 내용을 강제로 file에 쓰는 flush만 호출한다. */
		logger_flush(scx_http_stat_logger);
		break;
	case STAT_TYPE_NOS_PEEKCON:
		/* 1초단위로 순간 동접을 측정*/
		standalonestat_nos_sec_peekcon();
		break;
	case STAT_TYPE_ROTATE:
		/* doing directory의 통계 파일들을 모두 rotate 한후  done directory로 옮긴다. */
		if (gscx__config->stat_write_type == STAT_TYPE_DELIVERY) {
			logger_reopen(scx_origin_stat_logger);
		}
		logger_reopen(scx_traffic_stat_logger);
		logger_reopen(scx_nos_stat_logger);
		logger_reopen(scx_http_stat_logger);
		break;
	default:
		break;
	}
	return;
}



/*
 * origin 통계를 파일에 기록
 * 통계 포맷 : STATTIME RC_SEQ VRC_SEQ SVR_SEQ SP_SEQ CONT_SEQ VOL_SEQ PROTOCOL INBOUND OUTBOUND
 * 기록 예
20190410171630 59 125 725 11 75 449 1 387340 8754
20190410171630 59 125 725 11 75 451 1 3487506 2891
20190410171640 59 125 725 11 75 449 1 94372 9770
 */
void
standalonestat_write_origin()
{
	service_info_t *service = NULL;
	char 			log_line[4096] = "";
	int 			n;
	char 		timestring[128];
	time_t 		cur_time;
	struct tm 	result;
	struct tm	*c_tm = NULL;
	int 		i, lsize = 0;

	uint64_t	diff_origin_request = 0;
	uint64_t	diff_origin_response = 0;
	uint64_t	cur_origin_request = 0;
	uint64_t	cur_origin_response = 0;
	time_t		prev_time = 0;
	int			diff_time = 0;

	cur_time 	= scx_get_cached_time_sec();
	c_tm	= localtime_r(&cur_time, &result);
	strftime(timestring, sizeof(timestring), STAT_DATE_FORMAT, c_tm);

	standalonestat_rlock();
	lsize = iList.Size(gscx__standalonestat_service_list);
	for (i = 0; i < lsize; i++) {
		service = *(service_info_t **)iList.GetElement(gscx__standalonestat_service_list,i);

		cur_origin_request = ATOMIC_VAL(service->core->counter_outbound_request);
		diff_origin_request = cur_origin_request - service->core->prev_outbound_request;
		service->core->prev_outbound_request = cur_origin_request;

		cur_origin_response = ATOMIC_VAL(service->core->counter_outbound_response);
		diff_origin_response = cur_origin_response - service->core->prev_outbound_response;
		service->core->prev_outbound_response = cur_origin_response;

		prev_time = service->core->prev_inbound_time;
		service->core->prev_outbound_time = cur_time;
#if 0
		if (prev_time >= cur_time) {
			/* 이전 확인 시간이 현재 시간보다 크거나 같은 경우는 서버 시간이 변경된 경우라고 볼수 있기 때문에 이 경우는 통계기록을 하지 않는다. */
			TRACE((T_INFO, "The service(%s) origin check time not valid, previce(%d), current(%d).\n", vs_data(service->name) , prev_time, cur_time));
			continue;
		}

		/* origin 통계의 경우 전송량만을 기록하는 통계라서 시간 차이가 있어도 그냥 기록 한다. */
		diff_time = cur_time - prev_time;
		if (diff_time > (gscx__config->stat_write_period * 2)) {
			/* 마지막 통계 기록 후 기록 주기의 두배(20초) 이상이 지난 경우는 비정상 시간이라고 판단해서 통계에 기록 하지 않는다. */
			TRACE((T_INFO, "The service(%s) origin diff time not valid, previce(%d), current(%d).\n", vs_data(service->name) , prev_time, cur_time));
		}
#endif
//		printf ("service = %s, %d, %d\n", vs_data(service->name), service->stat_write_enable, service->stat_origin_enable);
		if (service->stat_write_enable == 1 && service->stat_origin_enable == 1) {
			/* 오리진 통계가 켜진 경우만 파일에 기록한다. */
			if (diff_origin_response > 0 || diff_origin_request > 0) {
				/* 트래픽이 있는 경우만 통계에 기록한다. */
				n = scx_snprintf(log_line, sizeof(log_line),
				"%s %d %d %d %d %d %d 1 %llu %llu\n",
									timestring, gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
									service->stat_sp_seq, service->stat_cont_seq, service->stat_vol_seq,
									diff_origin_response, diff_origin_request
									);

				logger_put(scx_origin_stat_logger, log_line, n);
			}
		}
	}
	standalonestat_unlock();

	return;
}

/*
 * traffic 통계를 파일에 기록
 * delivery type 통계 포맷 : STATTIME RC_SEQ VRC_SEQ SVR_SEQ IDC_NODE_ID RCID VRCID SVRID LASTSENTAT SP_SEQ CONT_SEQ VOL_SEQ PROTOCOL DIRECTION INBOUND OUTBOUND
 * 기록 예
20190410172830 59 125 725 SK04 sk-81 vrc1200.sk-81 fhs5144.sk-81 1554884910 11 75 449 1 1 557622 116360526
20190410172830 59 125 725 SK04 sk-81 vrc1200.sk-81 fhs5144.sk-81 1554884910 11 75 451 1 1 210726 66162806
20190410172840 59 125 725 SK04 sk-81 vrc1200.sk-81 fhs5144.sk-81 1554884920 11 75 449 1 1 301859 144906259
 * streaming type 통계 포맷 : STATTIME IDC_NODE_ID RC_SEQ VRC_SEQ SVR_SEQ CONT_SEQ SP_SEQ VOL_SEQ PROTOCOL INBOUND OUTBOUND
 * 기록 예
20201216102600 GN 3 10013 21727 10005 10001 20149 4 0 109883556
20201216102600 GN 3 10013 21727 10005 10001 20151 4 5 8855454
20201216102600 GN 3 10013 21727 10005 10001 20153 4 15 3226699
 */
void
standalonestat_write_traffic()
{
	service_info_t *service = NULL;
	char 			log_line[4096] = "";
	int 			n;
	char 		timestring[128];
	time_t 		cur_time;
	struct tm 	result;
	struct tm	*c_tm = NULL;
	int 		i, lsize = 0;

	uint64_t	diff_client_request = 0;
	uint64_t	diff_client_response = 0;
	uint64_t	cur_client_request = 0;
	uint64_t	cur_client_response = 0;
	time_t		prev_time = 0;
	int			diff_time = 0;

	cur_time 	= scx_get_cached_time_sec();
	c_tm	= localtime_r(&cur_time, &result);
	strftime(timestring, sizeof(timestring), STAT_DATE_FORMAT, c_tm);

	standalonestat_rlock();
	lsize = iList.Size(gscx__standalonestat_service_list);
	for (i = 0; i < lsize; i++) {
		service = *(service_info_t **)iList.GetElement(gscx__standalonestat_service_list,i);

		cur_client_request = ATOMIC_VAL(service->core->counter_inbound_request);
		diff_client_request = cur_client_request - service->core->prev_inbound_request;
		service->core->prev_inbound_request = cur_client_request;

		cur_client_response = ATOMIC_VAL(service->core->counter_inbound_response);
		diff_client_response = cur_client_response - service->core->prev_inbound_response;
		service->core->prev_inbound_response = cur_client_response;

		prev_time = service->core->prev_inbound_time;
		service->core->prev_inbound_time = cur_time;
#if 0
		if (prev_time >= cur_time) {
			/* 이전 확인 시간이 현재 시간보다 크거나 같은 경우는 서버 시간이 변경된 경우라고 볼수 있기 때문에 이 경우는 통계기록을 하지 않는다. */
			TRACE((T_INFO, "The service(%s) client check time not valid, previce(%d), current(%d).\n", vs_data(service->name) , prev_time, cur_time));
			continue;
		}

		/* traffic 통계의 경우 전송량만을 기록하는 통계라서 시간 차이가 있어도 그냥 기록 한다. */
		diff_time = cur_time - prev_time;
		if (diff_time > (gscx__config->stat_write_period * 2)) {
			/* 마지막 통계 기록 후 기록 주기의 두배(20초) 이상이 지난 경우는 비정상 시간이라고 판단해서 통계에 기록 하지 않는다. */
			TRACE((T_INFO, "The service(%s) client diff time not valid, previce(%d), current(%d).\n", vs_data(service->name) , prev_time, cur_time));
		}
#endif

		if (service->stat_write_enable == 1 && service->stat_traffic_enable == 1) {
			/* traffic 통계가 켜진 경우만 파일에 기록한다. */
			if (diff_client_request > 0 || diff_client_response > 0) {
				/* 트래픽이 있는 경우만 통계에 기록한다. */
				if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
					// Streaming type : STATTIME IDC_NODE_ID RC_SEQ VRC_SEQ SVR_SEQ CONT_SEQ SP_SEQ VOL_SEQ PROTOCOL INBOUND OUTBOUND
					n = scx_snprintf(log_line, sizeof(log_line),
						"%s %s %d %d %d %d %d %d 1 %llu %llu\n",
											timestring, vs_data(gscx__config->stat_idc_node_id),
											gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
											service->stat_cont_seq, service->stat_sp_seq, service->stat_vol_seq,
											diff_client_request, diff_client_response
											);
				}
				else {
					// Delivery type : STATTIME RC_SEQ VRC_SEQ SVR_SEQ IDC_NODE_ID RCID VRCID SVRID LASTSENTAT SP_SEQ CONT_SEQ VOL_SEQ PROTOCOL DIRECTION INBOUND OUTBOUND
					n = scx_snprintf(log_line, sizeof(log_line),
						"%s %d %d %d %s %s %s %s %u %d %d %d 1 1 %llu %llu\n",
											timestring, gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
											vs_data(gscx__config->stat_idc_node_id),vs_data(gscx__config->stat_rc_id), vs_data(gscx__config->stat_vrc_id), vs_data(gscx__config->stat_svr_id),
											cur_time, service->stat_sp_seq, service->stat_cont_seq, service->stat_vol_seq,
											diff_client_request, diff_client_response
											);
				}

				logger_put(scx_traffic_stat_logger, log_line, n);
			}
		}
	}
	standalonestat_unlock();

	return;
}

/*
 * nos(동접) 통계를 파일에 기록
 * delivery typ 통계 포맷 : STATTIME RC_SEQ VRC_SEQ SVR_SEQ IDC_NODE_ID RCID VRCID SVRID TIMESTAMP SP_SEQ CONT_SEQ VOL_SEQ PUSHEDAT UPDATEAT LASTSENTAT PEEKCON CURCON
 * 기록 예
20190410171220 59 125 725 SK04 sk-81 vrc1200.sk-81 fhs5144.sk-81 1554883940 11 75 449 1554883940 1554883940 1554883940 14 863
20190410171220 59 125 725 SK04 sk-81 vrc1200.sk-81 fhs5144.sk-81 1554883940 11 75 451 1554883940 1554883940 1554883940 3 1266
20190410171230 59 125 725 SK04 sk-81 vrc1200.sk-81 fhs5144.sk-81 1554883950 11 75 449 1554883950 1554883950 1554883950 7 552
 * streaming type 통계 포맷 : STATTIME IDC_NODE_ID RC_SEQ VRC_SEQ SVR_SEQ CONT_SEQ SP_SEQ VOL_SEQ PROTOCOL PEEKCON CURCON
 * 기록 예
20201216102700 GN 3 10013 21727 10005 10001 20149 4 123 0
20201216102700 GN 3 10013 21727 10005 10001 20151 4 5 0
20201216102700 GN 3 10013 21727 10005 10001 20153 4 3 0
 */
void
standalonestat_write_nos()
{
	service_info_t *service = NULL;
	char 			log_line[4096] = "";
	int 			n;
	char 		timestring[128];
	time_t 		cur_time;
	struct tm 	result;
	struct tm	*c_tm = NULL;
	int 		i, lsize = 0;




	uint32_t	peek_concurrent_count = 0;
	uint64_t	prev_inbound_counter = 0;
	uint64_t	cur_inbound_counter = 0;
	uint64_t	diff_inbound_counter = 0;


	cur_time = scx_get_cached_time_sec();
	c_tm = localtime_r(&cur_time, &result);
	strftime(timestring, sizeof(timestring), STAT_DATE_FORMAT, c_tm);

	standalonestat_rlock();
	lsize = iList.Size(gscx__standalonestat_service_list);
	for (i = 0; i < lsize; i++) {
		service = *(service_info_t **)iList.GetElement(gscx__standalonestat_service_list,i);
		if (service->stat_session_type_enable == 1) {
			peek_concurrent_count = service->core->peek_session_count;
			service->core->peek_session_count = 0;
			prev_inbound_counter = service->core->prev_session_total;
			cur_inbound_counter = ATOMIC_VAL(service->core->counter_session_total);
			service->core->prev_session_total = cur_inbound_counter;
		}
		else {
			peek_concurrent_count = service->core->peek_concurrent_count;
			service->core->peek_concurrent_count = 0;
			prev_inbound_counter = service->core->prev_inbound_counter;
			cur_inbound_counter = ATOMIC_VAL(service->core->counter_inbound_counter);
			service->core->prev_inbound_counter = cur_inbound_counter;
		}
		diff_inbound_counter = cur_inbound_counter - prev_inbound_counter;

		if (service->stat_write_enable == 1 && service->stat_nos_enable == 1) {
			/*
			 *  nos 통계가 켜진 경우만 파일에 기록한다.
			 *  traffic 통계나 오리진 통계와 다르게 동접이 0인 경우라도 통계에 기록 해야 한다.
			 */
			if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
				// Streaming type : STATTIME IDC_NODE_ID RC_SEQ VRC_SEQ SVR_SEQ CONT_SEQ SP_SEQ VOL_SEQ PROTOCOL PEEKCON CURCON
				n = scx_snprintf(log_line, sizeof(log_line),
						"%s %s %d %d %d %d %d %d 1 %d %d\n",
								timestring, vs_data(gscx__config->stat_idc_node_id),
								gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
								service->stat_cont_seq, service->stat_sp_seq,  service->stat_vol_seq,
								peek_concurrent_count, (int)diff_inbound_counter
								);
			}
			else {
				// Delivery type : STATTIME RC_SEQ VRC_SEQ SVR_SEQ IDC_NODE_ID RCID VRCID SVRID TIMESTAMP SP_SEQ CONT_SEQ VOL_SEQ PUSHEDAT UPDATEAT LASTSENTAT PEEKCON CURCON
				n = scx_snprintf(log_line, sizeof(log_line),
						"%s %d %d %d %s %s %s %s %u %d %d %d %u %u %u %d %d\n",
								timestring, gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
								vs_data(gscx__config->stat_idc_node_id),vs_data(gscx__config->stat_rc_id), vs_data(gscx__config->stat_vrc_id), vs_data(gscx__config->stat_svr_id),
								cur_time, service->stat_sp_seq, service->stat_cont_seq, service->stat_vol_seq,
								cur_time, cur_time, cur_time,
								peek_concurrent_count, (int)diff_inbound_counter
								);
			}

//			printf("%s", log_line);
			logger_put(scx_nos_stat_logger, log_line, n);
		}

	}
	standalonestat_unlock();
	return;
}

/*
 * 1초 단위로 서비스(볼륨)별 순간 동접을 기록한다.
 */
void
standalonestat_nos_sec_peekcon()
{
	service_info_t *service = NULL;
	int 		i, lsize = 0;
	uint32_t 	concurrent_count;
	uint32_t 	session_count;

	standalonestat_rlock();
	lsize = iList.Size(gscx__standalonestat_service_list);
	for (i = 0; i < lsize; i++) {
		service = *(service_info_t **)iList.GetElement(gscx__standalonestat_service_list,i);

		concurrent_count = scx_get_volume_concurrent_count(service);
		/* 현재의 동접(concurrent_count)이 1초 전(peek_concurrent_count)에 측정 했을때의 동접 보다 큰 경우 해당 값을 peek_concurrent_count에 없데이트 한다. */
		if (service->core->peek_concurrent_count < concurrent_count) {
			service->core->peek_concurrent_count = concurrent_count;
		}
		if (service->session_enable == 1) {
			session_count = scx_get_volume_session_count(service);
			if (service->core->peek_session_count < session_count) {
				service->core->peek_session_count = session_count;
			}
		}

	}
	standalonestat_unlock();
	return;
}



/*
 * content(http) 통계 기록 함수
 * 다른 통계와 다르게 content 통계는 사용자 연결 종료시에 nc_request_completed() 부터 직접 호출 된다.
 * delivery typ 통계 포맷 : STATTIME RC_SEQ VRC_SEQ SVR_SEQ IDC_NODE_ID RCID VRCID SVRID TIMESTAMP PROTOCOL SP_SEQ CONT_SEQ VOL_SEQ BEGINAT ENDAT DURATION CLIENT_IP SENDOFFSET OUTSIZE STATSIZE RESCODE TXMODE USERID FILE_PATH
 * 기록 예
20190410175500 23 159 925 SK04 sk-51 vrc1103.sk-51 fhs2155.sk-51 1554886500 1 5 21 163 0 0 61 222.233.27.17 0 3652339 11388416 206 1 cdn.podbbang.com /stg/21/163/data1/tbsadm/nf20190410002.mp3
20190410175500 23 159 925 SK04 sk-51 vrc1103.sk-51 fhs2155.sk-51 1554886500 1 5 21 163 0 0 2 223.39.151.3 0 3958370 3957888 206 1 cdn.podbbang.com /stg/21/163/data1/tbsadm/nf20190410001.mp3
 * streaming type 통계 포맷 : STATTIME IDC_NODE_ID RC_SEQ VRC_SEQ SVR_SEQ CONT_SEQ SP_SEQ VOL_SEQ PROTOCOL TOTALTIME PLAYOFFSET PLAYTIME TOTALSIZE SENDOFFSET OUTSIZE DURATION CLIENT_IP RES_CODE AUTH_CODE DRM_CODE TX_MODE USERID DOMAIN FILE_PATH
 * 기록 예
20201216102813 GN 3 10013 21727 10005 10001 20149 4 3898 0 664 539140730 0 217294992 664 125.61.45.27 200 200 0 1 - nsbsmedia-sbsinmstream0.lgucdn.com sbsmediamp4/202012/15/856X480_128_452676762_2020-12-15-184444904.mp4
20201216102814 GN 3 10013 21727 10005 10001 20149 4 5404 0 65 547849347 0 87657417 65 58.122.148.183 200 200 0 1 - nsbsmedia-sbsinmstream0.lgucdn.com sbsmediamp4/et/1095/et1095f0017500.mp4
20201216102815 GN 3 10013 21727 10005 10001 20165 4 27 0 10 7074653 0 7364078 10 1.252.160.209 200 200 0 1 - netv-sbsinmstream0.lgucdn.com netvmp4/upload/Media/sbs/202012/15/1920X1080_256_453172353_2020-12-15-232158777_t34.mp4
 */
void
standalonestat_write_http(nc_request_t *req)
{
	char 			log_line[4096] = "";
	int 			n;
	service_info_t 		*service = req->service;
	char 		timestring[128];
	time_t 		utime;
	struct tm 	result;
	struct tm	*c_tm = NULL;
	int 		s = 0;
	int			remove_virtual_path = 0;
	int			remain = 0;
	int			media_duration = 0;
	uint64_t	total_size = 0LL;

	if ( gscx__config->stat_write_enable != 1 ||
			service->stat_http_enable != 1 ||
			service->stat_write_enable != 1 ) {
		/* content 통계는 위 조건중 한가지만 맞지 않아도 통계를 기록 하지 않는다. */
		return;
	}
	if (scx_http_stat_logger == NULL) {
		return;
	}
	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
		__thread_jmp_working = 1;
		utime 	= scx_get_cached_time_sec();
		c_tm	= localtime_r(&utime, &result);
		strftime(timestring, sizeof(timestring), STAT_DATE_FORMAT, c_tm);
		if (req->streaming != NULL) {
			if (400 <= req->p1_error)  {

			}
			if (req->streaming->session != NULL && service->stat_session_type_enable == 1 /* && req->p1_error < 400 */) {
				/*
				 * 아래의 조건을 모두 만족할때에는 session 기준 content 통계가 기록 되기 때문에 여기서를 그냥 리턴한다.
				 * session 기능을 사용중이고  세션이 생성되어 있는 경우
				 * session 기준 통계를 기록하는 경우
				 * 응답코드가 400보다 작은 경우(세션이 생성된 상태라도 비정상 요청일때에는 바로 로그에 기록한다.)
				 */
				return;
			}
		}

		/* 요청 단위 contents 통계 */
		/* 마지막 FILE_PATH는 따로 기록되기 때문에 USERID까지만 출력 한다. */
		if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {

			// Streaming type : STATTIME IDC_NODE_ID RC_SEQ VRC_SEQ SVR_SEQ CONT_SEQ SP_SEQ VOL_SEQ PROTOCOL TOTALTIME PLAYOFFSET PLAYTIME TOTALSIZE SENDOFFSET OUTSIZE DURATION CLIENT_IP RES_CODE AUTH_CODE DRM_CODE TX_MODE USERID DOMAIN FILE_PATH
			if (req->service->streaming_method == 1 || req->service->hls_modify_manifest_enable == 1) {
				/* live 인 경우 TOTALTIME, PLAYOFFSET, PLAYTIME, TOTALSIZE, SENDOFFSET을 모두 0으로 기록한다. */
				n = scx_snprintf(log_line, sizeof(log_line),
								"%s %s %d %d %d %d %d %d 1 0 0 0 0 0 %llu %.2f %s %d 0 0 1 - %s ",
								timestring,	vs_data(gscx__config->stat_idc_node_id),
								gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
								service->stat_cont_seq, service->stat_sp_seq,  service->stat_vol_seq,
								req->res_body_size + req->res_hdr_size, (float)(req->t_res_compl - req->t_req_fba)/1000000.0,
								(req->client_ip?(char *)vs_data(req->client_ip):"-"), req->resultcode, vs_data(service->name));
			}
			else {
				if (req->streaming) {
					if ( req->streaming->builder != NULL) {
						media_duration = (int) req->streaming->builder->duration;
						/* 전송 해야할 파일 크기는 첫번째 파일의 크기로 한다. */
						total_size =  req->streaming->builder->media_list->media->msize;
					}
				}
				// STATTIME      	IDC_NODE_ID RC_SEQ 	VRC_SEQ SVR_SEQ CONT_SEQ 	SP_SEQ 	VOL_SEQ PROTOCOL 	TOTALTIME 	PLAYOFFSET 	PLAYTIME 	TOTALSIZE 	SENDOFFSET 	OUTSIZE 	DURATION 	CLIENT_IP 		RES_CODE 	AUTH_CODE 	DRM_CODE 	TX_MODE USERID 	DOMAIN 						FILE_PATH
				// 20201222115608 HY02 4 15 334 12308 84133 74761 									1 			596 		0 			7.74 		306246558 	0 			6641201 	7.74 		192.168.0.41 	200 		0 			0 			1 		- 		wowza-solsvc.solboxtb2.com 	mp4/Big_Buck_Bunny_HD.mp4
				// 20201222133127 HY02 4 15 334 12308 84133 74761                                   1 			596 		0 			27 			306246558 	0 			19766962 	27 			192.168.0.41 	200 		0 			0 			1 		- 		wowza-solsvc.solboxtb2.com 	mp4/Big_Buck_Bunny_HD.mp4

				n = scx_snprintf(log_line, sizeof(log_line),
								"%s %s %d %d %d %d %d %d 1 %d 0 %.2f %llu 0 %llu %.2f %s %d 0 0 1 - %s ",
								timestring,	vs_data(gscx__config->stat_idc_node_id),
								gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
								service->stat_cont_seq, service->stat_sp_seq,  service->stat_vol_seq,
								media_duration, (float)(req->t_res_compl - req->t_req_fba)/1000000.0, total_size,
								req->res_body_size + req->res_hdr_size, (float)(req->t_res_compl - req->t_req_fba)/1000000.0,
								(req->client_ip?(char *)vs_data(req->client_ip):"-"), req->resultcode, vs_data(service->name));
			}


		}
		else {
			// Delivery type : STATTIME RC_SEQ VRC_SEQ SVR_SEQ IDC_NODE_ID RCID VRCID SVRID TIMESTAMP PROTOCOL SP_SEQ CONT_SEQ VOL_SEQ BEGINAT ENDAT DURATION CLIENT_IP SENDOFFSET OUTSIZE STATSIZE RESCODE TXMODE USERID FILE_PATH
			n = scx_snprintf(log_line, sizeof(log_line),
							"%s %d %d %d %s %s %s %s %u 1 %d %d %d 0 0 %.2f %s 0 %llu %llu %d 1 %s ",
							timestring, gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
							vs_data(gscx__config->stat_idc_node_id),vs_data(gscx__config->stat_rc_id), vs_data(gscx__config->stat_vrc_id), vs_data(gscx__config->stat_svr_id),
							utime, service->stat_sp_seq, service->stat_cont_seq, service->stat_vol_seq,
							(float)(req->t_res_compl - req->t_req_fba)/1000000.0, (req->client_ip?(char *)vs_data(req->client_ip):"-"),
							req->res_body_size + req->res_hdr_size, (req->objstat.st_sizedecled==1?req->objstat.st_size:0),	/* 동적 컨텐츠(st_sizedecled가 0)인 경우 STATSIZE를 0으로 처리 */
							req->resultcode, vs_data(service->name)	);
		}
	//	printf("line = %d, data = '%s'\n", n, log_line);
		if(req->streaming != NULL) {
			if (req->streaming->real_path_len > 0) {
				/* 가상 파일 경로가 들어 있는 조건인지 확인 */
				remove_virtual_path = 1;
			}
		}

		if (service->stat_http_ignore_path == 1) {
			/* content path 대신 '-'로 기록 하는 경우 */
			n += scx_snprintf(log_line + n, sizeof(log_line) - n, "-\n");
		}
		else if (remove_virtual_path == 1) {
			/* 스트리밍 기능사용시(zipper로 동작이나 solproxy에서 streaming_enable가 1일 경우)에 HTTP(Content) 통계에 남는 FilePath  부분에 Client의 요청 경로에서 가상 파일 경로를 제외하고 남기도록 한다 */
			if ((sizeof(log_line) - n - 2) > req->streaming->real_path_len) {
				remain = req->streaming->real_path_len;
			}
			else {
				/* -2는 \n을 나중에 출력하기 때문에 여기서 빼고 계산한다. */
				remain = sizeof(log_line) - n - 2;
			}
			/* content 경로는 req->path가 아닌 req->ori_url를 사용한다. */
			if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
				/* streaming type의 경우 path 기록시 처음의 /는 빼고 기록 한다. */
				n += scx_snprintf(log_line + n, remain, "%s", vs_data(req->ori_url) + 1 );
			}
			else {
				n += scx_snprintf(log_line + n, remain, "%s", vs_data(req->ori_url));
			}
			n += scx_snprintf(log_line + n, sizeof(log_line) - n, "\n");
		}
		else {
			/* client로 부터 요청된 경로를 그대로 기록 */
			if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
				/* streaming type의 경우 처음의 /는 빼고 기록 한다. */
				n += scx_snprintf(log_line + n, sizeof(log_line) - n, "%s\n", vs_data(req->path) + 1);
			}
			else {
				n += scx_snprintf(log_line + n, sizeof(log_line) - n, "%s\n", vs_data(req->path));
			}
		}
	//	printf("last line = %d, data = '%s'\n", n, log_line);

		logger_put(scx_http_stat_logger, log_line, n);
		__thread_jmp_working = 0;
	}
	else {
		/* SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
		TRACE((T_ERROR, "%s() SIGSEGV occured. %s:%d\n", __func__,  __FILE__, __LINE__));
	}
	return;
}

/*
 * http 통계에 session 단위로 통계를 기록
 */
void
standalonestat_write_http_session(struct session_info_tag *session)
{
	char 			log_line[4096] = "";
	int 			n;
	service_info_t 	*service = session->service;
	char 		timestring[128];
	time_t 		utime;
	struct tm 	result;
	struct tm	*c_tm = NULL;
	int 		s = 0;
	int			remove_virtual_path = 0;
	int			remain = 0;
	int			media_duration = 0;
	uint64_t	total_size = 0LL;

	if ( gscx__config->stat_write_enable != 1 ||
			service->stat_http_enable != 1 ||
			service->stat_write_enable != 1 ) {
		/* content 통계는 위 조건중 한가지만 맞지 않아도 통계를 기록 하지 않는다. */
		return;
	}
	if (scx_http_stat_logger == NULL) {
		return;
	}

	utime 	= scx_get_cached_time_sec();
	c_tm	= localtime_r(&utime, &result);
	strftime(timestring, sizeof(timestring), STAT_DATE_FORMAT, c_tm);

	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
		__thread_jmp_working = 1;
		/* session 기반 contents 통계 */
		if (gscx__config->stat_write_type == STAT_TYPE_STREAMING) {
			// Streaming type : STATTIME IDC_NODE_ID RC_SEQ VRC_SEQ SVR_SEQ CONT_SEQ SP_SEQ VOL_SEQ PROTOCOL TOTALTIME PLAYOFFSET PLAYTIME TOTALSIZE SENDOFFSET OUTSIZE DURATION CLIENT_IP RES_CODE AUTH_CODE DRM_CODE TX_MODE USERID DOMAIN FILE_PATH

			if (session->is_live == 1) {
				/* live 인 경우 TOTALTIME, PLAYOFFSET, PLAYTIME, TOTALSIZE, SENDOFFSET을 모두 0으로 기록한다. */
				n = scx_snprintf(log_line, sizeof(log_line),
								"%s %s %d %d %d %d %d %d 1 0 0 0 0 0 %llu %d %s %d 0 0 1 - %s %s\n",
								timestring,	vs_data(gscx__config->stat_idc_node_id),
								gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
								service->stat_cont_seq, service->stat_sp_seq,  service->stat_vol_seq,
								session->res_body_size + session->res_hdr_size, session->end_time - session->start_time,
								(session->client_ip?session->client_ip:"-"), 200, /* session이 있는 경우 응답코드는 무조건 200이다 */
								vs_data(service->name), session->path+1); /* streaming type의 경우 path 기록시 처음의 /는 빼고 기록 한다. */
			}
			else {
				/* vod인 경우 */
				media_duration = (int)session->duration;
				total_size = session->file_size;
				n = scx_snprintf(log_line, sizeof(log_line),
								"%s %s %d %d %d %d %d %d 1 %d 0 %d %llu 0 %llu %d %s %d 0 0 1 - %s %s\n",
								timestring,	vs_data(gscx__config->stat_idc_node_id),
								gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
								service->stat_cont_seq, service->stat_sp_seq,  service->stat_vol_seq,
								media_duration, session->end_time - session->start_time, total_size,
								session->res_body_size + session->res_hdr_size, session->end_time - session->start_time,
								(session->client_ip?session->client_ip:"-"), 200, /* session이 있는 경우 응답코드는 무조건 200이다 */
								vs_data(service->name), session->path+1); /* streaming type의 경우 path 기록시 처음의 /는 빼고 기록 한다. */
			}

		}
		else {
			// Delivery type : STATTIME RC_SEQ VRC_SEQ SVR_SEQ IDC_NODE_ID RCID VRCID SVRID TIMESTAMP PROTOCOL SP_SEQ CONT_SEQ VOL_SEQ BEGINAT ENDAT DURATION CLIENT_IP SENDOFFSET OUTSIZE STATSIZE RESCODE TXMODE USERID FILE_PATH
			n = scx_snprintf(log_line, sizeof(log_line),
								"%s %d %d %d %s %s %s %s %u 1 %d %d %d 0 0 %d %s 0 %llu %llu %d 1 %s %s\n",
								timestring, gscx__config->stat_rc_seq, gscx__config->stat_vrc_seq, gscx__config->stat_svr_seq,
								vs_data(gscx__config->stat_idc_node_id),vs_data(gscx__config->stat_rc_id), vs_data(gscx__config->stat_vrc_id), vs_data(gscx__config->stat_svr_id),
								utime, service->stat_sp_seq, service->stat_cont_seq, service->stat_vol_seq,
								session->end_time - session->start_time, (session->client_ip?session->client_ip:"-"),
								session->res_body_size + session->res_hdr_size, session->res_body_size,	/* 파일 크기는 전송량과 동일하게 기록한다. */
								200, /* session이 있는 경우 응답코드는 무조건 200이다 */
								vs_data(service->name), session->path);
		}

		logger_put(scx_http_stat_logger, log_line, n);
		__thread_jmp_working = 0;
	}
	else {
		/* SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
		TRACE((T_ERROR, "%s() SIGSEGV occured. %s:%d\n", __func__,  __FILE__, __LINE__));
	}
}

void
standalonestat_wlock() {
	pthread_rwlock_wrlock(&__standalonestat_lock);
}

void
standalonestat_rlock() {
	pthread_rwlock_rdlock(&__standalonestat_lock);
}

void
standalonestat_unlock() {
	pthread_rwlock_unlock(&__standalonestat_lock);
}

/*
 * 통계에 사용되는 서비스 목록을 모두 지운다.
 */
int
standalonestat_service_clear_all()
{
	iList.Clear(gscx__standalonestat_service_list);
	TRACE((T_DAEMON, "The services has been cleared all from stat list.\n"));
	return 0;
}


/*
 * 통계에 사용될 서비스(볼륨) 목록 추가하는 함수
 */
int
standalonestat_service_push(service_info_t *service)
{
	iList.Add(gscx__standalonestat_service_list, (void *)&service);
//	printf("The service(%s) has been added to the stat list.\n", vs_data(service->name));
	TRACE((T_DAEMON, "The service(%s) has been added to the stat list.\n", vs_data(service->name)));
	return 0;
}


