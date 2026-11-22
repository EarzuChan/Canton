#pragma once

#include "Geometry.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace canton::core {
    enum class AnimationKind { Tween, Spring, Snap };

    struct AnimationSpec {
        AnimationKind kind = AnimationKind::Spring;
        float durationMillis = 300;
        float delayMillis = 0;
        float stiffness = 400;
        float dampingRatio = 1;
        float threshold = 0.01f;
        std::array<float, 4> bezier{0.4f, 0, 0.2f, 1};
        int iterations = 1;
        bool reverse = false;
        bool operator==(const AnimationSpec&) const = default;
    };

    inline std::pair<float, float> sampleSpring(float delta, float velocity, double seconds, const AnimationSpec& spec) {
        const double omega = std::sqrt(spec.stiffness), zeta = spec.dampingRatio;
        if (zeta < 1) {
            const auto decay = zeta * omega, frequency = omega * std::sqrt(1 - zeta * zeta);
            const auto b = (velocity + decay * delta) / frequency;
            const auto c = std::cos(frequency * seconds), s = std::sin(frequency * seconds), e = std::exp(-decay * seconds);
            return {static_cast<float>(e * (delta * c + b * s)), static_cast<float>(e * ((b * frequency - decay * delta) * c - (delta * frequency + decay * b) * s))};
        }
        if (std::abs(zeta - 1) < 1e-6) {
            const auto b = velocity + omega * delta, e = std::exp(-omega * seconds);
            return {static_cast<float>(e * (delta + b * seconds)), static_cast<float>(e * (velocity - omega * b * seconds))};
        }
        const auto root = std::sqrt(zeta * zeta - 1), r1 = -omega * (zeta - root), r2 = -omega * (zeta + root);
        const auto a = (velocity - r2 * delta) / (r1 - r2), b = delta - a;
        return {static_cast<float>(a * std::exp(r1 * seconds) + b * std::exp(r2 * seconds)), static_cast<float>(a * r1 * std::exp(r1 * seconds) + b * r2 * std::exp(r2 * seconds))};
    }

    inline float sampleEasing(float fraction, const std::array<float, 4>& bezier) {
        if (fraction <= 0 || fraction >= 1) return std::clamp(fraction, 0.0f, 1.0f);
        const auto curve = [](float t, float a, float b) {
            return 3 * (1 - t) * (1 - t) * t * a + 3 * (1 - t) * t * t * b + t * t * t;
        };
        float low = 0, high = 1;
        for (int i = 0; i < 24; ++i) {
            const auto mid = (low + high) / 2;
            if (curve(mid, bezier[0], bezier[2]) < fraction)
                low = mid;
            else
                high = mid;
        }
        return curve((low + high) / 2, bezier[1], bezier[3]);
    }

    // 多轴动画：各轴共用一份规格与起始时刻，弹簧按轴各自求解，任一轴未停稳就继续
    template <std::size_t N>
    struct VectorAnimation {
        using Value = std::array<float, N>;
        bool running = false;
        Value current{}, from{}, target{}, velocity{}, initialVelocity{};
        double startMillis = 0;
        AnimationSpec spec;

        void snapTo(const Value& value) {
            current = from = target = value;
            velocity = initialVelocity = {};
            running = false;
        }

        // 从当前值和速度出发，奔向新目标
        void animateTo(const Value& next, const AnimationSpec& nextSpec, double now) {
            from = current;
            target = next;
            initialVelocity = velocity;
            startMillis = now;
            spec = nextSpec;
            running = current != target || velocity != Value{};
        }

        const Value& sample(double now) {
            if (!running) return current;
            const auto elapsed = std::max(0.0, now - startMillis);
            if (spec.kind == AnimationKind::Spring) {
                bool moving = false;
                for (std::size_t axis = 0; axis < N; ++axis) {
                    const auto [delta, speed] = sampleSpring(from[axis] - target[axis], initialVelocity[axis], elapsed / 1000, spec);
                    current[axis] = target[axis] + delta;
                    velocity[axis] = speed;
                    moving = moving || std::abs(delta) > spec.threshold || std::abs(speed) > spec.threshold * 10;
                }
                running = moving;
            } else {
                const auto duration = spec.kind == AnimationKind::Snap ? 0.0 : static_cast<double>(spec.durationMillis);
                const auto cycleDuration = spec.delayMillis + duration;
                const auto finished = cycleDuration == 0 || elapsed >= cycleDuration * spec.iterations;
                const auto cycle = finished ? spec.iterations - 1 : static_cast<int>(elapsed / cycleDuration);
                const auto local = finished ? cycleDuration : elapsed - cycle * cycleDuration;
                const auto fraction = local < spec.delayMillis ? 0.0f : duration == 0 ? 1.0f : std::min(1.0f, static_cast<float>((local - spec.delayMillis) / duration));
                const auto eased = sampleEasing(fraction, spec.bezier);
                const auto progress = spec.reverse && cycle % 2 == 1 ? 1 - eased : eased;
                for (std::size_t axis = 0; axis < N; ++axis) current[axis] = from[axis] + (target[axis] - from[axis]) * progress;
                velocity = {};
                running = !finished;
            }
            if (!running) {
                current = target;
                velocity = {};
            }
            return current;
        }
    };

    struct SizeAnimation {
        bool initialized = false;
        VectorAnimation<2> axes;

        bool running() const noexcept {
            return axes.running;
        }

        Size target() const noexcept {
            return {axes.target[0], axes.target[1]};
        }

        const AnimationSpec& spec() const noexcept {
            return axes.spec;
        }

        Size update(Size next, const AnimationSpec& nextSpec, double now) {
            const VectorAnimation<2>::Value value{next.width, next.height};
            if (!initialized) {
                initialized = true;
                axes.snapTo(value);
                axes.spec = nextSpec;
            } else if (axes.target != value || axes.spec != nextSpec)
                axes.animateTo(value, nextSpec, now);
            axes.sample(now);
            for (auto& axis : axes.current) axis = std::max(0.0f, axis);
            return {axes.current[0], axes.current[1]};
        }
    };
}
