/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "../devices/fb_readback.h"

int main(void) {
    size_t stride = 0;
    assert(hv_fb_readback_layout(65, 4, &stride) == 2048 && stride == 512);
    assert(hv_fb_readback_layout(4096, 2048, &stride) == (32u << 20));
    assert(!hv_fb_readback_layout(4096, 2049, &stride));
    assert(!hv_fb_readback_layout(UINT32_MAX, 1, &stride));
    assert(!hv_fb_readback_layout(1, 0, &stride));

    uint8_t data[512] = {0};
    bool black;
    uint64_t initial = hv_fb_readback_measure(data, 2, 2, 256, &black);
    assert(black);
    data[3] = 255;
    data[255] = 1;
    assert(hv_fb_readback_measure(data, 2, 2, 256, &black) == initial && black);
    data[262] = 1;
    uint64_t colored = hv_fb_readback_measure(data, 2, 2, 256, &black);
    assert(!black && colored != initial);
    data[262] = 2;
    assert(hv_fb_readback_measure(data, 2, 2, 256, &black) != colored && !black);
    return 0;
}
