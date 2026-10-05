#pragma once

#include "audio/RawStereoPcm.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace bk::audio
{
    struct CaptureTiming
    {
        std::uint64_t packetIndex{};
        std::uint64_t sourceFirstFrame{};
        std::uint64_t sourceHns{};
        std::uint64_t copyHns{};
        std::uint64_t frameCount{};
        std::uint32_t sourceFlags{};
    };

    struct ReleaseTiming
    {
        std::uint64_t packetIndex{};
        std::uint64_t releasePacketId{};
        std::uint64_t firstReleasedFrame{};
        std::uint64_t releasedFrames{};
        std::uint64_t futureFrames{};
    };

    enum class RecordStop { Cutoff, Cancelled, CaptureError };

    struct FutureTiming
    {
        std::size_t delayPackets{};
        std::size_t bufferedPackets{};
        std::uint64_t bufferedFrames{};
        std::uint64_t receivedFrames{};
        std::uint64_t releasedFrames{};
        RecordStop stopReason{RecordStop::Cutoff};
        std::vector<CaptureTiming> packets;
        std::vector<ReleaseTiming> releases;
    };

    class PcmRecord
    {
    public:
        static constexpr std::size_t ArchiveFrameCap = PcmRateHz * 10;

        PcmRecord();
        std::size_t AppendPrefix(std::span<const RawStereoPcm> pcm);
        std::span<const RawStereoPcm> Frames() const noexcept;
        std::size_t Remaining() const noexcept;
        bool Complete() const noexcept;

        void SaveNewDir(const std::filesystem::path& directory,
                        const FutureTiming* delay = nullptr) const;

    private:
        std::vector<RawStereoPcm> frames_;
    };
}
