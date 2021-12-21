#ifndef __VSTRING_H__
#define  __VSTRING_H__

#include "reqpool.h"

#if 0
#pragma pack(4)
typedef 	unsigned long vs_uint32_t;
typedef struct vstring_tag {
	vs_uint32_t 	asize:16;  /* 할당 크기 */
	vs_uint32_t 	ssize:16; /* 스트링 길이*/
	unsigned char 	*data;
} vstring_t;
#pragma pack()
#ifndef TRUE
#define 	TRUE 	1
#endif
#ifndef FALSE
#define 	FALSE 	0
#endif

#ifndef min
#define	min(a,b)	(a>b?b:a)
#endif
#ifndef max
#define	max(a,b)	(a>b?a:b)
#endif


#define 	vs_data(_v) 		((_v)->data)
#define 	vs_length(_v) 		((_v)->ssize)
#define 	vs_remained(_v) 	((_v)->asize - (_v)->ssize)
#define 	vs_offset(_v, _o) 	(char *)((_v)->data + _o)
#endif

#define 	DIRECTION_LEFT	0
#define 	DIRECTION_RIGHT	1

vstring_t * vs_clone_new(mem_pool_t *mp, char *data);
vstring_t * vs_allocate(mem_pool_t *pool, size_t n, const char *data, int needclone);
vstring_t * vs_cat(vstring_t *ptr, const char *data);
int vs_sprintf(vstring_t *ptr, const char *fmt, ...);	/* 구현 안됨 */
char * vs_strstr_lg(vstring_t *v, const char *string);
void vs_truncate(vstring_t *v, off_t off);
void vs_update_length(vstring_t *v, ssize_t n);
vstring_t * vs_strdup_lg(mem_pool_t *mp, const char *data);
vstring_t * vs_strdup(mem_pool_t *mp, vstring_t *ptr);
char * vs_strcasestr_lg(vstring_t *ptr, const char *string);
char * vs_strchr_lg(vstring_t *v, int c);
int vs_strncasecmp_lg(vstring_t *ptr, const char *str, size_t n);
vstring_t * vs_strndup_lg(mem_pool_t *mp, char *data, size_t n);
int vs_strcasecmp_lg(vstring_t *v,const char *str) ;
char * vs_strcat_lg(vstring_t *v, const char *str);
int vs_strcasecmp(vstring_t *v1, vstring_t *v2);
char * vs_strcat(vstring_t *v, vstring_t *ptr);
int vs_strncasecmp_offset_lg(vstring_t *v, off_t o, const char *str, size_t n);
off_t vs_pickup_token(mem_pool_t *mp, vstring_t *v, off_t off, const char *sep, vstring_t **vtoken);
off_t vs_pickup_token_r(mem_pool_t *mp, vstring_t *v, off_t off, const char *sep, vstring_t **vtoken, int direction);
vstring_t * vs_strndup(mem_pool_t *mp, vstring_t *v, size_t n);
int vs_strcmp_lg(vstring_t *v, const char *str);
int vs_str_menipulate(mem_pool_t *mp, vstring_t *v, vstring_t **res_v, off_t start_off, off_t end_off);
void vs_trim_string(vstring_t *v);
void vs_tolower_string(vstring_t *v);
#endif
