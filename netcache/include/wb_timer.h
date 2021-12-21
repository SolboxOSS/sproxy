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
                                 wb_timer.h   
                              ------------------
      begin              :  Mon Jan 28  2008
      email              :  frau   (shlee@solbox.com)
                            comate (wglee@solbox.com)
 ******************************************************************************/

/******************************************************************************
    change history
    2008. 1. 28 : initial code (frau)
 ******************************************************************************/
#ifndef _HASHED_TIMER_H
#define _HASHED_TIMER_H
#include <pthread.h>
#include "bt_tmlist.h"

#define typecheck(type,x) \
({  type __dummy; \
   typeof(x) __dummy2; \
   (void)(&__dummy == &__dummy2); \
   1; \
})

#define time_after_eq(a,b)  \
   (typecheck(unsigned long long, a) && \
    typecheck(unsigned long long, b) && \
    ((long long)(a) - (long long)(b) >= 0))
#define time_before_eq(a,b) time_after_eq(b,a)

struct tvec_t_base_s;

struct timer_list {
	struct list_head 	entry;
	unsigned long long 	expires;

	pthread_mutex_t 	lock;
	char 				func_name[32];
	unsigned long 		magic;
	void 				(*function) (void *);
	void 				*data;
	void 				*tphandle;
	//void *damallocta;
	unsigned short 		recoverable:1;			/*1 if it need recover */
	unsigned short 		brun_forked:1;			/*1 if it should be run in a separate thread */
	unsigned short		bset:1; 				/* 1 if timer set */
	short 				brunning;				

	struct tvec_t_base_s *base;
};
typedef struct {
	void 	(*timer)(void *);
	void	*data;
} wb_timer_info_t;

#define TIMER_MAGIC	0x4b87ad6e
#define IS_TIMER_RUN(t)	(((t)->base != NULL) || (t)->brunning == 1)

#define TIMER_INITIALIZER(_function, _expires, _data) {		\
		.function = (_function),			\
		.expires = (_expires),				\
		.data = (_data),				\
		.base = NULL,					\
		.magic = TIMER_MAGIC,				\
		.lock = PTHREAD_MUTEX_INITIALIZER,			\
	}

/***
 * init_timer - initialize a timer.
 * @timer: the timer to be initialized
 *
 * init_timer() must be done to a timer prior calling *any* of the
 * other timer functions.
 */
static inline void init_timer(struct timer_list *timer)
{
	timer->base = NULL;
	timer->magic = TIMER_MAGIC;
	MUTEX_INIT(&timer->lock, NULL);
}

/***
 * timer_pending - is a timer pending?
 * @timer: the timer in question
 *
 * timer_pending will tell whether a given timer is currently pending,
 * or not. Callers must ensure serialization wrt. other operations done
 * to this timer, eg. interrupt contexts, or other CPUs on SMP.
 *
 * return value: 1 if the timer is pending, 0 if not.
 */
static inline int timer_pending(const struct timer_list *timer)
{
	return timer->base != NULL;
}
static inline int destroy_timer(struct timer_list *timer)
{
	int		r = -1;
	if (!timer_pending(timer)) {
		r = 0;
		MUTEX_DESTROY(&timer->lock);
	}
	else {
		fprintf(stderr, "timer is pending, free failed\n");
	}
	return r;
}

extern void add_timer_on(void *vt_base, struct timer_list *timer);
extern int del_timer(void *vt_base, struct timer_list *timer);
extern int __mod_timer(void *base, struct timer_list *timer, unsigned long long expires);
extern int mod_timer(void *, struct timer_list *timer, unsigned long long expires);
extern void lock_timer(void *base, int lock);

/***
 * add_timer - start a timer
 * @timer: the timer to be added
 *
 * The kernel will do a ->function(->data) callback from the
 * timer interrupt at the ->expired point in the future. The
 * current time is 'jiffies'.
 *
 * The timer's ->expired, ->function (and if the handler uses it, ->data)
 * fields must be set prior calling this function.
 *
 * Timers with an ->expired field in the past will be executed in the next
 * timer tick.
 */
static inline void add_timer(void *vt_base, struct timer_list *timer)
{
	__mod_timer(vt_base, timer, timer->expires);
}

# define del_timer_sync(t) del_timer(t)

typedef void (*cb_timer_t) (void *);
void add_timer_wrap(int port, struct timer_list *timer, unsigned int toff, cb_timer_t cb, void *data);

void *init_timers(void);
void run_local_timers(void);
void it_real_fn(unsigned long);
void run_timers(void *vt_base);
unsigned long long get_jiffies(void);
#endif

/******************************************************************************
                             E N D  O F  F I L E
 ******************************************************************************/
