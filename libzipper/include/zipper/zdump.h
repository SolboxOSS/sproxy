//
//  zdump.h
//  zipper
//
//  Created by voxio on 2016. 2. 1..
//  Copyright © 2016년 SolBox Inc. All rights reserved.
//

#ifndef zdump_h
#define zdump_h

#include "io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define zipper_dump_sync            MAKEFOURCC('z','d','m','p')
#define zipper_dump_current_ver     5

typedef struct _zipper_dump_header
{
    uint32_t sync;
    uint32_t ver:8;
    uint32_t ctxlen:24;
    uint32_t size;
     
} zipper_dump_header;

int load_vod_dump(void *vodctx, zipper_io_handle *io_handle, off_t *fo);

#ifdef __cplusplus
}
#endif

#endif /* zdump_h */
