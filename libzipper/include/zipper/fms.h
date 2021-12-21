
#ifndef __zipper__fms__
#define __zipper__fms__

#include "io.h"
#include <stdint.h>

int build_fms_handshake(zipper_io_handle *io_handle, uint8_t *s12, uint8_t *c1, uint8_t ver);

#endif /* defined(__zipper__fms__) */
