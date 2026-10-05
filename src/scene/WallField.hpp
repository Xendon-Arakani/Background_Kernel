#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace bk::scene
{
    enum class WallMode : std::uint32_t
    {
        Fixed,
        Field
    };

    struct WallField
    {
        WallMode mode{WallMode::Fixed};
        std::uint32_t passes{};
        float timeSec{};
        std::array<float,3> bands{};
        float level{};
        float balance{};
        float entropy{};
    };

    inline bool ValidWallField(const WallField& field) noexcept
    {
        if(field.mode!=WallMode::Fixed &&
            field.mode!=WallMode::Field) return false;
        if(!std::isfinite(field.timeSec) ||
            !std::isfinite(field.level) ||
            !std::isfinite(field.balance) ||
            !std::isfinite(field.entropy)) return false;
        for(const float band:field.bands)
            if(!std::isfinite(band)) return false;
        return true;
    }
}
