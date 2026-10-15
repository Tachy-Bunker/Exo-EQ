#include "DSP/TransformFilter.h"
#include <cstdio>
#include <cmath>

using namespace exoeq;

int main()
{
  int failures = 0;
  auto check = [&](bool cond, const char* label)
  {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", label);
    if (!cond) ++failures;
  };

  // --- 1. exponent=1.0 is a true no-op (within the Nyquist clamp). ---
  {
    std::vector<GeneratedNode> input = { {500.0, 2.0, 0.8}, {2000.0, -3.0, 0.5} };
    TransformParams p; p.pivotHz = 1000.0; p.exponent = 1.0;
    const auto out = ApplyTransform(input, p, 48000.0);
    check(out.size() == input.size(), "no-op transform preserves node count");
    check(std::fabs(out[0].freqHz - 500.0) < 1.0e-6, "exponent=1 leaves node 0's frequency unchanged");
    check(std::fabs(out[1].freqHz - 2000.0) < 1.0e-6, "exponent=1 leaves node 1's frequency unchanged");
    check(std::fabs(out[0].gainDb - 2.0) < 1.0e-9 && std::fabs(out[0].resonance - 0.8) < 1.0e-9, "gain/resonance pass through unchanged");
  }

  // --- 2. exponent>1 stretches nodes AWAY from the pivot (in log space). ---
  {
    std::vector<GeneratedNode> input = { {500.0, 0.0, 0.707} }; // below pivot
    TransformParams p; p.pivotHz = 1000.0; p.exponent = 2.0;
    const auto out = ApplyTransform(input, p, 48000.0);
    // pivot*(500/1000)^2 = 1000*0.25 = 250 -- further below pivot than 500 was
    check(out[0].freqHz < 500.0, "exponent>1 pushes a below-pivot node further below the pivot (stretch)");
    check(std::fabs(out[0].freqHz - 250.0) < 0.5, "exact stretch math: 1000*(500/1000)^2 == 250Hz");
  }

  // --- 3. exponent between 0 and 1 compresses nodes TOWARD the pivot. ---
  {
    std::vector<GeneratedNode> input = { {250.0, 0.0, 0.707} };
    TransformParams p; p.pivotHz = 1000.0; p.exponent = 0.5;
    const auto out = ApplyTransform(input, p, 48000.0);
    // pivot*(250/1000)^0.5 = 1000*0.5 = 500 -- closer to the pivot than 250 was
    check(out[0].freqHz > 250.0 && out[0].freqHz < 1000.0, "exponent in (0,1) pulls a node toward the pivot (compress)");
    check(std::fabs(out[0].freqHz - 500.0) < 0.5, "exact compress math: 1000*(250/1000)^0.5 == 500Hz");
  }

  // --- 4. Negative exponent mirrors around the pivot (same math as
  //        FractalGenerator.h's own `symmetric` option). ---
  {
    std::vector<GeneratedNode> input = { {2000.0, 0.0, 0.707} }; // 1 octave above a 1000Hz pivot
    TransformParams p; p.pivotHz = 1000.0; p.exponent = -1.0;
    const auto out = ApplyTransform(input, p, 48000.0);
    // pivot^2/freq = 1000000/2000 = 500 -- exactly 1 octave BELOW the pivot, the mirror image
    check(std::fabs(out[0].freqHz - 500.0) < 0.5, "exponent=-1 mirrors a node around the pivot exactly (pivot^2/freq)");
  }

  // --- 5. Nyquist clamp still applies, same guard as the fractal generator. ---
  {
    std::vector<GeneratedNode> input = { {100.0, 0.0, 0.707} };
    TransformParams p; p.pivotHz = 10.0; p.exponent = 5.0; // engineered to blow way past Nyquist
    const double sr = 48000.0;
    const auto out = ApplyTransform(input, p, sr);
    check(out[0].freqHz <= sr * 0.5 * 0.98 + 1.0e-6, "transform output respects the same 2%-guarded Nyquist bound as the fractal generator");
  }

  // --- 6. Empty input produces empty output, no crash. ---
  {
    std::vector<GeneratedNode> input;
    TransformParams p;
    const auto out = ApplyTransform(input, p, 48000.0);
    check(out.empty(), "empty input series produces an empty (not crashing) output series");
  }

  // --- 7. A pathological ratio/exponent combination that would produce
  //        a non-finite result leaves that node at its ORIGINAL
  //        frequency rather than propagating NaN/garbage. ---
  {
    std::vector<GeneratedNode> input = { {0.0001, 0.0, 0.707} }; // pathologically tiny, clamped internally but exercises the ratio guard
    TransformParams p; p.pivotHz = 1000.0; p.exponent = 1.0e308; // absurd exponent
    const auto out = ApplyTransform(input, p, 48000.0);
    check(std::isfinite(out[0].freqHz), "a pathological exponent never produces a non-finite frequency");
  }

  std::printf("\n%d check(s) failed.\n", failures);
  return failures == 0 ? 0 : 1;
}
