/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "neptune_angle.h"
#include <dlfcn.h>
#include <stdint.h>
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

EGLContext hv_npt_angle_context(HvNptAngle *a,int major,int minor,bool shared) {
    EGLint attrs[]={EGL_CONTEXT_CLIENT_VERSION,major,NPT_EGL_CONTEXT_MINOR,minor,EGL_NONE};
    return a->create_context(a->display,a->config,shared?a->root:EGL_NO_CONTEXT,attrs);
}
int hv_npt_angle_current(HvNptAngle *a,EGLContext ctx) {
    EGLSurface s=ctx==EGL_NO_CONTEXT?EGL_NO_SURFACE:a->surface;
    return a->make_current(a->display,s,s,ctx)?0:-1;
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
    a->root=hv_npt_angle_context(a,3,0,false);
    if (a->root==EGL_NO_CONTEXT || hv_npt_angle_current(a,a->root)) goto fail;
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
