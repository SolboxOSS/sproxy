#ifndef __META_DISK_CHCHE_H__
#define __META_DISK_CHCHE_H__


#pragma pack(4)
typedef struct tag_disk_minfo {
	uint32_t magic;			/* 캐시 파일인지 확인용 특수 문자 */
	uint32_t version; 		/* cache format 버전, 버전이 틀린 경우 해당 캐시 삭제 */
	char key[1024];	 		/* 	service->name+url */
	char st_devid[128];		/* req->objstat.st_devid와 동일, media의 변경 기준 */
	time_t vtime;			/* 미디어의 만료 시간 */
	time_t mtime;			/* 미디어의 변경 시간 */
	uint64_t media_size;	/* 원본 미디어의 파일 크기 */
	uint64_t meta_size;		/* 저장되는 metadata 크기 */
} disk_minfo_t;
#pragma pack()

#define MEDIA_CACHE_HEADER_MAGIC 			0x5AA5B53B
#define MEDIA_CACHE_VERSION					3
#define MEDIA_CACHE_BASE_OFFSET				2048

#define	MEDIA_METADATA_CACHE_DIR_MAX_LEN	128
#define MEDIA_METADATA_CACHE_PATH_MAX_LEN	MEDIA_METADATA_CACHE_DIR_MAX_LEN+32
#define MEDIA_METADATA_CACHE_EXT			"mdc"	/* 캐시 파일 확장자 */
#define MEDIA_METADATA_CACHE_REMOVE			"REMOVE"	/* 삭제된 파일 경로 */


typedef struct tag_mdc_context {
	void	*req;
	FILE	*fd;
	off_t 	base_offset;	/* 기준 offset */
} mdc_context_t;

int mdc_init();
int mdc_deinit();
int mdc_disable_cache_file(nc_request_t *req, char *url);
zipperCnt mdc_load_cache(nc_request_t *req, struct nc_stat *objstat, char *url);
int mdc_save_cache(nc_request_t *req, media_info_t *media, int is_update);

#endif /*__META_DISK_CHCHE_H__ */
