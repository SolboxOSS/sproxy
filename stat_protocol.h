/*
 * stat_protocol.h
 *
 *  Created on: 2016. 6. 10.
 *      Author: sd@solbox.com
 */

#ifndef _STAT_PROTOCOL_H_
#define _STAT_PROTOCOL_H_

#ifdef __cplusplus
extern "C" {
#endif
#include <time.h> // for time_t
/*
 * statd가 처리가능한 프로토콜에 대한 정의
 * RULE!!!!:
 * 	프로토콜을 변경해야하는 경우, 기존 프로토콜은 변경하지 않고, 새로운 타입을 추가한 뒤 "STAT_PROTOCOL_VERION"을 올린다
 * 	프로토콜 버전 규칙:
 * 		Major version: statd의 메이저 버전
 * 		Minor version: statd의 마이너 버전, 패킷 타입 추가의 경우 올린다
 * 		Release version: statd의 릴리즈 버전
 * 		기타: statd의 버전과 프로토콜 버전을 동일하게 유지하되, 상황에 따라 STAT_PROTOCOL_VERION 만 상향 가능.(단, 이 경우 추가된 프로토콜의 statd 지원 여부는 불확실)
 */
#define STAT_PROTOCOL_VERISON "0.0.4"
#define STAT_PROTOCOL_PACKAGE_VERISON "stat protocol 0.0.4"
#ifdef SOLBOX_UDP_SIZE_MAX
#define UDP_MAX_SIZE SOLBOX_UDP_SIZE_MAX
#else
#define UDP_MAX_SIZE 65507
#endif
/*
 * _pkt_type_t
 * 	다음에 대한 사항들이 약속된 타입
 * 		바디의 구조체
 * 		산출 방식
 * 		산출할 수 있는 통계들
 */
enum _pkt_type_t {
	//unknown_pkt_type = 0,
	ics_real_pkt_type = 0, /* ICS 2.x 패킷 타입들, 헤더 없는 벌크 패킷 타입 - statd에서 처리를 위해 지정 */
	ics_http_pkt_type, /* ICS 2.x 패킷 타입들, 헤더 없는 벌크 패킷 타입 - statd에서 처리를 위해 지정 */
	ics_isp_pkt_type, /* ICS 2.x 패킷 타입들, 헤더 없는 벌크 패킷 타입 - statd에서 처리를 위해 지정 */
 /* 1 ~ 99 예약 범위 */
 /* 100 ~ 199 패킷 타입 범위 */
	bulk_pkt_type = 100,	/* 바디로 복수의 패킷이 반복되어 온다, 바디의 패킷들은 여러 타입들이 올수 있다. 단, bulk_pkt_type이 다시 올수는 없다. */
  /* 200 ~ 254 통계 타입 범위 */
	//ics_net_bulk_pkt_type = 200, /* ICS 2.x 패킷 타입들, 헤더 없는 벌크 패킷 타입 - statd에서 처리를 위해 지정 */
	//ics_nos_bulk_pkt_type, /* ICS 2.x 패킷 타입들, 헤더 없는 벌크 패킷 타입 - statd에서 처리를 위해 지정 */
	//ics_http_bulk_pkt_type, /* ICS 2.x 패킷 타입들, 헤더 없는 벌크 패킷 타입 - statd에서 처리를 위해 지정 */
	//ics_isp_bulk_pkt_type, /* ICS 2.x 패킷 타입들, 헤더 없는 벌크 패킷 타입 - statd에서 처리를 위해 지정 */
	periodic_pkt_type = 200, /* struct _periodic_pkt_t */ /* traffic_stat, nos_stat 통계 산출 가능 타입 */
	txrx_pkt_type,	/* struct _txrx_pkt_t */ /*  traffic_stat 통계 산출용 타입 */
	nos_pkt_type,	/* struct _nos_pkt_t */ /* nos_stat 통계 산출용 타입 */
	origin_pkt_type, /* struct _origin_pkt_t */ /* origin_stat 산출 가능 타입 */
	http_pkt_type, /* struct _http_pkt_t */ /* http_stat 산출 가능 타입 */
	mms_pkt_type, /* struct _mms_pkt_t */ /* mms_stat 산출 가능 타입 */
	uv_pkt_type, /* struct _uv_pkt_t */ /* uv_stat 산출 가능 타입 */
	category_pkt_type, /* struct _category_pkt_t */ /* category_stat 산출 가능 타입 */
	zipper_pkt_type, /* struct _zipper_pkt_t */ /* traffic_stat, nos_stat, origin_traffic_stat, category_stat 산출 가능 타입 */
	reserved_pkt_type = 255
};

/*
 * _stat_type_t
 * 	어떠한 통계 파일을 생성할지에 대한 약속으로 다음 사항들에 대해 약속된 타입으로 statd에서만 사용하는 타입
 * 		수신 패킷
 * 		수신 패킷의 metric과 dimension
 * 	통계 파일의 기록 경로와 파일 이름, 기록 주기는 설정으로 명시
 */
enum _stat_type_t {
	unknown_stat_type = 0,
	/* 1 ~ 99 예약 범위 */
	/* 100 ~ 199 통계 타입 범위 */
	ics_net_stat_type = 100, /* ICS 2.x NET_STAT, Suzy traffic_stat_statd(ICS 2.x 호환성 유지용도) */
	ics_nos_stat_type = 101, /* ICS 2.x NOS_STAT, Suzy nos_stat_statd(ICS 2.x 호환성 유지용도) */
	ics_http_stat_type = 102, /* ICS 2.x HTTP_STAT, Suzy http_stat_statd(ICS 2.x 호환성 유지용도) */
	ics_isp_stat_type = 103, /* ICS 2.5 ISP_STAT(ICS 2.x 호환성 유지용도), not used */
	traffic_stat_type, /* Suzy traffic_stat_statd */
	nos_stat_type, /* Suzy nos_stat_statd */
	http_stat_type, /* Suzy http_stat_statd */
	origin_stat_type,	/* Suzy origin stat */
	mms_stat_type, /* ICS 2.x FMS_STAT, Suzy mms_stat_wowza, mms_stat_statd */
	mms_traffic_stat_type, /* ICS 2.0 FS_TRAFFIC, Suzy ms_traffic_stat_statd(traffic_stat_wowza와 동일) */
	mms_nos_stat_type, /* ICS 2.0 FS_NOS, Suzy ms_nos_stat_statd(nos_stat_wowza와 동일) */
	category_stat_type, /* CATEGORY_STAT */
	uv_stat_type, /* UV_STAT */
	/* 200 ~ 254 예약 범위 */
	reserved_stat_type = 255
};
// ICS 2.x 호환성 유지를 위해서 동일한 구조의 헤더 사용
struct _hdr_t {
	int size; // 바디의 크기, ics_real_pkt_type, ics_http_pkt_type, ics_isp_pkt_type와 같은 구 프로토콜들은 헤더의 크기를 포함한다.
	int type;
} __attribute((packed));


/*
 * FIXME:
 * 	_protocol_t와 category_t는 현재 기준으로는 중복될 수 있으므로 PREFIX나 POSTFIX 붙여야한다...
 */
enum _protocol_t {
	PROTOCOL_UNKNOWN = 0,
	PROTOCOL_HTTP=1,
	PROTOCOL_FTP=2,
	PROTOCOL_MMS=3,
	PROTOCOL_RTMP=4,
	PROTOCOL_RTSP=5,
	PROTOCOL_HLS=6,
	PROTOCOL_HDS=7,
	PROTOCOL_HSS=8,
	PROTOCOL_DASH=9,
	PROTOCOL_RESERVED = 15
};
enum _category_t {
	CATEGORY_UNKNOWN = 0,
	CATEGORY_PC = 1,
	CATEGORY_MOBILE = 2,
	CATEGORY_RESERVED = 7
};
/*
 * 참고
 * 	통계 프로토콜의 키가 되는 도메인 최대 길이는 255자로 제한한다.
 * 	근거는 RFC 2181의 다음 항목
 *	RFC 2181,
 *	11. Name syntax
 *	The DNS itself places only one restriction on the particular labels
 *	that can be used to identify resource records.  That one restriction
 *	relates to the length of the label and the full name.  The length of
 *	any one label is limited to between 1 and 63 octets.  A full domain
 *	name is limited to 255 octets (including the separators). ...
 *
 */
/* 패킷 타입별 구조체 */
struct _periodic_pkt_t {
	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
	unsigned int period;    /* 수집 주기 - 기본 5초 - */
	unsigned long long rx; /* 수집 주기 누적 rx - bytes 단위 - */
	unsigned long long tx; /* 수집 주기 누적 tx - bytes 단위 - */
	unsigned int session_count; /* 현재 세션 - 연결 - 수 */
	unsigned char protocol; /* 트래픽이 발생한 프로토콜 */
	char domain[256];
} __attribute((packed));

struct _txrx_pkt_t {
	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
	unsigned int period;    /* 수집 주기 - 기본 5초 - */
	unsigned long long tx; /* 수집 주기 누적 tx - bytes 단위 - */
	unsigned long long rx; /* 수집 주기 누적 rx - bytes 단위 - */
	unsigned char protocol; /*  프로토콜 */
	char domain[256];
} __attribute((packed));

struct _nos_pkt_t {
	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
	unsigned int period;    /* 수집 주기 - 기본 5초 - */
	unsigned int session_count; /* 현재 세션 - 연결 - 수 */
	unsigned char protocol; /* 트래픽이 발생한 프로토콜 */
	char domain[256];
} __attribute((packed));

struct _origin_pkt_t {
	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
	unsigned int period;    /* 수집 주기 - 기본 5초 - */
	unsigned long long tx; /* 수집 주기 누적 tx - bytes 단위 - */
	unsigned long long rx; /* 수집 주기 누적 rx - bytes 단위 - */
	unsigned char protocol; /* 트래픽이 발생한 프로토콜 */
	char domain[256];
} __attribute((packed));

struct _zipper_pkt_t {
 	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
 	unsigned int period;    /* 수집 주기 - 기본 5초 - */
 	unsigned long long tx; /* 수집 주기 누적 tx - bytes 단위 - */
 	unsigned long long rx; /* 수집 주기 누적 rx - bytes 단위 - */
 	unsigned long long origin_tx; /* 수집 주기 누적 오리진 tx - bytes 단위 - */
 	unsigned long long origin_rx; /* 수집 주기 누적 오리진 rx - bytes 단위 - */
 	unsigned int session_count; /* 현재 세션 - 연결 - 수 */
 	unsigned char protocol; /* 트래픽이 발생한 프로토콜 */
 	unsigned char category; /* 카테고리 */
 	char domain[256];
	} __attribute((packed));

struct _http_pkt_t {
	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
	unsigned int method; /* 메소드 */
	unsigned long long tx; /* 전송 bytes */
	unsigned long long rx; /* 수신 bytes */
	unsigned long long content_length; /* 파일 크기 */
	unsigned long long duration; /* 전송 소요 시간 - msec 단위 - */
	unsigned char status; /* 응답 코드 */
	int client_ip; /* client IP, IPv4만 대상이며, IPv6는 차후 고려한다.(그때즈음이면 다른식으로 통계 산출하겠지...) */
	int domain_offset; /* data에서 도메인 길이 */
	int uri_offset;  /* data에서 uri 길이 */
	char data[5120];  /* uri + 도메인 가 기록되는 버퍼 */
} __attribute((packed));

struct _mms_pkt_t {
	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
	unsigned int method; /* 메소드 */
	unsigned long long tx; /* 전송 bytes */
	unsigned long long rx; /* 수신 bytes */
	unsigned long long content_length; /* 파일 크기 */
	unsigned long long duration; /* 전송 소요 시간 - msec 단위 - */
	unsigned long long send_offset; /* 파일 전송 시작 offset */
	unsigned char status; /* 응답 코드 */
	unsigned char protocol; /* 프로토콜 */
	unsigned char auth_code; /* 인증 코드 */
	unsigned char drm_code; /* drm 코드 */
	int client_ip; /* client IP, IPv4만 대상이며, IPv6는 차후 고려한다.(그때즈음이면 다른식으로 통계 산출하겠지...) */
	int domain_offset; /* data에서 도메인 길이 */
	int uri_offset;  /* data에서 uri 길이 */
	char data[5120];  /* uri+도메인 가 기록되는 버퍼 */
} __attribute((packed));

struct _category_pkt_t {
 time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
 unsigned int period;    /* 수집 주기 - 기본 5초 - */
 unsigned long long tx; /* 수집 주기 누적 tx - bytes 단위 - */
 unsigned long long rx; /* 수집 주기 누적 rx - bytes 단위 - */
 unsigned int session_count; /* 현재 세션 - 연결 - 수 */
 unsigned char category; /* 카테고리 */
 char domain[256];
} __attribute((packed));

struct _uv_pkt_t {
	time_t stattime;           /* 통계 수집 - 통계 전송 - 시간 */
	unsigned int period;    /* 수집 주기 - 기본 5초 - */
	unsigned int duration; /* 재생 시간 */
	unsigned int session_count; /* 현재 세션 - 연결 - 수 */
	unsigned char protocol; /* 트래픽이 발생한 프로토콜 */
	unsigned char category; /* 트래픽이 발생한 카테고리  */
	int client_ip; /* client IP, IPv4만 대상이며, IPv6는 차후 고려한다.(그때즈음이면 다른식으로 통계 산출하겠지...) */
	char event[32]; /* 채널명 */
	char domain[256];
} __attribute((packed));

/*
 * bulk packet 예제

sturct _bulk_http_pkt_t {
struct _hdr_t header;
char body[UDP_MAX_SIZE - sizeof(struct _hdr_t)];
 };

struct _http_pkt_t pkt1, pkt2;

// pkt1, pkt2 채우는 코드가 있다고 치고...
// 아래와 같이 벌크 패킷의 바디를 채움
bulk.header.type = bulk_packet_type;
bulk.header.size = (sizeof(pkt1) - (sizeof(pkt1.domain) - strlen(pkt1.domain)) + (sizeof(pkt2) - (sizeof(pkt2.domain) - strlen(pkt2.domain));

memcpy(bulk.body, pkt1, (sizeof(pkt1) - (sizeof(pkt1.domain) - strlen(pkt1.domain)));
memcpy(bulk.body + (sizeof(pkt1) - (sizeof(pkt1.domain) - strlen(pkt1.domain)), pkt2, (sizeof(pkt2) - (sizeof(pkt2.domain) - strlen(pkt2.domain)));

 */
#ifdef __cplusplus
} /* end of extern */
#endif
#endif /* _STAT_PROTOCOL_H_ */
