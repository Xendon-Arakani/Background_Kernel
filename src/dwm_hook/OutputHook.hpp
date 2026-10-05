#pragma once

#include <windows.h>

namespace bk::dwm_hook
{
    DWORD StartOutputHook();
    DWORD StopOutputHook();
    DWORD AwaitHookReady(DWORD timeoutMs);
    DWORD GetHookDiag();
    DWORD GetRenderDiag();
}
