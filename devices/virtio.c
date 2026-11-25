#include "virtio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout of the one BAR (BAR 4, 16 KB), as QEMU's virtio-pci lays it out. */
#define BAR 4
#define COMMON 0x0000
#define ISR 0x1000
#define DEVICE 0x2000
#define NOTIFY 0x3000
#define NOTIFY_MULT 4

#define DESC_F_NEXT 1
#define DESC_F_WRITE 2
#define DESC_F_INDIRECT 4
#define DEVICE_NEEDS_RESET 64

static void add_cap(HvVirtio *v, uint8_t at, uint8_t next, uint8_t type, uint32_t off, uint32_t len) {
    uint8_t *c = v->pci.config + at;
    c[0] = 0x09;                    /* vendor-specific */
    c[1] = next;
    c[2] = type == 2 ? 20 : 16;
    c[3] = type;
    c[4] = BAR;
    memcpy(c + 8, &off, 4);
    memcpy(c + 12, &len, 4);
    if (type == 2) {
        uint32_t mult = NOTIFY_MULT;
        memcpy(c + 16, &mult, 4);
    }
}

void hv_virtio_reset(HvVirtio *v) {
    v->driver_features = 0;
    v->dfsel = v->gfsel = 0;
    v->status = 0;
    v->isr = 0;
    v->qsel = 0;
    v->config_vector = 0xffff;
    v->pci.msix_pba = 0;
    for (int i = 0; i < v->nq; i++) v->q[i] = (HvVirtq){.size = HV_VIRTQ_MAX_SIZE, .vector = 0xffff};
    hv_pci_set_intx(&v->pci, 0);
    if (v->reset_device) v->reset_device(v);
}

static uint64_t common_read(HvVirtio *v, uint64_t off) {
    HvVirtq *q = v->qsel < v->nq ? &v->q[v->qsel] : NULL;
    switch (off) {
    case 0x00: return v->dfsel;
    case 0x04: return v->dfsel < 2 ? (uint32_t)(v->features >> (32 * v->dfsel)) : 0;
    case 0x08: return v->gfsel;
    case 0x0c: return v->gfsel < 2 ? (uint32_t)(v->driver_features >> (32 * v->gfsel)) : 0;
    case 0x10: return v->config_vector;
    case 0x12: return v->nq;
    case 0x14: return v->status;
    case 0x15: return v->config_gen;
    case 0x16: return v->qsel;
    case 0x18: return q ? q->size : 0;
    case 0x1a: return q ? q->vector : 0xffff;
    case 0x1c: return q ? q->enabled : 0;
    case 0x1e: return v->qsel;                 /* queue_notify_off = queue index */
    case 0x20: return q ? (uint32_t)q->desc : 0;
    case 0x24: return q ? q->desc >> 32 : 0;
    case 0x28: return q ? (uint32_t)q->avail : 0;
    case 0x2c: return q ? q->avail >> 32 : 0;
    case 0x30: return q ? (uint32_t)q->used : 0;
    case 0x34: return q ? q->used >> 32 : 0;
    default: return 0;
    }
}

static void set64(uint64_t *f, uint64_t off, uint64_t v, unsigned size) {
    if (size == 8) *f = v;
    else if (off & 4) *f = (*f & 0xffffffffull) | v << 32;
    else *f = (*f & ~0xffffffffull) | (uint32_t)v;
}

static void common_write(HvVirtio *v, uint64_t off, uint64_t val, unsigned size) {
    HvVirtq *q = v->qsel < v->nq ? &v->q[v->qsel] : NULL;
    switch (off) {
    case 0x00: v->dfsel = (uint32_t)val; break;
    case 0x08: v->gfsel = (uint32_t)val; break;
    case 0x0c:
        if (v->gfsel < 2) {
            uint64_t mask = 0xffffffffull << (32 * v->gfsel);
            if (v->strict_dma && (v->status & 8)) break;
            uint64_t requested = (uint64_t)(uint32_t)val << (32 * v->gfsel);
            v->driver_features = (v->driver_features & ~mask) | (v->strict_dma ? requested : requested & v->features);
        }
        break;
    case 0x14:
        if ((uint8_t)val == 0) hv_virtio_reset(v);
        else {
            uint8_t status = (uint8_t)val;
            if (v->strict_dma)
                status = (status & ~DEVICE_NEEDS_RESET) | (v->status & DEVICE_NEEDS_RESET);
            v->status = status;
            if (v->strict_dma && (!(v->driver_features & HV_VIRTIO_F_VERSION_1) ||
                                  (v->driver_features & ~v->features))) v->status &= ~8u;
            if (v->strict_dma && !(v->status & 8)) v->status &= ~4u;
        }
        break;
    case 0x10: v->config_vector = val < v->pci.msix_nvec ? (uint16_t)val : 0xffff; break;
    case 0x16: v->qsel = (uint16_t)val; break;
    case 0x18:
        if (v->strict_dma && q && q->enabled) break;
        if (q && val && val <= HV_VIRTQ_MAX_SIZE && !(val & (val - 1))) q->size = (uint16_t)val;
        else if (q) /* the driver would index its rings by a size we do not use: say so */
            fprintf(stderr, "virtio %04x: queue %u size %llu refused (max %d)\n", v->pci.config[2] | v->pci.config[3] << 8,
                    v->qsel, (unsigned long long)val, HV_VIRTQ_MAX_SIZE);
        if (getenv("HV_VIRTIO_CFG_LOG"))
            fprintf(stderr, "virtio %04x: queue %u size %llu\n", v->pci.config[2] | v->pci.config[3] << 8, v->qsel, (unsigned long long)val);
        break;
    case 0x1a: if (q) q->vector = val < v->pci.msix_nvec ? (uint16_t)val : 0xffff; break;
    case 0x1c: if (q && (!v->strict_dma || !q->enabled)) q->enabled = val & 1; break;
    case 0x20: case 0x24: if (q && (!v->strict_dma || !q->enabled)) set64(&q->desc, off, val, size); break;
    case 0x28: case 0x2c: if (q && (!v->strict_dma || !q->enabled)) set64(&q->avail, off, val, size); break;
    case 0x30: case 0x34: if (q && (!v->strict_dma || !q->enabled)) set64(&q->used, off, val, size); break;
    default: break;
    }
}

static uint64_t bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvVirtio *v = d->opaque;
    if (bar != BAR) return ~0ull;
    if (off < ISR) return common_read(v, off);
    if (off < DEVICE) { /* ISR: reading clears it and drops the line */
        uint8_t isr = v->isr;
        v->isr = 0;
        hv_pci_set_intx(&v->pci, 0);
        return isr;
    }
    if (off < NOTIFY) {
        uint64_t r = 0;
        uint32_t o = (uint32_t)(off - DEVICE);
        if (o < v->config_len) memcpy(&r, v->config + o, size > v->config_len - o ? v->config_len - o : size);
        static int cfg_log = -1;
        if (cfg_log < 0) cfg_log = getenv("HV_VIRTIO_CFG_LOG") != NULL;
        if (cfg_log) /* debugging: which device config fields the driver reads */
            fprintf(stderr, "virtio %04x:%04x config read +%u (%u) = %llx\n", v->pci.config[0] | v->pci.config[1] << 8,
                    v->pci.config[2] | v->pci.config[3] << 8, o, size, (unsigned long long)r);
        return r;
    }
    return 0;
}

static void bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t val, unsigned size) {
    HvVirtio *v = d->opaque;
    if (bar != BAR) return;
    if (off < ISR) {
        common_write(v, off, val, size);
    } else if (off >= DEVICE && off < NOTIFY) {
        if (v->config_write) v->config_write(v, (uint32_t)(off - DEVICE), val, size);
    } else if (off >= NOTIFY) {
        int q = (int)((off - NOTIFY) / NOTIFY_MULT);
        if (q < v->nq && v->q[q].enabled && (v->status & 4) && v->notify &&
            (!v->strict_dma || !(v->status & 0xc0))) v->notify(v, q);
    }
}

void hv_virtio_init(HvVirtio *v, HvMachine *m, int device_id, uint32_t class_code, int nq, uint64_t features) {
    memset(v, 0, sizeof *v);
    v->m = m;
    v->nq = (uint16_t)nq;
    v->features = features | HV_VIRTIO_F_VERSION_1;
    hv_pci_init(&v->pci, 0x1af4, (uint16_t)(0x1040 + device_id), class_code << 8 | 1, 0x1af4, (uint16_t)device_id);
    v->pci.opaque = v;
    v->pci.bar_read = bar_read;
    v->pci.bar_write = bar_write;
    hv_pci_add_bar(&v->pci, BAR, 0x4000, 1);
    v->pci.config[0x06] |= 0x10;                 /* capability list */
    v->pci.config[0x34] = 0x40;
    add_cap(v, 0x40, 0x50, 1, COMMON, 0x38);
    add_cap(v, 0x50, 0x60, 3, ISR, 1);
    add_cap(v, 0x60, 0x70, 4, DEVICE, 0x100);
    add_cap(v, 0x70, 0x00, 2, NOTIFY, NOTIFY_MULT * HV_VIRTQ_MAX);
    hv_virtio_reset(v);
}

void hv_virtio_interrupt(HvVirtio *v) {
    v->isr |= 1;
    hv_pci_set_intx(&v->pci, 1);
}

void hv_virtio_queue_interrupt(HvVirtio *v, int q) {
    if (v->strict_dma) {
        uint16_t *flags = hv_virtio_dma_ptr(v, v->q[q].avail, sizeof(*flags));
        if (flags && (__atomic_load_n(flags, __ATOMIC_ACQUIRE) & 1)) return;
    }
    if (hv_pci_msix_enabled(&v->pci)) {
        hv_pci_msix_notify(&v->pci, v->q[q].vector);
    } else {
        hv_virtio_interrupt(v);
    }
}

void *hv_virtio_dma_ptr(HvVirtio *v, uint64_t addr, uint64_t len) {
    if (!v->strict_dma) return hv_phys_ptr(v->m, addr, len);
    HvRam *ram = hv_ram_lookup(v->m, addr);
    if (!ram || ram->readonly || len > ram->size - (addr - ram->pa)) return NULL;
    return ram->host + (addr - ram->pa);
}

static void queue_needs_reset(HvVirtio *v) {
    v->status |= DEVICE_NEEDS_RESET;
    if (hv_pci_msix_enabled(&v->pci)) hv_pci_msix_notify(&v->pci, v->config_vector);
    else {
        v->isr |= 2;
        hv_pci_set_intx(&v->pci, 1);
    }
}

int hv_virtq_pop(HvVirtio *v, int qi, HvVirtqElem *e) {
    HvVirtq *q = &v->q[qi];
    if (v->strict_dma && ((q->avail & 1) || (q->desc & 15) || (q->used & 3) ||
                          !hv_virtio_dma_ptr(v, q->desc, 16u * q->size) ||
                          !hv_virtio_dma_ptr(v, q->used, 4 + 8u * q->size))) {
        queue_needs_reset(v);
        return 0;
    }
    uint16_t *avail = hv_virtio_dma_ptr(v, q->avail, 4 + 2u * q->size);
    if (!avail) {
        if (v->strict_dma) queue_needs_reset(v);
        return 0;
    }
    uint16_t idx = __atomic_load_n(&avail[1], __ATOMIC_ACQUIRE);
    if (q->last_avail == idx) return 0;
    if (v->strict_dma && (uint16_t)(idx - q->last_avail) > q->size) {
        queue_needs_reset(v);
        return 0;
    }
    /* The guest writes the ring entry (and the descriptors) before the index,
       from another core: the acquire above keeps these reads after it, or a
       stale entry (the driver fills unused ones with 0xffff) comes back. */
    uint16_t head = __atomic_load_n(&avail[2 + q->last_avail % q->size], __ATOMIC_RELAXED);
    if (head >= q->size) {
        if (v->strict_dma) {
            queue_needs_reset(v);
            return 0;
        }
        /* Not written yet as far as this core can see (the driver keeps unused
           entries at 0xffff). Taking it would complete a request that does not
           exist (the guest panics "Adding invalid index 65535") or lose the
           real one: leave it for the next notify or poll. */
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x: avail entry %u not ready (head %u, idx %u); retrying later\n",
                    v->pci.config[2] | v->pci.config[3] << 8, q->last_avail, head, idx);
        return 0;
    }
    q->last_avail++;
    if (q->out[head]) {
        if (v->strict_dma) return 0;
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x q%d: head %u taken again while still outstanding\n",
                    v->pci.config[2] | v->pci.config[3] << 8, qi, head);
    }
    q->out[head] = 1;
    e->head = head;
    e->nseg = 0;
    uint64_t table = q->desc;
    uint32_t count = q->size;
    uint16_t i = head;
    int seen_writable = 0;
    for (uint32_t steps = 0; steps < HV_VIRTQ_MAX_SEGS; steps++) {
        if (i >= count) return -1;
        uint8_t *d = hv_virtio_dma_ptr(v, table + 16u * i, 16);
        if (!d) return -1;
        uint64_t addr;
        uint32_t len;
        uint16_t flags, next;
        memcpy(&addr, d, 8);
        memcpy(&len, d + 8, 4);
        memcpy(&flags, d + 12, 2);
        memcpy(&next, d + 14, 2);
        if (v->strict_dma && ((flags & ~(DESC_F_NEXT | DESC_F_WRITE)) ||
                              (seen_writable && !(flags & DESC_F_WRITE)) ||
                              (len && !hv_virtio_dma_ptr(v, addr, len)))) return -1;
        if (flags & DESC_F_WRITE) seen_writable = 1;
        if (flags & DESC_F_INDIRECT) { /* not offered, but harmless to follow */
            table = addr;
            count = len / 16;
            i = 0;
            continue;
        }
        if (e->nseg == HV_VIRTQ_MAX_SEGS) return -1;
        e->seg[e->nseg++] = (HvVirtqSeg){addr, len, (flags & DESC_F_WRITE) != 0};
        if (!(flags & DESC_F_NEXT)) return 1;
        i = next;
    }
    return -1;
}

void hv_virtq_push(HvVirtio *v, int qi, const HvVirtqElem *e, uint32_t written) {
    HvVirtq *q = &v->q[qi];
    if (e->head >= q->size || !q->out[e->head]) {
        /* Not something the driver gave us (or given back already): pushing it
           would corrupt its free list. */
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x q%d: completion of head %u that is not outstanding (dropped)\n",
                    v->pci.config[2] | v->pci.config[3] << 8, qi, e->head);
        return;
    }
    q->out[e->head] = 0;
    uint8_t *used = hv_virtio_dma_ptr(v, q->used, 4 + 8u * q->size);
    if (!used) return;
    uint16_t idx;
    if (v->strict_dma) idx = q->used_index;
    else memcpy(&idx, used + 2, 2);
    uint32_t id = e->head;
    uint32_t offset = 4 + 8u * (idx % q->size);
    if (v->strict_dma) {
        hv_dma_written(v->m, q->used + offset, 8);
        hv_dma_written(v->m, q->used + 2, 2);
    }
    memcpy(used + offset, &id, 4);
    memcpy(used + offset + 4, &written, 4);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    idx++;
    if (v->strict_dma) {
        q->used_index = idx;
        __atomic_store_n((uint16_t *)(used + 2), idx, __ATOMIC_RELEASE);
    } else memcpy(used + 2, &idx, 2);
}

static uint32_t copy(HvVirtio *v, const HvVirtqElem *e, int writable, uint32_t off, uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    for (int s = 0; s < e->nseg && done < len; s++) {
        const HvVirtqSeg *g = &e->seg[s];
        if (g->write != writable) continue;
        if (off >= g->len) {
            off -= g->len;
            continue;
        }
        uint32_t n = g->len - off;
        if (n > len - done) n = len - done;
        if (v->strict_dma && g->addr > UINT64_MAX - off) break;
        uint8_t *p = hv_virtio_dma_ptr(v, g->addr + off, n);
        if (!p) break;
        if (writable) {
            if (v->strict_dma) hv_dma_written(v->m, g->addr + off, n);
            memcpy(p, buf + done, n);
        } else memcpy(buf + done, p, n);
        done += n;
        off = 0;
    }
    return done;
}

uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t off, void *buf, uint32_t len) {
    return copy(v, e, 0, off, buf, len);
}

uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t off, const void *buf, uint32_t len) {
    return copy(v, e, 1, off, (uint8_t *)buf, len);
}

uint32_t hv_virtq_writable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int s = 0; s < e->nseg; s++)
        if (e->seg[s].write) n += e->seg[s].len;
    return n;
}
