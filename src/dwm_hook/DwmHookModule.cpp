#include "dwm_hook/OutputHook.hpp"

#include <windows.h>

#include <cstdint>

namespace
{
    HMODULE moduleInstance{};
}

extern "C" __declspec(dllexport) DWORD WINAPI BkStartHook(void*)
{
    return bk::dwm_hook::StartOutputHook();
}

extern "C" __declspec(dllexport) DWORD WINAPI BkStopHook(void*)
{
    return bk::dwm_hook::StopOutputHook();
}

extern "C" __declspec(dllexport) DWORD WINAPI BkAwaitHookReady(void* parameter)
{
    const auto timeout = static_cast<DWORD>(reinterpret_cast<uintptr_t>(parameter));
    return bk::dwm_hook::AwaitHookReady(timeout);
}

extern "C" __declspec(dllexport) DWORD WINAPI BkHookDiag(void*)
{
    return bk::dwm_hook::GetHookDiag();
}

extern "C" __declspec(dllexport) DWORD WINAPI BkRenderDiag(void*)
{
    return bk::dwm_hook::GetRenderDiag();
}

BOOL WINAPI DllMain(const HMODULE instance, const DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        moduleInstance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
