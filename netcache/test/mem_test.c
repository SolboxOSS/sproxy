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

#ifndef MODULE_TEST

#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#include <sys/vfs.h>
#include <ctype.h>

#include "disk_io.h"
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "bitmap.h"
#include "trace.h"
#include "bt_timer.h"
#include <mm_malloc.h>
#include <snprintf.h>
#endif
#include <assert.h>

memmgr_heap_t 	H;

void fdump(char *l)
{
fprintf(stderr, "%s\n", l);
}
void *
run_alloc(void *u)
{
	int		i;
	int 	memsz;
	void 	*mp;

	for (i = 0; i < 10000; i++) {
		memsz = rand() % 500000 + 1;
		mp = XMALLOC(memsz, AI_ETC);
		mp = XREALLOC(mp, memsz*2, AI_ETC);

		XFREE(mp);
	}
	return NULL;
}
int main()
{
	char					logfile[] = "./mem_test.log";
	nc_uint32_t				logmask = NC_WARN|NC_ERROR|NC_INFO;
	int						i = 0;
	void 					*mp = NULL;
	size_t					memsz = 0;
	char					_t[] = "01234567890!@#$%^&*(*)_";
	nc_int64_t				t;
	nc_int64_t				u;
	char					*p = NULL;
	pthread_t				tid[10];

	srand(time(NULL));
 	nc_setup_log(logmask, logfile, 50000, 10);
	mem_init(&H, 128*1024*1024);
	TRACE((T_WARN, "mem_init'ed\n"));
	fprintf(stderr, "sizeof(page_ctrl) = %d\n", sizeof(nc_page_ctrl_t));
	for (i = 0; i < 100; i++) {
		memsz = rand() % 500000 + 1;
		if ( i && (i % 10) == 0)
			mp = mem_strdup(&H, _t, AI_INODE, __FILE__, __LINE__);
		else {
			mp = mem_alloc(&H, memsz, 0, AI_INODE, U_FALSE, __FILE__, __LINE__);
			if (mp) {
				mp = mem_realloc(&H, mp, memsz*2, AI_INODE, __FILE__, __LINE__);
			}
		}
		assert(mp  != NULL);
		mem_free(&H, mp, 0, __nc_get_len(mp), __FILE__, __LINE__);
	}
	mem_stat(&H, &u, &t);
	fprintf(stderr, "%lld/%lld used\n", u, t);

	for (i = 0; i < 10; i++) {
		pthread_create(&tid[i], NULL, run_alloc, (void *)NULL);
	}

	for (i = 0; i < 10; i++) {
		pthread_join(tid[i], NULL);
	}





	p = memalign(512, 4096);
	free(p);
	malloc(10);
	nc_raw_dump_report(fdump);
}
