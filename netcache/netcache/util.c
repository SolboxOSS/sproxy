#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/types.h>



#include <sys/syscall.h>
#include <sys/uio.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>
#include <search.h>
#include <errno.h>
#include <time.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <utime.h>
#include <sys/dir.h>
#include <pthread.h>
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "trace.h"
#include "ncapi.h"
#include "rdtsc.h"
#include "bitmap.h"
#include "netcache.h"
#include <snprintf.h>
#include <sys/stat.h>


#define		NC_MEM_PADDING			0


#define		L_HEAD(l_)	(l_)->head
#define		L_TAIL(l_)	(l_)->tail

#define		LIST_VALID(l_)	( \
		((l_)->count == 0 && (L_HEAD(l_) == NULL) && (L_TAIL(l_) == NULL)) || \
		((l_)->count == 1 && (L_HEAD(l_) != NULL) && (L_TAIL(l_) != NULL) && (L_HEAD(l_) == L_TAIL(l_))) ||  \
		((l_)->count > 1))
#ifdef NC_MEASURE_MUTEX
static  pthread_mutex_t s__mls_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct tag_mls_t mls_t; 
static mls_t * 	lookup_ml_stat(char *f, int l);
static void 	update_ml_stat(mls_t *found, double msec);
#endif

/* 
 * This macro opens filename only if necessary and seeks to 0 so
 * that successive calls to the functions are more efficient.
 * It also reads the current contents of the file into the global buf.
 */
#define FILE_TO_BUF(filename, fd) do{				\
    static int local_n;						\
    if (fd == -1 && (fd = open(filename, O_RDONLY)) == -1) {	\
	TRACE((T_ERROR, "%s\n", BAD_OPEN_MESSAGE));			\
	_exit(102);						\
    }								\
    lseek(fd, 0L, SEEK_SET);					\
    if ((local_n = read(fd, buf, sizeof buf - 1)) < 0) {	\
	TRACE((T_ERROR, "read '%s' error:'%s'\n", filename, strerror(errno)));\
	perror(filename);					\
	fflush(NULL);						\
	_exit(103);						\
    }								\
    buf[local_n] = '\0';					\
}while(0)

/* evals 'x' twice */
#define SET_IF_DESIRED(x,y) do{  if(x) *(x) = (y); }while(0)


/*
 * memory category & statistics
 */
typedef struct {
	nc_int32_t 		count; 		/* # of allocations not freed yet */
	nc_int64_t 		allocated;	/* amount of allocation in bytes */
} ai_element_t;

static ai_element_t		__ai_category[AI_COUNT] ;

#define      NC_ALIGN_UNIT                   	8
#define      NC_ALLOC_ALIGN(_n)					((~(NC_ALIGN_UNIT-1))&((_n)+(NC_ALIGN_UNIT-1)))
/*
 * 할당된 메모리 구조
 *	
 *  +-----------------------------------------------+
 *  |      __meminfo_t                              |
 *  +-----------------------------------------------+
 *  |      실제 사용하는 영역                       |
 *  |                                               |
 *  |                                               |
 *  |                                               |
 *  |                                               |
 *  +-----------------------------------------------+
 *  |     8바이트 패딩 영역(overflow감내영역)       |
 *  +-----------------------------------------------+
 *
 */


#ifdef NC_MEM_DEBUG
	typedef struct {
		nc_uint32_t 	magic;
		nc_uint32_t 	length;
		nc_uint16_t 	category;
	#if defined(NC_MEMLEAK_CHECK)
		const char 		*s_file;
		nc_uint16_t 	s_line;
	#endif

	#if defined(NC_MEMLEAK_CHECK)
		nc_uint8_t 		aligned:1;
		link_node_t		node; 		/* debug only */
	#endif

	#ifdef NC_OVERFLOW_CHECK
		nc_uint32_t 	*barrier; 	/* 할당된 메모리가 깨졌는지 체크할 때 사용 */
	#endif
	} __meminfo_t;
#define 	__MEM_MAGIC 		0x5AA5B33B
#define 	__MEM_MAGIC_FREE 	0xB33B5AA5

#else
	/*
	 * top : 8 byte
	 * 할당된 메모리 구조
	 *
	 *
	 */
	typedef struct {
		nc_uint16_t 	magic;
		nc_uint16_t 	category;
		nc_uint32_t 	length;
	} __meminfo_t;
#define 	__MEM_MAGIC 		0x5AA5
#define 	__MEM_MAGIC_FREE 	0xB33B


#endif

#define	GET_MP(_mp, _p) __meminfo_t *_mp = (__meminfo_t *)((char *)(_p) - sizeof(__meminfo_t))
#define	GET_MP2UP(_mp) 	(void *)((char *)_mp + sizeof(__meminfo_t))


/*
 *	sizeof(nc_uint64_t)는 할당된 메모리 마지막에 추가 여유로 할당되는 부분임
 *  sqlite3나 curl등 외부 lib에서 memory overflow문제에 대한 최소한의 
 *  방어 fense임
 */
#define		__MEM_MAGIC_SIZE 	(sizeof(__meminfo_t) + sizeof(nc_uint64_t))



#if defined(NC_MEMLEAK_CHECK)

static link_list_t 			__dm_list = {.head = NULL, .tail=NULL};
static pthread_mutex_t 		__dm_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/*
 * aligned memory leak tracking variables
 */
static pthread_mutex_t  	s__md_lock = PTHREAD_MUTEX_INITIALIZER;
static u_hash_table_t   	*s__page_hash = NULL;

#endif




/*
 * application-scope global cached clock
 */
static pthread_rwlock_t 	__nc_clock_lock = PTHREAD_RWLOCK_INITIALIZER;
static time_t 				__nc_clock = 0;
extern __thread int 		___bypass_hook;

/*
 * prototypes
 */
#ifdef NC_MEM_DEBUG
static inline nc_uint32_t __nc_mark_free(__meminfo_t *mp, const char *f, int l);
static void __nc_update_info(__meminfo_t *p, size_t length, int aligned, const char *sf, int sl);
static inline void __nc_set_barrier(__meminfo_t *mp);
#endif
static inline void * __nc_set_info(__meminfo_t *mp, nc_uint32_t l, int category, int aligned, const char *sf, int sl);


void 
ai_add(int category, nc_int32_t sz)
{
	nc_int32_t 	c;
	nc_int64_t 	a;
	c = _ATOMIC_ADD(__ai_category[category].count, 1);
	a = _ATOMIC_ADD(__ai_category[category].allocated, sz);
}
void
ai_sub(int category, nc_int32_t sz)
{
	nc_int32_t 	c;
	nc_int64_t 	a;
	c = _ATOMIC_SUB(__ai_category[category].count, 1);
	a = _ATOMIC_SUB(__ai_category[category].allocated, sz);
}
void PUBLIC_IF bt_msleep(long timeout)
{
	struct timespec request, remaining; 
	extern int __terminating;
#define	WAIT_TIME_SLICE_MSEC	5000

#if 1
	request.tv_sec = timeout/1000;
	request.tv_nsec = 1000000L * (timeout % 1000L);

	if (timeout <= 0) {
		return;
	}
	while (nanosleep(&request, &remaining) == -1) {
		if (errno == EINTR) {
			if (__terminating) return;
			request = remaining;
		}
	}
#else
	int		r, to;
	extern int __terminating;

	to = min(timeout, WAIT_TIME_SLICE_MSEC); /* 5 sec */

	request.tv_sec  = to/1000L;
	request.tv_nsec = 1000000L * (to % 1000L);
	while (!__terminating && timeout > 0) {
		r = pselect(0, NULL, NULL, NULL, &request, NULL);
		if (r == 0) {
			/*
			 * timeout
			 *
			 */
			timeout -= to;

			to = min(timeout, WAIT_TIME_SLICE_MSEC); /* 5 sec */

			request.tv_sec  = to/1000L;
			request.tv_nsec = 1000000L * (to % 1000L);
		}
		else {
			/* error/interrupt because r == 0 is impossible*/

			break;
		}
	}
#endif
} /* end of function */

void bt_usleep(long timeout)
{
	struct timespec request;
	request.tv_sec = timeout/1000000;
	request.tv_nsec = 1000L * (timeout % 1000000L);

	if (timeout <= 0) {
		return;
	}
	pselect(0, NULL, NULL, NULL, &request, NULL);
} /* end of function */




/********************* DOUBLY-LINKED-LIST ************************/
/* add to tail */
static void link_dump(char *prefix, link_list_t *list, int forward);

int
link_move_head(link_list_t *list, link_node_t *m)
{
	link_del(list, m);
	link_prepend(list, m->data, m);
	return 0;
}
int
link_contains(link_list_t *list, link_node_t *m)
{
	link_node_t 	*cursor;

	cursor = L_HEAD(list);
	while (cursor) {
		if (cursor == m) {
			return 1;
		}
		cursor = cursor->next;
	}
	return 0;
}
/*
 * entry를 head에 append
 */
PUBLIC_IF CALLSYNTAX int 
link_prepend(link_list_t *list, void *data, link_node_t *m)
{


	ACQUIRE_LIST(list);

	CHECK_LIST(list);
#ifdef NC_DEBUG_LIST
	DEBUG_ASSERT(GET_ATOMIC_VAL(m->u.ns.inlist) == 0);
#endif
	/*
	 * 노드 초기화
	 */
	SET_NODE(m, data);

	/*
	 * 리스트에 노드 추가
	 */
	if (L_HEAD(list) == NULL) {
		L_HEAD(list) = 
		L_TAIL(list) = m;
	}
	else {
		m->next 		 	= L_HEAD(list);
		L_HEAD(list)->prev 	= m;
		L_HEAD(list)		= m;
	}

#ifdef NC_DEBUG_LIST 
	DEBUG_ASSERT(_ATOMIC_CAS(m->u.ns.inlist, 0, 1)); 
#endif
	list->count++;

	CHECK_LIST(list);
	RELEASE_LIST(list);
	return 0;
}

int  PUBLIC_IF
link_append(link_list_t *list, void *data, link_node_t *m)
{
	ACQUIRE_LIST(list);
	CHECK_LIST(list);

	/*
	 * 노드 초기화
	 */
	SET_NODE(m, data);

#ifdef NC_DEBUG_LIST 
	DEBUG_ASSERT(GET_ATOMIC_VAL(m->u.ns.inlist) == 0);
#endif
	/*
	 * 리스트에 끝에 노드 추가
	 */


	if (L_HEAD(list) == NULL)  {

		L_HEAD(list) 	= 
		L_TAIL(list) 	= m;
	}
	else {

		L_TAIL(list)->next 	= m;
		m->prev			 	= L_TAIL(list);
		L_TAIL(list) 		= m;
	}
	list->count++;
#ifdef NC_DEBUG_LIST 
	DEBUG_ASSERT(_ATOMIC_CAS(m->u.ns.inlist, 0, 1)); 
#endif

	CHECK_LIST(list);
	RELEASE_LIST(list);

	return 0;
}

int
link_empty(link_list_t *list)
{
  return L_HEAD(list) == NULL && list->count == 0;
}
void
link_verify(link_list_t *list)
{
	if (link_count(list, U_TRUE) != link_count(list, U_FALSE)) {
		link_dump("link_verify", list, U_TRUE);
		link_dump("link_verify", list, U_FALSE);
		TRAP;
	}
}
int PUBLIC_IF
link_del(link_list_t *list, link_node_t *m)
{
	CHECK_LIST(list);
	ACQUIRE_LIST(list);

#ifdef NC_DEBUG_LIST
	DEBUG_ASSERT(GET_ATOMIC_VAL(m->u.ns.inlist) == 1);
#endif

	if (L_HEAD(list) == m) {
		L_HEAD(list) = L_HEAD(list)->next;

		if (L_HEAD(list) == NULL) 
			L_TAIL(list) = NULL;
		else
			L_HEAD(list)->prev = NULL;
	}
	else if (L_TAIL(list) == m) {
		L_TAIL(list) = L_TAIL(list)->prev;

		if (L_TAIL(list) == NULL) 
			L_HEAD(list) = NULL;
		else
			L_TAIL(list)->next = NULL;
	}
	else {
		m->prev->next = m->next;
		m->next->prev = m->prev;
	}
#ifdef NC_DEBUG_LIST
	DEBUG_ASSERT(_ATOMIC_CAS(m->u.ns.inlist, 1, 0)); 
#endif

	list->count--;

	RELEASE_LIST(list);
	CHECK_LIST(list);

	return 0;
}

PUBLIC_IF CALLSYNTAX int
link_node_valid(link_node_t *m)
{
	return 0;
}
int
link_foreach(link_list_t *list, int (*proc)(void *, void *), void *ud)
{
	link_node_t	*nav;
	int			s = 0;

	nav = L_HEAD(list);
	while (nav) {
		if ((*proc)(nav->data, ud) == 0) break;
		nav = nav->next;
		s++;
	}

	return s;

}
static void
link_dump(char *prefix, link_list_t *list, int forward)
{
#if 0
	char 	buf[2048];
	char 	*p;
	int 	n, remained = 0;
	link_node_t 	*node ;

	node = (forward? L_HEAD(list):L_TAIL(list));
	p = buf; remained = sizeof(buf);
	n = snprintf(p, remained, "%s ==> LIST(%p).[cnt=%d]%s : ", prefix, list, list->count, (forward?"FOR":"BACK"));
	p+= n; remained -= n;
	while (node) {
		n = snprintf(p, remained, "%p(%d:%08llX) , ", node, node->inuse, node->clockstamp);
		p+= n; remained -= n;
		if (remained < 10) {
			*p = 0;
			TRACE((T_WARN, buf));
			p = buf; remained = sizeof(buf);
		}

		node = (forward? node->next:node->prev);

	}
	TRACE((T_WARN, "%s\n", buf));
	return;

#endif
}
PUBLIC_IF CALLSYNTAX void *
link_get_next(link_list_t *list, link_node_t *node)
{
	return (node->next)? (node->next)->data:NULL;
}
PUBLIC_IF CALLSYNTAX void *
link_get_prev(link_list_t *list, link_node_t *node)
{
	return (node->prev)? (node->prev)->data:NULL;
}
PUBLIC_IF CALLSYNTAX void *
link_get_node_prev(link_list_t *list, link_node_t *node)
{
	return (node->prev)? (node->prev):NULL;
}
PUBLIC_IF CALLSYNTAX link_node_t  *
link_get_head_node(link_list_t *list)
{
	link_node_t	*node;

	node = L_HEAD(list);
	if (node) {
		link_del(list, node);
	}
	return node;
}
/* get data from head */
void * PUBLIC_IF
link_get_head(link_list_t *list)
{
	link_node_t	*node;
	void		*data = NULL;

	node = L_HEAD(list);
	if (node) {
		link_del(list, node);
		data = node->data;
	}
	return data;
}
/* get data from head noremove */
PUBLIC_IF CALLSYNTAX void *
link_get_head_noremove(link_list_t *list)
{
	link_node_t	*node;
	void		*data = NULL;

	node = L_HEAD(list);
	if (node) {
		data = node->data;
	}
	return data;
}
/*
 * list b를 list a의 뒤에 join
 */
int  PUBLIC_IF
link_join(link_list_t *a, link_list_t *b)
{
	CHECK_LIST(a);
	CHECK_LIST(b);

	if (a->head) {
		/* list a가 비어있지 않는 경우*/
		a->tail->next = b->head;
		if (b->head) 
			b->head->prev = a->tail;
	}
	else {
		/* list a가 비어있는 경우*/
		a->head = b->head;
	}
	if (b->tail) a->tail = b->tail;


	b->head = b->tail = NULL;
	a->count = a->count + b->count;
	b->count = 0;

	CHECK_LIST(a);

	return  0;
}
int
link_count(link_list_t *list, int forward)
{
#if 0
	link_node_t		*node = NULL;
	int				cnt = 0;

	node = (forward?L_HEAD(list):L_TAIL(list));

	while (node) {
		node = (forward?node->next:node->prev);
		cnt++;
	}
	return cnt;
#else
	return list->count;
#endif
}
void *
link_get_tail(link_list_t *list)
{
	link_node_t	*node;
	void		*data = NULL;

	node = L_TAIL(list);
	if (node) {
		CHECK_NODE(node);
		link_del(list, node);
		data = node->data;
	}
	return data;
}
void *
link_get_tail_noremove(link_list_t *list)
{
	link_node_t	*node;
	void		*data = NULL;

	node = L_TAIL(list);
	if (node) {
		data = node->data;
		CHECK_NODE(node);
	}
	return data;
}
#if NOT_USED
static link_node_t *
link_get_tail_node_noremove(link_list_t *list)
{
	return L_TAIL(list);
}
#endif

void 
link_add_n_sort(link_list_t *list, void *data, link_node_t *m, int (*_comp)(void *a, void *b))
{

	CHECK_LIST(list);

	ACQUIRE_LIST(list);

	SET_NODE(m, data);
#ifdef NC_DEBUG_LIST
	DEBUG_ASSERT(GET_ATOMIC_VAL(m->u.ns.inlist) == 0);
#endif

    // Case 1: list is empty
    if (L_HEAD(list) == NULL) {
		L_TAIL(list) = m;
		L_HEAD(list) = m;
		goto L_as_done;
    }

    link_node_t *prev = NULL;
    link_node_t *cur  = L_HEAD(list);

    while (cur) {
        if (_comp(cur->data, m->data) < 0)
            break;

        prev = cur;
        cur = cur->next;
    }

    if (cur) {
        if (cur == L_HEAD(list)) {
            m->next 	= cur;
            cur->prev 	= m;
            L_HEAD(list) = m;
			goto L_as_done;
        }

        // Sub-case 2: insert after head
		prev->next 	= m;
        m->prev 	= prev;
		m->next		= cur;
        cur->prev 	= m;
    } else {
		// Case 3: List insertion is at end of list
		// so cur is empty
		m->next = NULL;
		m->prev = prev;

		if (prev) prev->next = m;

		L_TAIL(list) = m; 

    }
L_as_done:
	list->count++;
#ifdef NC_DEBUG_LIST 
	DEBUG_ASSERT(_ATOMIC_CAS(m->u.ns.inlist, 0, 1)); 
#endif

	CHECK_LIST(list);
	RELEASE_LIST(list);


	return;
}
int
link_prove(link_list_t *list, int forward)
{
	int				kept = list->count;
	int				check = 0;
	link_node_t 	*node;

	node = (forward?list->head:list->tail);
	while  (node && check <= kept) {
		check++;
		node = (forward?node->next:node->prev);
	}
	return (check == kept);
}
#if NOT_USED
void
link_sort(link_list_t *list, int (*compare)(void *a, void *b))
{
	link_node_t 	*top 	= L_HEAD(list);
	link_node_t 	*cursor = top;
	link_node_t 	*ptr, *tmp;
	int 			n = 0;

	if (cursor == NULL || cursor->next == NULL)
		return;

	cursor = cursor->next;	
	while (cursor) {
		n = 0;

		ptr 	= cursor;
		tmp 	= cursor->prev;
		cursor 	= cursor->next;
		while (tmp && (*compare)(tmp->data, ptr->data) > 0) {
			n++;
			tmp = tmp->prev;
		}
		if (n) {
			ptr->prev->next = ptr->next;
			if (ptr->next != NULL)
				ptr->next->prev = ptr->prev;

			if (tmp == NULL) {
				tmp = top;
				ptr->prev = NULL;
				ptr->next = tmp;
				ptr->next->prev = ptr;
				top = ptr;
			}
			else {
				tmp = tmp->next;
				tmp->prev->next = ptr;
				ptr->prev = tmp->prev;
				tmp->prev = ptr;
				ptr->next = tmp;
			}
		}
	}
}
#endif

unsigned long PUBLIC_IF 
path_hash(void *data)
{
#if 0
/*
 * prime number table
 * to hash character string, we pick up the value at index of prime number
 * This is just for performance
 */
static char pi[]={      0,1,1,1,0,1,0,1,0,0, /* 0-9 */
                        0,1,0,1,0,0,0,1,0,1, /* 10-19 */
                        0,0,0,1,0,0,0,0,0,1, /* 20-29 */
                        0,1,0,0,0,0,0,1,0,0, /* 30-39 */
                        0,1,0,1,0,0,0,1,0,0, /* 40-49 */
                        0,1,0,1,0,0,0,1,0,1, /* 50-59 */
                        0,1,0,0,0,0,0,1,0,0, /* 60-69 */
                        0,1,0,1,0,0,0,0,0,1, /* 70-69 */
                        0,0,0,1,0,0,0,0,0,1, /* 80-89 */
                        0,0,0,0,0,0,0,1,0,0, /* 90-99 */
                        0,1,0,1,0,0,0,1,0,1, /* 100-109 */
                        0,0,0,1,0,0,0,0,0,0, /* 110-119 */
                        0,0,0,0,0,0,0,1,0,0, /* 120-129 */
                        0,1,0,0,0,0,0,1,0,1, /* 130-139 */
                        0,0,0,0,0,0,0,0,0,1, /* 140-149 */
                        0,1,0,0,0,0,0,1,0,0, /* 150-159 */
                        0,0,0,1,0,0,0,1,0,0, /* 160-169 */
                        0,0,0,1,0,0,0,0,0,1, /* 170-179 */
                        0,1,0,0,0,0,0,0,0,0, /* 180-189 */
                        0,1,0,1,0,0,0,1,0,1, /* 190-199 */
                        0,0,0,0,0,0,0,0,0,0, /* 200-209 */
                        0,1,0,0,0,0,0,0,0,0, /* 210-219 */
                        0,0,0,1,0,0,0,1,0,1, /* 220-229 */
                        0,0,0,1,0,0,0,0,0,1, /* 230-239 */
                        0,1,0,0,0,0,0,0,0,0, /* 240-249 */
                        0,1,0,0,0,0,0,0,0,0  /* 250-259 */
};
    const char *s = data;
    register unsigned long n = 0;
    register unsigned long j = 0;
    register unsigned long i = 0;
	register int l = strlen(s);
#if 0
    while (*s) {
        j++;
        if (pi[j])  {
                n ^= 7 * (unsigned) *s;

        }
        s++;
    }

	while (j++ < l) {
		n ^= (n << 5) + (n >> 2) + (char)(s[j]&0xFF);
		j++;
	}
	i = n;
	fprintf(stderr, "[%s] => %u\n", data, i);
    return i; 
#else
	while (j < l) {
		n ^= (n << 5) + (n >> 2) + (char)(s[j]&0xFF);
		j+=2;
	}
	return n;
#endif
#else
	uint32_t FNV1A_Hash_Yoshimura(const char *str, uint32_t wrdlen);
	int 	l=-1;
	if (l < 0) {
		l = strlen((const char *)data);
	}
	return FNV1A_Hash_Yoshimura((const char *)data, l);
#endif
}
u_boolean_t PUBLIC_IF path_equal(void *a, void *b)
{
	char *as = (char *)a;
	char *bs = (char *)b;
	int		r;

	/*
 	 * REMARK!) we have to consider to use strcasecmp.
	 *	Some customer assumes case-insensitive file name.
 	 */	
	if (!a || !b) 
		return 0;
	r = strcmp(as, bs) == 0;
	return r;
}
long long
sys_gettickcount()
{
	long long v;
	struct timeval 	ts;

	gettimeofday(&ts, 0);
	v =  (long long)((unsigned long long)ts.tv_sec * 1000LL + (ts.tv_usec / 1000));
	return v;

}





#ifndef WIN32
void
print_trace (void)
{
       void *array[100];
       size_t size;
       char **strings;
       size_t i;
	   char   *tbuf = XMALLOC(2048, AI_ETC);;
	   char   *t = tbuf;
	   int    n, remained=2048;
     
       size = backtrace (array, 20);
       strings = backtrace_symbols (array, size);
     
       n = snprintf(t, remained,  "Current statck dump: %zd stack frames.\n", size);
	   remained -= n;
	   t += n;
     
       for (i = 0; i < size; i++) {
          n = snprintf(t, remained, "%s\n", strings[i]);
		  remained -= n;
		  t += n;
	   } 
	   *t = 0;
	   TRACE((T_ERROR, tbuf));
       free (strings);
	   XFREE(tbuf);
}
#endif

int
report_error_syslog(char *name, char *fmt, ...)
{
#ifdef LOG_USER
	static int 	__init_syslog = 0;
	if (!__init_syslog) {
		__init_syslog++;
		openlog(name, LOG_PID, LOG_USER);
	}
	va_list 	ap;
	va_start(ap, fmt);
	vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);
#endif
	return 0;
}


void
clrtid()
{
}

pid_t
gettid()
{
	return (nc_int64_t)syscall(__NR_gettid);
}


int
pthread_is_same(pthread_t t1, pthread_t t2)
{
	return !memcmp(&t1, &t2, sizeof(pthread_t));
}


#ifndef HAVE_WRITEV

#ifdef WIN32
ssize_t 
pwritev(HANDLE fd, const struct iovec *iov, int iovcnt, off_t offset)
#else
ssize_t 
pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
#endif
{
	ssize_t	nt = 0;

#ifndef WIN32
#if 0
	int 	i;
	ssize_t	nr;
	for (i = 0; i < iovcnt; i++) {
		nr = pwrite(fd, iov[i].iov_base,  iov[i].iov_len, offset);
		if (nr != iov[i].iov_len) {
			TRACE((T_WARN, "pwrite(FD[%d], offset=%lld, len=%%ld) = %d, errno=%d\n", fd, (long long)offset, (long)iov[i].iov_len, nr, errno));
			errno = EIO;
			nt = -1;
			break;
		}
		nt 		+= nr;
		offset 	+= nr;
	}
#else 
	/* using readv */
	lseek(fd, offset, 0);
	nt = writev(fd, iov, iovcnt);
#endif

#else

#endif
	return nt;
}


#endif


#ifndef HAVE_PREADV

#ifdef WIN32
ssize_t 
preadv(HANDLE fd, const struct iovec *iov, int iovcnt, off_t offset)
#else
ssize_t 
preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
#endif
{

	ssize_t	nt = 0;
#ifndef WIN32
#if 0
	int 	i;
	ssize_t	nr;
	for (i = 0; i < iovcnt; i++) {
		nr = pread(fd, iov[i].iov_base,  iov[i].iov_len, offset);
		if (nr != iov[i].iov_len) {
			TRACE((T_WARN, "pread(FD[%d], offset=%lld, len=%%ld) = %d, errno=%d\n", fd, (long long)offset, (long)iov[i].iov_len, nr, errno));
			errno = EIO;
			nt = -1;
			break;
		}
		nt 		+= nr;
		offset 	+= nr;
	}
#else
	lseek(fd, offset, 0);
	nt = readv(fd, iov, iovcnt);
#endif
#else

#endif
	return nt;
}
#endif


#ifdef WIN32
char * 
nc_lasterror(char *buf, int len) 
{ 
	char 	*p;

    LPVOID lpMsgBuf;
    LPVOID lpDisplayBuf;
    DWORD dw = GetLastError(); 

    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &lpMsgBuf,
        0, NULL );

	p = strchr(lpMsgBuf, '\n');
	if (p) *p = 0;
	p = strchr(lpMsgBuf, '\r');
	if (p) *p = 0;
	snprintf(buf, len, "%ld:%s", dw, lpMsgBuf);
	LocalFree(lpMsgBuf);
	return buf;
}
char *  PUBLIC_IF
nc_strerror(DWORD dw, char *buf, int len) 
{ 

    LPVOID lpMsgBuf;
    LPVOID lpDisplayBuf;
	char   *p = NULL;

    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &lpMsgBuf,
        0, NULL );
	snprintf(buf, len, "ECODE[%ld]['%s']", dw, lpMsgBuf);
	p = strchr(buf, '\n');
	if (p) *p = 0;
	p = strchr(buf, '\r');
	if (p) *p = 0;


	LocalFree(lpMsgBuf);
	return buf;
}
#else
char *  PUBLIC_IF
nc_strerror(int eno, char *buf, int len) 
{ 

	char 		ebuf[128];
	
	snprintf(buf, len, "%d:%s", eno, strerror_r(eno, ebuf, sizeof(ebuf)));
	return buf;
}

/* CHG 2018-04-11 huibong 삭제 처리 (#32013) */
/*
char * 
nc_lasterror(char *buf, int len)
{
	int		n;
	char	*pfirst = buf;
	n = snprintf(buf, len, "ECODE[%d]", errno);
	buf += n;
	strerror_r(errno, buf, len -n);
	return pfirst;
}
*/


#endif
static char	__crc_tbl[256];
static int	__crc_init = 0;
#define		WIDTH	(8*sizeof(char))
#define		TOPBIT	(1 << (WIDTH -1))
#define		POLYNOMIAL	0xD8
void
crc_init()
{
	unsigned char	rem, bit;
	int		i;


	for ( i = 0; i < 256; i++) 
	{
		rem = i << (WIDTH - 8);
	
		for (bit = 8; bit > 0; bit--) 
		{
				if (rem & TOPBIT)
					rem = (rem << 1) ^ POLYNOMIAL;
				else
					rem = (rem << 1);
		}
		__crc_tbl[i] = rem;
	}
	__crc_init++;
}
#ifndef HAVE_LOCALTIME_R 
pthread_mutex_t 	__localtime_lock = PTHREAD_MUTEX_INITIALIZER;
struct tm * PUBLIC_IF
localtime_r(const time_t *timep, struct tm *result)
{
	struct tm *t = NULL;
	pthread_mutex_lock(&__localtime_lock);
	t = localtime(timep);
	memcpy(result, t, sizeof (struct tm));
	pthread_mutex_unlock(&__localtime_lock);
	return result;
}
#endif
char
make_crc(char *buf, int len)
{
	register int			i;
	unsigned char			crc = 0, rem = 0;

	for (i = 0; i < len; i++) {
		crc = buf[i] ^ (rem >> (WIDTH - 8));
		rem = __crc_tbl[crc] ^ (rem << 8);
	}
	return rem;
}
char *
filetime(char *buf, int len, time_t t)
{
	struct tm 		tm_t;
	time_t 			st = t;

	localtime_r(&st, &tm_t);
	snprintf(buf , len - 1, 
				"%04d/%02d/%02d %02d:%02d:%02d",
                	tm_t.tm_year+1900, tm_t.tm_mon+1, tm_t.tm_mday, 
					tm_t.tm_hour, tm_t.tm_min, tm_t.tm_sec);
	return buf;
}



void * PUBLIC_IF
nc_aligned_malloc(nc_size_t size, int alignment, int category, const char *f, int l)
{
	void 	*aligned_ptr = NULL;
	
#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif

	aligned_ptr = memalign(alignment, size);
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif

	if (aligned_ptr == NULL) {
		TRACE((T_ERROR, "CRITICAL! - failed in allocatiion(align=%d, size=%lld)\n", alignment, size));
	}
#ifndef NC_RELEASE_BUILD
	else {
		memset(aligned_ptr, 0, size);
	}
#endif


#ifdef NC_MEMLEAK_CHECK
        pthread_mutex_lock(&s__md_lock);
        if (s__page_hash == NULL) {
                s__page_hash = (u_hash_table_t *)u_ht_table_new(u_ht_direct_hash, u_ht_direct_equal, NULL, NULL);
        }

        if (s__page_hash) {
				/*
				 * aligned memory 앞 부분에 meminfo를 할당할 수 없으므로,
				 * 별도의 메모리를 추가할당하고 그걸 aligned메모리의 아바타
				 * 로 tacking 정보에 등록되도록 함
				 */
                void      *p = (void *)__nc_malloc(1 /* 1byte*/, category, f, l);
				GET_MP(mp, p);

				DEBUG_ASSERT(p != NULL);
				__nc_update_info(mp, size, U_TRUE, f, l); /* 실제 aligned 메모리 크기로 변경해둬야함 */
                u_ht_insert(s__page_hash, aligned_ptr, p, U_FALSE);

        }
        pthread_mutex_unlock(&s__md_lock);
        nc_add_dm(0, size);
#endif
	
	return aligned_ptr;
}
void * PUBLIC_IF
nc_aligned_realloc(void *p, nc_size_t old_size, nc_size_t size, int alignment, int category, const char *f, int l)
{
	
	void *aligned_ptr = NULL;

	aligned_ptr = nc_aligned_malloc(size, alignment, category, f, l);
	memcpy(aligned_ptr, p, old_size);
	nc_aligned_free(p);
	p = aligned_ptr;

	
	return p;
}
char *
time2dp(char *buf, int buflen, nc_time_t secs)
{
#define		ONEMIN		(60)
#define		ONEHOUR		(60*ONEMIN)
#define		ONEDAY		(24*ONEHOUR)

	int				day = 0;
	int				hour = 0;
	int				mins = 0;
	char			*p = buf;
	int				n, remained = buflen;


	day		= secs / ONEDAY; secs = secs - day*ONEDAY;
	hour	= secs / ONEHOUR; secs = secs - hour*ONEHOUR;
	mins	= secs / ONEMIN; secs = secs - mins*ONEMIN;

	if (day > 0) {
		n = snprintf(p, remained, "%d day%s", day, (day > 1)?"s ":" ");
		p += n;
		remained -= n;
	}
	if (hour > 0) {
		n = snprintf(p, remained, "%d hour%s", hour, (hour > 1)?"s ":" ");
		p += n;
		remained -= n;
	}
	if (mins > 0) {
		n = snprintf(p, remained, "%d min%s", mins, (mins > 1)?"s ":" ");
		p += n;
		remained -= n;
	}
	n = snprintf(p, remained, "%d sec%s", secs, (secs > 1)?"s ":" ");
	p += n;
	remained -= n;
	*p = 0;
	return buf;

}
char *
ll2dp(char *buf, int buf_len, long long n)
{
	char 	*buf_org = buf;
	char 	tbuf[128], *tp;
	int 	l; //, i;
	int 	lt1000;
	int 	mt1000;
	int 	ltemit = 0;
	int 	remained=buf_len;

	l = snprintf(tbuf, sizeof(tbuf) -1, "%lld", n);
	lt1000 = l % 3;
	mt1000 = l - lt1000;
	tp = tbuf;
	if (lt1000> 0) {
		strncpy(buf, tp, lt1000);
		ltemit++;
		buf = buf + lt1000;
		tp  = tbuf + lt1000;
		remained -= lt1000;
	}
	while (mt1000 > 0 && remained > 0) {
		if (ltemit) {
			*buf = ',';
			buf += 1;
			remained -= 1;
		}
		strncpy(buf, tp, 3);
		buf += 3;
		tp += 3;
		remained -= 3;
		mt1000 -= 3;
		ltemit ++;
	}
	*buf = 0;
	return buf_org;
}
void PUBLIC_IF
nc_aligned_free(void *ptr_aligned)
{
	if (ptr_aligned) {
#ifdef NC_MEMLEAK_CHECK
		void			*ap = NULL;

		pthread_mutex_lock(&s__md_lock);
		ap = (void *)u_ht_lookup(s__page_hash, (void *)ptr_aligned);
		DEBUG_ASSERT(ap != NULL);
		u_ht_remove(s__page_hash, (void *)ptr_aligned);
		{
			/*
			 * leak 추적을 위해서 avatar로 할당했던 메모리 쪼가리 삭제
			 */
			GET_MP(mp, ap);

			nc_sub_dm(0, mp->length); /* aligned memory 크기만큼 빼줘야함(mp->length) */

			__nc_free(ap, __FILE__, __LINE__);
		}
		pthread_mutex_unlock(&s__md_lock);
		free(ptr_aligned); /* 실제 aligned memory block free */
#else
		free(ptr_aligned);
#endif
	}
}
void
dump_bits(unsigned long *bitmap, int bitlen, char *buf, int buflen, int *vbits)
{
	int			i;
	int			n, wr = 0;
	char		*p= buf;
	int			sb=-1,  cb = -1;
	int			cnt = 0;
	int 		tt;



	TRACE((T_DEBUG, "bit count=%d\n", bitlen));
	if (vbits) *vbits = 0;
	if (buf) *buf = 0;
	if (!bitmap) {
		return;
	}


	for (i = 0; i < bitlen; i++) {
		tt = test_bit(i, bitmap) ;
		if (tt) {
			if (sb < 0) sb = i;
			cb = i;
			cnt++;
			if (vbits) (*vbits)++;
		}
		else {
			if (cnt > 0) {
				if (cnt == 1) {
					n = snprintf(p, buflen - wr, "(%d) ", sb);
				}
				else {
					n = snprintf(p, buflen - wr, "(%d~%d) ", sb, cb);
				}
				p+= n;
				wr += n;
			}
			cb = -1;
			sb = -1;
			cnt = 0;
		}
	}
	TRACE((T_DEBUG, "sb[%d],cb[%d],cnt[%d]\n", sb, cb,cnt));
	if (sb >= 0 && cnt > 0) {
		n = snprintf(p, buflen - wr, "(%d~%d) ", sb, cb);
		p += n;
	}
	*p = 0;
}


time_t
nc_cached_clock()
{
	time_t 	c;
	pthread_rwlock_rdlock(&__nc_clock_lock);
	c = __nc_clock;
	pthread_rwlock_unlock(&__nc_clock_lock);
	return c;
}
void
nc_update_cached_clock()
{
	
	pthread_rwlock_wrlock(&__nc_clock_lock);
	__nc_clock = time(NULL);
	pthread_rwlock_unlock(&__nc_clock_lock);
}
char  *
nc_time2string(time_t *t, char *buf,int len)
{
	struct tm	lt;
	localtime_r(t, &lt);
	strftime(buf, len, "%Y-%m-%d %H:%M:%S", &lt);
	return buf;
}

/*
 * resource lekage management module
 */
typedef struct {
	void 	*handle;
	char	file[64];
	char 	type[8];
	int		lno;
} nc_resource_t;
static u_hash_table_t 	*__resources = NULL;
static pthread_mutex_t   __resource_lock = PTHREAD_MUTEX_INITIALIZER;
PUBLIC_IF void
nc_add_handle(void *handle, const char *type, const char *f, int l)
{
	int 			r;
	nc_resource_t 	*n;
	pthread_mutex_lock(&__resource_lock);
	if (__resources == NULL) {
		__resources = u_ht_table_new(u_ht_direct_hash, u_ht_direct_equal, NULL, NULL /*fc_inode_open_destructor*/);
	}
	n = (nc_resource_t *)XMALLOC(sizeof(nc_resource_t), AI_ETC);
	n->handle = handle;
	strcpy(n->file, f);
	strcpy(n->type, type);
	n->lno = l;

	r = u_ht_insert(__resources, (void *)handle, (void *)n, U_TRUE);

	pthread_mutex_unlock(&__resource_lock);
	if (!r) {
		n = u_ht_lookup(__resources, handle);
		TRACE((T_ERROR, "resouce, %p already in resource table at %d@%s(called %d@s)\n",
			handle, n->lno, n->file, l, f));
	}
}
PUBLIC_IF int
nc_del_handle(void *handle, const char *f, int l)
{
	u_boolean_t 	r;
	pthread_mutex_lock(&__resource_lock);
	r = u_ht_remove(__resources, handle);
	pthread_mutex_unlock(&__resource_lock);
	if (!r) {
		TRACE((T_ERROR, "resouce, %p not found in resource table at %d@%s\n",
			handle, l, f));
	}
	return 0;
}
PUBLIC_IF int
nc_pthread_mutex_init(pthread_mutex_t * restrict mutex,  const pthread_mutexattr_t  *restrict attr, const char *file, int lno)
{
	int 	r;

	r = pthread_mutex_init(mutex, attr);
	if (r) {
		TRACE((T_ERROR, "pthread_mutex_init - failed at %d@%s\n", lno, file));
	}
	else {
		nc_add_handle(mutex, "MTX", file, lno);
	}
	return r;
}
PUBLIC_IF int
nc_pthread_mutex_destroy(pthread_mutex_t *mutex, const char *file, int lno)
{
	int 	r;

	r = pthread_mutex_destroy(mutex);
	if (!r) {
		nc_del_handle(mutex, file, lno);
	}
	else {
		/* destroy error */
		TRACE((T_ERROR, "pthread_mutex_destroy - failed at %d@s\n", lno, file));
	}
	return r;
}
PUBLIC_IF int
nc_pthread_cond_init(pthread_cond_t * restrict cond,  const pthread_condattr_t  *restrict attr, const char *file, int lno)
{
	int 	r;

	r = pthread_cond_init(cond, attr);
	if (r) {
		TRACE((T_ERROR, "pthread_cond_init - failed at %d@%s\n", lno, file));
	}
	else {
		nc_add_handle(cond, "CON", file, lno);
	}
	return r;
}
PUBLIC_IF int
nc_pthread_cond_destroy(pthread_cond_t *cond, const char *file, int lno)
{
	int 	r;

	r = pthread_cond_destroy(cond);
	if (!r) {
		nc_del_handle(cond, file, lno);
	}
	else {
		/* destroy error */
		TRACE((T_ERROR, "pthread_cond_destroy - failed at %d@s\n", lno, file));
	}
	return r;
}
static int
dump_resource(void *k, void *v, void *ud)
{
	nc_resource_t 	*nv = (nc_resource_t *)v;
	TRACE((T_WARN, "[%s] %p at %d@%s\n", nv->type, nv, nv->lno, nv->file));
	return 0;
}
PUBLIC_IF void
nc_dump_resources()
{
	u_ht_foreach(__resources,	
				 dump_resource,
				 NULL);
}



/*
 * 
 */
void
block_signal()
{

	int 		i;
	sigset_t	newmask;
	sigemptyset(&newmask);

	for (i = SIGHUP; i < _NSIG; i++) {
		if ( (i == SIGSEGV) ||
			 (i == SIGINT) ||
			 (i == SIGKILL)
			 )
			continue;
		sigaddset(&newmask, i);
	}
	pthread_sigmask(SIG_BLOCK, &newmask, NULL);
}
void
block_signal_2(int sigblock)
{
#ifndef WIN32
	sigset_t	newmask;

	sigemptyset(&newmask);

#ifdef SIGPIPE
	sigaddset(&newmask, SIGPIPE);
#endif
#ifdef SIGALRM
	sigaddset(&newmask, SIGALRM);
#endif

	if (sigblock)
		pthread_sigmask(SIG_BLOCK, &newmask, NULL);
	else
		pthread_sigmask(SIG_UNBLOCK, &newmask, NULL);
#endif
}




#ifdef WIN32
PUBLIC_IF CALLSYNTAX char *
strerror_r(int eno, char *buf, int len)
{
	snprintf(buf, len-1, "STDERR:%d (no string data)",  eno);
}
#endif
char *
uuid2string(uuid_t uuid, char *buf, int l)
{
	int				i;
	char			*p = buf;
	nc_uint8_t		*puuid = (nc_uint8_t *)uuid;
	int				r;

	for (i = 0; i < sizeof(uuid_t); i++) {
		r = sprintf(p, "%02x", (puuid[i]&0xFF));
		p += r;
	}
	*p = 0;

	return buf;
}





nc_uint8_t
do_crc(char *d, int len)
{
	int 	 		real_blk_len = 0;
	unsigned long	xor_v = 0;
	unsigned char	xor_c = 0;
	int 			remained = 0;
	int 			off = 0;

	
	remained = real_blk_len = len;
	for (off = 0; remained > (int)sizeof(unsigned long); ) {
		xor_v 		= xor_v ^ *(unsigned long *)(d + off);
		off 		+= sizeof(unsigned long);
		remained 	-= sizeof(unsigned long);
	}
	for (; remained > 0; ) {
		xor_v 		= xor_v ^ *(unsigned char *)(d + off);
		off 		+= 1;
		remained 	-= 1;
	}
	xor_c = xor_v ^ ((xor_v >> 8) & 0xFF) ^ ((xor_v >> 16) & 0xFF) ^ ((xor_v >> 24) & 0xFF);
	
	return xor_c;
}



nc_uint32_t
get_random()
{
	nc_uint32_t 	r = 0;
#ifdef WIN32
	rand_s(&r);
#else
	int 			fd;

	fd = open("/dev/urandom", O_RDONLY);
	read(fd, &r, sizeof(r));
	close(fd);
#endif
	return r;
}

int
nr_cpu()
{
	int 	nprocs = 4;
#ifdef _WIN32
#ifndef _SC_NPROCESSORS_ONLN
	SYSTEM_INFO info;
	GetSystemInfo(&info);
#define sysconf(a) info.dwNumberOfProcessors
#define _SC_NPROCESSORS_ONLN
#endif
#endif
#ifdef _SC_NPROCESSORS_ONLN
	nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (nprocs < 1) {
		TRACE((T_ERROR, "nr_cpu() - could not determine # of cpus( default=4)\n"));
		nprocs = 4;
		goto L_nr_done;
	}
#else
	TRACE(T_ERROR, "Could not determine number of CPUs");
#endif
L_nr_done:
	return nprocs;
}

char *
trim_string(char *str)
{
	char 	*end;
	while (isspace(*str)) str++;
	if (*str == 0) return str;
	end = str + strlen(str) -1;
	while (end > str && isspace(*end)) end--;
	*(end+1) = 0;
	return str;
}
void
make_timeout( struct timespec *to, long msec, clockid_t cid)
{
	int 	ov = 0;

	if (cid == CLOCK_MONOTONIC) {
		clock_gettime(CLOCK_MONOTONIC_COARSE, to);
	}
	else {
		clock_gettime(CLOCK_REALTIME, to);
	}
	to->tv_nsec  += (long long)msec*1000000LL;
	if (to->tv_nsec > 1000000000LL) {
		ov 			= to->tv_nsec/1000000000LL;
		to->tv_sec 	+= ov;
		to->tv_nsec	= to->tv_nsec - ov*1000000000LL;
	}
}

/*
 * 4-byte  정렬을 기본으로, 첫 부분에 길이 저장
 */
char *
vstring_pack(char *ptr, int capa, char *str, int len)
{
	int lo = len;

	if (lo < 0) {
		/*
		 * null-terminated string, calc the length
		 */
		lo = len = strlen(str) + 1; /* NULL 포함 길이*/
	}
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(capa >= (lo + sizeof(nc_uint32_t)));
#endif
	/*
	 * pack-data의 크기 (4 bytes 단위정수배로 맞춰진 크기
	 */
	len = (nc_uint32_t)NC_ALIGNED_SIZE(len, sizeof(nc_uint32_t));
	*(nc_uint32_t *)ptr = len;
	ptr += sizeof(nc_uint32_t);


	/*
	 * str 에서 부터 NULL 문자까지 포함 복사
	 */
	memcpy(ptr, str, lo);

	return (ptr + len); 
}
char *
vstring_unpack(char *ptr, char **str, int *len)
{

	*len 	= *(nc_uint32_t *)ptr;
	ptr 	+= sizeof(nc_uint32_t); /* length만큼 건너뛰기 */

	if (str) {
		*str = (char *)XMALLOC(*len, AI_ETC);
		memcpy(*str, ptr, *len);
	}

	return (ptr + *len); 
}

int
vstring_size2(int len)
{

	len = NC_ALIGNED_SIZE(len, sizeof(nc_uint32_t));
	len += sizeof(nc_uint32_t); 

	return len;
}
int
vstring_size(char *ptr)
{
	int 	len = strlen(ptr) + 1;

	len = NC_ALIGNED_SIZE(len, sizeof(nc_uint32_t));
	len += sizeof(nc_uint32_t); 
	return len;
}
/*
 * !!IMPORTANT!!!
 * 	0 : complete and signalled
 * 	otherwise : error (see errno.h)
 */
int
condition_wait(pthread_mutex_t *lock, pthread_cond_t *cond, long msec, int (*completed)(void *), void *cb, int forever)
{
	struct 		timespec ts 	= {0,0};
	int 		signalled  		= 0;
	int 		r 	   			= ETIMEDOUT;



	signalled = (completed && (*completed)(cb));
	if (!signalled) {
		make_timeout(&ts, msec, CLOCK_MONOTONIC);
		r = pthread_cond_timedwait(cond, lock, &ts);
		if (r == 0) {
			if (!completed) 
				signalled = 1;
			else
				signalled = (completed && (*completed)(cb));
		}
	}

	return (signalled?0:r);
}
void
condition_init(pthread_cond_t *cond)
{
	pthread_condattr_t 		ca;

	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(cond, &ca); 
	pthread_condattr_destroy(&ca);
}

struct my_thread_info {
	void *	arg;
	void * (*pthread_func)(void *);
};
static void *
__thread_wrapper(void *u)
{
	void 	*arg, *r;
	void * (*func)(void *);
	struct my_thread_info *ti = (struct my_thread_info *)u;

	block_signal();
	arg = ti->arg;
	func = ti->pthread_func;
	XFREE(u);
	r = func(arg);
	return r;
}
int my_pthread_create(pthread_t *restrict thread,
	                 const pthread_attr_t *restrict attr,
					               void *(*start_routine)(void*), void *restrict arg)
{
	struct my_thread_info *ti = (struct my_thread_info *)XMALLOC(sizeof(struct my_thread_info), AI_ETC);
	ti->pthread_func = start_routine;
	ti->arg = arg;
	return pthread_create(thread, attr, __thread_wrapper, ti);
}

#define _hash_rotl(x, n) (((x) << (n)) | ((x) >> (32-(n))))

nc_uint32_t 
FNV1A_Hash_Yoshimura(const char *str, nc_uint32_t wrdlen)
{
	const nc_uint32_t PRIME = 709607;
	nc_uint32_t hash32 = 2166136261;
	nc_uint32_t hash32B = 2166136261;
	const char *p = str;
	nc_uint32_t Loop_Counter;
	nc_uint32_t Second_Line_Offset;

	if (wrdlen >= 2 * 2 * sizeof(nc_uint32_t)) {
		Second_Line_Offset = wrdlen - ((wrdlen >> 4) + 1) * (2 * 4);	// ((wrdlen>>1)>>3)
		Loop_Counter = (wrdlen >> 4);
		Loop_Counter++;
		for (; Loop_Counter; Loop_Counter--, p += 2 * sizeof(nc_uint32_t)) {
			hash32 =
				(hash32 ^
				 (_hash_rotl(*(nc_uint32_t *) (p + 0), 5) ^
				  *(nc_uint32_t *) (p + 0 + Second_Line_Offset))) * PRIME;
			hash32B =
				(hash32B ^
				 (_hash_rotl(*(nc_uint32_t *) (p + 4 + Second_Line_Offset), 5) ^
				  *(nc_uint32_t *) (p + 4))) * PRIME;
		}
	} else {
		if (wrdlen & 2 * sizeof(nc_uint32_t)) {
			hash32 = (hash32 ^ *(nc_uint32_t *) (p + 0)) * PRIME;
			hash32B = (hash32B ^ *(nc_uint32_t *) (p + 4)) * PRIME;
			p += 4 * sizeof(uint16_t);
		}
		if (wrdlen & sizeof(nc_uint32_t)) {
			hash32 = (hash32 ^ *(uint16_t *) (p + 0)) * PRIME;
			hash32B = (hash32B ^ *(uint16_t *) (p + 2)) * PRIME;
			p += 2 * sizeof(uint16_t);
		}
		if (wrdlen & sizeof(uint16_t)) {
			hash32 = (hash32 ^ *(uint16_t *) p) * PRIME;
			p += sizeof(uint16_t);
		}
		if (wrdlen & 1)
			hash32 = (hash32 ^ *p) * PRIME;
	}
	hash32 = (hash32 ^ _hash_rotl(hash32B, 5)) * PRIME;
	return hash32 ^ (hash32 >> 16);
}

static inline void *
__nc_set_info(__meminfo_t*mp, nc_uint32_t l, int category, int aligned, const char *sf, int sl)
{

	mp->magic 		= __MEM_MAGIC;
	mp->length 		= l;
	mp->category 	= category;

#ifdef NC_MEM_DEBUG

#if defined(NC_MEMLEAK_CHECK)
	mp->s_file 		= sf; /* SHOULD BE a source file name */
	mp->s_line 		= sl;
#endif

	nc_add_dm(category, l);
	ai_add(category, l);

#if defined(NC_MEMLEAK_CHECK)
	INIT_NODE(&mp->node);
	mp->aligned 	= aligned;
	pthread_mutex_lock(&__dm_lock);
	link_append(&__dm_list, mp, &mp->node);
	pthread_mutex_unlock(&__dm_lock);
#endif

#ifdef NC_OVERFLOW_CHECK
#if defined(NC_MEMLEAK_CHECK)
	if (!mp->aligned)
#endif
		__nc_set_barrier(mp);
	XVERIFY(GET_MP2UP(mp));
#endif

#endif /* end of memdebug*/


	return (void *)GET_MP2UP(mp);
}




#ifdef NC_MEM_DEBUG



#ifdef 	NC_OVERFLOW_CHECK
static inline void
__nc_set_barrier(__meminfo_t *mp)
{
	mp->barrier = (nc_uint32_t *)((char *)mp + sizeof(__meminfo_t) + NC_ALLOC_ALIGN(mp->length));
	(*mp->barrier) = ~__MEM_MAGIC;
}
static inline int
__nc_check_overflow(__meminfo_t *mp)
{

	return (*mp->barrier == ~__MEM_MAGIC); 
}
#endif


static void
__nc_update_info(__meminfo_t *mp, size_t length, int aligned, const char *sf, int sl)
{

#if defined(NC_MEMLEAK_CHECK)
	mp->s_file = sf;
	mp->s_line = sl;
#endif
	nc_sub_dm(mp->category, mp->length);
	ai_sub(mp->category, mp->length);
	mp->length = length;
	if (sl == 1881) {
		TRACE((T_ERROR, "%p - updateinfo at %d%s\n", mp, sl, sf));
	}
	nc_add_dm(mp->category, length);
	ai_add(mp->category, length);
#if defined(NC_MEMLEAK_CHECK)
	mp->aligned = aligned;
#endif

	return;
}
static inline int
__nc_check_magic(void *p)
{
	GET_MP(mp, p);
	return (mp->magic == __MEM_MAGIC); 
}
static inline nc_uint32_t
__nc_mark_free(__meminfo_t *mp, const char *f, int l)
{
	DEBUG_ASSERT(_ATOMIC_CAS(mp->magic, __MEM_MAGIC, __MEM_MAGIC_FREE));
#if defined(NC_MEMLEAK_CHECK)
	pthread_mutex_lock(&__dm_lock);
	link_del(&__dm_list, &mp->node);
	pthread_mutex_unlock(&__dm_lock);
	INIT_NODE(&mp->node);
	mp->s_file = NULL;
#endif

	return mp->length;
}
#endif /* NC_MEM_DEBUG END*/

#ifdef NC_MEM_DEBUG
nc_uint32_t
__nc_get_category(void *p)
{
	GET_MP(mp, p);
	return mp->category;
}
#endif

nc_uint32_t
__nc_get_len(void *p)
{
#ifdef NC_MEM_DEBUG
	if (__nc_check_magic(p))  {
		GET_MP(mp, p);
		return mp->length;
	}
	return 0;
#else
	GET_MP(mp, p);
	return mp->length;
#endif
}

#pragma GCC push_options
#pragma GCC optimize ("O0")
/*
 * remark:
 * 	nc_malloc에서 calloc을 이용하는 이유는
 *  calloc의 경우 메모리 할당과 함께 할당된 메모리를
 *  0으로 초기화함
 */

PUBLIC_IF CALLSYNTAX void *
__nc_malloc(size_t n, int category, const char *sf, int sl)
{
	void 			*ap = NULL;

#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif

	size_t 			nsz = NC_ALLOC_ALIGN(n+__MEM_MAGIC_SIZE + NC_MEM_PADDING+1);
	__meminfo_t 	*mp = NULL;


	DEBUG_ASSERT(nsz > 0);

	 
	mp = (__meminfo_t *)calloc(1, nsz);
	DEBUG_ASSERT(mp != NULL);

	ap =	__nc_set_info(mp, n, category, U_FALSE/* not aligned alloc */, sf, sl);

#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif

	return ap;

}


PUBLIC_IF CALLSYNTAX void *
__nc_calloc(size_t elcount, size_t elsize, int category, const char *sf, int sl)
{
	void 	*p;
#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif
	p = __nc_malloc(elcount*elsize, category, sf, sl);
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif

	return p;
}


PUBLIC_IF CALLSYNTAX int
nc_check_memory(void *memp, size_t sz)
{
#ifdef NC_MEM_DEBUG
	int 	r;
	
	GET_MP(mp, memp);
	r = __nc_check_magic(memp); 

#ifdef NC_OVERFLOW_CHECK
	r = r && __nc_check_overflow(mp);
#endif /* overflow */

	return r;
#else


	return U_TRUE;
#endif
}


PUBLIC_IF CALLSYNTAX void
__nc_free(void *p, const char *file, int lno)
{

#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif

#ifdef NC_MEM_DEBUG
	size_t		ol;
	int 		category;

	if (p) 
	{
		GET_MP(mp, p);

		DEBUG_ASSERT(nc_check_memory(p, 0));

		ol 			= __nc_get_len(p);
		category 	= __nc_get_category(p);
		nc_sub_dm(0, ol);
		ai_sub(category, ol);

		__nc_mark_free(mp, file, lno);

		free(mp);
	}
#else
	GET_MP(mp, p);
	DEBUG_ASSERT(mp->magic == __MEM_MAGIC);
	free(mp);
#endif
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
}
PUBLIC_IF CALLSYNTAX void *
__nc_realloc(void * old_, size_t reqsiz, int category, const char *sf, int sl)
{
	void 			*new_; 
	nc_uint32_t 	olds = 0;

#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif

	new_ = (void *)__nc_malloc(reqsiz, category, sf, sl);
	if (old_) {
		olds = __nc_get_len(old_);
		memcpy(new_, old_, min(olds, reqsiz));
		__nc_free(old_, sf, sl);
	}

#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
	return new_;
}
char *
__nc_strdup(const char *s, int category, const char *sf, int sl)
{
	char *n = NULL;
	int	cl;
#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif

	cl = strlen(s) + 1;
	n = (char *)__nc_malloc(cl, category, sf, sl);
	memcpy(n, s, cl);

#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
	return n;
}
char *
__nc_strndup(const char *s, size_t n, int category, const char *sf, int sl)
{
	char *news = NULL;

	news = (char *)__nc_malloc(n+1, category, sf, sl);
	memcpy(news, s, n+1);
	return news;
}

#pragma GCC pop_options



#if 0
void *
__nc_memcpy(void *dst, void *src, size_t n, int check_overflow)
{
	return dst;
}
#endif /***** INCOMPLETE *****/


/*
 * 
 * ************************ H E A P     T R A C E      D U M P 
 *
 */

struct  tag_alloc_info {
        int					count;
        nc_uint32_t     	sum;
};

#if defined(NC_MEMLEAK_CHECK)
static int
A_LEAK(void *k, void *v, void *u)
{
		char					zsum[128];
        struct  tag_alloc_info  *ai = (struct tag_alloc_info *)v;

        TRACE((T_WARN, "%-60s %8d  %12s\n", (char *)k, ai->count, (char *)ll2dp(zsum, sizeof(zsum), (long long)ai->sum)));
        return HT_CONTINUE;
}
#endif

void
__nc_dump_heap(nc_uint16_t class)
{

#if defined(NC_MEMLEAK_CHECK)
	nc_uint64_t 				leak = 0;
	__meminfo_t					*mp;
	char						dbuf[128];
	char						key[1024];
	struct tag_alloc_info		*ai;
	u_hash_table_t  			*s__leaksum_tbl = NULL;

	TRACE((T_WARN, "****************************** MEMORY LEAK REPORT **********************************\n"));
	TRACE((T_WARN, "remark: memories allocated from some dynamically loaded libraries(like http driver) \n"));
	TRACE((T_WARN, "*intentionally* left not freed to take advantage of CPP's __FILE__ macro\n"));
	TRACE((T_WARN, "************************************************************************************\n"));
	pthread_mutex_lock(&__dm_lock);
	s__leaksum_tbl = (u_hash_table_t *)u_ht_table_new(path_hash, path_equal, free, free);

	mp = (__meminfo_t *)link_get_head_noremove(&__dm_list);
	while (mp) {
		if (IS_ON(class, (1 << mp->category))) {

			DEBUG_ASSERT(mp->length > 0);

			leak += mp->length;

			sprintf(key, "%d@%s", mp->s_line, mp->s_file);
			ai = (struct tag_alloc_info *)u_ht_lookup(s__leaksum_tbl, (void *)key);

			if (ai) {
				/* 
 				 * 동일 라인에 대한 leak entry 발견
				 */
				ai->count++;
				ai->sum += mp->length;
			}
			else {
				/*
				 * 해당 라인의 메모리 leak 처음 발견.
				 */
				ai = (struct tag_alloc_info *)malloc(sizeof(struct tag_alloc_info));
				DEBUG_ASSERT(ai != NULL);
				ai->count = 1;
				ai->sum = mp->length;
				u_ht_insert(s__leaksum_tbl, strdup(key), ai, U_TRUE);
			}
		}
		mp = link_get_next(&__dm_list, &mp->node);
	}
	TRACE((T_WARN, "********************** LEAK SUMMARY REPORT[class=%u] *********************\n", class));
	TRACE((T_WARN, "%-60s - %8s  %8s\n", "LINE", "COUNT", "SIZE(B)"));
	u_ht_foreach(s__leaksum_tbl, A_LEAK, NULL);
	TRACE((T_WARN, "********************** END OF REPORT ***************************\n"));
	u_ht_table_free(s__leaksum_tbl);
	pthread_mutex_unlock(&__dm_lock);
#endif	
}
#if 0
void
__nc_dump_heap_class(nc_uint16_t class)
{

#if defined(NC_MEMLEAK_CHECK)

	__meminfo_t 	*mp;
	pthread_mutex_lock(&__dm_lock);
	
	TRACE((T_WARN, "Dump heap allocations not yet freed:\n"));
	mp = link_get_head_noremove(&__dm_list);
	while (mp) {
		if (IS_ON(class, (1 << mp->category))) {
			TRACE((T_WARN, "memory %p : %d@%s size %ld/magic[%16X] not freed\n", (char *)mp + sizeof(__meminfo_t), mp->s_line, mp->s_file, mp->length, mp->magic));
		}

		mp = link_get_next(&__dm_list, &mp->node);
	}
	pthread_mutex_unlock(&__dm_lock);
#endif	
}
#endif
char *
hz_string(char *buf, int len, char *ibuf)
{
	int 	n = 0;
	int 	off = 0;
	int 	rl = 0;
	if (ibuf) {
		n = snprintf(buf, len, "%s", ibuf);
		if (SNPRINTF_OVERFLOW(len, n)) {
			off = max(0, len - 4);
			rl = min(len - off -1, 3);
			rl = max(0, rl);
			strncpy(&buf[off], "...", rl);
		}
	}
	else {
		buf[0]=0;
	}
	return buf;
}
char *
hz_bytes2string(char *buf, int len, nc_uint64_t val)
{
#define 	HZ_GIGA 	1000000000LL
#define 	HZ_MEGA 	1000000LL
#define 	HZ_KILO 	1000LL
	if (val > HZ_GIGA) {
		/* more than giga */
		snprintf(buf, len, "%.2fGB", (val*1.0)/HZ_GIGA);
	}
	else if (val > HZ_MEGA) {
		snprintf(buf, len, "%.2fMB", (val*1.0)/HZ_MEGA);
	}
	else if (val > HZ_KILO) {
		snprintf(buf, len, "%.2fKB", (val*1.0)/HZ_MEGA);
	}
	else {
		snprintf(buf, len, "%lluB", val);
	}
	return buf;
}
char *
tolowerz(char *istr)
{
	register char 	*nstr;
	register int 	ilen = strlen(istr);
	register int 	i;

	nstr = (char *)XMALLOC(ilen+1, AI_ETC);
	for (i  = 0; i < ilen; i++,istr++)
		nstr[i] = tolower(*istr);
	nstr[i] = '\0';
	return nstr;
}
#ifndef WIN32
int
linux_kernel_version(int *M, int *m, int *p)
{
	struct utsname 	utsn;
	uname(&utsn);
	sscanf(utsn.release, "%d.%d.%d", M, m, p);
	return 0;
}
#endif

int
_nl_lock(nc_lock_t *L, char *f, int l)
{
	int r;

	r = pthread_mutex_lock(&L->_lock);
	if (r == 0) {
#ifdef NC_MEASURE_MUTEX
		if (L->_locked == 0 && L->_lock.__data.__owner == trc_thread_self()) {
			L->_ml_stat = lookup_ml_stat(f, l);
			PROFILE_CHECK(L->_tml);
		}
#endif
		L->_locked++;
	}
	return r;
}

int
_nl_unlock(nc_lock_t *L)
{
	int 			r;
	//double 			msec;
	//perf_val_t		s, e;
	/*
	 *  이 함수가 호출됐다면 lock을 가지고 있음을 의미 
	 *
	 */
#ifdef NC_MEASURE_MUTEX
	if (L->_locked == 1 && L->_lock.__data.__owner == trc_thread_self()) {
		PROFILE_CHECK(e);

		msec = (double)(PROFILE_GAP_USEC(L->_tml, e)/1000.0);
		update_ml_stat(L->_ml_stat, msec);
	}
#endif
	L->_locked--;
	r = pthread_mutex_unlock(&L->_lock);
	DEBUG_ASSERT(r == 0);
	return r;
}
int
_nl_owned(nc_lock_t *l)
{
	int 	r = 0;
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(l->_magic == 0x5AA5A55A);
#endif
	r = ((l->_lock).__data.__owner == trc_thread_self());
	return r;

}

int
_nl_init(nc_lock_t *l, pthread_mutexattr_t *m)
{
	int 	r;
#ifndef NC_RELEASE_BUILD
	l->_magic = 0x5AA5A55A;
#endif
	l->_locked = 0;
	r = pthread_mutex_init(&l->_lock, m);
	return 0;
}
int
_nl_destroy(nc_lock_t *l)
{
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(l->_magic == 0x5AA5A55A);
#endif
	return pthread_mutex_destroy(&l->_lock);
}
pthread_mutex_t *
_nl_get_lock_raw(nc_lock_t *l)
{
	return &l->_lock;
}


int 
_nl_cond_init(nc_cond_t *cond)
{
	pthread_condattr_t		cattr;

	pthread_condattr_init(&cattr);
	pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
#ifndef NC_RELEASE_BUILD
	cond->_magic = 0x5AA5A55A;
#endif
	pthread_cond_init(&cond->_cond, &cattr);
	pthread_condattr_destroy(&cattr);
	return 0;
}
int 
_nl_cond_signal(nc_cond_t *cond)
{
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(cond->_magic == 0x5AA5A55A);
#endif
	return pthread_cond_signal(&cond->_cond);
}
int 
_nl_cond_broadcast(nc_cond_t *cond)
{
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(cond->_magic == 0x5AA5A55A);
#endif
	return pthread_cond_broadcast(&cond->_cond);
}
int 
_nl_cond_timedwait(nc_cond_t *cond, nc_lock_t *lock, const struct timespec *ts)
{
	int	r;
	/*
	 * lock을 소유하고 있는 상태
	 * cond_wait 호출시 자동으로 lock release됨
	 */

	DEBUG_ASSERT(lock->_magic == 0x5AA5A55A);
	DEBUG_ASSERT(cond->_magic == 0x5AA5A55A);
	r = pthread_cond_timedwait(&cond->_cond, &lock->_lock, ts);
	if (r != 0 && r != ETIMEDOUT) {
		TRACE((T_WARN, "pthread_cond_timedwait got %d\n", r));
	}
	/*
	 * lock 재획득 된 상태
	 */
	return r;
}
int 
_nl_cond_timedwait2(nc_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *ts)
{
	int	r;
	/*
	 * lock을 소유하고 있는 상태
	 * cond_wait 호출시 자동으로 lock release됨
	 */

#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(cond->_magic == 0x5AA5A55A);
#endif
	r = pthread_cond_timedwait(&cond->_cond, mutex, ts);
	/*
	 * lock 재획득 된 상태
	 */
	return r;
}
int
_nl_cond_wait(nc_cond_t *cond, nc_lock_t *lock)
{
	int	r;
	DEBUG_ASSERT(lock->_magic == 0x5AA5A55A);
	DEBUG_ASSERT(cond->_magic == 0x5AA5A55A);
	r = pthread_cond_wait(&cond->_cond, &lock->_lock);
	return r;
}
int 
_nl_cond_destroy(nc_cond_t *cond)
{
#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT(cond->_magic == 0x5AA5A55A);
	cond->_magic = ~cond->_magic;
#endif
	return pthread_cond_destroy(&cond->_cond);
}


int
_sync_file(char *src, char *dest)
{
	int 	sfd = -1, dfd = -1;	
	char 	buffer[512];
	char 	ebuffer[512];
	int 	ior, iow;
	int 	errored = 0;
	struct stat ss;
	struct timeval 	ut[2];


	sfd = open(src, O_RDONLY);
	dfd = open(dest, O_CREAT|O_TRUNC|O_WRONLY, 0644);
	if (sfd < 0 || dfd < 0) {
		return -1;
	}
	stat(src, &ss);


	while ((ior = read(sfd, buffer, sizeof(buffer))) > 0) {
		iow = write(dfd, buffer, ior);
		if (iow != ior) {
			TRACE((T_WARN, "_sync_file : error while synching to the destionation, '%s':%s\n", 
					dest, strerror_r(errno, ebuffer, sizeof(ebuffer))));
			errored++;
			break;
		}
	}
	close(sfd);
	close(dfd);
	if (!errored) {
		/*
		 * update file properties 
		 */
		memset(&ut, 0, sizeof(ut));
		ut[0].tv_sec 	= ss.st_atim.tv_sec;
		ut[1].tv_sec 	= ss.st_mtim.tv_sec;
		utimes(dest, ut);
	}
	return (errored?-1:0);
}

int
opq_valid(nc_opaque_t *o)
{
	/*
	 * trickky!@!!
	 */
	return (o && o->len == sizeof(void *) && ((o->fmagic ^ o->rmagic) == 0xFFFFFFFF));
}
void
opq_reset(nc_opaque_t *o)
{
	memset(o, 0, sizeof(nc_opaque_t));
}
void
opq_copy(nc_opaque_t *dest, nc_opaque_t *src, int reset)
{
	*dest = *src;
	if (reset) {
		opq_reset(src);
	}
}
void
opq_set(nc_opaque_t *dest, void *p)
{
	dest->uptr = p;
	dest->len = sizeof(p);
	dest->fmagic = 0x5AA5A55A;
	dest->rmagic = ~dest->fmagic;
}

 
apc_overlapped_t *
apc_overlapped_new(void *u, apc_completion_proc_t proc)
{
	apc_overlapped_t *e = NULL;

	e = (apc_overlapped_t *)XMALLOC(sizeof(apc_overlapped_t), AI_ETC);
 
	if (proc) {
		e->type = OT_CALLBACK;
		e->u.ucallback.func = proc;
		e->u.ucallback.ud 	= u;
	}
	else {
		e->type = OT_EVENT;
		condition_init(&e->u.uevent.signal);
		pthread_mutex_init(&e->u.uevent.lock, NULL);
		e->u.uevent.finished = 0;
	}
	e->fmagic = e->rmagic = 0x5AA5;
	return e;
}
void
apc_overlapped_reset(apc_overlapped_t *e)
{
	if (e->type == OT_EVENT)
		e->u.uevent.finished = 0;
}
void
apc_overlapped_switch(apc_overlapped_t *e, void *u, apc_completion_proc_t proc)
{

	int 	nt = (proc?OT_CALLBACK:OT_EVENT);


	if (e->type == OT_EVENT) {
		pthread_cond_destroy(&e->u.uevent.signal);
		pthread_mutex_destroy(&e->u.uevent.lock);
	}
	if (nt == OT_CALLBACK) {
		e->type 			= OT_CALLBACK;
		e->u.ucallback.func = proc;
		e->u.ucallback.ud 	= u;
	}
	else {
		e->type 			= OT_EVENT;
		condition_init(&e->u.uevent.signal);
		pthread_mutex_init(&e->u.uevent.lock, NULL);
		e->u.uevent.finished = 0;

	}
}
void
apc_overlapped_signal(apc_overlapped_t *e)
{
	DEBUG_ASSERT(e != NULL);
	if (e->type == OT_EVENT) {
		pthread_mutex_lock(&e->u.uevent.lock);
		DEBUG_ASSERT(e->u.uevent.finished == 0);
		e->u.uevent.finished = 1;
		pthread_cond_broadcast(&e->u.uevent.signal);
		pthread_mutex_unlock(&e->u.uevent.lock);
	}
	else {
		void 			*u;
		apc_completion_proc_t	func;
		DEBUG_ASSERT(e->u.ucallback.func != NULL);
		func = e->u.ucallback.func;
		u 	 = e->u.ucallback.ud;
		(*func)(u);
	}
}
int
apc_overlapped_wait(apc_overlapped_t *e)
{
	int 	r;
	struct timespec			ts;

	if (e->type != OT_EVENT)
		return -1;

	pthread_mutex_lock(&e->u.uevent.lock);
	while (e->u.uevent.finished == 0) {
		make_timeout( &ts, 1000, CLOCK_MONOTONIC);
		r = pthread_cond_timedwait(&e->u.uevent.signal, &e->u.uevent.lock, &ts); 
		DEBUG_ASSERT(r == 0 || r == ETIMEDOUT);
	}
	pthread_mutex_unlock(&e->u.uevent.lock);
	DEBUG_ASSERT(e->u.uevent.finished != 0);
	r = e->u.uevent.finished;
	e->u.uevent.finished = 0;
	return r;
}
void
apc_overlapped_destroy(apc_overlapped_t *e)
{
	DEBUG_ASSERT(e != NULL);

	e->fmagic = e->rmagic = 0xA11A;
	if (e->type == OT_EVENT) {
		pthread_cond_destroy(&e->u.uevent.signal);
		pthread_mutex_destroy(&e->u.uevent.lock);
	}
	XFREE(e);
}

char *
apc_overlapped_dump(apc_overlapped_t *e, char *buf, int len)
{
	int 		n, remained=len;
	char 		*p = buf;


	n = snprintf(p, len, "<%s:", (e->type == OT_EVENT)?"EVENT":"CALLBACK");
	p += n; len -= n;
	switch (e->type) {
		case OT_EVENT:
			n = snprintf(p, remained, "%d", e->u.uevent.finished);
			p += n; len -= n;
			break;
		case OT_CALLBACK:
			n = snprintf(p, remained, "%p", e->u.ucallback.ud);
			p += n; len -= n;
			break;
	}
	n = snprintf(p, remained, ">");
	*p = '\0';
	return buf;
}
char *
ai_dump(char *abuf, int len)
{
	int 	i, n;
	char	*p = abuf;
	int 	rem = len;
	char	ai_str[][36] = {
	"AI_ETC",
	"AI_DRIVER",
	"AI_DRIVERLIB",
	"AI_INODE",
	"AI_BLOCK",
	"AI_ASIO",
	"AI_VOLUME",
	"AI_OPENCTX",
	"AI_IOCTX",
	"AI_MDB"
	};
	nc_uint32_t 	cnt;
	nc_uint64_t 	amo;




	for (i = 0; i < AI_COUNT; i++) {
		cnt = _ATOMIC_ADD(__ai_category[i].count,0);
		amo = _ATOMIC_ADD(__ai_category[i].allocated,0);
		n = snprintf(p, rem, "%s[%.2fMB/%u] ", ai_str[i], (float)(amo/1000000.0), cnt);
		p += n;
		rem -= n;
	}
	*p = '\0';
	return abuf;
}
char *
mode_dump(char *mbuf, int len, nc_mode_t mode)
{

	snprintf(mbuf, len, "mode(%s%s%s%s%s%s%s%s)",
			(IS_ON(mode, O_NCX_PRIVATE)?"Priv ":""),
			(IS_ON(mode, O_NCX_DONTCHECK_ORIGIN)?"don'tchk_orign ":""),
			(IS_ON(mode, O_NCX_NORANDOM)?"NoRand ":""),
			(IS_ON(mode, O_NCX_REFRESH_STAT)?"RF_stat ":""),
			(IS_ON(mode, O_NCX_NOCACHE)?"Nocache ":""),
			(IS_ON(mode, O_NCX_MUST_REVAL)?"M_reval ":""),
			(IS_ON(mode, O_NCX_TRY_NORANDOM)?"Try_NoR ":""),
			(IS_ON(mode, O_NCX_MUST_EXPIRE)?"M_expire ":"")
			);
	return mbuf;
}
char *
obi_dump(char *buf, int len, nc_obitinfo_t *pobi)
{

	/* 26 bit fields */
	snprintf(buf, len, "bits(%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s)",
			  			(pobi->op_bits.chunked ==1?			"Chnk "	:""),
			  			(pobi->op_bits.complete ==1?		"Cmpl "	:""),
			  			(pobi->op_bits.created ==1?			"Created " 	:""),
						(pobi->op_bits.doc ==1?				"Doc " 	:""),
						(pobi->op_bits.frio_inprogress ==1?	"FRIO "	:""),
						(pobi->op_bits.memres ==1?			"Mem "	:""),
			  			(pobi->op_bits.mustreval ==1?		"MustEval "	:""),
						(pobi->op_bits.needoverwrite ==1?	"OverW "	:""),
						(pobi->op_bits.nocache ==1?			"NoCache "	:""),
						(pobi->op_bits.nostore ==1?			"NoStore "	:""),
						(pobi->op_bits.noxform ==1?			"NoXform "	:""),
						(pobi->op_bits.immutable ==1?		"Immu "	:""),
						(pobi->op_bits.cookie ==1?			"Cookie "	:""),
						(pobi->op_bits.preservecase ==1?	"Case "	:""),
			  			(pobi->op_bits.priv ==1?			"Priv "	:""),
						(pobi->op_bits.pseudoprop ==1?		"Psdo "	:""),
			  			(pobi->op_bits.rangeable ==1?		"Rangeable "	:""),
						(pobi->op_bits.refreshstat ==1?		"RStat "	:""),
			  			(pobi->op_bits.sizedecled ==1?		"SzDecl "	:""),
			  			(pobi->op_bits.sizeknown ==1?		"SzKnown "	:""),
						(pobi->op_bits.staled ==1?			"Staled "	:""),
						(pobi->op_bits.template ==1?		"Templ "	:""),
						(pobi->op_bits.upgraded ==1?		"Up "	:""),
						(pobi->op_bits.upgradable ==1?		"Upd "	:""),
						(pobi->op_bits.validuuid ==1?		"UUID "	:""),
			  			(pobi->op_bits.vary ==1?			"Vary "	:""),
			  			(pobi->op_bits.memres ==1?			"Mem "	:""),
						(pobi->op_bits.xfv ==1?				"XFV "	:"")
				);
	return buf;
}
char *
stat_dump(char *buf, int len, nc_stat_t *stat)
{
	int 			rem = len, n;
	char			*buforg = buf;
	char			zobi[256];


	n = snprintf(buf, rem, "Sz[%lld]", stat->st_size); 
	if (SNPRINTF_OVERFLOW(rem, n))  {
		SNPRINTF_WRAP(buf, rem) ;
		return buforg;
	}	
	rem -= n; buf+= n;


	n = snprintf(buf, rem, "MT[%u]", stat->st_mtime); 
	if (SNPRINTF_OVERFLOW(rem, n))  {
		SNPRINTF_WRAP(buf, rem) ;
		return buforg;
	}	
	rem -= n; buf+= n;

	n = snprintf(buf, rem, "VT[%u]", stat->st_vtime); 
	if (SNPRINTF_OVERFLOW(rem, n))  {
		SNPRINTF_WRAP(buf, rem) ;
		return buforg;
	}	
	rem -= n; buf+= n;

	if (isprint(stat->st_devid[0])) {
		n = snprintf(buf, rem, "DEV[%s]", stat->st_devid); 
		if (SNPRINTF_OVERFLOW(rem, n))  {
			SNPRINTF_WRAP(buf, rem) ;
			return buforg;
		}	
		rem -= n; buf+= n;
	}

	n = snprintf(buf, rem, "(%s)", obi_dump(zobi, sizeof(zobi), &stat->obi));

	if (SNPRINTF_OVERFLOW(rem, n))  {
		SNPRINTF_WRAP(buf, rem) ;
		return buforg;
	}	
	rem -= n; buf+= n;


	n = snprintf(buf, rem, "O.Code[%d]", stat->st_origincode); 
	if (SNPRINTF_OVERFLOW(rem, n))  {
		SNPRINTF_WRAP(buf, rem) ;
		return buforg;
	}	

	return buforg;
}


/*
 * get real memory usage (RSS)
 */
int 
get_memory_usage()
{ 
	FILE* 		file 		= fopen("/proc/self/status", "r");
	int 		result 		= -1;
    char 		line[128];
	char		*tok;//, *L;

	while (fgets(line, 128, file) != NULL){
		if (strncmp(line, "VmRSS:", 6) == 0){
			tok = line;
			while (*tok && !isdigit(*tok)) tok++;
			if (isdigit(*tok)) result = atoi(tok);
			break;
		}
	}
	fclose(file);
	return result;
}
char *
get_parent(char *parent, int sz, char *path)
{
	char	*psep = NULL;


	strncpy(parent, path, sz-1);
	psep 	= strrchr(parent, '/');

	if (psep) 
		*(psep+1) = 0;
	else {
		strcpy(parent, "/");
	}
	return parent;
}

inline int 
count_bits(volatile unsigned long *addr, int n)
{
    int     s = 0;
    while (n > 0) {
        s += __builtin_popcountl(*addr);
        addr++;
        n--;
    }
    return s;
}
#pragma message (REMIND("ffz함수의 최신 버전 체크 필요"))

/*
 * long사이즈 메모리 영역에서 first zero bit위치 검색
*/
static __always_inline unsigned long 
ffz(unsigned long word)
{
	__asm__ ("rep; bsf %1,%0"
		: "=r" (word)
		: "r" (~word));
	return word;
}
/*
 *
 * @size : bit count
 *
 */
unsigned long 
find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long idx;

	for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
		if (addr[idx] != ~0UL)
			return min(idx * BITS_PER_LONG + ffz(addr[idx]), size);
	}

	return size;
}



#ifdef NC_MEASURE_MUTEX
/*
 * path-lock이 다양한 위치에서 호출되는데 각 위체에서 획득된 lock이
 * release될 떄까지 걸린 시간을 측정하고, 나중에
 * lock의 평균 유지 시간, 호출된 횟수 등으로 분석하기위해서 사용
 *
 */




struct tag_mls_t {
	char		*file;
	int			line;
	double		sum;
	double		vmin;
	double		vmax;
	double		vavg;
	long long	count;
};

static 	void 			*s__mls_tbl = NULL;
static  int 	 		s__mls_array_cnt = 0;
static  int 	 		s__mls_array_alloc = 0;
static  mls_t 	 		**s__mls_array = NULL;

/*
 * @f : source file name
 * @l : source line #
 * @msec : l@f에서 획득한 lock이 해제될 떄 까지 걸린 시간(msec)
 *
 * 소스에서 l@f에서 획득한 lock의 보유시간을 모두 합산
 */
static int
compare_ml_stat(const void *a, const void *b)
{
    mls_t   *pa = (mls_t *)a;
    mls_t   *pb = (mls_t *)b;
    int     r;

    r =  strcmp(pa->file, pb->file);
    if (r) return -r;
    return -1.0 * (pa->line - pb->line);

}
static mls_t *
lookup_ml_stat(char *f, int l)
{
	mls_t			key = {f, l, 0, 0};
	mls_t			**ap 	= NULL;
	mls_t			*found 	= NULL;

	pthread_mutex_lock(&s__mls_lock);
	if (s__mls_tbl)
		ap = (mls_t **)tfind(&key, &s__mls_tbl, compare_ml_stat);

	if (ap) {
		found 		= *ap;
	}
	else {
		found 		= (mls_t *)malloc(sizeof(mls_t));
		found->file = f; /* symbol table에 static하게 할당된 스트링 */
		found->line = l;
		found->sum  = 0;
		found->count= 0;

		if (s__mls_array_cnt >= s__mls_array_alloc) {
			s__mls_array_alloc += 128;
			s__mls_array = realloc(s__mls_array, s__mls_array_alloc*sizeof(mls_t *));
		}
		s__mls_array[s__mls_array_cnt] = found;
		ap = (mls_t **)tsearch(found, &s__mls_tbl,  compare_ml_stat);
		s__mls_array_cnt++;
	}
	pthread_mutex_unlock(&s__mls_lock);
	return found;
}
static void
update_ml_stat(mls_t *found, double msec)
{

	//pthread_spin_lock(&s__mls_lock);
	pthread_mutex_lock(&s__mls_lock);
	DEBUG_ASSERT(msec >= 0);
	found->sum += msec;

	if (found->count == 0) {
		found->vmin =
		found->vmax = msec;
	}
	else {
		found->vmin = min(found->vmin, msec);
		found->vmax = max(found->vmax, msec);
	}
	found->count++;
	//pthread_spin_unlock(&s__mls_lock);
	pthread_mutex_unlock(&s__mls_lock);
}
static int
sort_by_holdtime(const void *a, const void *b)
{
	mls_t 	*pa = *(mls_t **)a;
	mls_t 	*pb = *(mls_t **)b;
	double 	r;

	r =  (pa->vavg - pb->vavg);
	if (!r) return r;

	return (int)(r > 0.0? 1:-1);
}
void
report_ml_stat()
{
	int		i ;
	char	zc[64];

	//pthread_spin_lock(&s__mls_lock);
	pthread_mutex_lock(&s__mls_lock);
	for (i = 0; i < s__mls_array_cnt; i++) {
		if (s__mls_array[i]->count > 0)
			s__mls_array[i]->vavg = (double)s__mls_array[i]->sum/(1.0 * s__mls_array[i]->count);
		else
			s__mls_array[i]->vavg = 0.0;
	}

	qsort(s__mls_array, s__mls_array_cnt, sizeof(mls_t *), sort_by_holdtime);

	TRACE((T_ERROR, "%15s | %6s | %6s | %6s | %64s\n", "count", "avg", "min", "max", "source"));
	for (i = 0; i < s__mls_array_cnt; i++) {
		TRACE((T_ERROR, "%15s | %6.2f | %6.2f | %6.2f | %s:%d\n",
						ll2dp(zc, sizeof(zc), s__mls_array[i]->count), 
						s__mls_array[i]->vavg, 
						s__mls_array[i]->vmin, 
						s__mls_array[i]->vmax, 
						s__mls_array[i]->file, 
						s__mls_array[i]->line));
		s__mls_array[i]->vavg = 0.0; 
		s__mls_array[i]->vmin = 0.0; 
		s__mls_array[i]->vmax = 0.0; 
		s__mls_array[i]->count= 0; 
						
	}
	//pthread_spin_unlock(&s__mls_lock);
	pthread_mutex_unlock(&s__mls_lock);

}
#endif

#define		MODU_TERA		1000000000000L
#define		MODU_GIGA		1000000000L
#define		MODU_MEGA		1000000L
#define		MODU_KILO		1000L

#define		LARGER_THAN(n_, m_)		((n_) >= (m_))
char *
unitdump(char *buf, int buflen, long num)
{
	char			unit = ' ';
	long			modu = 1; 
	if (LARGER_THAN(num, MODU_TERA)) {
		modu 	= MODU_TERA;
		unit	= 'T';
	}
	else if (LARGER_THAN(num, MODU_GIGA)) {
		modu 	= MODU_GIGA;
		unit	= 'G';
	}
	else if (LARGER_THAN(num, MODU_MEGA)) {
		modu 	= MODU_MEGA;
		unit	= 'M';
	}
	else if (LARGER_THAN(num, MODU_KILO)) {
		modu 	= MODU_KILO;
		unit	= 'K';
	}
	sprintf(buf, "%.2f%c", (float)(num*1.0)/modu, unit);
	return buf;
}

#ifndef NC_RELEASE_BUILD
typedef struct {
	nc_int32_t	sno;
	int			line;
	char		*source;
	link_node_t	node;
} lock_info_t;
static __thread link_list_t		__lock_order = LIST_INITIALIZER;
int
lo_check(int sno_to_lock)
{
	lock_info_t	*l = NULL;
	int			r  = 0;
	lock_info_t  t = {-1, ""};
	l = &t;


	if (link_empty(&__lock_order)) {
		r = 1;
	}
	else {
		l = (lock_info_t *)link_get_head_noremove(&__lock_order);
		r = l->sno - sno_to_lock;
	}
	if (r < 0) {
		TRACE((T_ERROR, "prev lock (sno=%d) is lower than the request sno %d, alloc'ed at %d@%s\n", 
						l->sno,
						sno_to_lock,
						l->line, 
						l->source
			  ));
		TRAP;
	}
}
void
lo_push(int sno_to_lock, char *f, int l)
{
	lock_info_t	*li = (lock_info_t *)XMALLOC(sizeof(lock_info_t), AI_ETC);

	li->sno		= sno_to_lock;
	li->source	= f;
	li->line	= l;
	//TRACE((T_WARN, "%p::lock-info:%p\n",  &__lock_order, li));
	link_prepend(&__lock_order, li, &li->node);
}
int
lo_pop()
{
	lock_info_t	*l = (lock_info_t *)link_get_head(&__lock_order);
	int			sno = -1;

	if (l) {
		//TRACE((T_WARN, "%p::lock-info:%p\n",  &__lock_order, l));
		sno = l->sno;
		XFREE(l);
	}

	return sno;
}
#endif
