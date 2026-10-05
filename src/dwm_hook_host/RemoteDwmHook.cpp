#include "dwm_hook_host/RemoteDwmHook.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace bk::dwm_hook_host
{
    namespace
    {
        class DwmWaitError final : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

        bool EqualsInsensitive(const std::wstring_view lhs, const std::wstring_view rhs)
        {
            return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](const wchar_t a, const wchar_t b)
            {
                return std::towlower(a) == std::towlower(b);
            });
        }

        void ThrowLastError(const char* operation)
        {
            const DWORD error = GetLastError();
            throw std::runtime_error(std::string(operation) + " failed with Win32 error " + std::to_string(error));
        }
    }

    UniqueWindowsHandle::~UniqueWindowsHandle()
    {
        if (*this)
        {
            CloseHandle(handle_);
        }
    }

    UniqueWindowsHandle::UniqueWindowsHandle(UniqueWindowsHandle&& other) noexcept : handle_(other.Release()) {}

    UniqueWindowsHandle& UniqueWindowsHandle::operator=(UniqueWindowsHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (*this)
            {
                CloseHandle(handle_);
            }
            handle_ = other.Release();
        }
        return *this;
    }

    HANDLE UniqueWindowsHandle::Release() noexcept
    {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    bool EnableDebugPrivilege()
    {
        HANDLE rawToken{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken))
        {
            return false;
        }
        UniqueWindowsHandle token(rawToken);

        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid))
        {
            return false;
        }

        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!AdjustTokenPrivileges(token.Get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr))
        {
            return false;
        }
        return GetLastError() == ERROR_SUCCESS;
    }

    DWORD FindSessionDwm()
    {
        DWORD currentSession{};
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession))
        {
            ThrowLastError("ProcessIdToSessionId");
        }

        UniqueWindowsHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot)
        {
            ThrowLastError("CreateToolhelp32Snapshot(processes)");
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.Get(), &entry))
        {
            ThrowLastError("Process32FirstW");
        }
        do
        {
            if (!EqualsInsensitive(entry.szExeFile, L"dwm.exe"))
            {
                continue;
            }
            DWORD processSession{};
            if (ProcessIdToSessionId(entry.th32ProcessID, &processSession) && processSession == currentSession)
            {
                return entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot.Get(), &entry));

        throw std::runtime_error("No dwm.exe process was found in the current session");
    }

    uintptr_t FindRemoteModuleBase(const DWORD processId, const std::wstring_view moduleName)
    {
        UniqueWindowsHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId));
        if (!snapshot)
        {
            ThrowLastError("CreateToolhelp32Snapshot(modules)");
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot.Get(), &entry))
        {
            ThrowLastError("Module32FirstW");
        }
        do
        {
            if (EqualsInsensitive(entry.szModule, moduleName))
            {
                return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            }
        } while (Module32NextW(snapshot.Get(), &entry));
        return 0;
    }

    uintptr_t GetExportRva(const std::filesystem::path& modulePath, const char* exportName)
    {
        HMODULE module = LoadLibraryExW(modulePath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!module)
        {
            ThrowLastError("LoadLibraryExW(DONT_RESOLVE_DLL_REFERENCES)");
        }
        const FARPROC exportAddress = GetProcAddress(module, exportName);
        if (!exportAddress)
        {
            FreeLibrary(module);
            throw std::runtime_error(std::string("Export not found: ") + exportName);
        }
        const uintptr_t rva = reinterpret_cast<uintptr_t>(exportAddress) - reinterpret_cast<uintptr_t>(module);
        FreeLibrary(module);
        return rva;
    }

    uintptr_t GetLocalExportRva(const std::wstring_view moduleName, const char* exportName)
    {
        const std::wstring ownedName(moduleName);
        HMODULE module = GetModuleHandleW(ownedName.c_str());
        if (!module)
        {
            module = LoadLibraryW(ownedName.c_str());
        }
        if (!module)
        {
            ThrowLastError("LoadLibraryW(system module)");
        }
        const FARPROC exportAddress = GetProcAddress(module, exportName);
        if (!exportAddress)
        {
            throw std::runtime_error(std::string("System export not found: ") + exportName);
        }
        return reinterpret_cast<uintptr_t>(exportAddress) - reinterpret_cast<uintptr_t>(module);
    }

    RemoteDwmHook::RemoteDwmHook(const DWORD processId, std::filesystem::path path)
        : processId_(processId), path_(std::move(path))
    {
        constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
        process_ = UniqueWindowsHandle(OpenProcess(access, FALSE, processId_));
        if (!process_)
        {
            ThrowLastError("OpenProcess(dwm.exe)");
        }
    }

    uintptr_t RemoteDwmHook::RemoteSystemExport(const std::wstring_view moduleName, const char* exportName) const
    {
        const std::wstring ownedName(moduleName);
        HMODULE requestedModule = GetModuleHandleW(ownedName.c_str());
        if (!requestedModule)
        {
            requestedModule = LoadLibraryW(ownedName.c_str());
        }
        if (!requestedModule)
        {
            ThrowLastError("LoadLibraryW(system module)");
        }
        const FARPROC exportAddress = GetProcAddress(requestedModule, exportName);
        if (!exportAddress)
        {
            throw std::runtime_error(std::string("System export not found: ") + exportName);
        }

        HMODULE ownerModule{};
        if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(exportAddress),
            &ownerModule))
        {
            ThrowLastError("GetModuleHandleExW(export owner)");
        }
        std::wstring ownerPath(32768, L'\0');
        const DWORD ownerLength = GetModuleFileNameW(ownerModule, ownerPath.data(), static_cast<DWORD>(ownerPath.size()));
        if (!ownerLength || ownerLength == ownerPath.size())
        {
            ThrowLastError("GetModuleFileNameW(export owner)");
        }
        ownerPath.resize(ownerLength);
        const std::wstring ownerName = std::filesystem::path(ownerPath).filename().wstring();
        const uintptr_t remoteBase = FindRemoteModuleBase(processId_, ownerName);
        if (!remoteBase)
        {
            throw std::runtime_error("Required system module is not loaded in dwm.exe");
        }
        return remoteBase + (reinterpret_cast<uintptr_t>(exportAddress) - reinterpret_cast<uintptr_t>(ownerModule));
    }

    uintptr_t RemoteDwmHook::CallRemote(const uintptr_t functionAddress, const uintptr_t parameter, const std::wstring_view operation) const
    {
        UniqueWindowsHandle thread(CreateRemoteThread(
            process_.Get(), nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(functionAddress),
            reinterpret_cast<void*>(parameter), 0, nullptr));
        if (!thread)
        {
            ThrowLastError("CreateRemoteThread");
        }
        const DWORD waitResult=WaitForSingleObject(thread.Get(),15000);
        if(waitResult!=WAIT_OBJECT_0)
        {
            std::string label;
            label.reserve(operation.size());
            for(const wchar_t value:operation)
                label.push_back(value<=127?static_cast<char>(value):'?');
            throw DwmWaitError(
                label+
                (waitResult==WAIT_TIMEOUT
                    ? " timed out"
                    : " wait failed with Win32 error "+
                        std::to_string(GetLastError())));
        }
        DWORD exitCode{};
        if (!GetExitCodeThread(thread.Get(), &exitCode))
        {
            ThrowLastError("GetExitCodeThread");
        }
        return exitCode;
    }

    void RemoteDwmHook::UnloadChecked(const uintptr_t remoteBase) const
    {
        const uintptr_t freeLibrary =
            RemoteSystemExport(L"kernel32.dll", "FreeLibrary");
        if(!CallRemote(freeLibrary,remoteBase,L"FreeLibrary"))
            throw std::runtime_error("Remote FreeLibrary rejected the module");
        const ULONGLONG deadline=GetTickCount64()+5000;
        while(FindRemoteModuleBase(
            processId_,path_.filename().wstring()))
        {
            if(GetTickCount64()>=deadline)
                throw std::runtime_error(
                    "The remote module remained loaded after FreeLibrary");
            Sleep(10);
        }
    }

    bool RemoteDwmHook::IsLoaded() const
    {
        return FindRemoteModuleBase(
            processId_,path_.filename().wstring())!=0;
    }

    bool RemoteDwmHook::IsReady() const
    {
        const uintptr_t remoteBase=FindRemoteModuleBase(
            processId_,path_.filename().wstring());
        if(!remoteBase) return false;
        const uintptr_t readyAddress=remoteBase+
            GetExportRva(path_,"BkAwaitHookReady");
        return CallRemote(
            readyAddress,5000,L"BkAwaitHookReady")
            ==ERROR_SUCCESS;
    }

    bool RemoteDwmHook::LoadAndStart()
    {
        if(IsLoaded())
        {
            if(IsReady()) return false;
            StopAndUnload();
        }
        const std::wstring pathString = std::filesystem::absolute(path_).wstring();
        const size_t pathBytes = (pathString.size() + 1) * sizeof(wchar_t);
        void* remotePath = VirtualAllocEx(process_.Get(), nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remotePath)
        {
            ThrowLastError("VirtualAllocEx");
        }

        uintptr_t loadedModule{};
        try
        {
            if (!WriteProcessMemory(process_.Get(), remotePath, pathString.c_str(), pathBytes, nullptr))
            {
                ThrowLastError("WriteProcessMemory");
            }

            struct DwmLoad
            {
                uintptr_t loadLibraryW;
                uintptr_t getLastError;
                uintptr_t path;
                uintptr_t module;
                DWORD error;
                DWORD padding;
            };
            static_assert(offsetof(DwmLoad, module) == 24);
            static_assert(offsetof(DwmLoad, error) == 32);

            constexpr std::array<std::byte, 29> loadDllErrBytes{
                std::byte{0x53},
                std::byte{0x48}, std::byte{0x83}, std::byte{0xEC}, std::byte{0x20},
                std::byte{0x48}, std::byte{0x89}, std::byte{0xCB},
                std::byte{0x48}, std::byte{0x8B}, std::byte{0x4B}, std::byte{0x10},
                std::byte{0xFF}, std::byte{0x13},
                std::byte{0x48}, std::byte{0x89}, std::byte{0x43}, std::byte{0x18},
                std::byte{0xFF}, std::byte{0x53}, std::byte{0x08},
                std::byte{0x89}, std::byte{0x43}, std::byte{0x20},
                std::byte{0x48}, std::byte{0x83}, std::byte{0xC4}, std::byte{0x20},
                std::byte{0x5B},
            };
            constexpr std::byte returnOpcode{0xC3};
            DwmLoad request{
                RemoteSystemExport(L"kernel32.dll", "LoadLibraryW"),
                RemoteSystemExport(L"kernel32.dll", "GetLastError"),
                reinterpret_cast<uintptr_t>(remotePath),
            };
            void* remoteRequest = VirtualAllocEx(
                process_.Get(), nullptr, sizeof(request), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            void* remoteCode = VirtualAllocEx(
                process_.Get(), nullptr, loadDllErrBytes.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!remoteRequest || !remoteCode)
            {
                if (remoteRequest) VirtualFreeEx(process_.Get(), remoteRequest, 0, MEM_RELEASE);
                if (remoteCode) VirtualFreeEx(process_.Get(), remoteCode, 0, MEM_RELEASE);
                ThrowLastError("VirtualAllocEx(loader thunk)");
            }
            try
            {
                if (!WriteProcessMemory(process_.Get(), remoteRequest, &request, sizeof(request), nullptr) ||
                    !WriteProcessMemory(process_.Get(), remoteCode, loadDllErrBytes.data(), loadDllErrBytes.size(), nullptr) ||
                    !WriteProcessMemory(
                        process_.Get(),
                        static_cast<std::byte*>(remoteCode) + loadDllErrBytes.size(),
                        &returnOpcode,
                        sizeof(returnOpcode),
                        nullptr))
                {
                    ThrowLastError("WriteProcessMemory(loader thunk)");
                }
                DWORD previousProtection{};
                if (!VirtualProtectEx(
                        process_.Get(),
                        remoteCode,
                        loadDllErrBytes.size() + 1,
                        PAGE_EXECUTE_READ,
                        &previousProtection))
                {
                    ThrowLastError("VirtualProtectEx(loader thunk)");
                }
                FlushInstructionCache(process_.Get(), remoteCode, loadDllErrBytes.size() + 1);
                (void)CallRemote(
                    reinterpret_cast<uintptr_t>(remoteCode),
                    reinterpret_cast<uintptr_t>(remoteRequest),
                    L"diagnostic LoadLibraryW");
                if (!ReadProcessMemory(
                        process_.Get(), remoteRequest, &request, sizeof(request), nullptr))
                {
                    ThrowLastError("ReadProcessMemory(loader result)");
                }
            }
            catch(const DwmWaitError&)
            {
                throw;
            }
            catch (...)
            {
                VirtualFreeEx(process_.Get(), remoteCode, 0, MEM_RELEASE);
                VirtualFreeEx(process_.Get(), remoteRequest, 0, MEM_RELEASE);
                throw;
            }
            VirtualFreeEx(process_.Get(), remoteCode, 0, MEM_RELEASE);
            VirtualFreeEx(process_.Get(), remoteRequest, 0, MEM_RELEASE);

            loadedModule = request.module;
            if (!loadedModule)
            {
                throw std::runtime_error(
                    "Remote LoadLibraryW returned NULL with Win32 error " +
                    std::to_string(request.error));
            }
        }
        catch(const DwmWaitError&)
        {
            throw;
        }
        catch (...)
        {
            VirtualFreeEx(process_.Get(), remotePath, 0, MEM_RELEASE);
            throw;
        }
        VirtualFreeEx(process_.Get(), remotePath, 0, MEM_RELEASE);

        const uintptr_t remoteBase = FindRemoteModuleBase(processId_, path_.filename().wstring());
        if (!remoteBase)
        {
            throw std::runtime_error(
                "Remote LoadLibraryW returned module " + std::to_string(loadedModule) +
                ", but DwmHookModule.dll was not visible in the DWM module snapshot");
        }
        try
        {
            const uintptr_t startAddress = remoteBase +
                GetExportRva(path_, "BkStartHook");
            const uintptr_t result = CallRemote(
                startAddress, 0, L"BkStartHook");
            if (result != ERROR_SUCCESS)
                throw std::runtime_error(
                    "BkStartHook rejected initialization with code " +
                    std::to_string(result));
            const uintptr_t readyAddress = remoteBase +
                GetExportRva(path_, "BkAwaitHookReady");
            const uintptr_t readyResult = CallRemote(
                readyAddress, 5000, L"BkAwaitHookReady");
            if (readyResult != ERROR_SUCCESS)
            {
                const uintptr_t diagnosticsAddress = remoteBase +
                    GetExportRva(path_, "BkHookDiag");
                const uintptr_t diagnostics = CallRemote(
                    diagnosticsAddress, 0, L"BkHookDiag");
                const uintptr_t renderDiagAddr = remoteBase +
                    GetExportRva(
                        path_, "BkRenderDiag");
                const uintptr_t rendererDiagnostics = CallRemote(
                    renderDiagAddr, 0,
                    L"BkRenderDiag");
                throw std::runtime_error(
                    "The compositor hook loaded but did not produce a verified background frame (code " +
                    std::to_string(readyResult) + ", clear diagnostics " +
                    std::to_string(diagnostics >> 16) + "x" +
                    std::to_string(diagnostics & 0xffff) + ", renderer stage " +
                    std::to_string(rendererDiagnostics) + ")");
            }
        }
        catch(const DwmWaitError&)
        {
            throw;
        }
        catch(const std::exception& startError)
        {
            const std::string failure=startError.what();
            try
            {
                const uintptr_t stopAddress=remoteBase+
                    GetExportRva(path_,"BkStopHook");
                const uintptr_t stopResult=CallRemote(
                    stopAddress,0,L"BkStopHook(start rollback)");
                if(stopResult!=ERROR_SUCCESS)
                    throw std::runtime_error(
                        "BkStopHook returned "+
                        std::to_string(stopResult));
                UnloadChecked(remoteBase);
            }
            catch(const std::exception& rollbackError)
            {
                throw std::runtime_error(
                    failure+"; rollback left the module loaded: "+
                    rollbackError.what());
            }
            throw std::runtime_error(failure);
        }
        return true;
    }

    void RemoteDwmHook::StopAndUnload()
    {
        const uintptr_t remoteBase = FindRemoteModuleBase(processId_, path_.filename().wstring());
        if (!remoteBase)
        {
            return;
        }
        const uintptr_t stopAddress = remoteBase + GetExportRva(path_, "BkStopHook");
        const uintptr_t stopResult = CallRemote(stopAddress, 0, L"BkStopHook");
        if (stopResult != ERROR_SUCCESS)
        {
            throw std::runtime_error(
                "BkStopHook could not quiesce the hook; the module was left loaded (code " +
                std::to_string(stopResult) + ")");
        }
        UnloadChecked(remoteBase);
    }
}
