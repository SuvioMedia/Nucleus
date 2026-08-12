/**
 * FP16/scRGB final scene presenter for the Windows Tao host.
 *
 * Skia records the complete Compose scene into an ANGLE pbuffer backed by an
 * R16G16B16A16_FLOAT D3D11 texture. A tiny D3D pass copies that texture into a
 * flip-model DirectComposition swapchain tagged RGB_FULL_G10_NONE_P709. The
 * pass applies Windows' current SDR-white multiplier, so a linear scene value
 * of 1.0 keeps the same apparent UI white while values above it retain HDR
 * headroom. No path through ANGLE's HWND swapchain participates in output.
 */

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dcomp.h>

#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>

#include "nucleus_tao_hdr_scene.h"

extern "C" void *nucleus_tao_host_egl_proc(const char *name);

typedef HRESULT (WINAPI *PFN_DCompositionCreateDevice)(IDXGIDevice *, REFIID, void **);
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
    LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

typedef void (APIENTRY *PFN_glFlush)(void);
typedef void (APIENTRY *PFN_glViewport)(int, int, int, int);

static PFNEGLCHOOSECONFIGPROC pEglChooseConfig = NULL;
static PFNEGLCREATECONTEXTPROC pEglCreateContext = NULL;
static PFNEGLDESTROYCONTEXTPROC pEglDestroyContext = NULL;
static PFNEGLCREATEPBUFFERFROMCLIENTBUFFERPROC pEglCreatePbufferFromClientBuffer = NULL;
static PFNEGLDESTROYSURFACEPROC pEglDestroySurface = NULL;
static PFNEGLMAKECURRENTPROC pEglMakeCurrent = NULL;
static PFNEGLQUERYDISPLAYATTRIBEXTPROC pEglQueryDisplayAttribEXT = NULL;
static PFNEGLQUERYDEVICEATTRIBEXTPROC pEglQueryDeviceAttribEXT = NULL;
static PFN_glFlush pglFlush = NULL;
static PFN_glViewport pglViewport = NULL;
static PFN_DCompositionCreateDevice pDCompositionCreateDevice = NULL;
static PFN_D3DCompile sD3DCompile = NULL;
static BOOL sResolved = FALSE;

static void resolveFunctions(void) {
    if (sResolved) return;
    sResolved = TRUE;
    pEglChooseConfig = (PFNEGLCHOOSECONFIGPROC)nucleus_tao_host_egl_proc("eglChooseConfig");
    pEglCreateContext = (PFNEGLCREATECONTEXTPROC)nucleus_tao_host_egl_proc("eglCreateContext");
    pEglDestroyContext = (PFNEGLDESTROYCONTEXTPROC)nucleus_tao_host_egl_proc("eglDestroyContext");
    pEglCreatePbufferFromClientBuffer = (PFNEGLCREATEPBUFFERFROMCLIENTBUFFERPROC)
        nucleus_tao_host_egl_proc("eglCreatePbufferFromClientBuffer");
    pEglDestroySurface = (PFNEGLDESTROYSURFACEPROC)nucleus_tao_host_egl_proc("eglDestroySurface");
    pEglMakeCurrent = (PFNEGLMAKECURRENTPROC)nucleus_tao_host_egl_proc("eglMakeCurrent");
    pEglQueryDisplayAttribEXT = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)
        nucleus_tao_host_egl_proc("eglQueryDisplayAttribEXT");
    pEglQueryDeviceAttribEXT = (PFNEGLQUERYDEVICEATTRIBEXTPROC)
        nucleus_tao_host_egl_proc("eglQueryDeviceAttribEXT");
    pglFlush = (PFN_glFlush)nucleus_tao_host_egl_proc("glFlush");
    pglViewport = (PFN_glViewport)nucleus_tao_host_egl_proc("glViewport");

    HMODULE dcomp = LoadLibraryW(L"dcomp.dll");
    if (dcomp) {
        pDCompositionCreateDevice = (PFN_DCompositionCreateDevice)
            GetProcAddress(dcomp, "DCompositionCreateDevice");
    }
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler) sD3DCompile = (PFN_D3DCompile)GetProcAddress(compiler, "D3DCompile");
}

struct SceneConstants {
    float whiteScale;
    float sourceHeight;
    float padding[2];
};

struct NucleusHdrScene {
    HWND hwnd;
    int widthPx;
    int heightPx;

    EGLDisplay dpy;
    EGLConfig config;
    EGLContext context;
    EGLSurface pbuffer;

    ID3D11Device *device;
    ID3D11DeviceContext *immediate;
    ID3D11Texture2D *sceneTexture;
    ID3D11ShaderResourceView *sceneSrv;
    ID3D11VertexShader *vertexShader;
    ID3D11PixelShader *pixelShader;
    ID3D11Buffer *constants;

    IDXGISwapChain1 *swapChain;
    HANDLE frameLatencyWaitable;
    BOOL vsyncEnabled;
    IDCompositionDevice *dcompDevice;
    IDCompositionTarget *dcompTarget;
    IDCompositionVisual *dcompVisual;

    HMONITOR monitor;
    BOOL hdrOutput;
    float sdrWhiteNits;
    float maximumLuminanceNits;
    float headroom;
    LUID adapterLuid;
    long long generation;
    long long presentedFrames;
};

static const char kSceneShader[] =
    "cbuffer SceneConstants : register(b0) {"
    "  float whiteScale; float sourceHeight; float2 padding;"
    "};"
    "Texture2D<float4> sceneTexture : register(t0);"
    "struct VsOut { float4 position : SV_Position; };"
    "VsOut VS(uint id : SV_VertexID) {"
    "  VsOut o;"
    "  float2 p = float2((id << 1) & 2, id & 2);"
    "  o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);"
    "  return o;"
    "}"
    "float4 PS(VsOut input) : SV_Target {"
    "  int2 p = int2(input.position.xy);"
    "  int sourceY = max(0, (int)sourceHeight - 1 - p.y);"
    "  float4 c = sceneTexture.Load(int3(p.x, sourceY, 0));"
    "  c.rgb *= whiteScale;"
    "  return c;"
    "}";

static void releaseSceneTarget(NucleusHdrScene *s) {
    if (s->pbuffer != EGL_NO_SURFACE) {
        pEglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        pEglDestroySurface(s->dpy, s->pbuffer);
        s->pbuffer = EGL_NO_SURFACE;
    }
    if (s->sceneSrv) { s->sceneSrv->Release(); s->sceneSrv = NULL; }
    if (s->sceneTexture) { s->sceneTexture->Release(); s->sceneTexture = NULL; }
}

static BOOL createSceneTarget(NucleusHdrScene *s, int widthPx, int heightPx) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)widthPx;
    desc.Height = (UINT)heightPx;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(s->device->CreateTexture2D(&desc, NULL, &s->sceneTexture))) return FALSE;
    if (FAILED(s->device->CreateShaderResourceView(s->sceneTexture, NULL, &s->sceneSrv))) {
        releaseSceneTarget(s);
        return FALSE;
    }
    const EGLint attributes[] = {
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_NONE
    };
    s->pbuffer = pEglCreatePbufferFromClientBuffer(
        s->dpy,
        EGL_D3D_TEXTURE_ANGLE,
        (EGLClientBuffer)s->sceneTexture,
        s->config,
        attributes);
    if (s->pbuffer == EGL_NO_SURFACE) {
        releaseSceneTarget(s);
        return FALSE;
    }
    if (!pEglMakeCurrent(s->dpy, s->pbuffer, s->pbuffer, s->context)) {
        releaseSceneTarget(s);
        return FALSE;
    }
    if (pglViewport) pglViewport(0, 0, widthPx, heightPx);
    return TRUE;
}

static BOOL compileShaders(NucleusHdrScene *s) {
    if (!sD3DCompile) return FALSE;
    ID3DBlob *vs = NULL;
    ID3DBlob *ps = NULL;
    ID3DBlob *errors = NULL;
    HRESULT hr = sD3DCompile(
        kSceneShader,
        sizeof(kSceneShader) - 1,
        "NucleusHdrScene",
        NULL,
        NULL,
        "VS",
        "vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &vs,
        &errors);
    if (errors) { errors->Release(); errors = NULL; }
    if (FAILED(hr) || !vs) return FALSE;
    hr = sD3DCompile(
        kSceneShader,
        sizeof(kSceneShader) - 1,
        "NucleusHdrScene",
        NULL,
        NULL,
        "PS",
        "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &ps,
        &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || !ps) {
        vs->Release();
        return FALSE;
    }
    hr = s->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), NULL, &s->vertexShader);
    if (SUCCEEDED(hr)) {
        hr = s->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), NULL, &s->pixelShader);
    }
    vs->Release();
    ps->Release();
    if (FAILED(hr)) return FALSE;

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(SceneConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(s->device->CreateBuffer(&cb, NULL, &s->constants));
}

static BOOL isHdrColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
           colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

static float querySdrWhiteNits(const wchar_t *gdiDeviceName) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return 80.0f;
    }
    DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DISPLAYCONFIG_PATH_INFO) * pathCount);
    DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DISPLAYCONFIG_MODE_INFO) * modeCount);
    if (!paths || !modes) {
        if (paths) HeapFree(GetProcessHeap(), 0, paths);
        if (modes) HeapFree(GetProcessHeap(), 0, modes);
        return 80.0f;
    }
    LONG status = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &pathCount, paths, &modeCount, modes, NULL);
    float result = 80.0f;
    if (status == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < pathCount; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
            if (lstrcmpiW(source.viewGdiDeviceName, gdiDeviceName) != 0) continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = paths[i].targetInfo.adapterId;
            white.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
                result = 80.0f * (1.0f + ((float)white.SDRWhiteLevel / 1000.0f));
            }
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, modes);
    HeapFree(GetProcessHeap(), 0, paths);
    return result;
}

static void refreshOutput(NucleusHdrScene *s, BOOL force) {
    HMONITOR monitor = MonitorFromWindow(s->hwnd, MONITOR_DEFAULTTONEAREST);
    BOOL hdr = FALSE;
    float maximum = 80.0f;
    float white = 80.0f;
    wchar_t outputName[32] = {};

    IDXGIDevice *dxgiDevice = NULL;
    IDXGIAdapter *adapter = NULL;
    if (SUCCEEDED(s->device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC adapterDesc = {};
        if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) s->adapterLuid = adapterDesc.AdapterLuid;
        for (UINT index = 0;; ++index) {
            IDXGIOutput *output = NULL;
            if (adapter->EnumOutputs(index, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC outputDesc = {};
            output->GetDesc(&outputDesc);
            if (outputDesc.Monitor == monitor) {
                lstrcpynW(outputName, outputDesc.DeviceName, 32);
                IDXGIOutput6 *output6 = NULL;
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void **)&output6))) {
                    DXGI_OUTPUT_DESC1 desc1 = {};
                    if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                        hdr = isHdrColorSpace(desc1.ColorSpace);
                        if (desc1.MaxLuminance > 0.0f) maximum = desc1.MaxLuminance;
                    }
                    output6->Release();
                }
                output->Release();
                break;
            }
            output->Release();
        }
    }
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    if (outputName[0]) white = querySdrWhiteNits(outputName);
    if (maximum < white) maximum = white;
    float headroom = maximum / white;
    if (headroom < 1.0f) headroom = 1.0f;

    BOOL changed = force || monitor != s->monitor || hdr != s->hdrOutput ||
        white != s->sdrWhiteNits || maximum != s->maximumLuminanceNits;
    s->monitor = monitor;
    s->hdrOutput = hdr;
    s->sdrWhiteNits = white;
    s->maximumLuminanceNits = maximum;
    s->headroom = headroom;
    if (changed) {
        ++s->generation;
        s->presentedFrames = 0;
    }
}

static BOOL createSwapChain(NucleusHdrScene *s) {
    IDXGIDevice *dxgiDevice = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    BOOL ok = FALSE;
    do {
        if (FAILED(s->device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice))) break;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) break;
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory))) break;

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = (UINT)s->widthPx;
        desc.Height = (UINT)s->heightPx;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        HRESULT hr = factory->CreateSwapChainForComposition(s->device, &desc, NULL, &s->swapChain);
        if (FAILED(hr)) {
            desc.Flags = 0;
            if (FAILED(factory->CreateSwapChainForComposition(s->device, &desc, NULL, &s->swapChain))) break;
        }

        IDXGISwapChain3 *swapChain3 = NULL;
        if (SUCCEEDED(s->swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&swapChain3))) {
            UINT support = 0;
            if (SUCCEEDED(swapChain3->CheckColorSpaceSupport(
                    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            }
            swapChain3->Release();
        }
        IDXGISwapChain2 *swapChain2 = NULL;
        if (SUCCEEDED(s->swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void **)&swapChain2))) {
            swapChain2->SetMaximumFrameLatency(1);
            s->frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
            swapChain2->Release();
        }

        if (!pDCompositionCreateDevice) break;
        if (FAILED(pDCompositionCreateDevice(
                dxgiDevice, __uuidof(IDCompositionDevice), (void **)&s->dcompDevice))) break;
        if (FAILED(s->dcompDevice->CreateTargetForHwnd(s->hwnd, FALSE, &s->dcompTarget))) break;
        if (FAILED(s->dcompDevice->CreateVisual(&s->dcompVisual))) break;
        if (FAILED(s->dcompVisual->SetContent(s->swapChain))) break;
        if (FAILED(s->dcompTarget->SetRoot(s->dcompVisual))) break;
        if (FAILED(s->dcompDevice->Commit())) break;
        ok = TRUE;
    } while (0);
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    return ok;
}

extern "C" NucleusHdrScene *nucleus_tao_hdr_scene_create(
    HWND hwnd, void *eglDisplay, void *shareContext, int widthPx, int heightPx)
{
    resolveFunctions();
    if (!hwnd || !pEglChooseConfig || !pEglCreateContext || !pEglDestroyContext ||
        !pEglCreatePbufferFromClientBuffer || !pEglDestroySurface || !pEglMakeCurrent ||
        !pEglQueryDisplayAttribEXT || !pEglQueryDeviceAttribEXT || !pDCompositionCreateDevice ||
        !sD3DCompile) {
        return NULL;
    }
    if (widthPx < 1) widthPx = 1;
    if (heightPx < 1) heightPx = 1;
    EGLDisplay dpy = (EGLDisplay)eglDisplay;
    EGLContext shared = (EGLContext)shareContext;
    if (dpy == EGL_NO_DISPLAY || shared == EGL_NO_CONTEXT) return NULL;

    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 16,
        EGL_GREEN_SIZE, 16,
        EGL_BLUE_SIZE, 16,
        EGL_ALPHA_SIZE, 16,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 8,
        EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT,
        EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint configCount = 0;
    if (!pEglChooseConfig(dpy, configAttributes, &config, 1, &configCount) || configCount < 1) {
        return NULL;
    }
    const EGLint context3[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    const EGLint context2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = pEglCreateContext(dpy, config, shared, context3);
    if (context == EGL_NO_CONTEXT) context = pEglCreateContext(dpy, config, shared, context2);
    if (context == EGL_NO_CONTEXT) return NULL;

    EGLAttrib deviceAttribute = 0;
    EGLAttrib d3dAttribute = 0;
    if (!pEglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &deviceAttribute) || !deviceAttribute ||
        !pEglQueryDeviceAttribEXT(
            (EGLDeviceEXT)deviceAttribute, EGL_D3D11_DEVICE_ANGLE, &d3dAttribute) ||
        !d3dAttribute) {
        pEglDestroyContext(dpy, context);
        return NULL;
    }

    NucleusHdrScene *s = (NucleusHdrScene *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NucleusHdrScene));
    if (!s) {
        pEglDestroyContext(dpy, context);
        return NULL;
    }
    s->hwnd = hwnd;
    s->widthPx = widthPx;
    s->heightPx = heightPx;
    s->dpy = dpy;
    s->config = config;
    s->context = context;
    s->pbuffer = EGL_NO_SURFACE;
    s->device = (ID3D11Device *)d3dAttribute;
    s->device->AddRef();
    s->device->GetImmediateContext(&s->immediate);
    s->sdrWhiteNits = 80.0f;
    s->maximumLuminanceNits = 80.0f;
    s->headroom = 1.0f;
    s->vsyncEnabled = TRUE;

    if (!compileShaders(s) || !createSceneTarget(s, widthPx, heightPx) || !createSwapChain(s)) {
        nucleus_tao_hdr_scene_destroy(s);
        return NULL;
    }
    refreshOutput(s, TRUE);
    return s;
}

extern "C" void nucleus_tao_hdr_scene_destroy(NucleusHdrScene *s) {
    if (!s) return;
    releaseSceneTarget(s);
    if (s->dcompVisual) { s->dcompVisual->SetContent(NULL); s->dcompVisual->Release(); }
    if (s->dcompTarget) { s->dcompTarget->SetRoot(NULL); s->dcompTarget->Release(); }
    if (s->swapChain) s->swapChain->Release();
    if (s->dcompDevice) { s->dcompDevice->Commit(); s->dcompDevice->Release(); }
    if (s->constants) s->constants->Release();
    if (s->pixelShader) s->pixelShader->Release();
    if (s->vertexShader) s->vertexShader->Release();
    if (s->immediate) s->immediate->Release();
    if (s->device) s->device->Release();
    if (s->context != EGL_NO_CONTEXT) pEglDestroyContext(s->dpy, s->context);
    HeapFree(GetProcessHeap(), 0, s);
}

extern "C" BOOL nucleus_tao_hdr_scene_make_current(NucleusHdrScene *s) {
    if (!s || s->pbuffer == EGL_NO_SURFACE) return FALSE;
    return pEglMakeCurrent(s->dpy, s->pbuffer, s->pbuffer, s->context) ? TRUE : FALSE;
}

extern "C" BOOL nucleus_tao_hdr_scene_resize(NucleusHdrScene *s, int widthPx, int heightPx) {
    if (!s || !s->swapChain) return FALSE;
    if (widthPx < 1) widthPx = 1;
    if (heightPx < 1) heightPx = 1;
    if (widthPx == s->widthPx && heightPx == s->heightPx) return nucleus_tao_hdr_scene_make_current(s);
    releaseSceneTarget(s);
    HRESULT hr = s->swapChain->ResizeBuffers(
        3,
        (UINT)widthPx,
        (UINT)heightPx,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        s->frameLatencyWaitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
    if (FAILED(hr)) return FALSE;
    s->widthPx = widthPx;
    s->heightPx = heightPx;
    refreshOutput(s, TRUE);
    return createSceneTarget(s, widthPx, heightPx);
}

extern "C" BOOL nucleus_tao_hdr_scene_present(NucleusHdrScene *s) {
    if (!s || !s->sceneTexture || !s->sceneSrv || !s->swapChain) return FALSE;
    refreshOutput(s, FALSE);
    if (pglFlush) pglFlush();
    if (s->vsyncEnabled && s->frameLatencyWaitable) {
        WaitForSingleObjectEx(s->frameLatencyWaitable, 1000, TRUE);
    }

    ID3D11Texture2D *backBuffer = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    HRESULT hr = s->swapChain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), (void **)&backBuffer);
    if (SUCCEEDED(hr)) hr = s->device->CreateRenderTargetView(backBuffer, NULL, &rtv);
    if (FAILED(hr) || !rtv) {
        if (backBuffer) backBuffer->Release();
        return FALSE;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = s->immediate->Map(s->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        SceneConstants *constants = (SceneConstants *)mapped.pData;
        constants->whiteScale = s->sdrWhiteNits / 80.0f;
        constants->sourceHeight = (float)s->heightPx;
        constants->padding[0] = 0.0f;
        constants->padding[1] = 0.0f;
        s->immediate->Unmap(s->constants, 0);
    }

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)s->widthPx;
    viewport.Height = (FLOAT)s->heightPx;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    s->immediate->RSSetViewports(1, &viewport);
    s->immediate->OMSetRenderTargets(1, &rtv, NULL);
    s->immediate->IASetInputLayout(NULL);
    s->immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s->immediate->VSSetShader(s->vertexShader, NULL, 0);
    s->immediate->PSSetShader(s->pixelShader, NULL, 0);
    s->immediate->PSSetConstantBuffers(0, 1, &s->constants);
    s->immediate->PSSetShaderResources(0, 1, &s->sceneSrv);
    s->immediate->Draw(3, 0);
    ID3D11ShaderResourceView *nullSrv = NULL;
    s->immediate->PSSetShaderResources(0, 1, &nullSrv);
    s->immediate->OMSetRenderTargets(0, NULL, NULL);
    rtv->Release();
    backBuffer->Release();

    hr = s->swapChain->Present(0, 0);
    if (SUCCEEDED(hr)) {
        ++s->presentedFrames;
        return TRUE;
    }
    return FALSE;
}

extern "C" void nucleus_tao_hdr_scene_set_vsync_enabled(NucleusHdrScene *s, BOOL enabled) {
    if (s) s->vsyncEnabled = enabled;
}

extern "C" void *nucleus_tao_hdr_scene_egl_surface(NucleusHdrScene *s) {
    return s ? (void *)s->pbuffer : NULL;
}
extern "C" void *nucleus_tao_hdr_scene_egl_context(NucleusHdrScene *s) {
    return s ? (void *)s->context : NULL;
}
extern "C" void *nucleus_tao_hdr_scene_egl_config(NucleusHdrScene *s) {
    return s ? (void *)s->config : NULL;
}
extern "C" BOOL nucleus_tao_hdr_scene_is_hdr_output(NucleusHdrScene *s) {
    return s ? s->hdrOutput : FALSE;
}
extern "C" float nucleus_tao_hdr_scene_sdr_white_nits(NucleusHdrScene *s) {
    return s ? s->sdrWhiteNits : 80.0f;
}
extern "C" float nucleus_tao_hdr_scene_max_luminance_nits(NucleusHdrScene *s) {
    return s ? s->maximumLuminanceNits : 80.0f;
}
extern "C" float nucleus_tao_hdr_scene_headroom(NucleusHdrScene *s) {
    return s ? s->headroom : 1.0f;
}
extern "C" long long nucleus_tao_hdr_scene_generation(NucleusHdrScene *s) {
    return s ? s->generation : 0;
}
extern "C" long long nucleus_tao_hdr_scene_presented_frames(NucleusHdrScene *s) {
    return s ? s->presentedFrames : 0;
}
extern "C" long long nucleus_tao_hdr_scene_adapter_luid(NucleusHdrScene *s) {
    if (!s) return 0;
    return ((long long)(LONG)s->adapterLuid.HighPart << 32) | (unsigned long)s->adapterLuid.LowPart;
}
