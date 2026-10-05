#pragma once
#include "visualizer/AudioTrailMotion.hpp"
#include "visualizer/ContinuousMotion.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace bk::visualizer
{
    struct EntropyPursuit
    {
        double focusedHalfLifeSec{0.0};
        double uniformHalfLifeSec{1.0};
        double curveExponent{1.0};
        bool operator==(const EntropyPursuit&) const = default;
    };
    inline bool ValidEntropyPursuit(const EntropyPursuit& pursuit) noexcept
    {
        return std::isfinite(pursuit.focusedHalfLifeSec) &&
            pursuit.focusedHalfLifeSec>=0.0 &&
            std::isfinite(pursuit.uniformHalfLifeSec) &&
            pursuit.uniformHalfLifeSec>=0.0 &&
            std::isfinite(pursuit.curveExponent) && pursuit.curveExponent>0.0;
    }
    inline constexpr double MaxHalfLifeSec=10.0;
    inline constexpr double DbfsLevelOffset=100.0;

    struct VolumePursuit
    {
        double curveExponent{1.0};
        double zeroHalfLifeLevel{100.0};
        bool operator==(const VolumePursuit&) const = default;
    };

    inline bool ValidVolumePursuit(const VolumePursuit& pursuit) noexcept
    {
        return std::isfinite(pursuit.curveExponent) &&
            pursuit.curveExponent>0.0 &&
            std::isfinite(pursuit.zeroHalfLifeLevel) &&
            pursuit.zeroHalfLifeLevel>0.0;
    }

    struct PursuitMap
    {
        double focusedLife{};
        double uniformLife{1.0};
        double entropyCurve{1.0};
        double volumeCurve{1.0};
        double inverseZero{0.01};
        bool entropyLinear{true};
        bool volumeLinear{true};
    };

    inline PursuitMap CompilePursuit(
        const EntropyPursuit& entropy,const VolumePursuit& volume)
    {
        if(!ValidEntropyPursuit(entropy) || !ValidVolumePursuit(volume))
            throw std::invalid_argument("Invalid pursuit controls");
        return {entropy.focusedHalfLifeSec,entropy.uniformHalfLifeSec,
            entropy.curveExponent,volume.curveExponent,
            1.0/volume.zeroHalfLifeLevel,
            entropy.curveExponent==1.0,volume.curveExponent==1.0};
    }

    inline double ShiftedDbfsLevel(const double dbfs)
    {
        if(std::isnan(dbfs))
            throw std::invalid_argument("A measured digital level cannot be NaN");
        return std::max(dbfs+DbfsLevelOffset,0.0);
    }

    inline double VolumeHalfLife(
        const double dbfs,const PursuitMap& map)
    {
        const double normalized=std::clamp(
            ShiftedDbfsLevel(dbfs)*map.inverseZero,0.0,1.0);
        const double remaining=1.0-normalized;
        return MaxHalfLifeSec*(map.volumeLinear
            ? remaining : std::pow(remaining,map.volumeCurve));
    }

    inline double PlanarHalfLife(const double spectralEntropy,
        const double dbfs,const PursuitMap& map)
    {
        if(!std::isfinite(spectralEntropy))
            throw std::invalid_argument("Spectral entropy must be finite");
        const double bounded=std::clamp(spectralEntropy,0.0,1.0);
        const double shaped=map.entropyLinear
            ? bounded : std::pow(bounded,map.entropyCurve);
        const double entropyLife=std::lerp(
            map.focusedLife,map.uniformLife,shaped);
        const double volumeLife=VolumeHalfLife(dbfs,map);
        return std::max(
            entropyLife,volumeLife);
    }
    inline MotionPoint PursueBezier(const QuinticControls& sourceCurve,MotionPoint position,
                                    double spanSec,double halfLifeSec)
    {
        if(!std::isfinite(spanSec) || spanSec<0 || !std::isfinite(halfLifeSec) || halfLifeSec<0)
            throw std::invalid_argument("Invalid pursuit interval or half-life");
        if(halfLifeSec==0) return sourceCurve.back();
        if(spanSec==0) return position;
        const double decay=std::numbers::ln2*(spanSec/halfLifeSec);
        if(!std::isfinite(decay)) return sourceCurve.back();
        if(decay==0) return position;

        std::array<double,6> moments{};
        if(decay>=5) {
            moments[0]=-std::expm1(-decay);
            for(std::size_t n=1;n<moments.size();++n) moments[n]=1-static_cast<double>(n)*moments[n-1]/decay;
        } else {
            double term=decay/6,sum=term;std::size_t j=0;
            for(;;) {
                term*=decay/static_cast<double>(j+1)*static_cast<double>(j+6)/static_cast<double>(j+7);++j;
                const double next=sum+term;if(next==sum) break;sum=next;
            }
            moments[5]=std::exp(-decay)*sum;
            for(std::size_t n=5;n>0;--n) moments[n-1]=decay/static_cast<double>(n)*(1-moments[n]);
        }
        auto forwardDiff=sourceCurve;
        constexpr std::array<double,6> binomial{1,5,10,10,5,1};
        auto result=position+(sourceCurve.front()-position)*moments[0];
        for(std::size_t n=1;n<6;++n) {
            for(std::size_t i=0;i<6-n;++i) forwardDiff[i]=forwardDiff[i+1]-forwardDiff[i];
            result=result+forwardDiff[0]*(binomial[n]*moments[n]);
        }
        return result;
    }

    class PathPursuit
    {
    public:
        void Reset() { ready_=false;position_={};planarHalfLifeSec_=depthHalfLifeSec_=0;nextSegment_=0; }
        void PursueSegments(const ContinuousMotion& sourcePath,ContinuousMotion& visiblePath) {
            if(!sourcePath.Ready()) return;
            const auto& segments=sourcePath.Segments();
            if(segments.empty()) return;
            if(!ready_) {
                if(segments.front().startTick!=0) throw std::logic_error("Pursuit cannot start after discarded ghost history");
                position_=PositionOf(segments.front().start);visiblePath.PushWaypoint(segments.front().start);ready_=true;
            }
            if(nextSegment_<segments.front().startTick) throw std::logic_error("Unconsumed ghost trajectory was discarded");
            auto offset=static_cast<std::size_t>(nextSegment_-segments.front().startTick);
            for(;offset<segments.size();++offset) {
                const auto& segment=segments[offset];
                if(segment.startTick!=nextSegment_) throw std::logic_error("Noncontiguous ghost trajectory");
                planarHalfLifeSec_=segment.start.planarHalfLifeSec;
                depthHalfLifeSec_=segment.start.depthHalfLifeSec;
                const auto previous=position_;
                auto pursued=PursueBezier(
                    segment.controls,previous,1.0/TickRate,planarHalfLifeSec_);
                if(depthHalfLifeSec_!=planarHalfLifeSec_) {
                    pursued.z=depthHalfLifeSec_==0
                        ? segment.controls.back().z
                        : PursueBezier(segment.controls,previous,
                            1.0/TickRate,depthHalfLifeSec_).z;
                }
                position_=pursued;
                auto frame=segment.end;
                frame.dotXmm=static_cast<float>(position_.x);frame.dotYmm=static_cast<float>(position_.y);frame.dotZmm=static_cast<float>(position_.z);
                visiblePath.PushWaypoint(frame);++nextSegment_;
            }
        }
        double PlanarHalfLifeSec() const { return planarHalfLifeSec_; }
        double DepthHalfLifeSec() const { return depthHalfLifeSec_; }
        std::uint64_t PursuedSegments() const { return nextSegment_; }
    private:
        MotionPoint position_{};
        std::uint64_t nextSegment_{};
        double planarHalfLifeSec_{},depthHalfLifeSec_{};
        bool ready_{};
    };
}
