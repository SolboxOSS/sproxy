/*
 * 재기동 발생시 access 로그에 기록되지 못한 요청의 정보를 error 로그에 기록하는 모듈
 * 관련 일감 : https://jarvis.solbox.com/redmine/issues/33263
 * Client의 요청이 들어 온 경우 처리가 완료되서 access 로그에 기록되기 이전 까지 요청에 대한 정보를 공유 메모리에 기록
 * 공유 메모리는 SolProxy의 재기동이 발생해도 해제가 되지 않음
 * 공유메모리에는 최대 10000개의 요청을 기록 할수 있도록 4MB 정도를 할당
 * 로그 기록은 성능 저하를 막기 위해 lock을 사용 안함
 * 요청이 들어왔을때 로그 기록을 위한 slot(array) 할당 로직
 ** 각 request에 부여 되는 unique ID를 사용해서 전에 slot 갯수인 10000으로 나누어서 나머지 값을 slot 으로 사용
 ** slot이 이전 세션에서 사용중인 경우 더이상 진행 하지 않고 해당 요청에 대해서는 로그에 기록하지 않는다.
 ** 필요 정보들을 slot에 기록
 ** 사용자 종료시 access 로그 기록후에 할당 받은 slot을 반납
 * 위의 방식으로 했을때 운이 없는 몇개의 access에도 남지 않고 error 로그에도 기록이 되지 않을수 있지만 낮은 확률로 발생하기 때문에 무시한다.
 * 데몬 정상 종료시에 공유 메모리 삭제
 * 데몬 기동시에 공유 메모리가 있는지 확인
 ** 공유메모리가 있다는것은 이전에 데몬이 비정상 종료했다고 볼수 있음
 ** 이 경우 공유 메모리의 내용의 error 로그에 기록
 * 모니터링 요청은 공유메모리 기록에서 제외한다.
 * 로그에 기록되는 내용
 ** unique ID
 ** 처음 요청 수신후 재기동시까지 흐린 시간(단위 : 초)
 ** 요청 처리 phase
 ** 요청 도메인
 ** 요청 method
 ** 요청 URL
 ** Client IP
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <search.h>
#include <sys/prctl.h>
#include <errno.h>
#include <microhttpd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "common.h"
#include "scx_util.h"
#include "reqpool.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include "shm_log.h"

int 	gscx__shm_log_id = -1;
const key_t	gscx__shm_key = SHM_KEY_ID;
const int	gscx__shm_array_count = 10000;
void 	*gscx__shm_memory = NULL;
int		gscx__shm_memory_size = 0;


int		shm_log_check_previous_memory();
int 	shm_log_dump();
int		shm_log_memory_set();
int		shm_log_header_init();



int
shm_log_init()
{
	int	ret = 0;

	gscx__shm_memory_size = sizeof(shm_log_header_t) + (sizeof(shm_log_body_t) * gscx__shm_array_count);
	TRACE((T_INFO, "shared memory size = %d.\n", gscx__shm_memory_size));
	gscx__shm_log_id = shmget(gscx__shm_key, gscx__shm_memory_size, IPC_CREAT|IPC_EXCL|0666);
	if (gscx__shm_log_id == -1) {
		if (errno == EEXIST) {
			/*
			 * 공유 메모리가 이미 있는 경우
			 * 이전 프로세스가 비정상 종료된 상태로 본다.
			 * 기존 메모리에 있는 내용을 백업 받은후에 공유메모리를 삭제 하고 다시 만들어야 한다.
			 */
			TRACE((T_WARN, "shared memory exist(0x%x).\n", gscx__shm_key));
			ret = shm_log_check_previous_memory();
			return ret;

		}
		TRACE((T_ERROR, "shmget(0x%x) failed. reason(%s)\n", gscx__shm_key, strerror(errno)));
		return -1;
	}
	ret = shm_log_memory_set();

	return ret;
}

int
shm_log_deinit()
{
	if (gscx__shm_log_id >= 0) {
		/* 기존  메모리 삭제 */
	    if(shmctl(gscx__shm_log_id, IPC_RMID, NULL)==-1){
	    	TRACE((T_WARN, "shmctl failed(0x%x). reason(%s)\n", gscx__shm_key, strerror(errno)));
	    	return -1;
	    }
		TRACE((T_INFO, "shared memory destroyed(0x%x).\n", gscx__shm_key));
		gscx__shm_log_id = -1;
	}

	return 0;
}

/*
 * 공유 메모리에 있는 내용을 모두 error 로그에 기록한후
 * 기존 공유 메모리를 삭제하고 새로 공유 메모리를 생성한다.
 */
int
shm_log_check_previous_memory()
{
	int 	shmid = -1;
	int		ret = 0;


	shmid = shmget(gscx__shm_key, gscx__shm_memory_size, IPC_CREAT|0666);
	if (shmid == -1) {
		TRACE((T_ERROR, "shmget(0x%x) failed. reason(%s)\n", gscx__shm_key, strerror(errno)));
		return -1;
	}

    if((gscx__shm_memory = shmat(shmid, NULL, 0)) == NULL){
    	TRACE((T_ERROR, "shmat(0x%x) failed. reason(%s)\n", gscx__shm_key, strerror(errno)));
    	return -1;
    }
    /* 공유 메모리에 있는 내용을 error 로그 파일에 dump */
    ret = shm_log_dump();

    /* 기존 공유 메모리 삭제 */
    if(shmctl(shmid, IPC_RMID, NULL)==-1){
    	TRACE((T_WARN, "shmctl failed(0x%x). reason(%s)\n", gscx__shm_key, strerror(errno)));
    	return -1;
    }

    gscx__shm_log_id = shmget(gscx__shm_key, gscx__shm_memory_size, IPC_CREAT|IPC_EXCL|0666);
	if (gscx__shm_log_id == -1) {
		TRACE((T_ERROR, "shmget(0x%x) failed. reason(%s)\n", gscx__shm_key, strerror(errno)));
		return -1;
	}

	ret = shm_log_memory_set();
	return ret;
}



int
shm_log_dump()
{
	shm_log_header_t log_header;
	int		ret = 0;
	int		header_size = sizeof(shm_log_header_t);
	int		body_size = sizeof(shm_log_body_t);
	shm_log_body_t	*shm_log_slot;
	time_t 	cur_time;

	cur_time = scx_update_cached_time_sec();

	memcpy(&log_header, gscx__shm_memory, header_size);
	if (log_header.magic != SHM_LOG__MAGIC) {
    	TRACE((T_WARN, "shared memory magic different. current(0x%x), memory(0x%x)\n", log_header.magic, SHM_LOG__MAGIC));
    	return -1;
	}
	if (log_header.version != SHM_LOG_VERSION) {
    	TRACE((T_WARN, "shared memory version different. current(0x%x), memory(0x%x)\n", log_header.version, SHM_LOG_VERSION));
    	return -1;
	}
	scx_error_log(NULL, "************ Previous not completed request Start ************\n");
	scx_error_log(NULL, "UniqueID  Duration  Phase  Host  Method  URL  ClientIP\n");
	for(int i = 0; i < log_header.array_count;i++) {
		shm_log_slot = (shm_log_body_t *)(gscx__shm_memory + header_size + body_size * i);
		if (shm_log_slot->using == 1) {
			scx_error_log(NULL, "%lld %ld %d \"%s\" \"%s\" \"%s\" \"%s\"\n",
										shm_log_slot->id, cur_time - shm_log_slot->req_time, shm_log_slot->step,
										(shm_log_slot->host[0]=='\0'?"-":shm_log_slot->host),
										(shm_log_slot->method[0]=='\0'?"-":shm_log_slot->method),
										(shm_log_slot->path[0]=='\0'?"-":shm_log_slot->path),
										(shm_log_slot->client_ip[0]=='\0'?"-":shm_log_slot->client_ip));
		}
	}
	scx_error_log(NULL, "************ Previous not completed request End ************\n");
}


int
shm_log_memory_set()
{
	ASSERT(gscx__shm_log_id != -1);

    if((gscx__shm_memory = shmat(gscx__shm_log_id, NULL, 0))== NULL){
    	TRACE((T_ERROR, "shmat(0x%x) failed. reason(%s)\n", gscx__shm_key, strerror(errno)));
    	return -1;
    }
    memset(gscx__shm_memory, 0, gscx__shm_memory_size);

    shm_log_header_init();

	return 0;
}


int
shm_log_header_init()
{
	shm_log_header_t log_header;

	memset(&log_header, 0, sizeof(shm_log_header_t));
	log_header.magic = SHM_LOG__MAGIC;
	log_header.version = SHM_LOG_VERSION;
	log_header.array_count = gscx__shm_array_count;
	memcpy(gscx__shm_memory, &log_header,  sizeof(shm_log_header_t));

	return 0;
}

int
shm_log_create_request(nc_request_t *req)
{
	int array_pos;
	shm_log_body_t	*shm_log_slot;
	int		header_size = sizeof(shm_log_header_t);
	int		body_size = sizeof(shm_log_body_t);

	array_pos = (int)(req->id % gscx__shm_array_count);
	shm_log_slot = (shm_log_body_t *)(gscx__shm_memory + header_size + body_size * array_pos);
	if (shm_log_slot->using == 1) {
		/* 계산에서 나온 array가 사용중이면 그냥 리턴한다. */
		req->shm_log_slot = NULL;
		return 0;
	}
	memset(shm_log_slot, 0, body_size);
	req->shm_log_slot = shm_log_slot;
	shm_log_slot->using = 1;
	snprintf(shm_log_slot->path, 256, "%s", vs_data(req->url) );
	shm_log_slot->req_time = scx_get_cached_time_sec();
	shm_log_slot->id = req->id;
	shm_log_slot->step = req->step;
	return 0;
}


int
shm_log_update_host(nc_request_t *req)
{
	if (req->shm_log_slot == NULL) {
		return 0;
	}
	if (req->service) {
		/* access 로그를 기록하지 않도록 되어 있는 요청(모니터링 요청으로 판단)인 경우 메모리 slot을 반납한다. */
		if(req->service->log_access == 0){
			req->shm_log_slot->using = 0;
			req->shm_log_slot = NULL;
			return 0;
		}
	}
	snprintf(req->shm_log_slot->host, 32, "%s", vs_data(req->host) );
	snprintf(req->shm_log_slot->method, 10, "%s", vs_data(req->zmethod) );
	snprintf(req->shm_log_slot->client_ip, 20, "%s", vs_data(req->client_ip) );

	req->shm_log_slot->step = req->step;
	return 0;

}

/*
 * 상태 추적을 위해 step만 업데이트 한다
 */
int
shm_log_update_step(nc_request_t *req)
{
	if (req->shm_log_slot == NULL) {
		return 0;
	}
	req->shm_log_slot->step = req->step;
	return 0;

}

int
shm_log_complete_request(nc_request_t *req)
{
	if (req->shm_log_slot == NULL) {
		return 0;
	}
	req->shm_log_slot->using = 0;
	req->shm_log_slot = NULL;
	return 0;

}

