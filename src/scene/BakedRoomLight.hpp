#pragma once
#include "scene/RoomGeometry.hpp"
#include <filesystem>
#include <vector>

namespace bk::scene
{
    inline constexpr std::uint32_t PatchSide=16, FaceCount=5, PatchCount=FaceCount*PatchSide*PatchSide;
    inline constexpr std::uint32_t LightingSide=4*PatchSide, LightingCount=FaceCount*LightingSide*LightingSide;
    struct WallSample { Vec center,normal; std::array<double,3> reflectance; };
    inline std::array<double,4> SplineWeights(double fraction) {
        const double complement=1-fraction;
        return {
            complement*complement*complement/6,
            (3*fraction*fraction*fraction-6*fraction*fraction+4)/6,
            (-3*fraction*fraction*fraction+3*fraction*fraction+
                3*fraction+1)/6,
            fraction*fraction*fraction/6};
    }
    struct Patch { Vec center,normal,u,v; double area; std::array<double,3> reflectance; };
    struct BakedRoomLight
    {
        std::vector<Patch> patches;
        std::vector<double> formFactors;
        std::vector<std::array<float,4>> bounceResponse;
        std::vector<WallSample> wallSamples;
        std::vector<float> pointFactors;
        double reciprocityError{}, maxRowSum{}, energyScale{1};
    };
    BakedRoomLight GenerateTransport();
    void SaveTransport(
        const BakedRoomLight& transport,
        const std::filesystem::path& path);
    const BakedRoomLight& BuildTransport();
}
