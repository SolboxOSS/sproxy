#ifndef __ASIO_H__
#define __ASIO_H__


/*
 * 
 * ir #26497의 재수정으로 인해 아래 정의는 코멘트 처리
 * (2016.2.3)
 */

#pragma pack(8)

#define 	NC_MAX_VECTORIO 		128
/*
 * ASIO result code
 */
#define ASIOE_OK 					0
#define ASIOE_OSD_DOWN 				-1
#define ASIOE_CACHE_WRITE 			-2
#define ASIOE_CACHE_READ 			-3
#define ASIOE_INVALID_COMMAND 		-4



/*
 *
 * NetCache uniform IO context
 * 	
 */

#if 1
//typedef enum {IOT_UNKN=0, IOT_CACHE_READ=1, IOT_CACHE_WRITE=2, IOT_OSD_READ=3, IOT_OSD_WRITE=4, IOT_OSD_READ_ENTIRE=5, IOT_NULL=6 } nc_asio_type_t;
#define 	asio_is_command_type(_v, _i, _val) 	((_v)->iot[_i] == (_val))
#else
#define 	IOT_UNKN 				0
#define 	IOT_CACHE_READ 			1
#define 	IOT_CACHE_WRITE			2
#define 	IOT_OSD_READ			3
#define 	IOT_OSD_WRITE			4
#define 	IOT_OSD_READ_ENTIRE		5
#define 	IOT_NULL				6
#define 	asio_is_command_type(_v, _i, _val) 	((_v)->iot[_i] == (_val))
#endif

typedef enum {ASIO_ASYNC = 0, ASIO_SYNC = 1} nc_schedule_type_t;

#define 	ASIO_TYPE_CONTEXT 	"CTX"
#define 	ASIO_TYPE_VECTOR 	"VEC"


#define		ASIO_EOV			0x80000000

typedef enum {
	ASIOS_FREE= 		0,
	ASIOS_INIT= 		1,
	ASIOS_SCHEDULED= 	2,
	ASIOS_DISPATCHED= 	3,
	ASIOS_RUNNING= 		4,
	ASIOS_FRARRIVED= 	5, /* ir #29207 first response arrived */
	ASIOS_EOT= 			6, /* ir #29207 end of transfer */
	ASIOS_FINISHED=		7
} asio_state_t;


/* this state independently managed from ASIO's state */
#define		ASIOS_BIO_INIT		0 /* initial state*/ 
#define		ASIOS_BIO_PREP		1 /* in prepared state */
#define		ASIOS_BIO_REMIO		2 /* in remote-io */
#define		ASIOS_BIO_CPREP		3 /* in cascade prepared state */
#define		ASIOS_BIO_LOCIO		4 /* in local io */
#pragma pack(4)

struct nc_asio_context {
	nc_uint8_t				completed:1; 	/* io complete, applied to cachemgr */
	nc_uint8_t				cascade:1; 		/* io complete, applied to cachemgr */
	nc_uint8_t				state; 			/* block schedule state */

	nc_int16_t				error; 			/* IO error code, in success, 0 */
	nc_int32_t 				index;
	nc_blkno_t				blkno; 			


	nc_int32_t 				length;
	fc_blk_t				*block;			/* memory  where data copied to */
	nc_off_t 				offset;
	struct nc_asio_vector	*vector;		/* vector pointer */
}; 

typedef struct {
	nc_apcop_t				type;
	struct nc_asio_vector 	*vector;
	int 				  	cidx; /* context index */
} nc_asio_particle_t;
#pragma pack(0)
#define 	ASIO_CMD_FIRST 		0
#define 	ASIO_CMD_CASCADE 	1
struct nc_asio_vector; 

/* OSD_READ_ENTIRE 설정 시에는 context가 할당되지 않은 상태이므로,
 * driver는 asio_prepare_callback 함수를 통해서 그때 그때 context 블럭을 할당 받아야한다.
 */
typedef int (*nc_asio_prepare_callback_t)(struct nc_asio_vector *,  int cidx, nc_off_t offset, int prepchunk, int check_inode_lock);
typedef int (*nc_asio_block_io_callback_t)(struct nc_asio_vector *,  int cidx, nc_off_t offset, char *buffer, int len);
typedef void (*nc_asio_callback_t)(nc_asio_vector_t *, int idx, nc_off_t offset, void *tcb, nc_off_t len/*실제 해당 context에서 저장한 바이트 수*/, unsigned long err);
typedef nc_asio_type_t (*nc_user_callback_t)(nc_asio_type_t, fc_inode_t *inode, fc_blk_t *blk, int err, void *userdata);
typedef void (*nc_done_callback_t)(void *);

#pragma pack(8)
struct nc_asio_vector {
	nc_apcop_t 			type; 				/* APC_RUN_VECTOR, APC_READ_VECTOR_DONE 두가지 값을 가짐 */
	nc_asio_type_t		iot[2];				/* io type */
	nc_uint32_t			iov_id;				/* vector IO unique ID */
	nc_uint64_t			iov_signature;		/* inode signature  which used to check inconsistency between job and inode */
	nc_blkno_t			iov_sblkno;			/* start blk # */
	nc_int32_t			iov_cnt;			/* # of blocks necessary to do IO */
	nc_int32_t			iov_donecnt[2];		/* # of blocks handled so far */
	nc_uint8_t			iov_complex:1;		/* 1 if iot[0] in {OSD_READ} */
	nc_uint8_t 			iov_async:1;			/* 1 when the operation should be run in background */
	nc_uint8_t 			iov_hung:1;			/* 1 when the operation should be run in background */
	nc_uint8_t 			iov_inprogress:1;	/* 1 : IO가 현재 진행 중이며, 언제 끝날지 모르는 상태 */
	nc_uint8_t 			iov_needproperty:1;		/* 1 : IO가 현재 진행 중이며, 언제 끝날지 모르는 상태 */
	nc_int64_t 			iov_requested_bytes;
	nc_int64_t 			iov_xfer_bytes;
	nc_int32_t 			iov_xfer_blks;		/* # of blks filled */
	nc_int16_t			iov_error;			
	nc_int16_t 			iov_freed;

	nc_asio_prepare_callback_t	iov_asio_prep_context_callback;	/* called whenever a block IO completed regardless of success */
	nc_asio_block_io_callback_t	iov_asio_bio_callback;
	nc_asio_callback_t			iov_asio_callback;	/* called whenever a block IO completed regardless of success */
	nc_user_callback_t			iov_user_callback;	/* called whenever a block IO completed regardless of success */
	nc_done_callback_t			iov_done_callback;	/* called whenever vector destroyed */

	nc_path_lock_t		*lock;
	fc_inode_t			*iov_inode;
	int					 iov_kv_out_nf; /* 1 if free needed */
	nc_kv_list_t 		*iov_kv_out; 			
	nc_kv_list_t 		*iov_kv_in; 		
	void				*iov_cb; 
	void				*iov_done_cb; 
	void 				*iov_priv;
	nc_int32_t 			iov_allocated_context;
	asio_state_t 		state;
	nc_int8_t 			canceled;
	nc_int8_t 			locked;
#ifdef __PROFILE
	perf_val_t			t_begin;
	perf_val_t			t_schedule;
	perf_val_t			t_end;
	int 				f_prep; 	/* prepare_context call count */
#endif
	int 				*per_thread_running;
	link_node_t			node; 				/* necessary ? */
	bt_timer_t 			hang_timer;
	void				*priv;
	nc_asio_context_t	*iob;
}; 
typedef struct tag_asio_debug {
 	nc_uint64_t 	id;
	link_node_t 	node;
} asio_debug_t;
#pragma pack()

#define 	ASIO_BLOCK_NO(_v, _i) 		((_v)->iob[_i].blkno)
#define 	ASIO_BLOCK(_v, _i) 		((_v)->iob[_i].block)
#define 	ASIO_CONTEXT(_v, _i) 		(&(_v)->iob[_i])

#define	ASIO_VALID_IO(v)		((v) && (ASIO_VALID_VECTOR(v->vector)) && (strncmp((v)->type, ASIO_TYPE_CONTEXT, 3) == 0))
#define	ASIO_VALID_VECTOR(v)	((v) && ((v)->iov_signature == (v)->iov_inode->signature))

#ifndef NC_RELEASE_BUILD
#define		DEBUG_ASSERT_ASIO(v_, i_, x_)  \
	if (!(x_)) { \
		char	_dzbuf[256] = ""; \
		char	_ibuf[2048] = "NULL"; \
		if (i_) dm_dump_lv1(_ibuf, sizeof(_ibuf), (i_)); \
		TRACE((g__trace_error, (char *)"Error in Expr '%s' at %d@%s \n" \
									   "\t\t\t ASIO : %s\n" \
									   "\t\t\t INODE: %s\n", \
										#x_, __LINE__, __FILE__,  \
										asio_dump_lv1(_dzbuf, sizeof(_dzbuf), v_),\
										_ibuf \
										)); \
		TRAP; \
	} 
#define		DEBUG_ASSERT_ASIOC(e, n_, x)  \
	if (!(x)) { \
		char	_dzbuf[1024] = ""; \
		asio_dump_lv1(_dzbuf, sizeof(_dzbuf), e); \
		TRACE((g__trace_error, (char *)"Error in Expr '%s' (ctxidx=%d) at %d@%s : %s\n", #x, n_, __LINE__, __FILE__, _dzbuf));  \
		TRAP; \
	} 
#else
#define		DEBUG_ASSERT_ASIO(v_, i_, x_)  
#define		DEBUG_ASSERT_ASIOC(e, n_, x)  
#endif

void asio_init(void);
void asio_shutdown(void);
int asio_vector_count(nc_asio_vector_t *biov);
int asio_vector_full(nc_asio_vector_t *biov);
void asio_set_inode(nc_asio_vector_t *biov, fc_inode_t *inode);
nc_asio_vector_t * asio_new(fc_inode_t *inode, nc_asio_type_t first_type, nc_asio_type_t cas_type,  nc_blkno_t sblkno, int cnt, nc_user_callback_t callback, void *cb, int bwait, nc_kv_list_t *, int need_property);
int asio_append_block(nc_asio_vector_t *biov, fc_blk_t *blk);
int asio_schedule(nc_asio_vector_t *biov, nc_schedule_type_t stype);
int asio_cascade_io(nc_asio_context_t *bio);
void asio_cancel_io(nc_asio_vector_t *biov, int done, void *tcb, int err);
int asio_signal_cancel(fc_inode_t *inode);
void asio_destroy(fc_inode_t *inode, nc_asio_vector_t *biov);
char * asio_dump_lv1( char *buf, int buflen, nc_asio_vector_t	*vector);
int asio_cancel_schedule(nc_asio_vector_t *v, int e);
int asio_next_command(nc_asio_vector_t *v, int cindex);
int asio_update_done(nc_asio_vector_t *asiov, int cascade);
int asio_need_EOV(nc_asio_vector_t *asiov);
int asio_count_blocks(nc_asio_vector_t *asiov);
void asio_post_apc(void *d, const char *, int l);
char * asioc_dump_prefix(char *buf, int buflen, nc_asio_vector_t *v, int idx);
char * asiov_dump_prefix(char *buf, int buflen, nc_asio_vector_t *v);


#endif /* __ASIO_H__ */
