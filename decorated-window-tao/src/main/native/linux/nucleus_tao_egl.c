/**
 * JNI bridge: EGL renderer for the Tao backend on Linux.
 *
 * Drives Skia's GL backend through `GLAssembledInterface.createFromNativePointers`
 * + `DirectContext.makeGLWithInterface(...)`, which lets us hand Skia an
 * `eglGetProcAddress`-resolved set of GL entry points instead of the
 * GLX-flavoured `GrGLMakeNativeInterface()` that Skiko's own `MakeGL()` returns.
 *
 * Two attach paths share the helper:
 *   - `nativeAttachX11(Display*, Window, …)`     — `EGL_PLATFORM_X11_KHR`
 *   - `nativeAttachWayland(wl_display*, wl_surface*, …)` — `EGL_PLATFORM_WAYLAND_KHR`
 *                                                     + `wl_egl_window`
 *
 * Selection between this helper and the legacy `nucleus_tao_glx.c` is
 * driven by `nucleus.tao.linux.renderer` (system property) or
 * `NUCLEUS_TAO_LINUX_RENDERER` (env-var); see `TaoComposeSceneHostLinux`.
 *
 * Linked libraries: -ldl. libEGL.so.1, libGL.so.1 / libOpenGL.so.0,
 * libX11.so.6, libXext.so.6 and libwayland-egl.so.1 are dlopen-ed at
 * runtime so the build doesn't require the dev packages and the .so
 * ships standalone. libwayland-egl is only required for the Wayland
 * attach path; X11-only setups don't need it.
 *
 * Wayland architecture. tao 0.35 + GTK 3 owns the `wl_surface` and
 * paints a cairo-shm buffer to it on every draw signal (event_loop.rs:912
 * of tao 0.35); GTK delays its `xdg_wm_base.get_xdg_surface(wl_surface)`
 * setup until after the first draw. Rendering EGL directly onto that
 * surface races with GTK's commits and trips
 * `xdg_wm_base.error(invalid_surface_state)`. The fix:
 * `nativeAttachWayland` creates an owned `wl_subsurface` child of GTK's
 * surface — same architectural pattern as the X11 child-window fallback
 * we use for visual mismatches. GTK keeps owning the parent + xdg_toplevel,
 * we render into the subsurface in `set_desync` mode so our buffer commits
 * land independently. Implementation hand-rolls `wl_compositor.create_surface`,
 * `wl_subcompositor.get_subsurface`, `set_position` and `set_desync` against
 * libwayland-client.so.0 — the C protocol headers aren't pulled at build
 * time so the .so ships standalone.
 *
 * Selection: GDK auto-picks the backend (= native Wayland on Wayland
 * sessions, X11 elsewhere). Set `NUCLEUS_TAO_LINUX_RENDERER=x11` to force
 * XWayland — useful for apps that need X11-specific features Wayland
 * deliberately doesn't expose (always-on-top, programmatic positioning,
 * global pointer queries, etc.).
 *
 * TODO (planned follow-ups, in priority order):
 *   - `wp_fractional_scale_v1` + `wp_viewporter` binding to honor
 *     fractional HiDPI scales correctly on Wayland (GTK 3 reports only
 *     integer scales, so 125% / 150% sessions currently get a buffer at
 *     1x and the compositor upscales).
 *   - `zxdg_decoration_manager_v1.set_mode(client_side)` so KWin /
 *     Wayland-Plasma 5 doesn't draw a server-side titlebar around our
 *     custom-chrome windows.
 *   - Frame-callback occlusion timeout: `eglSwapBuffers` blocks
 *     indefinitely when the surface is fully occluded by another window
 *     on wlroots compositors. Detect via `xdg_toplevel.configure` losing
 *     the `activated` state and fall back to a polling redraw.
 *   - NVIDIA `egl-wayland2` runtime detection — driver < 560 with the
 *     legacy egl-wayland can't resize an EGLSurface (drag-resize is
 *     broken upstream); document the package as a runtime requirement.
 */

#include <jni.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "nucleus_tao_egl_internal.h"

#define NUCLEUS_TAO_EGL_DEBUG 0
#if NUCLEUS_TAO_EGL_DEBUG
#define DBG(...) fprintf(stderr, "[nucleus_tao_egl] " __VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

/* ── EGL types & constants (subset, re-declared) ────────────────────────── */

typedef void          *EGLDisplay;
typedef void          *EGLConfig;
typedef void          *EGLSurface;
typedef void          *EGLContext;
typedef int            EGLBoolean;
typedef int            EGLint;
typedef unsigned int   EGLenum;
typedef void          *EGLNativeDisplayType;
typedef unsigned long  EGLNativeWindowType;   /* X11 Window XID on Xlib */

#define EGL_TRUE                       1
#define EGL_FALSE                      0
#define EGL_NO_DISPLAY                 ((EGLDisplay) 0)
#define EGL_NO_CONTEXT                 ((EGLContext) 0)
#define EGL_NO_SURFACE                 ((EGLSurface) 0)
#define EGL_NONE                       0x3038
#define EGL_RED_SIZE                   0x3024
#define EGL_GREEN_SIZE                 0x3023
#define EGL_BLUE_SIZE                  0x3022
#define EGL_ALPHA_SIZE                 0x3021
#define EGL_DEPTH_SIZE                 0x3025
#define EGL_STENCIL_SIZE               0x3026
#define EGL_SAMPLES                    0x3031
#define EGL_SURFACE_TYPE               0x3033
#define EGL_RENDERABLE_TYPE            0x3040
#define EGL_NATIVE_VISUAL_ID           0x302E
#define EGL_WINDOW_BIT                 0x0004
#define EGL_OPENGL_BIT                 0x0008
#define EGL_OPENGL_API                 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION      0x3098
#define EGL_CONTEXT_MINOR_VERSION      0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK            0x30FD
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT  0x00000002
#define EGL_PLATFORM_X11_KHR           0x31D5
#define EGL_PLATFORM_WAYLAND_KHR       0x31D8
#define EGL_EXTENSIONS                 0x3055
#define EGL_COLOR_COMPONENT_TYPE_EXT   0x3339
#define EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT 0x333B
#define EGL_GL_COLORSPACE_KHR          0x309D
#define EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT 0x3350
#define EGL_GL_COLORSPACE_BT2020_PQ_EXT    0x3340

#define NUCLEUS_OUTPUT_SDR   0
#define NUCLEUS_OUTPUT_SCRGB 1
#define NUCLEUS_OUTPUT_PQ    2

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef char GLchar;
typedef unsigned char GLboolean;

#define GL_TEXTURE_2D          0x0DE1
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_WRAP_S      0x2802
#define GL_TEXTURE_WRAP_T      0x2803
#define GL_LINEAR              0x2601
#define GL_CLAMP_TO_EDGE       0x812F
#define GL_RGBA16F             0x881A
#define GL_RGBA                0x1908
#define GL_HALF_FLOAT          0x140B
#define GL_FRAMEBUFFER         0x8D40
#define GL_COLOR_ATTACHMENT0   0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RENDERBUFFER        0x8D41
#define GL_DEPTH24_STENCIL8    0x88F0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_VERTEX_SHADER       0x8B31
#define GL_FRAGMENT_SHADER     0x8B30
#define GL_COMPILE_STATUS      0x8B81
#define GL_LINK_STATUS         0x8B82
#define GL_TEXTURE0            0x84C0
#define GL_TRIANGLES           0x0004
#define GL_BLEND               0x0BE2
#define GL_FRAMEBUFFER_SRGB    0x8DB9

typedef struct PresentationFeedbackData PresentationFeedbackData;
typedef struct EglAttachment EglAttachment;

typedef struct {
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*GenRenderbuffers)(GLsizei, GLuint *);
    void (*DeleteRenderbuffers)(GLsizei, const GLuint *);
    void (*BindRenderbuffer)(GLenum, GLuint);
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*DeleteShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*DeleteProgram)(GLuint);
    void (*UseProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar *);
    void (*Uniform1i)(GLint, GLint);
    void (*ActiveTexture)(GLenum);
    void (*GenVertexArrays)(GLsizei, GLuint *);
    void (*DeleteVertexArrays)(GLsizei, const GLuint *);
    void (*BindVertexArray)(GLuint);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Disable)(GLenum);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
} PqGlFunctions;

/* ── Xlib types & constants (subset) ────────────────────────────────────── */

typedef unsigned long XID;
typedef XID           Window;
typedef XID           Colormap;
typedef XID           Pixmap;
typedef struct _XDisplay Display;
typedef void         *Visual;
typedef unsigned long VisualID;

typedef struct {
    int            x, y;
    int            width, height;
    int            border_width;
    int            depth;
    Visual        *visual;
    Window         root;
    int            class_;
    int            bit_gravity;
    int            win_gravity;
    int            backing_store;
    unsigned long  backing_planes;
    unsigned long  backing_pixel;
    int            save_under;
    Colormap       colormap;
    int            map_installed;
    int            map_state;
    long           all_event_masks;
    long           your_event_mask;
    long           do_not_propagate_mask;
    int            override_redirect;
    void          *screen;
} XWindowAttributes;

/* `XVisualInfo` matches Xutil.h. We only read .visual / .visualid / .depth /
 * .screen — the rest is here so the struct size matches Xlib's. */
typedef struct {
    Visual        *visual;
    VisualID       visualid;
    int            screen;
    int            depth;
    int            class_;
    unsigned long  red_mask;
    unsigned long  green_mask;
    unsigned long  blue_mask;
    int            colormap_size;
    int            bits_per_rgb;
} XVisualInfo;

typedef struct {
    Pixmap        background_pixmap;
    unsigned long background_pixel;
    Pixmap        border_pixmap;
    unsigned long border_pixel;
    int           bit_gravity;
    int           win_gravity;
    int           backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    int           save_under;
    long          event_mask;
    long          do_not_propagate_mask;
    int           override_redirect;
    Colormap      colormap;
    XID           cursor;
} XSetWindowAttributes;

typedef struct {
    short          x, y;
    unsigned short width, height;
} XRectangle;

#define None              0L
#define InputOutput       1
#define AllocNone         0
#define CWBorderPixel     (1L << 3)
#define CWEventMask       (1L << 11)
#define CWColormap        (1L << 13)

#define VisualIDMask      0x0001

/* XShape extension. Used to make the EGL-rendering child window
 * input-transparent so X routes pointer / keyboard events back to the GTK
 * parent — same trick the GLX helper uses. */
#define ShapeBounding 0
#define ShapeInput    2
#define ShapeSet      0
#define Unsorted      0

/* ── Function pointer types ─────────────────────────────────────────────── */

typedef EGLDisplay (*PFN_eglGetDisplay)(EGLNativeDisplayType);
typedef EGLDisplay (*PFN_eglGetPlatformDisplay)(EGLenum, void *, const intptr_t *);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay);
typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean (*PFN_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*PFN_eglDestroyContext)(EGLDisplay, EGLContext);
typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
typedef EGLBoolean (*PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglSwapInterval)(EGLDisplay, EGLint);
typedef EGLint     (*PFN_eglGetError)(void);
typedef void      *(*PFN_eglGetProcAddress)(const char *);
typedef const char *(*PFN_eglQueryString)(EGLDisplay, EGLint);
typedef EGLContext (*PFN_eglGetCurrentContext)(void);
typedef EGLDisplay (*PFN_eglGetCurrentDisplay)(void);

#define EGL_VENDOR  0x3053
#define EGL_VERSION 0x3054

/* ── Wayland client + EGL helpers ───────────────────────────────────────── */

/* Opaque types — we only ever pass them to libwayland-* and libwayland-egl. */
typedef struct wl_egl_window_  wl_egl_window;
typedef struct wl_display_     wl_display;
typedef struct wl_surface_     wl_surface;
typedef struct wl_proxy_       wl_proxy;
typedef struct wl_event_queue_ wl_event_queue;

typedef wl_egl_window *(*PFN_wl_egl_window_create)(wl_surface *, int, int);
typedef void           (*PFN_wl_egl_window_destroy)(wl_egl_window *);
typedef void           (*PFN_wl_egl_window_resize)(wl_egl_window *, int, int, int, int);

/* `wl_message` and `wl_interface` are the static introspection tables for
 * each Wayland interface. We don't define our own — we read pointers via
 * dlsym from libwayland-client.so.0 — but we need the layout to pass them
 * to `wl_proxy_marshal_flags`. */
struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};

/* Staging color-management-v1, version-1 wire declarations. They are kept
 * local for the same reason as the core Wayland declarations below: the
 * shipped helper has no build-time dependency on wayland-protocols or
 * wayland-scanner. Version 1 is intentionally bound even when a compositor
 * advertises v2, keeping the `wp_image_description_v1.ready(uint)` event ABI
 * stable while still exposing Windows-scRGB. */
static const struct wl_interface nucleus_color_manager_interface;
static const struct wl_interface nucleus_color_surface_interface;
static const struct wl_interface nucleus_image_description_interface;
static const struct wl_interface nucleus_parametric_creator_interface;
static const struct wl_interface nucleus_presentation_interface;
static const struct wl_interface nucleus_presentation_feedback_interface;

static const struct wl_interface *nucleus_manager_get_output_types[] = { NULL, NULL };
static const struct wl_interface *nucleus_manager_get_surface_types[] = {
    &nucleus_color_surface_interface, NULL
};
static const struct wl_interface *nucleus_manager_get_feedback_types[] = { NULL, NULL };
static const struct wl_interface *nucleus_manager_create_icc_types[] = { NULL };
static const struct wl_interface *nucleus_manager_create_params_types[] = {
    &nucleus_parametric_creator_interface
};
static const struct wl_interface *nucleus_manager_create_scrgb_types[] = {
    &nucleus_image_description_interface
};
static const struct wl_message nucleus_color_manager_requests[] = {
    { "destroy", "", NULL },
    { "get_output", "no", nucleus_manager_get_output_types },
    { "get_surface", "no", nucleus_manager_get_surface_types },
    { "get_surface_feedback", "no", nucleus_manager_get_feedback_types },
    { "create_icc_creator", "n", nucleus_manager_create_icc_types },
    { "create_parametric_creator", "n", nucleus_manager_create_params_types },
    { "create_windows_scrgb", "n", nucleus_manager_create_scrgb_types },
};
static const struct wl_message nucleus_color_manager_events[] = {
    { "supported_intent", "u", NULL },
    { "supported_feature", "u", NULL },
    { "supported_tf_named", "u", NULL },
    { "supported_primaries_named", "u", NULL },
    { "done", "", NULL },
};
static const struct wl_interface nucleus_color_manager_interface = {
    "wp_color_manager_v1", 1,
    7, nucleus_color_manager_requests,
    5, nucleus_color_manager_events,
};

static const struct wl_interface *nucleus_surface_set_description_types[] = {
    &nucleus_image_description_interface, NULL
};
static const struct wl_message nucleus_color_surface_requests[] = {
    { "destroy", "", NULL },
    { "set_image_description", "ou", nucleus_surface_set_description_types },
    { "unset_image_description", "", NULL },
};
static const struct wl_interface nucleus_color_surface_interface = {
    "wp_color_management_surface_v1", 1,
    3, nucleus_color_surface_requests,
    0, NULL,
};

static const struct wl_interface *nucleus_description_information_types[] = { NULL };
static const struct wl_message nucleus_image_description_requests[] = {
    { "destroy", "", NULL },
    { "get_information", "n", nucleus_description_information_types },
};
static const struct wl_message nucleus_image_description_events[] = {
    { "failed", "us", NULL },
    { "ready", "u", NULL },
};
static const struct wl_interface nucleus_image_description_interface = {
    "wp_image_description_v1", 1,
    2, nucleus_image_description_requests,
    2, nucleus_image_description_events,
};

static const struct wl_interface *nucleus_parametric_create_types[] = {
    &nucleus_image_description_interface
};
static const struct wl_message nucleus_parametric_creator_requests[] = {
    { "create", "n", nucleus_parametric_create_types },
    { "set_tf_named", "u", NULL },
    { "set_tf_power", "u", NULL },
    { "set_primaries_named", "u", NULL },
    { "set_primaries", "iiiiiiii", NULL },
    { "set_luminances", "uuu", NULL },
    { "set_mastering_display_primaries", "iiiiiiii", NULL },
    { "set_mastering_luminance", "uu", NULL },
    { "set_max_cll", "u", NULL },
    { "set_max_fall", "u", NULL },
};
static const struct wl_interface nucleus_parametric_creator_interface = {
    "wp_image_description_creator_params_v1", 1,
    10, nucleus_parametric_creator_requests,
    0, NULL,
};

static const struct wl_interface *nucleus_presentation_feedback_request_types[] = {
    NULL, &nucleus_presentation_feedback_interface
};
static const struct wl_message nucleus_presentation_requests[] = {
    { "destroy", "", NULL },
    { "feedback", "on", nucleus_presentation_feedback_request_types },
};
static const struct wl_message nucleus_presentation_events[] = {
    { "clock_id", "u", NULL },
};
static const struct wl_interface nucleus_presentation_interface = {
    "wp_presentation", 1,
    2, nucleus_presentation_requests,
    1, nucleus_presentation_events,
};

static const struct wl_interface *nucleus_feedback_sync_output_types[] = { NULL };
static const struct wl_message nucleus_presentation_feedback_events[] = {
    { "sync_output", "o", nucleus_feedback_sync_output_types },
    { "presented", "uuuuuuu", NULL },
    { "discarded", "", NULL },
};
static const struct wl_interface nucleus_presentation_feedback_interface = {
    "wp_presentation_feedback", 1,
    0, NULL,
    3, nucleus_presentation_feedback_events,
};

#define NUCLEUS_COLOR_MANAGER_GET_SURFACE       2
#define NUCLEUS_COLOR_MANAGER_CREATE_PARAMS     5
#define NUCLEUS_COLOR_MANAGER_CREATE_SCRGB      6
#define NUCLEUS_COLOR_SURFACE_SET_DESCRIPTION   1
#define NUCLEUS_COLOR_SURFACE_UNSET_DESCRIPTION 2
#define NUCLEUS_COLOR_FEATURE_WINDOWS_SCRGB     7
#define NUCLEUS_COLOR_FEATURE_PARAMETRIC         1
#define NUCLEUS_COLOR_INTENT_PERCEPTUAL          0
#define NUCLEUS_COLOR_TRANSFER_ST2084_PQ         11
#define NUCLEUS_COLOR_PRIMARIES_BT2020           6
#define NUCLEUS_PARAMETRIC_CREATE                0
#define NUCLEUS_PARAMETRIC_SET_TF_NAMED          1
#define NUCLEUS_PARAMETRIC_SET_PRIMARIES_NAMED   3
#define NUCLEUS_PRESENTATION_FEEDBACK             1

/* Subset of libwayland-client.so.0 we use for the wl_subsurface child path.
 * varargs `wl_proxy_marshal_flags` is the universal request-marshaller — we
 * call it with the same arg layout the inline statics in
 * <wayland-client-protocol.h> use. */
typedef wl_proxy *(*PFN_wl_proxy_marshal_flags)(
    wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface,
    uint32_t version, uint32_t flags, ...);
typedef int             (*PFN_wl_proxy_add_listener)(wl_proxy *, void (**)(void), void *);
typedef void            (*PFN_wl_proxy_destroy)(wl_proxy *);
typedef void            (*PFN_wl_proxy_set_queue)(wl_proxy *, wl_event_queue *);
typedef uint32_t        (*PFN_wl_proxy_get_version)(wl_proxy *);
typedef wl_event_queue *(*PFN_wl_display_create_queue)(wl_display *);
typedef int             (*PFN_wl_display_roundtrip_queue)(wl_display *, wl_event_queue *);
typedef int             (*PFN_wl_display_dispatch_queue)(wl_display *, wl_event_queue *);
typedef int             (*PFN_wl_display_dispatch_queue_pending)(wl_display *, wl_event_queue *);
typedef void            (*PFN_wl_event_queue_destroy)(wl_event_queue *);
typedef int             (*PFN_wl_display_flush)(wl_display *);

/* Wayland protocol opcodes (from wayland.xml; stable since the protocol
 * was frozen in 2014, won't change). Re-declared instead of imported so
 * the build doesn't pull in libwayland-dev headers. */
#define WL_MARSHAL_FLAG_DESTROY            1
#define WL_DISPLAY_GET_REGISTRY            1
#define WL_REGISTRY_BIND                   0
#define WL_COMPOSITOR_CREATE_SURFACE       0
#define WL_COMPOSITOR_CREATE_REGION        1
#define WL_REGION_DESTROY                  0
#define WL_REGION_ADD                      1
#define WL_SUBCOMPOSITOR_GET_SUBSURFACE    1
#define WL_SUBSURFACE_DESTROY              0
#define WL_SUBSURFACE_SET_POSITION         1
#define WL_SUBSURFACE_SET_DESYNC           5
#define WL_SURFACE_DESTROY                 0
#define WL_SURFACE_ATTACH                  1
#define WL_SURFACE_DAMAGE                  2
#define WL_SURFACE_FRAME                   3
#define WL_SURFACE_SET_OPAQUE_REGION       4
#define WL_SURFACE_SET_INPUT_REGION        5
#define WL_SURFACE_COMMIT                  6
#define WL_SURFACE_SET_BUFFER_SCALE        8

/* ── Xlib function pointer types ────────────────────────────────────────── */

typedef int          (*PFN_XGetWindowAttributes)(Display *, Window, XWindowAttributes *);
typedef VisualID     (*PFN_XVisualIDFromVisual)(Visual *);
typedef XVisualInfo *(*PFN_XGetVisualInfo)(Display *, long, XVisualInfo *, int *);
typedef int          (*PFN_XFree)(void *);
typedef Colormap     (*PFN_XCreateColormap)(Display *, Window, Visual *, int);
typedef int          (*PFN_XFreeColormap)(Display *, Colormap);
typedef Window       (*PFN_XCreateWindow)(Display *, Window, int, int, unsigned int, unsigned int,
                                          unsigned int, int, unsigned int, Visual *,
                                          unsigned long, XSetWindowAttributes *);
typedef int          (*PFN_XDestroyWindow)(Display *, Window);
typedef int          (*PFN_XMapWindow)(Display *, Window);
typedef int          (*PFN_XSync)(Display *, int);
typedef int          (*PFN_XFlush)(Display *);
typedef int          (*PFN_XResizeWindow)(Display *, Window, unsigned int, unsigned int);
typedef void         (*PFN_XShapeCombineRectangles)(Display *, Window, int, int, int,
                                                    XRectangle *, int, int, int);

/* ── Globals: dlopen handles + resolved symbols ─────────────────────────── */

static void *g_libegl = NULL;
static void *g_libgl  = NULL;     /* libGL.so.1 / libOpenGL.so.0 — for dlsym fallback */
static void *g_libx11 = NULL;

static PFN_eglGetDisplay         p_eglGetDisplay         = NULL;
static PFN_eglGetPlatformDisplay p_eglGetPlatformDisplay = NULL;
static PFN_eglInitialize         p_eglInitialize         = NULL;
static PFN_eglBindAPI            p_eglBindAPI            = NULL;
static PFN_eglChooseConfig       p_eglChooseConfig       = NULL;
static PFN_eglGetConfigAttrib    p_eglGetConfigAttrib    = NULL;
static PFN_eglCreateContext      p_eglCreateContext      = NULL;
static PFN_eglDestroyContext     p_eglDestroyContext     = NULL;
static PFN_eglCreateWindowSurface p_eglCreateWindowSurface = NULL;
static PFN_eglDestroySurface     p_eglDestroySurface     = NULL;
static PFN_eglMakeCurrent        p_eglMakeCurrent        = NULL;
static PFN_eglSwapBuffers        p_eglSwapBuffers        = NULL;
static PFN_eglSwapInterval       p_eglSwapInterval       = NULL;
static PFN_eglGetError           p_eglGetError           = NULL;
static PFN_eglGetProcAddress     p_eglGetProcAddress     = NULL;
static PFN_eglQueryString        p_eglQueryString        = NULL;
static PFN_eglGetCurrentContext  p_eglGetCurrentContext  = NULL;
static PFN_eglGetCurrentDisplay  p_eglGetCurrentDisplay  = NULL;

static PFN_XGetWindowAttributes  p_XGetWindowAttributes  = NULL;
static PFN_XVisualIDFromVisual   p_XVisualIDFromVisual   = NULL;
static PFN_XGetVisualInfo        p_XGetVisualInfo        = NULL;
static PFN_XFree                 p_XFree                 = NULL;
static PFN_XCreateColormap       p_XCreateColormap       = NULL;
static PFN_XFreeColormap         p_XFreeColormap         = NULL;
static PFN_XCreateWindow         p_XCreateWindow         = NULL;
static PFN_XDestroyWindow        p_XDestroyWindow        = NULL;
static PFN_XMapWindow            p_XMapWindow            = NULL;
static PFN_XSync                 p_XSync                 = NULL;
static PFN_XFlush                p_XFlush                = NULL;
static PFN_XResizeWindow         p_XResizeWindow         = NULL;
static PFN_XShapeCombineRectangles p_XShapeCombineRectangles = NULL;

static void *g_libxext = NULL;
static void *g_libwlegl = NULL;
static void *g_libwlclient = NULL;
static int g_libs_loaded = 0;

static PFN_wl_egl_window_create  p_wl_egl_window_create  = NULL;
static PFN_wl_egl_window_destroy p_wl_egl_window_destroy = NULL;
static PFN_wl_egl_window_resize  p_wl_egl_window_resize  = NULL;

/* libwayland-client function pointers + interface globals (the latter
 * are exported `const struct wl_interface` symbols in the .so). */
static PFN_wl_proxy_marshal_flags     p_wl_proxy_marshal_flags     = NULL;
static PFN_wl_proxy_add_listener      p_wl_proxy_add_listener      = NULL;
static PFN_wl_proxy_destroy           p_wl_proxy_destroy           = NULL;
static PFN_wl_proxy_set_queue         p_wl_proxy_set_queue         = NULL;
static PFN_wl_proxy_get_version       p_wl_proxy_get_version       = NULL;
static PFN_wl_display_create_queue          p_wl_display_create_queue          = NULL;
static PFN_wl_display_roundtrip_queue       p_wl_display_roundtrip_queue       = NULL;
static PFN_wl_display_dispatch_queue        p_wl_display_dispatch_queue        = NULL;
static PFN_wl_display_dispatch_queue_pending p_wl_display_dispatch_queue_pending = NULL;
static PFN_wl_event_queue_destroy           p_wl_event_queue_destroy           = NULL;
static PFN_wl_display_flush                 p_wl_display_flush                 = NULL;

static const struct wl_interface *g_wl_registry_interface     = NULL;
static const struct wl_interface *g_wl_compositor_interface   = NULL;
static const struct wl_interface *g_wl_subcompositor_interface= NULL;
static const struct wl_interface *g_wl_subsurface_interface   = NULL;
static const struct wl_interface *g_wl_surface_interface      = NULL;
static const struct wl_interface *g_wl_region_interface       = NULL;
static const struct wl_interface *g_wl_callback_interface     = NULL;
static const struct wl_interface *g_wl_output_interface       = NULL;

static int load_libs(void) {
    if (g_libs_loaded) return 1;

    if (!g_libegl) g_libegl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libegl) g_libegl = dlopen("libEGL.so",   RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libegl) {
        DBG("dlopen libEGL.so.1 failed: %s\n", dlerror());
        return 0;
    }

    /* libGL is *only* the dlsym fallback for proc-address resolution on
     * drivers that don't honor EGL_KHR_get_all_proc_addresses. Failing to
     * find it isn't fatal — modern Mesa & NVIDIA drivers return all entry
     * points through eglGetProcAddress directly. */
    if (!g_libgl) g_libgl = dlopen("libGL.so.1",     RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libgl) g_libgl = dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_GLOBAL);

    if (!g_libx11) g_libx11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libx11) g_libx11 = dlopen("libX11.so",   RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libx11) {
        DBG("dlopen libX11.so.6 failed: %s\n", dlerror());
        return 0;
    }

    /* libXext for XShape — required only for the child-window fallback path
     * that kicks in when GDK's X visual doesn't match any EGLConfig
     * (typical on XWayland; see attach loop below). Failing to load it
     * isn't fatal: we'll just refuse to fall back and surface a clearer
     * EGL_BAD_CONFIG error. */
    if (!g_libxext) g_libxext = dlopen("libXext.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libxext) g_libxext = dlopen("libXext.so",   RTLD_LAZY | RTLD_GLOBAL);

    /* libwayland-egl + libwayland-client — needed for the Wayland attach
     * path only. We don't fail load_libs() if missing; X11 sessions don't
     * need them and `nativeAttachWayland` will return 0 with a clear log. */
    if (!g_libwlegl) g_libwlegl = dlopen("libwayland-egl.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libwlegl) g_libwlegl = dlopen("libwayland-egl.so",   RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libwlclient) g_libwlclient = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_libwlclient) g_libwlclient = dlopen("libwayland-client.so",   RTLD_LAZY | RTLD_GLOBAL);

#define LOAD(lib, sym) p_##sym = (PFN_##sym) dlsym(lib, #sym)
    LOAD(g_libegl, eglGetDisplay);
    LOAD(g_libegl, eglGetPlatformDisplay);
    LOAD(g_libegl, eglInitialize);
    LOAD(g_libegl, eglBindAPI);
    LOAD(g_libegl, eglChooseConfig);
    LOAD(g_libegl, eglGetConfigAttrib);
    LOAD(g_libegl, eglCreateContext);
    LOAD(g_libegl, eglDestroyContext);
    LOAD(g_libegl, eglCreateWindowSurface);
    LOAD(g_libegl, eglDestroySurface);
    LOAD(g_libegl, eglMakeCurrent);
    LOAD(g_libegl, eglSwapBuffers);
    LOAD(g_libegl, eglSwapInterval);
    LOAD(g_libegl, eglGetError);
    LOAD(g_libegl, eglGetProcAddress);
    LOAD(g_libegl, eglQueryString);
    /* Used by nucleus_tao_texture_linux.c to resolve (and validate) the EGL
     * display/context the external-texture import must run on. */
    LOAD(g_libegl, eglGetCurrentContext);
    LOAD(g_libegl, eglGetCurrentDisplay);

    LOAD(g_libx11, XGetWindowAttributes);
    LOAD(g_libx11, XVisualIDFromVisual);
    LOAD(g_libx11, XGetVisualInfo);
    LOAD(g_libx11, XFree);
    LOAD(g_libx11, XCreateColormap);
    LOAD(g_libx11, XFreeColormap);
    LOAD(g_libx11, XCreateWindow);
    LOAD(g_libx11, XDestroyWindow);
    LOAD(g_libx11, XMapWindow);
    LOAD(g_libx11, XSync);
    LOAD(g_libx11, XFlush);
    LOAD(g_libx11, XResizeWindow);
    if (g_libxext) {
        p_XShapeCombineRectangles =
            (PFN_XShapeCombineRectangles) dlsym(g_libxext, "XShapeCombineRectangles");
    }
    if (g_libwlegl) {
        p_wl_egl_window_create  =
            (PFN_wl_egl_window_create)  dlsym(g_libwlegl, "wl_egl_window_create");
        p_wl_egl_window_destroy =
            (PFN_wl_egl_window_destroy) dlsym(g_libwlegl, "wl_egl_window_destroy");
        p_wl_egl_window_resize  =
            (PFN_wl_egl_window_resize)  dlsym(g_libwlegl, "wl_egl_window_resize");
    }
    if (g_libwlclient) {
        p_wl_proxy_marshal_flags =
            (PFN_wl_proxy_marshal_flags) dlsym(g_libwlclient, "wl_proxy_marshal_flags");
        p_wl_proxy_add_listener =
            (PFN_wl_proxy_add_listener)  dlsym(g_libwlclient, "wl_proxy_add_listener");
        p_wl_proxy_destroy =
            (PFN_wl_proxy_destroy)       dlsym(g_libwlclient, "wl_proxy_destroy");
        p_wl_proxy_set_queue =
            (PFN_wl_proxy_set_queue)     dlsym(g_libwlclient, "wl_proxy_set_queue");
        p_wl_proxy_get_version =
            (PFN_wl_proxy_get_version)   dlsym(g_libwlclient, "wl_proxy_get_version");
        p_wl_display_create_queue =
            (PFN_wl_display_create_queue)          dlsym(g_libwlclient, "wl_display_create_queue");
        p_wl_display_roundtrip_queue =
            (PFN_wl_display_roundtrip_queue)       dlsym(g_libwlclient, "wl_display_roundtrip_queue");
        p_wl_display_dispatch_queue =
            (PFN_wl_display_dispatch_queue)        dlsym(g_libwlclient, "wl_display_dispatch_queue");
        p_wl_display_dispatch_queue_pending =
            (PFN_wl_display_dispatch_queue_pending) dlsym(g_libwlclient, "wl_display_dispatch_queue_pending");
        p_wl_event_queue_destroy =
            (PFN_wl_event_queue_destroy)           dlsym(g_libwlclient, "wl_event_queue_destroy");
        p_wl_display_flush =
            (PFN_wl_display_flush)                 dlsym(g_libwlclient, "wl_display_flush");
        g_wl_registry_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_registry_interface");
        g_wl_compositor_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_compositor_interface");
        g_wl_subcompositor_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_subcompositor_interface");
        g_wl_subsurface_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_subsurface_interface");
        g_wl_surface_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_surface_interface");
        g_wl_region_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_region_interface");
        g_wl_callback_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_callback_interface");
        g_wl_output_interface =
            (const struct wl_interface *) dlsym(g_libwlclient, "wl_output_interface");
        nucleus_manager_get_surface_types[1] = g_wl_surface_interface;
        nucleus_manager_get_feedback_types[1] = g_wl_surface_interface;
        nucleus_presentation_feedback_request_types[0] = g_wl_surface_interface;
        nucleus_feedback_sync_output_types[0] = g_wl_output_interface;
    }
#undef LOAD

    g_libs_loaded =
        p_eglInitialize && p_eglBindAPI && p_eglChooseConfig &&
        p_eglCreateContext && p_eglCreateWindowSurface && p_eglMakeCurrent &&
        p_eglSwapBuffers && p_eglGetProcAddress &&
        p_XGetWindowAttributes && p_XVisualIDFromVisual;

    return g_libs_loaded;
}

/* ── Diagnostics: log EGL vendor/version once on first attach ───────────── */

/**
 * Logs the EGL vendor/version + a NVIDIA-specific runtime hint to stderr the
 * first time we initialize a display. Always emitted (regardless of
 * NUCLEUS_TAO_EGL_DEBUG) because the NVIDIA hint is actionable: drag-resize on
 * Wayland is broken on driver < 560 unless the system has the new
 * `egl-wayland2` external-platform library installed alongside the legacy
 * `egl-wayland`. Users hit this and file "resize stutters / locks up" without
 * knowing what to install.
 *
 * Heuristic: on Wayland sessions (`is_wayland`), check the EGL vendor string —
 * NVIDIA's contains "NVIDIA". The "is egl-wayland2 installed?" check would
 * require parsing /proc/self/maps for `libnvidia-egl-wayland2.so` which is
 * fragile; we just print the hint unconditionally on NVIDIA Wayland and let
 * users decide if they need it.
 */
static int g_diag_logged = 0;
static void log_egl_diagnostics_once(EGLDisplay edpy, int is_wayland) {
    if (g_diag_logged) return;
    g_diag_logged = 1;
    if (!p_eglQueryString) return;
    const char *vendor  = p_eglQueryString(edpy, EGL_VENDOR);
    const char *version = p_eglQueryString(edpy, EGL_VERSION);
    (void) version;
    DBG("EGL %s, vendor: %s, platform: %s\n",
        version ? version : "?",
        vendor  ? vendor  : "?",
        is_wayland ? "Wayland" : "X11");
    if (is_wayland && vendor && strstr(vendor, "NVIDIA")) {
        DBG("NOTE: NVIDIA Wayland — if drag-resize "
            "stutters or hangs, ensure your distro ships the\n"
            "[nucleus_tao_egl]       `egl-wayland2` package alongside "
            "`egl-wayland` (driver 560+ uses the new dma-buf backend that\n"
            "[nucleus_tao_egl]       supports proper EGLSurface resize; "
            "the legacy egl-wayland cannot resize EGLSurfaces).\n");
    }
}

static int extension_list_contains(const char *extensions, const char *name) {
    if (!extensions || !name || !*name) return 0;
    size_t length = strlen(name);
    const char *cursor = extensions;
    while ((cursor = strstr(cursor, name)) != NULL) {
        int starts = cursor == extensions || cursor[-1] == ' ';
        int ends = cursor[length] == '\0' || cursor[length] == ' ';
        if (starts && ends) return 1;
        cursor += length;
    }
    return 0;
}

/* ── Skia proc-address loader ───────────────────────────────────────────── */

/**
 * Signature matches Skia's `GrGLGetProc`:  void* fn(void* ctx, const char* name).
 * Tries `eglGetProcAddress` first — on every driver advertising
 * `EGL_KHR_get_all_proc_addresses` (Mesa 11+, NVIDIA 470+) this resolves all
 * core 1.0/1.1 entry points as well as extensions. Falls back to `dlsym` on
 * libGL/libOpenGL for the long tail of strict drivers, otherwise Skia
 * silently disables features whose entry points came back NULL.
 *
 * Address handed back to the JVM via `nativeGetProcAddrFunctionPointer`,
 * then forwarded to `GLAssembledInterface.createFromNativePointers(0, fnPtr)`.
 */
static void *nucleus_tao_egl_get_proc(void *ctx, const char *name) {
    (void) ctx;
    void *p = NULL;
    if (p_eglGetProcAddress) p = p_eglGetProcAddress(name);
    if (!p && g_libgl)        p = dlsym(g_libgl, name);
    return p;
}

/* ── Per-window state ───────────────────────────────────────────────────── */

struct EglAttachment {
    EGLDisplay display;
    EGLConfig  config;
    EGLContext context;
    EGLSurface surface;
    /* X11 plumbing. `parent_xid` is always the GTK-owned XID; `child_xid` is
     * non-zero only when GDK's visual didn't match any EGLConfig and we had
     * to create a child window with a Mesa-canonical visual on top. Both
     * stay 0 on the Wayland path. */
    Display   *xdisplay;
    Window     parent_xid;
    Window     child_xid;
    Colormap   child_colormap;
    /* Wayland plumbing. We never render directly to GTK's wl_surface —
     * instead we own a `wl_subsurface` child of it, which decouples our
     * buffer commits from GTK's xdg_shell handshake and its cairo paint
     * cycle (see file header for the full diagnosis). All these proxies
     * live on `wl_queue` so events on them don't race with GDK's default
     * queue. nativeDetach destroys them in this order: wl_window →
     * wl_subsurface → wl_child_surface → wl_compositor / wl_subcompositor →
     * wl_registry → wl_queue. */
    wl_display     *wl_display_conn;   /* GTK's wl_display — not owned. */
    wl_event_queue *wl_queue;
    wl_proxy       *wl_registry;
    wl_proxy       *wl_compositor;
    wl_proxy       *wl_subcompositor;
    wl_proxy       *wl_parent_surface; /* GTK's wl_surface — not owned, not destroyed. */
    wl_proxy       *wl_child_surface;
    wl_proxy       *wl_subsurface;
    wl_egl_window  *wl_window;
    wl_proxy       *wl_color_manager;
    wl_proxy       *wl_color_surface;
    wl_proxy       *wl_scrgb_description;
    wl_proxy       *wl_presentation;
    wl_proxy       *wl_outputs[16];
    uint32_t        wl_output_count;
    wl_proxy       *wl_presented_output;
    PresentationFeedbackData *wl_feedbacks[64];
    uint32_t        wl_feedback_count;
    int             output_mode;
    int             extended_scene;
    PqGlFunctions   pq_gl;
    GLuint          pq_scene_texture;
    GLuint          pq_scene_framebuffer;
    GLuint          pq_depth_stencil;
    GLuint          pq_present_program;
    GLuint          pq_present_vao;
    GLint           pq_scene_sampler;
    _Atomic uint64_t output_generation;
    _Atomic uint64_t presented_frames;
    /* Content-area origin inside the parent surface, logical px. (0,0) for a
     * plain undecorated toplevel; the GTK theme's shadow margins when the
     * hidden-titlebar CSD is active (GTK then draws its native drop shadow in
     * the ring around the content subsurface). Applied via
     * wl_subsurface.set_position — parent-surface state, takes effect on
     * GTK's next commit. */
    int             content_off_x;
    int             content_off_y;
    int             widthPx;
    int             heightPx;
    float      scale;
};

struct PresentationFeedbackData {
    EglAttachment *attachment;
    wl_proxy *proxy;
};

static void release_presentation_feedback(PresentationFeedbackData *feedback) {
    if (!feedback) return;
    EglAttachment *att = feedback->attachment;
    if (att) {
        for (uint32_t index = 0; index < att->wl_feedback_count; index++) {
            if (att->wl_feedbacks[index] == feedback) {
                const uint32_t last = --att->wl_feedback_count;
                att->wl_feedbacks[index] = att->wl_feedbacks[last];
                att->wl_feedbacks[last] = NULL;
                break;
            }
        }
    }
    if (feedback->proxy && p_wl_proxy_destroy) {
        p_wl_proxy_destroy(feedback->proxy);
    }
    free(feedback);
}

static int load_pq_gl_functions(PqGlFunctions *gl) {
    if (!gl) return 0;
#define LOAD_PQ_GL(member, symbol) do {                                      \
        void *address = nucleus_tao_egl_get_proc(NULL, symbol);               \
        _Static_assert(sizeof(gl->member) == sizeof(address),                 \
            "OpenGL function and data pointers must have matching sizes");  \
        memcpy(&gl->member, &address, sizeof(address));                       \
        if (!gl->member) return 0;                                            \
    } while (0)
    LOAD_PQ_GL(GenTextures, "glGenTextures");
    LOAD_PQ_GL(DeleteTextures, "glDeleteTextures");
    LOAD_PQ_GL(BindTexture, "glBindTexture");
    LOAD_PQ_GL(TexParameteri, "glTexParameteri");
    LOAD_PQ_GL(TexImage2D, "glTexImage2D");
    LOAD_PQ_GL(GenFramebuffers, "glGenFramebuffers");
    LOAD_PQ_GL(DeleteFramebuffers, "glDeleteFramebuffers");
    LOAD_PQ_GL(BindFramebuffer, "glBindFramebuffer");
    LOAD_PQ_GL(FramebufferTexture2D, "glFramebufferTexture2D");
    LOAD_PQ_GL(CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD_PQ_GL(GenRenderbuffers, "glGenRenderbuffers");
    LOAD_PQ_GL(DeleteRenderbuffers, "glDeleteRenderbuffers");
    LOAD_PQ_GL(BindRenderbuffer, "glBindRenderbuffer");
    LOAD_PQ_GL(RenderbufferStorage, "glRenderbufferStorage");
    LOAD_PQ_GL(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    LOAD_PQ_GL(CreateShader, "glCreateShader");
    LOAD_PQ_GL(ShaderSource, "glShaderSource");
    LOAD_PQ_GL(CompileShader, "glCompileShader");
    LOAD_PQ_GL(GetShaderiv, "glGetShaderiv");
    LOAD_PQ_GL(DeleteShader, "glDeleteShader");
    LOAD_PQ_GL(CreateProgram, "glCreateProgram");
    LOAD_PQ_GL(AttachShader, "glAttachShader");
    LOAD_PQ_GL(LinkProgram, "glLinkProgram");
    LOAD_PQ_GL(GetProgramiv, "glGetProgramiv");
    LOAD_PQ_GL(DeleteProgram, "glDeleteProgram");
    LOAD_PQ_GL(UseProgram, "glUseProgram");
    LOAD_PQ_GL(GetUniformLocation, "glGetUniformLocation");
    LOAD_PQ_GL(Uniform1i, "glUniform1i");
    LOAD_PQ_GL(ActiveTexture, "glActiveTexture");
    LOAD_PQ_GL(GenVertexArrays, "glGenVertexArrays");
    LOAD_PQ_GL(DeleteVertexArrays, "glDeleteVertexArrays");
    LOAD_PQ_GL(BindVertexArray, "glBindVertexArray");
    LOAD_PQ_GL(Viewport, "glViewport");
    LOAD_PQ_GL(Disable, "glDisable");
    LOAD_PQ_GL(DrawArrays, "glDrawArrays");
#undef LOAD_PQ_GL
    return 1;
}

static GLuint compile_pq_shader(
    PqGlFunctions *gl, GLenum type, const char *source)
{
    GLuint shader = gl->CreateShader(type);
    if (!shader) return 0;
    gl->ShaderSource(shader, 1, &source, NULL);
    gl->CompileShader(shader);
    GLint compiled = 0;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

static void destroy_pq_resources(EglAttachment *att) {
    if (!att) return;
    PqGlFunctions *gl = &att->pq_gl;
    if (att->pq_present_vao && gl->DeleteVertexArrays) {
        gl->DeleteVertexArrays(1, &att->pq_present_vao);
    }
    if (att->pq_present_program && gl->DeleteProgram) {
        gl->DeleteProgram(att->pq_present_program);
    }
    if (att->pq_depth_stencil && gl->DeleteRenderbuffers) {
        gl->DeleteRenderbuffers(1, &att->pq_depth_stencil);
    }
    if (att->pq_scene_framebuffer && gl->DeleteFramebuffers) {
        gl->DeleteFramebuffers(1, &att->pq_scene_framebuffer);
    }
    if (att->pq_scene_texture && gl->DeleteTextures) {
        gl->DeleteTextures(1, &att->pq_scene_texture);
    }
    att->pq_present_vao = 0;
    att->pq_present_program = 0;
    att->pq_depth_stencil = 0;
    att->pq_scene_framebuffer = 0;
    att->pq_scene_texture = 0;
    att->pq_scene_sampler = -1;
}

static int resize_pq_scene_target(EglAttachment *att, int width, int height) {
    if (!att || att->output_mode != NUCLEUS_OUTPUT_PQ ||
        !att->pq_scene_texture || !att->pq_scene_framebuffer ||
        !att->pq_depth_stencil) return 0;
    PqGlFunctions *gl = &att->pq_gl;
    width = width > 0 ? width : 1;
    height = height > 0 ? height : 1;
    gl->BindTexture(GL_TEXTURE_2D, att->pq_scene_texture);
    gl->TexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
        GL_RGBA, GL_HALF_FLOAT, NULL);
    gl->BindRenderbuffer(GL_RENDERBUFFER, att->pq_depth_stencil);
    gl->RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    gl->BindFramebuffer(GL_FRAMEBUFFER, att->pq_scene_framebuffer);
    gl->FramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        att->pq_scene_texture, 0);
    gl->FramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
        att->pq_depth_stencil);
    return gl->CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static int initialize_pq_resources(EglAttachment *att) {
    static const char vertex_source[] =
        "#version 330 core\n"
        "out vec2 textureCoordinate;\n"
        "void main() {\n"
        "  vec2 position = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "  textureCoordinate = position;\n"
        "  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char fragment_source[] =
        "#version 330 core\n"
        "in vec2 textureCoordinate;\n"
        "uniform sampler2D sceneTexture;\n"
        "out vec4 outputColor;\n"
        "vec3 encodePq(vec3 value) {\n"
        "  const float m1 = 0.1593017578125;\n"
        "  const float m2 = 78.84375;\n"
        "  const float c1 = 0.8359375;\n"
        "  const float c2 = 18.8515625;\n"
        "  const float c3 = 18.6875;\n"
        "  vec3 powered = pow(clamp(value, 0.0, 1.0), vec3(m1));\n"
        "  return pow((vec3(c1) + c2 * powered) / (vec3(1.0) + c3 * powered), vec3(m2));\n"
        "}\n"
        "void main() {\n"
        "  vec4 scene = texture(sceneTexture, textureCoordinate);\n"
        "  float alpha = clamp(scene.a, 0.0, 1.0);\n"
        "  vec3 straight = alpha > 0.000001 ? scene.rgb / alpha : vec3(0.0);\n"
        "  mat3 srgbToBt2020 = mat3(\n"
        "    0.627404, 0.069097, 0.016391,\n"
        "    0.329283, 0.919540, 0.088013,\n"
        "    0.043313, 0.011362, 0.895595);\n"
        "  vec3 normalizedNits = max(srgbToBt2020 * straight, vec3(0.0)) * (203.0 / 10000.0);\n"
        "  outputColor = vec4(encodePq(normalizedNits) * alpha, alpha);\n"
        "}\n";
    if (!att || !load_pq_gl_functions(&att->pq_gl)) return 0;
    PqGlFunctions *gl = &att->pq_gl;
    GLuint vertex = compile_pq_shader(gl, GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_pq_shader(gl, GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) {
        if (vertex) gl->DeleteShader(vertex);
        if (fragment) gl->DeleteShader(fragment);
        return 0;
    }
    att->pq_present_program = gl->CreateProgram();
    if (att->pq_present_program) {
        gl->AttachShader(att->pq_present_program, vertex);
        gl->AttachShader(att->pq_present_program, fragment);
        gl->LinkProgram(att->pq_present_program);
    }
    gl->DeleteShader(vertex);
    gl->DeleteShader(fragment);
    GLint linked = 0;
    if (att->pq_present_program) {
        gl->GetProgramiv(att->pq_present_program, GL_LINK_STATUS, &linked);
    }
    if (!linked) {
        destroy_pq_resources(att);
        return 0;
    }
    att->pq_scene_sampler =
        gl->GetUniformLocation(att->pq_present_program, "sceneTexture");
    gl->GenTextures(1, &att->pq_scene_texture);
    gl->BindTexture(GL_TEXTURE_2D, att->pq_scene_texture);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->GenFramebuffers(1, &att->pq_scene_framebuffer);
    gl->GenRenderbuffers(1, &att->pq_depth_stencil);
    gl->GenVertexArrays(1, &att->pq_present_vao);
    if (!att->pq_scene_texture || !att->pq_scene_framebuffer ||
        !att->pq_depth_stencil || !att->pq_present_vao ||
        !resize_pq_scene_target(att, att->widthPx, att->heightPx)) {
        destroy_pq_resources(att);
        return 0;
    }
    return 1;
}

static void present_pq_scene(EglAttachment *att) {
    if (!att || att->output_mode != NUCLEUS_OUTPUT_PQ ||
        !att->pq_present_program || !att->pq_scene_texture) return;
    PqGlFunctions *gl = &att->pq_gl;
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->Viewport(0, 0, att->widthPx, att->heightPx);
    gl->Disable(GL_BLEND);
    gl->Disable(GL_FRAMEBUFFER_SRGB);
    gl->UseProgram(att->pq_present_program);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, att->pq_scene_texture);
    if (att->pq_scene_sampler >= 0) gl->Uniform1i(att->pq_scene_sampler, 0);
    gl->BindVertexArray(att->pq_present_vao);
    gl->DrawArrays(GL_TRIANGLES, 0, 3);
    gl->BindVertexArray(0);
    gl->BindTexture(GL_TEXTURE_2D, 0);
    gl->UseProgram(0);
    gl->BindFramebuffer(GL_FRAMEBUFFER, att->pq_scene_framebuffer);
}

/* ── Internal surface shared inside libnucleus_tao_egl.so ───────────────── */
/* Implemented here because this TU owns the dlopen'd EGL entry points and the
 * per-window attachment state; see nucleus_tao_egl_internal.h. */

int nucleus_tao_egl_ensure_libs(void) {
    return load_libs();
}

void *nucleus_tao_egl_proc_address(const char *name) {
    return nucleus_tao_egl_get_proc(NULL, name);
}

void *nucleus_tao_egl_current_display(void) {
    return p_eglGetCurrentDisplay ? p_eglGetCurrentDisplay() : NULL;
}

void *nucleus_tao_egl_current_context(void) {
    return p_eglGetCurrentContext ? p_eglGetCurrentContext() : NULL;
}

void *nucleus_tao_egl_attachment_context(long long handle) {
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    return att ? att->context : NULL;
}

/* ── JNI surface ────────────────────────────────────────────────────────── */

/**
 * Creates an EGL display + context on the X11 connection [xdisplayPtr] and a
 * window surface bound to [xid]. The chosen `EGLConfig` is filtered to match
 * the X visual already assigned to the GTK window (typically ARGB32 when
 * `with_transparent(true)` was passed to the WindowBuilder), so
 * `eglCreateWindowSurface` doesn't return `EGL_BAD_MATCH` on Mesa.
 *
 * Caller must invoke this on the thread that will own the EGL context.
 * Returns an opaque attachment handle, or 0 on failure.
 */
JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeAttachX11(
    JNIEnv *env, jclass clazz,
    jlong xdisplayPtr, jlong xidLong,
    jint widthPx, jint heightPx)
{
    (void) env; (void) clazz;
    if (!xdisplayPtr || !xidLong) return 0;
    if (!load_libs()) return 0;

    Display *xdpy = (Display *) (uintptr_t) xdisplayPtr;
    Window   xwin = (Window)    (uintptr_t) xidLong;
    DBG("attachX11: xdpy=%p xid=0x%lx wxh=%dx%d\n", (void*)xdpy, xwin, widthPx, heightPx);

    /* 1) EGL display from the X11 connection. Prefer the EGL 1.5 platform
     *    function — it makes the platform explicit and works uniformly on
     *    Mesa & NVIDIA. Fall back to the legacy `eglGetDisplay(Display*)`
     *    which is sloppier but still accepts an X11 Display* as a native
     *    display type on every shipping driver. */
    EGLDisplay edpy = EGL_NO_DISPLAY;
    if (p_eglGetPlatformDisplay) {
        edpy = p_eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, xdpy, NULL);
    }
    if (edpy == EGL_NO_DISPLAY && p_eglGetDisplay) {
        edpy = p_eglGetDisplay((EGLNativeDisplayType) xdpy);
    }
    if (edpy == EGL_NO_DISPLAY) {
        DBG("eglGetDisplay returned EGL_NO_DISPLAY\n");
        return 0;
    }

    EGLint maj = 0, min = 0;
    if (!p_eglInitialize(edpy, &maj, &min)) {
        DBG("eglInitialize failed: 0x%x\n", p_eglGetError ? p_eglGetError() : 0);
        return 0;
    }
    DBG("EGL %d.%d initialized\n", maj, min);
    log_egl_diagnostics_once(edpy, /*is_wayland=*/0);

    /* 2) Desktop GL — must be set before eglCreateContext. Skia chooses
     *    GL vs GLES from `glGetString(GL_VERSION)` at make-current time;
     *    we want desktop because `GrGLMakeAssembledInterface` resolves
     *    desktop entry points. */
    if (!p_eglBindAPI(EGL_OPENGL_API)) {
        DBG("eglBindAPI(EGL_OPENGL_API) failed (driver lacks desktop GL?)\n");
        return 0;
    }

    /* 3) Pick a config matching the GTK window's X visual. On a compositing
     *    desktop with `with_transparent(true)`, GDK assigns an ARGB32 visual
     *    and we'll match against the ARGB EGL config; without compositing
     *    the visual is RGB888 and the same lookup picks an alpha=0 config. */
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,    /* Skia provides its own depth/stencil   */
        EGL_STENCIL_SIZE,    0,    /* attachment to the SkSurface's FBO.    */
        EGL_SAMPLES,         0,    /* MSAA off; Skia handles AA itself.     */
        EGL_NONE
    };
    EGLConfig cfgs[64];
    EGLint    ncfg = 0;
    if (!p_eglChooseConfig(edpy, cfg_attrs, cfgs, 64, &ncfg) || ncfg <= 0) {
        DBG("eglChooseConfig returned no configs\n");
        return 0;
    }

    XWindowAttributes wa;
    memset(&wa, 0, sizeof(wa));
    if (!p_XGetWindowAttributes(xdpy, xwin, &wa) || !wa.visual) {
        DBG("XGetWindowAttributes failed\n");
        return 0;
    }
    VisualID want = p_XVisualIDFromVisual(wa.visual);
    DBG("window visualid=0x%lx depth=%d wxh=%dx%d\n",
        want, wa.depth, wa.width, wa.height);
    DBG("eglChooseConfig returned %d configs\n", ncfg);

    EGLConfig chosen = NULL;
    for (EGLint i = 0; i < ncfg; ++i) {
        EGLint id = 0;
        p_eglGetConfigAttrib(edpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
        DBG("  cfg[%d] visualid=0x%lx\n", i, (unsigned long)(unsigned)id);
        if ((VisualID) id == want) { chosen = cfgs[i]; break; }
    }

    /* Phase-1 widening: GTK 3 with `decorations=false` and no `transparent=true`
     * gets a 24-bit RGB888 visual on most setups, which our default ALPHA_SIZE=8
     * config request can't match. Re-query without the alpha constraint so we
     * land on a 24-bit config whose native visual matches the window. The
     * eventual rounded-corner work will pass `with_transparent(true)` to tao
     * and round-trip through ARGB; until then this fallback keeps Mesa happy. */
    if (!chosen) {
        DBG("no ARGB EGL config matches X visualid 0x%lx — retrying without alpha\n", want);
        const EGLint cfg_attrs_no_alpha[] = {
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_DEPTH_SIZE,      0,
            EGL_STENCIL_SIZE,    0,
            EGL_SAMPLES,         0,
            EGL_NONE
        };
        EGLConfig cfgs2[64];
        EGLint ncfg2 = 0;
        if (p_eglChooseConfig(edpy, cfg_attrs_no_alpha, cfgs2, 64, &ncfg2) && ncfg2 > 0) {
            DBG("  retry returned %d configs\n", ncfg2);
            for (EGLint i = 0; i < ncfg2; ++i) {
                EGLint id = 0;
                p_eglGetConfigAttrib(edpy, cfgs2[i], EGL_NATIVE_VISUAL_ID, &id);
                DBG("    cfg2[%d] visualid=0x%lx\n", i, (unsigned long)(unsigned)id);
                if ((VisualID) id == want) { chosen = cfgs2[i]; break; }
            }
            /* If no exact-visual match, prefer an ARGB config (alpha > 0)
             * so the eventual alpha-blended rounded-corner work has alpha
             * available on the EGL surface. Mesa typically lists RGB-only
             * configs first, so cfgs2[0] is a 24-bit visual on most setups —
             * walk the list and grab the first one with EGL_ALPHA_SIZE > 0. */
            if (!chosen) {
                for (EGLint i = 0; i < ncfg2; ++i) {
                    EGLint a = 0;
                    p_eglGetConfigAttrib(edpy, cfgs2[i], EGL_ALPHA_SIZE, &a);
                    if (a > 0) { chosen = cfgs2[i]; break; }
                }
            }
            /* Last resort: take cfgs2[0]. NVIDIA accepts the cross-match
             * silently; Mesa surfaces EGL_BAD_MATCH from
             * eglCreateWindowSurface so the error is at least visible. */
            if (!chosen && ncfg2 > 0) chosen = cfgs2[0];
        }
    }
    if (!chosen) chosen = cfgs[0];
    DBG("chosen EGLConfig=%p\n", (void*)chosen);

    /* Re-read the chosen config's native visual ID — used below to decide
     * whether we can render straight into the GTK window or need a
     * child-window with a matching visual. */
    EGLint chosen_vid = 0;
    p_eglGetConfigAttrib(edpy, chosen, EGL_NATIVE_VISUAL_ID, &chosen_vid);
    int needs_child = ((VisualID) chosen_vid != want);
    DBG("chosen visualid=0x%lx, needs_child=%d\n", (unsigned long)(unsigned)chosen_vid, needs_child);

    /* 4) Compat profile 3.3 — minimum for Skia's modern GL renderer; drivers
     *    will hand us a higher version if available. */
    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
            EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_NONE
    };
    EGLContext ctx = p_eglCreateContext(edpy, chosen, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        DBG("eglCreateContext failed: 0x%x\n", p_eglGetError ? p_eglGetError() : 0);
        return 0;
    }

    /* When the GTK window's X visual doesn't match any of Mesa's EGLConfigs
     * (typical on XWayland: GDK's `screen.rgba_visual()` returns a different
     * 32-bit ARGB visual than the one Mesa registered with EGL), Mesa rejects
     * `eglCreateWindowSurface` with EGL_BAD_CONFIG. Mirror the GLX helper's
     * fallback: create a child X window with Mesa's expected visual on top
     * of the GTK parent, make it input-transparent via XShape so events
     * still flow to GTK, and bind EGL to that child instead.  */
    Window  egl_xid       = xwin;
    Window  child_xid     = (Window) None;
    Colormap child_cmap   = (Colormap) None;
    if (needs_child) {
        if (!p_XGetVisualInfo || !p_XCreateColormap || !p_XCreateWindow ||
            !p_XMapWindow      || !p_XSync) {
            DBG("Xlib symbols missing for child-window fallback\n");
            p_eglDestroyContext(edpy, ctx);
            return 0;
        }
        XVisualInfo template;
        memset(&template, 0, sizeof(template));
        template.visualid = (VisualID) chosen_vid;
        int n_vinfo = 0;
        XVisualInfo *vinfos = p_XGetVisualInfo(xdpy, VisualIDMask, &template, &n_vinfo);
        if (!vinfos || n_vinfo <= 0) {
            DBG("XGetVisualInfo for visualid 0x%lx returned no match\n",
                (unsigned long)(unsigned)chosen_vid);
            if (vinfos && p_XFree) p_XFree(vinfos);
            p_eglDestroyContext(edpy, ctx);
            return 0;
        }
        XVisualInfo vinfo = vinfos[0];
        if (p_XFree) p_XFree(vinfos);
        DBG("child visual: id=0x%lx depth=%d screen=%d\n",
            vinfo.visualid, vinfo.depth, vinfo.screen);

        child_cmap = p_XCreateColormap(xdpy, wa.root, vinfo.visual, AllocNone);

        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.colormap         = child_cmap;
        swa.event_mask       = 0;     /* don't subscribe — events go to parent */
        swa.background_pixel = 0;
        swa.border_pixel     = 0;
        unsigned int cw = (widthPx  > 0) ? (unsigned int) widthPx  : (unsigned int) wa.width;
        unsigned int ch = (heightPx > 0) ? (unsigned int) heightPx : (unsigned int) wa.height;
        if (cw == 0) cw = 1;
        if (ch == 0) ch = 1;
        child_xid = p_XCreateWindow(
            xdpy, xwin, 0, 0, cw, ch, 0,
            vinfo.depth, InputOutput, vinfo.visual,
            CWBorderPixel | CWColormap | CWEventMask, &swa);
        if (!child_xid) {
            DBG("XCreateWindow for child failed\n");
            if (p_XFreeColormap) p_XFreeColormap(xdpy, child_cmap);
            p_eglDestroyContext(edpy, ctx);
            return 0;
        }

        /* Make the child input-transparent so X11 routes pointer / keyboard
         * events back to the GTK parent (and therefore to tao's event loop).
         * Without this, every click hits the EGL surface and tao goes deaf. */
        if (p_XShapeCombineRectangles) {
            p_XShapeCombineRectangles(xdpy, child_xid, ShapeInput, 0, 0,
                                       NULL, 0, ShapeSet, Unsorted);
        } else {
            DBG("WARN: XShapeCombineRectangles not available — child window "
                "will eat input. Install libXext for proper input routing.\n");
        }
        p_XMapWindow(xdpy, child_xid);
        if (p_XSync) p_XSync(xdpy, 0);

        DBG("child_xid=0x%lx mapped over parent=0x%lx (%ux%u)\n",
            child_xid, xwin, cw, ch);
        egl_xid = child_xid;
    }

    EGLSurface surf = p_eglCreateWindowSurface(edpy, chosen,
                                               (EGLNativeWindowType) egl_xid, NULL);
    if (surf == EGL_NO_SURFACE) {
        DBG("eglCreateWindowSurface failed: 0x%x\n", p_eglGetError ? p_eglGetError() : 0);
        if (child_xid && p_XDestroyWindow)  p_XDestroyWindow(xdpy, child_xid);
        if (child_cmap && p_XFreeColormap)  p_XFreeColormap(xdpy, child_cmap);
        p_eglDestroyContext(edpy, ctx);
        return 0;
    }

    if (!p_eglMakeCurrent(edpy, surf, surf, ctx)) {
        DBG("eglMakeCurrent failed: 0x%x\n", p_eglGetError ? p_eglGetError() : 0);
        p_eglDestroySurface(edpy, surf);
        if (child_xid && p_XDestroyWindow)  p_XDestroyWindow(xdpy, child_xid);
        if (child_cmap && p_XFreeColormap)  p_XFreeColormap(xdpy, child_cmap);
        p_eglDestroyContext(edpy, ctx);
        return 0;
    }

    if (p_eglSwapInterval) p_eglSwapInterval(edpy, 1);

    EglAttachment *att = (EglAttachment *) calloc(1, sizeof(EglAttachment));
    if (!att) {
        p_eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        p_eglDestroySurface(edpy, surf);
        if (child_xid && p_XDestroyWindow)  p_XDestroyWindow(xdpy, child_xid);
        if (child_cmap && p_XFreeColormap)  p_XFreeColormap(xdpy, child_cmap);
        p_eglDestroyContext(edpy, ctx);
        return 0;
    }
    att->display        = edpy;
    att->config         = chosen;
    att->context        = ctx;
    att->surface        = surf;
    att->xdisplay       = xdpy;
    att->parent_xid     = xwin;
    att->child_xid      = child_xid;
    att->child_colormap = child_cmap;
    att->widthPx        = widthPx  > 0 ? widthPx  : wa.width;
    att->heightPx       = heightPx > 0 ? heightPx : wa.height;
    att->scale          = 1.0f;
    atomic_store(&att->output_generation, 1);
    atomic_store(&att->presented_frames, 0);
    DBG("attached: edpy=%p ctx=%p surf=%p (child=0x%lx)\n",
        edpy, (void*)ctx, (void*)surf, child_xid);
    return (jlong) (uintptr_t) att;
}

/* ── Wayland subsurface plumbing ────────────────────────────────────────── */

/**
 * State carried by the wl_registry::global listener while we're binding
 * `wl_compositor` and `wl_subcompositor` during attach. Owned by the calling
 * thread for the duration of the roundtrip — do not leak.
 */
typedef struct {
    wl_proxy *registry;
    wl_event_queue *queue;
    wl_proxy *compositor;
    wl_proxy *subcompositor;
    wl_proxy *color_manager;
    wl_proxy *color_surface;
    wl_proxy *scrgb_description;
    wl_proxy *presentation;
    wl_proxy *outputs[16];
    uint32_t output_count;
    int supports_perceptual;
    int supports_windows_scrgb;
    int supports_parametric;
    int supports_pq;
    int supports_bt2020;
    int scrgb_ready;
    int scrgb_failed;
} WlBindState;

static void wl_output_geometry_ignored(
    void *data, wl_proxy *output, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel,
    const char *make, const char *model, int32_t transform)
{
    (void) data; (void) output; (void) x; (void) y;
    (void) physical_width; (void) physical_height; (void) subpixel;
    (void) make; (void) model; (void) transform;
}

static void wl_output_mode_ignored(
    void *data, wl_proxy *output, uint32_t flags,
    int32_t width, int32_t height, int32_t refresh)
{
    (void) data; (void) output; (void) flags;
    (void) width; (void) height; (void) refresh;
}

static void (*const nucleus_wl_output_listener[])(void) = {
    (void (*)(void)) wl_output_geometry_ignored,
    (void (*)(void)) wl_output_mode_ignored,
};

static void presentation_clock_id_ignored(
    void *data, wl_proxy *presentation, uint32_t clock_id)
{
    (void) data; (void) presentation; (void) clock_id;
}

static void (*const nucleus_presentation_listener[])(void) = {
    (void (*)(void)) presentation_clock_id_ignored,
};

static void presentation_feedback_sync_output(
    void *data, wl_proxy *feedback, wl_proxy *output)
{
    (void) feedback;
    PresentationFeedbackData *feedback_data = (PresentationFeedbackData *) data;
    EglAttachment *att = feedback_data ? feedback_data->attachment : NULL;
    if (!att || att->wl_presented_output == output) return;
    att->wl_presented_output = output;
    atomic_fetch_add(&att->output_generation, 1);
    atomic_store(&att->presented_frames, 0);
}

static void presentation_feedback_presented(
    void *data, wl_proxy *feedback,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
    uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    (void) feedback; (void) tv_sec_hi; (void) tv_sec_lo; (void) tv_nsec;
    (void) refresh; (void) seq_hi; (void) seq_lo; (void) flags;
    PresentationFeedbackData *feedback_data = (PresentationFeedbackData *) data;
    EglAttachment *att = feedback_data ? feedback_data->attachment : NULL;
    if (att) atomic_fetch_add(&att->presented_frames, 1);
    release_presentation_feedback(feedback_data);
}

static void presentation_feedback_discarded(void *data, wl_proxy *feedback) {
    (void) feedback;
    release_presentation_feedback((PresentationFeedbackData *) data);
}

static void (*const nucleus_presentation_feedback_listener[])(void) = {
    (void (*)(void)) presentation_feedback_sync_output,
    (void (*)(void)) presentation_feedback_presented,
    (void (*)(void)) presentation_feedback_discarded,
};

static void color_manager_supported_intent(
    void *data, wl_proxy *manager, uint32_t intent)
{
    (void) manager;
    WlBindState *st = (WlBindState *) data;
    if (intent == NUCLEUS_COLOR_INTENT_PERCEPTUAL) st->supports_perceptual = 1;
}

static void color_manager_supported_feature(
    void *data, wl_proxy *manager, uint32_t feature)
{
    (void) manager;
    WlBindState *st = (WlBindState *) data;
    if (feature == NUCLEUS_COLOR_FEATURE_WINDOWS_SCRGB) st->supports_windows_scrgb = 1;
    if (feature == NUCLEUS_COLOR_FEATURE_PARAMETRIC) st->supports_parametric = 1;
}

static void color_manager_supported_tf(
    void *data, wl_proxy *manager, uint32_t value)
{
    (void) manager;
    WlBindState *st = (WlBindState *) data;
    if (value == NUCLEUS_COLOR_TRANSFER_ST2084_PQ) st->supports_pq = 1;
}

static void color_manager_supported_primaries(
    void *data, wl_proxy *manager, uint32_t value)
{
    (void) manager;
    WlBindState *st = (WlBindState *) data;
    if (value == NUCLEUS_COLOR_PRIMARIES_BT2020) st->supports_bt2020 = 1;
}

static void color_manager_done(void *data, wl_proxy *manager) {
    (void) data; (void) manager;
}

static void (*const nucleus_color_manager_listener[])(void) = {
    (void (*)(void)) color_manager_supported_intent,
    (void (*)(void)) color_manager_supported_feature,
    (void (*)(void)) color_manager_supported_tf,
    (void (*)(void)) color_manager_supported_primaries,
    (void (*)(void)) color_manager_done,
};

static void scrgb_description_failed(
    void *data, wl_proxy *description, uint32_t cause, const char *message)
{
    (void) description; (void) cause; (void) message;
    ((WlBindState *) data)->scrgb_failed = 1;
}

static void scrgb_description_ready(
    void *data, wl_proxy *description, uint32_t identity)
{
    (void) description; (void) identity;
    ((WlBindState *) data)->scrgb_ready = 1;
}

static void (*const nucleus_image_description_listener[])(void) = {
    (void (*)(void)) scrgb_description_failed,
    (void (*)(void)) scrgb_description_ready,
};

static void destroy_color_protocol_object(wl_proxy **object) {
    if (!object || !*object) return;
    p_wl_proxy_marshal_flags(
        *object, 0, NULL, p_wl_proxy_get_version(*object), WL_MARSHAL_FLAG_DESTROY);
    *object = NULL;
}

static void destroy_bound_outputs(WlBindState *st) {
    if (!st) return;
    for (uint32_t index = 0; index < st->output_count; index++) {
        if (st->outputs[index] && p_wl_proxy_destroy) {
            p_wl_proxy_destroy(st->outputs[index]);
            st->outputs[index] = NULL;
        }
    }
    st->output_count = 0;
}

static void destroy_bind_extension_globals(WlBindState *st) {
    if (!st) return;
    destroy_color_protocol_object(&st->presentation);
    destroy_bound_outputs(st);
    destroy_color_protocol_object(&st->color_manager);
}

static void discard_scrgb_surface_state(WlBindState *st) {
    if (!st) return;
    if (st->color_surface) {
        p_wl_proxy_marshal_flags(
            st->color_surface,
            NUCLEUS_COLOR_SURFACE_UNSET_DESCRIPTION,
            NULL,
            p_wl_proxy_get_version(st->color_surface),
            0);
    }
    destroy_color_protocol_object(&st->scrgb_description);
    destroy_color_protocol_object(&st->color_surface);
    st->scrgb_ready = 0;
    st->scrgb_failed = 0;
}

static int configure_windows_scrgb(
    WlBindState *st, wl_display *display, wl_proxy *surface)
{
    if (!st || !display || !surface || !st->color_manager ||
        !st->supports_perceptual || !st->supports_windows_scrgb) return 0;
    st->color_surface = p_wl_proxy_marshal_flags(
        st->color_manager,
        NUCLEUS_COLOR_MANAGER_GET_SURFACE,
        &nucleus_color_surface_interface,
        p_wl_proxy_get_version(st->color_manager),
        0,
        NULL,
        surface);
    st->scrgb_description = p_wl_proxy_marshal_flags(
        st->color_manager,
        NUCLEUS_COLOR_MANAGER_CREATE_SCRGB,
        &nucleus_image_description_interface,
        p_wl_proxy_get_version(st->color_manager),
        0,
        NULL);
    if (!st->color_surface || !st->scrgb_description) {
        discard_scrgb_surface_state(st);
        return 0;
    }
    if (st->queue) {
        p_wl_proxy_set_queue(st->color_surface, st->queue);
        p_wl_proxy_set_queue(st->scrgb_description, st->queue);
    }
    if (p_wl_proxy_add_listener(
            st->scrgb_description,
            (void (**)(void)) nucleus_image_description_listener,
            st) != 0 ||
        p_wl_display_roundtrip_queue(display, st->queue) < 0 ||
        !st->scrgb_ready || st->scrgb_failed) {
        discard_scrgb_surface_state(st);
        return 0;
    }
    p_wl_proxy_marshal_flags(
        st->color_surface,
        NUCLEUS_COLOR_SURFACE_SET_DESCRIPTION,
        NULL,
        p_wl_proxy_get_version(st->color_surface),
        0,
        st->scrgb_description,
        NUCLEUS_COLOR_INTENT_PERCEPTUAL);
    return 1;
}

static int configure_bt2020_pq(
    WlBindState *st, wl_display *display, wl_proxy *surface)
{
    if (!st || !display || !surface || !st->color_manager ||
        !st->supports_perceptual || !st->supports_parametric ||
        !st->supports_pq || !st->supports_bt2020) return 0;
    st->color_surface = p_wl_proxy_marshal_flags(
        st->color_manager,
        NUCLEUS_COLOR_MANAGER_GET_SURFACE,
        &nucleus_color_surface_interface,
        p_wl_proxy_get_version(st->color_manager),
        0,
        NULL,
        surface);
    wl_proxy *creator = p_wl_proxy_marshal_flags(
        st->color_manager,
        NUCLEUS_COLOR_MANAGER_CREATE_PARAMS,
        &nucleus_parametric_creator_interface,
        p_wl_proxy_get_version(st->color_manager),
        0,
        NULL);
    if (!st->color_surface || !creator) {
        if (creator && p_wl_proxy_destroy) p_wl_proxy_destroy(creator);
        discard_scrgb_surface_state(st);
        return 0;
    }
    if (st->queue) {
        p_wl_proxy_set_queue(st->color_surface, st->queue);
        p_wl_proxy_set_queue(creator, st->queue);
    }
    p_wl_proxy_marshal_flags(
        creator,
        NUCLEUS_PARAMETRIC_SET_TF_NAMED,
        NULL,
        p_wl_proxy_get_version(creator),
        0,
        NUCLEUS_COLOR_TRANSFER_ST2084_PQ);
    p_wl_proxy_marshal_flags(
        creator,
        NUCLEUS_PARAMETRIC_SET_PRIMARIES_NAMED,
        NULL,
        p_wl_proxy_get_version(creator),
        0,
        NUCLEUS_COLOR_PRIMARIES_BT2020);
    st->scrgb_description = p_wl_proxy_marshal_flags(
        creator,
        NUCLEUS_PARAMETRIC_CREATE,
        &nucleus_image_description_interface,
        p_wl_proxy_get_version(creator),
        WL_MARSHAL_FLAG_DESTROY,
        NULL);
    if (!st->scrgb_description) {
        discard_scrgb_surface_state(st);
        return 0;
    }
    if (st->queue) p_wl_proxy_set_queue(st->scrgb_description, st->queue);
    st->scrgb_ready = 0;
    st->scrgb_failed = 0;
    if (p_wl_proxy_add_listener(
            st->scrgb_description,
            (void (**)(void)) nucleus_image_description_listener,
            st) != 0 ||
        p_wl_display_roundtrip_queue(display, st->queue) < 0 ||
        !st->scrgb_ready || st->scrgb_failed) {
        discard_scrgb_surface_state(st);
        return 0;
    }
    p_wl_proxy_marshal_flags(
        st->color_surface,
        NUCLEUS_COLOR_SURFACE_SET_DESCRIPTION,
        NULL,
        p_wl_proxy_get_version(st->color_surface),
        0,
        st->scrgb_description,
        NUCLEUS_COLOR_INTENT_PERCEPTUAL);
    return 1;
}

static void wl_registry_global(
    void *data, wl_proxy *registry, uint32_t name,
    const char *interface, uint32_t version)
{
    WlBindState *st = (WlBindState *) data;
    if (!st->compositor && strcmp(interface, "wl_compositor") == 0) {
        /* `wl_registry::bind` has signature "usun" (uint name, string interface,
         * uint version, new_id). For new_id without statically-known interface
         * libwayland sends `interface_name`+`version`+`new_id_handle` triplet on
         * the wire. Match what `wl_registry_bind` inline does. */
        uint32_t v = version < 4 ? version : 4;
        st->compositor = p_wl_proxy_marshal_flags(
            registry, WL_REGISTRY_BIND, g_wl_compositor_interface, v, 0,
            name, "wl_compositor", v, NULL);
    } else if (!st->subcompositor && strcmp(interface, "wl_subcompositor") == 0) {
        uint32_t v = version < 1 ? version : 1;
        st->subcompositor = p_wl_proxy_marshal_flags(
            registry, WL_REGISTRY_BIND, g_wl_subcompositor_interface, v, 0,
            name, "wl_subcompositor", v, NULL);
    } else if (!st->color_manager && strcmp(interface, "wp_color_manager_v1") == 0) {
        uint32_t v = version < 1 ? version : 1;
        st->color_manager = p_wl_proxy_marshal_flags(
            registry, WL_REGISTRY_BIND, &nucleus_color_manager_interface, v, 0,
            name, "wp_color_manager_v1", v, NULL);
        if (st->color_manager) {
            if (st->queue) p_wl_proxy_set_queue(st->color_manager, st->queue);
            p_wl_proxy_add_listener(
                st->color_manager,
                (void (**)(void)) nucleus_color_manager_listener,
                st);
        }
    } else if (!st->presentation && strcmp(interface, "wp_presentation") == 0) {
        uint32_t v = version < 1 ? version : 1;
        st->presentation = p_wl_proxy_marshal_flags(
            registry, WL_REGISTRY_BIND, &nucleus_presentation_interface, v, 0,
            name, "wp_presentation", v, NULL);
        if (st->presentation) {
            if (st->queue) p_wl_proxy_set_queue(st->presentation, st->queue);
            p_wl_proxy_add_listener(
                st->presentation,
                (void (**)(void)) nucleus_presentation_listener,
                st);
        }
    } else if (g_wl_output_interface &&
               strcmp(interface, "wl_output") == 0 &&
               st->output_count < 16) {
        wl_proxy *output = p_wl_proxy_marshal_flags(
            registry, WL_REGISTRY_BIND, g_wl_output_interface, 1, 0,
            name, "wl_output", 1, NULL);
        if (output) {
            if (st->queue) p_wl_proxy_set_queue(output, st->queue);
            p_wl_proxy_add_listener(
                output,
                (void (**)(void)) nucleus_wl_output_listener,
                st);
            st->outputs[st->output_count++] = output;
        }
    }
}

static void wl_registry_global_remove(
    void *data, wl_proxy *registry, uint32_t name)
{
    (void) data; (void) registry; (void) name;
    /* No-op: compositor / subcompositor never disappear at runtime. */
}

/* Pointer-table fed to wl_proxy_add_listener — order must match the events
 * declared in wl_registry's wl_message[] (global, global_remove). */
static void (*const wl_registry_listener[])(void) = {
    (void (*)(void)) wl_registry_global,
    (void (*)(void)) wl_registry_global_remove,
};

/**
 * Sets the child subsurface's `buffer_scale` so the compositor reads our
 * `logical × scale` px buffer as `logical` surface units — matching GTK's
 * parent surface. Without it `buffer_scale` defaults to 1 and the subsurface
 * renders ~`scale`× oversized (and input lands in the wrong place because the
 * visible content no longer matches the scene's pixel geometry).
 *
 * GTK3 only ever reports an integer scale, so we mirror that integer here
 * (true fractional sharpness would need wp_viewporter + wp_fractional_scale_v1,
 * which is only clean once the renderer owns the toplevel — see file header).
 *
 * This only queues double-buffered surface state; it's applied atomically with
 * the new buffer at the next `eglSwapBuffers` commit, so no explicit
 * `wl_surface.commit` is issued here.
 */
static void wl_set_buffer_scale(EglAttachment *att, int scale) {
    if (!att || !att->wl_child_surface || !p_wl_proxy_marshal_flags) return;
    if (scale < 1) scale = 1;
    p_wl_proxy_marshal_flags(
        att->wl_child_surface, WL_SURFACE_SET_BUFFER_SCALE, NULL,
        p_wl_proxy_get_version(att->wl_child_surface), 0, scale);
}


/**
 * Wayland-native attach.
 *
 * We don't render onto GTK's wl_surface — GTK paints a cairo SHM buffer
 * onto it on every draw signal, and any `eglSwapBuffers` we'd issue on
 * the same surface would race with GTK's commit (and worse: trip the
 * xdg_shell `invalid_surface_state` error if we attached before GTK's
 * `get_xdg_surface`). Instead we own a `wl_subsurface` child of GTK's
 * surface. GTK keeps owning the parent + xdg_toplevel; we render into
 * the subsurface in `set_desync` mode so our commits land independently.
 *
 * Shape: same architectural pattern as the X11 child-window fallback,
 * just expressed through Wayland protocol primitives.
 */
JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeAttachWayland(
    JNIEnv *env, jclass clazz,
    jlong wlDisplayPtr, jlong wlSurfacePtr,
    jint widthPx, jint heightPx, jint bufferScale, jint swapInterval,
    jboolean extendedDynamicRange)
{
    (void) env; (void) clazz;
    if (!wlDisplayPtr || !wlSurfacePtr) return 0;
    if (!load_libs()) return 0;
    if (!p_wl_egl_window_create) {
        fprintf(stderr,
                "[nucleus_tao_egl] Wayland path unavailable — libwayland-egl.so.1 missing.\n");
        return 0;
    }
    if (!p_wl_proxy_marshal_flags || !g_wl_registry_interface ||
        !g_wl_compositor_interface || !g_wl_subcompositor_interface ||
        !g_wl_subsurface_interface || !g_wl_surface_interface) {
        fprintf(stderr,
                "[nucleus_tao_egl] Wayland path unavailable — libwayland-client.so.0 "
                "or its interface tables couldn't be resolved.\n");
        return 0;
    }

    wl_display *wdpy  = (wl_display *) (uintptr_t) wlDisplayPtr;
    wl_proxy   *wparent = (wl_proxy *)   (uintptr_t) wlSurfacePtr;
    int phys_w = widthPx > 0 ? widthPx : 1;
    int phys_h = heightPx > 0 ? heightPx : 1;
    DBG("attachWayland: wl_display=%p parent_wl_surface=%p wxh=%dx%d\n",
        (void*)wdpy, (void*)wparent, phys_w, phys_h);

    /* ── 1) Bind wl_compositor + wl_subcompositor on a private event queue ── */
    /* Private queue keeps registry / compositor / subcompositor / our
     * subsurface events away from GDK's default queue — otherwise our
     * roundtrip below would dispatch GDK events out of band and freeze GTK.
     */
    wl_event_queue *queue = p_wl_display_create_queue(wdpy);
    if (!queue) {
        DBG("wl_display_create_queue failed\n");
        return 0;
    }

    /* `wl_display.get_registry` constructor — we treat the wl_display* as a
     * wl_proxy, which is safe because libwayland's wl_display is allocated
     * through the same wl_proxy machinery (the type punning is documented
     * in the wayland-client header). */
    wl_proxy *registry = p_wl_proxy_marshal_flags(
        (wl_proxy *) wdpy, WL_DISPLAY_GET_REGISTRY,
        g_wl_registry_interface,
        p_wl_proxy_get_version((wl_proxy *) wdpy),
        0, NULL);
    if (!registry) {
        DBG("wl_display.get_registry returned NULL\n");
        p_wl_event_queue_destroy(queue);
        return 0;
    }
    p_wl_proxy_set_queue(registry, queue);

    WlBindState bind_state;
    memset(&bind_state, 0, sizeof(bind_state));
    bind_state.registry = registry;
    bind_state.queue = queue;
    p_wl_proxy_add_listener(
        registry, (void (**)(void)) wl_registry_listener, &bind_state);

    /* Roundtrip on OUR queue: send `wl_display.sync`, dispatch all events
     * received on `queue` until the sync done event arrives — covers the
     * initial burst of `wl_registry::global` events.
     *
     * Compositor / subcompositor pointers also need their own queue or
     * future events on them (e.g. the bind ack) would land on GDK's queue.
     * `wl_proxy_set_queue` on freshly-created proxies only matters for
     * future events — they're set inside `wl_registry_global` after creation
     * because the proxy is created by wl_proxy_marshal_flags above. We do
     * the set_queue here explicitly to be defensive. */
    if (p_wl_display_roundtrip_queue(wdpy, queue) < 0) {
        DBG("wl_display_roundtrip_queue failed\n");
        destroy_bind_extension_globals(&bind_state);
        if (bind_state.compositor) p_wl_proxy_destroy(bind_state.compositor);
        if (bind_state.subcompositor) p_wl_proxy_destroy(bind_state.subcompositor);
        p_wl_proxy_destroy(registry);
        p_wl_event_queue_destroy(queue);
        return 0;
    }
    if (bind_state.color_manager &&
        p_wl_display_roundtrip_queue(wdpy, queue) < 0) {
        destroy_bind_extension_globals(&bind_state);
        if (bind_state.compositor) p_wl_proxy_destroy(bind_state.compositor);
        if (bind_state.subcompositor) p_wl_proxy_destroy(bind_state.subcompositor);
        p_wl_proxy_destroy(registry);
        p_wl_event_queue_destroy(queue);
        return 0;
    }
    if (!bind_state.compositor || !bind_state.subcompositor) {
        fprintf(stderr,
                "[nucleus_tao_egl] Wayland compositor missing %s%s%s — "
                "compositor too old? subsurface support is mandatory in Wayland 1.4+.\n",
                bind_state.compositor ? "" : "wl_compositor",
                (!bind_state.compositor && !bind_state.subcompositor) ? " and " : "",
                bind_state.subcompositor ? "" : "wl_subcompositor");
        destroy_bind_extension_globals(&bind_state);
        if (bind_state.compositor) p_wl_proxy_destroy(bind_state.compositor);
        if (bind_state.subcompositor) p_wl_proxy_destroy(bind_state.subcompositor);
        p_wl_proxy_destroy(registry);
        p_wl_event_queue_destroy(queue);
        return 0;
    }
    p_wl_proxy_set_queue(bind_state.compositor, queue);
    p_wl_proxy_set_queue(bind_state.subcompositor, queue);

    /* ── 2) Create our owned child wl_surface via wl_compositor.create_surface ── */
    wl_proxy *child_surface = p_wl_proxy_marshal_flags(
        bind_state.compositor, WL_COMPOSITOR_CREATE_SURFACE,
        g_wl_surface_interface,
        p_wl_proxy_get_version(bind_state.compositor),
        0, NULL);
    if (!child_surface) {
        DBG("wl_compositor.create_surface returned NULL\n");
        destroy_bind_extension_globals(&bind_state);
        p_wl_proxy_destroy(bind_state.subcompositor);
        p_wl_proxy_destroy(bind_state.compositor);
        p_wl_proxy_destroy(registry);
        p_wl_event_queue_destroy(queue);
        return 0;
    }
    p_wl_proxy_set_queue(child_surface, queue);

    /* ── 3) Make the child a subsurface of GTK's parent ── */
    wl_proxy *subsurface = p_wl_proxy_marshal_flags(
        bind_state.subcompositor, WL_SUBCOMPOSITOR_GET_SUBSURFACE,
        g_wl_subsurface_interface,
        p_wl_proxy_get_version(bind_state.subcompositor),
        0,
        /* new_id placeholder */ NULL,
        /* surface (child)   */ child_surface,
        /* parent (GTK's)    */ wparent);
    if (!subsurface) {
        DBG("wl_subcompositor.get_subsurface returned NULL\n");
        p_wl_proxy_marshal_flags(child_surface, WL_SURFACE_DESTROY, NULL,
            p_wl_proxy_get_version(child_surface), WL_MARSHAL_FLAG_DESTROY);
        destroy_bind_extension_globals(&bind_state);
        p_wl_proxy_destroy(bind_state.subcompositor);
        p_wl_proxy_destroy(bind_state.compositor);
        p_wl_proxy_destroy(registry);
        p_wl_event_queue_destroy(queue);
        return 0;
    }
    p_wl_proxy_set_queue(subsurface, queue);

    /* Position relative to parent (top-left corner aligned), and `set_desync`
     * so our buffer commits don't have to wait for the parent's transaction.
     * Spec: subsurface becomes mapped once parent is mapped AND a non-NULL
     * buffer has been applied — first eglSwapBuffers does the latter, GTK
     * does the former. */
    p_wl_proxy_marshal_flags(
        subsurface, WL_SUBSURFACE_SET_POSITION, NULL,
        p_wl_proxy_get_version(subsurface), 0,
        /*x*/ 0, /*y*/ 0);
    p_wl_proxy_marshal_flags(
        subsurface, WL_SUBSURFACE_SET_DESYNC, NULL,
        p_wl_proxy_get_version(subsurface), 0);

    /* Make the child surface input-transparent so the compositor routes
     * pointer / keyboard / touch events to GTK's parent surface (where tao
     * has its event handlers wired). Without this our EGL-rendered surface
     * sits on top of GTK's parent and silently swallows every click — the
     * Wayland equivalent of XShape `ShapeInput=empty` that the X11 child
     * window uses for the same purpose.
     *
     * `wl_compositor.create_region` with no rects added = empty region.
     * `set_input_region(empty)` makes the surface ignore all input. The
     * region is double-buffered surface state, applied at the next commit
     * on the child (and that commit is gated on the parent's commit while
     * we're still in default sync mode — but `set_input_region` queued
     * before the first eglSwapBuffers is fine because the buffer commit
     * carries the input region with it). */
    if (g_wl_region_interface) {
        wl_proxy *empty_region = p_wl_proxy_marshal_flags(
            bind_state.compositor, WL_COMPOSITOR_CREATE_REGION,
            g_wl_region_interface,
            p_wl_proxy_get_version(bind_state.compositor),
            0, NULL);
        if (empty_region) {
            p_wl_proxy_set_queue(empty_region, queue);
            p_wl_proxy_marshal_flags(
                child_surface, WL_SURFACE_SET_INPUT_REGION, NULL,
                p_wl_proxy_get_version(child_surface), 0,
                empty_region);
            /* The compositor only needs the region for the duration of
             * `set_input_region` — we can destroy our handle right away. */
            p_wl_proxy_marshal_flags(empty_region, WL_REGION_DESTROY, NULL,
                p_wl_proxy_get_version(empty_region), WL_MARSHAL_FLAG_DESTROY);
            /* Explicit commit so the input region becomes active immediately
             * — without it, the empty region only takes effect at the first
             * `eglSwapBuffers` (which carries an implicit commit). DnD events
             * arriving before that first frame would otherwise reach the
             * subsurface and steal pointer focus from the GTK parent, which
             * is exactly the bug we're fixing. The subsurface is in desync
             * mode (set just above), so this commit applies independently of
             * the parent's transaction.
             *
             * `wl_surface.commit` takes no arguments — `flags=0` is correct. */
            p_wl_proxy_marshal_flags(
                child_surface, WL_SURFACE_COMMIT, NULL,
                p_wl_proxy_get_version(child_surface), 0);
        }
    }

    /* ── 4) EGL setup against our child wl_surface ── */
    /* The image description must be ready before it can be associated with a
     * surface. This private-queue roundtrip happens before EGL attaches the
     * first buffer, so no incorrectly-tagged frame can reach the compositor. */
    const int requested_extended = extendedDynamicRange == JNI_TRUE;

    EGLDisplay edpy = EGL_NO_DISPLAY;
    if (p_eglGetPlatformDisplay) {
        edpy = p_eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wdpy, NULL);
    }
    if (edpy == EGL_NO_DISPLAY && p_eglGetDisplay) {
        edpy = p_eglGetDisplay((EGLNativeDisplayType) wdpy);
    }
    if (edpy == EGL_NO_DISPLAY) {
        DBG("eglGetPlatformDisplay(WAYLAND) returned EGL_NO_DISPLAY\n");
        goto fail_after_subsurface;
    }
    EGLint maj = 0, min = 0;
    if (!p_eglInitialize(edpy, &maj, &min)) {
        DBG("eglInitialize failed: 0x%x\n", p_eglGetError ? p_eglGetError() : 0);
        goto fail_after_subsurface;
    }
    DBG("EGL %d.%d initialized (Wayland)\n", maj, min);
    log_egl_diagnostics_once(edpy, /*is_wayland=*/1);

    if (!p_eglBindAPI(EGL_OPENGL_API)) {
        DBG("eglBindAPI(EGL_OPENGL_API) failed on Wayland EGL\n");
        goto fail_after_subsurface;
    }

    const char *egl_extensions = p_eglQueryString
        ? p_eglQueryString(edpy, EGL_EXTENSIONS) : NULL;
    const EGLint cfg_attrs_sdr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_SAMPLES,         0,
        EGL_NONE
    };
    const EGLint cfg_attrs_scrgb[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        16,
        EGL_GREEN_SIZE,      16,
        EGL_BLUE_SIZE,       16,
        EGL_ALPHA_SIZE,      16,
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_SAMPLES,         0,
        EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT,
        EGL_NONE
    };
    const EGLint cfg_attrs_pq[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        10,
        EGL_GREEN_SIZE,      10,
        EGL_BLUE_SIZE,       10,
        EGL_ALPHA_SIZE,      2,
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_SAMPLES,         0,
        EGL_NONE
    };

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
            EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_NONE
    };
    const EGLint scrgb_surface_attrs[] = {
        EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT,
        EGL_NONE
    };
    const EGLint pq_surface_attrs[] = {
        EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_BT2020_PQ_EXT,
        EGL_NONE
    };
    const int can_scrgb =
        requested_extended &&
        extension_list_contains(egl_extensions, "EGL_EXT_pixel_format_float") &&
        extension_list_contains(egl_extensions, "EGL_EXT_gl_colorspace_scrgb_linear");
    const int can_pq =
        requested_extended &&
        extension_list_contains(egl_extensions, "EGL_EXT_gl_colorspace_bt2020_pq");
    int candidates[3];
    int candidate_count = 0;
    if (can_scrgb) candidates[candidate_count++] = NUCLEUS_OUTPUT_SCRGB;
    if (can_pq) candidates[candidate_count++] = NUCLEUS_OUTPUT_PQ;
    candidates[candidate_count++] = NUCLEUS_OUTPUT_SDR;

    wl_egl_window *wlwin = p_wl_egl_window_create((wl_surface *) child_surface, phys_w, phys_h);
    if (!wlwin) {
        DBG("wl_egl_window_create returned NULL\n");
        goto fail_after_subsurface;
    }
    EglAttachment *att = (EglAttachment *) calloc(1, sizeof(EglAttachment));
    if (!att) {
        if (p_wl_egl_window_destroy) p_wl_egl_window_destroy(wlwin);
        goto fail_after_subsurface;
    }
    att->display = edpy;
    att->widthPx = phys_w;
    att->heightPx = phys_h;
    att->pq_scene_sampler = -1;

    EGLConfig cfg = NULL;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    int output_mode = NUCLEUS_OUTPUT_SDR;
    for (int candidate_index = 0; candidate_index < candidate_count; candidate_index++) {
        const int candidate = candidates[candidate_index];
        discard_scrgb_surface_state(&bind_state);
        if (candidate == NUCLEUS_OUTPUT_SCRGB &&
            !configure_windows_scrgb(&bind_state, wdpy, child_surface)) continue;
        if (candidate == NUCLEUS_OUTPUT_PQ &&
            !configure_bt2020_pq(&bind_state, wdpy, child_surface)) continue;
        const EGLint *cfg_attrs =
            candidate == NUCLEUS_OUTPUT_SCRGB ? cfg_attrs_scrgb :
            candidate == NUCLEUS_OUTPUT_PQ ? cfg_attrs_pq : cfg_attrs_sdr;
        EGLint ncfg = 0;
        cfg = NULL;
        if (!p_eglChooseConfig(edpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg <= 0 || !cfg) {
            continue;
        }
        ctx = p_eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
        if (ctx == EGL_NO_CONTEXT) continue;
        const EGLint *surface_attrs =
            candidate == NUCLEUS_OUTPUT_SCRGB ? scrgb_surface_attrs :
            candidate == NUCLEUS_OUTPUT_PQ ? pq_surface_attrs : NULL;
        surf = p_eglCreateWindowSurface(
            edpy, cfg, (EGLNativeWindowType) wlwin, surface_attrs);
        if (surf == EGL_NO_SURFACE || !p_eglMakeCurrent(edpy, surf, surf, ctx)) {
            if (surf != EGL_NO_SURFACE) p_eglDestroySurface(edpy, surf);
            p_eglDestroyContext(edpy, ctx);
            surf = EGL_NO_SURFACE;
            ctx = EGL_NO_CONTEXT;
            continue;
        }
        att->config = cfg;
        att->context = ctx;
        att->surface = surf;
        att->output_mode = candidate;
        att->extended_scene = candidate != NUCLEUS_OUTPUT_SDR;
        if (candidate == NUCLEUS_OUTPUT_PQ && !initialize_pq_resources(att)) {
            p_eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            p_eglDestroySurface(edpy, surf);
            p_eglDestroyContext(edpy, ctx);
            surf = EGL_NO_SURFACE;
            ctx = EGL_NO_CONTEXT;
            att->config = NULL;
            att->context = EGL_NO_CONTEXT;
            att->surface = EGL_NO_SURFACE;
            att->output_mode = NUCLEUS_OUTPUT_SDR;
            att->extended_scene = 0;
            continue;
        }
        output_mode = candidate;
        break;
    }
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
        DBG("No usable Wayland EGL SDR/scRGB/PQ surface: 0x%x\n",
            p_eglGetError ? p_eglGetError() : 0);
        discard_scrgb_surface_state(&bind_state);
        free(att);
        if (p_wl_egl_window_destroy) p_wl_egl_window_destroy(wlwin);
        goto fail_after_subsurface;
    }

    /* eglSwapInterval(1) — relies on the JVM-side architecture binding the
     * EGL context to a *dedicated* swap thread, not the GTK main thread. On
     * Wayland the swap blocks waiting for the compositor's frame callback,
     * which only fires when the GTK main loop pumps `wl_display_dispatch`.
     * If the swap and the dispatch share a thread we deadlock. The Kotlin
     * side (`TaoComposeSceneHostLinux.SwapThread`) makes the context
     * current on a separate thread and calls `nativePresent` there, so the
     * GTK main thread keeps draining wl events while the swap thread waits
     * on the compositor — and we get true hardware vsync without melting
     * the CPU.
     *
     * swapInterval == 0 is used for popup overlays (drag ghosts): their EGL
     * child is a subsurface of GDK's own synchronized wl_subsurface, so FIFO
     * commits stay cached compositor-side and Mesa's pending
     * wp_commit_timer_v1 timestamp is never consumed — the next
     * set_timestamp raises a fatal "Commit already has timestamp" protocol
     * error. Interval 0 keeps Mesa off the fifo/commit-timing path entirely;
     * pacing for those surfaces is event-driven (pointer motion) anyway. */
    if (p_eglSwapInterval) p_eglSwapInterval(edpy, swapInterval ? 1 : 0);

    att->display          = edpy;
    att->config           = cfg;
    att->context          = ctx;
    att->surface          = surf;
    att->wl_queue         = queue;
    att->wl_registry      = registry;
    att->wl_compositor    = bind_state.compositor;
    att->wl_subcompositor = bind_state.subcompositor;
    att->wl_display_conn  = wdpy;
    att->wl_parent_surface = wparent;
    att->wl_child_surface = child_surface;
    att->wl_subsurface    = subsurface;
    att->wl_window        = wlwin;
    att->wl_color_manager = bind_state.color_manager;
    att->wl_color_surface = bind_state.color_surface;
    att->wl_scrgb_description = bind_state.scrgb_description;
    att->wl_presentation = bind_state.presentation;
    att->wl_output_count = bind_state.output_count;
    for (uint32_t index = 0; index < bind_state.output_count; index++) {
        att->wl_outputs[index] = bind_state.outputs[index];
    }
    att->output_mode      = output_mode;
    att->extended_scene   = output_mode != NUCLEUS_OUTPUT_SDR;
    att->widthPx          = phys_w;
    att->heightPx         = phys_h;
    att->scale            = (float) (bufferScale > 0 ? bufferScale : 1);
    atomic_store(&att->output_generation, 1);
    atomic_store(&att->presented_frames, 0);
    /* The registry listener points at stack-owned bind_state. All globals we
     * need have been bound, so destroy the registry before returning instead
     * of leaving a dangling listener behind. Existing global proxies remain
     * valid for the connection lifetime. */
    if (registry && p_wl_proxy_destroy) {
        p_wl_proxy_destroy(registry);
        registry = NULL;
        att->wl_registry = NULL;
    }
    /* Match GTK's integer surface scale so the `logical × scale` px buffer is
     * read as `logical` surface units (no oversize, input stays calibrated). */
    wl_set_buffer_scale(att, bufferScale);
    DBG("attached (Wayland subsurface): edpy=%p ctx=%p surf=%p child_surf=%p subsurf=%p scale=%d\n",
        edpy, (void*)ctx, (void*)surf, (void*)child_surface, (void*)subsurface,
        bufferScale);
    return (jlong) (uintptr_t) att;

fail_after_subsurface:
    /* Failure path: tear down the subsurface chain in destruction order. */
    discard_scrgb_surface_state(&bind_state);
    destroy_bind_extension_globals(&bind_state);
    p_wl_proxy_marshal_flags(subsurface, WL_SUBSURFACE_DESTROY, NULL,
        p_wl_proxy_get_version(subsurface), WL_MARSHAL_FLAG_DESTROY);
    p_wl_proxy_marshal_flags(child_surface, WL_SURFACE_DESTROY, NULL,
        p_wl_proxy_get_version(child_surface), WL_MARSHAL_FLAG_DESTROY);
    p_wl_proxy_destroy(bind_state.subcompositor);
    p_wl_proxy_destroy(bind_state.compositor);
    p_wl_proxy_destroy(registry);
    p_wl_event_queue_destroy(queue);
    return 0;
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeDetach(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att) return;
    if (att->display) {
        if (att->context && att->surface &&
            p_eglMakeCurrent(att->display, att->surface, att->surface, att->context)) {
            destroy_pq_resources(att);
        }
        p_eglMakeCurrent(att->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (att->surface) p_eglDestroySurface(att->display, att->surface);
        if (att->context) p_eglDestroyContext(att->display, att->context);
    }
    /* Tear down the child window if we created one. Order matters: destroy
     * the X window before its colormap to avoid X server warnings. */
    if (att->xdisplay && att->child_xid && p_XDestroyWindow) {
        p_XDestroyWindow(att->xdisplay, att->child_xid);
    }
    if (att->xdisplay && att->child_colormap && p_XFreeColormap) {
        p_XFreeColormap(att->xdisplay, att->child_colormap);
    }
    /* Wayland path: destroy the proxies in strict reverse-creation order to
     * keep the compositor's bookkeeping happy.
     *   wl_egl_window → wl_subsurface → wl_child_surface → globals →
     *   registry → queue
     * Inverting any pair triggers a `Bad object` protocol error from Mutter. */
    if (att->wl_window && p_wl_egl_window_destroy) {
        p_wl_egl_window_destroy(att->wl_window);
    }
    while (att->wl_feedback_count > 0) {
        PresentationFeedbackData *feedback =
            att->wl_feedbacks[att->wl_feedback_count - 1];
        release_presentation_feedback(feedback);
    }
    destroy_color_protocol_object(&att->wl_scrgb_description);
    destroy_color_protocol_object(&att->wl_color_surface);
    destroy_color_protocol_object(&att->wl_color_manager);
    destroy_color_protocol_object(&att->wl_presentation);
    for (uint32_t index = 0; index < att->wl_output_count; index++) {
        if (att->wl_outputs[index] && p_wl_proxy_destroy) {
            p_wl_proxy_destroy(att->wl_outputs[index]);
            att->wl_outputs[index] = NULL;
        }
    }
    att->wl_output_count = 0;
    if (att->wl_subsurface && p_wl_proxy_marshal_flags) {
        p_wl_proxy_marshal_flags(att->wl_subsurface, WL_SUBSURFACE_DESTROY,
            NULL, p_wl_proxy_get_version(att->wl_subsurface),
            WL_MARSHAL_FLAG_DESTROY);
    }
    if (att->wl_child_surface && p_wl_proxy_marshal_flags) {
        p_wl_proxy_marshal_flags(att->wl_child_surface, WL_SURFACE_DESTROY,
            NULL, p_wl_proxy_get_version(att->wl_child_surface),
            WL_MARSHAL_FLAG_DESTROY);
    }
    if (att->wl_compositor && p_wl_proxy_destroy)    p_wl_proxy_destroy(att->wl_compositor);
    if (att->wl_subcompositor && p_wl_proxy_destroy) p_wl_proxy_destroy(att->wl_subcompositor);
    if (att->wl_registry && p_wl_proxy_destroy)      p_wl_proxy_destroy(att->wl_registry);
    if (att->wl_queue && p_wl_event_queue_destroy)   p_wl_event_queue_destroy(att->wl_queue);
    free(att);
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeMakeCurrent(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att) return;
    p_eglMakeCurrent(att->display, att->surface, att->surface, att->context);
}

/**
 * Releases the EGL context from the calling thread, leaving no context
 * current. Required when handing the GL context between the GTK main
 * thread (which records draw commands via Skia) and the swap thread
 * (which calls `eglSwapBuffers` and blocks for vsync) — EGL contexts can
 * only be current on one thread at a time.
 */
JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeReleaseCurrent(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att) return;
    p_eglMakeCurrent(att->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeResize(
    JNIEnv *env, jclass clazz, jlong handle, jint widthPx, jint heightPx, jfloat scale)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att) return;
    att->widthPx  = widthPx  > 0 ? widthPx  : 1;
    att->heightPx = heightPx > 0 ? heightPx : 1;
    att->scale    = scale;
    /* When we have a child X window, X11 doesn't auto-resize children with
     * their parent — without this the EGL drawable freezes at its initial
     * size while the GTK frame stretches. XFlush (not XSync) — same NVIDIA
     * Blackwell deadlock concern as the GLX helper noted at length. */
    if (att->xdisplay && att->child_xid && p_XResizeWindow) {
        p_XResizeWindow(att->xdisplay, att->child_xid,
                        (unsigned int) att->widthPx, (unsigned int) att->heightPx);
        if (p_XFlush) p_XFlush(att->xdisplay);
    }
    /* Wayland: `wl_egl_window_resize` informs libwayland-egl that the EGL
     * back buffer should be reallocated at the new size on the next
     * eglSwapBuffers. Without this the buffer stays at its original
     * dimensions and the compositor scales it up, blurring the result. */
    if (att->wl_window && p_wl_egl_window_resize) {
        p_wl_egl_window_resize(att->wl_window,
                               att->widthPx, att->heightPx, 0, 0);
        /* Track DPI changes: re-assert the integer buffer scale so the new
         * buffer is still read as `logical` surface units. Queued state, lands
         * with the next eglSwapBuffers commit. */
        wl_set_buffer_scale(att, (int) (scale + 0.5f));
    }
    if (att->output_mode == NUCLEUS_OUTPUT_PQ) {
        resize_pq_scene_target(att, att->widthPx, att->heightPx);
    }
    /* If we render straight into the GTK X window, the EGL surface follows
     * automatically (GTK already issues XResizeWindow on the parent). */
}

/**
 * Positions the content subsurface at ([xLogical], [yLogical]) inside GTK's
 * parent surface. (0,0) for plain undecorated toplevels; the theme's shadow
 * margins when the hidden-titlebar CSD is active, so the EGL content fills
 * exactly the visible window area and GTK's own drop shadow stays visible in
 * the margin ring. Subsurface position is parent-surface state — it takes
 * effect on GTK's next commit, which follows naturally from its ongoing
 * draws. Cheap no-op when the offset is unchanged; no-op on X11 (the CSD is
 * never latched there).
 */
JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeSetContentOffset(
    JNIEnv *env, jclass clazz, jlong handle, jint xLogical, jint yLogical)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att || !att->wl_subsurface || !p_wl_proxy_marshal_flags) return;
    if (att->content_off_x == xLogical && att->content_off_y == yLogical) return;
    att->content_off_x = xLogical;
    att->content_off_y = yLogical;
    p_wl_proxy_marshal_flags(att->wl_subsurface, WL_SUBSURFACE_SET_POSITION,
        NULL, p_wl_proxy_get_version(att->wl_subsurface), 0,
        xLogical, yLogical);
    /* Sub-surface position is PARENT-surface state — it only takes effect on
     * GTK's next commit, and after a maximize/restore GTK has already
     * committed its reallocation by the time this runs and then goes idle,
     * which would leave the old offset applied forever (content shifted
     * bottom-right by the former shadow margins). Issue an empty commit on
     * GTK's toplevel surface ourselves: it applies pending state only, and
     * this call always runs on the GTK main thread (the render loop), so
     * GTK is never mid-way through its own attach/damage/commit sequence. */
    if (att->wl_parent_surface) {
        p_wl_proxy_marshal_flags(att->wl_parent_surface, WL_SURFACE_COMMIT,
            NULL, p_wl_proxy_get_version(att->wl_parent_surface), 0);
    }
    if (p_wl_display_flush && att->wl_display_conn) p_wl_display_flush(att->wl_display_conn);
}

/**
 * Declares which part of the content surface is fully opaque.
 *
 * Nothing ever set this, even though the whole design depends on it: without an
 * opaque region the compositor must assume our full-window surface is
 * translucent, so it cannot cull *anything* underneath — not the drop-shadow
 * subsurface's interior, not GTK's toplevel. It alpha-blends all three, every
 * frame, over the whole window.
 *
 * That lands on the compositor's frame timing, which is what GDK's frame clock
 * waits on, which is what sets how fast the window edge can move during a
 * resize. Measured: the toplevel's frame callback comes back ~5 ms sooner with
 * the shadow disabled entirely. Declaring the opaque region lets the compositor
 * discard the same work without removing the shadow.
 *
 * [cornerRadius] carves the four corners out of the region, because
 * `applyFrameDecoration` paints them transparent so the shadow shows through —
 * claiming them opaque would leave square corners with the shadow clipped away.
 *
 * Pass `logicalW <= 0` to clear the region (window genuinely translucent).
 * Coordinates are surface-local (logical) units. Queued state: it lands with the
 * next `eglSwapBuffers` commit, so there is no extra commit and no race with the
 * swap thread.
 */
JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeSetOpaqueRegion(
    JNIEnv *env, jclass clazz, jlong handle,
    jint logicalW, jint logicalH, jint cornerRadius)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att || !att->wl_child_surface || !p_wl_proxy_marshal_flags) return;
    if (!att->wl_compositor || !g_wl_region_interface) return;

    if (logicalW <= 0 || logicalH <= 0) {
        p_wl_proxy_marshal_flags(
            att->wl_child_surface, WL_SURFACE_SET_OPAQUE_REGION, NULL,
            p_wl_proxy_get_version(att->wl_child_surface), 0, NULL);
        return;
    }

    wl_proxy *region = p_wl_proxy_marshal_flags(
        (wl_proxy *) att->wl_compositor, WL_COMPOSITOR_CREATE_REGION,
        g_wl_region_interface,
        p_wl_proxy_get_version((wl_proxy *) att->wl_compositor), 0, NULL);
    if (!region) return;
    if (att->wl_queue && p_wl_proxy_set_queue) p_wl_proxy_set_queue(region, att->wl_queue);

    int r = cornerRadius;
    if (r < 0) r = 0;
    if (2 * r >= logicalW || 2 * r >= logicalH) r = 0;
    if (r == 0) {
        p_wl_proxy_marshal_flags(region, WL_REGION_ADD, NULL,
            p_wl_proxy_get_version(region), 0, 0, 0, logicalW, logicalH);
    } else {
        /* Everything except the four r x r corner squares. */
        p_wl_proxy_marshal_flags(region, WL_REGION_ADD, NULL,
            p_wl_proxy_get_version(region), 0, 0, r, logicalW, logicalH - 2 * r);
        p_wl_proxy_marshal_flags(region, WL_REGION_ADD, NULL,
            p_wl_proxy_get_version(region), 0, r, 0, logicalW - 2 * r, r);
        p_wl_proxy_marshal_flags(region, WL_REGION_ADD, NULL,
            p_wl_proxy_get_version(region), 0, r, logicalH - r, logicalW - 2 * r, r);
    }
    p_wl_proxy_marshal_flags(
        att->wl_child_surface, WL_SURFACE_SET_OPAQUE_REGION, NULL,
        p_wl_proxy_get_version(att->wl_child_surface), 0, region);
    p_wl_proxy_marshal_flags(region, WL_REGION_DESTROY, NULL,
        p_wl_proxy_get_version(region), WL_MARSHAL_FLAG_DESTROY);
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativePresent(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att) return JNI_FALSE;
    present_pq_scene(att);
    if (att->wl_presentation && att->wl_child_surface &&
        att->wl_feedback_count < 64) {
        PresentationFeedbackData *feedback_data =
            (PresentationFeedbackData *) calloc(1, sizeof(PresentationFeedbackData));
        wl_proxy *feedback = p_wl_proxy_marshal_flags(
            att->wl_presentation,
            NUCLEUS_PRESENTATION_FEEDBACK,
            &nucleus_presentation_feedback_interface,
            p_wl_proxy_get_version(att->wl_presentation),
            0,
            att->wl_child_surface,
            NULL);
        if (feedback && feedback_data) {
            feedback_data->attachment = att;
            feedback_data->proxy = feedback;
            att->wl_feedbacks[att->wl_feedback_count++] = feedback_data;
            if (att->wl_queue) p_wl_proxy_set_queue(feedback, att->wl_queue);
            if (p_wl_proxy_add_listener(
                    feedback,
                    (void (**)(void)) nucleus_presentation_feedback_listener,
                    feedback_data) != 0) {
                release_presentation_feedback(feedback_data);
            }
        } else {
            if (feedback && p_wl_proxy_destroy) p_wl_proxy_destroy(feedback);
            free(feedback_data);
        }
    }
    EGLBoolean presented = p_eglSwapBuffers(att->display, att->surface);
    if (presented && (!att->wl_child_surface || !att->wl_presentation)) {
        /* X11 has no Wayland presentation feedback. Wayland SDR keeps its
         * established swap-completion fallback when the stable presentation
         * protocol is unavailable; an HDR scene never reaches this branch on
         * a conforming compositor and therefore cannot be falsely confirmed. */
        if (!att->wl_child_surface || !att->extended_scene) {
            atomic_fetch_add(&att->presented_frames, 1);
        }
    }
    if (att->wl_display_conn && p_wl_display_flush) {
        p_wl_display_flush(att->wl_display_conn);
    }
    return presented ? JNI_TRUE : JNI_FALSE;
}

static void dispatch_pending_presentation_feedback(EglAttachment *att) {
    if (!att || !att->wl_display_conn || !att->wl_queue ||
        !p_wl_display_dispatch_queue_pending) return;
    p_wl_display_dispatch_queue_pending(att->wl_display_conn, att->wl_queue);
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeUsesExtendedScene(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    return att && att->extended_scene ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeFramebufferId(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    return att ? (jint) att->pq_scene_framebuffer : 0;
}

JNIEXPORT jint JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeOutputMode(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    return att ? (jint) att->output_mode : NUCLEUS_OUTPUT_SDR;
}

JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeOutputGeneration(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    dispatch_pending_presentation_feedback(att);
    return att ? (jlong) atomic_load(&att->output_generation) : 0;
}

JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativePresentedFrameCount(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    dispatch_pending_presentation_feedback(att);
    return att ? (jlong) atomic_load(&att->presented_frames) : 0;
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeSetSwapInterval(
    JNIEnv *env, jclass clazz, jlong handle, jint interval)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    if (!att || !p_eglSwapInterval) return;
    /* eglSwapInterval requires the surface to be current. The swap thread
     * always releases before signalling idle, so the caller must ensure it
     * holds the context (nativeMakeCurrent) before calling this. */
    p_eglSwapInterval(att->display, (EGLint) interval);
}

JNIEXPORT jint JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeWidth(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    return att ? (jint) att->widthPx : 0;
}

JNIEXPORT jint JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeHeight(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    return att ? (jint) att->heightPx : 0;
}

/* ── Input region (overlay slot of `NativeView`) ─────────────────────────
 *
 * Switches the EGL surface from the default "fully input-transparent"
 * mode (where every click falls through to GTK and reaches the embedded
 * native widget) to a region-restricted mode: the compositor / X server
 * only routes input to our surface when the cursor sits inside one of
 * the supplied rects. Outside those rects, input still falls through to
 * GTK as before. This is the Linux equivalent of macOS's
 * `NucleusTaoNativeOverlayView.hitTest:` returning `nil` for points
 * outside any registered region.
 *
 * [rectsPx] is a flat (x, y, w, h) × [count] float array in surface-
 * local pixels with a top-left origin (matches Compose `boundsInWindow`
 * → physical px). `count == 0` resets to the default empty region
 * (full passthrough).
 *
 * Wayland: marshals `wl_compositor.create_region` + `wl_region.add` for
 * each rect + `wl_surface.set_input_region` + `wl_surface.commit`.
 *
 * X11 child-window fallback: applies `XShapeCombineRectangles(ShapeInput,
 * rects, ShapeSet)` on `child_xid`. Only effective when a child window
 * was created (visual-mismatch fallback path) — the default-visual X11
 * path renders directly into GTK's xwin and has nothing we can shape
 * without breaking GTK input. We log a warning in that case; full
 * X11-default support would require always materialising a child window
 * for Compose, which is left as a follow-up. */
JNIEXPORT void JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeSetInputRegion(
    JNIEnv *env, jclass clazz, jlong handle, jfloatArray rectsPx, jint count)
{
    (void) clazz;
    DBG("nativeSetInputRegion handle=0x%lx count=%d\n",
        (unsigned long) handle, (int) count);
    if (handle == 0) return;
    EglAttachment *att = (EglAttachment *) (uintptr_t) handle;
    DBG("  wl_child_surface=%p wl_compositor=%p child_xid=0x%lx\n",
        (void *) att->wl_child_surface, (void *) att->wl_compositor,
        (unsigned long) att->child_xid);

    if (count < 0) count = 0;
    int safe_count = 0;
    if (count > 0 && rectsPx != NULL) {
        jsize len = (*env)->GetArrayLength(env, rectsPx);
        safe_count = (int) (len / 4);
        if (safe_count > count) safe_count = count;
    }
    if (safe_count > 0 && rectsPx != NULL) {
        jfloat *raw = (*env)->GetFloatArrayElements(env, rectsPx, NULL);
        if (raw != NULL) {
            for (int i = 0; i < safe_count; i++) {
                DBG("  rect[%d] = (%g, %g, %g, %g)\n",
                    i, raw[i*4+0], raw[i*4+1], raw[i*4+2], raw[i*4+3]);
            }
            (*env)->ReleaseFloatArrayElements(env, rectsPx, raw, JNI_ABORT);
        }
    }

    /* Wayland path. ───────────────────────────────────────────────── */
    if (att->wl_child_surface != NULL && att->wl_compositor != NULL &&
        g_wl_region_interface != NULL && p_wl_proxy_marshal_flags != NULL) {

        wl_proxy *region = p_wl_proxy_marshal_flags(
            (wl_proxy *) att->wl_compositor, WL_COMPOSITOR_CREATE_REGION,
            g_wl_region_interface,
            p_wl_proxy_get_version((wl_proxy *) att->wl_compositor),
            0, NULL);
        if (region != NULL) {
            if (att->wl_queue != NULL && p_wl_proxy_set_queue) {
                p_wl_proxy_set_queue(region, att->wl_queue);
            }
            /* Pull the float quartets into integer surface coords —
             * `wl_region.add` takes ints. We round-down position and
             * round-up size so the region never undershoots the rect
             * Compose drew (avoids 1-pixel unclickable seams on
             * fractional layouts). */
            if (safe_count > 0 && rectsPx != NULL) {
                jfloat *raw = (*env)->GetFloatArrayElements(env, rectsPx, NULL);
                if (raw != NULL) {
                    for (int i = 0; i < safe_count; i++) {
                        int x = (int) raw[i * 4 + 0];
                        int y = (int) raw[i * 4 + 1];
                        int w = (int) (raw[i * 4 + 2] + 0.5f);
                        int h = (int) (raw[i * 4 + 3] + 0.5f);
                        if (w <= 0 || h <= 0) continue;
                        p_wl_proxy_marshal_flags(
                            region, WL_REGION_ADD, NULL,
                            p_wl_proxy_get_version(region), 0,
                            x, y, w, h);
                    }
                    (*env)->ReleaseFloatArrayElements(env, rectsPx, raw, JNI_ABORT);
                }
            }
            p_wl_proxy_marshal_flags(
                (wl_proxy *) att->wl_child_surface, WL_SURFACE_SET_INPUT_REGION,
                NULL, p_wl_proxy_get_version((wl_proxy *) att->wl_child_surface), 0,
                region);
            p_wl_proxy_marshal_flags(
                region, WL_REGION_DESTROY, NULL,
                p_wl_proxy_get_version(region), WL_MARSHAL_FLAG_DESTROY);
            /* Commit so the new input region takes effect on the next
             * frame; subsurface is in desync mode so this lands without
             * waiting for the GTK parent. */
            p_wl_proxy_marshal_flags(
                (wl_proxy *) att->wl_child_surface, WL_SURFACE_COMMIT, NULL,
                p_wl_proxy_get_version((wl_proxy *) att->wl_child_surface), 0);
        }
        return;
    }

    /* X11 child-window fallback. ─────────────────────────────────── */
    if (att->xdisplay != NULL && att->child_xid != (Window) 0 &&
        p_XShapeCombineRectangles != NULL) {
        XRectangle stack_rects[32];
        XRectangle *rects = stack_rects;
        if (safe_count > (int) (sizeof(stack_rects) / sizeof(stack_rects[0]))) {
            rects = (XRectangle *) calloc((size_t) safe_count, sizeof(XRectangle));
            if (rects == NULL) return;
        }
        if (safe_count > 0 && rectsPx != NULL) {
            jfloat *raw = (*env)->GetFloatArrayElements(env, rectsPx, NULL);
            if (raw != NULL) {
                for (int i = 0; i < safe_count; i++) {
                    rects[i].x      = (short) raw[i * 4 + 0];
                    rects[i].y      = (short) raw[i * 4 + 1];
                    rects[i].width  = (unsigned short) (raw[i * 4 + 2] + 0.5f);
                    rects[i].height = (unsigned short) (raw[i * 4 + 3] + 0.5f);
                }
                (*env)->ReleaseFloatArrayElements(env, rectsPx, raw, JNI_ABORT);
            }
        }
        p_XShapeCombineRectangles(
            att->xdisplay, att->child_xid, ShapeInput, 0, 0,
            rects, safe_count, ShapeSet, Unsorted);
        if (rects != stack_rects) free(rects);
        return;
    }

    /* X11 default visual: no separate Compose window, can't shape
     * without breaking GTK. Falls back to current full-passthrough
     * behaviour — overlay clicks won't be intercepted. */
    DBG("nativeSetInputRegion: no shape-able backend (X11 default visual);"
        " overlay clicks pass through to GTK.\n");
}

/**
 * Returns the address of `nucleus_tao_egl_get_proc` so the JVM can pass it to
 * `GLAssembledInterface.createFromNativePointers(0, fnPtr)`. The function
 * pointer is stable for the lifetime of this shared object — same address
 * across multiple windows (Skia keeps a per-`GrGLInterface` ref to it).
 *
 * Loads the EGL libraries on first call so the proc loader is ready before
 * Skia starts asking for entry points.
 */
JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_window_tao_ffi_NativeTaoEglBridge_nativeGetProcAddrFunctionPointer(
    JNIEnv *env, jclass clazz)
{
    (void) env; (void) clazz;
    if (!load_libs()) return 0;
    return (jlong) (uintptr_t) &nucleus_tao_egl_get_proc;
}
