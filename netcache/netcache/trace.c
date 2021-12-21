/*
 * generic trace module
 * trace mask should be bitwise.
 *
 * remark) this module is still incomplete!
 *
 */
#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#include <dirent.h>
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <ctype.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <snprintf.h>


#include "trace.h"
#include "util.h"
#include "log.h"


static pthread_mutex_t		s__io_lock = PTHREAD_MUTEX_INITIALIZER;
static int					s__trace_init = 0;
static FILE		*			s__trace_output;


static trace_symbol_t	s__symbol_dict[] = {
		{"DEBUG", 		"DEBUG",		T_DEBUG},
		{"TRACE", 		"TRACE",		T_TRACE},
		{"INFO",  		"INFO",			T_INFO},
		{"WARN",  		"WARN",			T_WARN},
		{"ERROR", 		"ERROR",		T_ERROR},
		{"TRIGGER",    	"TRIGGER",		T_TRIGGER},
		{"INODE", 		"INODE",		T_INODE},
		{"BLOCK", 		"BLOCK",		T_BLOCK},
		{"PLUGIN", 		"PLUGIN",		T_PLUGIN},
		{"CACHE", 		"CACHE",		T_CACHE},
		{"ASIO", 		"ASIO",			T_ASIO},
		{"PERF", 		"PERF",			T_PERF},
		{"DAEMON", 		"DAEMON",		T_DAEMON},
		{"API", 		"API",			T_API},
		{"MONITOR", 	"MONITOR",		T_MONITOR},
		{"CLUSTER", 	"CLUSTER",		T_CLUSTER},
		{"STAT", 		"STAT",			T_STAT},
		{"API.OPEN", 		"API",		T_API_OPEN},
		{"API.READ", 		"API",		T_API_READ},
		{"API.WRITE", 		"API",		T_API_WRITE},
		{"API.CLOSE", 		"API",		T_API_CLOSE},
		{"API.VOLUME", 		"API",		T_API_VOLUME},
		{"API.PURGE", 		"API",		T_API_PURGE},
		{"API.GETATTR", 	"API",		T_API_GETATTR}
		};
int						s__symbol_enums = sizeof(s__symbol_dict)/sizeof(trace_symbol_t);
/*\
 *	globallaly accessabile logger
\*/
t_trace_callback		g__trace_proc = NULL;
PUBLIC_IF int 			g__trace_error = T_ERROR;


/* 
 * Current enabled trace mask
 */
int						g__trace_mask = T_WARN|T_ERROR|T_INFO;
LOG_HANDLE             *g_trc_log = NULL;

void trc_log_handle(LOG_HANDLE *plog) { g_trc_log = plog; }

void trc_redirect_file(FILE *f)
{
	s__trace_output = f;
	setbuf(f, NULL);
}
void trc_redirect_output(t_trace_callback logger, trace_symbol_t *symbols, int symbolcount)
{
	int			i;

	g__trace_proc = logger;

	for (i = 0; i < symbolcount; i++)
		strcpy(s__symbol_dict[i].symbol, symbols[i].symbol);
	g__trace_error = (1 << (i-1));
	s__symbol_enums = symbolcount;
}
PUBLIC_IF CALLSYNTAX long
trc_thread_self()
{
	long 	uid;
#ifdef WIN32
	uid = (long)GetCurrentThreadId();
#else
	uid = gettid();
#endif
	return uid;
}
void PUBLIC_IF trc_begin(char *pref)
{
}
void PUBLIC_IF
vtrace(char *fmt, va_list ap)
{
	struct timeval 			tp;
	char					sztime[128];
	char 					depth_str[512];
	int						depth_str_len = sizeof(depth_str);
	char 					*p; 
	int 					n; 
	nc_uint64_t				uid;
	struct tm				tm_now;
	//pthread_mutexattr_t 	mattr;
	sigset_t				tsignal,osignal;


	if (_ATOMIC_CAS(s__trace_init, 0, 1)) {
		if (!s__trace_output) s__trace_output = stderr;
        //pthread_mutexattr_init(&mattr);
        //pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
		//pthread_mutex_init(&s__io_lock, &mattr);
	    //s__thread_tbl = u_ht_table_new(u_ht_direct_hash, u_ht_direct_equal, NULL, NULL);
		if (!s__trace_output) s__trace_output = stderr;
	}


	uid = trc_thread_self();


	p = depth_str;
#if 0
	gettimeofday(&tp, NULL);
	tnow = time(NULL);
	localtime_r(&tnow,&tm_now);
	strftime(sztime, sizeof(sztime), "%m-%d %H:%M:%S", &tm_now);
#else
	gettimeofday(&tp, NULL);
	localtime_r(&tp.tv_sec, &tm_now);
	strftime(sztime, sizeof(sztime), "%H:%M:%S", &tm_now);
#endif


	n = snprintf(p, depth_str_len, "%s/%03d", sztime, (int)(tp.tv_usec / 1000));
	p += n;
	depth_str_len -= n;

	n = snprintf(p, depth_str_len, " {%05llu} [%5s] ", uid, "CUSTM");
	p += n;
	depth_str_len -= n;

#if 0
	for (i = 0; i < lf->depth && depth_str_len > 0; i++) {
		n = snprintf(p, depth_str_len, "%s", "    ");
		p += n;
		depth_str_len -= n;
	}
#endif

	*p = 0;

	sigfillset(&tsignal);
	sigaddset(&tsignal, SIGINT);
	pthread_sigmask(SIG_SETMASK, &tsignal, &osignal);

	pthread_mutex_lock(&s__io_lock);
    s__trace_output = _open_log_file(g_trc_log);
	fprintf(s__trace_output, "%s ", depth_str);
	vfprintf(s__trace_output, fmt, ap);
	fflush(s__trace_output);
	va_end(ap);
	pthread_mutex_unlock(&s__io_lock);
	pthread_sigmask(SIG_SETMASK, &osignal, NULL);
}

#ifndef __DEBUG_TRACE 
void PUBLIC_IF trc_end()
{
}
#else
void PUBLIC_IF trc_end(char *pref)
{
}
#endif
void trc_endx( char *fmt, ...)
{
}
void PUBLIC_IF trc_beginx(char *pref, char *fmt, ...)
{
}

/*\
 *
 *	WARNING!!! 
 *		this function is somewhat risky because the internally used array 
 *		may be overflowed
\*/
void PUBLIC_IF 
trace(nc_uint32_t level, char *fmt, ...)
{
	struct timeval 			tp;
	time_t 					tnow;
	va_list 				ap = {{0}};
	char					sztime[64];
	char 					depth_str[512];
	int						depth_str_len = sizeof(depth_str);
	char 					*p; 
	int 					n; 
	int						uid;
	struct tm				tm_now;
	//pthread_mutexattr_t 	mattr;
	sigset_t				tsignal, osignal;


	if (_ATOMIC_CAS(s__trace_init, 0, 1)) {
		if (!s__trace_output) s__trace_output = stderr;
        //pthread_mutexattr_init(&mattr);
        //pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
		//pthread_mutex_init(&s__io_lock, &mattr);
		if (!s__trace_output) s__trace_output = stderr;
	}

	if (!trc_is_on(level)) {
		return;
	}


	if (g__trace_proc) {
		va_start(ap, fmt);
		(*g__trace_proc)(level, fmt, ap);
		va_end(ap);
		return;
	}

	uid = trc_thread_self();


	p = depth_str;
#if 0
	gettimeofday(&tp, NULL);
	tnow = time(NULL);
	localtime_r(&tnow,&tm_now);
	//strftime(sztime, sizeof(sztime), "%m-%d %H:%M:%S", &tm_now);
	strftime(sztime, sizeof(sztime), "%H:%M:%S", &tm_now);
#else
	gettimeofday(&tp, NULL);
	localtime_r(&tp.tv_sec, &tm_now);
	strftime(sztime, sizeof(sztime), "%H:%M:%S", &tm_now);
#endif
	/* CHG 2018-07-04 huibong 1/1000 시간 단위 및 Thread ID 출력 format 변경 (#32230) */
	n = snprintf(p, depth_str_len, "%s.%03d", sztime, (int)(tp.tv_usec / 1000));
	p += n;
	depth_str_len -= n;

	n = snprintf(p, depth_str_len, " %05d [%5s] ", uid, trc_lookup_symbol(level));
	p += n;
	depth_str_len -= n;

#if 0
	for (i = 0; i < lf->depth && depth_str_len > 0; i++) {
		n = snprintf(p, depth_str_len, "%s", "    ");
		p += n;
		depth_str_len -= n;
	}
	*p = 0;
#endif

	sigfillset(&tsignal);
	sigaddset(&tsignal, SIGINT);
	pthread_sigmask(SIG_SETMASK, &tsignal, &osignal);

	pthread_mutex_lock(&s__io_lock);
    s__trace_output = _open_log_file(g_trc_log);
	va_start(ap, fmt);
	fprintf(s__trace_output, "%s ", depth_str);
	vfprintf(s__trace_output, fmt, ap);
	fflush(s__trace_output);
	va_end(ap);
	pthread_mutex_unlock(&s__io_lock);
	pthread_sigmask(SIG_SETMASK, &osignal, NULL);
}
void PUBLIC_IF 
trace_f(nc_uint32_t level, const char *func, char *fmt, ...)
{
	struct timeval 			tp;
	va_list 				ap = {{0}};
	char					sztime[64];
	char 					depth_str[512];
	int						depth_str_len = sizeof(depth_str);
	char					tname[32]="";
	char 					*p;
	int 					n;
	int						uid;
	struct tm				tm_now;
	//pthread_mutexattr_t 	mattr;
	sigset_t				tsignal,osignal;


	if (_ATOMIC_CAS(s__trace_init, 0, 1)) {
		if (!s__trace_output) s__trace_output = stderr;
        //pthread_mutexattr_init(&mattr);
        //pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
		//pthread_mutex_init(&s__io_lock, &mattr);
		if (!s__trace_output) s__trace_output = stderr;
	}

	if (!trc_is_on(level)) {
		return;
	}


	if (g__trace_proc) {
		va_start(ap, fmt);
		(*g__trace_proc)(level, fmt, ap);
		va_end(ap);
		return;
	}

	uid = trc_thread_self();


	p = depth_str;
#if 0
	gettimeofday(&tp, NULL);
	tnow = time(NULL);
	localtime_r(&tnow, &tm_now);
	//strftime(sztime, sizeof(sztime), "%m-%d %H:%M:%S", &tm_now);
	strftime(sztime, sizeof(sztime), "%H:%M:%S", &tm_now);
	
#else
	gettimeofday(&tp, NULL);
	localtime_r(&tp.tv_sec, &tm_now);
	strftime(sztime, sizeof(sztime), "%H:%M:%S", &tm_now);
#endif
	/* CHG 2018-07-04 huibong 1/1000 시간 단위 및 Thread ID 출력 format 변경 (#32230) */
	n = snprintf(p, depth_str_len, "%s.%03d", sztime, (int)(tp.tv_usec / 1000));

	p += n;
	depth_str_len -= n;

	pthread_getname_np(pthread_self(), tname, sizeof(tname));
	tname[10] = 0; 
	n = snprintf(p, depth_str_len, " %10s.%06d [%5s] %s:", tname, uid, trc_lookup_symbol(level), func);
	p += n;
	depth_str_len -= n;

#if 0
	for (i = 0; i < lf->depth && depth_str_len > 0; i++) {
		n = snprintf(p, depth_str_len, "%s", "    ");
		p += n;
		depth_str_len -= n;
	}
	*p = 0;
#endif
	sigfillset(&tsignal);
	sigaddset(&tsignal, SIGINT);
	pthread_sigmask(SIG_SETMASK, &tsignal, &osignal);

	pthread_mutex_lock(&s__io_lock);
    s__trace_output = _open_log_file(g_trc_log);
	va_start(ap, fmt);
	fprintf(s__trace_output, "%s ", depth_str);
	vfprintf(s__trace_output, fmt, ap);
	fflush(s__trace_output);
	va_end(ap);
	pthread_mutex_unlock(&s__io_lock);

	pthread_sigmask(SIG_SETMASK, &osignal, NULL);
}
PUBLIC_IF CALLSYNTAX FILE *
trc_fd()
{
	return s__trace_output;
}
PUBLIC_IF CALLSYNTAX void
trc_depth(nc_uint32_t x)
{
    int 			i;
    char 			buf[128]={0};

	g__trace_mask = x|T_MONITOR;
    for(i=0; i<s__symbol_enums; i++)
        if(((x >> i) & 1) == 1) {
            strcat(buf, s__symbol_dict[i].symbol);
            strcat(buf,"|");
        }
    buf[strlen(buf)-1]='\0';
	TRACE((T_INFO, "Global log mask changed to 0x%08X (%s)\n", g__trace_mask,buf));
}
PUBLIC_IF CALLSYNTAX unsigned int
trc_set_mask(char *str)
{
	char 	*			p = (char *)alloca(strlen(str)+1);
	char 	*			ptok = NULL;
	int 				i;
	unsigned int 		mask = 0;


	strcpy(p, str);

	ptok = strtok(p, ",|");
	while (ptok) { 

		while (strchr(" \t", *ptok)) ptok++;

		for (i = 0;i < sizeof(s__symbol_dict)/sizeof(trace_symbol_t); i++) {
			if (strcasecmp(s__symbol_dict[i].symbol, ptok) == 0) {
				mask |= s__symbol_dict[i].flag;
				break;
			}
		}
		ptok = strtok(NULL, ",|");
	}
	trc_depth(mask);
	return mask;

}
char *trc_hex(char *buf, char *toenc, int len)
{
	char *p = buf;
	int n, j, c=0;

	while (len > 0) {
		n = sprintf(p, "[%02d]  ", c++);
		p += n;
		for (j = 0; len > 0 && j< 16 && len > 0; j++) {
#if 0
			if (*toenc != 0 && isascii(*toenc) && isprint(*toenc))
				n = sprintf(p, "%2c ", *toenc);
			else
				n = sprintf(p, "%02X ", 0xFF & (*toenc));
#else
			n = sprintf(p, "0x%02x ", 0xFF & (*toenc));
#endif
			p += n;
			toenc++;
			len --;
		}
	}
	*p = 0;
	return buf;
}

char * 
trc_format(char *io_str, int iostrlen, char *fmt, va_list ap)
{
	char 				depth_str[256];
	char 				*p, *p2;
	int 				n;
	int					uid;
	int					depthlen = sizeof(depth_str);
	//pthread_mutexattr_t mattr;


	if (_ATOMIC_CAS(s__trace_init, 0, 1)) {
		if (!s__trace_output) s__trace_output = stderr;
        //pthread_mutexattr_init(&mattr);
        //pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
		//pthread_mutex_init(&s__io_lock, &mattr);
		if (!s__trace_output) s__trace_output = stderr;
	}
	uid = trc_thread_self();



	p = depth_str;

	/* thread unique ID */
	n = snprintf(p, depthlen, "<%02d> ", uid);
	p += n;
	depthlen -= n;


	*p = 0;
	p2 = io_str;

	n = snprintf(p2, iostrlen, "%s ", depth_str);
	iostrlen -= n;
	p2 += n;

	/* add user-provided string to (depth+function)string */
	//va_start(ap, fmt);
	n = vsnprintf(p2, iostrlen-1, fmt, ap);
	//va_end(ap);
	p2 += n;
	*p2 = 0;

	return io_str;
}
char *
trc_lookup_symbol(nc_uint32_t level)
{
	int					mask = level & g__trace_mask;
	static		char	default_symbol[] = "UNKN";
	int		i;

	for (i = s__symbol_enums-1; i >= 0 ; i--) {
		if ( ((1 << i) & mask) != 0) return (char *)s__symbol_dict[i].display;
	}
	return (char *)default_symbol;
}
#ifdef HAVE_SYSLOG_H
void
trc_syslog_redirect(nc_uint32_t level, char *fmt, va_list ap)
{
	int		i;				/* debug, trace, info, warn, error */
	static int maplevel[] = {LOG_DEBUG, LOG_INFO, LOG_INFO, LOG_WARNING, LOG_ERR};
	for (i = s__symbol_enums-1; i >= 0; i++) {
		if (((i << i) & level) != 0) break;
	}
	if (i < 0) return;

	vsyslog(maplevel[i], fmt, ap);

}
#endif
char *
trc_dump_tid(pthread_t *tid, char *buf)
{
	int 	i, n;
	char 	*p = buf;
	char    *ctid = (char *)tid;
	for (i = 0; i < sizeof(pthread_t); i++)  {
		n = sprintf(p, "%02X", 0xFF & ctid[i]);
		p += n;
	}
	*p = 0;
	return buf;
}
int
trc_is_on(nc_uint32_t x)
{
	return ((g__trace_mask & x) != 0) ;
}
