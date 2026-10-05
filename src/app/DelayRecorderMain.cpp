#include "audio/PcmRecord.hpp"
#include "audio/AudioLoopback.hpp"
#include "audio/FutureDelay.hpp"

#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    constexpr wchar_t StopEventName[] = L"Local\\BkRecordStop";
    HANDLE stopEvent{};

    BOOL WINAPI ConsoleControl(DWORD control)
    {
        if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT)
        {
            return FALSE;
        }
        SetEvent(stopEvent);
        return TRUE;
    }

    std::filesystem::path DefaultOutput()
    {
        GUID id{};
        if (FAILED(CoCreateGuid(&id)))
        {
            throw std::runtime_error("Creating recording identity failed");
        }
        wchar_t text[40]{};
        StringFromGUID2(id, text, 40);
        return std::filesystem::current_path() / L"recordings" / text;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && std::wstring(argv[1]) == L"--stop")
    {
        const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, StopEventName);
        if (!event) { return 0; }
        const bool signaled = SetEvent(event) != FALSE;
        CloseHandle(event);
        return signaled ? 0 : 1;
    }
    if (argc != 1 && !(argc == 3 && std::wstring(argv[1]) == L"--output"))
    {
        std::cout << "DelayRecorder --output DIR | --stop\n";
        return argc == 2 && std::wstring(argv[1]) == L"--help" ? 0 : 1;
    }

    int result = 1;
    try
    {
        const auto output = std::filesystem::absolute(
            argc == 3 ? std::filesystem::path(argv[2]) : DefaultOutput());
        if (std::filesystem::exists(output))
        {
            throw std::runtime_error("Output directory must not already exist");
        }
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, StopEventName);
        const DWORD creationError = GetLastError();
        if (!stopEvent || creationError == ERROR_ALREADY_EXISTS)
        {
            throw std::runtime_error("Recorder is already running, or stop event creation failed");
        }
        SetConsoleCtrlHandler(ConsoleControl, TRUE);
        bk::audio::PcmRecord captured;
        bk::audio::PcmRecord processed;
        bk::audio::FutureDelay gate;
        bk::audio::FutureTiming audit;
        audit.delayPackets = bk::audio::FutureDelay::FuturePacketCount;
        bk::audio::AudioLoopback capture;
        try
        {
            capture.StartStereoPcm();
            std::wcout << L"frames=480000 channels=2 format=f32\n"
                       << L"delay_packets=2\n"
                       << L"Output: " << output << std::endl;
            const HANDLE handles[]{stopEvent, capture.ReadyEvent()};
            bool cancelled = false;
            while (!processed.Complete() && !cancelled)
            {
                const DWORD ready = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (ready == WAIT_OBJECT_0)
                {
                    audit.stopReason = bk::audio::RecordStop::Cancelled;
                    break;
                }
                if (ready != WAIT_OBJECT_0 + 1)
                {
                    throw std::runtime_error("Waiting for audio readiness failed");
                }
                while (!processed.Complete())
                {
                    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
                    {
                        audit.stopReason = bk::audio::RecordStop::Cancelled;
                        cancelled = true;
                        break;
                    }
                    bk::audio::SourcePcmPacket packet;
                    if (!capture.ReadFullPacket(packet)) { break; }
                    auto released = gate.PushContiguous(packet);
                    captured.AppendPrefix(packet.pcm);
                    audit.packets.push_back({gate.InputPackets() - 1,
                        packet.sourceFirstFrame, packet.sourceHns,
                        packet.copyHns, packet.pcm.size(), packet.sourceFlags});
                    if (released)
                    {
                        const auto first = processed.Frames().size();
                        const auto count = std::min(released->packet.pcm.size(), processed.Remaining());
                        if (captured.Frames().size() < first + count ||
                            std::memcmp(captured.Frames().data() + first,
                                released->packet.pcm.data(), count * sizeof(bk::audio::RawStereoPcm)) != 0)
                        {
                            throw std::runtime_error("Delayed PCM diverged from the captured prefix");
                        }
                        processed.AppendPrefix(std::span(released->packet.pcm).first(count));
                        audit.releases.push_back({released->sourcePacketId,
                            released->releasePacketId, first, count,
                            released->futureFrames});
                    }
                }
            }
        }
        catch (const std::exception& error)
        {
            audit.stopReason = bk::audio::RecordStop::CaptureError;
            std::cerr << "capture=stopped error=" << error.what()
                      << " flush_withheld=false\n";
        }
        capture.Shutdown();
        audit.receivedFrames = gate.InputFrames();
        audit.bufferedPackets = gate.WithheldPackets();
        audit.bufferedFrames = gate.WithheldFrames();
        audit.releasedFrames = processed.Frames().size();
        captured.SaveNewDir(output, &audit);
        std::cout << "saved_frames=" << captured.Frames().size()
                  << " signed_values=" << captured.Frames().size() * 2
                  << " released_frames=" << audit.releasedFrames
                  << " complete=" << (processed.Complete() ? 1 : 0) << '\n'
                  << "received_packets=" << gate.InputPackets()
                  << " withheld_packets=" << gate.WithheldPackets()
                  << " withheld_frames=" << gate.WithheldFrames() << std::endl;
        result = processed.Complete() ? 0 : 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
    }
    SetConsoleCtrlHandler(ConsoleControl, FALSE);
    if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
    return result;
}
