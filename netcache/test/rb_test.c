#include <netcache_config.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef HAVE_SYS_DIR_H
#include <sys/dir.h>
#endif

#ifdef HAVE_DIR_H
#include <dir.h>
#endif

#ifndef MODULE_TEST

#include "ncapi.h"
#include <netcache.h>
#include <block.h>
#include <sys/vfs.h>
#include <ctype.h>

#include "disk_io.h"
#include "util.h"
#include "lock.h"
#include "hash.h"
#include "bitmap.h"
#include "trace.h"
#include "bt_timer.h"
#include <mm_malloc.h>
#include <snprintf.h>
#endif

extern int __terminating;
long long		WR = 0;
long long		RD = 0;
#define			IOSIZE		16384
int				wloop = 1000000;
volatile long long		read_bytes; 
int 			wt = 1;
int 			rt = 1;
void *
rb_reader_thread(void *u)
{
	nc_xtra_options_t 		*opt =  (nc_xtra_options_t *)u;
	int 			r;//, len, len2;
	char			tbuf[IOSIZE];
	//int				__t;
	time_t				S,E;
	//long long 		R = 0;
	//long long 		loop = 0;
	int				xseq = 0;
	int				off = 0;
	int				lseq = 0;
	char 			*pb = tbuf;
	char			seqbuf[16];
	int				rcvd;


	E = S = time(NULL);
	while (!__terminating) {
		rcvd = 0;
		pb = tbuf;
		do {
			r = kv_oob_read(opt, pb, (IOSIZE - rcvd),  10*1000);
			if (r > 0) {
				pb += r;
				rcvd += r;
			}
		} while (r > 0 && rcvd < IOSIZE);
		if (r <= 0) {
			fprintf(stderr, "read error %d(EOT=%d)\n", r, kv_oob_endofdata(opt));
			break;
		}

		strncpy(seqbuf, tbuf, 9);
		if (atoi(seqbuf) != lseq) {
			fprintf(stderr, "LINE-SEQ mismatch : expected %d, received %d\n", lseq, atoi(seqbuf));
			exit(1);
		}
		else {
			lseq ++;
		}

		read_bytes += IOSIZE;
		pb = tbuf + 9;
		off = 0;
		xseq = 0;
		r  = r - 9;


		while (r > 0) {
			if (*pb == '\n') break;
			if (*pb != '0'+xseq) {
				fprintf(stderr, "%d expected, but '%c' received at buffer offset %d\n", xseq, *pb, off);
			}
			off++;
			xseq = (xseq + 1)% 10;
			pb++;
			r--;
		}
	}
L_read_eot:
	rt = 0;
	return NULL;
}
void *
rb_writer_thread(void *u)
{
	nc_xtra_options_t 		*opt =  (nc_xtra_options_t *)u;
	int 			r;//, len, len2;//, tlen;
	char			tbuf[IOSIZE];
	//int				__t;
	//int				nr = 0;
	int				i;
	//nc_uint32_t		seed = 0;
	char 			*pxbuf;
	int				loop = 0;
	int				remained = IOSIZE;
	//int				dlen = 0;

	srand(time(NULL));

	while (!__terminating && wloop > 0) {
		wloop--;

		remained = IOSIZE;
		sprintf(tbuf, "%08d:", loop);
		pxbuf 		= tbuf + 9;
		remained 	= remained -  9 ;

		for (i = 0; i < remained-1; i++) {
			*pxbuf =  '0' + i%10;
			pxbuf++;
		}
		*pxbuf='\n';

		r = kv_oob_write(opt, tbuf, IOSIZE);
		if (r <= 0) {
			fprintf(stderr, "write error %d\n", r);
			break;
		}
		else {
			ASSERT(r == IOSIZE);
		}
		loop++;
	}
	kv_oob_write_eot(opt);
L_write_done:
	wt = 0;
	return NULL;
}

int main()
{
	nc_xtra_options_t 		*opt;
	pthread_t				tid[2];
	long long				B;
	char					logfile[] = "./rb_test.log";
	nc_uint32_t				logmask = NC_WARN|NC_ERROR|NC_INFO;


 	nc_setup_log(logmask, logfile, 50000, 10);
	read_bytes = 0;
	opt = kv_create(__FILE__, __LINE__);
	kv_setup_ring(opt, IOSIZE/3);

	pthread_create(&tid[0], NULL, rb_reader_thread, opt);
	pthread_create(&tid[1], NULL, rb_writer_thread, opt);
//	pthread_join(tid[0], NULL);
//	pthread_join(tid[1], NULL);
	while (1 && rt > 0 && wt > 0) {
		sleep(1);
		B = read_bytes;
		fprintf(stderr, "**** read speed=%.2f Mbps(wloop=%d)\n", (B*8)/1000000.0, wloop);
		read_bytes -= B;
	}
	return 0;
}
