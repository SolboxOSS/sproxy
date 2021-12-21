
#ifndef zipper_zippererr_h
#define zipper_zippererr_h

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    
    zipper_err_success = 0,
    
    zipper_err_param = 1,                 // 잘못된 전달 인자
    zipper_err_alloc = 2,                 // 메모리 allocate 오류
    zipper_err_implemented = 3,           // (아직) 지원하지 않은 기능
    zipper_err_internal = 4,              // 라이브러리 내부 오류 (버그)
    
    zipper_err_parse_support = 5,         // 지원하지 않는 미디어 포맷 혹은 프로토콜
    zipper_err_parse_format = 6,          // mp4 박스 파싱 오류 혹은 mp3 ID3 태그 파싱 오류
    zipper_err_parse_cmvd = 7,            // cmvd 박스 데이터 압축 해제 오류
    zipper_err_parse_stss = 8,            // 비디오에 stss 박스가 없는 경우 (세그먼트 할 수 없음)
    zipper_err_parse_acodec = 9,          // 오디오 코덱을 지원하지 않음
    zipper_err_parse_vcodec = 10,         // 비디오 코덱을 지원하지 않음
    zipper_err_parse_nomedia = 11,        // 비디오/오디오가 없음
    zipper_err_interleaved = 12,          // Interleaved 되어 있지 않음.
    
    zipper_err_segment_index = 13,        // 세그먼트 분할 범위를 벗어남
    zipper_err_segment_offset = 14,       // 주어진 offset과 size가 세그먼트 TS의 크기를 벗어남.
    zipper_err_no_subtrack = 15,          // 대상 서브 트랙이 없는 경우
    zipper_err_keyframe = 16,             // 세그먼트 내에 비디오 키 프레임이 없음
    
    zipper_err_io_handle = 17,             // I/O 핸들러 오류 (17)
    
    zipper_err_no_decoder = 18,            // AVC 디코더를 불러올 수 없다(FFMpeg).
    zipper_err_open_decoder = 19,          // AVC 디코더를 열수 없다
    zipper_err_decode = 20,                // AVC 디코딩 오류
    zipper_err_avcnal = 21,                // AVC NAL Unit 파싱 오류 (데이터 손상)
    zipper_err_scaler = 22,                // 리사이징 라이브러리를 열 수 없다.
    
    zipper_err_trackrange = 23,            // 잘못된 트랙 범위
    zipper_err_track_compatible = 24,      // 호환되지 않은 트랙
    zipper_err_no_basecontext = 25,        // 기준 미디어 Context를 찾을 수 없다.
    zipper_err_no_trackseq = 26,           // 해당 Adaptive 트랙 시퀀스를 찾을 수 없다.
    
    zipper_err_output_support = 27,        // 출력 포맷을 지원할 수 없다.
    
    zipper_err_eof = 28,                   // 더이상 빌드할 데이터가 없다. (EOF) (28)
    zipper_err_no_callback = 29,           // 콜백 함수가 설정되어 있지 않다.
    zipper_err_protocol_broken = 30,       // 프로토콜 오류
    zipper_err_rejected = 31,              // 요청 거부
    zipper_err_ssl = 32,                   // SSL 라이브러리 오류
    zipper_err_no_rpc = 33,                // RPC가 존재하지 않는다.
    zipper_err_duplicated = 34,            // 중복된 요청이다.
    zipper_err_no_segment = 35,            // 대상 세그먼트가 존재하지 않는다.
    zipper_err_segment_data = 36,          // 대상 세그먼트 데이터는 캐시되어 컨텍스트가 가지고 있지 않다.
    zipper_err_sound_process = 37,         // 사운드 프로세싱 오류
    zipper_err_encode = 38,                // 인코딩 오류
    zipper_err_create_file = 39,           // 파일을 생성할 수 없다.
    
    zipper_err_need_more = 40,             // 데이터가 더 필요하다.
    zipper_err_exitloop = 41,
};
    
#ifdef __cplusplus
}
#endif

#endif
