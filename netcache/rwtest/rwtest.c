#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

struct nc_rw_lock {
	pthread_mutex_t		lock;
	sem_t				sema;
	int					total;
};


static int
nc_rw_get_running(struct nc_rw_lock *rwl)
{
	int		v;
	sem_getvalue(&rwl->sema, &v);
	return v;
}
void 
nc_rw_init(struct nc_rw_lock *rwl, int maxwaiters)
{
	pthread_mutex_init(&rwl->lock, NULL);
	rwl->total = maxwaiters;
	sem_init(&rwl->sema, 0, rwl->total);
}
void
nc_rw_rdlock(struct nc_rw_lock *rwl)
{
	sem_wait(&rwl->sema);
}
void
nc_rw_rdunlock(struct nc_rw_lock *rwl)
{
	sem_post(&rwl->sema);
}
void
nc_rw_wrlock(struct nc_rw_lock *rwl)
{
	int		i;
	pthread_mutex_lock(&rwl->lock);

	for (i = 0; i < rwl->total;i++) {
		sem_wait(&rwl->sema);
	}
	pthread_mutex_unlock(&rwl->lock);
}
void
nc_rw_wrunlock(struct nc_rw_lock *rwl)
{
	int		i;

	for (i = 0; i < rwl->total; i++) {
		sem_post(&rwl->sema);
	}
}
void
nc_rw_destroy(struct nc_rw_lock *rwl)
{
	pthread_mutex_destroy(&rwl->lock);
	sem_destroy(&rwl->sema);
}

struct  nc_rw_lock	L;
int		w_cnt = 0;
int		r_cnt[2] = {0,0};
int		counter;
void *w_worker(void *d)
{
	int		*pcounter = (int *)d;
	int		i, j;
	for (i = 0; i < 1000; i++) {
		for (j = 0; j < 100; j++) {
			nc_rw_wrlock(&L);
			counter++;
			w_cnt++;
			nc_rw_wrunlock(&L);
		}
		putchar('W');
	}
}
void *r_worker(void *d)
{
	int		id = (int )d;
	int		i, j;

	for (i = 0; i < 1000; i++) {
		for (j = 0; j < 100; j++) {
			nc_rw_rdlock(&L);
			r_cnt[id]++;
			nc_rw_rdunlock(&L);
		}
		putchar((id == 0?'r':'R'));
	}
}
pthread_t	w;
pthread_t	r[2];
main()
{

	nc_rw_init(&L, 2);
	pthread_create(&w, NULL, w_worker, (void *)&counter);
	pthread_create(&r[0],NULL, r_worker, (void *)0);
	pthread_create(&r[1],NULL, r_worker, (void *)1);
	pthread_join(r[0], NULL);
	pthread_join(r[1], NULL);
	pthread_join(w, NULL);

	fprintf(stderr, "counter = %d\n", counter);
}
