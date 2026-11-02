#include "motion.h"

#include <algorithm>
#include <cmath>

namespace fl::motion {

namespace {
constexpr float kPi = 3.14159265358979f;
float clamp01(float t) { return std::clamp(t, 0.0f, 1.0f); }
}  // namespace

float smoothstep(float t) {
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}

float easeOutCubic(float t) {
    float u = 1 - clamp01(t);
    return 1 - u * u * u;
}

float singleBounce(float t, float overshoot, float peak) {
    t = clamp01(t);
    if (t < peak) return smoothstep(t / peak) * overshoot;
    return overshoot + (1 - overshoot) * smoothstep((t - peak) / (1 - peak));
}

float wave(uint64_t elapsedMs, int index, uint64_t periodMs) {
    // 0.9 rad between neighbours: four slots span most of one cycle, so the
    // wave visibly travels instead of looking like a flicker.
    float phase = 2 * kPi * static_cast<float>(elapsedMs % periodMs) / static_cast<float>(periodMs) - 0.9f * index;
    return (std::sin(phase) + 1) / 2;
}

std::wstring scramble(const std::wstring& text, float progress, uint32_t seed) {
    size_t locked = static_cast<size_t>(clamp01(progress) * static_cast<float>(text.size()));
    std::wstring out = text;
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = locked; i < out.size(); ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        wchar_t c = out[i];
        if (c >= L'A' && c <= L'Z') out[i] = static_cast<wchar_t>(L'A' + x % 26);
        else if (c >= L'a' && c <= L'z') out[i] = static_cast<wchar_t>(L'a' + x % 26);
        else if (c >= L'0' && c <= L'9') out[i] = static_cast<wchar_t>(L'0' + x % 10);
    }
    return out;
}

}  // namespace fl::motion
