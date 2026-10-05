#pragma once
#include "stroke/WallStrokeSettings.hpp"
#include "stroke/RayOrigin.hpp"
#include "visualizer/SceneSettings.hpp"
#include "tracking/HeadTrackState.hpp"
#include <commctrl.h>
#include <charconv>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <string>

namespace bk::app
{
    class SceneSettingsPanel
    {
    public:
        ~SceneSettingsPanel() { if(window_) DestroyWindow(window_); }
        void Show()
        {
            if(window_) { RefreshTrackStatus();ShowWindow(window_,SW_SHOWNORMAL);SetForegroundWindow(window_);AnnounceOpened();return; }
            INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES};
            InitCommonControlsEx(&controls);
            WNDCLASSW type{};type.lpfnWndProc=WindowProc;type.hInstance=GetModuleHandleW(nullptr);
            type.lpszClassName=L"BkSceneSettings";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            type.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
            if(!RegisterClassW(&type) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return;
            window_=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_TOOLWINDOW,type.lpszClassName,L"Scene settings",
                WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,1280,930,nullptr,nullptr,type.hInstance,this);
            if(window_) { ShowWindow(window_,SW_SHOW);SetForegroundWindow(window_);SetFocus(eyeDistance_);SendMessageW(eyeDistance_,EM_SETSEL,0,-1);AnnounceOpened(); }
        }
        bool Handle(MSG& message) { return window_ && IsDialogMessageW(window_,&message); }
        HWND Window() const { return window_; }
        void ShowError(const std::wstring& message)
        {
            Show();
            if(error_) SetWindowTextW(error_,message.c_str());
        }
    private:
        template<class Number>
        static void SetNumber(HWND control,const Number value)
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

        static bool ReadNumber(HWND control,double& value)
        {
            wchar_t input[128]{};
            GetWindowTextW(control,input,128);
            errno=0;
            wchar_t* tail{};
            value=std::wcstod(input,&tail);
            while(tail && iswspace(*tail)) ++tail;
            return tail!=input && tail && !*tail && errno!=ERANGE &&
                std::isfinite(value);
        }

        static LPARAM DialPosition(const double value)
        {
            return static_cast<LPARAM>(std::lround(
                std::clamp(value,0.0,1.0)*10000.0));
        }

        static void AnnounceOpened()
        {
            const auto event=OpenEventW(EVENT_MODIFY_STATE,FALSE,visualizer::settings::PanelOpenedEvent);
            if(event) { SetEvent(event);CloseHandle(event); }
        }
        void Build(HWND window)
        {
            const auto instance=GetModuleHandleW(nullptr);
            auto child=[&](const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
                const auto control=CreateWindowExW(std::wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,
                    x,y,w,h,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
                SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return control;
            };
            child(L"STATIC",L"Eye center (mm)",0,18,18,375,24,100);
            eyeDistance_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,18,48,160,26,101);
            double millimeters{}; const auto status=visualizer::settings::ReadEyeDistance(millimeters);
            if(status!=ERROR_SUCCESS) millimeters=scene::BaseEyeMm;
            SetNumber(eyeDistance_,millimeters);
            wchar_t geometry[256]{};swprintf_s(geometry,L"Box mm: %.2f x %.1f x %.1f",scene::ScreenChordMm,scene::ScreenHeightMm,scene::BackWallMm);
            child(L"STATIC",geometry,0,18,84,492,40,102);
            trackStatus_=child(L"STATIC",L"",0,18,128,396,54,106);
            child(L"BUTTON",L"Center head",WS_TABSTOP|BS_PUSHBUTTON,18,190,164,28,104);
            child(L"BUTTON",L"Refresh status",WS_TABSTOP|BS_PUSHBUTTON,194,190,120,28,105);
            visualizer::settings::SpectrumConfig spectrum;
            if(visualizer::settings::ReadSpectrum(spectrum)!=ERROR_SUCCESS) spectrum={};
            reverseTrackZ_=child(L"BUTTON",L"Reverse head depth",WS_TABSTOP|BS_AUTOCHECKBOX,18,225,320,26,114);
            SendMessageW(reverseTrackZ_,BM_SETCHECK,spectrum.reverseTrackZ?BST_CHECKED:BST_UNCHECKED,0);
            child(L"STATIC",L"Frequency anchors",0,18,259,480,20,115);
            for(int band=0;band<3;++band) {
                anchorEdit_[band]=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,18+160*band,283,148,25,110+band);
                SetNumber(anchorEdit_[band],spectrum.palette.anchorHz[band]);
            }
            child(L"STATIC",L"Entropy pursuit",0,18,398,485,20,118);
            child(L"STATIC",L"Focused half-life",0,18,424,150,20,119);
            child(L"STATIC",L"Uniform half-life",0,188,424,150,20,120);
            child(L"STATIC",L"Entropy curve",0,358,424,150,20,121);
            focusedLife_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,18,448,148,25,122);
            uniformLife_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,188,448,148,25,123);
            entropyCurve_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,358,448,148,25,124);
            visualizer::EntropyPursuit entropyPursuit;
            if(visualizer::settings::ReadEntropyPursuit(entropyPursuit)!=ERROR_SUCCESS) entropyPursuit={};
            SetNumber(focusedLife_,entropyPursuit.focusedHalfLifeSec);
            SetNumber(uniformLife_,entropyPursuit.uniformHalfLifeSec);
            SetNumber(entropyCurve_,entropyPursuit.curveExponent);
            child(L"STATIC",L"Volume pursuit",0,18,548,488,20,149);
            child(L"STATIC",L"Curve exponent",0,18,574,150,20,150);
            child(L"STATIC",L"Zero half-life",0,188,574,150,20,151);
            volumeCurve_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,18,598,148,25,152);
            volumeZero_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,188,598,148,25,153);
            visualizer::VolumePursuit volumePursuit;
            if(visualizer::settings::ReadVolumePursuit(volumePursuit)!=ERROR_SUCCESS)
                volumePursuit={};
            SetNumber(volumeCurve_,volumePursuit.curveExponent);
            SetNumber(volumeZero_,volumePursuit.zeroHalfLifeLevel);
            child(L"STATIC",L"Color palette",0,530,18,338,20,130);
            paletteChoice_=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
                530,42,338,180,155);
            for(const auto* name:std::array{
                    L"RGB",L"RBG",L"GRB",L"GBR",L"BRG",L"BGR",L"White"})
                SendMessageW(paletteChoice_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
            std::size_t paletteChoice{};
            if(visualizer::settings::ReadPaletteChoice(paletteChoice)!=ERROR_SUCCESS)
                paletteChoice=0;
            SendMessageW(paletteChoice_,CB_SETCURSEL,static_cast<WPARAM>(paletteChoice),0);
            child(L"STATIC",L"Paper white (nits)",0,530,190,338,20,132);
            paperWhite_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,530,216,148,25,133);
            double paperWhite{};
            if(visualizer::settings::ReadPaperWhite(paperWhite)!=ERROR_SUCCESS)
                paperWhite=render::hdr::BasePaperWhite;
            SetNumber(paperWhite_,paperWhite);
            visualizer::VerticalDbfsRange verticalRange;
            if(visualizer::settings::ReadVerticalRange(verticalRange)!=ERROR_SUCCESS)
                verticalRange={};
            child(L"STATIC",L"Vertical dBFS",0,530,326,338,20,135);
            child(L"STATIC",L"Bottom",0,530,352,148,20,136);
            child(L"STATIC",L"Reset top",0,700,352,148,20,137);
            verticalBottom_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,530,376,148,25,138);
            SetNumber(verticalBottom_,verticalRange.bottom);
            verticalTop_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,700,376,148,25,139);
            SetNumber(verticalTop_,verticalRange.top);
            visualizer::TopDecay topDecay;
            if(visualizer::settings::ReadTopDecay(topDecay)!=ERROR_SUCCESS)
                topDecay={};
            child(L"STATIC",L"Seconds per dB",0,530,411,148,20,157);
            child(L"STATIC",L"Lowest top (dBFS)",0,700,411,148,20,158);
            topSeconds_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,530,435,148,25,159);
            SetNumber(topSeconds_,topDecay.secondsPerDb);
            lowestTopDbfs_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,700,435,148,25,160);
            SetNumber(lowestTopDbfs_,topDecay.floorDbfs);
            child(L"STATIC",L"Sphere size",0,530,541,338,20,141);
            child(L"STATIC",L"Maximum (ref px)",0,530,567,148,20,146);
            child(L"STATIC",L"Curve exponent",0,700,567,148,20,147);
            maxDiameterPx_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,530,591,148,25,148);
            sizeCurve_=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,700,591,148,25,142);
            visualizer::MoveSizeMap sizeControls;
            if(visualizer::settings::ReadMoveSizeMap(sizeControls)!=ERROR_SUCCESS)
                sizeControls={};
            SetNumber(maxDiameterPx_,sizeControls.maxDiameterPx);
            SetNumber(sizeCurve_,sizeControls.curveExponent);
            invertSize_=child(L"BUTTON",L"Invert size",
                WS_TABSTOP|BS_AUTOCHECKBOX,530,625,338,24,156);
            SendMessageW(invertSize_,BM_SETCHECK,sizeControls.inverted?BST_CHECKED:BST_UNCHECKED,0);
            child(L"STATIC",L"Present sync",0,530,703,338,20,144);
            syncMode_=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,530,727,338,100,145);
            SendMessageW(syncMode_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Adaptive / none"));
            SendMessageW(syncMode_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"VSync"));
            render::PresentationSync syncMode{};
            if(visualizer::settings::ReadSyncMode(syncMode)!=ERROR_SUCCESS)
                syncMode=render::PresentationSync::Immediate;
            SendMessageW(syncMode_,CB_SETCURSEL,static_cast<WPARAM>(syncMode),0);
            child(L"STATIC",L"Ray source",0,900,18,340,20,200);
            auto cast=stroke::LoadCast();
            const bool liveOrigin=originBus_.Open();
            castDraft_=cast;
            const std::array<double,3> castValue{cast.across,cast.down,cast.depth};
            for(int axis=0;axis<3;++axis)
            {
                const auto id=201+axis;
                sourceLabel_[axis]=child(L"STATIC",L"",0,900,48+42*axis,22,24,210+axis);
                sourceDial_[axis]=child(TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ|TBS_AUTOTICKS,
                    924,42+42*axis,224,32,id);
                sourceEdit_[axis]=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|ES_RIGHT,
                    1154,45+42*axis,66,25,260+axis);
                child(L"STATIC",L"%",0,1224,48+42*axis,16,24,263+axis);
                SendMessageW(sourceDial_[axis],TBM_SETRANGE,TRUE,MAKELPARAM(0,10000));
                SendMessageW(sourceDial_[axis],TBM_SETTICFREQ,2500,0);
                SendMessageW(sourceDial_[axis],TBM_SETLINESIZE,0,1);
                SendMessageW(sourceDial_[axis],TBM_SETPAGESIZE,0,100);
                SendMessageW(sourceDial_[axis],TBM_SETPOS,TRUE,
                    DialPosition(castValue[axis]));
            }
            UpdateCastFields();
            child(L"STATIC",L"Wall stroke",0,900,174,340,20,220);
            const auto strokeValue=stroke::Load();
            const std::array<const wchar_t*,5> names{
                L"Rest width (cm)",L"Move width (cm)",
                L"Quiet trail (cm)",L"Loud trail (cm)",L"Wall opacity (%)"};
            const std::array<double,5> values{strokeValue.restWidthCm,
                strokeValue.fullWidthCm,strokeValue.quietTrailCm,
                strokeValue.loudTrailCm,strokeValue.opacityPct};
            for(int entry=0;entry<5;++entry)
            {
                const int column=entry&1,row=entry/2,x=900+174*column,y=202+58*row;
                child(L"STATIC",names[entry],0,x,y,160,20,230+entry);
                strokeEdit_[entry]=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,
                    x,y+22,158,25,250+entry);
                SetNumber(strokeEdit_[entry],values[entry]);
            }
            paintEnabled_=child(L"BUTTON",L"Wall paint",
                WS_TABSTOP|BS_AUTOCHECKBOX,900,366,340,24,272);
            SendMessageW(paintEnabled_,BM_SETCHECK,
                strokeValue.paintEnabled?BST_CHECKED:BST_UNCHECKED,0);
            error_=child(L"STATIC",L"",0,18,771,1210,28,103);
            if(!liveOrigin)
                SetWindowTextW(error_,L"Windows could not open live ray origin controls.");
            child(L"BUTTON",L"Save",WS_TABSTOP|BS_DEFPUSHBUTTON,510,807,80,28,IDOK);
            child(L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON,602,807,80,28,IDCANCEL);
            RefreshTrackStatus();
        }
        void UpdateCastFields()
        {
            constexpr std::array<const wchar_t*,3> names{L"X",L"Y",L"Z"};
            const std::array<double,3> values{
                castDraft_.across,castDraft_.down,castDraft_.depth};
            for(int axis=0;axis<3;++axis)
            {
                SetWindowTextW(sourceLabel_[axis],names[axis]);
                SetNumber(sourceEdit_[axis],values[axis]*100.0);
            }
        }
        void CastChanged(const HWND snapDial)
        {
            BeginPreview();
            if(snapDial)
            {
                const auto position=SendMessageW(snapDial,TBM_GETPOS,0,0);
                SendMessageW(snapDial,TBM_SETPOS,TRUE,
                    ((position+50)/100)*100);
            }
            stroke::CastPoint cast{
                static_cast<double>(SendMessageW(sourceDial_[0],TBM_GETPOS,0,0))/10000,
                static_cast<double>(SendMessageW(sourceDial_[1],TBM_GETPOS,0,0))/10000,
                static_cast<double>(SendMessageW(sourceDial_[2],TBM_GETPOS,0,0))/10000};
            castDraft_=cast;
            UpdateCastFields();
            if(!originBus_.Publish(cast))
                SetWindowTextW(error_,L"Windows could not move the wall stroke ray source.");
        }
        bool ApplyOrigin(const int axis)
        {
            double percent{};
            if(!ReadNumber(sourceEdit_[axis],percent) ||
                percent<0.0 || percent>100.0)
            {
                UpdateCastFields();
                SetWindowTextW(error_,L"Enter each ray source axis from 0 through 100 percent.");
                SetFocus(sourceEdit_[axis]);
                return false;
            }
            BeginPreview();
            auto* field=axis==0?&castDraft_.across:
                axis==1?&castDraft_.down:&castDraft_.depth;
            *field=percent*0.01;
            SendMessageW(sourceDial_[axis],TBM_SETPOS,TRUE,
                DialPosition(*field));
            UpdateCastFields();
            if(!originBus_.Publish(castDraft_))
                SetWindowTextW(error_,L"Windows could not move the wall stroke ray source.");
            return true;
        }

        void BeginPreview()
        {
            if(castPreview_) return;
            castRestore_=castDraft_;
            originBus_.Read(castRestore_);
            castPreview_=true;
        }

        void RestoreOrigin()
        {
            if(!castPreview_) return;
            stroke::CastPoint current{};
            if(originBus_.Read(current) &&
                current.across==castDraft_.across &&
                current.down==castDraft_.down &&
                current.depth==castDraft_.depth)
                originBus_.Publish(castRestore_);
            castPreview_=false;
        }
        void RefreshTrackStatus()
        {
            tracking::HeadTrackState status;
            if(!tracking::ReadHeadTrackState(status)) { SetWindowTextW(trackStatus_,L"Scene: unavailable");return; }
            wchar_t text[256]{};
            if(status.socketError) swprintf_s(text,L"UDP receiver error %u on 127.0.0.1:4242.",status.socketError);
            else if(!status.validPackets) swprintf_s(text,L"UDP packets: 0");
            else {
                LARGE_INTEGER now{};QueryPerformanceCounter(&now);
                const double age=1000.0*static_cast<double>(now.QuadPart-status.recvQpc)/status.qpcFrequency;
                swprintf_s(text,L"UDP packets: %llu; age: %.1f ms\nEye XYZ mm: %.1f, %.1f, %.1f",
                    status.validPackets,age,status.eye[0],status.eye[1],status.eye[2]);
            }
            SetWindowTextW(trackStatus_,text);
        }
        void CenterHead()
        {
            const auto event=OpenEventW(EVENT_MODIFY_STATE,FALSE,tracking::HeadCenterEvent);
            if(!event) { SetWindowTextW(error_,L"Start the kernel with Numpad 9, then its scene with Numpad 8.");return; }
            SetEvent(event);CloseHandle(event);
            SetWindowTextW(error_,L"Neutral: requested");
            RefreshTrackStatus();
        }
        void Save()
        {
            const auto length=GetWindowTextLengthW(eyeDistance_);
            std::wstring text(static_cast<std::size_t>(length)+1,L'\0');GetWindowTextW(eyeDistance_,text.data(),length+1);
            wchar_t* end{};errno=0;const double millimeters=std::wcstod(text.c_str(),&end);
            while(end && iswspace(*end)) ++end;
            if(end==text.c_str() || !end || *end || errno==ERANGE || !scene::ValidEyeDistanceMm(millimeters))
            {
                SetWindowTextW(error_,L"Enter a finite distance beyond the curved screen edges.");SetFocus(eyeDistance_);return;
            }
            visualizer::settings::SpectrumConfig spectrum;
            for(int band=0;band<3;++band) {
                wchar_t value[128]{};GetWindowTextW(anchorEdit_[band],value,128);
                errno=0;wchar_t* tail{};spectrum.palette.anchorHz[band]=std::wcstod(value,&tail);
                while(tail && iswspace(*tail)) ++tail;
                if(tail==value || !tail || *tail || errno==ERANGE) {
                    SetWindowTextW(error_,L"Enter three finite frequency values in Hz.");SetFocus(anchorEdit_[band]);return;
                }
            }
            spectrum.palette.compatChannel={0,1,2};
            spectrum.reverseTrackZ=SendMessageW(reverseTrackZ_,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
            if(!visualizer::ValidPalette(spectrum.palette)) {
                SetWindowTextW(error_,L"Require 0 <= low < middle < high <= 24000 Hz.");return;
            }
            const auto parse=[&](HWND control,double& parsed) {
                return ReadNumber(control,parsed);
            };
            auto strokeValue=stroke::Load();
            strokeValue.paintEnabled=
                SendMessageW(paintEnabled_,BM_GETCHECK,0,0)==BST_CHECKED;
            const std::array<double*,5> strokeField{&strokeValue.restWidthCm,
                &strokeValue.fullWidthCm,&strokeValue.quietTrailCm,
                &strokeValue.loudTrailCm,&strokeValue.opacityPct};
            for(int entry=0;entry<5;++entry)
            {
                double value{};
                if(!parse(strokeEdit_[entry],value))
                {SetWindowTextW(error_,L"Enter finite wall stroke values.");SetFocus(strokeEdit_[entry]);return;}
                *strokeField[entry]=value;
            }
            if(!stroke::Valid(strokeValue))
            {SetWindowTextW(error_,L"Wall stroke values must be finite.");return;}
            visualizer::EntropyPursuit entropyPursuit;
            if(!parse(focusedLife_,entropyPursuit.focusedHalfLifeSec) ||
               !parse(uniformLife_,entropyPursuit.uniformHalfLifeSec) ||
               !parse(entropyCurve_,entropyPursuit.curveExponent) ||
               !visualizer::ValidEntropyPursuit(entropyPursuit)) {
                SetWindowTextW(error_,L"Use nonnegative finite entropy half-lives and a positive finite entropy curve.");return;
            }
            visualizer::VolumePursuit volumePursuit;
            if(!parse(volumeCurve_,volumePursuit.curveExponent) ||
               !parse(volumeZero_,volumePursuit.zeroHalfLifeLevel) ||
               !visualizer::ValidVolumePursuit(volumePursuit)) {
                SetWindowTextW(error_,L"Use a positive finite dBFS half-life exponent and zero-half-life level.");
                SetFocus(volumeCurve_);return;
            }
            visualizer::MoveSizeMap sizeMap;
            sizeMap.inverted=SendMessageW(invertSize_,BM_GETCHECK,0,0)==BST_CHECKED;
            if(!parse(maxDiameterPx_,sizeMap.maxDiameterPx) ||
               !parse(sizeCurve_,sizeMap.curveExponent) ||
               !visualizer::ValidMoveSizeMap(sizeMap)) {
                SetWindowTextW(error_,L"Use a maximum of at least 1 reference pixel that fits the box, and a positive finite curve.");
                SetFocus(maxDiameterPx_);return;
            }
            const auto syncSelection=SendMessageW(syncMode_,CB_GETCURSEL,0,0);
            const auto syncMode=static_cast<render::PresentationSync>(syncSelection);
            if(syncSelection==CB_ERR || !render::ValidSyncMode(syncMode)) {
                SetWindowTextW(error_,L"Select no sync or VSync.");SetFocus(syncMode_);return;
            }
            const auto paletteSelection=SendMessageW(paletteChoice_,CB_GETCURSEL,0,0);
            if(paletteSelection==CB_ERR ||
               paletteSelection<0 ||
               static_cast<std::size_t>(paletteSelection)>=visualizer::PaletteCount) {
                SetWindowTextW(error_,L"Select an RGB frequency palette.");
                SetFocus(paletteChoice_);return;
            }
            const auto paletteChoice=static_cast<std::size_t>(paletteSelection);
            wchar_t paperWhiteText[128]{};GetWindowTextW(paperWhite_,paperWhiteText,128);
            errno=0;wchar_t* paperWhiteEnd{};const double paperWhite=std::wcstod(paperWhiteText,&paperWhiteEnd);
            while(paperWhiteEnd && iswspace(*paperWhiteEnd)) ++paperWhiteEnd;
            if(paperWhiteEnd==paperWhiteText || !paperWhiteEnd || *paperWhiteEnd || errno==ERANGE || !render::hdr::ValidPaperWhiteNits(paperWhite)) {
                SetWindowTextW(error_,L"Enter paper white above 0 and no greater than 10000 nits.");SetFocus(paperWhite_);return;
            }
            visualizer::TopDecay topDecay;
            if(!parse(topSeconds_,topDecay.secondsPerDb) ||
               !parse(lowestTopDbfs_,topDecay.floorDbfs) ||
               !visualizer::ValidTopDecay(topDecay)) {
                SetWindowTextW(error_,L"Use positive seconds per dBFS and a finite lowest Top.");
                SetFocus(topSeconds_);return;
            }
            wchar_t verticalBottomText[128]{};GetWindowTextW(verticalBottom_,verticalBottomText,128);
            errno=0;wchar_t* verticalBottomEnd{};const double verticalBottom=std::wcstod(verticalBottomText,&verticalBottomEnd);
            while(verticalBottomEnd && iswspace(*verticalBottomEnd)) ++verticalBottomEnd;
            if(verticalBottomEnd==verticalBottomText || !verticalBottomEnd || *verticalBottomEnd || errno==ERANGE ||
               !std::isfinite(verticalBottom)) {
                SetWindowTextW(error_,L"Enter a finite bottom dBFS value.");SetFocus(verticalBottom_);return;
            }
            wchar_t verticalTopText[128]{};GetWindowTextW(verticalTop_,verticalTopText,128);
            errno=0;wchar_t* verticalTopEnd{};const double verticalTop=std::wcstod(verticalTopText,&verticalTopEnd);
            while(verticalTopEnd && iswspace(*verticalTopEnd)) ++verticalTopEnd;
            const visualizer::VerticalDbfsRange verticalRange{verticalBottom,verticalTop};
            if(verticalTopEnd==verticalTopText || !verticalTopEnd || *verticalTopEnd || errno==ERANGE ||
               !visualizer::ValidTopMap(verticalRange,topDecay)) {
                SetWindowTextW(error_,L"Require Bottom below Lowest Top and Reset top at or above Lowest Top.");SetFocus(verticalTop_);return;
            }
            const auto spectralStatus=visualizer::settings::WriteSpectrum(spectrum);
            if(spectralStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the frequency settings.");return; }
            const auto paletteStatus=visualizer::settings::WritePaletteChoice(paletteChoice);
            if(paletteStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the color palette.");return; }
            const auto status=visualizer::settings::WriteEyeDistance(millimeters);
            if(status!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the distance. It was not applied.");return; }
            const auto halfLifeStatus=visualizer::settings::WriteEntropyPursuit(entropyPursuit);
            if(halfLifeStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the entropy LR/UD smoothing.");return; }
            const auto dbfsStatus=visualizer::settings::WriteVolumePursuit(volumePursuit);
            if(dbfsStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the dBFS XY smoothing.");return; }
            const auto sizeStatus=visualizer::settings::WriteMoveSizeMap(sizeMap);
            if(sizeStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the sphere-size controls.");return; }
            const auto syncStatus=visualizer::settings::WriteSyncMode(syncMode);
            if(syncStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save presentation synchronization.");return; }
            const auto paperWhiteStatus=visualizer::settings::WritePaperWhite(paperWhite);
            if(paperWhiteStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save paper white.");return; }
            const auto verticalRangeStatus=visualizer::settings::WriteVerticalRange(verticalRange);
            if(verticalRangeStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the vertical dBFS range.");return; }
            const auto topDecayStatus=visualizer::settings::WriteTopDecay(topDecay);
            if(topDecayStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the Top decay.");return; }
            const auto strokeStatus=stroke::Store(strokeValue);
            if(strokeStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save wall stroke values.");return; }
            const auto castStatus=stroke::StoreCast(castDraft_);
            if(castStatus!=ERROR_SUCCESS) { SetWindowTextW(error_,L"Windows could not save the ray source position.");return; }
            castPreview_=false;
            const auto event=OpenEventW(EVENT_MODIFY_STATE,FALSE,visualizer::settings::SettingsChanged);
            if(event) { SetEvent(event);CloseHandle(event); }
            const auto syncEvent=OpenEventW(EVENT_MODIFY_STATE,FALSE,visualizer::settings::SyncChangedEvent);
            if(syncEvent) { SetEvent(syncEvent);CloseHandle(syncEvent); }
            DestroyWindow(window_);
        }
        static LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
        {
            auto* self=reinterpret_cast<SceneSettingsPanel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
            if(message==WM_NCCREATE) {
                self=static_cast<SceneSettingsPanel*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
                self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
            }
            if(!self) return DefWindowProcW(window,message,wparam,lparam);
            if(message==WM_CREATE) { self->Build(window);return 0; }
            if(message==WM_HSCROLL) {
                const auto control=reinterpret_cast<HWND>(lparam);
                if(std::find(self->sourceDial_.begin(),self->sourceDial_.end(),control)!=
                    self->sourceDial_.end()) {
                    if(LOWORD(wparam)!=TB_ENDTRACK) self->CastChanged(
                        LOWORD(wparam)==TB_THUMBTRACK ||
                        LOWORD(wparam)==TB_THUMBPOSITION?control:nullptr);
                    return 0;
                }
            }
            if(message==WM_COMMAND) {
                const auto id=LOWORD(wparam);
                if(id>=260 && id<263 && HIWORD(wparam)==EN_KILLFOCUS)
                {self->ApplyOrigin(id-260);return 0;}
                if(LOWORD(wparam)==104) { self->CenterHead();return 0; }
                if(LOWORD(wparam)==105) { self->RefreshTrackStatus();return 0; }
                if(LOWORD(wparam)==IDOK) {
                    const auto focus=GetFocus();
                    const auto found=std::find(self->sourceEdit_.begin(),
                        self->sourceEdit_.end(),focus);
                    if(found!=self->sourceEdit_.end() &&
                        !self->ApplyOrigin(static_cast<int>(
                            found-self->sourceEdit_.begin()))) return 0;
                    self->Save();return 0;
                }
                if(LOWORD(wparam)==IDCANCEL) { self->RestoreOrigin();DestroyWindow(window);return 0; }
            }
            if(message==WM_CLOSE) { self->RestoreOrigin();DestroyWindow(window);return 0; }
            if(message==WM_DESTROY) {self->RestoreOrigin();self->originBus_.Close();}
            if(message==WM_NCDESTROY) { self->window_=self->eyeDistance_=self->error_=self->trackStatus_=self->paperWhite_=self->verticalBottom_=self->verticalTop_=self->topSeconds_=self->lowestTopDbfs_=self->focusedLife_=self->uniformLife_=self->entropyCurve_=self->volumeCurve_=self->volumeZero_=self->maxDiameterPx_=self->sizeCurve_=self->invertSize_=self->syncMode_=self->paletteChoice_=self->paintEnabled_=nullptr;self->sourceDial_.fill(nullptr);self->sourceLabel_.fill(nullptr);self->sourceEdit_.fill(nullptr);self->strokeEdit_.fill(nullptr);SetWindowLongPtrW(window,GWLP_USERDATA,0); }
            return DefWindowProcW(window,message,wparam,lparam);
        }
        HWND window_{},eyeDistance_{},error_{},trackStatus_{},reverseTrackZ_{},paperWhite_{},verticalBottom_{},verticalTop_{};
        HWND topSeconds_{},lowestTopDbfs_{};
        HWND focusedLife_{},uniformLife_{},entropyCurve_{},volumeCurve_{},volumeZero_{};
        HWND maxDiameterPx_{},sizeCurve_{},invertSize_{},syncMode_{},paletteChoice_{};
        HWND paintEnabled_{};
        std::array<HWND,3> anchorEdit_{};
        std::array<HWND,3> sourceDial_{},sourceLabel_{},sourceEdit_{};
        std::array<HWND,5> strokeEdit_{};
        stroke::OriginBus originBus_;
        stroke::CastPoint castDraft_{},castRestore_{};
        bool castPreview_{};
    };
}
