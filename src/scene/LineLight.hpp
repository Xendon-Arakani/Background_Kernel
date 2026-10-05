#pragma once
#include "scene/RoomGeometry.hpp"
#include "visualizer/AdaptiveTrail.hpp"
#include <span>
#include <vector>

namespace bk::scene
{
    struct LineLight
    {
        Vec startPoint{},endPoint{};
        std::array<double,3> startNitMm{},endNitMm{};
        double startRadius{},endRadius{};
        std::array<double,3> startNits{},endNits{};
        double startOpacity{1},endOpacity{1};
        double startEmitScale{1},endEmitScale{1};
    };

    inline std::vector<LineLight> BuildLineLights(
        std::span<const visualizer::TrailPoint> trail,
        std::span<const visualizer::TrailPoint> sweep,
        double opacity=1)
    {
        opacity=std::clamp(opacity,0.0,1.0);
        std::vector<LineLight> lines;
        lines.reserve(trail.size()+sweep.size());
        const auto makePoint=[&](const visualizer::TrailPoint& point) {
            const auto light=MakeLight(
                point.x,point.y,2*point.radius,
                {point.red,point.green,point.blue},point.z);
            std::array<double,3> emission;
            for(int channel=0;channel<3;++channel)
                emission[channel]=light.radius*light.emissionNits[channel];
            return LineLight{
                light.position,light.position,emission,emission,
                light.radius,light.radius,
                light.emissionNits,light.emissionNits,
                std::clamp<double>(point.opacity,0,1),
                std::clamp<double>(point.opacity,0,1)};
        };
        const auto appendPath=[&](
            std::span<const visualizer::TrailPoint> path,
            double endTick,bool historical) {
            if(path.size()<2) return;
            std::vector<LineLight> pointLights;
            pointLights.reserve(path.size());
            for(std::size_t pointIndex=0;pointIndex<path.size();++pointIndex) {
                auto point=path[pointIndex];
                if(historical) point.opacity=1;
                pointLights.push_back(makePoint(point));
            }
            const auto firstLine=lines.size();
            auto startLight=pointLights.front();
            for(std::size_t pointIndex=1;pointIndex<path.size();++pointIndex)
            {
                const auto& startPoint=path[pointIndex-1];
                const auto& endPoint=path[pointIndex];
                if(startPoint.tick>=endTick) break;
                auto endLight=pointLights[pointIndex];
                if(endPoint.tick>startPoint.tick)
                {
                    LineLight line{
                        startLight.startPoint,endLight.startPoint,
                        startLight.startNitMm,endLight.startNitMm,
                        startLight.startRadius,endLight.startRadius,
                        startLight.startNits,endLight.startNits,
                        startLight.startOpacity,endLight.startOpacity};
                    if(endPoint.tick>endTick)
                    {
                        const double tickRatio=(endTick-startPoint.tick)/
                            (endPoint.tick-startPoint.tick);
                        line.endPoint=line.startPoint+
                            (line.endPoint-line.startPoint)*tickRatio;
                        line.endRadius=std::lerp(
                            line.startRadius,line.endRadius,tickRatio);
                        for(int channel=0;channel<3;++channel)
                            line.endNits[channel]=std::lerp(
                                line.startNits[channel],
                                line.endNits[channel],tickRatio);
                        line.endOpacity=std::lerp(
                            line.startOpacity,line.endOpacity,tickRatio);
                    }
                    if(historical && endPoint.tick>=endTick && sweep.size()>1) {
                        const auto sweepSeam=makePoint(sweep.front());
                        line.endPoint=sweepSeam.startPoint;
                        line.endRadius=sweepSeam.startRadius;
                        line.endNits=sweepSeam.startNits;
                    }
                    lines.push_back(line);
                }
                startLight=endLight;
            }
            double totalDistance=0;
            if(historical)
                for(auto index=firstLine;index<lines.size();++index)
                    totalDistance+=Length(lines[index].endPoint-lines[index].startPoint);
            double fromTail=0;
            for(auto index=firstLine;index<lines.size();++index) {
                auto& line=lines[index];
                if(historical) {
                    line.startEmitScale=visualizer::TrailAlphaAtDist(fromTail,totalDistance);
                    fromTail+=Length(line.endPoint-line.startPoint);
                    line.endEmitScale=visualizer::TrailAlphaAtDist(fromTail,totalDistance);
                    line.startOpacity=line.startEmitScale;
                    line.endOpacity=line.endEmitScale;
                }
                line.startOpacity*=opacity;
                line.endOpacity*=opacity;
                for(int channel=0;channel<3;++channel) {
                    line.startNitMm[channel]=line.startRadius*
                        line.startNits[channel]*line.startEmitScale*
                        line.startOpacity;
                    line.endNitMm[channel]=line.endRadius*
                        line.endNits[channel]*line.endEmitScale*
                        line.endOpacity;
                }
            }
        };
        appendPath(trail,sweep.size()>1
            ? sweep.front().tick:std::numeric_limits<double>::infinity(),true);
        appendPath(sweep,std::numeric_limits<double>::infinity(),false);
        return lines;
    }

    template<class T> T StableLineMoment(T angle,T sineCosine)
    {
        if(angle>=T(1)) return angle-sineCosine;
        T term=T(2.0/3.0)*angle*angle*angle,sum=term;
        for(unsigned order=1;;++order)
        {
            term*=(-T(4)*angle*angle)/
                (T(2*order+2)*T(2*order+3));
            const T next=sum+term;
            if(next==sum) return sum;
            sum=next;
        }
    }

    inline std::array<double,3> LineIrradiance(const LineLight& input,Vec point,Vec normal)
    {
        auto line=input;
        double startHeight=Dot(line.startPoint-point,normal);
        double endHeight=Dot(line.endPoint-point,normal);
        if(startHeight<=0 && endHeight<=0) return {};
        if(startHeight<0 || endHeight<0)
        {
            const double planeRatio=startHeight/(startHeight-endHeight);
            const Vec intersection=line.startPoint+
                (line.endPoint-line.startPoint)*planeRatio;
            std::array<double,3> emission;
            for(int channel=0;channel<3;++channel)
                emission[channel]=std::lerp(
                    line.startNitMm[channel],line.endNitMm[channel],planeRatio);
            if(startHeight<0) {
                line.startPoint=intersection;line.startNitMm=emission;
            }
            else {
                line.endPoint=intersection;line.endNitMm=emission;
            }
        }
        const double length=Length(line.endPoint-line.startPoint);
        if(!(length>0)) return {};
        const auto tangent=(line.endPoint-line.startPoint)*(1/length);
        const auto pointOffset=line.startPoint-point;
        const double startAlong=Dot(pointOffset,tangent);
        const double endAlong=startAlong+length;
        const double middleAlong=(startAlong+endAlong)/2;
        const auto perpendicular=pointOffset-tangent*startAlong;
        const double normalDistSq=Dot(perpendicular,perpendicular);
        const double normalDistance=std::sqrt(normalDistSq);
        if(!(normalDistance>0)) return {};
        const double angleNumerator=normalDistance*length;
        const double angleBase=normalDistSq+startAlong*endAlong;
        const double denominator=
            angleNumerator*angleNumerator+angleBase*angleBase;
        const double angle=std::atan2(angleNumerator,angleBase);
        const double scale=angleNumerator/denominator;
        const double normalAcross=Dot(normal,perpendicular);
        const double normalAlong=Dot(normal,tangent);
        const double totalWeight=std::max(0.0,
            normalAcross*(angle+scale*
                (normalDistSq-startAlong*endAlong))/normalDistSq+
            normalAlong*scale*(startAlong+endAlong));
        const double delta=StableLineMoment(angle,scale*angleBase);
        const double moment=(normalAlong-
            normalAcross*middleAlong/normalDistSq)*delta-
            normalAlong*scale*(length*length/2);
        const double endWeight=std::clamp(
            totalWeight/2+moment/length,0.0,totalWeight);
        const double startWeight=totalWeight-endWeight;
        std::array<double,3> result;
        for(int channel=0;channel<3;++channel)
            result[channel]=line.startNitMm[channel]*startWeight+
                line.endNitMm[channel]*endWeight;
        return result;
    }
}
