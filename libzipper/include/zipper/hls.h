//
//  hls.h
//  zipper
//
//  Created by voxio on 2014. 10. 20..
//  Copyright (c) 2014년 SolBox Inc. All rights reserved.
//

#ifndef zipper_hls_h
#define zipper_hls_h

static const char sz_m3u8_head[] = "#EXTM3U\n";
static const char sz_m3u8_discon[] = "#EXT-X-DISCONTINUITY\n";
static const char sz_m3u8_end[] = "#EXT-X-ENDLIST\n";

#define HLSENC_METHOD_ON        (0x01 << 0)
#define HLSENC_METHOD_AESNI     (0x01 << 1)
#define HLSENC_METHOD_SAMPLE    (0x01 << 2)

#endif
