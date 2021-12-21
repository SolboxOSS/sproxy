#ifndef __STREAMING_LRU_H__
#define __STREAMING_LRU_H__


typedef enum {
	GNODE_TYPE_NOTDEFINE= 0,
	GNODE_TYPE_BUILDER,
	GNODE_TYPE_MEDIA
} scx_gnode_type_t;


typedef struct tag_gnode_info {
	glru_node_t			gnode;
	char 				*key;	/* media의 경우 host+url, builder의 경우 lang+contents */
	scx_gnode_type_t	type;
	int					id;		/* builder hash에서 찾을때 필요한 ID */
	void 				*context;
	int					commit; /* GLRUR_ALLOCATED 리턴으로 인해 glru_commit이 필요한 경우1, glru_commit()을 호출 이전에 먼저 0으로 셋팅 해야함 */
} gnode_info_t;

int scx_lru_init();
void scx_lru_destroy();
builder_info_t *allocate_builder();
media_info_t *allocate_media(void *data, const char * key);
int delete_media(media_info_t *media);
char *strm_make_media_key(nc_request_t *req, char *url);
media_info_t *strm_find_media(nc_request_t *req, int type, char *url, int allocifnotexist, int setprogress, int *gres);
media_info_t *strm_allocate_media(nc_request_t *req,int *gres);
int strm_commit_media(nc_request_t *req, media_info_t *media);
int strm_release_media(media_info_t *media);

#endif /*__STREAMING_LRU_H__ */
