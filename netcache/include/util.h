#ifndef __UTIL_H__
#define __UTIL_H__
#include <memory.h>
#include <hash.h>
#include <64bit_ops.h>
#include <ncapi.h>
#include "rdtsc.h"

#define	REMIND(s_) 	"[1;31m" s_ "[0m"

/* CHG 2018-07-04 huibong Header ÆÄÀÏ »óÈ£ ±³Â÷ include ¼öÁ¤ (#32238) */
/* #include <trace.h> */

#define	IS_ON(_v, _b)		(((_v) & (_b)) != 0)



#define	NC_LOCK_RAW(f, _sg)				ASSERT(pthread_mutex_lock(f) == 0)
#define	NC_UNLOCK_RAW(f, _sg)			pthread_mutex_unlock(f)
#define	NC_RWLOCK_RDLOCK_RAW(f, _sg)	pthread_rwlock_rdlock(f)
#define	NC_RWLOCK_UNLOCK_RAW(f, _sg)	pthread_rwlock_rdunlock(f)
#define	NC_RWLOCK_WRLOCK_RAW(f, _sg)	pthread_rwlock_wrlock(f)
#define NC_LOCK_CLEAN() 				
#define NC_LOCK_REMOVE_INFO(f,_sg)		


#define	AI_ETC					0
#define	AI_DRIVER				1
#define	AI_DRIVERLIB			2
#define	AI_INODE				3
#define	AI_BLOCK				4
#define	AI_ASIO					5
#define	AI_VOLUME				6
#define	AI_OPENCTX				7
#define	AI_IOCTX				8
#define	AI_MDB					9

#define AI_COUNT				10

#define	AI_ALL					0xFFFF

#ifndef max
#define	max(a,b)	((a>b)?a:b)
#endif

#ifndef min
#define	min(a,b)	((a>b)?b:a)
#endif

#define	SNPRINTF_OVERFLOW(len_, ret_) 	((ret_ < 0) || (ret_ >= len_))
#define	SNPRINTF_WRAP(buf, len) {\
			int 		off, rl; \
			off 	= max(0, len - 4); \
			rl 		= min(len - off -1, 3); \
			rl 		= max(0, rl); \
			strncpy(&buf[off], "...", rl); }




#if 1
#define 	MUTEX_INIT(a, b) 	pthread_mutex_init(a, b)
#define 	MUTEX_DESTROY(a) 	pthread_mutex_destroy(a)
#define 	COND_INIT(a, b) 	pthread_cond_init(a, b)
#define 	COND_DESTROY(a) 	pthread_cond_destroy(a)
#define 	DUMP_RESOURCES
#else
#define 	MUTEX_INIT(a, b) 	nc_pthread_mutex_init(a, b, __FILE__, __LINE__)
#define 	MUTEX_DESTROY(a) 	nc_pthread_mutex_destroy(a, __FILE__, __LINE__)
#define 	COND_INIT(a, b) 	nc_pthread_cond_init(a, b, __FILE__, __LINE__)
#define 	COND_DESTROY(a) 	nc_pthread_cond_destroy(a, __FILE__, __LINE__)
#define 	DUMP_RESOURCES 		nc_dump_resources()
PUBLIC_IF int nc_pthread_mutex_init(pthread_mutex_t * restrict mutex,  const pthread_mutexattr_t  *restrict attr, char *file, int lno);
PUBLIC_IF int nc_pthread_mutex_destroy(pthread_mutex_t *mutex, char *file, int lno);
PUBLIC_IF int nc_pthread_cond_init(pthread_cond_t * restrict cond,  const pthread_condattr_t  *restrict attr, char *file, int lno);
PUBLIC_IF int nc_pthread_cond_destroy(pthread_cond_t *cond, char *file, int lno);
PUBLIC_IF void nc_dump_resources(void);
#endif


PUBLIC_IF CALLSYNTAX nc_uint32_t __nc_get_len(void *p);
PUBLIC_IF CALLSYNTAX nc_uint32_t __nc_get_category(void *p);
PUBLIC_IF CALLSYNTAX void * __nc_malloc(size_t n, int category, const char *f, int l);
PUBLIC_IF CALLSYNTAX void __nc_zero(void *p);
PUBLIC_IF CALLSYNTAX void * __nc_calloc(size_t n, size_t m, int category, const char *f, int l);
PUBLIC_IF CALLSYNTAX void __nc_free(void *p, const char *file, int lno);
PUBLIC_IF CALLSYNTAX void * __nc_realloc(void * old_, size_t reqsiz, int category, const char *f, int l);
PUBLIC_IF CALLSYNTAX char * __nc_strdup(const char *s, int category, const char *f, int l);
PUBLIC_IF CALLSYNTAX char * __nc_strndup(const char *s, size_t n, int category, const char *sf, int sl);
PUBLIC_IF CALLSYNTAX void __nc_dump_heap(nc_uint16_t);
PUBLIC_IF CALLSYNTAX int nc_check_memory(void *memp, size_t sz);


#define	XMALLOC_FL(x, c, _f, _l)			__nc_malloc(x, c, _f, _l)
#define	XREALLOC_FL(x,y, c, _f, _l)			__nc_realloc(x,y, c, _f, _l)

#define	XMALLOC(x, c)						__nc_malloc(x, c, __FILE__, __LINE__)
#define	XCALLOC(x,y, c)						__nc_calloc(x,y, c, __FILE__, __LINE__)
#define	XREALLOC(x,y, c)					__nc_realloc(x,y, c, __FILE__, __LINE__)
#define	XFREE(x)							{ if (x) __nc_free(x, __FILE__, __LINE__);x=NULL ; }
#define	XFREE_CHECK(x,c)					{ if (x) __nc_free(x, __FILE__, __LINE__);x=NULL ; }
#define	XSTRDUP(x, c)						__nc_strdup(x, c, __FILE__, __LINE__)
#define	XSTRDUP_FL(x, c, f_, l_)			__nc_strdup(x, c, f_, l_)
#define	XSTRNDUP(x,_n, c)					__nc_strndup(x,_n, c, __FILE__, __LINE__)



#define	XVERIFY(x_)							DEBUG_ASSERT(nc_check_memory(x_, 0))


#define BUFFSIZE (64*1024)
typedef unsigned long long jiff;

#if !defined(restrict) && __STDC_VERSION__ < 199901
#if __GNUC__ > 2 || __GNUC_MINOR__ >= 92
#define restrict __restrict__
#else
#warning No restrict keyword?
#define restrict
#endif
#endif

#if __GNUC__ > 2 || __GNUC_MINOR__ >= 96
// won't alias anything, and aligned enough for anything
#define MALLOC __attribute__ ((__malloc__))
// no side effect, may read globals
//#define PURE __attribute__ ((__pure__))
// tell gcc what to expect:   if(unlikely(err)) die(err);
#define likely(x)       __builtin_expect(!!(x),1)
#define unlikely(x)     __builtin_expect(!!(x),0)
#define expected(x,y)   __builtin_expect((x),(y))
#else
#define MALLOC
#define PURE
#define likely(x)       (x)
#define unlikely(x)     (x)
#define expected(x,y)   (x)
#endif

typedef struct tag_blk_stream {
	void 	*inode;
	void 	*buffer;  	/* pointer to fc_blk_t */
	off_t 	offset; 	/* offset to read/write at the next time */
	size_t	buffersiz;
} blk_stream_t;
blk_stream_t *bs_open(blk_stream_t *bs, void *inode, fc_blk_t *blk, int blen);
int bs_lseek(blk_stream_t *bs, off_t off);
int bs_read(blk_stream_t *bs, unsigned char *buf, size_t len);
unsigned char bs_char(blk_stream_t *bs);





/*\
 *
 *  tlc_node_job_handler_proc_t callback : if someone dequeus a data from queue, it calls this callback
 *  when returning, its result passes to tlc_node_report_handler_proc_t, callback
 *  if either of two callback is NULL, the callbacking is passed with nothing.
 *  Usage scenerio:
 *		(1) a block to be used next time is cache-miss, and enqueu it to queue 
 *		    with fault-handler & report-handler for read-ahead
 *		(2) a worker thread dequeues the job, and calls node_handler_proc callback
 *		(3) after completion of the job, it calls node_report_handler_proc callback 
 *		    with the return value
 *
\*/
//typedef int (*node_job_handler_proc_t)(void *data);
//typedef int (*node_report_handler_proc_t)(void *data, int result);


#define		LOCK_SNUM(l)				(l)->SNUM

#ifndef NC_RELEASE_BUILD
#define		LO_CHECK_ORDER(sno)				lo_check(sno) 
#define		LO_PUSH_ORDER(sno)				lo_push(sno, __FILE__, __LINE__)
#define		LO_PUSH_ORDER_FL(sno, f, l)		lo_push(sno, f, l)
#define		LO_POP_ORDER(sno)				lo_pop(sno)
#else
#define		LO_CHECK_ORDER(sno)
#define		LO_PUSH_ORDER(sno)
#define		LO_PUSH_ORDER_FL(sno, f, l)		
#define		LO_POP_ORDER(sno)
#endif
#define		LON_NET_MUX				11
#define		LON_VOLUME				10
#define		LON_PATHLOCK			9
#define		LON_INODE				8
#define		LON_CLFU_INODE			3
#define		LON_CLFU_BLOCK			1

typedef struct nc_lock {
#ifndef NC_RELEASE_BUILD
	nc_uint32_t		_magic;
#endif
	pthread_mutex_t	_lock;
	int				_locked; /* increased whenever lock acquisition succeeded */
#ifdef NC_MEASURE_MUTEX
	void			*_ml_stat;
	perf_val_t		_tml;
#endif
} nc_lock_t;

typedef struct nc_cond {
#ifndef NC_RELEASE_BUILD
	nc_uint32_t		_magic;
#else
#endif
	pthread_cond_t	_cond;
} nc_cond_t;


#ifdef NC_MEASURE_MUTEX
#define	NC_LOCK_INITIALIZER {._lock = PTHREAD_MUTEX_INITIALIZER, _locked = 0, _ml_stat = 0}
#else
#define	NC_LOCK_INITIALIZER {._lock = PTHREAD_MUTEX_INITIALIZER, ._locked = 0}
#endif

PUBLIC_IF CALLSYNTAX int _nl_cond_init(nc_cond_t *l);
PUBLIC_IF CALLSYNTAX int _nl_cond_signal(nc_cond_t *l);
PUBLIC_IF CALLSYNTAX int _nl_cond_timedwait(nc_cond_t *cond, nc_lock_t *mutex, const struct timespec *abstime);
PUBLIC_IF CALLSYNTAX int _nl_cond_timedwait2(nc_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime);
PUBLIC_IF CALLSYNTAX int _nl_cond_wait(nc_cond_t *l, nc_lock_t *mutex);
PUBLIC_IF CALLSYNTAX int _nl_cond_broadcast(nc_cond_t *l);
PUBLIC_IF CALLSYNTAX int _nl_cond_destroy(nc_cond_t *l);

PUBLIC_IF CALLSYNTAX int _nl_init(nc_lock_t *l, pthread_mutexattr_t *ma);
PUBLIC_IF CALLSYNTAX int _nl_destroy(nc_lock_t *l);
PUBLIC_IF CALLSYNTAX int _nl_lock(nc_lock_t *L, char *f, int l);
PUBLIC_IF CALLSYNTAX int _nl_unlock(nc_lock_t *l);
PUBLIC_IF CALLSYNTAX int _nl_owned(nc_lock_t *l);
PUBLIC_IF CALLSYNTAX pthread_mutex_t * _nl_get_lock_raw(nc_lock_t *l);

struct tag_link_list;
//typedef int  (*node_lock_proc_t)(void *data, int enforce_lock);
#pragma pack(4)
typedef struct tag_link_list {
	struct tag_link_node	*head;
	struct tag_link_node	*tail;
	int 					count;
	int						ref;
} link_list_t;
#pragma pack()

typedef struct {
	char 	*name;
	double 	vmin, vmax;
	double 	sum;
	long 	count;
} mavg_t;
typedef struct {
#ifdef NC_RWLOCK_USE_SPINLOCK
	pthread_spinlock_t		slock;
#else
	pthread_mutex_t			slock;
#endif
	pthread_rwlock_t	rwlock;
	int						rcnt;
} nc_ulock_t;





mavg_t  * mavg_create(char *name);
void mavg_stat(mavg_t *hm, long *, double *vmin, double *vmax, double *vavg);
void mavg_update(mavg_t *hm, double v);
char * mavg_name(mavg_t *hm);

#define		NODE_MAGIC		0xa55a
#define		LIST_MAGIC		NODE_MAGIC


#define		ACQUIRE_LIST(l_)	ASSERT(_ATOMIC_CAS((l_)->ref, 0, 1))
#define		RELEASE_LIST(l_)	ASSERT(_ATOMIC_CAS((l_)->ref, 1, 0))
#define 	LIST_INITIALIZER	{NULL, NULL, 0, 0}
#define 	INIT_LIST(x)		{(x)->ref = 0; (x)->count = 0; (x)->head = (x)->tail = NULL;}


#define 	INIT_NODE(x)	{(x)->next = (x)->prev = NULL; (x)->u.ns.inlist = 0;}
#define 	RESET_NODE(x)	
#define 	CHECK_NODE(x)	
#define		SET_NODE(_x, _d) {(_x)->data = _d; (_x)->next = (_x)->prev = NULL; }

#ifdef NC_DEBUG_LIST
  	#define		ASSERT_LIST(l_, e_) { \
										if (!(e_)) { \
											TRACE((T_ERROR, "List assertion error on Expr '%s' at %d@%s\n" \
															"LIST: ref(%d),count(%d), H[%p],T[%p]\n", \
															#e_, __LINE__, __FILE__, (l_)->ref, \
															(l_)->count, (l_)->head, (l_)->tail)); \
											TRAP;\
										} \
									}




	#define 	VALID_LIST(l_) ( \
									(  \
										((l_)->head == NULL && (l_)->tail == NULL) ||  \
										((l_)->head != NULL && (l_)->tail != NULL) \
									) && \
									((l_)->count >= 0) \
								)
	#define 	CHECK_LIST(l_) 		ASSERT_LIST(l_, VALID_LIST(l_))

#else

	#define 	CHECK_LIST(l_) 

#endif



void nc_sub_dm(nc_int32_t asz, nc_int32_t sz);
void nc_add_dm(nc_int32_t asz, nc_int32_t sz);
int link_move_head(link_list_t *list, link_node_t *m);
void link_add_n_sort(link_list_t *list, void *data, link_node_t *m, int (*_comp)(void *a, void *b));
PUBLIC_IF CALLSYNTAX int link_prepend(link_list_t *list, void *data, link_node_t *m);
PUBLIC_IF CALLSYNTAX int link_add_in_order(link_list_t *list, long long (*comp)(void *, void *), void *data, link_node_t *m);
int link_append(link_list_t *list, void *data, link_node_t *m);
int link_foreach(link_list_t *list, int (*proc)(void *, void *), void *);
int link_del(link_list_t *list, link_node_t *m);
void link_del_if_possible(link_list_t *list, link_node_t *m);
void * link_get_head(link_list_t *list);
int link_prove(link_list_t *list, int);
PUBLIC_IF CALLSYNTAX void * link_get_head_noremove(link_list_t *list);
PUBLIC_IF void link_add_sort(link_list_t *list, void *data, link_node_t *m, long long (*_comp)(void *a, void *b));
PUBLIC_IF CALLSYNTAX link_node_t *link_get_head_node(link_list_t *list);
PUBLIC_IF CALLSYNTAX void * link_get_node_prev(link_list_t *list, link_node_t *node);

void * link_get_tail(link_list_t *list);
void * link_get_tail_noremove(link_list_t *list);
int link_join(link_list_t *a, link_list_t *b);
link_node_t * link_put_tail(link_list_t *list, void *data);
int link_empty(link_list_t *list);
PUBLIC_IF CALLSYNTAX void * link_get_next(link_list_t *list, link_node_t *node);
PUBLIC_IF CALLSYNTAX void * link_get_prev(link_list_t *list, link_node_t *node);
long long sys_gettickcount(void);
void link_add_after(link_list_t *list, link_node_t *prev, void *data, link_node_t *m);
int link_count(link_list_t *list, int forward);
void link_verify(link_list_t *list);
int link_contains(link_list_t *list, link_node_t *m);
PUBLIC_IF CALLSYNTAX int link_node_valid(link_node_t *m);
char * tolowerz(char *istr);


int LM_pthread_mutex_lock(pthread_mutex_t *lock, char *func, int lno, char *signature);
int LM_pthread_mutex_unlock(pthread_mutex_t *lock, char *func, int lno, char *signature);
int LM_pthread_rwlock_rdlock(pthread_rwlock_t *lock, char *func, int lno, char *signature);
int LM_pthread_rwlock_rdunlock(pthread_rwlock_t *lock, char *func, int lno, char *signature);
int LM_pthread_rwlock_wrlock(pthread_rwlock_t *lock, char *func, int lno, char *signature);
//int LM_pthread_rwlock_rdunlock(pthread_rwlock_t *lock, char *func, int lno, char *signature);
int LM_pthread_mutex_trylock(pthread_mutex_t *lock, char *func, int lno, char *signature);
int LM_pthread_check_all_unlocked(void);
int LM_pthread_lock_remove(pthread_mutex_t *lock, char *func, int lno, char *signature);
int LM_pthread_rw_unlock(pthread_rwlock_t *lock, char *func, int lno, char *signature);
int LM_pthread_rw_lock(pthread_rwlock_t *lock, int exclusive, char *func, int lno, char *signature);

void bt_usleep(long timeout);
void bt_msleep(long timeout);
#ifndef HAVE_LOCALTIME_R 
#include <pthread.h>
#undef localtime_r
struct tm *localtime_r(const time_t *timep, struct tm *result);
#endif

unsigned long path_hash(void *data);
u_boolean_t path_equal(void *a, void *b);
#define		STATT_ABS	0
#define		STATT_REL	1
int stats_register(char *name, float scale, int type);
long long stats_inc(int sid, long val);
int stats_init(char *filename);
int stats_dump(int interval);

void * hm_begin(char *, int duetime_msec);
void hm_end(void * hm_id);
int hm_start_monitor(void);
int report_error_syslog(char *name, char *fmt, ...);

pid_t gettid(void);
int pthread_is_same(pthread_t t1, pthread_t t2);

/* CHG 2018-04-11 huibong »èÁ¦ Ã³¸® (#32013) */
/* char * nc_lasterror(char *buf, int len) ; */

char *  nc_strerror(int, char *buf, int len) ;
char make_crc(char *buf, int len);

char * filetime(char *buf, int len, time_t t);
void * nc_aligned_malloc(nc_size_t size, int alignment, int category, const char *f, int l);
void * nc_aligned_realloc(void *p, nc_size_t old_size, nc_size_t size, int alignment, int category, const char *f, int l);
void nc_aligned_free(void *p);
char * ll2dp(char *buf, int buf_len, long long n);
char * time2dp(char *buf, int buflen, nc_time_t secs);
char * hz_bytes2string(char *buf, int len, nc_uint64_t val);

char  * nc_time2string(time_t *t, char *buf,int len);
time_t nc_cached_clock();
void nc_update_cached_clock();
PUBLIC_IF CALLSYNTAX nc_int32_t nc_block_size();
void dump_bits(unsigned long *bitmap, int bitlen, char *buf, int buflen, int *vbits);
void block_signal_2(int sigblock);
void block_signal();

void ai_add(int category, nc_int32_t sz);
void ai_sub(int category, nc_int32_t sz);
char * ai_dump(char *abuf, int len);

void crc_init();
nc_uint8_t do_crc(char *d, int len);
nc_uint32_t get_random();
char * uuid2string(uuid_t uuid, char *buf, int l);
int nr_cpu();
char * trim_string(char *str);
void make_timeout( struct timespec *to, long msec, clockid_t);
char * vstring_pack(char *ptr, int capa, char *str, int len);
char * vstring_unpack(char *ptr, char **str, int *len);
int vstring_size(char *ptr);
int vstring_size2(int len);
int my_pthread_create(pthread_t *restrict thread, const pthread_attr_t *restrict attr, void *(*start_routine)(void*), void *restrict arg);
void condition_init(pthread_cond_t *cond);
int condition_wait(pthread_mutex_t *, pthread_cond_t *cond, long msec, int (*completed)(void *), void *cb, int forever);
void clrtid();
int linux_kernel_version(int *M, int *m, int *p);

apc_overlapped_t * apc_overlapped_new(void *u, apc_completion_proc_t proc);
void apc_overlapped_reset(apc_overlapped_t *e);
void apc_overlapped_switch(apc_overlapped_t *e, void *u, apc_completion_proc_t proc);
void apc_overlapped_signal(apc_overlapped_t *e);
int apc_overlapped_wait(apc_overlapped_t *e);
void apc_overlapped_destroy(apc_overlapped_t *e);
char * apc_overlapped_dump(apc_overlapped_t *e, char *buf, int len);

char * mode_dump(char *mbuf, int len, nc_mode_t mode);
char * obi_dump(char *buf, int len, nc_obitinfo_t *pobi);
char * stat_dump(char *buf, int len, nc_stat_t *stat);

int get_memory_usage();
char * get_parent(char *parent, int sz, char *str);
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size);


#define	HM_SUICIDE_DISKIO		0
#define	HM_SUICIDE_RA			1
#define	HM_SUICIDE_CACHE		2
#define	HM_SUICIDE_TIMER		3


#ifdef NC_RELEASE_BUILD
#define		DEBUG_TRAP		
#else
#define		DEBUG_TRAP		TRACE((T_ERROR, "TRAP! in %s\n", __func__)); {char *trap= NULL; *(trap) = '\0';}
#endif

#ifndef TRAP
#define		TRAP		TRACE((T_ERROR, "TRAP! in %s\n", __func__)); {char *trap= NULL; *(trap) = '\0';}
#endif
#define		GET_ATOMIC_VAL(i)	__sync_add_and_fetch(&(i), 0)

pthread_rwlock_t * pthread_rwlock_wrapper_alloc(void);
void pthread_rwlock_wrapper_free(pthread_rwlock_t *rwlock);
void pthread_rwlock_wrapper_upgrade(pthread_rwlock_t *rwlock);

int nc_ulock_init(nc_ulock_t *ul);
int nc_ulock_rdlock(nc_ulock_t *ul);
int nc_ulock_wrlock(nc_ulock_t *ul);
int nc_ulock_upgrade(nc_ulock_t *ul);
int nc_ulock_unlock(nc_ulock_t *ul);
int nc_ulock_xowned(nc_ulock_t *ul);
void nc_ulock_destroy(nc_ulock_t *ul);

void __nc_dump_heap(nc_uint16_t class);
char *unitdump(char *buf, int buflen, long num);
void nc_dump_stack(int sig);
int lo_check(int sno_to_lock);
void lo_push(int sno_to_lock, char *f, int l);
int lo_pop();


#endif /* __UTIL_H__ */
