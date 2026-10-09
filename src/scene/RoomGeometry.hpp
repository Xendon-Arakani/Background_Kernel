#pragma once

#include "render/HdrToneMapper.hpp"
#include "scene/PhysicalUnits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace bk::scene
{
    inline constexpr double Pi = std::numbers::pi;
    inline constexpr float Roughness = 0.92f, BaseReflectance = 0.04f, NormalStrength = 0.10f;
    inline constexpr std::array<double, 3> Albedo{0.68, 0.65, 0.60};
    inline constexpr double AlbedoVariation = 0.03;

    struct Vec
    {
        double x{}, y{}, z{};
        Vec operator+(Vec b) const { return {x+b.x,y+b.y,z+b.z}; }
        Vec operator-(Vec b) const { return {x-b.x,y-b.y,z-b.z}; }
        Vec operator*(double k) const { return {x*k,y*k,z*k}; }
    };
    inline double Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    inline Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
    inline double Length(Vec a) { return std::sqrt(Dot(a,a)); }
    inline Vec Unit(Vec a) { const auto n=Length(a); return n>0 ? a*(1/n) : Vec{}; }
    inline double ScreenHalfChordMm() { return ScreenCurveRadiusMm*std::sin(ScreenArcWidthMm/(2*ScreenCurveRadiusMm)); }
    inline double FrontEdgeZmm() { return ScreenCurveRadiusMm*(std::cos(ScreenArcWidthMm/(2*ScreenCurveRadiusMm))-1); }
    inline bool ValidEyeDistanceMm(double millimeters)
    {
        return std::isfinite(millimeters) && millimeters > -FrontEdgeZmm() &&
            millimeters < std::sqrt(static_cast<double>(std::numeric_limits<float>::max()))/4;
    }
    inline Vec GlassPoint(double u, double v)
    {
        const auto angle=(u-0.5)*ScreenArcWidthMm/ScreenCurveRadiusMm;
        return {ScreenCurveRadiusMm*std::sin(angle), (0.5-v)*ScreenHeightMm,
                ScreenCurveRadiusMm*(std::cos(angle)-1)};
    }
    struct EyePose
    {
        Vec position{0,0,-BaseEyeMm};
    };
    inline EyePose EyeFromHeadPose(
        Vec headInScreen,
        std::array<double,4> headRotation,
        Vec eyeFromHead)
    {
        const double length=std::sqrt(
            headRotation[0]*headRotation[0]+headRotation[1]*headRotation[1]+
            headRotation[2]*headRotation[2]+headRotation[3]*headRotation[3]);
        if (!(length>0) || !std::isfinite(length)) throw std::invalid_argument("Invalid head orientation");
        for(auto& component:headRotation) component/=length;
        const Vec rotationAxis{
            headRotation[0],headRotation[1],headRotation[2]};
        const auto doubledCross=Cross(rotationAxis,eyeFromHead)*2;
        return {headInScreen+eyeFromHead+
            doubledCross*headRotation[3]+Cross(rotationAxis,doubledCross)};
    }
    struct Projection { double x{}, y{}; };
    inline Projection Project(Vec point, EyePose eye, double width, double height)
    {
        const auto eyeRay=point-eye.position;
        const auto eyeFromCenter=eye.position+Vec{0,0,ScreenCurveRadiusMm};
        const double rayPlanarSq=eyeRay.x*eyeRay.x+eyeRay.z*eyeRay.z;
        const double centerRayDot=
            eyeFromCenter.x*eyeRay.x+eyeFromCenter.z*eyeRay.z;
        const double centerDelta=
            eyeFromCenter.x*eyeFromCenter.x+eyeFromCenter.z*eyeFromCenter.z-
            ScreenCurveRadiusMm*ScreenCurveRadiusMm;
        const double discriminant=
            centerRayDot*centerRayDot-rayPlanarSq*centerDelta;
        if (!(rayPlanarSq>0) || discriminant<0)
            throw std::logic_error("Point cannot project onto calibrated glass");
        const double paneScale=
            (-centerRayDot+std::sqrt(discriminant))/rayPlanarSq;
        const auto panePoint=eye.position+eyeRay*paneScale;
        return {(0.5+ScreenCurveRadiusMm*std::atan2(
                    panePoint.x,panePoint.z+ScreenCurveRadiusMm)/ScreenArcWidthMm)*width,
                (0.5-panePoint.y/ScreenHeightMm)*height};
    }
    inline double DistanceToRay(Vec point, Vec origin, Vec ray)
    {
        const auto offset=point-origin;
        return Length(offset-ray*Dot(offset,ray));
    }
    struct Light
    {
        Vec position{};
        double radius{}, candela{};
        std::array<double,3> emissionNits{}, intensityRgb{};
    };
    inline Light MakeLight(double xMm, double yMm, double diameterMm, std::array<double,3> rgb, double zMm=BaseLightZmm)
    {
        Light light;
        light.radius=std::clamp(diameterMm,CornerMinTubeMm,FitMaxTubeMm)/2;
        const double controlInset=CornerMaxTubeMm/2;
        const double horizontal=std::clamp(
            (xMm-controlInset)/(ScreenChordMm-2*controlInset),0.0,1.0);
        const double vertical=std::clamp(
            (yMm-controlInset)/(ScreenHeightMm-2*controlInset),0.0,1.0);
        const double depth=std::clamp(
            (zMm-controlInset)/(BackWallMm-2*controlInset),0.0,1.0);
        light.position={
            std::lerp(-ScreenHalfChordMm()+light.radius,
                ScreenHalfChordMm()-light.radius,horizontal),
            std::lerp(ScreenHeightMm/2-light.radius,
                -ScreenHeightMm/2+light.radius,vertical),
            std::lerp(PaneThicknessMm+light.radius,
                BackWallMm-light.radius,depth)};
        for(int channel=0;channel<3;++channel)
        {
            light.emissionNits[channel]=render::hdr::SceneValueToNits(rgb[channel]);
            light.intensityRgb[channel]=Pi*light.radius*light.radius/
                SqMmPerSqM*light.emissionNits[channel];
        }
        light.candela=0.2627*light.intensityRgb[0]+0.6780*light.intensityRgb[1]+0.0593*light.intensityRgb[2];
        return light;
    }
    inline double MaterialNoise(Vec p)
    {
        const auto cellX=static_cast<std::int32_t>(std::floor(p.x));
        const auto cellY=static_cast<std::int32_t>(std::floor(p.y));
        const auto cellZ=static_cast<std::int32_t>(std::floor(p.z));
        const auto smooth=[](double fraction) {
            return fraction*fraction*(3-2*fraction);
        };
        const double blendX=smooth(p.x-cellX);
        const double blendY=smooth(p.y-cellY);
        const double blendZ=smooth(p.z-cellZ);
        const auto hash=[](
            std::int32_t cellX,std::int32_t cellY,std::int32_t cellZ) {
            std::uint32_t hash=static_cast<std::uint32_t>(cellX)*0x8da6b343u ^
                static_cast<std::uint32_t>(cellY)*0xd8163841u ^
                static_cast<std::uint32_t>(cellZ)*0xcb1ab31fu;
            hash^=hash>>16;hash*=0x7feb352du;
            hash^=hash>>15;hash*=0x846ca68bu;hash^=hash>>16;
            return static_cast<double>(hash>>8)/16777215.0;
        };
        return std::lerp(
            std::lerp(
                std::lerp(hash(cellX,cellY,cellZ),hash(cellX+1,cellY,cellZ),blendX),
                std::lerp(hash(cellX,cellY+1,cellZ),hash(cellX+1,cellY+1,cellZ),blendX),blendY),
            std::lerp(
                std::lerp(hash(cellX,cellY,cellZ+1),hash(cellX+1,cellY,cellZ+1),blendX),
                std::lerp(hash(cellX,cellY+1,cellZ+1),hash(cellX+1,cellY+1,cellZ+1),blendX),blendY),
            blendZ);
    }
    inline double RaySphere(Vec eye, Vec ray, const Light& light)
    {
        const auto delta=light.position-eye;
        const double along=Dot(delta,ray);
        const auto perpendicular=Cross(delta,ray);
        const double hitDepthSq=
            light.radius*light.radius-Dot(perpendicular,perpendicular);
        if (hitDepthSq<0) return std::numeric_limits<double>::infinity();
        const double distance=along-std::sqrt(hitDepthSq);
        return distance>0 ? distance : std::numeric_limits<double>::infinity();
    }
}
