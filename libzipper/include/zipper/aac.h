
#ifndef zipper_aac_h
#define zipper_aac_h

#include <stdint.h>

typedef struct __parse_aac_frame_param_t
{
    uint8_t     *data;
    uint32_t    offset;
    uint32_t    length;
    uint32_t    dsize;
    
    struct {
        
        struct {
            
            uint8_t cfg;
            uint8_t src;
            
        } frequency;
        
        uint8_t sbr_detected:1;
        uint8_t ps_detected:1;
        uint8_t resv:6;
        
    } cfg;
    
    struct {
        
        uint8_t common_window:1;
        uint8_t scale_factor_grouping:7;
        
        uint8_t window_sequence:2;
        uint8_t max_sfb:6;
        
        uint8_t num_windows:4;
        uint8_t num_window_groups:4;
        
        uint8_t num_swb;
        
        uint8_t window_group_length[8];
        uint8_t sfb_cb[8][64];
        uint16_t swb_offset[64];
        uint16_t sect_sfb_offset[8][1024];
        
        uint8_t num_sec[8];
        uint8_t sect_cb[8][64];
        uint16_t sect_start[8][64];
        uint16_t sect_end[8][64];
        
        struct {
            
            uint8_t bs_amp_res_fromheader:1;
            uint8_t bs_freq_scale:2;
            uint8_t bs_noise_bands:2;
            uint8_t bs_xover_band:3;
            
            uint8_t bs_start_freq:4;
            uint8_t bs_stop_freq:4;
            
            uint8_t bs_alter_scale:1;
            uint8_t num_noise_bands:3;
            uint8_t resv:4;
            
            uint8_t bs_num_env[2];
            uint8_t num_env_bands[2];
            uint8_t bs_num_noise[2];
            int8_t  bs_amp_res[2];
            uint8_t bs_freq_res[2][8];
            int8_t  bs_df_env[2][8];
            int8_t  bs_df_noise[2][8];
            
        } sbr;
        
    } aacflag;
    
} parse_aac_frame_param;

char parse_aac_frame(parse_aac_frame_param *prm);
uint8_t aac_program_config_element(parse_aac_frame_param *prm);

#endif
