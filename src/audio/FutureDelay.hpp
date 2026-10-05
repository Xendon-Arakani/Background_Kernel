#pragma once

#include "audio/SourcePcmPacket.hpp"

#include <deque>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bk::audio
{
    struct FutureRelease
    {
        SourcePcmPacket packet;
        std::uint64_t sourcePacketId{};
        std::uint64_t releasePacketId{};
        std::uint64_t futureFrames{};
    };

    class FutureDelay
    {
    public:
        static constexpr std::size_t FuturePacketCount = 2;

        std::optional<FutureRelease> PushContiguous(SourcePcmPacket packet)
        {
            if (gapFaulted_) { throw std::runtime_error("Packet gate is faulted; start a new session"); }
            constexpr std::uint32_t GapFlag = 1;
            constexpr std::uint32_t BadTimeFlag = 4;
            const auto count = static_cast<std::uint64_t>(packet.pcm.size());
            if (count == 0 || packet.sourceHns == 0 ||
                (packet.sourceFlags & BadTimeFlag) != 0 ||
                packet.sourceFirstFrame > std::numeric_limits<std::uint64_t>::max() - count)
            {
                LatchGapFault("Invalid packet identity or timestamp");
            }
            if (inputPackets_ != 0 &&
                ((packet.sourceFlags & GapFlag) != 0 ||
                 packet.sourceFirstFrame != nextSourceFrame_ ||
                 packet.sourceHns <= lastSourceQpc_))
            {
                LatchGapFault("Packet gap, overlap, or out-of-order source timestamp");
            }
            const auto end = packet.sourceFirstFrame + count;
            const auto sourceQpc = packet.sourceHns;
            withheld_.push_back({std::move(packet), inputPackets_});
            ++inputPackets_;
            inputFrames_ += count;
            nextSourceFrame_ = end;
            lastSourceQpc_ = sourceQpc;
            if (withheld_.size() <= FuturePacketCount) { return std::nullopt; }

            auto oldest = std::move(withheld_.front());
            withheld_.pop_front();
            return FutureRelease{std::move(oldest.packet), oldest.sourcePacketId,
                              inputPackets_ - 1, WithheldFrames()};
        }

        std::size_t WithheldPackets() const noexcept { return withheld_.size(); }
        std::uint64_t WithheldFrames() const noexcept
        {
            std::uint64_t result = 0;
            for (const auto& held : withheld_) { result += held.packet.pcm.size(); }
            return result;
        }
        std::uint64_t InputPackets() const noexcept { return inputPackets_; }
        std::uint64_t InputFrames() const noexcept { return inputFrames_; }
        bool GapFaulted() const noexcept { return gapFaulted_; }

    private:
        [[noreturn]] void LatchGapFault(const char* reason)
        {
            gapFaulted_ = true;
            throw std::runtime_error(reason);
        }
        struct HeldPacket
        {
            SourcePcmPacket packet;
            std::uint64_t sourcePacketId{};
        };
        std::deque<HeldPacket> withheld_;
        std::uint64_t inputPackets_{};
        std::uint64_t inputFrames_{};
        std::uint64_t nextSourceFrame_{};
        std::uint64_t lastSourceQpc_{};
        bool gapFaulted_{};
    };
}
