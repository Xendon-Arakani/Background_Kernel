#pragma once
#include "scene/LineLight.hpp"

namespace bk::scene
{
    inline double RayTube(Vec origin,Vec ray,const LineLight& line,double& parameter)
    {
        Light endpoint;endpoint.position=line.startPoint;endpoint.radius=line.startRadius;
        double hit=RaySphere(origin,ray,endpoint);parameter=0;
        endpoint.position=line.endPoint;endpoint.radius=line.endRadius;
        const auto last=RaySphere(origin,ray,endpoint);
        if(last<hit) { hit=last;parameter=1; }
        const auto delta=line.endPoint-line.startPoint;
        const double length=Length(delta);
        const double radiusDelta=line.endRadius-line.startRadius;
        if(!(length>std::abs(radiusDelta))) return hit;
        const auto tangent=delta*(1/length);
        const double shift=Dot(line.startPoint-origin,ray);
        const auto rayOffset=Cross(ray,Cross(origin-line.startPoint,ray));
        const double axialOffset=Dot(rayOffset,tangent);
        const double axialRate=Dot(ray,tangent);
        const double radiusRate=radiusDelta/length;
        const double slopeGap=1-radiusRate*radiusRate;
        const auto perpendicular=rayOffset-tangent*axialOffset;
        const auto perpRay=ray-tangent*axialRate;
        const double radius=line.startRadius+radiusRate*axialOffset;
        const double radiusDrift=radiusRate*axialRate;
        const double quadratic=Dot(perpRay,perpRay)-
            radiusDrift*radiusDrift/slopeGap;
        const double linear=Dot(perpendicular,perpRay)-
            radius*radiusDrift/slopeGap;
        const double constant=Dot(perpendicular,perpendicular)-
            radius*radius/slopeGap;
        const auto accept=[&](double rayDistance) {
            const double tubeRatio=
                (axialOffset+rayDistance*axialRate+
                    line.startRadius*radiusRate)/(length*slopeGap);
            const double distance=rayDistance+shift;
            if(tubeRatio>=0&&tubeRatio<=1&&distance>0&&distance<hit) {
                hit=distance;parameter=tubeRatio;
            }
        };
        if(quadratic==0) {
            if(linear!=0) accept(-constant/(2*linear));
        }
        else {
            const double discriminant=
                linear*linear-quadratic*constant;
            if(discriminant>=0) {
                const double root=std::sqrt(discriminant);
                accept((-linear-root)/quadratic);
                accept((-linear+root)/quadratic);
            }
        }
        return hit;
    }

    inline std::array<float,4> ProjectTubeBounds(const LineLight& line,EyePose eye,double width,double height)
    {
        const Vec lo{std::min(line.startPoint.x-line.startRadius,line.endPoint.x-line.endRadius),
            std::min(line.startPoint.y-line.startRadius,line.endPoint.y-line.endRadius),std::min(line.startPoint.z-line.startRadius,line.endPoint.z-line.endRadius)};
        const Vec hi{std::max(line.startPoint.x+line.startRadius,line.endPoint.x+line.endRadius),
            std::max(line.startPoint.y+line.startRadius,line.endPoint.y+line.endRadius),std::max(line.startPoint.z+line.startRadius,line.endPoint.z+line.endRadius)};
        const double frontScale=
            (FrontEdgeZmm()-eye.position.z)/(hi.z-eye.position.z);
        const double backScale=
            -eye.position.z/(lo.z-eye.position.z);
        double minX=std::numeric_limits<double>::infinity();
        double maxX=-minX,minY=minX,maxY=-minX;
        for(double depthScale:{frontScale,backScale}) {
            for(double x:{lo.x,hi.x}) {
                const double projected=
                    eye.position.x+depthScale*(x-eye.position.x);
                minX=std::min(minX,projected);
                maxX=std::max(maxX,projected);
            }
            for(double y:{lo.y,hi.y}) {
                const double projected=
                    eye.position.y+depthScale*(y-eye.position.y);
                minY=std::min(minY,projected);
                maxY=std::max(maxY,projected);
            }
        }
        const auto pixelX=[&](double x) { return width*(0.5+ScreenCurveRadiusMm*std::asin(std::clamp(x/ScreenCurveRadiusMm,-1.0,1.0))/ScreenArcWidthMm); };
        return {
            static_cast<float>(std::clamp(pixelX(minX)-1,0.0,width)),
            static_cast<float>(std::clamp(
                height*(0.5-maxY/ScreenHeightMm)-1,0.0,height)),
            static_cast<float>(std::clamp(pixelX(maxX)+1,0.0,width)),
            static_cast<float>(std::clamp(
                height*(0.5-minY/ScreenHeightMm)+1,0.0,height))};
    }
}
