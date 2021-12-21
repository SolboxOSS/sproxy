#include <config.h>
#include <stdlib.h>
#include <pthread.h>
#include <util.h>



int
nc_ulock_init(nc_ulock_t *ul)
{

#ifdef NC_RWLOCK_USE_SPINLOCK
	pthread_spin_init(&ul->slock, PTHREAD_PROCESS_PRIVATE);
#else
	pthread_mutex_init(&ul->slock, NULL);
#endif
	pthread_rwlock_init(&ul->rwlock, NULL);


	return 0;
}
int
nc_ulock_rdlock(nc_ulock_t *ul)
{
	int 	e = 0;

	if (pthread_rwlock_tryrdlock(&ul->rwlock))
		e = pthread_rwlock_rdlock(&ul->rwlock);


	return e;
}

int
nc_ulock_wrlock(nc_ulock_t *ul)
{
	int 	e = 0;

	if (pthread_rwlock_trywrlock(&ul->rwlock))
		e = pthread_rwlock_wrlock(&ul->rwlock);

	return e;
}
int
nc_ulock_upgrade(nc_ulock_t *ul)
{

	int 	e = 0;
#ifdef NC_RWLOCK_USE_SPINLOCK
	pthread_spin_lock(&ul->slock); 

	pthread_rwlock_unlock(&ul->rwlock);


	if ((e = pthread_rwlock_trywrlock(&ul->rwlock)) != 0) {
	
		/*
		 * need to wait for acquiring the writer lock
		 */
		pthread_spin_unlock(&ul->slock);
		/*
		 * try again by waiting
		 */
		e = pthread_rwlock_wrlock(&ul->rwlock); 
		return  e;
	} 
	else 
		pthread_spin_unlock(&ul->slock);
#else

	pthread_mutex_lock(&ul->slock);

	e = pthread_rwlock_unlock(&ul->rwlock);

	if (e = pthread_rwlock_trywrlock(&ul->rwlock)) {
	
		/*
		 * need to wait for acquiring the writer lock
		 */
		pthread_mutex_unlock(&ul->slock);
		/*
		 * try again by waiting
		 */
		e = pthread_rwlock_wrlock(&ul->rwlock); 
	} 
	else 
		e = pthread_mutex_unlock(&ul->slock);

#endif
	
	return e;
}
int
nc_ulock_unlock(nc_ulock_t *ul)
{
	int 	r = 0;	

	r = pthread_rwlock_unlock(&ul->rwlock);

	return r;
}
void
nc_ulock_destroy(nc_ulock_t *ul)
{
	pthread_rwlock_destroy(&ul->rwlock);
}
