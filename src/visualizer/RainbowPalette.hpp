#pragma once

#include "visualizer/SpectralMapping.hpp"

#include <array>
#include <string_view>

namespace bk::visualizer
{
    struct LinearBtColor
    {
        float red{};
        float green{};
        float blue{};
        bool operator==(const LinearBtColor&) const = default;
    };

    using BandColorMap = std::array<LinearBtColor, 3>;

    inline constexpr LinearBtColor BtRed{100.0f, 0.0f, 0.0f};
    inline constexpr LinearBtColor BtGreen{0.0f, 100.0f, 0.0f};
    inline constexpr LinearBtColor BtBlue{0.0f, 0.0f, 100.0f};
    inline constexpr LinearBtColor BtWhite{100.0f, 100.0f, 100.0f};

    inline constexpr std::array<std::string_view, 7> PaletteNames{{
        "RGB", "RBG", "GRB", "GBR", "BRG", "BGR", "White"}};

    inline constexpr std::array<BandColorMap, 7> PaletteMaps{{
        {{BtRed, BtGreen, BtBlue}},
        {{BtRed, BtBlue, BtGreen}},
        {{BtGreen, BtRed, BtBlue}},
        {{BtGreen, BtBlue, BtRed}},
        {{BtBlue, BtRed, BtGreen}},
        {{BtBlue, BtGreen, BtRed}},
        {{BtWhite, BtWhite, BtWhite}}
    }};

    inline constexpr std::size_t PaletteCount = PaletteMaps.size();

    inline SceneRgb ColorForPalette(
        const double frequencyHz,
        const SpectralResponse& response,
        const BandColorMap& palette) noexcept
    {
        const auto bandWeights = BandWeightsAtHz(frequencyHz, response);
        std::array<double, 3> rgb{};
        for (std::size_t band = 0; band < bandWeights.size(); ++band)
        {
            rgb[0] += bandWeights[band] * palette[band].red;
            rgb[1] += bandWeights[band] * palette[band].green;
            rgb[2] += bandWeights[band] * palette[band].blue;
        }
        return {
            static_cast<float>(rgb[0]),
            static_cast<float>(rgb[1]),
            static_cast<float>(rgb[2])};
    }
}
