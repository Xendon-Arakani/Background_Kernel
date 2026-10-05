#pragma once

#include "visualizer/AudioTrailMotion.hpp"
#include "visualizer/RainbowPalette.hpp"
#include "visualizer/SpectralMapping.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <limits>
#include <stdexcept>

namespace bk::visualizer
{
    class AudioSpectrum
    {
    public:
        static constexpr std::size_t WindowFrameCap = 2 * MaximumFramesPerTick;
        static constexpr std::size_t FftSize = std::bit_ceil(WindowFrameCap);
        static constexpr std::size_t HannCacheSlots =
            3*(MaximumFramesPerTick-MinimumFramesPerTick)+2;

        void Reset() { priorFrameCount_ = windowFrames_ = 0; peakHz_ = entropy_ = 0.0; dbfs_=-std::numeric_limits<double>::infinity(); hasPriorTick_ = false; }

        void AnalyzeTick(const std::span<const RawStereoPcm> samples)
        {
            if (samples.size() > MaximumFramesPerTick)
                throw std::invalid_argument("Frequency color requires canonical audio ticks");
            hasPriorTick_ = priorFrameCount_ != 0 && !samples.empty();
            windowFrames_ = priorFrameCount_ + samples.size();
            leftFft_.fill({});
            rightFft_.fill({});
            const auto& hann=HannWeights(windowFrames_);
            double rawStereoEnergy = 0.0;
            for (std::size_t i = 0; i < windowFrames_; ++i)
            {
                const auto sample = i < priorFrameCount_ ? priorTickPcm_[i] : samples[i - priorFrameCount_];
                const double left = std::isfinite(sample.leftSigned) ? sample.leftSigned : 0.0;
                const double right = std::isfinite(sample.rightSigned) ? sample.rightSigned : 0.0;
                rawStereoEnergy += left * left + right * right;
                leftFft_[i] = left * hann[i];
                rightFft_[i] = right * hann[i];
            }
            std::copy(samples.begin(), samples.end(), priorTickPcm_.begin());
            priorFrameCount_ = samples.size();
            peakHz_ = entropy_ = 0.0;
            dbfs_=rawStereoEnergy>0?10.0*std::log10(rawStereoEnergy/(2.0*windowFrames_)):-std::numeric_limits<double>::infinity();
            if (rawStereoEnergy == 0.0) return;
            InPlaceFft(leftFft_);
            InPlaceFft(rightFft_);
            std::size_t peak = 0;
            double peakPower = -1.0;
            for (std::size_t bin = 0; bin <= FftSize / 2; ++bin)
            {
                binPower_[bin] = std::norm(leftFft_[bin]) + std::norm(rightFft_[bin]);
                if (binPower_[bin] > peakPower) { peakPower = binPower_[bin]; peak = bin; }
            }
            auto oneSideEnergy=binPower_;
            for(std::size_t bin=1;bin<FftSize/2;++bin) oneSideEnergy[bin]*=2;
            entropy_=UnitSpectralEntropy(oneSideEnergy);
            double peakBin = static_cast<double>(peak);
            if (peak > 0 && peak < FftSize / 2 && binPower_[peak - 1] > 0 && binPower_[peak + 1] > 0)
            {
                const double leftLogPower = std::log(binPower_[peak - 1]);
                const double peakLogPower = std::log(binPower_[peak]);
                const double rightLogPower = std::log(binPower_[peak + 1]);
                const double curvature = leftLogPower - 2.0 * peakLogPower + rightLogPower;
                if (curvature < 0.0) peakBin += 0.5 * (leftLogPower - rightLogPower) / curvature;
            }
            peakHz_ = peakBin * PcmRateHz / FftSize;
        }

        void ApplyToFrame(TrailFrame& frame) const
        {
            const auto color = ColorForPalette(peakHz_, response_, colors_);
            frame.red = color.red;
            frame.green = color.green;
            frame.blue = color.blue;
            frame.spectralEntropy = entropy_;
            frame.dotZmm = static_cast<float>(std::lerp(
                static_cast<double>(static_cast<float>(scene::BackWallMm-PathInsetRadiusMm)),
                static_cast<double>(static_cast<float>(PathInsetRadiusMm)),entropy_));
        }
        void SetPalette(const SpectralPalette& palette) {
            response_=CompileSpectrum(palette);
        }
        void SetPaletteIndex(const std::size_t paletteIndex)
        {
            if (paletteIndex >= PaletteCount)
                throw std::invalid_argument("Invalid RGB permutation index");
            colors_=PaletteMaps[paletteIndex];
        }
        double Entropy() const { return entropy_; }
        double PeakHz() const { return peakHz_; }
        double WindowDbfs() const { return dbfs_; }
        std::size_t WindowFrames() const { return windowFrames_; }
        bool HasPriorTick() const { return hasPriorTick_; }

    private:
        const std::array<double,WindowFrameCap>& HannWeights(const std::size_t count)
        {
            for(std::size_t slot=0;slot<hannCounts_.size();++slot)
                if(hannCounts_[slot]==count) return hannWindows_[slot];
            const auto slot=static_cast<std::size_t>(
                std::find(hannCounts_.begin(),hannCounts_.end(),0)-hannCounts_.begin());
            if(slot==hannCounts_.size())
                throw std::logic_error("Canonical tick cadence exceeded its Hann window lengths");
            auto& weights=hannWindows_[slot];
            for(std::size_t i=0;i<count;++i)
                weights[i]=count>1
                    ? 0.5-0.5*std::cos(2.0*std::numbers::pi*
                        static_cast<double>(i)/static_cast<double>(count-1))
                    : 1.0;
            hannCounts_[slot]=count;
            return weights;
        }

        static const std::array<std::complex<double>,std::bit_width(FftSize)-1>& FftStageRoots()
        {
            static const auto steps=[] {
                std::array<std::complex<double>,std::bit_width(FftSize)-1> result{};
                std::size_t stage=0;
                for(std::size_t length=2;length<=FftSize;length<<=1)
                    result[stage++]=std::polar(1.0,-2.0*std::numbers::pi/static_cast<double>(length));
                return result;
            }();
            return steps;
        }

        static void InPlaceFft(std::array<std::complex<double>, FftSize>& values)
        {
            for (std::size_t i = 1, j = 0; i < FftSize; ++i)
            {
                std::size_t bit = FftSize >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                if (i < j) std::swap(values[i], values[j]);
            }
            const auto& steps=FftStageRoots();
            std::size_t stage=0;
            for (std::size_t length = 2; length <= FftSize; length <<= 1,++stage)
            {
                const auto step = steps[stage];
                for (std::size_t offset = 0; offset < FftSize; offset += length)
                {
                    std::complex<double> twiddle{1, 0};
                    for (std::size_t j = 0; j < length / 2; ++j)
                    {
                        const auto a = values[offset + j];
                        const auto b = values[offset + j + length / 2] * twiddle;
                        values[offset + j] = a + b;
                        values[offset + j + length / 2] = a - b;
                        twiddle *= step;
                    }
                }
            }
        }
        std::array<RawStereoPcm, MaximumFramesPerTick> priorTickPcm_{};
        std::array<std::array<double,WindowFrameCap>,HannCacheSlots> hannWindows_{};
        std::array<std::size_t,HannCacheSlots> hannCounts_{};
        std::array<std::complex<double>, FftSize> leftFft_{}, rightFft_{};
        std::array<double, FftSize / 2 + 1> binPower_{};
        std::size_t priorFrameCount_{}, windowFrames_{};
        double peakHz_{},entropy_{};
        double dbfs_{-std::numeric_limits<double>::infinity()};
        SpectralResponse response_{CompileSpectrum(SpectralPalette{})};
        BandColorMap colors_{PaletteMaps.front()};
        bool hasPriorTick_{};
    };
}
