#pragma once
#include "audio/RawStereoPcm.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bk::visualizer
{
    struct SceneRgb { float red{}, green{}, blue{}; };
    constexpr double BaseMidHz = 1000.0;
    constexpr unsigned BaseSpanOct = 3;
    constexpr double BaseLowHz = BaseMidHz / (1u << BaseSpanOct);
    constexpr double BaseHighHz = BaseMidHz * (1u << BaseSpanOct);

    struct SpectralPalette
    {
        std::array<double,3> anchorHz{BaseLowHz,BaseMidHz,BaseHighHz};
        std::array<std::uint32_t,3> compatChannel{0,2,1};
        bool operator==(const SpectralPalette&) const = default;
    };
    inline bool ValidPalette(const SpectralPalette& palette)
    {
        return std::isfinite(palette.anchorHz[0]) && std::isfinite(palette.anchorHz[1]) && std::isfinite(palette.anchorHz[2]) &&
            0<=palette.anchorHz[0] && palette.anchorHz[0]<palette.anchorHz[1] && palette.anchorHz[1]<palette.anchorHz[2] && palette.anchorHz[2]<=audio::PcmRateHz/2.0 &&
            palette.compatChannel[0]<3 && palette.compatChannel[1]<3 && palette.compatChannel[2]<3 &&
            palette.compatChannel[0]!=palette.compatChannel[1] && palette.compatChannel[0]!=palette.compatChannel[2] && palette.compatChannel[1]!=palette.compatChannel[2];
    }

    struct SpectralResponse
    {
        std::array<double,3> anchorHz{};
        std::array<double,2> spanScale{};
        std::array<double,2> inverseAnchor{};
        std::array<bool,2> logarithmic{};
    };

    inline SpectralResponse CompileSpectrum(const SpectralPalette& palette)
    {
        if(!ValidPalette(palette))
            throw std::invalid_argument("Invalid spectral palette");
        SpectralResponse response;
        response.anchorHz=palette.anchorHz;
        for(std::size_t band=0;band<response.spanScale.size();++band)
        {
            response.logarithmic[band]=palette.anchorHz[band]>0.0;
            if(response.logarithmic[band])
            {
                response.inverseAnchor[band]=1.0/palette.anchorHz[band];
                response.spanScale[band]=1.0/std::log(
                    palette.anchorHz[band+1]/palette.anchorHz[band]);
            }
            else
            {
                response.spanScale[band]=1.0/
                    (palette.anchorHz[band+1]-palette.anchorHz[band]);
            }
        }
        return response;
    }

    inline std::array<double,3> BandWeightsAtHz(
        double hz,const SpectralResponse& response)
    {
        if(!std::isfinite(hz)) hz=0;
        std::array<double,3> weights{};
        if(hz<=response.anchorHz.front()) weights.front()=1;
        else if(hz>=response.anchorHz.back()) weights.back()=1;
        else {
            const std::size_t band=hz<=response.anchorHz[1]?0:1;
            const double offset=response.logarithmic[band]
                ? std::log(hz*response.inverseAnchor[band])
                : hz-response.anchorHz[band];
            const double t=offset*response.spanScale[band];
            weights[band]=1-t;weights[band+1]=t;
        }
        return weights;
    }
    inline std::array<double,3> BandWeightsAtHz(double hz,const SpectralPalette& palette={})
    {
        if(!std::isfinite(hz)) hz=0;
        std::array<double,3> weights{};
        if(hz<=palette.anchorHz.front()) weights.front()=1;
        else if(hz>=palette.anchorHz.back()) weights.back()=1;
        else {
            const std::size_t i=hz<=palette.anchorHz[1]?0:1;
            const double t=palette.anchorHz[i]>0 ? std::log(hz/palette.anchorHz[i])/std::log(palette.anchorHz[i+1]/palette.anchorHz[i])
                : (hz-palette.anchorHz[i])/(palette.anchorHz[i+1]-palette.anchorHz[i]);
            weights[i]=1-t;weights[i+1]=t;
        }
        return weights;
    }
    inline SceneRgb ColorAtHz(double hz,const SpectralPalette& palette={})
    {
        const auto weights=BandWeightsAtHz(hz,palette);
        std::array<float,3> rgb{};
        for(std::size_t i=0;i<weights.size();++i)
            rgb[palette.compatChannel[i]]=static_cast<float>(100*weights[i]);
        return {rgb[0],rgb[1],rgb[2]};
    }
    inline double UnitSpectralEntropy(std::span<const double> energy)
    {
        if(energy.size()<2) return 0;
        double total=0;
        for(const double e:energy) if(std::isfinite(e) && e>0) total+=e;
        if(!(total>0) || !std::isfinite(total)) return 0;
        double entropy=0;
        for(const double e:energy) if(std::isfinite(e) && e>0) {
            const double probability=e/total;entropy-=probability*std::log(probability);
        }
        return std::clamp(entropy/std::log(static_cast<double>(energy.size())),0.0,1.0);
    }
}
