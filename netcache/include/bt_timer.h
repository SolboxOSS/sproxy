#ifndef __BT_TIMER_T
#define __BT_TIMER_T
#include <sys/queue.h>
#include <threadpool.h>

typedef struct timeout		bt_timer_t;
typedef struct timerwheels	bt_timerwheel_t;
typedef		uint64_t		bt_timeout_t;

TAILQ_HEAD(timeout_list, timeout);

struct timeout {
	int						flag;
	char					name[96]; /* 이전 bt_timer의 크기와 맞추기위해서 (solproxy와 바이너리 연동성확보)*/
	bt_timeout_t			expires; /* absolute*/
	struct timeout_list		*pending;
	TAILQ_ENTRY(timeout)	tqe;
	void					*tphandle;
	bt_timerwheel_t			*timerwheels;
	short					forkedrun; /* 1: 별개의 쓰레드에서 실행이 필요*/
	short					running; /* 타이머가 실행 중일 때 1임. 1이 된 상태에서 다른 모든 operation은 대기함 */
	void					(*callback)(void *);
	void					*callback_data;
	int						__debug_expired;
};

typedef void (*bt_timer_cb_t) (void *);

int bt_is_running(void *vtw, bt_timer_t *timer);
struct timerwheels * bt_init_timers(void);
void bt_run_timers(void *vtb);
void bt_init_timer(bt_timer_t *timer, const char *fname, int bset_run);
void bt_del_timer(void *vtw, bt_timer_t *timer);
void  bt_destroy_timer(struct timerwheels *base, bt_timer_t *timer);
void bt_set_timer(void *vtw, bt_timer_t *timer, unsigned int toff, bt_timer_cb_t cb, void *cbdata);

int bt_del_timer_v2(void *vtw, bt_timer_t *timer);
int  bt_destroy_timer_v2(struct timerwheels *base, bt_timer_t *timer);
int bt_set_timer_v2(void *vtw, bt_timer_t *timer, unsigned int toff, bt_timer_cb_t cb, void *cbdata);

#define	IS_TIMER_RUN(t_)	((t_)->running || bt_timer_pending(t_))


#endif /* __BT_TIMER_T */
