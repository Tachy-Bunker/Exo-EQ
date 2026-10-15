#pragma once

#include "FilterCore.h"
#include <cmath>
#include <algorithm>

namespace exoeq
{

class StateVariableFilter final : public IFilterNode
{
public:
  void SetSampleRate(double sampleRate) override
  {
    mSampleRate = std::max(1.0, sampleRate);
    RecalcCoeffs();
  }

  void SetParams(const FilterParams& params) override
  {
    mParams = params;
    mParams.cutoffHz = std::clamp(mParams.cutoffHz, 5.0, mSampleRate * 0.49);
    mParams.resonance = std::max(0.01, mParams.resonance);
    mParams.morph = std::clamp(mParams.morph, 0.0, 1.0);
    RecalcCoeffs();
  }

  void Reset() override
  {
    mIc1eq = 0.0;
    mIc2eq = 0.0;
  }

  double ProcessSample(double input) override
  {
    const double v3 = input - mIc2eq;
    const double v1 = mA1 * mIc1eq + mA2 * v3;
    const double v2 = mIc2eq + mA2 * mIc1eq + mA3 * v3;
    mIc1eq = FlushState(2.0 * v1 - mIc1eq);
    mIc2eq = FlushState(2.0 * v2 - mIc2eq);

    const double lp = v2;
    const double bp = v1;
    const double hp = input - mK * v1 - v2;

    // Equal-power (sin/cos) crossfade, not linear: the taps aren't
    // phase-aligned at a given probe frequency, so a linear blend leaves a
    // ~3dB dip at the quarter-points under high resonance (Phase 0 finding).
    constexpr double kHalfPi = 1.5707963267948966;
    double out;
    if (mParams.morph <= 0.5)
    {
      const double t = mParams.morph * 2.0 * kHalfPi;
      out = lp * std::cos(t) + bp * std::sin(t);
    }
    else
    {
      const double t = (mParams.morph - 0.5) * 2.0 * kHalfPi;
      out = bp * std::cos(t) + hp * std::sin(t);
    }
    return std::isfinite(out) ? out : 0.0;
  }

  // Test-only accessor: lets the harness probe raw LP/BP/HP without morph blending.
  struct Taps { double lp, bp, hp; };
  Taps ProcessSampleTaps(double input)
  {
    const double v3 = input - mIc2eq;
    const double v1 = mA1 * mIc1eq + mA2 * v3;
    const double v2 = mIc2eq + mA2 * mIc1eq + mA3 * v3;
    mIc1eq = 2.0 * v1 - mIc1eq;
    mIc2eq = 2.0 * v2 - mIc2eq;
    const double lp = v2, bp = v1, hp = input - mK * v1 - v2;
    return { lp, bp, hp };
  }

private:
  void RecalcCoeffs()
  {
    // Clamp strictly below 0.49*sr-derived angle too: cutoffHz is already
    // clamped in SetParams, but RecalcCoeffs also runs from SetSampleRate
    // (before SetParams has ever run, or with a stale cutoff left over from
    // a previous, higher sample rate) so re-clamp the angle itself here —
    // tan() approaching pi/2 is where g, and everything derived from it,
    // blows up to Inf/NaN and silently kills the whole render.
    const double maxHz = std::max(20.0, mSampleRate * 0.49);
    const double cutoffHz = std::clamp(mParams.cutoffHz, 5.0, maxHz);
    const double g = std::tan(3.14159265358979323846 * cutoffHz / mSampleRate);
    mK = 1.0 / std::max(0.01, mParams.resonance);
    mA1 = 1.0 / (1.0 + g * (g + mK));
    mA2 = g * mA1;
    mA3 = g * mA2;

    if (!std::isfinite(mA1) || !std::isfinite(mA2) || !std::isfinite(mA3) || !std::isfinite(mK))
    {
      // Last-resort safety net: never let a bad coefficient set latch in and
      // turn every future sample to NaN/Inf for the rest of the plugin's life.
      mK = 1.0; mA1 = 1.0; mA2 = 0.0; mA3 = 0.0;
    }
  }

  // Denormals (|x| below ~1e-308 but nonzero) run 10-100x slower on x86 FPUs
  // and are exactly what a decaying IIR filter's state tends toward once the
  // input goes quiet — this is what was overloading the CPU and causing
  // stutter in real-time preview. Flushing them (and any NaN/Inf that
  // sneaks in from an edge-case coefficient) to a clean 0.0 every sample is
  // cheap and keeps both the CPU cost and the output bounded.
  static double FlushState(double x)
  {
    if (!std::isfinite(x)) return 0.0;
    return (std::fabs(x) < 1.0e-300) ? 0.0 : x;
  }

  FilterParams mParams;
  double mSampleRate = 44100.0;
  double mIc1eq = 0.0, mIc2eq = 0.0;
  double mK = 1.0, mA1 = 0.0, mA2 = 0.0, mA3 = 0.0;
};

} // namespace exoeq
