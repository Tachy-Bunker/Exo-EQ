// STB_IMAGE_IMPLEMENTATION is deliberately NOT defined here, even though
// the comment that used to sit on this line argued for defining it in
// exactly one translation unit. Your linker errors (stbi_load,
// stbi_image_free, etc. "already defined in ExoEQ.obj", conflicting
// with IGraphicsWin.obj) prove iPlug2's own IGraphicsWin.cpp already
// defines STB_IMAGE_IMPLEMENTATION and compiles the real stb_image
// implementation in -- IGraphics itself uses stb_image internally for
// its own bitmap loading. Defining it again here gave the linker TWO
// implementations of every stbi_* function, which is exactly what
// LNK2005 means. Leaving it undefined here means DSP/ImageToEq.h's
// `#include "../Vendor/stb/stb_image.h"` only sees the DECLARATIONS
// (stb_image.h's normal header-only mode) -- ImageToEq.h's calls to
// stbi_load/stbi_image_free/etc. link against the one real
// implementation IGraphicsWin.obj already provides, instead of
// compiling a second, colliding copy.
#include "ExoEQ.h"
#include "FreqFormat.h"
#include "MorphSteps.h"
#include <array>
#include <chrono>
#include <complex>
#include <cstring>
#include <functional>
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "DSP/KeyTrack.h"
// BandGraphControl.h is deliberately NOT included any more. Nothing in this
// file has attached or referenced it since CircleViewControl replaced it, and
// it is a large header-only control class whose every method is implicitly
// inline -- pure weight on a translation unit that just hit MSVC's C1002
// "compiler is out of heap space". The file itself is still in the tree for
// rollback; only the include is gone.
#include "CircleViewControl.h"

// Conceptual-art pass: the advanced "lower drawer" backdrop -- a deep navy
// panel with a hairline top edge, drawn BEHIND the detail-panel knobs
// (attached right after circleView, so z-order puts it under them).
// Mouse-transparent so it never steals clicks from the controls on top.
// Concept 02 top bar: a flat toolbar backdrop plus plain text labels. Both
// are mouse-transparent and draw nothing interactive.
class ToolbarBackdrop final : public IControl
{
public:
  ToolbarBackdrop(const IRECT& bounds) : IControl(bounds, iplug::kNoParameter) { mIgnoreMouse = true; }
  void Draw(IGraphics& g) override
  {
    g.FillRect(IColor(255, 18, 24, 34), mRECT);
    g.FillRect(IColor(255, 40, 52, 70), IRECT(mRECT.L, mRECT.B - 1.f, mRECT.R, mRECT.B));
  }
};

class TextLabelControl final : public IControl
{
public:
  static constexpr float kUiTextScale = 1.4f; // matched to CircleViewControl's bump; Pro-Q's chrome text is much larger than ours was
  TextLabelControl(const IRECT& bounds, const char* text, float size, IColor color)
  : IControl(bounds, iplug::kNoParameter), mText(text), mSize(size * kUiTextScale), mColor(color)
  {
    mIgnoreMouse = true;
  }
  void Draw(IGraphics& g) override { g.DrawText(IText(mSize, mColor), mText, mRECT); }
private:
  const char* mText;
  float mSize;
  IColor mColor;
};

// Knob scale (per user request): diameter x2, clamped to the grid cell so
// neighbouring knobs don't overlap. Change kKnobScale to tune.
static constexpr float kKnobScale = 2.0f;
static float KnobSize(float base, const IRECT& cell)
{
  return std::min(base * kKnobScale, std::min(cell.W(), cell.H()) - 8.f);
}

// Reads a typed value: "440", "440 Hz", "2.5k", "2.5 kHz", "-3.5 dB", "50%".
// A trailing k/K multiplies by 1000; other unit text is ignored.
static bool ParseNumericEntry(const char* str, double& out)
{
  if (!str) return false;
  char* end = nullptr;
  const double v = std::strtod(str, &end);
  if (end == str) return false;
  while (*end == ' ') ++end;
  out = (*end == 'k' || *end == 'K') ? v * 1000.0 : v;
  return true;
}

// Knob for an int/enum parameter. It snaps to `steps` distinct positions, so the
// drawn position always matches a real value (no in-between positions).
// Note: snapping is applied in SetValue. If a mouse drag path skips SetValue in
// this iPlug2 version, the drag will still be continuous until the host echoes
// the value back; that path needs a check against the vendored SDK.
class ExoStepKnob : public IVKnobControl
{
public:
  ExoStepKnob(const IRECT& bounds, int paramIdx, const char* label, int steps)
  : IVKnobControl(bounds, paramIdx, label), mSteps(steps) {}

  // Steps can change at runtime (the morph knob's count depends on the filter type).
  void SetSteps(int steps) { mSteps = steps; }

  // For stepped knobs, the text box shows the step as a number: k + base, where k is
  // the 0-based step. Stage and partial counts use base 1 (1..N), levels and polarity 0.
  void SetIndexBase(int base) { mIndexBase = base; }

  // Clicking the value text under the knob opens a numeric entry (the value area
  // is the knob's own mValueBounds, shown by default via IVStyle::showValue). iPlug2's
  // entry parses with atof and would read "2.5k" as 2.5 Hz, so this path parses units
  // itself (ParseNumericEntry) and passes kNoValIdx so IGraphics calls
  // OnTextEntryCompletion instead of the param's own parser.
  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (GetStyle().showValue && GetValueBounds().Contains(x, y))
    {
      const IParam* p = GetParam(0);
      if (!p) return; // no parameter behind this knob: nothing to edit
      WDL_String cur;
      if (mSteps > 1)
      {
        const int k = std::clamp(static_cast<int>(std::lround(p->Value() / 100.0 * (mSteps - 1))), 0, mSteps - 1);
        cur.SetFormatted(32, "%d", k + mIndexBase);
      }
      else
      {
        cur.SetFormatted(32, "%.2f", p->Value());
      }
      GetUI()->CreateTextEntry(*this, DEFAULT_TEXT, GetValueBounds(), cur.Get(), kNoValIdx);
      return;
    }
    IVKnobControl::OnMouseDown(x, y, mod);
  }

  void OnTextEntryCompletion(const char* str, int /*valIdx*/) override
  {
    double v = 0.0;
    if (!ParseNumericEntry(str, v)) return; // unreadable entry: keep the current value
    const IParam* p = GetParam(0);
    if (!p) return;
    if (mSteps > 1)
    {
      // Typed value is the displayed count (k + base); convert back to the stored percent.
      const int k = std::clamp(static_cast<int>(std::lround(v)) - mIndexBase, 0, mSteps - 1);
      SetValueFromUserInput(p->ToNormalized(k * 100.0 / (mSteps - 1)), 0);
      return;
    }
    double lo = 0.0, hi = 1.0;
    p->GetBounds(lo, hi);
    v = std::clamp(v, lo, hi);
    SetValueFromUserInput(p->ToNormalized(v), 0);
  }

  void SetValue(double normalizedValue, int valIdx = 0) override
  {
    if (mSteps > 1)
    {
      const double n = static_cast<double>(mSteps - 1);
      normalizedValue = std::round(std::clamp(normalizedValue, 0.0, 1.0) * n) / n;
    }
    IVKnobControl::SetValue(normalizedValue, valIdx);
  }

  // T2-2: underline under the value text while the mouse is over it (cue for click-to-type).
  // T2-3: notch ticks around the ring for stepped knobs; the active step is brighter and longer.
  // T2-4: LP | BP | HP marker strip under the Morph knob. Only on, via SetStrip, for type 0.
  void SetStrip(bool on) { mStrip = on; }

  void Draw(IGraphics& g) override
  {
    IVKnobControl::Draw(g);
    if (GetStyle().showValue && mValueMouseOver)
    {
      const IRECT vb = GetValueBounds();
      const float half = vb.W() * 0.3f;
      const float y = vb.B - 1.f;
      g.DrawLine(IColor(220, 150, 200, 255), vb.MW() - half, y, vb.MW() + half, y, nullptr, 1.f);
    }
    if (mSteps > 1) DrawNotches(g);
    if (mStrip) DrawMorphStrip(g);
  }

private:
  void DrawNotches(IGraphics& g)
  {
    const float r = GetRadius();
    const float cx = mWidgetBounds.MW(), cy = mWidgetBounds.MH();
    const int n = mSteps;
    const int active = std::clamp(static_cast<int>(std::lround(GetValue() * (n - 1))), 0, n - 1);
    for (int i = 0; i < n; ++i)
    {
      const float t = static_cast<float>(i) / static_cast<float>(n - 1);
      const float deg = mAngle1 + t * (mAngle2 - mAngle1); // iPlug convention: 0 = top, clockwise
      const bool on = (i == active);
      const IColor col = on ? IColor(255, 235, 240, 245) : IColor(110, 120, 140, 160);
      g.DrawRadialLine(col, cx, cy, deg, r + 4.f, r + (on ? 8.f : 7.f), nullptr, on ? 2.f : 1.f);
    }
  }

  void DrawMorphStrip(IGraphics& g)
  {
    // LP / BP / HP ticks on the outer ring at 0, 50, 100 %. The name goes in the
    // knob label (UpdateStripLabel), so nothing is drawn under the value text.
    const float r = GetRadius();
    const float cx = mWidgetBounds.MW(), cy = mWidgetBounds.MH();
    const float angles[3] = { mAngle1, 0.5f * (mAngle1 + mAngle2), mAngle2 };
    for (float a : angles)
      g.DrawRadialLine(IColor(160, 120, 140, 160), cx, cy, a, r + 4.f, r + 7.f, nullptr, 1.f);
  }

  // Crash fix: IControl::SetDirty() calls SetValue() on the control, and SetLabelStr()
  // calls SetDirty(). Calling SetLabelStr from SetValue therefore recursed without end
  // (the "stack overflow" in the FL crash log). Only set the label when the name changes,
  // and never re-enter while already setting it.
  // Label-based stage name disabled: writing the label from the value path re-entered
  // SetDirty -> SetValue and overflowed the stack. Kept for reference, never called.
  void UpdateStripLabel()
  {
    if (!mStrip || mInStripLabel) return;
    const double v = GetValue();
    const char* name = v < 0.25 ? "Morph: LP" : (v < 0.75 ? "Morph: BP" : "Morph: HP");
    if (GetLabelStr() && std::strcmp(GetLabelStr(), name) == 0) return;
    mInStripLabel = true;
    SetLabelStr(name);
    mInStripLabel = false;
  }

  int mSteps = 0;
  int mIndexBase = 0;
  bool mStrip = false;
  bool mInStripLabel = false;
};

// Dirty Hardware Lab skin (Concept 02): dark body, blue-gray frame and arc,
// white pointer. Applied to every IVectorBase control (knobs, buttons, toggles,
// menus) once OnLayout has built them. Uses IVStyle::WithColor/WithLabelText/
// WithValueText, which are confirmed in the iPlug2 reference headers.
// Per-filter-type knob naming. "Cutoff/Reso/Morph/Extra" is wrong for most of
// the 45 types: a Comb reads as Frequency/Feedback, a Ring Modulator as
// Frequency/Depth, a resonator as Frequency/Decay. Each entry is
// {cutoff, reso, morph, extra} for one type ordinal, matching ExoEQ.cpp's
// InitEnum order. Anything not listed falls back to the generic set.
struct BandKnobNames { const char* cutoff; const char* reso; const char* morph; const char* extra; };

// Discrete position count for the per-band Morph knob, by filter-type ordinal.
// Derived from how each DSP type reads morph: a stage/partial/polarity count
// (floor-quantized or threshold) gets its real count; a continuous morph (crossfade,
// wet mix, pole slope, vowel interpolation, shape) returns 0 = stays continuous.
// MorphStepsForType now lives in MorphSteps.h (shared with the circle view).

// Number shown in the Morph knob's text box for stepped types: stage and partial
// counts start at 1 (matching the readout), levels and polarity start at 0.
static int MorphIndexBaseForType(int ordinal)
{
  switch (ordinal)
  {
    case 14: case 15: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24:
    case 41: case 42: case 43: return 1;
    default: return 0;
  }
}

// Readout text for the Morph knob. `percent` is the stored 0-100 value. For stepped
// types it shows the step the DSP actually uses (stage count, polarity, partials),
// so the readout snaps the same way the knob does.
static void FormatMorphReadout(int ordinal, double percent, WDL_String& out, int precision)
{
  const int n = MorphStepsForType(ordinal);
  char buf[64];
  if (n <= 1)
  {
    std::snprintf(buf, sizeof(buf), "%.*f%%", precision, percent);
  }
  else
  {
    const int k = std::clamp(static_cast<int>(std::lround(percent / 100.0 * (n - 1))), 0, n - 1);
    switch (ordinal)
    {
      case 36: case 37: std::snprintf(buf, sizeof(buf), "%s", k == 0 ? "+" : "-"); break;      // comb polarity
      case 41:          std::snprintf(buf, sizeof(buf), "%d stages", k + 1); break;           // phaser
      case 42: case 43: std::snprintf(buf, sizeof(buf), "%d partials", k + 1); break;         // phys resonators
      case 35:          std::snprintf(buf, sizeof(buf), "level %d", k); break;                // slewsonic
      case 17: case 18: case 19:
                        std::snprintf(buf, sizeof(buf), "%d stage%s", k + 1, k == 0 ? "" : "s"); break;
      default:          std::snprintf(buf, sizeof(buf), "%d stages", k + 1); break;           // AW 1-4 stage family
    }
  }
  out.Set(buf);
}


static BandKnobNames KnobNamesForType(int ordinal)
{
  switch (ordinal)
  {
    // Labels for ordinals 0-35 are from the sweep in the batch-5 audit: a knob
    // reads "n/a" when changing it does not change the sound for that type, and
    // ExoEQ hides n/a knobs. Ordinals 36+ keep the defaults below.
    case 0:  return { "Freq",   "Reso",     "Morph",    "n/a"    }; // T1-5: was "LP-BP-HP"; stage name moves to the morph caption (T2-4)
    case 1:  return { "Cutoff", "n/a",      "n/a",      "n/a"    }; // LP6: one-pole
    case 2: case 3: case 4:
             return { "Cutoff", "Reso",     "n/a",      "n/a"    }; // LP12/24/48
    case 5:  return { "Cutoff", "n/a",      "n/a",      "n/a"    }; // HP6: one-pole
    case 6: case 7: case 8:
             return { "Cutoff", "Reso",     "n/a",      "n/a"    }; // HP12/24/48
    case 9: case 10:
             return { "Center", "Width",    "n/a",      "n/a"    }; // BP12/24
    case 11: return { "Center", "Depth",    "n/a",      "n/a"    }; // Notch12
    case 12: return { "Freq",   "Charge",   "Morph",    "n/a"    }; // Capacitor
    case 13: return { "Freq",   "Lift",     "Morph",    "n/a"    }; // Air Lift
    case 14: case 15: case 18: case 19: case 22: case 23: case 24: case 25:
             return { "Cutoff", "Reso",     "Morph",    "n/a"    }; // AW Lowpass2/Highpass2, Z-BP/Notch, X-BP/Notch, Holt, Biquad Stack
    case 16: return { "Cutoff", "Reso",     "Stages",   "n/a"    }; // AW Z-Lowpass: morph = 1->2 stage crossfade
    case 17: return { "Cutoff", "n/a",      "Morph",    "n/a"    }; // AW Z-Highpass: reso hardcoded in source
    case 20: return { "Cutoff", "Reso",     "Morph",    "In Gain"}; // AW X-Lowpass: extra = input gain (0.1 = unity)
    case 21: return { "Cutoff", "n/a",      "Morph",    "n/a"    }; // AW X-Highpass: reso does not reach the filter
    case 26: case 27: case 28: case 29:
             return { "Cutoff", "Reso",     "Dry",      "Curve"  }; // AW Y family: morph = dry mix (0 = full filter), extra = companding curve
    case 30: case 31:
             return { "Cutoff", "Reso",     "Wet",      "NonLin" }; // AW BiquadNonLin: morph = wet, extra = nonlin shaping
    case 33: return { "Cutoff", "n/a",      "Slope",    "n/a"    }; // AW Hermepass: morph = pole slope; reso and extra do nothing
    case 34: return { "Cutoff", "Drive",    "Wet",      "n/a"    }; // AW AverMatrix: reso = drive/poles, morph = bipolar wet (default 0 = wet -1)
    case 35: return { "Cutoff", "n/a",      "Stages",   "n/a"    }; // AW SlewSonic: morph = stage count; reso fixed by design
    // Ordinals 36-44 (InitEnum list index minus one). Extra does nothing for any of
    // these types in the sweep, so it reads n/a and is hidden.
    case 36: return { "Freq",   "Depth",    "Polarity", "n/a"    }; // Comb Feedforward
    case 37: return { "Freq",   "Feedback", "Polarity", "n/a"    }; // Comb Feedback
    case 38: return { "Freq",   "Depth",    "Shape",    "n/a"    }; // Ring Modulator
    case 39: return { "Carrier","Index",    "Shape",    "n/a"    }; // Filter FM
    case 40: return { "Formant","Sharpness","Vowel",    "n/a"    }; // Vocal Formant
    case 41: return { "Freq",   "Feedback", "Stages",   "n/a"    }; // Phaser
    case 42: return { "Freq",   "Decay",    "Partials", "n/a"    }; // Phys Odd Resonator
    case 43: return { "Freq",   "Decay",    "Partials", "n/a"    }; // Phys Even Resonator
    case 44: return { "Rate",   "Bits",     "Smooth",   "n/a"    }; // Resample
    default: return { "Cutoff", "Reso",     "Morph",    "Extra"  };
  }
}

// FIXED THIS ROUND -- knobs that were retargeted to a different band
// (SetParamIdx) kept drawing the OLD band's value: the param value was
// updated, but nothing told the control to re-read it. Retargeting now
// always pushes the new param's current normalized value into the control
// and marks it dirty, so the drawn knob matches the number beside it.
static void RetargetControl(iplug::igraphics::IControl* c, int paramIdx)
{
  if (!c) return;
  c->SetParamIdx(paramIdx);
  if (paramIdx < 0) return;
  if (auto* d = c->GetDelegate())
    if (auto* p = d->GetParam(paramIdx))
      c->SetValueFromDelegate(p->GetNormalized());
  c->SetDirty(false);
}

static IVStyle ExoSkinStyle()
{
  return IVStyle()
    .WithColor(kBG, IColor(0, 0, 0, 0))
    .WithColor(kFG, IColor(255, 44, 52, 66))
    .WithColor(kPR, IColor(255, 64, 78, 100))
    .WithColor(kFR, IColor(255, 86, 106, 136))
    .WithColor(kHL, IColor(255, 130, 160, 200))
    .WithColor(kSH, IColor(140, 0, 0, 0))
    .WithColor(kX1, IColor(255, 110, 140, 182))
    .WithColor(kX2, IColor(255, 226, 234, 244))
    .WithColor(kX3, IColor(255, 24, 30, 42))
    .WithLabelText(IText(19.f, IColor(255, 178, 198, 222)))
    .WithValueText(IText(16.f, IColor(255, 226, 234, 244)))
    .WithRoundness(0.25f)
    .WithDrawShadows(false);
}

class DrawerBackdrop final : public IControl
{
public:
  DrawerBackdrop(const IRECT& bounds) : IControl(bounds, iplug::kNoParameter)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    g.FillRect(IColor(240, 14, 22, 40), mRECT);
    g.FillRect(IColor(255, 60, 80, 110), IRECT(mRECT.L, mRECT.T, mRECT.R, mRECT.T + 1.f));
  }
};
#include <array>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>

namespace
{
  // Fixes a real build error: pGraphics->CreateTextEntry(...) in this
  // iPlug2 version does NOT take a completion lambda as its 5th argument
  // (that argument is an int -- a max-length, per the compiler's own
  // error showing the real signature) -- completion is delivered via
  // IControl::OnTextEntryCompletion(), called BY IGraphics on whichever
  // control CreateTextEntry's first argument named. The formula/range
  // buttons this project already had (IVButtonControl) don't override
  // that virtual, so this tiny invisible proxy control exists purely to
  // receive that callback and forward it to an ordinary std::function --
  // one of these gets attached (hidden, zero visual footprint) per text
  // entry, same "small, this-is-exactly-what-it's-for" shape as every
  // other UI-only helper control in this project.
  class TextEntryCallbackControl : public iplug::igraphics::IControl
  {
  public:
    TextEntryCallbackControl(std::function<void(const char*)> onComplete)
    : IControl(iplug::igraphics::IRECT(), iplug::kNoParameter)
    , mOnComplete(std::move(onComplete))
    {
      mIgnoreMouse = true;
      mHide = true;
    }
    void Draw(iplug::igraphics::IGraphics& /*g*/) override {}
    void OnTextEntryCompletion(const char* str, int /*valIdx*/) override
    {
      if (mOnComplete) mOnComplete(str);
    }
  private:
    std::function<void(const char*)> mOnComplete;
  };
}

// T3-2: morph stage preview. The selected band's own response at each stepped morph stage,
// as thin vector lines (no bitmap cache to build). The active stage is brighter. Continuous types draw nothing.
// It only repaints when one of its band's params changes (see Signature).
class MorphSparkline : public IControl
{
public:
  MorphSparkline(const IRECT& bounds, std::function<int()> bandOf, double sampleRate)
  : IControl(bounds), mBandOf(std::move(bandOf)), mSampleRate(sampleRate) { mIgnoreMouse = true; }

  bool IsDirty() override
  {
    // Redraw only once the band's params have SETTLED. While a drag is moving them every refresh
    // sees a new signature, so the preview stays put (no table rebuilds mid-drag) and it updates
    // about 150 ms after the drag ends. Nothing here needs to know about mouse state.
    const std::array<double, 7> sig = Signature();
    const auto now = std::chrono::steady_clock::now();
    if (sig != mPendingSig)
    {
      mPendingSig = sig;
      mPendingSince = now;
      return IControl::IsDirty();
    }
    if (sig == mSig) return IControl::IsDirty();
    if (now - mPendingSince < std::chrono::milliseconds(150)) return false;
    mSig = sig;
    return true;
  }

  void Draw(IGraphics& g) override
  {
    using namespace iplug::igraphics;
    g.FillRect(IColor(255, 14, 14, 20), mRECT);
    const int b = mBandOf();
    if (b < 0 || b >= kMaxBands || mSampleRate <= 0.0) return;
    const int ordinal = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value());
    const int stages = MorphStepsForType(ordinal);
    const bool stepped = stages >= 2;
    const double morph = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph))->Value() / 100.0;
    // Stepped types: one curve per stage, the active one brighter. Continuous types: one curve,
    // the band's own response at its current morph (it is the active curve).
    const int drawStages = stepped ? stages : 1;
    const int activeStage = stepped ? std::clamp(static_cast<int>(std::lround(morph * (stages - 1))), 0, stages - 1) : 0;
    const double cutoff = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
    const double reso = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance))->Value();
    const double extra = GetDelegate()->GetParam(BandParamIdx(b, kBandFilterExtra))->Value() / 100.0;
    const auto freqs = exoeq::MakeLogFreqGrid(20.0, 20000.0, kPoints);
    const float midY = (mRECT.T + mRECT.B) * 0.5f;
    const float halfH = mRECT.H() * 0.5f;
    for (int k = 0; k < drawStages; ++k)
    {
      const double stageMorph = stepped ? static_cast<double>(k) / (stages - 1) : morph;
      exoeq::FilterParams fp{ cutoff, reso, stageMorph, static_cast<exoeq::FilterTypeId>(ordinal), extra };
      std::vector<std::complex<double>> h;
      exoeq::BuildResponseTable(fp, mSampleRate, freqs, h);
      const bool active = stepped ? (k == activeStage) : true;
      const IColor col = active ? IColor(255, 235, 240, 255) : IColor(80, 150, 165, 200);
      float px = mRECT.L, py = midY;
      for (int i = 0; i < kPoints; ++i)
      {
        const float db = std::clamp(static_cast<float>(exoeq::MagnitudeToDb(h[i])), -18.f, 18.f);
        const float x = mRECT.L + mRECT.W() * static_cast<float>(i) / (kPoints - 1);
        const float y = midY - halfH * db / 18.f;
        if (i > 0) g.DrawLine(col, px, py, x, y, nullptr, active ? 1.6f : 1.f);
        px = x;
        py = y;
      }
    }
  }

private:
  static constexpr int kPoints = 32;

  std::array<double, 7> Signature()
  {
    std::array<double, 7> s{};
    const int b = mBandOf();
    s[0] = b;
    if (b < 0 || b >= kMaxBands) return s;
    s[1] = GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value();
    s[2] = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph))->Value();
    s[3] = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
    s[4] = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance))->Value();
    s[5] = GetDelegate()->GetParam(BandParamIdx(b, kBandFilterExtra))->Value();
    s[6] = mSampleRate;
    return s;
  }

  std::function<int()> mBandOf;
  double mSampleRate;
  std::array<double, 7> mSig{};
  std::array<double, 7> mPendingSig{};
  std::chrono::steady_clock::time_point mPendingSince{};
};

ExoEQ::ExoEQ(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // 128 bands' worth of real, individually automatable params, generated
  // in a loop rather than hand-enumerated -- see ExoEQ.h for why (can't
  // drift out of sync the way separate per-band constants would at this
  // scale). EVERY band starts inactive -- a freshly loaded instance has
  // no bands at all until the user makes one (click-empty-space on the
  // graph, or a future "+" control); see ProcessBlock below for why that
  // means pure passthrough, not silence.
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    const std::string suffix = std::to_string(bandIndex + 1);
    GetParam(BandParamIdx(bandIndex, kBandCutoff))->InitDouble(("Cutoff " + suffix).c_str(), 1000., 20., 20000.0, 0.01, "", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
    // T2-1: one Hz/kHz format everywhere (see FreqFormat.h).
    GetParam(BandParamIdx(bandIndex, kBandCutoff))->SetDisplayFunc([](double v, WDL_String& out) {
      char buf[32];
      FormatHzReadout(v, buf, sizeof(buf));
      out.Set(buf);
    });
    GetParam(BandParamIdx(bandIndex, kBandResonance))->InitDouble(("Resonance " + suffix).c_str(), 0.707, 0.05, 20.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandMorph))->InitDouble(("Morph " + suffix).c_str(), 0., 0., 100.0, 0.01, ""); // 0=LP 50=BP 100=HP (MorphZ type only -- repurposed per-type for Capacitor/AirLift, see FilterTypes.h)
    // Readout follows the filter type's real step (see FormatMorphReadout).
    GetParam(BandParamIdx(bandIndex, kBandMorph))->SetDisplayFunc([this, bandIndex](double v, WDL_String& out) {
      const IParam* typeParam = GetParam(BandParamIdx(bandIndex, kBandFilterType));
      const int type = typeParam ? static_cast<int>(typeParam->Value()) : 0;
      const IParam* morph = GetParam(BandParamIdx(bandIndex, kBandMorph));
    FormatMorphReadout(type, v, out, morph ? morph->GetDisplayPrecision() : 0);
    });
    GetParam(BandParamIdx(bandIndex, kBandGainDb))->InitDouble(("Gain " + suffix).c_str(), 0., -24., 24.0, 0.01, "dB"); // previously-missing per-band trim -- see ExoEQ.h's note on kBandGainDb
    GetParam(BandParamIdx(bandIndex, kBandFilterExtra))->InitDouble(("Filter Extra " + suffix).c_str(), 10., 0., 100.0, 0.01, "%"); // only AW Y-Lowpass uses this (powFactor) -- 10% default is this type's own neutral position (see AirwindowsYLowpassFilter); a no-op for every other type

    // Order MUST exactly match FilterTypeId in DSP/FilterCore.h -- that
    // enum is the single source of truth ReadBandSettings casts this
    // param's integer value back into, so a mismatch here silently picks
    // the wrong filter type rather than failing to build.
    GetParam(BandParamIdx(bandIndex, kBandFilterType))->InitEnum(("Filter Type " + suffix).c_str(), 0, {
      "Morph",
      "Lowpass 6",  "Lowpass 12",  "Lowpass 24",  "Lowpass 48",
      "Highpass 6", "Highpass 12", "Highpass 24", "Highpass 48",
      "Bandpass 12", "Bandpass 24",
      "Notch 12",
      "Capacitor",
      "Air Lift",
      "AW Lowpass 2",
      "AW Highpass 2",
      "AW Z-Lowpass",
      "AW Z-Highpass",
      "AW Z-Bandpass",
      "AW Z-Notch",
      "AW X-Lowpass",
      "AW X-Highpass",
      "AW X-Bandpass",
      "AW X-Notch",
      "AW Holt Lowpass",
      "AW Biquad Stack",
      "AW Y-Lowpass",
      "AW Y-Highpass",
      "AW Y-Bandpass",
      "AW Y-Notch",
      "AW Biquad NonLin Lowpass",
      "AW Biquad NonLin Bandpass",
      "AW Z-Acid Lowpass",
      "AW Hermepass Highpass",
      "AW AverMatrix",
      "AW SlewSonic",
      "Comb Feedforward",
      "Comb Feedback",
      "Ring Modulator",
      "Filter FM",
      "Vocal Formant",
      "Phaser",
      "Phys Odd Resonator",
      "Phys Even Resonator",
      "Resample"
    });

    GetParam(BandParamIdx(bandIndex, kBandKeyTrackAmt))->InitDouble(("KeyTrack " + suffix).c_str(), 0., 0., 100.0, 0.01, "%");
    GetParam(BandParamIdx(bandIndex, kBandMidiChannel))->InitEnum(("MIDI Channel " + suffix).c_str(), 0,
      { "All", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16" });
    GetParam(BandParamIdx(bandIndex, kBandMute))->InitBool(("Mute " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandSolo))->InitBool(("Delta " + suffix).c_str(), false); // "Delta" = solo, per the spec's naming
    GetParam(BandParamIdx(bandIndex, kBandActive))->InitBool(("Active " + suffix).c_str(), false);

    // Sends + sidechain: per-node contained functions, per the spec. All
    // default OFF/neutral -- enabling one is an explicit user action, same
    // "off by default" philosophy as the band itself (kBandActive above).
    GetParam(BandParamIdx(bandIndex, kBandDelayEnabled))->InitBool(("Delay On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandDelayTimeMs))->InitDouble(("Delay Time " + suffix).c_str(), 250., 1., 2000.0, 0.01, "ms");
    GetParam(BandParamIdx(bandIndex, kBandDelayFeedback))->InitDouble(("Delay FB " + suffix).c_str(), 0.3, 0., 0.98, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandDelayMix))->InitDouble(("Delay Mix " + suffix).c_str(), 0.3, 0., 1.0, 0.001, "");

    GetParam(BandParamIdx(bandIndex, kBandDistortionEnabled))->InitBool(("Dist On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandDistortionDrive))->InitDouble(("Dist Drive " + suffix).c_str(), 1.0, 0.1, 20.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandDistortionCharacter))->InitDouble(("Dist Char " + suffix).c_str(), 0., 0., 1.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandDistortionMix))->InitDouble(("Dist Mix " + suffix).c_str(), 0.3, 0., 1.0, 0.001, "");

    GetParam(BandParamIdx(bandIndex, kBandFreqShiftEnabled))->InitBool(("Shift On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandFreqShiftHz))->InitDouble(("Shift Hz " + suffix).c_str(), 0., -5000., 5000.0, 0.01, "Hz");
    GetParam(BandParamIdx(bandIndex, kBandFreqShiftMix))->InitDouble(("Shift Mix " + suffix).c_str(), 0.3, 0., 1.0, 0.001, "");

    GetParam(BandParamIdx(bandIndex, kBandSidechainEnabled))->InitBool(("SC On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandSidechainModOctaves))->InitDouble(("SC Depth " + suffix).c_str(), 0., -8., 8.0, 0.001, "oct");

    // Effect order slots: integer 0/1/2, defaults reproduce the original
    // fixed freq-shift->distortion->delay order exactly (see BandChain.h
    // for the tie-break rule and why that makes this a safe default).
    GetParam(BandParamIdx(bandIndex, kBandFreqShiftSlot))->InitInt(("Shift Slot " + suffix).c_str(), 0, 0, 2);
    GetParam(BandParamIdx(bandIndex, kBandDistortionSlot))->InitInt(("Dist Slot " + suffix).c_str(), 1, 0, 2);
    GetParam(BandParamIdx(bandIndex, kBandDelaySlot))->InitInt(("Delay Slot " + suffix).c_str(), 2, 0, 2);

    // Energy2 (DSP/AirwindowsFilters.h / DSP/BandChain.h -- see ExoEQ.h's
    // enum comment for the globally-capped-at-20 design). Off by default,
    // same philosophy as every other send above; the two grouped knobs
    // and mix default to neutral/off (0.5/0.5/0 -- see BandSettings'
    // own defaults) so an enabled-but-untouched band is silent until the
    // user actually pushes a knob off-center.
    GetParam(BandParamIdx(bandIndex, kBandEnergy2Enabled))->InitBool(("Energy2 On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandEnergy2GlitterHiss))->InitDouble(("Energy2 GlitterHiss " + suffix).c_str(), 0.5, 0., 1.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandEnergy2RatScrapeFizz))->InitDouble(("Energy2 RatScrapeFizz " + suffix).c_str(), 0.5, 0., 1.0, 0.001, "");
    GetParam(BandParamIdx(bandIndex, kBandEnergy2Mix))->InitDouble(("Energy2 Mix " + suffix).c_str(), 0., -1.0, 1.0, 0.001, "");

    // Node groups (DSP/BandGroups.h, DSP/BandChain.h -- built and tested in
    // an earlier session, these 3 params are the entire piece that was
    // actually missing). GroupMask default 0 = member of no group, exactly
    // the "doesn't change behavior until a user opts in" default every
    // other feature here uses.
    GetParam(BandParamIdx(bandIndex, kBandGroupMask))->InitInt(("Group Mask " + suffix).c_str(), 0, 0, 255);
    GetParam(BandParamIdx(bandIndex, kBandGroupEnvEnabled))->InitBool(("Group Env On " + suffix).c_str(), false);
    GetParam(BandParamIdx(bandIndex, kBandGroupEnvModOctaves))->InitDouble(("Group Env Depth " + suffix).c_str(), 0., -8., 8.0, 0.001, "oct");
    // Source tag -- see ExoEQ.h's own note on kBandSource/EBandSource.
    // Order MUST match EBandSource exactly (HandBuilt=0, Fractal,
    // Transform, Image). Every band starts HandBuilt, same as every
    // existing project loading this update for the first time -- a
    // save from before this param existed has nothing to read here, so
    // it defaults to HandBuilt for every band, which is simply correct
    // (no generator had tagged anything before this param existed).
    GetParam(BandParamIdx(bandIndex, kBandSource))->InitEnum(("Source " + suffix).c_str(), 0, { "Hand-Built", "Fractal", "Transform", "Image" });

    // Circle assignment (DSP/CircleChain.h) -- defaults to 0, so every
    // band starts in the one implicit circle every existing project
    // already effectively has. kMaxCircles matches kMaxBands (256) by
    // convention, not necessity -- a circle just needs an ID in range;
    // there's no reason to allow MORE circles than there are bands to
    // put in them.
    GetParam(BandParamIdx(bandIndex, kBandCircleIndex))->InitInt(("Circle " + suffix).c_str(), 0, 0, exoeq::kMaxCircles - 1);
  }

  // All kMaxBands Band objects get added up front, at construction --
  // BandChain already costs almost nothing per inactive band
  // (Band::ProcessSample returns immediately on mute, before touching the
  // filter or any send) -- but they MUST start muted here, explicitly.
  // This was the actual bug behind "amplitude increases into oblivion
  // without touching any parameter": ProcessBlock's performance
  // optimization skips calling SetBandSettings on a band that's already
  // inactive (to avoid recomputing filter coefficients for slots nobody's
  // using), which means a band that's NEVER been activated never gets its
  // mute flag pushed at all -- it just sits at whatever AddBand()
  // constructed it with. BandSettings{}'s own default is mute=false, so
  // every one of the 127+ never-touched bands was a fully live, identical
  // 1kHz low-pass filter, all summing in phase -- ~128x amplitude in the
  // passband, exactly matching the reported symptom. Passing mute=true
  // here fixes the actual bug at its source, independent of the
  // ProcessBlock optimization.
  {
    exoeq::BandSettings initiallyMuted;
    initiallyMuted.mute = true;
    for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
    {
      // Every band starts assigned to circle 0 (matching kBandCircleIndex's
      // own default above) -- CircleChain::AssignBandToCircle both creates
      // the slot (first call for this bandIndex, so there's nothing to
      // move away from) and sets its initial settings in one call, same
      // as the old mChain[c].AddBand(initiallyMuted) this replaces.
      mCircleChain[0].AssignBandToCircle(bandIndex, 0, initiallyMuted);
      mCircleChain[1].AssignBandToCircle(bandIndex, 0, initiallyMuted);
    }
  }

  // See ExoEQ.h for why latch=true/portamento=0ms here exactly reproduces
  // the old mLastMidiNote behavior -- this is not a new audible default.
  mKeyTracker.SetLatch(true);
  mKeyTracker.SetPortamentoMs(0.0);
  mMultiChannelKeyTracker.SetLatch(true);
  mMultiChannelKeyTracker.SetPortamentoMs(0.0);

  // Real MTS-ESP (ExoEQ.h's mMtsEspClient) -- handed to the KeyTracker
  // once here; the global kGlobalMtsEspEnabled param (read every block
  // in ProcessBlock, see below) is what actually flips SetUseMtsEsp on,
  // so this alone changes nothing yet. Default param value is false
  // (see InitBool below), so an existing project opens with identical
  // behavior to before this feature existed.
  mKeyTracker.SetMtsEspClient(&mMtsEspClient);
  mMultiChannelKeyTracker.SetMtsEspClient(&mMtsEspClient);

  // Prefer MTS-ESP when a master is already present at construction time;
  // otherwise retain the normal MIDI/local-tuning default. Host-restored
  // project state still overrides this initial value during state load.
  GetParam(kGlobalMtsEspEnabled)->InitBool("MTS-ESP Enabled", mMtsEspClient.HasMaster());

  // UI-only selectors -- never read in ProcessBlock, purely which
  // group/connection the one fixed panel below is currently showing.
  GetParam(kGlobalGroupSelect)->InitInt("Group Select", 0, 0, exoeq::kNumGroups - 1);
  GetParam(kGlobalModConnSelect)->InitInt("Mod Conn Select", 0, 0, kMaxModConnections - 1);

  // Groups: all 8 start unmuted/0dB/no envelope, i.e. completely inert --
  // matches every band's own groupMask default of 0 (member of nothing),
  // so this feature changes nothing for an existing project either way.
  for (int g = 0; g < exoeq::kNumGroups; ++g)
  {
    const std::string gSuffix = std::to_string(g + 1);
    GetParam(GroupParamIdx(g, kGroupMute))->InitBool(("Group Mute " + gSuffix).c_str(), false);
    GetParam(GroupParamIdx(g, kGroupGainDb))->InitDouble(("Group Gain " + gSuffix).c_str(), 0., -24., 24.0, 0.01, "dB");
  }

  // 4 LFOs feeding the mod matrix's first 4 source slots (of
  // ModMatrix::kMaxSources=8 -- the other 4 stay at their default value of
  // 0.0 forever, reserved for a future second source type e.g. envelope
  // followers, same scope-limiting choice already made for sidechain's
  // fixed envelope params). Waveform enum order MUST match
  // DSP/LFO.h's LfoWaveform exactly.
  for (int i = 0; i < kMaxModSources; ++i)
  {
    const std::string lSuffix = std::to_string(i + 1);
    GetParam(LfoParamIdx(i, kLfoRateHz))->InitDouble(("LFO Rate " + lSuffix).c_str(), 1., 0.01, 20.0, 0.001, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
    GetParam(LfoParamIdx(i, kLfoWaveform))->InitEnum(("LFO Wave " + lSuffix).c_str(), 0, { "Sine", "Triangle", "Saw", "Square", "Sample & Hold" });
  }

  // Mod connections: each a (source, destination, depth, target band)
  // tuple, individually enabled -- all start disabled, so this is a pure
  // no-op bank of extra params until a user builds a patch. Destination
  // enum order MUST match DSP/ModMatrix.h's ModDestination exactly.
  // TargetBand defaults to 0 (band 1) rather than anything more "helpful"
  // -- there's no sane default target, and Enabled=false means it's moot
  // either way.
  for (int i = 0; i < kMaxModConnections; ++i)
  {
    const std::string cSuffix = std::to_string(i + 1);
    GetParam(ModConnParamIdx(i, kModConnEnabled))->InitBool(("Mod Conn On " + cSuffix).c_str(), false);
    GetParam(ModConnParamIdx(i, kModConnSourceIndex))->InitInt(("Mod Conn Src " + cSuffix).c_str(), 0, 0, exoeq::ModMatrix::kMaxSources - 1);
    GetParam(ModConnParamIdx(i, kModConnDestination))->InitEnum(("Mod Conn Dest " + cSuffix).c_str(), 0, {
      "Cutoff (oct)",
      "Resonance +",
      "Morph +",
      "Gain (dB)",
      "Dist Drive +",
      "Freq Shift (Hz) +"
    });
    GetParam(ModConnParamIdx(i, kModConnDepth))->InitDouble(("Mod Conn Depth " + cSuffix).c_str(), 0., -8., 8.0, 0.001, "");
    GetParam(ModConnParamIdx(i, kModConnTargetBand))->InitInt(("Mod Conn Target " + cSuffix).c_str(), 0, 0, kMaxBands - 1);
  }

  // Fractal generator (P5 scope step 1 -- DSP/FractalGenerator.h). All
  // one-shot batch params, read only when "Generate" is clicked -- see
  // ExoEQ.h's note on kGlobalFractalFilterType. Defaults reproduce
  // FractalGenParams{}'s own defaults exactly so a freshly-opened
  // instance's Generate button, clicked with zero knob-turning, gives
  // the same result the standalone DSP test file's "knob-driven default"
  // checks against.
  GetParam(kGlobalFractalSeedHz)->InitDouble("Fractal Seed Hz", 220., 20., 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalFractalRatio)->InitDouble("Fractal Ratio", 1.6180339887498949, 1.01, 8.0, 0.0001, ""); // golden ratio default; 2.0 = plain octave stack
  GetParam(kGlobalFractalBranchFactor)->InitInt("Fractal Branch", 2, 1, 4);
  GetParam(kGlobalFractalDepth)->InitInt("Fractal Depth", 3, 1, 4); // kept shallow -- see FractalGenerator.h's own note on the 128-band cap
  GetParam(kGlobalFractalGainDb)->InitDouble("Fractal Seed Gain", 0., -24., 24.0, 0.01, "dB");
  GetParam(kGlobalFractalGainDecay)->InitDouble("Fractal Gain Decay", 0.7, 0., 1.0, 0.001, "");
  GetParam(kGlobalFractalResonance)->InitDouble("Fractal Resonance", 0.707, 0.05, 20.0, 0.001, "");
  GetParam(kGlobalFractalJitterPct)->InitDouble("Fractal Jitter", 0., 0., 100.0, 0.01, "%");
  GetParam(kGlobalFractalJitterSeed)->InitInt("Fractal Jitter Seed", 1, 0, 999999);
  GetParam(kGlobalFractalSymmetric)->InitBool("Fractal Symmetric", false);
  // Deliberately only 4 options, ALL single-StateVariableFilter dropdown
  // types -- the user's explicit "cheap filter system" requirement.
  // Never an Airwindows/character type here, by design: see
  // GenerateFractalBands()'s own mapping comment for the FilterTypeId
  // ordinal each option actually writes.
  GetParam(kGlobalFractalFilterType)->InitEnum("Fractal Filter Type", 2, { "Lowpass 12", "Highpass 12", "Bandpass 12", "Notch 12" });

  // 9 user-defined knobs (EXOMUSICA letters) the inline formula box can reference by
  // letter. Each is a plain, real automatable 0..1 param -- the range
  // it's scaled into (mFractalVarMin/Max, default 0..1 passthrough) is
  // set separately via inline text, same as the formula itself; see
  // ExoEQ.h's own note on why that range lives as a plain member rather
  // than a param.
  {
    static const char* kVarLetters[kNumFractalCustomVars] = { "E", "X", "O", "M", "U", "S", "I", "C", "A" };
    for (int i = 0; i < kNumFractalCustomVars; ++i)
      GetParam(kGlobalFractalVarStart + i)->InitDouble((std::string("Fractal Var ") + kVarLetters[i]).c_str(), 0.5, 0., 1.0, 0.0001, "");
  }

  // Transform filter (P5 scope step 2 -- DSP/TransformFilter.h). Pivot
  // defaults to 1kHz (a neutral, roughly-centered choice -- it only
  // matters once exponent != 1 anyway); exponent defaults to 1.0 (exact
  // no-op), so a freshly-opened instance's Transform button, clicked
  // with zero knob-turning, changes nothing -- same "off/neutral by
  // default" guarantee every other feature in this plugin gives.
  GetParam(kGlobalTransformPivotHz)->InitDouble("Transform Pivot Hz", 1000., 20., 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalTransformExponent)->InitDouble("Transform Exponent", 1.0, -4.0, 4.0, 0.0001, ""); // 1=no-op, >1=stretch, 0..1=compress, <0=mirror/invert -- see TransformFilter.h

  // Image-to-EQ-curve (P5 scope step 3 of 3) -- see DSP/ImageToEq.h for
  // what each of these drives. Defaults match ImageToEqParams's own
  // struct defaults exactly, so a fresh instance's knobs read the same
  // values that struct would already default to if left untouched.
  GetParam(kGlobalImageNodeCount)->InitInt("Image Node Count", 24, 2, 64, ""); // same 64 cap as FractalGenParams::kMaxNodes, same reason
  GetParam(kGlobalImageMinFreqHz)->InitDouble("Image Min Hz", 40.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalImageMaxFreqHz)->InitDouble("Image Max Hz", 16000.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalImageGainRangeDb)->InitDouble("Image Gain Range", 24.0, 0.0, 48.0, 0.01, "dB");
  GetParam(kGlobalImageResonance)->InitDouble("Image Resonance", 0.707, 0.05, 20.0, 0.001, "");
  GetParam(kGlobalImageInvertLuminance)->InitBool("Image Invert Luminance", false);

  // Global FFT brick-wall stage (P6, first piece) -- off by default,
  // same "no audible or CPU change until deliberately turned on" rule
  // every other optional feature in this plugin already follows. The
  // default 20Hz-20kHz passband is a near-total-passthrough starting
  // point (so turning it on with no further adjustment is close to a
  // no-op, not a surprise full mute) -- the brick-wall edges only bite
  // once you actually narrow them.
  GetParam(kGlobalFftEnabled)->InitBool("FFT Stage Enabled", false);
  GetParam(kGlobalFftLowCutHz)->InitDouble("FFT Low Cut", 20.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));
  GetParam(kGlobalFftHighCutHz)->InitDouble("FFT High Cut", 20000.0, 20.0, 20000.0, 0.01, "Hz", IParam::kFlagsNone, "", IParam::ShapePowCurve(2.));

  // Circle-GUI round 11: Mel vs. log frequency axis for the graph (see
  // ExoEQ.h's own note on kGlobalFreqAxisMel). Defaults true (Mel) so
  // every existing project's graph looks identical to before this param
  // existed. Pure display setting, read only by CircleViewControl.h.
  GetParam(kGlobalFreqAxisMel)->InitBool("Mel Axis", true);
  GetParam(kGlobalBypass)->InitBool("Bypass", false);
  GetParam(kGlobalLowMotion)->InitBool("Low Motion", false);

#if IPLUG_EDITOR // http://bit.ly/2S64BDd
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  // Real UI, first pass: a Pro-Q-style spectrum graph (BandGraphControl --
  // see that file's header for the detail, and for the explicit caveat
  // that it's unverified against the real iPlug2 SDK) plus a fixed detail
  // panel beneath it that RETARGETS to whichever band is currently
  // selected, rather than existing per-band -- exactly the two choices
  // confirmed with the user (graph+nodes overview, one retargeting panel).
  mLayoutFunc = [&](IGraphics* pGraphics) {
    mCircleView = nullptr; // rebuilt below; never keep a pointer to the last window's control
    for (auto& v : mTabControls) v.clear(); // raw pointers into the last window; rebuilt below
    // FOUND THIS ROUND -- the actual cause of EVERY "hover doesn't light
    // anything up" report, knobs/buttons included: IGraphics never
    // dispatches OnMouseOver/OnMouseOut to ANY control (standard
    // IVKnobControl/IVButtonControl/IVToggleControl via their built-in
    // DrawPressableShape kHL highlight, AND CircleViewControl's own
    // idle-hover code -- rail rows, UNDO/REDO, preset tray, the node/
    // resonance-handle halos) unless this is turned on. It defaults to
    // false (IGraphics.h: `bool mEnableMouseOver = false;`) because
    // tracking mouse-over is extra work the framework doesn't do unless
    // asked. Every hover affordance built across every round was real
    // code sitting behind a switch nobody flipped -- this is that switch.
    pGraphics->EnableMouseOver(true);
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachPanelBackground(IColor(255, 10, 10, 12)); // conceptual-art pass: near-black
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    const IRECT bounds = pGraphics->GetBounds();
    const IRECT innerBounds = bounds.GetPadded(-10.f);

    // Three vertical sections now instead of two (circle-GUI round 2):
    // graph (42%) / per-band detail panel (30%) / global tab strip (28%,
    // NEW -- see the big comment block below the detail panel for why).
    // Built via direct L/T/R/B arithmetic rather than chained
    // GetFromTop/GetFromBottom calls on fractions of innerBounds.H(),
    // which was the previous code's approach -- that only worked for two
    // sections; three would mean each cut's fraction has to be taken of a
    // shrinking remainder instead of the constant total, which reads
    // error-prone. Same "manual IRECT construction via direct field
    // arithmetic" pattern CircleViewControl.h already uses elsewhere in
    // this project for the same reason (avoids guessing at IRECT helper
    // method names/behavior).
    // NEW THIS ROUND: a thin header strip above the graph for the
    // selected-circle Solo/Mute buttons (see CircleViewControl's new
    // GetSelectedCircle() getter below) -- carved off innerBounds FIRST,
    // same "carve the fixed-size piece before splitting the remainder by
    // fraction" approach panelRow3 already used below.
    const float headerH = 44.f; // was 30: the Mel Axis toggle's label and its ON/OFF value overlapped at 30
    const IRECT headerRow = IRECT(innerBounds.L, innerBounds.T, innerBounds.R, innerBounds.T + headerH);
    const IRECT belowHeader = IRECT(innerBounds.L, innerBounds.T + headerH, innerBounds.R, innerBounds.B);
    // FIXED/CHANGED THIS ROUND, per your explicit request ("the plugin
    // should definitely be fully what the black screen is right now, and
    // all the parameters will appear and disappear based on the actions,
    // a bit like on Fabfilter Pro-Q"): the graph used to get a fixed 42%
    // slice with the detail panel and tab strip permanently occupying
    // the other 58% underneath it, always visible whether or not there
    // was anything to show in them. Now graphArea is the ENTIRE
    // belowHeader region -- the graph IS the whole window below the
    // header, all the time. panelFull/tabsFull keep the exact same
    // bottom-anchored geometry they always had (still computed off
    // belowHeader, not off graphArea), so nothing about their own
    // internal GetGridCell layouts needed to change -- they now simply
    // FLOAT ON TOP of the graph's bottom portion instead of pushing it
    // up out of the way, and are HIDDEN by default (see the panelControls
    // vector and the new "Global FX" toggle below), only drawn when
    // there's something contextual to show -- a band selected for the
    // detail panel, the new header toggle for the FX tab strip. Being
    // attached to pGraphics AFTER circleView already puts them on top in
    // z-order, so no separate "draw on top" handling was needed.
    // Concept 02 layout split (this pass): the graph gets the TOP 72% of the
    // area below the header, and the advanced drawer owns the BOTTOM 28%.
    // The old version let the drawer float over the radar, which is why the
    // arcs were cut off. Now the radar never sits under the drawer. The
    // panel and the FX tab strip share the same drawer region; the Global FX
    // toggle already hides the panel whenever the FX strip is open, so the
    // two never show at once.
    // Two drawer depths. The compact band drawer (panelFull) is shallow: it only
    // ever shows Type/Cutoff/Reso/Gain/Mute. The Global FX drawer (tabsFull) is
    // deeper, because tabs like Fractal carry two knob rows plus a button row and
    // were overflowing past the bottom of the window at the shallow depth. The two
    // never show together (the FX backdrop is attached after the band panel, so it
    // covers it), so tabsFull is allowed to reach up over the graph.
    // Three drawer regions, bottom-anchored:
    //   panelFull     - always-on compact band row (Type/Cutoff/Reso/Gain/Mute/Delta)
    //   bandAdvArea   - per-band sends + Energy2 + slot order, above the compact row,
    //                   revealed by the header's "Band FX" button
    //   tabsFull      - the Global FX tab strip, same depth as bandAdvArea
    // bandAdvArea and tabsFull occupy the same screen space and are mutually
    // exclusive: each one's toggle closes the other. That is deliberate. The FX
    // backdrop is attached after the band knobs, so if both were shown the strip
    // would simply cover the band controls -- which is exactly the bug this
    // restructure fixes (the per-band sends were being "shown" underneath it and
    // were therefore unreachable).
    const float drawerH = belowHeader.H() * 0.17f;
    const float fxDrawerH = belowHeader.H() * 0.46f;
    const IRECT graphArea = IRECT(belowHeader.L, belowHeader.T, belowHeader.R, belowHeader.B - drawerH);
    const IRECT panelFull = IRECT(belowHeader.L, graphArea.B, belowHeader.R, belowHeader.B);
    const IRECT tabsFull = IRECT(belowHeader.L, belowHeader.B - fxDrawerH, belowHeader.R, belowHeader.B);
    const IRECT bandAdvArea = IRECT(belowHeader.L, belowHeader.B - fxDrawerH, belowHeader.R, panelFull.T);
    const IRECT advRow1 = bandAdvArea.GetGridCell(0, 0, 3, 1);
    const IRECT advRow2 = bandAdvArea.GetGridCell(1, 0, 3, 1);
    const IRECT advRow3 = bandAdvArea.GetGridCell(2, 0, 3, 1);
    // Row 1: the original core params. Row 2: the per-node sends +
    // sidechain added in this pass -- kept on a separate row rather than
    // widening row 1, since this is already 4 send-enable toggles + 9
    // numeric params and cramming it into one row would make every control
    // too small to use.
    const IRECT panelRow1 = panelFull;      // the compact row occupies the whole shallow drawer
    const IRECT panelRow2 = advRow2;        // sends cluster
    const IRECT panelRow3 = advRow3;        // slot-order cluster

    // ROUND 26 FIX -- real bug found from your screenshot: the detail-panel
    // knobs were rendering completely invisible (just black) even when a
    // band was selected. Root cause: iPlug2 draws attached controls in
    // ATTACH ORDER, later = on top. CircleViewControl now fills its ENTIRE
    // bounds with an opaque background every frame (ever since graphArea
    // became the whole window, last round's FabFilter-style change), and
    // circleView used to be attached AFTER all the panel knobs below --
    // so its own opaque backdrop was painted directly over them every
    // single frame, regardless of Hide(false). Only controls attached
    // AFTER circleView (header buttons, the X button) ever stayed visible.
    //
    // Fix: attach circleView FIRST, before any panel/tab control, so
    // everything else naturally draws on top of it. The problem is
    // circleView's constructor needs onBandSelected, and onBandSelected
    // needs the knob pointers below, which don't exist yet at this point.
    // Same forward-reference-slot trick already used for
    // panelCloseButtonSlot/fxVisible/activeTabIdx elsewhere in this file:
    // a shared_ptr<std::function<void(int)>> that circleView's ctor
    // captures a tiny forwarding lambda around, filled in with the real
    // onBandSelected once it exists further down.
    auto onBandSelectedSlot = std::make_shared<std::function<void(int)>>(nullptr);
    auto* circleView = new CircleViewControl(graphArea, GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0,
      [onBandSelectedSlot](int bandIndex) { if (*onBandSelectedSlot) (*onBandSelectedSlot)(bandIndex); });
    pGraphics->AttachControl(circleView);
    // Lower-drawer backdrop: attached here, before the detail-panel knobs,
    // so the knobs draw on top of it. Joins panelControls so it shows/hides
    // with the panel itself.
    auto* panelBackdrop = pGraphics->AttachControl(new DrawerBackdrop(panelFull));
    // Attached here, before the band knobs, so those draw on top of it. Hidden
    // until the header's "Band FX" button reveals it.
    auto* bandAdvBackdrop = pGraphics->AttachControl(new DrawerBackdrop(bandAdvArea));
    auto* bandAdvTitle = pGraphics->AttachControl(new TextLabelControl(IRECT(bandAdvArea.L + 18.f, bandAdvArea.T + 6.f, bandAdvArea.L + 260.f, bandAdvArea.T + 26.f), "BAND FX", 12.f, IColor(255, 170, 190, 215)));
    bandAdvBackdrop->Hide(true);
    bandAdvTitle->Hide(true);
    mCircleView = circleView; // ROUND 27 -- lets SerializeState/UnserializeState reach GetUserPresets/SetUserPresets below
    // ROUND 27 -- a project state restored BEFORE this editor was ever
    // opened leaves its user presets waiting in mPendingUserPresets (see
    // its own note in ExoEQ.h); apply them now that circleView actually
    // exists, then clear the flag so a later close/reopen of this same
    // editor instance doesn't re-apply a now-stale snapshot over presets
    // saved/deleted since.
    if (mHasPendingUserPresets)
    {
      std::vector<CircleViewControl::PresetCircleDef> presets;
      for (const auto& pend : mPendingUserPresets)
      {
        CircleViewControl::PresetCircleDef def{};
        std::snprintf(def.name, sizeof(def.name), "%s", pend.name);
        def.bandCount = pend.bandCount;
        for (int b = 0; b < 3; ++b)
          def.bands[b] = { pend.bands[b].ordinal, pend.bands[b].cutoffHz, pend.bands[b].resonance, pend.bands[b].forceMorph50 };
        presets.push_back(def);
      }
      circleView->SetUserPresets(presets);
      // Keep mPendingUserPresets: it is the cache OnUIClose refreshes and serialize reads while the GUI is closed.
    }

    // Detail panel controls are created ONCE, bound initially to band 0,
    // and retargeted (SetParamIdx) whenever BandGraphControl reports a
    // different selection -- this is the "one fixed panel" choice: no
    // per-band control instances, no recreate-on-select.
    // Filter-type dropdown goes FIRST in the row -- "what kind of filter"
    // reads as the primary decision, with cutoff/reso/morph as that
    // type's own parameters underneath it, same information hierarchy
    // FabFilter's own type-then-params layout uses.
    //
    // NOTE on IVMenuButtonControl: this is iPlug2's standard control for
    // a text-dropdown bound to an enum param (click it, get a popup list
    // of InitEnum's option strings) -- I can't verify its exact
    // constructor signature against the real vendored SDK here, same
    // caveat as SetParamIdx below. If this specific line throws a build
    // error, paste it.
    // Widened from 7 to 8 cells this round to fit the new "Extra" knob
    // (AW Y-Lowpass's powFactor, DSP/FilterCore.h's FilterParams::extra) --
    // a no-op dial for every other filter type, but needed its own real,
    // automatable slot per the user's explicit request rather than being
    // folded into Reso/Morph like every earlier Airwindows port.
    // Compact row is now 8 cells: Type, Cutoff, Reso, Morph, Extra, Gain, Mute, Delta.
    // Morph/Extra moved here from the Band FX drawer so they are visible per band
    // without opening the drawer. Labels are per-type via KnobNamesForType.
    auto* filterTypeMenu = pGraphics->AttachControl(new IVMenuButtonControl(panelRow1.GetGridCell(0, 0, 1, 8).GetCentredInside(90, 40), BandParamIdx(0, kBandFilterType), "Type"));
    auto* cutoffKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow1.GetGridCell(0, 1, 1, 8).GetCentredInside(KnobSize(70, panelRow1.GetGridCell(0, 1, 1, 8))), BandParamIdx(0, kBandCutoff), "Cutoff", 0));
    auto* resoKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow1.GetGridCell(0, 2, 1, 8).GetCentredInside(KnobSize(70, panelRow1.GetGridCell(0, 2, 1, 8))), BandParamIdx(0, kBandResonance), "Reso", 0));
    auto* morphKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow1.GetGridCell(0, 3, 1, 8).GetCentredInside(KnobSize(70, panelRow1.GetGridCell(0, 3, 1, 8))), BandParamIdx(0, kBandMorph), "Morph", 0));
    auto* extraKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow1.GetGridCell(0, 4, 1, 8).GetCentredInside(KnobSize(70, panelRow1.GetGridCell(0, 4, 1, 8))), BandParamIdx(0, kBandFilterExtra), "Extra", 0));
    auto* keyTrackKnob = pGraphics->AttachControl(new ExoStepKnob(advRow1.GetGridCell(0, 2, 1, 8).GetCentredInside(KnobSize(70, advRow1.GetGridCell(0, 2, 1, 8))), BandParamIdx(0, kBandKeyTrackAmt), "KeyTrk", 0));
    auto* midiChannelMenu = pGraphics->AttachControl(new IVMenuButtonControl(advRow1.GetGridCell(0, 3, 1, 8).GetCentredInside(86, 40), BandParamIdx(0, kBandMidiChannel), "MIDI"));
    auto* gainKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow1.GetGridCell(0, 5, 1, 8).GetCentredInside(KnobSize(70, panelRow1.GetGridCell(0, 5, 1, 8))), BandParamIdx(0, kBandGainDb), "Gain", 0));
    auto* muteToggle = pGraphics->AttachControl(new IVToggleControl(panelRow1.GetGridCell(0, 6, 1, 8).GetCentredInside(70, 40), BandParamIdx(0, kBandMute), "Mute"));
    auto* soloToggle = pGraphics->AttachControl(new IVToggleControl(panelRow1.GetGridCell(0, 7, 1, 8).GetCentredInside(70, 40), BandParamIdx(0, kBandSolo), "Delta"));

    // Row 2: 3 send enable+param clusters + sidechain, 13 controls across
    // 13 grid cells -- one cell per control keeps this simple; a tighter
    // per-send grouping (sub-grids) can replace this once it's confirmed
    // working, purely cosmetic.
    auto* delayOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 0, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandDelayEnabled), "Dly On"));
    auto* delayTimeKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 1, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 1, 1, 13))), BandParamIdx(0, kBandDelayTimeMs), "Dly Time", 0));
    auto* delayFbKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 2, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 2, 1, 13))), BandParamIdx(0, kBandDelayFeedback), "Dly FB", 0));
    auto* delayMixKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 3, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 3, 1, 13))), BandParamIdx(0, kBandDelayMix), "Dly Mix", 0));

    auto* distOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 4, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandDistortionEnabled), "Dist On"));
    auto* distDriveKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 5, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 5, 1, 13))), BandParamIdx(0, kBandDistortionDrive), "Drive", 0));
    auto* distCharKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 6, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 6, 1, 13))), BandParamIdx(0, kBandDistortionCharacter), "Char", 0));
    auto* distMixKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 7, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 7, 1, 13))), BandParamIdx(0, kBandDistortionMix), "Dist Mix", 0));

    auto* shiftOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 8, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandFreqShiftEnabled), "Shift On"));
    auto* shiftHzKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 9, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 9, 1, 13))), BandParamIdx(0, kBandFreqShiftHz), "Shift Hz", 0));
    auto* shiftMixKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 10, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 10, 1, 13))), BandParamIdx(0, kBandFreqShiftMix), "Shift Mix", 0));

    auto* scOnToggle = pGraphics->AttachControl(new IVToggleControl(panelRow2.GetGridCell(0, 11, 1, 13).GetCentredInside(60, 36), BandParamIdx(0, kBandSidechainEnabled), "SC On"));
    auto* scDepthKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow2.GetGridCell(0, 12, 1, 13).GetCentredInside(KnobSize(60, panelRow2.GetGridCell(0, 12, 1, 13))), BandParamIdx(0, kBandSidechainModOctaves), "SC Depth", 0));

    // Row 3: the three "order of effect" slot knobs (freq-shift/distortion/
    // delay), each 0-2 (int param), PLUS Energy2's on-toggle + 2 grouped
    // knobs + mix (widened from 3 to 7 cells this round) -- Energy2 isn't
    // reorderable (see BandChain.h's Band::ProcessSample for why it's
    // fixed right after the filter, not a 4th slot), so it rides on this
    // row for layout convenience, not because it's conceptually an
    // "order" control.
    auto* shiftSlotKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow3.GetGridCell(0, 0, 1, 7).GetCentredInside(KnobSize(60, panelRow3.GetGridCell(0, 0, 1, 7))), BandParamIdx(0, kBandFreqShiftSlot), "Shift Ord", 3));
    auto* distSlotKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow3.GetGridCell(0, 1, 1, 7).GetCentredInside(KnobSize(60, panelRow3.GetGridCell(0, 1, 1, 7))), BandParamIdx(0, kBandDistortionSlot), "Dist Ord", 3));
    auto* delaySlotKnob = pGraphics->AttachControl(new ExoStepKnob(panelRow3.GetGridCell(0, 2, 1, 7).GetCentredInside(KnobSize(60, panelRow3.GetGridCell(0, 2, 1, 7))), BandParamIdx(0, kBandDelaySlot), "Dly Ord", 3));

    // Energy2: On toggle + 2 grouped knobs (GlitterHiss, RatScrapeFizz) +
    // bipolar Mix. These are real, automatable per-band params like
    // everything else here -- the global 20-instance cap (see
    // DSP/BandChain.h's kMaxEnergy2Instances) is enforced in the DSP
    // layer, not in the UI; a band past the cap still shows and accepts
    // these controls, it just silently has no audible effect until a
    // slot frees up (same "inert past the limit" behavior kBandActive
    // already uses for inactive bands).
    auto* energy2OnToggle = pGraphics->AttachControl(new IVToggleControl(advRow1.GetGridCell(0, 4, 1, 8).GetCentredInside(60, 36), BandParamIdx(0, kBandEnergy2Enabled), "Energy2"));
    auto* energy2GlitterHissKnob = pGraphics->AttachControl(new ExoStepKnob(advRow1.GetGridCell(0, 5, 1, 8).GetCentredInside(KnobSize(60, advRow1.GetGridCell(0, 5, 1, 8))), BandParamIdx(0, kBandEnergy2GlitterHiss), "Glitr/Hiss", 0));
    auto* energy2RatScrapeFizzKnob = pGraphics->AttachControl(new ExoStepKnob(advRow1.GetGridCell(0, 6, 1, 8).GetCentredInside(KnobSize(60, advRow1.GetGridCell(0, 6, 1, 8))), BandParamIdx(0, kBandEnergy2RatScrapeFizz), "Rat/Scr/Fizz", 0));
    auto* energy2MixKnob = pGraphics->AttachControl(new ExoStepKnob(advRow1.GetGridCell(0, 7, 1, 8).GetCentredInside(KnobSize(60, advRow1.GetGridCell(0, 7, 1, 8))), BandParamIdx(0, kBandEnergy2Mix), "Energy2 Mix", 0));

    // NOTE on SetParamIdx: this is the one call in this whole file I'm
    // least certain matches the vendored SDK version's exact signature --
    // it's a real, commonly-used iPlug2 IControl method for exactly this
    // "rebind an existing control to a different param" pattern, but I
    // can't check it against the real headers here. If the build errors
    // on this specific call, paste it and I'll fix the exact signature.
    // NEW THIS ROUND -- every detail-panel control in one list, purely so
    // onBandSelected (right below) can Hide/show all of them in one pass
    // instead of needing a second hand-maintained capture list. Built
    // from the exact same pointers already captured into onBandSelected
    // itself, so there's no risk of the two lists drifting apart.
    // ORDER MATTERS. The first kCompactPanelCount entries are the always-on
    // compact row; everything after is the Band FX expansion. Two separate hide
    // loops used to hardcode different boundaries (4 and 6) into this same list,
    // which is why Reso and Mute silently vanished when two entries were added at
    // the front. The boundary is a named constant now, used by every loop.
    const std::vector<IControl*> panelControls = {
      panelBackdrop, filterTypeMenu, cutoffKnob, resoKnob, morphKnob, extraKnob, gainKnob, muteToggle, soloToggle,
      // --- everything from here on is Band FX ---
      bandAdvBackdrop, bandAdvTitle, midiChannelMenu, keyTrackKnob,
      delayOnToggle, delayTimeKnob, delayFbKnob, delayMixKnob,
      distOnToggle, distDriveKnob, distCharKnob, distMixKnob,
      shiftOnToggle, shiftHzKnob, shiftMixKnob,
      scOnToggle, scDepthKnob,
      shiftSlotKnob, distSlotKnob, delaySlotKnob,
      energy2OnToggle, energy2GlitterHissKnob, energy2RatScrapeFizzKnob, energy2MixKnob
    };
    constexpr size_t kCompactPanelCount = 9; // backdrop, Type, Cutoff, Reso, Morph, Extra, Gain, Mute, Delta
    // Starts hidden -- nothing is selected yet on a fresh load, so there's
    // nothing for the detail panel to show (the FabFilter Pro-Q-style
    // "parameters appear/disappear based on the action" behavior you
    // asked for).
    for (auto* c : panelControls) c->Hide(true);

    // NEW THIS ROUND -- a forward-reference slot for the panel's own
    // close button, which doesn't exist yet at this point (it needs
    // `circleView`, created further below) but onBandSelected (right
    // here) is the one place that already knows when to show/hide it.
    // A shared_ptr, not a plain IControl* local, for the same reason
    // fxVisible/activeTabIdx above are shared_ptrs -- onBandSelected
    // itself is stored as a long-lived std::function and outlives this
    // OnLayout call, so a raw reference to a stack local would dangle.
    auto panelCloseButtonSlot = std::make_shared<IControl*>(nullptr);
    auto selectedBandForPanel = std::make_shared<int>(-1);
    // T3-2: morph stage preview in the bottom strip of the Morph cell (drawn over the knob's margin, mouse-transparent).
    {
      const IRECT morphCell = panelRow1.GetGridCell(0, 3, 1, 8);
      pGraphics->AttachControl(new MorphSparkline(IRECT(morphCell.L + 6.f, morphCell.B - 16.f, morphCell.R - 6.f, morphCell.B - 2.f),
                                                  [selectedBandForPanel]() { return *selectedBandForPanel; }, GetSampleRate()));
    }
    auto bandFxVisible = std::make_shared<bool>(false);

    std::function<void()> refreshKnobNames = [this, cutoffKnob, resoKnob, morphKnob, extraKnob, selectedBandForPanel,
                                              panelRow1, filterTypeMenu, gainKnob, muteToggle, soloToggle]() {
      const int b = *selectedBandForPanel;
      if (b < 0) return;
      const BandKnobNames names = KnobNamesForType(static_cast<int>(GetParam(BandParamIdx(b, kBandFilterType))->Value()));
      // A knob whose label is "n/a" does nothing for this filter type, so it is
      // hidden instead of shown as a dead control. Runs after the panel's Hide
      // pass in onBandSelected, so it has the final say on these four knobs.
      auto apply = [](IControl* c, const char* label) {
        if (auto* vb = dynamic_cast<IVectorBase*>(c)) vb->SetLabelStr(label);
        c->Hide(std::string(label) == "n/a");
      };
      apply(cutoffKnob, names.cutoff);
      apply(resoKnob,   names.reso);
      apply(morphKnob,  names.morph);
      if (auto* sk = dynamic_cast<ExoStepKnob*>(morphKnob))
      {
        const int ordinal = static_cast<int>(GetParam(BandParamIdx(b, kBandFilterType))->Value());
        sk->SetSteps(MorphStepsForType(ordinal));
        sk->SetIndexBase(MorphIndexBaseForType(ordinal));
        sk->SetStrip(ordinal == 0); // T2-4: LP|BP|HP strip only for the LP-BP-HP morph type
      }
      apply(extraKnob,  names.extra);

      // Re-flow the compact row: hidden knobs give up their cell, so the
      // remaining controls close up from the left instead of leaving a gap.
      // Each slot: control, its cell-centred size (0 = knob, sized by KnobSize).
      struct CompactSlot { IControl* c; float w; float h; bool visible; };
      const std::string noA = "n/a";
      const CompactSlot slots[] = {
        { filterTypeMenu, 90.f, 40.f, true },
        { cutoffKnob,      0.f,  0.f, names.cutoff != noA },
        { resoKnob,        0.f,  0.f, names.reso   != noA },
        { morphKnob,       0.f,  0.f, names.morph  != noA },
        { extraKnob,       0.f,  0.f, names.extra  != noA },
        { gainKnob,        0.f,  0.f, true },
        { muteToggle,     70.f, 40.f, true },
        { soloToggle,     70.f, 40.f, true },
      };
      int cell = 0;
      for (const auto& sl : slots)
      {
        if (!sl.visible || !sl.c) continue;
        const IRECT r = panelRow1.GetGridCell(0, cell++, 1, 8);
        const bool isKnob = (sl.w == 0.f);
        sl.c->SetTargetRECT(isKnob ? r.GetCentredInside(KnobSize(70, r)) : r.GetCentredInside(sl.w, sl.h));
      }
    };
    mRefreshKnobNames = refreshKnobNames; // also driven from OnParamChangeUI on a filter-type change

    auto onBandSelected = [refreshKnobNames, panelControls, panelCloseButtonSlot, filterTypeMenu, cutoffKnob, resoKnob, morphKnob, extraKnob, keyTrackKnob, midiChannelMenu, gainKnob, muteToggle, soloToggle,
                            delayOnToggle, delayTimeKnob, delayFbKnob, delayMixKnob,
                            distOnToggle, distDriveKnob, distCharKnob, distMixKnob,
                            shiftOnToggle, shiftHzKnob, shiftMixKnob,
                            scOnToggle, scDepthKnob,
                            shiftSlotKnob, distSlotKnob, delaySlotKnob,
                            energy2OnToggle, energy2GlitterHissKnob, energy2RatScrapeFizzKnob, energy2MixKnob, selectedBandForPanel, bandFxVisible, kCompactPanelCount](int bandIndex) {
      *selectedBandForPanel = bandIndex;
      // The panel (and its own close button, once it exists) shows
      // once a real band is selected (bandIndex >= 0) and hides when
      // ClearSelection() below passes -1.
      // Keep selection focused on the professional EQ controls. The send,
      // sidechain, order, and Energy2 controls remain available through the
      // explicit FX surface instead of covering the graph whenever a node
      // is created.
      // Keep the graph dominant after creation. Only the filter identity,
      // frequency, Q, and mute state remain in the compact surface; every
      // other parameter is an intentional Global FX expansion.
      for (size_t i = 0; i < panelControls.size(); ++i)
        panelControls[i]->Hide(bandIndex < 0 || (i >= kCompactPanelCount && !*bandFxVisible));
      if (*panelCloseButtonSlot) (*panelCloseButtonSlot)->Hide(bandIndex < 0);
      // NEW THIS ROUND -- GUARD ADDED: bandIndex can now legitimately be
      // -1 (CircleViewControl's new rail-empty-space deselect, and the
      // panel's own "X" close button both call this with -1 to mean
      // "nothing selected"). BandParamIdx(-1, offset) would underflow
      // into a garbage/out-of-range param index -- every SetParamIdx
      // call below MUST be skipped in that case, not just the Hide()
      // calls above. This guard is the actual fix that makes -1 safe to
      // pass at all; without it, deselecting would silently retarget
      // every one of these controls at an invalid param index.
      if (bandIndex < 0) return;
      RetargetControl(filterTypeMenu, BandParamIdx(bandIndex, kBandFilterType));
      RetargetControl(gainKnob, BandParamIdx(bandIndex, kBandGainDb));
      if (refreshKnobNames) refreshKnobNames();
      RetargetControl(cutoffKnob, BandParamIdx(bandIndex, kBandCutoff));
      RetargetControl(resoKnob, BandParamIdx(bandIndex, kBandResonance));
      RetargetControl(morphKnob, BandParamIdx(bandIndex, kBandMorph));
      RetargetControl(extraKnob, BandParamIdx(bandIndex, kBandFilterExtra));
      RetargetControl(keyTrackKnob, BandParamIdx(bandIndex, kBandKeyTrackAmt));
      RetargetControl(midiChannelMenu, BandParamIdx(bandIndex, kBandMidiChannel));
      RetargetControl(muteToggle, BandParamIdx(bandIndex, kBandMute));
      RetargetControl(soloToggle, BandParamIdx(bandIndex, kBandSolo));

      RetargetControl(delayOnToggle, BandParamIdx(bandIndex, kBandDelayEnabled));
      RetargetControl(delayTimeKnob, BandParamIdx(bandIndex, kBandDelayTimeMs));
      RetargetControl(delayFbKnob, BandParamIdx(bandIndex, kBandDelayFeedback));
      RetargetControl(delayMixKnob, BandParamIdx(bandIndex, kBandDelayMix));

      RetargetControl(distOnToggle, BandParamIdx(bandIndex, kBandDistortionEnabled));
      RetargetControl(distDriveKnob, BandParamIdx(bandIndex, kBandDistortionDrive));
      RetargetControl(distCharKnob, BandParamIdx(bandIndex, kBandDistortionCharacter));
      RetargetControl(distMixKnob, BandParamIdx(bandIndex, kBandDistortionMix));

      RetargetControl(shiftOnToggle, BandParamIdx(bandIndex, kBandFreqShiftEnabled));
      RetargetControl(shiftHzKnob, BandParamIdx(bandIndex, kBandFreqShiftHz));
      RetargetControl(shiftMixKnob, BandParamIdx(bandIndex, kBandFreqShiftMix));

      RetargetControl(scOnToggle, BandParamIdx(bandIndex, kBandSidechainEnabled));
      RetargetControl(scDepthKnob, BandParamIdx(bandIndex, kBandSidechainModOctaves));

      RetargetControl(shiftSlotKnob, BandParamIdx(bandIndex, kBandFreqShiftSlot));
      RetargetControl(distSlotKnob, BandParamIdx(bandIndex, kBandDistortionSlot));
      RetargetControl(delaySlotKnob, BandParamIdx(bandIndex, kBandDelaySlot));

      RetargetControl(energy2OnToggle, BandParamIdx(bandIndex, kBandEnergy2Enabled));
      RetargetControl(energy2GlitterHissKnob, BandParamIdx(bandIndex, kBandEnergy2GlitterHiss));
      RetargetControl(energy2RatScrapeFizzKnob, BandParamIdx(bandIndex, kBandEnergy2RatScrapeFizz));
      RetargetControl(energy2MixKnob, BandParamIdx(bandIndex, kBandEnergy2Mix));
    };
    // ROUND 26: circleView already exists and is attached (moved above,
    // before the panel knobs, to fix the z-order bug) -- just wire the
    // real callback into the forward-reference slot it was given.
    *onBandSelectedSlot = onBandSelected;
    circleView->RestoreSelection(mSavedSelCircle, mSavedSelBand); // reopen: keep the node/band that was selected

    // NEW THIS ROUND -- the "explicit way to deselect/hide the detail
    // panel again" follow-up flagged last round (clicking empty graph
    // space can't double as deselect: that gesture is already taken by
    // "create a new band here"). A small close button pinned to the
    // panel's own top-right corner, visible only while the panel itself
    // is -- same Hide() toggling as every other contextual control here,
    // just driven by this button instead of a band selection. Clears
    // CircleViewControl's own selection AND hides every panelControls
    // entry in one action function, since neither side can fully do
    // this alone (the control has no reach into the panel's Hide()
    // calls; the panel's own button has no reach into the control's
    // private mSelectedBand without the new ClearSelection() getter).
    // NOTE on GetFromTRHC: this file already has GetFromBRHC proven to
    // compile (the build-info text at the bottom of OnLayout) -- IRECT's
    // usual quartet of corner-helpers (TLHC/TRHC/BLHC/BRHC) almost
    // certainly includes this one too, but it's not itself proven yet;
    // paste the error if this specific name doesn't compile.
    auto* panelCloseButton = pGraphics->AttachControl(new IVButtonControl(panelFull.GetFromTRHC(28.f, 28.f), SplashClickActionFunc, "X"));
    *panelCloseButtonSlot = panelCloseButton; // fills in the forward-reference slot onBandSelected captured above
    panelCloseButton->Hide(true); // starts hidden along with the rest of panelControls -- it didn't exist yet when that initial hide loop ran
    panelCloseButton->SetActionFunction([circleView, panelControls, panelCloseButton](IControl* pCaller) {
      circleView->ClearSelection();
      for (auto* c : panelControls) c->Hide(true);
      panelCloseButton->Hide(true);
    });

    // NEW THIS ROUND: Solo/Mute buttons for whichever circle is currently
    // selected on the graph -- confirmed as part of the original design
    // scope ("selected-circle Solo/Mute buttons", previously flagged as
    // deferred). Lives in the new header strip above the graph. Mute
    // toggles every band in the selected circle together (reads the
    // FIRST active band's own Mute state to decide which way to flip --
    // "any unmuted -> mute all", "all already muted -> unmute all", a
    // plain toggle rather than two separate buttons). Solo is one-way:
    // unmutes every band in the selected circle, mutes every band in
    // every OTHER circle -- standard mixing-console solo behavior, not a
    // toggle (clicking Solo on a different circle afterward re-solos that
    // one instead; there's no separate "un-solo" state to track).
    // NEW THIS ROUND -- master show/hide for the whole global-FX tab
    // strip (Groups/Mod Matrix/LFOs/Fractal/Transform/Image + MTS-ESP),
    // which now starts hidden along with the detail panel (same Pro-Q-
    // style "nothing shows until you ask for it" request) -- the tab
    // strip has no OTHER always-visible trigger once it's hidden
    // (clicking a tab BUTTON would normally reveal that tab's content,
    // but the buttons themselves live inside tabsFull and would be
    // hidden too), so this lives in the header, outside tabsFull,
    // specifically so it's never itself hidden by the thing it controls.
    // fxVisible is a shared_ptr<bool> (not a raw/member bool) purely so
    // this lambda and the tab-strip-building code far below can both
    // capture-by-value and still share the one flag, without adding a
    // new ExoEQ member for a single UI-only toggle.
    auto fxVisible = std::make_shared<bool>(false);
    // Concept 02 top bar: backdrop + wordmark + subtitle, attached before the
    // header buttons so those draw on top of it.
    pGraphics->AttachControl(new ToolbarBackdrop(headerRow));
    pGraphics->AttachControl(new TextLabelControl(headerRow.GetFromLeft(120.f).GetPadded(-14.f), "ExoEQ", 16.f, IColor(255, 235, 240, 248)));
    // T1-1: "Dirty Hardware Lab" label removed from the header; it overlapped the Band FX button.
    auto* globalFxButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 0, 1, 7).GetCentredInside(104, 34), SplashClickActionFunc, "Global FX"));
    // Placeholder: the real Global FX action function is installed further down,
    // once the tab strip it drives exists.

    // Band FX: reveals the selected band's sends / Energy2 / slot order. Its own
    // action function is installed below, next to Global FX's, because the two
    // must close each other.
    auto* bandFxButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 1, 1, 7).GetCentredInside(104, 34), SplashClickActionFunc, "Band FX"));
    mBandFxButton = bandFxButton;
    UpdateBandFxBadge(); // T2-7: show the badge straight away if a band FX is already on

    // FIXED THIS ROUND: both buttons below used to be plain fire-once
    // IVButtonControls with no memory of their own -- SplashClickActionFunc's
    // own press-flash was the ONLY thing that ever lit them up, so clicking
    // again to toggle back LOOKED like nothing happened cosmetically (the
    // flash still fired, the actual mute state really did flip, there was
    // just no persistent on/off look tied to it). Both now call
    // pCaller->SetValue(1.0/0.0) after every click to reflect whether this
    // circle is ACTUALLY (fully muted / soloed) right now -- IVButtonControl
    // draws filled/lit while its own value is > 0.5, same as any other
    // iPlug2 vector control (same can't-verify-against-your-exact-headers
    // caveat as everywhere else in this file; paste the error if SetValue
    // alone doesn't drive the look you'd expect).
    auto* muteCircleButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 4, 1, 7).GetCentredInside(104, 34), SplashClickActionFunc, "Mute Circle"));
    muteCircleButton->SetActionFunction([this, circleView](IControl* pCaller) {
      const int c = circleView->GetSelectedCircle();
      bool anyUnmuted = false;
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        if (static_cast<int>(GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != c) continue;
        if (GetParam(BandParamIdx(b, kBandMute))->Value() <= 0.5) { anyUnmuted = true; break; }
      }
      // anyUnmuted true -> this click MUTES everything in the circle (so
      // the button should now read "on"/lit); anyUnmuted false -> it
      // UNMUTES everything (button goes dark).
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        if (static_cast<int>(GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != c) continue;
        SetParameterValue(BandParamIdx(b, kBandMute), anyUnmuted ? 1.0 : 0.0);
      }
      pCaller->SetValue(anyUnmuted ? 1.0 : 0.0);
      pCaller->SetDirty(false);
    });

    // FIXED THIS ROUND: Solo Circle used to be a one-way action (always
    // re-solos the selected circle, never un-solos) -- clicking it a
    // second time did the exact same thing again, which is where "never
    // cosmetically turns off" came from: there was no "off" state to turn
    // off TO. Now a real toggle -- click once to solo, click again (while
    // still soloed) to restore every band to unmuted.
    auto* soloCircleButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 5, 1, 7).GetCentredInside(104, 34), SplashClickActionFunc, "Solo Circle"));
    soloCircleButton->SetActionFunction([this, circleView](IControl* pCaller) {
      const int c = circleView->GetSelectedCircle();
      bool alreadySoloed = true;
      bool anyActive = false;
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        anyActive = true;
        const bool inSelectedCircle = static_cast<int>(GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) == c;
        const bool muted = GetParam(BandParamIdx(b, kBandMute))->Value() > 0.5;
        if (inSelectedCircle == muted) { alreadySoloed = false; break; } // in-circle-but-muted or outside-but-unmuted -- not a clean solo
      }
      alreadySoloed = alreadySoloed && anyActive;
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        const bool inSelectedCircle = static_cast<int>(GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) == c;
        // Already soloed -> this click un-solos (unmute everyone).
        // Not yet soloed -> this click solos (mute everyone else).
        SetParameterValue(BandParamIdx(b, kBandMute), alreadySoloed ? 0.0 : (inSelectedCircle ? 0.0 : 1.0));
      }
      pCaller->SetValue(alreadySoloed ? 0.0 : 1.0);
      pCaller->SetDirty(false);
    });

    // NEW THIS ROUND: Mel vs. log frequency-axis toggle for the graph --
    // "switchable frequency calibration beyond Mel/perceptual" off the
    // original deferred list. A plain bound IVToggleControl on the new
    // kGlobalFreqAxisMel param -- CircleViewControl.h reads this param
    // directly (GetDelegate()->GetParam(kGlobalFreqAxisMel)) every time
    // it converts between screen X and Hz, so no SetParamIdx/retargeting
    // wiring is needed here the way the per-band detail panel needs.
    pGraphics->AttachControl(new IVToggleControl(headerRow.GetGridCell(0, 6, 1, 7).GetCentredInside(104, 34), kGlobalFreqAxisMel, "Mel Axis"));

    // Global (not per-band, not retargeted) -- real MTS-ESP on/off. This
    // is the one global control that stays visible regardless of which
    // tab is open (it's a single always-relevant toggle, not worth its
    // own tab) -- placed in the tab button row's far right rather than
    // the old top-right-of-window spot, since that spot is now inside
    // graphArea.
    //
    // Everything below this point used to float in a fixed top-right
    // stack (Groups/Mod Matrix/LFOs/Fractal/Transform/Image) that
    // predates the circle graph entirely -- once the graph started
    // filling the top half of the window, that stack visually collided
    // with it (confirmed directly from the user's own screenshot: the
    // Group#/Grp Mute/LFO/Formula boxes sitting on top of the black
    // graph area). Moved into a tab strip below the per-band detail
    // panel instead (user's explicit choice between 3 options: "Tabs
        // below detail panel"). Every control below gets pushed into
    // mTabControls[kTabX] as it's created so the tab buttons can
    // Hide()/show a whole tab's controls at once -- see ExoEQ.h's own
    // note on why Hide() per-control rather than a grouped-control API.
    // Concept 02 "GLOBAL FX" drawer: navy backdrop + title behind the tab strip.
    // Shown/hidden together with the tab controls by the Global FX toggle.
    auto* fxBackdrop = pGraphics->AttachControl(new DrawerBackdrop(tabsFull));
    auto* fxTitle = pGraphics->AttachControl(new TextLabelControl(IRECT(tabsFull.L + 18.f, tabsFull.T + 6.f, tabsFull.L + 220.f, tabsFull.T + 26.f), "GLOBAL FX", 12.f, IColor(255, 170, 190, 215)));
    fxBackdrop->Hide(true);
    fxTitle->Hide(true);
    const IRECT tabButtonRow = IRECT(tabsFull.L, tabsFull.T, tabsFull.R, tabsFull.T + 36.f);
    const IRECT tabContentArea = IRECT(tabsFull.L, tabsFull.T + 36.f, tabsFull.R, tabsFull.B);

    auto* mtsEspToggle = pGraphics->AttachControl(new IVToggleControl(tabButtonRow.GetGridCell(0, 6, 1, 9).GetCentredInside(90, 32), kGlobalMtsEspEnabled, "MTS-ESP"));
    // M1 bypass toggle and A/B compare button. Header row is 9 cells: 6 tabs, MTS, Bypass, A/B.
    // T1-4: Bypass and A/B moved to the always-visible header row (cells 2 and 3 of 7).
    // They used to live in the Global FX drawer, where they sat on top of the node stack.
    auto* bypassToggle = pGraphics->AttachControl(new IVToggleControl(headerRow.GetGridCell(0, 2, 1, 7).GetCentredInside(90, 34), kGlobalBypass, "Bypass"));
    // T4-8: Low Motion, over the graph's top-right corner (drawn after the circle view, so it sits on top of it).
    pGraphics->AttachControl(new IVToggleControl(IRECT(graphArea.R - 130.f, graphArea.T + 4.f, graphArea.R - 8.f, graphArea.T + 40.f), kGlobalLowMotion, "Low Motion"));
    auto* abButton = pGraphics->AttachControl(new IVButtonControl(headerRow.GetGridCell(0, 3, 1, 7).GetCentredInside(90, 34), [this](IControl*) { ToggleAB(); }, "A"));
    mAbButton = abButton;
    UpdateAbLabel();

    // Groups panel: ONE fixed set of controls (Select knob + Mute/Gain),
    // retargeted to whichever group the Select knob's own value picks --
    // same "one panel, retargeted" pattern as the 128-band detail panel
    // above, just driven by a plain knob instead of a graph click since
    // there's no spatial layout for 8 groups to click on.
    const IRECT groupsRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* groupSelectKnob = pGraphics->AttachControl(new ExoStepKnob(groupsRow.GetGridCell(0, 0, 1, 3).GetCentredInside(KnobSize(60, groupsRow.GetGridCell(0, 0, 1, 3))), kGlobalGroupSelect, "Group #", exoeq::kNumGroups));
    auto* groupMuteToggle = pGraphics->AttachControl(new IVToggleControl(groupsRow.GetGridCell(0, 1, 1, 3).GetCentredInside(60, 36), GroupParamIdx(0, kGroupMute), "Grp Mute"));
    auto* groupGainKnob = pGraphics->AttachControl(new IVKnobControl(groupsRow.GetGridCell(0, 2, 1, 3).GetCentredInside(KnobSize(60, groupsRow.GetGridCell(0, 2, 1, 3))), GroupParamIdx(0, kGroupGainDb), "Grp Gain"));
    mTabControls[kTabGroups] = { groupSelectKnob, groupMuteToggle, groupGainKnob };

    // Mod matrix panel: same retargeting idea, one row per connection
    // slot, selected by a Select knob rather than kMaxModConnections rows
    // of controls (8 connections * 5 params would be 40 live controls at
    // once -- a single retargeted row keeps this readable, matching how
    // the band panel already avoids showing 128 bands' worth of knobs).
    const IRECT modRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* modSelectKnob = pGraphics->AttachControl(new ExoStepKnob(modRow.GetGridCell(0, 0, 1, 6).GetCentredInside(KnobSize(60, modRow.GetGridCell(0, 0, 1, 6))), kGlobalModConnSelect, "Conn #", kMaxModConnections));
    auto* modOnToggle = pGraphics->AttachControl(new IVToggleControl(modRow.GetGridCell(0, 1, 1, 6).GetCentredInside(60, 36), ModConnParamIdx(0, kModConnEnabled), "Conn On"));
    auto* modSrcKnob = pGraphics->AttachControl(new ExoStepKnob(modRow.GetGridCell(0, 2, 1, 6).GetCentredInside(KnobSize(60, modRow.GetGridCell(0, 2, 1, 6))), ModConnParamIdx(0, kModConnSourceIndex), "Src", exoeq::ModMatrix::kMaxSources));
    auto* modDestMenu = pGraphics->AttachControl(new IVMenuButtonControl(modRow.GetGridCell(0, 3, 1, 6).GetCentredInside(90, 36), ModConnParamIdx(0, kModConnDestination), "Dest"));
    auto* modDepthKnob = pGraphics->AttachControl(new IVKnobControl(modRow.GetGridCell(0, 4, 1, 6).GetCentredInside(KnobSize(60, modRow.GetGridCell(0, 4, 1, 6))), ModConnParamIdx(0, kModConnDepth), "Depth"));
    auto* modTargetKnob = pGraphics->AttachControl(new ExoStepKnob(modRow.GetGridCell(0, 5, 1, 6).GetCentredInside(KnobSize(60, modRow.GetGridCell(0, 5, 1, 6))), ModConnParamIdx(0, kModConnTargetBand), "Target", kMaxBands));
    mTabControls[kTabModMatrix] = { modSelectKnob, modOnToggle, modSrcKnob, modDestMenu, modDepthKnob, modTargetKnob };

    // LFO row: the 4 sources the mod matrix's Src knob (0-3) actually
    // reads -- same fixed-panel-of-4 approach, no retargeting needed since
    // there are only 4 and they all fit on one row.
    const IRECT lfoRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    for (int i = 0; i < kMaxModSources; ++i)
    {
      const float cellW = lfoRow.W() / kMaxModSources;
      const IRECT cell = lfoRow.GetFromLeft(cellW * (i + 1)).GetFromRight(cellW);
      auto* lfoRateKnob = pGraphics->AttachControl(new IVKnobControl(cell.GetGridCell(0, 0, 1, 2).GetCentredInside(KnobSize(50, cell.GetGridCell(0, 0, 1, 2))), LfoParamIdx(i, kLfoRateHz), ("LFO" + std::to_string(i + 1) + " Hz").c_str()));
      auto* lfoWaveMenu = pGraphics->AttachControl(new IVMenuButtonControl(cell.GetGridCell(0, 1, 1, 2).GetCentredInside(70, 32), LfoParamIdx(i, kLfoWaveform), ("LFO" + std::to_string(i + 1) + " Wave").c_str()));
      mTabControls[kTabLfos].push_back(lfoRateKnob);
      mTabControls[kTabLfos].push_back(lfoWaveMenu);
    }

    // Fractal generator: top row of 9 knobs + Formula/Generate/Clear
    // buttons, plus the EXOMUSICA-letter var row beneath it -- the
    // tallest tab's content, so tabContentArea's own 2-row height (see
    // fractalRow/varRow below) is what the strip's total height was
    // sized against. Every knob here is a real automatable global param
    // (see the InitDouble/InitInt/InitEnum calls above) -- only the
    // formula text field and the Generate button are plain UI actions
    // with no param behind them (a formula isn't a numeric value iPlug2
    // params can hold; Generate is a one-shot action, not a state).
    const IRECT fractalRow = IRECT(tabContentArea.L, tabContentArea.T, tabContentArea.R, tabContentArea.T + tabContentArea.H() * 0.5f);
    auto* fSeedKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 0, 1, 10).GetCentredInside(KnobSize(55, fractalRow.GetGridCell(0, 0, 1, 10))), kGlobalFractalSeedHz, "Seed Hz"));
    auto* fRatioKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 1, 1, 10).GetCentredInside(KnobSize(55, fractalRow.GetGridCell(0, 1, 1, 10))), kGlobalFractalRatio, "Ratio"));
    auto* fBranchKnob = pGraphics->AttachControl(new ExoStepKnob(fractalRow.GetGridCell(0, 2, 1, 10).GetCentredInside(KnobSize(55, fractalRow.GetGridCell(0, 2, 1, 10))), kGlobalFractalBranchFactor, "Branch", 4));
    auto* fDepthKnob = pGraphics->AttachControl(new ExoStepKnob(fractalRow.GetGridCell(0, 3, 1, 10).GetCentredInside(KnobSize(55, fractalRow.GetGridCell(0, 3, 1, 10))), kGlobalFractalDepth, "Depth", 4));
    auto* fGainKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 4, 1, 10).GetCentredInside(KnobSize(55, fractalRow.GetGridCell(0, 4, 1, 10))), kGlobalFractalGainDb, "Seed Gain"));
    auto* fDecayKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 5, 1, 10).GetCentredInside(KnobSize(55, fractalRow.GetGridCell(0, 5, 1, 10))), kGlobalFractalGainDecay, "Decay"));
    auto* fJitterKnob = pGraphics->AttachControl(new IVKnobControl(fractalRow.GetGridCell(0, 6, 1, 10).GetCentredInside(KnobSize(55, fractalRow.GetGridCell(0, 6, 1, 10))), kGlobalFractalJitterPct, "Jitter"));
    auto* fSymmToggle = pGraphics->AttachControl(new IVToggleControl(fractalRow.GetGridCell(0, 7, 1, 10).GetCentredInside(55, 32), kGlobalFractalSymmetric, "Symm"));
    auto* fFilterMenu = pGraphics->AttachControl(new IVMenuButtonControl(fractalRow.GetGridCell(0, 8, 1, 10).GetCentredInside(90, 32), kGlobalFractalFilterType, "Filter"));
    mTabControls[kTabFractal] = { fSeedKnob, fRatioKnob, fBranchKnob, fDepthKnob, fGainKnob, fDecayKnob, fJitterKnob, fSymmToggle, fFilterMenu };

    // Formula button: pops a text-entry box over itself (IGraphics'
    // native CreateTextEntry -- same can't-verify-against-the-real-SDK
    // caveat as every other exact-signature note in this project) pre-
    // filled with the current mFractalFormula, and commits whatever's
    // typed back into it. Leaving the box empty restores the knob-driven
    // default path (see FractalGenerator.h).
    auto* formulaButton = pGraphics->AttachControl(new IVButtonControl(fractalRow.GetGridCell(0, 9, 1, 10).GetCentredInside(90, 32), SplashClickActionFunc, "Formula..."));
    // See the TextEntryCallbackControl note above this constructor --
    // CreateTextEntry's completion isn't a lambda argument in this
    // iPlug2 version, it's a virtual call targeting whichever control is
    // passed as its first argument.
    auto* formulaEntryProxy = pGraphics->AttachControl(new TextEntryCallbackControl([this](const char* str) { mFractalFormula = str ? str : ""; }));
    formulaButton->SetActionFunction([this, pGraphics, formulaButton, formulaEntryProxy](IControl* pCaller) {
      pGraphics->CreateTextEntry(*formulaEntryProxy, DEFAULT_TEXT, formulaButton->GetRECT(), mFractalFormula.c_str(), 256);
    });
    mTabControls[kTabFractal].push_back(formulaButton);
    // formulaEntryProxy stays permanently hidden (mHide=true from its own
    // constructor) whichever tab is active -- it's only ever a target for
    // CreateTextEntry, never meant to be seen -- so it's deliberately NOT
    // added to any mTabControls entry; the per-tab Hide() calls below
    // would be redundant on a control that's already always hidden.

    auto* generateButton = pGraphics->AttachControl(new IVButtonControl(fractalRow.GetGridCell(0, 9, 1, 10).GetTranslated(0.f, 40.f), SplashClickActionFunc, "Generate"));
    generateButton->SetActionFunction([this](IControl* pCaller) { GenerateFractalBands(); });
    mTabControls[kTabFractal].push_back(generateButton);
    // "Clear Fractal" -- deactivates exactly the bands this generator
    // made (kBandSource==Fractal), leaving hand-built bands and the
    // other two generators' output untouched. See ExoEQ.h's own note on
    // ClearBandsBySource for why this exists now: the stacking risk
    // flagged since the transform-filter round is real, and this is
    // the direct mitigation for it -- "undo what Generate just did"
    // without hunting for which bands those were.
    auto* clearFractalButton = pGraphics->AttachControl(new IVButtonControl(fractalRow.GetGridCell(0, 9, 1, 10).GetTranslated(0.f, 40.f).GetVShifted(38.f), SplashClickActionFunc, "Clear Fractal"));
    clearFractalButton->SetActionFunction([this](IControl* pCaller) { ClearBandsBySource(EBandSource::Fractal); });
    mTabControls[kTabFractal].push_back(clearFractalButton);

    // 9 custom-variable knobs, one per EXOMUSICA letter (the user's
    // easter egg), one cell each: a knob (the real automatable 0..1
    // param) plus a small "Range" button beneath it that pops a text
    // entry for "min,max" -- parsed and stored into mFractalVarMin/
    // Max[i] on commit (invalid text is ignored, keeping whatever range
    // was already set, rather than silently zeroing it). Labeled by the
    // actual letter the formula box uses, not a generic "Var 1/2/3" --
    // the whole point is that "E" in a knob label and "E" typed into
    // the formula are visibly the same thing, and the 9 labels read
    // E-X-O-M-U-S-I-C-A left to right across the row.
    const IRECT varRow = IRECT(tabContentArea.L, tabContentArea.T + tabContentArea.H() * 0.5f, tabContentArea.R, tabContentArea.B);
    static const char* kVarLetters[kNumFractalCustomVars] = { "E", "X", "O", "M", "U", "S", "I", "C", "A" };
    for (int i = 0; i < kNumFractalCustomVars; ++i)
    {
      const IRECT cell = varRow.GetGridCell(0, i, 1, kNumFractalCustomVars);
      auto* varKnob = pGraphics->AttachControl(new IVKnobControl(cell.GetGridCell(0, 0, 2, 1).GetCentredInside(KnobSize(50, cell.GetGridCell(0, 0, 2, 1))), kGlobalFractalVarStart + i, kVarLetters[i]));
      auto* rangeButton = pGraphics->AttachControl(new IVButtonControl(cell.GetGridCell(1, 0, 2, 1).GetCentredInside(70, 28), SplashClickActionFunc, "Range"));
      mTabControls[kTabFractal].push_back(varKnob);
      mTabControls[kTabFractal].push_back(rangeButton);
      const int varIndex = i; // local copy for the lambda, not the loop variable
      // Same proxy-control fix as the Formula button above -- one proxy
      // per range button, each closing over its own varIndex. Same
      // always-hidden reasoning as formulaEntryProxy above -- not added
      // to mTabControls.
      auto* rangeEntryProxy = pGraphics->AttachControl(new TextEntryCallbackControl([this, varIndex](const char* str) {
        if (!str) return;
        double newMin, newMax;
        // "min,max" -- a plain comma-separated pair, deliberately not a
        // formula (this box sets a RANGE, not a value) so there's
        // nothing to run through ExprEval here. Malformed input (not
        // exactly 2 comma-separated numbers, or min>=max) is ignored --
        // the previous range stays in effect rather than silently
        // collapsing to something unusable.
        if (std::sscanf(str, "%lf,%lf", &newMin, &newMax) == 2 && std::isfinite(newMin) && std::isfinite(newMax) && newMin < newMax)
        {
          mFractalVarMin[varIndex] = newMin;
          mFractalVarMax[varIndex] = newMax;
        }
      }));
      rangeButton->SetActionFunction([this, pGraphics, rangeButton, rangeEntryProxy, varIndex](IControl* pCaller) {
        char current[64];
        std::snprintf(current, sizeof(current), "%g,%g", mFractalVarMin[varIndex], mFractalVarMax[varIndex]);
        pGraphics->CreateTextEntry(*rangeEntryProxy, DEFAULT_TEXT, rangeButton->GetRECT(), current, 64);
      });
    }

    // Transform filter: pivot/exponent + Transform/Clear buttons. Reads
    // the CURRENT active band series rather than a formula -- see
    // GenerateTransformBands's own note on the stacking behavior that
    // follows from reusing the same non-destructive "write into new
    // inactive slots" policy the fractal generator uses. Shares the
    // fractal generator's own cheap-filter-type param
    // (kGlobalFractalFilterType) rather than a duplicate dropdown.
    const IRECT transformRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* tPivotKnob = pGraphics->AttachControl(new IVKnobControl(transformRow.GetGridCell(0, 0, 1, 3).GetCentredInside(KnobSize(60, transformRow.GetGridCell(0, 0, 1, 3))), kGlobalTransformPivotHz, "Pivot Hz"));
    auto* tExponentKnob = pGraphics->AttachControl(new IVKnobControl(transformRow.GetGridCell(0, 1, 1, 3).GetCentredInside(KnobSize(60, transformRow.GetGridCell(0, 1, 1, 3))), kGlobalTransformExponent, "Exponent"));
    auto* transformButton = pGraphics->AttachControl(new IVButtonControl(transformRow.GetGridCell(0, 2, 1, 3).GetCentredInside(90, 36), SplashClickActionFunc, "Transform"));
    transformButton->SetActionFunction([this](IControl* pCaller) { GenerateTransformBands(); });
    // "Clear Transform" -- same policy as Clear Fractal above, scoped
    // to kBandSource==Transform only.
    auto* clearTransformButton = pGraphics->AttachControl(new IVButtonControl(transformRow.GetGridCell(0, 2, 1, 3).GetCentredInside(90, 28).GetVShifted(42.f), SplashClickActionFunc, "Clear Transform"));
    clearTransformButton->SetActionFunction([this](IControl* pCaller) { ClearBandsBySource(EBandSource::Transform); });
    mTabControls[kTabTransform] = { tPivotKnob, tExponentKnob, transformButton, clearTransformButton };

    // Image-to-EQ-curve: 5 knobs + an invert toggle, a file-picker
    // button, a status text showing either the current file's name or
    // the last error, and the Generate/Clear buttons.
    //
    // THE FILE-PICKER BUTTON is the one piece of this round I can't
    // verify against your actual vendored IGraphics headers (same
    // can't-verify-against-the-real-SDK caveat used throughout this
    // project for UI/SDK-signature specifics) -- it calls
    // pGraphics->PromptForFile(...), which is IGraphics' own native
    // "open file" dialog call across all 3 platforms/formats (VST3/AAX/
    // CLAP on Win/Mac). If your iPlug2 version's exact signature differs
    // (argument order, an EFileAction enum name, etc.), paste the
    // compiler error and I'll match it exactly.
    const IRECT imageRow = tabContentArea.GetGridCell(0, 0, 1, 1);
    auto* iNodesKnob = pGraphics->AttachControl(new ExoStepKnob(imageRow.GetGridCell(0, 0, 1, 8).GetCentredInside(KnobSize(50, imageRow.GetGridCell(0, 0, 1, 8))), kGlobalImageNodeCount, "Nodes", 63));
    auto* iMinHzKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 1, 1, 8).GetCentredInside(KnobSize(50, imageRow.GetGridCell(0, 1, 1, 8))), kGlobalImageMinFreqHz, "Min Hz"));
    auto* iMaxHzKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 2, 1, 8).GetCentredInside(KnobSize(50, imageRow.GetGridCell(0, 2, 1, 8))), kGlobalImageMaxFreqHz, "Max Hz"));
    auto* iGainRangeKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 3, 1, 8).GetCentredInside(KnobSize(50, imageRow.GetGridCell(0, 3, 1, 8))), kGlobalImageGainRangeDb, "Gain Range"));
    auto* iResonanceKnob = pGraphics->AttachControl(new IVKnobControl(imageRow.GetGridCell(0, 4, 1, 8).GetCentredInside(KnobSize(50, imageRow.GetGridCell(0, 4, 1, 8))), kGlobalImageResonance, "Resonance"));
    auto* iInvertToggle = pGraphics->AttachControl(new IVToggleControl(imageRow.GetGridCell(0, 5, 1, 8).GetCentredInside(60, 30), kGlobalImageInvertLuminance, "Invert"));
    mTabControls[kTabImage] = { iNodesKnob, iMinHzKnob, iMaxHzKnob, iGainRangeKnob, iResonanceKnob, iInvertToggle };

    // Declared as ITextControl* BEFORE attaching, not via AttachControl's
    // own return value -- this iPlug2 version's AttachControl returns a
    // plain IControl*, not the templated/derived-type return some other
    // versions have, so `auto*` off of it was silently narrowing this to
    // IControl* (the actual compiler error: "'SetStr': is not a member
    // of IControl" below, since SetStr only exists on ITextControl).
    auto* imageStatusText = new ITextControl(imageRow.GetGridCell(0, 6, 1, 8).GetTranslated(0.f, -18.f), "No image selected", DEFAULT_TEXT.WithAlign(EAlign::Center));
    pGraphics->AttachControl(imageStatusText);
    mTabControls[kTabImage].push_back(imageStatusText);

    auto* pickImageButton = pGraphics->AttachControl(new IVButtonControl(imageRow.GetGridCell(0, 6, 1, 8).GetCentredInside(90, 28).GetTranslated(0.f, 14.f), SplashClickActionFunc, "Pick Image..."));
    pickImageButton->SetActionFunction([this, pGraphics, imageStatusText](IControl* pCaller) {
      WDL_String fileName, path;
      // PromptForFile(fileName, path, EFileAction::Open, ""): ""
      // leaves the format filter open to whatever stb_image itself can
      // decode (PNG/JPG/BMP/GIF/etc.) rather than hardcoding one
      // extension -- stbi_load's own format sniffing handles the rest.
      pGraphics->PromptForFile(fileName, path, EFileAction::Open, "");
      if (fileName.GetLength())
      {
        mImageFilePath = fileName.Get();
        mLastImageError.clear();
        // Show just the file name, not the full path -- the path can be
        // long and this is a narrow status line, not a path display.
        const char* base = std::strrchr(mImageFilePath.c_str(), '/');
#ifdef _WIN32
        const char* baseWin = std::strrchr(mImageFilePath.c_str(), '\\');
        if (!base || (baseWin && baseWin > base)) base = baseWin;
#endif
        imageStatusText->SetStr(base ? base + 1 : mImageFilePath.c_str());
      }
    });
    mTabControls[kTabImage].push_back(pickImageButton);

    auto* imageGenerateButton = pGraphics->AttachControl(new IVButtonControl(imageRow.GetGridCell(0, 7, 1, 8).GetCentredInside(90, 36), SplashClickActionFunc, "Generate from Image"));
    imageGenerateButton->SetActionFunction([this, imageStatusText](IControl* pCaller) {
      GenerateImageBands();
      // Surface a failure on the SAME status text the file name shows --
      // a deliberate choice over a separate error control: there's only
      // ever one thing to say about the current image at a time (either
      // "here's the file" or "here's why it didn't work"), so one line
      // covers both without the two ever needing to show at once.
      if (!mLastImageError.empty())
        imageStatusText->SetStr(("Error: " + mLastImageError).c_str());
    });
    mTabControls[kTabImage].push_back(imageGenerateButton);

    // "Clear Image" -- same policy as Clear Fractal/Clear Transform
    // above, scoped to kBandSource==Image only.
    auto* clearImageButton = pGraphics->AttachControl(new IVButtonControl(imageRow.GetGridCell(0, 7, 1, 8).GetCentredInside(90, 28).GetVShifted(42.f), SplashClickActionFunc, "Clear Image"));
    clearImageButton->SetActionFunction([this](IControl* pCaller) { ClearBandsBySource(EBandSource::Image); });
    mTabControls[kTabImage].push_back(clearImageButton);

    // onGroupSelected/onModConnSelected: identical retargeting idea to
    // onBandSelected above, just triggered by the Select knob's own value
    // changing rather than a graph click -- IVKnobControl supports an
    // action-function callback for exactly this ("do something extra when
    // this knob's value changes"), same mechanism BandGraphControl already
    // relies on internally for its own selection callback.
    groupSelectKnob->SetActionFunction([groupMuteToggle, groupGainKnob](IControl* pCaller) {
      const int g = static_cast<int>(pCaller->GetValue() * (exoeq::kNumGroups - 1) + 0.5);
      groupMuteToggle->SetParamIdx(GroupParamIdx(g, kGroupMute));
      groupGainKnob->SetParamIdx(GroupParamIdx(g, kGroupGainDb));
    });
    modSelectKnob->SetActionFunction([modOnToggle, modSrcKnob, modDestMenu, modDepthKnob, modTargetKnob](IControl* pCaller) {
      const int c = static_cast<int>(pCaller->GetValue() * (kMaxModConnections - 1) + 0.5);
      modOnToggle->SetParamIdx(ModConnParamIdx(c, kModConnEnabled));
      modSrcKnob->SetParamIdx(ModConnParamIdx(c, kModConnSourceIndex));
      modDestMenu->SetParamIdx(ModConnParamIdx(c, kModConnDestination));
      modDepthKnob->SetParamIdx(ModConnParamIdx(c, kModConnDepth));
      modTargetKnob->SetParamIdx(ModConnParamIdx(c, kModConnTargetBand));
    });

    // Tab buttons themselves, LAST -- after every tab's controls exist so
    // each button's click handler can capture `this` and call SwitchTab,
    // which just does Hide(t != activeTab) across all 6 mTabControls
    // entries (see ExoEQ.h's member comment). Starts on kTabGroups (index
    // 0) visible, every other tab hidden.
    static const char* kTabLabels[kNumGlobalTabs] = { "Groups", "Mod Matrix", "LFOs", "Fractal", "Transform", "Image" };
    // activeTab's own content still follows the existing per-tab Hide()
    // pattern unchanged; `fxVisible` (above, in the header) is now a
    // SEPARATE, higher-level on/off switch for the strip as a whole --
    // switchTab no longer decides visibility on its own, it just tracks
    // which tab is active so showFx (below) knows which one to reveal.
    // shared_ptr, not a plain stack int captured by reference -- switchTab
    // and globalFxButton's action function both outlive this OnLayout
    // call (they're stored as long-lived std::function closures on their
    // controls), so a `&activeTabIdx` reference to a local would dangle
    // the moment OnLayout returns. Same reasoning as fxVisible above.
    std::vector<IControl*> tabButtons;
    for (int t = 0; t < kNumGlobalTabs; ++t)
    {
      auto* tabButton = pGraphics->AttachControl(new IVButtonControl(tabButtonRow.GetGridCell(0, t, 1, 9).GetCentredInside(110, 32), SplashClickActionFunc, kTabLabels[t]));
      tabButton->SetActionFunction([this, t](IControl* pCaller) { SwitchGlobalTab(t); });
      tabButtons.push_back(tabButton);
    }
    SwitchGlobalTab(kTabGroups);

    // NEW THIS ROUND -- the actual show/hide driven by the header's new
    // "Global FX" button: hides/shows the tab BUTTONS, the MTS-ESP
    // toggle, and whichever tab's content is currently active, all
    // together. Starts hidden (fxVisible's own default, set above) --
    // switchTab(kTabGroups) just above already primed which tab WOULD
    // be showing once revealed, it doesn't itself make anything visible
    // here since every tab's content Hide() call runs before this.
    for (auto* b : tabButtons) b->Hide(true);
    mtsEspToggle->Hide(true);
    for (auto* c : mTabControls[mActiveTab]) if (c) c->Hide(true);
    // The two drawers share one region, so each toggle closes the other.
    auto showGlobalFx = [fxVisible, tabButtons, mtsEspToggle, this, fxBackdrop, fxTitle](bool on) {
      *fxVisible = on;
      fxBackdrop->Hide(!on);
      fxTitle->Hide(!on);
      for (auto* b : tabButtons) b->Hide(!on);
      mtsEspToggle->Hide(!on);
      for (auto* c : mTabControls[mActiveTab]) if (c) c->Hide(!on);
    };
    auto showBandFx = [bandFxVisible, panelControls, selectedBandForPanel, kCompactPanelCount](bool on) {
      *bandFxVisible = on;
      const bool hide = !on || *selectedBandForPanel < 0;
      for (size_t i = kCompactPanelCount; i < panelControls.size(); ++i)
        panelControls[i]->Hide(hide);
    };
    globalFxButton->SetActionFunction([showGlobalFx, showBandFx, fxVisible](IControl* /*pCaller*/) {
      const bool next = !*fxVisible;
      if (next) showBandFx(false);
      showGlobalFx(next);
    });
    bandFxButton->SetActionFunction([showGlobalFx, showBandFx, bandFxVisible](IControl* /*pCaller*/) {
      const bool next = !*bandFxVisible;
      if (next) showGlobalFx(false);
      showBandFx(next);
    });

    WDL_String buildInfoStr;
    GetBuildInfoStr(buildInfoStr, __DATE__, __TIME__);
    // Build info lives in the header strip now. At the bottom-right it landed
    // inside the FX drawer and collided with the Fractal tab's button column.
    // Bottom-left of the graph. In the header it sat on top of the Mel Axis
    // toggle's ON/OFF value; at the bottom-right it was inside the FX drawer.
    pGraphics->AttachControl(new ITextControl(IRECT(graphArea.L + 10.f, graphArea.B - 16.f, graphArea.L + 330.f, graphArea.B - 2.f), buildInfoStr.Get(), IText(9.f, IColor(255, 74, 86, 106), nullptr, EAlign::Near)));

    // Skin pass: every vector control (knob/button/toggle/menu) gets the same
    // Dirty Hardware Lab style. Runs last so it covers everything above.
    const IVStyle exoStyle = ExoSkinStyle();
    for (int ci = 0; ci < pGraphics->NControls(); ++ci)
      if (auto* vb = dynamic_cast<IVectorBase*>(pGraphics->GetControl(ci)))
        vb->SetStyle(exoStyle);
  };
#endif
}

#if IPLUG_DSP
void ExoEQ::OnReset()
{
  const double sr = GetSampleRate();
  for (auto& chain : mCircleChain)
  {
    chain.SetSampleRate(sr);
    chain.Reset();
  }
  mKeyTracker.SetSampleRate(sr);
  mMultiChannelKeyTracker.SetSampleRate(sr);
  mKeyTracker.Reset();

  for (auto& lfo : mModLfos)
    lfo.SetSampleRate(sr);

  // Global FFT stage: sample rate affects which Hz value each FFT bin
  // represents (used inside RunOneFrame's passband test), so it must be
  // pushed on every sample-rate change same as every other sample-rate-
  // aware DSP object above. Reset() clears its OLA/ring-buffer state --
  // same reasoning as mChain[c].Reset() just above: a sample-rate change
  // or hard transport reset should never leave stale audio sitting in
  // an internal buffer to bleed into whatever comes next.
  for (auto& fft : mFftContainer)
  {
    fft.SetSampleRate(sr);
    fft.Reset();
  }

  // Sample rate just changed -- latency in SAMPLES for any future fixed-
  // delay feature would itself change at the same cutoff/window-in-
  // seconds even though nothing about ITS setting moved, so this is
  // exactly one of the two places (alongside wherever such a feature's
  // own setting changes) that must recompute and re-report it. Today
  // this is a no-op (ComputeTotalLatencySamples() returns 0, see
  // ExoEQ.h's comment on it) but costs nothing to call unconditionally.
  RecomputeAndReportLatency();
}

int ExoEQ::ComputeTotalLatencySamples() const
{
  // First real contributor: the global FFT brick-wall stage. Its
  // latency is a fixed sample count (768 at the current 1024/256 FFT
  // config) independent of sample rate -- see FftContainer.h's own
  // note on why an STFT's latency is a frame-count property, not a
  // time property. Exactly one contribution regardless of channel
  // count: both channels' FftBrickwallContainer instances share the
  // identical fixed FFT/hop size, so there's nothing to sum across
  // channels here, unlike a per-band feature would need.
  int total = 0;
  if (GetParam(kGlobalFftEnabled)->Value() > 0.5)
    total += exoeq::FftBrickwallContainer::GetLatencySamples();

  // A future true-oversampling feature adds its own samples-at-current-
  // sample-rate contribution here too (and ONLY here).
  return total;
}

void ExoEQ::RecomputeAndReportLatency()
{
  const int latency = ComputeTotalLatencySamples();
  if (latency != mLastReportedLatencySamples)
  {
    SetLatency(latency);
    mLastReportedLatencySamples = latency;
  }
}

void ExoEQ::ProcessMidiMsg(const IMidiMsg& msg)
{
  const int midiChannel = static_cast<int>(msg.Channel());
  // A note-on with velocity 0 is a note-off per the MIDI spec (running-
  // status optimization) -- route it the same as a real kNoteOff. Phase 1
  // never handled note-off at all; with latch=true above that's still the
  // net effect (NoteOff is a no-op whenever nothing else is held), so this
  // only starts to matter once a future UI control switches latch off.
  if (msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() > 0)
  {
    mKeyTracker.NoteOn(msg.NoteNumber());
    mMultiChannelKeyTracker.NoteOn(midiChannel, msg.NoteNumber());
  }
  else if (msg.StatusMsg() == IMidiMsg::kNoteOff || (msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() == 0))
  {
    mKeyTracker.NoteOff(msg.NoteNumber());
    mMultiChannelKeyTracker.NoteOff(midiChannel, msg.NoteNumber());
  }

  SendMidiMsg(msg);
}

namespace
{
  exoeq::BandSettings ReadBandSettings(Plugin& plug, int bandIndex, double keyTrackedHz)
  {
    const double manualCutoff = plug.GetParam(BandParamIdx(bandIndex, kBandCutoff))->Value();
    const double resonance = plug.GetParam(BandParamIdx(bandIndex, kBandResonance))->Value();
    const double morph = plug.GetParam(BandParamIdx(bandIndex, kBandMorph))->Value() / 100.0;
    const double filterExtra = plug.GetParam(BandParamIdx(bandIndex, kBandFilterExtra))->Value() / 100.0; // only AW Y-Lowpass uses this (powFactor); a no-op for every other type
    const double keyTrackAmt = plug.GetParam(BandParamIdx(bandIndex, kBandKeyTrackAmt))->Value() / 100.0;
    const double cutoffHz = manualCutoff * (1.0 - keyTrackAmt) + keyTrackedHz * keyTrackAmt;

    const bool active = plug.GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    const bool mute = plug.GetParam(BandParamIdx(bandIndex, kBandMute))->Value() > 0.5;

    // Cast straight from the enum param's integer value -- InitEnum's
    // option order above is required to match FilterTypeId exactly, so
    // this cast is safe as long as that stays true.
    const auto filterType = static_cast<exoeq::FilterTypeId>(static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandFilterType))->Value()));

    exoeq::BandSettings s;
    s.filter = exoeq::FilterParams{ cutoffHz, resonance, morph, filterType, filterExtra };
    // Per-band trim gain -- the base value the mod matrix's GainDb
    // destination and node groups' accumulative gain both multiply
    // against (see ExoEQ.h's note on kBandGainDb for why this was
    // missing until now).
    s.gainLinear = std::pow(10.0, plug.GetParam(BandParamIdx(bandIndex, kBandGainDb))->Value() / 20.0);
    // Inactive bands are forced muted regardless of their own Mute param --
    // this is the entire cost of having 128 "slots" exist: an untouched
    // band is just permanently muted (near-zero CPU; see ExoEQ.h) rather
    // than literally absent.
    s.mute = mute || !active;
    s.solo = plug.GetParam(BandParamIdx(bandIndex, kBandSolo))->Value() > 0.5;

    s.delaySendEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandDelayEnabled))->Value() > 0.5;
    s.delaySend.delayMs = plug.GetParam(BandParamIdx(bandIndex, kBandDelayTimeMs))->Value();
    s.delaySend.feedback = plug.GetParam(BandParamIdx(bandIndex, kBandDelayFeedback))->Value();
    s.delaySend.mix = plug.GetParam(BandParamIdx(bandIndex, kBandDelayMix))->Value();

    s.distortionSendEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionEnabled))->Value() > 0.5;
    s.distortionSend.drive = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionDrive))->Value();
    s.distortionSend.character = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionCharacter))->Value();
    s.distortionSend.mix = plug.GetParam(BandParamIdx(bandIndex, kBandDistortionMix))->Value();

    s.freqShiftSendEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftEnabled))->Value() > 0.5;
    s.freqShiftSend.shiftHz = plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftHz))->Value();
    s.freqShiftSend.mix = plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftMix))->Value();

    // Energy2 -- a muted/inactive band's want is forced off the same way
    // `mute` above already forces everything else off, so a band that's
    // muted never occupies one of the global 20 slots just by having the
    // toggle left on from before (see BandChain::RecomputeEnergy2Slots /
    // Band::WantsEnergy2Slot, which checks !mute for exactly this reason).
    s.energy2Enabled = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2Enabled))->Value() > 0.5;
    s.energy2GlitterHiss = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2GlitterHiss))->Value();
    s.energy2RatScrapeFizz = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2RatScrapeFizz))->Value();
    s.energy2Mix = plug.GetParam(BandParamIdx(bandIndex, kBandEnergy2Mix))->Value();

    s.sidechainEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandSidechainEnabled))->Value() > 0.5;
    s.sidechainModOctaves = plug.GetParam(BandParamIdx(bandIndex, kBandSidechainModOctaves))->Value();
    // Envelope-follower attack/release/mode: fixed defaults, not exposed as
    // params this pass (see ExoEQ.h) -- EnvelopeFollowerParams{} default
    // (5ms attack, 100ms release, Peak mode) is already a reasonable
    // general-purpose setting.
    s.sidechainEnv = exoeq::EnvelopeFollowerParams{};

    s.freqShiftSlot = static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandFreqShiftSlot))->Value());
    s.distortionSlot = static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandDistortionSlot))->Value());
    s.delaySlot = static_cast<int>(plug.GetParam(BandParamIdx(bandIndex, kBandDelaySlot))->Value());

    // Node groups: raw int param read straight back as the bitmask --
    // GroupMask's own range (0-255 via InitInt) already matches
    // BandSettings::groupMask's 8-bit usage exactly, so this is a direct
    // cast, same as the filter-type enum cast above.
    s.groupMask = static_cast<unsigned int>(plug.GetParam(BandParamIdx(bandIndex, kBandGroupMask))->Value());
    s.groupEnvEnabled = plug.GetParam(BandParamIdx(bandIndex, kBandGroupEnvEnabled))->Value() > 0.5;
    s.groupEnvModOctaves = plug.GetParam(BandParamIdx(bandIndex, kBandGroupEnvModOctaves))->Value();

    return s;
  }

  // Applies the global mod matrix to one band's already-read (unmodulated)
  // settings, filtering the shared connection list down to just the
  // connections actually targeting this band. ModMatrix::Apply itself
  // takes a flat connection list with no per-band concept (by design --
  // it's generic, see ModMatrix.h), so the per-band targeting this plugin
  // actually needs is done here, one call per band, reusing the same
  // ModMatrix instance (its source values were already set once per block
  // in ProcessBlock -- only the connection list changes per band).
  exoeq::BandSettings ApplyModMatrixForBand(exoeq::ModMatrix& matrix, const exoeq::BandSettings& base,
                                             const std::vector<exoeq::ModMatrix::Connection>& allConnections,
                                             int bandIndex, const int* targetBand)
  {
    std::vector<exoeq::ModMatrix::Connection> forThisBand;
    for (size_t i = 0; i < allConnections.size(); ++i)
    {
      if (targetBand[i] == bandIndex)
        forThisBand.push_back(allConnections[i]);
    }
    if (forThisBand.empty())
      return base; // common case (no connection targets this band) -- skip ModMatrix entirely rather than pay for a copy+no-op Apply

    matrix.SetConnections(forThisBand);
    exoeq::BandSettings modulated;
    matrix.Apply(base, modulated);
    return modulated;
  }
}

void ExoEQ::SnapshotToSlot(int slot)
{
  mAbSlot[slot].resize(kNumParams);
  for (int i = 0; i < kNumParams; ++i)
    mAbSlot[slot][i] = GetParam(i)->Value();
  mAbHasSlot[slot] = true;
}

void ExoEQ::RecallSlot(int slot)
{
  if (!mAbHasSlot[slot]) return;
  // Only send params that differ, so a recall costs one message per changed param.
  for (int i = 0; i < kNumParams; ++i)
  {
    const double target = mAbSlot[slot][i];
    if (GetParam(i)->Value() != target)
      SendParameterValueFromUI(i, GetParam(i)->ToNormalized(target));
  }
  if (GetUI()) GetUI()->SetAllControlsDirty();
}

void ExoEQ::ToggleAB()
{
  SnapshotToSlot(mAbActive);             // keep edits to the slot we are leaving
  mAbActive = 1 - mAbActive;
  if (mAbHasSlot[mAbActive])
    RecallSlot(mAbActive);
  else
    SnapshotToSlot(mAbActive);           // first use of B: it starts as a copy of A
  UpdateAbLabel();
}

void ExoEQ::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // The host can change its channel routing while the plugin is live (FL's wrapper does
  // this when its routing tab opens and closes). When the main stereo pair is not
  // connected, the input or output buffers for those channels are not valid, so
  // touching them is the access violation. Output silence and do nothing else.
  if (NInChansConnected() < 2 || NOutChansConnected() < 2 || !inputs || !outputs)
  {
    for (int c = 0; c < 2; ++c)
      if (outputs && outputs[c])
        for (int i = 0; i < nFrames; ++i)
          outputs[c][i] = 0.0;
    return;
  }

  // M1 bypass: straight passthrough, no DSP. Latency: when bypass is on the audio is
  // not delayed by the FFT stage, so toggling bypass shifts the output timing by that
  // latency. Host PDC still reports the full latency; see the latency note in README.
  if (GetParam(kGlobalBypass)->Value() > 0.5)
  {
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < nFrames; ++i)
        outputs[c][i] = inputs[c][i];
    return;
  }

  // Cheap (one bool param read) to do unconditionally every block --
  // mirrors how mKeyTracker.Tick() below already re-derives its state
  // from current params rather than caching a stale decision. See
  // DSP/KeyTrack.h's SetUseMtsEsp/MtsEspActive for what this actually
  // gates: with no MTS-ESP master running anywhere (the common case),
  // this is a no-op regardless of the param's value.
  mKeyTracker.SetUseMtsEsp(GetParam(kGlobalMtsEspEnabled)->Value() > 0.5);
  mMultiChannelKeyTracker.SetUseMtsEsp(GetParam(kGlobalMtsEspEnabled)->Value() > 0.5);

  // Cheap to call every block (internally a single int compare-and-skip
  // -- see RecomputeAndReportLatency's own comment): this is what makes
  // toggling kGlobalFftEnabled take effect on the host's PDC reporting
  // right away, rather than only on the next OnReset/sample-rate change.
  RecomputeAndReportLatency();

  // Ticked once per block, not per sample: with portamento=0 (the only
  // reachable setting until a UI exposes it) Tick() is an instant jump
  // anyway, so this is still exactly Phase 1's behavior. Once portamento
  // gets a knob, ticking once per block will make the glide step in
  // block-sized increments rather than sample-smooth -- fine at small
  // buffer sizes, worth revisiting (per-sample tick inside the s-loop)
  // before portamento is actually exposed.
  const double keyTrackedHz = mKeyTracker.Tick();
  for (int midiChannel = 0; midiChannel < 16; ++midiChannel)
    mMultiChannelKeyTracker.Tick(midiChannel);

  // Groups: pushed to both chains every block, same "cheap, just always do
  // it" choice ProcessBlock already makes for mKeyTracker.SetUseMtsEsp
  // above -- only kNumGroups (8) param reads + SetGroupSettings calls,
  // nowhere near the per-band filter-coefficient cost the band-skip
  // optimization below exists to avoid.
  for (int g = 0; g < exoeq::kNumGroups; ++g)
  {
    exoeq::GroupSettings gs;
    gs.mute = GetParam(GroupParamIdx(g, kGroupMute))->Value() > 0.5;
    gs.gainLinear = std::pow(10.0, GetParam(GroupParamIdx(g, kGroupGainDb))->Value() / 20.0);
    // Envelope attack/release/mode: fixed defaults, not exposed as params
    // this pass -- same scope-limiting precedent as per-band sidechain's
    // own envelope (see ReadBandSettings's sidechainEnv comment above).
    gs.envelope = exoeq::EnvelopeFollowerParams{};
    mCircleChain[0].SetGroupSettings(g, gs);
    mCircleChain[1].SetGroupSettings(g, gs);
  }

  // Mod matrix: tick the 4 wired LFOs once per block (not per sample --
  // same block-rate-is-fine choice already made for mKeyTracker.Tick()
  // above) and feed them into source slots 0-3; slots 4-7 are reserved/
  // unwired (see ExoEQ.h's kMaxModSources comment) and simply keep
  // whatever SetSourceValue last left them at, which is never (always 0).
  for (int i = 0; i < kMaxModSources; ++i)
  {
    exoeq::LfoParams lp;
    lp.rateHz = GetParam(LfoParamIdx(i, kLfoRateHz))->Value();
    lp.waveform = static_cast<exoeq::LfoWaveform>(static_cast<int>(GetParam(LfoParamIdx(i, kLfoWaveform))->Value()));
    mModLfos[i].SetParams(lp);
    mModMatrix.SetSourceValue(i, mModLfos[i].Tick());
  }

  // Read every connection slot's params once per block (not once per band
  // -- kMaxModConnections is small (8), so this one pass plus the
  // per-band filter in ApplyModMatrixForBand above is far cheaper than
  // re-reading all 8 connections' params again for every one of up to 128
  // bands.
  std::vector<exoeq::ModMatrix::Connection> allModConnections;
  int modConnTargetBand[kMaxModConnections] = {};
  for (int i = 0; i < kMaxModConnections; ++i)
  {
    exoeq::ModMatrix::Connection c;
    c.enabled = GetParam(ModConnParamIdx(i, kModConnEnabled))->Value() > 0.5;
    c.sourceIndex = static_cast<int>(GetParam(ModConnParamIdx(i, kModConnSourceIndex))->Value());
    c.destination = static_cast<exoeq::ModDestination>(static_cast<int>(GetParam(ModConnParamIdx(i, kModConnDestination))->Value()));
    c.depth = GetParam(ModConnParamIdx(i, kModConnDepth))->Value();
    allModConnections.push_back(c);
    modConnTargetBand[i] = static_cast<int>(GetParam(ModConnParamIdx(i, kModConnTargetBand))->Value());
  }

  // PLUG_CHANNEL_IO is "2.2-2": the ONLY layout this plugin declares is
  // 2 main-in + 2 sidechain-in -> 2 out, chosen specifically so there's no
  // runtime ambiguity about which bus is which -- inputs[0..1] are always
  // main L/R, inputs[2..3] are always sidechain L/R, full stop. No
  // sidechain plugged in on the host side just means these read as
  // silence, harmless since every band's sidechainEnabled still defaults
  // to false.
  //
  // Reading an IParam::Value() is cheap regardless of band count; the
  // expensive part this avoids is downstream (SetBandSettings's filter
  // coefficient recalc), not this read.
  //
  // Only bands that are active, OR that just transitioned active->inactive
  // this block (to push the mute flag through exactly once), get their
  // settings pushed into BandChain at all -- an already-inactive band is
  // skipped entirely. Without this, every one of the kMaxBands bands would
  // recompute filter coefficients every block regardless of use.
  int numActiveBands = 0;
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (active) ++numActiveBands;
    if (!active && !mBandWasActive[bandIndex])
      continue;

    const int midiChannelChoice = static_cast<int>(GetParam(BandParamIdx(bandIndex, kBandMidiChannel))->Value());
    const double bandKeyTrackedHz = midiChannelChoice == 0 ? keyTrackedHz : mMultiChannelKeyTracker.CurrentHz(midiChannelChoice - 1);
    const exoeq::BandSettings baseSettings = ReadBandSettings(*this, bandIndex, bandKeyTrackedHz);
    const exoeq::BandSettings settings = ApplyModMatrixForBand(mModMatrix, baseSettings, allModConnections, bandIndex, modConnTargetBand);
    // Circle assignment is read every time a band's settings are pushed
    // (same cost tier as every other per-band param read in this loop) --
    // AssignBandToCircle only does real work (the move) when this value
    // actually changed since last block; otherwise it's exactly as cheap
    // as the flat SetBandSettings call it replaces. Both channels get the
    // SAME circle assignment, same as every other per-band setting here
    // being mirrored to both chains -- a band can't live in a different
    // circle on the left vs right channel.
    const int circleId = static_cast<int>(GetParam(BandParamIdx(bandIndex, kBandCircleIndex))->Value());
    mCircleChain[0].AssignBandToCircle(bandIndex, circleId, settings);
    mCircleChain[1].AssignBandToCircle(bandIndex, circleId, settings);
    mBandWasActive[bandIndex] = active;
  }

  // Deliberately NOT gating on NOutChansConnected() -- see the comment this
  // replaced: that call can report 0 in some hosts' offline/preview render
  // paths even though `outputs` is a real buffer the host expects filled,
  // which silently drops the whole output. Always process a fixed, known
  // channel count instead.
  const int nChans = 2;

  // With zero bands active (a fresh instance's starting state, per the
  // user's explicit "off by default" requirement), BandChain would sum
  // zero bands and output dead silence -- there's no separate "dry"
  // signal path through the parallel-band architecture otherwise. An EQ
  // with no bands added yet should pass audio through unaffected, so
  // that's handled explicitly here rather than silently going quiet.
  const bool bypass = (numActiveBands == 0);

  // The sidechain bus is optional. Hosts (FL Studio's wrapper among them) may pass only
  // the main pair when nothing is routed to sidechain, so inputs[2..3] can be missing.
  // Read them only when the host has connected at least four input channels; otherwise
  // the sidechain signal is silence. This was the crash: reading inputs[2 + c] blindly.
  const bool haveSidechain = NInChansConnected() >= 4 && inputs[2] != nullptr && inputs[3] != nullptr;

  for (int c = 0; c < nChans; c++)
  {
    for (int s = 0; s < nFrames; s++)
    {
      if (bypass)
      {
        outputs[c][s] = inputs[c][s];
        continue;
      }
      // Channel c's main signal pairs with sidechain channel (2+c) when it is present.
      const double sidechainSample = haveSidechain ? static_cast<double>(inputs[2 + c][s]) : 0.0;
      outputs[c][s] = static_cast<sample>(mCircleChain[c].ProcessSample(inputs[c][s], sidechainSample));
    }
  }

  // Global FFT brick-wall stage (P6, first piece) -- runs AFTER every
  // band has already been summed into `outputs` above, per channel.
  // This is the "global stage, not a per-band option" shape: exactly
  // one STFT per channel regardless of how many bands exist. Applies
  // regardless of whether `bypass` was active above -- a bypassed/
  // zero-band instance still has real audio in `outputs` (the dry
  // passthrough signal), which this stage should still shape if the
  // user has turned it on, same as any other global processing stage
  // would apply whether or not any band happens to be active.
  if (GetParam(kGlobalFftEnabled)->Value() > 0.5)
  {
    exoeq::FftBrickwallParams fftParams;
    fftParams.lowCutHz = GetParam(kGlobalFftLowCutHz)->Value();
    fftParams.highCutHz = GetParam(kGlobalFftHighCutHz)->Value();

    EnsureFftScratchCapacity(nFrames);

    for (int c = 0; c < nChans; ++c)
    {
      mFftContainer[c].SetParams(fftParams);
      // Convert this channel's block from iPlug2's `sample` type
      // (typically double) into the float scratch buffer
      // FftBrickwallContainer::Process actually needs, process
      // in-place in that scratch buffer (safe -- Process documents
      // in/out aliasing as fine), then convert back. Two linear passes
      // over nFrames floats is negligible next to the FFT itself, so
      // this isn't a meaningful added cost on top of the stage the
      // user already opted into.
      float* scratch = mFftScratch[c].data();
      for (int s = 0; s < nFrames; ++s)
        scratch[s] = static_cast<float>(outputs[c][s]);

      mFftContainer[c].Process(scratch, scratch, nFrames);

      for (int s = 0; s < nFrames; ++s)
        outputs[c][s] = static_cast<sample>(scratch[s]);
    }
  }
}

void ExoEQ::SwitchGlobalTab(int tab)
{
  mActiveTab = tab;
  for (int t = 0; t < kNumGlobalTabs; ++t)
    for (auto* c : mTabControls[t])
      if (c) c->Hide(t != tab);
}

void ExoEQ::OnUIClose()
{
  // Keep user presets alive past the window: copy them into the plugin-side cache so
  // SerializeState and the next reopen still have them.
  if (mCircleView)
  {
    mPendingUserPresets.clear();
    for (const auto& p : mCircleView->GetUserPresets())
    {
      PendingPresetDef pend{};
      std::snprintf(pend.name, sizeof(pend.name), "%s", p.name);
      pend.bandCount = p.bandCount;
      for (int b = 0; b < 3; ++b)
        pend.bands[b] = { p.bands[b].ordinal, p.bands[b].cutoffHz, p.bands[b].resonance, p.bands[b].forceMorph50 };
      mPendingUserPresets.push_back(pend);
    }
    mHasPendingUserPresets = true;
  }
  // The editor's controls are destroyed with the window, so drop every pointer into
  // them. They are rebuilt on the next OnLayout when the editor opens again.
  mRefreshKnobNames = nullptr;
  mAbButton = nullptr;
  mBandFxButton = nullptr;
  // The circle view is destroyed with the window. Leaving this pointer behind is what
  // crashed on reopen: OnParamChangeUI / serialization dereferenced the freed control.
  if (mCircleView)
  {
    mSavedSelCircle = mCircleView->GetSelectedCircle();
    mSavedSelBand = mCircleView->GetSelectedBand();
  }
  mCircleView = nullptr;
  // Tab controls live in the closed window too. Clearing them stops switchTab()
  // (reachable from the tab buttons and the Global FX button) touching freed controls.
  for (auto& v : mTabControls) v.clear();
}

// T2-7: "Band FX" gets a * while any active band has a band FX send on
// (delay, distortion, freq shift, sidechain, energy2).
// T2-9: active slot in brackets. A "*" appears when the current settings differ from
// the other slot, so you can see a change exists without switching.
void ExoEQ::UpdateAbLabel()
{
  if (!mAbButton) return;
  const int other = 1 - mAbActive;
  bool differs = false;
  if (mAbHasSlot[other])
    for (int i = 0; i < kNumParams && !differs; ++i)
      differs = GetParam(i)->Value() != mAbSlot[other][i];
  const char* base = mAbActive ? "A  [B]" : "[A]  B";
  if (auto* vb = dynamic_cast<IVectorBase*>(mAbButton))
    vb->SetLabelStr(differs ? (std::string(base) + " *").c_str() : base);
}

void ExoEQ::UpdateBandFxBadge()
{
  if (!mBandFxButton) return;
  bool any = false;
  for (int b = 0; b < kMaxBands && !any; ++b)
  {
    if (GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
    any = GetParam(BandParamIdx(b, kBandDelayEnabled))->Value() > 0.5
       || GetParam(BandParamIdx(b, kBandDistortionEnabled))->Value() > 0.5
       || GetParam(BandParamIdx(b, kBandFreqShiftEnabled))->Value() > 0.5
       || GetParam(BandParamIdx(b, kBandSidechainEnabled))->Value() > 0.5
       || GetParam(BandParamIdx(b, kBandEnergy2Enabled))->Value() > 0.5;
  }
  if (auto* vb = dynamic_cast<IVectorBase*>(mBandFxButton))
    vb->SetLabelStr(any ? "Band FX *" : "Band FX");
}

void ExoEQ::OnParamChangeUI(int paramIdx, EParamSource /*source*/)
{
  // Any band param moving invalidates the graph: node position, the response
  // curve, mute dimming, the rail's band counts. Cheap -- SetDirty(false) only
  // marks it for redraw, it does not draw.
  if (paramIdx < kBandParamsStart || paramIdx >= kNumParams) return;
  if (mCircleView) mCircleView->SetDirty(false);
  UpdateBandFxBadge();
  UpdateAbLabel();
  // A filter-type change renames the shaping knobs for that band.
  const int perBand = paramIdx - kBandParamsStart;
  if (perBand >= 0 && (perBand % kNumParamsPerBand) == kBandFilterType && mRefreshKnobNames)
    mRefreshKnobNames();
}

bool ExoEQ::SerializeState(IByteChunk& chunk) const
{
  // Base class first -- every normal automatable param, unchanged from
  // before this feature existed, so an old saved project with no
  // trailing formula string still round-trips through UnserializeState
  // below exactly as it always did (its GetStr just reads past end-of-
  // chunk safely, see there).
  const bool paramsOk = SerializeParams(chunk);
  chunk.PutStr(mFractalFormula.c_str());
  // The 9 custom variables' min/max ranges -- same reasoning as the
  // formula string just above: these are configuration, not automation,
  // so they ride this override rather than a normal param.
  for (int i = 0; i < kNumFractalCustomVars; ++i)
  {
    chunk.Put(&mFractalVarMin[i]);
    chunk.Put(&mFractalVarMax[i]);
  }
  // Image-to-EQ-curve's source file path is DELIBERATELY NOT persisted
  // here (changed from an earlier draft of this feature, per the user's
  // explicit call): picking an image and clicking Generate is a
  // ONE-SHOT action. Its only lasting effect is the ordinary band params
  // it writes -- cutoff/resonance/gain/filter-type/active -- which
  // SerializeParams above already saves and restores exactly like any
  // hand-built band, through the plugin's normal state (and so through
  // the DAW session's saved plugin state, FXB included) with zero extra
  // code needed for that part. There is no reason to also remember WHICH
  // image produced them: the image is a disposable input to a generator,
  // not a live dependency the plugin needs again on reload, and an
  // absolute path saved on one machine would silently fail to resolve on
  // another anyway. mImageFilePath lives only as transient in-session
  // state (so the status line can show the current file while you're
  // working), and intentionally resets to empty on every reload.

  // ROUND 27 -- user-saved preset circles (CircleViewControl::mUserPresets)
  // were session-only until now, flagged as a "bigger future task" when
  // SaveCurrentCircleAsPreset was first added. Persisted field-by-field
  // (not as a raw struct blob) so struct padding/layout differences
  // between builds can't corrupt a saved project -- same reasoning as
  // mFractalVarMin/Max just above using individual chunk.Put calls rather
  // than one chunk.Put(&wholeArray). mCircleView can be null here (no
  // graph built yet, e.g. a headless instantiation) -- writes a 0 count
  // in that case so UnserializeState's read loop stays in sync either way.
  std::vector<CircleViewControl::PresetCircleDef> userPresets;
  if (mCircleView)
    userPresets = mCircleView->GetUserPresets();
  else
    for (const auto& pend : mPendingUserPresets)
    {
      CircleViewControl::PresetCircleDef def{};
      std::snprintf(def.name, sizeof(def.name), "%s", pend.name);
      def.bandCount = pend.bandCount;
      for (int b = 0; b < 3; ++b)
        def.bands[b] = { pend.bands[b].ordinal, pend.bands[b].cutoffHz, pend.bands[b].resonance, pend.bands[b].forceMorph50 };
      userPresets.push_back(def);
    }
  const int32_t presetCount = static_cast<int32_t>(userPresets.size());
  chunk.Put(&presetCount);
  for (const auto& p : userPresets)
  {
    chunk.PutStr(p.name);
    chunk.Put(&p.bandCount);
    for (int b = 0; b < 3; ++b)
    {
      chunk.Put(&p.bands[b].ordinal);
      chunk.Put(&p.bands[b].cutoffHz);
      chunk.Put(&p.bands[b].resonance);
      chunk.Put(&p.bands[b].forceMorph50);
    }
  }

  return paramsOk;
}

int ExoEQ::UnserializeState(const IByteChunk& chunk, int startPos)
{
  int pos = UnserializeParams(chunk, startPos);
  if (pos >= 0 && pos < chunk.Size())
  {
    WDL_String tmp;
    pos = chunk.GetStr(tmp, pos);
    mFractalFormula = tmp.Get() ? tmp.Get() : "";

    // Loading a project saved before the EXOMUSICA variables existed means
    // there's nothing left to read here -- pos lands at/past chunk.Size()
    // and the loop below is simply skipped, leaving every range at its
    // 0..1 passthrough default (the same behavior as if ranging had
    // never been added), rather than reading garbage bytes.
    for (int i = 0; i < kNumFractalCustomVars && pos >= 0 && pos + static_cast<int>(sizeof(double)) * 2 <= chunk.Size(); ++i)
    {
      pos = chunk.Get(&mFractalVarMin[i], pos);
      pos = chunk.Get(&mFractalVarMax[i], pos);
    }

    // mImageFilePath is never read back here -- it's deliberately not
    // part of the saved chunk at all (see SerializeState's note above).
    // Every reload starts with no image selected; the bands an earlier
    // Generate-from-Image click produced are already restored above, as
    // ordinary params, with nothing image-specific required to get them
    // back.
    mImageFilePath.clear();

    // ROUND 27 -- read back the user-saved preset circles, mirroring
    // SerializeState's field-by-field write above. Bounds-checked the
    // same way the fractal-vars loop above is: a project saved before
    // this feature existed (or any truncated/corrupt tail) just leaves
    // pos at/past chunk.Size() and the loop below never runs, so the
    // tray falls back to empty user presets rather than reading garbage.
    std::vector<CircleViewControl::PresetCircleDef> userPresets;
    if (pos >= 0 && pos + static_cast<int>(sizeof(int32_t)) <= chunk.Size())
    {
      int32_t presetCount = 0;
      pos = chunk.Get(&presetCount, pos);
      // The GUI deliberately caps user presets at eight. Clamp the count
      // before entering the read loop so a corrupt or hostile state chunk
      // cannot request an unbounded number of string/record reads. The
      // bounds checks below still handle truncated chunks safely.
      constexpr int32_t kMaxSerializedUserPresets = 8;
      presetCount = std::clamp(presetCount, int32_t(0), kMaxSerializedUserPresets);
      for (int32_t i = 0; i < presetCount && pos >= 0 && pos < chunk.Size(); ++i)
      {
        CircleViewControl::PresetCircleDef def{};
        WDL_String name;
        pos = chunk.GetStr(name, pos);
        std::snprintf(def.name, sizeof(def.name), "%s", name.Get() ? name.Get() : "");
        pos = chunk.Get(&def.bandCount, pos);
        for (int b = 0; b < 3 && pos >= 0; ++b)
        {
          pos = chunk.Get(&def.bands[b].ordinal, pos);
          pos = chunk.Get(&def.bands[b].cutoffHz, pos);
          pos = chunk.Get(&def.bands[b].resonance, pos);
          pos = chunk.Get(&def.bands[b].forceMorph50, pos);
        }
        userPresets.push_back(def);
      }
    }
    if (mCircleView)
    {
      mCircleView->SetUserPresets(userPresets);
      mHasPendingUserPresets = false;
    }
    else
    {
      // GUI not open yet -- stash for OnLayout to apply once circleView
      // exists (see mPendingUserPresets's own note in ExoEQ.h).
      mPendingUserPresets.clear();
      for (const auto& p : userPresets)
      {
        PendingPresetDef pend{};
        std::snprintf(pend.name, sizeof(pend.name), "%s", p.name);
        pend.bandCount = p.bandCount;
        for (int b = 0; b < 3; ++b)
          pend.bands[b] = { p.bands[b].ordinal, p.bands[b].cutoffHz, p.bands[b].resonance, p.bands[b].forceMorph50 };
        mPendingUserPresets.push_back(pend);
      }
      mHasPendingUserPresets = true;
    }
  }
  else
  {
    // Loading a project saved before this feature existed -- no
    // trailing string to read, stay at the knob-driven default.
    mFractalFormula.clear();
    mImageFilePath.clear();
    if (mCircleView) mCircleView->SetUserPresets({});
    else { mPendingUserPresets.clear(); mHasPendingUserPresets = true; }
  }
  return pos;
}

void ExoEQ::GenerateFractalBands()
{
  const double sr = GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0;

  exoeq::FractalGenParams fp;
  fp.seedFreqHz = GetParam(kGlobalFractalSeedHz)->Value();
  fp.ratio = GetParam(kGlobalFractalRatio)->Value();
  fp.branchFactor = static_cast<int>(GetParam(kGlobalFractalBranchFactor)->Value());
  fp.depth = static_cast<int>(GetParam(kGlobalFractalDepth)->Value());
  fp.seedGainDb = GetParam(kGlobalFractalGainDb)->Value();
  fp.gainDecayPerLevel = GetParam(kGlobalFractalGainDecay)->Value();
  fp.resonance = GetParam(kGlobalFractalResonance)->Value();
  fp.jitterPct = GetParam(kGlobalFractalJitterPct)->Value();
  fp.jitterSeed = static_cast<unsigned int>(GetParam(kGlobalFractalJitterSeed)->Value());
  fp.symmetric = GetParam(kGlobalFractalSymmetric)->Value() > 0.5;
  fp.formula = mFractalFormula;
  // Scale each custom knob's 0..1 position into its user-set range
  // BEFORE handing it to the generator -- see FractalGenParams::
  // customVarValue's own note on why the generator never sees a raw
  // 0..1 value for any of E/X/O/M/U/S/I/C/A.
  for (int i = 0; i < kNumFractalCustomVars; ++i)
  {
    const double knob01 = GetParam(kGlobalFractalVarStart + i)->Value();
    fp.customVarValue[i] = mFractalVarMin[i] + knob01 * (mFractalVarMax[i] - mFractalVarMin[i]);
  }

  const auto nodes = exoeq::GenerateFractalSeries(fp, sr);
  WriteGeneratedNodesIntoInactiveBands(nodes, CheapFilterOrdinalFromParam(), EBandSource::Fractal);
}

void ExoEQ::GenerateTransformBands()
{
  const double sr = GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0;

  // Read every CURRENTLY ACTIVE band's cutoff/resonance/gain into a node
  // series -- this is the "transform filter" input (vs. the fractal
  // generator's closed-form formula), per the P5 scope doc's own plan
  // for the shared machine. Deliberately does NOT read sends, key-track,
  // Energy2, or group membership -- this is a frequency-domain remap of
  // the EQ CURVE, not a full clone of every per-band feature; a
  // transformed band starts as a plain filter band at its new frequency,
  // same as a fresh fractal-generated one.
  std::vector<exoeq::GeneratedNode> input;
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    if (GetParam(BandParamIdx(bandIndex, kBandActive))->Value() <= 0.5) continue;
    exoeq::GeneratedNode node;
    node.freqHz = GetParam(BandParamIdx(bandIndex, kBandCutoff))->Value();
    node.resonance = GetParam(BandParamIdx(bandIndex, kBandResonance))->Value();
    node.gainDb = GetParam(BandParamIdx(bandIndex, kBandGainDb))->Value();
    input.push_back(node);
  }
  if (input.empty()) return; // nothing active to transform -- a silent no-op, same as the fractal generator's empty-series early-return

  exoeq::TransformParams tp;
  tp.pivotHz = GetParam(kGlobalTransformPivotHz)->Value();
  tp.exponent = GetParam(kGlobalTransformExponent)->Value();

  const auto output = exoeq::ApplyTransform(input, tp, sr);
  WriteGeneratedNodesIntoInactiveBands(output, CheapFilterOrdinalFromParam(), EBandSource::Transform);
}

void ExoEQ::GenerateImageBands()
{
  // Silent no-op with no file picked yet -- same shape as the transform
  // filter's empty-input early-return, just at the "haven't even tried"
  // stage rather than "tried and got nothing to work with".
  if (mImageFilePath.empty())
  {
    mLastImageError = "no image selected";
    return;
  }

  const double sr = GetSampleRate() > 0.0 ? GetSampleRate() : 44100.0;

  exoeq::ImageToEqParams ip;
  ip.nodeCount = static_cast<int>(GetParam(kGlobalImageNodeCount)->Value());
  ip.minFreqHz = GetParam(kGlobalImageMinFreqHz)->Value();
  ip.maxFreqHz = GetParam(kGlobalImageMaxFreqHz)->Value();
  ip.gainRangeDb = GetParam(kGlobalImageGainRangeDb)->Value();
  ip.resonance = GetParam(kGlobalImageResonance)->Value();
  ip.invertLuminance = GetParam(kGlobalImageInvertLuminance)->Value() > 0.5;

  const auto result = exoeq::GenerateFromImageFile(mImageFilePath, ip, sr);
  if (!result.ok)
  {
    // No band touched, no param changed -- a bad path or an
    // unsupported/corrupt file leaves the plugin exactly as it was
    // before the click, same "fail loud but fail safe" policy as the
    // formula evaluator's own try/catch fallback in FractalGenerator.h.
    mLastImageError = result.errorMessage;
    return;
  }

  mLastImageError.clear();
  WriteGeneratedNodesIntoInactiveBands(result.nodes, CheapFilterOrdinalFromParam(), EBandSource::Image);
}

int ExoEQ::CheapFilterOrdinalFromParam() const
{
  // Cheap-filter-only mapping (user's explicit requirement, originally
  // for the fractal generator, now shared with the transform filter) --
  // all 4 options are single-StateVariableFilter dropdown types,
  // matching DSP/FilterCore.h's FilterTypeId ordinals exactly (see the
  // InitEnum call above for the option list these indices correspond
  // to: 0=LP12, 1=HP12, 2=BP12, 3=Notch12). Never maps to an Airwindows/
  // character type, by design -- those are far more expensive
  // per-instance than a single SVF, and a generator that can produce up
  // to 64 bands at once is exactly where that cost would add up.
  static const int kFilterOrdinal[4] = {
    static_cast<int>(exoeq::FilterTypeId::LP12),
    static_cast<int>(exoeq::FilterTypeId::HP12),
    static_cast<int>(exoeq::FilterTypeId::BP12),
    static_cast<int>(exoeq::FilterTypeId::Notch12)
  };
  const int filterChoice = std::clamp(static_cast<int>(GetParam(kGlobalFractalFilterType)->Value()), 0, 3);
  return kFilterOrdinal[filterChoice];
}

void ExoEQ::WriteGeneratedNodesIntoInactiveBands(const std::vector<exoeq::GeneratedNode>& nodes, int filterOrdinal, EBandSource source)
{
  if (nodes.empty()) return;

  // Non-destructive by default (confirmed with the user): write into the
  // first N currently-INACTIVE band slots, activating them, rather than
  // touching any band already in use. If there aren't enough free slots
  // for every generated node, the rest are simply dropped -- silently
  // truncating into bands you didn't intend to touch would be worse.
  //
  // KNOWN CONSEQUENCE, still true even with the source tag below: since
  // this activates new bands rather than EXCLUDING them from later
  // input, a band written here still reads as an ordinary active band
  // to the NEXT generator click -- including this same one. The tag
  // doesn't stop that (clicking Transform twice still compounds); what
  // it adds is the ability to find and clear exactly what a given
  // generator made afterward (see ClearBandsBySource below), as a real
  // mitigation rather than a full fix. See each generator's own header
  // comment for the un-changed stacking discussion.
  //
  // MOD-MATRIX COLLISION MITIGATION (new this round -- see the Features
  // Doc's own note on this risk): mod connections target a band by raw
  // index, so landing a generated node on an index a DORMANT (target
  // inactive, so currently silent) connection already points at would
  // wake that connection up unexpectedly the moment this activates the
  // band. To avoid that, slots are filled in TWO PASSES: first every
  // inactive band NOT targeted by any currently-enabled mod connection,
  // then (only if more nodes remain than that leaves room for) the
  // remaining inactive bands regardless of targeting -- so a collision
  // only ever happens when there's truly no untargeted room left, not
  // as the default outcome. This doesn't change WHERE an enabled
  // connection's target band physically sits, and it doesn't touch
  // connections at all -- it just steers new generator output away from
  // indices that already have one waiting, when it can.
  std::array<bool, kMaxBands> targetedByEnabledModConn{};
  for (int c = 0; c < kMaxModConnections; ++c)
  {
    if (GetParam(ModConnParamIdx(c, kModConnEnabled))->Value() <= 0.5) continue;
    const int target = std::clamp(static_cast<int>(GetParam(ModConnParamIdx(c, kModConnTargetBand))->Value()), 0, kMaxBands - 1);
    targetedByEnabledModConn[static_cast<size_t>(target)] = true;
  }

  auto writeOneBand = [&](int bandIndex, const exoeq::GeneratedNode& node)
  {
    SetParameterValue(BandParamIdx(bandIndex, kBandCutoff), GetParam(BandParamIdx(bandIndex, kBandCutoff))->ToNormalized(node.freqHz));
    SetParameterValue(BandParamIdx(bandIndex, kBandResonance), GetParam(BandParamIdx(bandIndex, kBandResonance))->ToNormalized(node.resonance));
    SetParameterValue(BandParamIdx(bandIndex, kBandGainDb), GetParam(BandParamIdx(bandIndex, kBandGainDb))->ToNormalized(node.gainDb));
    SetParameterValue(BandParamIdx(bandIndex, kBandFilterType), GetParam(BandParamIdx(bandIndex, kBandFilterType))->ToNormalized(static_cast<double>(filterOrdinal)));
    SetParameterValue(BandParamIdx(bandIndex, kBandSource), GetParam(BandParamIdx(bandIndex, kBandSource))->ToNormalized(static_cast<double>(source)));
    SetParameterValue(BandParamIdx(bandIndex, kBandMute), 0.0);
    SetParameterValue(BandParamIdx(bandIndex, kBandActive), 1.0);
  };

  size_t nodeIdx = 0;

  // Pass 1: inactive AND untargeted slots -- the preferred, collision-free case.
  for (int bandIndex = 0; bandIndex < kMaxBands && nodeIdx < nodes.size(); ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (active || targetedByEnabledModConn[static_cast<size_t>(bandIndex)]) continue;
    writeOneBand(bandIndex, nodes[nodeIdx]);
    ++nodeIdx;
  }

  // Pass 2: only runs if pass 1 didn't have enough room -- falls back to
  // inactive-but-targeted slots rather than dropping nodes that would
  // otherwise fit. This is the one case a collision can still happen;
  // it's a real tradeoff (silently dropping the node instead would avoid
  // it entirely), chosen because "the generator did less than you asked
  // because of an unrelated mod connection you may have forgotten about"
  // is a more confusing outcome than "this specific edge case can still
  // wake a connection up" -- flagged either way in this round's README.
  for (int bandIndex = 0; bandIndex < kMaxBands && nodeIdx < nodes.size(); ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (active || !targetedByEnabledModConn[static_cast<size_t>(bandIndex)]) continue;
    writeOneBand(bandIndex, nodes[nodeIdx]);
    ++nodeIdx;
  }

  if (GetUI())
    GetUI()->SetAllControlsDirty();
}

void ExoEQ::ClearBandsBySource(EBandSource source)
{
  // Deliberately never touches EBandSource::HandBuilt bands, regardless
  // of what's passed -- this function only ever clears a GENERATOR's
  // own output, never a user's hand-built work. (A caller passing
  // HandBuilt here would be a bug in ExoEQ.cpp itself, not something a
  // user can trigger from the UI -- there's no "Clear Hand-Built"
  // button.)
  if (source == EBandSource::HandBuilt) return;

  bool anyCleared = false;
  for (int bandIndex = 0; bandIndex < kMaxBands; ++bandIndex)
  {
    const bool active = GetParam(BandParamIdx(bandIndex, kBandActive))->Value() > 0.5;
    if (!active) continue;

    const auto bandSource = static_cast<EBandSource>(static_cast<int>(GetParam(BandParamIdx(bandIndex, kBandSource))->Value()));
    if (bandSource != source) continue;

    // Deactivating (rather than zeroing cutoff/gain/etc.) matches how
    // every other "remove this band" path in this plugin already works
    // -- an inactive band is forced muted and costs almost nothing per
    // block (see kBandActive's own note in ExoEQ.h), and its old
    // numeric values staying in place until overwritten is harmless
    // since they're never read while inactive.
    SetParameterValue(BandParamIdx(bandIndex, kBandActive), 0.0);
    SetParameterValue(BandParamIdx(bandIndex, kBandSource), GetParam(BandParamIdx(bandIndex, kBandSource))->ToNormalized(static_cast<double>(EBandSource::HandBuilt)));
    anyCleared = true;
  }

  if (anyCleared && GetUI())
    GetUI()->SetAllControlsDirty();
}

void ExoEQ::EnsureFftScratchCapacity(int nFrames)
{
  for (auto& scratch : mFftScratch)
  {
    if (static_cast<int>(scratch.size()) < nFrames)
      scratch.resize(static_cast<size_t>(nFrames));
  }
}
#endif
