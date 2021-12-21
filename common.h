#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>
#include <gnutls/gnutls.h>
#include <ncapi.h>
/* 모듈과 solproxy간에 공유가 필요한 구조체나 define문들을 여기에 적는다. */

#define SCX_YES 1
#define SCX_NO 0

struct site_tag;
struct tag_scx_list ;
//struct vstring_tag;
struct MHD_Connection;
struct streaming_tag;
struct request_mem_pool;
struct scx_config_tag;
struct shm_log_body;

#pragma pack(4)
typedef 	unsigned long vs_uint32_t;
typedef struct vstring_tag {
	vs_uint32_t 	asize:16;  /* 할당 크기 */
	vs_uint32_t 	ssize:16; /* 스트링 길이*/
	unsigned char 	*data;
} vstring_t;
#pragma pack()
#ifndef TRUE
#define 	TRUE 	1
#endif
#ifndef FALSE
#define 	FALSE 	0
#endif

#ifndef min
#define	min(a,b)	(a>b?b:a)
#endif
#ifndef max
#define	max(a,b)	(a>b?a:b)
#endif

#define 	vs_data(_v) 		((_v)->data)
#define 	vs_length(_v) 		((_v)->ssize)
#define 	vs_alloc_size(_v) 	((_v)->asize)
#define 	vs_remained(_v) 	((_v)->asize - (_v)->ssize)
#define 	vs_offset(_v, _o) 	(char *)((_v)->data + _o)

#define MAX_MODULE_COUNT		20			/* 로딩 가능 최대 모듈 수 */

typedef enum {
	VOLUME_STATIC			= 0, 	/* 설정 파일에 정의가 된 볼륨 */
	VOLUME_DYNAMIC 			= 1 	/* 동적으로 생성된 볼륨 */
	/* 이후 동적 볼륨도 통계(openapi용)를 제고하는 볼륨과 순수한 동적 볼륨으로 구분해야 할 필요 성이 있다. */
} scx_volume_type_t;

typedef enum {
	VOL_SERVICE_TYPE_COMMON			= 0, 	/* 기본 서비스 볼륨 */
	VOL_SERVICE_TYPE_SERVICE		= 1, 	/* 대표 도메인 서비스용 볼륨 */
	VOL_SERVICE_TYPE_FHS			= 2, 	/*  FHS 도메인 서비스용 볼륨 */
} scx_vol_service_type_t;


struct service_cmaf_ctx_tag;
/*
 * 이전에 적용된 netcache core 관련 설정을 기억
 * 새로운 설정과 이전 설정을 비교해서 변경된 값만 적용하기 위해 사용
 */
typedef struct applied_conf_tag {
	char *origins;
	char *parents;
	char *base_path;
	int32_t origin_protocol;
	int32_t is_base_path_changed;
	int32_t origin_policy;
	int32_t parent_policy;
	int32_t writeback_chunks;
	char *proxy_address;
	int32_t origin_connect_timeout;
	int32_t origin_transfer_timeout;
	int32_t nc_read_timeout;
	int32_t origin_max_fail_count;
	char fresh_check_policy[30];
	int	  origin_online_code_rev;
	int32_t negative_ttl;
	int32_t positive_ttl;
	int32_t follow_redir;
	int32_t dns_cache_ttl;
	char *origin_health_check_url;
	char *ad_origins;
	char *ad_base_path;
	int32_t ad_origin_protocol;
	int32_t is_ad_base_path_changed;
	int32_t ad_writeback_chunks;
	int32_t ad_origin_connect_timeout;
	int32_t ad_origin_transfer_timeout;
	int32_t ad_nc_read_timeout;
	char ad_fresh_check_policy[30];
	int32_t ad_origin_policy;
	int32_t ad_follow_redir;
	int	  ad_origin_online_code_rev;
	int32_t ad_negative_ttl;
	int32_t ad_positive_ttl;
} applied_conf_t;

/* reload를 해도 계속 유지 해야하는 service의 정보들을 가지고 있는 구조체 */
typedef struct service_core_tag {
	/* inbound : client쪽, outbound : origin쪽 */
	uint64_t 	counter_inbound_response;
	uint64_t 	counter_inbound_request;
	uint64_t 	counter_inbound_method[12];
	uint64_t	counter_inbound_code[64];	/* client response code별 누적용 */
	uint64_t 	counter_inbound_counter;	/* 현재까지 요청된 request 누적 count */
	uint64_t 	counter_session_total;		/* 현재까지 요청된 session 누적 count */
	uint64_t 	counter_outbound_response;
	uint64_t 	counter_outbound_request;
	uint64_t 	counter_outbound_method[12];
	uint64_t	counter_outbound_code[64];	/* origin response code별 누적용 */
	uint64_t 	counter_outbound_counter;

	uint64_t 	prev_inbound_request;	/* 마지막 확인시 Client에서 받은 양(traffic) 누적치 */
	uint64_t 	prev_inbound_response;	/* 마지막 확인시 Client로 전송된 양(traffic) 누적치 */
	time_t		prev_inbound_time;		/* 마지막 traffic 통계 확인 시간 */
	uint64_t 	prev_inbound_counter;	/* 마지막 확인시 request 누적 count */
	uint64_t 	prev_session_total;		/* 마지막 확인시 누적 session count */

	uint64_t 	prev_outbound_request;	/* 마지막 확인시 오리진으로 전송된 양(traffic) 누적치 */
	uint64_t 	prev_outbound_response;	/* 마지막 확인시 오리진에서 받은 양(traffic) 누적치 */
	time_t		prev_outbound_time;		/* 마지막 origin 통계 확인 시간 */

	uint32_t 	concurrent_count;		/* 현재 해당 서비스를 열고 있는 client의 연결수 */
	uint32_t	peek_concurrent_count;	/* 1초 단위 최대 동접수, nos 통계에 사용 */
	uint32_t 	session_count;			/* 현재 해당 서비스를 사용하고 있는 session 수 */
	uint32_t	peek_session_count;		/* 1초 단위 최대 session수, nos 통계에 사용 */
	uint32_t 	local_using_count;		/* client가 직접 사용하는것이 아닌 streaming 관련 부분에서 열고 있는 수 */
	uint32_t 	service_count;			/* service_stat_t를 참조하고 있는 service_info_t의 수 */

	uint32_t 	purge_revision;			/* domain을 여러개 사용하는 경우 purge가 여러번 호출되는것을 방지하기 위해 사용 */
	void		*module_ctx[MAX_MODULE_COUNT];		/* 모듈에서 사용하는 context, reload 시 service_info_t는 매번 새로 생성되서 이쪽으로 이동  */
	uint32_t	is_alias_mnt;			/* 0인 경우 실제 오리진 mount동작, 1인 경우 해당 볼륨은 실제 origin mount를 하지 않고 다른 볼륨의 mount를 가져다 쓴다. */
	nc_mount_context_t 	*mnt;			/* netcache mount context pointer */
	applied_conf_t		conf;			/* 이전에 적용된 설정을 기억 */
#ifdef ZIPPER
	nc_mount_context_t 	*ad_mnt;		/* 광고 VOD origin용 mount context pointer */
#endif
	struct service_cmaf_tag		*cmaf;
	void 		*master_service;		/* service 정보 저장 */
} service_core_t;


typedef struct service_info_tag {
	pthread_mutex_t 	lock;
	uint64_t 			current_origin_ver;
	struct site_tag 	*site;
	vstring_t 			*name;
	vstring_t 			*master_cache_domain;		/* master_cache_domain 기능을 사용하는 경우만 설정 */
	service_core_t 		*core;
	struct tag_scx_list *domain_list;		/* 해당 서비스와 연결된 virtual host name을 가지고 있는 리스트 */
	scx_volume_type_t	volume_type;		/* 동적으로 생성된 volume일 경우 VOLUME_DYNAMIC으로 셋팅됨 */
	vstring_t 			*origin_online_code;	/* 오리진 online 확인시 여기에 설정된 코드로 응답을 받은 경우 online 상태로 판단 */
	vstring_t 			*custom_user_agent;		/* 오리진 요청시 여기에 설정된 값으로 User-Agent 헤더를 변경해서 요청 */
	vstring_t 			*bypass_method;			/* 지정된 method들을 오리진으로 bypass 한다 */
	struct tag_scx_list	*remove_origin_request_headers;		/* 오리진에 요청시 지정된 헤더를 빼고 요청 */
	struct tag_scx_list *remove_origin_response_headers;	/* 오리진 응답중 지정된 헤더를 삭제하고 캐싱한다 */
	struct tag_scx_list	*referers_allow;			/* 지정된 referer 헤더에 대해서만 허용 */
	struct tag_scx_list	*referers_deny;			/* 지정된 referer 헤더가 들어 오면 차단 */
	void				*timer;				/* delayed memory 해제를 위한 timer */
	vstring_t 			*origin_hostname;	/* origin 서버에 요청할때 사용하는 host name */
	vstring_t 			*base_path;			/* origin 요청 url의 앞에 붙는 prefix */
	uint32_t			send_timeout;			/* Client가 데이터를 받는도중 pause를 하거나 Client의 네트웍이 갑자기 단절 되거나 했을때 적용되는 timeout */
	uint32_t			keepalive_timeout;		/* Client가 요청한 데이터를 전송한 상태에서 다음 request를 받기 위해 socket 연결을 유지 하는 시간 */
	vstring_t			*cdn_domain_postfix;	/* cdn 별 main 도메인, 이 값을 사용해서 fhs 도메인으로 들어온 요청을 볼륨으로 도메인으로 변환한다. */
	uint32_t 			remove_dir_level; 		/* 볼륨도메인으로 변환하기 전에 볼륨 구분용으로 사용된 첫번째 디렉토리를 제거 할지 여부 */
	struct tag_scx_list *sub_domain_list;		/* url에 들어있는 첫번째 디렉토리와 볼륨 도메인 매핑 정보*/
	////////////////////////////////// Content Router 기능 관련 설정 시작 //////////////////////////////////////////////
	int					cr_service_enable;		/* 서비스(볼륨) 별 content router 기능 사용여부, 1:사용, 0:사용 안함, default:0(사용안함) */
	int					cr_hot_content_day;		/* 파일이 만들어진 시점(mtime)을 기준으로 지정된 날짜가 지난 경우(cold content)만 content routing 처리한다. 단위:일(day), 기본값:10(일) */
	////////////////////////////////// Content Router 기능 관련 설정 끝   //////////////////////////////////////////////
	////////////////////////////////// 통계 관련 설정 시작 /////////////////////////////////////////////////////////////
	int					stat_write_enable;		/* 서비스 데몬의 통계 기록 여부 설정 */
	int					stat_origin_enable;		/* 오리진 통계 기록 여부 설정 */
	int					stat_traffic_enable;	/* 트래픽 통계 기록 여부 설정 */
	int					stat_nos_enable;		/* NOS 통계 기록 여부 설정 */
	int					stat_http_enable;		/* HTTP(Content) 통계 기록 여부 */
	int					stat_http_ignore_path;	/* HTTP(Content) 통계 기록시 실제 요청 path대신 -(dash)로 기록 */
	int					stat_sp_seq;			/* 통계 기록시 사용 되는 SP_SEQ */
	int					stat_cont_seq;			/* 통계 기록시 사용 되는 CONT_SEQ */
	int					stat_vol_seq;			/* 통계 기록시 사용 되는 VOL_SEQ */
	int					stat_session_type_enable;	/* session을 사용중인 경우 통계 기록시 session 기준으로 통계를 기록 */
	////////////////////////////////// 통계 관련 설정 끝 ///////////////////////////////////////////////////////////////
	/////////////////// 	streaming 관련 시작	///////////////////////////////////////
	vstring_t 			*ad_origin_hostname;	/* 광고 컨텐츠용 origin 서버에 요청할때 사용하는 host name */
	vstring_t 			*ad_base_path;			/* origin 요청 url의 앞에 붙는 prefix */
	uint32_t			hls_target_duration;/* TS Target Duration */
	uint32_t			quick_start_seg;	/* 퀵스타트를 위한 초기 최소 분할 세그먼트의 갯수 */
	mode_t				permitted_protocol;	/* service 가능한 streaming 프로토콜, HLS, DASH, Progressive등 */
	mode_t				permitted_mode;		/* service 가능한 streaming mode, single, multi, adaptive등 */
	uint32_t			session_timeout;	/* 마지막 요청후 세션 유지 시간 . 단위:초 */
	vstring_t			*hls_playlist_host_domain; 	/* 설정된 경우 HLS의 manifest 생성시 설정된 값을 사용해서 절대 경로를 생성 */
	vstring_t			*dash_manifest_host_domain; /* 설정된 경우 DASH의 manifest 생성시 설정된 값을 사용해서 절대 경로를 생성 */
	uint32_t			hls_playlist_protocol;		/* UNDEFINED_PROTOCOL : 미지정, 0: http, 1:https */
	uint32_t			dash_manifest_protocol;		/* DASH menifest 응답에 사용하는 프로토콜. default:미지정 (들어온 요청의 프로토콜 사용), UNDEFINED_PROTOCOL:미지정, 0:http, 1:https  */
	vstring_t			*hls_encrypt_key;		/* HLS segment(TS 생성)시 사용되는 기본 암호화 key */
	uint32_t			hls_encrypt_method;	/* HLS segment(TS 생성)시 0 이면 암호와 안함, 1이면 AES-128로 암호화 */
	vstring_t			*manifest_prefix_path;		/* fhs 도메인을 사용하는 경우 manifest 응답시 path의 첫 디렉토리에 들어갈 경로(서비스명-볼륨명 형식) */
	scx_vol_service_type_t	vol_service_type;	/* 해당 볼륨의 type을 지정, 0 : 일반 볼륨, 1 : 대표 도메인 서비스용 볼륨, 2 : FHS 도메인 서비스용 볼륨, default:0(일반 볼륨) */

	vstring_t			*cmaf_origin[2];	/* CMAF 오리진 서버 주소, http/https까지 지정 및 포트지정, 2개까지 지정가능(컴마로 구분), 예: http://localhost:9080 */
	vstring_t			*cmaf_origin_base_path; /* CMAF 오리진 요청시 요청 경로의 앞에 추가 경로를 붙이는 경우 사용 */
	vstring_t			*cmaf_origin_path; /* CMAF 오리진에 요청되는 mpd 파일의 경로, cmaf_base_path가 설정 된 경우 오리진에는 cmaf_base_path + cmaf_path 가 된다. */
	uint32_t			cmaf_origin_timeout;	/* CMAF 오리진에서 응답하지 않는 경우 timeout 설정,1초부터 설정가능, 단위:초, default:5   */
	uint32_t			cmaf_origin_disconnect_time;	/* CMAF에서 일정시간동안 사용자의 요청이 없는 경우 오리진과의 연결을 해제, 0으로 설정된 경우는 사용자 요청이 없더라도 연결을 끊지 않음, 단위:초, default:300 */
	uint32_t			cmaf_origin_connect_onload;		/* CMAF 볼륨 생성시 혹은 설정 변경시 오리진에 연결한 상태로 시작할지를 결정, 1:서비스 시작시 오리진 연결, 0:서비스시작시 오리진 연결 안함,사용자 요청이 들어온 시점에 오리진 요청, default:0 */
	uint32_t			cmaf_origin_retry_interval;	/* CMAF 오리진 재연결 시도 간격, 단위:초 ,default:2 */
	uint32_t			cmaf_origin_retry_max_count;	/* CMAF 오리진 재연결 시도 회수, 0이면 무제한, default:0 */
	uint32_t			cmaf_fragment_cache_count;	/* 메모리 상에 캐시될 Fragment의 최대 수, default:10 */
	uint32_t			cmaf_prefetch_fragment_count;	/* 오리진 연결시 가져올 과거 Fragment수, default:0 */
	uint32_t			cmaf_manifest_fragment_count;	/* 매니패스트 상에 기술될 Fragment의 최대 수, default:5 */
	vstring_t			*hls_modify_manifest_ext; /* 지정된 확장자로 들어 오는 경우 manifest로 인식해서 modification 기능 동작, 확장자는 대소문자 구분하지 않음, 기본값:m3u8 */
	uint32_t			hls_modify_manifest_ttl; /* manifest 의 TTL을 별도 지정, 설정하지 않는 경우 positive_ttl 값을 사용한다, 단위:초 */
	vstring_t			*session_id_name; /* session ID 명을 변경할수 있다, 기본값:solsessionid */
	vstring_t			*streaming_media_ext; /* 스트리밍 컨텐츠로 인식해서 session 기능 사용가능, 복수로 설정 가능(컴마로 구분), 기본값:없음 */
	struct tag_scx_list	*additional_manifest_cache_key_list; /* manifest 파일의 오리진 요청시 path외에 추가적으로 붙는 query parameter를 설정, 기본값:없음 */
	int					media_negative_ttl;		/* metadata parsing시 문제가 발생한 컨텐츠에 대해 지정된 시간동안 동일 요청 차단, 기본값:1(초) */

	uint32_t			session_enable:1;		/* streaming시 session 기능을 사용 여부. 1이면 사용, 0이면 사용 안함, default 1 */
	uint32_t			use_local_file:1;		/* 0:origin사용, 1:local file 사용 */
	uint32_t			hls_master_playlist_use_absolute_path:1; 	/* main manifest(master playlist)의 절대/상대 경로 응답 */
	uint32_t			hls_media_playlist_use_absolute_path:1; 	/* sub manifest(media playlist)의 절대/상대 경로 응답 */
	uint32_t			dash_manifest_use_absolute_path:1; 			/* 설정된 경우 DASH menifest 생성시 설정된 값을 사용해서 절대 경로를 생성한다. 1이면 절대경로, 0이면 상대경로, default:0 */
	uint32_t			hls_permit_discontinuty:1; 		/* HLS로 Zipping시 인코딩 속성이 다른 경우에 EXT-X-DISCONTINUITY 태그를 사용할수 있도록 허용. 0(허용안함), 1(허용) 가능 */
	uint32_t			hls_discontinuty_all:1; 		/* live 광고 stitching시 광고 ts에 모두 EXT-X-DISCONTINUITY 를 붙인다. 1 : 사용 , 0 : 사용안함  */
	uint32_t			hls_encrypt_key_protocol:1;		/* 0: http, 1:https */
	uint32_t			allow_different_protocol:1;		/* menifest에 지정된 프로토콜이 아닌 다른 프로토콜로 접근했을때 허용여부. 1:허용, 0:허용안함 */
	uint32_t			streaming_enable:1;				/* 0:사용안함, 1:사용, 스트리밍기능 사용 */
	uint32_t			id3_retain:1;					/* 0:유지안함, 1:유지 */
	uint32_t			streaming_method:2;				/* 0인 경우 VOD,  1인 경우 Live, 2인 경우 cmaf live로 동작, default : 0 */
	uint32_t			ad_allow_media_fault:1;			/* 광고로 지정된 컨텐츠가 없거나 문제가 있을경우 해당 광고만 제외하고 zipping을 할지, 아니면 404에러를 리턴할지 결정,  0:허용안함,1:허용, default:1(허용) */
	uint32_t			hls_modify_manifest_enable:1;		/* HLS Manifest 변경 기능 사용 여부 설정, 1:사용, 0:사용안함, 기본값:0(사용안함) */
	uint32_t			hls_modify_manifest_session_enable:1;		/* 신규 요청이 들어 올때마다 session ID를 추가로 달아서 응답, 1:사용, 0:사용안함, 기본값:0(사용안함) */
	uint32_t			hls_modify_manifest_redirect_enable:1;		/* manifest 요청에 session이 포함되어 있지 않은 경우 session을 추가후 redirect 한다, 1:사용, 0:사용안함, 기본값:0(사용안함) */
	uint32_t			hls_manifest_compress_enable:1;				/* live hls manifest 응답시 body를 gzip으로 압축하는 기능 사용 여부, 1:압축, 0:압축안함, 기본값:0(압축안함) */
	uint32_t			enable_crossdomain:1;						/* crossdomain.xml나 clientaccesspolicy.xml 요청에 대해 직접 응답한다, 1:사용, 0:사용안함, 기본값:0(사용안함) */
	uint32_t			rtmp_service_enable:1;					/* rtmp 서비스를 하는 경우 설정, 1:사용, 0:사용안함, default : 0(사용안함)  */
	uint32_t			progressive_convert_enable:1;	/* 스트리밍 기능에서 가상 파일(content.mp4)요청인 경우 변환을 할지 여부,0으로 설정된 경우 원본을 그대로 전달, 1로 설정된 경우 변환, 기본값:1(변환함) */
	uint32_t			enable_wowza_url:1;				/* wowza의 url 패턴을 사용할지 여부, 0:사용안함, 1:사용, 기본값:0(사용안함) */
	uint32_t			cache_request_enable:1;				/* 스트리밍 요청에 대해서만 허용하고 cache에 대해서 허용을 할건지에 대한 여부 설정, 0:cache요청거부, 1:cache 청허용, 기본값:1(cache요청허용) */
/////////////////// 	streaming 관련 끝	///////////////////////////////////////
	uint32_t			log_access:1;		/* access log on/off, 1이면 on 0이면 off */
	uint32_t			log_error:1;		/* access log on/off, 1이면 on 0이면 off */
	uint32_t			log_origin:1;		/* access log on/off, 1이면 on 0이면 off */
	uint32_t			follow_redir:1;		/* 301응답에 대해 자동으로 연결, 1이면 on, 0이면 off */
	uint32_t			log_gmt_time:1;		/* 1인 경우 GMT time, 0인 경우 local time */
	uint32_t			enable_https:1;		/* 볼륨별 https 서비스를 허용 여부 설정. https허용:1, https차단:0, default : 0(차단) */
	uint32_t			protocol:1;			/* 1인 경우 origin 접속시 https사용, 0인 경우 http 사용 */
	uint32_t			ignore_query:1;		/* 1인 경우 오리진 요청시 request url에 포함된 query paremeter를 제거하고 요청한다.  */
	uint32_t			origin_request_type:2;	/* 1이면 오리진 요청시 Full GET, 0이면Range Get */
	uint32_t			enable_async_accesshandler_callback:1;	/* 1이면 AccessHandlerCallback시 비동기(별도의 job thread 사용) 기능을 사용 */
	uint32_t			enable_async_reader_callback:1;			/* 1이면 ContentReaderCallback시 비동기(별도의 job thread 사용) 기능을 사용 */
	uint32_t			enable_async_complete_callback:1;		/* 1이면 RequestCompletedCallback시 비동기(별도의 job thread 사용) 기능을 사용 */
	uint32_t			use_origin_authorization:1;				/* 오리진 요청시 Authorization 헤더가 추가되는(양뱡향 오리진) 경우 1로 설정 */
	uint32_t			referer_not_exist_allow:1;				/* referer 헤더가 없는 경우 차단 여부, default : 1(허용), 0이면 차단, 1이면 허용 */
	struct tag_scx_list	*soluri2_secret;	/* soluri2 인증에 사용되는 키 */
	struct tag_scx_list	*soljwt_secret_list;		/* jwt 인증에 사용되는 키 */
	struct tag_scx_list	*soljwt_claim_list;	/* jwt 인증에 사용되는 claim 목록 */
	uint32_t		auth_start_time;	/* 설정이 되면 서버 시작 후 지정된 시간동안 soluri2 인증시 토큰 인증 시간이 만료 되더라도 인증 성공으로 한다 */
/////////////////// 	service reload 관련 시작	///////////////////////////////////////
	uint32_t 		using_count;		/* 현재 해당 서비스를 열고 있는 client의 연결수 */
	uint32_t		alias_count;		/* 해당 서비스를 사용하고 있는 vitual host의 숫자. (alias host의 수) */
	uint32_t		revision;			/* 설정을 적용한 버전, reload 할때마다 1씩 증가 */
	uint32_t		check_revision;		/* reload시에 설정 파일의 내용이 변하지 않았을때 marking */
	uint32_t		available;			/* 0이면 vm_add()에서 검색이 되더라도 NULL을 리턴한다. */
	off_t			st_size;			/* 설정 파일의 크기 */
	uint32_t		hash; 				/* 설정 파일 내용의 전체 hash값.  st_size, hash는 설정 파일의 변경 여부 확인에 사용된다. */
	uint32_t		hash_crt; 				/* crt 인증서 파일의 hash값. 설정 파일의 변경 여부 확인에 사용된다. */
	uint32_t		hash_key; 				/* key 인증서 파일의 hash값. 설정 파일의 변경 여부 확인에 사용된다. */
	vstring_t		*certificate_crt_path; /* crt 파일 경로 */
	vstring_t		*certificate_key_path; /* key 파일 경로 */
/////////////////// 	service reload 관련 끝	///////////////////////////////////////
/////////////////// 	limit rate 관련 시작	///////////////////////////////////////
	uint32_t 		limit_rate;			/* 세션별 전송속도 제한 (단위 : KByte/sec) */
	uint32_t 		limit_rate_after;	/* 설정된 크기 이후에 속도제한 (단위 : KByte) */
	uint32_t 		limit_traffic_rate;	/* 서비스별 속도제한 (단위 : KByte/sec) */
/////////////////// 	limit rate 관련 끝	///////////////////////////////////////

	//WEON
	uint32_t		opened;
} service_info_t;

typedef enum {
	SRP_INIT = 0, 	/* 초기 상태 */
	SRP_RETRY, 		/* 비동기 method 리턴 상태 */

	SRP_REQUEST, 	/* request 처리 상태 */
	SRP_READ_PRE, 	/* body data를 thread에서 읽기전 */
	SRP_READ_DOING, /* body data를 thread에서 읽는중 */
	SRP_READ_DONE, 	/* body data를 thread에서 읽는 과정 종료 */
	SRP_CLOSE,		/* client 응답 종료후 후처리 상태 */
	SRP_WAIT_JOB,		/* nc_open_extended_apc(), nc_read_apc()를 호출한 thread의 처리가 끝나지 않은 상태에서 callback이 호출된 상태 */
	SRP_DELAYED_CLOSE		/* client의 비정상 종료로 connection handle이 free된 상태 */
} scx_run_phase_t;


typedef enum {
	HTTP_IF_NULL = 0, 			/* NO CONDITIONAL request */
	HTTP_IF_NOT_MATCH = 1, 	/* IF_NOT_MATCH */
	HTTP_IF_MATCH = 2, 		/* IF MATCH */
	HTTP_IF_IMS = 3, 		/* IF_MODIFIED_SINCE */
	HTTP_IF_IUS = 4, 		/*IF_UNMODIFIED_SINCE */
	HTTP_IF_IR = 5 			/*IF_RANGE */
} scx_iftype_t;

#define RANGE_UNDEFINED 	-1
typedef struct {
	int64_t 	begin;
	int64_t 	end;
	int64_t 	size;	/* 전송 크기, 'bytes=-????'의 요청일때에만 설정됨 */
	uint8_t 	operated:1;
} scx_range_t;


typedef struct response_tag {
	int 				trigger;	//lua script를 실행한 후에 다음의 동작에 대한 값이 들어간다. scx_trigger_ret_t에 지정된 값만 허용
	int 				code;		// http response code
	void				*body;		// 만들어진 body를 저장하는 버퍼, 만들어진 미디어를 임시 저장하는 용도로도 사용됨, 세션 종료시에 메모리 해제를 별도로 해주어야함.
	int					body_len;	// body의 실제 사용 크기
	nc_xtra_options_t	*headers;	// custom response header를 여기에 추가하면 됨
} response_t;

typedef struct {
} scx_req_get_t;
typedef struct {
	nc_stat_t 			objstat;
} scx_req_head_t;
typedef struct {
	int                 oob_ring_opened;
	char 				dummy[1];
} scx_req_post_t;
typedef struct {
	int64_t 			offset;
} scx_req_put_t;

typedef enum {
	HR_GET 		= 0,
	HR_HEAD 	= 1,
	HR_POST 	= 2,
	HR_PUT 		= 3,
	HR_DELETE 	= 4,
	HR_OPTIONS 	= 5,
	HR_PURGE 	= 6,
	HR_PRELOAD 	= 7,
	HR_STAT 	= 8,
	HR_PURGEALL	= 9,
	HR_UNKNOWN 	= 10
} nc_method_t;

typedef enum {
	STEP_START = 0,
	STEP_PRE_OPEN,
	STEP_DOING_OPEN,
	STEP_POST_OPEN,
	STEP_VERIFY_CONDITION,
	STEP_REQUEST_DONE,	/* scx_finish_response()에서 MHD_queue_response() 호출시 MHD_NO가 리턴되는 경우 예외 처리용 */
	STEP_PRE_READ,
	STEP_DOING_READ,
	STEP_POST_READ
} flow_process_step_t;


/* tproxy 기능에 사용되는 구조체임, httpn_request.h에도 동일한 내용이 들어있어야 한다. */
typedef struct client_info_tag {
	int		size;	/* client_info_tag의 크기가 들어 간다. solproxy와 httpn간의 같은 헤더를 쓰는지 검증용 */
	char  	ip[64];
} client_info_t;

/* 비동기 처리용 context */
typedef struct scx_async_ctx_tag {
	void		*cr;			/* nc_request_t *req */
	scx_run_phase_t phase; 		/* 초기는 INIT */
	uint64_t 	pos; 			/* reader callback에서 사용 */
	size_t 		max;			/* reader callback에서 사용 */
	char 		*buf;
	int			reader_type;	/* object_reader() : 0, streaming_reader : 1 */
	int			toe;			/* complete callback에서 사용, MHD_RequestTerminationCode */
	int			skip_mtime;		/* 전송 속도 제한 에서 사용, 단위 : msec */
	int			res;			/* worker thread 실행 결과를 저장 */
	int			is_working;		/* worker thread에서 job을 진행중인 경우 1로 셋팅, 당연히 처음 할당 시는 0 */
	int			job_avail; 		/* worker에서 작업중인 경우 connection에 대한 처리를 할때 이 값이 0이면 그냥 리턴해야함. */
	///////////////////////////////////////////
	//아래 3개의 변수는 scx_wait_job_completed() 에서만 사용
	nc_file_handle_t 	*file;
	nc_stat_t 			objstat;
	int 				xfered;
	///////////////////////////////////////////
} scx_async_ctx_t;

typedef struct tls_info_tag {
	gnutls_protocol_t proto_ver;	/* ssl 프로토콜 버전 정보 */
	gnutls_kx_algorithm_t kx_algorithm;
	gnutls_cipher_algorithm_t cipher_algorithm;
	gnutls_mac_algorithm_t mac_algorithm;
} tls_info_t;

typedef struct request_tag {
	struct request_mem_pool	*pool;
	uint64_t			id;		/* unique ID */
	nc_file_handle_t 	*file;
	nc_method_t 		method;

	struct service_info_tag 	*service;
	struct scx_config_tag		*config;
	scx_async_ctx_t				async_ctx; /* 비동기 처리용 context */
	scx_async_ctx_t				async_ctx_2th; /* netcache 비동기 API 처리용 context */
	tls_info_t					*tls_info;	/* tls 정보 */
	//char 				*mimetype;
	vstring_t 			*zmethod; 	/* LUA MAP */
	vstring_t 			*host; 		/* LUA MAP */
	vstring_t 			*path; /* url decoding된 요청 path, 현재 인증에서만 사용  , LUA MAP*/
	vstring_t 			*ext;	/* 요청 파일의 확장자 */
	vstring_t 			*url; /* client에서 요청된 url, <path>?<query>, 단순 로그 기록용, LUA MAP */
	vstring_t			*ori_url; /* 실제 origin에 요청될 url, 파싱등 작업은 ori_url을 사용해야함. LUA MAP */
	struct streaming_tag		*streaming;
	vstring_t 			*version;
	vstring_t			*jwt_token;	/* rtmp 요청에 jwt token이 포함된 경우만 사용 */
	int 				p1_error; /* phase 1 error */
	int					file_errno;	/* file operation 과정에서 발생한 error code */
	union {
		scx_req_get_t 	get;
		scx_req_post_t 	post;
		scx_req_head_t 	head;
		scx_req_put_t 	put;
	} u;
	flow_process_step_t step;	/* NetCache 비동기 API 호출시 사용 */
	vstring_t 		*client_ip; /* LUA MAP */
	vstring_t 		*server_ip; /* LUA MAP */
	int 			client_port;
	int 			server_port;
	vstring_t 		*object_key;   /* LUA MAP */
	vstring_t 		*property_key; /* LUA MAP */

	nc_mode_t 			mode; /* nc_open 모드 , LUA MAP */
	int				connect_type;	/* LUA MAP, 0:http, 1:https, 2:tproxy, 접속한 서비스 포트, scx_port_type_t에 정의 되어 있음 */
	/*
	 * 아래에 대해서 LUA 매핑 방법 고민해야함
	 */
	scx_iftype_t 	condition;
	vstring_t		*etag;
	time_t 		timeanchor;  /* IMS, IUS */
	scx_range_t range; /* subrange=1일때 유효*/
	uint32_t 	subrange:1;
	uint32_t 	isopenapi:1; 		/* 1: openapi 요청인 경우 설정 */
	uint32_t 	inprogress:1;
	uint32_t 	nocache:1; 		/* LUA MAP */
	uint32_t	private:1;		/* LUA MAP */
	uint32_t 	keepalive:1;
	uint32_t 	verbosehdr:1;
	uint32_t	norandom:2;		/* 1인 경우 origin에 Full GET 요청을 한다. */ /* LUA MAP */
	uint32_t    secured:1;  	/* 1:https, 0:http */
	uint32_t 	respvia:1; 		/*1:응답에 via있음 */
	uint32_t	purge_key:1;	/* 1인 경우 object key기준 purge, 0인 경우 url 기준 purge , X-Cache-ID 헤더가 있으면 key로 사용*/
	uint32_t	purge_hard:1;	/* 1인 경우 hard purge, 0인 경우 soft purge */
	uint32_t	purge_volume:1;	/* 1인 경우 volume purge, 0인 경우 일반 purge */
	uint32_t	disable_status:1;	/* 1인 경우 통계에 사용안함 */
	uint32_t	is_suspeneded:1; 	/* 1인 경우 suspend 상태, 비동기 처리용 flag */
	uint32_t	kv_oob_write_error:1; 	/* POST 처리시 문제가 발생한 경우 설정, */
	uint32_t	stay_suspend:1;	/* scx_run_async_job에서 scx_sync_reader()호출후 이 값이 1인 경우 suspend 상태를 유지 한다. cmaf 의 경우만 사용*/
	uint32_t	skip_auth:1;	/* lua script 를 사용해서 특정 컨텐츠에 대해서 인증 기능을 OFF, 1로 설정되면 jwt 인증을 하지 않는다. LUA MAP */
	uint32_t	pretty:1;		/* json 응답시 공백과 개행문자를 사용해서 보기 좋게 응답한다. */
	uint32_t	resp_body_compressed:1;	/* 응답 body가 gzip으로 압축된 경우 1로 표시 */
	uint32_t 	resultcode;

	uint32_t	toe;	/* client의 연결 종료 코드 */
	int64_t 	cursor;		/* 전송해야 하는 파일의 현재 위치 */
	nc_ssize_t 	remained;	/* 전송해야할 크기 */
	int64_t 	objlength;	/* 파일을 전체 크기 */
	int			copied;		/* netcache 비동기 read 완료시 리턴된 크기 */

	nc_stat_t 	objstat;	 /* object stat */
	struct MHD_Response *response;
	response_t	scx_res;	/* client에게 response를 보낼때 사용 */
	nc_xtra_options_t	*options;	/* origin에 전달하기 위한 헤더 및 netcache Core에 전달하기 위한 정보를 저장 */
	uint64_t 	oversion; /* origin version */
	vstring_t 			*ovalue; /* origin string */
	/////////////////// 	모듈 관련 시작	///////////////////////////////////////
	void		*module_ctx[MAX_MODULE_COUNT];		/* 모듈에서 사용하는 context */
	uint32_t	expired;		/* soluri2 인증검사시 만료가 된 경우 1을 셋팅 */
	/////////////////// 	모듈 관련 끝  	///////////////////////////////////////
	///////////////////  스트리밍 관련 시작	///////////////////////////////////////
	uint32_t			hls_playlist_protocol;		/* hls manifest 절대 경로 응답시 사용될 프로토콜, 0: http, 1:https */
	uint32_t			dash_manifest_protocol;		/* DASH menifest 응답에 사용하는 프로토콜. default:미지정 (들어온 요청의 프로토콜 사용), UNDEFINED_PROTOCOL:미지정, 0:http, 1:https  */
	char				*app_inst;				/* wowza url 동작시 요청된 "/application/instance" 경로 */
	///////////////////  스트리밍 관련 끝	///////////////////////////////////////
	/* book-keeping info */
	nc_hit_status_t hi;
	double 		t_nc_read;
	double 		t_nc_open;
	double 		t_nc_close;
	double 		t_req_fba; 		/* request first byte arrival time */
	double 		t_req_lba; 		/* request last byte arrival time */
	double 		t_res_fbs; 		/* response header first byte send time */
	double 		t_res_compl; 	/* response + body finished time */
	double 		t_zipper_build;	/* zipper library working time */
	double 		ts;
	double 		te;
	uint64_t 	req_hdr_size;
	uint64_t	req_url_size;	/* 요청된 uri(argument 포함, url decoding 전) 길이, 통계에 사용 */
	uint64_t 	req_body_size;
	uint64_t 	res_hdr_size;
	uint64_t 	res_body_size;
	struct MHD_Connection *connection;		/* lua에서 사용하기 위해 추가 */
	client_info_t	*client_info;	/* httpn 드라이버에서 tproxy 기능에서 사용 */
/////////////////// traffic limit rate 관련 시작	///////////////////////////////////////
	void		*limit_rate_timer;			/* limit_rate 에 사용되는 timer */
	double 		t_res_sent; 	/* response finished time. limit_rate 계산에만 사용. 다른곳에서 사용불가 */
	uint32_t 	limit_rate;			/* 세션별 전송속도 제한 (단위 : KByte/sec) */
	uint32_t 	limit_rate_after;	/* 설정된 크기 이후에 속도제한 (단위 : KByte) */
	uint32_t 	limit_traffic_rate;	/* 서비스별 속도제한 (단위 : KByte/sec) */
/////////////////// traffic limit rate 관련 끝	///////////////////////////////////////
	char			msg[256];		/* bt_timer 사용용도 */
	struct shm_log_body		*shm_log_slot;	/* 공유 메모리에 할당 받은 array 번호, -1은 할당을 받지 못한 경우이다 */
} nc_request_t;

typedef struct request_mem_pool mem_pool_t;
//#define	MP_ALIGN_BOUNDARY 	8
struct request_mem_pool {
	size_t 						asize; 		/* allocated size */
	size_t 						rsize;		/* free remaining size */
	off_t						fpos;		/* points to free offset to data */
	int 						count;		/* pool count */
	int 						subpool; 	/* 1 : subpool */
	struct request_mem_pool 	*next; 		/* 현재 pool의 추가 할당된 pool의 포인터*/
	struct request_mem_pool		*current; 	/* 현재 할당이 진행되고 있는 pool */
	char 						*memory;
};


#endif /* __COMMON_H__ */
