
#ifndef __zipper__liverpc__
#define __zipper__liverpc__

#include <stdint.h>
#include <pthread.h>
#include "mp4.h"
#include "io.h"
#include "strmlock.h"

#ifdef __cplusplus
extern "C" {
#endif

// 라이브 세그먼트 인덱스
typedef uint32_t zlIndex;

// 라이브 RPC 핸들러 (콜백) ///////////////////////////////////////////////////////////////////////////
    
// 원격지의 라이브 Context에 데이터를 요청한다.
// io_handle : I/O 핸들러, buf 파라미터에 메모리 (재)할당 시 사용한다.
// context : 라이브 RPC 컨텍스트
// query : 요청 파라미터 문자열
// buf : 데이터를 복사할 버퍼, 버퍼의 크기(*size)보다 데이터 크기가 크면, "io_handle"을 사용하여 (재)할당 해주어야 한다.
// size : 데이터를 복사받을 버퍼의 현재 할당 크기, (재)할당 시 갱신해 주어야 한다.
// param : 사용자 지정 데이터
//
// 성공 시 데이터의 크기(bytes)를, 실패 시(혹은 데이터가 없을 경우) 0을 리턴한다.
//
// * 받은 데이터를 buf에 복사할 경우 아래의 "copy_rpc_buffer()" 함수를 사용하길 권장한다.
typedef uint32_t (*zipper_liverpc_get) (zipper_io_handle *io_handle, void *context, char *query, unsigned char **buf, uint32_t *size, void *param);

// 라이브 Context로부터 미디어 삽입 정보를 전달받았을 때 호출된다.
// context : 라이브 RPC 컨텍스트
// replace : 미디어 삽입 정보
// tt : 삽입 기준 시간(msec 단위 시스템 절대 시간)
// param : 사용자 지정 데이터
typedef int (*zipper_liverpc_relace) (void *context, const char *replace, uint64_t tt, void *param);
    
// 데이터를 RPC 버퍼에 복사한다.
// io_handle : I/O 핸들러 (zipper_liverpc_get() 함수의 io_handle 전달인자)
// rcvdata : 받은 데이터 버퍼
// rcvsize : 받은 데이터의 크기
// rpcbuf : 복사해 줄 버퍼 포인터 (zipper_liverpc_get() 함수의 buf 전달인자)
// rpcbufsize : 복사해 줄 버퍼의 할당 크기 (zipper_liverpc_get() 함수의 size 전달인자)
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int copy_rpc_buffer(zipper_io_handle* io_handle, unsigned char *rcvdata, uint32_t rcvsize, unsigned char **rpcbuf, uint32_t *rpcbufsize);
    
// 라이브 세그먼트 캐시를 저장한다.
//
// context : 라이브 Context
// index : 세그먼트 인덱스
// first : 첫번째 세그먼트 데이터 블럭
// fsize : 첫번째 세그먼트 데이터 블럭의 크기
// second : 두번째 세그먼트 데이터 블럭
// ssize : 두번째 세그먼트 데이터 블럭의 크기
// third : 세번째 세그먼트 데이터 블럭
// tsize : 세번째 세그먼트 데이터 블럭의 크기
// param : 사용자 데이터
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
typedef int (*zipper_live_cache_write) (void *context, zlIndex index, unsigned char *first, size_t fsize, unsigned char *second, size_t ssize, unsigned char *third, size_t tsize, void *param);

// 라이브 세그먼트 캐시를 읽어온다.
//
// context : 라이브 Context
// index : 세그먼트 인덱스
// offset : 세그먼트 데이터 블럭의 위치
// block : 세그먼트 데이터 출력 버퍼
// size : 세그먼트 데이터 블럭의 크기
// param : 사용자 데이터
//
// 성공 시 읽은 바이트수를, 실패 시 0을 리턴한다.
//
// [주의]
// 만약, zipper_io_handle.bufsize가 0이 아닐 경우, bufsize 단위로 읽기를 요청하므로
// 세그먼트 데이터 파일의 범위를 벗어날 수 있다.
typedef uint32_t (*zipper_live_cache_read) (void *context, zlIndex index, uint32_t offset, unsigned char *block, uint32_t size, void *param);
 
// 라이브 세그먼트 캐시를 삭제한다.
//
// context : 라이브 Context
// index : 세그먼트 인덱스
// reason : 삭제 코드
// param : 사용자 데이터
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
typedef int (*zipper_live_cache_remove) (void *context, zlIndex index, void *param);

typedef  struct _zipper_live_cache_callback
{
    zipper_live_cache_read      read;
    zipper_live_cache_write     write;
    zipper_live_cache_remove    remove;
    
    void *param; // 콜백으로 전달될 사용자 파라미터
    
} zipper_live_cache_callback;
 
typedef struct _live_rpc
{
    zipper_liverpc_get      get;
    zipper_liverpc_relace   replace;
    void *param;
        
} live_rpc;
    
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
#define RPC_QUERY_STRLEN                24     //(4 * (((sizeof(live_rpc_query)=16) + 2) / 3))
#define SEGMENT_FAR_INTERVAL            30000LL // 30 min
#define DEFAULT_GETLIST_DURATION        12000000LL
typedef enum {
    
    live_rpc_none = 0,
    live_rpc_cfg,
    live_rpc_list,
    live_rpc_segment,
        
} live_rpc_cmd;
    
typedef struct _live_rpc_query
{
    uint8_t cmd;
    uint8_t resv8;
    uint16_t resv16;
    
    zlIndex index;
    uint64_t uuid;
    
} __attribute__((packed, aligned(4))) live_rpc_query;
    
void enc_rpc_query(live_rpc_query *query, char *outstr);
void dec_rpc_query(char *instr, live_rpc_query *query);
    
typedef struct _live_segment_attr
{
    zlIndex     index;          // 세그먼트 인덱스
    
    uint32_t    mfb:12;         // 최대 ES 버퍼 (요구) 크기 (내부)
    uint32_t    duration:20;    // 세그먼트 duration (msec)
    
    uint64_t    pts;            // 세그먼트 기준 pts (내부 clock)
    
} __attribute__((packed, aligned(4))) live_segment_attr;

typedef struct _live_segment_attrEx
{
    zlIndex     index;
    
    uint32_t    mfb:12;
    uint32_t    duration:20;
    
    uint64_t    pts;
    
    uint32_t    cc;
    uint32_t    offset;
    
} __attribute__((packed, aligned(4))) live_segment_attrEx;
    
typedef struct _live_segment {
    
    live_segment_attrEx attr;
    uint64_t uuid;
    
    struct {

        struct _live_segment *side;
        struct _live_segment *far;
        
    } next;
    
} __attribute__((packed, aligned(4))) live_segment;


#define ZIPPER_BUILD_UPDATE_INTERVAL    400000LL // 400 msec
    
typedef struct _liverpc_media_desc
{
    char    group[512];
    char    media[512];
    
    mp4_track_desc audio;
    mp4_track_desc video;

    uint64_t uuid;
    uint32_t duration;
    
    uint8_t *cfgblock;
    
} liverpc_media_desc;
    
typedef struct _liverpc_context
{
    zipper_live_cache_callback cache;
    
    struct {
        
        strmLockCtx lock;
        uint64_t    tt;
        
    } update;
    
    struct {
        
        live_rpc    rpc;
        
        struct {
            
            uint8_t     *p;
            uint32_t    size;
            
        } buf;
        
        uint64_t    let;
        
    } rpc;
    
    liverpc_media_desc mdesc;
    
    struct {
        
        struct {
            
            live_segment    *first;
            live_segment    *last;
            live_segment    *far;
            
            zlIndex         li;
            
        } list;
        
        struct {
            
            live_segment    *first;
            live_segment    *last;
            uint32_t        cc;
            
        } pool;
        
    } segment;
    
    volatile sig_atomic_t ref;
    volatile sig_atomic_t uq;
    
} liverpc_context;
    
typedef struct _liverpcm_worker
{
    uint8_t stop;
    pthread_t thread;
    void *mngr;
    
    struct _liverpcm_worker *next;
    
} liverpcm_worker;
    
typedef struct _liverpcm_rpcctx_node
{
    void *ctx;
    uint64_t    lastreq;

    struct _liverpcm_rpcctx_node *prev;
    struct _liverpcm_rpcctx_node *next;
        
} liverpcm_rpcctx_node;
    
typedef struct _liverpcm_job
{
    void *ctx;
    struct _liverpcm_job *next;
        
} liverpcm_job;
    
typedef struct _zipper_liverpc_manager
{
    struct {
        
        zipper_io_handle mem;
                
    } res;
    
    struct {
        
        liverpcm_job *first;
        liverpcm_job *last;
        
        struct {
            
            liverpcm_job *first;
            liverpcm_job *last;

        } pool;
        
        uint32_t cc;
        
        liverpcm_worker foreman;
        pthread_mutex_t mutex;
        
    } jqueue;
    
    struct {

        liverpcm_rpcctx_node *head;
        liverpcm_rpcctx_node *tail;
        uint32_t cc;
        
    } rpcs;

    struct {
        
        liverpcm_worker *first;
        liverpcm_worker *last;
        pthread_mutex_t mutex;
        uint32_t cc;
        
    } workers;
    
} zipper_liverpc_manager;

live_segment *search_live_segment(char rpc, char shift, uint64_t val, live_segment *first, live_segment *last);
void free_live_segment(zipper_io_handle *io_handle, live_segment *segment, void *context, zipper_live_cache_remove rmfunc, void *param);
   
uint32_t get_liverpc_list(zipper_io_handle *io_handle, liverpc_context *context, char shift, uint64_t uuid, uint64_t start, uint64_t end, unsigned char **buf, uint32_t *size);
    
#ifdef __cplusplus
}
#endif

#endif /* defined(__zipper__liverpc__) */
