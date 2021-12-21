/*
 *
 * built-in load-balancing
 * 	consistent-hash 
 * 	round-robin
 *  primary-standby
 *
 * 호출 시나리오
 *
 *	
 * 	main 로직
 * 		1. pool = lb_get_pool()  	; 내부에서 기 설정된 정책에 따라 pool election
 * 		2. session = lb_pop_object(pool host); 선택된 pool에서서 host name을 가진 객체(session)선택
 * 		3. if (session == NULL) session = new_session(host)
 * 		4. session을 이용한 작업 실행
 * 		5. lb_push_object(pool, session, host)
 */
#ifndef WIN32
#include <error.h>
#endif
#include <fcntl.h>
#include <iconv.h>
#ifndef WIN32
#include <langinfo.h>
#endif
#ifndef WIN32
#include <libintl.h>
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>
#include <ctype.h>


#include <math.h>	// floorf() 사용 관련

#include <md5.h>	// consistent hash ring 관련 추가

#include "lb.h"

extern int 			__terminating;



static nc_pool_handle_t  lb_policy_rr(lb_t *lb, const void *) ;
static nc_pool_handle_t  lb_policy_hash(lb_t *lb, const void *) ;
static nc_pool_handle_t  lb_policy_ps(lb_t *lb, const void *) ; 
static lb_pool_t *  lb_find_pool_byid_nolock(lb_t *lb, char *id);
static int 			lb_is_online_nolock(lb_pool_t *pool);
static void 		lb_free_object_nolock(lb_object_t *lo, int);
static lb_object_t *lb_find_object_nolock(lb_pool_t *pool, const char *object_id);
static void 		lb_remove_object_nolock(lb_pool_t *pool, lb_object_t *lo);
static nc_uint64_t 	lb_make_index(nc_uint32_t conf, nc_uint32_t eind);
lb_pool_t * 		lb_pool_by_index(lb_t *lb, nc_uint32_t pi);
static void 		lb_rebuild_order(lb_t *lb);
static int 			lb_destroy_pool(lb_pool_t *pool);

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - 외부 호출 불필요한 함수는 static 로 선언 처리함.

static int lb_compare_consistent_hash_node( const void * a, const void * b );
static int lb_create_consistent_hash_ring( lb_t * lb );
static int lb_get_consistent_hash_index( lb_t *lb, unsigned int hash_value );

// ---------------------------

#define	LB_MAGIC 	0x5AA5A55A

extern void *__timer_wheel_base;


static pthread_mutex_t	__lb_global_lock	= PTHREAD_MUTEX_INITIALIZER;



// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
// - default 설정값 
#define DEFAULT_REQUEST_FAIL_COUNT_TO_OFFLINE		3	


// ---------------------------

lb_t * 
lb_create(cfs_origin_driver_t *drv, nc_lock_t *l, void (*expire_object)(void *object), void (*free_object)(void *object), nc_int32_t (*probe_online)(lb_t *lb, lb_pool_t*pool_id, void *ud))
{
	lb_t  					*lb = NULL;


	lb = (lb_t *)XMALLOC(sizeof(lb_t), AI_DRIVER);

	lb->magic 			= 0x5aa5a55a;
	lb->assoc_driver 	= drv;
	lb->cursor_idx 	 	= -1;
	lb->elect_pool 	 	= lb_policy_rr;	
	lb->probe_online 	= probe_online;
	lb->expire_object	= expire_object;
	lb->free_object 	= free_object;
	lb->expiry_time 	= 60*1000;
	lb->conf_version 	= 0;
	lb->lb_cursor 		= 0;
	lb->shutdown 		= 0;
	lb->last_utime 		= nc_cached_clock();

	// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
	// - consistent hash ring 객체 초기화.
	lb->consistent_hash_ring.server_count	= 0;
	lb->consistent_hash_ring.node_count		= 0;
	lb->consistent_hash_ring.min_value		= 0;
	lb->consistent_hash_ring.max_value		= 0;
	lb->consistent_hash_ring.node_arrary	= NULL;

	// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
	// - 각 origin 별 offline 전환 관련 오류 횟수값 default 값 설정 처리
	lb->request_fail_count_to_offline = DEFAULT_REQUEST_FAIL_COUNT_TO_OFFLINE;



	TRACE((0, "DRIVER[%s].LB[%p] - container created\n", drv->signature, lb));
	return lb;
}

int
lb_shutdown_signalled(lb_t *lb)
{
	return lb->shutdown != 0;
}
void
lb_signal_shutdown(lb_t *lb)
{
	int		trz;

	DEBUG_ASSERT(nc_check_memory(lb, 0));
	LB_TRANSACTION(lb, trz) {
		if (lb->shutdown == 0) {
			lb->shutdown = 1;
			TRACE((T_PLUGIN, "DRIVER[%s]/ref[%d]: LB[%p]/Ref[%d] shutdown marked\n", lb->assoc_driver->signature, lb->assoc_driver->refcnt, lb, lb->refcnt));
		}
	}
}
/*
 * cfs-lock 후 호출됨
 */
void
lb_destroy(lb_t *lb)
{
	int 		trz, i;


	TRACE((T_PLUGIN, "DRIVER[%s]/ref[%d] - destroying LB[%p]\n", lb->assoc_driver->signature, lb->assoc_driver->refcnt, lb));
L_retry:
	LB_TRANSACTION(lb, trz) {
		DEBUG_ASSERT(lb->magic == 0x5aa5a55a);
		DEBUG_ASSERT(lb->refcnt == 0);
		lb_signal_shutdown(lb);
		lb->magic 	= 0;
		for (i = 0;i < lb->pool_count; i++) {
			if (lb->pool_by_pri[i]) { 
				TRACE((T_PLUGIN, "DRIVER[%s]/ref[%d].LB[%p]-pool[%s] destroying\n", lb->assoc_driver->signature, lb->assoc_driver->refcnt, lb, lb->pool_by_pri[i]->id));
				lb_destroy_pool(lb->pool_by_pri[i]) ;
				lb->pool_by_pri[i] = NULL;
			}
		}
	}
	TRACE((T_PLUGIN, "DRIVER[%s]/ref[%d] - destroying LB[%p] DONE*********************\n", lb->assoc_driver->signature, lb->assoc_driver->refcnt, lb));

	// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
	// - consistent hash ring 객체에서 사용한 메모리 해제 처리
	if( lb->consistent_hash_ring.node_arrary != NULL )
	{
		XFREE( lb->consistent_hash_ring.node_arrary );
		lb->consistent_hash_ring.node_arrary = NULL;
	}

	lb->assoc_driver = NULL;
	DEBUG_ASSERT(nc_check_memory(lb, 0));
	XFREE(lb);
}

int
lb_set_policy(lb_t *lb, nc_policy_type_t pt)
{
	int 	r = 0;
	int 	trz;
	char	*pn = "null";

	LB_TRANSACTION(lb, trz) {
		switch ((nc_uint64_t)pt) {
			case 0:
				lb->elect_pool = lb_policy_rr;
				pn = "policy-rr(default)";
				break;

			case (long)LBP_POLICY_RR:
				lb->elect_pool = lb_policy_rr;
				pn = "policy-rr";
				break;

			case (long)LBP_POLICY_PS:
				lb->elect_pool = lb_policy_ps;
				pn = "policy-ps";
				break;

			case (long)LBP_POLICY_HASH:
				lb->elect_pool = lb_policy_hash;
				pn = "policy-hash";

				// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
				// - LB 정책이 hash 인 경우 consistent hash ring 생성 시도 
				// - 실제로 solproxy 에서 server 등록 후 policy 정책을 설정하므로.. 본 코드에서 consistent hash ring 이 생성됨.
				lb_create_consistent_hash_ring( lb );

				break;

			default:
				lb->elect_pool = pt;
				break;
		}
	}

	TRACE((T_PLUGIN, "LB[%p] - LB policy set to '%s'\n", lb, pn));
	return r;
}

lb_pool_t *
lb_create_pool(const char *id, void *userdata)
{
	lb_pool_t 	*pool;

	pool = (lb_pool_t *)XMALLOC(sizeof(lb_pool_t), AI_DRIVER);
	DEBUG_ASSERT(pool != NULL);
	pool->id 	= XSTRDUP(id, AI_DRIVER);
	INIT_LIST(&pool->olist);
	INIT_NODE(&pool->node);
	pool->online 	= 0;
	pool->priority 	= 0;
	pool->ioflags 	= 0;
	pool->userdata 	= userdata;
	pool->probe_time= 0;
	pool->fmagic 	= LB_MAGIC;
	pool->rmagic 	= ~pool->fmagic;

	// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
	// - origin 관련 추가된 변수 초기화 
	pool->request_fail_count = 0;
	memset( &( pool->last_fail_time ), 0, sizeof( struct timespec ) );

	TRACE((T_DEBUG, "LB - pool instance (id='%s', user=%p) created\n", id, userdata));
	return pool;
}

static int
lb_destroy_pool(lb_pool_t *pool)
{
	lb_object_t 	*o;
	void 			*uobj = NULL;
	o = (lb_object_t *)link_get_head(&pool->olist);
	while (o) {
		/*
		 * lb_object를 free하기 전에 user-object를 임시 보관
		 */
		uobj = o->uobject;

		/*
		 * lb_object를 free
		 */
		lb_free_object_nolock(o, (pool->lb_root->expire_object!=NULL));

		/*
		 * user-object의 삭제 요청
		 */
		(*pool->lb_root->expire_object)(uobj);

		o = (lb_object_t *)link_get_head(&pool->olist);
	}
	XFREE(pool->id);
	XFREE(pool);
	return 0;

}
static int
lb_probe_pool(lb_t *lb, lb_pool_t *pool)
{
	int r = 0;
	if (lb->probe_online) {
		r = (*lb->probe_online)(lb, pool, pool->userdata);
	}
	else {
		TRACE((T_ERROR, "POOL[%s] - probe_online() callback not defined\n", pool->id));
	}
	return r;
}
int
lb_healthy_nolock(lb_t *lb)
{
	return (lb->pool_count - lb->online_pool_count) == 0;
}
/*
 * 
 * lock을 획득한 상태에서 호출함
 */
void
lb_probe_pools(void *ud)
{
	lb_t 	*lb = (lb_t *)ud;
	int 	i;
	int 	remained_down = 0;
	int		pr = 0;
	lb_pool_t		*pool = NULL;


	DEBUG_ASSERT(nc_check_memory(lb, 0));


L_retry_probe:
	DEBUG_ASSERT(LB_COUNT(lb) > 0);

	remained_down = lb->pool_count - lb->online_pool_count;
	if (remained_down > 0) {
		for (i = 0; i < lb->pool_count; i++) {
			pool = lb->pool_by_pri[i];
			if (pool->online) continue;
			if (pool->probe_time == 0 || (pool->probe_time > nc_cached_clock())) continue;


			pr = lb_probe_pool(lb, pool);


			if (pr) {
				pool->proberemained--;
				TRACE((T_PLUGIN|T_DEBUG,"DRIVER[%s].LB - pool instance (id='%s') probing ok(remained=%d)\n", 
										lb->assoc_driver->signature,
										pool->id, 
										pool->proberemained));

				if ( pool->proberemained == 0) {
					TRACE((T_INFO|T_PLUGIN, "DRIVER[%s].LB - pool instance (id='%s') finally recovered\n", 
											lb->assoc_driver->signature,
											pool->id));
					lb_pool_set_online(lb, pool, U_TRUE, U_FALSE, U_FALSE);/* 2015.4.28 set_online()내에서 pool_by_pri array re-ordering발생*/
					goto L_retry_probe;
				}
				pool->probe_time = nc_cached_clock();
			}
			else {
				nc_time_t	lastdur = 0;
				char		tbuf[128];

				if (pool->t_chgtime > 0)
					lastdur = nc_cached_clock() - pool->t_chgtime ;

				TRACE((T_INFO, "DRIVER[%s].LB - pool instance (id='%s') still down(last for %s)\n", 
								lb->assoc_driver->signature,
								pool->id, 
								time2dp(tbuf, sizeof(tbuf), lastdur)
					  ));
								
				pool->probe_time = nc_cached_clock() + 5; /* next 5 sec later */
			}
		}
	}

}
/*
 * return:
 * 		 1 : pool status changed 
 *		 0 : pool status left unchanged
 */
static int
lb_pool_set_online_nolock(lb_t *lb, lb_pool_t *pool, int onoff, int needrecover, int force)
{
	int 		tf;
	int 		oc = 0;
	int			changed = 0;
	long		lastdur = 0;

	DEBUG_ASSERT(nc_check_memory(lb, 0));
	tf = (onoff?1:0);
	if (!force && (tf == 0) && (lb->online_pool_count == 1)) 
	{
		/*
		 * 상태 변경 OFFLINE 처리 요청이지만 online상태인 pool이 하나밖에 없으므로
		 * bypass하도록한다.
		 */
		TRACE((T_PLUGIN, "DRIVER[%s].LB[%p] - not permitted to swtich to OFFLINE cause online_pool_count=1\n", lb->assoc_driver->signature, lb));
	}
	else if ((tf ^ pool->online) != 0) 
	{

		// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
		// - origin 에서 연속 N 회 오류 발생시 offline 으로 전환한다.
		// - 또한 특정 시간이 지나면 fail count 값을 초기화 한다.
		if( tf )
		{
			// offline -> online 상태로 변경 요청인 경우..
			// - 오류 발생 count 값 초기화.
			pool->request_fail_count = 0;
		}
		else
		{
			// online -> offline 상태로 변경 요청인 경우....

			// 이전에 fail 이 발생한 적이 있는 경우...
			if( pool->request_fail_count )
			{
				struct timespec now_time;
				clock_gettime( CLOCK_MONOTONIC, &now_time );

				// 이전에 offline 발생한 시간에서 60 sec 이상(유효시간) 된 경우.. 이전에 발생한 오류는 무시
				// - 최규철 과장과 협의하여 시간 설정했으며... 하드코딩 함.
				if( now_time.tv_sec - pool->last_fail_time.tv_sec >= 60 )
				{
					pool->request_fail_count = 0;
				}
			}

			// 오류 발생 횟수를 1 증가, 시간 정보를 설정
			pool->request_fail_count++;
			clock_gettime( CLOCK_MONOTONIC, &( pool->last_fail_time ) );

			// 오류 발생 허용값 미만라면... offline 처리시키지 않는다.
			if( pool->request_fail_count < lb->request_fail_count_to_offline )
			{
				TRACE( ( T_INFO, "DRIVER[%s].pool[%s] not changed offline because an error occurred but within the allowed range.[ %d/%d ]\n"
					, lb->assoc_driver->signature, pool->id, pool->request_fail_count, lb->request_fail_count_to_offline ) );

				//return ( pool->online );
				// 상태가 변경되지 않았으므로.. 0 을 반환 처리한다.
				return 0;	
			}
		}

		/*
		 * 상태 변경 요청
		 */
		pool->online = tf;
		oc = lb->online_pool_count;
		if (tf == 0) {
			lb->online_pool_count--;
			pool->proberemained = 2;
			pool->probe_time = nc_cached_clock(); /* check right now */
		}
		else {
			lb->online_pool_count++;
			lb->probe_retry_count = 0;
		}
		if (pool->t_chgtime > 0)
			lastdur = nc_cached_clock() - pool->t_chgtime ;
		pool->t_chgtime = nc_cached_clock();

		// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
		// - 로그 수정
		// - https://jira.solbox.com/browse/PO-1443 에서 해당 로그가 관제에서 제거되어 수정해도 무방.
		//TRACE((T_PLUGIN|T_INFO, "LB/POOL[%s] - op(%s) results in %s(onlines remained:%d->%d)\n", pool->id, (onoff?"ON":"OFF"), (tf?"ONLINE":"OFFLINE"), oc, lb->online_pool_count));
		TRACE( ( T_INFO , "DRIVER[%s].pool[%s] status changed to %s, fail count[%d/%d]. LB online pool count [%d->%d](state last for %ld secs)\n"
			, lb->assoc_driver->signature, pool->id, ( tf ? "ONLINE" : "OFFLINE" ), pool->request_fail_count, lb->request_fail_count_to_offline
			, oc, lb->online_pool_count
			, lastdur) );


		if (lb->onoff_monitor) {
#if 0
			/*
			 * 2020.1.13 by weon
			 * 여기서 onoff_monitor callback을 부르면 
			 * lb-lock이 걸린 상태로 driver instance module이 호출됨
			 * instance module에서 lock을 시도하는 경우
			 * dead-lock발생 가능
			 * onoff monitor의 호출은 LB-lock범위 밖에서 호출하도록 수정
			 */
			(*lb->onoff_monitor)(lb, pool->id, lb->onoff_cbdata, tf);
#endif  
			changed = 1;
		}

		if (lb->elect_pool != lb_policy_ps)
			lb_rebuild_order(lb);

	}
	else
	{ 
		// NEW 2020-01-17 huibong origin offline 상태변경 민감도 조정 기능 (#32839)
		// - origin 이 정상 상황인 경우.. fail count 값을 초기화 한다
		if( pool->request_fail_count && tf )
		{
			// 이미 online 상태인데... 다시 online 상태로 요청인 경우..
			// - 오류 발생 count 값 초기화.
			pool->request_fail_count = 0;

			TRACE( ( T_INFO, "DRIVER[%s].pool[%s] fail count reset by normal response.[ %d/%d ]\n"
				,lb->assoc_driver->signature, pool->id, pool->request_fail_count, lb->request_fail_count_to_offline ) );
		}
	}

	return changed;
}

int
lb_pool_set_online(lb_t *lb, lb_pool_t *pool, int onoff, int needrecover, int force)
{
	int 	trz;
	int		changed = 0;
	void	(*monitor)(struct tag_lb *lb, char *id, void *cbdata, int online);
	LB_TRANSACTION(lb,trz) {
		changed = lb_pool_set_online_nolock(lb, pool, onoff, needrecover, force);
		monitor = lb->onoff_monitor;
	}
	if (changed && monitor)
		(*monitor)(lb, pool->id, lb->onoff_cbdata, onoff);
	return (pool->online);
}
#if 0
static int
lb_pool_compare_onoff(const void *a, const void *b)
{
	int 	r;
	lb_pool_t 	*pa = *(lb_pool_t **)a;
	lb_pool_t 	*pb = *(lb_pool_t **)b;

	r = pa->online - pb->online;
	return -r;
}
#endif
static int
lb_pool_compare_pri(const void *a, const void *b)
{
 	/*
	 * 선형 증가 형식의 순차화
	 */
	int 	r;
	lb_pool_t 	*pa = *(lb_pool_t **)a;
	lb_pool_t 	*pb = *(lb_pool_t **)b;

	if (pa->online && pb->online)
		r = (pa->priority - pb->priority);
#if 1
	else if (pa->online && !pb->online) 
		r = 999;
	else if (!pa->online && pb->online) 
		r = -999;
#endif
	else 
		r = 0;
	r = -r;
	return r;
}
static int
lb_pool_compare_id(const void *a, const void *b)
{
	lb_pool_t 	*pa = (lb_pool_t *)a;
	lb_pool_t 	*pb = *(lb_pool_t **)b;
#if NOT_USED
	lb_pool_t 	*pb2 = ((lb_pool_t **)b)[0];
	lb_pool_t 	*pb3 = ((lb_pool_t **)b)[0];
	lb_pool_t 	*pb4 = ((lb_pool_t **)b)[0];
	lb_pool_t 	*pb5 = ((lb_pool_t **)b)[0];
#endif
	//lb_pool_t 	*pb6 = ((lb_pool_t **)b)[0];
	//char 		*id;
	//id  = pb6->id;

	return strcasecmp(pa->id, pb->id);
}

// NEW 2019-04-19 huibong Origin, Parent 중복 등록시 check 로직 버그 수정 (#32604)
//                - qsort 에서 사용할 compare 함수 신규 추가
//                - 기존 lb_pool_compare_id() 함수는 인자 문제로.. qsort() 에서 정상 동작 안함.
//                - 이를 해결하기 위해 신규 함수 추가
static int
lb_pool_compare_id_by_qsort( const void *a, const void *b ) 
{
	lb_pool_t   *pa = *(lb_pool_t **)a;
	lb_pool_t   *pb = *(lb_pool_t **)b;

	//TRACE( (T_INFO, "huibong checkpoint4: [0x%X][0x%X] [%s][%s]\n", pa->fmagic, pb->fmagic, pa->id, pb->id) );

	return strcasecmp( pa->id, pb->id );
}

static void
lb_update_pri_index(lb_t *lb)
{
	int 	i;
	for (i = 0; i < lb->pool_count; i++) {
		lb->pool_by_pri[i]->pri_idx = i;
	}
}
static void
lb_rebuild_order(lb_t *lb)
{
	// BUG 2019-04-19 huibong Origin, Parent 중복 등록시 check 로직 버그 수정 (#32604) 
	//                - qsort 에서 사용하던 기존 lb_pool_compare_id() 함수는 인자 문제로.. qsort() 에서 정상 동작 안함.  
	//                - 이를 해결하기 위해 신규 함수 lb_pool_compare_id_by_qsort() 추가 후 변경
	qsort(lb->pool_by_id, 	lb->pool_count, sizeof(lb_pool_t *), lb_pool_compare_id_by_qsort );
	
	qsort(lb->pool_by_pri, 	lb->pool_count, sizeof(lb_pool_t *), lb_pool_compare_pri);

	// CHG 2019-07-19 huibong consistent hash ring 추가에 따른 불필요 기능 제거 (#32409)
	// - 기존 google hash 에서 사용하던 lb_pool_t 객체 불필요하여 제거 처리함.
	//qsort(lb->pool_by_not, 	lb->pool_count, sizeof(lb_pool_t *), lb_pool_compare_onoff);

#if 0
	{
		int 	i;
		for (i = 0; i < lb->online_pool_count; i++) {
			TRACE((T_WARN, "WEON:%s:[%d] %s(%d)\n", __func__, i, lb->pool_by_not[i]->id, lb->pool_by_not[i]->online));
		}
	}

#endif
	lb_update_pri_index(lb);
}
int
lb_add_pool(lb_t *lb, lb_pool_t *pool, int priority)
{
	lb_pool_t 	*found = NULL;
	int 		r = -1;
	int			trz;

	LB_TRANSACTION(lb, trz) {
		found = lb_find_pool_byid_nolock(lb, pool->id);
		if (!found) {
			pool->lb_root 	= lb;
			pool->priority 	= priority;
			lb->conf_version++; /* configuration version 변경 */
			lb->pool_by_id[lb->pool_count] = pool;
			lb->pool_by_pri[lb->pool_count] = pool;

			// CHG 2019-07-19 huibong consistent hash ring 추가에 따른 불필요 기능 제거 (#32409)
			// - 기존 google hash 에서 사용하던 lb_pool_t 객체 불필요하여 제거 처리함.
			//lb->pool_by_not[lb->pool_count] = pool;
			
			lb->pool_count++;
			lb->lb_index = LB_INVALID_HANDLE;
			//fprintf(stderr, "CURSOR = 0x%llx\n", lb->lb_index);
			r = 0;
		}
	}
	return r;
}

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - LB에 origin 등록 완료 후 필요한 후처리 작업을 진행할 함수 
int lb_add_pool_complete( lb_t * lb )
{
	// lb 가 유효하고... 정책 설정값이 hash 인 경우... 
	if( (lb != NULL) && lb->elect_pool == lb_policy_hash )
	{
		// consistent hash ring 생성 처리.
		// - solproxy 는 server 를 등록한 후 정책을 설정하는 순서로 동작, 본 코드는 실제 동작하지 않음.
		// - 또한 reload 시에도 기존 lb 삭제 후 재생성, server 등록, 정책 설정하므로.. 본 코드 동작 안함.
		// - 하지만.. 향후 solproxy 에서 순서 변경이 발생할 가능성 존재하여 코드 추가함.
		lb_create_consistent_hash_ring( lb );
	}

	return 0;
}

lb_pool_t *
lb_del_pool(lb_t *lb, const char *id)
{
	return NULL;
}
int
lb_set_pool_ioflag(lb_t *lb, lb_pool_t *pool, nc_uint32_t flags)
{
	return 0;
}
/*
 * push된 객체 중에서 삭제
 */
static void
lb_expire_object(void *vlo)
{
	lb_object_t 	*lo = (lb_object_t *)vlo;
	void 			*uobj = NULL;
	lb_pool_t		*pool = lo->lb_pool;
	int				trz;

	LB_TRANSACTION(pool->lb_root, trz) {

		LB_CHECK_CORRUPTION(pool);
		lb_remove_object_nolock(pool, lo);
		uobj = lo->uobject;
		lb_free_object_nolock(lo, (pool->lb_root->expire_object!=NULL));
		/*
		 * lo객체는 free된 상태
		 */
		(*pool->lb_root->expire_object)(uobj);
	}
}
/*
 * 해당 pool이 online일 때만 등록
 */
int
lb_push_object(lb_t *lb, nc_pool_handle_t poolinfo, void *object, const char *object_id)
{
	lb_object_t	*lo = (lb_object_t *)XMALLOC(sizeof(lb_object_t), AI_DRIVER);
	int 		res = -1;
	lb_pool_t	*pool = NULL;
	nc_uint32_t	pi;
	int 		trz;

	DEBUG_ASSERT(lb != NULL);
	LB_TRANSACTION(lb, trz) {

		if (lb_check_version(lb, poolinfo)) {
			pi = lb_get_pindex(poolinfo);
			pool = lb->pool_by_pri[pi];
			LB_CHECK_CORRUPTION(pool);
	
			if (lb_is_online_nolock(pool)) {
				lo->uobject = object;
				lo->lb_pool = pool;
				if (object_id)
					lo->uobject_id 	= XSTRDUP(object_id, AI_DRIVER);
				if (lb->expire_object) {
					TRACE((T_DEBUG, "LB - pool (id='%s') object pushed and expiry timer set(%ld)\n", pool->id, lb->expiry_time));
					bt_init_timer(&lo->t_expire, "lb object expire timer", U_FALSE);
					bt_set_timer(__timer_wheel_base,  &lo->t_expire, lb->expiry_time/* 60 sec*/, lb_expire_object, (void *)lo);
				}
				else {
					TRACE((T_DEBUG, "LB - pushing object done without expiry_timer\n"));
				}
				link_append(&pool->olist, (void *)lo,  &lo->node);
				res = 0;
			}
		}
		else {
			TRACE((T_DEBUG, "LB - pushing object failed 'cause versoin mismatch\n"));
		}
	}
	return res;
}
void *
lb_pop_object(lb_t *lb, nc_pool_handle_t poolinfo, const char *object_id)
{
	lb_object_t 	*lo = NULL;
	void 			*object = NULL;
	lb_pool_t 		*pool;
	nc_uint32_t 	pi;
	int 			trz;

	LB_TRANSACTION(lb, trz) {
		if (lb_check_version(lb, poolinfo)) {
			pi = lb_get_pindex(poolinfo);
			DEBUG_ASSERT(pi < lb->pool_count);
			DEBUG_ASSERT(pi >= 0);
			pool = lb->pool_by_pri[pi];
			if (pool && lb_is_online_nolock(pool)) {
				lo = lb_find_object_nolock(pool, object_id);
				LB_CHECK_CORRUPTION(pool);
			}
		
		}

		if (lo) {
			lb_remove_object_nolock(pool, lo);
			object = lo->uobject;
			lb_free_object_nolock(lo, (pool->lb_root->expire_object!=NULL));
		}
	}

	return object;
}
static nc_uint64_t
lb_make_index(nc_uint32_t conf, nc_uint32_t eind)
{
	register nc_uint64_t 	r = conf;
	r =  (r << 32 | (nc_uint64_t)eind);
	return r;
}
int
lb_check_version(lb_t *lb, nc_pool_handle_t v)
{
	return  (v >> 32) == lb->conf_version;
}
nc_uint32_t
lb_get_pindex(nc_uint64_t cv)
{
	return (nc_uint32_t)(cv & 0xFFFFFFFF);
}
int 
lb_pool_count_online(lb_t *lb)
{
	int 	trz;
	int 	oc = 0;
	LB_TRANSACTION(lb, trz) {
		oc = lb->online_pool_count;
	}
	return oc;
}
static nc_uint64_t
lb_find_next_pool_online_nolock(lb_t *lb, nc_uint64_t current)
{
	nc_uint64_t	found = LB_INVALID_HANDLE;
	nc_uint64_t fense = LB_INVALID_HANDLE;
	int 		lc = lb->pool_count;


	if (current == LB_INVALID_HANDLE)
		current = lb->lb_index;

	if (current == LB_INVALID_HANDLE || !lb_check_version(lb, current)) {
		/*
		 * 버전이 틀림
		 */
		current = 0;
	}
	else {
		/*
		 * current 다음 위치부터 검색
		 */
		fense 	= lb_get_pindex(current); 
		current = (fense + 1) % lb->pool_count;
	}
	while (lc > 0) {
		if (lb->pool_by_pri[current]->online) {
			found = lb_make_index(lb->conf_version, current);
			break;
		}
		current = (current + 1) % lb->pool_count;
		lc--;
	}
	return found;
	
}
static lb_pool_t *
lb_find_pool_byid_nolock(lb_t *lb, char *id)
{
	lb_pool_t 	key, *found = NULL;

	key.id = id;
	found = (lb_pool_t *)bsearch(&key, lb->pool_by_id, lb->pool_count, sizeof(lb_pool_t *), lb_pool_compare_id);

	return found;
}
static nc_pool_handle_t
lb_policy_rr(lb_t *lb, const void *hint) 
{
	nc_uint64_t 	handle;
	if (lb->lb_index != LB_INVALID_HANDLE) {
		if (!lb_check_version(lb, lb->lb_index)) {
			/*
			 * 현재 cursor는 config 버전이 바뀌어서 사용할 수 없음.
			 */
			goto L_start_first;
		}
		handle = lb_find_next_pool_online_nolock(lb, lb->lb_index);
	}
	else {
L_start_first:
		handle = lb_find_next_pool_online_nolock(lb, LB_INVALID_HANDLE);
	}

	if (handle != LB_INVALID_HANDLE) {
		lb->lb_index = handle;
	}
	return handle;
}
#if NOT_USED
static nc_int32_t google_consistent_hash(nc_uint64_t key, nc_int32_t num_buckets) 
{
	nc_int64_t b =-1, j = 0;
	if (num_buckets > 1) {
		while (j < num_buckets) {
			b = j;
			key = key * 2862933555777941757ULL + 1;
			j = (b + 1) * ((double)(1LL << 31) / (double)((key >> 33) + 1));
		}
	}
	else {
		b = 0; /* always 0 */
	}
	return b;
}
#endif

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - 전달받은 hash value 값을 consistent hash ring 에서 검색하여....
// - pool_by_id[] 에서 online 상태인 array index 정보를 반환한다.
// - 만약 최초 선택된 node 부터 등록된 서버 count + 3 개의 node 가 모두 offline 이면 -1 을 반환한다.
static int 
lb_get_consistent_hash_index( lb_t *lb, unsigned int hash_value )
{
	// 인자값은 호출 함수에서 유효성 check 할것...
	// 본 함수에서는 인자값은 모두 유효하다고 가정하고.. flow 진행.

	lb_consistent_ring_node_t * node = lb->consistent_hash_ring.node_arrary;

	int index_min = 0;
	int index_max = lb->consistent_hash_ring.node_count;
	int index_mid = 0;
	int index_result = 0;

	unsigned int value_mid, value_mid_before;

	while( 1 )
	{
		// ring 내에 중간 index 값 계산.
		index_mid = (int)((index_min + index_max) / 2);

		// max node index 초과할 경우.... 
		if( index_mid == lb->consistent_hash_ring.node_count )
		{
			// 0 번째 node 정보 사용.
			index_result = 0;
			break;
		}

		value_mid			= (node + index_mid)->value;
		value_mid_before	= ( index_mid == 0 ? 0 : (node + index_mid - 1)->value );

		// 범위내 존재하는 index 찾은 경우..
		if( hash_value <= value_mid && hash_value > value_mid_before )
		{
			index_result = index_mid;
			break;
		}

		// index_mid 값 재조정.
		if( value_mid < hash_value )
		{
			index_min = index_mid + 1;
		}
		else
		{
			index_max = index_mid - 1;
		}

		// 검색 범위 초과시
		// - 0 번 node 사용
		if( index_min > index_max )
		{
			index_result = 0;
			break;
		}
	}

	// 위에서 전달받은 hash 값을 이용하여 server index 정보 추출하여 최종 node 결정됨.
	// 이후 결정된 node 에 저장된 pool_by_id[] 의 index 정보를 이용하여....
	// pool_by_id[] 에서 서버 정보를 추출한 후 해당 서버가 online 상태인지 check.
	// 만약 online 상태가 아니라면... 
	// - 추출된 server index 값을 증가시킨 후 위의 check 를 반복
	// 반복 횟수
	// - lb 에 등록된 서버 갯수 4 배수  만큼 한다. (lb_consistent_ring_t::server_count * 4  )
	//    (동일 서버가 연속 3개까지 배치되는 경우가 종종 발견, #33116 에서 변경됨)
	// - 만약 online 상태인 서버가 1대인 경우 본 함수를 사용하지 않고.. 해당 서버를 직접 검색하여 전달하도록 코딩되어 있음. (lb_policy_hash() )

	// Consistent hash ring 에서 Online 상태 node 검색관련 최대 재시도 횟수 변경 (#33116)
	// - oringin 12 대중 4 대가 alive 된 상황에서 oringin 선정 실패 발생
	// - 이를 개선하기 위해 검색 재시도 횟수를 oringin count + 3 -> (origin count) * 4 로 변경
	// - 검색 시도 횟수가 특정값 이상이면.. 로깅 처리 추가 

	int result_server_index = -1;
	int search_count = 0;
	int max_search_count = ( lb->consistent_hash_ring.server_count ) * 4;

	do {

		// node 에 저장된 server 가 online 상태인 경우.
		if( lb->pool_by_id[ (node + index_result )->arraryindex_pool_by_id ]->online == 1 )
		{
			// pool_by_id[] 의 index 정보를 반환.
			result_server_index = (node + index_result)->arraryindex_pool_by_id ;
			break;
		}

		index_result++;

		// max node 범위를 초과한 경우. 0 번 node 부터 다시 검색.
		if( index_result >= lb->consistent_hash_ring.node_count )
			index_result = 0;
		
		search_count++;

	} while( search_count <= max_search_count );

	// 검색횟수가 지정된 값 이상으로 발생시 로깅
	if( search_count > ( ( lb->consistent_hash_ring.server_count ) * 3 ) )
	{
		TRACE( ( T_WARN, "DRIVER[%s] consistent hash search count too many : request_hash[%u], offline_skip_count[%d], result_origin_index[%d]\n"
			, lb->assoc_driver->signature, hash_value, search_count, result_server_index ) );
	}

	// 최종 pool_by_id[] 에서 online 상태인 server index 반환.
	return result_server_index;
}


// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - 전달받은 uri (hint) 정보를 이용하여 consistent hash ring 에서 적합한 server node 를 선택한 후
// - pool_by_id[] 의 array index 정보를 반환한다.
// - online 갯수가 0 이면  LB_INVALID_HANDLE 리턴
static nc_pool_handle_t
lb_policy_hash(lb_t *lb, const void *hint) 
{
	nc_uint64_t 	handle = LB_INVALID_HANDLE;
	nc_uint64_t		hv = 0;

	if( lb->online_pool_count <= 0 )
	{
		// CHG 2019-07-26 huibong logging level 변경
		// - ERROR 로 로깅할 경우 로그 감시에 문제 발생 가능하여 INFO 로 변경 처리
		TRACE( (T_PLUGIN, "DRIVER[%s].lb[%p] server_count[%d] server_0st[%s]: online server not exist.\n"
			, lb->assoc_driver->signature, lb, lb->pool_count, lb->pool_by_id[0]->id) );

		return LB_INVALID_HANDLE;
	}

	// test 소요시간 추출을 위한 코드
	//struct timespec time_start, time_now;
	//double time_elapsed = 0.0;
	//clock_gettime( CLOCK_MONOTONIC, &time_start );
	
	if( lb->online_pool_count == 1 )
	{
		// server 가 1대인 경우에는 consistent hash ring 을 생성하지 않으므로....
		// pool_by_id[] 에서 online 상태인 서버를 직접 찾아 해당 index 를 반환한다.
		// 추가로 online 상태이 서버가 1대인 경우에도 직접 검색 처리한다.

		int i;
		for( i = 0; i < lb->pool_count; i++ )
		{
			if( lb->pool_by_id[i]->online == 1 )
			{
				hv = i;
				handle = lb_make_index( lb->conf_version, hv );
			}
		}
	}
	else if( lb->consistent_hash_ring.node_arrary != NULL )
	{
		// consistent hash ring 을 이용하여 적합한 server 를 검색.

		// 전달받은 hint(uri) 정보를 md5 로 hash 처리하여 정수를 값을 생성.
		unsigned int hash_value = 0;
		nc_MD5_CTX md5Context;
		int arraryindex_pool_by_id;

		nc_MD5Init( &md5Context );
		nc_MD5Update( &md5Context, (unsigned char *)hint, strlen( hint ) );
		nc_MD5Final( &md5Context );

		hash_value = (md5Context.digest[3] << 24)
			| (md5Context.digest[2] << 16)
			| (md5Context.digest[1] << 8)
			| (md5Context.digest[0]);

		// consistent hash ring 에서 server 검색.
		arraryindex_pool_by_id = lb_get_consistent_hash_index( lb, hash_value );

		if( arraryindex_pool_by_id != -1 )
		{
			// consistent hash ring 에서 적합한 server 가 선정된 경우
			// - pool_by_id[] 상의 server index 정보 반환.
			hv = arraryindex_pool_by_id;
			handle = lb_make_index( lb->conf_version, hv );

			// test logging
			//TRACE( (T_INFO, "lb[%p] server_count[%d] -> consistent hash result index[%2llu] server[%s] uri[%s]\n"
			//	, lb, lb->pool_count, hv, lb->pool_by_id[hv]->id, hint) );
		}
		else
		{
			// consistent hash ring 에서 적합한 server 선택 실패시 LB_INVALID_HANDLE 반환
			TRACE( (T_INFO, "DRIVER[%s].lb[%p] server_count[%d] server_0st[%s]: server select failed from consistent hash.\n"
				, lb->assoc_driver->signature, lb, lb->pool_count, lb->pool_by_id[0]->id) );
		}
	}
	else
	{
		// server count 가 2 개 이상인데....
		// consistent hash ring 이 존재하지 않는 경우 LB_INVALID_HANDLE 반환
		// - 본 상황은 consistent hash ring 생성 관련 메모리 할당 실패시 발생 가능
		TRACE( (T_WARN, "DRIVER[%s].lb[%p] server_count[%d] server_0st[%s]: consistent hash ring not exist.\n"
				, lb->assoc_driver->signature, lb, lb->pool_count, lb->pool_by_id[0]->id) );
	}

	// test 소요시간 측정 및 최종 결과 로깅
	//clock_gettime( CLOCK_MONOTONIC, &time_now );
	//time_elapsed = (double)((time_now.tv_sec - time_start.tv_sec) * 1.0e9 + (time_now.tv_nsec - time_start.tv_nsec)) / 1.0e9;
	//TRACE( (T_INFO, "consistent hash select end: elapsed[%.6lf sec] lb[%p] server_count[%d] return[%llu][%llu] [%s][%s]\n"
	//	, time_elapsed, lb, lb->pool_count, hv, handle
	//	, ( handle == LB_INVALID_HANDLE ? "error" : lb->pool_by_id[hv]->id ), hint) );

	return handle;
}

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - 기존 google consistent hash 이용한 코드는 통째로 주석 처리함.
//static nc_pool_handle_t
//lb_policy_hash( lb_t *lb, const void *hint )
//{
//	nc_uint64_t 	handle = LB_INVALID_HANDLE;
//	nc_uint64_t		hv;
//
//	if( lb->online_pool_count > 0 )
//	{
//		hv = path_hash( (char *)hint );
//		/*
//		* lb_set_online에서 pool이 다운상태로 변할 때,
//		* pool의 online상태가 상위로 오도록 재정렬.
//		* pool_count대신 online 갯수를 modulo 값으로 이용
//		* online 갯수가 0이면 polish_hash는 LB_INVALID_HANDLE리턴
//		*/
//
//		hv = google_consistent_hash( hv, lb->online_pool_count );
//		TRACE( (0, "WEON::%s-OC[%d]:hint(%s) -> hash index[%d]\n",
//			__func__,
//			lb->online_pool_count,
//			(char *)hint,
//			hv) );
//		handle = lb_make_index( lb->conf_version, hv );
//
//	}
//
//	return handle;
//}

/*
 * policy : primary/secondary
 */
static nc_pool_handle_t
lb_policy_ps(lb_t *lb, const void *hint) 
{
	nc_uint64_t 	handle;
#if 0
	if (lb->lb_index != LB_INVALID_HANDLE) {
		if (!lb_check_version(lb, lb->lb_index) || ! lb_check_online(lb, lb->lb_index)) {
			/*
			 * 현재 cursor는 config 버전이 바뀌어서 사용할 수 없음.
			 */
			goto L_start_first;
		}
		handle = lb->lb_index;
	}
	else {
L_start_first:
		handle = lb_find_next_pool_online_nolock(lb, LB_INVALID_HANDLE);
	}
	if (handle != LB_INVALID_HANDLE) {
		lb->lb_index = handle;
	}
#else
	lb->lb_index = LB_INVALID_HANDLE;
	handle = lb_find_next_pool_online_nolock(lb, LB_INVALID_HANDLE);
	//if (handle != LB_INVALID_HANDLE) {
	//	lb->lb_index = handle;
	//}
#endif
	return handle;
}
static int
lb_is_online_nolock(lb_pool_t *pool)
{
	LB_CHECK_CORRUPTION(pool);
	return (pool->online != 0);
}
int
lb_check_online(lb_t *lb, nc_uint64_t phandle)
{
	nc_uint32_t 	pi;
	int 			r, trz;
	LB_TRANSACTION(lb, trz) {
		pi = lb_get_pindex(phandle);
		r = lb_is_online_nolock(lb_pool_by_index(lb, pi));
	}
	return r;
}
static void
lb_remove_object_nolock(lb_pool_t *pool, lb_object_t *lo)
{
	link_del(&pool->olist, &lo->node);
}
static void
lb_free_object_nolock(lb_object_t *lo, int timerrun)
{
	XFREE(lo->uobject_id);
	if (timerrun)
		bt_destroy_timer(__timer_wheel_base, &lo->t_expire);
	XFREE(lo);
}
void *
lb_pool_userdata(lb_t *lb, nc_pool_handle_t ph)
{
	nc_uint32_t	pi;
	lb_pool_t 	*lbpool = NULL;
	void 		*ud = NULL;
	int 		trz;
	LB_TRANSACTION(lb, trz) {
		if (lb_check_version(lb, ph)) {
			pi = lb_get_pindex(ph);
			lbpool = lb_pool_by_index(lb, pi);
			LB_CHECK_CORRUPTION(lbpool);
			ud = lbpool->userdata;
		}
	}
	return ud;
}

lb_pool_t *
lb_pool_by_index(lb_t *lb, nc_uint32_t pi)
{
	//nc_uint64_t 	ep = (nc_uint64_t)lb->elect_pool;
	if  (lb->elect_pool == lb_policy_rr) {
			LB_CHECK_CORRUPTION(lb->pool_by_pri[pi]);
			return lb->pool_by_pri[pi];
	}
	else if  (lb->elect_pool == lb_policy_ps) {
			LB_CHECK_CORRUPTION(lb->pool_by_pri[pi]);
			return lb->pool_by_pri[pi];
	}

	// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
	// - hash 정책 사용시.. 선택된 index 값을 pool_by_id[] 에 적용하여 server 를 추출하도록 수정
	//else if  (lb->elect_pool == lb_policy_hash) {
	//		LB_CHECK_CORRUPTION(lb->pool_by_pri[pi]);
	//		return lb->pool_by_not[pi];
	//}
	else if  (lb->elect_pool == lb_policy_hash) {
			LB_CHECK_CORRUPTION(lb->pool_by_id[pi]);
			return lb->pool_by_id[pi];
	}

	return NULL;
}

nc_pool_handle_t
lb_get_policy_pool(lb_t *lb, const void *hint)
{
	nc_uint64_t 	pool;	
	int		trz;

	DEBUG_ASSERT(LB_VALID(lb));
	DEBUG_ASSERT(nc_check_memory(lb, 0));

	if (lb->shutdown) 
		pool =  LB_INVALID_HANDLE;
	else
		pool = (*lb->elect_pool)(lb, hint);
	return pool;
}

static lb_object_t *
lb_find_object_nolock(lb_pool_t *pool, const char *object_id)
{
	lb_object_t 	*current;
	lb_object_t 	*found = NULL;

	LB_CHECK_CORRUPTION(pool);
	current = (lb_object_t *)link_get_head_noremove(&pool->olist);
	if (object_id == NULL) {
		found = current;
	}
	else {
		while (current) {
			if (strcasecmp(object_id, current->uobject_id) == 0) {
				found = current;
				break;
			}
			current = (lb_object_t *)link_get_next(&pool->olist, &current->node);
		}
	}
	return found;
}
void
lb_print(lb_t *lb)
{
	int 	i;

	for (i = 0; i < lb->pool_count; i++) {
		if (lb->pool_by_pri[i])
			fprintf(stderr, "[%02d] - %-10s %-10d %-10d\n", 
					i,
					lb->pool_by_pri[i]->id, 
					lb->pool_by_pri[i]->priority, 
					lb->pool_by_pri[i]->online );
	}
}



#if 1
void 
lb_set_monitor(lb_t *lb, void (*monitor_proc)(lb_t *, char *id, void *cbd, int online), void *cbdata  )
{
	int 	trz;
	LB_TRANSACTION(lb, trz) {
		lb->onoff_monitor = monitor_proc;
		lb->onoff_cbdata  = cbdata;
	}	
}

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - consistent hash ring 객체 생성시 qsort 처리를 위한 함수
// - lb_consistent_ring_node_t 의 value 값을 기준으로 작은 순서대로 정렬되도록 함.
static int 
lb_compare_consistent_hash_node( const void * a, const void * b )
{
	lb_consistent_ring_node_t * server1 = (lb_consistent_ring_node_t *)a;
	lb_consistent_ring_node_t * server2 = (lb_consistent_ring_node_t *)b;

	return (server1->value < server2->value) ? -1 : ((server1->value > server2->value) ? 1 : 0);
}

// NEW 2019-07-19 consistent hash ring 기능 추가 (#32409)
// - pool_by_id[] 에 등록된 server 정보를 이용하여 consistent hash ring 생성 및 초기화 . 
// - 성공시 0, 그외 반환값은 실패 ( -1: 시스템 오류, 1: 인자값 오류, 2: lb 상태값 오류 )
static int 
lb_create_consistent_hash_ring( lb_t * lb )
{
	// 1. lb 유효 check.
	if( lb == NULL )
	{
		TRACE( (T_ERROR, "lb[%p] not valid. consistent hash ring create failed.\n", lb) );
		return 1;
	}

	// 2. lb 정책 설정값 check
	// - policy 설정이 hash 가 아니라면 consistent hash 객체 만들 필요 없음.
	if( lb->elect_pool != lb_policy_hash )
	{
		TRACE( (T_WARN, "DRIVER[%s].lb[%p] policy setting is not hash. consistent hash ring not create.\n", lb->assoc_driver->signature, lb) );
		return 1;
	}

	// 3. pool count 정보 check
	// - 등록된 서버가 없거나 1대이면.. 못 만들거나.. 만들 필요 없는 상황.
	if( lb->pool_count <= 1 )
	{
		TRACE( (T_WARN, "DRIVER[%s].lb[%p] server_count[%d] server_0st[%s]: server count not enough for create consistent hash ring.\n"
			,lb->assoc_driver->signature, lb, lb->pool_count, (lb->pool_count < 1 ? "not exist" : lb->pool_by_id[0]->id ) )) ;
		return 2;
	}

	// 4. 이미 consistent hash ring 객체가 생성되어 있는 경우....
	if( lb->consistent_hash_ring.node_arrary != NULL )
	{
		// 기존값과 현재값이 동일한지 check 할 수 있으나.... 무지 복잡해짐.
		// - lb->conf_version 값은 server add 될 때마다 변경되는 값으로 사용 가능하나.. 
		// - server 1대만 수정할 경우 값이 동일하므로 적합하지 않음.
		// 따라서 기존값 삭제하고 다시 생성한다. (테스트 결과.. 소요시간 1/1000 sec 수준으로 미미함)
		XFREE( lb->consistent_hash_ring.node_arrary );
		lb->consistent_hash_ring.node_arrary = NULL;
	}

	// consistent hash ring 을 생성한다.
	// - 각 server 별 가상 node  갯수는 160 개 하드코딩 (ketama ring hash 참조)
	lb->consistent_hash_ring.node_arrary = (lb_consistent_ring_node_t *)XMALLOC( sizeof( lb_consistent_ring_node_t ) * (lb->pool_count) * 160, AI_DRIVER );
	if( lb->consistent_hash_ring.node_arrary == NULL )
	{
		TRACE( (T_WARN, "DRIVER[%s].lb[%p] server_count[%d]: consistent_hash_ring object memory alloc failed.\n", lb->assoc_driver->signature, lb, lb->pool_count) );
		return -1;
	}

	// 소요시간 추출을 위한 코드
	struct timespec time_start, time_now;
	double time_elapsed = 0.0;
	//memset( &time_start, 0x00, sizeof( struct timespec ) );
	//memset( &time_now, 0x00, sizeof( struct timespec ) );

	// consistent hash ring 생성 logging
	// - lb 를 정보를 표시할 방법이 마땅치 않아... pool_by_id[0] 에 등록된 server 정보를 로깅하도록 한다.
	TRACE( (T_INFO, "DRIVER[%s].consistent_hash_ring create start: lb[%p] server_count[%d] server_0st[%s]\n", lb->assoc_driver->signature, lb, lb->pool_count, lb->pool_by_id[0]->id ) );
	clock_gettime( CLOCK_MONOTONIC, &time_start );

	// pool_by_id[] 에 등록된 server 정보를 이용하여 consistent hash node 을 생성한다.
	// - pool_by_id[] 는 가장 기본 arrary 로 server dead 상황시 order 가 변경되지 않는 특성이 있어 사용.
	// - server 별 가중치 기능은 향후 사용 가능성이 있어 기능은 구현하되... 현재는 무조건 server 당 100% 으로 동일하게 설정한다.
	int i;
	unsigned int j, k, total_index = 0;
	char szServerId[512];				// 실제 각 lb_pool_t 객체의 id 값은 가변 크기이지만... 512 이상 될 경우 없으므로....
	unsigned int virtual_node_count;	// 장비별 가중치에 의한 장비별 실제 가상 node 갯수 
	nc_MD5_CTX md5Context;

	for( i = 0; i < lb->pool_count; i++ )
	{
		// server 별 weight 기능 (현재는 미사용 중이므로 주석 처리함)
		//float server_percent = (float)(lb->pool_by_id[i].weight) / (float)(lb->total_weight);
		// weight 값을 적용한 server 별 가상 node 갯수.
		//virtual_node_count = floorf( server_percent * 40.0 * (float)(lb->pool_count) );

		// server 별 가상 node 갯수.
		// - 각 MD5 값 1개당 4개의 int 정보를 생성하므로... 가상 node 기본 160 개를 위해서는 40 을 곱한다.
		// - 만약 server 별 weight 기능을 사용하려면 본 변수 선언 대신 위의 주석처리된 식을 사용하면 됨 (ketama 에 포함된 로직)
		virtual_node_count = 40;

		// 서버당 가상 node 수만큼 hash 정보 생성.
		for( j = 0; j < virtual_node_count; j++ )
		{
			snprintf( szServerId, 512, "%s-%d", lb->pool_by_id[i]->id, j );
		
			nc_MD5Init( &md5Context );
			nc_MD5Update( &md5Context, (unsigned char *)szServerId, strlen(szServerId) );
			nc_MD5Final( &md5Context );

			// md5 계산결과 16 Byte 중 4 byte 씩 추출하여 unsigned int 로 변환.
			// 따라서 가상 node 1개당 1개의 MD5 hash 값이 생성되며 이를 정수로 변환하여 4개의 값을 생성한다.
			for( k = 0; k < 4; k++ )
			{
				// hash node 의 값 저장.
				lb->consistent_hash_ring.node_arrary[ total_index ].value =   (md5Context.digest[3 + k * 4] << 24)
																			| (md5Context.digest[2 + k * 4] << 16) 
																			| (md5Context.digest[1 + k * 4] << 8) 
																			| (md5Context.digest[    k * 4]);
				
				// 해당 hash node 가 가르키는 pool_by_id[] 의 arrary index 값을 저장.
				lb->consistent_hash_ring.node_arrary[total_index].arraryindex_pool_by_id = i;

				// consistent ring hash arrary 의 index 값 증가 처리
				total_index++;
			}
		}
	}

	// sort 처리
	// - 각 node 별 생성된 hash 값(lb_consistent_ring_node_t.value) 기준, 낮은 순서로 sort 처리
	qsort( lb->consistent_hash_ring.node_arrary, total_index, sizeof( lb_consistent_ring_node_t ), lb_compare_consistent_hash_node );

	// consistent hash ring 관련 나머지 정보 기록
	lb->consistent_hash_ring.server_count	= lb->pool_count;
	lb->consistent_hash_ring.node_count		= total_index;
	lb->consistent_hash_ring.min_value		= lb->consistent_hash_ring.node_arrary[0].value;
	lb->consistent_hash_ring.max_value		= lb->consistent_hash_ring.node_arrary[total_index - 1].value;

	// 소요시간 측정.
	clock_gettime( CLOCK_MONOTONIC, &time_now );
	time_elapsed = (double)((time_now.tv_sec - time_start.tv_sec) * 1.0e9 + (time_now.tv_nsec - time_start.tv_nsec)) / 1.0e9;

	// 최종 처리 내역 logging
	TRACE( (T_INFO, "DRIVER[%s] consistent hash ring create ok.[%.6lf sec]: lb[%p] server_count[%d] server_0st[%s] -> node_count[%d] value[%u - %u]\n"
		, lb->assoc_driver->signature, time_elapsed, lb, lb->pool_count, lb->pool_by_id[0]->id
		, lb->consistent_hash_ring.node_count, lb->consistent_hash_ring.min_value, lb->consistent_hash_ring.max_value ) );

	// test 출력용 코드.
	//for( i = 0; i < lb->consistent_hash_ring.node_count; i++ )
	//{
	//	TRACE( (T_INFO, "lb[%p] consistent[%4d] = [%11u, %2d] -> [%s, %s]\n"
	//		, lb, i
	//		, lb->consistent_hash_ring.node_arrary[i].value
	//		, lb->consistent_hash_ring.node_arrary[i].arraryindex_pool_by_id
	//		, lb->pool_by_id[lb->consistent_hash_ring.node_arrary[i].arraryindex_pool_by_id]->id 
	//		, (lb->pool_by_id[lb->consistent_hash_ring.node_arrary[i].arraryindex_pool_by_id]->online) == 1 ? "on" : "off"  ));
	//}

	return 0;
}

#endif /* module test routine */
