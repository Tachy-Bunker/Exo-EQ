#pragma once

#include "EnvelopeFollower.h"

// DSP/BandGroups.h
//
// RECREATED THIS ROUND -- not the original file. Your build error
// (BandChain.h(10,10): cannot open include file 'BandGroups.h') proved
// this header never actually reached your disk, despite the
// groups_modmatrix round's README saying its DSP was "already fully
// built and tested in an earlier session" -- that session's own delivery
// evidently never landed at F:\Work\Exomusica\Tools\ExoEQV1\Projects\
// ExoEQ\DSP\BandGroups.h, or landed somewhere else/got removed since.
// Flagging this plainly rather than pretending it's unchanged: I did not
// have the original file anywhere in this sandbox's history to copy from
// (confirmed by grep across every update zip ever produced here), so
// this is a from-scratch reconstruction built to EXACTLY match what every
// existing call site already expects and has been compiling against in
// spirit -- BandChain.h's own usage (mGroups[g].mute, .gainLinear,
// mGroupEnv[g].SetParams(settings.envelope)/.Process(...)) and ExoEQ.cpp's
// (GroupSettings gs; gs.mute = ...; gs.gainLinear = ...; gs.envelope =
// EnvelopeFollowerParams{};) are both exact, so this struct's shape is
// not a guess -- it's the only shape that satisfies both without
// changing either of those already-written files. If the ORIGINAL
// BandGroups.h had extra fields neither file ever reads, this
// reconstruction won't have them; say if you remember something this is
// missing.
//
// Design (matching BandChain.h's own point-by-point comments on exactly
// how a band's groupMask bitmask is meant to behave):
// 1. Multi-membership: a band's groupMask can have more than one bit set
//    -- it's a member of every group whose bit is set, simultaneously.
// 2. Accumulative gain / any-member mute: BandChain::ProcessSample
//    multiplies in EVERY member group's gainLinear in turn, and treats
//    ANY member group being muted as muting the whole band's
//    contribution -- both already implemented in BandChain.h; this file
//    only needs to supply the GroupSettings TYPE those lines read.
// 3. Shared per-group envelope: a band with groupEnvEnabled reads its
//    LOWEST-set member group's shared EnvelopeFollower output instead of
//    running its own per-band one (BandChain.h's Band::ProcessSample) --
//    saves CPU when many bands in one group all want the same
//    envelope-driven modulation rather than N independent followers.
// 4. Only a group a band actually needs gets its follower ticked
//    (BandChain::RecomputeGroupNeeded/mGroupNeeded) -- an unused group
//    costs nothing beyond 8 cheap bool checks per block.
namespace exoeq
{

constexpr int kNumGroups = 8;

struct GroupSettings
{
  bool mute = false;
  double gainLinear = 1.0;
  // The shared envelope follower's own attack/release/mode -- BandChain.h
  // currently constructs this as a plain default-constructed
  // EnvelopeFollowerParams{} every block (ExoEQ.cpp: "Envelope attack/
  // release/mode: fixed defaults, not exposed as params this pass"), so
  // whatever EnvelopeFollowerParams{}'s own defaults are is what every
  // group's shared envelope actually runs at today -- this struct just
  // needs to hold the value, not decide it.
  EnvelopeFollowerParams envelope;
};

} // namespace exoeq
