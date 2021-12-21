#include <config.h>
#include <netcache_config.h>

#include <error.h>
#include <fcntl.h>
#include <iconv.h>
#include <langinfo.h>
#include <libintl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>


#include <netcache.h>
#include <block.h>
#include "util.h"
#include "disk_io.h"
#include "threadpool.h"
#include "trace.h"
#include "hash.h"
#include "httpn_driver.h"
#include "httpn_request.h"
#include "http_codes.h"


/*
 * 아래 함수 호출 시 httpcode는 '반드시' 오리진에서 받은 httpcode임
 *
 * REMARK)
 *	- CURLcode는 최종 확정된 값만 받음
 *	- httpcode는 바뀔 수 있음(CURLcode에 따라)
 *
 * Error handling adjusted based on 'IR#33244(https://jarvis.solbox.com/redmine/issues/33253?issue_count=48&issue_position=1&next_issue_id=33244), 
 * (by weon@solbox.com)
 */
int
httpn_handle_error(httpn_io_context_t *iocb, CURLcode *curle, int *httpcode, int *dtc)
{
	int		stderrno	= 0;
	int		curle_org	= *curle;
	int		http_org	= *httpcode;
	int		tflg		= 0;
	char	ziocb[512];
	static char				zdtc[][32] = {
		"0:HTTPDE_OK",
		"1:HTTPDE_RETRY",
		"2:HTTPDE_CRITICAL",
		"3:HTTPDE_USER",
		"4:UNKNOWN",
		"5:UNKNOWN",
		"6:UNKNOWN",
		"7:UNKNOWN"
	};

	*dtc = HTTPDE_OK;

	switch (*curle)  {
		case CURLE_OK:
			/*
			 * the response server is healthy even though it returns 5XX.
			 */
			if( *httpcode >= 500 )
			{
				*dtc = HTTPDE_USER; /* callback에서 판단하도록 */
			}
			break;
		case CURLE_ABORTED_BY_CALLBACK:
			tflg = T_WARN;
			if (iocb->premature) 
				*curle = CURLE_OK;

			if (iocb->timedout) {
				*curle 		= CURLE_OPERATION_TIMEDOUT;
				*httpcode	= HTTP_GATEWAY_TIMEOUT;
				*dtc		= HTTPDE_CRITICAL;
			}
			if (iocb->canceled) {
				tflg 		= 0;
				*dtc		= HTTPDE_OK;
				*curle		= CURLE_OK;
				stderrno	= ECANCELED;
			}
			break;
		case CURLE_OPERATION_TIMEDOUT:
			*httpcode	= HTTP_GATEWAY_TIMEOUT;
			//*dtc		= HTTPDE_CRITICAL;
			*dtc		= HTTPDE_RETRY;
			tflg		= T_WARN;
			break;
		case CURLE_WRITE_ERROR:
			*dtc		= HTTPDE_RETRY;
			tflg		= T_WARN;
			stderrno	= EIO;
			if (iocb->premature || iocb->canceled)  {
				*curle	= CURLE_OK;
				*dtc	= HTTPDE_OK;
				/* added in 2021.3.4 */
				stderrno= (iocb->premature?0:ECANCELED);
				tflg	= 0;
			}
			break;
		/*
		 * 2020.10.13 by weon@solbox.com
		 * connection-related errors: all regarded as server-offline by IR#33244
		 *
		 */
		case CURLE_UNSUPPORTED_PROTOCOL:
		case CURLE_COULDNT_RESOLVE_PROXY:
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_WEIRD_SERVER_REPLY:
		case CURLE_READ_ERROR:
		case CURLE_REMOTE_ACCESS_DENIED:
		case CURLE_HTTP_RETURNED_ERROR:
		case CURLE_UPLOAD_FAILED:
		case CURLE_SSL_ENGINE_NOTFOUND:
		case CURLE_SSL_ENGINE_SETFAILED:
		case CURLE_PEER_FAILED_VERIFICATION:
		case CURLE_CHUNK_FAILED:
		case CURLE_SSL_CONNECT_ERROR:
		case CURLE_GOT_NOTHING:
		case CURLE_RANGE_ERROR:
		case CURLE_HTTP_POST_ERROR:
		case CURLE_SSL_CACERT_BADFILE:
		case CURLE_SSL_SHUTDOWN_FAILED:
		case CURLE_SSL_CRL_BADFILE:
		case CURLE_SSL_ISSUER_ERROR:
		case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
		case CURLE_SSL_INVALIDCERTSTATUS:
		case CURLE_BAD_DOWNLOAD_RESUME:
		case CURLE_SEND_FAIL_REWIND:
			//tflg = T_WARN;

			/*
			 * 위의 에러 상황에서는 재시도 안함
			 */
			if (*httpcode == 0)
				*httpcode	= HTTP_BAD_GATEWAY;
			*dtc		= HTTPDE_CRITICAL;
			//tflg 		= T_WARN;
			stderrno	= EINVAL;
			break;
		case CURLE_COULDNT_CONNECT:
			*dtc = HTTPDE_RETRY;
			if (*httpcode == 0)
				*httpcode	= HTTP_BAD_GATEWAY;
			stderrno	= EREMOTEIO;
			break;
		case CURLE_SEND_ERROR:
		case CURLE_RECV_ERROR:
		case CURLE_PARTIAL_FILE:
			/*
			 * 2021.2.20  by weon
			 * need to make a wheel to retry in both cases
			 */
			*dtc = HTTPDE_RETRY;
			stderrno	= EIO;
			break;
		case CURLE_TOO_MANY_REDIRECTS:
			*httpcode	= HTTP_BAD_REQUEST;
			*dtc		= HTTPDE_CRITICAL;
			stderrno	= ELOOP;
			break;
		default:
			if (*httpcode == 0)
				*httpcode	= HTTP_BAD_GATEWAY;
			*dtc		= HTTPDE_CRITICAL;
			tflg 		= T_WARN;
			break;
	}

	if(stderrno == 0)
		stderrno = httpn_map_httpcode(iocb, *httpcode);
	DEBUG_ASSERT(iocb->last_curle == 0 || iocb->last_curle != 0 && stderrno != 0);

	if (iocb->method == HTTP_POST && *dtc == HTTPDE_RETRY) { 
		/*
		 *	POST의 경우 retry하지 않음
		 */
		*dtc		= HTTPDE_CRITICAL;
	}
	/*
	 * 2020.1.8 by weon
	 * tflg = 0으로 변경 (origin.log에 이미 해당 사유 리포팅함)
	 */
	TRACE((tflg|T_PLUGIN, "%d/%d :: %s/%s:IOCB[%d] {CURLE=%d:%s, httpcode=%d} => stderrno=%d, {CURLE=%d, httpcode=%d, DTC=%d:%s} :%s\n",
					iocb->tries,
					iocb->driverX->max_tries,
					iocb->driver->signature,
					iocb->wpath,
					iocb->id, 
					curle_org, 
					(char *)curl_easy_strerror(curle_org),
					http_org, 
					stderrno, 
					(int)*curle, 
					*httpcode, 
					*dtc,
					zdtc[-*dtc],
					httpn_dump_iocb(ziocb, sizeof(ziocb), iocb)
		  ));

	return stderrno;
}

