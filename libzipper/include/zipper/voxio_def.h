
#ifndef voxio_def_h
#define voxio_def_h

#include <unistd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//#define DEBUG_LOG
//#define DEBUG_ANALYSIS_PARSE
//#define DEBUG_ANALYSIS_INDEX
//#define DEBUG_ANALYSIS_PROTOCOL
//#define DEBUG_ANALYSIS_INTERLEAVED
//#define USE_AUDIO_CTTS
//#define SUPPORT_NOT_INTERLEAVED

#ifndef YES
#define YES		1
#endif

#ifndef NO
#define NO		0
#endif
   
#ifndef BOOL
#define BOOL char
#endif

#ifndef THREAD_RUNNING
#define THREAD_RUNNING  0
#endif

#ifndef THREAD_STOPPED
#define THREAD_STOPPED  2
#endif

#ifndef SOCKET_ERROR
#define SOCKET_ERROR    -1
#endif

#ifndef INVALID_HANDLE
#define INVALID_HANDLE	-1
#endif

#if defined(DEBUG_LOG) || defined(DEBUG_ANALYSIS_PARSE) || defined(DEBUG_ANALYSIS_INTERLEAVED)
void vxlog(const char *format, ...);
unsigned long long vxnow();
#else
#define vxlog(format, args...) ((void)0)
#endif

size_t sendx(int soc, void *buf, size_t len);
size_t writex(int fp, void *buf, size_t len);
size_t readx(int fp, void *buf, size_t len);
uint64_t systime();
void dump_packet(void *buf, size_t len, uint8_t index, char append);

static inline uint64_t abs_u64(uint64_t a, uint64_t b)
{
    return a > b ? a - b : b - a;
}

#ifdef __cplusplus
}
#endif
    
#endif
