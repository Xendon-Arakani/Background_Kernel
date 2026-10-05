#include "app/SceneSignals.hpp"
#include "render/WallStrokeCanvas.hpp"
#include "stroke/StrokeAudioColor.hpp"
#include "stroke/WallStrokeSettings.hpp"
#include "visualizer/AdaptiveTrail.hpp"
#include "visualizer/SceneSettings.hpp"
#include "visualizer/QuinticMotion.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cwctype>
#include <deque>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace
{
    using bk::stroke::Color;
    using bk::stroke::Point;
    using bk::stroke::Settings;

    constexpr wchar_t MainClass[]=L"StrokeLab";
    constexpr wchar_t CanvasClass[]=L"WallStrokeCanvas";
    constexpr int VolumeSlider=500;
    constexpr int SaveButton=501;
    constexpr int ClearButton=502;
    constexpr int VolumeEdit=503;
    constexpr int EditBase=600;

    struct EditField
    {
        const wchar_t* label{};
        double Settings::*member{};
        int row{},column{};
    };

    constexpr std::array<EditField,5> Fields{{
        {L"Rest width (cm)",&Settings::restWidthCm,0,0},
        {L"Move width (cm)",&Settings::fullWidthCm,0,1},
        {L"Quiet trail (cm)",&Settings::quietTrailCm,1,0},
        {L"Loud trail (cm)",&Settings::loudTrailCm,1,1},
        {L"Wall opacity (%)",&Settings::opacityPct,2,0}
    }};

    class StrokeDraft
    {
        struct Target
        {
            Point point{};
            Color color{};
            DWORD time{};
        };

        struct Line
        {
            Point start{},end{};
            float length{};
            float startRadius{},endRadius{};
            Color startColor{},endColor{};
        };

    public:
        void Configure(const Settings& settings,
            const bk::visualizer::VerticalDbfsRange& dbRange,
            const int width,const int height)
        {
            settings_=settings;
            dbRange_=dbRange;
            width_=std::max(width,1);
            height_=std::max(height,1);
            scale_=std::min(width_/static_cast<float>(bk::scene::ScreenChordMm),
                height_/static_cast<float>(bk::scene::ScreenHeightMm));
            compiled_=bk::stroke::Compile(settings_,scale_);
            radius_=compiled_.restRadiusPx;
            Trim();
            Render();
        }

        void Resize(const int width,const int height)
        {
            Configure(settings_,dbRange_,width,height);
            Clear();
        }

        void Clear()
        {
            targets_.clear();
            lines_.clear();
            shapes_.clear();
            velocity_={};
            acceleration_={};
            radius_=compiled_.restRadiusPx;
        }

        void Sample(const float dbfs)
        {
            samples_.push_back(dbfs);
            sampleSum_+=dbfs;
            if(samples_.size()>bk::visualizer::TickRate)
            {
                sampleSum_-=samples_.front();
                samples_.pop_front();
            }
            averageDbfs_=samples_.empty()?static_cast<float>(dbRange_.bottom):
                sampleSum_/static_cast<float>(samples_.size());
            Render();
        }

        void Add(const Point point,const Color color,const DWORD time)
        {
            targets_.push_back({point,color,time});
            if(targets_.size()<3) return;
            Draw();
            targets_.pop_front();
        }

        void Finish()
        {
            if(targets_.size()==2)
            {
                targets_.push_back(targets_.back());
                Draw();
            }
            targets_.clear();
            velocity_={};
            acceleration_={};
        }

        std::span<const bk::stroke::Shape> Shapes() const noexcept
        {
            return shapes_;
        }

        float Radius() const noexcept
        {
            return radius_;
        }

        float AverageDbfs() const noexcept
        {
            return averageDbfs_;
        }

    private:
        void Draw()
        {
            const auto& current=targets_[0];
            const auto& next=targets_[1];
            const auto& future=targets_[2];
            const auto convert=[](const Point point)
            {
                return bk::visualizer::MotionPoint{point.x,point.y,0};
            };
            const auto solved=bk::visualizer::SolveMinJerk(
                convert(current.point),convert(next.point),convert(future.point),
                velocity_,acceleration_,{0,0,-1},
                {static_cast<double>(width_),static_cast<double>(height_),1});
            velocity_=bk::visualizer::Velocity(solved.controls,1);
            acceleration_=bk::visualizer::Acceleration(solved.controls,1);
            const DWORD elapsed=std::max<DWORD>(next.time-current.time,1);
            const float distance=std::hypot(next.point.x-current.point.x,
                next.point.y-current.point.y);
            const float speedMm=1000*distance/(scale_*elapsed);
            const float fullSpeed=static_cast<float>(
                bk::scene::ScreenHeightMm*bk::visualizer::TickRate);
            const float response=std::clamp(speedMm/fullSpeed,0.0f,1.0f);
            const float nextRadius=std::lerp(compiled_.restRadiusPx,
                compiled_.fullRadiusPx,response);
            Flatten(solved.controls,0,1,current.color,next.color,
                radius_,nextRadius);
            radius_=nextRadius;
            Trim();
            Render();
        }

        void Flatten(const bk::visualizer::QuinticControls& controls,
            const double from,const double to,const Color& startColor,
            const Color& endColor,const float startRadius,
            const float endRadius)
        {
            double error{};
            for(std::size_t index=1;index+1<controls.size();++index)
            {
                const auto chord=bk::visualizer::Mix(controls.front(),
                    controls.back(),static_cast<double>(index)/5);
                error=std::max(error,bk::visualizer::Length(
                    controls[index]-chord));
            }
            const double middle=std::midpoint(from,to);
            if(error<=bk::visualizer::trail_detail::CoverageErrorMm*scale_ ||
                middle==from || middle==to)
            {
                const Point start{static_cast<float>(controls.front().x),
                    static_cast<float>(controls.front().y)};
                const Point end{static_cast<float>(controls.back().x),
                    static_cast<float>(controls.back().y)};
                const float length=std::hypot(end.x-start.x,end.y-start.y);
                if(length>0)
                {
                    const auto mixColor=[&](const double amount)
                    {
                        return Color{
                            std::lerp(startColor.red,endColor.red,
                                static_cast<float>(amount)),
                            std::lerp(startColor.green,endColor.green,
                                static_cast<float>(amount)),
                            std::lerp(startColor.blue,endColor.blue,
                                static_cast<float>(amount))};
                    };
                    lines_.push_back({start,end,length,
                        std::lerp(startRadius,endRadius,static_cast<float>(from)),
                        std::lerp(startRadius,endRadius,static_cast<float>(to)),
                        mixColor(from),mixColor(to)});
                }
                return;
            }
            const auto halves=bk::visualizer::SplitBezier(controls,0.5);
            Flatten(halves[0],from,middle,startColor,endColor,
                startRadius,endRadius);
            Flatten(halves[1],middle,to,startColor,endColor,
                startRadius,endRadius);
        }

        void Trim()
        {
            const float limit=std::max(compiled_.quietTrailPx,
                compiled_.loudTrailPx);
            float length{};
            for(auto it=lines_.rbegin();it!=lines_.rend();++it)
                length+=it->length;
            while(lines_.size()>1 && length-lines_.front().length>=limit)
            {
                length-=lines_.front().length;
                lines_.pop_front();
            }
            if(!(limit>0)) lines_.clear();
        }

        void Render()
        {
            shapes_.clear();
            const float range=static_cast<float>(dbRange_.top-dbRange_.bottom);
            const float level=range>0?std::clamp(
                (averageDbfs_-static_cast<float>(dbRange_.bottom))/range,
                0.0f,1.0f):0;
            const float span=std::lerp(compiled_.quietTrailPx,
                compiled_.loudTrailPx,level);
            if(!(span>0)) return;
            std::vector<Line> visible;
            float used{};
            for(auto it=lines_.rbegin();it!=lines_.rend() && used<span;++it)
            {
                Line line=*it;
                if(used+line.length>span)
                {
                    const float cut=1-(span-used)/line.length;
                    line.start={std::lerp(line.start.x,line.end.x,cut),
                        std::lerp(line.start.y,line.end.y,cut)};
                    line.startRadius=std::lerp(line.startRadius,line.endRadius,cut);
                    line.startColor={
                        std::lerp(line.startColor.red,line.endColor.red,cut),
                        std::lerp(line.startColor.green,line.endColor.green,cut),
                        std::lerp(line.startColor.blue,line.endColor.blue,cut)};
                    line.length=std::hypot(line.end.x-line.start.x,
                        line.end.y-line.start.y);
                }
                visible.push_back(line);
                used+=line.length;
            }
            float fromTail{};
            for(auto it=visible.rbegin();it!=visible.rend();++it)
            {
                const float startOpacity=std::clamp(fromTail/used,0.0f,1.0f);
                fromTail+=it->length;
                const float endOpacity=std::clamp(fromTail/used,0.0f,1.0f);
                shapes_.push_back({
                    {it->start.x,it->start.y,it->startRadius,startOpacity},
                    {it->end.x,it->end.y,it->endRadius,endOpacity},
                    {it->startColor.red,it->startColor.green,
                        it->startColor.blue,0},
                    {it->endColor.red,it->endColor.green,
                        it->endColor.blue,0}});
            }
        }

        Settings settings_{};
        bk::stroke::Compiled compiled_{};
        bk::visualizer::VerticalDbfsRange dbRange_{};
        std::deque<Target> targets_;
        std::deque<Line> lines_;
        std::deque<float> samples_;
        std::vector<bk::stroke::Shape> shapes_;
        bk::visualizer::MotionPoint velocity_{},acceleration_{};
        int width_{1},height_{1};
        float scale_{1},radius_{},sampleSum_{},averageDbfs_{-100};
    };

    class Workshop
    {
    public:
        ~Workshop()
        {
            color_.Shutdown();
            renderer_.reset();
            if(window_ && IsWindow(window_)) DestroyWindow(window_);
            if(claimed_ && ready_) ResetEvent(ready_);
            if(ready_) CloseHandle(ready_);
            if(stop_) CloseHandle(stop_);
            if(font_) DeleteObject(font_);
            if(heading_) DeleteObject(heading_);
            if(panelBrush_) DeleteObject(panelBrush_);
        }

        int Run(const HINSTANCE instance)
        {
            instance_=instance;
            stop_=CreateEventW(nullptr,TRUE,FALSE,
                bk::app::surface::WallStrokeStop);
            if(!stop_) throw std::runtime_error(
                "Wall stroke stop signal unavailable");
            if(GetLastError()==ERROR_ALREADY_EXISTS) return ERROR_BUSY;
            claimed_=true;
            ready_=CreateEventW(nullptr,TRUE,FALSE,
                bk::app::surface::WallStrokeReady);
            if(!ready_) throw std::runtime_error(
                "Wall stroke ready signal unavailable");
            ResetEvent(ready_);
            const INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES};
            if(!InitCommonControlsEx(&controls)) throw std::runtime_error(
                "Wall stroke slider unavailable");
            WNDCLASSEXW main{};
            main.cbSize=sizeof(main);
            main.hInstance=instance;
            main.lpfnWndProc=MainProc;
            main.lpszClassName=MainClass;
            main.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            panelBrush_=CreateSolidBrush(RGB(25,28,34));
            main.hbrBackground=panelBrush_;
            if(!RegisterClassExW(&main)) throw std::runtime_error(
                "Wall stroke window class unavailable");
            WNDCLASSEXW canvas=main;
            canvas.lpfnWndProc=CanvasProc;
            canvas.lpszClassName=CanvasClass;
            canvas.hbrBackground=nullptr;
            canvas.hCursor=LoadCursorW(nullptr,IDC_CROSS);
            if(!RegisterClassExW(&canvas)) throw std::runtime_error(
                "Wall stroke canvas class unavailable");
            applied_=bk::stroke::Load();
            opacity_=bk::stroke::Compile(applied_,1).opacity;
            if(bk::visualizer::settings::ReadVerticalRange(dbRange_)!=ERROR_SUCCESS)
                dbRange_={};
            fakeDbfs_=bk::stroke::LoadLabDbfs(
                static_cast<float>(dbRange_.bottom));
            color_.Start();
            window_=CreateWindowExW(0,MainClass,L"Wall stroke",
                WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,
                1400,900,nullptr,nullptr,instance,this);
            if(!window_) throw std::runtime_error("Wall stroke window unavailable");
            dpi_=GetDpiForWindow(window_);
            font_=CreateFontW(-MulDiv(10,static_cast<int>(dpi_),72),0,0,0,
                FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            heading_=CreateFontW(-MulDiv(15,static_cast<int>(dpi_),72),0,0,0,
                FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            canvas_=CreateWindowExW(0,CanvasClass,L"",WS_CHILD|WS_VISIBLE,
                0,0,1,1,window_,nullptr,instance,this);
            if(!canvas_) throw std::runtime_error(
                "Wall stroke drawing surface unavailable");
            BuildControls();
            FillControls();
            Layout();
            ShowWindow(window_,SW_SHOWMAXIMIZED);
            UpdateWindow(window_);
            Layout();
            draft_.Configure(applied_,dbRange_,canvasWidth_,canvasHeight_);
            renderer_=std::make_unique<bk::render::WallStrokeCanvas>();
            renderer_->Initialize(canvas_,canvasWidth_,canvasHeight_);
            Draw();
            if(WaitForSingleObject(stop_,0)==WAIT_OBJECT_0) return 0;
            SetEvent(ready_);
            LARGE_INTEGER frequency{},previous{};
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&previous);
            double accumulated{};
            bool running=true;
            while(running)
            {
                const bool minimized=IsIconic(window_)!=FALSE;
                const HANDLE handles[]{stop_,renderer_->FrameReady()};
                const DWORD result=MsgWaitForMultipleObjects(minimized?1:2,
                    handles,FALSE,minimized?100:INFINITE,QS_ALLINPUT);
                if(result==WAIT_OBJECT_0) break;
                if(result==WAIT_FAILED) throw std::runtime_error(
                    "Wall stroke frame wait failed");
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))
                {
                    if(message.message==WM_QUIT)
                    {
                        failure_=static_cast<int>(message.wParam);
                        running=false;
                        break;
                    }
                    if(!IsDialogMessageW(window_,&message))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                if(!running || WaitForSingleObject(stop_,0)==WAIT_OBJECT_0) break;
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                accumulated+=static_cast<double>(now.QuadPart-previous.QuadPart)/
                    frequency.QuadPart;
                previous=now;
                color_.Update();
                constexpr double step=1.0/bk::visualizer::TickRate;
                while(accumulated>=step)
                {
                    draft_.Sample(fakeDbfs_);
                    accumulated-=step;
                }
                if(result==WAIT_OBJECT_0+1 && !IsIconic(window_)) Draw();
            }
            bk::stroke::StoreLabDbfs(fakeDbfs_);
            return failure_;
        }

    private:
        int Scale(const int pixels) const noexcept
        {
            return MulDiv(pixels,static_cast<int>(dpi_),96);
        }

        HWND Control(const wchar_t* type,const wchar_t* text,const DWORD style,
            const int id=0)
        {
            const HWND control=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,
                0,0,1,1,window_,reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(id)),instance_,nullptr);
            if(!control) throw std::runtime_error(
                "Wall stroke control unavailable");
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            return control;
        }

        template<class Number>
        static void SetNumber(const HWND control,const Number value)
        {
            char narrow[64]{};
            const auto result=std::to_chars(narrow,narrow+sizeof(narrow)-1,value);
            if(result.ec!=std::errc{}) return;
            wchar_t wide[64]{};
            const auto length=static_cast<std::size_t>(result.ptr-narrow);
            for(std::size_t index=0;index<length;++index)
                wide[index]=static_cast<unsigned char>(narrow[index]);
            SetWindowTextW(control,wide);
        }

        void BuildControls()
        {
            title_=Control(L"STATIC",L"Wall stroke workshop",0);
            SendMessageW(title_,WM_SETFONT,reinterpret_cast<WPARAM>(heading_),FALSE);
            hint_=Control(L"STATIC",L"",0);
            volumeText_=Control(L"STATIC",L"",0);
            slider_=Control(TRACKBAR_CLASSW,L"",
                TBS_HORZ|TBS_AUTOTICKS|WS_TABSTOP,VolumeSlider);
            SendMessageW(slider_,TBM_SETRANGEMIN,FALSE,0);
            SendMessageW(slider_,TBM_SETRANGEMAX,FALSE,10000);
            SendMessageW(slider_,TBM_SETTICFREQ,2500,0);
            SendMessageW(slider_,TBM_SETLINESIZE,0,1);
            SendMessageW(slider_,TBM_SETPAGESIZE,0,100);
            volumeEdit_=Control(L"EDIT",L"",
                WS_BORDER|ES_AUTOHSCROLL|ES_RIGHT|WS_TABSTOP,VolumeEdit);
            endpoints_=Control(L"STATIC",L"",0);
            for(std::size_t index=0;index<Fields.size();++index)
            {
                labels_[index]=Control(L"STATIC",Fields[index].label,0);
                edits_[index]=Control(L"EDIT",L"",
                    WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,
                    EditBase+static_cast<int>(index));
            }
            colorLabel_=Control(L"STATIC",L"Orb color",0);
            colorText_=Control(L"STATIC",L"",0);
            save_=Control(L"BUTTON",L"Save",BS_DEFPUSHBUTTON|WS_TABSTOP,
                SaveButton);
            clear_=Control(L"BUTTON",L"Clear canvas",
                BS_PUSHBUTTON|WS_TABSTOP,ClearButton);
            paintToggle_=Control(L"BUTTON",L"Enable wall paint",
                BS_AUTOCHECKBOX|WS_TABSTOP);
            status_=Control(L"STATIC",L"",0);
            UpdateVolume();
        }

        void FillControls()
        {
            for(std::size_t index=0;index<Fields.size();++index)
            {
                SetNumber(edits_[index],applied_.*Fields[index].member);
            }
            const auto label=color_.Palette()+L" / audio";
            SetWindowTextW(colorText_,label.c_str());
            SendMessageW(paintToggle_,BM_SETCHECK,
                applied_.paintEnabled?BST_CHECKED:BST_UNCHECKED,0);
            UpdateVolume();
        }

        void Layout()
        {
            if(!canvas_) return;
            RECT client{};
            GetClientRect(window_,&client);
            const int panel=Scale(350),gap=Scale(18),half=Scale(149);
            const int width=std::max(1,static_cast<int>(client.right)-panel);
            const int height=std::max(1,static_cast<int>(client.bottom));
            if(width!=canvasWidth_ || height!=canvasHeight_)
            {
                canvasWidth_=width;
                canvasHeight_=height;
                MoveWindow(canvas_,0,0,width,height,TRUE);
                draft_.Resize(width,height);
                if(renderer_) renderer_->Resize(width,height);
            }
            const int x=width+gap,full=panel-2*gap;
            const auto place=[&](const HWND control,const int left,
                const int top,const int w,const int h)
            {
                MoveWindow(control,left,Scale(top),w,Scale(h),TRUE);
            };
            place(title_,x,18,full,30);
            place(hint_,x,58,full,44);
            place(volumeText_,x,112,full,44);
            place(slider_,x,161,full-Scale(100),32);
            place(volumeEdit_,x+full-Scale(90),164,Scale(90),27);
            place(endpoints_,x,194,full,24);
            for(std::size_t index=0;index<Fields.size();++index)
            {
                const int left=x+Fields[index].column*(half+Scale(14));
                const int top=231+Fields[index].row*67;
                place(labels_[index],left,top,half,22);
                place(edits_[index],left,top+24,half,27);
            }
            place(paintToggle_,x,425,full,24);
            place(colorLabel_,x,454,half,22);
            place(colorText_,x,478,full,40);
            place(save_,x,540,half,32);
            place(clear_,x+half+Scale(14),540,half,32);
            place(status_,x,590,full,90);
        }

        void UpdateVolume()
        {
            if(!slider_) return;
            const float range=static_cast<float>(dbRange_.top-dbRange_.bottom);
            const float level=range>0?std::clamp(
                (fakeDbfs_-static_cast<float>(dbRange_.bottom))/range,
                0.0f,1.0f):0;
            SendMessageW(slider_,TBM_SETPOS,TRUE,
                static_cast<LPARAM>(std::lround(level*10000)));
            SetNumber(volumeEdit_,fakeDbfs_);
            wchar_t text[160]{};
            swprintf_s(text,L"Lab volume: %.2f dBFS\nOne-second average: %.2f dBFS",
                fakeDbfs_,draft_.AverageDbfs());
            SetWindowTextW(volumeText_,text);
            swprintf_s(text,L"Bottom %.5g                         Top %.5g",
                dbRange_.bottom,dbRange_.top);
            SetWindowTextW(endpoints_,text);
        }

        void SliderMoved(const bool snap)
        {
            if(snap)
            {
                const auto position=SendMessageW(slider_,TBM_GETPOS,0,0);
                SendMessageW(slider_,TBM_SETPOS,TRUE,
                    ((position+50)/100)*100);
            }
            const float level=static_cast<float>(
                SendMessageW(slider_,TBM_GETPOS,0,0))/10000;
            fakeDbfs_=std::lerp(static_cast<float>(dbRange_.bottom),
                static_cast<float>(dbRange_.top),level);
            UpdateVolume();
        }

        static bool ReadEdit(const HWND edit,double& value)
        {
            wchar_t text[96]{};
            GetWindowTextW(edit,text,96);
            wchar_t* end{};
            errno=0;
            value=std::wcstod(text,&end);
            if(end==text || errno==ERANGE || !std::isfinite(value)) return false;
            while(*end && std::iswspace(*end)) ++end;
            return *end==L'\0';
        }

        void ApplyVolume()
        {
            double value{};
            if(!ReadEdit(volumeEdit_,value) || value<dbRange_.bottom ||
                value>dbRange_.top)
            {
                SetNumber(volumeEdit_,fakeDbfs_);
                SetWindowTextW(status_,
                    L"Enter a finite lab volume inside the displayed dBFS range.");
                return;
            }
            fakeDbfs_=static_cast<float>(value);
            UpdateVolume();
            bk::stroke::StoreLabDbfs(fakeDbfs_);
        }

        void Save()
        {
            auto candidate=applied_;
            candidate.paintEnabled=
                SendMessageW(paintToggle_,BM_GETCHECK,0,0)==BST_CHECKED;
            for(std::size_t index=0;index<Fields.size();++index)
            {
                if(!ReadEdit(edits_[index],candidate.*Fields[index].member))
                {
                    SetWindowTextW(status_,
                        L"Enter a finite number in every field. Nothing changed.");
                    SetFocus(edits_[index]);
                    return;
                }
            }
            if(!bk::stroke::Valid(candidate)) return;
            if(bk::stroke::Store(candidate)!=ERROR_SUCCESS)
            {
                SetWindowTextW(status_,
                    L"Windows could not save settings. Nothing changed.");
                return;
            }
            applied_=candidate;
            opacity_=bk::stroke::Compile(applied_,1).opacity;
            draft_.Configure(applied_,dbRange_,canvasWidth_,canvasHeight_);
            color_.Configure();
            FillControls();
            SetWindowTextW(status_,L"Saved and compiled.");
            SetFocus(canvas_);
        }

        void Pointer(const HWND canvas,const LPARAM position,const bool)
        {
            const Point point{
                std::clamp(static_cast<float>(GET_X_LPARAM(position)),
                    0.0f,static_cast<float>(canvasWidth_)),
                std::clamp(static_cast<float>(GET_Y_LPARAM(position)),
                    0.0f,static_cast<float>(canvasHeight_))};
            cursor_=point;
            if(!hovering_)
            {
                TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,canvas,0};
                TrackMouseEvent(&track);
                hovering_=true;
            }
            if(drawing_)
            {
                color_.Update();
                draft_.Add(point,color_.Current(),
                    static_cast<DWORD>(GetMessageTime()));
            }
        }

        void Draw()
        {
            const auto shapes=applied_.paintEnabled?draft_.Shapes():
                std::span<const bk::stroke::Shape>{};
            renderer_->Draw(shapes,cursor_,color_.Current(),
                draft_.Radius(),opacity_,
                hovering_,drawing_);
        }

        static Workshop* Self(const HWND window,const UINT message,
            const LPARAM state)
        {
            if(message==WM_NCCREATE)
            {
                const auto* create=reinterpret_cast<const CREATESTRUCTW*>(state);
                SetWindowLongPtrW(window,GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            }
            return reinterpret_cast<Workshop*>(
                GetWindowLongPtrW(window,GWLP_USERDATA));
        }

        static LRESULT CALLBACK MainProc(const HWND window,const UINT message,
            const WPARAM value,const LPARAM state)
        {
            auto* self=Self(window,message,state);
            if(!self) return DefWindowProcW(window,message,value,state);
            try
            {
                switch(message)
                {
                case WM_SIZE:self->Layout();return 0;
                case WM_MOVE:
                case WM_DISPLAYCHANGE:
                    if(self->renderer_) self->renderer_->RefreshDisplay();
                    return 0;
                case WM_HSCROLL:
                    if(reinterpret_cast<HWND>(state)==self->slider_)
                    {
                        self->SliderMoved(LOWORD(value)==TB_THUMBTRACK ||
                            LOWORD(value)==TB_THUMBPOSITION);
                        if(LOWORD(value)==TB_ENDTRACK)
                            bk::stroke::StoreLabDbfs(self->fakeDbfs_);
                    }
                    return 0;
                case WM_COMMAND:
                    if(LOWORD(value)==VolumeEdit &&
                        HIWORD(value)==EN_KILLFOCUS)
                        self->ApplyVolume();
                    else if(LOWORD(value)==SaveButton && HIWORD(value)==BN_CLICKED)
                        self->Save();
                    else if(LOWORD(value)==ClearButton &&
                        HIWORD(value)==BN_CLICKED)
                    {
                        self->draft_.Clear();
                        SetFocus(self->canvas_);
                    }
                    return 0;
                case WM_CTLCOLORSTATIC:
                {
                    const auto dc=reinterpret_cast<HDC>(value);
                    SetTextColor(dc,RGB(225,230,237));
                    SetBkColor(dc,RGB(25,28,34));
                    return reinterpret_cast<LRESULT>(self->panelBrush_);
                }
                case WM_CLOSE:DestroyWindow(window);return 0;
                case WM_DESTROY:PostQuitMessage(self->failure_);return 0;
                }
            }
            catch(const std::exception& error)
            {
                self->failure_=1;
                OutputDebugStringA(error.what());
                PostQuitMessage(1);
            }
            return DefWindowProcW(window,message,value,state);
        }

        static LRESULT CALLBACK CanvasProc(const HWND window,const UINT message,
            const WPARAM value,const LPARAM state)
        {
            auto* self=Self(window,message,state);
            if(!self) return DefWindowProcW(window,message,value,state);
            switch(message)
            {
            case WM_ERASEBKGND:return 1;
            case WM_LBUTTONDOWN:
                SetFocus(window);
                SetCapture(window);
                self->drawing_=true;
                self->Pointer(window,state,true);
                return 0;
            case WM_MOUSEMOVE:self->Pointer(window,state,false);return 0;
            case WM_LBUTTONUP:
                self->Pointer(window,state,false);
                self->draft_.Finish();
                self->drawing_=false;
                if(GetCapture()==window) ReleaseCapture();
                return 0;
            case WM_CAPTURECHANGED:
                self->draft_.Finish();
                self->drawing_=false;
                return 0;
            case WM_MOUSELEAVE:self->hovering_=false;return 0;
            case WM_RBUTTONDOWN:self->draft_.Clear();return 0;
            case WM_KEYDOWN:
                if(value==VK_ESCAPE)
                {
                    PostMessageW(self->window_,WM_CLOSE,0,0);
                    return 0;
                }
                break;
            }
            return DefWindowProcW(window,message,value,state);
        }

        HINSTANCE instance_{};
        HWND window_{},canvas_{},title_{},hint_{},volumeText_{},slider_{},volumeEdit_{};
        HWND endpoints_{},colorLabel_{},colorText_{},save_{},clear_{},status_{};
        HWND paintToggle_{};
        std::array<HWND,Fields.size()> labels_{},edits_{};
        HFONT font_{},heading_{};
        HBRUSH panelBrush_{};
        HANDLE stop_{},ready_{};
        bool claimed_{},hovering_{},drawing_{};
        UINT dpi_{96};
        int canvasWidth_{1},canvasHeight_{1},failure_{};
        float fakeDbfs_{-100};
        Point cursor_{};
        Settings applied_{};
        float opacity_{};
        bk::visualizer::VerticalDbfsRange dbRange_{};
        StrokeDraft draft_;
        bk::stroke::AudioColor color_;
        std::unique_ptr<bk::render::WallStrokeCanvas> renderer_;
    };
}

int WINAPI wWinMain(const HINSTANCE instance,HINSTANCE,PWSTR commandLine,int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(commandLine && wcsstr(commandLine,L"--stop"))
    {
        const HANDLE stop=OpenEventW(EVENT_MODIFY_STATE,FALSE,
            bk::app::surface::WallStrokeStop);
        if(stop)
        {
            SetEvent(stop);
            CloseHandle(stop);
        }
        return 0;
    }
    try
    {
        Workshop workshop;
        return workshop.Run(instance);
    }
    catch(const std::exception& error)
    {
        OutputDebugStringA(error.what());
        MessageBoxA(nullptr,error.what(),"Wall stroke workshop failed",
            MB_OK|MB_ICONERROR);
        return 1;
    }
}
