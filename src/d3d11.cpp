// ngxGym -- a scriptable DLSS host, built to be a game as far as ReShade and the
// dlss5-bridge add-on are concerned, so that the transitions those two fail on can
// be reproduced in seconds instead of by launching Baldur's Gate 3 and playing to a
// particular state.
//
// PHASE 2. A real D3D11 DLSS host driven by a scenario file: display modes,
// resolution changes, quality presets, feature recreation, DLSS switched off and on,
// HDR, and the deliberate misbehaviour a real game cannot be asked to perform.
//
// Three rules that make it honest, all easy to lose:
//
//   D3D11 and DXGI are reached through the IMPORT TABLE and never through
//   LoadLibrary. A LoadLibrary of the system copy walks past ReShade's proxy and the
//   run would prove only that this program can clear a buffer.
//
//   The parameter block is Reset and fully repopulated every frame. If jitter were
//   set once and left, an "omit jitter" later in a scenario would still find last
//   frame's value in the block, the bridge's absence handling would never run, and
//   the run would go green having exercised nothing.
//
//   Every failure is loud and stops the run. A host that limps on after a failed
//   feature creation tests the bridge against a state no game ever presents.
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
#include "scenario.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

// ---------------------------------------------------------------------------
// The scene. Deliberately not a renderer: one fullscreen triangle, a procedural
// pattern panned by a constant velocity, depth from a plane equation, and motion
// vectors correct BY CONSTRUCTION rather than by differencing matrices. A bridge fed
// zero motion vectors is not being tested the way a game tests it, and a scene
// elaborate enough to produce real ones would be the bulk of this program.
// ---------------------------------------------------------------------------
static const char kShader[] = R"(
cbuffer C : register(b0)
{
    float2 pan;
    float2 jitter;
    float2 inv_render;
    float2 mv_texel;
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
    float2 p = i.uv / inv_render + pan;
    float  chk = step(0.5, frac(floor(p.x / 24.0) * 0.5 + floor(p.y / 24.0) * 0.5));
    float  n   = noise(p * 0.08) * 0.6 + noise(p * 0.31) * 0.3;
    o.color = float4(saturate(float3(chk * 0.8 + n, n, 1.0 - chk * 0.6) * 1.4), 1);
    o.mv    = mv_texel;
    o.depth = saturate(0.2 + 0.6 * i.uv.y);
    return o;
}
)";

struct CB { float pan[2]; float jitter[2]; float inv_render[2]; float mv_texel[2]; };

struct Tex
{
    ID3D11Texture2D           *tex = nullptr;
    ID3D11RenderTargetView    *rtv = nullptr;
    ID3D11ShaderResourceView  *srv = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
    ID3D11DepthStencilView    *dsv = nullptr;
    void Release()
    {
        if (dsv) { dsv->Release(); dsv = nullptr; }
        if (uav) { uav->Release(); uav = nullptr; }
        if (srv) { srv->Release(); srv = nullptr; }
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
    }
};

struct Host
{
    HWND                 hwnd = nullptr;
    ID3D11Device        *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    IDXGIFactory2       *fac = nullptr;
    IDXGISwapChain1     *sc  = nullptr;

    UINT out_w = 1920, out_h = 1080, rw = 0, rh = 0;
    int  quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    bool hdr = false;
    Mode mode = MODE_WINDOWED;
    DXGI_FORMAT display_fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;

    Tex color, mv, depth, output;
    // Not a fifth slot. Most games supply none, and the four are handled
    // together everywhere; this one is its own thing, exactly as it is in the
    // add-on under test.
    Tex  exposure;
    bool exposure_on = false;
    ID3D11VertexShader      *vs  = nullptr;
    ID3D11PixelShader       *ps  = nullptr;
    ID3D11Buffer            *cb  = nullptr;
    ID3D11DepthStencilState *dss = nullptr;

    NVSDK_NGX_Parameter *caps = nullptr;
    NVSDK_NGX_Parameter *p    = nullptr;
    NVSDK_NGX_Handle    *feat = nullptr;

    bool dlss_on   = true;
    bool transpose = false;
    bool stale = false;
    Omit omit      = OMIT_NONE;

    int frame = 0, delivered = 0, evaluated = 0;
    RECT     windowed_rect = {};
    LONG_PTR windowed_style = 0;
};

    // The motion vector, computed on the CPU and written verbatim. It used to be
    // "-pan * mv_scale * inv_render" under a comment claiming it was correct by
    // construction, and it was wrong twice over: it multiplied by the very scale
    // NGX multiplies by, and it encoded the CUMULATIVE pan rather than the per-frame
    // delta. At frame 100 with a 1280-wide render that wrote 37.0, which NGX read as
    // 47360 pixels of motion. It is right on frame 1 and only there, which is how it
    // survived being read several times.
    //
    // Doing the arithmetic on the CPU is not a shortcut: it puts the value where it
    // can be printed and checked against MV.Scale, which is the only way anyone was
    // ever going to notice.
static const float kVelX = 0.37f;
static const float kVelY = 0.11f;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static bool MakeTex(ID3D11Device *dev, Tex *t, UINT w, UINT h, DXGI_FORMAT fmt,
                    UINT bind, DXGI_FORMAT srv_fmt = DXGI_FORMAT_UNKNOWN,
                    DXGI_FORMAT dsv_fmt = DXGI_FORMAT_UNKNOWN)
{
    t->Release();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt; d.SampleDesc = { 1, 0 };
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t->tex))) return false;
    if ((bind & D3D11_BIND_RENDER_TARGET) &&
        FAILED(dev->CreateRenderTargetView(t->tex, nullptr, &t->rtv))) return false;
    if ((bind & D3D11_BIND_UNORDERED_ACCESS) &&
        FAILED(dev->CreateUnorderedAccessView(t->tex, nullptr, &t->uav))) return false;
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

// One place, so the create block and the evaluate block cannot drift apart -- which
// is a disagreement real games do have and this host should only produce on purpose.
static unsigned int HostCreateFlags(const Host &h)
{
    unsigned int fl = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (h.hdr) fl |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    return fl;
}

static void ReleaseFeat(Host &h)
{
    if (h.feat != nullptr) { NVSDK_NGX_D3D11_ReleaseFeature(h.feat); h.feat = nullptr; }
}

// Ask NGX for the render size this quality wants, then rebuild the four textures
// and the feature. Every path that changes a shape comes through here, which is why
// there is only one place that can get the order wrong.
static bool Rebuild(Host &h, const char *why)
{
    unsigned int maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0.0f;
    NGX_DLSS_GET_OPTIMAL_SETTINGS(h.caps, h.out_w, h.out_h,
                                  static_cast<NVSDK_NGX_PerfQuality_Value>(h.quality),
                                  &h.rw, &h.rh, &maxw, &maxh, &minw, &minh, &sharp);
    if (h.rw == 0 || h.rh == 0) { h.rw = h.out_w; h.rh = h.out_h; }

    printf("  rebuild (%s): %ux%u -> %ux%u, quality %d, hdr %d\n",
           why, h.rw, h.rh, h.out_w, h.out_h, h.quality, h.hdr ? 1 : 0);

    if (!MakeTex(h.dev, &h.color, h.rw, h.rh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
        !MakeTex(h.dev, &h.mv, h.rw, h.rh, DXGI_FORMAT_R16G16_FLOAT,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
        !MakeTex(h.dev, &h.depth, h.rw, h.rh, DXGI_FORMAT_R32_TYPELESS,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL,
                 DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT) ||
        !MakeTex(h.dev, &h.output, h.out_w, h.out_h, h.display_fmt,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))
    { printf("FAIL: textures\n"); return false; }

    // 1x1 R32_FLOAT, which is the shape every exposure texture in evidence has had.
    // Made once and kept: unlike the four above, its shape never depends on the
    // render size. Kept out of the four deliberately, because most games supply none
    // and the add-on under test keeps its own mirror of it out of its slot array for
    // the same reason.
    if (h.exposure.tex == nullptr &&
        !MakeTex(h.dev, &h.exposure, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE))
    { printf("FAIL: exposure texture\n"); return false; }

    ReleaseFeat(h);

    CreateContract cc = {};
    SetU(&cc.width, h.rw); SetU(&cc.height, h.rh);
    SetU(&cc.out_width, h.out_w); SetU(&cc.out_height, h.out_h);
    SetU(&cc.perf_quality, static_cast<unsigned int>(h.quality));
    if (h.omit != OMIT_FLAGS) SetU(&cc.create_flags, HostCreateFlags(h));
    SetU(&cc.output_subrects, 0);
    SetU(&cc.node_mask_creation, 1); SetU(&cc.node_mask_visibility, 1);

    // A game whose DLSS is off creates no feature on a mode change, and one that
    // never had DLSS creates none at all. The bridge counts creates: one here
    // while the substitute contract holds the session reads as "the game has
    // already used DLSS itself" and refuses the re-arm, which is not what a game
    // with DLSS off does.
    if (!h.dlss_on) { printf("  no feature: dlss is off\n"); return true; }

    h.p->Reset();
    ApplyCreate(h.p, cc);
    const NVSDK_NGX_Result r =
        NVSDK_NGX_D3D11_CreateFeature(h.ctx, NVSDK_NGX_Feature_SuperSampling, h.p, &h.feat);
    if (NVSDK_NGX_FAILED(r) || h.feat == nullptr)
    { printf("FAIL: CreateFeature -> 0x%08X\n", r); return false; }
    return true;
}

// Resize the swapchain to the window's current client area, in the current format,
// then rebuild everything sized to it.
static bool ResizeToWindow(Host &h, const char *why)
{
    RECT c = {};
    GetClientRect(h.hwnd, &c);
    const UINT w = static_cast<UINT>(c.right - c.left);
    const UINT hh = static_cast<UINT>(c.bottom - c.top);
    if (w == 0 || hh == 0) return true;   // minimised; nothing to do yet

    h.color.Release(); h.mv.Release(); h.depth.Release(); h.output.Release();
    const HRESULT hr = h.sc->ResizeBuffers(0, w, hh, h.display_fmt,
                                           DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    if (FAILED(hr)) { printf("FAIL: ResizeBuffers 0x%08lX\n", hr); return false; }
    h.out_w = w; h.out_h = hh;
    return Rebuild(h, why);
}

// Defined below with the window helpers; ApplyMode has to know.
static bool BackgroundMode();

static bool ApplyMode(Host &h, Mode m)
{
    if (m == h.mode) return true;

    // Exclusive fullscreen takes the display whatever anybody wants, so it is
    // the one mode a background run cannot honour. Refused rather than
    // silently downgraded: a scenario that asked for it and got borderless
    // would report on a state it never reached.
    if (m == MODE_EXCLUSIVE && BackgroundMode())
    {
        printf("  mode exclusive: refused, this run is in the background\n");
        return true;
    }


    // Leaving exclusive first, always. Changing window style while the swapchain
    // owns the display leaves the desktop mode changed if anything then fails.
    BOOL fs = FALSE;
    h.sc->GetFullscreenState(&fs, nullptr);
    if (fs && m != MODE_EXCLUSIVE) h.sc->SetFullscreenState(FALSE, nullptr);

    if (m == MODE_EXCLUSIVE)
    {
        const HRESULT hr = h.sc->SetFullscreenState(TRUE, nullptr);
        if (FAILED(hr))
        {
            // Not fatal and worth saying rather than asserting: exclusive fullscreen
            // is refused on some configurations, and a run that reports why is more
            // useful than one that dies.
            printf("  mode exclusive refused 0x%08lX, staying in %s\n", hr,
                   h.mode == MODE_BORDERLESS ? "borderless" : "windowed");
            return true;
        }
    }
    else if (m == MODE_BORDERLESS)
    {
        if (h.mode == MODE_WINDOWED)
        { h.windowed_style = GetWindowLongPtrW(h.hwnd, GWL_STYLE); GetWindowRect(h.hwnd, &h.windowed_rect); }
        HMONITOR mon = MonitorFromWindow(h.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongPtrW(h.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h.hwnd, BackgroundMode() ? HWND_BOTTOM : HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW |
                     (BackgroundMode() ? SWP_NOACTIVATE : 0u));
    }
    else
    {
        if (h.windowed_style != 0)
        {
            SetWindowLongPtrW(h.hwnd, GWL_STYLE, h.windowed_style);
            SetWindowPos(h.hwnd, BackgroundMode() ? HWND_BOTTOM : HWND_TOP, h.windowed_rect.left, h.windowed_rect.top,
                         h.windowed_rect.right - h.windowed_rect.left,
                         h.windowed_rect.bottom - h.windowed_rect.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW |
                     (BackgroundMode() ? SWP_NOACTIVATE : 0u));
        }
    }

    h.mode = m;
    return ResizeToWindow(h, "mode change");
}

static bool ApplyHdr(Host &h, bool on)
{
    if (on == h.hdr) return true;
    h.hdr = on;
    // scRGB float for SDR, HDR10 for on. Changing the swapchain format is a
    // ResizeBuffers, which is also a rebuild -- deliberately the same path a
    // resolution change takes, because that is what a game does here too.
    h.display_fmt = on ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!ResizeToWindow(h, on ? "hdr on" : "hdr off")) return false;

    IDXGISwapChain3 *sc3 = nullptr;
    if (SUCCEEDED(h.sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr)
    {
        sc3->SetColorSpace1(on ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                               : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        sc3->Release();
    }
    return true;
}

static bool RenderFrame(Host &h)
{
    const float jx = Halton((h.frame % 32) + 1, 2) - 0.5f;
    const float jy = Halton((h.frame % 32) + 1, 3) - 0.5f;
    const float panx = static_cast<float>(h.frame) * kVelX;
    const float pany = static_cast<float>(h.frame) * kVelY;
    const float mvsx = -static_cast<float>(h.rw), mvsy = -static_cast<float>(h.rh);

    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (SUCCEEDED(h.ctx->Map(h.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        CB c = {};
        c.pan[0] = panx; c.pan[1] = pany;
        c.jitter[0] = 2.0f * jx / static_cast<float>(h.rw);
        c.jitter[1] = -2.0f * jy / static_cast<float>(h.rh);
        c.inv_render[0] = 1.0f / static_cast<float>(h.rw);
        c.inv_render[1] = 1.0f / static_cast<float>(h.rh);
        // The pattern pans by +kVelX per frame, so a feature now was at x + kVelX
        // last frame; DLSS wants previous-minus-current, which is +kVelX pixels.
        // Divided by MV.Scale because NGX multiplies by it.
        c.mv_texel[0] = kVelX / mvsx;
        c.mv_texel[1] = kVelY / mvsy;
        memcpy(ms.pData, &c, sizeof(c));
        h.ctx->Unmap(h.cb, 0);
    }

    ID3D11RenderTargetView *rts[2] = { h.color.rtv, h.mv.rtv };
    const float clr[4] = { 0, 0, 0, 1 };
    h.ctx->ClearRenderTargetView(h.color.rtv, clr);
    h.ctx->ClearRenderTargetView(h.mv.rtv, clr);
    h.ctx->ClearDepthStencilView(h.depth.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    h.ctx->OMSetRenderTargets(2, rts, h.depth.dsv);
    h.ctx->OMSetDepthStencilState(h.dss, 0);
    D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(h.rw), static_cast<float>(h.rh), 0, 1 };
    h.ctx->RSSetViewports(1, &vp);
    h.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    h.ctx->VSSetShader(h.vs, nullptr, 0);
    h.ctx->PSSetShader(h.ps, nullptr, 0);
    h.ctx->VSSetConstantBuffers(0, 1, &h.cb);
    h.ctx->PSSetConstantBuffers(0, 1, &h.cb);
    // Nine identical draws of the one triangle, on purpose. ReShade's generic
    // depth add-on (examples/09-depth/generic_depth_addon.cpp) does not count a
    // frame in which the only depth-stencil received 8 draw calls or fewer -- a
    // special case for emulators that present more often than they render -- and
    // it skips a depth-stencil whose frame drew 3 vertices or fewer as unused.
    // One fullscreen triangle is exactly one draw of three vertices, so no depth
    // buffer was ever selected here, the DEPTH semantic never bound, and the
    // substitute contract could not arm in this gym at all. The extra draws
    // write the same pixels at the same depth; the picture does not change.
    for (int i = 0; i < 9; ++i) h.ctx->Draw(3, 0);
    ID3D11RenderTargetView *none[2] = { nullptr, nullptr };
    h.ctx->OMSetRenderTargets(2, none, nullptr);

    if (h.dlss_on && h.feat != nullptr)
    {
        EvalContract ec = {};
        if (h.omit != OMIT_JITTER)  { SetF(&ec.jitter_x, jx);     SetF(&ec.jitter_y, jy); }
        if (h.omit != OMIT_MVSCALE) { SetF(&ec.mv_scale_x, mvsx); SetF(&ec.mv_scale_y, mvsy); }
        SetF(&ec.sharpness, 0.0f);
        SetF(&ec.pre_exposure, 1.0f);
        SetU(&ec.reset, h.frame == 0 ? 1u : 0u);
        SetU(&ec.subrect_w, h.rw); SetU(&ec.subrect_h, h.rh);
        if (h.omit != OMIT_FLAGS) SetU(&ec.create_flags, HostCreateFlags(h));
        if (h.omit != OMIT_QUALITY) SetU(&ec.perf_quality, static_cast<unsigned int>(h.quality));

        h.p->Reset();
        ApplyEval(h.p, ec);

        // The misbehaviour. Baldur's Gate 3 emitted exactly this after a borderless
        // to fullscreen change: the four scalars transposed in the EVALUATE block
        // while the images it handed over stayed honest, and it did not stop.
        if (h.transpose)
        {
            h.p->Set("Width",     h.out_w); h.p->Set("Height",    h.out_h);
            h.p->Set("OutWidth",  h.rw);    h.p->Set("OutHeight", h.rh);
        }
        // The other misbehaviour. Baldur's Gate 3's character creator creates a
        // preview feature through the same parameter block, and from then on
        // every evaluate of the MAIN feature carries the preview's four scalars
        // -- 1920x1080 -> 1280x720 on a 3413x960 -> 5120x1440 session -- while. Here
        // 960x540 -> 640x360, which is deliberately NOT a transposition of this
        // host's own 1280x720 -> 1920x1080: the swap repair alone would fix that.
        // the textures and DLSS.Render.Subrect.Dimensions stay honest (#18).
        if (h.stale)
        {
            h.p->Set("Width",      960u); h.p->Set("Height",     540u);
            h.p->Set("OutWidth",   640u); h.p->Set("OutHeight",  360u);
        }

        h.p->Set("Color",  static_cast<ID3D11Resource *>(h.color.tex));
        h.p->Set("Output", static_cast<ID3D11Resource *>(h.output.tex));
        h.p->Set("Depth",  static_cast<ID3D11Resource *>(h.depth.tex));
        h.p->Set("MotionVectors", static_cast<ID3D11Resource *>(h.mv.tex));
        // The create flags deliberately never carry AutoExposure, so switching this
        // on is the Bannerlord and Red Dead Redemption 2 shape: DLSS is driven from
        // an exposure texture and told nothing about it.
        if (h.exposure_on)
        {
            h.p->Set("ExposureTexture", static_cast<ID3D11Resource *>(h.exposure.tex));
            // Read straight back, once. The add-on reported "no such key here" for
            // this key across a whole run where the host believed it was setting it,
            // and there was no way to tell whether the host's Set never landed or the
            // add-on was reading a different block. One line settles it, on the same
            // object, in the same frame.
            static bool said = false;
            if (!said)
            {
                said = true;
                ID3D11Resource *back = nullptr;
                const NVSDK_NGX_Result g = h.p->Get("ExposureTexture", &back);
                printf("  ExposureTexture set=%p, read back 0x%08X -> %p\n",
                       static_cast<void *>(h.exposure.tex), g, static_cast<void *>(back));
            }
        }

        const NVSDK_NGX_Result r = NVSDK_NGX_D3D11_EvaluateFeature(h.ctx, h.feat, h.p, nullptr);
        ++h.evaluated;
        if (NVSDK_NGX_SUCCEED(r)) ++h.delivered;
        else if (h.delivered == 0 || (h.evaluated % 600) == 0)
            printf("  EvaluateFeature frame %d -> 0x%08X\n", h.frame, r);
    }


    // Read one motion-vector texel back, once, and print what NGX will make of it.
    // The previous version of this scene carried a comment claiming the vectors were
    // correct by construction; they were wrong by a factor of 128000 and nobody could
    // have seen it, because nothing printed a number. A comment asserting correctness
    // with no runnable check behind it is the same failure as a PASS with no
    // assertion. Printed, not asserted -- same cadence as the ExposureTexture
    // read-back below, and for the same reason.
    if (h.frame == 2)
    {
        D3D11_TEXTURE2D_DESC sd = {};
        h.mv.tex->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ID3D11Texture2D *st = nullptr;
        if (SUCCEEDED(h.dev->CreateTexture2D(&sd, nullptr, &st)) && st != nullptr)
        {
            h.ctx->CopyResource(st, h.mv.tex);
            D3D11_MAPPED_SUBRESOURCE ms2 = {};
            if (SUCCEEDED(h.ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms2)))
            {
                // R16G16_FLOAT: two halves. Converted by hand rather than pulling in
                // a library for four lines.
                const unsigned short *px = static_cast<const unsigned short *>(ms2.pData);
                auto half = [](unsigned short v) {
                    const int s2 = (v >> 15) & 1, e = (v >> 10) & 0x1F, m = v & 0x3FF;
                    float f = 0.0f;
                    if (e == 0)      f = m / 1024.0f / 16384.0f;
                    else if (e < 31) f = (1.0f + m / 1024.0f) * powf(2.0f, static_cast<float>(e - 15));
                    return s2 ? -f : f;
                };
                const float tx = half(px[0]), ty = half(px[1]);
                printf("  MV texel (%.8f, %.8f) x MV.Scale (%.1f, %.1f) = (%.3f, %.3f) px,"
                       " scene velocity is (%.2f, %.2f)\n",
                       tx, ty, mvsx, mvsy, tx * mvsx, ty * mvsy, kVelX, kVelY);
                h.ctx->Unmap(st, 0);
            }
            st->Release();
        }
    }
    ID3D11Texture2D *bb = nullptr;
    if (SUCCEEDED(h.sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb != nullptr)
    {
        // Both arms of this were the same texture, under a comment describing a
        // choice that was never made -- so the DLSS-off segment presented one frozen
        // upscaled frame for nine hundred frames. The colour target is the render
        // resolution and the back buffer is the output resolution, so it cannot be
        // a CopyResource; CopySubresourceRegion places it in the corner, which is
        // ugly and is not the point. What matters is that presents keep happening
        // and that what is presented CHANGES, because a frozen image is
        // indistinguishable from a bridge that has stopped.
        if (h.dlss_on && h.feat != nullptr)
        {
            h.ctx->CopyResource(bb, h.output.tex);
        }
        else
        {
            D3D11_BOX box = { 0, 0, 0, h.rw, h.rh, 1 };
            h.ctx->CopySubresourceRegion(bb, 0, 0, 0, 0, h.color.tex, 0, &box);
        }
        bb->Release();
    }
    h.sc->Present(0, 0);
    ++h.frame;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return true;
}


// Run without taking the screen. NGXGYM_BACKGROUND=1 in the environment creates
// the window without activating it and drops it to the bottom of the z-order, so
// a suite can run while somebody works.
//
// NOT minimised: a minimised window has a zero-size client area, the swapchain
// extent goes to 0x0 and the Vulkan half refuses to build one -- correctly, since
// there is nothing to present to. Occluded is the state that keeps rendering.
//
// WS_EX_TOOLWINDOW keeps it out of alt-tab and the taskbar as well.
static bool BackgroundMode()
{
    static int cached = -1;
    if (cached < 0)
    {
        char v[8] = {};
        cached = (GetEnvironmentVariableA("NGXGYM_BACKGROUND", v, sizeof(v)) != 0 &&
                  v[0] == '1') ? 1 : 0;
    }
    return cached != 0;
}

static void ShowHostWindow(HWND w)
{
    if (!BackgroundMode()) { ShowWindow(w, SW_SHOW); return; }
    ShowWindow(w, SW_SHOWNOACTIVATE);
    SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    printf("background: the window is shown without focus and sent to the back. "
           "Exclusive fullscreen is refused in this mode.\n");
}


// What the driver put into this process, and what the swapchain says it
// presented. Printed at the end of every run so a driver-side feature that has
// no API of its own -- Smooth Motion is one -- can be told apart by what it
// changes: the NVIDIA modules loaded, and the present statistics. Compare a run
// with the feature on against one with it off; the line that differs is the
// detector, and there is none until that comparison has been made.
typedef BOOL (WINAPI *PFN_EnumProcessModules)(HANDLE, HMODULE *, DWORD, DWORD *);
typedef DWORD (WINAPI *PFN_GetModuleFileNameExA)(HANDLE, HMODULE, LPSTR, DWORD);
static void PrintNvModules()
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    PFN_EnumProcessModules   enumMods = reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    PFN_GetModuleFileNameExA nameOf   = reinterpret_cast<PFN_GetModuleFileNameExA>(GetProcAddress(k32, "K32GetModuleFileNameExA"));
    if (enumMods == nullptr || nameOf == nullptr) { printf("modules: unavailable\n"); return; }
    HMODULE mods[1024]; DWORD bytes = 0;
    if (!enumMods(GetCurrentProcess(), mods, sizeof(mods), &bytes)) { printf("modules: enumeration failed\n"); return; }
    const DWORD n = bytes / sizeof(HMODULE);
    printf("nvidia modules in this process (%lu modules total):\n", static_cast<unsigned long>(n));
    for (DWORD i = 0; i < n && i < 1024; ++i)
    {
        char path[MAX_PATH]; if (nameOf(GetCurrentProcess(), mods[i], path, MAX_PATH) == 0) continue;
        const char *leaf = strrchr(path, '\\'); leaf = leaf ? leaf + 1 : path;
        if (_strnicmp(leaf, "nv", 2) == 0 || _strnicmp(leaf, "_nv", 3) == 0) printf("  %s\n", path);
    }
}

static void PrintPresentStats(IDXGISwapChain1 *sc)
{
    DXGI_FRAME_STATISTICS st = {};
    const HRESULT hr = sc != nullptr ? sc->GetFrameStatistics(&st) : E_POINTER;
    if (FAILED(hr)) { printf("present statistics: unavailable (0x%08X)\n", static_cast<unsigned>(hr)); return; }
    printf("present statistics: PresentCount %u  PresentRefreshCount %u  SyncRefreshCount %u\n",
           st.PresentCount, st.PresentRefreshCount, st.SyncRefreshCount);
}

int main(int argc, char **argv)
{
    // Unbuffered, always. When the runner redirects stdout to a file it becomes
    // block-buffered, so a crash loses everything not yet flushed -- and an empty
    // host.out beside a 0xC0000409 reads as "it died before main", which is a wrong
    // conclusion this file's author reached once already.
    setvbuf(stdout, nullptr, _IONBF, 0);

    Scenario sc = {};
    if (argc > 1 && strstr(argv[1], ".txt") != nullptr)
    {
        if (!ScenarioLoad(&sc, argv[1])) { printf("FAIL: scenario\n"); return 2; }
    }
    else
    {
        // No scenario file: the smoke test, so a bare run still does something.
        ScenarioAdd(&sc, STEP_FRAMES, argc > 1 ? atoi(argv[1]) : 600);
        strncpy_s(sc.name, "smoke", _TRUNCATE);
    }
    printf("ngxGym: scenario '%s', %d steps\n", sc.name, sc.count);

    Host h;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ngxGym";
    RegisterClassExW(&wc);
    RECT r = { 0, 0, static_cast<LONG>(h.out_w), static_cast<LONG>(h.out_h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    h.hwnd = CreateWindowExW(BackgroundMode() ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0, L"ngxGym", L"ngxGym", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (h.hwnd == nullptr) { printf("FAIL: no window\n"); return 2; }
    ShowHostWindow(h.hwnd);

    D3D_FEATURE_LEVEL got = {};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 want, 2, D3D11_SDK_VERSION, &h.dev, &got, &h.ctx)))
    { printf("FAIL: D3D11CreateDevice\n"); return 2; }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&h.fac))))
    { printf("FAIL: CreateDXGIFactory2\n"); return 2; }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = h.out_w; sd.Height = h.out_h; sd.Format = h.display_fmt;
    sd.SampleDesc = { 1, 0 }; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (FAILED(h.fac->CreateSwapChainForHwnd(h.dev, h.hwnd, &sd, nullptr, nullptr, &h.sc)))
    { printf("FAIL: CreateSwapChainForHwnd\n"); return 2; }

    wchar_t here[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, here, MAX_PATH);
    if (wchar_t *s = wcsrchr(here, L'\\')) *(s + 1) = 0;
    const wchar_t *paths[1] = { here };
    NVSDK_NGX_FeatureCommonInfo ci = {};
    ci.PathListInfo.Path = paths;
    ci.PathListInfo.Length = 1;

    // A real GUID, 8-4-4-4-12 hex. NGX parses this rather than hashing it, and
    // answers 0xBAD00005 InvalidParameter to a malformed one -- which is worth
    // knowing, because that code usually means something else entirely.
    NVSDK_NGX_Result nr = NVSDK_NGX_D3D11_Init_with_ProjectID(
        "a7d3f0c8-6b21-4e5a-9f14-3c07b1e9d240", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
        here, h.dev, &ci, NVSDK_NGX_Version_API);
    printf("Init_with_ProjectID -> 0x%08X\n", nr);
    if (NVSDK_NGX_FAILED(nr)) { printf("FAIL: NGX init\n"); return 3; }

    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&h.caps)) || !h.caps)
    { printf("FAIL: GetCapabilityParameters\n"); return 3; }
    int avail = 0;
    h.caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (!avail)
    {
        int res = 0; h.caps->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &res);
        printf("FAIL: DLSS unavailable, FeatureInitResult 0x%08X\n", res);
        return 3;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&h.p)) || !h.p)
    { printf("FAIL: AllocateParameters\n"); return 3; }

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "scene", nullptr, nullptr,
                          "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "scene", nullptr, nullptr,
                          "ps", "ps_5_0", 0, 0, &psb, &err)))
    { printf("FAIL: shader: %s\n", err ? static_cast<const char *>(err->GetBufferPointer()) : "?"); return 2; }
    h.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &h.vs);
    h.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &h.ps);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(h.dev->CreateBuffer(&bd, nullptr, &h.cb))) { printf("FAIL: cbuffer\n"); return 2; }
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    h.dev->CreateDepthStencilState(&dsd, &h.dss);

    h.dlss_on = !sc.nodlss;
    if (!Rebuild(h, "start")) return 3;

    // How many frames the scenario asks for, so a run that ends early is a failure
    // rather than a short green one. Closing the window at frame 5 of 7600 used to
    // abandon the loop with rc still 0, print the counters, print ok and return 0.
    int rc = 0, want_frames = 0;
    for (int i = 0; i < sc.count; ++i)
        if (sc.steps[i].kind == STEP_FRAMES) want_frames += sc.steps[i].a;
    for (int s = 0; s < sc.count && rc == 0; ++s)
    {
        const Step &st = sc.steps[s];
        switch (st.kind)
        {
        case STEP_FRAMES:
            printf("[%d/%d] frames %d\n", s + 1, sc.count, st.a);
            for (int i = 0; i < st.a; ++i) if (!RenderFrame(h)) { s = sc.count; break; }
            break;
        case STEP_MODE:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            if (!ApplyMode(h, static_cast<Mode>(st.a))) rc = 4;
            break;
        case STEP_RESIZE:
            printf("[%d/%d] resize %d %d\n", s + 1, sc.count, st.a, st.b);
            {
                RECT wr = { 0, 0, st.a, st.b };
                AdjustWindowRect(&wr, static_cast<DWORD>(GetWindowLongPtrW(h.hwnd, GWL_STYLE)), FALSE);
                SetWindowPos(h.hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                             SWP_NOMOVE | SWP_NOZORDER);
                if (!ResizeToWindow(h, "resize")) rc = 4;
            }
            break;
        case STEP_PRESET:
            printf("[%d/%d] preset %d\n", s + 1, sc.count, st.a);
            h.quality = st.a;
            if (!Rebuild(h, "preset change")) rc = 4;
            break;
        case STEP_RECREATE:
            printf("[%d/%d] recreate at an unchanged shape\n", s + 1, sc.count);
            if (!Rebuild(h, "recreate")) rc = 4;
            break;
        case STEP_DLSS:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            h.dlss_on = st.a != 0;
            // Back on after a rebuild that skipped the create: build one now.
            if (h.dlss_on && h.feat == nullptr && !Rebuild(h, "dlss on")) rc = 4;
            break;
        case STEP_HDR:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            if (!ApplyHdr(h, st.a != 0)) rc = 4;
            break;
        case STEP_TRANSPOSE:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            h.transpose = st.a != 0;
            break;
        case STEP_STALE:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            h.stale = st.a != 0;
            break;
        case STEP_OMIT:
            printf("[%d/%d] omit %d\n", s + 1, sc.count, st.a);
            h.omit = static_cast<Omit>(st.a);
            if (st.a == OMIT_FLAGS && !Rebuild(h, "omit flags")) rc = 4;
            break;
        case STEP_EXPOSURE:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            h.exposure_on = st.a != 0;
            break;
        // Loud, not silent. A verb the parser accepted and the executor drops is a
        // run that proves something other than what its file says -- which happened
        // here: exposure on/off parsed, printed nothing, changed nothing, and the
        // scenario reported PASS having tested the state it was not testing. A
        // switch over an enum warns about this only at /Wall, so the default does
        // the job instead.
        default:
            printf("[%d/%d] FAIL: step kind %d is parsed but not executed\n",
                   s + 1, sc.count, static_cast<int>(st.kind));
            rc = 5;
            break;
        }
    }

    printf("frames %d, evaluates %d, succeeded %d\n", h.frame, h.evaluated, h.delivered);

    ReleaseFeat(h);
    if (h.p) NVSDK_NGX_D3D11_DestroyParameters(h.p);
    NVSDK_NGX_D3D11_Shutdown1(h.dev);
    if (h.dss) h.dss->Release(); if (h.cb) h.cb->Release();
    if (h.vs) h.vs->Release();   if (h.ps) h.ps->Release();
    if (vsb) vsb->Release();     if (psb) psb->Release();
    h.color.Release(); h.mv.Release(); h.depth.Release(); h.output.Release();
    h.exposure.Release();
    BOOL fs = FALSE;
    if (SUCCEEDED(h.sc->GetFullscreenState(&fs, nullptr)) && fs) h.sc->SetFullscreenState(FALSE, nullptr);
    h.sc->Release(); h.fac->Release(); h.ctx->Release(); h.dev->Release();
    DestroyWindow(h.hwnd);

    if (h.frame < want_frames)
    { printf("FAIL: ran %d of %d frames the scenario asked for\n", h.frame, want_frames); return 6; }
    PrintNvModules();
    PrintPresentStats(h.sc);
    if (rc != 0) { printf("FAIL: scenario stopped\n"); return rc; }
    if (h.evaluated > 0 && h.delivered == 0) { printf("FAIL: no evaluate succeeded\n"); return 4; }
    printf("ok\n");
    return 0;
}
