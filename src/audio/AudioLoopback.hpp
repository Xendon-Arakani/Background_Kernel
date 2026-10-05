#pragma once

#include "audio/RawStereoPcm.hpp"
#include "audio/SourcePcmPacket.hpp"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <vector>
#include <limits>

namespace bk::audio
{
    class AudioLoopback
    {
    public:
        AudioLoopback() = default;
        ~AudioLoopback();

        AudioLoopback(const AudioLoopback&) = delete;
        AudioLoopback& operator=(const AudioLoopback&) = delete;

        void StartStereoPcm();
        void Shutdown() noexcept;
        bool DrainEventPcm(
            std::vector<RawStereoPcm>& dstPcm,
            std::vector<std::uint64_t>& dstHns);
        bool DrainReadyPcm(
            std::vector<RawStereoPcm>& dstPcm,
            std::vector<std::uint64_t>& dstHns,
            std::size_t dstFrameCap = std::numeric_limits<std::size_t>::max(),
            std::vector<std::size_t>* epochEdges = nullptr);
        bool ReadFullPacket(SourcePcmPacket& packet);
        HANDLE ReadyEvent() const noexcept;

    private:
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
        Microsoft::WRL::ComPtr<IMMDevice> endpoint_;
        Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
        Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient_;
        HANDLE event_{};
        bool comInitialized_{};
        bool started_{};
        bool firstPacket_{true};
    };
}
