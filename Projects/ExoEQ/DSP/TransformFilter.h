#pragma once

#include "FractalGenerator.h"
#include <vector>
#include <cmath>
#include <algorithm>

// TransformFilter.h
// P5 scope step 2 of 3 (see the Features Doc's "P5 scope" section). Same
// one-shot-batch-on-the-UI-thread nature as DSP/FractalGenerator.h (see
// that file's own header note on why audio-thread performance concerns
// don't apply here) -- the difference is the INPUT: instead of a
// closed-form formula, this reads the plugin's own CURRENT active band
// series and remaps it, reusing exoeq::GeneratedNode as the shared
// node type between both generators (per the Features Doc's "one
// generic node-series generator machine" plan).
//
// Scope decision made here, not left to the UI layer: the three ideas
// the doc listed separately -- "frequency warp via a log/exp curve",
// "mirror/invert around a pivot frequency", and "a stretch curve" --
// all turn out to be the SAME operation, a power map around a pivot
// frequency in log-frequency space:
//
//   freq' = pivot * (freq / pivot) ^ exponent
//
// exponent = 1 is a no-op. exponent > 1 spreads bands away from the
// pivot (a "stretch"/expansion). exponent between 0 and 1 compresses
// them toward the pivot. A NEGATIVE exponent is exactly the "mirror/
// invert around a pivot" idea (freq' = pivot^2/freq when exponent =
// -1), which is also exactly the same math FractalGenerator.h's own
// `symmetric` option already uses. So rather than three separate knobs/
// code paths that would need to interact in unobvious ways, this is one
// pivot + one exponent (continuous, can go negative) -- simpler to
// build, simpler to use, and nothing the three original ideas described
// is actually missing.
namespace exoeq
{

struct TransformParams
{
  double pivotHz = 1000.0;
  // Continuous: 1.0 = no change, >1 = stretch away from pivot, 0..1 =
  // compress toward pivot, <0 = mirror/invert (freq' = pivot^2/freq
  // when exactly -1, a mirror AND a stretch together for any other
  // negative value). One knob covers all three of the doc's original
  // ideas -- see the header note above.
  double exponent = 1.0;
};

// Reads an existing node series (built by the caller from the plugin's
// own current active bands -- see ExoEQ.cpp's GenerateTransformBands)
// and returns a NEW series with each node's frequency remapped. gainDb
// and resonance pass through UNCHANGED -- this transform only moves
// WHERE a band sits in the spectrum, not its character, matching the
// doc's own framing of this as a frequency-domain operation. sampleRate
// is required for the same reason FractalGenerator.h's own Nyquist
// clamp is: the clamp must be correct for whatever rate is live right
// now, computed fresh on every call rather than cached.
inline std::vector<GeneratedNode> ApplyTransform(const std::vector<GeneratedNode>& input, const TransformParams& params, double sampleRate)
{
  std::vector<GeneratedNode> output;
  output.reserve(input.size());

  const double pivot = std::max(1.0, params.pivotHz);

  for (const auto& node : input)
  {
    GeneratedNode out = node;
    const double ratio = std::max(1.0e-6, node.freqHz / pivot);
    double freq = pivot * std::pow(ratio, params.exponent);
    if (!std::isfinite(freq) || freq <= 0.0)
      freq = node.freqHz; // a pathological exponent/ratio combination leaves this node where it was, rather than producing garbage
    out.freqHz = detail::ClampToSafeRange(freq, sampleRate);
    output.push_back(out);
  }

  return output;
}

} // namespace exoeq
