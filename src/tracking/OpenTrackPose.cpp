#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "tracking/OpenTrackPose.hpp"
#include <bit>
#include <cstring>
#include <mutex>
#include <thread>

namespace bk::tracking
{
    bool DecodeExactPose(std::span<const std::byte> packet,PoseCmDeg& pose)
    {
        static_assert(sizeof(double)==8 && std::endian::native==std::endian::little);
        if(packet.size()!=sizeof(PoseCmDeg)) return false;
        PoseCmDeg candidate;std::memcpy(candidate.data(),packet.data(),sizeof(candidate));
        for(double value:candidate) if(!std::isfinite(value)) return false;
        pose=candidate;return true;
    }
    struct OpenTrackPose::Impl
    {
        SOCKET socket{INVALID_SOCKET};
        WSAEVENT readable{WSA_INVALID_EVENT};
        HANDLE stop{},updated{};
        bool wsa{};
        std::uint16_t port{};
        mutable std::mutex mutex;
        PoseSample latest;
        std::thread worker;
        void SetSocketError(int error) { std::lock_guard lock(mutex);latest.socketError=static_cast<std::uint32_t>(error); }
        void Start(std::uint16_t requested)
        {
            WSADATA data{};const auto startup=WSAStartup(MAKEWORD(2,2),&data);
            if(startup) { SetSocketError(startup);return; } wsa=true;
            socket=WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,0);
            if(socket==INVALID_SOCKET) { SetSocketError(WSAGetLastError());return; }
            BOOL exclusive=TRUE;
            if(setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))) { SetSocketError(WSAGetLastError());return; }
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(requested);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(bind(socket,reinterpret_cast<const sockaddr*>(&address),sizeof(address))) { SetSocketError(WSAGetLastError());return; }
            int bytes=sizeof(address);
            if(getsockname(socket,reinterpret_cast<sockaddr*>(&address),&bytes)) { SetSocketError(WSAGetLastError());return; }
            port=ntohs(address.sin_port);
            readable=WSACreateEvent();stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);updated=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(readable==WSA_INVALID_EVENT||!stop||!updated) { SetSocketError(static_cast<int>(GetLastError()));return; }
            if(WSAEventSelect(socket,readable,FD_READ)) { SetSocketError(WSAGetLastError());return; }
            worker=std::thread([this] { Run(); });
        }
        void Run()
        {
            const HANDLE events[]{stop,readable};
            for(;;)
            {
                const auto ready=WaitForMultipleObjects(2,events,FALSE,INFINITE);
                if(ready==WAIT_OBJECT_0) return;
                if(ready!=WAIT_OBJECT_0+1) { SetSocketError(static_cast<int>(GetLastError()));return; }
                WSANETWORKEVENTS network{};
                if(WSAEnumNetworkEvents(socket,readable,&network)) { SetSocketError(WSAGetLastError());return; }
                if(network.iErrorCode[FD_READ_BIT]) { SetSocketError(network.iErrorCode[FD_READ_BIT]);return; }
                for(;;)
                {
                    std::array<std::byte,sizeof(PoseCmDeg)+1> sizeProbe{};
                    const int count=recv(socket,reinterpret_cast<char*>(sizeProbe.data()),static_cast<int>(sizeProbe.size()),0);
                    if(count==SOCKET_ERROR)
                    {
                        const int error=WSAGetLastError();
                        if(error==WSAEWOULDBLOCK) break;
                        if(error==WSAEMSGSIZE) { std::lock_guard lock(mutex);++latest.badPackets;continue; }
                        SetSocketError(error);return;
                    }
                    PoseCmDeg pose;const bool valid=DecodeExactPose(std::span(sizeProbe.data(),static_cast<std::size_t>(count)),pose);
                    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
                    {
                        std::lock_guard lock(mutex);
                        if(valid) { latest.pose=pose;++latest.validPackets;latest.recvQpc=qpc.QuadPart; }
                        else ++latest.badPackets;
                    }
                    SetEvent(updated);
                    if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0) return;
                }
            }
        }
        ~Impl()
        {
            if(stop) SetEvent(stop);
            if(worker.joinable()) worker.join();
            if(socket!=INVALID_SOCKET) closesocket(socket);
            if(readable!=WSA_INVALID_EVENT) WSACloseEvent(readable);
            if(stop) CloseHandle(stop);
            if(updated) CloseHandle(updated);
            if(wsa) WSACleanup();
        }
    };
    OpenTrackPose::OpenTrackPose(std::uint16_t port):impl_(std::make_unique<Impl>()) { impl_->Start(port); }
    OpenTrackPose::~OpenTrackPose()=default;
    PoseSample OpenTrackPose::LatestPose() const { std::lock_guard lock(impl_->mutex);return impl_->latest; }
    std::uint16_t OpenTrackPose::Port() const { return impl_->port; }
    void* OpenTrackPose::PoseReadyEvent() const { return impl_->updated; }
}
