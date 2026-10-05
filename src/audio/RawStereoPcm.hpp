#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace bk::audio
{
    inline constexpr std::uint32_t PcmRateHz = 48'000;

    struct RawStereoPcm
    {
        float leftSigned{};
        float rightSigned{};
    };

    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    static_assert(sizeof(RawStereoPcm) == 8);
    static_assert(std::is_trivially_copyable_v<RawStereoPcm>);
    static_assert(std::endian::native == std::endian::little);
}
