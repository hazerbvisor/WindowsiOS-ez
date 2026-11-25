/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_FB_READBACK_H
#define WINIOS_FB_READBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline size_t hv_fb_readback_layout(uint32_t width, uint32_t height, size_t *stride) {
    if (!width || !height || width > 4096 || height > 4096) return 0;
    size_t row = ((size_t)width * 4 + 255) & ~(size_t)255;
    if (height > (32u << 20) / row) return 0;
    *stride = row;
    return row * height;
}

/* Alpha and row padding have no visual content. The fingerprint is kept local. */
static inline uint64_t hv_fb_readback_measure(const uint8_t *data, uint32_t width, uint32_t height,
                                              size_t stride, bool *black) {
    uint64_t fingerprint = UINT64_C(14695981039346656037);
    unsigned rgb = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = data + (size_t)y * stride;
        for (uint32_t x = 0; x < width; ++x, row += 4) {
            unsigned pixel = (unsigned)row[0] | (unsigned)row[1] << 8 | (unsigned)row[2] << 16;
            rgb |= pixel;
            fingerprint = (fingerprint ^ pixel) * UINT64_C(1099511628211);
        }
    }
    *black = rgb == 0;
    return fingerprint;
}

#endif
