#pragma once

#include "BandChain.h"
#include <array>
#include <unordered_map>

// DSP/CircleChain.h
//
// The "circles" architecture, confirmed with the user 2026-10-14 alongside
// the GUI redesign: a circle is a depth-slot that can hold any number of
// bands, bands inside ONE circle still sum in PARALLEL exactly like
// BandChain always has, but circles themselves now run in SERIES --
// circle 0's output feeds circle 1's input, circle 1 feeds circle 2, and
// so on. This is the DSP side of the GUI's "stream of sound trickles top
// to bottom" framing: the explicit answer to the open question flagged
// after the GUI mockup round was SERIES between circles (the user's pick),
// not a purely cosmetic reordering with everything secretly still parallel.
//
// WHY THIS IS A NEW FILE, NOT A BandChain.h CHANGE: BandChain itself is
// untouched -- a circle literally IS a BandChain (same parallel-sum-of-
// bands object this project already had, already tested). CircleChain is
// just an array of kMaxCircles of them, run one after another. This keeps
// every existing BandChain behavior (groups, Energy2 cap, sidechain,
// per-band sends/order) exactly as it already was, inside each circle.
//
// NO SEPARATE "ORDER" PARAM: a band's circle assignment (ExoEQ.h's new
// kBandCircleIndex) IS its series position -- circle 0 always runs first,
// circle 1 second, etc, by construction (mCircles[0] processes before
// mCircles[1] in the ProcessSample loop below, unconditionally). The GUI's
// "drag a circle's rail row to reorder it" therefore doesn't need a new
// DSP param at all: reordering two circles is just the GUI reassigning
// every affected band's kBandCircleIndex to swap which physical circle ID
// (0..kMaxCircles-1) currently holds which set of bands -- cheaper than a
// separate indirection table, and there's no way for a "circle order"
// array to drift out of sync with "which circle actually runs when" since
// there's only one source of truth (the circle ID itself).
//
// COST WHEN UNUSED: every existing project (one circle, circle 0, holding
// every band -- see ExoEQ.cpp's constructor) pays for exactly one circle's
// worth of series overhead: 255 further ProcessSample calls on EMPTY
// BandChains, each one skipped outright (see the `continue` below) rather
// than actually invoked -- not even an empty-loop no-op, a real branch
// around the call. Multi-circle series mode only costs real per-sample
// work in circles that actually hold a band.
//
// KNOWN TRADEOFF, FLAGGED RATHER THAN HIDDEN: BandChain's group-envelope
// followers (BandChain.h's mGroupEnv, one array of kNumGroups instances
// PER BandChain) are no longer shared plugin-wide the instant two circles
// both contain a member of the same group -- each circle's own BandChain
// now runs its own independent copy of that group's envelope follower,
// fed the same sidechain input. The VALUE each copy computes is identical
// (same input stream, same params, same deterministic filter math -- zero
// audible difference), so this is a CPU-duplication cost, not a
// correctness bug: only matters if you have many circles all sharing one
// active group's envelope at once, and even then it's one extra
// EnvelopeFollower::Process() call per redundant circle, not per band.
// Not fixed this round (would mean pulling group-envelope computation out
// of BandChain entirely into this class, real surgery on a file this
// round deliberately leaves untouched) -- revisit if it ever shows up on
// a profiler, same "good enough for v1, measure before optimizing further"
// call this project already made for Energy2's slot cap and group-needed
// recompute.
namespace exoeq
{

constexpr int kMaxCircles = 256;

class CircleChain
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = sampleRate;
    for (auto& circle : mCircles) circle.SetSampleRate(sampleRate);
  }

  void Reset()
  {
    for (auto& circle : mCircles) circle.Reset();
  }

  // Pushes `settings` for the plugin's flat band slot `bandIndex` into
  // whichever circle it currently belongs to, moving it there first if
  // `circleId` differs from where it was last time this was called for
  // that band. The move itself (RemoveBand + AddBand, which is what
  // actually costs anything beyond a plain settings update) only happens
  // on an actual circle-assignment CHANGE -- an untouched band's ordinary
  // per-block settings refresh is exactly as cheap as the flat
  // SetBandSettings call this replaces.
  void AssignBandToCircle(int bandIndex, int circleId, const BandSettings& settings)
  {
    if (circleId < 0) circleId = 0;
    if (circleId >= kMaxCircles) circleId = kMaxCircles - 1;

    auto it = mBandLocation.find(bandIndex);
    if (it != mBandLocation.end() && it->second.circleId == circleId)
    {
      mCircles[circleId].SetBandSettings(it->second.slot, settings);
      return;
    }

    if (it != mBandLocation.end())
      mCircles[it->second.circleId].RemoveBand(it->second.slot);

    const size_t newSlot = mCircles[circleId].AddBand(settings);
    mBandLocation[bandIndex] = { circleId, newSlot };
  }

  // Group settings are a plugin-wide bus concept, independent of which
  // circle a band happens to live in -- forwarded to every circle's own
  // BandChain so a band's groupMask behaves identically regardless of its
  // circle assignment (see this header's own top comment on the one
  // known cost of doing it this way: redundant, not incorrect).
  void SetGroupSettings(int groupIndex, const GroupSettings& settings)
  {
    for (auto& circle : mCircles) circle.SetGroupSettings(groupIndex, settings);
  }

  // How many bands circle `circleId` currently holds -- the GUI's rail
  // picker reads this directly for its band-count digit, and ProcessSample
  // below uses it to skip empty circles outright.
  size_t CircleBandCount(int circleId) const
  {
    if (circleId < 0 || circleId >= kMaxCircles) return 0;
    return mCircles[circleId].NumBands();
  }

  double ProcessSample(double input, double sidechainInput = 0.0)
  {
    double signal = input;
    for (int circleId = 0; circleId < kMaxCircles; ++circleId)
    {
      // Empty circle: skip the call entirely (not even an empty-band-loop
      // no-op) -- this is what keeps every existing single-circle project
      // from paying for 255 further circles it never created.
      if (mCircles[circleId].NumBands() == 0) continue;
      signal = mCircles[circleId].ProcessSample(signal, sidechainInput);
    }
    return signal;
  }

private:
  struct BandLocation { int circleId; size_t slot; };

  std::array<BandChain, kMaxCircles> mCircles;
  // bandIndex -> where it currently lives. A plain hash map rather than a
  // kMaxBands-sized fixed array deliberately: this header doesn't need to
  // know ExoEQ.h's kMaxBands constant at all this way (no circular
  // include, no "these two constants must match" footgun), and the map
  // only ever holds as many entries as there are bands that have EVER
  // been assigned a circle at least once -- cheap either way at these
  // counts (up to a few hundred).
  std::unordered_map<int, BandLocation> mBandLocation;
  double mSampleRate = 44100.0;
};

} // namespace exoeq
