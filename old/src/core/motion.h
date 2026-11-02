// Timing curves and text effects for the window's few animations. Pure math,
// no UI, so the curves can be tested. Curves after the FlutterFX widgets
// (github.com/flutterfx/flutterfx_widgets, MIT): tools/curves.dart,
// loader-avatars and fx_10_hyper_text.
#pragma once
#include <cstdint>
#include <string>

namespace fl::motion {

// t is clamped to 0..1 by every curve.
float smoothstep(float t);
float easeOutCubic(float t);
// Rises to `overshoot` at `peak`, then settles on 1. For something that arrives.
float singleBounce(float t, float overshoot = 1.25f, float peak = 0.7f);

// 0..1, one breath every `periodMs`; each `index` lags the one before, so a row
// of slots breathes as a wave instead of in unison.
float wave(uint64_t elapsedMs, int index, uint64_t periodMs = 1600);

// `text` while it decodes: characters before progress * length are final, the
// rest are random characters of the same kind (A-Z, a-z, 0-9). Spaces and
// punctuation stay put, so word shapes and line breaks hold. `seed` picks the
// random characters; change it to roll them.
std::wstring scramble(const std::wstring& text, float progress, uint32_t seed);

}  // namespace fl::motion
