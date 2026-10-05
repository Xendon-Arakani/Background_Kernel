#include "audio/AudioLoopback.hpp"

#include <ksmedia.h>
#include <mmreg.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace bk::audio
{
    namespace
    {
        void ThrowIfFailed(const HRESULT result, const char* operation)
        {
            if (FAILED(result))
            {
                char message[192]{};
                sprintf_s(
                    message,
                    "%s failed with HRESULT 0x%08X",
                    operation,
                    static_cast<unsigned int>(result));
                throw std::runtime_error(message);
            }
        }

        WAVEFORMATEXTENSIBLE ExactStereoPcm()
        {
            WAVEFORMATEXTENSIBLE format{};
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels = 2;
            format.Format.nSamplesPerSec = PcmRateHz;
            format.Format.wBitsPerSample = 32;
            format.Format.nBlockAlign = 8;
            format.Format.nAvgBytesPerSec =
                PcmRateHz * format.Format.nBlockAlign;
            format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) -
                sizeof(WAVEFORMATEX);
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            return format;
        }
    }

    AudioLoopback::~AudioLoopback()
    {
        Shutdown();
    }

    void AudioLoopback::StartStereoPcm()
    {
        Shutdown();
        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(comResult))
        {
            comInitialized_ = true;
        }
        else if (comResult != RPC_E_CHANGED_MODE)
        {
            ThrowIfFailed(comResult, "CoInitializeEx");
        }

        try
        {
            ThrowIfFailed(
                CoCreateInstance(
                    __uuidof(MMDeviceEnumerator),
                    nullptr,
                    CLSCTX_ALL,
                    IID_PPV_ARGS(&enumerator_)),
                "Create MMDeviceEnumerator");
            ThrowIfFailed(
                enumerator_->GetDefaultAudioEndpoint(
                    eRender, eConsole, &endpoint_),
                "Get default render endpoint");
            ThrowIfFailed(
                endpoint_->Activate(
                    __uuidof(IAudioClient),
                    CLSCTX_ALL,
                    nullptr,
                    reinterpret_cast<void**>(audioClient_.GetAddressOf())),
                "Activate WASAPI audio client");

            const auto format = ExactStereoPcm();
            constexpr DWORD streamFlags =
                AUDCLNT_STREAMFLAGS_LOOPBACK |
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            ThrowIfFailed(
                audioClient_->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    streamFlags,
                    0,
                    0,
                    &format.Format,
                    nullptr),
                "Initialize 48 kHz stereo loopback");

            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event_)
            {
                ThrowIfFailed(
                    HRESULT_FROM_WIN32(GetLastError()),
                    "Create WASAPI event");
            }
            ThrowIfFailed(
                audioClient_->SetEventHandle(event_),
                "Set WASAPI event handle");
            ThrowIfFailed(
                audioClient_->GetService(
                    IID_PPV_ARGS(&captureClient_)),
                "Get WASAPI capture service");
            ThrowIfFailed(audioClient_->Start(), "Start WASAPI loopback");
            started_ = true;
            firstPacket_ = true;
        }
        catch (...)
        {
            Shutdown();
            throw;
        }
    }

    void AudioLoopback::Shutdown() noexcept
    {
        if (audioClient_ && started_)
        {
            audioClient_->Stop();
        }
        started_ = false;
        firstPacket_ = true;
        captureClient_.Reset();
        audioClient_.Reset();
        endpoint_.Reset();
        enumerator_.Reset();
        if (event_)
        {
            CloseHandle(event_);
            event_ = nullptr;
        }
        if (comInitialized_)
        {
            CoUninitialize();
            comInitialized_ = false;
        }
    }

    bool AudioLoopback::DrainEventPcm(
        std::vector<RawStereoPcm>& dstPcm,
        std::vector<std::uint64_t>& dstHns)
    {
        if (!captureClient_)
        {
            return true;
        }
        if (WaitForSingleObject(event_, 0) != WAIT_OBJECT_0)
        {
            return true;
        }
        return DrainReadyPcm(dstPcm, dstHns);
    }

    bool AudioLoopback::DrainReadyPcm(
        std::vector<RawStereoPcm>& dstPcm,
        std::vector<std::uint64_t>& dstHns,
        const std::size_t dstFrameCap,
        std::vector<std::size_t>* epochEdges)
    {
        if (!captureClient_)
        {
            return true;
        }
        if (dstPcm.size() != dstHns.size())
        {
            throw std::runtime_error(
                "PCM and QPC loopback queues lost alignment");
        }

        UINT32 packetFrames = 0;
        ThrowIfFailed(
            captureClient_->GetNextPacketSize(&packetFrames),
            "Get WASAPI packet size");
        bool continuous=true;
        while (packetFrames != 0 && dstPcm.size() < dstFrameCap)
        {
            BYTE* bytes = nullptr;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            ThrowIfFailed(
                captureClient_->GetBuffer(
                    &bytes,
                    &packetFrames,
                    &flags,
                    &devicePosition,
                    &qpcPosition),
                "Get WASAPI loopback buffer");

            if (firstPacket_)
            {
                firstPacket_ = false;
            }
            else if ((flags &
                AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0)
            {
                continuous=false;
                if(epochEdges) epochEdges->push_back(dstPcm.size());
                else {dstPcm.clear();dstHns.clear();}
            }

            if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ||
                qpcPosition == 0)
            {
                if(!epochEdges)
                {
                    dstPcm.clear();dstHns.clear();
                    captureClient_->ReleaseBuffer(packetFrames);
                    return false;
                }
                epochEdges->push_back(dstPcm.size());
                continuous=false;
                ThrowIfFailed(captureClient_->ReleaseBuffer(packetFrames),
                    "Release untimed WASAPI packet");
                ThrowIfFailed(captureClient_->GetNextPacketSize(&packetFrames),
                    "Get next WASAPI packet size");
                continue;
            }
            const std::size_t start = dstPcm.size();
            const std::size_t retainedFrames = std::min<std::size_t>(
                packetFrames, dstFrameCap - start);
            dstPcm.resize(start + retainedFrames);
            dstHns.resize(start + retainedFrames);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !bytes)
            {
                for (std::size_t index = 0; index < retainedFrames; ++index)
                {
                    dstPcm[start + index] = {};
                }
            }
            else
            {
                const auto* values = reinterpret_cast<const float*>(bytes);
                for (std::size_t index = 0; index < retainedFrames; ++index)
                {
                    dstPcm[start + index] = {
                        values[index * 2],
                        values[index * 2 + 1],
                    };
                }
            }
            for (std::size_t index = 0; index < retainedFrames; ++index)
            {
                dstHns[start + index] = qpcPosition +
                    static_cast<std::uint64_t>(index) * 10'000'000ULL /
                        PcmRateHz;
            }
            ThrowIfFailed(
                captureClient_->ReleaseBuffer(packetFrames),
                "Release WASAPI loopback buffer");
            ThrowIfFailed(
                captureClient_->GetNextPacketSize(&packetFrames),
                "Get next WASAPI packet size");
        }
        return continuous;
    }

    bool AudioLoopback::ReadFullPacket(SourcePcmPacket& packet)
    {
        if (!captureClient_) { return false; }
        UINT32 frames = 0;
        ThrowIfFailed(captureClient_->GetNextPacketSize(&frames), "Get WASAPI packet size");
        if (frames == 0) { return false; }
        BYTE* bytes = nullptr;
        DWORD flags = 0;
        UINT64 device = 0, qpc = 0;
        ThrowIfFailed(captureClient_->GetBuffer(&bytes, &frames, &flags, &device, &qpc),
                      "Get complete WASAPI packet");
        if (frames == 0) { return false; }
        SourcePcmPacket copied;
        try
        {
            if ((!firstPacket_ && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) ||
                (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 || qpc == 0)
            {
                throw std::runtime_error("WASAPI packet discontinuity or timestamp error");
            }
            copied.sourceFirstFrame = device;
            copied.sourceHns = qpc;
            copied.sourceFlags = flags;
            copied.pcm.resize(frames);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0)
            {
                if (!bytes) { throw std::runtime_error("WASAPI packet has no data"); }
                std::memcpy(copied.pcm.data(), bytes,
                            copied.pcm.size() * sizeof(RawStereoPcm));
            }
        }
        catch (...)
        {
            captureClient_->ReleaseBuffer(frames);
            throw;
        }
        ThrowIfFailed(captureClient_->ReleaseBuffer(frames), "Release complete WASAPI packet");
        firstPacket_ = false;
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const auto ticks = static_cast<std::uint64_t>(now.QuadPart);
        const auto rate = static_cast<std::uint64_t>(frequency.QuadPart);
        copied.copyHns = (ticks / rate) * 10'000'000ULL +
                             (ticks % rate) * 10'000'000ULL / rate;
        packet = std::move(copied);
        return true;
    }

    HANDLE AudioLoopback::ReadyEvent() const noexcept
    {
        return event_;
    }
}
