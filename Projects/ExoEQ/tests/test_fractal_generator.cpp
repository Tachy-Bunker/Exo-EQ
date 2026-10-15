#include "DSP/FractalGenerator.h"
#include <cstdio>
#include <cmath>

using namespace exoeq;

int main()
{
  int failures = 0;
  auto check = [&](bool cond, const char* label)
  {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", label);
    if (!cond) ++failures;
  };

  // --- 1. Knob-driven default: no formula needed, produces a sane series. ---
  {
    FractalGenParams p;
    p.seedFreqHz = 220.0;
    p.ratio = 2.0; // plain octave stack for a predictable check
    p.branchFactor = 2;
    p.depth = 2;
    const auto nodes = GenerateFractalSeries(p, 48000.0);
    check(nodes.size() >= 3, "knob-driven generator produces more than just the seed node");
    check(nodes[0].freqHz > 219.0 && nodes[0].freqHz < 221.0, "seed node sits at the seed frequency");
  }

  // --- 2. Nyquist clamp: an absurd seed frequency never exceeds the
  //        safety-guarded Nyquist bound at a given sample rate. ---
  {
    FractalGenParams p;
    p.seedFreqHz = 500000.0; // absurdly high on purpose
    p.ratio = 3.0;
    p.branchFactor = 3;
    p.depth = 3;
    const double sr = 48000.0;
    const auto nodes = GenerateFractalSeries(p, sr);
    bool allSafe = true;
    for (const auto& n : nodes)
      if (n.freqHz > sr * 0.5 * 0.98 + 1.0e-6) allSafe = false;
    check(allSafe, "every generated node is clamped below the 2%-guarded Nyquist bound");
    check(!nodes.empty(), "clamping doesn't drop nodes to an empty series");
  }

  // --- 3. Nyquist clamp tracks sample rate -- the same params at two
  //        different rates respect each rate's own bound. ---
  {
    FractalGenParams p;
    p.seedFreqHz = 30000.0;
    p.ratio = 1.5;
    p.branchFactor = 2;
    p.depth = 2;
    const auto nodesLowSr = GenerateFractalSeries(p, 44100.0);
    const auto nodesHighSr = GenerateFractalSeries(p, 192000.0);
    bool lowSrSafe = true, highSrSafe = true;
    for (const auto& n : nodesLowSr) if (n.freqHz > 44100.0 * 0.5 * 0.98 + 1.0e-6) lowSrSafe = false;
    for (const auto& n : nodesHighSr) if (n.freqHz > 192000.0 * 0.5 * 0.98 + 1.0e-6) highSrSafe = false;
    check(lowSrSafe, "44.1kHz series respects 44.1kHz's own Nyquist bound");
    check(highSrSafe, "192kHz series respects 192kHz's own (much higher) Nyquist bound");
    // At the higher rate, the clamp has far more headroom, so more of the
    // naturally-generated high nodes should survive unclamped/distinct.
    check(nodesHighSr.size() >= nodesLowSr.size(), "higher sample rate's extra headroom doesn't produce FEWER usable nodes than the lower rate");
  }

  // --- 4. Depth/branch factor are bounded even with out-of-range input
  //        (defensive clamp, not a param-validation crash). ---
  {
    FractalGenParams p;
    p.seedFreqHz = 440.0;
    p.ratio = 1.5;
    p.branchFactor = 999; // way out of range
    p.depth = 999;
    const auto nodes = GenerateFractalSeries(p, 48000.0);
    check(nodes.size() <= static_cast<size_t>(FractalGenParams::kMaxNodes), "even pathological branch/depth input stays within kMaxNodes");
  }

  // --- 5. Jitter with the same seed is reproducible; different seeds differ. ---
  {
    FractalGenParams p;
    p.seedFreqHz = 440.0;
    p.ratio = 1.5;
    p.branchFactor = 2;
    p.depth = 2;
    p.jitterPct = 10.0;
    p.jitterSeed = 42;
    const auto nodesA = GenerateFractalSeries(p, 48000.0);
    const auto nodesB = GenerateFractalSeries(p, 48000.0);
    p.jitterSeed = 1337;
    const auto nodesC = GenerateFractalSeries(p, 48000.0);

    bool sameSeedIdentical = (nodesA.size() == nodesB.size());
    for (size_t i = 0; sameSeedIdentical && i < nodesA.size(); ++i)
      if (std::fabs(nodesA[i].freqHz - nodesB[i].freqHz) > 1.0e-9) sameSeedIdentical = false;
    check(sameSeedIdentical, "same jitter seed reproduces the exact same series");

    bool anyDifferent = (nodesA.size() != nodesC.size());
    for (size_t i = 0; !anyDifferent && i < nodesA.size() && i < nodesC.size(); ++i)
      if (std::fabs(nodesA[i].freqHz - nodesC[i].freqHz) > 1.0e-6) anyDifferent = true;
    check(anyDifferent, "a different jitter seed produces a measurably different series");
  }

  // --- 6. Inline composer path: a simple formula overrides the knob path. ---
  {
    FractalGenParams p;
    p.seedFreqHz = 100.0;
    p.branchFactor = 1;
    p.depth = 4;
    p.formula = "pow(2, k)"; // exact octave stack: 100, 200, 400, 800
    const auto nodes = GenerateFractalSeries(p, 48000.0);
    check(nodes.size() == 4, "formula path produces branchFactor*depth nodes");
    check(std::fabs(nodes[0].freqHz - 100.0) < 0.5, "formula node 0 == seed * 2^0");
    check(std::fabs(nodes[1].freqHz - 200.0) < 0.5, "formula node 1 == seed * 2^1");
    check(std::fabs(nodes[3].freqHz - 800.0) < 0.5, "formula node 3 == seed * 2^3");
  }

  // --- 7. Malformed formula degrades gracefully instead of throwing out
  //        of GenerateFractalSeries or producing a non-finite frequency. ---
  {
    FractalGenParams p;
    p.seedFreqHz = 220.0;
    p.ratio = 2.0;
    p.branchFactor = 2;
    p.depth = 2;
    p.formula = "pow(2, k, )"; // malformed on purpose
    bool threw = false;
    std::vector<GeneratedNode> nodes;
    try { nodes = GenerateFractalSeries(p, 48000.0); }
    catch (...) { threw = true; }
    check(!threw, "a malformed formula does not throw out of GenerateFractalSeries");
    bool allFinite = !nodes.empty();
    for (const auto& n : nodes) if (!std::isfinite(n.freqHz)) allFinite = false;
    check(allFinite, "a malformed formula still yields finite frequencies (falls back to ratio^k per node)");
  }

  // --- 8. ExprEval directly: functions, variables, precedence. ---
  {
    ExprEval::Vars v; v.k = 3.0; v.depth = 1.0; v.n = 8.0; v.seed = 42.0;
    check(std::fabs(ExprEval::Evaluate("2 + 3 * 4", v) - 14.0) < 1.0e-9, "operator precedence: 2+3*4 == 14");
    check(std::fabs(ExprEval::Evaluate("(2 + 3) * 4", v) - 20.0) < 1.0e-9, "parens override precedence: (2+3)*4 == 20");
    check(std::fabs(ExprEval::Evaluate("k", v) - 3.0) < 1.0e-9, "variable k reads from Vars");
    check(std::fabs(ExprEval::Evaluate("pow(2, k)", v) - 8.0) < 1.0e-9, "pow(2,k) with k=3 == 8");
    check(std::fabs(ExprEval::Evaluate("phi", v) - 1.6180339887498949) < 1.0e-9, "phi constant is the golden ratio");
    bool threwOnUnknown = false;
    try { ExprEval::Evaluate("bogusVar", v); } catch (...) { threwOnUnknown = true; }
    check(threwOnUnknown, "an unknown variable throws (caught upstream by GenerateFractalSeries, not silently 0)");
  }

  // --- 9. Custom variables E/X/O/M/U/S/I/C/A (one per letter of
  //        EXOMUSICA -- the user's easter egg): reach the formula with
  //        their already-scaled values (NOT raw 0..1), and an untouched
  //        slot defaults to 0 rather than erroring. Order matches
  //        ExprEval::Vars::custom's own order: E=0, X=1, O=2, M=3, U=4,
  //        S=5, I=6, C=7, A=8. ---
  {
    FractalGenParams p;
    p.seedFreqHz = 1.0; // 1.0 so the formula's own output IS the frequency, nothing to disentangle
    p.branchFactor = 1;
    p.depth = 1;
    p.formula = "E + X * k"; // E=500 (base), X=100 (per-node step)
    p.customVarValue[0] = 500.0; // E
    p.customVarValue[1] = 100.0; // X
    const auto nodes = GenerateFractalSeries(p, 48000.0);
    check(nodes.size() == 1, "formula with custom vars produces the expected node count");
    check(std::fabs(nodes[0].freqHz - 500.0) < 0.5, "node 0: E + X*0 == E == 500Hz");
  }
  {
    ExprEval::Vars v;
    v.custom[0] = 42.0; // E
    // X/O/M/U/S/I/C/A (custom[1..8]) left at their struct default of 0.0
    check(std::fabs(ExprEval::Evaluate("E", v) - 42.0) < 1.0e-9, "variable 'E' reads its scaled custom value");
    check(std::fabs(ExprEval::Evaluate("A", v) - 0.0) < 1.0e-9, "an untouched custom variable ('A') defaults to 0, not an error");
    check(std::fabs(ExprEval::Evaluate("E + X + O + M + U + S + I + C + A", v) - 42.0) < 1.0e-9, "all 9 custom variables (EXOMUSICA) are reachable by name");
    bool lowercaseRejected = false;
    try { ExprEval::Evaluate("e", v); } catch (...) { lowercaseRejected = true; }
    check(lowercaseRejected, "lowercase 'e' is NOT one of the custom variables -- matching is case-sensitive/uppercase-only, by design");
  }

  std::printf("\n%d check(s) failed.\n", failures);
  return failures == 0 ? 0 : 1;
}
