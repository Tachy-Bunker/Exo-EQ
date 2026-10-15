#pragma once

#include "FractalGenerator.h" // shared GeneratedNode type + detail::ClampToSafeRange, same reuse as TransformFilter.h
#include "../Vendor/stb/stb_image.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

// ImageToEq.h
// P5 scope step 3 of 3 (the last piece -- see the Features Doc's "P5
// scope" section). Same one-shot-batch-on-the-UI-thread nature as the
// other two generators: this runs once when the user picks an image and
// clicks "Generate from Image", not per sample, so none of the usual
// audio-thread performance work applies here either -- decoding a
// modest image and sampling a few dozen columns from it is negligible
// next to a single audio buffer's processing time, let alone this
// running on the UI thread specifically.
//
// Vendored dependency: Vendor/stb/stb_image.h, fetched verbatim from the
// stb project's own GitHub repo (nothings/stb, MIT/public-domain dual
// license per the file's own header) -- same "vendor the real minimal
// thing" pattern this project already used for libMTSClient.h/.cpp.
// stb_image is a single header; STB_IMAGE_IMPLEMENTATION is defined in
// exactly one place (see ExoEQ.cpp's own #define right before its
// #include of this file) so the actual decoder body is compiled once,
// not once per translation unit that includes this header.
//
// Method: decode to 8-bit grayscale (stbi_load with desired_channels=1,
// which does the RGB-to-luminance conversion internally using the same
// weighted formula stb_image always uses for this -- not reimplemented
// here), then sample N evenly-spaced columns across the image width,
// averaging each column's full height into one luminance value per
// column (reduces single-row noise/aliasing rather than picking one
// arbitrary row). Column's x-position maps to frequency on a LOG scale
// (so the generated series reads as a spectrum the way a real EQ curve
// would, not linearly bunched at the low end) and luminance maps to
// gain -- brighter means louder, by convention (matches "it reads as a
// curve" the way a bright image region reads as a peak).
namespace exoeq
{

struct ImageToEqParams
{
  int nodeCount = 24;          // how many columns to sample, 2..64 (same kMaxNodes cap as the fractal generator)
  double minFreqHz = 40.0;     // left edge of the log-frequency mapping
  double maxFreqHz = 16000.0;  // right edge -- clamped to Nyquist same as every generated node
  double gainRangeDb = 24.0;   // luminance 0..255 maps to -gainRangeDb/2 .. +gainRangeDb/2
  double resonance = 0.707;    // every generated node shares one resonance value -- the image drives frequency+gain, not Q
  bool invertLuminance = false; // some images read better as "dark = loud" (e.g. a photo negative, or a hand-drawn curve on a light background)
};

struct ImageToEqResult
{
  bool ok = false;
  std::string errorMessage; // human-readable reason when ok==false -- stbi_failure_reason()'s own text, or a local check's message
  std::vector<GeneratedNode> nodes;
};

// Loads and converts in one call -- there is no reason to decode an
// image and NOT immediately reduce it to a node series in this
// plugin's actual use of stb_image, so this is the one function
// ExoEQ.cpp calls rather than exposing the raw decode step separately.
inline ImageToEqResult GenerateFromImageFile(const std::string& filePath, const ImageToEqParams& params, double sampleRate)
{
  ImageToEqResult result;

  int width = 0, height = 0, channelsInFile = 0;
  // desired_channels=1: ask stb_image to hand back GRAYSCALE directly --
  // it does the RGB/RGBA-to-luminance reduction internally (a standard
  // weighted-average formula, not reimplemented here), so this file
  // never touches per-channel pixel data itself.
  unsigned char* pixels = stbi_load(filePath.c_str(), &width, &height, &channelsInFile, 1);
  if (!pixels)
  {
    result.errorMessage = stbi_failure_reason() ? stbi_failure_reason() : "unknown image load failure";
    return result;
  }

  if (width < 1 || height < 1)
  {
    stbi_image_free(pixels);
    result.errorMessage = "decoded image has zero width or height";
    return result;
  }

  const int nodeCount = std::clamp(params.nodeCount, 2, 64); // same upper bound as FractalGenParams::kMaxNodes, for the same reason (128-band cap headroom)
  const double minFreq = std::max(20.0, std::min(params.minFreqHz, params.maxFreqHz - 1.0));
  const double maxFreq = std::max(minFreq + 1.0, params.maxFreqHz);
  const double logMin = std::log(minFreq);
  const double logMax = std::log(maxFreq);

  result.nodes.reserve(static_cast<size_t>(nodeCount));

  for (int i = 0; i < nodeCount; ++i)
  {
    // Column position, log-mapped to frequency -- i=0 is the image's
    // left edge (minFreqHz), i=nodeCount-1 is its right edge (maxFreqHz).
    const double t = (nodeCount > 1) ? static_cast<double>(i) / static_cast<double>(nodeCount - 1) : 0.0;
    const double freq = std::exp(logMin + t * (logMax - logMin));

    const int col = std::clamp(static_cast<int>(t * (width - 1) + 0.5), 0, width - 1);

    // Average the FULL column height into one luminance value, rather
    // than sampling a single arbitrary row -- a thin horizontal line in
    // an otherwise-dark image shouldn't vanish or dominate depending on
    // exactly which row happened to get picked.
    long sum = 0;
    for (int row = 0; row < height; ++row)
      sum += pixels[static_cast<size_t>(row) * width + col];
    double luminance01 = (static_cast<double>(sum) / static_cast<double>(height)) / 255.0; // 0..1
    if (params.invertLuminance) luminance01 = 1.0 - luminance01;

    GeneratedNode node;
    node.freqHz = detail::ClampToSafeRange(freq, sampleRate);
    node.gainDb = (luminance01 - 0.5) * params.gainRangeDb;
    node.resonance = params.resonance;
    result.nodes.push_back(node);
  }

  stbi_image_free(pixels);
  result.ok = true;
  return result;
}

} // namespace exoeq
