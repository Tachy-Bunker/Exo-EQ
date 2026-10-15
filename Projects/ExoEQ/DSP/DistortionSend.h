#pragma once

#include <cmath>
#include <algorithm>

// DistortionSend.h
// Second of the per-band sends (DelaySend was first) -- same shape
// (ProcessSample in, ProcessSample out, SetParams) so it drops into Band
// the same way. Deliberately not a plain tanh clipper: `character` blends
// between soft saturation (tanh) and triangle-wave foldback. Foldback
// reflects the signal back on itself past a threshold instead of just
// flattening it, which folds the harmonic spectrum over on itself as drive
// increases -- a harsher, less monotonic timbre than ordinary saturation,
// closer to a wavefolder than a fuzz pedal. `character` lets a band sit
// anywhere between "warm" and "folded".

namespace exoeq
{

struct DistortionSendParams
{
  double drive = 1.0;      // gain into the shaper; >1 pushes harder into both stages
  double character = 0.0;  // 0 = pure soft saturation, 1 = pure foldback
  double mix = 0.3;         // 0 = dry, 1 = fully wet
};

class DistortionSend
{
public:
  void SetSampleRate(double /*sampleRate*/) { /* stateless -- kept for shape-parity with other sends */ }

  void SetParams(const DistortionSendParams& p)
  {
    mParams = p;
    mParams.drive = std::clamp(mParams.drive, 0.01, 64.0);
    mParams.character = std::clamp(mParams.character, 0.0, 1.0);
    mParams.mix = std::clamp(mParams.mix, 0.0, 1.0);
  }

  void Reset() { /* stateless */ }

  double ProcessSample(double input)
  {
    const double x = input * mParams.drive;

    const double soft = std::tanh(x);
    const double folded = Foldback(x, 1.0);

    // Softness->fold blend happens pre-normalization: folded content is
    // naturally louder/busier than tanh at the same drive, so trim it
    // relative to how much of it is in the mix rather than giving the
    // folded extreme a jump in level when `character` crosses over.
    const double shaped = soft * (1.0 - mParams.character) + folded * mParams.character;
    const double out = input * (1.0 - mParams.mix) + shaped * mParams.mix;
    return std::isfinite(out) ? out : 0.0;
  }

private:
  // Triangle-wave foldback: reflects x back into [-threshold, threshold]
  // instead of clamping it, so pushing drive harder folds the waveform
  // over on itself repeatedly rather than just squaring it off. Closed
  // form via fmod -- no loop, so cost is flat regardless of how far
  // outside the threshold x lands (matters for weak devices: a naive
  // `while (x > thresh) reflect` implementation is unbounded-cost at
  // high drive, this is not).
  static double Foldback(double x, double threshold)
  {
    const double period = 4.0 * threshold;
    double xm = std::fmod(x + threshold, period);
    if (xm < 0.0) xm += period;
    return std::fabs(xm - 2.0 * threshold) - threshold;
  }

  DistortionSendParams mParams;
};

} // namespace exoeq
