/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_NEPTUNE_ANGLE_H
#define WINIOS_NEPTUNE_ANGLE_H
#include <stdbool.h>
#include <EGL/egl.h>
typedef struct HvNptAngle {
    void *module;
    EGLDisplay display;
    EGLConfig config;
    EGLContext root;
    EGLSurface surface;
    bool share_verified;
    EGLBoolean (*initialize)(EGLDisplay,EGLint *,EGLint *);
    EGLBoolean (*terminate)(EGLDisplay);
    EGLBoolean (*bind_api)(EGLenum);
    EGLBoolean (*choose_config)(EGLDisplay,const EGLint *,EGLConfig *,EGLint,EGLint *);
    EGLContext (*create_context)(EGLDisplay,EGLConfig,EGLContext,const EGLint *);
    EGLBoolean (*destroy_context)(EGLDisplay,EGLContext);
    EGLSurface (*create_pbuffer)(EGLDisplay,EGLConfig,const EGLint *);
    EGLBoolean (*destroy_surface)(EGLDisplay,EGLSurface);
    EGLBoolean (*make_current)(EGLDisplay,EGLSurface,EGLSurface,EGLContext);
    __eglMustCastToProperFunctionPointerType (*get_proc)(const char *);
} HvNptAngle;
bool hv_npt_angle_open(HvNptAngle *a,const char *path);
void hv_npt_angle_close(HvNptAngle *a);
EGLContext hv_npt_angle_context(HvNptAngle *a,int major,int minor);
int hv_npt_angle_current(HvNptAngle *a,EGLContext ctx);
#endif
