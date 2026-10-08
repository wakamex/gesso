// libva and libva-drm loaded at run time. FFmpeg's VAAPI code calls these functions directly; defining
// them here, rather than linking the system libraries, lets a gesso app start on systems without
// libva, where VAAPI simply reports itself unavailable and decoding falls back to software.
#include <dlfcn.h>
#include <pthread.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

static void *va, *va_drm;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void) {
    va = dlopen("libva.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (va) va_drm = dlopen("libva-drm.so.2", RTLD_NOW);
}

static void *sym(void **lib, const char *name) {
    pthread_once(&once, load);
    return *lib ? dlsym(*lib, name) : NULL;
}

// Each wrapper looks its function up once and returns `fail` when it is missing.
#define WRAP(lib, ret, fail, name, params, args) \
    ret name params { \
        static ret(*fn) params; \
        if (!fn) fn = (ret(*) params)sym(&lib, #name); \
        return fn ? fn args : fail; \
    }

#define ERR VA_STATUS_ERROR_UNKNOWN
WRAP(va, VAStatus, ERR, vaBeginPicture, (VADisplay d, VAContextID c, VASurfaceID s), (d, c, s))
WRAP(va, VAStatus, ERR, vaCreateBuffer, (VADisplay d, VAContextID c, VABufferType t, unsigned int size, unsigned int n, void *data, VABufferID *id), (d, c, t, size, n, data, id))
WRAP(va, VAStatus, ERR, vaCreateConfig, (VADisplay d, VAProfile p, VAEntrypoint e, VAConfigAttrib *a, int n, VAConfigID *id), (d, p, e, a, n, id))
WRAP(va, VAStatus, ERR, vaCreateContext, (VADisplay d, VAConfigID c, int w, int h, int flag, VASurfaceID *targets, int n, VAContextID *id), (d, c, w, h, flag, targets, n, id))
WRAP(va, VAStatus, ERR, vaCreateImage, (VADisplay d, VAImageFormat *f, int w, int h, VAImage *image), (d, f, w, h, image))
WRAP(va, VAStatus, ERR, vaCreateSurfaces, (VADisplay d, unsigned int format, unsigned int w, unsigned int h, VASurfaceID *s, unsigned int n, VASurfaceAttrib *a, unsigned int na), (d, format, w, h, s, n, a, na))
WRAP(va, VAStatus, ERR, vaDeriveImage, (VADisplay d, VASurfaceID s, VAImage *image), (d, s, image))
WRAP(va, VAStatus, ERR, vaDestroyBuffer, (VADisplay d, VABufferID b), (d, b))
WRAP(va, VAStatus, ERR, vaDestroyConfig, (VADisplay d, VAConfigID c), (d, c))
WRAP(va, VAStatus, ERR, vaDestroyContext, (VADisplay d, VAContextID c), (d, c))
WRAP(va, VAStatus, ERR, vaDestroyImage, (VADisplay d, VAImageID i), (d, i))
WRAP(va, VAStatus, ERR, vaDestroySurfaces, (VADisplay d, VASurfaceID *s, int n), (d, s, n))
WRAP(va, VAStatus, ERR, vaEndPicture, (VADisplay d, VAContextID c), (d, c))
WRAP(va, const char *, "libva is not available", vaErrorStr, (VAStatus s), (s))
WRAP(va_drm, VADisplay, NULL, vaGetDisplayDRM, (int fd), (fd))
WRAP(va, VAStatus, ERR, vaGetImage, (VADisplay d, VASurfaceID s, int x, int y, unsigned int w, unsigned int h, VAImageID i), (d, s, x, y, w, h, i))
WRAP(va, VAStatus, ERR, vaInitialize, (VADisplay d, int *major, int *minor), (d, major, minor))
// vaMapBuffer2 arrived in libva 2.21; older libva has only vaMapBuffer, which maps for reading and writing.
VAStatus vaMapBuffer2(VADisplay d, VABufferID b, void **p, uint32_t flags) {
    static VAStatus (*fn2)(VADisplay, VABufferID, void **, uint32_t);
    static VAStatus (*fn)(VADisplay, VABufferID, void **);
    if (!fn2 && !fn && !(fn2 = (VAStatus(*)(VADisplay, VABufferID, void **, uint32_t))sym(&va, "vaMapBuffer2")))
        fn = (VAStatus(*)(VADisplay, VABufferID, void **))sym(&va, "vaMapBuffer");
    return fn2 ? fn2(d, b, p, flags) : fn ? fn(d, b, p) : ERR;
}
WRAP(va, int, 0, vaMaxNumImageFormats, (VADisplay d), (d))
WRAP(va, int, 0, vaMaxNumProfiles, (VADisplay d), (d))
WRAP(va, int, 0, vaMaxNumEntrypoints, (VADisplay d), (d))
WRAP(va, VAStatus, ERR, vaPutImage, (VADisplay d, VASurfaceID s, VAImageID i, int sx, int sy, unsigned int sw, unsigned int sh, int dx, int dy, unsigned int dw, unsigned int dh), (d, s, i, sx, sy, sw, sh, dx, dy, dw, dh))
WRAP(va, VAStatus, ERR, vaQueryConfigEntrypoints, (VADisplay d, VAProfile p, VAEntrypoint *e, int *n), (d, p, e, n))
WRAP(va, VAStatus, ERR, vaQueryConfigProfiles, (VADisplay d, VAProfile *p, int *n), (d, p, n))
WRAP(va, VAStatus, ERR, vaQueryImageFormats, (VADisplay d, VAImageFormat *f, int *n), (d, f, n))
WRAP(va, VAStatus, ERR, vaQuerySurfaceAttributes, (VADisplay d, VAConfigID c, VASurfaceAttrib *a, unsigned int *n), (d, c, a, n))
WRAP(va, const char *, NULL, vaQueryVendorString, (VADisplay d), (d))
WRAP(va, VAStatus, ERR, vaRenderPicture, (VADisplay d, VAContextID c, VABufferID *b, int n), (d, c, b, n))
WRAP(va, VAStatus, ERR, vaSetDriverName, (VADisplay d, char *name), (d, name))
WRAP(va, VAMessageCallback, NULL, vaSetErrorCallback, (VADisplay d, VAMessageCallback cb, void *user), (d, cb, user))
WRAP(va, VAMessageCallback, NULL, vaSetInfoCallback, (VADisplay d, VAMessageCallback cb, void *user), (d, cb, user))
WRAP(va, VAStatus, ERR, vaSyncSurface, (VADisplay d, VASurfaceID s), (d, s))
WRAP(va, VAStatus, ERR, vaTerminate, (VADisplay d), (d))
WRAP(va, VAStatus, ERR, vaUnmapBuffer, (VADisplay d, VABufferID b), (d, b))
WRAP(va, VAStatus, ERR, vaExportSurfaceHandle, (VADisplay d, VASurfaceID s, uint32_t type, uint32_t flags, void *desc), (d, s, type, flags, desc))
