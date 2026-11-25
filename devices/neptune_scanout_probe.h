/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_NEPTUNE_SCANOUT_PROBE_H
#define WINIOS_NEPTUNE_SCANOUT_PROBE_H

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

enum { HV_NPT_PROBE_SIDE = 16, HV_NPT_PROBE_TILES = 9 };

typedef struct HvNptProbeTile {
    uint32_t x, y, width, height;
} HvNptProbeTile;

static inline HvNptProbeTile hv_npt_probe_tile(uint32_t width, uint32_t height, unsigned index) {
    uint32_t w = width < HV_NPT_PROBE_SIDE ? width : HV_NPT_PROBE_SIDE;
    uint32_t h = height < HV_NPT_PROBE_SIDE ? height : HV_NPT_PROBE_SIDE;
    unsigned col = index % 3, row = index / 3;
    return (HvNptProbeTile){
        (width - w) * (col + 1) / 4, (height - h) * (row + 1) / 4, w, h
    };
}

static inline int hv_npt_probe_shm(int fd, uint32_t width, uint32_t height, uint32_t stride,
                                   uint32_t offset, uint64_t resource_bytes, HvNptProbeTile tile,
                                   uint8_t *rgbx) {
    if (!tile.width || !tile.height || tile.width > HV_NPT_PROBE_SIDE ||
        tile.height > HV_NPT_PROBE_SIDE || tile.x >= width || tile.y >= height ||
        tile.width > width - tile.x || tile.height > height - tile.y ||
        (uint64_t)width * 4 > stride || offset >= resource_bytes ||
        (uint64_t)stride * (height - 1) + (uint64_t)width * 4 > resource_bytes - offset)
        return EINVAL;
    struct stat info;
    if (fstat(fd, &info)) return errno;
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size < resource_bytes)
        return EIO;

    size_t row_bytes = (size_t)tile.width * 4;
    uint64_t start = offset + (uint64_t)tile.y * stride + (uint64_t)tile.x * 4;
    for (uint32_t y = 0; y < tile.height; ++y) {
        size_t done = 0;
        while (done < row_bytes) {
            ssize_t count = pread(fd, rgbx + y * row_bytes + done, row_bytes - done,
                                  (off_t)(start + (uint64_t)y * stride + done));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return count < 0 ? errno : EIO;
            done += (size_t)count;
        }
    }
    return 0;
}

#endif
