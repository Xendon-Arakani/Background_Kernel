#pragma once

#include "render/HdrToneMapper.hpp"
#include "scene/PhysicalUnits.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace bk::stroke
{
    struct Settings
    {
        double restWidthCm{scene::CornerMinTubeMm/10};
        double fullWidthCm{scene::CornerMaxTubeMm/10};
        double quietTrailCm{scene::ScreenChordMm/10};
        double loudTrailCm{scene::BackWallMm/10};
        double opacityPct{100};
        bool paintEnabled{true};
    };

    inline bool Valid(const Settings& value) noexcept
    {
        return std::isfinite(value.restWidthCm) &&
            std::isfinite(value.fullWidthCm) &&
            std::isfinite(value.quietTrailCm) &&
            std::isfinite(value.loudTrailCm) &&
            std::isfinite(value.opacityPct);
    }

    struct Compiled
    {
        float restRadiusPx{};
        float fullRadiusPx{};
        float quietTrailPx{};
        float loudTrailPx{};
        float opacity{};
        bool paintEnabled{};
    };

    inline Compiled Compile(const Settings& value,const float scale) noexcept
    {
        return {
            static_cast<float>(5*std::max(0.0,value.restWidthCm)*scale),
            static_cast<float>(5*std::max(0.0,value.fullWidthCm)*scale),
            static_cast<float>(10*std::max(0.0,value.quietTrailCm)*scale),
            static_cast<float>(10*std::max(0.0,value.loudTrailCm)*scale),
            static_cast<float>(std::clamp(value.opacityPct*0.01,0.0,1.0)),
            value.paintEnabled};
    }

    struct Point
    {
        float x{},y{};
    };

    struct Color
    {
        float red{},green{},blue{};
    };

    struct Shape
    {
        std::array<float,4> start{};
        std::array<float,4> end{};
        std::array<float,4> startColor{};
        std::array<float,4> endColor{};
    };

    struct TileRange
    {
        std::uint32_t first{},count{};
    };

    struct View
    {
        std::span<const Shape> shapes;
        std::span<const TileRange> ranges;
        std::span<const std::uint32_t> indices;
        float scale{},width{},height{},tilePx{},opacity{};
        bool paintEnabled{};
        std::uint32_t tileCols{},tileRows{};
        std::uint64_t version{};
    };

    static_assert(sizeof(Shape)==64);
}
