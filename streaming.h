#ifndef __STREAMING_H__
#define __STREAMING_H__

#include <containers.h> /* c container library */
#include "zipper.h"
#include "zippererr.h"
#include "zlive.h"
#include "solCMAF.h"
#include "err.h"

#define ERROR_MSG_BUF_SIZE		512
//#define	MAX_STREAMING_URL_LEN	DEFAULT_POOL_SIZE/2

typedef enum {
	MEDIA_TYPE_NORMAL		= 0, 	/* 일반 파일*/
	MEDIA_TYPE_CROSSDOMAIN,
	MEDIA_TYPE_CLIENTACCESSPOLICY,
	MEDIA_TYPE_HLS,
	MEDIA_TYPE_HLS_MAIN_M3U8,
	MEDIA_TYPE_HLS_SUB_M3U8,
	MEDIA_TYPE_HLS_VTT_M3U8,
	MEDIA_TYPE_HLS_FMP4_MAIN_M3U8,
	MEDIA_TYPE_HLS_FMP4_AUDIO_M3U8,
	MEDIA_TYPE_HLS_FMP4_VIDEO_M3U8,
	MEDIA_TYPE_HLS_AUDIOLIST,
	MEDIA_TYPE_HLS_TS,
	MEDIA_TYPE_HLS_VTT,
	MEDIA_TYPE_HLS_MANIFEST,
	MEDIA_TYPE_MPEG_DASH_MANIFEST,		/* Fragmented MP4 type DASH */
	MEDIA_TYPE_MPEG_DASH_TS_MANIFEST,	/* MEPG2-TS type DASH */
	MEDIA_TYPE_MPEG_DASH_YT_MANIFEST,	/* Youtube type(단일 Fragmented MP4 + Range-Get 기반) DASH */
	MEDIA_TYPE_MPEG_DASH_VIDEO_INIT,
	MEDIA_TYPE_MPEG_DASH_VIDEO,
	MEDIA_TYPE_MPEG_DASH_AUDIO_INIT,
	MEDIA_TYPE_MPEG_DASH_AUDIO,
	MEDIA_TYPE_MPEG_DASH_TS_INDEX,
	MEDIA_TYPE_MPEG_DASH_TS,
	MEDIA_TYPE_MPEG_DASH_YT_VIDEO,
	MEDIA_TYPE_MPEG_DASH_YT_AUDIO,
	MEDIA_TYPE_MSS_MANIFEST,
	MEDIA_TYPE_MSS_VIDEO,
	MEDIA_TYPE_MSS_AUDIO,
	MEDIA_TYPE_MP4,
	MEDIA_TYPE_M4A,
	MEDIA_TYPE_M4V,
	MEDIA_TYPE_MP3,
	MEDIA_TYPE_FLV,
	MEDIA_TYPE_FLAC,
	MEDIA_TYPE_MP4_DOWN,
	MEDIA_TYPE_M4A_DOWN,
	MEDIA_TYPE_M4V_DOWN,
	MEDIA_TYPE_MP3_DOWN,
	MEDIA_TYPE_FLAC_DOWN,
	MEDIA_TYPE_SMIL_DOWN,
	MEDIA_TYPE_SUBTITLE_DOWN,
	MEDIA_TYPE_CMAF_VIDEO_TUNNEL,
	MEDIA_TYPE_CMAF_AUDIO_TUNNEL,
	MEDIA_TYPE_ENC_KEY,
	MEDIA_TYPE_MODIFY_HLS_MANIFEST,		/* manifest edit 기능용 */
	MEDIA_TYPE_DOWN, 		/* 캐시로 동작하는 경우 */
	MEDIA_TYPE_FCS_IDENT
} scx_media_type_t;

#define MEDIA_TYPE_RTMP	101

#define ARGUMENT_NAME_START		"start"
#define ARGUMENT_NAME_END		"end"
#define ARGUMENT_NAME_DURATION	"duration"
#define ARGUMENT_NAME_TEMPO		"tempo"
#define ARGUMENT_NAME_BYTES		"bytes"
#define ARGUMENT_NAME_BG_TEMPO	"bgtempo"


#define	O_PROTOCOL_RTMP				0x00100000
#define	O_PROTOCOL_PROGRESSIVE		0x01000000
#define	O_PROTOCOL_HLS				0x02000000
#define	O_PROTOCOL_DASH				0x04000000
#define	O_PROTOCOL_MSS				0x08000000	/* Microsoft Smooth Streaming */
#define	O_PROTOCOL_CORS				0x10000000	/* Cross-origin resource sharing */
#define	O_PROTOCOL_TUNNEL			0x20000000


#define O_STRM_TYPE_SINGLE			0x10000000
#define O_STRM_TYPE_MULTI			0x20000000
#define O_STRM_TYPE_ADAPTIVE		0x40000000

#define O_STRM_TYPE_DIRECT			0x00100000	/* orgin파일을 그대로 전송하는 경우 */
#define O_STRM_TYPE_PSEUDO			0x00200000	/* pseudo Streaming */
#define O_STRM_TYPE_TEMPO			0x00400000	/* 오디오 배속 변경 Streaming */
#define O_STRM_TYPE_PREVIEW			0x00800000	/* 미리보기 기능 */


typedef enum {
	O_CONTENT_TYPE_VOD		= 2000,
	O_CONTENT_TYPE_ADVER,
	O_CONTENT_TYPE_LIVE,
	O_CONTENT_TYPE_SUBTITLE
} scx_content_type_t;


#define SUBTITLE_TYPE_UNKNOWN		0
#define SUBTITLE_TYPE_SRT			1
#define SUBTITLE_TYPE_VTT			2


typedef enum {
	ENCRYPT_METHOD_NONE = 0,
	ENCRYPT_METHOD_AES_128,
	ENCRYPT_METHOD_SAMPLE_AES
} scx_enc_method;

struct MHD_Connection;
struct nc_stat;
struct session_info_tag;
/*
 * netcache core에 대한 정보를 가지고 있는 구조체
 * source_info_t user가 들어 올때마다 새로 생성하고 공유하지 않는다.
 * mpool과 req는 리스트의 처음에만 할당 되고 이후부터는 NULL임.
 */
typedef struct tag_source_info {
	/* 이하는 리스트의 처음에만 할당되는 변수들임 */
	mem_pool_t 			*mpool;		/* pool은 list의 처음에만 할당한다. 두번째 부터는 NULL */
	/* 아래는 리스트 마다 할당되는 변수들임 */
	zipperCnt			mcontext;
#if 0
	/* live ad stiching 기능을 개발하면서 아래 부분은 필요 없어짐 */
	void		 		*service;	/* service_info_t *의 정보를 기억, 설정 reloading 때문에 기억하지 않고 매번 찾도록 한다. */
#endif
	void			 	*file; 		/* nc_file_handle_t의 pointer */
	struct nc_stat 		objstat;
	struct tag_source_info	*next;
} source_info_t;



/* 미디어의 인덱싱 정보를 저장하는 구조체
 * media 의 검색은 url과 host 두가지 key로 검색한다.*/
typedef struct tag_media_info {
	mem_pool_t 			*mpool;
	int					id;			/*random으로 생성한 unique ID */
	char				*url;		/* 동영상의 path(origin 요청 경로), 실제 요청될 origin의 url, object_key도 같은걸로 사용한다.*/
//	char 				*host;
	int					type;		/* 0:VOD, 1:광고, 2:LIVE */
//	void		 		*service;	/* service_info_t *의 정보를 기억, 설정 reloading 때문에 기억하지 않고 매번 찾도록 한다. */
	char				st_devid[128];	/* req->objstat.st_devid와 동일, media의 변경 기준 */
	time_t				atime;	/* 마지막 사용 시간 */
	time_t				vtime;	/* 미디어의 만료 시간 */
	time_t				mtime;	/* 미디어의 변경 시간 */
	time_t				etime;	/* 마지막 에러 발생 시간 */
	uint64_t			msize;	/* 원본 미디어의 파일 크기 */
	zipperCnt			mcontext;
	uint32_t			mcontext_size;
	void				*ginfo;
	volatile int		available;	/* 업데이트나 삭제가 되어야 하는 경우 이곳을 0으로 셋팅. 사용 가능은 1 */
	int					revision; 	/* 업데이트 마다 1씩 증가 */
	volatile int		refcount;	/* 해당 media를 열고 있는 수 */
	volatile unsigned long 	account;	/* 총 사용된 수 */
	int					seq_id;		/* 디버깅용 ID, 순차 증가 */
	pthread_mutex_t 	lock;
} media_info_t;

typedef struct tag_media_info_list {
	media_info_t 		*media;
	float      			duration;   /* 단일 미디어 재생시간  (초) */
	int					is_base;	/* adaptive streaming에서 base media의 경우 여기에 1이 들어감 */
	int					revision; 	/* media_info_t의 revision */
	struct tag_media_info_list *next;
	track_composition_param tcprm;
} media_info_list_t;

typedef struct tag_builder_info {
	mem_pool_t 			*mpool;
	zipperBldr 			bcontext;
	uint32_t			bcontext_size;
	media_info_list_t	*media_list;	/* builder 생성에 사용되고 있는 media의 pointer들 */
	float      			duration;   /* 전체 재생시간  (초) */
	time_t				ctime;		/* builder 생성시간 */
	time_t				vtime;		/* builder의 만료 시간(media의 만료시간과 동일). 여러개의 media를 사용할 경우 media의 만료시간중 가장 짧은 만료 시간을 사용한다. */
	volatile time_t		mtime;		/* 마지막 동영상의 변경시간 */
	int					is_adaptive;	/* adaptive contents일 때에는 1일 셋팅된다. */
	int				    bandwidth;		/* media가 여러개인 경우 첫 media의 bandwidth, width, height가 들어간다. adaptive가 아닌 경우에만 사용 */
	int					width;
	int					height;
	int					adaptive_track_cnt;		/* adaptive contents인 경우 세팅됨, 하나의 track을 만드는데 사용된 bitrate 별 content의 수 */
	char 				*adaptive_track_name[20];	/* adaptive contents의 track(bitrate 별) name, 최대 20개까지 가능 */
	char				*codecs[20];	/* 필수 값이 아니때문에 NULL이 들어 올수 있다 */
	char				*resolution[20];	/* 필수 값이 아니때문에 NULL이 들어 올수 있다 */
} builder_info_t;

#define USE_ANOTHER_SUBTILTE_STRUCT 1
/*
 *  client로 부터 들어온 요청에 포함된 동영상들의  경로 및 range 정보를 저장
 * content_info의 변수들의 메모리 할당은  streaming_t의 mpool을 사용한다.
 * adaptive streaming의 경우 main1->sub1->sub1->main2->sub2->sub2->main3->sub3->sub3의 순서로 리스트에 저장된다
 */
typedef struct tag_content_info {
	char			 		*path;
	int						type;		/* scx_content_type_t에 지정된 값이 들어감, 0:VOD, 1:광고, 2:LIVE , 자막인 경우 O_CONTENT_TYPE_SUBTITLE이 들어감 */
	int			 			start;		/* start와 end는 milisecond 단위임 */
	int			 			end;		/* end가 range 요청에 들어 있지 않은 경우는 EOF(동영상의 끝까지)가 들어 간다. */
	int						is_base;	/* adaptive streaming에서 base media의 경우 여기에 1이 들어감 */
	int						available;	/* 1 경우 사용 , 0인 경우 무시, smil 파싱 단계에서만 사용 */
	char					*bitrate;	/* int로 해도 되지만 혹시나 문자가 들어 올 경우를 대비해서 문자열로함 */
	char					*codecs;	/* 필수 값이 아니때문에 NULL이 들어 올수 있다 */
	char					*resolution;	/* 필수 값이 아니때문에 NULL이 들어 올수 있다 */
#ifndef USE_ANOTHER_SUBTILTE_STRUCT
	// 자막 관련 부분 시작
	char					*subtitle_lang;	/* kr, en 등 */
	char 					*subtitle_name;		/* Korean, English 등 */
	int						subtitle_type;	/* 1:srt, 0:vtt */
	int						subtitle_order;		/* smil 파일에 들어 있는 자막 순서, 1부터 시작 */
	// 자막 관련 부분 끝
#endif
	struct tag_content_info	*next;
} content_info_t;

typedef struct tag_subtitle_info {
	char			 		*path;
	int			 			start;		/* start와 end는 milisecond 단위임 */
	int			 			end;		/* end가 range 요청에 들어 있지 않은 경우는 EOF(동영상의 끝까지)가 들어 간다. */
	int						is_base;	/* adaptive streaming에서 base media의 경우 여기에 1이 들어감 */
	int						available;	/* 1 경우 사용 , 0인 경우 무시, smil 파싱 단계에서만 사용 */
	char					*subtitle_lang;	/* kr, en 등 */
	char 					*subtitle_name;		/* Korean, English 등 */
	int						subtitle_type;		/* 1:srt, 0:vtt */
	int						subtitle_order;		/* smil 파일에 들어 있는 자막 순서, 1부터 시작 */
	struct tag_subtitle_info	*next;
} subtitle_info_t;

/* smil 파일별 내용을 저장 하는 구조체 */
typedef struct tag_adaptive_info {
	char	*path;		/* smil혹은 mp3/mp4 파일 경로 */
//	char	*arg;		/* smil혹은 mp3/mp4 파일 오리진 요청시 argument가 필요한 경우 */
	int		type;		/*  0:VOD, 1:광고, 2:LIVE */
	int		is_smil;	/* smil 파일인 경우는 1, mp3/mp4 파일인 경우는 0 */
	int		start;		/* start와 end는 milisecond 단위임 */
	int		end;		/* end가 range 요청에 들어 있지 않은 경우는 EOF(동영상의 끝까지)가 들어 간다. */
	int		order;		/* 컨텐츠 순서, 0부터 시작 */
	struct tag_content_info	*contents;		/* smil을 파싱한 결과를 리스트로 저장, smil이 아닌 경우는 NULL이 들어감 */
	struct tag_subtitle_info *subtitle;		/* smil에서 파싱한 자막 정보를 저장, smil이 아니거나 자막이 없는 경우는 NULL이 저장됨 */
	struct tag_adaptive_info *next;
} adaptive_info_t;

#define O_FLAG_GLRU_MEDIA		0x00000001
#define O_FLAG_GLRU_BUILDER		0x00000002
#define O_FLAG_HASH_MEDIA		0x00000004
#define O_FLAG_HASH_BUILDER		0x00000008

#define IS_SET_FLAG(x,flag) 		ASSERT((x & flag) != 0)
#define IS_NOT_SET_FLAG(x,flag) 	ASSERT((x & flag) == 0)	/* 셋팅이 되어 있으면 에러 발생 */
#define TOGGLE_FLAG(x,flag) 		x ^=  flag
#define SET_FLAG(x,flag)			{IS_NOT_SET_FLAG(x,flag);TOGGLE_FLAG(x,flag);}
#define UNSET_FLAG(x,flag)		{IS_SET_FLAG(x,flag);TOGGLE_FLAG(x,flag);}
/* 스트리밍 기능이 사용될때 가장 기본적인 정보를 담고 있는 context */
typedef struct streaming_tag {
	mem_pool_t 		*mpool;
	char			*instance;		/* 통계 구분용 */
	int				media_type;		/* 미디어 스트리밍 type */
	int				protocol;		/* hls, dash, progresive, crossdomain */
	mode_t			media_mode;		/* 요청 url이 정상인지에 대한 판단에 사용 */
	int				build_type;		/* zipper.h에 정의된 값이 들어간다. */
	int				is_adaptive;	/* adaptive content일 때에는 1일 셋팅된다. */
	char *			rep_id; 		/* DASH와 HLS fmp4에서는 Representation ID, MSS,HLS ts의 adaptive streaming에서는 bitrate */
	int				rep_num;		/* CMAF DASH에서 track(audio/video) 구분을 위해 사용 */
	int				ts_num;			/* media(ts,m4s) 파일의 segment number */
	uint64_t		media_size;		/* 전송될  m3u8이나 ts파일의 전체 크기 */
	char			*argument; 		/* url에 포함된 argument,  ?를 포함한 파라미터가 들어 간다. */
	adaptive_info_t * adaptive_info;	/* adaptive mode 요청시 smil 파일 정보들을 저장 */
	char			*media_path;	/* 단순 download 서비스인 경우 media 파일의 경로 */
	int 			real_path_len;		/* request url(req->ori_url)에서 가상 파일을 제외한 파일 경로의 마지막 위치(/포함) */
	char			*lang;
	char			*key;			/* key는 lang+content로 한다. */
	float			arg_tempo;		/* argument에 포함된 tempo(오디오배속). */
	int				use_bg;			/* tempo사용시 배경음악(집중력강화) 사용여부, 1: 사용, 0: 사용안함 */
	int				session_id;		/* client의 요청에 포함된 session id, 여기에는 request에 session id가 들어온 경우에만 값이 들어가야한다. */
	int				flag;
	char			*live_path;		/* client의 요청 url에 들어 있는 라이브 path */
	int				live_real_path_len;	/* live_path에서 가상 파일을 뺀 path 길이 (마지막 '/' 포함) */
	char			*live_origin_path;	/* 라이브 오리진 서버에 요청될때 사용될 실제 path */
	struct session_info_tag *session;	/* session관련 context */
	content_info_t 	*content;
	subtitle_info_t *subtitle;
	int				subtitle_count;	/* 첫번째 smil 파일에 들어 있는 자막 stream의 수 */
	builder_info_t 	*builder;
	source_info_t 	*source;
	zipper_io_handle *ioh;
	zipperRngGet	rgCtx;
	zipper_builder_param *bprm;
	scFragment 		fragment;			/* CMAF fragment context */
	void			*options; 			/* netcache core에 요청시 새로 만들어서 한다. */
	nc_request_t 	*req;
} streaming_t;




/* streaming_reader()에서 buffer를 파라미터로 넘겨주기위한 구조체 */
typedef struct tag_buffer_param {
	nc_request_t *req;
	char * buf;
	uint32_t offset;
	uint32_t size;
	int	enable_realloc; 	/* 버퍼의 용량이 부족한 경우 alloc 가능 여부, 0:realloc 안됨, 1:realloc 가능 */
} buffer_paramt_t;

typedef struct service_cmaf_tag
{
	char * name;
	char * origin_path[2];	// cmaf 오리진에 연결하는 경로, 이 경로가 바뀌는 경우 새로 연결한다.
	uint32_t	path_hash; 	// 오리진 설정이 변경 되었는지 확인을 위해
	solCMAF ctx;
	pthread_mutex_t  *plock[cmafAdaptiveIndexMax];	/* 여기는 상황을 봐서 rw lock으로 바꿀수도 있다 */
	int		id; 	/* cmaf 구분용 */
	service_info_t *service;
    int 	ready;
    int 	expired;
    /* 아래 3개의 값은 현재 생성되고 있는 fragment 에 대한 정보를 가지고 있다. */
	uint32_t	cur_seq[cmafAdaptiveIndexMax];
	uint64_t	cur_timebase[cmafAdaptiveIndexMax];
	uint32_t	cur_pos[cmafAdaptiveIndexMax];	/* 마지막 생성 위치 */
	List	*suspend_list[cmafAdaptiveIndexMax];	/* 전송을 위해 suspend 상태에서 대기 하고 있는  client list, nc_request_t* 가 들어간다. */

//    int		manifest_updated;	/* 1로 설정 되어 있으면 manifest를 갱신 해야 한다. */
    solCMAFIO io;
    pthread_t 	monitor_tid;
    int 	available;		/* 모니터링용 thread 종료시 사용, 정상 : 1, 종료시 0으로 셋팅 */
    int		origin_timeout;		/* 오리진 연결 timeout */
    int		retry_delay;		/* 오리진 연결 실패(timeout)시 재시도 간격(밀리세컨드) */
} service_cmaf_t;


int strm_init();
void strm_deinit();
int strm_create_builder(nc_request_t *req, uint32_t dur);
streaming_t	*strm_create_streaming(nc_request_t *req);
int strm_prepare_stream(nc_request_t *req);
int strm_destroy_stream(nc_request_t *req);
int strm_live_session_check(nc_request_t *req);
int strm_live_session_destory(void *session_info);
int strm_live_session_repack(nc_request_t *req);
int strm_cmaf_check(service_info_t *service);
int strm_cmaf_create(service_info_t *service);
int strm_cmaf_close(service_info_t *service);
int strm_cmaf_stop(service_info_t *service);
int strm_cmaf_destroy(service_info_t *service);
int strm_cmaf_check_ready(nc_request_t *req);
int strm_cmaf_build_response(nc_request_t *req);
int strm_cmaf_add_supend_list(nc_request_t *req);
void strm_reset_media(media_info_t * media);
void strm_destroy_media(media_info_t * media);
int strm_get_media_attr(nc_request_t *req, int type, const char * url, struct nc_stat *objstat);
ssize_t streaming_reader(void *cr, uint64_t pos, char *buf, size_t max);
int strm_add_streaming_header(nc_request_t *req);
int strm_check_policy_file(nc_request_t *req);
int strm_host_parser(nc_request_t *req);
int strm_wowza_url_manage(nc_request_t *req);
int strm_url_parser(nc_request_t *req);
int strm_check_manifest(nc_request_t *req);
int strm_handle_adaptive(nc_request_t *req);
int strm_handle_smil(nc_request_t *req, adaptive_info_t *adaptive_info, mem_pool_t *tmp_mpool);
int strm_check_protocol_permition(nc_request_t *req);
int strm_purge(nc_request_t *req);
int strm_purge_media_info(nc_request_t *req, int type, char *path);
void strm_write_cache_status();
struct _zipper_io_handle *strm_set_io_handle(streaming_t *param);
int strm_check_media_info(nc_request_t *req, int type, media_info_t *media);
int strm_check_streaming_media_ext(nc_request_t *req);
void strm_compress_manifest(nc_request_t *req);

#endif /*__STREAMING_H__ */
