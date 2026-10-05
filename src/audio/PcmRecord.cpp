#include "audio/PcmRecord.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>

namespace bk::audio
{
    namespace
    {
        template<class Writer>
        void WriteThenCommit(const std::filesystem::path& final, Writer write)
        {
            auto temporary = final;
            temporary += L".partial";
            {
                std::ofstream stream(temporary, std::ios::binary);
                stream.exceptions(std::ios::badbit | std::ios::failbit);
                stream.imbue(std::locale::classic());
                write(stream);
                stream.flush();
                stream.close();
            }
            const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("Opening recording for durable flush failed");
            }
            const bool flushed = FlushFileBuffers(file) != FALSE;
            CloseHandle(file);
            if (!flushed || !MoveFileExW(temporary.c_str(), final.c_str(),
                                       MOVEFILE_WRITE_THROUGH))
            {
                throw std::runtime_error("Committing recording failed");
            }
        }
    }

    PcmRecord::PcmRecord()
    {
        frames_.reserve(ArchiveFrameCap);
    }

    std::size_t PcmRecord::AppendPrefix(const std::span<const RawStereoPcm> pcm)
    {
        const std::size_t count = std::min(pcm.size(), Remaining());
        if (count == 0) { return 0; }
        frames_.insert(frames_.end(), pcm.begin(), pcm.begin() + count);
        return count;
    }

    std::span<const RawStereoPcm> PcmRecord::Frames() const noexcept
    {
        return frames_;
    }

    std::size_t PcmRecord::Remaining() const noexcept
    {
        return ArchiveFrameCap - frames_.size();
    }

    bool PcmRecord::Complete() const noexcept
    {
        return frames_.size() == ArchiveFrameCap;
    }

    void PcmRecord::SaveNewDir(const std::filesystem::path& directory,
                                  const FutureTiming* delay) const
    {
        if (delay && delay->releasedFrames > frames_.size())
        {
            throw std::runtime_error("Released prefix exceeds captured recording");
        }
        if (!directory.parent_path().empty())
        {
            std::filesystem::create_directories(directory.parent_path());
        }
        if (!std::filesystem::create_directory(directory))
        {
            throw std::runtime_error("Recording directory already exists; refusing to overwrite");
        }
        WriteThenCommit(directory / "samples.f32", [&](std::ofstream& stream) {
            stream.write(reinterpret_cast<const char*>(frames_.data()),
                static_cast<std::streamsize>(frames_.size() * sizeof(RawStereoPcm)));
        });
        WriteThenCommit(directory / "samples.csv", [&](std::ofstream& stream) {
            stream << "frame,left,right\n";
            stream << std::setprecision(std::numeric_limits<float>::max_digits10);
            for (std::size_t index = 0; index < frames_.size(); ++index)
            {
                stream << std::noshowpos << index << ',' << std::showpos
                       << frames_[index].leftSigned << ',' << frames_[index].rightSigned << '\n';
            }
        });
        if (delay)
        {
            WriteThenCommit(directory / "packets.csv", [&](std::ofstream& stream) {
                stream << "packet_index,device_start_frame,source_qpc_100ns,copied_qpc_100ns,frame_count,flags\n";
                for (const auto& packet : delay->packets)
                {
                    stream << packet.packetIndex << ',' << packet.sourceFirstFrame << ','
                           << packet.sourceHns << ',' << packet.copyHns << ','
                           << packet.frameCount << ',' << packet.sourceFlags << '\n';
                }
            });
            WriteThenCommit(directory / "releases.csv", [&](std::ofstream& stream) {
                stream << "packet_index,released_after_packet_index,first_released_frame,released_frames,lookahead_frames\n";
                for (const auto& release : delay->releases)
                {
                    stream << release.packetIndex << ',' << release.releasePacketId << ','
                           << release.firstReleasedFrame << ',' << release.releasedFrames << ','
                           << release.futureFrames << '\n';
                }
            });
        }
        const auto released = delay ? delay->releasedFrames : frames_.size();
        const bool processingComplete = Complete() && released == ArchiveFrameCap;
        WriteThenCommit(directory / "recording.json", [&](std::ofstream& stream) {
            stream << "{\n"
                   << "  \"sample_rate_hz\": " << PcmRateHz << ",\n"
                   << "  \"channels\": [\"left\", \"right\"],\n"
                   << "  \"format\": \"little-endian IEEE-754 float32\",\n"
                   << "  \"layout\": \"L0,R0,L1,R1,...\",\n"
                   << "  \"frame_count\": " << frames_.size() << ",\n"
                   << "  \"scalar_count\": " << frames_.size() * 2 << ",\n"
                   << "  \"frame_limit\": " << ArchiveFrameCap << ",\n"
                   << "  \"complete\": " << (processingComplete ? "true" : "false") << ",\n"
                   << "  \"capture_complete\": " << (Complete() ? "true" : "false") << ",\n"
                   << "  \"released_frame_count\": " << released << ",\n"
                   << "  \"delay_packets\": " << (delay ? delay->delayPackets : 0) << ",\n";
            if (delay)
            {
                const char* reason = delay->stopReason == RecordStop::Cutoff ? "cutoff" :
                    delay->stopReason == RecordStop::Cancelled ? "cancelled" : "capture_error";
                stream << "  \"stop_reason\": \"" << reason << "\",\n"
                       << "  \"received_packet_count\": " << delay->packets.size() << ",\n"
                       << "  \"received_frame_count\": " << delay->receivedFrames << ",\n"
                       << "  \"buffered_packet_count\": " << delay->bufferedPackets << ",\n"
                       << "  \"buffered_frame_count\": " << delay->bufferedFrames << ",\n";
            }
            stream
                   << "  \"normalized\": false,\n"
                   << "  \"clipped\": false\n}\n";
        });
    }
}
