#pragma once
#include "scene/BakedRoomLight.hpp"
#include "scene/LineLight.hpp"

namespace bk::scene
{
    inline double TriangleSolidAngle(Vec firstRay,Vec nextRay,Vec lastRay)
    {
        return 2*std::atan2(
            std::abs(Dot(firstRay,Cross(nextRay,lastRay))),
            1+Dot(firstRay,nextRay)+Dot(nextRay,lastRay)+
                Dot(lastRay,firstRay));
    }
    inline double PatchSolidAngle(const Patch& patch,Vec source)
    {
        const auto lowerLeft=Unit(patch.center-patch.u-patch.v-source);
        const auto lowerRight=Unit(patch.center+patch.u-patch.v-source);
        const auto upperRight=Unit(patch.center+patch.u+patch.v-source);
        const auto upperLeft=Unit(patch.center-patch.u+patch.v-source);
        return TriangleSolidAngle(lowerLeft,lowerRight,upperRight)+
            TriangleSolidAngle(lowerLeft,upperRight,upperLeft);
    }
    inline std::array<double,3> TubeSideFlux(const LineLight& line)
    {
        std::array<double,3> flux{};
        for(int channel=0;channel<3;++channel)
            flux[channel]=Pi*Pi*Length(line.endPoint-line.startPoint)*
                (line.startNitMm[channel]+line.endNitMm[channel])/1e6;
        return flux;
    }
    inline double CommonFluxScale(const LineLight& line,std::span<const Patch> patches)
    {
        std::array<double,3> captured{};const auto emitted=TubeSideFlux(line);
        for(const auto& patch:patches) {
            const auto e=LineIrradiance(line,patch.center,patch.normal);
            for(int channel=0;channel<3;++channel)
                captured[channel]+=e[channel]*patch.area/1e6;
        }
        double scale=1;
        for(int channel=0;channel<3;++channel)
            if(captured[channel]>0)
                scale=std::min(scale,emitted[channel]/captured[channel]);
        return scale;
    }
}
