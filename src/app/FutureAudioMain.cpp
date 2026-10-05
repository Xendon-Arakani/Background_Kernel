#include "audio/AudioLoopback.hpp"
#include "audio/FutureDelay.hpp"

#include <windows.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    std::wstring PidStopName(DWORD processId)
    {
        return L"Local\\BkCapture-" + std::to_wstring(processId);
    }
    HANDLE stopEvent{};
    BOOL WINAPI StopOnCtrl(DWORD control)
    {
        if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT) { return FALSE; }
        SetEvent(stopEvent);
        return TRUE;
    }
    std::uint64_t QpcHnsNow()
    {
        LARGE_INTEGER qpc{}, frequency{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&frequency);
        return (qpc.QuadPart / frequency.QuadPart) * 10'000'000ULL +
            (qpc.QuadPart % frequency.QuadPart) * 10'000'000ULL / frequency.QuadPart;
    }
#pragma pack(push, 1)
    struct FutureWireHead
    {
        std::uint32_t magic{0x31574B42}, version{1};
        std::uint64_t sourcePacketId{}, releasePacketId{}, sourceFirstFrame{};
        std::uint64_t sourceQpc{}, copyQpc{}, releaseQpc{}, futureFrames{};
        std::uint64_t firstOutFrame{};
        std::uint32_t frameCount{}, sourceFlags{};
    };
#pragma pack(pop)
    static_assert(sizeof(FutureWireHead) == 80);
    void WriteAll(const void* data, DWORD bytes)
    {
        const auto* next = static_cast<const char*>(data);
        while (bytes)
        {
            DWORD written{};
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), next, bytes, &written, nullptr) || !written)
            {
                throw std::runtime_error("PCM consumer pipe closed or write failed");
            }
            next += written;
            bytes -= written;
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring(argv[1]) == L"--stop")
    {
        wchar_t* end{};
        const auto processId = wcstoul(argv[2], &end, 10);
        if (!processId || *end) { return 1; }
        const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, PidStopName(processId).c_str());
        if (!event) { return 0; }
        const bool ok = SetEvent(event) != FALSE;
        CloseHandle(event);
        return ok ? 0 : 1;
    }
    if (argc != 1) { std::cerr << "FutureAudio --stop PID\n"; return 1; }
    int result = 0;
    try
    {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, PidStopName(GetCurrentProcessId()).c_str());
        const auto error = GetLastError();
        if (!stopEvent || error == ERROR_ALREADY_EXISTS)
        {
            throw std::runtime_error("Tiny waveform capture already active or event unavailable");
        }
        SetConsoleCtrlHandler(StopOnCtrl, TRUE);
        bk::audio::AudioLoopback capture;
        bk::audio::FutureDelay gate;
        capture.StartStereoPcm();
        const HANDLE handles[]{stopEvent, capture.ReadyEvent()};
        std::uint64_t firstFrame = 0;
        bool stopping = false;
        while (!stopping)
        {
            const auto ready = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (ready == WAIT_OBJECT_0) { break; }
            if (ready != WAIT_OBJECT_0 + 1) { throw std::runtime_error("Audio readiness wait failed"); }
            while (true)
            {
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) { stopping = true; break; }
                bk::audio::SourcePcmPacket packet;
                if (!capture.ReadFullPacket(packet)) { break; }
                auto released = gate.PushContiguous(std::move(packet));
                if (!released) { continue; }
                const auto& source = released->packet;
                const FutureWireHead header{0x31574B42, 1, released->sourcePacketId,
                    released->releasePacketId, source.sourceFirstFrame,
                    source.sourceHns, source.copyHns, QpcHnsNow(),
                    released->futureFrames, firstFrame,
                    static_cast<std::uint32_t>(source.pcm.size()), source.sourceFlags};
                WriteAll(&header, sizeof(header));
                WriteAll(source.pcm.data(), header.frameCount * sizeof(bk::audio::RawStereoPcm));
                firstFrame += header.frameCount;
            }
        }
        capture.Shutdown();
        std::cerr << "capture=stopped released_frames=" << firstFrame
                  << " withheld_packets=" << gate.WithheldPackets() << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << "capture=fault error=" << error.what() << '\n';
        result = 1;
    }
    SetConsoleCtrlHandler(StopOnCtrl, FALSE);
    if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
    return result;
}
