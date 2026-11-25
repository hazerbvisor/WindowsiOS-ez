/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "neptune_wire.h"
#include <string.h>

/* VirtIO 1.2 GPU wire fields are little-endian; do not cast guest buffers. */
enum {
    GET_INFO=0x108, GET_CAP=0x109, UNREF=0x102, CREATE_BLOB=0x10c,
    CTX_CREATE=0x200, CTX_DESTROY, CTX_ATTACH, CTX_DETACH, CREATE_3D,
    SUBMIT=0x207, MAP=0x208, UNMAP=0x209,
    OK=0x1100, OK_INFO=0x1102, OK_CAP=0x1103, OK_MAP=0x1106,
    ERROR=0x1200, INVALID=0x1205, HDR=24, FENCE=1, RING=2
};
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t get64(const uint8_t *p) { return get32(p)|(uint64_t)get32(p+4)<<32; }
static void put32(uint8_t *p,uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i));
}
size_t hv_neptune_wire_command(HvNeptune *n,const void *request,size_t length,
                               void *response,size_t capacity,uint64_t *ticket) {
    if (!n || !request || !response || !ticket || length<HDR || capacity<HDR) return 0;
    *ticket=0;
    const uint8_t *q=request;
    uint8_t *r=response;
    uint32_t type=get32(q), flags=get32(q+4), ctx=get32(q+16);
    uint64_t guest_fence=get64(q+8);
    uint32_t ring=q[20], rc=INVALID;
    size_t out=HDR;
    int ret=-1;
    memset(r,0,HDR);
    if (flags&~(FENCE|RING) || ((flags&RING)&&(!(flags&FENCE)||!ctx)) ||
        (!(flags&RING)&&ring)) goto done;
    if (flags&FENCE) {
        memcpy(r+4,q+4,17); /* flags, fence, context, ring; padding remains zero */
    }
    /* Context timeline fences on management commands require lifetime
     * rules beyond this staged decoder. Reject before any side effects.
     * SUBMIT supports exact deferred per-context fences. */
    if ((flags&RING) && type!=SUBMIT) goto done;
    switch(type) {
    case GET_INFO: {
        uint8_t cap[HV_NPT_MAX_CAPSET]; uint32_t version=0,size=0;
        if (length!=32 || ctx || get32(q+24)>1 || capacity<40) break;
        uint32_t id=get32(q+24)==0?HV_NPT_CAPSET:2;
        if (hv_neptune_capset_id(n,id,cap,sizeof(cap),&version,&size)) break;
        memset(r+HDR,0,16);
        put32(r+24,id); put32(r+28,version); put32(r+32,size);
        rc=OK_INFO; out=40; break;
    }
    case GET_CAP: {
        uint8_t cap[HV_NPT_MAX_CAPSET]; uint32_t version=0,size=0;
        if (length!=32 || ctx ||
            hv_neptune_capset_id(n,get32(q+24),cap,sizeof(cap),&version,&size) ||
            get32(q+28)!=version || size>sizeof(cap) || size>capacity-HDR) break;
        memcpy(r+HDR,cap,size); rc=OK_CAP; out=HDR+size; break;
    }
    case CTX_CREATE:
        if (length==96 && ctx && get32(q+24)<=64)
            ret=hv_neptune_context_create_cap(n,ctx,get32(q+28),(const char *)(q+32),get32(q+24));
        break;
    case CTX_DESTROY:
        if (length==HDR && ctx) ret=hv_neptune_context_destroy(n,ctx);
        break;
    case CTX_ATTACH: case CTX_DETACH:
        if (length==32 && ctx) ret=hv_neptune_blob_attach(n,ctx,get32(q+24),type==CTX_ATTACH);
        break;
    case CREATE_3D: {
        if (length!=72 || ctx) break;
        uint32_t args[11];
        for (unsigned i=0;i<11;++i) args[i]=get32(q+24+i*4);
        ret=hv_neptune_create_3d(n,args);
        break;
    }
    case CREATE_BLOB:
        /* Only HOST3D without guest scatter entries is implemented. */
        if (length==56 && ctx && get32(q+28)==2 && get32(q+36)==0)
            ret=hv_neptune_blob_create(n,ctx,get32(q+24),get64(q+40),get64(q+48),get32(q+32));
        break;
    case MAP: {
        uint32_t info=0;
        if (length!=40 || capacity<32) break;
        if (hv_neptune_blob_map(n,get32(q+24),get64(q+32),&info)) { rc=ERROR; break; }
        memset(r+HDR,0,8); put32(r+HDR,info); rc=OK_MAP; out=32; break;
    }
    case UNMAP:
        if (length==32) ret=hv_neptune_blob_unmap(n,get32(q+24));
        break;
    case UNREF:
        if (length==32) ret=hv_neptune_blob_destroy(n,get32(q+24));
        break;
    case SUBMIT: {
        if (length<32 || !ctx) break;
        uint32_t bytes=get32(q+24);
        if (!bytes || bytes%4 || bytes>HV_NPT_MAX_SUBMIT || bytes!=length-32) break;
        ret=hv_neptune_submit(n,ctx,q+32,bytes);
        if (!ret && (flags&FENCE) && ((flags&RING)?
            hv_neptune_fence_submit(n,ctx,ring,guest_fence,ticket):
            hv_neptune_fence_submit_legacy(n,ctx,guest_fence,ticket))) {
            *ticket=0; rc=ERROR; goto done;
        }
        break;
    }
    default: rc=ERROR; break;
    }
    if (!ret) rc=OK;
done:
    put32(r,rc);
    return out;
}
