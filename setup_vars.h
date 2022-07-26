#ifndef __SETUP_VARS_H__
#define __SETUP_VARS_H__

#define 	SV_ORIGIN 				"origin"				/* default/site both, not inherited */
#define 	SV_PARENT 				"parent"				/* default/site both, not inherited */
#define 	SV_DOMAIN 				"domain"				/* site only, ex)"192.168.0.1, 192.168.0.2" */
#define		SV_MASTER_CACHE_DOMAIN	"master_cache_domain"	/* site only, not inherited, 오리진 캐시을 공유할 master 볼륨(domain) 명을 지정, 기본값 : 없음 */
#define 	SV_PROXY 				"proxy_address" 		/* default/site both, not inherited */
#define 	SV_FOLLOW_REDIR 		"follow_redirection" 	/* default/site both, inherited */
#define 	SV_NEGATIVE_TTL 		"negative_ttl"			/* default/site both, inherited */
#define 	SV_POSITIVE_TTL 		"positive_ttl"			/* default/site both, inherited */
#define		SV_ENABLE_FASTCRC		"fastcrc"				/* default only, 1이면 동작, 0이면 사용안함. 변경시 cache 삭제해야함 */
#define		SV_ENABLE_FORCE_CLOSE	"force_close"			/* default only, 1이면 동작, 0이면 사용안함 */
#define 	SV_READAHEAD_MB 		"ra_mb"					/* default only, Disk(캐싱된 경우)나 오리진(캐싱되지 않은 경우)으로 부터 read시 사용되는 크기, default : 4(MB) */
#define 	SV_CERTIFICATE_CRT 	 	"certificate_crt"		/* default/site both, inherited */
#define 	SV_CERTIFICATE_KEY	 	"certificate_key"		/* default/site both, inherited */
#define 	SV_ENABLE_SNI		 	"enable_sni"			/* default only, 1인 경우만 동작, default:1 */
#define 	SV_SSL_PRIORITIES	 	"ssl_priorities"		/* default only, ssl priorities 설정, default:NORMAL */
#define 	SV_ENABLE_HTTPS	 		"enable_https"			/* default/site, not inherited, 볼륨별 https 서비스를 허용 여부 설정. https허용:1, https차단:0, default : 0(차단) */
#if 0
#define 	SV_INMEMORY_OBJECT 		"inmemory_objects"		/* default only, obsolete in 3.0 */
#else
#define 	SV_INODE_SIZE	 		"inode_size"			/* default only, inode memory 크기를 지정, 단위: MB, 설정을 하지 않는 경우 cache_size에 따라 자동 설정 */
#endif
#define 	SV_MEMORY_CACHE 		"cache_size"			/* default only */
#define 	SV_CHUNK_SIZE 			"chunk_size"			/* default only, 변경시 cache 삭제해야함 */
#define 	SV_WRITEBACK_CHUNKS 	"writeback_chunks"		/* default/site, not inherited */
#define 	SV_DRIVER_CLASS 		"driver_class"			/* default/site, not inherited :HTTPN밖에 없음 */
#define 	SV_CACHE_PATH 			"cache_dir"				/* default only */
#define		SV_DISK_HIGH_WM			"disk_hwm"				/* default only */
#define 	SV_DISK_LOW_WM			"disk_lwm"				/* default only */
#define 	SV_MAX_PATH_LOCK_SIZE	"max_path_lock_size"	/* default only, path lock의 최대 크기를 지정, 기본값 : 10000 */
#define 	SV_DISK_MEDIA_CACHE_DIR					"disk_media_cache_dir"				/* default only, metadata disk cache 경로, 지정하지 않을 경우 cache 경로 아래에 media 폴더로 생성 */
#define 	SV_DISK_MEDIA_CACHE_HIGH				"disk_media_cache_high"				/* default only, metadata disk cache 최대 용량, 단위:MB, default:51200(50GB) */
#define 	SV_DISK_MEDIA_CACHE_LOW					"disk_media_cache_low"				/* default only, metadata disk cache 최소 용량, 단위:MB, default:46080(45GB) */
#define 	SV_DISK_MEDIA_CACHE_MONITOR_PERIOD		"disk_media_cache_monitor_period"	/* default only, metadata disk cache manager 동작 주기, 단위:초, default:60 */
#define 	SV_DISK_MEDIA_CACHE_ENABLE				"disk_media_cache_enable"			/* default only, metadata disk cache 사용 여부, 1:사용, 0:사용안함, default:1 */
#define 	SV_WORKERS 				"workers"				/* default only */
#define 	SV_POOL_SIZE 			"pool_size"				/* default only */
#define 	SV_MAX_VIRTUAL_HOST		"max_virtual_host"		/* default only , not inherited, default : 2500, 생성 가능한 최대 가상호스트(볼륨) 수*/
#define 	SV_CLUSTER_IP 			"cluster_ip"			/* default only */
#define 	SV_CLUSTER_TTL 			"cluster_ttl"			/* default only */
#define 	SV_LOG_DIR 				"log_directory"			/* default only */
#define 	SV_LOG_ACCESS			"log_access"			/* default/site, inherited, 0으로 설정된 경우만 로그기록을 하지 않는다.*/
#define 	SV_LOG_ERROR			"log_error"				/* default/site, inherited, 0으로 설정된 경우만 로그기록을 하지 않는다.*/
#define 	SV_LOG_ORIGIN			"log_origin"			/* default/site, inherited, 0으로 설정된 경우만 로그기록을 하지 않는다.*/
#define 	SV_HOST_NAME 			"host_name"				/* default only*/
#define 	SV_HTTPS_PORT 			"https_port"			/* default only*/
#define 	SV_HTTP_PORT 			"http_port"				/* default only*/
#define 	SV_TPROXY_PORT 			"tproxy_port"			/* default only*/
#define 	SV_HTTP_LISTEN_IP		"listen_ip"				/* default only*/
#define 	SV_MANAGEMENT_PORT	 	"management_port"		/* default only, PURGE, Preload, OpenAPI 요청 처리 전용 port, default : 61800 */
#define 	SV_MANAGEMENT_LISTEN_IP	"management_listen_ip"	/* default only, 지정된 IP로 들어온 management 요청만 처리, default : 없음 */
#define		SV_MANAGEMENT_ENABLE_WITH_SERVICE_PORT	"management_enable_with_service_port"		/* default only, 서비스 포트로 PURGE, Preload, OpenAPI 요청 허용, 기본값:0(허용안함), 0:허용안함, 1:허용, reload 가능 */
#define 	SV_USE_HEAD 			"head_for_property"		/* default/site, not inherited */
#define 	SV_LOG_FORMAT 			"log_format"			/* default only*/
#define 	SV_LOG_GMT_TIME 		"log_gmttime"			/* default only*/
#define 	SV_LOG_SIZE				"log_size"				/* default only */
#define 	SV_LOG_LEVEL			"log_mask"				/* default only */
#define 	SV_LOGROTATE_SIGNAL_ENABLE 	"logrotate_signal_enable"	/* default only, 1이면 logrotate와 연동, 0이면 연동 안함, default : 1 */
#define 	SV_ALLOW_DYNAMIC_VHOST	"allow_dynamic_vhost"	/* default only, 동적 볼륨 생성 여부, 1이면 허용, 0이면 차단, default : 1 */
#define 	SV_FORCE_SIGJMP_RESTART	"force_sigjmp_restart"		/* default only, sigsetjmp()를 사용한 구간에서 SIGSEGV 예외가 발생 했을때 재기동 여부, 1:재기동,0:재기동안함,기본값:0(재기동안함)*/
#define 	SV_IGNORE_HEADERS 		"ignore_headers"		/* default/site both, not inherited
															 * default에 해당 설정이 있는 경우, 
															 * 특정 서비스에서 ignore_header= 으로 
															 * default에 있는 설정을 override할 수 있음
															 */
#define 	SV_REMOVE_ORIGIN_REQEUEST_HEADERS	"remove_origin_request_headers"		/* site only, default : 없음, 오리진에 요청시 지정된 헤더를 빼고 요청. 헤더가 여러개인 경우 ,(컴마)로 구분 */
#define 	SV_REMOVE_ORIGIN_RESPONSE_HEADERS	"remove_origin_response_headers"	/* site only, default : 없음, 오리진 응답중 지정된 헤더를 삭제하고 캐싱한다. 헤더가 여러개인 경우 ,(컴마)로 구분 */
#define 	SV_IGNORE_QUERY	 		"ignore_query"			/* default/site, not inherited, default : 0, 오리진에 요청시 request url에 포함된 query paremeter를 제거하고 요청한다. 1(query parameter무시), 0(query parameter 포함) */
#define 	SV_ORIGIN_TIMEOUT 		"origin_timeout"		/* default/site, not inherited, 초단위,  SV_ORIGIN_CONNECT_TIMEOUT와 동일, 삭제예정*/
#define 	SV_ORIGIN_CONNECT_TIMEOUT  	"origin_connect_timeout"	/* default/site, not inherited, 초단위, default 3초 */
#define 	SV_ORIGIN_TRANSFER_TIMEOUT  "origin_transfer_timeout"	/* default/site, not inherited, 초단위, default 5초 */
#define 	SV_NC_READ_TIMEOUT  		"nc_read_timeout"	/* site only, nc_read() 최대 대기 시간 지정,  초단위, 설정하지 않는 경우 origin_transfer_timeout와 동일하게 설정됨  */
#define 	SV_ORIGIN_HEALTH_CHECK_URL	"origin_health_check_url"	/* site only, 오리진 health check(ONLINE 확인) 시 사용되는 url을 지정 */
#define 	SV_ORIGIN_MAX_FAIL_COUNT	"origin_max_fail_count"		/* default/site, not inherited, 초단위,  Origin 접속실패가 연속으로 설정값을 초과 하는 경우 OFFLINE, default는 2 */
#define 	SV_ORIGIN_PROTOCOL 		"origin_protocol"		/* default/site, not inherited, origin 접속시 http/https 사용 선택, http:0, https:1, default는 0 */
#define 	SV_ORIGIN_POLICY 		"origin_policy"			/* default/site, not inherited, origin의 Load Balance 정책 결정, 설정가능값(rr : RoundRobin, ps : Primary/Secondary, hash), default는 rr */
#define 	SV_PARENT_POLICY 		"parent_policy"			/* default/site, not inherited, parent의 Load Balance 정책 결정, 설정가능값(rr : RoundRobin, ps : Primary/Secondary, hash), default는 rr */
#define		SV_ORIGIN_ONLINE_CODE	"origin_online_code"	/* default/site, not inherited, 오리진 online 상태 확인시 5XX로 응답이 온 경우라도 해당 코드가 설정되어 있는 경우는 Online 상태로 판단. */
#define 	SV_ORIGIN_REQUEST_TYPE	"origin_request_type"	/* default/site, not inherited, origin에 요청시 Full GET으로 할지 Range GET으로 할지 지정, 설정가능값(2:Full GET후 range GET, 1:Full GET, 0:Range Get), default는 0 */
#define 	SV_REAL_USER 			"user"					/* default only */
#define 	SV_REAL_GROUP 			"group"					/* default only , 현재 사용안함*/
#define 	SV_FRESH_CHECK_POLICY 	"fresh_check_policy"	/* default/site, not inherited, 설정가능값 : mtime,size,devid,alwaysexpire */
#define		SV_HANG_DETECT			"hang_detect"			/* default only, default : 1, hang 감지 모니터링, 1:동작, 0:동작안함 */
#define		SV_HANG_DETECT_TIMEOUT	"hang_detect_timeout"	/* default only, default : 행감지 요청 timeout */
#define		SV_NCAPI_HANG_DETECT			"ncapi_hang_detect"			/* default only, API 행감지 기능 동작 여부, default : 1, 1:동작, 0:동작안함 */
#define		SV_NCAPI_HANG_DETECT_TIMEOUT	"ncapi_hang_detect_timeout"	/* default only, API 행감지 timeout, default : 120, 단위 : 초 */
#define		SV_CLEAN_START			"clean_start"			/* default only, default : 0, 데몬 기동시 모든 volume에 대해 volume purge 실행, 1:동작, 0:동작안함 */

#define		SV_ENABLE_ASYNC_CALLBACK	"enable_async_callback"		/* default, default : 1, 비동기 처리 여부, 1 : 비동기, 0 : 동기 */
#define		SV_ENABLE_ASYNC_NCAPI		"enable_async_ncapi"		/* default, default : 0, netcache 비동기 api(open, read) 사용 여부, 1:사용, 0:사용안함 */
#define		SV_JOB_THREAD_COUNT		"job_thread_count"		/* default, 비동기 job thread의 수, 설정되지 않는 경우 workers * 2로 설정됨 */
#define		SV_MAX_JOB_QUEUE_LEN	"max_job_queue_len"		/* default, 비동기 동작시 job queue의 최대 크기, 설정되지 않는 경우 job thread의 수와 동일 */
#define		SV_ENABLE_ASYNC_ACCESSHANDLER_CALLBACK	"enable_async_accesshandler_callback"	/*  default/site,  inherited, AccessHandlerCallback시 비동기(별도의 job thread 사용) 기능을 사용 여부 설정, default:1(비동기), 1(비동기), 0(동기) */
#define		SV_ENABLE_ASYNC_READER_CALLBACK			"enable_async_reader_callback"			/*  default/site,  inherited, ContentReaderCallback시 비동기(별도의 job thread 사용) 기능을 사용 여부 설정, default:1(비동기), 1(비동기), 0(동기) */
#define		SV_ENABLE_ASYNC_COMPLETE_CALLBACK		"enable_async_complete_callback"		/*  default/site,  inherited, RequestCompletedCallback시 비동기(별도의 job thread 사용) 기능을 사용 여부 설정, default:1(비동기), 1(비동기), 0(동기) */

//#define 	SV_PID_FILE				"pid_file"				/* default only, ex)/var/run/solproxy.pid */
//#define 	SV_LUA_POOL_SIZE		"lua_pool_size"			/* default only */
#define 	SV_COLD_CACHE_ENABLE	"cold_cache_enable"		/* default only */
#define 	SV_COLD_CACHE_RATIO		"cold_cache_ratio"		/* default only */
#define 	SV_REQUEST_TIMEOUT		"request_timeout"		/* default only, Client가 접속후에 request를 보내기까지의 허용 시간. 단위 : 초, default : 5, 0으로 설정한 경우 timeout 제한 없음 */
#define 	SV_SEND_TIMEOUT			"send_timeout"			/* site only, Client가 데이터를 받는도중 pause를 하거나 Client의 네트웍이 갑자기 단절 되거나 했을때 적용되는 timeout. 단위 : 초, default : 120, 0으로 설정한 경우 timeout 제한 없음 */
#define 	SV_KEEPALIVE_TIMEOUT	"keepalive_timeout"		/* site only, Client가 요청한 데이터를 전송한 상태에서 다음 request를 받기 위해 socket 연결을 유지 하는 시간. 단위 : 초, default : 10, 0으로 설정한 경우 timeout 제한 없음 */
#define 	SV_CORE_FILE_PATH		"core_file_path"		/* default only */
#define 	SV_USE_SELECT			"use_select"			/* default only, 1로 설정된 경우만 select 사용, polling_policy와 동일 기존과 호환성을 위해 남겨둠 */
#define 	SV_POLLING_POLICY		"polling_policy"		/* default only, 1로 설정된 경우만 select 사용 */
#define 	SV_STAT_SOCKET_PATH 	"stat_socket_path"		/* default only, 통계 전송용 unix domain sock 파일 경로 */
#define 	SV_STAT_ENABLE			"stat_enable"			/* default/site , not inherited, 수집된 통계 정보를 statd에 전송 여부, default : 1, 1 :통계 전송, 0 :통계 전송 안함 */

#define		SV_SOLURI2_SECRET		"soluri2_secret"		/* default/site, not inherited, default : 없음, soluri2 인증에 사용되는 키, 설정이 되면 인증을 사용하고, 설정되지 않으면 인증을 사용하지 않음, 컴마로 구분되어서 여러개의 키를 설정가능 */
#define		SV_AUTH_START_TIME		"auth_start_time"		/* default/site, not inherited, default : 60, 단위: 초, 설정이 되면 서버 시작 후 지정된 시간동안 soluri2 인증시 토큰 인증 시간이 만료 되더라도 인증 성공으로 한다. */
#define		SV_SOLJWT_SECRET		"soljwt_secret"			/* site only, default : 없음, jwt 인증에 사용되는 키, 설정이 되면 인증을 사용하고, 설정되지 않으면 인증을 사용하지 않음, 컴마로 구분되어서 여러개의 키를 설정가능 */
#define		SV_SOLJWT_CLAIMS		"soljwt_claims"			/* site only, default : 없음, jwt 인증에 사용되는 기본 claim인 exp와 path외의 인식할 클레임을 지정, 컴마(,)와 콜론(:) 구분되어서 여러개 설정가능. 예: soljwt_claims = alowip:clientip, alowcc:alowcc  */

#define 	SV_BASE_PATH			"base_path"				/* default/site, not inherited, origin에 파일을 요청을 할 경우 base_path+request path가 된다. */
#define 	SV_ORIGIN_HOSTNAME		"origin_hostname"		/* default/site, not inherited, 지정된 경우 origin 접속시의 hostname에 이값을 사용한다. */
#define 	SV_CUSTOM_USER_AGENT	"custom_user_agent"		/* default/site, not inherited, 설정된 값으로 User-Agent 헤더를 바꿔서 오리진에 요청, 0으로 설정되는 경우 Client로 부터 들어온 User-Agent를 오리진에 전달 */
#define 	SV_BYPASS_METHOD		"bypass_method"			/* default/site, not inherited, 지정된 method들을 오리진으로 bypass 한다, 여러 method를 적을 경우 컴마(,)로 구분한다. default:없음 */
#define		SV_REMOVE_DIR_LEVEL		"remove_dir_level"		/* site only, 볼륨도메인으로 변환하기 전에 볼륨 구분용으로 사용된 첫번째 디렉토리를 제거 할지 여부, 1이면 첫번째 디렉토리를 빼고 볼륨으로 전달, 0이면 요청 들어온 url을 그대로 전달, default : 0 */
#define		SV_SUB_DOMAIN_LIST		"sub_domain_list"		/* site only, url에 들어있는 첫번째 디렉토리와 볼륨 도메인 매핑 정보, "볼륨명:볼륨도메인,볼륨명:볼륨도메인", default : 없음 */

#define 	SV_DNS_CACHE_TTL 		"dns_cache_ttl"			/* site only, 오리진 요청시 사용되는 DNS cache의 유효기간, default : 60(초), 0:cache 사용 안함, -1:cache 제한없음 */
#define 	SV_REFERERS_ALLOW 		"referers_allow"		/* site only, 지정된 referer 헤더에 대해서만 허용, default : 없음, 컴마(,)를 사용해서 여러개 지정 가능 */
#define 	SV_REFERERS_DENY 		"referers_deny"			/* site only, 지정된 referer 헤더가 들어 오면 차단, default : 없음, 컴마(,)를 사용해서 여러개 지정 가능 */
#define 	SV_REFERER_NOT_EXIST_ALLOW 		"referer_not_exist_allow"			/* site only, referer 헤더가 없는 경우 차단 여부, default : 1(허용), 0이면 차단, 1이면 허용 */
///////////////////////////////////////// Content Router 기능 관련 시작 ////////////////////////////////////////////
#define		SV_CR_SERVER			"cr_server"				/* default only, redis proxy 서버의 주소와 port, 복수의 서버 설정시 세미콜론(;)으로 구분, ip:port, reload 안됨 */
#define		SV_CR_ENABLE			"cr_enable"				/* default only, content router 기능 사용 여부, 1:사용, 0:사용안함, reload 가능 */
#define		SV_CR_UPDATE_ENABLE		"cr_update_enable"		/* default only, redis proxy 서버에 자신의 정보를 업데이트 할지 여부, 기본값:1(기능사용), 0:업데이트안함, 1:업데이트함, reload 가능 */
#define		SV_CR_UPDATE_DURATION	"cr_update_duration"	/* default only, redis proxy 서버에 정보를 업데이트 하는 주기, 단위:초, 기본값:2(초), reload 가능 */
#define		SV_CR_THRESHOLD_TRAFFIC	"cr_threshold_traffic"	/* default only, 지정된 트래픽을 넘기는 경우 해당 서버가 선정되더라도 content routing을 하지 않는다. 단위:Mbps, reload 가능 */
#define		SV_CR_THRESHOLD_CPU		"cr_threshold_cpu"		/* default only, 지정된 cpu 사용률을 넘기는 경우 해당 서버가 선정되더라도 content routing을 하지 않는다. 단위:퍼센트(100%가 최고임), reload 가능 */
#define		SV_CR_TTL				"cr_ttl"				/* default only, redis 에 서버 정보 기록시 설정되는 TTL, 단위:초, reload 가능 */
#define		SV_CR_ALIVE_CHECK_TIMEOUT	"cr_alive_check_timeout"	/* default only, 자신의 hang 상태를 확인하기 위해 자신의 서비스 포트로 요청을 할때 사용되는 timeout 시간, 단위:초, default:1(초), reload 가능 */
#define		SV_CR_GROUP				"cr_group"				/* default only, redis proxy에 등록시 key 앞에 붙는  prefix, reload 가능, 기본값 : cr */
#define		SV_CR_SERVER_DOMAIN		"cr_server_domain"		/* default only, 서버에 접근 가능한 domain 이나 IP, reload 가능 */
#define		SV_CR_POLICY			"cr_policy"				/* default only, content routing 과정에서 traffic이나 CPU 사용률을 넘긴 서버가 선정되는 경우 정책, 기본값:0(다른 서버 선정),0:다른 서버 선정, 1:routing 안함 */
#define		SV_CR_SERVICE_ENABLE	"cr_service_enable"		/* site only, 서비스(볼륨) 별 content router 기능 사용여부, 1:사용, 0:사용 안함, default:0(사용안함) */
#define		SV_CR_HOT_CONTENT_DAY	"cr_hot_content_day"	/* site only, 파일이 만들어진 시점(mtime)을 기준으로 지정된 날짜가 지난 경우(cold content)만 content routing 처리한다. 단위:일(day), 기본값:10(일) */
///////////////////////////////////////// Content Router 기능 관련 끝   ////////////////////////////////////////////
///////////////////////////////////////// Streaming 기능 관련 시작 ////////////////////////////////////////////
#define		SV_STREAMING_ENABLE				"streaming_enable"			/* default/site, not inherited, default : 0, cache에서 streaming 기능 사용(0:사용안함, 1 사용) */
#define		SV_STREAMING_TARGET_DURATION	"streaming_target_duration"	/* default/site, not inherited */
#define		SV_SUBTITLE_TARGET_DURATION		"subtitle_target_duration"	/* site only, default : 60, 자막 segment의 생성시간, 단위 : 초*/
#define		SV_QUICK_START_SEG				"quick_start_seg"			/* default/site, not inherited ,퀵스타트를 위한 초기 최소 분할 세그먼트의 갯수(단위 : 초, default : 0)*/
#define		SV_SESSION_ENABLE				"session_enable"			/* default/site, inherited, default : 0 */
#define		SV_SESSION_TIMEOUT				"session_timeout"			/* default/site, inherited, default : 30 */
#if 0
#define		SV_SMIL_ORIGIN					"smil_origin"				/* default/site, not inherited, smil 파일을 받을 서버를 별도로 지정. 없을때에는 origin 설정을 따른다.*/
#endif
#if 0
#define		SV_STREAMING_PROTOCOL			"streaming_protocol"		/* default/site, not inherited, 복수의 protocol을 설정 가능(comma로 구분), streaming_allow_protocol로 대체 됨(삭제예정). default : progressive,hls,dash */
#else
#define		SV_STREAMING_ALLOW_PROTOCOL		"streaming_allow_protocol"	/* default/site, not inherited, 복수의 protocol을 설정 가능(comma로 구분), default : progressive,hls,dash */
#endif
#define		SV_STREAMING_MODE				"streaming_mode"			/* default/site, not inherited, 복수의 mode 설정 가능(comma로 구분), default : single,multi,adaptive */
#if 0
#define		SV_STREAMING_TYPE				"streaming_type"			/* default/site, not inherited, streaming_method로 대체됨(삭제예정), 0인 경우 VOD,  1인 경우 Live, 2인 경우 cmaf로 동작, default : 0 */
#else
#define		SV_STREAMING_METHOD				"streaming_method"			/* default/site, not inherited,  0인 경우 VOD,  1인 경우 Live 광고 삽입, 2인 경우 cmaf로 동작, default : 0 */
#endif
#define		SV_EANBLE_WOWZA_URL				"enable_wowza_url"			/* site only, wowza의 url 패턴을 사용할지 여부, 0:사용안함, 1:사용, 기본값:0(사용안함) */
#define		SV_CACHE_REQUEST_ENABLE			"cache_request_enable"		/* site only, 스트리밍 요청에 대해서만 허용하고 cache에 대해서 허용을 할건지에 대한 여부 설정, 0:cache요청거부, 1:cache 청허용, 기본값:1(cache요청허용) */
#define		SV_STREAMING_MEDIA_EXT			"streaming_media_ext"		/* site only, 지정 값과 요청에 포함된 확장자가 동일하면 스트리밍 컨텐츠로 인식해서 session 기능 사용가능, 복수로 설정 가능(컴마로 구분), 기본값:없음 */
#define		SV_IO_BUFFER_SIZE				"io_buffer_size"			/* default only, zipper 라이브러리에서 파일을 읽을때 사용되는 버퍼 크기(단위 : KBYTE, default : 256) */
#define		SV_BUILDER_CACHE_SIZE			"builder_cache_size"		/* default only, builder index cache 크기 지정(default : 2000), 현재 사용안함 */
#define		SV_MEDIA_CACHE_SIZE				"media_cache_size"			/* default only, media index cache 크기 지정(default : 2000) */
#define		SV_MEDIA_NEGATIVE_TTL			"media_negative_ttl"		/* site only, metadata parsing시 문제가 발생한 컨텐츠에 대해 지정된 시간동안 동일 요청 차단, 단위 : 초, 기본값:1(초), -1을 지정할 경우 동작안함 */
#define		SV_USE_LOCAL_FILE				"use_local_file"			/* default/site, inherited, default : 0, origin 서버를 사용하지 않고 local file을 사용함. 0:origin사용, 1:local file 사용 */
#define		SV_ALLOW_DIFFERENT_PROTOCOL		"allow_different_protocol"	/* default/site, not inherited, default : 1(허용), menifest에 지정된 프로토콜이 아닌 다른 프로토콜로 접근했을때 허용여부. 1:허용, 0:허용안함 */
#define		SV_HLS_PLAYLIST_HOST_DOMAIN		"hls_playlist_host_domain"	/* default/site, not inherited, default : 사용안함, 설정된 경우 HLS의 manifest 생성시 설정된 값을 사용해서 절대 경로를 생성한다. */
#define		SV_HLS_MASTER_PLAYLIST_USE_ABSOLUTE_PATH	"hls_master_playlist_use_absolute_path"	/* default/site, not inherited, default : 사용안함, 설정된 경우 HLS의 manifest 생성시 설정된 값을 사용해서 절대 경로를 생성한다. */
#define		SV_HLS_MEDIA_PLAYLIST_USE_ABSOLUTE_PATH		"hls_media_playlist_use_absolute_path"	/* default/site, not inherited, default : 사용안함, 설정된 경우 HLS의 manifest 생성시 설정된 값을 사용해서 절대 경로를 생성한다. */
#define		SV_HLS_ENCRYPT_METHOD			"hls_encrypt_method"		/* default/site, not inherited, default : NONE, 설정된 경우 HLS segment(TS 생성)시 지정된 암호화를 한다. */
#define		SV_HLS_ENCRYPT_KEY				"hls_encrypt_key"			/* default/site, not inherited, default : 없음, HLS segment(TS 생성)시 사용되는 암호화 key */
#define		SV_HLS_PLAYLIST_PROTOCOL		"hls_playlist_protocol"		/* default/site, not inherited, default : 없음, HLS manifest 응답에 사용하는 프로토콜. default : 없음. http, https 가능 */
#define		SV_HLS_PERMIT_DISCONTINUTY		"hls_permit_discontinuty"	/* default/site, not inherited, default : 0(허용안함), HLS로 Zipping시 인코딩 속성이 다른 경우에 EXT-X-DISCONTINUITY 태그를 사용할수 있도록 허용. 0(허용안함), 1(허용) 가능 */
#define		SV_HLS_DISCONTINUTY_ALL			"hls_discontinuty_all"		/* site, not inherited, default : 0, live 광고 stitching시 광고 ts에 모두 EXT-X-DISCONTINUITY 를 붙인다. 1 : 사용 , 0 : 사용안함 */
#define		SV_HLS_ENCRYPT_KEY_PROTOCOL		"hls_encrypt_key_protocol"	/* default/site, not inherited, default : http, HLS의 인증키 요청에 사용하는 프로토콜. http, https 가능 */
#define		SV_HLS_SEGMENT_PROTOCOL			"hls_segment_protocol"		/* default/site, not inherited, default : http, HLS의 segment 요청에 사용하는 프로토콜. http, https 가능 */
#define		SV_ID3_RETAIN					"id3_retain"				/* default/site, not inherited, default : 0, mp3, flac 파일에서 id3 정보를 유지 할지 여부 결정. 0:유지안함, 1:유지 */

#define		SV_CDN_DOMAIN_POSTFIX			"cdn_domain_postfix"		/* default/site, inherited, cdn 별 main 도메인, 이 값을 사용해서 fhs 도메인으로 들어온 요청을 볼륨으로 도메인으로 변환한다. */
#define		SV_MANIFEST_PREFIX_PATH			"manifest_prefix_path"		/* site only, default:없음, fhs 도메인을 사용하는 경우 manifest 응답시 path의 첫 디렉토리에 들어갈 경로(서비스명-볼륨명 형식) */
#define		SV_PERMIT_INIT_EXPIRE_DURATION	"permit_init_expire_duration"	/* default only, default:60(초), 인증을 사용하는 경우 서비스 데몬 기동후 설정된 시간 내에 들어 오는 요청에 대해서 expire된 요청이라도 일정시간 동안 허용해준다. */
#define		SV_VOL_SERVICE_TYPE				"vol_service_type"			/* site only, default:0(일반 볼륨), 해당 볼륨의 type을 지정, 0 : 일반 볼륨, 1 : 대표 도메인 서비스용 볼륨, 2 : FHS 도메인 서비스용 볼륨*/

#define		SV_AD_ORIGIN					"ad_origin"					/* default/site, not inherited, 광고용 vod 오리진 서버 주소 */
#define		SV_AD_ORIGIN_POLICY				"ad_origin_policy"			/* default/site, not inherited, 광고용 vod origin의 Load Balance 정책 결정, 설정가능값(rr : RoundRobin, ps : Primary/Secondary, hash), default는 hash */
#define 	SV_AD_ORIGIN_PROTOCOL 			"ad_origin_protocol"		/* default/site, not inherited, 광고용 vod origin 접속시 http/https 사용 선택, http:0, https:1, default는 0 */
#define 	SV_AD_NEGATIVE_TTL 				"ad_negative_ttl"	/* default/site both, inherited */
#define 	SV_AD_POSITIVE_TTL 				"ad_positive_ttl"	/* default/site both, inherited */
#define 	SV_AD_FOLLOW_REDIR 				"ad_follow_redirection" 	/* default/site both, inherited */
#define 	SV_AD_BASE_PATH					"ad_base_path"					/* default/site, not inherited, origin에 광고 파일을 요청을 할 경우 base_path+request path가 된다. */
#define 	SV_AD_ORIGIN_HOSTNAME			"ad_origin_hostname"			/* default/site, not inherited, 지정된 경우 광고 origin 접속시의 hostname에 이값을 사용한다. */
#define 	SV_AD_FRESH_CHECK_POLICY 		"ad_fresh_check_policy"			/* default/site, not inherited, 설정가능값 : mtime,size,devid,alwaysexpire */
#define 	SV_AD_ALLOW_MEDIA_FAULT			"ad_allow_media_fault"			/* site, not inherited, 광고로 지정된 컨텐츠가 없거나 문제가 있을 경우 해당 광고만 제외하고 zipping을 할지, 아니면 404에러를 리턴할지 결정,  0:허용안함,1:허용, default:1(허용) */

#define 	SV_CMAF_ORIGIN 						"cmaf_origin"					/* site, not inherited, CMAF 오리진 서버 주소, http/https까지 지정 및 포트지정, 2개까지 지정가능(컴마로 구분), 예: http://localhost:9080 */
#define 	SV_CMAF_ORIGIN_BASE_PATH 			"cmaf_origin_base_path"			/* site, not inherited, CMAF 오리진 요청시 요청 경로의 앞에 추가 경로를 붙이는 경우 사용 */
#define 	SV_CMAF_ORIGIN_PATH 				"cmaf_origin_path"				/* site, not inherited, CMAF 오리진에 요청되는 mpd 파일의 경로, cmaf_base_path가 설정 된 경우 오리진에는 cmaf_base_path + cmaf_path 가 된다. */
#define 	SV_CMAF_ENABLE_TUNNEL 				"cmaf_enable_tunnel"			/* site, not inherited, CMAF tunneling 기능 on/off, 1 : on, 0 : off, default : 0, 현재 미구현 */
#define 	SV_CMAF_ORIGIN_TIMEOUT 				"cmaf_origin_timeout"			/* site, not inherited, CMAF 오리진에서 응답하지 않는 경우 timeout 설정, default : 5(초)*/
#define 	SV_CMAF_ORIGIN_DISCONNECT_TIME		"cmaf_origin_disconnect_time"	/* site, not inherited, CMAF에서 일정시간동안 사용자의 요청이 없는 경우 오리진과의 연결을 해제, 0으로 설정된 경우는 사용자 요청이 없더라도 연결을 끊지 않음, 단위:초, default:300 */
#define 	SV_CMAF_ORIGIN_CONNECT_ONLOAD		"cmaf_origin_connect_onload"	/* site, not inherited, CMAF 볼륨 생성시 혹은 설정 변경시 오리진에 연결한 상태로 시작할지를 결정, 1:서비스 시작시 오리진 연결, 0:서비스시작시 오리진 연결 안함,사용자 요청이 들어온 시점에 오리진 요청, default:0 */
#define 	SV_CMAF_FRAGMENT_CACHE_COUNT 		"cmaf_fragment_cache_count"		/* site, not inherited, 메모리 상에 캐시될 Fragment의 최대 수, default:10 */
#define 	SV_CMAF_PREFETCH_FRAGMENT_COUNT 	"cmaf_prefetch_fragment_count"	/* site, not inherited, 오리진 연결시 가져올 과거 Fragment수, default:0 */
#define 	SV_CMAF_MANIFEST_FRAGMENT_COUNT 	"cmaf_manifest_fragment_count"	/* site, not inherited,매니패스트 상에 기술될 Fragment의 최대 수, default:5 */
#define 	SV_DASH_MANIFEST_HOST_DOMAIN 		"dash_manifest_host_domain"		/* site, not inherited, 설정된 경우 절대경로로 DASH manifest 응답시 host(ip도 가능) 까지 포함해서 응답한다. */
#define 	SV_DASH_MANIFEST_USE_ABSOLUTE_PATH	"dash_manifest_use_absolute_path"	/* site, not inherited, 설정된 경우 DASH manifest 생성시 설정된 값을 사용해서 절대 경로를 생성한다. 1이면 절대경로, 0이면 상대경로, default:0 */
#define 	SV_DASH_MANIFEST_PROTOCOL 			"dash_manifest_protocol"		/* site, not inherited, DASH manifest 응답에 사용하는 프로토콜. default:미지정(들어온 요청의 프로토콜 사용). http, https 가능 */

#define 	SV_HLS_MODIFY_MANIFEST_ENABLE			"hls_modify_manifest_enable"			/* site, HLS Manifest 변경 기능 사용 여부 설정, 1:사용, 0:사용안함, 기본값:0(사용안함) */
#define 	SV_HLS_MODIFY_MANIFEST_EXT				"hls_modify_manifest_ext"			/* site, 지정된 확장자로 들어 오는 경우 manifest로 인식해서 modification 기능 동작, 확장자는 대소문자 구분하지 않음, 기본값:m3u8*/
#define 	SV_HLS_MODIFY_MANIFEST_TTL				"hls_modify_manifest_ttl"			/* site, manifest 의 TTL을 별도 지정, 설정하지 않는 경우 positive_ttl 값을 사용한다, 단위:초 */
#define 	SV_HLS_MODIFY_MANIFEST_SESSION_ENABLE	"hls_modify_manifest_session_enable"	/* site, 신규 요청이 들어 올때마다 session ID를 추가로 달아서 응답, 1:사용, 0:사용안함, 기본값:0(사용안함) */
#define 	SV_HLS_MODIFY_MANIFEST_REDIRECT_ENABLE	"hls_modify_manifest_redirect_enable"	/* site, manifest 요청에 session이 포함되어 있지 않은 경우 session을 추가후 redirect 한다, 1:사용, 0:사용안함, 기본값:0(사용안함) */
#define 	SV_HLS_MANIFEST_COMPRESS_ENABLE			"hls_manifest_compress_enable"		/* site, live hls manifest 응답시 body를 gzip으로 압축하는 기능 사용 여부, 1:압축, 0:압축안함, 기본값:0(압축안함) */
#define 	SV_ADDITIONAL_MANIFEST_CACHE_KEY		"additional_manifest_cache_key"		/* site, manifest 파일의 오리진 요청시 path외에 추가적으로 붙는 query parameter를 설정, 컴마(,)로 구분해서 여러개 설정 가능, 기본값:없음 */
#define 	SV_SESSION_ID_NAME						"session_id_name"			/* site, session ID 명을 변경할수 있다, 기본값:solsessionid */
#define 	SV_ENABLE_CROSSDOMAIN					"enable_crossdomain"			/* site, crossdomain.xml나 clientaccesspolicy.xml 요청에 대해 직접 응답한다, 1:사용, 0:사용안함, 기본값:0(사용안함) */
#define 	SV_PROGRESSIVE_CONVERT_ENABLE			"progressive_convert_enable"	/* site, 스트리밍 기능에서 가상 파일(content.mp4)요청인 경우 변환을 할지 여부, 0으로 설정된 경우 원본을 그대로 전달, 1로 설정된 경우 변환, 기본값:1(변환함) */
///////////////////////////////////////// Streaming 기능 관련 끝 ////////////////////////////////////////////
///////////////////////////////////////// RTMP streaming 기능 관련 시작 ////////////////////////////////////////////
#define 	SV_RTMP_PORT	 			"rtmp_port"				/* default only, rtmp 서비스를 위한 포트, default : 1935 */
#define 	SV_RTMP_ENABLE	 			"rtmp_enable"			/* default only, rtmp 서비스를 하는 경우 설정, 1:사용, 0:사용안함, default : 0(사용안함) */
#define 	SV_RTMP_WORKER_POOL_SIZE	"rtmp_worker_pool_size"	/* default only, rtmp 처리용 work thread 수 설정, default : 10 */
#define 	SV_RTMP_SERVICE_ENABLE	 	"rtmp_service_enable"	/* site, 서비스별 rtmp 사용 여부, 1:사용, 0:사용안함, default : 0(사용안함) */
///////////////////////////////////////// RTMP streaming 기능 관련 끝 //////////////////////////////////////////////
///////////////////////////////////////// 통계 기능 관련 시작 ////////////////////////////////////////////////
#define 	SV_STAT_RC_SEQ			"stat_rc_seq"			/* default, 통계 기록시 사용 되는 RC_SEQ, 기본값:없음 */
#define 	SV_STAT_VRC_SEQ			"stat_vrc_seq"			/* default, 통계 기록시 사용 되는 VRC_SEQ, 기본값:없음 */
#define 	SV_STAT_SVR_SEQ			"stat_svr_seq"			/* default, 통계 기록시 사용 되는 SVR_SEQ, 기본값:없음 */
#define 	SV_STAT_IDC_NODE_ID		"stat_idc_node_id"		/* default, 통계 기록시 사용 되는 IDC_NODE_ID, 기본값:없음 */
#define 	SV_STAT_RC_ID			"stat_rc_id"			/* default, 통계 기록시 사용 되는 RC_ID, 기본값:없음 */
#define 	SV_STAT_VRC_ID			"stat_vrc_id"			/* default, 통계 기록시 사용 되는 VRC_ID, 기본값:없음 */
#define 	SV_STAT_SVR_ID			"stat_svr_id"			/* default, 통계 기록시 사용 되는 SVR_ID, 기본값:없음 */
#define 	SV_STAT_WRITE_PERIOD	"stat_write_period"		/* default, 통계 기록 주기, 단위:초, 기본값:10(초) */
#define 	SV_STAT_ROTATE_PERIOD	"stat_rotate_period"	/* default, 통계 파일 rotate (파일명 변경) 주기, 단위:초, 기본값:60(초) */
#define 	SV_STAT_ORIGIN_PATH		"stat_origin_path"		/* default, origin 통계 파일이 기록될 경로, 기본값:/usr/service/stat/origin_stat */
#define 	SV_STAT_ORIGIN_PREFIX	"stat_origin_prefix"	/* default, origin 통계 파일명의 prefix, 기본값:origin_stat_statd_ */
#define 	SV_STAT_TRAFFIC_PATH	"stat_traffic_path"		/* default, traffic 통계 파일이 기록될 경로, 기본값:/usr/service/stat/traffic_stat */
#define 	SV_STAT_TRAFFIC_PREFIX	"stat_traffic_prefix"	/* default, traffic 통계 파일명의 prefix, 기본값 traffic_stat_statd_ */
#define 	SV_STAT_NOS_PATH		"stat_nos_path"			/* default, nos 통계 파일이 기록될 경로, 기본값:/usr/service/stat/nos_stat */
#define 	SV_STAT_NOS_PREFIX		"stat_nos_prefix"		/* default, nos 통계 파일명의 prefix, 기본값:nos_stat_statd_ */
#define 	SV_STAT_HTTP_PATH		"stat_http_path"		/* default, http 통계 파일이 기록될 경로, 기본값:/usr/service/stat/http_stat */
#define 	SV_STAT_HTTP_PREFIX		"stat_http_prefix"		/* default, http 통계 파일명의 prefix, 기본값:http_stat_statd_ */
#define 	SV_STAT_DOING_DIR_NAME	"stat_doing_dir_name"	/* default, 생성중인 통계파일이 저장될 directory 명, 기본값 doing */
#define 	SV_STAT_DONE_DIR_NAME	"stat_done_dir_name"	/* default, 생성이 완료된 통계파일이 저장될 directory 명, 기본값:done */
#define 	SV_STAT_WRITE_ENABLE	"stat_write_enable"		/* default, inherited, 서비스 데몬의 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:0(기록안함) */
#define 	SV_STAT_WRITE_TYPE		"stat_write_type"		/* default, inherited, 통계 기록 방식 설정, 0:delivery type 통계기록, 1:streaming type 통계기록, 기본값:0(delivery type) */
#define 	SV_STAT_ORIGIN_ENABLE	"stat_origin_enable"	/* default, inherited, 오리진 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_TRAFFIC_ENABLE	"stat_traffic_enable"	/* default, inherited, 트래픽 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_NOS_ENABLE		"stat_nos_enable"		/* default, inherited, NOS 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_HTTP_ENABLE		"stat_http_enable"		/* default, inherited, HTTP(Content) 통계 기록 여부, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_SERVICE_WRITE_ENABLE	"stat_service_write_enable"		/* site, inherited, 서비스 별 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:0(기록안함) */
#define 	SV_STAT_SERVICE_ORIGIN_ENABLE	"stat_service_origin_enable"	/* site, inherited, 서비스 별 오리진 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_SERVICE_TRAFFIC_ENABLE	"stat_service_traffic_enable"	/* site, inherited, 서비스 별 트래픽 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_SERVICE_NOS_ENABLE		"stat_service_nos_enable"		/* site, inherited, 서비스 별 NOS 통계 기록 여부 설정, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_SERVICE_HTTP_ENABLE		"stat_service_http_enable"		/* site, inherited, 서비스 별HTTP(Content) 통계 기록 여부, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_HTTP_IGNORE_PATH	"stat_http_ignore_path"		/* site, HTTP(Content) 통계 기록시 실제 요청 path대신 -(dash)로 기록, 1:-로 기록 , 0:요청 path 사용, 기본값:0(요청 path 사용) */
#define 	SV_STAT_SP_SEQ			"stat_sp_seq"			/* site, 통계 기록시 사용 되는 SP_SEQ, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_CONT_SEQ		"stat_cont_seq"			/* site, 통계 기록시 사용 되는 CONT_SEQ, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_VOL_SEQ			"stat_vol_seq"			/* site, 통계 기록시 사용 되는 VOL_SEQ, 1:통계기록 , 0:통계기록안함, 기본값:1(기록) */
#define 	SV_STAT_SESSION_TYPE_ENABLE	"stat_session_type_enable"	/* site, session을 사용중인 경우 통계 기록시 session 기준으로 통계를 기록, 1:session 기준통계생성, 0:요청단위통계, 기본값:0(요청단위통계) */
///////////////////////////////////////// 통계 기능 관련 끝 //////////////////////////////////////////////////

///////////////////////////////////////// 전송 속도 제한 관련 ////////////////////////////////////////////
#define		SV_LIMIT_RATE					"limit_rate"				/* default/site, not inherited, 세션별 전송속도 제한 (단위 : KByte/sec) */
#define		SV_LIMIT_RATE_AFTER				"limit_rate_after"			/* default/site, not inherited, 설정된 크기 이후에 속도제한 (단위 : KByte) */
#define		SV_LIMIT_TRAFFIC_AFTER			"limit_traffic_rate"		/* default/site, not inherited, 서비스별 속도제한 (단위 : KByte/sec) */

///////////////////////////////////////// GEOIP related vars ////////////////////////////////////////////
#define 	SV_GEOIP_COUNTRY 		"geoip_country"			/* default only*/
#define 	SV_GEOIP_CITY 			"geoip_city"			/* default only*/
#define 	SV_GEOIP_ISP 			"geoip_isp"			/* default only*/

///////////////////////////////////////// 모듈 구조 관련 ////////////////////////////////////////////
#define 	SV_MODULE_ORDER			"module_order"		/* default only, default : 없음, 모듈의 phase function의 동작 순서를 정의, 지정 하지 않는 경우 내부적으로 정해진 순서를 따른다. */
#define 	SV_MODULE_PATH			"module_path"		/* default only, default : 없음, 동적 모듈들의 so 파일 경로 */

#endif /* __SETUP_VARS_H__ */
