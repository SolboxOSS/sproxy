#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <execinfo.h>
#include <search.h>
#include <pthread.h>
#include <config.h>

/* CHG 2018-05-10 huibong dlfcn.h 사용시 컴파일 오류 관련 __USE_GNU 선언 추가 (#32089) */
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <dlfcn.h> /* define 문제로 config.h 이후에 선언 */

#include <util.h>
#include <netcache_types.h>
#include <netcache_config.h>
#include <trace.h>




#ifdef NC_HEAPTRACK

__thread int 	___trace_upper = 0;
__thread int 	___bypass_hook 	= 0; /* libc 함수를 바로 호출할 때 사용 */

#define		ALIGN_UNIT					8
#define     ALIGNED_VALUE(_l)         ( ((_l)+ALIGN_UNIT-1) & ~(ALIGN_UNIT-1) )
#define		HT_MAGIC					0xA55A5AA55AA5A55B

#define		HT_OUTPUT_FORMAT_H			"%-80s %8s %12s"
#define		HT_OUTPUT_FORMAT			"%-80s %8ld %12s"

#ifndef min
#define min(_a, _b)				((_a) > (_b)? (_b):(_a))
#endif

static void 		(*__dump_proc)(char *) = NULL;
__thread 	int 	___no_tracing 	= 0; /* libc 함수를 바로 호출할 때 사용 */

#define		SET_CALLER(_s, _a)	{ \
								void * 		bt[16]; \
								char **		bt_syms = NULL; \
								int 		bt_size = 0; \
								assert(___no_tracing == 0); \
								memset(bt, 0, sizeof(bt)); \
								___no_tracing++; \
								bt_size = backtrace(bt, 2+___trace_upper); \
								/* bt_syms ****malloc'ed**** from backtrace_symbols */ \
								bt_syms = backtrace_symbols(bt, bt_size+___trace_upper); \
								(_s) 	= ht_add_symbol_if_absent(bt_syms[1+___trace_upper]); \
								free(bt_syms); \
								(_a) 	= bt[1+___trace_upper]; \
								___no_tracing--; \
								assert(___no_tracing == 0); \
							}


#define 	ASSERT_MH(m_, e_)	if (!(e_))  \
	{ \
									char _dbuf[8192]; \
	TRACE((g__trace_error, "Error in Expr '%s' at %d@%s::%s\n", \
													#e_, __LINE__, __FILE__, \
													ht_dump_mh(_dbuf, sizeof(_dbuf), m_))); \
													TRAP; \
								}




#pragma pack(8) 
/* 64bit build*/
typedef struct node_ {
	void 			*d;
	struct node_ 	*n;
	struct node_ 	*p;
} node_t;

typedef struct {
	node_t		*H;
	node_t		*T;
	int			count;
} dllist_t;
#define	DLLIST_INITIALIZER	{NULL, NULL, 0}

struct leak_info {
	long long	sum;		/* total allocation in bytes */
	long		count;		/* total alloc calls not freed */
	void		*address;	/* backtraced caller address info */
	char		*symbol;	/* backtraced caller address info */
	node_t		node;
};


typedef struct {
	unsigned long long 	fmagic;
	unsigned long long* rmagic;
	char				trace;
	char				aligned;
	void*				address; 	/* caller's address */
	char*				symbol; 	/* caller's function name? */
	size_t				size;		/* real alloc size */
	node_t				node;
	char				data[0];
} __mh_t;
#pragma pack()
typedef struct tag_shash_entry {
	void 					*addr; /* aligned memory block addr */
	void					*data;
	struct tag_shash_entry *next;
	
} shash_entry_t;
typedef struct {
	int				nels;
	shash_entry_t 	**buckets;
} shash_t;


static pthread_mutex_t		___alloc_lock 		= PTHREAD_MUTEX_INITIALIZER;
static dllist_t 			___alloc_root 		= {NULL, NULL};
static shash_t 				___alloc_aamap;

static long long 			___alloc_kept = 0;
static long long 			___alloc_counter = 0;
static void*				___alloc_symdict = NULL; /* call stack 심볼을 uniq하게 등록해둔 테이블 */



//static int ht_count(dllist_t *root);
static void ht_append(dllist_t *, void *data, node_t *d);
static void ht_remove(dllist_t *, node_t *m);
//static void ht_sort(dllist_t *root);
static void *ht_get_head(dllist_t *root);
static void *ht_get_next(node_t *m);
static void ht_lineinfo(char *buf, void *address, char *symbol);
static void * __t_malloc(size_t sz);
static void * __t_calloc(size_t nel, size_t sz);
static void __t_free(void *p);
static char * __t_strdup(const char *s);
static char * __t_strndup(const char *s, size_t);
static int __t_range(void *p);
static char *ht_add_symbol_if_absent(char *p);
//static void SYMBOL(const void *nodep, VISIT val, int lev);
static int ht_compare_li(const void *a, const void *b);
static int ht_shash_add(shash_t *ht, void *addr, void *d);
static int ht_shash_remove(shash_t *ht, void *addr);
static void * ht_shash_lookup(shash_t *ht, void *p);
static void * malloc_internal(size_t sz);
static char * ht_dump_mh(char *buf, int len, __mh_t *pa);
static void * ht_shash_dump(shash_t *ht);



static 		void *(*___libc_malloc)(size_t) 				= __t_malloc;			
static 		void *(*___libc_calloc)(size_t, size_t)			= __t_calloc;	
static 		void *(*___libc_realloc)(void *, size_t)		= NULL;	
static 		void  (*___libc_free)(void *)					= __t_free;				
static 		void *(*___libc_memalign)(size_t, size_t)		= NULL;
static 		size_t (*___libc_malloc_usable_size) (void *__ptr)  = NULL;
static 		char *(*___libc_strdup)(const char *)			= __t_strdup;		
static 		char *(*___libc_strndup)(const char *,size_t)	= __t_strndup;		








static void
__init_hooks()
{
	pthread_mutexattr_t		mattr;

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&___alloc_lock, &mattr);

	___libc_calloc			= dlsym(RTLD_NEXT, "calloc");
	___libc_malloc			= dlsym(RTLD_NEXT, "malloc");
	___libc_realloc			= dlsym(RTLD_NEXT, "realloc");
	___libc_free			= dlsym(RTLD_NEXT, "free");
	___libc_malloc_usable_size = dlsym(RTLD_NEXT, "malloc_usable_size");
	___libc_memalign		= dlsym(RTLD_NEXT, "memalign");
	___libc_strdup			= dlsym(RTLD_NEXT, "strdup");
	___libc_strndup			= dlsym(RTLD_NEXT, "strndup");
	___alloc_aamap.nels		= 7181;
	___alloc_aamap.buckets	= ___libc_calloc(___alloc_aamap.nels, sizeof(void *));
}



void __attribute__ ((constructor)) 
__premain()
{
	__init_hooks();
}



char *
nc_get_heap_alloc_counter_x()
{
	int					c = 0;
	__mh_t *			pmh;
	node_t * 			nav = NULL;
	static char 		_zac[256]="";
	long long			l_user = 0;
	long long			l_sys = 0;;

	pthread_mutex_lock(&___alloc_lock);
	nav = ht_get_head(&___alloc_root);
	while (nav) {
		pmh 		= (__mh_t *)nav->d;
		if (pmh->trace)
			l_user 	+= pmh->size;
		else
			l_sys 	+= pmh->size;
		nav = ht_get_next(nav);
		c++;
	}
	pthread_mutex_unlock(&___alloc_lock);
	sprintf(_zac, "a_cnt=%lld, link_cnt=%d(user=%.2f MB, sys=%.2fMB)", 
				___alloc_counter, c, l_user/1000000.0, ((long)c * sizeof(__mh_t))/1000000.0);
	return _zac;
}
static char *
ht_dump_mh(char *buf, int len, __mh_t *pa)
{
	snprintf(buf, len, "{F[%llx],R[%llx],Sz[%ld],(%s%s) at %s}",
				pa->fmagic,
				*pa->rmagic,
				(long)pa->size,
				(pa->trace?"TRC ":""),
				(pa->aligned?"ALGN ":""),
				pa->symbol
				);
	return buf;
}

static void *
ht_shash_dump(shash_t *ht)
{
	int 	i, cnt;
	char	buf[10240];
	char	*z = buf;
	int 	n, rem=sizeof(buf);
	shash_entry_t		*nav;

	pthread_mutex_lock(&___alloc_lock);


	for (i = 0; i < ht->nels; i++)  {
		cnt = 0;
		z = buf;
		n = snprintf(z, rem, "BI[%d]:", i);
		z+= n; rem -= n;
		nav  = ht->buckets[i];
		while (nav) {
			n = snprintf(z, rem, "(%p %p) ", nav->addr, nav->data);
			z+= n; rem -= n;
			nav = nav->next;
			cnt++;
		}
		if (cnt > 0)
			TRACE((T_WARN, "%s\n", buf));
	}
	pthread_mutex_unlock(&___alloc_lock);
	return NULL;
}

static void *
ht_shash_lookup(shash_t *ht, void *p)
{
	int 	bi = -1;
	shash_entry_t		*nav;
	void 	*d = NULL;

	pthread_mutex_lock(&___alloc_lock);
	bi = (unsigned long long)p % ht->nels;
	nav  = ht->buckets[bi];
	while (nav) {
		if (nav->addr == p) {
			d = nav->data;
			break;
		}
		nav = nav->next;
	}
	pthread_mutex_unlock(&___alloc_lock);
	return d;
}
static int
ht_shash_remove(shash_t *ht, void *addr)
{
	shash_entry_t 	*nav, *bef;
	int				bi;
	int				found = 0;
	int				scanned = 0;
	

	bi = ((unsigned long long)addr % ht->nels);


	pthread_mutex_lock(&___alloc_lock);

	nav  = ht->buckets[bi];
	bef  = NULL;

	while (nav) {
		scanned++;

		if (nav->addr == addr) {
			found++;
			break; /* found */
		}
		bef = nav;
		nav = nav->next;
	}
	
	if (nav) {
		/* found matching */

		ASSERT(nav->addr == addr);
		if (bef == NULL) {
			/* head entry */
			TRACE((0, "BIDX=%d ADDR[%p](=%p) (HEAD)\n", bi, addr, ht->buckets[bi]));
			ht->buckets[bi] = nav->next;
		}
		else {
			bef->next = nav->next;
			TRACE((0, "BIDX=%d ADDR[%p] (MIDDLE)\n", bi, addr));
		}
		nav->next = NULL;
	}
	pthread_mutex_unlock(&___alloc_lock);

	if (nav) ___libc_free(nav);
	return (nav != NULL);
}
static int
ht_shash_add(shash_t *ht, void *addr/*aligned*/, void *d/*mh_t*/)
{
	shash_entry_t 	*e;
	shash_entry_t 	*nav;
	int				bi;
	int				app = 0;

	e 		= malloc_internal(sizeof(shash_entry_t));
	e->addr = addr;
	e->data = d;
	e->next = NULL;

	bi = (unsigned long long)addr % ht->nels;
	pthread_mutex_lock(&___alloc_lock);
	if (ht->buckets[bi] == NULL) {
		ht->buckets[bi] = e;
		TRACE((0, "NO_TRACE=0, BIDX=%d, MEMORY[%p] added(HEAD)\n", bi, addr));
	}
	else {
		nav = ht->buckets[bi];
		while (nav) {
			app++;
			if (nav->next == NULL) break;
			nav = nav->next;
		}
		ASSERT(nav->next == NULL);
		nav->next = e;
		app++;
		TRACE((0, "NO_TRACE=0, BIDX=%d, MEMORY[%p] added(APPEND after [%p], total %d)\n", bi, addr, nav, app));

	}
	pthread_mutex_unlock(&___alloc_lock);
	return 0;
}


long long
nc_get_heap_allocated()
{
	return __sync_add_and_fetch(&___alloc_kept, 0);
}
static int
lip_compare(const void *a, const void *b)
{
	struct leak_info 	*li_a = *(struct leak_info **)a, 
						*li_b = *(struct leak_info **)b;


	return (int)(li_b->sum - li_a->sum); 

}
static void
__dump_leak(struct leak_info *pli)
{
	char						lbuf[512];
	char						cinfo[256]="";
	char						zn[64];


	ht_lineinfo(cinfo, pli->address, pli->symbol);
	if (cinfo[0]=='?' || cinfo[0] == '\0')
		snprintf(lbuf, sizeof(lbuf), HT_OUTPUT_FORMAT, pli->symbol, pli->count, ll2dp(zn, sizeof(zn), (long long)pli->sum));
	else
		snprintf(lbuf, sizeof(lbuf), HT_OUTPUT_FORMAT, cinfo, pli->count, ll2dp(zn, sizeof(zn), (long long)pli->sum));
	__dump_proc(lbuf);
}
int 
nc_raw_dump_report(void  (*dump_proc)(char *))
{
	void*						summary = NULL; /* call stack 심볼을 uniq하게 등록해둔 테이블 */
	node_t * 					nav = NULL;
	__mh_t *					pmh;
	struct leak_info			key,
								*pli,
								**ap,
								**sorted = NULL;
	long long					ll_leakcount = 0;
	int							ll_cnt = 0;
	int							ll_alloc = 0;
	long long					l_sum = 0;
	int							i;
	char						zd[32];




	pthread_mutex_lock(&___alloc_lock);

	___no_tracing = 1;


	
	nav = ht_get_head(&___alloc_root);
	ll_cnt = 0;

	ll_alloc = 256;
	sorted = calloc(ll_alloc, sizeof(struct leak_info *));
	while (nav) {
		pmh 		= (__mh_t *)nav->d;
		ll_leakcount ++;

		memset(&key, 0, sizeof(key));
		key.symbol 	= pmh->symbol;

		ap 			= NULL;
		l_sum 	 	+= pmh->size;
		if (summary)
			ap = (struct leak_info **)tfind(&key, &summary, ht_compare_li);


		if (ap) {
			/* found */
			pli 		= *ap;
			pli->sum 	+= pmh->size;
			pli->count++;
		}
		else {
			pli 			= malloc(sizeof(struct leak_info));
			pli->sum 		= pmh->size;
			pli->count		= 1;
			pli->address	= pmh->address;
			pli->symbol		= pmh->symbol;

			if (ll_cnt >= ll_alloc) {
				ll_alloc += 128;
				sorted = realloc(sorted, ll_alloc * sizeof(struct leak_info *));
			}
				
			sorted[ll_cnt] 	= pli;
			ap = (struct leak_info **)tsearch(pli, &summary, ht_compare_li);
			ll_cnt++;

		}
		nav = ht_get_next(nav);
	}

	TRACE((T_MONITOR, "** HEAP LEAKAGE REPORT\n"));
	TRACE((T_MONITOR, "      - Total Leakage Location : %d(%lld)\n", ll_cnt, ll_leakcount));
	TRACE((T_MONITOR, "      - Total Leakage Bytes    : %s\n", ll2dp(zd, sizeof(zd), l_sum)));
	TRACE((T_MONITOR, "      - Total Leakage Counter  : %s(%lld)\n", ll2dp(zd, sizeof(zd), ___alloc_kept), ___alloc_counter));
	TRACE((T_MONITOR, "\n"));
	__dump_proc = dump_proc;
	qsort(sorted, ll_cnt, sizeof(struct leak_info *), lip_compare);

	TRACE((T_MONITOR, "Sorting done\n"));
	for (i = 0; i < ll_cnt; i++) {
		//twalk(summary, __dump_tree);
		__dump_leak(sorted[i]);
	}
	TRACE((T_MONITOR, "Report done\n"));

	//TRACE((T_MONITOR, "** HEAP LEAKGAGE ADDRESS\n"));
	//twalk(___alloc_symdict, SYMBOL);
	ht_shash_dump(&___alloc_aamap);
	___no_tracing = 0;

	//BUGGY LINE:tdestroy(...)
	//tdestroy(summary, ___libc_free);
	pthread_mutex_unlock(&___alloc_lock);

	return 0;

}


/*
 *
 * wrapper
 *
 */
char *
strdup(const char *s)
{
	char *n;

#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif
	n = malloc(strlen(s)+1);
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
	strcpy(n, s);

	return n;
}
char *
strndup(const char *s, size_t n)
{
	char 		*as;
	size_t 		msz;

#ifdef NC_HEAPTRACK
	___trace_upper++;
#endif
	msz 	= min(strlen(s), n);
	as 		= malloc(msz + 1);
#ifdef NC_HEAPTRACK
	___trace_upper--;
#endif
	strncpy(as, s, msz);

	return as;
}
static void *
malloc_internal(size_t sz)
{
	size_t	asiz 	= ALIGNED_VALUE(sz) + sizeof(__mh_t) + sizeof(long long);
	__mh_t*	pa 		= NULL;

	pa 			= (__mh_t *)___libc_calloc(1, asiz);
	pa->fmagic	= HT_MAGIC;
	pa->size	= sz;
	pa->rmagic  =  (unsigned long long *)(((char  *)pa + asiz) - sizeof(long long));
	*pa->rmagic	= ~HT_MAGIC;

	return pa;
}

#if 1
void *
memalign(size_t align, size_t size)
{
	__mh_t		*pa;
	//void 		*np;
	void 		*ap = NULL;


	/*
	 * aligned memory 할당
	 */
	ap = ___libc_memalign(align,size);
	

	if (___no_tracing == 0) {
		pa = malloc_internal(sizeof(void *));
		SET_CALLER(pa->symbol, pa->address);
		pa->size 				+= size;
		pa->trace 				= 1;
		pa->aligned 			= 1;
		*(long long *)pa->data	= (long long)ap;
		ht_append(&___alloc_root, pa, &pa->node);
		ht_shash_add(&___alloc_aamap, ap, pa);
		__sync_add_and_fetch(&___alloc_kept, pa->size);
		__sync_add_and_fetch(&___alloc_counter, 1);
	}
		
	return (void *)ap;
}
#endif
void *
malloc(size_t sz)
{
	__mh_t		*pa;
	//void 		*np;

	if (sz == 0) sz = 1;

	pa = malloc_internal(sz);

	pa->trace = (___no_tracing == 0);
	if (pa->trace) { 
		SET_CALLER(pa->symbol, pa->address);
		ht_append(&___alloc_root, pa, &pa->node);
		__sync_add_and_fetch(&___alloc_kept, pa->size);
		__sync_add_and_fetch(&___alloc_counter, 1);
	}
		
	return (void *)pa->data;
}
size_t
malloc_usable_size(void *v)
{
	__mh_t		*pa;

	pa = (__mh_t *)((char *)v - sizeof(__mh_t));
	if (v) {
		if (__t_range(v)) {
			return 0;
		}
		else {

			pa = (__mh_t *)((char *)v - sizeof(__mh_t));
			if (pa->fmagic != HT_MAGIC) {
				TRACE((T_ERROR, "%p - not allocated addr(dump of shash)\n", v));
				pa = v;
				return 0;
			}
			ASSERT_MH(pa, pa->size > 0);
			ASSERT_MH(pa, pa->fmagic == HT_MAGIC);
			ASSERT_MH(pa, *pa->rmagic == ~HT_MAGIC);
	
			/*
			 * aligned_alloc을 통해서 들어온 할당들은
			 * magic값이 없음
			 */
			return ___libc_malloc_usable_size((void *)pa);
		}
	}
	return 0;
}


void *
calloc(size_t nel, size_t sz) 
{
	__mh_t		*pa;
	
	pa = malloc_internal(nel*sz);
	pa->trace = (___no_tracing == 0);
	if (pa->trace) { 
		SET_CALLER(pa->symbol, pa->address);
		ht_append(&___alloc_root, pa, &pa->node);
		__sync_add_and_fetch(&___alloc_kept, pa->size);
		__sync_add_and_fetch(&___alloc_counter, 1);
	}
	
	return (void *)pa->data;
}
void *
realloc(void *ap, size_t sz)
{
	__mh_t	*	pa_o = NULL;
	__mh_t	*	pa_n = NULL;
	size_t		sz_o;


	if (ap) {
		pa_o = (__mh_t *)((char *)ap - sizeof(__mh_t));
		sz_o = pa_o->size;
	}
	pa_n = malloc_internal(sz);
	pa_n->trace = (___no_tracing == 0);
	if (pa_n->trace) { 
		SET_CALLER(pa_n->symbol, pa_n->address);
		ht_append(&___alloc_root, pa_n, &pa_n->node);
		__sync_add_and_fetch(&___alloc_counter, 1);
		__sync_add_and_fetch(&___alloc_kept, pa_n->size);
	}	
	if (pa_o && pa_n)
		memcpy(pa_n->data, pa_o->data, min(sz_o, sz));

	
	if (ap) free(ap);
	
	return pa_n->data;
}
void 
free(void *v)
{
	__mh_t 	*pa = NULL;
	//void 	*v_org = v;
	
	if (v) {
		if (__t_range(v)) {
			__t_free(v);
			return;
		}
		else {

			pa = (__mh_t *)ht_shash_lookup(&___alloc_aamap, v);

			if (pa) { 
				ASSERT(*(long long *)pa->data == (long long)v);
				ASSERT(pa->aligned == 1);
				ASSERT(ht_shash_remove(&___alloc_aamap, v) != 0);
			}
			else {
				pa = (__mh_t *)((char *)v - sizeof(__mh_t));
			}
			if (pa->fmagic != HT_MAGIC) {
				TRACE((T_ERROR, "%p - not allocated addr(dump of shash)\n", v));
				nc_dump_stack(0);
				pa = v;
				goto L_direct_free;
			}
			if (pa->fmagic == ~HT_MAGIC) {
				TRACE((T_ERROR, "%p - double freed addr(dump of shash)\n", v));
				TRAP;
			}
			ASSERT_MH(pa, pa->size > 0);
			ASSERT_MH(pa, pa->fmagic == HT_MAGIC);
			ASSERT_MH(pa, *pa->rmagic == ~HT_MAGIC);
			pa->fmagic = ~HT_MAGIC;
			pa->rmagic = HT_MAGIC;
	
			/*
			 * aligned_alloc을 통해서 들어온 할당들은
			 * magic값이 없음
			 */
			if (pa->trace) {
				ht_remove(&___alloc_root, &pa->node);
				__sync_sub_and_fetch(&___alloc_kept, pa->size);
				__sync_sub_and_fetch(&___alloc_counter, 1);
			}
L_direct_free:
			___libc_free((void *)pa);
			return;
		}
	}
}

#if NOT_USED
static int
ht_count(dllist_t *root)
{
	int 	r;
	pthread_mutex_lock(&___alloc_lock);
	r = root->count;
	pthread_mutex_unlock(&___alloc_lock);
	return r;
}
#endif
static void
ht_append(dllist_t *root, void *data, node_t *m)
{
	pthread_mutex_lock(&___alloc_lock);
	
	m->d = data;
	m->n = NULL;
	m->p = NULL;

	if (root->count == 0) {
		root->H = root->T = m;
	}
	else {
		root->T->n 	= m;
		m->p 		= root->T;
		root->T		= m;
	}
	root->count++;

	pthread_mutex_unlock(&___alloc_lock);
}
static void 
ht_remove(dllist_t *root, node_t *m)
{
	/*노드를 리스트에서 삭제*/
	pthread_mutex_lock(&___alloc_lock);

	assert(root->count > 0);

	if (m->n != NULL)
		m->n->p = m->p;
	else
		root->T = m->p;
	
	if (m->p != NULL) 
		m->p->n = m->n;
	else
		root->H = m->n;

	root->count--;
	pthread_mutex_unlock(&___alloc_lock);
}

#if NOT_USED
static void
ht_swap(struct leak_info *a, struct leak_info *b)
{
	struct leak_info	T;

	memcpy(&T, 	a, 	sizeof(T));

	a->sum		= b->sum;
	a->count 	= b->count;
	a->address 	= b->address;
	a->symbol 	= b->symbol;

	b->sum		= T.sum;
	b->count 	= T.count;
	b->address 	= T.address;
	b->symbol 	= T.symbol;


};
static void
ht_sort(dllist_t *root)
{
	node_t 					*pp, *pn, *anc;
	int 					changed = 1;
	struct leak_info 		*pli_pp;
	struct leak_info 		*pli_pn;


	/* q sort */


	while (changed) { 
		changed = 0;
		pp 		= ht_get_head(root);
		while (pp) {
			anc = pn  = pp->n? pp->n:NULL;

			if (!pn) break;

			if (pn) {
				pli_pp = (struct leak_info *)pp->d;
				pli_pn = (struct leak_info *)pn->d;
				if (pli_pp->sum < pli_pn->sum) {
					ht_swap(pli_pp, pli_pn);
					changed++;
				}
			}
			pp 	= pn;
		}
	}
}
#endif
static void *
ht_get_head(dllist_t *root)
{
	return root->H;
}
static void *
ht_get_next(node_t *m)
{
	return (m?m->n:NULL);
}




static void 
ht_lineinfo(char *buf, void *address, char *symbol)
{
	int 				rc;
	Dl_info 			info;
	const void			*addr;
	FILE				*output;
	char 				cmd_line[1024];
	char 				line[1024], *ptr_line, *pos;
	char 				function_name[1024];
	int 				file_line;
	char 				*plf = NULL;

	rc = dladdr(address, &info);
	if ((rc == 0) || !info.dli_fname || !info.dli_fname[0]) {
		/*
		 * TODO!!!!
		 * dlopened library
		 * I don't know why
		 */
		TRACE((0, "dladdr - rc[%d],info.dli_fname[%p], dli_path[%s], symbol[%s]\n", 
						rc,
						info.dli_fname,
						((info.dli_fname && info.dli_fname[0])? info.dli_fname:"?"),
						symbol));
		return;
	}

	addr = address;
	if (info.dli_fbase >= (const void *) 0x40000000)
		addr = (void *) ((unsigned long) ((const char *) addr) - (unsigned long) info.dli_fbase);

	snprintf(	cmd_line, 
				sizeof(cmd_line),
				"addr2line --functions --demangle -e $(which %s) %p",
				info.dli_fname, 
				addr);

	TRACE((0, ">>>CMD(%p, '%s'):'%s'\n", address, symbol, cmd_line));
	output = popen(cmd_line, "r");

	if (!output) {
		TRACE((T_MONITOR, "no-output\n"));
		sprintf(buf, "[%s]", symbol?symbol:"null");
		return;
	}

	function_name[0] = '\0';
	file_line = 0;
	while (!feof(output)) 
	{
		ptr_line = fgets(line, sizeof(line) - 1, output);
		TRACE((0, "<<<OUT:%s\n", (ptr_line?ptr_line:"")));

		if (ptr_line && ptr_line[0]) {
			pos = strchr(ptr_line, '\n');

			if (pos) pos[0] = '\0';

			if (strchr(ptr_line, ':')) {
				file_line = 1;

				/* addr2line 호출 결과.. 소스 파일명 추출 실패시 */
				/* CHG 2018-04-26 huibong addr2line 에서 정보 추출 실패시 로깅 개선 (#32077) */
				if( strlen( ptr_line ) > 2 && ptr_line[0] == '?' && ptr_line[1] == '?' )
				{
					sprintf( buf, "%s%s%s%s \n"
							 , symbol
							 , ( function_name[0] ) ? " [" : ""
							 , function_name
							 , ( function_name[0] ) ? "]" : "" );
				}
				else
				{
#if 0
					sprintf( buf, "%s%s%s%s [0x%llx]\n",
							 ptr_line,
							 ( function_name[0] ) ? " [function " : "",
							 function_name,
							 ( function_name[0] ) ? "]" : "",
							 address );	/* ??:0 [function ??] 이런식으로 symbol이 깨지더라도 주소는 기록되게한다. */
#else
					sprintf( buf, "%s%s%s%s [0x%llx]\n",
							 ptr_line,
							 ( function_name[0] ) ? " [" : "",
							 function_name,
							 ( function_name[0] ) ? "]" : "",
							 address );	/* ??:0 [function ??] 이런식으로 symbol이 깨지더라도 주소는 기록되게한다. */
#endif
				}

				function_name[0] = '\0';
			} 
			else 
			{
				snprintf(function_name, sizeof(function_name), "%s", ptr_line);
			}
		}
	}

	/* CHG 2018-04-26 huibong popen 사용에 따른 pclose 처리 누락 수정 (#32077) */
	pclose( output );

#if 1
	if (function_name[0]) {
		TRACE((T_MONITOR, "function_name:::  %s\n", function_name));
	}	/* {} 표시를 하지 않으면 TRACE define에서 if 문이 포함 되어 있어서 아래의 else if 가 정상 동작 하지 않는다. */
	else if (0 == file_line) {	/* popen으로 정보를 못가져 오는 경우 symbol 정보라도 기록을 한다.*/

		if (symbol) strcpy(buf, symbol);
	}
#endif
	plf = strrchr(buf, '\r'); if (plf) *plf =  0;
	plf = strrchr(buf, '\n'); if (plf) *plf =  0;

}



static pthread_mutex_t 			__t_lock = PTHREAD_MUTEX_INITIALIZER;
static char 					__t_heap[8*1024*1024];
static char *					__t_cursor = __t_heap;
static int
__t_range(void *p)
{
	return ((char *)p >= __t_heap  && (char *)p < ((char *)__t_heap+sizeof(__t_heap)));
}
static void *
__t_calloc(size_t nel, size_t sz)
{
	size_t 		asz = ALIGNED_VALUE(sz);
	char * 		ptr;
	

	pthread_mutex_lock(&__t_lock);
	ptr 		= __t_cursor;
	__t_cursor +=  (nel * asz);
	if (!__t_range(__t_cursor))
		abort();
	pthread_mutex_unlock(&__t_lock);
	return ptr;
}

static void *
__t_malloc(size_t sz)
{
	size_t 		asz = ALIGNED_VALUE(sz);
	char * 		ptr;
	

	pthread_mutex_lock(&__t_lock);
	ptr 		= __t_cursor;
	__t_cursor +=  asz;
	assert(__t_range(__t_cursor));
	pthread_mutex_unlock(&__t_lock);
	return ptr;
}
static void 
__t_free(void *p)
{
	/* no real free */
}

static char *
__t_strdup(const char *s)
{
	char *n;

	n = __t_malloc(strlen(s)+1);
	strcpy(n, s);

	return n;
}
static char *
__t_strndup(const char *s, size_t n)
{
	char 		*as;
	size_t 		msz;

	msz 	= min(strlen(s), n);
	as 		= __t_malloc(msz + 1);

	strncpy(as, s, msz);

	return as;
}
#if NOT_USED
static void
SYMBOL(const void *nodep, VISIT val, int lev)
{
	if (val == leaf || val == postorder) {
		TRACE((T_MONITOR, "%s\n", *(char **)nodep));
	}
}
#endif
static int
ht_compare_symbols(const void *a, const void *b)
{
	return strcmp(a, b);
}
static int
ht_compare_li(const void *a, const void *b)
{
	struct leak_info 	*pli_a = (struct leak_info *)a,
						*pli_b = (struct leak_info *)b;

	return strcmp(pli_a->symbol, pli_b->symbol);
}
static char *
ht_add_symbol_if_absent(char *p)
{
	void 	*ap = NULL;
	char 	*dp = NULL;

	pthread_mutex_lock(&___alloc_lock);

	if (___alloc_symdict)
		ap = tfind(p, &___alloc_symdict, ht_compare_symbols);
	if (!ap) {
		ap = tsearch(dp = ___libc_strdup(p), &___alloc_symdict, ht_compare_symbols);
		if (*(char **)ap != dp) {
			free(dp); /* already exist */
		}
	}
	pthread_mutex_unlock(&___alloc_lock);
	
	return *(char **)ap;
}


#else
int 
nc_raw_dump_report(void  (*dump_proc)(char *))
{
	return 0;
}
long long
nc_get_heap_allocated()
{
	return 0;
}
#endif /*HEAPTRACK*/





#if 0 /* module test */

void BABO_BOT(int v)
{
	char	 *p = NULL;

	p = malloc(100);
	p = realloc(p, 200);
	free(p);
}
void BABO3(int v)
{
	v = 0;
	BABO_BOT(v);
}
void BABO2()
{
	BABO3(100);
}
void BABO()
{
	char 	*p = NULL;

	p = malloc(100);
	free(p);
}
void 
dump_leak_info(char *lb)
{
	fprintf(stderr, "%s\n", lb);
}

void *
test_thr(void *u)
{
	int 		i;
	char		*p;
	for (i = 0 ; i < 1000; i++) {
		p = malloc(100);
		p = realloc(p, 80);
		free(p);

		p = strdup("babo");
		free(p);
	}
}
main()
{
	char 			*p = NULL;
	int				i;
	pthread_t		T[10];


	fprintf(stderr, "my main \n");


	for (i = 0; i < 1000; i++) {
		BABO();
		BABO2();
	}
#if 1
	for (i = 0; i < 10; i++) {
		pthread_create(&T[i], NULL, test_thr, NULL);
	}
	for (i = 0; i < 10; i++) {
		pthread_join(T[i], NULL);
	}
#endif
	fprintf(stderr, "joined all\n");
	malloc(128);
	fprintf(stderr, "------------------------------------- report----\n");
	fprintf(stderr, HT_OUTPUT_FORMAT_H "\n", "function", "count", "size(B)");
	nc_raw_dump_report(LEAK);
}
#endif /* module test */

#if NOT_USED
static void
__dump_tree(const void *nodep, VISIT val, int lev)
{
	char						lbuf[512];
	char						cinfo[256]="";
	char						zn[64];

	struct leak_info *pli = *(struct leak_info **)nodep;
	if (val == leaf || val == postorder) {

		ht_lineinfo(cinfo, pli->address, pli->symbol);
		if (cinfo[0]=='?' || cinfo[0] == '\0')
			snprintf(lbuf, sizeof(lbuf), HT_OUTPUT_FORMAT, pli->symbol, pli->count, ll2dp(zn, sizeof(zn), (long long)pli->sum));
		else
			snprintf(lbuf, sizeof(lbuf), HT_OUTPUT_FORMAT, cinfo, pli->count, ll2dp(zn, sizeof(zn), (long long)pli->sum));
		__dump_proc(lbuf);
	}
}
#endif
