#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <microhttpd.h>
#include <regex.h>
//#include <ncapi.h>
#include <sys/mman.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "parse_defs.h"
#include "luaembed.h"
#include "scx_util.h"
#include "../include/rdtsc.h"
extern vstring_t		*gscx__config_root;
extern size_t 			gscx__site_pool_size;
extern rewrite_scripts_t	*gscx__current_rewrite_scripts;
static vstring_t * site_get_R_value(nc_request_t *req, vstring_t *var);
static int site_check_assertion(nc_request_t *req, trigger_t *trg);
static int scx_dup_compiled(site_t *site);
/*
 * site configuration handling
 *
 * 	one file per one site
 * 		
 *	
 *
 */

phasetype_t
scx_lookup_phase(const char *trigger_str)
{
	struct st_phase{
		char name[20];
		phasetype_t phase;
	} phase_map[] =  {
		{"host_rewrite", LUA_PHASE_HOST_REWRITE},		/* volume이 결정되기 이전의 단계, 이곳에서만 host 변경을 통한 volume 변경이 가능함 */
		{"trigger", LUA_PHASE_CLIENT_REQUEST},		/* trigger는 기존에 설치된것과의 호환성을 유지하기 위해 사용함. */
		{"client_request", LUA_PHASE_CLIENT_REQUEST},
		{"client_response", LUA_PHASE_CLIENT_RESPONSE},
		{"cache_lookup", LUA_PHASE_CACHE_LOOKUP},
		{"cache_save", LUA_PHASE_CACHE_SAVE},
		{"origin_request", LUA_PHASE_ORIGIN_REQUESE},
		{"origin_response", LUA_PHASE_ORIGIN_RESPONSE}
	};
	int 	i;
	int 	phase_cnt = howmany(sizeof(phase_map), sizeof(struct st_phase));
	for (i = 0; i < phase_cnt; i++) {
		if (strcmp(trigger_str, phase_map[i].name) == 0) {
			return phase_map[i].phase;
		}
	}
	return LUA_PHASE_UNKNOWN;

}

optype_t 
scx_lookup_operator(const char *operand)
{
	struct {
		char name[4];
		optype_t op;
	} op_map[] =  {
		{"=", OPR_MATCH},
		{"!=", OPR_NOTMATCH},
		{"~", OPR_CONTAIN},
		{"!~", OPR_NOTCONTAIN}
	};
	int 	i;
	for (i = 0; i < 4; i++) {
		if (strcmp(operand, op_map[i].name) == 0) {
			return op_map[i].op;
		}
	}
	return OPR_UNKNOWN;
	
}

/*
 * 주어진 matching은 '.'으로 구분된 호스트 또는 도메인 명이어야한다.
 * matching의 방법은 max-matching임
 */
site_t *
scx_site_alloc(const char *name, off_t st_size)
{
	site_t 	*s;
	mem_pool_t 	*mpool = NULL;
	size_t	alloc_size = 0;
	if (gscx__site_pool_size < (st_size+1024)) { /* 파일의 크기가 기본 pool 크기보다 큰 경우 파일 크기 기준으로 할당한다. */
		alloc_size = st_size + 1024;
	}
	else {
		alloc_size = gscx__site_pool_size;
	}

	mpool = mp_create(alloc_size);

	s = mp_alloc(mpool, sizeof(site_t));
	s->name 	= vs_strdup_lg(mpool, name);
	s->mp		= mpool;

	s->st_setup 	= st_create(mpool, 100);
	s->st_trigger 	= st_create(mpool, 5);
	s->certificate	= NULL;
	s->service	= NULL;

	return s;
}
void
scx_site_commit(site_t *site)
{
	st_commit(site->st_setup);
	//st_commit(site->st_trigger);
}


site_t *
scx_site_clone(const char *name, site_t *ref)
{
	site_t 	*clone = NULL;

	clone = scx_site_alloc(name, 0);
#if 0
	if (ref->cert)	/* 인증서 경로는 복사가 필요 없을 수도 있다. */
		clone->cert			= vs_strdup(clone->mp, ref->cert);
	if (ref->key)
		clone->key			= vs_strdup(clone->mp, ref->key);
#endif
	clone->st_setup 	= st_clone_table(clone->mp, ref->st_setup);
#if 0
	clone->st_trigger 	= st_clone_table(clone->mp, ref->st_trigger);
	scx_dup_compiled(clone);
#else
	/*
	 * st_clone_table()으로 lua script는 복사가 정상적으로 되지 않는다.
	 * lua script 가 포함된 st_clone_table()에서  lua script(site->st_trigger->table[0].value)->script) 가 NULL로 들어가 죽는 현상이 나올수 있어서
	 * lua script는 clone 되지 않도록 한다.
	 */
	clone->st_trigger 	= st_create(clone->mp, 1);
#endif

	return clone;
}


int
scx_site_update_param(site_t *site, const char *var, const char *value)
{
	TRC_BEGIN((__func__));
	TRACE((T_DAEMON, "VAR[%s] = VALUE[%s]\n", var, value));
	st_put(site->mp, site->st_setup, var, (void *)value, strlen(value)+1, SITE_KEY_OVERWRITE);
	TRC_END;
	return 0;
}
int
scx_site_update_trigger(site_t *site, const char *pha, const char *var, const char *op, const char *operand, const char *script)
{
	trigger_t 	*trigger;
	int 		r = 0;
	char 		key[64] = {0};
	TRC_BEGIN((__func__));
	trigger = mp_alloc(site->mp, sizeof(trigger_t));
	trigger->phase = scx_lookup_phase(pha);
	/* variable이 NULL인 경우 operator와 operand가 있어도 모두 NULL 처리 한다. */
	if (var != NULL) {
		trigger->variable = vs_strdup_lg(site->mp, var);
		trigger->operator = scx_lookup_operator(op);
		if (trigger->operator == OPR_UNKNOWN) {
			/* unknown operator */
			TRC_END;
			return -1;
		}
		if (operand[0] == '/') {
			trigger->pattern_op++;
			/* 앞뒤의 '/'의 삭제 */
			trigger->operand 	= vs_strndup_lg(site->mp, (char *)operand+1, strlen(operand+1)-1);
			r = regcomp(&trigger->compiled, vs_data(trigger->operand), REG_EXTENDED);
		}
		else {
			trigger->operand 	= vs_strdup_lg(site->mp, operand);
			r = 0;
		}
	}
	else {
		trigger->variable = NULL;
		trigger->operator = OPR_UNKNOWN;
		trigger->operand = NULL;
	}

	trigger->script 	= vs_strdup_lg(site->mp, script);
	if (r) {
		char ebuf[512];
		regerror(r, &trigger->compiled, ebuf, sizeof(ebuf));
		TRACE((T_TRIGGER|T_ERROR, "trigger value[%s] - %s\n", operand, ebuf));
		r = -1;
	}
	else {
		if(var != NULL) {
			TRACE((T_DAEMON|T_TRIGGER, "(%s %s %s %s) added\n", pha, var, op, operand));
			/* key 중복 방지를 위해 모두 합친 문자열을 key로 한다. ( 예:client_request_REQUEST.method_=_/GET|HEAD/ ) */
			snprintf(key,sizeof(key), "%s_%s_%s_%s", pha, var, op, operand);
		}
		else {
			TRACE((T_DAEMON|T_TRIGGER, "(%s %s) added\n", pha, var));
			snprintf(key,sizeof(key), "%s", pha);
		}
		st_put(site->mp, site->st_trigger, key, trigger, sizeof(trigger_t), SITE_KEY_ADD);
	}
	TRC_END;
	return r;
}
void
scx_site_destroy(site_t *site)
{
	int 		i = 0;
	trigger_t 	*trigger = NULL;
	trigger = (void *)st_get_by_index(site->st_trigger, i++);
	while (trigger) {
		if(trigger->pattern_op) {
			regfree(&trigger->compiled); /* site의 regex compiled context를 free한다. */
		}
		trigger = (void *)st_get_by_index(site->st_trigger, i++);
	}
	if(site->certificate != NULL) scx_sni_unload_keys(site->certificate);

	/* memory pool만 삭제하면 모두 지워진다. */
	mp_free(site->mp);

}

rewrite_scripts_t *
site_copy_rewrite_script(site_t *site)
{
	rewrite_scripts_t *rewrite_scripts = NULL;
	mem_pool_t 	*mpool = NULL;
	trigger_t 	*trigger = NULL;
	int count = 0;
	int 		i = 0;

	mpool = mp_create(gscx__site_pool_size);
	rewrite_scripts = mp_alloc(mpool, sizeof(rewrite_scripts_t));
	rewrite_scripts->mp = mpool;

	trigger = (void *)st_get_by_index(site->st_trigger, i++);
	while (trigger) {
		if(trigger->phase == LUA_PHASE_HOST_REWRITE) {	/* host_rewrite phase는 default의 trigger만 인식한다. */
			rewrite_scripts->script[count] = mp_alloc(mpool, vs_length(trigger->script)+1);
			memcpy(rewrite_scripts->script[count], vs_data(trigger->script), vs_length(trigger->script));
			count++;
		}
		trigger = (void *)st_get_by_index(site->st_trigger, i++);
	}
	rewrite_scripts->count = count;
	return rewrite_scripts;
}

#if 1
int
site_run_host_rewrite(nc_request_t *req)
{
	int 		i = 0;
	rewrite_scripts_t *rewrite_scripts = NULL;
	double 		d1,d2;

	req->scx_res.trigger = LUA_TRIGGER_CONTINUE;
	rewrite_scripts = gscx__current_rewrite_scripts; /* host_rewrite phase는 default의 trigger만 인식한다. */
	d1 = sx_get_time();

	for(i=0; i < rewrite_scripts->count;i++) {
		TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - running trigger host rewrite phase[%d]\n", req->id, vs_data(req->url), i));
		scx_lua_rewrite_phase(req, rewrite_scripts->script[i]);
		TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger host rewrite phase[%d] done\n", req->id, vs_data(req->url), i));
		if(req->scx_res.trigger != LUA_TRIGGER_CONTINUE)
			break;
	}
	d2 = sx_get_time();
	TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger host rewrite time %.2f microseconds\n", req->id, vs_data(req->url), (float)(d2-d1)));
	return 0;
}
#else
int
site_run_host_rewrite(nc_request_t *req)
{
	int 		i = 0;
	trigger_t 	*trigger = NULL;
	char 		*value;
	int 		hit = 0;
	int 		asserted = 0;
	site_t 		*site;
	double 		d1,d2;

	req->scx_res.trigger = LUA_TRIGGER_CONTINUE;

	site = gscx__default_site; /* host_rewrite phase는 default의 trigger만 인식한다. */
	d1 = sx_get_time();

	trigger = (void *)st_get_by_index(site->st_trigger, i++);
	while (trigger) {
		if(trigger->phase == LUA_PHASE_HOST_REWRITE) {
//				asserted = site_check_assertion(req, trigger);
//				if (asserted) {
				TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - running trigger host rewrite phase[%d]\n", req->id, vs_data(req->url), trigger->phase));
				scx_lua_rewrite_phase(req, vs_data(trigger->script));
				TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger host rewrite phase[%d] done\n", req->id, vs_data(req->url), trigger->phase));
//					hit++;
				if(req->scx_res.trigger != LUA_TRIGGER_CONTINUE)
					break;
//				}
		}
		trigger = (void *)st_get_by_index(site->st_trigger, i++);
	}

	d2 = sx_get_time();
	TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger host rewrite time %.2f microseconds\n", req->id, vs_data(req->url), (float)(d2-d1)));

	return hit;
}
#endif

int
site_run_trigger_client_request(nc_request_t *req)
{
	int 		i = 0;
	trigger_t 	*trigger = NULL;
	char 		*value;
	int 		hit = 0;
	int 		asserted = 0;
	site_t 		*site;

	double 		d1,d2;

	req->scx_res.trigger = LUA_TRIGGER_CONTINUE;
	site = req->service->site;
	d1 = sx_get_time();

	trigger = (void *)st_get_by_index(site->st_trigger, i++);
	while (trigger) {
		if(trigger->phase == LUA_PHASE_CLIENT_REQUEST) {
			if(trigger->variable == NULL) {
				asserted = 1;	/* trigger에 variable가 없는 경우는 operator 비교를 하지 않는다. */
			}
			else  {
				asserted = site_check_assertion(req, trigger);
			}
			if (asserted) {
				TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - running trigger client request phase[%d]\n", req->id, vs_data(req->url), trigger->phase));
				scx_lua_client_request_phase(req, vs_data(trigger->script));
				TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger client request phase[%d] done\n", req->id, vs_data(req->url), trigger->phase));
				hit++;
				if(req->scx_res.trigger != LUA_TRIGGER_CONTINUE) {
					break;
				}
			}
		}
		trigger = (void *)st_get_by_index(site->st_trigger, i++);
	}

	d2 = sx_get_time();
	TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger client request time %.2f microseconds\n", req->id, vs_data(req->url), (float)(d2-d1)));
	return hit;
}

int
site_run_trigger_client_response(nc_request_t *req)
{
	int 		i = 0;
	trigger_t 	*trigger = NULL;
	char 		*value;
	int 		hit = 0;
	int 		asserted = 0;
	site_t 		*site;

	double 		d1,d2;

	req->scx_res.trigger = LUA_TRIGGER_CONTINUE;
	site = req->service->site;
	d1 = sx_get_time();

	trigger = (void *)st_get_by_index(site->st_trigger, i++);
	while (trigger) {
		if(trigger->phase == LUA_PHASE_CLIENT_RESPONSE) {
			TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - running trigger client response phase[%d]\n", req->id, vs_data(req->url), trigger->phase));
			scx_lua_client_response_phase(req, vs_data(trigger->script));
			TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger client response phase[%d] done\n", req->id, vs_data(req->url), trigger->phase));
			hit++;
			if(req->scx_res.trigger != LUA_TRIGGER_CONTINUE) {
				break;
			}
		}
		trigger = (void *)st_get_by_index(site->st_trigger, i++);
	}

	d2 = sx_get_time();
	TRACE((T_TRIGGER|T_DAEMON, "[%llu] URL[%s] - trigger client response time %.2f microseconds\n", req->id, vs_data(req->url), (float)(d2-d1)));
	return hit;
}

int
site_run_origin_trigger(nc_origin_io_command_t *command, int phase_id)
{
	int 		i = 0;
	trigger_t 	*trigger = NULL;
	char 		*value;
	int 		hit = 0;
	int 		asserted = 0;
	site_t 		*site;
	double 		d1,d2;
	int 		trig_phase;

	if(phase_id == NC_ORIGIN_PHASE_REQUEST) {
		trig_phase = LUA_PHASE_ORIGIN_REQUESE;
	}
	else if (phase_id == NC_ORIGIN_PHASE_RESPONSE) {
		trig_phase =  LUA_PHASE_ORIGIN_RESPONSE;
	}
	else {

	}
	site = (site_t *)command->cbdata;
	d1 = sx_get_time();

	trigger = (void *)st_get_by_index(site->st_trigger, i++);
	while (trigger) {
		if(trigger->phase == trig_phase) {
			 /* 현재는 조건 검사 없이 해당 phase에서는 무조건 실행한다. */
			//asserted = site_check_assertion(req, trigger); 현재는 조건 검사 부분을 뺐다
			//if (asserted) {
				TRACE((T_TRIGGER|T_DAEMON, "URL[%s] - running origin trigger phase[%d]\n", command->url, trig_phase));
				scx_lua_origin_phase(command, vs_data(trigger->script), trig_phase);
				TRACE((T_TRIGGER|T_DAEMON, "URL[%s] - origin trigger phase[%d] done\n", command->url, trig_phase));
				hit++;
			//	if(req->scx_res.trigger != LUA_TRIGGER_CONTINUE)
			//		break;
			//}
			break;
		}
		trigger = (void *)st_get_by_index(site->st_trigger, i++);
	}

	d2 = sx_get_time();
	TRACE((T_TRIGGER|T_DAEMON, "URL[%s] - origin trigger time %.2f microseconds\n", command->url, (float)(d2-d1)/1000.0));

	return hit;
}


static int
site_check_assertion(nc_request_t *req, trigger_t *trg)
{
	vstring_t 	*value;
	int 		ass = 0;

	value = site_get_R_value(req, trg->variable);
	if (!value) return 0;
	if (trg->pattern_op) {
		ass = regexec(&trg->compiled, vs_data(value),  0, NULL, 0);

	}
	else {
		ass = vs_strcasecmp(value, trg->operand);
		if (trg->operator == OPR_NOTMATCH) ass = !ass;
	}

	return (ass == 0);
}
static vstring_t *
site_get_R_value(nc_request_t *req, vstring_t *var)
{
	char 			*v = NULL;
	vstring_t 		*vs = NULL;
	if (vs_strncasecmp_lg(var, "HEADER.", 7) == 0) {
		/* TODO: 변수 명에 '-'는 어떻게 할건지 */
		v = (char *)kv_find_val_extened(req->options, vs_data(var)+7, 0, 1);
		if (v) {
			vs = vs_allocate(req->pool, 0, v, 0);
		}
	}
	else if (vs_strncasecmp_offset_lg(var, 0, "REQUEST.", 8) == 0) {
		if (vs_strncasecmp_offset_lg(var, 8, "URL", 4) == 0) {
			vs = req->url;
		}
		else if (vs_strncasecmp_offset_lg(var, 8, "PATH", 4) == 0) {
			vs = req->path;
		}
		else if (vs_strncasecmp_offset_lg(var, 8, "METHOD", 6) == 0) {
			vs = req->zmethod;
		}
	}
	return vs;
}
unsigned int
scx_get_uint(site_t *site, const char *var_name, unsigned int defval)
{
	char 			*value = NULL;
	unsigned int	intval = 0;
	value = (char *)st_get(site->st_setup, var_name);
	intval = defval;
	if (value) {
		intval = (unsigned int)atoi(value);
	}
	return intval;
}
unsigned long long
scx_get_uint64(site_t *site, const char *var_name, unsigned long long defval)
{
	char 				*value = NULL;
	unsigned long long	intval = 0;
	value = (char *)st_get(site->st_setup, var_name);
	intval = defval;
	if (value) {
		intval = (unsigned long long)atoll(value);
	}
	return intval;
}

int
scx_get_int(site_t *site, const char *var_name, int defval)
{
	char 			*value = NULL;
	int	intval = 0;
	value = (char *)st_get(site->st_setup, var_name);
	intval = defval;
	if (value) {
		intval = (int)atoi(value);
	}
	return intval;
}

vstring_t * 
scx_get_vstring(site_t *site, const char *var_name, vstring_t *defval)
{
	char 			*value = NULL;
	vstring_t 		*vstr = NULL;
	value = (char *)st_get(site->st_setup, var_name);

	vstr = defval;
	if (value) {
		/*
		 * 실제 스트링 메모리 할당하지 않고, symbol table내의
		 * value 포인터만 이용
		 */
		vstr = vs_allocate(site->mp, 0, value, 0);
		vs_trim_string(vstr);
	}
	//여기에 trim 함수가 들어간다.
	return vstr;
}
vstring_t * 
scx_get_vstring_pool(mem_pool_t *pool, site_t *site, const char *var_name, vstring_t *defval)
{
	char 			*value = NULL;
	vstring_t 		*vstr = NULL;
	value = (char *)st_get(site->st_setup, var_name);

	vstr = defval;
	if (value) {
		/*
		 * 실제 스트링 메모리 할당하지 않고, symbol table내의
		 * value 포인터만 이용
		 */
		vstr = vs_allocate(pool, 0, value, 0);
		vs_trim_string(vstr);
	}
	return vstr;
}
vstring_t * 
scx_get_vstring_lg(site_t *site, const char *var_name, char *defval)
{
	char 			*value = NULL;
	vstring_t 		*vstr = NULL;
	value = (char *)st_get(site->st_setup, var_name);

	if (value) {
		/*
		 * 실제 스트링 메모리 할당하지 않고, symbol table내의
		 * value 포인터만 이용
		 */
		vstr = vs_allocate(site->mp, 0, value, 0);
		vs_trim_string(vstr);
	}
	else {
		vstr = vs_strdup_lg(site->mp, defval);
	}
	return vstr;
}
void
scx_set_vstring(site_t *site, const char *var_name, vstring_t *val)
{
	st_put(site->mp, site->st_setup, var_name, vs_data(val), vs_length(val)+1, SITE_KEY_ADD);
}
/*
 * 아래 함수에서 pool에서 메모리할당을 사용하지 않음을 유의할것
 */
int
scx_contains(site_t *site, const char *var_name, vstring_t *element)
{
	char 	*value;
	char 	*poff = NULL;
	int 	conta = 0;

	value = (char *)st_get(site->st_setup, var_name);
	if (value && vs_length(element) > 0) {
		poff = strcasestr(value, vs_data(element));
		if (poff) {
			conta = (poff[vs_length(element)] == '\0' ||
					poff[vs_length(element)] == ' ' ||
					poff[vs_length(element)] == ';' ||
					poff[vs_length(element)] == '\t' ||
					poff[vs_length(element)] == ',') ;
		}
		
	}
	return conta;
}

/*
 * open된 설정파일의 내용을 모두 공유 메모리에 기록한다.
 */
void
scx_conf_dump_to_shm(const char *matching, FILE *fd)
{
	char *p = (char *)matching;
	unsigned char buf[1024] = {'\0'};
	size_t len;
	int shf_fd = 0;
	void * shm_ptr ;
	fseek(fd, 0, SEEK_SET);

	/* reload시 설정없데이트에 대한 문제를 해결 못해서 당분간 처음의 default.conf의 설정만 기록 하도록 한다. */
	if (strcasecmp(matching, "default.conf") == 0) {
		shf_fd = shm_open(NAME_SHM_PROG, O_CREAT|O_TRUNC|O_RDWR, 0666);
	}
	else {
		/* service 설정은 기록 하지 않는다. */
		return;
		shf_fd = shm_open(NAME_SHM_PROG, O_RDWR, 0666);
		lseek(shf_fd, 0, SEEK_END);
	}
	if(shf_fd < 0)
		return;

	len = snprintf(buf, sizeof(buf)-1, "********************** configuration (%s) start ********************\n", p);
	write(shf_fd, buf, len);
	while (1) {
		len  = fread (buf, 1, sizeof(buf), fd);
		if(len <= 0 )
			break;
		write(shf_fd, buf, len);
	}
	len = snprintf(buf, sizeof(buf)-1, "********************** configuration (%s) end   ********************\n", p);
	write(shf_fd, buf, len);
	close(shf_fd);
	return;
}

/*
 * 공유메모리에 기록된 설정파일 로그를 삭제한다.
 */
void
scx_conf_close_shm()
{
	shm_unlink(NAME_SHM_PROG);
//	 munmap(shf_fd, shm_length);
//	 close(shf_fd);
}

void
scx_conf_dump_from_shm()
{
	unsigned char buf[1024] = {'\0'};
	size_t len;
	int shf_fd = 0;
	shf_fd = shm_open(NAME_SHM_PROG, O_RDONLY, 0666);
	if(shf_fd < 0){
		printf("%s service is stoped.\n", PROG_SHORT_NAME);
		return;
	}
	while (1) {
		len  = read (shf_fd, buf, sizeof(buf)-1);
		if(len <= 0 )
			break;
		buf[len] = '\0';
		printf("%s", buf);
	}
	close(shf_fd);
}

/*
 * trigger의 compiled에 컴파일 된 정규식은 복사가 되지 않기 때문에 별도로 컴파일 한다.
 * 이렇게 하지 않을 경우 default site와 compiled의 컴파일 결과가 동일한 메모리 번지를 가리키고 있기 때문에
 * regfree()시 중복 삭제가 될수도 있고
 * default 설정이 업데이트 되는 경우 영향을 받을 수도 있다.
 */
static int
scx_dup_compiled(site_t *site)
{
	int 		i = 0;
	trigger_t 	*trigger = NULL;
	trigger = (void *)st_get_by_index(site->st_trigger, i++);
	while (trigger) {
		if(trigger->pattern_op) {
			regcomp(&trigger->compiled, vs_data(trigger->operand), REG_EXTENDED);
		}
		trigger = (void *)st_get_by_index(site->st_trigger, i++);
	}
}
