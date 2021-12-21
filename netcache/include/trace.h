#ifndef __TRACE_H__
#define __TRACE_H__
#include <stdarg.h>
#include <signal.h>
#ifdef UNIX
#define USE_SYSLOG
#include <syslog.h>
#endif /* UNIX*/
#include <pthread.h>
#include <lock.h>
#include "util.h"

#include "log.h"

#define	T_DEBUG		(1<<0)
#define	T_TRACE		(1<<1)
#define	T_INFO		(1<<2)
#define	T_WARN		(1<<3)
#define	T_ERROR		(1<<4)
#define	T_TRIGGER	(1<<5)
#define	T_INODE 	(1<<6)
#define	T_BLOCK 	(1<<7)
#define	T_DAV   	(1<<8)
#define	T_CACHE 	(1<<9)
#define	T_ASIO 		(1<<10)
#define	T_PERF 		(1<<11)
#define T_DAEMON  	(1<<12)
#define	T_API		(1<<13)
#define	T_MONITOR	(1<<14)
#define	T_CLUSTER	(1<<15)
#define T_STAT		(1<<16)
#define T_API_OPEN				(1 << 17)
#define T_API_READ				(1 << 18)
#define T_API_WRITE				(1 << 19)
#define T_API_CLOSE				(1 << 20)
#define T_API_VOLUME			(1 << 21)
#define T_API_PURGE				(1 << 22)
#define T_API_GETATTR			(1 << 23)

#define T_PLUGIN 	T_DAV

#ifdef __cplusplus
extern "C" {
#endif
typedef void (*t_trace_callback)(nc_uint32_t mask, char *format, va_list vap);

typedef struct {
	char			symbol[64]; /* for example DEBUG, TRACE, INFO,..etc */
	char			display[64]; /* for example DEBUG, TRACE, INFO,..etc */
	nc_uint32_t  	flag;
} trace_symbol_t;



int trc_is_on(nc_uint32_t x);
void trc_begin(char *);
void trc_beginx(char *, char *fmt, ...);
#ifdef __DEBUG_TRACE
void trc_end(char *pref);
#else
void trc_end(void);
#endif

void trc_endx(char *fmt, ...);
void trace(nc_uint32_t leve, char *fmt, ...);
void trace_f(nc_uint32_t level, const char *func, char *fmt, ...);
char * trc_hex(char *buf, char *toenc, int len);
/* get the thread unique id */
long trc_thread_self(void);
/* format trace string into io_str */
char * trc_format(char *io_str, int iostrlen, char *fmt, va_list ap);
/* lookup the symbol corresponding the level */
char * trc_lookup_symbol(nc_uint32_t level);
/* redirect trace output to the given callback */
void trc_redirect_output(t_trace_callback logger, trace_symbol_t *symbols, int symbolcount);
void trc_syslog_redirect(nc_uint32_t level, char *fmt, va_list ap);
void trc_redirect_file(FILE *f);
void trc_log_handle(LOG_HANDLE * plog);
char * trc_dump_tid(pthread_t *tid, char *buf);
PUBLIC_IF CALLSYNTAX long trc_thread_self();
PUBLIC_IF CALLSYNTAX FILE * trc_fd();
PUBLIC_IF CALLSYNTAX void vtrace(char *fmt, va_list ap);
PUBLIC_IF CALLSYNTAX unsigned int trc_set_mask(char *str);
PUBLIC_IF CALLSYNTAX unsigned int trc_get_mask();
PUBLIC_IF CALLSYNTAX void trc_depth(nc_uint32_t x);



#if !defined(TRACE)
#	ifndef TRC_BEGIN
#		define	TRC_BEGIN(x)	trc_begin x
#	endif

#	ifndef TRC_BEGINX
#		define	TRC_BEGINX(x)	trc_beginx x
#	endif 

#	ifndef TRC_END
#		ifdef __DEBUG_TRACE
#			define	TRC_END			trc_end(__func__)
#		else
#			define	TRC_END			trc_end()
#		endif
#	endif

#	define TRACE_ON(x) 	trc_is_on(x)

#	define	TRC_ENDX(x)		trc_endx x
#	define	TRACE(x)		TRACE_NF x
#	define	TRACE_N(mask, ...)	if (TRACE_ON(mask)) trace(mask, __VA_ARGS__)
#	define	TRACE_NF(mask, ...)	if (TRACE_ON(mask)) trace_f(mask, __func__, __VA_ARGS__)
#	define	TRACE_X(mask, sign, ...)	if (TRACE_ON(mask)) trace_f(mask, sign, __VA_ARGS__)
#endif
#	define	TRC_DEPTH(n)	trc_depth n
#	define	TRC_MASK(n)		trc_depth n
#	define	TRC_HEX(x)		trc_hex x
#	define	TRC_REDIRECT(x)	trc_redirect_output x
#	define	TRC_REDIRECT_FILE(x)	trc_redirect_file x
#	define 	TRC_LOG_HANDLE(x) trc_log_handle x


#if 0
#	error ("BABO2")
#	define 	TRACE_ON(x) 	U_FALSE
#	define	TRC_BEGIN(x)	
#	define	TRC_BEGINX(x)	
#	define	TRC_END			
#	define	TRC_ENDX(x)		
#	define	TRACE(x)	



#	define	TRC_DEPTH(n)	
#	define	TRC_HEX(x)		
#	define	TRC_DEPTH(n)	
#	define	TRC_MASK(n)	
#	define	TRC_REDIRECT(x)	
#	define	TRC_REDIRECT_FILE(x)	
#	define TRC_LOG_HANDLE(x)

#endif




#ifndef ASSERT


 #ifdef NC_RELEASE_BUILD
   #define		ASSERT(x)					if (!(x)) { \
												char *p=NULL; \
												TRACE((g__trace_error, (char *)"Error in Expr '%s' at %d@%s\n", \
													#x, __LINE__, __FILE__));  \
												*p = 0; \
									    	}

 #else
   #define		ASSERT(x)					if (!(x)) { \
												char *p=NULL; \
												TRACE((g__trace_error, (char *)"Error in Expr '%s' at %d@%s\n", \
													#x, __LINE__, __FILE__));  \
												*p = 0; \
									    	}
 #endif
#endif


#ifndef DEBUG_ASSERT
#ifdef NC_RELEASE_BUILD

#define	DEBUG_ASSERT(x)

#else
#define	DEBUG_ASSERT(x)  	if (!(x)) { \
								TRACE((g__trace_error, (char *)"Error in Expr '%s' at %d@%s\n", \
									#x, __LINE__, __FILE__));  \
								*p = 0; \
					}
#endif
#endif




#define		CHECK_RETURN(x, success,buf)	if ((x) != success) \
												TRACE((g__trace_error, "Error in Expr '%s' at %d@%s:'%s'\n", \
													#x, __LINE__, __FILE__, strerror_r(errno, buf, sizeof(buf)))) 

#define		CHECK_RETURN_ERRNO(x, e,buf)	if ((e =(x)) != 0) \
												TRACE((g__trace_error, "Error in Expr '%s' at %d@%s:'%s'\n", \
													#x, __LINE__, __FILE__, strerror_r(e, buf, sizeof(buf)))) 


extern t_trace_callback		 g__trace_proc;
extern int					 g__trace_debug;
extern int					 g__trace_trace;
extern int					 g__trace_warning;
extern int					 g__trace_error;

#ifdef __cplusplus
};
#endif



#endif /*__TRACE_H__*/

