//
//  iso639.h
//  mp42http
//
//  Created by voxio on 2014. 10. 13..
//  Copyright (c) 2014년 SolBox Inc. All rights reserved.
//

#ifndef mp42http_iso639_h
#define mp42http_iso639_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _iso639_code
{
    char    desc[16];
    char    code639_2[4];
    char    code639_1[4];
    
} iso639_code;

enum {
    
    lang_English    = 0,
    lang_Chinese,
    lang_French,
    lang_German,
    lang_Italian,
    lang_Japanese,
    lang_Korean,
    lang_Polish,
    lang_Portuguese,
    lang_Russian,
    lang_Spanish,
    lang_Thai,
    lang_Turkish,
    lang_Vietnamese,
    lang_Undefined,
    lang_End,
    
};

static const iso639_code iso639_codelist[lang_End] = {
    
    { "English",                "eng",  "en" },
    { "Chinese",                "chi",  "zh" },
    { "French",                 "fre",  "fr" },
    { "German",                 "ger",  "de" },
    { "Italian",                "ita",  "it" },
    { "Japanese",               "jpn",  "ja" },
    { "Korean",                 "kor",  "ko" },
    { "Polish",                 "pol",  "pl" },
    { "Portuguese",             "por",  "pt" },
    { "Russian",                "rus",  "ru" },
    { "Spanish",                "spa",  "es" },
    { "Thai",                   "tha",  "th" },
    { "Turkish",                "tur",  "tr" },
    { "Vietnamese",             "vie",  "vi" },
    { "Undefined",              "und",  "un" },    
};

uint8_t iso639_lang_code(uint16_t code);
uint16_t iso639_mp4_code(uint8_t code);

#ifdef __cplusplus
}
#endif
    
#endif
