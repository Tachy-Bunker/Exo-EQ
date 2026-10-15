#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include "Tuning.h"
#include "MtsEspClient.h"

namespace exoeq
{

// Stateless building block from Phase 1 -- kept exactly as-is (same
// signature, same math) so nothing already relying on it (tests,
// ExoEQ.cpp's current Phase-1-shaped manual/keytrack blend) changes
// behavior. KeyTracker below is the new, stateful Phase 2 layer on top.
inline double MidiNoteToHz(int midiNote, double a4Hz = 440.0)
{
  return a4Hz * std::pow(2.0, (midiNote - 69) / 12.0);
}

// Phase 2: latch + portamento + arbitrary tuning, as one small state
// machine a voice/band can own. This is "what note is keytracking
// actually pointing at right now", built on top of MidiNoteToHz/TuningTable
// rather than replacing them -- isolated here exactly like the spec asked
// for keytracking to be, so MTS-ESP can plug in later (as another
// NoteToHz-shaped source) without touching callers.
class KeyTracker
{
public:
  void SetSampleRate(double sampleRate) { mSampleRate = std::max(1.0, sampleRate); RecalcGlideCoeff(); }

  // nullptr = plain 12-TET via MidiNoteToHz. A non-null pointer must
  // outlive this KeyTracker (ownership stays with whoever loaded the
  // Scala file -- a KeyTracker doesn't manage tuning-table lifetime).
  void SetTuning(const TuningTable* tuning) { mTuning = tuning; }

  // Real MTS-ESP (DSP/MtsEspClient.h), the live protocol -- distinct
  // from SetTuning's static .scl/.tun file table. nullptr (the default)
  // = MTS-ESP never consulted, zero behavior change from before this
  // existed. A non-null pointer must outlive this KeyTracker, same
  // ownership rule as SetTuning.
  void SetMtsEspClient(const MtsEspClient* client) { mMtsEsp = client; }

  // Explicit per-instance opt-in, per the vendor API's own recommended
  // step 7: a plug-in that already has local tuning support (this one
  // does -- TuningTable) should let the LOCAL tuning override MTS-ESP
  // unless the user explicitly turns MTS-ESP on, or provide an option
  // to disable it outright. This bool IS that option. Default false:
  // SetMtsEspClient alone does nothing until this is also true.
  void SetUseMtsEsp(bool use) { mUseMtsEsp = use; }

  // True once MTS-ESP is both enabled AND actually connected to a
  // running master right now -- for a future "connected" UI indicator
  // (vendor API's own recommended step 9), and so callers can tell
  // whether MTS-ESP is silently falling back to local tuning this
  // instant (enabled but no master present) vs. truly inactive.
  bool MtsEspActive() const { return mUseMtsEsp && mMtsEsp && mMtsEsp->HasMaster(); }

  void SetLatch(bool latch) { mLatch = latch; }
  void SetPortamentoMs(double ms) { mPortamentoMs = std::max(0.0, ms); RecalcGlideCoeff(); }
  void SetFallbackNote(int midiNote) { mFallbackNote = std::clamp(midiNote, 0, 127); }

  void NoteOn(int midiNote)
  {
    // Last-note priority: re-pressing an already-held note just moves it
    // to the top of the stack instead of double-counting it.
    mHeldNotes.erase(std::remove(mHeldNotes.begin(), mHeldNotes.end(), midiNote), mHeldNotes.end());
    mHeldNotes.push_back(midiNote);
    mTargetHz = NoteToHz(midiNote);
  }

  void NoteOff(int midiNote)
  {
    mHeldNotes.erase(std::remove(mHeldNotes.begin(), mHeldNotes.end(), midiNote), mHeldNotes.end());
    if (!mHeldNotes.empty())
      mTargetHz = NoteToHz(mHeldNotes.back()); // fall back to whatever's still held
    else if (!mLatch)
      mTargetHz = NoteToHz(mFallbackNote); // no latch: nothing held -> back to the fallback pitch
    // latch + nothing held: mTargetHz deliberately stays at the last
    // played note -- that's the entire point of latch.
  }

  void Reset()
  {
    mHeldNotes.clear();
    mCurrentHz = mTargetHz = NoteToHz(mFallbackNote);
  }

  // Advances the glide by one sample and returns the current frequency.
  // One-pole exponential glide: exact portamento time, can't overshoot or
  // ring regardless of how the target jumps around, trivially cheap.
  //
  // When MTS-ESP is active, this ALSO re-queries the master every call
  // for whichever note is currently on top of the held stack -- the
  // vendor API's own recommended step 3 ("continuously query retuning
  // whilst a note is held... do this if you can and as often as
  // possible"), preferred here over note-on-only per their step 4's
  // own guidance when no separate user-facing choice is offered yet.
  // TuningTable/plain-12edo never need this (those don't change while
  // a note is held), so this extra query is skipped entirely unless
  // MTS-ESP is both enabled and actually connected -- zero added cost
  // for every user who isn't using MTS-ESP at all.
  double Tick()
  {
    if (MtsEspActive() && !mHeldNotes.empty())
      mTargetHz = mMtsEsp->NoteToFrequencyHz(mHeldNotes.back());

    mCurrentHz += (mTargetHz - mCurrentHz) * mGlideCoeff;
    return mCurrentHz;
  }

  double CurrentHz() const { return mCurrentHz; }
  double TargetHz() const { return mTargetHz; }
  bool AnyNoteHeld() const { return !mHeldNotes.empty(); }

private:
  // Precedence, per the vendor API's own recommended step 7: real
  // MTS-ESP (when enabled AND a master is actually connected right
  // now) wins over the local .scl TuningTable, which wins over plain
  // 12-TET. The moment MTS-ESP is enabled but NOT connected (no master
  // running), this falls straight through to the local tuning
  // automatically -- exactly the "let local tuning automatically
  // override MTS-ESP" half of that recommendation, satisfied by simply
  // checking HasMaster() every time rather than latching a decision.
  double NoteToHz(int midiNote) const
  {
    if (MtsEspActive())
      return mMtsEsp->NoteToFrequencyHz(midiNote);
    return mTuning ? mTuning->NoteToHz(midiNote) : MidiNoteToHz(midiNote);
  }

  void RecalcGlideCoeff()
  {
    if (mPortamentoMs <= 0.0) { mGlideCoeff = 1.0; return; } // instant jump, no glide
    const double tauSamples = (mPortamentoMs * 0.001) * mSampleRate;
    mGlideCoeff = 1.0 - std::exp(-1.0 / std::max(1.0, tauSamples));
  }

  const TuningTable* mTuning = nullptr;
  const MtsEspClient* mMtsEsp = nullptr;
  bool mUseMtsEsp = false;
  std::vector<int> mHeldNotes;
  bool mLatch = false;
  int mFallbackNote = 69; // A4, matches MidiNoteToHz's own default anchor
  double mPortamentoMs = 0.0;
  double mGlideCoeff = 1.0;
  double mSampleRate = 44100.0;
  double mCurrentHz = 440.0;
  double mTargetHz = 440.0;
};

} // namespace exoeq
