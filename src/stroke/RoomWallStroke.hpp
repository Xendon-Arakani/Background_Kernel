#pragma once

#include "stroke/WallStrokeSettings.hpp"
#include "stroke/RayOrigin.hpp"
#include "scene/BakedRoomLight.hpp"
#include "scene/RoomGeometry.hpp"
#include "visualizer/AdaptiveTrail.hpp"
#include "visualizer/QuinticMotion.hpp"
#include "visualizer/AudioTrailMotion.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace bk::stroke
{
    class RoomWallStroke
    {
        struct WallHit
        {
            Point point{};
            scene::Vec world{};
            unsigned face{};
            bool valid{};
        };

        struct PaintHit
        {
            WallHit wall{};
            visualizer::MotionPoint target{};
            scene::Vec origin{};
            scene::Vec direction{};
            Color color{};
        };

        struct TrailLine
        {
            Point start{},end{};
            float length{};
            float startRadius{},endRadius{};
            Color startColor{},endColor{};
            std::uint32_t faceMask{};
        };

    public:
        void Configure(const Settings& settings,const CastPoint& cast,
            const visualizer::VerticalDbfsRange& dbRange)
        {
            settings_=settings;
            compiled_=Compile(settings,Scale());
            MoveOrigin(cast);
            dbRange_=dbRange;
            const float spacing=std::min(DepthMm(),
                static_cast<float>(scene::ScreenHeightMm))*Scale()/
                    scene::LightingSide;
            tilePx_=std::ceil(std::max({1.0f,spacing,
                2*std::max(compiled_.restRadiusPx,
                    compiled_.fullRadiusPx)+2}));
            tileCols_=static_cast<std::uint32_t>(
                std::ceil(NetWidth()/tilePx_));
            tileRows_=static_cast<std::uint32_t>(
                std::ceil(NetHeight()/tilePx_));
            ranges_.resize(static_cast<std::size_t>(tileCols_)*tileRows_);
            if(segments_.empty()) radius_=compiled_.restRadiusPx;
            TrimHistory();
            RenderTrail();
        }

        void MoveOrigin(const CastPoint cast) noexcept
        {
            const auto glass=OriginPosition({cast.across,cast.down,0});
            if(!originSet_) {originDepth_=cast.depth;originSet_=true;}
            depthGoal_=cast.depth;
            glassZ_=glass.z;
            depthSpan_=scene::BackWallMm-glassZ_;
            origin_={glass.x,glass.y,glassZ_+originDepth_*depthSpan_};
        }

        void Reset()
        {
            targets_.clear();
            segments_.clear();
            dbfs_.clear();
            shapes_.clear();
            indices_.clear();
            std::fill(ranges_.begin(),ranges_.end(),TileRange{});
            dbfsSum_=0;
            averageDbfs_=static_cast<float>(dbRange_.bottom);
            velocity_={};
            acceleration_={};
            radius_=compiled_.restRadiusPx;
            ++version_;
        }

        void Advance(const visualizer::TrailFrame& frame)
        {
            originDepth_=std::lerp(depthGoal_,originDepth_,DepthRetained);
            origin_.z=glassZ_+originDepth_*depthSpan_;
            AddDbfs(frame.instantaneousDbfs);
            const auto light=scene::MakeLight(frame.dotXmm,frame.dotYmm,
                2*frame.dotRadiusMm,{frame.red,frame.green,frame.blue},
                frame.dotZmm);
            const auto origin=origin_;
            const auto direction=scene::Unit(light.position-origin);
            if(scene::Dot(direction,direction)>0) beamDirection_=direction;
            const auto hit=ExitFromOrb(light.position,beamDirection_);
            if(!hit.valid)
                throw std::logic_error("Laser has no room surface hit");
            targets_.push_back({hit,{light.position.x,light.position.y,
                light.position.z},origin,beamDirection_,{frame.red/100,frame.green/100,
                frame.blue/100}});
            if(targets_.size()<3) return;
            DrawSegment();
            targets_.pop_front();
        }

        void Commit()
        {
            TrimHistory();
            RenderTrail();
        }

        View CurrentView()
        {
            return {shapes_,ranges_,indices_,Scale(),
                static_cast<float>(NetWidth()),static_cast<float>(NetHeight()),
                tilePx_,compiled_.opacity,compiled_.paintEnabled,
                tileCols_,tileRows_,version_};
        }

    private:
        void AddDbfs(double dbfs)
        {
            const float sample=std::isfinite(dbfs)
                ?static_cast<float>(dbfs):static_cast<float>(dbRange_.bottom);
            dbfs_.push_back(sample);
            dbfsSum_+=sample;
            if(dbfs_.size()>visualizer::TickRate)
            {
                dbfsSum_-=dbfs_.front();
                dbfs_.pop_front();
            }
            averageDbfs_=dbfs_.empty()?static_cast<float>(dbRange_.bottom):
                dbfsSum_/static_cast<float>(dbfs_.size());
        }

        void DrawSegment()
        {
            const auto& current=targets_[0];
            const auto& next=targets_[1];
            const auto& future=targets_[2];
            curveDirection_=current.direction;
            const double halfWidth=scene::ScreenHalfChordMm();
            const double halfHeight=scene::ScreenHeightMm/2;
            const auto solved=visualizer::SolveMinJerk(
                current.target,next.target,future.target,velocity_,acceleration_,
                {-halfWidth,-halfHeight,scene::PaneThicknessMm},
                {halfWidth,halfHeight,scene::BackWallMm});
            velocity_=visualizer::Velocity(solved.controls,1);
            acceleration_=visualizer::Acceleration(solved.controls,1);
            const float move=static_cast<float>(scene::Length(
                next.wall.world-current.wall.world));
            const float response=std::clamp(move/
                static_cast<float>(scene::ScreenHeightMm),0.0f,1.0f);
            const float nextRadius=std::lerp(compiled_.restRadiusPx,
                compiled_.fullRadiusPx,response);
            Flatten(solved.controls,0,1,current.color,next.color,
                radius_,nextRadius,current.origin,next.origin,
                current.wall,next.wall);
            radius_=nextRadius;
        }

        void Flatten(const visualizer::QuinticControls& controls,
            const double from,const double to,const Color& startColor,
            const Color& endColor,const float startRadius,
            const float endRadius,const scene::Vec startOrigin,
            const scene::Vec endOrigin,const WallHit& first,const WallHit& last)
        {
            constexpr std::array<double,5> probes{0,0.25,0.5,0.75,1};
            std::array<WallHit,probes.size()> hits{};
            hits.front()=first;
            hits.back()=last;
            bool sameFace=true;
            double error{};
            for(std::size_t index=1;index+1<probes.size();++index)
            {
                const auto target=visualizer::EvalBezier(controls,probes[index]);
                const double amount=std::lerp(from,to,probes[index]);
                const auto origin=OriginAt(startOrigin,endOrigin,amount);
                const scene::Vec position{target.x,target.y,target.z};
                const auto direction=scene::Unit(position-origin);
                hits[index]=ExitFromOrb(position,scene::Dot(direction,direction)>0
                    ?direction:curveDirection_);
                if(!hits[index].valid)
                    throw std::logic_error("Laser curve has no room surface hit");
            }
            for(std::size_t index=1;index<probes.size();++index)
            {
                sameFace=sameFace && hits[index].face==hits.front().face;
            }
            for(std::size_t index=1;index+1<probes.size();++index)
            {
                const Point chord{
                    std::lerp(hits.front().point.x,hits.back().point.x,
                        static_cast<float>(probes[index])),
                    std::lerp(hits.front().point.y,hits.back().point.y,
                        static_cast<float>(probes[index]))};
                error=std::max(error,static_cast<double>(std::hypot(
                    hits[index].point.x-chord.x,
                    hits[index].point.y-chord.y)));
            }
            const double middle=std::midpoint(from,to);
            const double tolerance=
                visualizer::trail_detail::CoverageErrorMm*Scale();
            if(sameFace && (error<=tolerance ||
                middle==from || middle==to))
            {
                AppendLine(first.point,last.point,from,to,startColor,endColor,
                    startRadius,endRadius,first.face);
                return;
            }
            double spread{};
            for(const auto& hit:hits)
                spread=std::max(spread,scene::Length(hit.world-first.world));
            if(!sameFace && (spread*Scale()<=tolerance ||
                middle==from || middle==to))
            {
                for(std::size_t index=1;index<hits.size();++index)
                {
                    const auto& previous=hits[index-1];
                    const auto& next=hits[index];
                    const double low=std::lerp(from,to,probes[index-1]);
                    const double high=std::lerp(from,to,probes[index]);
                    if(previous.face==next.face)
                    {
                        AppendLine(previous.point,next.point,low,high,
                            startColor,endColor,startRadius,endRadius,
                            previous.face);
                        continue;
                    }
                    const auto seam=SharedEdge(previous,next);
                    const double crossing=std::midpoint(low,high);
                    AppendLine(previous.point,MapPoint(seam,previous.face),
                        low,crossing,startColor,endColor,startRadius,endRadius,
                        previous.face);
                    AppendLine(MapPoint(seam,next.face),next.point,
                        crossing,high,startColor,endColor,startRadius,endRadius,
                        next.face);
                }
                return;
            }
            const auto halves=visualizer::SplitBezier(controls,0.5);
            Flatten(halves[0],from,middle,startColor,endColor,
                startRadius,endRadius,startOrigin,endOrigin,first,hits[2]);
            Flatten(halves[1],middle,to,startColor,endColor,
                startRadius,endRadius,startOrigin,endOrigin,hits[2],last);
        }

        void AppendLine(const Point start,const Point end,
            const double from,const double to,const Color& startColor,
            const Color& endColor,const float startRadius,
            const float endRadius,const unsigned face)
        {
            const float length=std::hypot(end.x-start.x,end.y-start.y);
            if(!(length>0)) return;
            const auto mixColor=[&](const double amount)
            {
                return Color{
                    std::lerp(startColor.red,endColor.red,
                        static_cast<float>(amount)),
                    std::lerp(startColor.green,endColor.green,
                        static_cast<float>(amount)),
                    std::lerp(startColor.blue,endColor.blue,
                        static_cast<float>(amount))};
            };
            segments_.push_back({start,end,length,
                std::lerp(startRadius,endRadius,static_cast<float>(from)),
                std::lerp(startRadius,endRadius,static_cast<float>(to)),
                mixColor(from),mixColor(to),1u<<face});
        }

        static scene::Vec MixOrigin(const scene::Vec first,
            const scene::Vec last,const double amount)
        {
            return {std::lerp(first.x,last.x,amount),
                std::lerp(first.y,last.y,amount),
                std::lerp(first.z,last.z,amount)};
        }

        static scene::Vec SharedEdge(const WallHit& first,const WallHit& last)
        {
            auto point=MixOrigin(first.world,last.world,0.5);
            for(const unsigned face:{first.face,last.face})
            {
                if(face==0) point.z=scene::BackWallMm;
                if(face==1) point.x=-scene::ScreenHalfChordMm();
                if(face==2) point.x=scene::ScreenHalfChordMm();
                if(face==3) point.y=scene::ScreenHeightMm/2;
                if(face==4) point.y=-scene::ScreenHeightMm/2;
            }
            if(first.face==5 || last.face==5) point.z=PaneFront(point.x);
            return point;
        }

        static scene::Vec OriginAt(const scene::Vec first,
            const scene::Vec last,const double amount)
        {
            auto origin=MixOrigin(first,last,amount);
            origin.z=std::max(origin.z,PaneFront(origin.x));
            return origin;
        }

        void TrimHistory()
        {
            const float limit=std::max(compiled_.quietTrailPx,
                compiled_.loudTrailPx);
            float length{};
            for(auto it=segments_.rbegin();it!=segments_.rend();++it)
                length+=it->length;
            while(segments_.size()>1 &&
                length-segments_.front().length>=limit)
            {
                length-=segments_.front().length;
                segments_.pop_front();
            }
            if(!(limit>0)) segments_.clear();
        }

        void RenderTrail()
        {
            shapes_.clear();
            ++version_;
            const float denominator=static_cast<float>(
                dbRange_.top-dbRange_.bottom);
            const float level=denominator>0?std::clamp(
                (averageDbfs_-static_cast<float>(dbRange_.bottom))/denominator,
                0.0f,1.0f):0;
            const float span=std::lerp(compiled_.quietTrailPx,
                compiled_.loudTrailPx,level);
            if(!(span>0))
            {
                BuildTiles();
                return;
            }
            std::vector<TrailLine> visible;
            visible.reserve(segments_.size());
            float used{};
            for(auto it=segments_.rbegin();it!=segments_.rend() && used<span;++it)
            {
                TrailLine line=*it;
                if(used+line.length>span)
                {
                    const float keep=(span-used)/line.length;
                    const float cut=1-keep;
                    line.start={std::lerp(line.start.x,line.end.x,cut),
                        std::lerp(line.start.y,line.end.y,cut)};
                    line.startRadius=std::lerp(line.startRadius,
                        line.endRadius,cut);
                    line.startColor={
                        std::lerp(line.startColor.red,line.endColor.red,cut),
                        std::lerp(line.startColor.green,line.endColor.green,cut),
                        std::lerp(line.startColor.blue,line.endColor.blue,cut)};
                    line.length=std::hypot(line.end.x-line.start.x,
                        line.end.y-line.start.y);
                }
                visible.push_back(line);
                used+=line.length;
            }
            float fromTail{};
            for(auto it=visible.rbegin();it!=visible.rend();++it)
            {
                const float startOpacity=std::clamp(fromTail/used,0.0f,1.0f);
                fromTail+=it->length;
                const float endOpacity=std::clamp(fromTail/used,0.0f,1.0f);
                shapes_.push_back({
                    {it->start.x,it->start.y,it->startRadius,startOpacity},
                    {it->end.x,it->end.y,it->endRadius,endOpacity},
                    {it->startColor.red,it->startColor.green,
                        it->startColor.blue,
                        static_cast<float>(it->faceMask)},
                    {it->endColor.red,it->endColor.green,
                        it->endColor.blue,0}});
            }
            BuildTiles();
        }

        static float DepthMm() noexcept
        {
            return static_cast<float>(scene::BackWallMm-scene::FrontEdgeZmm());
        }

        static float Scale() noexcept
        {
            const double width=scene::ScreenChordMm+GlassArcMm()+
                2*DepthMm();
            const double height=scene::ScreenHeightMm+2*DepthMm();
            return static_cast<float>(std::sqrt(
                scene::RefWidthPx*scene::RefHeightPx/(width*height)));
        }

        static int NetWidth() noexcept
        {
            return static_cast<int>(std::ceil(
                (scene::ScreenChordMm+GlassArcMm()+
                    2*DepthMm())*Scale()));
        }

        static int NetHeight() noexcept
        {
            return static_cast<int>(std::ceil(
                (scene::ScreenHeightMm+2*DepthMm())*Scale()));
        }

        static double GlassArcMm() noexcept
        {
            const double radius=scene::ScreenCurveRadiusMm+
                scene::PaneThicknessMm;
            return 2*radius*std::asin(
                scene::ScreenHalfChordMm()/radius);
        }

        static double PaneFront(const double x)
        {
            const double radius=scene::ScreenCurveRadiusMm+
                scene::PaneThicknessMm;
            return std::sqrt(radius*radius-x*x)-scene::ScreenCurveRadiusMm;
        }

        WallHit ExitFromOrb(const scene::Vec orb,const scene::Vec ray)
        {
            double distance=std::numeric_limits<double>::infinity();
            unsigned face{};
            const double tolerance=64*std::numeric_limits<double>::epsilon()*
                (scene::ScreenCurveRadiusMm+scene::PaneThicknessMm);
            const auto accept=[&](const double next,const unsigned nextFace,
                const auto& inside)
            {
                if(next< -tolerance || !std::isfinite(next)) return;
                const double forward=std::max(0.0,next);
                if(!(forward<distance)) return;
                const auto point=orb+ray*forward;
                if(inside(point))
                {
                    distance=forward;
                    face=nextFace;
                }
            };
            const double halfWidth=scene::ScreenHalfChordMm();
            const double halfHeight=scene::ScreenHeightMm/2;
            if(ray.z>0) accept((scene::BackWallMm-orb.z)/ray.z,0,
                [&](const scene::Vec point)
                {
                    return std::abs(point.x)<=halfWidth+tolerance &&
                        std::abs(point.y)<=halfHeight+tolerance;
                });
            if(ray.x!=0)
            {
                const bool left=ray.x<0;
                const double next=((left?-halfWidth:halfWidth)-orb.x)/ray.x;
                accept(next,left?1u:2u,[&](const scene::Vec point)
                {
                    return std::abs(point.y)<=halfHeight+tolerance &&
                        point.z>=PaneFront(point.x)-tolerance &&
                        point.z<=scene::BackWallMm+tolerance;
                });
            }
            if(ray.y!=0)
            {
                const bool ceiling=ray.y>0;
                const double next=((ceiling?halfHeight:-halfHeight)-orb.y)/ray.y;
                accept(next,ceiling?3u:4u,[&](const scene::Vec point)
                {
                    return std::abs(point.x)<=halfWidth+tolerance &&
                        point.z>=PaneFront(point.x)-tolerance &&
                        point.z<=scene::BackWallMm+tolerance;
                });
            }
            const double radius=scene::ScreenCurveRadiusMm+
                scene::PaneThicknessMm;
            const double offset=orb.z+scene::ScreenCurveRadiusMm;
            const double a=ray.x*ray.x+ray.z*ray.z;
            const double b=2*(orb.x*ray.x+offset*ray.z);
            const double c=orb.x*orb.x+offset*offset-radius*radius;
            const double discriminant=b*b-4*a*c;
            if(a>0 && discriminant>=0)
            {
                const double q=-0.5*(b+std::copysign(
                    std::sqrt(discriminant),b));
                const std::array<double,2> roots=q==0
                    ?std::array<double,2>{-b/(2*a),-b/(2*a)}
                    :std::array<double,2>{q/a,c/q};
                for(const double next:roots)
                    accept(next,5,[&](const scene::Vec point)
                    {
                        return std::abs(point.x)<=halfWidth+tolerance &&
                            std::abs(point.y)<=halfHeight+tolerance &&
                            point.z+scene::ScreenCurveRadiusMm>=0 &&
                            ray.x*point.x+ray.z*(point.z+
                                scene::ScreenCurveRadiusMm)<=0;
                    });
            }
            if(!std::isfinite(distance)) return {};
            const auto hit=orb+ray*distance;
            return {MapPoint(hit,face),hit,face,true};
        }

        static Point MapPoint(const scene::Vec& hit,const unsigned face)
        {
            const float depth=DepthMm(),scale=Scale();
            const float width=static_cast<float>(scene::ScreenChordMm);
            const float height=static_cast<float>(scene::ScreenHeightMm);
            if(face==0) return {(depth+static_cast<float>(hit.x+
                scene::ScreenHalfChordMm()))*scale,
                (depth+height*0.5f-static_cast<float>(hit.y))*scale};
            if(face==1) return {static_cast<float>(hit.z-
                scene::FrontEdgeZmm())*scale,
                (depth+height*0.5f-static_cast<float>(hit.y))*scale};
            if(face==2) return {(depth+width+static_cast<float>(
                scene::BackWallMm-hit.z))*scale,
                (depth+height*0.5f-static_cast<float>(hit.y))*scale};
            const float x=(depth+static_cast<float>(hit.x+
                scene::ScreenHalfChordMm()))*scale;
            if(face==3) return {x,static_cast<float>(hit.z-
                scene::FrontEdgeZmm())*scale};
            if(face==4) return {x,(depth+height+static_cast<float>(
                scene::BackWallMm-hit.z))*scale};
            const double angle=std::atan2(hit.x,
                hit.z+scene::ScreenCurveRadiusMm);
            const float u=static_cast<float>(0.5+angle*
                (scene::ScreenCurveRadiusMm+scene::PaneThicknessMm)/GlassArcMm());
            return {(2*depth+width+(1-u)*
                static_cast<float>(GlassArcMm()))*scale,
                (depth+height*0.5f-static_cast<float>(hit.y))*scale};
        }

        template<class Accept>
        void ShapeTiles(const Shape& shape,const Accept& accept) const
        {
            const float padding=std::max(shape.start[2],shape.end[2])+1;
            const float left=std::min(shape.start[0],shape.end[0])-padding;
            const float right=std::max(shape.start[0],shape.end[0])+padding;
            const float top=std::min(shape.start[1],shape.end[1])-padding;
            const float bottom=std::max(shape.start[1],shape.end[1])+padding;
            const int firstX=std::clamp(static_cast<int>(std::floor(left/tilePx_)),
                0,static_cast<int>(tileCols_)-1);
            const int lastX=std::clamp(static_cast<int>(std::floor(right/tilePx_)),
                0,static_cast<int>(tileCols_)-1);
            const int firstY=std::clamp(static_cast<int>(std::floor(top/tilePx_)),
                0,static_cast<int>(tileRows_)-1);
            const int lastY=std::clamp(static_cast<int>(std::floor(bottom/tilePx_)),
                0,static_cast<int>(tileRows_)-1);
            for(int y=firstY;y<=lastY;++y)
                for(int x=firstX;x<=lastX;++x)
                    accept(static_cast<std::size_t>(y)*tileCols_+x);
        }

        void BuildTiles()
        {
            std::fill(ranges_.begin(),ranges_.end(),TileRange{});
            for(const auto& shape:shapes_)
                ShapeTiles(shape,[&](const std::size_t tile)
                {
                    ++ranges_[tile].count;
                });
            std::uint32_t total{};
            for(auto& range:ranges_)
            {
                range.first=total;
                total+=range.count;
                range.count=0;
            }
            indices_.resize(total);
            for(std::uint32_t index=0;index<shapes_.size();++index)
                ShapeTiles(shapes_[index],[&](const std::size_t tile)
                {
                    auto& range=ranges_[tile];
                    indices_[range.first+range.count++]=index;
                });
        }

        Settings settings_{};
        Compiled compiled_{};
        scene::Vec origin_{OriginPosition(CastPoint{})};
        inline static const double DepthRetained=
            std::exp2(-1/(0.25*visualizer::TickRate));
        double originDepth_{},depthGoal_{},glassZ_{},depthSpan_{};
        bool originSet_{};
        scene::Vec beamDirection_{0,0,1},curveDirection_{0,0,1};
        visualizer::VerticalDbfsRange dbRange_{};
        std::deque<PaintHit> targets_;
        std::deque<TrailLine> segments_;
        std::deque<float> dbfs_;
        visualizer::MotionPoint velocity_{},acceleration_{};
        std::vector<Shape> shapes_;
        std::vector<TileRange> ranges_;
        std::vector<std::uint32_t> indices_;
        float dbfsSum_{};
        float averageDbfs_{-100};
        float radius_{};
        float tilePx_{1};
        std::uint32_t tileCols_{},tileRows_{};
        std::uint64_t version_{};
    };
}
