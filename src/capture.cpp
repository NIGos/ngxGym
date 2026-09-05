// Gym-only final-frame readback, after ReShade/bridge effects and before Present.
// Staged only for placement diagnostics. PNG/screenshots can apply another HDR
// tone map, so keep the actual R10 PQ samples. Never installed into a game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <cstdio>
#include "reshade_events.hpp"

extern "C" __declspec(dllexport) const char *NAME = "ngxGym final-frame capture";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Diagnostic raw HDR capture";
static void (*unregister_addon)(HMODULE);

static void Capture(reshade::api::effect_runtime *rt, reshade::api::command_list *cmd,
                    reshade::api::resource_view rtv, reshade::api::resource_view)
{
    static unsigned frame = 0;
    if (++frame != 1200 || rt->get_device()->get_api() != reshade::api::device_api::d3d11) return;
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    auto *view = reinterpret_cast<ID3D11RenderTargetView *>(rtv.handle);
    ID3D11Resource *res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex)))) { res->Release(); return; }
    res->Release();
    ID3D11Device *dev = nullptr;
    tex->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC d = {}; tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R10G10B10A2_UNORM || d.SampleDesc.Count != 1)
    { dev->Release(); tex->Release(); return; }
    d.BindFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &stage)))
    {
        ctx->CopyResource(stage, tex);
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map)))
        {
            FILE *file = nullptr;
            if (fopen_s(&file, "gym-display.bin", "wb") == 0 && file)
            {
                const unsigned header[] = { 0x31584D47, d.Width, d.Height, static_cast<unsigned>(d.Format) };
                bool ok = fwrite(header, sizeof(header), 1, file) == 1;
                for (UINT y = 0; y < d.Height && ok; ++y)
                    ok = fwrite(static_cast<const char *>(map.pData) + y * map.RowPitch, 4, d.Width, file) == d.Width;
                ok = fclose(file) == 0 && ok;
                if (!ok) DeleteFileA("gym-display.bin");
            }
            ctx->Unmap(stage, 0);
        }
        stage->Release();
    }
    dev->Release(); tex->Release();
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HMODULE modules[256]; DWORD bytes = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return FALSE;
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 256; ++i)
        {
            auto reg = reinterpret_cast<bool (*)(HMODULE, uint32_t)>(GetProcAddress(modules[i], "ReShadeRegisterAddon"));
            auto event = reinterpret_cast<void (*)(reshade::addon_event, void *)>(GetProcAddress(modules[i], "ReShadeRegisterEvent"));
            if (reg && event && reg(self, 18))
            {
                unregister_addon = reinterpret_cast<void (*)(HMODULE)>(GetProcAddress(modules[i], "ReShadeUnregisterAddon"));
                event(reshade::addon_event::reshade_finish_effects, reinterpret_cast<void *>(&Capture));
                return TRUE;
            }
        }
        return FALSE;
    }
    if (reason == DLL_PROCESS_DETACH && unregister_addon) unregister_addon(self);
    return TRUE;
}
