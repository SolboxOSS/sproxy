
#ifndef zipper_zipper__h
#define zipper_zipper__h

#include "mp4.h"
#include "io.h"
#include "zdump.h"

#include <sys/types.h>
#include <openssl/aes.h>

#ifdef __cplusplus
extern "C" {
#endif
    
typedef struct _zipper_media_context* zipperCnt;                // 미디어 Context,         생성후 공유 가능 (Read-Only)
typedef struct _zipper_builder_context* zipperBldr;             // HTTP 출력 Context,      생성후 공유 가능 (Read-Only)
typedef struct _zipper_rangeget_context* zipperRngGet;          // HTTP RangeGet Context,  생성 후 공유 불가능 (zipper_build() 함수 호출 시마다 내부 연산 루틴에서 업데이트됨)

typedef struct _segment_range
{
    uint32_t    start;
    
    uint32_t    last:1;
    uint32_t    fcnt:31;
    
} segment_range;
    
typedef struct _track_segment
{
    uint32_t    flag;
    
    struct {
        
        uint64_t    start;
        uint64_t    end;
        
    } msec;
    
    struct {
        
        struct {
            
            uint64_t    audio;
            uint64_t    video;
            
        } spts;
        
        segment_range *entry;
        uint32_t cc;
        
    } data;
    
} track_segment;

enum {
    
    mp4size_ftyp = 0,
    mp4size_moov,
    mp4size_mvex,
    
    mp4size_vtrk,
    mp4size_vstsd,
    mp4size_vidx,
    
    mp4size_atrk,
    mp4size_astsd,
    mp4size_aidx,
    
    mp4size_mdat,
    mp4size_end,
};

typedef struct _mp4box_build_area
{
    uint64_t    flag:4;
    uint64_t    size:60;

    
} mp4box_build_area;

// MP4 빌드 정보
typedef struct _zipper_builder_mp4out_matrix
{
    struct {
        
        uint32_t audio;
        uint32_t video;
        uint32_t text;
        
    } tscale;
    
    mp4box_build_area area[mp4size_end];
    
    struct {
        
        uint32_t    text;
        uint32_t    audio;
        uint64_t    video;
        
    } chunk;
    
    uint32_t    stss;
    
    struct {
        
        uint32_t    vpad:1;
        uint32_t    audio:31;
        uint32_t    video;
        
    } ctts;
    
    struct {
        
        uint32_t audio;
        uint32_t video;
        uint32_t text;
        
    } stsz;
    
    struct {
        
        uint32_t audio;
        uint32_t video;
        uint32_t text;
        
    } stts;
    
    uint32_t    area_max;
    uint32_t    frame_src_max;
    uint32_t    frame_mux_max;
    uint64_t    duration;
    
    struct _zipper_builder_mp4out_matrix *next;
    
} zipper_builder_mp4out_matrix;

typedef struct _track_stream_mux {
    
    char        lang[4];
    uint8_t     idx[8];     // 미디어 Context 내의 트랙 인덱스
    
} track_stream_mux;

// HTTP 출력 구성 트랙
typedef struct _zipper_track
{
    zipperCnt   context;        // 미디어 Context
    char        *descstr;       // 구분 명칭
    
    struct {
        
        uint32_t    discon:1;
        uint32_t    adaptbase:1;
        uint32_t    child:8;
        uint32_t    resv:22;
        
    } bi;
    
    track_stream_mux    smux;
    track_segment       segment;
    
    pthread_mutex_t     auto_update_lock;
    uint64_t            auto_update_tt;
    
    struct {
        
        struct _zipper_track *link;  // 내부 연결용
        struct _zipper_track *seq;   // 다음 이어(Next) 재생 트랙
        struct _zipper_track *adapt; // 다음 선택(Adaptive) 재생 트랙
        
    } next;
    
} zipper_track;

typedef struct _zipper_create_builder_param
{
    uint32_t    flag;       // option bit flag
    
    struct {
        
        uint16_t    qs;     // 퀵스타트를 위한 초기 최소 분할 세그먼트의 갯수
        uint16_t    dur;    // 분할 단위 (msec)
        
    } segment;
    
} zipper_create_builder_param;

typedef struct _zipper_builder_context
{
    uint32_t msize;
    uint32_t segment;
    
    zipper_create_builder_param opt;
    
    zipper_track *track;
    zipper_builder_mp4out_matrix *mp4_matrix;
    
} zipper_builder_context;

typedef enum {
    
    codec_index_none    = 0,
    codec_index_mp3     = 3,
    codec_index_aac     = 15,
    codec_index_mp4v    = 16,
    codec_index_avc     = 27,
    codec_index_text    = 29,
    codec_index_hevc    = 36,
    codec_index_flac    = 58,

    codec_index_ac3     = 129,
    codec_index_ec3     = 135,

    codec_index_vp6     = 139,
    codec_index_vp6a    = 140,
    codec_index_srsh263 = 141,

    
} media_desc_codec_index;

enum {
    
    src_format_unknown   = 0,
    src_format_mp4,
    src_format_id3,
    src_format_mp3,
    src_format_flac,
    src_format_flv,
    src_format_srt,
    src_format_zdmp,
};

typedef struct _zipper_media_desc
{
    float       duration;                   // duration (초)
    
    uint32_t    bit64:1;
    uint32_t    acc:4;
    uint32_t    vcc:4;
    uint32_t    tcc:4;
    uint32_t    sfmt:6;
    uint32_t    resv:13;
    uint32_t    bandwidth;
    
    struct {
        
        uint8_t     sdci;                   // 코덱 인덱스 (media_desc_codec_index)
        uint8_t     lang;                   // 언어 코드
        uint32_t    bandwidth;
        
        uint32_t    ch:4;                   // 채널
        uint32_t    obj_type:8;
        uint32_t    freq:20;                // frequency
        
    } audio[MULTI_TRACK_MAX];
    
    struct {
        
        uint8_t     sdci;                   // 코덱 인덱스 (media_desc_codec_index)
        uint8_t     lang;                   // 언어 코드
        uint32_t    bandwidth;
        
        union {
            
            struct {
                
                uint32_t    profile:8;      // H.264 프로파일 인덱스
                uint32_t    compatible:8;   // H.264 프로파일 호환성 코드
                uint32_t    level:16;       // 레벨 인덱스
                
            } avc;
            
            struct {
                
                uint8_t interlaced:1;
                uint8_t version2:2;
                uint8_t version:5;
                
            } vp6;
            
        } codec;
        
        struct {
            
            uint16_t    width;
            uint16_t    height;
            
        } resolution; // 비디오 크기
        
    } video[MULTI_TRACK_MAX];
    
    struct {
        
        uint8_t     sdci; // 코덱 인덱스 (media_desc_codec_index)
        uint8_t     lang; // 언어 코드
        
    } text[MULTI_TRACK_MAX];
    
} zipper_media_desc;

typedef struct _zipper_media_context
{
    uint32_t msize;
    zipper_media_desc   desc;
    zidx_context        *ctx;
    
} zipper_media_context;


#ifndef EOF
#define EOF     (-1)
#endif

#define EOF64       0xffffffffffffffffLL

#define BLDRFLAG_PERMIT_DISCONTINUTY (0x01 << 0)
#define BLDRFLAG_SEGMENT_BASED_ONLY (0x01 << 1)

#define drmProviderNone         0
#define drmProviderClearKey     (0x01 << 0)
#define drmProviderFairPlay     (0x01 << 1)
#define drmProviderPlayReady    (0x01 << 2)
#define drmProviderWidevine     (0x01 << 3)
    
typedef struct _zipper_drmcfg_clearkey
{
    uint8_t kid[16];
    
} zipper_drmcfg_clearkey;

typedef struct _zipper_drmcfg_playready
{
    uint8_t kid[16];
    const char *laUrl;
    
} zipper_drmcfg_playready;

typedef struct _zipper_drmcfg_fairplay
{
    uint8_t kid[16];
    
} zipper_drmcfg_fairplay;

enum {
    
    widevineTrackTypeHD = 0,
    widevineTrackTypeSD,
    widevineTrackTypeAudio,
};
    
typedef struct _zipper_drmcfg_widevine
{
    uint8_t kid[16];
    uint8_t cid[16];
    uint8_t tracktype;
    const char *provider;
    const char *pssh;
    
} zipper_drmcfg_widevine;

typedef struct _zipper_hls_encrypt
{
    const char  *uri;           // Encryption Key URL (M3U8 빌드시에만 사용/설정)
    
} zipper_hls_encrypt;
    
// HTTP 출력 암호화 설정
typedef struct _zipper_builder_encrypt
{
    uint32_t    useiv:1;        // IV를 사용할 경우 1로 설정
    uint32_t    smplaes:1;      // SAMPLE-AES 방식으로 암호화
    uint32_t    aesni:1;        // Intel AES NI를 사용할 경우에 1로 설정
    uint32_t    drmprovider:8;  // DRM 공급자 조합
    uint32_t    resv:21;
    
    uint8_t     key[16];        // Encryption Key
    uint8_t     iv[16];         // AES-128 IV(Initialization Vector)

    union {
        zipper_hls_encrypt      hls;
        
    } format;

    struct {
        
        zipper_drmcfg_clearkey      *clearkey;
        zipper_drmcfg_fairplay      *fairplay;
        zipper_drmcfg_playready     *playready;
        zipper_drmcfg_widevine      *widevine;
        
    } provider;

} zipper_builder_encrypt;

#define DASH_TYPE_SEP               0
#define DASH_TYPE_TS                1
#define DASH_TYPE_SINGLE            2

// MPD URL 포맷 설정
typedef struct _mpd_builder_format
{
    // BLDFLAG_MPD_TS를 사용할 경우 audio의 init에 sidx를, audio의 seq에 ts 파일명 포맷을 명시
    // BLDFLAG_MPD_UNI를 사용할 경우 audio.init, video.init에 단일 Fragmented MP4 파일 URL을 명시한다.
    
    uint8_t     type;
    uint8_t     resv;
    uint16_t    resv2;
    
    struct {
        char    *init;
        char    *seq;
        
    } audio;
    
    struct {
        char    *init;
        char    *seq;
        
    } video;
    
} mpd_builder_format;

static const char IIS_AUDIO_FRAGMENT_FORMAT[] = "QualityLevels({bitrate})/Fragments(audio={start time})";
static const char IIS_VIDEO_FRAGMENT_FORMAT[] = "QualityLevels({bitrate})/Fragments(video={start time})";

// IIS URL 포맷 설정
typedef struct _iis_builder_format
{
    struct {
        
        char *format;
        
    } audio;
    
    struct {
        
        char *format;
        
    } video;
    
} iis_builder_format;

typedef struct _zipper_flv_keymap_entry
{
    uint64_t    pts:28;     // 키프레임 pts (msec)
    uint64_t    offset:36;  // 키프레임 태그의 위치
    
} zipper_flv_keymap_entry;

enum {
    
    BLDTYPE_NONE	= 0,
    BLDTYPE_M3U8,           // M3U8 파일을 출력한다. (HLS)
    BLDTYPE_MPD,            // MPD 파일을 출력한다. (DASH)
    BLDTYPE_STS,            // 세그먼트 TS 파일을 출력한다. (HLS/DASH)
    BLDTYPE_MP3,            // MP3 파일을 출력한다. (PDL/Pseudo)
    BLDTYPE_MP4,            // MP4 파일을 출력한다. (PDL/Pseudo)
    BLDTYPE_FMP4INIT,       // 초기화용 Fragmented MP4 파일을 출력한다. (DASH)
    BLDTYPE_FMP4,           // Fragmented MP4 파일을 출력한다. (DASH)
    BLDTYPE_SIDX,           // 인덱스 MP4 파일을 출력한다. (DASH)
    BLDTYPE_THUMBNAIL,      // 썸네일 이미지를 출력한다.
    BLDTYPE_FLV,            // FLV 파일을 출력한다.
    BLDTYPE_FLVKEYMAP,      // FLV 파일에 대한 키맵을 출력한다.
    BLDTYPE_FMP4SINGLE,     // 단일 Fragmented MP4 파일을 출력한다 (DASH)
    BLDTYPE_IIS,            // IIS Smooth Streaming Manifest 파일을 출력한다.
    BLDTYPE_MOOF,           // Moof 박스를 출력한다. (Moof 박스만 있는 Fragmented MP4 파일 형태)
    BLDTYPE_FLAC,           // FLAC 파일을 출력한다. (PDL/Pseudo)
    BLDTYPE_FMP4M3U8,       // Fragmented MP4 기반 마스터 M3U8 파일을 출력한다.
    BLDTYPE_FMP4SUBM3U8,    // Fragmented MP4 기반의 서브 M3U8 파일을 출력한다.
    BLDTYPE_VTT             // 세그먼트 VTT
};

#define BLDFLAG_INCLUDE_VIDEO       (0x01 << 0)     // 비디오 포함
#define BLDFLAG_INCLUDE_AUDIO       (0x01 << 1)     // 오디오 포함
#define BLDFLAG_INCLUDE_TEXT        (0x01 << 2)     // 텍스트 포함
#define BLDFLAG_INCLUDE_AV          (BLDFLAG_INCLUDE_AUDIO | BLDFLAG_INCLUDE_VIDEO)
#define BLDFLAG_INCLUDE_ALL         (BLDFLAG_INCLUDE_AUDIO | BLDFLAG_INCLUDE_VIDEO | BLDFLAG_INCLUDE_TEXT) // 오디오, 비디오, 텍스트 모두 포함
#define BLDFLAG_CAL_SIZE            (0x01 << 3)     // 출력 사이즈(bytes)만 계산
#define BLDFLAG_ENCRYPT             (0x01 << 4)     // 암호화

#define M3U8_TARGET_VER_DEFAULT     3   // default HLS version = 3 (2010/11/19, http://tools.ietf.org/html/draft-pantos-http-live-streaming-05)

enum {
    
    THUMBNAIL_COMPRESSOR_JPEG   = 0,
    THUMBNAIL_COMPRESSOR_PNG    = 1,    // 아직 구현되지 않음.
};

typedef struct _build_buffer_param
{
    uint32_t    local:1;
    uint32_t    avflag:3;
    uint32_t    offset:28;
    off_t       fo;
    
    struct {
        
        uint8_t *p;
        size_t size;
        
    } aes;

    struct {
        
        uint8_t *p;
        size_t size;
        
    } mux;

    struct {

        uint8_t     *p;
        uint32_t    size;
        
        zipperCnt   media;
        uint64_t    offset;
        uint32_t    length;

    } chunk;
    
} build_buffer_param;

typedef struct _last_chunkget_stat
{
    zipper_track *track;
    
    uint32_t    entry_cc;
    uint32_t    entry_offset;
    
    off_t       last_offset;
    
} last_chunkget_stat;

typedef struct _last_flvget_stat
{
    zipper_track *track;
    
    uint32_t    entry_cc;
    uint32_t    entry_offset;
    off_t       last_offset;
    
    struct {
        
        uint64_t prev;
        uint64_t start;
        
    } basepts;
    
    uint32_t    tscale[5];
    
    uint8_t     part;
    uint32_t    prevts;
    
} last_flvget_stat;

typedef struct _ts_mux
{
    uint8_t     pat:4;
    uint8_t     aud:4;
    uint8_t     vid:4;
    uint8_t     txt:4;
    
    uint16_t    enc:3;
    uint16_t    pcr:2;
    uint16_t    bpos:11;
    
    uint16_t    aless;
    uint16_t    vless;
    uint16_t    tless;
    uint16_t    resv;
    
} ts_mux;

typedef struct _last_stsget_stat
{
    ts_mux      mux;
    uint8_t     *buf;
    
    struct {
        
        AES_KEY     key;
        
        uint8_t     iv[16];
        uint8_t     ivinit[16];
        
    } enc;
    
    uint16_t    pid[5];
    uint32_t    tscale[5];
    
    struct {
        
        int64_t pcr;
        uint64_t prev;
        uint64_t start;
        
    } basepts;

    struct {
        
        zipper_track *t;
        uint32_t sth:1;
        uint32_t scc:31;
        uint32_t fcnt;
        
    } resume;
    
} last_stsget_stat;

typedef struct _last_nonefmt_stat
{
    last_chunkget_stat chunk;
    
    uint16_t minblock;
    uint16_t maxblock;
    uint32_t minfrm;
    uint32_t maxfrm;
    
    struct {
        uint32_t cur;
        uint32_t total;
    } samples;
    
    id3_chunk *id3_foot;
    
    zipperCnt id3_media;

    struct {
        
        zipperCnt   ctx;
        uint32_t    vorbis;
        uint32_t    pic;
        
    } flac_ei;
    
    uint64_t nonefmt_body;
    
} last_nonefmt_stat;
    
enum {
    
    fmp4size_ftyp   = 0,
    fmp4size_sidx,
    fmp4size_moof,
    fmp4size_vtraf,
    fmp4size_vtrns,
    fmp4size_vsenc,
    fmp4size_atraf,
    fmp4size_atrns,
    fmp4size_asenc,
    fmp4size_mdat,
    fmp4size_end,
};

typedef struct _last_fmp4get_stat
{
    zipper_track    *track;
    uint32_t        bit64:1;
    uint32_t        single:1;
    uint32_t        index:31;
    
    uint32_t        area[fmp4size_end];
    uint32_t        area_max;
    
    uint32_t        fcnt;
    uint32_t        chunk;
    
    uint8_t         *trns;
    uint32_t        trnssize;
    
    uint8_t         *senc;
    uint32_t        sencsize;
    
    uint32_t        entry_offset;
    off_t           last_offset;
    
    uint32_t        moof_size;
    off_t           moof_offset;
    
    struct {
        
        uint32_t    duration;
        uint32_t    index;
        
        uint64_t    basepts;
        uint64_t    sdts;
        uint64_t    nextsdts;
        
    } pos;
    
    struct {
        
        AES_KEY         key;
        uint8_t         iv[16];
        uint8_t         ec[16];
        uint8_t         method:3;
        uint8_t         init:5;
        unsigned int    cnt;
        
    } enc;
    
} last_fmp4get_stat;

typedef struct _zipper_builder_attr
{
    uint32_t    bldtype:8;      // 출력 타입 (BLDTYPE_ enumerate 참조)
    uint32_t    bldflag:24;     // 출력 옵션 (BLDFLAG_ enumerate 참조)
    
    struct {
        
        uint32_t    adapt;    // 어댑티브 인덱스
        uint32_t    seq;      // 세그먼트 인덱스 (BLDTYPE_STS, BLDTYPE_FMP4, BLDTYPE_THUMBNAIL에 대해서만 유효)
        
    } index;
    
} zipper_builder_attr;

typedef struct _zipper_rangeget_context
{
    uint32_t msize;
    zipper_builder_attr     bldattr;
    build_buffer_param      buf;
    
    union {
        
        struct {
            
            last_chunkget_stat  cgstat;
            uint8_t             area;
            
            struct {
                
                uint8_t         *audio;
                uint32_t        asize;
                uint8_t         *video;
                uint32_t        vsize;
                
            } idx;
            
        } mp4;

        struct {
            
            last_nonefmt_stat cgstat;
            
        } nonefmt;
        
        struct {
            
            last_stsget_stat cgstat;
            
        } sts;
        
        struct {
            
            last_fmp4get_stat cgstat;
            
        } fmp4;
        
        struct {
            
            last_flvget_stat cgstat;
            
        } flv;
        
    } cache;
    
} zipper_rangeget_context;

/*
 *
 * zipper_builder_param의 출력 포맷(m3u8, mpd) 설정 방법
 *
 * %i: 세그먼트 인덱스 (0~), 숫자 정렬 맞춤 가능, 예) "%06i"
 * %a: 어댑티브 인덱스 (0~)
 * %e: 출력 포맷 확장자
 * %d: 트랙 구분 명칭, zipper_add_track, zipper_add_adaptive_track 참조
 *
 */

typedef struct _zipper_builder_param
{
    zipper_builder_attr   attr;
    
    struct {
        
        struct {
            
            zipperRngGet ctx; // HTTP RangeGet Context
            
            off_t   start;      // 시작 offset (bytes)
            off_t   size;       // 출력할 크기 (bytes), *0이면 전체가 되고, start값은 무시된다.
            
            // * 세그먼트 TS, Fragmented MP4는 Range Get을 지원하지 않는다 단, ctx는 이용될 수 있다).
            //   따라서, start, size는 모두 0으로 재설정된다.
            
            
        } range_get;    // Range-Get 파라미터
        
        union {
            
            struct {
                
                uint32_t    compressor:4;       // 압축 포맷
                uint32_t    width:14;           // 출력 너비 (px, 0일 경우 원본 비율에 의거함)
                uint32_t    height:14;          // 출력 높이 (px, 0일 경우 원본 비율에 의거함)
                
                union {
                    
                    struct {
                        uint8_t     quality;    // 압축 품질 (0~100)
                    } jpeg;
                    
                    struct {
                        uint8_t     interlaced;
                    } png;
                    
                } param;
                
            } thumbnail; // 썸네일 이미지 설정
            
            struct {
                
                char        *format;            // 출력 포맷

            } m3u8; // M3U8 설정

            struct {
                
                char *init;         // 초기화 파일 출력 포맷
                char *segment;      // 세그먼트 파일 출력 포맷
                
            } fmp4subm3u8;
            
            struct {
                
                char *video;    // 비디오 M3U8 출력 포맷
                char *audio;    // 오디오 M3U8 출력 포맷
                
            } fmp4m3u8;
            
            //  MPD, 마스터 M3U8 생성시 자막 트랙(M3U8)을 지정/설정한다.
            // subtrk[i].url 이 NULL이면 (i-1)이 마지막 자막 트랙이 된다.
            
            struct {

                uint16_t pid[5];    // PID 매핑
                uint8_t patcnt;     // PAT/PMT 시작 카운터
                uint8_t pcrtrack;   // PCR 트랙 타입
                int64_t basepcr;   // PCR 시작 기준 값 (basepts와의 차이값)
                
            } sts; // STS 설정 (zlive 지원 용)
            
            struct {
                
                float               min_buffer_time;    // 초기 버퍼 요구량(sec)
                mpd_builder_format  *format;            // 출력 포맷
                
            } mpd; // MPD 설정
            
            struct {
                
                iis_builder_format *format;
                
            } iis; // IIS 설정
            
            struct {
                
                uint32_t    include_keyframes:1; // onMetaData에 keyframes 리스트 정보 추가
                uint32_t    eos_is_key:1;       // EOS 태그를 key frames으로 인식 처리 (flvtool2 호환)
                uint32_t    fpos_tscale:30;     //  1 이상의 값이면 onMetaData의 keyframes.filepositions의 값을 times으로 하고 본값을 time scale로 적용.
                
            } flv; // FLV 설정
            
            struct {
                
                uint32_t    id3:1;
                uint32_t    resv:31;
                
                zipperCnt   id3media;
                
            } mp3;

            struct {
                
                uint32_t    id3:1;
                uint32_t    resv:31;
                
                zipperCnt   id3media;
                
            } flac;
            
        } attr;
        
        struct {
            
            uint8_t def; // 기본 선택 자막인 경우 1, 그렇지 않으면 0 (def=1인 자막이 없는 경우 첫번째 자막이 자동 DEFAULT+AUTOSELECT로 처리)
            char *url; // 자막 M3U8 URL(*필수) (생성하는 M3U8의 상대 URL 혹은 절대 URL 모두 가능, zipper에서 별도로 보정하지 않음)
            char *desc; // NULL일 경우 lang 필드의 값을 그대로 사용
            char *lang; // ISO639-1 3자리 언어코드(*필수) (예:"kor", "eng", iso639.h 참조)
            
            // 마지막 자막 기술 후 다음 인덱스의 url을 NULL로 설정하시면 됩니다.
            /*
                예) 한국어/영어 2개 자막 설정 시
             
                .sub_link[0].url = "kor/sub.m3u8";
                .sub_link[0].lang = "kor";
                .sub_link[1].url = "eng/sub.m3u8";
                .sub_link[1].lang = "eng";
                .sub_link[2].url = NULL;
                .sub_link[2].lang = NULL;
             
             */
                        
        } sub_link[8];

    } target; // 출력 대상 설정
    
    struct {
        
        uint8_t *buf;       // 출력 버퍼 (사전에 계산되어진 크기만큼 미리 할당되어 있어야 함, NULL이면 무시됨)
        off_t   size;       // 출력 크기 (bytes)
        
        off_t   written;    // 최종 쓰여진 크기 혹은 계산된 크기
        uint64_t lastdts;   // 맨 마지막에 쓰여진 타임스탬프(dts), zlive 전용
        
    } output;
    
    zipper_builder_encrypt      *encrypt;   // 암호화 설정
    uint64_t                    basepts;    // 시작 타임스탬프 (90Khz), BLDTYPE_FLV, BLDTYPE_STS에만 적용됨.
    
} zipper_builder_param;

// 미디어 파일(혹은 덤프 파일)에 대한 미디어 Context를 생성한다.
// io_handle : I/O 핸들러
// context : [out] 생성된 context
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_create_media_context(zipper_io_handle *io_handle, zipperCnt *context);

// 미디어 Context를 해제한다.
void zipper_free_media_context(zipper_io_handle *io_handle, zipperCnt *context);

// 미디어 정보를 구한다.
// desc : 정보를 받을 zipper_media_desc 구조체 포인터
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_media_context_info(zipperCnt context, zipper_media_desc *desc);

// 미디어가 interleaved 되어 있는지 체크한다.
// interleaved 되어 있으면 1을, 그렇지 않으면(가상 interleaved 모드인 경우) 0을 리턴한다.
char zipper_media_is_interleaved(zipperCnt context);

// 컨텐스트의 메모리 사용량(bytes)을 구한다.
// context : zipper Context (zipperCnt, zipperBldr, zipperRngGet)
//
// * zipperBldr의 경우에는 zipper_end_track() 호출 이후에 유효하다.
uint32_t zipper_context_size(void *context);

// 컨텍스트 유형을 구한다. (deprecated, always return 0)
char zipper_context_type(zipperCnt context);

// 컨텍스트를 덤프한다.
// io_handle : I/O 핸들러
// context : 대상 미디어 컨텍스트
// dump_size : [out]덤프 파일의 크기(bytes)
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
// I/O 핸들러의 쓰기 콜백(zipper_write_func)을 통해서 덤프한다.
// I/O 핸들러가 NULL이면 덤프 크기만을 구하여 dump_size에 기록한다.
int zipper_context_dump(zipper_io_handle *io_handle, zipperCnt context, size_t *dump_size);

// 오류 메세지를 리턴한다.
// code : zipper 라이브러리의 오류 코드
const char *zipper_err_msg(int code);

// 언어명을 리턴한다.
// code : 언어 코드
const char *zipper_language_desc(uint8_t code);


// 언어코드(인덱스)를 리턴한다.
// desc : ISO639-2 3자리 문자열 코드
uint8_t zipper_language_code(const char *desc);

// 언어명에 해당하는 트랙 인덱스를 리턴한다.
// context: zipper Context
// lang : 언어코드
// track : 트랙 타입 (비디오=BLDFLAG_INCLUDE_VIDEO, 오디오=BLDFLAG_INCLUDE_AUDIO)
uint8_t zipper_index_of_lang(zipperCnt context, uint8_t lang, uint8_t track);

// HTTP 출력 Context를 생성한다.
// io_handle : I/O 핸들러
// context : [out] 생성된 context
// segment : 분할 단위 (msec)
// flag : 옵션 flag
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_create_builder_context(zipper_io_handle *io_handle, zipperBldr *context, zipper_create_builder_param *param);

#define TRACK_SMUX_QUERY_LANG       (0x01 << 0)
#define TRACK_SMUX_QUERY_INDEX      (0x01 << 1)
#define TRACK_SMUX_QUERY_TEMPO      (0x01 << 2)

#define TRACK_COMPFLAG_NOT_SEEKABLE         (0x01 << 0)
#define TRACK_COMPFLAG_NONE_STSS            (0x01 << 1)

typedef struct _track_composition_param
{
    const char *desc;   // 미디어 Context의 구분 명칭
    zipperCnt media;    // 추가할 미디어 Context
    
    struct {
        
        uint8_t             qtype;  // query type
        track_stream_mux    qdata;
        
    } smux;
    
    union {
        
        struct {
            
            uint32_t flag;  // 옵션 flag, TRACK_COMPFLAG_* 참조
            
            uint64_t start; // 구간 시작 (msec)
            uint64_t end;   // 구간 끝 (msec)
            // * 만약 end가 0이면, start는 절대시간이 아닌 현재 시간으로부터의 상대 시간이 된다.
            
        } range; // zipper_add_track 호출 시 참조,
        
        zipperCnt adaptBase;    // zipper_add_adaptive_track 호출 시 참조
        // 선택 범위 내의 가장 선두(맨 처음 추가된) 트랙의 미디어 Context
        
    } comp;
    
    
} track_composition_param;

// 이어(Next) 재생 트랙을 추가한다.
// io_handle : I/O 핸들러
// bldr : 출력 Context
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_add_track(zipper_io_handle *io_handle, zipperBldr bldr, track_composition_param *param);

// 선택(Adaptive) 재생 트랙을 추가한다.
// io_handle : I/O 핸들러
// bldr : 출력 Context
//
// * 구간은 선두 트랙에 자동 종속됨.
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_add_adaptive_track(zipper_io_handle *io_handle, zipperBldr bldr, track_composition_param *param);

// 트랙 추가를 종료한다.
// io_handle : I/O 핸들러
// bldr : 출력 Context
void zipper_end_track(zipper_io_handle *io_handle, zipperBldr bldr);

// HTTP 출력 Context를 해제한다.
void zipper_free_builder_context(zipper_io_handle *io_handle, zipperBldr *context);

// HTTP 출력 Context에 미디어가 추가되어 있는 지 확인한다.
char zipper_have_media(zipperBldr context);

// HTTP 출력 Context의 총 duration을 리턴한다.
// context : HTTP 출력 Context
// adaptive_index : Adaptive Index(0~)
uint32_t zipper_track_duration(zipperBldr context, uint8_t adaptive_index);

// HTTP 출력 Context의 세그먼트 갯수를 리턴한다.
// context : HTTP 출력 Context
// adaptive_index : Adaptive Index(0~)
unsigned int zipper_segment_count(zipperBldr context, uint8_t adaptive_index);

// HTTP 미디어를 출력한다.
// io_handle : I/O 핸들러
// context : HTTP 출력 Context
// param : [in/out] 출력 옵션을 설정한 zipper_builder_param 구조체 포인터
//
// * Adaptive M3U8 (Main M3U8) 혹은 MPD를 출력할 경우에는 adaptive_index는 0xFF로 설정해 주어야 한다.
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_build(zipper_io_handle *io_handle, zipperBldr context, zipper_builder_param *param);

// HTTP RangeGet Context를 생성한다.
// io_handle : I/O 핸들러
// context : [out] 생성된 context
//
// 성공 시 0을, 실패 시 오류 코드를 리턴한다.
int zipper_create_rangeget_context(zipper_io_handle *io_handle, zipperRngGet *context);

// HTTP RangeGet 상태 Context를 해제한다.
void zipper_free_rangeget_context(zipper_io_handle *io_handle, zipperRngGet *context);

// 세그먼트 시작 타임을 리턴한다.
// context : HTTP 출력 Context
// adaptive_index : Adaptive Index(0~)
// index: 세그먼트 인덱스 (0~)
//
// 만약 EOF(0xFFFFFFFFFFFFFFFF, -1)를 리턴하면 해당되는 세그먼트를 찾지 못한 경우이다.
// * MPD의 경우 각 세그먼트에 대해서 SegmentTemplate의 $Time$으로 대치되는 값이다.
uint64_t zipper_segment_pts(zipperBldr context, uint8_t adaptive_index, uint32_t index);


// 해당 어뎁티브 인덱스의 트랙 비트레이트를 리턴한다.
// context : HTTP 출력 Context
// adaptive_index : Adaptive Index(0~)
// track : 트랙 타입 (비디오=BLDFLAG_INCLUDE_VIDEO, 오디오=BLDFLAG_INCLUDE_AUDIO)
//
//// 만약 EOF(0xFFFFFFFFF, -1)를 리턴하면 해당되는 어뎁티브 인덱스를 찾지 못한 경우이다.
uint32_t zipper_adaptive_bps(zipperBldr context, uint8_t adaptive_index, uint8_t track);

// 해당 비트레이트의 대한 어뎁티브 인덱스를 리턴한다(0~).
// context : HTTP 출력 Context
// bps : 비트레이트
// track : 트랙 타입 (비디오=BLDFLAG_INCLUDE_VIDEO, 오디오=BLDFLAG_INCLUDE_AUDIO)
//
// 만약 찾지 못하였을 경우 0xFF를 리턴한다.
//
// * IIS Smooth Streaming의 경우 Adaptive내 Fragments를 비트레이트 정보로 구분하므로,
// 본함수를 통해 매칭되는 어뎁티브 인덱스를 찾아야 한다.
uint8_t zipper_adaptive_index_by_bps(zipperBldr context, uint32_t bps, uint8_t track);

// 세그먼트의 duration을 리턴한다.(msec)
// context : HTTP 출력 Context
// adaptive_index : Adaptive Index(0~)
// index: 세그먼트 인덱스 (0~)
//
// 만약 EOF(0xFFFFFFFF, -1)를 리턴하면 해당되는 세그먼트를 찾지 못한 경우이다.
uint32_t zipper_segment_duration(zipperBldr context, uint8_t adaptive_index, uint32_t index);

// 시작 타임에 대응되는 세그먼트를 찾는다.
// context : HTTP 출력 Context
// adaptive_index : Adaptive Index(0~)
// pts : 시작 시간
//
// 만약 EOF(0xFFFFFFFF, -1)를 리턴하면 해당되는 세그먼트를 찾지 못한 경우이다.
// * zipper_segment_pts 참조
uint32_t zipper_segment_index(zipperBldr context, uint8_t adaptive_index, uint64_t pts);


// 라이브러리 버전 정보를 리턴한다.
const char *zipper_version();

int stsformat(char *buf, const char *fmt, char *track_desc, uint8_t adapt, uint32_t index, const char *ext, int extlen);
int stsformat2(char *buf, const char *fmt, char *track_desc, uint8_t adapt, uint32_t index, uint64_t pts, const char *ext, int extlen);
    
// m3u8 매니패스트 수정 함수 /////////
/*
 * 매니패스트의 세그먼트 기술을 변경합니다.
 *
 * @param:
 * ih[in] : I/O 핸들러
 * in[in] : 원본 매니패스트 데이터
 * insize[in] : 원본 매니패스트 데이터의 크기
 * segment_fmt[in] : 세그먼트 기술 포맷 (zipper 라이브러리 포맷 참조)
 * out[out] : 수정된 매니패스트 데이터 블럭
 * outsize[out] : 수정된 매니패스트의 데이터 크기(빌드된)
 * obsize[out] : 수정된 매니패스트 데이터 블럭의 크기(할당된)
 *
 *
 * @return
 * 성공 시 0을, 실패 시 오류 코드를 리턴합니다.
 *
 * @remark
 * segment_fmt에는 다음의 포맷 문자가 유효하다.
 *
 *      %i : 세그먼트 인덱스
 *      %e, %f : 원본 매니패스트에 기술된 원본 파일 명
 *
 */
int edit_manifest(zipper_io_handle *ih, uint8_t *in, size_t insize, char *segment_fmt, uint8_t **out, size_t *outsize, size_t *obsize);

/*
 * MPEG 코덱 기술 문자열 생성 함수
 *
 * @param
 * context[in] : 대상 미디어 컨텍스트
 * audio[in] : 대상 오디오 트랙의 인덱스 (0xff이면 제외)
 * video[in] : 대상 비디오 트랙의 인덱스 (0xff이면 제외)
 * p[out] : 기술 문자열을 저장할 버퍼
 *
 * @return
 * 성공 시 문자열의 길이를, 그렇지 않으면 0을 리턴합니다.
 *
 * @remark
 * 버퍼의 크기는 미리 할당해야 하며, 생성되는 문자열의 최대 길이는 32bytes 입니다.
 */
size_t zipper_codec_desc(zipperCnt context, uint8_t audio, uint8_t video, char *p);
   
// flac 포맷 관련 유틸리티 함수 //////////////////////////////////////////////////////////////////////////////////////////////////////

// pts에 해당하는 프레임 offset을 리턴한다.
// io_handle : I/O 핸들러
// pts : 기준 시간 (msec)
// dur : [out] FLAC의 stream info의 duration 필드 offset
// samples : [out] 리턴 값에 해당하는 프레임 이전까지의 sample 갯수
//
// 프레임 offset을 찾은 경우 offset을, 그렇지 않으면 -1을 리턴한다.
// 본 함수는 비동기 I/O를 지원하지 않는다.
off_t zipper_flac_offset(zipper_io_handle *io_handle, uint64_t pts, off_t *dur, uint32_t *samples);

// 내부 공유 함수
mp4_track_desc *_zipper_track_desc(uint8_t type, void *context, uint8_t track, uint8_t index);
    
// zipper Vairant API //////////////////////////////////////////
typedef struct _zipper_variant_context *zipperVariant;          // Zipper Variant Context

#define variant_track_video     1
#define variant_track_audio     2
#define variant_track_subtitle  4

/*
 zipper variant 컨텍스트 생성 함수
 */
int zipper_create_variant_context(zipper_io_handle *io_handle, zipperVariant *ctx);

/*
 zipper variant 컨텍스트 해제 함수
 */
void zipper_free_variant_context(zipper_io_handle *io_handle, zipperVariant *ctx);

typedef struct _zipper_variant_track {

    struct {
        
        struct {
            zipperCnt ctx;          // 트랙 소스의 zipper 미디어 컨텍스트
                                    // NULL로 지정할 경우 최소 아래의 bandwidth 필드 정보는 직접 기술해 주어야 한다.
                                    // zipper_variant_build()시 오류 리턴
            
            struct {
                
                uint8_t video:4;    // 트랙 소스에 사용된 비디오 트랙 인덱스(0~)
                uint8_t audio:4;    // 트랙 소스에 사용된 오디오 트랙 인덱스(0~)
                
            } map;
            
        } src;                  // 미디어 컨텍스트를 사용해 스트림 속성을 지정할 경우 사용
                                // 아래 정보를 직접 기술 시 무시된다.
        
        uint32_t bandwidth;     // BADNWIDTH 정보,
                                // 0이면 ctx(NULL이 아닐 경우)로부터 정보를 구한다.
                                // * BANDWIDTH 정보는 반드시 필요하다.만약 0으로 설정 시 .src 정보를 기술해야 한다.
        
        struct {

            uint16_t width;     // 비디오 해상도 너비
            uint16_t height;    // 비디오 해상도 높이
                                // 둘 중 하나라도 0이면 ctx가 NULL이 아니면 ctx로 부터 그렇지 않으면 기술을 생략한다.

        } resolution;

        const char *codecs;     // 코덱 정보
                                // NULL이고 ctx가 NULL이 아니면 ctx로 부터 그렇지 않으면 기술을 생략한다.

    } media;                    // .media 정보는 해당 트랙이 스트림으로 사용시(.group.sef == NULL) 스트림 속성 기술에 필요한 정보들이다.
                                // 만약 트랙 그룹인 경우에는 기술하지 않아도 된다(생략 가능).
    
    const char *url;            // 트랙의 매니패스트 URL(상대 경로 권장)
                                // 직접 사용(별도의 매크로 없음)

    uint8_t avflag;             // 트랙 composition(조합, 단일 혹은 멀티 가능)
                                // variant_track_* 비트 플래그 조합

    const char *lang;           // 언어 코드 (ISO639-2 3자리)
                                // NULL이면 미디어 컨텍스트로부터 상속 트랙 그룹으로 지정된 경우에만 유효하다.

    const char *name;           // 트랙 구분 명(그룹 내)
                                // 트랙 그룹으로 지정된 경우에만 유효하다.

    int priority;               // 기술 상 우선 순위 (순서)
                                // 이미 같은 priority를 가진 트랙이 있으면 해당 트랙의 다음 순서로 배치된다.
    
    struct {

        const char *self;       // 그룹명 (*중요)
                                // NULL일 경우 스트림으로 그렇지 않으면 트랙 그룹으로 인식한다.
                                // N개의 트랙이 동일한 그룹명을 가지고 그룹을 형성한다(단일도 가능)

                
                                // 스트림인 경우 나머지 트랙에 대해 트랙 그룹을 명시, 연결해야 한다.
                                // 모두 optional이다. (NULL(기본값)로 생략 가능)

        const char *video;      // 비디오 트랙 그룹명
        const char *audio;      // 오디오 트랙 그룹명
        const char *subtitle;   // 자막(WebVTT) 트랙 그룹명

    } group;

    char def;                   // 1=기본 선택, 0=
                                // 모든 트랙이 0이면 가장 높은 priority를 가진 트랙이 자동으로 1로 설정된다.

    char autoselect;            // 1=자동 선택 가능,
                                // 2=사용자에 의해서만 선택
                                // 모든 트랙이 0이면 가장 높은 priority를 가진 트랙이 자동으로 1로 설정된다.

} zipper_variant_track;

/*
 zipper variant 컨텍스트 트랙 및 스트림 추가 함수
 
 @param
    track[in]: 추가할 트랙 및 스트림 속성 구조체(zipper_variant_track 구조체 참조)
 
 */
int zipper_variant_add_track(zipper_io_handle *io_handle, zipperVariant ctx, zipper_variant_track *track);

typedef struct _zipper_variant_build_param {

    uint8_t flag;       // 옵션 비트 플래그
    uint8_t format;     // 출력 포맷 (BLDTYPE_*, 현재는 M3U8(BLDTYPE_M3U8)만)
        
    struct {

        uint8_t ver;    // M3U8 버전 (0이면 자동 할당)

    } m3u8;

    struct {

        size_t written; // 출력된 크기(bytes)가 저장된다.

    } output;

} zipper_variant_build_param;

/*
 zipper variant 매니패스트 출력 함수
 
 @param
    param[in/out]: 출력 설정 및 결과 정보 저장 구조체 (zipper_variant_build_param 구조체 참조)
 */
int zipper_variant_build(zipper_io_handle *io_handle, zipperVariant ctx, zipper_variant_build_param *param);


#ifdef __cplusplus
}
#endif

#endif

