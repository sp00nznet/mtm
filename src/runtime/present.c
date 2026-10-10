/*
 * The presenter: the game's 640x480 picture in its window, scaled on the GPU.
 * docs/presenter.md has the options and the reasoning.
 *
 * Hover!'s presenter (itself gunman's), with SimCity 2000's Scale2x and glow
 * added. One difference from Hover!: there the game fought any resize of its
 * frame, so the presenter needed a window of its own. Here the host already
 * owns the game's window (ddraw.c turns its fullscreen popup into an
 * ordinary window), so the presenter draws straight into it, and only the
 * mouse needs mapping back to 640x480.
 *
 * The game thread hands over each picture (present_frame) and goes on; this
 * module's own thread uploads it and draws with Direct3D 11 (WARP without a
 * GPU, one GDI StretchDIBits without Direct3D at all). It never runs guest
 * code and never takes the machine lock.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"

#define VW 640
#define VH 480

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

/* Shader modes, in menu order. */
enum { F_SHARP, F_SMOOTH, F_NEAREST, F_INTEGER, F_SCALE2X, F_COUNT };
static const char* const k_filters[F_COUNT] = { "sharp", "smooth", "nearest", "integer", "scale2x" };
enum { D_OFF, D_16BIT, D_8BIT, D_COUNT };
static const char* const k_dithers[D_COUNT] = { "off", "16bit", "8bit" };

static const char k_hlsl[] =
"Texture2D t0 : register(t0);\n"
"SamplerState s_lin : register(s0);\n"
"SamplerState s_pt : register(s1);\n"
"cbuffer C : register(b0) { float2 src; float2 dst; int mode; int crt; int curve; int dither;\n"
"                           int look; int glow; int2 pad; };\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) {\n"
"  V o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
"}\n"
"float3 sharp(float2 uv) {\n"                        /* sharp-bilinear (gunman) */
"  float2 scale = max(dst / src, 1.0);\n"
"  float2 texel = uv * src, fl = floor(texel), c = frac(texel) - 0.5;\n"
"  float2 range = 0.5 - 0.5 / scale;\n"
"  float2 f = (c - clamp(c, -range, range)) * scale + 0.5;\n"
"  return t0.Sample(s_lin, (fl + f) / src).rgb;\n"
"}\n"
"float3 at(float2 p) { return t0.Load(int3(clamp(p, 0, src - 1), 0)).rgb; }\n"
"bool same(float3 a, float3 b) { return all(abs(a - b) < 0.004); }\n"
"float3 scale2x(float2 uv) {\n"                      /* EPX (SimCity 2000): rounds diagonal staircases */
"  float2 tex = uv * src, p = floor(tex), s = frac(tex);\n"
"  float3 P = at(p), A = at(p + float2(0, -1)), B = at(p + float2(1, 0)),\n"
"         C = at(p + float2(-1, 0)), D = at(p + float2(0, 1));\n"
"  if (s.y < 0.5) {\n"
"    if (s.x < 0.5) return (same(C, A) && !same(C, D) && !same(A, B)) ? A : P;\n"
"    return (same(A, B) && !same(A, C) && !same(B, D)) ? B : P;\n"
"  }\n"
"  if (s.x < 0.5) return (same(D, C) && !same(D, B) && !same(C, A)) ? C : P;\n"
"  return (same(B, D) && !same(B, A) && !same(D, C)) ? D : P;\n"
"}\n"
"float3 base(float2 uv) {\n"
"  if (mode == 1) return t0.Sample(s_lin, uv).rgb;\n"
"  if (mode == 4) return scale2x(uv);\n"
"  if (mode >= 2) return t0.Sample(s_pt, uv).rgb;\n"
"  return sharp(uv);\n"
"}\n"
"float3 bright(float2 uv) {\n"                       /* only bright, saturated light glows */
"  float3 c = t0.SampleLevel(s_lin, uv, 0).rgb;\n"
"  float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b));\n"
"  return c * saturate((hi - 0.55) * 3.0) * saturate((hi - lo) * 3.0 + 0.2);\n"
"}\n"
"float3 bloom(float2 uv) {\n"
"  float3 g = 0; float w = 0;\n"
"  [unroll] for (int r = 1; r <= 3; r++)\n"
"    [unroll] for (int k = 0; k < 8; k++) {\n"
"      float a = k * 0.785398 + r * 0.4, wt = 1.0 / r;\n"
"      g += bright(uv + float2(cos(a), sin(a)) * r * 1.6 / src) * wt; w += wt;\n"
"    }\n"
"  return g / w;\n"
"}\n"
"static const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
"float4 ps(V i) : SV_Target {\n"
"  float2 uv = i.uv;\n"
"  if (crt && curve) {\n"                             /* a gentle barrel, as on a 14-inch tube */
"    float2 c = uv * 2 - 1; c *= 1 + (c.yx * c.yx) * float2(0.035, 0.05); uv = c * 0.5 + 0.5;\n"
"    if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);\n"
"  }\n"
"  float3 c = base(uv);\n"
"  if (glow) c += bloom(uv) * 0.9;\n"
"  if (dither) {\n"                                   /* ordered, per game pixel */
"    int2 p = int2(uv * src) & 3;\n"
"    float3 lv = dither == 1 ? float3(31, 63, 31) : float3(5, 5, 5);\n"
"    c = floor(c * lv + bayer[p.y * 4 + p.x] / 16.0) / lv;\n"
"  }\n"
"  if (crt) {\n"
"    /* Scanlines need a few screen rows per game row; below ~3x they beat\n"
"     * against the pixel grid, so they fade in from 1.5x to 3x (gunman). */\n"
"    float k = saturate((dst.y / src.y - 1.5) / 1.5);\n"
"    float d = frac(uv.y * src.y) - 0.5;\n"
"    c *= lerp(1.0, exp(-d * d * 8.0) * 1.6, k);\n"
"    uint m = (uint)i.pos.x % 3u;\n"
"    float3 mask = m == 0u ? float3(1.25, 0.87, 0.87) : m == 1u ? float3(0.87, 1.25, 0.87)\n"
"                                                     : float3(0.87, 0.87, 1.25);\n"
"    c *= lerp(float3(1, 1, 1), mask, k);\n"
"    float2 v = uv * (1 - uv.yx); c *= pow(saturate(v.x * v.y * 18.0), 0.2);\n"   /* vignette */
"  }\n"
"  if (look) {\n"                                     /* vivid: saturation, a little contrast */
"    float l = dot(c, float3(0.299, 0.587, 0.114));\n"
"    c = lerp(float3(l, l, l), c, 1.3);\n"
"    c = saturate((c - 0.5) * 1.08 + 0.5);\n"
"  }\n"
"  return float4(saturate(c), 1);\n"
"}\n";

/* ------------------------------------------------------------ state */

static char   g_ini[MAX_PATH];
static int    g_filter = F_SHARP, g_crt, g_curve = 1, g_dither, g_vivid, g_glow, g_scale, g_full;
static int    g_pause_bg = 1;
static HWND   g_wnd;
static HMENU  g_menu;
static HANDLE g_ready;                   /* auto-reset: a picture (or a resize) to show */
static CRITICAL_SECTION g_lock;          /* g_pic, against the game thread */
static uint32_t g_pic[VW * VH], g_copy[VW * VH];
static int    g_have;
static LONG   g_saved_style;
static RECT   g_saved_rect;

static struct {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    IDXGISwapChain* sc;
    ID3D11RenderTargetView* rtv;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11SamplerState* samp[2];
    ID3D11Buffer* cb;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    int cw, ch, failed;
} d;

/* ------------------------------------------------------------ settings */

static int pick(const char* key, const char* const* names, int n, int dflt) {
    char b[32];
    GetPrivateProfileStringA("video", key, names[dflt], b, sizeof b, g_ini);
    for (int i = 0; i < n; i++)
        if (!_stricmp(b, names[i])) return i;
    return dflt;
}
static void put(const char* key, const char* v) { WritePrivateProfileStringA("video", key, v, g_ini); }
static void put_int(const char* key, int v) { char b[16]; _snprintf(b, sizeof b, "%d", v); put(key, b); }

static void load(void) {
    char b[8];
    if (!GetPrivateProfileStringA("video", "filter", "", b, sizeof b, g_ini)) {   /* first run: the defaults, written */
        put("filter", "sharp");
        put_int("scale", 0);
        put_int("fullscreen", 0);
        put_int("crt", 0);
        put_int("curvature", 1);
        put_int("glow", 0);
        put("dither", "off");
        put_int("vivid", 0);
        put_int("pause_in_background", 1);
    }
    g_filter = pick("filter", k_filters, F_COUNT, F_SHARP);
    g_dither = pick("dither", k_dithers, D_COUNT, D_OFF);
    g_scale = GetPrivateProfileIntA("video", "scale", 0, g_ini);
    g_crt = GetPrivateProfileIntA("video", "crt", 0, g_ini);
    g_curve = GetPrivateProfileIntA("video", "curvature", 1, g_ini);
    g_glow = GetPrivateProfileIntA("video", "glow", 0, g_ini);
    g_vivid = GetPrivateProfileIntA("video", "vivid", 0, g_ini);
    g_pause_bg = GetPrivateProfileIntA("video", "pause_in_background", 1, g_ini);
    if (g_scale < 0 || g_scale > 8) g_scale = 0;
    if (g_ready) SetEvent(g_ready);
}

void present_init(const char* ini, int scale) {
    strncpy(g_ini, ini, sizeof g_ini - 1);
    load();
    if (scale > 0) g_scale = scale;               /* --scale: this run only */
}

int present_pause_in_background(void) { return g_pause_bg; }

/* ------------------------------------------------------------ Direct3D */

static int d3d_fail(const char* what, HRESULT hr) {
    fprintf(stderr, "[present] Direct3D 11 unavailable (%s, 0x%08lX): drawing with GDI\n",
            what, (unsigned long)hr);
    d.failed = 1;
    return 0;
}

static int d3d_init(void) {
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                                               D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
    if (FAILED(hr))     /* no GPU (an RDP session without one): WARP is still a GPU pipeline */
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1,
                                           D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
    if (FAILED(hr)) return d3d_fail("device", hr);
    IDXGIFactory* f = NULL;             /* Alt+Enter is ours (borderless), not DXGI's */
    if (SUCCEEDED(IDXGISwapChain_GetParent(d.sc, &IID_IDXGIFactory, (void**)&f))) {
        IDXGIFactory_MakeWindowAssociation(f, g_wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        IDXGIFactory_Release(f);
    }
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn compile = dc ? (compile_fn)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) return d3d_fail("d3dcompiler_47.dll", 0);
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr))
        hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "[present] %s\n", (const char*)ID3D10Blob_GetBufferPointer(err));
        return d3d_fail("shader", hr);
    }
    ID3D11Device_CreateVertexShader(d.dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &d.vs);
    ID3D11Device_CreatePixelShader(d.dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &d.ps);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);
    D3D11_SAMPLER_DESC s = {0};
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[0]);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[1]);
    D3D11_BUFFER_DESC b = {0};
    b.ByteWidth = 48;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(d.dev, &b, NULL, &d.cb);
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = VW; td.Height = VH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8X8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(d.dev, &td, NULL, &d.tex))) return d3d_fail("texture", 0);
    ID3D11Device_CreateShaderResourceView(d.dev, (ID3D11Resource*)d.tex, NULL, &d.srv);
    fprintf(stderr, "[present] Direct3D 11 presenter\n");
    return 1;
}

/* Where the picture goes in a cw x ch client: 4:3 kept, letterboxed. */
static RECT fit(int cw, int ch) {
    RECT r;
    int dw, dh;
    if (g_filter == F_INTEGER) {
        int k = max(1, min(cw / VW, ch / VH));
        dw = VW * k;
        dh = VH * k;
    } else if ((long long)cw * VH > (long long)ch * VW) {
        dh = ch;
        dw = (int)((long long)ch * VW / VH);
    } else {
        dw = cw;
        dh = (int)((long long)cw * VH / VW);
    }
    r.left = (cw - dw) / 2;
    r.top = (ch - dh) / 2;
    r.right = r.left + dw;
    r.bottom = r.top + dh;
    return r;
}

static void draw(void) {
    RECT cr;
    GetClientRect(g_wnd, &cr);
    int cw = cr.right, ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    EnterCriticalSection(&g_lock);
    int have = g_have;
    if (have) memcpy(g_copy, g_pic, sizeof g_copy);
    LeaveCriticalSection(&g_lock);
    if (!have) return;
    RECT r = fit(cw, ch);

    if (d.failed || (!d.dev && !d3d_init())) {          /* GDI: one StretchDIBits */
        HDC dc = GetDC(g_wnd);
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), VW, -VH, 1, 32, BI_RGB } };
        SetStretchBltMode(dc, g_filter == F_SMOOTH || g_filter == F_SHARP ? HALFTONE : COLORONCOLOR);
        StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, VW, VH, g_copy, &bi,
                      DIB_RGB_COLORS, SRCCOPY);
        ExcludeClipRect(dc, r.left, r.top, r.right, r.bottom);
        FillRect(dc, &cr, (HBRUSH)GetStockObject(BLACK_BRUSH));
        ReleaseDC(g_wnd, dc);
        return;
    }
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.tex, 0, NULL, g_copy, VW * 4, 0);
    if (!d.rtv || cw != d.cw || ch != d.ch) {
        if (d.rtv) { ID3D11RenderTargetView_Release(d.rtv); d.rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(d.sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return;
        ID3D11Texture2D* back = NULL;
        IDXGISwapChain_GetBuffer(d.sc, 0, &IID_ID3D11Texture2D, (void**)&back);
        ID3D11Device_CreateRenderTargetView(d.dev, (ID3D11Resource*)back, NULL, &d.rtv);
        ID3D11Texture2D_Release(back);
        d.cw = cw;
        d.ch = ch;
    }
    struct { float src[2], dst[2]; int mode, crt, curve, dither, look, glow, pad[2]; } c = {
        { (float)VW, (float)VH }, { (float)(r.right - r.left), (float)(r.bottom - r.top) },
        g_filter, g_crt, g_curve, g_dither, g_vivid, g_glow, { 0 } };
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.cb, 0, NULL, &c, 0, 0);
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(d.ctx, d.rtv, black);
    D3D11_VIEWPORT vp = { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), 0, 1 };
    ID3D11DeviceContext_RSSetViewports(d.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 1, &d.rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(d.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(d.ctx, d.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d.ctx, d.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d.ctx, 0, 1, &d.srv);
    ID3D11DeviceContext_PSSetSamplers(d.ctx, 0, 2, d.samp);
    ID3D11DeviceContext_PSSetConstantBuffers(d.ctx, 0, 1, &d.cb);
    ID3D11DeviceContext_Draw(d.ctx, 3, 0);
    IDXGISwapChain_Present(d.sc, 1, 0);
}

static DWORD WINAPI thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_ready, 250);    /* and redraw now and then: a window uncovered */
        draw();
    }
}

void present_frame(const uint32_t* bgra) {
    if (!g_ready) return;
    EnterCriticalSection(&g_lock);
    memcpy(g_pic, bgra, sizeof g_pic);
    g_have = 1;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_ready);
}

/* ------------------------------------------------------------ the window */

/* Everything below runs on the game's thread, inside its window procedure. */

static void size_to(int scale) {
    MONITORINFO mi = { sizeof mi };
    double k = scale;
    if (GetMonitorInfoA(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        /* At most 95% x 90% of the work area (Hover!'s rule): scale 0 fills
         * that, fractionally, which sharp-bilinear takes well; a larger
         * chosen scale is cut down to it. */
        double fw = (mi.rcWork.right - mi.rcWork.left) * 0.95 / VW, fh = (mi.rcWork.bottom - mi.rcWork.top) * 0.90 / VH;
        double most = fw < fh ? fw : fh;
        if (k < 1 || k > most) k = most;
    }
    if (k < 1) k = 1;
    DWORD style = GetWindowLongA(g_wnd, GWL_STYLE);
    RECT r = { 0, 0, (int)(VW * k), (int)(VH * k) };
    AdjustWindowRect(&r, style, g_menu != NULL);
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(g_wnd, NULL, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
}

static void set_fullscreen(int on) {
    if (on == g_full) return;
    g_full = on;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        g_saved_style = GetWindowLongA(g_wnd, GWL_STYLE);
        GetWindowRect(g_wnd, &g_saved_rect);
        GetMonitorInfoA(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(g_wnd, NULL);
        SetWindowLongA(g_wnd, GWL_STYLE, (g_saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongA(g_wnd, GWL_STYLE, g_saved_style);
        SetMenu(g_wnd, g_menu);
        SetWindowPos(g_wnd, NULL, g_saved_rect.left, g_saved_rect.top, g_saved_rect.right - g_saved_rect.left,
                     g_saved_rect.bottom - g_saved_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    put_int("fullscreen", on);
    SetEvent(g_ready);
}

/* ------------------------------------------------------------ menu */

enum { ID_FILTER = 0x6E00,  /* + F_* */
       ID_SCALE = 0x6E10,   /* + 1..4 */
       ID_DITHER = 0x6E20,  /* + D_* */
       ID_CRT = 0x6E30, ID_CURVE, ID_GLOW, ID_VIVID, ID_FULL, ID_PAUSE_BG };

static HMENU make_menu(void) {
    HMENU bar = CreateMenu(), v = CreatePopupMenu(), f = CreatePopupMenu(), s = CreatePopupMenu(),
          dm = CreatePopupMenu();
    AppendMenuA(f, MF_STRING, ID_FILTER + F_SHARP, "&Sharp (crisp pixels, smooth edges)");
    AppendMenuA(f, MF_STRING, ID_FILTER + F_SMOOTH, "S&mooth (bilinear)");
    AppendMenuA(f, MF_STRING, ID_FILTER + F_NEAREST, "&Nearest (raw pixels)");
    AppendMenuA(f, MF_STRING, ID_FILTER + F_INTEGER, "&Integer (whole multiples only)");
    AppendMenuA(f, MF_STRING, ID_FILTER + F_SCALE2X, "Scale&2x (rounded diagonals)");
    for (int i = 1; i <= 4; i++) {
        char t[32];
        _snprintf(t, sizeof t, "&%dx (%dx%d)", i, VW * i, VH * i);
        AppendMenuA(s, MF_STRING, ID_SCALE + i, t);
    }
    AppendMenuA(dm, MF_STRING, ID_DITHER + D_OFF, "&Off");
    AppendMenuA(dm, MF_STRING, ID_DITHER + D_16BIT, "&16-bit (High Color desktop)");
    AppendMenuA(dm, MF_STRING, ID_DITHER + D_8BIT, "&8-bit (216-colour web palette)");
    AppendMenuA(v, MF_POPUP, (UINT_PTR)f, "&Filter");
    AppendMenuA(v, MF_POPUP, (UINT_PTR)s, "Window &size");
    AppendMenuA(v, MF_STRING, ID_FULL, "F&ullscreen\tF11");
    AppendMenuA(v, MF_SEPARATOR, 0, NULL);
    AppendMenuA(v, MF_STRING, ID_CRT, "&CRT (scanlines, mask)");
    AppendMenuA(v, MF_STRING, ID_CURVE, "CRT c&urvature");
    AppendMenuA(v, MF_STRING, ID_GLOW, "&Glow (bright colours bloom)");
    AppendMenuA(v, MF_POPUP, (UINT_PTR)dm, "&Dithering");
    AppendMenuA(v, MF_STRING, ID_VIVID, "&Vivid colour");
    AppendMenuA(v, MF_SEPARATOR, 0, NULL);
    AppendMenuA(v, MF_STRING, ID_PAUSE_BG, "&Pause when in the background");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)v, "&Video");
    return bar;
}

static void update_menu(HMENU m) {
    for (int i = 0; i < F_COUNT; i++) CheckMenuItem(m, ID_FILTER + i, g_filter == i ? MF_CHECKED : MF_UNCHECKED);
    for (int i = 0; i < D_COUNT; i++) CheckMenuItem(m, ID_DITHER + i, g_dither == i ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, ID_CRT, g_crt ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, ID_CURVE, g_curve ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, ID_GLOW, g_glow ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, ID_VIVID, g_vivid ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, ID_FULL, g_full ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, ID_PAUSE_BG, g_pause_bg ? MF_CHECKED : MF_UNCHECKED);
}

static int command(UINT id) {
    if (id >= ID_FILTER && id < ID_FILTER + F_COUNT) put("filter", k_filters[id - ID_FILTER]);
    else if (id >= ID_DITHER && id < ID_DITHER + D_COUNT) put("dither", k_dithers[id - ID_DITHER]);
    else if (id >= ID_SCALE + 1 && id <= ID_SCALE + 4) {
        put_int("scale", (int)(id - ID_SCALE));
        set_fullscreen(0);
        size_to((int)(id - ID_SCALE));
    }
    else if (id == ID_CRT) put_int("crt", !g_crt);
    else if (id == ID_CURVE) put_int("curvature", !g_curve);
    else if (id == ID_GLOW) put_int("glow", !g_glow);
    else if (id == ID_VIVID) put_int("vivid", !g_vivid);
    else if (id == ID_PAUSE_BG) put_int("pause_in_background", !g_pause_bg);
    else if (id == ID_FULL) { set_fullscreen(!g_full); return 1; }
    else return 0;
    int scale = g_scale;
    load();
    if (id < ID_SCALE || id > ID_SCALE + 4) g_scale = scale;   /* keep a --scale */
    return 1;
}

void present_start(HWND game) {
    g_wnd = game;
    g_menu = make_menu();
    InitializeCriticalSection(&g_lock);
    g_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetWindowLongA(g_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetMenu(g_wnd, g_menu);
    size_to(g_scale);
    if (GetPrivateProfileIntA("video", "fullscreen", 0, g_ini)) set_fullscreen(1);
    CloseHandle(CreateThread(NULL, 0, thread, NULL, 0, NULL));
}

/* 640x480 -> client: where a scripted click has to land so that the
 * mapping below brings it back to the same game position. */
void present_unmap(int* x, int* y) {
    RECT cr, f;
    if (!g_ready || !GetClientRect(g_wnd, &cr)) return;
    f = fit(cr.right, cr.bottom);
    *x = f.left + (*x * (f.right - f.left) + (f.right - f.left) / 2) / VW;
    *y = f.top + (*y * (f.bottom - f.top) + (f.bottom - f.top) / 2) / VH;
}

int present_message(HWND h, UINT m, WPARAM w, LPARAM* l, LRESULT* r) {
    (void)h;
    if (!g_ready) return 0;
    *r = 0;
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) {
        RECT cr, f;
        GetClientRect(g_wnd, &cr);
        f = fit(cr.right, cr.bottom);
        int x = ((short)LOWORD(*l) - f.left) * VW / max(f.right - f.left, 1);
        int y = ((short)HIWORD(*l) - f.top) * VH / max(f.bottom - f.top, 1);
        *l = MAKELPARAM(max(0, min(VW - 1, x)), max(0, min(VH - 1, y)));
        return 0;                                   /* on to the game, mapped */
    }
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11) { set_fullscreen(!g_full); return 1; }
        if (w == VK_ESCAPE && g_full) { set_fullscreen(0); return 1; }
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN) { set_fullscreen(!g_full); return 1; }   /* Alt+Enter */
        return 0;
    case WM_INITMENUPOPUP:
        update_menu((HMENU)w);
        return 1;
    case WM_COMMAND:
        return HIWORD(w) == 0 && command(LOWORD(w));
    case WM_SIZE:
        SetEvent(g_ready);
        return 0;
    case WM_PAINT:
        ValidateRect(g_wnd, NULL);
        SetEvent(g_ready);
        return 1;
    case WM_ERASEBKGND:
        *r = 1;
        return 1;
    }
    return 0;
}
