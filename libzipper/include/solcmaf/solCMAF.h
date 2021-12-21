//
//  solCMAF.h
//  solCMAF
//
//  Created by Hyungpyo Oh on 22/10/2018.
//  Copyright © 2018 Hyungpyo Oh. All rights reserved.
//

#ifndef solCMAF_h
#define solCMAF_h

#include "zipper.h"

#include <stdint.h>

#ifdef DEBUG_LOG
void dbglog(const char *format, ...);
#else
#define dbglog(format, args...) ((void)0)
#endif

typedef void *(*solCMAFIO_malloc)(size_t size);
typedef void *(*solCMAFIO_calloc)(size_t size, size_t multiply);
typedef void *(*solCMAFIO_realloc)(void *p, size_t newsize);
typedef void (*solCMAFIO_free)(void *p);

typedef size_t (*solCMAFIO_Write) (uint8_t *p, size_t size, void *param);
typedef void (*solCMAFIO_Log) (void *param, const char *fmt, ...);

// solCMAF 라이브러리 I/O 구조체 (zipper_io_handle과 유사, 간소화 버전)
typedef struct _solCMAFIO
{
    struct {
        
        solCMAFIO_malloc malloc;
        solCMAFIO_calloc calloc;
        solCMAFIO_realloc realloc;
        solCMAFIO_free free;
        
    } mem;

    struct {
        
        solCMAFIO_Write cb;
        void *param;
        
    } write;
    
    struct {
        
        solCMAFIO_Log cb;
        void *param;
        
    } log;
    
} solCMAFIO;

typedef enum {

    solCMAF_DashStream = 0,
    solCMAF_HlsStream,
    
} solCMAF_Stream;

// solCMAF 러이브러리 컨텍스트
typedef struct _solCMAFCtx* solCMAF;

enum {
    
    solCMAF_DisconnectBy_close = 0,
    solCMAF_DisconnectBy_open,                  // 연결 실패
    solCMAF_DisconnectBy_SendHeader,            // 요청 해더 송신 실패,
    solCMAF_DisconnectBy_ReceiveHeader,         // 응답 헤더 수신 실패,
    solCMAF_DisconnectBy_Reject,                // 서버 거부 (* 이 경우 이벤트 콜백의 tb는 응답 코드)
    solCMAF_DisconnectBy_ReceiveBody,           // 응답 데이터 수신 실패
    solCMAF_DisconnectBy_RecevieChunkHeader,    // 응답 Chunk 헤더 수신 혹은 파싱 실패
    solCMAF_DisconnectBy_RecevieChunkBody,      // 응답 Chunk 데이터 수신 실패
};

// solCMAF 이벤트
typedef enum {
    
    solCMAF_Event_Connected = 0,                // 연결됨
    solCMAF_Event_Disconnected,                 // 연결이 끊어짐
    solCMAF_Event_Expired,                      // 라이브러리 연결이 만료됨.
    solCMAF_Event_RepresentationAdded,          // Representation(Adaptive)가 추가됨
    solCMAF_event_BeforeRepresentationRemoved,  // Representation(Adaptive)가 제외됨
    solCMAF_Event_AddChunk,                     // CMAF Chunk가 추가됨.
    solCMAF_Event_Ready,                        // solCMAF_build* 함수 준비 완료 (호출 가능)
    solCMAF_Event_Switch,                       // 현재 연결 소스 변경 시
    solCMAF_Event_Error                         // 오류 발생
    
} solCMAF_Event;

// solCMAF 미디어 정보
typedef struct _solCMAFMediaInfo
{
    char        *idstr;
    
    uint32_t    codec:8;
    uint32_t    tscale:24;
    uint32_t    trk:4;
    uint32_t    bandwidth:28;
    
    struct {
        
        uint8_t     *chunk;
        uint16_t    size;
        uint16_t    cfgoff;
        uint16_t    cfgsize;
        
    } stsd;
    
    struct {
        
        uint8_t *p;
        uint32_t size;
        
    } cfgs[32];

    union {
        
        struct {
            
            uint16_t width;
            uint16_t height;
            float fps;
            
        } video;
        
        struct {
            
            uint32_t ch:8;
            uint32_t freq:24;
            
        } audio;
        
    } attr;
    
} solCMAFMediaInfo;

typedef struct _solCMAFFrame
{
    frame_header fh;
    
    uint8_t *p;
    uint64_t dts;
    uint64_t gts;
    
    uint32_t expired:1;
    uint32_t alloced:31;
    
    struct _solCMAFFrame *prev;
    struct _solCMAFFrame *next;
    
} solCMAFFrame;

// 서브 트랙(매니패스트) 구분 인덱스 (Switching Set, Selection Set 통합)
typedef uint8_t cmafAdaptiveIndex;

// 서브 트랙의 구성 가능 수는 최대 64(1~64, 0은 마스터)개로 제한한다.
#define cmafAdaptiveIndexMax            65
#define cmafAdaptiveIndexMaster         0
#define cmafAdaptiveIndexBase           1

/*
 * solCMAF 이벤트 콜백
 *
 * @param
 * ctx[in] : 대상 solCMAF 라이브러리 컨텍스트
 * ev[in] : 이벤트 타입
 * adapt[in] : 이벤트의 대상 서브 트랙 구분 인덱스
 * what[in] :
 *
 *      solCMAF_Event_AddChunk: 이벤트의 대상 Fragment 인덱스
 *      solCMAF_Event_Error: 오류 코드
 *      solCMAF_Event_Disconnected: 원인 코드 ( *solCMAF_DisconnectBy_ enum 참조)
  *
 * tb[in] : 이벤트가 solCMAF_Event_AddChunk인 경우 이벤트의 대상 Fragment Base 시간, solCMAF_Event_Disconnected인 경우 errno
 * length[in] : solCMAF_Event_AddChunk 전용 파라미터, 이벤트의 대상 Fragment의 현재 크기
 * param[in] : 콜백 사용자 파라미터
 *
 * @return
 * 성공 시 0을, 그렇지 않으면 오류 코드를 리턴한다. 오류 코드를 리턴한면 solCMAF은 모든 동작 및 연결을 종료한다.
 *
 */
typedef int (*solCMAF_event_callback)(solCMAF ctx, solCMAF_Event ev, cmafAdaptiveIndex adapt, uint32_t what, uint64_t tb, uint32_t length, void *param);

/*
 * solCMAF 라이브러리 설정 구조체
 * (solCMAF_create_ctx 호출 시 사용)
 */
typedef struct _solCMAFCreateParam
{
    // 소스 연결 설정
    struct {
        
        time_t timeout;             // 소켓 타임아웃(초, 0이면 무시)
        uint32_t retry;               // 소켓 통신 실패 시 재시도 간격(밀리세컨드, 0~)
        
    } pull;
    
    struct {
        
        uint16_t manifest;  // 매니패스트 상에 기술된 Fragment의 최대 수
        uint16_t prefetch;  // 초기 로딩 시에 요청할 과거 Fragment 수 (0~)
        uint32_t cache;     // 메모리 상에 캐시될 Fragment의 최대 수
        
    } stack;
    
    struct {
    
        uint8_t mpd:1;  // always YES(1)
        uint8_t fmp4:1; // always YES(1)
        uint8_t m3u8:1; // HLS 매니패스트, 현재 미구현
        uint8_t ts:1;   // MPEG2-TS Segment, 현재 미구현
        uint8_t rtmp:1; // 설정 시 지원
        uint8_t resv:3;
        
    } support;
    
    struct {
        
        uint32_t rpidx:1; // DASH Representation ID를 Adaptive Index로 대체 사용
        uint32_t always_use_list:1; // 매니패스트에 항상 세그먼트 리스트 표시
        uint32_t resv:30;
        
    } opt;
    
    // 이벤트 콜백 설정
    struct {
        
        solCMAF_event_callback cb; // 이벤트 콜백 함수
        void *param;               // 이벤트 콜백 함수 사용자 파라미터
        
    } event;
    
} solCMAFCreateParam;

/*
 * solCMAF 러이브러리 컨텍스트 생성 함수
 *
 * @param
 * io[in] : solCMAF I/O 핸들러 구조체 (메모리 관련)
 * param[in] : 컨텍스트 생성 설정 구조체
 * ctx[out] : solCMAF 라이브러리 컨텍스트
 *
 * @return
 * 성공 시 0을, 실패 시 오류 코드를 리턴한다.
 */
int solCMAF_create(solCMAFIO *io, solCMAFCreateParam *param, solCMAF *ctx);

/*
 * solCMAF 러이브러리 컨텍스트 해제 함수
 *
 * @param
 * io[in] : solCMAF I/O 핸들러 구조체 (메모리 관련)
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 */
void solCMAF_free(solCMAFIO *io, solCMAF *ctx);

/*
 * solCMAF 러이브러리 소스 URL 추가 함수
 *
 * @param
 * io[in]: solCMAF I/O 핸들러 구조체 (메모리 관련)
 * ctx[in]: solCMAF 라이브러리 컨텍스트
 * url[in]: 추가할 소스 URL
 * master[in]: true(1)이면 마스터로 취급, 그렇지 않으면 슬레이브로 취급
 *
 * @return
 * 성공 시 0을, 그렇지 않으면 오류 코드를 리턴한다.
 *
 * @remark
 * master 설정과 별개로 첫번째 추가하는 소스 URL은 마스터로 취급되며, 이후 master 설정 시 변경된다.
 */
int solCMAF_addSource(solCMAFIO *io, solCMAF ctx, const char *url, char master);

/*
 * solCMAF 러이브러리 소스 URL 리셋 함수
 *
 * @param
 * io[in]: solCMAF I/O 핸들러 구조체 (메모리 관련)
 * ctx[in]: solCMAF 라이브러리 컨텍스트
 */
void solCMAF_resetSource(solCMAFIO *io, solCMAF ctx);

/*
 * 소스 서버(origin) 연결 함수
 *
 * @param
 * io[in] : solCMAF I/O 핸들러 구조체 (메모리 관련)
 * ctx[in] : solCMAF 라이브러리 컨텍스트
  * block[in] : 1이면 연결이 완료될 때까지 동기로 진행된다(함수 내부에서 blocking).
 * timeout[in] : block이 1인 경우 최대 대기 시간(초)
 *
 * @return
 * 성공 시 0을, 그렇지 않으면 오류 코드를 리턴한다.
 *
 * @remark
 * 기본적인 연결 처리는 비동기로 진행되며, 별도의 이벤트(solCMAF_Event_connected)를 연결 완료를 통보받을 수 있다.
 * 연결 완료의 시점은 각 서브 트랙의 CMAF Chunk를 하나 이상 수신한 경우이다.
 */
int solCMAF_connect(solCMAFIO *io, solCMAF ctx, char block, time_t timeout);

/*
 * 소스 서버(origin) 연결 종료 함수
 *
 * @param
 * io[in] : solCMAF I/O 핸들러 구조체 (메모리 관련)
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 */
void solCMAF_close(solCMAFIO *io, solCMAF ctx);

/*
 * solCMAF 매니패스트 경로 설정 구조체
 * (solCMAF_build_manifest 호출 시 사용)
 */
typedef struct _solCMAFManifestPath {
    
    /* 매니패스트 상에 기술된 각 구성 요소의 경로 포맷
     * 포맷에는 다음의 매크로 문자열을 사용할 수 있다.
     *
     * %a : cmafAdaptiveIndex (각 서브 트랙의 타입과 관계없이 단위 증가 값)
     * %r : cmafRepresentationId (각 서브 트랙의 Representation ID)
     * %t : Fragment Base 시간 (DASH 매니패스트 상에서는 $Time$으로 대체)
     * %i : Fragment 인덱스 (DASH 매니패스트 상에서는 $Number$로 대체)
     * %s : Fragment의 트랙 타입 ('video', 'audio', 'text',...)
     *
     */

    const char *baseUrl;    // 매니패스트의 절대 경로
    const char *manifest;   // * 서브 매니패스트 경로 (CMAF HLS 전용)
    const char *init;       // 초기화 파일 경로
    const char *fragment;   // 세그먼트 파일 경로
    const char *tunnel;     // solCMAF 터널 경로
    char *muxbuf;           // 빌드 출력에 필요한 문자열 버퍼 (NULL로 지정 시, 내부 임시 버퍼 자체 생성, 사용)
    
} solCMAFManifestPath;


/*
 * 대상 트랙 인덱스로부터 Representation ID를 구한다.
 *
 */
const char *solCMAF_RepresentationID(solCMAF ctx, cmafAdaptiveIndex adapt);

/*
 * 대상 Representation ID로부터 Representation ID를 구한다.
 *
 * @remark
 * 만약 찾을 수 없는 경우에는 cmafAdaptiveIndexMaster(0)을 리턴한다.
 */
cmafAdaptiveIndex solCMAF_AdaptiveIndex(solCMAF ctx, const char *representation);

/*
 * 매니패스트 출력 함수
 *
 * @param
 * io[in] : solCMAF I/O 핸들러 구조체 (메모리 및 출력(write) 관련)
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 * stream[in] : 매니패스트 포맷
 * adapt[in] : 대상 매니패스트 (0은 마스터)
 * path[in] : 매니패스트의 하위 요소 경로 포맷
 * calsize[in] : 1이면 라이브러리 내부에서 캐싱하고 크기 계산만 진행한다.
 *
 * @return
 * 성공 시 매니패스트의 전체 크기(bytes)를, 그렇지 않으면 0을 리턴한다.
 *
 * @remark
 * 출력은 I/O 핸들러 write 콜백을 통해 진행된다(calsize = 0인 경우).
 * 호출 전 solCMAF_stat 함수를 통해 solCMAFStat.progress.lmt 값을 확인하고 요청 헤더와 비교하여 변경되지 않은 경우에는 304로 응답하고, 변경된 경우에만 정상(200) 응답 처리할 수 있다.
 */
size_t solCMAF_build_manifest(solCMAFIO *io, solCMAF ctx, solCMAF_Stream stream, cmafAdaptiveIndex adapt, solCMAFManifestPath *path, char calsize);

/*
 * 트랙 Init 출력 함수
 *
 * @param
 * io[in] : solCMAF I/O 핸들러 구조체 (메모리 및 출력(write) 관련)
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 * adapt[in] : 대상 트랙 (1~)
 * calsize[in] : 1이면 라이브러리 내부에서 캐싱하고 크기 계산만 진행한다.
 *
 * @return
 * 성공 시 Init 파일의 전체 크기(bytes)를, 그렇지 않으면 0을 리턴한다.
 *
 * @remark
 * 출력은 I/O 핸들러 write 콜백을 통해 진행된다(calsize = 0인 경우).
 */
size_t solCMAF_build_init(solCMAFIO *io, solCMAF ctx, cmafAdaptiveIndex adapt, char calsize);

typedef struct _solCMAFFragment *scFragment;
typedef uint32_t cmafSeqIdx;

/*
 * 트랙 Fragment 액세스 함수
 *
 * @param
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 * adapt[in] : 대상 트랙 (1~)
 * seq[in] : 대상 Fragment 인덱스 (HLS의 경우 sequence number, DASH의 경우 $Number$ 값에 대응)
 * tb[in] : 대상 Fragment Base 시간(DASH의 $Time$ 값에 대응, 90000Hz)
 *
 * @return
 * 성공 시 대상 Fragment 핸들을 리턴한다. 존재하지 않으면 NULL을 리턴한다.
 *
 * @remark
 * 리턴된 Fragment 핸들은 solCMAF_release_fragment 함수를 통해 반드시 해제해 주어야 한다.
 */
scFragment solCMAF_get_fragment(solCMAF ctx, cmafAdaptiveIndex adapt, cmafSeqIdx seq, uint64_t tb);

/*
 * 트랙 Fragment의 seq 번호 구하기 함수
 *
 * @param
 * fragment[in] : 대상 트랙 Fragment
 */
uint32_t solCMAF_segOf_fragment(scFragment fragment);

/*
 * 트랙 Fragment의 기준 타임스탬프 구하기 함수
 *
 * @param
 * fragment[in] : 대상 트랙 Fragment
 */
uint64_t solCMAF_timebaseOf_fragment(scFragment fragment);

/*
 * 터널의 시작 Fragment 액세스 함수
 *
 * @param
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 * adapt[in] : 대상 트랙 (1~)
 *
 * @return
 * 성공 시 대상 Fragment 핸들을 리턴한다. 존재하지 않으면 NULL을 리턴한다.
 *
 * @remark
 * 리턴된 Fragment 핸들은 solCMAF_release_fragment 함수를 통해 반드시 해제해 주어야 한다.
 */
scFragment solCMAF_startOf_tunnel(solCMAF ctx, cmafAdaptiveIndex adapt);

/*
 * 터널의 다음 Fragment 액세스 함수
 *
 * @param
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 * last[in] : 마지막 전송된 Fragment
 *
 * @return
 * 성공 시 대상 Fragment 핸들을 리턴한다. 존재하지 않으면 NULL을 리턴한다.
 *
 * @remark
 * 리턴된 Fragment 핸들은 solCMAF_release_fragment 함수를 통해 반드시 해제해 주어야 한다.
 */
scFragment solCMAF_nextOf_tunnel(solCMAF ctx, scFragment last);

/*
 * 트랙 Fragment 해제 함수
 *
 * @param
 * fragment[in] : Fragment 핸들
 */
void solCMAF_release_fragment(scFragment fragment);

/*
 * 트랙 Fragment 완료 여부 확인 함수
 * fragment[in] : Fragment 핸들
 *
 * @return
 * 완료된 Fragment면 1, 그렇지 않으면 0을 리턴한다.
 */
char solCMAF_isCompleted_fragment(scFragment fragment);


/*
* 트랙 Fragment 만료 여부 확인 함수
* fragment[in] : Fragment 핸들
*
* @return
* 만료된 Fragment면 1, 그렇지 않으면 0을 리턴한다.
*/
char solCMAF_isExpired_fragment(scFragment fragment);

/*
 * 트랙 Fragment 의 크기 확인 함수
 * fragment[in] : Fragment 핸들
 *
 * @return
 * Fragment의 크기(bytes)를 리턴한다.
 */
size_t solCMAF_sizeOf_fragment(scFragment fragment);

/*
 * 트랙 Fragment 출력 함수
 *
 * @param
 * io[in] : solCMAF I/O 핸들러 구조체 (메모리 및 출력(write) 관련)
 * ctx[in] : solCMAF 라이브러리 컨텍스트
 * fragment[in] : 대상 Fragment의 핸들
 * offset[in] : 대상 Fragment의 시작 offset(bytes)
 * length[in] : 빌드할 크기(bytes)
 *
 * @return
 * 성공 시 Fragment의 출력 크기(bytes)를 그렇지 않으면 0을 리턴한다.
 * 대상 Fragment가 CMAF/HTTP Chunk Push의 대상(빌드 진행 중)이면, 이후 추가될 CMAF Chunk 블럭은 이벤트(solCMAF_Event_AddChunk)를 통해 추가 출력을 시도해야 한다.
 */
size_t solCMAF_build_fragment(solCMAFIO *io, solCMAF ctx, scFragment fragment, uint32_t offset, uint32_t length);

// zipperRtmp 지원 함수
int solCMAF_alloc_mediaInfo(solCMAF ctx, cmafAdaptiveIndex adapt, solCMAFMediaInfo *info);
void solCMAF_free_mediaInfo(solCMAF ctx, solCMAFMediaInfo *info);
solCMAFFrame *solCMAF_frameOf(solCMAF ctx, const char *idstr, solCMAFFrame **prev, uint64_t lastdts);
int64_t solCMAF_lastDtsOf(solCMAF ctx, const char *idstr);
void solCMAF_releaseFrame(solCMAF ctx, solCMAFFrame *f);

#endif /* solCMAF_h */
