/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../devices/fb_readback.h"
#include "../devices/neptune_scanout_probe.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    enum { width = 1024, height = 768, pitch = 4352, offset = 4096 };
    uint64_t bytes = offset + (uint64_t)pitch * height;
    HvNptProbeTile tile = hv_npt_probe_tile(width, height, 4);
    assert(tile.x == 504 && tile.y == 376 && tile.width == 16 && tile.height == 16);
    for (unsigned i = 0; i < HV_NPT_PROBE_TILES; i++) {
        HvNptProbeTile edge = hv_npt_probe_tile(width, height, i);
        assert(edge.x + edge.width <= width && edge.y + edge.height <= height);
        HvNptProbeTile small = hv_npt_probe_tile(1, 2, i);
        assert(!small.x && !small.y && small.width == 1 && small.height == 2);
    }

    char path[] = "/tmp/winios-scanout-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(unlink(path) == 0);
    assert(ftruncate(fd, (off_t)bytes) == 0);
    uint8_t pixels[HV_NPT_PROBE_SIDE * HV_NPT_PROBE_SIDE * 4];
    bool black = false;
    uint32_t row = tile.width * 4;
    off_t start = offset + (off_t)tile.y * pitch + tile.x * 4;
    const uint8_t alpha = 255;
    assert(pwrite(fd, &alpha, 1, start + 3) == 1);
    assert(hv_npt_probe_shm(fd, width, height, pitch, offset, bytes, tile, pixels) == 0);
    assert(pixels[3] == 255);
    hv_fb_readback_measure(pixels, tile.width, tile.height, row, &black);
    assert(black);

    const uint8_t color[] = {42, 0, 0, 255};
    assert(pwrite(fd, color, sizeof(color), start + pitch + 8) == (ssize_t)sizeof(color));
    assert(hv_npt_probe_shm(fd, width, height, pitch, offset, bytes, tile, pixels) == 0);
    assert(memcmp(pixels + row + 8, color, sizeof(color)) == 0);
    hv_fb_readback_measure(pixels, tile.width, tile.height, row, &black);
    assert(!black);
    assert(lseek(fd, 0, SEEK_CUR) == 0);

    assert(hv_npt_probe_shm(fd, width, height, 32, offset, bytes, tile, pixels) == EINVAL);
    assert(hv_npt_probe_shm(fd, width, height, pitch, offset, offset, tile, pixels) == EINVAL);
    assert(hv_npt_probe_shm(fd, width, height, pitch, offset, bytes,
                           (HvNptProbeTile){width, 0, 1, 1}, pixels) == EINVAL);
    assert(ftruncate(fd, (off_t)bytes - 1) == 0);
    assert(hv_npt_probe_shm(fd, width, height, pitch, offset, bytes, tile, pixels) == EIO);
    assert(close(fd) == 0);
    assert(hv_npt_probe_shm(fd, width, height, pitch, offset, bytes, tile, pixels) == EBADF);
}
