/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "neptune_angle.h"
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* ANGLE's published EGL extension constants. */
#define NPT_PLATFORM_ANGLE 0x3202
#define NPT_PLATFORM_ANGLE_TYPE 0x3203
#define NPT_PLATFORM_ANGLE_METAL 0x3489
#define NPT_EGL_DEVICE_EXT 0x322C
#define NPT_EGL_METAL_DEVICE_ANGLE 0x34A6
#define NPT_EGL_ES3_BIT 0x0040
#define NPT_EGL_CONTEXT_MINOR 0x30FB
typedef EGLDisplay (*GetPlatform)(EGLenum,void *,const EGLint *);
typedef EGLBoolean (*QueryDisplay)(EGLDisplay,EGLint,intptr_t *);
typedef EGLBoolean (*QueryDevice)(void *,EGLint,intptr_t *);

EGLContext hv_npt_angle_context(HvNptAngle *a,int major,int minor) {
    EGLint attrs[]={EGL_CONTEXT_CLIENT_VERSION,major,NPT_EGL_CONTEXT_MINOR,minor,EGL_NONE};
    /* Only the bootstrap context is unshared. Keep ctx0, guest subcontexts
     * and the external EGL winsys on the same ANGLE display/share group. */
    return a->create_context(a->display,a->config,a->root,attrs);
}
int hv_npt_angle_current(HvNptAngle *a,EGLContext ctx) {
    EGLSurface s=ctx==EGL_NO_CONTEXT?EGL_NO_SURFACE:a->surface;
    return a->make_current(a->display,s,s,ctx)?0:-1;
}

/* Exercise the actual ANGLE dispatch and object namespaces before handing the
 * display to virglrenderer. FBO names themselves remain context-local. */
static bool probe_share_group(HvNptAngle *a) {
#define GL_PROC(sym) __typeof__(&sym) p_##sym = (__typeof__(&sym))a->get_proc(#sym); if (!p_##sym) return false
    GL_PROC(glGenTextures); GL_PROC(glBindTexture); GL_PROC(glTexStorage2D); GL_PROC(glDeleteTextures);
    GL_PROC(glGenFramebuffers); GL_PROC(glBindFramebuffer); GL_PROC(glFramebufferTexture2D);
    GL_PROC(glCheckFramebufferStatus); GL_PROC(glDeleteFramebuffers); GL_PROC(glGetError);
    GL_PROC(glFenceSync); GL_PROC(glFlush); GL_PROC(glIsSync); GL_PROC(glClientWaitSync); GL_PROC(glDeleteSync);
#undef GL_PROC
    EGLContext producer=hv_npt_angle_context(a,3,0);
    EGLContext consumer=hv_npt_angle_context(a,3,0);
    GLuint texture=0, target=0;
    GLsync fence=NULL;
    bool verified=false;
    if (producer==EGL_NO_CONTEXT || consumer==EGL_NO_CONTEXT || hv_npt_angle_current(a,producer)) goto done;
    p_glGenTextures(1,&texture);
    p_glBindTexture(GL_TEXTURE_2D,texture);
    p_glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,4,4);
    fence=p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    p_glFlush();
    if (!texture || !fence || p_glGetError()!=GL_NO_ERROR || hv_npt_angle_current(a,consumer)) goto done;
    p_glGenFramebuffers(1,&target);
    p_glBindFramebuffer(GL_FRAMEBUFFER,target);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    GLenum status=p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLenum wait=p_glIsSync(fence)?p_glClientWaitSync(fence,0,0):GL_WAIT_FAILED;
    GLenum error=p_glGetError();
    verified=status==GL_FRAMEBUFFER_COMPLETE && wait!=GL_WAIT_FAILED && error==GL_NO_ERROR;
    if (!verified) fprintf(stderr,"winvm: ANGLE share probe framebuffer=0x%x wait=0x%x GL=0x%x\n",status,wait,error);
    p_glDeleteFramebuffers(1,&target);
done:
    if (producer!=EGL_NO_CONTEXT && !hv_npt_angle_current(a,producer)) {
        if (fence) p_glDeleteSync(fence);
        if (texture) p_glDeleteTextures(1,&texture);
    }
    if (hv_npt_angle_current(a,a->root)) {
        (void)hv_npt_angle_current(a,EGL_NO_CONTEXT);
        verified=false;
    }
    if (consumer!=EGL_NO_CONTEXT) a->destroy_context(a->display,consumer);
    if (producer!=EGL_NO_CONTEXT) a->destroy_context(a->display,producer);
    return verified;
}
bool hv_npt_angle_open(HvNptAngle *a,const char *path) {
    memset(a,0,sizeof(*a));
    if (!path || path[0]!='/') return false;
    a->module=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!a->module) return false;
#define LOAD(field,symbol) do { union { void *p; __typeof__(a->field) f; } c; c.p=dlsym(a->module,symbol); if (!c.p) goto fail; a->field=c.f; } while(0)
    LOAD(initialize,"eglInitialize"); LOAD(terminate,"eglTerminate"); LOAD(bind_api,"eglBindAPI");
    LOAD(choose_config,"eglChooseConfig"); LOAD(create_context,"eglCreateContext");
    LOAD(destroy_context,"eglDestroyContext"); LOAD(create_pbuffer,"eglCreatePbufferSurface");
    LOAD(destroy_surface,"eglDestroySurface"); LOAD(make_current,"eglMakeCurrent"); LOAD(get_proc,"eglGetProcAddress");
#undef LOAD
    GetPlatform get=(GetPlatform)a->get_proc("eglGetPlatformDisplayEXT");
    if (!get) goto fail;
    EGLint platform_attrs[]={NPT_PLATFORM_ANGLE_TYPE,NPT_PLATFORM_ANGLE_METAL,EGL_NONE};
    a->display=get(NPT_PLATFORM_ANGLE,(void *)EGL_DEFAULT_DISPLAY,platform_attrs);
    if (a->display==EGL_NO_DISPLAY || !a->initialize(a->display,NULL,NULL) || !a->bind_api(EGL_OPENGL_ES_API)) goto fail;
    /* Confirm that ANGLE actually selected Metal, not a software or GL backend. */
    QueryDisplay query_display=(QueryDisplay)a->get_proc("eglQueryDisplayAttribEXT");
    QueryDevice query_device=(QueryDevice)a->get_proc("eglQueryDeviceAttribEXT");
    intptr_t egl_device=0,metal_device=0;
    if (!query_display || !query_device || !query_display(a->display,NPT_EGL_DEVICE_EXT,&egl_device) ||
        !egl_device || !query_device((void *)egl_device,NPT_EGL_METAL_DEVICE_ANGLE,&metal_device) || !metal_device) goto fail;
    EGLint cfg_attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,NPT_EGL_ES3_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLint count=0;
    if (!a->choose_config(a->display,cfg_attrs,&a->config,1,&count) || count!=1) goto fail;
    EGLint pb_attrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
    a->surface=a->create_pbuffer(a->display,a->config,pb_attrs);
    if (a->surface==EGL_NO_SURFACE) goto fail;
    a->root=hv_npt_angle_context(a,3,0);
    if (a->root==EGL_NO_CONTEXT || hv_npt_angle_current(a,a->root)) goto fail;
    a->share_verified=probe_share_group(a);
    if (!a->share_verified) {
        fprintf(stderr,"winvm: ANGLE could not share a complete framebuffer and GL sync\n");
        goto fail;
    }
    return true;
fail:
    hv_npt_angle_close(a);
    return false;
}
void hv_npt_angle_close(HvNptAngle *a) {
    if (a->display!=EGL_NO_DISPLAY) {
        if (a->make_current) a->make_current(a->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (a->root!=EGL_NO_CONTEXT && a->destroy_context) a->destroy_context(a->display,a->root);
        if (a->surface!=EGL_NO_SURFACE && a->destroy_surface) a->destroy_surface(a->display,a->surface);
        if (a->terminate) a->terminate(a->display);
    }
    if (a->module) dlclose(a->module);
    memset(a,0,sizeof(*a));
}
