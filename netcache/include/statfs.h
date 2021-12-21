#ifndef _STATFS_H_
#define _STATFS_H_
struct statfs {
	long 	f_type;
	long 	f_bsize;
	long long 	f_blocks;
	long long 	f_bfree;
	long long 	f_bavail;
	long long 	f_files;
	long long 	f_ffree;
	long 		f_fsid;
	long 		f_namelen;
};
int statfs(const char *filepath, struct statfs *buf);
#endif /* _STATFS_H_ */
