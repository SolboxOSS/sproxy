#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <memory.h>
#include <string.h>
#include <time.h>
//#include <ncapi.h>
//#include <trace.h>
#include <getopt.h>
#include <errno.h>
#include <syslog.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
#include "scx_util.h"

extern uid_t 		gscx__real_uid;
extern gid_t 		gscx__real_gid;

#define 	LOG_ROTATE_SEQ		1		//로그 파일이 지정된 크기를 넘긴 경우
#define 	LOG_ROTATE_DATE		2		//날짜가 변경된 경우
#define 	LOG_ROTATE_SIGNAL	3		//외부에서 Rotate Signal을 받은 경우
#define 	LOG_PAGE_SIZE		4096

static void logger_flush_nolock(scx_logger_t *logger);
static char * logger_make_path(scx_logger_t *logger, char *lfname, int sufxno);
static int logger_need_flush(scx_logger_t *logger, size_t len);
static void logger_check_rotation(scx_logger_t *logger);
static void logger_prepare_internal(scx_logger_t *logger);
static int logger_rotate_internal(scx_logger_t *logger, int type);
static int logger_sprintf(char *lfname, char *fmt);
static scx_ringbuffer_t * scx_rb_create(size_t size);
static void scx_rb_destroy(scx_ringbuffer_t *ring);
static  int scx_rb_is_full(scx_ringbuffer_t *ring);
static int scx_rb_is_empty(scx_ringbuffer_t *ring);
static int scx_rb_get_writable(scx_ringbuffer_t *ring, char **ptr, size_t *len);
static int scx_rb_commit_write(scx_ringbuffer_t *ring, size_t len);
static int scx_rb_get_readable(scx_ringbuffer_t *ring, char **ptr, size_t *len);
static int scx_rb_commit_read(scx_ringbuffer_t *ring, size_t len);
static int scx_rb_is_runout(scx_ringbuffer_t *ring, size_t len);
static void scx_rb_rewind(scx_ringbuffer_t *ring);
static int scx_rb_valid_range(scx_ringbuffer_t *ring, char *ptr, off_t len) ;
static void scx_rb_validate(scx_ringbuffer_t *rb);
static int get_day_in_year();


scx_logger_t *
logger_open(char *str, uint64_t threshold, uint32_t flag, int max_kepts)
{
	scx_logger_t 	*nl = NULL;
	int	pagesize = getpagesize();

	nl = (scx_logger_t *)SCX_CALLOC(1, sizeof(scx_logger_t));

	nl->log_flags = flag;
	nl->log_maxkepts = max_kepts;
	strcpy(nl->log_template, str); 
	nl->log_written = 0;
	nl->log_threshold = threshold;
	nl->log_fd = -1;
	nl->log_flushsize = LOG_PAGE_SIZE;
	nl->log_yday = get_day_in_year();
	nl->ring = scx_rb_create(LOG_PAGE_SIZE*8);	/* 32768 */
	pthread_mutex_init(&nl->log_writerlock, NULL); 
	logger_prepare_internal(nl);
	return nl;
}
void
logger_close(scx_logger_t *logger)
{
	char 	lfname_new[1024];
	char	*filename = NULL;
	logger_flush(logger);
	pthread_mutex_destroy(&logger->log_writerlock);
	scx_rb_destroy(logger->ring);
	close(logger->log_fd);
	if ((logger->log_flags & SCX_LOGF_STAT) != 0) {
		/* 종료시에도 통계파일을 완료 경로(done)로 이동 시킨다. */
		filename = strrchr(logger->log_path, '/');
		snprintf(lfname_new, 1024, "%s/%s" , logger->stat_done_path, filename);
		rename(logger->log_path, lfname_new);
	}
	SCX_FREE(logger);
}
void
logger_flush(scx_logger_t *logger)
{
	pthread_mutex_lock(&logger->log_writerlock);
	logger_flush_nolock(logger);
	pthread_mutex_unlock(&logger->log_writerlock);
}

static void
logger_flush_nolock(scx_logger_t *logger)
{
	size_t 		readable_len;
	char 		*readable_ptr = NULL;
	int 		len;
	do {
		scx_rb_validate(logger->ring);
		scx_rb_get_readable(logger->ring, &readable_ptr, &readable_len);
		if (readable_len > 0) {
			/*
			 * 어쩌면 너무 클 수 있음, 너무 클 경우, 일정 크기까지만
			 * 디스크에 저장하는게 효과적일 수 있음.
			 * 향후 다듬어야할 필요 있음
			 * 저장이 실패하는 경우도 발생. - 파일 시스템 풀
			 */
			ASSERT(scx_rb_valid_range(logger->ring, readable_ptr, readable_len) != 0);
			len = write(logger->log_fd, readable_ptr, readable_len);
			scx_rb_commit_read(logger->ring, readable_len);
			scx_rb_validate(logger->ring);
		}
	} while (readable_len > 0) ;
}

/*
 * 단순 로깅만 제공
 * 저장 실패 시 오류(음수) 반환.
 * 오류가 발생하는 경우는 ringbuffer가 full이 나는 경우
 * 대기하지않고 리턴한다.
 */
int
logger_put(scx_logger_t *logger, const char *log, int len)
{
	size_t 		writable_len = 0;
	char 		*writable_ptr = NULL;
	size_t 		readable_len = 0;
	char 		*readable_ptr = NULL;
	int 		r = 0, nf=0;
	int 		wlen  = 0,wrlen = 0;




	TRC_BEGIN((__func__));

	pthread_mutex_lock(&logger->log_writerlock);
	scx_rb_validate(logger->ring);
	logger_check_rotation(logger);


	/*
	 * 현재 링버퍼에 남아서 디스크로 저장안된 로그를 확인 후
	 * 존재하면 저장
	 */
	scx_rb_get_readable(logger->ring, &readable_ptr, &readable_len);
	if (readable_len > 0 && (nf = logger_need_flush(logger, readable_len)))  {
		/*
		 * 어쩌면 너무 클 수 있음, 너무 클 경우, 일정 크기까지만
		 * 디스크에 저장하는게 효과적일 수 있음.
		 * 향후 다듬어야할 필요 있음
		 * 저장이 실패하는 경우도 발생. - 파일 시스템 풀
		 */
		wlen = min(readable_len, logger->log_flushsize);
		if (wlen > 0) {
//			ASSERT(logger->log_fd > 0);
			//TRACE((T_INFO, "readable =%ld, flushsize=%ld, wlen=%ld\n", readable_len, logger->log_flushsize, wlen));
			if (logger->log_fd > 0) {	// inode full, readonly filesystem  등으로 로그 파일 생성에 실패한 경우는 skip 한다.
				ASSERT(scx_rb_valid_range(logger->ring, readable_ptr, wlen) != 0);
				wrlen = write(logger->log_fd, readable_ptr, wlen);
			}
			// logger->log_fd가 없어서 실제 파일에 기록을 하지 않았더라도 버퍼는 비운다.
			scx_rb_commit_read(logger->ring, wlen);
			//TRACE((T_INFO, "write (%ld) -> %ld\n", wlen, wrlen));
		}
		else {
			scx_rb_rewind(logger->ring);
		}
	}
	else {
		TRACE((T_DEBUG, "logger [%s] -  readable_len=%d, need_flush=%d(given strlen=%d)\n", 
						logger->log_template, readable_len, nf, len));
	}

	scx_rb_validate(logger->ring);
	/*
	 * 현재 링버퍼에 caller가 요청한 길이 만큼을 저장할 공간이 있는지
	 * 확인 후 있으면 저장
	 * 없으면 해당 로그 drop
	 */
	scx_rb_get_writable(logger->ring, &writable_ptr, &writable_len);
	if (writable_len > len) {
		ASSERT(scx_rb_valid_range(logger->ring, writable_ptr, len) != 0);
		memcpy(writable_ptr, log, len);
		scx_rb_commit_write(logger->ring, len);
		logger->log_written += len;
	}
	else {
		/* 링버퍼 풀*/
		r = -1;
		TRACE((T_DAEMON, "logger [%s] -  writable_len=%d, (given strlen=%d), FULL\n", 
						logger->log_template, writable_len, len));
	}
	scx_rb_validate(logger->ring);
	pthread_mutex_unlock(&logger->log_writerlock);
	TRC_END;
	return r;

}

void
logger_set_done_path(scx_logger_t *logger, char *path)
{
	snprintf(logger->stat_done_path, 1024, "%s", path);
}

int
logger_reopen(scx_logger_t *logger)
{
	if ((logger->log_flags & SCX_LOGF_SIGNAL) == 0) {
		return -1;
	}
	pthread_mutex_lock(&logger->log_writerlock);

	logger_rotate_internal(logger, LOG_ROTATE_SIGNAL);

	pthread_mutex_unlock(&logger->log_writerlock);
	return 0;
}

/*
 * 호출 전 파일 리스트
 * 		xxxxx
 * 		xxxxx.0
 * 		xxxxx.1
 * 		xxxxx.2
 * 		xxxxx.3
 * 		xxxxx.4
 * 		xxxxx.5
 *  리턴 시 파일 리스트
 * 	    xxxxx  신규 생성
 *		xxxxx   -----> xxxx.0
 * 		xxxxx.0 -----> xxxx.1
 * 		xxxxx.1 -----> xxxx.2
 * 		xxxxx.2 -----> xxxx.3
 * 		xxxxx.3 -----> xxxx.4
 * 		xxxxx.4 -----> xxxx.5
 * 		xxxxx.5 -----> overwritten
 */
static int
logger_rotate_internal(scx_logger_t *logger, int type)
{
	int 	i;
	char 	lfname_old[1024];
	char 	lfname_new[1024];
	char	*filename = NULL;

#if 0
	/*
	 * 로테이트 최대 값을 가진 파일은 삭제
	 */
	logger_make_path(lfname); 
	unlink(lfname);
#endif
	/*
	 * 저장해야 할 내용을 먼저 저장하고
	 * 현재 사용중인 파일을 닫는다
	 */
	if (logger->log_fd > 0) {	// inode full, readonly filesystem  등으로 로그 파일 생성에 실패한 경우는 skip 한다.
		logger_flush_nolock(logger);
		close(logger->log_fd);

		if(type == LOG_ROTATE_SEQ){
			/*
			 * 나머지 파일 이동
			 */
			for (i = logger->log_maxkepts-1; i > 0; i--) {
				logger_make_path(logger, lfname_old, i-1);  /* 기존 파일 명*/
				logger_make_path(logger, lfname_new, i); 	/* 신규 파일 명*/
				rename(lfname_old, lfname_new);
			}


			logger_make_path(logger, lfname_old, -1); /* .x 확장자 없는 파일 명*/
			logger_make_path(logger, lfname_new, 0);  /* .0 */
			if (rename(lfname_old, lfname_new) != 0) {
				TRACE((T_ERROR, "%s ==> %s : %s\n", lfname_old, lfname_new, strerror(errno)));
			}
		}
		else if (type == LOG_ROTATE_SIGNAL) {
			if ((logger->log_flags & SCX_LOGF_STAT) != 0) {
				/* 생성이 완료된 통계파일을 완료 경로(done)로 이동 시킨다. */
				filename = strrchr(logger->log_path, '/');
				snprintf(lfname_new, 1024, "%s/%s" , logger->stat_done_path, filename);
				rename(logger->log_path, lfname_new);
			}
		}
	}

	logger->log_fd = -1;

	logger_prepare_internal(logger);
}
static void 
logger_prepare_internal(scx_logger_t *logger)
{
	char 	lfname[1024]="";
	struct stat fs;
	
	if ((logger->log_flags & SCX_LOGF_FILE) != 0) {
		logger_make_path(logger, lfname, -1); /* 확장자 없는 파일명 생성 */
		if (logger->log_fd < 0) {
			logger->log_fd = open(lfname, O_CREAT|O_APPEND|O_WRONLY, 0644);
			snprintf(logger->log_path, 1024, "%s", lfname);
		}
	}
	else if ((logger->log_flags & SCX_LOGF_PIPE) != 0) {
		/* pipe 생성 */
		logger->log_fd = -1; /* 나중에 구현 */
	}
	logger->log_written = 0;
	logger->log_yday = get_day_in_year();
	if (logger->log_fd > 0) {
		if ((logger->log_flags & SCX_LOGF_STAT) != 0) {
			/* 통계 파일 전환은 INFO level 로그로 남기기에는 너무 많아서 로그 레벨을 다르게 가져 간다. */
			TRACE((T_DAEMON, "logger, %s - prepared as fd[%d]\n", lfname, logger->log_fd));
		}
		else
		{
			TRACE((T_INFO, "logger, %s - prepared as fd[%d]\n", lfname, logger->log_fd));
		}
		fchown(logger->log_fd, gscx__real_uid, gscx__real_gid);
		if (fstat(logger->log_fd, &fs) == 0) {
			logger->log_written = fs.st_size;
		}
		else {
			fprintf(stderr, "log file get stat - error(%s)\n", strerror(errno));
			TRACE((T_WARN, "log file get stat - error(%s)\n", strerror(errno)));
		}
	}
	else {
		TRACE((T_ERROR, "logger, %s - preparation failed(%s)\n", lfname, strerror(errno)));
		syslog(LOG_PID|LOG_ERR, "%s log file(%s) open failed(%s).", PROG_SHORT_NAME, lfname, strerror(errno));
	}
}
static void
logger_check_rotation(scx_logger_t *logger)
{
	if ( ((logger->log_flags & SCX_LOGF_ROTATE) == 0) ||
		 ((logger->log_flags & SCX_LOGF_PIPE) != 0))
		return; /*로테이션 설정 없음 */

	/*
	 * 날짜가 변경된 경우 rotation한다.
	 */
	if (logger->log_yday != get_day_in_year()) {
		logger_rotate_internal(logger, LOG_ROTATE_DATE);
		return;
	}
	if (logger->log_threshold < 0) {
		/*
		 * 로테이션 기준 크기가 없음,
		 * 계속 기존 파일에 저장
		 */
		return;
	}
	/* 
	 * 현재까지 저장한 크기를 이용하여 rotation여부 결정
	 */
	if (logger->log_written > logger->log_threshold) {
		logger_rotate_internal(logger, LOG_ROTATE_SEQ);
//		logger_prepare_internal(logger);
	}
}
static int
logger_need_flush(scx_logger_t *logger, size_t len)
{
	int r;
	r = (logger->log_flushsize <= len || scx_rb_is_runout(logger->ring, len));
	//TRACE((T_INFO, "logger_need_flush: readable_len=%ld, need_flush=%d\n", len, r));
	return r;
}
static char *
logger_make_path(scx_logger_t *logger, char *lfname, int sufxno)
{
	int 	n;

	n = logger_sprintf(lfname, logger->log_template);
	lfname[n] = 0;
	if (sufxno >= 0) {
		sprintf(lfname+n, ".%d", sufxno);
	}
	return lfname;
}
static int
logger_sprintf(char *lfname, char *fmt)
{
	char 	*pfmt = fmt;
	int 	in_format = 0;
	char 	*target = lfname;
	int 	n;
	time_t	now;
	struct tm 	tm_now;
	int 		ol = 0;


	now = time(NULL);
	localtime_r(&now, &tm_now);
	
	while (*pfmt) {
		switch (*pfmt) {
			case '%':
				if (in_format) {
					*target++ = *pfmt;
					in_format = 0;
					ol++;
				}
				else {
					in_format = 1;
				}
				break;
			case 'Y':
				if (in_format) {
					n = sprintf(target, "%04d", tm_now.tm_year+1900);
					target += n;
					ol += n;
					in_format = 0;
				}
				else {
					*target++ = *pfmt;
					ol += 1;
				}
				break;
			case 'y':
				if (in_format) {
					n = sprintf(target, "%02d", (tm_now.tm_year%100));
					target += n;
					ol += n;
					in_format = 0;
				}
				else {
					*target++ = *pfmt;
					ol += 1;
				}
				break;
			case 'm':
				if (in_format) {
					n = sprintf(target, "%02d", tm_now.tm_mon+1);
					target += n;
					ol += n;
					in_format = 0;
				}
				else {
					*target++ = *pfmt;
					ol += 1;
				}
				break;
			case 'M':
				if (in_format) {
					n = sprintf(target, "%02d", tm_now.tm_min);
					target += n;
					ol += n;
					in_format = 0;
				}
				else {
					*target++ = *pfmt;
					ol += 1;
				}
				break;
			case 'd':
				if (in_format) {
					n = sprintf(target, "%02d", tm_now.tm_mday);
					target += n;
					ol += n;
					in_format = 0;
				}
				else {
					*target++ = *pfmt;
					ol += 1;
				}
				break;
			case 'h':
				if (in_format) {
					n = sprintf(target, "%02d", tm_now.tm_hour);
					target += n;
					ol += n;
					in_format = 0;
				}
				else {
					*target++ = *pfmt;
					ol += 1;
				}
				break;
			case 's':
				if (in_format) {
					n = sprintf(target, "%02d", tm_now.tm_sec);
					target += n;
					ol += n;
					in_format = 0;
				}
				else {
					*target++ = *pfmt;
					ol += 1;
				}
				break;
			default:
				*target++ = *pfmt;
				in_format = 0;
				ol += 1;
				break;
		}
		pfmt++;
	}
	*pfmt = '\0';

	return ol;
}

static void
scx_rb_validate(scx_ringbuffer_t *rb)
{
	ASSERT((rb->magic_a ^ rb->magic_b) == 0);
}
static scx_ringbuffer_t *
scx_rb_create(size_t size)
{
	scx_ringbuffer_t *ring = NULL;
	ring = (scx_ringbuffer_t *)SCX_CALLOC(1, sizeof(scx_ringbuffer_t));

	ring->magic_a 	= 0xa55a;
	ring->magic_b 	= 0xa55a;
	ring->buffer 	= (char *)SCX_CALLOC(1, size + 10);
	ring->size 		= size;
	ring->write 	= 0;
	ring->read 		= 0;
	ring->eot 		= 0;
	//TRACE((T_INFO, "%s: (size=%ld, write=%ld)\n", __func__, ring->size, ring->write));
	pthread_mutex_init(&ring->lock, NULL);
	pthread_cond_init(&ring->cond, NULL);
	return ring;

}
static int
scx_rb_valid_range(scx_ringbuffer_t *ring, char *ptr, off_t len)
{
	//return (((long)(ptr - ring->buffer) + len) <= ring->size);
	return (
			((ptr + len) <= ((char *)ring->buffer + ring->size)) &&
			((ptr + len) >= ring->buffer) 
			 );
}
static void
scx_rb_destroy(scx_ringbuffer_t *ring)
{
	pthread_mutex_destroy(&ring->lock);
	pthread_cond_destroy(&ring->cond);
	SCX_FREE(ring->buffer);
	SCX_FREE(ring);
}
static  int
scx_rb_is_full(scx_ringbuffer_t *ring)
{
	return ((ring->write + 1) % ring->size == ring->read);
}
static int
scx_rb_is_empty(scx_ringbuffer_t *ring)
{
	return ring->read == ring->write;
}

static int
scx_rb_get_writable(scx_ringbuffer_t *ring, char **ptr, size_t *len)
{
	int 			once = 1;
	struct timeval 	tv;
	struct timespec	ts;
	long 			delta_sec = 0;
	long 			msec = 5; /* 5 milisec */
	long 			vd = 0;





	*len 	= (ring->size - ring->write);
	*ptr 	= (ring->buffer + ring->write);
	vd 		= (long)(ring->write - ring->read);

	/*
	 * full인 경우, 딱 한번 5msec만 대기 
	 */
	pthread_mutex_lock(&ring->lock);

#if 1
	ASSERT(vd >= 0);
	if (*len < LOG_PAGE_SIZE && vd > 0) {
		/*
		 *  +------------+-------+-------+
		 *  |            |valid  |       |
		 *  +------------+-------+-------+
		 *              read    write   size 
		 *
		 */
		 memmove(ring->buffer, ring->buffer + ring->read, ring->write - ring->read);
		 ring->write = ring->write - ring->read;
		 ring->read  = 0;
		 *ptr = (ring->buffer + ring->write);
	}
	else if (vd == 0) {
		ring->write 	= 0;
		ring->read 		= 0;
		 *ptr 			= ring->buffer;
	}
#endif
#if 0
	while (*len == 0 && once) {
		gettimeofday(&tv, NULL);
		ts.tv_sec  	= tv.tv_sec + (delta_sec = (msec/1000L));
		ts.tv_nsec 	= tv.tv_usec*1000L + ((msec - delta_sec*1000L)*1000000L);
		pthread_cond_timedwait(&ring->cond, &ring->lock, &ts);
		*len = (ring->size - ring->write);
		*ptr = (ring->buffer + ring->write);
		once = 0;
	}
#endif
	pthread_mutex_unlock(&ring->lock);
	return 0;
}
static int
scx_rb_commit_write(scx_ringbuffer_t *ring, size_t len)
{
	ring->write += len;

	//TRACE((T_INFO, "logger_commit_write: size=%ld, write=%ld (len=%d)\n", ring->size, ring->write, len));
	/* reader에게 신호 전달 */
	pthread_mutex_lock(&ring->lock);
	pthread_cond_signal(&ring->cond);
	pthread_mutex_unlock(&ring->lock);
}
static int
scx_rb_get_readable(scx_ringbuffer_t *ring, char **ptr, size_t *len)
{
	*ptr = ring->buffer + ring->read;
	*len = (ring->write - ring->read);
	//TRACE((T_INFO, "get_readable : %ld available\n", *len));

	return 0;
}
static int
scx_rb_set_eot(scx_ringbuffer_t *ring)
{
	ring->eot = 1;
	return 0;
}
static int
scx_rb_eot(scx_ringbuffer_t *ring)
{
	return ring->eot;
}
static int
scx_rb_commit_read(scx_ringbuffer_t *ring, size_t len)
{
	ring->read += len;
	if (ring->read >= ring->size) {
		ring->read -= ring->size;
		ring->write -= ring->size;
	}
	//TRACE((T_INFO, "logger_commit_read: size=%ld, read=%ld write=%ld, (len=%d)\n", ring->size, ring->read, ring->write, len));
	/* writer에게 신호 전달 */
	pthread_mutex_lock(&ring->lock);
	pthread_cond_signal(&ring->cond);
	pthread_mutex_unlock(&ring->lock);
	return 0;
}

static int
scx_rb_is_runout(scx_ringbuffer_t *ring, size_t len)
{
	//TRACE((T_INFO, "logger_is_runout: size - write = %ld(size=%ld, write=%ld)\n", ring->size - ring->write, ring->size, ring->write));
	return (ring->size - ring->write) <= LOG_PAGE_SIZE;
}
static void
scx_rb_rewind(scx_ringbuffer_t *ring)
{
	ring->read = 0;
	ring->write = 0;
}

int get_day_in_year()
{
	time_t	now;
	struct tm 	tm_now;
	now = time(NULL);
	localtime_r(&now, &tm_now);
	return tm_now.tm_yday;
}

