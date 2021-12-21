#ifndef __FIO_H__
#define  __FIO_H__


/*
 * 이 구조체는 open요청 하나마다 동적으로 할당된다
 * close호출 시 삭제
 */
typedef struct tag_nc_file_ref {
	nc_volume_context_t *volume;
	nc_path_lock_t		*lock;
	fc_inode_t 			*inode;
	nc_kv_list_t 		list; /*extended key-value list if any*/
	mode_t 				mode;
} fc_file_t;

#endif
