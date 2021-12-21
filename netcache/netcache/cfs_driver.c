#include <config.h>
#include "netcache_types.h"
#include "hash.h"
#include "util.h"
#include <errno.h>
#include <dlfcn.h>
//#include <duma.h>
#include "util.h"
#include "trace.h"
#include "ncapi.h"
#include <netcache.h>
#include "rdtsc.h"
#include "cfs_driver.h"




#define 	MAX_CFS_DRIVER_COUNT 	10
extern int g__default_gid;
extern int g__default_uid;
cfs_origin_driver_t *__driver_class[MAX_CFS_DRIVER_COUNT];
int 				 __driver_class_count = 0;
#define CFS_DRIVER_CLASS_COUNT  howmany(sizeof(__driver_class), sizeof(cfs_origin_driver_t *))
#define 	CFS_CHECK_DRIVER_OPERATION(_d_, _op, _io_) { \
													if (!(_d_)) { \
														TRACE((T_ERROR, "invalid driver instance given\n")); \
														return -EINVAL; \
													} \
													DEBUG_ASSERT(nc_check_memory((_d_), 0)); \
													if (!(_d_)->_op) { \
														TRACE((0, "driver does not support the operation, '%s'\n",  #_op, "\n")); \
														return -ENOSYS; \
													} \
													if (!cfs_is_online(_d_, _io_)) { \
														TRACE((T_ERROR, "driver is offline \n" )); \
														return -EREMOTEIO; \
													} \
												}
#define 	CFS_CHECK_DRIVER_OPERATION_RTPTR(_d_, _op, _io_, _onlinechk, _type_) { \
													if (!(_d_)) { \
														TRACE((T_ERROR, "invalid driver instance given\n")); \
														errno = EINVAL; \
														return (_type_)NULL; \
													} \
													DEBUG_ASSERT(nc_check_memory((_d_), 0)); \
													if (!(_d_)->_op) { \
														TRACE((0, "driver does not support the operation, '%s'\n", #_op, "\n")); \
														errno = ENOSYS; \
														return (_type_)NULL; \
													} \
													if (_onlinechk && !cfs_is_online(_d_, _io_)) { \
														TRACE((T_ERROR, "driver is offline \n" )); \
														errno = EREMOTEIO; \
														return (_type_)NULL; \
													} \
												}
#define 	CFS_CHECK_DRIVER_OPERATION_RD(_d, _op) { \
													if (!(_d)) { \
														TRACE((T_ERROR, "invalid driver instance given\n")); \
														return -EINVAL; \
													} \
													DEBUG_ASSERT(nc_check_memory((_d), 0)); \
													if (!(_d)->_op) { \
														TRACE((0, "driver does not support the operation, '%s'\n", #_op, "\n")); \
														return -ENOSYS; \
													} \
												}
#define 	CFS_CHECK_DRIVER_OPERATION_VOID(_d, _op) { \
													if (!(_d)) { \
														TRACE((T_ERROR, "invalid driver instance given\n")); \
														return ; \
													} \
													DEBUG_ASSERT(nc_check_memory((_d), 0)); \
													if (!(_d)->_op) { \
														TRACE((0, "driver does not support the operation, '%s'\n", #_op, "\n")); \
														return ; \
													} \
													if (!cfs_is_online(_d, NCOF_READABLE|NCOF_WRITABLE)) { \
														TRACE((T_ERROR, "driver is offline \n" )); \
														errno = EREMOTEIO; \
														return NULL; \
													} \
												}
#ifdef USE_VM_STAT
#define	CFS_INCREASE_VM_COUNTER(_x, _v) _ATOMIC_ADD((_x), (_v))
#else
#define	CFS_INCREASE_VM_COUNTER(_x, _v)
#endif

								
static pthread_mutex_t  s__cfs_lock 				= PTHREAD_MUTEX_INITIALIZER;
u_hash_table_t *		s__device_table 			= NULL;
#if 0
u_hash_table_t *		s__driver_instance_table 	= NULL;
#endif
int cfs_lock_driver(cfs_origin_driver_t *drv, cfs_lock_t shared);
int cfs_unlock_driver(cfs_origin_driver_t *drv, cfs_lock_t shared);

typedef struct {
	char					name[NC_MAX_STRINGLEN];
	cfs_origin_driver_t 	*LSD;
	void 					*handle;
} LSD_info_t;

static void 
lsd_free(void *d)
{
	LSD_info_t *lsd = (LSD_info_t *)d;

	if (lsd && lsd->handle)  {
		if (lsd->LSD->unload) {
			lsd->LSD->unload();
		}

		dlclose(lsd->handle);
	}
}
void s_free(void *f)
{
	XFREE(f);
	return;
}

cfs_origin_driver_t *
cfs_load_driver(char *name, char *path)
{
	cfs_load_class_proc_t 	p_class_load = NULL;
	void 					*d_handle = NULL;
	char 					*derror = NULL;
	cfs_origin_driver_t 	*DC;
	LSD_info_t 				*lsd = NULL;

	


	if (!s__device_table) {
		s__device_table = (u_hash_table_t *)u_ht_table_new(path_hash, path_equal, NULL, lsd_free); 
	}
	lsd = (LSD_info_t *)u_ht_lookup(s__device_table, (char *)name);
	if (lsd) {
		TRACE((T_WARN, "library name, '%s', path '%s' already loaded\n", name, path));
		/* already loaded */
		
		return NULL;
	}

	d_handle = dlopen(path, RTLD_LAZY);
	//d_handle = dlopen(path, RTLD_NOW|RTLD_DEEPBIND);
	if (d_handle == NULL) {
		TRACE((T_ERROR, "failed in calling dlopen(%s): std(%s), dlerror(%s)\n", path, strerror(errno), dlerror()));
		
		return NULL;
	}

	p_class_load = (cfs_load_class_proc_t)dlsym(d_handle, "load");
	if ((derror = dlerror()) != NULL) {
		TRACE((T_ERROR, "dlsym(%s, 'load') failed:%s\n", path, derror));
		
		return NULL;
	}

	if (!p_class_load) {
		TRACE((T_ERROR, "plugin, '%s' does not have the symbole, 'load'\n", path));
		
		return NULL;
	}

	DC = __driver_class[__driver_class_count] = (*p_class_load)();
	__driver_class_count++;
	TRACE((T_INFO, "NetCache OSD drvier, '%s' successfully registerd\n", __driver_class[__driver_class_count-1]->name));
	TRACE((T_PLUGIN, "-- driver class information :\n"
					"name : '%s'\n"
					"devclass : %p\n"
					"open : %p\n"
					"read : %p\n"
					"write : %p\n"
					"mknod : %p\n"
					"mkdir : %p\n"
					"utimens : %p\n"
					"write_stream : %p\n"
					"close : %p\n"
					"getattr : %p\n"
					"readdir : %p\n"
					"ioctl : %p\n"
					"bind_context : %p\n"
					"unbind_context : %p\n"
					"create_instance : %p\n"
					"destroy_instance : %p\n"
					"lasterror : %p\n"
					"set_lasterror : %p\n"
					"unlink : %p\n"
					"link : %p\n"
					"rmdir : %p\n"
					"rename : %p\n"
					"flush : %p\n"
    				"online : %p\n"
					"mc : %p\n"
					"ctx : %p\n"
					"hint_proc: %p\n" 
					"prefix : '%s'\n"
					"preservecase : %d\n"
					"prefix_len : %d \n"
					"ctx_cnt : %d\n"
					"driver_data : %p\n",

					DC->name,
					DC->devclass,
					DC->open,
					DC->read,
					DC->write,
					DC->mknod,
					DC->mkdir,
					DC->utimens,
					DC->write_stream,
					DC->close,
					DC->getattr,
					DC->readdir,
					DC->lioctl,
					DC->bind_context,
					DC->unbind_context,
					DC->create_instance,
					DC->destroy_instance,
					DC->lasterror,
					DC->set_lasterror,
					DC->unlink,
					DC->link,
					DC->rmdir,
					DC->rename,
					DC->flush,
    				DC->online,
					DC->mc,
					DC->ctx,
					DC->hint_proc, 
					DC->prefix,
					DC->preservecase,
					DC->prefix_len,
					DC->ctx_cnt,
					DC->driver_data));

	lsd = XMALLOC(sizeof(LSD_info_t), AI_DRIVER);
	lsd->handle = d_handle;
	lsd->LSD = DC;
	strncpy(lsd->name, name, sizeof(lsd->name)-1);
	if (!u_ht_insert(s__device_table, lsd->name, (void *)lsd, U_FALSE)) {
		/* insert fail */
		
		return NULL;
	}
	
	return DC;
}
void
cfs_unload_all()
{
#if 1
	if (s__device_table) {
		u_ht_table_free(s__device_table);
		s__device_table = NULL;
	}
#endif

#if 0
	if (s__driver_instance_table) {
		u_ht_table_free(s__driver_instance_table);
		s__driver_instance_table = NULL;
	}
#endif
	
	return ;
}


cfs_origin_driver_t *
cfs_find_driver_name(char *name)
{
	//int 		i;
	LSD_info_t 	*lsd = NULL;

	lsd = (LSD_info_t *)u_ht_lookup(s__device_table, (void *)name);

	if (!lsd) return NULL;

	return lsd->LSD;
}

int
cfs_errno(cfs_origin_driver_t *drv)
{
	return drv->lasterror();
}
int
cfs_is_online(struct cfs_origin_driver *drv, int ioflag)
{
	return drv->online;
}

struct tag_driver_fi {
	char						*qr_path;
	cfs_origin_driver_t			*driver;
};
#if NOT_USED
static  int
cfs_find_driver_entry(void *key, void *value, void *ud)
{
	struct tag_driver_fi	*fi = (struct tag_driver_fi *)ud;
	cfs_origin_driver_t 	*osd = (cfs_origin_driver_t *)value;
	int						n;

	n = strlen(osd->signature);
	if (strncmp(fi->qr_path, osd->signature, n) == 0) {
		fi->driver = osd;
		return HT_STOP;
	}
	return HT_CONTINUE;
}
#endif
int 
cfs_is_online_x(char *qr_path, int ioflag)
{
#if 0
	struct tag_driver_fi	fi;

	fi.qr_path 	= qr_path;
	fi.driver 	= NULL;

	u_ht_foreach(s__driver_instance_table, cfs_find_driver_entry, &fi);

	if (fi.driver)
		return cfs_is_online(fi.driver, ioflag);
#endif
	return 0;
}

cfs_origin_driver_t *
cfs_create_driver_instance(char *name, char *signature)
{
	cfs_origin_driver_t 	*drvclass = NULL, *driver = NULL;
int sc_update_hint(nc_volume_context_t *mc, char *path, void *hint, int hint_len);
int sc_update_result(nc_volume_context_t *mc, char *path, int errcode);

	

	pthread_mutex_lock(&s__cfs_lock);
	drvclass = cfs_find_driver_name(name);
	if (drvclass == NULL) {
		TRACE((T_ERROR, "driver class '%s' - not found\n", name));
		
		pthread_mutex_unlock(&s__cfs_lock);
		return NULL;
	}
	if (!drvclass->create_instance) {
		TRACE((T_ERROR, "driver class '%s' - create_instance method not supplied\n", name));
		
		pthread_mutex_unlock(&s__cfs_lock);
		return NULL;
	}
	driver = drvclass->create_instance(signature, NULL, NULL);
	if (driver) {
		driver->devclass	= drvclass;
		driver->magic		= NC_MAGIC_KEY;
	}
	if (driver) {
		TRACE((T_PLUGIN, "instance, name('%s'), signature('%s') successfully created\n",
			driver->name, signature));

		/*
		 * ir #25884 : rwlock 초기화
		 */
		driver->rwlock = (pthread_rwlock_t *)XMALLOC(sizeof(pthread_rwlock_t), AI_VOLUME);
		pthread_rwlock_init(driver->rwlock, NULL);
	}
#if 0
	ASSERT(u_ht_insert(s__driver_instance_table, driver->signature, (void *)driver, U_FALSE));
#endif	
	pthread_mutex_unlock(&s__cfs_lock);
	return driver;
	
}
int
cfs_destroy_driver_instance(cfs_origin_driver_t *drv)
{
	char					signature[256]="";
	cfs_origin_driver_t 	*drvclass = NULL;
	int (*dip)(struct cfs_origin_driver *driver) = NULL;
	pthread_rwlock_t		*rwlock = NULL;

	
	pthread_mutex_lock(&s__cfs_lock);

	rwlock = drv->rwlock; /* persist after destroy_instance */
L_retry_first:
	cfs_lock_driver(drv, cfs_lock_exclusive);	
	if (!drv->destroy_instance) {
		TRACE((T_ERROR, "driver %p - destroy_instance method not supplied\n", drv));
		
		cfs_unlock_driver(drv, cfs_lock_exclusive);	
		pthread_mutex_unlock(&s__cfs_lock);
		return -1;
	}

	drv->shutdown = 1;


	while (CFS_DRIVER_INUSE(drv)) {
		cfs_unlock_driver(drv, cfs_lock_exclusive);	
		bt_msleep(500);
		cfs_lock_driver(drv, cfs_lock_exclusive);	
	}

	drvclass = drv->devclass;
	strcpy(signature, drv->signature);
	/*
	 * ir #25884 : rwlock 초기화
	 */
	dip = drvclass->destroy_instance;

	if (dip(drv) == -EBUSY) {
		cfs_unlock_driver(drv, cfs_lock_exclusive);	
		bt_msleep(1000);
		goto L_retry_first;
	}
	/*
	 * dip(drv)가 실행되면 drv 메모리 구조체는 free됨
	 * 그러므로 cfs_unlock_driver() 호출은  불가능
	 */
	//cfs_unlock_driver(drv, cfs_lock_exclusive);	
	pthread_rwlock_unlock(rwlock);
	pthread_rwlock_destroy(rwlock);
	XFREE(rwlock);

#if 0
	ASSERT(u_ht_remove(s__driver_instance_table, signature));
	
#endif
	pthread_mutex_unlock(&s__cfs_lock);
	return 0;
}

#if NOT_USED
typedef struct {
	nc_volume_context_t 	*ap_vol;
	nc_fill_dir_proc_t 	ap_cb;
	void * 				ap_cb_data;
} cfs_readdir_cb_t;

static int 
cfs_readdir_proc(	
								char *entry_name,
								char *path,
								nc_stat_t *nstat,
								void *hint,
								int hint_len,
								nc_off_t off,
								void *cb)
{
#if 0
#undef st_mtime
	int 	r;
	
	cfs_readdir_cb_t * cfs_cb = (cfs_readdir_cb_t *)cb;

	nstat->st_gid = g__default_gid;
	nstat->st_uid = g__default_uid;
	TRACE((T_PLUGIN, "name[%s] : mode[0x%08X], size[%lld], mtime[%lld]\n",
					(char *)entry_name,
					(int)nstat->st_mode,
					(long long)nstat->st_size,
					(long long)nstat->st_mtime));
	r = (cfs_cb->ap_cb)(cfs_cb->ap_vol, cfs_cb->ap_cb_data, entry_name, nstat, 0);
	sc_update_existing(0, (char *)path, (char *)path, cfs_cb->ap_vol, nstat, hint, hint_len, U_FALSE);


	
	return r;
#else
	return 0;
#endif
}
int 
cfs_readdir(	nc_volume_context_t 	*mc,
				cfs_origin_driver_t *drv, 
				char *path, 
				void *cb,  
				nc_fill_dir_proc_t dir_proc,
				cfs_off_t offset,
				void *ud
)
{
#if 0
	int 				r;
	cfs_readdir_cb_t 	*ap_cb = NULL;

	

	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, readdir);


	CFS_INCREASE_VM_COUNTER(mc->cnt_readdir, 1);

	ap_cb = XMALLOC(sizeof(cfs_readdir_cb_t), AI_DRIVER);
	ap_cb->ap_cb 		= dir_proc;
	ap_cb->ap_cb_data 	= cb;
	ap_cb->ap_vol 		= mc;

	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->readdir(mc->osd, path, ap_cb, cfs_readdir_proc, 0, mc);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);

	//TRACE((T_ERROR, "READDIR(%s) - %d\n", path, r));

	XFREE(ap_cb);
	
	return r;
#else
	return 0;
#endif
}
#endif

int
cfs_getattr(nc_volume_context_t *mc, char *path , nc_stat_t *old_s, nc_stat_t *new_s, nc_xtra_options_t *kv, nc_mode_t mode, struct apc_open_context *oc)
{
	int r;
#ifdef __PROFILE
	perf_val_t 				ms, me;
	long long 				md;
#endif
	
	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, getattr);

	CFS_INCREASE_VM_COUNTER(mc->cnt_getattr, 1);

	CFS_DRIVER_REF(mc->osd);

	DO_PROFILE_USEC(ms, me, md) {
		cfs_lock_driver(mc->osd, cfs_lock_shared);
		r = mc->osd->getattr(mc->osd, path, old_s, new_s, kv, mode, oc);
		cfs_unlock_driver(mc->osd, cfs_lock_shared);
	}

#ifdef __PROFILE
	md = md/1000LL;
	if (md > 50000) {
		TRACE((T_WARN, "MOUNT(%s)/path(%s) - too slow (%.2f msec)in %s\n", mc->signature, path, (float)(md/1000.0), __func__));
	}
#endif
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
/*
 *  
 *  
 */

int
cfs_read_vector(nc_volume_context_t *mc, nc_asio_vector_t *vector, nc_origin_session_t ctx)
{
	int 					rblks = 0;

	

	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, read);
	CFS_INCREASE_VM_COUNTER(mc->cnt_read, 1);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	rblks = mc->osd->read(mc->osd, vector, ctx);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);

	
	return rblks;
}
int
cfs_write_vector(nc_volume_context_t *mc, nc_asio_vector_t *vector)
{
	int 					wblks = 0;

	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, write, NCOF_WRITABLE);
	CFS_INCREASE_VM_COUNTER(mc->cnt_write, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	wblks = mc->osd->write(vector);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);


	
	return wblks;
}
int
cfs_write_stream(void *file_handle, char *buf, nc_ssize_t len)
{
	//int 					s = 0;

#if 0
	

	CFS_CHECK_DRIVER_OPERATION(mc->osd, write_stream, NCOF_WRITABLE);
	CFS_INCREASE_VM_COUNTER(mc->cnt_write, 1);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	s = mc->osd->write_stream(file_handle, buf, len);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);

	
	return wblks;
#else
	return -ENOSYS;
#endif
}
int
cfs_flush(nc_volume_context_t *mc, struct tag_fc_inode_info *inode)
{
	int 					r = 0;

	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, flush, NCOF_WRITABLE);
	CFS_INCREASE_VM_COUNTER(mc->cnt_write, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->flush(inode);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);


	
	return r;
}
int
cfs_utimens(nc_volume_context_t *mc, char *path, struct nc_timespec tv[2])
{
	int r;
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, utimens, NCOF_WRITABLE);
	CFS_INCREASE_VM_COUNTER(mc->cnt_utimens, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->utimens(mc->osd, path, tv);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_mkdir(nc_volume_context_t *mc, char *path, mode_t mode)
{
	int r;
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, mkdir, NCOF_WRITABLE);

	TRACE((T_PLUGIN, "calling driver->mkdir\n"));
	CFS_INCREASE_VM_COUNTER(mc->cnt_mkdir, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->mkdir(mc->osd, path, mode);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_mknod(nc_volume_context_t *mc, char *path, mode_t mode, void *hint, int hint_len)
{
	int  r;
	
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, mknod, NCOF_WRITABLE);

	CFS_INCREASE_VM_COUNTER(mc->cnt_mknod, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->mknod(mc->osd, path, mode, hint, hint_len);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_unlink(nc_volume_context_t *mc, char *path, nc_xtra_options_t *req, nc_xtra_options_t **res)
{
	int 	r;
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, unlink, NCOF_WRITABLE);

	CFS_INCREASE_VM_COUNTER(mc->cnt_unlink, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->unlink(mc->osd, path, req, res);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_rmdir(nc_volume_context_t *mc, char *path)
{
	int r;
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, rmdir, NCOF_WRITABLE);

	CFS_INCREASE_VM_COUNTER(mc->cnt_rmdir, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->rmdir(mc->osd, path);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_truncate(nc_volume_context_t *mc, char *path, nc_size_t len)
{
	int r;
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, truncate, NCOF_WRITABLE);

	CFS_INCREASE_VM_COUNTER(mc->cnt_truncate, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->truncate(mc->osd, path, len);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_ftruncate(nc_volume_context_t *mc, struct dev_file_handle *dh, nc_size_t len)
{
	int r;
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, ftruncate, NCOF_WRITABLE);

	CFS_INCREASE_VM_COUNTER(mc->cnt_truncate, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->ftruncate(mc->osd, dh, len);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_rename(nc_volume_context_t *mc, dev_file_handle_t *dh, char *src, char *dest)
{
	int 	r;
	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, rename, NCOF_WRITABLE);

	CFS_INCREASE_VM_COUNTER(mc->cnt_rename, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->rename(mc->osd, dh, src, dest);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
struct dev_file_handle * 
cfs_open(nc_volume_context_t *mc, char *path, void *hint, int hint_len, int mode, nc_xtra_options_t *opt)
{
	struct dev_file_handle *r;

	
	CFS_CHECK_DRIVER_OPERATION_RTPTR(mc->osd, open, NCOF_READABLE, U_FALSE, struct dev_file_handle *);

	CFS_INCREASE_VM_COUNTER(mc->cnt_open, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->open(mc->osd, path, hint, hint_len, mode, opt);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}
int 
cfs_close(nc_volume_context_t *mc, dev_file_handle_t *handle)
{
	int 	r;

	

	//CFS_CHECK_DRIVER_OPERATION(mc->osd, close, NCOF_READABLE);

	if (!(mc->osd)) { 
		TRACE((T_ERROR, "invalid driver instance given\n")); \
		
		return -EINVAL; \
	} 
	if (!(mc->osd)->close) { \
		TRACE((T_ERROR, "driver does not support the operation, close\n" )); \
		
		return -ENOSYS; \
	}

	CFS_INCREASE_VM_COUNTER(mc->cnt_close, 1);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->close(handle);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}

int	
cfs_link(struct cfs_origin_driver *drv, char *path1, char *path2)
{
	return -ENOSYS;
}
int 
cfs_statfs(nc_volume_context_t *mc, char *path, nc_statvfs_t *fs)
{
	int 	r;

	
	CFS_CHECK_DRIVER_OPERATION(mc->osd, statfs, NCOF_READABLE);

	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->statfs(mc->osd, path, fs);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);
	
	return r;
}

/*
 *  idle check하고 진입하는 대신
 *  진입해서 BUSY 여부를 판단하고 계속 할지 -EBUSY에 따라 대기할지 결정한다
 */
int	
cfs_ioctl(nc_volume_context_t *mc, int cmd, void *val, int vallen)
{
	int 			r = 0;

	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, lioctl);

	//
	// NEW) 2020-01-25 by weon
	// driver내에서IOCB를 기반으로 operation이 진행중인 경우
	// IOCTL은 모든 operation이 종료될 때까지 대기.
	//
	//

	cfs_lock_driver(mc->osd, cfs_lock_exclusive);
	r = mc->osd->lioctl(mc->osd, cmd, val, vallen);
	while (r == -EBUSY) {
		/* driver busy, try later */
		cfs_unlock_driver(mc->osd, cfs_lock_exclusive);
		bt_msleep(500);
		cfs_lock_driver(mc->osd, cfs_lock_exclusive);
		r = mc->osd->lioctl(mc->osd, cmd, val, vallen);
	} 
	cfs_unlock_driver(mc->osd, cfs_lock_exclusive);


	return r;
}
int 
cfs_bind_context(nc_volume_context_t *mc, char *prefix, nc_origin_info_t *ctx, int ctxcnt)
{
	return cfs_bind_context_x(mc, prefix, ctx, ctxcnt, NC_CT_ORIGIN);
}
int 
cfs_unbind_context(nc_volume_context_t *mc)
{
	return cfs_unbind_context_x(mc, NC_CT_ORIGIN);
}
int 
cfs_bind_context_x(nc_volume_context_t *mc, char *prefix, nc_origin_info_t *ctx, int ctxcnt, int ctxtype)
{
	int 	r = 0;

	cfs_lock_driver(mc->osd, cfs_lock_exclusive);
	r = mc->osd->bind_context(mc->osd, prefix, ctx, ctxcnt, ctxtype);
	cfs_unlock_driver(mc->osd, cfs_lock_exclusive);

	return r;
}
int 
cfs_unbind_context_x(nc_volume_context_t *mc, int ctxtype)
{
	int 	r = 0;
	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, unbind_context);

	cfs_lock_driver(mc->osd, cfs_lock_exclusive);
	r = mc->osd->unbind_context(mc->osd, ctxtype);
	cfs_unlock_driver(mc->osd, cfs_lock_exclusive);

	return r;
}
int 
cfs_set_lasterror(nc_volume_context_t *mc, int v)
{
	int 	r = 0;
	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, set_lasterror);

	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->set_lasterror(v);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);

	return r;
}

int 
cfs_iserror(nc_volume_context_t *mc, int v)
{
	int 	r = 0;
	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, iserror);

	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->iserror(v);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);

	return r;
}
int 
cfs_issameresponse(nc_volume_context_t *mc, int c1, int c2)
{
	int 	r = 0;
	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, issamecode);

	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	r = mc->osd->issamecode(c1,c2);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);

	return r;
}

void cfs_set_trace(nc_volume_context_t *mc, int onoff)
{
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	mc->osd->trace = onoff;
	cfs_unlock_driver(mc->osd, cfs_lock_shared);

	return ;
}


struct dev_file_handle *	
cfs_create(nc_volume_context_t *mc, char *path, int mode, void *hint, int hint_len, nc_xtra_options_t *opt)
{
	dev_file_handle_t *r;
	
	CFS_CHECK_DRIVER_OPERATION_RTPTR(mc->osd, create, NCOF_WRITABLE, U_FALSE, struct dev_file_handle *);

	CFS_INCREASE_VM_COUNTER(mc->cnt_create, 1);
	r = mc->osd->create(mc->osd, path, mode, hint, hint_len, opt);
	
	return r;
}

int
cfs_set_notifier(cfs_origin_driver_t *drv, cfs_notifier_callback_t cb, nc_volume_context_t *mnt)
{
	int 	r;
	
	CFS_CHECK_DRIVER_OPERATION(drv, set_notifier, NCOF_READABLE);

	r = drv->set_notifier(drv, cb, mnt);
	return r;
}



int
cfs_lock_driver(cfs_origin_driver_t *drv, cfs_lock_t shared)
{
	int r = 0;
	r = (shared == cfs_lock_shared?pthread_rwlock_rdlock(drv->rwlock):pthread_rwlock_wrlock(drv->rwlock));
	return r;
}
int
cfs_unlock_driver(cfs_origin_driver_t *drv, cfs_lock_t shared)
{
	int r = 0;
	r = pthread_rwlock_unlock(drv->rwlock);
	return r;
}
nc_origin_session_t  
cfs_allocate_session(nc_volume_context_t *mc, struct apc_open_context *aoc)  
{
	nc_origin_session_t ctx = NULL;
	

	CFS_CHECK_DRIVER_OPERATION_RTPTR(mc->osd, allocate_context, NCOF_READABLE, U_FALSE, nc_origin_session_t);
	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	ctx = mc->osd->allocate_context(mc->osd, aoc); 
	cfs_unlock_driver(mc->osd, cfs_lock_shared);

	CFS_DRIVER_UNREF(mc->osd);
	
	return ctx;
}
int 
cfs_valid_session(nc_volume_context_t *mc, nc_origin_session_t ctx, char *path, nc_off_t *off, nc_size_t *len)
{
	int		v = U_FALSE;
	
	if (!ctx) return v;

	CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, valid_context);
	CFS_DRIVER_REF(mc->osd);

	cfs_lock_driver(mc->osd, cfs_lock_shared);
	v = mc->osd->valid_context(mc->osd, ctx, path, off, len);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);

	
	return v;
}
char *
cfs_dump_session(nc_volume_context_t *mc, char *buf, int l, void *u)
{
	
	char *s;

	if (!mc->osd->dump_session) return NULL;

	CFS_DRIVER_REF(mc->osd);
	cfs_lock_driver(mc->osd, cfs_lock_shared);
	s = mc->osd->dump_session(buf, l, u);
	cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);

	
	return s;
}

void 
cfs_free_session( nc_volume_context_t *mc, nc_origin_session_t ctx )
{
	// CHG 2019-06-12 huibong reload 처리와 동시성 문제로 SIGSEGV 발생 (#32701)
	//                - config reload 처리시 호출되는 nvm_destroy_internal() 에서 volume 객체 메모리 해제했으나..
	//                - 본 함수에서 해당 volume 메모리 참조하여 SIGSEGV 발생
	//                - volume 정보가 유효하지 않은 경우 IOCB 객체를 release 하지 못하므로 메모리 누수 발생
	//                - 참조 대상 memory 가 유효한지 check 하도록 기능 추가했으나
	//                - 이미 free 되었다가 재할당된 memory 영역일 수 있어 SIGSEGV 재발생 가능함. (재발시 다른 방안 검토해야 함)
	if( mc == NULL || mc->freed == 1 )
	{
		// volume 정보가 유효하지 않은 경우 IOCB 객체를 release 하지 않으면 메모리 누수 발생하므로...
		// driver class 가 유효한 경우.. 해당 class 이용하여 iocb 객체 release 처리
		// 만약 driver 를 httpn 만 사용하는 경우가 아니라면.. 본 코드 제거해야 함.
		if( __driver_class_count > 0 && __driver_class[0]->free_context != NULL )
		{
			// httpn_free_context 함수를 direct 로 호출하지 못해서.. 편법 사용.
			// 실제로는 httpn_free_context( NULL, ctx ); 호출하면 됨.
			// cfs_lock_shared  lock 처리는  다른 origin 가능성이 크므로 안 함. 
			__driver_class[0]->free_context( NULL, ctx );
		}
		else
		{
			// iocb 객체 release 처리할 수 없으므로.. 메모리 누수 발생.. 이를 경고, 로깅 처리
			TRACE( (T_WARN, "volume and driver free_context function not valid. iocb[%p] memory leak.\n", ctx) );
		}
	}
	else
	{
		CFS_DRIVER_REF(mc->osd);
		cfs_lock_driver( mc->osd, cfs_lock_shared );
		mc->osd->free_context( mc->osd, ctx );
		cfs_unlock_driver( mc->osd, cfs_lock_shared );
		CFS_DRIVER_UNREF(mc->osd);
	}

	return;
}

int
cfs_set_read_range(nc_volume_context_t *mc, nc_origin_session_t ctx, nc_int64_t offset, nc_int64_t length)
{
    int     v = U_TRUE;

    if (!ctx) return U_FALSE;

    CFS_CHECK_DRIVER_OPERATION_RD(mc->osd, set_read_range);
	CFS_DRIVER_REF(mc->osd);
    cfs_lock_driver(mc->osd, cfs_lock_shared);
    v = mc->osd->set_read_range(mc->osd, ctx, offset, length); /* assume the offset is 0 */
    cfs_unlock_driver(mc->osd, cfs_lock_shared);
	CFS_DRIVER_UNREF(mc->osd);

    return v;
}

