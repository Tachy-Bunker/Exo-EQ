#pragma once

#include "FilterCore.h"
#include "FilterTypes.h"
#include "AirwindowsFilters.h"
#include "DelaySend.h"
#include "DistortionSend.h"
#include "FrequencyShiftSend.h"
#include "EnvelopeFollower.h"
#include "BandGroups.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <array>
#include <utility>
#include <cmath>

// BandChain.h
// The Phase 3 abstraction: a set of bands running in PARALLEL (not series),
// summed at the output. This is the architecture decision from the spec doc
// (mrugalla's parallel-bandpass-vs-series-bells tradeoff): parallel bands
// avoid the gain-compensation "floor" problem series-bells has, and keep
// each band's sends/sidechain isolated to its own output before the sum.
//
// "Order of effect" lives WITHIN a band (its own effect chain), not across
// bands — this class only owns the per-band filter + gain + Delta-solo logic.
// Per-band sends (frequency shift, distortion, delay) and sidechain input
// plug in later as additional per-band effect stages; this is the scaffold
// they attach to.

namespace exoeq
{

// Energy2 (AirwindowsFilters.h's AirwindowsEnergy2Processor) is the one
// per-band feature in this project that is NOT allowed to scale with
// kMaxBands=128 the way everything else does -- the user explicitly
// asked for a hard global cap ("let's say the user can only have 20
// Energy2 instances maximum"), since most projects wouldn't use more
// than that and it keeps a worst-case project (128 bands all wanting
// Energy2) from silently costing 128x this stage's CPU. Enforced in
// BandChain::RecomputeEnergy2Slots, first-come-first-served by band
// index -- see that function's comment for exactly how ties/overflow
// are handled.
constexpr int kMaxEnergy2Instances = 20;

struct BandSettings
{
  FilterParams filter;
  double gainLinear = 1.0;
  bool mute = false;
  bool solo = false; // "Delta" — isolate this band's own output
  bool delaySendEnabled = false;
  DelaySendParams delaySend;
  bool distortionSendEnabled = false;
  DistortionSendParams distortionSend;
  bool freqShiftSendEnabled = false;
  FrequencyShiftSendParams freqShiftSend;

  // Energy2 (see AirwindowsFilters.h's AirwindowsEnergy2Processor header
  // note): NOT a filter-type dropdown entry -- a separate send-like
  // stage, same shape as the three sends above, EXCEPT it's globally
  // capped at kMaxEnergy2Instances (see ExoEQ.h) across the whole
  // plugin, not per-band-unlimited like everything else here. Wanting
  // it enabled costs nothing to STORE (these are plain doubles/bools
  // like any other per-band setting); whether it actually RUNS depends
  // on whether BandChain granted this band a live slot out of the
  // capped pool -- see BandChain::RecomputeEnergy2Slots. A band that
  // wants Energy2 but didn't get a slot (cap already full) silently
  // gets no Energy2 processing, same "silently inert past the cap"
  // behavior as every other capped resource in this project.
  bool energy2Enabled = false;
  double energy2GlitterHiss = 0.5;   // 0..1, 0.5 = neutral (off), same convention as the source's own per-band knobs
  double energy2RatScrapeFizz = 0.5; // 0..1, 0.5 = neutral (off)
  double energy2Mix = 0.0;           // -1..1 bipolar, 0 = no effect

  // Sidechain: modulates THIS band's cutoff from an external envelope.
  // Not wired to a real second audio input bus yet (that needs a
  // config.h/CMakeLists.txt change -- a host-I/O decision, not a DSP one);
  // BandChain::ProcessSample's sidechain argument defaults to 0.0 so every
  // existing single-argument call site is unaffected either way.
  bool sidechainEnabled = false;
  double sidechainModOctaves = 0.0; // cutoff *= 2^(sidechainModOctaves * envelope); +open-up, -duck
  EnvelopeFollowerParams sidechainEnv;

  // Reorderable per-node effect order (spec: "order of effect" as a
  // property of each node, not a fixed global chain). Each send gets a
  // SLOT (0, 1, or 2) instead of a hand-picked set of orderings, so any
  // of the 6 permutations is just three numbers rather than a 6-way enum
  // the UI has to special-case. Defaults (0/1/2) reproduce the exact
  // fixed order this already shipped with (freq-shift, distortion,
  // delay) -- existing projects/automation see no change until a user
  // deliberately moves something. Ties (two sends sharing a slot) are
  // broken by this same freqShift-before-distortion-before-delay
  // priority, so a half-edited project never becomes ambiguous.
  int freqShiftSlot = 0;
  int distortionSlot = 1;
  int delaySlot = 2;

  // Node groups (see BandGroups.h for the full design rationale).
  // groupMask bit g set => this band is a member of group g; defaults to
  // 0 (no groups), so an untouched band is completely unaffected by any
  // group settings, same zero-cost-when-unused rule as everything else
  // in this struct.
  unsigned int groupMask = 0;
  // Opts this band's cutoff modulation into its lowest-set member
  // group's SHARED envelope instead of running its own per-band
  // EnvelopeFollower (sidechainEnabled/sidechainEnv above). Mutually
  // exclusive in practice -- if both are set, groupEnvEnabled wins (see
  // Band::ProcessSample) since that's the one actually saving CPU across
  // multiple bands.
  bool groupEnvEnabled = false;
  double groupEnvModOctaves = 0.0; // same semantics as sidechainModOctaves
};

class Band
{
public:
  void SetSampleRate(double sampleRate)
  {
    mFilter.SetSampleRate(sampleRate);
    mDelaySend.SetSampleRate(sampleRate);
    mDistortionSend.SetSampleRate(sampleRate);
    mFreqShiftSend.SetSampleRate(sampleRate);
    mSidechainEnv.SetSampleRate(sampleRate);
    mEnergy2.SetSampleRate(sampleRate);
  }

  void SetSettings(const BandSettings& s)
  {
    mSettings = s;
    mBaseFilterParams = s.filter; // kept unmodulated; sidechain modulation is applied on top of this, per-sample
    mFilter.SetParams(s.filter);
    mDelaySend.SetParams(s.delaySend);
    mDistortionSend.SetParams(s.distortionSend);
    mFreqShiftSend.SetParams(s.freqShiftSend);
    mSidechainEnv.SetParams(s.sidechainEnv);
    // Cheap to set regardless of whether this band actually holds a
    // live Energy2 slot right now (see mEnergy2SlotGranted) -- SetParams
    // here is just a few clamps/pow() calls, not the capped resource
    // itself, so there's no reason to gate it on the slot too.
    mEnergy2.SetParams(s.energy2GlitterHiss, s.energy2RatScrapeFizz, s.energy2Mix);
  }

  const BandSettings& Settings() const { return mSettings; }

  // Set by BandChain::RecomputeEnergy2Slots, which is the single place
  // that enforces the global kMaxEnergy2Instances cap across every band
  // in this chain -- a Band never decides for itself whether it's
  // allowed to run Energy2, it's just told the answer.
  void SetEnergy2SlotGranted(bool granted) { mEnergy2SlotGranted = granted; }
  bool WantsEnergy2Slot() const { return mSettings.energy2Enabled && !mSettings.mute; }
  bool HasEnergy2Slot() const { return mEnergy2SlotGranted; }

  void Reset()
  {
    mFilter.Reset();
    mDelaySend.Reset();
    mDistortionSend.Reset();
    mFreqShiftSend.Reset();
    mSidechainEnv.Reset();
    mEnergy2.Reset();
  }

  // Backward-compatible single-argument form -- every existing caller
  // (BandChain's own default-argument overload, every prior test) gets
  // exactly the old behavior: the sidechain branch below is skipped
  // entirely whenever a band's sidechainEnabled is false (the default),
  // so this costs nothing when the feature isn't in use.
  double ProcessSample(double input) { return ProcessSample(input, 0.0); }
  double ProcessSample(double input, double sidechainInput) { return ProcessSample(input, sidechainInput, 0.0); }

  // `groupEnvValue`: this band's lowest-set member group's CURRENT shared
  // envelope value, already computed by BandChain for this sample (see
  // BandChain::ProcessSample) -- a plain double, not a pointer/reference
  // to the follower itself, so a Band stays ignorant of which group (if
  // any) it belongs to; BandChain owns that bookkeeping entirely.
  double ProcessSample(double input, double sidechainInput, double groupEnvValue)
  {
    if (mSettings.mute)
      return 0.0;

    if (mSettings.groupEnvEnabled)
    {
      // Shared-group-envelope path: skips this band's OWN EnvelopeFollower
      // entirely (that's the whole point -- see BandGroups.h point 3).
      FilterParams modulated = mBaseFilterParams;
      modulated.cutoffHz = mBaseFilterParams.cutoffHz * std::pow(2.0, mSettings.groupEnvModOctaves * groupEnvValue);
      mFilter.SetParams(modulated);
    }
    else if (mSettings.sidechainEnabled)
    {
      const double env = mSidechainEnv.Process(sidechainInput);
      // NOTE on cost: this recomputes the filter's coefficients (a tan()
      // call among others) every single sample while a band's sidechain
      // is active -- inherent to any real-time envelope-modulated
      // resonant filter (an auto-wah/envelope-filter IS this cost; it's
      // not something to optimize away without changing what the effect
      // does). Completely avoided, zero extra cost, whenever
      // sidechainEnabled is false -- same zero-cost-when-disabled
      // philosophy as the three sends above.
      FilterParams modulated = mBaseFilterParams;
      modulated.cutoffHz = mBaseFilterParams.cutoffHz * std::pow(2.0, mSettings.sidechainModOctaves * env);
      mFilter.SetParams(modulated);
    }

    double out = mFilter.ProcessSample(input);

    // Energy2 runs right after the filter, before the reorderable sends
    // -- it's a harmonic/energy character stage on this band's filtered
    // tone, same conceptual spot as the filter itself, not a per-node
    // send the user reorders (the user never asked for it to be
    // reorderable, and giving it a 4th slot would mean redesigning the
    // slot scheme for a feature that's capped globally anyway). Gated
    // on BOTH energy2Enabled AND mEnergy2SlotGranted -- the latter is
    // false whenever this band wanted a slot but the global cap was
    // already full, so it's a true no-op (not even ProcessSample is
    // called) rather than running silently muted.
    if (mSettings.energy2Enabled && mEnergy2SlotGranted)
      out = mEnergy2.ProcessSample(out);

    // Sends run on the band's OWN filtered output, after the filter and
    // before the band gain — per the spec, a per-node send is isolated to
    // that band, not a global effect, and gain should scale the sent signal
    // too (otherwise Delta-soloing a quiet band would still blast the send).
    //
    // Order within the band is now USER-SET per node (freqShiftSlot/
    // distortionSlot/delaySlot in BandSettings), not fixed. Built as a
    // tiny 3-element array sorted by slot every sample -- 3 elements is
    // cheap enough (a handful of comparisons) that this isn't worth
    // caching/recomputing only on settings-change, unlike the filter
    // coefficient recalc elsewhere in this file. kEffectId order
    // (FreqShift=0, Distortion=1, Delay=2) is the tiebreak when two
    // sends share a slot, matching this chain's original fixed default
    // so an untouched project's effect order is bit-for-bit unchanged.
    enum EffectId { kFreqShift = 0, kDistortion = 1, kDelay = 2 };
    std::array<std::pair<int, EffectId>, 3> order = {
      std::make_pair(mSettings.freqShiftSlot, kFreqShift),
      std::make_pair(mSettings.distortionSlot, kDistortion),
      std::make_pair(mSettings.delaySlot, kDelay)
    };
    std::stable_sort(order.begin(), order.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [slot, effect] : order)
    {
      (void)slot;
      switch (effect)
      {
        case kFreqShift:
          if (mSettings.freqShiftSendEnabled) out = mFreqShiftSend.ProcessSample(out);
          break;
        case kDistortion:
          if (mSettings.distortionSendEnabled) out = mDistortionSend.ProcessSample(out);
          break;
        case kDelay:
          if (mSettings.delaySendEnabled) out = mDelaySend.ProcessSample(out);
          break;
      }
    }
    return out * mSettings.gainLinear;
  }

private:
  MultiPoleFilter mFilter; // dispatches to whichever FilterTypeId this band's FilterParams.type selects (FilterTypes.h); MorphZ reproduces the original single-SVF behavior exactly
  DelaySend mDelaySend;
  DistortionSend mDistortionSend;
  FrequencyShiftSend mFreqShiftSend;
  EnvelopeFollower mSidechainEnv;
  AirwindowsEnergy2Processor mEnergy2;
  bool mEnergy2SlotGranted = false;
  FilterParams mBaseFilterParams;
  BandSettings mSettings;
};

class BandChain
{
public:
  BandChain()
  {
    for (auto& e : mGroupEnv) e.SetSampleRate(mSampleRate);
  }

  size_t AddBand(const BandSettings& settings)
  {
    mBands.push_back(std::make_unique<Band>());
    mBands.back()->SetSampleRate(mSampleRate);
    mBands.back()->SetSettings(settings);
    RecomputeGroupNeeded();
    RecomputeEnergy2Slots();
    return mBands.size() - 1;
  }

  void RemoveBand(size_t index)
  {
    if (index < mBands.size())
      mBands.erase(mBands.begin() + static_cast<long>(index));
    RecomputeGroupNeeded();
    RecomputeEnergy2Slots();
  }

  void SetSampleRate(double sampleRate)
  {
    mSampleRate = sampleRate;
    for (auto& b : mBands) b->SetSampleRate(sampleRate);
    for (auto& e : mGroupEnv) e.SetSampleRate(sampleRate);
  }

  void SetBandSettings(size_t index, const BandSettings& settings)
  {
    if (index < mBands.size())
      mBands[index]->SetSettings(settings);
    RecomputeGroupNeeded();
    RecomputeEnergy2Slots();
  }

  // Returns how many bands currently hold a live, GRANTED Energy2 slot
  // (0..kMaxEnergy2Instances) -- for UI/metering ("18/20 Energy2 slots
  // in use"), not used internally by anything else here. Deliberately
  // counts grants, not raw demand (WantsEnergy2Slot()) -- demand can
  // exceed the cap, but slots in use never can.
  int Energy2SlotsInUse() const
  {
    int n = 0;
    for (auto& b : mBands) if (b->HasEnergy2Slot()) ++n;
    return n;
  }

  // Group settings are set by chain index (0..kNumGroups-1), independent
  // of any band -- a group exists (and can be configured) whether or not
  // any band currently belongs to it.
  void SetGroupSettings(int groupIndex, const GroupSettings& settings)
  {
    if (groupIndex < 0 || groupIndex >= kNumGroups) return;
    mGroups[groupIndex] = settings;
    mGroupEnv[groupIndex].SetParams(settings.envelope);
  }

  const GroupSettings& GetGroupSettings(int groupIndex) const { return mGroups[groupIndex]; }

  void Reset()
  {
    for (auto& b : mBands) b->Reset();
    for (auto& e : mGroupEnv) e.Reset();
  }

  size_t NumBands() const { return mBands.size(); }

  // Any soloed ("Delta") band mutes every non-soloed band for this call,
  // matching "isolate this band's output" without needing per-band state
  // changes — solo is read, not written, by the mix logic.
  //
  // `sidechainInput` defaults to 0.0 so every existing single-argument
  // call site (every prior test, ExoEQ.cpp's current main-input-only
  // calls) compiles and behaves exactly as before -- it's dead/unused
  // unless a band's own sidechainEnabled is set, in which case it's fed
  // to that band's envelope follower. All bands in a chain currently
  // share the one sidechain input signal; per-band sidechain SOURCES
  // (e.g. a different band listening to a different input) would be a
  // later extension, not needed for a first real sidechain feature.
  double ProcessSample(double input, double sidechainInput = 0.0)
  {
    // Advance only the group envelopes something actually reads this
    // block (see BandGroups.h point 4) -- an unused group's follower
    // never gets Process() called, so it costs nothing.
    double groupEnvValue[kNumGroups] = {};
    for (int g = 0; g < kNumGroups; ++g)
      if (mGroupNeeded[g])
        groupEnvValue[g] = mGroupEnv[g].Process(sidechainInput);

    const bool anySolo = std::any_of(mBands.begin(), mBands.end(),
                                      [](const auto& b) { return b->Settings().solo; });
    double sum = 0.0;
    for (auto& b : mBands)
    {
      const BandSettings& s = b->Settings();
      if (anySolo && !s.solo)
        continue;

      const double envForThisBand = s.groupEnvEnabled ? groupEnvValue[LowestSetGroupBit(s.groupMask)] : 0.0;
      double out = b->ProcessSample(input, sidechainInput, envForThisBand);

      // Accumulative group gain/mute (see BandGroups.h point 2): every
      // member group's gain multiplies in, and ANY member group being
      // muted silences the band's contribution outright.
      if (s.groupMask != 0)
      {
        for (int g = 0; g < kNumGroups; ++g)
        {
          if (!(s.groupMask & (1u << g))) continue;
          if (mGroups[g].mute) { out = 0.0; break; }
          out *= mGroups[g].gainLinear;
        }
      }

      sum += out;
    }
    return sum;
  }

private:
  // Index of the lowest set bit in `mask`, or 0 if mask is 0 (a band with
  // groupEnvEnabled but no group membership reads group 0's envelope,
  // which will simply read 0.0 since group 0 can't be "needed" by a
  // member-less band -- harmless, not a real-world case the UI should
  // even allow, but defined rather than undefined).
  static int LowestSetGroupBit(unsigned int mask)
  {
    if (mask == 0) return 0;
    int g = 0;
    while (!(mask & (1u << g)) && g < kNumGroups - 1) ++g;
    return g;
  }

  // Recomputed in full on every AddBand/RemoveBand/SetBandSettings call
  // (O(numBands), which only happens at block rate at most -- see
  // ExoEQ.cpp's ProcessBlock -- never per-sample) rather than
  // incrementally tracked, since incrementally maintaining a multi-band
  // reference count per group correctly across membership CHANGES (not
  // just add/remove) is real bookkeeping complexity for a cost that's
  // already negligible at block rate.
  void RecomputeGroupNeeded()
  {
    for (int g = 0; g < kNumGroups; ++g) mGroupNeeded[g] = false;
    for (auto& b : mBands)
    {
      const BandSettings& s = b->Settings();
      if (!s.groupEnvEnabled || s.groupMask == 0) continue;
      mGroupNeeded[LowestSetGroupBit(s.groupMask)] = true;
    }
  }

  // Enforces the global kMaxEnergy2Instances cap across THIS chain.
  // First-come-first-served by band index (lower band index wins a slot
  // over a higher one when both want it and the cap is tight) -- simple,
  // deterministic, and matches how every other "first N win" resource
  // in audio software behaves; a priority/stealing scheme (e.g. "newest
  // wins") was considered and rejected because it would mean a band's
  // Energy2 can silently stop working the moment a LATER band enables
  // it, which is a much more confusing failure mode for a user working
  // bottom-up through their bands than "the last few bands I added
  // don't get a slot until I free one up".
  //
  // NOTE on the two-channel plugin shell: ExoEQ owns one BandChain per
  // audio channel (mChain[2] in ExoEQ.h), each calling this
  // independently with THE SAME band settings (same energy2Enabled
  // etc. mirrored to both channels by ExoEQ::ProcessBlock) -- so a
  // given band index gets the identical grant/deny decision on both
  // chains since both see the same WantsEnergy2Slot() answers in the
  // same order. The cap is semantically "20 ENABLED BANDS", not "20 raw
  // Energy2Processor objects" (which would silently mean only 10 bands
  // on a stereo signal) -- this function is what makes that true.
  void RecomputeEnergy2Slots()
  {
    int granted = 0;
    for (auto& b : mBands)
    {
      const bool wants = b->WantsEnergy2Slot();
      const bool grant = wants && (granted < kMaxEnergy2Instances);
      b->SetEnergy2SlotGranted(grant);
      if (grant) ++granted;
    }
  }

  std::vector<std::unique_ptr<Band>> mBands;
  double mSampleRate = 44100.0;
  std::array<GroupSettings, kNumGroups> mGroups;
  std::array<EnvelopeFollower, kNumGroups> mGroupEnv;
  bool mGroupNeeded[kNumGroups] = {};
};

} // namespace exoeq
