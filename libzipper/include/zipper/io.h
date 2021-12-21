//
//  ioh.h
//  zipper
//
//  Created by voxio on 2014. 9. 17..
//  Copyright (c) 2014년 SolBox Inc. All rights reserved.
//

#ifndef zipper_ioh_h
#define zipper_ioh_h

#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIO_TRY_AGAIN  -2
    
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// mp4 파일 크기 구하기 함수, 전체 크기(bytes)를 리턴한다.
// context : 미디어 Context, zipper_create_media_context, zipper_media_info 내에서 호출 시에는 NULL
// param : 사용자 파리미터
typedef off_t (*zipper_msize_func)(void *context, void *param);

// mp4 절대 위치 읽기 함수, 읽은 크기(bytes)를 리턴한다.
//
// context : 미디어 Context, zipper_create_media_context, zipper_media_info 내에서 호출 시에는 NULL
// buf : 출력 버퍼
// size : 읽을 크기(bytes)
// offset : 읽을 위치
// param : 사용자 파리미터
//
// [주의]
// 만약, zipper_io_handle.bufsize가 0이 아닐 경우, bufsize 단위로 읽기를 요청하므로
// mp4 파일의 범위를 벗어날 수 있다.
typedef ssize_t (*zipper_mio_func)(void *context, void *buf, size_t size, off_t offset, void *param);

// ts 출력 함수
// build_segmented_ts 함수 내부로부터 먹싱후 출력 시 일정 단위(188 * N)로 호출된다.
// 0이 아닌 값(마이너스 값)을 리턴하면 build_segmented_ts 함수의 먹싱 루틴이 종료된다.
// block: 블럭 데이터
// size: 블럭 데이터의 크기
// param : 사용자 파리미터
typedef int (*zipper_write_func)(unsigned char *block, size_t size, void *param);

typedef void *(*zipper_malloc)(size_t size, void *param);
typedef void *(*zipper_calloc)(size_t size, size_t multiply, void *param);
typedef void *(*zipper_realloc)(void *buf, size_t newsize, void *param);
typedef void (*zipper_free)(void *buf, void *param);

// 메모리 Pool 기반 메모리 할당 함수
//
// size : 할당 받을 크기
// pool : 메모리 Pool 참조 포인터 (최초 호출 시 (*pool)은 NULL 이다.)
typedef void *(*zipper_alloc_from_pool)(size_t size, void **pool, void *param);
    
// 메모리 Pool 해제 함수
//
// pool : zipper_alloc_from_pool으로부터 할당된 메모리 Pool의 참조 포인터
typedef void (*zipper_free_pool) (void *pool, void *param);
    
// I/O 핸들러
typedef struct _zipper_io_handle
{
    uint32_t        bufsize;        // io 수행 시 기준 버퍼 크기, 0이면 무시
    
    struct {
        
        // reader 핸들러 우선순위: handle => mfp => (rfp,sfp)
        zipper_msize_func       sizefp;     // mp4 파일 크기 구하기 함수
        zipper_mio_func         readfp;     // mp4 읽기 함수 (절대 offset 지정), NULL이면 무시된다.
        void                    *param;     // 사용자 파라미터
        
    } reader;
    
    struct {
        
        zipper_write_func     fp;     // 출력 함수 (ts)
        void                    *param; // 사용자 파라미터
        
    } writer;
    
    struct {
        
        unsigned char       *data;  // 반드시 bufsize 크기만큼 allocate 해주어야 한다.
        uint32_t            offset;
        uint32_t            length;
        
    } readbuf;   // 읽기 버퍼 (bufsize가 0이 아닌 경우 필요)
    
    struct {
        
        zipper_malloc     malloc;
        zipper_calloc     calloc;
        zipper_realloc    realloc;
        zipper_free       free;
        
        struct {

            zipper_alloc_from_pool alloc;
            zipper_free_pool       free;
            
        } pool; // 별도의 메모리 Pool 기반 콜백
        
        void *param; // 사용자 파라미터
        
    } memfunc; // 메모리 관리 콜백, NULL이 아니면 본 함수를 통해서 메모리를 할당하고, 해제한다.
    
} zipper_io_handle;

void *mallocx(zipper_io_handle *io_handle, void *old, size_t size);
void *callocx(zipper_io_handle *io_handle, size_t size, size_t multiply);
void freex(zipper_io_handle *io_handle, void *buf);
void mresetrbuf(zipper_io_handle *io_handle);
void *pallocx(zipper_io_handle *io_handle, size_t size, void **pool);
    
off_t msize(void *context, zipper_io_handle *io_handle);
ssize_t mread(void *context, zipper_io_handle *io_handle, void *buf, size_t size, off_t *offset);
off_t mseek(void *context, zipper_io_handle *io_handle, off_t offset, int whence, off_t *pos);
int mwrite(zipper_io_handle *io_handle, void *buf, size_t size, off_t *written);

#ifdef __cplusplus
}
#endif
   
#endif
