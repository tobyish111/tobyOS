/* programs/linux-egltest/main.c
 *
 * Tier 3 Phase 1c: drive REAL Mesa against our DRM render node and let
 * it tell us what is missing.
 *
 * Slice 98's DRMTEST proves the ioctls WE wrote work when WE call them.
 * That is a much weaker claim than "Mesa works", because Mesa exercises
 * paths we never thought about. This client is the smallest program
 * that makes Mesa do the real thing:
 *
 *     eglGetPlatformDisplay(SURFACELESS) -> eglInitialize
 *     -> eglChooseConfig -> eglCreateContext -> eglMakeCurrent
 *     -> glGetString(GL_RENDERER)
 *
 * eglInitialize is where Mesa loads dri/virtio_gpu_dri.so, opens
 * /dev/dri/renderD128 and runs its own capability handshake. THE
 * DELIVERABLE IS THE GAP LIST: whatever "[drm] UNHANDLED ioctl" and
 * "[linux] UNHANDLED syscall" lines appear while this runs are exactly
 * the work remaining -- the same instrument-first method that closed
 * the Track B bring-up.
 *
 * GL_RENDERER is the verdict: "virgl" means the guest renders on the
 * host GPU; "llvmpipe"/"softpipe" means Mesa fell back to software
 * (still informative -- the EGL stack loaded -- but not tier 3).
 *
 * EGL/GL are resolved with dlopen at RUNTIME, not linked: the Debian
 * libEGL cannot be fed to the cross toolchain without dragging its whole
 * /lib64 closure in, and dlopen is exactly how chrome loads EGL anyway.
 *
 * exit_group: 3 = GPU (virgl), 4 = software fallback, 10 = loader,
 * 11..15 = the numbered EGL step that failed.
 */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned int EGLenum;
typedef int EGLint;
typedef unsigned int EGLBoolean;

#define EGL_NO_DISPLAY        ((EGLDisplay)0)
#define EGL_NO_CONTEXT        ((EGLContext)0)
#define EGL_NO_SURFACE        ((EGLSurface)0)
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_PLATFORM_GBM_MESA 0x31D7
#define EGL_SURFACE_TYPE      0x3033
#define EGL_PBUFFER_BIT       0x0001
#define EGL_RENDERABLE_TYPE   0x3040
#define EGL_OPENGL_ES2_BIT    0x0004
#define EGL_RED_SIZE          0x3024
#define EGL_GREEN_SIZE        0x3023
#define EGL_BLUE_SIZE         0x3022
#define EGL_NONE              0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API     0x30A0
#define EGL_VENDOR            0x3053
#define EGL_TRUE              1

#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02

static EGLDisplay (*p_eglGetPlatformDisplay)(EGLenum, void *, const EGLint *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglBindAPI)(EGLenum);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                       EGLint, EGLint *);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                        const EGLint *);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                                      EGLContext);
static EGLint     (*p_eglGetError)(void);
static const char *(*p_eglQueryString)(EGLDisplay, EGLint);
static const unsigned char *(*p_glGetString)(unsigned int);

static void *bind_sym(void *h, const char *name) {
    void *s = dlsym(h, name);
    if (!s) printf("[egltest] dlsym %s FAILED\n", name);
    return s;
}

/* Slice 100: ground-truth check of the sysfs tree libdrm validates a DRM
 * fd against. Theorising about which file libdrm wants is exactly the
 * mistake this arc keeps paying for -- ask the kernel directly and print
 * the answers, so the next step is chosen from data. */
static void probe_sysfs(void) {
    static const char *paths[] = {
        "/sys/dev/char/226:128",
        "/sys/dev/char/226:128/device",
        "/sys/dev/char/226:128/device/drm",
        "/sys/dev/char/226:128/device/vendor",
        "/sys/dev/char/226:128/device/uevent",
    };
    struct stat st;
    for (unsigned i = 0; i < sizeof(paths)/sizeof(paths[0]); i++)
        printf("[egltest] stat %-42s %s\n", paths[i],
               stat(paths[i], &st) == 0 ? "OK" : "MISSING");
    char lnk[128];
    long n = readlink("/sys/dev/char/226:128/device/subsystem", lnk,
                      sizeof(lnk) - 1);
    if (n > 0) { lnk[n] = 0; printf("[egltest] readlink subsystem -> %s\n", lnk); }
    else       { printf("[egltest] readlink subsystem FAILED\n"); }
    int fd = open("/sys/dev/char/226:128/device/vendor", O_RDONLY);
    if (fd >= 0) {
        char b[32]; long r = read(fd, b, sizeof(b) - 1);
        if (r > 0) { b[r] = 0; printf("[egltest] vendor file: %s", b); }
        close(fd);
    }
    fflush(stdout);
}

int main(void) {
    printf("[egltest] start\n");
    fflush(stdout);
    probe_sysfs();

    /* Debian's libEGL.so.1 is libglvnd, which finds its vendor driver
     * through /usr/share/glvnd/egl_vendor.d/*.json. Without that config
     * staged, glvnd has no vendor and every call returns
     * EGL_BAD_PARAMETER (0x300c) -- measured, gap-list entry #2. Load
     * Mesa's EGL directly when that happens: the layer under test is
     * Mesa -> libdrm -> /dev/dri, not glvnd's dispatch. (Staging the
     * JSON is still the right fix for chrome, which links libEGL.so.1.) */
    void *hegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!hegl) {
        printf("[egltest] dlopen EGL FAILED: %s\n", dlerror());
        return 10;
    }
    void *hgl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!hgl) {
        printf("[egltest] dlopen libGLESv2.so.2 FAILED: %s\n", dlerror());
        return 10;
    }
    printf("[egltest] EGL + GLESv2 loaded\n");
    fflush(stdout);

    p_eglGetPlatformDisplay = bind_sym(hegl, "eglGetPlatformDisplay");
    p_eglInitialize         = bind_sym(hegl, "eglInitialize");
    p_eglBindAPI            = bind_sym(hegl, "eglBindAPI");
    p_eglChooseConfig       = bind_sym(hegl, "eglChooseConfig");
    p_eglCreateContext      = bind_sym(hegl, "eglCreateContext");
    p_eglMakeCurrent        = bind_sym(hegl, "eglMakeCurrent");
    p_eglGetError           = bind_sym(hegl, "eglGetError");
    p_eglQueryString        = bind_sym(hegl, "eglQueryString");
    p_glGetString           = bind_sym(hgl,  "glGetString");
    if (!p_eglGetPlatformDisplay || !p_eglInitialize || !p_glGetString)
        return 10;

    /* PLATFORM CHOICE, and it matters (gap-list entry #5).
     *
     * SURFACELESS made Mesa call libdrm's drmGetDevices2(), which
     * ENUMERATES devices by listing /dev/dri and walking
     * /sys/dev/char/<maj>:<min>/... -- neither of which we serve -- so
     * Mesa concluded there was no GPU and loaded swrast_dri.so without
     * ever opening our node.
     *
     * GBM takes an fd we open OURSELVES: no enumeration, Mesa asks the
     * fd what it is (DRM_IOCTL_VERSION -> "virtio_gpu") and loads that
     * driver. It is also the platform chrome's Ozone/GBM path uses, so
     * it is the more relevant one to prove. Surfaceless is kept as a
     * fallback so this client still reports something useful once
     * enumeration exists. */
    EGLDisplay dpy = EGL_NO_DISPLAY;
    void *hgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (hgbm) {
        void *(*p_gbm_create_device)(int) = bind_sym(hgbm, "gbm_create_device");
        int fd = open("/dev/dri/renderD128", O_RDWR);
        if (fd < 0) fd = open("/dev/dri/card0", O_RDWR);
        printf("[egltest] /dev/dri fd=%d\n", fd);
        fflush(stdout);
        if (p_gbm_create_device && fd >= 0) {
            void *gbm = p_gbm_create_device(fd);
            printf("[egltest] gbm_create_device -> %s\n", gbm ? "ok" : "NULL");
            fflush(stdout);
            if (gbm) {
                dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm, NULL);
                printf("[egltest] GBM display %s\n",
                       dpy == EGL_NO_DISPLAY ? "FAILED" : "ok");
                fflush(stdout);
            }
        }
    } else {
        printf("[egltest] libgbm.so.1 not loadable: %s\n", dlerror());
    }
    if (dpy == EGL_NO_DISPLAY) {
        printf("[egltest] falling back to surfaceless\n");
        fflush(stdout);
        dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                      NULL, NULL);
    }
    if (dpy == EGL_NO_DISPLAY) {
        printf("[egltest] eglGetPlatformDisplay FAILED (err=0x%x)\n",
               p_eglGetError());
        return 11;
    }
    printf("[egltest] display ok\n");
    fflush(stdout);

    EGLint major = 0, minor = 0;
    if (p_eglInitialize(dpy, &major, &minor) != EGL_TRUE) {
        printf("[egltest] eglInitialize FAILED (err=0x%x) -- the [drm] and "
               "[linux] UNHANDLED lines above ARE the gap list\n",
               p_eglGetError());
        return 12;
    }
    printf("[egltest] EGL %d.%d initialized\n", major, minor);
    const char *vend = p_eglQueryString(dpy, EGL_VENDOR);
    if (vend) printf("[egltest] EGL_VENDOR: %s\n", vend);
    fflush(stdout);

    p_eglBindAPI(EGL_OPENGL_ES_API);

    /* NO EGL_SURFACE_TYPE constraint. Asking for EGL_PBUFFER_BIT
     * returned zero configs with EGL_SUCCESS: a GBM display advertises
     * WINDOW configs (gbm_surface), not pbuffers. We render to no
     * surface at all (surfaceless context), so any config will do. */
    EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (p_eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) != EGL_TRUE ||
        ncfg < 1) {
        printf("[egltest] eglChooseConfig FAILED (err=0x%x n=%d)\n",
               p_eglGetError(), ncfg);
        return 13;
    }
    printf("[egltest] config ok\n");
    fflush(stdout);

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        printf("[egltest] eglCreateContext FAILED (err=0x%x)\n",
               p_eglGetError());
        return 14;
    }
    if (p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                         ctx) != EGL_TRUE) {
        printf("[egltest] eglMakeCurrent FAILED (err=0x%x)\n",
               p_eglGetError());
        return 15;
    }
    printf("[egltest] context current\n");
    fflush(stdout);

    const unsigned char *r   = p_glGetString(GL_RENDERER);
    const unsigned char *v   = p_glGetString(GL_VENDOR);
    const unsigned char *ver = p_glGetString(GL_VERSION);
    printf("[egltest] GL_VENDOR:   %s\n", v   ? (const char *)v   : "(null)");
    printf("[egltest] GL_RENDERER: %s\n", r   ? (const char *)r   : "(null)");
    printf("[egltest] GL_VERSION:  %s\n", ver ? (const char *)ver : "(null)");
    fflush(stdout);

    if (r && (strstr((const char *)r, "virgl") ||
              strstr((const char *)r, "Virgl"))) {
        printf("[egltest] PASS: rendering through virgl on the HOST GPU\n");
        return 3;
    }
    printf("[egltest] SOFTWARE fallback (%s): the EGL stack loads, but the "
           "DRM path was not used\n", r ? (const char *)r : "?");
    return 4;
}
