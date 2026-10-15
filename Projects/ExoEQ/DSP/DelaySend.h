#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// DelaySend.h
// First of the per-band "contained functions" from the spec (sidechain,
// frequency shifting, distortion, delay) — a simple feedback delay line a
// Band can run on its own filtered output before summing into the mix.
// Deliberately minimal: a circular buffer, linear-interpolated read for
// fractional delay times, feedback, and a dry/wet mix. Frequency-shift and
// distortion sends follow the same shape (ProcessSample in, ProcessSample
// out, SetParams) so they drop into Band the same way DelaySend does.

namespace exoeq
{

struct DelaySendParams
{
  double delayMs = 250.0;
  double feedback = 0.3;   // 0..<1 ; clamped below 1 to stay stable
  double mix = 0.3;        // 0=dry, 1=fully wet
};

class DelaySend
{
public:
  void SetSampleRate(double sampleRate)
  {
    mSampleRate = std::max(1.0, sampleRate);
    // 2 seconds max delay is plenty for a band send; resize once, not per-block.
    mBuffer.assign(static_cast<size_t>(mSampleRate * 2.0) + 4, 0.0);
    mWritePos = 0;
  }

  void SetParams(const DelaySendParams& p)
  {
    mParams = p;
    mParams.feedback = std::clamp(mParams.feedback, 0.0, 0.98);
    mParams.mix = std::clamp(mParams.mix, 0.0, 1.0);
    const double maxDelayMs = (mBuffer.size() - 4) * 1000.0 / mSampleRate;
    mParams.delayMs = std::clamp(mParams.delayMs, 0.0, maxDelayMs);
  }

  void Reset()
  {
    std::fill(mBuffer.begin(), mBuffer.end(), 0.0);
    mWritePos = 0;
  }

  double ProcessSample(double input)
  {
    if (mBuffer.empty())
      return input;

    const double delaySamples = mParams.delayMs * 0.001 * mSampleRate;
    const double readPosF = static_cast<double>(mWritePos) - delaySamples;
    const size_t bufSize = mBuffer.size();
    // Wrap into [0, bufSize) before splitting into integer/fractional parts.
    double wrapped = std::fmod(readPosF, static_cast<double>(bufSize));
    if (wrapped < 0.0) wrapped += static_cast<double>(bufSize);
    const size_t i0 = static_cast<size_t>(wrapped);
    const size_t i1 = (i0 + 1) % bufSize;
    const double frac = wrapped - static_cast<double>(i0);
    const double delayed = mBuffer[i0] * (1.0 - frac) + mBuffer[i1] * frac;

    mBuffer[mWritePos] = input + delayed * mParams.feedback;
    mWritePos = (mWritePos + 1) % bufSize;

    return input * (1.0 - mParams.mix) + delayed * mParams.mix;
  }

private:
  DelaySendParams mParams;
  std::vector<double> mBuffer;
  size_t mWritePos = 0;
  double mSampleRate = 44100.0;
};

} // namespace exoeq
