#ifndef __SHM_LOG_H__
#define __SHM_LOG_H__

#define SHM_LOG_VERSION		1
#define SHM_LOG__MAGIC 		0x3B35B313
#define SHM_KEY_ID			0x18b3a553

typedef struct {
	int32_t	magic; 		/* 이 값이 틀린 경우는 공유 메모리 삭제 */
	int32_t	version; 	/* 초기는 1 */
	int32_t	array_count;	/* log array 갯수 */
	char	reserved[28];
} shm_log_header_t;

typedef struct shm_log_body {
	int32_t		using;	/* 0 이면 미사용, 1이면 사용중 */
	uint64_t	id;		/* unique ID */
	char		host[32]; 	/* 요청 에 포함된 host */
	char	 	path[256];	/* 요청 url */
	char		method[10];
	char		client_ip[20];
	time_t 		req_time; 	/* 처음 요청 시간 */
	flow_process_step_t step;	/* NetCache 비동기 API 호출시 사용 */
} shm_log_body_t;


int shm_log_init();
int shm_log_deinit();
int shm_log_create_request(struct request_tag *req);
int shm_log_update_host(struct request_tag *req);
int shm_log_complete_request(struct request_tag *req);
#endif /* __SHM_LOG_H__ */
