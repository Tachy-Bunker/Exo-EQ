// STB_IMAGE_IMPLEMENTATION is deliberately NOT defined here, even though
// the comment that used to sit on this line argued for defining it in
// exactly one translation unit. Your linker errors (stbi_load,
// stbi_image_free, etc. "already defined in ExoEQ.obj", conflicting
// with IGraphicsWin.obj) prove iPlug2's own IGraphicsWin.cpp already
// defines STB_IMAGE_IMPLEMENTATION and compiles the real stb_image
// implementation in -- IGraphics itself uses stb_image internally for
// its own bitmap loading. Defining it again here gave the linker TWO
// implementations of every stbi_* function, which is exactly what
// LNK2005 means. Leaving it undefined here means DSP/ImageToEq.h's
// `#include "../Vendor/stb/stb_image.h"` only sees the DECLARATIONS
// (stb_image.h's normal header-only mode) -- ImageToEq.h's calls to
// stbi_load/stbi_image_free/etc. link against the one real
// implementation IGraphicsWin.obj already provides, instead of
// compiling a second, colliding copy.
#include "ExoEQ.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "DSP/KeyTrack.h"
#include "BandGraphControl.h" // no longer attached in OnLayout (see CircleViewControl below) -- left in the tree, unused, rather than deleted, in case of rollback
#include "CircleViewControl.h"
#include <array>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

namespace
{
  // Fixes a real build error: pGraphics->CreateTextEntry(...) in this
  // iPlug2 version does NOT take a completion lambda as its 5th argument
  // (that argument is an int -- a max-length, per the compiler's own
  // error showing the real signature) -- completion is delivered via
  // IControl::OnTextEntryCompletion(), called BY IGraphics on whichever
  // control CreateTextEntry's first argument named. The formula/range
  // buttons this project already had (IVButtonControl) don't override
  // that virtual, so this tiny invisible proxy control exists purely to
  // receive that callback and forward it to an ordinary std::function --
  // one of these gets attached (hidden, zero visual footprint) per text
  // entry, same "small, this-is-exactly-what-it's-for" shape as every
  // other UI-only helper control in this project.
  class TextEntryCallbackControl : public iplug::igraphics::IControl
  {
  public:
    TextEntryCallbackControl(std::function<void(const char*)> onComplete)
    : IControl(iplug::igraphics::IRECT(), iplug::kNoParameter)
    , mOnComplete(std::move(onComplete))
    {
      mIgnoreMouse = true;
      mHide = true;
    }
    void Draw(iplug::igraphics::IGraphics& /*g*/) override {}
    void OnTextEntryCompletion(const char* str, int /*valIdx*/) override
    {
      if (mOnComplete) mOnComplete(str);
    }
  private:
    std::function<void(const char*)> mOnComplete;
  };
}

ExoEQ::ExoEQ(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // 128 bands' worth of real, individually automatable params, generated
  // in a loop rather than hand-enumerated -- see ExoEQ.h for why (can't
  // drift out of sync the way separate per-band constants would at this
  // scale). EVERY band starts inactive -- a freshly loaded instance has
  // no bands at all until the user makes one (click-empty-space on the
  // graph, or a future "+" control); see ProcessBlock below for why that
  // means pure passthrough, not silence.
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    const std::string suffix = std::to_string(bandIndex + 1);
    GetParam(BandParamIdx(bandIndex, kBandCutoff))->InitDouble(("Cutoff " + suffix).c_str(), 1000., 20., 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
    GetParam(BandParamIdx(bandIndex, kBandResonance))->InitDouble(("Resonance " + suffix).c_str(), 0.707, 0.05, 20.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandMorph))->InitDouble(("Morph " + suffix).c_str(), 0., 0., 100.0, 0.01, "%"); // 0=LP 50=BP 100=HP (MorphZ type only -- repurposed per-type for Capacitor/AirLift, see FilterTypes.h)
    GetParam(BandParamIdx(bandIndex, kBandGainDb))->InitDouble(("Gain " + suffix).c_str(), 0., -24., 24.0, 0.01, "dB"); // previously-missing per-band trim -- see ExoEQ.h's note on kBandGainDb
    GetParam(BandParamIdx(bandIndex, kBandFilterExtra))->InitDouble(("Filter Extra " + suffix).c_str(), 10., 0., 100.0, 0.01, "%"); // only AW Y-Lowpass uses this (powFactor) -- 10% default is this type's own neutral position (see AirwindowsYLowpassFilter); a no-op for every other type

    // Order MUST exactly match FilterTypeId in DSP/FilterCore.h -- that
    // enum is the single source of truth ReadBandSettings casts this
    // param's integer value back into, so a mismatch here silently picks
    // the wrong filter type rather than failing to build.
    GetParam(BandParamIdx(bandIndex, kBandFilterType))->InitEnum(("Filter Type " + suffix).c_str(), 0, {
      "Morph (LP-BP-HP)",
      "Lowpass 6",  "Lowpass 12",  "Lowpass 24",  "Lowpass 48",
      "Highpass 6", "Highpass 12", "Highpass 24", "Highpass 48",
      "Bandpass 12", "Bandpass 24",
      "Notch 12",
      "Capacitor",
      "Air Lift",
      "AW Lowpass 2",
      "AW Highpass 2",
      "AW Z-Lowpass",
      "AW Z-Highpass",
      "AW Z-Bandpass",
      "AW Z-Notch",
      "AW X-Lowpass",
      "AW X-Highpass",
      "AW X-Bandpass",
      "AW X-Notch",
      "AW Holt Lowpass",
      "AW Biquad Stack",
      "AW Y-Lowpass",
      "AW Y-Highpass",
      "AW Y-Bandpass",
      "AW Y-Notch",
      "AW Biquad NonLin Lowpass",
      "AW Biquad NonLin Bandpass",
      "AW Z-Acid Lowpass",
      "AW Hermepass Highpass",
      "AW AverMatrix",
      "AW SlewSonic",
      "Comb Feedforward",
      "Comb Feedback",
      "Ring Modulator",
      "Filter FM",
      "Vocal Formant",
      "Phaser",
      "Phys Odd Resonator",
      "Phys Even Resonator",
      "Resample"
    });

    GetParam(BandParamIdx(bandIndex, kBandKeyTrackAmt))->InitDouble(("KeyTrack " + suffix).c_str(), 0., 0., 100.0, 0.01, "%");
    GetParam(BandParamIdx(bandIndex, kBandMute))->InitBool(("Mute " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandSolo))->InitBool(("Delta " + suffix).c_str(), false); // "Delta" = solo, per the spec's naming
    GetParam(BandParamIdx(bandIndex, kBandActive))->InitBool(("Active " + suffix).c_str(), false);

    // Sends + sidechain: per-node contained functions, per the spec. All
    // default OFF/neutral -- enabling one is an explicit user action, same
    // "off by default" philosophy as the band itself (kBandActive above).
    GetParam(BandParamIdx(bandIndex, kBandDelayEnabled))->InitBool(("Delay On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandDelayTimeMs))->InitDouble(("Delay Time " + suffix).c_str(), 250., 1., 2000.0, 0.01, "ms");
    GetParam(BandParamIdx(bandIndex, kBandDelayFeedback))->InitDouble(("Delay FB " + suffix).c_str(), 0.3, 0., 0.98, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandDelayMix))->InitDouble(("Delay Mix " + suffix).c_str(), 0.3, 0., 1.0, 0.001, "");

    GetParam(BandParamIdx(bandIndex, kBandDistortionEnabled))->InitBool(("Dist On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandDistortionDrive))->InitDouble(("Dist Drive " + suffix).c_str(), 1.0, 0.1, 20.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandDistortionCharacter))->InitDouble(("Dist Char " + suffix).c_str(), 0., 0., 1.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandDistortionMix))->InitDouble(("Dist Mix " + suffix).c_str(), 0.3, 0., 1.0, 0.001, "");

    GetParam(BandParamIdx(bandIndex, kBandFreqShiftEnabled))->InitBool(("Shift On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandFreqShiftHz))->InitDouble(("Shift Hz " + suffix).c_str(), 0., -5000., 5000.0, 0.01, "Hz");
    GetParam(BandParamIdx(bandIndex, kBandFreqShiftMix))->InitDouble(("Shift Mix " + suffix).c_str(), 0.3, 0., 1.0, 0.001, "");

    GetParam(BandParamIdx(bandIndex, kBandSidechainEnabled))->InitBool(("SC On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandSidechainModOctaves))->InitDouble(("SC Depth " + suffix).c_str(), 0., -8., 8.0, 0.001, "oct");

    // Effect order slots: integer 0/1/2, defaults reproduce the original
    // fixed freq-shift->distortion->delay order exactly (see BandChain.h
    // for the tie-break rule and why that makes this a safe default).
    GetParam(BandParamIdx(bandIndex, kBandFreqShiftSlot))->InitInt(("Shift Slot " + suffix).c_str(), 0, 0, 2);
    GetParam(BandParamIdx(bandIndex, kBandDistortionSlot))->InitInt(("Dist Slot " + suffix).c_str(), 1, 0, 2);
    GetParam(BandParamIdx(bandIndex, kBandDelaySlot))->InitInt(("Delay Slot " + suffix).c_str(), 2, 0, 2);

    // Energy2 (DSP/AirwindowsFilters.h / DSP/BandChain.h -- see ExoEQ.h's
    // enum comment for the globally-capped-at-20 design). Off by default,
    // same philosophy as every other send above; the two grouped knobs
    // and mix default to neutral/off (0.5/0.5/0 -- see BandSettings'
    // own defaults) so an enabled-but-untouched band is silent until the
    // user actually pushes a knob off-center.
    GetParam(BandParamIdx(bandIndex, kBandEnergy2Enabled))->InitBool(("Energy2 On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandEnergy2GlitterHiss))->InitDouble(("Energy2 GlitterHiss " + suffix).c_str(), 0.5, 0., 1.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandEnergy2RatScrapeFizz))->InitDouble(("Energy2 RatScrapeFizz " + suffix).c_str(), 0.5, 0., 1.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandEnergy2Mix))->InitDouble(("Energy2 Mix " + suffix).c_str(), 0., -1.0, 1.0, 0.001, "");

    // Node groups (DSP/BandGroups.h, DSP/BandChain.h -- built and tested in
    // an earlier session, these 3 params are the entire piece that was
    // actually missing). GroupMask default 0 = member of no group, exactly
    // the "doesn't change behavior until a user opts in" default every
    // other feature here uses.
    GetParam(BandParamIdx(bandIndex, kBandGroupMask))->InitInt(("Group Mask " + suffix).c_str(), 0, 0, 255);
    GetParam(BandParamIdx(bandIndex, kBandGroupEnvEnabled))->InitBool(("Group Env On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandGroupEnvModOctaves))->InitDouble(("Group Env Depth " + suffix).c_str(), 0., -8., 8.0, 0.001, "oct");
    // Source tag -- see ExoEQ.h's own note on kBandSource/EBandSource.
    // Order MUST match EBandSource exactly (HandBuilt=0, Fractal,
    // Transform, Image). Every band starts HandBuilt, same as every
    // existing project loading this update for the first time -- a
    // save from before this param existed has nothing to read here, so
    // it defaults to HandBuilt for every band, which is simply correct
    // (no generator had tagged anything before this param existed).
    GetParam(BandParamIdx(bandIndex, kBandSource))->InitEnum(("Source " + suffix).c_str(), 0, { "Hand-Built", "Fractal", "Transform", "Image" });

    // Circle assignment (DSP/CircleChain.h) -- defaults to 0, so every
    // band starts in the one implicit circle every existing project
    // already effectively has. kMaxCircles matches kMaxBands (256) by
    // convention, not necessity -- a circle just needs an ID in range;
    // there's no reason to allow MORE circles than there are bands to
    // put in them.
    GetParam(BandParamIdx(bandIndex, kBandCircleIndex))->InitInt(("Circle " + suffix).c_str(), 0, 0, exoeq::kMaxCircles - 1);
  }

  // All kMaxBands Band objects get added up front, at construction --
  // BandChain already costs almost nothing per inactive band
  // (Band::ProcessSample returns immediately on mute, before touching the
  // filter or any send) -- but they MUST start muted here, explicitly.
  // This was the actual bug behind "amplitude increases into oblivion
  // without touching any parameter": ProcessBlock's performance
  // optimization skips calling SetBandSettings on a band that's already
  // inactive (to avoid recomputing filter coefficients for slots nobody's
  // using), which means a band that's NEVER been activated never gets its
  // mute flag pushed at all -- it just sits at whatever AddBand()
  // constructed it with. BandSettings{}'s own default is mute=false, so
  // every one of the 127+ never-touched bands was a fully live, identical
  // 1kHz low-pass filter, all summing in phase -- ~128x amplitude in the
  // passband, exactly matching the reported symptom. Passing mute=true
  // here fixes the actual bug at its source, independent of the
  // ProcessBlock optimization.
  {
    exoeq::BandSettings initiallyMuted;
    initiallyMuted.mute = true;
    for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
    {
      // Every band starts assigned to circle 0 (matching kBandCircleIndex's
      // own default above) -- CircleChain::AssignBandToCircle both creates
      // the slot (first call for this bandIndex, so there's nothing to
      // move away from) and sets its initial settings in one call, same
      // as the old mChain[c].AddBand(initiallyMuted) this replaces.
      mCircleChain[0].AssignBandToCircle(bandIndex, 0, initiallyMuted);
      mCircleChain[1].AssignBandToCircle(bandIndex, 0, initiallyMuted);
    }
  }

  // See ExoEQ.h for why latch=true/portamento=0ms here exactly reproduces
  // the old mLastMidiNote behavior -- this is not a new audible default.
  mKeyTracker.SetLatch(true);
  mKeyTracker.SetPortamentoMs(0.0);

  // Real MTS-ESP (ExoEQ.h's mMtsEspClient) -- handed to the KeyTracker
  // once here; the global kGlobalMtsEspEnabled param (read every block
  // in ProcessBlock, see below) is what actually flips SetUseMtsEsp on,
  // so this alone changes nothing yet. Default param value is false
  // (see InitBool below), so an existing project opens with identical
  // behavior to before this feature existed.
  mKeyTracker.SetMtsEspClient(&mMtsEspClient);

  GetParam(kGlobalMtsEspEnabled)->InitBool("MTS-ESP Enabled", false);

  // UI-only selectors -- never read in ProcessBlock, purely which
  // group/connection the one fixed panel below is currently showing.
  GetParam(kGlobalGroupSelect)->InitInt("Group Select", 0, 0, exoeq::kNumGroups - 1);
  GetParam(kGlobalModConnSelect)->InitInt("Mod Conn Select", 0, 0, kMaxModConnections - 1);

  // Groups: all 8 start unmuted/0dB/no envelope, i.e. completely inert --
  // matches every band's own groupMask default of 0 (member of nothing),
  // so this feature changes nothing for an existing project either way.
  for (int g = 0; g < exoeq::kNumGroups; ++g)
  {
    const std::string gSuffix = std::to_string(g + 1);
    GetParam(GroupParamIdx(g, kGroupMute))->InitBool(("Group Mute " + gSuffix).c_str(), false);
    GetParam(GroupParamIdx(g, kGroupGainDb))->InitDouble(("Group Gain " + gSuffix).c_str(), 0., -24., 24.0, 0.01, "dB");
  }

  // 4 LFOs feeding the mod matrix's first 4 source slots (of
  // ModMatrix::kMaxSources=8 -- the other 4 stay at their default value of
  // 0.0 forever, reserved for a future second source type e.g. envelope
  // followers, same scope-limiting choice already made for sidechain's
  // fixed envelope params). Waveform enum order MUST match
  // DSP/LFO.h's LfoWaveform exactly.
  for (int i = 0; i < kMaxModSources; ++i)
  {
    const std::string lSuffix = std::to_string(i + 1);
    GetParam(LfoParamIdx(i, kLfoRateHz))->InitDouble(("LFO Rate " + lSuffix).c_str(), 1., 0.01, 20.0, 0.001, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
    GetParam(LfoParamIdx(i, kLfoWaveform))->InitEnum(("LFO Wave " + lSuffix).c_str(), 0, { "Sine", "Triangle", "Saw", "Square", "Sample & Hold" });
  }

  // Mod connections: each a (source, destination, depth, target band)
  // tuple, individually enabled -- all start disabled, so this is a pure
  // no-op bank of extra params until a user builds a patch. Destination
  // enum order MUST match DSP/ModMatrix.h's ModDestination exactly.
  // TargetBand defaults to 0 (band 1) rather than anything more "helpful"
  // -- there's no sane default target, and Enabled=false means it's moot
  // either way.
  for (int i = 0; i < kMaxModConnections; ++i)
  {
    const std::string cSuffix = std::to_string(i + 1);
    GetParam(ModConnParamIdx(i, kModConnEnabled))->InitBool(("Mod Conn On " + cSuffix).c_str(), false);
    GetParam(ModConnParamIdx(i, kModConnSourceIndex))->InitInt(("Mod Conn Src " + cSuffix).c_str(), 0, 0, exoeq::ModMatrix::kMaxSources - 1);
    GetParam(ModConnParamIdx(i, kModConnDestination))->InitEnum(("Mod Conn Dest " + cSuffix).c_str(), 0, {
      "Cutoff (oct)",
      "Resonance +",
      "Morph +",
      "Gain (dB)",
      "Dist Drive +",
      "Freq Shift (Hz) +"
    });
    GetParam(ModConnParamIdx(i, kModConnDepth))->InitDouble(("Mod Conn Depth " + cSuffix).c_str(), 0., -8., 8.0, 0.001, "");
    GetParam(ModConnParamIdx(i, kModConnTargetBand))->InitInt(("Mod Conn Target " + cSuffix).c_str(), 0, 0, kMaxBands - 1);
  }

  // Fractal generator (P5 scope step 1 -- DSP/FractalGenerator.h). All
  // one-shot batch params, read only when "Generate" is clicked -- see
  // ExoEQ.h's note on kGlobalFractalFilterType. Defaults reproduce
  // FractalGenParams{}'s own defaults exactly so a freshly-opened
  // instance's Generate button, clicked with zero knob-turning, gives
  // the same result the standalone DSP test file's "knob-driven default"
  // checks against.
  GetParam(kGlobalFractalSeedHz)->InitDouble("Fractal Seed Hz", 220., 20., 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalFractalRatio)->InitDouble("Fractal Ratio", 1.6180339887498949, 1.01, 8.0, 0.0001, ""); // golden ratio default; 2.0 = plain octave stack
  GetParam(kGlobalFractalBranchFactor)->InitInt("Fractal Branch", 2, 1, 4);
  GetParam(kGlobalFractalDepth)->InitInt("Fractal Depth", 3, 1, 4); // kept shallow -- see FractalGenerator.h's own note on the 128-band cap
  GetParam(kGlobalFractalGainDb)->InitDouble("Fractal Seed Gain", 0., -24., 24.0, 0.01, "dB");
  GetParam(kGlobalFractalGainDecay)->InitDouble("Fractal Gain Decay", 0.7, 0., 1.0, 0.001, "");
  GetParam(kGlobalFractalResonance)->InitDouble("Fractal Resonance", 0.707, 0.05, 20.0, 0.001, "");
  GetParam(kGlobalFractalJitterPct)->InitDouble("Fractal Jitter", 0., 0., 100.0, 0.01, "%");
  GetParam(kGlobalFractalJitterSeed)->InitInt("Fractal Jitter Seed", 1, 0, 999999);
  GetParam(kGlobalFractalSymmetric)->InitBool("Fractal Symmetric", false);
  // Deliberately only 4 options, ALL single-StateVariableFilter dropdown
  // types -- the user's explicit "cheap filter system" requirement.
  // Never an Airwindows/character type here, by design: see
  // GenerateFractalBands()'s own mapping comment for the FilterTypeId
  // ordinal each option actually writes.
  GetParam(kGlobalFractalFilterType)->InitEnum("Fractal Filter Type", 2, { "Lowpass 12", "Highpass 12", "Bandpass 12", "Notch 12" });

  // 9 user-defined knobs (EXOMUSICA letters) the inline formula box can reference by
  // letter. Each is a plain, real automatable 0..1 param -- the range
  // it's scaled into (mFractalVarMin/Max, default 0..1 passthrough) is
  // set separately via inline text, same as the formula itself; see
  // ExoEQ.h's own note on why that range lives as a plain member rather
  // than a param.
  {
    static const char* kVarLetters[kNumFractalCustomVars] = { "E", "X", "O", "M", "U", "S", "I", "C", "A" };
    for (int i = 0; i < kNumFractalCustomVars; ++i)
      GetParam(kGlobalFractalVarStart + i)->InitDouble((std::string("Fractal Var ") + kVarLetters[i]).c_str(), 0.5, 0., 1.0, 0.0001, "");
  }

  // Transform filter (P5 scope step 2 -- DSP/TransformFilter.h). Pivot
  // defaults to 1kHz (a neutral, roughly-centered choice -- it only
  // matters once exponent != 1 anyway); exponent defaults to 1.0 (exact
  // no-op), so a freshly-opened instance's Transform button, clicked
  // with zero knob-turning, changes nothing -- same "off/neutral by
  // default" guarantee every other feature in this plugin gives.
  GetParam(kGlobalTransformPivotHz)->InitDouble("Transform Pivot Hz", 1000., 20., 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalTransformExponent)->InitDouble("Transform Exponent", 1.0, -4.0, 4.0, 0.0001, ""); // 1=no-op, >1=stretch, 0..1=compress, <0=mirror/invert -- see TransformFilter.h

  // Image-to-EQ-curve (P5 scope step 3 of 3) -- see DSP/ImageToEq.h for
  // what each of these drives. Defaults match ImageToEqParams's own
  // struct defaults exactly, so a fresh instance's knobs read the same
  // values that struct would already default to if left untouched.
  GetParam(kGlobalImageNodeCount)->InitInt("Image Node Count", 24, 2, 64, ""); // same 64 cap as FractalGenParams::kMaxNodes, same reason
  GetParam(kGlobalImageMinFreqHz)->InitDouble("Image Min Hz", 40.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalImageMaxFreqHz)->InitDouble("Image Max Hz", 16000.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalImageGainRangeDb)->InitDouble("Image Gain Range", 24.0, 0.0, 48.0, 0.01, "dB");
  GetParam(kGlobalImageResonance)->InitDouble("Image Resonance", 0.707, 0.05, 20.0, 0.001, "");
  GetParam(kGlobalImageInvertLuminance)->InitBool("Image Invert Luminance", false);

  // Global FFT brick-wall stage (P6, first piece) -- off by default,
  // same "no audible or CPU change until deliberately turned on" rule
  // every other optional feature in this plugin already follows. The
  // default 20Hz-20kHz passband is a near-total-passthrough starting
  // point (so turning it on with no further adjustment is close to a
  // no-op, not a surprise full mute) -- the brick-wall edges only bite
  // once you actually narrow them.
  GetParam(kGlobalFftEnabled)->InitBool("FFT Stage Enabled", false);
  GetParam(kGlobalFftLowCutHz)->InitDouble("FFT Low Cut", 20.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalFftHighCutHz)->InitDouble("FFT High Cut", 20000.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));

  // Circle-GUI round 11: Mel vs. log frequency axis for the graph (see
  // ExoEQ.h's own note on kGlobalFreqAxisMel). Defaults true (Mel) so
  // every existing project's graph looks identical to before this param
  // existed. Pure display setting, read only by CircleViewControl.h.
  GetParam(kGlobalFreqAxisMel)->InitBool("Mel Axis", true);

#if IPLUG_EDITOR // http://bit.ly/2S64BDd
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  // Real UI, first pass: a Pro-Q-style spectrum graph (BandGraphControl --
  // see that file's header for the detail, and for the explicit caveat
  // that it's unverified against the real iPlug2 SDK) plus a fixed detail
  // panel beneath it that RETARGETS to whichever band is currently
  // selected, rather than existing per-band -- exactly the two choices
  // confirmed with the user (graph+nodes overview, one retargeting panel).
  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachPanelBackground(COLOR_GRAY);
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    const IRECT bounds = pGraphics->GetBounds();
    const IRECT innerBounds = bounds.GetPadded(-10.f);

    // Three vertical sections now instead of two (circle-GUI round 2):
    // graph (42%) / per-band detail panel (30%) / global tab strip (28%,
    // NEW -- see the big comment block below the detail panel for why).
    // Built via direct L/T/R/B arithmetic rather than chained
    // GetFromTop/GetFromBottom calls on fractions of innerBounds.H(),
    // which was the previous code's approach -- that only worked for two
    // sections; three would mean each cut's fraction has to be taken of a
    // shrinking remainder instead of the constant total, which reads
    // error-prone. Same "manual IRECT construction via direct field
    // arithmetic" pattern CircleViewControl.h already uses elsewhere in
    // this project for the same reason (avoids guessing at IRECT helper
    // method names/behavior).
    // NEW THIS ROUND: a thin header strip above the graph for the
    // selected-circle Solo/Mute buttons (see CircleViewControl's new
    // GetSelectedCircle() getter below) -- carved off innerBounds FIRST,
    // same "carve the fixed-size piece before splitting the remainder by
    // fraction" approach panelRow3 already used below.
    const float headerH = 30.f;
    const IRECT headerRow = IRECT(innerBounds.L, innerBounds.T, innerBounds.R, innerBounds.T + headerH);
    const IRECT belowHeader = IRECT(innerBounds.L, innerBounds.T + headerH, innerBounds.R, innerBounds.B);
    // FIXED/CHANGED THIS ROUND, per your explicit request ("the plugin
    // should definitely be fully what the black screen is right now, and
    // all the parameters will appear and disappear based on the actions,
    // a bit like on Fabfilter Pro-Q"): the graph used to get a fixed 42%
    // slice with the detail panel and tab strip permanently occupying
    // the other 58% underneath it, always visible whether or not there
    // was anything to show in them. Now graphArea is the ENTIRE
    // belowHeader region -- the graph IS the whole window below the
    // header, all the time. panelFull/tabsFull keep the exact same
    // bottom-anchored geometry they always had (still computed off
    // belowHeader, not off graphArea), so nothing about their own
    // internal GetGridCell layouts needed to change -- they now simply
    // FLOAT ON TOP of the graph's bottom portion instead of pushing it
    // up out of the way, and are HIDDEN by default (see the panelControls
    // vector and the new "Global FX" toggle below), only drawn when
    // there's something contextual to show -- a band selected for the
    // detail panel, the new header toggle for the FX tab strip. Being
    // attached to pGraphics AFTER circleView already puts them on top in
    // z-order, so no separate "draw on top" handling was needed.
    const float panelH = belowHeader.H() * 0.30f;
    const IRECT graphArea = belowHeader;
    const IRECT panelFull = IRECT(belowHeader.L, belowHeader.B - panelH - belowHeader.H() * 0.28f, belowHeader.R, belowHeader.B - belowHeader.H() * 0.28f);
    const IRECT tabsFull = IRECT(belowHeader.L, belowHeader.B - belowHeader.H() * 0.28f, belowHeader.R, belowHeader.B);
    const IRECT panelRow3 = panelFull.GetFromBottom(panelFull.H() * 0.25f);
    const IRECT panelArea = panelFull.GetFromTop(panelFull.H() * 0.75f);
    // Row 1: the original core params. Row 2: the per-node sends +
    // sidechain added in this pass -- kept on a separate row rather than
    // widening row 1, since this is already 4 send-enable toggles + 9
    // numeric params and cramming it into one row would make every control
    // too small to use.
    const IRECT panelRow1 = panelArea.GetGridCell(0, 0, 2, 1);
    const IRECT panelRow2 = panelArea.GetGridCell(1, 0, 2, 1);

    // ROUND 26 FIX -- real bug found from your screenshot: the detail-panel
    // knobs were rendering completely invisible (just black) even when a
    // band was selected. Root cause: iPlug2 draws attached controls in
    // ATTACH ORDER, later = on top. CircleViewControl now fills its ENTIRE
    // bounds with an opaque background every frame (ever since graphArea
    // became the whole window, last round's FabFilter-style change), and
    // circleView used to be attached AFTER all the panel knobs below --
    // so its own opaque backdrop was painted directly over them every
    // single frame, regardless of Hide(false). Only controls attached
    // AFTER circleView (header buttons, the X button) ever stayed visible.
    //
    // Fix: attach circleView FIRST, before any panel/tab control, so
    // everything else naturally draws on top of it. The problem is
    // circleView's constructor needs onBandSelected, and onBandSelected
    // needs the knob pointers below, which don't exist yet at this point.
    // Same forward-reference-slot trick already used for
    // panelCloseButtonSlot/fxVisible/activeTabIdx elsewhere in this file:
    // a shared_ptr<std::function<void(int)>> that circleView's ctor
    // captures a tiny forwarding lambda around, filled in with the real
    // onBandSelected once it exists further down.
    auto onBandSelectedSlot = std::make_shared<std::function<void(int)>>(nullptr);
    auto* circleView = new CircleViewControl(graphArea, GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0,
      [onBandSelectedSlot](int bandIndex) { if (*onBandSelectedSlot) (*onBandSelectedSlot)(bandIndex); });
    pGraphics->AttachControl(circleView);
    mCircleView = circleView; // ROUND 27 -- lets SerializeState/UnserializeState reach GetUserPresets/SetUserPresets below
    // ROUND 27 -- a project state restored BEFORE this editor was ever
    // opened leaves its user presets waiting in mPendingUserPresets (see
    // its own note in ExoEQ.h); apply them now that circleView actually
    // exists, then clear the flag so a later close/reopen of this same
    // editor instance doesn't re-apply a now-stale snapshot over presets
    // saved/deleted since.
    if (mHasPendingUserPresets)
    {
      std::vector<CircleViewControl::PresetCircleDef> presets;
      for (const auto& pend : mPendingUserPresets)
      {
        CircleViewControl::PresetCircleDef def{};
        std::snprintf(def.name, sizeof(def.name), "%s", pend.name);
        def.bandCount = pend.bandCount;
        for (int b = 0; b < 3; ++b)
          def.bands[b] = { pend.bands[b].ordinal, pend.bands[b].cutoffHz, pend.bands[b].resonance, pend.bands[b].forceMorph50 };
        presets.push_back(def);
      }
      circleView->SetUserPresets(presets);
      mHasPendingUserPresets = false;
      mPendingUserPresets.clear();
    }

    // Detail panel controls are created ONCE, bound initially to band 0,
    // and retargeted (SetParamIdx) whenever BandGraphControl reports a
    // different selection -- this is the "one fixed panel" choice: no
    // per-band control instances, no recreate-on-select.
    // Filter-type dropdown goes FIRST in the row -- "what kind of filter"
    // reads as the primary decision, with cutoff/reso/morph as that
    // type's own parameters underneath it, same information hierarchy
    // FabFilter's own type-then-params layout uses.
    //
    // NOTE on IVMenuButtonControl: this is iPlug2's standard control for
    // a text-dropdown bound to an enum param (click it, get a popup list
    // of InitEnum's option strings) -- I can't verify its exact
    // constructor signature against the real vendored SDK here, same
    // caveat as SetParamIdx below. If this specific line throws a build
    // error, paste it.
    // Widened from 7 to 8 cells this round to fit the new "Extra" knob
    // (AW Y-Lowpass's powFactor, DSP/FilterCore.h's FilterParams::extra) --
    // a no-op dial for every other filter type, but needed its own real,
    // automatable slot per the user's explicit request rather than being
    // folded into Reso/Morph like every earlier Airwindows port.
    auto* filterTypeMenu = pGraphics->AttachControl(new IVMenuButtonControl(panelRow1.GetGridCell(0, 0, 1, 9).GetCentredInside(90, 40), BandParamIdx(0, kBandFilterType), "Type"));
    auto* cutoffKnob = pGraphics->AttachControl(new IVKnobControl(panelRow1.GetGridCell(0, 1, 1, 9).GetCentredInside(70), BandParamIdx(0, kBandCutoff), "Cutoff"));
    auto* resoKnob = pGraphics->AttachControl(new IVKnobControl(panelRow1.GetGridCell(0, 2, 1, 9).GetCentredInside(70), BandParamIdx(0, kBandResonance), "Reso"));
    auto* morphKnob = pGraphics->AttachControl(new IVKnobControl(panelRow1.GetGridCell(0, 3, 1, 9).GetCentredInside(70), BandParamIdx(0, kBandMorph), "Morph"));
    auto* extraKnob = pGraphics->AttachControl(new IVKnobControl(panelRow1.GetGridCell(0, 4, 1, 9).GetCentredInside(70), BandParamIdx(0, kBandFilterExtra), "Extra"));
    auto* keyTrackKnob = pGraphics->AttachControl(new IVKnobControl(panelRow1.GetGridCell(0, 5, 1, 9).GetCentredInside(70), BandParamIdx(0, kBandKeyTrackAmt), "KeyTrk"));
    auto* muteToggle = pGraphics->AttachControl(new IVToggleControl(panelRow1.GetGridCell(0, 6, 1, 9).GetCentredInside(70, 40), BandParamIdx(0, kBandMute), "Mute"));
    auto* soloToggle = pGraphics->AttachControl(new IVToggleControl(panelRow1.GetGridCell(0, 7, 1, 9).GetCentredInside(70, 40), BandParamIdx(0, kBandSolo), "Delta"));

    // Row 2: 3 send enable+param clusters + sidechain, 13 controls across
    // 13 grid cells -- one cell per control keeps this simple; a tighter
    // per-send grouping (sub-grids) can replace this once it's confirmed
    // working, purely cosmetic.
    auto* delayOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 0, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandDelayEnabled), "Dly On"));
    auto* delayTimeKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 1, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandDelayTimeMs), "Dly Time"));
    auto* delayFbKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 2, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandDelayFeedback), "Dly FB"));
    auto* delayMixKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 3, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandDelayMix), "Dly Mix"));

    auto* distOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 4, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandDistortionEnabled), "Dist On"));
    auto* distDriveKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 5, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandDistortionDrive), "Drive"));
    auto* distCharKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 6, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandDistortionCharacter), "Char"));
    auto* distMixKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 7, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandDistortionMix), "Dist Mix"));

    auto* shiftOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 8, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandFreqShiftEnabled), "Shift On"));
    auto* shiftHzKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 9, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandFreqShiftHz), "Shift Hz"));
    auto* shiftMixKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 10, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandFreqShiftMix), "Shift Mix"));

    auto* scOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 11, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandSidechainEnabled), "SC On"));
    auto* scDepthKnob = pGraphics->AttachControl(new IVKnobControl(panelRow2.GetGridCell(0, 12, 1, 13).GetCentredInside(60), BandParamIdx(0, kBandSidechainModOctaves), "SC Depth"));

    // Row 3: the three "order of effect" slot knobs (freq-shift/distortion/
    // delay), each 0-2 (int param), PLUS Energy2's on-toggle + 2 grouped
    // knobs + mix (widened from 3 to 7 cells this round) -- Energy2 isn't
    // reorderable (see BandChain.h's Band::ProcessSample for why it's
    // fixed right after the filter, not a 4th slot), so it rides on this
    // row for layout convenience, not because it's conceptually an
    // "order" control.
    auto* shiftSlotKnob = pGraphics->AttachControl(new IVKnobControl(panelRow3.GetGridCell(0, 0, 1, 7).GetCentredInside(60), BandParamIdx(0, kBandFreqShiftSlot), "Shift Ord"));
    auto* distSlotKnob = pGraphics->AttachControl(new IVKnobControl(panelRow3.GetGridCell(0, 1, 1, 7).GetCentredInside(60), BandParamIdx(0, kBandDistortionSlot), "Dist Ord"));
    auto* delaySlotKnob = pGraphics->AttachControl(new IVKnobControl(panelRow3.GetGridCell(0, 2, 1, 7).GetCentredInside(60), BandParamIdx(0, kBandDelaySlot), "Dly Ord"));

    // Energy2: On toggle + 2 grouped knobs (GlitterHiss, RatScrapeFizz) +
    // bipolar Mix. These are real, automatable per-band params like
    // everything else here -- the global 20-instance cap (see
    // DSP/BandChain.h's kMaxEnergy2Instances) is enforced in the DSP
    // layer, not in the UI; a band past the cap still shows and accepts
    // these controls, it just silently has no audible effect until a
    // slot frees up (same "inert past the limit" behavior kBandActive
    // already uses for inactive bands).
    auto* energy2OnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow3.GetGridCell(0, 3, 1, 7).GetCentredInside(60, 36), BandParamIdx(0, kBandEnergy2Enabled), "Energy2"));
    auto* energy2GlitterHissKnob = pGraphics->AttachControl(new IVKnobControl(panelRow3.GetGridCell(0, 4, 1, 7).GetCentredInside(60), BandParamIdx(0, kBandEnergy2GlitterHiss), "Glitr/Hiss"));
    auto* energy2RatScrapeFizzKnob = pGraphics->AttachControl(new IVKnobControl(panelRow3.GetGridCell(0, 5, 1, 7).GetCentredInside(60), BandParamIdx(0, kBandEnergy2RatScrapeFizz), "Rat/Scr/Fizz"));
    auto* energy2MixKnob = pGraphics->AttachControl(new IVKnobControl(panelRow3.GetGridCell(0, 6, 1, 7).GetCentredInside(60), BandParamIdx(0, kBandEnergy2Mix), "Energy2 Mix"));

    // NOTE on SetParamIdx: this is the one call in this whole file I'm
    // least certain matches the vendored SDK version's exact signature --
    // it's a real, commonly-used iPlug2 IControl method for exactly this
    // "rebind an existing control to a different param" pattern, but I
    // can't check it against the real headers here. If the build errors
    // on this specific call, paste it and I'll fix the exact signature.
    // NEW THIS ROUND -- every detail-panel control in one list, purely so
    // onBandSelected (right below) can Hide/show all of them in one pass
    // instead of needing a second hand-maintained capture list. Built
    // from the exact same pointers already captured into onBandSelected
    // itself, so there's no risk of the two lists drifting apart.
    const std::vector<IControl*> panelControls = {
      filterTypeMenu, cutoffKnob, resoKnob, morphKnob, extraKnob, keyTrackKnob, muteToggle, soloToggle,
      delayOnToggle, delayTimeKnob, delayFbKnob, delayMixKnob,
      distOnToggle, distDriveKnob, distCharKnob, distMixKnob,
      shiftOnToggle, shiftHzKnob, shiftMixKnob,
      scOnToggle, scDepthKnob,
      shiftSlotKnob, distSlotKnob, delaySlotKnob,
      energy2OnToggle, energy2GlitterHissKnob, energy2RatScrapeFizzKnob, energy2MixKnob
    };
    // Starts hidden -- nothing is selected yet on a fresh load, so there's
    // nothing for the detail panel to show (the FabFilter Pro-Q-style
    // "parameters appear/disappear based on the action" behavior you
    // asked for).
    for (auto* c : panelControls) c->Hide(true);

    // NEW THIS ROUND -- a forward-reference slot for the panel's own
    // close button, which doesn't exist yet at this point (it needs
    // `circleView`, created further below) but onBandSelected (right
    // here) is the one place that already knows when to show/hide it.
    // A shared_ptr, not a plain IControl* local, for the same reason
    // fxVisible/activeTabIdx above are shared_ptrs -- onBandSelected
    // itself is stored as a long-lived std::function and outlives this
    // OnLayout call, so a raw reference to a stack local would dangle.
    auto panelCloseButtonSlot = std::make_shared<IControl*>(nullptr);

    auto onBandSelected = [panelControls, panelCloseButtonSlot, filterTypeMenu, cutoffKnob, resoKnob, morphKnob, extraKnob, keyTrackKnob, muteToggle, soloToggle,
                            delayOnToggle, delayTimeKnob, delayFbKnob, delayMixKnob,
                            distOnToggle, distDriveKnob, distCharKnob, distMixKnob,
                            shiftOnToggle, shiftHzKnob, shiftMixKnob,
                            scOnToggle, scDepthKnob,
                            shiftSlotKnob, distSlotKnob, delaySlotKnob,
                            energy2OnToggle, energy2GlitterHissKnob, energy2RatScrapeFizzKnob, energy2MixKnob](int bandIndex) {
      // The panel (and its own close button, once it exists) shows
      // once a real band is selected (bandIndex >= 0) and hides when
      // ClearSelection() below passes -1.
      for (auto* c : panelControls) c->Hide(bandIndex < 0);
      if (*panelCloseButtonSlot) (*panelCloseButtonSlot)->Hide(bandIndex < 0);
      // NEW THIS ROUND -- GUARD ADDED: bandIndex can now legitimately be
      // -1 (CircleViewControl's new rail-empty-space deselect, and the
      // panel's own "X" close button both call this with -1 to mean
      // "nothing selected"). BandParamIdx(-1, offset) would underflow
      // into a garbage/out-of-range param index -- every SetParamIdx
      // call below MUST be skipped in that case, not just the Hide()
      // calls above. This guard is the actual fix that makes -1 safe to
      // pass at all; without it, deselecting would silently retarget
      // every one of these controls at an invalid param index.
      if (bandIndex < 0) return;
      filterTypeMenu->SetParamIdx(BandParamIdx(bandIndex, kBandFilterType));
      cutoffKnob->SetParamIdx(BandParamIdx(bandIndex, kBandCutoff));
      resoKnob->SetParamIdx(BandParamIdx(bandIndex, kBandResonance));
      morphKnob->SetParamIdx(BandParamIdx(bandIndex, kBandMorph));
      extraKnob->SetParamIdx(BandParamIdx(bandIndex, kBandFilterExtra));
      keyTrackKnob->SetParamIdx(BandParamIdx(bandIndex, kBandKeyTrackAmt));
      muteToggle->SetParamIdx(BandParamIdx(bandIndex, kBandMute));
      soloToggle->SetParamIdx(BandParamIdx(bandIndex, kBandSolo));

      delayOnToggle->SetParamIdx(BandParamIdx(bandIndex, kBandDelayEnabled));
      delayTimeKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDelayTimeMs));
      delayFbKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDelayFeedback));
      delayMixKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDelayMix));

      distOnToggle->SetParamIdx(BandParamIdx(bandIndex, kBandDistortionEnabled));
      distDriveKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDistortionDrive));
      distCharKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDistortionCharacter));
      distMixKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDistortionMix));

      shiftOnToggle->SetParamIdx(BandParamIdx(bandIndex, kBandFreqShiftEnabled));
      shiftHzKnob->SetParamIdx(BandParamIdx(bandIndex, kBandFreqShiftHz));
      shiftMixKnob->SetParamIdx(BandParamIdx(bandIndex, kBandFreqShiftMix));

      scOnToggle->SetParamIdx(BandParamIdx(bandIndex, kBandSidechainEnabled));
      scDepthKnob->SetParamIdx(BandParamIdx(bandIndex, kBandSidechainModOctaves));

      shiftSlotKnob->SetParamIdx(BandParamIdx(bandIndex, kBandFreqShiftSlot));
      distSlotKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDistortionSlot));
      delaySlotKnob->SetParamIdx(BandParamIdx(bandIndex, kBandDelaySlot));

      energy2OnToggle->SetParamIdx(BandParamIdx(bandIndex, kBandEnergy2Enabled));
      energy2GlitterHissKnob->SetParamIdx(BandParamIdx(bandIndex, kBandEnergy2GlitterHiss));
      energy2RatScrapeFizzKnob->SetParamIdx(BandParamIdx(bandIndex, kBandEnergy2RatScrapeFizz));
      energy2MixKnob->SetParamIdx(BandParamIdx(bandIndex, kBandEnergy2Mix));
    };
    // ROUND 26: circleView already exists and is attached (moved above,
    // before the panel knobs, to fix the z-order bug) -- just wire the
    // real callback into the forward-reference slot it was given.
    *onBandSelectedSlot = onBandSelected;

    // NEW THIS ROUND -- the "explicit way to deselect/hide the detail
    // panel again" follow-up flagged last round (clicking empty graph
    // space can't double as deselect: that gesture is already taken by
    // "create a new band here"). A small close button pinned to the
    // panel's own top-right corner, visible only while the panel itself
    // is -- same Hide() toggling as every other contextual control here,
    // just driven by this button instead of a band selection. Clears
    // CircleViewControl's own selection AND hides every panelControls
    // entry in one action function, since neither side can fully do
    // this alone (the control has no reach into the panel's Hide()
    // calls; the panel's own button has no reach into the control's
    // private mSelectedBand without the new ClearSelection() getter).
    // NOTE on GetFromTRHC: this file already has GetFromBRHC proven to
    // compile (the build-info text at the bottom of OnLayout) -- IRECT's
    // usual quartet of corner-helpers (TLHC/TRHC/BLHC/BRHC) almost
    // certainly includes this one too, but it's not itself proven yet;
    // paste the error if this specific name doesn't compile.
    auto* panelCloseButton = pGraphics->AttachControl(new IVButtonControl(panelFull.GetFromTRHC(28.f, 28.f), SplashClickActionFunc, "X"));
    *panelCloseButtonSlot = panelCloseButton; // fills in the forward-reference slot onBandSelected captured above
    panelCloseButton->Hide(true); // starts hidden along with the rest of panelControls -- it didn't exist yet when that initial hide loop ran
    panelCloseButton->SetActionFunction([circleView, panelControls, panelCloseButton](IControl* pCaller) {
      circleView->ClearSelection();
      for (auto* c : panelControls) c->Hide(true);
      panelCloseButton->Hide(true);
    });

    // NEW THIS ROUND: Solo/Mute buttons for whichever circle is currently
    // selected on the graph -- confirmed as part of the original design
    // scope ("selected-circle Solo/Mute buttons", previously flagged as
    // deferred). Lives in the new header strip above the graph. Mute
    // toggles every band in the selected circle together (reads the
    // FIRST active band's own Mute state to decide which way to flip --
    // "any unmuted -> mute all", "all already muted -> unmute all", a
    // plain toggle rather than two separate buttons). Solo is one-way:
    // unmutes every band in the selected circle, mutes every band in
    // every OTHER circle -- standard mixing-console solo behavior, not a
    // toggle (clicking Solo on a different circle afterward re-solos that
    // one instead; there's no separate "un-solo" state to track).
    // NEW THIS ROUND -- master show/hide for the whole global-FX tab
    // strip (Groups/Mod Matrix/LFOs/Fractal/Transform/Image + MTS-ESP),
    // which now starts hidden along with the detail panel (same Pro-Q-
    // style "nothing shows until you ask for it" request) -- the tab
    // strip has no OTHER always-visible trigger once it's hidden
    // (clicking a tab BUTTON would normally reveal that tab's content,
    // but the buttons themselves live inside tabsFull and would be
    // hidden too), so this lives in the header, outside tabsFull,
    // specifically so it's never itself hidden by the thing it controls.
    // fxVisible is a shared_ptr<bool> (not a raw/member bool) purely so
    // this lambda and the tab-strip-building code far below can both
    // capture-by-value and still share the one flag, without adding a
    // new ExoEQ member for a single UI-only toggle.
    auto fxVisible = std::make_shared<bool>(false);
    auto* globalFxButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 0, 1, 7).GetCentredInside(90, 24), SplashClickActionFunc, "Global FX"));

    auto* muteCircleButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 4, 1, 7).GetCentredInside(90, 24), SplashClickActionFunc, "Mute Circle"));
    muteCircleButton->SetActionFunction([this, circleView](IControl* pCaller) {
      const int c = circleView->GetSelectedCircle();
      bool anyUnmuted = false;
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        if (static_cast<int>(GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != c) continue;
        if (GetParam(BandParamIdx(b, kBandMute))->Value() <= 0.5) { anyUnmuted = true; break; }
      }
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        if (static_cast<int>(GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != c) continue;
        SetParameterValue(BandParamIdx(b, kBandMute), anyUnmuted ? 1.0 : 0.0);
      }
    });

    auto* soloCircleButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 5, 1, 7).GetCentredInside(90, 24), SplashClickActionFunc, "Solo Circle"));
    soloCircleButton->SetActionFunction([this, circleView](IControl* pCaller) {
      const int c = circleView->GetSelectedCircle();
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        const bool inSelectedCircle = static_cast<int>(GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) == c;
        SetParameterValue(BandParamIdx(b, kBandMute), inSelectedCircle ? 0.0 : 1.0);
      }
    });

    // NEW THIS ROUND: Mel vs. log frequency-axis toggle for the graph --
    // "switchable frequency calibration beyond Mel/perceptual" off the
    // original deferred list. A plain bound IVToggleControl on the new
    // kGlobalFreqAxisMel param -- CircleViewControl.h reads this param
    // directly (GetDelegate()->GetParam(kGlobalFreqAxisMel)) every time
    // it converts between screen X and Hz, so no SetParamIdx/retargeting
    // wiring is needed here the way the per-band detail panel needs.
    pGraphics->AttachControl(new IVToggleControl(headerRow.GetGridCell(0, 6, 1, 7).GetCentredInside(90, 24), kGlobalFreqAxisMel, "Mel Axis"));

    // Global (not per-band, not retargeted) -- real MTS-ESP on/off. This
    // is the one global control that stays visible regardless of which
    // tab is open (it's a single always-relevant toggle, not worth its
    // own tab) -- placed in the tab button row's far right rather than
    // the old top-right-of-window spot, since that spot is now inside
    // graphArea.
    //
    // Everything below this point used to float in a fixed top-right
    // stack (Groups/Mod Matrix/LFOs/Fractal/Transform/Image) that
    // predates the circle graph entirely -- once the graph started
    // filling the top half of the window, that stack visually collided
    // with it (confirmed directly from the user's own screenshot: the
    // Group#/Grp Mute/LFO/Formula boxes sitting on top of the black
    // graph area). Moved into a tab strip below the per-band detail
    // panel instead (user's explicit choice between 3 options: "Tabs
        // below detail panel"). Every control below gets pushed into
    // mTabControls[kTabX] as it's created so the tab buttons can
    // Hide()/show a whole tab's controls at once -- see ExoEQ.h's own
    // note on why Hide() per-control rather than a grouped-control API.
    const IRECT tabButtonRow = IRECT(tabsFull.L, tabsFull.T, tabsFull.R, tabsFull.T + 36.f);
    const IRECT tabContentArea = IRECT(tabsFull.L, tabsFull.T + 36.f, tabsFull.R, tabsFull.B);

    auto* mtsEspToggle = pGraphics->AttachControl(new IVToggleControl(tabButtonRow.GetGridCell(0, 6, 1, 7).GetCentredInside(90, 32), kGlobalMtsEspEnabled, "MTS-ESP"));

    // Groups panel: ONE fixed set of controls (Select knob + Mute/Gain),
    // retargeted to whichever group the Select knob's own value picks --
    // same "one panel, retargeted" pattern as the 128-band detail panel
    // above, just driven by a plain knob instead of a graph click since
    // there's no spatial layout for 8 groups to click on.
    const IRECT groupsRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* groupSelectKnob = pGraphics->AttachControl(new IVKnobControl(groupsRow.GetGridCell(0, 0, 1, 3).GetCentredInside(60), kGlobalGroupSelect, "Group #"));
    auto* groupMuteToggle = pGraphics->AttachControl(new IVToggleControl(groupsRow.GetGridCell(0, 1, 1, 3).GetCentredInside(60, 36), GroupParamIdx(0, kGroupMute), "Grp Mute"));
    auto* groupGainKnob = pGraphics->AttachControl(new IVKnobControl(groupsRow.GetGridCell(0, 2, 1, 3).GetCentredInside(60), GroupParamIdx(0, kGroupGainDb), "Grp Gain"));
    mTabControls[kTabGroups] = { groupSelectKnob, groupMuteToggle, groupGainKnob };

    // Mod matrix panel: same retargeting idea, one row per connection
    // slot, selected by a Select knob rather than kMaxModConnections rows
    // of controls (8 connections * 5 params would be 40 live controls at
    // once -- a single retargeted row keeps this readable, matching how
    // the band panel already avoids showing 128 bands' worth of knobs).
    const IRECT modRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* modSelectKnob = pGraphics->AttachControl(new IVKnobControl(modRow.GetGridCell(0, 0, 1, 6).GetCentredInside(60), kGlobalModConnSelect, "Conn #"));
    auto* modOnToggle = pGraphics->AttachControl(new IVToggleControl(modRow.GetGridCell(0, 1, 1, 6).GetCentredInside(60, 36), ModConnParamIdx(0, kModConnEnabled), "Conn On"));
    auto* modSrcKnob = pGraphics->AttachControl(new IVKnobControl(modRow.GetGridCell(0, 2, 1, 6).GetCentredInside(60), ModConnParamIdx(0, kModConnSourceIndex), "Src"));
    auto* modDestMenu = pGraphics->AttachControl(new IVMenuButtonControl(modRow.GetGridCell(0, 3, 1, 6).GetCentredInside(90, 36), ModConnParamIdx(0, kModConnDestination), "Dest"));
    auto* modDepthKnob = pGraphics->AttachControl(new IVKnobControl(modRow.GetGridCell(0, 4, 1, 6).GetCentredInside(60), ModConnParamIdx(0, kModConnDepth), "Depth"));
    auto* modTargetKnob = pGraphics->AttachControl(new IVKnobControl(modRow.GetGridCell(0, 5, 1, 6).GetCentredInside(60), ModConnParamIdx(0, kModConnTargetBand), "Target"));
    mTabControls[kTabModMatrix] = { modSelectKnob, modOnToggle, modSrcKnob, modDestMenu, modDepthKnob, modTargetKnob };

    // LFO row: the 4 sources the mod matrix's Src knob (0-3) actually
    // reads -- same fixed-panel-of-4 approach, no retargeting needed since
    // there are only 4 and they all fit on one row.
    const IRECT lfoRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    for (int i = 0; i < kMaxModSources; ++i)
    {
      const float cellW = lfoRow.W() / kMaxModSources;
      const IRECT cell = lfoRow.GetFromLeft(cellW * (i + 1)).GetFromRight(cellW);
      auto* lfoRateKnob = pGraphics->AttachControl(new IVKnobControl(cell.GetGridCell(0, 0, 1, 2).GetCentredInside(50), LfoParamIdx(i, kLfoRateHz), ("LFO" + std::to_string(i + 1) + " Hz").c_str()));
      auto* lfoWaveMenu = pGraphics->AttachControl(new IVMenuButtonControl(cell.GetGridCell(0, 1, 1, 2).GetCentredInside(70, 32), LfoParamIdx(i, kLfoWaveform), ("LFO" + std::to_string(i + 1) + " Wave").c_str()));
      mTabControls[kTabLfos].push_back(lfoRateKnob);
      mTabControls[kTabLfos].push_back(lfoWaveMenu);
    }

    // Fractal generator: top row of 9 knobs + Formula/Generate/Clear
    // buttons, plus the EXOMUSICA-letter var row beneath it -- the
    // tallest tab's content, so tabContentArea's own 2-row height (see
    // fractalRow/varRow below) is what the strip's total height was
    // sized against. Every knob here is a real automatable global param
    // (see the InitDouble/InitInt/InitEnum calls above) -- only the
    // formula text field and the Generate button are plain UI actions
    // with no param behind them (a formula isn't a numeric value iPlug2
    // params can hold; Generate is a one-shot action, not a state).
    const IRECT fractalRow = IRECT(tabContentArea.L, tabContentArea.T, tabContentArea.R, tabContentArea.T + tabContentArea.H() * 0.5f);
    auto* fSeedKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 0, 1, 10).GetCentredInside(55), kGlobalFractalSeedHz, "Seed Hz"));
    auto* fRatioKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 1, 1, 10).GetCentredInside(55), kGlobalFractalRatio, "Ratio"));
    auto* fBranchKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 2, 1, 10).GetCentredInside(55), kGlobalFractalBranchFactor, "Branch"));
    auto* fDepthKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 3, 1, 10).GetCentredInside(55), kGlobalFractalDepth, "Depth"));
    auto* fGainKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 4, 1, 10).GetCentredInside(55), kGlobalFractalGainDb, "Seed Gain"));
    auto* fDecayKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 5, 1, 10).GetCentredInside(55), kGlobalFractalGainDecay, "Decay"));
    auto* fJitterKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 6, 1, 10).GetCentredInside(55), kGlobalFractalJitterPct, "Jitter"));
    auto* fSymmToggle = pGraphics->AttachControl(new IVToggleControl(fractalRow.GetGridCell(0, 7, 1, 10).GetCentredInside(55, 32), kGlobalFractalSymmetric, "Symm"));
    auto* fFilterMenu = pGraphics->AttachControl(new IVMenuButtonControl(fractalRow.GetGridCell(0, 8, 1, 10).GetCentredInside(90, 32), kGlobalFractalFilterType, "Filter"));
    mTabControls[kTabFractal] = { fSeedKnob, fRatioKnob, fBranchKnob, fDepthKnob, fGainKnob, fDecayKnob, fJitterKnob, fSymmToggle, fFilterMenu };

    // Formula button: pops a text-entry box over itself (IGraphics'
    // native CreateTextEntry -- same can't-verify-against-the-real-SDK
    // caveat as every other exact-signature note in this project) pre-
    // filled with the current mFractalFormula, and commits whatever's
    // typed back into it. Leaving the box empty restores the knob-driven
    // default path (see FractalGenerator.h).
    auto* formulaButton = pGraphics->AttachControl(new IVButtonControl(fractalRow.GetGridCell(0, 9, 1, 10).GetCentredInside(90, 32), SplashClickActionFunc, "Formula..."));
    // See the TextEntryCallbackControl note above this constructor --
    // CreateTextEntry's completion isn't a lambda argument in this
    // iPlug2 version, it's a virtual call targeting whichever control is
    // passed as its first argument.
    auto* formulaEntryProxy = pGraphics->AttachControl(new TextEntryCallbackControl([this](const char* str) { mFractalFormula = str ? str : ""; }));
    formulaButton->SetActionFunction([this, pGraphics, formulaButton, formulaEntryProxy](IControl* pCaller) {
      pGraphics->CreateTextEntry(*formulaEntryProxy, DEFAULT_TEXT, formulaButton->GetRECT(), mFractalFormula.c_str(), 256);
    });
    mTabControls[kTabFractal].push_back(formulaButton);
    // formulaEntryProxy stays permanently hidden (mHide=true from its own
    // constructor) whichever tab is active -- it's only ever a target for
    // CreateTextEntry, never meant to be seen -- so it's deliberately NOT
    // added to any mTabControls entry; the per-tab Hide() calls below
    // would be redundant on a control that's already always hidden.

    auto* generateButton = pGraphics->AttachControl(new IVButtonControl(fractalRow.GetGridCell(0, 9, 1, 10).GetTranslated(0.f, 40.f), SplashClickActionFunc, "Generate"));
    generateButton->SetActionFunction([this](IControl* pCaller) { GenerateFractalBands(); });
    mTabControls[kTabFractal].push_back(generateButton);
    // "Clear Fractal" -- deactivates exactly the bands this generator
    // made (kBandSource==Fractal), leaving hand-built bands and the
    // other two generators' output untouched. See ExoEQ.h's own note on
    // ClearBandsBySource for why this exists now: the stacking risk
    // flagged since the transform-filter round is real, and this is
    // the direct mitigation for it -- "undo what Generate just did"
    // without hunting for which bands those were.
    auto* clearFractalButton = pGraphics->AttachControl(new IVButtonControl(fractalRow.GetGridCell(0, 9, 1, 10).GetTranslated(0.f, 40.f).GetVShifted(38.f), SplashClickActionFunc, "Clear Fractal"));
    clearFractalButton->SetActionFunction([this](IControl* pCaller) { ClearBandsBySource(EBandSource::Fractal); });
    mTabControls[kTabFractal].push_back(clearFractalButton);

    // 9 custom-variable knobs, one per EXOMUSICA letter (the user's
    // easter egg), one cell each: a knob (the real automatable 0..1
    // param) plus a small "Range" button beneath it that pops a text
    // entry for "min,max" -- parsed and stored into mFractalVarMin/
    // Max[i] on commit (invalid text is ignored, keeping whatever range
    // was already set, rather than silently zeroing it). Labeled by the
    // actual letter the formula box uses, not a generic "Var 1/2/3" --
    // the whole point is that "E" in a knob label and "E" typed into
    // the formula are visibly the same thing, and the 9 labels read
    // E-X-O-M-U-S-I-C-A left to right across the row.
    const IRECT varRow = IRECT(tabContentArea.L, tabContentArea.T + tabContentArea.H() * 0.5f, tabContentArea.R, tabContentArea.B);
    static const char* kVarLetters[kNumFractalCustomVars] = { "E", "X", "O", "M", "U", "S", "I", "C", "A" };
    for (int i = 0; i < kNumFractalCustomVars; ++i)
    {
      const IRECT cell = varRow.GetGridCell(0, i, 1, kNumFractalCustomVars);
      auto* varKnob = pGraphics->AttachControl(new IVKnobControl(cell.GetGridCell(0, 0, 2, 1).GetCentredInside(50), kGlobalFractalVarStart + i, kVarLetters[i]));
      auto* rangeButton = pGraphics->AttachControl(new IVButtonControl(cell.GetGridCell(1, 0, 2, 1).GetCentredInside(70, 28), SplashClickActionFunc, "Range"));
      mTabControls[kTabFractal].push_back(varKnob);
      mTabControls[kTabFractal].push_back(rangeButton);
      const int varIndex = i; // local copy for the lambda, not the loop variable
      // Same proxy-control fix as the Formula button above -- one proxy
      // per range button, each closing over its own varIndex. Same
      // always-hidden reasoning as formulaEntryProxy above -- not added
      // to mTabControls.
      auto* rangeEntryProxy = pGraphics->AttachControl(new TextEntryCallbackControl([this, varIndex](const char* str) {
        if (!str) return;
        double newMin, newMax;
        // "min,max" -- a plain comma-separated pair, deliberately not a
        // formula (this box sets a RANGE, not a value) so there's
        // nothing to run through ExprEval here. Malformed input (not
        // exactly 2 comma-separated numbers, or min>=max) is ignored --
        // the previous range stays in effect rather than silently
        // collapsing to something unusable.
        if (std::sscanf(str, "%lf,%lf", &newMin, &newMax) == 2 && std::isfinite(newMin) && std::isfinite(newMax) && newMin < newMax)
        {
          mFractalVarMin[varIndex] = newMin;
          mFractalVarMax[varIndex] = newMax;
        }
      }));
      rangeButton->SetActionFunction([this, pGraphics, rangeButton, rangeEntryProxy, varIndex](IControl* pCaller) {
        char current[64];
        std::snprintf(current, sizeof(current), "%g,%g", mFractalVarMin[varIndex], mFractalVarMax[varIndex]);
        pGraphics->CreateTextEntry(*rangeEntryProxy, DEFAULT_TEXT, rangeButton->GetRECT(), current, 64);
      });
    }

    // Transform filter: pivot/exponent + Transform/Clear buttons. Reads
    // the CURRENT active band series rather than a formula -- see
    // GenerateTransformBands's own note on the stacking behavior that
    // follows from reusing the same non-destructive "write into new
    // inactive slots" policy the fractal generator uses. Shares the
    // fractal generator's own cheap-filter-type param
    // (kGlobalFractalFilterType) rather than a duplicate dropdown.
    const IRECT transformRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* tPivotKnob = pGraphics->AttachControl(new IVKnobControl(transformRow.GetGridCell(0, 0, 1, 3).GetCentredInside(60), kGlobalTransformPivotHz, "Pivot Hz"));
    auto* tExponentKnob = pGraphics->AttachControl(new IVKnobControl(transformRow.GetGridCell(0, 1, 1, 3).GetCentredInside(60), kGlobalTransformExponent, "Exponent"));
    auto* transformButton = pGraphics->AttachControl(new IVButtonControl(transformRow.GetGridCell(0, 2, 1, 3).GetCentredInside(90, 36), SplashClickActionFunc, "Transform"));
    transformButton->SetActionFunction([this](IControl* pCaller) { GenerateTransformBands(); });
    // "Clear Transform" -- same policy as Clear Fractal above, scoped
    // to kBandSource==Transform only.
    auto* clearTransformButton = pGraphics->AttachControl(new IVButtonControl(transformRow.GetGridCell(0, 2, 1, 3).GetCentredInside(90, 28).GetVShifted(42.f), SplashClickActionFunc, "Clear Transform"));
    clearTransformButton->SetActionFunction([this](IControl* pCaller) { ClearBandsBySource(EBandSource::Transform); });
    mTabControls[kTabTransform] = { tPivotKnob, tExponentKnob, transformButton, clearTransformButton };

    // Image-to-EQ-curve: 5 knobs + an invert toggle, a file-picker
    // button, a status text showing either the current file's name or
    // the last error, and the Generate/Clear buttons.
    //
    // THE FILE-PICKER BUTTON is the one piece of this round I can't
    // verify against your actual vendored IGraphics headers (same
    // can't-verify-against-the-real-SDK caveat used throughout this
    // project for UI/SDK-signature specifics) -- it calls
    // pGraphics->PromptForFile(...), which is IGraphics' own native
    // "open file" dialog call across all 3 platforms/formats (VST3/AAX/
    // CLAP on Win/Mac). If your iPlug2 version's exact signature differs
    // (argument order, an EFileAction enum name, etc.), paste the
    // compiler error and I'll match it exactly.
    const IRECT imageRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* iNodesKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 0, 1, 8).GetCentredInside(50), kGlobalImageNodeCount, "Nodes"));
    auto* iMinHzKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 1, 1, 8).GetCentredInside(50), kGlobalImageMinFreqHz, "Min Hz"));
    auto* iMaxHzKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 2, 1, 8).GetCentredInside(50), kGlobalImageMaxFreqHz, "Max Hz"));
    auto* iGainRangeKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 3, 1, 8).GetCentredInside(50), kGlobalImageGainRangeDb, "Gain Range"));
    auto* iResonanceKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 4, 1, 8).GetCentredInside(50), kGlobalImageResonance, "Resonance"));
    auto* iInvertToggle = pGraphics->AttachControl(new IVToggleControl(imageRow.GetGridCell(0, 5, 1, 8).GetCentredInside(60, 30), kGlobalImageInvertLuminance, "Invert"));
    mTabControls[kTabImage] = { iNodesKnob, iMinHzKnob, iMaxHzKnob, iGainRangeKnob, iResonanceKnob, iInvertToggle };

    // Declared as ITextControl* BEFORE attaching, not via AttachControl's
    // own return value -- this iPlug2 version's AttachControl returns a
    // plain IControl*, not the templated/derived-type return some other
    // versions have, so `auto*` off of it was silently narrowing this to
    // IControl* (the actual compiler error: "'SetStr': is not a member
    // of IControl" below, since SetStr only exists on ITextControl).
    auto* imageStatusText = new ITextControl(imageRow.GetGridCell(0, 6, 1, 8).GetTranslated(0.f, -18.f), "No image selected", DEFAULT_TEXT.WithAlign(EAlign::Center));
    pGraphics->AttachControl(imageStatusText);
    mTabControls[kTabImage].push_back(imageStatusText);

    auto* pickImageButton = pGraphics->AttachControl(new IVButtonControl(imageRow.GetGridCell(0, 6, 1, 8).GetCentredInside(90, 28).GetTranslated(0.f, 14.f), SplashClickActionFunc, "Pick Image..."));
    pickImageButton->SetActionFunction([this, pGraphics, imageStatusText](IControl* pCaller) {
      WDL_String fileName, path;
      // PromptForFile(fileName, path, EFileAction::Open, ""): ""
      // leaves the format filter open to whatever stb_image itself can
      // decode (PNG/JPG/BMP/GIF/etc.) rather than hardcoding one
      // extension -- stbi_load's own format sniffing handles the rest.
      pGraphics->PromptForFile(fileName, path, EFileAction::Open, "");
      if (fileName.GetLength())
      {
        mImageFilePath = fileName.Get();
        mLastImageError.clear();
        // Show just the file name, not the full path -- the path can be
        // long and this is a narrow status line, not a path display.
        const char* base = std::strrchr(mImageFilePath.c_str(), '/');
#ifdef _WIN32
        const char* baseWin = std::strrchr(mImageFilePath.c_str(), '\\');
        if (!base || (baseWin && baseWin > base)) base = baseWin;
#endif
        imageStatusText->SetStr(base ? base + 1 : mImageFilePath.c_str());
      }
    });
    mTabControls[kTabImage].push_back(pickImageButton);

    auto* imageGenerateButton = pGraphics->AttachControl(new IVButtonControl(imageRow.GetGridCell(0, 7, 1, 8).GetCentredInside(90, 36), SplashClickActionFunc, "Generate from Image"));
    imageGenerateButton->SetActionFunction([this, imageStatusText](IControl* pCaller) {
      GenerateImageBands();
      // Surface a failure on the SAME status text the file name shows --
      // a deliberate choice over a separate error control: there's only
      // ever one thing to say about the current image at a time (either
      // "here's the file" or "here's why it didn't work"), so one line
      // covers both without the two ever needing to show at once.
      if (!mLastImageError.empty())
        imageStatusText->SetStr(("Error: " + mLastImageError).c_str());
    });
    mTabControls[kTabImage].push_back(imageGenerateButton);

    // "Clear Image" -- same policy as Clear Fractal/Clear Transform
    // above, scoped to kBandSource==Image only.
    auto* clearImageButton = pGraphics->AttachControl(new IVButtonControl(imageRow.GetGridCell(0, 7, 1, 8).GetCentredInside(90, 28).GetVShifted(42.f), SplashClickActionFunc, "Clear Image"));
    clearImageButton->SetActionFunction([this](IControl* pCaller) { ClearBandsBySource(EBandSource::Image); });
    mTabControls[kTabImage].push_back(clearImageButton);

    // onGroupSelected/onModConnSelected: identical retargeting idea to
    // onBandSelected above, just triggered by the Select knob's own value
    // changing rather than a graph click -- IVKnobControl supports an
    // action-function callback for exactly this ("do something extra when
    // this knob's value changes"), same mechanism BandGraphControl already
    // relies on internally for its own selection callback.
    groupSelectKnob->SetActionFunction([groupMuteToggle, groupGainKnob](IControl* pCaller) {
      const int g = static_cast<int>(pCaller->GetValue() * (exoeq::kNumGroups - 1) + 0.5);
      groupMuteToggle->SetParamIdx(GroupParamIdx(g, kGroupMute));
      groupGainKnob->SetParamIdx(GroupParamIdx(g, kGroupGainDb));
    });
    modSelectKnob->SetActionFunction([modOnToggle, modSrcKnob, modDestMenu, modDepthKnob, modTargetKnob](IControl* pCaller) {
      const int c = static_cast<int>(pCaller->GetValue() * (kMaxModConnections - 1) + 0.5);
      modOnToggle->SetParamIdx(ModConnParamIdx(c, kModConnEnabled));
      modSrcKnob->SetParamIdx(ModConnParamIdx(c, kModConnSourceIndex));
      modDestMenu->SetParamIdx(ModConnParamIdx(c, kModConnDestination));
      modDepthKnob->SetParamIdx(ModConnParamIdx(c, kModConnDepth));
      modTargetKnob->SetParamIdx(ModConnParamIdx(c, kModConnTargetBand));
    });

    // Tab buttons themselves, LAST -- after every tab's controls exist so
    // each button's click handler can capture `this` and call SwitchTab,
    // which just does Hide(t != activeTab) across all 6 mTabControls
    // entries (see ExoEQ.h's member comment). Starts on kTabGroups (index
    // 0) visible, every other tab hidden.
    static const char* kTabLabels[kNumGlobalTabs] = { "Groups", "Mod Matrix", "LFOs", "Fractal", "Transform", "Image" };
    // activeTab's own content still follows the existing per-tab Hide()
    // pattern unchanged; `fxVisible` (above, in the header) is now a
    // SEPARATE, higher-level on/off switch for the strip as a whole --
    // switchTab no longer decides visibility on its own, it just tracks
    // which tab is active so showFx (below) knows which one to reveal.
    // shared_ptr, not a plain stack int captured by reference -- switchTab
    // and globalFxButton's action function both outlive this OnLayout
    // call (they're stored as long-lived std::function closures on their
    // controls), so a `&activeTabIdx` reference to a local would dangle
    // the moment OnLayout returns. Same reasoning as fxVisible above.
    auto activeTabIdx = std::make_shared<int>(kTabGroups);
    auto switchTab = [this, activeTabIdx](int activeTab) {
      *activeTabIdx = activeTab;
      for (int t = 0; t < kNumGlobalTabs; ++t)
        for (auto* c : mTabControls[t])
          c->Hide(t != activeTab);
    };
    std::vector<IControl*> tabButtons;
    for (int t = 0; t < kNumGlobalTabs; ++t)
    {
      auto* tabButton = pGraphics->AttachControl(new IVButtonControl(tabButtonRow.GetGridCell(0, t, 1, 7).GetCentredInside(110, 32), SplashClickActionFunc, kTabLabels[t]));
      tabButton->SetActionFunction([switchTab, t](IControl* pCaller) { switchTab(t); });
      tabButtons.push_back(tabButton);
    }
    switchTab(kTabGroups);

    // NEW THIS ROUND -- the actual show/hide driven by the header's new
    // "Global FX" button: hides/shows the tab BUTTONS, the MTS-ESP
    // toggle, and whichever tab's content is currently active, all
    // together. Starts hidden (fxVisible's own default, set above) --
    // switchTab(kTabGroups) just above already primed which tab WOULD
    // be showing once revealed, it doesn't itself make anything visible
    // here since every tab's content Hide() call runs before this.
    for (auto* b : tabButtons) b->Hide(true);
    mtsEspToggle->Hide(true);
    for (auto* c : mTabControls[*activeTabIdx]) c->Hide(true);
    globalFxButton->SetActionFunction([fxVisible, tabButtons, mtsEspToggle, this, activeTabIdx](IControl* pCaller) {
      *fxVisible = !*fxVisible;
      for (auto* b : tabButtons) b->Hide(!*fxVisible);
      mtsEspToggle->Hide(!*fxVisible);
      for (auto* c : mTabControls[*activeTabIdx]) c->Hide(!*fxVisible);
    });

    WDL_String buildInfoStr;
    GetBuildInfoStr(buildInfoStr, __DATE__, __TIME__);
    pGraphics->AttachControl(new ITextControl(innerBounds.GetFromBRHC(300, 16), buildInfoStr.Get(), DEFAULT_TEXT.WithAlign(EAlign::Far)));
  };
#endif
}

#if IPLUG_DSP
void ExoEQ::OnReset()
{
  const double sr = GetSampleRate();
  for (auto& chain : mCircleChain)
  {
    chain.SetSampleRate(sr);
    chain.Reset();
  }
  mKeyTracker.SetSampleRate(sr);
  mKeyTracker.Reset();

  for (auto& lfo : mModLfos)
    lfo.SetSampleRate(sr);

  // Global FFT stage: sample rate affects which Hz value each FFT bin
  // represents (used inside RunOneFrame's passband test), so it must be
  // pushed on every sample-rate change same as every other sample-rate-
  // aware DSP object above. Reset() clears its OLA/ring-buffer state --
  // same reasoning as mChain[c].Reset() just above: a sample-rate change
  // or hard transport reset should never leave stale audio sitting in
  // an internal buffer to bleed into whatever comes next.
  for (auto& fft : mFftContainer)
  {
    fft.SetSampleRate(sr);
    fft.Reset();
  }

  // Sample rate just changed -- latency in SAMPLES for any future fixed-
  // delay feature would itself change at the same cutoff/window-in-
  // seconds even though nothing about ITS setting moved, so this is
  // exactly one of the two places (alongside wherever such a feature's
  // own setting changes) that must recompute and re-report it. Today
  // this is a no-op (ComputeTotalLatencySamples() returns 0, see
  // ExoEQ.h's comment on it) but costs nothing to call unconditionally.
  RecomputeAndReportLatency();
}

int ExoEQ::ComputeTotalLatencySamples() const
{
  // First real contributor: the global FFT brick-wall stage. Its
  // latency is a fixed sample count (768 at the current 1024/256 FFT
  // config) independent of sample rate -- see FftContainer.h's own
  // note on why an STFT's latency is a frame-count property, not a
  // time property. Exactly one contribution regardless of channel
  // count: both channels' FftBrickwallContainer instances share the
  // identical fixed FFT/hop size, so there's nothing to sum across
  // channels here, unlike a per-band feature would need.
  int total = 0;
  if (GetParam(kGlobalFftEnabled)->Value() > 0.5)
    total += exoeq::FftBrickwallContainer::GetLatencySamples();

  // A future true-oversampling feature adds its own samples-at-current-
  // sample-rate contribution here too (and ONLY here).
  return total;
}

void ExoEQ::RecomputeAndReportLatency()
{
  const int latency = ComputeTotalLatencySamples();
  if (latency != mLastReportedLatencySamples)
  {
    SetLatency(latency);
    mLastReportedLatencySamples = latency;
  }
}

void ExoEQ::ProcessMidiMsg(const IMidiMsg& msg)
{
  // A note-on with velocity 0 is a note-off per the MIDI spec (running-
  // status optimization) -- route it the same as a real kNoteOff. Phase 1
  // never handled note-off at all; with latch=true above that's still the
  // net effect (NoteOff is a no-op whenever nothing else is held), so this
  // only starts to matter once a future UI control switches latch off.
  if (msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() > 0)
    mKeyTracker.NoteOn(msg.NoteNumber());
  else if (msg.StatusMsg() == IMidiMsg::kNoteOff || (msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() == 0))
    mKeyTracker.NoteOff(msg.NoteNumber());

  SendMidiMsg(msg);
}

namespace
{
  exoeq::BandSettings ReadBandSettings(Plugin& plug, int bandIndex, double keyTrackedHz)
  {
    const double manualCutoff = plug.GetParam(BandParamIdx(bandIndex, kBandCutoff))->Value();
    const double resonance = plug.GetParam(BandParamIdx(bandIndex, kBandResonance))->Value();
    const double morph = plug.GetParam(BandParamIdx(bandIndex, kBandMorph))->Value() / 100.0;
    const double filterExtra = plug.GetParam(BandParamIdx(bandIndex, kBandFilterExtra))->Value() / 100.0; // only AW Y-Lowpass uses this (powFactor); a no-op for every other type
    const double keyTrackAmt = plug.GetParam(BandParamIdx(bandIndex, kBandKeyTrackAmt))->Value() / 100.0;
    const double cutoffHz = manualCutoff * (1.0 - keyTrackAmt) + keyTrackedHz * keyTrackAmt;

    const bool active = plug.GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    const bool mute = plug.GetParam(BandParamIdx(bandIndex, kBandMute))->Value() > 0.5;

    // Cast straight from the enum param's integer value -- InitEnum's
    // option order above is required to match FilterTypeId exactly, so
    // this cast is safe as long as that stays true.
    const auto filterType = static_cast<exoeq::FilterTypeId>(static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandFilterType))->Value()));

    exoeq::BandSettings s;
    s.filter = exoeq::FilterParams{ cutoffHz, resonance, morph, filterType, filterExtra };
    // Per-band trim gain -- the base value the mod matrix's GainDb
    // destination and node groups' accumulative gain both multiply
    // against (see ExoEQ.h's note on kBandGainDb for why this was
    // missing until now).
    s.gainLinear = std::pow(10.0, plug.GetParam(BandParamIdx(bandIndex, kBandGainDb))->Value() / 20.0);
    // Inactive bands are forced muted regardless of their own Mute param --
    // this is the entire cost of having 128 "slots" exist: an untouched
    // band is just permanently muted (near-zero CPU; see ExoEQ.h) rather
    // than literally absent.
    s.mute = mute || !active;
    s.solo = plug.GetParam(BandParamIdx(bandIndex, kBandSolo))->Value() > 0.5;

    s.delaySendEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandDelayEnabled))->Value() > 0.5;
    s.delaySend.delayMs = plug.GetParam(BandParamIdx(bandIndex, kBandDelayTimeMs))->Value();
    s.delaySend.feedback = plug.GetParam(BandParamIdx(bandIndex, kBandDelayFeedback))->Value();
    s.delaySend.mix = plug.GetParam(BandParamIdx(bandIndex, kBandDelayMix))->Value();

    s.distortionSendEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionEnabled))->Value() > 0.5;
    s.distortionSend.drive = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionDrive))->Value();
    s.distortionSend.character = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionCharacter))->Value();
    s.distortionSend.mix = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionMix))->Value();

    s.freqShiftSendEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftEnabled))->Value() > 0.5;
    s.freqShiftSend.shiftHz = plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftHz))->Value();
    s.freqShiftSend.mix = plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftMix))->Value();

    // Energy2 -- a muted/inactive band's want is forced off the same way
    // `mute` above already forces everything else off, so a band that's
    // muted never occupies one of the global 20 slots just by having the
    // toggle left on from before (see BandChain::RecomputeEnergy2Slots /
    // Band::WantsEnergy2Slot, which checks !mute for exactly this reason).
    s.energy2Enabled = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2Enabled))->Value() > 0.5;
    s.energy2GlitterHiss = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2GlitterHiss))->Value();
    s.energy2RatScrapeFizz = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2RatScrapeFizz))->Value();
    s.energy2Mix = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2Mix))->Value();

    s.sidechainEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandSidechainEnabled))->Value() > 0.5;
    s.sidechainModOctaves = plug.GetParam(BandParamIdx(bandIndex, kBandSidechainModOctaves))->Value();
    // Envelope-follower attack/release/mode: fixed defaults, not exposed as
    // params this pass (see ExoEQ.h) -- EnvelopeFollowerParams{} default
    // (5ms attack, 100ms release, Peak mode) is already a reasonable
    // general-purpose setting.
    s.sidechainEnv = exoeq::EnvelopeFollowerParams{};

    s.freqShiftSlot = static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftSlot))->Value());
    s.distortionSlot = static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandDistortionSlot))->Value());
    s.delaySlot = static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandDelaySlot))->Value());

    // Node groups: raw int param read straight back as the bitmask --
    // GroupMask's own range (0-255 via InitInt) already matches
    // BandSettings::groupMask's 8-bit usage exactly, so this is a direct
    // cast, same as the filter-type enum cast above.
    s.groupMask = static_cast<unsigned int>(plug.GetParam(BandParamIdx(bandIndex, kBandGroupMask))->Value());
    s.groupEnvEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandGroupEnvEnabled))->Value() > 0.5;
    s.groupEnvModOctaves = plug.GetParam(BandParamIdx(bandIndex, kBandGroupEnvModOctaves))->Value();

    return s;
  }

  // Applies the global mod matrix to one band's already-read (unmodulated)
  // settings, filtering the shared connection list down to just the
  // connections actually targeting this band. ModMatrix::Apply itself
  // takes a flat connection list with no per-band concept (by design --
  // it's generic, see ModMatrix.h), so the per-band targeting this plugin
  // actually needs is done here, one call per band, reusing the same
  // ModMatrix instance (its source values were already set once per block
  // in ProcessBlock -- only the connection list changes per band).
  exoeq::BandSettings ApplyModMatrixForBand(exoeq::ModMatrix& matrix, const exoeq::BandSettings& base,
                                             const std::vector<exoeq::ModMatrix::Connection>& allConnections,
                                             int bandIndex, const int* targetBand)
  {
    std::vector<exoeq::ModMatrix::Connection> forThisBand;
    for (size_t i = 0; i < allConnections.size(); ++i)
    {
      if (targetBand[i] == bandIndex)
        forThisBand.push_back(allConnections[i]);
    }
    if (forThisBand.empty())
      return base; // common case (no connection targets this band) -- skip ModMatrix entirely rather than pay for a copy+no-op Apply

    matrix.SetConnections(forThisBand);
    exoeq::BandSettings modulated;
    matrix.Apply(base, modulated);
    return modulated;
  }
}

void ExoEQ::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // Cheap (one bool param read) to do unconditionally every block --
  // mirrors how mKeyTracker.Tick() below already re-derives its state
  // from current params rather than caching a stale decision. See
  // DSP/KeyTrack.h's SetUseMtsEsp/MtsEspActive for what this actually
  // gates: with no MTS-ESP master running anywhere (the common case),
  // this is a no-op regardless of the param's value.
  mKeyTracker.SetUseMtsEsp(GetParam(kGlobalMtsEspEnabled)->Value() > 0.5);

  // Cheap to call every block (internally a single int compare-and-skip
  // -- see RecomputeAndReportLatency's own comment): this is what makes
  // toggling kGlobalFftEnabled take effect on the host's PDC reporting
  // right away, rather than only on the next OnReset/sample-rate change.
  RecomputeAndReportLatency();

  // Ticked once per block, not per sample: with portamento=0 (the only
  // reachable setting until a UI exposes it) Tick() is an instant jump
  // anyway, so this is still exactly Phase 1's behavior. Once portamento
  // gets a knob, ticking once per block will make the glide step in
  // block-sized increments rather than sample-smooth -- fine at small
  // buffer sizes, worth revisiting (per-sample tick inside the s-loop)
  // before portamento is actually exposed.
  const double keyTrackedHz = mKeyTracker.Tick();

  // Groups: pushed to both chains every block, same "cheap, just always do
  // it" choice ProcessBlock already makes for mKeyTracker.SetUseMtsEsp
  // above -- only kNumGroups (8) param reads + SetGroupSettings calls,
  // nowhere near the per-band filter-coefficient cost the band-skip
  // optimization below exists to avoid.
  for (int g = 0; g < exoeq::kNumGroups; ++g)
  {
    exoeq::GroupSettings gs;
    gs.mute = GetParam(GroupParamIdx(g, kGroupMute))->Value() > 0.5;
    gs.gainLinear = std::pow(10.0, GetParam(GroupParamIdx(g, kGroupGainDb))->Value() / 20.0);
    // Envelope attack/release/mode: fixed defaults, not exposed as params
    // this pass -- same scope-limiting precedent as per-band sidechain's
    // own envelope (see ReadBandSettings's sidechainEnv comment above).
    gs.envelope = exoeq::EnvelopeFollowerParams{};
    mCircleChain[0].SetGroupSettings(g, gs);
    mCircleChain[1].SetGroupSettings(g, gs);
  }

  // Mod matrix: tick the 4 wired LFOs once per block (not per sample --
  // same block-rate-is-fine choice already made for mKeyTracker.Tick()
  // above) and feed them into source slots 0-3; slots 4-7 are reserved/
  // unwired (see ExoEQ.h's kMaxModSources comment) and simply keep
  // whatever SetSourceValue last left them at, which is never (always 0).
  for (int i = 0; i < kMaxModSources; ++i)
  {
    exoeq::LfoParams lp;
    lp.rateHz = GetParam(LfoParamIdx(i, kLfoRateHz))->Value();
    lp.waveform = static_cast<exoeq::LfoWaveform>(static_cast<int>(GetParam(LfoParamIdx(i, kLfoWaveform))->Value()));
    mModLfos[i].SetParams(lp);
    mModMatrix.SetSourceValue(i, mModLfos[i].Tick());
  }

  // Read every connection slot's params once per block (not once per band
  // -- kMaxModConnections is small (8), so this one pass plus the
  // per-band filter in ApplyModMatrixForBand above is far cheaper than
  // re-reading all 8 connections' params again for every one of up to 128
  // bands.
  std::vector<exoeq::ModMatrix::Connection> allModConnections;
  int modConnTargetBand[kMaxModConnections] = {};
  for (int i = 0; i < kMaxModConnections; ++i)
  {
    exoeq::ModMatrix::Connection c;
    c.enabled = GetParam(ModConnParamIdx(i, kModConnEnabled))->Value() > 0.5;
    c.sourceIndex = static_cast<int>(GetParam(ModConnParamIdx(i, kModConnSourceIndex))->Value());
    c.destination = static_cast<exoeq::ModDestination>(static_cast<int>(GetParam(ModConnParamIdx(i, kModConnDestination))->Value()));
    c.depth = GetParam(ModConnParamIdx(i, kModConnDepth))->Value();
    allModConnections.push_back(c);
    modConnTargetBand[i] = static_cast<int>(GetParam(ModConnParamIdx(i, kModConnTargetBand))->Value());
  }

  // PLUG_CHANNEL_IO is "2.2-2": the ONLY layout this plugin declares is
  // 2 main-in + 2 sidechain-in -> 2 out, chosen specifically so there's no
  // runtime ambiguity about which bus is which -- inputs[0..1] are always
  // main L/R, inputs[2..3] are always sidechain L/R, full stop. No
  // sidechain plugged in on the host side just means these read as
  // silence, harmless since every band's sidechainEnabled still defaults
  // to false.
  //
  // Reading an IParam::Value() is cheap regardless of band count; the
  // expensive part this avoids is downstream (SetBandSettings's filter
  // coefficient recalc), not this read.
  //
  // Only bands that are active, OR that just transitioned active->inactive
  // this block (to push the mute flag through exactly once), get their
  // settings pushed into BandChain at all -- an already-inactive band is
  // skipped entirely. Without this, every one of the kMaxBands bands would
  // recompute filter coefficients every block regardless of use.
  int numActiveBands = 0;
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (active) ++numActiveBands;
    if (!active && !mBandWasActive[bandIndex])
      continue;

    const exoeq::BandSettings baseSettings = ReadBandSettings(*this, bandIndex, keyTrackedHz);
    const exoeq::BandSettings settings = ApplyModMatrixForBand(mModMatrix, baseSettings, allModConnections, bandIndex, modConnTargetBand);
    // Circle assignment is read every time a band's settings are pushed
    // (same cost tier as every other per-band param read in this loop) --
    // AssignBandToCircle only does real work (the move) when this value
    // actually changed since last block; otherwise it's exactly as cheap
    // as the flat SetBandSettings call it replaces. Both channels get the
    // SAME circle assignment, same as every other per-band setting here
    // being mirrored to both chains -- a band can't live in a different
    // circle on the left vs right channel.
    const int circleId = static_cast<int>(GetParam(BandParamIdx(bandIndex, kBandCircleIndex))->Value());
    mCircleChain[0].AssignBandToCircle(bandIndex, circleId, settings);
    mCircleChain[1].AssignBandToCircle(bandIndex, circleId, settings);
    mBandWasActive[bandIndex] = active;
  }

  // Deliberately NOT gating on NOutChansConnected() -- see the comment this
  // replaced: that call can report 0 in some hosts' offline/preview render
  // paths even though `outputs` is a real buffer the host expects filled,
  // which silently drops the whole output. Always process a fixed, known
  // channel count instead.
  const int nChans = 2;

  // With zero bands active (a fresh instance's starting state, per the
  // user's explicit "off by default" requirement), BandChain would sum
  // zero bands and output dead silence -- there's no separate "dry"
  // signal path through the parallel-band architecture otherwise. An EQ
  // with no bands added yet should pass audio through unaffected, so
  // that's handled explicitly here rather than silently going quiet.
  const bool bypass = (numActiveBands == 0);

  for (int c = 0; c < nChans; c++)
  {
    for (int s = 0; s < nFrames; s++)
    {
      if (bypass)
      {
        outputs[c][s] = inputs[c][s];
        continue;
      }
      // inputs[2]/inputs[3] are the sidechain bus's L/R, guaranteed by the
      // single-layout "2.2-2" PLUG_CHANNEL_IO -- channel c's main signal
      // pairs with sidechain channel (2+c).
      const double sidechainSample = static_cast<double>(inputs[2 + c][s]);
      outputs[c][s] = static_cast<sample>(mCircleChain[c].ProcessSample(inputs[c][s], sidechainSample));
    }
  }

  // Global FFT brick-wall stage (P6, first piece) -- runs AFTER every
  // band has already been summed into `outputs` above, per channel.
  // This is the "global stage, not a per-band option" shape: exactly
  // one STFT per channel regardless of how many bands exist. Applies
  // regardless of whether `bypass` was active above -- a bypassed/
  // zero-band instance still has real audio in `outputs` (the dry
  // passthrough signal), which this stage should still shape if the
  // user has turned it on, same as any other global processing stage
  // would apply whether or not any band happens to be active.
  if (GetParam(kGlobalFftEnabled)->Value() > 0.5)
  {
    exoeq::FftBrickwallParams fftParams;
    fftParams.lowCutHz = GetParam(kGlobalFftLowCutHz)->Value();
    fftParams.highCutHz = GetParam(kGlobalFftHighCutHz)->Value();

    EnsureFftScratchCapacity(nFrames);

    for (int c = 0; c < nChans; ++c)
    {
      mFftContainer[c].SetParams(fftParams);
      // Convert this channel's block from iPlug2's `sample` type
      // (typically double) into the float scratch buffer
      // FftBrickwallContainer::Process actually needs, process
      // in-place in that scratch buffer (safe -- Process documents
      // in/out aliasing as fine), then convert back. Two linear passes
      // over nFrames floats is negligible next to the FFT itself, so
      // this isn't a meaningful added cost on top of the stage the
      // user already opted into.
      float* scratch = mFftScratch[c].data();
      for (int s = 0; s < nFrames; ++s)
        scratch[s] = static_cast<float>(outputs[c][s]);

      mFftContainer[c].Process(scratch, scratch, nFrames);

      for (int s = 0; s < nFrames; ++s)
        outputs[c][s] = static_cast<sample>(scratch[s]);
    }
  }
}

bool ExoEQ::SerializeState(IByteChunk& chunk) const
{
  // Base class first -- every normal automatable param, unchanged from
  // before this feature existed, so an old saved project with no
  // trailing formula string still round-trips through UnserializeState
  // below exactly as it always did (its GetStr just reads past end-of-
  // chunk safely, see there).
  const bool paramsOk = SerializeParams(chunk);
  chunk.PutStr(mFractalFormula.c_str());
  // The 9 custom variables' min/max ranges -- same reasoning as the
  // formula string just above: these are configuration, not automation,
  // so they ride this override rather than a normal param.
  for (int i = 0; i < kNumFractalCustomVars; ++i)
  {
    chunk.Put(&mFractalVarMin[i]);
    chunk.Put(&mFractalVarMax[i]);
  }
  // Image-to-EQ-curve's source file path is DELIBERATELY NOT persisted
  // here (changed from an earlier draft of this feature, per the user's
  // explicit call): picking an image and clicking Generate is a
  // ONE-SHOT action. Its only lasting effect is the ordinary band params
  // it writes -- cutoff/resonance/gain/filter-type/active -- which
  // SerializeParams above already saves and restores exactly like any
  // hand-built band, through the plugin's normal state (and so through
  // the DAW session's saved plugin state, FXB included) with zero extra
  // code needed for that part. There is no reason to also remember WHICH
  // image produced them: the image is a disposable input to a generator,
  // not a live dependency the plugin needs again on reload, and an
  // absolute path saved on one machine would silently fail to resolve on
  // another anyway. mImageFilePath lives only as transient in-session
  // state (so the status line can show the current file while you're
  // working), and intentionally resets to empty on every reload.

  // ROUND 27 -- user-saved preset circles (CircleViewControl::mUserPresets)
  // were session-only until now, flagged as a "bigger future task" when
  // SaveCurrentCircleAsPreset was first added. Persisted field-by-field
  // (not as a raw struct blob) so struct padding/layout differences
  // between builds can't corrupt a saved project -- same reasoning as
  // mFractalVarMin/Max just above using individual chunk.Put calls rather
  // than one chunk.Put(&wholeArray). mCircleView can be null here (no
  // graph built yet, e.g. a headless instantiation) -- writes a 0 count
  // in that case so UnserializeState's read loop stays in sync either way.
  const std::vector<CircleViewControl::PresetCircleDef> userPresets = mCircleView ? mCircleView->GetUserPresets() : std::vector<CircleViewControl::PresetCircleDef>();
  const int32_t presetCount = static_cast<int32_t>(userPresets.size());
  chunk.Put(&presetCount);
  for (const auto& p : userPresets)
  {
    chunk.PutStr(p.name);
    chunk.Put(&p.bandCount);
    for (int b = 0; b < 3; ++b)
    {
      chunk.Put(&p.bands[b].ordinal);
      chunk.Put(&p.bands[b].cutoffHz);
      chunk.Put(&p.bands[b].resonance);
      chunk.Put(&p.bands[b].forceMorph50);
    }
  }

  return paramsOk;
}

int ExoEQ::UnserializeState(const IByteChunk& chunk, int startPos)
{
  int pos = UnserializeParams(chunk, startPos);
  if (pos >= 0 && pos < chunk.Size())
  {
    WDL_String tmp;
    pos = chunk.GetStr(tmp, pos);
    mFractalFormula = tmp.Get() ? tmp.Get() : "";

    // Loading a project saved before the EXOMUSICA variables existed means
    // there's nothing left to read here -- pos lands at/past chunk.Size()
    // and the loop below is simply skipped, leaving every range at its
    // 0..1 passthrough default (the same behavior as if ranging had
    // never been added), rather than reading garbage bytes.
    for (int i = 0; i < kNumFractalCustomVars && pos >= 0 && pos + static_cast<int>(sizeof(double)) * 2 <= chunk.Size(); ++i)
    {
      pos = chunk.Get(&mFractalVarMin[i], pos);
      pos = chunk.Get(&mFractalVarMax[i], pos);
    }

    // mImageFilePath is never read back here -- it's deliberately not
    // part of the saved chunk at all (see SerializeState's note above).
    // Every reload starts with no image selected; the bands an earlier
    // Generate-from-Image click produced are already restored above, as
    // ordinary params, with nothing image-specific required to get them
    // back.
    mImageFilePath.clear();

    // ROUND 27 -- read back the user-saved preset circles, mirroring
    // SerializeState's field-by-field write above. Bounds-checked the
    // same way the fractal-vars loop above is: a project saved before
    // this feature existed (or any truncated/corrupt tail) just leaves
    // pos at/past chunk.Size() and the loop below never runs, so the
    // tray falls back to empty user presets rather than reading garbage.
    std::vector<CircleViewControl::PresetCircleDef> userPresets;
    if (pos >= 0 && pos + static_cast<int>(sizeof(int32_t)) <= chunk.Size())
    {
      int32_t presetCount = 0;
      pos = chunk.Get(&presetCount, pos);
      for (int32_t i = 0; i < presetCount && pos >= 0 && pos < chunk.Size(); ++i)
      {
        CircleViewControl::PresetCircleDef def{};
        WDL_String name;
        pos = chunk.GetStr(name, pos);
        std::snprintf(def.name, sizeof(def.name), "%s", name.Get() ? name.Get() : "");
        pos = chunk.Get(&def.bandCount, pos);
        for (int b = 0; b < 3 && pos >= 0; ++b)
        {
          pos = chunk.Get(&def.bands[b].ordinal, pos);
          pos = chunk.Get(&def.bands[b].cutoffHz, pos);
          pos = chunk.Get(&def.bands[b].resonance, pos);
          pos = chunk.Get(&def.bands[b].forceMorph50, pos);
        }
        userPresets.push_back(def);
      }
    }
    if (mCircleView)
    {
      mCircleView->SetUserPresets(userPresets);
      mHasPendingUserPresets = false;
    }
    else
    {
      // GUI not open yet -- stash for OnLayout to apply once circleView
      // exists (see mPendingUserPresets's own note in ExoEQ.h).
      mPendingUserPresets.clear();
      for (const auto& p : userPresets)
      {
        PendingPresetDef pend{};
        std::snprintf(pend.name, sizeof(pend.name), "%s", p.name);
        pend.bandCount = p.bandCount;
        for (int b = 0; b < 3; ++b)
          pend.bands[b] = { p.bands[b].ordinal, p.bands[b].cutoffHz, p.bands[b].resonance, p.bands[b].forceMorph50 };
        mPendingUserPresets.push_back(pend);
      }
      mHasPendingUserPresets = true;
    }
  }
  else
  {
    // Loading a project saved before this feature existed -- no
    // trailing string to read, stay at the knob-driven default.
    mFractalFormula.clear();
    mImageFilePath.clear();
    if (mCircleView) mCircleView->SetUserPresets({});
    else { mPendingUserPresets.clear(); mHasPendingUserPresets = true; }
  }
  return pos;
}

void ExoEQ::GenerateFractalBands()
{
  const double sr = GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0;

  exoeq::FractalGenParams fp;
  fp.seedFreqHz = GetParam(kGlobalFractalSeedHz)->Value();
  fp.ratio = GetParam(kGlobalFractalRatio)->Value();
  fp.branchFactor = static_cast<int>(GetParam(kGlobalFractalBranchFactor)->Value());
  fp.depth = static_cast<int>(GetParam(kGlobalFractalDepth)->Value());
  fp.seedGainDb = GetParam(kGlobalFractalGainDb)->Value();
  fp.gainDecayPerLevel = GetParam(kGlobalFractalGainDecay)->Value();
  fp.resonance = GetParam(kGlobalFractalResonance)->Value();
  fp.jitterPct = GetParam(kGlobalFractalJitterPct)->Value();
  fp.jitterSeed = static_cast<unsigned int>(GetParam(kGlobalFractalJitterSeed)->Value());
  fp.symmetric = GetParam(kGlobalFractalSymmetric)->Value() > 0.5;
  fp.formula = mFractalFormula;
  // Scale each custom knob's 0..1 position into its user-set range
  // BEFORE handing it to the generator -- see FractalGenParams::
  // customVarValue's own note on why the generator never sees a raw
  // 0..1 value for any of E/X/O/M/U/S/I/C/A.
  for (int i = 0; i < kNumFractalCustomVars; ++i)
  {
    const double knob01 = GetParam(kGlobalFractalVarStart + i)->Value();
    fp.customVarValue[i] = mFractalVarMin[i] + knob01 * (mFractalVarMax[i] - mFractalVarMin[i]);
  }

  const auto nodes = exoeq::GenerateFractalSeries(fp, sr);
  WriteGeneratedNodesIntoInactiveBands(nodes, CheapFilterOrdinalFromParam(), EBandSource::Fractal);
}

void ExoEQ::GenerateTransformBands()
{
  const double sr = GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0;

  // Read every CURRENTLY ACTIVE band's cutoff/resonance/gain into a node
  // series -- this is the "transform filter" input (vs. the fractal
  // generator's closed-form formula), per the P5 scope doc's own plan
  // for the shared machine. Deliberately does NOT read sends, key-track,
  // Energy2, or group membership -- this is a frequency-domain remap of
  // the EQ CURVE, not a full clone of every per-band feature; a
  // transformed band starts as a plain filter band at its new frequency,
  // same as a fresh fractal-generated one.
  std::vector<exoeq::GeneratedNode> input;
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    if (GetParam(BandParamIdx(bandIndex, kBandActive))->Value() <= 0.5) continue;
    exoeq::GeneratedNode node;
    node.freqHz = GetParam(BandParamIdx(bandIndex, kBandCutoff))->Value();
    node.resonance = GetParam(BandParamIdx(bandIndex, kBandResonance))->Value();
    node.gainDb = GetParam(BandParamIdx(bandIndex, kBandGainDb))->Value();
    input.push_back(node);
  }
  if (input.empty()) return; // nothing active to transform -- a silent no-op, same as the fractal generator's empty-series early-return

  exoeq::TransformParams tp;
  tp.pivotHz = GetParam(kGlobalTransformPivotHz)->Value();
  tp.exponent = GetParam(kGlobalTransformExponent)->Value();

  const auto output = exoeq::ApplyTransform(input, tp, sr);
  WriteGeneratedNodesIntoInactiveBands(output, CheapFilterOrdinalFromParam(), EBandSource::Transform);
}

void ExoEQ::GenerateImageBands()
{
  // Silent no-op with no file picked yet -- same shape as the transform
  // filter's empty-input early-return, just at the "haven't even tried"
  // stage rather than "tried and got nothing to work with".
  if (mImageFilePath.empty())
  {
    mLastImageError = "no image selected";
    return;
  }

  const double sr = GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0;

  exoeq::ImageToEqParams ip;
  ip.nodeCount = static_cast<int>(GetParam(kGlobalImageNodeCount)->Value());
  ip.minFreqHz = GetParam(kGlobalImageMinFreqHz)->Value();
  ip.maxFreqHz = GetParam(kGlobalImageMaxFreqHz)->Value();
  ip.gainRangeDb = GetParam(kGlobalImageGainRangeDb)->Value();
  ip.resonance = GetParam(kGlobalImageResonance)->Value();
  ip.invertLuminance = GetParam(kGlobalImageInvertLuminance)->Value() > 0.5;

  const auto result = exoeq::GenerateFromImageFile(mImageFilePath, ip, sr);
  if (!result.ok)
  {
    // No band touched, no param changed -- a bad path or an
    // unsupported/corrupt file leaves the plugin exactly as it was
    // before the click, same "fail loud but fail safe" policy as the
    // formula evaluator's own try/catch fallback in FractalGenerator.h.
    mLastImageError = result.errorMessage;
    return;
  }

  mLastImageError.clear();
  WriteGeneratedNodesIntoInactiveBands(result.nodes, CheapFilterOrdinalFromParam(), EBandSource::Image);
}

int ExoEQ::CheapFilterOrdinalFromParam() const
{
  // Cheap-filter-only mapping (user's explicit requirement, originally
  // for the fractal generator, now shared with the transform filter) --
  // all 4 options are single-StateVariableFilter dropdown types,
  // matching DSP/FilterCore.h's FilterTypeId ordinals exactly (see the
  // InitEnum call above for the option list these indices correspond
  // to: 0=LP12, 1=HP12, 2=BP12, 3=Notch12). Never maps to an Airwindows/
  // character type, by design -- those are far more expensive
  // per-instance than a single SVF, and a generator that can produce up
  // to 64 bands at once is exactly where that cost would add up.
  static const int kFilterOrdinal[4] = {
    static_cast<int>(exoeq::FilterTypeId::LP12),
    static_cast<int>(exoeq::FilterTypeId::HP12),
    static_cast<int>(exoeq::FilterTypeId::BP12),
    static_cast<int>(exoeq::FilterTypeId::Notch12)
  };
  const int filterChoice = std::clamp(static_cast<int>(GetParam(kGlobalFractalFilterType)->Value()), 0, 3);
  return kFilterOrdinal[filterChoice];
}

void ExoEQ::WriteGeneratedNodesIntoInactiveBands(const std::vector<exoeq::GeneratedNode>& nodes, int filterOrdinal, EBandSource source)
{
  if (nodes.empty()) return;

  // Non-destructive by default (confirmed with the user): write into the
  // first N currently-INACTIVE band slots, activating them, rather than
  // touching any band already in use. If there aren't enough free slots
  // for every generated node, the rest are simply dropped -- silently
  // truncating into bands you didn't intend to touch would be worse.
  //
  // KNOWN CONSEQUENCE, still true even with the source tag below: since
  // this activates new bands rather than EXCLUDING them from later
  // input, a band written here still reads as an ordinary active band
  // to the NEXT generator click -- including this same one. The tag
  // doesn't stop that (clicking Transform twice still compounds); what
  // it adds is the ability to find and clear exactly what a given
  // generator made afterward (see ClearBandsBySource below), as a real
  // mitigation rather than a full fix. See each generator's own header
  // comment for the un-changed stacking discussion.
  //
  // MOD-MATRIX COLLISION MITIGATION (new this round -- see the Features
  // Doc's own note on this risk): mod connections target a band by raw
  // index, so landing a generated node on an index a DORMANT (target
  // inactive, so currently silent) connection already points at would
  // wake that connection up unexpectedly the moment this activates the
  // band. To avoid that, slots are filled in TWO PASSES: first every
  // inactive band NOT targeted by any currently-enabled mod connection,
  // then (only if more nodes remain than that leaves room for) the
  // remaining inactive bands regardless of targeting -- so a collision
  // only ever happens when there's truly no untargeted room left, not
  // as the default outcome. This doesn't change WHERE an enabled
  // connection's target band physically sits, and it doesn't touch
  // connections at all -- it just steers new generator output away from
  // indices that already have one waiting, when it can.
  std::array<bool, kMaxBands> targetedByEnabledModConn{};
  for (int c = 0; c < kMaxModConnections; ++c)
  {
    if (GetParam(ModConnParamIdx(c, kModConnEnabled))->Value() <= 0.5) continue;
    const int target = std::clamp(static_cast<int>(GetParam(ModConnParamIdx(c, kModConnTargetBand))->Value()), 0, kMaxBands - 1);
    targetedByEnabledModConn[static_cast<size_t>(target)] = true;
  }

  auto writeOneBand = [&](int bandIndex, const exoeq::GeneratedNode& node)
  {
    SetParameterValue(BandParamIdx(bandIndex, kBandCutoff), GetParam(BandParamIdx(bandIndex, kBandCutoff))->ToNormalized(node.freqHz));
    SetParameterValue(BandParamIdx(bandIndex, kBandResonance), GetParam(BandParamIdx(bandIndex, kBandResonance))->ToNormalized(node.resonance));
    SetParameterValue(BandParamIdx(bandIndex, kBandGainDb), GetParam(BandParamIdx(bandIndex, kBandGainDb))->ToNormalized(node.gainDb));
    SetParameterValue(BandParamIdx(bandIndex, kBandFilterType), GetParam(BandParamIdx(bandIndex, kBandFilterType))->ToNormalized(static_cast<double>(filterOrdinal)));
    SetParameterValue(BandParamIdx(bandIndex, kBandSource), GetParam(BandParamIdx(bandIndex, kBandSource))->ToNormalized(static_cast<double>(source)));
    SetParameterValue(BandParamIdx(bandIndex, kBandMute), 0.0);
    SetParameterValue(BandParamIdx(bandIndex, kBandActive), 1.0);
  };

  size_t nodeIdx = 0;

  // Pass 1: inactive AND untargeted slots -- the preferred, collision-free case.
  for (int bandIndex = 0; bandIndex < kMaxBands && nodeIdx < nodes.size(); ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (active || targetedByEnabledModConn[static_cast<size_t>(bandIndex)]) continue;
    writeOneBand(bandIndex, nodes[nodeIdx]);
    ++nodeIdx;
  }

  // Pass 2: only runs if pass 1 didn't have enough room -- falls back to
  // inactive-but-targeted slots rather than dropping nodes that would
  // otherwise fit. This is the one case a collision can still happen;
  // it's a real tradeoff (silently dropping the node instead would avoid
  // it entirely), chosen because "the generator did less than you asked
  // because of an unrelated mod connection you may have forgotten about"
  // is a more confusing outcome than "this specific edge case can still
  // wake a connection up" -- flagged either way in this round's README.
  for (int bandIndex = 0; bandIndex < kMaxBands && nodeIdx < nodes.size(); ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (active || !targetedByEnabledModConn[static_cast<size_t>(bandIndex)]) continue;
    writeOneBand(bandIndex, nodes[nodeIdx]);
    ++nodeIdx;
  }

  if (GetUI())
    GetUI()->SetAllControlsDirty();
}

void ExoEQ::ClearBandsBySource(EBandSource source)
{
  // Deliberately never touches EBandSource::HandBuilt bands, regardless
  // of what's passed -- this function only ever clears a GENERATOR's
  // own output, never a user's hand-built work. (A caller passing
  // HandBuilt here would be a bug in ExoEQ.cpp itself, not something a
  // user can trigger from the UI -- there's no "Clear Hand-Built"
  // button.)
  if (source == EBandSource::HandBuilt) return;

  bool anyCleared = false;
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (!active) continue;

    const auto bandSource = static_cast<EBandSource>(static_cast<int>(GetParam(BandParamIdx(bandIndex, kBandSource))->Value()));
    if (bandSource != source) continue;

    // Deactivating (rather than zeroing cutoff/gain/etc.) matches how
    // every other "remove this band" path in this plugin already works
    // -- an inactive band is forced muted and costs almost nothing per
    // block (see kBandActive's own note in ExoEQ.h), and its old
    // numeric values staying in place until overwritten is harmless
    // since they're never read while inactive.
    SetParameterValue(BandParamIdx(bandIndex, kBandActive), 0.0);
    SetParameterValue(BandParamIdx(bandIndex, kBandSource), GetParam(BandParamIdx(bandIndex, kBandSource))->ToNormalized(static_cast<double>(EBandSource::HandBuilt)));
    anyCleared = true;
  }

  if (anyCleared && GetUI())
    GetUI()->SetAllControlsDirty();
}

void ExoEQ::EnsureFftScratchCapacity(int nFrames)
{
  for (auto& scratch : mFftScratch)
  {
    if (static_cast<int>(scratch.size()) < nFrames)
      scratch.resize(static_cast<size_t>(nFrames));
  }
}
#endif
