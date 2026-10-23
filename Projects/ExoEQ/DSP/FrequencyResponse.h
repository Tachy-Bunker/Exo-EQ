#pragma once

#include "FilterCore.h"
#include "FilterTypes.h"
#include <complex>
#include <cmath>
#include <vector>
#include <algorithm>

// FrequencyResponse.h
// Measured small-signal complex response of a band's ACTUAL filter (the same
// MultiPoleFilter BandChain runs), used by the graph's response curve.
//
// REWRITTEN THIS ROUND:
//  - Measures MultiPoleFilter, not StateVariableFilter. The old version only
//    matched the MorphZ type; every other type was measured through the wrong
//    filter.
//  - One filter instance is reused across a whole table (was: a new filter per
//    frequency point).
//  - The measurement window is a whole number of cycles, so leakage cancels in
//    the correlation ratio, and it is never shorter than ~1024 samples.
//  - BuildResponseTable() fills a log-spaced table once per band; the graph
//    reads the table, so painting costs a few lookups per band, not a filter run.
//
// Scope: this is SMALL-SIGNAL. Nonlinear types (Airwindows sin-folds, combs,
// ring mod, resampler, FM, formant, phaser, physical resonators) are measured
// with a tiny probe, so the curve shows their response to a quiet input only.
// Their loud-signal character is not shown.
//
// Sums are complex: parallel bands add as complex gains, so overlap cancels or
// reinforces correctly.

namespace exoeq
{

// Probe amplitude. Small enough to stay in the filter's linear region for
// most types; the ratio cancels the amplitude anyway.
constexpr double kResponseProbeAmp = 1e-3;

// Complex gain of one band's filter at `freqHz`, measured with a reused
// filter. `filter` must already have SetSampleRate() applied.
inline std::complex<double> MeasureComplexGain(IFilterNode& filter, const FilterParams& params, double sampleRate, double freqHz)
{
  constexpr double kPi = 3.14159265358979323846;
  const double omega = 2.0 * kPi * freqHz / sampleRate;
  const double period = sampleRate / std::max(1.0, freqHz); // samples per cycle

  // Settle time: high Q rings longer; low cutoffs settle slower in samples.
  const double settle = std::clamp(params.resonance * 400.0 + sampleRate / std::max(20.0, params.cutoffHz) * 20.0, 500.0, 20000.0);
  const int settleSamples = static_cast<int>(settle + 3.0 * period);

  // Measurement: whole cycles, at least 1024 samples.
  const int cycles = std::max(4, static_cast<int>(std::ceil(1024.0 / period)));
  const int windowSamples = std::max(1, static_cast<int>(std::lround(cycles * period)));

  filter.Reset();
  filter.SetParams(params);

  std::complex<double> inputCorr(0.0, 0.0), outputCorr(0.0, 0.0);
  const int total = settleSamples + windowSamples;
  for (int n = 0; n < total; ++n)
  {
    const double x = kResponseProbeAmp * std::cos(omega * n);
    const double y = filter.ProcessSample(x);
    if (n >= settleSamples)
    {
      const std::complex<double> basis = std::exp(std::complex<double>(0.0, -omega * n));
      inputCorr += x * basis;
      outputCorr += y * basis;
    }
  }

  if (std::abs(inputCorr) < 1e-18)
    return std::complex<double>(0.0, 0.0);
  return outputCorr / inputCorr;
}

// Fills `out[i]` with the complex gain at `freqs[i]` for one band. One filter
// instance is reused for the whole table.
inline void BuildResponseTable(const FilterParams& params, double sampleRate, const std::vector<double>& freqs, std::vector<std::complex<double>>& out)
{
  MultiPoleFilter filter;
  filter.SetSampleRate(sampleRate);
  out.resize(freqs.size());
  for (size_t i = 0; i < freqs.size(); ++i)
    out[i] = MeasureComplexGain(filter, params, sampleRate, freqs[i]);
}

// Log-spaced grid over [fMin, fMax] with `n` points.
inline std::vector<double> MakeLogFreqGrid(double fMin, double fMax, int n)
{
  std::vector<double> f(static_cast<size_t>(n));
  const double lo = std::log(fMin), hi = std::log(fMax);
  for (int i = 0; i < n; ++i)
    f[static_cast<size_t>(i)] = std::exp(lo + (hi - lo) * i / std::max(1, n - 1));
  return f;
}

// Linear interpolation in log-frequency on a table built by MakeLogFreqGrid.
inline std::complex<double> InterpResponse(const std::vector<std::complex<double>>& table, double fMin, double fMax, double freqHz)
{
  if (table.empty()) return { 0.0, 0.0 };
  const double lo = std::log(fMin), hi = std::log(fMax);
  const double pos = std::clamp((std::log(std::clamp(freqHz, fMin, fMax)) - lo) / (hi - lo), 0.0, 1.0) * static_cast<double>(table.size() - 1);
  const size_t i0 = static_cast<size_t>(pos);
  const size_t i1 = std::min(i0 + 1, table.size() - 1);
  const double frac = pos - static_cast<double>(i0);
  return table[i0] * (1.0 - frac) + table[i1] * frac;
}

inline double MagnitudeToDb(std::complex<double> gain)
{
  return 20.0 * std::log10(std::max(1e-12, std::abs(gain)));
}

} // namespace exoeq
