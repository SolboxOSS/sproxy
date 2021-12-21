#ifndef __STATUS_H__
#define __STATUS_H__

typedef struct {
	char 			code[5];
	char 			page[64];
	int			 	pos;	/* 배열에서의 위치 */
} code_element_t;

void scx_init_status();
void scx_deinit_status();
void status_setup_element();
int status_get_pos_by_int(int code);
int status_get_pos_by_char(const char *zcode);
void status_destory_element();
void scx_update_volume_service_start_stat(nc_request_t *req);
void scx_update_volume_service_end_stat(nc_request_t *req);
uint32_t scx_get_volume_concurrent_count(service_info_t *service);
uint32_t scx_get_volume_session_count(service_info_t *service);
void scx_update_res_body_size(nc_request_t *req, size_t size);
uint64_t scx_get_volume_send_size(service_info_t *service);
void scx_update_volume_origin_stat(service_info_t *service, nc_method_t m, double sentb, double receivedb, const char *zcode);
void internal_update_service_start_stat(service_info_t *service);
void internal_update_service_end_stat(service_info_t *service);
void session_update_service_start_stat(service_info_t *service);
void session_update_service_end_stat(service_info_t *service);
void scx_write_status();
void scx_write_concurrent_status();
void scx_dump_service_stat(service_info_t *service); /* 이름 변경 필요 */
void scx_write_system_status();
void scx_update_system_status();
#endif /* __STATUS_H__ */
