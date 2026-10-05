#pragma once

#include <cstdint>

namespace bk::render
{
    enum class PresentationSync : std::uint32_t
    {
        Immediate = 0,
        VSync = 1
    };

    inline constexpr bool ValidSyncMode(const PresentationSync mode) noexcept
    {
        return mode==PresentationSync::Immediate || mode==PresentationSync::VSync;
    }
}
