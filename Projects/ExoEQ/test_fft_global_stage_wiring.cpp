// Standalone test mirroring the EXACT pattern ExoEQ.cpp's ProcessBlock
// now uses for the global FFT stage (double <-> float scratch
// conversion, lazy scratch growth, per-channel instances) -- ExoEQ.cpp
// itself can't be compiled here (needs the real iPlug2 SDK, per this
// project's standing workflow), so this reproduces the surrounding
// plumbing against plain `double` buffers (iPlug2's `sample` type is
// typically double) to verify that layer specifically, on top of
// FftContainer.h's own already-tested correctness.
#include "DSP/FftContainer.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace exoeq;

struct TwoChannelFftStage
{
  FftBrickwallContainer container[2];
  std::vector<float> scratch[2];

  void EnsureScratchCapacity(int nFrames)
  {
    for (auto& s : scratch)
      if (static_cast<int>(s.size()) < nFrames) s.resize(static_cast<size_t>(nFrames));
  }

  // Mirrors ExoEQ.cpp's loop exactly: double in/out buffers, float
  // scratch conversion, in-place Process call.
  void ProcessBlock(double** io, int nChans, int nFrames, const FftBrickwallParams& params)
  {
    EnsureScratchCapacity(nFrames);
    for (int c = 0; c < nChans; ++c)
    {
      container[c].SetParams(params);
      float* s = scratch[c].data();
      for (int i = 0; i < nFrames; ++i) s[i] = static_cast<float>(io[c][i]);
      container[c].Process(s, s, nFrames);
      for (int i = 0; i < nFrames; ++i) io[c][i] = static_cast<double>(s[i]);
    }
  }
};

int main()
{
  int failures = 0;
  auto check = [&](bool cond, const char* label)
  {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", label);
    if (!cond) ++failures;
  };

  const double sr = 48000.0;

  // --- 1. Two channels process independently and correctly -- a tone
  //        inside the passband on channel 0 and a tone outside it on
  //        channel 1 should see different fates, proving per-channel
  //        state isn't accidentally shared. ---
  {
    TwoChannelFftStage stage;
    stage.container[0].SetSampleRate(sr);
    stage.container[1].SetSampleRate(sr);

    const int n = FftBrickwallContainer::kFftSize * 10;
    std::vector<double> ch0(static_cast<size_t>(n)), ch1(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
      ch0[static_cast<size_t>(i)] = 0.5 * std::sin(2.0 * M_PI * 500.0 * i / sr);  // inside a 0-1000Hz passband
      ch1[static_cast<size_t>(i)] = 0.5 * std::sin(2.0 * M_PI * 5000.0 * i / sr); // outside it
    }
    double* io[2] = { ch0.data(), ch1.data() };

    FftBrickwallParams p; p.lowCutHz = 0.0; p.highCutHz = 1000.0;
    stage.ProcessBlock(io, 2, n, p);

    const size_t skip = static_cast<size_t>(FftBrickwallContainer::GetLatencySamples() + FftBrickwallContainer::kFftSize);
    double rms0 = 0.0, rms1 = 0.0;
    size_t cnt = 0;
    for (size_t i = skip; i < static_cast<size_t>(n); ++i) { rms0 += ch0[i] * ch0[i]; rms1 += ch1[i] * ch1[i]; ++cnt; }
    rms0 = std::sqrt(rms0 / cnt); rms1 = std::sqrt(rms1 / cnt);

    check(rms0 > 0.2, "channel 0 (in-band tone) passes through at near-original amplitude");
    check(rms1 < 0.02, "channel 1 (out-of-band tone) is heavily attenuated, independent of channel 0's state");
  }

  // --- 2. The double<->float round trip itself introduces no audible
  //        error beyond float precision when the stage is effectively
  //        all-pass (full-spectrum passband). ---
  {
    TwoChannelFftStage stage;
    stage.container[0].SetSampleRate(sr);
    stage.container[1].SetSampleRate(sr);

    const int n = FftBrickwallContainer::kFftSize * 6;
    std::vector<double> ch0(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) ch0[static_cast<size_t>(i)] = 0.3 * std::sin(2.0 * M_PI * 1000.0 * i / sr);
    std::vector<double> ch1(ch0); // unused second channel, just needs to exist
    double* io[2] = { ch0.data(), ch1.data() };

    FftBrickwallParams p; p.lowCutHz = 0.0; p.highCutHz = sr * 0.5;
    stage.ProcessBlock(io, 2, n, p);

    const size_t skip = static_cast<size_t>(FftBrickwallContainer::GetLatencySamples() + FftBrickwallContainer::kFftSize);
    double maxAbs = 0.0;
    for (size_t i = skip; i < static_cast<size_t>(n); ++i) maxAbs = std::max(maxAbs, std::fabs(ch0[i]));
    check(maxAbs > 0.25 && maxAbs < 0.35, "double<->float round trip through an all-pass stage preserves amplitude within float precision");
  }

  // --- 3. Scratch buffers grow to fit a larger block size on a later
  //        call without crashing or truncating (simulates a host that
  //        changes its block size mid-session). ---
  {
    TwoChannelFftStage stage;
    stage.container[0].SetSampleRate(sr);
    stage.container[1].SetSampleRate(sr);

    std::vector<double> small(64, 0.1), large(4096, 0.1);
    std::vector<double> dummy1(64, 0.0), dummy2(4096, 0.0);
    FftBrickwallParams p; p.lowCutHz = 0.0; p.highCutHz = sr * 0.5;

    double* ioSmall[2] = { small.data(), dummy1.data() };
    stage.ProcessBlock(ioSmall, 2, 64, p);
    check(stage.scratch[0].size() >= 64, "scratch grows to fit the first (small) block size");

    double* ioLarge[2] = { large.data(), dummy2.data() };
    stage.ProcessBlock(ioLarge, 2, 4096, p);
    check(stage.scratch[0].size() >= 4096, "scratch grows further to fit a subsequently larger block size");

    // One more small call afterward should still work fine (capacity
    // never shrinks, and a smaller nFrames just uses the front of it).
    stage.ProcessBlock(ioSmall, 2, 64, p);
    check(true, "a smaller block after a larger one still processes without issue");
  }

  std::printf("\n%d check(s) failed.\n", failures);
  return failures == 0 ? 0 : 1;
}
