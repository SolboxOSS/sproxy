#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory.h>
//#include <ncapi.h>

#include "common.h"
#include "reqpool.h"
//#include "trace.h"
#include "scx_util.h"
/*
 * memory pool management routines
 * 본 메모리 풀 관리는, 항상 할당만 있고, 프리는 없다.
 * solhttpd에서 사용은, request의 진입 시점에 pool의 생성
 * request처리 과정의 동적 메모리 할당은 본 메모리 할당 루틴과
 * vstring모듈에서 메모리 할당을 통해서 일어나며,
 * 할당된 메모리의 삭제는 request처리를 완료한 시점에
 * mp_free() 호출 때 일괄 프리가 일어나도록 구현되어있다.
 * 기대효과는 memory alloc/free 과정의 부하를 절감되어, 
 * 응답속도의 개선, CPU 사용량의 절감에 있다.
 */

#define 	MP_ALIGN_UNIT 			(sizeof(long))
#define 	MP_ALIGN(_n) 			((_n+(MP_ALIGN_UNIT-1)) & (~(MP_ALIGN_UNIT-1)))

#define 	MP_REQUIRED_SIZE(_n)	(sizeof(size_t) + MP_ALIGN(_n))
static mem_pool_t * mp_allocate_pool(mem_pool_t *mainpool, size_t size);

mem_pool_t *
mp_create(size_t size)
{
	mem_pool_t 	*mp = NULL;

	mp = mp_allocate_pool(NULL, size);
	mp->current = mp; 	/* self pointing*/
	mp->count 	= 1; 	/* # of pool */
	return mp;
}
static mem_pool_t *
mp_allocate_pool(mem_pool_t *mainpool, size_t size)
{
	mem_pool_t 	*mp;

	mp = (mem_pool_t *)SCX_CALLOC(1, sizeof(mem_pool_t));
	mp->memory = (char *)SCX_CALLOC(1, size); /* 메모리를 0으로 초기화 포함*/
	mp->current = NULL;
	mp->next 	= NULL;
	mp->subpool = (mainpool != NULL);
	if (mainpool) mainpool->count++;
	mp->asize 	= size;
	mp->rsize 	= size;
	mp->fpos 	= 0;
	return mp;
}
size_t 
mp_consumed(mem_pool_t *pool)
{
	ASSERT(pool && pool->subpool == 0);


	/*
	 * 주의 : 이 계산은 정확하지 않음
	 * 각 풀에서 남은 공간이 작아서 할당되지 못한 fragment 있음을 주의
	 */
	return (pool->count*pool->asize - pool->current->rsize);
}
void *
mp_alloc(mem_pool_t *mp, size_t size)
{
	void 	*p = NULL;

	if (mp->asize < MP_REQUIRED_SIZE(size)) {
		/*
		 * 최대 할당 크기보다 큰 요청이 입력됨
		 */
		 TRACE((T_ERROR, "pool error: allowable allocation should be smaller than %ld(request size : %ld)\n", (unsigned long)mp->asize, MP_REQUIRED_SIZE(size)));
		 ASSERT(0);
		 return NULL;
	}
	if (mp->current->rsize < MP_REQUIRED_SIZE(size)) {
		mem_pool_t 	*newmp = NULL;
		newmp = mp_allocate_pool(mp, mp->asize);

		mp->current->next = newmp;
		mp->current = newmp;
	}

	if (mp->current->rsize >= MP_REQUIRED_SIZE(size)) {

		p = (void *)((char *)mp->current->memory + mp->current->fpos);
		mp->current->fpos 	+= MP_REQUIRED_SIZE(size);
		mp->current->rsize 	-= MP_REQUIRED_SIZE(size);
		*(size_t *)p  = size; /* 할당 크기 정보의 저장, 향후 realloc시 사용*/
		p = (char *)p + sizeof(size_t);
	//	size_t 	newasiz = mp->asize + max(size, 4096);
	}
	return p;
}
void *
mp_realloc(mem_pool_t *mp, void *p, size_t size)
{
	size_t 	oldsize = -1;
	void 	*newp = NULL;

	newp = mp_alloc(mp, size);
	if (p) {
		/* 이전 할당된 메모리 크기를 확인 */
		oldsize = * (size_t *)((char *)p - sizeof(long));
		memcpy(newp, p, oldsize);
	}
	return newp;
}
void
mp_free(mem_pool_t *mp)
{
	mem_pool_t	*nextpool;

	while (mp) {
		nextpool = mp->next;
		SCX_FREE(mp->memory);
		SCX_FREE(mp);
		mp = nextpool;
	}
}

char *
mp_strdup(mem_pool_t *mp, const char *string)
{
	char * v;
	int len = strlen(string) + 1;
	v = mp_alloc(mp, len);
	if (!v) return NULL;
	strncpy(v, string, len);
	return v;
}
