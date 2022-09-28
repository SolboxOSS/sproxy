#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <regex.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <setjmp.h>

#include <dict.h>
//#include <ncapi.h>

//#include <trace.h>
#include <microhttpd.h>
#include "common.h"
#include "scx_glru.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "limitrate.h"

#include "scx_util.h"
#include "smilparser.h"
#include "sessionmgr.h"
#include "status.h"
#include "voxio_def.h"
//#include "solCMAF.h"
//#include "err.h"
#include "scx_list.h"
#include "streaming.h"
#include "streaming_lru.h"
#include "meta_disk_cache.h"
#include "soluri2.h"
#include "scx_timer.h"
#include "md5.h"
#include "soljwt.h"

pthread_mutex_t 	gscx__media_lock = PTHREAD_MUTEX_INITIALIZER;

int 	gscx__io_buffer_size = 65536*4;	/* zipper 라이브러리에서 io 요청시 사용하는 버퍼 크기 */

int		gscx__list_count = 0;	/* media list와 delete list에 들어 있는 media의 숫자 */

extern regex_t 			*gscx__url_preg;
extern int				gscx__use_aesni;

extern volatile uint64_t gscx__media_context_size;
extern volatile int 	gscx__media_context_create_interval_cnt;
extern volatile uint64_t gscx__media_context_create_interval_size;
extern int				gscx__service_available;
extern int				gscx__enable_async;
extern __thread jmp_buf 	__thread_jmp_buf;
extern __thread int		__thread_jmp_working;

typedef struct tag_strm_reg {
	regex_t *preg;
	int	media_type;
	int	protocol;	/* hls, dash, progresive, crossdomain */
	int	build_type; /* zipper.h 에 정의된 type */
	int count;		/* 패턴매칭으로 추출해야 하는 인자 수 */
	char mime_type[32];
	char pattern[64];
} strm_reg_t;
/*
 * VOD용 가상 파일 패턴
 * */
static strm_reg_t gscx__strm_vod_reg[] = {
/* 	{preg, media_type, 						protocol, 				build_type, 	count, mime_type, 				pattern}, */
	{NULL, MEDIA_TYPE_CROSSDOMAIN, 			O_PROTOCOL_CORS,		BLDTYPE_NONE, 		0, "text/xml", 				"^crossdomain\\.xml$"},
	{NULL, MEDIA_TYPE_CLIENTACCESSPOLICY, 	O_PROTOCOL_CORS,		BLDTYPE_NONE, 		0, "text/xml", 				"^clientaccesspolicy\\.xml$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_MANIFEST, 	O_PROTOCOL_DASH,		BLDTYPE_MPD, 		0, "application/dash+xml", 	"^manifest\\.mpd$"}, /* BLDTYPE_MPD이 맞는지 나중에 확인 필요*/
	{NULL, MEDIA_TYPE_MPEG_DASH_TS_MANIFEST,O_PROTOCOL_DASH,		BLDTYPE_MPD, 		0, "application/dash+xml", 	"^manifest_ts\\.mpd$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_YT_MANIFEST,O_PROTOCOL_DASH,		BLDTYPE_MPD, 		0, "application/dash+xml", 	"^manifest_yt\\.mpd$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_VIDEO_INIT, O_PROTOCOL_DASH,		BLDTYPE_FMP4INIT, 	1, "video/mp4", 			"^([0-9]+)_video_init\\.m4s$"}, /* BLDTYPE_FMP4INIT 맞는지 확인 필요 */
	{NULL, MEDIA_TYPE_MPEG_DASH_VIDEO, 		O_PROTOCOL_DASH,		BLDTYPE_FMP4, 		2, "video/mp4", 			"^([0-9]+)_video_segment_([0-9]+)\\.m4s$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_AUDIO_INIT, O_PROTOCOL_DASH,		BLDTYPE_FMP4INIT, 	1, "audio/mp4", 			"^([0-9]+)_audio_init\\.m4s$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_AUDIO, 		O_PROTOCOL_DASH,		BLDTYPE_FMP4, 		2, "audio/mp4", 			"^([0-9]+)_audio_segment_([0-9]+)\\.m4s$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_TS_INDEX, 	O_PROTOCOL_DASH,		BLDTYPE_SIDX, 		1, "application/octet-stream", "^([0-9]+)_segment_index\\.sidx$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_TS, 		O_PROTOCOL_DASH,		BLDTYPE_STS, 		2, "video/MP2T", 			"^([0-9]+)_segment_([0-9]+)\\.ts$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_YT_VIDEO, 	O_PROTOCOL_DASH,		BLDTYPE_FMP4SINGLE, 1, "video/mp4", 			"^([0-9]+)_video_single\\.m4s$"},
	{NULL, MEDIA_TYPE_MPEG_DASH_YT_AUDIO, 	O_PROTOCOL_DASH,		BLDTYPE_FMP4SINGLE, 1, "audio/mp4", 			"^([0-9]+)_audio_single\\.m4s$"},
	{NULL, MEDIA_TYPE_MSS_MANIFEST, 		O_PROTOCOL_MSS,			BLDTYPE_IIS, 		0, "text/xml", 				"^Manifest$"},
	{NULL, MEDIA_TYPE_MSS_VIDEO, 			O_PROTOCOL_MSS,			BLDTYPE_MOOF, 		2, "video/mp4", 			"^([0-9]+)_video_segment_([0-9]+)\\.ismv$"},
	{NULL, MEDIA_TYPE_MSS_AUDIO, 			O_PROTOCOL_MSS,			BLDTYPE_MOOF, 		2, "video/mp4", 			"^([0-9]+)_audio_segment_([0-9]+)\\.isma$"},
	{NULL, MEDIA_TYPE_HLS_MAIN_M3U8, 		O_PROTOCOL_HLS, 		BLDTYPE_NONE, 		0, "application/vnd.apple.mpegurl", "^playlist\\.m3u8$"},
//	{NULL, MEDIA_TYPE_HLS_MAIN_M3U8_TS,		O_PROTOCOL_HLS|O_PROTOCOL_SEP_TRACK,		BLDTYPE_NONE, 		0, "application/vnd.apple.mpegurl", "^playlist_ts\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_SUB_M3U8, 		O_PROTOCOL_HLS, 		BLDTYPE_M3U8, 		1, "application/vnd.apple.mpegurl", "^content[_]?([0-9]*)\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_VIDEO_M3U8, 		O_PROTOCOL_HLS, 		BLDTYPE_M3U8, 		1, "application/vnd.apple.mpegurl", "^video[_]?([0-9]*)\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_AUDIO_M3U8, 		O_PROTOCOL_HLS, 		BLDTYPE_M3U8, 		1, "application/vnd.apple.mpegurl", "^audio[_]?([0-9]*)\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_VTT_M3U8, 		O_PROTOCOL_HLS, 		BLDTYPE_M3U8, 		1, "application/vnd.apple.mpegurl", "^subtitle[_]?([0-9]*)\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_FMP4_MAIN_M3U8, 	O_PROTOCOL_HLS, 		BLDTYPE_FMP4M3U8, 	0, "application/vnd.apple.mpegurl", "^playlist_fmp4\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_FMP4_AUDIO_M3U8, 	O_PROTOCOL_HLS, 		BLDTYPE_FMP4SUBM3U8,2, "application/vnd.apple.mpegurl", "^fmp4_audio[_]?([0-9]*)\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_FMP4_VIDEO_M3U8, 	O_PROTOCOL_HLS, 		BLDTYPE_FMP4SUBM3U8,2, "application/vnd.apple.mpegurl", "^fmp4_video[_]?([0-9]*)\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_TS, 				O_PROTOCOL_HLS,			BLDTYPE_STS, 		2, "video/MP2T", 			"^content[_]?([0-9]*)_([0-9]+)\\.ts$"},
	{NULL, MEDIA_TYPE_HLS_VIDEO_TS, 		O_PROTOCOL_HLS,			BLDTYPE_STS, 		2, "video/MP2T", 			"^video[_]?([0-9]*)_([0-9]+)\\.ts$"},
	{NULL, MEDIA_TYPE_HLS_AUDIO_TS, 		O_PROTOCOL_HLS,			BLDTYPE_STS, 		2, "video/MP2T", 			"^audio[_]?([0-9]*)_([0-9]+)\\.ts$"},
	{NULL, MEDIA_TYPE_HLS_VTT, 				O_PROTOCOL_HLS, 		BLDTYPE_VTT, 		2, "text/vtt", 				"^subtitle[_]?([0-9]*)_([0-9]+)\\.vtt$"},
	{NULL, MEDIA_TYPE_MP4, 					O_PROTOCOL_PROGRESSIVE,	BLDTYPE_MP4, 		0, "video/mp4", 			"^content\\.mp4$"},
	{NULL, MEDIA_TYPE_M4A, 					O_PROTOCOL_PROGRESSIVE,	BLDTYPE_MP4, 		0, "audio/mp4", 			"^content\\.m4a$"},
	{NULL, MEDIA_TYPE_M4V, 					O_PROTOCOL_PROGRESSIVE,	BLDTYPE_MP4, 		0, "video/mp4", 			"^content\\.m4v$"},
	{NULL, MEDIA_TYPE_MP3, 					O_PROTOCOL_PROGRESSIVE,	BLDTYPE_MP3, 		0, "audio/mpeg", 			"^content\\.mp3$"},
	{NULL, MEDIA_TYPE_FLV, 					O_PROTOCOL_PROGRESSIVE,	BLDTYPE_FLV, 		0, "video/x-flv", 			"^content\\.flv$"},
	{NULL, MEDIA_TYPE_FLAC, 				O_PROTOCOL_PROGRESSIVE,	BLDTYPE_FLAC, 		0, "audio/flac", 			"^content\\.flac$"},
	{NULL, MEDIA_TYPE_MP4_DOWN,				O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "video/mp4", 			"^.+\\.[mM][pP]4$"}, /* content.mpX로 끝나지 않는 mpX파일은 일반 다운로드이다. 이부분이 항상 마지막에 와야 한다. */
	{NULL, MEDIA_TYPE_M4A_DOWN,				O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "audio/mp4", 			"^.+\\.[mM]4[aA]$"},
	{NULL, MEDIA_TYPE_M4V_DOWN,				O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "video/mp4", 			"^.+\\.[mM]4[vV]$"},
	{NULL, MEDIA_TYPE_MP3_DOWN,				O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "audio/mpeg", 			"^.+\\.[mM][pP]3$"},
	{NULL, MEDIA_TYPE_FLAC_DOWN,			O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "audio/flac", 			"^.+\\.[fF][lL][aA][cC]"},
#ifdef ZIPPER
	// solproxy에서 아래 부분이 있을경우 smil 파일 자체로 퍼지가 안되는 문제 발생
	// https://jarvis.solbox.com/redmine/issues/33477
	{NULL, MEDIA_TYPE_SMIL_DOWN,			O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "application/smil+xml", 	"^.+\\.[sS][mM][iI][lL]"},	/* 이 type은 캐싱된 smil 퍼지 용도로만 사용 */
	{NULL, MEDIA_TYPE_SUBTITLE_DOWN,		O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "text/plain", 	"^.+\\.[sS][rR][tT]"},	/* 이 type은 캐싱된 srt 파일 퍼지 용도로만 사용 */
	{NULL, MEDIA_TYPE_SUBTITLE_DOWN,		O_PROTOCOL_PROGRESSIVE,	BLDTYPE_NONE, 		0, "text/plain", 	"^.+\\.[vV][tT][tT]"},	/* 이 type은 캐싱된 vtt 퍼지 용도로만 사용 */
#endif
	{NULL, MEDIA_TYPE_CMAF_VIDEO_TUNNEL,	O_PROTOCOL_TUNNEL,		BLDTYPE_NONE, 		1, "video/mpeg", 			"^([0-9]+)_video_tunnel\\.cmaf$"},
	{NULL, MEDIA_TYPE_CMAF_AUDIO_TUNNEL,	O_PROTOCOL_TUNNEL,		BLDTYPE_NONE, 		1, "audio/mpeg", 			"^([0-9]+)_audio_tunnel\\.cmaf$"},
	{NULL, MEDIA_TYPE_ENC_KEY,				O_PROTOCOL_HLS,			BLDTYPE_NONE, 		0, "binary/octet-stream",	"^decrypt\\.key$"},
	{NULL, MEDIA_TYPE_MODIFY_HLS_MANIFEST,	O_PROTOCOL_HLS, 		BLDTYPE_NONE, 		0, "application/vnd.apple.mpegurl", "ddddddddddddddd" /* 패턴 매칭이 필요 없어서 이렇게 표기함. */ },
	{NULL, MEDIA_TYPE_DOWN,					O_PROTOCOL_HLS,			BLDTYPE_NONE, 		0, 	"", 					"ddddddddddddddd" /* 패턴 매칭이 필요 없어서 이렇게 표기함. */ },
	{NULL, MEDIA_TYPE_FCS_IDENT, 			O_PROTOCOL_CORS,		BLDTYPE_NONE, 		0, "text/html", 			"^/fcs/$"} /* 패턴은 사용안함 */
};


/* Live ad stitching 용 가상 파일 패턴 */
static strm_reg_t gscx__strm_live_reg[] = {
/* 	{preg, media_type, 						protocol, 				build_type, 	count, mime_type, 				pattern}, */
	{NULL, MEDIA_TYPE_CROSSDOMAIN, 			O_PROTOCOL_CORS,		BLDTYPE_NONE, 		0, "text/xml", 				"^crossdomain\\.xml$"},
	{NULL, MEDIA_TYPE_CLIENTACCESSPOLICY, 	O_PROTOCOL_CORS,		BLDTYPE_NONE, 		0, "text/xml", 				"^clientaccesspolicy\\.xml$"},
	{NULL, MEDIA_TYPE_HLS_MAIN_M3U8, 		O_PROTOCOL_HLS, 		BLDTYPE_NONE, 		0, "application/vnd.apple.mpegurl", "^playlist\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_SUB_M3U8, 		O_PROTOCOL_HLS, 		BLDTYPE_NONE, 		0, "application/vnd.apple.mpegurl", "^[a-zA-Z0-9._\\-]+\\.m3u8$"},
	{NULL, MEDIA_TYPE_HLS_TS, 				O_PROTOCOL_HLS,			BLDTYPE_STS, 		0, "video/MP2T", 			"^[a-zA-Z0-9._\\-]+\\.ts$"},
	{NULL, MEDIA_TYPE_FCS_IDENT, 			O_PROTOCOL_CORS,		BLDTYPE_NONE, 		0, "text/html", 			"^/fcs/$"}
};

const int gscx__count_strm_vod_reg = howmany(sizeof(gscx__strm_vod_reg), sizeof(strm_reg_t));
const int gscx__count_strm_live_reg = howmany(sizeof(gscx__strm_live_reg), sizeof(strm_reg_t));

#ifdef ZIPPER
/* '/[application]/[instance]/[protocol]/[lang]/[version]/[content]/[meta]/[content]/[meta]/[virtual_file]?[argument]' */
//const char gscx__strm_url_pattern[] = {"^\\/+([a-zA-Z0-9.\\-_]+)\\/+([a-zA-Z0-9.\\-_]+)\\/+([a-zA-Z0-9.\\-_]+)\\/+([a-zA-Z0-9.\\-_]+)\\/+" /*여기까지가 lang까지 */
					//"[/a-zA-Z0-9.\\-_]+(/[a-zA-Z0-9\\-_]+\\.[a-zA-Z0-9]+)(\\?.*)?$"};
const char gscx__strm_url_pattern[] = {
	"^/+([a-zA-Z0-9._\\-]+)/+([a-zA-Z0-9._\\-]+)/+([a-zA-Z0-9\\-]+)/+([a-zA-Z]+)/+([a-zA-Z0-9._\\-]+)"
	"(/*.*)" /* 인코딩된 한글 경로 들이 들어 올수도 있어서 변경함 */ //"(/*[/a-zA-Z0-9\\-]+)" 	/* contents 경로들 */
	"/+([^?]+\\.?[a-zA-Z0-9]+)"  /* virtual file */
	//"/([a-zA-Z0-9_\\-]+)\\.([a-zA-Z0-9]+)"  /* virtual file name,format */
	"(\\?{1}.*)?$"		/* argument parameter */
	};
#else
/* '/[content path]/[virtual_file]?[argument]' */
#if 0
const char gscx__strm_url_pattern[] = {
		"^(.*\\.[Mm][Pp][34]|.*\\.[mM][oO][vV]|.*\\.[mM][kK][vV]|.*\\.[mM][4][vVaA]|.*\\.[fF][lL][aA][cC]|.*\\.[sS][mM][iI][lL])"  /* contents 경로 */
	"/+([^?^/]+\\.?[a-zA-Z0-9]+)"  /* virtual file */
	//"/([a-zA-Z0-9_\\-]+)\\.([a-zA-Z0-9]+)"  /* virtual file name,format */
	"(\\?{1}.*)?$"          /* argument parameter */
	};

#else
/*
 * 가상 파일명 없이 progressive download 서비스하는 경우 url 패턴.
 * 이때 가장 문제가 실제 원본의 경로가 /mp4/content.mp4와 같은 경우 미리보기를 지원할수 없다.
 * https://jarvis.solbox.com/redmine/issues/33050
 */
const char gscx__strm_url_pattern[] = {
	"^(.*)"  /* contents 경로 */
	"/+([^?^/]+\\.?[a-zA-Z0-9]+)+"  /* virtual file */
	"(\\?{1}.*)?$"		/* argument parameter */
	};
#endif
#endif

/*
 * 와우자 url 패턴
 * https://jarvis.solbox.com/redmine/issues/33317
 */
const char gscx__wowza_url_pattern[] = {
	"^/+([a-zA-Z0-9._\\-]+)/+([a-zA-Z0-9._\\-]+)/"	/* application / instance */
	"(.*)"  /* contents 경로(처음의 /가 제외 되기 때문에 따로 처리 해줘야 한다.) */
	"/+([^?^/]+\\.?[a-zA-Z0-9]+)+"  /* virtual file */
	"(\\?{1}.*)?$"		/* argument parameter */
	};

#define MEDIA_FORMAT_MAX_LEN 4096
#define MEDIA_ARGUMENT_MAX_LEN MEDIA_FORMAT_MAX_LEN-32
const char gscx__hls_ts_format[] = {"content_%i.%e"};	//TS 파일명의 규칙 format
const char gscx__hls_adaptive_ts_format[] = {"content_%d_%i.%e"};	//adaptive TS 파일명의 규칙 format
const char gscx__hls_adaptive_vtt_format[] = {"subtitle_%d_%i.%e"};	//adaptive TS 파일명의 규칙 format
const char gscx__hls_extinf_format[] = {"#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1280000\n"};	//m3u8생성 규칙 format
const char gscx__hls_m3u8_format[] = {"content.%e"};	//main m3u8생성 규칙 format
const char gscx__hls_fmp4_audio_m3u8_format[] = {"fmp4_audio_%a.%e"};	// HLS fmp4 audio manifest naming 규칙
const char gscx__hls_fmp4_video_m3u8_format[] = {"fmp4_video_%a.%e"};	// HLS fmp4 video manifest naming 규칙
const char gscx__hls_fmp4_subtitle_m3u8_format[] = {"subtitle_%a.%e"};	// HLS fmp4 subtitle manifest naming 규칙
const char gscx__hls_fmp4_video_init_format[] 	= {"%a_video_init.m4s"};			//HLS fmp4 video init 파일 naming 규칙
const char gscx__hls_fmp4_audio_init_format[] 	= {"%a_audio_init.m4s"};			//HLS fmp4 audio init 파일 naming 규칙
#if 0
const char gscx__hls_fmp4_video_seq_format[] 	= {"%a_video_segment_%t.m4s"};	//HLS fmp4 video segment 파일 naming 규칙
const char gscx__hls_fmp4_audio_seq_format[] 	= {"%a_audio_segment_%t.m4s"};	//HLS fmp4 audio init 파일 naming 규칙
#else
const char gscx__hls_fmp4_video_seq_format[] 	= {"%a_video_segment_%i.m4s"};	//HLS fmp4 video segment 파일 naming 규칙
const char gscx__hls_fmp4_audio_seq_format[] 	= {"%a_audio_segment_%i.m4s"};	//HLS fmp4 audio init 파일 naming 규칙
#endif
const char gscx__hls_adaptive_extinf_format[] = {"#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%d"};	//m3u8생성 규칙 format
const char gscx__hls_adaptive_m3u8_format[] = {"content_%d.%e"};	//adaptive main m3u8생성 규칙 format
const char gscx__dash_video_init_format[] 	= {"$RepresentationID$_video_init.m4s"};	//DASH 생성 규칙 format
const char gscx__dash_audio_init_format[] 	= {"$RepresentationID$_audio_init.m4s"};	//DASH 생성 규칙 format
#if 0
const char gscx__dash_video_seq_format[] 	= {"$RepresentationID$_video_segment_$Time$.m4s"};	//DASH 생성 규칙 format
const char gscx__dash_audio_seq_format[] 	= {"$RepresentationID$_audio_segment_$Time$.m4s"};	//DASH 생성 규칙 format
#else
const char gscx__dash_video_seq_format[] 	= {"$RepresentationID$_video_segment_$Number$.m4s"};	//DASH 생성 규칙 format
const char gscx__dash_audio_seq_format[] 	= {"$RepresentationID$_audio_segment_$Number$.m4s"};	//DASH 생성 규칙 format
#endif
const char gscx__dash_ts_index_format[] 	= {"$RepresentationID$_segment_index.sidx"};
#if 0
const char gscx__dash_ts_format[] 			= {"$RepresentationID$_segment_$Time$.ts"};
#else
const char gscx__dash_ts_format[] 			= {"$RepresentationID$_segment_$Number$.ts"};
#endif
const char gscx__dash_yt_video_format[] 	= {"%a_video_single.m4s"};	//DASH 생성 규칙 format
const char gscx__dash_yt_audio_format[] 	= {"%a_audio_single.m4s"};	//DASH 생성 규칙 format
const char gscx__mss_video_format[] 		= {"{bitrate}_video_segment_{start time}.ismv"};	//Smooth Streaming 생성 규칙 format
const char gscx__mss_audio_format[] 		= {"{bitrate}_audio_segment_{start time}.isma"};	//Smooth Streaming 생성 규칙 format

const char gscx__strm_crossdomain_pattern[] = {".*crossdomain\\.xml(\\?{1}.*)?$"};
const char gscx__strm_clientaccesspolicy_pattern[] = {".*clientaccesspolicy\\.xml(\\?{1}.*)?$"};

/* contents 패턴
 * 예) 	"/2f696e666f2f7072655f373230702e6d7034/0-0/2f61642f666c7962655f373230702ee6d7023/0-0"
 * 		"/2f696e666f2f7072655f373230702e6d7034/0-0/2f61642f666c7962655f373230702ee6d7023/0-0"
 */
const char gscx__strm_content_pattern[] = {"^/*([a-fA-F0-9]+)/+([0-9])-?([0-9]+)?-?([0-9]*)-?([a-zA-Z0-9]*)/*(.*)$"};


regex_t 	*gscx__strm_url_preg = NULL;
regex_t 	*gscx__strm_crossdomain_preg = NULL;
regex_t 	*gscx__strm_clientaccesspolicy_preg = NULL;
regex_t 	*gscx__strm_content_preg = NULL;

void * strm_cmaf_malloc(size_t size);
void * strm_cmaf_calloc(size_t size, size_t multiply);
void * strm_cmaf_realloc(void *buf, size_t newsize);
void strm_cmaf_free(void *buf);
void * strm_malloc(size_t size, void *param);
void * strm_calloc(size_t size, size_t multiply, void *param);
void * strm_realloc(void *buf, size_t newsize, void *param);
void strm_free(void *buf, void *param);
void *strm_palloc(size_t size, void **pool, void *param);
void strm_pfree(void *pool, void *param);
int strm_strcmp(const void *k1, const void *k2);
unsigned strm_create_hash(const void *p);
void strm_media_key_val_free(void *key, void *datum);
void strm_builder_key_val_free(void *key, void *datum);
ssize_t strm_source_reader(void *context, void *buf, size_t size, off_t offset, void *param);
ssize_t strm_source_size(void *context,  void *param);
source_info_t * strm_create_source_info(nc_request_t *req, int type, char *url, zipperCnt context);

source_info_t * strm_find_source_info(streaming_t *param, zipperCnt context);
void strm_destroy_source_info(nc_request_t *req, source_info_t *source);
int strm_set_io_memfunc(zipper_io_handle *ioh);
void strm_destroy_io_handle(zipper_io_handle *ioh);
media_info_t * strm_create_media(nc_request_t *req, int type, char *url);
int strm_set_media(nc_request_t *req, media_info_t 	*media,  int type, char *url, zipperCnt ctx, struct nc_stat *objstat);
int strm_set_objstat(nc_request_t *req);
int strm_write_func(unsigned char *block, size_t size, void *param);
static void strm_make_x_play_durations_header(streaming_t *streaming);
zipperBldr strm_live_create_preroll_bldr_callback(uint32_t dur, void *param);
void strm_live_expire_preroll_bldr_callback(zipperBldr bldr, void *param);
int strm_live_create_context(nc_request_t *req);
void * strm_cmaf_monitor(void *d);
void strm_destroy_builder(builder_info_t * builder);
void strm_expire_medialist(nc_request_t *req);
int strm_regex_compile();
int strm_regex_free();
mode_t strm_protocol_parser(nc_request_t *req, char *protocol);
int strm_content_parser(nc_request_t *req, char *contents,  void *mpool);
char *strm_smil_read(nc_request_t *req, adaptive_info_t *adaptive_info, void *tmp_mpool);
int strm_media_type_parser(nc_request_t *req, char *virtualfile);
int strm_get_track_id(nc_request_t *req);
int strm_make_path(nc_request_t *req,  int type, const char * url, char *buf, int length);
void *strm_file_open(nc_request_t *req, int type, const char * url, struct nc_stat *objstat, mode_t mode);
void strm_file_close(nc_request_t *req, void * file);
ssize_t strm_file_read(nc_request_t *req, void * file, void *buf, off_t offset, size_t toread);
int strm_file_errno(nc_request_t *req);
int strm_make_enc_key(nc_request_t *req,  char *buf);

int strm_destroy_streaming(nc_request_t *req);

int
strm_init()
{
	int	rv_uint = 0;

//	sm_init();	/* module 구조로 변경 됨에 따라 여기서 실행할 필요가 없다. */

	/* dash 패턴 매칭용 compiled form 생성 */
	if (strm_regex_compile() == 0) {
		return 0;
	}
	smil_parser_init();

	gscx__io_buffer_size = gscx__config->io_buffer_size * 1024;

	scx_lru_init();

	return 1;
}

void
strm_deinit()
{

//	sm_deinit();	/* module 구조로 변경 됨에 따라 여기서 실행할 필요가 없다. */
	/* 패턴 매칭용 compiled form 삭제 */
	strm_regex_free();
	smil_parser_deinit();
	scx_lru_destroy();
}


void *
strm_cmaf_malloc(size_t size)
{
	return SCX_MALLOC(size);
}

void *
strm_cmaf_calloc(size_t size, size_t multiply)
{
    return SCX_CALLOC(size, multiply);
}

void *
strm_cmaf_realloc(void *buf, size_t newsize)
{
    return SCX_REALLOC(buf, newsize);
}

void
strm_cmaf_free(void *buf)
{
	SCX_FREE(buf);
}


void *
strm_malloc(size_t size, void *param)
{
	return SCX_MALLOC(size);
}

void *
strm_calloc(size_t size, size_t multiply, void *param)
{
    return SCX_CALLOC(size, multiply);
}

void *
strm_realloc(void *buf, size_t newsize, void *param)
{
    return SCX_REALLOC(buf, newsize);
}

void
strm_free(void *buf, void *param)
{
	SCX_FREE(buf);
}

void *
strm_palloc(size_t size, void **pool, void *param)
{
	mem_pool_t *ptp;

    if((*pool) == NULL) {

        ptp = mp_create(65536);
        ASSERT(ptp);

        *pool = (void *)ptp;
    }
    else {
    	ptp = (mem_pool_t *)(*pool);
    }

    return (void *)mp_alloc(ptp, size);
}

void
strm_pfree(void *pool, void *param)
{
	if (pool) mp_free(pool);
}

/*
 * context가 NULL인 경우는  zipper_create_media_context에서 readfp의 콜백으로 호출되는 경우이고
 * context에 값이 들어 있는 경우는  zipper_build()에서  콜백으로 호출 되는 경우이다.
 * zipper_create_media_context()에서 호출 되는 경우는 param에 source_info_t *가 들어 있고
 * zipper_build()에서 호출 되는 경우에는 builder_info_t * 가 들어 있다
 * 이 함수 내에서 source info를 생성하는 방향으로 개발 해야 함.
 */
ssize_t
strm_source_reader(void *context, void *buf, size_t size, off_t offset, void *param)
{
	source_info_t 	*source = NULL;
	nc_request_t 	*req = NULL;
	builder_info_t 	*builder = NULL;
	streaming_t *streaming = NULL;
	ssize_t 		copied = 0;
	size_t 			toread;
	int				using_j_enable = 0;

	if (__thread_jmp_working == 1) {
		/* zipper library를 벗어나서 SIGSEGV 발생하는 경우는 죽어야 하므로 gscx__j_enable을 0으로 바꾼다. */
		using_j_enable = 1;
		__thread_jmp_working = 0;
	}
	streaming = (streaming_t *) param;
	req = (nc_request_t *)streaming->req;
	if(context == NULL) {
		/*
		 * context가 NULL인 경우는  zipper_create_media_context()에서 readfp의 콜백으로 호출되는 경우이다.
		 * 이 경우는 source_info_t 가 한개만 들어 있기 때문에 검색이 필요 없다.
		 */
		source = streaming->source;
		//req = (nc_request_t *)source->req;
	}
	else {
		/*
		 * context에 값이 들어 있는 경우는  zipper_build()에서  콜백으로 호출 되는 경우이다.
		 * 이 경우는 param에 builder_info_t * 가 들어 있다.
		 * builder_info_t내에서 context의 정보를 가지고 있는
		 */
		source = strm_find_source_info(streaming, (zipperCnt)context);
	}
	if (source == NULL) {
		scx_error_log(req, "Failed to find source info.\n");
    	copied = 0;
    	goto strm_source_reader_end;
	}

    if(offset <= source->objstat.st_size) {
       	//offset + size가 파일의 크기 보다 큰 경우 파일 크기 내에서만 읽는다;
		toread = min(size, source->objstat.st_size - offset);

		copied = strm_file_read(req, source->file, buf, offset, toread);
    }
    else {
    	scx_error_log(req, "Invalid request offset. file(%lld),offset(%lld)\n",source->objstat.st_size, offset);
    	copied = 0;
    	goto strm_source_reader_end;
    }
//    printf("strm_source_reader offset = %d, size = %d, calsize = %d, copied = %d, toread = %d\n",
//    		offset,size, offset+size, copied, toread);

strm_source_reader_end:
	if(using_j_enable == 1) {
		__thread_jmp_working = 1;
	}
    return copied;
}

/*
 * 원본 동영상의 크기를 리턴한다.
 */
ssize_t
strm_source_size(void * context,  void *param)
{
	source_info_t 	*source = NULL;
	nc_request_t 	*req = NULL;
	builder_info_t 	*builder = NULL;
	streaming_t *streaming = NULL;
	ssize_t 		src_size = 0;
	double 			ts, te;
	size_t 			toread;
	int	using_j_enable = 0;

	if (__thread_jmp_working == 1) {
		/* zipper library를 벗어나서 SIGSEGV 발생하는 경우는 죽어야 하므로 gscx__j_enable을 0으로 바꾼다. */
		using_j_enable = 1;
		__thread_jmp_working = 0;
	}
	if(context == NULL) {
		/*
		 * context가 NULL인 경우는  zipper_create_media_context()에서 readfp의 콜백으로 호출되는 경우이다.
		 * 이 경우는 source_info_t 가 한개만 들어 있기 때문에 검색이 필요 없다.
		 */
		streaming = (streaming_t *) param;
		source = streaming->source;

	}
	else {
		streaming = (streaming_t *) param;

		source = strm_find_source_info(streaming,(zipperCnt) context);
	}
	if (source == NULL) {
		src_size = 0;
	}
	else {
		src_size = source->objstat.st_size;
	}
	if(using_j_enable == 1) {
		__thread_jmp_working = 1;
	}
	return src_size;

}

/*
 * strm_find_source_info()에서 호출 되는 경우에는  context가 들어오고
 * strm_create_media()에서 호출 되는 경우에는  context가 NULL이다.
 */
source_info_t *
strm_create_source_info(nc_request_t *req, int type, char *url, zipperCnt context)
{
	streaming_t		*streaming = req->streaming;
	source_info_t *source = NULL;
	source_info_t *head_source = NULL;
	source_info_t *prev = NULL;
	service_info_t *service = req->service;
	void * file = NULL;
	int		source_alocated = 0;
	mode_t 	mode = O_RDONLY;

	mem_pool_t 			*mpool = NULL;

	if (context == NULL || req->streaming->source == NULL) {
		/* strm_find_source_info()에서 첫번째  호출시나
		 * strm_create_media()에서 호출 되는 경우는 이곳만 실행된다 */

		mpool = mp_create(sizeof(mem_pool_t) + 2048);
		ASSERT(mpool);
		source = (source_info_t *)mp_alloc(mpool,sizeof(source_info_t));
		ASSERT(source);
		source->mpool = mpool;
		if(context != NULL) {
			/* strm_find_source_info()에서는  호출 될때는 source 정보를 따로 저장한다. */
			req->streaming->source = source;
			source_alocated = 1;
		}
	}
	else {
		 /* strm_find_source_info()에서 두번째 호출 될때 부터는 이곳이 실행된다. */
		source = head_source = req->streaming->source;

		while (source->next) {
			source = source->next;
		}
		source->next = (source_info_t *)mp_alloc(head_source->mpool,sizeof(source_info_t));
		ASSERT(source->next);
		source = source->next;
		source->mpool = NULL;
	}
#if 0
	/* strm_file_open()에서 중복 구현 되어 있는 부분임 */
	if(service->origin_hostname) {	/* origin 요청시 host 헤더를 변경하는 경우 */
		kv_replace(streaming->options, MHD_HTTP_HEADER_HOST, vs_data(service->origin_hostname), FALSE);
	}
#ifndef ZIPPER
	/* streaming의 경우에는 host가 도메인이 아니라 직접 전달을 할수 없다. */
	else {	/* client의 요청에 포함된 host 헤더를 전달 */
		kv_replace(streaming->options, MHD_HTTP_HEADER_HOST, host, FALSE);
	}
#endif
#endif
	/*
	 * hls manifest 수정 기능을 제외하고는 모두 원본 파일을 range로 읽는다.
	 * dash manifest 수정 기능이 개발되면 그 경우도 추가해 주어야 한다.
	 */
	if (req->service->hls_modify_manifest_enable == 1) {
		mode = O_RDONLY|O_NCX_NORANDOM;
	}

	file = strm_file_open(req, type, url, &source->objstat, mode);

	if (file == NULL) {
		scx_error_log(req, "URL[%s] - open error (%d)\n", url, strm_file_errno(req));
		req->p1_error = MHD_HTTP_NOT_FOUND;
		goto set_origin_info_error;
	}
	source->file = file;
	source->mcontext = context;
	source->next = NULL;
	return source;
set_origin_info_error:
	if (file) {
		strm_file_close(req, file);
	}

	if (mpool) mp_free(mpool);
	if (source_alocated) req->streaming->source = NULL;
	return NULL;
}

/*
 * 원본 파일을 열어서 파일의 정보를 objstat에 저장한다.
 */
int
strm_get_media_attr(nc_request_t *req, int type, const char * url, struct nc_stat *objstat)
{
	streaming_t		*streaming = req->streaming;
	service_info_t *service = req->service;
	void 	*file = NULL;
	int 	ret = 0;
#if 0
	/* strm_file_open()에서 중복 구현 되어 있는 부분임 */
	if(service->origin_hostname) {	/* origin 요청시 host 헤더를 변경하는 경우 */
		kv_replace(streaming->options, MHD_HTTP_HEADER_HOST, vs_data(service->origin_hostname), FALSE);
	}
#ifndef ZIPPER
	/* streaming의 경우에는 host가 도메인이 아니라 직접 전달을 할수 없다. */
	else {	/* client의 요청에 포함된 host 헤더를 전달 */
		kv_replace(streaming->options, MHD_HTTP_HEADER_HOST, host, FALSE);
	}
#endif
#endif

	/* 원본 파일을 range로 읽는다. */
	file = strm_file_open(req, type, url, objstat, O_RDONLY);

	if (file == NULL) {
		scx_error_log(req, "Virtual host(%s), URL[%s] - open error (%d)\n", vs_data(service->name), url, strm_file_errno(req));
		req->p1_error = MHD_HTTP_NOT_FOUND;
		goto end_get_media_attr;
	}

	strm_file_close(req, file);
	ret = 1;
end_get_media_attr:
	return ret;
}

source_info_t *
strm_find_source_info(streaming_t *param, zipperCnt context)
{
	builder_info_t 	*builder = param->builder;
	source_info_t 	*source = param->source;
	media_info_t 	*media = NULL;
	nc_request_t *req = (nc_request_t *)param->req;
	media_info_list_t * media_list = builder->media_list;
	while (source) {
		/* source info가 만들어진게 있으면 해당 source info를 리턴한다. */
		if (source->mcontext == context) {
			return source;
		}
		source = source->next;
	}
	/* source info가 없는 경우 새로 생성한다. */
	/* 먼저 context에 해당하는 media info를 찾는다. */
	while (media_list) {

		if (media_list->media->mcontext == context) {
			media = media_list->media;
			break;
		}
		media_list = media_list->next;
	}
	if (media == NULL)
		return NULL;
	//ASSERT(media);
	source = strm_create_source_info(param->req, media->type, media->url, context);
	if (source == NULL) {
		return NULL;
	}
	/* 도중에 파일이 변경 되는 경우가 있어서 source를 만든후 다시 파일 정보를 metadata의 정보와 비교한다. */
	if (media->mtime != source->objstat.st_mtime) {
		media->available = 0;
		TRACE((T_INFO, "[%llu] media info mtime changed. cache(%ld), changed(%ld), path(%s)\n", req->id, media->mtime, source->objstat.st_mtime, media->url));
		scx_error_log(req, "media info mtime changed. cache(%ld), changed(%ld), path(%s)\n", media->mtime, source->objstat.st_mtime, media->url);

		return NULL;
	}
	else if (media->msize != source->objstat.st_size) {
		media->available = 0;
		TRACE((T_INFO, "[%llu] media info size changed. cache(%lld), changed(%lld), path(%s)\n", req->id, media->msize, source->objstat.st_size, media->url));
		scx_error_log(req, "media info size changed. cache(%ld), changed(%ld), path(%s)\n", media->msize, source->objstat.st_size, media->url);
		return NULL;
	}
	else if(strncmp(media->st_devid, source->objstat.st_devid, 128) != 0) {
		media->available = 0;
		TRACE((T_INFO, "[%llu] media info ETag changed. cache(%s), changed(%s), path(%s)\n", req->id, media->st_devid, source->objstat.st_devid, media->url));
		scx_error_log(req, "media info ETag changed. cache(%s), changed(%s), path(%s)\n", media->st_devid, source->objstat.st_devid, media->url);
		return NULL;
	}
	return source;
}

void
strm_destroy_source_info(nc_request_t *req, source_info_t *source)
{
	void 	*file = NULL;
	mem_pool_t 		*mpool = NULL;

	source_info_t *cur = source;
	while (cur) {
		file = cur->file;
		if (file) {
			strm_file_close(req, file);
			cur->file = NULL;
		}
		cur = cur->next;
	}

	if (source->mpool) {
		mpool = source->mpool;
		source->mpool = NULL;
		mp_free(mpool);
	}
}

/*
 * io handle는 http 연결 마다 새로 만들어야 한다.
 * zipper_create_media_context()에서 호출 되는 경우는 param에 source_info_t *가 들어 있고
 * zipper_build()에서 호출 되는 경우에는 builder_info_t * 가 들어 있다
 */
struct _zipper_io_handle *
strm_set_io_handle(streaming_t *param)
{
	zipper_io_handle *ioh = NULL;

	ioh = (zipper_io_handle *)SCX_CALLOC(1, sizeof(zipper_io_handle));
	ASSERT(ioh);
    ioh->reader.sizefp = strm_source_size;
    ioh->reader.readfp = strm_source_reader;
    ioh->reader.param = param;
    if (param != NULL) {
		ioh->bufsize = gscx__io_buffer_size;
		ioh->readbuf.data = (unsigned char *)SCX_MALLOC(ioh->bufsize);
    }
    ioh->writer.fp = strm_write_func;

    strm_set_io_memfunc(ioh);
	return ioh;
}

/*
 * zipper_io_handle의 memory 관련 callback function 등록
 */
int
strm_set_io_memfunc(zipper_io_handle *ioh)
{
	ioh->memfunc.malloc = strm_malloc;
	ioh->memfunc.calloc = strm_calloc;
	ioh->memfunc.realloc = strm_realloc;
	ioh->memfunc.free = strm_free;
	ioh->memfunc.pool.alloc = strm_palloc;
	ioh->memfunc.pool.free = strm_pfree;
	ioh->memfunc.param = NULL;
	return 1;
}

void
strm_destroy_io_handle(zipper_io_handle *ioh)
{
	streaming_t *streaming = NULL;
	if (NULL != ioh->readbuf.data) {
		SCX_FREE(ioh->readbuf.data);
	}

	if (ioh->reader.param) {
		streaming = (streaming_t *) ioh->reader.param;
	}
	SCX_FREE(ioh);

}

/*
 * builder를 생성하는 과정에서 처음  media 정보 참조할때 사용된다.
 * 지정된 host와 url이 동일한 media 정보가 있는지 확인하고
 * 있으면 찾은 정보를 리턴하고
 * 없는 경우는 새로 생성한후 리턴한다.
 * 배속 오디오 파일을 open 하는 경우에는 zatt에 1을 입력한다.
 * type : 0:VOD, 1:광고, 2:LIVE
 */
media_info_t *
strm_create_media(nc_request_t *req, int type, char *url)
{
	/* scx_media_dct로부터 정보가 있는지 확인 하고 없으면 새로 생성 하는 부분이 있어야 함 */
	media_info_t 	*media = NULL;
	zipperCnt 	ctx = NULL;
	zipper_io_handle  *ioh = NULL;
	streaming_t		*param = NULL;
	int				ret = 0;
	int				gres = 0;
	source_info_t *source = NULL;
	double 			ts, te;
	struct nc_stat 		mdc_objstat;
	zipperCnt mdc_context = NULL;
	int		must_update = 0;
//	jb = SCX_MALLOC(sizeof(jmp_buf));
	/* 여기에서 key(host+url)를 만드는 과정 필요 */
	while (1) {

		printf("strm_create_media() called, url(%s)\n", url);

		if (type == O_CONTENT_TYPE_SUBTITLE) {
			// 자막은 cache를 하지 않기 때문에 아래 부분들은 skip하고 source 생성부터 진행한다.
			media = strm_allocate_media(req, &gres);
			goto next_create_media;
		}
		/* strm_find_media()을 호출한후 부터 commit이 일어날때 까지는 동일 media는 오직 한 세션만 진입이 가능하다. */
		media = strm_find_media(req, type, url, U_TRUE, U_TRUE, &gres);
		if (media == NULL) {
			/*
			 * 캐싱이 된 상태에서 원본 파일이 변경된 경우이거나
			 * media cache가 모두 사용중이라 새로 추가가 불가능한 경우
			 * 배속 audio 요청인 경우
			 * media를 glru에서 할당 받지 않고 직접 생성한다.
			 */
			media = strm_allocate_media(req, &gres);
		}
		if (gres == GLRUR_FOUND )  {
			break;
		}

		/* else if를 안쓴 이유는 위에서 캐싱되었지만 컨텐츠에 문제가 있어  strm_reset_media()가 호출 되는 경우가 있기 때문임 */
		if (gres == GLRUR_ALLOCATED || gres == GRLUR_RESETTED) {
			/* 여기에 들어오는 경우는 media가 새로 만들어 졌거나 원본이 변경 되어서 새로 할당된 경우이다. */

			/*
			 * disk cache가 있는지 확인
			 * cache가 있는 경우 유효성 검사후 strm_set_media()만 호출하고 mdc_save_cache()는 호출하면 안된다.
			 */
			mdc_context = mdc_load_cache(req, &mdc_objstat, url);
			if (mdc_context != NULL) {
				media_info_t 	*temp_media = NULL;
				zipper_io_handle  *temp_ioh = NULL;
				time_t			timenow = scx_get_cached_time_sec();;
				if (mdc_objstat.st_vtime < timenow) {
					//temp_media = alloca(sizeof(media_info_t));
					temp_media = mp_alloc(req->pool,sizeof(media_info_t));
					strncpy(temp_media->st_devid, mdc_objstat.st_devid, 128);
					//temp_media->url = (char *)alloca(strlen(url) + 1);
					temp_media->url = (char *)mp_alloc(req->pool,strlen(url) + 1);
					snprintf(temp_media->url, strlen(url) + 1, "%s", url);

					temp_media->vtime = mdc_objstat.st_vtime;
					temp_media->mtime = mdc_objstat.st_mtime;
					temp_media->msize = mdc_objstat.st_size;
					/*
					 * 유효 시간이 지난 경우 원본을 다시 열어서 갱신 여부를 확인한다.
					 * 이때 media->st_devid ID가 변경된 경우는 해당 media의 available을 0로 표시하고
					 * media를 찾는 과정을 반복한다.
					 */
					/* 만료 시간이 지난 컨텐츠 들은 시간을 다시 확인 한다. */
					if (strm_check_media_info(req, type, temp_media) < 0) {
						/* disk cache에 문제가 있는 경우 zipper context해제후  캐시 파일을 지운다 */
						TRACE((T_DAEMON, "[%llu] mdc file check failed(%s).\n", req->id, url));
						temp_ioh = strm_set_io_handle(NULL);
						if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
							__thread_jmp_working = 1;
							zipper_free_media_context(temp_ioh,&mdc_context);
							__thread_jmp_working = 0;
						}
						else {
							/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
							TRACE((T_ERROR, "zipper_free_media_context() SIGSEGV occured. %s:%d\n",  __FILE__, __LINE__));
						}
						strm_destroy_io_handle(temp_ioh);
						mdc_disable_cache_file(req, url);
						goto next_create_media;
					}
					else {

						if (temp_media->vtime != mdc_objstat.st_vtime) {
							/* TTL이 갱신된 경우 */
							mdc_objstat.st_vtime = temp_media->vtime;
							/* disk cache의 TTL을 업데이트 한다. */
							must_update = 1;

						}
					}

				}

				/* 마지막에 strm_set_media()를 호출해 주어야 함 */
				strm_set_media(req, media, type, url, mdc_context, &mdc_objstat);
				if (must_update) {
					/* TTL이 갱신 된 경우 기존 캐시 파일에 TTL을 갱신한다. */
					mdc_save_cache(req, media, 1);
				}
				goto create_media_end;
			}


next_create_media:

			if( gres == GRLUR_RESETTED && (media->etime + req->service->media_negative_ttl) >= scx_get_cached_time_sec() ) {
				/*
				 * 컨텐츠에 문제가 있어서 GRLUR_RESETTED 된 상태에서  컨텐츠 에러가 발생한 시간이 현재와 비교해서 1초 이상 차이가 나지 않는 경우는 미디어를 다시 검사 하지 않고 에러 처리 한다.
				 * 문제가 있는 컨텐츠에 동시에 요청이 들어와서 서버 응답이 지연되는 현상을 회피하기 위해 이 루틴을 추가
				 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/33500
				 */
				media->available = 0;
				scx_error_log(req, "Request repeated to failed content(%s)\n", url);
				break;
			}

			source = strm_create_source_info(req, type, url, NULL);
			if (source == NULL) {
				//goto create_media_error;
				media->available = 0;
				media->etime = scx_get_cached_time_sec(); //마지막 에러 발생시간을 기록한다.
				break;
			}

			if (!source->objstat.st_sizedecled) {
				/* 이부분을 에러 처리 할지는 고민 해봐야 함 */
				TRACE((T_WARN, "[%llu] '%s' is not rangable\n", req->id, url));
				//goto create_media_error;
				media->available = 0;
				media->etime = scx_get_cached_time_sec(); //마지막 에러 발생시간을 기록한다.
				break;
			}
			/* 여기서 사용되는 param은 streaming_t을 쓰지만 파싱을 위해서 이 함수 내에서만 사용 되기 때문에
			 * memory pool 방식을 사용하지 않고 단순한 malloc만한다.
			 * 이함수를 벗어나기 전에 반드시 삭제 되어야 한다. */
			param = (streaming_t *)SCX_CALLOC(1, sizeof(streaming_t));
			memset(param, 0, sizeof(streaming_t));
			param->req = req;
			param->source = source;
			ioh = strm_set_io_handle(param);
			if (!ioh) {
				//goto create_media_error;
				media->available = 0;
				break;
			}
			ts = sx_get_time();

			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_create_media_context(ioh,&ctx);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_create_media_context() SIGSEGV occured.(%s), %s:%d\n", req->id, url, __FILE__, __LINE__));
			}
#ifdef DEBUG
			printf("zipper_create_media_context() ret(%d), url(%s)\n", ret, url);
#endif
			te = sx_get_time();
			req->t_zipper_build += (te - ts);
			if (req->file_errno) {
				/*
				 * zipper_create_media_context() 과정에서 file_errno가 셋팅 되는 경우는 인덱스를 생성하는 중에 오리진 장애가 발생한걸로 보고 직접 IO error를 설정 한다.
				 * zipper_create_media_context() 동작 중에 strm_source_reader()가 호출 되는데 strm_source_reader()에서 error를 리턴해도
				 * mp3의 경우는 media context가 에러 없이 생성되기 때문에 file_errno를 이용해서 강제로 media를 에러로 처리 한다.
				 */
				ret = zipper_err_io_handle;
			}
			if (zipper_err_success == ret) {
				//성공 했을 경우
				//리턴값을 media에 다시 넣는 이유는 다른 세션에서 동시에 같은 컨텐츠를 인덱싱 했을 경우
				//나중에 인덱싱 된 정보를 hls_add_media2list()에서 삭제하고 이전의 인덱싱 정보의 포인터를 리턴해 주기때문이다.

				strm_set_media(req, media, type, url, ctx, &source->objstat);
				ATOMIC_ADD(gscx__media_context_create_interval_cnt, 1);
				ATOMIC_ADD(gscx__media_context_create_interval_size, media->mcontext_size);
				/* 디스크 캐시에 기록, 자막은 cache에 기록하지 않는다. */
				if (type != O_CONTENT_TYPE_SUBTITLE) {
					mdc_save_cache(req, media, 0);
				}
			}
			else {
				media->available = 0;
				media->etime = scx_get_cached_time_sec(); //마지막 에러 발생시간을 기록한다.
				scx_error_log(req, "Failed to create media context.(%s), reason(%s)\n", url, zipper_err_msg(ret));
		//			if(media->contex) free_mp4tohls_context(&media->context);
				//goto create_media_error;
				break;
			}

		}

		break;
	}

create_media_end:
//	ASSERT(media->available); /*media->available가 0인 경우 여기로 들어오면 안된다. */
//	strm_use_media(media);
//create_media_error:
	if (media != NULL) {
		strm_commit_media(req, media);
//		if(gres == GLRUR_ALLOCATED)  {

//		}
		if (media->available == 0) {	/* 원본 media에 문제가 있는 경우 */
			strm_release_media(media);
			media = NULL;
			scx_error_log(req, "Find media info not available(%s)\n", url);
		}
		else {
			media->etime = 0; 	// media가 정상인 경우 이 시간을 reset 한다.
//			strm_setuse_media(media);
		}
	}

	if (ioh) {	//release_media_handle을 호출 하도록 변경
		strm_destroy_io_handle(ioh);
	}
	if (param) {
		SCX_FREE(param);
	}
	if (source) {
		strm_destroy_source_info(req, source);
	}
//	SCX_FREE(jb);
	return media;
}

int
strm_set_media(nc_request_t *req, media_info_t 	*media,  int type, char *url, zipperCnt ctx, struct nc_stat *objstat)
{
	int				len;
	/*
	 * strm_reset_media()에서 reset된 경우에는 media->url가 이미할당 되어 있다.
	 * 이 경우에는 새로 할당 하는 부분은 필요 없다.
	 */
	if(media->url == NULL) {
		len = strlen(url) + 1;
		media->url = (char *)mp_alloc(media->mpool, len);
		ASSERT(media->url);
		snprintf(media->url, len, "%s", url);
		media->type = type;
//		media->refcount = 0;
		media->account = 0;
		pthread_mutex_init(&media->lock, NULL);
	}
	media->mcontext = ctx;
	media->atime = scx_get_cached_time_sec();
	media->vtime = objstat->st_vtime;
	media->mtime = objstat->st_mtime;
	media->msize = objstat->st_size;
	media->etime = 0;
	strncpy(media->st_devid, objstat->st_devid, 128);

	media->available = TRUE;
	media->revision++;
	media->mcontext_size = zipper_context_size(media->mcontext);
	TRACE((T_DAEMON, "[%llu] Create media. url(%s), size(%d)\n", req->id, url, media->mcontext_size));
	if (0 > media->mcontext_size) media->mcontext_size = 0;
	ATOMIC_ADD(gscx__media_context_size, media->mcontext_size);
	return 1;
}

void
strm_destroy_media(media_info_t * media)
{
	zipper_io_handle  *ioh = NULL;
	mem_pool_t 		*mpool = NULL;

	ioh = strm_set_io_handle(NULL);
	pthread_mutex_destroy(&media->lock);

	if (media->mcontext) {
		if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
			__thread_jmp_working = 1;
			zipper_free_media_context(ioh,&media->mcontext);
			__thread_jmp_working = 0;
		}
		else {
			/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
			TRACE((T_ERROR, "zipper_free_media_context() SIGSEGV occured. %s:%d, url = %s\n",  __FILE__, __LINE__, media->url?media->url:"empty"));
		}
		media->mcontext = NULL;
	}
	if (media->mcontext_size) {
		ATOMIC_SUB(gscx__media_context_size, media->mcontext_size);
	}
	strm_destroy_io_handle(ioh);
	if (media->mpool) {
		mpool = media->mpool;
		media->mpool = NULL;
		mp_free(mpool);
	}
}

void
strm_reset_media(media_info_t * media)
{
	zipper_io_handle  *ioh = NULL;
	ioh = strm_set_io_handle(NULL);
	if (media->mcontext) {
		if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
			__thread_jmp_working = 1;
			zipper_free_media_context(ioh,&media->mcontext);
			__thread_jmp_working = 0;
		}
		else {
			/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
			TRACE((T_ERROR, "zipper_free_media_context() SIGSEGV occured. %s:%d\n",  __FILE__, __LINE__));
		}
	}
	if (media->mcontext_size) {
		ATOMIC_SUB(gscx__media_context_size, media->mcontext_size);
		media->mcontext_size = 0;
	}
	strm_destroy_io_handle(ioh);
	media->available = 1;
}

/*
 * X-Play-Durations 헤더를 생성하는 함수
 * media가 한개인 경우는 'X-Play-Durations : 0.000'
 * 두개 이상인 경우는 'X-Play-Durations : 0.000(0.000,0.000,0.000)'의 형식으로 만든다.
 * 자막 요청의 경우에는 X-Play-Durations 헤더를 응답하지 않는다.
 */
static void
strm_make_x_play_durations_header(streaming_t *streaming)
{
	int	len = 0;
	char rewbuf[256] = {'\0'};
	if ( streaming->builder != NULL && streaming->builder->media_list) { // 자막 요청의 경우 streaming->builder->media_list가 NULL이다
		media_info_list_t 	*media_list = streaming->builder->media_list;
		ASSERT(media_list);
		if (streaming->builder->duration == media_list->duration) {
			/* 컨텐츠가 한개만 있는 경우 */
			snprintf(rewbuf, 256, "%g", streaming->builder->duration);
		}
		else {
			/* 컨텐츠가 두개 이상인 경우 */
			snprintf(rewbuf, 256, "%g(%g", streaming->builder->duration, media_list->duration);
			media_list = media_list->next;
			while (media_list != NULL) { /* content가 한개 이상인 경우만 헤더를 만든다.*/
				if (media_list->is_base == 1) {
					len = strlen(rewbuf);
					/* adaptive track 인 경우 base track 사용한다. */
					snprintf(rewbuf+len, 256 - len, ",%g",  media_list->duration);
				}
				media_list = media_list->next;
			}
			len = strlen(rewbuf);
			snprintf(rewbuf+len, 256 - len, ")");
		}
		len = strlen(rewbuf);
		streaming->x_play_duration  = (char *) mp_alloc(streaming->mpool, len+1);
		snprintf(streaming->x_play_duration , len + 1, "%s", rewbuf);

	}
	return;
}

/*
 * client의 요청에 해당하는 builder 정보가 있는지 확인 하고 없는 경우 새로 생성한다.
 * strm_live_create_preroll_bldr_callback()에서 호출 될때에만 dur이 0이 아닌 값이 들어 있다.
 * 이 경우 설정된 hls_target_duration이 아닌 dur 값을 사용한다.
 */
int
strm_create_builder(nc_request_t *req, uint32_t dur)
{
	streaming_t	*streaming = req->streaming;
	mem_pool_t 		*mpool = NULL;
	builder_info_t 	*builder = NULL;
	media_info_t 	*media = NULL;
	media_info_list_t *cur_list = NULL, *prev_list = NULL;
	zipper_io_handle  *ioh = NULL;
	track_composition_param tcprm;
	zipperCnt adaptBase = NULL;	/* base media 기록용 */
	char			url_path[1024] = {'\0'};

	int				key_len;
	int				ret = 0;
	char			*host, *url;

	int				i;
	time_t			timenow;
	struct nc_stat 		objstat;
	int	gres = 0;
	subtitle_info_t *subtitle = NULL;


	ASSERT(streaming);
	if (streaming->content == NULL) {
		/* media가 한개도 지정되지 않은 경우 */
		scx_error_log(req, "media content not assigned(%s).\n", vs_data(req->ori_url));
		return 0;
	}


	builder = allocate_builder(NULL, req->streaming->key, 1);

	builder->is_adaptive = streaming->is_adaptive;
//		builder->media_list = media_list = mp_alloc(builder->mpool, sizeof(media_info_t));

	/*
	 * 여기에 세그먼트에 사용될 media들을 선택하는 부분이 들어간다.
	 * media가 없는 경우는 새로 만드는 부분도 필요함
	 * media들중에 만료가 된것이 있는지 확인하고 한개라도 만료된 media가 있으면
	 * builder정보를 만료 시키고 새로 만드는 과정이 필요하다.
	 */

	ioh = strm_set_io_handle(NULL); /* 이때는 strm_source_reader()를 사용 하지 않을것 같음 */
	if (!ioh) {
		goto create_builder_error;
	}
	/* HLS main manifest 요청인 경우는  zipper의  bcontext를 생성할 필요가 없고 media_list와 subtitle_list만 만들면 된다  */
	if(streaming->media_type == MEDIA_TYPE_HLS_FMP4_MAIN_M3U8 ||
			streaming->media_type == MEDIA_TYPE_HLS_MAIN_M3U8 ) {
		builder->bcontext = NULL;
		goto skip_create_bcontext;
	}

	/*
	 * 여기에 media들을 사용해서 builder handle을 만드는 부분을 들어간다.
	 */
	zipper_create_builder_param cbprm;
	if ((streaming->protocol & O_PROTOCOL_RTMP) != 0) {
		cbprm.flag = BLDRFLAG_SEGMENT_BASED_ONLY | BLDRFLAG_PERMIT_DISCONTINUTY;
		cbprm.segment.qs = 0;
		cbprm.segment.dur = 1000; // 1초 (RTMP Random Access Point를 효율적으로 제공하기 위해 1초로 고정해 놓을 것)

	}
	else {
		if ((streaming->protocol & O_PROTOCOL_HLS) != 0 &&
					1 == req->service->hls_permit_discontinuty) {
			/* HLS인 경우만 이 옵션 사용 가능 */
			cbprm.flag = BLDRFLAG_PERMIT_DISCONTINUTY;
		}
		else {
			cbprm.flag = 0;
		}
		if ((streaming->protocol & O_PROTOCOL_HLS) != 0 ||
				(streaming->protocol & O_PROTOCOL_DASH) != 0 ||
				(streaming->protocol & O_PROTOCOL_MSS) != 0) {
			/*
			 * progressive download 형태가 아닌 경우 zipper library에서 불필요하게 호출되면시 CPU를 소모하는 부분을 없애기 위해 설정
			 * https://jarvis.solbox.com/redmine/issues/32564 일감 참조
			 */
			cbprm.flag |= BLDRFLAG_SEGMENT_BASED_ONLY;
		}
		if (req->streaming->media_type == MEDIA_TYPE_HLS_VTT_M3U8 ||
						req->streaming->media_type == MEDIA_TYPE_HLS_VTT) {
			cbprm.segment.dur = req->service->subtitle_target_duration * 1000; /* HLS의 TS 생성 주기, 단위 : msec*/
		}
		else {
			/*
			 * strm_live_create_preroll_bldr_callback() 에서 호출 되는 경우(live ad stitching)는 파라미터로 넘어온 dur 값을 사용하고
			 * 이외의 경우는 설정에 지정된 hls_target_duration 값을 사용한다.
			 */
			if (dur) {

				cbprm.segment.dur = dur;
			}
			else {
				cbprm.segment.dur = req->service->hls_target_duration * 1000; /* HLS의 TS 생성 주기, 단위 : msec*/

			}
			cbprm.segment.qs = req->service->quick_start_seg;
		}
	}
	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
		__thread_jmp_working = 1;
		ret = zipper_create_builder_context(ioh, &builder->bcontext, &cbprm);
		__thread_jmp_working = 0;
	}
	else {
		/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
		ret = zipper_err_internal;
		TRACE((T_ERROR, "[%llu] zipper_create_builder_context() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
	}
	if (ret != zipper_err_success) {
		scx_error_log(req, "'%s' zipper_create_builder_context error(%s)\n", vs_data(req->ori_url), zipper_err_msg(ret));
		goto create_builder_error;
	}

skip_create_bcontext:

	if (req->streaming->media_type == MEDIA_TYPE_HLS_VTT_M3U8 ||
				req->streaming->media_type == MEDIA_TYPE_HLS_VTT) {
		subtitle = streaming->subtitle;
		while (subtitle) {
			// 자막 요청인 경우는 요청된 자막 스트림에 대해서만 media context를 만든다.
			if(!(streaming->rep_num == subtitle->subtitle_order)) {
				subtitle = subtitle->next;
				continue;
			}
			/* media가 있는지 검색하고 없는 경우 새로 만든다. */
			media = strm_create_media(req, O_CONTENT_TYPE_SUBTITLE, subtitle->path);
			if (!media) {
				goto create_builder_error;
			}
			cur_list = mp_alloc(builder->mpool, sizeof(media_info_list_t));
			ASSERT(cur_list);
			if (builder->subtitle_list == NULL) {
				builder->subtitle_list = cur_list;
			}
			else {
				prev_list->next = cur_list;
			}
			cur_list->media = media;
			if (builder->bcontext) {	// HLS main manifest 요청 아닌 경우만 실행
				memset(&tcprm, 0, sizeof(track_composition_param));
				tcprm.comp.range.start = subtitle->start;
				tcprm.comp.range.end = subtitle->end;
				tcprm.media = media->mcontext;
				tcprm.smux.qtype = TRACK_SMUX_QUERY_LANG;
#ifdef DEBUG
				printf("base track url = %s, type(%d), start(%d), end(%d), context(%lld)\n", media->url, subtitle->subtitle_type, subtitle->start, subtitle->end, media->mcontext);
#endif
				if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
					__thread_jmp_working = 1;
					ret = zipper_add_track(ioh, builder->bcontext, &tcprm);
					__thread_jmp_working = 0;
				}
				else {
					/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
					ret = zipper_err_internal;
					TRACE((T_ERROR, "[%llu] zipper_add_track() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
				}

				if (ret != zipper_err_success) {
					scx_error_log(req, "'%s' zipper_add_track error.(%s)\n", media->url, zipper_err_msg(ret));
					goto create_builder_error;
				}
			}
			prev_list = cur_list;
			subtitle = subtitle->next;
		}
		zipper_end_track(ioh, builder->bcontext);
		goto create_builder_end;
	}

	content_info_t *content = streaming->content;
	/* 트랙 추가과정 */
	while (content) {
		if (streaming->is_adaptive == 1 && streaming->rep_id != NULL) {
			if (streaming->adaptive_info->next == NULL) {
				/* adaptive streaming 요청시 전체 track을 사용하지 않고 요청된 트랙만 추가해서 metadata parsing으로 인한 부하를 줄인다. */
				if ((streaming->protocol & O_PROTOCOL_HLS) != 0 ) {
					/*
					 * adaptive streaming이고 submanifest 요청이나 TS, m4s, m4a 인(streaming->rep_id가 지정된) 경우에는
					 * rep_id(bitrate)에 해당하는 컨텐츠의 metadata만 사용하도록 한다.
					 * strm_create_builder()에서 아래의 조건을 만족 하는 경우에 요청된 해상도에 해당 하는 파일로만 builder context를 생성
					 *    adaptive(streaming->is_adaptive가 1인 경우)이고 컨텐츠 한개만 사용(streaming->adaptive_info->next가 NULL일때)하는 경우
					 *    submanifest 요청이나 TS, m4s, m4a 요청인 경우 (streaming->rep_id가 지정된 경우)
					 * 	  content->bitrate에 값이 있는 경우만 비교, 없는 경우는 기존과 동일하게 builder context에 포함
					 * 이때 skip되는 컨텐츠가 아닌 경우 content->is_base를 1로 설정해야 한다.
					 */

					/* fregmented mp4 방식 HLS일때 요청된 track만 추가한다 */
					if (content->order == streaming->rep_num){
						content->is_base = 1;
					}
					else if (strcmp(content->bitrate, streaming->rep_id) == 0) {	//TS 방식 HLS인 경우
						content->is_base = 1;
					}
					else {
						content = content->next;
						continue;
					}
				}
				else if((streaming->protocol & O_PROTOCOL_DASH) != 0) {
					if (streaming->media_type == MEDIA_TYPE_MPEG_DASH_VIDEO_INIT ||
							streaming->media_type == MEDIA_TYPE_MPEG_DASH_VIDEO ||
							streaming->media_type == MEDIA_TYPE_MPEG_DASH_AUDIO_INIT ||
							streaming->media_type == MEDIA_TYPE_MPEG_DASH_AUDIO ) {
						/* fregmented mp4 요청인 경우 요청된 track만 추가 한다. */
						if (content->order == streaming->rep_num){
							content->is_base = 1;
						}
						else {
							content = content->next;
							continue;
						}
					}
				}
			}
		}

		/* media가 있는지 검색하고 없는 경우 새로 만든다. */
		media = strm_create_media(req, content->type, content->path);
		if (!media) {
			if (content->type == O_CONTENT_TYPE_ADVER && req->service->ad_allow_media_fault == 1) {
				/* 광고 컨텐츠이고 ad_allow_media_fault가 1로 허용 설정이 된 경우 해당 미디어에 문제가 있으면 skip 한다. */
				TRACE((T_INFO, "[%llu] AD Content Skipped(%s). fault content path(%s)\n", req->id, vs_data(req->ori_url), content->path));
				content = content->next;
				req->p1_error = 0; /* strm_create_media()에서 설정된 에러 코드를 초기화한다. */
				continue;
			}
			goto create_builder_error;
		}

		if (builder->vtime == 0 || builder->vtime > media->vtime) {
			builder->vtime = media->vtime; /* 사용된 컨텐츠중 만료 시간이 가장짧은 동영상의 mtime을 상속 받는다. */
		}
		builder->mtime = media->mtime; /* 동영상의 마지막 원본 컨텐츠의 시간을 넣어야 하지만 검사하기 귀찮아서 -_- */

		/* gres 가 GRLUR_RESETTED인 경우에도 이전에 builder 생성에 실패 한 경우는
		 * builder->media_list가 NULL이거나 일부만 들어 있는 경우가 있어서
		 * 메모리 낭비가 생기더라도 재 할당 한다.
		 */
		cur_list = mp_alloc(builder->mpool, sizeof(media_info_list_t));
		ASSERT(cur_list);
		if (builder->media_list == NULL) {
			builder->media_list = cur_list;
			/* media가 여러개인 경우 첫 미디어의 정보들이 들어간다. */
			if (0 < media->mcontext->desc.bandwidth)
				builder->bandwidth = (int)media->mcontext->desc.bandwidth;
			if (0 < media->mcontext->desc.video[0].resolution.width)
				builder->width = (int)media->mcontext->desc.video[0].resolution.width;
			if (0 < media->mcontext->desc.video[0].resolution.height)
				builder->height = (int)media->mcontext->desc.video[0].resolution.height;
		}
		else {
			prev_list->next = cur_list;
		}
		content->media = media;
		cur_list->media = media;
		cur_list->revision = media->revision;
		if (builder->is_adaptive == 1) {
			cur_list->tcprm.desc = content->bitrate;
		}
		else {
			cur_list->tcprm.desc = NULL;
		}
		cur_list->tcprm.media = media->mcontext;
		cur_list->tcprm.smux.qtype = TRACK_SMUX_QUERY_LANG;
		if (req->streaming->lang) strncpy(cur_list->tcprm.smux.qdata.lang, req->streaming->lang, 3);
		if (builder->bcontext) {	// HLS main manifest 요청 아닌 경우만 실행
			if (builder->is_adaptive == 1 && content->is_base == 0) {
				/* adaptive track일 경우 sub media는 zipper_add_adaptive_track()을 사용한다. */

				cur_list->tcprm.comp.adaptBase = adaptBase;
				cur_list->is_base = 0;
	//				printf("sub track url = %s, start(%d), end(%d), context(%lld)\n", media->url, content->start, content->end, cur_list->tcprm.comp.adaptBase);
#ifdef DEBUG
				printf("sub track url = %s, type(%d), start(%d), end(%d), context(%lld)\n", media->url, content->type, content->start, content->end, cur_list->tcprm.comp.adaptBase);
#endif
				if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
					__thread_jmp_working = 1;
					ret = zipper_add_adaptive_track(ioh, builder->bcontext, &cur_list->tcprm);
					__thread_jmp_working = 0;
				}
				else {
					/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
					ret = zipper_err_internal;
					TRACE((T_ERROR, "[%llu] zipper_add_adaptive_track() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
				}
			}
			else {
				/* adaptive track인 경우 base media의 정보를 저장한다. */
				adaptBase = media->mcontext;	/* base media의 임시로 정보를 저장. adaptive track이 아닌 경우는 이부분은 의미 없음 */
				cur_list->tcprm.comp.range.start = content->start;
				cur_list->tcprm.comp.range.end = content->end;
				cur_list->is_base = 1;
#ifdef DEBUG
				printf("base track url = %s, type(%d), start(%d), end(%d), context(%lld)\n", media->url, content->type, content->start, content->end, media->mcontext);
#endif
				if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
					__thread_jmp_working = 1;
					ret = zipper_add_track(ioh, builder->bcontext, &cur_list->tcprm);
					__thread_jmp_working = 0;
				}
				else {
					/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
					ret = zipper_err_internal;
					TRACE((T_ERROR, "[%llu] zipper_add_track() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
				}
				/*
				 * content의 시간을 쓰지 않고 cur_list->tcprm.comp.range의 시간을 쓰는 이유는
				 * 시간 지정이 맞지 않는(end가 실제 동영상보다 크게 지정된) 경우 zipper_add_track()호출 후
				 * 라이브러리상에서 보정이 이루어 지기 때문이다.

				 */
				if(cur_list->tcprm.comp.range.end == EOF) {
					/* range.end가 EOF일 경우에는 실제 파일 크기 기준으로 계산한다. */
					cur_list->duration = media->mcontext->desc.duration - (float)cur_list->tcprm.comp.range.start/1000.0;
				}
				else {
					cur_list->duration = (float)(cur_list->tcprm.comp.range.end - cur_list->tcprm.comp.range.start)/1000.0;
				}
				builder->duration += cur_list->duration;
			}

			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_add_track error.(%s)\n", media->url, zipper_err_msg(ret));
				goto create_builder_error;
			}
		}	// end of if (skip_bcontext == 0)
		content = content->next;
		prev_list = cur_list;
	}
	if (builder->media_list == NULL) {
		/* 정상 생성된 컨텐츠가 한개도 없는 경우 에러 처리 한다. */
		scx_error_log(req, "'%s' empty builder->media_list\n", vs_data(req->ori_url));
		goto create_builder_error;
	}
	if (builder->bcontext) {
		zipper_end_track(ioh, builder->bcontext);
	}
	if(streaming->media_type == MEDIA_TYPE_HLS_FMP4_MAIN_M3U8 ||
				streaming->media_type == MEDIA_TYPE_HLS_MAIN_M3U8 ) {
		/* menifest 파일 생성시 들어갈 bitrate 관련 태그정보를 따로 저장한다.
		 * 처음 한번만 리스트에 적용하면 이후에 다시 넣을 필요는 없다. */
		content = streaming->content;
		for (i = 0; i < 20 ; i++) {
#if 0
			builder->adaptive_track_name[i] =  mp_alloc(builder->mpool, strlen(content->bitrate)+1);
			sprintf(builder->adaptive_track_name[i], "%s", content->bitrate);
			if (content->codecs != NULL ) {
				builder->codecs[i] =  mp_alloc(builder->mpool, strlen(content->codecs)+1);
				sprintf(builder->codecs[i], "%s", content->codecs);
			}
			if (content->resolution != NULL ) {
				builder->resolution[i] =  mp_alloc(builder->mpool, strlen(content->resolution)+1);
				sprintf(builder->resolution[i], "%s", content->resolution);
			}
#else
			builder->track_info[i].adaptive_track_name =  mp_alloc(builder->mpool, strlen(content->bitrate)+1);
			sprintf(builder->track_info[i].adaptive_track_name, "%s", content->bitrate);
			if (content->codecs != NULL ) {
				builder->track_info[i].codecs =  mp_alloc(builder->mpool, strlen(content->codecs)+1);
				sprintf(builder->track_info[i].codecs, "%s", content->codecs);
			}

			if (content->resolution != NULL ) {
				builder->track_info[i].resolution =  mp_alloc(builder->mpool, strlen(content->resolution)+1);
				sprintf(builder->track_info[i].resolution, "%s", content->resolution);
			}
			builder->track_info[i].content = content;
#endif
			content = content->next;
			/* track 한개일 때는 content가 NULL인 조건에 걸리고 여러개의 track인 경우는 content->is_base == 1 조건에 걸린다. */
			if(NULL == content || content->is_base == 1) {
				/* 처음의 한 track(첫 base부터 다음 이전 base까지) 만 저장한다. */
				builder->adaptive_track_cnt = i+1;
				break;
			}
		}
	}


create_builder_end:
	if (ioh) {
		strm_destroy_io_handle(ioh);
	}
	streaming->builder = builder;
	if (builder->bcontext) {
		builder->bcontext_size = zipper_context_size(builder->bcontext);
		if (0 > builder->bcontext_size) builder->bcontext_size = 0;
	}
	strm_make_x_play_durations_header(streaming);
	return 1;
create_builder_error:

	if (builder->bcontext) {
		if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
			__thread_jmp_working = 1;
			zipper_free_builder_context(ioh,&builder->bcontext);
			__thread_jmp_working = 0;
		}
		else {
			/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
			ret = zipper_err_internal;
			TRACE((T_ERROR, "[%llu] zipper_free_builder_context() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
		}
		builder->bcontext = NULL;
	}


	if (ioh) {
		strm_destroy_io_handle(ioh);
	}
	/* pseudo streaming */
	cur_list = builder->media_list;
	while (cur_list) {
		//if (media == NULL) break;
		media = cur_list->media;
		if (media) {
			strm_release_media(media);
			cur_list->media = NULL;
		}
		cur_list = cur_list->next;
	}
	builder->media_list = NULL;

	cur_list = builder->subtitle_list;
	while (cur_list) {
		media = cur_list->media;
		if (media) {
			strm_destroy_media(media);
			cur_list->media = NULL;
		}
		cur_list = cur_list->next;
	}
	builder->subtitle_list = NULL;

	if (builder->mpool) mp_free(builder->mpool);

	builder = NULL;
	//if (mpool) mp_free(mpool);
//	if (streaming) SCX_FREE(streaming);
	/* 이부분 보강 해야함 */
	return 0;
}

/*
* DASH의 경우 XML에서 &를 사용할수 없기 때문에 streaming->argument에 &가 들어 있는 경우
* 새로운 메모리를 할당해서 &대신 url encoding 한 %26을 대신 붙인다.
* https://jarvis.solbox.com/redmine/issues/33091 이슈로 %26대신 &amp;로 변경
 */
static int
strm_url_encode_argument(nc_request_t *req)
{
	int ret;
	streaming_t	*streaming = req->streaming;
	char *new_argument = NULL;
	int	alloc_size = 0;
	int	arg_len = 0;
	int i = 0;
	int	ampersand_cnt = 0;
	unsigned char    *pos_prev;
	unsigned char    *pos_new;
	if (streaming->argument == NULL) return 1;

	arg_len = strlen(streaming->argument);
	pos_prev = streaming->argument;
	for (i = 0; i < arg_len; i++) {
		if (*pos_prev == '&') ampersand_cnt++;
		pos_prev++;
	}
	if (ampersand_cnt == 0) {
		/* &가 한개도 없는 경우는 기존과 동일하게 동작하기 때문에 여기서 그냥 리턴한다. */
		return 1;
	}
#if 0
	alloc_size = arg_len + ampersand_cnt * 3 + 1;
	new_argument = (char *) mp_alloc(streaming->mpool, alloc_size);
	pos_new = new_argument;
	pos_prev = streaming->argument;
	for (i = 0; i < arg_len; i++) {
		if (*pos_prev == '&')  {
			*pos_new++ = '%';
			*pos_new++ = '2';
			*pos_new++ = '6';
		}
		else {
			*pos_new++ = *pos_prev;
		}
		pos_prev++;

	}
#else
	/* DASH의 경우 XML에서 &를 사용할수 없기 때문에 '&'대신 '&amp;'를 붙인다. */
	alloc_size = arg_len + ampersand_cnt * 5 + 1;
	new_argument = (char *) mp_alloc(streaming->mpool, alloc_size);
	pos_new = new_argument;
	pos_prev = streaming->argument;
	for (i = 0; i < arg_len; i++) {
		if (*pos_prev == '&')  {
			*pos_new++ = '&';
			*pos_new++ = 'a';
			*pos_new++ = 'm';
			*pos_new++ = 'p';
			*pos_new++ = ';';
		}
		else {
			*pos_new++ = *pos_prev;
		}
		pos_prev++;

	}
#endif
	*pos_new = '\0';
	streaming->argument = new_argument;
	return 1;
}

/*
 * 요청 URL에 들어 있는 argument에 session ID의 유무에 따라 새로운 argument를 만들어준다.
 *
 */
static int
strm_update_session_argument(nc_request_t *req)
{
	int ret;
	streaming_t	*streaming = req->streaming;
	char *argument = NULL;
	int	alloc_size = 0;
	ASSERT(req->streaming);
	if ((streaming->protocol & O_PROTOCOL_DASH) != 0 && streaming->argument != NULL)  {
		/* streaming->argument에 들어 있는 &를 모두 %26로 변경 */
		strm_url_encode_argument(req);
	}
	if (req->service->session_enable == 1 && streaming->session) {
		if (streaming->session_id) {
			/* request url의 argument에 session_id가 포함된 경우  argument 수정이 필요 없다.*/

		}
		else {
			alloc_size = vs_length(req->service->session_id_name)+25;
			/* 새로운 session id가 생성된 경우*/
			if (streaming->argument != NULL) {
				/* argument 가 있는 경우는 session_id를 포함한 새로운 argument 를 할당 한다.*/
				alloc_size += strlen(streaming->argument);
				argument = (char *) mp_alloc(streaming->mpool, alloc_size);
				/* argument(get) parameter가 있는 경우 session id 뒤에 붙여 준다.*/
				if ((streaming->protocol & O_PROTOCOL_DASH) != 0)  {

#if 0
					/* DASH의 경우 XML에서 &를 사용할수 없기 때문에 url encoding 한 %26을 대신 붙인다. */
					snprintf(argument, alloc_size, "%s%%26%s=%d",
							streaming->argument, vs_data(req->service->session_id_name), streaming->session->id);
#else
					/* DASH의 경우 XML에서 &를 사용할수 없기 때문에 '&'대신 '&amp;'를 붙인다. */
					snprintf(argument, alloc_size, "%s&amp;%s=%d",
							streaming->argument, vs_data(req->service->session_id_name), streaming->session->id);
#endif
				}
				else {
					snprintf(argument, alloc_size, "%s&%s=%d",
							streaming->argument, vs_data(req->service->session_id_name), streaming->session->id);
				}
				streaming->argument = argument;
			}
			else {

				/* request에 argument가 없는 경우는 새로운 argument에 session_id만 추가한다. */
				streaming->argument = (char *) mp_alloc(streaming->mpool, alloc_size);
				snprintf(streaming->argument, alloc_size,  "?%s=%d", vs_data(req->service->session_id_name), streaming->session->id);
			//	printf("argument = %s\n", streaming->argument);
			}

		}
	}
	return 1;
}

//////////////////////////////////////////////////////////////// Live Ad Stitching 기능 Start //////////////////////////////////////////////////////////////
// https://jarvis.solbox.com/redmine/issues/32389 일감 참조
/*
 * zlive_create() 실행시 callback으로 호출되는 함수
 * preroll 광고 builder context 생성후 광고 시작 시점에서 callback 되는 function
 * 성공시 빌더 컨텍스트(zipperBldr)를 리턴하고, 실패시 NULL을 리턴해야함.
 * NULL을 리턴하는 경우는 광고 없이 본 live stream만 나간다.
 * callback 호출은 context 생성시(main manifest 응답시)가 아닌 광고 정보를 처음 내보내주는 sub manifest 응답시에 호출된다.
 */
zipperBldr
strm_live_create_preroll_bldr_callback(uint32_t dur, void *param)
{
	live_info_t *l_info = (live_info_t *)param;
	nc_request_t *req = (nc_request_t *)l_info->req;
	zipperBldr bcontext = NULL;
	session_info_t *session = (session_info_t *)req->streaming->session;
	ASSERT(session);	/* session 기능을 사용하지 않는 상태에서 이곳에 들어 오는 경우는 없어야 한다. */
	/* 여기서 builder context를 생성해야 한다.*/
//	printf("%s() duration = %d\n", __func__, dur);
	if (strm_create_builder(req, dur) == 1) {
		bcontext = req->streaming->builder->bcontext;
//		req->streaming->builder->bcontext = NULL;  /* 연결 종료시 해당 zipperBldr context가 free되지 않도록 하기 위함 */
		session->l_info.builder = (void *) req->streaming->builder;
		req->streaming->builder = NULL;  /* 연결 종료시 해당 builder context가 free되지 않도록 하기 위함 */

		return bcontext;
	}

	return NULL;
}
/*
 * zipper 라이브러리에서 live 기능 사용시
 * preroll 광고가 모두 publishing되고 매니패스트 상에서 더이상 기술되지 않는 상태가 오면 바로 호출됨(혹은 zlive_free()호출시)
 */
void
strm_live_expire_preroll_bldr_callback(zipperBldr bldr, void *param)
{
	live_info_t *l_info = (live_info_t *)param;
	nc_request_t *req = (nc_request_t *)l_info->req;
	zipper_io_handle  *ioh = NULL;
	session_info_t *session = (session_info_t *)req->streaming->session;
	builder_info_t 	*builder = NULL ;
//	printf("%s()\n", __func__);
	builder = (builder_info_t *)session->l_info.builder;
	if (builder != NULL) {
		strm_destroy_builder(builder);
		session->l_info.builder = NULL;
	}

	/*
	 * 중요 : 아래의 부분들은 zlive_create()시 옵션으로 zlive_discon(EXT-X-DISCONTINUITY 사용)를 사용하는 경우에만 실행 되어야 하고
	 * zlive_no_discon나 zlive_adaptive_discon를 사용하는 경우는 skip 되어야 한다.
	 */
	/* 광고가 끝난 시점이기 때문에 zlive context도 필요 없다 */
	if (session->l_info.z_ctx != NULL) {
		/* live session 기능을 사용중인 경우 live session 먼저 삭제 한다. */
		strm_live_session_destory((void *)session);
	}
	/* 광고가 종료 되었음을 명시적으로 설정 */
	session->l_info.ad_finished  = 1;
}


/*
 * 이시점에 session은 생성된 상태임
 * req->streaming->session->z_ctx이 NULL이 인 경우 zlive_create()를 호출
 * 필요한 경우 session->z_ctx대신 z_ctxf를 포함한 다른 구조체 사용이 필요할 수도 있음
 */
int
strm_live_session_check(nc_request_t *req)
{
	streaming_t	*streaming = req->streaming;
	int	ret;
	ASSERT(req->streaming->session);	/* 라이브 기능을 사용하는 경우는 여기까지 왔을 때에는 무조건 session이 생성 되어 있어야 한다. */
	session_info_t *session = (session_info_t *)streaming->session;
	TRACE((T_DAEMON, "[%llu] %s(), session = %d\n", req->id,  __func__, req->streaming->session->id));
	session->l_info.req = (void *)req; /* req는 접속시 마디 바뀌기 때문에 session 확인시 마다 바꿔줘야 한다. */
	if (streaming->content == NULL) {
		/* 광고가 한개 없는 경우는 zlive context를 생성하지 않는다. */
		session->l_info.ad_finished = 1;
	}
	else if (session->l_info.z_ctx == NULL) {
		/* 라이브 세션이 처음 생성된 경우 */
		zipper_io_handle ioh;
		memset(&ioh, 0, sizeof(ioh));
		strm_set_io_memfunc(&ioh);

        // zlive 컨텍스트 생성 ////////////////////////////////////////////////////
        zlive_create_param zlcp;

        zlcp.type = zlive_type_hls; // HLS 스트림 연동
        zlcp.discon = zlive_discon; //zlive_no_discon, zlive_adaptive_discon, zlive_discon 설정 가능, 확실한 동작을 위해서 항상 EXT-X-DISCONTINUITY를 사용하도록 한다.
        zlcp.disall = req->service->hls_discontinuty_all; /* manifest의 목록에 있는 TS를 합한 시간보다 광고가 적을 경우 TS마다 EXT-X-DISCONTINUITY를 붙인다. */
        zlcp.fpadding = 0; // MPEG2-TS의 패킷 카운터에 대해 광고 세그먼트와의 연속성 처리를 위한 Chunk 패딩 처리 여부(HLS 전용)
        zlcp.preroll.create = strm_live_create_preroll_bldr_callback;
        zlcp.preroll.expire = strm_live_expire_preroll_bldr_callback;
        zlcp.preroll.param = (void *)&session->l_info;
        if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
        	__thread_jmp_working = 1;
        	ret = zlive_create(&ioh, &zlcp, &session->l_info.z_ctx);
        	__thread_jmp_working = 0;
        }
        else {
        	/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
        	ret = zipper_err_internal;
        	TRACE((T_ERROR, "[%llu] zlive_create() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
        }

        if(ret != zipper_err_success) {
			scx_error_log(req, "zlive_create error. reason : %s\n",  zipper_err_msg(ret));
            return 0;
        }
	}

	return 1;
}

/*
 *sessionmgr.c의 sm_destroy_session()에서 호출된다.
 */
int
strm_live_session_destory(void *session_info)
{
	session_info_t *session = (session_info_t *)session_info;

	/* 여기에서 zlive_free()를 호출해야 한다. */
	zipper_io_handle ioh;

	memset(&ioh, 0, sizeof(ioh));
	strm_set_io_memfunc(&ioh);
	// zlive 컨텍스트 해제
	zlive_free(&ioh, &session->l_info.z_ctx);
	/* zlive_free()에서 session->l_info.z_ctx를 NULL로 설정 하는 부분이 있기 때문에 따로 설정하지 않는다. */
//	printf("%s()\n", __func__);
}


/* live ad stitching 사용시 origin으로 부터 파일을 읽을때 사용하는 context */
typedef struct strm_live_io_ctx_tag
{
	nc_request_t *req;
	void * file;
	struct nc_stat 		objstat;
} strm_live_io_ctx_t;

static ssize_t
strm_live_read_func(void *context, void *buf, size_t size, off_t offset, void *param)
{
	strm_live_io_ctx_t *io_ctx = (strm_live_io_ctx_t *)param;
	ssize_t copied = 0;
	nc_request_t *req = io_ctx->req;

	int	using_j_enable = 0;

	if (__thread_jmp_working == 1) {
		/* zipper library를 벗어나서 SIGSEGV 발생하는 경우는 죽어야 하므로 gscx__j_enable을 0으로 바꾼다. */
		using_j_enable = 1;
		__thread_jmp_working = 0;
	}

	ASSERT(io_ctx->file);

//	printf("%s() size = %d, offset = %d\n", __func__, size, offset);

	copied = strm_file_read(req, io_ctx->file, buf, offset, size);
	if(using_j_enable == 1) {
		__thread_jmp_working = 1;
	}
    return copied;
}

static off_t
strm_live_size_func(void *context, void *param)
{
	strm_live_io_ctx_t *io_ctx = (strm_live_io_ctx_t *)param;
	nc_request_t *req = io_ctx->req;
	ASSERT(io_ctx->file);
//	printf("%s() = %lld\n", __func__, io_ctx->objstat.st_size);


    return io_ctx->objstat.st_size;
}

/* 필요 없어 보이는 함수 */
static
int strm_live_write_func(unsigned char *block, size_t size, void *param)
{
	strm_live_io_ctx_t *io_ctx = (strm_live_io_ctx_t *)param;
	nc_request_t *req = io_ctx->req;
//	printf("%s()\n", __func__);
//    zld_roll_io *io = (zld_roll_io *)param;

//    memcpy(&io->buf[io->wo], block, size);
//    io->wo += size;

    return zipper_err_success;
}

/* strm_live_edit_hls_manifest_session() 호출시 사용됨 */
typedef struct _m3u8_format_template_stat {

    const char *fmt;
    size_t fmtlen;
    char *inbuf;
    size_t inbufsize;
    char *tmpbuf;	// strm_parse_swap_url_callback에서 사용하는 임시 작업용 buffer
	size_t tmpbufsize;
    char *outbuf;	// 재생성(출력) 버퍼의 포인터
    size_t outbufsize; // 재생성(출력) 버퍼의 할당 크기
    size_t outbuflen; // 재생성된 매니패스트의 크기
	void * arg;

} m3u8_format_template_stat;

/*
 * zlive_m3u8_parse()에서 callback으로 호출된다.
 * 태그의 URL 속성을 교체
 * EXTINF, EXT-X-STREAM-INF 태그이거나 혹은 태그에 URI 속성이 있을 경우 호출됨
 */
static int
strm_parse_swap_url_callback(zipper_io_handle* ih, zlive_m3u8_tag *tag, zurl *srcUrl, zstr *swapUrl, void *param)
{
	/*
	 * url을 수정하기 위해서는 param에 url을 수정해서 swapUrl에 넘겨 줄수있는 작업용 memory가 필요하다.
	 * 작업용 memory는 미리 할당되어 있어야 함.
	 */
	m3u8_format_template_stat *stat = (m3u8_format_template_stat *)param;

	nc_request_t 	*req = (nc_request_t *)stat->arg;
	streaming_t 	*streaming = req->streaming;
	int	pos = 0;

	/* srcUrl->path 가 상대 경로인 경우는 항상 포함 해야 한다. */
	/* 중요:  URL이 교체되지 않는 경우에는 swapUrl.len을 0으로 설정해 주어야 무시됨 */
    swapUrl->len = 0;
//	printf("%s called len = %d, url(%s)\n", __func__, srcUrl->file.len, srcUrl->file.val);
#if 0
	if (req->service->hls_media_playlist_use_absolute_path || req->service->hls_master_playlist_use_absolute_path) {
		/* 정적 URL 사용시 */
		if (req->service->hls_playlist_host_domain) {
			snprintf(stat->tmpbuf, stat->tmpbufsize,
					"%s://%s",
					req->hls_playlist_protocol?"https":"http", vs_data(req->service->hls_playlist_host_domain));
			pos = strlen(stat->tmpbuf);
		}
		if (req->service->manifest_prefix_path) {
			snprintf(stat->tmpbuf+pos, stat->tmpbufsize-pos,
					"/%s", vs_data(req->service->manifest_prefix_path));
			pos = strlen(stat->tmpbuf);
		}
		if (req->app_inst) {
			snprintf(stat->tmpbuf+pos, stat->tmpbufsize-pos,
					"/%s", req->app_inst);
			pos = strlen(stat->tmpbuf);
		}
		snprintf(stat->tmpbuf+pos, streaming->real_path_len+1, "%s/", vs_data(req->ori_url));
		pos = strlen(stat->tmpbuf);

	}
//	printf("tempbuf = '%s'\npos = %d\n", stat->tmpbuf, pos);
	snprintf(stat->tmpbuf+pos, srcUrl->file.len+1, "%s", srcUrl->file.val);
	pos = strlen(stat->tmpbuf);

	/* argument 추가 */
	if (streaming->argument) {
		snprintf(stat->tmpbuf+pos, stat->tmpbufsize - pos, "%s", streaming->argument);
	}
	pos = strlen(stat->tmpbuf);
#else
	if (req->service->hls_media_playlist_use_absolute_path || req->service->hls_master_playlist_use_absolute_path) {
		/* 정적 URL 사용시 */
		if (req->service->hls_playlist_host_domain) {
			memcpy(stat->tmpbuf+pos, "http", 4);
			pos += 4;
			if(req->hls_playlist_protocol == 1) {
				*(stat->tmpbuf+pos++) = 's';
			}
			*(stat->tmpbuf+pos++) = ':';
			*(stat->tmpbuf+pos++) = '/';
			*(stat->tmpbuf+pos++) = '/';
			memcpy(stat->tmpbuf+pos, vs_data(req->service->hls_playlist_host_domain), vs_length(req->service->hls_playlist_host_domain));
			pos += vs_length(req->service->hls_playlist_host_domain);

		}

		if (req->service->manifest_prefix_path) {
			if( *(vs_data(req->service->manifest_prefix_path)) != '/' ) {
				*(stat->tmpbuf+pos++) = '/';
			}
			memcpy(stat->tmpbuf+pos, vs_data(req->service->manifest_prefix_path), vs_length(req->service->manifest_prefix_path));
			pos += vs_length(req->service->manifest_prefix_path);
		}

		if (req->app_inst) {
			if( *req->app_inst != '/' ) {
				*(stat->tmpbuf+pos++) = '/';
			}
			memcpy(stat->tmpbuf+pos, req->app_inst, strlen(req->app_inst));
			pos += strlen(req->app_inst);
		}

		memcpy(stat->tmpbuf+pos, vs_data(req->ori_url), streaming->real_path_len);
		pos += streaming->real_path_len;
		if( *(stat->tmpbuf+pos-1) != '/' ) {
			*(stat->tmpbuf+pos++) = '/';
		}
	}
	//printf("tempbuf = '%s'\npos = %d\n", stat->tmpbuf, pos);
	memcpy(stat->tmpbuf+pos, srcUrl->file.val, srcUrl->file.len);
	pos += srcUrl->file.len;

	/* argument 추가 */
	if (streaming->argument) {
		memcpy(stat->tmpbuf+pos, streaming->argument, strlen(streaming->argument));
		pos += strlen(streaming->argument);
	}
	*(stat->tmpbuf+pos) = '\0';	//혹시 몰라서 마지막에 null을 넣는다.

#endif
	swapUrl->val = stat->tmpbuf;
	swapUrl->len = pos;
//	printf("%s called\ntempbuf = '%s'\npos = %d\n", __func__, stat->tmpbuf, pos);

    return zipper_err_success;
}

/*
 * zlive_m3u8_parse()에서 callback으로 호출된다.
 * 태그를 조회하고 수정 및 동작 흐름을 제어
 * 0(zlive_tag_continue)을 리턴하면 출력 및 다음 태그를 진행
 * 1(zlive_tag_skip)을 리턴하면 해당 태그는 출력에서 제외
 * 2를 리턴하면 전체 파싱 루프를 종료
 */
static int
strm_parse_tag_callback(zlive_m3u8_tag *tag, void *param)
{

#if 0
	if (tag->tag == zlM3u8TagDiscon) {
		return zlive_tag_skip;
	}
#endif
    return zlive_tag_continue;
}

/*
 * 오리진에서 받은 manifest롤 기준으로 출력 manifest buffer 크기를 계산
 * manifest 기본 응답
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:3
#EXT-X-MEDIA-SEQUENCE:5405481
#EXTINF:2.002,
media_w67665283_DVR_5405481.ts
#EXTINF:2.002,
media_w67665283_DVR_5405482.ts
#EXTINF:2.002,
media_w67665283_DVR_5405483.ts
#EXTINF:2.002,
media_w67665283_DVR_5405484.ts
 */
static int
strm_calc_manifest_outbuf_size(nc_request_t *req, char *inbuf)
{
	streaming_t 	*streaming = req->streaming;
	int url_len = 0;
	int i = 0;
	int line_count = 0;
	char *bufpos = inbuf;
	int cal_buf_size = 0;


	/* 한개 line에 필요한 memory를 계산 한다. */
	if (req->service->hls_media_playlist_use_absolute_path || req->service->hls_master_playlist_use_absolute_path) {
		/* 정적 URL 사용시 */
		if (req->service->hls_playlist_host_domain) {
			url_len += 10 + vs_length(req->service->hls_playlist_host_domain);	//'https://' 포함
		}
		if (req->service->manifest_prefix_path) {
			url_len += vs_length(req->service->manifest_prefix_path);

		}
		if (req->app_inst) {
			url_len += strlen(req->app_inst);
		}
	}
	url_len += vs_length(req->ori_url);
	if (streaming->argument) {	/* 추가될 argument 길이 */
		url_len += strlen(streaming->argument);
	}
	url_len += 100; /* 100 byte의 여유분을 추가 */

	/* 오리진으로 부터 받은 manifest 파일에 있는 line 수를 count 한다 */
	while ('\0' != *bufpos) {
		if ('\n' == *bufpos) {
			line_count++;
		}
		bufpos++;
	}
//	printf("line count = %d, url len = %d\n", line_count, url_len);
	/*
	 * manifest에서 처음 4개의 기본 정보를 가지고 있는 line을 제외하고는
	 * 시간 정보를 가지고 있는 EXTINF와 path가 반복 되기 때문에 path의 갯수만 계산에 반영한다.
	 */
	cal_buf_size = (line_count / 2) * url_len;

	return cal_buf_size;
}
/* 성공한 경우 zipper_err_success가 리턴된다. */
static int
strm_live_edit_hls_manifest_session(nc_request_t *req, zipper_io_handle *ioh, m3u8_format_template_stat *p_ftstat)
{
	int ret;
	zlive_m3u8_parse_param pprm;
	streaming_t	*streaming = req->streaming;
	int cal_buf_size = 0;

	cal_buf_size = strm_calc_manifest_outbuf_size(req, p_ftstat->inbuf);

	p_ftstat->inbufsize = strlen(p_ftstat->inbuf);	/* inbufsize에는 inbuf에 들어 있는 텍스트의 길이가 정확하게 들어가야 한다. 그렇지 않은 경우 parse 에러가 발생함.*/
	p_ftstat->outbuf = SCX_MALLOC(cal_buf_size);
	p_ftstat->outbufsize = cal_buf_size;
	p_ftstat->tmpbuf = SCX_CALLOC(1, MEDIA_FORMAT_MAX_LEN);
	p_ftstat->tmpbufsize = MEDIA_FORMAT_MAX_LEN;
	p_ftstat->outbuflen = 0;
	p_ftstat->arg = (void *)req;

	/* 세션 정보를 argument 추가한다. */
	strm_update_session_argument(req);

	pprm.rebuild.enable = 1;
	pprm.rebuild.ih = ioh;
	pprm.rebuild.p = &p_ftstat->outbuf;
	pprm.rebuild.size = &p_ftstat->outbufsize;
	pprm.rebuild.len = &p_ftstat->outbuflen;

	pprm.cb.tag = strm_parse_tag_callback;
	pprm.cb.swapUrl = strm_parse_swap_url_callback;
	pprm.cb.param = (void *)p_ftstat;
	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
		__thread_jmp_working = 1;
		ret = zlive_m3u8_parse((const char *)p_ftstat->inbuf, p_ftstat->inbufsize, NULL, &pprm);
		__thread_jmp_working = 0;
	}
	else {
		/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
		ret = zipper_err_internal;
		TRACE((T_ERROR, "[%llu] zlive_m3u8_parse() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
	}

//	printf("manifest call size = %d, out size = %d\n", cal_buf_size, strlen(p_ftstat->outbuf));

	return ret;

}

/*
 * zlive_m3u8_parse 테스트용 함수
 * 실제 사용되지는 않는다.
 */
int
test_zlive_m3u8_parse(nc_request_t *req)
{
	int ret;
	zipper_io_handle *temp_ioh = NULL;
	char testfmt[] = {"ext_%f_end"};
	char *outbuffer  = NULL;
	size_t outsize = 0, allocsize = 0;
	zlive_m3u8_parse_param pprm;
	temp_ioh = (zipper_io_handle *)SCX_CALLOC(1, sizeof(zipper_io_handle));
	ASSERT(temp_ioh);
	memset(temp_ioh, 0, sizeof(zipper_io_handle));
	strm_set_io_memfunc(&temp_ioh);

	allocsize = 1000;
	outbuffer = SCX_CALLOC(1, allocsize);

	m3u8_format_template_stat ftstat;
	ftstat.outbuf = SCX_CALLOC(1, MEDIA_FORMAT_MAX_LEN);
	ftstat.outbuflen = MEDIA_FORMAT_MAX_LEN;
	ftstat.tmpbuf = SCX_CALLOC(1, MEDIA_FORMAT_MAX_LEN);
	ftstat.tmpbufsize = MEDIA_FORMAT_MAX_LEN;
	ftstat.arg = (void *)req;
	pprm.rebuild.enable = 1;
	pprm.rebuild.ih = temp_ioh;
	pprm.rebuild.p = &outbuffer;
	pprm.rebuild.size = &allocsize;
	pprm.rebuild.len = &outsize;

	pprm.cb.tag = strm_parse_tag_callback;
	pprm.cb.swapUrl = strm_parse_swap_url_callback;
	pprm.cb.param = (void *)&ftstat;

	char in_buffer[5000];
	//sprintf(in_buffer, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:13\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:5.796,\ncontent_0.ts\n#EXTINF:1.568,\ncontent_1.ts\n#EXTINF:1.401,\ncontent_2.ts\nEXTINF:2.536,\ncontent_3.ts\n#EXTINF:1.134,\ncontent_4.ts\n#EXT-X-ENDLI\n");
	sprintf(in_buffer, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:16052\n#EXTINF:4.004,\nmedia_16052.ts\n#EXTINF:4.004,\nmedia_16053.ts\n#EXTINF:4.004,\nmedia_16054.ts\n");
	ret = zlive_m3u8_parse((const char *)in_buffer, strlen(in_buffer), NULL, &pprm);
	if (ret != zipper_err_success) {
		scx_error_log(req, "zlive_m3u8_parse failed.(%s)\n", zipper_err_msg(ret));
	}
	else {
		printf("edited1 = '%s'\n", outbuffer);
	}
	SCX_FREE(ftstat.outbuf);
	SCX_FREE(ftstat.tmpbuf);

	SCX_FREE(outbuffer);
	SCX_FREE(temp_ioh);

}

/*
 요청된 미디어(매니패스트, 세그먼트 파일 등)에 대해 광고 대체, 수정 요청 등의 작업 요청
 수정된 사항이 있으면 out에 데이터를, outlen에 데이터 크기를 기술한다.
*/
int
strm_live_session_repack(nc_request_t *req)
{
	zipper_io_handle ioh;
    zlive_select_e sel;
	streaming_t	*streaming = req->streaming;
	int	ret = 1;
	int zres = 0;
	void *file = NULL;
	ssize_t need_size = 0;
	zipperBldr bldr = NULL;
	m3u8_format_template_stat ftstat;
	strm_live_io_ctx_t io_ctx;
	session_info_t *session = (session_info_t *)streaming->session;
	ASSERT(req->streaming->session);
	ASSERT(streaming->live_origin_path);
	memset(&ftstat, 0, sizeof(ftstat));
	memset(&ioh, 0, sizeof(ioh));
	memset(&io_ctx, 0, sizeof(io_ctx));
	memset(&io_ctx.objstat, 0, sizeof(nc_stat_t));
	strm_set_io_memfunc(&ioh);
	io_ctx.req = req;
	ioh.reader.readfp = strm_live_read_func;
	ioh.reader.sizefp = strm_live_size_func;
	ioh.reader.param = (void *)&io_ctx;

	/* 아래 두 라인 필요 없어 보임, strm_live_write_func()도 필요 없을듯 */
	ioh.writer.fp = strm_live_write_func;
	ioh.writer.param = (void *)&io_ctx;
	/* 여기서 오리진 파일을 open 하는 동작이 필요 할듯 */
	/* manifest나 ts 파일은 full GET으로 읽는다. */
	io_ctx.file = strm_file_open(req, O_CONTENT_TYPE_LIVE, streaming->live_origin_path, &io_ctx.objstat, O_RDONLY|O_NCX_NORANDOM);
	if (io_ctx.file == NULL){
		scx_error_log(req, "URL[%s] - open error (%d)\n", streaming->live_origin_path, strm_file_errno(req));
		req->p1_error = MHD_HTTP_NOT_FOUND;
		ret = 0;
		goto strm_live_session_repack_end;
	}
	if (session->l_info.ad_finished == 1) {
		/* 이 경우는 광고가 끝난 시점이기 때문에 zlive_select() 호출 없이 바로 원본을 내보내도록 한다. */
		sel = zlive_select_src;
	}
	else {
		sel = zlive_select(&ioh, session->l_info.z_ctx, streaming->live_origin_path);
	}

	switch(sel) {
	        case zlive_select_src:
	            /*
	             * POINT: 소스를 그대로 내보낸다. * 여기선 그냥 아무 것도 안한다(By Pass).
	             * TS와 manifest를 구분해서 동작해야함
	             * TS인 경우는 원본을 그대로 전달
	             * manifest인 경우는 body를 고쳐서 session 정보등을 추가해서 전달 해야함.
	             */
	        	//printf("%s() zlive_select_src selected.\n", __func__);
	        	if (streaming->media_type ==  MEDIA_TYPE_HLS_TS) {
	        		/* 원본 TS를 내보내는 경우는 광고가 끝난 시점이다. */
	        		streaming->source = strm_create_source_info(req, O_CONTENT_TYPE_LIVE, streaming->live_origin_path, NULL);
	        		if (streaming->source == NULL) {
	        			scx_error_log(req, "Host(%s), URL[%s] - open error (%d)\n", vs_data(req->service->name), streaming->live_origin_path, strm_file_errno(req));
	        			req->p1_error = MHD_HTTP_NOT_FOUND;
	        			ret = 0;
	        			goto strm_live_session_repack_end;
	        		}
	        		memcpy(&req->objstat, &streaming->source->objstat, sizeof(nc_stat_t));
	        		streaming->media_size = req->objstat.st_size;
	        		streaming->media_mode |= O_STRM_TYPE_DIRECT;
	        	}
	        	else {
	        		/* media_type이 MEDIA_TYPE_HLS_MAIN_M3U8나 MEDIA_TYPE_HLS_SUB_M3U8 인경우 */
	        		/* 원본의 manifest 응답에 session을 달아서 내보낸다 */
					/* 원본 파일을 모두 읽은 후에 strm_live_edit_hls_manifest_session()을 호출 하면 될듯 */

					ssize_t 			copied = 0;

					ftstat.inbuf = SCX_CALLOC(1, io_ctx.objstat.st_size+1);

					copied = strm_file_read(req, io_ctx.file, ftstat.inbuf, 0, io_ctx.objstat.st_size);
					if (copied != io_ctx.objstat.st_size) {
						scx_error_log(req, "URL[%s] read error (%d)\n", streaming->live_origin_path, strm_file_errno(req));
						req->p1_error = MHD_HTTP_NOT_FOUND;
						ret = 0;
						goto strm_live_session_repack_end;
					}
					ret = strm_live_edit_hls_manifest_session(req, &ioh, &ftstat);
					if (ret != zipper_err_success) {
						scx_error_log(req, "URL[%s] edit manifest failed.(%s)\n", streaming->live_origin_path, zipper_err_msg(ret));
						req->p1_error = MHD_HTTP_NOT_FOUND;
						ret = 0;
						goto strm_live_session_repack_end;
					}
					req->scx_res.body = ftstat.outbuf;
					ftstat.outbuf = NULL; /* 함수의 끝에서 memory 해제를 방지하기 위해 NULL로 설정 */

					streaming->media_size = strlen(req->scx_res.body);
					//printf("zlive_select_src media size = %d\n", streaming->media_size);
					req->objstat.st_mtime = io_ctx.objstat.st_mtime;
					strm_set_objstat(req);
	        	}
	        	ret = 1;
	            break;

	        case zlive_select_err:
	            // POINT: 오류가 발생하여 아무 것도 수행할 수 없음. * 여기선 그냥 아무 것도 안한다(By Pass).
	        	//printf("%s() zlive_select_err selected.(%s)\n", __func__,  zipper_err_msg(ret));
	    		scx_error_log(req, "URL[%s] - zlive_select_err error.\n", streaming->live_origin_path);
	    		req->p1_error = MHD_HTTP_NOT_FOUND;
	    		goto strm_live_session_repack_end;
	            break;

	        case zlive_select_ad: // POINT: 광고 세그먼트 내보내야 한다.
	        	//printf("%s() zlive_select_ad selected.\n", __func__);
				// POINT: 빌더 컨텍스트를 zlive로부터 얻는다.
	        	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
	        		__thread_jmp_working = 1;
	        		bldr = zlive_roll_build_context(session->l_info.z_ctx);
	        		__thread_jmp_working = 0;
	        	}
	        	else {
	        		/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
	        		bldr = NULL;
	        		TRACE((T_ERROR, "[%llu] zlive_roll_build_context() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
	        	}
				if(bldr) {
					zipper_builder_param bprm;
					buffer_paramt_t		buffer_param = {req, NULL, 0, 0, 1};

					// 광고 컨텍스트 Read 콜백으로 모드 변경
					ioh.reader.readfp = strm_source_reader;
					ioh.reader.sizefp = strm_source_size;
					ioh.reader.param = (void *)req->streaming;
					ioh.writer.param = (void *)&buffer_param;
					ioh.writer.fp = strm_write_func; 	/* 이부분은 필요한지 정확히 모르겠음 */
					zlive_roll_build_param(session->l_info.z_ctx, &bprm);
					bprm.attr.bldflag |= BLDFLAG_CAL_SIZE;
					/* 아래의 동작은 req->streaming->builder가 NULL인 것을 가정해서 동작 하기 때문에 NULL아닌 상태로 여기로 들어오는 경우가 있어서는 안된다. */
					ASSERT(req->streaming->builder == NULL);
					/* 아래의 동작은 req->streaming->source가 NULL인 것을 가정해서 동작 하기 때문에 NULL아닌 상태로 여기로 들어오는 경우가 있어서는 안된다. */
					ASSERT(req->streaming->source == NULL);
					/* 할수 zipper builder 종료후 req->streaming->builder는 반드시 NULL로 해주어야 한다. 그렇지 않은 경우 session 할당된 builder가 destroy된다. */
					req->streaming->builder = req->streaming->session->l_info.builder;
					if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
						__thread_jmp_working = 1;
						zres = zipper_build(&ioh, bldr, &bprm);
						__thread_jmp_working = 0;
					}
					else {
						/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
						zres = zipper_err_internal;
						TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
					}
					if(zres != zipper_err_success) {
						scx_error_log(req, "%s() '%s' zipper_build error(%s)\n", __func__, vs_data(req->url), zipper_err_msg(zres));
						req->p1_error = MHD_HTTP_NOT_FOUND;
						ret = 0;
						req->streaming->builder = NULL;
						if (req->streaming->source != NULL) {
							strm_destroy_source_info(req, req->streaming->source);
							req->streaming->source = NULL;
						}
						goto strm_live_session_repack_end;
					}

					bprm.attr.bldflag ^= BLDFLAG_CAL_SIZE;
					streaming->media_size = bprm.output.written;
					bprm.target.range_get.start = 0;
					bprm.target.range_get.size = streaming->media_size;
					buffer_param.offset = 0;
					buffer_param.buf = SCX_CALLOC(1, streaming->media_size);
					buffer_param.size = streaming->media_size;
					if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
						__thread_jmp_working = 1;
						zres = zipper_build(&ioh, bldr, &bprm);
						__thread_jmp_working = 0;
					}
					else {
						/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
						zres = zipper_err_internal;
						TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
					}
					if(zres != zipper_err_success) {
						scx_error_log(req, "%s() '%s' zipper_build error(%s)\n", __func__, vs_data(req->url), zipper_err_msg(zres));
						req->p1_error = MHD_HTTP_NOT_FOUND;
						ret = 0;
						req->streaming->builder = NULL;
						if (req->streaming->source != NULL) {
							strm_destroy_source_info(req, req->streaming->source);
							req->streaming->source = NULL;
						}
						goto strm_live_session_repack_end;
					}
					req->scx_res.body = (void *)buffer_param.buf;
					req->streaming->builder = NULL;
					if (req->streaming->source != NULL) {
						strm_destroy_source_info(req, req->streaming->source);
						req->streaming->source = NULL;
					}
					req->objstat.st_mtime = io_ctx.objstat.st_mtime;
					strm_set_objstat(req);

				}
				ret = 1;
	            break;

	        case zlive_select_repack_segment:
	        case zlive_select_repack_segment_with_fpadding:
	        case zlive_select_repack_manifest:
	        	//printf("%s() zlive_select_repack_segment selected.\n", __func__);
	        	need_size = io_ctx.objstat.st_size;

	        	// zlive_repack()시 출력 버퍼의 필요한 최대 예상 크기를 구한다.
	        	if (sel == zlive_select_repack_segment_with_fpadding) {
	        		need_size += 5640;
	        	}
	        	else if (sel == zlive_select_repack_manifest) {
	        		need_size += zlive_discontinuty_count(session->l_info.z_ctx) * (BYTES_OF_HLS_DISCONTINUTY_TAG + 5);
	        	}
				ftstat.inbuf = SCX_CALLOC(1, need_size+1);
				if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
					__thread_jmp_working = 1;
					zres = zlive_repack(&ioh, session->l_info.z_ctx, ftstat.inbuf);
					__thread_jmp_working = 0;
				}
				else {
					/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
					zres = zipper_err_internal;
					TRACE((T_ERROR, "[%llu] zlive_repack() SIGSEGV occured. path(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
				}
				if(zres == 0) {
					zres =  zlive_last_error(session->l_info.z_ctx);

					scx_error_log(req, "%s() '%s' zipper_build error(%s)\n", __func__, vs_data(req->url), zipper_err_msg(zres));
					req->p1_error = MHD_HTTP_NOT_FOUND;
					ret = 0;
					goto strm_live_session_repack_end;
				}
				if (sel == zlive_select_repack_manifest) {
					zres = strm_live_edit_hls_manifest_session(req, &ioh, &ftstat);
					if (zres != zipper_err_success) {
						scx_error_log(req, "URL[%s] edit manifest failed.(%s)\n", streaming->live_origin_path, zipper_err_msg(zres));
						req->p1_error = MHD_HTTP_NOT_FOUND;
						ret = 0;
						goto strm_live_session_repack_end;
					}
					req->scx_res.body = ftstat.outbuf;
					ftstat.outbuf = NULL; /* 함수의 끝에서 memory 해제를 방지하기 위해 NULL로 설정 */
					streaming->media_size = strlen(req->scx_res.body);
					printf("m3u8 = %s\n", req->scx_res.body);
				}
				else {
					/* zlive_select_repack_segment_with_fpadding와 zlive_select_repack_segment의 경우는 session 추가가 필요 없으므로 그냥 전송한다. */
					req->scx_res.body = ftstat.inbuf;
					ftstat.inbuf = NULL; /* 함수의 끝에서 memory 해제를 방지하기 위해 NULL로 설정 */
					streaming->media_size = zres;
					/* zres를 그냥 전달 하면 될듯 */

				}
				req->objstat.st_mtime = io_ctx.objstat.st_mtime;
				strm_set_objstat(req);
				ret = 1;
	            break;
	    }	//end of switch(sel)
strm_live_session_repack_end:
	if (ftstat.inbuf != NULL) {
		SCX_FREE(ftstat.inbuf);
		ftstat.inbuf = NULL;
	}
	if (ftstat.outbuf != NULL) {
		SCX_FREE(ftstat.outbuf);
		ftstat.outbuf = NULL;
	}
	if (ftstat.tmpbuf != NULL) {
		SCX_FREE(ftstat.tmpbuf);
		ftstat.tmpbuf = NULL;
	}
	if (io_ctx.file != NULL) {
		strm_file_close(req, io_ctx.file);
	}

	return ret;
}

//////////////////////////////////////////////////////////////// Live Ad Stitching 기능 End //////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////// CMAF Live 기능 Start //////////////////////////////////////////////////////////////
// https://jarvis.solbox.com/redmine/issues/32447 일감 참조

volatile long long int 	gscx__cmaf_id = 0;



/*
 * solCMAFIO의 memory 관련 callback function 등록
 */
static int
strm_set_cmaf_io_memfunc(solCMAFIO *ioh)
{
	ioh->mem.malloc = strm_malloc;
	ioh->mem.calloc = strm_calloc;
	ioh->mem.realloc = strm_realloc;
	ioh->mem.free = strm_free;
	return 1;
}
int
strm_cmaf_wakeup_connection(service_cmaf_t *cmaf, cmafAdaptiveIndex adapt)
{
	nc_request_t 	*req = NULL;
	int 			i, lsize = 0;
	int				cur_track = 0, end_track = 0;
	int	ret;
	if (adapt == 0) {
		/* 전체 트랙의 모든 connection들을 resume 시켜야 하는 경우 */
		cur_track = 1;
		end_track = cmafAdaptiveIndexMax;
	}
	else {
		/* 특정 트랙의 모든 connection들을 resume 시켜야 하는 경우 */
		cur_track = adapt;
		end_track = adapt + 1;
	}
//	pthread_mutex_lock(cmaf->plock);
	for(; cur_track < end_track; cur_track++) {
		pthread_mutex_lock(cmaf->plock[cur_track]);
		if (cmaf->suspend_list[cur_track]) {
			lsize = iList.Size(cmaf->suspend_list[cur_track]);
			if (lsize > 0) {
				for (i = 0; i < lsize; i++) {
					//req = *(nc_request_t **)iList.GetElement(cmaf->suspend_list[cur_track],i);
					ret = iList.PopFront(cmaf->suspend_list[cur_track],&req);
					if (ret < 0) {
						TRACE((T_ERROR, "Service List PopFront.(reason : %s)\n", iError.StrError(ret)));
					}
					else if(ret == 0) {
						break;
					}
					ASSERT(req->connection);
					ASSERT(req->is_suspeneded == 1);
					req->is_suspeneded = 0;	/* 이부분이 MHD_resume_connection() 뒤에 들어가게 되면 죽는 현상이 가끔 발생한다. */
					MHD_resume_connection(req->connection);
					TRACE((T_DEBUG,"cmaf resume track(%d), pos = %d\n", cur_track, req->cursor));
				}
				TRACE((T_DAEMON,"cmaf resume track(%d), count(%d)\n", cur_track, lsize));
			}

		}
		pthread_mutex_unlock(cmaf->plock[cur_track]);
	}
//	pthread_mutex_unlock(cmaf->plock);

	return 1;
}

static int
strm_cmaf_lib_event(solCMAF ctx, solCMAF_Event ev, cmafAdaptiveIndex adapt, uint32_t what, uint64_t tb, uint32_t length, void *param)
{
	service_cmaf_t *cmaf = (service_cmaf_t *)param;
	int				ret;
	ASSERT(cmaf);
    switch(ev) {

        case solCMAF_Event_Ready:
           // printf("solCMAF library %s ready. id=%d\n", (what == 1)?"Master":"Backup", cmaf->id);
        	/* what이 1이면 Master stream이고 0이면 Backup stream 임*/
            TRACE((T_INFO, "CMAF channel(%s) %s ready. id=%d\n", cmaf->name, (what == 1)?"Master":"Backup", cmaf->id));
            cmaf->ready = 1;
            cmaf->expired = 0;
            break;

        case solCMAF_Event_Error:
        	//printf("CMAF channel(%s) error=%d, id=%d\n", cmaf->name, what, cmaf->id);
        	TRACE((T_INFO, "CMAF channel(%s) error=%d, id=%d\n", cmaf->name, what, cmaf->id));
            cmaf->ready = 0;
            cmaf->expired = 1;
            strm_cmaf_wakeup_connection(cmaf, 0);

//            strm_cmaf_create_connect_thread(cmaf->service);
            break;

        case solCMAF_Event_Expired:
        	//printf("CMAF channel(%s) expired(EOF), id=%d, adapt=%d\n", cmaf->name, cmaf->id, adapt);
        	TRACE((T_INFO, "CMAF channel(%s) expired(EOF), id=%d, adapt=%d\n", cmaf->name, cmaf->id, adapt));
            cmaf->ready = 0;
            cmaf->expired = 1;
            strm_cmaf_wakeup_connection(cmaf, 0);

//            strm_cmaf_create_connect_thread(cmaf->service);
            break;

        case solCMAF_Event_AddChunk:
//        	printf("CMAF channel(%s) add chunk: id=%d, adapt=%u, seq=%u, timebase=%llu, length=%u\n", cmaf->name, cmaf->id, adapt, what, tb, length);
        	TRACE((T_DAEMON, "CMAF channel(%s) add chunk: id=%d,adapt=%u, seq=%u, timebase=%llu, length=%u\n", cmaf->name, cmaf->id, adapt, what, tb, length));
        	ATOMIC_SET(cmaf->cur_seq[adapt], what);
        	ATOMIC_SET(cmaf->cur_timebase[adapt], tb);
        	ATOMIC_SET(cmaf->cur_pos[adapt], length);
        	strm_cmaf_wakeup_connection(cmaf, adapt);
            break;
        case solCMAF_Event_Switch:
        	/* what이 1이면 Master stream이고 0이면 Backup stream 임*/
          //  printf( "solCMAF library  channel switch to %s. id=%d,\n", (what == 1)?"Master":"Backup", cmaf->id);
            TRACE((T_INFO, "CMAF channel(%s) switch to %s. id=%d,\n", cmaf->name, (what == 1)?"Master":"Backup", cmaf->id));

        	break;
        case solCMAF_Event_RepresentationAdded:
            {
            	solCMAFMediaInfo minfo;
            	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
            		__thread_jmp_working = 1;
            		ret = solCMAF_alloc_mediaInfo(ctx, adapt, &minfo);
            		__thread_jmp_working = 0;
            	}
            	else {
            		/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
            		ret = zipper_err_internal;
            		TRACE((T_ERROR, "solCMAF_alloc_mediaInfo() SIGSEGV occured. CMAF channel(%s) %s:%d\n", cmaf->name, __FILE__, __LINE__));
            	}
            	if(ret == zipper_err_success) {
                	//printf("representation[%s] added\n", minfo->idstr);
                	TRACE((T_INFO, "representation[%s] added\n", minfo.idstr));
                	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
                		__thread_jmp_working = 1;
                		solCMAF_free_mediaInfo(ctx, &minfo);
                		__thread_jmp_working = 0;
                	}
                	else {
                		/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
                		TRACE((T_ERROR, "solCMAF_free_mediaInfo() SIGSEGV occured. CMAF channel(%s) %s:%d\n", cmaf->name, __FILE__, __LINE__));
                	}
                }
            }
            break;
        case solCMAF_event_BeforeRepresentationRemoved:
            {
            	solCMAFMediaInfo minfo;
            	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
            		__thread_jmp_working = 1;
            		ret = solCMAF_alloc_mediaInfo(ctx, adapt, &minfo);
            		__thread_jmp_working = 0;
            	}
            	else {
            		/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
            		ret = zipper_err_internal;
            		TRACE((T_ERROR, "solCMAF_alloc_mediaInfo() SIGSEGV occured. CMAF channel(%s) %s:%d\n", cmaf->name, __FILE__, __LINE__));
            	}
				if(ret == zipper_err_success) {
                	//printf("representation[%s] will be removed\n", minfo->idstr);
                	TRACE((T_INFO, "representation[%s] will be removed\n", minfo.idstr));
                	if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
                		__thread_jmp_working = 1;
                		solCMAF_free_mediaInfo(ctx, &minfo);
                		__thread_jmp_working = 0;
                	}
                	else {
                		/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
                		TRACE((T_ERROR, "solCMAF_free_mediaInfo() SIGSEGV occured. CMAF channel(%s) %s:%d\n", cmaf->name, __FILE__, __LINE__));
                	}
                }
            }
            break;

        case solCMAF_Event_Disconnected:
        	//printf("CMAF channel(%s) disconnected. id=%d, adapt=%d, what=%d, errno=%d(%s)\n", cmaf->name, cmaf->id, adapt, what, tb, strerror(tb));
        	TRACE((T_INFO, "CMAF channel(%s) disconnected. id=%d, adapt=%d, what=%d, errno=%d(%s)\n", cmaf->name, cmaf->id, adapt, what, tb, strerror(tb)));
        	break;
        default:
            break;
    }

    return error_success; //error_success
}

/*
 * 두개의 오리진 url을 사용해서 hash 값을 생성
 */
uint32_t
strm_cmaf_make_hash(char * path_a, char * path_b)
{
	char *buf = NULL;
	uint8_t* ptr = NULL;
	uint32_t hash = 2166136261U;
    for (ptr = (uint8_t *)path_a; *ptr;) {
        hash = (hash ^ *ptr++) * 16777619U;
    }
    if (path_b != NULL) {
		for (ptr = (uint8_t *)path_b; *ptr;) {
			hash = (hash ^ *ptr++) * 16777619U;
		}
    }
    return hash;
}

/*
 * cmaf_origin, cmaf_origin_base_path, cmaf_origin_path를 사용해서 오리진 경로 생성
아래의 부분은 별도의 thread에서 진행 하도록 하는게 성능에 좋을것 같음
load 시
solCMAF_create() 호출
solCMAF_connect() 호출
reload 시
생성된 오리진 경로가 기존의 오리진 경로와 다른 경우 solCMAF_free()를 호출한후 위의 load 과정을 반복
 */
int
strm_cmaf_check(service_info_t *service)
{
	char * temp_path[2] = {NULL, NULL};
	char * old_origin_path[2] = {NULL, NULL};
	int path_len[2] = {0,0};
	int path_size[2] = {0,0};
	uint32_t path_hash = 0;
	int i = 0;
	int ret = 0;

	service_cmaf_t *cmaf = NULL;

	ASSERT(service->core);
	path_size[0] = vs_length(service->cmaf_origin[0]) + vs_length(service->cmaf_origin_path) + 5;
	if (service->cmaf_origin_base_path != NULL) {
		path_size[0] += vs_length(service->cmaf_origin_base_path);
	}
	temp_path[0] = SCX_CALLOC(1, path_size[0]);
	strcpy(temp_path[0], vs_data(service->cmaf_origin[0]));
	path_len[0] = strlen(temp_path[0]);
	*(temp_path[0]+path_len[0]++) = '/';
	if (service->cmaf_origin_base_path != NULL) {
		strcpy(temp_path[0]+path_len[0], vs_data(service->cmaf_origin_base_path));
		path_len[0] = strlen(temp_path[0]);
		*(temp_path[0]+path_len[0]++) = '/';
	}
	strcpy(temp_path[0]+path_len[0], vs_data(service->cmaf_origin_path));

	/*
	 * 경로에 포함된 slash를 제거 한다.
	 * 처음 부터 하면 http:// 부분의 //가 /로 바뀔수 있어서 중간 부분 부터 검사한다
	 */
	scx_remove_slash(temp_path[0]+vs_length(service->cmaf_origin[0]));
	path_len[0] = strlen(temp_path[0]);

	if (service->cmaf_origin[1] != NULL) {
		path_size[1] = vs_length(service->cmaf_origin[1]) + vs_length(service->cmaf_origin_path) + 5;
		if (service->cmaf_origin_base_path != NULL) {
			path_size[1] += vs_length(service->cmaf_origin_base_path);
		}
		temp_path[1] = SCX_CALLOC(1, path_size[1]);
		strcpy(temp_path[1], vs_data(service->cmaf_origin[1]));
		path_len[1] = strlen(temp_path[1]);
		*(temp_path[1]+path_len[1]++) = '/';
		if (service->cmaf_origin_base_path != NULL) {
			strcpy(temp_path[1]+path_len[1], vs_data(service->cmaf_origin_base_path));
			path_len[1] = strlen(temp_path[1]);
			*(temp_path[1]+path_len[1]++) = '/';
		}
		strcpy(temp_path[1]+path_len[1], vs_data(service->cmaf_origin_path));

		/*
		 * 경로에 포함된 중복된 /(slash)를 한개의 slash로 만든다.
		 * 처음 부터 하면 http:// 부분의 //가 /로 바뀔수 있어서 중간 부분 부터 검사한다
		 */
		scx_remove_slash(temp_path[1]+vs_length(service->cmaf_origin[1]));
		path_len[1] = strlen(temp_path[1]);
	}

//	printf("origin path = '%s'\n", temp_path[0]);

	path_hash = strm_cmaf_make_hash(temp_path[0], temp_path[1]);

	if (service->core->cmaf == NULL) {
		/* 볼륨 생성시 */
		cmaf = SCX_CALLOC(1, sizeof(service_cmaf_t));
        cmaf->ready = 0;
        cmaf->expired = 1;
        cmaf->id = ATOMIC_ADD(gscx__cmaf_id, 1);
		for (i = 0; i < cmafAdaptiveIndexMax; i++) {
#if 0
			cmaf->suspend_list[i] = iList.Create(sizeof (nc_request_t *));
#else
			cmaf->suspend_list[i] = iList.CreateWithAllocator(sizeof (nc_request_t *), &iCCLMalloc);
#endif
			cmaf->plock[i] = (pthread_mutex_t *)SCX_CALLOC(1, sizeof(pthread_mutex_t));
			pthread_mutex_init(cmaf->plock[i], NULL);
		}

		cmaf->origin_path[0] = temp_path[0];
		cmaf->origin_path[1] = temp_path[1];
		temp_path[0] = NULL;
		temp_path[1] = NULL;
		cmaf->name = SCX_CALLOC(1, vs_length(service->name) + 1);
		strncpy(cmaf->name, vs_data(service->name), vs_length(service->name));
		service->core->cmaf = cmaf;
		cmaf->origin_timeout = service->cmaf_origin_timeout;
		cmaf->retry_delay = 1000; /* 재시도 간격은 따로 설정으로 만들지 않고 여기서 1초로 하드코딩 한다. */
		cmaf->service = service;
		cmaf->path_hash = path_hash;
		strm_cmaf_create_ctx(service);
		pthread_mutex_lock(cmaf->plock[0]);
		cmaf->available = 1;
		/* cmaf_origin_connect_onload 설정이 1로 되어 있는 경우에만 볼륨 생성시 오리진으로 연결 하도록 한다. */
		if (service->cmaf_origin_connect_onload == 1) {
			pthread_create(&cmaf->monitor_tid, NULL, strm_cmaf_monitor, (void *)cmaf);
			TRACE((T_INFO, "CMAF monitor thread started. service(%s), id(%d)\n", vs_data(service->name), cmaf->id));
		}
		else {
			cmaf->monitor_tid = 0;
		}
		pthread_mutex_unlock(cmaf->plock[0]);

	}
	else {
		cmaf = service->core->cmaf;
		cmaf->service = service;
		if (cmaf->origin_timeout != service->cmaf_origin_timeout) {
			cmaf->origin_timeout = service->cmaf_origin_timeout;
		}
		if(cmaf->origin_path[0] == NULL) {
			/*
			 * cmaf 사용->미사용->사용 상태 일때 이곳이 실행
			 * 일부 정보는 재사용 해야함.
			 */
			TRACE((T_INFO, "Resume CMAF service.(%s), id(%d)\n", vs_data(service->name), cmaf->id));
			cmaf->origin_path[0] = temp_path[0];
			cmaf->origin_path[1] = temp_path[1];
			temp_path[0] = NULL;
			temp_path[1] = NULL;
			cmaf->path_hash = path_hash;
			/* 언제나 여기에 들어오는 경우는 모니터링 thread가 내려가 있어야 한다. */
			pthread_mutex_lock(cmaf->plock[0]);
			ASSERT(cmaf->monitor_tid == 0);
			if (service->cmaf_origin_connect_onload == 1) {
				cmaf->available = 1;
				pthread_create(&cmaf->monitor_tid, NULL, strm_cmaf_monitor, (void *)cmaf);
				TRACE((T_INFO, "CMAF monitor thread started. service(%s), id(%d)\n", vs_data(service->name), cmaf->id));
			}
			pthread_mutex_unlock(cmaf->plock[0]);

		}
		/* 오리진 경로가 변경 되었는지 확인 */
		else if (cmaf->path_hash == path_hash) {
			/* 오리진 경로가 동일한 경우는 따로 할 일이 없음 */
		}
		else {
			/* 오리진 경로가 변경된 경우 */
			/* 오리진 정보를 먼저 변경 */
			old_origin_path[0] = cmaf->origin_path[0];
			cmaf->origin_path[0] = temp_path[0];
			temp_path[0] = NULL;
			/* 백업 오리진은 있다가 없어지거나 반대의 경우도 있을수 있기 때문에 처리가 복잡하다. */
			if (cmaf->origin_path[1] != NULL) {
				old_origin_path[1] = cmaf->origin_path[1];
			}
			if (temp_path[1] != NULL) {
				cmaf->origin_path[1] = temp_path[1];
				temp_path[1] = NULL;
			}
			else {
				cmaf->origin_path[1] = NULL;
			}
			cmaf->path_hash = path_hash;
			strm_cmaf_close(service); /* 기존 오리진 연결을 종료 한다. */
            SCX_FREE(old_origin_path[0]); /* 기존 path 정보를 가지고 있는 메모리를 해제 한다. */
            if (old_origin_path[1] != NULL) {
            	SCX_FREE(old_origin_path[1]);
            }
		}

	}

	ret = 1;
cmaf_check_cmaf_end:
	if (temp_path[0] != NULL) {
		SCX_FREE(temp_path[0]);
	}
	if (temp_path[1] != NULL) {
		SCX_FREE(temp_path[1]);
	}
	return ret;
}

int
strm_cmaf_create_ctx(service_info_t *service)
{
	solCMAFCreateParam ccp;
	service_cmaf_t *cmaf = NULL;
	int	ret = 0;

	ASSERT(service->core->cmaf);
	cmaf = service->core->cmaf;

	strm_set_cmaf_io_memfunc(&cmaf->io);

	cmaf->io.write.param = NULL;
	memset(&ccp, 0, sizeof(solCMAFCreateParam));
    ccp.pull.timeout = cmaf->origin_timeout; // 소켓 타임아웃, 초단위, 0이면 무시
    ccp.pull.retry = cmaf->retry_delay; 	// 소켓 통신 실패 시 재시도 간격(밀리세컨드, 0~)
    // 세그먼트 리스트 및 캐싱 범위 설정
    ccp.stack.cache = service->cmaf_fragment_cache_count;	// 메모리 상에 캐시될 Fragment의 최대 수
    ccp.stack.prefetch = service->cmaf_prefetch_fragment_count;	// 오리진 연결시 가져올 과거 Fragment수
    ccp.stack.manifest = service->cmaf_manifest_fragment_count;	// 매니패스트 상에 기술된 Fragment의 최대 수

    // 이벤트 콜백 설정 (전역)
    ccp.event.cb = strm_cmaf_lib_event;
    ccp.event.param = cmaf;

    ccp.support.rtmp = 1;	/* rtmp live 기능을 사용하기 위해서는 이 값을 1로 설정해주어야 한다. */
    ccp.opt.rpidx = 1;	/* Representation id가 문자열이 아닌 숫자로 사용 */
    ccp.opt.always_use_list = 0;	/* 매니패스트 출력 시 항상 세그먼트 리스트를 명시, 임시 테스트용 */

    TRACE((T_INFO, "'%s' connect CMAF origin. id(%d)\n", vs_data(service->name), cmaf->id));
    //printf("'%s' connect CMAF origin.\n", vs_data(service->name));
    if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
    	__thread_jmp_working = 1;
    	ret = solCMAF_create(&cmaf->io, &ccp, &cmaf->ctx);
    	__thread_jmp_working = 0;
    }
    else {
    	/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
    	ret = zipper_err_internal;
    	TRACE((T_ERROR, "solCMAF_create() SIGSEGV occured. Service(%s). %s:%d\n", vs_data(service->name), __FILE__, __LINE__));
    }

    if (ret != error_success) {
    	/* 여기서 에러가 발생하는 경우는 메모리 할당 불가 상태이기 때문에 서비스가 불가능하다. */
    	TRACE((T_ERROR, "Failed to create CMAF context. Service(%s), id(%d)\n", vs_data(service->name), cmaf->id));
    	ASSERT(0);
    }

	return 1;

}

int
strm_cmaf_close(service_info_t *service)
{
	service_cmaf_t *cmaf = NULL;

	ASSERT(service->core->cmaf);
	cmaf = service->core->cmaf;
	/* 기존에 연결되어 있는 사용자 들을 모두 종료 시키기 위해서 아래처럼 마킹후 resume을 호출 한다. */
    cmaf->ready = 0;
    cmaf->expired = 1;
    strm_cmaf_wakeup_connection(cmaf, 0);
    /*
     * solCMAF_close() 호출시 solCMAF_Event_Expired 이벤트가 발생
     * 이후 monitoring thread에서 solCMAF_connect()가 호출됨
     */
	solCMAF_close(&cmaf->io, cmaf->ctx);


	return 1;

}

/*
 * CMAF 기능을 사용하다가 사용하지 않는 경우 호출
 */
int
strm_cmaf_stop(service_info_t *service)
{
	service_cmaf_t *cmaf = NULL;
	cmaf = service->core->cmaf;
	char * temp_path[2];

	if (cmaf->origin_path[0] == NULL) {
		/* 이전에 이미 cmaf를 사용 중지 한 경우 이기 때문에 그냥 리턴한다. */
		return 1;
	}

	TRACE((T_INFO, "Stop CMAF service.(%s), id(%d)\n", vs_data(service->name), cmaf->id));
//	pthread_mutex_lock(cmaf->plock);
	temp_path[0] = cmaf->origin_path[0];
	temp_path[1] = cmaf->origin_path[1];
	cmaf->origin_path[0] = NULL;
	cmaf->origin_path[1] = NULL;
	SCX_FREE(temp_path[0]);
	if (temp_path[1] != NULL) {
		SCX_FREE(temp_path[1]);
	}
//	pthread_mutex_unlock(cmaf->plock);
	cmaf->available = 0;
    pthread_mutex_lock(cmaf->plock[0]);
	if (cmaf->monitor_tid)
	{
		pthread_cancel(cmaf->monitor_tid);
		pthread_join(cmaf->monitor_tid, NULL);
		cmaf->monitor_tid = 0;
	}
	pthread_mutex_unlock(cmaf->plock[0]);
	strm_cmaf_close(service);
	return 1;
}

int
strm_cmaf_destroy(service_info_t *service)
{
	nc_request_t *req = NULL;
	int i = 0, cur = 0;
	int lsize = 0;
	int ret = 0;
	service_cmaf_t *cmaf = NULL;
	cmaf = service->core->cmaf;
	cmaf->available = 0;
    cmaf->ready = 0;
    pthread_mutex_lock(cmaf->plock[0]);
	if (cmaf->monitor_tid)
	{
		pthread_cancel(cmaf->monitor_tid);
		pthread_join(cmaf->monitor_tid, NULL);
		cmaf->monitor_tid = 0;
	}
	pthread_mutex_unlock(cmaf->plock[0]);
//	pthread_mutex_lock(cmaf->plock);
	for (cur = 0; cur < cmafAdaptiveIndexMax; cur++) {
		pthread_mutex_lock(cmaf->plock[cur]);
		if (cmaf->suspend_list[cur]) {
			lsize = iList.Size(cmaf->suspend_list[cur]);
			ASSERT(lsize == 0); /* 현재 예상으로는 사용자가 모두 없어진 경우에만 strm_cmaf_destroy()가 호출 되어야 하기 때문에 resume 되는 경우가 없어야 한다. */
			for (i = 0; i < lsize; i++) {
				req = *(nc_request_t **)iList.GetElement(cmaf->suspend_list[cur],i);
				/* 여기서 suspend 되어 있는 socket을 모두 resume 시켜 주면 될것 같음 */
				ASSERT(req->connection);
				ASSERT(req->is_suspeneded == 1);
				req->is_suspeneded = 0;
				MHD_resume_connection(req->connection);
			}
			ret = iList.Finalize(cmaf->suspend_list[cur]);
			if (0 > ret) {
				TRACE((T_ERROR, "CMAF client List remove failed.(reason : %s). service(%s), id = %d\n", iError.StrError(ret), vs_data(service->name), cmaf->id));
			}
			cmaf->suspend_list[cur] = NULL;
		}
		pthread_mutex_unlock(cmaf->plock[cur]);
		pthread_mutex_destroy(cmaf->plock[cur]);
		SCX_FREE(cmaf->plock[cur]);
	}

//	pthread_mutex_unlock(cmaf->plock);
	/*
	 * 아래 부분은 사용자가 있는 경우 문제가 되는데  실행되는 시점은 고민해볼 필요가 있음
	 * vm_destroy_service()가 해당 볼륨의 사용자가 없을때 호출 되기 때문에 바로 지워도 될걸로 보임
	 */

	if (cmaf->origin_path[0] != NULL) {
		SCX_FREE(cmaf->origin_path[0]);
	}
	if (cmaf->origin_path[1] != NULL) {
		SCX_FREE(cmaf->origin_path[1]);
	}
	if (cmaf->ctx != NULL) {
		solCMAF_close(&cmaf->io, cmaf->ctx);
		solCMAF_free(&cmaf->io, &cmaf->ctx);
		cmaf->ctx = NULL;
	}
	if (cmaf->name != NULL) {
		SCX_FREE(cmaf->name);
	}
	//SCX_FREE(cmaf->plock)
	SCX_FREE(cmaf);

	return 1;
}

typedef enum {
    strm_CMAF_build_manifest = 0,    // solCMAF_build_manifest 호출시
	strm_CMAF_build_init,     // solCMAF_build_init 호출시
	strm_CMAF_build_fragment          // solCMAF_build_fragment 호출시
} strm_CMAF_build_type;
/*
 * cmaf write callback 호출시 필요 정보를 전달하기 위한 구조체
 */
typedef struct cmaf_write_param_tag
{
	service_cmaf_t *cmaf;
	int	build_type;
	nc_request_t *req;
    char *tmpbuf;	// manifest 수정시 사용하는 임시 작업용 buffer
	size_t tmpbufsize;
    char *outbuf;	// req->scx_res.body 가 들어가면 된다.
    size_t outbufsize; // 버퍼의 할당 크기
    size_t outbufpos; // 현재까지 쓰여진 위치
} cmaf_write_param_t;


size_t
strm_cmaf_lib_write(uint8_t *p, size_t size, void *param)
{
	cmaf_write_param_t *write_param = (cmaf_write_param_t *)param;
	TRACE((T_DEBUG, "[%llu]called, size = %d\n", write_param->req->id, size));
    memcpy(write_param->outbuf+write_param->outbufpos, p, size);
    write_param->outbufpos += size;
    return size;
}
/*
 * 주기적으로 실행되면서 cmaf 관련 동작을 한다.
 */
void *
strm_cmaf_monitor(void *d)
{
	service_cmaf_t *cmaf = (service_cmaf_t *)d;
	int interval = 500;	/* 일단 500msec마다 실행 하도록 한다. */
	solCMAFIO io;
	int	ret = 0;
	cmaf_write_param_t write_param;
	uint64_t last_send = 0;
	uint64_t current_send = 0;
	time_t check_time = 0;	/* 전송량 변화가 없는 경우 시작 시간 */
	time_t current_time;
	service_info_t *service = NULL;
	double 		ts, te;

	memset(&write_param, 0, sizeof(cmaf_write_param_t));

	write_param.cmaf = cmaf;
	strm_set_cmaf_io_memfunc(&io);
    io.write.cb = strm_cmaf_lib_write;
	io.write.param = NULL;

	TRACE((T_INFO, "cmaf monitor thread started(%s). id(%d)\n", cmaf->name, cmaf->id));
	while (gscx__signal_shutdown == 0 && cmaf->available == 1) {
		/* reload로 service가 변경되는 경우가 발생할수 있어서 아래의 루틴 실행 전에 service 정보를 기억한다. */
		service = cmaf->service;
		ASSERT(service != NULL);


		if (cmaf->origin_path[0] == NULL) {
			/* CMAF 를 사용 중지한 경우 */
		}
		else if (cmaf->ready == 0 && cmaf->expired == 1) {
        	//bt_msleep(1000);	/* 이벤트가 발생하게 된후 최소 1초 후에 아래의 과정을 실행 하도록 한다. */
            /* solCMAF_connect() 호출시 넘겨준 io handler는 solCMAF_free()가 호출 되기 전까지는 solCMAF 라이브러리에서 계속 사용되므로 해제하면 안된다. */
			pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
			/* solCMAF 러이브러리 소스 URL 리셋 함수 */
			solCMAF_resetSource(&cmaf->io, cmaf->ctx);
			/* master 설정(1)과 별개로 첫번째 추가하는 소스 URL은 마스터로 취급되며, 이후 master 설정 시 변경된다. */
			solCMAF_addSource(&cmaf->io, cmaf->ctx, cmaf->origin_path[0], 1);
			if (cmaf->origin_path[1] != NULL) {
				solCMAF_addSource(&cmaf->io, cmaf->ctx, cmaf->origin_path[1], 0);
			}
			ts = sx_get_time();
        	TRACE((T_INFO, "Start to connect CMAF origin(%s). id(%d)\n", cmaf->name, cmaf->id));
        	ret = solCMAF_connect(&cmaf->io, cmaf->ctx, 1, cmaf->origin_timeout);
        	te = sx_get_time();
            if (ret != error_success)  {
            	TRACE((T_WARN, "Failed to connect CMAF origin(%s). reason(%d), id(%d), delay(%.2f)\n", cmaf->name, ret, cmaf->id, (float)((te-ts)/1000000.0)));
            	bt_msleep(1000 * cmaf->origin_timeout); /* 오리진 접속 실패시 바로 접속하지 않고 일정시간 동안 대기 했다가 다시 연결을 시도한다. */
            }
            else {
            	/* error_success 라고 해도 실제 오리진에 연결된 상태가 아니기 때문에 expire 상태만 바꿔 준다. */
            	TRACE((T_INFO, "Success to connect CMAF origin(%s). id(%d), delay(%.2f)\n", cmaf->name, cmaf->id, (float)((te-ts)/1000000.0)));
            	cmaf->expired = 0; /* 오리진이 정상 상태로 돌아 오기 전에 잘못 판단해서 다시 connect를 호출하는 현상을 방지 하기 위해 설정 */
            }
    		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        	last_send = 0;
        	check_time = scx_get_cached_time_sec();
        }
		if (service->cmaf_origin_disconnect_time > 0) {
			/*
			 * 일정 시간 동안 사용자 요청이 없는 경우 연결 종료할지 판단하는 부분
			 *
			 * 오리진과 연결중 상태(cmaf->ready 가 0이고 cmaf->expired가 1인 경우)일때에는 이 검사를 하지 않는다.
			 ** 연결중인 상태인 경우는 오리진 연결이 되지 않은 상태이므로 사용자가 계속 요청을 하고 있어도 에러가 발생하기 때문에 트래픽이 없을수 있다
			 */

			current_send = scx_get_volume_send_size(service);
			if (last_send == current_send) {
				/*
				 * 전송량이 증가하지 않은 경우
				 * 예외처리
				 ** 오리진과 정상적으로 연결 되어 있는 상태인지 확인
				 *** cmaf->ready 가 0인 경우는 오리진 연결이 되지 않은 상태이므로 사용자가 계속 요청을 하고 있어도 에러가 발생하기 때문에 트래픽이 없을수 있다.
				 ** scx_get_volume_concurrent_count()로 현재 연결된 사용자가 없는 것을 확인
				 ** 위의 두가지 경우에 해당하면 check_time을 0으로 설정
				 * check_time이 0인 경우
				 ** check_time을 현재 시간으로 설정
				 * check_time이 0이 아닌 경우
				 ** 비교를 위한 시간(check_time)과 현재 시간을 비교해서 cmaf_origin_disconnect_time 시간을 지났는지 확인
				 *** 시간이 지난 경우 오리진과 연결 종료 및 thread 종료
				 */
				if (scx_get_volume_concurrent_count(service) > 0) {
					/* 사용자가 연결 되어 있는 경우는 스트리밍 중으로 판단한다. */
					check_time = scx_get_cached_time_sec();
				}
				else {

					/* 사용자 연결 및 요청 트래픽이 없는 상태 */
					current_time = scx_get_cached_time_sec();
					if (check_time == 0) {
						check_time = current_time;
					}
					else if (current_time > (check_time + service->cmaf_origin_disconnect_time)) {
						/* cmaf_origin_disconnect_time에 지정된 시간이 지난 경우 오리진과의 연결을 종료하고 thread도 중지 한다. */
						TRACE((T_INFO, "cmaf idle status detect.(%s), id(%d)\n", cmaf->name, cmaf->id));
//						sleep(20);
						pthread_mutex_lock(cmaf->plock[0]);

#if 0
						if (cmaf->monitor_tid == 0) {
							/* strm_cmaf_stop()나 strm_cmaf_destroy()에서 pthread_cancel이 호출된 경우 이므로 아래의 루틴을 타지 않고 바로 종료한다. */
							pthread_mutex_unlock(cmaf->plock[0]);
							break;
						}
#endif
						cmaf->ready = 0;
						cmaf->expired = 1;
						cmaf->monitor_tid = 0;
						solCMAF_close(&cmaf->io, cmaf->ctx);
						pthread_mutex_unlock(cmaf->plock[0]);
						TRACE((T_INFO, "cmaf monitor thread stopped.(%s), id(%d)\n", cmaf->name, cmaf->id));
						pthread_detach(pthread_self());	/* thread에서 자체적으로 종료하는 경우 이므로 detach를 호출해주어야지만 메모리 누수가 없다. */
						pthread_exit(0);
					}
				}
			}
			else
			{
				/* 전송량이 증가한 경우 */
				last_send = current_send;
				check_time = scx_get_cached_time_sec();
			}


		}


		bt_msleep(interval);
	}
	TRACE((T_INFO, "cmaf monitor thread stopped.(%s), id(%d)\n", cmaf->name, cmaf->id));
	pthread_exit(0);
}

/*
 * cmaf origin과 연결이 된 상태인지 확인 한다.
 */
int
strm_cmaf_check_ready(nc_request_t *req)
{
	streaming_t 	*streaming = req->streaming;
	service_info_t 	*service = req->service;
	service_cmaf_t *cmaf = service->core->cmaf;
	int 		retry_cnt = 0;
	if (cmaf->ready == 0) {
		/* 오리진과 통신이 준비 되지 않은 경우 */
		pthread_mutex_lock(cmaf->plock[0]);
		if (cmaf->monitor_tid == 0) {
			pthread_create(&cmaf->monitor_tid, NULL, strm_cmaf_monitor, (void *)cmaf);
			TRACE((T_INFO, "CMAF monitor thread started. service(%s), id(%lld)\n", vs_data(service->name), cmaf->id));
		}
		else {
			pthread_mutex_unlock(cmaf->plock[0]);
			req->p1_error = MHD_HTTP_NOT_FOUND;
			return 0;
		}
		pthread_mutex_unlock(cmaf->plock[0]);
		while (cmaf->ready == 0) {
			if (gscx__signal_shutdown == 1 ||  cmaf->available == 0) {
				req->p1_error = MHD_HTTP_NOT_FOUND;
				return 0;
			}
			else if (retry_cnt > 50) {
				/* 500 msec 만큼 대기하면서 오리진 연결상태를 확인한다. */
				req->p1_error = MHD_HTTP_NOT_FOUND;
				return 0;
			}

			retry_cnt++;
			bt_msleep(10);
		}
	}
	return 1;
}

int
strm_cmaf_build_response(nc_request_t *req)
{
	streaming_t 	*streaming = req->streaming;
	service_info_t 	*service = req->service;
	service_cmaf_t *cmaf = service->core->cmaf;
	size_t 		require_size = 0;
	int 		alloc_len = 0;
	int 		bufpos = 0;
	solCMAFIO 	io;
	int			track_id;
	double 			ts, te;
	cmaf_write_param_t write_param;
	scFragment fragment = NULL;
	int ret = 0;
	solCMAFManifestPath mp;
	int muxbuf_size = 0;
	write_param.req = req;
	ASSERT(cmaf);
#if 0
	int 		retry_cnt = 0;
	if (cmaf->ready == 0) {
		/* 오리진과 통신이 준비 되지 않은 경우 */
		pthread_mutex_lock(cmaf->plock[0]);
		if (cmaf->monitor_tid == 0) {
			pthread_create(&cmaf->monitor_tid, NULL, strm_cmaf_monitor, (void *)cmaf);
			TRACE((T_INFO, "CMAF monitor thread started. service(%s), id(%lld)\n", vs_data(service->name), cmaf->id));
		}
		else {
			pthread_mutex_unlock(cmaf->plock[0]);
			req->p1_error = MHD_HTTP_NOT_FOUND;
			return 0;
		}
		pthread_mutex_unlock(cmaf->plock[0]);
		while (cmaf->ready == 0) {
			if (gscx__signal_shutdown == 1 ||  cmaf->available == 0) {
				req->p1_error = MHD_HTTP_NOT_FOUND;
				return 0;
			}
			else if (retry_cnt > 50) {
				/* 500 msec 만큼 대기하면서 오리진 연결상태를 확인한다. */
				req->p1_error = MHD_HTTP_NOT_FOUND;
				return 0;
			}

			retry_cnt++;
			bt_msleep(10);
		}
	}
#else
	if (strm_cmaf_check_ready(req) == 0) {
		return 0;
	}
#endif


	strm_set_cmaf_io_memfunc(&io);
    io.write.cb = strm_cmaf_lib_write;
	io.write.param = NULL;
	memset(&req->objstat, 0, sizeof(nc_stat_t));

	switch(streaming->media_type) {

	case MEDIA_TYPE_MPEG_DASH_MANIFEST:
	    /* 아래의 부분들은 현재는 solCMAF_create()단계 에서 사용하지만 solCMAF_build_manifest() 호출 시점에 설정 할수 있도록 변경 되어야 한다. */
	    /* 매니패스트 상에 기술된 각 구성 요소의 경로 포맷
	     * 포맷에는 다음의 매크로 문자열을 사용할 수 있다.
	     *
	     * %a : cmafAdaptiveIndex (각 서브 트랙의 타입과 관계없이 단위 증가 값, DASH에서는 $RepresentationID$로 대체)
	     * %t : Fragment Base 시간 (DASH 매니패스트 상에서는 $Time$으로 대체)
	     * %i : Fragment 인덱스 (DASH 매니패스트 상에서는 $Number$로 대체)
	     *
	     * DASH 매니패스트 상에서는 Representation id는 '%s/%a'로 기술된다. 예) audio/1, video/2
	     */
		alloc_len = streaming->real_path_len + 1;
		if(service->dash_manifest_use_absolute_path == 1) {
			alloc_len += vs_length(service->dash_manifest_host_domain) + 9; /* 'https://' 포함 */
			if (service->manifest_prefix_path) {
				alloc_len += vs_length(service->manifest_prefix_path) + 1; /* 서비스명-볼륨명 형식으로 첫번째 디렉토리에 들어간다. */
			}
		}
		mp.baseUrl = (char *) mp_alloc(streaming->mpool, alloc_len);
		if(service->dash_manifest_use_absolute_path == 1) {
			bufpos = snprintf((char *)mp.baseUrl, alloc_len, "%s://%s",
							req->dash_manifest_protocol?"https":"http", vs_data(service->dash_manifest_host_domain));
			if (service->manifest_prefix_path) {
				snprintf((char *)mp.baseUrl+bufpos, alloc_len-bufpos,
						"/%s", vs_data(service->manifest_prefix_path));
				bufpos = strlen(mp.baseUrl);
			}

		}
		muxbuf_size = alloc_len;
		snprintf((char *)mp.baseUrl + bufpos, streaming->real_path_len + 1 ,"%s", vs_data(req->ori_url)); /* '/'까지 기록하기 위해 real_path_len+1을 한다.*/

		if (streaming->argument) {
			alloc_len = strlen(streaming->argument) + 30;
			mp.manifest = (char *) mp_alloc(streaming->mpool, alloc_len);
			mp.init = (char *) mp_alloc(streaming->mpool, alloc_len);
			mp.fragment = (char *) mp_alloc(streaming->mpool, alloc_len);

			snprintf((char *)mp.manifest, alloc_len, "content_%%a.m3u8%s", streaming->argument);
			snprintf((char *)mp.init, alloc_len, "%%a_%%s_init.m4s%s", streaming->argument);
			snprintf((char *)mp.fragment, alloc_len, "%%a_%%s_segment_%%i.m4s%s", streaming->argument);
			muxbuf_size += alloc_len;
		}
		else {
			/* argument 가 없는 경우는 단순하다. */
			alloc_len = 30;
			mp.manifest = (char *) mp_alloc(streaming->mpool, alloc_len);
			mp.init = (char *) mp_alloc(streaming->mpool, alloc_len);
			mp.fragment = (char *) mp_alloc(streaming->mpool, alloc_len);
			snprintf((char *)mp.manifest, alloc_len, "content_%%a.m3u8");// 서브 매니패스트 경로 (CMAF HLS 전용)
			snprintf((char *)mp.init, alloc_len, "%%a_%%s_init.m4s");
			snprintf((char *)mp.fragment, alloc_len, "%%a_%%s_segment_%%i.m4s");
			muxbuf_size += alloc_len;
		}
		mp.muxbuf = (char *) mp_alloc(streaming->mpool, muxbuf_size);
		mp.tunnel = "%a_%s_tunnel.cmaf";		// 터널링 기능은 argument 없이 사용한다.
		/* 만들어질 manifest의 크기를 알아낸다. */
		ts = sx_get_time();
		require_size = solCMAF_build_manifest(&io, cmaf->ctx, solCMAF_DashStream, cmafAdaptiveIndexMaster, &mp, 1);
		req->scx_res.body = SCX_CALLOC(1, require_size);
		streaming->media_size = require_size;
		write_param.build_type = strm_CMAF_build_manifest;
		write_param.outbuf = req->scx_res.body;
		write_param.outbufsize = require_size;
		write_param.outbufpos = 0;
		io.write.param = (void *)&write_param;
		ret = solCMAF_build_manifest(&io, cmaf->ctx, solCMAF_DashStream, cmafAdaptiveIndexMaster,  &mp, 0);
		te = sx_get_time();
		req->t_zipper_build += (te - ts);
		strm_set_objstat(req);
		break;
	case MEDIA_TYPE_MPEG_DASH_AUDIO_INIT:
	case MEDIA_TYPE_MPEG_DASH_VIDEO_INIT:
		track_id = streaming->rep_num;
		ts = sx_get_time();
		require_size = solCMAF_build_init(&io, cmaf->ctx, track_id, 1);
		req->scx_res.body = SCX_MALLOC(require_size);
		streaming->media_size = require_size;
		write_param.build_type = strm_CMAF_build_init;
		write_param.outbuf = req->scx_res.body;
		write_param.outbufsize = require_size;
		write_param.outbufpos = 0;
		io.write.param = (void *)&write_param;
		ret = solCMAF_build_init(&io, cmaf->ctx, track_id, 0);
		te = sx_get_time();
		req->t_zipper_build += (te - ts);
		strm_set_objstat(req);
		break;
	case MEDIA_TYPE_MPEG_DASH_AUDIO:
	case MEDIA_TYPE_MPEG_DASH_VIDEO:
		/* 여기서는 fragment가 완료된 경우에만 처리하고 만들어지는 중인 경우는 streaming_reader()에서 처리 할수 있도록 한다. */
		ASSERT(streaming->fragment == NULL);
		track_id = streaming->rep_num;
		ts = sx_get_time();
		streaming->fragment = solCMAF_get_fragment(cmaf->ctx, track_id, streaming->ts_num, 0);
		te = sx_get_time();
		req->t_zipper_build += (te - ts);
		if (streaming->fragment != NULL) {
			if(solCMAF_isCompleted_fragment(streaming->fragment) == YES) {
				/* fragment가 완성된 경우 */
				streaming->media_size = solCMAF_sizeOf_fragment(streaming->fragment);
				strm_set_objstat(req);
			}
			else {
				/* fragment를 생성중인 경우 */
				/* chunked로 전송하기 위한 property를 설정 */
				streaming->media_size = 0;
				strm_set_objstat(req);
				req->objstat.st_rangeable = 0;
				req->objstat.st_sizeknown = 0;
				req->objstat.st_sizedecled = 0;
				req->objstat.st_chunked = 1;
			}

		}
		else {
			/* 없는 fragment(segment) 를 요청한 경우 */
			req->p1_error = MHD_HTTP_NOT_FOUND;
			return 0;
		}
		break;
	case MEDIA_TYPE_CMAF_VIDEO_TUNNEL:
	case MEDIA_TYPE_CMAF_AUDIO_TUNNEL:
		track_id = streaming->rep_num;
		streaming->fragment = solCMAF_startOf_tunnel(cmaf->ctx, track_id);
		if (streaming->fragment != NULL) {
			/* chunked로 전송하기 위한 property를 설정 */
			streaming->media_size = 0;
			strm_set_objstat(req);
			req->objstat.st_rangeable = 0;
			req->objstat.st_sizeknown = 0;
			req->objstat.st_sizedecled = 0;
			req->objstat.st_chunked = 1;
		}
		else {
			/* 없는 fragment(segment) 를 요청한 경우 */
			req->p1_error = MHD_HTTP_NOT_FOUND;
			return 0;
		}

		break;
	default :
			req->p1_error = MHD_HTTP_NOT_FOUND;
			return 0;
		break;
	}

//	strm_set_objstat(req);

	return 1;
}

/*
 * cmaf live 기능으로 동작시 MHD의 reader callback 과정 streaming_reader()를 통해서 호출 된다.
 */
ssize_t
strm_cmaf_reader(void *cr, uint64_t pos, char *buf, size_t max)
{
	nc_request_t 	*req = cr;
	ssize_t 		copied = 0;
	ssize_t 		sendto = 0; /* 전송할 크기 */
	ssize_t			remain = 0;
	size_t 			fragment_pos = 0;
	int				ret = 0;
	double 			ts, te;
	int				track;
	scFragment 		last_fragment = NULL;

	streaming_t		*streaming = req->streaming;
	service_info_t 	*service = req->service;
	service_cmaf_t 	*cmaf = service->core->cmaf;
	cmaf_write_param_t write_param;
	solCMAFIO 	io;

	ASSERT(streaming->fragment != NULL);
	write_param.req = req;
	if (cmaf->ready == 0) {
		/* 오리진 연결이 끊어진 경우라고 보고 전송 완료를 리턴한다. */
		return MHD_CONTENT_READER_END_OF_STREAM;
	}
	strm_set_cmaf_io_memfunc(&io);
    io.write.cb = strm_cmaf_lib_write;
	io.write.param = NULL;
//	printf("%s pos(%lld), max(%d).\n", __func__, pos, max);
	TRACE((T_DEBUG, "[%llu] %s st_size = %lld , cursor[%lld], try_read=%ld, remained=%ld\n",
			req->id, __func__, req->objstat.st_size,
			(long long)req->cursor, max, req->remained ));
	if (req->objstat.st_sizeknown == 1) {
		/* 완성된 fragment를 요청한 경우 */
		sendto = min(max, req->remained);
		write_param.build_type = strm_CMAF_build_fragment;
		write_param.outbuf = buf;
		write_param.outbufsize = sendto;
		write_param.outbufpos = 0;
		io.write.param = (void *)&write_param;
		ts = sx_get_time();
		ret = solCMAF_build_fragment(&io, cmaf->ctx, streaming->fragment, req->cursor, sendto);
		te = sx_get_time();
		req->t_zipper_build += (te - ts);
	}
	else {
		/* 만들어지고 있는 fragment를 요청한 경우 */
		if(solCMAF_isCompleted_fragment(streaming->fragment) == YES) {
			fragment_pos  = solCMAF_sizeOf_fragment(streaming->fragment);
			/* fragment가 전송 도중 완성된 경우 */
			if (req->cursor >= fragment_pos) {
				/* 전송이 모두 끝난 상태 */
				if ((streaming->protocol & O_PROTOCOL_TUNNEL) != 0) {
					/* tunneling 인 경우에는 fragment를 모두 전송하면 다음 fragment를 가져와서 계속 전송한다. */
					last_fragment = streaming->fragment;
					streaming->fragment = solCMAF_nextOf_tunnel(cmaf->ctx, last_fragment);
					if (streaming->fragment == NULL) {
						/* 무슨 이유에서인지 다음 fragment를 받아 올수 없는 경우는 연결을 종료 시킨다. */
						ret = MHD_CONTENT_READER_END_OF_STREAM;
					}
					else {
						solCMAF_release_fragment(last_fragment);
						ret = 0;
						req->cursor = 0;
					}
				}
				else {
					ret = MHD_CONTENT_READER_END_OF_STREAM;
				}
			}
			else {
				remain = fragment_pos - req->cursor;
				sendto = min(max, remain);
				write_param.build_type = strm_CMAF_build_fragment;
				write_param.outbuf = buf;
				write_param.outbufsize = sendto;
				write_param.outbufpos = 0;
				io.write.param = (void *)&write_param;
				ts = sx_get_time();
				ret = solCMAF_build_fragment(&io, cmaf->ctx, streaming->fragment, req->cursor, sendto);
				te = sx_get_time();
				req->t_zipper_build += (te - ts);
			}
		}
		else {
			/* fragment가 생성중인 경우 */
			fragment_pos  = solCMAF_sizeOf_fragment(streaming->fragment);
			if (req->cursor >= fragment_pos) {
				/*
				 * 아직 전송에 필요한 chunk가 생성이 되지 않았음
				 * suspend를 한후 list에 session 정보를 저장하고 chunk가 생성될때까지 기다려야 한다.
				 */
				copied = 0;

				if (gscx__enable_async) {
//					track = atoi(streaming->rep_id);
//					pthread_mutex_lock(cmaf->plock);
//					iList.Add(cmaf->suspend_list[track], (void *)&req);
					if(req->is_suspeneded == 1) {
						/* suspend 상태로 strm_cmaf_reader()가 호출 되었기 때문에 suspend를 다시 호출할 필요가 없음 */
						req->stay_suspend = 1;	/* scx_run_async_job()에서 resume가 되지 않도록 설정한다. */
					}
					else {
						req->is_suspeneded = 1;
						/* job thread의 대기 큐가 다 차서 suspend가 아닌 상태 scx_handle_async_method() 에서 직접 호출 되는 경우 이기 때문에 suspend를 해줘야 한다. */
						MHD_suspend_connection(req->connection);
						strm_cmaf_add_supend_list(req);

					}
//					pthread_mutex_unlock(cmaf->plock);
//					printf("cmaf suspend track(%d), pos = %d\n", track, req->cursor);
				}
				else {
					/* 비동기 지원이 되지 않는 경우는 무한 호출을 회피하기 위해서 잠깐 sleep 후 리턴한다. */
					TRACE((T_WARN, "[%llu] CMAF Live is required async mode\n", req->id));
					bt_msleep(1);

				}

			}
			else {
				/* 전송에 필요한 chunk가 생성된 상태 */
				remain = fragment_pos - req->cursor;
				sendto = min(max, remain);
				write_param.build_type = strm_CMAF_build_fragment;
				write_param.outbuf = buf;
				write_param.outbufsize = sendto;
				write_param.outbufpos = 0;
				io.write.param = (void *)&write_param;
				ts = sx_get_time();
				ret = solCMAF_build_fragment(&io, cmaf->ctx, streaming->fragment, req->cursor, sendto);
				te = sx_get_time();
				req->t_zipper_build += (te - ts);
		//		printf("track = %s, seq = %d, pos = %d, sednto = %d, max = %d\n", streaming->rep_id, streaming->ts_num,req->cursor, sendto, max);
			}
		}

	}
	if (ret != MHD_CONTENT_READER_END_OF_STREAM) {
		copied = ret;
		req->cursor	+= copied;
		if (req->remained > 0)
			req->remained 	-= copied;

	}
	return copied;
}

int
strm_cmaf_add_supend_list(nc_request_t *req)
{
	int		track_id;
	int 	ret = 0;
	streaming_t		*streaming = req->streaming;
	service_info_t 	*service = req->service;
	service_cmaf_t 	*cmaf = service->core->cmaf;

	track_id = streaming->rep_num;
	pthread_mutex_lock(cmaf->plock[track_id]);
	iList.Add(cmaf->suspend_list[track_id], (void *)&req);
	pthread_mutex_unlock(cmaf->plock[track_id]);
	TRACE((T_DEBUG, "cmaf suspend track(%d), pos = %d\n", track_id, req->cursor));
	return ret;
}
//////////////////////////////////////////////////////////////// CMAF Live 기능 End //////////////////////////////////////////////////////////////

void
strm_destroy_builder(builder_info_t * builder)
{
	int i;
	media_info_t *media;
	zipper_io_handle  *ioh = NULL;
	media_info_list_t * media_list = builder->media_list;
	media_info_list_t * subtitle_list = builder->subtitle_list;
	mem_pool_t 		*mpool = NULL;
	if (builder->bcontext) {
		ioh = strm_set_io_handle(NULL);
		zipper_free_builder_context(ioh,&builder->bcontext);
		builder->bcontext = NULL;
		strm_destroy_io_handle(ioh);
	}

	/* 세그먼트에 사용된 media 정보들을 찾아서 모두 release해 준다. */
	while (media_list) {
		media = media_list->media;
		if (media) {
			strm_release_media(media);
			media_list->media = NULL;
		}
//		strm_unuse_media(media);
		media_list = media_list->next;
	}
	builder->media_list = NULL;

	while (subtitle_list) {
		media = subtitle_list->media;
		if (media) {
			strm_release_media(media);
			subtitle_list->media = NULL;
		}
		subtitle_list = subtitle_list->next;
	}
	builder->subtitle_list = NULL;

//	pthread_mutex_destroy(&builder->lock);
	if (builder->mpool) {
		mpool = builder->mpool;
		builder->mpool = NULL;
		mp_free(mpool);
	}
}

/* builder에 있는 모든 media를 TTL을 expire 시킨다 */
void
strm_expire_medialist(nc_request_t *req)
{
	media_info_t 	*media = NULL;
	media_info_list_t *cur_list = NULL;
	time_t			expiredtime = scx_get_cached_time_sec()-1;
	cur_list = req->streaming->builder->media_list;
	while (cur_list) {
		//if (media == NULL) break;
		media = cur_list->media;
		if (media) {
			media->vtime = expiredtime;
		}
		cur_list = cur_list->next;
	}
}

/*
 * value가 NULL인 key를 찾아서 nokey_var에 넣어 준다.
 * nokey_var에 이미 값이 들어 있는 경우는 skip 한다.
 */
static int
strm_query_null_value_iterator(void *cr, enum MHD_ValueKind kind, const char *key, const char *value)
{
	char **nokey_var = (char **)cr;
	if (value == NULL && *nokey_var == NULL) {
		*nokey_var =  key;
	}
	return MHD_YES;
}

/*
 * client 로 부터 들어온 url에 포함된 query parameter중 value가 없이 들어온 key를 찾아서 리턴한다.
 * 예) /stream/playlist.m3u8?DVR&token=39djo3la73 에서 DVR을 리턴
 * value가 없는 key가 여러개인 경우 첫 key가 선택된다.
 * MHD_lookup_connection_value()는 value가 있는 경우와 key=의 형태는 찾을수 있지만 key만 있는 경우는 찾을수 없어서
 * key만 있는 경우를 찾기 위해 별도로 분리했음(와우자의 경우 타임시프트 기능을 사용할때 DVR key만 사용한다.)
 */
static const char*
strm_find_query_null_value(nc_request_t *req)
{
	int result;
	char *nokey_var = NULL;	// key가 없이 value만 있는 값을 저장

	result = MHD_get_connection_values(req->connection, MHD_GET_ARGUMENT_KIND, strm_query_null_value_iterator, &nokey_var);

	return nokey_var;
}
/*
 * HLS manifest 수정 기능 사용시 호출됨
 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/32679
 */
int
strm_modify_hls_manifest(nc_request_t *req)
{
	zipper_io_handle ioh;
	streaming_t	*streaming = req->streaming;
	int	ret = 1;
	int zres = 0;
	void *file = NULL;
	char *open_path = vs_data(req->ori_url);
	ssize_t need_size = 0;
	zipperBldr bldr = NULL;
	m3u8_format_template_stat ftstat;
	strm_live_io_ctx_t io_ctx;


	memset(&ftstat, 0, sizeof(ftstat));
	memset(&ioh, 0, sizeof(ioh));
	memset(&io_ctx, 0, sizeof(io_ctx));
	memset(&io_ctx.objstat, 0, sizeof(nc_stat_t));
	strm_set_io_memfunc(&ioh);
	io_ctx.req = req;
	ioh.reader.readfp = strm_live_read_func;
	ioh.reader.sizefp = strm_live_size_func;
	ioh.reader.param = (void *)&io_ctx;

	/* 아래 두 라인 필요 없어 보임, strm_live_write_func()도 필요 없을듯 */
	ioh.writer.fp = strm_live_write_func;
	ioh.writer.param = (void *)&io_ctx;
	/* 오리진에 요청되는 manifest 요청 경로에 지정된 query를 추가 하는 부분 */
	if (req->service->additional_manifest_cache_key_list != NULL) {
		int list_cnt = 0;
		int	i;
		char *key = NULL;
		const char *var = NULL;
		const char *nokey_var = NULL;	// key가 없이 value만 있는 값을 저장
		int	arg_cnt = 0;
		int alloc_size = vs_length(req->url)+1; //오리진에 요청되는 경로가 client의 요청 경로 보다 길지 않기 때문에 이렇게 처리 한다.
		int	write_pos = 0;
		scx_list_t *list_root = NULL;

		open_path = alloca(alloc_size);
		write_pos = scx_snprintf(open_path, alloc_size, "%s", vs_data(req->ori_url));

		list_root = req->service->additional_manifest_cache_key_list;
		list_cnt = scx_list_get_size(list_root);
		// key가 없이 value만 있는 경우(예:/stream/playlist.m3u8?DVR에서 DVR)의 key값을 찾는다.
		nokey_var = strm_find_query_null_value(req);
		for(i = 0; i < list_cnt; i++) {
			key = scx_list_get_by_index_key(list_root,i);
			if (nokey_var != NULL) {
				if (strcmp(nokey_var, key) == 0)  {
				/* key가 없이 value만 있을때 해당 value가 지정된 key와 같은 경우 */
					write_pos += scx_snprintf(open_path+write_pos, alloc_size-write_pos,
							"%c%s",
							(arg_cnt++ == 0)?'?':'&', nokey_var);	//argument가 처음인 경우 ?를 추가하고 이후는 &를 추가한다.
					continue;

				}
			}
			var = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, key);
			if (var) {
				write_pos += scx_snprintf(open_path+write_pos, alloc_size-write_pos,
						"%c%s=%s",
						(arg_cnt++ == 0)?'?':'&', key, var);	//argument가 처음인 경우 ?를 추가하고 이후는 &를 추가한다.

			}

		}
	}
	/* manifest 파일은 full GET으로 읽는다. */
	io_ctx.file = strm_file_open(req, O_CONTENT_TYPE_LIVE, open_path, &io_ctx.objstat, O_RDONLY|O_NCX_NORANDOM);
	if (io_ctx.file == NULL){
		scx_error_log(req, "URL[%s] - open error (%d)\n", open_path, strm_file_errno(req));
		req->p1_error = MHD_HTTP_NOT_FOUND;
		ret = 0;
		goto strm_modify_hls_manifest_end;
	}
	if (io_ctx.objstat.st_size > 5120000) {
		/* manifest 수정 가능 원본 파일 최대 크기는 5MB로 한다. */
		scx_error_log(req, "URL[%s] - manifest size (%lld) too big\n", open_path, io_ctx.objstat.st_size);
		req->p1_error = MHD_HTTP_UNSUPPORTED_MEDIA_TYPE;
		ret = 0;
		goto strm_modify_hls_manifest_end;
	}

	/* HLS manifest의 크기가 너무 작은 경우도 에러로 처리한다. */
	if (io_ctx.objstat.st_size  < 20) {
		scx_error_log(req, "URL[%s] - manifest size(%lld) too small.\n", open_path, io_ctx.objstat.st_size );
		req->p1_error = MHD_HTTP_UNSUPPORTED_MEDIA_TYPE;
		ret = 0;
		goto strm_modify_hls_manifest_end;
	}
	/* media_type이 MEDIA_TYPE_HLS_MAIN_M3U8나 MEDIA_TYPE_HLS_SUB_M3U8 인경우 */
	/* 원본의 manifest 응답에 session을 달아서 내보낸다 */
	/* 원본 파일을 모두 읽은 후에 strm_live_edit_hls_manifest_session()을 호출 하면 될듯 */

	ssize_t 			copied = 0;

	ftstat.inbuf = SCX_CALLOC(1, io_ctx.objstat.st_size+1);

	copied = strm_file_read(req, io_ctx.file, ftstat.inbuf, 0, io_ctx.objstat.st_size);
	if (copied != io_ctx.objstat.st_size) {
		scx_error_log(req, "URL[%s] read error (%d)\n", open_path, strm_file_errno(req));
		req->p1_error = MHD_HTTP_NOT_FOUND;
		ret = 0;
		goto strm_modify_hls_manifest_end;
	}

	/* body가 HLS manifest 형태인지 확인 한다. */
	if (strncasecmp(ftstat.inbuf, "#EXTM3U", 7) != 0 )
	{
		scx_error_log(req, "URL[%s] - not supported manifest format\n", open_path);
		req->p1_error = MHD_HTTP_UNSUPPORTED_MEDIA_TYPE;
		ret = 0;
		goto strm_modify_hls_manifest_end;
	}

	ret = strm_live_edit_hls_manifest_session(req, &ioh, &ftstat);
	if (ret != zipper_err_success) {
		scx_error_log(req, "URL[%s] modify manifest failed.(%s)\n", open_path, zipper_err_msg(ret));
		req->p1_error = MHD_HTTP_NOT_FOUND;
		ret = 0;
		goto strm_modify_hls_manifest_end;
	}
	req->scx_res.body = ftstat.outbuf;
	ftstat.outbuf = NULL; /* 함수의 끝에서 memory 해제를 방지하기 위해 NULL로 설정 */

#if 0
	streaming->media_size = strlen(req->scx_res.body);
#else
	streaming->media_size = ftstat.outbuflen;
#endif
	req->objstat.st_mtime = io_ctx.objstat.st_mtime;
	strm_set_objstat(req);
	ret = 1;
strm_modify_hls_manifest_end:
	if (ftstat.inbuf != NULL) {
		SCX_FREE(ftstat.inbuf);
		ftstat.inbuf = NULL;
	}
	if (ftstat.outbuf != NULL) {
		SCX_FREE(ftstat.outbuf);
		ftstat.outbuf = NULL;
	}
	if (ftstat.tmpbuf != NULL) {
		SCX_FREE(ftstat.tmpbuf);
		ftstat.tmpbuf = NULL;
	}
	if (io_ctx.file != NULL) {
		strm_file_close(req, io_ctx.file);
	}
	return ret;
}

/*
 * HLS manifest 수정 기능 사용시 호출됨
 * session ID를 추가후에 redirect 응답을 한다.
 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/32679
 */
int
strm_redirect_manifest(nc_request_t *req)
{
	streaming_t	*streaming = req->streaming;
	int	ret = 1;
	vstring_t 		*v_token = NULL;
	int				path_len = 0;	//query parameter의 시작 부분의 offset. ? 포함
	int	alloc_size = 0;
	int pos = 0;

	/* 세션 정보를 argument 추가한다. */
	strm_update_session_argument(req);

	/* url에 들어 있는 path의 길이를 알아낸다. */
	path_len = vs_pickup_token_r(NULL, req->ori_url, vs_length(req->ori_url), "?", &v_token, DIRECTION_RIGHT);
	if (path_len <= 0) {
		/* url에 query가 없는 경우 */
		path_len = vs_length(req->ori_url);
	}
	else {
		path_len + 1;
	}
#if 0
	/* 302 redirect 방식은 지원하지 않는 플레이어들이 많아서 사용 불가로 판단 */
	char *redirect_path = NULL;

	if (req->service->hls_playlist_host_domain == NULL) {
		/* hls_playlist_host_domain 설정이 없는 경우에는 404를 응답한다. */
		TRACE((T_WARN, "[%llu] URL[%s] Failed to redirect URL.(reason : hls_playlist_host_domain not defined.)\n", req->id, vs_data(req->ori_url)));
		ret = nce_handle_error(MHD_HTTP_NOT_FOUND, req);
		return 0;
	}
	ASSERT(0 < streaming->real_path_len);
	alloc_size = path_len + 10;
	alloc_size += vs_length(req->service->hls_playlist_host_domain) + 10; /* 'scheme://domain:port' 의 길이 */
	if (streaming->argument) {
		alloc_size += strlen(streaming->argument);
	}

	redirect_path = (void *)SCX_CALLOC(1, alloc_size);
	pos = snprintf(redirect_path, alloc_size, "%s://%s",
				req->hls_playlist_protocol?"https":"http", vs_data(req->service->hls_playlist_host_domain));
	pos += snprintf(redirect_path + pos, path_len+1, "%s", vs_data(req->ori_url));
	if (streaming->argument) {
		/* streaming->argument에는 이미 ?가 포함 되어 있기 때문에 따로 ?를 추가할 필요가 없다. */
		pos += sprintf(redirect_path + pos,  "%s", streaming->argument);
	}

	nce_create_response_from_buffer(req, 0, NULL);

	MHD_add_response_header(req->response, MHD_HTTP_HEADER_LOCATION, redirect_path);

	if (redirect_path) {
		SCX_FREE(redirect_path);
		redirect_path = NULL;
	}

	if (req->response)
		nce_add_basic_header(req, req->response);
	/* 일부 플레이어에서 CORS 헤더가 없으면 플레이를 하지 않는 현상이 있어서 redirect 시에도 CORS 헤더를 포함해서 응답해야한다. */
	strm_add_streaming_header(req);
	req->resultcode = MHD_HTTP_FOUND;
	ret = scx_finish_response(req);
#else
	if (req->scx_res.body != NULL) {
		SCX_FREE(req->scx_res.body);
		req->scx_res.body = NULL;
	}
	ASSERT(0 < streaming->real_path_len);
	alloc_size = path_len + 100;
	if (req->service->hls_playlist_host_domain) {
		alloc_size += vs_length(req->service->hls_playlist_host_domain); /* 'scheme://domain:port' 의 길이 */
	}


	if (streaming->argument) {
		alloc_size += strlen(streaming->argument);
	}
	req->scx_res.body = (void *)SCX_CALLOC(1, alloc_size);

	/* BANDWIDTH 항목이 빠질 경우 아이폰에서 재생이 되지 않는다. */
	snprintf(req->scx_res.body, 4096, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-STREAM-INF:BANDWIDTH=102400\n");
	pos = strlen(req->scx_res.body);
	if (req->service->hls_media_playlist_use_absolute_path || req->service->hls_master_playlist_use_absolute_path) {
		/* 정적 URL 사용시 */
		if (req->service->hls_playlist_host_domain) {
			snprintf(req->scx_res.body+pos, alloc_size,
					"%s://%s",
					req->hls_playlist_protocol?"https":"http", vs_data(req->service->hls_playlist_host_domain));
			pos = strlen(req->scx_res.body);
		}
		if (req->service->manifest_prefix_path) {
			snprintf(req->scx_res.body+pos, alloc_size-pos,
					"/%s", vs_data(req->service->manifest_prefix_path));
			pos = strlen(req->scx_res.body);
		}
		if (req->app_inst != NULL) {
			snprintf(req->scx_res.body+pos, alloc_size-pos,
					"/%s", req->app_inst);
			pos = strlen(req->scx_res.body);
		}
		snprintf(req->scx_res.body + pos, streaming->real_path_len+1, "%s/", vs_data(req->ori_url));
		pos = strlen(req->scx_res.body);
	}

	snprintf(req->scx_res.body + pos, path_len - streaming->real_path_len+1, "%s", vs_data(req->ori_url)+streaming->real_path_len);
	pos = strlen(req->scx_res.body);

	if (streaming->argument) {
		/* streaming->argument에는 이미 ?가 포함 되어 있기 때문에 따로 ?를 추가할 필요가 없다. */
		sprintf(req->scx_res.body + pos,  "%s", streaming->argument);
		pos = strlen(req->scx_res.body);
	}
#if 1
	streaming->media_size = pos;
	req->resultcode = MHD_HTTP_OK;
	strm_set_objstat(req);

#else
	nce_create_response_from_buffer(req, strlen(req->scx_res.body), req->scx_res.body);

	if (req->response)
		nce_add_basic_header(req, req->response);
	/* 일부 플레이어에서 CORS 헤더가 없으면 플레이를 하지 않는 현상이 있어서 redirect 시에도 CORS 헤더를 포함해서 응답해야한다. */
	strm_add_streaming_header(req);
	req->resultcode = MHD_HTTP_OK;
	ret = scx_finish_response(req);
#endif
#endif
	return 1;
}


/*
 * HLS manifest body를 gzip으로 압축한다.
 */
void
strm_compress_manifest(nc_request_t *req)
{
	char *content_encoding = NULL;
	int out_buff_size = 0;
	void *out_buff = NULL;
	int gzip_size = 0;


	if (req->service->hls_manifest_compress_enable != 1) {
		return;
	}
	if (req->streaming->media_type != MEDIA_TYPE_MODIFY_HLS_MANIFEST) {
		return;
	}
	// body가 10KB 이상인 경우만 압축 하도록 한다
	if (req->streaming->media_size < 10240) {
		return;
	}
	// range GET 요청에 대해서는 압축을 하지 않는다.
	if (req->subrange == 1) {
		return;
	}
	/* request의  Accept-Encoding header에  gzip이 있는 경우만 압축 한다. */
	content_encoding = (char *)MHD_lookup_connection_value(req->connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_ACCEPT_ENCODING);
	if(content_encoding == NULL) {
		return;
	}
	if (strcasestr(content_encoding, "gzip") == NULL) {
		return;
	}

	out_buff_size = req->streaming->media_size / 10;
	if (out_buff_size < 16384) {	/* manifest의 크기가 작으면 압축 효율이 떨어 질수 있어서 이렇게 할당 한다. */
		out_buff_size = 16384;
	}
	out_buff = SCX_MALLOC(out_buff_size);

	if  (sigsetjmp(__thread_jmp_buf, 1) == 0) {
		__thread_jmp_working = 1;
		gzip_size = scx_compress_gzip(req, (unsigned char *)req->scx_res.body, req->streaming->media_size, out_buff, out_buff_size);
		__thread_jmp_working = 0;
	}
	else {
		/* 압축 과정에서 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
		gzip_size = 0;
		TRACE((T_ERROR, "[%llu] scx_compress_gzip() SIGSEGV occured. URL(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
	}
	if (gzip_size > 0) {
		SCX_FREE(req->scx_res.body);
		req->scx_res.body = out_buff;
		req->streaming->media_size = gzip_size;
		req->resp_body_compressed = 1;
	}
	return ;
}


/*
 * mp4를 제외한 menifest,index,crossdomain,ts,m4s 등은
 * strm_prepare_stream()에서  html body 데이터를 미리 생성해서 buffer에 넣어놓고
 * streaming_reader()에서는 buffer를 읽는 역할을 한다.
 * mp4의 경우는 strm_prepare_stream()에서는 전송될 크기만 구하고
 * streaming_reader()에서 실제 미디어 데이터를 생성한다.
 */
int
strm_prepare_stream(nc_request_t *req)
{
	mem_pool_t 		*mpool = NULL;
	zipper_io_handle  *ioh = NULL;
	streaming_t 	*streaming = req->streaming;
	service_info_t 	*service = req->service;
	builder_info_t 	*builder = NULL;
	zipperRngGet rgCtx = NULL;
	zipper_builder_param *bprm = streaming->bprm;
	double 					ts, te;
	int			i;
	int ret = 0, pos = 0;
	buffer_paramt_t		buffer_param = {req, NULL, 0, 0, 1};
	int			track_id = 0;
	uint32_t  segment_number = 0;

	char *argument = NULL;
	int	argument_len = 0;
	int format_len = 0;
	char * hls_format = NULL;
	int	 hls_format_len = 0;
	char * hls_init_format = NULL;
	int	 hls_init_format_len = 0;
	char * hls_audio_format = NULL;
	char * hls_video_format = NULL;
	char * hls_subtitle_format = NULL;
	int	 hls_audio_format_len = 0;
	int	 hls_video_format_len = 0;
	int	 hls_subtitle_format_len = 0;
	mpd_builder_format *dash_format  = NULL;
	iis_builder_format *mss_format  = NULL;
	char *dash_video_init_format = NULL;
	char *dash_video_seq_format = NULL;
	char *dash_audio_init_format = NULL;
	char *dash_audio_seq_format = NULL;
	source_info_t *source = NULL;
	char *abs_path = NULL;
	int abs_path_len = 0;
	char *key_url = NULL;
	char string_buf[20] = {'\0'};
	zipper_builder_encrypt      *encrypt = NULL;
	int 	max_body_len = 0;
	int		track_cnt = 0;
	char *hostdomain = NULL;
	subtitle_info_t *subtitle = NULL;
	int array_pos = 0;
	char * subtitle_url = NULL;
	int subtitle_url_size = 0;

	memset(&req->objstat, 0, sizeof(nc_stat_t));

	if (MEDIA_TYPE_CROSSDOMAIN == streaming->media_type) {
		/*
		 *  Adobe Flash 기반 플레이어의 경우 http://....../crossdomain.xml 에 대해서 요청이 오게 되며,
		 *  이경우 원본하고 상관없이 아래의 내용을 리턴해야함
		<?xml version="1.0"?>
		<!DOCTYPE cross-domain-policy SYSTEM "http://www.adobe.com/xml/dtds/cross-domain-policy.dtd">
		<cross-domain-policy>
		    <allow-access-from domain="*" />
		    <site-control permitted-cross-domain-policies="all"/>
		</cross-domain-policy>
		 *
		 */
		/* 이부분은 매번 만들필요 없는데 일단 이렇게 하고 나중에 초기화시 한번만 만들도록 수정 해야 한다. */
		req->scx_res.body = (void *)SCX_CALLOC(1, 512);
		ret = scx_snprintf(req->scx_res.body, 512,
				"<?xml version=\"1.0\"?>\n"
				"<!DOCTYPE cross-domain-policy SYSTEM \"http://www.adobe.com/xml/dtds/cross-domain-policy.dtd\">\n"
				"<cross-domain-policy>\n"
				"\t<allow-access-from domain=\"*\" secure=\"false\"/>\n"
				"\t<site-control permitted-cross-domain-policies=\"all\"/>\n"
				"</cross-domain-policy>\n");
		streaming->media_size = ret;
	}
	else if (MEDIA_TYPE_CLIENTACCESSPOLICY == streaming->media_type) {
		req->scx_res.body = (void *)SCX_CALLOC(1, 512);
		ret = scx_snprintf(req->scx_res.body, 512,
				"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
				"<access-policy>\n"
				"\t<cross-domain-access>\n"
				"\t\t<policy>\n"
				"\t\t\t<allow-from http-request-headers=\"*\">\n"
				"\t\t\t\t<domain uri=\"*\"/>\n"
				"\t\t\t</allow-from>\n"
				"\t\t\t<grant-to>\n"
				"\t\t\t\t<resource path=\"/\" include-subpaths=\"true\"/>\n"
				"\t\t\t</grant-to>\n"
				"\t\t</policy>\n"
				"\t</cross-domain-access>\n"
				"</access-policy>\n");
		streaming->media_size = ret;
	}
	else if (MEDIA_TYPE_FCS_IDENT == streaming->media_type) {
		req->scx_res.body = (void *)SCX_CALLOC(1, 512);
		ret = scx_snprintf(req->scx_res.body, 512,
				"<html><head><title>%s %s</title></head><body>%s %s</body></html>\n", PROG_NAME, PROG_VERSION, PROG_NAME, PROG_VERSION);
		streaming->media_size = ret;
	}
	else if (MEDIA_TYPE_ENC_KEY == streaming->media_type) {
		req->scx_res.body = (void *)SCX_CALLOC(1, 16);
		if (ENCRYPT_METHOD_NONE < req->service->hls_encrypt_method) {
			strm_make_enc_key(req, req->scx_res.body);
			streaming->media_size = 16;
		}
		else {
			scx_error_log(req, "Host(%s), URL[%s] - encryption key not configured.\n", vs_data(req->service->name), vs_data(req->url));
			req->p1_error = MHD_HTTP_NOT_FOUND;
			goto send_stream_error;
		}
	}
	else if (MEDIA_TYPE_MODIFY_HLS_MANIFEST == streaming->media_type) {
		if (service->hls_modify_manifest_redirect_enable == 1 && streaming->session_id == 0) {
			/* 요청에 session_id가 없는 경우 redirect 대상이 된다. */
			ret = strm_redirect_manifest(req);
		}
		else {
			if (service->hls_modify_manifest_ttl > 0) {
				/*
				 * TTL 이 지정된 경우에는 ttl 값을 바꿔준다.
				 * TTL 이 지정되지 않은 경우에는 오리진의 max-age 헤더 값을 사용한다.
				 */
				kv_set_pttl(streaming->options, service->hls_modify_manifest_ttl);
			}
			ret = strm_modify_hls_manifest(req);
		}
		return ret;
	}
	else if ( (streaming->media_mode & O_STRM_TYPE_DIRECT) != 0 ) {
		streaming->source = strm_create_source_info(req, O_CONTENT_TYPE_VOD, streaming->media_path, NULL);
		if (streaming->source == NULL) {
			scx_error_log(req, "Host(%s), URL[%s] - open error (%d)\n", vs_data(req->service->name), streaming->media_path, strm_file_errno(req));
			req->p1_error = MHD_HTTP_NOT_FOUND;
			goto send_stream_error;
		}
		memcpy(&req->objstat, &streaming->source->objstat, sizeof(nc_stat_t));
		streaming->media_size = req->objstat.st_size;
	}
	else {
		builder = streaming->builder;
		ASSERT(streaming->builder);
		/* 일단은 매번 할당 하지만 session 기능 사용시는 세션 생성시 처음 한번만 할당 하면 될듯
		  	  이 경우 ioh->reader.param만 매번 할당 하면 된다. */
		ioh = strm_set_io_handle(streaming);
		if (!ioh) {
			goto send_stream_error;
		}
		if (builder->is_adaptive == 1) {
			track_id = strm_get_track_id(req);
		}
		/* 세션 정보를 argument 추가한다. */
		strm_update_session_argument(req);

		memset(bprm, 0, sizeof(zipper_builder_param));

		bprm->attr.bldtype = streaming->build_type;
		switch(streaming->media_type) {
		case MEDIA_TYPE_HLS_MAIN_M3U8:
		case MEDIA_TYPE_HLS_AUDIOLIST:
		case MEDIA_TYPE_HLS_SUB_M3U8:
		case MEDIA_TYPE_HLS_VTT_M3U8:
		case MEDIA_TYPE_HLS_TS:
		case MEDIA_TYPE_HLS_VTT:
			hls_format = (char *) mp_alloc(streaming->mpool, MEDIA_FORMAT_MAX_LEN);
			ASSERT(hls_format);
			break;
		case MEDIA_TYPE_HLS_FMP4_MAIN_M3U8:
			hls_video_format = (char *) mp_alloc(streaming->mpool, MEDIA_FORMAT_MAX_LEN);
			hls_audio_format = (char *) mp_alloc(streaming->mpool, MEDIA_FORMAT_MAX_LEN);
			ASSERT(hls_video_format);
			ASSERT(hls_audio_format);
			if (streaming->subtitle_count > 0) {
				hls_subtitle_format = (char *) mp_alloc(streaming->mpool, MEDIA_FORMAT_MAX_LEN);
				ASSERT(hls_subtitle_format);
			}
			break;
		case MEDIA_TYPE_HLS_FMP4_AUDIO_M3U8:
		case MEDIA_TYPE_HLS_FMP4_VIDEO_M3U8:
			hls_format = (char *) mp_alloc(streaming->mpool, MEDIA_FORMAT_MAX_LEN);
			hls_init_format = (char *) mp_alloc(streaming->mpool, MEDIA_FORMAT_MAX_LEN);
			ASSERT(hls_format);
			ASSERT(hls_init_format);
			break;
		case MEDIA_TYPE_MPEG_DASH_MANIFEST:
		case MEDIA_TYPE_MPEG_DASH_TS_MANIFEST:
		case MEDIA_TYPE_MPEG_DASH_YT_MANIFEST:
			dash_format = (mpd_builder_format *) mp_alloc(streaming->mpool, sizeof(mpd_builder_format));
			ASSERT(dash_format);
			break;
		case MEDIA_TYPE_MSS_MANIFEST:
		case MEDIA_TYPE_MSS_VIDEO:
		case MEDIA_TYPE_MSS_AUDIO:
			mss_format = (iis_builder_format *) mp_alloc(streaming->mpool, sizeof(iis_builder_format));
			ASSERT(mss_format);
			break;
		default:
			break;
		}


		streaming->ioh = ioh; /* strm_clean_stream에서 해제를 위해 */
		/* MEDIA_TYPE_MP3와 MEDIA_TYPE_MP4를 제외한 다른 media type들은 모두 여기서 컨텐츠를 생성한다. */
		switch(streaming->media_type) {
		case MEDIA_TYPE_MP3:
		case MEDIA_TYPE_MP4:
		case MEDIA_TYPE_M4A:
		case MEDIA_TYPE_M4V:
		case MEDIA_TYPE_FLV:
		case MEDIA_TYPE_FLAC:
			ret = zipper_create_rangeget_context(ioh, &rgCtx);
			streaming->rgCtx = rgCtx;

			if (streaming->media_type == MEDIA_TYPE_MP3) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_AUDIO | BLDFLAG_CAL_SIZE;
				if (req->service->id3_retain) {
					bprm->target.attr.mp3.id3 = 1;
					bprm->target.attr.mp3.id3media = NULL;
				}
			}
			else if (streaming->media_type == MEDIA_TYPE_FLAC) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_AUDIO | BLDFLAG_CAL_SIZE;
				if (req->service->id3_retain) {
					bprm->target.attr.flac.id3 = 1;		/* 1이면 ID3 정보를 유지, https://jarvis.solbox.com/redmine/issues/30517 참고 */
					bprm->target.attr.flac.id3media = NULL;	/* ID3 정보를 유지할 미디어의 context */
				}
			}
			else if (streaming->media_type == MEDIA_TYPE_M4A) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_AUDIO | BLDFLAG_CAL_SIZE;
			}
			else if (streaming->media_type == MEDIA_TYPE_M4V) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_VIDEO | BLDFLAG_CAL_SIZE;
			}
			else {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
			}
			bprm->attr.index.adapt = track_id;
			bprm->target.range_get.ctx = rgCtx;
			if (MEDIA_TYPE_FLV == streaming->media_type) {
				/*
				 * include_keyframe은 FLV의 처음 위치 요청(start가 0)에 대해서만 1로 하고
				 * 이후의 요청에 대해서는 0으로 해야 한다.
				 * pseudo streaming으로 동작 할때 1로 주게 되면 플레이어서 재생 시간 오류가 발생할 가능성이 있음
				 * pseudo_start는 시작 timestamp를 설정 해준다.
				 * */

				if (streaming->content) {
					if (streaming->content->start == 0) {
						bprm->target.attr.flv.include_keyframes = 1;
						//bprm->target.attr.flv.pseudo_start = 0;
						bprm->basepts = 0;
					}
					else {
						bprm->target.attr.flv.include_keyframes = 0;
						bprm->basepts = streaming->content->start * 90; /* msec 단위를 1/90000초 단위로 변경한다. */
						//bprm->basepts = streaming->content->start * 9 / 100; /* msec 단위를 1/90초 단위로 변경한다. */
						//bprm->target.attr.flv.pseudo_start = streaming->content->start;
					}
				}
				else { /* streaming->content 가 NULL인 경우는 builder context가 캐싱된 상태임
				 	 	 이 경우는 start가 0이다.*/
					bprm->target.attr.flv.include_keyframes = 1;
				}
				bprm->target.attr.flv.fpos_tscale = 1000000;
				bprm->target.attr.flv.eos_is_key = 1;
			}

			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, streaming->builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			req->t_zipper_build += (te - ts);

			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;

			break;
		case MEDIA_TYPE_HLS_MAIN_M3U8:
			if ( (streaming->protocol & O_PROTOCOL_SEP_TRACK) != 0) {
				zipperVariant v = NULL;
				zipper_variant_track vt;
				zipper_variant_build_param vbp;	


				if((ret = zipper_create_variant_context(ioh, &v)) != zipper_err_success) {
					scx_error_log(req, "'%s' couldn't create zipper variant context, code=%d(%s)\n", vs_data(req->url), ret, zipper_err_msg(ret));
					goto zipper_build_error;
				}		
				memset(&vt, 0, sizeof(vt));		
				//첫번째 MP4 HLS(video TS) 어뎁티브 스트림 추가 (zipperCnt 컨텍스트 사용 안하고 직접 입력)
				vt.media.bandwidth = 4000000; // 4Mbps
				vt.media.resolution.width = 1920;
				vt.media.resolution.height = 1080;
				vt.media.codecs = "avc1,mp4a.40.2";
				vt.url = "[첫번째 MP4 HLS(video TS) 어뎁티브 스트림 URL]";
				vt.avflag = variant_track_video;
//				vt.group.audio = "track1";

				if((ret = zipper_variant_add_track(ioh, v, &vt)) != zipper_err_success) {
					scx_error_log(req, "'%s' couldn't add track to zipper variant context, code=%d(%s)\n", vs_data(req->url), ret, zipper_err_msg(ret));
					goto zipper_build_error;
				}	


				// 최종 매니패스트 출력
				memset(&vbp, 0, sizeof(vbp));

				vbp.format = BLDTYPE_M3U8;
				vbp.m3u8.ver = 3;
				vbp.flag = BLDFLAG_CAL_SIZE;

				if((ret = zipper_variant_build(ioh, v, &vbp)) != zipper_err_success ||	vbp.output.written == 0) {
					scx_error_log(req, "'%s' couldn't build zipper variant context, code=%d(%s)\n", vs_data(req->url), ret, zipper_err_msg(ret));
					goto zipper_build_error;
				}

				vbp.flag = vbp.flag ^ BLDFLAG_CAL_SIZE;
				buffer_param.offset = 0;
				buffer_param.buf = (void *)SCX_MALLOC(vbp.output.written);
				buffer_param.size = vbp.output.written;
				streaming->ioh->writer.param = &buffer_param;

				if((ret = zipper_variant_build(ioh, v, &vbp)) != zipper_err_success ||	vbp.output.written == 0) {
					scx_error_log(req, "'%s' couldn't build zipper variant context, code=%d(%s)\n", vs_data(req->url), ret, zipper_err_msg(ret));
					goto zipper_build_error;
				}
				streaming->media_size = vbp.output.written;
				req->scx_res.body = (void *)buffer_param.buf;
				zipper_free_variant_context(ioh, &v);

			}
			else {
				/* 현재는 playlist를 하드코딩으로 처리하지만 나중에 동적으로 처리할수 있도록 개선해야 한다. */
				if (req->service->hls_master_playlist_use_absolute_path) {
					/* 정적 URL 사용시 */
					ASSERT(0 < streaming->real_path_len);
					abs_path_len = streaming->real_path_len + 1;
					if (req->service->hls_playlist_host_domain) {
						/* content routing 동작 부분 */
						if (req->service->cr_service_enable == 1) {
							/*
							* content의 mtime이 cr_hot_content_day에 지정된 날짜가 지나지 않은 컨텐츠는 Hot content라고 판단하고
							* content routing 동작을 하지 않고 요청을 받은 서버가 처리 한다.
							*/
							if (streaming->builder->media_list->media->mtime < (scx_get_cached_time_sec() - (req->service->cr_hot_content_day * 86400)) ) {
								/*
								* content routing에 사용되는 content의 경로는 요청 url이 아닌 오리진에 요청되는 컨텐츠 경로 기준으로 한다.
								* adaptive의 경우에는 smil 파일이 들어 있는 컨텐츠중 제일 첫번째 컨텐츠의 경로를 사용한다.
								*/
								hostdomain = cr_find_node(req, streaming->builder->media_list->media->url);
							}


						}
						if (hostdomain == NULL) {
							hostdomain = vs_data(req->service->hls_playlist_host_domain);
						}
					}

					if (hostdomain != NULL) {
						abs_path_len += strlen(hostdomain) + 10; /* 'scheme://domain:port' 의 길이 */
					}
					if (req->service->manifest_prefix_path) {
						abs_path_len += vs_length(req->service->manifest_prefix_path) + 1; /* 서비스명-볼륨명 형식으로 첫번째 디렉토리에 들어간다. */
					}
					if (req->app_inst != NULL) {
						abs_path_len += strlen(req->app_inst) + 1;
					}
					abs_path = (void *)SCX_CALLOC(1, abs_path_len);
					if (hostdomain != NULL) {
						pos = snprintf(abs_path, abs_path_len,
								"%s://%s",
								req->hls_playlist_protocol?"https":"http", hostdomain);
					}
					if (req->service->manifest_prefix_path) {
						snprintf(abs_path+pos, abs_path_len-pos,
								"/%s", vs_data(req->service->manifest_prefix_path));
						pos = strlen(abs_path);
					}
					if (req->app_inst != NULL) {
						snprintf(abs_path+pos, abs_path_len-pos,
								"/%s", req->app_inst);
						pos = strlen(abs_path);
					}
					snprintf(abs_path+pos, streaming->real_path_len, "%s", vs_data(req->ori_url));
				}

				/* body buffer 크기 계산 및 할당 */
				if (builder->is_adaptive == 1) {
					track_cnt = builder->adaptive_track_cnt;
				}
				else {
					track_cnt = 1;
				}
				if (NULL != streaming->argument) {
					argument_len = strlen(streaming->argument);
				}
				max_body_len = (abs_path_len + argument_len + 300) * track_cnt + 1024; /* 300은 EXT-X-STREAM-INF에 들어가는 내용의 최대 길이임 */
				req->scx_res.body = (void *)SCX_CALLOC(1, max_body_len);

				sprintf(req->scx_res.body, "#EXTM3U\n#EXT-X-VERSION:%u\n", 3);
				snprintf(hls_format, max_body_len, "m3u8%s\n", (streaming->argument ?streaming->argument :""));

				if (builder->is_adaptive == 1) {
					for(i = 0; i < builder->adaptive_track_cnt; i++) {
						stsformat(req->scx_res.body+strlen(req->scx_res.body),
								(char *)gscx__hls_adaptive_extinf_format, builder->track_info[i].adaptive_track_name, i, 0, hls_format, strlen(hls_format));
						/*
						*  smil 파일에 들어 있는  codecs와 resolution 정보가 들어간다.
						*  예 : '#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=4511805,CODECS="avc1.100.31, mp4a.40.2",RESOLUTION=1280x720'
						*/
						if (builder->track_info[i].codecs != NULL) {
							sprintf(req->scx_res.body+strlen(req->scx_res.body), ",CODECS=\"%s\"", builder->track_info[i].codecs);
						}
						if (builder->track_info[i].resolution != NULL) {
							sprintf(req->scx_res.body+strlen(req->scx_res.body), ",RESOLUTION=%s", builder->track_info[i].resolution);
						}
						/* smil 파일에 자막이 포함 되어 있는 경우 SUBTITLES attribute가 추가 된다. */
						if (streaming->subtitle_count > 0) {
							sprintf(req->scx_res.body+strlen(req->scx_res.body), ",SUBTITLES=\"subs\"");
						}
						sprintf(req->scx_res.body+strlen(req->scx_res.body), "\n");
						/*  정적 URL 사용시 */
						if (req->service->hls_master_playlist_use_absolute_path) {
							snprintf(req->scx_res.body+strlen(req->scx_res.body), max_body_len-strlen(req->scx_res.body)-1,
									"%s/", abs_path);
						}

						stsformat(req->scx_res.body+strlen(req->scx_res.body),
								(char *)gscx__hls_adaptive_m3u8_format, (char *)builder->track_info[i].adaptive_track_name, i, 0, hls_format, strlen(hls_format));
					}

					subtitle_url_size =  hls_subtitle_format_len + 50;
					subtitle = streaming->subtitle;
					while (subtitle && streaming->subtitle_count >  array_pos) {	// 첫번째 smil에 들어 있는 자막 트랙만 반영하기 위해 ramain_count를 사용
						// 예) #EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME="english",LANGUAGE="en",DEFAULT=YES,AUTOSELECT=YES,URI="subtitle_1.m3u8"
						sprintf(req->scx_res.body+strlen(req->scx_res.body),
								"#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"%s\",LANGUAGE=\"%s\"",
								subtitle->subtitle_name, subtitle->subtitle_lang);
						if (array_pos == 0) {
							sprintf(req->scx_res.body+strlen(req->scx_res.body), ",DEFAULT=YES,AUTOSELECT=YES");
						}
						else {
							sprintf(req->scx_res.body+strlen(req->scx_res.body), ",DEFAULT=NO,AUTOSELECT=NO");
						}
						sprintf(req->scx_res.body+strlen(req->scx_res.body),
								",URI=\"subtitle_%d.m3u8%s\"\n", array_pos, (streaming->argument?streaming->argument :""));
						array_pos++;
						subtitle = subtitle->next;
					}
				}	//end of if (builder->is_adaptive == 1)
				else {
					/* EXT-X-STREAM-INF 태그에 BANDWIDTH attribute가 없는 경우는 IOS에서 재생이 안되서 하드코딩으로 값을 넣는다. */
					snprintf(string_buf, 12, "%d", builder->bandwidth);
					stsformat(req->scx_res.body+strlen(req->scx_res.body),
							(char *)gscx__hls_adaptive_extinf_format, string_buf, 0, 0, hls_format, strlen(hls_format));
					/* adaptive가 아닌 경우에는 media 정보로 부터 resolution 정보를 가져 온다. */
					sprintf(req->scx_res.body+strlen(req->scx_res.body), ",RESOLUTION=%dx%d", builder->width, builder->height);
					sprintf(req->scx_res.body+strlen(req->scx_res.body), "\n");
					/*  정적 URL 사용시 */
					if (req->service->hls_master_playlist_use_absolute_path) {
						snprintf(req->scx_res.body+strlen(req->scx_res.body), max_body_len-strlen(req->scx_res.body)-1,
								"%s/", abs_path);
					}
					stsformat(req->scx_res.body+strlen(req->scx_res.body),
							(char *)gscx__hls_m3u8_format, NULL, 0, 0, hls_format, strlen(hls_format));
				}
				if (abs_path) SCX_FREE(abs_path);
				req->streaming->media_size = strlen(req->scx_res.body);
			}
			break;

		case MEDIA_TYPE_HLS_AUDIOLIST:
		case MEDIA_TYPE_HLS_SUB_M3U8:
		case MEDIA_TYPE_HLS_VTT_M3U8:
			if (streaming->media_type == MEDIA_TYPE_HLS_VTT_M3U8) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_TEXT | BLDFLAG_CAL_SIZE;
			}
			else {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
			}

			if (req->service->hls_media_playlist_use_absolute_path) {
				/* 정적 URL 사용시 */
				ASSERT(0 < streaming->real_path_len);
				abs_path_len = streaming->real_path_len + 1;
				if (req->service->hls_playlist_host_domain) {
					abs_path_len += vs_length(req->service->hls_playlist_host_domain) + 10; /* 'scheme://domain:port' 의 길이 */
				}
				if (req->service->manifest_prefix_path) {
					abs_path_len += vs_length(req->service->manifest_prefix_path) + 1; /* 서비스명-볼륨명 형식으로 첫번째 디렉토리에 들어간다. */
				}
				if (req->app_inst != NULL) {
					abs_path_len += strlen(req->app_inst) + 1;
				}
				abs_path = (void *)SCX_CALLOC(1, abs_path_len);
				if (req->service->hls_playlist_host_domain) {
					pos = snprintf(abs_path, abs_path_len,
							"%s://%s",
							req->hls_playlist_protocol?"https":"http", vs_data(req->service->hls_playlist_host_domain));
				}
				if (req->service->manifest_prefix_path) {
					snprintf(abs_path+pos, abs_path_len-pos,
							"/%s", vs_data(req->service->manifest_prefix_path));
					pos = strlen(abs_path);
				}
				if (req->app_inst != NULL) {
					snprintf(abs_path+pos, abs_path_len-pos,
							"/%s", req->app_inst);
					pos = strlen(abs_path);
				}
				snprintf(abs_path+pos, streaming->real_path_len, "%s", vs_data(req->ori_url));

				hls_format_len = scx_snprintf(hls_format, MEDIA_FORMAT_MAX_LEN, "%s/", abs_path);
				if (abs_path) {
					SCX_FREE(abs_path);
					abs_path = NULL;
				}
			}
			if (streaming->media_type == MEDIA_TYPE_HLS_VTT_M3U8) {
				snprintf(hls_format+hls_format_len, MEDIA_FORMAT_MAX_LEN, "subtitle_%d_%%i.vtt%s", streaming->rep_num, (streaming->argument ?streaming->argument :""));
				bprm->attr.index.adapt = 0;	//자막은 adaptive track을 구성하지 않기 때문에 항상 0이다
			}
			else {
				if (ENCRYPT_METHOD_NONE < req->service->hls_encrypt_method) {
					encrypt = (zipper_builder_encrypt *)mp_alloc(streaming->mpool, sizeof(zipper_builder_encrypt));
					/* key_url의 크기는 vs_length(req->service->hls_playlist_host_domain) + streaming->real_path_len + strlen(streaming->argument) + 20 정도이지만 귀찮아서 MEDIA_FORMAT_MAX_LEN를 사용 -_- */
					key_url = mp_alloc(streaming->mpool,  MEDIA_FORMAT_MAX_LEN);

					if (hls_format != NULL) {
						snprintf(key_url, MEDIA_FORMAT_MAX_LEN,
														"%sdecrypt.key%s",
														hls_format, (streaming->argument ?streaming->argument :"") );
					}
					else {
						snprintf(key_url, MEDIA_FORMAT_MAX_LEN,
														"decrypt.key%s",
														(streaming->argument ?streaming->argument :"") );
					}
					encrypt->format.hls.uri = key_url;
					if (ENCRYPT_METHOD_SAMPLE_AES == req->service->hls_encrypt_method) {
						encrypt->smplaes = 1;
					}
#ifdef ENABLE_AESNI
					encrypt->aesni = gscx__use_aesni;
#endif
					bprm->encrypt = encrypt;
					bprm->attr.bldflag |= BLDFLAG_ENCRYPT;

				}
				if (builder->is_adaptive == 1) {
					snprintf(hls_format+hls_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_adaptive_ts_format, (streaming->argument ?streaming->argument :""));
				}
				else {
					snprintf(hls_format+hls_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_ts_format, (streaming->argument ?streaming->argument :""));
				}
				bprm->attr.index.adapt = track_id;
			}
			bprm->target.attr.m3u8.format = hls_format;

			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			req->t_zipper_build += (te - ts);
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;

			break;
		case MEDIA_TYPE_HLS_TS:
		case MEDIA_TYPE_HLS_VTT:
			if (streaming->media_type == MEDIA_TYPE_HLS_VTT_M3U8) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_TEXT | BLDFLAG_CAL_SIZE;
			}
			else {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
			}
			if (streaming->media_type == MEDIA_TYPE_HLS_VTT_M3U8) {
				bprm->attr.index.adapt = 0;	//자막은 adaptive track을 구성하지 않기 때문에 항상 0이다
			}
			else {
				if (ENCRYPT_METHOD_NONE < req->service->hls_encrypt_method) {
					strm_make_enc_key(req, string_buf);
					encrypt = (zipper_builder_encrypt *)mp_alloc(streaming->mpool, sizeof(zipper_builder_encrypt));
					memcpy(encrypt->key, string_buf, 16);
					if (ENCRYPT_METHOD_SAMPLE_AES == req->service->hls_encrypt_method) {
						encrypt->smplaes = 1;
					}
	#ifdef ENABLE_AESNI
					encrypt->aesni = gscx__use_aesni;
	#endif
					bprm->encrypt = encrypt;
					bprm->attr.bldflag |= BLDFLAG_ENCRYPT;
				}
				bprm->attr.index.adapt = track_id;
			}
			//bprm->attr.index = req->streaming->ts_num;
			bprm->attr.index.seq = req->streaming->ts_num;

			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;

			bprm->attr.bldflag = bprm->attr.bldflag ^ BLDFLAG_CAL_SIZE;
			bprm->target.range_get.start = 0;
			bprm->target.range_get.size = streaming->media_size;
			buffer_param.offset = 0;
			buffer_param.buf = (void *)SCX_MALLOC(streaming->media_size);;
			buffer_param.size = streaming->media_size;
			streaming->ioh->writer.param = &buffer_param;
			bprm->attr.index.adapt = track_id;
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			req->scx_res.body = (void *)buffer_param.buf;
			break;
		case MEDIA_TYPE_HLS_FMP4_MAIN_M3U8:
			bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
			if (req->service->hls_media_playlist_use_absolute_path) {
				ASSERT(0 < streaming->real_path_len);
				abs_path_len = streaming->real_path_len + 1;
				if (req->service->hls_playlist_host_domain) {
					/* content routing 동작 부분 */
					if (req->service->cr_service_enable == 1) {
						/*
						 * content의 mtime이 cr_hot_content_day에 지정된 날짜가 지나지 않은 컨텐츠는 Hot content라고 판단하고
						 * content routing 동작을 하지 않고 요청을 받은 서버가 처리 한다.
						 */
						if (streaming->builder->media_list->media->mtime < (scx_get_cached_time_sec() - (req->service->cr_hot_content_day * 86400)) ) {
							/*
							 * content routing에 사용되는 content의 경로는 요청 url이 아닌 오리진에 요청되는 컨텐츠 경로 기준으로 한다.
							 * adaptive의 경우에는 smil 파일이 들어 있는 컨텐츠중 제일 첫번째 컨텐츠의 경로를 사용한다.
							 */
							hostdomain = cr_find_node(req, streaming->builder->media_list->media->url);
						}


					}
					if (hostdomain == NULL) {
						hostdomain = vs_data(req->service->hls_playlist_host_domain);
					}
				}
				if (hostdomain != NULL) {
					abs_path_len += strlen(hostdomain) + 10; /* 'scheme://domain:port' 의 길이 */
				}
				if (req->service->manifest_prefix_path) {
					abs_path_len += vs_length(req->service->manifest_prefix_path) + 1; /* 서비스명-볼륨명 형식으로 첫번째 디렉토리에 들어간다. */
				}
				if (req->app_inst != NULL) {
					abs_path_len += strlen(req->app_inst) + 1;
				}
				abs_path = (void *)SCX_CALLOC(1, abs_path_len);
				if (hostdomain != NULL) {
					pos = snprintf(abs_path, abs_path_len,
							"%s://%s",
							req->hls_playlist_protocol?"https":"http", hostdomain);
				}
				if (req->service->manifest_prefix_path) {
					snprintf(abs_path+pos, abs_path_len-pos,
							"/%s", vs_data(req->service->manifest_prefix_path));
					pos = strlen(abs_path);
				}
				if (req->app_inst != NULL) {
					snprintf(abs_path+pos, abs_path_len-pos,
							"/%s", req->app_inst);
					pos = strlen(abs_path);
				}
				snprintf(abs_path+pos, streaming->real_path_len, "%s", vs_data(req->ori_url));
				hls_video_format_len = scx_snprintf(hls_video_format, MEDIA_FORMAT_MAX_LEN, "%s/", abs_path);
				hls_audio_format_len = scx_snprintf(hls_audio_format, MEDIA_FORMAT_MAX_LEN, "%s/", abs_path);
				if (streaming->subtitle_count > 0) {
					hls_subtitle_format_len = scx_snprintf(hls_subtitle_format, MEDIA_FORMAT_MAX_LEN, "%s/", abs_path);
				}
				if (abs_path) {
					SCX_FREE(abs_path);
					abs_path = NULL;
				}
			}
// segment MP4 방식의 HLS의 경우 AES-128 암호화를 사용하지 못한다.
#if 0
			if (ENCRYPT_METHOD_NONE < req->service->hls_encrypt_method) {
				encrypt = (zipper_builder_encrypt *)mp_alloc(streaming->mpool, sizeof(zipper_builder_encrypt));
				/* key_url의 크기는 vs_length(req->service->hls_playlist_host_domain) + streaming->real_path_len + strlen(streaming->argument) + 20 정도이지만 귀찮아서 MEDIA_FORMAT_MAX_LEN를 사용 -_- */
				key_url = mp_alloc(streaming->mpool,  MEDIA_FORMAT_MAX_LEN);
				snprintf(key_url, MEDIA_FORMAT_MAX_LEN,
						"license.key%s",
						(streaming->argument ?streaming->argument :"") );
				encrypt->format.hls.uri = key_url;
				if (ENCRYPT_METHOD_SAMPLE_AES == req->service->hls_encrypt_method) {
					encrypt->smplaes = 1;
				}
#ifdef ENABLE_AESNI
				encrypt->aesni = gscx__use_aesni;
#endif
				bprm->encrypt = encrypt;
				bprm->attr.bldflag |= BLDFLAG_ENCRYPT;

			}
#endif
			if (builder->is_adaptive == 1) {
				snprintf(hls_video_format+hls_video_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_video_m3u8_format, (streaming->argument ?streaming->argument :""));
				snprintf(hls_audio_format+hls_audio_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_audio_m3u8_format, (streaming->argument ?streaming->argument :""));
			}
			else {
				snprintf(hls_video_format+hls_video_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_video_m3u8_format, (streaming->argument ?streaming->argument :""));
				snprintf(hls_audio_format+hls_audio_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_audio_m3u8_format, (streaming->argument ?streaming->argument :""));
			}
			bprm->target.attr.fmp4m3u8.video = hls_video_format;
			bprm->target.attr.fmp4m3u8.audio = hls_audio_format;


			/* smil 파일에 자막이 포함 되어 있는 경우 처리 부분 */
			if (streaming->subtitle_count > 0 && builder->is_adaptive == 1) {
				subtitle_url_size =  hls_subtitle_format_len + 50;
				subtitle = streaming->subtitle;
				while (subtitle && streaming->subtitle_count >  array_pos) {	// 첫번째 smil에 들어 있는 자막 트랙만 반영하기 위해 array_pos를 사용
					subtitle_url =  (char *) mp_alloc(streaming->mpool, subtitle_url_size);
					snprintf(subtitle_url, subtitle_url_size, "%ssubtitle_%d.m3u8%s", hls_subtitle_format, array_pos, (streaming->argument?streaming->argument :""));
					bprm->target.sub_link[array_pos].url = subtitle_url;
					bprm->target.sub_link[array_pos].desc = subtitle->subtitle_name;
					bprm->target.sub_link[array_pos].lang = subtitle->subtitle_lang;
					array_pos++;
					subtitle = subtitle->next;
				}
			}

			bprm->attr.index.adapt = 0xff; /* adaptive 형태로 출력 */
			bprm->target.range_get.ctx = rgCtx;
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			req->t_zipper_build += (te - ts);
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;
			break;
		case MEDIA_TYPE_HLS_FMP4_VIDEO_M3U8:
		case MEDIA_TYPE_HLS_FMP4_AUDIO_M3U8:
			bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
			if (req->service->hls_media_playlist_use_absolute_path) {
				/* 정적 URL 사용시 */
				ASSERT(0 < streaming->real_path_len);
				abs_path_len = streaming->real_path_len + 1;
				if (req->service->hls_playlist_host_domain) {
					abs_path_len += vs_length(req->service->hls_playlist_host_domain) + 10; /* 'scheme://domain:port' 의 길이 */
				}
				if (req->app_inst != NULL) {
					abs_path_len += strlen(req->app_inst) + 1;
				}
				abs_path = (void *)SCX_CALLOC(1, abs_path_len);
				if (req->service->hls_playlist_host_domain) {
					pos = snprintf(abs_path, abs_path_len,
							"%s://%s",
							req->hls_playlist_protocol?"https":"http", vs_data(req->service->hls_playlist_host_domain));
				}
				if (req->app_inst != NULL) {
					snprintf(abs_path+pos, abs_path_len-pos,
							"/%s", req->app_inst);
					pos = strlen(abs_path);
				}
				snprintf(abs_path+pos, streaming->real_path_len, "%s", vs_data(req->ori_url));

				hls_format_len = scx_snprintf(hls_format, MEDIA_FORMAT_MAX_LEN, "%s/", abs_path);
				hls_init_format_len = scx_snprintf(hls_init_format, MEDIA_FORMAT_MAX_LEN, "%s/", abs_path);
				if (abs_path) {
					SCX_FREE(abs_path);
					abs_path = NULL;
				}
			}
// segment MP4 방식의 HLS의 경우 AES-128 암호화를 사용하지 못한다.
#if 0
			if (ENCRYPT_METHOD_NONE < req->service->hls_encrypt_method) {
				encrypt = (zipper_builder_encrypt *)mp_alloc(streaming->mpool, sizeof(zipper_builder_encrypt));
				/* key_url의 크기는 vs_length(req->service->hls_playlist_host_domain) + streaming->real_path_len + strlen(streaming->argument) + 20 정도이지만 귀찮아서 MEDIA_FORMAT_MAX_LEN를 사용 -_- */
				key_url = mp_alloc(streaming->mpool,  MEDIA_FORMAT_MAX_LEN);
				snprintf(key_url, MEDIA_FORMAT_MAX_LEN,
						"license.key%s",
						(streaming->argument ?streaming->argument :"") );
				encrypt->format.hls.uri = key_url;
				if (ENCRYPT_METHOD_SAMPLE_AES == req->service->hls_encrypt_method) {
					encrypt->smplaes = 1;
				}
#ifdef ENABLE_AESNI
				encrypt->aesni = gscx__use_aesni;
#endif
				bprm->encrypt = encrypt;
				bprm->attr.bldflag |= BLDFLAG_ENCRYPT;

			}
#endif
			if (streaming->is_adaptive == 1 && streaming->adaptive_info->next == NULL) {
				/*
				 * 하나의 컨텐츠(smil 파일이 한개)로만 adaptive streaming 하는 경우
				 * media indexing 부하를 최소화 하기 위해서 요청된 트랙만 zipper_add_track()에서 추가 하는 방식으로 했기 때문에
				 * 아래 처럼 track 번호를 직접 지정 하고
				 * bprm->attr.index.adapt를 0으로 한다.
				 */
				if (streaming->media_type == MEDIA_TYPE_HLS_FMP4_VIDEO_M3U8) {
					snprintf(hls_init_format+hls_init_format_len, MEDIA_FORMAT_MAX_LEN, "%d_video_init.m4s%s", streaming->rep_num, (streaming->argument ?streaming->argument :""));
					snprintf(hls_format+hls_format_len, MEDIA_FORMAT_MAX_LEN, "%d_video_segment_%%i.m4s%s", streaming->rep_num, (streaming->argument ?streaming->argument :""));
				}
				else {	/* MEDIA_TYPE_HLS_FMP4_AUDIO_M3U8 인 경우 */
					snprintf(hls_init_format+hls_init_format_len, MEDIA_FORMAT_MAX_LEN, "%d_audio_init.m4s%s", streaming->rep_num, (streaming->argument ?streaming->argument :""));
					snprintf(hls_format+hls_format_len, MEDIA_FORMAT_MAX_LEN, "%d_audio_segment_%%i.m4s%s", streaming->rep_num, (streaming->argument ?streaming->argument :""));
				}

				bprm->attr.index.adapt = 0;
			}
			else {
				/*
				 * 두개 이상의 파일을 사용해서 zipping 을 하는 경우에는 모든 트랙을 추가 하는 방식으로 한다.
				 * 요청된 track 뿐만 아니라 모든 트랙을 다 zipper_add_track()에 추가 하는 경우
				 */
				if (streaming->media_type == MEDIA_TYPE_HLS_FMP4_VIDEO_M3U8) {
					snprintf(hls_init_format+hls_init_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_video_init_format, (streaming->argument ?streaming->argument :""));
					snprintf(hls_format+hls_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_video_seq_format, (streaming->argument ?streaming->argument :""));
				}
				else {	/* MEDIA_TYPE_HLS_FMP4_AUDIO_M3U8 인 경우 */
					snprintf(hls_init_format+hls_init_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_audio_init_format, (streaming->argument ?streaming->argument :""));
					snprintf(hls_format+hls_format_len, MEDIA_FORMAT_MAX_LEN, "%s%s", gscx__hls_fmp4_audio_seq_format, (streaming->argument ?streaming->argument :""));
				}
				bprm->attr.index.adapt = streaming->rep_num;
			}

			bprm->target.attr.fmp4subm3u8.init = hls_init_format;
			bprm->target.attr.fmp4subm3u8.segment = hls_format;


			bprm->target.range_get.ctx = rgCtx;
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			req->t_zipper_build += (te - ts);
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;
			break;
		case MEDIA_TYPE_MPEG_DASH_MANIFEST:
		case MEDIA_TYPE_MPEG_DASH_TS_MANIFEST:
		case MEDIA_TYPE_MPEG_DASH_YT_MANIFEST:
			bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
            bprm->target.attr.mpd.min_buffer_time = 0;
            bprm->target.attr.mpd.format = dash_format;
			bprm->attr.index.adapt = 0xFF; /* DASH의 경우 adaptive의 유무에 관계 없이 동일 하다 */
			pos = 0;
			if (req->service->dash_manifest_use_absolute_path) {
				/* 정적 URL 사용시 */
				ASSERT(0 < streaming->real_path_len);
				abs_path_len = streaming->real_path_len + 2;
				if (service->dash_manifest_host_domain) {
						/* content routing 동작 부분 */
					if (req->service->cr_service_enable == 1) {
						/*
						 * content의 mtime이 cr_hot_content_day에 지정된 날짜가 지나지 않은 컨텐츠는 Hot content라고 판단하고
						 * content routing 동작을 하지 않고 요청을 받은 서버가 처리 한다.
						 */
						if (streaming->builder->media_list->media->mtime < (scx_get_cached_time_sec() - (req->service->cr_hot_content_day * 86400)) ) {
							/*
							 * content routing에 사용되는 content의 경로는 요청 url이 아닌 오리진에 요청되는 컨텐츠 경로 기준으로 한다.
							 * adaptive의 경우에는 smil 파일이 들어 있는 컨텐츠중 제일 첫번째 컨텐츠의 경로를 사용한다.
							 */
							hostdomain = cr_find_node(req, streaming->builder->media_list->media->url);
						}


					}
					if (hostdomain == NULL) {
						hostdomain = vs_data(req->service->dash_manifest_host_domain);
					}
				}	// end of if (service->dash_manifest_host_domain)
				if (hostdomain != NULL) {
					abs_path_len += strlen(hostdomain) + 10; /* 'scheme://domain:port' 의 길이 */
				}
				if (req->service->manifest_prefix_path) {
					abs_path_len += vs_length(req->service->manifest_prefix_path) + 1; /* 서비스명-볼륨명 형식으로 첫번째 디렉토리에 들어간다. */
				}
				if (req->app_inst != NULL) {
					abs_path_len += strlen(req->app_inst) + 1;
				}
				abs_path = (void *)SCX_CALLOC(1, abs_path_len);
				if (hostdomain != NULL) {
					pos = snprintf(abs_path, abs_path_len,
							"%s://%s",
							req->dash_manifest_protocol?"https":"http", hostdomain);
				}
				if (req->service->manifest_prefix_path) {
					snprintf(abs_path+pos, abs_path_len-pos,
							"/%s", vs_data(req->service->manifest_prefix_path));
					pos = strlen(abs_path);
				}
				if (req->app_inst != NULL) {
					snprintf(abs_path+pos, abs_path_len-pos,
							"/%s", req->app_inst);
					pos = strlen(abs_path);
				}
				snprintf(abs_path+pos, streaming->real_path_len, "%s", vs_data(req->ori_url));
				pos = strlen(abs_path);
				snprintf(abs_path+pos, abs_path_len, "/");
				pos = strlen(abs_path);
			}

			if (NULL != streaming->argument) argument_len = strlen(streaming->argument);
			if (streaming->media_type == MEDIA_TYPE_MPEG_DASH_MANIFEST) {
				format_len = pos + argument_len + strlen(gscx__dash_audio_init_format) + 1;
				dash_audio_init_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_audio_init_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_audio_init_format, (streaming->argument ?streaming->argument :"") );
				format_len = pos + argument_len + strlen(gscx__dash_audio_seq_format) + 1;
				dash_audio_seq_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_audio_seq_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_audio_seq_format, (streaming->argument ?streaming->argument :"") );
				format_len = pos + argument_len + strlen(gscx__dash_video_init_format) + 1;
				dash_video_init_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_video_init_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_video_init_format, (streaming->argument ?streaming->argument :"") );
				format_len = pos + argument_len + strlen(gscx__dash_video_seq_format) + 1;
				dash_video_seq_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_video_seq_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_video_seq_format, (streaming->argument ?streaming->argument :"") );
				dash_format->type = DASH_TYPE_SEP;
			}
			else if (streaming->media_type == MEDIA_TYPE_MPEG_DASH_TS_MANIFEST) {
				format_len = pos + argument_len + strlen(gscx__dash_ts_index_format) + 1;
				dash_audio_init_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_audio_init_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_ts_index_format, (streaming->argument ?streaming->argument :"") );
				format_len = pos + argument_len + strlen(gscx__dash_ts_format) + 1;
				dash_audio_seq_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_audio_seq_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_ts_format, (streaming->argument ?streaming->argument :"") );
				dash_format->type = DASH_TYPE_TS;
			}
			else {
				format_len = pos + argument_len + strlen(gscx__dash_yt_audio_format) + 1;
				dash_audio_init_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_audio_init_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_yt_audio_format, (streaming->argument ?streaming->argument :"") );
				format_len = pos + argument_len + strlen(gscx__dash_yt_video_format) + 1;
				dash_video_init_format = (char *) mp_alloc(streaming->mpool, format_len);
				snprintf(dash_video_init_format, format_len, "%s%s%s", (abs_path?abs_path:""), gscx__dash_yt_video_format, (streaming->argument ?streaming->argument :"") );
				dash_format->type = DASH_TYPE_SINGLE;
			}
			if (abs_path) {
				SCX_FREE(abs_path);
				abs_path = NULL;
			}
			dash_format->audio.init = dash_audio_init_format;
			dash_format->audio.seq = dash_audio_seq_format;
			dash_format->video.init = dash_video_init_format;
			dash_format->video.seq = dash_video_seq_format;

			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			req->t_zipper_build += (te - ts);
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;


			break;

		case MEDIA_TYPE_MPEG_DASH_AUDIO_INIT:
		case MEDIA_TYPE_MPEG_DASH_AUDIO:
		case MEDIA_TYPE_MPEG_DASH_VIDEO_INIT:
		case MEDIA_TYPE_MPEG_DASH_VIDEO:
			if (streaming->media_type == MEDIA_TYPE_MPEG_DASH_AUDIO_INIT ||
				streaming->media_type == MEDIA_TYPE_MPEG_DASH_AUDIO	) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_AUDIO | BLDFLAG_CAL_SIZE;
			}
			else {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_VIDEO | BLDFLAG_CAL_SIZE;
			}
			if (streaming->is_adaptive == 1 && streaming->adaptive_info->next == NULL) {
				/*
				 * 하나의 컨텐츠(smil 파일이 한개)로만 adaptive streaming 하는 경우
				 * media indexing 부하를 최소화 하기 위해서 요청된 트랙만 zipper_add_track()에서 추가 하는 방식으로 했기 때문에
				 * 아래 처럼 track 번호를 직접 지정 하고
				 * bprm->attr.index.adapt를 0으로 한다.
				 */
				bprm->attr.index.adapt = 0;
			}
			else {
				bprm->attr.index.adapt = atoi(streaming->rep_id);
			}

			if (streaming->media_type == MEDIA_TYPE_MPEG_DASH_AUDIO ||
				streaming->media_type == MEDIA_TYPE_MPEG_DASH_VIDEO) {
#if 0
				segment_number = zipper_segment_index(builder->bcontext, bprm->attr.index.adapt, streaming->ts_num);
#else
				segment_number = streaming->ts_num - 1; //manifest에 1번 부터 시작이라 1번 segment 요청이 실제로 0번 segment 요청 이다.
#endif
				if(segment_number == (uint32_t)EOF) {
					scx_error_log(req, "'%s' zipper_segment_index error(%d)\n", vs_data(req->url), streaming->ts_num);
					goto send_stream_error;
				}
				bprm->attr.index.seq = segment_number;
			}
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;;
			break;
		case MEDIA_TYPE_MPEG_DASH_TS_INDEX:
		case MEDIA_TYPE_MPEG_DASH_TS:
			bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
			bprm->attr.index.adapt = atoi(streaming->rep_id);
			if (streaming->media_type == MEDIA_TYPE_MPEG_DASH_TS) {
#if 0
				segment_number = zipper_segment_index(builder->bcontext, bprm->attr.index.adapt, streaming->ts_num);
#else
				segment_number = streaming->ts_num - 1; //manifest에 1번 부터 시작이라 1번 segment 요청이 실제로 0번 segment 요청 이다.
#endif
				if(segment_number == (uint32_t)EOF) {
					scx_error_log(req, "'%s' zipper_segment_index error(%d)\n", vs_data(req->url), streaming->ts_num);
					goto send_stream_error;
				}
				bprm->attr.index.seq = segment_number;
			}
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;
			if (streaming->media_type == MEDIA_TYPE_MPEG_DASH_TS) {

				bprm->attr.bldflag = bprm->attr.bldflag ^ BLDFLAG_CAL_SIZE;
				bprm->target.range_get.start = 0;
				bprm->target.range_get.size = streaming->media_size;
				buffer_param.offset = 0;
				buffer_param.buf = (void *)SCX_MALLOC(streaming->media_size);
				buffer_param.size = streaming->media_size;
				streaming->ioh->writer.param = &buffer_param;
				bprm->attr.index.seq = segment_number;
				ts = sx_get_time();
				if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
					__thread_jmp_working = 1;
					ret = zipper_build(ioh, builder->bcontext, bprm);
					__thread_jmp_working = 0;
				}
				else {
					/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
					ret = zipper_err_internal;
					TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
				}
				te = sx_get_time();
				if (ret != zipper_err_success) {
					scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
					goto zipper_build_error;
				}
				req->scx_res.body = (void *)buffer_param.buf;
			}
			break;
		case MEDIA_TYPE_MPEG_DASH_YT_VIDEO:
		case MEDIA_TYPE_MPEG_DASH_YT_AUDIO:
			if (MEDIA_TYPE_MPEG_DASH_YT_AUDIO == streaming->media_type) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_AUDIO | BLDFLAG_CAL_SIZE;
			}
			else {	/* MEDIA_TYPE_MPEG_DASH_YT_VIDEO */
				bprm->attr.bldflag = BLDFLAG_INCLUDE_VIDEO | BLDFLAG_CAL_SIZE;
			}
			ret = zipper_create_rangeget_context(ioh, &rgCtx);
			streaming->rgCtx = rgCtx;
			bprm->target.range_get.ctx = rgCtx;
			bprm->attr.index.adapt = atoi(streaming->rep_id);
			bprm->attr.index.seq = 0;
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;;
			break;
		case MEDIA_TYPE_MSS_MANIFEST:
			bprm->attr.bldflag = BLDFLAG_INCLUDE_ALL | BLDFLAG_CAL_SIZE;
            bprm->target.attr.iis.format = mss_format;
			bprm->attr.index.adapt = 0xFF; /* 이 부분 확인 필요 */
			if (NULL != streaming->argument) argument_len = strlen(streaming->argument);

			format_len = argument_len + strlen(gscx__mss_audio_format) + 1;
			dash_audio_init_format = (char *) mp_alloc(streaming->mpool, format_len);
			snprintf(dash_audio_init_format, format_len, "%s%s", gscx__mss_audio_format, (streaming->argument ?streaming->argument :"") );
			format_len = argument_len + strlen(gscx__mss_video_format) + 1;
			dash_video_init_format = (char *) mp_alloc(streaming->mpool, format_len);
			snprintf(dash_video_init_format, format_len, "%s%s", gscx__mss_video_format, (streaming->argument ?streaming->argument :"") );

			mss_format->audio.format = dash_audio_init_format;
			mss_format->video.format = dash_video_init_format;

			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			req->t_zipper_build += (te - ts);
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;
			break;
		case MEDIA_TYPE_MSS_VIDEO:
		case MEDIA_TYPE_MSS_AUDIO:
			if (MEDIA_TYPE_MSS_VIDEO == streaming->media_type) {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_VIDEO | BLDFLAG_CAL_SIZE;
			}
			else {
				bprm->attr.bldflag = BLDFLAG_INCLUDE_AUDIO | BLDFLAG_CAL_SIZE;
			}
			bprm->attr.index.adapt = track_id;
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				segment_number = zipper_segment_index(builder->bcontext, bprm->attr.index.adapt, streaming->ts_num);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				segment_number = -1;
				TRACE((T_ERROR, "[%llu] zipper_segment_index() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}

			if(segment_number < 0) {
				scx_error_log(req, "'%s' zipper_segment_index error(%d)\n", vs_data(req->url), streaming->ts_num);
				goto send_stream_error;
			}
			bprm->attr.index.seq = segment_number;
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(ioh, builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->url), zipper_err_msg(ret));
				goto zipper_build_error;
			}
			streaming->media_size = bprm->output.written;
			break;
		default :
			break;
		}

		memset(&req->objstat, 0, sizeof(nc_stat_t));
		snprintf(req->objstat.st_devid, 128, "%X", builder->ctime);
		req->objstat.st_ctime = builder->ctime;
		req->objstat.st_mtime = builder->mtime;

	}

	if ( (streaming->media_mode & O_STRM_TYPE_DIRECT) == 0 ) {
		/* `=============== */
		/* 아래 부분은 일반 파일과 유사하게 동작하기 위해 objstat을 설정 한다.
		 * O_STRM_TYPE_DIRECT인 경우는 origin에서 받은 objstat을
		 * 그대로 사용하면 되므로 아래의 과정이 필요 없다.*/

		strm_set_objstat(req);
	}
	/*
	 * HLS나 DASH인 경우는 body가 이미 만들어진 상태 이기 때문에 streaming->builder를 더 유지할 필요가 없으므로
	 * 여기서 삭제 한다.
	 * media memory metadata를 효율적으로 사용하기 위해 추가
	 */
	if (NULL != req->scx_res.body && streaming->builder &&
			((streaming->protocol & O_PROTOCOL_HLS) != 0 || (streaming->protocol & O_PROTOCOL_DASH) != 0)) {
			strm_destroy_builder(streaming->builder);
			streaming->builder = NULL;
	}
	return 1;
zipper_build_error:
	if (streaming->builder != NULL &&
			ret != zipper_err_segment_index) {	/* segment index is out of range에러는 컨텐츠가 정상적인 경우에도 발생하고 있기 때문에 제외한다. */
		/* media가 문제가 있는것으로 보고 모두 expire 시킨다 */
		strm_expire_medialist(req);
	}
	if (buffer_param.buf != NULL && req->scx_res.body == NULL) {
		// https://jarvis.solbox.com/redmine/issues/33648 버그 관련 수정
		SCX_FREE(buffer_param.buf);
	}
send_stream_error:
	if (rgCtx) {
		zipper_free_rangeget_context(ioh, &rgCtx);
		streaming->rgCtx = NULL;
	}
	if (ioh) {
		strm_destroy_io_handle(ioh);
		streaming->ioh = NULL;
	}
	return 0;
}

/*
 * 일반 파일과 유사하게 동작하기 위해 objstat을 설정 한다.
 */
int
strm_set_objstat(nc_request_t *req)
{
	streaming_t 	*streaming = NULL;
	if (req->streaming == NULL) return 0;
	streaming = req->streaming;
	req->objstat.st_size = streaming->media_size;
	req->objstat.st_mode = 0;
	if (req->objstat.st_mtime == 0) {
#ifdef ZIPPER
	if (req->service->streaming_method >= 1) {
		/* live ad stitching이나 CMAF live 기능을 사용하는 경우 오리진으로부터 mtime을 받지 못하면 현재 시간으로 설정한다. */
		req->objstat.st_mtime = scx_get_cached_time_sec();
	}
	else
#endif
		{
			req->objstat.st_mtime  = 1034694000; /* Last-Modified 헤더를 "Wed, 16 Oct 2002 00:00:00 GMT"로 리턴한다.*/
		}
	}

	req->objstat.st_rangeable = 1;
	req->objstat.st_private = 0;
	req->objstat.st_sizeknown = 1;
	req->objstat.st_sizedecled = 1;
	req->objstat.st_chunked = 0;
	req->objstat.st_origincode = 206;

	return 1;
}

int
strm_destroy_stream(nc_request_t *req)
{
	streaming_t 	*streaming = NULL;
	if (req->streaming == NULL) return 0;
	streaming = req->streaming;
	ASSERT(streaming);
	if (streaming->session) {
		sm_release_session(req);
		streaming->session = NULL;
	}
	/* ioh와 rgCtx의 해제는 session 종료시에 할수 있도록 수정 해야 한다. */
	if (streaming->rgCtx) {
		zipper_free_rangeget_context(streaming->ioh, &streaming->rgCtx);
		streaming->rgCtx = NULL;
	}
	if (streaming->builder) {
		strm_destroy_builder(streaming->builder);
		streaming->builder = NULL;
	}
	if (streaming->fragment) {
		solCMAF_release_fragment(streaming->fragment);
		streaming->fragment = NULL;
	}
	if (streaming->ioh) {
		strm_destroy_io_handle(streaming->ioh);
		streaming->ioh = NULL;
	}
	if (streaming->source) {
		strm_destroy_source_info(req, streaming->source);
		streaming->source = NULL;
	}

	strm_destroy_streaming(req);
}

/*
 * zipper_build시 callback으로 호출 되는 함수
 */

int
strm_write_func(unsigned char *block, size_t size, void *param)
{
	buffer_paramt_t * buffer_param = (buffer_paramt_t *)param;
	nc_request_t 	*req = buffer_param->req;
	uint32_t alloc_size = 0;
	char * buf = NULL;

	if (!block || !param) return zipper_err_io_handle;
	if (buffer_param->size < (buffer_param->offset + size)) {
		/* 할당된 메모리보다 쓰여질 메모리가 더 큰 경우 */
		if (buffer_param->enable_realloc == 0) {
			/*
			 * enable_realloc가 0인 경우는 memory를 호출 한쪽에 memory를 할당게 아니라 다른 곳에 할당한 메모리를 전달하는 경우 이기 때문에 realloc을 할수 없다.
			 * 이 경우는 그냥 error를 리턴하는 방법 밖에 없음
			 */
			scx_error_log(req, "'%s' memory not enough, allocated(%llu), required(%llu)\n",
					__func__, buffer_param->size, buffer_param->offset + size);
			return zipper_err_io_handle;
		}
		else {
			/* 필요 용량보다 100KB를 더 크게 memory를 재할당후에 기존 메모리를 복사하고 기존 메모리는 해제한다. */
			TRACE((T_INFO, "[%llu] memory not enough, allocated(%llu), required(%llu)\n", req->id, buffer_param->size, buffer_param->offset + size));
			alloc_size = buffer_param->offset + size + 102400;
			buf = SCX_MALLOC(alloc_size);
			memset(buf+buffer_param->offset, 0, alloc_size-buffer_param->offset);	// 혹시 몰라서 offset 이후 부분은 memset을 한다.
			memcpy(buf, buffer_param->buf, buffer_param->offset);
			SCX_FREE(buffer_param->buf);
			buffer_param->buf = buf;
			buffer_param->size = alloc_size;
		}
	}
	memcpy(buffer_param->buf+buffer_param->offset, block, size);
	buffer_param->offset += size;
//	printf("%s offset = %lld\n", __func__,buffer_param->offset);
	return zipper_err_success;
}


/*
 * MHD에서 파일 전송을 위해 callback 되는 함수
 */
/*
 * ts 파일을 전송할 경우에는 한번에 필요한 버퍼 크기를 할당 받아 ts 파일을 미리 만들어 놓고
 * 요청 되는 부분에 대해서만 조금씩 리턴하는 부분을 추가 해야한다.
 */
ssize_t
streaming_reader(void *cr, uint64_t pos, char *buf, size_t max)
{
	nc_request_t 	*req = cr;
	ssize_t 			copied = 0;
	ssize_t 			r = 0;
	ssize_t 			sendto = 0; /* 전송할 크기 */
	int					ret = 0;
	double 					ts, te;
	int					track_id = 0;
	streaming_t			*streaming = req->streaming;
	zipper_builder_param *bprm = streaming->bprm;
	buffer_paramt_t		buffer_param = {req, NULL, 0, 0, 0};
	TRACE((T_DEBUG, "[%llu] streaming_reader st_size = %lld , cursor[%lld], try_read=%ld, remained=%ld\n",
			req->id, req->objstat.st_size,
			(long long)req->cursor, max, req->remained ));

	if (max == 0) {
		return MHD_CONTENT_READER_END_OF_STREAM;
	}
	if (!gscx__service_available) {	/* shutdown 과정에서 들어오는 요청의 경우 파일 읽기가 끝난걸로 처리한다. */
		TRACE((T_DAEMON, "[%llu] streaming_reader request rejected. (reason : shutdown progress)\n", req->id));
		return MHD_CONTENT_READER_END_OF_STREAM;
	}
#if 0
	if ((streaming->protocol & O_PROTOCOL_PROGRESSIVE) != 0) {

		/* 속도 제한은 mp4나 mp3 요청일 때만 걸린다. */
		if (0 < req->limit_rate || 0 < req->limit_traffic_rate) {
			max = limitrate_control(cr, max);
		}
	}
#endif
	if (req->scx_res.body) {
		/*
		 * 미리 버퍼를 생성하는 media type들은 버퍼의 내용만 전달 한다.
		 * https://jarvis.solbox.com/redmine/issues/32831 일감의 수정으로
		 * scx_res.body를 사용하는 경우에는 reader callback을 호출 하지 않고
		 * 직접 MHD_create_response_from_buffer()로 전송 하기 때문에 이곳이 호출 될일은 없다.
		 */
		sendto = min(req->cursor+max, streaming->media_size) - req->cursor;
		memcpy(buf, req->scx_res.body+req->cursor, sendto);
		req->cursor	+= sendto;
		if (req->remained > 0)
			req->remained 	-= sendto;
		r = sendto;
	}
	else if ( (streaming->media_mode & O_STRM_TYPE_DIRECT) != 0 ) {
		/* 원본에서 파일을 직접 다운로드 받는 경우 */
		sendto = min(max, req->remained);

		copied = strm_file_read(req, streaming->source->file, buf, req->cursor, sendto);
		if (copied <= 0) {
			r = MHD_CONTENT_READER_END_OF_STREAM;
		}
		else {
			req->cursor	+= copied;
			if (req->remained > 0)
				req->remained 	-= copied;
			r = copied;
		}
	}
	else if (req->service->streaming_method == 2) {
		/* CMAF live 전송인 경우 */
		r = strm_cmaf_reader(cr, pos, buf, max);
	}
	else {

		ASSERT(bprm->attr.bldtype);
		if (bprm->attr.bldflag & BLDFLAG_CAL_SIZE) {
			bprm->attr.bldflag = bprm->attr.bldflag ^ BLDFLAG_CAL_SIZE; /* BLDFLAG_CAL_SIZE 만 없앤다 */
		}

		sendto = min(max, req->remained);


		while (sendto > 0) {
			bprm->target.range_get.start = req->cursor;
			bprm->target.range_get.size = sendto;
			buffer_param.offset = r;
			buffer_param.buf = buf;
			buffer_param.size = max;
			streaming->ioh->writer.param = &buffer_param;
			ts = sx_get_time();
			if (sigsetjmp(__thread_jmp_buf, 1) == 0) {
				__thread_jmp_working = 1;
				ret = zipper_build(streaming->ioh, streaming->builder->bcontext, bprm);
				__thread_jmp_working = 0;
			}
			else {
				/* zipper 라이브러리 문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
				ret = zipper_err_internal;
				TRACE((T_ERROR, "[%llu] zipper_build() SIGSEGV occured. url(%s). %s:%d\n", req->id, vs_data(req->ori_url), __FILE__, __LINE__));
			}
			te = sx_get_time();
			req->t_zipper_build += (te - ts);
			if (ret != zipper_err_success) {
				scx_error_log(req, "'%s' zipper_build error(%s)\n", vs_data(req->ori_url), zipper_err_msg(ret));
				if (streaming->builder != NULL &&
						ret != zipper_err_segment_index) {	/* segment index is out of range에러는 컨텐츠가 정상적인 경우에도 발생하고 있기 때문에 제외한다. */
					/* media가 문제가 있는것으로 보고 모두 expire 시킨다 */
					strm_expire_medialist(req);
				}
				r = MHD_CONTENT_READER_END_OF_STREAM;
				break;
			}

			copied = bprm->output.written;
			if( copied == 0 ) break;
			r += copied;
			sendto -= copied;
			req->cursor	+= copied;
			if (req->remained > 0)
				req->remained 	-= copied;
		}
	}
	if (r != MHD_CONTENT_READER_END_OF_STREAM) {
		scx_update_res_body_size(req, r);

	}
	return r;
}

int
strm_regex_compile()
{
	int i;
	int ret;
	char buf[128];
	for (i = 0;i < gscx__count_strm_vod_reg;i++) {
		gscx__strm_vod_reg[i].preg = (regex_t *)SCX_MALLOC(sizeof(regex_t));
		ret = regcomp(gscx__strm_vod_reg[i].preg, gscx__strm_vod_reg[i].pattern, REG_EXTENDED);
		if (ret) {

			regerror(ret, gscx__strm_vod_reg[i].preg, buf, sizeof(buf) );
			TRACE((T_ERROR, "vod regex compile error(%s), %s\n",gscx__strm_vod_reg[i].pattern, buf));
			return 0;
		}
	}
	for (i = 0;i < gscx__count_strm_live_reg;i++) {
		gscx__strm_live_reg[i].preg = (regex_t *)SCX_MALLOC(sizeof(regex_t));
		ret = regcomp(gscx__strm_live_reg[i].preg, gscx__strm_live_reg[i].pattern, REG_EXTENDED);
		if (ret) {

			regerror(ret, gscx__strm_live_reg[i].preg, buf, sizeof(buf) );
			TRACE((T_ERROR, "live regex compile error(%s), %s\n",gscx__strm_live_reg[i].pattern, buf));
			return 0;
		}
	}
	gscx__strm_url_preg = (regex_t *)SCX_MALLOC(sizeof(regex_t));
	ret = regcomp(gscx__strm_url_preg, gscx__strm_url_pattern, REG_EXTENDED);
	if (ret) {
		regerror(ret, gscx__strm_url_preg, buf, sizeof(buf) );
		TRACE((T_ERROR, "strm url regex compile error(%s), %s\n",gscx__strm_url_pattern, buf));
		SCX_FREE(gscx__strm_url_preg);
		gscx__strm_url_preg = NULL;
		return 0;
	}

	gscx__strm_crossdomain_preg = (regex_t *)SCX_MALLOC(sizeof(regex_t));
	ret = regcomp(gscx__strm_crossdomain_preg, gscx__strm_crossdomain_pattern, REG_EXTENDED);
	if (ret) {
		regerror(ret, gscx__strm_crossdomain_preg, buf, sizeof(buf) );
		TRACE((T_ERROR, "strm url regex compile error(%s), %s\n",gscx__strm_crossdomain_pattern, buf));
		SCX_FREE(gscx__strm_crossdomain_preg);
		gscx__strm_crossdomain_preg = NULL;
		return 0;
	}
	gscx__strm_clientaccesspolicy_preg = (regex_t *)SCX_MALLOC(sizeof(regex_t));
	ret = regcomp(gscx__strm_clientaccesspolicy_preg, gscx__strm_clientaccesspolicy_pattern, REG_EXTENDED);
	if (ret) {
		regerror(ret, gscx__strm_clientaccesspolicy_preg, buf, sizeof(buf) );
		TRACE((T_ERROR, "strm url regex compile error(%s), %s\n",gscx__strm_clientaccesspolicy_pattern, buf));
		SCX_FREE(gscx__strm_clientaccesspolicy_preg);
		gscx__strm_clientaccesspolicy_preg = NULL;
		return 0;
	}

	gscx__strm_content_preg = (regex_t *)SCX_MALLOC(sizeof(regex_t));
	ret = regcomp(gscx__strm_content_preg, gscx__strm_content_pattern, REG_EXTENDED);
	if (ret) {
		regerror(ret, gscx__strm_content_preg, buf, sizeof(buf) );
		TRACE((T_ERROR, "strm url regex compile error(%s), %s\n",gscx__strm_content_pattern, buf));
		SCX_FREE(gscx__strm_content_preg);
		gscx__strm_content_preg = NULL;
		return 0;
	}


	return 1;
}

int
strm_regex_free()
{
	int i;
	for(i = 0;i < gscx__count_strm_vod_reg;i++) {
		if(gscx__strm_vod_reg[i].preg != NULL) {
			if(gscx__strm_vod_reg[i].preg->buffer) regfree(gscx__strm_vod_reg[i].preg);
			SCX_FREE(gscx__strm_vod_reg[i].preg);
		}
	}
	for(i = 0;i < gscx__count_strm_live_reg;i++) {
		if(gscx__strm_live_reg[i].preg != NULL) {
			if(gscx__strm_live_reg[i].preg->buffer) regfree(gscx__strm_live_reg[i].preg);
			SCX_FREE(gscx__strm_live_reg[i].preg);
		}
	}
	if(gscx__strm_url_preg != NULL) {
		regfree(gscx__strm_url_preg);
		SCX_FREE(gscx__strm_url_preg);
		gscx__strm_url_preg = NULL;
	}
	if(gscx__strm_crossdomain_preg != NULL) {
		regfree(gscx__strm_crossdomain_preg);
		SCX_FREE(gscx__strm_crossdomain_preg);
		gscx__strm_crossdomain_preg = NULL;
	}
	if(gscx__strm_clientaccesspolicy_preg != NULL) {
		regfree(gscx__strm_clientaccesspolicy_preg);
		SCX_FREE(gscx__strm_clientaccesspolicy_preg);
		gscx__strm_clientaccesspolicy_preg = NULL;
	}
	if(gscx__strm_content_preg != NULL) {
		regfree(gscx__strm_content_preg);
		SCX_FREE(gscx__strm_content_preg);
		gscx__strm_content_preg = NULL;
	}
}

/*
 * streaming에 필요한 메모리풀 할당 및 기본값 설정이 이루어진다.
 */
streaming_t	*
strm_create_streaming(nc_request_t *req)
{
	streaming_t		*streaming = NULL;
	mem_pool_t 			*mpool = NULL;
	ASSERT(req);
	mpool = mp_create(sizeof(mem_pool_t) + 10240);
	ASSERT(mpool);
	streaming = (streaming_t *)mp_alloc(mpool,sizeof(streaming_t));
	ASSERT(streaming);
	memset(streaming, 0, sizeof(streaming_t));
	streaming->mpool = mpool;
	streaming->req = req;
	streaming->session_id	= 0;
	streaming->session	= NULL;
	req->streaming = streaming;
	streaming->options = kv_create_pool_d(streaming->mpool, mp_alloc, __FILE__, __LINE__);
	kv_set_opaque(streaming->options, (void *)req);
	kv_replace(streaming->options, MHD_HTTP_HEADER_USER_AGENT, PROG_SHORT_NAME, FALSE);
	return streaming;
}

/*
 * streaming 에 할당된 메모리풀을 해제한다.
 */
int
strm_destroy_streaming(nc_request_t *req)
{
	mem_pool_t 		*mpool = NULL;
	if (NULL == req->streaming) return SCX_YES;
	if (req->streaming->mpool) {
		mpool = req->streaming->mpool;
		req->streaming->mpool = NULL;	/* mp_free를 하는 순간 streaming context자체가 없어지므로 이런식의 해제는 안된다. */
		mp_free(mpool);
	}
	req->streaming = NULL;
	return SCX_YES;
}

/*
 *  crossdomain.xml과 clientaccesspolicy.xml은 /(루트)경로로만 요청이 들어오기 때문에 zipper에서는 볼륨이 선택 되기 전에 호출 되어야 한다.
 *  solproxy 에서는 host 헤더 기반으로 볼륨이 결정 되기 때문에 strm_url_parser() 이전에 호출되면 된다.
 */
int
strm_check_policy_file(nc_request_t *req)
{
	int 		ret = 0;
	streaming_t		*streaming = NULL;
	size_t     nmatch = 10;
	regmatch_t pmatch[10];

	/* crossdomain.xml나clientaccesspolicy.xml 인지 먼저 검사함 */
	ret = regexec(gscx__strm_crossdomain_preg, vs_data(req->ori_url), nmatch, pmatch, 0);
	if (ret == 0) { /* 패턴 일치 */
		streaming = strm_create_streaming(req);
		streaming->media_type = MEDIA_TYPE_CROSSDOMAIN;
		streaming->protocol = O_PROTOCOL_CORS;
		streaming->build_type = BLDTYPE_NONE;
#ifdef ZIPPER	/* cache에서는 crossdomain.xml도 domain을 타고 들어 오므로 아래와 같은 처리가 필요 없다 */
		/* CORS 요청 처리용 특수 볼륨, 이 볼륨은 통계에서도 제외 된다.*/
		req->host = vs_allocate(req->pool, strlen(OTHERS_VOLUME_NAME), OTHERS_VOLUME_NAME, 1);
#endif
		goto strm_check_policy_file_success;
	}
	ret = regexec(gscx__strm_clientaccesspolicy_preg, vs_data(req->ori_url), nmatch, pmatch, 0);
	if (ret == 0) { /* 패턴 일치 */
		streaming = strm_create_streaming(req);
		streaming->media_type = MEDIA_TYPE_CLIENTACCESSPOLICY;
		streaming->protocol = O_PROTOCOL_CORS;
		streaming->build_type = BLDTYPE_NONE;
#ifdef ZIPPER	/* cache에서는 crossdomain.xml도 domain을 타고 들어 오므로 아래와 같은 처리가 필요 없다 */
		/* CORS 요청 처리용 특수 볼륨, 이 볼륨은 통계에서도 제외 된다.*/
		req->host = vs_allocate(req->pool, strlen(OTHERS_VOLUME_NAME), OTHERS_VOLUME_NAME, 1);
#endif
		goto strm_check_policy_file_success;
	}
	/* /fcs/ident, /fcs/uInfo 요청 인지 검사 */
	ret = strncasecmp("/fcs/ident", vs_data(req->ori_url), 10);
	if (ret != 0) {
		ret = strncasecmp("/fcs/uInfo", vs_data(req->ori_url), 10);
	}
	if (ret == 0) {
		streaming = strm_create_streaming(req);
		streaming->media_type = MEDIA_TYPE_FCS_IDENT;
		streaming->protocol = O_PROTOCOL_CORS;
		streaming->build_type = BLDTYPE_NONE;
#ifdef ZIPPER	/* cache에서는 crossdomain.xml도 domain을 타고 들어 오므로 아래와 같은 처리가 필요 없다 */
		/* CORS 요청 처리용 특수 볼륨, 이 볼륨은 통계에서도 제외 된다.*/
		req->host = vs_allocate(req->pool, strlen(OTHERS_VOLUME_NAME), OTHERS_VOLUME_NAME, 1);
#endif
		goto strm_check_policy_file_success;
	}
strm_check_policy_file_success:
	return 1;
}

/*
 * zipper로 동작 할때에는 첫번째 디렉토리(application)에 들어오는 값을 host로 사용한다.
 * 이후에 host가 아닌 순수한 service 개념이 생기면 service로 변경 가능
 */
int
strm_host_parser(nc_request_t *req)
{
	int 		ret = 0;
	streaming_t		*streaming = NULL;
	size_t     nmatch = 10;
	regmatch_t pmatch[10];
#ifdef ZIPPER
	if (NULL != req->streaming) {
		/* req->streaming()이 할당 되어 있는 경우는 strm_check_policy_file()에서 policy 파일 요청이라고 판단된 경우로 이미 req->host가 정해져 있으므로 아래와 과정이 필요 없다. */
		return 1;
	}
	/*
	 * 현재는 단순하게 하기 위해서 strm_url_parser()의 패턴 매칭 방식을 사용하지만 불필요한 부하 발생 가능성이 있음.
	 * 이 부분은 나중에 다른 방식으로 튜닝 필요
	 */
	ret = regexec(gscx__strm_url_preg, vs_data(req->ori_url), nmatch, pmatch, 0);
	if (ret == 0) { /* 패턴 일치 */
		req->host = vs_allocate(req->pool, pmatch[1].rm_eo - pmatch[1].rm_so, vs_data(req->ori_url) + pmatch[1].rm_so , 1);
		return 1;
	}
	else {
		return 0;
	}

#endif
}

/*
 * ori_uri에 와우자 url(/application/instance/content_path/virtualfile?argument) 형태로 들어 있는것을
 * application/instance를 제거 하고 /content_path/virtualfile?argument로 만들어 준다.
 * https://jarvis.solbox.com/redmine/issues/33317
 */
int
strm_wowza_url_manage(nc_request_t *req)
{
	int 		ret = 0;
	int			start_off = 0;
	int			end_off = 0;
	int			i = 0;
	int			found_cnt = 0;
	char		*ori_url = NULL;

	if (NULL != req->streaming) {
		/* req->streaming()이 할당 되어 있는 경우는 strm_check_policy_file()에서 policy 파일 요청이라고 판단된 경우로 아래 부분을 진핼 할 필요가 없음 */
		return 1;
	}

	if (*vs_data(req->ori_url) == '/') {
		/* url이 /로 시작하는 경우 /는 제외 한다. */
		start_off = 1;
	}
	ori_url = vs_data(req->ori_url) + start_off;
	for(i = start_off; i < vs_length(req->ori_url); i++) {
		if (*ori_url == '/') {
			found_cnt++;
			if (found_cnt == 2) {
				end_off = i;
				break;
			}
		}
		*ori_url++;
	}
	if (end_off == 0) {
		scx_error_log(req, "%s wowza url pattern not matched. url(%s)\n", __func__, vs_data(req->ori_url));
		return -1;
	}


	req->app_inst = mp_alloc(req->pool, end_off - start_off + 1);
	snprintf(req->app_inst, end_off - start_off + 1, "%s",vs_data(req->ori_url) + start_off);	/* application/instance를 제외한 파일 경로의 시작 위치 (/포함) */
	req->ori_url = vs_allocate(req->pool, 0, vs_data(req->ori_url)+end_off, 0);	/* application/instance를 제외한 파일 경로로 ori_url을 다시 업데이트 한다 */
#ifdef DEBUG
	printf("app_inst = '%s', url = '%s'\n", req->app_inst, vs_data(req->ori_url));
#endif

	/* req->path 경로도 업데이트 한다. */
	scx_make_path_to_url(req);
	return 0;
}

/* 입력 형태 : /[application]/[instance]/[protocol]/[lang]/[version]/[content]/[meta]/[content]/[meta]/[virtual_file]?[argument]
 *           |<--                           real_path_len                                        -->|
 * connection 연결이 되어 있는 동안 계속 사용할 정보에는 req->streaming->mpool을 사용해 메모리를 할당 해야 한다.
 * strm_url_parser()를 벋어나면 사용 하지 않을 변수에는 tmp_mpool에서 할당 받는다.
 * */
int
strm_url_parser(nc_request_t *req)
{
	streaming_t		*streaming = NULL;
	mem_pool_t 			*mpool = NULL;
	mem_pool_t 			*tmp_mpool = NULL;	/* 파싱에만 사용하는 임시용 메모리풀. 이함수를 벗어 날때 반드시 해제해야한다. */
	mode_t		protocol_mode = 0;
	size_t     nmatch = 10;
	regmatch_t pmatch[10];
	builder_info_t 	*find_builder = NULL;
	int 		ret = 0;
	vstring_t	*application = NULL;
	vstring_t 	*instance = NULL;
	vstring_t 	*protocol = NULL;
	vstring_t 	*lang = NULL;
	vstring_t 	*version = NULL;
	vstring_t 	*contents	= NULL;	/* 동영상들에 대한 정보가 들어갈곳 */
	vstring_t	*virtualfile = NULL;
	vstring_t	*argument = NULL; /* ?를 포함한 파라미터가 들어 간다. */
	vstring_t	*bytes = NULL;	/* youtube type DASH의 특정 player에서 사용 */
	vstring_t 	*zip_contents	= NULL;	/* 임시 파일 경로 */

	char		*tmp_str = NULL;
	int			tmp_len = 0;

	const char 	*var = NULL;
	int			arg_start = 0;	/* argument에 포함된 start time */
	int			arg_end = 0;	/* argument에 포함된 end time */
	int			arg_dur = 0;	/* argument에 포함된 duration time */
	float		arg_tempo = 0.0;

	char		*error_str = NULL;
	int			error_len = 0;
	int			is_without_virtualfile_streaming = 0;

	if (NULL != req->streaming) {
		/* req->streaming()이 할당 되어 있는 경우는 strm_check_policy_file()에서 policy 파일 요청이라고 판단된 경우로 아래 부분을 진핼 할 필요가 없음 */
		return 1;
	}

	tmp_mpool = mp_create(sizeof(mem_pool_t) + 10240);
	ASSERT(tmp_mpool);

	ret = regexec(gscx__strm_url_preg, vs_data(req->ori_url), nmatch, pmatch, 0);

	if (ret == 0) { /* 패턴 일치 */
#ifdef ZIPPER
		application			= vs_allocate(tmp_mpool, pmatch[1].rm_eo - pmatch[1].rm_so, vs_data(req->ori_url) + pmatch[1].rm_so, 1);
		instance			= vs_allocate(tmp_mpool, pmatch[2].rm_eo - pmatch[2].rm_so, vs_data(req->ori_url) + pmatch[2].rm_so, 1);
		protocol			= vs_allocate(tmp_mpool, pmatch[3].rm_eo - pmatch[3].rm_so, vs_data(req->ori_url) + pmatch[3].rm_so, 1);
		lang				= vs_allocate(tmp_mpool, pmatch[4].rm_eo - pmatch[4].rm_so, vs_data(req->ori_url) + pmatch[4].rm_so, 1);
		version				= vs_allocate(tmp_mpool, pmatch[5].rm_eo - pmatch[5].rm_so, vs_data(req->ori_url) + pmatch[5].rm_so, 1);
		contents			= vs_allocate(tmp_mpool, pmatch[6].rm_eo - pmatch[6].rm_so, vs_data(req->ori_url) + pmatch[6].rm_so, 1);
		virtualfile			= vs_allocate(tmp_mpool, pmatch[7].rm_eo - pmatch[7].rm_so, vs_data(req->ori_url) + pmatch[7].rm_so, 1);
		argument			= vs_allocate(tmp_mpool, pmatch[8].rm_eo - pmatch[8].rm_so, vs_data(req->ori_url) + pmatch[8].rm_so, 1);
#else
		contents			= vs_allocate(tmp_mpool, pmatch[1].rm_eo - pmatch[1].rm_so, vs_data(req->ori_url) + pmatch[1].rm_so, 1);
		virtualfile			= vs_allocate(tmp_mpool, pmatch[2].rm_eo - pmatch[2].rm_so, vs_data(req->ori_url) + pmatch[2].rm_so, 1);
		argument			= vs_allocate(tmp_mpool, pmatch[3].rm_eo - pmatch[3].rm_so, vs_data(req->ori_url) + pmatch[3].rm_so, 1);
#endif
#if 1
#ifdef DEBUG
#ifdef ZIPPER
		printf("application = %s\n", vs_data(application));
		printf("instance = %s\n", vs_data(instance));
		printf("protocol = %s\n", vs_data(protocol));
		printf("lang = %s\n", vs_data(lang));
		printf("version = %s\n", vs_data(version));
#endif
		printf("contents = %s\n", vs_data(contents));
		printf("virtualfile = %s\n", vs_data(virtualfile));
		printf("argument = %s\n", vs_data(argument));
#endif
#endif	//endof #if 0
		streaming = strm_create_streaming(req);


#ifdef ZIPPER
		if (vs_length(lang) != 3) {	/* language는 3글자이어야 함. 예:eng,kor,jpn */
				scx_error_log(req, "Invalid language code(%s), url(%s)\n", vs_data(lang), vs_data(req->ori_url));
			goto strm_url_parser_error;
		}

#if 0
		/* zipper인 경우에 host를 알아내는 부분을 strm_url_parser()가 호출 되기 이전에 따로 하는 방식으로 변경 되었기 때문에 이부분은 skip 한다. */
		/* streaming 기능을 사용할때에는 application에 들어오는 값을 host로 사용한다.
		 * 이후에 host가 아닌 순수한 service 개념이 생기면 service로 변경 가능 */
		req->host = vs_allocate(req->pool, vs_length(application), vs_data(application), 1);
#endif
		streaming->media_mode = strm_protocol_parser(req, vs_data(protocol));
		if (streaming->media_mode == 0) {	/* parsing 오류 */
			scx_error_log(req, "Invalid protocol(%s), url(%s)\n", vs_data(protocol), vs_data(req->ori_url));
			goto strm_url_parser_error;
		}
		streaming->lang = (char *) mp_alloc(streaming->mpool,vs_length(lang)+1);
		sprintf(streaming->lang , "%s", vs_data(lang));
		streaming->instance = (char *) mp_alloc(streaming->mpool,vs_length(instance)+1);
		sprintf(streaming->instance , "%s", vs_data(instance));
#else
		streaming->media_mode = O_STRM_TYPE_SINGLE;	/* cache에서의 streaming은 single 모드(한개의 파일)만 지원한다. */
#endif

		ret = strm_media_type_parser(req, vs_data(virtualfile));
		if (ret == 0) {	/* parsing 오류 */
			goto strm_url_parser_error;
		}

		if (vs_length(argument) > 0 ) {
			streaming->argument = (char *) mp_alloc(streaming->mpool, vs_length(argument)+1);
			sprintf(streaming->argument , "%s", vs_data(argument));
			if ( (O_STRM_TYPE_SINGLE & streaming->media_mode ) != 0) {
				/*
				 * start, end, duration query parameter는 single 모드(smil을 사용한 ataptive포함)에서만 동작한다.
				 */
				var = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, ARGUMENT_NAME_START);
				if (var) {
					if (MEDIA_TYPE_FLV == streaming->media_type) {
						/* FLV에서는 pseudo streaming시 micro second로 들어오는 경우와 second로 들어오는 경우가 있다 */
						if (atoll(var) > 1000000) {	/* 1000000 보다 큰수가 들어오면 micro second라고 판단하고 milisecond로 바꾼다 */
							arg_start = atoll(var) / 1000;
						}
						else {
							arg_start = (int)(atof(var) * 1000.0);
						}
					}
					else {
						arg_start = (int)(atof(var) * 1000.0);
					}
					if (arg_start < 0) arg_start = 0;

				}
				var = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, ARGUMENT_NAME_END);
				if (var) {
					if (MEDIA_TYPE_FLV == streaming->media_type) {
						/* FLV에서는 pseudo streaming시 micro second로 들어오는 경우와 second로 들어오는 경우가 있다 */
						if (atoll(var) > 1000000) {	/* 1000000 보다 큰수가 들어오면 micro second라고 판단하고 milisecond로 바꾼다 */
							arg_end = atoll(var) / 1000;
						}
						else {
							arg_end = (int)(atof(var) * 1000.0);
						}
					}
					else {
						arg_end = (int)(atof(var) * 1000.0);
					}
					if (arg_end < 0) arg_end = 0;
				}
				var = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, ARGUMENT_NAME_DURATION);
				if (var) {
					arg_dur = (int)(atof(var) * 1000.0);
					if (arg_dur < 0) arg_dur = 0;
				}

				if (arg_start > 0 || arg_end > 0 || arg_dur > 0
						|| soljwt_check_streaming_claim(req) == SCX_YES) {	// jwt claim에 start나 duration이 있는 경우 가상파일명을 사용하지 않는 경우에도 미리보기가 가능하도록 한다. */
					if (arg_start >= arg_end && arg_end != 0) {
						/* 비정상 요청으로 보고 start와 end argument를 모두 무시한다 */
						arg_start = 0;
						arg_end = 0;
					}
					if(arg_dur > 0)	{
						arg_end = arg_start + arg_dur;
					}

					/*
					 * 아래는 가상 파일명을 사용하지 않고 pseudo streaming이나 미리보기를 하는 경우에 해당됨
					 * 이때는 media_type과 build_type을 변경해야함.
					 * 가상 파일명을 사용하지 않기 때문에 가상 파일명도 media_path에 포함 시켜야 한다.
					 * 현재 SolProxy에서는 가상파일명 없이는 스트리밍 미리보기를 지원하지 않고 있기 때문에 아래로 들어 오지는 않는다.
					 */
					if (MEDIA_TYPE_MP4_DOWN == streaming->media_type ||
							MEDIA_TYPE_M4A_DOWN == streaming->media_type ||
							MEDIA_TYPE_M4V_DOWN == streaming->media_type ||
							MEDIA_TYPE_MP3_DOWN == streaming->media_type ||
							MEDIA_TYPE_FLAC_DOWN == streaming->media_type  ) {
						switch(streaming->media_type) {
						case MEDIA_TYPE_MP4_DOWN:
							streaming->media_type = MEDIA_TYPE_MP4;
							streaming->build_type = BLDTYPE_MP4;
							break;
						case MEDIA_TYPE_M4A_DOWN:
							streaming->media_type = MEDIA_TYPE_M4A;
							streaming->build_type = BLDTYPE_MP4;
							break;
						case MEDIA_TYPE_M4V_DOWN:
							streaming->media_type = MEDIA_TYPE_M4V;
							streaming->build_type = BLDTYPE_MP4;
							break;
						case MEDIA_TYPE_MP3_DOWN:
							streaming->media_type = MEDIA_TYPE_MP3;
							streaming->build_type = BLDTYPE_MP3;
							break;
						case MEDIA_TYPE_FLAC_DOWN:
							streaming->media_type = MEDIA_TYPE_FLAC;
							streaming->build_type = BLDTYPE_FLAC;
							break;
						default :
							break;
						}
						/* 가상 파일 경로를 포함해서 path를 다시 만든다. */
						tmp_len = vs_length(contents) + vs_length(virtualfile)+2;
						tmp_str = mp_alloc(tmp_mpool, tmp_len);
						snprintf(tmp_str, tmp_len, "%s/%s", vs_data(contents), vs_data(virtualfile));
						contents = vs_allocate(tmp_mpool,strlen(tmp_str), tmp_str, 0);
						is_without_virtualfile_streaming = 1;
#ifdef DEBUG
//						printf("contents path = %s\n", vs_data(contents));
#endif
					}
				}
			} // end of if ( (O_STRM_TYPE_SINGLE & streaming->media_mode ) != 0)

			if ((streaming->protocol & O_PROTOCOL_DASH) != 0
					&& BLDTYPE_FMP4SINGLE == streaming->build_type) {
				/* youtube 방식 DASH(단일 Fragmented MP4 + Range-Get 기반)인 경우 플레이어(CastLabs player)에 따라
				 * range 요청에 헤더에 있지 않고 argument에 bytes파라미터로 들어 오는 경우가 있음
				 * 이때 응답은 206으로 나가야한다.
				 * 예) http://image.test.com/image.test.com/_pc_/multi/eng/0/2f6d70342f4e616d752e30312e6d7034/0-0-10000/2f6d70342f4e616d752e30312e6d7034/0-200-/0_audio_single.m4s?solsessionid=1401685170?bytes=608-10095
				 */
				var = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, ARGUMENT_NAME_BYTES);
				if (var) {
					/* request header의 range 처리로직에 넘기기 위해서 아래처럼 형식을 맞춘다. */
					tmp_len = strlen("bytes=") + strlen(var) + 1;
					tmp_str = mp_alloc(tmp_mpool, tmp_len);
					snprintf(tmp_str, tmp_len,"bytes=%s",var);
					bytes = vs_allocate(tmp_mpool,strlen(tmp_str), tmp_str, 0);
					hdr_req_range(NULL, bytes, 0, req, NULL, 0);
				}
			}
		}

#ifdef ZIPPER
		streaming->real_path_len = pmatch[6].rm_eo + 1; /* application부터 meta까지의 길이 */

		/* key는 lang+content로 한다. 세션 생성시 사용 */
		streaming->key = (char *) mp_alloc(streaming->mpool,
				vs_length(lang) + vs_length(contents) + 1);
		sprintf(streaming->key , "%s%s", vs_data(lang),vs_data(contents));
#else
		if (MEDIA_TYPE_MP4_DOWN == streaming->media_type ||
			MEDIA_TYPE_M4A_DOWN == streaming->media_type ||
			MEDIA_TYPE_M4V_DOWN == streaming->media_type ||
			MEDIA_TYPE_MP3_DOWN == streaming->media_type ||
			MEDIA_TYPE_FLAC_DOWN == streaming->media_type) {
			/* cache에서 streaming enable로 동작하는 경우에 원본 컨텐츠를 바로 받을때는 원래의 cache쪽으로 보내기 위해 여기서 바로 리턴한다. */
			goto strm_url_parser_error;
		}

		if (is_without_virtualfile_streaming == 0) {	// 가상 파일 없이 스트리밍 하는 경우는 real_path_len을 설정하지 않아야 컨텐츠 통계에 전체 경로가 표시된다. */
			streaming->real_path_len = pmatch[1].rm_eo + 1; /* 실제 파일 path */
		}

		/* key는 content로 한다. 세션 생성시 사용 */
		streaming->key = (char *) mp_alloc(streaming->mpool, vs_length(contents) + 1);
		sprintf(streaming->key , "%s", vs_data(contents));
#endif
		strlen(streaming->key);	//key가 깨지버그 확인 목적으로 임시 코딩, 나중에 문제가 없으면 이 라인은 삭제 해도 된다. 관련일감:https://jarvis.solbox.com/redmine/issues/32810
		streaming->bprm = (zipper_builder_param *) mp_alloc(streaming->mpool,sizeof(zipper_builder_param));

#ifdef ZIPPER
		/*
		 * 원본 동영상을 cache해서 단순히 전달만 하는 경우
		 * 이때는 가상 파일이 없으므로 virtualfile을 파일 경로에 다시 붙인다.
		 */
		if ( ((streaming->media_mode & O_STRM_TYPE_SINGLE) != 0) &&
				(streaming->media_type == MEDIA_TYPE_MP4_DOWN ||
						streaming->media_type == MEDIA_TYPE_MP3_DOWN ||
						streaming->media_type == MEDIA_TYPE_FLAC_DOWN ||
						streaming->media_type == MEDIA_TYPE_SMIL_DOWN || streaming->media_type == MEDIA_TYPE_SUBTITLE_DOWN) ){
				/* '/vod/_definst_/pdl/eng/0/dir1/dir2/sample.mp4'에서와 같이 단순 다운로드인 경우를 판별 한다. */
			streaming->media_mode |= O_STRM_TYPE_DIRECT;
			streaming->media_path = (char *) mp_alloc(streaming->mpool,vs_length(contents)+vs_length(virtualfile)+2);
			sprintf(streaming->media_path , "%s/%s", vs_data(contents), vs_data(virtualfile));
#ifdef DEBUG
			printf("contents path = %s\n", streaming->media_path);
#endif
		}
		else {
#else
		{
#endif

			if ( vs_length(contents) < 3 ) {
				/* /vod/_definst_/single/eng/1234//content.mp4와 같이 contents 항목에 빈 파일명이 들어 오는 경우 에러를 리턴한다.
				 * origin의 파일을 직접 다운로드 하는 O_STRM_TYPE_DIRECT의 경우는 contents에 경로가 없어도
				 * 파일명만으로 받을수 있기 때문에 예외임 */
				scx_error_log(req, "Empty contents path.(%s)\n", vs_data(req->ori_url));
				goto strm_url_parser_error;
			}
			ret = strm_content_parser(req, vs_data(contents), tmp_mpool);
			if (ret == 0) {	/* parsing 오류 */
				goto strm_url_parser_error;
			}
#ifdef ZIPPER
			if (streaming->live_path != NULL) {
				/* live ad stitching 기능으로 동작 하는 경우 오리진에 요청할 path를 만든다*/
				switch (streaming->media_type) {
				case MEDIA_TYPE_HLS_MAIN_M3U8:
					/* client로부터 master manifest(playlist.m3u8)이 요청된 경우는 request url의 live content path에 있는 경로 그대로 요청하면 됨 */
					streaming->live_origin_path =  streaming->live_path;
					break;
				case MEDIA_TYPE_HLS_SUB_M3U8:
				case MEDIA_TYPE_HLS_TS:
					/* sub manifest나 ts요청시 원본의 경로는 req->streaming->live_path에서 req->streaming->live_real_path_len까지와 가상 파일명을 붙이면 된다.*/
					tmp_len = streaming->live_real_path_len + vs_length(virtualfile) + 1;
					streaming->live_origin_path = (char *) mp_alloc(streaming->mpool, tmp_len);
					memcpy(streaming->live_origin_path, streaming->live_path, streaming->live_real_path_len);
					memcpy(streaming->live_origin_path + streaming->live_real_path_len, vs_data(virtualfile), vs_length(virtualfile));
					break;
				default :
					scx_error_log(req, "live virtual file type missmatch.(%s)\n", vs_data(req->ori_url));
					goto strm_url_parser_error;
				}
//				printf("origin path = %s\n", streaming->live_origin_path);

			}
#endif

			if (arg_start > 0 || arg_end > 0) {
				if ( (O_STRM_TYPE_SINGLE & streaming->media_mode) != 0) {
					streaming->content->start = arg_start;
					if (arg_end > 0) {
						streaming->content->end = arg_end;
					}
				}
				else if( (O_STRM_TYPE_ADAPTIVE & streaming->media_mode)  != 0) {
					/*
					 * 여기에 들어 오는 경우는
					 * Zipper의 Adaptive mode로 요청이 들어온게 아니라
					 * single 모드나 SolProxy에서 smil 파일로 요청이 들어와서 adaptive mode로 변경된 경우이다.
					 */
					streaming->adaptive_info->start = arg_start;
					if (arg_end > 0) {
						streaming->adaptive_info->end = arg_end;
					}
				}
			}
			/*
			 * content.mp4 요청시에 원본 컨텐츠를 변환 없이 내보내는 기능
			 * https://jarvis.solbox.com/redmine/issues/32999
			 */
			if (req->service->progressive_convert_enable == 0
					&& streaming->content != NULL) {

				if (streaming->content->next == NULL
						&& streaming->content->start== 0 &&
						streaming->content->end == EOF) {
					/* content가 한개만 있고 미리보기 요청이 아닌 경우만 해당 */
					if(streaming->media_type == MEDIA_TYPE_MP4
							|| streaming->media_type == MEDIA_TYPE_M4A
							|| streaming->media_type == MEDIA_TYPE_M4V
							|| streaming->media_type == MEDIA_TYPE_MP3
							|| streaming->media_type == MEDIA_TYPE_FLAC) {
						streaming->media_mode = O_STRM_TYPE_SINGLE | O_STRM_TYPE_DIRECT;
						streaming->media_path = streaming->content->path;
						streaming->content = NULL;
						switch(streaming->media_type) {
						case MEDIA_TYPE_MP4 :
							streaming->media_type = MEDIA_TYPE_MP4_DOWN;
							break;
						case MEDIA_TYPE_M4A :
							streaming->media_type = MEDIA_TYPE_M4A_DOWN;
							break;
						case MEDIA_TYPE_M4V :
							streaming->media_type = MEDIA_TYPE_M4V_DOWN;
							break;
						case MEDIA_TYPE_MP3 :
							streaming->media_type = MEDIA_TYPE_MP3_DOWN;
							break;
						case MEDIA_TYPE_FLAC :
							streaming->media_type = MEDIA_TYPE_FLAC_DOWN;
							break;
						default :
							break;
						}
					}

				}
			}
		}
	}
	else {
#ifdef ZIPPER	/* zipper가 아닌 경우는 일반 요청도 이쪽으로 들어 오기 때문에 에러 로그로 기록 할 필요는 없다 */
	    error_len = regerror (ret, gscx__strm_url_preg, NULL, 0);
	    error_str = (char *) mp_alloc(tmp_mpool, error_len);
	    regerror (ret, gscx__strm_url_preg, error_str, error_len);
		scx_error_log(req, "base url pattern not matched, url(%s), reason(%s)\n", vs_data(req->ori_url), error_str);
#endif
		goto strm_url_parser_error;
	}


strm_url_parser_success:
	if (tmp_mpool) mp_free(tmp_mpool);
	return 1;
strm_url_parser_error:
	if(req->streaming) strm_destroy_streaming(req);

	if (tmp_mpool) mp_free(tmp_mpool);
	return 0;
}

/*
 * 요청 URL에 포함된 확장자가 streaming_media_ext에 지정된 확장자와 동일한 경우 streaming 컨텐츠로 인식하도록 한다.
 * https://jarvis.solbox.com/redmine/issues/33317
 */
int
strm_check_streaming_media_ext(nc_request_t *req)
{
	int 	ret = 0;
	char 	*ext_list;
	char 	*poff = NULL;
	int 	conta = 0;
	char	*ext;
	int		ext_len = 0;
	vstring_t 		*v_token = NULL;
	streaming_t *streaming = 0;

	if (NULL != req->streaming) {
		/* req->streaming()이 할당 되어 있는 경우는 이미 streaming 컨텐츠로 인식했기 때문에 아래의 과정이 필요 없음 */
		return 1;
	}

	if (req->ext == NULL || req->service->streaming_media_ext == NULL) {
		return 1;
	}

	/* 대소문자 구분 없이 비교한다. */
	ext = vs_data(req->ext);
	ext_len = strlen(ext);
	ext_list = vs_data(req->service->streaming_media_ext);
	if (ext_list && ext > 0) {
		poff = strcasestr(ext_list, ext);
		if (poff) {
			conta = (*(poff+ext_len) == '\0' ||
					*(poff+ext_len) == ' ' ||
					*(poff+ext_len)== ';' ||
					*(poff+ext_len) == '\t' ||
					*(poff+ext_len) == ',') ;
		}

	}
	if (conta != 1) {
		return 0;
	}
	streaming = strm_create_streaming(req);
	streaming->media_type = MEDIA_TYPE_DOWN;
	streaming->protocol = O_PROTOCOL_HLS;	/* sesson 기능이 동작하려면 protocol이 HLS아 DASH 여야 한다 */
	streaming->build_type = BLDTYPE_NONE;
	streaming->media_mode = O_STRM_TYPE_SINGLE | O_STRM_TYPE_DIRECT;

	/* key는 session 생성시 사용된다. key에 경로를 넣게 되면 ts의 경로가 현재의 경로와 틀린 경우 에러 처리가 될수 있기 때문에 key에는 고정 값을 넣는다. */
	streaming->key = (char *) mp_alloc(streaming->mpool, 10 );
	sprintf(streaming->key,  "fixed_key");
	/* streaming인 경우 media_path에 지정된 경로로 파일을 open 한다 */
	scx_update_ignore_query_url(req);
	streaming->media_path = (char *) mp_alloc(streaming->mpool,vs_length(req->ori_url)+1);
	/* session 처리를 위해서 streaming->real_path_len도 업데이트 해야함 */
	streaming->real_path_len = vs_pickup_token_r(NULL, req->ori_url, vs_length(req->ori_url), "/", &v_token, DIRECTION_RIGHT); /* 여기서는 v_token과 DIRECTION_RIGHT은 의미 없이 형식만 맞춰주기 위함임 */
	if (streaming->real_path_len > 0) streaming->real_path_len++;	/* 위의 리턴값음 position이라서 length로 변환을 하려면 +1을 해 주어야 한다. */
	sprintf(streaming->media_path , "%s", vs_data(req->ori_url));
#ifdef DEBUG
	printf("streaming->media_path = '%s', real_path_len = %d\n", streaming->media_path, streaming->real_path_len);
#endif

	req->streaming = streaming;
	return 0;
}

/*
 *
 */
mode_t
strm_protocol_parser(nc_request_t *req, char *protocol)
{
	mode_t			media_mode = 0;
	int ret = 0;
	streaming_t *streaming = req->streaming;

	TRC_BEGIN((__func__));

	if(strcasecmp(protocol, "single")  == 0) {
		media_mode = O_STRM_TYPE_SINGLE;
	}
	else if (strcasecmp(protocol, "multi")  == 0){
		media_mode = O_STRM_TYPE_MULTI;
	}
	else if (strcasecmp(protocol, "adaptive")  == 0){
		media_mode = O_STRM_TYPE_ADAPTIVE;
	}

	TRC_END;
	return media_mode;
}
/*
 * contents에는 protocol이 single인 경우는 실제 origin의 path가오고
 * multi나 adaptive인 경우는 "/2f696e666f2f7072655f373230702e6d7034/0-0-0-706172616d313d76616c75653126706172616d323d76616c756532/2f61642f666c7962655f373230702ee6d7023/0-0-0"
 * (/content path/type-start-end-encoded argument)의 형태로 들어 온다.
  * 다른 형태의 값이 들어 오면 아래 부분을 새로 만들어야함.
 * connection 연결이 되어 있는 동안 계속 사용할 정보에는 req->streaming->mpool을 사용해 메모리를 할당 해야 한다.
 * strm_url_parser()를 벋어나면 사용 하지 않을 변수에는 mpool에서 할당 받는다.
 */
int
strm_content_parser(nc_request_t *req, char *raw_contents, void *tmp_mpool)
{
	streaming_t *streaming = req->streaming;
	content_info_t 	*content = NULL;
	adaptive_info_t *adaptive = NULL;
	size_t     nmatch = 10;
	regmatch_t pmatch[10];
	int 		ret = 0;
	vstring_t	*path = NULL;
	vstring_t 	*type = NULL;
	vstring_t 	*start = NULL;
	vstring_t 	*end = NULL;
	vstring_t 	*argument = NULL;
	const int	max_decode_size = 1024;
	int 		count = 0;
	char *decoded = NULL;
	int			pathlen = 0;
	int			n_type = 0;
	int			i;


	decoded = (char *)mp_alloc(tmp_mpool,max_decode_size);
	ASSERT(decoded);


	if ((streaming->media_mode & O_STRM_TYPE_SINGLE) != 0) {

		if(strcasestr(raw_contents+(strlen(raw_contents) - 5), ".smil") != NULL) {
			//smil 파일을 사용하는 경우는 single mode로 들어 오더라도 adaptive mode 로 바꾼다.
			streaming->media_mode = O_STRM_TYPE_ADAPTIVE;
			ASSERT(streaming->adaptive_info == NULL);
			adaptive = (adaptive_info_t *)mp_alloc(streaming->mpool,sizeof(adaptive_info_t));
			ASSERT(adaptive);
			streaming->adaptive_info = adaptive;

			adaptive->type = O_CONTENT_TYPE_VOD;


			pathlen = strlen(raw_contents);
			adaptive->path = (char *) mp_alloc(streaming->mpool,pathlen+1);
			strncpy(adaptive->path, raw_contents, pathlen);

			adaptive->is_smil = 1;
			adaptive->end = EOF;

		}
		else {
			content = (content_info_t *)mp_alloc(streaming->mpool,sizeof(content_info_t));
			ASSERT(content);
			streaming->content = content;
			content->path = (char *) mp_alloc(streaming->mpool,strlen(raw_contents)+1);
			sprintf(content->path, "%s", raw_contents);
			content->start = 0;
			content->end = EOF;
		}

	}
	else if ((streaming->media_mode & O_STRM_TYPE_MULTI) != 0) {
		while (1) {
			ret = regexec(gscx__strm_content_preg, raw_contents, nmatch, pmatch, 0);
			if (ret == 0) { /* 패턴 일치 */
				path	= vs_allocate(tmp_mpool, pmatch[1].rm_eo - pmatch[1].rm_so, raw_contents + pmatch[1].rm_so, 1);
				type	= vs_allocate(tmp_mpool, pmatch[2].rm_eo - pmatch[2].rm_so, raw_contents + pmatch[2].rm_so, 1);
				start	= vs_allocate(tmp_mpool, pmatch[3].rm_eo - pmatch[3].rm_so, raw_contents + pmatch[3].rm_so, 1);
				end		= vs_allocate(tmp_mpool, pmatch[4].rm_eo - pmatch[4].rm_so, raw_contents + pmatch[4].rm_so, 1);
				argument= vs_allocate(tmp_mpool, pmatch[5].rm_eo - pmatch[5].rm_so, raw_contents + pmatch[5].rm_so, 1);
#ifdef DEBUG
				printf("path = %s, start = %s, end = %s, arg = %s\n", vs_data(path), vs_data(start), vs_data(end), vs_data(argument));
#endif
				raw_contents =  raw_contents + pmatch[6].rm_so;

				hex2ascii(decoded, vs_data(path), max_decode_size);

				switch( atoi(vs_data(type)) ) {
				case 0:
					n_type = O_CONTENT_TYPE_VOD;
					break;
				case 1:
					n_type = O_CONTENT_TYPE_ADVER;
					break;
				case 2:
					n_type = O_CONTENT_TYPE_LIVE;
					break;
				default :
					scx_error_log(req, "Unsupported Meta type(%s), url(%s)\n", vs_data(type), vs_data(req->url));
					return 0;
				}
				if (n_type == O_CONTENT_TYPE_LIVE) {
					streaming->live_path = (char *) mp_alloc(streaming->mpool,strlen(decoded)+1);
					sprintf(streaming->live_path, "%s", decoded);
					/* 원본 live 경로에서 가상파일을 뺀 유효 path의 길이를 찾는다. */
					for(i = strlen(streaming->live_path) - 1; i > 0; i--) {
						if(streaming->live_path[i] == '/') {
							streaming->live_real_path_len = i+1;
							break;
						}
					}
					/*
					 * req->streaming->live_path에 원본 master manifest 의 경로가 들어 있음
					 * sub manifest나 ts요청시 원본의 경로는 req->streaming->live_path에서 req->streaming->live_real_path_len까지와 가상 파일명을 붙이면 된다.
					 */
					/* live origin의 path인 경우는 live ad stitching은 post 광고만 지원하므로 live path가 나오는 경우 마지막이라고 보고 종료한다. */
					break; //end of while loop
				}

				if (streaming->content == NULL) {
					content = (content_info_t *)mp_alloc(streaming->mpool,sizeof(content_info_t));
					ASSERT(content);
					streaming->content = content;
				}
				else {
					content->next = (content_info_t *)mp_alloc(streaming->mpool,sizeof(content_info_t));
					ASSERT(content->next);
					content = content->next;
				}

				content->type = n_type;

				if (content->path == NULL) {
					content->path = (char *) mp_alloc(streaming->mpool,strlen(decoded)+1);
					sprintf(content->path, "%s", decoded);

				}

				if (vs_length(start) == 0) {
					content->start = 0;
				}
				else {
					content->start = atoi(vs_data(start));
				}

				if (vs_length(end) == 0) {
					/* 미디어의 종료 시간이 지정 되지 않은 경우는 끝까지(EOF) 재생한다. */
					content->end = EOF;
				}
				else {
					content->end = atoi(vs_data(end));
				}
				if (content->end == 0) {
					content->end = EOF;
				}
#ifdef DEBUG
				printf("path = %s, start = %d, end = %d\n", content->path, content->start, content->end);
#endif
			}
			else {
				scx_error_log(req, "Invalid multi content pattern(%s), url(%s)\n", raw_contents, vs_data(req->url));
				return 0;
			}
			if (count++ > 40) {
				/* 무한 루프 방지용, content의 최대 개수는 40이하로 제한 */
				scx_error_log(req, "Contents parsing excess max loop, url(%s)\n", vs_data(req->url));
				return 0;
			}
			/* 경우에 따라 아래의 두가지 경우가 모두 발생해서 다음과 같이 처리함 */
			if (raw_contents == NULL) {
				break;
			}
			if (*raw_contents == NULL) {
				break;
			}
		}
	}
	else if ((streaming->media_mode & O_STRM_TYPE_ADAPTIVE) != 0) {
		//		streaming->is_adaptive = 1;	/* adaptive mode로 들어 오는 요청은 모두 adaptive라고 판단한다. */
		while (1) {
			ret = regexec(gscx__strm_content_preg, raw_contents, nmatch, pmatch, 0);
			if (ret == 0) { /* 패턴 일치 */
				path	= vs_allocate(tmp_mpool, pmatch[1].rm_eo - pmatch[1].rm_so, raw_contents + pmatch[1].rm_so, 1);
				type	= vs_allocate(tmp_mpool, pmatch[2].rm_eo - pmatch[2].rm_so, raw_contents + pmatch[2].rm_so, 1);
				start	= vs_allocate(tmp_mpool, pmatch[3].rm_eo - pmatch[3].rm_so, raw_contents + pmatch[3].rm_so, 1);
				end		= vs_allocate(tmp_mpool, pmatch[4].rm_eo - pmatch[4].rm_so, raw_contents + pmatch[4].rm_so, 1);
				argument= vs_allocate(tmp_mpool, pmatch[5].rm_eo - pmatch[5].rm_so, raw_contents + pmatch[5].rm_so, 1);
#ifdef DEBUG
				printf("path = %s, start = %s, end = %s, arg = %s\n", vs_data(path), vs_data(start), vs_data(end), vs_data(argument));
#endif
				raw_contents = raw_contents + pmatch[6].rm_so;

				hex2ascii(decoded, vs_data(path), max_decode_size);

				switch( atoi(vs_data(type)) ) {
				case 0:
					n_type = O_CONTENT_TYPE_VOD;
					break;
				case 1:
					n_type = O_CONTENT_TYPE_ADVER;
					break;
				case 2:
					n_type = O_CONTENT_TYPE_LIVE;
					break;
				default :
					scx_error_log(req, "Unsupported Meta type(%s), url(%s)\n", vs_data(type), vs_data(req->url));
					return 0;
				}
				if (n_type == O_CONTENT_TYPE_LIVE) {
					streaming->live_path = (char *) mp_alloc(streaming->mpool,strlen(decoded)+1);
					sprintf(streaming->live_path, "%s", decoded);
					/* 원본 live 경로에서 가상파일을 뺀 유효 path의 길이를 찾는다. */
					for(i = strlen(streaming->live_path) - 1; i > 0; i--) {
						if(streaming->live_path[i] == '/') {

							streaming->live_real_path_len = i+1;
							break;
						}
					}
					/* live origin의 path인 경우는 live ad stitching은 post 광고만 지원하므로 live path가 나오는 경우 마지막이라고 보고 종료한다. */
					break;
				}
				if (streaming->adaptive_info == NULL) {
					adaptive = (adaptive_info_t *)mp_alloc(streaming->mpool,sizeof(adaptive_info_t));
					ASSERT(adaptive);
					streaming->adaptive_info = adaptive;
				}
				else {
					adaptive->next = (adaptive_info_t *)mp_alloc(streaming->mpool,sizeof(adaptive_info_t));
					ASSERT(adaptive->next);
					adaptive = adaptive->next;
				}

				adaptive->type = n_type;

				if (adaptive->path == NULL) {
					pathlen = strlen(decoded);
					adaptive->path = (char *) mp_alloc(streaming->mpool,pathlen+1);
					strncpy(adaptive->path, decoded, pathlen);
				}

				if(strcasestr(adaptive->path+(pathlen-5), ".smil") != NULL) {
					adaptive->is_smil = 1;
				}
				else {
					adaptive->is_smil = 0;
				}

				if (vs_length(start) == 0) {
					adaptive->start = 0;
				}
				else {
					adaptive->start = atoi(vs_data(start));
				}

				if (vs_length(end) == 0) {
					/* 미디어의 종료 시간이 지정 되지 않은 경우는 끝까지(EOF) 재생한다. */
					adaptive->end = EOF;
				}
				else {
					adaptive->end = atoi(vs_data(end));
				}
				if (adaptive->end == 0) {
					adaptive->end = EOF;
				}
#ifdef DEBUG
				printf("path = %s, start = %d, end = %d\n", adaptive->path, adaptive->start, adaptive->end);
#endif
			}
			else {
				scx_error_log(req, "Invalid adaptive content pattern(%s), url(%s)\n", raw_contents, vs_data(req->url));
				return 0;
			}
			if (count++ > 40) {
				/* 무한 루프 방지용, content의 최대 개수는 40이하로 제한 */
				scx_error_log(req, "Contents parsing excess max loop, url(%s)\n", vs_data(req->url));
				return 0;
			}
			/* 경우에 따라 아래의 두가지 경우가 모두 발생해서 다음과 같이 처리함 */
			if (raw_contents == NULL) {
				break;
			}
			if (*raw_contents == NULL) {
				break;
			}
		}
	}
	return 1;
}

/*
 * 미디어 파일의 type과 id등을 알아 낸다.
 */
int
strm_media_type_parser(nc_request_t *req, char *virtualfile)
{
	scx_media_type_t media_type = MEDIA_TYPE_NORMAL;
	int ret = 0;
	streaming_t *streaming = req->streaming;
	size_t     nmatch = 3;
	regmatch_t pmatch[3];
	char 	token[20];
#ifdef ZIPPER
	ASSERT(req->service);
	if (req->service->streaming_method == 1) {
		/* live ad stitching 으로 동작시 */
		for(int i = 0;i < gscx__count_strm_live_reg;i++) {
			ret = regexec(gscx__strm_live_reg[i].preg, virtualfile, nmatch, pmatch, 0);
			if(ret == 0) { /* 패턴 일치 */
				for(int j = 1; j < (gscx__strm_live_reg[i].count + 1); j++) {
					memset(token, 0, sizeof(token));
					strncpy(token, virtualfile + pmatch[j].rm_so, pmatch[j].rm_eo - pmatch[j].rm_so );
					if(j == 1 && token[0] != 0) {
						streaming->rep_id = (char *) mp_alloc(streaming->mpool,strlen(token)+1);
						sprintf(streaming->rep_id, "%s", token);
	//					printf("rep id = %s\n", streaming->rep_id);
					}
					else if(j == 2 && token != NULL) {
						streaming->ts_num = atoi(token);
	//					printf("ts_num = %d\n", streaming->ts_num);
					}
				}
				media_type = gscx__strm_live_reg[i].media_type;
				streaming->media_type = gscx__strm_live_reg[i].media_type;
				streaming->build_type = gscx__strm_live_reg[i].build_type;
				streaming->protocol = gscx__strm_live_reg[i].protocol;
				break;
			}
		}
	}
	else {
#else
	{
		if (req->service->hls_modify_manifest_enable == 1) {
			/*
			 * manifest modify 기능이 켜진 경우에는 여기서 처리하고 바로 리턴한다.
			 * 이 기능은 Zipper일때는 동작하지 않고 SolProxy일 경우만 동작해야 한다.
			 */

			goto strm_media_type_parser_done;
		}

#endif
		/* VOD로 동작시 */
		for(int i = 0;i < gscx__count_strm_vod_reg;i++) {
			ret = regexec(gscx__strm_vod_reg[i].preg, virtualfile, nmatch, pmatch, 0);
			if(ret == 0) { /* 패턴 일치 */
				for(int j = 1; j < (gscx__strm_vod_reg[i].count + 1); j++) {
					memset(token, 0, sizeof(token));
					strncpy(token, virtualfile + pmatch[j].rm_so, pmatch[j].rm_eo - pmatch[j].rm_so );
					if(j == 1 && token[0] != 0) {
						streaming->rep_id = (char *) mp_alloc(streaming->mpool,strlen(token)+1);
						sprintf(streaming->rep_id, "%s", token);
						streaming->rep_num = atoi(streaming->rep_id);
	//					printf("rep id = %s\n", streaming->rep_id);
					}
					else if(j == 2 && token != NULL) {
						streaming->ts_num = atoi(token);
	//					printf("ts_num = %d\n", streaming->ts_num);
					}
				}
				media_type = gscx__strm_vod_reg[i].media_type;
				streaming->media_type = gscx__strm_vod_reg[i].media_type;
				streaming->build_type = gscx__strm_vod_reg[i].build_type;
				streaming->protocol = gscx__strm_vod_reg[i].protocol;
				break;
			}
		}
	}

strm_media_type_parser_done:
	if (media_type == MEDIA_TYPE_NORMAL) {
#ifdef ZIPPER
		scx_error_log(req, "media type parsing error(%s), url(%s)\n", virtualfile, vs_data(req->url));
#endif
	}
	TRACE((T_DEBUG, "[%llu] (%s) media type = %d\n", req->id, __FUNCTION__, media_type));
	return media_type;
}

/*
 * hls_modify_manifest_ext에 지정되어 있는 확장자와 같은지 확인한다.
 * 비교시 확장자는 대소문자를 구분하지 않는다.
 */
int
strm_check_manifest(nc_request_t *req)
{
	streaming_t		*streaming = NULL;
	service_info_t *service = req->service;
	int 			ret = 0;
	int				query_off;	//query parameter의 시작 부분의 offset. ? 포함
	vstring_t 		*v_token = NULL;
	int 			ext_off	= 0;	/* url에 포함된 확장자의 예상 위치 */
	int				ext_len = vs_length(service->hls_modify_manifest_ext);

	if (NULL != req->streaming) {
		/* req->streaming()이 할당 되어 있는 경우는 strm_check_policy_file()이나 다른 스트리밍 타입으로 판단된 경우로 아래 부분을 진핼 할 필요가 없음 */
		return 1;
	}


	query_off = vs_pickup_token_r(NULL, req->ori_url, vs_length(req->ori_url), "?", &v_token, DIRECTION_RIGHT);
	if (query_off > 0) {
		/* 요청에 query가 포함된 경우 */
		ext_off = query_off-ext_len;
	}
	else {
		ext_off = vs_length(req->ori_url) - ext_len;
	}
	if (vs_length(req->ori_url) <= ext_off) {
		return 1;
	}

//	printf("ext = %s\n", vs_data(req->ori_url)+query_off-ext_len);
//	printf("arg = %s\n",vs_data(req->ori_url)+query_off+1);
	/* 요청 url의 확장자를 찾아서 지정된 확장자와 동일한지 비교 한다. */
	if (vs_strncasecmp_lg(service->hls_modify_manifest_ext, vs_data(req->ori_url)+ext_off, ext_len) == 0) {
		streaming = strm_create_streaming(req);
//		printf("query_off = %d\n", query_off);
		if (query_off != 0) { /* argument가 포함된 경우 path에 별도의 메모리를 할당한다. */
			/* streaming->argument 에는 ?을 포함한 파라미터가 들어간다. */
			streaming->argument = (char *) mp_alloc(streaming->mpool, vs_length(req->ori_url)-query_off+2);
			sprintf(streaming->argument , "%s", vs_data(req->ori_url)+query_off);
//			printf("streaming->argument = '%s'\n",streaming->argument);
		}
		 streaming->media_type = MEDIA_TYPE_MODIFY_HLS_MANIFEST;
		 streaming->protocol = O_PROTOCOL_HLS;
		 streaming->build_type = BLDTYPE_NONE;
		 streaming->media_mode = O_STRM_TYPE_DIRECT;
		/* key는 session 생성시 사용된다. key에 경로를 넣게 되면 ts의 경로가 현재의 경로와 틀린 경우 에러 처리가 될수 있기 때문에 key에는 고정 값을 넣는다. */
		streaming->key = (char *) mp_alloc(streaming->mpool, 10 );
		sprintf(streaming->key,  "fixed_key");
		req->streaming = streaming;
		/* manifest 응답에 절대 경로를 사용하기 위해서는  streaming->real_path_len에 값이 들어가 있어야 한다. */
		streaming->real_path_len = vs_pickup_token_r(NULL, req->ori_url, ext_off, "/", &v_token, DIRECTION_RIGHT); /* 여기서는 v_token과 DIRECTION_RIGHT은 의미 없이 형식만 맞춰주기 위함임 */
		if (streaming->real_path_len > 0) streaming->real_path_len++;	/* 위의 리턴값음 position이라서 length로 변환을 하려면 +1을 해 주어야 한다. */
		TRACE((T_DEBUG, "[%llu] resolved manifest type\n", req->id));
	}

strm_check_manifest_success:

	return 1;
strm_check_manifestr_error:


	if(req->streaming) strm_destroy_streaming(req);

	return 0;
}


int
strm_handle_adaptive(nc_request_t *req)
{
	streaming_t *streaming = req->streaming;
	int ret = 0;
	mem_pool_t 		*tmp_mpool = NULL;	/* 파싱에만 사용하는 임시용 메모리풀. 이 함수를 벗어 날때 반드시 해제 되어야한다. */
	adaptive_info_t *adaptive, *head_adaptive;
	content_info_t *content, *head_content;
	content_info_t *new_content;
	subtitle_info_t *subtitle, *new_subtitle;
	int smil_cnt = 0;	/* 요청중 smil 파일로 구성된게 몇개 인지 확인 용*/
	int	found = 0;
	int is_base = 0, sub_is_base = 0;
	int	order = 0;

	if((streaming->media_mode & O_STRM_TYPE_ADAPTIVE) == 0){
		/* adaptive mode가 아닌 경우 이 함수가 호출 되면 안됨. */
		TRACE((T_ERROR, "[%llu] (%s) invalid media mode(%X)\n", req->id, __FUNCTION__, streaming->media_mode));
		ASSERT(0);
	}
	if ((streaming->protocol & O_PROTOCOL_PROGRESSIVE) != 0) {
		scx_error_log(req, "Adaptive mode is not support progressive streaming.\n");
		goto strm_handle_adaptive_error;
	}
	tmp_mpool = mp_create(sizeof(mem_pool_t) + 10240);     //smil파일의 크기는 최대 10kB까지 허용한다.
	ASSERT(tmp_mpool);

	/*
	 * 여기쯤에서 multi smil 을 다루는 부분이 추가 되어야 한다.
	 * smil이 아닌 경우는 skip 하는 부분도 들어가야 함.
	 */

	adaptive = streaming->adaptive_info;
	while (adaptive) {
		if(adaptive->is_smil) {
			/* strm_handle_smil() 호출 */
			ret = strm_handle_smil(req, adaptive, tmp_mpool);
			if (1 != ret) {
				goto strm_handle_adaptive_error;
			}
			if (adaptive->audio_only_track_exist == 1) {
				/* audio 분리 track 인 경우  streaming->protocol에  marking을 한다. */
				streaming->protocol |= O_PROTOCOL_SEP_TRACK;
			}
			streaming->is_adaptive = 1; /* 한개라도 smil파일이 있는 경우는 정상적인 adaptive라고 판단한다. */
			smil_cnt++;
		}
		else if (!req->service->hls_permit_discontinuty){
			/* smil 파일이 아닌 mp3나 mp4가 지정된 경우에는 hls_permit_discontinuty가 1로 되어 있어야만 허용이 된다. */
			scx_error_log(req, "Specifying media files in adaptive mode is not supported.\n");
			goto strm_handle_adaptive_error;
		}
		adaptive = adaptive->next;
	}
	if (!streaming->is_adaptive) {
		/* adaptive mode에서 adaptive 응답이 불가능한 요청(예를 들어 모두 mp4 직접 지정인 경우)인 경우는 에러 처리 한다. */
		scx_error_log(req, "The adaptive mode must consist of one or more smil files.\n");
		goto strm_handle_adaptive_error;
	}
	/*
	 * 리스트 검사 및 content_info_t 생성
	 ** 첫번째 adaptive 리스트와 나머지 adaptive 리스트를 모두 검사 해서 리스트에 포함된 bitrate 정보가 같은게 없는 경우 해당 content는 제외
	 *** smil 이 아닌 직접 미디어를 지정 하는 경우는 비교 하지 않고 skip
	 *** 이때 첫번째 adaptive 리스트에 있는 content 들만 사용 가능/불가능을 marking 한후 이 기준으로 content_info_t를 생성 하면 될듯.
	 ** 리스트에 포함된 content list(bitrate 가 다른)가 몇개 인지 counting 해야함.
	 *** smil이 아닌 컨텐츠를 직접 지정하는 경우 counting 된 리스트 만큼 content_info_t를 반복 생성해야함.
	 **** 예를 들어 a.mp4와 b.smil(b_240.mp4, b_320.mp4, b_480.mp4로 구성)을 zipping하는 경우 b.smil이 3개로 구성 되어 있으므로 content_info_t 리스트 생성시 a.mp4도 3개의 리스트로 만들어야 한다.
	 */

	/* 첫 adaptive 리스트를 찾는다. */
	adaptive = streaming->adaptive_info;
	while (adaptive) {
		if(adaptive->is_smil) {
			head_adaptive = adaptive;
			break;
		}
		adaptive = adaptive->next;
	}
	/* 첫 adaptive 리스트를 모두 available 상태로 바꾼다. */
	content = head_adaptive->contents;
	while (content) {
		content->available = 1;
		content = content->next;
	}

	if (smil_cnt > 1) {
		/* 여러개의 smil 파일로 구성된 경우만 아래의 정합성 비교 과정을 거친다. */
		adaptive = head_adaptive->next;
		while (adaptive)  {
			if(adaptive->is_smil) {
				/* 양쪽 smil 파일에 동일한 system-bitrate를 가지는게 있는지 확인한다. */
				head_content = head_adaptive->contents;
				while (head_content) {
					if (head_content->available) {
						found = 0;
						content = adaptive->contents;
						while (content) {
							if (strcmp(head_content->bitrate, content->bitrate) == 0) {
								found = 1;
								break;
							}
							content = content->next;
						}
						if (found != 1) {
							/* 양쪽 smil파일에 동일한 system-bitrate가 없는 경우는 head_adaptive의 해당 bitrate를 사용 안함으로 체크 한다.*/
							head_content->available = 0;
						}
					}
					head_content = head_content->next;
				}
			}
			adaptive = adaptive->next;
		}
	}
	/* 여기까지 진행이 된 상태면 head_adaptive에 있는 contents에는 모든 smil 파일에 공통적으로 들어 있는 system-bitrate에 대해서만 available이 1로 체크가 되어 있다 */
	/* 첫 adaptive 리스트(head_adaptive)의 available content 수 만큼 각 미디어들의 content list를 만든다. */

	adaptive = streaming->adaptive_info;
	while (adaptive)  {
		head_content = head_adaptive->contents;
		is_base = 1;
		order = 0;
		while (head_content) {
			// 자막 content는 다른 부분에서 처리한다.
			if (head_content->available ) {
				/* 여기서 새로운 content를 생성 */
				/* base 가 1인 경우 content->is_base를 1로 설정한다. */
				if (streaming->content == NULL) {
					new_content = (content_info_t *)mp_alloc(streaming->mpool,sizeof(content_info_t));
					ASSERT(new_content);
					streaming->content = new_content;
				}
				else {
					new_content->next = (content_info_t *)mp_alloc(streaming->mpool,sizeof(content_info_t));
					ASSERT(new_content->next);
					new_content = new_content->next;
				}

				if (head_content->type != O_CONTENT_TYPE_SUBTITLE) {
					if (is_base) {
						/* 각 content의 첫 track인 경우 */
						new_content->is_base = 1;
					}
					if(adaptive->is_smil) {
						content = adaptive->contents;
						while (content) {
							if (strcmp(head_content->bitrate, content->bitrate) == 0) {
								new_content->path = (char *) mp_alloc(streaming->mpool,strlen(content->path)+1);
								ASSERT(new_content->path);
								sprintf(new_content->path, "%s", content->path);
								new_content->bitrate = (char *) mp_alloc(streaming->mpool,strlen(content->bitrate)+1);
								ASSERT(new_content->bitrate);
								sprintf(new_content->bitrate, "%s", content->bitrate);
								if (content->codecs != NULL ) {
									new_content->codecs =  mp_alloc(streaming->mpool, strlen(content->codecs)+1);
									sprintf(new_content->codecs, "%s", content->codecs);
								}
								if (content->resolution != NULL ) {
									new_content->resolution =  mp_alloc(streaming->mpool, strlen(content->resolution)+1);
									sprintf(new_content->resolution, "%s", content->resolution);
								}

								break;

							}
							content = content->next;
						}
					}
					else {
						/*
						 * smil이 아닌 mp4나 mp3 인 경우 여기서 생성한다.
						 * 이 경우 path에는 url에 있던 컨텐츠의 경로가 들어가고
						 * bitrate 항목에는 첫번째 smil 파일에 들어 있는것과 동일하게 들어간다.
						 * */
						new_content->path = (char *) mp_alloc(streaming->mpool,strlen(adaptive->path)+1);
						ASSERT(new_content->path);
						sprintf(new_content->path, "%s", adaptive->path);
						new_content->bitrate = (char *) mp_alloc(streaming->mpool,strlen(head_content->bitrate)+1);
						ASSERT(new_content->bitrate);
						sprintf(new_content->bitrate, "%s", head_content->bitrate);

					}
					is_base = 0;
					new_content->type = adaptive->type;
				}

				new_content->order = order++;
				new_content->start = adaptive->start;
				new_content->end = adaptive->end;

			}
			head_content = head_content->next;
		}
		//자막 처리 부분
		subtitle = head_adaptive->subtitle;
		sub_is_base = 1;
		while (subtitle) {
			/* base 가 1인 경우 content->is_base를 1로 설정한다. */
			if (streaming->subtitle == NULL) {
				new_subtitle = (subtitle_info_t *)mp_alloc(streaming->mpool,sizeof(subtitle_info_t));
				ASSERT(new_subtitle);
				streaming->subtitle = new_subtitle;
			}
			else {
				new_subtitle->next = (subtitle_info_t *)mp_alloc(streaming->mpool,sizeof(subtitle_info_t));
				ASSERT(new_subtitle->next);
				new_subtitle = new_subtitle->next;
			}
			if (sub_is_base) {
				/* 각 content의 첫 track인 경우 */
				new_subtitle->is_base = 1;
			}
			if (adaptive == streaming->adaptive_info) {
				/* 첫 smil 파일에 들어 있는 자막의 수만 count 한다. */
				streaming->subtitle_count++;
			}
			if (subtitle->path != NULL ) {
				new_subtitle->path =  mp_alloc(streaming->mpool, strlen(subtitle->path)+1);
				sprintf(new_subtitle->path, "%s", subtitle->path);
			}
			if (subtitle->subtitle_lang != NULL ) {
				new_subtitle->subtitle_lang =  mp_alloc(streaming->mpool, strlen(subtitle->subtitle_lang)+1);
				sprintf(new_subtitle->subtitle_lang, "%s", subtitle->subtitle_lang);
			}
			if (subtitle->subtitle_name != NULL ) {
				new_subtitle->subtitle_name =  mp_alloc(streaming->mpool, strlen(subtitle->subtitle_name)+1);
				sprintf(new_subtitle->subtitle_name, "%s", subtitle->subtitle_name);
			}

			new_subtitle->subtitle_type = subtitle->subtitle_type;
			new_subtitle->subtitle_order = subtitle->subtitle_order;
			new_subtitle->available = 1;
			new_subtitle->start = adaptive->start;
			new_subtitle->end = adaptive->end;
#ifdef DEBUG
			printf("name: '%s', type = %d, language: '%s', start: %d, end: %d, path: '%s'\n",
					new_subtitle->subtitle_name, new_subtitle->subtitle_type, (new_subtitle->subtitle_lang != NULL)?new_subtitle->subtitle_lang:"NULL",
					new_subtitle->start, new_subtitle->end, new_subtitle->path);
#endif
			sub_is_base = 0;
			subtitle = subtitle->next;
		}
		adaptive = adaptive->next;
	}

	if (tmp_mpool) mp_free(tmp_mpool);
	streaming->adaptive_info->contents = NULL;	/* 앞으로 이부분을 사용할 일은 없지만 혹시나 해서 NULL로 확실하게 초기화 한다. */
	return 1;
strm_handle_adaptive_error:
	if (tmp_mpool) mp_free(tmp_mpool);
	return 0;
}

/*
 * strm_handle_smil()호출시 adaptive_info의 contents를 제외한 모든 값이 채워져 있어야 한다.
 */
int
strm_handle_smil(nc_request_t *req, adaptive_info_t *adaptive_info, mem_pool_t *tmp_mpool)
{

	streaming_t *streaming = req->streaming;
	char *smil_buffer = NULL;
	int ret = 0;

	smil_buffer = strm_smil_read(req, adaptive_info, tmp_mpool);
	if (smil_buffer == NULL) {
		goto strm_handle_smil_error;
	}
	if  (sigsetjmp(__thread_jmp_buf, 1) == 0) {
		__thread_jmp_working = 1;
		ret = smil_parse(req, adaptive_info, smil_buffer, tmp_mpool);
		__thread_jmp_working = 0;
	}
	else {
		/* smil 파일 파싱 과정에서  문제로 SIGSEGV가 발생하는 경우 재기동 되지 않고 여기가 호출 된다. */
		ret = 0;
		TRACE((T_ERROR, "[%llu] smil_parse() SIGSEGV occured. path(%s). %s:%d\n", req->id, adaptive_info->path, __FILE__, __LINE__));
	}
	if (ret == 0) {
		goto strm_handle_smil_error;
	}
	return 1;
strm_handle_smil_error:
	return 0;
}

/*
 * origin 서버로 부터 smil 파일을 받아서 할당한 메모리 버퍼에 복사한후에 해당 버퍼를 리턴한다.
 */
char *
strm_smil_read(nc_request_t *req, adaptive_info_t *adaptive_info, void *tmp_mpool)
{
	streaming_t *streaming = req->streaming;
	struct nc_stat 		objstat;
	char *smil_buffer = NULL;
	service_info_t *service = req->service;

	ssize_t 		copied = 0;
	size_t 			toread = 0;
	off_t 			offset = 0;
	char			url_path[1024] = {'\0'};

	void *file = NULL;
	/* 확장자가 .smil이 아닌 경우 에러 처리한다. */
	if (NULL == strcasestr(adaptive_info->path, ".smil")){
		scx_error_log(req, "PATH[%s] is not smil file.\n", adaptive_info->path);
		req->p1_error = MHD_HTTP_BAD_REQUEST;
		goto strm_smil_read_error;
	}

	snprintf(url_path, 1024, "%s", adaptive_info->path);

	/* smil 파일을 range로 읽을 필요가 없다. */
	file = strm_file_open(req, adaptive_info->type, url_path, &objstat, O_RDONLY|O_NCX_NORANDOM);

	if (file == NULL) {
		scx_error_log(req, "PATH[%s] - smil open error (%d)\n", url_path, strm_file_errno(req));
		req->p1_error = MHD_HTTP_BAD_REQUEST;
		goto strm_smil_read_error;
	}
	/* smil 파일 크기가 10KB 이상이 될 가능성은 없으므로 10KB 이상인 파일을 에러 처리 한다. */
	if (objstat.st_size  > 10240) {
		scx_error_log(req, "PATH[%s] - smil file size(%d) too large.\n", url_path, objstat.st_size );
		req->p1_error = MHD_HTTP_BAD_REQUEST;
		goto strm_smil_read_error;
	}
	smil_buffer = mp_alloc(tmp_mpool, objstat.st_size);
	if (NULL == smil_buffer) {
		scx_error_log(req, "PATH[%s] - smil buffer allocation failed(size=%d)\n", url_path, objstat.st_size);
		req->p1_error = MHD_HTTP_BAD_REQUEST;
		goto strm_smil_read_error;
	}
	toread = objstat.st_size;
	while (toread > 0) {
		copied = strm_file_read(req, file, smil_buffer + offset, offset, toread);
		if (copied <= 0) {
			break;
		}
		offset += copied;
		toread -= copied;

	}
strm_smil_read_error:
	if (file) {
		strm_file_close(req, file);
	}
//	printf("smil = %s\n", smil_buffer);

	return smil_buffer;
}


int
strm_add_streaming_header(nc_request_t *req)
{
	char 		time_string[128];
	time_t 		utime;
	struct tm 	result;
	struct tm	*gmt_tm = NULL;
	int			ret = 0;
	int			len = 0;
	streaming_t 	*streaming = req->streaming;
#if 0  /* ETAG는 스트리밍 기능에서 당분간 사용하지 않는다. */
	if (req->objstat.st_devid[0] != 0 && isprint(req->objstat.st_devid[0])) {
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_ETAG, req->objstat.st_devid);
		TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "URL[%s], header => ('%s', '%s')\n", vs_data(req->url), MHD_HTTP_HEADER_ETAG, req->objstat.st_devid));
	}
#endif

	scx_add_cors_header(req);
	//MHD_add_response_header(req->response, MHD_HTTP_HEADER_CACHE_CONTROL, "max-age=86400, must-revalidate");

	if (req->service->streaming_method == 1 &&
			(streaming->media_type ==  MEDIA_TYPE_HLS_MAIN_M3U8 || streaming->media_type ==  MEDIA_TYPE_HLS_SUB_M3U8)) {
		/* live ad stiching 기능으로 설정이 되어 있고  Manifest file 인 경우 컨텐츠가 캐싱 되지 않도록 nocache 헤더를 추가한다.*/
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache, no-store, must-revalidate, max-age=0");
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_PRAGMA, "no-cache");
	}
	else if (streaming->session) {
		/* session 기능이 동작하는 경우에는 캐시가 되지 않도록 한다. */
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache, no-store, must-revalidate, max-age=0");
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_PRAGMA, "no-cache");
	}
	else if (streaming->media_type == MEDIA_TYPE_MODIFY_HLS_MANIFEST) {
		/* Manifest 수정시에는 live를 생각해서 max-age를 1초로 한다. nocache로 응답하는 경우 parent-edge 구조에서는 edge가 캐시하기 어려울수도 있다. */
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_CACHE_CONTROL, "max-age=1");
	}

	if ( req->objstat.st_mtime != 0) {
		utime 	= req->objstat.st_mtime;
#if 1	/* NetCache Core의 st_mtime 기록 방식의 변경으로 localtime_r()에서 gmtime_r() 변경됨. */
		gmt_tm 	= gmtime_r(&utime, &result);
#else
		gmt_tm 	= localtime_r(&utime, &result);
#endif
		if (gmt_tm) {
			gmt_tm->tm_isdst = -1;
			strftime(time_string, sizeof(time_string), GMT_DATE_FORMAT, gmt_tm);
			MHD_add_response_header(req->response, MHD_HTTP_HEADER_LAST_MODIFIED, time_string);
			TRACE(((req->verbosehdr?T_INFO:T_DAEMON), "[%llu] URL[%s], header => ('%s', '%s')\n",
					req->id, vs_data(req->url), MHD_HTTP_HEADER_LAST_MODIFIED, time_string));
		}
		else {
			scx_error_log(req, "URL[%s] - mtime[%ld], gmtime conversion failed(%s)\n",
					vs_data(req->url), req->objstat.st_mtime, (char *)ctime_r(&utime, time_string));
		}
	}


	char 			rewbuf[256] = {"text/html; charset=UTF-8"};
	if (req->streaming->media_type > 0) {
		if (req->service->streaming_method == 1) {
			/* 라이브 서비스 인 경우 */
			for(int i = 0;i < gscx__count_strm_live_reg;i++) {
				if(gscx__strm_live_reg[i].media_type == req->streaming->media_type) {
					snprintf(rewbuf, 256, gscx__strm_live_reg[i].mime_type);
					break;
				}
			}
		}
		else  {
			/* vod 인 경우 */
			for(int i = 0;i < gscx__count_strm_vod_reg;i++) {
				if(gscx__strm_vod_reg[i].media_type == req->streaming->media_type) {
					snprintf(rewbuf, 256, gscx__strm_vod_reg[i].mime_type);
					break;
				}
			}
		}
		MHD_add_response_header(req->response, MHD_HTTP_HEADER_CONTENT_TYPE, rewbuf);
	}

	if (streaming->x_play_duration) {
		MHD_add_response_header(req->response, "X-Play-Durations", streaming->x_play_duration);
	}
	return 0;
}


/*
 * 설정파일에서 허가된 protocol인지 확인 한다.
 */
int
strm_check_protocol_permition(nc_request_t *req)
{
	streaming_t 	*streaming = req->streaming;
	service_info_t *service = req->service;
	if ((streaming->protocol & O_PROTOCOL_CORS) != 0) {
		/* crossdomain.xml나 clientaccesspolicy.xml 요청은 아래의 확인 과정을 거치지 않는다. */
		return 1;
	}
	if ( (streaming->protocol & service->permitted_protocol) == 0) {
		/* 허가된 프로토콜이 아님 */
		scx_error_log(req, "Protocol Not Allowed. URL(%s)\n", vs_data(req->url));
		return 0;
	}
	if ( (streaming->media_mode & service->permitted_mode) == 0) {
		/* 허가된 mode가 아님 */
		scx_error_log(req, "Media Mode Not Allowed. URL(%s)\n", vs_data(req->url));
		return 0;
	}
	if ((streaming->protocol & O_PROTOCOL_HLS) != 0 &&
			0 == service->allow_different_protocol) {
		/* HLS이고 allow_different_protocol이 0(허용안함)인 경우는
		 * menifest에 정의된 프로토콜(http, https)로 접속 했는지 확인하는 과정을 거친다. */
		switch (streaming->media_type ) {
		case MEDIA_TYPE_HLS_MAIN_M3U8:
		case MEDIA_TYPE_HLS_SUB_M3U8:
			if (req->secured != req->hls_playlist_protocol) {
				scx_error_log(req, "Playlist Protocol Not Allowed. URL(%s)\n", vs_data(req->url));
				return 0;
			}
			break;
		case MEDIA_TYPE_HLS_TS:
			if (req->secured != req->hls_playlist_protocol) {
				scx_error_log(req, "Segment Protocol Not Allowed. URL(%s)\n", vs_data(req->url));
				return 0;
			}
			break;
		case MEDIA_TYPE_ENC_KEY:
			if (req->secured != service->hls_encrypt_key_protocol) {
				scx_error_log(req, "Encrypt key Protocol Not Allowed. URL(%s)\n", vs_data(req->url));
				return 0;
			}
			break;
		default :
			break;
		}
	}
	return 1;
}

/*
 * client의 요청 url에 포함된 rep_id(bandwidth)와 builder의 adaptive_track_name 비교해서
 * track id를 리턴한다.
 */
int
strm_get_track_id(nc_request_t *req)
{
	streaming_t 	*streaming = req->streaming;

	builder_info_t 	*builder = streaming->builder;
	int i = 0;
	int	track_id = 0;
	unsigned int bps = 0;
	if (!streaming->rep_id) return 0;
	/*
	 * MSS(microsoft smooth streaming)의 경우는 stream(track) 구분을 bitrate로 하기 때문에
	 * track을 만들때 사용한 정보로는 알수가 없어서 zipper library의 zipper_adaptive_index_by_bps를 통해서 확인을 해야한다.
	 */
	if (streaming->media_type == MEDIA_TYPE_MSS_VIDEO ||
		streaming->media_type == MEDIA_TYPE_MSS_AUDIO) {
		bps = atoi(streaming->rep_id);
		if (streaming->media_type == MEDIA_TYPE_MSS_VIDEO) {
			track_id = zipper_adaptive_index_by_bps(builder->bcontext, bps, BLDFLAG_INCLUDE_VIDEO);

		}
		else if (streaming->media_type == MEDIA_TYPE_MSS_AUDIO) {
			track_id = zipper_adaptive_index_by_bps(builder->bcontext, bps, BLDFLAG_INCLUDE_AUDIO);
		}
		return track_id;
	}
	for (i = 0; i < builder->adaptive_track_cnt; i++) {
		if (strcmp(streaming->rep_id, builder->track_info[i].adaptive_track_name) == 0) {
			return i;
		}
	}
	return 0;
}

/*
 * 오리진에 요청할 content의 url을 생성 한다.
 * 2020/12/07 base_path를 netcache에 직접 설정 하도록 수정 하면서 이 부분은 필요없어짐
 */
int
strm_make_path(nc_request_t *req,  int type, const char * url, char *buf, int length)
{
	streaming_t *streaming = req->streaming;
	service_info_t * service = req->service;
	int		slash_require = 0;	/* url_path 생성시에 중간에 '/' 삽입이 필요한 경우 1로 셋팅 */
	int 	ret = 0;

#if 0
	// 2020/12/07 base_path를 netcache에 직접 설정 하도록 수정 하면서 이 부분은 필요없어짐
#ifdef ZIPPER
	/*
	 * content type에 따라 base_path를 다르게 적용한다.
	 * 광고의 경우라도 광고용 오리진이 별도로 지정되지 않는 경우에는 vod 용 오리진의 base_path 설정을 사용한다.
	 */
	if (*url != '/') { /* base path와 url을 합칠 경우에 url의 앞에 '/'이 없는지 확인 */
		if (type == O_CONTENT_TYPE_ADVER) {
			if (service->ad_base_path) {
				if ( *(vs_data(service->ad_base_path) + vs_length(service->ad_base_path)) != '/') {
					slash_require = 1;
				}
			}
		}
		else {
			if (service->base_path) {
				if ( *(vs_data(service->base_path) + vs_length(service->base_path)) != '/') {
					slash_require = 1;
				}
			}
		}
	}
	/* 설정파일에 base_path가 설정 되어 있는 경우에는 path의 앞쪽에 base_path를 추가해준다. */
	/* smil_origin이 지정된 경우 smil_origin에 지정된 service의 설정을 따르는 경우 */
	if (type == O_CONTENT_TYPE_ADVER) {
		ret = scx_snprintf(buf, length, "%s%s%s", service->ad_base_path?vs_data(service->ad_base_path):"", slash_require?"/":"", url);
	}
	else {
		ret = scx_snprintf(buf, length, "%s%s%s", service->base_path?vs_data(service->base_path):"", slash_require?"/":"", url);
	}
#else
	/*
	 * cache(SolProxy)의 경우 scx_request_handler()에서 strm_url_parser()의 호출이 base_path 처리 부분 보다
	 * 빠른 경우 media url에 base_path 가 적용되 되지 않기 때문에 여기서 base_path를 추가 해주는 동작을 해야 한다.
	 * 반대로 scx_request_handler()에서 base_path를 처리를 한후 strm_url_parser()를 호출 하는 경우는
	 * 여기서 아무 처리를 하지 않아도 된다. 이 경우는 단순히 url을 buf에 복사해 주면 됨
	 */

#if 1
	if (*url != '/') { /* base path와 url을 합칠 경우에 url의 앞에 '/'이 없는지 확인 */
		if (service->base_path) {
			if ( *(vs_data(service->base_path) + vs_length(service->base_path)) != '/') {
				slash_require = 1;
			}
		}
	}
	/* 설정파일에 base_path가 설정 되어 있는 경우에는 path의 앞쪽에 base_path를 추가해준다. */
	ret = scx_snprintf(buf, length, "%s%s%s", service->base_path?vs_data(service->base_path):"", slash_require?"/":"", url);
#else
	/* cache의 경우는 scx_request_handler()에서 base_path를 처리 하기 때문에 여기서 처리 하면 중복이 된다. */
	ret = scx_snprintf(buf, length, "%s", url);
#endif
#endif
#else
	ret = scx_snprintf(buf, length, "%s", url);
#endif

	return ret;
}

/*
 * 스트리밍으로 동작 할때에는 content 캐시 뿐만 아니라 media index 캐시도 같이 퍼지 해야 한다.
 * 에러가 발생할 경우 -1을 리턴
 */
int
strm_purge(nc_request_t *req)
{
	char	open_path[1024] = {'\0'};
	int		cnt = 0;
	int		slash_require = 0;
	streaming_t *streaming = req->streaming;
	service_info_t * service = req->service;
	content_info_t *content = NULL;
	adaptive_info_t *adaptive;
	subtitle_info_t *subtitle = NULL;

	ASSERT(streaming);

	if ( (streaming->media_mode & O_STRM_TYPE_DIRECT) != 0 ) { /* 현재는 virtual file을 사용 안하는 경우에만 가능 */
		/* 주의 : streaming->media_path에는 zipper로 동작 하면서 단순 캐시인 경우만 값이 들어간다. */
		if (1 == service->use_local_file) {
			cnt += 1;
		}
		else {
			strm_make_path(req, O_CONTENT_TYPE_VOD, streaming->media_path, open_path, sizeof(open_path));
			cnt += nc_purge(req->service->core->mnt, open_path, req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);
		}
		strm_purge_media_info(req, O_CONTENT_TYPE_VOD, streaming->media_path);
	}
	else {
		/* adaptive mode로 smil이 아닌 content를 직접 지정하는 경우는 동일 파일에 대해 여러번 퍼지가 발생 할수 있지만 성능에 영향을 미치는 부분이 아니기 때문에 별도 처리를 하지 않는다. */
		content = streaming->content;
		while (content) {
			if (1 == service->use_local_file) {
				cnt += 1;
			}
			else {

				strm_make_path(req, content->type, content->path, open_path, sizeof(open_path));
#ifdef ZIPPER
				/* 광고 오리진이 별도로 설정된 경우에 다르게 동작 하는 부분 */
				if (content->type == O_CONTENT_TYPE_ADVER && service->core->ad_mnt) {
					cnt += nc_purge(service->core->ad_mnt, open_path, req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);;
				}
				else
#endif
				{
					cnt += nc_purge(service->core->mnt, open_path, req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);
				}
			}
			strm_purge_media_info(req, content->type, content->path);
			content = content->next;
		}

		/* 자막 파일이 있는 경우 같이 퍼지 한다. */
		subtitle = streaming->subtitle;
		while (subtitle) {
			if (1 == service->use_local_file) {
				cnt += 1;
			}
			else {

				strm_make_path(req, O_CONTENT_TYPE_VOD, subtitle->path, open_path, sizeof(open_path));
				cnt += nc_purge(service->core->mnt, open_path, req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);

			}
			subtitle = subtitle->next;
		}

		if ( (streaming->media_mode & O_STRM_TYPE_ADAPTIVE) != 0 ) {
			/* adaptive mode에는 smil이 한개 이상 있으므로 smil 파일도 같이 퍼지 한다. */
			adaptive = streaming->adaptive_info;
			while (adaptive) {
				if(adaptive->is_smil) {
					strm_make_path(req, adaptive->type, adaptive->path, open_path, sizeof(open_path));
#ifdef ZIPPER
					if (adaptive->type == O_CONTENT_TYPE_ADVER && service->core->ad_mnt) {
						cnt += nc_purge(service->core->ad_mnt, open_path, req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);;
					}
					else
#endif
					{
						cnt += nc_purge(service->core->mnt, open_path, req->purge_key?TRUE:FALSE, req->purge_hard?TRUE:FALSE);
					}
				}
				adaptive = adaptive->next;
			}
		}

	}
	return cnt;
}

/*
 * 캐싱되어 있는 media index 정보를 퍼지 한다.
 * 단일 파일 퍼지에 대해서만 지원하고 패턴 퍼지는 지원하지 않음.
 * 소프트 퍼지인 경우는 media index의 유효시간을 만료 시키고
 * 하드 퍼지인 경우는 media index를 not available 상태로 바꾼다.
 */
int
strm_purge_media_info(nc_request_t *req, int type, char *path)
{
	media_info_t 	*media = NULL;
	int				gres = 0;

	media = strm_find_media(req, type, path, U_FALSE, U_TRUE, &gres);
	if (media != NULL) {
		strm_commit_media(req, media);
		if (media->available != 0) {
			if (req->purge_hard == TRUE) {
				media->available = 0;
				media->etime = 0;
			}
			else {
				media->vtime = 0; /* TTL을 만료 시킨다. */
			}
		}
		TRACE((T_INFO, "[%llu] (%s) media info %s purged.\n", req->id, path, req->purge_hard==TRUE?"hard":"soft"));
		strm_release_media(media);
		media = NULL;
	}
	/* media metadata disk cache를 삭제 */
	mdc_disable_cache_file(req, path);
	return SCX_YES;
}

void *
strm_file_open(nc_request_t *req, int type, const char * url, struct nc_stat *objstat, mode_t mode)
{
	streaming_t *streaming = req->streaming;
	service_info_t * service  = req->service;
	double 	ts, te;
	void	*file = NULL;
	char	open_path[1024] = {'\0'};
	struct stat	st;

	strm_make_path(req, type, url, open_path, sizeof(open_path));
#ifdef DEBUG
//	printf("open path = %s\n", open_path);
#endif
	ts = sx_get_time();
	if (1 == service->use_local_file) {
		/* 설정파일에 base_path가 설정 되어 있는 경우에는 path의 앞쪽에 base_path를 추가해준다. */
		if (req->service->base_path) {
			scx_snprintf(open_path, 1024, "%s%s", vs_data(service->base_path), url);
		}
		scx_check_local_url_encoding(open_path);
		file = open(open_path, O_RDONLY);
		if ((int)file >= 0) {
			objstat->st_sizedecled = 1;
			objstat->st_vtime = scx_get_cached_time_sec() + 86400;	/* 만료시간을 하루로 한다 */
			fstat((int)file, &st);
			objstat->st_atime = st.st_atim.tv_sec;
			objstat->st_mtime = st.st_mtim.tv_sec;//st.st_mtime;
			objstat->st_ctime = st.st_ctim.tv_sec;
			objstat->st_uid = st.st_uid;
			objstat->st_gid = st.st_gid;
			objstat->st_gid = st.st_gid;
			snprintf(objstat->st_devid, 128, "%X", objstat->st_mtime);	/* devid(ETAG) 생성방식은 고민이 필요함 */
#if 1
			objstat->st_size = (nc_size_t)st.st_size;
#else
			objstat->st_size = lseek((int)file, 0, SEEK_END);
			lseek((int)file, 0, SEEK_SET);
#endif
		}
		else {
			req->file_errno = errno;
		}
	}
	else {
		/* Zipper로 동작하는 경우에는 host가 도메인이 아니라 직접 전달을 할수 없다. */
		/* 광고와 본 content를 구분해서 hostname 변경 기능이 동작 한다. */
		if (type == O_CONTENT_TYPE_ADVER) {
			/* 여기는 zipper로 동작 할때만 들어와야 한다. */
			if(service->ad_origin_hostname) {
				kv_replace(streaming->options, MHD_HTTP_HEADER_HOST, vs_data(service->ad_origin_hostname), FALSE);
			}
		}
		else if(service->origin_hostname) {
			/* zipper와 solproxy에서 streaming으로 동작하는 경우 모두 들어 올수 있다. */
			kv_replace(streaming->options, MHD_HTTP_HEADER_HOST, vs_data(service->origin_hostname), FALSE);
		}
#ifndef ZIPPER
		else {	/* client의 요청에 포함된 host 헤더를 전달 */
			/* solproxy에서 streaming 기능을 사용하는 경우에만 이곳에 들어 온다. */
			kv_replace(streaming->options, MHD_HTTP_HEADER_HOST, vs_data(req->host), FALSE);
		}
#endif

#ifdef ZIPPER
		/* 광고 오리진이 별도로 설정된 경우에 다르게 동작 하는 부분 */
		if (type == O_CONTENT_TYPE_ADVER && service->core->ad_mnt) {
			CHECK_TIMER("nc_open_extended2", req, file = (void *)nc_open_extended2(service->core->ad_mnt, open_path, open_path, mode, objstat, streaming->options));
			//WEON
			if (file) {
				__sync_add_and_fetch(&service->opened, 1);
			}
		}
		else
#endif
		{
			if (service->core->mnt == NULL) {
				/* 오리진 설정이 되지 않은 볼륨으로 요청이 들어 오는 경우는 에러 처리 한다. */
				scx_error_log(req, "%s undefined origin. url(%s)\n", __func__, open_path);
				req->p1_error = MHD_HTTP_BAD_REQUEST;
			}
			else {
				CHECK_TIMER("nc_open_extended2", req, file = (void *)nc_open_extended2(service->core->mnt, open_path, open_path, mode, objstat, streaming->options));
				//WEON
				if (file) {
					__sync_add_and_fetch(&service->opened, 1);
				}
			}
		}

		if (NULL == file) {
			req->file_errno = nc_errno();
		}
		else if (objstat->st_origincode >= 300 || objstat->st_origincode < 200) {
			/* 200이나 206이 아닌 다른 코드를 오리진에서 리턴하는 경우 처리가 불가능하므로 바로 file을 닫는다. */
			scx_error_log(req, "Not Supported Origin Code(%d)\n", objstat->st_origincode);
			ASSERT(nc_check_memory((nc_file_handle_t *)file, 0));
			CHECK_TIMER("nc_close", req, nc_close((nc_file_handle_t *)file, 0));
			//WEON
			if (file) {
				__sync_sub_and_fetch(&service->opened, 1);
			}
			file = NULL;
		}
	}
	te = sx_get_time();
	req->t_nc_open += (te - ts);

//	objstat->st_vtime = time(NULL) + 86400;	/* 만료시간을 하루로 한다 */
	return file;
}

void
strm_file_close(nc_request_t *req, void * file)
{
	double 			ts, te;
	int		ret = 0;
	service_info_t * service  = req->service;
	if (NULL == file) return;
	ts = sx_get_time();

	if (1 == service->use_local_file) {
		ret = close((int)file);
		if (0 > ret) {
			req->file_errno = errno;
		}
	}
	else {
	//nc_close((nc_file_handle_t *)file);
		ASSERT(nc_check_memory((nc_file_handle_t *)file, 0));
		CHECK_TIMER("nc_close", req, ret = nc_close((nc_file_handle_t *)file, 0));
		//WEON
		if (file) {
			__sync_sub_and_fetch(&service->opened, 1);
		}
		if (0 > ret) {
			req->file_errno = nc_errno();
		}
	}

	te = sx_get_time();
	req->t_nc_close += (te - ts);
	return;
}

ssize_t
strm_file_read(nc_request_t *req, void * file, void *buf, off_t offset, size_t toread)
{
	ssize_t copied = 0;
	double 			ts, te;
	service_info_t * service  = req->service;
	if (toread <= 0) return 0;
	ts = sx_get_time();
	if (1 == service->use_local_file) {
		lseek((int)file, offset, SEEK_SET);
		copied = read((int)file, buf, toread);
		if (0 > copied) {
			req->file_errno = errno;
		}
	}
	else {
	//copied = nc_read((nc_file_handle_t *)file, buf, (nc_off_t)offset, (nc_size_t)toread);
		ASSERT(nc_check_memory((nc_file_handle_t *)file, 0));
		copied = nc_read((nc_file_handle_t *)file, buf, (nc_off_t)offset, (nc_size_t)toread);
		if (copied < toread && ETIMEDOUT == nc_errno()){
			/* nc_read()에서 timeout이 걸린 경우 오리진이 서비스하기에 비정상적으로 느린것으로 판단해서 Client의 연결을 끊는다. */
			req->file_errno = nc_errno();
			copied = -1;
			TRACE((T_INFO, "[%llu] Read timeout, offset = %lld, toread = %lld\n", req->id, req->cursor, toread));
			scx_error_log(req, "strm_file_read() read timeout, URL[%s], offset = %lld, toread = %lld\n", vs_data(req->url), req->cursor, toread);
		}
		else if (0 > copied) {
			req->file_errno = nc_errno();
		}
		else if (0 == copied) {
			if (EIO == nc_errno()) {
				/* EIO가 발생하는 경우는 읽는 도중에 오리진이 죽었다고 본다 */
				req->file_errno = nc_errno();
				copied = -1;
			}
		}
	}
	te = sx_get_time();
	req->t_nc_read += (te -ts);
//	printf("[%llu] strm_file_read offset=%lld, toread=%ld, copied=%lld\n", req->id, offset, toread, copied);
	TRACE((T_DEBUG, "[%llu] strm_file_read offset=%lld, toread=%ld, copied=%lld\n", req->id, offset, toread, copied));
	return copied;
}

int
strm_file_errno(nc_request_t *req)
{
	return req->file_errno;
}

/*
 * 암호화 key를 생성한다.
 * key는 hls_encrypt_key;solsessionid;(session에 저장된 path)를 사용해서 md5hash로 만든다.
 * session 기능을 사용하지 않는 경우는 'hls_encrypt_key;content_path'를 사용해서 md5hash를 만든다.
 ** 이 경우 컨텐츠 별로 동일한 복호화 key를 가지게 된다.
 * 생성된 key는 session 정보에 같이 저장하고 ts 요청이 암호화에 사용한다.
 * TS 요청시 session 기능을 사용하지 않거나 재기동 등으로 저장된 session이 없는 경우는 key를 새로 생성한후 다시 session에 저장한다.
 * 만들어진 암호화 key는 16byte(128bit)이다
 */
int
strm_make_enc_key(nc_request_t *req,  char *buf)
{

	char *token = NULL;
	int alloc_size = 0;
	int token_len = 0;
	struct MD5Context md5;
	session_info_t * session = NULL;

	if (req->streaming->session) {
		session = req->streaming->session;
		if(session->enc_key) {
			/* session에 enc_key가 만들어져 있는 경우는 해당 key를 사용한다. */
			memcpy(buf, session->enc_key, 16);
			return 0;
		}
		alloc_size = vs_length(req->service->hls_encrypt_key) + 2;
		alloc_size += 11 + strlen(session->path); // session ID hex 길이 + path 길이
		token = alloca(alloc_size);
		snprintf(token, alloc_size, "%s;0X%X;%s",
									vs_data(req->service->hls_encrypt_key),
									session->id,
									session->path);
		token_len = strlen(token);
	}
	else {
		/* session 기능을 사용하지 않는 경우는 'hls_encrypt_key;content_path'로 key를 설정한다. */
		alloc_size = vs_length(req->service->hls_encrypt_key) + 2;
		alloc_size += req->streaming->real_path_len; // session ID hex 길이 + path 길이
		token = alloca(alloc_size);
		snprintf(token, alloc_size, "%s;%s",
									vs_data(req->service->hls_encrypt_key),vs_data(req->ori_url));
		token_len = strlen(token);
	}
	/* 만들어진  token을 사용해서 16 byte MD5 hash를 생성 */
	MD5Init(&md5);
	MD5Update(&md5, token, token_len);
	MD5Final(buf, &md5);
	if (session) {
		session->enc_key = mp_alloc(session->mpool, 16);
		memcpy(session->enc_key, buf, 16);
	}

	return 0;
}

/*
 * media_info에 저장되어 있는 파일의 정보와 실제 파일의 정보가 같은지 확인한다.
 * media_info의 TTL만료시 호출
 * 파일이 동일한 경우 0을 리턴
 * 파일이 갱신된 경우 -1을 리턴
 */
int
strm_check_media_info(nc_request_t *req, int type, media_info_t *media)
{
	/* 캐싱된 media 정보가 있는 경우 만료 시간이 지났는지 확인 한다. */
	struct nc_stat 		objstat;
	memset(&objstat, 0, sizeof(struct nc_stat));
	if (strm_get_media_attr(req, type, media->url, &objstat) == 0 ) {
		TRACE((T_INFO, "[%llu] Failed to get media attribute. path(%s)\n", req->id, media->url));
		/* 원본 파일에 문제가 있는 경우 */
		return -1;
	}

	if (media->mtime != objstat.st_mtime) {
		/*
		 * 원본 파일이 변경된 경우 available을 0로 marking 한 후에
		 * 캐싱되지 않는 media를 새로 생성한다.
		 */
		TRACE((T_INFO, "[%llu] Cached media info mtime updated. cache(%ld), update(%ld), path(%s)\n", req->id, media->mtime, objstat.st_mtime, media->url));
		return -1;
	}
	else if (media->msize != objstat.st_size) {
		/*
		 * 원본 파일의 크기가 변경된 경우도 파일이 변경 된걸로 판단.
		 */
		TRACE((T_INFO, "[%llu] Cached media size changed. cache(%lld), change(%lld), path(%s)\n", req->id, media->msize, objstat.st_size, media->url));
		return -1;
	}
	else if(strncmp(media->st_devid, objstat.st_devid, 128) != 0) {
		/*
		 * ETag가 바뀌는 경우에도 파일이 변경된걸로 판단한다.
		 * 이 검사를 skip 하는 기능이 필요할수도 있지만 설정이 너무 복잡해지는것 같아서 제외함.
		 */
		TRACE((T_INFO, "[%llu] Cached media info ETag updated. cache(%s), update(%s), path(%s)\n", req->id, media->st_devid, objstat.st_devid, media->url));
		return -1;
	}
	else {
		/* 여기 까지 오는 경우는 원본 파일이 변경되지 않고 netcache core에서 만료 시간이 갱신된 경우라고 봐야  */
		TRACE((T_DAEMON|T_DEBUG, "[%llu] Cached media info expire time update. cache(%ld), update(%ld), path(%s)\n", req->id, media->vtime, objstat.st_mtime, media->url));
		media->vtime = objstat.st_vtime;
	}

	return 0;
}



