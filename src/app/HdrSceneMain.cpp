#include "render/HdrRoomRenderer.hpp"
#include "audio/AudioCapture.hpp"
#include "visualizer/AudioTrailMotion.hpp"
#include "visualizer/AdaptiveTrail.hpp"
#include "visualizer/AudioSpectrum.hpp"
#include "visualizer/SceneSettings.hpp"
#include "visualizer/SceneRunState.hpp"
#include "tracking/OpenTrackPose.hpp"
#include "tracking/HeadTrackState.hpp"
#include "app/SceneSignals.hpp"
#include "stroke/WallStrokeSettings.hpp"
#include "stroke/RoomWallStroke.hpp"

#include <windows.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr wchar_t WindowClassName[] = L"RoomPreviewWindow";
    constexpr wchar_t WindowTitle[] = L"HDR room";
    constexpr int RenderToggleKey = 9;
    constexpr UINT SettingsMessage=WM_APP;
    constexpr UINT SyncMessage=WM_APP+1;
    constexpr UINT StopMessage=WM_APP+2;
    constexpr UINT OriginMessage=WM_APP+3;
    constexpr UINT SpawnWorkerMessage=0x052C;
    constexpr WPARAM SpawnWorkerMode=0xD;
    constexpr LPARAM SpawnWorkerTarget=1;

    struct WindowMessage
    {
        DWORD threadId{};
        UINT message{};
    };

    struct DeskHostSearch
    {
        HWND iconHost{};
    };

    struct DeskHost
    {
        HWND window{};
        bool needsReparent{};
    };

    BOOL CALLBACK FindIconHost(HWND window,LPARAM state)
    {
        const HWND view=FindWindowExW(
            window,nullptr,L"SHELLDLL_DefView",nullptr);
        if(!view) return TRUE;
        static_cast<DeskHostSearch*>(
            reinterpret_cast<void*>(state))->iconHost=window;
        return FALSE;
    }

    HWND FindChildWorker(HWND parent)
    {
        HWND worker{};
        HWND best{};
        long long bestArea{};
        while((worker=FindWindowExW(
            parent,worker,L"WorkerW",nullptr)))
        {
            RECT bounds{};
            if(!IsWindowVisible(worker) || !GetWindowRect(worker,&bounds))
                continue;
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

    DeskHost DesktopHost()
    {
        const HWND progman=FindWindowW(L"Progman",nullptr);
        if(!progman) return {};
        DWORD_PTR messageResult{};
        SendMessageTimeoutW(
            progman,
            SpawnWorkerMessage,
            SpawnWorkerMode,
            SpawnWorkerTarget,
            SMTO_ABORTIFHUNG,
            2000,
            &messageResult);
        const ULONGLONG deadline=GetTickCount64()+2000;
        do
        {
            if(const HWND child=FindChildWorker(progman))
                return {child,true};
            Sleep(10);
        } while(GetTickCount64()<deadline);
        DeskHostSearch search;
        EnumWindows(FindIconHost,
            reinterpret_cast<LPARAM>(&search));
        if(!search.iconHost) return {};
        const HWND worker=FindWindowExW(
            nullptr,search.iconHost,L"WorkerW",nullptr);
        return {worker,false};
    }

    class SceneApp
    {
    public:
        SceneApp(
            const bool desktopMode,
            const bool startEnabled,
            const bool manageHotkey,
            const bool visualizerMode,
            const wchar_t* stopEventName,
            const wchar_t* readyEventName)
            : desktopMode_(desktopMode),
              startEnabled_(startEnabled),
              manageHotkey_(manageHotkey),
              visualizerMode_(visualizerMode),
              stopEventName_(stopEventName),
              readyEventName_(readyEventName) {}

        ~SceneApp()
        {
            Shutdown();
        }

        int Run(const HINSTANCE instance, const int showCommand)
        {
            instance_ = instance;
            MSG queueSeed{};
            PeekMessageW(&queueSeed,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
            const DWORD threadId=GetCurrentThreadId();
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency))
                throw std::runtime_error("The presentation QPC clock is unavailable");
            clockHz_ = static_cast<double>(frequency.QuadPart);
            syncChangedEvent_=CreateEventW(nullptr,FALSE,FALSE,
                bk::visualizer::settings::SyncChangedEvent);
            if(!syncChangedEvent_)
                throw std::runtime_error("Creating presentation-sync settings event failed");
            UpdateSync();
            if (desktopMode_)
            {
                if(!ClaimSurface()) return ERROR_BUSY;
            }
            if (visualizerMode_)
            {
                if(!originBus_.Open())
                    throw std::runtime_error("Creating live ray origin channel failed");
                eyeChangedEvent_=CreateEventW(nullptr,FALSE,FALSE,bk::visualizer::settings::SettingsChanged);
                eyeAppliedEvent_=CreateEventW(nullptr,FALSE,FALSE,bk::visualizer::settings::SettingsApplied);
                if(!eyeChangedEvent_ || !eyeAppliedEvent_) throw std::runtime_error("Creating physical-view settings events failed");
                UpdateSettings();
                headCenterEvent_=CreateEventW(nullptr,FALSE,FALSE,bk::tracking::HeadCenterEvent);
                if(!headCenterEvent_) throw std::runtime_error("Creating head-centering event failed");
                trackSource_=std::make_unique<bk::tracking::OpenTrackPose>();
                trackWriter_=std::make_unique<bk::tracking::HeadTrackStateWriter>();
                runtimeWriter_=std::make_unique<bk::visualizer::runtime::SceneStateWriter>();
                runtime_.processId=GetCurrentProcessId();runtime_.frequency=frequency.QuadPart;
            }
            RegisterWindowClass();
            if (desktopMode_)
            {
                if (manageHotkey_ && !RegisterHotKey(
                    nullptr,
                    RenderToggleKey,
                    MOD_NOREPEAT,
                    VK_NUMPAD9))
                {
                    throw std::runtime_error(
                        "Numpad 9 is already registered as a global hotkey by another application");
                }
                hotkeyOwned_ = manageHotkey_;
                if (startEnabled_)
                {
                    EnableRenderer();
                }
            }
            else
            {
                CreateSurfaceWindow(showCommand);
                RECT client{};
                GetClientRect(window_, &client);
                renderer_ = std::make_unique<bk::render::HdrRoomRenderer>();
                renderer_->Initialize(
                    window_,
                    static_cast<uint32_t>(client.right),
                    static_cast<uint32_t>(client.bottom));
                renderer_->SetPaperWhiteNits(static_cast<float>(paperWhiteNits_));
                renderer_->SetPresentationSync(syncMode_);
            }

            syncPost_={threadId,SyncMessage};
            ArmSignal(syncWait_,syncChangedEvent_,syncPost_);
            if(visualizerMode_)
            {
                settingsPost_={threadId,SettingsMessage};
                ArmSignal(settingsWait_,eyeChangedEvent_,settingsPost_);
                originPost_={threadId,OriginMessage};
                ArmSignal(originWait_,originBus_.Changed(),originPost_);
            }
            if(stopEvent_)
            {
                stopPost_={threadId,StopMessage};
                ArmSignal(
                    stopWait_,stopEvent_,stopPost_,WT_EXECUTEONLYONCE);
                if(WaitForSingleObject(stopEvent_,0)==WAIT_OBJECT_0)
                {
                    Shutdown();
                    return 0;
                }
            }
            if(readyEvent_ && startEnabled_)
            {
                if(!window_ || !renderer_ || !IsWindowVisible(window_) ||
                    GetParent(window_)!=wallpaperHost_)
                    throw std::runtime_error(
                        "The Explorer surface failed readiness validation");
                SetEvent(readyEvent_);
            }

            MSG message{};
            while (message.message != WM_QUIT)
            {
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if(message.message==SettingsMessage)
                    {
                        UpdateSettings();
                        continue;
                    }
                    if(message.message==SyncMessage)
                    {
                        UpdateSync();
                        continue;
                    }
                    if(message.message==OriginMessage)
                    {
                        UpdateOrigin();
                        continue;
                    }
                    if(message.message==StopMessage)
                    {
                        stopRequested_=true;
                        continue;
                    }
                    if (message.message == WM_HOTKEY &&
                        message.wParam == RenderToggleKey)
                    {
                        toggleRequested_ = true;
                        continue;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (message.message == WM_QUIT)
                {
                    break;
                }
                if(stopRequested_)
                {
                    break;
                }

                if (toggleRequested_)
                {
                    toggleRequested_ = false;
                    ToggleRenderer();
                }

                if (!renderer_)
                {
                    WaitMessage();
                    continue;
                }

                if (!minimized_)
                {
                    if (visualizerMode_)
                    {
                        LARGE_INTEGER audioStart{},audioEnd{},pathEnd{};QueryPerformanceCounter(&audioStart);
                        const double previousTick=visiblePath_.CurrentTick();
                        UpdateVisualizer();
                        QueryPerformanceCounter(&audioEnd);
                        const auto frame=bk::visualizer::SizedCurrentFrame(visiblePath_,sizeResponse_);
                        const auto trail =
                            bk::visualizer::BuildAdaptiveTrail(visiblePath_,sizeResponse_);
                        const auto sweep=bk::visualizer::BuildPresentSweep(
                            visiblePath_,sizeResponse_);
                        const auto tracked=trackSource_->LatestPose();
                        if(WaitForSingleObject(headCenterEvent_,0)==WAIT_OBJECT_0) {
                            headView_.CenterNext();centerAckPending_=true;
                        }
                        const auto eye=headView_.ApplyPose(tracked,eyeDistanceMm_,trackDepthScale_);
                        const auto* strokeFrame=visiblePath_.Ready()?&frame:nullptr;
                        const auto strokeView=stroke_.CurrentView();
                        QueryPerformanceCounter(&pathEnd);
                        renderer_->Render(
                            strokeFrame,trail.points,sweep.points,&eye,{},&strokeView);
                        bk::tracking::HeadTrackState status;
                        status.flags=(tracked.validPackets?1u:0u)|(headView_.Centered()?2u:0u)|(headView_.OffsetValid()?4u:0u);
                        status.socketError=tracked.socketError;status.validPackets=tracked.validPackets;status.badPackets=tracked.badPackets;
                        status.recvQpc=tracked.recvQpc;LARGE_INTEGER presented{};QueryPerformanceCounter(&presented);
                        status.presentQpc=presented.QuadPart;status.qpcFrequency=static_cast<std::int64_t>(clockHz_);
                        const auto capture=audioCapture_->ReadStats();
                        runtime_.ready=(visiblePath_.Ready()?1u:0u)|(visiblePath_.WaitingForFuture()?2u:0u);
                        ++runtime_.presentations;runtime_.captureFrames=capture.copiedFrames;runtime_.captureBreaks=capture.epochBreaks;
                        runtime_.queuedFrames=capture.queuedFrames;runtime_.motionTick=visiblePath_.CurrentTick();
                        if(visiblePath_.Ready() && previousTick==visiblePath_.CurrentTick()) ++runtime_.heldClockFrames;
                        runtime_.trailPoints=trail.points.size();runtime_.sweepPoints=sweep.points.size();
                        runtime_.presentQpc=presented.QuadPart;
                        runtime_.audioMs=1000*(audioEnd.QuadPart-audioStart.QuadPart)/clockHz_;
                        runtime_.pathMs=1000*(pathEnd.QuadPart-audioEnd.QuadPart)/clockHz_;
                        runtime_.renderMs=1000*(presented.QuadPart-pathEnd.QuadPart)/clockHz_;
                        runtime_.totalAudioMs+=runtime_.audioMs;runtime_.totalPathMs+=runtime_.pathMs;runtime_.totalRenderMs+=runtime_.renderMs;
                        runtime_.maxRenderMs=std::max(runtime_.maxRenderMs,runtime_.renderMs);
                        runtime_.analysisDbfs=spectrum_.WindowDbfs();
                        runtime_.planarHalfLife=pursuit_.PlanarHalfLifeSec();
                        runtime_.depthHalfLife=pursuit_.DepthHalfLifeSec();
                        runtime_.spectralEntropy=frame.spectralEntropy;
                        runtime_.dotXmm=frame.dotXmm;runtime_.dotYmm=frame.dotYmm;runtime_.dotZmm=frame.dotZmm;
                        runtime_.dotRadiusMm=frame.dotRadiusMm;
                        if(visiblePath_.Ready()) {
                            const auto current=bk::visualizer::PositionAtTick(visiblePath_,visiblePath_.CurrentTick());
                            const auto past=bk::visualizer::PositionAtTick(visiblePath_,visiblePath_.CurrentTick()-
                                bk::visualizer::SizeLagTicks);
                            runtime_.sizeMoveMm=bk::visualizer::Length(current-past);
                        }
                        runtimeWriter_->Publish(runtime_);
                        status.rawPose=tracked.pose;status.neutralPose=headView_.NeutralPose();status.eye={eye.position.x,eye.position.y,eye.position.z};
                        status.distanceMm=eyeDistanceMm_;
                        status.lightDepth=bk::scene::MakeLight(frame.dotXmm,frame.dotYmm,2*frame.dotRadiusMm,{frame.red,frame.green,frame.blue},frame.dotZmm).position.z;
                        trackWriter_->Publish(status);
                        if(centerAckPending_ && headView_.Centered() && !headView_.CenterPending()) {
                            const auto centerAck=OpenEventW(EVENT_MODIFY_STATE,FALSE,bk::tracking::HeadAppliedEvent);
                            if(centerAck) { SetEvent(centerAck);CloseHandle(centerAck); }
                            centerAckPending_=false;
                        }
                        visiblePath_.MarkPresented();
                        sourcePath_.MarkPresented();
                        if(eyeAckPending_) { SetEvent(eyeAppliedEvent_);eyeAckPending_=false; }
                    }
                    else
                    {
                        renderer_->Render();
                    }
                }
                else
                {
                    WaitMessage();
                }
            }

            Shutdown();
            return static_cast<int>(message.wParam);
        }

    private:
        bool ClaimSurface()
        {
            ownerMutex_=CreateMutexW(
                nullptr,FALSE,bk::app::surface::OwnerName);
            if(!ownerMutex_)
                throw std::runtime_error(
                    "Creating surface ownership failed");
            const DWORD ownerResult=WaitForSingleObject(ownerMutex_,0);
            if(ownerResult!=WAIT_OBJECT_0 &&
                ownerResult!=WAIT_ABANDONED)
                return false;
            ownerHeld_=true;
            stopEvent_=CreateEventW(
                nullptr,TRUE,FALSE,stopEventName_);
            if(!stopEvent_)
                throw std::runtime_error(
                    "Creating the surface stop signal failed");
            if(GetLastError()==ERROR_ALREADY_EXISTS)
            {
                CloseHandle(stopEvent_);
                stopEvent_=nullptr;
                return false;
            }
            readyEvent_=CreateEventW(
                nullptr,TRUE,FALSE,readyEventName_);
            if(!readyEvent_)
                throw std::runtime_error(
                    "Creating the surface ready signal failed");
            ResetEvent(readyEvent_);
            return true;
        }

        void Shutdown() noexcept
        {
            if(shutdown_) return;
            shutdown_=true;
            if(readyEvent_) ResetEvent(readyEvent_);
            CloseSignal(stopWait_);
            CloseSignal(settingsWait_);
            CloseSignal(syncWait_);
            CloseSignal(originWait_);
            originBus_.Close();
            audioCapture_.reset();
            runtimeWriter_.reset();
            trackWriter_.reset();
            trackSource_.reset();
            DisableRenderer();
            if(window_)
            {
                const HWND surface=window_;
                unloadingWindow_=true;
                DestroyWindow(surface);
                unloadingWindow_=false;
                window_=nullptr;
            }
            if(hotkeyOwned_)
            {
                UnregisterHotKey(nullptr,RenderToggleKey);
                hotkeyOwned_=false;
            }
            if(classOwned_)
            {
                UnregisterClassW(WindowClassName,instance_);
                classOwned_=false;
            }
            if(headCenterEvent_)
            {
                CloseHandle(headCenterEvent_);
                headCenterEvent_=nullptr;
            }
            if(eyeChangedEvent_)
            {
                CloseHandle(eyeChangedEvent_);
                eyeChangedEvent_=nullptr;
            }
            if(eyeAppliedEvent_)
            {
                CloseHandle(eyeAppliedEvent_);
                eyeAppliedEvent_=nullptr;
            }
            if(syncChangedEvent_)
            {
                CloseHandle(syncChangedEvent_);
                syncChangedEvent_=nullptr;
            }
            if(readyEvent_)
            {
                CloseHandle(readyEvent_);
                readyEvent_=nullptr;
            }
            if(stopEvent_)
            {
                CloseHandle(stopEvent_);
                stopEvent_=nullptr;
            }
            if(ownerHeld_)
            {
                ReleaseMutex(ownerMutex_);
                ownerHeld_=false;
            }
            if(ownerMutex_)
            {
                CloseHandle(ownerMutex_);
                ownerMutex_=nullptr;
            }
        }

        static void CALLBACK PostSignal(void* context,BOOLEAN)
        {
            const auto& post=*static_cast<WindowMessage*>(context);
            PostThreadMessageW(post.threadId,post.message,0,0);
        }

        static void ArmSignal(
            HANDLE& wait,
            HANDLE event,
            WindowMessage& post,
            const ULONG flags=WT_EXECUTEDEFAULT)
        {
            if(!RegisterWaitForSingleObject(&wait,event,PostSignal,&post,
                INFINITE,flags))
                throw std::runtime_error("Registering settings signal failed");
        }

        static void CloseSignal(HANDLE& wait) noexcept
        {
            if(!wait) return;
            UnregisterWaitEx(wait,INVALID_HANDLE_VALUE);
            wait=nullptr;
        }

        static LRESULT CALLBACK WindowProcedure(const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
        {
            SceneApp* application = reinterpret_cast<SceneApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
                application = static_cast<SceneApp*>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
                application->window_ = window;
            }

            if (!application)
            {
                return DefWindowProcW(window, message, wParam, lParam);
            }
            return application->HandleMessage(message, wParam, lParam);
        }

        LRESULT HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam)
        {
            switch (message)
            {
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE)
                {
                    DestroyWindow(window_);
                    return 0;
                }
                break;

            case WM_SIZE:
            {
                const uint32_t width = LOWORD(lParam);
                const uint32_t height = HIWORD(lParam);
                minimized_ = wParam == SIZE_MINIMIZED || width == 0 || height == 0;
                if (!minimized_ && renderer_)
                {
                    renderer_->Resize(width, height);
                }
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_DESTROY:
                if (!unloadingWindow_)
                {
                    PostQuitMessage(0);
                }
                return 0;
            }

            return DefWindowProcW(window_, message, wParam, lParam);
        }

        void RegisterWindowClass()
        {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.style = CS_HREDRAW | CS_VREDRAW;
            windowClass.lpfnWndProc = WindowProcedure;
            windowClass.hInstance = instance_;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = WindowClassName;
            windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            if (!RegisterClassExW(&windowClass))
            {
                throw std::runtime_error("RegisterClassExW failed");
            }
            classOwned_=true;
        }

        void CreateSurfaceWindow(const int showCommand)
        {
            if (desktopMode_)
            {
                const DeskHost host=DesktopHost();
                wallpaperHost_=host.window;
                needsReparent_=host.needsReparent;
                if (!wallpaperHost_)
                {
                    throw std::runtime_error("Explorer wallpaper host was not found");
                }
                POINT primaryOrigin{};
                const HMONITOR primaryMonitor = MonitorFromPoint(
                    primaryOrigin, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO monitorInfo{sizeof(monitorInfo)};
                if (!GetMonitorInfoW(primaryMonitor, &monitorInfo))
                {
                    throw std::runtime_error("The primary monitor rectangle is unavailable");
                }
                primaryOrigin = {monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top};
                if (!ScreenToClient(wallpaperHost_, &primaryOrigin))
                {
                    throw std::runtime_error("The primary monitor could not be mapped into Explorer");
                }
                const int monitorWidth =
                    monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
                const int monitorHeight =
                    monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
                const HWND createHost=needsReparent_
                    ? FindWindowW(L"Progman",nullptr)
                    : wallpaperHost_;
                if(!createHost)
                    throw std::runtime_error(
                        "Explorer surface creation host was not found");
                window_ = CreateWindowExW(
                    WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                    WindowClassName,
                    L"Desktop scene",
                    WS_CHILD,
                    primaryOrigin.x,
                    primaryOrigin.y,
                    monitorWidth,
                    monitorHeight,
                    createHost,
                    nullptr,
                    instance_,
                    this);
                if (!window_)
                {
                    throw std::runtime_error(
                        "Creating the Explorer child surface failed: "+
                        std::to_string(GetLastError()));
                }
                if(needsReparent_)
                {
                    SetLastError(ERROR_SUCCESS);
                    const HWND priorParent=SetParent(window_,wallpaperHost_);
                    if(!priorParent && GetLastError()!=ERROR_SUCCESS)
                        throw std::runtime_error(
                            "Attaching the Explorer child surface failed: "+
                            std::to_string(GetLastError()));
                }
                SetWindowPos(
                    window_, HWND_BOTTOM,
                    primaryOrigin.x, primaryOrigin.y, monitorWidth, monitorHeight,
                    SWP_NOACTIVATE);
                return;
            }

            RECT rectangle{0, 0, 1280, 720};
            AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
            window_ = CreateWindowExW(
                0,
                WindowClassName,
                WindowTitle,
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                rectangle.right - rectangle.left,
                rectangle.bottom - rectangle.top,
                nullptr,
                nullptr,
                instance_,
                this);
            if (!window_)
            {
                throw std::runtime_error("CreateWindowExW failed");
            }
            ShowWindow(window_, showCommand);
            UpdateWindow(window_);
        }

        void EnableRenderer()
        {
            if (renderer_)
            {
                return;
            }
            if (!window_)
            {
                CreateSurfaceWindow(SW_SHOWNOACTIVATE);
            }
            RECT client{};
            GetClientRect(window_, &client);
            const auto width = static_cast<uint32_t>(
                std::max(client.right, 1L));
            const auto height = static_cast<uint32_t>(
                std::max(client.bottom, 1L));
            if (visualizerMode_)
            {
                ResetTimeline(width, height);
            }
            auto warmRenderer = std::make_unique<bk::render::HdrRoomRenderer>();
            warmRenderer->Initialize(
                window_,
                width,
                height);
            warmRenderer->SetPaperWhiteNits(static_cast<float>(paperWhiteNits_));
            warmRenderer->SetPresentationSync(syncMode_);
            if(visualizerMode_)
            {
                const auto warmEye=headView_.ApplyPose(trackSource_->LatestPose(),eyeDistanceMm_,trackDepthScale_);
                warmRenderer->Render(nullptr,{}, {},&warmEye);
            }
            else
            {
                warmRenderer->Render();
            }
            warmRenderer->WaitForGpu();
            if(visualizerMode_)
            {
                auto freshCapture=std::make_unique<bk::audio::AudioCapture>();
                freshCapture->Start();audioCapture_=std::move(freshCapture);
            }
            renderer_ = std::move(warmRenderer);
            SetWindowPos(
                window_, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(window_, SW_SHOWNOACTIVATE);
            UpdateWindow(window_);
        }

        void DisableRenderer()
        {
            const HWND priorHost=wallpaperHost_;
            audioCapture_.reset();
            pendingAudio_.clear();
            pendingAudioHns_.clear();
            pendingEdges_.clear();
            pendingOffset_ = 0;
            tickCadence_.Reset();
            visiblePath_.Reset(1, 1);
            sourcePath_.Reset(1, 1);pursuit_.Reset();
            sourceClock_.Reset();
            stroke_.Reset();
            strokeTick_=-1;
            if (renderer_)
            {
                if(window_) ShowWindow(window_, SW_HIDE);
                renderer_.reset();
            }
            if (desktopMode_ && window_)
            {
                const HWND renderWindow = window_;
                unloadingWindow_ = true;
                DestroyWindow(renderWindow);
                unloadingWindow_ = false;
                window_ = nullptr;
                wallpaperHost_ = nullptr;
                minimized_ = false;
            }
            if(priorHost && IsWindow(priorHost))
                RedrawWindow(priorHost,nullptr,nullptr,
                    RDW_INVALIDATE|RDW_INTERNALPAINT|RDW_ALLCHILDREN);
            const HWND progman=FindWindowW(L"Progman",nullptr);
            if(progman)
                RedrawWindow(progman,nullptr,nullptr,
                    RDW_INVALIDATE|RDW_INTERNALPAINT|RDW_ALLCHILDREN);
        }

        void ToggleRenderer()
        {
            if (renderer_)
            {
                DisableRenderer();
            }
            else
            {
                EnableRenderer();
            }
        }

        void ResetTimeline(const std::uint32_t, const std::uint32_t)
        {
            pendingAudio_.clear();
            pendingAudioHns_.clear();
            pendingEdges_.clear();
            pendingOffset_ = 0;
            consumedFrames_ = 0;
            completedTicks_ = 0;
            tickCadence_.Reset();
            audioModel_.Reset(bk::scene::ScreenChordMm, bk::scene::ScreenHeightMm);
            spectrum_.Reset();
            pursuit_.Reset();
            visiblePath_.Reset(bk::scene::ScreenChordMm, bk::scene::ScreenHeightMm);
            sourcePath_.Reset(bk::scene::ScreenChordMm, bk::scene::ScreenHeightMm);
            sourceClock_.Reset();
            auto silentBoundary=audioModel_.CurrentFrame();
            spectrum_.ApplyToFrame(silentBoundary);
            silentBoundary.planarHalfLifeSec=bk::visualizer::PlanarHalfLife(
                silentBoundary.spectralEntropy,spectrum_.WindowDbfs(),
                pursuitMap_);
            silentBoundary.depthHalfLifeSec=bk::visualizer::VolumeHalfLife(
                spectrum_.WindowDbfs(),pursuitMap_);
            for(std::size_t boundary=0;
                boundary<bk::visualizer::StartupPointCount;++boundary)
                sourcePath_.PushWaypoint(silentBoundary);
        }

        void UpdateVisualizer()
        {
            if (!audioCapture_)
            {
                return;
            }
            audioCapture_->DrainEpochPcm(
                pendingAudio_, pendingAudioHns_,pendingEdges_);
            std::size_t edgeCursor{};
            std::uint32_t tickFrames = tickCadence_.NextFrameCount();
            while (pendingAudio_.size() - pendingOffset_ >= tickFrames)
            {
                const auto* tickStart =
                    pendingAudio_.data() + pendingOffset_;
                const std::span<const bk::visualizer::RawStereoPcm>
                    tickSamples(tickStart, tickFrames);
                bool crossed=false;
                while(edgeCursor<pendingEdges_.size() &&
                    pendingEdges_[edgeCursor]<pendingOffset_+tickFrames)
                {
                    ++edgeCursor;
                    ++runtime_.handledBreaks;
                    crossed=true;
                }
                if(crossed) spectrum_.Reset();
                audioModel_.Tick(
                    tickSamples,
                    bk::scene::ScreenChordMm,
                    bk::scene::ScreenHeightMm);
                spectrum_.AnalyzeTick(tickSamples);
                auto coloredFrame = audioModel_.CurrentFrame();
                spectrum_.ApplyToFrame(coloredFrame);
                consumedFrames_ += tickFrames;
                ++completedTicks_;
                {
                    coloredFrame.planarHalfLifeSec=bk::visualizer::PlanarHalfLife(
                        coloredFrame.spectralEntropy,spectrum_.WindowDbfs(),
                        pursuitMap_);
                    coloredFrame.depthHalfLifeSec=bk::visualizer::VolumeHalfLife(
                        spectrum_.WindowDbfs(),pursuitMap_);
                    const auto tick = sourcePath_.PushWaypoint(coloredFrame);
                    const double cadenceOffset =
                        static_cast<double>(completedTicks_) * bk::visualizer::PcmRateHz /
                            bk::visualizer::TickRate - static_cast<double>(consumedFrames_);
                    const double tickEndHns = static_cast<double>(
                        pendingAudioHns_[pendingOffset_ + tickFrames - 1]) +
                        (1.0 + cadenceOffset) * 10'000'000.0 / bk::visualizer::PcmRateHz;
                    sourceClock_.AnchorSourceTick(tick, tickEndHns);
                }
                pendingOffset_ += tickFrames;
                tickCadence_.Advance();
                tickFrames = tickCadence_.NextFrameCount();
            }

            pendingEdges_.erase(pendingEdges_.begin(),pendingEdges_.begin()+
                static_cast<std::ptrdiff_t>(edgeCursor));

            if (pendingOffset_ == pendingAudio_.size())
            {
                for(auto& edge:pendingEdges_) edge-=pendingOffset_;
                pendingAudio_.clear();
                pendingAudioHns_.clear();
                pendingOffset_ = 0;
            }
            else if (pendingOffset_ >= bk::visualizer::PcmRateHz)
            {
                for(auto& edge:pendingEdges_) edge-=pendingOffset_;
                pendingAudio_.erase(
                    pendingAudio_.begin(),
                    pendingAudio_.begin() +
                        static_cast<std::ptrdiff_t>(pendingOffset_));
                pendingAudioHns_.erase(
                    pendingAudioHns_.begin(),
                    pendingAudioHns_.begin() +
                        static_cast<std::ptrdiff_t>(pendingOffset_));
                pendingOffset_ = 0;
            }
            LARGE_INTEGER now{};
            if (!QueryPerformanceCounter(&now))
                throw std::runtime_error("Reading the presentation QPC clock failed");
            const double sourceTick=sourceClock_.SourceReadyTick(
                static_cast<double>(now.QuadPart) * 10'000'000.0 / clockHz_);
            sourcePath_.AdvanceToTick(sourceTick);
            pursuit_.PursueSegments(sourcePath_,visiblePath_);
            const double visibleTick=sourceTick-bk::visualizer::PursuitExtraTicks;
            visiblePath_.AdvanceToTick(visibleTick);
            AdvanceStroke();
        }

        void AdvanceStroke()
        {
            if(!visiblePath_.Ready() || visiblePath_.Segments().empty()) return;
            const auto first=static_cast<std::int64_t>(
                visiblePath_.Segments().front().startTick);
            if(strokeTick_+1<first)
                throw std::logic_error(
                    "Unconsumed wall trajectory was discarded");
            const auto current=static_cast<std::int64_t>(
                std::floor(visiblePath_.CurrentTick()));
            bool changed=false;
            while(strokeTick_<current)
            {
                const double tick=static_cast<double>(++strokeTick_);
                const auto point=bk::visualizer::SizedFrameAtTick(
                    visiblePath_,tick,sizeResponse_);
                stroke_.Advance(point);
                changed=true;
            }
            if(changed) stroke_.Commit();
        }

        void UpdateOrigin()
        {
            bk::stroke::CastPoint point{};
            if(originBus_.Read(point)) stroke_.MoveOrigin(point);
        }

        void UpdateSettings()
        {
            double eyeDistance{};
            const auto status=bk::visualizer::settings::ReadEyeDistance(eyeDistance);
            if(status!=ERROR_SUCCESS) throw std::runtime_error("Reading eye distance failed: "+std::to_string(status));
            bk::visualizer::settings::SpectrumConfig spectrumConfig;
            const auto controlsStatus=bk::visualizer::settings::ReadSpectrum(spectrumConfig);
            if(controlsStatus!=ERROR_SUCCESS) throw std::runtime_error("Reading spectral settings failed: "+std::to_string(controlsStatus));
            std::size_t colorPalette{};
            const auto paletteStatus=bk::visualizer::settings::ReadPaletteChoice(colorPalette);
            if(paletteStatus!=ERROR_SUCCESS)
                throw std::runtime_error("Reading color palette failed: "+std::to_string(paletteStatus));
            bk::visualizer::EntropyPursuit entropyPursuit;
            const auto halfLifeStatus=bk::visualizer::settings::ReadEntropyPursuit(entropyPursuit);
            if(halfLifeStatus!=ERROR_SUCCESS) throw std::runtime_error("Reading entropy LR/UD smoothing failed: "+std::to_string(halfLifeStatus));
            bk::visualizer::VolumePursuit volumePursuit;
            const auto dbfsStatus=bk::visualizer::settings::ReadVolumePursuit(volumePursuit);
            if(dbfsStatus!=ERROR_SUCCESS) throw std::runtime_error(
                "Reading dBFS XY smoothing failed: "+std::to_string(dbfsStatus));
            const auto pursuitMap=bk::visualizer::CompilePursuit(
                entropyPursuit,volumePursuit);
            bk::visualizer::MoveSizeMap sizeMap;
            const auto sizeStatus=bk::visualizer::settings::ReadMoveSizeMap(
                sizeMap);
            if(sizeStatus!=ERROR_SUCCESS) throw std::runtime_error(
                "Reading displacement-size controls failed: "+std::to_string(sizeStatus));
            const auto sizeResponse=bk::visualizer::CompileSize(sizeMap);
            bk::visualizer::VerticalDbfsRange verticalRange;
            const auto verticalRangeStatus=bk::visualizer::settings::ReadVerticalRange(verticalRange);
            if(verticalRangeStatus!=ERROR_SUCCESS) throw std::runtime_error("Reading vertical dBFS range failed: "+std::to_string(verticalRangeStatus));
            bk::visualizer::TopDecay topDecay;
            const auto topDecayStatus=
                bk::visualizer::settings::ReadTopDecay(topDecay);
            if(topDecayStatus!=ERROR_SUCCESS) throw std::runtime_error(
                "Reading Top decay failed: "+
                std::to_string(topDecayStatus));
            if(!bk::visualizer::ValidTopMap(verticalRange,topDecay))
                throw std::runtime_error(
                    "Vertical dBFS range and Top decay are incompatible");
            double paperWhite{};
            const auto paperWhiteStatus=bk::visualizer::settings::ReadPaperWhite(paperWhite);
            if(paperWhiteStatus!=ERROR_SUCCESS) throw std::runtime_error("Reading paper white failed: "+std::to_string(paperWhiteStatus));
            spectrum_.SetPalette(spectrumConfig.palette);
            spectrum_.SetPaletteIndex(colorPalette);
            stroke_.Configure(bk::stroke::Load(),bk::stroke::LoadCast(),
                verticalRange);
            UpdateOrigin();
            audioModel_.SetTopMap(verticalRange,topDecay);
            if(renderer_) renderer_->SetPaperWhiteNits(static_cast<float>(paperWhite));
            eyeDistanceMm_=eyeDistance;
            paperWhiteNits_=paperWhite;
            trackDepthScale_=spectrumConfig.reverseTrackZ!=0?1.0:-1.0;
            pursuitMap_=pursuitMap;
            sizeResponse_=sizeResponse;
            runtime_.focusedHalfLife=entropyPursuit.focusedHalfLifeSec;
            runtime_.uniformHalfLife=entropyPursuit.uniformHalfLifeSec;
            runtime_.entropyCurve=entropyPursuit.curveExponent;
            runtime_.volumeCurve=volumePursuit.curveExponent;
            runtime_.zeroHalfLifeLevel=volumePursuit.zeroHalfLifeLevel;
            runtime_.sizeCurve=sizeMap.curveExponent;
            runtime_.maxDiameterPx=sizeMap.maxDiameterPx;
            eyeAckPending_=true;
        }

        void UpdateSync()
        {
            bk::render::PresentationSync syncMode{};
            const auto status=bk::visualizer::settings::ReadSyncMode(syncMode);
            if(status!=ERROR_SUCCESS)
                throw std::runtime_error("Reading presentation synchronization failed: "+std::to_string(status));
            if(renderer_) renderer_->SetPresentationSync(syncMode);
            syncMode_=syncMode;
        }

        HINSTANCE instance_{};
        HWND window_{};
        HWND wallpaperHost_{};
        HANDLE ownerMutex_{},stopEvent_{},readyEvent_{};
        HANDLE eyeChangedEvent_{},eyeAppliedEvent_{},syncChangedEvent_{};
        HANDLE stopWait_{},settingsWait_{},syncWait_{};
        HANDLE originWait_{};
        WindowMessage stopPost_{},settingsPost_{},syncPost_{};
        WindowMessage originPost_{};
        bk::stroke::OriginBus originBus_;
        HANDLE headCenterEvent_{};
        bool centerAckPending_{};
        std::unique_ptr<bk::tracking::OpenTrackPose> trackSource_;
        std::unique_ptr<bk::tracking::HeadTrackStateWriter> trackWriter_;
        std::unique_ptr<bk::visualizer::runtime::SceneStateWriter> runtimeWriter_;
        bk::visualizer::runtime::SceneRunState runtime_;
        bk::tracking::HeadView headView_;
        double eyeDistanceMm_{bk::scene::BaseEyeMm};
        double paperWhiteNits_{bk::render::hdr::BasePaperWhite};
        double trackDepthScale_{-1.0};
        bk::visualizer::PursuitMap pursuitMap_{};
        bk::visualizer::SizeResponse sizeResponse_{};
        bk::render::PresentationSync syncMode_{bk::render::PresentationSync::Immediate};
        bool eyeAckPending_{};
        bool minimized_{};
        bool desktopMode_{};
        bool startEnabled_{};
        bool hotkeyOwned_{};
        bool toggleRequested_{};
        bool stopRequested_{};
        bool unloadingWindow_{};
        bool needsReparent_{};
        bool ownerHeld_{};
        bool classOwned_{};
        bool shutdown_{};
        bool manageHotkey_{};
        bool visualizerMode_{};
        const wchar_t* stopEventName_{};
        const wchar_t* readyEventName_{};
        std::unique_ptr<bk::render::HdrRoomRenderer> renderer_;
        std::unique_ptr<bk::audio::AudioCapture> audioCapture_;
        bk::visualizer::AudioTrailMotion audioModel_;
        bk::visualizer::AudioSpectrum spectrum_;
        bk::visualizer::PathPursuit pursuit_;
        bk::visualizer::TickCadence tickCadence_;
        std::vector<bk::visualizer::RawStereoPcm> pendingAudio_;
        std::vector<std::uint64_t> pendingAudioHns_;
        std::vector<std::size_t> pendingEdges_;
        bk::visualizer::ContinuousMotion visiblePath_;
        bk::visualizer::ContinuousMotion sourcePath_;
        bk::visualizer::AudioMotionClock sourceClock_;
        std::uint64_t consumedFrames_{};
        std::uint64_t completedTicks_{};
        double clockHz_{};
        std::size_t pendingOffset_{};
        bk::stroke::RoomWallStroke stroke_;
        std::int64_t strokeTick_{-1};
    };
}

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR commandLine, const int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool visualizerMode =
        commandLine && wcsstr(commandLine, L"--visualizer-worker");
#ifdef BK_VISUALIZER
    visualizerMode = true;
#endif
    const wchar_t* stopEventName = visualizerMode
        ? bk::app::surface::VisualStop
        : bk::app::surface::RenderStop;
    const wchar_t* readyEventName = visualizerMode
        ? bk::app::surface::VisualReady
        : bk::app::surface::RenderReady;
    if (commandLine && wcsstr(commandLine, L"--stop"))
    {
        const HANDLE event = OpenEventW(
            EVENT_MODIFY_STATE, FALSE, stopEventName);
        if (event)
        {
            SetEvent(event);
            CloseHandle(event);
        }
        return 0;
    }
    try
    {
        bool desktopMode = commandLine && wcsstr(commandLine, L"--desktop");
        const bool startEnabled = !(commandLine && wcsstr(commandLine, L"--disabled"));
        const bool manageHotkey = !(commandLine && (
            wcsstr(commandLine, L"--render-worker") ||
            wcsstr(commandLine, L"--visualizer-worker")));
#ifdef BK_DESKTOP
        desktopMode = true;
#endif
        SceneApp application(
            desktopMode,
            startEnabled,
            manageHotkey,
            visualizerMode,
            stopEventName,
            readyEventName);
        return application.Run(instance, showCommand);
    }
    catch (const std::exception& error)
    {
        const bool managed=commandLine && (
            wcsstr(commandLine,L"--render-worker") ||
            wcsstr(commandLine,L"--visualizer-worker"));
        if(!managed)
            MessageBoxA(nullptr,error.what(),
                "Kernel failed",MB_OK|MB_ICONERROR);
        return 1;
    }
}
