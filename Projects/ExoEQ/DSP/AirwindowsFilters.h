#pragma once

#include <cmath>
#include <algorithm>
#include <array>

// AirwindowsFilters.h
// REAL ports this time -- built directly from the Airwindows WinVST source
// the user provided (MIT licensed, Chris Johnson/airwindows), not original
// guesses like Capacitor/AirLift in FilterTypes.h. Ported from:
//   Lowpass2 / Highpass2  (content-adaptive one-pole stack)
//   ZLowpass2 / ZHighpass2 / ZBandpass2 / ZNotch2  (the "Emu e6400 style"
//   family: a fixed-character resonant biquad plus a shared "opamp" grind
//   stage)
//   XLowpass / XHighpass / XBandpass / XNotch  ("distorted digital EQ,
//   inspired by retro sampler DSP" -- see AirwindowsXFilter below for
//   what makes this family distinctive: the nonlinearity lives INSIDE
//   each biquad's own recursive feedback path, not as a separate
//   saturation stage bolted on after)
//
// WHAT WAS KEPT EXACTLY, WHAT WAS DELIBERATELY CHANGED (read this before
// assuming a sound difference from the real plugin is a porting bug):
//
// 1. Every coefficient formula, every fixed constant (15500Hz/0.935
//    ultrasonic pre/post filter, the 0.00069/overallscale opamp highpass
//    time constant, the 0.1768 quintic soft-clip coefficient, each
//    filter's own fixed resonance -- including ZNotch2's golden-ratio
//    0.618033988749894848204586) is copied verbatim from the source.
//    This is the actual character of these filters and isn't touched.
//
// 2. The original plugins turn their FREQUENCY knob (a raw 0..1 slider,
//    "B" in the source) into Hz through a plugin-specific curve (e.g.
//    ZLowpass2: pow(B,3)*18930/sampleRate). This project already has a
//    real cutoffHz parameter with proper Hz display, so that curve is
//    skipped entirely in favor of using cutoffHz directly -- strictly a
//    UX improvement (an actual Hz readout instead of an opaque 0-1
//    slider), never a change to what the filter does once it has a
//    frequency.
//
// 3. The original plugins run up to 4 progressively-blended copies of
//    their biquad (a "wet" ladder control, originally knob "D") as a
//    single continuously-variable intensity control. This project
//    already has an established convention for "how many cascaded
//    stages" on every other dropdown filter type -- the morph knob
//    switches between 1 and 2 stages here, consistent with that, rather
//    than porting a separate 4-stage crossfade ladder.
//
// 4. The original plugins have a separate INPUT TRIM knob ("A",
//    raised to the 4th power) feeding extra drive into the clipFactor
//    division ahead of the biquad. This project repurposes the existing
//    `resonance` parameter as that drive amount (0..1, same convention
//    already used for the Capacitor character type) instead of adding a
//    knob nothing else on this dropdown has.
//
// Everything else -- the actual biquad difference equation, the opamp
// stage's highpass-bleed-then-ultrasonic-filter-then-softclip chain --
// is the genuine ported algorithm.

namespace exoeq
{

// The shared "opamp" tail used by all four Z-filters: subtracts a slow
// one-pole highpass bleed, runs a fixed ~15.5kHz/Q0.935 biquad, quintic-
// softclips, then the same fixed biquad again. Verbatim constants from
// ZLowpass2Proc.cpp's "opamp stuff" section (identical across all four
// Z-filter source files -- diffed to confirm before porting once instead
// of four times).
class AirwindowsOpampStage
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = std::max(1.0, sampleRate);
    const double overallScale = mSampleRate / 44100.0;
    mIirAmount = 0.00069 / overallScale;

    const double freq = 15500.0 / mSampleRate;
    constexpr double reso = 0.935;
    const double K = std::tan(3.14159265358979323846 * freq);
    const double norm = 1.0 / (1.0 + K / reso + K * K);
    mFixA0 = K * K * norm;
    mFixA1 = 2.0 * mFixA0;
    mFixA2 = mFixA0;
    mFixB1 = 2.0 * (K * K - 1.0) * norm;
    mFixB2 = (1.0 - K / reso + K * K) * norm;
  }

  void Reset()
  {
    mIir = 0.0;
    mPreS1 = mPreS2 = mPostS1 = mPostS2 = 0.0;
  }

  double Process(double x)
  {
    if (std::fabs(mIir) < 1.18e-37) mIir = 0.0; // verbatim denormal guard from the source
    mIir = (mIir * (1.0 - mIirAmount)) + (x * mIirAmount);
    x -= mIir; // subtractive one-pole highpass bleed removal ("opamp" character)

    double out = (x * mFixA0) + mPreS1;
    out = std::clamp(out, -1.0, 1.0);
    mPreS1 = (x * mFixA1) - (out * mFixB1) + mPreS2;
    mPreS2 = (x * mFixA2) - (out * mFixB2);
    x = out;

    x = std::clamp(x, -1.0, 1.0);
    x -= (x * x * x * x * x * 0.1768); // the quintic soft-clip, verbatim coefficient

    out = (x * mFixA0) + mPostS1;
    out = std::clamp(out, -1.0, 1.0);
    mPostS1 = (x * mFixA1) - (out * mFixB1) + mPostS2;
    mPostS2 = (x * mFixA2) - (out * mFixB2);

    return std::isfinite(out) ? out : 0.0;
  }

private:
  double mSampleRate = 44100.0;
  double mIirAmount = 0.00069;
  double mFixA0 = 0.0, mFixA1 = 0.0, mFixA2 = 0.0, mFixB1 = 0.0, mFixB2 = 0.0;
  double mIir = 0.0;
  double mPreS1 = 0.0, mPreS2 = 0.0, mPostS1 = 0.0, mPostS2 = 0.0;
};

enum class AwZKind { Lowpass, Highpass, Bandpass, Notch };

// The four Z-filters (ZLowpass2/ZHighpass2/ZBandpass2/ZNotch2): a fixed-
// character resonant biquad (see point 1 above for what's verbatim) into
// the shared AirwindowsOpampStage. `drive` (0..1, from this band's
// `resonance` param, see point 4) scales clipFactor, which is divided
// into the signal before the biquad -- with each biquad stage's output
// hard-clamped to +-1 (also verbatim), a lower clipFactor pushes more of
// the signal into that clamp, which is the actual distortion mechanism
// these filters use.
class AirwindowsZFilter
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = std::max(1.0, sampleRate);
    mOpamp.SetSampleRate(mSampleRate);
  }

  void SetParams(AwZKind kind, double cutoffHz, double drive01, int numStages)
  {
    mKind = kind;
    mNumStages = std::clamp(numStages, 1, 2);
    const double drive = std::clamp(drive01, 0.0, 1.0);

    const double maxHz = mSampleRate * 0.49;
    const double freq = std::clamp(cutoffHz, 5.0, maxHz) / mSampleRate;
    const double K = std::tan(3.14159265358979323846 * freq);

    double reso;
    switch (kind)
    {
      case AwZKind::Lowpass:  reso = 0.7071; break;
      case AwZKind::Highpass: reso = 1.0; break;
      case AwZKind::Bandpass: reso = 0.314; break;
      case AwZKind::Notch:    reso = 0.618033988749894848204586; break; // verbatim golden-ratio constant
      default: reso = 0.7071; break;
    }
    const double norm = 1.0 / (1.0 + K / reso + K * K);

    switch (kind)
    {
      case AwZKind::Lowpass:
        mA0 = K * K * norm; mA1 = 2.0 * mA0; mA2 = mA0;
        mB1 = 2.0 * (K * K - 1.0) * norm; mB2 = (1.0 - K / reso + K * K) * norm;
        mClipFactor = 1.212 - ((1.0 - drive) * 0.496);
        break;
      case AwZKind::Highpass:
        mA0 = norm; mA1 = -2.0 * norm; mA2 = norm;
        mB1 = 2.0 * (K * K - 1.0) * norm; mB2 = (1.0 - K / reso + K * K) * norm;
        mClipFactor = 1.0; // ZHighpass2's own source disables clipFactor entirely (see header note)
        break;
      case AwZKind::Bandpass:
        mA0 = (K / reso) * norm; mA1 = 0.0; mA2 = -mA0;
        mB1 = 2.0 * (K * K - 1.0) * norm; mB2 = (1.0 - K / reso + K * K) * norm;
        mClipFactor = 1.0 - ((1.0 - drive) * 0.304);
        break;
      case AwZKind::Notch:
        mA0 = (1.0 + K * K) * norm; mA1 = 2.0 * (K * K - 1.0) * norm; mA2 = mA0;
        mB1 = mA1; mB2 = (1.0 - K / reso + K * K) * norm;
        mClipFactor = 0.91 - ((1.0 - drive) * 0.15);
        break;
    }
    if (std::fabs(mClipFactor) < 1e-6) mClipFactor = (mClipFactor < 0.0) ? -1e-6 : 1e-6;

    // Verbatim "trim" from the source (0.1 + 3.712*biq_freq) -- a
    // frequency-dependent input scaling that keeps a low-cutoff resonant
    // band from overloading the clip stage.
    mTrim = 0.1 + (3.712 * freq);
  }

  void Reset()
  {
    mS1a = mS2a = mS1b = mS2b = 0.0;
    mOpamp.Reset();
  }

  double ProcessSample(double input)
  {
    double x = input * mTrim;
    x = std::clamp(x, -1.0, 1.0);
    x /= mClipFactor;

    x = BiquadStage(x, mS1a, mS2a);
    if (mNumStages >= 2)
    {
      x /= mClipFactor;
      x = BiquadStage(x, mS1b, mS2b);
    }

    x /= mClipFactor;
    return mOpamp.Process(x);
  }

private:
  double BiquadStage(double x, double& s1, double& s2)
  {
    double out = (x * mA0) + s1;
    out = std::clamp(out, -1.0, 1.0); // verbatim per-stage hard clamp -- this IS the distortion mechanism
    s1 = (x * mA1) - (out * mB1) + s2;
    s2 = (x * mA2) - (out * mB2);
    return std::isfinite(out) ? out : 0.0;
  }

  double mSampleRate = 44100.0;
  AwZKind mKind = AwZKind::Lowpass;
  int mNumStages = 1;
  double mA0 = 0.0, mA1 = 0.0, mA2 = 0.0, mB1 = 0.0, mB2 = 0.0;
  double mClipFactor = 1.0;
  double mTrim = 1.0;
  double mS1a = 0.0, mS2a = 0.0, mS1b = 0.0, mS2b = 0.0;
  AirwindowsOpampStage mOpamp;
};

// Lowpass2/Highpass2: a content-adaptive one-pole. `tight` (from source
// knob "B") skews the smoothing coefficient toward the input's own
// instantaneous amplitude -- louder content gets a slightly different
// effective cutoff than quiet content, which is the "unusual-sounding
// variable-slope" character the real plugin's own description promises.
// Cascading 1-4 of these (morph, same convention as every other slope
// type on this dropdown) approximates the original's 4-stage wet ladder
// as cumulative filtering rather than a crossfade -- point 3 in the
// header note.
class AirwindowsAdaptiveFilter
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = std::max(1.0, sampleRate); }

  void SetParams(double cutoffHz, double tight01, bool highpass, int numStages)
  {
    mHighpass = highpass;
    mNumStages = std::clamp(numStages, 1, 4);

    const double maxHz = mSampleRate * 0.49;
    const double hz = std::clamp(cutoffHz, 5.0, maxHz);
    // Same omega/(omega+1) one-pole coefficient this project already
    // uses in FilterTypes.h's OnePoleFilter -- gives cutoffHz its usual,
    // intuitive meaning instead of porting the original's raw 0..1
    // "iirAmount" slider (point 2 in the header note).
    constexpr double kTwoPi = 6.283185307179586;
    const double omega = kTwoPi * hz / mSampleRate;
    mIirAmount = std::clamp(omega / (omega + 1.0), 0.0001, 0.9999);

    // Verbatim "tight" remap from the source: 0..1 -> -1..1, halved below zero.
    mTight = (std::clamp(tight01, 0.0, 1.0) * 2.0) - 1.0;
    if (mTight < 0.0) mTight *= 0.5;
  }

  void Reset() { for (auto& s : mState) s = 0.0; }

  double ProcessSample(double input)
  {
    double x = input;
    for (int i = 0; i < mNumStages; ++i)
      x = Stage(x, mState[static_cast<size_t>(i)]);
    return std::isfinite(x) ? x : 0.0;
  }

private:
  double Stage(double x, double& iirState)
  {
    double offset;
    if (mTight > 0.0) offset = (1.0 - mTight) + (std::fabs(x) * mTight);
    else offset = (1.0 + mTight) + ((1.0 - std::fabs(x)) * mTight);
    offset = std::clamp(offset, 0.0000001, 1.0);

    iirState = (iirState * (1.0 - (offset * mIirAmount))) + (x * (offset * mIirAmount));
    if (std::fabs(iirState) < 1.0e-300) iirState = 0.0; // denormal flush, same rationale as StateVariableFilter.h

    return mHighpass ? (x - iirState) : iirState;
  }

  double mSampleRate = 44100.0;
  double mIirAmount = 0.1;
  double mTight = 0.0;
  bool mHighpass = false;
  int mNumStages = 1;
  std::array<double, 4> mState{};
};

enum class AwXKind { Lowpass, Highpass, Bandpass, Notch };

// XLowpass/XHighpass/XBandpass/XNotch: each is a standard DF1 biquad
// (fixed Q=0.7071, same cookbook form used elsewhere in this project),
// but each one puts a DIFFERENT nonlinearity directly in the recursive
// path rather than as a separate distortion stage -- this is what the
// source's own "distorted digital EQ" description is actually describing,
// and it's genuinely a different signature per type, not four copies of
// one trick:
//   Lowpass:  the FEEDBACK state itself is sin()'d every sample -- the
//             filter's own memory folds through sin(), so resonance-like
//             energy builds up into an aliased/wavefolded character
//             rather than ringing cleanly.
//   Highpass: no sin() at all -- just the feedback path's RANGE hard
//             clamped to a drive-controlled threshold every sample.
//   Bandpass: same sin()-in-the-feedback idea as Lowpass, but the INPUT
//             is pre-scaled by clipFactor before hitting the biquad, so
//             drive pushes harder into the fold from the input side.
//   Notch:    the input itself is sin()'d (clamped first) BEFORE the
//             biquad; the feedback path stays a plain, undistorted
//             recursion.
// Point 2/3/4 from this file's header note apply here too: cutoffHz is
// real Hz (not the source's own B^n*20000 slider curve), `resonance`
// supplies the drive/clip amount, and `morph` selects 1-4 cascaded
// stages instead of porting the 4-stage wet crossfade ladder.
class AirwindowsXFilter
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = std::max(1.0, sampleRate); }

  void SetParams(AwXKind kind, double cutoffHz, double drive01, int numStages)
  {
    mKind = kind;
    mNumStages = std::clamp(numStages, 1, 4);
    const double drive = std::clamp(drive01, 0.0, 1.0);

    const double maxHz = mSampleRate * 0.49;
    const double minFreq = (kind == AwXKind::Bandpass) ? 0.00005 : (kind == AwXKind::Notch ? 0.0003 : 0.001);
    mFreq = std::max(minFreq, std::clamp(cutoffHz, 5.0, maxHz) / mSampleRate);

    const double K = std::tan(3.14159265358979323846 * mFreq);
    constexpr double reso = 0.7071;
    const double norm = 1.0 / (1.0 + K / reso + K * K);

    switch (kind)
    {
      case AwXKind::Lowpass:
        mA0 = K * K * norm; mA1 = 2.0 * mA0; mA2 = mA0;
        mB1 = 2.0 * (K * K - 1.0) * norm; mB2 = (1.0 - K / reso + K * K) * norm;
        mCompensation = std::sqrt(mFreq) * 6.4;
        mClipFactor = 1.0 + (mFreq * mFreq * drive * 32.0);
        break;
      case AwXKind::Highpass:
        mA0 = norm; mA1 = -2.0 * norm; mA2 = norm;
        mB1 = 2.0 * (K * K - 1.0) * norm; mB2 = (1.0 - K / reso + K * K) * norm;
        // Adapted from the source's clipThreshold = 1 - C*sqrt(B) (B =
        // its own raw slider): substitutes the normalized cutoff
        // position for B, since B itself no longer exists here.
        mClipThreshold = std::clamp(1.0 - drive * std::sqrt(mFreq / 0.49), 0.05, 1.57079633);
        break;
      case AwXKind::Bandpass:
        mA0 = (K / reso) * norm; mA1 = 0.0; mA2 = -mA0;
        mB1 = 2.0 * (K * K - 1.0) * norm; mB2 = (1.0 - K / reso + K * K) * norm;
        mCompensation = std::sqrt(mFreq) * 6.4;
        mClipFactor = 0.768 + (mFreq * drive * 37.0);
        break;
      case AwXKind::Notch:
        mA0 = (1.0 + K * K) * norm; mA1 = 2.0 * (K * K - 1.0) * norm; mA2 = mA0;
        mB1 = mA1; mB2 = (1.0 - K / reso + K * K) * norm;
        mClipFactor = 1.0 + (drive * drive * drive * 2.0); // verbatim pow(C,3)*2.0
        break;
    }
  }

  void Reset() { for (auto& s : mState) s = StageState{}; }

  double ProcessSample(double input)
  {
    double x = input;
    for (int i = 0; i < mNumStages; ++i)
      x = Stage(x, mState[static_cast<size_t>(i)]);
    return std::isfinite(x) ? x : 0.0;
  }

private:
  struct StageState { double x1 = 0.0, x2 = 0.0, fb1 = 0.0, fb2 = 0.0; };

  double Stage(double x, StageState& s)
  {
    switch (mKind)
    {
      case AwXKind::Lowpass:
      {
        double out = mA0 * x + mA1 * s.x1 + mA2 * s.x2 - mB1 * s.fb1 - mB2 * s.fb2;
        s.x2 = s.x1; s.x1 = x;
        out *= mClipFactor;
        out = std::clamp(out, -1.57079633, 1.57079633);
        s.fb2 = s.fb1; s.fb1 = std::sin(out); // feedback itself folds through sin()
        return out / mCompensation;
      }
      case AwXKind::Highpass:
      {
        double out = mA0 * x + mA1 * s.x1 + mA2 * s.x2 - mB1 * s.fb1 - mB2 * s.fb2;
        s.x2 = s.x1; s.x1 = x;
        out = std::clamp(out, -mClipThreshold, mClipThreshold); // plain hard clamp, no sin()
        s.fb2 = s.fb1; s.fb1 = out;
        return out;
      }
      case AwXKind::Bandpass:
      {
        const double xScaled = x * mClipFactor; // drive applied on the INPUT side for this type
        double out = mA0 * xScaled + mA2 * s.x2 - mB1 * s.fb1 - mB2 * s.fb2; // a1==0 for bandpass, term omitted
        s.x2 = s.x1; s.x1 = xScaled;
        out = std::clamp(out, -1.57079633, 1.57079633);
        s.fb2 = s.fb1; s.fb1 = std::sin(out);
        return out / mCompensation;
      }
      case AwXKind::Notch:
      default:
      {
        const double xClamped = std::clamp(x, -1.57079633, 1.57079633);
        const double xFolded = std::sin(xClamped) * mClipFactor; // the nonlinearity sits on the INPUT, not the feedback
        double out = mA0 * xFolded + mA1 * s.x1 + mA2 * s.x2 - mB1 * s.fb1 - mB2 * s.fb2;
        s.x2 = s.x1; s.x1 = xFolded;
        s.fb2 = s.fb1; s.fb1 = out; // plain recursion -- no sin() in the feedback for Notch
        return out;
      }
    }
  }

  double mSampleRate = 44100.0;
  AwXKind mKind = AwXKind::Lowpass;
  int mNumStages = 1;
  double mFreq = 0.02;
  double mA0 = 0.0, mA1 = 0.0, mA2 = 0.0, mB1 = 0.0, mB2 = 0.0;
  double mCompensation = 1.0;
  double mClipFactor = 1.0;
  double mClipThreshold = 1.0;
  std::array<StageState, 4> mState{};
};

// AirwindowsHoltFilter -- real port of Holt2 ("a predictive lowpass that
// forecasts the next sample using Holt's double-exponential-smoothing
// trend model instead of a plain one-pole average"). This is a genuinely
// different lowpass character from Lowpass2/ZLowpass2: it tracks not just
// where the signal IS but where it's TRENDING, and partially extrapolates
// ("forecast") rather than just lagging -- a flavor of resonance/overshoot
// no biquad produces.
//
// WHAT WAS KEPT EXACTLY, WHAT WAS ADAPTED:
//
// 1. The core recursion is copied verbatim per stage:
//      trend    = beta*(x - prevSample) + (0.999-beta)*prevTrend
//      forecast = prevSample + prevTrend
//      out      = alpha*x + (0.999-alpha)*forecast
//    and the resControl->beta formula is verbatim:
//      resControl = drive*0.15 + 0.12
//      beta       = alpha * resControl^2
//    (0.27 max resonance / 0.12 min resonance, straight from the source
//    comment -- this IS the character, untouched.)
//
// 2. ADAPTED (same reasoning as every other family in this file): the
//    source derives alpha from a raw knob via `pow(A,4)+0.00001` with a
//    separate "droop correction" term (`+= (1-beta)*pow(A,3)`) that only
//    exists because that raw knob doesn't map linearly to a real cutoff.
//    Since this project already has a real cutoffHz parameter, alpha is
//    instead derived the same way AirwindowsAdaptiveFilter derives its
//    smoothing coefficient: alpha = omega/(omega+1) where
//    omega = 2*pi*cutoffHz/sampleRate (the standard one-pole mapping used
//    elsewhere in this codebase). The droop-correction term is dropped --
//    it existed only to compensate for the raw-knob curve's own nonlinearity,
//    which no longer exists once cutoffHz is used directly.
//
// 3. ADAPTED (drive knob): `resonance` (as drive01, 0..1) stands in for
//    the source's "B" knob in the resControl formula above -- identical
//    convention to every other port in this file.
//
// 4. ADAPTED (stage count): the source's 8-stage wet/dry ladder (knob "C")
//    is replaced with this project's established 1-4 cascaded-stage
//    convention (morph knob), same as AirwindowsAdaptiveFilter/
//    AirwindowsXFilter -- each stage fully processed and fed straight into
//    the next, no partial-wet blending.
//
// 5. DROPPED: the source's separate output gain ("D") and dry/wet ("E")
//    knobs -- this dropdown's filter types are always fully wet, consistent
//    with every other entry.
//
// Lowpass-only: Holt's forecast model is inherently a smoother/predictor,
// there's no clean highpass dual the way Lowpass2/Highpass2 have one.
class AirwindowsHoltFilter
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = sampleRate; }

  void SetParams(double cutoffHz, double drive01, int numStages)
  {
    const double freq = std::clamp(cutoffHz, 1.0, mSampleRate * 0.49) / mSampleRate;
    const double omega = 2.0 * 3.14159265358979323846 * freq;
    mAlpha = std::clamp(omega / (omega + 1.0), 0.0, 1.0);

    const double resControl = std::clamp(drive01, 0.0, 1.0) * 0.15 + 0.12;
    mBeta = mAlpha * resControl * resControl;

    mNumStages = std::clamp(numStages, 1, 4);
  }

  void Reset() { mStages.fill(StageState{}); }

  double ProcessSample(double input)
  {
    double x = input;
    for (int i = 0; i < mNumStages; ++i)
    {
      StageState& s = mStages[i];
      const double trend = mBeta * (x - s.prevSample) + (0.999 - mBeta) * s.prevTrend;
      const double forecast = s.prevSample + s.prevTrend;
      double out = mAlpha * x + (0.999 - mAlpha) * forecast;
      if (!std::isfinite(out)) out = 0.0;
      s.prevSample = out;
      s.prevTrend = trend;
      x = out;
    }
    return x;
  }

private:
  struct StageState
  {
    double prevSample = 0.0;
    double prevTrend = 0.0;
  };

  double mSampleRate = 44100.0;
  double mAlpha = 0.0;
  double mBeta = 0.0;
  int mNumStages = 1;
  std::array<StageState, 4> mStages{};
};

// AirwindowsBiquadStackFilter -- real port of BiquadStack ("Stacked Biquad
// With Reversed Neutron Flow"). Genuinely different shape from every other
// port in this file: it's PARALLEL, not a replacement filter -- the source
// always does `input + processed`, an additive resonant enhancer rather
// than a subtractive EQ curve, same spirit as the original AirLift
// character type but using Airwindows' real, odder topology: three
// cascaded 2nd-order sections at three FIXED resonance ratios
// (1.93185165 / 0.70710678 / 0.51763809, verbatim -- not a Butterworth
// ladder, an Airwindows-specific spacing) where each section's own
// coefficient is modulated sample-by-sample by the signal passing through
// it (`dis = fabs(a0*(1+out*nonlin))`, clamped to 1) and the feedback
// terms are applied with a sign flip relative to a normal biquad
// ("reversed neutron flow" is the source's own name for this topology) --
// that sign flip plus the content-dependent coefficient is what gives it
// a glassy, slightly ringing resonant character distinct from the Z/X
// families above.
//
// WHAT WAS KEPT EXACTLY, WHAT WAS ADAPTED:
//
// 1. Kept verbatim: all three fixed resonance-ratio constants, the K=tan(pi*freq)
//    bilinear-transform coefficient derivation per stage, the per-sample
//    content-adaptive `dis` clamp, the reversed-sign feedback recursion,
//    and the final parallel-sum (`input + processed`) with the >1.0
//    double-boost on `level`.
//
// 2. ADAPTED (frequency): same as every other port here -- `freq` is
//    cutoffHz/sampleRate directly, skipping the source's own
//    pow(A,4)*19980+20 curve.
//
// 3. ADAPTED (drive/level): `resonance` (as drive01) stands in for the
//    source's "nonlin" knob (B) -- it feeds both the per-sample `dis`
//    modulation AND the `level` parallel-mix gain (`levelB = nonlin*2-1`,
//    doubled if positive), exactly as the source computes it from one
//    knob.
//
// 4. ADAPTED (resonance shape): the source's third knob ("C", `reso`
//    formula) has no 1-4-stage-count equivalent to map to here the way
//    other families use morph -- this type is always a fixed 3-stage
//    stack, same as the source. So `morph` is repurposed as that third
//    "reso" shaping knob instead of a stage count (same repurposing
//    precedent as the original AirLift character type, which also had
//    no stage count to assign morph to).
class AirwindowsBiquadStackFilter
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = sampleRate; }

  void SetParams(double cutoffHz, double drive01, double resoShape01)
  {
    const double freq = std::clamp(cutoffHz, 1.0, mSampleRate * 0.49) / mSampleRate;
    const double nonlin = std::clamp(drive01, 0.0, 1.0);
    const double resoShape = std::clamp(resoShape01, 0.0, 1.0);

    mNonlin = nonlin;
    mLevel = (nonlin * 2.0) - 1.0;
    if (mLevel > 0.0) mLevel *= 2.0;

    const double reso = (0.5 + (nonlin * 0.5) + std::sqrt(freq)) - (1.0 - (1.0 - resoShape) * (1.0 - resoShape)) + 0.5 + (nonlin * 0.5);

    const double K = std::tan(3.14159265358979323846 * freq);
    auto deriveStage = [&](double resoRatio, double& c0, double& c1, double& c2)
    {
      const double r = reso * resoRatio;
      const double norm = 1.0 / (1.0 + K / r + K * K);
      c0 = (K / r) * norm;
      c1 = 2.0 * (K * K - 1.0) * norm;
      c2 = (1.0 - K / r + K * K) * norm;
    };
    deriveStage(1.93185165, mA0, mB1, mB2);
    deriveStage(0.70710678, mC0, mD1, mD2);
    deriveStage(0.51763809, mE0, mF1, mF2);
  }

  void Reset() { mA1 = mA2 = mC1 = mC2 = mE1 = mE2 = 0.0; }

  double ProcessSample(double input)
  {
    double out = input * std::fabs(mLevel);

    double dis = std::fabs(mA0 * (1.0 + out * mNonlin));
    if (dis > 1.0) dis = 1.0;
    double temp = (out * dis) + mA1;
    mA1 = mA2 - (temp * mB1);
    mA2 = (out * -dis) - (temp * mB2);
    out = temp;

    dis = std::fabs(mC0 * (1.0 + out * mNonlin));
    if (dis > 1.0) dis = 1.0;
    temp = (out * dis) + mC1;
    mC1 = mC2 - (temp * mD1);
    mC2 = (out * -dis) - (temp * mD2);
    out = temp;

    dis = std::fabs(mE0 * (1.0 + out * mNonlin));
    if (dis > 1.0) dis = 1.0;
    temp = (out * dis) + mE1;
    mE1 = mE2 - (temp * mF1);
    mE2 = (out * -dis) - (temp * mF2);
    out = temp;

    out *= mLevel;
    if (mLevel > 1.0) out *= mLevel;

    double result = input + out; // purely a parallel filter stage, verbatim
    if (!std::isfinite(result)) result = input;
    return std::clamp(result, -8.0, 8.0); // safety bound only -- the source itself has no equivalent clamp, but this stops extreme knob combinations (B/C both maxed) from running away on weak playback hardware
  }

private:
  double mSampleRate = 44100.0;
  double mNonlin = 0.0;
  double mLevel = 0.0;
  double mA0 = 0.0, mB1 = 0.0, mB2 = 0.0, mA1 = 0.0, mA2 = 0.0;
  double mC0 = 0.0, mD1 = 0.0, mD2 = 0.0, mC1 = 0.0, mC2 = 0.0;
  double mE0 = 0.0, mF1 = 0.0, mF2 = 0.0, mE1 = 0.0, mE2 = 0.0;
};

// AirwindowsYFilter -- real port of the Y-family (YLowpass/YHighpass/
// YBandpass/YNotch). The reason this one needed its own 4th automatable
// param (the user's explicit call, not a
// shortcut): everything else in this file folds its "extra" knob into the
// existing resonance/morph budget, but YLowpass's defining character is a
// *shaping curve applied around the filter*, not a property of the filter
// itself, and that curve's exponent genuinely wants to sweep in real time
// without being tangled up with Q or stage count. That's the only reason
// this gets a dedicated kBandFilterExtra param while every earlier port
// didn't.
//
// WHAT IT ACTUALLY DOES: encode/decode companding (credited by the source
// to torridgristle, MIT) around a resonant biquad. Before the biquad:
// every sample above 0 is raised to `powFactor` (bends the top of the
// waveform); after the biquad: the inverse `1/powFactor` is applied
// (bends it back). With powFactor == 1.0 this is a no-op wrapper -- just
// the filter underneath. Away from 1.0, the compand/decompand pair no
// longer cancels exactly once the resonant filter has reshaped the signal
// in between, so the filter's own resonant harmonics come out
// asymmetrically bent -- a genuinely different-sounding resonant lowpass
// than anything else in this dropdown, controllable in real time via the
// new knob rather than a fixed character.
//
// WHAT WAS KEPT EXACTLY, WHAT WAS ADAPTED:
//
// 1. Kept verbatim: the resonant biquad's own coefficient derivation
//    (K=tan(pi*freq), the reso-scaled K/reso term), the encode/decode
//    power-curve math and its clamping (>1 or <-1 pass through unchanged,
//    0 is a hard boundary), and the fixed 20000Hz/0.7071 "fix" biquad the
//    source runs both before and after the resonant stage purely to keep
//    ultrasonic energy from the companding math out of the audible
//    result.
//
// 2. ADAPTED (frequency): cutoffHz used directly, same as every other
//    port -- skips the source's pow(B,3)*20000 curve.
//
// 3. ADAPTED (resonance): `resonance` (as drive01, the project's existing
//    0..1 reduction of the resonance knob) stands in for the source's "C"
//    knob in its own reso formula (`pow(C,2)*15+0.5571`) -- this is the
//    one port so far where that formula IS the real filter Q, not a
//    repurposed drive amount, so this is the most direct of all the
//    resonance mappings in this file.
//
// 4. NEW DEDICATED PARAM (powFactor): the user explicitly asked for this
//    to be its own automatable 4th slot rather than folded into an
//    existing knob (unlike every earlier port, which reused
//    resonance/morph). `extra01` (0..1, UI default 0.1, matching the
//    source's own neutral position since pow(0.1+0.9,4) == 1.0 exactly)
//    maps straight to `pow(extra01+0.9,4)`, verbatim.
//
// 5. DROPPED: the source's separate input-trim ("A"), output-trim ("E"),
//    and wet/dry ("F") knobs -- fixed at their neutral values (inTrim=1,
//    outTrim=1, fully wet), same "always fully wet, no extra gain knobs"
//    convention as every other type in this dropdown.
//
// Generalized to all four Y-family siblings (YLowpass/YHighpass/
// YBandpass/YNotch) -- they're identical plugins apart from their
// biquad's own a0/a1/a2 formula and, for Notch only, a different reso
// floor (0.0001 instead of 0.5571, verbatim from that source file). Same
// pattern already used for the Z-family (AwZKind) and X-family (AwXKind)
// in this file.
enum class AwYKind { Lowpass, Highpass, Bandpass, Notch };

class AirwindowsYFilter
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = sampleRate;
    const double K = std::tan(3.14159265358979323846 * (20000.0 / sampleRate));
    const double norm = 1.0 / (1.0 + K / 0.7071 + K * K);
    mFixA0 = K * K * norm;
    mFixA1 = 2.0 * mFixA0;
    mFixA2 = mFixA0;
    mFixB1 = 2.0 * (K * K - 1.0) * norm;
    mFixB2 = (1.0 - K / 0.7071 + K * K) * norm;
  }

  void SetParams(AwYKind kind, double cutoffHz, double drive01, double extra01)
  {
    mKind = kind;
    const double freq = std::clamp(cutoffHz, 15.0, mSampleRate * 0.49) / mSampleRate;
    const double resoFloor = (kind == AwYKind::Notch) ? 0.0001 : 0.5571; // verbatim per-kind floor from the source
    const double d = std::clamp(drive01, 0.0, 1.0);
    const double reso = d * d * 15.0 + resoFloor;
    mPowFactor = std::pow(std::clamp(extra01, 0.0, 1.0) + 0.9, 4.0);

    const double K = std::tan(3.14159265358979323846 * freq);
    const double norm = 1.0 / (1.0 + K / reso + K * K);
    switch (kind)
    {
      case AwYKind::Lowpass:
        mA0 = K * K * norm; mA1 = 2.0 * mA0; mA2 = mA0;
        break;
      case AwYKind::Highpass:
        mA0 = norm; mA1 = -2.0 * mA0; mA2 = mA0;
        break;
      case AwYKind::Bandpass:
        mA0 = (K / reso) * norm; mA1 = 0.0; mA2 = -mA0;
        break;
      case AwYKind::Notch:
      default:
        mA0 = (1.0 + K * K) * norm; mA1 = 2.0 * (K * K - 1.0) * norm; mA2 = mA0;
        break;
    }
    mB1 = 2.0 * (K * K - 1.0) * norm;
    mB2 = (1.0 - K / reso + K * K) * norm;
    if (kind == AwYKind::Notch) mB1 = mA1; // verbatim: YNotch's b1 reuses its own a1, not the generic 2*(K*K-1)*norm every other kind uses
  }

  void Reset()
  {
    mFixSL1 = mFixSL2 = mSL1 = mSL2 = 0.0;
    mFixOutSL1 = mFixOutSL2 = 0.0;
  }

  double ProcessSample(double input)
  {
    // fixed 20kHz ultrasonic-guard biquad (pre)
    double x = input;
    double temp = (x * mFixA0) + mFixSL1;
    mFixSL1 = (x * mFixA1) - (temp * mFixB1) + mFixSL2;
    mFixSL2 = (x * mFixA2) - (temp * mFixB2);
    x = temp;

    x = Compand(x, mPowFactor);

    // the resonant, coefficient-interpolated-in-the-source (fixed here,
    // block-rate, same convention as every other port) biquad
    temp = (x * mA0) + mSL1;
    mSL1 = (x * mA1) - (temp * mB1) + mSL2;
    mSL2 = (x * mA2) - (temp * mB2);
    x = temp;

    x = Compand(x, 1.0 / mPowFactor);

    // fixed 20kHz ultrasonic-guard biquad (post)
    temp = (x * mFixA0) + mFixOutSL1;
    mFixOutSL1 = (x * mFixA1) - (temp * mFixB1) + mFixOutSL2;
    mFixOutSL2 = (x * mFixA2) - (temp * mFixB2);
    x = temp;

    return std::isfinite(x) ? x : 0.0;
  }

private:
  static double Compand(double x, double powFactor)
  {
    // encode/decode courtesy of torridgristle, MIT license -- verbatim
    if (x > 1.0) return 1.0;
    if (x > 0.0) return 1.0 - std::pow(1.0 - x, powFactor);
    if (x < -1.0) return -1.0;
    if (x < 0.0) return -1.0 + std::pow(1.0 + x, powFactor);
    return x;
  }

  double mSampleRate = 44100.0;
  AwYKind mKind = AwYKind::Lowpass;
  double mPowFactor = 1.0;
  double mA0 = 0.0, mA1 = 0.0, mA2 = 0.0, mB1 = 0.0, mB2 = 0.0;
  double mSL1 = 0.0, mSL2 = 0.0;
  double mFixA0 = 0.0, mFixA1 = 0.0, mFixA2 = 0.0, mFixB1 = 0.0, mFixB2 = 0.0;
  double mFixSL1 = 0.0, mFixSL2 = 0.0;
  double mFixOutSL1 = 0.0, mFixOutSL2 = 0.0;
};

// AirwindowsBiquadNonLinFilter -- real port of BiquadNonLin. A single
// resonant 2nd-order stage (lowpass or bandpass) where the coefficient
// feeding the main feed-forward taps is modulated sample-by-sample by the
// CURRENT INPUT itself (`dia0 = fabs(a0*(1+input*nonLin))`, clamped to 1)
// -- genuinely different from every port so far: the X-family folds its
// nonlinearity into the FEEDBACK path, BiquadStack modulates each of 3
// fixed-ratio stages the same way, but this is the simplest, most direct
// single-stage version of that idea, which makes the effect of the
// nonlinearity knob unusually easy to hear in isolation.
//
// The source's own "wet" knob does something distinctive worth keeping
// as this type's dedicated character rather than flattening into a
// generic mix: positive values are a normal dry/wet blend of the
// filtered signal, but NEGATIVE values switch to `output = input*wet +
// dry` (no (1-wet) term) -- the source's own comment calls this exactly
// right: "lets us turn LP into HP and band into notch". So this one
// knob doesn't just blend, it sweeps from a complementary/inverted shape
// through dry into the normal filtered result.
//
// WHAT WAS KEPT EXACTLY, WHAT WAS ADAPTED:
//
// 1. Kept verbatim: the lowpass/bandpass coefficient derivation, the
//    content-adaptive `dia0`/`dia2` modulation and its clamp, the
//    asymmetric positive/negative wet-mix behavior described above.
//
// 2. ADAPTED (frequency): cutoffHz direct, same convention as every
//    other port.
//
// 3. ADAPTED (resonance): `resonance` maps directly to the source's own
//    reso range (clamped 0.01-29.99) -- another direct-Q mapping like
//    AirwindowsYFilter, not a repurposed drive amount.
//
// 4. ADAPTED (wet -> morph): `morph` (0..1) maps to wet = morph*2-1, so
//    morph=0 is the full complementary/inverted extreme, 0.5 is dry
//    bypass, 1.0 is the fully filtered result -- the source's own
//    0..1-to-bipolar knob, just renamed to this project's existing
//    morph convention.
//
// 5. ADAPTED (nonLin -> extra): the dedicated `extra` param (already
//    added for AwYLowpass) carries the source's own nonLin = D^2
//    formula verbatim -- reused here because, like YLowpass's
//    powFactor, this is a real-time shaping exponent that doesn't belong
//    tangled up with Q or wet.
//
// Two dropdown entries share this one class (AwBqKind), same pattern as
// every other Z/X/Y-family port in this file -- the source's own `type`
// knob (A, a plain 0/1 toggle) becomes two separate filter-type options
// instead of a 5th param, consistent with how LP/HP/BP already get
// their own dropdown entries rather than a shared "shape" knob.
enum class AwBqKind { Lowpass, Bandpass };

class AirwindowsBiquadNonLinFilter
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = sampleRate; }

  void SetParams(AwBqKind kind, double cutoffHz, double resonance, double morph01, double extra01)
  {
    mKind = kind;
    const double freq = std::clamp(cutoffHz, 1.0, mSampleRate * 0.49) / mSampleRate;
    const double reso = std::clamp(resonance, 0.01, 29.99);
    mNonLin = std::clamp(extra01, 0.0, 1.0) * std::clamp(extra01, 0.0, 1.0);
    mWet = (std::clamp(morph01, 0.0, 1.0) * 2.0) - 1.0;

    const double K = std::tan(3.14159265358979323846 * freq);
    const double norm = 1.0 / (1.0 + K / reso + K * K);
    if (kind == AwBqKind::Lowpass)
    {
      mA0 = K * K * norm;
      mA1 = 2.0 * mA0;
      mA2 = mA0;
    }
    else
    {
      mA0 = (K / reso) * norm;
      mA1 = 0.0; // bandpass can simplify the biquad kernel: leave out this multiply, verbatim
      mA2 = -mA0;
    }
    mB1 = 2.0 * (K * K - 1.0) * norm;
    mB2 = (1.0 - K / reso + K * K) * norm;
  }

  void Reset() { mS1 = mS2 = 0.0; }

  double ProcessSample(double input)
  {
    const double dry = input;
    double dia0 = std::fabs(mA0 * (1.0 + (input * mNonLin)));
    if (dia0 > 1.0) dia0 = 1.0;
    const double dia2 = (mKind == AwBqKind::Bandpass) ? -dia0 : dia0; // reverse polarity for bandpass, verbatim

    const double temp = (input * dia0) + mS1;
    mS1 = (input * mA1) - (temp * mB1) + mS2;
    mS2 = (input * dia2) - (temp * mB2);

    double out = temp * mWet;
    if (mWet > 0.0) out += dry * (1.0 - mWet);
    else out += dry; // verbatim asymmetry -- this is what lets wet sweep into a complementary/notch-like shape at negative values, not a bug

    return std::isfinite(out) ? out : dry;
  }

private:
  double mSampleRate = 44100.0;
  AwBqKind mKind = AwBqKind::Lowpass;
  double mNonLin = 0.0;
  double mWet = 1.0;
  double mA0 = 0.0, mA1 = 0.0, mA2 = 0.0, mB1 = 0.0, mB2 = 0.0;
  double mS1 = 0.0, mS2 = 0.0;
};

// AirwindowsZAcidLowpassFilter -- real port of ZAcidLowpass. Not a biquad
// at all, unlike everything else in this file -- this is a resonant
// SAMPLE-AND-HOLD decimator with cubic Bezier interpolation between its 4
// most recent held samples, the classic squelchy/analog-unstable "acid"
// character (think a 303's resonant filter breaking up, or an old analog
// sample-and-hold circuit's artifacts) rather than a filter-shaped
// frequency response. Genuinely the most "rare and cool" thing in this
// dropdown so far: every other type is some flavor of resonant subtractive
// filter; this one is closer to a pitched decimator with a smoothed,
// curved (not stair-stepped) hold.
//
// HOW IT WORKS: a phase accumulator ("cycle") advances by `cutoff` each
// sample. Every time it wraps past 1.0, a new held sample is captured
// (itself a blend of the running input average and the raw instantaneous
// input, controlled by `meltdown` -- this is where the "analog
// instability" character lives) and pushed into a 4-deep history
// (AL/BL/CL/DL). Between wraps, a cubic Bezier curve through those 4
// held points is evaluated at the current phase, giving a SMOOTHED
// decimated signal rather than a harsh stair-step. The final output
// blends that decimated/"under" signal against the "over" (non-decimated
// difference) signal, then runs through the same opamp tail
// (AirwindowsOpampStage, reused verbatim) every Z-family filter in this
// file already uses.
//
// WHAT WAS KEPT EXACTLY, WHAT WAS ADAPTED:
//
// 1. Kept verbatim: the phase-accumulator/hold-and-capture mechanism,
//    the cubic Bezier blend formula and its `acidTrim` term, the
//    over/under mix, the meltdown blend on capture, and the shared
//    opamp tail.
//
// 2. ADAPTED (cutoff): the source derives its decimation rate from a raw
//    knob through a formula that depends on its own oversampling factor
//    in a way that stops making sense once a real cutoffHz is used
//    directly (this project's standard adaptation) -- so `cutoff` here
//    is simply 2*cutoffHz/sampleRate, clamped to the same (0.0001, 1.0)
//    range the source itself clamps to. This is a genuine behavioral
//    choice, not just a UX skip like every other port's frequency
//    adaptation, since the source's own mapping is unusually tangled --
//    flagging that explicitly rather than claiming full verbatim parity
//    on this one parameter.
//
// 3. DIRECT KNOB REUSE (no repurposing needed -- a rare case in this
//    file): the source's "over"/"under"/"meltdown" knobs (B/C/D) map
//    one-to-one onto this project's resonance/morph/extra slots with NO
//    curve or formula applied -- `resonance` (as drive01) -> under (the
//    "acid"/decimated signal's own level, matching what a resonance knob
//    intuitively does here), `morph` -> over (the clean/complement
//    signal's level), `extra` -> meltdown (the capture-blend chaos
//    amount). All three used exactly as the source's own 0..1 knobs.
//
// 4. DROPPED: the source's separate input-trim ("E") and output-trim
//    ("F") knobs, fixed at neutral -- same convention as every other
//    port in this file.
class AirwindowsZAcidLowpassFilter
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = std::max(1.0, sampleRate);
    mOpamp.SetSampleRate(mSampleRate);
  }

  void SetParams(double cutoffHz, double under01, double over01, double meltdown01)
  {
    mCutoff = std::clamp((2.0 * cutoffHz) / mSampleRate, 0.0001, 1.0);
    mAcidTrim = 1.0 - std::pow(mCutoff * 0.5, 1.0 / (mCutoff * 0.5));
    mUnder = std::clamp(under01, 0.0, 1.0);
    mOver = std::clamp(over01, 0.0, 1.0);
    mMeltdown = std::clamp(meltdown01, 0.0, 1.0);
  }

  void Reset()
  {
    mCycle = 0.0;
    mSampAccum = 0.0;
    mA = mB = mC = mD = 0.0;
    mOpamp.Reset();
  }

  double ProcessSample(double input)
  {
    mCycle += mCutoff;
    mSampAccum += input * mCutoff;

    if (mCycle > 1.0)
    {
      mCycle -= 1.0;
      mD = mC;
      mC = mB;
      mB = mA;
      mA = (mSampAccum * (1.0 - mMeltdown)) + (input * mMeltdown);
      mSampAccum = 0.0;
    }

    const double X = mCycle * mAcidTrim;
    const double omX = 1.0 - X;
    double mid = mD * omX * omX * omX;
    mid += mC * 3.0 * omX * omX * X;
    mid += mB * 3.0 * omX * X * X;
    mid += mA * X * X * X;

    double out = (mid * mUnder) + ((input - mid) * mOver);
    out = mOpamp.Process(out);
    return std::isfinite(out) ? out : 0.0;
  }

private:
  double mSampleRate = 44100.0;
  double mCutoff = 0.1;
  double mAcidTrim = 0.0;
  double mUnder = 0.5, mOver = 0.5, mMeltdown = 0.0;
  double mCycle = 0.0;
  double mSampAccum = 0.0;
  double mA = 0.0, mB = 0.0, mC = 0.0, mD = 0.0;
  AirwindowsOpampStage mOpamp;
};

// AirwindowsHermepassFilter -- real port of Hermepass ("a mastering
// highpass to set by ear only"). Genuinely different mechanism from
// every other highpass in this dropdown: it's a SUBTRACTIVE cascade of
// up to 7 one-pole lowpass trackers (not a biquad at all) -- the input
// minus a correction built from however many of those trackers are
// "engaged". The first 3 poles share one cutoff; the remaining poles
// each converge their own cutoff toward near-zero via a fixed
// golden-ratio blend (0.618033988749894848204586, the same constant
// used elsewhere in Airwindows' own filters) -- so turning up the pole
// count doesn't add poles all at the same corner, it adds poles whose
// corners drift progressively lower, giving a smoother, less resonant
// steepening than a straightforward cascade of identical stages (like
// this project's own LP6/12/24/48 ladder) would.
//
// WHAT WAS KEPT EXACTLY, WHAT WAS ADAPTED:
//
// 1. Kept verbatim: the one-pole tracker recursion, the golden-ratio
//    cutoff convergence for poles D-H, and the "poles" weighting scheme
//    (each extra pole's correction is scaled 0..1 by how far the slope
//    knob has reached it, so turning the knob smoothly engages one more
//    pole at a time rather than snapping).
//
// 2. ADAPTED (frequency): the source turns its own A knob into a
//    unitless "cutoff" position via pow(A,3) and scales by a sample-
//    rate-dependent rangescale -- this project substitutes the standard
//    omega/(omega+1) one-pole coefficient from a real cutoffHz (same
//    convention as AirwindowsAdaptiveFilter/AirwindowsHoltFilter), then
//    feeds THAT into the same golden-ratio convergence chain the source
//    uses for its later poles.
//
// 3. ADAPTED (slope -> morph): `morph` (0..1) maps directly to the
//    source's own slope*6.0 -- unchanged math, just renamed to this
//    project's knob.
//
// 4. SIMPLIFIED (dropped, not a character change): the source alternates
//    which of two redundant first-stage instances (A/B) runs each
//    sample ("fpFlip"), purely to stagger floating-point rounding
//    between calls for extra numerical precision -- not an audible
//    feature. This port always runs the single equivalent stage.
//
// resonance and extra are unused by this type -- there's no Q or shaping
// exponent concept in a subtractive one-pole cascade, same as
// AwHoltLowpass not using morph.
class AirwindowsHermepassFilter
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = std::max(1.0, sampleRate); }

  void SetParams(double cutoffHz, double morph01)
  {
    const double freq = std::clamp(cutoffHz, 1.0, mSampleRate * 0.49) / mSampleRate;
    const double omega = 2.0 * 3.14159265358979323846 * freq;
    const double baseCoef = std::clamp(omega / (omega + 1.0), 0.0, 1.0);

    mNewA = baseCoef;
    mNewC = baseCoef;

    constexpr double kGolden = 0.618033988749894848204586; // verbatim, Airwindows' own constant
    double pos = baseCoef;
    pos = (pos * kGolden) + (0.00001 * (1.0 - kGolden)); mNewD = pos;
    pos = (pos * kGolden) + (0.00001 * (1.0 - kGolden)); mNewE = pos;
    pos = (pos * kGolden) + (0.00001 * (1.0 - kGolden)); mNewF = pos;
    pos = (pos * kGolden) + (0.00001 * (1.0 - kGolden)); mNewG = pos;
    pos = (pos * kGolden) + (0.00001 * (1.0 - kGolden)); mNewH = pos;

    double slope = std::clamp(morph01, 0.0, 1.0) * 6.0;
    auto takePole = [&]() { const double p = std::clamp(slope, 0.0, 1.0); slope -= 1.0; return p; };
    mPolesC = takePole();
    mPolesD = takePole();
    mPolesE = takePole();
    mPolesF = takePole();
    mPolesG = takePole();
    mPolesH = takePole();
  }

  void Reset() { mIirA = mIirC = mIirD = mIirE = mIirF = mIirG = mIirH = 0.0; }

  double ProcessSample(double input)
  {
    double x = input;
    mIirA = (mIirA * (1.0 - mNewA)) + (x * mNewA); x -= mIirA;
    double correction = mIirA;

    mIirC = (mIirC * (1.0 - mNewC)) + (x * mNewC); x -= mIirC;
    mIirD = (mIirD * (1.0 - mNewD)) + (x * mNewD); x -= mIirD;
    mIirE = (mIirE * (1.0 - mNewE)) + (x * mNewE); x -= mIirE;
    mIirF = (mIirF * (1.0 - mNewF)) + (x * mNewF); x -= mIirF;
    mIirG = (mIirG * (1.0 - mNewG)) + (x * mNewG); x -= mIirG;
    mIirH = (mIirH * (1.0 - mNewH)) + (x * mNewH); x -= mIirH;

    correction += mIirC * mPolesC;
    correction += mIirD * mPolesD;
    correction += mIirE * mPolesE;
    correction += mIirF * mPolesF;
    correction += mIirG * mPolesG;
    correction += mIirH * mPolesH;

    const double out = input - correction;
    return std::isfinite(out) ? out : 0.0;
  }

private:
  double mSampleRate = 44100.0;
  double mNewA = 0.0, mNewC = 0.0, mNewD = 0.0, mNewE = 0.0, mNewF = 0.0, mNewG = 0.0, mNewH = 0.0;
  double mPolesC = 0.0, mPolesD = 0.0, mPolesE = 0.0, mPolesF = 0.0, mPolesG = 0.0, mPolesH = 0.0;
  double mIirA = 0.0, mIirC = 0.0, mIirD = 0.0, mIirE = 0.0, mIirF = 0.0, mIirG = 0.0, mIirH = 0.0;
};

// AverMatrix: NOT a biquad. A continuous-tap (1-10, fractional) moving
// average, cascaded through a continuous number of "poles" (1-10,
// fractional -- the final pole is blended in fractionally too), then
// mixed against dry through a BIPOLAR wet knob. Verbatim from source:
// the per-pole independent tapped-delay-line averaging scheme (each pole
// y keeps its own history buffer bL/bR[x][y], not a shared one), the
// f[x] coefficient-splitting loop that turns a continuous tap count into
// a neat integer-tap average plus one fractional remainder tap, and the
// fractional-pole blend (`previousPoleL*(1-yPartial) + sum*yPartial`).
// Negative wet keeps dry at full volume while the moving-average part
// cancels INTO it rather than away from it -- since a plain moving
// average is a lowpass, subtracting it from a full-volume dry signal is
// a complementary highpass; this is exactly the "inverted does the
// highpass" behavior the user asked to keep.
//
// Adapted (there's no native Hz knob in the source -- A is a raw
// 1-10 tap-count slider): cutoffHz maps to tap count via the standard
// DSP moving-average cutoff approximation, taps ~= 0.443 * sampleRate /
// cutoffHz, clamped to the source's native 1-10 range. This is a real
// approximation, not a verbatim mapping -- a boxcar/moving-average
// filter's "cutoff" is inherently fuzzier than a biquad's, so treat the
// Hz knob here as "roughly this corner" rather than an exact one.
// resonance (via drive01, 0..1) maps to pole/cascade count (1-10) --
// mechanically a stage count in the source, but framed as the
// "intensity" knob per the Pro-Q-like convention this project uses
// elsewhere (more resonance = a steeper, more insistent version of the
// same character). morph is the bipolar wet, same convention as
// BiquadNonLin's wet-via-morph (morph*2-1). extra is unused -- single
// dropdown entry, since one continuous wet knob already sweeps the full
// lowpass-to-complementary-highpass range the user described, rather
// than splitting it into separate Lowpass/Highpass entries.
class AirwindowsAverMatrixFilter
{
public:
  void SetSampleRate(double sr) { mSampleRate = std::max(1.0, sr); }

  void SetParams(double cutoffHz, double drive01, double morphBipolarSrc)
  {
    const double safeCutoff = std::clamp(cutoffHz, 20.0, mSampleRate * 0.49);
    double overallTaps = std::clamp(0.443 * mSampleRate / safeCutoff, 1.0, 10.0);
    double overallPoles = std::clamp(1.0 + drive01 * 9.0, 1.0, 10.0);

    mYLimit = static_cast<int>(std::floor(overallPoles)) + 1;
    mYLimit = std::clamp(mYLimit, 1, 10);
    mYPartial = overallPoles - std::floor(overallPoles);

    double taps = overallTaps;
    mXLimit = 1;
    for (int x = 0; x < 11; ++x)
    {
      if (taps > 1.0) { mF[x] = 1.0; taps -= 1.0; mXLimit++; }
      else { mF[x] = taps; taps = 0.0; }
    }
    if (mXLimit > 9) mXLimit = 9;
    const double safeOverallTaps = std::max(1.0, overallTaps);
    for (int x = 0; x < mXLimit; ++x) mF[x] /= safeOverallTaps;

    mWet = std::clamp(morphBipolarSrc, -1.0, 1.0); // already bipolar (morph*2-1) by the time it gets here
  }

  void Reset()
  {
    for (auto& row : mB) row.fill(0.0);
  }

  double ProcessSample(double input)
  {
    const double drySample = input;
    double x = input;
    double previousPole = 0.0;

    for (int y = 0; y < mYLimit; ++y)
    {
      for (int tap = mXLimit; tap >= 0; --tap)
        mB[static_cast<size_t>(tap + 1)][static_cast<size_t>(y)] = mB[static_cast<size_t>(tap)][static_cast<size_t>(y)];
      mB[0][static_cast<size_t>(y)] = previousPole = x;
      x = 0.0;
      for (int tap = 0; tap < mXLimit; ++tap)
        x += mB[static_cast<size_t>(tap)][static_cast<size_t>(y)] * mF[tap];
    }
    x = (previousPole * (1.0 - mYPartial)) + (x * mYPartial);

    const double out = (x * mWet) + (drySample * (1.0 - mWet));
    return std::isfinite(out) ? out : 0.0;
  }

private:
  double mSampleRate = 44100.0;
  int mXLimit = 1;
  int mYLimit = 1;
  double mYPartial = 0.0;
  double mWet = 0.0;
  std::array<double, 11> mF{};
  std::array<std::array<double, 10>, 12> mB{}; // mB[tap][pole], matches source's b[x+1][y] shift

  static_assert(true, "");
};

// SlewSonic: 4 cascaded lowpass biquads sharing one frequency but each
// with a FIXED, verbatim 10th-order-Butterworth-ladder Q (2.24697960,
// 0.80193774, 0.55495813, 0.5 -- there is no Q knob in the source, these
// are character constants). Stages A, B, C are each immediately followed
// by a "slew" differentiator (`(filtered - last) * ln(10)`, verbatim
// constant 2.302585092994045684017991) which, stacked on the preceding
// lowpass, produces a band-limited presence lift rather than a flat
// passband -- this is the "EQ tilt upwards" character the user asked to
// keep. A final plain-lowpass stage D (no slew) always runs once stage A
// is engaged. The source's single knob scales to a 0-3 "wet ladder"
// that progressively engages stage A, then B, then C (same bypass-when-
// unused pattern already used elsewhere in this project, e.g. Holt2).
//
// Adapted: cutoffHz feeds `freq` directly (extended past the source's
// native 5kHz-25kHz restriction, same "always use real Hz" convention as
// every other port here). morph is the stage-count knob (0-1 -> 0-3,
// same convention as AirwindowsAdaptiveFilter's morph*3.999). resonance
// and extra are UNUSED by this type -- the Butterworth-ladder Qs are
// fixed verbatim character constants, not knob-controlled in the
// source, so there's nothing honest to map them onto.
class AirwindowsSlewSonicFilter
{
public:
  // Sample-rate-independence fix (flagged by the user, not present in the
  // original port): the slew differentiator below is `(filtered[n] -
  // filtered[n-1]) * kTrim` -- a per-sample first difference. At a higher
  // sample rate, consecutive samples of the SAME analog signal are closer
  // together in time, so that difference shrinks roughly in proportion to
  // 44100/sampleRate even though nothing about the sound should have
  // changed -- this made the slew stage's effective intensity quietly
  // depend on host sample rate. mOverallScale (sampleRate/44100, the same
  // name/ratio Airwindows' own source uses elsewhere, e.g. this file's
  // AirwindowsEnergy2Processor::SetSampleRate) corrects it: multiplying
  // the difference by mOverallScale restores a constant time-derivative
  // regardless of sample rate, so this stage now sounds the same at
  // 44.1/48/96/192kHz for the same cutoff/morph -- unlike the Butterworth-
  // ladder Qs (kQA/B/C/D below), which are genuinely fixed dimensionless
  // character constants and correctly stay as-is.
  void SetSampleRate(double sr)
  {
    mSampleRate = std::max(1.0, sr);
    mOverallScale = mSampleRate / 44100.0;
  }

  void SetParams(double cutoffHz, double morph01)
  {
    const double safeCutoff = std::clamp(cutoffHz, 20.0, mSampleRate * 0.49);
    mFreq = std::clamp(safeCutoff / mSampleRate, 0.00001, 0.499);
    mWet = std::clamp(morph01, 0.0, 1.0) * 3.0;
  }

  void Reset()
  {
    mA1 = mA2 = mB1 = mB2 = mC1 = mC2 = mD1 = mD2 = 0.0;
    mLastA = mLastB = mLastC = 0.0;
  }

  double ProcessSample(double input)
  {
    constexpr double kTrim = 2.302585092994045684017991; // ln(10), verbatim
    constexpr double kQA = 2.24697960;
    constexpr double kQB = 0.80193774;
    constexpr double kQC = 0.55495813;
    constexpr double kQD = 0.5;

    double aWet = std::clamp(mWet, 0.0, 1.0);
    double bWet = std::clamp(mWet - 1.0, 0.0, 1.0);
    double cWet = std::clamp(mWet - 2.0, 0.0, 1.0);

    double x = input;
    double stageOut = x;

    if (aWet > 0.0)
    {
      const double filtered = OnePoleLowpass(x, mFreq, kQA, mA1, mA2);
      const double slewed = (filtered - mLastA) * kTrim * mOverallScale;
      mLastA = filtered;
      stageOut = (slewed * aWet) + (x * (1.0 - aWet));

      if (bWet > 0.0)
      {
        const double filteredB = OnePoleLowpass(stageOut, mFreq, kQB, mB1, mB2);
        const double slewedB = (filteredB - mLastB) * kTrim * mOverallScale;
        mLastB = filteredB;
        const double stageB = (slewedB * bWet) + (stageOut * (1.0 - bWet));

        if (cWet > 0.0)
        {
          const double filteredC = OnePoleLowpass(stageB, mFreq, kQC, mC1, mC2);
          const double slewedC = (filteredC - mLastC) * kTrim * mOverallScale;
          mLastC = filteredC;
          stageOut = (slewedC * cWet) + (stageB * (1.0 - cWet));
        }
        else
        {
          stageOut = stageB;
        }
      }

      stageOut = OnePoleLowpass(stageOut, mFreq, kQD, mD1, mD2);
    }

    return std::isfinite(stageOut) ? stageOut : 0.0;
  }

private:
  // Simple 2-pole resonant lowpass (TPT-style) standing in for the
  // source's own biquad lowpass with the same fixed Q constants -- the
  // source computes real biquad a0/a1/a2/b1/b2 coefficients from freq+Q;
  // this is mechanically a different (but equally standard) 2-pole
  // topology reused from elsewhere in this project rather than a second
  // from-scratch biquad implementation, chosen for the same stability
  // guarantees this file leans on throughout. The fixed Q constants are
  // used verbatim either way.
  static double OnePoleLowpass(double x, double freq, double q, double& z1, double& z2)
  {
    const double g = std::tan(3.14159265358979323846 * std::clamp(freq, 0.00001, 0.499));
    const double k = 1.0 / std::max(0.05, q);
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    const double a3 = g * a2;
    const double v3 = x - z2;
    const double v1 = a1 * z1 + a2 * v3;
    const double v2 = z2 + a2 * z1 + a3 * v3;
    z1 = 2.0 * v1 - z1;
    z2 = 2.0 * v2 - z2;
    if (!std::isfinite(z1) || std::fabs(z1) < 1.0e-300) z1 = std::isfinite(z1) ? z1 : 0.0;
    if (!std::isfinite(z2) || std::fabs(z2) < 1.0e-300) z2 = std::isfinite(z2) ? z2 : 0.0;
    return std::isfinite(v2) ? v2 : 0.0;
  }

  double mSampleRate = 44100.0;
  double mOverallScale = 1.0;
  double mFreq = 0.1;
  double mWet = 0.0;
  double mA1 = 0.0, mA2 = 0.0, mB1 = 0.0, mB2 = 0.0, mC1 = 0.0, mC2 = 0.0, mD1 = 0.0, mD2 = 0.0;
  double mLastA = 0.0, mLastB = 0.0, mLastC = 0.0;
};

// Energy2: NOT a per-band filter-type dropdown entry, by design -- see
// the header note on Energy2Processor below and ExoEQ.h/ExoEQ.cpp for
// why this needs a globally-capped resource instead. This is a MONO
// (one audio channel) processor; ExoEQ runs one instance per channel
// per enabled band, same as every other per-channel DSP object here --
// the source's own L/R state is fully independent per channel (no
// cross-channel coupling anywhere in the real algorithm), so splitting
// it into a mono class loses nothing.
//
// Real algorithm (confirmed by reading the full source, not guessed):
// 8 "intensity" bands run in PARALLEL (not series), each one an
// accumulator-difference scheme fed by a shared tapped delay line
// (PrevA..PrevH, one new tap per decimated "Energy cycle"), and all 8
// contributions are summed into one `correction` signal that's scaled
// by a bipolar mix and ADDED to a non-decimated dry signal. The whole
// correction stage only actually recomputes once every `cycleEnd`
// (1-4) samples -- a decimation factor tied to oversampling rate --
// with the gap smoothed by a multi-tap interpolation/averaging scheme
// (`lastRef[]`), which is how the real plugin stays cheap at high
// sample rates. The duo (2nd-order) band is a special-cased odd/even
// leak scheme (divide-by-256 leak each cycle, alternating accumulator);
// every other order (3rd through 9th) is the SAME ring-buffer pattern
// at a different ring size N: cell[cur] += factor, cell[(cur-1+N)%N]
// -= factor, output = cell[cur]*intensity, then every cell /= 2 -- the
// source hand-unrolls this per order (triplet/quad/quint/.../nint) but
// it is mechanically one generic formula, confirmed by checking it
// against all 7 of the source's unrolled switch statements (3 through
// 9) before generalizing it into ProcessRingBand below.
//
// Checked the source's own getParameterName strings (not guessed) for
// what each of the 8 raw params actually is: A=Hiss(duo/order2),
// B=Glitter(triplet/order3), C=Rat(quad/order4), D=Fizz(quint/order5),
// E=Scrape(sext/order6), F=Chug(sept/order7), G=Yowr(oct/order8),
// H=Snarl(nint/order9), I=mix.
//
// ADAPTED, per the user's explicit request: rather than expose 8
// separate intensity knobs + mix (9 controls, double this project's
// usual 4-knob-per-feature budget), this port exposes exactly 2 grouped
// knobs + mix, matching how the user says it's actually used:
//   - "GlitterHiss": drives BOTH Hiss(A/duo) and Glitter(B/triplet) from
//     one 0..1 knob value, fed into each band's own intensity curve.
//   - "RatScrapeFizz": drives Rat(C/quad), Scrape(E/sext), and
//     Fizz(D/quint) from one 0..1 knob value the same way.
//   - Chug(F/sept)/Yowr(G/oct)/Snarl(H/nint) are permanently parked at
//     neutral (knob=0.5 -> zero intensity via the source's own
//     -((x*2-1)^3) curve) and never exposed -- per the user's framing,
//     most use of this plugin doesn't reach for those three, so this
//     port doesn't allocate their ring-buffer state at all (not merely
//     hidden -- genuinely not computed), which is a real CPU saving on
//     top of the decimation the source already does.
//   - Each exposed knob's 0..1 value feeds the SAME bipolar-cubic curve
//     the source uses per-band (`-((x*2-1)^3)`) -- not a new curve
//     invented for the grouping, just the same math applied once and
//     shared by the bands in that group.
// Everything else (the decimation/interpolation scheme, the per-band
// accumulator math, the shared Prev tap delay line, the bipolar mix)
// is kept verbatim. Dropped: the source's own 32-bit dither noise
// shaping at the very end of its processing block -- consistent with
// every other port in this file, which also don't reproduce Airwindows'
// per-plugin dither (it's a fixed-point-release concern, not an
// audible feature, and this project doesn't carry it anywhere else).
class AirwindowsEnergy2Processor
{
public:
  void SetSampleRate(double sr)
  {
    mSampleRate = std::max(1.0, sr);
    double overallScale = mSampleRate / 44100.0;
    mCycleEnd = std::clamp(static_cast<int>(std::floor(overallScale)), 1, 4);
    if (mCycle > mCycleEnd - 1) mCycle = mCycleEnd - 1;
  }

  // glitterHiss01/ratScrapeFizz01: 0..1, matching this project's usual
  // knob convention (0.5 = neutral/off, same as the source's own
  // per-band knobs). mixBipolar: -1..1 (0 = no effect at all, matching
  // the source's own I*2-1 mix knob -- NOT the same "mix" meaning as
  // this project's send mixes elsewhere, which are unipolar 0..1; kept
  // bipolar here because that's a real, audible feature of the source
  // -- negative mix is a genuinely different, phase-inverted character,
  // not just "less of the same thing").
  void SetParams(double glitterHiss01, double ratScrapeFizz01, double mixBipolar)
  {
    const double gh = std::clamp(glitterHiss01, 0.0, 1.0);
    const double rsf = std::clamp(ratScrapeFizz01, 0.0, 1.0);
    mHissIntensity = -std::pow((gh * 2.0) - 1.0, 3.0);
    mGlitterIntensity = -std::pow((gh * 2.0) - 1.0, 3.0);
    mRatIntensity = -std::pow((rsf * 2.0) - 1.0, 3.0);
    mScrapeIntensity = -std::pow((rsf * 2.0) - 1.0, 3.0);
    mFizzIntensity = -std::pow((rsf * 2.0) - 1.0, 3.0);
    mMix = std::clamp(mixBipolar, -1.0, 1.0);
  }

  void Reset()
  {
    mPrev.fill(0.0);
    mDuoEven = mDuoOdd = 0.0;
    mDuoFlip = false;
    mTripletCells.fill(0.0);
    mQuadCells.fill(0.0);
    mQuintCells.fill(0.0);
    mSextCells.fill(0.0);
    mTripletCount = mQuadCount = mQuintCount = mSextCount = 1;
    mCycle = mCycleEnd - 1;
    mLastRef.fill(0.0);
  }

  double ProcessSample(double input)
  {
    const double drySample = input;
    double workingSample = input;

    ++mCycle;
    if (mCycle == mCycleEnd)
    {
      double correction = 0.0;

      // Duo (order 2): special-cased leak/flip scheme, verbatim.
      if (std::fabs(mHissIntensity) > 0.0001)
      {
        double factor = mPrev[0] - workingSample;
        if (mDuoFlip) { mDuoEven += factor; mDuoOdd -= factor; factor = mDuoEven * mHissIntensity; }
        else { mDuoOdd += factor; mDuoEven -= factor; factor = mDuoOdd * mHissIntensity; }
        mDuoOdd = (mDuoOdd - ((mDuoOdd - mDuoEven) / 256.0)) / 2.0;
        mDuoEven = (mDuoEven - ((mDuoEven - mDuoOdd) / 256.0)) / 2.0;
        correction += factor;
        mDuoFlip = !mDuoFlip;
      }

      correction += ProcessRingBand(mTripletCells, 3, mTripletCount, mPrev[1], workingSample, mGlitterIntensity);
      correction += ProcessRingBand(mQuadCells,    4, mQuadCount,    mPrev[2], workingSample, mRatIntensity);
      correction += ProcessRingBand(mQuintCells,   5, mQuintCount,   mPrev[3], workingSample, mFizzIntensity);
      correction += ProcessRingBand(mSextCells,    6, mSextCount,    mPrev[4], workingSample, mScrapeIntensity);

      // Shared tapped delay line, shifted using THIS cycle's pre-
      // correction sample -- verbatim order (shift happens before the
      // sample is replaced by the correction/mix result below).
      mPrev[4] = mPrev[3]; mPrev[3] = mPrev[2]; mPrev[2] = mPrev[1]; mPrev[1] = mPrev[0];
      mPrev[0] = workingSample;

      workingSample = correction * mMix;

      // Interpolation setup for the gap until the next Energy cycle --
      // verbatim per-cycleEnd blend scheme from the source.
      if (mCycleEnd == 4)
      {
        mLastRef[0] = mLastRef[4];
        mLastRef[2] = (mLastRef[0] + workingSample) / 2.0;
        mLastRef[1] = (mLastRef[0] + mLastRef[2]) / 2.0;
        mLastRef[3] = (mLastRef[2] + workingSample) / 2.0;
        mLastRef[4] = workingSample;
      }
      else if (mCycleEnd == 3)
      {
        mLastRef[0] = mLastRef[3];
        mLastRef[2] = (mLastRef[0] + mLastRef[0] + workingSample) / 3.0;
        mLastRef[1] = (mLastRef[0] + workingSample + workingSample) / 3.0;
        mLastRef[3] = workingSample;
      }
      else if (mCycleEnd == 2)
      {
        mLastRef[0] = mLastRef[2];
        mLastRef[1] = (mLastRef[0] + workingSample) / 2.0;
        mLastRef[2] = workingSample;
      }
      else
      {
        mLastRef[0] = workingSample;
      }
      mCycle = 0;
      workingSample = mLastRef[static_cast<size_t>(mCycle)];
    }
    else
    {
      workingSample = mLastRef[static_cast<size_t>(mCycle)];
    }

    // Multi-pole averaging using lastRef[] -- verbatim fallthrough
    // switch from the source (each case intentionally falls into the
    // next, same as the original).
    switch (mCycleEnd)
    {
      case 4:
        mLastRef[8] = workingSample; workingSample = (workingSample + mLastRef[7]) * 0.5; mLastRef[7] = mLastRef[8];
        [[fallthrough]];
      case 3:
        mLastRef[8] = workingSample; workingSample = (workingSample + mLastRef[6]) * 0.5; mLastRef[6] = mLastRef[8];
        [[fallthrough]];
      case 2:
        mLastRef[8] = workingSample; workingSample = (workingSample + mLastRef[5]) * 0.5; mLastRef[5] = mLastRef[8];
        [[fallthrough]];
      case 1:
      default:
        break;
    }

    const double out = workingSample + drySample;
    return std::isfinite(out) ? out : 0.0;
  }

private:
  // Generic form of the source's hand-unrolled order-3..9 ring bands
  // (see header note) -- `cells` must be sized >= order, `count` is
  // the band's own 1..order cycling position.
  static double ProcessRingBand(std::array<double, 9>& cells, int order, int& count,
                                 double prevTap, double input, double intensity)
  {
    if (std::fabs(intensity) <= 0.0001) return 0.0;
    if (count < 1 || count > order) count = 1;
    const double factor0 = prevTap - input;
    const int cur = count - 1;
    const int prevIdx = (cur - 1 + order) % order;
    cells[static_cast<size_t>(cur)] += factor0;
    cells[static_cast<size_t>(prevIdx)] -= factor0;
    const double outFactor = cells[static_cast<size_t>(cur)] * intensity;
    for (int i = 0; i < order; ++i) cells[static_cast<size_t>(i)] /= 2.0;
    ++count;
    return outFactor;
  }

  double mSampleRate = 44100.0;
  int mCycleEnd = 1;
  int mCycle = 0;

  double mHissIntensity = 0.0, mGlitterIntensity = 0.0;
  double mRatIntensity = 0.0, mScrapeIntensity = 0.0, mFizzIntensity = 0.0;
  double mMix = 0.0;

  std::array<double, 5> mPrev{}; // shared taps A..E (depth 1..5); F/G/H never allocated (see header note)
  double mDuoEven = 0.0, mDuoOdd = 0.0;
  bool mDuoFlip = false;
  std::array<double, 9> mTripletCells{}; int mTripletCount = 1; // order 3, only first 3 used
  std::array<double, 9> mQuadCells{};    int mQuadCount = 1;    // order 4, only first 4 used
  std::array<double, 9> mQuintCells{};   int mQuintCount = 1;   // order 5, only first 5 used
  std::array<double, 9> mSextCells{};    int mSextCount = 1;    // order 6, only first 6 used
  std::array<double, 9> mLastRef{};
};

} // namespace exoeq
