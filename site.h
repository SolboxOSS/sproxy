#ifndef __SITE_H__
#define __SITE_H__

#include <regex.h>
#include "reqpool.h"
#include "setup_vars.h"

struct request_tag; 
struct symbol_table_tag; 
typedef struct symbol_table_tag  symbol_table_t;
typedef struct site_tag {
/*
 * 아래 각 필드는 hashmap(code.google.com/p/sparsehash)와 같은
 * 고속 해시맵으로 변환할 필요 검토해야함
 * 아래와 같은 식으로 코딩하는 경우, 확장성을 저해하게되며,
 * lua script에서 해당 변수 참조할 때 switch문으로 접근하게되므로
 * 성능 저하 및 CPU 소모 심함
 */ 
	vstring_t 		*name;
	mem_pool_t 		*mp;	/* volume별 memory pool */
	void			*certificate;
	symbol_table_t 	*st_setup;
	symbol_table_t 	*st_trigger;
	void			*service;
} site_t;
typedef enum {
	OPR_MATCH 		= 0,
	OPR_NOTMATCH 	= 1,
	OPR_CONTAIN 	= 2,
	OPR_NOTCONTAIN 	= 3,
	OPR_UNKNOWN 	= 4
}optype_t;
typedef enum {
	VT_STRING	= 0,
	VT_UINT		= 1,
	VT_BOOL		= 2
} valtype_t;

typedef enum {
	LUA_PHASE_UNKNOWN      	= 0,
	LUA_PHASE_HOST_REWRITE,
	LUA_PHASE_CLIENT_REQUEST,
	LUA_PHASE_CLIENT_RESPONSE,
	LUA_PHASE_CACHE_LOOKUP,
	LUA_PHASE_CACHE_SAVE,
	LUA_PHASE_ORIGIN_REQUESE,
	LUA_PHASE_ORIGIN_RESPONSE
} phasetype_t;

typedef struct {
	phasetype_t		phase;
	vstring_t 		*variable;
	optype_t 		operator;
	vstring_t  		*operand;	
	int 			pattern_op; /* 1: operand가 '/', '/'로 둘러쌓인 경우 */
	vstring_t 		*script;
	regex_t 		compiled;
} trigger_t;

typedef struct site_context {
	mem_pool_t 	*pool;
	FILE 		*file;
	site_t 		*site;
	void 		*vscanner;
} site_context_t; 

typedef enum {
	SITE_KEY_ADD		= 0,
	SITE_KEY_OVERWRITE	= 1
} sitekey_t;

typedef struct rewrite_scripts_tag {
	mem_pool_t 	*mp;
	int			count;
	char      	*script[10];
} rewrite_scripts_t;

//union YYSTYPE {
//	kv_xtra_options_t 	*options;
//}

site_t * scx_site_create(const char *path);
site_t * scx_site_clone(const char *matching, site_t *);
int scx_site_update_trigger(site_t *site, const char *pha, const char *var, const char *op, const char *operand, const char *script);
int scx_site_update_param(site_t *site, const char *var, const char *value);
unsigned long long scx_get_uint64(site_t *site, const char *var_name, unsigned long long defval);
vstring_t * scx_get_vstring(site_t *site, const char *var_name, vstring_t *defval);
vstring_t * scx_get_vstring_pool(mem_pool_t *pool, site_t *site, const char *var_name, vstring_t *defval);
vstring_t * scx_get_vstring_lg(site_t *site, const char *var_name, char *defval);
unsigned int scx_get_uint(site_t *site, const char *var_name, unsigned int defval);
int scx_get_int(site_t *site, const char *var_name, int defval);
void scx_set_vstring(site_t *site, const char *var_name, vstring_t *val);
site_t * scx_site_alloc(const char *name, off_t st_size);
void scx_site_destroy(site_t *site);
rewrite_scripts_t * site_copy_rewrite_script(site_t *site);
int site_run_host_rewrite(struct request_tag *req);
int site_run_trigger_client_request(struct request_tag *req);
int site_run_trigger_client_response(struct request_tag *req);
int site_run_origin_trigger(nc_origin_io_command_t *command, int phase_id);
int scx_contains(site_t *site, const char *var_name, vstring_t *element);
void scx_site_commit(site_t *site);

symbol_table_t * st_create(mem_pool_t *pool, int tblsize);
int st_put(mem_pool_t *pool, symbol_table_t *T, const char *var, void *data, size_t datalen, int overwrite);
void * st_get(symbol_table_t *T, const char *var);
void st_commit(symbol_table_t *T);
symbol_table_t * st_clone_table(mem_pool_t *pool, symbol_table_t *T);
void * st_get_by_index(symbol_table_t *T, int idx);

void scx_conf_dump_to_shm(const char *matching, FILE *fd);
void scx_conf_close_shm();
void scx_conf_dump_from_shm();
#endif /*__SITE_H__ */
