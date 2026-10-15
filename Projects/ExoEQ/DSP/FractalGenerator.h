#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cctype>

// FractalGenerator.h
// P5's first generative-layer piece (see the Features Doc's "P5 scope"
// section for the full plan -- this is step 1 of 3, the only one with no
// external dependencies and no open UI-design question blocking it).
//
// ONE-SHOT batch generator, not per-sample DSP: this runs once when the
// user clicks "Generate" and produces a flat list of (freqHz, gainDb,
// resonance) nodes, which ExoEQ.cpp then writes into real band params
// (see its own comment on this). That means none of this code runs on
// the audio thread and none of it needs to be fast in the way every
// other file in DSP/ does -- it's a few hundred evaluations at most, run
// once, on the UI thread.
//
// Three explicit user requirements this round, each with its own
// mechanism below:
//   1. "a cheap filter system" -- GeneratedNode doesn't carry a filter
//      type at all; ExoEQ.cpp always assigns generated bands one of the
//      project's own cheapest single-SVF dropdown types (BP12 by
//      default), never an Airwindows/character type. Keeping that
//      decision in ExoEQ.cpp rather than here, since "which types count
//      as cheap" is a plugin-level policy, not something this generator
//      itself should hardcode.
//   2. "not extend past Nyquist" -- every generated freqHz is clamped
//      into (20Hz, sampleRate*0.5*kNyquistGuard) -- see ClampToSafeRange.
//      The guard factor (0.98, not 1.0) is deliberate: biquad/SVF
//      coefficient derivations (tan(pi*f/sr)) blow up or go unstable as
//      f approaches sr/2 exactly, so a flat 2% safety margin below the
//      true Nyquist point is cheap insurance, not just a semantic
//      "not over 50%" check.
//   3. "easy to use for anyone, with an inline composer" -- the default
//      path (FractalGenParams::formula empty) needs zero math literacy:
//      just knobs (seed freq, ratio, branch factor, depth, gain decay,
//      jitter). The inline composer is OPT-IN: a short text formula,
//      evaluated once per generated node index via the tiny expression
//      evaluator below, for whoever wants to type their own curve
//      instead of turning knobs. Someone who never touches the formula
//      box never needs to know it exists.

namespace exoeq
{

struct GeneratedNode
{
  double freqHz = 1000.0;
  double gainDb = 0.0;
  double resonance = 0.707;
};

// ---------------------------------------------------------------------
// Tiny expression evaluator for the inline composer. Deliberately small
// and hand-written rather than pulling in a general-purpose expression
// library -- this runs a handful of times per Generate click, so there's
// nothing to optimize, but a hand-rolled recursive-descent parser over a
// short string is also just genuinely simpler to audit/trust than a
// dependency would be for something this narrow (4 binary operators, one
// unary minus, parens, a fixed variable/function set).
//
// Grammar (standard precedence): expr := term (('+'|'-') term)*
//                                 term := unary (('*'|'/') unary)*
//                                 unary := '-'? power
//                                 power := atom ('^' unary)?   (right-assoc)
//                                 atom := number | ident | ident '(' expr (',' expr)* ')' | '(' expr ')'
//
// Variables available: k (0-based node index), depth (0-based level this
// node belongs to), n (total node count being generated), seed (the
// user's jitter-seed param, exposed for formulas that want to derive
// their own pseudo-randomness from it), and E/X/O/M/U/S/I/C/A -- 9 user-
// defined knobs, one per letter of EXOMUSICA (the user's easter egg,
// spelled out deliberately rather than a generic a/b/c/.../i so the
// in-formula variable names are visibly the company name). Each one is
// a single real automatable param (a plain 0..1 knob) plus a user-set
// min/max RANGE (set via inline text, not automation -- see
// FractalGenParams::customVarValue) that the knob's 0..1 position is
// scaled into before the formula ever sees it. So "E" in a formula is
// never raw 0..1 -- it's already in whatever range the user typed, e.g.
// a knob labeled "E" ranged 20..2000 by its own min/max text box reads
// as a frequency-ish number directly, no formula-side rescaling needed.
// Case-sensitive and deliberately UPPERCASE to match the easter egg --
// "e" (lowercase) is not one of these and is simply an unknown variable.
// Functions available: sin, cos, log, log2, exp, abs, sqrt, pow(a,b),
// min(a,b), max(a,b).
class ExprEval
{
public:
  // Order matches EXOMUSICA letter-by-letter: E,X,O,M,U,S,I,C,A -- all 9
  // letters are distinct (no repeats in "EXOMUSICA"), so each gets its
  // own slot with no collision to resolve.
  static constexpr int kNumCustomVars = 9;

  struct Vars
  {
    double k = 0.0;
    double depth = 0.0;
    double n = 0.0;
    double seed = 0.0;
    double custom[kNumCustomVars] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }; // E, X, O, M, U, S, I, C, A in order
  };

  // Throws std::runtime_error on malformed input -- callers (the
  // generator below) catch this and fall back to the knob-driven
  // default rather than letting a typo in the formula box silently
  // produce garbage or crash the Generate click.
  static double Evaluate(const std::string& expr, const Vars& vars)
  {
    Parser p(expr, vars);
    const double result = p.ParseExpr();
    p.SkipSpaces();
    if (p.mPos != p.mText.size())
      throw std::runtime_error("unexpected trailing input in formula");
    if (!std::isfinite(result))
      throw std::runtime_error("formula produced a non-finite value");
    return result;
  }

private:
  class Parser
  {
  public:
    Parser(const std::string& text, const Vars& vars) : mText(text), mVars(vars) {}

    double ParseExpr()
    {
      double value = ParseTerm();
      for (;;)
      {
        SkipSpaces();
        if (Peek() == '+') { Advance(); value += ParseTerm(); }
        else if (Peek() == '-') { Advance(); value -= ParseTerm(); }
        else break;
      }
      return value;
    }

    double ParseTerm()
    {
      double value = ParseUnary();
      for (;;)
      {
        SkipSpaces();
        if (Peek() == '*') { Advance(); value *= ParseUnary(); }
        else if (Peek() == '/')
        {
          Advance();
          const double divisor = ParseUnary();
          if (divisor == 0.0) throw std::runtime_error("division by zero in formula");
          value /= divisor;
        }
        else break;
      }
      return value;
    }

    double ParseUnary()
    {
      SkipSpaces();
      if (Peek() == '-') { Advance(); return -ParseUnary(); }
      if (Peek() == '+') { Advance(); return ParseUnary(); }
      return ParsePower();
    }

    double ParsePower()
    {
      const double base = ParseAtom();
      SkipSpaces();
      if (Peek() == '^')
      {
        Advance();
        const double exponent = ParseUnary(); // right-associative
        return std::pow(base, exponent);
      }
      return base;
    }

    double ParseAtom()
    {
      SkipSpaces();
      if (Peek() == '(')
      {
        Advance();
        const double value = ParseExpr();
        SkipSpaces();
        Expect(')');
        return value;
      }
      if (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '.')
        return ParseNumber();
      if (std::isalpha(static_cast<unsigned char>(Peek())) || Peek() == '_')
        return ParseIdentifierOrCall();
      throw std::runtime_error("unexpected character in formula");
    }

    double ParseNumber()
    {
      const size_t start = mPos;
      while (mPos < mText.size() && (std::isdigit(static_cast<unsigned char>(mText[mPos])) || mText[mPos] == '.'))
        ++mPos;
      return std::stod(mText.substr(start, mPos - start));
    }

    double ParseIdentifierOrCall()
    {
      const size_t start = mPos;
      while (mPos < mText.size() && (std::isalnum(static_cast<unsigned char>(mText[mPos])) || mText[mPos] == '_'))
        ++mPos;
      const std::string ident = mText.substr(start, mPos - start);

      SkipSpaces();
      if (Peek() == '(')
      {
        Advance();
        std::vector<double> args;
        args.push_back(ParseExpr());
        SkipSpaces();
        while (Peek() == ',')
        {
          Advance();
          args.push_back(ParseExpr());
          SkipSpaces();
        }
        Expect(')');
        return CallFunction(ident, args);
      }

      return LookupVariable(ident);
    }

    double LookupVariable(const std::string& name) const
    {
      if (name == "k") return mVars.k;
      if (name == "depth") return mVars.depth;
      if (name == "n") return mVars.n;
      if (name == "seed") return mVars.seed;
      if (name == "pi") return 3.14159265358979323846;
      if (name == "phi") return 1.6180339887498949; // golden ratio, convenience constant for exactly this generator's own default
      // EXOMUSICA letter -> custom slot, in company-name order (not
      // alphabetical) -- E is slot 0, A is slot 8, matching the knob/
      // range order ExoEQ.cpp exposes. Deliberately a fixed lookup
      // table rather than arithmetic on the char, since the letters
      // aren't consecutive in the alphabet.
      if (name.size() == 1)
      {
        static const char kExomusicaLetters[kNumCustomVars] = { 'E', 'X', 'O', 'M', 'U', 'S', 'I', 'C', 'A' };
        for (int i = 0; i < kNumCustomVars; ++i)
          if (name[0] == kExomusicaLetters[i])
            return mVars.custom[i];
      }
      throw std::runtime_error("unknown variable '" + name + "' in formula");
    }

    double CallFunction(const std::string& name, const std::vector<double>& args) const
    {
      auto need = [&](size_t count) { if (args.size() != count) throw std::runtime_error("wrong argument count for '" + name + "'"); };
      if (name == "sin")  { need(1); return std::sin(args[0]); }
      if (name == "cos")  { need(1); return std::cos(args[0]); }
      if (name == "log")  { need(1); if (args[0] <= 0.0) throw std::runtime_error("log of non-positive value"); return std::log(args[0]); }
      if (name == "log2") { need(1); if (args[0] <= 0.0) throw std::runtime_error("log2 of non-positive value"); return std::log2(args[0]); }
      if (name == "exp")  { need(1); return std::exp(args[0]); }
      if (name == "abs")  { need(1); return std::fabs(args[0]); }
      if (name == "sqrt") { need(1); if (args[0] < 0.0) throw std::runtime_error("sqrt of negative value"); return std::sqrt(args[0]); }
      if (name == "pow")  { need(2); return std::pow(args[0], args[1]); }
      if (name == "min")  { need(2); return std::min(args[0], args[1]); }
      if (name == "max")  { need(2); return std::max(args[0], args[1]); }
      throw std::runtime_error("unknown function '" + name + "' in formula");
    }

    char Peek() const { return mPos < mText.size() ? mText[mPos] : '\0'; }
    void Advance() { ++mPos; }
    void SkipSpaces() { while (mPos < mText.size() && std::isspace(static_cast<unsigned char>(mText[mPos]))) ++mPos; }
    void Expect(char c)
    {
      if (Peek() != c) throw std::runtime_error(std::string("expected '") + c + "' in formula");
      Advance();
    }

    const std::string& mText;
    const Vars& mVars;
    size_t mPos = 0;

    friend double exoeq::ExprEval::Evaluate(const std::string&, const Vars&);
  };
};

// ---------------------------------------------------------------------

struct FractalGenParams
{
  double seedFreqHz = 220.0;
  double ratio = 1.6180339887498949; // golden ratio default; 2.0 gives a plain octave/harmonic stack instead
  int branchFactor = 2;              // children spawned per level, 2-4
  int depth = 3;                     // recursion levels, 1-4 (kept shallow -- see header note on the 128-band cap)
  double gainDecayPerLevel = 0.7;    // 0..1 multiplier applied to gain once per level
  double seedGainDb = 0.0;
  double resonance = 0.707;
  double jitterPct = 0.0;            // 0..100, +/- random deviation applied to each node's frequency
  unsigned int jitterSeed = 1;
  bool symmetric = false;            // mirror children above/below the seed in log-frequency space

  // Inline composer: when non-empty, OVERRIDES the ratio/branchFactor/
  // depth knobs above entirely -- this formula computes the frequency
  // MULTIPLIER (relative to seedFreqHz) for generated node index k,
  // using the k/depth/n/seed variables ExprEval exposes. Left empty, the
  // knob-driven IFS recursion below runs instead -- so a user who never
  // opens the formula box gets a fully knob-controlled generator with no
  // math required.
  std::string formula;

  // 9 user-defined knobs, one per letter of EXOMUSICA (order: E,X,O,M,U,
  // S,I,C,A -- matching ExprEval::Vars::custom's own order, see there).
  // Each already resolved to a real value (NOT a raw 0..1 knob position)
  // by the caller: customVarValue[i] = varMin[i] + knob01[i] *
  // (varMax[i] - varMin[i]). This struct only ever sees the final scaled
  // value -- the knob position and the min/max range it's scaled
  // against both live in ExoEQ (the knob is a real automatable param;
  // min/max are plain stored doubles set via inline text, same
  // mechanism as `formula` above), since neither belongs in this
  // one-shot-math-only file. A formula that never references any of
  // E/X/O/M/U/S/I/C/A simply ignores these -- there's no requirement to
  // use any of them.
  double customVarValue[ExprEval::kNumCustomVars] = { 0., 0., 0., 0., 0., 0., 0., 0., 0. };

  // Hard cap, not exposed as a param -- this is the safety bound the
  // "don't extend past Nyquist" requirement and the "depth stays
  // unwieldy past 4" doc note both ultimately rely on; see
  // GenerateFractalSeries's own clamp of the knob-driven node count
  // against this before any generation work happens.
  static constexpr int kMaxNodes = 64;
};

namespace detail
{
  // Simple deterministic LCG, not meant to be cryptographically anything
  // -- just reproducible per jitterSeed so regenerating with the same
  // seed gives the same jitter pattern (a real, if modest, usability
  // requirement: "I liked that jitter, give it to me again").
  inline double DeterministicJitter01(unsigned int seed, int index)
  {
    unsigned int x = seed * 2654435761u + static_cast<unsigned int>(index) * 40503u;
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return static_cast<double>(x) / static_cast<double>(0xffffffffu); // [0,1)
  }

  inline double ClampToSafeRange(double freqHz, double sampleRate)
  {
    constexpr double kNyquistGuard = 0.98; // 2% safety margin below true Nyquist -- see header note on why this isn't just "<=0.5"
    const double maxHz = sampleRate * 0.5 * kNyquistGuard;
    return std::clamp(freqHz, 20.0, maxHz);
  }
}

// Generates up to FractalGenParams::kMaxNodes nodes. sampleRate is
// required (not optional) specifically so the Nyquist clamp is correct
// for whatever rate the host is actually running at right now -- this
// generator is called fresh from a live "Generate" button click, never
// cached across a sample-rate change, so there's no stale-clamp risk the
// way there would be if this were precomputed at construction time.
inline std::vector<GeneratedNode> GenerateFractalSeries(const FractalGenParams& params, double sampleRate)
{
  std::vector<GeneratedNode> nodes;

  if (!params.formula.empty())
  {
    // Inline-composer path: evaluate the user's formula once per node,
    // for a fixed, modest node count (depth/branchFactor still bound how
    // many nodes a knob-driven run would have produced, so a formula
    // doesn't get to ask for more nodes than the knob path could -- the
    // formula controls SHAPE, not node count, keeping this predictable).
    const int nodeCount = std::clamp(params.branchFactor * std::max(1, params.depth), 1, FractalGenParams::kMaxNodes);
    for (int k = 0; k < nodeCount; ++k)
    {
      ExprEval::Vars vars;
      vars.k = static_cast<double>(k);
      vars.depth = static_cast<double>(k % std::max(1, params.depth));
      vars.n = static_cast<double>(nodeCount);
      vars.seed = static_cast<double>(params.jitterSeed);
      for (int i = 0; i < ExprEval::kNumCustomVars; ++i) vars.custom[i] = params.customVarValue[i];

      double multiplier;
      try
      {
        multiplier = ExprEval::Evaluate(params.formula, vars);
      }
      catch (const std::exception&)
      {
        // A malformed formula should degrade gracefully, not abort the
        // whole generate click or crash -- fall back to this node's
        // plain ratio^k position, same as the knob-driven default would
        // give, and keep going so the rest of the series still forms.
        multiplier = std::pow(params.ratio, static_cast<double>(k));
      }
      if (!std::isfinite(multiplier) || multiplier <= 0.0)
        multiplier = std::pow(params.ratio, static_cast<double>(k));

      GeneratedNode node;
      double freq = params.seedFreqHz * multiplier;
      if (params.jitterPct > 0.0)
      {
        const double j = (detail::DeterministicJitter01(params.jitterSeed, k) * 2.0 - 1.0) * (params.jitterPct / 100.0);
        freq *= (1.0 + j);
      }
      node.freqHz = detail::ClampToSafeRange(freq, sampleRate);
      node.gainDb = params.seedGainDb * std::pow(params.gainDecayPerLevel, vars.depth);
      node.resonance = params.resonance;
      nodes.push_back(node);
    }
    return nodes;
  }

  // Knob-driven IFS path (the default, zero-math-required mode): one
  // seed node, then branchFactor children per level, each level's
  // frequency scaled by `ratio` relative to its parent and gain scaled
  // by gainDecayPerLevel -- a direct, literal reading of "self-similar
  // structure repeated at a shrinking scale", which is what "fractal"
  // concretely means here (an IFS, not an actual infinite-fractal-
  // dimension object, which wouldn't make sense for a finite band count
  // anyway).
  const int branch = std::clamp(params.branchFactor, 1, 4);
  const int depth = std::clamp(params.depth, 1, 4);

  GeneratedNode seed;
  seed.freqHz = detail::ClampToSafeRange(params.seedFreqHz, sampleRate);
  seed.gainDb = params.seedGainDb;
  seed.resonance = params.resonance;
  nodes.push_back(seed);

  int nodeIndex = 1;
  for (int level = 1; level <= depth && static_cast<int>(nodes.size()) < FractalGenParams::kMaxNodes; ++level)
  {
    const double levelRatio = std::pow(params.ratio, static_cast<double>(level));
    const double levelGain = params.seedGainDb * std::pow(params.gainDecayPerLevel, static_cast<double>(level));

    for (int b = 0; b < branch && static_cast<int>(nodes.size()) < FractalGenParams::kMaxNodes; ++b)
    {
      // Spread this level's children across a small log-spaced fan
      // around levelRatio rather than stacking them all on exactly the
      // same frequency -- (b+1)/(branch+1) keeps every child distinct
      // and ordered without needing its own extra param.
      const double spread = static_cast<double>(b + 1) / static_cast<double>(branch + 1);
      double freq = params.seedFreqHz * levelRatio * (0.5 + spread);

      if (params.symmetric && (b % 2 == 1))
        freq = params.seedFreqHz * params.seedFreqHz / freq; // mirror in log-frequency space around the seed

      if (params.jitterPct > 0.0)
      {
        const double j = (detail::DeterministicJitter01(params.jitterSeed, nodeIndex) * 2.0 - 1.0) * (params.jitterPct / 100.0);
        freq *= (1.0 + j);
      }

      GeneratedNode node;
      node.freqHz = detail::ClampToSafeRange(freq, sampleRate);
      node.gainDb = levelGain;
      node.resonance = params.resonance;
      nodes.push_back(node);
      ++nodeIndex;
    }
  }

  return nodes;
}

} // namespace exoeq
