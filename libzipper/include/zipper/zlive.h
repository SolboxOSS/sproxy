//
//  zlive.h
//  zipper
//
//  Created by Hyungpyo Oh on 2018. 7. 12..
//  Copyright © 2018년 Voxio Dev. All rights reserved.
//

#ifndef zlive_h
#define zlive_h

#include "io.h"
#include "zipper.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef enum {
        
        zlM3u8TagNone = 0,
        zlM3u8TagM3u,
        zlM3u8TagExtInf,
        zlM3u8TagVersion,
        zlM3u8TagMediaSeq,
        zlM3u8TagTargetDuration,
        zlM3u8TagType,
        zlM3u8TagKey,
        zlM3u8TagStreamInf,
        zlM3u8TagMedia,
        zlM3u8TagDiscon,
        zlM3u8TagEndList,
        zlM3u8TagUnknown,
        
    } zlive_m3u8_tag_type;

    typedef struct _zstr
    {
        const char *val;
        size_t len;
        
    } zstr;
    
#define ZURL_MAX_PARAM  10
    
    typedef struct _zurl
    {
        zstr src;       // 원본 문자열

        zstr scheme;    // 프로토콜
        zstr host;      // 호스트
        zstr path;      // 경로
        zstr file;      // 파일명
        
        struct {
            
            zstr name;  // 파라미터 이름
            zstr value; // 파리미터 값
            
        } param[ZURL_MAX_PARAM]; // GET 파라미터 (최대 10개까지)
        
    } zurl;

    typedef struct _zlive_m3u8_attr
    {
        zstr name;
        uint8_t quot:1;
        uint8_t type:7; // 1 이면 숫자형(val.intr), 2이면 실수형(val.flt) 0이면 문자열(val.str) 타입
        
        union {
            
            zstr str;
            uint32_t intr;
            float flt;
            
        } val;
        
    } zlive_m3u8_attr;
    
#define m3u8TagAttrMax  10

    typedef struct _zlive_m3u8_tag {

        zlive_m3u8_tag_type tag;
        
        uint32_t columns;
        
        zstr path;
        
        union {
            
            struct {
                
                uint32_t seq;
                uint32_t flt:1;
                uint32_t dur:31;
                
            } segment;

            struct {
                
                zlive_m3u8_attr list[m3u8TagAttrMax];
                uint8_t cc;
                
            } desc;
            
        } attr;
        
    } zlive_m3u8_tag;

    typedef struct _zlive_m3u8_desc
    {
        uint32_t edge:1;    // 실제 세그먼트를 포함하고 있으면 1
        uint32_t vod:1;     // VOD일 경우 1 (* EXT-X-ENDLIST 태그를 포함한 경우)
        uint32_t ver:30;    // M3U8 버전
        uint32_t seq;       // M3U8의 기준 시퀀스 번호
        
    } zlive_m3u8_desc;
    
    typedef struct _zlive_context* zlive;
    
    enum {
        
        zlive_tag_continue = 0,
        zlive_tag_skip,
        zlive_tag_stop,
    };
    
    typedef int (*zlive_m3u8_tag_callback) (zlive_m3u8_tag *tag, void *param);
    typedef int (*zlive_m3u8_swap_url_callback) (zipper_io_handle* ih, zlive_m3u8_tag *tag, zurl *srcUrl, zstr *swapUrl, void *param);
    
    typedef struct _zlive_m3u8_parse_param
    {
        struct {

            char enable;
            zipper_io_handle *ih;
            
            uint8_t **p;
            size_t *size;
            size_t *len;
            
        } rebuild;
        
        struct {
            
            zlive_m3u8_tag_callback tag;
            zlive_m3u8_swap_url_callback swapUrl;
            void *param;
            
        } cb;
        
    } zlive_m3u8_parse_param;

    // M3U8 유틸 함수 (외/내부용)
    
    // edit_manifest() 대응 함수
    int zlive_m3u8_edit_template(zipper_io_handle *ih, const char *src, size_t srclen, const char *fmt, uint8_t **out, size_t *size, size_t *length);
    int zlive_m3u8_parse(const char *src, size_t srclen, zlive_m3u8_desc *desc, zlive_m3u8_parse_param *param);
    
    enum {
      
        zlive_type_hls = 0,
        zlive_type_dash,
    };
    
    enum {
      
        zlive_no_discon = 0,
        zlive_adaptive_discon,
        zlive_discon,
        
    };
    
    typedef zipperBldr (*zlive_create_preroll_callback) (uint32_t dur, void *param);
    typedef void (*zlive_expire_preroll_callback) (zipperBldr bldr, void *param);

    typedef struct _zlive_create_param
    {
        uint32_t disall:1;
        uint32_t fpadding:1;
        uint32_t discon:2;
        uint32_t type:4;
        uint32_t resv:24;
        
        struct {
            
            zlive_create_preroll_callback create;
            zlive_expire_preroll_callback expire;
            void *param;
            
        } preroll;
        
    } zlive_create_param;
    
    int zlive_create(zipper_io_handle *ih, zlive_create_param *param, zlive *ctx);
    void zlive_free(zipper_io_handle *ih, zlive *ctx);
    
    typedef enum {
        
        zlive_select_src = 0,
        zlive_select_repack_segment,
        zlive_select_repack_segment_with_fpadding,
        zlive_select_repack_manifest,
        zlive_select_ad,
        zlive_select_err,
        
    } zlive_select_e;
    
    zlive_select_e zlive_select(zipper_io_handle *ih, zlive ctx, const char *path);
        
#define BYTES_OF_HLS_DISCONTINUTY_TAG    21
    
    size_t zlive_discontinuty_count(zlive ctx);
    size_t zlive_repack(zipper_io_handle *ih, zlive ctx, uint8_t *out);
    
    int zlive_last_error(zlive ctx);
    void zlive_roll_build_param(zlive ctx, zipper_builder_param *bprm);
    zipperBldr zlive_roll_build_context(zlive ctx);
    
    
#ifdef __cplusplus
}
#endif
    
#endif /* zlive_h */
