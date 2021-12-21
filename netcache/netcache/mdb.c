#include <config.h>
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
#include <pthread.h>
#ifdef HAVE_SYS_DIR_H
#include <sys/dir.h>
#endif
#ifdef HAVE_DIR_H
#include <dir.h>
#endif
#include <dirent.h>
#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#ifdef WIN32
#include <statfs.h>
#endif
#ifndef WIN32
#include <sys/vfs.h>
#endif
#include <ctype.h>
#include <regex.h>
#include "disk_io.h"
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "bitmap.h"
#include "trace.h"
#include "bt_timer.h"
#include "md5.h"
#include <mm_malloc.h>
#include "tlc_queue.h"
#include <sqlite3.h>


#define MDB_OPCODE_OPERATION


#define MDB_RWLOCK_DEBUG

#define MDB_SLEEP_MS 	10 /* msec*/
#define MDB_MAX_RETRY 	500 /* 100 times retry*/
#undef 	MDB_USE_PRIMARY_KEY





#if SQLITE_VERSION_NUMBER < 3006020
#error 	"SQLite Version too old. need to upgrade 3.6.20 or the later!"
#endif
#define 	MDB_CACHE_SIZE		(2*1024*1024) 	/* MB */


#define 	MDBE_OK					0
#define		MDBE_ERROR				-1
#define		MDBE_OBJECT_ERROR 		-2 /* object 존재 안함*/
#define		MDBE_COLUMN_ERROR 		-3 /* column 존재 안함*/

#define MDB_CHECK_BIT(_bv)	((_bv)?1:0)
#define	MDB_UPDATE_THRESHOLD	0



#define 		MDB_BPI(_s, _y) 	sqlite3_bind_parameter_index(_s, _y)

#if SQLITE_VERSION_NUMBER < 3007015
#define	MDB_CHECK_SUPPORT(x)	{\
									int _r = (x); \
									if (_r != SQLITE_OK && _r != SQLITE_DONE ) \
										TRACE((T_ERROR, "SQLite3 Configuration '%s' not supported in this version(error code=%d)\n", #x, _r)); \
								}
#define	MDB_CHECK_ERROR(x_)		(x_ != SQLITE_OK && x_ != SQLITE_DONE)

#define	MDB_CHECK_RESULT(_mdb, x)	{ \
										int _r = (x); \
										if (_r != SQLITE_OK && _r != SQLITE_DONE ) { \
											TRACE((T_ERROR, "SQLite3 Operation '%s' got error(%d) in %d@%s\n", #x, (int) _r, __LINE__,__FILE__)); \
											DEBUG_TRAP; \
										} \
									}
#else
#define	MDB_CHECK_SUPPORT(x)	{\
									int _r = (x); \
									if (_r != SQLITE_OK && _r != SQLITE_DONE ) \
										TRACE((T_ERROR, "SQLite3 Configuration '%s' not supported in this version(%s)\n", #x, sqlite3_errstr(_r))); \
								}
#define	MDB_CHECK_ERROR(x_)		(x_ != SQLITE_OK && x_ != SQLITE_DONE)
#define	MDB_CHECK_RESULT(_mdb, x)	{\
										int _r = (x); \
										if (_r != SQLITE_OK && _r != SQLITE_DONE ) \
											TRACE((T_ERROR, "SQLite3 Operation '%s' got error(%d:%s) in %d@%s\n", \
															(char *)#x, \
															(int) _r, \
															(char *)sqlite3_errstr(_r), __LINE__, (char *)__FILE__)); \
									}
#endif

#ifdef MDB_LRU_DEBUG
#define	RECL_INTERVAL		(1800)
#else
#define	RECL_INTERVAL		(3600)
#endif
#define 		MDB_NORMALIZE_VIEWINDEX(_CT, _V)  (nc_int32_t)(_V - ((nc_cached_clock() - _CT) / RECL_INTERVAL)*8)


/*
 * SCHEMA 설명
 *  	- obits는 complete등 다양한 bitfield에 대해서 저장하는 공간
 * 		- primary key = {volume_name, case_identifier, object_key}
 * 		- secondary key = {atime}
 * 		- secondary key = {volume_name, case_identifier, object_path}
 */


/* CHG 2018-10-17 huibong soft purge 성능 향상을 위한 idx_object_path index 생성시 vtime 정보 추가 (#32386) */

#define	MDB_DB_SCHEMA_OBJECTS 		"CREATE TABLE objects( " \
									" 		volume_name		VARCHAR(64) ," \
									" 		case_id			CHAR 	," \
									" 		object_key 		VARCHAR(4906) ," \
									" 		object_path 	VARCHAR(4906) ," \
									" 		uuid			BLOB 	," \
									" 		size			INT8 	," \
									" 		rsize			INT8 	," \
									" 		cversion		INT4 	DEFAULT 1," \
									" 		mode			INT4 	," \
									" 		ctime			INT4 	," \
									" 		mtime			INT4 	," \
									" 		vtime			INT4 	," \
									" 		viewcount		INT4 	," \
									" 		viewindex		INT4 	," \
									" 		hid				INT8 	," \
									" 		devid			CHAR(16) ," \
									" 		obits			INT8 	," \
									" 		origincode		INT2 	," \
									" 		packed_property	BLOB 	," \
									"		atime			DATETIME," \
									"		masked			INTEGER  PRIMARY KEY AUTOINCREMENT," \
									"		UNIQUE (volume_name,object_key, case_id, cversion) " \
									"		);" \
									"CREATE INDEX idx_object_hid ON objects(hid); "  \
									"CREATE INDEX idx_object_lru ON objects(atime,ctime,viewcount); "  \
									"CREATE INDEX idx_object_path ON objects(volume_name, object_path, case_id, cversion, vtime); "  \
									"CREATE TRIGGER insert_atime_trigger AFTER INSERT ON objects " \
									"BEGIN " \
									" 		UPDATE objects SET atime=STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE volume_name=NEW.volume_name AND object_key=NEW.object_key AND case_id=NEW.case_id AND cversion=NEW.cversion; " \
									"END; " \
									"CREATE TRIGGER update_atime_trigger AFTER UPDATE ON objects " \
									"BEGIN " \
									" 		UPDATE objects SET atime=STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE volume_name=NEW.volume_name AND object_key=NEW.object_key AND case_id=NEW.case_id AND cversion=NEW.cversion; " \
									"END; " 




/* ir #24803 : volumes table 생성 */
#define	MDB_DB_SCHEMA_VOLUMES 		"CREATE TABLE volumes( " \
									" 		volume_name		VARCHAR(64) , " \
									" 		version 		INT4	, "\
									"		PRIMARY KEY(volume_name) " \
									"		ON CONFLICT REPLACE " \
									"		);"  \
									"INSERT INTO volumes(volume_name, version) VALUES ('___VOLUMES_AGE___', 1); "


#define		MDB_VOLUMES_AGE			"___VOLUMES_AGE___"

/* ir #24803 : cversion 컬럼 추가 (기본값 0) */
#define	MDB_ADD_COLUMN_CVERSION 	"ALTER TABLE objects ADD COLUMN cversion INT4 DEFAULT 0 "
/*
 * position-based parametized query
 * 등록과 변경은 하나의 transaction으로 처리.
 */
#define	MDB_UPDATE_BY_ROWID_PGL "UPDATE objects SET "\
								"			size		= @size, "\
								"			rsize		= @rsize, "\
								"			mode		= @mode, "\
								"			vtime		= @vtime, "\
								"			obits		= @obits "\
								"WHERE rowid=@rowid "

#define	MDB_INSERT_PGL		"INSERT INTO objects( " \
							"				volume_name, " \
							"				case_id, " \
							"				object_key, " \
							"				object_path, " \
							"				uuid, " \
							"				size, " \
							"				rsize, " \
							"				cversion, " \
							"				mode, " \
							"				ctime, " \
							"				mtime, " \
							"				vtime, " \
							"				viewcount, " \
							"				viewindex, " \
							"				hid, " \
							"				devid, " \
							"				obits, " \
							"				origincode, " \
							"				packed_property) " \
							"VALUES(  " \
							"		@volume_name, " \
							"		@case_id, " \
							"		@object_key, " \
							"		@object_path, " \
							"		@uuid, " \
							"		@size, " \
							"		@rsize, " \
							"		@cversion, " \
							"		@mode, " \
							"		@ctime, " \
							"		@mtime, " \
							"		@vtime, " \
							"		@viewcount, " \
							"		@viewindex, " \
							"		@hid, " \
							"		@devid, " \
							"		@obits, " \
							"		@origincode, " \
							"		@packed_property) " \



		

#define MDB_DB_LOOKUP 		"SELECT  " \
							"		volume_name, " \
							"		case_id, " \
							"		object_key, " \
							"		object_path, " \
							"		uuid, " \
							"		size, " \
							"		rsize, " \
							"		cversion, " \
							"		mode, " \
							"		ctime, " \
							"		mtime, " \
							"		vtime, " \
							"		viewcount, " \
							"		viewindex, " \
							"		hid, " \
							"		devid, " \
							"		obits, " \
							"		origincode, " \
							"		packed_property, " \
							"		rowid " \
							"FROM objects WHERE hid=@hid AND object_key=@key AND volume_name=@signature AND case_id=@case_id; "

							//"FROM objects WHERE hid=@hid AND object_key=@key AND volume_name=@signature AND case_id=@case_id ORDER BY cversion DESC; "

#define	MDB_DB_REMOVE		"DELETE FROM objects WHERE hid=@hid AND object_key=@object_key AND volume_name=@volume_name AND case_id=@case_id AND cversion=@cversion; "

#define	MDB_DB_REUSE		"UPDATE objects " \
						    "SET  cversion=@cversion, vtime=@vtime " \
							"WHERE rowid=@rowid " \

 /*
  * 사용된 시간이 가장 오래된것 순서로 100 records만 가져오기
  */


#define MDB_DB_GET_OLDEST			"SELECT volume_name, object_key, object_path, obits, uuid, cversion, rowid " \
									"FROM objects "\
									"ORDER BY atime " \
									"LIMIT 100"




/* CHG 2018-10-17 huibong soft purge 조회 성능 향상 (#32386, #32378) 
                  - index 를 사용하도록 vtime <> 0 연산을 vtime > 0 으로 변경
*/
#define MDB_DB_GET_SOFT_PURGE_KEY 	"SELECT object_key, object_path, obits, uuid, cversion, rowid FROM objects WHERE volume_name=? AND case_id=? AND object_key GLOB ? AND vtime <> 0  "
#define MDB_DB_GET_SOFT_PURGE_PATH 	"SELECT object_key, object_path, obits, uuid, cversion, rowid  FROM objects WHERE volume_name=? AND case_id=? AND object_path GLOB ? AND vtime <> 0  "

#define MDB_DB_GET_HARD_PURGE_KEY 	"SELECT object_key, object_path, obits, uuid, cversion , rowid FROM objects WHERE volume_name=? AND case_id=? AND object_key GLOB ?  "
#define MDB_DB_GET_HARD_PURGE_PATH 	"SELECT object_key, object_path, obits, uuid, cversion , rowid FROM objects WHERE volume_name=? AND case_id=? AND object_path GLOB ?  "


#define		NO_TX					0
#define		SHARED_TX				1
#define		IMMEDIATE_TX			2

#define		MDB_PREPARE_OP_TRANSACTION(f_, db_, s_, tx_, ret_) L_##f_ : \
		if (tx_> 0) { \
			do { \
				if (tx_ == SHARED_TX) \
					ret_ = sqlite3_exec((s_)->DB, "BEGIN TRANSACTION", NULL, NULL, NULL); \
				else \
					ret_ = sqlite3_exec((s_)->DB, "BEGIN IMMEDIATE TRANSACTION", NULL, NULL, NULL); \
			} while (ret_ == SQLITE_BUSY); \
		}


#define		MDB_FINISH_OP_TRANSACTION(f_, db_, s_, tx_, ret_) \
	if (tx_) { \
		if (ret_ == SQLITE_BUSY) { \
			/* sleep */ \
			MDB_CHECK_RESULT(db_, sqlite3_exec((s_)->DB, "ROLLBACK TRANSACTION", NULL, NULL, NULL)); \
			bt_msleep(100); \
			goto L_##f_ ;\
		} \
		else if (ret_ == SQLITE_OK)  { \
			MDB_CHECK_RESULT(db_, sqlite3_exec((s_)->DB, "COMMIT TRANSACTION", NULL, NULL, NULL)); \
		} \
		else {  \
			MDB_CHECK_RESULT(db_, sqlite3_exec((s_)->DB, "ROLLBACK TRANSACTION", NULL, NULL, NULL)); \
		} \
	} 


extern void  *	__timer_wheel_base; 

struct property_pack_info {
	nc_xtra_options_t 	*property;
	char 		*vfree;
	int 		packed;
	int 		allocsiz;
	char 		*allocptr;
	int 		cnt;
}; 
#define	RWLOCK_MAGIC 0x5AA5A55A
#define	RWLOCK_CHECK_MAGIC(_rw) 		DEBUG_ASSERT((_rw)->magic == RWLOCK_MAGIC)
typedef struct mdb_rwlock {
	nc_uint32_t			magic;
	int 				waiters;
	int 				reader;
	int 				writer;
	pid_t 				owner;
	nc_time_t 			t_xlock_bg; /* write가 lock획득한 시점 */
	char				xo_file[128];
	int					xo_line;
	pthread_mutex_t 	lock;
	pthread_cond_t 		signal;
	link_list_t 		queue;
} mdb_rwlock_t;

/* disk-based meta DB */
#define 	MDB_MAGIC 	0xA55A5AA5

#define		MDB_OP_INSERT				MDB_INSERT_PGL
#define		MDB_OP_UPDATE_ROWID			MDB_UPDATE_BY_ROWID_PGL 
#define		MDB_OP_SOFT_PURGE_ROWID		"UPDATE objects SET vtime=0 WHERE rowid=@rowid"
#define		MDB_OP_HARD_PURGE_ROWID		"DELETE FROM objects WHERE rowid=@rowid"
#define		MDB_OP_REUSE_ROWID			MDB_DB_REUSE
#define		MDB_OP_LOOKUP				MDB_DB_LOOKUP

#define		MDB_OP_LIST_SOFT_PURGE_WITH_KEY		MDB_DB_GET_SOFT_PURGE_KEY
#define		MDB_OP_LIST_SOFT_PURGE_WITH_PATH	MDB_DB_GET_SOFT_PURGE_PATH
#define		MDB_OP_LIST_HARD_PURGE_WITH_KEY		MDB_DB_GET_HARD_PURGE_KEY
#define		MDB_OP_LIST_HARD_PURGE_WITH_PATH	MDB_DB_GET_HARD_PURGE_PATH




#define		MDB_STMT_INSERT						0
#define		MDB_STMT_UPDATE_ROWID				1
#define		MDB_STMT_SOFT_PURGE_ROWID			2
#define		MDB_STMT_HARD_PURGE_ROWID			3 
#define		MDB_STMT_REUSE_ROWID				4		
#define		MDB_STMT_LOOKUP						5		
#define		MDB_STMT_LIST_SPWK					5		
#define		MDB_STMT_LIST_SPWP					7
#define		MDB_STMT_LIST_HPWK					8
#define		MDB_STMT_LIST_HPWP					9

#define		MDB_MAX_STMTS						9

typedef struct tag_mdb_session {
	sqlite3 		*DB;
	link_node_t 	node;
	sqlite3_stmt	*stmt[MDB_MAX_STMTS];
	int 			refs;
	int 			maxretry;	/* max allowable retry */
	int 			sleepms; 	/*sleep time in ms */
	int 			handled;	/* # of use */
	int 			state; /* 0: idle or committed, 1: begin */
	char 			*query;
} mdb_session_t ;
#define 	MDB_WRITER_NETCACHE	0
#define 	MDB_WRITER_VOLUMES	1
struct tag_nc_mdb_handle {
	char 				_corrupt[256];
	nc_uint32_t 		magic; /* 0xA55A5AA5 */
	char				dbpath[1024];
	char				dbvolpath[1024];
	char				dblrupath[1024]; /* lru db , temporary!!!*/
	nc_part_element_t 	*part;
	char				*signature;
	int 				needstop;
	int 				refs; /* reference count */
	mdb_rwlock_t 		lock;
	mdb_rwlock_t 		vollock;
	pthread_mutex_t 	poollock;

	nc_lock_t 			batchlock;

	nc_cond_t	 		donesignal;
	int					donewaiters;
	mdb_tx_info_t		txno;
	mdb_tx_info_t		txno_done;
	pthread_mutex_t 	writerlock;
	pthread_cond_t 		writersignal;
	link_list_t 		pool; /*DB session pool */
 	int 				session_count;	
	bt_timer_t 			t_checkpoint_timer; 
	mdb_session_t 		*writers[2];
	mdb_session_t 		*volumes;

	pthread_t			batch_handler;
	tlc_queue_t			batch_queue;
};
typedef enum {
	MDBOP_INSERT = 1,
	MDBOP_PURGE_ROWID = 2,
	MDBOP_UPDATE_ROWID = 3
} mdb_opcode_t;

static nc_uint64_t				s__upseq = 1LL;

/*
 * TODO)
 * 	- 신중하게 판단해야함
 *  - 당분간 remove는 동기식 operation으로 홀딩
 */
typedef struct mdb_removei_tag {
	// in params
	mdb_opcode_t 	op;
	nc_uint64_t		seq;
	char			*signature;
	char 			case_id;
	char			*key;
	uuid_t			uuid;
	nc_uint32_t		cversion;
	nc_uint64_t		hid;
	//
	// out params
	mdb_tx_info_t	*ptxno;
	nc_cond_t		handled;
	nc_lock_t		lock;
} mdb_removei_t;

typedef struct mdb_remove_rowi_tag {
	// in params
	mdb_opcode_t 	op;
	nc_uint64_t		seq;
	uuid_t			uuid;
	nc_int64_t		rowid;
	nc_uint32_t		ishard;
	nc_time_t		atime;
	//
	// out params
	int				affected;
	mdb_tx_info_t	*ptxno;
	nc_cond_t		handled;
	nc_lock_t		lock;
} mdb_rrow_t;

typedef struct mdb_update_rowi_tag {
	// in params
	mdb_opcode_t 	op;
	nc_uint64_t		seq;

	nc_size_t		size;
	nc_size_t		rsize;
	nc_mode_t		mode;
	nc_time_t		vtime;
	nc_uint64_t		obi;
	uuid_t			uuid;
	nc_int64_t		rowid;
	//
	// out params
	mdb_tx_info_t	*ptxno;
	int				affected;
	nc_cond_t		handled;
	nc_lock_t		lock;
} mdb_updrow_t;


typedef struct mdb_inserti_tag {
	//
	// in params
	mdb_opcode_t 	op;
	nc_uint64_t		seq;
	int				insert;
	char			*signature;
	char 			case_id;
	char			*key;
	char			*path;
	char			*packed;
	size_t			packed_size;
	uuid_t			uuid;
	nc_devid_t 		devid;			/**<특정 deivce에 저장된 객체의 ID */
	int				viewindex;
	int				viewcount;
	nc_int64_t		size;
	nc_int64_t		rsize;
	nc_uint32_t		cversion;
	nc_uint32_t		imode;
	nc_time_t		ctime;
	nc_time_t		mtime;
	nc_time_t		vtime;
	nc_uint64_t		hid;
	nc_uint64_t		obit;
	nc_int16_t		origincode;

	//
	// out params
	mdb_tx_info_t	*ptxno;
	nc_int64_t		rowid;
	nc_int64_t		affected;
	nc_cond_t		handled;
	nc_lock_t		lock;
} mdb_inserti_t;

#define	MDB_IO_READ			0
#define	MDB_IO_WRITE		1
#define	MDB_IO_REMOVE		2
static nc_uint64_t		__mdb_io_counter[3] = {0,0,0};
static int 				__mdb_global_init = 0;
//#if defined(NC_MEM_DEBUG) 
static int 				__mdb_enable_custom_alloc = 1;
//#endif
static nc_uint32_t		__primary_age = 1;
static nc_mdb_handle_t	*__primary_mdb = NULL;
static mdb_session_t * mdb_create_session(nc_mdb_handle_t *mdb, char * dbname);
static mdb_session_t * mdb_pop_session(nc_mdb_handle_t *mdb, int needcreate);
static void mdb_push_session(nc_mdb_handle_t *mdb, mdb_session_t *dbs, int needclose);
static int mdb_check_error(nc_mdb_handle_t *mdb, int ret);
static char * mdb_inode_pack_properties(nc_xtra_options_t *property, size_t *packed_size);
static nc_xtra_options_t * mdb_inode_unpack_properties(char *packed_array);
static int mdb_purge_with_key(	nc_volume_context_t *volume, 
								nc_mdb_handle_t *mdb, 
								struct purge_info pi[], 
								int key_cnt, 
								int ishard, 
								int iskey,
								int (*purge_cache_object)(nc_volume_context_t *vol, char *path, char *key, uuid_t , nc_int64_t, int , int , nc_path_lock_t *, nc_part_element_t *, void *), 
								void *delete_cache_ctx
								);

static void MDB_report_log(void *not_used, int resultcode, char * msg);
static nc_uint32_t mdb_read_version_internal__(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *vname);
static int mdb_check_exists(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *table, char *column);
static int mdb_check_exist_object(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *type, char *object);
static int mdb_upgrade_version_internal__(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *volume, nc_uint32_t);
static int mdb_blocking_prepare_v2( sqlite3 *db, char *zSql, int nSql, sqlite3_stmt **ppStmt,   const char **pz);
static int mdb_blocking_step(sqlite3_stmt *pStmt);
static mdb_session_t * mdb_acquire_writer(nc_mdb_handle_t *mdb, int wt);
static void mdb_release_writer(nc_mdb_handle_t *mdb, mdb_session_t *s, int wt);
static int mdb_register_hash_function(sqlite3 *);
static void mdb_force_checkpoint(nc_mdb_handle_t *mdb, mdb_session_t * dbs, int forced);
static void mdb_checkpoint_timer(void *mdb);
static int mdb_reuse_direct(nc_mdb_handle_t *mdb, mdb_session_t *dbs, uuid_t uuid, nc_int64_t rowid, nc_time_t vtime, nc_uint32_t nver);

static void * mdb_batch_upsert_handler(void *u);
static int mdb_busy_handler(void *pArg, int nbusy);
static int __mdb_convert_table(nc_mdb_handle_t *mdb, mdb_session_t *dbs);
static sqlite3_stmt * mdb_get_cached_stmt(nc_mdb_handle_t *mdb, mdb_session_t *session, int idx);
static int mdb_op_purge_rowid(nc_mdb_handle_t * mdb, mdb_session_t *session, int tx, int ishard, nc_int64_t	rowid); 
static int mdb_op_insert(	
				nc_mdb_handle_t* mdb,
				mdb_session_t *	session, 
				int				txmode,
				char *			signature, 
				char *			path, 
				char *			key, 
				nc_uint64_t		hid,
				char			caseid, 
				uuid_t			uuid,
				int				cversion,
				nc_size_t		size,
				nc_size_t		rsize,
				nc_time_t		ctime,
				nc_time_t		mtime,
				nc_time_t		vtime,
				nc_mode_t		mode,
				char			*devid,
				nc_uint64_t		obi,
				int				origincode,
				int				viewcount,
				int				viewindex,
				char			*packed,
				int				packed_size
				);
nc_uint32_t FNV1A_Hash_Yoshimura(char *str, nc_uint32_t wrdlen);





#pragma pack(1)
typedef struct {
	char 	signature[4];
	long 	line;
} mdb_line_t;
#pragma pack(0)
#define MDB_ML_SIZE sizeof(mdb_line_t)
#define MDB_ML_PAD  8
#define MDB_ML_SIZEX(_x) 	(_x + MDB_ML_SIZE+ MDB_ML_PAD)


#define	MDB_ENABLE_SINGLE_WRITER




static char s__op_array[][512] = {
	MDB_OP_INSERT,
	MDB_OP_UPDATE_ROWID,
	MDB_OP_SOFT_PURGE_ROWID,
	MDB_OP_HARD_PURGE_ROWID,
	MDB_OP_REUSE_ROWID,
	MDB_OP_LOOKUP,
	MDB_OP_LIST_SOFT_PURGE_WITH_KEY,
	MDB_OP_LIST_SOFT_PURGE_WITH_PATH,
	MDB_OP_LIST_HARD_PURGE_WITH_KEY,	
	MDB_OP_LIST_HARD_PURGE_WITH_PATH
};
void *
mdb_pool_malloc(int sz)
{
	return __nc_malloc(sz, AI_MDB, __FILE__, __LINE__);
}
void 
mdb_pool_free(void *m)
{
	__nc_free(m, __FILE__, __LINE__);
}
void
mdb_check_leak(void *u)
{
	mdb_line_t 	*L = (mdb_line_t *)u;
	if (memcmp(L->signature, "MDB", 3) == 0) {
		TRACE((T_INFO, "@@LEAK at %d@mdb.c\n", L->line));
	}
}
void *
mdb_pool_realloc(void *m, int sz)
{
	return XREALLOC(m, sz, AI_MDB);
}
int
mdb_pool_size(void *m)
{
	return (m?__nc_get_len(m):0);
}
int
mdb_pool_roundup(int sz)
{
	return ((sz + 7)/8)*8;
}
int
mdb_pool_init(void *u)
{
	return SQLITE_OK;
}
void
mdb_pool_shutdown(void *u)
{
}

struct sqlite3_mem_methods __pool_allocator = {
	.xMalloc 	= mdb_pool_malloc,
	.xFree 		= mdb_pool_free,
	.xRealloc 	= mdb_pool_realloc,
	.xSize 		= mdb_pool_size,
	.xRoundup 	= mdb_pool_roundup,
	.xInit 		= mdb_pool_init,
	.xShutdown 	= mdb_pool_shutdown,
	NULL
};

static void 
mdb_rwlock_init(mdb_rwlock_t *rwlock)
{
}
/*
 * 주의 사항
 *  	- 반드시 mdb_rwlock_wrlock이 호출된 상태에서 호출되어야한다.
 *
 * 
 */
static void
mdb_rwlock_rdlock(mdb_rwlock_t *rwlock, char *f, int l)
{
}
static void
mdb_rwlock_wrlock(mdb_rwlock_t *rwlock, char *f, int l)
{
}
/*
 * unlock은 조심스럽게 고려됨
 * lock의 공유로 인해 다른 thread가 대기할 수 있으므로
 * rwlock_unlock(wrlock)은 pthread lock을 획득하지 않고 일부 처리
 */
static void
mdb_rwlock_unlock(mdb_rwlock_t *rwlock)
{
}


static void
mdb_counter(int type)
{
	_ATOMIC_ADD	(__mdb_io_counter[type], 1);
}
void
mdb_get_counter(nc_uint64_t *r, nc_uint64_t *w, nc_uint64_t *m)
{
	*r = _ATOMIC_ADD(__mdb_io_counter[MDB_IO_READ], 0);	
	*w = _ATOMIC_ADD(__mdb_io_counter[MDB_IO_WRITE], 0);	
	*m = _ATOMIC_ADD(__mdb_io_counter[MDB_IO_REMOVE], 0);	
	_ATOMIC_SUB(__mdb_io_counter[MDB_IO_READ], *r);	
	_ATOMIC_SUB(__mdb_io_counter[MDB_IO_WRITE], *w);	
	_ATOMIC_SUB(__mdb_io_counter[MDB_IO_REMOVE], *m);	
}

nc_mdb_handle_t *
mdb_open(nc_part_element_t *part, char *partition)
{
	nc_mdb_handle_t			*tdb = NULL;
	char 					*errmsg = NULL;
	pthread_mutexattr_t		mattr;
	int 					tm ;
	char 					*tmz = NULL;
	const int 				on = 1;
	nc_uint32_t 			tvolage = 0;

	int						chke = 0;
	mdb_session_t		*dbs;



	if (access(partition, R_OK|W_OK)) {
		/* error accessing the partition */
		TRACE((T_ERROR, "partition, '%s' - failed to acquire permission(R/W)\n", partition));
		
		return NULL;
	}

	tdb = (nc_mdb_handle_t *)XMALLOC(sizeof(nc_mdb_handle_t), AI_MDB);


	sprintf(tdb->dbpath, "%s/netcache.mdb", partition);
	sprintf(tdb->dbvolpath, "%s/volumes.mdb", partition);
	/*
	 * initialize pool
	 */
	INIT_LIST(&tdb->pool);
	mdb_rwlock_init(&tdb->lock);
	mdb_rwlock_init(&tdb->vollock);
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
	MUTEX_INIT(&tdb->poollock, &mattr);
	_nl_init(&tdb->batchlock, &mattr);
	MUTEX_INIT(&tdb->writerlock, &mattr);
	COND_INIT(&tdb->writersignal, NULL);
	_nl_cond_init(&tdb->donesignal);
	tdb->donewaiters = 0;
	tdb->txno.decade = tdb->txno.txno = 0;
	tdb->txno_done.decade = tdb->txno_done.txno = -1LL;
	if (!__mdb_global_init) {
#if 0
		if (sqlite3_enable_shared_cache(U_TRUE) == SQLITE_OK){
			TRACE((T_INFO, "SQLite3 - shared cache enabled OK\n"));
		}
#endif
	if (__mdb_enable_custom_alloc) {
		TRACE((T_INFO, "SQLite3 - custom allocator enabled\n"));
		MDB_CHECK_SUPPORT(sqlite3_config(SQLITE_CONFIG_MALLOC, &__pool_allocator));
	}
	else {
		TRACE((T_INFO, "SQLite3 - custom allocator disabled\n"));
	}
		MDB_CHECK_SUPPORT(sqlite3_config(SQLITE_CONFIG_MEMSTATUS, on));

#ifdef SQLITE_CONFIG_MULTITHREAD
		MDB_CHECK_SUPPORT(sqlite3_config(SQLITE_CONFIG_MULTITHREAD));
		TRACE((T_INFO, "SQLite3 - multi-threading chosen\n"));
#endif

#ifdef SQLITE_CONFIG_LOG
		MDB_CHECK_SUPPORT(sqlite3_config(SQLITE_CONFIG_LOG, MDB_report_log, NULL));
#endif


#if 0
		def_mms = 512LL*1024LL*1024LL;
		max_mms = 2*1024LL*1024LL*1024LL;
		MDB_CHECK_SUPPORT(sqlite3_config(SQLITE_CONFIG_MMAP_SIZE, def_mms, max_mms));
#endif

#if 0
		sqlmemsiz = 60000000;
		psqlmembuf = nc_aligned_malloc(sqlmemsiz, 1024*4);
		MDB_CHECK_SUPPORT(sqlite3_config(SQLITE_CONFIG_HEAP, psqlmembuf, sqlmemsiz, 8));
#endif



		sqlite3_initialize();
		tm = sqlite3_threadsafe();
		switch (tm) {
			case 0:
				tmz = "0:Single-threaded";
				break;
			case 1:
				tmz = "1:Serialized";
				break;
			case 2:
				tmz = "1:Multithreaded";
				break;
			default:
				tmz = "Unknown";
				break;
		}
		TRACE((T_INFO, "SQLite Version : %s(Thread Mode=%s)\n", (char *)sqlite3_libversion(), tmz));

		__mdb_global_init++;
	}

	dbs = tdb->writers[MDB_WRITER_NETCACHE] = mdb_create_session(tdb, tdb->dbpath);
	if (!dbs) {
		TRACE((T_ERROR, "Fatal error in creating DB session\n"));
		exit(1);
	}
	if (mdb_register_hash_function(dbs->DB) < 0) {
		TRACE((T_ERROR, "Error in registering the hash function, mdb_hash\n"));
		TRAP;
	}

	chke 	= mdb_check_exists(tdb, dbs, "objects", "cversion"); 
	if (chke == MDBE_OBJECT_ERROR) { 
		/* table  존재 안함  */
		errmsg = NULL;
		sqlite3_exec(dbs->DB, MDB_DB_SCHEMA_OBJECTS, NULL, NULL,  &errmsg);
		TRACE((T_INFO, "MDB creating objects ...got %s\n", (errmsg?errmsg:"OK")));
		if (errmsg) sqlite3_free(errmsg); 
	}
	else {
		chke = mdb_check_exists(tdb, dbs, "objects", "rsize");
		if (MDBE_COLUMN_ERROR == chke) {
			/* columne not found, old version table */
			__mdb_convert_table(tdb, dbs);
		} 

	}


#if 0
		/*
		 * 2021.2.25 by weon
		 * 넥슨에서 1700만건의 record를 가진 mdb 발견됨
		 * 저정도 크기의 MDB의 경우 pragma quick_check 실행이 10분 이상 걸림
		 * quick_check이외의 대안 필요
		 * 일단 무결성 시험을 해서 뭔가 안다고해도 할 수 있는게 없으므로 코드 막아둠
		 */
		{
			int					qc = SQLITE_OK;
			perf_val_t			s,e;
			long				d;
			PROFILE_CHECK(s);
			qc = sqlite3_exec(tdb->writers[MDB_WRITER_NETCACHE]->DB, "PRAGMA quick_check", NULL, NULL,  &errmsg);
			PROFILE_CHECK(e);
			d = PROFILE_GAP_MSEC(s, e);
			if (qc != SQLITE_OK) {
				TRACE((T_ERROR, "***** FATAL ERROR : DB path '%s' got integrity error:%s\n", tdb->dbpath, errmsg)); 
				exit(1);
			}
			else {
				TRACE((T_INFO, "MDB '%s' integrity-check OK(%.2f sec)\n", tdb->dbpath, (float)(d/1000.0)));
			}
		}
#endif

	tdb->writers[MDB_WRITER_VOLUMES] = mdb_create_session(tdb, tdb->dbvolpath);
	if (tdb->writers[MDB_WRITER_VOLUMES]) {
		/*
		 * 없었더라도 생성됨 (volumes.mdb)
		 */
		if (mdb_check_exists(tdb, tdb->writers[MDB_WRITER_VOLUMES], "volumes", "version") < 0) {
			/*
			 * table 존재 안함, 생성
			 */
			sqlite3_exec(tdb->writers[MDB_WRITER_VOLUMES]->DB, MDB_DB_SCHEMA_VOLUMES, NULL, NULL,  &errmsg);
			if (errmsg) sqlite3_free(errmsg); 
			tvolage = 1;
		}
		else {
			/*
			 * table 존재, 읽기
			 */
			tvolage = mdb_read_volume_age(tdb, MDB_VOLUMES_AGE);

		}
		if ((tvolage != VOLUME_VERSION_NULL) && (__primary_age < tvolage)) {
			/*
			 * primary partition 변경
			 */

			TRACE((T_INFO, "'%s' chosen as primary volumes candidate\n", partition));
			__primary_mdb = tdb;
			__primary_age = tvolage;
		}
		{
			int		qc = SQLITE_OK;
			qc = sqlite3_exec(tdb->writers[MDB_WRITER_VOLUMES]->DB, "PRAGMA quick_check", NULL, NULL,  &errmsg);
			if (qc != SQLITE_OK) {
				TRACE((T_ERROR, "FATAL ERROR : DB path '%s' got integrity error:%s\n", tdb->dbvolpath, errmsg)); 
				exit(1);
			}
			else {
				TRACE((T_INFO, "MDB '%s' integrity-check OK\n", tdb->dbvolpath));
			}
		}
	}

	if (__primary_mdb == NULL) {
		DEBUG_ASSERT(tdb != NULL);
		__primary_mdb = tdb;
		__primary_age = 1;
	}


	bt_init_timer(&tdb->t_checkpoint_timer, "sqlite3 volumes commit timer", U_TRUE);
	bt_set_timer(__timer_wheel_base,  &tdb->t_checkpoint_timer, 300000/* 5 mins*/, mdb_checkpoint_timer, (void *)tdb);
	tdb->part = part;
	tdb->magic = MDB_MAGIC;
	tdb->signature = XSTRDUP(partition, AI_MDB);
	tdb->refs = 0;

	tdb->batch_queue 	= tlcq_init(U_TRUE); /* waitable concurrent queue */
	pthread_create(&tdb->batch_handler, NULL, mdb_batch_upsert_handler, tdb);
	pthread_setname_np(tdb->batch_handler, "MDB");
	
	return tdb;
}

static int
__mdb_convert_table(nc_mdb_handle_t *mdb, mdb_session_t *dbs)
{
	int		dbret;
	char	*errmsg = NULL;
	char	convert_qry[] =	"INSERT INTO objects ( " \
								"volume_name, "\
								"case_id, "\
								"object_key, "\
								"object_path, "\
								"uuid, "\
								"size, "\
								"rsize, "\
								"cversion, "\
								"mode, "\
								"ctime, "\
								"mtime, "\
								"vtime, "\
								"viewcount, "\
								"viewindex, "\
								"hid, "\
								"devid ,"\
								"obits ,"
								"origincode, "\
								"packed_property, "\
								"atime)"\
							"SELECT "\
								"volume_name, "\
								"case_id, "\
								"object_key, "\
								"object_path, "\
								"uuid, "\
								"size , "\
								"size , "\
								"cversion, "\
								"mode , "\
								"ctime , "\
								"mtime , "\
								"vtime , "\
								"viewcount, "\
								"viewindex, "\
								"hid , "\
								"devid , "\
								"obits , "\
								"origincode, "\
								"packed_property, "\
								"atime "\
								"FROM objects_OLD; " ;
	TRACE((T_INFO, "Converting old meta-DB...(a few miniutes would be taken)\n"));
	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, "ALTER TABLE objects RENAME TO objects_OLD;", NULL, NULL, &errmsg)); 
	if (dbret != SQLITE_OK) {
		TRACE((T_WARN, "Creating objects_OLD failed:%s\n", errmsg));
		sqlite3_free(errmsg);
		return dbret;
	}
	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, "DROP INDEX idx_object_hid;", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, "DROP INDEX idx_object_lru;", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, "DROP INDEX idx_object_path;", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, "DROP TRIGGER insert_atime_trigger;", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, "DROP TRIGGER update_atime_trigger;", NULL, NULL, NULL)); 

	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, MDB_DB_SCHEMA_OBJECTS, NULL, NULL, &errmsg)); 
	if (dbret != SQLITE_OK) {
		TRACE((T_WARN, "Creating objects schema failed:%s\n", errmsg));
		sqlite3_free(errmsg);
		return dbret;
	}


	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, convert_qry, NULL, NULL, &errmsg)); 
	if (dbret != SQLITE_OK) {
		TRACE((T_WARN, "Migrating data failed:%s\n", errmsg));
		sqlite3_free(errmsg);
		return dbret;
	}

	MDB_CHECK_RESULT(mdb, dbret = sqlite3_exec(dbs->DB, "DROP TABLE objects_OLD;", NULL, NULL, &errmsg)); 
	if (dbret != SQLITE_OK) {
		TRACE((T_WARN, "Removing objects_OLD failed:%s\n", errmsg));
		sqlite3_free(errmsg);
		return dbret;
	}
	TRACE((T_INFO, "Converting OLD meta-DB successfully finished\n"));

	return dbret;
}

void
mdb_close(nc_mdb_handle_t *tdb)
{
	mdb_session_t 	*dbs;
	int 			n = 0;
	struct stat 	st;

	tdb->needstop = U_TRUE;
	while ( bt_del_timer_v2(__timer_wheel_base,  &tdb->t_checkpoint_timer) < 0)
		bt_msleep(1);


	for (n = 0; n < 2; n++) {
		if  (tdb->writers[n]) {
			mdb_push_session(tdb, tdb->writers[n], U_TRUE);
		}
	}
	pthread_join(tdb->batch_handler, NULL);

	dbs = mdb_pop_session(tdb, U_TRUE);
	mdb_force_checkpoint(tdb,  dbs, U_TRUE);
	TRACE((T_INFO, "%s: MDB-WAL flushed ok\n", tdb->dbpath));
	MDB_CHECK_RESULT(mdb, sqlite3_exec(dbs->DB, "PRAGMA vacuum", NULL, NULL, NULL)); 
	mdb_push_session(tdb, dbs, U_FALSE);

	while ((dbs = mdb_pop_session(tdb, U_FALSE)) != NULL) {
		mdb_push_session(tdb, dbs, U_TRUE);
		n++;
	}


	TRACE((T_INODE, "MDB[%s] - %d cached session destroyed\n", tdb->dbpath, n));
	sqlite3_shutdown();
}









int
mdb_reuse(nc_mdb_handle_t *mdb, uuid_t uuid, nc_int64_t rowid, nc_time_t vtime, nc_uint32_t nver)
{
#define	MDB_REUSE_RETRY 	10
	mdb_session_t 	*dbs;
	int 			dbret 		= 0;
	int 			r = 0;
	char			zuuid[128];


	
	dbs = mdb_pop_session(mdb, U_TRUE);

	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, dbs, IMMEDIATE_TX, dbret);

	
	r = mdb_reuse_direct(mdb, dbs, uuid, rowid, vtime, nver);

	if (r != 1) {
		/*
		 * generally 1 should be returned
		 */
		TRACE((T_INODE, "UUID[%s].ROYWID[%lld]- already purged(ignorable)\n",  uuid2string(uuid, zuuid, sizeof(zuuid)), rowid, r));
	}

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, dbs, IMMEDIATE_TX, SQLITE_OK);
	mdb_push_session(mdb, dbs,  SQLITE_OK);


	mdb_counter(MDB_IO_WRITE);


	
	return r;
}
#define	MDB_ISSUE_AND_WAIT(db_, opd_, cond_)	{ \
											struct timespec 	ts; \
											_nl_cond_init(&(opd_)->handled); \
											_nl_init(&(opd_)->lock, NULL);	\
											tlcq_enqueue((db_)->batch_queue, (opd_)); \
											_nl_lock(&(opd_)->lock, __FILE__, __LINE__); \
											while ((cond_)) { \
												make_timeout( &ts, 500, CLOCK_MONOTONIC); \
												ret = _nl_cond_timedwait(&(opd_)->handled, &(opd_)->lock, &ts); \
											} \
										}
#define	MDB_SIGNAL_OR_FREE(db_, dbs_, opd_)	if ((opd_)->ptxno) { \
											_nl_lock(&(opd_)->lock,__FILE__, __LINE__); \
											*(opd_)->ptxno = (db_)->txno; \
											(opd_)->affected = sqlite3_changes((dbs_)->DB); \
											_nl_cond_signal(&(opd_)->handled); \
											_nl_unlock(&(opd_)->lock); \
										} \
										else { \
											XFREE((opd_)); \
										}
//
// assumes that the value of txno is copied from mdb 
//
int
mdb_wait_done(nc_mdb_handle_t *mdb, mdb_tx_info_t *txno)
{

#ifdef NC_ENABLE_MDB_WAIT
	int						r;
	struct timespec			ts;
	nc_time_t				to = nc_cached_clock() + 30; /* timeout */

	_nl_lock(&mdb->batchlock, __FILE__, __LINE__);
	mdb->donewaiters++;

	while (!mdb_txno_isnull(txno) && mdb_txno_compare(txno, &mdb->txno_done) > 0) {
		make_timeout(&ts, 10, CLOCK_MONOTONIC);
		r = _nl_cond_timedwait(&mdb->donesignal, &mdb->batchlock, &ts);

		if (r == ETIMEDOUT && nc_cached_clock() > to)  {
			TRACE((T_INFO, "******** MDB Transaction No Wrapped Over *****\n"));
			break; /* would be mdb->txno_done wrapped over */
		}
	}
	mdb->donewaiters--;
	_nl_unlock(&mdb->batchlock);
#endif

	return 0;

}


/*
 * returns rowid which is persistent over record life cycle
 */
nc_int64_t
mdb_insert(nc_mdb_handle_t *mdb, fc_inode_t *inode)
{
	mdb_inserti_t		*pupi;
	int					ret;
	char				ibuf[2048];
	mdb_tx_info_t		txno;
	nc_int64_t			rowid = -1LL;


#ifndef NC_RELEASE_BUILD
	DEBUG_ASSERT_FILE(inode, inode->volume != NULL);
	DEBUG_ASSERT(VOLUME_INUSE(inode->volume));
#endif
	pupi = (mdb_inserti_t *)XMALLOC(sizeof(mdb_inserti_t), AI_ETC);

	pupi->op 		= MDBOP_INSERT;

	pupi->seq		= _ATOMIC_ADD(s__upseq, 1);

	pupi->case_id  	= 'S';
	if (!inode->volume->preservecase) {
		pupi->case_id	= 'I';
		pupi->key 	= tolowerz(inode->q_id);
		pupi->path 	= tolowerz(inode->q_path);
	}
	else {
		pupi->key 	= XSTRDUP(inode->q_id, AI_ETC);
		pupi->path 	= XSTRDUP(inode->q_path, AI_ETC);
	}

	pupi->packed 	= mdb_inode_pack_properties(inode->property, &pupi->packed_size); /* variable 필드들를 binary array로 packing */
	pupi->viewindex = MDB_NORMALIZE_VIEWINDEX(inode->ctime, inode->viewcount);				
	pupi->signature = XSTRDUP(inode->volume->signature, AI_ETC);
	memcpy(pupi->uuid, inode->uuid, sizeof(inode->uuid));
	pupi->size 		= inode->size;
	pupi->rsize 	= inode->rsize;
	pupi->cversion 	= inode->cversion;
	pupi->imode 	= inode->imode;
	pupi->ctime 	= inode->ctime;
	pupi->mtime 	= inode->mtime;
	pupi->vtime 	= inode->vtime;
	pupi->viewcount = inode->viewcount;
	pupi->viewindex = MDB_NORMALIZE_VIEWINDEX(inode->ctime, inode->viewcount);				
	pupi->hid  	 	= inode->hid;
	memcpy(pupi->devid, inode->devid, sizeof(inode->devid));
	pupi->obit 	  	= inode->ob_obit;
	pupi->origincode= inode->origincode;
	TRACE((T_INODE, "INODE[%d] - meta-info inserting;{%s}\n", inode->uid, dm_dump_lv1(ibuf, sizeof(ibuf), inode)));


	mdb_txno_null(&txno);
	pupi->ptxno		= &txno;

	MDB_ISSUE_AND_WAIT(mdb, pupi, mdb_txno_isnull(&txno)); /* last param is waiting condition */
	rowid = pupi->rowid;

	if (rowid < 0) {
		TRACE((T_ERROR, "Volume[%s].Key[%s] - INODE[%d].UUID[%s] insert failed\n",
						inode->volume->signature,
						inode->q_id,
						inode->uid,
						uuid2string(inode->uuid, ibuf, sizeof(ibuf))
			  ));

	}

	TRACE((T_INODE, "Volume[%s].Key[%s] - INODE[%d] upsert done(ROWID[%lld])\n",
					inode->volume->signature,
					inode->q_id,
					inode->uid,
					rowid));




	XFREE(pupi->packed);
	XFREE(pupi->key);
	XFREE(pupi->path);
	XFREE(pupi->signature);
	_nl_cond_destroy(&pupi->handled);
	_nl_destroy(&pupi->lock);
	XFREE(pupi);

	mdb_wait_done(mdb, &txno);

	return rowid;
}





/*
 * ir # 24803 : cversion의 추가에 의해서 select의 동작 방식 변경
 *  	기존에는 무조건 하나의 레코드만 select되었으나, 24803 패치로 인해
 * 		복수개가 select된다고 전제가 되어지며, 이때 select되는 roaw는
 * 		cversion의 값에 의해 큰 숫자가 먼저 나오도록 정렬된다
 *  	
 * 		그러므로 실제로 선택되는 row는 cversion이 가장 높은 row가 된다
 */
nc_mdb_inode_info_t *
mdb_load(nc_mdb_handle_t *mdb, int casepreserve, char *signature, char *object_key)
{
	nc_uint64_t 				hid 		= 0;
	nc_mdb_inode_info_t 		*minode 	= NULL;
	sqlite3_stmt 				*stmt	 	= NULL;
	mdb_session_t 				*dbs;
	char 						*key 		= NULL;
	int 						dbret 		= 0;
	char 						case_id		='S';
	char 						*packed	 	= NULL;
	int 						colpos 		= 0;
#ifdef __PROFILE
	perf_val_t					ms, me;
	long long					ud;
#endif
	char						bbuf[128];
	char						zuuid[64];

	


	TRACE((0, ">>>MDB_LOAD(%s) \n", object_key));
	dbs = mdb_pop_session(mdb, U_TRUE);
	//MDB_CHECK_RESULT(mdb, sqlite3_exec(dbs->DB, "PRAGMA read_uncommitted=true", NULL, NULL, NULL)); 

	key = (char *)object_key;
	if (!casepreserve)
		key = tolowerz(key);

	hid = FNV1A_Hash_Yoshimura(key, strlen(key));

L_load_retry:
	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, dbs, SHARED_TX, dbret);


	stmt	= mdb_get_cached_stmt(mdb, dbs, MDB_STMT_LOOKUP);

	PROFILE_CHECK(ms);
	dbs->query = MDB_DB_LOOKUP;


	/*
	 * 파라미터 인덱스 및 바인드 정보
	 * INSERT.#1: signature
	 * INSERT.#2: case-id : 'I', 'S'
	 * INSERT.#3: id
	 */

	/* 
	 * select에 대한 where-clause의 input 파라미터 바인딩
	 */
	case_id= casepreserve?'S':'I';
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt,  MDB_BPI(stmt, "@hid"), 		hid));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  	MDB_BPI(stmt, "@key"), 		key, 		-1, SQLITE_STATIC)); /* INSERT.#3 : object_key */
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  	MDB_BPI(stmt, "@signature"),signature, 	-1, SQLITE_STATIC)); /* INSERT.#1 : signature*/
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  	MDB_BPI(stmt, "@case_id"), 	&case_id, 	1, 	SQLITE_STATIC)); /* INSERT.#2 : case_id */

	dbret = mdb_blocking_step(stmt);
	if (dbret == SQLITE_ROW) {
		minode = (nc_mdb_inode_info_t *)XMALLOC(sizeof(nc_mdb_inode_info_t), AI_MDB);
		/* #1 : signature */
		sqlite3_column_bytes(stmt, 0);
		minode->part 				= mdb->part;
		minode->vol_signature 		= XSTRDUP((char *)sqlite3_column_text(stmt, colpos++), AI_MDB);
		minode->casesensitive 		= (char)(*sqlite3_column_text(stmt, colpos++) == 'S')?U_TRUE:U_FALSE;
		minode->q_id 				= XSTRDUP((char *)sqlite3_column_text(stmt, colpos++), AI_MDB);
		minode->q_path 				= XSTRDUP((char *)sqlite3_column_text(stmt, colpos++), AI_MDB);
		memcpy(minode->uuid, sqlite3_column_blob(stmt,  colpos++), sizeof(uuid_t)); 
		minode->size 				= sqlite3_column_int64(stmt, colpos++);
		minode->rsize 				= sqlite3_column_int64(stmt, colpos++);
		minode->cversion 			= sqlite3_column_int(stmt, colpos++);
		minode->mode 				= sqlite3_column_int(stmt, colpos++);
		minode->ctime 				= sqlite3_column_int(stmt, colpos++);
		minode->mtime 				= sqlite3_column_int(stmt, colpos++);
		minode->vtime 				= sqlite3_column_int(stmt, colpos++);
		minode->viewcount 			= (nc_uint32_t)sqlite3_column_int(stmt, colpos++);
		minode->viewindex 			= (nc_int32_t)sqlite3_column_int(stmt, colpos++);
		minode->hid 				= (nc_int64_t)sqlite3_column_int64(stmt, colpos++);
		strcpy(minode->devid, (char *)sqlite3_column_text(stmt,  colpos++)); /* null은 다음 assign으로 제거될꺼임 */
		minode->obi.op_bit_s 		= sqlite3_column_int64(stmt, colpos++);
		minode->origincode 			= sqlite3_column_int(stmt, colpos++);
		packed 						= (char *)sqlite3_column_blob(stmt, colpos++);
		minode->property 			= NULL;
		if (packed) {
			minode->property = mdb_inode_unpack_properties(packed);
		}
		minode->rowid				= sqlite3_column_int64(stmt, colpos++);
		/*
		 * ROWID
		 */

		DEBUG_ASSERT(strcmp(minode->vol_signature, signature) == 0);

		TRACE((T_INODE, "Volume[%s].CKey[%s] : found and loaded from MDB;{VT[%lld],UUID[%s],ROWID[%lld],HID[%llu],CVer[%u] %s}\n", 
						signature, 
						minode->q_id,
						(long)minode->vtime,
						uuid2string(minode->uuid, zuuid, sizeof(zuuid)),
						minode->rowid,
						minode->hid,
						minode->cversion,
						(char *)obi_dump(bbuf, sizeof(bbuf), &minode->obi)
						));
	}	


	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, dbs, SHARED_TX, SQLITE_OK);

	PROFILE_CHECK(me);
#ifdef __PROFILE
	ud = PROFILE_GAP_USEC(ms, me);
	if (ud > 500000) {
		TRACE((T_INFO, "MDB_LOAD - too slow : %.2f msec\n", (ud/1000.0)));
	}
#endif



	if (!casepreserve) XFREE(key);
	mdb_push_session(mdb, dbs,  SQLITE_OK);
	mdb_counter(MDB_IO_READ);


	TRACE((0, "<<<MDB_LOAD \n"));
	
	return minode;
}
int
mdb_invalidate(nc_mdb_handle_t *mdb, int preservecase, char *signature, char *qid)
{
	return 0;
}

int
mdb_get_lru_entries(nc_mdb_handle_t *mdb, struct purge_info *pi, int maxcount)
{
	sqlite3_stmt 			*stmt = NULL;
	mdb_session_t 			*dbs;
	int 					dbret = 0;
	int 					aidx = 0;
	int 					colpos = 0;
	nc_time_t				tnow = 0;
	char					zuuid[64];

	


	dbs = mdb_pop_session(mdb, U_TRUE);

	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, dbs, SHARED_TX, dbret);

	dbs->query = MDB_DB_GET_OLDEST;
	dbret = mdb_blocking_prepare_v2(dbs->DB, MDB_DB_GET_OLDEST, -1, &stmt, NULL);
	if (dbret != SQLITE_OK) {
		TRACE((T_ERROR, "PARTITION[%s] - error on GET_OLDEST preparation : %s\n", mdb->dbpath, sqlite3_errmsg(dbs->DB)));
	}

	tnow = nc_cached_clock();
	do {
		dbret = mdb_blocking_step(stmt);
		if (dbret == SQLITE_ROW) {
			colpos = 0;
			pi[aidx].vol_signature 	= XSTRDUP((char *)sqlite3_column_text(stmt, colpos++), AI_MDB);
			pi[aidx].key 			= XSTRDUP((char *)sqlite3_column_text(stmt, colpos++), AI_MDB);
			pi[aidx].path 			= XSTRDUP((char *)sqlite3_column_text(stmt, colpos++), AI_MDB);
			pi[aidx].obi.op_bit_s 	= sqlite3_column_int64(stmt, colpos++);
			memcpy(pi[aidx].uuid, sqlite3_column_blob(stmt,  colpos++), sizeof(uuid_t)); 
			pi[aidx].cversion 		= sqlite3_column_int(stmt, colpos++);
			pi[aidx].rowid			= sqlite3_column_int64(stmt, colpos++);


			TRACE((T_INODE, "[%d-th] Volume[%s].Key[%s]/UUID[%s] - found\n", 
							aidx,
							pi[aidx].vol_signature,
							pi[aidx].key,
							uuid2string(pi[aidx].uuid, zuuid, sizeof(zuuid))
							));
			aidx++;
		}	
	} while ((dbret == SQLITE_ROW) && (aidx < maxcount));
	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, dbs, SHARED_TX, SQLITE_OK);

	mdb_push_session(mdb, dbs, U_FALSE);
	if ((nc_cached_clock() - tnow) > 3) {
		TRACE((T_INFO, "too long to select %d rows\n", aidx));
	}

	mdb_counter(MDB_IO_READ);
	

	return aidx;
}

int
mdb_remove_rowid_direct(nc_mdb_handle_t	*mdb, uuid_t uuid, nc_int64_t rowid)
{

	int					r;
	mdb_session_t		*dbs;

	dbs = mdb_pop_session(mdb, U_TRUE);

	r = mdb_op_purge_rowid(	mdb, dbs, IMMEDIATE_TX, U_TRUE, rowid);

	mdb_push_session(mdb, dbs, U_FALSE); 
	mdb_counter(MDB_IO_REMOVE);
	return r;
}

int
mdb_list_purge_targets(	nc_volume_context_t *volume, 
						nc_mdb_handle_t *mdb, 
						char *pattern, 
						int ishard, 
						int iskey, 
						struct purge_info *pi,
						int limit)
{
	sqlite3_stmt 			*stmt = NULL;
	mdb_session_t			*dbs;

	char 					*pattern_n = NULL;
	int 					key_cnt = 0;
	int 					dbret = 0;
	char 					case_id='S';
	int 					eor = 0;
	char					zuuid[64];
	char					dquery[2048]="";

	char					*query;
	char					*errmsg = NULL;
	int						i;
	int						counter_id = 0;
	


	dbs = mdb_pop_session(mdb, U_TRUE);


	/*
	 * construct a dynamic query string
	 */
	if (iskey) 
		query = ishard?MDB_DB_GET_HARD_PURGE_KEY: MDB_DB_GET_SOFT_PURGE_KEY;
	else
		query = ishard?MDB_DB_GET_HARD_PURGE_PATH: MDB_DB_GET_SOFT_PURGE_PATH;
	sprintf(dquery, "%s LIMIT %d;", query, limit);

	counter_id = (ishard?MDB_IO_REMOVE:MDB_IO_WRITE);


	/*
	 * immediate transaction setup
	 */
	do {
		dbret = sqlite3_exec(dbs->DB, "BEGIN IMMEDIATE TRANSACTION", NULL, NULL, &errmsg); 
		if (dbret == SQLITE_BUSY) bt_msleep(10);
	} while (dbret == SQLITE_BUSY);

	TRACE((T_INODE, "VOLUME[%s] - query[%s] executing...\n", volume->signature, dquery));

	/*
	 *
	 * prepare selection
	 *
	 */

	dbret = mdb_blocking_prepare_v2(dbs->DB, dquery, -1, &stmt, NULL);

	case_id = (volume->preservecase?'S':'I');
	pattern_n = pattern;
	if (!volume->preservecase)
		pattern_n = tolowerz(pattern_n);

	sqlite3_bind_text(stmt,  1, volume->signature, -1, SQLITE_STATIC); /* INSERT.#1 : volume->signature*/
	sqlite3_bind_text(stmt,  2, &case_id, 			1, SQLITE_STATIC); /* INSERT.#2 : case_id */
	sqlite3_bind_text(stmt,  3, pattern_n, 		   -1, SQLITE_STATIC); /* INSERT.#3 : key, or origin path */


	mdb_counter(MDB_IO_READ);
	/*
	 * fetch result rows
	 */
	key_cnt = 0;
	while (!mdb->needstop && !eor) {
		/*
		 *  limit 만큼의 query 결과를 모두 추출
		 */
		dbret = mdb_blocking_step(stmt);

		if (dbret == SQLITE_ROW) {
			pi[key_cnt].key 			= XSTRDUP((char *)sqlite3_column_text(stmt, 0), AI_MDB);
			pi[key_cnt].path 			= XSTRDUP((char *)sqlite3_column_text(stmt, 1), AI_MDB);
			pi[key_cnt].obi.op_bit_s 	= sqlite3_column_int64(stmt, 2);

			memcpy(pi[key_cnt].uuid, sqlite3_column_blob(stmt,  3), sizeof(uuid_t)); 

			pi[key_cnt].cversion 		= sqlite3_column_int(stmt, 4);
			pi[key_cnt].rowid			= sqlite3_column_int64(stmt, 5);


			TRACE((T_INODE, "[%d-th] Volume[%s].%s[%s]/UUID[%s].ROWID[%lld] - found(purge_mode=[%s,%s])\n", 
							key_cnt,
							volume->signature,
							(iskey?"CKey":"CPath"),
							(iskey?pi[key_cnt].key:pi[key_cnt].path),
							uuid2string(pi[key_cnt].uuid, zuuid, sizeof(zuuid)),
							pi[key_cnt].rowid,
							(iskey?"key":"path"),
							(ishard?"hard":"soft")
							));
			key_cnt++;

		}	
		else
			eor = 1;

	} 
	
	if (stmt) {
		MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
		MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
		MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));
	}
	if (!volume->preservecase)
		XFREE(pattern_n);
	/*
	 * query 실행을 통해서 모은 객체 id정보로
	 * 실제 실행
	 */
	
	for (i = 0; i < key_cnt; i++) {
		TRACE((T_INODE, "[%03d] Volume[??].Key[%s] - UUID[%s].ROWID[%lld] purged(%s)\n",
						i,
						pi[i].key,
						uuid2string(pi[i].uuid, zuuid, sizeof(zuuid)),
						pi[i].rowid,
						(ishard?"HARD":"SOFT")
			  ));
		mdb_op_purge_rowid(	mdb, dbs, NO_TX, ishard, pi[i].rowid);
		mdb_counter(counter_id);
	}
	MDB_CHECK_RESULT(mdb, sqlite3_exec(dbs->DB, "COMMIT", NULL, NULL, &errmsg)); 
	mdb_push_session(mdb, dbs, U_FALSE); 


	return key_cnt;

}
/*
 * sql query에 limit없이 실행하는 경우, 
 * 과도한 메모리가 할당될 수 도 있고, 또는 아예 OOM kill
 * 상황이 발생할 수도 있음.
 * 따라서 실행의 방식은 최대 100개 단위로 LIMIT을 두고
 * fetch 후 실행하는 방식을 취함
 * soft-purge의 경우
 * 	- select 컨디션에 vtime != 0 추가 필요
 * hard-purge의 경우
 * 	
 */
int
mdb_purge(	nc_volume_context_t *volume, 
			nc_mdb_handle_t *mdb, 
			char *pattern, 
			int ishard, 
			int iskey, 
			int (*purge_cache_object)(nc_volume_context_t *vol, char *path, char *key, uuid_t uuid, nc_int64_t rowid, int ishard, int istempl, nc_path_lock_t *, nc_part_element_t *, void *), 
			void *purge_cache_ctx,
			int limitcount)
{
	//sqlite3_stmt 			*stmt = NULL;
	//sqlite3					*dbs;

	struct purge_info 		*pi = NULL;
	//char 					*pattern_n = NULL;
	int 					key_cnt = 0;
	//int 					dbret = 0;
	int 					i;
	//char 					case_id='S';
	int 					purged = 0;
	//int 					eor = 0;
	//char					zuuid[64];

	/* NEW 2018-10-17 각 작업별 수행시간 측정을 위한 기능 추가 (#32386) */
	long long mdb_elapsed_microsec = 0;	// MDB 조회 처리 total 시간		
	long microsec_perform = 0;
#ifdef __PROFILE
	perf_val_t time_start, time_end;
#endif
	//char					*query;
	//char					*errmsg = NULL;

	//
	// stack allocation to avoid free
	pi = (struct purge_info *)alloca(limitcount * sizeof(struct purge_info));;

	

		/*
		 * query 선택
		 */

	TRACE((T_INODE, "Volume[%s]:  pattern[%s] ishard[%d], iskey[%d] begin\n", 
						volume->signature, 
						pattern, 
						ishard, 
						iskey
						));
#ifdef __PROFILE
	PROFILE_CHECK(time_start);
#endif 


	do {
		key_cnt = mdb_list_purge_targets(	volume, 
											mdb, 
											pattern, 
											ishard, 
											iskey, 
											pi,
											limitcount);
		TRACE((T_INODE, "Volume[%s]:  pattern[%s] ishard[%d], iskey[%d] - %d candidates found\n", 
						volume->signature, 
						pattern, 
						ishard, 
						iskey,
						key_cnt
						));

		if (key_cnt > 0) {
			purged += mdb_purge_with_key(volume, mdb, pi, key_cnt, ishard, iskey, purge_cache_object, purge_cache_ctx);
			for (i = 0; i < key_cnt; i++) {
				XFREE(pi[i].key);
				XFREE(pi[i].path);
			}
		}
	} while (key_cnt == limitcount);


#if 1
		
	PROFILE_CHECK(time_end);

	microsec_perform = PROFILE_GAP_USEC(time_start, time_end);

	/* NEW 2018-10-17 각 작업별 수행시간 측정을 위한 기능 추가 (#32386) */
	TRACE((0, "* mdb purge report: %s, %s, %s, %s, %d purged, elapsed time= %ld msec\n",
		volume->signature,
		(ishard ? "hard" : "soft"),
		pattern,
		(iskey ? "key" : "path"),
		purged,
		(long)(mdb_elapsed_microsec / 1000.0)
		));

	return purged;
#endif
}
static int
mdb_check_error(nc_mdb_handle_t *mdb, int ret)
{
	int 	r = (ret?MDBE_ERROR:MDBE_OK);
	return r;
}


static int 
mdb_busy_handler(void *pArg, int nbusy)
{
	mdb_session_t *s = (mdb_session_t *)pArg;

	TRACE((T_INFO, "QUERY[%s] - busy signalled %d time(s), retry again after %d msec sleep\n", 
						s->query, 
						nbusy,
						s->sleepms
						));
	sqlite3_sleep(s->sleepms);
	return s->maxretry - nbusy;
}
/*
 * DB session pool management
 */

static mdb_session_t *
mdb_create_session(nc_mdb_handle_t *mdb, char * dbname)
{
	int 			ret;
	mdb_session_t 	*ns = NULL;
	sqlite3 		*conn = NULL;
	char 			zpragma[1024];

	
	
	pthread_mutex_lock(&mdb->poollock); 
	ret = sqlite3_open_v2(dbname, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX , NULL);
	if (mdb_check_error(mdb, ret) == MDBE_OK) {
		ns = (mdb_session_t *)XMALLOC(sizeof(mdb_session_t), AI_MDB);
		ns->DB = conn;
	}
	else {
		TRACE((T_WARN, "ERROR in sqlite3_open\n"));
		TRAP;
	}

	sqlite3_busy_handler(ns->DB, mdb_busy_handler, (void *)ns);
	sqlite3_busy_timeout(ns->DB, 500);



	sprintf(zpragma, "PRAGMA cache_size=-%d", MDB_CACHE_SIZE); /* in unit of KB */
	MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, zpragma, NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA foreign_keys=OFF", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA page_size=4096", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA synchronous=OFF", NULL, NULL, NULL)); 
	/*
	 * 2016.10.10 update
	 * journal mode update from "MEMORY" to "WAL" for disk-io efficiency
	 */
	MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA journal_mode=WAL", NULL, NULL, NULL)); 
	//MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA journal_mode=OFF", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL)); 
	//MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA mmap_size=536870912", NULL, NULL, NULL)); 

#if 0
	MDB_CHECK_RESULT(mdb, sqlite3_exec(ns->DB, "PRAGMA locking_mode=EXCLUSIVE", NULL, NULL, NULL)); 
	MDB_CHECK_RESULT(mdb, sqlite3_extended_result_codes(ns->DB, U_TRUE);
#endif

	ns->handled = 0;
	ns->sleepms = MDB_SLEEP_MS; /* 10 ms */
	ns->maxretry= MDB_MAX_RETRY; /*max 5 secs */
	mdb->session_count++;
	pthread_mutex_unlock(&mdb->poollock); 
	return ns;
}
static mdb_session_t *
mdb_acquire_writer(nc_mdb_handle_t *mdb, int wt)
{
	mdb_session_t 	*s = NULL;

#if 1
	if (wt == MDB_WRITER_NETCACHE) {
		s = mdb_pop_session(mdb, U_TRUE);
	}
	else {
		pthread_mutex_lock(&mdb->writerlock); 

		while ((s = mdb->writers[wt]) == NULL) {
      		pthread_cond_wait(&mdb->writersignal, &mdb->writerlock);
		}
		mdb->writers[wt] = NULL;
		s->refs++;
		pthread_mutex_unlock(&mdb->writerlock);
	}
#else
	pthread_mutex_lock(&mdb->writerlock); 

	while ((s = mdb->writers[wt]) == NULL) {
   		pthread_cond_wait(&mdb->writersignal, &mdb->writerlock);
	}
	mdb->writers[wt] = NULL;
	s->refs++;
	pthread_mutex_unlock(&mdb->writerlock);
#endif
	return s;
}
static void
mdb_release_writer(nc_mdb_handle_t *mdb, mdb_session_t *s, int wt)
{

#if 1
	if (wt == MDB_WRITER_NETCACHE) {
		mdb_push_session(mdb, s, U_FALSE); 
	}
	else {
		pthread_mutex_lock(&mdb->writerlock);
		s->refs--; 
		mdb->writers[wt] = s;
    	pthread_cond_signal(&mdb->writersignal);
		pthread_mutex_unlock(&mdb->writerlock); 
	}
#else
	pthread_mutex_lock(&mdb->writerlock);
	s->refs--; 
	mdb->writers[wt] = s;
   	pthread_cond_signal(&mdb->writersignal);
	pthread_mutex_unlock(&mdb->writerlock);
#endif
}
static mdb_session_t *
mdb_pop_session(nc_mdb_handle_t *mdb, int needcreate)
{
	mdb_session_t 	*ns = NULL;

	
	
	pthread_mutex_lock(&mdb->poollock);

	ns = (mdb_session_t *)link_get_head(&mdb->pool);

	if (!ns && needcreate) {
		ns = mdb_create_session(mdb, mdb->dbpath);
	}
	if (ns) {
		ns->refs++;
		ns->query = NULL;
		ns->state = 0;
		ns->handled++; 
	}
	pthread_mutex_unlock(&mdb->poollock);
	return ns;
}
static void
mdb_push_session(nc_mdb_handle_t *mdb, mdb_session_t *dbs, int needclose)
{
	DEBUG_ASSERT(dbs->state == 0);
	if (needclose) {
		sqlite3_close(dbs->DB);
		XFREE(dbs);
	}
	else {
		dbs->refs--; 
#if 0
		if ((dbs->handled % 100000) == 0) {
			/*
			 *
			 * 아래 함수 실행은 DB locked 에러가 리턴될 수 있으나 무시함
			 * best effort로 실행되고 반복실행 중에 성공되기도 함
			 */
			sqlite3_exec(dbs->DB, "PRAGMA optimize", NULL, NULL, NULL);
		}
#endif
		dbs->query  = NULL;
		RESET_NODE(&dbs->node);
		pthread_mutex_lock(&mdb->poollock);
		link_append(&mdb->pool, dbs, &dbs->node);
		pthread_mutex_unlock(&mdb->poollock); 
	}
}

static int 
MDB_pack_property(char *key, char *val, void *cb)
{
	struct 			property_pack_info *ppi = (struct property_pack_info *)cb;
	char 			zproperty[4096];
	int 			len, slen;
	char 			*old 	= NULL;
	//nc_off_t		used;

	slen = sprintf(zproperty, "%s:%s", key, val);
	if (slen > 1) {
		len = vstring_size2(slen+1);

		if ((ppi->allocptr + ppi->allocsiz) < (ppi->vfree + len)) {
			ppi->allocsiz = ppi->allocsiz + max(len, 256);
			ppi->allocptr = XREALLOC(ppi->allocptr, ppi->allocsiz, AI_MDB);
			ppi->vfree 	  = ppi->allocptr + ppi->packed; /* vfree를 realloc된 base부터 재설정 */
		}
		old 		= ppi->vfree;
		XVERIFY(ppi->allocptr);
		ppi->vfree 	= vstring_pack(ppi->vfree, (ppi->allocptr + ppi->allocsiz - ppi->vfree), zproperty, slen+1); /* len은 NULL을 뺸 길이임*/
		ppi->packed+= ((long long)ppi->vfree - (long long)old);
		ppi->cnt++;
	}
	return 0;
}

#if NOT_USED
/*
 * property가 저장될 때 크기를 추정하기위한 함수.
 * 정확한 길이 보다 넉넉한 크기를 산출하는 용도
 */
static int
MDB_size_property(char *key, char *val, void *cb)
{
	nc_size_t 	*sz = cb;

	(*sz) += sizeof(nc_uint32_t) + NC_ALIGNED_SIZE((strlen(key) + strlen(val) + 2/*null + ':'*/), sizeof(nc_uint32_t));
	return 0;
}
#endif


static char *
mdb_inode_pack_properties(nc_xtra_options_t *property, size_t *packed_size)
{
	//char						*packed_buffer = NULL;
	struct property_pack_info  	ppi;
	nc_size_t 					psz = 0;


#if 0
	size_t 						alloc_size = 64;
	memset(&ppi, 0, sizeof(ppi));
	if (property) {
		kv_for_each(property, MDB_size_property, &psz);
		alloc_size += psz;
	}
	packed_buffer = ppi.vfree 	= (char *)XMALLOC(alloc_size, AI_MDB);
#else

	ppi.allocsiz  	= 256;
	ppi.allocptr 	= ppi.vfree = (char *)XMALLOC(ppi.allocsiz, AI_MDB);
#endif
	if (property) {
		ppi.property 	= property;
		ppi.cnt 		= 0;
		ppi.packed 		= 0;
		kv_for_each(property, MDB_pack_property, &ppi);

	}
	DEBUG_ASSERT((char *)ppi.allocptr + ppi.allocsiz >= ppi.vfree);

	psz = vstring_size2(1);
	if ((ppi.allocptr + ppi.allocsiz) < (ppi.vfree + psz)) {
		ppi.allocsiz = ppi.allocsiz + max(psz, 256);
		ppi.allocptr = XREALLOC(ppi.allocptr, ppi.allocsiz, AI_MDB);
		ppi.vfree 	  = ppi.allocptr + ppi.packed; /* vfree를 realloc된 base부터 재설정 */
	}
	ppi.vfree 	= vstring_pack(ppi.vfree, (ppi.allocptr + ppi.allocsiz - ppi.vfree), "", 1);/* end of last property*/

	DEBUG_ASSERT((char *)ppi.allocptr + ppi.allocsiz >= ppi.vfree);
	*packed_size = (size_t )((char *)ppi.vfree - (char *)ppi.allocptr);

	return ppi.allocptr;
}
static nc_xtra_options_t *
mdb_inode_unpack_properties(char *packed_array)
{
	nc_xtra_options_t 	*properties = NULL;
	char 				*zproperty = NULL;
	char 				*sep = NULL;
	int 				zproperty_len;
	int 				eop = 0;


	do  {
		packed_array = vstring_unpack(packed_array, &zproperty, &zproperty_len);
		if (*zproperty == '\0') {
			eop++;
			XFREE(zproperty);
			break;
		}
		if (!properties) 
			properties = kv_create_d(__FILE__, __LINE__);
		sep = strchr(zproperty, ':');
		*sep = '\0';
		kv_add_val(properties, zproperty, sep+1);
		XFREE(zproperty);
	} while (!eop);
	return properties;
}
int
mdb_check_valid( nc_mdb_handle_t *mdb)
{
	return mdb->magic == MDB_MAGIC;
}
static void
mdb_checkpoint_timer(void *u)
{
	nc_mdb_handle_t *mdb = (nc_mdb_handle_t *)u;
	mdb_session_t 	*dbs 	= NULL;
	int				wt[2] = {
						MDB_WRITER_NETCACHE,
						MDB_WRITER_VOLUMES
					};
	int				i;

	
	for (i = 0; i < 2; i++) {
		dbs = mdb_acquire_writer(mdb, wt[i]);
		if (dbs) {
			mdb_force_checkpoint(mdb,  dbs, U_TRUE);
		}
		mdb_release_writer(mdb, dbs, wt[i]);
	}
	bt_set_timer(__timer_wheel_base,  &mdb->t_checkpoint_timer, 300000/* 5 mins*/, mdb_checkpoint_timer, (void *)mdb);
}
int
mdb_purge_rowid(nc_mdb_handle_t 	*mdb,
				 int				ishard,
				 int				iskey,
				 uuid_t				uuid,
				 nc_int64_t			rowid, 
				 mdb_tx_info_t		*txno
				 )
{

#ifdef MDB_ENABLE_SINGLE_WRITER
	mdb_rrow_t		*prrow;
	struct timespec	ts;
	int				af = 0;

	prrow 			= (mdb_rrow_t *)XMALLOC(sizeof(mdb_rrow_t), AI_ETC);
	prrow->op 		= MDBOP_PURGE_ROWID;
	memcpy(prrow->uuid,	uuid, sizeof(uuid_t));
	prrow->ishard	= ishard;
	prrow->rowid	= rowid;
	prrow->ptxno	= txno;

	if (txno) {
		mdb_txno_null(txno);
		_nl_cond_init(&prrow->handled);
		_nl_init(&prrow->lock, NULL);
	}

	tlcq_enqueue(mdb->batch_queue, prrow);


	if (txno) {
		_nl_lock(&prrow->lock, __FILE__, __LINE__);
		while (mdb_txno_isnull(txno)) {
			make_timeout( &ts, 500, CLOCK_MONOTONIC);
			_nl_cond_timedwait(&prrow->handled, &prrow->lock, &ts);
		}
		_nl_unlock(&prrow->lock);

		if (prrow->ptxno) {
			_nl_cond_destroy(&prrow->handled);
			_nl_destroy(&prrow->lock);
		}
		af = prrow->affected;
		XFREE(prrow);
	}

	return af;
#else
	int			r;
	mdb_session_t		*dbs;

	dbs = mdb_pop_session(mdb, U_TRUE);

	r = mdb_op_purge_rowid(mdb, dbs, IMMEDIATE_TX, ishard, rowid);, 

	mdb_push_session(mdb, dbs, U_FALSE); 
	mdb_counter(MDB_IO_REMOVE);
	return r;
#endif
}
int
mdb_update_rowid(nc_mdb_handle_t	*mdb,
				 uuid_t				uuid,
				 nc_int64_t			rowid,
				 nc_size_t			size,
				 nc_size_t			rsize,
				 nc_time_t			vtime,
				 nc_mode_t			mode,
				 nc_uint64_t		obi,
				 mdb_tx_info_t		*txno
				 )
{

#ifdef MDB_ENABLE_SINGLE_WRITER
	mdb_updrow_t	*prrow;
	struct timespec	ts;
	int				af = 0;

	prrow 			= (mdb_updrow_t *)XMALLOC(sizeof(mdb_updrow_t), AI_ETC);
	prrow->op 		= MDBOP_UPDATE_ROWID;
	memcpy(prrow->uuid,	uuid, sizeof(uuid_t));
	prrow->size		= size;	
	prrow->rsize	= rsize; /* realsize */
	prrow->vtime	= vtime;
	prrow->obi		= obi;
	prrow->mode		= mode;
	prrow->rowid	= rowid;

	prrow->ptxno	= txno;

	if (txno) {
		mdb_txno_null(txno);
		_nl_cond_init(&prrow->handled);
		_nl_init(&prrow->lock, NULL);
	}

	tlcq_enqueue(mdb->batch_queue, prrow);


	if (txno) {
		_nl_lock(&prrow->lock, __FILE__, __LINE__);
		while (mdb_txno_isnull(txno)) {
			make_timeout( &ts, 500, CLOCK_MONOTONIC);
			_nl_cond_timedwait(&prrow->handled, &prrow->lock, &ts);
		}
		_nl_unlock(&prrow->lock);

		if (prrow->ptxno) {
			_nl_cond_destroy(&prrow->handled);
			_nl_destroy(&prrow->lock);
		}
		af = prrow->affected;
		XFREE(prrow);
	}

	return af;
#else
	int		r;

	mdb_session_t		*dbs;

	dbs = mdb_pop_session(mdb, U_TRUE);
	r = mdb_op_update_rowid(mdb, dbs, U_TRUE, uuid, rowid, size, rsize, vtime, mode, obi);
	mdb_push_session(mdb, dbs, U_FALSE); 
	mdb_counter(MDB_IO_WRITE);
	return r;

#endif

}

/*
 * called through pm_purge()
 */

static int
mdb_purge_with_key(	nc_volume_context_t *	volume, 
					nc_mdb_handle_t *		mdb, 
					struct purge_info 		pi[], 
					int 					key_cnt, 
					int 					ishard, 
					int 					iskey,
					int (*purge_cache_callback)(nc_volume_context_t *vol, char *path, char *key, uuid_t uuid, nc_int64_t rowid,  int ishard, int istempl, nc_path_lock_t *, nc_part_element_t *, void *), 
					void *					ctx
					)
{
	int 			i;
	int				retry = 0;
	int 			done = 0; //, dc = 0;
	//char 			case_id;
	char			zuuid[64];
	int				r;
	nc_path_lock_t	*pl = NULL;
#ifdef __PROFILE
	perf_val_t			ms, me;
	long				ud;
#endif
	perf_val_t			ts, te;
	long				wd;

	mdb_tx_info_t		txno;

	mdb_txno_null(&txno);

	PROFILE_CHECK(ms);

	for (i = 0; i < key_cnt;i++) {

		pl = nvm_path_lock_ref(volume, pi[i].path, __FILE__, __LINE__); 
		nvm_path_lock(volume, pl, __FILE__, __LINE__);
		retry 	= 0;
		PROFILE_CHECK(ts);
		while (nvm_path_busy_nolock(pl))  {
			/*
			 * work on the path, pi[i].path in progress
			 * need to wait
			 */
			nvm_path_unlock(volume, pl, __FILE__, __LINE__);
			retry++;
			bt_msleep(10);

			nvm_path_lock(volume, pl, __FILE__, __LINE__);
		}

		PROFILE_CHECK(te);
		wd = PROFILE_GAP_MSEC(ts, te);
		if (wd  > 500) {
			TRACE((T_INFO, "Volume[%s].Key[%s] - purged after %.2f sec(s) which is too long time in opening phase\n",  volume->signature,  pi[i].key, (wd/1000.0)));
		}


		r = (*purge_cache_callback)(volume, pi[i].path, pi[i].key, pi[i].uuid, pi[i].rowid, ishard, pi[i].ob_template != 0, pl, mdb->part, ctx);
		if (r < 0) {
			/*
			 * 삭제 실패, 이미 다른 operation 진행 중
			 */
			TRACE((T_INFO|T_INODE, "Volume[%s].CKey[%s].PATH{%s] - purge_cache_object(UUID[%s].ROWID[%lld]) returned %d(would be already purge in progress)\n", 
							volume->signature,
							pi[i].key,
							pi[i].path,
							uuid2string(pi[i].uuid, zuuid, sizeof(zuuid)),
							pi[i].rowid,
							r
							));
		}
		else {
			/*	
			 * 캐시 삭제 성공
			 */
			done++;
		}

		nvm_path_unlock(volume, pl, __FILE__, __LINE__);
		nvm_path_lock_unref(volume, pl, __FILE__, __LINE__);

	}

	//if (!mdb_txno_isnull(&txno)) {
	//	mdb_wait_done(mdb, &txno);
	//}



	PROFILE_CHECK(me);
#ifdef __PROFILE
	ud = PROFILE_GAP_MSEC(ms, me);
#endif
	return done;
}
int
mdb_check_status(char *buf, int len)
{
	int 		mem_current, mem_highwater;
	int 		pc_current, pc_highwater;
	int 		sc_current, sc_highwater;
	int 		n;
	sqlite3_status(SQLITE_STATUS_MEMORY_USED , &mem_current, &mem_highwater, U_TRUE);
	sqlite3_status(SQLITE_STATUS_PAGECACHE_USED , &pc_current, &pc_highwater, U_TRUE);
	sqlite3_status(SQLITE_STATUS_SCRATCH_USED , &sc_current, &sc_highwater, U_TRUE);
	n = snprintf(buf, len, "SQLite3 Memory Usage: malloc=%.2f MB, scratch=%.2f MB, page cache=%.2f MB\n",
			(1.0*mem_current/1000000.0),
			(1.0*pc_current/1000000.0),
			(1.0*sc_current/1000000.0));

	*(buf + n) = '\0';
	if (mem_current > 100000000) {
		sqlite3_release_memory(mem_current - 100000000);
		TRACE((T_INFO, "SQLite3 : releasing some memory...\n"));
	}
	return n;
}
static void 
MDB_report_log(void *not_used, int resultcode, char * msg)
{
	TRACE((T_INFO, "SQLite Alert - SQLITE3 result code %d : %s\n", resultcode, msg));
}

/*
 * MDB에 저장된 volumes 테이블에서
 * 특수 레코드(MDB_VOLUMES_AGE)의 존재 확인 및 데이타 버전 정보 읽기
 */
int
mdb_read_volumes_version(char *volume)
{
	nc_uint32_t	dbage = VOLUME_VERSION_NULL;

	if (__primary_mdb) {
		DEBUG_ASSERT(__primary_mdb != NULL);
		dbage = mdb_read_volume_age(__primary_mdb, volume);
	}
	return dbage;
}

#if NOT_USED
static int
mdb_check_object_exist(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *objecttype, char *objectname)
{
	char 			tbl_qry[1024];
	int 			dbret;
	sqlite3_stmt 	*stmt = NULL;
	int 			exist = MDBE_OK;
	/*
 	 * sqlite3 version
	 * sqlite> SELECT count(*) FROM sqlite_master WHERE type='table' and name='%s';
	 */
	sprintf(tbl_qry, "SELECT name FROM sqlite_master WHERE type='%s' and name='%s'; ", objecttype, objectname);
	if (dbs) {
		dbret = mdb_blocking_prepare_v2(dbs->DB, tbl_qry, -1, &stmt, NULL);
		dbret = mdb_blocking_step(stmt);
		exist = (dbret == SQLITE_ROW);
		if (!exist) {
			/* table이 존재하지 않음 */
			exist = MDBE_OBJECT_ERROR;
		}
		else 
			exist = MDBE_OK;
		MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
		MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));
		mdb_rwlock_unlock(&mdb->lock);
	}
	return exist;
}
#endif
/*
 * table : table 존재 여부 체크
 * column : table 내에 해당 column 존재 체크
 */
static int
mdb_check_exists(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *table, char *column)
{
	char 			tbl_qry[1024];
	int 			dbret;
	sqlite3_stmt 	*stmt = NULL;
	int 			exist = MDBE_OK;
#if 0
	/*
	 * table 존재 체크를 INFORMATION_SCHEMA를 이용해서 확인 (ANSI 표준)
	 */
	sprintf(tbl_qry, "SELECT table_name FROM INFORMATION_SCHEMA.TABLES WHERE  table_name='%s'", table);
#else
	/*
 	 * sqlite3 version
	 * sqlite> SELECT count(*) FROM sqlite_master WHERE type='table' and name='%s';
	 */
	sprintf(tbl_qry, "SELECT name FROM sqlite_master WHERE type='table' and name='%s'; ", table);
#endif
	if (dbs) {
		dbret = mdb_blocking_prepare_v2(dbs->DB, tbl_qry, -1, &stmt, NULL);
		dbret = mdb_blocking_step(stmt);
		exist = (dbret == SQLITE_ROW);
		if (!exist) {
			/* table이 존재하지 않음 */
			exist = MDBE_OBJECT_ERROR;
			goto L_check_finalize;
		}
		else
			exist = MDBE_OK;
		MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
		MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));
		/*
		 * table이 존재하는 것을 information_schema를 통해서 확인
		 */
		exist = MDBE_COLUMN_ERROR;
		sprintf(tbl_qry, "pragma table_info(%s)", table);
		dbret = mdb_blocking_prepare_v2(dbs->DB, tbl_qry, -1, &stmt, NULL);
		dbret = mdb_blocking_step(stmt);
		if (dbret == SQLITE_ROW) {
			int 	ismatch = 0;
			char 	*colval  = NULL;
		 	while (!ismatch && (dbret == SQLITE_ROW)) {
				colval 	= (char *)sqlite3_column_text(stmt, 1); /* 0번째  컬럼은 컬럼 인덱스임 */
				ismatch = (strcasecmp(colval, column) == 0); 
				dbret = mdb_blocking_step(stmt);
			}
			if (ismatch) exist = MDBE_OK;
		}
	
L_check_finalize:
		MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
		MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));
	}
	return exist;
}
static int
mdb_check_exist_object(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *type, char *object)
{
	char 			tbl_qry[1024];
	int 			dbret;
	sqlite3_stmt 	*stmt = NULL;
	int 			exist = U_FALSE;
	/*
 	 * sqlite3 version
	 * sqlite> SELECT count(*) FROM sqlite_master WHERE type='table' and name='%s';
	 */
	sprintf(tbl_qry, "SELECT name FROM sqlite_master WHERE type='%s' and name='%s'; ", type, object);

	if (dbs) {
		dbret = mdb_blocking_prepare_v2(dbs->DB, tbl_qry, -1, &stmt, NULL);
		dbret = mdb_blocking_step(stmt);
		exist = (dbret == SQLITE_ROW);
		MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
		MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));
	}
	return exist;
}

/*
 * load caching version from the mdb, MDB.
 */
nc_uint32_t
mdb_read_volume_age(nc_mdb_handle_t *mdb, char *volume)
{
	mdb_session_t 	*dbs = NULL;
	nc_uint32_t 	version = 0;
	int				dbret;


	/*
 	 * exclusive -> shared lock downgrade
	 */
	dbs = mdb_acquire_writer(mdb, MDB_WRITER_VOLUMES);
	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, dbs, SHARED_TX, dbret);

	version = mdb_read_version_internal__(mdb, dbs, volume);

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, dbs, SHARED_TX, SQLITE_OK);
	mdb_release_writer(mdb, dbs, MDB_WRITER_VOLUMES);
	mdb_counter(MDB_IO_READ);

	return version;
}
/*
 * __VOLUME_AGE__레코드의 생성 또는 값의 변경 수행
 * remark : no-transaction!, 상위 operation의 transaction에 속함
 */
static int
mdb_upgrade_version_internal__(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *volume, nc_uint32_t version)
{
	int 			dbret;
	sqlite3_stmt 	*stmt = NULL;
	int 			r = 0;


	dbret = mdb_blocking_prepare_v2(dbs->DB, "INSERT INTO volumes values(?, ?); ", -1, &stmt, NULL);
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  1, volume, -1, SQLITE_STATIC)); 
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,  2, version));
	dbret = mdb_blocking_step(stmt);
	if (dbret != SQLITE_DONE) {
		/*
		 * insert 실패 - 이미 존재하니까 update해야함
		 */
		r = -1;

		if (dbret == SQLITE_CONSTRAINT) {
			r = 0;
			MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
			MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));
			if (version == VOLUME_VERSION_NULL) {
				dbret = mdb_blocking_prepare_v2(dbs->DB, "UPDATE volumes SET version=version+1 WHERE volume_name=?; ", -1, &stmt, NULL);
				MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  1, volume, -1, SQLITE_STATIC)); 
			}
			else {
				
				dbret = mdb_blocking_prepare_v2(dbs->DB, "UPDATE volumes SET version=? WHERE volume_name=?; ", -1, &stmt, NULL);
				
				MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,  1, version));
				MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  2, volume, -1, SQLITE_STATIC)); 
			}
			dbret = mdb_blocking_step(stmt);
			r =  (dbret == SQLITE_DONE)?0:-1;
		}
		/* update 해당하는 부분에 대한 binding */

	}
	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));
	return r;
}
int
mdb_get_primary(char *buf)
{
	int 	found = -1;
	if (__primary_mdb) {
		found++;
		strcpy(buf, __primary_mdb->signature);
	}
	return found;
}
static nc_uint32_t
mdb_read_version_internal__(nc_mdb_handle_t *mdb, mdb_session_t *dbs, char *vname)
{
	int 			dbret;
	sqlite3_stmt 	*stmt = NULL;
	nc_uint32_t 	version = VOLUME_VERSION_NULL; /* insert 시점에 1로 증가 */
	char 			volume_name[1024] = MDB_VOLUMES_AGE;
	
	if (vname) {
		strncpy(volume_name, vname, sizeof(volume_name)-1);
	}

	
	dbret = mdb_blocking_prepare_v2(dbs->DB, "SELECT version FROM volumes WHERE volume_name=?; ", -1, &stmt, NULL);
	
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  1, volume_name, -1, SQLITE_STATIC)); 
	dbret = mdb_blocking_step(stmt);
	if ((dbret != SQLITE_DONE) && (dbret != SQLITE_ROW)) {
		TRACE((T_ERROR, "PARTITION[%s] - SQLite3 got error on reading version : %s\n", mdb->dbpath, sqlite3_errmsg(dbs->DB)));
	}
	else {
		if (dbret == SQLITE_ROW) {
			version = sqlite3_column_int(stmt, 0);
		}
	}
	MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_finalize(stmt));

	return version;
}


/*
 * 볼륨 퍼지 요청 시 해당 volume의 age(version)만 1증가
 * 기존의 objects table에 존재하는 해당 volume에 속한 객체들의
 * 에대한 operation은 아무것도 안함
 *
 */
int
mdb_purge_volume(char *volume, nc_uint32_t version)
{
	int 			wt = MDB_WRITER_VOLUMES;
	mdb_session_t 	*dbs = NULL;
	int 			r = 0;
	nc_mdb_handle_t	*mdb = __primary_mdb;
	nc_uint32_t 	ver = VOLUME_VERSION_NULL;


	/*
	 * ir #26340 : memory-only인 경우 버그 수정
	 */
	if (mdb == NULL) return 0;


	dbs = mdb_acquire_writer(mdb, wt);
	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, dbs, IMMEDIATE_TX, r);

	r = mdb_upgrade_version_internal__(mdb, dbs, volume, version);
	if (r >= 0) {
		/*  
		 * 아무튼 version 정보 변경 성공 
		 * volumes table 자체 버전 변경 (__VOLUMES_AGE__)
		 */
		__primary_age++;
		r = mdb_upgrade_version_internal__(mdb, dbs, MDB_VOLUMES_AGE, __primary_age);

	}
	else {
		TRACE((T_INFO, "volume '%s' caching version update failed\n", volume));
	}
	if (r >= 0)
		ver = mdb_read_version_internal__(mdb, dbs, volume);
	

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, dbs, IMMEDIATE_TX, SQLITE_OK);
	mdb_release_writer(mdb, dbs, wt);
	mdb_counter(MDB_IO_WRITE);

	return r;
}


/*
 * shared cache mode에서 db locked 상황을 처리하기위해서
 * 추가되는 루틴들
 */

typedef struct UnlockNotification UnlockNotification;
struct UnlockNotification {
  int fired;                         /* True after unlock event has occurred */
  pthread_cond_t cond;               /* Condition variable to wait on */
  pthread_mutex_t mutex;             /* Mutex to protect structure */
  struct timespec ts;
};
static void 
mdb_unlock_notify_cb(void **apArg, int nArg)
{
  int i;
  for(i=0; i<nArg; i++){
    UnlockNotification *p = (UnlockNotification *)apArg[i];
    pthread_mutex_lock(&p->mutex);
    p->fired = 1;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
  }
}

static int 
mdb_wait_for_unlock_notify(sqlite3 *db)
{
  int rc;
  UnlockNotification un;

  /* Initialize the UnlockNotification structure. */
  un.fired = 0;
  pthread_mutex_init(&un.mutex, 0);
  pthread_cond_init(&un.cond, 0);

  make_timeout(&un.ts, 5000, CLOCK_MONOTONIC);

  /* Register for an unlock-notify callback. */
  rc = sqlite3_unlock_notify(db, mdb_unlock_notify_cb, (void *)&un);
  DEBUG_ASSERT( rc==SQLITE_LOCKED || rc==SQLITE_OK );

  /* The call to sqlite3_unlock_notify() always returns either SQLITE_LOCKED 
  ** or SQLITE_OK. 
  **
  ** If SQLITE_LOCKED was returned, then the system is deadlocked. In this
  ** case this function needs to return SQLITE_LOCKED to the caller so 
  ** that the current transaction can be rolled back. Otherwise, block
  ** until the unlock-notify callback is invoked, then return SQLITE_OK.
  */
  if( rc== SQLITE_OK ){
    pthread_mutex_lock(&un.mutex);
    if( !un.fired ){
      pthread_cond_wait(&un.cond, &un.mutex);
    }
    pthread_mutex_unlock(&un.mutex);
  }
  else {
	TRACE((T_ERROR, "DEADLOCK or TIMEOUT?\n"));
  }

  /* Destroy the mutex and condition variables. */
  pthread_cond_destroy(&un.cond);
  pthread_mutex_destroy(&un.mutex);

  return rc;
}
static int 
mdb_blocking_prepare_v2(
  sqlite3 *db,              /* Database handle. */
  char *zSql,         /* UTF-8 encoded SQL statement. */
  int nSql,                 /* Length of zSql in bytes. */
  sqlite3_stmt **ppStmt,    /* OUT: A pointer to the prepared statement */
  const char **pz           /* OUT: End of parsed string */
){
  int rc;
  while( SQLITE_LOCKED==(rc = sqlite3_prepare_v2(db, zSql, nSql, ppStmt, pz)) ){
    rc = mdb_wait_for_unlock_notify(db);
    if( rc != SQLITE_OK ) break;
  }
  return rc;
}
static int 
mdb_blocking_step(sqlite3_stmt *pStmt)
{
	int				rc;
	perf_val_t		ts, te;
	long				d;

	DO_PROFILE_USEC(ts, te, d) {
		while( SQLITE_LOCKED == (rc = sqlite3_step(pStmt)) ){
			rc = mdb_wait_for_unlock_notify(sqlite3_db_handle(pStmt));
			if( rc != SQLITE_OK ) break;
			//sqlite3_reset(pStmt);
			//if (rc != SQLITE_OK && rc < 100) 
			//	TRACE((T_WARN, "sqlite3_step returns %d:%s, going to retry (%.2f sec)\n", rc, sqlite3_errstr(rc), (float)d/1000000.0));
		}
	}
	DEBUG_ASSERT(rc != SQLITE_LOCKED);

	if (rc != SQLITE_OK && rc < 100) {
	  TRACE((T_WARN, "**** sqlite3_step returns %d:%s [%d] (%.2f sec)\n", rc, sqlite3_errstr(rc), SQLITE_LOCKED, (float)d/1000000.0));
	}
	return rc;
}



static void 
mdb_hash(sqlite3_context *context, int argc, sqlite3_value **argv) 
{
	nc_int64_t 	hid = 0;
	char		*v = (char *)sqlite3_value_text(argv[0]);

	hid = FNV1A_Hash_Yoshimura(v, strlen(v));
	sqlite3_result_int64(context, hid);

}
static int
mdb_register_hash_function(sqlite3 *db)
{
	int r;
	r = sqlite3_create_function(db, "mdbhash", 1, SQLITE_ANY, NULL, mdb_hash, NULL, NULL);
	return (r == SQLITE_OK)?0:-1;
}
static void
mdb_force_checkpoint(nc_mdb_handle_t *mdb, mdb_session_t * dbs, int forced)
{
        int     r = 0;
        int     nlog = 0;
        int     nchkpt = 0;


        if (forced) {
                r = sqlite3_wal_checkpoint_v2(dbs->DB, NULL, SQLITE_CHECKPOINT_TRUNCATE, &nlog, &nchkpt);
                if (r != SQLITE_OK) {
                        TRACE((T_INFO, "Database[%s] - WAL checkpoint error(%s), will be tried later \n", mdb->dbpath, sqlite3_errmsg(dbs->DB)));
                }
        }
        return ;
}
static sqlite3_stmt *
mdb_get_cached_stmt(nc_mdb_handle_t *mdb, mdb_session_t *session, int idx)
{
	sqlite3_stmt	*s = NULL;
	int				dbret;

	s = session->stmt[idx];

	if (s == NULL) {
		MDB_CHECK_RESULT(mdb, dbret = mdb_blocking_prepare_v2(session->DB, s__op_array[idx], -1, &session->stmt[idx], NULL));
		s = session->stmt[idx];
	}
	return s;
}

static int
mdb_reuse_direct(nc_mdb_handle_t *mdb, mdb_session_t *dbs,  uuid_t uuid, nc_int64_t rowid, nc_time_t vtime, nc_uint32_t nver)
{
	/*
	 * 	@new_v 	= new_cversion
	 *  @old_v 	= old_cversion
	 *  @hid   	= hid
	 *  @volume = signature
	 *  @key 	= object_key
	 *  @caseid = caseid
	 */

	sqlite3_stmt 	*stmt 	= NULL;
	int				dbret 	= 0;
	int				aff		= -1;

	stmt	= mdb_get_cached_stmt(mdb, dbs, MDB_STMT_REUSE_ROWID);
	DEBUG_ASSERT(stmt != NULL);

	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt, 	MDB_BPI(stmt, "@cversion"),		nver));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, 	MDB_BPI(stmt, "@rowid"), 		rowid));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt, 	MDB_BPI(stmt, "@vtime"), 		vtime));
	MDB_CHECK_RESULT(mdb, (dbret = mdb_blocking_step(stmt)));
	aff = sqlite3_changes(dbs->DB);
	MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));

	return aff;
}
int
mdb_txno_isnull(mdb_tx_info_t *z)
{
	return (z->decade == -1) && (z->txno == -1);
}
void
mdb_txno_null(mdb_tx_info_t *z)
{
	z->decade 	= -1;
	z->txno 	= -1;
}
int
mdb_txno_compare(mdb_tx_info_t *a, mdb_tx_info_t *b)
{
	if (a->decade != b->decade) return a->decade - b->decade;

	return a->txno - b->txno;
}



static int
mdb_op_insert(	
				nc_mdb_handle_t* mdb,
				mdb_session_t *	session, 
				int				txmode,
				char *			signature, 
				char *			path, 
				char *			key, 
				nc_uint64_t		hid,
				char			caseid, 
				uuid_t			uuid,
				int				cversion,
				nc_size_t		size,
				nc_size_t		rsize,
				nc_time_t		ctime,
				nc_time_t		mtime,
				nc_time_t		vtime,
				nc_mode_t		mode,
				char			*devid,
				nc_uint64_t		obi,
				int				origincode,
				int				viewcount,
				int				viewindex,
				char			*packed,
				int				packed_size
)
{
	sqlite3_stmt 		*stmt = NULL;
	int					dbret;
	int					affected;

	stmt	= mdb_get_cached_stmt(mdb, session, MDB_STMT_INSERT);

	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, session, txmode, dbret);

	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  MDB_BPI(stmt, "@volume_name"), signature, -1, SQLITE_STATIC));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  MDB_BPI(stmt, "@case_id"), 	&caseid, 1, SQLITE_STATIC));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  MDB_BPI(stmt, "@object_key"), key, -1, SQLITE_STATIC));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  MDB_BPI(stmt, "@object_path"), path, -1, SQLITE_STATIC));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_blob(stmt,  MDB_BPI(stmt, "@uuid"), uuid, sizeof(uuid_t), SQLITE_STATIC)); 
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, MDB_BPI(stmt, "@size"), size));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, MDB_BPI(stmt, "@rsize"), rsize));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,   MDB_BPI(stmt, "@cversion"), cversion));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, MDB_BPI(stmt, "@mode"), mode));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,   MDB_BPI(stmt, "@ctime"), ctime));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,   MDB_BPI(stmt, "@mtime"), mtime));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,   MDB_BPI(stmt, "@vtime"), vtime));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,   MDB_BPI(stmt, "@viewcount"), viewcount));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,   MDB_BPI(stmt, "@viewindex"), viewindex));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, MDB_BPI(stmt, "@hid"),hid));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_text(stmt,  MDB_BPI(stmt, "@devid"), devid, -1, SQLITE_STATIC));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, MDB_BPI(stmt, "@obits"), obi));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, MDB_BPI(stmt, "@origincode"), origincode));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_blob(stmt,  MDB_BPI(stmt, "@packed_property"), packed, packed_size, SQLITE_STATIC));
	
	MDB_CHECK_RESULT(mdb, dbret = mdb_blocking_step(stmt));
					
	affected = sqlite3_changes(session->DB);

	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, session, txmode, dbret);

	return affected;
}
static int
mdb_op_update_rowid(	nc_mdb_handle_t	*	mdb,
						mdb_session_t *		session, 
						int					tx,
						uuid_t				uuid,
						nc_int64_t			rowid,
						nc_size_t			size,
						nc_size_t			rsize,
						nc_time_t			vtime,
						nc_mode_t			mode,
						nc_uint64_t			obi
					)
{
	sqlite3_stmt 		*stmt = NULL;
	int					dbret = 0;
	int					affected = 0;

	stmt	= mdb_get_cached_stmt(mdb, session, MDB_STMT_UPDATE_ROWID);

	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, session, tx, dbret);

	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt,  	MDB_BPI(stmt, "@size"), 			size));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt,  	MDB_BPI(stmt, "@rsize"), 			rsize));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt,  	MDB_BPI(stmt, "@mode"), 			mode));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int(stmt,  		MDB_BPI(stmt, "@vtime"), 			vtime));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt,  	MDB_BPI(stmt, "@obits"), 			obi));
	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt,		MDB_BPI(stmt, "@rowid"),			rowid));

	MDB_CHECK_RESULT(mdb, mdb_blocking_step(stmt));
	affected = sqlite3_changes(session->DB);

	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, session, tx, dbret);

	return affected;

}
static int
mdb_op_purge_rowid(	nc_mdb_handle_t	*	mdb,
					mdb_session_t *		session,
					int					tx,
					int					ishard,
					nc_int64_t			rowid
		)
{

	sqlite3_stmt 		*stmt = NULL;
	int					dbret = 0;
	int					affected = 0;

	stmt	= mdb_get_cached_stmt(mdb, session, (ishard?MDB_STMT_HARD_PURGE_ROWID:MDB_STMT_SOFT_PURGE_ROWID));

	MDB_PREPARE_OP_TRANSACTION(__func__, mdb, session, tx, dbret);

	MDB_CHECK_RESULT(mdb, sqlite3_bind_int64(stmt, MDB_BPI(stmt, "@rowid"), rowid));

	MDB_CHECK_RESULT(mdb, mdb_blocking_step(stmt));
	affected = sqlite3_changes(session->DB);

	MDB_CHECK_RESULT(mdb, sqlite3_clear_bindings(stmt));
	MDB_CHECK_RESULT(mdb, sqlite3_reset(stmt));

	MDB_FINISH_OP_TRANSACTION(__func__, mdb, session, tx, dbret);


	return affected;
}

/*
 * insert & update & remove batch handler
 */
static void *
mdb_batch_upsert_handler(void *u)
{
	nc_mdb_handle_t		*mdb = (nc_mdb_handle_t *)u;

	mdb_session_t		*dbs = NULL;
	mdb_inserti_t		*upsi;
	mdb_updrow_t		*updrow;
	mdb_rrow_t 			*rrow;
#ifdef __PROFILE
	perf_val_t			ms, me;
	long				ud;
#endif
	mdb_opcode_t		*command;
	int					wl = 0;
#define	MDB_BATCH_THRESHOLD	500

#define	MDB_BATCH_DEQUEUE	min(100, MDB_BATCH_THRESHOLD)
	mdb_inserti_t		*br[MDB_BATCH_DEQUEUE];
	char				zuuid[128];
	int					bridx;
	int					deqed = 0;
	int					handled_ops = 0;
	int					r;
	int					counter_id = 0;
	char				*z_rrow_hard;
	char				*z_rrow_soft;


	dbs = mdb_pop_session(mdb, U_TRUE);
	DEBUG_ASSERT(dbs != NULL);


	while (!(mdb->needstop && wl == 0)) {
		deqed = tlcq_dequeue_batch(mdb->batch_queue, (void **)br, MDB_BATCH_DEQUEUE, 500);
		if (deqed > 0) {
			/*
			 * upsert 요청이 존재함:처리 시작
			 */

			do {
				r = sqlite3_exec(dbs->DB, "BEGIN IMMEDIATE   TRANSACTION", NULL, NULL, NULL);
				if (r == SQLITE_BUSY) bt_msleep(10);
			} while (r == SQLITE_BUSY);

			bridx = 0;
			handled_ops = 0;

			PROFILE_CHECK(ms);

			do {
				do {
					command		= (mdb_opcode_t *)br[bridx];
					counter_id	= -1;
					switch (*command) {
						case MDBOP_PURGE_ROWID:
							rrow = (mdb_rrow_t *)br[bridx];
							/* 
							 * select에 대한 where-clause의 input 파라미터 바인딩
							 */
							dbs->query 	= (rrow->ishard?z_rrow_hard:z_rrow_soft);
							counter_id 	= rrow->ishard ? MDB_IO_REMOVE:MDB_IO_WRITE;

							r = mdb_op_purge_rowid(	mdb, dbs, NO_TX, rrow->ishard, rrow->rowid);
					
							if (r == 0) {
								TRACE((T_INODE, "PURGE_ROWID::UUID[%s].ROWID[%lld] - already purged\n", uuid2string(rrow->uuid, zuuid, sizeof(zuuid)), rrow->rowid));
							}
							else {
								TRACE((0, "PURGE_ROWID::UUID[%s].ROWID[%lld] - OK\n", uuid2string(rrow->uuid, zuuid, sizeof(zuuid)), rrow->rowid));
							}

							MDB_SIGNAL_OR_FREE(mdb, dbs, rrow);



							break;
						case MDBOP_UPDATE_ROWID:
							updrow = (mdb_updrow_t *)br[bridx];
							/* 
							 * select에 대한 where-clause의 input 파라미터 바인딩
							 */
							counter_id 	= MDB_IO_WRITE;

							r = mdb_op_update_rowid(mdb, dbs, NO_TX, updrow->uuid, updrow->rowid, updrow->size, updrow->rsize, updrow->vtime, updrow->mode, updrow->obi);

							if (r == 0) {
								TRACE((T_INODE, "UPDATE_ROWID::UUID[%s].ROWID[%lld] - already purged\n", uuid2string(updrow->uuid, zuuid, sizeof(zuuid)), updrow->rowid));
							}
					
							MDB_SIGNAL_OR_FREE(mdb, dbs, updrow);


							break;
						case MDBOP_INSERT:
							upsi = (mdb_inserti_t *)br[bridx];
							dbs->query	= s__op_array[MDB_STMT_INSERT];
							counter_id 	= MDB_IO_WRITE;
			
							r = mdb_op_insert(	mdb,
												dbs, 
												NO_TX,
												upsi->signature, 
												upsi->path, 
												upsi->key, 
												upsi->hid,
												upsi->case_id, 
												upsi->uuid,
												upsi->cversion,
												upsi->size,
												upsi->rsize,
												upsi->ctime,
												upsi->mtime,
												upsi->vtime,
												upsi->imode,
												upsi->devid,
												upsi->obit,
												upsi->origincode,
												upsi->viewcount,
												upsi->viewindex,
												upsi->packed,
												upsi->packed_size);
							if (r == 0) {
								/* insert failed */
								upsi->rowid = -1LL;
							}
							else {
								upsi->rowid = sqlite3_last_insert_rowid(dbs->DB);
							}


							TRACE((T_INODE, "INSERT:Volume[%s].CKey[%s] - UUID[%s].ROWID[%lld] - inserted (affected=%d)\n", 
											upsi->signature, 
											upsi->key, 
											uuid2string(upsi->uuid, zuuid, sizeof(zuuid)),
											upsi->rowid, 
											r
											));
							MDB_SIGNAL_OR_FREE(mdb, dbs, upsi);

							break;
						default:
							TRACE((T_ERROR, "UNKNOWN COMMAND:%d\n",*command));
							break;
					}
					DEBUG_ASSERT(counter_id >= 0);
					mdb_counter(counter_id);


					handled_ops++;
					bridx++;
					deqed--;
				} while  (deqed > 0);

				if (handled_ops > MDB_BATCH_THRESHOLD) break;

				bridx = 0;
				/*
				 * wait없이 큐에 있는것만 fetch
				 */
				deqed = tlcq_dequeue_batch(mdb->batch_queue, (void **)br, MDB_BATCH_DEQUEUE, 0);

			} while (deqed > 0);

		
			PROFILE_CHECK(me);
#ifdef __PROFILE
			ud = PROFILE_GAP_MSEC(ms, me);

			if (ud > 1000) {
				TRACE((T_INFO, "MDB_BATCH_EXEC(%d ops) - too slow : %.2f sec\n", handled_ops, (ud/1000.0)));
			}
#endif
			//
			// END TRANSACTION
			//
		
			MDB_CHECK_RESULT(mdb, sqlite3_exec(dbs->DB, "COMMIT TRANSACTION", NULL, NULL, NULL));

#ifdef NC_ENABLE_MDB_WAIT
			pthread_mutex_lock(&mdb->poollock);
			mdb->txno_done = mdb->txno;
			mdb->txno.txno++;
			if (mdb->txno.txno == 0 || mdb->txno.txno < 0) { /* overflowed*/
				mdb->txno.decade++;
				mdb->txno.txno = 0;
			}
			TRACE((T_INODE, "%d ops handled, BROADCASTING[%lld.%lld]\n", handled_ops, mdb->txno_done.decade, mdb->txno_done.txno));
			if (mdb->donewaiters > 0)
				_nl_cond_broadcast(&mdb->donesignal);
			pthread_mutex_unlock(&mdb->poollock);
#endif
			wl = tlcq_length(mdb->batch_queue);

			if (mdb->needstop && wl > MDB_BATCH_THRESHOLD) {
				TRACE((T_INFO, "*** batch upsert queue increasing, current %d reqs in queue\n", wl));
			}

			//if (!r) {
			//	/*
			//	 * batch 처리해야할 껀 수가 없음
			//	 * best effort실행
			//	 */
			//	sqlite3_exec(dbs->DB, "PRAGMA optimize", NULL, NULL, NULL);
			//}
		}




	}
	/* TEMP */
	mdb_release_writer(mdb, dbs,  MDB_WRITER_NETCACHE);
	return NULL;
}

