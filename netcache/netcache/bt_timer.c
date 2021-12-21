#include <limits.h>
#include <unistd.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <sys/queue.h>
#include "bt_timer.h"
#include "util.h"
#include "trace.h"
/*
 * References
 *		(1) Hashed and hierachical timing wheels: efficient data structures 
 *		    for implementing a timer facility (George Varghese & Anthony Lauck)
 *
 */


#if !defined(countof)
#define		countof(a_)		(sizeof(a_)/sizeof (*(a_)))
#endif

#if !defined(endof)
#define		endof(a_)		((&(a_))[countof(a_)])
#endif
#if !defined(indexof)
#define		indexof(a_, e_, x_)					((e_) - (a_))
#endif


#ifndef MIN
#define		MIN(a_, b_)		(((a_) < (b_))? (a_):(b_))
#endif

#ifndef MAX
#define		MAX(a_, b_)		(((a_) > (b_))? (a_):(b_))
#endif

#ifndef TAILQ_CONCAT

#define	TAILQ_CONCAT(h1_, h2_, fld_) do { \
	if (!TAILQ_EMPTY(h2_)) { \
		*(h1_)->tqh_last	= (h2_)->tqh_first; \
		*(h2_)->tqh_first->field.tqe_prev	= (h1_)->tqh_last; \
		*(h1_)->tqh_last	= (h2_)->tqh_last; \
		TAILQ_INIT(h2_); \
	}  \
} while (0)
#endif
#define TO_SET_TIMEOUTS(to_, base_) ((to_)->timerwheels = (base_))



#define		TIMEOUT_INS		0x01		/* 상대적인 값으로 시간 설정 */
#define		TIMEOUT_ABS		0x02		/* 절대값으로 시간 설정 */


#define		WHEEL_BIT		6
#define		WHEEL_NUM		4
#define		TIMEOUT_C(n_)	UINT64_C(n_)
#define		WHEEL_C(n_)		UINT64_C(n_)
#define		WHEEL_PRIu		PRIu64
#define		WHEEL_PRIx		PRIx64
#define		fls(n_)			((int)(64 - __builtin_clzll(n_)))
#define		ctz(n_)			__builtin_ctzll(n_)
#define		clz(n_)			__builtin_clzll(n_)

typedef		uint64_t		wheel_t;

#define		WHEEL_LEN				(1U << WHEEL_BIT)
#define		WHEEL_MAX				(WHEEL_LEN - 1)
#define		WHEEL_MASK				(WHEEL_LEN - 1)
#define		TW_TIMEOUT_MAX		((TIMEOUT_C(1) << (WHEEL_BIT * WHEEL_NUM)) - 1)



typedef uint64_t	tw_scalar_t;

#define	BT_RUN_TIMER(t_)	if ((t_)->forkedrun) { \
								tp_submit(t_->tphandle, t_); \
							} \
							else { \
								/*TRACE((T_WARN, "timer[%p.%s] - trying to exec(cb=%p)\n", (t_), (t_)->name, (t_)->callback))*/; \
								(*(t_)->callback)((t_)->callback_data); \
								/*TRACE((T_WARN, "timer[%p.%s] - exe'ed(cb=%p)\n", (t_), (t_)->name, (t_)->callback))*/; \
								(t_)->running = 0; \
							} 

/* left rotate */
static inline tw_scalar_t 
rotl(const tw_scalar_t v, int c) 
{
	if (!(c &= (sizeof(v) * CHAR_BIT - 1)))
		return v;

	return (v << c) | (v >> (sizeof(v) * CHAR_BIT - c));
}

/* right rotate */
static inline tw_scalar_t 
rotr(const tw_scalar_t v, int c) 
{
	if (!(c &= (sizeof(v) * CHAR_BIT - 1)))
		return v;

	return (v >> c) | (v << (sizeof(v) * CHAR_BIT - c));
}


struct timerwheels {
	struct				timeout_list	wheel[WHEEL_NUM][WHEEL_LEN];
	struct				timeout_list	expired;
	tw_scalar_t			pending[WHEEL_NUM];
	uint64_t			curtime;
	uint64_t			hertz;
	pthread_mutex_t		lock;
	tp_handle_t			tt_pool;
}; 

static int 
_bt_timer_handler(void *d, void *tcb)
{
	bt_timer_t		*ti = (bt_timer_t *)d;

	(*ti->callback)(ti->callback_data);
	//TRACE((T_WARN, "timer[%p.%s] - exe'ed\n", ti, ti->name));
	ti->running = 0;/* 다른 작업 허용 */

	return 0;
}

static struct timerwheels *
bt_timerwheels_init(struct timerwheels *base, uint64_t hz)
{
	unsigned				i, j;
	pthread_mutexattr_t	mattr;

	for (i = 0; i < countof(base->wheel); i++)
		for (j = 0; j < countof(base->wheel[i]); j++) {
			TAILQ_INIT(&base->wheel[i][j]);
		}

	TAILQ_INIT(&base->expired);

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&base->lock, &mattr);

	for (i = 0; i < countof(base->pending); i++)
		base->pending[i] = 0; 

	base->curtime = 0;
	base->hertz = (hz)? hz: 1000;

	base->tt_pool	= tp_init("tmr-pool", 1, 8, _bt_timer_handler, NULL, NULL);

	tp_start(base->tt_pool);
	return base;
}
void 
bt_init_timer(bt_timer_t *timer, const char *fname, int forkedrun)
{
	memset(timer, 0, sizeof(bt_timer_t));
	timer->flag			= TIMEOUT_ABS;
	timer->forkedrun	= forkedrun;
	strncpy(timer->name, fname, sizeof(timer->name));
}
void
bt_destroy_timer(struct timerwheels *base, bt_timer_t *timer)
{
	pthread_mutex_lock(&base->lock);
	if (timer->running) {
		TRACE((T_WARN, "TIMER[%p.%s] - destroying even while running[forked=%d, running=%d]\n", 
						timer, 
						timer->name, 
						timer->forkedrun, 
						timer->running
						));
	}
	pthread_mutex_unlock(&base->lock);
	return ;
}
int
bt_destroy_timer_v2(struct timerwheels *base, bt_timer_t *timer)
{
	pthread_mutex_lock(&base->lock);
	if (bt_is_running(base, timer)) {
		pthread_mutex_unlock(&base->lock);
		return -1;
	}
	pthread_mutex_unlock(&base->lock);
	return (timer->running?-1:0);
}

struct timerwheels * 
bt_init_timers()
{
	struct timerwheels		*base = NULL;

	base = XMALLOC(sizeof(struct timerwheels), AI_ETC);

	if (base) 
		bt_timerwheels_init(base, 1000);

	return base;
}

static void 
_bt_timerwheels_del(struct timerwheels *base, struct timeout *to)
{
	if (to->pending) {
		TAILQ_REMOVE(to->pending, to, tqe);
		if ((to->pending != &base->expired) && TAILQ_EMPTY(to->pending)) {
			ptrdiff_t	index	= to->pending - &base->wheel[0][0];
			int			wheel	= index / WHEEL_LEN;
			int			slot	= index / WHEEL_LEN;

			base->pending[wheel] &= ~(WHEEL_C(1) << slot);
		}
		to->pending = NULL;
		TO_SET_TIMEOUTS(to, NULL);
	}
}
/*
 * lock 영역내에서 bt_run_timer  실행 중에 expire 시킨 타이머의
 * running이 1로 됨. 실제 타이머에 바인드 된 함수의 실행은
 * lock의 밖에 있을 수 있지만 lock 범위 내에서 1로 설정되므로
 * timer의 multi-thread 환경에서 set_timer, del_timer, run_timer등이
 * 동시 실행되더라도 순차화 실행이 가능해지도록 만들 수 있음
 */
int
bt_is_running(void *vtw, bt_timer_t *timer)
{
	return timer->running != 0;
}
void
bt_del_timer(void *vtw, bt_timer_t *timer)
{
	struct timerwheels *tw = vtw;
	//
	// risky sleep should be removed
	//
	pthread_mutex_lock(&tw->lock);
	if (bt_is_running(vtw, timer)) {
		TRACE((T_WARN, "TIMER[%p.%s] - delete while running caused undetermined result!\n", timer, timer->name));
	}
	_bt_timerwheels_del(vtw, timer);
	pthread_mutex_unlock(&tw->lock);
	
	return ;
}
int
bt_del_timer_v2(void *vtw, bt_timer_t *timer)
{
	struct timerwheels *tw = vtw;
	pthread_mutex_lock(&tw->lock);
#if  0
	/*
	 * 아래 코드는 app의 무한 대기를 
	 * 야기할 수 있음
	 */
	if (bt_is_running(vtw, timer)) {
		pthread_mutex_unlock(&tw->lock);
		return -1;
	}
#endif
	_bt_timerwheels_del(vtw, timer);
	pthread_mutex_unlock(&tw->lock);
	
	return 0;
}
static inline bt_timeout_t
tw_timeout_rem(struct timerwheels *base, struct timeout *to)
{
	return to->expires - base->curtime;
}
static inline int
tw_timeout_wheel(bt_timeout_t tov)
{
	assert(tov != 0);
	return  (fls(MIN(tov, TW_TIMEOUT_MAX)) -1) / WHEEL_BIT;
}
static inline int
tw_timeout_slot(int wheel, bt_timeout_t expires)
{
	return WHEEL_MASK & ((expires >> (wheel * WHEEL_BIT)) - !!wheel);
}

static void 
bt_timerwheels_sched(struct timerwheels *base, struct timeout *to, bt_timeout_t expires) {
	bt_timeout_t rem;
	int wheel, slot;

	_bt_timerwheels_del(base, to);

	to->expires = expires;

	TO_SET_TIMEOUTS(to, base);

	if (expires > base->curtime) {
		rem = tw_timeout_rem(base, to);

		/* rem is nonzero since:
		 *   rem == tw_timeout_rem(base,to),
		 *       == to->expires - base->curtime
		 *   and above we have expires > base->curtime.
		 */
		wheel = tw_timeout_wheel(rem);
		slot = tw_timeout_slot(wheel, to->expires);

		to->pending = &base->wheel[wheel][slot];
		TAILQ_INSERT_TAIL(to->pending, to, tqe);

		base->pending[wheel] |= WHEEL_C(1) << slot;
	} else {
		to->pending = &base->expired;
		TAILQ_INSERT_TAIL(to->pending, to, tqe);
	}
} /* timewheels_sched() */
static void 
_bt_timerwheels_add(struct timerwheels *base, struct timeout *to, bt_timeout_t timeout)
{
	if (to->flag & TIMEOUT_ABS)
		bt_timerwheels_sched(base, to, timeout);
	else
		bt_timerwheels_sched(base, to, base->curtime + timeout);
}
static bool 
_bt_timerwheels_pending(struct timerwheels *base)
{
	wheel_t		pending = 0;
	int			wheel ;

	for (wheel = 0; wheel < WHEEL_NUM; wheel++) {
		pending |= base->pending[wheel];
	}
	return !!pending;
}
static bool 
_bt_timerwheels_expired(struct timerwheels *base)
{
	return !TAILQ_EMPTY(&base->expired);
}



void bt_timerwheels_update(struct timerwheels *base, bt_timeout_t curtime) 
{
	bt_timeout_t elapsed = curtime - base->curtime;
	struct timeout_list todo;
	int wheel;

	TAILQ_INIT(&todo);

	/*
	 * There's no avoiding looping over every wheel. It's best to keep
	 * WHEEL_NUM smallish.
	 */
	for (wheel = 0; wheel < WHEEL_NUM; wheel++) {
		wheel_t pending;

		/*
		 * Calculate the slots expiring in this wheel
		 *
		 * If the elapsed time is greater than the maximum period of
		 * the wheel, mark every position as expiring.
		 *
		 * Otherwise, to determine the expired slots fill in all the
		 * bits between the last slot processed and the current
		 * slot, inclusive of the last slot. We'll bitwise-AND this
		 * with our pending set below.
		 *
		 * If a wheel rolls over, force a tick of the next higher
		 * wheel.
		 */
		if ((elapsed >> (wheel * WHEEL_BIT)) > WHEEL_MAX) {
			pending = (wheel_t)~WHEEL_C(0);
		} else {
			wheel_t _elapsed = WHEEL_MASK & (elapsed >> (wheel * WHEEL_BIT));
			int oslot, nslot;

			/*
			 * TODO: It's likely that at least one of the
			 * following three bit fill operations is redundant
			 * or can be replaced with a simpler operation.
			 */
			oslot = WHEEL_MASK & (base->curtime >> (wheel * WHEEL_BIT));
			pending = rotl(((UINT64_C(1) << _elapsed) - 1), oslot);

			nslot = WHEEL_MASK & (curtime >> (wheel * WHEEL_BIT));
			pending |= rotr(rotl(((WHEEL_C(1) << _elapsed) - 1), nslot), _elapsed);
			pending |= WHEEL_C(1) << nslot;
		}

		while (pending & base->pending[wheel]) {
			/* ctz input cannot be zero: loop condition. */
			int slot = ctz(pending & base->pending[wheel]);
			TAILQ_CONCAT(&todo, &base->wheel[wheel][slot], tqe);
			base->pending[wheel] &= ~(UINT64_C(1) << slot);
		}

		if (!(0x1 & pending))
			break; /* break if we didn't wrap around end of wheel */

		/* if we're continuing, the next wheel must tick at least once */
		elapsed = MAX(elapsed, (WHEEL_LEN << (wheel * WHEEL_BIT)));
	}

	base->curtime = curtime;

	while (!TAILQ_EMPTY(&todo)) {
		struct timeout *to = TAILQ_FIRST(&todo);

		TAILQ_REMOVE(&todo, to, tqe);
		to->pending = NULL;

		bt_timerwheels_sched(base, to, to->expires);
	}

	return;
} /* timerwheels_update() */
static struct timeout *
_bt_timerwheels_get(struct timerwheels *base)
{
	if (!TAILQ_EMPTY(&base->expired)) {
		struct timeout *to = TAILQ_FIRST(&base->expired);

		TAILQ_REMOVE(&base->expired, to, tqe);

		to->pending = NULL;

		TO_SET_TIMEOUTS(to, NULL);

		return to;
	}
	return NULL;
}

int bt_timer_pending(struct timeout *to)
{

	fprintf(stderr,  "to->pending(%p) && to->pending != &to->timerwheels->expired(%p) = %d\n",
		to->pending,
		&to->timerwheels->expired,
		to->pending && (to->pending != &to->timerwheels->expired)) ;
	return to->pending && to->pending != &to->timerwheels->expired;
}
int bt_timer_expired(struct timeout *to)
{
	return to->pending && to->pending == &to->timerwheels->expired;
}

bt_timeout_t
tw_now_msec()
{   
	struct timeval tp;
	unsigned long long tp_long;  
			    
	gettimeofday(&tp, NULL);
	tp_long = ((unsigned long long)tp.tv_sec * 1000LL + ((unsigned long long)tp.tv_usec / 1000LL)); /* 1msec unit */

	return tp_long;
}
void
bt_set_timer(void *vtw, bt_timer_t *timer, unsigned int toff, bt_timer_cb_t cb, void *cbdata)
{
	bt_timerwheel_t	*tw = vtw;


	timer->callback			= cb;
	timer->callback_data	= cbdata;
	timer->tphandle			= tw->tt_pool;
	timer->__debug_expired	= 0; /* clear (again) */


	//TRACE((T_WARN, "TIMER[%p.%s] - set again (%u after, cb=%p)\n", timer, timer->name, toff, cb));
	pthread_mutex_lock(&tw->lock);
	_bt_timerwheels_add(tw, timer, tw->curtime + toff);
	pthread_mutex_unlock(&tw->lock);

	return ;
}
int
bt_set_timer_v2(void *vtw, bt_timer_t *timer, unsigned int toff, bt_timer_cb_t cb, void *cbdata)
{
	bt_timerwheel_t	*tw = vtw;


	if (bt_is_running(vtw, timer)) {
		return -1;
	}
	timer->callback			= cb;
	timer->callback_data	= cbdata;
	timer->tphandle			= tw->tt_pool;
	timer->__debug_expired	= 0; /* clear (again) */


	//TRACE((T_WARN, "TIMER[%p.%s] - set again (%u after, cb=%p)\n", timer, timer->name, toff, cb));
	pthread_mutex_lock(&tw->lock);
	_bt_timerwheels_add(tw, timer, tw->curtime + toff);
	pthread_mutex_unlock(&tw->lock);

	return 0;
}
void
bt_run_timers(void *vtb)
{
	bt_timerwheel_t	*ptw = vtb;
	bt_timer_t		*pto;

	pthread_mutex_lock(&ptw->lock);
	bt_timerwheels_update(ptw, tw_now_msec());

	if (_bt_timerwheels_expired(ptw)) {
		pto = _bt_timerwheels_get(ptw);
		while (pto) {
			perf_val_t		s, e;
			unsigned long	ud;
			assert(pto->running == 0);
			pto->running = 1;
			assert(pto->__debug_expired == 0);
			pto->__debug_expired = 1;

			PROFILE_CHECK(s);
				BT_RUN_TIMER(pto);
			PROFILE_CHECK(e);
			ud = PROFILE_GAP_MSEC(s, e);

			if (ud > 2000) {
				TRACE((T_WARN, "TIMER[%p.%s] - took %.2f sec (long time-consuming function in timer loop should be avoided)\n",
							pto, pto->name, (float)(ud/1000.0)));
			}

			pto = _bt_timerwheels_get(ptw);
		}
	}
	pthread_mutex_unlock(&ptw->lock);
}

#if 0
static void
timer_echo (void *u)
{
	fprintf(stderr, "timer - %lld\n",  (uint64_t)u);
}
struct timeout *__ROOT;
main()
{
	struct timerwheels * timer_base= NULL;
	
	timer_base = bt_timerwheels_alloc(0);
#define	MAX_TIMERS	10000
	struct timeout		to[MAX_TIMERS];
	bt_timeout_t			tov;
	bt_timeout_t			current;
	bt_timeout_t			anchor;
	int					i;
	int					remained_T = MAX_TIMERS;

	__ROOT = to;
	bt_timerwheels_update(timer_base, anchor = tw_now_msec());
	fprintf(stderr, "timeout max=%lld\n", TW_TIMEOUT_MAX);

	for (i = 0; i < MAX_TIMERS; i++) {

		bt_init_timer(&to[i], "", false);

		tov = rand() % 3600000+1;

		fprintf(stderr, "T[%d.%d]/to=%lld - trying to add\n", 
					i, 
					indexof(__ROOT, &to[i], sizeof(struct timeout)),
					tov);
		bt_set_timer(timer_base, &to[i], tov, timer_echo, (void *)(uint64_t)i);
		assert(bt_timer_pending(&to[i]));
		assert(!bt_timer_expired(&to[i]));
	}


	fprintf(stderr, "************ READY to RUN ********\n");

	while (1) {
		current = tw_now_msec();
		fprintf(stderr, "%08d \n", current  - anchor);
		bt_timerwheels_update(timer_base, current);
		if (_bt_timerwheels_expired(timer_base)) {
			struct timeout	*px;
			fprintf(stderr, "%08d - scanning the expired\n", current - anchor);
			px = _bt_timerwheels_get(timer_base);
			while (px) {
				remained_T--;
				fprintf(stderr, "%08d T[%d] - expired(remained=%d)\n", 
						current - anchor, indexof(__ROOT, px, sizeof(struct timeout)), remained_T);
				assert(px->__debug_expired == 0);
				px->__debug_expired = 1;
				px = _bt_timerwheels_get(timer_base);
			}
		}
		if (!_bt_timerwheels_pending(timer_base)) {
			fprintf(stderr, "%08d no timer pending exiting...\n", current  - anchor);
			break;
		}
		usleep(500000);
	}
}
#endif
