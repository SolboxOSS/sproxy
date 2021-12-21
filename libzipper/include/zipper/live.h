
#ifndef zipper_live_h
#define zipper_live_h

#include "rtmp.h"
#include "liverpc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LIVE_TSCALE                             1000
#define LIVE_SEGMENT_RANGE_TOTAL_DEFAULT        30000   /// 30 sec
#define LIVE_CONFIG_BOX                         MAKEFOURCC('z','l','c','f')
    
    
enum {
    
    zipper_live_event_none      = 0,
    zipper_live_event_connect,      // Broadcaster와 연결이 완료되었음.
    zipper_live_event_publish,      // 라이브 스트림이 생성 완료되었음.
    zipper_live_event_mediaUpdate,  // 라이브 미디어 정보가 업데이트 되었음.
    zipper_live_event_delete,       // 라이브 스트림이 종료(EOS)되었음.
};

// 라이브 핸들러의 이벤트를 처리한다.
//
// context : 라이브 Handler
// evnt : 이벤트 코드 (zipper_live_event_ enum 참조)
// param : 사용자 데이터
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
typedef int (*zipper_live_handler_event) (void *handler, int evnt, void *param);

// Broadcaster에게 패킷을 송신한다.
//
// context : 라이브 Handler
// packet : 전송할 패킷 데이터
// size : 패킷 데이터의 크기(bytes)
// param : 사용자 데이터
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
typedef int (*zipper_live_handler_send) (void *handler, unsigned char *packet, size_t size, void *param);
  
typedef struct _zipper_live_callback
{
    zipper_live_handler_event   event;
    zipper_live_handler_send    send;
    
    void *param; // 콜백으로 전달될 사용자 파라미터
        
} zipper_live_callback;
  
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
typedef struct _zipper_live_url_param
{
    char    *name;
    char    *value;
    
    struct _zipper_live_url_param *next;
    
} zipper_live_url_param;

typedef struct _live_segment_buffer
{
    struct {
        
        uint8_t *p;
        uint32_t size;
        
    } buf;

    struct {
        
        frame64     *array;
        uint32_t    size;
        
    } frms;
    
} live_segment_buffer;

typedef struct _segment_range
{
    uint32_t    start;
    
    uint32_t    last:1;
    uint32_t    fcnt:31;
    
} segment_range;

typedef struct _live_replace_context
{
    uint64_t    start;
    void        *bldr;

    uint32_t    expired:1;
    uint32_t    offset:15;
    uint32_t    count:16;
    
    uint32_t    endseq;
    uint32_t    duration;
    
    uint64_t    jitter;
    
    struct _live_replace_context *prev;
    struct _live_replace_context *next;
    
    volatile sig_atomic_t   ref;
    
} live_replace_context;

typedef struct _live_ext_inf
{
    uint8_t     dur;
    uint8_t     dis;
    uint16_t    resv;
    
} live_ext_inf;

typedef struct _bldr_live_segment_list
{
    uint32_t                dis:1;
    uint32_t                sublen:5;
    uint32_t                dur:26;
    
    union {
    
        struct {
            
            uint16_t    offset;
            uint16_t    count;
            
        } ins;

        zlIndex                 live;
        
    } index;
    
    uint64_t                pts;
    
    live_replace_context            *replace;
    struct _bldr_live_segment_list  *next;
    
} bldr_live_segment;

typedef struct _bldr_live_segment
{
    uint32_t seq;
    
    struct {
        
        uint32_t max;
        uint32_t total;
        
    } dur;
    
    struct {
        
        bldr_live_segment *first;
        bldr_live_segment *last;
        uint32_t cc;
        
    } list;
    
    struct {
        
        bldr_live_segment *first;
        bldr_live_segment *last;
        uint32_t cc;
        
    } pool;
    
} bldr_live_segment_list;
    
typedef struct _live_replace_event
{
    uint64_t    et;
    uint64_t    tt;
    
    char        *desc;
    uint32_t    length;
    
    struct _live_replace_event *next;
    
} live_replace_event;
    
typedef struct _zipper_live_handler
{
    void *livectx;
        
    zipper_live_callback callback;
    
    struct {
      
        char *group;
        char *media;
        
        zipper_live_url_param   *param;
        
    } url;
    
    struct {
        
        uint8_t     *p;
        uint32_t    size;
        uint32_t    offset;
        
    } buf;
    
    struct {
        
        uint64_t sessionId;
        
        struct {
            
            uint64_t track[5];
            
            struct {
                
                uint64_t base;
                uint64_t offset;
                
            } recal;
            
        } pts;
        
        uint8_t     vgon:1;
        uint8_t     step:7;
        uint8_t     achk:2;
        uint8_t     vchk:2;
        uint8_t     protocol:4;
        uint16_t    resv;
        
    } stat;
    
    union {
        
        zipper_live_stat_rtmp   *rtmp;
        
    } prt;
    
} zipper_live_handler;
    
typedef struct _live_context
{
    zipper_live_cache_callback cache;

    struct {
        
        live_segment *lastload;
        int res;
        
    } recovery;
    
    struct {
        
        struct {
            
            char    *group;
            char    *media;
            
        } url;

        mp4_track_desc  audio;
        mp4_track_desc  video;
        
        uint64_t uuid;
        
    } info;
    
    struct {
        
        struct {
            
            int         soc;
            uint16_t    port;
            
            struct {
                
                uint8_t     *p;
                uint16_t    offset;
                
                uint8_t     *es;
                uint32_t    essize;
                
            } buf;
            
            struct {
                
                uint8_t     pat:4;
                uint8_t     aud:4;
                uint8_t     vid:4;
                uint8_t     pcr:4;
                
                uint64_t    patt;
                
            } mux;
            
        } fwd;
        
        struct {
            
            zlIndex                 index;
            strmLockCtx             lock;
            live_segment_buffer     buf;
            
            live_segment    *first;
            live_segment    *last;
            live_segment    *end;
            live_segment    *far;
            
            struct {
                
                struct {
                
                    uint32_t    total;
                    
                } cur;
                
                struct {
                    
                    uint32_t    total;
                    
                } cfg;
                
            } range;
            
            struct {
                
                live_segment    *first;
                live_segment    *last;
                uint32_t        cc;
                
            } pool;
            
        } segment;
        
        uint64_t sys;
        uint64_t pcr[5];                    
        
    } stat;
    
    struct {
        
        live_replace_event  *first;
        live_replace_event  *last;
        
        uint32_t            length;
        pthread_mutex_t     mutex;
        uint64_t            lt;
        
    } replace;
    
} live_context;

typedef struct _live_segmentlist_attr
{
    zlIndex    first;       // 요청 구간의 첫번째 인덱스
    zlIndex    last;        // 요청 구간의 마지막 인덱스
    zlIndex    firstest;    // 전체의 첫번째 인덱스
    zlIndex    lastest;     // 전체의 마지막 인덱스

    uint32_t    cc;         // 세그먼트 카운트
    uint32_t    nextdur;    // 다음 세그먼트의 예상 duration (msec)
    
    uint64_t    uuid;
    
} __attribute__((packed, aligned(4))) live_segmentlist_attr; // sizeof() == 36

int alloc_url_param(zipper_io_handle *io_handle, void *handler, const char *url);
void live_avflag_update(zipper_io_handle *io_handle, void *context, uint8_t avflag, uint32_t audBandwidth, uint32_t vidBandwidth, uint16_t width, uint16_t height);
int live_media_update(zipper_io_handle *io_handle, void *context, uint8_t track, uint8_t codec, void *cfgblock, size_t cfgsize, void *cfgblock2, size_t cfgsize2, uint32_t opt);
void add_live_frame(zipper_io_handle *io_handle, void *context, char track, int64_t dts, int32_t cts, uint8_t *buf, uint32_t size, char key);
void release_live_stream(zipper_io_handle *io_handle, void *context, char all);
char update_live_track(zipper_io_handle *io_handle, void *track, void *matrix, void *child, uint8_t **rcvbuf, uint32_t *rcvsize);
int get_live_segment(zipper_io_handle *io_handle, void *context, zlIndex index, uint32_t offset, unsigned char *buf, uint32_t size, off_t *pos);
void free_bldr_live_segment(zipper_io_handle *io_handle, bldr_live_segment *segment);
void write_config_segment(zipper_io_handle *io_handle, void *context);
int check_media_update(zipper_io_handle *io_handle, void *handler);
void live_update_uuid(zipper_io_handle *io_handle, void *context, uint8_t track, char lock);
    
#ifdef __cplusplus
}
#endif

#endif
