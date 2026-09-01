// ngxhost -- a scriptable DLSS host, built to be a game as far as ReShade and the
// dlss5-bridge add-on are concerned, so that the transitions those two fail on can
// be reproduced in seconds instead of by launching Baldur's Gate 3 and playing to a
// particular state.
//
// PHASE 1. A real D3D11 DLSS host: a moving scene, correct motion vectors, sub-pixel
// jitter, a real NGX SuperSampling feature created and evaluated every frame. The
// add-on attaches to it with no cooperation, because from its side there is nothing
// to distinguish this from a game.
//
// Two rules that make it honest, both easy to lose:
//
//   D3D11 and DXGI are reached through the IMPORT TABLE and never through
//   LoadLibrary. A LoadLibrary of the system copy walks past ReShade's proxy and the
//   run would prove only that this program can clear a buffer.
//
//   The parameter block is Reset and fully repopulated every frame. If jitter were
//   set once and left, an "omit" in a later scenario would still find last frame's
//   value in the block, the bridge's absence handling would never run, and the test
//   would go green having exercised nothing.
//
//   ..\build.cmd

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>

#include "contract.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

// ---------------------------------------------------------------------------
// The scene. Deliberately not a renderer: one fullscreen triangle, a procedural
// pattern panned by a constant velocity, depth from a plane equation, and motion
// vectors that are correct BY CONSTRUCTION rather than by differencing matrices.
// A bridge fed zero motion vectors is not being tested the way a game tests it, and
// a scene elaborate enough to produce real ones would be the bulk of this program.
// ---------------------------------------------------------------------------
static const char kShader[] = R"(
cbuffer C : register(b0)
{
    float2 pan;        // pixels this frame, in render space
    float2 jitter;     // sub-pixel, already in NDC
    float2 inv_render;
    float2 mv_scale;   // what the contract declares, so MV is written in its units
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv  = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.pos.xy += jitter;
    return o;
}

float hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), f.x),
                lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), f.x), f.y);
}

struct PSOut { float4 color : SV_Target0; float2 mv : SV_Target1; float depth : SV_Depth; };

PSOut ps(VSOut i)
{
    PSOut o;
    // The pattern moves; the camera does not. High-frequency content on purpose --
    // a flat image would upscale identically however wrong the vectors were.
    float2 p = i.uv / inv_render + pan;
    float  chk = step(0.5, frac(floor(p.x / 24.0) * 0.5 + floor(p.y / 24.0) * 0.5));
    float  n   = noise(p * 0.08) * 0.6 + noise(p * 0.31) * 0.3;
    o.color = float4(saturate(float3(chk * 0.8 + n, n, 1.0 - chk * 0.6) * 1.4), 1);

    // Where this pixel WAS, minus where it is: a constant field, exactly -pan,
    // expressed in the units MV.Scale declares.
    o.mv = -pan * mv_scale * inv_render;

    // A tilted plane. Not reversed-Z, and the contract says so.
    o.depth = saturate(0.2 + 0.6 * i.uv.y);
    return o;
}
)";

struct CB { float pan[2]; float jitter[2]; float inv_render[2]; float mv_scale[2]; };

// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void NgxLog(const char *msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{ printf("  [ngx] %s", msg); }

struct Tex
{
    ID3D11Texture2D          *tex = nullptr;
    ID3D11RenderTargetView   *rtv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11UnorderedAccessView*uav = nullptr;
    ID3D11DepthStencilView   *dsv = nullptr;
    void Release()
    {
        if (dsv) { dsv->Release(); dsv = nullptr; }
        if (uav) { uav->Release(); uav = nullptr; }
        if (srv) { srv->Release(); srv = nullptr; }
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
    }
};

static bool MakeTex(ID3D11Device *dev, Tex *t, UINT w, UINT h, DXGI_FORMAT fmt,
                    UINT bind, DXGI_FORMAT srv_fmt = DXGI_FORMAT_UNKNOWN,
                    DXGI_FORMAT dsv_fmt = DXGI_FORMAT_UNKNOWN)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt; d.SampleDesc = { 1, 0 };
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t->tex))) return false;

    if (bind & D3D11_BIND_RENDER_TARGET)
        if (FAILED(dev->CreateRenderTargetView(t->tex, nullptr, &t->rtv))) return false;
    if (bind & D3D11_BIND_UNORDERED_ACCESS)
        if (FAILED(dev->CreateUnorderedAccessView(t->tex, nullptr, &t->uav))) return false;
    if (bind & D3D11_BIND_DEPTH_STENCIL)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = dsv_fmt; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateDepthStencilView(t->tex, &dd, &t->dsv))) return false;
    }
    if (bind & D3D11_BIND_SHADER_RESOURCE)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = srv_fmt != DXGI_FORMAT_UNKNOWN ? srv_fmt : fmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(t->tex, &sd, &t->srv))) return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    const int  frames  = argc > 1 ? atoi(argv[1]) : 600;
    const UINT out_w   = argc > 2 ? static_cast<UINT>(atoi(argv[2])) : 1920;
    const UINT out_h   = argc > 3 ? static_cast<UINT>(atoi(argv[3])) : 1080;
    // 0 MaxPerf, 1 Balanced, 2 MaxQuality, 3 UltraPerf, 4 UltraQuality, 5 DLAA.
    const int  quality = argc > 4 ? atoi(argv[4]) : 2;
    printf("ngxhost: %d frames, %ux%u output, PerfQualityValue %d\n",
           frames, out_w, out_h, quality);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ngxhost";
    RegisterClassExW(&wc);
    RECT r = { 0, 0, static_cast<LONG>(out_w), static_cast<LONG>(out_h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"ngxhost", L"ngxhost", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("FAIL: no window\n"); return 2; }
    ShowWindow(hwnd, SW_SHOW);

    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL got = {};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 want, 2, D3D11_SDK_VERSION, &dev, &got, &ctx)))
    { printf("FAIL: D3D11CreateDevice\n"); return 2; }

    IDXGIFactory2 *fac = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac))))
    { printf("FAIL: CreateDXGIFactory2\n"); return 2; }

    // The display format IS the DLSS output format, so presenting is one
    // CopyResource and no shader.
    const DXGI_FORMAT display_fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = out_w; sd.Height = out_h; sd.Format = display_fmt;
    sd.SampleDesc = { 1, 0 }; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    IDXGISwapChain1 *sc = nullptr;
    if (FAILED(fac->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc)))
    { printf("FAIL: CreateSwapChainForHwnd\n"); return 2; }

    // ---- NGX ------------------------------------------------------------
    // Init_with_ProjectID, which is the documented path for an application with no
    // assigned NVIDIA application id. PathListInfo points NGX at this folder, which
    // is what makes the staged snippet the one it loads -- the driver store ships no
    // super-resolution snippet at all, so the application folder always decides.
    wchar_t here[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, here, MAX_PATH);
    if (wchar_t *s = wcsrchr(here, L'\\')) *(s + 1) = 0;
    const wchar_t *paths[1] = { here };
    NVSDK_NGX_FeatureCommonInfo ci = {};
    ci.PathListInfo.Path = paths;
    ci.PathListInfo.Length = 1;
    ci.LoggingInfo.LoggingCallback = &NgxLog;
    ci.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;

    // A real GUID, 8-4-4-4-12 hex. The first attempt spelled "ngxhost0001" into the
    // last group as a joke and NGX answered 0xBAD00005 InvalidParameter -- it does
    // parse this, it does not merely hash it.
    NVSDK_NGX_Result nr = NVSDK_NGX_D3D11_Init_with_ProjectID(
        "a7d3f0c8-6b21-4e5a-9f14-3c07b1e9d240", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
        here, dev, &ci, NVSDK_NGX_Version_API);
    printf("Init_with_ProjectID -> 0x%08X\n", nr);
    if (NVSDK_NGX_FAILED(nr)) { printf("FAIL: NGX init\n"); return 3; }

    NVSDK_NGX_Parameter *caps = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&caps)) || !caps)
    { printf("FAIL: GetCapabilityParameters\n"); return 3; }

    int avail = 0;
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    printf("SuperSampling available: %d\n", avail);
    if (!avail)
    {
        int res = 0; caps->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &res);
        printf("FAIL: DLSS not available here, FeatureInitResult 0x%08X\n", res);
        return 3;
    }

    unsigned int rw = 0, rh = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0.0f;
    // A helper that READS capabilities is fine. The ones that WRITE contract keys
    // are not -- see contract.h.
    NGX_DLSS_GET_OPTIMAL_SETTINGS(caps, out_w, out_h,
                                  static_cast<NVSDK_NGX_PerfQuality_Value>(quality),
                                  &rw, &rh, &maxw, &maxh, &minw, &minh, &sharp);
    if (rw == 0 || rh == 0) { rw = out_w; rh = out_h; }
    printf("optimal render size: %ux%u -> %ux%u\n", rw, rh, out_w, out_h);

    NVSDK_NGX_Parameter *p = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&p)) || !p)
    { printf("FAIL: AllocateParameters\n"); return 3; }

    // ---- resources ------------------------------------------------------
    Tex color, mv, depth, output;
    if (!MakeTex(dev, &color, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
        !MakeTex(dev, &mv, rw, rh, DXGI_FORMAT_R16G16_FLOAT,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
        !MakeTex(dev, &depth, rw, rh, DXGI_FORMAT_R32_TYPELESS,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL,
                 DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT) ||
        !MakeTex(dev, &output, out_w, out_h, display_fmt,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))
    { printf("FAIL: textures\n"); return 2; }

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "scene", nullptr, nullptr,
                          "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "scene", nullptr, nullptr,
                          "ps", "ps_5_0", 0, 0, &psb, &err)))
    { printf("FAIL: shader: %s\n", err ? static_cast<const char *>(err->GetBufferPointer()) : "?"); return 2; }

    ID3D11VertexShader *vs = nullptr; ID3D11PixelShader *ps = nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *cb = nullptr;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) { printf("FAIL: cbuffer\n"); return 2; }

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState *dss = nullptr; dev->CreateDepthStencilState(&dsd, &dss);

    // ---- create ---------------------------------------------------------
    CreateContract cc = {};
    SetU(&cc.width, rw); SetU(&cc.height, rh);
    SetU(&cc.out_width, out_w); SetU(&cc.out_height, out_h);
    SetU(&cc.perf_quality, static_cast<unsigned int>(quality));
    SetU(&cc.create_flags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                           NVSDK_NGX_DLSS_Feature_Flags_IsHDR);
    SetU(&cc.output_subrects, 0);
    SetU(&cc.node_mask_creation, 1); SetU(&cc.node_mask_visibility, 1);

    p->Reset();
    ApplyCreate(p, cc);
    NVSDK_NGX_Handle *feat = nullptr;
    nr = NVSDK_NGX_D3D11_CreateFeature(ctx, NVSDK_NGX_Feature_SuperSampling, p, &feat);
    printf("CreateFeature -> 0x%08X handle=%p\n", nr, static_cast<void *>(feat));
    if (NVSDK_NGX_FAILED(nr) || !feat) { printf("FAIL: CreateFeature\n"); return 3; }

    // ---- the loop -------------------------------------------------------
    const float mvsx = -static_cast<float>(rw), mvsy = -static_cast<float>(rh);
    const int   phases = 8 * (out_w / (rw ? rw : 1)) * (out_w / (rw ? rw : 1)) + 8;
    int delivered = 0;

    for (int i = 0; i < frames; ++i)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { i = frames; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }

        const float jx = Halton((i % phases) + 1, 2) - 0.5f;
        const float jy = Halton((i % phases) + 1, 3) - 0.5f;
        const float panx = static_cast<float>(i) * 0.37f;
        const float pany = static_cast<float>(i) * 0.11f;

        D3D11_MAPPED_SUBRESOURCE ms = {};
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        {
            CB c = {};
            c.pan[0] = panx; c.pan[1] = pany;
            c.jitter[0] = 2.0f * jx / static_cast<float>(rw);
            c.jitter[1] = -2.0f * jy / static_cast<float>(rh);
            c.inv_render[0] = 1.0f / static_cast<float>(rw);
            c.inv_render[1] = 1.0f / static_cast<float>(rh);
            c.mv_scale[0] = mvsx; c.mv_scale[1] = mvsy;
            memcpy(ms.pData, &c, sizeof(c));
            ctx->Unmap(cb, 0);
        }

        ID3D11RenderTargetView *rts[2] = { color.rtv, mv.rtv };
        const float clr[4] = { 0, 0, 0, 1 };
        ctx->ClearRenderTargetView(color.rtv, clr);
        ctx->ClearRenderTargetView(mv.rtv, clr);
        ctx->ClearDepthStencilView(depth.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(2, rts, depth.dsv);
        ctx->OMSetDepthStencilState(dss, 0);
        D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(rw), static_cast<float>(rh), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb); ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(3, 0);
        ID3D11RenderTargetView *none[2] = { nullptr, nullptr };
        ctx->OMSetRenderTargets(2, none, nullptr);

        // Reset and repopulate every frame. See the note at the top of this file.
        EvalContract ec = {};
        SetF(&ec.jitter_x, jx);      SetF(&ec.jitter_y, jy);
        SetF(&ec.mv_scale_x, mvsx);  SetF(&ec.mv_scale_y, mvsy);
        SetF(&ec.sharpness, 0.0f);
        SetF(&ec.pre_exposure, 1.0f);
        SetU(&ec.reset, i == 0 ? 1u : 0u);
        SetU(&ec.subrect_w, rw);     SetU(&ec.subrect_h, rh);

        p->Reset();
        ApplyEval(p, ec);
        p->Set("Color",  static_cast<ID3D11Resource *>(color.tex));
        p->Set("Output", static_cast<ID3D11Resource *>(output.tex));
        p->Set("Depth",  static_cast<ID3D11Resource *>(depth.tex));
        p->Set("MotionVectors", static_cast<ID3D11Resource *>(mv.tex));

        nr = NVSDK_NGX_D3D11_EvaluateFeature(ctx, feat, p, nullptr);
        if (NVSDK_NGX_SUCCEED(nr)) ++delivered;
        else if (i < 3 || (i % 600) == 0)
            printf("  EvaluateFeature frame %d -> 0x%08X\n", i, nr);

        ID3D11Texture2D *bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb)
        { ctx->CopyResource(bb, output.tex); bb->Release(); }
        sc->Present(0, 0);
    }

    printf("evaluates succeeded: %d of %d\n", delivered, frames);

    NVSDK_NGX_D3D11_ReleaseFeature(feat);
    NVSDK_NGX_D3D11_DestroyParameters(p);
    NVSDK_NGX_D3D11_Shutdown1(dev);

    if (dss) dss->Release(); if (cb) cb->Release();
    if (vs) vs->Release(); if (ps) ps->Release();
    if (vsb) vsb->Release(); if (psb) psb->Release();
    color.Release(); mv.Release(); depth.Release(); output.Release();
    BOOL fs = FALSE;
    if (SUCCEEDED(sc->GetFullscreenState(&fs, nullptr)) && fs) sc->SetFullscreenState(FALSE, nullptr);
    sc->Release(); fac->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hwnd);

    printf("ok\n");
    return delivered > 0 ? 0 : 4;
}
