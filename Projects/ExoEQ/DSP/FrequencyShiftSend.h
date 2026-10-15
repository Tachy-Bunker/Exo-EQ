#pragma once

#include <cmath>
#include <algorithm>

// FrequencyShiftSend.h
// Third per-band send (after DelaySend, DistortionSend) -- same shape
// (ProcessSample in, ProcessSample out, SetParams) so it drops into Band
// the same way. This is a genuine single-sideband frequency SHIFTER, not a
// pitch shifter: a pitch shifter multiplies every partial's frequency by a
// ratio (harmonic content stays harmonic); this ADDS a fixed Hz offset to
// every partial, which is what makes frequency shifting sound metallic/
// inharmonic/bell-like rather than "pitched" -- genuinely a different,
// rarer effect, and the one actually useful for the kind of spectral
// mangling a modular/experimental filter plugin like this is for.
//
// Implementation: classic analog-style SSB modulator. Two allpass-filter
// chains (4 first-order stages each) built from a well-known coefficient
// set approximate a wideband 90-degree phase split (an IIR Hilbert
// transformer) across most of the audible range with well under a degree
// of phase error -- no FFT, no added latency worth mentioning, cheap
// enough per sample for a weak device. The split pair is then modulated
// by a quadrature oscillator at the shift frequency and summed/differenced
// to cancel one sideband, leaving the signal shifted up or down by
// `shiftHz` (sign gives direction).

namespace exoeq
{

struct FrequencyShiftSendParams
{
  double shiftHz = 0.0;  // +up / -down, in Hz; typically kept well under 1kHz
  double mix = 0.3;       // 0 = dry, 1 = fully wet
};

class FrequencyShiftSend
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = std::max(1.0, sampleRate);
    Reset();
  }

  void SetParams(const FrequencyShiftSendParams& p)
  {
    mParams = p;
    mParams.mix = std::clamp(mParams.mix, 0.0, 1.0);
  }

  void Reset()
  {
    for (auto& s : mChainA) s = {0.0, 0.0};
    for (auto& s : mChainB) s = {0.0, 0.0};
    mOscPhase = 0.0;
    // One chain needs a one-sample head start relative to the other so the
    // two outputs line up in time as well as in phase -- otherwise the
    // "90 degrees apart" relationship is right in phase but off in timing,
    // which leaks the cancelled sideband back in.
    mDelayedInput = 0.0;
  }

  double ProcessSample(double input)
  {
    if (mParams.shiftHz == 0.0)
      return input; // no-op shortcut; also avoids waking the oscillator for nothing

    // Chain A operates on the input delayed by one sample so both chains'
    // group delay lines up (same trick real analog phase-splitter designs
    // use: one path is "the signal", the other is "the signal in
    // quadrature", and they have to agree on WHEN as well as on phase).
    const double chainAIn = mDelayedInput;
    mDelayedInput = input;

    double a = chainAIn;
    for (size_t i = 0; i < kNumStages; ++i)
      a = RunAllpass(mChainA[i], kCoeffsA[i], a);

    double b = input;
    for (size_t i = 0; i < kNumStages; ++i)
      b = RunAllpass(mChainB[i], kCoeffsB[i], b);

    const double phaseInc = 2.0 * 3.14159265358979323846 * mParams.shiftHz / mSampleRate;
    mOscPhase += phaseInc;
    if (mOscPhase > 3.14159265358979323846) mOscPhase -= 2.0 * 3.14159265358979323846;
    else if (mOscPhase < -3.14159265358979323846) mOscPhase += 2.0 * 3.14159265358979323846;

    const double osc_cos = std::cos(mOscPhase);
    const double osc_sin = std::sin(mOscPhase);

    // Single-sideband sum: cancels the mirrored sideband, leaving the
    // signal shifted by +shiftHz (the subtraction form below shifts up for
    // positive shiftHz with this chain ordering; shiftHz's own sign still
    // controls direction via the oscillator phase increment).
    const double shifted = a * osc_cos + b * osc_sin;
    const double out = input * (1.0 - mParams.mix) + shifted * mParams.mix;
    return std::isfinite(out) ? out : 0.0;
  }

private:
  struct AllpassState { double xm1, ym1; };

  static double RunAllpass(AllpassState& s, double coeff, double x)
  {
    // First-order allpass: y[n] = -a*x[n] + x[n-1] + a*y[n-1]
    const double y = -coeff * x + s.xm1 + coeff * s.ym1;
    s.xm1 = x;
    s.ym1 = y;
    return y;
  }

  // Published 4-stage quadrature allpass coefficient pair (an IIR
  // approximation to a wideband 90-degree Hilbert split) -- chain A and
  // chain B together hold phase quadrature to well under a degree of error
  // across most of the audible range, which is what keeps the rejected
  // sideband down without needing an FFT-based Hilbert transform.
  static constexpr size_t kNumStages = 4;
  static constexpr double kCoeffsA[kNumStages] = { 0.6923877777561618, 0.9360654322959878, 0.9882295226859894, 0.9987488452738250 };
  static constexpr double kCoeffsB[kNumStages] = { 0.4021921162425817, 0.8561710882420600, 0.9722909545651049, 0.9952884791278430 };

  AllpassState mChainA[kNumStages]{};
  AllpassState mChainB[kNumStages]{};
  double mOscPhase = 0.0;
  double mDelayedInput = 0.0;
  double mSampleRate = 44100.0;
  FrequencyShiftSendParams mParams;
};

} // namespace exoeq
