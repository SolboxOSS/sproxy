
#ifndef rtmp_h
#define rtmp_h

#include "io.h"
#include "zipper.h"
#include "solCMAF.h"
#include "zlive.h"

#ifdef __cplusplus
extern "C" {
#endif

static const char *zipper_rtmp_version = "v1.1.38";
    
typedef struct _zipper_rtmp_context* zipperRtmp;                // RTMP 컨텍스트
typedef struct _zipper_rtmp_pull_context *zipperRtmpPull;       // RTMP Pull 컨텍스트
    
typedef enum {

    zipper_rtmp_source_none = 0,
    zipper_rtmp_source_bldr,        // 빌드 컨텍스트 기반 VOD
    zipper_rtmp_source_solCMAF,     // solCMAF 라이브 소스
    zipper_rtmp_source_vodctx,      // 미디어 컨텍스트 기반 VOD
    
} zipper_rtmp_source_type;
    
typedef struct _zipper_rtmp_source {
    
    zipper_rtmp_source_type type; // 소스 타입 (zipper_rtmp_source_type 참조)
    uint8_t bldflag; // 트랙 출력 비트 플래그 (BLDFLAG_INCLUDE_AUDIO | BLDFLAG_INCLUDE_VIDEO | BLDFLAG_INCLUDE_ALL)
    
    union {
    
        zipperBldr bldr;        // zipper 빌드 컨텍스트 소스
        zipperCnt vodctx;       // zipper 미디어 컨텍스트 소스
        solCMAF cmaf;    // zipper RTMP Pull 컨텍스트 소스(라이브)
        
    } src;
    
    struct {
        
        uint8_t  video;
        uint8_t  audio;
        uint16_t resv;
        
    } track;
        
} zipper_rtmp_source;
    
#define RTMP_URL_MAX_PARAM  10
  
typedef struct _rtmpUrl
{
    struct {
    
        zstr src;
        zstr host;      // 호스트
        zstr app;       // 경로
        zstr path;      // 파일명
        
        struct {
            
            zstr name;  // 파라미터 이름
            zstr value; // 파리미터 값
            
        } param[RTMP_URL_MAX_PARAM]; // GET 파라미터 (최대 10개까지)
        
    } playUrl; // 재생 요청 URL
    
    zstr swfUrl; // SWF URL
    zstr pageUrl; // Reference URL
    
} rtmpUrl;
    
// zipperRtmp 소스 설정 콜백
// rtmp : [in] RTMP 컨텍스트
// url : [in] 요청 URL
// src : [out] 소스 설정 구조체
// param : [in] 사용자 파라미터
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
typedef int (*zipper_rtmp_setSource)(zipperRtmp rtmp, rtmpUrl *url, zipper_rtmp_source *src, void *param);
    
typedef enum {
    
    zipper_rtmp_call_handshake = 0,
    zipper_rtmp_call_connect,
    zipper_rtmp_call_createStream,
    zipper_rtmp_call_play,
    zipper_rtmp_call_pause,
    zipper_rtmp_call_pause_raw,
    zipper_rtmp_call_seek,
    zipper_rtmp_call_getStreamLength,
    zipper_rtmp_call_closeStream,
    zipper_rtmp_call_close,
    zipper_rtmp_call_releaseStream,
    zipper_rtmp_call_end,
    
} zipper_rtmp_call;

typedef int (*zipper_rtmp_beforeCall) (zipperRtmp rtmp, zipper_rtmp_call call, uint32_t callarg, void *param);
    
typedef struct _zipper_create_rtmp_param
{
    struct {
        
        zipper_rtmp_setSource src;  // 소스 설정 콜백 (zipper_rtmp_setSource 참조)
        zipper_rtmp_beforeCall call; // RTMP RPC 호출 콜백
        void *param;                // 사용자 파라미터
        
    } callback;
    
    struct {

        uint32_t rc:1;                  // 1이면, 전송 Rate Control 동작(VOD)
        uint32_t fcts:13;               // 강제 CTS 적용(ms)
        uint32_t chunksize:18;          // RTMP 패킷 단위 전송 크기(최대값 > 4096), bytes 기본값은 4096
        
    } config;
    
} zipper_create_rtmp_param;
    
// RTMP 컨텍스트를 생성한다.
// io_handle : I/O 핸들러
// rtmp : [out] 생성된 context
// param : [in] RTMP 컨텍스트 설정 구조체
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_create_rtmp_context(zipper_io_handle *io_handle, zipperRtmp *rtmp, zipper_create_rtmp_param *param);

// RTMP 컨텍스트를 해제한다.
// io_handle : I/O 핸들러
// rtmp : [in] RTMP 컨텍스트
void zipper_free_rtmp_context(zipper_io_handle *io_handle, zipperRtmp *rtmp);

// RTMP 요청 패킷을 처리한다.
// io_handle : I/O 핸들러
// rtmp : [in] RTMP 컨텍스트
// inbuf : [in] 수신된 패킷 데이터
// inbuf_size : [in] 수신된 패킷 데이터의 크기
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
//
// 처리 과정에서 송신할 패킷이 있으면 zipper_io_handle.write.fp 콜백 함수를 호출한다.
// 수신된 패킷 데이터가 없더라도 (다음 패킷을) 송신할 준비가 되면 inbuf=NULL, inbuf_size=0으로 호출하여야 한다.
// zipper_err_need_more를 리턴하면 현재는 보낼 데이터가 없지만 향후에 보낼 수 있음을 의미하며, 다시 zipper_rtmp_process를 호출해야 한다.
// zipper_err_eof를 리턴하면 더 이상 보낼 데이터가 없음을 의미하며, 연결을 종료 처리한다.
int zipper_rtmp_process(zipper_io_handle *io_handle, zipperRtmp rtmp, uint8_t *inbuf, size_t inbuf_size);

// RTMP RPC 함수명 문자열을 리턴한다.
// call : RPC(zipper_rtmp_call enumerate 참조)
//
const char *zipper_rtmp_call_string(zipper_rtmp_call call);
    
///////// RTMP Pull 컨텍스트 관련 ///////////////////////////////////
    
// zipperRtmpPull 만료 통지 콜백
// rtmp : [in] RTMP Pull 컨텍스트
// reason : [in] 만료 원인(오류) 코드
// param : [in] 사용자 파라미터
//
// 해당 콜백 내에서 RTMP Pull 컨텍스트 해제(zipper_free_rtmp_pull_context)도 가능하다.
typedef void (*zipper_rtmp_pull_expired)(zipperRtmpPull rtmp, int reason, void *param);

typedef struct _zipper_create_rtmp_pull_param {
    
    struct {

        zipper_rtmp_pull_expired expired;
        void *param;
        
    } callback;
    
    struct {
        
        int interval;   // 재접속 시도 간격(초, 0~)
        int max;        // 최대 접속 시도 수 (0이면 무제한)
        
    } retry;
    
    struct {
        
        const char *master; // Master RTMP URL
        const char *slave;  // Slave RTMP URL
        
    } url;
    
} zipper_create_rtmp_pull_param;

// RTMP Pull 컨텍스트를 생성한다.
// io_handle : I/O 핸들러
// rtmp : [out] 생성된 context
// param : [in] RTMP Pull 컨텍스트 설정 구조체
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
//
// 별도의 시작 함수 호출 없이 생성 시 바로 Pulling을 시작한다.
int zipper_create_rtmp_pull_context(zipper_io_handle *io_handle, zipperRtmpPull *pull, zipper_create_rtmp_pull_param *param);

// RTMP Pull 컨텍스트를 해제한다.
// io_handle : I/O 핸들러
// rtmp : [in] RTMP Pull 컨텍스트
void zipper_free_rtmp_pull_context(zipper_io_handle *io_handle, zipperRtmpPull *pull);

// RTMP Pulling을 시작(재게)한다.
// rtmp : [in] RTMP Pull 컨텍스트
int zipper_rtmp_pull_start(zipperRtmpPull *pull);
   

    
#ifdef __cplusplus
}
#endif


#endif /* rtmp_h */
