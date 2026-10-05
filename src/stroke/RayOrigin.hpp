#pragma once

#include "stroke/WallStrokeSettings.hpp"
#include "scene/RoomGeometry.hpp"
#include <cstring>

namespace bk::stroke
{
    inline scene::Vec OriginPosition(const CastPoint value) noexcept
    {
        const double x=std::lerp(-scene::ScreenHalfChordMm(),
            scene::ScreenHalfChordMm(),static_cast<double>(value.across));
        const double y=std::lerp(scene::ScreenHeightMm/2,
            -scene::ScreenHeightMm/2,static_cast<double>(value.down));
        const double radius=scene::ScreenCurveRadiusMm+scene::PaneThicknessMm;
        const double glass=std::sqrt(radius*radius-x*x)-
            scene::ScreenCurveRadiusMm;
        return {x,y,std::lerp(glass,scene::BackWallMm,
            static_cast<double>(value.depth))};
    }

    class OriginBus
    {
        struct State
        {
            volatile LONG sequence{};
            LONG valid{};
            CastPoint point{};
        };

    public:
        OriginBus()=default;
        OriginBus(const OriginBus&)=delete;
        OriginBus& operator=(const OriginBus&)=delete;
        ~OriginBus() { Close(); }

        bool Open() noexcept
        {
            if(state_) return true;
            mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,
                PAGE_READWRITE,0,sizeof(State),
                L"Local\\BkRayOrigin");
            if(mapping_) state_=static_cast<State*>(MapViewOfFile(mapping_,
                FILE_MAP_WRITE,0,0,sizeof(State)));
            changed_=CreateEventW(nullptr,FALSE,FALSE,
                L"Local\\BkRayChanged");
            if(!state_ || !changed_) { Close();return false; }
            return true;
        }

        void Close() noexcept
        {
            if(state_) {UnmapViewOfFile(state_);state_=nullptr;}
            if(mapping_) {CloseHandle(mapping_);mapping_=nullptr;}
            if(changed_) {CloseHandle(changed_);changed_=nullptr;}
        }

        bool Read(CastPoint& point) const noexcept
        {
            if(!state_) return false;
            const LONG before=state_->sequence;
            if(before&1) return false;
            MemoryBarrier();
            CastPoint copy{};
            std::memcpy(&copy,&state_->point,sizeof(copy));
            const LONG valid=state_->valid;
            MemoryBarrier();
            if(before!=state_->sequence || !valid || !Valid(copy)) return false;
            point=copy;
            return true;
        }

        bool Publish(const CastPoint point) noexcept
        {
            if(!state_ || !Valid(point)) return false;
            InterlockedIncrement(&state_->sequence);
            std::memcpy(&state_->point,&point,sizeof(point));
            state_->valid=1;
            InterlockedIncrement(&state_->sequence);
            return SetEvent(changed_)!=FALSE;
        }

        HANDLE Changed() const noexcept { return changed_; }

    private:
        HANDLE mapping_{},changed_{};
        State* state_{};
    };
}
