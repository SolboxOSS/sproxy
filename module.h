#ifndef __MODULE_H__
#define __MODULE_H__

#define MAX_MODULE_NAME_LEN			64
#define MAX_MODULE_DESC_LEN			256
#define __MODULE_HANDLER_VERSION	1	/* 모듈 구조가 변경됨에 따라 버전이 올라간다. */

#define REQUEST_ARGUMENT_KIND	1
#define REQUEST_HEADER_KIND		2

/*
 * 동적 모듈 작성시 __MODULE_HANDLER_VERSION 버전과의 호환성을 생각해서 작성해야 한다.
 * module init 단계에서 반드시
 * 모듈 HANDLER 버전에 따른 기능 변경사항
 * 1 : 기본 callback function 추가
 */

/* 여기에 get_module_magic_first와 get_module_magic_last를 이용해서 검사하는 매크로를 만들어야 한다. */
#define MODULE_CONSISTENCY_CHECK(module, m_version) { \
				int h_version = module.callback->get_module_handler_version();	\
				if(h_version < m_version) {	\
					TRACE((T_ERROR, "Module handler version(%d) too old, '%s' module required version(%d).\n", h_version, module.name, m_version));	\
					ASSERT(0);\
				}\
			}


typedef enum {
	PHASE_VOLUME_CREATE,	/* service 파라미터 필요 */
	PHASE_VOLUME_RELOAD,	/* service 파라미터 필요 */
	PHASE_VOLUME_DESTROY,	/* service 파라미터 필요 */
	PHASE_ACCEPT_POLICY,	/* sockaddr * 파라미터 */
	PHASE_URI_PARSE,		/* req 파라미터 필요 */
	PHASE_HOST_REWRITE,		/* req 파라미터 필요 */
	PHASE_VOLUME_LOOKUP,	/* req 파라미터 필요 */
	PHASE_HEADER_PARSE,		/* req 파라미터 필요 */
	PHASE_CLIENT_REQUEST,	/* req 파라미터 필요 */
	PHASE_CONTENT_VERIFY,	/* req 파라미터 필요 */
	PHASE_BUILD_RESPONSE,	/* req 파라미터 필요 */
	PHASE_CLIENT_RESPONSE,	/* req 파라미터 필요 */
	PHASE_COMPLETE,			/* req 파라미터 필요 */
	PHASE_COUNT
} phase_type_t;

typedef enum {
	MODULE_TYPE_STATIC = 0,
	MODULE_TYPE_DYNAMIC
} module_type_t;

typedef struct phase_context {
	void *service;	/* service_info_t */
	void *req;	/* nc_request_t	*/
	void *addr;	/* struct sockaddr */
	void *command; /* nc_origin_io_command_t */
} phase_context_t;

typedef int (*module_func)(phase_context_t *ctx);	/* index는 모듈을 구분하기 위한 번호임 */


/*
 * 모든 module function 들은 정상인 경우 SCX_YES을 리턴하고 에러가 발생한 경우 SCX_NO을 리턴해야 함
 */

typedef int (*dyn_reg_phase_func)(phase_type_t phase, module_func func);
typedef void *(*cb_void_p_func)();	/* 리턴 타입이 void * 인 callback function 프로토 타입 */
typedef void (*cb_void_func)();	/* 리턴 타입이 void 인 callback function 프로토 타입 */
typedef int (*cb_int_func)();	/* 리턴 타입이 int 인 callback function 프로토 타입 */
/* solproxy의 함수중 dynamic module에서 사용이 필요한것들을 callback으로 넘겨기 위한 구조체
 * 버전간의 호환성 문제 때문에 각 function의 위치는 절대 변경해서는 안된다.
 * */
typedef struct module_callback_tag {
	cb_int_func			get_module_handler_version;	/* 모듈 핸들러의 호환성 검사용 function */
	dyn_reg_phase_func 	reg_phase_func;				/* 모듈의 phase 핸들러 등록 function */
	cb_void_p_func	 	malloc;
	cb_void_p_func	 	calloc;
	cb_void_p_func	 	realloc;
	cb_void_func	 	free;
	cb_void_p_func		mp_create;
	cb_void_p_func		mp_alloc;
	cb_void_func		mp_free;
	void 				*reserved[41];	/* callback function이 한개 추가 될때마다 여기에서 한개씩 빼야한다. 전체 50개 */
} module_callback_t;

/* module의 name은 변수명 규칙과 유사하게 공백 없이 만들어져야 한다. */
typedef struct module_driver {
	char 	name[MAX_MODULE_NAME_LEN+1];
	char	desc[MAX_MODULE_DESC_LEN+1];	/* 모듈에 대한 설명 */
	int	 	version;	/* 모듈 버전 */
	int		(*init)();
	int		(*deinit)();
	int		index;		/* 메모리에 올라간 모듈 순서, service_info나 nc_request에 포함된 module_ctx를 사용하기 위한 index */
	module_callback_t	*callback;
} module_driver_t;


typedef struct scx_static_module_tag {
	char			name[MAX_MODULE_NAME_LEN+1];	/* 설정파일에서 모듈지정시 사용하는 이름 */
	int				is_base;					/* 기본으로 로딩 되어야 하는 경우 1, 지정시만 로딩 되어야 하는 경우는 0 */
	module_driver_t *driver;
} scx_static_module_t;


typedef struct scx_module_tag {
	char			name[MAX_MODULE_NAME_LEN+1];	/* 설정파일에서 모듈지정시 사용하는 이름 */
	module_driver_t *driver;
	module_type_t	type; 		/* module가 정적/동적인지를 나타냄 */
	void 			*d_handle;		/* dynamic module에서의 open handle */
	int				is_base;
} scx_module_t;



int scx_init_module();
int scx_deinit_module();
int scx_reg_phase_func(phase_type_t phase, module_func func);
int scx_phase_handler(phase_type_t phase, phase_context_t *ctx);



#endif /*__MODULE_H__ */
