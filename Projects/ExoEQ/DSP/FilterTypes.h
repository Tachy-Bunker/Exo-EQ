#pragma once

#include "FilterCore.h"
#include "StateVariableFilter.h"
#include "AirwindowsFilters.h"
#include "CharacterFilters.h"
#include <array>
#include <cmath>
#include <algorithm>

// FilterTypes.h
// The dropdown filter-TYPE system (spec: "usable with a dropdown like in
// FabFilter going highpass > highpass 24oct"). FilterParams::type picks
// one of FilterTypeId's options (FilterCore.h); this file is what each
// one actually does.
//
// Everything that isn't MorphZ/Capacitor/AirLift is built by CASCADING
// the already-tested 2-pole TPT StateVariableFilter rather than deriving
// new closed-form multi-pole coefficients from scratch -- every slope
// option inherits that core's existing stability guarantees (denormal
// flushing, NaN guards, coefficient clamping) for free, instead of
// needing its own copy of all of that safety work. 6dB/oct is the one
// exception (a single pole is inherently a different, simpler topology
// than "half" a 2-pole stage), so it gets its own tiny OnePoleFilter.
//
// Slope ladder, matching FabFilter's own convention (dB/oct = poles * 6):
//   6dB/oct  = 1 one-pole stage
//   12dB/oct = 1 two-pole SVF stage   (this is the plugin's ORIGINAL filter)
//   24dB/oct = 2 cascaded two-pole SVF stages
//   48dB/oct = 4 cascaded two-pole SVF stages
//
// Notch12 uses the identity notch = lowpass + highpass (derivable
// directly from the SVF's own taps, see StateVariableFilter.h's
// ProcessSampleTaps -- no extra coefficients needed). BP12/BP24 just
// read the bandpass tap through 1 or 2 cascaded stages, same ladder
// logic as the LP/HP slopes.

namespace exoeq
{

// True single-pole (6dB/oct) LP/HP -- the one slope the 2-pole SVF core
// can't produce as "half a stage".
class OnePoleFilter
{
public:
  void SetSampleRate(double sr) { mSampleRate = std::max(1.0, sr); RecalcCoeff(); }
  void SetCutoff(double hz)
  {
    mCutoffHz = std::clamp(hz, 5.0, mSampleRate * 0.49);
    RecalcCoeff();
  }
  void Reset() { mState = 0.0; }

  double ProcessLP(double x)
  {
    mState += mCoeff * (x - mState);
    if (!std::isfinite(mState)) mState = 0.0;
    if (std::fabs(mState) < 1.0e-300) mState = 0.0; // denormal flush, same rationale as StateVariableFilter
    return mState;
  }
  double ProcessHP(double x) { return x - ProcessLP(x); }

private:
  void RecalcCoeff()
  {
    constexpr double kTwoPi = 6.283185307179586;
    const double omega = kTwoPi * mCutoffHz / mSampleRate;
    mCoeff = std::clamp(omega / (omega + 1.0), 0.0001, 0.9999);
    if (!std::isfinite(mCoeff)) mCoeff = 0.5; // last-resort fallback, same philosophy as StateVariableFilter
  }

  double mSampleRate = 44100.0;
  double mCutoffHz = 1000.0;
  double mCoeff = 0.1;
  double mState = 0.0;
};

// Dispatches to whichever topology FilterParams::type asks for. Public
// surface matches IFilterNode so it drops straight into Band in place of
// the plain StateVariableFilter it's replacing.
class MultiPoleFilter final : public IFilterNode
{
public:
  void SetSampleRate(double sampleRate) override
  {
    mSampleRate = std::max(1.0, sampleRate);
    for (auto& s : mStages) s.SetSampleRate(mSampleRate);
    mOnePoleA.SetSampleRate(mSampleRate);
    mOnePoleB.SetSampleRate(mSampleRate);
    mAirTap.SetSampleRate(mSampleRate);
    mAwZLowpass.SetSampleRate(mSampleRate);
    mAwZLowpassFull.SetSampleRate(mSampleRate);
    mAwZHighpass.SetSampleRate(mSampleRate);
    mAwZBandpass.SetSampleRate(mSampleRate);
    mAwZNotch.SetSampleRate(mSampleRate);
    mAwLowpass2.SetSampleRate(mSampleRate);
    mAwHighpass2.SetSampleRate(mSampleRate);
    mAwXLowpass.SetSampleRate(mSampleRate);
    mAwXHighpass.SetSampleRate(mSampleRate);
    mAwXBandpass.SetSampleRate(mSampleRate);
    mAwXNotch.SetSampleRate(mSampleRate);
    mAwHoltLowpass.SetSampleRate(mSampleRate);
    mAwBiquadStack.SetSampleRate(mSampleRate);
    mAwYLowpass.SetSampleRate(mSampleRate);
    mAwYHighpass.SetSampleRate(mSampleRate);
    mAwYBandpass.SetSampleRate(mSampleRate);
    mAwYNotch.SetSampleRate(mSampleRate);
    mAwBiquadNonLinLowpass.SetSampleRate(mSampleRate);
    mAwBiquadNonLinBandpass.SetSampleRate(mSampleRate);
    mAwZAcidLowpass.SetSampleRate(mSampleRate);
    mAwHermepassHighpass.SetSampleRate(mSampleRate);
    mAwAverMatrix.SetSampleRate(mSampleRate);
    mAwSlewSonic.SetSampleRate(mSampleRate);
    mCombFeedforward.SetSampleRate(mSampleRate);
    mCombFeedback.SetSampleRate(mSampleRate);
    mRingModulator.SetSampleRate(mSampleRate);
    mFilterFM.SetSampleRate(mSampleRate);
    mVocalFormant.SetSampleRate(mSampleRate);
    mPhaser.SetSampleRate(mSampleRate);
    mPhysOddResonator.SetSampleRate(mSampleRate);
    mPhysEvenResonator.SetSampleRate(mSampleRate);
    mResample.SetSampleRate(mSampleRate);
  }

  void SetParams(const FilterParams& params) override
  {
    mParams = params;
    mParams.cutoffHz = std::clamp(mParams.cutoffHz, 5.0, mSampleRate * 0.49);
    mParams.resonance = std::max(0.01, mParams.resonance);
    mParams.morph = std::clamp(mParams.morph, 0.0, 1.0);

    for (auto& s : mStages) s.SetParams(mParams); // harmless for stages a given type doesn't use
    mOnePoleA.SetCutoff(mParams.cutoffHz);
    mOnePoleB.SetCutoff(mParams.cutoffHz);

    // AirLift repurposes cutoff/resonance/morph (see header comment):
    // cutoffHz = where the presence bump sits, resonance = its width
    // (same knob, same units a user already understands from every other
    // type), morph = lift AMOUNT (0 = off, 1 = strong) instead of LP/HP
    // blend, since AirLift has no LP/HP side to blend between.
    FilterParams airTapParams = mParams;
    airTapParams.resonance = std::max(0.3, mParams.resonance); // a very low Q would make the "bump" too wide to read as presence
    mAirTap.SetParams(airTapParams);

    // Real Airwindows ports (AirwindowsFilters.h): resonance doubles as
    // each type's "drive"/"tight" knob (point 4 in that file's header
    // note), morph picks 1 vs 2 (Z-family) or 1-4 (Lowpass2/Highpass2
    // family) cascaded stages, same convention as this file's own
    // LP6/12/24/48 ladder.
    const double drive01 = std::clamp((mParams.resonance - 0.01) / (20.0 - 0.01), 0.0, 1.0);
    const int zStages = (mParams.morph >= 0.5) ? 2 : 1;
    const int adaptiveStages = std::clamp(1 + static_cast<int>(mParams.morph * 3.999), 1, 4);

    // Z-Lowpass morph is a continuous crossfade between the 1-stage and 2-stage
    // outputs (was a >=0.5 threshold). morph 0 = 1 stage, morph 1 = 2 stages.
    mAwZLowpass.SetParams(AwZKind::Lowpass, mParams.cutoffHz, drive01, 1);
    mAwZLowpassFull.SetParams(AwZKind::Lowpass, mParams.cutoffHz, drive01, 2);
    mZLowpassMorph = std::clamp(mParams.morph, 0.0, 1.0);
    mAwZHighpass.SetParams(AwZKind::Highpass, mParams.cutoffHz, drive01, zStages);
    mAwZBandpass.SetParams(AwZKind::Bandpass, mParams.cutoffHz, drive01, zStages);
    mAwZNotch.SetParams(AwZKind::Notch, mParams.cutoffHz, drive01, zStages);
    mAwLowpass2.SetParams(mParams.cutoffHz, drive01, /*highpass=*/false, adaptiveStages);
    mAwHighpass2.SetParams(mParams.cutoffHz, drive01, /*highpass=*/true, adaptiveStages);

    const int xStages = adaptiveStages; // X-family also ported with the 1-4 cascade convention
    mAwXLowpass.SetParams(AwXKind::Lowpass, mParams.cutoffHz, drive01, xStages);
    mXLowpassInGain = std::pow(std::clamp(mParams.extra, 0.0, 1.0) + 0.9, 4.0); // extra 0.1 = unity
    mAwXHighpass.SetParams(AwXKind::Highpass, mParams.cutoffHz, drive01, xStages);
    mAwXBandpass.SetParams(AwXKind::Bandpass, mParams.cutoffHz, drive01, xStages);
    mAwXNotch.SetParams(AwXKind::Notch, mParams.cutoffHz, drive01, xStages);

    mAwHoltLowpass.SetParams(mParams.cutoffHz, drive01, adaptiveStages);
    mAwBiquadStack.SetParams(mParams.cutoffHz, drive01, mParams.morph); // morph repurposed as "reso shape" here -- no stage count for this fixed-3-stage type (same precedent as AirLift)
    mAwYLowpass.SetParams(AwYKind::Lowpass, mParams.cutoffHz, drive01, mParams.extra); // the one family that uses the dedicated `extra` param (powFactor) -- morph is unused here, there's no stage-count/wet-ladder concept in the source to assign it to
    mAwYHighpass.SetParams(AwYKind::Highpass, mParams.cutoffHz, drive01, mParams.extra);
    mAwYBandpass.SetParams(AwYKind::Bandpass, mParams.cutoffHz, drive01, mParams.extra);
    mAwYNotch.SetParams(AwYKind::Notch, mParams.cutoffHz, drive01, mParams.extra);
    mYWet = 1.0 - std::clamp(mParams.morph, 0.0, 1.0); // source wet/dry: morph 0 = fully processed

    // BiquadNonLin: resonance maps DIRECTLY to Q here (not drive01 -- see
    // AirwindowsBiquadNonLinFilter's header note), morph is the bipolar
    // wet sweep, extra carries the nonLin shaping exponent.
    mAwBiquadNonLinLowpass.SetParams(AwBqKind::Lowpass, mParams.cutoffHz, mParams.resonance, mParams.morph, mParams.extra);
    mAwBiquadNonLinBandpass.SetParams(AwBqKind::Bandpass, mParams.cutoffHz, mParams.resonance, mParams.morph, mParams.extra);

    // ZAcidLowpass: all three knobs plug in directly, no curve or
    // repurposing -- see AirwindowsZAcidLowpassFilter's header note.
    mAwZAcidLowpass.SetParams(mParams.cutoffHz, drive01, mParams.morph, mParams.extra);

    // Hermepass: morph is the pole-count/slope knob, same as the
    // source's own knob -- resonance and extra are unused by this type.
    mAwHermepassHighpass.SetParams(mParams.cutoffHz, mParams.morph);

    // AverMatrix: cutoffHz -> tap count (approximate, see header note),
    // drive01 -> pole/cascade count, morph -> bipolar wet, REVERSED so the
    // default morph (0) is full wet (+1) and morph 1 is the complement (-1).
    // extra unused.
    mAwAverMatrix.SetParams(mParams.cutoffHz, drive01, 1.0 - (mParams.morph * 2.0));

    // SlewSonic: cutoffHz direct, morph -> stage count (0-3). resonance
    // and extra unused -- fixed verbatim Butterworth-ladder Qs.
    mAwSlewSonic.SetParams(mParams.cutoffHz, mParams.morph);

    // ORIGINAL character types (CharacterFilters.h) -- see FilterCore.h's
    // enum comment. Comb/RingMod-FM/Phaser/PhysResonator all follow this
    // file's established drive01/stage-count conventions; VocalFormant
    // and Resample repurpose cutoffHz the same way AirLift/Capacitor do
    // (see each class's own header note for why).
    mCombFeedforward.SetParams(mParams.cutoffHz, drive01, mParams.morph, /*feedbackTopology=*/false);
    mCombFeedback.SetParams(mParams.cutoffHz, drive01, mParams.morph, /*feedbackTopology=*/true);
    mRingModulator.SetParams(mParams.cutoffHz, drive01, mParams.morph, /*fmMode=*/false);
    mFilterFM.SetParams(mParams.cutoffHz, drive01, mParams.morph, /*fmMode=*/true);
    mVocalFormant.SetParams(mParams.cutoffHz, drive01, mParams.morph);
    mPhaser.SetParams(mParams.cutoffHz, drive01, mParams.morph);
    mPhysOddResonator.SetParams(mParams.cutoffHz, drive01, mParams.morph, /*oddHarmonics=*/true);
    mPhysEvenResonator.SetParams(mParams.cutoffHz, drive01, mParams.morph, /*oddHarmonics=*/false);
    mResample.SetParams(mParams.cutoffHz, drive01, mParams.morph);
  }

  void Reset() override
  {
    for (auto& s : mStages) s.Reset();
    mOnePoleA.Reset();
    mOnePoleB.Reset();
    mAirTap.Reset();
    mCapacitorEnv = 0.0;
    mAwZLowpass.Reset();
    mAwZLowpassFull.Reset();
    mAwZHighpass.Reset();
    mAwZBandpass.Reset();
    mAwZNotch.Reset();
    mAwLowpass2.Reset();
    mAwHighpass2.Reset();
    mAwXLowpass.Reset();
    mAwXHighpass.Reset();
    mAwXBandpass.Reset();
    mAwXNotch.Reset();
    mAwHoltLowpass.Reset();
    mAwBiquadStack.Reset();
    mAwYLowpass.Reset();
    mAwYHighpass.Reset();
    mAwYBandpass.Reset();
    mAwYNotch.Reset();
    mAwBiquadNonLinLowpass.Reset();
    mAwBiquadNonLinBandpass.Reset();
    mAwZAcidLowpass.Reset();
    mAwHermepassHighpass.Reset();
    mAwAverMatrix.Reset();
    mAwSlewSonic.Reset();
    mCombFeedforward.Reset();
    mCombFeedback.Reset();
    mRingModulator.Reset();
    mFilterFM.Reset();
    mVocalFormant.Reset();
    mPhaser.Reset();
    mPhysOddResonator.Reset();
    mPhysEvenResonator.Reset();
    mResample.Reset();
  }

  double ProcessSample(double input) override
  {
    switch (mParams.type)
    {
      case FilterTypeId::MorphZ:
        return mStages[0].ProcessSample(input); // byte-for-byte the plugin's original behavior

      case FilterTypeId::LP6:  return mOnePoleA.ProcessLP(input);
      case FilterTypeId::HP6:  return mOnePoleA.ProcessHP(input);

      case FilterTypeId::LP12: return Cascade(input, 1, TapKind::LP);
      case FilterTypeId::HP12: return Cascade(input, 1, TapKind::HP);
      case FilterTypeId::LP24: return Cascade(input, 2, TapKind::LP);
      case FilterTypeId::HP24: return Cascade(input, 2, TapKind::HP);
      case FilterTypeId::LP48: return Cascade(input, 4, TapKind::LP);
      case FilterTypeId::HP48: return Cascade(input, 4, TapKind::HP);

      case FilterTypeId::BP12: return Cascade(input, 1, TapKind::BP);
      case FilterTypeId::BP24: return Cascade(input, 2, TapKind::BP);

      case FilterTypeId::Notch12:
      {
        const auto t = mStages[0].ProcessSampleTaps(input);
        const double out = t.lp + t.hp; // identity: no extra coefficients needed
        return std::isfinite(out) ? out : 0.0;
      }

      case FilterTypeId::Capacitor:
        return ProcessCapacitor(input);

      case FilterTypeId::AirLift:
      {
        // Dry signal plus a scaled bandpass "bump" at the chosen
        // frequency -- the character here is deliberately a simple
        // additive lift rather than a surgical, flat-elsewhere shelf;
        // that's what makes it sit in the "colorful, not surgical"
        // Airwindows-flavored bucket rather than duplicating the clean
        // BP12/BP24 types above.
        const auto t = mAirTap.ProcessSampleTaps(input);
        const double amount = mParams.morph * 3.0; // up to +3x the raw bp tap -- tuned by ear-equivalent reasoning, not measured against a reference; revisit once it's actually audible to the user
        const double out = input + t.bp * amount;
        return std::isfinite(out) ? out : 0.0;
      }

      case FilterTypeId::AwLowpass2:  return mAwLowpass2.ProcessSample(input);
      case FilterTypeId::AwHighpass2: return mAwHighpass2.ProcessSample(input);
      case FilterTypeId::AwZLowpass:  {
        const double y1 = mAwZLowpass.ProcessSample(input);
        if (mZLowpassMorph <= 0.0) return y1;
        const double y2 = mAwZLowpassFull.ProcessSample(input);
        return y1 + (y2 - y1) * mZLowpassMorph;
      }
      case FilterTypeId::AwZHighpass: return mAwZHighpass.ProcessSample(input);
      case FilterTypeId::AwZBandpass: return mAwZBandpass.ProcessSample(input);
      case FilterTypeId::AwZNotch:    return mAwZNotch.ProcessSample(input);
      case FilterTypeId::AwXLowpass:  return mAwXLowpass.ProcessSample(input * mXLowpassInGain);
      case FilterTypeId::AwXHighpass: return mAwXHighpass.ProcessSample(input);
      case FilterTypeId::AwXBandpass: return mAwXBandpass.ProcessSample(input);
      case FilterTypeId::AwXNotch:    return mAwXNotch.ProcessSample(input);
      case FilterTypeId::AwHoltLowpass: return mAwHoltLowpass.ProcessSample(input);
      case FilterTypeId::AwBiquadStack: return mAwBiquadStack.ProcessSample(input);
      case FilterTypeId::AwYLowpass:    return YWetMix(mAwYLowpass.ProcessSample(input), input);
      case FilterTypeId::AwYHighpass:   return YWetMix(mAwYHighpass.ProcessSample(input), input);
      case FilterTypeId::AwYBandpass:   return YWetMix(mAwYBandpass.ProcessSample(input), input);
      case FilterTypeId::AwYNotch:      return YWetMix(mAwYNotch.ProcessSample(input), input);
      case FilterTypeId::AwBiquadNonLinLowpass:  return mAwBiquadNonLinLowpass.ProcessSample(input);
      case FilterTypeId::AwBiquadNonLinBandpass: return mAwBiquadNonLinBandpass.ProcessSample(input);
      case FilterTypeId::AwZAcidLowpass: return mAwZAcidLowpass.ProcessSample(input);
      case FilterTypeId::AwHermepassHighpass: return mAwHermepassHighpass.ProcessSample(input);
      case FilterTypeId::AwAverMatrix: return mAwAverMatrix.ProcessSample(input);
      case FilterTypeId::AwSlewSonic: return mAwSlewSonic.ProcessSample(input);
      case FilterTypeId::CombFeedforward: return mCombFeedforward.ProcessSample(input);
      case FilterTypeId::CombFeedback: return mCombFeedback.ProcessSample(input);
      case FilterTypeId::RingModulator: return mRingModulator.ProcessSample(input);
      case FilterTypeId::FilterFM: return mFilterFM.ProcessSample(input);
      case FilterTypeId::VocalFormant: return mVocalFormant.ProcessSample(input);
      case FilterTypeId::Phaser: return mPhaser.ProcessSample(input);
      case FilterTypeId::PhysOddResonator: return mPhysOddResonator.ProcessSample(input);
      case FilterTypeId::PhysEvenResonator: return mPhysEvenResonator.ProcessSample(input);
      case FilterTypeId::Resample: return mResample.ProcessSample(input);

      default:
        return input;
    }
  }

private:
  enum class TapKind { LP, HP, BP };

  double Cascade(double input, int numStages, TapKind kind)
  {
    double x = input;
    for (int i = 0; i < numStages; ++i)
    {
      const auto t = mStages[static_cast<size_t>(i)].ProcessSampleTaps(x);
      x = (kind == TapKind::LP) ? t.lp : (kind == TapKind::HP) ? t.hp : t.bp;
    }
    return std::isfinite(x) ? x : 0.0;
  }

  // "Capacitor" character type: a one-pole filter whose own cutoff
  // coefficient is nudged by a fast internal envelope of the recent
  // input level (louder transients "charge" it faster -- the capacitor
  // metaphor the name is going for), followed by a gentle tanh
  // saturation stage. This is NOT a port of Airwindows' real Capacitor
  // plugin (no source available to port from here) -- it's an original
  // design built in that same spirit: simple, a little lo-fi, audibly
  // alive rather than a flat textbook response. `morph` blends the
  // underlying LP/HP side (0=LP character, 1=HP character, equal-power
  // crossfade like MorphZ); `resonance` doubles as the saturation drive
  // amount, so the knob stays meaningful instead of doing nothing for
  // this type.
  double ProcessCapacitor(double input)
  {
    const double absIn = std::fabs(input);
    // Fast-charge/slow-settle envelope -- deliberately asymmetric so a
    // sudden transient audibly "kicks" the filter brighter/duller before
    // settling back, rather than tracking level smoothly like a proper
    // sidechain envelope (EnvelopeFollower.h) would.
    const double chargeCoeff = (absIn > mCapacitorEnv) ? 0.35 : 0.002;
    mCapacitorEnv += (absIn - mCapacitorEnv) * chargeCoeff;
    if (!std::isfinite(mCapacitorEnv)) mCapacitorEnv = 0.0;

    // Modulate the one-pole's own cutoff by up to +-40% from the charge
    // envelope -- clamped well away from instability (OnePoleFilter's
    // own coefficient clamp is a second line of defense regardless).
    const double modulatedHz = std::clamp(mParams.cutoffHz * (1.0 + 0.4 * mCapacitorEnv), 5.0, mSampleRate * 0.49);
    mOnePoleA.SetCutoff(modulatedHz);
    mOnePoleB.SetCutoff(modulatedHz);

    const double lp = mOnePoleA.ProcessLP(input);
    const double hp = mOnePoleB.ProcessHP(input);

    constexpr double kHalfPi = 1.5707963267948966;
    const double t = mParams.morph * kHalfPi;
    double blended = lp * std::cos(t) + hp * std::sin(t);

    // Soft saturation, driven by resonance (see doc comment above) --
    // tanh is self-limiting, so this can't blow up regardless of drive.
    const double drive = 1.0 + mParams.resonance * 2.0; // resonance range is already clamped >=0.01 upstream
    blended = std::tanh(blended * drive) / std::tanh(drive); // normalized so unity-level input stays roughly unity-level out at drive=1

    return std::isfinite(blended) ? blended : 0.0;
  }

  FilterParams mParams;
  double mSampleRate = 44100.0;
  std::array<StateVariableFilter, 4> mStages; // reused for MorphZ(1 stage)/LP-HP-BP-Notch cascades up to 48dB/oct
  OnePoleFilter mOnePoleA, mOnePoleB; // LP/HP sides for LP6/HP6 and for Capacitor's blend
  StateVariableFilter mAirTap; // AirLift's presence-bump source
  double mCapacitorEnv = 0.0;

  // Real Airwindows ports -- see AirwindowsFilters.h.
  AirwindowsZFilter mAwZLowpass, mAwZHighpass, mAwZBandpass, mAwZNotch;
  AirwindowsZFilter mAwZLowpassFull; // 2-stage partner for the continuous Z-Lowpass morph
  double mZLowpassMorph = 0.0;
  double mXLowpassInGain = 1.0;
  double mYWet = 1.0;
  double YWetMix(double processed, double dry) const { return processed * mYWet + dry * (1.0 - mYWet); }
  AirwindowsAdaptiveFilter mAwLowpass2, mAwHighpass2;
  AirwindowsXFilter mAwXLowpass, mAwXHighpass, mAwXBandpass, mAwXNotch;
  AirwindowsHoltFilter mAwHoltLowpass;
  AirwindowsBiquadStackFilter mAwBiquadStack;
  AirwindowsYFilter mAwYLowpass, mAwYHighpass, mAwYBandpass, mAwYNotch;
  AirwindowsBiquadNonLinFilter mAwBiquadNonLinLowpass, mAwBiquadNonLinBandpass;
  AirwindowsZAcidLowpassFilter mAwZAcidLowpass;
  AirwindowsHermepassFilter mAwHermepassHighpass;
  AirwindowsAverMatrixFilter mAwAverMatrix;
  AirwindowsSlewSonicFilter mAwSlewSonic;

  // ORIGINAL character types -- see CharacterFilters.h.
  CombFilter mCombFeedforward, mCombFeedback;
  RingModFilter mRingModulator, mFilterFM;
  VocalFormantFilter mVocalFormant;
  PhaserFilter mPhaser;
  PhysResonatorFilter mPhysOddResonator, mPhysEvenResonator;
  ResampleFilter mResample;
};

} // namespace exoeq
