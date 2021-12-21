//
//  ts.h
//  zipper
//
//  Created by voxio on 2014. 9. 17..
//  Copyright (c) 2014년 SolBox Inc. All rights reserved.
//

#ifndef zipper_ts_h
#define zipper_ts_h

#include "mp4.h"
#include "io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TS_CLOCK_RATE                   90000LL
#define PTS_PADDING                     3003LL

#define ADTS_SYNC_HEADER                0xFFF0
#define ADTS_HEADER_SIZE                7

#define NAL_TYPE_NONEIDR_PICTURE        1
#define NAL_TYPE_IDR_PICTURE            5
#define NAL_TYPE_SEI                    6
#define NAL_TYPE_SPS                    7
#define NAL_TYPE_PPS                    8
#define NAL_TYPE_AU                     9
#define NAL_TYPE_ENDSEQ                 10
#define NAL_TYPE_ENDSTRM                11

#define NAL_TYPE_VPS_HEVC               32
#define NAL_TYPE_SPS_HEVC               33
#define NAL_TYPE_PPS_HEVC               34
#define NAL_TYPE_AU_HEVC                35
#define NAL_TYPE_ENDSEQ_HEVC            36
#define NAL_TYPE_ENDSTRM_HEVC           37
    
#define NAL_TYPE_IDR_W_RADL_HEVC        19
#define NAL_TYPE_IDR_N_LP_HEVC          20
#define NAL_TYPE_SLICE_CRA_HEVC         21

#define GTS(x,y)                        (uint64_t)(((double)(x * TS_CLOCK_RATE) / (double)y + 0.5))

#define PCR_OUT_INTERVAL                5400LL
#define TS_PACKET_SIZE                  188
#define TS_PACKET_CAPA                  184
#define TS_HEADER_SIZE                  4
#define SEGMENT_TS_OUTBUF_SIZE          1504
#define SEGMENT_TS_ALLOC_SIZE           (SEGMENT_TS_OUTBUF_SIZE + 16)
#define PAT_AF_SIZE                     167
#define PATPMT_SIZE                     376
#define PES_VIDEO_HSIZE                 19
#define PES_AUDIO_HSIZE                 14
#define PASS_SIZE                       17
#define PASS_LEN                        13
#define DESC_SIZE_BASE                  21
#define PED_BASESIZE                    5
#define TSH_COUNTER_OFFSET              3
#define PES_FRONT_HEADER                6

#define TS_PREFIX_SYNCCODE              0x47

#define PAF_FLAG_PCR                    (0x01 << 0)
#define PAF_FLAG_DISCON                 (0x01 << 1)
#define PAF_FLAG_RAP                    (0x01 << 2)

#define AUDIO_STREAM_ID                 0xC0
#define VIDEO_STREAM_ID                 0xE0

#define REGISTRATION_DESCRIPTOR_TAG     0x05
#define PRIVATE_DESCRIPTOR_TAG          0x0F
    
#define ADAPTIVE_ONLY_FLAG              0x30
#define SCEP_PADDING                    64

static const uint16_t ts_pid[5] = { 80, 82, 81, 0, 83 };
static const uint8_t ts_sid[5] = { 0x00, 0xE0, 0xC0, 0, 0xFF };

static const uint8_t au_nal_unit[6] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xe0 };
static const uint8_t filler_nal_unit[5] = { 0x00, 0x00, 0x00, 0x01, 0x0c };
static const uint8_t nal_size_bs[4] = { 0x00, 0x00, 0x00, 0x01 };
static const uint8_t nal_ep[3] = { 0x00, 0x00, 0x03 };
    
static const uint8_t hevc_au_nal[7] = { 0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x50 };
    
uint8_t get_frequency_index(uint32_t rate);
void apply_size_adts_header(uint32_t size, uint8_t *adts);

uint32_t calculate_crc(uint8_t *buf, uint16_t len);
uint16_t es_descriptor_size(uint8_t descid);
uint16_t es_descriptor(uint8_t descid, uint8_t *buf, uint8_t *desc, int desc_size);
void mux_pcr_timestamp(uint64_t clock, uint8_t *buf);
void mux_mpeg2ts_timestamp(int type, uint64_t t, uint8_t *out);
uint16_t ts_header(uint8_t *buf, uint16_t tsid, uint8_t counter, uint8_t pes);
uint16_t adaptation_field(uint8_t *buf, uint8_t size, uint8_t flag, uint64_t pcr);
uint16_t pes_header(uint8_t *buf, uint8_t sid, uint16_t len, uint64_t dts, uint64_t pts);
uint32_t count_ts_packet(frame_header *frame, mp4_track_desc *trkdesc);
    
#ifdef __cplusplus
}
#endif
    
#endif
