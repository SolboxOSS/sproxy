#ifndef __LOG_H__
#define __LOG_H__

#include <stdio.h>
#include <fcntl.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct _log_handle {
    FILE*  fp;             // file pointer of LogFile
    int    cnt;            // Log Count
    int    today_cnt;
    int    max_cnt;        // Max LogCount
    int    sys_alert_cnt;  // count syslog call
	pthread_mutex_t lock;

    int    file_size;      // Log File size
    char   file_title[128];
    char   file_basetitle[64];
    char   file_ext[12];
    char   file_name[256];
    char   log_dir[256];
} LOG_HANDLE ;

#define MAX_LOGFILE_SIZE    1024*1024*500        // 20 MB
#define MIN_LOGFILE_SIZE    1024*1024*1         // 1 MB
#define MAX_LOG_LENGTH      2048
#define MAX_LOGFILE_NAME    256

#ifdef WIN32
#define FOPEN(x, f, cnt) \
    if((x = fopen(f,"a+")) == NULL) {\
        if(cnt%100 == 0) {\
            if( cnt == 100 ) { cnt = 0 ;}\
        }\
        cnt++;\
        x = stderr;\
    }
#else
#define FOPEN(x, f, cnt) \
    if((x = fopen(f,"a+")) == NULL) {\
        if(cnt%100 == 0) {\
            syslog(1, "[BTLOGLIB] OpenLogFile line[%d] fopen fail !!! "\
                    "fn[%s] errmsg[%d:%s]. [%d]\n",\
                    __LINE__, f, errno, strerror(errno), cnt);\
            if( cnt == 100 ) { cnt = 0 ;}\
        }\
        cnt++;\
        x = stderr;\
    }
#endif 

#ifdef WIN32
#define FCLOSE(x, fn, cnt, ret)  \
    if( x != NULL && x != stderr) \
{\
    ret = fclose(x) ;\
    if(ret != 0) {\
        if(cnt%100 == 0) {\
            if( cnt == 100 ) { cnt = 0 ;}\
        }\
        cnt++;\
    } else {\
        x = NULL;\
    }\
}

#else

#define FCLOSE(x, fn, cnt, ret)  \
    if( x != NULL && x != stderr) \
{\
    ret = fclose(x) ;\
    if(ret != 0) {\
        if(cnt%100 == 0) {\
            syslog(1, "[BTLOGLIB] OpenLogFile line[%d] fclose fail. fn[%s] "\
                    "errmsg[%d:%s]. [%d]\n",\
                    __LINE__, fn, errno, strerror(errno), cnt); \
            if( cnt == 100 ) { cnt = 0 ;}\
        }\
        cnt++;\
    } else {\
        x = NULL;\
    }\
}

#endif

LOG_HANDLE* _init_log(const char *fname, int ms, int mc);
FILE* _open_log_file(LOG_HANDLE * plog);
int write_log(LOG_HANDLE * plog, char *fmt, ...);

#ifdef __cplusplus
};
#endif



#endif /*__LOG_H__*/
