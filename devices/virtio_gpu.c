/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "virtio_gpu.h"

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

struct HvVirtioGpu {
    HvVirtio vio;
    HvFbDisplay *fb;
    Resource resources[RESOURCES];
    Scanout screen;
    Cursor cursor;
    size_t allocated;
    bool pending[QUEUES];
    uint64_t completed[QUEUES], malformed, command_errors, publishes, resets;
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

static void edid(uint8_t data[1024]) {
    static const uint8_t header[] = {0,255,255,255,255,255,255,0,0x5c,0x53,1,0,0,0,0,0,1,34,1,4,0x80,27,20,120,0x0a};
    static const uint8_t dtd[] = {0x64,0x19,0,0x40,0x41,0,0x26,0x30,0x18,0x88,0x36,0,0x0e,0xcb,0,0,0,0x18};
    memcpy(data, header, sizeof(header));
    data[35] = 0x21; /* established 640x480 and 800x600 at 60 Hz */
    data[36] = 0x08; /* established 1024x768 at 60 Hz */
    memset(data + 38, 1, 16);
    memcpy(data + 54, dtd, sizeof(dtd));
    data[75] = 0xfc;
    memcpy(data + 77, "WindowsiOS\n  ", 13);
    for (unsigned pos = 90; pos <= 108; pos += 18) data[pos + 3] = 0x10;
    for (unsigned i = 0; i < 127; i++) data[127] -= data[i];
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
        reply.display.modes[0].rect = (Rect){0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT};
        reply.hdr.type = OK_DISPLAY_INFO;
        bytes = sizeof(DisplayInfo);
        break;
    case GET_EDID:
        if (got < 8 || !(gpu->vio.driver_features & EDID_FEATURE) || writable < sizeof(EdidReply)) break;
        if (body[0]) { reply.hdr.type = ERR_SCANOUT; break; }
        reply.hdr.type = OK_EDID;
        reply.edid.size = 128;
        edid(reply.edid.data);
        bytes = sizeof(EdidReply);
        break;
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
    if (reply.hdr.type >= ERR_UNSPEC) gpu->command_errors++;
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

static void notify(HvVirtio *vio, int queue) {
    HvVirtioGpu *gpu = vio->dev;
    bool completed = false;
    gpu->pending[queue] = true;
    for (unsigned n = 0; n < REQUESTS_PER_POLL; n++) {
        HvVirtqElem elem;
        int result = hv_virtq_pop(vio, queue, &elem);
        if (!result) { gpu->pending[queue] = false; break; }
        uint32_t written = 0;
        if (result > 0) {
            if (!queue) written = control(gpu, &elem);
            else cursor_command(gpu, &elem);
        }
        if (result < 0 || (!queue && !written)) gpu->malformed++;
        hv_virtq_push(vio, queue, &elem, written);
        gpu->completed[queue]++;
        completed = true;
    }
    if (completed) hv_virtio_queue_interrupt(vio, queue);
}

void hv_virtio_gpu_poll(HvVirtioGpu *gpu) {
    if (!gpu || !(gpu->vio.status & 4) || (gpu->vio.status & 0xc0)) return;
    for (int q = 0; q < QUEUES && !(gpu->vio.status & 64); q++)
        if (gpu->pending[q]) notify(&gpu->vio, q);
}

static void reset_device(HvVirtio *vio) {
    HvVirtioGpu *gpu = vio->dev;
    gpu->resets++;
    clear_scanout(gpu);
    for (unsigned i = 0; i < RESOURCES; i++)
        if (gpu->resources[i].id) release(gpu, &gpu->resources[i]);
    gpu->cursor = (Cursor){0};
    memset(gpu->pending, 0, sizeof(gpu->pending));
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
    snprintf(buffer, capacity,
             "graphics: adapter=VirtIO-GPU-2D display=%s status=0x%02x features=0x%" PRIx64
             " scanout=%ux%u host_cpu_bytes=%zu resources=%u\n"
             "graphics_queues: control_completed=%" PRIu64 " cursor_completed=%" PRIu64
             " control_pending=%u cursor_pending=%u malformed=%" PRIu64
             " command_errors=%" PRIu64 " scanout_updates=%" PRIu64 " resets=%" PRIu64 "\n",
             gpu->screen.front ? "virtio-scanout" : "RAMFB", (unsigned)gpu->vio.status, gpu->vio.driver_features,
             gpu->screen.rect.width, gpu->screen.rect.height, gpu->allocated + 2 * frame, resources,
             gpu->completed[0], gpu->completed[1], (unsigned)gpu->pending[0], (unsigned)gpu->pending[1],
             gpu->malformed, gpu->command_errors, gpu->publishes, gpu->resets);
}
