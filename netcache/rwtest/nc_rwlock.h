#ifndef __NC_RWLOCK_H__
#define __NC_RWLOCK_H__
typedef struct nc_rw_lock nc_rw_lock_t; 

void nc_rw_init(struct nc_rw_lock *rwl, int maxwaiters);
void nc_rw_rdlock(struct nc_rw_lock *rwl);
void nc_rw_rdunlock(struct nc_rw_lock *rwl);
void nc_rw_wrlock(struct nc_rw_lock *rwl);
void nc_rw_wrunlock(struct nc_rw_lock *rwl);
void nc_rw_destroy(struct nc_rw_lock *rwl);


#endif /* __NC_RWLOCK_H__ */
