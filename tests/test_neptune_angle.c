/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../devices/neptune_angle.h"
#include <assert.h>

static unsigned calls;
static EGLContext expected_root;
static int bootstrap, child;

static EGLContext EGLAPIENTRY create_context(EGLDisplay display, EGLConfig config, EGLContext share,
                                             const EGLint *attributes) {
    assert(display && config);
    assert(share == expected_root);
    assert(attributes[0] == EGL_CONTEXT_CLIENT_VERSION && attributes[1] == 3);
    assert(attributes[4] == EGL_NONE);
    ++calls;
    return share == EGL_NO_CONTEXT ? (EGLContext)&bootstrap : (EGLContext)&child;
}

int main(void) {
    HvNptAngle angle = {
        .display = (EGLDisplay)&calls, .config = (EGLConfig)&expected_root,
        .create_context = create_context
    };
    expected_root = EGL_NO_CONTEXT;
    angle.root = hv_npt_angle_context(&angle,3,0);
    assert(angle.root == (EGLContext)&bootstrap);
    expected_root = angle.root;
    /* The ctx0 callback and every guest context use the same path. */
    assert(hv_npt_angle_context(&angle,3,0) == (EGLContext)&child);
    assert(hv_npt_angle_context(&angle,3,1) == (EGLContext)&child);
    assert(calls == 3);
    return 0;
}
