#pragma once
#include "audio/AudioLoopback.hpp"
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace bk::audio
{
    class AudioCapture
    {
    public:
        struct CaptureStats
        {
            std::uint64_t copiedFrames{};
            std::uint64_t epochBreaks{};
            std::uint64_t queuedFrames{};
        };
        ~AudioCapture() { Shutdown(); }
        void Start()
        {
            Shutdown();
            stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            ready_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(!stop_||!ready_) { Shutdown();throw std::runtime_error("Create independent audio capture events failed"); }
            try { worker_=std::thread([this]{ CaptureOnThread(); }); }
            catch(...) { Shutdown();throw; }
            if(WaitForSingleObject(ready_,INFINITE)!=WAIT_OBJECT_0) {
                Shutdown();throw std::runtime_error("Independent audio initialization handoff failed");
            }
            std::exception_ptr error;{ std::lock_guard lock(mutex_);error=error_; }
            if(error) { Shutdown();std::rethrow_exception(error); }
        }
        void Shutdown() noexcept
        {
            if(stop_) SetEvent(stop_);
            if(worker_.joinable()) worker_.join();
            if(stop_) CloseHandle(stop_);if(ready_) CloseHandle(ready_);
            stop_=ready_=nullptr;
            std::lock_guard lock(mutex_);
            queuedPcm_.clear();queuedQpc_.clear();queuedEdges_.clear();
            error_=nullptr;stats_={};
        }
        bool DrainEpochPcm(
            std::vector<RawStereoPcm>& dstPcm,
            std::vector<std::uint64_t>& dstQpc,
            std::vector<std::size_t>& dstEdges)
        {
            std::lock_guard lock(mutex_);
            if(error_) std::rethrow_exception(error_);
            const bool continuous=queuedEdges_.empty();
            if(dstPcm.size()!=dstQpc.size()) throw std::runtime_error("Visualizer PCM/time queues differ");
            const auto edgeBase=dstPcm.size();
            for(const auto edge:queuedEdges_) dstEdges.push_back(edgeBase+edge);
            queuedEdges_.clear();
            if(dstPcm.empty()) { dstPcm.swap(queuedPcm_);dstQpc.swap(queuedQpc_); }
            else {
                dstPcm.insert(dstPcm.end(),queuedPcm_.begin(),queuedPcm_.end());
                dstQpc.insert(dstQpc.end(),queuedQpc_.begin(),queuedQpc_.end());
                queuedPcm_.clear();queuedQpc_.clear();
            }
            return continuous;
        }
        CaptureStats ReadStats() const
        {
            std::lock_guard lock(mutex_);
            auto result=stats_;
            result.queuedFrames=queuedPcm_.size();
            return result;
        }
    private:
        void PublishEpoch(
            std::vector<RawStereoPcm>& pcm,
            std::vector<std::uint64_t>& qpc,
            const std::vector<std::size_t>& edges)
        {
            std::lock_guard lock(mutex_);
            const auto edgeBase=queuedPcm_.size();
            for(const auto edge:edges) queuedEdges_.push_back(edgeBase+edge);
            stats_.epochBreaks+=edges.size();
            if(pcm.size()!=qpc.size()) throw std::runtime_error("Captured PCM/time queues differ");
            stats_.copiedFrames+=pcm.size();
            if(queuedPcm_.empty()) { queuedPcm_.swap(pcm);queuedQpc_.swap(qpc); }
            else {
                queuedPcm_.insert(queuedPcm_.end(),pcm.begin(),pcm.end());
                queuedQpc_.insert(queuedQpc_.end(),qpc.begin(),qpc.end());
            }
        }
        void CaptureOnThread() noexcept
        {
            try {
                AudioLoopback source;source.StartStereoPcm();SetEvent(ready_);
                const HANDLE events[]{stop_,source.ReadyEvent()};
                std::vector<RawStereoPcm> pcm;std::vector<std::uint64_t> qpc;
                std::vector<std::size_t> edges;
                for(;;) {
                    const auto result=WaitForMultipleObjects(2,events,FALSE,INFINITE);
                    if(result==WAIT_OBJECT_0) break;
                    if(result!=WAIT_OBJECT_0+1) throw std::runtime_error("Independent WASAPI event wait failed");
                    pcm.clear();qpc.clear();edges.clear();
                    source.DrainReadyPcm(pcm,qpc,
                        std::numeric_limits<std::size_t>::max(),&edges);
                    PublishEpoch(pcm,qpc,edges);
                }
            } catch(...) {
                { std::lock_guard lock(mutex_);error_=std::current_exception(); }
                SetEvent(ready_);
            }
        }
        HANDLE stop_{},ready_{};
        std::thread worker_;
        mutable std::mutex mutex_;
        std::vector<RawStereoPcm> queuedPcm_;
        std::vector<std::uint64_t> queuedQpc_;
        std::vector<std::size_t> queuedEdges_;
        std::exception_ptr error_;
        CaptureStats stats_;
    };
}
