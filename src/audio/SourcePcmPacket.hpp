#pragma once

#include "audio/RawStereoPcm.hpp"

#include <vector>

namespace bk::audio
{
    struct SourcePcmPacket
    {
        std::vector<RawStereoPcm> pcm;
        std::uint64_t sourceFirstFrame{};
        std::uint64_t sourceHns{};
        std::uint64_t copyHns{};
        std::uint32_t sourceFlags{};
    };
}
