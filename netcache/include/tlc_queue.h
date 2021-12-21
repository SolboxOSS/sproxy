/************************************************************************

                   (c) COPYRIGHT 2007 by SOLUTIONBOX, Inc.
                          All rights reserved.

     This software is confidential and proprietary to SOLUTIONBOX, Inc.  
     No part of this software may be reproduced, stored, transmitted, 
     disclosed or used in any form or by any means other than as 
     expressly provided by the written license agreement
     between Besttoday and its licensee.
     
                             SOLUTIONBOX, Inc.
                           4F, Sunghwan Bldg.  
                     770-9 Yeoksam-Dong, Kangnam-Gu,
                      Seoul, 135-080, South Korea
 
                        Tel: +82 (2) 2182-3600
                        Fax: +82 (2) 2058-2651     

******************************************************************************/

/******************************************************************************
                         Two-lock based Concurrent Queue
                     ---------------------------------------
      begin              :  2009-06-16
      email              :  Tawan Won   (taehwan.weon@gmail.com)
 ******************************************************************************/

/******************************************************************************
    change history
    2007. 9. 26 : initial code (frau)
    2009. 6. 16 : resotred to almostly the original one.
 ******************************************************************************/
#ifndef __TLC_QUEUE_H__
#define __TLC_QUEUE_H__
#include <pthread.h>
#include <util.h>


#pragma pack(8)
typedef struct tlcqueue {
	link_list_t			list;

	int					waitable; /* default 0 */
    pthread_mutex_t     *lock;	/* condition lock, used only if waitable=1 */
    pthread_cond_t      waitcond;
} tlc_queue_info_t;
#pragma pack()
typedef  tlc_queue_info_t *tlc_queue_t;

tlc_queue_t   tlcq_init(int waitable);
tlc_queue_t   tlcq_init_with_lock(int waitable, pthread_mutex_t *lock);
int 		 tlcq_length(tlc_queue_t q);
int			 tlcq_enqueue(tlc_queue_t q, void *buf);
void *		 tlcq_dequeue(tlc_queue_t q, int msec);
void 		 tlcq_destroy(tlc_queue_t q);
int 		 tlcq_dequeue_batch(tlc_queue_t q, void *va[], int array_len, int msec);


#endif /* __TLC_QUEUE_H__ */

/******************************************************************************
                             E N D  O F  F I L E
 ******************************************************************************/
