//
//  hevc.h
//  zipper
//
//  Created by Hyungpyo Oh on 2018. 8. 7..
//  Copyright © 2018년 Voxio Dev. All rights reserved.
//

#ifndef hevc_h
#define hevc_h

#include <stdint.h>

#define MAX_SUB_LAYERS 7
#define hevc_nal_type(x)    ((x >> 1) & 0x3F)

typedef struct _hevc_sps
{
    uint32_t sps_id;
    
    struct {
        
        uint8_t general_profile_space;
        uint8_t general_tier_flag;
        uint8_t general_profile_idc;
        uint8_t general_level_idc;
        
        uint8_t compatibility_flag[4];
        uint8_t indicator_flag[6];
        
        uint8_t sub_layer_profile_present_flag[MAX_SUB_LAYERS];
        uint8_t sub_layer_level_present_flag[MAX_SUB_LAYERS];
        
    } ptl;
    
    uint8_t max_sub_layers:4;
    uint8_t chroma_format_idc:4;
    
    uint16_t width;
    uint16_t height;
    
    struct {
        
        uint16_t left;
        uint16_t right;
        uint16_t top;
        uint16_t bottom;
        uint16_t unit;
        
    } crop;
    
    uint8_t bit_depth_luma;
    uint8_t bit_depth_chroma;
    
    // added 2018.4.22
    
    uint8_t max_pic_order_cnt;
    uint8_t num_short_term_ref_pic_sets;
    
    uint8_t aspect_ratio_idc;
    uint16_t sar_width;
    uint16_t sar_height;
    
    uint8_t overscan_appropriate;
    uint8_t video_format;
    uint8_t colour_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coefficients;
    
} hevc_sps;

void read_hevc_sps(hevc_sps *sps, void *data, int size);

#endif /* hevc_h */
