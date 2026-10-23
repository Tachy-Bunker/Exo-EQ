#pragma once

#include <cstddef>
#include <cmath>
#include <cstdio>

// Single frequency readout format for the whole plugin (roadmap T2-1).
// "440 Hz" when the value is whole, "3220.54 Hz" when it has a fraction. Never abbreviated to kHz.
// Use this for every Hz/kHz readout so the knob, node tag, hover label, and
// band list never disagree.
inline void FormatHzReadout(double hz, char* out, std::size_t n)
{
  const double rounded = std::round(hz * 100.0) / 100.0;
  if (std::fabs(rounded - std::round(rounded)) < 1e-9)
    std::snprintf(out, n, "%.0f Hz", rounded);
  else
    std::snprintf(out, n, "%.2f Hz", rounded);
}

// Node readouts (hover tag and selected tag) use the abbreviated form: "3.22 kHz" from 1 kHz up,
// "440 Hz" below. Freq/Cutoff knobs keep FormatHzReadout's full-number form.
inline void FormatHzAbbrev(double hz, char* out, std::size_t n)
{
  if (hz >= 1000.0)
    std::snprintf(out, n, "%.2f kHz", hz / 1000.0);
  else
    FormatHzReadout(hz, out, n);
}
