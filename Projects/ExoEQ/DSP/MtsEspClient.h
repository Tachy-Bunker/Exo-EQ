#pragma once

#include "../Vendor/MTS-ESP/libMTSClient.h"
#include <algorithm>
#include <cmath>

namespace exoeq
{

// MtsEspClient.h
// Thin RAII wrapper around ODDSound's real MTS-ESP client API
// (Vendor/MTS-ESP/libMTSClient.h/.cpp -- vendored verbatim from the
// source the user provided, MIT-style permissive license per that
// file's own header). This is the REAL protocol, not a .scl/.tun file
// parser (that's DSP/Tuning.h's TuningTable, already shipped) -- it
// talks to a running MTS-ESP master plug-in (or hardware controller)
// elsewhere in the DAW over shared memory/IPC, live, per-note.
//
// Isolation note (matches the project's own architecture decision):
// KeyTracker is the one place that decides HOW a MIDI note becomes a
// frequency -- this class is a dumb, narrow wrapper around the vendor
// API and does not know about KeyTracker, latch, portamento, or
// TuningTable at all. KeyTracker.h is what wires this in as an
// alternative/overriding tuning source, same seam the spec called for
// before TuningTable existed.
//
// How the vendored client actually works (read the real libMTSClient.cpp,
// not guessed): MTS_RegisterClient() does NOT link against a static
// library -- it dlopen()s/LoadLibrary()s a SHARED library that an
// MTS-ESP master installs system-wide at a fixed OS path (Windows:
// %COMMONPROGRAMFILES%\MTS-ESP\LIBMTS.dll, Mac:
// /Library/Application Support/MTS-ESP/libMTS.dylib, Linux:
// /usr/local/lib/libMTS.so or /usr/lib/libMTS.so). If that shared
// library isn't present on the machine (no MTS-ESP master software
// ever installed), every function pointer inside it stays null and
// MTS_HasMaster() simply returns false -- there is no crash, no
// exception, no special-case needed here; "not connected" is the
// default, always-safe state. This is exactly why it's safe to call
// MTS_RegisterClient() unconditionally in the plugin constructor
// regardless of whether the end user has MTS-ESP installed at all.
class MtsEspClient
{
public:
  MtsEspClient() { mClient = MTS_RegisterClient(); }
  ~MtsEspClient() { if (mClient) MTS_DeregisterClient(mClient); }

  MtsEspClient(const MtsEspClient&) = delete;
  MtsEspClient& operator=(const MtsEspClient&) = delete;

  // True only when an MTS-ESP master is actually present AND connected
  // right now -- this is what KeyTracker checks before trusting
  // anything else below, so a machine with no MTS-ESP master installed
  // (the common case) always falls through to the existing
  // TuningTable/plain-12edo path with zero behavior change.
  bool HasMaster() const { return mClient && MTS_HasMaster(mClient); }

  // midiChannel: 0-15, or -1 if unknown/not using multi-channel tables
  // -- per the vendor API's own recommendation, NOT supplied (-1) if
  // the plugin's MPE mode is enabled (ExoEQ doesn't have an MPE mode
  // yet, so this is always explicit 0-15 or -1 for now).
  double NoteToFrequencyHz(int midiNote, int midiChannel = -1) const
  {
    if (!mClient) return FallbackHz(midiNote);
    const char note = static_cast<char>(std::clamp(midiNote, 0, 127));
    const signed char chan = static_cast<signed char>(std::clamp(midiChannel, -1, 15));
    const double hz = MTS_NoteToFrequency(mClient, note, chan);
    return (hz > 0.0 && std::isfinite(hz)) ? hz : FallbackHz(midiNote);
  }

  // Per the vendor API's own Scala .kbm support: a note can be
  // explicitly UNMAPPED by a master's keyboard mapping (no frequency
  // assigned at all) -- callers should skip sounding the note entirely
  // when this is true, same as the vendor's own recommended step 5.
  bool ShouldFilterNote(int midiNote, int midiChannel = -1) const
  {
    if (!mClient) return false;
    const char note = static_cast<char>(std::clamp(midiNote, 0, 127));
    const signed char chan = static_cast<signed char>(std::clamp(midiChannel, -1, 15));
    return MTS_ShouldFilterNote(mClient, note, chan);
  }

  const char* ScaleName() const
  {
    if (!mClient) return "";
    const char* name = MTS_GetScaleName(mClient);
    return name ? name : "";
  }

private:
  // 12-TET-at-A440 fallback, used only if MTS_RegisterClient() itself
  // somehow returned null (never observed in the vendor source --
  // registration always succeeds locally regardless of master
  // presence -- but a defensive fallback costs nothing and keeps this
  // class never-crashing under any condition).
  static double FallbackHz(int midiNote)
  {
    return 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
  }

  MTSClient* mClient = nullptr;
};

} // namespace exoeq
