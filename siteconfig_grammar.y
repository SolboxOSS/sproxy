%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
//#include <ncapi.h>
#include <util.h>
#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "siteconfig_grammar.h"
#include "httpd.h"
#include <libgen.h>

extern vstring_t		*gscx__config_root ;
#define 	scanner 	context->vscanner
int yylex_init (void ** );
void yyset_in  (FILE * in_str ,void * );
int yylex_destroy (void * yyscanner );
extern int  yylex(YYSTYPE * yylval_param,YYLTYPE * yylloc_param ,void * yyscanner);
static int _vo = 0;



/* 입력 받은 conf path 의 내용을 parsing 하여 site_t 객체 값을 셋팅한다. */

site_t *
scx_site_create(const char *path)
{
	TRC_BEGIN((__func__));

	if (!path)
	{
		TRC_END;
		return NULL;
	}

	int			ret = 0;
	struct stat	st;
	FILE*		f = NULL;
	site_t*		newsite = NULL;
	int 		r = 0;

	memset(&st, 0x00, sizeof(st));
	ret = stat(path, &st);

	/* 디렉토리는 제외한다. */
	if (ret >= 0 && S_ISDIR(st.st_mode) == 0)
	{
		if (f = fopen(path, "rb"))
		{
			char* confname = basename(path);

			newsite = scx_site_alloc(confname, st.st_size);
			if (newsite == NULL)
			{
				if (f) { fclose(f); }
				TRC_END;
				return NULL;
			}

			site_context_t 	S = { NULL, NULL };
			S.file = f;
			S.site = newsite;

			yylex_init(&S.vscanner);
			yyset_in(S.file, S.vscanner);
			r = yyparse(&S);
			yylex_destroy(S.vscanner);

			if (r)
			{
				/* 파싱 실패*/
				scx_site_destroy(newsite);
				newsite = NULL;
				TRACE((T_WARN, "configuration file(%s) parse failed.\n", path));
			}

			if (newsite && newsite->st_setup)
			{
				scx_site_commit(newsite);
				scx_conf_dump_to_shm(confname, f);	/* 설정 파일의 내용을 공유메모리에 복사한다. */
			}

			if (f) { fclose(f); }
		}
	}

	TRC_END;

	return newsite;
}

%}

/* %define api.pure full */
%locations
%pure-parser
%lex-param {void *scanner} 
%parse-param {site_context_t *context}

%token <token> TOK_SERVER 
%token <token> TOK_TRIGGER 
%token <token> TOK_BEGIN_GROUP 
%token <token> TOK_END_GROUP 
%token <token> TOK_OPERATOR 
%token <token> TOK_EQUAL 
%token <token> TOK_VALUE 
%token <token> TOK_VARIABLE 
%token <token> TOK_SCRIPT 
%union {
	struct {
		char 	*pstr;
		char 	string[4092];
	} token;
}
%%

site: /* empty */
		| server_config trigger_list
		;
server_config: TOK_SERVER TOK_BEGIN_GROUP statement_list TOK_END_GROUP 
				;
statement_list: /*empty*/
			| statement_list statement
			;

statement: TOK_VARIABLE TOK_EQUAL TOK_VALUE { 
				scx_site_update_param(context->site, (const char *)$1.string, (const char *)$3.string); 
					_vo && fprintf(stderr, "server set: '%s' = '%s'\n", $1.string, $3.string);
				}
		;

trigger_list :  /*empty*/
			| trigger_list trigger
			| trigger_list trigger_new			
			;
trigger: TOK_TRIGGER TOK_VARIABLE TOK_OPERATOR TOK_VALUE TOK_BEGIN_GROUP TOK_SCRIPT  TOK_END_GROUP { 
								_vo && fprintf(stderr, "trigger: ('%s', '%s', '%s')/%s/\n", $2.string, $3.string, $4.string, $6.string);
								scx_site_update_trigger(context->site, (const char *)$1.string, (const char *)$2.string, (const char *)$3.string, (const char *)$4.string, (const char *)$6.string);
							}
							;
trigger_new: TOK_TRIGGER TOK_BEGIN_GROUP TOK_SCRIPT  TOK_END_GROUP { 
								_vo && fprintf(stderr, "trigger: /%s/\n", $3.string);
								scx_site_update_trigger(context->site, (const char *)$1.string, NULL, NULL, NULL, (const char *)$3.string);
							}
