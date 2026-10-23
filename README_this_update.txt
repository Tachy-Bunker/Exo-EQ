EXOEQ UPDATE -- response curve now measures the real filter (roadmap item 1)
===========================================================================
Changed:
  Projects/ExoEQ/CircleViewControl.h        (curve source, cache, complex sum)
  Projects/ExoEQ/DSP/FrequencyResponse.h    (REPLACES your copy)
New (optional, not in the plugin build):
  Projects/ExoEQ/tools/ResponseCheck.cpp    (standalone check program)

WHAT CHANGED
- The graph curve is now the measured complex response of each band's actual
  filter (MultiPoleFilter, the same one the audio chain runs), summed as complex
  gains. Previously it was a hand-written approximation per type.
- FrequencyResponse.h had measured StateVariableFilter, which only matches the
  MorphZ type. It now measures MultiPoleFilter, so every type is measured
  through its own code.
- Each band's curve is cached. It only recomputes when that band's filter
  params change or the sample rate changes. Gain changes are free (applied
  outside the table).
- During a retune / resonance / gain drag a 32-point grid is used so mouse
  events stay cheap; the full 96-point grid is rebuilt on release.
- Morph and extra use the same /100 scaling as ReadBandSettings in ExoEQ.cpp.

LIMITS (say if any of these matter)
- Small-signal only: the curve is the response to a quiet probe. Nonlinear
  types (Airwindows sin-folds, combs, ring mod, resampler, FM, formant,
  phaser, physical resonators) show their quiet-input shape, not their loud
  character.
- Comb types read flat in the measurement: their delay needs a longer settle
  than the probe allows. Treat their curve as not reliable.
- Key-track and sidechain / group modulation are not reflected; the curve
  shows each band at its manual cutoff.
- Solo is not applied to the curve (same as before).

VERIFY
  g++ -std=c++17 -O2 -I. tools/ResponseCheck.cpp -o ResponseCheck   (run in Projects/ExoEQ)
  Expect: LP12 -3.0 dB at cutoff (1 kHz), flat below, rolling off above;
  HP12 mirror image; Notch12 a dip at 1 kHz; MorphZ at morph 0 matches LP12.

Not compiled in the plugin build here. The check program compiled and ran in
the sandbox with the full DSP tree; the plugin build has not been run.
