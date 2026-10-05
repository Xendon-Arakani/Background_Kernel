#pragma once

#include "visualizer/AudioTrailMotion.hpp"
#include "visualizer/QuinticMotion.hpp"

#include <deque>

namespace bk::visualizer
{
    inline MotionPoint PositionOf(const TrailFrame& frame)
    {
        return {frame.dotXmm, frame.dotYmm, frame.dotZmm};
    }

    struct MotionSegment
    {
        std::uint64_t startTick{};
        QuinticControls controls{};
        TrailFrame start{}, end{};
    };

    class AudioMotionClock
    {
    public:
        void Reset() { *this = {}; }
        void AnchorSourceTick(const std::uint64_t tick, const double sourceHns)
        {
            tick_ = tick;
            sourceHns_ = sourceHns;
            ready_ = true;
        }
        double SourceReadyTick(const double nowHns) const
        {
            if (!ready_) return 0.0;
            return static_cast<double>(tick_) +
                (nowHns - sourceHns_) * TickRate / 10'000'000.0 -
                static_cast<double>(SourceFutureTicks);
        }
    private:
        std::uint64_t tick_{};
        double sourceHns_{};
        bool ready_{};
    };

    class ContinuousMotion
    {
    public:
        void Reset(const double widthMm, const double heightMm)
        {
            *this = {};
            shortSideMm_=std::min(widthMm,heightMm);
            boxMin_ = {static_cast<float>(std::min(PathInsetRadiusMm,0.5*widthMm)),
                      static_cast<float>(std::min(PathInsetRadiusMm,0.5*heightMm)),
                      static_cast<float>(PathInsetRadiusMm)};
            boxMax_ = {static_cast<float>(widthMm-std::min(PathInsetRadiusMm,0.5*widthMm)),
                      static_cast<float>(heightMm-std::min(PathInsetRadiusMm,0.5*heightMm)),
                      static_cast<float>(scene::BackWallMm-PathInsetRadiusMm)};
        }

        std::uint64_t PushWaypoint(const TrailFrame& frame)
        {
            const auto index = nextTick_++;
            if (index == 0) initialPosition_ = PositionOf(frame);
            waypoints_.push_back({index, frame});
            return index;
        }

        void AdvanceToTick(const double requestedTick)
        {
            if (!std::isfinite(requestedTick)) return;
            if (!started_)
            {
                if (waypoints_.size() < SourceFutureTicks + 1) return;
                started_ = true;
                tick_ = static_cast<double>(waypoints_.front().tick);
                presentedTick_ = tick_;
            }
            const double target = std::max(tick_, requestedTick);
            for (;;)
            {
                if (!active_)
                {
                    if (waypoints_.size() < SourceFutureTicks + 1) break;
                    const auto& current = waypoints_[0];
                    const auto& next = waypoints_[1];
                    const auto& future = waypoints_[2];
                    const auto solved = SolveMinJerk(PositionOf(current.frame), PositionOf(next.frame),
                        PositionOf(future.frame), velocity_, acceleration_, boxMin_, boxMax_);
                    segments_.push_back({current.tick, solved.controls, current.frame, next.frame});
                    ++solveCount_;
                    hullSolveCount_ += solved.hullConstrained;
                    active_ = true;
                }
                if (!(target > tick_)) break;
                const auto& segment = segments_.back();
                const double endTick = static_cast<double>(segment.startTick + 1);
                tick_ = std::min(target, endTick);
                if (tick_ < endTick) break;
                velocity_ = Velocity(segment.controls, 1.0);
                acceleration_ = Acceleration(segment.controls, 1.0);
                const auto displacement = PositionOf(segment.end) - PositionOf(segment.start);
                speedMmPerTick_ = Length(displacement);
                waypoints_.pop_front();
                active_ = false;
            }
            waitingForFuture_ = !active_ && target > tick_;
        }

        TrailFrame CurrentFrame() const
        {
            if (!started_) return {};
            if (!active_) return waypoints_.front().frame;
            const auto& segment = segments_.back();
            const double u = tick_ - static_cast<double>(segment.startTick);
            auto frame = segment.start;
            const auto position = EvalBezier(segment.controls, u);
            frame.dotXmm = static_cast<float>(position.x);
            frame.dotYmm = static_cast<float>(position.y);
            frame.dotZmm = static_cast<float>(position.z);
            frame.spectralEntropy = std::lerp(segment.start.spectralEntropy,segment.end.spectralEntropy,u);
            frame.red = static_cast<float>(std::lerp(segment.start.red, segment.end.red, u));
            frame.green = static_cast<float>(std::lerp(segment.start.green, segment.end.green, u));
            frame.blue = static_cast<float>(std::lerp(segment.start.blue, segment.end.blue, u));
            return frame;
        }

        void MarkPresented()
        {
            if (!started_) return;
            presentedTick_ = tick_;
            const double cutoff = tick_ - HistorySeconds * TickRate;
            while (segments_.size() > 1 &&
                static_cast<double>(segments_.front().startTick + 1) <= cutoff)
                segments_.pop_front();
        }

        bool Ready() const { return started_; }
        bool WaitingForFuture() const { return waitingForFuture_; }
        double CurrentTick() const { return tick_; }
        double PresentedTick() const { return presentedTick_; }
        double SpeedMmPerTick() const { return speedMmPerTick_; }
        MotionPoint InitialPosition() const { return initialPosition_; }
        double ShortSideMm() const
        {
            return shortSideMm_;
        }
        std::uint64_t SolveCount() const { return solveCount_; }
        std::uint64_t BoundSolveCount() const { return hullSolveCount_; }
        const std::deque<MotionSegment>& Segments() const { return segments_; }

    private:
        struct Waypoint { std::uint64_t tick{}; TrailFrame frame{}; };
        std::deque<Waypoint> waypoints_;
        std::deque<MotionSegment> segments_;
        MotionPoint boxMin_{}, boxMax_{}, velocity_{}, acceleration_{}, initialPosition_{};
        double tick_{}, presentedTick_{}, speedMmPerTick_{},shortSideMm_{};
        std::uint64_t nextTick_{}, solveCount_{}, hullSolveCount_{};
        bool started_{}, active_{}, waitingForFuture_{};
    };
}
