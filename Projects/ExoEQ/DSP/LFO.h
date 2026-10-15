#pragma once

#include <cmath>

// M_PI isn't standard C++ -- glibc/macOS expose it as an extension, but
// MSVC only defines it when _USE_MATH_DEFINES is set before <cmath> is
// included (the exact C2065 "undeclared identifier" your build hit). A
// local constant sidesteps that entirely rather than depending on a
// define some other header may or may not have already set first.
namespace exoeq { constexpr double kPi = 3.14159265358979323846; }

// DSP/LFO.h
//
// RECREATED THIS ROUND, same situation as DSP/BandGroups.h's own header
// note -- the original never actually reached your disk. Shape here is
// fixed by ExoEQ.h/.cpp's existing, already-written usage (not a guess):
// ExoEQ.h declares `exoeq::LFO mModLfos[kMaxModSources]`; ExoEQ.cpp's
// ProcessBlock does `exoeq::LfoParams lp; lp.rateHz = ...; lp.waveform =
// static_cast<exoeq::LfoWaveform>(...); mModLfos[i].SetParams(lp);
// mModMatrix.SetSourceValue(i, mModLfos[i].Tick());` once per block (not
// per sample -- see that file's own comment on why block-rate ticking is
// fine here). The waveform dropdown's option list (ExoEQ.cpp's InitEnum
// for kLfoWaveform) is "Sine, Triangle, Saw, Square, Sample & Hold", in
// that exact order -- LfoWaveform's enumerators below MUST stay in that
// order for the cast from the param's int value to land on the right one.
namespace exoeq
{

enum class LfoWaveform
{
  Sine = 0,
  Triangle,
  Saw,
  Square,
  SampleAndHold
};

struct LfoParams
{
  double rateHz = 1.0;
  LfoWaveform waveform = LfoWaveform::Sine;
};

// Ticked once per block (per ExoEQ.cpp's own calling convention above),
// returns a value in [-1, 1] -- ModMatrix's connections apply this
// multiplied by a connection's Depth, so the exact bipolar range here is
// what every Depth knob's -8..8 range is scaled against.
class LFO
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0; }

  void SetParams(const LfoParams& params) { mParams = params; }

  void Reset() { mPhase = 0.0; mSampleAndHoldValue = 0.0; mTickedOnce = false; }

  double Tick()
  {
    const double phaseInc = mParams.rateHz / mSampleRate;
    // Ticked once per BLOCK, not per sample (see this file's own header
    // note) -- advancing phase by a per-sample increment here would make
    // the LFO run far too slowly at any real block size. There's no
    // block-size parameter available to this class, so instead this
    // advances by the per-sample increment scaled by a fixed nominal
    // block length; this is an approximation (actual rate tracks the
    // host's real block size, not a fixed assumption) good enough for a
    // musical-rate modulation source, not a sample-accurate one -- same
    // "block-rate is fine for this" call ExoEQ.cpp's own comment already
    // made for ticking this once per block in the first place. If you
    // want sample-accurate phase tracking, say so and this moves to a
    // per-sample Tick(nFrames) call instead.
    constexpr double kAssumedBlockFrames = 512.0;
    mPhase += phaseInc * kAssumedBlockFrames;

    const bool wrapped = mPhase >= 1.0;
    if (wrapped) mPhase -= std::floor(mPhase);

    double value = 0.0;
    switch (mParams.waveform)
    {
      case LfoWaveform::Sine:
        value = std::sin(mPhase * 2.0 * kPi);
        break;
      case LfoWaveform::Triangle:
        value = 4.0 * std::abs(mPhase - 0.5) - 1.0;
        break;
      case LfoWaveform::Saw:
        value = mPhase * 2.0 - 1.0;
        break;
      case LfoWaveform::Square:
        value = (mPhase < 0.5) ? 1.0 : -1.0;
        break;
      case LfoWaveform::SampleAndHold:
        // New random value only at each cycle wrap (or on the very first
        // tick ever, so it doesn't start at a hardcoded 0) -- held steady
        // in between, same convention as every hardware/software S&H LFO.
        if (wrapped || !mTickedOnce)
          mSampleAndHoldValue = (static_cast<double>(mRng()) / static_cast<double>(mRng.max())) * 2.0 - 1.0;
        value = mSampleAndHoldValue;
        break;
    }
    mTickedOnce = true;
    return value;
  }

private:
  double mSampleRate = 44100.0;
  double mPhase = 0.0;
  double mSampleAndHoldValue = 0.0;
  bool mTickedOnce = false;
  LfoParams mParams;
  // A plain fast PRNG rather than <random>'s heavier engines -- this only
  // needs "looks random enough for an LFO", not cryptographic or
  // statistical quality, and avoids pulling in <random>'s larger
  // machinery for one call per cycle wrap.
  struct FastRng
  {
    unsigned int state = 0x9e3779b9u;
    unsigned int operator()()
    {
      state ^= state << 13; state ^= state >> 17; state ^= state << 5;
      return state;
    }
    static unsigned int max() { return 0xffffffffu; }
  } mRng;
};

} // namespace exoeq
