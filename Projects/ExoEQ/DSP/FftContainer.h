#pragma once

#include "../Vendor/pffft/pffft.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// FftContainer.h -- P6 scope, first concrete piece: the "FFT bandpass
// container" (see the Features Doc's P6 scoping note). This is the
// thing an SVF cascade genuinely cannot do: a near-ideal, brick-wall
// magnitude response with no resonant ringing and no slope-order
// tradeoff -- the cutoff is as steep as the FFT bin resolution allows,
// not "steeper if you stack more biquads." The tradeoff for that is the
// one this whole file exists to make explicit and correct: real,
// unavoidable LATENCY (the STFT window is genuine lookahead) and real,
// higher-than-a-biquad CPU cost per instance.
//
// LIBRARY CHOICE (the user's explicit call: most performant/most CPU
// efficient): PFFFT, vendored verbatim from the actively-maintained
// cpuimage/pffft mirror of Julien Pommier's original (BSD/FFTPACK
// license, confirmed from the file's own header). Chosen over KissFFT
// specifically for performance -- PFFFT is SIMD-vectorized (SSE/NEON/
// AltiVec, auto-selected at compile time) and is the de facto standard
// choice for exactly this "fast real-valued FFT in an audio plugin"
// use case; KissFFT is simpler to vendor but meaningfully slower with
// no SIMD path, which is the wrong tradeoff for a feature already
// being added specifically for its process cost per instance. Verified
// by actually compiling it and round-tripping a real forward+inverse
// transform before writing anything against it (not just reading the
// header).
//
// ARCHITECTURE: this is a BLOCK-based processor, not a per-sample
// IFilterNode like every other filter type in this plugin -- an STFT
// fundamentally can't run one sample at a time, it needs a full
// analysis frame before it can produce anything. That's WHY this is
// its own container type, not a 33rd dropdown entry cascaded through
// the existing per-sample ProcessBlock loop: wiring it into ExoEQ.cpp
// means adding a parallel, block-based path alongside the per-sample
// BandChain path, exactly the "optional parallel path" the Architecture
// decisions section already calls for FFT-domain features. NOT wired
// into ExoEQ.h/.cpp yet in this round -- see this round's README for
// why that's a deliberate stopping point, not an oversight.
//
// METHOD: classic streaming STFT with overlap-add (OLA). Fixed FFT
// size and hop chosen for a known-good, verified-COLA (constant
// overlap-add) configuration rather than exposing them as free knobs
// yet: kFftSize=1024, Hann window, 75% overlap (kHopSize = kFftSize/4
// = 256) -- Hann at 75% overlap sums to an exactly flat unity gain
// across frame boundaries, which is what makes OLA reconstruction
// artifact-free in the first place. A future round could expose FFT
// size as a latency/frequency-resolution tradeoff knob; fixed for now
// so the first version has one less variable to get wrong.
//
// LATENCY: GetLatencySamples() below is NOT advisory -- it's the exact
// number ExoEQ.cpp's ComputeTotalLatencySamples/SetLatency hook (added
// back in the fractal-generator round specifically for this moment)
// must add once this is wired in, per active FFT container instance in
// the chain. This is genuinely the first feature in this project to
// put a nonzero number through that hook.
namespace exoeq
{

// pffft's transform functions REQUIRE their input/output/work buffers
// to be allocated with correct SIMD alignment (pffft_aligned_malloc) --
// this isn't a performance nicety, it's a hard correctness requirement
// of the library's own contract. A plain std::vector<float> is NOT
// guaranteed to meet it (the standard allocator typically only
// guarantees alignof(float), well under what AVX/NEON paths need), and
// passing one in anyway doesn't fail loudly -- it silently reads/writes
// slightly out of bounds of the undersized allocation, corrupting
// whatever heap memory happens to sit next to it. That exact bug
// existed in an earlier draft of this file (found via an irregular-
// chunk-size test unexpectedly corrupting an unrelated buffer) and is
// why these 3 buffers specifically go through this tiny RAII wrapper
// instead of std::vector. mInputRing/mOutputAccum/mWindow are NEVER
// passed to pffft directly (only copied into/out of the aligned
// buffers), so they're fine as plain std::vector.
class PffftAlignedBuffer
{
public:
  explicit PffftAlignedBuffer(int count)
  {
    // This vendored pffft variant exposes pffft_aligned_calloc (not
    // _malloc) -- calloc semantics (zeroed) are a bonus here, not just
    // tolerated, since every one of these buffers wants to start at
    // zero anyway.
    //
    // EXTRA PADDING, deliberately: pffft_aligned_calloc's own slack
    // formula (align_bytes/sizeOfElement + 1 extra elements before
    // rounding up to a 64-byte boundary) leaves only a few bytes of
    // margin when asked for exactly `count` float-sized elements --
    // tight enough that it was verified (via AddressSanitizer, not
    // assumed) to heap-overflow in practice when the exact element
    // count was requested. Asking for extra headroom here costs
    // nothing that matters (a few dozen floats on a buffer already
    // sized in the thousands) and removes the risk entirely rather
    // than relying on getting right up to the library's own margin.
    mData = static_cast<float*>(pffft_aligned_calloc(static_cast<size_t>(count) + 32, sizeof(float)));
  }
  ~PffftAlignedBuffer() { pffft_aligned_free(mData); }
  PffftAlignedBuffer(const PffftAlignedBuffer&) = delete;
  PffftAlignedBuffer& operator=(const PffftAlignedBuffer&) = delete;
  float* data() { return mData; }
  const float* data() const { return mData; }
  float& operator[](size_t i) { return mData[i]; }
  const float& operator[](size_t i) const { return mData[i]; }
private:
  float* mData = nullptr;
};

struct FftBrickwallParams
{
  // Passband: [lowCutHz, highCutHz]. Bins outside this range are
  // zeroed entirely (not just attenuated) -- that's what "brick-wall"
  // means here, and it's the one thing a cascaded SVF structurally
  // cannot reproduce regardless of how many stages you add.
  double lowCutHz = 20.0;
  double highCutHz = 20000.0;
};

class FftBrickwallContainer
{
public:
  static constexpr int kFftSize = 1024;
  static constexpr int kHopSize = kFftSize / 4; // 256 -- 75% overlap, COLA-exact for Hann

  // The exact, required latency contribution of one instance, in
  // samples, independent of sample rate (an STFT's latency is a frame-
  // count property, not a time property -- the same 1024/256 config is
  // ~23ms at 44.1kHz but a fixed 768 samples regardless of the actual
  // sample rate). ExoEQ.cpp's ComputeTotalLatencySamples must sum this
  // once per active instance once this is wired into the shell.
  static constexpr int GetLatencySamples() { return kFftSize - kHopSize; }

  FftBrickwallContainer()
  {
    mSetup = pffft_new_setup(kFftSize, PFFFT_REAL);
    mWindow.resize(kFftSize);
    // Hann window, periodic form (not symmetric) -- the form that
    // actually satisfies COLA at a given hop/overlap ratio; the
    // symmetric textbook Hann (dividing by N-1) does NOT sum flat
    // across frame boundaries and would introduce audible amplitude
    // modulation at the hop rate if used here instead.
    for (int i = 0; i < kFftSize; ++i)
      mWindow[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * i / kFftSize));

    mInputRing.assign(static_cast<size_t>(kFftSize), 0.0f);
    mOutputAccum.assign(static_cast<size_t>(kFftSize), 0.0f);
    mPendingOutput.reserve(static_cast<size_t>(kHopSize) * 4);
  }

  ~FftBrickwallContainer()
  {
    if (mSetup) pffft_destroy_setup(mSetup);
  }

  // Not copyable -- holds a raw PFFFT_Setup* this constructor/destructor
  // pair owns exclusively; copying would double-free on destruction.
  // Nothing in this project needs to copy a filter instance (BandChain
  // already stores these by value in a fixed-size array elsewhere, so
  // if this is ever added there it needs to go in via emplace/reset,
  // not assignment -- flagging this now rather than after it bites).
  FftBrickwallContainer(const FftBrickwallContainer&) = delete;
  FftBrickwallContainer& operator=(const FftBrickwallContainer&) = delete;

  void SetSampleRate(double sampleRate) { mSampleRate = sampleRate; }

  void SetParams(const FftBrickwallParams& params) { mParams = params; }

  // Processes nFrames of mono audio. `out` may alias `in` (writes
  // happen into a separate pending-output queue before anything is
  // copied back, so in-place use is safe). The first GetLatencySamples()
  // samples of output across the FIRST call (or after Reset()) are
  // zero/silence -- that's the unavoidable lookahead filling up, not a
  // bug; ExoEQ.cpp's own PDC reporting is what tells the host to expect
  // that and compensate, exactly like any other plugin with real
  // algorithmic latency.
  void Process(const float* in, float* out, int nFrames)
  {
    for (int i = 0; i < nFrames; ++i)
    {
      PushInputSample(in[i]);
      out[i] = PopOutputSample();
    }
  }

  // Resets all internal state (ring buffers, accumulator) to silence --
  // for a sample-rate change or a hard transport reset, same moment
  // every other filter type in this plugin already re-zeros its own
  // state (see OnReset in ExoEQ.cpp, once this is actually wired in).
  void Reset()
  {
    std::fill(mInputRing.begin(), mInputRing.end(), 0.0f);
    std::fill(mOutputAccum.begin(), mOutputAccum.end(), 0.0f);
    mInputWritePos = 0;
    mSamplesSinceLastHop = 0;
    mPendingOutput.clear();
    mFlushedInitialLatency = false;
  }

private:
  void PushInputSample(float x)
  {
    mInputRing[static_cast<size_t>(mInputWritePos)] = x;
    mInputWritePos = (mInputWritePos + 1) % kFftSize;
    ++mSamplesSinceLastHop;

    if (mSamplesSinceLastHop >= kHopSize)
    {
      mSamplesSinceLastHop = 0;
      RunOneFrame();
    }
  }

  float PopOutputSample()
  {
    if (mPendingOutput.empty())
    {
      // Still filling the initial latency window (or a brief underrun
      // that shouldn't normally happen at these fixed block sizes) --
      // silence, not garbage. This is exactly the window
      // GetLatencySamples() exists to tell the host about in advance.
      return 0.0f;
    }
    const float v = mPendingOutput.front();
    mPendingOutput.erase(mPendingOutput.begin());
    return v;
  }

  void RunOneFrame()
  {
    // Read the most recent kFftSize samples out of the ring buffer in
    // correct chronological order (oldest first) into a contiguous
    // analysis frame -- pffft needs a flat array, not a wrapped ring.
    for (int i = 0; i < kFftSize; ++i)
    {
      const int ringIdx = (mInputWritePos + i) % kFftSize; // oldest-to-newest
      mAnalysisFrame[static_cast<size_t>(i)] = mInputRing[static_cast<size_t>(ringIdx)] * mWindow[static_cast<size_t>(i)];
    }

    pffft_transform_ordered(mSetup, mAnalysisFrame.data(), mFreqDomain.data(), mWorkBuf.data(), PFFFT_FORWARD);

    // Ordered real-FFT bin layout from pffft_transform_ordered: bin 0 is
    // DC, bin 1 is Nyquist (the real/Nyquist pair pffft packs there for
    // a real transform), then bins [2,3] = (re,im) of frequency 1,
    // [4,5] = frequency 2, etc., up to kFftSize/2 - 1.
    const double binHz = mSampleRate / static_cast<double>(kFftSize);
    const int numBins = kFftSize / 2;
    for (int k = 1; k < numBins; ++k)
    {
      const double freqHz = k * binHz;
      const bool inPassband = (freqHz >= mParams.lowCutHz && freqHz <= mParams.highCutHz);
      if (!inPassband)
      {
        mFreqDomain[static_cast<size_t>(2 * k)] = 0.0f;
        mFreqDomain[static_cast<size_t>(2 * k + 1)] = 0.0f;
      }
    }
    // DC (bin 0) and Nyquist (bin 1, packed at index 1 per pffft's real
    // layout) follow the same passband test using their own fixed
    // frequencies (0 Hz and sampleRate/2 respectively).
    if (!(0.0 >= mParams.lowCutHz && 0.0 <= mParams.highCutHz))
      mFreqDomain[0] = 0.0f;
    const double nyquistHz = mSampleRate * 0.5;
    if (!(nyquistHz >= mParams.lowCutHz && nyquistHz <= mParams.highCutHz))
      mFreqDomain[1] = 0.0f;

    pffft_transform_ordered(mSetup, mFreqDomain.data(), mAnalysisFrame.data(), mWorkBuf.data(), PFFFT_BACKWARD);

    // pffft's inverse transform is UNNORMALIZED (output is the
    // mathematical IDFT scaled by N) -- pffft's own docs are explicit
    // that callers must divide by N themselves. Folding that scale-down
    // into the SAME pass as the synthesis window (applied here, not at
    // analysis) keeps one pass doing both jobs instead of two.
    //
    // SECOND, separate scale factor -- the actual COLA normalization:
    // with a Hann window applied at BOTH analysis and synthesis, the
    // quantity that must sum to a flat constant across overlapping
    // frames is the window SQUARED, not the window itself. At this
    // exact 1024/256 (75% overlap) configuration, sum(hann(i)^2) over
    // all contributing frame shifts equals 1.5 EVERYWHERE (verified
    // numerically, not assumed) -- so without correcting for it, OLA
    // reconstruction comes out 1.5x too loud. kOlaNormalization below
    // is that correction; if kFftSize/kHopSize or the window function
    // ever change, this constant must be re-derived (by the same
    // numerical sum, not guessed) or reconstruction gain will be wrong
    // again.
    constexpr float kOlaNormalization = 1.5f;
    const float scale = 1.0f / (static_cast<float>(kFftSize) * kOlaNormalization);
    for (int i = 0; i < kFftSize; ++i)
      mAnalysisFrame[static_cast<size_t>(i)] *= mWindow[static_cast<size_t>(i)] * scale;

    // Overlap-add: sum this frame's windowed-synthesis output into the
    // accumulator at the current hop offset, emit (pop) the oldest
    // kHopSize samples -- which are now "finished" (no later frame will
    // ever contribute to them again) -- into the pending-output queue,
    // then shift the accumulator left by one hop's worth for the next
    // frame.
    for (int i = 0; i < kFftSize; ++i)
      mOutputAccum[static_cast<size_t>(i)] += mAnalysisFrame[static_cast<size_t>(i)];

    for (int i = 0; i < kHopSize; ++i)
      mPendingOutput.push_back(mOutputAccum[static_cast<size_t>(i)]);

    // Shift the accumulator down by one hop and zero the newly-exposed
    // tail, ready for the next frame's contribution.
    std::memmove(mOutputAccum.data(), mOutputAccum.data() + kHopSize, sizeof(float) * static_cast<size_t>(kFftSize - kHopSize));
    std::fill(mOutputAccum.end() - kHopSize, mOutputAccum.end(), 0.0f);
  }

  PFFFT_Setup* mSetup = nullptr;
  double mSampleRate = 44100.0;
  FftBrickwallParams mParams;

  std::vector<float> mWindow;
  std::vector<float> mInputRing;
  std::vector<float> mOutputAccum;
  PffftAlignedBuffer mAnalysisFrame{kFftSize};
  PffftAlignedBuffer mFreqDomain{kFftSize};
  PffftAlignedBuffer mWorkBuf{kFftSize};
  std::vector<float> mPendingOutput;

  int mInputWritePos = 0;
  int mSamplesSinceLastHop = 0;
  bool mFlushedInitialLatency = false; // reserved; not yet used -- see README's note on initial-silence behavior
};

} // namespace exoeq
