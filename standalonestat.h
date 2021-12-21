#ifndef __STANDALONE_STAT_H__
#define __STANDALONE_STAT_H__

/*
 */
struct request_tag;

int standalonestat_init();
void standalonestat_deinit();
void standalonestat_write_http(struct request_tag *req);
void standalonestat_write_http_session(struct session_info_tag *session);
void standalonestat_wlock();
void standalonestat_rlock();
void standalonestat_unlock();
int standalonestat_service_clear_all();
int standalonestat_service_push(service_info_t *service);

#endif /* __STANDALONE_STAT_H__ */
