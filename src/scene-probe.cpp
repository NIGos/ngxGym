// Standalone diagnostic wrapper; production bridge uses the same implementation.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include "../../ngxbridge/scene-probe.hpp"
extern "C" __declspec(dllexport) const char *NAME = "Scene stage probe";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Read-only Vulkan stage observations";
static void (*unregister_addon)(HMODULE);
BOOL WINAPI DllMain(HMODULE self, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE modules[256]; DWORD bytes = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return FALSE;
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 256; ++i) {
            auto reg = reinterpret_cast<bool (*)(HMODULE, uint32_t)>(GetProcAddress(modules[i], "ReShadeRegisterAddon"));
            if (!reg || !reg(self, 18)) continue;
            unregister_addon = reinterpret_cast<void (*)(HMODULE)>(GetProcAddress(modules[i], "ReShadeUnregisterAddon"));
            if (scene_probe::Start(self, modules[i])) return TRUE;
            if (unregister_addon) unregister_addon(self);
            return FALSE;
        }
        return FALSE;
    }
    if (reason == DLL_PROCESS_DETACH) {
        if (unregister_addon) unregister_addon(self);
        scene_probe::Stop();
    }
    return TRUE;
}
