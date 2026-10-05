#pragma once
#include "tracking/OpenTrackPose.hpp"
#include <windows.h>
#include <cstring>

namespace bk::tracking
{
    inline constexpr wchar_t HeadStateName[]=L"Local\\BkHeadState";
    inline constexpr wchar_t HeadCenterEvent[]=L"Local\\BkHeadCenter";
    inline constexpr wchar_t HeadAppliedEvent[]=L"Local\\BkHeadApplied";
    inline constexpr std::uint32_t HeadTrackStateMagic=0x42544831;
    struct HeadTrackState
    {
        std::uint32_t magic{HeadTrackStateMagic},version{2},flags{},socketError{};
        std::uint64_t validPackets{},badPackets{};
        std::int64_t recvQpc{},presentQpc{},qpcFrequency{};
        PoseCmDeg rawPose{},neutralPose{};
        std::array<double,3> eye{};
        double distanceMm{},boxDepth{scene::BackWallMm},lightDepth{scene::BaseLightZmm};
    };
    struct HeadStateBlock { volatile LONG sequence{};LONG reserved{};HeadTrackState data; };
    static_assert(sizeof(HeadTrackState)==200 && sizeof(HeadStateBlock)==208);
    class HeadTrackStateWriter
    {
    public:
        HeadTrackStateWriter()
        {
            mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(HeadStateBlock),HeadStateName);
            if(mapping_) block_=static_cast<HeadStateBlock*>(MapViewOfFile(mapping_,FILE_MAP_WRITE,0,0,sizeof(HeadStateBlock)));
        }
        ~HeadTrackStateWriter() { if(block_) UnmapViewOfFile(block_);if(mapping_) CloseHandle(mapping_); }
        void Publish(const HeadTrackState& status)
        {
            if(!block_) return;
            InterlockedIncrement(&block_->sequence);std::memcpy(&block_->data,&status,sizeof(status));InterlockedIncrement(&block_->sequence);
        }
    private:
        HANDLE mapping_{};HeadStateBlock* block_{};
    };
    inline bool ReadHeadTrackState(HeadTrackState& status)
    {
        const auto handle=OpenFileMappingW(FILE_MAP_READ,FALSE,HeadStateName);
        if(!handle) return false;
        const auto* block=static_cast<const HeadStateBlock*>(MapViewOfFile(handle,FILE_MAP_READ,0,0,sizeof(HeadStateBlock)));
        bool valid=false;
        if(block)
        {
            const auto before=block->sequence;MemoryBarrier();
            std::memcpy(&status,&block->data,sizeof(status));MemoryBarrier();
            valid=!(before&1)&&before==block->sequence&&status.magic==HeadTrackStateMagic&&status.version==2;
            UnmapViewOfFile(block);
        }
        CloseHandle(handle);return valid;
    }
}
