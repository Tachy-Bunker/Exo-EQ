#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "DSP/BandChain.h"
#include "DSP/CircleChain.h"
#include "DSP/KeyTrack.h"
#include "DSP/MtsEspClient.h"
#include "DSP/BandGroups.h"
#include "DSP/ModMatrix.h"
#include "DSP/LFO.h"
#include "DSP/FractalGenerator.h"
#include "DSP/TransformFilter.h"
#include "DSP/ImageToEq.h"
#include "DSP/FftContainer.h"
#include <functional>
#include <string>
#include <vector>

const int kNumPresets = 1;

// Scale decision (confirmed with the user, originally 128, raised to 256
// in the circles/GUI round): each band gets REAL, individually
// DAW-automatable params -- not a "selected slot" scheme. The user's own
// reasoning for the raise: a future zoom feature (for tight clusters of
// bands on the new circle gauge) makes 128 feel tight once it exists, so
// headroom was taken now rather than revisited later -- a param-chunk
// scheme (fewer automatable params, internal state via
// PLUG_DOES_STATE_CHUNKS) was considered and explicitly rejected, same as
// before, in favor of per-band-by-name automation even at this count.
//
// Params are computed, not hand-enumerated -- BandParamIdx(bandIndex,
// offset) is the single source of truth both the constructor and
// ProcessBlock use, so there's no way for the two to drift out of sync the
// way separate kCutoffA/kCutoffB constants would once there are 256 of them.
constexpr int kMaxBands = 256;

enum EBandParamOffset
{
  kBandCutoff = 0,
  kBandResonance,
  kBandMorph,
  // Per-band makeup/trim gain -- a real gap this project otherwise had
  // (BandSettings::gainLinear existed and was already read by the mod
  // matrix's GainDb destination and by node groups' accumulative gain,
  // but nothing ever SET its base value -- it silently defaulted to
  // 1.0/0dB forever). Added now because the fractal generator (see
  // DSP/FractalGenerator.h) needs a real per-band level to write its
  // per-level gain decay into -- without this there'd be nowhere for
  // that part of "fractal" to go. Independent of and stacks with the
  // mod matrix's GainDb modulation and any group gain, same layering
  // every other per-band + global gain-ish control in this plugin uses.
  kBandGainDb,
  // 4th generic filter-type knob -- added specifically for AwYLowpass's
  // powFactor (DSP/FilterCore.h's FilterParams::extra). Unused by every
  // other dropdown type; see DSP/AirwindowsFilters.h's
  // AirwindowsYLowpassFilter header note for why this one needed a
  // dedicated automatable slot instead of reusing resonance/morph like
  // every earlier Airwindows port.
  kBandFilterExtra,
  // Dropdown filter TYPE (spec: "usable with a dropdown like in
  // FabFilter going highpass > highpass 24oct"). See DSP/FilterTypes.h
  // for what each option actually does; DSP/FilterCore.h's FilterTypeId
  // is the single source of truth for the option list/order, which this
  // param's InitEnum call in ExoEQ.cpp must match exactly.
  kBandFilterType,
  kBandKeyTrackAmt,
  kBandMidiChannel,
  kBandMute,
  kBandSolo,
  // "Active" is new: with 256 bands always existing as real params, this
  // is what lets a user's project only show/use however many bands they've
  // actually added, while the rest sit fully inert. Inactive bands are
  // forced muted (see ReadBandSettings in ExoEQ.cpp) regardless of their
  // own Mute param, and BandChain costs almost nothing per inactive band
  // (Band::ProcessSample returns immediately on mute, before touching the
  // filter or any send) -- so having 128 "slots" exist doesn't mean 128
  // bands' worth of audio CPU whenever most are inactive.
  kBandActive,

  // Per-node contained functions (spec: "sidechain input, sends (frequency
  // shifting, distortion, delay)") -- the DSP for all of these has existed
  // and been unit-tested since earlier in the project; this is what
  // actually makes them reachable from a knob. All three sends' DSP
  // classes (DelaySend/DistortionSend/FrequencyShiftSend) already clamp
  // their own parameters to sane/stable ranges internally, so these UI
  // ranges are chosen for usability, not safety -- a value outside them
  // still can't destabilize anything downstream.
  kBandDelayEnabled,
  kBandDelayTimeMs,
  kBandDelayFeedback,
  kBandDelayMix,

  kBandDistortionEnabled,
  kBandDistortionDrive,
  kBandDistortionCharacter,
  kBandDistortionMix,

  kBandFreqShiftEnabled,
  kBandFreqShiftHz,
  kBandFreqShiftMix,

  // Sidechain's envelope-follower attack/release/mode aren't exposed yet
  // (kept at fixed, reasonable defaults in ReadBandSettings) -- just
  // enable + modulation depth for this pass, to keep scope bounded;
  // worth revisiting once this first layer is confirmed working.
  kBandSidechainEnabled,
  kBandSidechainModOctaves,

  // Reorderable per-node effect order (spec: "order of effect"). Each
  // send gets a slot number (0/1/2); BandChain.h sorts by slot every
  // sample and runs the sends in that order. Default values (0/1/2
  // below, see ExoEQ.cpp) reproduce the exact fixed order this shipped
  // with before (freq-shift, distortion, delay) -- existing projects see
  // zero change until a user deliberately drags something.
  kBandFreqShiftSlot,
  kBandDistortionSlot,
  kBandDelaySlot,

  // Energy2 (spec: real port of Airwindows' Energy2, user-requested).
  // NOT a filter-type dropdown entry -- see DSP/AirwindowsFilters.h's
  // AirwindowsEnergy2Processor and DSP/BandChain.h's
  // kMaxEnergy2Instances/RecomputeEnergy2Slots for why. These 4 params
  // exist on every one of the 256 band slots same as everything else
  // (cheap to declare/automate), but only kMaxEnergy2Instances=20
  // enabled bands actually get live Energy2 processing at once --
  // enforced in DSP/BandChain.h, not here. The 2 grouped knobs
  // (GlitterHiss, RatScrapeFizz) collapse the source's 8 raw intensity
  // controls down to how the user says this is actually used; Mix is
  // the source's own bipolar wet.
  kBandEnergy2Enabled,
  kBandEnergy2GlitterHiss,
  kBandEnergy2RatScrapeFizz,
  kBandEnergy2Mix,

  // Node groups (spec: "node groups (multi-group membership, accumulative
  // bitmask params, auto-disabling global envelope)"). The DSP for this
  // (DSP/BandGroups.h + the group-handling parts of DSP/BandChain.h) was
  // built and unit-tested in an earlier session; these 3 params are what
  // finally make it reachable. GroupMask is an int 0..255 read as a raw
  // bitmask (bit g set = member of group g) rather than 8 separate bool
  // params/band -- same "one automatable int beats 8 bools" choice this
  // project hasn't made before, but 8 extra params on every one of 128
  // bands (1024 params) for membership alone was judged not worth it next
  // to a single int a host's generic automation lane can still drive.
  kBandGroupMask,
  kBandGroupEnvEnabled,
  kBandGroupEnvModOctaves,

  // Per-band SOURCE tag (the gap flagged across all three generators'
  // READMEs/Features-Doc notes this round, now finally addressed): a
  // plain enum recording WHO wrote this band's current settings --
  // HandBuilt (the default, for a band you added/edited yourself) or
  // one of the 3 generators. Purely bookkeeping: nothing in the DSP
  // reads this, it changes no audio. What it unlocks: ClearBandsBySource
  // below, which lets you clear exactly "everything the fractal
  // generator made" (etc.) without hunting for which bands those were
  // by ear/eye, and it's the foundation for being able to SEE which
  // bands are generated vs. hand-built (a future UI refinement -- not
  // built this round, see the README). Every generator's write path
  // (WriteGeneratedNodesIntoInactiveBands) sets this on every band it
  // touches; every other band-editing path (dragging on the graph,
  // typing a knob) leaves it alone, so a hand-edited band keeps
  // whatever tag it already had -- INCLUDING a generator's tag, if you
  // hand-tweak a generated band afterward. That's deliberate: "this
  // band originated from the fractal generator" stays true/useful
  // information even after a tweak, and flipping it back to HandBuilt
  // the moment you touch one knob would make the tag far less reliable
  // for its actual purpose (bulk-clearing a generator's output), not
  // more accurate.
  kBandSource,

  // Which circle (DSP/CircleChain.h) this band currently lives in --
  // confirmed with the user alongside the circle-based GUI redesign:
  // circles hold any number of bands (parallel within a circle, same as
  // this plugin's whole architecture always was) and now run in SERIES
  // with each other, in circle-ID order (see CircleChain.h for exactly
  // why that needs no separate "order" param). Defaults to 0 -- every
  // band starts in circle 0, the same single implicit circle every
  // existing project already has every band in, so loading an older
  // save is a complete no-op here: one circle, every band in it, series
  // chain of length 1, byte-for-byte the old flat parallel sum. Range is
  // 0..kMaxCircles-1; the GUI is what actually creates/deletes/reorders
  // circles by moving bands' values here, there is no separate "circle
  // exists" param -- a circle simply exists for as long as at least one
  // band's CircleIndex points at it (CircleChain::CircleBandCount reads
  // as 0 otherwise, which is what the GUI uses to know a circle is gone).
  kBandCircleIndex,

  kNumParamsPerBand
};

// EBandSource's own enum, int-castable to/from kBandSource's param
// value -- kept separate from EBandParamOffset itself so call sites
// that mean "which generator" read clearly rather than casting a band
// param offset value. Order MUST match the InitEnum option list in
// ExoEQ.cpp's constructor exactly (same rule as every other dropdown
// param in this project).
enum class EBandSource
{
  HandBuilt = 0,
  Fractal,
  Transform,
  Image,
  kNumBandSources
};

// Group/LFO/mod-connection counts and per-item param offsets -- same
// "computed index, not hand enumerated" philosophy as BandParamIdx below,
// now extended to the 3 other repeating structures this round adds.
constexpr int kMaxModSources = 4; // 4 global LFOs; ModMatrix::kMaxSources itself allows 8, the other 4 are reserved/unwired for now (kept at 0) -- scope-limiting this pass same as sidechain's fixed envelope was
constexpr int kMaxModConnections = 8;

enum EGroupParamOffset
{
  kGroupMute = 0,
  kGroupGainDb,
  kNumParamsPerGroup
};

enum ELfoParamOffset
{
  kLfoRateHz = 0,
  kLfoWaveform,
  kNumParamsPerLfo
};

enum EModConnParamOffset
{
  kModConnEnabled = 0,
  kModConnSourceIndex,
  kModConnDestination,
  kModConnDepth,
  kModConnTargetBand,
  kNumParamsPerModConn
};

enum EParams
{
  kBandParamsStart = 0,

  // Global (not per-band) params go AFTER every band param, deliberately
  // -- appending here means no existing band's param index ever moves,
  // so a project automated against an earlier build keeps working
  // exactly as before. First one: real MTS-ESP (the live protocol,
  // DSP/MtsEspClient.h -- distinct from a future per-band .scl file
  // picker, which would be a band-level or global tuning-table param,
  // not this). One global toggle for the whole plugin, not per-band --
  // matches the spec's own framing of MTS-ESP as instance-wide tuning,
  // same as KeyTracker already being one shared instance (ExoEQ.h's
  // mKeyTracker), not one per band.
  kGlobalMtsEspEnabled = kBandParamsStart + kMaxBands * kNumParamsPerBand,

  // UI-only selectors (never read by the DSP, never saved as anything
  // meaningful on their own) -- same retargeting-panel pattern already
  // used for the 128 bands (BandGraphControl's onBandSelected), just for
  // the 8 groups and kMaxModConnections mod slots instead, since neither
  // has a spectrum-graph-like visual to click on. GroupSelect picks which
  // group's Mute/GainDb knobs the one fixed group panel is currently
  // bound to; ModConnSelect does the same for one connection's 5 knobs.
  kGlobalGroupSelect,
  kGlobalModConnSelect,

  // Groups: kNumGroups (8) * kNumParamsPerGroup (Mute, GainDb) real,
  // automatable globals -- GroupParamIdx(g, offset) below is the single
  // source of truth, same role BandParamIdx already plays for bands.
  kGlobalGroupParamsStart,

  // LFOs: the mod matrix's 4 wired sources (RateHz, Waveform per LFO).
  kGlobalLfoParamsStart = kGlobalGroupParamsStart + exoeq::kNumGroups * kNumParamsPerGroup,

  // Mod connections: kMaxModConnections (8) * kNumParamsPerModConn (5)
  // real, automatable globals -- a connection with Enabled=false is a
  // complete no-op (ModMatrix::Apply skips it), so all 8 start disabled
  // and the matrix has zero audible effect on an existing/fresh project
  // until a user deliberately wires one, same "off by default" guarantee
  // every other optional feature in this plugin already gives.
  kGlobalModConnParamsStart = kGlobalLfoParamsStart + kMaxModSources * kNumParamsPerLfo,

  kGlobalModConnParamsEnd = kGlobalModConnParamsStart + kMaxModConnections * kNumParamsPerModConn,

  // Fractal generator (P5 scope, step 1 of 3 -- see the Features Doc).
  // One-shot batch params: read only at the moment the Generate button
  // is clicked (DSP/FractalGenerator.h does the actual math), not by
  // per-sample DSP -- but still real, automatable globals rather than
  // plain UI-only state, since there's a real (if unusual) use for
  // automating a knob and re-clicking Generate at a new value. The
  // formula text itself can't be one of these (iPlug2 params are
  // numeric) -- see mFractalFormula and the SerializeState/
  // UnserializeState overrides below for how that one piece of state is
  // handled instead.
  kGlobalFractalSeedHz = kGlobalModConnParamsEnd,
  kGlobalFractalRatio,
  kGlobalFractalBranchFactor,
  kGlobalFractalDepth,
  kGlobalFractalGainDb,
  kGlobalFractalGainDecay,
  kGlobalFractalResonance,
  kGlobalFractalJitterPct,
  kGlobalFractalJitterSeed,
  kGlobalFractalSymmetric,
  // Cheap-filter-only choice (user's explicit requirement): 4 options,
  // all single-StateVariableFilter dropdown types -- never an Airwindows
  // or character type, which is the whole point. See ExoEQ.cpp's
  // GenerateFractalSeries for the enum-ordinal mapping.
  kGlobalFractalFilterType,

  // 9 user-defined knobs, one per letter of EXOMUSICA -- the user's
  // easter egg: the inline formula references them by their actual
  // uppercase letter (E, X, O, M, U, S, I, C, A -- all 9 distinct, no
  // collisions), spelling the company name out across the generator's
  // own knob row rather than a generic a/b/c/.../i. Each is a plain
  // 0..1 automatable param; the RANGE it's scaled into (min/max, e.g.
  // "E" ranged 20..2000 reads as a frequency-ish number directly in the
  // formula) is set via inline text, same mechanism as the formula box
  // itself, so it lives as a plain member (mFractalVarMin/Max below),
  // not a param -- a min/max pair isn't something you'd automate, just
  // configure once. Order here MUST match DSP/FractalGenerator.h's own
  // ExprEval letter table exactly (E=0 ... A=8).
  kGlobalFractalVarStart,
  kGlobalFractalVarE = kGlobalFractalVarStart,
  kGlobalFractalVarX,
  kGlobalFractalVarO,
  kGlobalFractalVarM,
  kGlobalFractalVarU,
  kGlobalFractalVarS,
  kGlobalFractalVarI,
  kGlobalFractalVarC,
  kGlobalFractalVarA,

  // Transform filter (P5 scope step 2 of 3 -- DSP/TransformFilter.h).
  // Same one-shot-batch-on-the-UI-thread nature as the fractal
  // generator; reads the CURRENT active band series instead of a
  // formula. Collapsed to one pivot + one continuous exponent, which
  // covers all three of the doc's original ideas (warp/mirror-invert/
  // stretch) as a single power map -- see TransformFilter.h's own
  // header note for why. No "cheap filter" param of its own: it reuses
  // kGlobalFractalFilterType (see GenerateTransformBands in ExoEQ.cpp)
  // rather than duplicating a near-identical 4-option dropdown, since
  // both generators share the same "never Airwindows/character" policy.
  kGlobalTransformPivotHz,
  kGlobalTransformExponent,

  // Image-to-EQ-curve (P5 scope step 3 of 3, the last piece -- see
  // DSP/ImageToEq.h). Same one-shot-batch nature as the other two
  // generators; the image FILE PATH itself can't be a param (not
  // numeric), so it lives as mImageFilePath below, same reasoning as
  // mFractalFormula. These 5 are the numeric ImageToEqParams fields;
  // invertLuminance is a plain on/off toggle param (1.0 == true).
  kGlobalImageNodeCount,
  kGlobalImageMinFreqHz,
  kGlobalImageMaxFreqHz,
  kGlobalImageGainRangeDb,
  kGlobalImageResonance,
  kGlobalImageInvertLuminance,

  // P6's first piece (DSP/FftContainer.h): a GLOBAL post-band-sum stage,
  // not a per-band dropdown entry -- the user's explicit call (made
  // after being told the tradeoff: a per-band option capped like
  // Energy2 would let you run several FFT containers on different
  // bands at once, at a higher total CPU cost; one shared global stage
  // is cheaper because there's only ever one STFT running regardless of
  // how many bands exist, which is what "more optimized" meant here).
  // Runs once per channel, AFTER mChain[c].ProcessSample has already
  // summed every band for that channel -- see ProcessBlock's own
  // comment on exactly where. 3 params: On/off, and the brick-wall
  // passband's two edges (the container's own resonance/gain concepts
  // don't apply to a global stage, so only the passband is exposed).
  kGlobalFftEnabled,
  kGlobalFftLowCutHz,
  kGlobalFftHighCutHz,

  // Circle-GUI round 11: "switchable frequency calibration beyond Mel/
  // perceptual" off the original deferred list. A plain on/off -- Mel
  // (the confirmed default, true) vs. a standard log-frequency axis
  // (false) -- rather than an open-ended set of calibration curves,
  // since those are the only two this plugin's graph (CircleViewControl)
  // actually implements math for. Defaults to true (Mel) specifically so
  // every existing saved project's graph looks exactly like it already
  // did before this param existed -- a pure UI/display setting, read
  // only by CircleViewControl.h, never by any DSP path (the filters
  // themselves don't care what axis the GUI drew their knob position on).
  kGlobalFreqAxisMel,

  // M1 bypass: a plain on/off that passes the input straight to the output and
  // skips all DSP. Appended last so existing saved state keeps its indices.
  kGlobalBypass,

  // T4-8: Low Motion. Turns off the load sweep and any other UI motion. Appended last like Bypass.
  kGlobalLowMotion,

  kNumParams = kGlobalLowMotion + 1
};

constexpr int kNumFractalCustomVars = 9;

// Global control tabs (circle-GUI round 2): Groups/Mod Matrix/LFOs/
// Fractal/Transform/Image used to float in a fixed top-right stack that
// predates the circle graph -- now that the graph fills the top half of
// the window, that stack visually collided with it (confirmed from the
// user's own screenshot). Moved into a tab strip below the per-band
// detail panel instead (user's explicit choice: "Tabs below detail
// panel"). These are plain UI-organization indices, not params -- see
// ExoEQ.cpp's layout lambda for where each control gets tagged with one.
enum EGlobalTab
{
  kTabGroups = 0,
  kTabModMatrix,
  kTabLfos,
  kTabFractal,
  kTabTransform,
  kTabImage,
  kNumGlobalTabs
};

inline int BandParamIdx(int bandIndex, int offset)
{
  return kBandParamsStart + bandIndex * kNumParamsPerBand + offset;
}

inline int GroupParamIdx(int groupIndex, int offset)
{
  return kGlobalGroupParamsStart + groupIndex * kNumParamsPerGroup + offset;
}

inline int LfoParamIdx(int lfoIndex, int offset)
{
  return kGlobalLfoParamsStart + lfoIndex * kNumParamsPerLfo + offset;
}

inline int ModConnParamIdx(int connIndex, int offset)
{
  return kGlobalModConnParamsStart + connIndex * kNumParamsPerModConn + offset;
}

using namespace iplug;
using namespace igraphics;

class ExoEQ final : public Plugin
{
public:
  ExoEQ(const InstanceInfo& info);

#if IPLUG_DSP // http://bit.ly/2S64BDd
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnReset() override;
#endif

  // Overridden ONLY to also persist mFractalFormula (a plain string, not
  // an IParam -- see ExoEQ.h's note on kGlobalFractalFilterType above).
  // Calls the base class's own param (de)serialization first/last so
  // every existing automatable param still round-trips exactly as
  // before; this just appends/reads one extra length-prefixed string
  // after it. NOTE on exact signature: this is iPlug2's standard
  // "override to add custom state alongside normal params" pattern
  // (SerializeParams/UnserializeParams + IByteChunk::PutStr/GetStr) --
  // same can't-verify-against-the-real-SDK caveat as every other
  // UI/SDK-signature note in this project; if this doesn't compile
  // against your vendored headers, paste the error.
  // The circle graph is attached with kNoParameter, so iPlug2 never tells it
  // about param changes -- which is why turning a knob moved nothing on the
  // circle, while dragging a node updated the knobs fine. This hook fires on the
  // UI thread for every param change, from any source, and repaints the graph.
  void OnParamChangeUI(int paramIdx, EParamSource source) override;
  void UpdateBandFxBadge(); // T2-7
  void UpdateAbLabel();     // T2-9
  void OnUIClose() override;

  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

  // The fractal generator's "Generate" button calls this directly (see
  // ExoEQ.cpp's layout lambda) -- reads the current global fractal knobs
  // + mFractalFormula, runs DSP/FractalGenerator.h's one-shot math, and
  // writes the result into real band params. Lives on ExoEQ itself
  // (not a free function in ExoEQ.cpp like ReadBandSettings) because it
  // needs to call SetParameterValue, which only the plugin/delegate can do.
  void GenerateFractalBands();

  // Transform filter's "Transform" button calls this directly. Reads
  // every CURRENTLY ACTIVE band's cutoff/resonance/gain into a node
  // series, runs DSP/TransformFilter.h's pivot/exponent remap, and
  // writes the result into new, currently-INACTIVE band slots --
  // same non-destructive "never overwrite a band already in use"
  // policy GenerateFractalBands already follows, for the same reason.
  //
  // KNOWN STACKING BEHAVIOR, flagged rather than silently allowed to
  // surprise you: a transformed band is a real, ordinary active band
  // once written -- there is no "generator-made" vs. "hand-made" tag on
  // a band anywhere in this plugin. So clicking Transform a SECOND time
  // reads its own first output as part of the input series and
  // transforms that too, compounding. This is usable as a deliberate
  // "keep stretching further" workflow, but it will walk toward the
  // 128-band cap / FractalGenParams::kMaxNodes faster than it looks if
  // clicked repeatedly without deactivating the previous result first.
  // No tracking mechanism exists to distinguish "original" from
  // "generated" bands -- say if you want one added (it would need a new
  // per-band flag, not just a UI change).
  void GenerateTransformBands();

  // Image-to-EQ-curve's "Generate from Image" button calls this
  // directly. Reads mImageFilePath + the current global image params,
  // runs DSP/ImageToEq.h's decode-and-sample (stb_image, vendored under
  // Vendor/stb/ -- see that header's own note on STB_IMAGE_IMPLEMENTATION
  // being defined exactly once, in ExoEQ.cpp right before this file is
  // first included), and writes the result through the same shared
  // WriteGeneratedNodesIntoInactiveBands helper the other two generators
  // use -- same non-destructive policy, same stacking caveat applies
  // (see GenerateTransformBands's comment above; this generator can
  // compound the same way if run repeatedly). On failure (bad path,
  // corrupt/unsupported image), sets mLastImageError and does nothing
  // else -- no band is touched, no crash, no param change.
  void GenerateImageBands();

  // Shared by all three generators (factored out rather than duplicated,
  // now that there are three) -- writes a node series into the first N
  // currently-inactive band slots, forcing the given cheap filter-type
  // ordinal on each AND tagging kBandSource with `source` on every band
  // it touches. Single source of truth for the "never overwrite a
  // hand-built band" policy AND for the source-tagging policy, so all
  // three generators can never drift apart on either the way three
  // independent copies of this loop eventually would.
  void WriteGeneratedNodesIntoInactiveBands(const std::vector<exoeq::GeneratedNode>& nodes, int filterOrdinal, EBandSource source);

  // Maps the shared kGlobalFractalFilterType param (4 cheap-only
  // options) to its real FilterTypeId ordinal. Shared for the same
  // reason as the method above.
  int CheapFilterOrdinalFromParam() const;

  // Deactivates every band currently tagged with the given source --
  // the direct mitigation for the stacking risk flagged across all 3
  // generators' README notes: instead of hunting for which bands a
  // given generator made (by ear, or by remembering what you clicked),
  // one call clears exactly those and nothing else. Does NOT affect
  // kBandSource==HandBuilt bands regardless of `source` (there is no
  // "clear my hand-built bands" button here -- that's just deleting a
  // user's own work, a different and much more dangerous action this
  // doesn't attempt). The "Clear Generated" button in the UI calls this
  // once per generator source (Fractal, Transform, Image) rather than
  // once for "everything non-HandBuilt", so a user who only wants to
  // clear e.g. the image-generated bands and keep a fractal pass intact
  // still can.
  void ClearBandsBySource(EBandSource source);

private:
  // PDC (plugin delay compensation) hook -- the user flagged this
  // explicitly: ExoEQ must always keep the host truthfully informed of
  // its own algorithmic latency via SetLatency(), not just "sound right"
  // locally. Right now this computes to 0 for every feature that exists
  // (every filter type, send, Energy2, groups, mod matrix is either a
  // zero-latency IIR/biquad/one-pole structure or a non-delaying
  // per-sample operation -- DelaySend's own delay line is an INTENTIONAL
  // audible effect delay, not hidden algorithmic latency, so it correctly
  // does NOT get reported here any more than a tempo-synced delay plugin
  // reports its delay time as PDC). The reason this function exists at
  // all despite always returning 0 today: it's the single required hook
  // point for every future feature that DOES introduce real fixed-sample
  // latency -- true oversampling (anti-aliasing filter group delay) and
  // the roadmap's FFT-domain features (STFT/lookahead) are the two known
  // candidates on this project's own roadmap. The rule going forward:
  // any such feature adds its contribution here, in samples, and nowhere
  // else -- so SetLatency() is never called ad hoc from inside a feature,
  // avoiding exactly the "which of N call sites actually calls
  // SetLatency" drift bug this centralizes against.
  //
  // UPDATE: this is no longer a 0-contributor function. The global FFT
  // brick-wall stage (kGlobalFftEnabled) is the first real contributor
  // -- exoeq::FftBrickwallContainer::GetLatencySamples() (768, fixed
  // regardless of sample rate) whenever that stage is switched on, 0
  // otherwise. This is called every block (see ProcessBlock) specifically
  // so toggling the Enabled param mid-session reports the change
  // immediately, not just on the next OnReset/sample-rate change.
  int ComputeTotalLatencySamples() const;

  // Grows mFftScratch[c] to at least nFrames (never shrinks) -- the one
  // allocation path the global FFT stage needs, called from
  // ProcessBlock but written to only actually touch the heap the first
  // time (or the first time after a host increases its block size),
  // same "allocate lazily, once" discipline as everywhere else in this
  // plugin that avoids audio-thread allocation.
  void EnsureFftScratchCapacity(int nFrames);

  // Re-derives latency from current settings and pushes it to the host
  // ONLY when it actually changed (SetLatency() itself can be non-free on
  // some hosts -- they may flush/reset internal buffers on a latency
  // change, so calling it unconditionally every block once a real
  // latency source exists would be wasteful at best).
  void RecomputeAndReportLatency();
  int mLastReportedLatencySamples = -1;

  // One CircleChain per audio channel -- replaces the old flat
  // BandChain mChain[2] (DSP/CircleChain.h). Each CircleChain owns up to
  // kMaxCircles real BandChain instances, run in series, circle 0 first;
  // every existing project before this round lands with every band in
  // circle 0 (see kBandCircleIndex's own comment above), so this is a
  // behavior-preserving superset, not a rewrite of what already worked.
  exoeq::CircleChain mCircleChain[2];

  // Tracks whether each band was active as of the last block, so
  // ProcessBlock can skip calling SetBandSettings (which recomputes filter
  // coefficients -- a tan() call among others, not free) for bands that
  // are inactive AND were already inactive last block. Without this,
  // every one of the kMaxBands=128 bands would recalc its filter
  // coefficients every single block regardless of use, which is pure
  // waste on exactly the kind of weaker device this needs to run well on.
  bool mBandWasActive[kMaxBands] = {};

  // Phase 2 integration: ONE shared KeyTracker for the whole plugin (not
  // per-channel/per-band -- every band's keytrack amount reads the same
  // pitch). Latch defaults to TRUE and portamento to 0ms deliberately: see
  // ExoEQ.cpp for why that reproduces Phase 1's old mLastMidiNote behavior
  // exactly.
  exoeq::KeyTracker mKeyTracker;
  exoeq::MultiChannelKeyTracker mMultiChannelKeyTracker;

  // Real MTS-ESP client (DSP/MtsEspClient.h, vendored from
  // Vendor/MTS-ESP/libMTSClient.h/.cpp). Constructed unconditionally --
  // registering is always safe even with no MTS-ESP master software
  // installed anywhere on the machine (see that header's own note), so
  // there's no reason to make this optional/lazy. Handed to mKeyTracker
  // by pointer in the constructor; kGlobalMtsEspEnabled (read in
  // ProcessBlock) is the only thing that actually turns its effect on.
  exoeq::MtsEspClient mMtsEspClient;

  // Mod matrix (DSP/ModMatrix.h) wiring: ONE shared matrix + 4 LFOs for
  // the whole plugin (not per-channel/per-band), matching mKeyTracker's
  // own "one shared instance" precedent -- modulation is computed once
  // per band per block from BandSettings, upstream of the per-channel
  // BandChain::SetBandSettings calls, so both channels see identical
  // modulated settings (there's no per-channel modulation source anyway,
  // same as how cutoff/resonance/etc. are already channel-agnostic).
  exoeq::LFO mModLfos[kMaxModSources];
  exoeq::ModMatrix mModMatrix;

  // Fractal generator's inline-composer formula (DSP/FractalGenerator.h,
  // ExoEQ.h's own note above on why this is a plain string, not a
  // param). Empty by default -- the knob-driven path -- persisted via
  // SerializeState/UnserializeState above, not by the normal per-param
  // host automation/save mechanism.
  std::string mFractalFormula;

  // ROUND 27 -- forward-declared below (full definition only needed in
  // ExoEQ.cpp, which already includes CircleViewControl.h) so
  // SerializeState/UnserializeState can reach circleView's user-saved
  // presets (GetUserPresets/SetUserPresets) without pulling the whole
  // graph header into this one. Set once in OnLayout right after
  // circleView is constructed; null before the graph is first built
  // (e.g. a headless/DSP-only instantiation), checked before use.
  class CircleViewControl* mCircleView = nullptr;

  // Set during layout. Re-applies the per-filter-type knob names for whichever
  // band is currently in the panel. Called from OnParamChangeUI so the labels
  // follow a filter-type change, not just a new selection.
  std::function<void()> mRefreshKnobNames;

  // M1 A/B compare: two full-parameter snapshots. mAbActive is the slot being edited.
  // Not saved with the project (session-only).
  std::vector<double> mAbSlot[2];
  bool mAbHasSlot[2] = { false, false };
  int mAbActive = 0;
  iplug::igraphics::IControl* mAbButton = nullptr;
  iplug::igraphics::IControl* mBandFxButton = nullptr; // T2-7
  void SnapshotToSlot(int slot);
  void RecallSlot(int slot);
  void ToggleAB();

  // ROUND 27 -- most hosts restore plugin state (UnserializeState) BEFORE
  // the GUI is ever opened (OnLayout only runs the first time someone
  // actually opens the editor window), so mCircleView above is very
  // commonly still null exactly when UnserializeState needs to hand it
  // the restored user presets. This is where they land instead in that
  // case; OnLayout applies it to the freshly-built circleView the moment
  // one exists, then clears it so a later close/reopen of the editor
  // doesn't stomp presets you've saved/deleted since. A plain struct
  // copy, not CircleViewControl::PresetCircleDef directly, to keep this
  // header free of the CircleViewControl.h include (same reasoning as
  // the raw pointer above) -- ExoEQ.cpp converts between the two.
  struct PendingPresetDef { char name[32]; int bandCount; struct { int ordinal; double cutoffHz; double resonance; bool forceMorph50; } bands[3]; };
  std::vector<PendingPresetDef> mPendingUserPresets;
  bool mHasPendingUserPresets = false;

  // The 9 custom-variable knobs' (EXOMUSICA letters) user-set ranges -- each knob
  // itself (kGlobalFractalVarE..A, one per EXOMUSICA letter) is a real automatable 0..1 param, but
  // the range it's scaled into before a formula ever sees it is set via
  // inline text (same mechanism as mFractalFormula above, same reason:
  // a min/max pair isn't something you'd automate), so it lives here,
  // not as a param. Defaults to a plain 0..1 passthrough range for every
  // slot -- a user who never touches a variable's range gets exactly
  // the knob's own 0..1 position in the formula, same as if ranging had
  // never been added.
  double mFractalVarMin[kNumFractalCustomVars] = { 0., 0., 0., 0., 0., 0., 0., 0., 0. };
  double mFractalVarMax[kNumFractalCustomVars] = { 1., 1., 1., 1., 1., 1., 1., 1., 1. };

  // Image-to-EQ-curve's current source file. UNLIKE mFractalFormula,
  // this is deliberately TRANSIENT, NOT persisted by SerializeState/
  // UnserializeState (confirmed with the user): picking an image and
  // clicking Generate is meant to be a one-shot action whose only
  // lasting effect is the ordinary band params it writes -- those
  // already save/restore with the rest of the plugin's state (and so
  // with the DAW session's saved state, FXB included), with nothing
  // image-specific needed to get them back. Keeping this path around
  // forever would also mean carrying a non-portable absolute path in
  // every saved project for no benefit. Resets to empty on every reload
  // and is only ever used as in-session UI feedback (the status line
  // showing the current file name) -- GenerateImageBands() is a silent
  // no-op if this is empty.
  std::string mImageFilePath;

  // Last error message from a failed GenerateFromImageFile call (empty
  // when the last attempt succeeded, or none has been made yet) -- NOT
  // persisted (it's transient UI feedback, not configuration); surfaced
  // via a status text control, see ExoEQ.cpp's layout lambda.
  std::string mLastImageError;

  // The global FFT brick-wall stage (P6, first piece) -- ONE instance
  // per channel, running AFTER mChain[c] has already summed every band
  // for that channel (see ProcessBlock), never per-band. This is the
  // "global stage, not a per-band option" shape the user picked: only
  // ever one STFT running per channel regardless of how many bands
  // exist, rather than paying for a separate STFT per FFT-tagged band.
  // Not copyable (see FftContainer.h's own note on why), so this is an
  // array of 2 independently default-constructed instances, never
  // assigned or copied as a whole.
  exoeq::FftBrickwallContainer mFftContainer[2];

  // Per-channel scratch buffers for the float<->sample conversion the
  // FFT stage needs (FftBrickwallContainer::Process is float-only;
  // iPlug2's `sample` type is typically double) -- members rather than
  // block-local locals specifically to avoid a heap allocation every
  // single ProcessBlock call; only grows (via EnsureFftScratchCapacity
  // in ExoEQ.cpp) the first time a host hands this a larger block size
  // than it's seen before, same "don't allocate on the audio thread
  // every block" discipline the rest of this plugin already follows.
  std::vector<float> mFftScratch[2];

  // Tab-strip bookkeeping (circle-GUI round 2) -- every control belonging
  // to one of the 6 global tabs gets pushed into its slot here as it's
  // created in the layout lambda, so the tab-button click handlers can
  // iterate and Hide()/un-Hide() a whole tab's controls without depending
  // on any grouped-control API this project hasn't otherwise used.
  // IControl::Hide(bool) is a plain, certain, already-used-in-this-file
  // call (see TextEntryCallbackControl's constructor); this only
  // repurposes it per-tab. Cleared and refilled at the start of every
  // mLayoutFunc call -- editor teardown/recreation (e.g. a host closing
  // and reopening the UI) must not leave stale pointers here.
  std::vector<iplug::igraphics::IControl*> mTabControls[kNumGlobalTabs];
  int mSavedSelCircle = 0; // selection kept across a GUI close/reopen (see OnUIClose / OnLayout)
  int mSavedSelBand = -1;
  int mActiveTab = kTabGroups; // which global tab is showing; read by SwitchGlobalTab/showGlobalFx
  void SwitchGlobalTab(int tab);
};
