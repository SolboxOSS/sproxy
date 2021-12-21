#ifndef __SHMEM_WRAPPER_H__
#define __SHMEM_WRAPPER_H__

#include <stdio.h>
#include <stdlib.h>
#ifdef WIN32
#include <windows.h>
#include <unistd.h>
#include <sys/types.h>
//#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
typedef HANDLE 	nc_shm_mapf_t;
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
typedef int 	nc_shm_mapf_t;
#endif

typedef struct nc_shm_tag {
	void 				*base;
	size_t 				len;
	char 				*path;
	nc_shm_mapf_t 	map;
} nc_shm_t;

nc_shm_t * nc_shm_attach(const char *path, size_t size, int);
int nc_shm_getdata(const nc_shm_t * shm, void * __restrict dst, off_t from, size_t len);
int nc_shm_putdata(const nc_shm_t * shm, const void * src, off_t from, size_t len);
void nc_shm_detach(nc_shm_t *map);
void nc_shm_clear(nc_shm_t *shm);



#endif /*__SHMEM_WRAPPER_H__*/
