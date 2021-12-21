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

extern int __terminating;
static u_hash_table_t *uuid_ht = NULL;


void dm_make_uuid(uuid_t uuid);
char * uuid2string(uuid_t uuid, char *buf, int l);

uuid_t		uuid;
static unsigned long
uuid_hash(void *u)
{
	int 		i;
	unsigned long			h = 0;

	uuid_t 	*uuid = u;
	

	for (i = 0; i < sizeof(uuid_t); ) {
		h += (*uuid)[i];
		i += 2;
	}
	return h;
}
static u_boolean_t
uuid_equal(void *a, void *b)
{
	uuid_t	*u1 = a;
	uuid_t 	*u2 = b;

	return memcmp(*u1, *u2, sizeof(uuid_t)) == 0;
}

static uuid_t	k_uuid;
static int 		k_uuid_init = 0;


void *
uuid_generator(void *u)
{
	int			maxg = *(int *)u;
	int			i;
	uuid_t		*uuid, *lookup;
	char		d1[128], d2[128];
	
	fprintf(stderr, "uuid_generator began\n");
	for (i = 0; i < maxg; i++) {
		uuid = XMALLOC(sizeof(uuid_t), AI_ETC);
		dm_make_uuid(*uuid);
#if 0
		if (__sync_bool_compare_and_swap(&k_uuid_init, 0, 1)) {
			memcpy(k_uuid, *uuid, sizeof(uuid_t));
		}

		if (i && (i % 10) == 0) {
			memcpy(*uuid, k_uuid, sizeof(uuid_t));
		}
#endif
		lookup = (uuid_t *)u_ht_lookup(uuid_ht, (void *)*uuid);

		if (lookup) {
			TRACE((T_ERROR, "UUID[%s] already gen'ed\n",  uuid2string(*lookup, d1, sizeof(d1))));
			TRAP;
		}
		else {
			if (!u_ht_insert_dbg(uuid_ht, (void *)uuid, uuid, U_FALSE, __FILE__, __LINE__)) {
				TRACE((T_ERROR, "UUID[%s] insert failed\n",  uuid2string(*uuid, d1, sizeof(d1))));
			}
			else {
				if (i && (i % 10) == 0)
					TRACE((T_MONITOR, "so far %d entries gen and added\n", i+1));
			}
		}
	}

	return NULL;
}

int 
main()
{
#define MAX_WORKER		10
	pthread_t				tid[MAX_WORKER];
	char					logfile[] = "./uuid_test.log";
	nc_uint32_t				logmask = NC_WARN|NC_ERROR|NC_INFO;
	int						maxg = 100000;
	int						i;


 	nc_setup_log(logmask, logfile, 50000, 10);
	uuid_ht = (u_hash_table_t *)u_ht_table_new(uuid_hash, uuid_equal, NULL, NULL); 

	for (i = 0; i < MAX_WORKER; i++) 
		pthread_create(&tid[i], NULL, uuid_generator, &maxg);
	TRACE((T_MONITOR, "waiting for completion...\n"));
	for (i = 0; i < MAX_WORKER; i++)  {
		pthread_join(tid[i], NULL);
	}
	fprintf(stderr, "%d workers finished\n", MAX_WORKER);
	return 0;
}
