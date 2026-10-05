#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace bk::dwm_hook_host
{
    class UniqueWindowsHandle
    {
    public:
        UniqueWindowsHandle() = default;
        explicit UniqueWindowsHandle(HANDLE handle) noexcept : handle_(handle) {}
        ~UniqueWindowsHandle();

        UniqueWindowsHandle(const UniqueWindowsHandle&) = delete;
        UniqueWindowsHandle& operator=(const UniqueWindowsHandle&) = delete;
        UniqueWindowsHandle(UniqueWindowsHandle&& other) noexcept;
        UniqueWindowsHandle& operator=(UniqueWindowsHandle&& other) noexcept;

        [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
        [[nodiscard]] explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
        [[nodiscard]] HANDLE Release() noexcept;

    private:
        HANDLE handle_{};
    };

    [[nodiscard]] DWORD FindSessionDwm();
    [[nodiscard]] uintptr_t FindRemoteModuleBase(DWORD processId, std::wstring_view moduleName);
    [[nodiscard]] uintptr_t GetExportRva(const std::filesystem::path& modulePath, const char* exportName);
    [[nodiscard]] uintptr_t GetLocalExportRva(std::wstring_view moduleName, const char* exportName);
    [[nodiscard]] bool EnableDebugPrivilege();

    class RemoteDwmHook
    {
    public:
        RemoteDwmHook(DWORD processId, std::filesystem::path path);

        bool IsLoaded() const;
        bool IsReady() const;
        bool LoadAndStart();
        void StopAndUnload();

    private:
        [[nodiscard]] uintptr_t CallRemote(uintptr_t functionAddress, uintptr_t parameter, std::wstring_view operation) const;
        [[nodiscard]] uintptr_t RemoteSystemExport(std::wstring_view moduleName, const char* exportName) const;
        void UnloadChecked(uintptr_t remoteBase) const;

        DWORD processId_{};
        std::filesystem::path path_;
        UniqueWindowsHandle process_;
    };
}
