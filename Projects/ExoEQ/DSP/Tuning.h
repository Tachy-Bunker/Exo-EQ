#pragma once

#include <array>
#include <vector>
#include <string>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <algorithm>

// Tuning.h -- Phase 2: real microtonal tuning, replacing the hardcoded
// 12-TET assumption MidiNoteToHz (KeyTrack.h) has had since Phase 1.
//
// NOTE on scope: this parses Scala .scl files (the scale itself: a list of
// steps up to the repeat interval) and maps them onto the keyboard using
// Scala's convention of a fixed reference note -- it does NOT yet parse a
// companion .kbm keyboard-mapping file (which lets a .scl's root sit on
// any key, with non-uniform mapping, unused keys, etc.). That's a real gap
// if you load scales with custom .kbm files; plain .scl-only scales (the
// common case) map correctly. Flagging this now rather than quietly
// pretending full Scala support exists.
//
// Also deliberately anchored at MIDI 69 / A4 (440Hz by default), not
// Scala's own usual root of middle C (60) -- this keeps it consistent with
// MidiNoteToHz and every other A4=440 assumption already in this codebase,
// at the cost of diverging slightly from strict Scala convention. Worth
// reconsidering once/if a .kbm parser is added (a .kbm's own "reference
// key"/"reference frequency" fields are exactly what would replace this).

namespace exoeq
{

class TuningTable
{
public:
  static constexpr int kReferenceMidiNote = 69; // A4 -- see file header note on scope

  TuningTable() { ResetToDefault12Tet(); }

  void ResetToDefault12Tet()
  {
    for (int i = 0; i < 128; ++i)
      mCentsFromReference[i] = static_cast<double>(i - kReferenceMidiNote) * 100.0;
  }

  double NoteToHz(int midiNote, double referenceHz = 440.0) const
  {
    midiNote = std::clamp(midiNote, 0, 127);
    return referenceHz * std::pow(2.0, mCentsFromReference[midiNote] / 1200.0);
  }

  // Parses Scala .scl TEXT (not a path -- reading the file off disk is a
  // plugin-shell/host concern, this just needs the bytes). On success,
  // fills `out` and returns true; on a malformed scale, returns false and
  // leaves `out` untouched (callers should keep whatever tuning was active
  // rather than silently reset to 12-TET on a bad file).
  static bool LoadScala(const std::string& sclText, TuningTable& out)
  {
    std::vector<double> stepsCents; // cumulative cents from the scale's root; root itself (0 cents) is implicit
    std::istringstream stream(sclText);
    std::string line;
    int parseStage = 0; // 0=expecting description, 1=expecting note count, 2=reading steps
    int noteCountDeclared = 0;

    while (std::getline(stream, line))
    {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      size_t start = 0;
      while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        ++start;
      line = line.substr(start);
      if (line.empty() || line[0] == '!')
        continue; // comment or blank line

      if (parseStage == 0) { parseStage = 1; continue; } // description line, discarded
      if (parseStage == 1)
      {
        noteCountDeclared = std::atoi(line.c_str());
        parseStage = 2;
        continue;
      }

      const double cents = ParseScalaStep(line);
      if (!std::isfinite(cents))
        return false;
      stepsCents.push_back(cents);
    }

    if (noteCountDeclared <= 0 || static_cast<int>(stepsCents.size()) != noteCountDeclared)
      return false;

    const double periodCents = stepsCents.back(); // last step = the repeat interval (usually ~1200, but not required)
    if (periodCents <= 0.0)
      return false;

    const int stepsPerPeriod = static_cast<int>(stepsCents.size());
    std::array<double, 128> computed{};
    for (int midiNote = 0; midiNote < 128; ++midiNote)
    {
      const int degreeFromRoot = midiNote - kReferenceMidiNote;
      int period, stepIndex;
      if (degreeFromRoot >= 0)
      {
        period = degreeFromRoot / stepsPerPeriod;
        stepIndex = degreeFromRoot - period * stepsPerPeriod;
      }
      else
      {
        // Ceiling division so a partial octave below root still rounds to
        // a full extra period down, not truncated toward zero.
        const int n = -degreeFromRoot;
        period = -((n + stepsPerPeriod - 1) / stepsPerPeriod);
        stepIndex = degreeFromRoot - period * stepsPerPeriod;
      }
      const double cents = (stepIndex == 0) ? 0.0 : stepsCents[static_cast<size_t>(stepIndex) - 1];
      computed[static_cast<size_t>(midiNote)] = period * periodCents + cents;
    }

    out.mCentsFromReference = computed;
    return true;
  }

private:
  // A Scala step is either a ratio "n/d" (or a bare integer "n", meaning
  // n/1) or a cents value -- per the Scala spec, a token is a cents value
  // if and only if it contains a decimal point.
  static double ParseScalaStep(const std::string& token)
  {
    if (token.find('.') != std::string::npos)
      return std::atof(token.c_str());

    const size_t slashPos = token.find('/');
    double num, den;
    if (slashPos != std::string::npos)
    {
      num = std::atof(token.substr(0, slashPos).c_str());
      den = std::atof(token.substr(slashPos + 1).c_str());
    }
    else
    {
      num = std::atof(token.c_str());
      den = 1.0;
    }
    if (num <= 0.0 || den <= 0.0)
      return std::nan("");
    return 1200.0 * std::log2(num / den);
  }

  std::array<double, 128> mCentsFromReference{};
};

} // namespace exoeq
