#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"


/*
 * 	data == NULL: x+1 크기만큼 할당
 *  data != NULL: x+1 크기만큼 할당 후, need_clone=1이면 data copy
 */
vstring_t *
vs_allocate(mem_pool_t *mp, size_t x, const char *data, int needclone)
{
	vstring_t 	*v = NULL;
	char 		*end;

	if (data) {
		if (needclone) {
#if 1
			v = mp_alloc(mp, sizeof(vstring_t) + x+2);
			if (!v) return NULL;
			v->data  = (char *)((char *)v + sizeof(vstring_t));
			strncpy(v->data, data, x);
#else
			v = mp_alloc(mp, sizeof(vstring_t));
			v->data  = mp_alloc(mp, x+10);
			strncpy(v->data, data, x);
#endif
			

			v->data[x+1] = 0;
			v->asize = x + 1;
			//v->ssize = strlen(data);
			v->ssize = strlen(v->data);
		}
		else {
			v = mp_alloc(mp, sizeof(vstring_t));
			v->data  = (unsigned char *)data;
			v->asize = 0;
			v->ssize = strlen(data); /* 1바이트는 null 가정*/
		}
	}
	else {
		v = mp_alloc(mp, sizeof(vstring_t) + x + 1);
		if (!v) return NULL;
		v->data  = (char *)v + sizeof(vstring_t);
		v->asize = x + 1;
		v->ssize = 0;
	}
	return v;
}
vstring_t * 
vs_clone_new(mem_pool_t *mp, char *data)
{

	return vs_allocate(mp, strlen(data), data, TRUE);
}
char * 
vs_strstr_lg(vstring_t *v, const char *string)
{
	return strstr(v->data, string);
}
char * 
vs_strcasestr_lg(vstring_t *v, const char *string)
{
	return strcasestr(v->data, string);
}
int
vs_strcmp_lg(vstring_t *v, const char *str)
{
	return strcmp(v->data, str);
}
int
vs_strncasecmp_lg(vstring_t *v, const char *str, size_t n)
{
	return strncasecmp(v->data, str, min(v->ssize, n));
}
int
vs_strncasecmp_offset_lg(vstring_t *v, off_t o, const char *str, size_t n)
{
	return strncasecmp(v->data+o, str, min(v->ssize, n));
}

char * 
vs_strstr(vstring_t *v, vstring_t *string)
{
	return strstr(v->data, string->data);
}
char * 
vs_strcasestr(vstring_t *v, vstring_t *string)
{
	return strcasestr(v->data, string->data);
}
int
vs_strcmp(vstring_t *v, vstring_t *str)
{
	return strcmp(v->data, str->data);
}
vstring_t *
vs_strdup(mem_pool_t *mp, vstring_t *ptr)
{
	return vs_allocate(mp, ptr->ssize, ptr->data, TRUE);
}
vstring_t *
vs_strdup_lg(mem_pool_t *mp, const char *data)
{
	return vs_allocate(mp, strlen(data), data, TRUE);
}
size_t
vs_strlen(vstring_t *v)
{
	return v->ssize;
}

vstring_t *
vs_strndup_lg(mem_pool_t *mp, char *data, size_t n)
{
	vstring_t 	*v = NULL;
	v = vs_allocate(mp, n, data, TRUE);
	return v;
}
vstring_t *
vs_strndup(mem_pool_t *mp, vstring_t *v, size_t n)
{
	vstring_t 	*nv = NULL;
	nv = vs_allocate(mp, n, vs_data(v), TRUE);
	return nv;
}
void
vs_truncate(vstring_t *v, off_t off)
{
	v->data[off] = '\0';
	v->ssize = min(off, v->ssize);
}
void
vs_update_length(vstring_t *v, ssize_t n)
{
	if (n == (ssize_t)-1)
		v->ssize = strlen(v->data);
	else
		v->ssize = n;
}
char *
vs_strchr_lg(vstring_t *v, int c)
{
	return strchr((const char *)v->data, c);
}
int
vs_strcasecmp_lg(vstring_t *v,const char *str) 
{
	return strcasecmp((const char *)v->data, str);
}

char *
vs_strcat_lg(vstring_t *v, const char *str)
{
	strcat(v->data + v->ssize, str);
	vs_update_length(v, v->ssize + strlen(str));
	return v->data;
}
char *
vs_strcat(vstring_t *v, vstring_t *ptr)
{
	memcpy(v->data+v->ssize, ptr->data, ptr->ssize+1/*null 포함*/);
	vs_update_length(v, v->ssize + ptr->ssize);
	return v->data;
}
int
vs_strcasecmp(vstring_t *v1, vstring_t *v2)
{
	return strncasecmp(vs_data(v1), vs_data(v2), min(v1->ssize, v2->ssize));
}
#define 	VS_CHAR(_v, _off) 	(vs_data(_v) + _off)[0]
off_t
vs_pickup_token(mem_pool_t *mp, vstring_t *v, off_t off, const char *sep, vstring_t **vtoken)
{
	char 	*sp = NULL;
	char 	*ep = NULL;
	off_t 	woff = off;

	if (*vtoken) *vtoken = NULL;

	/* skip white space */
	if (vs_length(v) <= woff) {
		return woff;
	}
	while (strchr(" \t", VS_CHAR(v, woff))) woff++;

	if (vs_length(v) <= woff) {
		return woff;
	}

	if (VS_CHAR(v, woff)) {
		sp = vs_data(v) + woff;

		/* 
		 * buffer 끝이거나 sep 문자열 중의 하나를 만날 때까지 woff 증가
		 */
		while (VS_CHAR(v, woff) != 0 && strchr(sep, VS_CHAR(v, woff)) == NULL) woff++;
		ep = vs_data(v) + woff;
		woff++; 
		if (sp != ep) {
			*vtoken = vs_strndup_lg(mp, sp, (size_t)(ep - sp));
		}
	}
	return woff;
}

/*
 * 지정된 separator를 마지막 부분 부터 검색을 해서
 * direction에 지정된 데로 앞/뒤의 값을 리턴한다.
 * mp가 NULL인 경우는  찾아낸 offset만 반환한다.
 * 지정된 seperator를 찾지 못한 경우는 0을 반환 한다.
 */
off_t
vs_pickup_token_r(mem_pool_t *mp, vstring_t *v, off_t off, const char *sep, vstring_t **vtoken, int direction)
{
	unsigned char 	*sp = NULL;
	unsigned char 	*ep = NULL;
	off_t 	woff = off;
	size_t  nsize = 0;
	if (*vtoken) *vtoken = NULL;

	/* skip white space */
	if (vs_length(v) < woff) {
		return 0;
	}
	while (strchr(" \t", VS_CHAR(v, woff))) woff--;

	if ( 0 >= woff) {
		return 0;
	}

	if (VS_CHAR(v, woff)) {
		ep = vs_data(v) + woff;

		/*
		 * buffer 시작이거나 sep 문자열 중의 하나를 만날 때까지 woff 감소
		 */
		while (woff > 0 && strchr(sep, VS_CHAR(v, woff)) == NULL) woff--;
		if(mp == NULL) {
			return woff;
		}
		sp = vs_data(v) + woff;
		//woff--;
		if (sp != ep) {
			//*vtoken = vs_strndup_lg(mp, sp+1, (size_t)(ep - sp));
			//발견된 token 기준으로 좌우 문자열을 리턴할지 판단한다.
			if(direction == DIRECTION_RIGHT){
				nsize = (size_t)(ep - sp);
				*vtoken = vs_strndup_lg(mp,  sp+1,nsize);
			}
			else {
				nsize =  (size_t)(sp - vs_data(v));
				*vtoken = vs_strndup_lg(mp,  vs_data(v),nsize);
			}
		}
	}
	return woff;
}

/*
 * 입력된 string의 중간 부분만을 삭제 하는 동작을 한다.
 * end_off이 0인 경우는 start_off의 앞부분만을 사용한다.
 */
int
vs_str_menipulate(mem_pool_t *mp, vstring_t *v, vstring_t **res_v, off_t start_off, off_t end_off)
{
	size_t  nsize = 0;	//할당될 string의 크기
	int left_len = start_off;
	/*
	 * v가 '/lava.mp4/playlist.m3u8?test=1'인 경우
	 * start_off에는 9가  end_off에는 23이 들어간다.
	 */

	if (*res_v) *res_v = NULL;
	//string의 크기를 계산한다.
	if(end_off==0){
		nsize = start_off;
	}
	else {
		nsize = vs_length(v) - (end_off - start_off);
//		right_len = vs_length(v) - end_off + 1;
	}
	//string을 할당 후에
	//앞 뒤의 string을 붙인다.
	*res_v =  vs_allocate(mp, nsize, NULL, TRUE);
	if(!*res_v) return 0;
	strncpy(vs_data(*res_v), vs_data(v), left_len);
	(*res_v)->data[left_len] = 0;
	(*res_v)->ssize = left_len;
	if(end_off){
		vs_strcat_lg(*res_v, vs_data(v) + end_off);
	}

	return nsize;
}

/*
 * 문자열 앞/뒤의 공백을 제거한다.
 */
void
vs_trim_string(vstring_t *v)
{
		unsigned char    *end;
        int		pos = 0;
        if (NULL == v) return;
        if (isspace(*v->data))	/* 문자열의 앞쪽에 공백이 있으면 제거 */
        {
        	while (isspace(*(v->data+pos)) && pos < (v->ssize-1) ) pos++;
        	memmove(v->data, v->data+pos, v->ssize-pos);
        	*(v->data+v->ssize-pos) = '\0';
        	v->ssize = strlen(v->data);
        }
        /* 문자열의 뒷쪽에 공백이 있으면 제거 */
        end = v->data + strlen(v->data) -1;
        while (end > v->data && isspace(*end)) end--;
        if((v->data + strlen(v->data) -1) >  end ) {
			*(end+1) = '\0';
			v->ssize = strlen(v->data);
        }
        return ;
}

/*
 * 문자열을 소문자로 변환한다.
 */
void
vs_tolower_string(vstring_t *v)
{
		unsigned char    *end;
        end = v->data + strlen(v->data) -1;
        while (end > v->data ) {
        	*end = tolower(*end);
        	end--;
        }
        return ;
}

