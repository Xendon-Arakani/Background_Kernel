#include "app/SceneProcess.hpp"
#include "app/DesktopBackdrop.hpp"

#include <limits>
#include <stdexcept>

namespace
{
    using namespace bk::app::surface;

    class SceneKernelSession
    {
    public:
        ~SceneKernelSession() { Shutdown(); }

        int Run()
        {
            owner_=CreateMutexW(nullptr,FALSE,KernelOwner);
            if(!owner_) throw std::runtime_error("Kernel ownership failed");
            const DWORD claimed=WaitForSingleObject(owner_,0);
            if(claimed!=WAIT_OBJECT_0 && claimed!=WAIT_ABANDONED)
                return ERROR_BUSY;
            owned_=true;
            stop_=CreateEventW(nullptr,TRUE,FALSE,KernelStop);
            if(!stop_ || GetLastError()==ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Kernel stop signal unavailable");
            if(!backdrop_.Black())
                throw std::runtime_error("Desktop background control failed");
            backdropTick_=CreateWaitableTimerW(nullptr,FALSE,nullptr);
            if(!backdropTick_)
                throw std::runtime_error("Desktop background timer failed");
            toggle_=CreateSemaphoreW(nullptr,0,
                std::numeric_limits<LONG>::max(),SceneToggle);
            if(!toggle_ || GetLastError()==ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Scene toggle queue unavailable");
            strokeToggle_=CreateSemaphoreW(nullptr,0,
                std::numeric_limits<LONG>::max(),WallStrokeToggle);
            if(!strokeToggle_ || GetLastError()==ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Wall stroke toggle queue unavailable");
            ready_=CreateEventW(nullptr,TRUE,FALSE,KernelReady);
            if(!ready_ || !childJob_.Create())
                throw std::runtime_error("Kernel child ownership failed");
            ResetEvent(ready_);
            if(!StopScene(AudioSceneProcess,scene_))
                throw std::runtime_error("Prior scene did not stop");
            if(!StopScene(WallStrokeProcess,stroke_))
                throw std::runtime_error("Prior wall stroke did not stop");
            if(WaitForSingleObject(stop_,0)!=WAIT_TIMEOUT) return 0;
            StartBackdrop();
            SetEvent(ready_);
            for(;;)
            {
                RefreshScene(scene_);
                RefreshScene(stroke_);
                if(scene_ || stroke_) StopBackdrop();
                else StartBackdrop();
                HANDLE handles[]{stop_,toggle_,strokeToggle_,backdropTick_,nullptr,nullptr};
                DWORD count=4;
                if(scene_) handles[count++]=scene_;
                if(stroke_) handles[count++]=stroke_;
                const DWORD result=MsgWaitForMultipleObjects(
                    count,handles,FALSE,INFINITE,QS_ALLINPUT);
                if(result==WAIT_OBJECT_0) break;
                if(result==WAIT_FAILED)
                    throw std::runtime_error("Kernel message wait failed");
                if(result==WAIT_OBJECT_0+1)
                {
                    if(WaitForSingleObject(stop_,0)==WAIT_OBJECT_0) break;
                    if(SceneActive(AudioSceneProcess,scene_))
                    {
                        StartBackdrop();
                        if(!StopScene(AudioSceneProcess,scene_))
                            throw std::runtime_error("Scene did not stop");
                    }
                    else
                    {
                        StartBackdrop();
                        if(!StopScene(WallStrokeProcess,stroke_))
                            ShowSceneError(WallStrokeProcess,
                                L"The workshop must finish stopping before scene 8 can start.",panel_);
                        else if(WaitForSingleObject(stop_,0)==WAIT_TIMEOUT)
                        {
                            if(!backdrop_.PrepareScene())
                                throw std::runtime_error("Desktop scene plane failed");
                            if(!StartScene(
                                AudioSceneProcess,scene_,panel_,
                                childJob_.Get(),stop_))
                            {
                                if(!backdrop_.CoverScene())
                                    throw std::runtime_error("Desktop background restore failed");
                            }
                        }
                    }
                }
                else if(result==WAIT_OBJECT_0+2)
                {
                    if(WaitForSingleObject(stop_,0)==WAIT_OBJECT_0) break;
                    if(SceneActive(WallStrokeProcess,stroke_))
                    {
                        StartBackdrop();
                        if(!StopScene(WallStrokeProcess,stroke_))
                            throw std::runtime_error("Wall stroke did not stop");
                    }
                    else
                    {
                        StartBackdrop();
                        if(!StopScene(AudioSceneProcess,scene_))
                            ShowSceneError(AudioSceneProcess,
                                L"Scene 8 must finish stopping before the workshop can start.",panel_);
                        else if(WaitForSingleObject(stop_,0)==WAIT_TIMEOUT)
                        {
                            if(!backdrop_.PrepareScene())
                                throw std::runtime_error("Desktop scene plane failed");
                            if(!StartScene(
                                WallStrokeProcess,stroke_,panel_,
                                childJob_.Get(),stop_))
                            {
                                if(!backdrop_.CoverScene())
                                    throw std::runtime_error("Desktop background restore failed");
                            }
                        }
                    }
                }
                else if(result==WAIT_OBJECT_0+3)
                    backdrop_.Advance();
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))
                {
                    if(message.message==WM_QUIT) return 0;
                    if(!panel_.Handle(message))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
            return 0;
        }

    private:
        void StartBackdrop()
        {
            if(backdropMoving_) return;
            if(!backdrop_.CoverScene())
                throw std::runtime_error("Desktop background disable failed");
            backdrop_.Restart();
            LARGE_INTEGER first{};
            first.QuadPart=-330000;
            if(!SetWaitableTimer(
                backdropTick_,&first,33,nullptr,nullptr,FALSE))
                throw std::runtime_error("Desktop background timer failed");
            backdropMoving_=true;
        }

        void StopBackdrop()
        {
            if(!backdropMoving_) return;
            CancelWaitableTimer(backdropTick_);
            backdrop_.RevealScene();
            backdropMoving_=false;
        }

        void Shutdown() noexcept
        {
            if(owned_)
            {
                if(ready_) ResetEvent(ready_);
                try { StopScene(AudioSceneProcess,scene_); }
                catch(...) {}
                try { StopScene(WallStrokeProcess,stroke_); }
                catch(...) {}
            }
            childJob_.Reset();
            if(backdropTick_) CancelWaitableTimer(backdropTick_);
            backdrop_.Restore();
            if(scene_) CloseHandle(scene_);
            if(stroke_) CloseHandle(stroke_);
            if(strokeToggle_) CloseHandle(strokeToggle_);
            if(ready_) CloseHandle(ready_);
            if(toggle_) CloseHandle(toggle_);
            if(stop_) CloseHandle(stop_);
            if(backdropTick_) CloseHandle(backdropTick_);
            if(owned_) ReleaseMutex(owner_);
            if(owner_) CloseHandle(owner_);
        }

        HANDLE owner_{},stop_{},ready_{},toggle_{},scene_{};
        HANDLE strokeToggle_{},stroke_{};
        HANDLE backdropTick_{};
        bool owned_{},backdropMoving_{};
        OwnerJob childJob_;
        bk::app::DesktopBackdrop backdrop_;
        bk::app::SceneSettingsPanel panel_;
    };
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR commandLine,int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(commandLine && wcsstr(commandLine,L"--stop"))
    {
        SignalScene(KernelStop);
        return 0;
    }
    try
    {
        SceneKernelSession kernel;
        return kernel.Run();
    }
    catch(const std::exception& error)
    {
        OutputDebugStringA(error.what());
        if(!commandLine || !wcsstr(commandLine,L"--kernel-worker"))
            MessageBoxA(nullptr,error.what(),
                "Kernel failed",MB_OK|MB_ICONERROR);
        return 1;
    }
}
