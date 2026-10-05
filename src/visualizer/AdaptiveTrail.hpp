#pragma once

#include "visualizer/MoveSizeMap.hpp"

#include <vector>

namespace bk::visualizer
{
    struct TrailPoint
    {
        float x{}, y{}, radius{};
        float red{}, green{}, blue{}, opacity{};
        double tick{}, ageSeconds{}, distanceFromTail{};
        float z{static_cast<float>(scene::BaseLightZmm)};
    };

    struct AdaptiveTrail
    {
        std::vector<TrailPoint> points;
        double requestedSec{}, actualSec{}, lengthMm{};
    };

    namespace trail_detail
    {
        inline const double CoverageErrorMm = 0.25 * std::min(scene::ScreenChordMm/scene::RefWidthPx, scene::ScreenHeightMm/scene::RefHeightPx);

        inline void AppendPaintPoint(AdaptiveTrail& path, const MotionSegment& segment,
                                     const MotionPoint position, const MotionPoint past,
                                     const double u, const double nowTick,
                                     const double sizeReferenceMm,
                                     const SizeResponse& sizeResponse)
        {
            TrailPoint point;
            point.x = static_cast<float>(position.x);
            point.y = static_cast<float>(position.y);
            point.z = static_cast<float>(position.z);
            point.radius = static_cast<float>(RadiusForMove(
                position-past,sizeReferenceMm,sizeResponse));
            point.red = static_cast<float>(std::lerp(segment.start.red, segment.end.red, u));
            point.green = static_cast<float>(std::lerp(segment.start.green, segment.end.green, u));
            point.blue = static_cast<float>(std::lerp(segment.start.blue, segment.end.blue, u));
            point.tick = static_cast<double>(segment.startTick) + u;
            point.ageSeconds = (nowTick - point.tick) / TickRate;
            if (!path.points.empty())
            {
                const auto& preceding = path.points.back();
                path.lengthMm += std::hypot(static_cast<double>(point.x) - preceding.x,
                                               static_cast<double>(point.y) - preceding.y,
                                               static_cast<double>(point.z) - preceding.z);
            }
            point.distanceFromTail = path.lengthMm;
            path.points.push_back(point);
        }

        inline void FlattenBezier(AdaptiveTrail& path, const MotionSegment& segment,
                                  const QuinticControls& controls, const QuinticControls& past,
                                  const double from, const double to, const double nowTick,
                                  const double sizeReferenceMm,
                                  const SizeResponse& sizeResponse)
        {
            double outlineErrorMm = 0.0;
            for (std::size_t i = 1; i + 1 < controls.size(); ++i)
            {
                const auto timeChord = Mix(controls.front(), controls.back(), static_cast<double>(i) / 5.0);
                const auto centerError = controls[i] - timeChord;
                outlineErrorMm = std::max(outlineErrorMm, Length(centerError));
            }
            const double middle = std::midpoint(from, to);
            outlineErrorMm += RadiusChordError(controls,past,sizeReferenceMm,sizeResponse);
            if (outlineErrorMm <= CoverageErrorMm || middle == from || middle == to)
            {
                AppendPaintPoint(path,segment,controls.back(),past.back(),to,nowTick,
                    sizeReferenceMm,sizeResponse);
                return;
            }
            const auto halves = SplitBezier(controls, 0.5);
            const auto pastHalves = SplitBezier(past, 0.5);
            FlattenBezier(path,segment,halves[0],pastHalves[0],from,middle,nowTick,
                sizeReferenceMm,sizeResponse);
            FlattenBezier(path,segment,halves[1],pastHalves[1],middle,to,nowTick,
                sizeReferenceMm,sizeResponse);
        }
    }

    inline AdaptiveTrail BuildTrailPath(const ContinuousMotion& motion,
                                     const double fromTick, const double toTick,
                                     const SizeResponse& sizeResponse)
    {
        AdaptiveTrail path;
        if (!(toTick > fromTick)) return path;
        const double sizeLag = SizeLagTicks;
        const double sizeReference = motion.ShortSideMm();
        for (const auto& segment : motion.Segments())
        {
            const double start = static_cast<double>(segment.startTick);
            double cursor = std::max(fromTick, start);
            const double end = std::min(toTick, start + 1.0);
            while (end > cursor)
            {
                const double referenceTick = cursor - sizeLag;
                double next;
                QuinticControls past{};
                if (referenceTick < 0.0)
                {
                    next = std::min(end, sizeLag);
                    past.fill(motion.InitialPosition());
                }
                else
                {
                    const auto& previous = SegmentAtTick(motion, referenceTick);
                    const double previousStart = static_cast<double>(previous.startTick);
                    next = std::min(end, previousStart + 1.0 + sizeLag);
                    past = ClipCurve(previous.controls, referenceTick - previousStart,
                        next - sizeLag - previousStart);
                }
                if (!(next > cursor)) throw std::logic_error("Non-advancing displacement history span");
                const double from = cursor - start, to = next - start;
                const auto clipped = ClipCurve(segment.controls, from, to);
                if (path.points.empty())
                    trail_detail::AppendPaintPoint(path,segment,clipped.front(),past.front(),from,
                        toTick,sizeReference,sizeResponse);
                trail_detail::FlattenBezier(path,segment,clipped,past,from,to,toTick,
                    sizeReference,sizeResponse);
                cursor = next;
            }
        }
        if (!path.points.empty())
            path.actualSec = (toTick - path.points.front().tick) / TickRate;
        return path;
    }

    inline AdaptiveTrail BuildAdaptiveTrail(const ContinuousMotion& motion,
        const SizeResponse& sizeResponse)
    {
        const double duration = TrailTimeForStep(motion.SpeedMmPerTick());
        auto path = BuildTrailPath(motion, motion.CurrentTick() - duration * TickRate,
                                    motion.CurrentTick(),sizeResponse);
        path.requestedSec = duration;
        for (auto& point : path.points)
            point.opacity = TrailAlphaAtDist(point.distanceFromTail, path.lengthMm);
        return path;
    }

    inline AdaptiveTrail BuildPresentSweep(const ContinuousMotion& motion,
        const SizeResponse& sizeResponse)
    {
        auto path=BuildTrailPath(motion,motion.PresentedTick(),motion.CurrentTick(),sizeResponse);
        for (auto& point : path.points) point.opacity = 1.0f;
        return path;
    }
}
