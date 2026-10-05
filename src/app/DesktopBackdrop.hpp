#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <limits>

namespace bk::app
{
    class DesktopBackdrop
    {
    public:
        DesktopBackdrop() = default;
        DesktopBackdrop(const DesktopBackdrop&) = delete;
        DesktopBackdrop& operator=(const DesktopBackdrop&) = delete;
        ~DesktopBackdrop() { Restore(); }

        bool Black() noexcept
        {
            if(active_) return true;
            const HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            if(FAILED(init) && init!=RPC_E_CHANGED_MODE) return false;
            comOwned_=SUCCEEDED(init);
            const HRESULT made=CoCreateInstance(
                CLSID_DesktopWallpaper,nullptr,CLSCTX_ALL,
                IID_PPV_ARGS(&wallpaper_));
            if(FAILED(made))
            {
                CloseCom();
                return false;
            }
            if(FAILED(wallpaper_->GetBackgroundColor(&priorColor_)))
            {
                CloseCom();
                return false;
            }
            DESKTOP_SLIDESHOW_STATE slideState{};
            if(SUCCEEDED(wallpaper_->GetStatus(&slideState)) &&
                (slideState&DSS_SLIDESHOW)!=0)
            {
                if(FAILED(wallpaper_->GetSlideshowOptions(
                    &slideOpts_,&slideTick_)) ||
                    FAILED(wallpaper_->SetSlideshowOptions(
                        slideOpts_,std::numeric_limits<UINT>::max())))
                {
                    CloseCom();
                    return false;
                }
                slideSaved_=true;
            }
            active_=true;
            if(!CreateCover())
            {
                Restore();
                return false;
            }
            if(FAILED(wallpaper_->Enable(TRUE)))
            {
                Restore();
                return false;
            }
            Restart();
            return true;
        }

        bool PrepareScene() noexcept
        {
            return CoverScene();
        }

        bool CoverScene() noexcept
        {
            if(!active_ || !wallpaper_ || !base_) return false;
            return ShowCover();
        }

        void RevealScene() noexcept
        {
            if(base_) ShowWindow(base_,SW_HIDE);
            coverShown_=false;
        }

        void Advance() noexcept
        {
            if(!active_ || !coverShown_ || !shade_) return;
            constexpr ULONGLONG leg=4000;
            const ULONGLONG phase=(GetTickCount64()-started_)%(leg*2);
            const ULONGLONG rise=phase<=leg?phase:leg*2-phase;
            const BYTE alpha=static_cast<BYTE>((rise*255+leg/2)/leg);
            if(alpha==alpha_) return;
            if(SetLayeredWindowAttributes(shade_,0,alpha,LWA_ALPHA))
                alpha_=alpha;
        }

        void Restart() noexcept
        {
            started_=GetTickCount64();
            alpha_=0;
            if(shade_) SetLayeredWindowAttributes(shade_,0,0,LWA_ALPHA);
        }

        void Restore() noexcept
        {
            if(wallpaper_ && active_)
            {
                wallpaper_->SetBackgroundColor(priorColor_);
                wallpaper_->Enable(TRUE);
                if(slideSaved_)
                    wallpaper_->SetSlideshowOptions(slideOpts_,slideTick_);
            }
            DestroyCover();
            if(wallpaper_)
            {
                wallpaper_->Release();
                wallpaper_=nullptr;
            }
            active_=false;
            slideSaved_=false;
            if(comOwned_) CoUninitialize();
            comOwned_=false;
        }

    private:
        static constexpr wchar_t BaseClass[]=L"BkBase";
        static constexpr wchar_t ShadeClass[]=L"BkShade";

        static LRESULT CALLBACK WindowProc(
            HWND window,UINT message,WPARAM word,LPARAM value) noexcept
        {
            if(message==WM_ERASEBKGND)
            {
                RECT area{};
                GetClientRect(window,&area);
                const auto brush=reinterpret_cast<HBRUSH>(
                    GetClassLongPtrW(window,GCLP_HBRBACKGROUND));
                FillRect(reinterpret_cast<HDC>(word),&area,brush);
                return 1;
            }
            if(message==WM_PAINT)
            {
                PAINTSTRUCT paint{};
                const HDC dc=BeginPaint(window,&paint);
                RECT area{};
                GetClientRect(window,&area);
                const auto brush=reinterpret_cast<HBRUSH>(
                    GetClassLongPtrW(window,GCLP_HBRBACKGROUND));
                FillRect(dc,&area,brush);
                EndPaint(window,&paint);
                return 0;
            }
            return DefWindowProcW(window,message,word,value);
        }

        bool CreateCover() noexcept
        {
            instance_=GetModuleHandleW(nullptr);
            shadeBrush_=CreateSolidBrush(RGB(8,8,8));
            if(!shadeBrush_)
            {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return false;
            }
            WNDCLASSEXW baseClass{sizeof(baseClass)};
            baseClass.lpfnWndProc=WindowProc;
            baseClass.hInstance=instance_;
            baseClass.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            baseClass.lpszClassName=BaseClass;
            if(!RegisterClassExW(&baseClass)) return false;
            baseOwned_=true;
            WNDCLASSEXW shadeClass{sizeof(shadeClass)};
            shadeClass.lpfnWndProc=WindowProc;
            shadeClass.hInstance=instance_;
            shadeClass.hbrBackground=shadeBrush_;
            shadeClass.lpszClassName=ShadeClass;
            if(!RegisterClassExW(&shadeClass)) return false;
            shadeOwned_=true;
            host_=FindWindowW(L"Progman",nullptr);
            if(!host_)
            {
                SetLastError(ERROR_NOT_FOUND);
                return false;
            }
            POINT origin{};
            const HMONITOR monitor=MonitorFromPoint(
                origin,MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO info{sizeof(info)};
            if(!GetMonitorInfoW(monitor,&info)) return false;
            origin={info.rcMonitor.left,info.rcMonitor.top};
            if(!ScreenToClient(host_,&origin)) return false;
            width_=info.rcMonitor.right-info.rcMonitor.left;
            height_=info.rcMonitor.bottom-info.rcMonitor.top;
            base_=CreateWindowExW(
                WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,BaseClass,L"",
                WS_CHILD|WS_VISIBLE,origin.x,origin.y,width_,height_,
                host_,nullptr,instance_,nullptr);
            if(!base_) return false;
            if(!SetLayeredWindowAttributes(base_,0,255,LWA_ALPHA)) return false;
            shade_=CreateWindowExW(
                WS_EX_LAYERED|WS_EX_NOACTIVATE,ShadeClass,L"",
                WS_CHILD|WS_VISIBLE,0,0,width_,height_,
                base_,nullptr,instance_,nullptr);
            if(!shade_) return false;
            if(!SetLayeredWindowAttributes(shade_,0,0,LWA_ALPHA)) return false;
            RedrawWindow(base_,nullptr,nullptr,
                RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
            coverShown_=true;
            return PositionCover();
        }

        bool PositionCover() noexcept
        {
            if(!base_ || !host_) return false;
            const HWND icons=FindWindowExW(
                host_,nullptr,L"SHELLDLL_DefView",nullptr);
            POINT origin{};
            const HMONITOR monitor=MonitorFromPoint(
                origin,MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO info{sizeof(info)};
            if(!GetMonitorInfoW(monitor,&info)) return false;
            origin={info.rcMonitor.left,info.rcMonitor.top};
            if(!ScreenToClient(host_,&origin)) return false;
            return SetWindowPos(base_,icons?icons:HWND_TOP,
                origin.x,origin.y,width_,height_,
                SWP_NOACTIVATE|SWP_SHOWWINDOW)!=FALSE;
        }

        bool ShowCover() noexcept
        {
            if(!base_ || !PositionCover()) return false;
            coverShown_=true;
            return true;
        }

        void DestroyCover() noexcept
        {
            if(base_) DestroyWindow(base_);
            shade_=nullptr;
            base_=nullptr;
            coverShown_=false;
            if(shadeOwned_) UnregisterClassW(ShadeClass,instance_);
            if(baseOwned_) UnregisterClassW(BaseClass,instance_);
            shadeOwned_=false;
            baseOwned_=false;
            if(shadeBrush_) DeleteObject(shadeBrush_);
            shadeBrush_=nullptr;
            host_=nullptr;
        }

        void CloseCom() noexcept
        {
            if(wallpaper_)
            {
                wallpaper_->Release();
                wallpaper_=nullptr;
            }
            if(comOwned_) CoUninitialize();
            comOwned_=false;
        }

        IDesktopWallpaper* wallpaper_{};
        HINSTANCE instance_{};
        HWND host_{},base_{},shade_{};
        HBRUSH shadeBrush_{};
        COLORREF priorColor_{};
        DESKTOP_SLIDESHOW_OPTIONS slideOpts_{};
        ULONGLONG started_{};
        UINT slideTick_{};
        int width_{},height_{};
        BYTE alpha_{};
        bool comOwned_{},active_{};
        bool baseOwned_{},shadeOwned_{},coverShown_{};
        bool slideSaved_{};
    };
}
