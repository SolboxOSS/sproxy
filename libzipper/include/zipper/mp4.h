//
//  mp4.h
//  zipper
//
//  Created by voxio on 2014. 9. 17..
//  Copyright (c) 2014년 SolBox Inc. All rights reserved.
//

#ifndef zipper_mp4_h
#define zipper_mp4_h

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BORDERCNV
#define BORDERCNV

#define BE16(a)                                                         \
((((a)>>8)&0xFF) |                                                      \
(((a)<<8)&0xFF00))
#define BE24(a)                                                         \
((((a)>>16)&0xFF) |                                                     \
((a)&0xFF00) |                                                          \
(((a)<<16)&0xFF0000))
#define BE32(a)                                                         \
((((a)>>24)&0xFF) |                                                     \
(((a)>>8)&0xFF00) |                                                     \
(((a)<<8)&0xFF0000) |                                                   \
((((a)<<24))&0xFF000000))
#define BE64(a)                                                         \
(BE32(((a) >> 32) & 0xFFFFFFFFLL) |                                     \
((BE32((a)&0xFFFFFFFFLL)<<32)&0xFFFFFFFF00000000LL))

#endif

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |   \
((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif

#define MULTI_TRACK_MAX     8

static const uint32_t aac_freq[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0 };

// parse
static const uint16_t mp3_sample_rate_table[16] = {
    
    11025,  12000,  8000,   0,
    0,      0,      0,      0,
    22050,  24000,  16000,  0,
    44100,  48000,  32000,  0,
};

static const uint16_t mp3_bitrate_table[256] = {
    
    // reserved
    0,  0,  0,  0,  0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,
    // mpeg2.5 layer 3
    0,  8,  16, 24, 32,     40,     48,     56,     64,     80,     96,     112,    128,    144,    160,    0,
    // mpeg2.5 layer 2
    0,  8,  16, 24, 32,     40,     48,     56,     64,     80,     96,     112,    128,    144,    160,    0,
    // mpeg2.5 layer 1
    0,  32, 48, 56, 64,     80,     96,     112,    128,    144,    160,    176,    192,    224,    256,    0,
    // reserved
    0,  0,  0,  0,  0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,
    // reserved
    0,  0,  0,  0,  0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,
    // reserved
    0,  0,  0,  0,  0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,
    // reserved
    0,  0,  0,  0,  0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,
    // reserved
    0,  0,  0,  0,  0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,
    // mpeg2 layer 3
    0,  8,  16, 24, 32,     40,     48,     56,     64,     80,     96,     112,    128,    144,    160,    0,
    // mpeg2 layer 2
    0,  8,  16, 24, 32,     40,     48,     56,     64,     80,     96,     112,    128,    144,    160,    0,
    // mpeg2 layer 1
    0,  32, 48, 56, 64,     80,     96,     112,    128,    144,    160,    176,    192,    224,    256,    0,
    // reserved
    0,  0,  0,  0,  0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,      0,
    // mpeg1 layer 3
    0,  32, 40, 48, 56,     64,     80,     96,     112,    128,    160,    192,    224,    256,    320,    0,
    // mpeg1 layer 2
    0,  32, 48, 56, 64,     80,     96,     112,    128,    160,    192,    224,    256,    320,    384,    0,
    // mpeg1 layer 1
    0,  32, 64, 96, 128,    160,    192,    224,    256,    288,    320,    352,    384,    416,    448,    0,
};

static const uint16_t mp3_samples_table[16] = {
    
    0,      576,    1152,   384,
    0,      0,      0,      0,
    0,      576,    1152,   384,
    0,      1152,   1152,   384,
};

static const uint8_t mp3_padding_size[4] = { 0, 1,  1,  4 };

// 비디오 Configuration 정보
typedef struct _codec_config_block
{
    uint8_t         *data;
    uint32_t        size;
    
} __attribute__((packed, aligned(4))) codec_config_block;

typedef struct _mp4_track_desc
{
    uint32_t    ctts:1;
    uint32_t    stss:1;
    uint32_t    vfy:1;
    uint32_t    ls:4;       // nal size (video)
    uint32_t    sps:5;
    uint32_t    pps:5;
    uint32_t    lang:7;
    uint32_t    codec:8;
    
    uint32_t    tick;
    uint32_t    tscale;
    
    struct {
        uint32_t    size;
        uint32_t    tsc;
    } max;
    
    uint64_t    duration;
    
    codec_config_block cfgblock;
    
    struct {
        
        uint8_t     *chunk;
        uint16_t    size;
        uint16_t    cfgoff;
        uint16_t    cfgsize;
        
    } stsd;
    
    struct _mp4_track_desc  *next;
    
} __attribute__((packed, aligned(4))) mp4_track_desc;

#define MP4_TRACK_CHUNK     0
#define MP4_TRACK_VIDEO     (0x01 << 0)
#define MP4_TRACK_SOUND     (0x01 << 1)
#define MP4_TRACK_TEXT      (0x01 << 2)

#define MOFFSET32(x)    (*((uint32_t *)&((uint8_t *)x)[12]))
#define MOFFSET64(x)    (*((uint64_t *)&((uint8_t *)x)[12]))

#define DTS32(x)        (*((uint32_t *)&((uint8_t *)x)[16]))
#define DTS64(x)        (*((uint64_t *)&((uint8_t *)x)[20]))

#define PTS32(x)        (DTS32(x) == 0 ? 0L : DTS32(x) + (*((int32_t *)&((uint8_t *)x)[8])))
#define PTS64(x)        (DTS64(x) == 0 ? 0L : DTS64(x) + (int64_t)(*((int32_t *)&((uint8_t *)x)[8])))

typedef struct _frame_header
{
    uint32_t key:1;     // key 프레임이면 1, 비디오 프레임에 대해서만
    uint32_t pcr:1;
    uint32_t split:1;   // 분할된 프레임 (이 경우 frame32/64의 offset은 첫번째 파티션 슬롯의 offset)
    uint32_t track:3;   // 트랙 유형 (MP4_TRACK_CHUNK, MP4_TRACK_SOUND, MP4_TRACK_VIDEO, MP4_TRACK_TEXT)
    uint32_t idx:3;     // 트랙 인덱스 (0~)
    uint32_t size:23;   // 프레임 크기
    
    uint32_t tsc;    // TS Packet Count
    int32_t cts;     // PTS - DTS
    
} __attribute__((packed, aligned(4))) frame_header;
   
typedef struct _frame32
{
    frame_header        header;
    
    uint32_t            offset;
    uint32_t            dts;
    
} __attribute__((packed, aligned(4))) frame32; // 16 bytes

typedef struct _frame64
{
    frame_header        header;
    
    uint64_t            offset;
    uint64_t            dts;
    
} __attribute__((packed, aligned(4))) frame64; // 24 bytes

typedef struct _parse_frame_node
{
    uint32_t                sync:1;
    uint32_t                smpl:1;
    uint32_t                track:3;
    uint32_t                idx:3;
    uint32_t                size:24;
    
    uint32_t                pos;
    off_t                   offset;
    
    uint64_t                pts;
    uint64_t                dts;
    
    struct _parse_frame_node  *prev;
    struct _parse_frame_node  *next;
    struct _parse_frame_node  *mnext;
    
} parse_frame_node;

typedef struct _raps_table
{
    uint32_t    *entry;
    uint32_t    cc;
    
} raps_table;

enum {
    
    EICHUNK_NONE = 0,
    EICHUNK_ID3_V1,
    EICHUNK_ID3_V1EXT,
    EICHUNK_ID3_V2,
};
    
typedef struct _id3_chunk
{
    uint32_t type:4;
    uint32_t size:28;
    
    struct _id3_chunk *next;

    uint64_t offset;
    
} id3_chunk;
    
typedef struct _mp4_box_range
{
    off_t       offset;
    off_t       size;
    
} mp4_box_range;

typedef struct _mp4_mdia_attr
{
    uint32_t    track:2;
    uint32_t    idx:4;
    uint32_t    lang:8;
    uint32_t    resv:18;
    
    uint32_t    tscale;
    uint64_t    duration;
    
    uint32_t    lctype;
    uint32_t    stco;
    uint32_t    co64;
    uint32_t    stsc;
    uint32_t    stsz;
    uint32_t    stts;
    uint32_t    ctts;
    uint32_t    stss;
    
} mp4_mdia_attr;

typedef struct _zcxt_parse_param
{
    void *mempool;
    
    struct {
        
        off_t   audio[MULTI_TRACK_MAX];
        off_t   video[MULTI_TRACK_MAX];
        
    } chunksize;
    
    parse_frame_node  *fn_head;
    parse_frame_node  *fn_tail;
    
    mp4_track_desc  *trkdesc;
    
    union {
        
        mp4_mdia_attr   mp4;
        
    } stat;
     
} zctx_parse_param;

// mp4 파싱 정보
typedef struct _zidx_context
{
    uint32_t    interleaved:1;      // 가상 interleaved 여부
    uint32_t    bit64:1;
    uint32_t    fcc:30;             // frame_array의 크기(갯수)
    
    struct {
        
        struct {
            
            struct {
                
                uint64_t    created;    // 생성시간 (초, 1.1.1904 ~)
                uint64_t    modified;   // 최종 수정 시간(초, 1.1.1904 ~)
                
            } date;
            
        } global;
        
        mp4_track_desc  *audio;
        mp4_track_desc  *video;
        
    } config;
    
    union {
        
        frame32     *f32;       // 프레임 정보들 (기본형)
        frame64     *f64;       // 프레임 정보들 (확장형, 가상 interleaved 지원용)
        
    } frame_array;
    
    struct {
        
        uint32_t foot:1;
        uint32_t size:31;
        
        id3_chunk *head;
        id3_chunk *tail;
        
    } id3;
    
    raps_table *rt; // RAP(Random Access Point) index 정보들
    
    struct {
        
        off_t fo;
        
        struct {
        
            uint32_t indexed:1;
            uint32_t arranged:1;
            uint32_t ref:1;
            uint32_t ready:1;
            uint32_t offset:28;
            uint32_t size;
            uint8_t *p;
            
            struct {
                
                off_t offset;
                size_t size;
                
            } target;
            
        } buf;
                
        zctx_parse_param *param;
        
    } parse;
    
} zidx_context;

typedef struct _mp4_box_h
{
    uint32_t size;
    uint32_t type;
    
}  __attribute__((packed, aligned(4))) mp4_box_h;

typedef struct _mp4_verflag_h
{
    uint32_t    ver:8;
    uint32_t    flag:24;
    
}  __attribute__((packed, aligned(4))) mp4_verflag_h;

typedef struct _mp4_fullbox_h
{
    mp4_box_h   box;

    uint32_t    ver:8;
    uint32_t    flag:24;

} __attribute__((packed, aligned(4))) mp4_fullbox_h;

#define STSD_MP4A                       0x6134706d
#define STSD_AC3                        0x332D6361
#define STSD_EC3                        0x332D6365
#define STSD_AVC1                       0x31637661
#define STSD_MP4V                       0x7634706d
#define STSD_HEVC                       0x31766568
#define STSD_HVC1                       0x31637668
    
#define BOX_FULL_READING(x) ((x) - sizeof(mp4_box_h))

#define INTERLEAVED_CUTLINE 270000LL

// build

static const uint8_t chunk_mdat[] = {           0x00, 0x00, 0x00, 0x00, 0x6D, 0x64, 0x61, 0x74 };
    
static const uint8_t chunk_ftyp[] = {           0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70, 0x69, 0x73, 0x6F, 0x6D, 0x00, 0x00, 0x00, 0x00, 0x69, 0x73, 0x6F, 0x6D, 0x6D, 0x70, 0x34, 0x32 };   // isom, 0, isom mp42
static const uint8_t chunk_ftyp_dash[] = {      0x00, 0x00, 0x00, 0x1C, 0x66, 0x74, 0x79, 0x70, 0x69, 0x73, 0x6F, 0x6D, 0x00, 0x00, 0x00, 0x00, 0x69, 0x73, 0x6F, 0x6D, 0x6D, 0x70, 0x34, 0x32, 0x64, 0x61, 0x73, 0x68  };   // isom, 0, isom mp42 dash
static const uint8_t chunk_styp[] = {           0x00, 0x00, 0x00, 0x18, 0x73, 0x74, 0x79, 0x70, 0x6D, 0x73, 0x64, 0x68, 0x00, 0x00, 0x00, 0x00, 0x6D, 0x73, 0x64, 0x68, 0x6D, 0x73, 0x69, 0x78 };   // msdh, 0, msdh, msix
static const uint8_t chunk_sisx[] = {           0x00, 0x00, 0x00, 0x18, 0x73, 0x74, 0x79, 0x70, 0x6D, 0x73, 0x64, 0x68, 0x00, 0x00, 0x00, 0x00, 0x6D, 0x73, 0x64, 0x68, 0x73, 0x69, 0x73, 0x78 };   // msdh, 0, msdh, sisx

static const uint8_t chunk_mvhd_foot[] = {  0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static const uint8_t chunk_tkhd_foot[] = {  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00 }; // 오디오이면 tkhd_foot[12] = 1, 그렇지 않으면 0

static const uint8_t chunk_hdlr_vide[] = {  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0x69, 0x64, 0x65, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x69, 0x64, 0x65, 0x6F, 0x48, 0x61, 0x6E,
    0x64, 0x6C, 0x65, 0x72, 0x00 };

static const uint8_t chunk_hdlr_soun[] = {  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x6F, 0x75, 0x6E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x6F, 0x75, 0x6E, 0x64, 0x48, 0x61, 0x6E,
    0x64, 0x6C, 0x65, 0x72, 0x00 };

static const uint8_t chunk_vmhd[] = {   0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t chunk_smhd[] = {   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static const uint8_t chunk_dinf[] = {   0x00, 0x00, 0x00, 0x1C, 0x64, 0x72, 0x65, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x0C, 0x75, 0x72, 0x6C, 0x20, 0x00, 0x00, 0x00, 0x01 }; // dinf-dref-url


static const uint8_t chunk_mp3_stsd[] = {   0x00, 0x00, 0x00, 0x00, // ver, flag(0,0)
    0x00, 0x00, 0x00, 0x01,	// entry count = 1 (fixed)
    0x00, 0x00, 0x00, 0x50, 0x6D, 0x70, 0x34, 0x61,	// mp4a
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // sampleEntry (fixed)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // reserved (fixed)
    0x00, 0x00, // channel count, 32
    0x00, 0x10,	// sample size(16)
    0xFF, 0xFE, 0x00, 0x00, // reserved
    0x00, 0x00,  // sample rate << 16, 40
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x2C, 0x65, 0x73, 0x64, 0x73,	// esds
    0x00, 0x00, 0x00, 0x00, // ver, flag(0,0)
    0x03, 0x80, 0x80, 0x80, // es_id(3)
    0x1B,
    0x00, 0x02, 0x1F,
    0x04, 0x80, 0x80, 0x80, // es_id(4)
    0x0D,
    0x6B, // MPEG-1 ADTS
    0x15,
    0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, // max bitrate, 74
    0x00, 0x00, 0x00, 0x00,	// avg bitrate, 78
    
};
    
static const uint8_t chunk_mp4v_stsd[] = { 0x00, 0x00, 0x00, 0x00, // ver, flag(0,0)
    
    0x00, 0x00, 0x00, 0x01,	// entry count = 1 (fixed)
    0x00, 0x00, 0x00, 0x50, 0x6D, 0x70, 0x34, 0x76,	// mp4V
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // sampleEntry (fixed)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // reserved (fixed)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // reserved (fixed)
    0x00, 0x00, // width, 40
    0x00, 0x10,	// height
    0x00, 0x48, 0x00, 0x00,
    0x00, 0x48, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x18, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x2C, // 94
    0x65, 0x73, 0x64, 0x73,	// esds
    0x00, 0x00, 0x00, 0x00, // ver, flag(0,0)
    0x03, 0x80, 0x80, 0x80, // es_id(3)
    0x1B, // remain length, 끝까지, 110
    0x00, 0x02, // ES ID(?)
    0x1F, // priority
    0x04, 0x80, 0x80, 0x80, // es_id(4)
    0x0D, // es_id(4) length, 05까지, 118
    0x20, // mpeg4 video
    0x11,
    0x10, 0x00, 0x00, // buffer size (65536),121
    0x00, 0x00, 0x00, 0x00, // max bitrate, 74, 124
    0x00, 0x00, 0x00, 0x00,	// avg bitrate, 78, 128
    0x05, 0x80, 0x80, 0x80, // es_id(5, decoder specific descriptor)
    0x01, // mp4v configuration size, 136
};
    
static const uint8_t chunk_aac_stsd[] = {   0x00, 0x00, 0x00, 0x00, // ver, flag(0,0)
    0x00, 0x00, 0x00, 0x01,	// entry count = 1 (fixed)
    0x00, 0x00, 0x00, 0x50, 0x6D, 0x70, 0x34, 0x61,	// mp4a
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // sampleEntry (fixed)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // reserved (fixed)
    0x00, 0x00, // channel count, 32
    0x00, 0x10,	// sample size(16)
    0x00, 0x00, 0x00, 0x00, // reserved
    0x00, 0x00,  // sample rate << 16, 40
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x2C,
    0x65, 0x73, 0x64, 0x73,	// esds
    0x00, 0x00, 0x00, 0x00, // ver, flag(0,0)
    0x03, 0x80, 0x80, 0x80, // es_id(3)
    0x1B, // remain length, 60
    0x00, 0x02, // ES ID(?)
    0x00, // priority
    0x04, 0x80, 0x80, 0x80, // es_id(4)
    0x0D, // es_id(4) length, 68
    0x40, // mpeg4 audio
    0x15,
    0x01, 0xE8, 0x48, // buffer size (125000)
    0x00, 0x00, 0x00, 0x00, // max bitrate, 74
    0x00, 0x00, 0x00, 0x00,	// avg bitrate, 78
    0x05, 0x80, 0x80, 0x80, // es_id(5, decoder specific descriptor)
    0x01, // aac config block size
};

static const uint8_t chunk_avc_stsd[] = {   0x00, 0x00, 0x00, 0x00, // ver, flag(0,0)
    0x00, 0x00, 0x00, 0x01,	// entry count = 1 (fixed)
    0x00, 0x00, 0x00, 0x86,
    0x61, 0x76, 0x63, 0x31, // avc1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // sampleEntry (fixed)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, // width, 40
    0x00, 0x00, // height, 42
    0x00, 0x48, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x18, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x30, // 94
    0x61, 0x76, 0x63, 0x43, // avcC
    0x01,
    0x00, 0x00, 0x00, // profile, compatibility, level, 103
    0xFF, // length minus one & 0xFC, 106
    0xE1,
    0x00, 0x00, // sps size, 108
};

static const uint8_t chunk_mp4_esds_foot[] = {
    
    0x06, 0x80, 0x80, 0x80, // es_id(6, SL Config descriptor)
    0x01,
    0x02
};

static const uint8_t chunk_trex[] = {
    0x00, 0x00, 0x00, 0x20, 0x74, 0x72, 0x65, 0x78,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };


static const uint8_t chunk_idx_void[] = {
    0x00, 0x00, 0x00, 0x14, 0x73, 0x74, 0x73, 0x7A,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x73, 0x74, 0x73, 0x63,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x73, 0x74, 0x74, 0x73,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x73, 0x74, 0x63, 0x6F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };


typedef struct _stsd_box_audio_header
{
    mp4_fullbox_h box;
    uint32_t    entry;
    
    mp4_box_h  cbox; // mp4a
    
    uint16_t        resv[3];
    uint16_t        count;   // 1
    
    uint32_t        resv2[2];
    uint16_t        channel;
    uint16_t        smplsize;
    uint32_t        resv3;
    uint16_t        freq;
    uint16_t        resv4;
    
} __attribute__((packed, aligned(4))) stsd_box_audio_header;

typedef struct _stsd_box_video_header
{
    mp4_fullbox_h box;
    uint32_t    entry;
    
    mp4_box_h  cbox; // mp4v, avc1, hvc1
    
    uint16_t        resv[3];
    uint16_t        count;      // 1
    
    uint32_t        resv2[4];
    
    
    uint16_t        width;
    uint16_t        height;
    
    uint32_t        hres;   // 0x00480000
    uint32_t        vres;   // 0x00480000
    
    uint32_t        resv3;
    uint16_t        fcount; // 1
    
    uint32_t        resv4[8];
    
    uint16_t        depth; // 0x0018
    uint16_t        resv5; // 0xFFFF
    
} __attribute__((packed, aligned(2))) stsd_box_video_header;
    
static const uint8_t esds_cfg_tag_chunk[] = { 0x06, 0x80, 0x80, 0x80, 0x01, 0x02 };

typedef struct _mp4_esds_tag
{
    uint8_t tag;
    uint8_t padding[3];
    uint8_t remain;
    
} __attribute__((packed, aligned(1))) mp4_esds_tag;

typedef struct _esds_box_header
{
    mp4_fullbox_h box; // esds
    
    struct {
        
        mp4_esds_tag tag; // 0x03
        
        uint16_t esid;      // 2
        uint8_t priority;   // 5bit (0x1f = max);
        
    } __attribute__((packed, aligned(1))) desc_tag;
    
    struct {
        
        mp4_esds_tag tag; // 0x04
        
        uint8_t strm; // aac = 0x40, mp3 = 0x6b
        
        uint32_t stype:8; // audio = 0x15, video = 0x11
        uint32_t bsize:24; // audio = 0x10000, video = 0x100000
        
        uint32_t maxbps;
        uint32_t avgbps;
        
    } __attribute__((packed, aligned(1))) dec_desc_tag;
    
} __attribute__((packed, aligned(1))) esds_box_header;
    
typedef struct _avcc_box_header
{
    mp4_box_h box;
    
    uint8_t cfgver; // 1
    uint8_t profile;
    uint8_t compatibility;
    uint8_t level;
    
    uint8_t hlen; // 0xFF (0xFC | (length - 1))
    
} __attribute__((packed, aligned(1))) avcc_box_header;

typedef struct _avcc_ps_header
{
    uint8_t count; // 0xE1 for SPS, 0x01 for PPS
    uint16_t length;
    
} __attribute__((packed, aligned(1))) avcc_ps_header;
    
typedef struct _hvcc_box_header
{
    mp4_box_h box;
    
    uint8_t ver; // 1
    uint8_t profile;
    uint8_t compatibility_flag[4];
    uint8_t indicator_flag[6];
    
    uint8_t level;
    
    uint16_t spatial_segmentation; // (0 | 0xF000)
    uint8_t parallelism_type; // (0 | 0xFc)
    
    uint8_t chroma; // ( | 0xFc)
    uint8_t depth_luma; // ( | 0xF8)
    uint8_t depth_chroma; // ( | 0xF8)
    
    uint16_t avgfps; // 0, undefined
    uint8_t ls; // 0x0F
    
    uint8_t nals; // 3 (VPS, SPS, PPS)
    
} __attribute__((packed, aligned(1))) hvcc_box_header;

typedef struct _hvcc_ps_header
{
    uint8_t     naltype;
    uint16_t    nals;
    uint16_t    length;
    
} __attribute__((packed, aligned(1))) hvcc_ps_header;
    
typedef struct _mvhd_box_header
{
    uint32_t    ver:8;
    uint32_t    flag:24;
    
    uint32_t    ctime;
    uint32_t    mtime;
    
    uint32_t    tscale;
    uint32_t    duration;
    
} __attribute__((packed, aligned(4))) mvhd_box_header;

typedef struct _mvhd_box_header64
{
    uint32_t    ver:8;
    uint32_t    flag:24;
    
    uint64_t    ctime;
    uint64_t    mtime;
    
    uint32_t    tscale;
    uint64_t    duration;
    
} __attribute__((packed, aligned(4))) mvhd_box_header64;

typedef struct _tkhd_box_header
{
    uint32_t    ver:8;
    uint32_t    flag:24;
    
    uint32_t    ctime;
    uint32_t    mtime;
    uint32_t    trackid;
    uint32_t    resv;
    uint32_t    duration;
    
} __attribute__((packed, aligned(4))) tkhd_box_header;

typedef struct _tkhd_box_header64
{
    uint32_t    ver:8;
    uint32_t    flag:24;
    
    uint64_t    ctime;
    uint64_t    mtime;
    uint32_t    trackid;
    uint32_t    resv;
    uint64_t    duration;
    
} __attribute__((packed, aligned(4))) tkhd_box_header64;

typedef struct _stsc_entry
{
    uint32_t    first;
    uint32_t    samples;
    uint32_t    sdi;        // sample description index
    
} __attribute__((packed, aligned(4))) stsc_entry;

typedef struct _sidx_moof
{
    uint32_t    moof_size;
    uint32_t    duration;
    uint32_t    sap_flag;   // v:0x00000090, a:0x00000000
    
} __attribute__((packed, aligned(4))) sidx_moof;
    
typedef struct _sidx_box_header
{
    uint32_t    ver:8;      // 0
    uint32_t    flag:24;
    
    uint32_t    track_id;
    uint32_t    timescale;
    
    uint32_t    start_time;
    uint32_t    offset;
    
    uint32_t    count;
    
} __attribute__((packed, aligned(4))) sidx_box_header;

typedef struct _sidx_box_header64
{
    uint32_t    ver:8;      // 1
    uint32_t    flag:24;
    
    uint32_t    track_id;
    uint32_t    timescale;
    
    uint64_t    start_time;
    uint64_t    offset;
    
    uint32_t    count;
    
} __attribute__((packed, aligned(4))) sidx_box_header64;

typedef struct _mfhd_box_header
{
    uint32_t    ver:8;      // 1
    uint32_t    flag:24;
    
    uint32_t    seq;        // 1~
    
} __attribute__((packed, aligned(4))) mfhd_box_header;

typedef struct _tfhd_box_video_header
{
    uint32_t    ver:8;      // 0
    uint32_t    flag:24;    // 0
    
    uint32_t    track_id;
    
} __attribute__((packed, aligned(4))) tfhd_box_video_header;

typedef struct _tfhd_box_audio_header
{
    uint32_t    ver:8;      // 0
    uint32_t    flag:24;    // 0x200000
    
    uint32_t    track_id;
    uint32_t    smplflag;   // 0
    
} __attribute__((packed, aligned(4))) tfhd_box_audio_header;

typedef struct _tfdt_box_header
{
    uint32_t    ver:8;          // 0
    uint32_t    flag:24;
    
    uint32_t    start_dts;
    
} __attribute__((packed, aligned(4))) tfdt_box_header;

typedef struct _tfdt_box_header64
{
    uint32_t    ver:8;          // 1
    uint32_t    flag:24;
    
    uint64_t    start_dts;
    
} __attribute__((packed, aligned(4))) tfdt_box_header64;

typedef struct _trun_box_video_header
{
    uint32_t    ver:8;          // 0
    uint32_t    flag:24;        // 0x010f00
    
    uint32_t    count;
    int32_t     offset;
    
} __attribute__((packed, aligned(4))) trun_box_video_header;

typedef struct _trun_box_audio_header
{
    uint32_t    ver:8;          // 0
    uint32_t    flag:24;        // 0x010300
    
    uint32_t    count;
    int32_t     offset;
    
} __attribute__((packed, aligned(4))) trun_box_audio_header;

typedef struct _trun_box_audio_entry
{
    uint32_t    duration;
    uint32_t    size;
    
} __attribute__((packed, aligned(4))) trun_box_audio_entry;

typedef struct _trun_box_video_entry
{
    uint32_t    duration;
    uint32_t    size;
    uint32_t    flag;
    uint32_t    cts;
    
} __attribute__((packed, aligned(4))) trun_box_video_entry;

typedef struct _aud_cfg
{
    uint32_t     ch:4;
    uint32_t     obj:8;
    uint32_t    freq:20;
        
} aud_cfg;
   
int get_audio_config(uint8_t *block, uint32_t size, aud_cfg *cfg, uint8_t codec);
uint32_t put_mp4_box_header(uint32_t size, uint32_t type, uint8_t *buf);
uint32_t put_mp4_fullbox_header(uint32_t size, uint32_t type, uint8_t *buf, uint32_t ver, uint32_t flag);
uint32_t get_nal_size(uint32_t nal_size, uint8_t nal_size_length, uint32_t offset, uint32_t size);
uint32_t get_bits(uint8_t *buf, int start, int count);
    
// DRM Provider Desc.
    

    
static const uint8_t chunk_cenc_sinf[] = {
    
    // sinf
    0x00, 0x00, 0x00, 0x50, 0x73, 0x69, 0x6E, 0x66,
    
    // frma
    0x00, 0x00, 0x00, 0x0C, 0x66, 0x72, 0x6D, 0x61,
    0x61, 0x76, 0x63, 0x31, // 16 (original box: mp4v, mp4a, avc1)
    
    // schm
    0x00, 0x00, 0x00, 0x14, 0x73, 0x63, 0x68, 0x6D,
    0x00, 0x00, 0x00, 0x00, 0x63, 0x65, 0x6E, 0x63,
    0x00, 0x01, 0x00, 0x00,

    // schi
    0x00, 0x00, 0x00, 0x28, 0x73, 0x63, 0x68, 0x69,
    
    // tenc
    0x00, 0x00, 0x00, 0x20, 0x74, 0x65, 0x6E, 0x63,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, // protected(1)
    0x08, // IV Size = 8
    0x02, 0x94, 0xB9, 0x59, 0x9D, 0x75, 0x5D, 0xE2, 0xBB, 0xF0, 0xFD, 0xCA, 0x3F, 0xA5, 0xEA, 0xB7 // 64 (key id)
    
};
    
typedef struct _saiz_box_header
{
    uint32_t    ver:8;          // 0
    uint32_t    flag:24;        // 1
    
    uint32_t    type;           // 'cenc'
    uint32_t    param;          // 0
    
    uint8_t     defsize;        // 0
    uint32_t    count;          //
    
} __attribute__((packed, aligned(1))) saiz_box_header;
    
typedef struct _senc_box_header
{
    uint32_t    ver:8;          // 0
    uint32_t    flag:24;        // 2
    
    uint32_t    count;
    
} __attribute__((packed, aligned(4))) senc_box_header;

typedef struct _saio_box_header
{
    uint32_t    ver:8;          // 0
    uint32_t    flag:24;        // 1
    
    uint32_t    type;           // 'cenc'
    uint32_t    param;          // 0

    uint32_t    count;          // 1
    uint32_t    offset;         // from (offset of moof) to (end of senc_box_header)
    
} __attribute__((packed, aligned(4))) saio_box_header;
    
typedef struct _senc_box_entry
{
    uint8_t iv[8];
    
    uint16_t count; // 1;
    uint16_t clear;
    uint32_t encrypted;
    
} __attribute__((packed, aligned(4))) senc_box_entry;
    
static const uint8_t cencSystemId_ClearKey[16] = { 0x10, 0x77, 0xEF, 0xEC, 0xC0, 0xB2, 0x4D, 0x02, 0xAC, 0xE3, 0x3C, 0x1E, 0x52, 0xE2, 0xFB, 0x4B };
static const uint8_t cencSystemId_PlayReady[16] = { 0x9A, 0x04, 0xF0, 0x79, 0x98, 0x40, 0x42, 0x86, 0xAB, 0x92, 0xE6, 0x5B, 0xE0, 0x88, 0x5F, 0x95 };
static const uint8_t cencSystemId_Widevine[16] = { 0xED, 0xEF, 0x8B, 0xA9, 0x79, 0xD6, 0x4A, 0xCE, 0xA3, 0xC8, 0x27, 0xDC, 0xD5, 0x1D, 0x21, 0xED };
static const uint8_t cencSystemId_FairPlay[16] = { 0x29, 0x70, 0x1F, 0xE4, 0x3C, 0xC7, 0x4A, 0x34, 0x8C, 0x5B, 0xAE, 0x90, 0xC7, 0x43, 0x9A, 0x47 };
    
    
static const char cencSystemGuid_ClearKey[] =   "1077efec-c0b2-4d02-ace3-3c1e52e2fb4b";
static const char cencSystemGuid_PlayReady[] =  "9a04f079-9840-4286-ab92-e65be0885f95";
static const char cencSystemGuid_Widevine[] =   "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
static const char cencSystemGuid_FairPlay[] =   "29701fe4-3cc7-4a34-8c5b-ae90c7439a47";
    
typedef struct _drm_playready_header
{
    uint32_t size;
    uint16_t count; // 0x01
    
    struct {
        
        uint16_t type;  // 0x01
        uint16_t length;
        
    } record;
        
}  __attribute__((packed, aligned(2))) drm_playready_header;
   
typedef struct _drm_playready_guid
{
    uint32_t    a;
    uint16_t    b;
    uint16_t    c;
    uint8_t     d[8];
        
} __attribute__((packed, aligned(2))) drm_playready_guid;

static const char playready_record_xml[] =
    "<WRMHEADER xmlns=\"http://schemas.microsoft.com/DRM/2007/03/PlayReadyHeader\" version=\"4.0.0.0\">"
    "<DATA><PROTECTINFO><KEYLEN>16</KEYLEN><ALGID>AESCTR</ALGID></PROTECTINFO>"
    "<KID>%s</KID><LA_URL>%s</LA_URL></DATA></WRMHEADER>";


#ifdef __cplusplus
}
#endif
    
#endif
