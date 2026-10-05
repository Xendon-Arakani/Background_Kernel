#pragma once
#include "scene/RoomGeometry.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace bk::tracking
{
    using PoseCmDeg = std::array<double,6>;
    bool DecodeExactPose(std::span<const std::byte> packet, PoseCmDeg& pose);
    struct PoseSample
    {
        PoseCmDeg pose{};
        std::uint64_t validPackets{},badPackets{};
        std::int64_t recvQpc{};
        std::uint32_t socketError{};
    };
    class OpenTrackPose
    {
    public:
        explicit OpenTrackPose(std::uint16_t port=4242);
        ~OpenTrackPose();
        OpenTrackPose(const OpenTrackPose&)=delete;
        OpenTrackPose& operator=(const OpenTrackPose&)=delete;
        PoseSample LatestPose() const;
        std::uint16_t Port() const;
        void* PoseReadyEvent() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class HeadView
    {
    public:
        void CenterNext() { centerPending_=true; }
        scene::EyePose ApplyPose(const PoseSample& sample,double distanceMm,double depthScale=-1.0)
        {
            if(sample.validPackets)
            {
                if(!centered_ || centerPending_) { neutralPose_=sample.pose;centered_=true;centerPending_=false; }
                const scene::Vec screenOffsetMm{
                    -(sample.pose[0]-neutralPose_[0])*10.0,
                    (sample.pose[1]-neutralPose_[1])*10.0,
                    depthScale*(sample.pose[2]-neutralPose_[2])*10.0};
                const double safeCoordMm=std::sqrt(static_cast<double>(std::numeric_limits<float>::max()))/4;
                offsetValid_=std::isfinite(screenOffsetMm.x)&&std::isfinite(screenOffsetMm.y)&&std::isfinite(screenOffsetMm.z)&&
                    std::abs(screenOffsetMm.x)<safeCoordMm&&std::abs(screenOffsetMm.y)<safeCoordMm&&std::abs(screenOffsetMm.z)<safeCoordMm&&
                    screenOffsetMm.z-distanceMm<scene::FrontEdgeZmm();
                if(offsetValid_) screenOffsetMm_=screenOffsetMm;
            }
            scene::EyePose eye;eye.position=screenOffsetMm_+scene::Vec{0,0,-distanceMm};
            if(eye.position.z>=scene::FrontEdgeZmm()) { screenOffsetMm_={};eye.position={0,0,-distanceMm};offsetValid_=false; }
            return eye;
        }
        const PoseCmDeg& NeutralPose() const { return neutralPose_; }
        bool Centered() const { return centered_; }
        bool CenterPending() const { return centerPending_; }
        bool OffsetValid() const { return offsetValid_; }
    private:
        PoseCmDeg neutralPose_{};
        scene::Vec screenOffsetMm_{};
        bool centered_{},centerPending_{},offsetValid_{};
    };
}
