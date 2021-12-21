#ifndef __RDTSC_H__
#define __RDTSC_H__
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include "trace.h"

#ifdef __PROFILE

/*
 * gettimeofday()를 사용하지않고 clock_gettime(MONOTONIC)을 사용하는형태로
 * 수정
 * gettimeofday()의 호출값은 NTP등에 의해서 조정되어
 * 값이 계속 증가하지 않고 조정되어 시스템이 이상하게 동작할 수있음
 * by weon@solbox.com 
 * 2019.8.20
 */

#include <sys/time.h>


static inline long 
__diff_usec(perf_val_t *s, perf_val_t *e)
{
	register long long ss = (s->tv_sec)*1000000LL + s->tv_nsec/1000LL;
	register long long se = (e->tv_sec)*1000000LL + e->tv_nsec/1000LL;

	return (long)(se - ss);
}
static inline long long 
__clock_mtime(perf_val_t x)
{
	register long long l = x.tv_sec*1000LL + x.tv_nsec/1000000;
	return l;
}
#		define	DO_PROFILE_USEC(s, e, d) for (clock_gettime(CLOCK_MONOTONIC, &(s)),d=-1; d  == (long)-1; clock_gettime(CLOCK_MONOTONIC, &(e)), d=__diff_usec(&(s),&(e)))
#		define PROFILE_CHECK(s) 	clock_gettime(CLOCK_MONOTONIC, &(s))
#		define PROFILE_INIT(v_)	{v_.tv_sec = 0; v_.tv_nsec = 0;}
#		define PROFILE_NULL(v_)	(v_.tv_sec == 0) 
#		define PROFILE_GAP_USEC(s,e) 	__diff_usec(&(s),&(e))
#		define PROFILE_GAP_MSEC(s,e) 	(__diff_usec(&(s),&(e))/1000.0)
#		define PROFILE_GAP_SEC(s,e) 	(__diff_usec(&(s),&(e))/1000000.0)
#		define CHECK_WRAP_TIME(_vs_,_ve_, _l, _s_)		{float msec = PROFILE_GAP_MSEC(_vs_, _ve_); if (msec > _l) TRACE((T_WARN, "%s - slow %.2f msec\n", (_s_), msec));}
#else

typedef unsigned long perf_val_t;
#define	DO_PROFILE_USEC(s,e,d)
#define	PROFILE_INIT(v_)
#define PROFILE_NULL(v_)
#define	PROFILE_GAP_USEC(s,e)			0
#define PROFILE_CHECK(s)
#define CHECK_WRAP_TIME(_vs_, _ve_, _l, _s_)		


#endif

#endif

