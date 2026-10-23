// Standalone check for the measured response (not part of the plugin build).
// Build from the Projects/ExoEQ folder:
//   g++ -std=c++17 -O2 -I. tools/ResponseCheck.cpp -o ResponseCheck
// Prints the measured dB at a few frequencies per type and the cost of one
// 96-point table, so you can confirm the curve matches the real filter.
#include "DSP/FrequencyResponse.h"
#include <cstdio>
#include <chrono>
#include <memory>
using namespace exoeq;
int main()
{
  const double sr = 48000.0;
  const double freqs[] = { 100, 1000, 3000, 10000 };
  const FilterTypeId types[] = { FilterTypeId::MorphZ, FilterTypeId::LP12, FilterTypeId::HP12,
                                 FilterTypeId::Notch12, FilterTypeId::AwXLowpass };
  const char* names[] = { "MorphZ", "LP12", "HP12", "Notch12", "AwXLowpass" };
  std::unique_ptr<MultiPoleFilter> filter(new MultiPoleFilter());
  filter->SetSampleRate(sr);
  for (int t = 0; t < 5; ++t)
  {
    FilterParams p{ 1000.0, 0.707, 0.0, types[t], 0.1 };
    std::printf("%-12s", names[t]);
    for (double f : freqs)
      std::printf("  %6.0f Hz: %7.2f dB", f, MagnitudeToDb(MeasureComplexGain(*filter, p, sr, f)));
    std::printf("\n");
  }
  FilterParams p{ 1000.0, 2.0, 0.0, FilterTypeId::LP12, 0.1 };
  auto grid = MakeLogFreqGrid(20.0, 20000.0, 96);
  std::vector<std::complex<double>> tbl;
  auto t0 = std::chrono::steady_clock::now();
  BuildResponseTable(p, sr, grid, tbl);
  auto t1 = std::chrono::steady_clock::now();
  std::printf("96-point table: %.1f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());
  return 0;
}
