#define _POSIX_C_SOURCE 200809L  // O_CLOEXEC
#include "gs_video.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <math.h>

#if defined(__linux__)
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1  // SDL's copies of the Khronos headers, not the build host's
#include <SDL3/SDL_egl.h>
#include <SDL3/SDL_opengles2.h>
#include <fcntl.h>
#include <libavutil/hwcontext_vaapi.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#elif defined(_WIN32)
#define COBJMACROS
#include <d3d11.h>
#include <libavutil/hwcontext_d3d11va.h>
#endif

typedef enum { SOFTWARE, VAAPI, D3D11VA, VIDEOTOOLBOX } path;
static const char *path_names[] = { "software", "vaapi", "d3d11va", "videotoolbox" };

// What gs_video_prepare chose, for the renderer created after it.
static path prepared = SOFTWARE;
#if defined(__linux__)
static char va_device[32];  // the render node whose VAAPI driver decodes H.264
#endif

struct gs_video {
    SDL_Renderer *renderer;
    AVCodecContext *codec;
    AVPacket *packet;
    path path;
    enum AVPixelFormat hw_format;
    // Decoded frames waiting to be shown, oldest first.
    SDL_Mutex *lock;
    SDL_Condition *space;
    AVFrame **queue;
    int cap, count;
    bool stopped, failed;
    // The frame on screen, kept until the next replaces it, since hardware textures read from it.
    AVFrame *current;
    SDL_Texture *texture;
    int tex_w, tex_h;
    SDL_PixelFormat tex_format;
    SDL_Colorspace tex_colorspace;
    long long decoded, shown, dropped;
#if defined(__linux__)
    PFNEGLCREATEIMAGEPROC egl_create_image;
    PFNEGLDESTROYIMAGEPROC egl_destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target;
    PFNGLACTIVETEXTUREPROC gl_active_texture;
    PFNGLBINDTEXTUREPROC gl_bind_texture;
    bool egl_modifiers;
#elif defined(_WIN32)
    ID3D11DeviceContext *d3d11_context;
#endif
};

// ---- Choosing the path ----

#if defined(__linux__)
// True when the render node's VAAPI driver decodes H.264 High profile. Fedora's Mesa, for one, ships
// VAAPI drivers built without H.264.
static bool vaapi_decodes_h264(const char *node) {
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = false;
    VADisplay d = vaGetDisplayDRM(fd);
    int major, minor;
    if (d && vaInitialize(d, &major, &minor) == VA_STATUS_SUCCESS) {
        int n = vaMaxNumEntrypoints(d), count = 0;
        VAEntrypoint *e = n > 0 ? SDL_malloc(n * sizeof *e) : NULL;
        if (e && vaQueryConfigEntrypoints(d, VAProfileH264High, e, &count) == VA_STATUS_SUCCESS)
            for (int i = 0; i < count; i++) ok |= e[i] == VAEntrypointVLD;
        SDL_free(e);
        vaTerminate(d);
    }
    close(fd);
    return ok;
}
#endif

const char *gs_video_prepare(bool hardware) {
    prepared = SOFTWARE;
    if (!hardware) return NULL;
#if defined(__linux__)
    for (int i = 128; i < 136 && prepared == SOFTWARE; i++) {
        SDL_snprintf(va_device, sizeof va_device, "/dev/dri/renderD%d", i);
        if (vaapi_decodes_h264(va_device)) prepared = VAAPI;
    }
    if (prepared != VAAPI) return NULL;
    // Decoded surfaces reach OpenGL ES as dma-bufs through EGL.
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    SDL_SetHint("SDL_RENDER_OPENGL_NV12_RG_SHADER", "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    return "opengles2";
#elif defined(_WIN32)
    // The decoder uses the renderer's device from its own thread, and SDL makes the device
    // single-threaded unless asked otherwise.
    SDL_SetHint(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1");
    prepared = D3D11VA;
    return "direct3d11";
#elif defined(__APPLE__)
    prepared = VIDEOTOOLBOX;
    return "metal";
#else
    return NULL;
#endif
}

// Sets up the hardware device for the renderer, or returns false to decode in software.
static bool open_hardware(gs_video *v) {
    const char *name = SDL_GetRendererName(v->renderer);
    SDL_PropertiesID props = SDL_GetRendererProperties(v->renderer);
    (void)props;
#if defined(__linux__)
    if (prepared != VAAPI || SDL_strcmp(name, "opengles2") != 0) return false;
    const char *ext = NULL;
    EGLDisplay display = SDL_EGL_GetCurrentDisplay();
    PFNEGLQUERYSTRINGPROC query = (PFNEGLQUERYSTRINGPROC)SDL_EGL_GetProcAddress("eglQueryString");
    if (display && query) ext = query(display, EGL_EXTENSIONS);
    if (!ext || !SDL_strstr(ext, "EGL_EXT_image_dma_buf_import")) return false;
    v->egl_modifiers = SDL_strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    v->egl_create_image = (PFNEGLCREATEIMAGEPROC)SDL_EGL_GetProcAddress("eglCreateImage");
    v->egl_destroy_image = (PFNEGLDESTROYIMAGEPROC)SDL_EGL_GetProcAddress("eglDestroyImage");
    v->gl_image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)SDL_GL_GetProcAddress("glEGLImageTargetTexture2DOES");
    v->gl_active_texture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
    v->gl_bind_texture = (PFNGLBINDTEXTUREPROC)SDL_GL_GetProcAddress("glBindTexture");
    if (!v->egl_create_image || !v->egl_destroy_image || !v->gl_image_target || !v->gl_active_texture || !v->gl_bind_texture) return false;
    if (av_hwdevice_ctx_create(&v->codec->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, va_device, NULL, 0) < 0) return false;
    v->path = VAAPI, v->hw_format = AV_PIX_FMT_VAAPI;
    return true;
#elif defined(_WIN32)
    ID3D11Device *device = SDL_GetPointerProperty(props, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, NULL);
    if (prepared != D3D11VA || SDL_strcmp(name, "direct3d11") != 0 || !device) return false;
    // The decoder and the renderer share the device from different threads.
    ID3D10Multithread *mt;
    if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return false;
    AVD3D11VADeviceContext *hw = ((AVHWDeviceContext *)ref->data)->hwctx;
    hw->device = device;
    ID3D11Device_AddRef(device);
    ID3D11Device_GetImmediateContext(device, &hw->device_context);
    v->d3d11_context = hw->device_context;
    ID3D11DeviceContext_AddRef(v->d3d11_context);
    if (av_hwdevice_ctx_init(ref) < 0) return av_buffer_unref(&ref), false;
    v->codec->hw_device_ctx = ref;
    v->path = D3D11VA, v->hw_format = AV_PIX_FMT_D3D11;
    return true;
#elif defined(__APPLE__)
    if (prepared != VIDEOTOOLBOX || SDL_strcmp(name, "metal") != 0) return false;
    if (av_hwdevice_ctx_create(&v->codec->hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0) < 0) return false;
    v->path = VIDEOTOOLBOX, v->hw_format = AV_PIX_FMT_VIDEOTOOLBOX;
    return true;
#else
    (void)name;
    return false;
#endif
}

// The decoder offers the hardware format when its device can decode the stream; otherwise the
// stream decodes in software.
static enum AVPixelFormat get_format(AVCodecContext *c, const enum AVPixelFormat *formats) {
    gs_video *v = c->opaque;
    for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++)
        if (v->hw_format != AV_PIX_FMT_NONE && *f == v->hw_format) return *f;
    for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_YUV420P || *f == AV_PIX_FMT_YUVJ420P || *f == AV_PIX_FMT_NV12) {
            v->path = SOFTWARE;
            return *f;
        }
    return AV_PIX_FMT_NONE;
}

gs_video *gs_video_new(SDL_Renderer *r, int queue, int threads) {
    const AVCodec *h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    gs_video *v = SDL_calloc(1, sizeof *v);
    if (!v || !h264) return SDL_free(v), NULL;
    v->renderer = r;
    v->cap = queue < 2 ? 2 : queue;
    v->queue = SDL_calloc(v->cap, sizeof *v->queue);
    v->lock = SDL_CreateMutex();
    v->space = SDL_CreateCondition();
    v->codec = avcodec_alloc_context3(h264);
    v->packet = av_packet_alloc();
    v->hw_format = AV_PIX_FMT_NONE;
    if (!v->queue || !v->lock || !v->space || !v->codec || !v->packet) return gs_video_free(v), NULL;
    v->codec->opaque = v;
    v->codec->get_format = get_format;
    v->codec->pkt_timebase = (AVRational){ 1, 90000 };
    if (!open_hardware(v)) {
        // Frame threads decode several frames at once, each holding a picture in memory.
        int cores = SDL_GetNumLogicalCPUCores();
        v->codec->thread_count = threads > 0 ? threads : cores < 4 ? cores : 4;
        v->codec->thread_type = FF_THREAD_FRAME;
    }
    if (avcodec_open2(v->codec, h264, NULL) < 0) return gs_video_free(v), NULL;
    return v;
}

static void clear_queue(gs_video *v) {
    for (int i = 0; i < v->count; i++) av_frame_free(&v->queue[i]);
    v->count = 0;
}

void gs_video_free(gs_video *v) {
    if (!v) return;
    clear_queue(v);
    av_frame_free(&v->current);
    if (v->texture) SDL_DestroyTexture(v->texture);
    avcodec_free_context(&v->codec);
    av_packet_free(&v->packet);
#if defined(_WIN32)
    if (v->d3d11_context) ID3D11DeviceContext_Release(v->d3d11_context);
#endif
    SDL_DestroyCondition(v->space);
    SDL_DestroyMutex(v->lock);
    SDL_free(v->queue);
    SDL_free(v);
}

// ---- Decoding ----

static bool push(gs_video *v, AVFrame *f) {
    SDL_LockMutex(v->lock);
    while (v->count == v->cap && !v->stopped) SDL_WaitCondition(v->space, v->lock);
    bool ok = !v->stopped;
    if (ok) v->queue[v->count++] = f, v->decoded++;
    SDL_UnlockMutex(v->lock);
    if (!ok) av_frame_free(&f);
    return ok;
}

bool gs_video_decode(gs_video *v, const uint8_t *data, size_t len, double pts) {
    if (v->stopped || v->failed || av_new_packet(v->packet, (int)len) < 0) return false;
    SDL_memcpy(v->packet->data, data, len);
    v->packet->pts = v->packet->dts = llround(pts * 90000);
    int sent = avcodec_send_packet(v->codec, v->packet);
    av_packet_unref(v->packet);
    // A damaged access unit is skipped; the decoder recovers at the next one it can use.
    if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_INVALIDDATA) return !(v->failed = true);
    for (;;) {
        AVFrame *f = av_frame_alloc();
        int got = f ? avcodec_receive_frame(v->codec, f) : AVERROR(ENOMEM);
        if (got < 0) {
            av_frame_free(&f);
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF || got == AVERROR_INVALIDDATA) return !v->stopped;
            return !(v->failed = true);
        }
        if (!push(v, f)) return false;
    }
}

void gs_video_flush(gs_video *v) {
    avcodec_flush_buffers(v->codec);
    SDL_LockMutex(v->lock);
    clear_queue(v);
    SDL_SignalCondition(v->space);
    SDL_UnlockMutex(v->lock);
}

void gs_video_stop(gs_video *v) {
    SDL_LockMutex(v->lock);
    v->stopped = true;
    SDL_BroadcastCondition(v->space);
    SDL_UnlockMutex(v->lock);
}

// ---- Showing ----

static double frame_seconds(const AVFrame *f) {
    int64_t t = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    return t == AV_NOPTS_VALUE ? 0 : t / 90000.0;
}

static SDL_Colorspace frame_colorspace(const AVFrame *f) {
    return SDL_DEFINE_COLORSPACE(SDL_COLOR_TYPE_YCBCR, f->color_range, f->color_primaries, f->color_trc, f->colorspace, f->chroma_location);
}

// Makes v->texture a w x h texture of `format` for frame f, recreating it when anything differs or `fresh` is set.
static bool texture_for(gs_video *v, const AVFrame *f, SDL_PixelFormat format, int access, int w, int h, bool fresh, SDL_PropertiesID extra) {
    SDL_Colorspace cs = frame_colorspace(f);
    if (v->texture && !fresh && v->tex_w == w && v->tex_h == h && v->tex_format == format && v->tex_colorspace == cs) return true;
    if (v->texture) SDL_DestroyTexture(v->texture);
    SDL_PropertiesID p = extra ? extra : SDL_CreateProperties();
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, cs);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, access);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
    v->texture = SDL_CreateTextureWithProperties(v->renderer, p);
    SDL_DestroyProperties(p);
    if (!v->texture) return false;
    SDL_SetTextureBlendMode(v->texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(v->texture, SDL_SCALEMODE_LINEAR);
    v->tex_w = w, v->tex_h = h, v->tex_format = format, v->tex_colorspace = cs;
    return true;
}

static void hw_size(const AVFrame *f, int *w, int *h) {
    const AVHWFramesContext *frames = (const AVHWFramesContext *)f->hw_frames_ctx->data;
    *w = frames->width, *h = frames->height;
}

#if defined(__linux__)
// Imports a VAAPI surface's planes, exported as dma-bufs, into the two OpenGL ES textures of an NV12 texture.
static bool show_vaapi(gs_video *v, const AVFrame *f) {
    const AVHWDeviceContext *dev = (const AVHWDeviceContext *)((const AVHWFramesContext *)f->hw_frames_ctx->data)->device_ref->data;
    VADisplay display = ((const AVVAAPIDeviceContext *)dev->hwctx)->display;
    VASurfaceID surface = (VASurfaceID)(uintptr_t)f->data[3];
    VADRMPRIMESurfaceDescriptor desc;
    if (vaSyncSurface(display, surface) != VA_STATUS_SUCCESS ||
        vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc) != VA_STATUS_SUCCESS)
        return SDL_SetError("Couldn't export the VAAPI surface");
    bool ok = desc.num_layers == 2;
    int w, h;
    hw_size(f, &w, &h);
    // A new texture each frame: the images it was bound to hold the surface's memory.
    ok = ok && texture_for(v, f, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STATIC, w, h, true, 0);
    SDL_PropertiesID tp = ok ? SDL_GetTextureProperties(v->texture) : 0;
    GLuint tex[2] = { (GLuint)SDL_GetNumberProperty(tp, SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0),
                      (GLuint)SDL_GetNumberProperty(tp, SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_UV_NUMBER, 0) };
    ok = ok && tex[0] && tex[1];
    EGLDisplay egl = SDL_EGL_GetCurrentDisplay();
    for (uint32_t i = 0; ok && i < 2; i++) {
        const uint32_t o = desc.layers[i].object_index[0];
        EGLAttrib a[20];
        int k = 0;
        a[k++] = EGL_LINUX_DRM_FOURCC_EXT, a[k++] = desc.layers[i].drm_format;
        a[k++] = EGL_WIDTH, a[k++] = w >> i;  // the chroma plane is half size
        a[k++] = EGL_HEIGHT, a[k++] = h >> i;
        a[k++] = EGL_DMA_BUF_PLANE0_FD_EXT, a[k++] = desc.objects[o].fd;
        a[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT, a[k++] = desc.layers[i].offset[0];
        a[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT, a[k++] = desc.layers[i].pitch[0];
        if (v->egl_modifiers) {
            a[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, a[k++] = (EGLAttrib)(desc.objects[o].drm_format_modifier & 0xffffffff);
            a[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, a[k++] = (EGLAttrib)(desc.objects[o].drm_format_modifier >> 32);
        }
        a[k] = EGL_NONE;
        EGLImage image = v->egl_create_image(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
        if (image == EGL_NO_IMAGE) {
            ok = SDL_SetError("Couldn't import a decoded surface into EGL");
            break;
        }
        v->gl_active_texture(GL_TEXTURE0 + i);
        v->gl_bind_texture(GL_TEXTURE_2D, tex[i]);
        v->gl_image_target(GL_TEXTURE_2D, image);
        v->egl_destroy_image(egl, image);
    }
    for (uint32_t i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);
    return ok;
}
#endif

#if defined(_WIN32)
// Copies the decoder's texture array slice into the renderer's texture, on the GPU.
static bool show_d3d11(gs_video *v, const AVFrame *f) {
    int w, h;
    hw_size(f, &w, &h);
    if (!texture_for(v, f, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STATIC, w, h, false, 0)) return false;
    ID3D11Resource *dst = SDL_GetPointerProperty(SDL_GetTextureProperties(v->texture), SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, NULL);
    if (!dst) return SDL_SetError("Couldn't get the texture's Direct3D 11 resource");
    ID3D11DeviceContext_CopySubresourceRegion(v->d3d11_context, dst, 0, 0, 0, 0, (ID3D11Resource *)f->data[0], (UINT)(uintptr_t)f->data[1], NULL);
    return true;
}
#endif

static bool show(gs_video *v, const AVFrame *f) {
    switch (f->format) {
#if defined(__linux__)
    case AV_PIX_FMT_VAAPI: return show_vaapi(v, f);
#endif
#if defined(_WIN32)
    case AV_PIX_FMT_D3D11: return show_d3d11(v, f);
#endif
    case AV_PIX_FMT_VIDEOTOOLBOX: {
        int w, h;
        hw_size(f, &w, &h);
        SDL_PropertiesID p = SDL_CreateProperties();
        SDL_SetPointerProperty(p, SDL_PROP_TEXTURE_CREATE_METAL_PIXELBUFFER_POINTER, f->data[3]);
        return texture_for(v, f, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STATIC, w, h, true, p);
    }
    case AV_PIX_FMT_NV12:
        return texture_for(v, f, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, f->width, f->height, false, 0) &&
               SDL_UpdateNVTexture(v->texture, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return texture_for(v, f, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width, f->height, false, 0) &&
               SDL_UpdateYUVTexture(v->texture, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
    default:
        return SDL_SetError("Unsupported decoded format %s", av_get_pix_fmt_name(f->format));
    }
}

SDL_Texture *gs_video_frame(gs_video *v, double clock, SDL_FRect *src) {
    AVFrame *next = NULL;
    SDL_LockMutex(v->lock);
    int taken = 0;
    while (taken < v->count && frame_seconds(v->queue[taken]) <= clock) taken++;
    if (taken) {
        for (int i = 0; i < taken - 1; i++) av_frame_free(&v->queue[i]);
        v->dropped += taken - 1;
        next = v->queue[taken - 1];
        SDL_memmove(v->queue, v->queue + taken, (v->count - taken) * sizeof *v->queue);
        v->count -= taken;
        SDL_SignalCondition(v->space);
    }
    SDL_UnlockMutex(v->lock);
    if (next) {
        if (show(v, next)) {
            av_frame_free(&v->current);
            v->current = next;
            v->shown++;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "gs_video: %s", SDL_GetError());
            av_frame_free(&next);
            v->dropped++;
        }
    }
    if (!v->current || !v->texture) return NULL;
    if (src) *src = (SDL_FRect){ 0, 0, (float)v->current->width, (float)v->current->height };
    return v->texture;
}

gs_video_info gs_video_get_info(gs_video *v) {
    SDL_LockMutex(v->lock);
    gs_video_info i = {
        .path = path_names[v->path],
        .width = v->current ? v->current->width : 0,
        .height = v->current ? v->current->height : 0,
        .queued = v->count,
        .decoded = v->decoded, .shown = v->shown, .dropped = v->dropped,
        .next_pts = v->count ? frame_seconds(v->queue[0]) : -1,
    };
    SDL_UnlockMutex(v->lock);
    return i;
}
