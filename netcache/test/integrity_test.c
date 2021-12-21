#include <netcache_config.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include "trace.h"
#include <pthread.h>
#include <mm_malloc.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "netcache.h"
#include "ncapi.h"
#include <sqlite3.h>
#include <dirent.h>




int check_file_exist(char *vsignature, char *path, char *key, int cver, nc_obitinfo_t obi, uuid_t uuid);

char	cache_dir[128] = "/stg/cache/solproxy";

main(int argc, char *argv[])
{
	int					ret;
	nc_mdb_handle_t		*mdb;
	sqlite3				*dbs;
	sqlite3_stmt 		*stmt = NULL;
	int					colpos;
	char				*cpath;
	char				vsignature[128];
	char				key[2048];
	char				path[2048];
	int					cversion;
	char				dbpath[1024];
	uuid_t				uuid;
	nc_obitinfo_t		obi;
	
#define	GET_RECORDS		"SELECT volume_name, object_key, object_path, obits, cversion, uuid FROM objects"
#define	CHECK_EXIST		"SELECT count(*) FROM objects where"

	sprintf(dbpath,"%s/netcache.mdb", cache_dir);

	ret = sqlite3_open_v2(dbpath, &dbs, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX , NULL);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "sqlite3_open_v2(%s) - return NULL\n", dbpath);
		exit(1);
	}

	ret = sqlite3_prepare_v2(dbs, GET_RECORDS, -1, &stmt, NULL);
	ret = sqlite3_step(stmt);
	while (ret == SQLITE_ROW) {
		colpos = 0;
		strcpy(vsignature, (char *)sqlite3_column_text(stmt, colpos++));
		strcpy(key,			(char *)sqlite3_column_text(stmt, colpos++));
		strcpy(path,		(char *)sqlite3_column_text(stmt, colpos++));
		obi.op_bit_s 		= sqlite3_column_int64(stmt, colpos++);
		cversion 			= sqlite3_column_int(stmt, colpos++);
		memcpy(uuid, sqlite3_column_blob(stmt,  colpos++), sizeof(uuid_t)); 
		check_file_exist(vsignature, path, key, cversion, obi, uuid);

		ret = sqlite3_step(stmt);
	}
	sqlite3_clear_bindings(stmt);
	sqlite3_reset(stmt);
	sqlite3_finalize(stmt);

}
int 
check_file_exist(char *vsignature, char *path, char *key, int cver, nc_obitinfo_t obi, uuid_t uuid)
{
	char		*zuuid = NULL;
	char		*pf = NULL;
	int			n, i;
	struct stat	s_;
	nc_uint8_t	*puuid = (nc_uint8_t *)uuid;



	zuuid = (char *)alloca(strlen(cache_dir) + 16*2 + 10);
	pf = zuuid;
	n  = sprintf(pf, "%s/%02X/%02X/", cache_dir, puuid[1], puuid[5]);
	pf += n;

	for (i = 0; i < sizeof(uuid_t); i++) {
		n = sprintf(pf, "%02x", (puuid[i] & 0xFF));
		pf += n;
	}
	*pf = 0;

	if (ob_template != 0) return 0;

	if (stat(zuuid, &s_) != 0)  {
		/* file not found */
		fprintf(stdout, "%s/%s - UUID[%s] has no caching file:errno=%d\n", vsignature, key, zuuid, errno);
	}
	else {
		fprintf(stdout, "%s/%s - UUID[%s] OK\n", vsignature, key, zuuid);
		unlink(zuuid);
	}

}

