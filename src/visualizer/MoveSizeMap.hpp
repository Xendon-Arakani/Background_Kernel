#pragma once

#include "visualizer/ContinuousMotion.hpp"

#include <limits>
#include <stdexcept>

namespace bk::visualizer
{
    struct MoveSizeMap
    {
        double maxDiameterPx{scene::BaseMaxRefPx};
        double curveExponent{1.0};
        bool inverted{};
        bool operator==(const MoveSizeMap&) const = default;
    };

    inline bool ValidSizeCurve(const double exponent) noexcept
    {
        return std::isfinite(exponent) && exponent>0.0;
    }

    inline bool ValidMoveSizeMap(const MoveSizeMap& controls) noexcept
    {
        return scene::ValidMaxRefPx(controls.maxDiameterPx) &&
            ValidSizeCurve(controls.curveExponent);
    }

    struct SizeResponse
    {
        double lowRadiusMm{};
        double radiusSpanMm{};
        double curveExponent{1.0};
        bool inverted{};
        bool linear{true};
    };

    inline SizeResponse CompileSize(const MoveSizeMap& controls)
    {
        if(!ValidMoveSizeMap(controls))
            throw std::invalid_argument("Invalid displacement-size controls");
        const double lowRadiusMm=0.5*MinDiameterMm;
        const double highRadiusMm=
            0.5*scene::TubeMmForRefPx(controls.maxDiameterPx);
        return {lowRadiusMm,highRadiusMm-lowRadiusMm,
            controls.curveExponent,controls.inverted,
            controls.curveExponent==1.0};
    }

    inline double RadiusPerMoveMm(const double referenceMm,
        const SizeResponse& response)
    {
        return referenceMm > 0.0
            ? response.radiusSpanMm/referenceMm
            : 0.0;
    }

    inline double RadiusForMove(const MotionPoint displacement,
        const double referenceMm,
        const SizeResponse& response)
    {
        const double moveFraction = referenceMm > 0.0
            ? std::min(Length(displacement) / referenceMm, 1.0)
            : 0.0;
        const double curvedFraction=response.linear
            ? moveFraction : std::pow(moveFraction,response.curveExponent);
        const double diameterFraction=response.inverted
            ? 1.0-curvedFraction : curvedFraction;
        return std::lerp(response.lowRadiusMm,
            response.lowRadiusMm+response.radiusSpanMm,diameterFraction);
    }

    inline const MotionSegment& SegmentAtTick(const ContinuousMotion& motion, const double tick)
    {
        const auto& segments = motion.Segments();
        if (!std::isfinite(tick) || segments.empty() ||
            tick < static_cast<double>(segments.front().startTick) ||
            tick > static_cast<double>(segments.back().startTick + 1))
            throw std::logic_error("Requested dot-size history is not retained");
        const auto offset = static_cast<std::size_t>(
            std::floor(tick) - static_cast<double>(segments.front().startTick));
        return segments[std::min(offset, segments.size() - 1)];
    }

    inline MotionPoint PositionAtTick(const ContinuousMotion& motion, const double tick)
    {
        if (tick <= 0.0) return motion.InitialPosition();
        const auto& segment = SegmentAtTick(motion, tick);
        return EvalBezier(segment.controls, tick - static_cast<double>(segment.startTick));
    }

    inline TrailFrame FrameAtTick(
        const ContinuousMotion& motion,const double tick)
    {
        const auto& segment=SegmentAtTick(motion,std::max(0.0,tick));
        const double u=std::clamp(tick-static_cast<double>(segment.startTick),0.0,1.0);
        auto frame=segment.start;
        const auto position=EvalBezier(segment.controls,u);
        frame.dotXmm=static_cast<float>(position.x);
        frame.dotYmm=static_cast<float>(position.y);
        frame.dotZmm=static_cast<float>(position.z);
        frame.spectralEntropy=std::lerp(segment.start.spectralEntropy,
            segment.end.spectralEntropy,u);
        frame.red=static_cast<float>(std::lerp(segment.start.red,segment.end.red,u));
        frame.green=static_cast<float>(std::lerp(segment.start.green,segment.end.green,u));
        frame.blue=static_cast<float>(std::lerp(segment.start.blue,segment.end.blue,u));
        return frame;
    }

    inline double RadiusAtTick(const ContinuousMotion& motion, const double tick,
        const SizeResponse& response)
    {
        return RadiusForMove(PositionAtTick(motion, tick) -
            PositionAtTick(motion, tick - SizeLagTicks),
            motion.ShortSideMm(),response);
    }

    inline TrailFrame SizedCurrentFrame(const ContinuousMotion& motion,
        const SizeResponse& response)
    {
        auto frame = motion.CurrentFrame();
        frame.dotRadiusMm = static_cast<float>(motion.Ready()
            ? RadiusAtTick(motion, motion.CurrentTick(),response)
            : RadiusForMove({}, motion.ShortSideMm(),response));
        return frame;
    }

    inline TrailFrame SizedFrameAtTick(
        const ContinuousMotion& motion,const double tick,
        const SizeResponse& response)
    {
        auto frame=FrameAtTick(motion,tick);
        frame.dotRadiusMm=static_cast<float>(RadiusAtTick(motion,tick,response));
        return frame;
    }

    inline QuinticControls ClipCurve(QuinticControls controls, const double from, const double to)
    {
        if (to < 1.0) controls = SplitBezier(controls, to)[0];
        if (from > 0.0) controls = SplitBezier(controls, from / to)[1];
        return controls;
    }

    inline double RadiusChordError(const QuinticControls& current, const QuinticControls& past,
                                   const double referenceMm,
                                   const SizeResponse& response)
    {
        if (!(referenceMm > 0.0)) return 0.0;
        QuinticControls difference{};
        for (std::size_t i = 0; i < difference.size(); ++i) difference[i] = current[i] - past[i];
        auto direction = EvalBezier(difference, 0.5);
        double norm = Length(direction);
        if (norm == 0.0)
        {
            for (const auto point : difference)
                if ((norm = Length(point)) > 0.0) { direction = point; break; }
        }
        if (norm == 0.0) return 0.0;
        direction = direction * (1.0 / norm);
        if(!response.linear)
        {
            double lower=std::numeric_limits<double>::infinity(),upper=0.0;
            for(const auto point:difference)
            {
                lower=std::min(lower,Dot(direction,point));
                upper=std::max(upper,Length(point));
            }
            lower=std::max(lower,0.0);
            const auto radius=[&](const double distance)
            {
                return RadiusForMove({distance,0,0},referenceMm,response);
            };
            return std::abs(radius(lower)-radius(upper));
        }
        const double first = Length(difference.front());
        const double last = Length(difference.back());
        double error = 0.0;
        for (std::size_t i = 0; i < difference.size(); ++i)
        {
            const auto point = difference[i];
            const double chord = std::lerp(first, last, static_cast<double>(i) / 5.0);
            error = std::max({error, Length(point) - chord,
                chord - Dot(direction,point)});
        }
        const double cap = referenceMm;
        if ((first < cap && last > cap) || (last < cap && first > cap))
        {
            const double u = (cap - first) / (last - first);
            error += cap - std::lerp(std::min(first, cap), std::min(last, cap), u);
        }
        return error * RadiusPerMoveMm(referenceMm,response);
    }
}
