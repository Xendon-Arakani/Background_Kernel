#pragma once
#include <algorithm>
#include <cmath>

namespace bk::scene
{
    inline constexpr double MillimetersPerInch=25.4;
    inline constexpr double SqMmPerSqM=1'000'000.0;
    inline constexpr double ScreenArcWidthMm=1196.7;
    inline constexpr double ScreenHeightMm=339.2;
    inline constexpr double ScreenCurveRadiusMm=1800.0;
    inline const double ScreenChordMm=2*ScreenCurveRadiusMm*std::sin(ScreenArcWidthMm/(2*ScreenCurveRadiusMm));
    inline constexpr double BackWallMm=304.8;
    inline constexpr double PaneThicknessMm=12.0;
    inline constexpr double PaneRefractiveIndex=1.49;
    inline constexpr double PaneAbsorptionPerMm=0.0001;
    inline constexpr double BaseLightZmm=152.4;
    inline constexpr double BaseEyeMm=609.6;
    inline constexpr double RefWidthPx=5120,RefHeightPx=1440;
    inline constexpr double CornerMinTubeMm=0.3746279093570406;
    inline constexpr double CornerMaxTubeMm=3.738947867346764;
    inline constexpr double CornerMinRefPx=1.0;
    inline constexpr double BaseMaxRefPx=10.0;
    inline constexpr double FitMaxTubeMm=
        std::min(ScreenHeightMm,BackWallMm-PaneThicknessMm);

    inline double TubeMmForRefPx(const double pixels) noexcept
    {
        return CornerMinTubeMm+
            (pixels-CornerMinRefPx)*
            (CornerMaxTubeMm-CornerMinTubeMm)/
            (BaseMaxRefPx-CornerMinRefPx);
    }

    inline bool ValidMaxRefPx(const double pixels) noexcept
    {
        if(!std::isfinite(pixels) || pixels<CornerMinRefPx) return false;
        const double diameter=TubeMmForRefPx(pixels);
        return std::isfinite(diameter) && diameter<=FitMaxTubeMm;
    }
}
