#pragma once
#include "scene/PhysicalUnits.hpp"
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace bk::visualizer::runtime
{
    inline constexpr wchar_t SceneStateName[]=L"Local\\SceneState";
    inline constexpr std::uint32_t SceneRunStateMagic=0x42565231;
    struct SceneRunState
    {
        std::uint32_t magic{SceneRunStateMagic},version{9},processId{},ready{};
        std::uint64_t presentations{},captureFrames{},captureBreaks{},queuedFrames{};
        std::uint64_t handledBreaks{},heldClockFrames{},trailPoints{},sweepPoints{};
        std::int64_t presentQpc{},frequency{};
        double motionTick{},audioMs{},pathMs{},renderMs{},totalAudioMs{},totalPathMs{},totalRenderMs{},maxRenderMs{};
        double analysisDbfs{},planarHalfLife{},depthHalfLife{},spectralEntropy{};
        double focusedHalfLife{},uniformHalfLife{1},entropyCurve{1},sizeCurve{1};
        double volumeCurve{1},zeroHalfLifeLevel{100};
        double maxDiameterPx{scene::BaseMaxRefPx};
        double dotXmm{},dotYmm{},dotZmm{},dotRadiusMm{},sizeMoveMm{};
    };
    struct SceneStateBlock { volatile LONG sequence{};LONG reserved{};SceneRunState data; };
    class SceneStateWriter
    {
    public:
        SceneStateWriter() {
            mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SceneStateBlock),SceneStateName);
            if(mapping_) block_=static_cast<SceneStateBlock*>(MapViewOfFile(mapping_,FILE_MAP_WRITE,0,0,sizeof(SceneStateBlock)));
        }
        ~SceneStateWriter() { if(block_) UnmapViewOfFile(block_);if(mapping_) CloseHandle(mapping_); }
        void Publish(const SceneRunState& status) {
            if(!block_) return;
            InterlockedIncrement(&block_->sequence);std::memcpy(&block_->data,&status,sizeof(status));InterlockedIncrement(&block_->sequence);
        }
    private:
        HANDLE mapping_{};SceneStateBlock* block_{};
    };
    inline bool ReadSceneRunState(SceneRunState& status) {
        HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,SceneStateName);if(!mapping) return false;
        const auto* block=static_cast<const SceneStateBlock*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(SceneStateBlock)));
        bool valid=false;
        if(block) {
            LONG before=block->sequence;MemoryBarrier();std::memcpy(&status,&block->data,sizeof(status));MemoryBarrier();
            valid=!(before&1)&&before==block->sequence&&status.magic==SceneRunStateMagic&&status.version==9;
            UnmapViewOfFile(block);
        }
        CloseHandle(mapping);return valid;
    }
}
