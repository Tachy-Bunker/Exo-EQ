#pragma once

namespace exoeq
{

// Selectable filter TYPE, dropdown-style (the FabFilter "Highpass >
// Highpass 24oct" pattern the user asked for) -- separate from the
// continuous morph knob, which only ever applied to the original
// MorphZ type. MorphZ=0 is deliberately first/default: every band that
// predates this feature keeps its exact prior behavior (continuous
// LP-BP-HP morph via the validated TPT StateVariableFilter) with zero
// migration needed.
//
// "6/12/24/48" naming matches dB/octave slope, same convention
// FabFilter/most pro EQs use (1/2/4/8 poles respectively) -- see
// FilterTypes.h for how each is actually built.
//
// Capacitor and AirLift are ORIGINAL implementations done for this
// project, not ports of real Airwindows plugins -- there's no Airwindows
// source vendored in this sandbox to port from, and guessing at exact
// published algorithms from memory risks quietly shipping something that
// sounds like neither the real plugin nor a defensible original. Instead
// these are built from scratch in the Airwindows AESTHETIC the user
// asked for (simple, colorful, a bit lo-fi/characterful rather than
// surgical) -- see FilterTypes.h for exactly what each one does.
enum class FilterTypeId
{
  MorphZ = 0,   // existing continuous LP-BP-HP morph (unchanged default)
  LP6, LP12, LP24, LP48,
  HP6, HP12, HP24, HP48,
  BP12, BP24,
  Notch12,
  Capacitor,    // character type: self-modulating one-pole + soft saturation (ORIGINAL, not a port)
  AirLift,      // character type: resonant presence/air bump (ORIGINAL, not a port)

  // REAL Airwindows ports (MIT licensed, from the user-provided source) --
  // see DSP/AirwindowsFilters.h for exactly what's verbatim vs. adapted.
  AwLowpass2,   // Lowpass2: content-adaptive one-pole stack
  AwHighpass2,  // Highpass2: same, highpass side
  AwZLowpass,   // ZLowpass2: Emu e6400-style resonant/grind lowpass
  AwZHighpass,  // ZHighpass2: same family, highpass
  AwZBandpass,  // ZBandpass2: same family, bandpass
  AwZNotch,     // ZNotch2: same family, notch (golden-ratio resonance)
  AwXLowpass,   // XLowpass: sin()-folded feedback biquad
  AwXHighpass,  // XHighpass: hard-clamped feedback biquad, no fold
  AwXBandpass,  // XBandpass: sin()-folded feedback, input-side drive
  AwXNotch,     // XNotch: sin()-folded INPUT, plain feedback
  AwHoltLowpass, // Holt2: predictive lowpass (trend/forecast model, lowpass-only)
  AwBiquadStack, // BiquadStack: parallel "reversed neutron flow" resonant enhancer, 3 fixed stages
  AwYLowpass,    // YLowpass: resonant lowpass wrapped in an asymmetric encode/decode companding curve (uses the dedicated `extra` param, see FilterParams below)
  AwYHighpass,   // YHighpass: same Y-family companding wrapper, highpass side
  AwYBandpass,   // YBandpass: same family, bandpass
  AwYNotch,      // YNotch: same family, notch (its own reso floor, see AirwindowsYFilter)
  AwBiquadNonLinLowpass,  // BiquadNonLin: content-adaptive single-stage resonant lowpass, bipolar wet (morph) sweeps into a complementary/inverted shape
  AwBiquadNonLinBandpass, // BiquadNonLin: same, bandpass side (bipolar wet sweeps toward notch-like)
  AwZAcidLowpass, // ZAcidLowpass: resonant Bezier-interpolated sample-and-hold decimator ("acid" character), not a biquad at all
  AwHermepassHighpass, // Hermepass: subtractive one-pole cascade highpass, golden-ratio pole convergence, not a biquad
  AwAverMatrix, // AverMatrix: continuous-tap moving average x continuous poles, bipolar wet (negative wet = complementary highpass)
  AwSlewSonic,  // SlewSonic: 4-stage fixed-Q lowpass+slew ladder, "EQ tilt upwards" character

  // ORIGINAL character types (DSP/CharacterFilters.h) -- NOT ports,
  // there is no vendored source for these (unlike the Aw* entries
  // above). Built from real, public/academic DSP technique to cover
  // filter CATEGORIES the user asked about from Icarus by Tone2
  // (closed-source, nothing to port from) -- see CharacterFilters.h's
  // own header note for exactly what technique each one is built from.
  CombFeedforward, // y = x + g*x[n-D], classic feedforward comb
  CombFeedback,    // y = x + g*y[n-D], classic feedback comb
  RingModulator,   // signal x sine at cutoffHz, classic ring modulation
  FilterFM,        // one-pole lowpass whose cutoff is FM'd by an audio-rate oscillator
  VocalFormant,    // 3-formant parallel bandpass bank, sweeps A-E-I-O-U
  Phaser,          // N-stage first-order allpass cascade with feedback, moving notches
  PhysOddResonator,  // harmonic resonator bank, odd harmonics only (clarinet-like)
  PhysEvenResonator, // harmonic resonator bank, even harmonics only (flute-like)
  Resample,        // zero-order-hold decimator, classic bitcrush/sample-rate-reduction

  kNumFilterTypes
};

struct FilterParams
{
  double cutoffHz = 1000.0;
  double resonance = 0.707;
  double morph = 0.0;
  FilterTypeId type = FilterTypeId::MorphZ;

  // 4th generic knob, added specifically for AwYLowpass's powFactor (the
  // user explicitly asked for this to be its own automatable slot rather
  // than folded into resonance/morph like every earlier port -- see
  // AirwindowsFilters.h's AirwindowsYLowpassFilter header note). 0..1,
  // default 0.1 so an untouched band maps to powFactor == 1.0 (neutral,
  // see SetParams there) -- unused by every other type, default placed
  // last so old 3-arg/4-arg brace-inits elsewhere in the codebase keep
  // compiling unchanged.
  double extra = 0.1;
};

class IFilterNode
{
public:
  virtual ~IFilterNode() = default;
  virtual void SetSampleRate(double sampleRate) = 0;
  virtual void SetParams(const FilterParams& params) = 0;
  virtual void Reset() = 0;
  virtual double ProcessSample(double input) = 0;
};

} // namespace exoeq
