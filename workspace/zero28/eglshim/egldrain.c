#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef void         *EGLDisplay;
typedef void         *EGLSurface;
typedef int           EGLint;
typedef unsigned int  EGLBoolean;

#define EGL_TRUE              1
#define EGL_SWAP_BEHAVIOR     0x3093
#define EGL_BUFFER_PRESERVED  0x3094
#define EGL_BUFFER_DESTROYED  0x3095

typedef void*      (*dlsym_t)(void*, const char*);
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*eglSurfaceAttrib_t)(EGLDisplay, EGLSurface, EGLint, EGLint);
typedef EGLBoolean (*eglQuerySurface_t)(EGLDisplay, EGLSurface, EGLint, EGLint*);
typedef int        (*nanosleep_t)(const struct timespec*, struct timespec*);
typedef int        (*clock_nanosleep_t)(clockid_t, int, const struct timespec*, struct timespec*);

/* Forward declaration — eglSwapBuffers is defined below but called from
 * maybe_drain(), which appears earlier in the file. */
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface sfc);

static dlsym_t              real_dlsym;
static eglSwapBuffers_t     real_eglSwapBuffers;
static eglSurfaceAttrib_t   real_eglSurfaceAttrib;
static eglQuerySurface_t    real_eglQuerySurface;
static nanosleep_t          real_nanosleep;
static clock_nanosleep_t    real_clock_nanosleep;

#define MAX_SURFACES 8
typedef struct {
    EGLDisplay dpy;
    EGLSurface sfc;
} SurfaceSlot;

static SurfaceSlot surface_cache[MAX_SURFACES];
static int         surface_count;
static pthread_mutex_t surface_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Render-thread state — only touched on the render thread after render_tid is
 * set, so no mutex needed for these fields. render_tid itself is a plain int
 * which is atomically readable on aarch64 aligned 4-byte access. */
static pid_t             render_tid;
static int               pending_drains;
static struct timespec   last_swap_ts;
static int               in_drain;
static EGLDisplay        last_dpy;
static EGLSurface        last_sfc;

__attribute__((constructor))
static void egldrain_init(void) {
    /* Bootstrap real dlsym via dlvsym to avoid recursing into our own
     * dlsym override. GLIBC_2.17 is the only version exported on this
     * device (glibc 2.29, aarch64). */
    real_dlsym = (dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    dprintf(2, "[egldrain] init: real_dlsym=%p\n", (void*)real_dlsym);
}

/* Try to drain once: issue 1 extra eglSwapBuffers under in_drain guard.
 * Called from nanosleep / clock_nanosleep hooks when the render thread has
 * been idle long enough. */
static void maybe_drain(void) {
    if (pending_drains <= 0 || in_drain)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec  - last_swap_ts.tv_sec)  * 1000L
                    + (now.tv_nsec - last_swap_ts.tv_nsec) / 1000000L;
    if (elapsed_ms <= 20)
        return;

    static int first_drain_logged;
    in_drain = 1;
    /* Call our own eglSwapBuffers wrapper; the in_drain guard there makes it
     * do exactly one real swap and return without re-arming pending_drains. */
    EGLBoolean drc = eglSwapBuffers(last_dpy, last_sfc);
    pending_drains--;
    in_drain = 0;

    if (!first_drain_logged) {
        first_drain_logged = 1;
        dprintf(2, "[egldrain] first drain fired elapsed_ms=%ld pending->%d rc=%u\n",
                elapsed_ms, pending_drains, (unsigned)drc);
    }
}

/* Exported eglSwapBuffers hook. Invoked from two paths:
 *   (a) PLT-interposition: any NEEDED-linked caller of eglSwapBuffers hits
 *       our symbol first because we're at the front of the load order.
 *   (b) dlsym hook below: SDL2's private function pointer points here. */
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface sfc) {
    static unsigned long call_count;
    static int tid_logged;
    call_count++;

    if (!tid_logged) {
        tid_logged = 1;
        long tid = syscall(SYS_gettid);
        render_tid = (pid_t)tid;
        dprintf(2, "[egldrain] render thread tid=%ld\n", tid);
    }

    if (call_count == 1 || call_count == 100 || call_count % 1000 == 0) {
        dprintf(2, "[egldrain] eglSwapBuffers call#%lu dpy=%p sfc=%p\n",
                call_count, dpy, sfc);
    }

    if (!real_eglSwapBuffers) {
        if (!real_dlsym) return 0;
        real_eglSwapBuffers = (eglSwapBuffers_t)real_dlsym(RTLD_NEXT, "eglSwapBuffers");
        if (!real_eglSwapBuffers) return 0;
    }
    if (!real_eglSurfaceAttrib && real_dlsym) {
        real_eglSurfaceAttrib = (eglSurfaceAttrib_t)real_dlsym(RTLD_NEXT, "eglSurfaceAttrib");
    }
    if (!real_eglQuerySurface && real_dlsym) {
        real_eglQuerySurface = (eglQuerySurface_t)real_dlsym(RTLD_NEXT, "eglQuerySurface");
    }

    /* Recursion guard: drain path calls us with in_drain=1. Just do a single
     * real swap and return; skip state update and PRESERVED setup. */
    if (in_drain) {
        return real_eglSwapBuffers(dpy, sfc);
    }

    /* Check surface cache for (dpy, sfc) pair */
    int is_new = 1;
    pthread_mutex_lock(&surface_mutex);
    for (int i = 0; i < surface_count; i++) {
        if (surface_cache[i].dpy == dpy && surface_cache[i].sfc == sfc) {
            is_new = 0;
            break;
        }
    }
    if (is_new && surface_count < MAX_SURFACES) {
        surface_cache[surface_count].dpy = dpy;
        surface_cache[surface_count].sfc = sfc;
        surface_count++;
    }
    pthread_mutex_unlock(&surface_mutex);

    if (is_new && real_eglSurfaceAttrib) {
        dprintf(2, "[egldrain] new surface call#%lu dpy=%p sfc=%p\n",
                call_count, dpy, sfc);
        EGLBoolean ok = real_eglSurfaceAttrib(dpy, sfc, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
        dprintf(2, "[egldrain] PRESERVED set on dpy=%p sfc=%p -> rc=%u\n",
                dpy, sfc, (unsigned)ok);
        if (real_eglQuerySurface) {
            EGLint val = 0;
            EGLBoolean qrc = real_eglQuerySurface(dpy, sfc, EGL_SWAP_BEHAVIOR, &val);
            dprintf(2, "[egldrain] PRESERVED query on dpy=%p sfc=%p -> qrc=%u val=0x%x\n",
                    dpy, sfc, (unsigned)qrc, (unsigned)val);
        }
    }

    EGLBoolean rc = real_eglSwapBuffers(dpy, sfc);
    if (rc == EGL_TRUE) {
        /* Arm the drain. The sleep hooks will fire it after 20 ms of idle. */
        pending_drains = 1;
        clock_gettime(CLOCK_MONOTONIC, &last_swap_ts);
        last_dpy = dpy;
        last_sfc = sfc;
    }
    return rc;
}

/* nanosleep hook. Only the render thread runs the drain; all other threads
 * fast-path through to the real syscall with zero extra overhead. */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!real_nanosleep) {
        if (!real_dlsym) {
            errno = ENOSYS;
            return -1;
        }
        real_nanosleep = (nanosleep_t)real_dlsym(RTLD_NEXT, "nanosleep");
        if (!real_nanosleep) {
            errno = ENOSYS;
            return -1;
        }
    }

    /* Fast path: non-render thread or render_tid not yet known. */
    pid_t rtid = render_tid;
    if (rtid == 0 || (pid_t)syscall(SYS_gettid) != rtid) {
        return real_nanosleep(req, rem);
    }

    maybe_drain();
    return real_nanosleep(req, rem);
}

/* clock_nanosleep hook — same shape as nanosleep hook. */
int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *req, struct timespec *rem) {
    if (!real_clock_nanosleep) {
        if (!real_dlsym) {
            errno = ENOSYS;
            return -1;
        }
        real_clock_nanosleep = (clock_nanosleep_t)real_dlsym(RTLD_NEXT, "clock_nanosleep");
        if (!real_clock_nanosleep) {
            errno = ENOSYS;
            return -1;
        }
    }

    pid_t rtid = render_tid;
    if (rtid == 0 || (pid_t)syscall(SYS_gettid) != rtid) {
        return real_clock_nanosleep(clock_id, flags, req, rem);
    }

    maybe_drain();
    return real_clock_nanosleep(clock_id, flags, req, rem);
}

/* dlsym hook. Intercepts lookups for the two EGL entry points we care
 * about. Everything else is passed through unchanged. */
void* dlsym(void* handle, const char* symbol) {
    if (!real_dlsym) {
        real_dlsym = (dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
        if (!real_dlsym) return NULL;
    }
    void* result = real_dlsym(handle, symbol);
    if (!result) return result;

    if (strcmp(symbol, "eglSwapBuffers") == 0) {
        if (!real_eglSwapBuffers) real_eglSwapBuffers = (eglSwapBuffers_t)result;
        /* SDL2 dlopens libEGL with RTLD_LOCAL → symbols are not in the global
         * namespace, so RTLD_NEXT can't see them. Opportunistically use SDL2's
         * own handle to pre-resolve the other entry points we need. */
        if (!real_eglSurfaceAttrib) {
            real_eglSurfaceAttrib = (eglSurfaceAttrib_t)real_dlsym(handle, "eglSurfaceAttrib");
        }
        if (!real_eglQuerySurface) {
            real_eglQuerySurface = (eglQuerySurface_t)real_dlsym(handle, "eglQuerySurface");
        }
        static int logged;
        if (!logged) {
            logged = 1;
            dprintf(2, "[egldrain] dlsym(eglSwapBuffers) hooked: real=%p wrapper=%p attrib=%p query=%p\n",
                    result, (void*)&eglSwapBuffers,
                    (void*)real_eglSurfaceAttrib, (void*)real_eglQuerySurface);
        }
        return (void*)&eglSwapBuffers;
    }
    if (strcmp(symbol, "eglSurfaceAttrib") == 0) {
        if (!real_eglSurfaceAttrib) real_eglSurfaceAttrib = (eglSurfaceAttrib_t)result;
        /* Pass through — no hook needed on this entry point. */
    }
    return result;
}
