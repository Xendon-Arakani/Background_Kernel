#pragma once
#include "scene/RoomGeometry.hpp"
#include <array>
#include <cmath>
#include <cstdint>

namespace bk::scene
{
    struct WallField
    {
        std::array<float,3> albedo{
            static_cast<float>(Albedo[0]),
            static_cast<float>(Albedo[1]),
            static_cast<float>(Albedo[2])};
        float timeSec{};
        std::array<float,3> bands{};
        float level{};
        float balance{};
        float entropy{};
    };

    inline bool ValidWallField(const WallField& field) noexcept
    {
        for(const float channel:field.albedo)
            if(!std::isfinite(channel) || channel<0 || channel>1)
                return false;
        if(!std::isfinite(field.timeSec) ||
            !std::isfinite(field.level) ||
            !std::isfinite(field.balance) ||
            !std::isfinite(field.entropy)) return false;
        for(const float band:field.bands)
            if(!std::isfinite(band)) return false;
        return true;
    }
}
