/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "neptune_backend.h"

#if defined(WINIOS_ENABLE_NEPTUNE)
#define VIRGL_RENDERER_UNSTABLE_APIS 1
#include <virglrenderer.h>
#include "neptune_angle.h"
#include "machine.h"
#include <sys/uio.h>
/* Pinned virgl_hw.h defines this six-u32 ABI; the installed public header forward declares it. */
struct virgl_box { uint32_t x,y,z,w,h,d; };
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

typedef struct Blob {
    uint32_t id, context;
    uint64_t bytes, offset;
    void *map;
    uint64_t mapped_bytes;
    uint32_t width,height,bpp,target,format,entry_count;
    HvNptBacking *entries;
    uint8_t *backing;
    uint64_t backing_bytes;
    struct iovec iov;
} Blob;
typedef struct Pending {
    HvNeptuneFence fence;
    bool done, legacy;
} Pending;

#define NPT_API(X) \
 X(virgl_renderer_init) X(virgl_renderer_cleanup) X(virgl_renderer_reset) \
 X(virgl_renderer_get_cap_set) X(virgl_renderer_fill_caps) \
 X(virgl_renderer_context_create_with_flags) X(virgl_renderer_context_destroy) \
 X(virgl_renderer_submit_cmd) X(virgl_renderer_context_create_fence) \
 X(virgl_renderer_context_poll) X(virgl_renderer_resource_create_blob) \
 X(virgl_renderer_create_fence) X(virgl_renderer_poll) X(virgl_renderer_resource_create) \
 X(virgl_renderer_resource_attach_iov) X(virgl_renderer_resource_detach_iov) \
 X(virgl_renderer_transfer_write_iov) X(virgl_renderer_transfer_read_iov) \
 X(virgl_renderer_resource_map) X(virgl_renderer_resource_unmap) \
 X(virgl_renderer_resource_get_map_info) X(virgl_renderer_resource_unref) \
 X(virgl_renderer_ctx_attach_resource) X(virgl_renderer_ctx_detach_resource) \
 X(virgl_renderer_create_handle_for_scanout) X(virgl_renderer_release_handle_for_scanout)
struct HvNeptune {
    void *renderer, *dxmt;
    HvNptAngle angle;
#define FIELD(sym) __typeof__(&sym) sym;
    NPT_API(FIELD)
#undef FIELD
    struct virgl_renderer_callbacks callbacks;
    pthread_mutex_t mutex;
    bool initialized;
    uint32_t contexts[HV_NPT_MAX_CONTEXTS], context_caps[HV_NPT_MAX_CONTEXTS];
    uint64_t ctx_attempts,ctx_success,ctx_fail,ctx_destroy,ctx_missing_destroy;
    uint32_t last_ctx_id,last_ctx_cap;
    int last_ctx_rc;
    Blob blobs[HV_NPT_MAX_BLOBS];
    Pending pending[HV_NPT_MAX_FENCES];
    uint64_t allocated, next_ticket;
};
/* virglrenderer is process-global. Multiple instances are not supported. */
static pthread_mutex_t instance_lock = PTHREAD_MUTEX_INITIALIZER;
static bool instance_live;
/* The renderer is process-global and workers may emit logs on other
 * threads. Static bounded storage avoids callback lifetime races. */
static pthread_mutex_t renderer_log_lock=PTHREAD_MUTEX_INITIALIZER;
static char renderer_logs[3][192];
static unsigned renderer_log_next;
static void renderer_log(enum virgl_log_level_flags level,const char *message,void *data) {
    (void)data;
    if (level<VIRGL_LOG_LEVEL_WARNING || !message) return;
    pthread_mutex_lock(&renderer_log_lock);
    char *dst=renderer_logs[renderer_log_next++%3];
    size_t i=0;
    for (; i<191 && message[i]; ++i) {
        unsigned char ch=(unsigned char)message[i];
        dst[i]=(ch>=32 && ch<127)?(char)ch:' ';
    }
    dst[i]=0;
    pthread_mutex_unlock(&renderer_log_lock);
}

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
static void old_fence(void *cookie,uint32_t fence) {
    HvNeptune *n=cookie;
    pthread_mutex_lock(&n->mutex);
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) {
        Pending *p=&n->pending[i];
        if (p->legacy && p->fence.ticket && p->fence.ticket<=fence) p->done=true;
    }
    pthread_mutex_unlock(&n->mutex);
}
static void *egl_display(void *cookie) { return ((HvNeptune *)cookie)->angle.display; }
static virgl_renderer_gl_context create_gl(void *cookie,int scanout,struct virgl_renderer_gl_ctx_param *p) {
    (void)scanout;
    /* vrend's ctx0 owns objects used by the other contexts and polls their
     * GL syncs. Its shared=false flag requests our root, not an isolated group. */
    return hv_npt_angle_context(&((HvNeptune *)cookie)->angle,p->major_ver,p->minor_ver);
}
static void destroy_gl(void *cookie,virgl_renderer_gl_context context) {
    HvNptAngle *a=&((HvNeptune *)cookie)->angle;
    if (context) a->destroy_context(a->display,(EGLContext)context);
}
static int current_gl(void *cookie,int scanout,virgl_renderer_gl_context context) {
    (void)scanout;
    return hv_npt_angle_current(&((HvNeptune *)cookie)->angle,(EGLContext)context);
}
static void context_fence(void *cookie, uint32_t context, uint32_t ring, uint64_t ticket) {
    HvNeptune *n=cookie;
    pthread_mutex_lock(&n->mutex);
    for (size_t i=0; i<HV_NPT_MAX_FENCES; ++i) {
        Pending *p=&n->pending[i];
        if (p->fence.ticket && !p->legacy && p->fence.guest_id==ticket && p->fence.context==context && p->fence.ring==ring) {
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
    if (!hv_npt_angle_open(&n->angle,getenv("WINIOS_NEPTUNE_EGL"))) goto fail;
    n->renderer=dlopen(renderer_path,RTLD_LOCAL|RTLD_NOW);
    if (!n->renderer) goto fail;
#define LOAD(sym) do { \
    union { void *p; __typeof__(&sym) f; } cast; \
    cast.p=dlsym(n->renderer,#sym); if (!cast.p) goto fail; n->sym=cast.f; \
} while(0);
    NPT_API(LOAD)
#undef LOAD
    pthread_mutex_lock(&renderer_log_lock);
    memset(renderer_logs,0,sizeof(renderer_logs)); renderer_log_next=0;
    pthread_mutex_unlock(&renderer_log_lock);
    union { void *p; __typeof__(&virgl_set_log_callback) f; } logfn;
    logfn.p=dlsym(n->renderer,"virgl_set_log_callback");
    if (logfn.f) logfn.f(renderer_log,NULL,NULL);
    n->callbacks.version=VIRGL_RENDERER_CALLBACKS_VERSION;
    n->callbacks.write_fence=old_fence;
    n->callbacks.write_context_fence=context_fence;
    /* Actual VirGL shadow contexts run through ANGLE's Metal backend;
     * DirectX 11 still goes only through Neptune/DXMT. */
    n->callbacks.create_gl_context=create_gl;
    n->callbacks.destroy_gl_context=destroy_gl;
    n->callbacks.make_current=current_gl;
    n->callbacks.get_egl_display=egl_display;
    int flags=VIRGL_RENDERER_USE_GLES|VIRGL_RENDERER_NATIVE_SHARE_TEXTURE|
              VIRGL_RENDERER_NEPTUNE|VIRGL_RENDERER_RENDER_SERVER|VIRGL_RENDERER_THREAD_SYNC;
    if (n->virgl_renderer_init(n,flags,&n->callbacks)) goto fail;
    n->initialized=true;
    uint8_t caps[HV_NPT_MAX_CAPSET]; uint32_t version=0,size=0;
    if (hv_neptune_capset(n,caps,sizeof(caps),&version,&size) ||
        hv_neptune_capset_id(n,2,caps,sizeof(caps),&version,&size)) goto fail;
    return n;
fail:
    hv_neptune_close(n);
    return NULL;
fail_instance:
    pthread_mutex_lock(&instance_lock); instance_live=false; pthread_mutex_unlock(&instance_lock);
    return NULL;
}

int hv_neptune_capset(HvNeptune *n,void *out,size_t capacity,uint32_t *version,uint32_t *size) {
    return hv_neptune_capset_id(n,HV_NPT_CAPSET,out,capacity,version,size);
}
int hv_neptune_capset_id(HvNeptune *n,uint32_t id,void *out,size_t capacity,uint32_t *version,uint32_t *size) {
    if (!n || !n->initialized || !out || !version || !size || (id!=2 && id!=HV_NPT_CAPSET)) return -1;
    uint32_t v=0,s=0;
    n->virgl_renderer_get_cap_set(id,&v,&s);
    if (s<8 || s>HV_NPT_MAX_CAPSET || s>capacity) return -1;
    memset(out,0,s);
    n->virgl_renderer_fill_caps(id,v,out);
    /* Pinned Neptune v2 capset has wire_format_version as the first u32. */
    uint32_t wire; memcpy(&wire,out,sizeof(wire));
    if (id==HV_NPT_CAPSET && wire!=2) return -1;
    *version=v; *size=s;
    return 0;
}

int hv_neptune_context_create(HvNeptune *n,uint32_t id,const char *name,size_t length) {
    return hv_neptune_context_create_cap(n,id,HV_NPT_CAPSET,name,length);
}
int hv_neptune_context_create_cap(HvNeptune *n,uint32_t id,uint32_t capset,const char *name,size_t length) {
    if (!n) return -1;
    n->ctx_attempts++;
    n->last_ctx_id=id; n->last_ctx_cap=capset;
    /* A zero context_init is the legacy VirGL context, as in upstream QEMU. */
    if (!capset) capset=1;
    int rc=EINVAL;
    if ((capset!=1 && capset!=2 && capset!=HV_NPT_CAPSET) ||
        !id || id>INT32_MAX || length>64 || (length && !name)) goto failed;
    int existing=context_slot(n,id);
    if (existing>=0) {
        if (n->context_caps[existing]!=capset) goto failed;
        n->ctx_success++; n->last_ctx_rc=0; return 0;
    }
    rc=ENOSPC;
    for (size_t i=0;i<HV_NPT_MAX_CONTEXTS;++i) if (!n->contexts[i]) {
        char safe[65]={0};
        if (length) memcpy(safe,name,length);
        rc=n->virgl_renderer_context_create_with_flags(id,capset,(uint32_t)length,safe);
        if (rc) goto failed;
        n->contexts[i]=id; n->context_caps[i]=capset;
        n->ctx_success++; n->last_ctx_rc=0;
        return 0;
    }
failed:
    n->last_ctx_rc=rc; n->ctx_fail++; return -1;
}

int hv_neptune_context_destroy(HvNeptune *n,uint32_t id) {
    if (!n) return -1;
    int slot=context_slot(n,id);
    /* The driver destroys unused context IDs as well as live contexts.
     * Upstream virglrenderer treats a missing ID as a no-op. Global resources
     * are independently owned and may outlive their creating context. */
    if (slot<0) { n->ctx_missing_destroy++; return 0; }
    n->virgl_renderer_context_destroy(id);
    n->contexts[slot]=0; n->context_caps[slot]=0;
    n->ctx_destroy++;
    /* Destroy joins renderer context workers. Never report success for
     * timeline work abandoned by teardown: invalidate unretired tickets
     * so the VirtIO queue completes them with an error. Legacy ctx0
     * fences are independent and continue to retire normally. */
    pthread_mutex_lock(&n->mutex);
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) {
        Pending *p=&n->pending[i];
        if (p->fence.ticket && !p->legacy && !p->done && p->fence.context==id)
            *p=(Pending){0};
    }
    pthread_mutex_unlock(&n->mutex);
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

/* Typed shadow textures use the same resource namespace and conservative
 * allocation accounting. Initial candidate supports single-level 2D only. */
int hv_neptune_create_3d(HvNeptune *n,const uint32_t a[11]) {
    if (!n || !a || !a[0] || a[0]>INT32_MAX || blob(n,a[0]) || (a[1]!=0 && a[1]!=2) ||
        !a[4] || !a[5] || a[6]!=1 || a[7]!=1 || a[8]!=0 || a[9]>1 || (a[10]&~1u)) return -1;
    uint32_t bpp=0;
    if (a[1]==0) { if (a[5]!=1 || a[4]>(32u<<20)) return -1; bpp=1; }
    else { if (a[4]>4096 || a[5]>4096 || (a[2]!=1 && a[2]!=2 && a[2]!=67 && a[2]!=134)) return -1; bpp=4; }
    uint64_t bytes=(uint64_t)a[4]*a[5]*(a[1]==0?1:16);
    if (bytes>HV_NPT_BLOB_BUDGET-n->allocated) return -1;
    for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) if (!n->blobs[i].id) {
        struct virgl_renderer_resource_create_args args={
            .handle=a[0],.target=a[1],.format=a[2],.bind=a[3],.width=a[4],
            .height=a[5],.depth=a[6],.array_size=a[7],.last_level=a[8],.nr_samples=a[9],.flags=a[10]
        };
        if (n->virgl_renderer_resource_create(&args,NULL,0)) return -1;
        n->blobs[i]=(Blob){.id=a[0],.bytes=bytes,.width=a[4],.height=a[5],.bpp=bpp,.target=a[1],.format=a[2]};
        n->allocated+=bytes;
        return 0;
    }
    return -1;
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
    if (!b || context_slot(n,context)<0) return -1;
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
    if (n->virgl_renderer_resource_unmap(resource)) return -1;
    b->map=NULL; b->offset=0; b->mapped_bytes=0;
    return 0;
}

/* Use host-owned bounce backing rather than exposing arbitrary guest RAM to
 * asynchronous renderer writes. Synchronize explicit transfers only. */
static int sync_backing(Blob *b,HvMachine *m,bool to_host) {
    uint64_t offset=0;
    for (uint32_t i=0;i<b->entry_count;++i) {
        HvNptBacking *e=&b->entries[i];
        HvRam *r=hv_ram_lookup(m,e->pa);
        if (!r || r->readonly || e->len>r->size-(e->pa-r->pa)) return -1;
        if (to_host) memcpy(b->backing+offset,r->host+(e->pa-r->pa),e->len);
        else {
            hv_dma_written(m,e->pa,e->len);
            memcpy(r->host+(e->pa-r->pa),b->backing+offset,e->len);
        }
        offset+=e->len;
    }
    return 0;
}
int hv_neptune_attach_backing(HvNeptune *n,HvMachine *m,uint32_t id,const HvNptBacking *entries,uint32_t count) {
    Blob *b=n?blob(n,id):NULL;
    if (!b || !b->bpp || b->backing || !entries || !count || count>8192) return -1;
    uint64_t total=0;
    for (uint32_t i=0;i<count;++i) {
        HvRam *r=hv_ram_lookup(m,entries[i].pa);
        if (!entries[i].len || !r || r->readonly || entries[i].len>r->size-(entries[i].pa-r->pa) ||
            entries[i].len>HV_NPT_BLOB_BUDGET-n->allocated-total) return -1;
        total+=entries[i].len;
    }
    if (total<(uint64_t)b->width*b->height*b->bpp || total>(64u<<20)) return -1;
    b->entries=malloc(count*sizeof(*entries)); b->backing=malloc((size_t)total);
    if (!b->entries || !b->backing) goto fail;
    memcpy(b->entries,entries,count*sizeof(*entries));
    b->entry_count=count; b->backing_bytes=total;
    b->iov=(struct iovec){.iov_base=b->backing,.iov_len=(size_t)total};
    if (sync_backing(b,m,true) || n->virgl_renderer_resource_attach_iov((int)id,&b->iov,1)) goto fail;
    n->allocated+=total; return 0;
fail:
    free(b->entries); free(b->backing); b->entries=NULL; b->backing=NULL;
    b->entry_count=0; b->backing_bytes=0; b->iov=(struct iovec){0}; return -1;
}
int hv_neptune_detach_backing(HvNeptune *n,uint32_t id) {
    Blob *b=n?blob(n,id):NULL;
    if (!b || !b->backing) return -1;
    n->virgl_renderer_resource_detach_iov((int)id,NULL,NULL);
    n->allocated-=b->backing_bytes;
    free(b->entries); free(b->backing); b->entries=NULL; b->backing=NULL;
    b->entry_count=0; b->backing_bytes=0; b->iov=(struct iovec){0}; return 0;
}
int hv_neptune_transfer(HvNeptune *n,HvMachine *m,uint32_t id,uint32_t context,const uint32_t a[6],uint64_t offset,uint32_t level,uint32_t stride,uint32_t layer_stride,bool to_host) {
    Blob *b=n?blob(n,id):NULL;
    if (!b || !b->backing || !a || level || (context && context_slot(n,context)<0) ||
        a[2] || a[5]!=1 || !a[3] || !a[4] || a[0]>=b->width || a[1]>=b->height ||
        a[3]>b->width-a[0] || a[4]>b->height-a[1]) return -1;
    uint64_t row=(uint64_t)a[3]*b->bpp;
    if (!stride) stride=b->width*b->bpp;
    if (stride<row || offset>=b->backing_bytes || (uint64_t)stride*(a[4]-1)+row>b->backing_bytes-offset) return -1;
    if (sync_backing(b,m,true)) return -1;
    struct virgl_box box={a[0],a[1],a[2],a[3],a[4],a[5]};
    int ret=to_host?n->virgl_renderer_transfer_write_iov(id,context,0,stride,layer_stride,&box,offset,&b->iov,1):
        n->virgl_renderer_transfer_read_iov(id,context,0,stride,layer_stride,&box,offset,&b->iov,1);
    if (ret) return -1;
    if (!to_host) return sync_backing(b,m,false);
    return 0;
}

int hv_neptune_blob_destroy(HvNeptune *n,uint32_t resource) {
    Blob *b=n?blob(n,resource):NULL;
    if (!b || b->map) return -1;
    if (b->backing) (void)hv_neptune_detach_backing(n,resource);
    n->virgl_renderer_resource_unref(resource);
    n->allocated-=b->bytes; *b=(Blob){0};
    return 0;
}

void *hv_neptune_scanout_texture(HvNeptune *n,uint32_t resource,uint32_t width,uint32_t height,uint32_t format,uint32_t stride,uint32_t offset) {
    Blob *b=n?blob(n,resource):NULL;
    if (!b || !width || !height || width>4096 || height>4096 || stride<width*4 ||
        (format!=1 && format!=2 && format!=67 && format!=134) ||
        offset>=b->bytes || (uint64_t)stride*(height-1)+(uint64_t)width*4>b->bytes-offset) return NULL;
    virgl_renderer_native_handle handle=NULL;
    enum virgl_renderer_native_handle_type type=n->virgl_renderer_create_handle_for_scanout(resource,width,height,format,0,stride,offset,&handle);
    if (type!=VIRGL_NATIVE_HANDLE_METAL_TEXTURE) {
        if (type!=VIRGL_NATIVE_HANDLE_NONE && handle) n->virgl_renderer_release_handle_for_scanout(type,handle);
        return NULL;
    }
    return handle;
}
/* Typed VirGL resources carry a retained native Metal texture when
 * allocated with the scanout bind. Pinned virglrenderer explicitly supports
 * this case in create_handle_for_scanout, not just SHM blobs. */
void *hv_neptune_typed_scanout(HvNeptune *n,uint32_t resource,uint32_t width,uint32_t height) {
    Blob *b=n?blob(n,resource):NULL;
    if (!b || b->target!=2 || b->bpp!=4 || width!=b->width || height!=b->height) return NULL;
    return hv_neptune_scanout_texture(n,resource,width,height,b->format,width*4,0);
}
unsigned hv_neptune_resource_count(HvNeptune *n,uint64_t *bytes) {
    unsigned count=0;
    if (bytes) *bytes=n?n->allocated:0;
    if (n) for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) count+=n->blobs[i].id!=0;
    return count;
}
void hv_neptune_context_diagnostics(HvNeptune *n,char *buffer,size_t capacity) {
    if (!capacity) return;
    if (!n) { *buffer=0; return; }
    unsigned live=0;
    for (size_t i=0;i<HV_NPT_MAX_CONTEXTS;++i) live+=n->contexts[i]!=0;
    int len=snprintf(buffer,capacity,"neptune_contexts: live=%u attempts=%" PRIu64 " success=%" PRIu64 " fail=%" PRIu64 " destroyed=%" PRIu64 " missing_destroy=%" PRIu64 " last_id=%u last_cap=%u last_rc=%d container_shmem=%u angle_share_verified=%u\n",
        live,n->ctx_attempts,n->ctx_success,n->ctx_fail,n->ctx_destroy,n->ctx_missing_destroy,
        n->last_ctx_id,n->last_ctx_cap,n->last_ctx_rc,getenv("WINIOS_NEPTUNE_SHM_DIR")?1u:0u,
        (unsigned)n->angle.share_verified);
    if (len<0) return;
    size_t used=(size_t)len;
    pthread_mutex_lock(&renderer_log_lock);
    unsigned count=renderer_log_next<3?renderer_log_next:3;
    for (unsigned i=0;i<count && used<capacity;++i) {
        const char *line=renderer_logs[(renderer_log_next-count+i)%3];
        int k=snprintf(buffer+used,capacity-used,"renderer_warning: %s\n",line);
        if (k>0) used+=(size_t)k;
    }
    pthread_mutex_unlock(&renderer_log_lock);
}
void hv_neptune_scanout_release(HvNeptune *n,void *texture) {
    if (n && texture) n->virgl_renderer_release_handle_for_scanout(VIRGL_NATIVE_HANDLE_METAL_TEXTURE,texture);
}

bool hv_neptune_blob_exists(HvNeptune *n,uint32_t resource) { return n && blob(n,resource)!=NULL; }

void *hv_neptune_aperture_ptr(HvNeptune *n,uint64_t offset,uint64_t bytes) {
    if (!n || !bytes || offset>=HV_NPT_APERTURE_BYTES || bytes>HV_NPT_APERTURE_BYTES-offset) return NULL;
    for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) {
        Blob *b=&n->blobs[i];
        if (!b->map || offset<b->offset) continue;
        uint64_t rel=offset-b->offset;
        if (rel<b->mapped_bytes && bytes<=b->mapped_bytes-rel) return (uint8_t *)b->map+rel;
    }
    return NULL;
}

int hv_neptune_reset(HvNeptune *n) {
    if (!n || !n->initialized) return -1;
    for (size_t i=0;i<HV_NPT_MAX_BLOBS;++i) if (n->blobs[i].id) {
        if (n->blobs[i].map) (void)hv_neptune_blob_unmap(n,n->blobs[i].id);
        if (n->blobs[i].backing) (void)hv_neptune_detach_backing(n,n->blobs[i].id);
        n->virgl_renderer_resource_unref(n->blobs[i].id);
    }
    n->virgl_renderer_reset();
    memset(n->contexts,0,sizeof(n->contexts));
    memset(n->context_caps,0,sizeof(n->context_caps));
    memset(n->blobs,0,sizeof(n->blobs));
    n->allocated=0;
    pthread_mutex_lock(&n->mutex);
    memset(n->pending,0,sizeof(n->pending));
    pthread_mutex_unlock(&n->mutex);
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
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) {
        Pending *candidate=&n->pending[i];
        if (candidate->fence.ticket && !candidate->legacy && candidate->fence.context==context &&
            candidate->fence.ring==ring && candidate->fence.guest_id==guest_id) {
            pthread_mutex_unlock(&n->mutex); return -1;
        }
        if (!candidate->fence.ticket && !p) p=candidate;
    }
    if (!p) { pthread_mutex_unlock(&n->mutex); return -1; }
    uint64_t id=++n->next_ticket;
    *p=(Pending){.fence={.context=context,.ring=ring,.guest_id=guest_id,.ticket=id}};
    pthread_mutex_unlock(&n->mutex);
    /* Neptune ARM_EVENT_FENCE pairs the actual wire fence ID, not our
     * private descriptor ticket. Reserve before a synchronous callback. */
    if (n->virgl_renderer_context_create_fence(context,0,ring,guest_id)) {
        pthread_mutex_lock(&n->mutex); *p=(Pending){0}; pthread_mutex_unlock(&n->mutex);
        return -1;
    }
    *ticket=id;
    return 0;
}

int hv_neptune_fence_submit_legacy(HvNeptune *n,uint32_t context,uint64_t guest_id,uint64_t *ticket) {
    if (!n || !ticket || context_slot(n,context)<0) return -1;
    pthread_mutex_lock(&n->mutex);
    if (n->next_ticket>=INT32_MAX) { pthread_mutex_unlock(&n->mutex); return -1; }
    Pending *p=NULL;
    for (size_t i=0;i<HV_NPT_MAX_FENCES;++i) if (!n->pending[i].fence.ticket) { p=&n->pending[i]; break; }
    if (!p) { pthread_mutex_unlock(&n->mutex); return -1; }
    uint64_t id=++n->next_ticket;
    *p=(Pending){.fence={.context=context,.guest_id=guest_id,.ticket=id},.legacy=true};
    pthread_mutex_unlock(&n->mutex);
    if (n->virgl_renderer_create_fence((int)id,context)) {
        pthread_mutex_lock(&n->mutex); *p=(Pending){0}; pthread_mutex_unlock(&n->mutex);
        return -1;
    }
    *ticket=id; return 0;
}

void hv_neptune_poll(HvNeptune *n) {
    if (!n) return;
    n->virgl_renderer_poll();
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
            if (n->blobs[i].backing) (void)hv_neptune_detach_backing(n,n->blobs[i].id);
        n->virgl_renderer_resource_unref(n->blobs[i].id);
        }
        /* Cleanup joins the renderer's contexts before releasing callback state. */
        n->virgl_renderer_cleanup(n);
    }
    if (n->renderer) dlclose(n->renderer);
    if (n->dxmt) dlclose(n->dxmt);
    hv_npt_angle_close(&n->angle);
    pthread_mutex_destroy(&n->mutex);
    free(n);
    pthread_mutex_lock(&instance_lock); instance_live=false; pthread_mutex_unlock(&instance_lock);
}
#else
/* Keep the default engine and portable build independent of renderer libs.
 * Intentionally no stub pretending to execute graphics. */
typedef int hv_neptune_disabled_translation_unit;
#endif
