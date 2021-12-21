
#ifndef base64_h
#define base64_h

#include <stdlib.h>
#include <stdint.h>

size_t base64_decode(const char* code, uint8_t *buf);
size_t base64_encode(const uint8_t* buf, size_t len, char *code);

#endif /* base64_h */
