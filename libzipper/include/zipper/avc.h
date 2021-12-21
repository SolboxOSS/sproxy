//
//  avc.h
//  zipper
//
//  Created by Hyungpyo Oh on 2018. 8. 7..
//  Copyright © 2018년 Voxio Dev. All rights reserved.
//

#ifndef avc_h
#define avc_h

#include <stdint.h>

#define avc_nal_type(x)     (x & 0x1F)

typedef struct
{
    uint8_t* start;
    uint8_t* p;
    uint8_t* end;
    int bits_left;
    
} bs_t;

typedef struct _avc_sps
{
    uint8_t profile_idc;
    uint8_t constraint_set0_flag:1;
    uint8_t constraint_set1_flag:1;
    uint8_t constraint_set2_flag:1;
    uint8_t constraint_set3_flag:1;
    uint8_t gaps_in_frame_num_value_allowed_flag:4;
    uint8_t frame_mbs_only_flag:4;
    uint8_t mb_adaptive_frame_field_flag:4;
    uint8_t direct_8x8_inference_flag:4;
    
    uint8_t level_idc;
    
    uint32_t seq_parameter_set_id;
    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma;
    uint32_t bit_depth_chroma;
    
    int ScalingList4x4[6][16];
    int UseDefaultScalingMatrix4x4Flag[6];
    int ScalingList8x8[2][64];
    int UseDefaultScalingMatrix8x8Flag[2];
    
    uint32_t log2_max_frame_num_minus4;
    uint32_t pic_order_cnt_type;
    
    uint32_t num_ref_frames;
    uint16_t pic_width_in_mbs_minus1;
    uint16_t pic_height_in_map_units_minus1;
    
    
    uint16_t frame_crop_left_offset;
    uint16_t frame_crop_right_offset;
    uint16_t frame_crop_top_offset;
    uint16_t frame_crop_bottom_offset;
    
    struct {
        
        uint8_t aspect_ratio_idc;
        uint16_t sar_width;
        uint16_t sar_height;
        
        uint8_t overscan_appropriate;
        uint8_t video_format;
        uint8_t colour_primaries;
        uint8_t transfer_characteristics;
        uint8_t matrix_coefficients;
        
    } vui;
    
} avc_sps;

static inline uint32_t bs_eof(bs_t* b)
{
    if (b->p >= b->end) { return 1; } else { return 0; }
}

static inline uint32_t bs_read_u1(bs_t* b)
{
    uint32_t r = 0;
    if (bs_eof(b)) { return 0; }
    
    b->bits_left--;
    r = ((*(b->p)) >> b->bits_left) & 0x01;
    
    if (b->bits_left == 0) {
        
        b->p++;
        b->bits_left = 8;
        
        if(b->p - b->start > 1) {
            
            if(*(b->p) == 0x03 &&
               *(b->p - 1) == 0x00 &&
               *(b->p - 2) == 0x00) {
                
                b->p++;
            }
        }
    }
    
    return r;
}

uint32_t bs_read_u(bs_t* b, int n);
void bs_init(bs_t* b, uint8_t* buf, int size);
uint32_t bs_read_f(bs_t* b, int n);
uint32_t bs_read_u8(bs_t* b);
uint32_t bs_read_ue(bs_t* b);
int32_t bs_read_se(bs_t* b);

void read_avc_sps(avc_sps *sps, void *data, int size);
void get_avc_resolution(avc_sps *sps, uint16_t *width, uint16_t *height);

#endif /* avc_h */
