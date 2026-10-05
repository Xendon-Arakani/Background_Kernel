#include <windows.h>
#include <tlhelp32.h>
#include "visualizer/SceneSettings.hpp"
#include "app/SceneSettingsPanel.hpp"
#include "app/SceneProcess.hpp"
#include "app/DesktopPointer.hpp"
#include "stroke/RayOrigin.hpp"

#include <filesystem>
#include <string>

namespace
{
    using namespace bk::app::surface;
    constexpr wchar_t ControlStopName[] =
        L"Local\\BkKeyStop";
    constexpr UINT KernelInput=WM_APP;
    constexpr UINT SceneInput=WM_APP+1;
    constexpr UINT SettingsInput=WM_APP+2;
    constexpr UINT CenterInput=WM_APP+3;
    constexpr UINT StrokeInput=WM_APP+4;
    constexpr UINT OriginScroll=WM_APP+5;
    constexpr DWORD KernelScan=0x49;
    constexpr DWORD SceneScan=0x48;
    constexpr DWORD SettingsScan=0x50;
    constexpr DWORD StrokeScan=0x47;
    class KeypadApp
    {
    public:
        ~KeypadApp()
        {
            Shutdown();
        }

        int Run()
        {
            stopEvent_ = CreateEventW(
                nullptr, TRUE, FALSE, ControlStopName);
            if (!stopEvent_ || GetLastError() == ERROR_ALREADY_EXISTS)
            {
                if (stopEvent_)
                {
                    CloseHandle(stopEvent_);
                    stopEvent_ = nullptr;
                }
                return 0;
            }
            controlOwned_ = true;
            originTarget_=bk::stroke::LoadCast();
            if(!originBus_.Open() || !originBus_.Publish(originTarget_))
            {
                MessageBoxW(nullptr,L"The live ray origin channel could not start.",
                    L"Kernel control",MB_OK|MB_ICONERROR);
                return 1;
            }
            MSG message{};
            PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            if (!RegisterInput())
            {
                Shutdown();
                return 1;
            }
            bool running = true;
            while (running)
            {
                const HANDLE handles[]{stopEvent_};
                const DWORD result = MsgWaitForMultipleObjects(
                    1, handles, FALSE, INFINITE, QS_ALLINPUT);
                if (result == WAIT_OBJECT_0)
                {
                    break;
                }
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if (message.message == KernelInput)
                    {
                        ToggleKernel();
                    }
                    else if (message.message == SceneInput)
                    {
                        QueueSceneToggle();
                    }
                    else if (message.message == StrokeInput)
                    {
                        QueueStroke();
                    }
                    else if (message.message == SettingsInput)
                    {
                        settingsPanel_.Show();
                    }
                    else if(message.message==OriginScroll)
                    {
                        ScrollOrigin(bk::app::UnpackPointer(message.lParam),
                            static_cast<SHORT>(message.wParam));
                    }
                    else if (message.message == CenterInput)
                    {
                        SignalScene(bk::tracking::HeadCenterEvent);
                    }
                    else if (message.message == WM_QUIT)
                    {
                        running = false;
                    }
                    else if (!settingsPanel_.Handle(message))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
            Shutdown();
            return 0;
        }

        static int StopAll()
        {
            SignalScene(ControlStopName);
            HANDLE kernel{};
            HANDLE render{};
            HANDLE visual{};
            HANDLE stroke{};
            const bool kernelStopped = StopScene(SceneKernelProcess,kernel);
            const bool visualStopped = StopScene(AudioSceneProcess, visual);
            const bool renderStopped = StopScene(LegacyScene, render);
            const bool strokeStopped = StopScene(WallStrokeProcess,stroke);
            return kernelStopped && visualStopped && renderStopped && strokeStopped ? 0 : 1;
        }

    private:
        static bool SceneRunning() noexcept
        {
            const HANDLE ready=OpenEventW(SYNCHRONIZE,FALSE,VisualReady);
            const bool running=ready && WaitForSingleObject(ready,0)==WAIT_OBJECT_0;
            if(ready) CloseHandle(ready);
            return running;
        }

        static bool DesktopHovered() noexcept
        {
            POINT point{};
            return GetCursorPos(&point) && bk::app::DesktopAt(point);
        }

        void ScrollOrigin(const POINT point,const SHORT wheel)
        {
            if(!SceneRunning() || !bk::app::DesktopAt(point)) return;
            originBus_.Read(originTarget_);
            auto target=originTarget_;
            target.depth=std::clamp(target.depth+
                static_cast<double>(wheel)/WHEEL_DELTA*0.05,0.0,1.0);
            PublishOrigin(target);
        }

        void PublishOrigin(const bk::stroke::CastPoint target)
        {
            if(target.across==originTarget_.across &&
                target.down==originTarget_.down && target.depth==originTarget_.depth)
                return;
            if(!originBus_.Publish(target)) return;
            originTarget_=target;
        }

        static bool ModifierDown()
        {
            return (GetAsyncKeyState(VK_SHIFT)&0x8000) ||
                (GetAsyncKeyState(VK_CONTROL)&0x8000) ||
                (GetAsyncKeyState(VK_MENU)&0x8000) ||
                (GetAsyncKeyState(VK_LWIN)&0x8000) ||
                (GetAsyncKeyState(VK_RWIN)&0x8000);
        }

        static LRESULT CALLBACK KeyHook(
            const int code,WPARAM message,LPARAM state)
        {
            if(code==HC_ACTION && active_)
            {
                const auto& key=*reinterpret_cast<KBDLLHOOKSTRUCT*>(state);
                const bool extended=(key.flags&LLKHF_EXTENDED)!=0;
                UINT action{};
                bool* held{};
                if(!extended && key.scanCode==KernelScan)
                {
                    action=KernelInput;
                    held=&active_->kernelHeld_;
                }
                else if(!extended && key.scanCode==SceneScan)
                {
                    action=SceneInput;
                    held=&active_->sceneHeld_;
                }
                else if(!extended && key.scanCode==StrokeScan)
                {
                    action=StrokeInput;
                    held=&active_->strokeHeld_;
                }
                else if(key.scanCode==SettingsScan)
                {
                    action=extended?CenterInput:SettingsInput;
                    held=extended?&active_->centerHeld_:
                        &active_->settingsHeld_;
                }
                if(held)
                {
                    const bool released=message==WM_KEYUP ||
                        message==WM_SYSKEYUP;
                    if(released)
                    {
                        const bool handled=*held;
                        *held=false;
                        if(handled) return 1;
                    }
                    else if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN)
                    {
                        if(!*held && !ModifierDown() &&
                            ((action!=KernelInput && action!=SceneInput) ||
                                DesktopHovered()))
                        {
                            *held=true;
                            PostThreadMessageW(
                                active_->threadId_,action,0,0);
                        }
                        if(*held) return 1;
                    }
                }
            }
            return CallNextHookEx(nullptr,code,message,state);
        }

        static LRESULT CALLBACK MouseHook(const int code,WPARAM message,
            LPARAM state)
        {
            if(code==HC_ACTION && active_)
            {
                const auto& mouse=*reinterpret_cast<MSLLHOOKSTRUCT*>(state);
                const auto packed=bk::app::PackPointer(mouse.pt);
                if(message==WM_MOUSEWHEEL && !ModifierDown() &&
                    bk::app::DesktopAt(mouse.pt) && SceneRunning())
                {
                    if(PostThreadMessageW(active_->threadId_,OriginScroll,
                        static_cast<WPARAM>(static_cast<SHORT>(
                            HIWORD(mouse.mouseData))),static_cast<LPARAM>(packed)))
                        return 1;
                }
            }
            return CallNextHookEx(nullptr,code,message,state);
        }

        static DWORD WINAPI InputThread(void* state)
        {
            auto* self=static_cast<KeypadApp*>(state);
            MSG seed{};
            PeekMessageW(&seed,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
            self->keyboardHook_=SetWindowsHookExW(
                WH_KEYBOARD_LL,KeyHook,GetModuleHandleW(nullptr),0);
            self->mouseHook_=SetWindowsHookExW(
                WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);
            SetEvent(self->inputReady_);
            if(!self->keyboardHook_ || !self->mouseHook_) return GetLastError();
            MSG message{};
            while(GetMessageW(&message,nullptr,0,0)>0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            UnhookWindowsHookEx(self->keyboardHook_);
            self->keyboardHook_=nullptr;
            UnhookWindowsHookEx(self->mouseHook_);
            self->mouseHook_=nullptr;
            return 0;
        }

        bool RegisterInput()
        {
            active_=this;
            threadId_=GetCurrentThreadId();
            inputReady_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(inputReady_)
                inputThread_=CreateThread(nullptr,0,InputThread,
                    this,0,&inputThreadId_);
            if(inputThread_ && WaitForSingleObject(
                inputReady_,StartTimeoutMs)==WAIT_OBJECT_0 &&
                keyboardHook_ && mouseHook_)
                return true;
            MessageBoxW(nullptr,
                L"The physical keypad listener could not start.",
                L"Kernel control",MB_OK|MB_ICONERROR);
            UnregisterInput();
            return false;
        }

        void UnregisterInput()
        {
            if(inputThread_)
            {
                PostThreadMessageW(inputThreadId_,WM_QUIT,0,0);
                WaitForSingleObject(inputThread_,StopTimeoutMs);
                CloseHandle(inputThread_);
                inputThread_=nullptr;
            }
            if(active_==this) active_=nullptr;
            if(keyboardHook_)
            {
                UnhookWindowsHookEx(keyboardHook_);
                keyboardHook_=nullptr;
            }
            if(inputReady_)
            {
                CloseHandle(inputReady_);
                inputReady_=nullptr;
            }
            if(mouseHook_) {UnhookWindowsHookEx(mouseHook_);mouseHook_=nullptr;}
            inputThreadId_=0;
            threadId_=0;
            kernelHeld_=sceneHeld_=settingsHeld_=centerHeld_=false;
            strokeHeld_=false;
        }

        void Shutdown()
        {
            if (!controlOwned_)
            {
                return;
            }
            UnregisterInput();
            StopScene(SceneKernelProcess,kernelProcess_);
            kernelJob_.Reset();
            if (stopEvent_)
            {
                CloseHandle(stopEvent_);
                stopEvent_ = nullptr;
            }
            controlOwned_ = false;
        }

        void ToggleKernel()
        {
            if (SceneActive(SceneKernelProcess,kernelProcess_))
            {
                if(!StopScene(SceneKernelProcess,kernelProcess_))
                    ShowSceneError(SceneKernelProcess,
                        L"The kernel did not finish stopping.",settingsPanel_);
                kernelJob_.Reset();
                return;
            }
            kernelJob_.Reset();
            if(!kernelJob_.Create())
            {
                ShowSceneError(SceneKernelProcess,
                    L"Windows could not create kernel ownership.",settingsPanel_);
                return;
            }
            if(!StartScene(SceneKernelProcess,kernelProcess_,settingsPanel_,
                kernelJob_.Get(),stopEvent_)) kernelJob_.Reset();
        }

        void QueueSceneToggle()
        {
            const HANDLE ready=OpenEventW(SYNCHRONIZE,FALSE,KernelReady);
            const bool running=ready &&
                WaitForSingleObject(ready,0)==WAIT_OBJECT_0;
            if(ready) CloseHandle(ready);
            if(!running) return;
            const HANDLE toggle=OpenSemaphoreW(
                SEMAPHORE_MODIFY_STATE,FALSE,SceneToggle);
            if(toggle)
            {
                const bool sent=ReleaseSemaphore(toggle,1,nullptr)!=FALSE;
                CloseHandle(toggle);
                if(!sent) ShowSceneError(AudioSceneProcess,
                    L"Windows could not queue the scene toggle.",settingsPanel_);
            }
        }

        void QueueStroke()
        {
            const HANDLE ready=OpenEventW(SYNCHRONIZE,FALSE,KernelReady);
            const bool running=ready &&
                WaitForSingleObject(ready,0)==WAIT_OBJECT_0;
            if(ready) CloseHandle(ready);
            if(!running) return;
            const HANDLE toggle=OpenSemaphoreW(
                SEMAPHORE_MODIFY_STATE,FALSE,WallStrokeToggle);
            if(!toggle) return;
            const bool sent=ReleaseSemaphore(toggle,1,nullptr)!=FALSE;
            CloseHandle(toggle);
            if(!sent) ShowSceneError(WallStrokeProcess,
                L"Windows could not queue the wall stroke toggle.",settingsPanel_);
        }

        HANDLE stopEvent_{},inputReady_{},inputThread_{};
        HHOOK keyboardHook_{},mouseHook_{};
        bk::stroke::OriginBus originBus_;
        bk::stroke::CastPoint originTarget_{};
        bk::app::SceneSettingsPanel settingsPanel_;
        HANDLE kernelProcess_{};
        OwnerJob kernelJob_;
        inline static KeypadApp* active_{};
        DWORD threadId_{},inputThreadId_{};
        bool controlOwned_{};
        bool kernelHeld_{},sceneHeld_{},settingsHeld_{},centerHeld_{};
        bool strokeHeld_{};
    };
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (commandLine && wcsstr(commandLine, L"--stop"))
    {
        return KeypadApp::StopAll();
    }
    KeypadApp controller;
    return controller.Run();
}
