//
//  srt.h
//  zipper
//
//  Created by Hyungpyo Oh on 2022/01/12.
//  Copyright © 2022 Voxio Dev. All rights reserved.
//

#ifndef srt_h
#define srt_h

#include "mp4.h"
#include "io.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    
    subAlignCenter = 0,
    subAlignLeft = 1,
    subAlignRight = 2,
    subAlignBottom = 0,
    subAlignTop = 4,
    subAlignMiddle = 8,
    subBold = 16,
    subItalic = 32,
};

typedef struct _subTitleHeader
{
    uint32_t line:6;
    uint32_t cpl:6;
    uint32_t attr:8;
    uint32_t len:12;
    uint32_t dur;
            
} subTitleHeader;

typedef struct _subtitleFrame
{
    char desc[16];
    uint8_t *p;
    uint64_t gts;
    uint64_t def:1;
    uint64_t seq:63;
    
    struct _subtitleFrame *next;

} subtitleFrame;

static const char webvttHeader[] = "WEBVTT\n\n";

static inline const char *srtItemTextOf(uint8_t *p) { return (const char *)&p[sizeof(subTitleHeader)]; }
static inline const subTitleHeader *srtItemHeaderOf(uint8_t *p) { return (const subTitleHeader *)p; };

int parse_srt(zipper_io_handle *io_handle, zidx_context *ctx, void *pdesc, off_t *fo);

#ifdef __cplusplus
}
#endif

#endif /* srt_h */
