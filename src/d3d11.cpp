// ngxhost -- a scriptable DLSS host, built to be a game as far as ReShade and the
// dlss5-bridge add-on are concerned, so that the transitions those two fail on can
// be reproduced in seconds instead of by launching Baldur's Gate 3 and playing to a
// particular state.
//
// PHASE 0. This file is not yet a DLSS host. It is the cheapest possible
// falsification of the whole idea: a window, a real flip-model D3D11 swapchain, and
// three hundred presents. If ReShade does not load here, or the add-on does not
// register, nothing else in the plan is worth writing -- and finding that out costs
// forty lines rather than a week.
//
// The one rule that makes it honest: D3D11 and DXGI are reached through the IMPORT
// TABLE (d3d11.lib, dxgi.lib) and never through LoadLibrary. A LoadLibrary of the
// system copy walks straight past ReShade's proxy, and the test would then prove
// only that this program can clear a buffer.
//
//   ..\build.cmd

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <cstdio>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char **argv)
{
    const int frames = argc > 1 ? atoi(argv[1]) : 300;
    printf("ngxhost phase 0: %d frames\n", frames);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ngxhost";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, 1280, 720 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"ngxhost", L"ngxhost", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) { printf("FAIL: no window\n"); return 2; }
    ShowWindow(hwnd, SW_SHOW);

    ID3D11Device        *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL    got = {};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   want, 2, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) { printf("FAIL: D3D11CreateDevice 0x%08lX\n", hr); return 2; }

    IDXGIFactory2 *fac = nullptr;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
    if (FAILED(hr)) { printf("FAIL: CreateDXGIFactory2 0x%08lX\n", hr); return 2; }

    // Flip model with ALLOW_MODE_SWITCH, because phase 2 needs SetFullscreenState
    // and a swapchain created without that flag cannot take it.
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format      = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.SampleDesc  = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    IDXGISwapChain1 *sc1 = nullptr;
    hr = fac->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { printf("FAIL: CreateSwapChainForHwnd 0x%08lX\n", hr); return 2; }

    printf("device and swapchain up, presenting\n");
    fflush(stdout);

    for (int i = 0; i < frames; ++i)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { i = frames; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }

        ID3D11Texture2D *bb = nullptr;
        if (SUCCEEDED(sc1->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb != nullptr)
        {
            ID3D11RenderTargetView *rtv = nullptr;
            if (SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv != nullptr)
            {
                // Something that moves, so a human watching can tell it is alive and
                // so a later phase's readback has a signal rather than a flat colour.
                const float t = static_cast<float>(i) / static_cast<float>(frames);
                const float c[4] = { 0.05f, t, 1.0f - t, 1.0f };
                ctx->ClearRenderTargetView(rtv, c);
                rtv->Release();
            }
            bb->Release();
        }
        sc1->Present(0, 0);
    }

    // Exclusive fullscreen has to be released before the swapchain goes, or the
    // display mode is left changed for the whole desktop. Phase 0 never enters it;
    // this is here so the shape is right before phase 2 does.
    BOOL fs = FALSE;
    if (SUCCEEDED(sc1->GetFullscreenState(&fs, nullptr)) && fs) sc1->SetFullscreenState(FALSE, nullptr);

    sc1->Release(); fac->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hwnd);
    printf("ok: %d frames presented\n", frames);
    return 0;
}
