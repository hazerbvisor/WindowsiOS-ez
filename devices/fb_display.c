#include "fb_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAMFBCfg written by EDK2 QemuRamFbDxe via fw_cfg DMA to "etc/ramfb" (28 bytes, big-endian):
   struct RAMFBCfg {
       uint64_t addr;
       uint32_t fourcc;
       uint32_t flags;
       uint32_t width;
       uint32_t height;
       uint32_t stride;
   } __attribute__((packed));
*/

static uint32_t be32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static uint64_t be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static void on_ramfb_write(void *opaque, const uint8_t *data, uint32_t len) {
    HvFbDisplay *fb = opaque;
    if (len < 28) return;

    uint64_t addr = be64(data + 0);
    uint32_t fourcc = be32(data + 8);
    uint32_t width = be32(data + 16);
    uint32_t height = be32(data + 20);
    uint32_t stride = be32(data + 24);

    if (width < 64 || width > 8192 || height < 64 || height > 8192) return;
    if (stride == 0) stride = width * 4;
    if (stride < width * 4 || stride > 65536) return;

    uint64_t total_bytes = (uint64_t)stride * (height - 1) + (uint64_t)width * 4;
    void *host_ptr = hv_phys_ptr(fb->m, addr, total_bytes);
    if (!host_ptr) {
        fprintf(stderr, "winvm: ramfb ignored invalid PA 0x%llx (%ux%u stride %u)\n",
                (unsigned long long)addr, width, height, stride);
        return;
    }

    pthread_mutex_lock(&fb->mu);
    fb->fb_pa = addr;
    fb->fb_host = (const uint8_t *)host_ptr;
    fb->width = width;
    fb->height = height;
    fb->stride = stride;
    fb->fourcc = fourcc;
    fb->active = 1;
    pthread_mutex_unlock(&fb->mu);

    fprintf(stderr, "winvm: ramfb configured %ux%u stride=%u fourcc=0x%08x at PA 0x%llx\n",
            width, height, stride, fourcc, (unsigned long long)addr);
}

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg) {
    HvFbDisplay *fb = calloc(1, sizeof *fb);
    if (!fb) return NULL;
    fb->m = m;
    pthread_mutex_init(&fb->mu, NULL);
    if (fw_cfg) {
        hv_fw_cfg_add_writable_file(fw_cfg, "etc/ramfb", 28, on_ramfb_write, fb);
    }
    return fb;
}

HvFbSource hv_fb_display_source_locked(const HvFbDisplay *fb) {
    bool ramfb_ready = fb->active && fb->fb_host && fb->width && fb->height;
    if (fb->prefer_ramfb && ramfb_ready) return HV_FB_RAMFB;
    if (fb->native_texture) return HV_FB_NATIVE;
    if (fb->scanout) return HV_FB_VIRTIO;
    return ramfb_ready ? HV_FB_RAMFB : HV_FB_NONE;
}

void hv_fb_display_prefer_ramfb(HvFbDisplay *fb, bool prefer) {
    if (!fb) return;
    pthread_mutex_lock(&fb->mu);
    fb->prefer_ramfb = prefer;
    pthread_mutex_unlock(&fb->mu);
}

bool hv_fb_display_prefers_ramfb(HvFbDisplay *fb) {
    if (!fb) return false;
    pthread_mutex_lock(&fb->mu);
    bool prefer = fb->prefer_ramfb;
    pthread_mutex_unlock(&fb->mu);
    return prefer;
}

void hv_fb_display_diagnostics(HvFbDisplay *fb, char *buffer, size_t capacity) {
    if (!capacity) return;
    *buffer = '\0';
    if (!fb) return;
    static const char *const names[] = {
        [HV_FB_NONE] = "none", [HV_FB_RAMFB] = "RAMFB",
        [HV_FB_VIRTIO] = "VirtIO-2D", [HV_FB_NATIVE] = "Metal-native",
    };
    pthread_mutex_lock(&fb->mu);
    HvFbSource source = hv_fb_display_source_locked(fb);
    snprintf(buffer, capacity, "display_source: active=%s preference=%s ramfb=%ux%u native=%ux%u\n",
             names[source], fb->prefer_ramfb ? "RAMFB" : "auto",
             fb->active ? fb->width : 0, fb->active ? fb->height : 0,
             fb->native_width, fb->native_height);
    pthread_mutex_unlock(&fb->mu);
}

int hv_fb_display_acquire_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h,
                                uint32_t *stride, uint64_t *scanout_serial) {
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    HvFbSource source = hv_fb_display_source_locked(fb);
    if (source == HV_FB_VIRTIO) {
        *pixels = fb->scanout;
        *w = fb->scanout_width;
        *h = fb->scanout_height;
        *stride = *w * 4;
        if (scanout_serial) *scanout_serial = fb->scanout_serial;
        return 0;
    }
    if (source != HV_FB_RAMFB) {
        pthread_mutex_unlock(&fb->mu);
        return -1;
    }
    *pixels = fb->fb_host;
    *w = fb->width;
    *h = fb->height;
    *stride = fb->stride;
    if (scanout_serial) *scanout_serial = 0;
    return 0;
}

void hv_fb_display_release_frame(HvFbDisplay *fb) {
    pthread_mutex_unlock(&fb->mu);
}

void hv_fb_display_set_scanout(HvFbDisplay *fb, uint8_t *pixels, uint32_t w, uint32_t h) {
    pthread_mutex_lock(&fb->mu);
    uint8_t *old = fb->scanout;
    fb->native_texture = NULL;
    fb->native_width = fb->native_height = 0;
    fb->scanout = pixels;
    fb->scanout_width = w;
    fb->scanout_height = h;
    if (++fb->scanout_serial == 0) fb->scanout_serial++;
    pthread_mutex_unlock(&fb->mu);
    free(old);
}

int hv_fb_display_get_size(HvFbDisplay *fb,uint32_t *w,uint32_t *h) {
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    uint32_t width=0,height=0;
    switch (hv_fb_display_source_locked(fb)) {
    case HV_FB_NATIVE: width=fb->native_width; height=fb->native_height; break;
    case HV_FB_VIRTIO: width=fb->scanout_width; height=fb->scanout_height; break;
    case HV_FB_RAMFB: width=fb->width; height=fb->height; break;
    case HV_FB_NONE: break;
    }
    pthread_mutex_unlock(&fb->mu);
    if (!width || !height) return -1;
    if (w) *w=width;
    if (h) *h=height;
    return 0;
}
void hv_fb_display_set_native(HvFbDisplay *fb,void *texture,uint32_t w,uint32_t h) {
    pthread_mutex_lock(&fb->mu);
    uint8_t *old=fb->scanout;
    fb->scanout=NULL;
    fb->scanout_width=fb->scanout_height=0;
    fb->native_texture=texture;
    fb->native_width=texture?w:0;
    fb->native_height=texture?h:0;
    if (++fb->scanout_serial==0) fb->scanout_serial++;
    pthread_mutex_unlock(&fb->mu);
    free(old);
}
void hv_fb_display_native_dirty(HvFbDisplay *fb) {
    pthread_mutex_lock(&fb->mu);
    if (fb->native_texture && ++fb->scanout_serial==0) fb->scanout_serial++;
    pthread_mutex_unlock(&fb->mu);
}
