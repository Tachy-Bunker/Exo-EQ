#pragma once

#include "FilterCore.h"
#include "StateVariableFilter.h"
#include <complex>
#include <cmath>
#include <vector>

// FrequencyResponse.h
// Math for the planned spectrum-graph UI (draggable band nodes over a
// response curve, Pro-Q style). Deliberately separated from any drawing
// code: this is pure DSP analysis, fully testable without iGraphics or
// the real iPlug2 SDK, which the actual curve-rendering control will not
// be (same limitation as every other "can't compile-test this without
// the vendored SDK" caveat so far).
//
// Returns a genuine COMPLEX gain (magnitude AND phase), not just a dB
// magnitude number, specifically so multiple parallel bands' responses
// can be summed correctly. Bands here run in parallel, not in series --
// summing their magnitudes (or dB values) directly is what most simple
// EQ-curve implementations do, and it's wrong whenever bands overlap:
// two bands' responses can partially cancel or reinforce depending on
// their relative phase at a given frequency, and only summing the actual
// complex gains captures that. Getting this wrong would show a visibly
// inaccurate curve on exactly the kind of overlapping/resonant bands this
// plugin is built around.
//
// Implementation: reuses the REAL StateVariableFilter class (zero risk of
// the analysis formula drifting from what the audio engine actually
// does), driven with a real cosine at the target frequency, and measures
// the steady-state complex gain via a single-frequency correlation
// (a Goertzel-style single-bin DFT) of the output against the SAME
// windowed input -- taking the ratio of output-correlation to
// input-correlation cancels out windowing/leakage error as long as the
// measurement window is long relative to the filter's own settle time,
// rather than needing a hand-derived closed-form transfer function that
// could subtly mismatch the actual difference equations.

namespace exoeq
{

// Complex gain of a single band's filter at `freqHz`, at the given
// sample rate -- H(e^{j*2*pi*freqHz/sampleRate}) for THIS implementation's
// actual difference equations, not an idealized textbook SVF.
inline std::complex<double> ComplexFilterResponse(const FilterParams& params, double sampleRate, double freqHz)
{
  StateVariableFilter filter;
  filter.SetSampleRate(sampleRate);
  filter.SetParams(params);

  const double omega = 2.0 * 3.14159265358979323846 * freqHz / sampleRate;

  // Settle long enough for the filter's transient to decay -- scales with
  // resonance (a high-Q band rings longer) and with how low the cutoff is
  // relative to the sample rate (slower filters settle slower in samples).
  const int settleSamples = std::clamp(static_cast<int>(params.resonance * 400.0 + sampleRate / std::max(20.0, params.cutoffHz) * 20.0), 500, 20000);
  const int windowSamples = std::max(2000, settleSamples); // measurement window, post-settle

  std::complex<double> inputCorr(0.0, 0.0), outputCorr(0.0, 0.0);

  for (int n = 0; n < settleSamples + windowSamples; ++n)
  {
    const double x = std::cos(omega * n);
    const double y = filter.ProcessSample(x);
    if (n >= settleSamples)
    {
      const std::complex<double> basis = std::exp(std::complex<double>(0.0, -omega * n));
      inputCorr += x * basis;
      outputCorr += y * basis;
    }
  }

  if (std::abs(inputCorr) < 1e-12)
    return std::complex<double>(0.0, 0.0); // degenerate (shouldn't happen for a real cosine, guards div-by-~0 anyway

  return outputCorr / inputCorr;
}

inline double MagnitudeToDb(std::complex<double> gain)
{
  const double mag = std::abs(gain);
  return 20.0 * std::log10(std::max(1e-12, mag));
}

// Sums the COMPLEX gains of every given band at `freqHz` -- correct for
// a parallel topology (see file header). Muted bands are skipped (they
// contribute silence, not a 0dB-passthrough or anything else); this
// mirrors Band::ProcessSample's own mute behavior, not just magnitude=0.
struct BandResponseInput { FilterParams filter; bool muted; double gainLinear; };

inline std::complex<double> ComplexCombinedResponse(const std::vector<BandResponseInput>& bands, double sampleRate, double freqHz)
{
  std::complex<double> total(0.0, 0.0);
  for (const auto& b : bands)
  {
    if (b.muted) continue;
    total += ComplexFilterResponse(b.filter, sampleRate, freqHz) * b.gainLinear;
  }
  return total;
}

} // namespace exoeq
