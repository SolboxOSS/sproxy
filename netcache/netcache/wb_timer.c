/*****************************************************************************

                   (c) COPYRIGHT 2008 by SOLUTIONBOX, Inc.
                          All rights reserved.

     This software is confidential and proprietary to SOLUTIONBOX, Inc.  
     No part of this software may be reproduced, stored, transmitted, 
     disclosed or used in any form or by any means other than as 
     expressly provided by the written license agreement
     between Besttoday and its licensee.
     
                             SOLUTIONBOX, Inc.
                           4F, Sunghwan Bldg.  
                     770-9 Yeoksam-Dong, Kangnam-Gu,
                      Seoul, 135-080, South Korea
 
                        Tel: +82 (2) 2182-3600
                        Fax: +82 (2) 2058-2651     

******************************************************************************/

/******************************************************************************
                                 wb_timer.c 
                              ------------------
      begin              :  Mon Jan 28  2008
      email              :  frau   (shlee@solbox.com)
                            comate (wglee@solbox.com)
 ******************************************************************************/

/******************************************************************************
    change history
    2008. 1. 28 : initial code (frau)
 ******************************************************************************/
#include <config.h>
#include <netcache_config.h>
#include <netcache_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include "util.h"
#include "bt_tmlist.h"
#include "wb_timer.h"
#include "trace.h"
#include "rdtsc.h"

#define	BUG_ON(x)				/*if (x) fprintf(stderr, "BUG FOUND in %d@%s\n", __LINE__, __FILE__) */

/*
 * per-CPU timer vector definitions:
 */
#if	0
#define TVN_BITS 6
#define TVR_BITS 8
#else
#define TVN_BITS 7
#define TVR_BITS 9
#endif
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1)
#define TVR_MASK (TVR_SIZE - 1)

typedef struct tvec_s {
	struct list_head vec[TVN_SIZE];
} tvec_t;

typedef struct tvec_root_s {
	struct list_head vec[TVR_SIZE];
} tvec_root_t;

struct tvec_t_base_s {
	pthread_mutex_t lock;
	unsigned long long timer_jiffies;
	struct timer_list *running_timer;
	tvec_root_t tv1;
	tvec_t tv2;
	tvec_t tv3;
	tvec_t tv4;
	tvec_t tv5;
} ____cacheline_aligned_in_smp;

typedef struct tvec_t_base_s tvec_base_t;

static inline void set_running_timer(tvec_base_t * base, struct timer_list *timer)
{
	base->running_timer = timer;
}

/* Fake initialization */
//static tvec_base_t tvec_bases = { PTHREAD_MUTEX_INITIALIZER };

unsigned long long __jiffies_base = 0;
unsigned long long __jiffies_current = 0;
unsigned long long get_jiffies(void)
{
	struct timeval tp;
	unsigned long long tp_long;
	//static long long __current_backup = 0LL;
	//long long t_delta;			//comate 10.24 follow flexd

#if 1
	gettimeofday(&tp, NULL);
	tp_long = ((unsigned long long)tp.tv_sec * 1000LL + ((unsigned long long)tp.tv_usec / 1000LL));	/* 1msec unit */
	//tp_long = (tp.tv_sec*1000 + (tp.tv_usec/1000))/100;   /* 100msec unit*/
#else
	tp_long = time(NULL);
#endif

#if	0
	if (__jiffies_base == 0) {
		__jiffies_base = tp_long;
	}

	return (tp_long - __jiffies_base);
#else
	return tp_long;
#endif
}
static void check_timer_failed(struct timer_list *timer)
{
	int ret;
	static int whine_count;
	if (whine_count < 16) {
		whine_count++;
		fprintf(stderr, "Uninitialised timer!\n");
		fprintf(stderr, "This is just a warning.  Your computer is OK\n");
		fprintf(stderr, "function=0x%p, data=0x%p\n", timer->function, timer->data);
		abort();
		//dump_stack();
	}
	/*
	 * Now fix it up
	 */
	ret = MUTEX_INIT(&timer->lock, NULL);
	if (ret != 0)
		fprintf(stderr, "check_timer_failed - lock error(%s[%d])\n", strerror(ret), ret);

	timer->magic = TIMER_MAGIC;
}

static inline void check_timer(struct timer_list *timer)
{
	if (timer->magic != TIMER_MAGIC)
		check_timer_failed(timer);
}


static void internal_add_timer(tvec_base_t * base, struct timer_list *timer)
{
	unsigned long long expires = timer->expires;
	unsigned long long idx = expires - base->timer_jiffies;
	struct list_head *vec;

	//fprintf(stderr, "internal_add_timer(first) - timer entry(0x%p:p[0x%p]n[0x%p])\n", 
	//              &timer->entry, timer->entry.prev, timer->entry.next);
	if (idx < TVR_SIZE) {
		int i = expires & TVR_MASK;
		vec = base->tv1.vec + i;
	} else if (idx < 1 << (TVR_BITS + TVN_BITS)) {
		int i = (expires >> TVR_BITS) & TVN_MASK;
		vec = base->tv2.vec + i;
	} else if (idx < 1 << (TVR_BITS + 2 * TVN_BITS)) {
		int i = (expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK;
		vec = base->tv3.vec + i;
	} else if (idx < 1 << (TVR_BITS + 3 * TVN_BITS)) {
		int i = (expires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK;
		vec = base->tv4.vec + i;
	} else if ((signed long long) idx < 0) {
		/*
		 * Can happen if you add a timer with expires == jiffies,
		 * or you set a timer to go off in the past
		 */
		vec = base->tv1.vec + (base->timer_jiffies & TVR_MASK);
	} else {
		int i;
		/* If the timeout is larger than 0xffffffff on 64-bit
		 * architectures then we use the maximum timeout:
		 */
		if (idx > 0xffffffffUL) {
			idx = 0xffffffffUL;
			expires = idx + base->timer_jiffies;
		}
		i = (expires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK;
		vec = base->tv5.vec + i;
	}
	/*
	 * Timers are FIFO:
	 */
	tm_list_add_tail(&timer->entry, vec);
}
void lock_timer(void *base, int lock)
{
	tvec_base_t *tb = (tvec_base_t *)base;
	(lock?pthread_mutex_lock(&tb->lock):pthread_mutex_unlock(&tb->lock));
}

int __mod_timer(void *vt_base, struct timer_list *timer, unsigned long long expires)
{
	tvec_base_t *tvec_bases = (tvec_base_t *) vt_base;
	tvec_base_t *old_base, *new_base;
	//unsigned long flags;
	int ret = 0;


	check_timer(timer);

	// how map? 
	pthread_mutex_lock(&timer->lock);
	new_base = tvec_bases;
  repeat:
	old_base = timer->base;

	/*
	 * Prevent deadlocks via ordering by old_base < new_base.
	 */
	if (old_base && (new_base != old_base)) {
		if (old_base < new_base) {
			pthread_mutex_lock(&new_base->lock);
			pthread_mutex_lock(&old_base->lock);
		} else {
			pthread_mutex_lock(&old_base->lock);
			pthread_mutex_lock(&new_base->lock);
		}
		/*
		 * The timer base might have been cancelled while we were
		 * trying to take the lock(s):
		 */
		if (timer->base != old_base) {
			pthread_mutex_unlock(&new_base->lock);
			pthread_mutex_unlock(&old_base->lock);
			goto repeat;
		}
	} else {
		pthread_mutex_lock(&new_base->lock);
		if (timer->base != old_base) {
			pthread_mutex_unlock(&new_base->lock);
			goto repeat;
		}
	}

	/*
	 * Delete the previous timeout (if there was any), and install
	 * the new one:
	 */
	if (old_base) {
		//fprintf(stderr, "__mod_timer - tm_list_del(0x%p)\n", &timer->entry);
		tm_list_del(&timer->entry);
		ret = 1;
	}
	timer->expires = expires;
	internal_add_timer(new_base, timer);
	timer->base = new_base;

	if (old_base && (new_base != old_base)) {
		pthread_mutex_unlock(&old_base->lock);
	}
	pthread_mutex_unlock(&new_base->lock);
	pthread_mutex_unlock(&timer->lock);

	return ret;
}


/***
 * add_timer_on - start a timer on a particular CPU
 * @timer: the timer to be added
 *
 * This is not very scalable on SMP. Double adds are not possible.
 */
void add_timer_on(void *vt_base, struct timer_list *timer)
{
	tvec_base_t *base = (tvec_base_t *) vt_base;
	//unsigned long flags;

	BUG_ON(timer_pending(timer) || !timer->function);

	check_timer(timer);

	pthread_mutex_lock(&base->lock);
	internal_add_timer(base, timer);
	//fprintf(stderr, "add_timer_on - intenal_add(0x%p:p[0x%p]n[0x%p])\n", 
	//              &timer->entry, timer->entry.prev, timer->entry.next);
	timer->base = base;
	pthread_mutex_unlock(&base->lock);
}

/***
 * mod_timer - modify a timer's timeout
 * @timer: the timer to be modified
 *
 * mod_timer is a more efficient way to update the expire field of an
 * active timer (if the timer is inactive it will be activated)
 *
 * mod_timer(timer, expires) is equivalent to:
 *
 *     del_timer(timer); timer->expires = expires; add_timer(timer);
 *
 * Note that if there are multiple unserialized concurrent users of the
 * same timer, then mod_timer() is the only safe way to modify the timeout,
 * since add_timer() cannot modify an already running timer.
 *
 * The function returns whether it has modified a pending timer or not.
 * (ie. mod_timer() of an inactive timer returns 0, mod_timer() of an
 * active timer returns 1.)
 */
int mod_timer(void *vt_base, struct timer_list *timer, unsigned long long expires)
{
	//tvec_base_t   *tvec_bases = (tvec_base_t *)vt_base;
	BUG_ON(!timer->function);

	check_timer(timer);

	/*
	 * This is a common optimization triggered by the
	 * networking code - if the timer is re-modified
	 * to be the same thing then just return:
	 */
	if (timer->expires == expires && timer_pending(timer))
		return 1;

	return __mod_timer(vt_base, timer, expires);
}


/***
 * del_timer - deactive a timer.
 * @timer: the timer to be deactivated
 *
 * del_timer() deactivates a timer - this works on both active and inactive
 * timers.
 *
 * The function returns whether it has deactivated a pending timer or not.
 * (ie. del_timer() of an inactive timer returns 0, del_timer() of an
 * active timer returns 1.)
 */
int del_timer(void *vt_base, struct timer_list *timer)
{
	//unsigned long flags;
	tvec_base_t *base;

	check_timer(timer);

  repeat:
	base = timer->base;
	if (!base)
		return 0;
	pthread_mutex_lock(&base->lock);
	if (base != timer->base) {
		pthread_mutex_unlock(&base->lock);
		goto repeat;
	}
	//fprintf(stderr, "deltimer - tm_list_del(0x%p)\n", &timer->entry);
	tm_list_del(&timer->entry);
	timer->base = NULL;
	pthread_mutex_unlock(&base->lock);

	return 1;
}

static int cascade(tvec_base_t * base, tvec_t * tv, int index)
{
	/* cascade all the timers from tv up one level */
	struct list_head *head, *curr;

	head = tv->vec + index;
	curr = head->next;
	/*
	 * We are removing _all_ timers from the list, so we don't  have to
	 * detach them individually, just clear the list afterwards.
	 */
	while (curr != head) {
		struct timer_list *tmp;

		tmp = tm_list_entry(curr, struct timer_list, entry);
		BUG_ON(tmp->base != base);
		curr = curr->next;
		//fprintf(stderr, "casecade - intenal_add(0x%p:p[0x%p]n[0x%p])\n", 
		//          &tmp->entry, tmp->entry.prev, tmp->entry.next);
		internal_add_timer(base, tmp);
	}
	INIT_LIST_HEAD(head);

	return index;
}


struct tag_fork_run_info {
	void	(*func)(void *);
	void	*data;
};
static void *
timer_run_wrapper(void *v)
{
	struct tag_fork_run_info *fi = (struct tag_fork_run_info *)v;

	fi->func(fi->data);
	XFREE(v);
	return NULL;
}
static void 
timer_fork_run( void *tphandle, void (*function) (void *), void *data)
{
#if 0
	pthread_attr_t	tattr;
	pthread_t 		tid;
	//int				vc;
	size_t 			stksiz;
	struct tag_fork_run_info	*fi;
	
	
	fi = (struct tag_fork_run_info *)XMALLOC(sizeof(struct tag_fork_run_info), AI_ETC);
	fi->func = function;
	fi->data = data;

	pthread_attr_init(&tattr);
	//pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM);
	/* 
	 * making it as detached means that the thread would not be joined any more
	 */	
	stksiz = 8000000; /* 8MB */
	pthread_attr_setstacksize(&tattr, stksiz);
	pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
	my_pthread_create(&tid, &tattr, timer_run_wrapper, (void *)fi);

	pthread_attr_destroy(&tattr);
//	if (vc == 0)
//		pthread_detach(tid);

	//return ((vc != 0)? -1:0); /* thread creation failed */
#else
	wb_timer_info_t	*ti = XMALLOC(sizeof(wb_timer_info_t), AI_ETC);
	ti->timer 	= function;
	ti->data 	= data;
	tp_submit(tphandle, ti);
#endif
}

/***
 * __run_timers - run all expired timers (if any) .
 * @base: the timer vector to be processed.
 *
 * This function cascades all vectors and executes all expired timer
 * vectors.
 */
#define INDEX(N) (base->timer_jiffies >> (TVR_BITS + N * TVN_BITS)) & TVN_MASK

static inline void __run_timers(tvec_base_t * base)
{
#ifdef __PROFILE
	perf_val_t 	ms, me;
	long long	ud;
#endif
	char 		fname[128]="";
	short	run_sep = 0;
	struct timer_list *timer;
	void		*tph = NULL;


	pthread_mutex_lock(&base->lock);
	while (time_after_eq(get_jiffies(), base->timer_jiffies)) {
		struct list_head work_list = LIST_HEAD_INIT(work_list);
		struct list_head *head = &work_list;
		int index = base->timer_jiffies & TVR_MASK;

		/*
		 * Cascade timers:
		 */
		if (!index &&
			(!cascade(base, &base->tv2, INDEX(0))) &&
			(!cascade(base, &base->tv3, INDEX(1))) && !cascade(base, &base->tv4, INDEX(2)))
			cascade(base, &base->tv5, INDEX(3));
		++base->timer_jiffies;
		//base->timer_jiffies = get_jiffies(); /* weon */

		tm_list_splice_init(base->tv1.vec + index, &work_list);
	  repeat:
		if (!tm_list_empty(head)) {
			void (*fn) (void *);
			void *data;
			//TRC_BEGIN(("__run_timer"));

			timer = tm_list_entry(head->next, struct timer_list, entry);
			DEBUG_ASSERT(timer->magic == TIMER_MAGIC);
			_ATOMIC_ADD(timer->brunning, 1); 
			fn = timer->function;
			strcpy(fname, timer->func_name);
			data = timer->data;
			tph = timer->tphandle;

			//fprintf(stderr, "__run_timers - tm_list_del(0x%p:p[0x%p]n[0x%p])\n", 
			//      &timer->entry, timer->entry.prev, timer->entry.next);
			tm_list_del(&timer->entry);
			set_running_timer(base, timer);
			//smp_wmb();
			timer->base = NULL;
			run_sep = timer->brun_forked;
#if	1
			pthread_mutex_unlock(&base->lock);

			if (fn) {
				DO_PROFILE_USEC(ms, me, ud) {
					if (run_sep) {
						timer_fork_run(tph, fn, data);
					}
					else {
						fn(data);
					}
				}
			}
			pthread_mutex_lock(&base->lock);
#else
			/* original source code */
			TRACE((T_TRACE, "timer(0x%p) expired, processing callback(0x%p) with data(0x%p)...\n", timer, fn, data));
			DO_PROFILE_USEC(ms, me, ud) {
				fn(data);
			}
#ifdef __PROFILE
			if (ud > 10000) {
				TRACE((T_INFO, "timer function (%s)(forkrun=%d) - took long time (%.2f msec)\n", 
						fname, run_sep,  (ud/1000.0)));
			}
#endif /* END OF PROFILE */

#endif
			//TRC_END;
			goto repeat;
		}
	}
	set_running_timer(base, NULL);
	pthread_mutex_unlock(&base->lock);
}

/******************************************************************/

/*
 * Timekeeping variables
 */
//unsigned long tick_usec = TICK_USEC;      /* USER_HZ period (usec) */
//unsigned long tick_nsec = TICK_NSEC;      /* ACTHZ period (nsec) */

/* 
 * The current time 
 * wall_to_monotonic is what we need to add to xtime (or xtime corrected 
 * for sub jiffie times) to get to monotonic time.  Monotonic is pegged at zero
 * at zero at system boot time, so wall_to_monotonic will be negative,
 * however, we will ALWAYS keep the tv_nsec part positive so we can use
 * the usual normalization.
 */
struct timespec xtime __attribute__ ((aligned(16)));
struct timespec wall_to_monotonic __attribute__ ((aligned(16)));


/* Don't completely fail for HZ > 500.  */
//int tickadj = 500/HZ ? : 1;       /* microsecs */


/*
 * phase-lock loop variables
 */
/* TIME_ERROR prevents overwriting the CMOS clock */
#if	0
int time_state = TIME_OK;		/* clock synchronization status */
int time_status = STA_UNSYNC;	/* clock status bits        */
long time_offset;				/* time adjustment (us)     */
long time_constant = 2;			/* pll time constant        */
long time_tolerance = MAXFREQ;	/* frequency tolerance (ppm)    */
long time_precision = 1;		/* clock precision (us)     */
long time_maxerror = NTP_PHASE_LIMIT;	/* maximum error (us)       */
long time_esterror = NTP_PHASE_LIMIT;	/* estimated error (us)     */
long time_phase;				/* phase offset (scaled us) */
long time_freq = (((NSEC_PER_SEC + HZ / 2) % HZ - HZ / 2) << SHIFT_USEC) / NSEC_PER_USEC;
					/* frequency offset (scaled ppm) */
long time_adj;					/* tick adjust (scaled 1 / HZ)  */
long time_reftime;				/* time at last adjustment (s)  */
long time_adjust;
long time_next_adjust;
#endif


/* jiffies at the most recent update of wall time */
//unsigned long wall_jiffies = INITIAL_JIFFIES;
unsigned long wall_jiffies = 0;



static tvec_base_t *_init_timers()
{
	int j;
	tvec_base_t *base;
	pthread_mutexattr_t 	mattr;

#if	0
	base = &tvec_bases;
#else
	base = (tvec_base_t *) XMALLOC(sizeof(tvec_base_t), AI_ETC);
	memset(base, 0, sizeof(tvec_base_t));
#endif
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);

	MUTEX_INIT(&base->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);
	for (j = 0; j < TVN_SIZE; j++) {
		INIT_LIST_HEAD(base->tv5.vec + j);
		INIT_LIST_HEAD(base->tv4.vec + j);
		INIT_LIST_HEAD(base->tv3.vec + j);
		INIT_LIST_HEAD(base->tv2.vec + j);
	}
	for (j = 0; j < TVR_SIZE; j++)
		INIT_LIST_HEAD(base->tv1.vec + j);

	base->timer_jiffies = get_jiffies();
	return base;
}
void run_timers(void *vt_base)
{
	tvec_base_t *base = (tvec_base_t *) vt_base;
	__run_timers(base);
}

void *init_timers(void)
{
	tvec_base_t *base;
	base = _init_timers();
	return base;
}

/******************************************************************************
                             E N D  O F  F I L E
 ******************************************************************************/
