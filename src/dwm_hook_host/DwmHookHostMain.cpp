#include "common/DwmTarget.hpp"
#include "dwm_hook_host/RemoteDwmHook.hpp"

#include <windows.h>
#include <winver.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    class HookHostLock
    {
    public:
        HookHostLock()
        {
            mutex_=CreateMutexW(
                nullptr,FALSE,L"Local\\BkDwmHook");
            if(!mutex_)
                throw std::runtime_error("Creating DWM control ownership failed");
            const DWORD result=WaitForSingleObject(mutex_,30000);
            if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED)
            {
                CloseHandle(mutex_);
                mutex_=nullptr;
                throw std::runtime_error("DWM control ownership timed out");
            }
            owned_=true;
        }

        ~HookHostLock()
        {
            if(owned_) ReleaseMutex(mutex_);
            if(mutex_) CloseHandle(mutex_);
        }

        HookHostLock(const HookHostLock&)=delete;
        HookHostLock& operator=(const HookHostLock&)=delete;

    private:
        HANDLE mutex_{};
        bool owned_{};
    };

    void AppendHostLog(const std::string_view message)
    {
        wchar_t executable[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (!length || length == MAX_PATH)
        {
            return;
        }
        const std::filesystem::path logPath =
            std::filesystem::path(executable).parent_path() / L"DwmHookHost.log";
        std::ofstream log(logPath, std::ios::app);
        if (log)
        {
            log << "PID " << GetCurrentProcessId() << ": " << message << '\n';
        }
    }

    struct ExeVersion
    {
        DWORD major{};
        DWORD minor{};
        DWORD build{};
        DWORD revision{};
    };

    ExeVersion ReadFileVersion(const std::filesystem::path& path)
    {
        DWORD unused{};
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
        if (!size)
        {
            throw std::runtime_error("GetFileVersionInfoSizeW failed");
        }
        std::vector<std::byte> data(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        {
            throw std::runtime_error("GetFileVersionInfoW failed");
        }
        VS_FIXEDFILEINFO* info{};
        UINT infoSize{};
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || infoSize < sizeof(VS_FIXEDFILEINFO))
        {
            throw std::runtime_error("VerQueryValueW failed");
        }
        return {
            HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS),
        };
    }

    bool ValidateTargetBuild(const bool force)
    {
        using RtlVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rtlGetVersion = reinterpret_cast<RtlVersionFn>(
            ntdll ? GetProcAddress(ntdll, "RtlGetVersion") : nullptr);
        OSVERSIONINFOW osVersion{sizeof(osVersion)};
        if (!rtlGetVersion || rtlGetVersion(&osVersion) != 0)
        {
            throw std::runtime_error("RtlGetVersion failed");
        }
        DWORD revision{};
        DWORD revisionSize = sizeof(revision);
        if (RegGetValueW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                L"UBR",
                RRF_RT_REG_DWORD,
                nullptr,
                &revision,
                &revisionSize) != ERROR_SUCCESS)
        {
            throw std::runtime_error("Reading the Windows UBR failed");
        }

        const ExeVersion dwmCore = ReadFileVersion(L"C:\\Windows\\System32\\dwmcore.dll");
        AppendHostLog(
            "version probe: Windows " + std::to_string(osVersion.dwMajorVersion) + "." +
            std::to_string(osVersion.dwMinorVersion) + "." +
            std::to_string(osVersion.dwBuildNumber) + "." + std::to_string(revision) +
            ", dwmcore " + std::to_string(dwmCore.major) + "." +
            std::to_string(dwmCore.minor) + "." + std::to_string(dwmCore.build) + "." +
            std::to_string(dwmCore.revision) + ", expected Windows " +
            std::to_string(bk::target::RequiredOsBuild) + "." +
            std::to_string(bk::target::RequiredOsRevision) + ", dwmcore " +
            std::to_string(bk::target::RequiredDwmBuild) + "." +
            std::to_string(bk::target::RequiredDwmFixedRev));
        const bool osValid = osVersion.dwMajorVersion == 10 && osVersion.dwMinorVersion == 0 &&
            osVersion.dwBuildNumber == bk::target::RequiredOsBuild && revision == bk::target::RequiredOsRevision;
        const bool dwmCoreValid = dwmCore.major == 6 && dwmCore.minor == 2 &&
            dwmCore.build == bk::target::RequiredDwmBuild && dwmCore.revision == bk::target::RequiredDwmFixedRev;
        if (!osValid || !dwmCoreValid)
        {
            std::wcerr << L"verified=false windows="
                       << osVersion.dwMajorVersion << L'.' << osVersion.dwMinorVersion << L'.'
                       << osVersion.dwBuildNumber << L'.' << revision << L" dwmcore="
                       << dwmCore.major << L'.' << dwmCore.minor << L'.' << dwmCore.build << L'.'
                       << dwmCore.revision << L'\n';
            if (!force)
            {
                std::wcerr << L"expected_windows=10.0." << bk::target::RequiredOsBuild << L'.'
                           << bk::target::RequiredOsRevision << L" expected_dwmcore=6.2."
                           << bk::target::RequiredDwmBuild << L'.' << bk::target::RequiredDwmFixedRev
                           << L'\n';
                return false;
            }
            std::wcerr << L"version_override=true\n";
        }
        return true;
    }

    std::filesystem::path ModuleBesideHost()
    {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length == executable.size())
        {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        executable.resize(length);
        return std::filesystem::path(executable).parent_path() / L"DwmHookModule.dll";
    }

    std::filesystem::path StagedModulePath(const std::filesystem::path& source, const bool stage)
    {
        std::wstring programData(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"ProgramData",
            programData.data(),
            static_cast<DWORD>(programData.size()));
        if (!length || length >= programData.size())
        {
            throw std::runtime_error("ProgramData is unavailable");
        }
        programData.resize(length);
        const std::filesystem::path directory =
            std::filesystem::path(programData) / L"BackgroundKernel";
        const std::filesystem::path destination = directory / source.filename();
        if (stage)
        {
            std::filesystem::create_directories(directory);
            std::filesystem::copy_file(
                source,
                destination,
                std::filesystem::copy_options::overwrite_existing);
        }
        return destination;
    }

    struct WorkerSearch
    {
        HWND iconHost{};
    };

    BOOL CALLBACK FindIconHost(HWND window,LPARAM state)
    {
        if(!FindWindowExW(window,nullptr,L"SHELLDLL_DefView",nullptr))
            return TRUE;
        static_cast<WorkerSearch*>(
            reinterpret_cast<void*>(state))->iconHost=window;
        return FALSE;
    }

    HWND FindChildWorker(HWND progman)
    {
        HWND worker{};
        HWND best{};
        long long bestArea{};
        while((worker=FindWindowExW(
            progman,worker,L"WorkerW",nullptr)))
        {
            RECT bounds{};
            if(!GetWindowRect(worker,&bounds)) continue;
            const long long area=static_cast<long long>(
                bounds.right-bounds.left)*(bounds.bottom-bounds.top);
            if(area>bestArea)
            {
                best=worker;
                bestArea=area;
            }
        }
        return best;
    }

    HWND FindWallpaperWorker()
    {
        const HWND progman = FindWindowW(L"Progman", nullptr);
        if(!progman) return nullptr;
        if(const HWND child=FindChildWorker(progman)) return child;
        WorkerSearch search;
        EnumWindows(FindIconHost,reinterpret_cast<LPARAM>(&search));
        if(!search.iconHost) return nullptr;
        return FindWindowExW(
            nullptr,search.iconHost,L"WorkerW",nullptr);
    }

    HWND EnsureWallpaper()
    {
        const HWND progman=FindWindowW(L"Progman",nullptr);
        if(!progman) return nullptr;
        DWORD_PTR result{};
        SendMessageTimeoutW(
            progman,0x052C,0xD,1,SMTO_ABORTIFHUNG,2000,&result);
        const ULONGLONG deadline=GetTickCount64()+2000;
        do
        {
            if(const HWND worker=FindWallpaperWorker()) return worker;
            Sleep(10);
        } while(GetTickCount64()<deadline);
        return nullptr;
    }

    void RestoreWallpaper()
    {
        const HWND worker = FindWallpaperWorker();
        if (worker && !IsWindowVisible(worker))
        {
            ShowWindowAsync(worker, SW_SHOW);
        }
        const HWND progman = FindWindowW(L"Progman", nullptr);
        if (progman)
        {
            RedrawWindow(
                progman,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_INTERNALPAINT | RDW_ALLCHILDREN);
        }
    }

    int WatchModule(const DWORD processId)
    {
        SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
        const std::wstring moduleName = ModuleBesideHost().filename().wstring();
        for (;;)
        {
            try
            {
                if (!bk::dwm_hook_host::FindRemoteModuleBase(processId, moduleName))
                {
                    break;
                }
            }
            catch (...)
            {
                break;
            }
            Sleep(500);
        }
        RestoreWallpaper();
        return 0;
    }

    void StartWatchdog(const DWORD processId)
    {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            executable.data(),
            static_cast<DWORD>(executable.size()));
        if (!length || length == executable.size())
        {
            throw std::runtime_error("GetModuleFileNameW(watchdog) failed");
        }
        executable.resize(length);
        std::wstring commandLine = L"\"" + executable + L"\" --watch " +
            std::to_wstring(processId);
        STARTUPINFOW startupInfo{sizeof(startupInfo)};
        PROCESS_INFORMATION processInformation{};
        if (!CreateProcessW(
                executable.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startupInfo,
                &processInformation))
        {
            throw std::runtime_error(
                "CreateProcessW(watchdog) failed with Win32 error " +
                std::to_string(GetLastError()));
        }
        CloseHandle(processInformation.hThread);
        CloseHandle(processInformation.hProcess);
    }

    void LogDwmMitigations(const DWORD processId)
    {
        const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
        if (!process)
        {
            AppendHostLog("could not query DWM mitigations: " + std::to_string(GetLastError()));
            return;
        }
        PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
        PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
        const bool signatureOk = GetProcessMitigationPolicy(
            process, ProcessSignaturePolicy, &signature, sizeof(signature));
        const bool dynamicCodeOk = GetProcessMitigationPolicy(
            process, ProcessDynamicCodePolicy, &dynamicCode, sizeof(dynamicCode));
        const bool imageLoadOk = GetProcessMitigationPolicy(
            process, ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad));
        CloseHandle(process);
        AppendHostLog(
            "DWM mitigations: signature=" +
            std::to_string(signatureOk ? signature.Flags : UINT32_MAX) +
            ", dynamicCode=" + std::to_string(dynamicCodeOk ? dynamicCode.Flags : UINT32_MAX) +
            ", imageLoad=" + std::to_string(imageLoadOk ? imageLoad.Flags : UINT32_MAX));
    }
}

int wmain(const int argumentCount, wchar_t** arguments)
{
    try
    {
        AppendHostLog("process entered");
        bool start = false;
        bool stop = false;
        bool forceVersion = false;
        bool unsafeHookAllowed = false;
        DWORD watchedProcess{};
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::wstring_view argument(arguments[index]);
            start |= argument == L"--start";
            stop |= argument == L"--stop";
            forceVersion |= argument == L"--force-version";
            unsafeHookAllowed |= argument == L"--allow-unsafe-display-hook";
            if (argument == L"--watch" && index + 1 < argumentCount)
            {
                watchedProcess = static_cast<DWORD>(std::stoul(arguments[++index]));
            }
        }
        if (watchedProcess)
        {
            AppendHostLog("watchdog entered");
            (void)bk::dwm_hook_host::EnableDebugPrivilege();
            return WatchModule(watchedProcess);
        }
        HookHostLock control;
        if (start == stop)
        {
            std::wcout << L"DwmHookHost --start|--stop [--force-version]\n";
            return 2;
        }
        if (start && !unsafeHookAllowed)
        {
            std::wcerr << L"dwm_hook=disabled\n";
            return 7;
        }

        const std::filesystem::path sourceModulePath = ModuleBesideHost();
        const std::filesystem::path stagedModulePath =
            StagedModulePath(sourceModulePath,false);
        const std::filesystem::path killSwitch = sourceModulePath.parent_path() / L"BackgroundKernel.disabled";
        if (start && std::filesystem::exists(killSwitch))
        {
            std::wcerr << L"kill_switch=" << killSwitch << L'\n';
            return 3;
        }
        if (start && !std::filesystem::exists(sourceModulePath))
        {
            std::wcerr << L"module_missing=" << sourceModulePath << L'\n';
            return 4;
        }
        if(stop && !std::filesystem::exists(sourceModulePath) &&
            !std::filesystem::exists(stagedModulePath))
        {
            std::wcerr << L"local_module=false\n";
            return 4;
        }
        if (start && !ValidateTargetBuild(forceVersion))
        {
            return 5;
        }
        if(start && !EnsureWallpaper())
            throw std::runtime_error("Explorer wallpaper host is unavailable");

        if (!bk::dwm_hook_host::EnableDebugPrivilege())
        {
            std::wcerr << L"debug_privilege=false\n";
            return 6;
        }
        AppendHostLog("debug privilege enabled");

        const DWORD dwmProcess = bk::dwm_hook_host::FindSessionDwm();
        AppendHostLog("target DWM located");
        LogDwmMitigations(dwmProcess);
        if (start)
        {
            if(std::filesystem::exists(stagedModulePath))
            {
                bk::dwm_hook_host::RemoteDwmHook prior(dwmProcess,stagedModulePath);
                if(prior.IsReady())
                {
                    AppendHostLog("existing module is ready");
                    std::wcout << L"dwm_hook=active pid=" << dwmProcess << L'\n';
                    return 0;
                }
                if(prior.IsLoaded()) prior.StopAndUnload();
            }
            const auto modulePath=StagedModulePath(sourceModulePath,true);
            AppendHostLog("using machine-readable staged module");
            bk::dwm_hook_host::RemoteDwmHook module(dwmProcess,modulePath);
            const bool loadedNow=module.LoadAndStart();
            AppendHostLog("module rendered a verified frame");
            if(loadedNow) try
            {
                StartWatchdog(dwmProcess);
                AppendHostLog("watchdog launched");
            }
            catch (...)
            {
                module.StopAndUnload();
                throw;
            }
            std::wcout << L"dwm_hook=loaded pid=" << dwmProcess << L'\n';
        }
        else
        {
            const std::filesystem::path modulePath =
                std::filesystem::exists(stagedModulePath)
                    ? stagedModulePath
                    : sourceModulePath;
            AppendHostLog("using machine-readable staged module");
            bk::dwm_hook_host::RemoteDwmHook module(dwmProcess,modulePath);
            module.StopAndUnload();
            std::error_code removalError;
            std::filesystem::remove(stagedModulePath, removalError);
            std::wcout << L"dwm_hook=unloaded pid=" << dwmProcess << L'\n';
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        AppendHostLog(std::string("ERROR: ") + error.what());
        std::cerr << "DwmHookHost: " << error.what() << '\n';
        return 1;
    }
}
