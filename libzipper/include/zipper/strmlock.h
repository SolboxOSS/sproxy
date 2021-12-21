
#ifndef __zipper__strmlock__
#define __zipper__strmlock__

#include "io.h"

//#define STRM_SPIN_LOCK

#ifndef STRM_SPIN_LOCK
#include <signal.h>
#include <pthread.h>
#else
#ifdef __APPLE__
#include <libkern/OSAtomic.h>
#else
#include <pthread.h>
#endif
#endif

typedef struct _strm_lock_context* strmLockCtx;

typedef struct _strm_lock_context
{
#ifndef STRM_SPIN_LOCK

    volatile sig_atomic_t   disable;
    volatile sig_atomic_t   ref;
   
#else
#ifdef __APPLE__
    OSSpinLock plock;
    
#else
    pthread_spinlock_t  plock;
#endif
    
    uint32_t disable:1;
    uint32_t ref:31;
    
#endif
    
} strm_lock_context;

int create_strm_lock(zipper_io_handle *io_handle, strmLockCtx *ctx);
void destroy_strm_lock(zipper_io_handle *io_handle, strmLockCtx *ctx);

void strm_lock(strmLockCtx ctx, char owner);
char strm_try_lock(strmLockCtx ctx, char owner);
void strm_unlock(strmLockCtx ctx, char owner);


#endif /* defined(__zipper__strmlock__) */
