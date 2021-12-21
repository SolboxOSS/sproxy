#ifndef __MDB_H__
#define __MDB_H__

struct tag_nc_mdb_handle;
typedef struct tag_nc_mdb_handle nc_mdb_handle_t;
void mdb_close_lru_cursor(void *v_cursor);
nc_mdb_inode_info_t ** mdb_cursor_entries(void *v_cursor *mdb, int maxcount, int *returned);
void * mdb_prepare_lru_cursor(nc_mdb_handle_t *mdb);


#endif /* __MDB_H__ */
