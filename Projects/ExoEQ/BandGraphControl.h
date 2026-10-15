#pragma once

#include "IControl.h"
#include "DSP/FrequencyResponse.h"
#include "../ExoEQ.h" // BandParamIdx, kMaxBands, EBandParamOffset
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>

// BandGraphControl.h
// UI ONLY -- not DSP, not unit-tested, and NOT verified against the real
// iPlug2/IGraphics SDK (it isn't vendored in the sandbox this project is
// built in). Everything in DSP/FrequencyResponse.h that this draws from
// IS tested; this file is best-effort against my knowledge of iPlug2's
// IControl/IGraphics API and may need small fixes once it actually
// compiles against the real headers -- paste the exact error if so,
// same as every other build issue so far.
//
// Pro-Q-style spectrum graph: draws the combined complex frequency
// response (DSP/FrequencyResponse.h, correct for parallel bands) as a
// curve, one draggable node per ACTIVE band positioned at (its own
// cutoff frequency, its own magnitude at that frequency). Click-drag a
// node: horizontal = cutoff (log-frequency), vertical = resonance.
// Click empty space: activates the first currently-inactive band slot
// at the clicked frequency -- this IS the "add a band" UI, there's no
// separate button for it.

class BandGraphControl : public iplug::igraphics::IControl
{
public:
  using IControl = iplug::igraphics::IControl;
  using IRECT = iplug::igraphics::IRECT;
  using IGraphics = iplug::igraphics::IGraphics;
  using IMouseMod = iplug::igraphics::IMouseMod;

  // `onBandSelected` is called whenever the user selects a band (click or
  // drag start) -- the detail panel uses this to retarget its controls.
  BandGraphControl(const IRECT& bounds, double sampleRate, std::function<void(int)> onBandSelected)
  : IControl(bounds, iplug::kNoParameter)
  , mSampleRate(sampleRate)
  , mOnBandSelected(std::move(onBandSelected))
  {
    mIgnoreMouse = false;
  }

  void SetSampleRate(double sr) { mSampleRate = sr; }

  void Draw(IGraphics& g) override
  {
    g.FillRect(iplug::igraphics::COLOR_BLACK, mRECT);

    // Gather active bands once per draw -- GetDelegate() is the plugin
    // instance (standard iPlug2 pattern for a control reading params it
    // isn't bound to via mParamIdx).
    std::vector<exoeq::BandResponseInput> curveBands;
    struct NodeInfo { int bandIndex; double cutoffHz; double ownDb; };
    std::vector<NodeInfo> nodes;

    for (int b = 0; b < kMaxBands; ++b)
    {
      const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
      if (!active) continue;
      const bool mute = GetDelegate()->GetParam(BandParamIdx(b, kBandMute))->Value() > 0.5;
      const double cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
      const double resonance = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance))->Value();
      const double morph = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph))->Value() / 100.0;
      const double filterExtra = GetDelegate()->GetParam(BandParamIdx(b, kBandFilterExtra))->Value() / 100.0; // only AW Y-Lowpass uses this; a no-op for every other type
      // Must read the real filter type, not assume MorphZ -- otherwise
      // the curve/node preview would silently draw the wrong shape for
      // every band using one of the dropdown filter types (FilterTypes.h).
      const auto filterType = static_cast<exoeq::FilterTypeId>(static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value()));

      exoeq::FilterParams fp{ cutoffHz, resonance, morph, filterType, filterExtra };
      curveBands.push_back({ fp, mute, 1.0 });

      const double ownDb = exoeq::MagnitudeToDb(exoeq::ComplexFilterResponse(fp, mSampleRate, cutoffHz));
      nodes.push_back({ b, cutoffHz, ownDb });
    }

    // Combined curve: log-spaced frequency points, correct complex sum.
    constexpr int kNumCurvePoints = 200;
    std::vector<std::pair<float, float>> curvePoints;
    curvePoints.reserve(kNumCurvePoints);
    for (int i = 0; i < kNumCurvePoints; ++i)
    {
      const double t = static_cast<double>(i) / (kNumCurvePoints - 1);
      const double freqHz = FreqAtT(t);
      const double db = curveBands.empty() ? 0.0 : exoeq::MagnitudeToDb(exoeq::ComplexCombinedResponse(curveBands, mSampleRate, freqHz));
      curvePoints.emplace_back(FreqToX(freqHz), DbToY(db));
    }
    for (size_t i = 1; i < curvePoints.size(); ++i)
      g.DrawLine(iplug::igraphics::COLOR_GREEN, curvePoints[i - 1].first, curvePoints[i - 1].second, curvePoints[i].first, curvePoints[i].second, nullptr, 2.f);

    // Zero dB reference line.
    const float zeroY = DbToY(0.0);
    g.DrawLine(iplug::igraphics::IColor(255, 80, 80, 80), mRECT.L, zeroY, mRECT.R, zeroY);

    for (const auto& n : nodes)
    {
      const float x = FreqToX(n.cutoffHz);
      const float y = DbToY(n.ownDb);
      const bool selected = (n.bandIndex == mSelectedBand);
      const float radius = selected ? 7.f : 5.f;
      g.FillCircle(selected ? iplug::igraphics::COLOR_WHITE : iplug::igraphics::COLOR_YELLOW, x, y, radius);
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    const int hit = FindNodeNear(x, y);
    if (hit >= 0)
    {
      mDraggedBand = hit;
      SelectBand(hit);
    }
    else
    {
      // Empty space: activate the first inactive band slot at the clicked
      // frequency AND resonance -- this is the entire "add a band"
      // interaction. Setting resonance here too (not just cutoff) is what
      // makes the new node actually appear where you clicked instead of
      // always spawning at the default 0.707 resonance's height.
      for (int b = 0; b < kMaxBands; ++b)
      {
        const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
        if (active) continue;
        const double freqHz = std::clamp(XToFreq(x), 20.0, 20000.0);
        PushParamValue(BandParamIdx(b, kBandCutoff), freqHz, 20.0, 20000.0);
        PushParamValue(BandParamIdx(b, kBandResonance), ResonanceFromY(y), 0.05, 20.0);
        PushParamValue(BandParamIdx(b, kBandActive), 1.0, 0.0, 1.0);
        mDraggedBand = b;
        SelectBand(b);
        break;
      }
    }
    SetDirty(false);
  }

  void OnMouseDrag(float x, float y, float /*dX*/, float /*dY*/, const IMouseMod& /*mod*/) override
  {
    if (mDraggedBand < 0) return;
    const double freqHz = std::clamp(XToFreq(x), 20.0, 20000.0);
    PushParamValue(BandParamIdx(mDraggedBand, kBandCutoff), freqHz, 20.0, 20000.0);
    PushParamValue(BandParamIdx(mDraggedBand, kBandResonance), ResonanceFromY(y), 0.05, 20.0);

    SetDirty(false);
  }

  void OnMouseUp(float /*x*/, float /*y*/, const IMouseMod& /*mod*/) override { mDraggedBand = -1; }

private:
  // Vertical position maps to resonance -- higher on screen = higher Q.
  // Shared by OnMouseDown (placing a new node) and OnMouseDrag (reshaping
  // an existing one) so both agree on where a given Y lands.
  double ResonanceFromY(float y) const
  {
    const double t = std::clamp(static_cast<double>(1.0f - (y - mRECT.T) / mRECT.H()), 0.0, 1.0);
    return 0.05 + t * (20.0 - 0.05); // matches the Resonance param's own range (ExoEQ.cpp InitDouble)
  }

  void SelectBand(int bandIndex)
  {
    mSelectedBand = bandIndex;
    if (mOnBandSelected) mOnBandSelected(bandIndex);
  }

  // Pushes a UI-driven parameter change. SendParameterValueFromUI expects
  // a NORMALIZED [0,1] value in some iPlug2 versions and others take the
  // real value directly with a separate normalize step -- this assumes
  // the normalized form (ToNormalizedParam equivalent done inline via the
  // min/max here) since that's the more common call shape. If this is
  // the wrong overload/normalization for the vendored SDK version,
  // whatever compiler error comes back will point at exactly this line.
  void PushParamValue(int paramIdx, double realValue, double rangeMin, double rangeMax)
  {
    const double normalized = std::clamp((realValue - rangeMin) / (rangeMax - rangeMin), 0.0, 1.0);
    GetDelegate()->SendParameterValueFromUI(paramIdx, normalized);
  }

  // NOT const: GetDelegate() isn't const-qualified in this iPlug2 version,
  // so a const FindNodeNear can't call it (this was the actual bug behind
  // the whole cascade of "const object must be initialized" errors below
  // -- those were a side effect of the GetDelegate() call failing, not
  // real problems with those individual lines).
  int FindNodeNear(float x, float y)
  {
    constexpr float kHitRadius = 10.f;
    int best = -1;
    float bestDist = kHitRadius;
    for (int b = 0; b < kMaxBands; ++b)
    {
      const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
      if (!active) continue;
      const double cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
      const double resonance = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance))->Value();
      const double morph = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph))->Value() / 100.0;
      const double filterExtra = GetDelegate()->GetParam(BandParamIdx(b, kBandFilterExtra))->Value() / 100.0;
      const auto filterType = static_cast<exoeq::FilterTypeId>(static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value()));
      const double ownDb = exoeq::MagnitudeToDb(exoeq::ComplexFilterResponse(exoeq::FilterParams{cutoffHz, resonance, morph, filterType, filterExtra}, mSampleRate, cutoffHz));
      const float nx = FreqToX(cutoffHz);
      const float ny = DbToY(ownDb);
      const float dist = std::sqrt((x - nx) * (x - nx) + (y - ny) * (y - ny));
      if (dist < bestDist) { bestDist = dist; best = b; }
    }
    return best;
  }

  // Log-frequency mapping, 20Hz-20kHz across the control's width.
  float FreqToX(double freqHz) const
  {
    const double t = std::log2(std::clamp(freqHz, 20.0, 20000.0) / 20.0) / std::log2(20000.0 / 20.0);
    return static_cast<float>(mRECT.L + t * mRECT.W());
  }
  double XToFreq(float x) const
  {
    // (x - mRECT.L) / mRECT.W() is float; clamp needs all three arguments
    // the same type, hence the explicit cast rather than mixing it with
    // the 0.0/1.0 double literals directly.
    const double t = std::clamp(static_cast<double>((x - mRECT.L) / mRECT.W()), 0.0, 1.0);
    return 20.0 * std::pow(20000.0 / 20.0, t);
  }
  double FreqAtT(double t) const { return 20.0 * std::pow(20000.0 / 20.0, t); }

  // dB mapping: +12dB at the top, -36dB at the bottom -- a reasonable
  // default range for a resonant filter curve; worth revisiting once
  // this is actually on screen.
  float DbToY(double db) const
  {
    constexpr double kTopDb = 12.0, kBottomDb = -36.0;
    const double t = std::clamp((db - kBottomDb) / (kTopDb - kBottomDb), 0.0, 1.0);
    return static_cast<float>(mRECT.B - t * mRECT.H());
  }

  double mSampleRate;
  int mSelectedBand = 0;
  int mDraggedBand = -1;
  std::function<void(int)> mOnBandSelected;
};
