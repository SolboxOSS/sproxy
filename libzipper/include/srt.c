//
//  srt.c
//  zipper
//
//  Created by Hyungpyo Oh on 2022/01/12.
//  Copyright © 2022 Voxio Dev. All rights reserved.
//

#include "srt.h"
#include "voxio_def.h"
#include "zipper.h"
#include "zippererr.h"
#include "iso639.h"
#include "ts.h"

#include <string.h>
#include <stdio.h>

#define srtGuessLimit   1024
#define maxSrtSize      2097152

typedef enum {

    srtStepTimeHeader = 0,
    srtStepSubtitle,
    
} srtStep;

typedef enum {

    srtTimeTokenHour = 0,
    srtTimeTokenMin,
    srtTimeTokenSec,
    srtTimeTokenMs
    
} srtTimeTokenStep;

typedef struct _srtSubItem
{
    uint64_t start;
    uint64_t end;
        
    char *sub;
    
    unsigned char attr;
    unsigned short sublen;
    
} srtSubItem;

static uint32_t _srtUintOf(const char *seq)
{
    uint32_t res = 0;
    
    while(seq[0]) {

        if(seq[0] != ' ') {
        
            if(seq[0] < '0' ||  seq[0] > '9') return 0;

            res *= 10;
            res += seq[0] - '0';
        }

        seq++;
    }
    
    return res;
}

static unsigned char _attrOfTimeKey(char *key, char *value)
{
    if(!strncmp(key, "line", 4) || !strncmp(key, "position", 8)) {

        int num = 0;
        
        while(value[0]) {
        
            if(value[0] < '0' ||  value[0] > '9') {
                
                if(value[0] == '-') return 0;
                
            } else {

                num *= 10;
                num |= value[0] - '0';
            }
            
            value++;
        }

        if(num < 40) return subAlignTop;
        else if(num <= 60) return subAlignMiddle;
        
    } else if(!strncmp(key, "align", 5)) {
     
        if(!strncmp(value, "left", 4) || !strncmp(value, "start", 5)) {
            
            return subAlignLeft;
            
        } else if(!strncmp(value, "right", 5) || !strncmp(value, "end", 3)) {
            
            return subAlignRight;
        }
    }
    
    return 0;
}

static unsigned char _srtTimeAttr(char *attr)
{
    unsigned char res = 0;
    char *key = NULL, *value = NULL;
    
    while(attr[0]) {
        
        if(key) {
            
            if(value) {

                if(attr[0] == ' ') {
                    
                    attr[0] = '\0';
                    while(value[0] == ' ') value++;
                    res |= _attrOfTimeKey(key, value);
                    attr[0] = ' ';
                    
                    key = NULL;
                    value = NULL;
                }
              
            } else if(attr[0] == ':') {
                
                value = attr + 1;
            }
            
        } else if(attr[0] != ' ') key = attr;
        
        attr++;
    }
    
    if(key && value) res |= _attrOfTimeKey(key, value);
    
    return res;
}

static char _srtTag(char *tag, size_t *offset)
{
    if(!strncmp(tag, "<b>", 3)) {

        *offset += 3;
        return 'b';
        
    } else if(!strncmp(tag, "<b/>", 4)) {

            *offset += 4;
            return 'b';

    } else if(!strncmp(tag, "</b>", 4)) {

            *offset += 4;
            return 'b';

    } else if(!strncmp(tag, "<i>", 3)) {

            *offset += 3;
            return 'i';
            
    } else if(!strncmp(tag, "<i/>", 4)) {

            *offset += 4;
            return 'i';

    } else if(!strncmp(tag, "</i>", 4)) {

            *offset += 4;
            return 'i';

    } else if(!strncmp(tag, "{\\an", 4)) {
    
        tag += 4;
        if(tag[0] != '\0' && tag[1] == '}') {
            
            *offset += 5;
            return tag[0];
        }
    }
    
    return 0;
}

int _srtheader(char *str, size_t len, srtSubItem *item)
{
    size_t i;
    BOOL end = NO;
    srtTimeTokenStep tt = srtTimeTokenHour;
    char *token = str;
    
    item->start = 0;
    item->end = 0;
    item->attr = 0;
     
    for(i = 0; i < len; i++) {
        
        if(tt == srtTimeTokenMs && !end && i > 1 && str[i] == '>' && str[i-1] == '-' && str[i-2] == '-') {
            
            str[i-2] = '\0';
            
            item->start += (uint64_t)_srtUintOf(token) * 90LL;
            end = YES;
            token = &str[i+1];
            tt = srtTimeTokenHour;

            str[i-2] = '-';
            
        } else if(str[i] == ':') {
            
            str[i] = '\0';
            
            switch(tt) {
                    
                case srtTimeTokenHour:
                    if(end) item->end = (uint64_t)_srtUintOf(token) * 324000000LL;
                    else item->start = (uint64_t)_srtUintOf(token) * 324000000LL;
                    token = &str[i+1];
                    tt = srtTimeTokenMin;
                    break;
                    
                case srtTimeTokenMin:
                    if(end) item->end += (uint64_t)_srtUintOf(token) * 5400000LL;
                    else item->start += (uint64_t)_srtUintOf(token) * 5400000LL;
                    token = &str[i+1];
                    tt = srtTimeTokenSec;
                    break;
                    
                default:
                    return zipper_err_exitloop;
            }
            
            str[i] = ':';
            
        } else if(str[i] == ',' || str[i] == '.') {
            
            if(tt != srtTimeTokenSec) return zipper_err_exitloop;
            str[i] = '\0';

            if(end) item->end += (uint64_t)_srtUintOf(token) * 90000LL;
            else item->start += (uint64_t)_srtUintOf(token) * 90000LL;
            token = &str[i+1];
            tt = srtTimeTokenMs;
                            
            str[i] = ',';
            if(end) break;
        }
    }
    
    if(tt != srtTimeTokenMs || !end) return zipper_err_exitloop;
    
    uint64_t ms = 0;
    
    while(token[0]) {
        
        if(token[0] < '0' ||  token[0] > '9') {
            
            item->attr = _srtTimeAttr(token);
            break;
        }

        ms *= 10;
        ms += token[0] - '0';
        
        token++;
    }
    
    item->end += ms * 90LL;
    
    if(item->end >= item->start) return zipper_err_success;
    return zipper_err_parse_format;
}

int _srtAddSubItem(zipper_io_handle *io_handle, zidx_context *ctx, zipper_media_desc *desc, srtSubItem *item)
{
    parse_frame_node *node;
    

    if(!desc->tcc) {
        
        desc->tcc = 1;
        desc->text[0].lang = lang_Undefined;
        desc->text[0].sdci = codec_index_text;
        
        ctx->config.text = (mp4_track_desc *)callocx(io_handle, sizeof(mp4_track_desc), 1);
        
        ctx->config.text->codec = codec_index_text;
        ctx->config.text->tick = 0;
        ctx->config.text->tscale = TS_CLOCK_RATE;
        ctx->config.text->vfy = YES;
    }

    if(ctx->parse.param->fn_tail) {
        
        if(ctx->parse.param->fn_tail->pts >= item->start) {
            
            item->start = ctx->parse.param->fn_tail->pts;
        }
    }
    
    ctx->config.text->cfgblock.data = mallocx(io_handle, ctx->config.text->cfgblock.data, ctx->config.text->cfgblock.size + sizeof(subTitleHeader) + item->sublen + 1);
    if(!ctx->config.text->cfgblock.data) return zipper_err_alloc;
    
    subTitleHeader *h = (subTitleHeader *)&ctx->config.text->cfgblock.data[ctx->config.text->cfgblock.size];
    char *line = NULL, *sub = (char *)&ctx->config.text->cfgblock.data[ctx->config.text->cfgblock.size + sizeof(subTitleHeader)];
    
    memset(h, 0, sizeof(subTitleHeader));
    h->dur = (uint32_t)(item->end - item->start);
    h->line = 1;
    h->attr = item->attr;
    
    // left trim
    while(item->sub[0] == '\n' || item->sub[0] == ' ') item->sub++;
    
    size_t i, cpl;
    char *str = item->sub;
    char tag;
    
    for(i = 0; str[i]; i++) {
        
        if((tag = _srtTag(&str[i], &i)) != 0) {
            
            switch(tag) {
                    
                case 'b':
                    h->attr |= subBold;
                    break;
                    
                case 'i':
                    h->attr |= subItalic;
                    break;
                    
                case '1':
                    h->attr |= subAlignLeft | subAlignBottom;
                    break;
                    
                case '2':
                    h->attr |= subAlignCenter | subAlignBottom;
                    break;

                case '3':
                    h->attr |= subAlignRight | subAlignBottom;
                    break;

                case '4':
                    h->attr |= subAlignLeft | subAlignMiddle;
                    break;

                case '5':
                    h->attr |= subAlignCenter | subAlignMiddle;
                    break;

                case '6':
                    h->attr |= subAlignRight | subAlignMiddle;
                    break;

                case '7':
                    h->attr |= subAlignLeft | subAlignTop;
                    break;

                case '8':
                    h->attr |= subAlignCenter | subAlignTop;
                    break;

                case '9':
                    h->attr |= subAlignRight | subAlignTop;
                    break;
            }
            
        } else {

            sub[h->len] = str[i];

            if(str[i] == '\n') {
                
                if(line) {
                    
                    cpl = 0;
                    
                    while(line < &sub[h->len]) {
                               
                        if((line[0] & 0xc0) != 0x80) cpl++;
                        line++;
                    }
                    
                    if(cpl > h->cpl) h->cpl = (uint32_t)cpl;
                }
                
                h->line++;
                line = NULL;
                
            } else if(line == NULL) line = &sub[h->len];

            h->len++;
        }
    }
    
    sub[h->len] = '\0';
    
    for(i = h->len-1; i >= 0; i--) {
        
        if(sub[i] == '\n' || sub[i] == ' ') {
            
            if(sub[i] == '\n') h->line--;
            h->len--;
            sub[i] = '\0';
            
        } else break;
    }
    
    if(h->len > 0) {
                
        node = (parse_frame_node *)pallocx(io_handle, sizeof(parse_frame_node), &ctx->parse.param->mempool);
        node->track = MP4_TRACK_TEXT;
        node->idx = 0;
        node->offset = ctx->config.text->cfgblock.size;
        node->size = sizeof(subTitleHeader) + h->len + 1;
        node->dts = node->pts = item->start;
        node->sync = YES;
    
        if(!ctx->parse.param->fn_head) {
    
            ctx->parse.param->fn_head = ctx->parse.param->fn_tail = node;
    
        } else {
    
            ctx->parse.param->fn_tail->next = node;
            ctx->parse.param->fn_tail->mnext = node;
            node->prev = ctx->parse.param->fn_tail;
    
            ctx->parse.param->fn_tail = node;
        }
        
//        fprintf(stderr, "%02d:%02d:%02d.%03d (dur=%.3f, attr=%02x, len=%u, cpl=%u, line=%u)\n[%s]\n",
//                (int)(item->start / 324000000LL),
//                (int)((item->start % 324000000LL) / 5400000LL),
//                (int)((item->start % 5400000LL) / 90000LL),
//                (int)((item->start % 90000LL) / 90LL),
//                (double)h->dur / (double)TS_CLOCK_RATE,
//                h->attr,
//                h->len,
//                h->cpl,
//                h->line,
//                srtItemTextOf((uint8_t *)h));

        ctx->config.text->cfgblock.size += node->size;
        ctx->parse.param->chunksize.text[0] += node->size;
        
    }
    
    return zipper_err_success;
}

int parse_srt(zipper_io_handle *io_handle, zidx_context *ctx, void *pdesc, off_t *fo)
{
    zipper_media_desc *desc = (zipper_media_desc *)pdesc;
    int res = zipper_err_success;
    uint32_t unit;
    ssize_t rb;
    
    if(!ctx->parse.buf.target.size) {
    
        ctx->parse.buf.target.size = msize(NULL, io_handle);
        if(ctx->parse.buf.target.size > maxSrtSize) return zipper_err_parse_format;
    
        ctx->parse.buf.target.offset = 0;
        ctx->parse.buf.p = (uint8_t *)mallocx(io_handle, NULL, ctx->parse.buf.target.size);
        ctx->parse.buf.size = (uint32_t)ctx->parse.buf.target.size;
        ctx->parse.buf.offset = 0;
    }

    do {
        
        unit = ctx->parse.buf.size - ctx->parse.buf.offset;
        
        if(io_handle->readbuf.length) {

            if(unit > io_handle->readbuf.length) unit = io_handle->readbuf.length;
            
        } else if(io_handle->bufsize && unit > io_handle->bufsize) unit = io_handle->bufsize;
                        
        rb = mread(NULL, io_handle, &ctx->parse.buf.p[ctx->parse.buf.offset], unit, fo);
        
        if(rb == ZIO_TRY_AGAIN) return zipper_err_need_more;
        else if(rb != unit) {
            return zipper_err_io_handle;
        }
        else {
            
            ctx->parse.buf.offset += unit;
            
            if(ctx->parse.buf.offset == ctx->parse.buf.size) {
                
                ctx->parse.buf.offset = 0;
                ctx->parse.buf.ready = YES;
                
            } else {
                
                ctx->parse.buf.target.offset += unit;
            }
        }
        
    } while(!ctx->parse.buf.ready);
    
    uint8_t *p;
    size_t plen = 0, poffset = 0;
    
    p = (uint8_t *)mallocx(io_handle, NULL, ctx->parse.buf.size + 1);
    if(!p) return zipper_err_alloc;

    // extract BOM(Byte Order Mark)
    if(ctx->parse.buf.p[0] == 0xEF && ctx->parse.buf.p[1] == 0xBB && ctx->parse.buf.p[2] == 0xBF) ctx->parse.buf.offset = 3;

    while(ctx->parse.buf.offset < ctx->parse.buf.size) {
        
        if(ctx->parse.buf.p[ctx->parse.buf.offset] != '\r') p[plen++] = ctx->parse.buf.p[ctx->parse.buf.offset];
        ctx->parse.buf.offset++;
    }
    
    p[plen] = '\0';
            
    //srtHeader psh, sh;
    char lf, *line, *seq = NULL;
    srtStep step = srtStepTimeHeader;
    srtSubItem prev, cur;
    uint64_t dur = 0;
                    
    line = (char *)&p[poffset];
    prev.sub = NULL;

    for(; res == zipper_err_success && poffset <= plen; poffset++) {
            
        if(!desc->tcc && poffset > srtGuessLimit) {
            
            res = zipper_err_parse_format;
            break;
        }
        
        if(p[poffset] == '\n' || p[poffset] == '\0') {

            lf = p[poffset];
            p[poffset] = '\0';
                
            if(line[0]) {
                    
                switch(step) {
                
                    case srtStepTimeHeader:
                        res = _srtheader(line, (size_t)((char *)&p[poffset] - line), &cur);
                        
                        if(res == zipper_err_success) {

                            if(prev.sub) {
                                
                                if(seq) {
                                    
                                    seq[0] = '\0';
                                    prev.sublen = (unsigned short)(seq - prev.sub);
                                    
                                } else {
                                    
                                    line[0] = '\0';
                                    prev.sublen = (unsigned short)(line - prev.sub);
                                }
                                
                                res = _srtAddSubItem(io_handle, ctx, desc, &prev);
                                dur = prev.end;
                            }

                            prev.start = cur.start;
                            prev.end = cur.end;
                            prev.attr = cur.attr;
                            prev.sub = NULL;
                            seq = NULL;

                            step = srtStepSubtitle;
                            
                        } else if(res == zipper_err_exitloop) {

                            if(_srtUintOf(line) > 0) seq = line;
                            else seq = NULL;
                            
                            res = zipper_err_success;
                            
                        } else {
                            prev.sub = NULL;
                        }
                        break;
                        
                    case srtStepSubtitle:
                        prev.sub = line;
                        step = srtStepTimeHeader;
                        break;
                }
            }
                
            p[poffset] = lf;
            line = (char *)&p[poffset+1];
        }
    }

    if(prev.sub) {
        
        prev.sublen = (unsigned short)((char *)&p[plen] - prev.sub);
        res = _srtAddSubItem(io_handle, ctx, desc, &prev);
        dur = prev.end;
    }

    freex(io_handle, p);
    
    if(res == zipper_err_success) {

        desc->sfmt = src_format_srt;
        
        if(ctx->parse.param->fn_tail) {
            
            desc->duration = (float)((double)ctx->parse.param->fn_tail->pts / (double)TS_CLOCK_RATE);
            ctx->config.text->duration =  dur;
        }
    }
    
    return res;
}
