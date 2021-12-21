/*
 * partition manager 
 */
#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_SYS_DIR_H
#include <sys/dir.h>
#endif
#ifdef HAVE_DIR_H
#include <dir.h>
#endif
#include <dirent.h>
#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#ifdef WIN32
#include <statfs.h>
#endif
#ifndef WIN32
#include <sys/vfs.h>
#endif
#include <math.h>
#include <ctype.h>
#include "disk_io.h"
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "bitmap.h"
#include <threadpool.h>
#include "trace.h"
#include "bt_timer.h"
#include <mm_malloc.h>

#define 	PM_MAX_PLC_SIZE 	100

typedef struct tag_part_table_info {
	link_list_t			list;
	pthread_mutex_t		lock;
	int					count;
	int					reclaim_lowmark; /*= DM_RECLAIM_LOWMARK; */
	int					reclaim_highmark; /*= DM_RECLAIM_HIGHMARK;*/
	bt_timer_t			timer_load_monitor;
#ifdef NC_MULTIPLE_PARTITIONS
	tp_handle_t 			query_handler;
#endif
	link_list_t 		frees;

} part_table_info_t;
struct tag_query_inode;
typedef struct tag_query_inode_inode_quark {
	struct tag_query_inode *query;
	int 					jindex; /* partition index */
	int 					valid;
}  part_query_inode_quark_t;


#define 	DEBUG_MAGIC 	0xA11A
typedef struct tag_query_inode {
	nc_uint16_t 			magic;

	pthread_mutex_t 		lock;
	pthread_cond_t 			signal;
	char 					*signature;
	char 					*path;

	int 					casepreserve;
	nc_uint8_t 				found; /* 1 if entry found in a partition */
	nc_uint8_t 				pickedup; /* 1 if result picked up */
	int 					handled; /* # of lookuped partition */
	int 					count; /* # of active paritions when creating this job*/
	nc_part_element_t 		**part;
	nc_mdb_inode_info_t		*minode;
	part_query_inode_quark_t *slice;
} part_query_inode_t;


extern int 				g__nc_inited;
extern int				__terminating;
extern __thread int		__nc_errno;
extern void *			__timer_wheel_base;

static int 						pm_update_usage(nc_part_element_t *pe);
static int 						pm_prepare_partition(char *path);
static void *  					pm_reclaim_thread(void *v);
#ifdef NC_MULTIPLE_PARTITIONS
static int 						pm_do_query(void *d, void *tcb);
#endif
static nc_mdb_inode_info_t * 	pm_load_disk_inode_from_partition(nc_part_element_t *part, int preservecase, char *signature, char *qid);
static int 						pm_purge_cache_callback(nc_volume_context_t *volume, char *key, char *path, uuid_t uuid, nc_int64_t, int ishard, int istempl, nc_path_lock_t *pl, nc_part_element_t *, void *ctx);
static int pm_reclaim_if_possible(nc_volume_context_t *volume, char *path, char *key, uuid_t uuid, int istempl, nc_path_lock_t *pl, nc_part_element_t *part);



static size_t 					__part_maxblksize = 512;
static part_table_info_t		__part_table;






/*
 * 	LOOKUP-CACHE OPERATIONS
 *
 */



static void
pm_recalc_align(const char *path, size_t blksiz)
{
	size_t 	t = __part_maxblksize;
	__part_maxblksize = max(__part_maxblksize, blksiz);
	if (t != __part_maxblksize) {
		TRACE((T_INFO, "Partition[%s]:block size '%ld B', chosen as a default align unit(old=%ld)\n", path, (long)__part_maxblksize, (long)t));
	}

}
size_t
pm_get_align_size()
{
	return __part_maxblksize;
}
void
pm_free_minode(nc_mdb_inode_info_t *mi)
{
	if (mi->vol_signature) {
		XFREE(mi->vol_signature);
		mi->vol_signature = NULL;
	}
	if (mi->q_path) {
		XFREE(mi->q_path);
		mi->q_path = NULL;
	}
	if (mi->q_id) {
		XFREE(mi->q_id);
		mi->q_id = NULL;
	}
	if (mi->property) {
		kv_destroy(mi->property);
		mi->property = NULL;
	}
	XFREE(mi);
}

static void
pm_check_unlock()
{
#ifndef WIN32
	DEBUG_ASSERT(__part_table.lock.__data.__owner != trc_thread_self());
#endif
}






static void
pm_update_CDF()
{
	int		ioper = 0;
	int		iomax = 0;
	int		ioroom = 0;
	int		r_anch = 0;
	int 	pcand = 0;
	nc_part_element_t	*p, *last_p = NULL;;

	pthread_mutex_lock(&__part_table.lock);
	p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	while (p ) {
		if (pm_check_under_mark(p, PM_CHECK_HIGH, U_FALSE)) { 
			if (iomax < p->ios) {
				iomax = p->ios;
			}
			p->ios = max(1, p->ios);
			if ((p->valid & PS_WOK) == 0)  {
				TRACE((T_DEBUG, "partition[%s] - included 'cause enough space\n", p->path));
				p->valid |= PS_WOK;
			}
		}
		else {
			if ((p->valid & PS_WOK) != 0)  {
				TRACE((T_DEBUG, "partition[%s] - excluded 'cause not enough space\n", p->path));
				p->valid = p->valid & ~PS_WOK;
			}
		}
		p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
	}
	if (iomax < __part_table.count) {
		iomax = __part_table.count;
	}
	pcand = 0; /* # of writable partitions */
	p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	while (p  && p->valid) {
		if ((p->valid & PS_WOK) != 0) {
			ioroom += iomax - p->ios;
			pcand++;
		}
		p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
	}
	ioroom = max(1, ioroom);
	/* calc random range */
	p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	if (pcand > 1) {
		/*
		 * load-balancing is available only if # of writable partitions is larger than 1 
		 */
		while (p  && p->valid) {
			if ((p->valid & PS_WOK) != 0) {
				ioper = (int)round(((iomax - p->ios)*100.0)/ioroom);
				p->r_s = r_anch;
				p->r_e = max(r_anch+ioper -1, p->r_s);
				if (p->r_s == p->r_e) {
					p->valid = p->valid & ~PS_WOK;	 /* temporarily disable */
					TRACE((T_INFO, "partition[%s] - excluded 'cause too busy\n", p->path));
				}
				else {
					r_anch = p->r_e +1;
					TRACE((T_DEBUG, "partition[%s] - iomax=%d, ios=%d, ioper=%d, r_anch=%d, [%d : %d ]\n",
								p->path, iomax, p->ios, ioper, r_anch, p->r_s, p->r_e));
					last_p = p;
				}
			}
			else {
				TRACE((T_INFO, "partition[%s] - excluded 'cause write prohibited\n", p->path, (float)p->fs_usedper));
			}
			p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
		}
		if (last_p) {
			last_p->r_e = min(99, last_p->r_e); /* to avoid arithmetic underflow */
			last_p->r_e = max(99, last_p->r_e); /* to avoid arithmetic underflow */
		}
	}
	else {
		/* only one partition active */
		if ((p->valid & PS_WOK) != 0) {
			p->r_s = 0;
			p->r_e = 99;
		}
		else {
			p->r_s = 0;
			p->r_e = 0;
		}
	}
	pthread_mutex_unlock(&__part_table.lock);
}
static void 
pm_load_monitor(void *d)
{
	int 	i;
	nc_part_element_t	*pel; /*, *last_p = NULL;; */
	nc_part_element_t	*part[255];
	int 				 part_count = 0;

	


	if (__part_table.count <= 0) return;

	

	TRACE((T_DEBUG, "partition count = %d\n", __part_table.count));
	pthread_mutex_lock(&__part_table.lock);
	pel = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	while (pel) {
		part[part_count++] = pel;

		pel = (nc_part_element_t *)link_get_next(&__part_table.list, &pel->node);
	}
	pthread_mutex_unlock(&__part_table.lock);

	/* update disk usage  without lock*/
	for (i = 0; i < part_count;i++) {
		pm_check_under_mark(part[i], PM_CHECK_HIGH, U_TRUE);
	}

	pm_update_CDF();
	pm_check_unlock();

	bt_set_timer(__timer_wheel_base,  &__part_table.timer_load_monitor, 5000/* 5 sec*/, pm_load_monitor, (void *)NULL);

	
}
void
pm_stop()
{
#ifdef NC_MULTIPLE_PARTITIONS
	tp_stop(__part_table.query_handler);
#endif
	bt_destroy_timer(__timer_wheel_base,  &__part_table.timer_load_monitor);
	pthread_mutex_destroy(&__part_table.lock);
}

void
pm_get_winfo(int *config, int *running)
{
#ifdef NC_MULTIPLE_PARTITIONS
	*running 	= tp_get_workers( __part_table.query_handler); 
	*config 	= tp_get_configured( __part_table.query_handler);
#else
	*running 	= 0;
	*config 	= 0;
#endif
}
int
pm_init(int high, int low)
{
	pthread_mutexattr_t	mattr;
#ifdef NC_MULTIPLE_PARTITIONS
	int 				nrcpu = 0;
#endif
	INIT_LIST(&__part_table.list);
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&__part_table.lock, &mattr);
	pthread_mutexattr_destroy(&mattr);
	__part_table.count = 0;
	__part_table.reclaim_lowmark = low; 
	__part_table.reclaim_highmark = high;
	bt_init_timer(&__part_table.timer_load_monitor, "partition load monitor", U_FALSE);
	bt_set_timer(__timer_wheel_base,  &__part_table.timer_load_monitor, 10/* 10 sec*/, pm_load_monitor, (void *)NULL);
#ifdef NC_MULTIPLE_PARTITIONS
	nrcpu = nr_cpu();
	__part_table.query_handler = tp_init("Parts Loader", 1, 8, pm_do_query, NULL, NULL);
	if (__part_table.query_handler)
		tp_start(__part_table.query_handler);
#endif
	return 0;
}

int
pm_check_partition()
{
	return __part_table.count;
}
nc_part_element_t *
pm_add_partition(char *path, int weight)
{
	nc_part_element_t	*p = NULL;
	struct statfs		ps;
	int					match = 0;

	if (access(path, R_OK|W_OK) != 0) {
		return NULL; /* access permission error */
	}

	if (statfs(path, &ps) != 0) {
		return NULL;
	}

	pm_recalc_align(path, ps.f_bsize);
	/* before acquiring the global lock, we need to create cache directories */
	pm_prepare_partition(path); /* create cache directories if not exists */

	pthread_mutex_lock(&__part_table.lock); 
	p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	while (p && (match == 0)) {
		match = (strcasecmp(path, p->path) == 0);

		p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
	}
	if (match) {
		pthread_mutex_unlock(&__part_table.lock); 
		TRACE((T_WARN, "partition(%s) - already added\n", path));
		/* duplicate addition */
		return p;
	}
	/* not found, add this one */
	p = (nc_part_element_t *)XMALLOC(sizeof(nc_part_element_t), AI_VOLUME);
	p->path			= XSTRDUP(path, AI_VOLUME);
	p->weight		= weight;
	p->ios			= 0;
	p->r_s = p->r_e	= 0;
	p->valid		= PS_ROK;
	p->devid		= 0; /* not used , 2015.6.10*/
	p->mdb			= mdb_open(p, path);
	if (!p->mdb) {
		/* partition creation failed */
		TRACE((T_ERROR, "partition(%s) - failed to create MDB\n", path));
		XFREE(p);
		pm_check_unlock();
		return NULL;
	}
	INIT_NODE(&p->node);
	pm_update_usage(p);
	link_append(&__part_table.list, p, &p->node);
	__part_table.count++;
	p->needstop	= 0;
	p->stopped	= 0;

	//my_pthread_create(&p->reclaimer, &tattr, pm_reclaim_thread, (void *)p);
	pthread_create(&p->reclaimer, NULL , pm_reclaim_thread, (void *)p);
	pthread_setname_np(p->reclaimer, "reclaim");

#if 0
	/* 
	 * partition의 lock 생성
	 */
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&p->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);
#else
	pthread_rwlock_init(&p->rwlock, NULL);
#endif

	pthread_mutex_unlock(&__part_table.lock); 
	pm_load_monitor(NULL);
	TRACE((T_INFO, "Partition[%s]:successfully added\n", path));
	pm_check_unlock();
	return p;
}
void
pm_update_ios(nc_part_element_t *pe, int nio)
{
	pthread_mutex_lock(&__part_table.lock);
	pe->ios += nio;
	pthread_mutex_unlock(&__part_table.lock);
}
static int
pm_update_usage(nc_part_element_t *pe)
{
	struct statfs	sf;
	long long 		block_used = 0;



	if (statfs(pe->path, &sf) != 0) {
		TRACE((T_WARN, "partition[%s] - statfs error[%s]\n", pe->path, strerror(errno)));
		return -1;
	}

	pthread_rwlock_wrlock(&pe->rwlock);
	if (sf.f_blocks > 0) {
		block_used = sf.f_blocks - sf.f_bfree;
		if (block_used == 0) {
			pe->fs_usedper = 0.0;
		}
		else {
			/* the following fomula borrowed from linux df.c */
			pe->fs_usedper = (block_used*100.0/(block_used + sf.f_bavail) + 0.5);
		}
		pe->valid		|= PS_ROK;
	}
	pthread_rwlock_unlock(&pe->rwlock);
	TRACE((T_DEBUG, "Partition[%s]:usage=%.2f %%\n", pe->path, pe->fs_usedper));
	return 0;
}
nc_part_element_t *
pm_elect_part(nc_size_t size, int bforce)
{
	nc_uint32_t	R = 0;
	char			logbuf[1024];
	char			*plog = logbuf;
	int				n, remained = sizeof(logbuf)-1;
	nc_part_element_t *p = NULL, *pdefault = NULL;
	nc_part_element_t *elected = NULL;
	pthread_mutex_lock(&__part_table.lock);

	
	R = get_random();
	R = R % 100;
	pdefault = p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	while (p) {
		if ((p->valid & PS_WOK) != 0) {
			n = snprintf(plog, remained, "%s[%d:%d]", p->path, p->r_s, p->r_e);
			remained -= n;
			plog += n;
			if (!elected && (p->r_s <= R) && (R <= p->r_e)) {
				/* found */
				elected = p;
				break; /* commented for debugging */
			}
		}
		p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
	}

	*plog = 0;
	TRACE((T_DEBUG, "PARTS(R=%d):%s => %s(total=%d) \n", R, logbuf, (elected?elected->path:"null"), __part_table.count));

	pthread_mutex_unlock(&__part_table.lock);
	pm_check_unlock();

	/*
	 * force=TRUE인경우 강제 선택
	 */
	if (elected) pdefault = elected;

	return (bforce?pdefault:elected);
}
nc_mdb_inode_info_t *
pm_load_disk_inode(int casepreserve, char *signature, char *qid)
{
	nc_mdb_inode_info_t		*minode = NULL;
#ifdef __PROFILE
	perf_val_t	ms, me;
	long long	ud;
#endif
#ifdef NC_MULTIPLE_PARTITIONS
	nc_part_element_t		*p = NULL;
 	int 					i;
	int 					part_count = 0;
	part_query_inode_t 		*query = NULL;
#endif
	nc_part_element_t		*part = NULL;


#ifdef NC_MULTIPLE_PARTITIONS

	pthread_mutex_lock(&__part_table.lock);
	if (link_empty(&__part_table.list)) {
		pthread_mutex_unlock(&__part_table.lock);
		pm_check_unlock();
		
		return NULL;
	}
	pthread_mutex_unlock(&__part_table.lock);

	query = (part_query_inode_t *)XMALLOC(sizeof(part_query_inode_t), AI_VOLUME);
	MUTEX_INIT(&query->lock, NULL);
	condition_init(&query->signal);

	query->magic 		= DEBUG_MAGIC;
	query->signature 	= XSTRDUP(signature, AI_VOLUME);
	query->path 		= XSTRDUP(qid, AI_VOLUME);
	query->found 		= 0;
	query->casepreserve = casepreserve;
	query->handled  	= 0;
	query->count  		= 0;
	query->pickedup 	= 0;
	query->minode  		= NULL;
	/*
	 * 모든 파티션에 query를 뿌려서 실행해야함
	 */
	query->part  		= (nc_part_element_t **)XMALLOC(sizeof(nc_part_element_t *)*__part_table.count, AI_VOLUME);

	pthread_mutex_lock(&__part_table.lock);
	p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	while (p) {
		DEBUG_ASSERT(mdb_check_valid(p->mdb));
		query->part[part_count++] = p;
		query->count++;
		p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
	}
	pthread_mutex_unlock(&__part_table.lock);


	DO_PROFILE_USEC(ms, me, ud) {
		query->slice = (part_query_inode_quark_t *)XMALLOC(sizeof(part_query_inode_quark_t)*part_count, AI_VOLUME);

		/*
		 * partition별 요청을 등록
		 */
		for (i = 0; i < part_count; i++) {
			query->slice[i].jindex 	= i;
			query->slice[i].valid 	= 1;
			query->slice[i].query 	= query;
			tp_submit(__part_table.query_handler, (void *)&query->slice[i]);
		}
		/* wait until found or looked up throughly */
		pthread_mutex_lock(&query->lock);
		/*
 		 * 처리된 결과 조건 
		 * 	- 모든 slice들의 실행이 완료(query->handled == query->count)
		 *	- slice중에 어느 하나의 실행에서 query->found가 1로 설정됨
		 *
		 * query->handled == query->count || * query->found
		 */
		while (! ((query->handled == query->count) || ( query->found == 1))) {
			condition_wait(&query->lock, &query->signal, 500, NULL, NULL, U_FALSE);
		}
		minode = query->minode;
		pthread_mutex_unlock(&query->lock); 
		query->pickedup = 1; /* unlock후하고 이후에 query메모리는 접근 불가 */

		TRACE((T_INODE, "Volume[%s]:CKey[%s] - done(%s)\n", 
						signature, 
						qid, 
						(minode?"found":"not found")
						));


	}
#else
	part = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	if (!part) return NULL;

	DO_PROFILE_USEC(ms, me, ud) {
		minode = pm_load_disk_inode_from_partition(part, casepreserve, signature, qid);
	}

#endif


#ifdef __PROFILE
	if (ud > 1000000) {
		TRACE((T_INFO, "Volume[%s]:CKey[%s] - lookup slow, %.2f msec\n",
						signature, 
						qid, 
						(ud/1000.0)));
	}
#endif
	pm_check_unlock();
	
	return minode;
}
char *
pm_create_cache_path(nc_part_element_t *part, int ispriv, uuid_t uuid, nc_uint32_t ai_flag)
{
	char		*buf = NULL;
	char		*pf = NULL;
	int			n, i;
	nc_uint8_t	*puuid = (nc_uint8_t *)uuid;



	buf = (char *)XMALLOC(strlen(part->path) + 16*2 + 10, ai_flag);
	pf = buf;
	if (ispriv == 0)
		n  = sprintf(pf, "%s/%02X/%02X/", part->path, puuid[1], puuid[5]);
	else {
		/*
		 * ispriv = TRUE 의 경우 
		 * 객체의 경로는 <partition>/private/<uuid>
		 * 가 됨
		 */
		n  = sprintf(pf, "%s/private/", part->path);
	}
	pf += n;

	for (i = 0; i < sizeof(uuid_t); i++) {
		n = sprintf(pf, "%02x", (puuid[i] & 0xFF));
		pf += n;
	}
	*pf = 0;
	return buf;
}
int
pm_reuse_inode(nc_part_element_t *part,  uuid_t uuid, nc_int64_t rowid, nc_time_t vtime, nc_uint32_t new_cversion)
{
	int		r = 0;

	

	/*
	 * MDB를 먼저 지워야 racing condition 발생하지 않음
	 */
	DEBUG_ASSERT(part != NULL);
	DEBUG_ASSERT(part->mdb != NULL);

	r = mdb_reuse(part->mdb, uuid, rowid, vtime, new_cversion); 

	
	return r;
}


int
pm_remove_metainfo(fc_inode_t *inode)
{
	int		r = -1;
	char	zuuid[128];


	

	/*
	 * MDB를 먼저 지워야 racing condition 발생하지 않음
	 */
#ifndef NC_RELEASE_BUILD
	/*
	 * 성능저하 예상
	 */
	{
		nc_path_lock_t	*tpl = NULL;
		tpl = nvm_path_lock_ref(inode->volume, inode->q_path, __FILE__, __LINE__);
		DEBUG_ASSERT_PATHLOCK(tpl, nvm_path_lock_is_for(tpl, inode->q_path));
		DEBUG_ASSERT_PATHLOCK(tpl, nvm_path_lock_owned(inode->volume, tpl));

		nvm_path_lock_unref(inode->volume, tpl, __FILE__, __LINE__);
	}
#endif

	r= mdb_remove_rowid_direct(inode->part->mdb, inode->uuid, inode->rowid);
	TRACE((T_INODE,"Volume[%s].Key[%s]/UUID[%s] - INODE[%d]/ROWID[%lld]MDB removed\n", 
					inode->volume->signature, 
					inode->q_id, 
					uuid2string(inode->uuid,zuuid, sizeof(zuuid)),
					inode->uid,
					inode->rowid
					));
	
	return r;
}


nc_int64_t
pm_add_cache(nc_part_element_t *part, fc_inode_t *inode, int binsert)
{
	int			r;
	DEBUG_ASSERT(part != NULL);

	if (binsert) {
		r = mdb_insert(inode->part->mdb, inode);
	}
	else {
		r = mdb_update_rowid(inode->part->mdb,
							inode->uuid,
							inode->rowid,
							inode->size,
							inode->rsize,
							inode->vtime,
							inode->imode,
							inode->ob_obit,
							NULL);
	}
	return r;
}

/*
 * partition 준비 단계에서 호출되어서
 * partition의 private 디렉토리 아래에
 * 지워지지 않은 파일들 모두 삭제
 */
static void
pm_cleanup_privates(char *dir)
{
    DIR             *dp =NULL;
    struct dirent   *dirp ;
	char 			path[1024];

    dp = opendir(dir) ;
	if (!dp) return;


    while((dirp = readdir(dp)) != NULL) {

        if(strcmp( dirp->d_name, ".") == 0 || strcmp(dirp->d_name,"..") == 0) {
            continue ;
        }
		sprintf(path, "%s/%s", dir, dirp->d_name);
		if (unlink(path) < 0) { /* 경로가 file인지 아닌지 구별안함 */
			TRACE((T_ERROR, "Private cache, '%s' unlink failed:%s\n", path, strerror(errno)));
		}
		else {
			TRACE((T_INFO, "Private cache, '%s' unlink OK:%s\n", path));
		}

	}

	closedir(dp);

}
static int
pm_prepare_partition(char *path)
{
	int 		i, j, nw;
	char 		cdir[NC_MAXPATHLEN];
	char		*p, *pc;


	

	p = (char *)path + strlen(path) -1 ;
	while (*p == ' ') *p--=0;


	sprintf(cdir, "%s/private", path);
	if (mkdir(cdir, 0755) < 0) {
		TRACE((T_INFO, "Creating %s failed(%s)\n", cdir, strerror(errno)));
	}
	if (errno == EEXIST) {
		TRACE((T_INFO, "Cleaning up %s ... \n", cdir));
		pm_cleanup_privates(cdir);
	}

	for (i = 0; i < NC_CACHE_DIR_COUNT; i++) {
		if (*p == '/')
			nw = snprintf(cdir, NC_MAXPATHLEN, "%s%02X", path, i);
		else
			nw = snprintf(cdir, NC_MAXPATHLEN, "%s/%02X", path, i);
		cdir[nw] = 0;
#ifdef WIN32
		if (mkdir(cdir) < 0) 
#else
		if (mkdir(cdir, 0755) < 0) 
#endif
		{
			if (errno != EEXIST) {
				TRACE((T_ERROR, "mkdir('%s', 0755) - failed:'error=%d'\n", (char *)cdir, (int)errno));
				
				return -1;
			}
		}
		pc = cdir + nw;
		for (j = 0; j < NC_CACHE_DIR_COUNT;j++) {
			nw = snprintf(pc, NC_MAXPATHLEN-nw, "/%02X", j);
			*(pc+nw) = '\0';
#ifdef WIN32
			if (mkdir(cdir ) < 0) 
#else
			if (mkdir(cdir, 0755) < 0) 
#endif
			{
				if (errno != EEXIST) {
					TRACE((T_ERROR, "mkdir('%s', 0755) - failed:'error=%d'\n", (char *)cdir, (int)errno));
					
					return -1;
				}
			}
		}
	}
	
	return 0;
}
int
pm_check_under_mark(nc_part_element_t *part, int markv, int bupdate)
{
	int r = 0;
	if (bupdate)
		pm_update_usage(part);
	

	DEBUG_ASSERT((markv == PM_CHECK_LOW) || (markv == PM_CHECK_HIGH));
	pthread_rwlock_rdlock(&part->rwlock);
	switch (markv) {
		case PM_CHECK_LOW:
			if (part->fs_usedper < (1.0 * __part_table.reclaim_lowmark))
				r++;
			
			TRACE((T_DEBUG, "part[%s] : %.2f < %.2f = %d\n", part->path, part->fs_usedper, (float)1.8*__part_table.reclaim_lowmark, r));
			break;
		case PM_CHECK_HIGH:
			if (part->fs_usedper < (1.0 * __part_table.reclaim_highmark))
				r++;
			TRACE((T_DEBUG, "part[%s] : %.2f < %.2f = %d\n", part->path, part->fs_usedper, (float)1.0*__part_table.reclaim_lowmark, r));
			break;
		default:
			TRACE((T_DEBUG, "part[%s] : unknown mark!\n", part->path));
			break;
	}
	pthread_rwlock_unlock(&part->rwlock);
	return r;
}


static void  *
pm_reclaim_thread(void *v)
{
	nc_part_element_t *p = (nc_part_element_t *)v;
#define	MAX_RECLAIM_COUNT	100
	char			zuuid[64];
	struct purge_info	pi[MAX_RECLAIM_COUNT]; 
	int					count = 0;
	int					i;
	int					removed = 0, removed_total = 0;
	int					first = 1;
	int 				fetched = 0;
	long 				handled = 0;
	long 				skipped = 0;
	nc_volume_context_t	*volume = NULL;
	nc_path_lock_t		*pl = NULL;
	int 				res = 0;
	mdb_tx_info_t		txno;
	int					need_reclaim;
	int					bw = 0;
	int					rip = 0;
	char				ebuf[128];

	
	TRACE((T_DEBUG, "partition[%s] - check watermark...\n", p->path));
	removed_total = 0;
	while (!p->needstop) {

		if (g__nc_inited == 0) {
			/* not yet initialized */
			bt_msleep(1000);
			continue;
		}
		removed = 0;
		handled = 0;
		if (!pm_check_under_mark(p, PM_CHECK_LOW, U_TRUE)) { 
			TRACE((T_DEBUG, "partition[%s] - is going to reclaimed\n", p->path));
			first = 1;
			removed = 0;
			while (p->needstop == 0 && !pm_check_under_mark(p, PM_CHECK_LOW, U_TRUE) && (first || removed > 0)) 
			{ 
				count 	= mdb_get_lru_entries(p->mdb, pi, MAX_RECLAIM_COUNT);

				removed = 0;
				fetched = count;	
				pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
				handled	= count;
				skipped	= 0;
				volume 	= NULL;

				mdb_txno_null(&txno);
				need_reclaim = U_TRUE;

				for (i = 0;  i < count  && need_reclaim; i++) {
					need_reclaim = !pm_check_under_mark(p, PM_CHECK_LOW, U_TRUE);

					volume = nvm_lookup_volume(pi[i].vol_signature);
					res 		= -1;
					pl 			= NULL;


					/*
					 * count가 최종 삭제되는 레코드를 의미하는 경우
					 * transaction no를 저장할 수 있는 메모리 지정
					 */

					if (nvm_is_valid(volume)) {
						/*
						 * normal purge processing
						 */
						pl = nvm_path_lock_ref(volume, pi[i].path, __FILE__, __LINE__);
						nvm_path_lock(volume, pl, __FILE__, __LINE__);
						bw = 0;
						while (nvm_path_busy_nolock(pl)) {
							nvm_path_unlock(volume, pl, __FILE__, __LINE__);
							bt_msleep(100);
							bw++;
							nvm_path_lock(volume, pl, __FILE__, __LINE__);
						}
						if (bw != 0) {
							TRACE((T_INFO, "Volume[%s].Key[%s] - was busy for seconds\n", volume->signature, pi[i].key));
						}

						/*
						 * return values:
						 *	    0 : 캐싱 객체가 메모리에 로딩되어있지 않음
						 *		1 : found in VIT, purged (mdb also purged)
						 *	   -1 : 캐싱 객체가 로딩되어있지만 mdb_dirty 상태라서 mdb_update만 실행.  reclaim 패스
						 */
						rip =  pm_reclaim_if_possible(volume, pi[i].path, pi[i].key, pi[i].uuid, pi[i].ob_template != 0, pl, p);

						switch (rip) {
							case 0:
								/*
								 * 캐싱 객체가 VIT에 로딩되어 있지 않음
								 * - 캐싱 파일 삭제
								 * - mdb_purge
								 */
								if (!uuid_is_null(pi[i].uuid))  {
									res = pm_unlink_uuid(volume->signature, pi[i].key, U_FALSE, p, pi[i].uuid, NULL);
									if (res < 0) {
										TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned %d:%s\n",
														volume->signature,
														pi[i].key,
														uuid2string(pi[i].uuid, zuuid, sizeof(zuuid)),
														-res,
														strerror_r(-res, ebuf, sizeof(ebuf))
														));
									}
									else {
										TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned OK\n",
														volume->signature,
														pi[i].key,
														uuid2string(pi[i].uuid, zuuid, sizeof(zuuid))
														));
									}
								}
	
	
								if ((i == count -1) || !need_reclaim) {
									/*
									 * last entry
									 */
									mdb_purge_rowid(p->mdb, U_TRUE, U_FALSE, pi[i].uuid, pi[i].rowid, &txno);
								}
								else
									mdb_purge_rowid(p->mdb, U_TRUE, U_FALSE, pi[i].uuid, pi[i].rowid, NULL);
								removed++;
								break;
							case 1:
								/*
								 *  VIT에서 캐싱 객체가 발견되어서, 적절한 purge 실행됨 
								 */
								removed ++;
								break;
							case -1:
								/*
								 * r = -1 : 캐싱 객체가 발견되었지만, mdb_dirty상태라서 
								 * mdb_update만 실행. 이번 reclaim에서는 제외함
								 */
								skipped++;
								break;
						}

#ifndef NC_RELEASE_BUILD
						TRACE((T_INODE, "Volume[%s].Key[%s] - UUID[%s] [%d, r/s=(%d/%d)](%s)\n",
										volume->signature, 
										pi[i].key, 
										uuid2string(pi[i].uuid, zuuid,sizeof(zuuid)),
										rip, removed, skipped,
										(((i == count -1) || !need_reclaim)?"WAIT":"") 
										));
#endif
						nvm_path_unlock(volume, pl, __FILE__, __LINE__);
						nvm_path_lock_unref(volume, pl, __FILE__, __LINE__);

					}
					else {
						/* if (!uuid_is_null(pi[i].uuid))   */
						if (pi[i].ob_validuuid != 0) {
							res = pm_unlink_uuid(pi[i].vol_signature, pi[i].key, U_FALSE, p, pi[i].uuid, NULL);
							if (res < 0) {
								TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned %d:%s\n",
												pi[i].vol_signature,
												pi[i].key,
												uuid2string(pi[i].uuid, zuuid, sizeof(zuuid)),
												-res,
												strerror_r(-res, ebuf, sizeof(ebuf))
												));
							}
							else {
								TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned OK\n",
												pi[i].vol_signature,
												pi[i].key,
												uuid2string(pi[i].uuid, zuuid, sizeof(zuuid))
												));
							}
						}
						/*
						 * call and forget
						 */
						if (need_reclaim == 0 || (i ==  count -1))
							mdb_purge_rowid(p->mdb, U_TRUE, U_FALSE, pi[i].uuid, pi[i].rowid, &txno);
						else
							mdb_purge_rowid(p->mdb, U_TRUE, U_FALSE, pi[i].uuid, pi[i].rowid, NULL);

						removed ++;
#ifndef NC_RELEASE_BUILD
						TRACE((T_INODE, "Volume[%s].Key[%s] - **UUID[%s] [%d, r/s=(%d/%d)](%s)\n",
										pi[i].vol_signature, 
										pi[i].key, 
										uuid2string(pi[i].uuid, zuuid,sizeof(zuuid)),
										rip, removed, skipped,
										(((i == count -1) || !need_reclaim)?"WAIT":"") 
										));
#endif
					}

					/*
					 * return value 가 0인 경우 객체를 여기서 지워야함
					 */
				}
#ifdef	NC_ENABLE_MDB_WAIT 
				if (!mdb_txno_isnull(&txno)) {
					mdb_wait_done(p->mdb, &txno);
				}
#endif
				removed_total += removed;
				TRACE((0, "%d/%d purged(%d skipped), total=%d\n", removed, count, skipped, removed_total));
				for (i = 0; i < count; i++) {
					XFREE(pi[i].vol_signature);
					XFREE(pi[i].path);
					XFREE(pi[i].key);
				}


				pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
				first = 0;

				TRACE((T_INODE, "partition[%s] - %d of %d caching object(s) removed\n", p->path, removed, fetched));
			}
			if (removed_total > 0)
				TRACE((T_MONITOR, "partition[%s] - %d caching object(s) removed\n", p->path, removed_total));
			removed_total = 0;

		}
#if 0
		else {
			TRACE((T_INFO, "partition[%s] - is healthy and under the low watermark(used %.2f)\n", p->path, p->fs_usedper));
		}
		TRACE((T_INFO,  "Partition, '%s' - %d entries handled(not mean reclaimed)\n", p->path, handled));
		mdb_check_status(mdb_stat, sizeof(mdb_stat));
		TRACE((T_INFO,  "%s\n", mdb_stat));
#endif

		bt_msleep(10000); /* in every 10 secs */
	}
	TRACE((T_INFO, "Partition, '%s' - reclaim thread shundowned\n", p->path));
	p->stopped = 1;
	return NULL;
}
static void
pm_destroy_partition(nc_part_element_t *p)
{
	int 	e;	
	TRACE((T_INODE, "Partition, '%s' - destroying\n", p->path));
	DEBUG_ASSERT(p != NULL);
	DEBUG_ASSERT(p->path != NULL);
	p->needstop = 1;
	pthread_kill(p->reclaimer, SIGTERM);
	if ((e = pthread_join(p->reclaimer, NULL)) != 0) {
		TRACE((T_INODE, "pthread_join returned %d, waiting until stop confirmed\n", e));
		while (p->stopped == 0) bt_msleep(10);
	}
	TRACE((T_INODE, "Partition, '%s' - reclaim thread shutdown confirmed\n", p->path));

	mdb_close(p->mdb);
	XFREE(p->path);
	XFREE(p);
	return;
}


int
pm_destroy()
{
	nc_part_element_t 	*p;
#ifdef NC_MULTIPLE_PARTITIONS
	char 				primary[2048]="";
	int 				primary_valid = 0;
	char				sv[2048];
	char				dv[2048];
#endif


#ifdef NC_MULTIPLE_PARTITIONS
	if (mdb_get_primary(primary) >= 0) {
		TRACE((T_INFO, "Primary parition for 'volumes.mdb' is '%s'\n", primary));
		primary_valid++;
	}
#endif
	pthread_mutex_lock(&__part_table.lock);
	p = (nc_part_element_t *)link_get_head(&__part_table.list);
	pthread_mutex_unlock(&__part_table.lock);

	while (p) {

#ifdef NC_MULTIPLE_PARTITIONS
		if (primary_valid && (strcasecmp(p->path, primary) != 0)) {
			/* primary valid, and the target, p is slave partition */ 
			sprintf(sv, "%s/volumes.mdb", primary);
			sprintf(dv, "%s/volumes.mdb", p->path);
			if (_sync_file(sv, dv) < 0) {
				TRACE((T_ERROR, "Syncing volumes.mdb to '%s' failed\n", dv));
			}
		}
#endif
		pm_destroy_partition(p);

		pthread_mutex_lock(&__part_table.lock);
		p = (nc_part_element_t *)link_get_head(&__part_table.list);
		pthread_mutex_unlock(&__part_table.lock);
	}
	pthread_mutex_unlock(&__part_table.lock);
	while (bt_destroy_timer_v2(__timer_wheel_base, &__part_table.timer_load_monitor) < 0) {
		bt_msleep(1);
	}
	pm_stop();
	return 0;
}
/*
 * partition parallel lookup
 * 	- when destroy the query entry?
 */
static nc_mdb_inode_info_t *
pm_load_disk_inode_from_partition(nc_part_element_t *part, int preservecase, char *signature, char *qid)
{
	nc_mdb_inode_info_t		*minode = NULL;
#ifdef __PROFILE
	perf_val_t				ms, me;
	long long				ud;
#endif

	

	DO_PROFILE_USEC(ms, me, ud) {
		if (part->valid) {
			minode = mdb_load(part->mdb, preservecase, signature, qid);
			if (minode) {
				minode->part = part;
			}
		}
	}
#ifdef __PROFILE
	if (ud > 2000000) {
		TRACE((T_INFO, "Parition[%s]:Volume[%s]:CKey[%s] - load slow, %.2f msec\n",
						part->path, 
						signature, 
						qid, 
						(ud/1000.0)));
	}
#endif

	return minode;
}

#ifdef NC_MULTIPLE_PARTITIONS

static int
pm_do_query(void *d, void *tcb)
{
 	part_query_inode_quark_t 	*qi = (part_query_inode_quark_t *)d;
 	part_query_inode_t 			*q = (part_query_inode_t *)qi->query;
	nc_mdb_inode_info_t 		*minode = NULL;
	int 						query_destroyable = 0;
	int 						need_signal = 0;
	int 						skipped = 0;
	int  						jdx = 0;
	int  						myhandled = 0;

	
	DEBUG_ASSERT(qi->valid > 0);
	qi->valid = 0;
	if (qi->query->found) {
		TRACE((T_INODE, "Partition[%s]:Volume[%s]:CKey[%s] skipping[%d/%d]\n", q->part[qi->jindex]->path, q->signature, q->path, q->handled, q->count));
		skipped++;
		goto L_skip_run;
	}
	jdx = qi->jindex;

	/*
	 * query->path는 객체의 key
	 */

	/*
	 *
	 * 	MDB에 query실행 
	 *
	 */


	minode = pm_load_disk_inode_from_partition(qi->query->part[jdx], q->casepreserve, qi->query->signature, qi->query->path);


	DEBUG_ASSERT(qi->query->handled < qi->query->count);
	
	if (minode != NULL) {
		/*
		 * MDB에서 record발견
		 */

		need_signal++;

		/*
		 *
		 * 여러개의 파티션 중 단 하나의 파티션에서만 발견되어야함
		 *
		 */
		_ATOMIC_CAS(qi->query->found, 0, 1); 

		qi->query->minode = minode;
	}

L_skip_run: 

	myhandled = _ATOMIC_ADD(qi->query->handled, 1);

	TRACE((T_INODE, "Partition[%s]:Volume[%s]:CKey[%s] lookup done[%d/%d](%s)\n", 
					q->part[jdx]->path, 
					q->signature, 
					q->path, 
					q->handled, 
					q->count,
					(minode?"found":"not found")));

	pthread_mutex_lock(&qi->query->lock);
	query_destroyable = (qi->query->count - myhandled);

	if ((query_destroyable == 0) || (minode != NULL)) {
		/*
		 * condition 달성
		 */
		pthread_cond_broadcast(&qi->query->signal);
	}
	pthread_mutex_unlock(&qi->query->lock);
	/* we can free query here ??????? */

	if (query_destroyable == 0) {
		q->magic = 0;
		while (!q->pickedup) bt_msleep(1);
		XFREE(q->path);
		XFREE(q->signature);
		COND_DESTROY(&q->signal);
		MUTEX_DESTROY(&q->lock);
		XFREE(q->part);
		XFREE(q->slice);
		XFREE(q);
	}
	return 0;
}
#endif

int
pm_invalidate_node(int preservecase, char *signature, char *qid)
{
	int 				ret;
	nc_part_element_t *p = NULL;
	int 				ucount = 0;

	
	pthread_mutex_lock(&__part_table.lock);
	p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	while (p) {
		ret = mdb_invalidate(p->mdb, preservecase, signature, qid);
		ucount += ret;
		p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
	}
	pthread_mutex_unlock(&__part_table.lock);
	pm_check_unlock();
	
	return ucount;
}
/*
 * v24헤더에 뺴먹은 것들이 있어서, 정보의 완전 복구가 불가능함
 * 복구의 목적을 재사용이 아니라, 지우기 위한 것을 목표로 선택
 */
nc_mdb_inode_info_t *
pm_map_header_to_minode(nc_part_element_t *part, void *header, char * msignature, char *qpath, char *qid)
{
	nc_mdb_inode_info_t 	*minode;
	fc_common_header_t 		*pcommon = (fc_common_header_t *)header;
	fc_header_info_v30_t 	*h30 = (fc_header_info_v30_t *)header;
	int 					hv;


	hv = dm_check_magic(pcommon->magic);
	if (hv == INODE_HDR_VERSION_V30) {
			minode = XMALLOC(sizeof(nc_mdb_inode_info_t), AI_VOLUME);
			minode->magic 		= 77;
			memcpy(minode->uuid, h30->uuid, sizeof(h30->uuid));
			minode->size 		= h30->size;
			minode->mode 		= h30->mode;
			minode->ctime 		= h30->ctime;
			minode->mtime 		= h30->mtime;
			minode->vtime 		= h30->vtime;
			minode->xtime 		= h30->vtime;
			minode->cversion 	= h30->cversion; /* ir # 24803 : cversion 추가 */
			minode->obi.op_bit_s 	= h30->obi.op_bit_s;
			if (h30->cversion == 0) {
				/*
				 * 이전 버전 캐싱 파일임.
				 */
				minode->cversion = 1;
			}

			minode->vol_signature 	= XSTRDUP(msignature, AI_VOLUME);;
			minode->q_path 			= XSTRDUP(qpath, AI_VOLUME);
			minode->q_id 			= XSTRDUP(qid, AI_VOLUME);
			minode->part 			= part;
			minode->property 		= NULL;
			minode->hid 			= dm_make_hash_key(minode->casesensitive, minode->q_id);
	}
	return minode;
}
int
pm_unlink_uuid(char *vol_signature, char *key, int ispriv, nc_part_element_t *part, uuid_t uuid, char *cpath_ifany)
{
	char 			*cpath 	= cpath_ifany;
	int 			r 		= 0;
	int				tflg = 0;

	errno = 0;

	if (cpath_ifany == NULL)
		cpath = pm_create_cache_path(part, ispriv, uuid, AI_ETC);

	r = unlink(cpath);
	if (r != 0) {
		r = -errno;
		tflg = T_INFO;
	}


	if (cpath_ifany == NULL)
		XFREE(cpath);
	return r;
}

/*
 * 유념할 부분
 * mdb쪽에서 이미 path-lock은 확보되어 있음
 * 그러므로 ic-lock을 호출해도 lock-order에 지장없음
 *
 * DESCRIPTION:
 *		mdb에서 선택된 db record각각에 대해서 호출되며, 
 * 		아래 함수의 실행 결과값에 따라 MDB쪽에서 자동으로 record삭제
 * 		이 함수가 호출되기 전에 필요한 path-lock은 이미 확보한 상태임
 *
 */
static int
pm_purge_cache_callback(nc_volume_context_t *volume, char *path, char *key, uuid_t uuid, nc_int64_t rowid, int ishard, int istempl, nc_path_lock_t *pl, nc_part_element_t *part, void *ctx)
{
	fc_inode_t		*inode 	= NULL;
	char			zuuid[128];
	char			ebuf[128];
	int				r;


	if (ishard) {

		IC_LOCK(CLFU_LM_EXCLUSIVE); 
		/*
		 * VIT 및 CLFU에서 제외
		 */
		inode = nvm_lookup_inode(	volume, 
									DM_LOOKUP_WITH_KEY, 
									key, 
									U_TRUE,  /* cache에서 제외*/
									U_FALSE,  /* refcnt 그대로 */
									U_FALSE,	/* DON't mark busy */
									NULL,
									__FILE__, 
									__LINE__);

		if (inode) {
			DEBUG_ASSERT(ic_is_busy(inode) == 0);
			/*
			 * inode는 VIT및 CLFU에서 제거됨
			 */
			_ATOMIC_CAS(inode->cstat, IS_CACHED, IS_ORPHAN);

			/*
			 ********************************************************************************************
			 * IMPORTANT!!!!) mdb기준 삭제가 이루어지는 호출 흐름에선
			 * 	   이 위치에서 실제 mdb삭제 안함
			 *
			 ********************************************************************************************
			 */
			inode->rowid = -1LL; /*  이 콜백이 불리기 전에 이미 MDB에서 삭제됨 */
		}

		IC_UNLOCK;


		if (inode) {
			r = nvm_purge_inode(volume, inode, U_FALSE,  NULL);
			if (r == -EBUSY) {
				TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%ld]/R[%d]/UUID[%s] returned EBUSY, setting doc=1\n",
								volume->signature,
								key,
								inode->uid,
								inode->refcnt,
								uuid2string(uuid, zuuid, sizeof(zuuid))
								));
				inode->ob_doc 	= 1;
			}
		}
		else if (!uuid_is_null(uuid))  {
			r = pm_unlink_uuid(volume->signature, key, U_FALSE, part, uuid, NULL);
			if (r < 0) {
				TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned %d:%s(not loaded)\n",
								volume->signature,
								key,
								uuid2string(uuid, zuuid, sizeof(zuuid)),
								-r,
								strerror_r(-r, ebuf, sizeof(ebuf))
								));
			}
			else {
				TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned OK(not loaded)\n",
								volume->signature,
								key,
								uuid2string(uuid, zuuid, sizeof(zuuid))
								));
			}
		}
	}
	else {
		IC_LOCK(CLFU_LM_SHARED); 
		inode = nvm_lookup_inode(	volume, 
									DM_LOOKUP_WITH_KEY, 
									key, 
									ishard, 
									U_FALSE, 
									U_FALSE,	/* don't mark busy*/
									NULL,
									__FILE__, 
									__LINE__);
		if (inode)  {
			TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%d]/R[%d] - softly purged\n",
							volume->signature,
							key,
							inode->uid,
							INODE_GET_REF(inode)
							));
			inode->vtime = 0; /* MDB기준으로 한 퍼지 이므로 DM_UPDATE_METAINFO이용안함*/
		}
		IC_UNLOCK; 
	}

L_nothing_todo:
	return 0;

}
/*
 * REMARK!: called only within the reclaiming thread
 */
static int
pm_reclaim_if_possible(nc_volume_context_t *volume, char *path, char *key, uuid_t uuid, int istempl, nc_path_lock_t *pl, nc_part_element_t *part)
{
	fc_inode_t		*inode 	= NULL;
	char			zuuid[128];
	int				r;



	IC_LOCK(CLFU_LM_EXCLUSIVE); 
	/*
	 * VIT 및 CLFU에서 제외
	 */
	inode = nvm_lookup_inode(	volume, 
								DM_LOOKUP_WITH_KEY, 
								key, 
								U_FALSE,  /* cache에서 제외*/
								U_FALSE,  /* refcnt 그대로 */
								U_FALSE, 
								NULL,
								__FILE__, 
								__LINE__);


	if (inode && dm_is_mdb_dirty(inode)) {
		/*
		 * 대상 캐싱 객체는 MDB에 기록이 늦었을 뿐이고
		 * 실제 최종 atime은 최근일 수 있음
		 * 일단 MDB 업데이트해두어서 reclaim되지 않도록 해둠
		 */
		mdb_update_rowid(inode->part->mdb,
						inode->uuid,
						inode->rowid,
						inode->size,
						inode->rsize,
						inode->vtime,
						inode->imode,
						inode->ob_obit,
						NULL);
		inode->mdbversion = 0; 
		IC_UNLOCK;
		return -1; /* 이번 reclaim 대상에서 제외*/
	}
	if (inode && inode->ob_doc == 1) {
		IC_UNLOCK;
		return -1;
	}
	if (inode && ic_is_busy(inode)) {
		/*
		 * 현재 추가 operation 중임, reclaim skip
		 */
		IC_UNLOCK;
		return -1;
	}
	if (inode) {
		ic_set_busy(inode, U_TRUE, __FILE__, __LINE__);
		/*  reset in nvm_purge_inode() */
	}

	IC_UNLOCK;


	if (inode) {
		r = nvm_purge_inode(volume, inode, U_TRUE, NULL);
		if (r == -EBUSY) {
			TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%ld]/R[%d]/UUID[%s] returned EBUSY, setting doc=1\n",
							volume->signature,
							key,
							inode->uid,
							inode->refcnt,
							uuid2string(uuid, zuuid, sizeof(zuuid))
							));
			inode->ob_doc 	= 1;
		}
		return 1; /* mdb delete까지 실행*/
	}
#if 0
	else if (!uuid_is_null(uuid))  {
		r = pm_unlink_uuid(volume->signature, key, U_FALSE, part, uuid, NULL);
		if (r < 0) {
			TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned %d:%s\n",
							volume->signature,
							key,
							uuid2string(uuid, zuuid, sizeof(zuuid)),
							-r,
							strerror_r(-r, ebuf, sizeof(ebuf))
							));
		}
		else {
			TRACE((T_INODE, "Volume[%s].Key[%s] - pm_unlink_uuid(UUID[%s]) returned OK\n",
							volume->signature,
							key,
							uuid2string(uuid, zuuid, sizeof(zuuid))
							));
		}
	}
#endif

	return 0;

}
int
pm_purge(nc_volume_context_t *volume, char *pattern, int iskey, int ishard)
{
#define		MAX_PURGE_PER_ATTEMPT	1000
	int 				purged = 0;
	int					purged_sum  = 0;
	nc_part_element_t	*p = NULL;
	pthread_mutex_lock(&__part_table.lock); 
	p = (nc_part_element_t *)link_get_head_noremove(&__part_table.list);
	pthread_mutex_unlock(&__part_table.lock); 
	while (p) {
		do {
			purged = mdb_purge(volume, p->mdb, pattern, ishard, iskey, pm_purge_cache_callback, p, MAX_PURGE_PER_ATTEMPT);
			purged_sum += purged;
		} while (purged == MAX_PURGE_PER_ATTEMPT);
		pthread_mutex_lock(&__part_table.lock);
		p = (nc_part_element_t *)link_get_next(&__part_table.list, &p->node);
		pthread_mutex_unlock(&__part_table.lock);
	}
	return purged_sum;
}
