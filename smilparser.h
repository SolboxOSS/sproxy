#ifndef __SMIL_PARSER_H__
#define __SMIL_PARSER_H__

struct tag_adaptive_info;

void smil_parser_init();
void smil_parser_deinit();
int smil_parse(nc_request_t *req, struct tag_adaptive_info	*adaptive_info, char *smil_buffer, mem_pool_t *tmp_mpool);

#endif /*__SMIL_PARSER_H__ */
