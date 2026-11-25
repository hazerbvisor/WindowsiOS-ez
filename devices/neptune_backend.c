/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "neptune_backend.h"

#if defined(WINIOS_ENABLE_NEPTUNE)
#include <virglrenderer.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct Blob {
    uint32_t id, context;
    uint64_t bytes, offset;
    void *map;
    uint64_t mapped_bytes;
} Blob;
typedef struct Pending {
    HvNeptuneFence fence;
    bool done;
} Pending;

#define NPT_API(X) \
 X(virgl_renderer_init) X(virgl_renderer_cleanup) \
 X(virgl_renderer_get_cap_set) X(virgl_renderer_fill_caps) \
 X(virgl_renderer_context_create_with_flags) X(virgl_renderer_context_destroy) \
 X(virgl_renderer_submit_cmd) X(virgl_renderer_context_create_fence) \
 X(virgl_renderer_context_poll) X(virgl_renderer_resource_create_blob) \
 X(virgl_renderer_resource_map) X(virgl_renderer_resource_unmap) \
 X(virgl_renderer_resource_get_map_info) X(virgl_renderer_resource_unref) \
 X(virgl_renderer_ctx_attach_resource) X(virgl_renderer_ctx_detach_resource)
struct HvNeptune {
    void *renderer, *dxmt;
#define FIELD(sym) __typeof__(&sym) sym;
    NPT_API(FIELD)
#undef FIELD
    struct virgl_renderer_callbacks callbacks;
    pthread_mutex_t mutex;
    bool initialized;
    uint32_t contexts[HV_NPT_MAX_CONTEXTS];
    Blob blobs[HV_NPT_MAX_BLOBS];
    Pending pending[HV_NPT_MAX_FENCES];
    uint64_t allocated, next_ticket;
};
/* virglrenderer is process-global. Multiple instances are not supported. */
static pthread_mutex_t instance_lock = PTHREAD_MUTEX_INITIALIZER;
static bool instance_live;

static int context_slot(HvNeptune *n, uint32_t id) {
    if (!id) return -1;
    for (int i=0; i<HV_NPT_MAX_CONTEXTS; ++i)
        if (n->contexts[i]==id) return i;
    return -1;
}
static Blob *blob(HvNeptune *n, uint32_t id) {
    if (!id) return NULL;
    for (size_t i=0; i<HV_NPT_MAX_BLOBS; ++i)
        if (n->blobs[i].id==id) return &n->blobs[i];
    return NULL;
}
static void old_fence(void *cookie, uint32_t fence) { (void)cookie; (void)fence; }
static void context_fence(void *cookie, uint32_t context, uint32_t ring, uint64_t ticket) {
    HvNeptune *n=cookie;
    pthread_mutex_lock(&n->mutex);
    for (size_t i=0; i<HV_NPT_MAX_FENCES; ++i) {
        Pending *p=&n->pending[i];
        if (p->fence.ticket==ticket && p->fence.context==context && p->fence.ring==ring) {
            p->done=true;
            break;
        }
    }
    pthread_mutex_unlock(&n->mutex);
}

HvNeptune *hv_neptune_open(const char *renderer_path, const char *dxmt_path) {
    if (!renderer_path || !dxmt_path || renderer_path[0]!='/' || dxmt_path[0]!='/') return NULL;
    pthread_mutex_lock(&instance_lock);
    if (instance_live) { pthread_mutex_unlock(&instance_lock); return NULL; }
    instance_live=true;
    pthread_mutex_unlock(&instance_lock);
    HvNeptune *n=calloc(1,sizeof(*n));
    if (!n) goto fail_instance;
    if (pthread_mutex_init(&n->mutex,NULL)) { free(n); goto fail_instance; }
    n->dxmt=dlopen(dxmt_path,RTLD_LOCAL|RTLD_NOW);
    if (!n->dxmt || !dlsym(n->dxmt,"D3D11CreateDevice") ||
        !dlsym(n->dxmt,"CreateDXGIFactory1") || !dlsym(n->dxmt,"dxmt_native_abi_version") ||
        dlsym(n->dxmt,"D3D12CreateDevice")) goto fail;
    /* Trusted host paths, never derived from a guest packet. The pinned
     * Neptune render worker loads these two exports from the same dylib. */
    if (setenv("NPT_BACKEND","dxmt",1) ||
        setenv("NPT_D3D11_LIBRARY_PATH",dxmt_path,1) ||
        setenv("NPT_DXGI_LIBRARY_PATH",dxmt_path,1) ||
        setenv("NPT_D3D12_LIBRARY_PATH","/nonexistent/windowsios-dx12-disabled",1)) goto fail;
    n->renderer=dlopen(renderer_path,RTLD_LOCAL|RTLD_NOW);
    if (!n->renderer) goto fail;
#define LOAD(sym) do { \
    union { void *p; __typeof__(&sym) f; } cast; \
    cast.p=dlsym(n->renderer,#sym); if (!cast.p) goto fail; n->sym=cast.f; \
} while(0);
    NPT_API(LOAD)
#undef LOAD
    n->callbacks.version=VIRGL_RENDERER_CALLBACKS_VERSION;
    n->callbacks.write_fence=old_fence;
    n->callbacks.write_context_fence=context_fence;
    /* No GL, Venus, Vulkan or external renderer process. Build the pinned
     * renderer with render-server-mode=thread and worker=thread. */
    int flags=VIRGL_RENDERER_NO_VIRGL|VIRGL_RENDERER_NEPTUNE|
              VIRGL_RENDERER_RENDER_SERVER|VIRGL_RENDERER_THREAD_SYNC;
    if (n->virgl_renderer_init(n,flags,&n->callbacks)) goto fail;
    n->initialized=true;
    uint8_t caps[HV_NPT_MAX_CAPSET]; uint32_t version=0,size=0;
    if (hv_neptune_capset(n,caps,sizeof(caps),&version,&size)) goto fail;
    return n;
fail:
    hv_neptune_close(n);
    return NULL;
fail_instance:
    pthread_mutex_lock(&instance_lock); instance_live=false; pthread_mutex_unlock(&instance_lock);
    return NULL;
}

int hv_neptune_capset(HvNeptune *n, void *out, size_t capacity, uint32_t *version, uint32_t *size) {
    if (!n || !n->initialized || !out || !version || !size) return -1;
    uint32_t v=0,s=0;
    n->virgl_renderer_get_cap_set(HV_NPT_CAPSET,&v,&s);
    if (s<8 || s>HV_NPT_MAX_CAPSET || s>capacity) return -1;
    memset(out,0,s);
    n->virgl_renderer_fill_caps(HV_NPT_CAPSET,v,out);
    /* Pinned Neptune v2 capset has wire_format_version as the first u32. */
    uint32_t wire; memcpy(&wire,out,sizeof(wire));
    if (wire!=2) return -1;
    *version=v; *size=s;
    return 0;
}

int hv_neptune_context_create(HvNeptune *n,uint32_t id,const char *name,size_t length) {
    if (!n || !id || id>INT32_MAX || length>64 || (length && !name) || context_slot(n,id)>=0) return -1;
    for (size_t i=0;i<HV_NPT_MAX_CONTEXTS;++i) if (!n->contexts[i]) {
        char safe[65]={0};
        if (length) memcpy(safe,name,length);
        if (n->virgl_renderer_context_create_with_flags(id,HV_NPT_CAPSET,(uint32_t)length,safe)) return -1;
        n->contexts[i]=id;
        return 0;
    }
    return -1;
}

int hv_neptune_context_destroy(HvNeptune *n,uint32_t id) {
    if (!n) return -1;
    int slot=context_slot(n,id);
    if (slot<0) return -1;
    /* Require resource and fence retirement first. Never silently signal
     * outstanding work as successful when a context is destroyed. */
    for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) if (n->blobs[i].id && n->blobs[i].context==id) return -1;
    pthread_mutex_lock(&n->mutex);
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) if (n->pending[i].fence.ticket && n->pending[i].fence.context==id) {
        pthread_mutex_unlock(&n->mutex); return -1;
    }
    pthread_mutex_unlock(&n->mutex);
    n->virgl_renderer_context_destroy(id);
    n->contexts[slot]=0;
    return 0;
}

int hv_neptune_submit(HvNeptune *n,uint32_t context,const void *command,size_t bytes) {
    if (!n || context_slot(n,context)<0 || !command || !bytes || bytes>HV_NPT_MAX_SUBMIT || bytes%4) return -1;
    /* Renderer explicitly requires an immutable, aligned host command
     * buffer. Never pass writable guest memory to it directly. */
    void *copy=malloc(bytes);
    if (!copy) return -1;
    memcpy(copy,command,bytes);
    int ret=n->virgl_renderer_submit_cmd(copy,(int)context,(int)(bytes/4));
    free(copy);
    return ret ? -1 : 0;
}

int hv_neptune_blob_create(HvNeptune *n,uint32_t context,uint32_t resource,uint64_t blob_id,uint64_t bytes,uint32_t flags) {
    if (!n || context_slot(n,context)<0 || !resource || resource>INT32_MAX ||
        blob(n,resource) || !bytes || bytes>HV_NPT_BLOB_BUDGET-n->allocated ||
        (flags & ~(VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE|VIRGL_RENDERER_BLOB_FLAG_USE_SHAREABLE))) return -1;
    for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) if (!n->blobs[i].id) {
        struct virgl_renderer_resource_create_blob_args a={
            .res_handle=resource,.ctx_id=context,.blob_mem=VIRGL_RENDERER_BLOB_MEM_HOST3D,
            .blob_flags=flags,.blob_id=blob_id,.size=bytes
        };
        if (n->virgl_renderer_resource_create_blob(&a)) return -1;
        n->blobs[i]=(Blob){.id=resource,.context=context,.bytes=bytes};
        n->allocated+=bytes;
        return 0;
    }
    return -1;
}

int hv_neptune_blob_attach(HvNeptune *n,uint32_t context,uint32_t resource,bool attach) {
    Blob *b=n?blob(n,resource):NULL;
    if (!b || b->context!=context || context_slot(n,context)<0) return -1;
    if (attach) n->virgl_renderer_ctx_attach_resource((int)context,(int)resource);
    else n->virgl_renderer_ctx_detach_resource((int)context,(int)resource);
    return 0;
}

int hv_neptune_blob_map(HvNeptune *n,uint32_t resource,uint64_t offset,uint32_t *map_info) {
    Blob *b=n?blob(n,resource):NULL;
    if (!b || b->map || !map_info || offset%4096 || b->bytes%4096 ||
        offset>=HV_NPT_APERTURE_BYTES || b->bytes>HV_NPT_APERTURE_BYTES-offset) return -1;
    for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) {
        Blob *a=&n->blobs[i];
        if (a->map && offset<a->offset+a->mapped_bytes && a->offset<offset+b->bytes) return -1;
    }
    void *map=NULL; uint64_t size=0; uint32_t info=0;
    if (n->virgl_renderer_resource_map(resource,&map,&size)) return -1;
    if (!map || size!=b->bytes || n->virgl_renderer_resource_get_map_info(resource,&info)) {
        n->virgl_renderer_resource_unmap(resource); return -1;
    }
    b->map=map; b->offset=offset; b->mapped_bytes=size; *map_info=info;
    return 0;
}

int hv_neptune_blob_unmap(HvNeptune *n,uint32_t resource) {
    Blob *b=n?blob(n,resource):NULL;
    if (!b || !b->map) return -1;
    /* Remove accessibility first. The caller must quiesce normal-memory
     * guest mappings before calling this API; currently only scalar accesses
     * through the serialized host aperture are supported. */
    b->map=NULL; b->offset=0; b->mapped_bytes=0;
    return n->virgl_renderer_resource_unmap(resource) ? -1 : 0;
}

int hv_neptune_blob_destroy(HvNeptune *n,uint32_t resource) {
    Blob *b=n?blob(n,resource):NULL;
    if (!b || b->map) return -1;
    n->virgl_renderer_resource_unref(resource);
    n->allocated-=b->bytes; *b=(Blob){0};
    return 0;
}

int hv_neptune_aperture_access(HvNeptune *n,uint64_t offset,void *data,size_t bytes,bool write) {
    if (!n || !data || (bytes!=1 && bytes!=2 && bytes!=4 && bytes!=8) ||
        offset>=HV_NPT_APERTURE_BYTES || bytes>HV_NPT_APERTURE_BYTES-offset) return -1;
    for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) {
        Blob *b=&n->blobs[i];
        if (!b->map || offset<b->offset) continue;
        uint64_t rel=offset-b->offset;
        if (rel>=b->mapped_bytes || bytes>b->mapped_bytes-rel) continue;
        /* Volatile byte accesses deliberately do not offer the atomic
         * semantics required by shared feedback rings. */
        volatile uint8_t *p=(volatile uint8_t *)b->map+rel;
        for (size_t j=0;j<bytes;++j) {
            if (write) p[j]=((const uint8_t *)data)[j];
            else ((uint8_t *)data)[j]=p[j];
        }
        return 0;
    }
    return -1;
}

int hv_neptune_fence_submit(HvNeptune *n,uint32_t context,uint32_t ring,uint64_t guest_id,uint64_t *ticket) {
    if (!n || !ticket || context_slot(n,context)<0 || ring>255) return -1;
    pthread_mutex_lock(&n->mutex);
    if (n->next_ticket==UINT64_MAX) { pthread_mutex_unlock(&n->mutex); return -1; }
    Pending *p=NULL;
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) if (!n->pending[i].fence.ticket) { p=&n->pending[i]; break; }
    if (!p) { pthread_mutex_unlock(&n->mutex); return -1; }
    uint64_t id=++n->next_ticket;
    *p=(Pending){.fence={.context=context,.ring=ring,.guest_id=guest_id,.ticket=id}};
    pthread_mutex_unlock(&n->mutex);
    /* No MERGEABLE: each opaque host ticket has an exact completion.
     * Register before submission, which can synchronously invoke callback. */
    if (n->virgl_renderer_context_create_fence(context,0,ring,id)) {
        pthread_mutex_lock(&n->mutex); *p=(Pending){0}; pthread_mutex_unlock(&n->mutex);
        return -1;
    }
    *ticket=id;
    return 0;
}

void hv_neptune_poll(HvNeptune *n) {
    if (!n) return;
    for (size_t i=0;i<HV_NPT_MAX_CONTEXTS;++i)
        if (n->contexts[i]) n->virgl_renderer_context_poll(n->contexts[i]);
}

int hv_neptune_fence_take(HvNeptune *n,uint64_t ticket,HvNeptuneFence *out) {
    if (!n || !ticket || !out) return -1;
    int result=-1;
    pthread_mutex_lock(&n->mutex);
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) {
        Pending *p=&n->pending[i];
        if (p->fence.ticket!=ticket) continue;
        result=p->done ? 1 : 0;
        if (p->done) { *out=p->fence; *p=(Pending){0}; }
        break;
    }
    pthread_mutex_unlock(&n->mutex);
    return result;
}

void hv_neptune_close(HvNeptune *n) {
    if (!n) return;
    if (n->initialized) {
        for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) if (n->blobs[i].id) {
            if (n->blobs[i].map) (void)hv_neptune_blob_unmap(n,n->blobs[i].id);
            n->virgl_renderer_resource_unref(n->blobs[i].id);
        }
        /* Cleanup joins the renderer's contexts before releasing callback state. */
        n->virgl_renderer_cleanup(n);
    }
    if (n->renderer) dlclose(n->renderer);
    if (n->dxmt) dlclose(n->dxmt);
    pthread_mutex_destroy(&n->mutex);
    free(n);
    pthread_mutex_lock(&instance_lock); instance_live=false; pthread_mutex_unlock(&instance_lock);
}
#else
/* Keep the default engine and portable build independent of renderer libs.
 * Intentionally no stub pretending to execute graphics. */
typedef int hv_neptune_disabled_translation_unit;
#endif
