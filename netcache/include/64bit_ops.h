#ifndef __64bit_ops_H__


#if 0


#ifndef WIN32
#define __64OPT_H__
#include "../asmlib.h"
#define	strcpy	A_strcpy
#define	memcpy	A_memcpy
#define	memmove	A_memmove
#define	memset	A_memset
#define	strcat	A_strcat
#define	strlen	A_strlen


char *A_strcpy(char *dest, const char *src);
void *A_memmove(void *dest, const void *src, size_t n);
void *A_memcpy(void *dest, const void *src, size_t n);
void *A_memset(void *s, int c, size_t n);
char *A_strcat(char *dest, const char *src);
size_t A_strlen(const char *s);
#endif


#endif

#endif /* __64bit_ops_H__*/
