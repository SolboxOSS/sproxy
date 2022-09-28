//
//  hdr.h
//  zipper
//
//  Created by Hyungpyo Oh on 2022/04/28.
//  Copyright © 2022 Voxio Dev. All rights reserved.
//

#ifndef hdr_h
#define hdr_h

enum mvColorPrimaries {
    
    cpUnknown = 0,
    cpBT709 = 1, // BT.709
    cpUnspecified = 2,
    cpBT470 = 4, // BT.470 System M
    cpBT601Pal = 5, // BT.601 PAL
    cpBT601NTSC = 6, // BT.601 NTSC
    cpSMPTE240M = 7, // SMPTE 240M
    cpGeneric = 8, // Generic Film
    cpBT2020 = 9, // BT.2020
    cpXYZ = 10, // XYZ
    cpDCIP3 = 11, // DCI P3
    cpDpP3 = 12, // Display P3
    cpEBUTech3213 = 22, // EBU Tech 3213
};

enum mvTransferCharacteristics
{
    tcUnknown = 0,
    tcBT709 = 1, // BT.709
    tcUnspecified = 2,
    tcBT470SysM = 4, // BT.470 System M
    tcBT470SysBg = 5, // BT.470 System B/G
    tcBT601 = 6, // BT.601
    tcSMPTE240M = 7, // SMPTE 240M
    tcLinear = 8, // Linear
    tcLogarithmic100 = 9, // Logarithmic (100:1)
    tcLogarithmic316 = 10, // Logarithmic (316.22777:1)
    tcXVYCC = 11, // xvYCC
    tcBT1361 = 12, // BT.1361
    tcSRGB = 13, // sRGB / sYCC
    tcBT2020_10bit = 14, // BT.2020 (10-bit)
    tcBT2020_12bit = 15, // BT.2020 (12-bit)
    tcPQ = 16, // PQ
    tcSMPTE428M = 17, // SMPTE 428M
    tcHLG = 18, // HLG
};

static const char *hlsTC[19] = {
    
    "",
    "SDR",
    "",
    "",
    "",
    "",
    "SDR",
    "",
    "",
    "",
    "",
    "",
    "",
    "SDR",
    "SDR",
    "SDR",
    "PQ",
    "",
    "HLG"
};

enum mvMatrixCoefficients
{
    mcIdentity = 0, // Identity
    mcBT709 = 1, // BT.709
    mcUnspecified = 2,
    mcFCC = 4, // FCC 73.682
    mcBT470 = 5, // BT.470 System B/G
    mcBT601 = 6, // BT.601 (same as BT.470 System B/G)
    mcSMPTE240M = 7, // SMPTE 240M
    mcYCGCO = 8, // YCgCo
    mcBT2020 = 9, // BT.2020 non-constant
    mcBT2020c = 10, // BT.2020 constant
    mcYDZDX = 11, // Y'D'zD'x
    mcChromaticity = 12, // Chromaticity-derived non-constant
    mcChromaticityc = 13, // Chromaticity-derived constant
    mcICTCP = 14, // ICtCp
};

static inline uint8_t trans_char_filter(uint8_t val)
{
    if(val == 1 || val == 2 || val == 3 || val == 12 || val > 18) return 0;
    
    return val;
}

#endif /* hdr_h */
