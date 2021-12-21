#ifndef __NETCACHE_CONFIG_H__
#define __NETCACHE_CONFIG_H__

#define  __CONFIG_WAIT_COMPLETION_INTERVAL_USEC 	2


//#include <config.h>
//#include <netcache_types.h>

#if NC_ENABLE_GPERFPROF
#include <gperftools/profiler.h>
#endif



/*
 * NC_ENABLE_WRITE_VECTOR definition은 웬만하면 활성화할 수 없음
 * 이유는 현재 기본 동작 방식인 block하나를 원본에서 가져오면
 * 그때마다 하나의 KAIO를 생성 스케줄하는 경우보다
 * 모든 block을 수신 후 전체를 batch로 내리면 커널부하가
 * 늘어나고, ASIO 실행 대기시간이 늘어남
 * by weon@solbox.com
 */
//#define		NC_ENABLE_WRITE_VECTOR
/*
 * 이전의 경우 CRC 저장 및 디스크 캐싱 블럭을 읽으면 CRC 검증을 기본으로 했으나
 * 최근(2020년초) MDB 및 캐싱 파일 저장 방식을 수정하면서 CRC 사용을 비활성화 함
 * 현재 저장의 순서는 디스크에 block데이타 저장, 저장완료 이벤트 수신 후 
 * blockmap 정보 변경의 순으로 실행되도록 함으로써 파일 시스템이 깨지지 않는 한 
 * block데이타가 잘못될 수 없도록 함
 * by weon@solbox.com
 *
 */
//#define		NC_ENABLE_CRC
#define		NC_PASS_BLOCK_WQ
#define			NC_LAZY_BLOCK_CACHE
//#define 		EXPERIMENTAL_USE_SLOCK
/*
 * define if multiple-caching partition required
 */
//#define			NC_MULTIPLE_PARTITIONS

//
// MDB 비동기 operation에 대해서 sync opeartion
// 활성화
//
#define NC_ENABLE_MDB_WAIT

//
//최대 inode갯수를 한정시킬 때만 사용
//
#define	UNLIMIT_MAX_INODES


/*
 * exclusive-lock획득 수단으로 spinlock사용
 */
//#define		NC_RWLOCK_USE_SPINLOCK

/*
 * 활성화되면 cache file header동기화가 한시간 이상 느려짐
 * #define		NC_ENABLE_LAZY_HEADER_SYNC
 */

/*
 * block, inode cache manager lock의
 * 사용 추적(lock횟수, lock위치, lock holding타임)
 */
//#define NC_MEASURE_CLFU
/*
 * path-lock의 사용 추적(lock횟수, lock위치, lock holding타임)
 */
//#define NC_MEASURE_PATHLOCK
/*
 * mutex의 사용 추적(lock횟수, lock위치, lock holding타임)
 */
//#define NC_MEASURE_MUTEX


//#define NC_DEBUG_CLFU
//#define NC_DEBUG_BCM
//#define NC_DEBUG_PATHLOCK


#ifdef NC_DEBUG_BCM
#define	NC_DEBUG_LIST
#endif
#define	NC_DEBUG_LIST


#define NC_VOLUME_LOCK


//#pragma message("**********NC_BLOCK_TRACE는 release전에 undef!*************")
//#define 	NC_BLOCK_TRACE



#if 1
#ifndef		NC_DEBUG_BUILD
#define		NC_RELEASE_BUILD
#endif
#else
#pragma message("***************** DEBUG BUILD *******************")
#undef NC_RELEASE_BUILD
#undef NC_DEBUG_BUILD
#endif





#define		__PROFILE 


/* #define VM_USE_STAT */

/*
 * memory debugger flag
 */
#ifdef NC_HEAPTRACK
 #ifndef NC_MEM_DEBUG
  #define NC_MEM_DEBUG
 #endif
#endif

#ifdef NC_OVERFLOW_CHECK
 #ifndef NC_MEM_DEBUG
  #define NC_MEM_DEBUG
 #endif
#endif

#ifdef NC_MEMLEAK_CHECK
 #ifndef NC_MEM_DEBUG
  #define NC_MEM_DEBUG
 #endif
#endif


#ifdef WIN32

#define 	CALLSYNTAX	
#ifdef BUILD_DLL
#define	PUBLIC_IF	__declspec(dllexport)
#else
#define	PUBLIC_IF	__declspec(dllimport)
#endif

#else /* UNIX */

#define 	CALLSYNTAX	
#define PUBLIC_IF
#endif



/*
 * 실험적 또는 튜닝 옵션 적용 정의
 */

/*
 * path-lock에 대해서 mutex 대신 rwlock 사용
 */
//#define 	PATHLOCK_USE_RWLOCK
#include <snprintf.h>

#endif /* __CONFIG_H__ */
