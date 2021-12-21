#ifndef __SCX_UTIL_H__
#define __SCX_UTIL_H__

#include <containers.h> /* c container library */

#define USE_NC_ALLOCATOR 0
#define USE_MEM_TRACE	 1
#define USE_MEM_LIST_CNT 0

#define SCX_MALLOC(x)              scx_malloc((x), 0, __FILE__, __LINE__)
#define SCX_CALLOC(x,y)            scx_calloc((x),(y), __FILE__, __LINE__)
#define SCX_REALLOC(x,y)           scx_realloc((x),(y), __FILE__, __LINE__)
#define SCX_FREE(x)             { if (x) scx_free((x), __FILE__, __LINE__);(x)=NULL ; }
#define SCX_STRDUP(x)              scx_strdup((x), __FILE__, __LINE__)
#define SCX_STRNDUP(x,_n)          scx_strndup((x),(_n), __FILE__, __LINE__)

#define 	ATOMIC_ADD(_i, _v) 	__sync_add_and_fetch(&(_i), (_v))
#define 	ATOMIC_SUB(_i, _v) 	__sync_sub_and_fetch(&(_i), (_v))
#define 	ATOMIC_VAL(_i) 		__sync_sub_and_fetch(&(_i), 0)
#define 	ATOMIC_SET(_i, _v)	__sync_val_compare_and_swap (&(_i), __sync_sub_and_fetch(&(_i), 0), (_v))

#define BASE64_ENCODED_LENGTH(len)  ((((len) + 2) / 3) * 4)
#define BASE64_DECODED_LENGTH(len)  ((((len) + 3) / 4) * 3)

void scx_add_dm(int asz, int sz);
void scx_sub_dm(int asz, int sz);
void *scx_malloc(size_t n, int clear, const char *sf, int sl);
void *scx_calloc(size_t elcount, size_t elsize, const char *sf, int sl);
void *scx_realloc(void * old_, size_t reqsiz, const char *sf, int sl);
void scx_free(void *p, const char *sf, int lno);
char *scx_strdup(const char *s, const char *sf, int sl);
char *scx_strndup(const char *s, size_t n, const char *sf, int sl);
void *scx_dict_malloc(size_t size);
void scx_dict_free(void *buf);
void set_resource_limits();
int is_valid_ip_address(char *ip);
double scx_get_cached_time_usec();
time_t scx_get_cached_time_sec();
double scx_update_cached_time_usec();
time_t scx_update_cached_time_sec();
double sx_get_time();
void scx_used_memory_list();
int scx_mkdir(const char *dir);
int scx_check_dir(const char *dir);

void hex2ascii(char *dest, char *src, int max);
char *ascii2hex(char *dest, char *src, int len);
size_t scx_url_decode(char *str);
size_t scx_url_decode_e(char *str);

int scx_base64_decode(char *decoded, const char* src);
int scx_encode_base64(char *dst, char *src);
int scx_encode_base64_url(char *dst, char *src);

int scx_check_url_encoding(char *url);
int scx_check_local_url_encoding(char *url);
int scx_get_cache_size();

void scx_init_proctitle (int argc, char **argv);
void scx_set_proctitle (const char *prog, const char *txt);


unsigned int scx_mix(unsigned int a, unsigned int b, unsigned int c);
unsigned int scx_hash(const void *p);

int  scx_snprintf(char *buf, size_t size, const char *format, ...);
int scx_remove_slash (char *str);
int scx_is_valid_number(char * string);

void *ccl_malloc(size_t size);
void ccl_free(void *buf);
void *ccl_realloc(void *buf, size_t size);
void *ccl_calloc (size_t nmemb, size_t size);
extern ContainerAllocator iCCLMalloc;
int scx_compress_gzip(nc_request_t *req, unsigned char *in_buf, int in_buf_size, void *out_buf, int out_buf_size);



#endif /* __SCX_UTIL_H__ */
