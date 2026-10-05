#include "visualizer/AudioTrailMotion.hpp"

#include <algorithm>
#include <cmath>

namespace bk::visualizer
{
    namespace
    {
        float InsetAxisPos(const double fraction, const double extent)
        {
            const double inset = std::min(
                static_cast<double>(PathInsetRadiusMm), 0.5 * extent);
            return static_cast<float>(
                inset + fraction * (static_cast<double>(extent) - 2.0 * inset));
        }

        double FinitePcmOrZero(const float sample)
        {
            return std::isfinite(sample) ? static_cast<double>(sample) : 0.0;
        }
    }

    AudioTrailMotion::AudioTrailMotion()
    {
        bottomAmp_=std::pow(10.0,resetRange_.bottom/20.0);
        Reset(scene::ScreenChordMm, scene::ScreenHeightMm);
    }

    void AudioTrailMotion::Reset(
        const double widthMm,
        const double heightMm)
    {
        ResetTop();
        const std::array<RawStereoPcm,1> zero{{{0.0f,0.0f}}};
        Tick(zero,widthMm,heightMm);
        ResetTop();
    }

    void AudioTrailMotion::SetTopMap(
        const VerticalDbfsRange& range,
        const TopDecay& decay)
    {
        if (!ValidTopMap(range,decay))
            throw std::invalid_argument(
                "Vertical dBFS range and Top decay are incompatible");
        if(range==resetRange_ && decay==topDecay_) return;
        resetRange_ = range;
        topDecay_=decay;
        bottomAmp_=std::pow(10.0,resetRange_.bottom/20.0);
        fallPerTick_=static_cast<float>(
            1.0/(topDecay_.secondsPerDb*TickRate));
        ResetTop();
    }

    void AudioTrailMotion::SetTop(const float dbfs) noexcept
    {
        activeTopDbfs_=dbfs;
        const double topAmp=std::pow(
            10.0,static_cast<double>(activeTopDbfs_)/20.0);
        ampScale_=1.0/(topAmp-bottomAmp_);
    }

    void AudioTrailMotion::ResetTop() noexcept
    {
        SetTop(static_cast<float>(std::max(
            resetRange_.top,topDecay_.floorDbfs)));
    }

    float AudioTrailMotion::InnerTop(
        const double dbfs,const double amplitude,
        const double heightMm) const noexcept
    {
        float top=static_cast<float>(dbfs);
        const float infinity=std::numeric_limits<float>::infinity();
        if(!(static_cast<double>(top)>dbfs))
            top=std::nextafter(top,infinity);
        const float edge=InsetAxisPos(0.0,heightMm);
        for(;;) {
            const double topAmp=std::pow(
                10.0,static_cast<double>(top)/20.0);
            const double level=std::clamp(
                (amplitude-bottomAmp_)/(topAmp-bottomAmp_),0.0,1.0);
            if(InsetAxisPos(1.0-level,heightMm)>edge) return top;
            top=std::nextafter(top,infinity);
        }
    }

    void AudioTrailMotion::AdvanceTop(
        const double dbfs,const double amplitude,
        const double heightMm) noexcept
    {
        if(std::isfinite(dbfs) &&
            frame_.dotYmm<=InsetAxisPos(0.0,heightMm)) {
            SetTop(InnerTop(dbfs,amplitude,heightMm));
            return;
        }
        const float floor=static_cast<float>(topDecay_.floorDbfs);
        SetTop(std::max(floor,activeTopDbfs_-fallPerTick_));
    }

    const TrailFrame& AudioTrailMotion::Tick(
        const std::span<const RawStereoPcm> samples,
        const double widthMm,
        const double heightMm)
    {
        if (samples.empty())
            throw std::invalid_argument("An audio tick requires PCM samples");

        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        for (const auto& sample : samples)
        {
            const double left = FinitePcmOrZero(sample.leftSigned);
            const double right = FinitePcmOrZero(sample.rightSigned);
            leftEnergy += left * left;
            rightEnergy += right * right;
        }
        frame_ = {};
        const double count = static_cast<double>(samples.size());
        frame_.leftRms = std::sqrt(leftEnergy / count);
        frame_.rightRms = std::sqrt(rightEnergy / count);
        const double rmsSum = frame_.leftRms + frame_.rightRms;
        frame_.stereoBalance = rmsSum>0.0
            ? (frame_.rightRms - frame_.leftRms) / rmsSum : 0.0;
        const double meanStereoPower=(leftEnergy+rightEnergy)/(2.0*count);
        frame_.instantaneousDbfs=meanStereoPower>0.0
            ? 10.0*std::log10(meanStereoPower)
            : -std::numeric_limits<double>::infinity();
        const double stereoRms=std::sqrt(meanStereoPower);
        frame_.normalizedLevel=std::clamp(
            (stereoRms-bottomAmp_)*ampScale_,0.0,1.0);
        frame_.dotXmm = InsetAxisPos(rmsSum>0.0 ? frame_.rightRms/rmsSum : 0.5, widthMm);
        frame_.dotYmm = InsetAxisPos(1.0 - frame_.normalizedLevel, heightMm);
        AdvanceTop(frame_.instantaneousDbfs,
            stereoRms,heightMm);
        return frame_;
    }

    const TrailFrame& AudioTrailMotion::CurrentFrame() const noexcept
    {
        return frame_;
    }

}
