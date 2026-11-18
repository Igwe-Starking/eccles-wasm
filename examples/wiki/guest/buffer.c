/* buffer.c - hand a block of the guest's own memory to the host and get an answer back. */
#include "eccles_wasm_runtime.h"

EW_IMPORT("host_checksum") unsigned host_checksum(const void *data, unsigned len);
EW_IMPORT("host_report")   void     host_report(int code);

static uint8_t data[32];

EW_EXPORT("run") unsigned run(void) {
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)(i * 7);
    unsigned sum = host_checksum(data, sizeof data);       /* the pointer is an offset into the guest's memory */
    host_report((int)sum & 0xFF);
    return sum;
}
