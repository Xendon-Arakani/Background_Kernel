#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include "app/SceneSettingsPanel.hpp"
#include "app/SceneSignals.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace bk::app::surface
{
    constexpr DWORD StartTimeoutMs = 30000;
    constexpr DWORD StopTimeoutMs = 10000;
    constexpr DWORD KillTimeoutMs = 5000;

    struct SceneSpec
    {
        const wchar_t* exeName{};
        const wchar_t* startArg{};
        const wchar_t* stopName{};
        const wchar_t* readyName{};
        const wchar_t* title{};
    };

    constexpr SceneSpec LegacyScene{
        L"SceneKernel.exe",
        L"--render-worker",
        bk::app::surface::RenderStop,
        bk::app::surface::RenderReady,
        L"Desktop scene"};
    constexpr SceneSpec AudioSceneProcess{
        L"AudioScene.exe",
        L"--visualizer-worker",
        bk::app::surface::VisualStop,
        bk::app::surface::VisualReady,
        L"Audio scene"};
    constexpr SceneSpec SceneKernelProcess{
        L"SceneKernel.exe",
        L"--kernel-worker",
        KernelStop,
        KernelReady,
        L"Scene kernel"};
    constexpr SceneSpec WallStrokeProcess{
        L"StrokeLab.exe",
        L"--wall-stroke-worker",
        WallStrokeStop,
        WallStrokeReady,
        L"Wall stroke"};

    class OwnerJob
    {
    public:
        OwnerJob() = default;
        OwnerJob(const OwnerJob&) = delete;
        OwnerJob& operator=(const OwnerJob&) = delete;
        ~OwnerJob() { Reset(); }
        bool Create()
        {
            if(handle_) return true;
            handle_=CreateJobObjectW(nullptr,nullptr);
            if(!handle_) return false;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags=
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if(SetInformationJobObject(handle_,
                JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
                return true;
            const DWORD error=GetLastError();
            Reset();
            SetLastError(error);
            return false;
        }
        void Reset() noexcept
        {
            if(handle_) CloseHandle(handle_);
            handle_=nullptr;
        }
        HANDLE Get() const noexcept { return handle_; }
    private:
        HANDLE handle_{};
    };

    inline bool SpawnOwned(
        const std::wstring& executable,
        std::wstring& command,
        HANDLE job,
        PROCESS_INFORMATION& process)
    {
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        if(!bytes) return false;
        std::vector<BYTE> storage(bytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb=sizeof(startup);
        startup.lpAttributeList=
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if(!InitializeProcThreadAttributeList(
            startup.lpAttributeList,1,0,&bytes)) return false;
        const bool configured=UpdateProcThreadAttribute(
            startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &job,sizeof(job),nullptr,nullptr)!=FALSE;
        const bool created=configured && CreateProcessW(
            executable.c_str(),command.data(),nullptr,nullptr,FALSE,
            CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,
            nullptr,nullptr,&startup.StartupInfo,&process)!=FALSE;
        const DWORD error=GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if(!created) SetLastError(error);
        return created;
    }

    inline bool SignalScene(const wchar_t* name)
    {
        const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (!event)
        {
            return false;
        }
        const bool signaled = SetEvent(event) != FALSE;
        CloseHandle(event);
        return signaled;
    }

    inline bool SceneEventExists(const wchar_t* name)
    {
        const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (!event)
        {
            return false;
        }
        CloseHandle(event);
        return true;
    }

    inline std::filesystem::path ControlExePath()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path);
    }

    inline std::wstring SceneExePath(const SceneSpec& spec)
    {
        auto path = ControlExePath();
        if (path.empty())
        {
            return {};
        }
        path.replace_filename(spec.exeName);
        return path.wstring();
    }

    inline HANDLE OpenScene(const std::wstring& path)
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return nullptr;
        }
        const auto name = std::filesystem::path(path).filename().wstring();
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        HANDLE match{};
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, name.c_str()) != 0)
                {
                    continue;
                }
                const HANDLE process = OpenProcess(
                    SYNCHRONIZE | PROCESS_TERMINATE |
                        PROCESS_QUERY_LIMITED_INFORMATION,
                    FALSE,
                    entry.th32ProcessID);
                if (!process)
                {
                    continue;
                }
                std::wstring actual(32768, L'\0');
                DWORD length = static_cast<DWORD>(actual.size());
                if (QueryFullProcessImageNameW(
                        process, 0, actual.data(), &length))
                {
                    actual.resize(length);
                    if (_wcsicmp(actual.c_str(), path.c_str()) == 0)
                    {
                        match = process;
                        break;
                    }
                }
                CloseHandle(process);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return match;
    }

    inline void RefreshScene(HANDLE& process)
    {
        if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(process);
            process = nullptr;
        }
    }

    inline bool SceneActive(const SceneSpec& spec, HANDLE& process)
    {
        RefreshScene(process);
        if (!process)
        {
            const auto path = SceneExePath(spec);
            if (!path.empty())
            {
                process = OpenScene(path);
            }
        }
        return process || SceneEventExists(spec.stopName);
    }

    inline bool StopScene(const SceneSpec& spec, HANDLE& saved)
    {
        RefreshScene(saved);
        const auto path = SceneExePath(spec);
        SignalScene(spec.stopName);
        HANDLE process = saved;
        saved = nullptr;
        if (!process && !path.empty())
        {
            process = OpenScene(path);
        }
        const ULONGLONG deadline = GetTickCount64() + StopTimeoutMs;
        for (;;)
        {
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(process);
                process = nullptr;
            }
            if (!process && !path.empty())
            {
                process = OpenScene(path);
            }
            if (!process && !SceneEventExists(spec.stopName))
            {
                return true;
            }
            if (GetTickCount64() >= deadline)
            {
                break;
            }
            SignalScene(spec.stopName);
            Sleep(10);
        }
        if (process)
        {
            TerminateProcess(process, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process, KillTimeoutMs);
            CloseHandle(process);
            process = nullptr;
        }
        for (int pass = 0; pass < 4 && !path.empty(); ++pass)
        {
            process = OpenScene(path);
            if (!process)
            {
                break;
            }
            TerminateProcess(process, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process, KillTimeoutMs);
            CloseHandle(process);
            process = nullptr;
        }
        const ULONGLONG closeBy = GetTickCount64() + KillTimeoutMs;
        while (SceneEventExists(spec.stopName) && GetTickCount64() < closeBy)
        {
            Sleep(10);
        }
        const HANDLE left = path.empty() ? nullptr : OpenScene(path);
        if (left)
        {
            CloseHandle(left);
        }
        return !left && !SceneEventExists(spec.stopName);
    }

    inline void ShowSceneError(
        const SceneSpec& spec,
        const std::wstring& reason,
        bk::app::SceneSettingsPanel& panel)
    {
        const std::wstring message =
            std::wstring(spec.title)+L": "+reason;
        OutputDebugStringW(message.c_str());
        panel.ShowError(message);
    }

    inline bool StartScene(
        const SceneSpec& spec,
        HANDLE& saved,
        bk::app::SceneSettingsPanel& panel,
        HANDLE job,
        HANDLE cancel)
    {
        RefreshScene(saved);
        const auto executable = SceneExePath(spec);
        if (executable.empty())
        {
            ShowSceneError(
                spec,L"The executable path is unavailable.",panel);
            return false;
        }
        HANDLE existing = OpenScene(executable);
        if (saved || existing)
        {
            if(existing) CloseHandle(existing);
            ShowSceneError(spec,
                L"The process must finish stopping before it can start.",panel);
            return false;
        }
        const HANDLE ready = CreateEventW(
            nullptr, TRUE, FALSE, spec.readyName);
        if (!ready)
        {
            ShowSceneError(
                spec,
                L"Windows could not create the readiness signal. Error " +
                    std::to_wstring(GetLastError()) + L".",
                panel);
            return false;
        }
        ResetEvent(ready);
        std::wstring command = L"\"" + executable + L"\" " + spec.startArg;
        PROCESS_INFORMATION process{};
        if (!SpawnOwned(executable,command,job,process))
        {
            const DWORD error = GetLastError();
            CloseHandle(ready);
            ShowSceneError(
                spec,
                L"Windows could not create the process. Error " +
                    std::to_wstring(error) + L".",
                panel);
            return false;
        }
        CloseHandle(process.hThread);
        const HANDLE waits[]{cancel,process.hProcess,ready};
        const DWORD result=WaitForMultipleObjects(
            3,waits,FALSE,StartTimeoutMs);
        CloseHandle(ready);
        if (result == WAIT_OBJECT_0 + 2 &&
            WaitForSingleObject(cancel,0)==WAIT_TIMEOUT &&
            WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
        {
            saved = process.hProcess;
            return true;
        }
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(process.hProcess, &exitCode);
        saved = process.hProcess;
        StopScene(spec, saved);
        if(result==WAIT_OBJECT_0) return false;
        if (result == WAIT_TIMEOUT)
        {
            ShowSceneError(
                spec,L"Startup timed out and was rolled back.",panel);
        }
        else
        {
            ShowSceneError(
                spec,
                L"The worker exited with code " +
                    std::to_wstring(exitCode) + L".",
                panel);
        }
        return false;
    }

}
