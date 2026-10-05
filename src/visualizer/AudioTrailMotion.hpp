#pragma once

#include "audio/RawStereoPcm.hpp"
#include "scene/PhysicalUnits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>

namespace bk::visualizer
{
    using audio::PcmRateHz;
    using audio::RawStereoPcm;
#ifndef BK_TICK_RATE
#define BK_TICK_RATE 144
#endif
    constexpr std::uint32_t TickRate = BK_TICK_RATE;
    static_assert(TickRate > 0 && TickRate <= PcmRateHz);

    struct VerticalDbfsRange
    {
        double bottom{-100.0};
        double top{0.0};
        bool operator==(const VerticalDbfsRange&) const = default;
    };

    struct TopDecay
    {
        double secondsPerDb{1.0};
        double floorDbfs{-10.0};
        bool operator==(const TopDecay&) const = default;
    };

    inline bool ValidVerticalRange(const VerticalDbfsRange& range) noexcept
    {
        if (!std::isfinite(range.bottom) || !std::isfinite(range.top) ||
            !(range.bottom < range.top))
            return false;
        const double bottomAmplitude=std::pow(10.0,range.bottom/20.0);
        const double topAmplitude=std::pow(10.0,range.top/20.0);
        return std::isfinite(bottomAmplitude) && std::isfinite(topAmplitude) &&
            topAmplitude>bottomAmplitude;
    }

    inline bool ValidTopDecay(const TopDecay& decay) noexcept
    {
        return std::isfinite(decay.secondsPerDb) &&
            decay.secondsPerDb>0.0 &&
            std::isfinite(decay.floorDbfs);
    }

    inline bool ValidTopMap(
        const VerticalDbfsRange& range,
        const TopDecay& decay) noexcept
    {
        return ValidVerticalRange(range) && ValidTopDecay(decay) &&
            range.bottom<decay.floorDbfs && range.top>=decay.floorDbfs;
    }

    inline double MapVerticalAmp(
        const double amplitude, const VerticalDbfsRange& range)
    {
        if (!ValidVerticalRange(range))
            throw std::invalid_argument("Vertical dBFS bottom must be below top");
        if (!std::isfinite(amplitude) || amplitude<0.0)
            throw std::invalid_argument("Measured vertical amplitude must be finite and nonnegative");
        const double bottomAmplitude=std::pow(10.0,range.bottom/20.0);
        const double topAmplitude=std::pow(10.0,range.top/20.0);
        return std::clamp((amplitude-bottomAmplitude)/
            (topAmplitude-bottomAmplitude),0.0,1.0);
    }

    inline double MapVerticalDbfs(
        const double dbfs, const VerticalDbfsRange& range)
    {
        if (!ValidVerticalRange(range))
            throw std::invalid_argument("Vertical dBFS bottom must be below top");
        if (dbfs == -std::numeric_limits<double>::infinity()) return 0.0;
        if (!std::isfinite(dbfs))
            throw std::invalid_argument("Measured vertical dBFS must be finite or silence");
        return MapVerticalAmp(std::pow(10.0,dbfs/20.0),range);
    }

    constexpr std::uint32_t MinimumFramesPerTick = PcmRateHz / TickRate;
    constexpr std::uint32_t MaximumFramesPerTick =
        (PcmRateHz + TickRate - 1) / TickRate;
    constexpr std::size_t TickCadenceLength =
        TickRate / std::gcd(PcmRateHz, TickRate);

    consteval auto MakeFrameCadence()
    {
        std::array<std::uint32_t, TickCadenceLength> cadence{};
        std::uint64_t previousBoundary = 0;
        for (std::size_t tick = 0; tick < cadence.size(); ++tick)
        {
            const std::uint64_t boundary =
                (static_cast<std::uint64_t>(tick) + 1) * PcmRateHz /
                TickRate;
            cadence[tick] = static_cast<std::uint32_t>(
                boundary - previousBoundary);
            previousBoundary = boundary;
        }
        return cadence;
    }

    inline constexpr auto TickFrameCadence = MakeFrameCadence();

    class TickCadence
    {
    public:
        std::uint32_t NextFrameCount() const noexcept
        {
            return TickFrameCadence[index_];
        }

        void Advance() noexcept
        {
            ++index_;
            if (index_ == TickCadenceLength)
            {
                index_ = 0;
            }
        }

        void Reset() noexcept
        {
            index_ = 0;
        }

    private:
        std::size_t index_{};
    };
    inline const double PathInsetDiameterMm = scene::CornerMaxTubeMm;
    inline const double PathInsetRadiusMm = PathInsetDiameterMm * 0.5f;
    constexpr double TrailSeconds = 0.25;
    constexpr double SizeLagTicks = 1.0;
    constexpr double SizeLagSeconds = SizeLagTicks/TickRate;
    inline const double MinDiameterMm = scene::CornerMinTubeMm;
    inline const double RefDiameterMm = scene::CornerMaxTubeMm;
    constexpr double HistorySeconds = TrailSeconds + SizeLagSeconds;
    inline const double TrailStartStepMm = scene::ScreenChordMm/scene::RefWidthPx;
    inline const double TrailFullStepMm = scene::ScreenChordMm;
    constexpr std::size_t TrailSegmentCapacity =
        (TickRate + 3) / 4;
    constexpr std::size_t SourceFutureTicks = 2;
    constexpr std::size_t PursuitExtraTicks = 1;
    constexpr std::size_t VisibleFutureTicks =
        SourceFutureTicks + PursuitExtraTicks;
    constexpr std::size_t StartupPointCount = VisibleFutureTicks + 1;
    inline double TrailTimeForStep(const double mmPerTick) noexcept
    {
        if (!(mmPerTick > TrailStartStepMm)) return 0.0;
        if (mmPerTick >= TrailFullStepMm) return TrailSeconds;
        return TrailSeconds *
            (mmPerTick - TrailStartStepMm) /
            (TrailFullStepMm - TrailStartStepMm);
    }

    constexpr float TrailAlphaAtDist(
        const double distanceFromTail, const double currentLength) noexcept
    {
        if (!(currentLength > 0.0) || !(distanceFromTail > 0.0)) return 0.0f;
        if (distanceFromTail >= currentLength) return 1.0f;
        return static_cast<float>(distanceFromTail / currentLength);
    }
    struct TrailFrame
    {
        float dotXmm{};
        float dotYmm{};
        float dotZmm{static_cast<float>(scene::BaseLightZmm)};
        float dotRadiusMm{static_cast<float>(PathInsetRadiusMm)};
        float red{100.0f};
        float green{100.0f};
        float blue{100.0f};

        double leftRms{};
        double rightRms{};
        double instantaneousDbfs{-std::numeric_limits<double>::infinity()};
        double normalizedLevel{};
        double stereoBalance{};
        double spectralEntropy{};
        double planarHalfLifeSec{};
        double depthHalfLifeSec{};
    };

    class AudioTrailMotion
    {
    public:
        AudioTrailMotion();

        void Reset(double widthMm, double heightMm);
        void SetTopMap(
            const VerticalDbfsRange& range,
            const TopDecay& decay);
        VerticalDbfsRange VerticalRange() const noexcept
        {
            return {resetRange_.bottom,activeTopDbfs_};
        }
        double ActiveTopDbfs() const noexcept { return activeTopDbfs_; }
        const TrailFrame& Tick(
            std::span<const RawStereoPcm> samples,
            double widthMm,
            double heightMm);
        const TrailFrame& CurrentFrame() const noexcept;

    private:
        void SetTop(float dbfs) noexcept;
        void ResetTop() noexcept;
        float InnerTop(double dbfs,double amplitude,
            double heightMm) const noexcept;
        void AdvanceTop(double dbfs,double amplitude,
            double heightMm) noexcept;

        TrailFrame frame_{};
        VerticalDbfsRange resetRange_{};
        TopDecay topDecay_{};
        float activeTopDbfs_{};
        double bottomAmp_{};
        double ampScale_{};
        float fallPerTick_{1.0f/TickRate};
    };
}
