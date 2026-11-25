/* SPDX-License-Identifier: GPL-2.0-or-later
 * WinPVG P0: host-only protocol decoder and logical resource ledger.
 * Not wired to PCI, DMA, guest memory, WDDM or Metal.
 * Caller serializes all access and supplies an immutable packet.
 */
#ifndef WPVG_LAB_H
#define WPVG_LAB_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { WPVG_HEADER = 32, WPVG_MAX_PACKET = 65536, WPVG_SLOTS = 64 };
#define WPVG_BUDGET (256ull * 1024 * 1024)
enum wpvg_op { WPVG_PING = 1, WPVG_CREATE_BUFFER = 2,
    WPVG_DESTROY = 3, WPVG_UPLOAD = 4, WPVG_CREATE_TEXTURE = 5,
    WPVG_CLEAR = 6, WPVG_TRIANGLE = 7, WPVG_FENCE = 8 };
enum wpvg_result { WPVG_OK = 0, WPVG_BAD_PACKET, WPVG_STALE,
    WPVG_BAD_RESOURCE, WPVG_LIMIT, WPVG_UNSUPPORTED };
enum wpvg_kind { WPVG_BUFFER = 1, WPVG_TEXTURE = 2 };
struct wpvg_resource { uint64_t bytes; uint32_t id, kind; };
struct wpvg_state {
    uint64_t bytes, last_request;
    uint32_t generation;
    struct wpvg_resource res[WPVG_SLOTS];
};
struct wpvg_command {
    uint16_t op;
    uint32_t generation, id;
    uint64_t request;
    const uint8_t *payload;
    size_t payload_len;
};
static inline uint16_t wpvg_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static inline uint32_t wpvg_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline uint64_t wpvg_le64(const uint8_t *p) {
    return (uint64_t)wpvg_le32(p) | (uint64_t)wpvg_le32(p+4)<<32;
}
static inline void wpvg_init(struct wpvg_state *s) {
    memset(s, 0, sizeof *s); s->generation = 1;
}
/* Refuse generation wrap; recreate the whole device instead. */
static inline int wpvg_reset(struct wpvg_state *s) {
    if (s->generation == UINT32_MAX) return WPVG_LIMIT;
    uint32_t next = s->generation + 1;
    memset(s, 0, sizeof *s); s->generation = next; return WPVG_OK;
}
/* Wire: magic WPVG, major=0 (u16), opcode (u16), total bytes (u32),
 * generation (u32), request (u64), handle (u32), reserved zero (u32).
 * No struct casts or unaligned reads.
 */
static inline int wpvg_decode(const void *data, size_t n, struct wpvg_command *c) {
    if (!data || !c || n < WPVG_HEADER || n > WPVG_MAX_PACKET)
        return WPVG_BAD_PACKET;
    const uint8_t *p = data;
    if (memcmp(p, "WPVG", 4) || wpvg_le16(p+4) != 0 ||
        wpvg_le32(p+8) != n || wpvg_le32(p+28) != 0)
        return WPVG_BAD_PACKET;
    c->op=wpvg_le16(p+6); c->generation=wpvg_le32(p+12);
    c->request=wpvg_le64(p+16); c->id=wpvg_le32(p+24);
    c->payload=p+WPVG_HEADER; c->payload_len=n-WPVG_HEADER;
    return WPVG_OK;
}
static inline struct wpvg_resource *wpvg_find(struct wpvg_state *s, uint32_t id) {
    if (!id) return NULL;
    for (size_t i=0;i<WPVG_SLOTS;i++) if (s->res[i].id==id) return &s->res[i];
    return NULL;
}
static inline int wpvg_apply_mock(struct wpvg_state *s, const void *packet, size_t n) {
    if (!s) return WPVG_BAD_PACKET;
    struct wpvg_command c;
    int e=wpvg_decode(packet,n,&c);
    if (e) return e;
    if (c.generation!=s->generation || !c.request || c.request<=s->last_request)
        return WPVG_STALE;
    struct wpvg_resource *r=wpvg_find(s,c.id);
    size_t len=c.payload_len;
    uint64_t bytes=0;
    uint32_t kind=0;
    switch(c.op) {
    case WPVG_PING: case WPVG_FENCE:
        if (len || c.id) return WPVG_BAD_PACKET;
        break;
    case WPVG_CREATE_BUFFER:
        if (len!=8 || !c.id) return WPVG_BAD_PACKET;
        bytes=wpvg_le64(c.payload); kind=WPVG_BUFFER; break;
    case WPVG_CREATE_TEXTURE: {
        if (len!=8 || !c.id) return WPVG_BAD_PACKET;
        uint32_t w=wpvg_le32(c.payload), h=wpvg_le32(c.payload+4);
        if (!w || !h || w>8192 || h>8192) return WPVG_LIMIT;
        bytes=(uint64_t)w*h*4; kind=WPVG_TEXTURE; break;
    }
    case WPVG_DESTROY:
        if (len) return WPVG_BAD_PACKET;
        if (!r) return WPVG_BAD_RESOURCE;
        s->bytes-=r->bytes; memset(r,0,sizeof *r); break;
    case WPVG_UPLOAD: {
        if (len<=8) return WPVG_BAD_PACKET;
        if (!r || r->kind!=WPVG_BUFFER) return WPVG_BAD_RESOURCE;
        uint64_t offset=wpvg_le64(c.payload);
        if (offset>r->bytes || len-8>r->bytes-offset) return WPVG_LIMIT;
        break; /* Mock validates only; no pixels or buffers are stored. */
    }
    case WPVG_CLEAR: case WPVG_TRIANGLE:
        if (len) return WPVG_BAD_PACKET;
        if (!r || r->kind!=WPVG_TEXTURE) return WPVG_BAD_RESOURCE;
        break;
    default: return WPVG_UNSUPPORTED;
    }
    if (kind) {
        if (r) return WPVG_BAD_RESOURCE;
        if (!bytes || bytes>WPVG_BUDGET-s->bytes) return WPVG_LIMIT;
        for (size_t i=0;i<WPVG_SLOTS;i++) if (!s->res[i].id) {r=&s->res[i];break;}
        if (!r) return WPVG_LIMIT;
        r->id=c.id; r->kind=kind; r->bytes=bytes; s->bytes+=bytes;
    }
    s->last_request=c.request;
    return WPVG_OK;
}
#endif
