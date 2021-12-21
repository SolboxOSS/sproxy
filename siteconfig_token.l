%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
//#include <ncapi.h>
#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "scx_util.h"
#include "siteconfig_grammar.h"
//#include "siteconfig_token.h"
/*#define 	YY_DECL		site_token_t *yylex(void) */
/*site_token_t 	yylval; */
static int _vo = 0;
%}
%option noyyalloc noyyrealloc noyyfree
%option debug
%option reentrant stack
%option bison-bridge
%option bison-locations
%option noyywrap
/*%option header-file="siteconfig_token.h"*/
%x str 
%x lua lua_internal trigger server server_set server_set_value server_set_quoted trigger_quote trigger_op
%%

\#.*\n  {
			//fprintf(stderr, "<server_set>:comment(%s)\n", yytext);
		}
server 	{
			_vo&& fprintf(stderr, "TOK:server\n");
			BEGIN(server);
			strcpy(yylval->token.string, yytext);
			return TOK_SERVER;
		}
<server>\{ 		{
			BEGIN(server_set);
			_vo&& fprintf(stderr, "TOK<server>: BEGIN_GROUP\n");
			strcpy(yylval->token.string, yytext); 
			return TOK_BEGIN_GROUP;
		}
<server>\} 		{
			BEGIN(INITIAL);
				_vo&& fprintf(stderr, "TOK<server_set>: END_GROUP\n");
			/**yylval->token.pstr = '\0'; */
			return TOK_END_GROUP;
		}
<server_set>\} 		{
			BEGIN(INITIAL);
				_vo&& fprintf(stderr, "TOK<server_set>: END_GROUP\n");
			/* *yylval->token.pstr = '\0'; */
			return TOK_END_GROUP;
		}

<server_set>\#.*\n {
			_vo&& fprintf(stderr, "<server_set>:comment(%s)\n", yytext);
		}

<server_set>[[:alpha:]][_[:alnum:]]* {
					_vo&& fprintf(stderr, "TOK<server_set>: VAR[%s]\n", yytext);
					strcpy(yylval->token.string, yytext);
					return TOK_VARIABLE;
			}
<server_set>= {
				BEGIN(server_set_value);
				_vo&& fprintf(stderr, "TOK<server_set>: = (%s:op)\n", yytext);
				strcpy(yylval->token.string, yytext);
				return TOK_EQUAL;
			
			}
<server_set_value>[[:alnum:] ][[:punct:][:blank:][:alnum:]]* {
					int 	len;
					char 	*psp = yytext;
					char 	*pcan = yytext;
					BEGIN(server_set);
					while (*psp == ' ') psp++;

					pcan = psp;
					len = strlen(pcan);
					while (*pcan) {
						if (*pcan == '\\') {
							memmove(pcan, pcan+1, strlen(pcan+1)+1);
							pcan++; 
						}
						pcan++;
					}

					_vo&& fprintf(stderr, "TOK<server_set>: VALUE[%s]\n", psp);
					strcpy(yylval->token.string, psp);
					return TOK_VALUE;
			}
<server_set_value>\"([^\\\"]|\\.)*\" {
				char 	*pe;
				BEGIN(server_set);
				pe = yytext+strlen(yytext);
				if (*(pe-1) == '"') *(pe-1) = '\0';
				strcpy(yylval->token.string, &yytext[1]);
				_vo&& fprintf(stderr, "TOK<server_set>: QUOTE (%s)\n", yylval->token.string);
				return TOK_VALUE;
			}

<trigger_quote>[^\\\n\"]+ {
			char  *yptr = yytext;
			_vo&& fprintf(stderr, "yytext='%c'\n", *yptr);
			while (*yptr) *yylval->token.pstr++ = *yptr++;
		}



host_rewrite|trigger|client_request|client_response|cache_lookup|cache_save|origin_request|origin_response 	{
			_vo&& fprintf(stderr, "TOK_TRIGGER\n"); 
			strcpy(yylval->token.string, yytext);
			BEGIN(trigger);
			return TOK_TRIGGER;
		}
<trigger>[a-z][a-z0-9\._]* {
				_vo&& fprintf(stderr, "<trigger>:TOK_VAR(%s)\n", yytext);
				strcpy(yylval->token.string, yytext);
				return TOK_VARIABLE;
		}
<trigger>\~ {
				BEGIN(trigger_op);
				_vo&& fprintf(stderr, "<trigger>:TOK_OP(%s)\n", yytext);
				strcpy(yylval->token.string, yytext);
				return TOK_OPERATOR;
		}
<trigger>\!\~  {
				BEGIN(trigger_op);
				_vo && fprintf(stderr, "<trigger>:TOK_OP(%s)\n", yytext);
				strcpy(yylval->token.string, yytext);
				return TOK_OPERATOR;
		}
<trigger>=  {
				BEGIN(trigger_op);
				_vo && fprintf(stderr, "<trigger>:TOK_OP(%s)\n", yytext);
				strcpy(yylval->token.string, yytext);
				return TOK_OPERATOR;
		}
<trigger>\!= {
				BEGIN(trigger_op);
				_vo && fprintf(stderr, "<trigger>:TOK_OP(%s)\n", yytext);
				strcpy(yylval->token.string, yytext);
				return TOK_OPERATOR;
		}
		
<trigger>\{ 		{
			_vo && fprintf(stderr, "<trigger_op>:TOK_BEGIN_GROUP\n");
			BEGIN(lua_internal);
			strcpy(yylval->token.string, yytext); 
			return TOK_BEGIN_GROUP;
		}		

<trigger_op>\"(\\.|[^\\"])*\" {
			char 	*pe;
			_vo && fprintf(stderr, "<trigger>:TOK_VALUE(%s)\n", yytext);
			if (pe) *pe = 0;
			strcpy(yylval->token.string, &yytext[1]);
			return TOK_VALUE;
		}
<trigger_op>\/(\\.|[^\\"])*\/ {
			_vo && fprintf(stderr, "<trigger>:TOK_VALUE(%s)\n", yytext);
			strcpy(yylval->token.string, yytext);
			return TOK_VALUE;
		}

<trigger_op>[[:alnum:]]+ {
			_vo && fprintf(stderr, "<trigger>:TOK_VALUE(%s)\n", yytext);
			strcpy(yylval->token.string, yytext);
			return TOK_VALUE;
		}
<trigger_op>\{ 		{
			_vo && fprintf(stderr, "<trigger_op>:TOK_BEGIN_GROUP\n");
			BEGIN(lua_internal);
			strcpy(yylval->token.string, yytext); 
			return TOK_BEGIN_GROUP;
		}
<lua_internal>\} 		{
			BEGIN(INITIAL);
			_vo && fprintf(stderr, "<trigger_op>:TOK_END_GROUP\n");
			strcpy(yylval->token.string, yytext); 
			return TOK_END_GROUP;
		}
<lua_internal>[^\}]*  {
			_vo && fprintf(stderr, "<trigger_op>:TOK_SCRIPT('%s')\n", yytext);
			yylval->token.pstr = yylval->token.string;
			strcpy(yylval->token.string, yytext);
			_vo && fprintf(stderr, "<trigger_op>:TOK_SCRIPT.cp('%s'), sizeof(string)=%d\n", yylval->token.string, sizeof(yylval->token.string));
			return TOK_SCRIPT;
		}
<*>.		{/* silently ignore*/; } 
<*>\n		{/* silently ignore*/; } 
.			{/* silently ignore*/; } 

%%

#if 0
main()
{
	YYSTYPE	_yylval;
	void 	*myscanner = NULL;
	yylex_init(&myscanner);
	yylex(&_yylval, &myscanner); 
	/*
		yylex_destroy(&myscanner);
	*/
}
#endif
void yyerror(YYLTYPE *l, site_context_t *context, char *msg)
{
//	fprintf(stderr, "YYERROR:'%s (%d,%d:%d,%d)'\n", msg, l->first_line, l->first_column, l->last_line, l->last_column);
	TRACE((T_WARN, "YYERROR:'%s (%d,%d:%d,%d)'\n", msg, l->first_line, l->first_column, l->last_line, l->last_column));
}

void *yyalloc (yy_size_t  size , yyscan_t yyscanner)
{
    return SCX_MALLOC(size);
}

void *yyrealloc  (void * ptr, yy_size_t  size , yyscan_t yyscanner)
{
    /* The cast to (char *) in the following accommodates both
     * implementations that use char* generic pointers, and those
     * that use void* generic pointers.  It works with the latter
     * because both ANSI C and C++ allow castless assignment from
     * any pointer type to void*, and deal with argument conversions
     * as though doing an assignment.
     */
    return SCX_REALLOC(ptr, size);
}

void yyfree (void * ptr , yyscan_t yyscanner)
{
	SCX_FREE(ptr);   /* see yyrealloc() for (char *) cast */
}

