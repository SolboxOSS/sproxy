#ifndef __CYCLIC_LFH_H__
#define __CYCLIC_LFH_H__
#define	INLINE inline
//#define	INLINE 



/*
 * PUBLIC APIs
 *	clfu_hit(...)
 *	clfu_add(...)
 *	clfu_remove(...)
 *	clfu_reclaim(...)
 *	clfu_spin(...)
 */

#include "lru.h"
#include "util.h"
#include "trace.h"

#define CLFU_MAX_BAND_COUNT	255
#define CLFU_MIN_BAND_COUNT	2
#define CLFU_DEFAULT_BAND_COUNT	4

#define	LFU_NEXTBAND(R, bi)	((bi + 1 + (R)->bandcount) % (R)->bandcount)
#define	LFU_BANDOFFSET(R, bi)	(((R)->current +(bi) + (R)->bandcount) % (R)->bandcount)
#define	LFU_DISTANCE(R, bi)	((bi - (R)->current + (R)->bandcount) % (R)->bandcount)
/* LFU_BASECOUNT should be optimized laster */
#define	LFU_BASECOUNT(R, bi)	(LFU_DISTANCE(R, bi)*(R)->basecount)
#define	LFU_HIGHESTBAND(R,bi)	(LFU_DISTANCE(R, bi) == (R)->bandcount-1)
#define LFU_BAND(R, a)			((int)(((char *)a - (char *)R->lru)/sizeof(fc_lru_root_t *)))
#define	LFU_TOPMOST(R, b)		(((R->current -1 + R->bandcount) % R->bandcount) == (b)) 
#define CLFU_MAX_LRUW_ARRAY 	13
#define CLFU_LRUW_KEY(x) 		((long long)(x) % CLFU_MAX_LRUW_ARRAY)
#define CLFU_SIGNATURE 		0x55AA


typedef enum {CLFU_LT_NULL=0, CLFU_LT_UPGRADABLE=1, CLFU_LT_EXCLUSIVE=2} clfu_lock_type_t;
typedef enum {CLFU_LM_SHARED=1, CLFU_LM_EXCLUSIVE=2} clfu_lock_mode_t;

typedef struct {
	char 			signature[128];
	int				basecount;
	int				hot_ratio; /* default 70% */
	int				total;
	int				current;
	int				bandcount;
 	int 			debug;
	unsigned int 	clock; /* initially 0, increased by 1 in every spin */
	time_t			lasttuned;
	long				kept;
	clfu_lock_type_t	locktype;
	union {
		nc_ulock_t 		*rwl;
		nc_lock_t		*xl;
		void			*lock;
	} u;

	/*
	 * dynamic stat
	 */
	nc_int64_t			chkscanned; /* navi'ed nodes to find victim */
	nc_int64_t			chkvictim;  /* victime check count */

	fc_lru_root_t	**lru;		/* in periodic way, we move ptr to lru to the next band */
#ifdef NC_MEASURE_CLFU
	void 			*bl;
	perf_val_t		t_s;
	perf_val_t		t_e;
#endif
} fc_clfu_root_t;
#define		u_xl 	u.xl
#define		u_rwl 	u.rwl



#define 	CLFU_LOCK(clfu_, lm_) 	clfu_lock(clfu_, lm_, __FILE__, __LINE__)
#define 	CLFU_UNLOCK(clfu_) 		clfu_unlock(clfu_, __FILE__, __LINE__)

fc_clfu_root_t * clfu_create(void *givenlock, clfu_lock_type_t locktype, int bandcount, fc_check_idle_t check_idle, fc_dump_t dumper, int blockcount, int bottomratio);
void clfu_set_debug(fc_clfu_root_t *root);
fc_clfu_root_t * clfu_set_max(fc_clfu_root_t *root, int cnt);
void clfu_clean(fc_clfu_root_t *mptr, fc_destructor_t free_proc);
int clfu_spin(fc_clfu_root_t *clfu_root);
int clfu_cached(lru_node_t *node);
int clfu_add_nolock(fc_clfu_root_t *root, void *data, lru_node_t *cn);
int clfu_add(fc_clfu_root_t *root, void *data, lru_node_t *node);
int clfu_lock(fc_clfu_root_t *root, clfu_lock_mode_t, char *f, int l);
void clfu_upgradelock(fc_clfu_root_t *root, char *f, int l);
void clfu_unlock(fc_clfu_root_t *root, char *f, int l);
int clfu_remove(fc_clfu_root_t *root, lru_node_t *node);
int clfu_upgrade(fc_clfu_root_t *root, fc_lru_root_t *lru, void *data, lru_node_t *ln);
void clfu_check_pop(fc_clfu_root_t *root, int cursor);
int clfu_get_stat(fc_clfu_root_t *root, int *bottom_per, int *base_cnt, int *kept, int *total);
int clfu_hit(fc_clfu_root_t *root, lru_node_t *cn, int hitcnt);
void clfu_set_signature(fc_clfu_root_t *root, const char *signature);
void * clfu_reclaim_node(fc_clfu_root_t *root);
void clfu_dump(fc_clfu_root_t *root, fc_dump_node_proc_t proc, int forward);
void clfu_for_each(fc_clfu_root_t *root, int (* proc)(void *, void *), void *ud, int forward);
int clfu_is_hot(fc_clfu_root_t *root, lru_node_t *cn);
int clfu_current_percent(fc_clfu_root_t *root);
long clfu_kept_count(fc_clfu_root_t *root);

#endif
