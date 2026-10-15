#include "DSP/AirwindowsFilters.h"
#include <cstdio>
#include <cmath>
#include <vector>

// test_sampleRate_independence.cpp
// User-flagged requirement: with parameter VALUES held fixed (cutoffHz,
// morph, etc. -- real Hz/physical units, not normalized 0-1 fractions of
// Nyquist), the plugin should sound the same regardless of host sample
// rate. This is already true by construction for anything built from
// bilinear-transform biquad coefficients (omega = 2*pi*cutoffHz/sampleRate
// -- the SR term is baked into the coefficient derivation) or from a
// taps-count/decimation-rate formula that itself scales with sampleRate
// (AverMatrix's taps, Energy2's cycle rate). AwSlewSonicFilter was the one
// exception found: its slew differentiator was a raw per-sample first
// difference with a fixed trim constant, which is NOT sample-rate
// invariant on its own (see AirwindowsFilters.h's own comment on
// mOverallScale for the physical reason). This test drives the SAME
// cutoff/morph through two different sample rates and confirms the
// RELATIVE shape of the response (not an exact sample-for-sample match,
// which isn't realistic across different sample rates/phase alignments)
// is consistent -- specifically, that the slew stage's gain relative to a
// bypassed pass is now sample-rate-independent, where before this fix it
// was not.

using namespace exoeq;

namespace
{
  // RMS of a fixed test signal (a simple two-tone burst) through the
  // filter at a given sample rate, with slew fully engaged (morph=1).
  double SlewedRms(double sampleRate)
  {
    AirwindowsSlewSonicFilter f;
    f.SetSampleRate(sampleRate);
    f.SetParams(2000.0, 1.0); // fixed real-Hz cutoff, all 3 slew stages engaged
    f.Reset();

    double sumSq = 0.0;
    int n = 0;
    const int totalSamples = static_cast<int>(sampleRate * 0.05); // 50ms, same physical duration regardless of SR
    for (int i = 0; i < totalSamples; ++i)
    {
      const double t = static_cast<double>(i) / sampleRate;
      const double x = std::sin(2.0 * 3.14159265358979323846 * 500.0 * t);
      const double y = f.ProcessSample(x);
      sumSq += y * y;
      ++n;
    }
    return std::sqrt(sumSq / std::max(1, n));
  }
}

int main()
{
  int failures = 0;
  auto check = [&](bool cond, const char* label)
  {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", label);
    if (!cond) ++failures;
  };

  const double rms44 = SlewedRms(44100.0);
  const double rms48 = SlewedRms(48000.0);
  const double rms96 = SlewedRms(96000.0);
  const double rms192 = SlewedRms(192000.0);

  std::printf("RMS @44.1k=%.6f @48k=%.6f @96k=%.6f @192k=%.6f\n", rms44, rms48, rms96, rms192);

  // Before the mOverallScale fix, doubling the sample rate roughly halved
  // the slew stage's output level (the per-sample first-difference shrinks
  // as samples get closer together in time) -- 96k would have come out at
  // roughly half of 48k's level, and 192k at roughly a quarter of that.
  // After the fix, the same real-Hz cutoff/morph should give results
  // within a small tolerance of each other across all 4 rates (not
  // bit-identical -- different sample rates really do produce slightly
  // different quantized filter responses -- but nowhere near a 2x/4x
  // swing).
  check(std::fabs(rms48 - rms44) / rms44 < 0.15, "48kHz within 15% of 44.1kHz (same real-Hz cutoff/morph)");
  check(std::fabs(rms96 - rms44) / rms44 < 0.15, "96kHz within 15% of 44.1kHz -- NOT half, confirming the SR-independence fix");
  check(std::fabs(rms192 - rms44) / rms44 < 0.20, "192kHz within 20% of 44.1kHz -- NOT a quarter, confirming the fix holds at extreme oversampling-friendly rates");

  std::printf("\n%d check(s) failed.\n", failures);
  return failures == 0 ? 0 : 1;
}
