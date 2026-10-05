#pragma once
#include "scene/RoomGeometry.hpp"

namespace bk::scene
{
    struct Refraction { Vec direction{}; double reflectance{1}; bool transmitted{}; };
    inline Refraction RefractInterface(
        Vec incoming,Vec incidentNormal,double sourceIor,double targetIor)
    {
        const double incidentCos=std::clamp(
            -Dot(incoming,incidentNormal),0.0,1.0);
        const double iorRatio=sourceIor/targetIor;
        const double transmitSinSq=iorRatio*iorRatio*
            std::max(0.0,1-incidentCos*incidentCos);
        if(transmitSinSq>=1) return {};
        const double transmitCos=std::sqrt(1-transmitSinSq);
        const double sReflect=
            (sourceIor*incidentCos-targetIor*transmitCos)/
            (sourceIor*incidentCos+targetIor*transmitCos);
        const double pReflect=
            (targetIor*incidentCos-sourceIor*transmitCos)/
            (targetIor*incidentCos+sourceIor*transmitCos);
        return {
            Unit(incoming*iorRatio+incidentNormal*
                (iorRatio*incidentCos-transmitCos)),
            (sReflect*sReflect+pReflect*pReflect)/2,true};
    }
    inline double PaneCylinderExit(Vec origin,Vec ray,double radius)
    {
        const Vec centerOffset=origin+Vec{0,0,ScreenCurveRadiusMm};
        const double rayPlanarSq=ray.x*ray.x+ray.z*ray.z;
        const double offsetRayDot=
            centerOffset.x*ray.x+centerOffset.z*ray.z;
        const double radiusDelta=centerOffset.x*centerOffset.x+
            centerOffset.z*centerOffset.z-radius*radius;
        return -radiusDelta/(offsetRayDot+std::sqrt(
            offsetRayDot*offsetRayDot-rayPlanarSq*radiusDelta));
    }
    struct PaneRay { Vec origin{},direction{}; double transmission{},pathMm{}; };
    inline PaneRay SmoothPaneRay(Vec front,Vec incoming,double thickness=PaneThicknessMm,double ior=PaneRefractiveIndex)
    {
        const Vec radial=Unit({front.x,0,front.z+ScreenCurveRadiusMm});
        const auto entry=RefractInterface(incoming,radial*(-1),1,ior);
        if(!entry.transmitted) return {};
        const double distance=PaneCylinderExit(front,entry.direction,ScreenCurveRadiusMm+thickness);
        const auto back=front+entry.direction*distance;
        const Vec normal=Unit({back.x,0,back.z+ScreenCurveRadiusMm});
        const auto exit=RefractInterface(entry.direction,normal*(-1),ior,1);
        if(!exit.transmitted) return {};
        return {back,exit.direction,(1-entry.reflectance)*(1-exit.reflectance)*std::exp(-PaneAbsorptionPerMm*distance),distance};
    }
}
