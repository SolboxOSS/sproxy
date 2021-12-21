#ifndef __LB_H__
#define __LB_H__



#include <config.h>
#include <netcache_config.h>
#include <netcache.h>
#include <util.h>

#define	 LB_MAX_POOLS  			100
#define	 LB_INVALID_HANDLE		(nc_uint64_t)(-1)

#if 1
#define		LB_REF(l_)				{_ATOMIC_ADD((l_)->refcnt,1); (l_)->last_utime = nc_cached_clock(); DEBUG_ASSERT((l_)->refcnt > 0); }
#define		LB_UNREF(l_)			{_ATOMIC_SUB((l_)->refcnt,1); (l_)->last_utime = nc_cached_clock(); DEBUG_ASSERT((l_)->refcnt >= 0); }
#define		LB_BUSY(l_)				(GET_ATOMIC_VAL((l_)->refcnt) != 0 || (l_)->timer_set != 0 || (l_)->timer_run != 0)

/*
 * 현재 사용중이 아니거나 또는
 * 사중중으로 표시되지만, 실제 LB 최종사용시간이 300초 이상지났다면 IDLE이라고 본다
 */
#define		LB_IDLE(l_)				(!LB_BUSY(l_) || (LB_BUSY(l_) && ((nc_cached_clock() - (l_)->last_utime) > 300)))
#define		LB_COUNT(l_)			GET_ATOMIC_VAL((l_)->refcnt)
#define		LB_VALID(l_)			((l_)->magic == 0x5aa5a55a)
#else
#define		LB_REF(l_)				TRACE((T_WARN, "[%s].%s : %d at %d@%s\n", (l_)->assoc_driver->signature, __func__, _ATOMIC_ADD((l_)->refcnt,1), __LINE__, __FILE__))
#define		LB_UNREF(l_)			TRACE((T_WARN, "[%s].%s : %d at %d@%s\n", (l_)->assoc_driver->signature, __func__, _ATOMIC_SUB((l_)->refcnt,1), __LINE__, __FILE__))
#define		LB_IDLE(l_)				(GET_ATOMIC_VAL((l_)->refcnt) == 0)
#endif

#define	LB_CHECK_CORRUPTION(_p)	DEBUG_ASSERT(((_p)->fmagic ^ (_p)->rmagic) == 0xFFFFFFFF)
#if 0
#define	LB_TRANSACTION(LBP, zz)	TRACE((T_DEBUG, "LB Transaction at %d@%s\n", __LINE__, __func__));   \
							for (zz =1, _nl_lock(LBP->lock, __FILE__, __LINE__); zz; zz = 0, _nl_unlock(LBP->lock))
#define	LB_UNLOCK(lb_)		_nl_unlock((lb_)->lock)
#define	LB_LOCK(lb_)		_nl_lock((lb_)->lock)
#else
#define	LB_TRANSACTION(LBP, zz)	
#define	LB_UNLOCK(lb_)		
#define	LB_LOCK(lb_)	
#endif


typedef long long 	nc_pool_handle_t;
struct tag_lb; 
struct tag_lb_pool; 

typedef struct {
	char 			*uobject_id;
	void 			*uobject;
	struct tag_lb 	*lb_root;
	struct tag_lb_pool 	*lb_pool;
	bt_timer_t		t_expire;
	link_node_t 	node;
} lb_object_t;

typedef struct tag_lb_pool {
	nc_uint32_t		fmagic;
	char 			*id;
	struct tag_lb 	*lb_root;
	nc_int32_t 		pri_idx;
	nc_int32_t		priority;	/* 숫자가 작을수록 우선순위 높음 */
	link_list_t 	olist;
	nc_uint32_t		online;		// 1:online , 그 외는 offline 상태

	////////////////////////////////////////
	// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
	// - origin 에서 연속 N 회 오류 발생시 offline 으로 전환한다.
	// - 또한 특정 시간이 지나면 fail count 값을 초기화 한다.

	int				request_fail_count;			// 오류 발생 횟수
	struct timespec last_fail_time;				// 마지막 오류 발생 시간.

	///////////////////////////////////////
	
	nc_int32_t		probe_time;	/* next probing time */
	nc_int32_t		proberemained;
	nc_uint32_t 	ioflags;	/* read, write, etc */
	void 			*userdata;
	link_node_t 	node;
	nc_uint32_t		rmagic;
	nc_time_t		t_chgtime;	/* != 0: 온/오프라인이 된 시점 */

} lb_pool_t;

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - consistent hash ring 의 각 node 정보
typedef struct tag_lb_consistent_ring_node {

	unsigned int value;				// consistent hash ring 상에서의 위치 값 (md5 hash 로 생성된 값)
	int arraryindex_pool_by_id;		// pool_by_id[] 에 등록된 lb_pool_t 객체를 나타내는 array index

} lb_consistent_ring_node_t;

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - consistent hash ring 정보 
typedef struct tag_lb_consistent_ring {

	int server_count;							// consistent hash ring 에 등록된 server count;	
	int node_count;								// consistent hash ring 에 등록된 전체 node count
	unsigned int min_value;						// consistent hash ring 에 등록된 node 의 value 값 중 최소값
	unsigned int max_value;						// consistent hash ring 에 등록된 node 의 value 값 중 최대값

	lb_consistent_ring_node_t * node_arrary;	// consistent hash ring node array 

} lb_consistent_ring_t;

typedef struct tag_lb {
	nc_uint32_t 		magic;
	nc_uint32_t 		conf_version;
	int					refcnt;
	lb_pool_t 			*pool_by_id[LB_MAX_POOLS]; 		/* pool의 id기반 sort, consistent hash ring 에서 사용 */
	lb_pool_t 			*pool_by_pri[LB_MAX_POOLS]; 	/* pool의 priority기반 sort */

	// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
	// - consistent hash ring 객체 추가.
	lb_consistent_ring_t consistent_hash_ring;

	// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
	// - 각 origin 또는 parent 에서  연속 N 회 오류 발생시 offline 으로 전환
	// - 이와 관련 각 origin, parent 가 offline 상태 전환될 연속 오류 횟수
	int					request_fail_count_to_offline;		// conf 에 설정된 offline 전환 관련 연속 오류 횟수

	void 				*onoff_cbdata;
	cfs_origin_driver_t *assoc_driver;	/*2020-04-30 */
	nc_int32_t 			shutdown;
	nc_int32_t 			pool_count;
	nc_int32_t 			online_pool_count;
	nc_int32_t			probe_retry_count;
	nc_int32_t			probe_interval;
	nc_int32_t			expiry_time;
	bt_timer_t			t_recovery_timer;
	nc_int32_t			timer_set;
	nc_int32_t			timer_run;
	nc_time_t			last_utime;
	union {
		struct {
			nc_uint32_t 	pversion;
			nc_uint32_t 	pindex;
		} ucursor_s;
		nc_uint64_t 		aindex;
	} u;
#define lb_cursor 		u.ucursor_s.pversion
#define lb_pindex 		u.ucursor_s.pindex
#define lb_index 		u.aindex

	nc_int32_t 			cursor_idx; /* pool_by_pri의 pool index를 가르킨다.*/
	nc_int32_t 			(*probe_online)(struct tag_lb *lb, lb_pool_t *pool, void *ud);
	nc_pool_handle_t 	(*elect_pool)(struct tag_lb *lb, const void *hint);
	void 				(*expire_object)(void *object);
	void 				(*free_object)(void *object);
	void				(*onoff_monitor)(struct tag_lb *lb, char *id, void *cbdata, int online);
	nc_lock_t 			*lock; /* set by caller */
	link_node_t			node;
} lb_t;
typedef 	nc_pool_handle_t 	(*nc_policy_type_t)(lb_t *, const void *);
#define		LBP_POLICY_RR 		(nc_policy_type_t)1
#define		LBP_POLICY_PS 		(nc_policy_type_t)2
#define		LBP_POLICY_HASH 	(nc_policy_type_t)3


#define			LB_GET(pool_)	(pool_)->lb_root

lb_t * 			lb_create(cfs_origin_driver_t *drv, nc_lock_t *l, void (*expire_object)(void *object), void (*free_object)(void *object), nc_int32_t (*probe_online)(lb_t *lb, lb_pool_t*pool_id, void *ud));
void 			lb_destroy(lb_t *);
int 			lb_set_policy(lb_t *lb, nc_pool_handle_t (*elect_pool)(lb_t *, const void *));
lb_pool_t * 	lb_create_pool(const char *id, void *usedata);
int 			lb_pool_set_online(lb_t *lb, lb_pool_t *pool, int onoff, int need_recover, int force);
int 			lb_add_pool(lb_t *lb, lb_pool_t *pool, int priority);

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - LB에 origin 등록 완료 후 필요한 후처리 작업을 진행할 함수 
int				lb_add_pool_complete( lb_t * lb );

lb_pool_t * 	lb_del_pool(lb_t *lb, const char *id);
int 			lb_set_pool_ioflag(lb_t *lb, lb_pool_t *pool, nc_uint32_t flags);
int 			lb_push_object(lb_t *lb, nc_pool_handle_t poolinfo, void *object, const char *object_id);
void * 			lb_pop_object(lb_t *lb, nc_pool_handle_t poolinfo, const char *object_id);
lb_pool_t * 	lb_pool_by_index(lb_t *lb, nc_uint32_t pi);
nc_pool_handle_t lb_get_policy_pool(lb_t *lb, const void *hint);
void 			lb_print(lb_t *lb);
int 			lb_check_version(lb_t *lb, nc_pool_handle_t );
int 			lb_pool_count_online(lb_t *lb);
nc_uint32_t 	lb_get_pindex(nc_uint64_t cv);
void * 			lb_pool_userdata(lb_t *lb, nc_pool_handle_t ph);
int 			lb_check_online(lb_t *lb, nc_uint64_t phandle);
void 			lb_set_monitor(lb_t *lb, void (*monitor_proc)(lb_t *, char *id, void *cbd, int online), void *cbdata  );
void			lb_signal_shutdown(lb_t *lb);
void lb_probe_pools(void *ud);
int lb_shutdown_signalled(lb_t *lb);
int lb_healthy_nolock(lb_t *lb);
#endif
