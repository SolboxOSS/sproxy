
#ifndef zipper_livesvr_h
#define zipper_livesvr_h

#include "zipper.h"
#include <stdint.h>
#include <netinet/in.h>

typedef struct _live_server_context* liveSvr;

#define SVR_PROTOCOL_HLS    0
#define SVR_PROTOCOL_DASH   1
#define SVR_PROTOCOL_DASHT  2

#define LIVESVR_SESSION_TIMEOUT         20000000LL  // 20 sec
#define LIVESVR_CLIENT_BUFSIZE          65536

typedef struct _live_server_ad_content
{
    zipperCnt   ctx;
    int         fp;
    
    struct _live_server_ad_content *next;
    
} live_server_ad_content;

typedef struct _live_server_ad
{
    char        *desc;
    
    zipperBldr          bldr;
    uint32_t            ref;
    
    live_server_ad_content  *contents;
    
    struct _live_server_ad     *prev;
    struct _live_server_ad     *next;
    
} live_server_ad;

typedef struct _live_server_bldr
{
    char        *session;
    uint64_t    stime;
    
    zipperBldr      ctx;
    zipperRngGet    rng;
    zipperCnt       rpc;
    
    struct _live_server_stream      *stream;
    
    struct _live_server_bldr *prev;
    struct _live_server_bldr *next;
    
    pthread_mutex_t     mutex;
    
} live_server_bldr;

typedef struct _live_stream_bldr
{
    live_server_bldr            *bldr;
    
    struct _live_stream_bldr    *prev;
    struct _live_stream_bldr    *next;
    
} live_stream_bldr;

typedef struct _live_server_stream
{
    zipperCnt   ctx;    // live context
    //zipperCnt   rpc;    // liverpc context
    liveSvr     svr;
    
    uint16_t    tport;
    live_stream_bldr    *sbldrs;
    
    struct _live_client_context     *broadcaster;

    struct _live_server_stream      *prev;
    struct _live_server_stream      *next;
    
} live_server_stream;

typedef struct _live_client_context
{
    struct {
        
        uint8_t     stop;
        uint8_t     where;
        pthread_t   thread;
        
        int soc;
        struct sockaddr_in addr;
        
        liveSvr svr;
        
    } wrkthread;
    
    struct {
        
        zipperLiveHandler   lh;
        
        union {
            
            live_server_stream  *stream;
            live_server_bldr    *bldr;
            
        } ctx;
        
    } handler;
    
    struct {
        
        uint8_t type;
        uint32_t index;
        uint32_t offset;
        
    } req;
    
    struct _live_client_context *prev;
    struct _live_client_context *next;
    
} live_client_context;

typedef struct _live_server_thread
{
    uint8_t     stop;
    uint8_t     where;
    pthread_t   thread;

    liveSvr svr;
    
} live_server_thread;

typedef struct _live_server_context
{
    const char      *cache_dir;
    
    uint16_t        port[3];
    int             soc[3];
    
    uint16_t        tbase;
    
    struct {
        
        uint8_t     protocol;
        uint32_t    sdur;
        
    } outformat;
    
    struct {
        
        uint32_t    dur;
        uint32_t    len;
        
    } range;
    
    zipper_io_handle        *io;
    int                     tmpfp;
    
    struct {
        
        live_server_stream      *head;
        live_server_stream      *tail;
        uint32_t                cc;
        
        zipperLiveRpcMngr       rpcm;
        pthread_mutex_t         mutex;
        
    } streams;
    
    struct {
        
        live_server_bldr    *head;
        live_server_bldr    *tail;
        uint32_t            cc;
        
        pthread_mutex_t     mutex;
        
    } bldrs;
    
    struct {
        
        live_server_ad      *head;
        live_server_ad      *tail;
        uint32_t            cc;
        
        pthread_mutex_t     mutex;
        
    } ads;

    live_server_thread  svrthread[2];
    live_server_thread  gbgthread;
    
    struct {
        
        live_client_context *head;
        live_client_context *tail;
        
        pthread_mutex_t     mutex;
        
    } clients[2];
    
} live_server_context;

void create_live_server(liveSvr *svr, zipper_io_handle *io);
void destroy_live_server(liveSvr *svr);

void set_server_segment_range(liveSvr svr, uint32_t dur, uint32_t len);
void set_server_port(liveSvr svr, uint16_t input, uint16_t output, uint16_t ctrl);
void start_live_server(liveSvr svr, uint32_t segment_duration, uint8_t protocol, const char *cache_dir, uint16_t tport);

void check_dir_and_create(const char *dir);
int do_mkdir(const char *path, mode_t mode);


#endif
