#pragma once

#include "FilterCore.h"
#include "StateVariableFilter.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <vector>

// CharacterFilters.h
// ORIGINAL filter types -- NOT ports. There is no vendored source for
// any of these the way there is for the Airwindows plugins
// (AirwindowsFilters.h): Icarus by Tone2 is closed-source, so nothing
// here claims to reproduce its exact algorithm. Each class below is
// built from a real, well-documented public/academic DSP technique
// that produces the same CATEGORY of character Icarus's filter list
// names (comb, ring mod, formant/vocal, phaser, physical-modeling
// resonator, sample-rate reduction) -- textbook material, decades old,
// not proprietary to any product. Where a technique has a canonical
// reference (comb filters, first-order allpass phasers, modal/harmonic
// resonator banks, vowel formant tables, zero-order-hold decimation)
// it's named in that class's own header comment.

namespace exoeq
{

// ---- Comb ----
// Feedforward (y = x + g*x[n-D]) and feedback (y = x + g*y[n-D]) comb
// topologies -- the two classic comb filter forms, interpolated delay
// line for a click-free cutoff sweep. cutoffHz sets delay time
// (D = sampleRate/cutoffHz), resonance (drive01) is the feedback/
// feedforward gain (clamped well inside stability), morph flips the
// gain's polarity (negative-gain comb puts notches where positive-gain
// puts peaks, and vice versa -- a real, audible difference in a comb,
// not a cosmetic one).
class CombFilter
{
public:
  void SetSampleRate(double sr)
  {
    mSampleRate = std::max(1.0, sr);
    const size_t maxDelay = static_cast<size_t>(mSampleRate / 15.0) + 4; // supports down to 15Hz
    mBuffer.assign(maxDelay, 0.0);
    mWriteIdx = 0;
  }

  void SetParams(double cutoffHz, double drive01, double polarityMorph, bool feedbackTopology)
  {
    mFeedback = feedbackTopology;
    const double safeCutoff = std::clamp(cutoffHz, 15.0, mSampleRate * 0.49);
    const double maxDelaySamples = mBuffer.empty() ? 1.0 : static_cast<double>(mBuffer.size()) - 2.0;
    mDelaySamples = std::clamp(mSampleRate / safeCutoff, 1.0, maxDelaySamples);
    mGain = std::clamp(drive01, 0.0, 0.97);
    mPolarity = (polarityMorph >= 0.5) ? -1.0 : 1.0;
  }

  void Reset() { std::fill(mBuffer.begin(), mBuffer.end(), 0.0); mWriteIdx = 0; }

  double ProcessSample(double input)
  {
    if (mBuffer.empty()) return input;
    const double readPos = static_cast<double>(mWriteIdx) - mDelaySamples;
    const double delayed = ReadInterpolated(readPos);

    const double out = input + mPolarity * mGain * delayed;
    mBuffer[mWriteIdx] = mFeedback ? out : input;
    mWriteIdx = (mWriteIdx + 1) % mBuffer.size();
    return std::isfinite(out) ? out : 0.0;
  }

private:
  double ReadInterpolated(double pos) const
  {
    const double size = static_cast<double>(mBuffer.size());
    while (pos < 0.0) pos += size;
    const size_t i0 = static_cast<size_t>(pos) % mBuffer.size();
    const size_t i1 = (i0 + 1) % mBuffer.size();
    const double frac = pos - std::floor(pos);
    return mBuffer[i0] * (1.0 - frac) + mBuffer[i1] * frac;
  }

  double mSampleRate = 44100.0;
  std::vector<double> mBuffer;
  size_t mWriteIdx = 0;
  double mDelaySamples = 100.0;
  double mGain = 0.5;
  double mPolarity = 1.0;
  bool mFeedback = false;
};

// ---- Ring modulator / audio-rate FM filter ----
// One internal oscillator, two uses: RingMod multiplies the signal by
// a sine at cutoffHz (textbook ring modulation -- sum/difference
// sidebands, not a slope); FilterFM instead uses that same oscillator
// to modulate a one-pole lowpass's own cutoff at audio rate, which
// produces a wobbling/metallic texture instead of amplitude sidebands.
// cutoffHz = oscillator/carrier frequency, resonance (depth01) =
// RingMod's dry/wet (1 = classic full ring mod) or FilterFM's
// modulation depth in octaves, morph blends the oscillator's shape
// from pure sine toward sine+3rd-harmonic for a grittier modulator.
class RingModFilter
{
public:
  void SetSampleRate(double sr) { mSampleRate = std::max(1.0, sr); }

  void SetParams(double cutoffHz, double depth01, double shapeMorph, bool fmMode)
  {
    mFmMode = fmMode;
    mCarrierHz = std::clamp(cutoffHz, 1.0, mSampleRate * 0.49);
    mDepth = std::clamp(depth01, 0.0, 1.0);
    mShape = std::clamp(shapeMorph, 0.0, 1.0);
  }

  void Reset() { mPhase = 0.0; mLp = 0.0; }

  double ProcessSample(double input)
  {
    constexpr double kTwoPi = 6.283185307179586;
    double osc = std::sin(mPhase);
    if (mShape > 0.0)
      osc = (1.0 - mShape) * osc + mShape * (std::sin(3.0 * mPhase) * 0.5);

    double out;
    if (mFmMode)
    {
      const double modulatedHz = std::clamp(mCarrierHz * std::pow(2.0, mDepth * 3.0 * osc), 20.0, mSampleRate * 0.49);
      const double omega = kTwoPi * modulatedHz / mSampleRate;
      const double coeff = std::clamp(omega / (omega + 1.0), 0.0001, 0.9999);
      mLp += coeff * (input - mLp);
      if (!std::isfinite(mLp)) mLp = 0.0;
      out = mLp;
    }
    else
    {
      out = (1.0 - mDepth) * input + mDepth * (input * osc);
    }

    mPhase += kTwoPi * mCarrierHz / mSampleRate;
    if (mPhase > kTwoPi) mPhase -= kTwoPi;

    return std::isfinite(out) ? out : 0.0;
  }

private:
  double mSampleRate = 44100.0;
  double mCarrierHz = 1000.0;
  double mDepth = 0.5;
  double mShape = 0.0;
  bool mFmMode = false;
  double mPhase = 0.0;
  double mLp = 0.0;
};

// ---- Phaser-as-filter ----
// N-stage first-order allpass cascade with feedback -- the standard
// Schroeder/CCRMA phaser topology (Direct Form II transposed allpass
// per stage), textbook material, not product-specific. Produces moving
// NOTCHES rather than a slope. cutoffHz = allpass corner, morph =
// stage count (1-8, same "more stages = more character" convention
// used throughout this project's dropdown), resonance = feedback
// amount (sharper, more resonant notches at higher settings).
class PhaserFilter
{
public:
  void SetSampleRate(double sr) { mSampleRate = std::max(1.0, sr); }

  void SetParams(double cutoffHz, double feedback01, double stageMorph)
  {
    constexpr double kPi = 3.14159265358979323846;
    const double safeCutoff = std::clamp(cutoffHz, 20.0, mSampleRate * 0.49);
    const double t = std::tan(kPi * safeCutoff / mSampleRate);
    mCoeff = std::clamp((1.0 - t) / (1.0 + t), -0.999, 0.999);
    mNumStages = std::clamp(1 + static_cast<int>(stageMorph * 7.999), 1, 8);
    mFeedback = std::clamp(feedback01, 0.0, 0.95);
  }

  void Reset() { mZ.fill(0.0); mFeedbackState = 0.0; }

  double ProcessSample(double input)
  {
    double x = input + mFeedback * mFeedbackState;
    for (int i = 0; i < mNumStages; ++i)
    {
      const double y = mZ[static_cast<size_t>(i)] - mCoeff * x;
      mZ[static_cast<size_t>(i)] = x + mCoeff * y;
      x = y;
    }
    mFeedbackState = x;
    if (!std::isfinite(mFeedbackState) || std::fabs(mFeedbackState) > 50.0)
    {
      mFeedbackState = 0.0;
      mZ.fill(0.0);
      return 0.0;
    }
    return x;
  }

private:
  double mSampleRate = 44100.0;
  double mCoeff = 0.0;
  int mNumStages = 4;
  double mFeedback = 0.0;
  std::array<double, 8> mZ{};
  double mFeedbackState = 0.0;
};

// ---- Vocal formant filter ----
// A parallel bank of 3 resonant bandpass stages (this project's own
// TPT StateVariableFilter, reused for its existing stability work)
// tuned to classic vowel formant frequencies. The formant numbers below
// are generic acoustic-phonetics averages in the lineage of Peterson &
// Barney's widely published 1952 vowel formant measurements -- common
// public reference data used in countless open vocal-filter/vocoder
// designs, not specific to any product. vowelMorph sweeps through
// [A, E, I, O, U] in sequence, linearly interpolating each formant's
// own frequency between the two nearest vowels (cheaper than
// crossfading two full filter banks, and avoids any phase-cancellation
// dip doing that would risk). cutoffHz acts as a transpose control
// around a 1kHz reference (same "repurposed knob for a character type
// with no single corner frequency" convention as AirLift/Capacitor
// elsewhere in this project) rather than a literal corner frequency.
// resonance sets each formant's Q (higher = narrower/more robotic).
class VocalFormantFilter
{
public:
  void SetSampleRate(double sr)
  {
    mSampleRate = std::max(1.0, sr);
    for (auto& s : mStages) s.SetSampleRate(mSampleRate);
  }

  void SetParams(double cutoffHz, double resonance01, double vowelMorph)
  {
    const double transpose = std::clamp(cutoffHz / 1000.0, 0.25, 4.0);
    const double q = 2.0 + std::clamp(resonance01, 0.0, 1.0) * 18.0;

    const double pos = std::clamp(vowelMorph, 0.0, 1.0) * 4.0;
    const int idx0 = std::clamp(static_cast<int>(pos), 0, 4);
    const int idx1 = std::min(idx0 + 1, 4);
    const double frac = pos - static_cast<double>(idx0);

    for (int f = 0; f < 3; ++f)
    {
      const double freq = std::clamp(
          (kVowelFormants[static_cast<size_t>(idx0)][static_cast<size_t>(f)] * (1.0 - frac) +
           kVowelFormants[static_cast<size_t>(idx1)][static_cast<size_t>(f)] * frac) * transpose,
          40.0, mSampleRate * 0.49);
      FilterParams p;
      p.cutoffHz = freq;
      p.resonance = q;
      mStages[static_cast<size_t>(f)].SetParams(p);
    }
  }

  void Reset() { for (auto& s : mStages) s.Reset(); }

  double ProcessSample(double input)
  {
    double out = 0.0;
    for (auto& s : mStages) out += s.ProcessSampleTaps(input).bp;
    out /= 3.0;
    return std::isfinite(out) ? out : 0.0;
  }

private:
  static constexpr double kVowelFormants[5][3] = {
    {800.0, 1150.0, 2900.0}, // A (ah)
    {400.0, 1700.0, 2600.0}, // E (eh)
    {270.0, 2300.0, 3000.0}, // I (ee)
    {450.0, 800.0,  2830.0}, // O (oh)
    {325.0, 700.0,  2530.0}, // U (oo)
  };

  double mSampleRate = 44100.0;
  std::array<StateVariableFilter, 3> mStages;
};

// ---- Physical-modeling harmonic resonator bank (Odd/Even) ----
// A small bank of resonant bandpass stages tuned to the odd-only
// (1,3,5,7,...) or even-only (2,4,6,8,...) harmonic series above
// cutoffHz (the fundamental), each scaled by the natural 1/n harmonic
// rolloff -- modal/additive resonance synthesis, the decades-old public
// technique behind physical-modeling instruments that favor odd
// harmonics (closed-pipe/clarinet-like) or emphasize even ones
// (open-pipe/flute-like). This names the harmonic-series CHARACTER the
// category describes, not a specific instrument model. resonance sets
// each harmonic's Q (higher = longer "ring", closer to a resonant
// physical body), morph sets how many harmonics are active (1-8 --
// fewer is both cheaper AND a purer/simpler tone, more is denser and
// more CPU, a real cost/character tradeoff the knob makes explicit).
class PhysResonatorFilter
{
public:
  void SetSampleRate(double sr)
  {
    mSampleRate = std::max(1.0, sr);
    for (auto& s : mStages) s.SetSampleRate(mSampleRate);
  }

  void SetParams(double cutoffHz, double resonance01, double countMorph, bool oddHarmonics)
  {
    mFundamental = std::clamp(cutoffHz, 20.0, mSampleRate * 0.49);
    mQ = 2.0 + std::clamp(resonance01, 0.0, 1.0) * 38.0;
    mNumActive = std::clamp(1 + static_cast<int>(std::clamp(countMorph, 0.0, 1.0) * 7.999), 1, 8);
    mOdd = oddHarmonics;

    for (int i = 0; i < 8; ++i)
    {
      const int harmonicIndex = mOdd ? (2 * i + 1) : (2 * (i + 1));
      const double freq = std::clamp(mFundamental * harmonicIndex, 20.0, mSampleRate * 0.49);
      FilterParams p;
      p.cutoffHz = freq;
      p.resonance = mQ;
      mStages[static_cast<size_t>(i)].SetParams(p);
    }
  }

  void Reset() { for (auto& s : mStages) s.Reset(); }

  double ProcessSample(double input)
  {
    double sum = 0.0;
    for (int i = 0; i < mNumActive; ++i)
    {
      const int harmonicIndex = mOdd ? (2 * i + 1) : (2 * (i + 1));
      const double amp = 1.0 / static_cast<double>(harmonicIndex);
      sum += mStages[static_cast<size_t>(i)].ProcessSampleTaps(input).bp * amp;
    }
    const double norm = mNumActive > 0 ? std::sqrt(static_cast<double>(mNumActive)) : 1.0;
    const double out = sum / norm;
    return std::isfinite(out) ? out : 0.0;
  }

private:
  double mSampleRate = 44100.0;
  double mFundamental = 200.0;
  double mQ = 10.0;
  int mNumActive = 4;
  bool mOdd = true;
  std::array<StateVariableFilter, 8> mStages;
};

// ---- Resample (zero-order-hold decimator) ----
// Holds the input for N samples before taking a new one, producing the
// stair-stepped aliasing character of a reduced effective sample rate
// -- the standard bitcrusher/decimator technique, textbook, not
// product-specific. cutoffHz is repurposed as the effective reduced
// sample rate (not a corner frequency for this type -- same
// "repurposed knob" convention as AirLift/Capacitor/VocalFormant
// elsewhere in this project), resonance is dry/wet, morph blends from
// a hard zero-order hold (0, harsh/stepped) toward linear interpolation
// between held values (1, smoother, less aliased).
class ResampleFilter
{
public:
  void SetSampleRate(double sr) { mSampleRate = std::max(1.0, sr); }

  void SetParams(double effectiveRateHz, double mix01, double smoothMorph)
  {
    const double safeRate = std::clamp(effectiveRateHz, 200.0, mSampleRate);
    mHoldSamples = std::clamp(mSampleRate / safeRate, 1.0, 1000.0);
    mMix = std::clamp(mix01, 0.0, 1.0);
    mSmooth = std::clamp(smoothMorph, 0.0, 1.0);
  }

  void Reset() { mHeld = 0.0; mPrevHeld = 0.0; mCounter = 0.0; }

  double ProcessSample(double input)
  {
    mCounter += 1.0;
    if (mCounter >= mHoldSamples)
    {
      mCounter -= mHoldSamples;
      mPrevHeld = mHeld;
      mHeld = input;
    }
    double stepped = mHeld;
    if (mSmooth > 0.0)
    {
      const double frac = mHoldSamples > 0.0 ? std::clamp(mCounter / mHoldSamples, 0.0, 1.0) : 0.0;
      const double interp = mPrevHeld * (1.0 - frac) + mHeld * frac;
      stepped = mHeld * (1.0 - mSmooth) + interp * mSmooth;
    }
    const double out = (1.0 - mMix) * input + mMix * stepped;
    return std::isfinite(out) ? out : 0.0;
  }

private:
  double mSampleRate = 44100.0;
  double mHoldSamples = 4.0;
  double mMix = 1.0;
  double mSmooth = 0.0;
  double mHeld = 0.0;
  double mPrevHeld = 0.0;
  double mCounter = 0.0;
};

} // namespace exoeq
