/*\
 *
 *	original code came from SUN developer's site
 *
\*/
#ifndef __LOCK_H__
#define __LOCK_H__
typedef unsigned int	u_uint32;


typedef struct __CRITICAL_SECTION *u_CR_t;
u_CR_t u_CR_create(int rwlock);
int u_CR_enter(u_CR_t lock, u_uint32 timeout, char *file, int lno);
int u_CR_leave(u_CR_t lock, char *file, int lno);
void u_CR_destroy(u_CR_t lock);
int u_CR_unlock(u_CR_t lpCS, char *file, int line);
int u_CR_lock(u_CR_t lpCS, int rdonly, char *file, int line);


#define	LOCK_BLOCK_COND(lck, e, r, t) 	for (r = (e?u_CR_enter(lck, 0, __FILE__, __LINE__):0),  t = 0; \
									  (t == 0); \
									  t = 1, r = (e?u_CR_leave(lck, __FILE__, __LINE__):0))
#define	LOCK_BLOCK(lck, r, t) 	for (r = u_CR_enter(lck, 0, __FILE__, __LINE__),  t = 0; \
									  (t == 0); \
									  t = 1, r = u_CR_leave(lck, __FILE__, __LINE__))
#define	LOCK_RELEASE(lck)		u_CR_leave(lck, __FILE__, __LINE__)
#define	LOCK_INIT(lck, rdlock)			lck = u_CR_create(rdlock)
#define	LOCK_FREE(lck)			u_CR_destroy(lck);

#define	READ_LOCK_BLOCK(lck, r, t) for (r = u_CR_lock(lck, 1, __FILE__, __LINE__), t = 0; \
									(t == 0) ;\
									t = 1, r = u_CR_unlock(lck, __FILE__, __LINE__))

#define	WRITE_LOCK_BLOCK(lck, r, t) for (r = u_CR_lock(lck, 0, __FILE__, __LINE__), t = 0; \
									(t == 0) ;\
									t = 1, r = u_CR_unlock(lck, __FILE__, __LINE__))
#define WRITE_LOCK_RELEASE(lck)		u_CR_unlock(lck, __FILE__, __LINE__)
#define READ_LOCK_RELEASE(lck)		u_CR_unlock(lck, __FILE__, __LINE__)


/*\
 *	LOCK error code table: almostly obsolete (93/9/20)
\*/
#define	LOCKE_OK				0
#define	LOCKE_ACQUIRED			LOCKE_OK
#define	LOCKE_BUSY				-1
#define	LOCKE_ALREADYIDLE		-2
#define	LOCKE_INCONSISTENT		-3
#define	LOCKE_STILLBUSY			LOCKE_OK
#define	LOCKE_RELEASED			LOCKE_OK
#define	LOCKE_ALREADYRELEASED	LOCKE_OK


#endif /* __LOCK_H__ */

