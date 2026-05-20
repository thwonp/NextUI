#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

/* --- EGL type aliases (avoid pulling in EGL headers) --- */
typedef void         *EGLDisplay;
typedef void         *EGLSurface;
typedef int           EGLint;
typedef unsigned int  EGLBoolean;

#define EGL_TRUE              1
#define EGL_SWAP_BEHAVIOR     0x3093
#define EGL_BUFFER_PRESERVED  0x3094

/* --- real function pointers, resolved once at load time --- */
static EGLBoolean (*real_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*real_eglSurfaceAttrib)(EGLDisplay, EGLSurface, EGLint, EGLint);

__attribute__((constructor))
static void egldrain_init(void) {
    real_eglSwapBuffers   = dlsym(RTLD_NEXT, "eglSwapBuffers");
    real_eglSurfaceAttrib = dlsym(RTLD_NEXT, "eglSurfaceAttrib");
}

/* Single-entry cache: avoid re-setting PRESERVED on every swap for the
 * common single-surface-per-process case. If the surface changes
 * (recreated / multiple surfaces), the next swap will re-set on the new
 * surface. */
static EGLDisplay cached_dpy;
static EGLSurface cached_sfc;

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface sfc) {
    if (!real_eglSwapBuffers) return 0; /* defensive; shouldn't happen */

    if ((dpy != cached_dpy || sfc != cached_sfc) && real_eglSurfaceAttrib) {
        real_eglSurfaceAttrib(dpy, sfc, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
        cached_dpy = dpy;
        cached_sfc = sfc;
    }

    EGLBoolean rc = real_eglSwapBuffers(dpy, sfc);
    if (rc == EGL_TRUE) {
        real_eglSwapBuffers(dpy, sfc);  /* drain one PowerVR queue slot */
    }
    return rc;
}
