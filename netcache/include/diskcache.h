#ifndef __DISKCACHE_H__
#define __DISKCACHE_H__
fc_file_t * dm_lookup_inode(nc_volume_context_t *mnt, int lookup_type, const char *path, int *openhit, int makeref);
int dm_isolate_inode(fc_file_t *inode) ;



#endif /* __DISKCACHE_H__ */
