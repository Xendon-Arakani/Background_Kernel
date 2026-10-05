#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace bk::visualizer
{
    struct MotionPoint
    {
        double x{}, y{}, z{};
        MotionPoint operator+(const MotionPoint b) const { return {x + b.x, y + b.y, z + b.z}; }
        MotionPoint operator-(const MotionPoint b) const { return {x - b.x, y - b.y, z - b.z}; }
        MotionPoint operator*(const double s) const { return {x * s, y * s, z * s}; }
    };

    inline double Length(MotionPoint p) { return std::hypot(p.x,p.y,p.z); }
    inline double Dot(MotionPoint a,MotionPoint b) { return a.x*b.x+a.y*b.y+a.z*b.z; }

    using QuinticControls = std::array<MotionPoint, 6>;

    inline MotionPoint Mix(const MotionPoint a, const MotionPoint b, const double u)
    {
        return {std::lerp(a.x, b.x, u), std::lerp(a.y, b.y, u), std::lerp(a.z, b.z, u)};
    }

    inline MotionPoint EvalBezier(const QuinticControls& controls, const double u)
    {
        auto points = controls;
        for (std::size_t n = points.size() - 1; n > 0; --n)
            for (std::size_t i = 0; i < n; ++i)
                points[i] = Mix(points[i], points[i + 1], u);
        return points[0];
    }

    inline MotionPoint Velocity(const QuinticControls& controls, const double u)
    {
        std::array<MotionPoint, 5> points{};
        for (std::size_t i = 0; i < points.size(); ++i)
            points[i] = (controls[i + 1] - controls[i]) * 5.0;
        for (std::size_t n = points.size() - 1; n > 0; --n)
            for (std::size_t i = 0; i < n; ++i)
                points[i] = Mix(points[i], points[i + 1], u);
        return points[0];
    }

    inline MotionPoint Acceleration(const QuinticControls& controls, const double u)
    {
        std::array<MotionPoint, 4> points{};
        for (std::size_t i = 0; i < points.size(); ++i)
            points[i] = (controls[i + 2] - controls[i + 1] * 2.0 + controls[i]) * 20.0;
        for (std::size_t n = points.size() - 1; n > 0; --n)
            for (std::size_t i = 0; i < n; ++i)
                points[i] = Mix(points[i], points[i + 1], u);
        return points[0];
    }

    inline std::array<QuinticControls, 2> SplitBezier(const QuinticControls& controls, const double u)
    {
        std::array<QuinticControls, 2> result{};
        auto work = controls;
        result[0][0] = work[0];
        result[1][5] = work[5];
        for (std::size_t row = 1; row < 6; ++row)
        {
            for (std::size_t i = 0; i < 6 - row; ++i)
                work[i] = Mix(work[i], work[i + 1], u);
            result[0][row] = work[0];
            result[1][5 - row] = work[5 - row];
        }
        return result;
    }

    struct MotionSolve
    {
        QuinticControls controls{};
        bool hullConstrained{};
    };

    namespace jerk_detail
    {
        using SolveVector = std::array<double, 4>;
        struct AxisSolve
        {
            std::array<double, 6> controls{};
            bool constrained{};
        };

        inline constexpr double JerkHessian[4][4]{
            {60, -120, -10, 0}, {-120, 320, 40, -5},
            {-10, 40, 30, -30}, {0, -5, -30, 40}};
        inline constexpr double JerkMap[6][4]{
            {1, 0, 0, 0}, {-3, 1, 0, 0}, {3, -3, 0, 0},
            {-3, 9, 1, 0}, {3, -11, -3, 1}, {-1, 4, 3, -3}};
        inline constexpr double JerkGram[3][3]{{6, 3, 1}, {3, 4, 3}, {1, 3, 6}};
        inline constexpr double SolveRoundoff = 4096.0 * std::numeric_limits<double>::epsilon();

        inline SolveVector JerkGradient(const double start, const double next,
                                        const double future, const double velocity,
                                        const double acceleration)
        {
            const std::array<double, 6> d{
                -start - 0.6 * velocity - 0.15 * acceleration,
                2.0 * start + velocity + 0.15 * acceleration,
                next - start - 0.4 * velocity - 0.05 * acceleration,
                -7.0 * next, 10.0 * next, future - 4.0 * next};
            SolveVector gradient{};
            for (std::size_t block = 0; block < 2; ++block)
                for (std::size_t i = 0; i < 3; ++i)
                    for (std::size_t j = 0; j < 3; ++j)
                        for (std::size_t k = 0; k < 4; ++k)
                            gradient[k] += JerkMap[block * 3 + i][k] * JerkGram[i][j] * d[block * 3 + j];
            return gradient;
        }

        struct Constraint { SolveVector normal{}; double bound{}; };

        inline bool SolveLinear(std::array<std::array<double, 9>, 8>& rows, const std::size_t n)
        {
            for (std::size_t col = 0; col < n; ++col)
            {
                std::size_t pivot = col;
                for (std::size_t row = col + 1; row < n; ++row)
                    if (std::abs(rows[row][col]) > std::abs(rows[pivot][col])) pivot = row;
                if (std::abs(rows[pivot][col]) <= SolveRoundoff) return false;
                std::swap(rows[col], rows[pivot]);
                const double divisor = rows[col][col];
                for (std::size_t k = col; k <= n; ++k) rows[col][k] /= divisor;
                for (std::size_t row = 0; row < n; ++row)
                {
                    if (row == col) continue;
                    const double factor = rows[row][col];
                    for (std::size_t k = col; k <= n; ++k)
                        rows[row][k] -= factor * rows[col][k];
                }
            }
            return true;
        }

        inline AxisSolve SolveAxisJerk(double start, double next, double future,
                                       double velocity, double acceleration,
                                       const double lower, const double upper)
        {
            AxisSolve result;
            const double extent = upper - lower;
            if (!(extent > 0.0))
            {
                result.controls.fill(lower);
                return result;
            }
            start = (start - lower) / extent;
            next = (next - lower) / extent;
            future = (future - lower) / extent;
            velocity /= extent;
            acceleration /= extent;
            const auto inBounds = [](const double value) {
                return std::isfinite(value) && value >= -SolveRoundoff && value <= 1.0 + SolveRoundoff;
            };
            const double velocityCtrl = start + velocity / 5.0;
            const double accelCtrl = start + 0.4 * velocity + 0.05 * acceleration;
            if (!inBounds(start) || !inBounds(next) || !inBounds(future) ||
                !inBounds(velocityCtrl) || !inBounds(accelCtrl))
                throw std::logic_error("Invalid bounded-trajectory initial state");

            const double nextResidual = next - start - velocity - acceleration * 0.5;
            const double futureResidual = future - start - 2.0 * velocity - 2.0 * acceleration;
            const double powerCubic = (680.0 * nextResidual - 70.0 * futureResidual) / 191.0;
            const double powerQuartic = (-650.0 * nextResidual + 95.0 * futureResidual) / 191.0;
            const double powerQuintic = (161.0 * nextResidual - 25.0 * futureResidual) / 191.0;
            SolveVector solved{};
            solved[0] = start + 0.6 * velocity + 0.15 * acceleration + powerCubic / 10.0;
            solved[1] = start + 0.8 * velocity + 0.3 * acceleration + 0.4 * powerCubic + powerQuartic / 5.0;
            const double nextVelocity = 5.0 * (next - solved[1]);
            const double nextAccel = 20.0 * (next - 2.0 * solved[1] + solved[0]);
            const double nextQuintic = -powerQuartic / 5.0 - powerQuintic;
            solved[2] = next + 0.6 * nextVelocity + 0.15 * nextAccel + nextQuintic;
            solved[3] = next + 0.8 * nextVelocity + 0.3 * nextAccel + 3.0 * nextQuintic;

            const double joinCtrlLow = std::max(0.0, 2.0 * next - 1.0);
            const double joinCtrlHigh = std::min(1.0, 2.0 * next);
            const std::array<Constraint, 10> constraints{{
                {{{-1, 0, 0, 0}}, 0}, {{{1, 0, 0, 0}}, 1},
                {{{0, -1, 0, 0}}, -joinCtrlLow}, {{{0, 1, 0, 0}}, joinCtrlHigh},
                {{{0, 0, -1, 0}}, 0}, {{{0, 0, 1, 0}}, 1},
                {{{0, 0, 0, -1}}, 0}, {{{0, 0, 0, 1}}, 1},
                {{{-1, 4, 0, 0}}, 4.0 * next},
                {{{1, -4, 0, 0}}, 1.0 - 4.0 * next}}};
            const auto feasible = [&](const SolveVector& candidate) {
                for (const auto& constraint : constraints)
                {
                    double value = 0.0;
                    for (std::size_t i = 0; i < 4; ++i) value += constraint.normal[i] * candidate[i];
                    if (!std::isfinite(value) || value > constraint.bound + SolveRoundoff) return false;
                }
                return true;
            };
            if (!feasible(solved))
            {
                result.constrained = true;
                const auto gradient = JerkGradient(start, next, future, velocity, acceleration);
                double bestCost = std::numeric_limits<double>::infinity();
                SolveVector best{};
                std::array<std::size_t, 4> active{};
                const auto consider = [&](const std::size_t count) {
                    const std::size_t n = 4 + count;
                    std::array<std::array<double, 9>, 8> rows{};
                    for (std::size_t i = 0; i < 4; ++i)
                    {
                        for (std::size_t j = 0; j < 4; ++j) rows[i][j] = JerkHessian[i][j];
                        rows[i][n] = -gradient[i];
                    }
                    for (std::size_t k = 0; k < count; ++k)
                    {
                        const auto& constraint = constraints[active[k]];
                        for (std::size_t i = 0; i < 4; ++i)
                            rows[i][4 + k] = rows[4 + k][i] = constraint.normal[i];
                        rows[4 + k][n] = constraint.bound;
                    }
                    if (!SolveLinear(rows, n)) return;
                    SolveVector candidate{};
                    for (std::size_t i = 0; i < 4; ++i) candidate[i] = rows[i][n];
                    if (!feasible(candidate)) return;
                    double cost = 0.0;
                    for (std::size_t i = 0; i < 4; ++i)
                    {
                        cost += 2.0 * gradient[i] * candidate[i];
                        for (std::size_t j = 0; j < 4; ++j)
                            cost += candidate[i] * JerkHessian[i][j] * candidate[j];
                    }
                    if (cost < bestCost) { bestCost = cost; best = candidate; }
                };
                const auto enumerate = [&](auto&& self, const std::size_t first,
                                           const std::size_t count) -> void {
                    consider(count);
                    if (count == active.size()) return;
                    for (std::size_t i = first; i < constraints.size(); ++i)
                    {
                        active[count] = i;
                        self(self, i + 1, count + 1);
                    }
                };
                enumerate(enumerate, 0, 0);
                if (!std::isfinite(bestCost))
                    throw std::logic_error("Bounded minimum-jerk solve has no feasible result");
                solved = best;
            }
            const std::array<double, 6> controls{
                start, velocityCtrl, accelCtrl, solved[0], solved[1], next};
            for (std::size_t i = 0; i < controls.size(); ++i)
            {
                result.controls[i] = lower + extent * std::clamp(controls[i], 0.0, 1.0);
            }
            return result;
        }
    }

    inline MotionSolve SolveMinJerk(const MotionPoint current, const MotionPoint next,
                                    const MotionPoint future, const MotionPoint velocity,
                                    const MotionPoint acceleration, const MotionPoint lower,
                                    const MotionPoint upper)
    {
        const auto x = jerk_detail::SolveAxisJerk(current.x, next.x, future.x, velocity.x,
                                                  acceleration.x, lower.x, upper.x);
        const auto y = jerk_detail::SolveAxisJerk(current.y, next.y, future.y, velocity.y,
                                                  acceleration.y, lower.y, upper.y);
        const auto z = jerk_detail::SolveAxisJerk(current.z, next.z, future.z, velocity.z,
                                                  acceleration.z, lower.z, upper.z);
        MotionSolve result;
        for (std::size_t i = 0; i < result.controls.size(); ++i)
            result.controls[i] = {x.controls[i], y.controls[i], z.controls[i]};
        result.controls.front() = current;
        result.controls.back() = next;
        result.hullConstrained = x.constrained || y.constrained || z.constrained;
        return result;
    }
}
