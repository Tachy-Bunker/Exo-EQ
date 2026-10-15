#include "DSP/ModMatrix.h"
#include <cstdio>
#include <cmath>
#include <vector>

// test_modmatrix_targeting.cpp
// ExoEQ.cpp's ApplyModMatrixForBand() filters ONE shared connection list
// (read once per block, not once per band) down to only the connections
// whose TargetBand param matches the band currently being processed,
// before calling ModMatrix::Apply -- since ModMatrix.h itself has no
// per-band concept (deliberately generic, see its own header comment).
// This re-implements that exact filter here, standalone, to confirm two
// bands reading the SAME shared connection list each only see their own
// connection's effect, and a band with no matching connection is passed
// through completely unmodulated (the common-case fast path).

using namespace exoeq;

namespace
{
  // Mirrors ExoEQ.cpp's ApplyModMatrixForBand exactly.
  BandSettings ApplyForBand(ModMatrix& matrix, const BandSettings& base,
                             const std::vector<ModMatrix::Connection>& allConnections,
                             int bandIndex, const int* targetBand)
  {
    std::vector<ModMatrix::Connection> forThisBand;
    for (size_t i = 0; i < allConnections.size(); ++i)
      if (targetBand[i] == bandIndex) forThisBand.push_back(allConnections[i]);
    if (forThisBand.empty()) return base;
    matrix.SetConnections(forThisBand);
    BandSettings modulated;
    matrix.Apply(base, modulated);
    return modulated;
  }
}

int main()
{
  int failures = 0;
  auto check = [&](bool cond, const char* label)
  {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", label);
    if (!cond) ++failures;
  };

  ModMatrix matrix;
  matrix.SetSourceValue(0, 1.0); // source 0 pegged at +1 for a predictable result

  BandSettings base;
  base.filter.cutoffHz = 1000.0;

  std::vector<ModMatrix::Connection> allConnections;
  int targetBand[2];

  ModMatrix::Connection connForBand5;
  connForBand5.sourceIndex = 0;
  connForBand5.destination = ModDestination::CutoffOctaves;
  connForBand5.depth = 1.0; // +1 octave at source=1.0
  connForBand5.enabled = true;
  allConnections.push_back(connForBand5);
  targetBand[0] = 5;

  ModMatrix::Connection connForBand9;
  connForBand9.sourceIndex = 0;
  connForBand9.destination = ModDestination::GainDb;
  connForBand9.depth = 6.0; // +6dB at source=1.0
  connForBand9.enabled = true;
  allConnections.push_back(connForBand9);
  targetBand[1] = 9;

  // Band 5 should get the cutoff-octave connection and nothing else.
  {
    const BandSettings out = ApplyForBand(matrix, base, allConnections, 5, targetBand);
    check(std::fabs(out.filter.cutoffHz - 2000.0) < 1.0e-6, "band 5 gets its own +1oct connection (1000Hz -> 2000Hz)");
    check(std::fabs(out.gainLinear - base.gainLinear) < 1.0e-9, "band 5 is NOT affected by band 9's gain connection");
  }

  // Band 9 should get the gain-dB connection and nothing else.
  {
    const BandSettings out = ApplyForBand(matrix, base, allConnections, 9, targetBand);
    const double expectedGain = base.gainLinear * std::pow(10.0, 6.0 / 20.0);
    check(std::fabs(out.gainLinear - expectedGain) < 1.0e-9, "band 9 gets its own +6dB connection");
    check(std::fabs(out.filter.cutoffHz - base.filter.cutoffHz) < 1.0e-9, "band 9 is NOT affected by band 5's cutoff connection");
  }

  // A band with no matching connection is untouched -- and, since the
  // fast path returns `base` directly rather than calling Apply, this
  // also confirms that path doesn't accidentally zero/reset anything.
  {
    const BandSettings out = ApplyForBand(matrix, base, allConnections, 42, targetBand);
    check(std::fabs(out.filter.cutoffHz - base.filter.cutoffHz) < 1.0e-9, "band 42 (no connections target it) is completely unmodulated");
  }

  // Disabling a connection removes its effect even though it's still in
  // the shared list and still targets this band.
  {
    std::vector<ModMatrix::Connection> withDisabled = allConnections;
    withDisabled[0].enabled = false;
    int t[2] = { 5, 9 };
    const BandSettings out = ApplyForBand(matrix, base, withDisabled, 5, t);
    check(std::fabs(out.filter.cutoffHz - base.filter.cutoffHz) < 1.0e-9, "a disabled connection has zero effect even when it targets this band");
  }

  std::printf("\n%d check(s) failed.\n", failures);
  return failures == 0 ? 0 : 1;
}
