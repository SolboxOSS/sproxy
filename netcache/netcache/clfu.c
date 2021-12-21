/*
 * PUBLIC APIs
 *	clfu_hit(...)
 *	clfu_add(...)
 *	clfu_remove(...)
 *	clfu_reclaim(...)
 *	clfu_spin(...)
 */

#include <config.h>
#include <netcache_config.h>
#include <rdtsc.h>
#include <netcache_types.h>
#include <netcache.h>
#include <search.h>



#ifdef NC_MEASURE_CLFU
static bl_t * 	clfu_lookup_bl_stat(char *f, int l);
static void 	clfu_update_bl_stat(bl_t *found, double msec);
#endif

#define	CLFU_NEED_TUNE(root_) 	((nc_cached_clock() - (root_)->lasttuned) > 5) 

static fc_lru_root_t * clfu_find_lru(fc_clfu_root_t *root, lru_node_t *cn);



void 
clfu_set_signature(fc_clfu_root_t *root, const char *signature)
{
	strcpy(root->signature, signature);
}

fc_clfu_root_t * 
clfu_create(void *givenlock, 
			clfu_lock_type_t locktype, 
			int bandcount, 
			fc_check_idle_t check_idle, 
			fc_dump_t dumper, 
			int blockcount, 
			int bottomratio)
{
	fc_clfu_root_t		*mptr;
	//pthread_mutexattr_t mattr;
	int					i;




	mptr = (fc_clfu_root_t *)XMALLOC(sizeof(fc_clfu_root_t), AI_ETC);
	DEBUG_ASSERT(mptr != NULL);
	mptr->basecount = CLFU_DEFAULT_BAND_COUNT;

	mptr->lru = (fc_lru_root_t **)XMALLOC(bandcount* sizeof(fc_lru_root_t *), AI_ETC);
	DEBUG_ASSERT(mptr->lru != NULL);


	for (i = 0; i < bandcount; i++) {
		mptr->lru[i] 		= LRU_create(i, 0/*not used */, check_idle, dumper);
		DEBUG_ASSERT(mptr->lru[i] != NULL);
		mptr->lru[i]->root 	= &mptr->lru[i];
	}



	mptr->bandcount = bandcount;
	mptr->kept 		= 0;
	mptr->lasttuned = 0;
	mptr->current 	= 0;
	mptr->clock 	= 0;

	mptr->hot_ratio 	= (100 - bottomratio);
	mptr->total 		= blockcount;



	mptr->locktype 		= locktype;
	if (givenlock && (locktype != CLFU_LT_NULL)) {
		if (locktype == CLFU_LT_UPGRADABLE)
			mptr->u_rwl	= givenlock;
		else
			mptr->u_xl 	= givenlock;
	}

	return mptr;
}
void
clfu_set_debug(fc_clfu_root_t *root)
{
	root->debug = 1;
}
fc_clfu_root_t *
clfu_set_max(fc_clfu_root_t *root, int cnt)
{
	DEBUG_ASSERT(cnt > 0);
	root->total = cnt;
	return root;
}


void
clfu_clean(fc_clfu_root_t *mptr, fc_destructor_t free_proc)
{
	int					i;



	DEBUG_ASSERT(mptr != NULL);
	mptr->basecount = CLFU_DEFAULT_BAND_COUNT;

	DEBUG_ASSERT(mptr->lru != NULL);
	for (i = 0; i < mptr->bandcount; i++) {
		LRU_destroy(mptr->lru[i], free_proc);
	}

	XFREE(mptr->lru);
	XFREE(mptr);
}
int 
clfu_spin(fc_clfu_root_t *clfu_root)
{
	int		nextband; //, n;


	clfu_root->clock++;
	nextband = (clfu_root->clock) % clfu_root->bandcount;	
	DEBUG_ASSERT((nextband >= 0));

	LRU_join(clfu_root->lru[nextband], clfu_root->lru[clfu_root->current]);
	clfu_root->current = nextband;
	clfu_check_pop(clfu_root, nextband);

	TRACE((T_INFO, "[%s] victim perf= %lld/%lld(%.2f scan per victim)\n",
				clfu_root->signature,
				clfu_root->chkscanned,
				clfu_root->chkvictim,
				(float)(1.0 *clfu_root->chkscanned/clfu_root->chkvictim)
		  ));
	clfu_root->chkscanned = 0; 
	clfu_root->chkvictim  = 0; 



	return nextband;
}
/*\
 *		the initial call of cache
 *		Assumption, node's hitcount should be 0 before call
\*/
int
clfu_cached(lru_node_t *node)
{
	return (node->state == LRU_CACHED);
}
int 
clfu_add(fc_clfu_root_t *root, void *data, lru_node_t *cn)
{


	cn->hitcount 	= 0;
	cn->state 		= LRU_CACHED;
	cn->clockstamp 	= root->lru[root->current]->signature;


   	LRU_add(root->lru[root->current], data, cn);

	root->kept++;



	return 0;
}
int
clfu_lock(fc_clfu_root_t *root, clfu_lock_mode_t lockmode, char *f, int l)
{
#ifdef NC_MEASURE_CLFU
	int 	owned = 0;
#endif




	switch (lockmode) {
		case CLFU_LM_SHARED:
			DEBUG_ASSERT (root->locktype == CLFU_LT_UPGRADABLE); 
			nc_ulock_rdlock(root->u_rwl);

			break;
		case CLFU_LM_EXCLUSIVE:
			if (root->locktype == CLFU_LT_UPGRADABLE)  {
				nc_ulock_wrlock(root->u_rwl);
			}
			else {
				_nl_lock(root->u_xl, f, l);
			}
#if defined(NC_MEASURE_CLFU)
			if (!owned) {
				root->bl = clfu_lookup_bl_stat(f, l);
				PROFILE_CHECK(root->t_s);
				PROFILE_CHECK(root->t_e);
			}
#endif
			break;
	}

	return 1; /* 중요*/
}
void
clfu_unlock(fc_clfu_root_t *root, char *f, int l)
{

#if defined(NC_MEASURE_CLFU)
	int 			owned = 0;
	perf_val_t		s, e;
	void 			*kept = NULL;




#endif


#if defined(NC_MEASURE_CLFU)

	kept 		=  root->bl;

	if (root->bl) {
		s 	= root->t_s;
		PROFILE_CHECK(e);

		kept = root->bl;
		root->bl = NULL;
	}

#endif


	switch (root->locktype) {
		case CLFU_LT_UPGRADABLE:
			nc_ulock_unlock(root->u_rwl);
			break;
		case CLFU_LT_EXCLUSIVE:
			_nl_unlock(root->u_xl);
#if defined(NC_MEASURE_CLFU)
			owned = _nl_owned(root->u_xl);
#endif
			break;
#if defined (__clang__)
		case CLFU_LT_NULL:
			break;
#endif

	}
#if defined(NC_MEASURE_CLFU)
	if (kept && !owned) {
		double msec;
		msec = (double)(PROFILE_GAP_USEC(s, e)/1000.0);
		clfu_update_bl_stat(kept, msec);
	}
#endif

	return ;
}

void 
clfu_upgradelock(fc_clfu_root_t *root, char *f, int l)
{
	DEBUG_ASSERT(root->locktype == CLFU_LT_UPGRADABLE);

	nc_ulock_upgrade(root->u_rwl);
#if defined(NC_MEASURE_CLFU)
	root->bl = clfu_lookup_bl_stat(f, l);
	PROFILE_CHECK(root->t_s);
	PROFILE_CHECK(root->t_e);
#endif

}


int 
clfu_remove(fc_clfu_root_t *root, lru_node_t *node)
{
	int 			r = 0;
	fc_lru_root_t 	*lru_root;


	lru_root 	= clfu_find_lru(root, node);
	if (lru_root) {
		r = LRU_remove((fc_lru_root_t *)node->root, node);
		LRU_init_node(node, __FILE__, __LINE__); 



		root->kept--;
	}

  	return r;
}
/*
 * remark) 
 * 	this function called in controlled manner.
 *  If the node is on the top LRU, this function would be never called
 */
int 
clfu_upgrade(fc_clfu_root_t *root, fc_lru_root_t *lru, void *data, lru_node_t *ln)
{
	int 				r = 0;




	DEBUG_ASSERT(ln->state != LRU_CACHED);

	ln->hitcount 	= 1;
	ln->clockstamp 	= lru->signature; 
	ln->state 		= LRU_CACHED;


	r = LRU_add(lru, data, ln);


	root->kept++;


	return r;
}
void
clfu_check_pop(fc_clfu_root_t *root, int cursor)
{
	float 	hi_mark, lo_mark;
	float 	v;
	int 	hot_count = 0;



	if (root->kept >= (root->total*0.9) ) {
		/* clfu생성때 주어진hot_ratio값으로 기준이되는 hot_count계산 */
		v = (((float)((float)root->hot_ratio*(float)root->total))/100.0);
		/* 기준값을 기준으로 hi, lo값 추정 */
		hi_mark = v * 1.1;
		lo_mark = v * 0.9;

		/* 실제 hot_count 계산 */
		hot_count = root->kept - root->lru[cursor]->lru_cnt;
		if ((float)hot_count < lo_mark) {
			root->basecount = max(root->basecount-1, CLFU_MIN_BAND_COUNT);
		}
		else if ((float)hot_count > hi_mark) {
			root->basecount = min(root->basecount+1, CLFU_MAX_BAND_COUNT);
		}
	}
}
int
clfu_get_stat(fc_clfu_root_t *root, int *bottom_per, int *base_cnt, int *kept, int *total)
{
	if (bottom_per) {
		if (root->kept > 0) {
			*bottom_per = (int)((root->lru[root->current]->lru_cnt * 100.0)/root->kept);
		}
		else
			*bottom_per = 0;
	}
	if (base_cnt)
		*base_cnt = root->basecount;
	if (kept)
		*kept = root->kept;
	if (total) {
		*total = (root->total > 0? root->total:root->kept);
	}
	return 0;
}
static fc_lru_root_t *
clfu_find_lru(fc_clfu_root_t *root, lru_node_t *cn)
{
	fc_lru_root_t *lru = NULL;

	lru = (fc_lru_root_t *)cn->root;
	if (cn->clockstamp != lru->signature) {
		lru = root->lru[root->current];
		/* side effect 
		 * link_join에서 join시 node의 root갱신을 안하므로, 여기서 1차 보정해야함
		 */
		cn->root 	 = (void *)lru;
		cn->clockstamp = lru->signature;
		cn->hitcount   = 1;  /* 0일 수도 있음...하지만 일단 1로 보정함 */
	}
	return lru;
}
int 
clfu_hit(fc_clfu_root_t *root, lru_node_t *cn, int ntimes)
{
	int				nextband;
	int				curband, homeband;
	fc_lru_root_t	*lru_root = NULL;
	int 			r = 0;



	

	lru_root 		=  clfu_find_lru(root, cn);
	homeband 		= 
	curband 		=  lru_root->id; /* 밴드 id */
	cn->hitcount	+= ntimes;


	if (LFU_TOPMOST(root, curband)) {
		cn->hitcount = min(root->basecount, cn->hitcount); /* limit to basecount */
		r = LRU_hit(lru_root, cn);
	}
	else  {

		while ((cn->hitcount) > root->basecount) {
			nextband  		=  LFU_NEXTBAND(root, curband);
			cn->hitcount 	-= root->basecount;
			curband			= nextband;
			if (LFU_TOPMOST(root, curband))  {
				cn->hitcount = min(root->basecount, cn->hitcount); /* limit to basecount */
				break;
			}	
		}

		if (homeband != curband) {
			clfu_remove(root, cn);
			clfu_upgrade(root, root->lru[curband], cn->bnode.data, cn);
		}
		else
			r = LRU_hit(lru_root, cn);
	}
	/*
	 *
	 * check if lru[cursor]->cnt is inbetween [0.4~0.6] 
	 * 매 5초마다로 주기 변경(2019.8.26)
	 *
	 */
	if (CLFU_NEED_TUNE(root)) { 
		clfu_check_pop(root, root->current);
		root->lasttuned = nc_cached_clock();
	}

	return 0;
}


void *
clfu_reclaim_node(fc_clfu_root_t *root)
{
	int				cursor;//, prevcursor = -1;
	int				reclaimbands = 0;
	lru_node_t 		*cn = NULL;
	int				band_navi_cnt = 0;
	int				navi_cnt = 0;


	cursor = root->current;

	root->chkvictim++; 

	/* check if lru[cursor]->cnt is inbetween [0.4~0.6] */

	if (CLFU_NEED_TUNE(root)) {
		clfu_check_pop(root, root->current);
		root->lasttuned = nc_cached_clock();
	}


	reclaimbands = 0;
	do {
		band_navi_cnt = 0;
		cn = LRU_reclaim_node(root->lru[cursor], &band_navi_cnt);
		navi_cnt += band_navi_cnt;


		if (cn == NULL) {
			cursor = LFU_NEXTBAND(root, cursor);
			if (reclaimbands >= root->bandcount) {
				TRACE((T_WARN, "%s.FATAL: reclaiming gave up after %d band nav.((%d nodes scanned)total bands=%d), kept=%d\n", 
								root->signature, 
								(int)reclaimbands, 
								navi_cnt,
								(int)root->bandcount, 
								(int)root->kept));
				break;
			}
			reclaimbands++;
		}
	} while (cn == NULL);
	root->chkscanned += navi_cnt; 

	if (cn) {

		root->kept--;
		LRU_init_node(cn, __FILE__, __LINE__); 
	}

	return (cn? cn->bnode.data:NULL);
}
long
clfu_kept_count(fc_clfu_root_t *root)
{
	return root->kept;
}


void
clfu_dump(fc_clfu_root_t *root, fc_dump_node_proc_t proc, int forward)
{
	int				cnt, totalcnt = 0;;
	char			*dumpline;
	int				cursor;


	cursor = root->current;
	do {
		if (root->lru[cursor]->lru_cnt > 0)  {
			dumpline= LRU_dump(root->lru[cursor], proc, forward, &cnt);

			if (LFU_TOPMOST(root, cursor))  {
				TRACE((T_ERROR, "*[%02d]/%d:%d : %s\n", cursor, root->lru[cursor]->lru_cnt, cnt, (dumpline?dumpline:"NULL")));
			}
			else {
				TRACE((T_ERROR, "[%02d]/%d:%d : %s\n", cursor, root->lru[cursor]->lru_cnt, cnt, (dumpline?dumpline:"NULL")));
			}

			if (dumpline) {
				XFREE(dumpline);
			}
			totalcnt += cnt;
		}
		cursor = LFU_NEXTBAND(root, cursor);
	} while (root->current != cursor);

	TRACE((T_INFO, "CLFU - %d items cached\n", totalcnt));
}
void
clfu_for_each(fc_clfu_root_t *root, int (* proc)(void *, void *), void *ud, int forward)
{
	int			i;
	int			cycled = 0;

	for (	i = (root->current -1 + root->bandcount) % root->bandcount;
			!cycled ;
			i = (i -1 + root->bandcount) % root->bandcount ) {


		if (root->lru[i]->lru_cnt > 0)  {
			LRU_for_each(root->lru[i], proc, ud, forward);
		}

		if (i == root->current) cycled++;
	}
}
int
clfu_is_hot(fc_clfu_root_t *root, lru_node_t *cn)
{
	int 	ishot = 0;
	int 	bottom = 0;



	/*
 	 * current는 가장 낮은 밴드
	 */
 	bottom = root->current;

	ishot = ((void *)cn->root != (void *)root->lru[bottom]);

	return ishot;
}


/*
 *
 ****************************** CLFU lock duration ranking tool ************************************
 *
 */
#ifdef NC_MEASURE_CLFU
/*
 * path-lock이 다양한 위치에서 호출되는데 각 위체에서 획득된 lock이
 * release될 떄까지 걸린 시간을 측정하고, 나중에
 * lock의 평균 유지 시간, 호출된 횟수 등으로 분석하기위해서 사용
 *
 */




struct tag_bl_t {
	char		*file;
	int			line;
	double		sum;
	double		vmin;
	double		vmax;
	double		vavg;
	long long	count;
};

static 	void 			*s__bl_tbl = NULL;
static  int 	 		s__bl_array_cnt = 0;
static  int 	 		s__bl_array_alloc = 0;
static  bl_t 	 		**s__bl_array = NULL;
static  pthread_mutex_t s__bl_lock = PTHREAD_MUTEX_INITIALIZER;
/*
 * @f : source file name
 * @l : source line #
 * @msec : l@f에서 획득한 lock이 해제될 떄 까지 걸린 시간(msec)
 *
 * 소스에서 l@f에서 획득한 lock의 보유시간을 모두 합산
 */
static int
clfu_compare_bl_stat(const void *a, const void *b)
{
    bl_t   *pa = (bl_t *)a;
    bl_t   *pb = (bl_t *)b;
    int     r;

    r =  strcmp(pa->file, pb->file);
    if (r) return -r;
    return -1.0 * (pa->line - pb->line);

}
static bl_t *
clfu_lookup_bl_stat(char *f, int l)
{
	bl_t			key = {f, l, 0, 0};
	bl_t			**ap 	= NULL;
	bl_t			*found 	= NULL;

	pthread_mutex_lock(&s__bl_lock);
	if (s__bl_tbl)
		ap = (bl_t **)tfind(&key, &s__bl_tbl, clfu_compare_bl_stat);

	if (ap) {
		found 		= *ap;
	}
	else {
		found 		= (bl_t *)malloc(sizeof(bl_t));
		found->file = f;
		found->line = l;
		found->sum  = 0;
		found->count= 0;

		if (s__bl_array_cnt >= s__bl_array_alloc) {
			s__bl_array_alloc += 128;
			s__bl_array = realloc(s__bl_array, s__bl_array_alloc*sizeof(bl_t *));
		}
		s__bl_array[s__bl_array_cnt] = found;
		ap = (bl_t **)tsearch(found, &s__bl_tbl,  clfu_compare_bl_stat);
		s__bl_array_cnt++;
	}
	pthread_mutex_unlock(&s__bl_lock);
	return found;
}
static void
clfu_update_bl_stat(bl_t *found, double msec)
{

	pthread_mutex_lock(&s__bl_lock);
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
	pthread_mutex_unlock(&s__bl_lock);
}
static int
clfu_sort_by_holdtime(const void *a, const void *b)
{
	bl_t 	*pa = *(bl_t **)a;
	bl_t 	*pb = *(bl_t **)b;
	double 	r;

	r =  (pa->vavg - pb->vavg);
	if (!r) return r;

	return (int)(r > 0.0? 1:-1);
}
void
clfu_report_bl_stat()
{
	int		i ;
	char	zc[64];

	pthread_mutex_lock(&s__bl_lock);
	for (i = 0; i < s__bl_array_cnt; i++) {
		if (s__bl_array[i]->count > 0)
			s__bl_array[i]->vavg = (double)s__bl_array[i]->sum/(1.0 * s__bl_array[i]->count);
		else
			s__bl_array[i]->vavg = 0.0;
	}

	qsort(s__bl_array, s__bl_array_cnt, sizeof(bl_t *), clfu_sort_by_holdtime);

	TRACE((T_MONITOR, "%15s | %6s | %6s | %6s | %64s\n", "count", "avg", "min", "max", "source"));
	for (i = 0; i < s__bl_array_cnt; i++) {
		TRACE((T_MONITOR, "%15s | %6.2f | %6.2f | %6.2f | %s:%d\n",
						ll2dp(zc, sizeof(zc), s__bl_array[i]->count), 
						s__bl_array[i]->vavg, 
						s__bl_array[i]->vmin, 
						s__bl_array[i]->vmax, 
						s__bl_array[i]->file, 
						s__bl_array[i]->line));
		s__bl_array[i]->vavg = 0.0; 
		s__bl_array[i]->vmin = 0.0; 
		s__bl_array[i]->vmax = 0.0; 
		s__bl_array[i]->count = 0; 

	}
	pthread_mutex_unlock(&s__bl_lock);

}
#endif
