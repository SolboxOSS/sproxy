//
//  flv.h
//  zipper
//
//  Created by Hyungpyo Oh on 2020/10/23.
//  Copyright © 2020 Voxio Dev. All rights reserved.
//

#ifndef flv_h
#define flv_h

#include "zipper.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLV_TYPEFLAG_VIDEO  (0x01 << 0)
#define FLV_TYPEFLAG_AUDIO  (0x01 << 2)

#define FLV_TAG_AUDIO       8
#define FLV_TAG_VIDEO       9
#define FLV_TAG_SCRIPT      18

#define FLV_TAG_HSIZE       15

#define FLV_VFRAME_KEY      1
#define FLV_VFRAME_INTER    2

typedef struct _flv_header
{
    uint8_t     tag[4];
    uint8_t     typeflag;
    uint32_t    hsize;
    
} __attribute__((packed, aligned(1))) flv_header;

typedef struct _flv_tag_header {
    
    uint32_t   pre_tag_size;
    uint32_t   type:8;
    uint32_t   size:24;
    uint32_t   pts;
    uint8_t    resv[3];
    uint8_t    flag;
    
} __attribute__((packed, aligned(1))) flv_tag_header;

typedef struct _flv_av_tag
{
    uint32_t    size;
    
    uint32_t    type:8;
    uint32_t    dsize:24;
    
    uint32_t    pts;
    uint32_t    flag;
    
} __attribute__((packed, aligned(4))) flv_av_tag;

static const uint8_t flv_metadata_tag_name[] = {
    
    0x00, 0x00, 0x00, 0x02, 0x00, 0x0A, 0x6F, 0x6E,
    0x4D, 0x65, 0x74, 0x61, 0x44, 0x61, 0x74, 0x61,
    0x08,
    0x00, 0x00, 0x00, 0x00, // 17
};

static const uint8_t flv_metadata_end[] = { 0x00, 0x00, 0x09 };

typedef struct _flv_metadata_tag
{
    uint32_t    size;
    
    uint32_t    type:8;
    uint32_t    dsize:24;
    
    uint32_t    pts;
    
} __attribute__((packed, aligned(4))) flv_metadata_tag;

static const uint8_t flv_avc_eos[] = {
    
    0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x17, 0x02, 0x00, 0x00, 0x00,
};

int parse_flv(zipper_io_handle *io_handle, zidx_context *ctx, zipper_media_desc *desc, off_t *fo);

uint8_t flv_codec_by_codec_index(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif /* flv_h */
