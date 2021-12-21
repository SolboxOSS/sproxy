#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include <sys/resource.h>
#include <errno.h>
#include <sys/stat.h>
#include <zlib.h>
//#include <ncapi.h>
//#include <trace.h>
#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "scx_util.h"

volatile long long int     	g__scx_dynamic_memory_sum = 0LL;	//전체 할당량
volatile long long int		g__scx_adynamic_memory_sum = 0LL;	//g__scx_dynamic_memory_sum + __scx_meminfo_t 헤더 크기


ContainerAllocator iCCLMalloc = { ccl_malloc, ccl_free, ccl_realloc, ccl_calloc };

#pragma pack(0)
typedef struct {
	uint64_t 	magic;
	uint64_t 	length;
#if USE_MEM_LIST_CNT
	int				line;
#endif
	uint64_t 	*barrier; /* 할당된 메모리가 깨졌는지 체크할 때 사용 */
} __scx_meminfo_t;
#pragma pack()

#if USE_MEM_LIST_CNT
typedef struct {
	uint64_t	count;
	uint64_t	size;
	char 		file[64];
} __scx_memtrace_t;
volatile __scx_memtrace_t	__scx_line_count[10000] = {0};
#endif

#define 	__SCX_MAGIC 			0x5AA5B33B
#define 	__SCX_MAGIC_FREE 		0xB33B5AA5
#define		__SCX_MAGIC_SIZE 		(sizeof(__scx_meminfo_t) + sizeof(long long))
#define      SCX_ALLOC_ALIGN_UNIT	(sizeof(long long))
#define      SCX_ALLOC_ALIGN(_n)   	(((_n)+(SCX_ALLOC_ALIGN_UNIT-1)) & (~(SCX_ALLOC_ALIGN_UNIT-1)))
extern vstring_t *gscx__core_file_path;	//core dump file이 생성될 위치를 지정한다

static inline void
__scx_set_barrier(__scx_meminfo_t *mp)
{
	mp->barrier = (long long *)((char *)mp + sizeof(__scx_meminfo_t) + SCX_ALLOC_ALIGN(mp->length));
	(*mp->barrier) = ~__SCX_MAGIC;
}
static inline int
__scx_check_overflow(__scx_meminfo_t *mp)
{

	return (*mp->barrier == ~__SCX_MAGIC);
}


static inline void *
__scx_set_info(void *p, unsigned int l, const char *sf, int sl)
{
	__scx_meminfo_t 	*mp = (__scx_meminfo_t *)p;
	mp->magic = __SCX_MAGIC;
	mp->length = l;
#if USE_MEM_LIST_CNT
	mp->line = sl;
#endif
	__scx_set_barrier(mp);
	return (void *)((char *)p + sizeof(__scx_meminfo_t));
}

static inline unsigned int
__scx_set_free(void *p)
{
	__scx_meminfo_t 	*mp = (__scx_meminfo_t *)((char *)p - sizeof(__scx_meminfo_t));
	mp->magic = __SCX_MAGIC_FREE;
	return mp->length;
}

static inline int
__scx_check_magic(void *p)
{
	__scx_meminfo_t 	*mp = (__scx_meminfo_t *)((char *)p - sizeof(__scx_meminfo_t));
	if (mp->magic != __SCX_MAGIC) {
		if (__SCX_MAGIC_FREE == mp->magic || 0 == mp->magic ) {
			TRACE((T_ERROR, "address %p, seems double free\n", p));
		}
		else {
			TRACE((T_ERROR, "address %p, corrupt at topmost area of the allocated memory block.\n", p));
		}
		ASSERT(0);

	}
	if (!__scx_check_overflow(mp)) {
		TRACE((T_ERROR, "address %p, corrupt beyond the bottom area of the allocated memory block.\n", p));
		ASSERT(0);
	}
//	return (mp->magic == __SCX_MAGIC);
	return 1;
}
static unsigned int
scx_get_len(void *p)
{
	__scx_meminfo_t 	*mp = (__scx_meminfo_t *)((char *)p - sizeof(__scx_meminfo_t));
	if (mp->magic != __SCX_MAGIC) abort();
	return mp->length;
}

#if USE_MEM_LIST_CNT
static unsigned int
scx_get_line(void *p)
{
	__scx_meminfo_t 	*mp = (__scx_meminfo_t *)((char *)p - sizeof(__scx_meminfo_t));
	if (mp->magic != __SCX_MAGIC) abort();
	return mp->line;
}
#endif

void
scx_add_dm(int asz, int sz)
{
//	ATOMIC_ADD(g__scx_dynamic_memory_sum, sz);	// 현재 사용하고 있는 부분이 없이 부하만 발생시키고 있어서 이부분은 사용하지 않는다.
	ATOMIC_ADD(g__scx_adynamic_memory_sum, asz);
}
void
scx_sub_dm(int asz, int sz)
{
//	ATOMIC_SUB(g__scx_dynamic_memory_sum, sz);	// 현재 사용하고 있는 부분이 없이 부하만 발생시키고 있어서 이부분은 사용하지 않는다.
	ATOMIC_SUB(g__scx_adynamic_memory_sum, asz);
}

/*
 * clear가 1로 설정되면 calloc(), 이외에는 malloc()로 메모리 할당
 */
void *
scx_malloc(size_t n, int clear, const char *sf, int sl)
{
#if USE_NC_ALLOCATOR
#if USE_MEM_TRACE
	int nsz = SCX_ALLOC_ALIGN(n+__SCX_MAGIC_SIZE + 1);
	__scx_meminfo_t         *mp;
	mp = (__scx_meminfo_t *)__nc_malloc(nsz, sf, sl);
	scx_add_dm(nsz, n);
#if USE_MEM_LIST_CNT
	if (sl < 10000) {
		if (ATOMIC_ADD(__scx_line_count[sl].count, 1) == 1)
			snprintf(__scx_line_count[sl].file, 64, "%s", sf);
		ATOMIC_ADD(__scx_line_count[sl].size, n);
	}
#endif	/* end of USE_MEM_LIST_CNT */
	return  __scx_set_info((void *)mp, n, sf, sl);
#else
	return __nc_malloc(n, sf, sl);
#endif	/* end of USE_MEM_TRACE */
#else
	int                     nsz = SCX_ALLOC_ALIGN(n+__SCX_MAGIC_SIZE + 1);
	__scx_meminfo_t         *mp;
	if (1 == clear) {
			mp = (__scx_meminfo_t *)calloc(1, nsz);
	}
	else {
		mp = (__scx_meminfo_t *)malloc(nsz);
	}
//      TRACE((T_INFO, "****total %llu,  %s , size = %d, alloced = %d\n",  __sync_add_and_fetch(&g__scx_adynamic_memory_sum, 0), sf, n, nsz));
	ASSERT(mp);
	scx_add_dm(nsz, n);
#if USE_MEM_LIST_CNT
	if (sl < 10000) {
		if (ATOMIC_ADD(__scx_line_count[sl].count, 1) == 1)
			snprintf(__scx_line_count[sl].file, 64, "%s", sf);
		ATOMIC_ADD(__scx_line_count[sl].size, n);
	}
#endif	/* end of USE_MEM_LIST_CNT */
	return  __scx_set_info((void *)mp, n, sf, sl);
#endif	/* end of USE_NC_ALLOCATOR */

}

void *
scx_calloc(size_t elcount, size_t elsize, const char *sf, int sl)
{
	return scx_malloc(elcount*elsize, 1, sf, sl);
}

void *
scx_realloc(void * old_, size_t reqsiz, const char *sf, int sl)
{
#if USE_NC_ALLOCATOR
#if USE_MEM_TRACE
	void    *new_ = (void *)scx_malloc(reqsiz, 0, sf, sl);
	unsigned int    olds = 0;
	if (old_) {
			olds = scx_get_len(old_);
			memcpy(new_, old_, min(olds, reqsiz));
			scx_free(old_, sf, sl);
	}
	return new_;
#else
	return __nc_realloc(old_, reqsiz, sf, sl);
#endif	/* end of USE_MEM_TRACE */
#else
	void    *new_ = (void *)scx_malloc(reqsiz, 0, sf, sl);
	unsigned int    olds = 0;
	if (old_) {
			olds = scx_get_len(old_);
			memcpy(new_, old_, min(olds, reqsiz));
			scx_free(old_, sf, sl);
	}
	return new_;
#endif	/* end of USE_NC_ALLOCATOR */

}

void
scx_free(void *p, const char *sf, int lno)
{
#if USE_NC_ALLOCATOR
#if USE_MEM_TRACE
	if (p) {
		if (__scx_check_magic(p)) {
			size_t  ol = 0;
			ol = scx_get_len(p);
			scx_sub_dm(SCX_ALLOC_ALIGN(ol+__SCX_MAGIC_SIZE + 1), ol);
#if USE_MEM_LIST_CNT
			int sl = scx_get_line(p);
			if (sl < 10000) {
				ATOMIC_SUB(__scx_line_count[sl].count, 1);
				ATOMIC_SUB(__scx_line_count[sl].size, ol);
			}
#endif	/* end of USE_MEM_LIST_CNT */
			__scx_set_free(p);
			//                      TRACE((T_INFO, "****total %llu,  %s , size =%d, freed = %d\n",  __sync_add_and_fetch(&g__scx_adynamic_memory_sum, 0), sf, ol, SCX_ALLOC_ALIGN(ol+__SCX_MAGIC_SIZE + 1)));
			__nc_free((char *)p - sizeof(__scx_meminfo_t), sf, lno);
		}
		else {
			__scx_meminfo_t         *mp = (__scx_meminfo_t *)((char *)p - sizeof(__scx_meminfo_t));
			TRACE((T_ERROR, "address %p, seems double free at %d@%s(freed_magic=%d)\n", p, lno, sf, (int)(mp->magic == __SCX_MAGIC_FREE)));
			ASSERT(0);
			//TRAP;
		}
	}
#else
	__nc_free(p, sf, lno);
#endif	/* end of USE_MEM_TRACE */
#else
	if (p) {
		if (__scx_check_magic(p)) {
			size_t  ol = 0;
			ol = scx_get_len(p);
			scx_sub_dm(SCX_ALLOC_ALIGN(ol+__SCX_MAGIC_SIZE + 1), ol);
#if USE_MEM_LIST_CNT
			int sl = scx_get_line(p);
			if (sl < 10000) {
				ATOMIC_SUB(__scx_line_count[sl].count, 1);
				ATOMIC_SUB(__scx_line_count[sl].size, ol);
			}
#endif	/* end of USE_MEM_LIST_CNT */
			__scx_set_free(p);
			//                      TRACE((T_INFO, "****total %llu,  %s , size =%d, freed = %d\n",  __sync_add_and_fetch(&g__scx_adynamic_memory_sum, 0), sf, ol, SCX_ALLOC_ALIGN(ol+__SCX_MAGIC_SIZE + 1)));
			free((char *)p - sizeof(__scx_meminfo_t));
		}
		else {
			__scx_meminfo_t         *mp = (__scx_meminfo_t *)((char *)p - sizeof(__scx_meminfo_t));
			TRACE((T_ERROR, "address %p, seems double free at %d@%s(freed_magic=%d)\n", p, lno, sf, (int)(mp->magic == __SCX_MAGIC_FREE)));
			ASSERT(0);
		}
	}
#endif	/* end of USE_NC_ALLOCATOR */

}

char *
scx_strdup(const char *s, const char *sf, int sl)
{
#if USE_NC_ALLOCATOR
#if USE_MEM_TRACE
	char *n = (char *)scx_malloc(strlen(s)+1, 1, sf, sl);
	strcpy(n, s);
	return n;
#else
	return __nc_strdup(s, sf, sl);
#endif	/* end of USE_MEM_TRACE */
#else
	char *n = (char *)scx_malloc(strlen(s)+1, 1, sf, sl);
	strcpy(n, s);
	return n;
#endif	/* end of USE_NC_ALLOCATOR */

}

char *
scx_strndup(const char *s, size_t n, const char *sf, int sl)
{
#if USE_NC_ALLOCATOR
#if USE_MEM_TRACE
	char *news = (char *)scx_malloc(n+1, 1, sf, sl);
	strncpy(news, s, n);
	return news;
#else
#endif	/* end of USE_MEM_TRACE */
	return __nc_strndup(s, n, sf, sl);
#else
	char *news = (char *)scx_malloc(n+1, 1, sf, sl);
	strncpy(news, s, n);
	return news;
#endif	/* end of USE_NC_ALLOCATOR */

}

void
scx_used_memory_list()
{
#if USE_MEM_LIST_CNT
	int i = 0;
	uint64_t count = 0;
	uint64_t size = 0;
	for(i = 0; i < 10000; i++) {
		count = ATOMIC_VAL(__scx_line_count[i].count);
		size = ATOMIC_VAL(__scx_line_count[i].size);
		if(count > 0)
			TRACE((T_INFO, "source line = %d, count = %lld, size = %lld, file = %s\n", i, count, size, __scx_line_count[i].file));
	}
#endif
}

void *
scx_dict_malloc(size_t size)
{
	return SCX_CALLOC(1, size);
}

void
scx_dict_free(void *buf)
{
	SCX_FREE(buf);
}

//core 파일 on/off 설정 가능하게 변경
void
set_resource_limits()
{
    struct rlimit rlim;
#if 0
 	 //현재 netcache core의 nc_init()에서 core dump를 막고 있기 때문에 이부분이 동작하지 않는다.
    getrlimit(RLIMIT_CORE, &rlim);
	if(gscx__core_file_path)
	{
		rlim.rlim_cur = rlim.rlim_max = RLIM_INFINITY;
		//rlim.rlim_cur = rlim.rlim_max = 1024*1024*100;
		chdir(vs_data(gscx__core_file_path));	// core file이 생성될 위치를 변경한다.
	}
	else
	{
		rlim.rlim_cur = rlim.rlim_max = 0;
	}
    if (setrlimit(RLIMIT_CORE, (const struct rlimit *)&rlim) == -1)
    {
    	TRACE((T_ERROR, "setrlimit(RLIMIT_CORE) failed(%s).\n", strerror(errno)));
        exit(0);
    }
#endif
    getrlimit(RLIMIT_NOFILE, &rlim);
    rlim.rlim_cur = rlim.rlim_max = 262144;
    if (setrlimit(RLIMIT_NOFILE, (const struct rlimit *)&rlim) == -1)
    {
    	TRACE((T_ERROR, "setrlimit(RLIMIT_NOFILE) failed(%s).\n",strerror(errno)));
//        exit(0);
    }

#if 0
    rlim.rlim_cur = rlim.rlim_max = 8388608 * 2;
    if (setrlimit(RLIMIT_STACK, (const struct rlimit *)&rlim) == -1)
    {
    	TRACE((T_ERROR, "setrlimit(RLIMIT_STACK) failed(%s).\n", strerror(errno)));
        exit(0);
    }
#endif
}

/*
 * IP Address 인지를 판별
 * IP Address인 경우 1을 리턴
 */
int
is_valid_ip_address(char *ip)
{
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ip, &(sa.sin_addr));
    return result != 0;
}

static pthread_rwlock_t __scx_clock_lock = PTHREAD_RWLOCK_INITIALIZER;
static volatile double   __scx_time_usec = 0;
static volatile time_t __scx_time_sec = 0;
double
scx_get_cached_time_usec()
{
	double 			t;
//	pthread_rwlock_rdlock(&__scx_clock_lock);
	t = __scx_time_usec;
//	pthread_rwlock_unlock(&__scx_clock_lock);
    return t;
}

time_t
scx_get_cached_time_sec()
{
	time_t			t_sec;
//	pthread_rwlock_rdlock(&__scx_clock_lock);
	t_sec = __scx_time_sec;
//	pthread_rwlock_unlock(&__scx_clock_lock);
    return t_sec;
}
/* 성능 측정을 위한 시간이외에는 가능하면 cache time을 사용하도록 해야 한다. */
double
scx_update_cached_time_usec()
{
	struct timeval 	tv;
	double 			t;
	gettimeofday(&tv, NULL);
	pthread_rwlock_wrlock(&__scx_clock_lock);
	__scx_time_usec = (tv.tv_sec) * 1000000.0 + (tv.tv_usec);
	t = __scx_time_usec;
	__scx_time_sec = tv.tv_sec;
	pthread_rwlock_unlock(&__scx_clock_lock);
	return t;
}

time_t
scx_update_cached_time_sec()
{
	struct timeval 	tv;
	time_t			t_sec;
	gettimeofday(&tv, NULL);
	pthread_rwlock_wrlock(&__scx_clock_lock);
	__scx_time_usec = (tv.tv_sec) * 1000000.0 + (tv.tv_usec);
	__scx_time_sec = tv.tv_sec;
	t_sec = __scx_time_sec;
	pthread_rwlock_unlock(&__scx_clock_lock);
	return t_sec;
}

double
sx_get_time()
{
	struct timeval 	tv;
	double 			t;

	gettimeofday(&tv, NULL);
	t = (tv.tv_sec) * 1000000.0 + (tv.tv_usec);
	return t;
}


int
scx_mkdir(const char *dir)
{
	char tmp[1024];
	char *p = NULL;
	size_t len;
	int ret = 0;
	struct stat  dirstat;

	snprintf(tmp, sizeof(tmp),"%s",dir);
	len = strlen(tmp);
	if (tmp[len - 1] == '/')
		tmp[len - 1] = 0;
	for (p = tmp + 1; *p; p++)
		if(*p == '/') {
			*p = 0;
			ret = stat(tmp, &dirstat);
			if (ret < 0) {
				ret = mkdir(tmp, S_IRWXU| S_IRWXG | S_IROTH | S_IXOTH);
				if (ret < 0) return ret;
			}
			*p = '/';
		}
	ret = mkdir(tmp, S_IRWXU| S_IRWXG| S_IROTH | S_IXOTH);
	return ret;
}

/*
 * 지정된 디렉토리를 확인해보고
 * 디렉토리가 없는 경우 새로 생성한다.
 */
int
scx_check_dir(const char *dir)
{
	int i, ret;
	struct stat  dirstat;
	ret = stat(dir, &dirstat);
	if (ret < 0) {
		ret = scx_mkdir(dir);
		if (ret < 0) {
			TRACE((T_INFO, "Failed to create directory(%s), reason(%s)\n", dir, strerror(errno)));
			return -1;
		}
		else {
			TRACE((T_INFO, "Create directory(%s)\n", dir));
		}
	}
	else if (!S_ISDIR(dirstat.st_mode)) {
		/* 디렉토리가 아닌 경우 에러를 리턴함 */
		TRACE((T_ERROR, "'%s' is not directory.\n", dir));
		return -1;
	}
	return 0;
}


void
hex2ascii(char *dest, char *src, int max)
{
      char tmp[4]={0x0};
      char **ptr=NULL,
           *buf=dest;
      *buf=0x0;
      if(*src){
          for(int i=0;src[i] && i < max;i+=2){
              strncpy(tmp,&src[i],2);
              sprintf(tmp,"%c",strtol(tmp,ptr,16) );
              *buf++=tmp[0];
          }
      }
      *buf=0x0;
}

char *
ascii2hex(char *dest, char *src, int len)
{
    static char  hex[] = "0123456789abcdef";
    while (len--) {
        *dest++ = hex[(*src & 0xf0) >> 4];
        *dest++ = hex[*src++ & 0xf];
    }
    return dest;
}


#if 0
#define IS_ALNUM(ch) \
		( ch >= 'a' && ch <= 'z' ) || \
		( ch >= 'A' && ch <= 'Z' ) || \
		( ch >= '0' && ch <= '9' ) || \
		( ch >= '-' && ch <= '.' )

static char _x2c(char hex_up, char hex_low)
{
        char digit;

        digit = 16 * (hex_up >= 'A'
                ? ((hex_up & 0xdf) - 'A') + 10 : (hex_up - '0'));
        digit += (hex_low >= 'A'
                ? ((hex_low & 0xdf) - 'A') + 10 : (hex_low - '0'));
        return (digit);
}
size_t
scx_url_decode(char *str)
{
        int i = 0, j = 0;
        int len;
        if (!str) return NULL;
        len = strlen( str );
        for (i = j = 0; j < len; i++, j++)
        {
                switch(str[j])
                {
                case '+':
                        str[i] = ' ';
                        break;

                case '%':
                		if (IS_ALNUM(str[i+1]) && IS_ALNUM(str[i+2]) && j < (len - 2)) {
							str[i] = _x2c(str[j + 1], str[j + 2]);
							j += 2;
                		}
                		else {
                			str[i] = str[j];
                		}
                        break;

                default:
                        str[i] = str[j];
                        break;
                }
        }
        str[i]= '\0';

        return strlen(str);
}
size_t
scx_url_decode_e (char *str)
{
	return scx_url_decode(char *str);
}
#else
size_t
scx_url_decode (char *str)
{
  char *rpos = str;
  char *wpos = str;
  char *end;
  unsigned int num;
  char buf3[3];

  while ('\0' != *rpos)
    {
      switch (*rpos)
        {
        case '%':
          if ( ('\0' == rpos[1]) ||
               ('\0' == rpos[2]) )
          {
            *wpos = '\0';
            return wpos - str;
          }
          buf3[0] = rpos[1];
          buf3[1] = rpos[2];
          buf3[2] = '\0';
          num = strtoul (buf3, &end, 16);
          if ('\0' == *end)
            {
              *wpos = (char)((unsigned char) num);
              wpos++;
              rpos += 3;
              break;
            }
          /* intentional fall through! */
        default:
          *wpos = *rpos;
          wpos++;
          rpos++;
        }
    }
  *wpos = '\0'; /* add 0-terminator */
  return wpos - str; /* = strlen(val) */
}

/*
 * url decoding시 일부 문자는 decoding에서 제외
 * 현재는 ?(%3F), #(%23)만 decoding에서 제외
 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/33636?
 * scx_check_url_encoding()와 pair로 동작해야 한다.
 */
size_t
scx_url_decode_e (char *str)
{
  char *rpos = str;
  char *wpos = str;
  char *end;
  unsigned int num;
  char buf3[3];

  while ('\0' != *rpos)
    {
      switch (*rpos)
        {
        case '%':
          if ( ('\0' == rpos[1]) ||
               ('\0' == rpos[2]) )
          {
            *wpos = '\0';
            return wpos - str;
          }
          else if ( ('3' == rpos[1]) &&
                  (('F' == rpos[2]) || ('f' == rpos[2])) )
          {	//?(%3F)
              *wpos = *rpos;
              wpos++;
              rpos++;
              continue;
          }
          else if ( ('2' == rpos[1]) && ('3' == rpos[2]) )
          {	//#(%23)
              *wpos = *rpos;
              wpos++;
              rpos++;
              continue;
          }
          buf3[0] = rpos[1];
          buf3[1] = rpos[2];
          buf3[2] = '\0';
          num = strtoul (buf3, &end, 16);
          if ('\0' == *end)
            {
              *wpos = (char)((unsigned char) num);
              wpos++;
              rpos += 3;
              break;
            }
          /* intentional fall through! */
        default:
          *wpos = *rpos;
          wpos++;
          rpos++;
        }
    }
  *wpos = '\0'; /* add 0-terminator */
  return wpos - str; /* = strlen(val) */
}
#endif

static const char scx_base64_chars[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char scx_base64_url_chars[] =	/* url safe base64 code */
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static const char scx_base64_digits[] = /* 표준 base64와 url safe base64를 모두 지원하도록 테이블 생성 */
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 62, 0, 62, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    0, 0, 0, -1, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0, 63, 0, 26,
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
    45, 46, 47, 48, 49, 50, 51, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };


int
scx_base64_decode_internal(char * decoded, const char* src)
{
	size_t in_len = strlen (src);
	char* dest = decoded;

	if (in_len % 4)
	{
		/* Wrong base64 string length */
		return SCX_NO;
	}
	while (*src) {
		char a = scx_base64_digits[(unsigned char)*(src++)];
		char b = scx_base64_digits[(unsigned char)*(src++)];
		char c = scx_base64_digits[(unsigned char)*(src++)];
		char d = scx_base64_digits[(unsigned char)*(src++)];
		*(dest++) = (a << 2) | ((b & 0x30) >> 4);
		if (c == (char)-1)
			break;
		*(dest++) = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == (char)-1)
			break;
		*(dest++) = ((c & 0x03) << 6) | d;
	}
	*dest = 0;
	return SCX_YES;
}

/*
 * url safe 형식도 같이 지원하기 위해서 내부에서 변환 처리를 한다.
 */
int
scx_base64_decode(char * decoded, const char* src)
{
	int z, len;
	char *input;
	int ret = 0;

	len = strlen(src);
	z = 4 - (len % 4);
	if (z > 0) {
		input = alloca(len+z+1);
		sprintf(input, "%s",src);
		if (z < 4) {
			while (z--)
				input[len++] = '=';
		}
		input[len] = '\0';
	}
	else {
		input = src;
	}
	ret = scx_base64_decode_internal(decoded, input);

	return ret;
}

int
scx_encode_base64_internal(char *dst, char *src, const char *basis)
{
    u_char         *d, *s;
    size_t          len;

    len = strlen(src);
    s = src;
    d = dst;

    while (len > 2) {
        *d++ = basis[(s[0] >> 2) & 0x3f];
        *d++ = basis[((s[0] & 3) << 4) | (s[1] >> 4)];
        *d++ = basis[((s[1] & 0x0f) << 2) | (s[2] >> 6)];
        *d++ = basis[s[2] & 0x3f];

        s += 3;
        len -= 3;
    }

    if (len) {
        *d++ = basis[(s[0] >> 2) & 0x3f];

        if (len == 1) {
            *d++ = basis[(s[0] & 3) << 4];
            *d++ = '=';

        } else {
            *d++ = basis[((s[0] & 3) << 4) | (s[1] >> 4)];
            *d++ = basis[(s[1] & 0x0f) << 2];
        }

        *d++ = '=';
    }

    *d = 0;
    return 0;
}

int
scx_encode_base64(char *dst, char *src)	/* original base64 endcoding */
{
	int ret = 0;
	ret = scx_encode_base64_internal(dst,src, scx_base64_chars);
	return ret;
}

int
scx_encode_base64_url(char *dst, char *src) /* URL safe base64 encoding */
{
	int ret = 0;
	ret = scx_encode_base64_internal(dst,src, scx_base64_url_chars);
	return ret;
}

/*
 * url에  ?, #이 두번 encoding이 된게 있는지 확인해서
 * 한번 decoding 해준다.
 * ? : %253F인경우 %3F로 변환
 * # : %2523인경우 %23으로 변환
 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/33636
 * scx_url_decode_e()와 pair로 동작
 */
int
scx_check_url_encoding(char *url)
{
	char 	*rpos = url;
	char 	*wpos = url;
    size_t 	len;
    while ('\0' != *rpos)
    {
    	if (*rpos == '%')
    	{
    		if ( ('\0' != rpos[1]) &&
    				('\0' != rpos[2]) &&
					('\0' != rpos[3]) &&
					('\0' != rpos[4]) )
    		{
    			if ( ('2' == rpos[1]) &&
    					('5' == rpos[2]) &&
						(	('3' == rpos[3]) &&
							(('F' == rpos[4]) || ('f' == rpos[4]))
						) ||
						(	('2' == rpos[3]) &&
							('3' == rpos[4])
						)
					)
    			{
    				// %253F나 %2523 경우 25를 뺀다.
    				*wpos = *rpos;
    				rpos++;
    				rpos++;
    				rpos++;
    				wpos++;
    				continue;
    			}
    		}
    	} // end of if (*rpos == '%')
    	if (*rpos != *wpos) {
    		*wpos = *rpos;
    	}
    	rpos++;
    	wpos++;

    }
    if (*wpos != '\0') {
    	*wpos = '\0';
    }
    return 0;
}

/*
 * url에  ?, #가 인코딩된 %3F와  %23가 있는지 확인해서 해당 문자만 디코딩 한다.
 * strm_file_open()에서 로컬 파일을 사용하는 경우 호출된다.
 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/33636
 * scx_url_decode_e()와 pair로 동작
 */
int
scx_check_local_url_encoding(char *url)
{
	char 	*rpos = url;
	char 	*wpos = url;
    size_t 	len;
    while ('\0' != *rpos)
    {
    	if (*rpos == '%')
    	{
    		if ( ('\0' != rpos[1]) &&
    				('\0' != rpos[2]))
    		{
    			if (	('3' == rpos[1]) &&
    					(('F' == rpos[2]) || ('f' == rpos[2]))
					) {
    				*wpos = '?';
    				rpos++;
					rpos++;
					rpos++;
					wpos++;
					continue;
    			}
    			else if (	('2' == rpos[3]) &&
							('3' == rpos[4])
					) {
    				*wpos = '#';
    				rpos++;
					rpos++;
					rpos++;
					wpos++;
					continue;
    			}

    		}
    	} // end of if (*rpos == '%')
    	if (*rpos != *wpos) {
    		*wpos = *rpos;
    	}
    	rpos++;
    	wpos++;

    }
    if (*wpos != '\0') {
    	*wpos = '\0';
    }
    return 0;
}

unsigned int
scx_mix(unsigned int a, unsigned int b, unsigned int c)
{
    a=a-b;  a=a-c;  a=a^(c >> 13);
    b=b-c;  b=b-a;  b=b^(a << 8);
    c=c-a;  c=c-b;  c=c^(b >> 13);
    a=a-b;  a=a-c;  a=a^(c >> 12);
    b=b-c;  b=b-a;  b=b^(a << 16);
    c=c-a;  c=c-b;  c=c^(b >> 5);
    a=a-b;  a=a-c;  a=a^(c >> 3);
    b=b-c;  b=b-a;  b=b^(a << 10);
    c=c-a;  c=c-b;  c=c^(b >> 15);
    return c;
}

unsigned int
scx_hash(const void *p)
{
    unsigned int hash = 2166136261U;
    for (const uint8_t *ptr = p; *ptr;) {
    hash = (hash ^ *ptr++) * 16777619U;
    }
    return hash;
}

/*
 * 메모리 cache의 크기를 전체 물리 메모리 기준으로 일정 비율로 계산한다.
 */
int
scx_get_cache_size()
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);

//    uint64_t cache_size = pages * page_size / (1024 * 1024) / 2;
    /* 전체 메모리의 40%로 계산 */
    uint64_t cache_size = pages * page_size / 10 * 4 / (1024 * 1024 ) ;

    return (int)cache_size;
}

extern char **environ;

static char **argv0;
static int argv_lth;

void
scx_init_proctitle (int argc, char **argv)
{
	int i;
	char **envp = environ;

	/*
	 * Move the environment so we can reuse the memory.
	 * (Code borrowed from sendmail.)
	 * WARNING: ugly assumptions on memory layout here;
	 *          if this ever causes problems, #undef DO_PS_FIDDLING
	 */
	for (i = 0; envp[i] != NULL; i++)
		continue;

	environ = malloc(sizeof(char *) * (i + 1));
	if (environ == NULL)
		return;

	for (i = 0; envp[i] != NULL; i++)
		if ((environ[i] = strdup(envp[i])) == NULL)
			return;
	environ[i] = NULL;

	argv0 = argv;
	if (i > 0)
		argv_lth = envp[i-1] + strlen(envp[i-1]) - argv0[0];
	else
		argv_lth = argv0[argc-1] + strlen(argv0[argc-1]) - argv0[0];
}

void
scx_set_proctitle (const char *prog, const char *txt)
{
	int i;
	const int BUFSIZE = 64;
	char buf[BUFSIZE];

	if (!argv0)
		return;

	if (strlen(prog) + strlen(txt) + 5 > BUFSIZE)
		return;

	snprintf(buf, BUFSIZE, "%s: %s", prog, txt);

	i = strlen(buf);
	if (i > argv_lth - 2) {
		i = argv_lth - 2;
		buf[i] = '\0';
	}
	memset(argv0[0], '\0', argv_lth);       /* clear the memory area */
	strcpy(argv0[0], buf);

	argv0[1] = NULL;
}

/*
 * snprintf()가 buffer size보다 써야할 문자열이 큰 경우 return값이
 * buffer에 쓰여진 크기가 아닌 써야할 문자열의 크기가 리턴되는 문제가 있어서
 * snprintf()의 리턴값을 사용하기 어려움.
 * 이 문제를 해결하기 위해 아래의 함수를 만듬
 */
int
scx_snprintf(char *buf, size_t size, const char *format, ...)
{
	int len = 0;
	va_list va;
    va_start (va, format);
    len = vsnprintf( buf, size, format, va );
    va_end (va);
    len = (len >= size) ? strlen(buf) : len;
    return len;

}

/*
 * path에 포함된 연속된 slash(/)를 한개의 slash로 만든다
 */
int
scx_remove_slash (char *str)
{
  int rindex = 0, windex = 0 ;
  char lchar = ' ' ;
  while ( str[rindex] != '\0' ) {
    if ( str[rindex] == '/' ) {
      if ( lchar != '/' )
        str[windex++] = str[rindex] ;
    } else { str[windex++] = str[rindex] ; }
    lchar = str[rindex++] ;
  }
  str[windex] = '\0' ;
  return strlen(str);
}

int
scx_is_valid_number(char * string)
{
	int len = strlen(string);
	for(int i = 0; i < len; i ++)
	{
		//ASCII value of 0 = 48, 9 = 57. So if value is outside of numeric range then fail
		//Checking for negative sign "-" could be added: ASCII value 45.
		if (string[i] < 48 || string[i] > 57)
			return 0;
	}
	return 1;
}

/* container library memory allocation function */
void *
ccl_malloc(size_t size)
{
	return SCX_CALLOC(1, size);
}

void
ccl_free(void *buf)
{
	SCX_FREE(buf);
}

void *
ccl_realloc(void *buf, size_t size)
{
	return SCX_REALLOC(buf, size);
}

void *
ccl_calloc (size_t nmemb, size_t size)
{
	return SCX_CALLOC(nmemb, size);
}




#define CHUNK 16384
#define windowBits 15
#define GZIP_ENCODING 16
/*
 * in_buff에 있는 내용을 gzip으로 압축한다.
 * 압축에 성공하는 경우 리턴값으로 압축 크기를 리턴하고
 * 실패하는 경우 -1을 리턴한다.
 */
int
scx_compress_gzip(nc_request_t *req, unsigned char *in_buf, int in_buf_size, void *out_buf, int out_buf_size)
{
	int ret = 0;
	z_stream strm;
	int in_buf_off = 0;
	int in_buf_remain = in_buf_size;
	int out_buf_off = 0;
	int out_buf_remain = out_buf_size;
	int flush;
	int avail_in;
	int have;

	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits | GZIP_ENCODING, 8, Z_DEFAULT_STRATEGY);
	if (ret != Z_OK) {
		TRACE((T_WARN, "[%llu] deflateInit2() failed. reason(%s)\n", req->id, vs_data(req->url), zError(ret)));
		return -1;
	}
	do {
		strm.next_in = in_buf + in_buf_off;
		avail_in = (in_buf_remain > CHUNK) ? CHUNK : in_buf_remain;;
		strm.avail_in = (in_buf_remain > CHUNK) ? CHUNK : in_buf_remain;
		in_buf_off += avail_in;
		in_buf_remain -= avail_in;
		flush = (in_buf_remain > 0) ? Z_NO_FLUSH : Z_FINISH;
		do {
			strm.avail_out = (out_buf_remain > CHUNK) ? CHUNK : out_buf_remain;
			strm.next_out = out_buf + out_buf_off;
			ret = deflate(&strm, flush);
			if (ret == Z_STREAM_ERROR)	{
				TRACE((T_WARN, "[%llu] deflate() failed. reason(%s)\n", req->id, vs_data(req->url), zError(ret)));
				return -1;
			}
			have = CHUNK - strm.avail_out;
			out_buf_off += have;

			//printf("in size(%d), off(%d), out size(%d), off(%d), avail_out(%d), ret = %d\n", in_buf_size, in_buf_off, out_buf_size, out_buf_off, strm.avail_out, ret);
		} while (strm.avail_out == 0);

	} while (in_buf_remain > 0);
	ret = deflateEnd(&strm);
	if (ret != Z_OK) {
		TRACE((T_WARN, "[%llu] deflateEnd() failed. reason(%s)\n", req->id, vs_data(req->url), zError(ret)));
		return -1;;
	}
	return out_buf_off;
}


