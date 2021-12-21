#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#ifndef WIN32
#include <syslog.h>
#endif
#include <pthread.h>

#include "util.h"
#include "log.h"
#include <snprintf.h>

int  _rename_old_log_files(const char *logdir,
        const char *logfiletitle,
        const char *logext,
        int   todaycnt,
        int   maxcnt);

int   _rename_old_log_index(char *name,
        int  index,
        int  *piorgindex,
        int  *parr,
        int  *pmaxcnt);

pthread_mutex_t logLock;

void _init_log_lock()
{
    pthread_mutex_init(&logLock, NULL);
}

LOG_HANDLE* _init_log(const char *fname, int ms, int mc)
{
	pthread_mutexattr_t mattr;
    /* Initialize  */
    LOG_HANDLE * plog = (LOG_HANDLE*)malloc(sizeof(LOG_HANDLE));

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, NC_PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&plog->lock, &mattr);
    plog->cnt = 0 ;
    plog->today_cnt = 0 ;
    plog->max_cnt = (mc > 5000) ? 5000 : ((mc < 0)? 0 : mc);
    plog->sys_alert_cnt = 0 ;
    plog->fp = NULL ;
    memset(plog->file_title, 0x00, sizeof(plog->file_title));
    memset(plog->file_name , 0x00, sizeof(plog->file_name));

    /* Check LogFileSize */
    plog->file_size = (((ms) > (MAX_LOGFILE_SIZE)) ? (MAX_LOGFILE_SIZE) :
                  (((ms) < (MIN_LOGFILE_SIZE)) ? (MIN_LOGFILE_SIZE) : (ms)));

    /* Check LogFileName */
    if (fname == NULL || !strlen(fname))
    {
        report_error_syslog("SBFS","[LOG] Invalid LogFile Name\n");
        free(plog);
        return NULL;
    }
    else
    {
        /* Title and Ext */
        char    *p;
        int     i, iresult ;
        char    sz_log_temp[256];
        time_t  now;
        struct tm  tm_now, tm_res;

        snprintf(sz_log_temp, sizeof(sz_log_temp)-1, "%s", fname);
        p = strrchr(sz_log_temp, '.');
        if(p)
        {
            strcpy(plog->file_ext, p+1);
            *p = '\0' ;

#ifdef WIN32
            p = strrchr(sz_log_temp , '\\');
			if (!p) 
            	p = strrchr(sz_log_temp , '/');
#else
            p = strrchr(sz_log_temp , '/');
#endif
            if(p)
            {
                strcpy(plog->file_basetitle, p+1);
                *p = '\0' ;
                snprintf(plog->log_dir, sizeof(plog->log_dir)-1, "%s", sz_log_temp);
            }
            else
            {
                snprintf(plog->file_basetitle, sizeof(plog->file_basetitle)-1, "%s", sz_log_temp);
                snprintf(plog->log_dir, sizeof(plog->log_dir)-1, ".");
            } /* end of if */
        }
        else
        {
            /* set default extension name (log) */
            snprintf(plog->file_ext, sizeof(plog->file_ext), "log");
#ifdef WIN32
            p = strrchr(sz_log_temp , '\\');
			if (!p) 
            	p = strrchr(sz_log_temp , '/');
#else
            p = strrchr(sz_log_temp, '/');
#endif
            if( p )
            {
                strcpy(plog->file_basetitle, p+1 );
                *p = '\0' ;
                snprintf(plog->log_dir, sizeof(plog->log_dir)-1, "%s", sz_log_temp);
            }
            else
            {
                snprintf(plog->file_basetitle, sizeof(plog->file_basetitle)-1, "%s", sz_log_temp);
                snprintf(plog->log_dir, sizeof(plog->log_dir), "." );
            } /* end of if */
        } /* end of if */

        /* Get LogFileName By Date */
        now = nc_cached_clock();
        tm_now = *localtime_r(&now, &tm_res);

        snprintf(plog->file_title, sizeof(plog->file_title)-1 , "%s_%04d%02d%02d",
                plog->file_basetitle,
                tm_now.tm_year+1900, tm_now.tm_mon+1, tm_now.tm_mday);

        snprintf(plog->file_name, sizeof(plog->file_name)-1, "%s/%s_%04d%02d%02d.%s",
                plog->log_dir,
                plog->file_basetitle,
                tm_now.tm_year+1900, tm_now.tm_mon+1, tm_now.tm_mday,
                plog->file_ext);

        DIR *check = NULL;
        check = opendir(plog->log_dir);
        if(!check)
        {
            report_error_syslog("SBFS","[Log] Directory doesn't exist! [%s]\n",plog->log_dir);
            free(plog);
            return NULL;
        }

		/* BUG 2018-08-30 huibong opendir() 호출 관련 closedir() 호출 누락 수정 (#32357) */
		closedir(check);

        i = 0 ;
        do
        {
            snprintf(sz_log_temp, sizeof(sz_log_temp)-1, "%s.%d", plog->file_name, i);

            /* 0 if OK , -1 on error : F_OK - test for existence of file */
            iresult =  access(sz_log_temp , F_OK);
            if( iresult == 0) i++ ;
        } while (iresult == 0);

        plog->cnt = i;
    }

    return plog;
}

FILE* _open_log_file(LOG_HANDLE *plog)
{
    if(!plog) return stderr;

    long offset = 0;
    time_t now  = 0;
    struct tm tm_now;
    char sz_temp_file[256] = {0};
    int iret = 0;

    pthread_mutex_lock(&plog->lock);
    now = nc_cached_clock();
    localtime_r(&now, &tm_now);
    snprintf(sz_temp_file, sizeof(sz_temp_file)-1, "%s/%s_%04d%02d%02d.%s",
            plog->log_dir,
            plog->file_basetitle,
            tm_now.tm_year+1900, tm_now.tm_mon + 1, tm_now.tm_mday,
            plog->file_ext);
#if 0 /*def WIN32 */
	if (plog->fp == NULL) {
        	FOPEN( plog->fp, sz_temp_file, plog->sys_alert_cnt);
        	memset(plog->file_name, 0x00, sizeof(plog->file_name));
        	strcpy(plog->file_name, sz_temp_file);
	}
#else
    if(strcmp(sz_temp_file, plog->file_name) != 0)
    { 
        /* Date Changed */
        snprintf(plog->file_title, sizeof(plog->file_title)-1, "%s_%04d%02d%02d",
            plog->file_basetitle,
            tm_now.tm_year+1900, tm_now.tm_mon + 1, tm_now.tm_mday);

        if(plog->fp != NULL && plog->fp !=stderr)
        {
            FCLOSE(plog->fp, plog->file_name, plog->sys_alert_cnt, iret);

            plog->fp = NULL ;
        }
        plog->today_cnt = 0;

        FOPEN( plog->fp, sz_temp_file, plog->sys_alert_cnt);
        memset(plog->file_name, 0x00, sizeof(plog->file_name));
        strcpy(plog->file_name, sz_temp_file);
    }
    else
    { 
        /* Same Date */
        if(plog->fp !=NULL && plog->fp != stderr)
        {
            iret = access(sz_temp_file, W_OK);
            if(iret != 0)
            { 
                /* an error has previously occurred reading from  or writing */
                if(plog->sys_alert_cnt%100 == 0)
                {
#ifndef WIN32
                    syslog(LOG_ALERT,
                            "[LOG] OpenLogFile line[%d] fn[%s] for "
                            "write permission. failed. errmsg[%d:%s]\n",
                            __LINE__, sz_temp_file, errno, strerror(errno));
#endif
                    plog->sys_alert_cnt=0;
                }

                plog->sys_alert_cnt++;

                FCLOSE(plog->fp,sz_temp_file,plog->sys_alert_cnt,iret);
                FOPEN( plog->fp,sz_temp_file,plog->sys_alert_cnt);
            }

            /* Already Logfile opened */
            offset = ftell(plog->fp);
#ifndef WIN32
            if(offset >= plog->file_size)
            {
                FCLOSE(plog->fp, plog->file_name,
                        plog->sys_alert_cnt, iret);

                plog->fp = NULL;
                _rename_old_log_files(plog->log_dir,
                        plog->file_title,
                        plog->file_ext,
                        plog->today_cnt,
                        plog->max_cnt);

                snprintf(sz_temp_file, sizeof(sz_temp_file)-1, "%s.0", plog->file_name);

                /* rename logfile.ext -> logfile.ext.0 */
                rename(plog->file_name , sz_temp_file) ;

                FOPEN(plog->fp, plog->file_name,
                        plog->sys_alert_cnt);

                plog->today_cnt++;
            }
            else if(offset < 0)
            {
                /* fp error */
                if(plog->sys_alert_cnt%100 == 0)
                {
#ifndef WIN32
                    syslog(LOG_ALERT,
                            "[BTLOGLIB] ftell fail. line[%d] file[%s:%p] ret[%d]"
                            " errmsg[%d:%s]\n",
                            __LINE__, plog->file_name , plog->fp,
                            (int)offset, errno, strerror(errno));
#endif
                    plog->sys_alert_cnt=0;
                }
                plog->sys_alert_cnt++ ;

                FCLOSE(plog->fp, plog->file_name,
                        plog->sys_alert_cnt,  iret);

                FOPEN(plog->fp, plog->file_name,
                        plog->sys_alert_cnt);
            } /* end of if */
#endif

        }
        else if(plog->fp == stderr)
        {
            FOPEN( plog->fp , plog->file_name, plog->sys_alert_cnt);
            if(plog->sys_alert_cnt%100 == 0)
            {
#ifndef WIN32
                syslog(LOG_ALERT,
                        "[BTLOGLIB] OpenLogFile fp is stderr. retry to fopen. "
                        "get FP[%p]\n",
                        plog->fp);
#endif
                plog->sys_alert_cnt=0;
            }
            plog->sys_alert_cnt++ ;
        }
        else
        {
            FOPEN( plog->fp , plog->file_name, plog->sys_alert_cnt);
        } /* end of if */
    } /* end of if */
#endif
    pthread_mutex_unlock(&plog->lock);

    return plog->fp;
}

int   _rename_old_log_files(const char *logdir,
                            const char *logfiletitle,
                            const char *logext,
                            int   todaycnt,
                            int   maxcnt)
{

    DIR             *dp =NULL;
    struct dirent   *dirp ;
    int     slot[5000] ;
    char    *p ;
    char    str_idx[12];
    char    pc_title_dot[256];
    char    pc_temp[256];
    char    pc_fname[256];
    int     idx = 0;



    memset(&slot, 0, sizeof(slot));

    snprintf(pc_title_dot, sizeof(pc_title_dot), "%s.%s.", logfiletitle, logext);

    /* Open Log Directory */
    dp = opendir(logdir) ;
    if( dp == NULL )
    {
        fprintf(stderr,
                "[LOG] %s opendir(%s) fail. errmsg[%d:%s]\n",
                __FUNCTION__, logdir, errno, strerror(errno));
        return 0 ;
    } /* end of if */

    while((dirp = readdir(dp)) != NULL)
    {
        if(strcmp( dirp->d_name, ".") == 0 || strcmp(dirp->d_name,"..") == 0)
        {
            continue ;
        }

        if(strncmp(dirp->d_name, pc_title_dot, strlen(pc_title_dot)) == 0)
        {
            memset(pc_temp, 0x00, sizeof(pc_temp));

            strcpy(pc_temp, dirp->d_name);
            p = strrchr(pc_temp, '.');
            strcpy(str_idx, p+1);
            *p = '\0';
            idx = atoi(str_idx);

            if(idx < sizeof(slot) && idx < todaycnt && idx <= maxcnt)
            {
                if(!slot[idx])
                {
                    snprintf(pc_fname, sizeof(pc_fname), "%s/%s", logdir, pc_temp);


                    _rename_old_log_index(pc_fname,
                                          idx,
                                          &idx,
                                          &slot[0],
                                          &maxcnt);
                }
            } /* end of if */
        } /* end of if */
    } /* end of while */

    if( dp != NULL ) closedir(dp);

    return 1 ;
} /* end of function */

/*-----------------------------------------------------------------------------*
 * Internal function
 */
int  _rename_old_log_index(char *name,
                            int  index,
                            int  *piorgindex,
                            int  *parr,
                            int  *pmaxcnt)
{
    int  iret;
    char filename[256];
    char newfilename[256];

    snprintf(filename,    sizeof(filename)-1, "%s.%d", name , index);
    snprintf(newfilename, sizeof(newfilename)-1,"%s.%d", name , index+1);

    if(access(newfilename, F_OK) == 0)
    {
        if(index>*pmaxcnt-2)
        {
            /* Because MaxLogCount is over , and overwrite  */
            rename(filename , newfilename);
            *(parr+index) = 1 ; // Renamed Info Update
            if( index > *piorgindex )
            {
                _rename_old_log_index(name, index-1, piorgindex, parr, pmaxcnt);
            }
        }
        else
        {
            iret = _rename_old_log_index(name, index+1, piorgindex, parr,
                                         pmaxcnt);
        }
    }
    else
    {
        if(index < *pmaxcnt)
        {
            rename(filename , newfilename);
            *(parr+index) = 1 ; // Renamed Info Update
            if(index > *piorgindex)
            {
                _rename_old_log_index(name, index-1, piorgindex, parr, pmaxcnt);
            } /* end of if */
        } /* end of if */
    } /* end of if */

    return 1;

} /* end of function */

int write_log(LOG_HANDLE * plog, char *fmt, ...)
{
    FILE *fp = _open_log_file(plog);
	va_list ap;
	struct timeval 	tp;
	struct tm		tm_now;
	char			sztime[64];
	char 			*depth_str = NULL;
	int				depth_str_len,uid,n;
	char 			*p;

	depth_str = (char *)XMALLOC(1024+1, AI_ETC);
	depth_str_len = 1024;
#if 0
    	uid = ((pthread_self() >> 8) & 0xFFFF);
#else
	uid = (long)gettid() & 0xFFFF;
#endif

	gettimeofday(&tp, NULL);
	localtime_r(&tp.tv_sec,&tm_now);
	p = depth_str;
	strftime(sztime, sizeof(sztime), "%m-%d %H:%M:%S", &tm_now);
	n = snprintf(p, depth_str_len, "%s/%03d", sztime, (int)(tp.tv_usec / 1000));
	p += n;
	depth_str_len -= n;

	n = snprintf(p, depth_str_len, " {%02d} ", uid);
	p += n;
	depth_str_len -= n;
    *p = 0;

    pthread_mutex_lock(&logLock);
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    fflush(fp);
    va_end(ap);
    pthread_mutex_unlock(&logLock);
    XFREE(depth_str);

    return 0;
}

