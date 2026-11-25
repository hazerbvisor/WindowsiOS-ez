#ifndef WINVM_FB_DISPLAY_H
#define WINVM_FB_DISPLAY_H

#include <pthread.h>
#include <stdint.h>

#include "fw_cfg.h"
#include "machine.h"
#include "pcie.h"

/* Linear framebuffer display supporting QEMU `ramfb` (etc/ramfb in fw_cfg, used by
   EDK2 ArmVirtQemu GOP -> winload.efi -> Windows 11 BasicDisplay.sys + DWM)
   as well as optional VBE/bochs scanout. Zero MMIO exits per pixel write! */

typedef struct HvFbDisplay {
    HvMachine *m;
    pthread_mutex_t mu;

    /* Active scanout geometry (protected by mu) */
    uint64_t fb_pa;
    const uint8_t *fb_host;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t fourcc; /* DRM fourcc, e.g. XR24 / AR24 (32-bit BGRX) */
    int active;
    uint8_t *scanout;
    uint32_t scanout_width, scanout_height;
    uint64_t scanout_serial;
    /* Borrowed native texture; owner retains until replaced under mu. */
    void *native_texture;
    uint32_t native_width, native_height;

    /* Metal presenter handle (created lazily by fb_metal.m) */
    void *metal;
} HvFbDisplay;

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg);
/* A successful borrow holds the display lock. Release after copying or uploading pixels. */
int hv_fb_display_acquire_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h,
                                uint32_t *stride, uint64_t *scanout_serial);
void hv_fb_display_release_frame(HvFbDisplay *fb);
int hv_fb_display_get_size(HvFbDisplay *fb,uint32_t *w,uint32_t *h);
/* Texture is retained by the renderer owner, not by this C display object. */
void hv_fb_display_set_native(HvFbDisplay *fb,void *texture,uint32_t w,uint32_t h);
void hv_fb_display_native_dirty(HvFbDisplay *fb);
/* Takes ownership of a tightly packed BGRA scanout, or falls back to RAMFB with NULL. */
void hv_fb_display_set_scanout(HvFbDisplay *fb, uint8_t *pixels, uint32_t w, uint32_t h);

/* Metal layer presentation & PNG screenshot helpers (implemented in devices/fb_metal.m) */
void hv_fb_metal_set_layer(HvFbDisplay *fb, void *ca_metal_layer);
void hv_fb_metal_set_rate(HvFbDisplay *fb, unsigned fps);
uint64_t hv_fb_metal_frames(HvFbDisplay *fb);
void hv_fb_metal_diagnostics(HvFbDisplay *fb,char *buffer,size_t capacity);
int hv_fb_save_png(HvFbDisplay *fb, const char *path);

#endif
