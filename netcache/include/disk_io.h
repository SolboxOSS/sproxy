#ifndef __DISK_IO_H
#define __DISK_IO_H

#ifndef WIN32
typedef void (*dio_callback_t)(int fd, long long off, size_t len, char *buf, int err, void *cb);

int dio_init(int max_ioreq);
int dio_count_worker();
int dio_file_size(int fd, nc_size_t *sz);
int dio_valid(int fd) ;
void dio_set_write_done_callback(dio_callback_t callback);
void dio_set_read_done_callback(dio_callback_t callback);
int dio_open_file(char *filename, long long fsize, int bcreat, int brdonly);
nc_fio_handle_t dio_open_file_direct(char *filename, long long fsize, int bcreat, int brdonly);
int dio_close_file(int fdesc);
int dio_submit_write(int fd, long blkno, char *block, long long off, size_t len, void *cb);
int dio_submit_read(int fd, char *block, long long off, size_t len, void *data);
int dio_schedule(int waitinmsec);
int dio_submit_writev(int fd, long blkno[], char *block[], long long off[], size_t len[], void *data, int nvec);
int dio_submit_readv(int fd, long blkno[], char *block[], long long off[], size_t len[], void *data, int nvec);
#else
int dio_extend_file(HANDLE hf, long long fsize);
int dio_file_size(HANDLE fd, nc_size_t *sz);
typedef void (*dio_callback_t)(int fd, long long off, size_t len, char *buf, int err, void *cb);
int dio_init(int max_ioreq);
int dio_valid(HANDLE fd) ;
void dio_set_write_done_callback(dio_callback_t callback);
void dio_set_read_done_callback(dio_callback_t callback);
HANDLE dio_open_file(char *filename, long long fsize, int bcreat, int brdonly);
nc_fio_handle_t dio_open_file_direct(char *filename, long long fsize, int bcreat, int brdonly);
int dio_close_file(HANDLE fdesc);
int dio_submit_write(HANDLE fd, long blkno, char *block, long long off, size_t len, void *cb);
int dio_submit_read(HANDLE fd, char *block, long long off, size_t len, void *data);
int dio_schedule(int waitinmsec);
int dio_submit_writev(HANDLE fd, long blkno[], char *block[], long long off[], size_t len[], void *data, int nvec);
int dio_submit_readv(HANDLE fd, long blkno[], char *block[], long long off[], size_t len[], void *data, int nvec);
int dio_extend_file(HANDLE hf, long long fsize);
#endif
int dio_block_io(nc_asio_vector_t *, int, nc_asio_type_t type, tp_data_t tcb);
int dio_block_io_vector(nc_asio_vector_t *asiov, nc_asio_type_t type, tp_data_t tcb);
int dio_block_io_sync(nc_asio_context_t *asioc, nc_asio_type_t type, tp_data_t tcb);
int dio_busy();
int dio_get_wait(int *maxios, int *pending);
void dio_get_params(void *iocb, nc_asio_vector_t **v, int *bidx, long *transfered, long *error);
void __dio_put_free(void *nciocb);

#endif /* __DISK_IO_H */
