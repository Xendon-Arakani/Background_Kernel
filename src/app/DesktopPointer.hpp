#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cwchar>

namespace bk::app
{
    inline bool DesktopAt(const POINT point) noexcept
    {
        const HWND window=WindowFromPoint(point);
        if(!window) return false;
        const HWND root=GetAncestor(window,GA_ROOT);
        wchar_t name[64]{};
        GetClassNameW(root,name,64);
        return std::wcscmp(name,L"Progman")==0 ||
            std::wcscmp(name,L"WorkerW")==0;
    }

    inline bool BlankDesktop(const POINT point) noexcept
    {
        if(!DesktopAt(point)) return false;
        const HWND window=WindowFromPoint(point);
        wchar_t name[64]{};
        GetClassNameW(window,name,64);
        if(std::wcscmp(name,WC_LISTVIEWW)!=0) return true;
        DWORD processId{};
        GetWindowThreadProcessId(window,&processId);
        const HANDLE process=OpenProcess(PROCESS_VM_OPERATION|PROCESS_VM_WRITE,
            FALSE,processId);
        if(!process) return false;
        LVHITTESTINFO hit{};
        hit.pt=point;
        ScreenToClient(window,&hit.pt);
        void* remote=VirtualAllocEx(process,nullptr,sizeof(hit),
            MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        DWORD_PTR item{};
        const bool blank=remote && WriteProcessMemory(process,remote,&hit,
            sizeof(hit),nullptr) && SendMessageTimeoutW(window,LVM_HITTEST,0,
            reinterpret_cast<LPARAM>(remote),SMTO_ABORTIFHUNG|SMTO_BLOCK,
            50,&item) && static_cast<INT_PTR>(item)<0;
        if(remote) VirtualFreeEx(process,remote,0,MEM_RELEASE);
        CloseHandle(process);
        return blank;
    }

    inline LONG64 PackPointer(const POINT point) noexcept
    {
        return static_cast<LONG64>(static_cast<ULONGLONG>(
            static_cast<DWORD>(point.x)) |
            (static_cast<ULONGLONG>(static_cast<DWORD>(point.y))<<32));
    }

    inline POINT UnpackPointer(const LONG64 packed) noexcept
    {
        return {static_cast<LONG>(packed),
            static_cast<LONG>(static_cast<ULONGLONG>(packed)>>32)};
    }
}
