#pragma once

#include <algorithm>
#include <cmath>

namespace bk::render::hdr
{
    constexpr double SourcePeakNits = 10000.0;
    constexpr double ScRgbWhiteNits = 80.0;
    constexpr double BasePaperWhite = 200.0;
    constexpr double SceneChannelMax = 100.0;

    inline bool ValidPaperWhiteNits(const double nits) noexcept
    {
        return std::isfinite(nits) && nits > 0.0 && nits <= SourcePeakNits;
    }

    constexpr double SceneValueToNits(
        const double channel) noexcept
    {
        return std::clamp(channel, 0.0, SceneChannelMax) *
            (SourcePeakNits / SceneChannelMax);
    }

    inline double PqEncodeNits(const double nits) noexcept
    {
        constexpr double lumPower = 2610.0 / 16384.0;
        constexpr double codePower = 2523.0 / 32.0;
        constexpr double codeOffset = 3424.0 / 4096.0;
        constexpr double codeScale = 2413.0 / 128.0;
        constexpr double codeSlope = 2392.0 / 128.0;
        const double normalized = std::clamp(nits / SourcePeakNits, 0.0, 1.0);
        const double power = std::pow(normalized, lumPower);
        return std::pow(
            (codeOffset + codeScale * power) / (1.0 + codeSlope * power),
            codePower);
    }

    inline double PqDecodeNits(const double signal) noexcept
    {
        constexpr double lumPower = 2610.0 / 16384.0;
        constexpr double codePower = 2523.0 / 32.0;
        constexpr double codeOffset = 3424.0 / 4096.0;
        constexpr double codeScale = 2413.0 / 128.0;
        constexpr double codeSlope = 2392.0 / 128.0;
        const double power = std::pow(
            std::clamp(signal, 0.0, 1.0), 1.0 / codePower);
        const double numerator = std::max(power - codeOffset, 0.0);
        const double denominator = std::max(
            codeScale - codeSlope * power, 1.0e-12);
        return SourcePeakNits *
            std::pow(numerator / denominator, 1.0 / lumPower);
    }

    inline double EetfNits(
        const double sourceNits,
        const double targetMinNits,
        const double targetMaxNits) noexcept
    {
        const double safeMax = std::clamp(
            targetMaxNits, 1.0, SourcePeakNits);
        const double safeMin = std::clamp(targetMinNits, 0.0, safeMax);
        const double sourceBlack = PqEncodeNits(0.0);
        const double sourceWhite = PqEncodeNits(SourcePeakNits);
        const double sourceRange = sourceWhite - sourceBlack;
        const auto normalize = [=](const double pq) noexcept
        {
            return (pq - sourceBlack) / sourceRange;
        };
        const double minLum = normalize(PqEncodeNits(safeMin));
        const double maxLum = normalize(PqEncodeNits(safeMax));
        const double input = std::clamp(
            normalize(PqEncodeNits(sourceNits)), 0.0, 1.0);

        double mapped = input;
        if (maxLum < 1.0 - 1.0e-9)
        {
            const double kneeStart = std::clamp(
                1.5 * maxLum - 0.5, 0.0, 1.0 - 1.0e-9);
            if (input >= kneeStart)
            {
                const double t = std::clamp(
                    (input - kneeStart) / (1.0 - kneeStart), 0.0, 1.0);
                const double square = t * t;
                const double cube = square * t;
                mapped =
                    (2.0 * cube - 3.0 * square + 1.0) * kneeStart +
                    (cube - 2.0 * square + t) * (1.0 - kneeStart) +
                    (-2.0 * cube + 3.0 * square) * maxLum;
            }
        }

        mapped += minLum * std::pow(1.0 - mapped, 4.0);
        mapped = std::clamp(mapped, minLum, maxLum);
        return PqDecodeNits(mapped * sourceRange + sourceBlack);
    }
}
