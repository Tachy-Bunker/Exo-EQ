#pragma once

#include "BandChain.h" // BandSettings
#include <vector>
#include <cmath>
#include <algorithm>

// DSP/ModMatrix.h
//
// RECREATED THIS ROUND, same situation as DSP/BandGroups.h and DSP/LFO.h's
// own header notes -- the original never actually reached your disk.
// Shape here is fixed by ExoEQ.cpp's existing, already-written usage:
//   - exoeq::ModMatrix::kMaxSources (used for the Mod Conn Src param's
//     range, 0..kMaxSources-1)
//   - exoeq::ModMatrix::Connection{ enabled, sourceIndex, destination,
//     depth } (ExoEQ.cpp builds these directly from 4 params)
//   - mModMatrix.SetSourceValue(i, mModLfos[i].Tick()) once per block
//   - ApplyModMatrixForBand's own calls: matrix.SetConnections(forThisBand);
//     BandSettings modulated; matrix.Apply(base, modulated);
// and by the Mod Conn Dest dropdown's exact option list/order in
// ExoEQ.cpp ("Cutoff (oct)", "Resonance +", "Morph +", "Gain (dB)",
// "Dist Drive +", "Freq Shift (Hz) +") -- ModDestination's enumerators
// below MUST stay in that order.
namespace exoeq
{

enum class ModDestination
{
  CutoffOct = 0,
  ResonancePlus,
  MorphPlus,
  GainDb,
  DistDrivePlus,
  FreqShiftHzPlus
};

class ModMatrix
{
public:
  static constexpr int kMaxSources = 8;

  struct Connection
  {
    bool enabled = false;
    int sourceIndex = 0;
    ModDestination destination = ModDestination::CutoffOct;
    double depth = 0.0; // same units as the destination's own knob range (ExoEQ.cpp: -8..8)
  };

  // Set once per LFO per block (ProcessBlock) -- plain storage, no
  // smoothing of its own (the LFO itself is already only ticked once per
  // block, see DSP/LFO.h's own comment on why that's an accepted
  // approximation, not a bug to fix here).
  void SetSourceValue(int sourceIndex, double value)
  {
    if (sourceIndex >= 0 && sourceIndex < kMaxSources)
      mSourceValues[sourceIndex] = value;
  }

  // Replaces the connection list this matrix applies on the next Apply()
  // call. ExoEQ.cpp calls this once PER BAND (with that band's own
  // already-filtered connection subset) immediately before calling
  // Apply() for that same band -- so this is a cheap vector assignment on
  // a short list (at most kMaxModConnections=8 entries), not a hot-path
  // per-sample operation.
  void SetConnections(const std::vector<Connection>& connections) { mConnections = connections; }

  // Accumulates every enabled connection's (sourceValue * depth) per
  // destination -- "accumulative," matching this project's own
  // established convention for multi-membership effects (node groups'
  // accumulative gain is the direct precedent) -- then applies each
  // destination's accumulated total ONCE, so two connections both
  // targeting Cutoff add their octave shifts together before the single
  // pow(2, ...) rather than compounding multiplicatively against each
  // other.
  void Apply(const BandSettings& base, BandSettings& outModulated) const
  {
    outModulated = base;

    double cutoffOctTotal = 0.0, resonanceTotal = 0.0, morphTotal = 0.0,
           gainDbTotal = 0.0, distDriveTotal = 0.0, freqShiftHzTotal = 0.0;

    for (const auto& c : mConnections)
    {
      if (!c.enabled) continue;
      if (c.sourceIndex < 0 || c.sourceIndex >= kMaxSources) continue;
      const double amount = mSourceValues[c.sourceIndex] * c.depth;
      switch (c.destination)
      {
        case ModDestination::CutoffOct:      cutoffOctTotal    += amount; break;
        case ModDestination::ResonancePlus:  resonanceTotal    += amount; break;
        case ModDestination::MorphPlus:      morphTotal        += amount; break;
        case ModDestination::GainDb:         gainDbTotal       += amount; break;
        case ModDestination::DistDrivePlus:  distDriveTotal    += amount; break;
        case ModDestination::FreqShiftHzPlus:freqShiftHzTotal  += amount; break;
      }
    }

    if (cutoffOctTotal != 0.0)
      outModulated.filter.cutoffHz = base.filter.cutoffHz * std::pow(2.0, cutoffOctTotal);
    if (resonanceTotal != 0.0)
      outModulated.filter.resonance = std::max(0.05, base.filter.resonance + resonanceTotal);
    if (morphTotal != 0.0)
      outModulated.filter.morph = std::clamp(base.filter.morph + morphTotal, 0.0, 100.0);
    if (gainDbTotal != 0.0)
      outModulated.gainLinear = base.gainLinear * std::pow(10.0, gainDbTotal / 20.0);
    if (distDriveTotal != 0.0)
      outModulated.distortionSend.drive = base.distortionSend.drive + distDriveTotal;
    if (freqShiftHzTotal != 0.0)
      outModulated.freqShiftSend.shiftHz = base.freqShiftSend.shiftHz + freqShiftHzTotal;
  }

private:
  double mSourceValues[kMaxSources] = {};
  std::vector<Connection> mConnections;
};

} // namespace exoeq
