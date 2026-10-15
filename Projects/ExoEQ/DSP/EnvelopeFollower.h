#pragma once

#include <cmath>
#include <algorithm>

// EnvelopeFollower.h
// Foundational block for two things the spec still has open: Phase 3's
// sidechain input (duck/trigger a band's filter from an external signal)
// and Phase 4's generic modulation matrix (any detected envelope as a mod
// source, not just sidechain). Built standalone and tested on its own
// first, same as DelaySend/DistortionSend/FrequencyShiftSend were, rather
// than wired straight into BandChain -- sidechain specifically needs a
// second host-level audio input bus (a config.h/CMakeLists change, a real
// architecture decision, not a drop-in), so that wiring is deliberately
// NOT done here; this is the detector those features will both consume.
//
// Peak mode: tracks |x| directly -- fast, simple, the usual choice for
// "duck the filter the instant something hits". RMS mode: tracks the
// mean-square level over a short window before the attack/release smoothing
// -- smoother, less jittery on complex/noisy material, the usual choice for
// a modulation SOURCE you don't want twitching sample-to-sample.

namespace exoeq
{

enum class EnvelopeDetectMode { Peak, RMS };

struct EnvelopeFollowerParams
{
  double attackMs = 5.0;
  double releaseMs = 100.0;
  EnvelopeDetectMode mode = EnvelopeDetectMode::Peak;
  double rmsWindowMs = 10.0; // only used in RMS mode
};

class EnvelopeFollower
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = std::max(1.0, sampleRate);
    RecalcCoeffs();
  }

  void SetParams(const EnvelopeFollowerParams& p)
  {
    mParams = p;
    mParams.attackMs = std::max(0.0, mParams.attackMs);
    mParams.releaseMs = std::max(0.0, mParams.releaseMs);
    mParams.rmsWindowMs = std::max(0.1, mParams.rmsWindowMs);
    RecalcCoeffs();
  }

  void Reset()
  {
    mEnvelope = 0.0;
    mRmsState = 0.0;
  }

  // Feeds one input sample, returns the current envelope value (always
  // >= 0, a linear-scale level, not dB).
  double Process(double input)
  {
    double detected;
    if (mParams.mode == EnvelopeDetectMode::RMS)
    {
      const double sq = input * input;
      mRmsState += (sq - mRmsState) * mRmsCoeff;
      detected = std::sqrt(std::max(0.0, mRmsState));
    }
    else
    {
      detected = std::fabs(input);
    }

    // Classic one-pole attack/release follower: different time constants
    // depending on whether the detected level is rising or falling.
    const double coeff = (detected > mEnvelope) ? mAttackCoeff : mReleaseCoeff;
    mEnvelope += (detected - mEnvelope) * coeff;
    if (!std::isfinite(mEnvelope)) mEnvelope = 0.0;
    return mEnvelope;
  }

  double Current() const { return mEnvelope; }

private:
  void RecalcCoeffs()
  {
    mAttackCoeff = TimeToCoeff(mParams.attackMs);
    mReleaseCoeff = TimeToCoeff(mParams.releaseMs);
    mRmsCoeff = TimeToCoeff(mParams.rmsWindowMs);
  }

  double TimeToCoeff(double ms) const
  {
    if (ms <= 0.0) return 1.0; // instant
    const double tauSamples = (ms * 0.001) * mSampleRate;
    return 1.0 - std::exp(-1.0 / std::max(1.0, tauSamples));
  }

  EnvelopeFollowerParams mParams;
  double mSampleRate = 44100.0;
  double mAttackCoeff = 1.0;
  double mReleaseCoeff = 1.0;
  double mRmsCoeff = 1.0;
  double mEnvelope = 0.0;
  double mRmsState = 0.0;
};

} // namespace exoeq
