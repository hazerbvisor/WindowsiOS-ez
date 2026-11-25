/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "virtio_gpu.h"
#include "virtio_gpu_modes.h"
#ifdef WINIOS_ENABLE_NEPTUNE
#include "neptune_scanout_probe.h"
#include "neptune_wire.h"
#include <time.h>
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire structures follow VirtIO 1.2 §5.7. The supported hosts are little-endian. */
enum {
    GET_DISPLAY_INFO = 0x0100, RESOURCE_CREATE_2D, RESOURCE_UNREF, SET_SCANOUT, RESOURCE_FLUSH,
    TRANSFER_TO_HOST_2D, RESOURCE_ATTACH_BACKING, RESOURCE_DETACH_BACKING, GET_CAPSET_INFO,
    GET_CAPSET, GET_EDID,
    UPDATE_CURSOR = 0x0300, MOVE_CURSOR,
    OK_NODATA = 0x1100, OK_DISPLAY_INFO, OK_EDID = 0x1104,
    ERR_UNSPEC = 0x1200, ERR_OOM, ERR_SCANOUT, ERR_RESOURCE, ERR_PARAMETER = 0x1205,
    FENCE = 1, EDID_FEATURE = 1 << 1,
    BGRA = 1, BGRX = 2, RGBA = 67, RGBX = 134,
    RESOURCES = 64, BACKING_ENTRIES = 8192, SURFACE_LIMIT = 32 << 20,
    RESOURCE_BUDGET = (256 << 20) - 3 * SURFACE_LIMIT,
    DEFAULT_WIDTH = 1024, DEFAULT_HEIGHT = 768, CURSOR_SIZE = 64,
    QUEUES = 2, REQUESTS_PER_POLL = 8
};

typedef struct Header {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t context;
    uint8_t ring, pad[3];
} Header;

typedef struct Rect { uint32_t x, y, width, height; } Rect;
typedef struct Entry { uint64_t addr; uint32_t len, pad; } Entry;
typedef struct Resource {
    uint32_t id, format, width, height, count;
    size_t size;
    uint8_t *pixels;
    Entry *backing;
    uint64_t backing_bytes;
} Resource;

typedef struct Scanout {
    uint32_t resource;
    Rect rect;
    uint8_t *front;
} Scanout;

typedef struct Cursor {
    uint32_t x, y, hot_x, hot_y;
    bool visible;
    uint8_t pixels[CURSOR_SIZE * CURSOR_SIZE * 4];
} Cursor;

#ifdef WINIOS_ENABLE_NEPTUNE
/* Store the private descriptor snapshot and response until the exact renderer
 * fence completes. No callback thread touches guest memory or used rings. */
typedef struct NeptuneReply {
    uint64_t ticket;
    uint32_t opcode;
    HvVirtqElem elem;
    uint32_t bytes;
    uint8_t response[24];
} NeptuneReply;
typedef struct ProducerProbe {
    uint64_t next_ms;
    unsigned attempts, black, errors;
    int last_error;
    uint32_t first_resource, last_resource, last_nonblack_resource;
    bool enabled;
} ProducerProbe;
#endif

struct HvVirtioGpu {
    HvVirtio vio;
    HvFbDisplay *fb;
    uint32_t preferred_width, preferred_height;
    Resource resources[RESOURCES];
    Scanout screen;
    Cursor cursor;
    size_t allocated;
    bool pending[QUEUES];
    uint64_t completed[QUEUES], malformed, command_errors, publishes, resets, edid_compat;
    struct { uint32_t opcode, response, flags, context, length; uint64_t count; } errors[16];
#ifdef WINIOS_ENABLE_NEPTUNE
    HvNeptune *neptune;
    pthread_t neptune_owner;
    bool neptune_reset_pending;
    void *native_texture;
    HvNptScanout native;
    ProducerProbe probe;
    NeptuneReply deferred[HV_NPT_MAX_FENCES];
    uint64_t neptune_submits, neptune_completed, neptune_fence_errors;
#endif
};

typedef struct DisplayInfo {
    Header hdr;
    struct { Rect rect; uint32_t enabled, flags; } modes[16];
} DisplayInfo;

typedef struct EdidReply {
    Header hdr;
    uint32_t size, pad;
    uint8_t data[1024];
} EdidReply;

typedef struct CursorCommand {
    Header hdr;
    uint32_t scanout, x, y, pos_pad, resource, hot_x, hot_y, pad;
} CursorCommand;

_Static_assert(sizeof(Header) == 24 && sizeof(Entry) == 16, "VirtIO wire layouts");
_Static_assert(sizeof(DisplayInfo) == 408 && sizeof(EdidReply) == 1056 &&
               sizeof(CursorCommand) == 56, "VirtIO reply and cursor layouts");

static Resource *lookup(HvVirtioGpu *gpu, uint32_t id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < RESOURCES; i++)
        if (gpu->resources[i].id == id) return &gpu->resources[i];
    return NULL;
}

static uint64_t segment_capacity(const HvVirtqElem *elem, int writable) {
    uint64_t size = 0;
    for (int i = 0; i < elem->nseg; i++)
        if (elem->seg[i].write == writable) size += elem->seg[i].len;
    return size;
}

static bool fits(const Rect *r, const Resource *res) {
    return r->width && r->height && r->x < res->width && r->y < res->height &&
           r->width <= res->width - r->x && r->height <= res->height - r->y;
}

static void detach(HvVirtioGpu *gpu, Resource *res) {
    gpu->allocated -= sizeof(Entry) * res->count;
    free(res->backing);
    res->backing = NULL;
    res->count = 0;
    res->backing_bytes = 0;
}

static void clear_scanout(HvVirtioGpu *gpu) {
    hv_fb_display_set_scanout(gpu->fb, NULL, 0, 0);
#ifdef WINIOS_ENABLE_NEPTUNE
    if (gpu->native_texture) hv_neptune_scanout_release(gpu->neptune,gpu->native_texture);
    gpu->native_texture=NULL;
    gpu->native=(HvNptScanout){0};
#endif
    free(gpu->screen.front);
    gpu->screen = (Scanout){0};
}

static void release(HvVirtioGpu *gpu, Resource *res) {
    if (gpu->screen.resource == res->id) clear_scanout(gpu);
    detach(gpu, res);
    gpu->allocated -= res->size;
    free(res->pixels);
    *res = (Resource){0};
}

static uint32_t create_2d(HvVirtioGpu *gpu, uint32_t id, uint32_t format, uint32_t width, uint32_t height) {
    if (!id || lookup(gpu, id)) return ERR_RESOURCE;
    if (format != BGRA && format != BGRX && format != RGBA && format != RGBX) return ERR_PARAMETER;
    if (!width || !height || width > 4096 || height > 4096) return ERR_PARAMETER;
    size_t size = (size_t)width * height * 4;
    if (size > SURFACE_LIMIT) return ERR_PARAMETER;
    if (size > RESOURCE_BUDGET - gpu->allocated) return ERR_OOM;
    for (unsigned i = 0; i < RESOURCES; i++) {
        if (gpu->resources[i].id) continue;
        uint8_t *pixels = calloc(1, size);
        if (!pixels) return ERR_OOM;
        gpu->resources[i] = (Resource){.id=id, .format=format, .width=width, .height=height,
                                      .size=size, .pixels=pixels};
        gpu->allocated += size;
        return OK_NODATA;
    }
    return ERR_OOM;
}

static uint32_t attach(HvVirtioGpu *gpu, Resource *res, uint32_t count, const HvVirtqElem *elem) {
    const uint32_t entry_offset = sizeof(Header) + 8;
    if (res->backing || !count || count > BACKING_ENTRIES) return ERR_PARAMETER;
    uint32_t bytes = count * sizeof(Entry);
    if (segment_capacity(elem, 0) < entry_offset + bytes) return ERR_PARAMETER;
    if (bytes > RESOURCE_BUDGET - gpu->allocated) return ERR_OOM;
    Entry *entries = malloc(bytes);
    if (!entries) return ERR_OOM;
    if (hv_virtq_read(&gpu->vio, elem, entry_offset, entries, bytes) != bytes) {
        free(entries);
        return ERR_PARAMETER;
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        const Entry *entry = &entries[i];
        if (!entry->len || entry->len > SURFACE_LIMIT - total ||
            !hv_virtio_dma_ptr(&gpu->vio, entry->addr, entry->len)) {
            free(entries);
            return ERR_PARAMETER;
        }
        total += entry->len;
    }
    res->backing = entries;
    res->count = count;
    res->backing_bytes = total;
    gpu->allocated += bytes;
    return OK_NODATA;
}

static uint32_t transfer(HvVirtioGpu *gpu, Resource *res, const Rect *rect, uint64_t offset) {
    if (!fits(rect, res) || !res->backing) return ERR_PARAMETER;
    uint32_t stride = res->width * 4, width = rect->width * 4;
    uint64_t extent = (uint64_t)(rect->height - 1) * stride + width;
    if (offset > res->backing_bytes || extent > res->backing_bytes - offset) return ERR_PARAMETER;

    /* Walk scatter entries monotonically across rows. The backing list is a private validated copy. */
    unsigned i = 0;
    uint64_t base = 0;
    for (unsigned y = 0; y < rect->height; y++) {
        uint64_t position = offset + (uint64_t)y * stride;
        uint8_t *dst = res->pixels + (size_t)(rect->y + y) * stride + rect->x * 4;
        for (uint32_t left = width; left;) {
            while (position >= base + res->backing[i].len) base += res->backing[i++].len;
            const Entry *entry = &res->backing[i];
            uint32_t skip = (uint32_t)(position - base);
            uint32_t size = entry->len - skip;
            if (size > left) size = left;
            const void *src = hv_virtio_dma_ptr(&gpu->vio, entry->addr + skip, size);
            if (!src) return ERR_PARAMETER;
            memcpy(dst, src, size);
            position += size;
            dst += size;
            left -= size;
        }
    }
    return OK_NODATA;
}

static void bgra_copy(uint8_t *dst, const uint8_t *src, uint32_t pixels, uint32_t format) {
    if (format == BGRA || format == BGRX) memcpy(dst, src, pixels * 4u);
    else for (uint32_t i = 0; i < pixels * 4u; i += 4) {
        dst[i] = src[i + 2];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i];
        dst[i + 3] = src[i + 3];
    }
    if (format == BGRX || format == RGBX)
        for (uint32_t i = 0; i < pixels * 4u; i += 4) dst[i + 3] = 255;
}

static void update_front(Scanout *screen, const Resource *res, const Rect *r) {
    const Rect *s = &screen->rect;
    uint32_t x = r->x > s->x ? r->x : s->x;
    uint32_t y = r->y > s->y ? r->y : s->y;
    uint32_t x_end = r->x + r->width < s->x + s->width ? r->x + r->width : s->x + s->width;
    uint32_t y_end = r->y + r->height < s->y + s->height ? r->y + r->height : s->y + s->height;
    if (x >= x_end || y >= y_end) return;
    for (uint32_t row = y; row < y_end; row++) {
        uint8_t *dst = screen->front + ((size_t)(row - s->y) * s->width + x - s->x) * 4;
        const uint8_t *src = res->pixels + ((size_t)row * res->width + x) * 4;
        bgra_copy(dst, src, x_end - x, res->format);
    }
}

static uint8_t *new_snapshot(const Scanout *screen) {
    return malloc((size_t)screen->rect.width * screen->rect.height * 4);
}

static void publish(HvVirtioGpu *gpu, uint8_t *pixels) {
    const Scanout *s = &gpu->screen;
    const Cursor *cursor = &gpu->cursor;
    uint32_t w = s->rect.width, h = s->rect.height;
    memcpy(pixels, s->front, (size_t)w * h * 4);
    if (cursor->visible) {
        int64_t x = (int64_t)cursor->x - cursor->hot_x, y = (int64_t)cursor->y - cursor->hot_y;
        for (unsigned row = 0; row < CURSOR_SIZE; row++) {
            if (y + row < 0 || y + row >= h) continue;
            for (unsigned col = 0; col < CURSOR_SIZE; col++) {
                if (x + col < 0 || x + col >= w) continue;
                const uint8_t *source = cursor->pixels + (row * CURSOR_SIZE + col) * 4;
                uint8_t *dest = pixels + ((size_t)(y + row) * w + x + col) * 4;
                unsigned alpha = source[3];
                for (unsigned channel = 0; channel < 3; channel++)
                    dest[channel] = (uint8_t)((source[channel] * alpha + dest[channel] * (255 - alpha) + 127) / 255);
            }
        }
    }
    hv_fb_display_set_scanout(gpu->fb, pixels, w, h);
    gpu->publishes++;
}

static uint32_t set_scanout(HvVirtioGpu *gpu, uint32_t scanout, uint32_t id, const Rect *rect) {
    if (scanout) return ERR_SCANOUT;
    if (!id) {
        clear_scanout(gpu);
        return OK_NODATA;
    }
    Resource *res = lookup(gpu, id);
    if (!res) return ERR_RESOURCE;
    if (!fits(rect, res)) return ERR_PARAMETER;
    Scanout next = {.resource = id, .rect = *rect};
    /* Drop the old committed front first to keep peak snapshots within the budget. */
    clear_scanout(gpu);
    next.front = new_snapshot(&next);
    uint8_t *pixels = next.front ? new_snapshot(&next) : NULL;
    if (!pixels) {
        free(next.front);
        return ERR_OOM;
    }
    update_front(&next, res, rect);
    gpu->screen = next;
    publish(gpu, pixels);
    return OK_NODATA;
}

static uint32_t flush(HvVirtioGpu *gpu, const Resource *res, const Rect *rect) {
    if (!fits(rect, res)) return ERR_PARAMETER;
    if (gpu->screen.resource != res->id) return OK_NODATA;
    uint8_t *pixels = new_snapshot(&gpu->screen);
    if (!pixels) return ERR_OOM;
    update_front(&gpu->screen, res, rect);
    publish(gpu, pixels);
    return OK_NODATA;
}

static void record_error(HvVirtioGpu *gpu,const Header *hdr,uint32_t response,uint64_t length) {
    if (response<ERR_UNSPEC) return;
    gpu->command_errors++;
    for (unsigned i=0;i<16;++i) {
        if (!gpu->errors[i].count || (gpu->errors[i].opcode==hdr->type && gpu->errors[i].response==response)) {
            gpu->errors[i].opcode=hdr->type;
            gpu->errors[i].response=response;
            gpu->errors[i].flags=hdr->flags;
            gpu->errors[i].context=hdr->context;
            gpu->errors[i].length=(uint32_t)length;
            gpu->errors[i].count++;
            break;
        }
    }
}

static uint32_t control(HvVirtioGpu *gpu, const HvVirtqElem *elem) {
    struct { Header hdr; uint8_t body[32]; } req = {0};
    union { Header hdr; DisplayInfo display; EdidReply edid; } reply = {0};
    uint32_t got = hv_virtq_read(&gpu->vio, elem, 0, &req, sizeof(req));
    uint64_t writable = segment_capacity(elem, 1);
    if (got < sizeof(Header) || writable < sizeof(Header)) return 0;
    if (req.hdr.flags & FENCE) {
        reply.hdr.flags = FENCE;
        reply.hdr.fence = req.hdr.fence;
        reply.hdr.context = req.hdr.context;
    }
    reply.hdr.type = ERR_PARAMETER;
    uint32_t bytes = sizeof(Header);
    uint32_t body[8];
    memcpy(body, req.body, sizeof(body));
    Rect rect;
    memcpy(&rect, body, sizeof(rect));
    Resource *res;
    if (req.hdr.flags & ~FENCE || req.hdr.context) goto done;
    got -= sizeof(Header);
    switch (req.hdr.type) {
    case GET_DISPLAY_INFO:
        if (writable < sizeof(DisplayInfo)) break;
        reply.display.modes[0].enabled = 1;
        reply.display.modes[0].rect = (Rect){0, 0, gpu->preferred_width, gpu->preferred_height};
        reply.hdr.type = OK_DISPLAY_INFO;
        bytes = sizeof(DisplayInfo);
        break;
    case GET_EDID: {
        bool neptune=false;
#ifdef WINIOS_ENABLE_NEPTUNE
        neptune=gpu->neptune!=NULL;
#endif
        if (got < 8 || writable < sizeof(EdidReply) ||
            !hv_gpu_edid_available(gpu->vio.features,gpu->vio.driver_features,neptune)) break;
        if (body[0]) { reply.hdr.type = ERR_SCANOUT; break; }
        reply.hdr.type = OK_EDID;
        reply.edid.size = 128;
        hv_gpu_mode_edid(reply.edid.data);
        bytes = sizeof(EdidReply);
        if (!(gpu->vio.driver_features & EDID_FEATURE)) gpu->edid_compat++;
        break;
    }
    case RESOURCE_CREATE_2D:
        if (got >= 16) reply.hdr.type = create_2d(gpu, body[0], body[1], body[2], body[3]);
        break;
    case SET_SCANOUT:
        if (got >= 24) reply.hdr.type = set_scanout(gpu, body[4], body[5], &rect);
        break;
    case RESOURCE_FLUSH:
        if (got < 24) break;
        res = lookup(gpu, body[4]);
        reply.hdr.type = res ? flush(gpu, res, &rect) : ERR_RESOURCE;
        break;
    case TRANSFER_TO_HOST_2D:
        if (got < 32) break;
        res = lookup(gpu, body[6]);
        reply.hdr.type = res ? transfer(gpu, res, &rect, (uint64_t)body[5] << 32 | body[4]) : ERR_RESOURCE;
        break;
    case RESOURCE_ATTACH_BACKING:
    case RESOURCE_DETACH_BACKING:
    case RESOURCE_UNREF:
        if (got < 8) break;
        res = lookup(gpu, body[0]);
        if (!res) { reply.hdr.type = ERR_RESOURCE; break; }
        if (req.hdr.type == RESOURCE_ATTACH_BACKING) reply.hdr.type = attach(gpu, res, body[1], elem);
        else {
            if (req.hdr.type == RESOURCE_UNREF) release(gpu, res);
            else detach(gpu, res);
            reply.hdr.type = OK_NODATA;
        }
        break;
    case GET_CAPSET_INFO:
    case GET_CAPSET:
        break; /* zero capsets, including no 3D contexts */
    default:
        reply.hdr.type = ERR_UNSPEC;
        break;
    }
done:
    record_error(gpu,&req.hdr,reply.hdr.type,segment_capacity(elem,0));
    return hv_virtq_write(&gpu->vio, elem, 0, &reply, bytes);
}

static void cursor_command(HvVirtioGpu *gpu, const HvVirtqElem *elem) {
    CursorCommand c;
    if (hv_virtq_read(&gpu->vio, elem, 0, &c, sizeof(c)) != sizeof(c) || c.scanout ||
        c.hdr.context || c.hdr.flags) return;
    Cursor *cursor = &gpu->cursor;
    if (c.hdr.type == UPDATE_CURSOR) {
        if (c.resource) {
            const Resource *res = lookup(gpu, c.resource);
            if (!res || res->width != CURSOR_SIZE || res->height != CURSOR_SIZE ||
                c.hot_x >= CURSOR_SIZE || c.hot_y >= CURSOR_SIZE) return;
            bgra_copy(cursor->pixels, res->pixels, CURSOR_SIZE * CURSOR_SIZE, res->format);
            cursor->hot_x = c.hot_x;
            cursor->hot_y = c.hot_y;
        }
        cursor->visible = c.resource != 0;
    } else if (c.hdr.type != MOVE_CURSOR) return;
    cursor->x = c.x;
    cursor->y = c.y;
    if (!gpu->screen.front) return;
    uint8_t *pixels = new_snapshot(&gpu->screen);
    if (pixels) publish(gpu, pixels);
}

#ifdef WINIOS_ENABLE_NEPTUNE
/* BAR 0/1 is free (MSI-X is BAR2, modern virtio config BAR4). It is
 * advertised as host-visible shared memory only in the lab candidate.
 * Resolve the live BAR on every CPU access, never retain a TLB pointer. */
static void *neptune_cpu_ptr(void *opaque,uint64_t pa,uint64_t bytes) {
    HvVirtioGpu *gpu=opaque;
    HvPciDev *d=&gpu->vio.pci;
    HvPcie *bus=d->bus;
    uint64_t base=hv_pci_bar_addr(d,0);
    if (!gpu->neptune || gpu->neptune_reset_pending || !bytes || !hv_pci_mem_enabled(d) || !base ||
        base<bus->high_mmio_base || base-bus->high_mmio_base>=bus->high_mmio_size ||
        HV_NPT_APERTURE_BYTES>bus->high_mmio_size-(base-bus->high_mmio_base) ||
        pa<base || pa-base>=HV_NPT_APERTURE_BYTES ||
        bytes>HV_NPT_APERTURE_BYTES-(pa-base)) return NULL;
    /* Refuse overlaps with any other enabled BAR instead of intercepting its MMIO. */
    for (int slot=0;slot<HV_PCI_SLOTS;++slot) {
        HvPciDev *other=bus->dev[slot];
        if (!other || !hv_pci_mem_enabled(other)) continue;
        for (int bar=0;bar<6;++bar) {
            if (other==d && bar==0) continue;
            uint64_t len=other->bar_size[bar], start=hv_pci_bar_addr(other,bar);
            if (len && start && start<base+HV_NPT_APERTURE_BYTES &&
                base<start+len && start+len>=start) return NULL;
        }
    }
    return hv_neptune_aperture_ptr(gpu->neptune,pa-base,bytes);
}

static uint32_t neptune_scanout_command(HvVirtioGpu *gpu,const HvVirtqElem *elem,const Header *hdr) {
    Header response=*hdr;
    response.type=ERR_PARAMETER;
    uint32_t body[18]={0};
    uint64_t got=segment_capacity(elem,0);
    if ((hdr->flags&~FENCE) || hdr->context) goto out;
    if (hdr->type==SET_SCANOUT) {
        if (got!=sizeof(Header)+24 ||
            hv_virtq_read(&gpu->vio,elem,sizeof(Header),body,24)!=24) goto out;
        if (body[4]) { response.type=ERR_SCANOUT; goto out; }
        if (!body[5]) { clear_scanout(gpu); response.type=OK_NODATA; goto out; }
        if (body[0] || body[1] || !body[2] || !body[3]) goto out;
        void *tex=hv_neptune_typed_scanout(gpu->neptune,body[5],body[2],body[3]);
        if (!tex) { response.type=ERR_UNSPEC; goto out; }
        clear_scanout(gpu);
        gpu->native_texture=tex;
        gpu->native=(HvNptScanout){.resource=body[5], .width=body[2], .height=body[3],
                                  .stride=body[2]*4};
        hv_fb_display_set_native(gpu->fb,tex,body[2],body[3]);
        gpu->publishes++;
        response.type=OK_NODATA;
    } else if (hdr->type==0x10d) {
        /* SET_SCANOUT_BLOB: rect, scanout, resource, width/height, format,
         * padding, four strides and four offsets. Initially full-frame only. */
        if (got!=sizeof(Header)+sizeof(body) ||
            hv_virtq_read(&gpu->vio,elem,sizeof(Header),body,sizeof(body))!=sizeof(body)) goto out;
        if (body[4]) { response.type=ERR_SCANOUT; goto out; }
        if (!body[5]) { clear_scanout(gpu); response.type=OK_NODATA; goto out; }
        if (body[0] || body[1] || body[2]!=body[6] || body[3]!=body[7] ||
            body[11] || body[12] || body[13] || body[15] || body[16] || body[17]) goto out;
        void *tex=hv_neptune_scanout_texture(gpu->neptune,body[5],body[6],body[7],body[8],body[10],body[14]);
        if (!tex) { response.type=ERR_UNSPEC; goto out; }
        clear_scanout(gpu);
        gpu->native_texture=tex;
        gpu->native=(HvNptScanout){.resource=body[5], .width=body[6], .height=body[7],
                                  .stride=body[10], .offset=body[14], .blob_command=true};
        hv_fb_display_set_native(gpu->fb,tex,body[6],body[7]);
        gpu->publishes++;
        response.type=OK_NODATA;
    } else if (hdr->type==RESOURCE_FLUSH) {
        if (got!=sizeof(Header)+24 ||
            hv_virtq_read(&gpu->vio,elem,sizeof(Header),body,24)!=24) goto out;
        if (body[4]!=gpu->native.resource || !gpu->native_texture) { response.type=ERR_RESOURCE; goto out; }
        if (!body[2] || !body[3] || body[0]>=gpu->native.width || body[1]>=gpu->native.height ||
            body[2]>gpu->native.width-body[0] || body[3]>gpu->native.height-body[1]) goto out;
        hv_fb_display_native_dirty(gpu->fb);
        gpu->publishes++;
        response.type=OK_NODATA;
    }
out:
    record_error(gpu,hdr,response.type,got);
    return hv_virtq_write(&gpu->vio,elem,0,&response,sizeof(response));
}

static uint32_t neptune_backing_command(HvVirtioGpu *gpu,const HvVirtqElem *elem,const Header *hdr,bool *deferred) {
    Header response=*hdr; response.type=ERR_PARAMETER;
    uint32_t body[12]={0};
    uint64_t length=segment_capacity(elem,0);
    if (segment_capacity(elem,1)<sizeof(response)) return 0;
    bool transfer3d=hdr->type==0x205 || hdr->type==0x206;
    if ((hdr->flags&~(FENCE|2u)) || ((hdr->flags&2u) && (!(hdr->flags&FENCE) || !hdr->context || !transfer3d)) ||
        (!(hdr->flags&2u) && hdr->ring)) goto done;
    NeptuneReply *slot=NULL;
    if (transfer3d && (hdr->flags&FENCE)) {
        for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) if (!gpu->deferred[i].ticket) { slot=&gpu->deferred[i]; break; }
        if (!slot) { response.type=ERR_OOM; goto done; }
    }
    if (hdr->type==RESOURCE_ATTACH_BACKING) {
        if (length<32 || hv_virtq_read(&gpu->vio,elem,24,body,8)!=8 ||
            !body[1] || body[1]>8192 || length!=32+(uint64_t)body[1]*16) goto done;
        HvNptBacking *entries=calloc(body[1],sizeof(*entries));
        if (!entries) { response.type=ERR_OOM; goto done; }
        bool valid=true;
        for (uint32_t i=0;i<body[1];++i) {
            Entry e;
            if (hv_virtq_read(&gpu->vio,elem,32+i*16,&e,16)!=16 ||
                !hv_virtio_dma_ptr(&gpu->vio,e.addr,e.len) || !e.len) { valid=false; break; }
            entries[i]=(HvNptBacking){.pa=e.addr,.len=e.len};
        }
        if (valid && !hv_neptune_attach_backing(gpu->neptune,gpu->vio.m,body[0],entries,body[1]))
            response.type=OK_NODATA;
        free(entries);
    } else if (hdr->type==RESOURCE_DETACH_BACKING) {
        if (length==32 && hv_virtq_read(&gpu->vio,elem,24,body,8)==8 &&
            !hv_neptune_detach_backing(gpu->neptune,body[0])) response.type=OK_NODATA;
    } else if (hdr->type==0x205 || hdr->type==0x206) {
        if (length!=72 || hv_virtq_read(&gpu->vio,elem,24,body,48)!=48) goto done;
        uint64_t offset=(uint64_t)body[7]<<32|body[6];
        if (!hv_neptune_transfer(gpu->neptune,gpu->vio.m,body[8],hdr->context,body,offset,body[9],body[10],body[11],hdr->type==0x205))
            response.type=OK_NODATA;
    } else if (hdr->type==TRANSFER_TO_HOST_2D) {
        if (length!=56 || hv_virtq_read(&gpu->vio,elem,24,body,32)!=32) goto done;
        uint32_t box[6]={body[0],body[1],0,body[2],body[3],1};
        uint64_t offset=(uint64_t)body[5]<<32|body[4];
        if (!hv_neptune_transfer(gpu->neptune,gpu->vio.m,body[6],hdr->context,box,offset,0,0,0,true))
            response.type=OK_NODATA;
    }
done:
    if (response.type==OK_NODATA && transfer3d && (hdr->flags&FENCE)) {
        uint64_t ticket=0;
        int rc=(hdr->flags&2u)?
            hv_neptune_fence_submit(gpu->neptune,hdr->context,hdr->ring,hdr->fence,&ticket):
            hv_neptune_fence_submit_legacy(gpu->neptune,hdr->context,hdr->fence,&ticket);
        if (!rc && ticket && slot) {
            slot->ticket=ticket; slot->opcode=hdr->type; slot->elem=*elem;
            slot->bytes=sizeof(response); memcpy(slot->response,&response,sizeof(response));
            *deferred=true; gpu->neptune_submits++;
            return 0;
        }
        response.type=ERR_UNSPEC;
    }
    record_error(gpu,hdr,response.type,length);
    return hv_virtq_write(&gpu->vio,elem,0,&response,sizeof(response));
}

static bool neptune_control(HvVirtioGpu *gpu,const HvVirtqElem *elem,uint32_t *written,bool *deferred) {
    Header hdr={0};
    HvVirtio *v=&gpu->vio;
    if (!gpu->neptune || (v->driver_features & 0x19)!=0x19 ||
        hv_virtq_read(v,elem,0,&hdr,sizeof(hdr))!=sizeof(hdr)) return false;
    if (hdr.type==RESOURCE_ATTACH_BACKING || hdr.type==RESOURCE_DETACH_BACKING || hdr.type==TRANSFER_TO_HOST_2D || hdr.type==0x205 || hdr.type==0x206) {
        uint32_t id=0,offset=hdr.type==TRANSFER_TO_HOST_2D?48:(hdr.type==0x205 || hdr.type==0x206)?56:24;
        if (hv_virtq_read(v,elem,offset,&id,4)==4 && hv_neptune_blob_exists(gpu->neptune,id)) {
            *written=neptune_backing_command(gpu,elem,&hdr,deferred); return true;
        }
    }
    if (hdr.type==SET_SCANOUT) {
        uint32_t id=0;
        if (hv_virtq_read(v,elem,sizeof(hdr)+20,&id,4)==4 &&
            ((id && hv_neptune_blob_exists(gpu->neptune,id)) || (!id && gpu->native.resource))) {
            *written=neptune_scanout_command(gpu,elem,&hdr); return true;
        }
    }
    if (hdr.type==0x10d) {
        *written=neptune_scanout_command(gpu,elem,&hdr);
        return true;
    }
    if (hdr.type==RESOURCE_FLUSH && gpu->native.resource) {
        uint32_t id=0;
        if (hv_virtq_read(v,elem,sizeof(hdr)+16,&id,4)==4 && id==gpu->native.resource) {
            *written=neptune_scanout_command(gpu,elem,&hdr);
            return true;
        }
    }
    bool route=(hdr.type>=0x200 && hdr.type<=0x209) || hdr.type==GET_CAPSET_INFO ||
        hdr.type==GET_CAPSET || hdr.type==0x10c;
    if (hdr.type==RESOURCE_CREATE_2D) {
        uint32_t id=0;
        if (hv_virtq_read(v,elem,24,&id,4)==4 && hv_neptune_blob_exists(gpu->neptune,id)) {
            hdr.type=ERR_RESOURCE; *written=hv_virtq_write(v,elem,0,&hdr,sizeof(hdr)); return true;
        }
    }
    if (hdr.type==RESOURCE_UNREF) {
        uint32_t id=0;
        if (hv_virtq_read(v,elem,sizeof(hdr),&id,4)==4)
            route=hv_neptune_blob_exists(gpu->neptune,id);
        /* Clear only after successful unref (see below). */
    }
    if (!route) return false;
    uint64_t length=segment_capacity(elem,0), capacity=segment_capacity(elem,1);
    if (length<sizeof(hdr) || length>HV_NPT_MAX_SUBMIT+32 || capacity<sizeof(hdr)) return true;
    NeptuneReply *slot=NULL;
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) if (!gpu->deferred[i].ticket) { slot=&gpu->deferred[i]; break; }
    uint8_t reply[24+HV_NPT_MAX_CAPSET]={0};
    uint64_t ticket=0;
    size_t n=0;
    if (!slot) {
        Header error=hdr; error.type=ERR_OOM;
        memcpy(reply,&error,sizeof(error)); n=sizeof(error);
    } else {
        uint8_t *request=malloc((size_t)length);
        if (!request) {
            Header error=hdr; error.type=ERR_OOM;
            memcpy(reply,&error,sizeof(error)); n=sizeof(error);
        } else {
            if (hv_virtq_read(v,elem,0,request,(uint32_t)length)==length) {
                /* Keep the resource namespace common across 2D and 3D. */
                uint32_t id=0;
                if ((hdr.type==0x10c || hdr.type==0x204) && length>=28) memcpy(&id,request+24,4);
                if (id && lookup(gpu,id)) {
                    Header error=hdr; error.type=ERR_RESOURCE;
                    memcpy(reply,&error,sizeof(error)); n=sizeof(error);
                } else {
                    /* Invalidate any exclusive reservation before map/unmap or resource replacement. */
                    if (hdr.type==0x208 || hdr.type==0x209 || hdr.type==RESOURCE_UNREF || hdr.type==0x10c)
                        __atomic_add_fetch(&v->m->cpu_aperture_epoch,1,__ATOMIC_RELEASE);
                    n=hv_neptune_wire_command(gpu->neptune,request,(size_t)length,reply,
                    capacity<sizeof(reply)?(size_t)capacity:sizeof(reply),&ticket);
                }
            }
            free(request);
        }
    }
    if (ticket) {
        /* Currently only SUBMIT can defer, whose response is a header. */
        if (n!=sizeof(Header)) { v->status|=64; return true; }
        slot->ticket=ticket;
        slot->opcode=hdr.type;
        slot->elem=*elem;
        slot->bytes=(uint32_t)n;
        memcpy(slot->response,reply,n);
        *deferred=true;
        gpu->neptune_submits++;
    } else if (n) {
        uint32_t type=0; memcpy(&type,reply,4);
        record_error(gpu,&hdr,type,length);
        if (type==OK_NODATA && hdr.type==RESOURCE_UNREF && gpu->native.resource) {
            uint32_t id=0;
            if (hv_virtq_read(v,elem,sizeof(hdr),&id,4)==4 && id==gpu->native.resource) clear_scanout(gpu);
        }
        *written=hv_virtq_write(v,elem,0,reply,(uint32_t)n);
    }
    return true;
}

static void neptune_retire(HvVirtioGpu *gpu) {
    if (!gpu->neptune) return;
    hv_neptune_poll(gpu->neptune);
    bool interrupt=false;
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) {
        NeptuneReply *p=&gpu->deferred[i];
        if (!p->ticket) continue;
        HvNeptuneFence fence;
        int result=hv_neptune_fence_take(gpu->neptune,p->ticket,&fence);
        if (!result) continue;
        Header response; memcpy(&response,p->response,sizeof(response));
        if (result<0 || fence.context!=response.context || fence.ring!=response.ring || fence.guest_id!=response.fence) {
            response.type=ERR_UNSPEC;
            memcpy(p->response,&response,sizeof(response));
            Header failed=response; failed.type=p->opcode;
            record_error(gpu,&failed,ERR_UNSPEC,0);
            gpu->neptune_fence_errors++;
        }
        uint32_t written=hv_virtq_write(&gpu->vio,&p->elem,0,p->response,p->bytes);
        hv_virtq_push(&gpu->vio,0,&p->elem,written);
        gpu->completed[0]++;
        gpu->neptune_completed++;
        /* A GPU completion can change the current native texture in place.
         * Re-sample after retirement even without a separate FLUSH. */
        if (result>0 && response.type==OK_NODATA && gpu->native_texture)
            hv_fb_display_native_dirty(gpu->fb);
        *p=(NeptuneReply){0};
        interrupt=true;
    }
    if (interrupt) hv_virtio_queue_interrupt(&gpu->vio,0);
}

static void probe_producer(HvVirtioGpu *gpu) {
    ProducerProbe *p = &gpu->probe;
    if (!p->enabled || !gpu->native_texture || gpu->pending[0] ||
        p->attempts >= HV_NPT_PROBE_TILES) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return;
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    if (now_ms < p->next_ms) return;
    for (size_t i = 0; i < HV_NPT_MAX_FENCES; ++i)
        if (gpu->deferred[i].ticket) return;

    /* This probe can synchronize with GLES. Keep it off the ordinary
     * execution path and sample only an idle control queue. */
    p->next_ms = now_ms + 1000;
    bool black;
    p->last_error = hv_neptune_probe_scanout(gpu->neptune, &gpu->native, p->attempts, &black);
    if (!p->attempts) p->first_resource = gpu->native.resource;
    p->last_resource = gpu->native.resource;
    p->attempts++;
    if (p->last_error) p->errors++;
    else if (black) p->black++;
    else p->last_nonblack_resource = gpu->native.resource;
}
#endif

void hv_virtio_gpu_set_cpu_readback(HvVirtioGpu *gpu, bool enabled) {
#ifdef WINIOS_ENABLE_NEPTUNE
    if (!gpu || gpu->probe.enabled == enabled) return;
    if (enabled) gpu->probe = (ProducerProbe){0};
    gpu->probe.enabled = enabled;
#else
    (void)gpu; (void)enabled;
#endif
}

void hv_virtio_gpu_start_neptune(HvVirtioGpu *gpu,int ncpus) {
#ifdef WINIOS_ENABLE_NEPTUNE
    /* Explicit lab override only. Driver v0.6 still needs the VirGL shadow
     * context and scanout integration; do not expose this in ordinary UI. */
    const char *enabled=getenv("WINIOS_NEPTUNE_LAB");
    if (!gpu || ncpus<1 || ncpus>HV_MAX_CPUS || !enabled || strcmp(enabled,"1") || gpu->neptune) return;
    gpu->neptune=hv_neptune_open(getenv("WINIOS_NEPTUNE_RENDERER"),getenv("WINIOS_NEPTUNE_DXMT"));
    if (!gpu->neptune) { fprintf(stderr,"winvm: Neptune lab renderer unavailable; leaving 2D adapter\n"); return; }
    gpu->neptune_owner=pthread_self();
    HvVirtio *v=&gpu->vio;
    hv_pci_add_bar_flags(&v->pci,0,HV_NPT_APERTURE_BYTES,1,1);
    /* VirtIO PCI shared-memory capability, cfg_type=8, id=1, 64-bit length.
     * MSI-X ends at 0x94; prepend this read-only capability at 0xa0. */
    uint8_t *cap=v->pci.config+0xa0;
    cap[0]=9; cap[1]=v->pci.config[0x34]; cap[2]=24; cap[3]=8; cap[4]=0; cap[5]=1;
    uint32_t lo=(uint32_t)HV_NPT_APERTURE_BYTES,hi=(uint32_t)(HV_NPT_APERTURE_BYTES>>32);
    memcpy(cap+12,&lo,4); memcpy(cap+20,&hi,4);
    v->pci.config[0x34]=0xa0;
    v->features|=0x19; /* VIRGL transport, RESOURCE_BLOB, CONTEXT_INIT */
    v->config[12]=2; /* Actual Neptune and VirGL2 capsets, probed before activation. */
    v->m->cpu_aperture_opaque=gpu;
    v->m->cpu_aperture_ptr=neptune_cpu_ptr;
    fprintf(stderr,"winvm: Neptune SMP LAB transport enabled (%d vCPUs)\n",ncpus);
#else
    (void)gpu; (void)ncpus;
#endif
}

static void reset_resources(HvVirtioGpu *gpu);
static void process_queue(HvVirtio *vio, int queue);

static void notify(HvVirtio *vio, int queue) {
#ifdef WINIOS_ENABLE_NEPTUNE
    HvVirtioGpu *gpu=vio->dev;
    if (gpu->neptune) {
        gpu->pending[queue]=true;
        return;
    }
#endif
    process_queue(vio,queue);
}

static void process_queue(HvVirtio *vio, int queue) {
    HvVirtioGpu *gpu = vio->dev;
    bool completed = false;
    gpu->pending[queue] = true;
    for (unsigned n = 0; n < REQUESTS_PER_POLL; n++) {
        HvVirtqElem elem;
        int result = hv_virtq_pop(vio, queue, &elem);
        if (!result) { gpu->pending[queue] = false; break; }
        uint32_t written = 0;
        if (result > 0) {
            if (!queue) {
#ifdef WINIOS_ENABLE_NEPTUNE
                bool deferred=false;
                if (neptune_control(gpu,&elem,&written,&deferred)) {
                    if (deferred) continue;
                } else
#endif
                written = control(gpu, &elem);
            } else cursor_command(gpu, &elem);
        }
        if (result < 0 || (!queue && !written)) gpu->malformed++;
        hv_virtq_push(vio, queue, &elem, written);
        gpu->completed[queue]++;
        completed = true;
    }
    if (completed) hv_virtio_queue_interrupt(vio, queue);
}

void hv_virtio_gpu_poll(HvVirtioGpu *gpu) {
    if (!gpu) return;
#ifdef WINIOS_ENABLE_NEPTUNE
    if (gpu->neptune && gpu->neptune_reset_pending) {
        reset_resources(gpu);
        gpu->neptune_reset_pending=false;
    }
#endif
    if (!(gpu->vio.status & 4) || (gpu->vio.status & 0xc0)) return;
#ifdef WINIOS_ENABLE_NEPTUNE
    neptune_retire(gpu);
#endif
    for (int q = 0; q < QUEUES && !(gpu->vio.status & 64); q++)
        if (gpu->pending[q]) process_queue(&gpu->vio, q);
#ifdef WINIOS_ENABLE_NEPTUNE
    if (!(gpu->vio.status & 0xc0)) probe_producer(gpu);
#endif
}

static void reset_resources(HvVirtioGpu *gpu) {
    clear_scanout(gpu);
#ifdef WINIOS_ENABLE_NEPTUNE
    if (gpu->neptune) {
        memset(gpu->deferred,0,sizeof(gpu->deferred));
        __atomic_add_fetch(&gpu->vio.m->cpu_aperture_epoch,1,__ATOMIC_RELEASE);
        (void)hv_neptune_reset(gpu->neptune);
    }
#endif
    for (unsigned i = 0; i < RESOURCES; i++)
        if (gpu->resources[i].id) release(gpu, &gpu->resources[i]);
    gpu->cursor = (Cursor){0};
}

static void reset_device(HvVirtio *vio) {
    HvVirtioGpu *gpu=vio->dev;
    gpu->resets++;
    memset(gpu->pending,0,sizeof(gpu->pending));
#ifdef WINIOS_ENABLE_NEPTUNE
    if (gpu->neptune && !pthread_equal(pthread_self(),gpu->neptune_owner)) {
        memset(gpu->deferred,0,sizeof(gpu->deferred));
        gpu->neptune_reset_pending=true;
        __atomic_add_fetch(&vio->m->cpu_aperture_epoch,1,__ATOMIC_RELEASE);
        return;
    }
    gpu->neptune_reset_pending=false;
#endif
    reset_resources(gpu);
}

HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, HvFbDisplay *fb) {
    if (!fb) return NULL;
    HvVirtioGpu *gpu = calloc(1, sizeof(*gpu));
    if (!gpu) return NULL;
    HvVirtio *vio = &gpu->vio;
    hv_virtio_init(vio, m, 16, 0x038000, QUEUES, EDID_FEATURE);
    hv_pci_add_bar(&vio->pci, 2, 0x2000, 1);
    hv_pci_add_msix(&vio->pci, 0x88, 2, 0, 0x1000, 3);
    vio->pci.config[0x2e] = 0x00;
    vio->pci.config[0x2f] = 0x11; /* QEMU virtio-gpu subsystem used by the Windows INF. */
    vio->config_len = 16;
    vio->config[8] = 1; /* num_scanouts; events and num_capsets remain zero. */
    vio->dev = gpu;
    vio->strict_dma = 1;
    vio->notify = notify;
    vio->reset_device = reset_device;
    gpu->fb = fb;
    hv_gpu_preferred_mode(getenv("WINIOS_GPU_RESOLUTION"),&gpu->preferred_width,&gpu->preferred_height);
    if (hv_pcie_attach(bus, &vio->pci) < 0) {
        free(gpu);
        return NULL;
    }
    return gpu;
}

void hv_virtio_gpu_reset(HvVirtioGpu *gpu) {
    if (gpu) hv_virtio_reset(&gpu->vio);
}

void hv_virtio_gpu_destroy(HvVirtioGpu *gpu) {
    if (!gpu) return;
    hv_virtio_gpu_reset(gpu);
    gpu->vio.pci.bus->dev[gpu->vio.pci.slot] = NULL;
#ifdef WINIOS_ENABLE_NEPTUNE
    if (gpu->neptune) {
        gpu->vio.m->cpu_aperture_ptr=NULL;
        gpu->vio.m->cpu_aperture_opaque=NULL;
        hv_neptune_close(gpu->neptune);
    }
#endif
    free(gpu);
}

void hv_virtio_gpu_diagnostics(const HvVirtioGpu *gpu, char *buffer, size_t capacity) {
    if (!capacity) return;
    if (!gpu) {
        snprintf(buffer, capacity, "graphics: adapter=RAMFB\n");
        return;
    }
    unsigned resources = 0;
    for (unsigned i = 0; i < RESOURCES; i++) resources += gpu->resources[i].id != 0;
    size_t frame = (size_t)gpu->screen.rect.width * gpu->screen.rect.height * 4;
    const char *display=gpu->screen.front ? "virtio-scanout" : "RAMFB";
    uint32_t width=gpu->screen.rect.width,height=gpu->screen.rect.height;
#ifdef WINIOS_ENABLE_NEPTUNE
    if (gpu->native_texture) { display="Metal-native"; width=gpu->native.width; height=gpu->native.height; }
#endif
    snprintf(buffer, capacity,
             "graphics: adapter=%s display=%s status=0x%02x features=0x%" PRIx64
             " scanout=%ux%u host_cpu_bytes=%zu resources=%u\n"
             "graphics_queues: control_completed=%" PRIu64 " cursor_completed=%" PRIu64
             " control_pending=%u cursor_pending=%u malformed=%" PRIu64
             " command_errors=%" PRIu64 " scanout_updates=%" PRIu64 " resets=%" PRIu64 "\n"
             "edid: offered=%u negotiated=%u compat_served=%" PRIu64 "\n",
             
#ifdef WINIOS_ENABLE_NEPTUNE
             gpu->neptune ? "Neptune-LAB" : "VirtIO-GPU-2D",
#else
             "VirtIO-GPU-2D",
#endif
             display, (unsigned)gpu->vio.status, gpu->vio.driver_features,
             width, height, gpu->allocated + 2 * frame, resources,
             gpu->completed[0], gpu->completed[1], (unsigned)gpu->pending[0], (unsigned)gpu->pending[1],
             gpu->malformed, gpu->command_errors, gpu->publishes, gpu->resets,
             (unsigned)((gpu->vio.features & EDID_FEATURE)!=0),
             (unsigned)((gpu->vio.driver_features & EDID_FEATURE)!=0),gpu->edid_compat);
    size_t used=strlen(buffer);
#ifdef WINIOS_ENABLE_NEPTUNE
    if (gpu->neptune && used<capacity) {
        uint64_t bytes=0; unsigned count=hv_neptune_resource_count(gpu->neptune,&bytes);
        const HvNptScanout *scanout = &gpu->native;
        const ProducerProbe *p = &gpu->probe;
        const char *command = !scanout->resource ? "none" : scanout->blob_command ? "blob" : "typed";
        int n=snprintf(buffer+used,capacity-used,
            "neptune: resources=%u bytes=%" PRIu64 " fenced=%" PRIu64 " retired=%" PRIu64 " reply_errors=%" PRIu64 "\n"
            "scanout_producer: resource=%u command=%s stride=%u offset=%u enabled=%u"
            " attempts=%u black_tiles=%u nonblack_tiles=%u errors=%u last_rc=%d"
            " first_resource=%u last_resource=%u last_nonblack_resource=%u\n",
            count,bytes,gpu->neptune_submits,gpu->neptune_completed,gpu->neptune_fence_errors,
            scanout->resource,command,scanout->stride,scanout->offset,(unsigned)p->enabled,
            p->attempts,p->black,p->attempts-p->black-p->errors,p->errors,p->last_error,
            p->first_resource,p->last_resource,p->last_nonblack_resource);
        if (n>0) used+=(size_t)n;
        if (used<capacity) {
            hv_neptune_context_diagnostics(gpu->neptune,buffer+used,capacity-used);
            used+=strlen(buffer+used);
        }
    }
#endif
    for (unsigned i=0;i<16 && used<capacity;++i) if (gpu->errors[i].count) {
        int n=snprintf(buffer+used,capacity-used,"gpu_error: op=%04x resp=%04x count=%" PRIu64 " flags=%x ctx=%u len=%u\n",
            gpu->errors[i].opcode,gpu->errors[i].response,gpu->errors[i].count,
            gpu->errors[i].flags,gpu->errors[i].context,gpu->errors[i].length);
        if (n>0) used+=(size_t)n;
    }
}
