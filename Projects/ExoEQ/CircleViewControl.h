#pragma once

#include "IControl.h"
#include "FreqFormat.h"
#include "MorphSteps.h"
#include "ExoEQ.h" // BandParamIdx, kMaxBands, EBandParamOffset, kMaxCircles
#include "DSP/CircleChain.h"
#include "DSP/FrequencyResponse.h" // measured complex response of the REAL band filter (see that header)
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// CircleViewControl.h
// UI ONLY -- not DSP, not unit-tested, and NOT verified against the real
// iPlug2/IGraphics SDK (same standing caveat as BandGraphControl.h, which
// this replaces in ExoEQ.cpp's layout -- "this view is the start of our
// GUI, it replaces the band list" per the user's own confirmation after
// the mockup round on the design-canvas artifact).
//
// THIS IS ROUND 1 OF THE REAL GUI, NOT THE WHOLE THING. What's wired here:
//   - Depth-stacked circle overview (rolodex scale/opacity falloff),
//     each circle's own hue from its bands' average cutoff frequency
//     (red=low .. cyan=high, routed around yellow-green), a fixed amber
//     accent for every band inside whichever circle, dimmed non-selected
//     circles, an 80%-black scrim behind the selected circle's footprint.
//   - Right-docked rail: one row per circle that actually has a band in
//     it, max 8 visible with an alpha fade past that, click a row to
//     select that circle.
//   - Selected circle unrolled to a 180 degree gauge, Mel/perceptual
//     frequency axis (20Hz..20kHz left to right), its own bands drawn on
//     the arc.
//   - Tap empty space on the gauge = create a Bell (point) band. Click-
//     drag-release = create a Band-Pass (range) band, release point sets
//     bandwidth via resonance. Both land in the CURRENTLY SELECTED circle
//     (kBandCircleIndex), first free inactive band slot.
//
// STALE NOTE (kept for history, see later rounds' README entries for
// what's actually still missing): most of what this comment originally
// deferred -- rail inertia-drag, the crosshair/Hz readout, shift-click
// mute + Solo/Mute buttons, switchable frequency calibration, the
// gesture-direction-sorted filter-type picker, automation-ghost trail,
// undo/redo (top-left corner, see Undo()/Redo()), and the preset-circle
// tray (left edge, hover to reveal, see DrawPresetTray) -- is built as
// of this round. STILL NOT BUILT: a dedicated trash UI distinct from
// Undo (deleting is already one double-click/Undo press away); the
// first-timer tutorial tour (needs your own steps/content, explicitly
// OK'd to wait).
class CircleViewControl : public iplug::igraphics::IControl
{
public:
  using IControl = iplug::igraphics::IControl;
  using IRECT = iplug::igraphics::IRECT;
  using IGraphics = iplug::igraphics::IGraphics;
  using IMouseMod = iplug::igraphics::IMouseMod;
  using IColor = iplug::igraphics::IColor;

  // `onBandSelected` is the SAME callback BandGraphControl used to drive
  // the existing per-band detail panel's retargeting -- kept identical so
  // this is a drop-in replacement in ExoEQ.cpp's OnLayout, not a parallel
  // UI that needs its own detail panel wired up separately.
  CircleViewControl(const IRECT& bounds, double sampleRate, std::function<void(int)> onBandSelected)
  : IControl(bounds, iplug::kNoParameter)
  , mSampleRate(sampleRate)
  , mOnBandSelected(std::move(onBandSelected))
  {
    mIgnoreMouse = false;
  }

  void SetSampleRate(double sr) { mSampleRate = sr; }

  // NEW THIS ROUND -- lets ExoEQ.cpp's layout lambda build real Solo/Mute
  // buttons for "whichever circle is currently selected" without this
  // control needing to own/draw those buttons itself (mSelectedCircle
  // stays private state otherwise; this is the one piece of it the
  // outside world legitimately needs to read).
  int GetSelectedCircle() const { return mSelectedCircle; }

  // NEW THIS ROUND -- the "explicit way to deselect/hide the detail
  // panel again" follow-up flagged last round. Doesn't call
  // mOnBandSelected (that callback means "show the panel for THIS band",
  // the opposite of what a deselect should do) -- ExoEQ.cpp's new close
  // button below calls this AND hides the panel controls itself in the
  // same action function, since this control has no way to reach into
  // the panel's own Hide() calls from here.
  void ClearSelection() { mSelectedBand = -1; SetDirty(false); }
  // Persisted across a close/reopen of the editor: the plugin reads these in OnUIClose
  // and writes them back into the new control in OnLayout.
  int GetSelectedBand() const { return mSelectedBand; }
  void RestoreSelection(int circle, int band)
  {
    mSelectedCircle = circle < 0 ? 0 : circle;
    mSelectedBand = -1;
    if (band >= 0) SelectBand(band); // same path a click uses, so the detail panel retargets too
    SetDirty(false);
  }

  // FOUND THIS ROUND -- the actual cause of "nodes still mismatch where we
  // click": Draw() sized the rail at 152px (widened a few rounds back for
  // the larger text), but EVERY mouse-handling function below had been
  // left at the OLD width, 88px, hardcoded separately at each call site.
  // mainArea (and therefore GetArcGeom's cx/cy/r, and therefore every
  // single Hz<->screen conversion) was 64px wider during a click than
  // during the paint that drew the dot you thought you were clicking --
  // a fixed, compounding offset, not the animation-lag issue fixed last
  // round (that was real too, just not the dominant one). One shared
  // constant now, so Draw() and every OnMouse*/FindBandNear/
  // FindResonanceHandleNear call agree by construction -- this can't
  // drift apart again the way two separately-typed literals did.
  static constexpr float kRailW = 152.f;

  // T4-1 load sweep. IGraphics calls IsDirty() once per display refresh, before it collects
  // dirty rects, so advancing here makes the sweep run every frame until it finishes.
  bool IsDirty() override
  {
    // T4-8: Low Motion skips the sweep entirely -- jump straight to the finished curve (one repaint).
    if (mLoadProgress < 1.f && GetDelegate()->GetParam(kGlobalLowMotion)->Value() > 0.5)
    {
      mLoadProgress = 1.f;
      return true;
    }
    if (mLoadProgress < 1.f)
    {
      mLoadProgress = std::min(1.f, mLoadProgress + 0.14f); // doubled step: same ~0.25 s sweep at 30 fps
      return true;
    }
    return IControl::IsDirty();
  }

  void Draw(IGraphics& g) override
  {
    using namespace iplug::igraphics;
    g.FillRect(IColor(255, 10, 10, 12), mRECT); // conceptual-art pass: near-black

    // Split: rail docked on the right (per the rail-right round confirmed
    // earlier), everything else (depth stack + selected-circle gauge) in
    // the remaining area to its left.
    const IRECT railArea = mRECT.GetFromRight(kRailW);
    // Built directly rather than via a "reduced by" helper (not confident
    // that exact helper name exists in the vendored IRECT -- GetFromRight/
    // GetFromLeft/GetPadded etc. above are the ones already proven to
    // compile elsewhere in this project's controls, so this one spot
    // avoids guessing at an unproven name).
    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);

    const std::vector<CircleInfo> circles = GatherCircles();
    // FIXED THIS ROUND: this used to `return` here whenever `circles` was
    // empty (a fresh project, zero active bands -- every band defaults to
    // inactive, see ExoEQ.h's own note on kBandActive), which drew
    // NOTHING -- not even the gauge's axis labels or any visible click
    // target. OnMouseDown below had the exact same bail, so there was no
    // way to ever click-create the first band: a real deadlock on an
    // empty/fresh plugin instance (confirmed from the user's own report
    // -- "I don't see anything when clicking on the black screen").
    // Every helper below already tolerates an empty `circles` vector fine
    // (plain for-loops over size() == 0, bounds-checked array access) --
    // EnsureSelectionValid's only real job is clamping mSelectedCircle
    // back to a circle that still exists when the previously-selected one
    // was just emptied out from under it, which still needs calling, just
    // not gating the whole draw on its return value.
    EnsureSelectionValid(circles);

    // T4-5: the grid, frequency labels and dB rings never change for a given size, so they are
    // rendered once into an IGraphics layer and blitted each frame. CheckLayer() reports false
    // after a resize or a DPI/scale change, which triggers a re-render.
    if (!g.CheckLayer(mGridLayer))
    {
      g.StartLayer(this, mainArea, false);
      DrawFrequencyGrid(g, mainArea);
      DrawDbRings(g, mainArea);
      mGridLayer = g.EndLayer();
    }
    g.DrawLayer(mGridLayer);
    DrawDepthStack(g, mainArea, circles);
    DrawScrim(g, mainArea, circles);
    DrawResponseCurve(g, mainArea, mSelectedCircle);
    // Ghosts fade out from 7 circles on: full at 7, a third less per extra circle, gone by 10.
    mGhostFade = std::clamp((10.f - static_cast<float>(circles.size())) / 3.f, 0.f, 1.f);
    // T3-1: ghost curves for up to 4 other circles (faint, dashed). Cap keeps weak machines responsive.
    {
      int shown = 0;
      for (const auto& c : circles)
      {
        if (c.circleId == mSelectedCircle) continue;
        if (shown++ >= 4) break;
        DrawResponseCurve(g, mainArea, c.circleId, true);
      }
    }
    DrawSelectedGauge(g, mainArea, circles);
    DrawRail(g, railArea, circles);
    // T2-8: global bypass dims the graph so the state is visible from anywhere in the view.
    if (GetDelegate()->GetParam(kGlobalBypass)->Value() > 0.5)
    {
      g.FillRect(IColor(150, 10, 10, 12), mRECT);
      // T2-8: the dry response is the flat 0 dB curve. Draw it dashed on top of the dim so the
      // bypassed state reads as "this is what the signal is doing now", not just a faded graph.
      const ArcGeom dryArc = GetArcGeom(mainArea);
      const float dryR = RadiusForDb(dryArc.r, 0.0);
      constexpr int kDashSteps = 48;
      for (int i = 0; i < kDashSteps; i += 2)
      {
        const double t0 = static_cast<double>(i) / kDashSteps;
        const double t1 = static_cast<double>(i + 1) / kDashSteps;
        const double a0 = (180.0 + t0 * 180.0) * kPi / 180.0;
        const double a1 = (180.0 + t1 * 180.0) * kPi / 180.0;
        g.DrawLine(IColor(200, 200, 215, 240),
                   dryArc.cx + dryR * static_cast<float>(std::cos(a0)), dryArc.cy + dryR * static_cast<float>(std::sin(a0)),
                   dryArc.cx + dryR * static_cast<float>(std::cos(a1)), dryArc.cy + dryR * static_cast<float>(std::sin(a1)),
                   nullptr, 1.2f);
      }
    }
    // NEW THIS ROUND: live crosshair + Hz readout -- another item off the
    // original deferred list. Drawn last so it sits on top of everything
    // else.
    DrawArcHoverTick(g, mainArea);
    DrawCrosshair(g, mainArea);
    // NEW THIS ROUND: the tap-creates-a-band filter-type popover --
    // drawn last/on top of everything, same as the crosshair above.
    DrawTypePicker(g);
    // NEW THIS ROUND: undo/redo (top-left corner) and the preset-circle
    // tray (left edge, hover to reveal) -- see DrawUndoRedo/
    // DrawPresetTray's own comments below for what each one does.
    DrawUndoRedo(g);
    DrawPresetTray(g);
    DrawStatusLamps(g, mainArea);
    // Concept 02 graph label, top-left of the radar.
    g.DrawText(ScaledText(11.f, IColor(255, 130, 165, 200)), "SPECTRAL DEPTH", IRECT(mainArea.L + 16.f, mainArea.T + 44.f, mainArea.L + 260.f, mainArea.T + 60.f));
    g.DrawText(ScaledText(10.f, IColor(255, 95, 110, 130)), "circle stack  |  signal flow  |  morph", IRECT(mainArea.L + 16.f, mainArea.T + 62.f, mainArea.L + 320.f, mainArea.T + 76.f));
  }

  // Tracks the cursor while it's over the gauge (hover OR an active
  // drag) and draws a vertical guide + the exact Hz value at that X --
  // "the crosshair + live Hz/cents readout" from the original deferred
  // list (cents omitted -- nothing in this plugin's param set currently
  // exposes a note/cents reference to measure against; Hz alone is what
  // every other control in this file already works in). NOTE on
  // OnMouseOver/OnMouseOut: standard iPlug2 IControl virtuals for hover
  // tracking, same can't-verify-against-your-exact-headers caveat as
  // every other UI/SDK-signature note in this project -- paste the error
  // if either doesn't compile.
  void OnMouseOver(float x, float y, const IMouseMod& /*mod*/) override
  {
    mHoverValid = true;
    mHoverX = x;
    mHoverY = y; // NEW THIS ROUND -- needed so the crosshair can project onto the arc, not just track X
    // NEW THIS ROUND -- hover-reveal for the preset-circle tray: showing
    // whenever the cursor is within kTrayHoverZonePx of the left edge.
    mTrayHover = (x - mRECT.L) <= kTrayHoverZonePx || mDraggingPreset;
    SetDirty(false);
  }

  void OnMouseOut() override
  {
    mHoverValid = false;
    // Keep the tray showing while a preset drag is actually in flight --
    // mouse capture during a drag can still fire OnMouseOut on some
    // frameworks right as the cursor crosses back over the control's own
    // edge, which would otherwise yank the tray away mid-drag.
    mTrayHover = mDraggingPreset;
    SetDirty(false);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mDraggingGain = false;
    // Click the selected node's tag to type "freq [gain]" for that node. Checked
    // first so it takes precedence over the node/drag hit-tests below.
    if (mSelectedBand >= 0 && mSelectedTagShown && mSelectedTagRect.Contains(x, y))
    {
      // Left half of the tag edits frequency, right half edits gain. Each has its own
      // text entry, so typing one value never changes the other.
      const bool gainSide = x >= mSelectedTagRect.MW();
      const double value = gainSide
        ? GetDelegate()->GetParam(BandParamIdx(mSelectedBand, kBandGainDb))->Value()
        : GetDelegate()->GetParam(BandParamIdx(mSelectedBand, kBandCutoff))->Value();
      char cur[32];
      std::snprintf(cur, sizeof(cur), "%.2f", value);
      mNodeEntryBand = mSelectedBand;
      mNodeEntryField = gainSide ? 1 : 0;
      GetUI()->CreateTextEntry(*this, DEFAULT_TEXT, mSelectedTagRect, cur, kNoValIdx);
      return;
    }
    // NEW THIS ROUND: if the type-picker card is open (see OnMouseUp's
    // new mPendingPick* handling), THIS click resolves it -- either picks
    // a row (commits the pending band with that filter type) or lands
    // outside the card (cancels -- the pending band slot is simply never
    // activated, nothing to undo). Either way this click is fully
    // consumed here and never falls through to the normal hit-testing
    // below, so you can't simultaneously dismiss the card AND start some
    // other gesture with the same click.
    if (mPendingPickBand >= 0)
    {
      const IRECT cardRect = GetPickerCardRect();
      if (cardRect.Contains(x, y))
      {
        const int row = static_cast<int>((y - cardRect.T) / kPickerRowH);
        int pickerCount = 0;
        const PickerItem* pickerItems = PickerItems(pickerCount);
        if (row >= 0 && row < pickerCount)
        {
          const IRECT mainAreaForPick(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
          // FIXED THIS ROUND: uses the tap's full (x,y) via the arc
          // mapping, not just X against a flat line.
          const double hz = HzFromArcPoint(mainAreaForPick, mPendingPickX, mPendingPickY);
          const PickerItem& item = pickerItems[row];
          PushUndoSnapshot(); // NEW THIS ROUND -- before any mutation, see Undo()/Redo() below
          PushParamValue(BandParamIdx(mPendingPickBand, kBandCutoff), std::clamp(hz, 20.0, 20000.0), 20.0, 20000.0);
          PushParamValue(BandParamIdx(mPendingPickBand, kBandFilterType), static_cast<double>(item.ordinal), 0.0, static_cast<double>(kNumFilterTypeOptions - 1));
          if (item.forceMorph50)
            PushParamValue(BandParamIdx(mPendingPickBand, kBandMorph), 50.0, 0.0, 100.0);
          PushParamValue(BandParamIdx(mPendingPickBand, kBandCircleIndex), static_cast<double>(mSelectedCircle), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
          PushParamValue(BandParamIdx(mPendingPickBand, kBandActive), 1.0, 0.0, 1.0);
          SelectBand(mPendingPickBand);
        }
      }
      // Outside the card, or inside it but not on a row (shouldn't
      // happen given cardRect's own height matches kNumPickerItems *
      // kPickerRowH exactly, kept as a safety net) -- either way, cancel.
      mPendingPickBand = -1;
      SetDirty(false);
      return;
    }

    // NEW THIS ROUND: undo/redo buttons, top-left corner -- checked before
    // everything else below (they're meant to always be clickable
    // regardless of what else is going on, same priority as the picker
    // card's own early-return above).
    if (GetUndoButtonRect().Contains(x, y)) { Undo(); return; }
    if (GetRedoButtonRect().Contains(x, y)) { Redo(); return; }

    // NEW THIS ROUND: starting a drag out of the preset-circle tray --
    // only reachable while the tray is actually showing (mTrayHover),
    // same hover-to-reveal panel DrawPresetTray draws. A hit inside the
    // panel on a real row starts the drag (resolved on release, see
    // OnMouseUp's mDraggingPreset branch); a hit inside the panel but not
    // on any row, or outside it entirely while the tray happens to be
    // showing, falls through to the normal hit-testing below exactly as
    // if the tray weren't there.
    if (mTrayHover)
    {
      const IRECT trayRect = GetPresetTrayRect();
      if (trayRect.Contains(x, y))
      {
        const int presetCount = TotalPresetCount();
        const int row = static_cast<int>((y - trayRect.T) / kPresetRowH);
        if (row >= 0 && row < presetCount)
        {
          mDraggingPreset = true;
          mDraggedPresetIndex = row;
          mPresetDragX = x;
          mPresetDragY = y;
          return;
        }
        // NEW THIS ROUND -- the one extra, non-draggable row right
        // after every real preset: "+ Save Current". A plain click
        // (not a drag start, since there's nothing at this row to drag
        // OUT of yet), consumed here so it doesn't fall through to
        // anything else.
        if (row == presetCount)
        {
          SaveCurrentCircleAsPreset();
          return;
        }
      }
    }

    const std::vector<CircleInfo> circles = GatherCircles();
    // Same fix as Draw() above -- this used to bail out here whenever
    // `circles` was empty, which meant a fresh/empty project could NEVER
    // create its first band: clicking anywhere did nothing, forever.
    // mSelectedCircle defaults to 0 and stays valid for an empty project
    // (there's nothing to clamp it against yet), so the rest of this
    // function -- rail hit-test, existing-band hit-test (naturally finds
    // nothing), then falling through to start a new-band drag -- all work
    // correctly with circles.empty() == true; nothing past this point
    // actually required a pre-existing circle.
    EnsureSelectionValid(circles);

    const IRECT railArea = mRECT.GetFromRight(kRailW);
    if (railArea.Contains(x, y))
    {
      // NEW THIS ROUND: rail press no longer selects immediately -- it
      // starts a drag-to-scroll gesture instead, resolved on release
      // (see OnMouseUp's new mDraggingRail branch): a small movement
      // still counts as a tap-to-select, same as before, but a real
      // drag now scrolls the rail live, and a fast flick-release keeps
      // scrolling briefly with a decelerating animation -- the "rail
      // inertia-drag/momentum" deferred item.
      mDraggingRail = true;
      mRailDragStartY = y;
      mRailDragTotalMovePx = 0.f;
      mRailDragLastDeltaY = 0.f;
      mDraggingNewBand = false;
      mDraggingResonanceHandle = false;
      return;
    }

    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
    const IRECT gaugeArea = GaugeArea(mainArea);
    if (!gaugeArea.Contains(x, y)) return;

    // Existing-band hit test first -- clicking an existing band selects
    // it (for the detail panel), same split BandGraphControl already
    // used, AND now starts a RETUNE drag (move its own cutoff by
    // dragging), fixed this round -- previously this just selected the
    // band and returned, with no way to move or delete a band already on
    // the graph at all once created (only the detail panel's own knobs
    // could touch it). Empty space is still what creates a brand new band.
    // NEW -- right-drag on a node = vertical gain (no extra handle). Up boosts,
    // down cuts, Shift = fine. Frequency and resonance stay on left-drag.
    if (mod.R)
    {
      const int gainBand = FindBandNear(mainArea, x, y);
      if (gainBand >= 0)
      {
        SelectBand(gainBand);
        mDraggingGain = true;
        mDraggedBand = gainBand;
        mGainDragStartY = y;
        mGainDragStartDb = GetDelegate()->GetParam(BandParamIdx(gainBand, kBandGainDb))->Value();
        PushUndoSnapshot();
        GetDelegate()->BeginInformHostOfParamChangeFromUI(BandParamIdx(gainBand, kBandGainDb));
        SetDirty(false);
        return;
      }
    }

    const int hitBand = FindBandNear(mainArea, x, y);
    if (hitBand >= 0)
    {
      // NEW THIS ROUND: shift-click a band to toggle its Mute instantly,
      // without opening the detail panel or starting a retune drag --
      // `mod.S` is iPlug2's standard IMouseMod shift-key flag (same
      // can't-verify-against-your-exact-headers caveat as every other
      // UI/SDK-signature note in this project; paste the error if this
      // specific field name doesn't compile).
      if (mod.A)
      {
        // Alt-click: move this band to the next circle (wraps around) and follow it,
        // so the node stays in view. Same membership param the rest of the view uses.
        PushUndoSnapshot();
        const int cur = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(hitBand, kBandCircleIndex))->Value());
        const int next = (cur + 1) % exoeq::kMaxCircles;
        PushParamValue(BandParamIdx(hitBand, kBandCircleIndex), static_cast<double>(next), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
        mSelectedCircle = next;
        SelectBand(hitBand);
        mDraggingNewBand = false;
        mDraggingExistingBand = false;
        mDraggingResonanceHandle = false;
        SetDirty(false);
        return;
      }
      if (mod.S)
      {
        PushUndoSnapshot();
        const bool currentlyMuted = GetDelegate()->GetParam(BandParamIdx(hitBand, kBandMute))->Value() > 0.5;
        PushParamValue(BandParamIdx(hitBand, kBandMute), currentlyMuted ? 0.0 : 1.0, 0.0, 1.0);
        SelectBand(hitBand);
        mDraggingNewBand = false;
        mDraggingExistingBand = false;
        mDraggingResonanceHandle = false;
        SetDirty(false);
        return;
      }
      SelectBand(hitBand);
      mDraggingNewBand = false;
      mDraggingExistingBand = true;
      mDraggingResonanceHandle = false;
      mDraggedBand = hitBand;
      // Snapshot once, at the START of the drag, not on every move --
      // one Undo() press should undo the WHOLE retune back to before you
      // started dragging, not one pixel of it at a time.
      PushUndoSnapshot();
      // NEW THIS ROUND: bracket the upcoming drag with Begin/
      // EndInformHostOfParamChangeFromUI (End is in OnMouseUp below) --
      // this was missing entirely before. Without it, every single
      // mouse-move during a retune drag sent its own independent
      // SendParameterValueFromUI call with no "this is one continuous
      // gesture" framing around them, which is what most hosts need to
      // (a) record ONE automation gesture instead of a flood of
      // disconnected points, and (b) group it as ONE undo step rather
      // than potentially one per mouse-move. Every other continuous-drag
      // control in iPlug2 (IVKnobControl etc.) does this same bracketing
      // internally; this control does it explicitly since it talks to
      // params directly rather than through a standard knob/slider base
      // class. NOTE on the exact call names: standard iPlug2
      // IEditorDelegate methods, same can't-verify-against-your-exact-
      // headers caveat as everywhere else in this project.
      GetDelegate()->BeginInformHostOfParamChangeFromUI(BandParamIdx(hitBand, kBandCutoff));
      SetDirty(false);
      return;
    }

    // NEW -- the resonance/width handle (the "second point" on a wide-
    // type band's dotted center line) checked only once the band's own
    // dot misses, same priority split hit-testing everywhere else in
    // this file already uses for a smaller secondary target.
    const int hitHandle = FindResonanceHandleNear(mainArea, x, y);
    if (hitHandle >= 0)
    {
      SelectBand(hitHandle);
      mDraggingNewBand = false;
      mDraggingExistingBand = false;
      mDraggingResonanceHandle = true;
      mDraggedBand = hitHandle;
      PushUndoSnapshot();
      GetDelegate()->BeginInformHostOfParamChangeFromUI(BandParamIdx(hitHandle, kBandResonance));
      SetDirty(false);
      return;
    }

    // Circle outlines are selectable targets too. Previously only the tiny
    // rail row changed mSelectedCircle, so clicking a visible depth arc
    // appeared to do nothing. Band dots remain higher priority because the
    // band hit-test above runs first.
    if (!circles.empty())
    {
      const ArcGeom arc = GetArcGeom(mainArea);
      int selectedPos = 0;
      for (size_t i = 0; i < circles.size(); ++i)
        if (circles[i].circleId == mSelectedCircle) selectedPos = static_cast<int>(i);
      const float dx = x - arc.cx;
      const float dy = std::min(0.f, y - arc.cy);
      const float distance = std::sqrt(dx * dx + dy * dy);
      double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx)) * 180.0 / kPi;
      if (angle <= 0.0) angle += 360.0;
      int nearestCircle = -1;
      float nearestDistance = 16.f;
      if (angle >= 180.0 && angle <= 360.0)
      {
        for (size_t i = 0; i < circles.size(); ++i)
        {
          const int rank = std::abs(static_cast<int>(i) - selectedPos);
          const float radius = arc.r * std::max(0.55f, 1.0f - rank * 0.08f);
          const float ringDistance = std::abs(distance - radius);
          if (ringDistance < nearestDistance)
          {
            nearestDistance = ringDistance;
            nearestCircle = circles[i].circleId;
          }
        }
      }
      if (nearestCircle >= 0 && nearestCircle != mSelectedCircle)
      {
        mSelectedCircle = nearestCircle;
        mRailScrollOffset = 0;
        mSelectedBand = -1;
        if (mOnBandSelected) mOnBandSelected(-1);
        SetDirty(false);
        return;
      }
    }

    mDragStartX = x;
    mDragStartY = y;
    mDraggingNewBand = true;
    mDraggingExistingBand = false;
    mDraggingResonanceHandle = false;
    mDragIsRange = false; // becomes true once the drag moves far enough (see OnMouseDrag)
  }

  void OnMouseDrag(float x, float y, float /*dX*/, float dY, const IMouseMod& mod) override
  {
    // Keep the crosshair tracking during an active drag even if
    // OnMouseOver doesn't fire mid-drag (mouse capture suppresses plain
    // hover events in a lot of UI frameworks while a button is held) --
    // without this the readout would freeze at wherever the drag started.
    mHoverValid = true;
    mHoverX = x;
    mHoverY = y;
    if (mDraggingPreset)
    {
      // NEW THIS ROUND -- just tracks the cursor; the actual drop
      // decision (replace/insert/add-new) happens once on release, see
      // OnMouseUp below. DrawPresetTray's own ghost-dot rendering reads
      // mPresetDragX/Y directly.
      mPresetDragX = x;
      mPresetDragY = y;
      SetDirty(false);
      return;
    }
    if (mDraggingRail)
    {
      // NEW THIS ROUND: live drag-to-scroll on the rail. Accumulates
      // raw pixel movement and converts whole rows at a time (one rail
      // row = railArea.H() / kRailVisibleRows px, same constant
      // DrawRail/RailRowAt already use) -- `dY` is iPlug2's own per-
      // event mouse delta (same parameter every other IControl drag
      // handler in this codebase already receives, just unused until
      // now), which doubles as this gesture's velocity signal for the
      // flick-to-keep-scrolling check in OnMouseUp below.
      const IRECT railArea = mRECT.GetFromRight(kRailW);
      // Literal 8, not a shared named constant -- matches the existing
      // local `kMaxVisibleRows = 8` already duplicated independently in
      // DrawRail/RailRowAt/ClampRailScrollOffset (pre-dating this round);
      // kept that same pattern here rather than risking a rename/shared-
      // constant refactor across all 4 spots in one go.
      const float rowH = railArea.H() / 8.f;
      mRailDragTotalMovePx += dY;
      mRailDragLastDeltaY = dY;
      while (mRailDragTotalMovePx >= rowH) { mRailScrollOffset -= 1; mRailDragTotalMovePx -= rowH; }
      while (mRailDragTotalMovePx <= -rowH) { mRailScrollOffset += 1; mRailDragTotalMovePx += rowH; }
      SetDirty(false);
      return;
    }
    if (mDraggingGain)
    {
      // 0.12 dB per px vertical; Shift drops to 0.02 dB/px for fine trim.
      constexpr double kGainDbPerPx = 0.12, kGainDbPerPxFine = 0.02;
      const double perPx = mod.S ? kGainDbPerPxFine : kGainDbPerPx;
      const double targetDb = mGainDragStartDb + (mGainDragStartY - y) * perPx;
      PushParamValue(BandParamIdx(mDraggedBand, kBandGainDb), std::clamp(targetDb, -24.0, 24.0), -24.0, 24.0);
      SetDirty(false);
      return;
    }
    if (mDraggingResonanceHandle)
    {
      // Radial drag: the band's own angle (its cutoff/frequency) never
      // changes here, only the distance from the arc's center -- same
      // geometry ResonanceHandleRadius/FindResonanceHandleNear already
      // use, just solved in reverse (distance -> resonance).
      const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
      const ArcGeom arc = GetArcGeom(mainArea);
      const std::vector<CircleInfo> circles = GatherCircles();
      int selectedPos = 0;
      for (size_t i = 0; i < circles.size(); ++i) if (circles[i].circleId == mSelectedCircle) selectedPos = static_cast<int>(i);
      const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(mDraggedBand, kBandCircleIndex))->Value());
      int circlePos = selectedPos;
      for (size_t i = 0; i < circles.size(); ++i) if (circles[i].circleId == circleId) circlePos = static_cast<int>(i);
      const float ringRadius = arc.r * std::max(0.55f, 1.0f - std::abs(circlePos - selectedPos) * 0.08f);
      const double dx = static_cast<double>(x - arc.cx);
      const double dy = std::min(0.0, static_cast<double>(y - arc.cy));
      const float dist = static_cast<float>(std::sqrt(dx * dx + dy * dy));
      const float denom = std::max(1.f, ringRadius * (kResHandleOuterFrac - kResHandleInnerFrac));
      const float t = std::clamp((dist - ringRadius * kResHandleInnerFrac) / denom, 0.f, 1.f);
      const double resonance = UnitToResonance(t);
      PushParamValue(BandParamIdx(mDraggedBand, kBandResonance), resonance, 0.05, 20.0);
      SetDirty(false);
      return;
    }
    if (mDraggingExistingBand)
    {
      // Retune live, same gaugeArea math every other coordinate<->Hz
      // conversion in this file uses -- the band's dot tracks the cursor
      // in real time rather than only on release, since this IS the
      // direct-manipulation gesture for an already-existing band (unlike
      // new-band creation, which deliberately waits for release to decide
      // tap-vs-range).
      // FIXED THIS ROUND: retune-drag now maps the cursor to Hz via its
      // ANGLE around the arc's own center (HzFromArcPoint), not via X
      // against a flat line -- this keeps the dragged dot riding the
      // actual curve.
      const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
      const double targetHz = HzFromArcPoint(mainArea, x, y);
      // NEW THIS ROUND -- fine-tune retune: hold Shift while dragging an
      // existing band and it follows the cursor's TARGET position slowly
      // (damped) instead of jumping straight to it, for precise
      // placement near a specific Hz. Damping toward the target rather
      // than scaling mouse delta, since this control maps drag position
      // to Hz by ABSOLUTE angle around the arc (HzFromArcPoint), not by
      // accumulated delta the way a knob's fine-drag usually works --
      // damping is the natural fit for an absolute-position mapping.
      // `mod.S` is the same IMouseMod shift flag already proven
      // elsewhere in this file (shift-click-mute).
      double hz = targetHz;
      if (mod.S)
      {
        const double currentHz = GetDelegate()->GetParam(BandParamIdx(mDraggedBand, kBandCutoff))->Value();
        constexpr double kFineDamping = 0.12; // smaller = finer/slower follow
        hz = currentHz + (targetHz - currentHz) * kFineDamping;
      }
      PushParamValue(BandParamIdx(mDraggedBand, kBandCutoff), std::clamp(hz, 20.0, 20000.0), 20.0, 20000.0);
      SetDirty(false);
      return;
    }
    if (!mDraggingNewBand) return;
    constexpr float kRangeThresholdPx = 6.f;
    if (std::abs(x - mDragStartX) > kRangeThresholdPx)
      mDragIsRange = true;
    // Visual-only feedback while dragging could go here (a preview arc);
    // deferred this round -- the band itself is only created on release,
    // see OnMouseUp, matching the mockups' "tap vs. drag-release" split.
    (void)y;
  }

  void OnMouseUp(float x, float y, const IMouseMod& /*mod*/) override
  {
    if (mDraggingPreset)
    {
      // NEW THIS ROUND -- resolving a preset-circle drag-drop, per your
      // spec: drop at DEAD CENTER of the selected circle's own arc ->
      // REPLACE it (every existing band in it deactivated first); drop
      // on the rail, between two rows -> INSERT at that specific order
      // (every circle at/after that point shifts up one slot to make
      // room); drop anywhere else -> ADD as a brand new circle (first
      // free circle ID) and select it.
      mDraggingPreset = false;
      const int presetIndex = mDraggedPresetIndex;
      mDraggedPresetIndex = -1;
      const int presetCount = TotalPresetCount();
      if (presetIndex < 0 || presetIndex >= presetCount) { SetDirty(false); return; }
      const PresetCircleDef preset = GetPresetAt(presetIndex);

      const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
      const IRECT railArea = mRECT.GetFromRight(kRailW);
      const ArcGeom arc = GetArcGeom(mainArea);
      const std::vector<CircleInfo> circles = GatherCircles();

      PushUndoSnapshot();

      constexpr float kDeadCenterRadiusPx = 42.f; // generous -- this is a "drop near the middle" gesture, not a precise hit-test
      const float distFromCenter = std::sqrt((x - arc.cx) * (x - arc.cx) + (y - arc.cy) * (y - arc.cy));
      if (distFromCenter <= kDeadCenterRadiusPx && !circles.empty())
      {
        // REPLACE the selected circle.
        ClearCircleBands(mSelectedCircle);
        PlacePresetBandsInCircle(preset, mSelectedCircle);
      }
      else if (railArea.Contains(x, y))
      {
        // INSERT at a specific order between two rail rows.
        const int insertAt = RailInsertionPoint(railArea, circles, y);
        ShiftCircleIndicesUpFrom(insertAt);
        PlacePresetBandsInCircle(preset, insertAt);
      }
      else
      {
        // ADD as a new circle, anywhere else.
        const int freeCircle = FirstFreeCircleId(circles);
        if (freeCircle >= 0) PlacePresetBandsInCircle(preset, freeCircle);
      }
      SetDirty(false);
      return;
    }
    if (mDraggingRail)
    {
      mDraggingRail = false;
      constexpr float kTapMovePxThreshold = 6.f; // barely-moved press+release still counts as a tap-to-select
      if (std::abs(mRailDragTotalMovePx) < kTapMovePxThreshold && std::abs(y - mRailDragStartY) < kTapMovePxThreshold)
      {
        const IRECT railArea = mRECT.GetFromRight(kRailW);
        const std::vector<CircleInfo> circles = GatherCircles();
        const int hitCircle = RailRowAt(railArea, circles, y);
        if (hitCircle >= 0 && hitCircle != mSelectedCircle)
        {
          mSelectedCircle = hitCircle;
          mRailScrollOffset = 0; // re-center the window on the newly-selected row
        }
        else if (hitCircle < 0 && mSelectedBand >= 0)
        {
          // NEW THIS ROUND -- a second, more discoverable way to close
          // the detail panel besides its own "X" button: tapping empty
          // rail space (below the last row, or an otherwise-empty rail)
          // deselects too. This spot was free to repurpose -- it was
          // always a no-op tap before (RailRowAt returning -1 did
          // nothing), unlike tapping empty GAUGE space, which is already
          // taken by "create a new band here" and couldn't double as
          // deselect. Reuses the exact same mOnBandSelected(-1) callback
          // ExoEQ.cpp's close button drives, so the panel-hiding logic
          // lives in exactly one place either way.
          mSelectedBand = -1;
          if (mOnBandSelected) mOnBandSelected(-1);
        }
        else if (hitCircle < 0)
        {
          // Empty rail space is an explicit create-circle gesture when no
          // band is selected. This gives the user a discoverable way to
          // start a new depth layer without guessing how to manufacture a
          // circle from the arc. The new circle starts with one neutral Bell
          // band at 1 kHz and is selected immediately.
          const int freeCircle = FirstFreeCircleId(circles);
          const int freeBand = FirstInactiveBandSlot();
          if (freeCircle >= 0 && freeBand >= 0)
          {
            PushUndoSnapshot();
            PushParamValue(BandParamIdx(freeBand, kBandCutoff), 1000.0, 20.0, 20000.0);
            PushParamValue(BandParamIdx(freeBand, kBandResonance), 0.707, 0.05, 20.0);
            PushParamValue(BandParamIdx(freeBand, kBandFilterType), 0.0, 0.0, static_cast<double>(kNumFilterTypeOptions - 1));
            PushParamValue(BandParamIdx(freeBand, kBandCircleIndex), static_cast<double>(freeCircle), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
            PushParamValue(BandParamIdx(freeBand, kBandActive), 1.0, 0.0, 1.0);
            mSelectedCircle = freeCircle;
            SelectBand(freeBand);
          }
        }
        SetDirty(false);
        return;
      }

      // NEW THIS ROUND: a real drag that ends while still moving fast
      // keeps scrolling briefly and decelerates -- the actual "momentum/
      // inertia" part of the deferred list item (last round's wheel-
      // scroll was real scrolling, but stopped the instant you stopped
      // turning the wheel). NOTE: SetAnimation/GetAnimationProgress are
      // standard iPlug2 IControl animation-timer calls, but this is the
      // one API in this whole project I have the least confidence is
      // exactly right without seeing your vendored IControl.h -- paste
      // the error if this specific block doesn't compile; everything
      // else in this round does not depend on it.
      constexpr float kFlickThresholdPx = 3.f; // px of movement on the LAST drag event before release
      if (std::abs(mRailDragLastDeltaY) > kFlickThresholdPx)
      {
        const int direction = (mRailDragLastDeltaY > 0.f) ? -1 : 1; // same sign convention as the live-scroll loop in OnMouseDrag above
        constexpr int kMomentumExtraRows = 4; // fixed glide distance -- simple, predictable deceleration rather than literal velocity-based physics
        const int startOffset = mRailScrollOffset;
        SetAnimation([this, direction, startOffset, kMomentumExtraRows](IControl* /*pCaller*/) {
          const double p = GetAnimationProgress(); // 0..1 over the animation's duration
          const double eased = 1.0 - std::pow(1.0 - p, 3.0); // ease-out cubic -- fast start, gentle stop
          mRailScrollOffset = startOffset + static_cast<int>(direction * kMomentumExtraRows * eased);
          SetDirty(false);
        }, 450);
      }
      return;
    }
    if (mDraggingGain)
    {
      GetDelegate()->EndInformHostOfParamChangeFromUI(BandParamIdx(mDraggedBand, kBandGainDb));
      mDraggingGain = false;
      (void)x; (void)y;
      return;
    }
    if (mDraggingResonanceHandle)
    {
      GetDelegate()->EndInformHostOfParamChangeFromUI(BandParamIdx(mDraggedBand, kBandResonance));
      mDraggingResonanceHandle = false;
      (void)x; (void)y;
      return;
    }
    if (mDraggingExistingBand)
    {
      // Closes the Begin/EndInformHostOfParamChangeFromUI bracket opened
      // in OnMouseDown above -- every intermediate value was already
      // pushed live by OnMouseDrag, this just tells the host the gesture
      // is over.
      GetDelegate()->EndInformHostOfParamChangeFromUI(BandParamIdx(mDraggedBand, kBandCutoff));
      mDraggingExistingBand = false;
      (void)x; (void)y;
      return;
    }
    if (!mDraggingNewBand) return;
    mDraggingNewBand = false;

    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);

    const int freeBand = FirstInactiveBandSlot();
    if (freeBand < 0) return; // every one of kMaxBands slots already in use

    // FIXED THIS ROUND: both the press-point and release-point Hz now
    // come from the arc mapping (HzFromArcPoint), using each point's full
    // (x,y), not just X against a flat line.
    const double startHz = HzFromArcPoint(mainArea, mDragStartX, mDragStartY);
    const double endHz = HzFromArcPoint(mainArea, x, y);
    // FIXED THIS ROUND (the "dot doesn't land where the mouse points" bug
    // you reported): for a range drag, this used to place the dot at the
    // GEOMETRIC MEAN of the start and end Hz -- some point BETWEEN where
    // you pressed and where you released, never exactly under either one.
    // For a tap that's invisible (start==end, no visible gap), but for
    // any real drag the dot would visibly appear away from your cursor
    // at release, which is exactly what you saw. A band's cutoff now
    // always tracks the RELEASE point directly (endHz) -- wherever the
    // mouse is when you let go is exactly where the dot appears, for
    // both a tap (start==end, no behavior change there) and a drag.
    // Bandwidth (resonance, below) still derives from how far apart
    // start/end were, same as before -- only the dot's own X position
    // changed.
    const double cutoffHz = endHz;

    if (mDragIsRange)
    {
      // NEW THIS ROUND: drag-release no longer force-creates Band-Pass 12
      // every time -- the drag's own DIRECTION (angle of the vector from
      // press to release) now picks the filter type, straight off the
      // SAME PickerItems() list the tap-popover shows -- the "connected
      // gesture-sorted filter-type popover card" piece that was still
      // missing (tap-opens-a-list was built; picking-by-swipe-direction
      // wasn't). This is a direct radial-pie-menu mapping, not a visible
      // card -- you don't see a menu while dragging (no spec existed for
      // what that overlay should look like), but the RESULT is sorted by
      // the same list, picked by the same gesture the deferred item
      // named. Full 360 degrees split into one slice per PickerItems()
      // entry (5 slices here, so 72 degrees each), slice 0 centered on
      // "straight right" (angle 0) and proceeding counter-clockwise in
      // screen terms -- an arbitrary but fixed and predictable
      // assignment; say if you want a specific direction pinned to a
      // specific type instead (e.g. "always Lowpass when dragging left")
      // and this mapping can be swapped for that.
      int pickerCount = 0;
      const PickerItem* pickerItems = PickerItems(pickerCount);
      double angleDeg = std::atan2(static_cast<double>(y - mDragStartY), static_cast<double>(x - mDragStartX)) * 180.0 / kPi;
      if (angleDeg < 0.0) angleDeg += 360.0;
      const double sliceWidth = 360.0 / pickerCount;
      const int slice = std::clamp(static_cast<int>(angleDeg / sliceWidth), 0, pickerCount - 1);
      const PickerItem& gestureItem = pickerItems[slice];

      PushUndoSnapshot();
      PushParamValue(BandParamIdx(freeBand, kBandCutoff), std::clamp(cutoffHz, 20.0, 20000.0), 20.0, 20000.0);
      const double octaveSpan = std::max(0.1, std::log2(std::max(startHz, endHz) / std::max(20.0, std::min(startHz, endHz))));
      const double resonance = std::clamp(1.4 / octaveSpan, 0.05, 20.0);
      PushParamValue(BandParamIdx(freeBand, kBandResonance), resonance, 0.05, 20.0);
      PushParamValue(BandParamIdx(freeBand, kBandFilterType), static_cast<double>(gestureItem.ordinal), 0.0, static_cast<double>(kNumFilterTypeOptions - 1));
      if (gestureItem.forceMorph50)
        PushParamValue(BandParamIdx(freeBand, kBandMorph), 50.0, 0.0, 100.0);
      PushParamValue(BandParamIdx(freeBand, kBandCircleIndex), static_cast<double>(mSelectedCircle), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
      PushParamValue(BandParamIdx(freeBand, kBandActive), 1.0, 0.0, 1.0);
      SelectBand(freeBand);
      SetDirty(false);
      return;
    }

    // A plain tap creates a neutral Bell immediately. This is the fast path
    // used by professional EQs: click, then drag the node. Filter choice is
    // still available through the directional gesture and the selected
    // band's Type control.
    // Drawn/hit-tested with this
    // file's own existing FillRect/DrawText/Contains primitives rather
    // than IGraphics' native popup-menu API (CreatePopupMenu/
    // IPopupMenu/OnPopupMenuSelection) -- deliberately, since this file
    // has an established pattern of avoiding SDK calls it can't verify
    // against your exact vendored headers, and a native popup menu would
    // be 3 more of those at once. Nothing is created yet -- the actual
    // PushParamValue calls happen in OnMouseDown below once a row (or
    // nothing) is picked.
    PushUndoSnapshot();
    PushParamValue(BandParamIdx(freeBand, kBandCutoff), std::clamp(cutoffHz, 20.0, 20000.0), 20.0, 20000.0);
    PushParamValue(BandParamIdx(freeBand, kBandResonance), 0.707, 0.05, 20.0);
    PushParamValue(BandParamIdx(freeBand, kBandFilterType), 0.0, 0.0, static_cast<double>(kNumFilterTypeOptions - 1));
    PushParamValue(BandParamIdx(freeBand, kBandMorph), 50.0, 0.0, 100.0);
    PushParamValue(BandParamIdx(freeBand, kBandCircleIndex), static_cast<double>(mSelectedCircle), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
    PushParamValue(BandParamIdx(freeBand, kBandActive), 1.0, 0.0, 1.0);
    SelectBand(freeBand);
    SetDirty(false);
  }

  // NEW THIS ROUND: deleting a band. Double-click an existing band on
  // the gauge to deactivate it (kBandActive -> 0, same "inert past the
  // limit" mechanism every other inactive band already uses -- see
  // ExoEQ.h's own note on kBandActive) -- there was previously NO way to
  // remove a band from the graph itself once created, only mute it via
  // the detail panel's own Mute toggle (which silences it but leaves it
  // occupying one of the kMaxBands slots forever). A plain double-click
  // on empty gauge space is a no-op (FindBandNear returns -1, nothing
  // happens), matching the existing single-click split between "hit an
  // existing band" vs "empty space".
  void OnMouseDblClick(float x, float y, const IMouseMod& /*mod*/) override
  {
    const IRECT railArea = mRECT.GetFromRight(kRailW);
    if (railArea.Contains(x, y))
    {
      // NEW THIS ROUND: double-click a rail row to delete that WHOLE
      // circle -- the "preset-circle tray/drag-to-delete-a-circle" item
      // from the original deferred list, built as a double-click rather
      // than an actual drag-to-a-trash-icon gesture (no tray UI exists
      // yet to drag TO) -- same mechanism as deleting a single band:
      // deactivates every band currently assigned to that circle
      // (kBandCircleIndex == hitCircle), which is enough to make the
      // circle itself disappear next frame (CircleChain::CircleBandCount
      // reads 0, same "a circle exists only while >=1 band points at it"
      // rule DSP/CircleChain.h's own header note already documents).
      const std::vector<CircleInfo> circles = GatherCircles();
      const int hitCircle = RailRowAt(railArea, circles, y);
      if (hitCircle < 0) return;
      PushUndoSnapshot();
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        if (static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != hitCircle) continue;
        PushParamValue(BandParamIdx(b, kBandActive), 0.0, 0.0, 1.0);
        if (mSelectedBand == b) mSelectedBand = -1;
      }
      SetDirty(false);
      return;
    }

    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
    const IRECT gaugeArea = GaugeArea(mainArea);
    if (!gaugeArea.Contains(x, y)) return;

    const int hitBand = FindBandNear(mainArea, x, y);
    if (hitBand < 0) return;

    PushUndoSnapshot();
    PushParamValue(BandParamIdx(hitBand, kBandActive), 0.0, 0.0, 1.0);
    mDraggingNewBand = false;
    mDraggingExistingBand = false;
    if (mSelectedBand == hitBand) mSelectedBand = -1;
    SetDirty(false);
  }

  // ROUND 28 FIX -- this used to say `private:` here, which quietly put
  // PresetCircleDef/PresetBand (and GetUserPresets/SetUserPresets, added
  // last round for save/reload persistence) out of reach of ExoEQ.cpp's
  // SerializeState/UnserializeState, breaking the build with a batch of
  // C2248 "cannot access private" errors. Everything below was already
  // written assuming it's reachable the same way the rest of this class
  // is (no prior `private:` existed before this one was added at some
  // point) -- removed rather than re-split, since nothing below actually
  // needs real encapsulation from ExoEQ.cpp, same as every other public
  // helper in this file.
  // Local constant instead of M_PI -- M_PI isn't standard/portable on
  // MSVC without defining _USE_MATH_DEFINES before every system header
  // that might declare it first; same fix already used in DSP/LFO.h.
  static constexpr double kPi = 3.14159265358979323846;

  // NOTE on these two ordinals: they must match ExoEQ.cpp's InitEnum
  // option list order for kBandFilterType exactly (same rule as every
  // other dropdown in this project) -- "Morph (LP-BP-HP)" is index 0,
  // "Bandpass 12" is the Bandpass-family entry after the 8 lowpass/
  // highpass slopes. A Bell isn't a distinct dropdown entry in this
  // plugin's filter list -- MorphZ (index 0) at its BP midpoint (morph=50)
  // reads as a bell-shaped boost/notch, so a tap-created "Bell" is really
  // MorphZ with Morph forced to 50 (set below via PushParamValue in
  // OnMouseUp... see the follow-up line this comment sits above). If
  // this doesn't match your actual current FilterTypes.h ordering,
  // paste the real option list and these two constants are the only
  // lines that need to change.
  static constexpr int kBellOrdinal = 0; // Morph (LP-BP-HP), forced to its BP midpoint
  static constexpr int kBandPass12Ordinal = 9; // "Bandpass 12" per ExoEQ.cpp's current InitEnum list
  static constexpr int kNumFilterTypeOptions = 45; // length of that same InitEnum list (counted from ExoEQ.cpp directly) -- update together if it changes

  // NEW THIS ROUND -- the tap-creates-a-popover-card item list (see
  // OnMouseUp/DrawTypePicker/OnMouseDown's new mPendingPick* handling).
  // Ordinals read straight off ExoEQ.cpp's real InitEnum option list
  // (same source as the two constants above, same "update together if
  // it changes" rule) -- a short, deliberately curated shortlist of the
  // 5 most fundamental shapes (not all 46 options; this is a quick-pick
  // card, not a replacement for the detail panel's own full Type
  // dropdown, which still has every option).
  //
  // Returned via a function-local static array (not a class-level
  // `static constexpr` member) deliberately -- a class-level constexpr
  // array taken by reference/index can need an out-of-line definition
  // pre-C++17 and risk an "undefined reference" link error depending on
  // this project's exact C++ standard setting; a function-local static
  // has no such ambiguity under any standard.
  struct PickerItem { const char* label; int ordinal; bool forceMorph50; };
  static const PickerItem* PickerItems(int& countOut)
  {
    static const PickerItem kItems[] = {
      { "Bell",        0,  true  }, // Morph (LP-BP-HP) at its BP midpoint, same as every prior round's tap default
      { "Lowpass 12",  2,  false },
      { "Highpass 12", 6,  false },
      { "Bandpass 12", 9,  false },
      { "Notch 12",    11, false },
    };
    countOut = static_cast<int>(sizeof(kItems) / sizeof(kItems[0]));
    return kItems;
  }
  // Large, high-contrast quick picker: this is the first decision a user
  // makes after creating a node, so it should read like a deliberate tool
  // palette rather than a tiny default menu.
  static constexpr float kPickerRowH = 32.f;
  static constexpr float kPickerW = 188.f;

  // NEW THIS ROUND -- the preset-circle library/tray, per your spec:
  // "a sort of library on the left which pops up when you hover over it,
  // and then you can drag and drop a circle to the bands". A small fixed
  // library of starter circles (1-3 bands each), same function-local-
  // static-array pattern as PickerItems() above for the same reason (no
  // class-level constexpr array of structs). Ordinals reuse the exact
  // same 5 values PickerItems() already uses (Bell=0/forceMorph50,
  // Lowpass12=2, Highpass12=6, Bandpass12=9, Notch12=11) rather than
  // guessing at any OTHER FilterTypes.h ordinal I haven't already
  // confirmed against your real InitEnum list -- these 4 templates are a
  // reasonable, deliberately simple starting set; say if you want
  // specific ones (exact types/Hz/Q) and these can be swapped for those
  // instead of staying a guess.
  struct PresetBand { int ordinal; double cutoffHz; double resonance; bool forceMorph50; };
  // NOTE: `name` changed this round from `const char*` to a fixed char
  // buffer -- a built-in preset's name can safely be a string-literal
  // pointer (lives for the program's whole life), but a USER-SAVED
  // preset (new this round, see SaveCurrentCircleAsPreset) needs its own
  // stable storage, since there's no string literal to point at. A
  // fixed buffer lets both kinds share one struct/one code path rather
  // than needing a separate type (and separate draw/drag logic) for
  // user presets.
  struct PresetCircleDef { char name[32]; int bandCount; PresetBand bands[3]; };
  static const PresetCircleDef* PresetCircles(int& countOut)
  {
    static const PresetCircleDef kPresets[] = {
      { "Clean Vocal Shelf", 1, { { 0, 3000.0, 0.7,  true  } } },
      { "Warm Low Boost",    1, { { 0, 120.0,  0.9,  true  } } },
      { "Air Lift Top",      1, { { 0, 14000.0, 0.6, true  } } },
      { "Notch Cleanup",     1, { { 11, 1000.0, 4.0, false } } },
    };
    countOut = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets;
  }
  static constexpr float kPresetRowH = 38.f; // widened this round from 28 to fit a second preview line (see DrawPresetTray) -- GetPresetTrayRect sizes off this same constant, so nothing else needed to change
  static constexpr int kMaxUserPresets = 8; // caps tray growth -- oldest user preset drops once a 9th is saved

  // NEW THIS ROUND -- the built-in 4 plus however many you've saved this
  // session, as one continuous 0-based index (built-ins first, your own
  // after). Everywhere that used to call PresetCircles() directly now
  // goes through these two instead, so the tray, the drag-ghost, and the
  // drop-resolution logic all see the same combined list without three
  // separate places needing to know how to stitch it together.
  int TotalPresetCount() const
  {
    int builtinCount = 0;
    PresetCircles(builtinCount);
    return builtinCount + static_cast<int>(mUserPresets.size());
  }

  PresetCircleDef GetPresetAt(int index) const
  {
    int builtinCount = 0;
    const PresetCircleDef* builtins = PresetCircles(builtinCount);
    if (index >= 0 && index < builtinCount) return builtins[index];
    const int userIdx = index - builtinCount;
    if (userIdx >= 0 && userIdx < static_cast<int>(mUserPresets.size())) return mUserPresets[userIdx];
    return PresetCircleDef{ "?", 0, {} }; // shouldn't happen -- every call site bounds-checks against TotalPresetCount() first; this is just a safe fallback instead of UB on a bad index
  }

  // NEW THIS ROUND -- "save current circle as a new preset" (the actual
  // "library" half of the spec: not just 4 fixed starters forever, but
  // a place to put your OWN circles too). Session-only -- these live in
  // memory, not in the plugin's saved state/project file (no param slot
  // was carved out for "arbitrary number of named circle templates", and
  // adding one is a bigger change than this round's scope), so they're
  // gone on reload. Captures up to 3 of the selected circle's active
  // bands (PresetCircleDef's own fixed-size cap, same limit every
  // built-in preset already lives under) -- a circle with more than 3
  // bands only has its first 3 captured, flagged here and in the README
  // rather than silently losing bands with no explanation.
  void SaveCurrentCircleAsPreset()
  {
    PresetCircleDef def{};
    std::snprintf(def.name, sizeof(def.name), "Custom %d", static_cast<int>(mUserPresets.size()) + 1);
    def.bandCount = 0;
    for (int b = 0; b < kMaxBands && def.bandCount < 3; ++b)
    {
      if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
      if (static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != mSelectedCircle) continue;
      PresetBand& pb = def.bands[def.bandCount];
      pb.cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
      pb.resonance = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance))->Value();
      pb.ordinal = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value());
      pb.forceMorph50 = false; // morph is captured as part of the filter type's own normal param set elsewhere, not re-forced here -- forceMorph50 only matters for the BUILT-IN presets' own Bell shorthand
      ++def.bandCount;
    }
    if (def.bandCount == 0) return; // nothing to save -- empty circle, or none selected
    mUserPresets.push_back(def);
    if (mUserPresets.size() > static_cast<size_t>(kMaxUserPresets)) mUserPresets.erase(mUserPresets.begin());
    SetDirty(false);
  }

  // ROUND 27 -- user presets were session-only (flagged as a "bigger
  // future task" back when SaveCurrentCircleAsPreset was added); this is
  // that task. ExoEQ::SerializeState/UnserializeState (ExoEQ.cpp) read
  // and write mUserPresets through these two so your saved circles
  // survive a project save/reload, same as every other param. Plain
  // getter/setter, no validation here -- UnserializeState is responsible
  // for not handing back garbage (see its own bounds-checked read loop).
  const std::vector<PresetCircleDef>& GetUserPresets() const { return mUserPresets; }
  void SetUserPresets(const std::vector<PresetCircleDef>& presets)
  {
    mUserPresets = presets;
    if (mUserPresets.size() > static_cast<size_t>(kMaxUserPresets)) mUserPresets.resize(kMaxUserPresets);
    SetDirty(false);
  }
  static constexpr float kTrayWidth = 160.f;
  static constexpr float kTrayHoverZonePx = 18.f; // widened this round to match the new PRESETS letter-stack's own width -- 14px cut it off mid-letter

  // The tray panel itself -- full height of the control, docked at its
  // left edge, same "clamped fully inside mRECT" idea GetPickerCardRect
  // already uses (trivial here since this one's always anchored at a
  // fixed edge, not a tap point).
  IRECT GetPresetTrayRect() const
  {
    // +1 row this round for the "+ Save Current" action row at the
    // bottom, after every real preset (built-in + your own).
    const int rowCount = TotalPresetCount() + 1;
    return IRECT(mRECT.L, mRECT.T + 30.f, mRECT.L + kTrayWidth, mRECT.T + 30.f + kPresetRowH * rowCount);
  }

  void DrawPresetTray(IGraphics& g)
  {
    using namespace iplug::igraphics;
    // The thin always-visible strip at the left edge, so there's
    // something to find/hover over in the first place -- a plain
    // vertical label, not a full preview of the panel's contents.
    const IRECT edgeTab(mRECT.L, mRECT.T + 30.f, mRECT.L + 4.f, mRECT.B - 30.f);
    g.FillRect(IColor(160, 217, 164, 76), edgeTab);

    // NEW THIS ROUND -- the 4px-wide edge tab alone is too subtle to
    // actually discover ("pops up when you hover over it" only works if
    // something tells you to hover there in the first place). Stacks
    // "PRESETS" one letter per line down the tab's own height instead of
    // one sideways/rotated string -- simpler and safer than guessing at
    // IGraphics' text-rotation API (not something this file has used or
    // proven anywhere else), and reuses DrawText exactly as already
    // proven everywhere else in this file.
    {
      static const char* kLetters[] = { "P", "R", "E", "S", "E", "T", "S" };
      constexpr int kLetterCount = 7;
      const float letterH = (edgeTab.H()) / static_cast<float>(kLetterCount + 1); // +1 so it doesn't run edge-to-edge
      for (int i = 0; i < kLetterCount; ++i)
      {
        const IRECT letterRect(mRECT.L, edgeTab.T + i * letterH, mRECT.L + 16.f, edgeTab.T + (i + 1) * letterH);
        g.DrawText(ScaledText(9.f, IColor(190, 217, 164, 76)), kLetters[i], letterRect);
      }
    }

    if (mTrayHover)
    {
      const IRECT trayRect = GetPresetTrayRect();
      g.FillRect(IColor(235, 28, 28, 34), trayRect);
      const IColor borderColor(255, 90, 90, 100);
      g.FillRect(borderColor, IRECT(trayRect.L, trayRect.T, trayRect.R, trayRect.T + 1.f));
      g.FillRect(borderColor, IRECT(trayRect.L, trayRect.B - 1.f, trayRect.R, trayRect.B));
      g.FillRect(borderColor, IRECT(trayRect.L, trayRect.T, trayRect.L + 1.f, trayRect.B));
      g.FillRect(borderColor, IRECT(trayRect.R - 1.f, trayRect.T, trayRect.R, trayRect.B));
      const int presetCount = TotalPresetCount();
      for (int i = 0; i < presetCount; ++i)
      {
        const PresetCircleDef preset = GetPresetAt(i);
        const IRECT rowRect(trayRect.L, trayRect.T + i * kPresetRowH, trayRect.R, trayRect.T + (i + 1) * kPresetRowH);
        // NEW -- cosmetic hover brighten, same signal as every other
        // clickable/draggable row in this file.
        if (mHoverValid && rowRect.Contains(mHoverX, mHoverY))
          g.FillRect(IColor(60, 217, 164, 76), rowRect);
        const IRECT nameRect(rowRect.L, rowRect.T, rowRect.R, rowRect.T + 20.f);
        const IRECT previewRect(rowRect.L, rowRect.T + 20.f, rowRect.R, rowRect.B);
        g.DrawText(ScaledText(11.f, COLOR_WHITE), preset.name, nameRect.GetPadded(-6.f));
        // A one-line preview under each preset's name (its first band's
        // own type + Hz), so you can tell what you're about to drop
        // without already knowing the names by heart. Only the FIRST
        // band shown even for a preset with more than one -- a short
        // label, not a full dump of every band.
        if (preset.bandCount > 0)
        {
          const PresetBand& pb = preset.bands[0];
          const char* typeName = "Type";
          if (pb.ordinal == 0) typeName = "Bell";
          else if (pb.ordinal == 2) typeName = "LP12";
          else if (pb.ordinal == 6) typeName = "HP12";
          else if (pb.ordinal == 9) typeName = "BP12";
          else if (pb.ordinal == 11) typeName = "Notch12";
          char buf[48];
          char hzText[24];
          FormatHzReadout(pb.cutoffHz, hzText, sizeof(hzText));
          std::snprintf(buf, sizeof(buf), "%s @ %s", typeName, hzText);
          g.DrawText(ScaledText(9.f, COLOR_GRAY), buf, previewRect.GetPadded(-6.f));
        }
      }

      // NEW THIS ROUND -- the "+ Save Current" action row, right after
      // every real preset. Not draggable (OnMouseDown's tray hit-test
      // treats this exact row index, == presetCount, as a click-to-save
      // instead of a drag start) -- captures mSelectedCircle's own
      // bands (up to 3) as a new entry at the end of this same list.
      {
        const IRECT saveRowRect(trayRect.L, trayRect.T + presetCount * kPresetRowH, trayRect.R, trayRect.T + (presetCount + 1) * kPresetRowH);
        if (mHoverValid && saveRowRect.Contains(mHoverX, mHoverY))
          g.FillRect(IColor(60, 217, 164, 76), saveRowRect);
        g.DrawText(ScaledText(11.f, IColor(255, 217, 164, 76)), "+ Save Current", saveRowRect.GetPadded(-6.f));
      }
    }

    if (mDraggingPreset && mDraggedPresetIndex >= 0)
    {
      // Drag ghost -- a small dot + the preset's own name following the
      // cursor, same minimal-feedback approach DrawCrosshair already
      // uses elsewhere in this file (no new draw primitive introduced).
      const int presetCount = TotalPresetCount();
      g.FillCircle(IColor(200, 217, 164, 76), mPresetDragX, mPresetDragY, 5.f);
      if (mDraggedPresetIndex < presetCount)
      {
        const PresetCircleDef preset = GetPresetAt(mDraggedPresetIndex);
        g.DrawText(ScaledText(11.f, COLOR_WHITE), preset.name, IRECT(mPresetDragX + 8.f, mPresetDragY - 8.f, mPresetDragX + 140.f, mPresetDragY + 8.f));
      }
    }

    // NEW THIS ROUND -- live drop-target feedback while a preset is
    // being dragged: highlights whichever outcome WOULD happen if you
    // released right now (replace/insert/add-new), same three outcomes
    // OnMouseUp's own drop resolution already implements -- this is
    // purely visual, it doesn't change what actually happens on release.
    if (mDraggingPreset)
    {
      const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
      const IRECT railArea = mRECT.GetFromRight(kRailW);
      const ArcGeom arc = GetArcGeom(mainArea);
      constexpr float kDeadCenterRadiusPx = 42.f; // same radius OnMouseUp's own drop resolution uses
      const float distFromCenter = std::sqrt((mPresetDragX - arc.cx) * (mPresetDragX - arc.cx) + (mPresetDragY - arc.cy) * (mPresetDragY - arc.cy));
      if (distFromCenter <= kDeadCenterRadiusPx)
      {
        // REPLACE -- ring around the whole selected-circle arc.
        DrawHemiArc(g, IColor(220, 217, 76, 76), arc.cx, arc.cy, arc.r, 4.f);
      }
      else if (railArea.Contains(mPresetDragX, mPresetDragY))
      {
        // INSERT -- a highlighted line at the exact gap this would land in.
        const std::vector<CircleInfo> circles = GatherCircles();
        constexpr int kMaxVisibleRows = 8;
        const float rowH = railArea.H() / static_cast<float>(kMaxVisibleRows);
        const int row = static_cast<int>((mPresetDragY - railArea.T) / rowH);
        const float lineY = railArea.T + row * rowH;
        g.FillRect(IColor(220, 217, 164, 76), IRECT(railArea.L, lineY - 1.5f, railArea.R, lineY + 1.5f));
      }
      else
      {
        // ADD NEW -- a small "+" marker right at the drop point.
        g.DrawText(ScaledText(16.f, IColor(220, 217, 164, 76)), "+", IRECT(mPresetDragX - 10.f, mPresetDragY - 10.f, mPresetDragX + 10.f, mPresetDragY + 10.f));
      }
    }
  }

  // Deactivates every active band currently assigned to `circleId` --
  // same mechanism OnMouseDblClick's own circle-delete already uses.
  void ClearCircleBands(int circleId)
  {
    for (int b = 0; b < kMaxBands; ++b)
    {
      if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
      if (static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != circleId) continue;
      PushParamValue(BandParamIdx(b, kBandActive), 0.0, 0.0, 1.0);
      if (mSelectedBand == b) mSelectedBand = -1;
    }
  }

  // Writes `preset`'s own bands into fresh inactive slots, all tagged to
  // `circleId`, and selects the circle (and its last-placed band) --
  // same per-band param set OnMouseUp's own band-creation path already
  // pushes, just looped over however many bands this preset has.
  void PlacePresetBandsInCircle(const PresetCircleDef& preset, int circleId)
  {
    int lastPlaced = -1;
    for (int i = 0; i < preset.bandCount; ++i)
    {
      const int freeBand = FirstInactiveBandSlot();
      if (freeBand < 0) break; // every slot already in use -- place as many as fit, same honest partial-success as everywhere else band slots run out
      const PresetBand& pb = preset.bands[i];
      PushParamValue(BandParamIdx(freeBand, kBandCutoff), std::clamp(pb.cutoffHz, 20.0, 20000.0), 20.0, 20000.0);
      PushParamValue(BandParamIdx(freeBand, kBandResonance), std::clamp(pb.resonance, 0.05, 20.0), 0.05, 20.0);
      PushParamValue(BandParamIdx(freeBand, kBandFilterType), static_cast<double>(pb.ordinal), 0.0, static_cast<double>(kNumFilterTypeOptions - 1));
      if (pb.forceMorph50)
        PushParamValue(BandParamIdx(freeBand, kBandMorph), 50.0, 0.0, 100.0);
      PushParamValue(BandParamIdx(freeBand, kBandCircleIndex), static_cast<double>(circleId), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
      PushParamValue(BandParamIdx(freeBand, kBandActive), 1.0, 0.0, 1.0);
      lastPlaced = freeBand;
    }
    mSelectedCircle = circleId;
    if (lastPlaced >= 0) SelectBand(lastPlaced);
  }

  // Makes room for a new circle AT `insertAt` by moving every band
  // currently in circle `insertAt` or any circle after it up by one slot
  // (kBandCircleIndex + 1) -- series order is circle-ID order (see
  // CircleInfo/GatherCircles' own comment), so this is a real reorder,
  // not just a relabeling. SIMPLIFICATION, flagged here and in the
  // README: if a band is already sitting in the very last circle slot
  // (kMaxCircles - 1), it has nowhere to shift UP to and this clamps it
  // in place instead of erroring or wrapping -- with 256 circle slots
  // and realistic band counts this is a corner case, not something a
  // real project is likely to hit, but it IS a real edge case rather
  // than a fully general reorder.
  void ShiftCircleIndicesUpFrom(int insertAt)
  {
    // Walk from the HIGHEST circle index down to `insertAt` so a band
    // already shifted into a slot this same pass doesn't get shifted a
    // second time.
    for (int targetId = exoeq::kMaxCircles - 1; targetId >= insertAt; --targetId)
    {
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        if (static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != targetId) continue;
        const int shifted = std::min(exoeq::kMaxCircles - 1, targetId + 1);
        if (shifted != targetId)
          PushParamValue(BandParamIdx(b, kBandCircleIndex), static_cast<double>(shifted), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
      }
    }
  }

  void SwapCircleOrder(int firstId, int secondId)
  {
    if (firstId == secondId) return;
    for (int b = 0; b < kMaxBands; ++b)
    {
      if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
      const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value());
      if (circleId == firstId)
        PushParamValue(BandParamIdx(b, kBandCircleIndex), static_cast<double>(secondId), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
      else if (circleId == secondId)
        PushParamValue(BandParamIdx(b, kBandCircleIndex), static_cast<double>(firstId), 0.0, static_cast<double>(exoeq::kMaxCircles - 1));
    }
  }

  // NEW THIS ROUND -- undo/redo, per your spec ("undo redo would be just
  // some arrows"). Whole-project snapshots (every band param's current
  // NORMALIZED value, captured/restored via IParam::GetNormalized()/
  // SendParameterValueFromUI directly -- no per-param real-value-range
  // bookkeeping needed, unlike PushParamValue elsewhere in this file,
  // since normalized values round-trip through any param's range as-is)
  // rather than per-action diffs -- simpler and more robust against
  // missing a mutation site than hand-tracking "what exactly changed"
  // for every one of this file's many PushParamValue call sites. Capped
  // depth (kUndoDepth) so this can't grow unbounded over a long session.
  // NOTE on IParam::GetNormalized(): standard iPlug2 IParam method, same
  // can't-verify-against-your-exact-headers caveat as everywhere else in
  // this project -- paste the error if this specific call doesn't
  // compile.
  static constexpr int kUndoDepth = 24;

  std::vector<double> CaptureSnapshot()
  {
    std::vector<double> snap(static_cast<size_t>(kMaxBands) * kNumParamsPerBand);
    for (int i = 0; i < kMaxBands * kNumParamsPerBand; ++i)
      snap[i] = GetDelegate()->GetParam(kBandParamsStart + i)->GetNormalized();
    return snap;
  }

  void RestoreSnapshot(const std::vector<double>& snap)
  {
    for (int i = 0; i < kMaxBands * kNumParamsPerBand && i < static_cast<int>(snap.size()); ++i)
      GetDelegate()->SendParameterValueFromUI(kBandParamsStart + i, snap[i]);
  }

  void PushUndoSnapshot()
  {
    mUndoStack.push_back(CaptureSnapshot());
    if (mUndoStack.size() > static_cast<size_t>(kUndoDepth)) mUndoStack.erase(mUndoStack.begin());
    mRedoStack.clear(); // a fresh action invalidates whatever redo history existed, same as every standard undo/redo stack
  }

  void Undo()
  {
    if (mUndoStack.empty()) return;
    mRedoStack.push_back(CaptureSnapshot());
    if (mRedoStack.size() > static_cast<size_t>(kUndoDepth)) mRedoStack.erase(mRedoStack.begin());
    RestoreSnapshot(mUndoStack.back());
    mUndoStack.pop_back();
    mSelectedBand = -1; // the restored state may not even have this band active anymore -- safest to just drop the selection rather than point it at something stale
    SetDirty(false);
  }

  void Redo()
  {
    if (mRedoStack.empty()) return;
    mUndoStack.push_back(CaptureSnapshot());
    if (mUndoStack.size() > static_cast<size_t>(kUndoDepth)) mUndoStack.erase(mUndoStack.begin());
    RestoreSnapshot(mRedoStack.back());
    mRedoStack.pop_back();
    mSelectedBand = -1;
    SetDirty(false);
  }

  // Small top-left pair of text buttons -- "just some arrows" per your
  // spec, built as plain text labels ("UNDO"/"REDO") rather than literal
  // arrow glyphs: a Unicode arrow character's rendering depends on
  // whatever font this project's build actually embeds (ROBOTO_FN in
  // config.h), which isn't something I can verify glyph coverage for
  // from here -- plain ASCII text is guaranteed to render in any font.
  // Dimmed (not hidden) whenever that direction's stack is empty, same
  // "still there, visibly inert" convention a disabled toolbar button
  // uses elsewhere.
  IRECT GetUndoButtonRect() const { return IRECT(mRECT.L + 8.f, mRECT.T + 4.f, mRECT.L + 48.f, mRECT.T + 22.f); }
  IRECT GetRedoButtonRect() const { return IRECT(mRECT.L + 52.f, mRECT.T + 4.f, mRECT.L + 92.f, mRECT.T + 22.f); }

  void DrawUndoRedo(IGraphics& g)
  {
    using namespace iplug::igraphics;
    const IRECT undoRect = GetUndoButtonRect();
    const IRECT redoRect = GetRedoButtonRect();
    const IColor activeBg(180, 60, 60, 68);
    const IColor inactiveBg(90, 40, 40, 44);
    const IColor activeText = COLOR_WHITE;
    const IColor inactiveText = COLOR_GRAY;
    // NEW -- brighten on hover, but only when the button is actually
    // clickable (an empty stack stays dim/inert so hovering it doesn't
    // falsely promise an action nothing will happen on).
    const bool undoHover = mHoverValid && !mUndoStack.empty() && undoRect.Contains(mHoverX, mHoverY);
    const bool redoHover = mHoverValid && !mRedoStack.empty() && redoRect.Contains(mHoverX, mHoverY);
    const IColor hoverBg(220, 100, 100, 112);
    g.FillRect(mUndoStack.empty() ? inactiveBg : (undoHover ? hoverBg : activeBg), undoRect);
    g.FillRect(mRedoStack.empty() ? inactiveBg : (redoHover ? hoverBg : activeBg), redoRect);
    g.DrawText(ScaledText(11.f, mUndoStack.empty() ? inactiveText : activeText), "UNDO", undoRect);
    g.DrawText(ScaledText(11.f, mRedoStack.empty() ? inactiveText : activeText), "REDO", redoRect);
  }

  struct CircleInfo { int circleId; double avgHz; int bandCount; };

  // One pass over every band slot, bucketed by its current circle
  // assignment -- O(kMaxBands), same cost tier as BandGraphControl's own
  // per-draw gather loop, called once per Draw/mouse-event, not per
  // sample (this is UI-thread work, nowhere near the audio thread).
  std::vector<CircleInfo> GatherCircles()
  {
    double sumHz[exoeq::kMaxCircles] = {};
    int count[exoeq::kMaxCircles] = {};
    for (int b = 0; b < kMaxBands; ++b)
    {
      const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
      if (!active) continue;
      const int circleId = std::clamp(static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value()), 0, exoeq::kMaxCircles - 1);
      sumHz[circleId] += GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
      ++count[circleId];
    }
    std::vector<CircleInfo> result;
    for (int c = 0; c < exoeq::kMaxCircles; ++c)
      if (count[c] > 0)
        result.push_back({ c, sumHz[c] / count[c], count[c] });
    // Series/signal-flow order IS circle-ID order (see DSP/CircleChain.h's
    // header note on why there's no separate order param) -- GatherCircles
    // already iterates circleId ascending, so `result` is naturally in
    // signal-flow order without a separate sort.
    return result;
  }

  bool EnsureSelectionValid(const std::vector<CircleInfo>& circles)
  {
    if (circles.empty()) { mSelectedCircle = 0; return false; }
    for (const auto& c : circles) if (c.circleId == mSelectedCircle) return true;
    mSelectedCircle = circles.front().circleId;
    return true;
  }

  // Smallest circle ID (0..kMaxCircles-1) currently held by NO active
  // band -- `circles` is already in ascending-ID order (GatherCircles'
  // own comment), so this is a simple ordered scan, not a search.
  // MOVED to right after CircleInfo/GatherCircles/EnsureSelectionValid --
  // your build error (C2065 'CircleInfo': undeclared identifier) was
  // this function's own parameter type being used before CircleInfo's
  // struct declaration: a member function's PARAMETER TYPES are parsed
  // immediately at their point of declaration, unlike the function BODY
  // (which gets "complete-class" lookup and can reference anything
  // declared anywhere in the class) -- so a nested type used as a
  // parameter type has to already be declared above it, textually.
  int FirstFreeCircleId(const std::vector<CircleInfo>& circles) const
  {
    int next = 0;
    for (const auto& c : circles)
    {
      if (c.circleId != next) return next; // a gap -- reuse it
      ++next;
    }
    return (next < exoeq::kMaxCircles) ? next : -1; // -1: every one of kMaxCircles slots already holds a circle
  }

  // Which circle-ID "gap" the rail drop point falls into, for the
  // preset tray's insert-at-order case -- same row math RailRowAt
  // already uses, but returns the BOUNDARY between rows (where a new
  // circle should land) rather than an existing row's own circle ID.
  // MOVED for the same CircleInfo-declaration-order reason as
  // FirstFreeCircleId just above.
  int RailInsertionPoint(const IRECT& railArea, const std::vector<CircleInfo>& circles, float y) const
  {
    constexpr int kMaxVisibleRows = 8;
    const float rowH = railArea.H() / static_cast<float>(kMaxVisibleRows);
    const int row = static_cast<int>((y - railArea.T) / rowH);
    int selectedIdx = 0;
    for (size_t i = 0; i < circles.size(); ++i) if (circles[i].circleId == mSelectedCircle) selectedIdx = static_cast<int>(i);
    const int firstVisible = ClampRailScrollOffset(selectedIdx, static_cast<int>(circles.size())) + selectedIdx - kMaxVisibleRows / 2;
    const int idx = std::clamp(firstVisible + row, 0, static_cast<int>(circles.size()));
    if (idx < static_cast<int>(circles.size())) return circles[idx].circleId;
    return circles.empty() ? 0 : std::min(exoeq::kMaxCircles - 1, circles.back().circleId + 1);
  }

  // Red(low)..magenta..violet..blue..cyan(high), routed around yellow-
  // green per the user's explicit call during the mockup round -- a
  // 4-stop piecewise lerp across that same hue path, keyed by this
  // circle's OWN average frequency (one flat color for the whole circle,
  // NOT a per-band/per-position gradient -- the correction the user made
  // after the first version of this got it wrong).
  // Fixed per-circle palette (see the palette table in the README). Index = circleId % 8.
  // Ghost = bg(10,10,12) + 0.35*(color - bg): same hue, dimmed, so ghosted nodes stay tied to their circle.
  static iplug::igraphics::IColor CircleColorFor(int circleId, bool ghost)
  {
    using iplug::igraphics::IColor;
    struct Pal { int r, g, b, gr, gg, gb; };
    static const Pal kPal[8] = {
      { 255, 107,  74,  96,  44,  34 }, // 0 coral
      {  47, 209, 181,  23,  80,  71 }, // 1 teal
      { 255, 178,  63,  96,  69,  30 }, // 2 amber
      {  91, 108, 255,  38,  44,  97 }, // 3 indigo
      { 182, 224,  60,  70,  85,  29 }, // 4 lime
      { 224,  69, 123,  85,  31,  51 }, // 5 rose
      {  79, 184, 240,  34,  71,  92 }, // 6 sky
      { 178, 107, 214,  69,  44,  83 }, // 7 plum
    };
    const Pal& p = kPal[((circleId % 8) + 8) % 8];
    return ghost ? IColor(255, p.gr, p.gg, p.gb) : IColor(255, p.r, p.g, p.b);
  }

  iplug::igraphics::IColor CircleHueColor(double avgHz) const
  {
    using namespace iplug::igraphics;
    const double t = std::clamp(std::log2(std::clamp(avgHz, 20.0, 20000.0) / 20.0) / std::log2(20000.0 / 20.0), 0.0, 1.0);
    struct Stop { double t; int r, g, b; };
    static const Stop kStops[] = {
      { 0.0,  214, 74,  79  }, // red
      { 0.35, 170, 70,  196 }, // magenta
      { 0.55, 143, 95,  201 }, // violet
      { 0.75, 90,  127, 212 }, // blue
      { 1.0,  63,  160, 201 }  // cyan
    };
    for (size_t i = 1; i < sizeof(kStops) / sizeof(Stop); ++i)
    {
      if (t > kStops[i].t) continue;
      const Stop& a = kStops[i - 1];
      const Stop& b = kStops[i];
      const double lt = (t - a.t) / std::max(1e-6, (b.t - a.t));
      return IColor(255,
        static_cast<int>(a.r + (b.r - a.r) * lt),
        static_cast<int>(a.g + (b.g - a.g) * lt),
        static_cast<int>(a.b + (b.b - a.b) * lt));
    }
    return IColor(255, kStops[4].r, kStops[4].g, kStops[4].b);
  }

  // One fixed amber accent for every band inside every circle, regardless
  // of that circle's own hue or the band's own frequency -- per the
  // user's correction: bands are a complementary color, not individually
  // tinted by position.
  iplug::igraphics::IColor AccentColor() const { return iplug::igraphics::IColor(255, 232, 236, 242); }

  // Conceptual-art palette: blue-gray for structure, white for nodes,
  // restrained blue-gray glow for active states.
  // Hemicircle (180..360 degrees) as a polyline of DrawLine segments, 3 degrees
  // per step. Replaces g.DrawArc, which in the user's build only drew from
  // 180 to 270 degrees (arcs ended flat at the vertical center line).
  static void DrawHemiArc(IGraphics& g, const iplug::igraphics::IColor& color, float cx, float cy, float r, float thickness)
  {
    constexpr int kSteps = 60;
    float px = cx - r, py = cy;
    for (int i = 1; i <= kSteps; ++i)
    {
      const double a = (180.0 + 180.0 * i / kSteps) * kPi / 180.0;
      const float nx = cx + r * static_cast<float>(std::cos(a));
      const float ny = cy + r * static_cast<float>(std::sin(a));
      g.DrawLine(color, px, py, nx, ny, nullptr, thickness);
      px = nx;
      py = ny;
    }
  }

  // NEW -- same polyline technique as DrawHemiArc above, but for an
  // arbitrary [angleLoDeg, angleHiDeg] sub-span rather than the whole
  // 180..360 hemicircle. This is the visible connecting line for a
  // two-band circle's draggable segment (DrawSelectedGauge below draws
  // it, FindBandNear's widened hit-test matches its geometry exactly).
  static void DrawArcSpan(IGraphics& g, const iplug::igraphics::IColor& color, float cx, float cy, float r, double angleLoDeg, double angleHiDeg, float thickness)
  {
    const double span = std::max(0.0, angleHiDeg - angleLoDeg);
    const int steps = std::max(1, static_cast<int>(span / 3.0));
    const double a0 = angleLoDeg * kPi / 180.0;
    float px = cx + r * static_cast<float>(std::cos(a0));
    float py = cy + r * static_cast<float>(std::sin(a0));
    for (int i = 1; i <= steps; ++i)
    {
      const double a = (angleLoDeg + span * i / steps) * kPi / 180.0;
      const float nx = cx + r * static_cast<float>(std::cos(a));
      const float ny = cy + r * static_cast<float>(std::sin(a));
      g.DrawLine(color, px, py, nx, ny, nullptr, thickness);
      px = nx;
      py = ny;
    }
  }

  // Text multiplier. Left at 1.0 on purpose: ExoEQ's layout already attaches
  // AttachCornerResizer(EUIResizerMode::Scale), so iPlug2 scales all drawing,
  // text included, with the window. A second multiplier on top of that caused
  // the overlap/wrap seen in the 4x test. Only change this if text needs to
  // scale differently from the window.
  static constexpr float kUiTextScale = 1.5f; // matched to Pro-Q's label size; every box below was widened to suit
  static iplug::igraphics::IText ScaledText(float size, iplug::igraphics::IColor color)
  {
    return iplug::igraphics::IText(size * kUiTextScale, color);
  }

  // ---- Pro-Q-style readability layer -------------------------------------
  // Three things the FabFilter comparison made obvious that this graph simply
  // did not have: a labelled frequency grid, dB reference rings, and the actual
  // EQ response curve. Without them the arc shows WHERE bands sit but nothing
  // about WHAT they do, and there is no reference between 20Hz and 20kHz.

  static constexpr float kDbRange = 18.f;   // radial span mapped either side of the 0 dB arc
  static constexpr float kDbSpanFrac = 0.17f; // that range as a fraction of the base radius

  // dB -> radius. 0 dB sits exactly on the base arc; boost pushes outward.
  // FIXED THIS ROUND -- "after adding a lot of bands there's a limit on
  // the curve, some added bands look like they do nothing": this used to
  // be a HARD clamp at +/-kDbRange. Stack enough bands at/near the same
  // spot and their summed response (ResponseDbAt sums every band's own
  // BandResponseDb) blows past that well before you've run out of bands
  // to add -- once it's pinned at the limit, every FURTHER band really
  // does draw as doing nothing, because the curve is already dead flat
  // there; it's a rendering ceiling, not the DSP actually doing nothing.
  // tanh is a soft-knee instead: near 0 dB it's close to linear (tanh's
  // own slope at the origin is 1, so ordinary, modest filters read
  // exactly as before), and it asymptotes toward the SAME +/-kDbRange
  // bound further out -- so the curve still can't run off the graph with
  // a "super high" value -- but it never actually FLATTENS: every extra
  // band stacked on top still nudges it some amount, just a shrinking
  // amount the further out you already are, instead of zero.
  float RadiusForDb(float baseRadius, double db) const
  {
    const double compressed = static_cast<double>(kDbRange) * std::tanh(db / static_cast<double>(kDbRange));
    return baseRadius + static_cast<float>(compressed / kDbRange) * baseRadius * kDbSpanFrac;
  }

  // Approximate magnitude response of ONE band, in dB, at normalised position t.
  // This is a DISPLAY approximation driven by the band's own cutoff/Q/gain/type
  // ordinal -- it is not the DSP's own transfer function, so it shows the shape
  // and direction of a filter, not a measured curve. Swapping this for the real
  // per-type magnitude response is the obvious follow-up.
  double BandResponseDb(int ordinal, double cutoffHz, double q, double gainDb, double t)
  {
    const double f = TToHz(t);
    if (f <= 0.0 || cutoffHz <= 0.0) return 0.0;
    const double x = std::log2(f / cutoffHz);          // octaves from cutoff
    const double width = std::max(0.25, 1.0 / std::max(0.2, q)); // octaves
    const double bump = std::exp(-(x / width) * (x / width));

    // Families follow ExoEQ.cpp's InitEnum ordering.
    if (ordinal >= 1 && ordinal <= 4)                  // Lowpass 6/12/24/48
    {
      const double slope[4] = { 6.0, 12.0, 24.0, 48.0 };
      return -slope[ordinal - 1] * std::max(0.0, x) + (q > 0.8 ? 6.0 * bump : 0.0);
    }
    if (ordinal >= 5 && ordinal <= 8)                  // Highpass 6/12/24/48
    {
      const double slope[4] = { 6.0, 12.0, 24.0, 48.0 };
      return -slope[ordinal - 5] * std::max(0.0, -x) + (q > 0.8 ? 6.0 * bump : 0.0);
    }
    if (ordinal == 9 || ordinal == 10)                 // Bandpass 12/24
      return -(ordinal == 9 ? 12.0 : 24.0) * std::abs(x);
    if (ordinal == 11)                                 // Notch 12
      return -36.0 * bump;
    // Morph (0) and everything experimental: read as a peaking/bell trim.
    return gainDb * bump;
  }

  // One contributing band, already read out of the param system.
  // One active band, as the response curve needs it: the exact FilterParams the
  // audio chain builds (ReadBandSettings in ExoEQ.cpp, mirrored here), the band's
  // linear gain, and its slot so the cached table can be found.
  struct ResponseBand { exoeq::FilterParams filter; double gainLinear; int slot; };

  // Per-band cache of the measured complex response. Rebuilt only when that
  // band's filter params or the sample rate change. Gain is NOT in the key: it
  // is applied outside the table, so gain drags cost nothing.
  // While a retune drag is in progress a coarse grid is used, so each mouse
  // event stays cheap; the full grid is rebuilt once on release.
  static constexpr int kCurvePointsFull = 96;
  static constexpr int kCurvePointsDrag = 32;
  static constexpr double kCurveFMin = 20.0, kCurveFMax = 20000.0;
  struct BandResponseCache
  {
    bool valid = false;
    exoeq::FilterParams key{};
    double sampleRate = 0.0;
    int points = 0;
    std::vector<double> freqs;
    std::vector<std::complex<double>> h;
  };
  BandResponseCache mBandCache[kMaxBands];

  bool IsRetuneDragging() const { return mDraggingExistingBand || mDraggingResonanceHandle || mDraggingGain; }

  static bool SameFilterParams(const exoeq::FilterParams& a, const exoeq::FilterParams& b)
  {
    return a.cutoffHz == b.cutoffHz && a.resonance == b.resonance && a.morph == b.morph
        && a.type == b.type && a.extra == b.extra;
  }

  // Makes sure slot `b` has a table that matches its current params.
  void EnsureBandCache(int b, const exoeq::FilterParams& fp)
  {
    BandResponseCache& c = mBandCache[b];
    const int wantPoints = IsRetuneDragging() ? kCurvePointsDrag : kCurvePointsFull;
    if (c.valid && c.points == wantPoints && c.sampleRate == mSampleRate && SameFilterParams(c.key, fp))
      return;
    c.freqs = exoeq::MakeLogFreqGrid(kCurveFMin, kCurveFMax, wantPoints);
    exoeq::BuildResponseTable(fp, mSampleRate, c.freqs, c.h);
    c.key = fp;
    c.sampleRate = mSampleRate;
    c.points = wantPoints;
    c.valid = true;
  }

  // PERFORMANCE / SAFETY. This used to read every param for all 256 band slots
  // on EVERY curve sample. DrawResponseCurve takes 140 samples and the crosshair
  // called it again, so a single repaint did roughly 36,000 GetParam() calls --
  // while the host is pushing param changes from its own thread during a knob
  // drag. That is both far too slow for a weak machine and the most likely
  // trigger for the FL access violation. Now the active bands are gathered ONCE
  // per repaint and the curve is evaluated from that small snapshot.
  void GatherResponseBands(int circleId, std::vector<ResponseBand>& out)
  {
    out.clear();
    if (mSampleRate <= 0.0) return;
    for (int b = 0; b < kMaxBands; ++b)
    {
      const iplug::IParam* pActive = GetDelegate()->GetParam(BandParamIdx(b, kBandActive));
      if (!pActive || pActive->Value() <= 0.5) continue;
      const iplug::IParam* pCircle = GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex));
      if (!pCircle || static_cast<int>(pCircle->Value()) != circleId) continue;
      const iplug::IParam* pMute = GetDelegate()->GetParam(BandParamIdx(b, kBandMute));
      if (pMute && pMute->Value() > 0.5) continue;
      const iplug::IParam* pType = GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType));
      const iplug::IParam* pCut  = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff));
      const iplug::IParam* pReso = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance));
      const iplug::IParam* pGain = GetDelegate()->GetParam(BandParamIdx(b, kBandGainDb));
      const iplug::IParam* pMorph = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph));
      const iplug::IParam* pExtra = GetDelegate()->GetParam(BandParamIdx(b, kBandFilterExtra));
      if (!pType || !pCut || !pReso || !pGain || !pMorph || !pExtra) continue;

      // Mirrors ExoEQ.cpp's ReadBandSettings exactly: morph and extra are
      // percent in the UI, /100 for the DSP; gain is dB -> linear.
      // Key-track and sidechain/group modulation are not reflected: the
      // curve shows the band at its manual cutoff.
      exoeq::FilterParams fp{ pCut->Value(), pReso->Value(), pMorph->Value() / 100.0,
                              static_cast<exoeq::FilterTypeId>(static_cast<int>(pType->Value())),
                              pExtra->Value() / 100.0 };
      EnsureBandCache(b, fp);
      out.push_back({ fp, std::pow(10.0, pGain->Value() / 20.0), b });
    }
  }

  // Summed response at normalised position t, in dB. Parallel bands add as
  // COMPLEX gains (so overlap cancels or reinforces correctly), each read from
  // that band's measured table. GatherResponseBands guarantees every band in
  // `bands` has a current table before this runs.
  double ResponseDbAt(const std::vector<ResponseBand>& bands, double t)
  {
    const double hz = TToHz(t);
    std::complex<double> total(0.0, 0.0);
    for (const auto& rb : bands)
    {
      const BandResponseCache& c = mBandCache[rb.slot];
      total += exoeq::InterpResponse(c.h, kCurveFMin, kCurveFMax, hz) * rb.gainLinear;
    }
    return exoeq::MagnitudeToDb(total);
  }

  // Frequency grid: radial ticks at the decade/half-decade marks Pro-Q labels,
  // each with its own label just outside the arc. Replaces the two lone
  // "20Hz"/"20kHz" end labels, which gave no reference anywhere in between.
  void DrawFrequencyGrid(IGraphics& g, const IRECT& area)
  {
    using namespace iplug::igraphics;
    const ArcGeom arc = GetArcGeom(area);
    struct Tick { double hz; const char* label; bool major; };
    static const Tick kTicks[] = {
      {    20.0,  "20",   true  }, {    50.0,  "50",   false },
      {   100.0,  "100",  true  }, {   200.0,  "200",  false },
      {   500.0,  "500",  false }, {  1000.0,  "1k",   true  },
      {  2000.0,  "2k",   false }, {  5000.0,  "5k",   false },
      { 10000.0,  "10k",  true  }, { 20000.0,  "20k",  true  },
    };
    for (const auto& tick : kTicks)
    {
      const double t = HzToT(tick.hz);
      const double a = (180.0 + t * 180.0) * kPi / 180.0;
      const float ca = static_cast<float>(std::cos(a));
      const float sa = static_cast<float>(std::sin(a));
      const float rIn = arc.r * 0.30f;
      const float rOut = arc.r * (tick.major ? 1.055f : 1.03f);
      // Dotted radial, so the grid stays behind the curve rather than competing.
      const int steps = tick.major ? 34 : 24;
      for (int i = 0; i <= steps; ++i)
      {
        const float rr = rIn + (rOut - rIn) * i / steps;
        g.FillCircle(BlueGray(tick.major ? 30 : 18), arc.cx + rr * ca, arc.cy + rr * sa, 0.7f);
      }
      // Clamped inside the graph: at radius*1.10 the labels near the top of the
      // arc were landing above the graph area and getting clipped away.
      const float lx = std::clamp(arc.cx + (arc.r * 1.10f) * ca, area.L + 28.f, area.R - 28.f);
      const float ly = std::clamp(arc.cy + (arc.r * 1.10f) * sa, area.T + 10.f, area.B - 10.f);
      g.DrawText(ScaledText(tick.major ? 11.f : 9.f, BlueGray(tick.major ? 235 : 165)), tick.label,
                 IRECT(lx - 26.f, ly - 9.f, lx + 26.f, ly + 9.f));
    }
  }

  // dB reference rings either side of the 0 dB arc, labelled at the left foot.
  void DrawDbRings(IGraphics& g, const IRECT& area)
  {
    using namespace iplug::igraphics;
    const ArcGeom arc = GetArcGeom(area);
    const double kRings[] = { 12.0, 6.0, -6.0, -12.0 };
    for (double db : kRings)
    {
      const float r = RadiusForDb(arc.r, db);
      // Unlabelled on purpose. Every ring meets the left foot at the same
      // height, so stacked labels there overlapped into an unreadable smear.
      // The dB value is in the crosshair readout instead, where it has room.
      DrawHemiArc(g, BlueGray(16), arc.cx, arc.cy, r, 1.f);
    }
  }

  // The selected circle's summed response, drawn as a radial deviation from the
  // 0 dB arc: outward is boost, inward is cut. This is the polar equivalent of
  // Pro-Q's curve, and it is what makes the graph show what the filters DO.
  void DrawResponseCurve(IGraphics& g, const IRECT& area, int circleId, bool ghost = false)
  {
    using namespace iplug::igraphics;
    const ArcGeom arc = GetArcGeom(area);
    std::vector<ResponseBand> bands;
    GatherResponseBands(circleId, bands);
    if (!ghost) mLastResponseBands = bands; // reused by the crosshair readout; ghosts must not overwrite it
    if (bands.empty()) return;
    // T2-5: the curve takes its circle's hue (CircleHueColor of the mean band frequency).
    const IColor hue = CircleColorFor(circleId, false);
    constexpr int kSteps = 140;
    float px = 0.f, py = 0.f;
    const int limit = ghost ? kSteps : static_cast<int>(kSteps * mLoadProgress + 0.5f); // T4-1 sweep
    for (int i = 0; i <= kSteps && i <= limit; ++i)
    {
      const double t = static_cast<double>(i) / kSteps;
      const double db = ResponseDbAt(bands, t);
      const float r = RadiusForDb(arc.r, db);
      const double a = (180.0 + t * 180.0) * kPi / 180.0;
      const float cx = arc.cx + r * static_cast<float>(std::cos(a));
      const float cy = arc.cy + r * static_cast<float>(std::sin(a));
      // Shading back to the 0 dB arc. Drawn every 4th sample, not every sample:
      // at 1-per-sample the 140 strokes overlapped into a solid dark comb that
      // read as noise instead of as a filled region.
      if ((i % 4) == 0 && std::abs(db) > 0.4)
      {
        const float zx = arc.cx + arc.r * static_cast<float>(std::cos(a));
        const float zy = arc.cy + arc.r * static_cast<float>(std::sin(a));
        if (!ghost) g.DrawLine(IColor(30, hue.R, hue.G, hue.B), cx, cy, zx, zy, nullptr, 1.2f);
      }
      if (i > 0)
        // Ghosts are dashed (every other segment) and faint, so they read as 'other circles'.
        if (!ghost || (i & 1) == 0)
          g.DrawLine(IColor(ghost ? static_cast<int>(90.f * mGhostFade) : 255, hue.R, hue.G, hue.B), px, py, cx, cy, nullptr, ghost ? 1.4f : 2.2f);
      px = cx;
      py = cy;
    }
  }

  static iplug::igraphics::IColor BlueGray(int a) { return iplug::igraphics::IColor(a, 116, 138, 168); }

  void DrawDepthStack(IGraphics& g, const IRECT& area, const std::vector<CircleInfo>& circles)
  {
    using namespace iplug::igraphics;
    // Depth rank: distance from the selected circle in the (already
    // signal-flow-ordered) `circles` list -- rank 0 is the selected
    // circle itself (drawn largest, full opacity), rank N dims and
    // shrinks further out, same rolodex falloff as the mockups.
    int targetSelectedPos = 0;
    for (size_t i = 0; i < circles.size(); ++i) if (circles[i].circleId == mSelectedCircle) targetSelectedPos = static_cast<int>(i);
    if (std::abs(mVisualSelectedPos - static_cast<float>(targetSelectedPos)) > 0.01f)
    {
      mVisualSelectedPos += (static_cast<float>(targetSelectedPos) - mVisualSelectedPos) * 0.18f;
      SetDirty(false);
    }
    else
      mVisualSelectedPos = static_cast<float>(targetSelectedPos);

    const ArcGeom arc = GetArcGeom(area);
    const float cx = arc.cx;
    const float cy = arc.cy;
    const float baseRadius = arc.r;

    // Radar/oscilloscope grid: faint concentric rings, radial spokes, and a
    // static sweep line (static on purpose -- an animated sweep needs a timer).
    for (int ring = 1; ring <= 3; ++ring)
      DrawHemiArc(g, BlueGray(22), cx, cy, baseRadius * ring / 3.f, 1.f);
    for (int spoke = 0; spoke <= 6; ++spoke)
    {
      const double ang = (180.0 + spoke * 30.0) * kPi / 180.0;
      for (int d = 1; d <= 24; ++d)
      {
        const float f = baseRadius * d / 24.f;
        g.FillCircle(BlueGray(26), cx + f * static_cast<float>(std::cos(ang)), cy + f * static_cast<float>(std::sin(ang)), 0.8f);
      }
    }
    const double sweepAng = 300.0 * kPi / 180.0;
    for (int d = 1; d <= 30; ++d)
    {
      const float f = baseRadius * d / 30.f;
      g.FillCircle(IColor(static_cast<int>(60.f * d / 30.f), 150, 175, 210), cx + f * static_cast<float>(std::cos(sweepAng)), cy + f * static_cast<float>(std::sin(sweepAng)), 1.1f);
    }

    for (size_t i = 0; i < circles.size(); ++i)
    {
      const float rank = std::abs(static_cast<float>(i) - mVisualSelectedPos);
      const float scale = std::max(0.55f, 1.0f - rank * 0.08f);
      const float opacity = std::max(0.12f, 1.0f - rank * 0.16f);
      const IColor faded = BlueGray(static_cast<int>(opacity * 255));
      const float r = baseRadius * scale;
      DrawHemiArc(g, faded, cx, cy, r, 2.5f + (rank < 0.05f ? 1.f : 0.f));
    }
    char layerLabel[32];
    std::snprintf(layerLabel, sizeof(layerLabel), "CIRCLE %d", targetSelectedPos + 1);
    g.DrawText(ScaledText(11.f, IColor(190, 210, 220, 235)), layerLabel,
               IRECT(cx - baseRadius, cy - baseRadius - 24.f, cx - baseRadius + 104.f, cy - baseRadius - 8.f)); // T1-2: moved off the top-centre where the node tag sits
  }

  void DrawScrim(IGraphics& g, const IRECT& area, const std::vector<CircleInfo>& /*circles*/)
  {
    using namespace iplug::igraphics;
    // 80%-opacity black specifically behind the SELECTED circle's own
    // footprint (not the whole background) -- ghosts peeking out past
    // its edges keep their own, lesser depth-dim level from DrawDepthStack.
    const ArcGeom arc = GetArcGeom(area);
    g.FillRect(IColor(204, 10, 10, 12), IRECT(arc.cx - arc.r, arc.cy - arc.r, arc.cx + arc.r, arc.cy));
  }

  void DrawSelectedGauge(IGraphics& g, const IRECT& area, const std::vector<CircleInfo>& circles)
  {
    using namespace iplug::igraphics;
    // FIXED THIS ROUND: `sel` not found (mSelectedCircle has zero bands,
    // including the common "brand new project, nothing created yet" case)
    // used to bail out of this WHOLE function, which skipped the axis
    // labels and the gauge's own baseline below -- leaving an empty
    // project with literally no visible target to click/drag on, on top
    // of the OnMouseDown/Draw bails already fixed above. `sel` itself is
    // still only used to decide whether there's anything to look up in
    // the per-band loop below (there's nothing to look up when it's
    // null, so that loop's body just never matches -- kept as a `continue`
    // guard instead of returning out of the whole function).
    const CircleInfo* sel = nullptr;
    for (const auto& c : circles) if (c.circleId == mSelectedCircle) sel = &c;

    const IRECT gaugeArea = GaugeArea(area);
    const IColor accent = AccentColor();

    // Baseline guide, drawn even with zero bands -- a plain thin bar so
    // "this is the clickable gauge" reads visually before the first band
    // ever gets created, rather than empty space that's indistinguishable
    // from the rest of the black background. FillRect rather than
    // DrawLine deliberately -- FillRect is already proven to compile
    // elsewhere in this exact file/project; DrawLine's exact signature
    // isn't, and there's no reason to introduce a new unverified call for
    // what a 1px-tall rect does just as well.
    // FIXED THIS ROUND (the "line for the bands is a horizontal line, but
    // it should follow the hemicircle's line instead" report): this used
    // to be a flat FillRect baseline at a fixed y, completely detached
    // from the curve DrawDepthStack draws behind it. Now it's the SAME
    // g.DrawArc call/geometry as DrawDepthStack's own rank-0 (selected)
    // circle -- the guide track band dots sit on IS the visible curve,
    // not an approximation of it.
    {
      const ArcGeom arc = GetArcGeom(area);
      DrawHemiArc(g, IColor(190, 128, 146, 174), arc.cx, arc.cy, arc.r, 2.f);
    }

    // NEW THIS ROUND -- which band (if any) the cursor is currently
    // close enough to for a hover tooltip, found during the same loop
    // that draws every dot below rather than a second pass over
    // kMaxBands. Drawn AFTER the loop (see the bottom of this function)
    // so the tooltip box always sits on top of every dot, not just the
    // hovered one's.
    int hoveredBand = -1;
    float hoveredBandX = 0.f, hoveredBandY = 0.f;
    float selectedBandX = 0.f, selectedBandY = 0.f;
    float hoveredBandDist = 22.f; // keep the readout aligned with the generous selectable hitbox

    if (sel)
    {
      // FIXED: band dots/handles now place themselves using the SAME
      // instant, discrete circle position FindBandNear/
      // FindResonanceHandleNear use for hit-testing (selectedPos, an int),
      // not the eased mVisualSelectedPos DrawDepthStack's own ring
      // outlines slide with. Those two used to disagree for as long as
      // the rolodex-slide animation was still easing toward its target,
      // which is exactly the reported bug: a just-selected circle (one
      // that isn't the first/outermost) visually read as if it were still
      // sitting at its OLD rank while the real click target had already
      // snapped to the new one underneath -- "nodes appear to be on the
      // selected layer but you have to click in the dimmed circle below
      // to actually hit them". The ring outlines keep their nice slide;
      // only the clickable dots/handles are now instant.
      int selectedPos = 0;
      for (size_t i = 0; i < circles.size(); ++i)
        if (circles[i].circleId == mSelectedCircle) selectedPos = static_cast<int>(i);

      const ArcGeom segArc = GetArcGeom(area);

      // NEW -- "two points" per SINGLE wide-type band (not a line between
      // two different bands): a dotted line from the circle's own center
      // out to that band's own dot, ending in a second, separately
      // draggable point whose distance along the line reflects the
      // band's CURRENT resonance/width. Narrow-focused types (bell/
      // bandpass/notch/formant/resonator) skip this entirely -- their
      // whole character already lives at the one point. Drawn before the
      // dot loop so the dots end up on top of their own lines.
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        const int ordinal = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value());
        const double morph = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph))->Value();
        if (IsNarrowFocusType(ordinal, morph)) continue;

        const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value());
        int circlePos = selectedPos;
        const IColor nodeCol = CircleColorFor(circleId, circleId != mSelectedCircle);
        for (size_t ci = 0; ci < circles.size(); ++ci) if (circles[ci].circleId == circleId) circlePos = static_cast<int>(ci);
        const float ringRadius = segArc.r * std::max(0.55f, 1.0f - std::abs(circlePos - selectedPos) * 0.08f);
        const double cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
        const double resonance = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance))->Value();
        const float handleRadius = ResonanceHandleRadius(ringRadius, resonance);
        float dotX = 0.f, dotY = 0.f, handleX = 0.f, handleY = 0.f;
        ArcPointForHzAtRadius(area, cutoffHz, ringRadius, dotX, dotY);
        ArcPointForHzAtRadius(area, cutoffHz, handleRadius, handleX, handleY);

        bool handleHovered = false;
        if (mHoverValid)
        {
          const float hd = std::sqrt((mHoverX - handleX) * (mHoverX - handleX) + (mHoverY - handleY) * (mHoverY - handleY));
          handleHovered = hd <= 14.f;
        }

        // Dotted line, center -> dot: short alternating strokes along the
        // same radial direction, not a solid DrawLine, so it reads as a
        // distinct "this is an axis, not an edge" affordance.
        {
          const double angleRad = std::atan2(static_cast<double>(dotY - segArc.cy), static_cast<double>(dotX - segArc.cx));
          const float totalLen = std::sqrt((dotX - segArc.cx) * (dotX - segArc.cx) + (dotY - segArc.cy) * (dotY - segArc.cy));
          constexpr float kDashLen = 5.f, kGapLen = 4.f;
          float d = 0.f;
          const IColor dashColor = handleHovered ? IColor(200, 120, 190, 235) : IColor(70, nodeCol.R, nodeCol.G, nodeCol.B);
          while (d < totalLen)
          {
            const float d2 = std::min(d + kDashLen, totalLen);
            const float ax = segArc.cx + d * static_cast<float>(std::cos(angleRad));
            const float ay = segArc.cy + d * static_cast<float>(std::sin(angleRad));
            const float bx2 = segArc.cx + d2 * static_cast<float>(std::cos(angleRad));
            const float by2 = segArc.cy + d2 * static_cast<float>(std::sin(angleRad));
            g.DrawLine(dashColor, ax, ay, bx2, by2, nullptr, 1.5f);
            d += kDashLen + kGapLen;
          }
        }
        // The second point itself -- smaller than the main dot, brightens
        // and grows on hover (same visual language as the main dot).
        const IColor handleColor = handleHovered ? COLOR_WHITE : IColor(220, nodeCol.R, nodeCol.G, nodeCol.B);
        if (handleHovered) g.FillCircle(IColor(55, 90, 180, 235), handleX, handleY, 9.f);
        g.FillCircle(handleColor, handleX, handleY, handleHovered ? 4.5f : 3.f);
      }

      for (int b = 0; b < kMaxBands; ++b)
      {
        const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
        if (!active) continue;
        const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value());
        int circlePos = selectedPos;
        const IColor nodeCol = CircleColorFor(circleId, circleId != mSelectedCircle);
        for (size_t ci = 0; ci < circles.size(); ++ci)
        {
          if (circles[ci].circleId == circleId) circlePos = static_cast<int>(ci);
        }
        const float circleRank = std::abs(static_cast<float>(circlePos - selectedPos));
        const float circleRadius = GetArcGeom(area).r * std::max(0.55f, 1.0f - circleRank * 0.08f);

        const double cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();

        // NEW THIS ROUND -- the "automation ghost/audio-rate-particle
        // rendering" deferred item. No visual spec for this ever existed
        // (flagged every round it came up), so this is the most literal
        // reasonable reading of "automation ghost": a short fading trail
        // of this band's OWN recent cutoff values, sampled once per
        // repaint, drawn behind the live dot -- when the host is
        // automating this band's cutoff, the trail visibly stretches out
        // along the arc behind the moving dot; when it's sitting still,
        // every trail sample is the same value and the trail collapses
        // invisibly back onto the dot itself (harmless overdraw, not a
        // separate visual state to handle). PushGhostHistory below does
        // the actual sampling/shifting; drawn oldest-first so the live
        // dot (drawn after this loop) ends up on top.
        PushGhostHistory(b, cutoffHz);
        for (int h = kGhostTrailLen - 1; h >= 1; --h)
        {
          if (h >= mGhostFilled[b]) continue;
          float gx = 0.f, gy = 0.f;
          ArcPointForHzAtRadius(area, mGhostHistory[b][h], circleRadius, gx, gy);
          const float fade = 1.f - static_cast<float>(h) / static_cast<float>(kGhostTrailLen);
          g.FillCircle(IColor(static_cast<int>(fade * 110.f), nodeCol.R, nodeCol.G, nodeCol.B), gx, gy, 2.f + fade * 2.f);
        }

        // FIXED THIS ROUND: dot position is now a point ON the hemicircle
        // arc (ArcPointForHz, below), not a flat x/fixed-y pair.
        float x = 0.f, y = 0.f;
        ArcPointForHzAtRadius(area, cutoffHz, circleRadius, x, y);
        const bool selected = (b == mSelectedBand);
        const bool hovered = mHoverValid && std::sqrt((mHoverX - x) * (mHoverX - x) + (mHoverY - y) * (mHoverY - y)) <= 18.f;
        // NEW THIS ROUND -- muted bands now read as muted ON THE GRAPH
        // itself (dimmed + smaller), not just in the new hover tooltip --
        // before this, a muted band looked completely identical to an
        // active one until you opened the detail panel or hovered it.
        const bool muted = GetDelegate()->GetParam(BandParamIdx(b, kBandMute))->Value() > 0.5;
        IColor dotColor = selected ? COLOR_WHITE : nodeCol;
        float dotRadius = selected ? 6.f : (hovered ? 5.5f : 4.5f);
        // A soft halo makes the active node unmistakable against the arc
        // stack and gives the user a generous visual target for the next
        // drag, matching the direct manipulation language of modern EQs.
        if (selected)
        {
          g.FillCircle(IColor(34, CircleColorFor(circleId, false).R, CircleColorFor(circleId, false).G, CircleColorFor(circleId, false).B), x, y, 16.f);
          g.FillCircle(IColor(70, CircleColorFor(circleId, false).R, CircleColorFor(circleId, false).G, CircleColorFor(circleId, false).B), x, y, 11.f);
          // T3-3 (trial): stage marks on the selected node's own halo, for stepped morph types only.
          // n dots fan out over ~0.9 rad, on the node's radial line; the active stage is white.
          const int stages = MorphStepsForType(static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value()));
          if (stages > 1)
          {
            const double pm = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph))->Value() / 100.0;
            const int activeStage = std::clamp(static_cast<int>(std::lround(pm * (stages - 1))), 0, stages - 1);
            const ArcGeom arcG = GetArcGeom(area);
            const double theta = std::atan2(y - arcG.cy, x - arcG.cx);
            for (int k = 0; k < stages; ++k)
            {
              const double a = theta + (static_cast<double>(k) / (stages - 1) - 0.5) * 0.9;
              const float sx = x + 20.f * static_cast<float>(std::cos(a));
              const float sy = y + 20.f * static_cast<float>(std::sin(a));
              if (k == activeStage) g.FillCircle(COLOR_WHITE, sx, sy, 2.5f);
              else                  g.FillCircle(IColor(120, 170, 190, 220), sx, sy, 1.5f);
            }
          }
        }
        else if (hovered)
          g.FillCircle(IColor(55, 90, 180, 235), x, y, 11.f);
        if (muted)
        {
          dotColor = IColor(140, dotColor.R, dotColor.G, dotColor.B); // same hue, lower alpha -- "still here, just not doing anything right now"
          dotRadius *= 0.75f;
        }
        g.FillCircle(dotColor, x, y, dotRadius);
        if (selected)
        {
          selectedBandX = x;
          selectedBandY = y;
        }

        // NEW THIS ROUND -- hover-tooltip candidacy check, same spot the
        // dot itself is drawn so there's no second coordinate lookup.
        if (mHoverValid)
        {
          const float dist = std::sqrt((mHoverX - x) * (mHoverX - x) + (mHoverY - y) * (mHoverY - y));
          if (dist < hoveredBandDist) { hoveredBandDist = dist; hoveredBand = b; hoveredBandX = x; hoveredBandY = y; }
        }
      }
    }

    // (the old lone "20Hz"/"20kHz" end labels are now part of DrawFrequencyGrid)

    // NEW THIS ROUND -- Pro-Q-style "hover a node, see its own numbers"
    // tooltip: Hz + Q (resonance) + Mute state, right above whichever
    // dot the cursor is closest to (within hoveredBandDist, set above).
    // Separate from DrawCrosshair's own general "wherever you are on the
    // arc" readout -- this one is about a SPECIFIC existing band, not an
    // arbitrary hover position, so it only appears right at a real dot.
    mSelectedTagShown = false; // re-set below only when the selected tag is actually drawn
    if (hoveredBand >= 0)
    {
      const double hz = GetDelegate()->GetParam(BandParamIdx(hoveredBand, kBandCutoff))->Value();
      const double q = GetDelegate()->GetParam(BandParamIdx(hoveredBand, kBandResonance))->Value();
      const bool muted = GetDelegate()->GetParam(BandParamIdx(hoveredBand, kBandMute))->Value() > 0.5;
      char buf[48];
      char hzText[24];
      FormatHzAbbrev(hz, hzText, sizeof(hzText));
      std::snprintf(buf, sizeof(buf), "%s  Q %.2f%s", hzText, q, muted ? "  (muted)" : "");
      const float labelHalfW = 92.f;
      const float labelX = std::clamp(hoveredBandX, area.L + labelHalfW, area.R - labelHalfW);
      const IRECT tipRect(labelX - labelHalfW, hoveredBandY - 34.f, labelX + labelHalfW, hoveredBandY - 12.f);
      g.FillRect(IColor(200, 24, 24, 28), tipRect);
      g.DrawText(ScaledText(10.f, COLOR_WHITE), buf, tipRect);
    }
    else if (mSelectedBand >= 0)
    {
      // Pro-Q does not float a panel over its own curve: the selected band's
      // numbers live in the docked strip at the bottom. Ours did both, so the
      // big three-line HUD covered the curve it was describing AND duplicated
      // the drawer that now shows Type/Cutoff/Reso/Gain/Mute. Reduced to a
      // single compact value tag pinned to the node.
      const double hz = GetDelegate()->GetParam(BandParamIdx(mSelectedBand, kBandCutoff))->Value();
      const double gainDb = GetDelegate()->GetParam(BandParamIdx(mSelectedBand, kBandGainDb))->Value();
      char tag[48];
      char hzText[24];
      FormatHzAbbrev(hz, hzText, sizeof(hzText));
      std::snprintf(tag, sizeof(tag), "%s  %+.1fdB", hzText, gainDb);
      const float halfW = 86.f;
      const float tagX = std::clamp(selectedBandX, mRECT.L + halfW + 4.f, mRECT.R - halfW - 4.f);
      float tagTop = selectedBandY - 38.f;
      if (tagTop < mRECT.T + 4.f) tagTop = selectedBandY + 14.f;
      const IRECT tagRect(tagX - halfW, tagTop, tagX + halfW, tagTop + 24.f);
      mSelectedTagRect = tagRect;
      mSelectedTagShown = true;
      g.FillRect(IColor(225, 12, 16, 26), tagRect);
      g.DrawText(ScaledText(10.f, COLOR_WHITE), tag, tagRect);
    }
  }

  // NEW THIS ROUND -- paired with OnMouseOver/OnMouseOut/OnMouseDrag's
  // mHoverValid/mHoverX tracking above. A no-op whenever the cursor
  // isn't over the gauge area at all (outside the control, or over the
  // rail/depth-stack background).
  // T5-1: hovering close to the arc shows a tick and the frequency it would place a node at,
  // before any click. Upper half only (the arc is the top of the hemicircle).
  void DrawArcHoverTick(IGraphics& g, const IRECT& mainArea)
  {
    using namespace iplug::igraphics;
    if (!mHoverValid) return;
    const ArcGeom arc = GetArcGeom(mainArea);
    const float dx = mHoverX - arc.cx;
    const float dy = mHoverY - arc.cy;
    if (dy > 0.f || std::abs(std::sqrt(dx * dx + dy * dy) - arc.r) > 12.f) return;
    const double hz = HzFromArcPoint(mainArea, mHoverX, mHoverY);
    float px = 0.f, py = 0.f, tx = 0.f, ty = 0.f;
    ArcPointForHzAtRadius(mainArea, hz, arc.r, px, py);
    ArcPointForHzAtRadius(mainArea, hz, arc.r + 16.f, tx, ty);
    g.DrawLine(IColor(170, 220, 230, 255), px, py, tx, ty, nullptr, 1.f);
    char buf[32];
    FormatHzReadout(hz, buf, sizeof(buf));
    g.DrawText(ScaledText(10.f, IColor(235, 205, 220, 245)), buf, IRECT(tx - 40.f, ty - 16.f, tx + 40.f, ty - 3.f));
  }

  void DrawCrosshair(IGraphics& g, const IRECT& area)
  {
    using namespace iplug::igraphics;
    if (!mHoverValid) return;
    const IRECT gaugeArea = GaugeArea(area);
    if (mHoverX < gaugeArea.L || mHoverX > gaugeArea.R) return;

    // FIXED THIS ROUND: the crosshair now projects the raw cursor
    // position onto the SAME hemicircle arc the band dots sit on
    // (HzFromArcPoint/ArcPointForHz, below) instead of drawing a flat
    // vertical guide at a fixed line -- keeps the hover readout visually
    // consistent with where a click would actually land a band now.
    const double hz = HzFromArcPoint(area, mHoverX, mHoverY);
    float px = 0.f, py = 0.f;
    ArcPointForHz(area, hz, px, py);

    const ArcGeom arc = GetArcGeom(area);
    g.FillCircle(IColor(130, 255, 255, 255), px, py, 3.f);
    // Radial guide from the arc's own center out to the hover point --
    // dashed via small FillCircle dots along the radius rather than a
    // new, unverified DrawLine call (FillCircle/FillRect are the only
    // two draw primitives already proven to compile everywhere in this
    // file).
    constexpr int kGuideDots = 10;
    for (int i = 1; i <= kGuideDots; ++i)
    {
      const float f = static_cast<float>(i) / static_cast<float>(kGuideDots);
      g.FillCircle(IColor(45, 255, 255, 255), arc.cx + (px - arc.cx) * f, arc.cy + (py - arc.cy) * f, 1.1f);
    }

    // Hz AND the curve's dB at that frequency -- this is where the dB reference
    // lives now that the rings themselves are unlabelled.
    const double curveDb = ResponseDbAt(mLastResponseBands, HzToT(hz));
    char buf[48];
    char hzText[24];
    FormatHzReadout(hz, hzText, sizeof(hzText));
    std::snprintf(buf, sizeof(buf), "%s   %+.1f dB", hzText, curveDb);
    const float labelHalfW = 92.f;
    const float labelX = std::clamp(px, gaugeArea.L + labelHalfW, gaugeArea.R - labelHalfW);
    g.DrawText(ScaledText(11.f, COLOR_WHITE), buf, IRECT(labelX - labelHalfW, gaugeArea.T - 22.f, labelX + labelHalfW, gaugeArea.T));
  }

  void DrawRail(IGraphics& g, const IRECT& railArea, const std::vector<CircleInfo>& circles)
  {
    using namespace iplug::igraphics;
    constexpr int kMaxVisibleRows = 8;
    const float rowH = railArea.H() / static_cast<float>(kMaxVisibleRows);

    // Center the SELECTED circle's row in the middle of the visible
    // window, same framing as the mockups -- scroll offset is just
    // "selected row sits at visible slot kMaxVisibleRows/2", PLUS
    // mRailScrollOffset (NEW this round -- see OnMouseWheel below) for
    // browsing past the 8-row window without changing the selection.
        int selectedIdx = 0;
    for (size_t i = 0; i < circles.size(); ++i) if (circles[i].circleId == mSelectedCircle) selectedIdx = static_cast<int>(i);
    const int firstVisible = ClampRailScrollOffset(selectedIdx, static_cast<int>(circles.size())) + selectedIdx - kMaxVisibleRows / 2;

    for (int row = 0; row < kMaxVisibleRows; ++row)
    {
      const int idx = firstVisible + row;
      if (idx < 0 || idx >= static_cast<int>(circles.size())) continue;
      const IRECT rowRect(railArea.L, railArea.T + row * rowH, railArea.R, railArea.T + (row + 1) * rowH);
      const bool isCenter = (row == kMaxVisibleRows / 2);
      // Vertical fade toward the top/bottom of the 8 visible rows --
      // approximates the mockups' CSS mask-image fade since IGraphics
      // has no direct equivalent; distance-from-center drives alpha.
      const float distFromCenter = std::abs(row - kMaxVisibleRows / 2) / static_cast<float>(kMaxVisibleRows / 2);
      // NEW THIS ROUND -- a circle whose every active band is muted now
      // dims its own rail row too, same "still here, just not doing
      // anything right now" visual language the per-band dots got two
      // rounds ago -- before this, a fully-muted circle looked identical
      // to a fully-active one on the rail.
      bool allMuted = true;
      for (int b = 0; b < kMaxBands; ++b)
      {
        if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
        if (static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value()) != circles[idx].circleId) continue;
        if (GetDelegate()->GetParam(BandParamIdx(b, kBandMute))->Value() <= 0.5) { allMuted = false; break; }
      }
      const float muteDim = allMuted ? 0.45f : 1.0f;
      // Floor raised from 0.15: off-centre rail rows were effectively unreadable
      // against the near-black background. Pro-Q keeps every row legible and only
      // uses brightness for emphasis, not for hiding information.
      const float alpha = (isCenter ? 1.0f : std::max(0.5f, 1.0f - distFromCenter * 0.5f)) * muteDim;
      // T2-5: each rail row takes its circle's own hue (CircleHueColor, from its avg frequency).
      const IColor hue = CircleColorFor(circles[idx].circleId, false);
      const IColor rowColor(static_cast<int>(alpha * 255), 200, 212, 228);
      if (isCenter)
      {
        // Glowing active row: stacked translucent layers, widest/faintest outside.
        for (int gl = 4; gl >= 1; --gl)
          g.FillRect(IColor(14 + (4 - gl) * 8, 122, 92, 214), rowRect.GetPadded(static_cast<float>(gl) * 2.f)); // violet active glow (Concept 02)
        g.FillRect(IColor(230, hue.R, hue.G, hue.B), IRECT(rowRect.L, rowRect.T, rowRect.L + 2.f, rowRect.B));
      }
      else
      {
        // NEW -- cosmetic hover brighten, same "this is clickable" signal
        // every other interactive bit in this file now gives.
        const bool rowHovered = mHoverValid && rowRect.Contains(mHoverX, mHoverY);
        g.FillRect(IColor(static_cast<int>(alpha * (rowHovered ? 55.f : 20.f)), 150, 170, 200), rowRect);
      }
      const float lineY = rowRect.MH();
      const float lineL = rowRect.L + 8.f;
      const float lineR = rowRect.R - 8.f;
      g.FillRect(IColor(static_cast<int>(alpha * 70), hue.R, hue.G, hue.B), IRECT(lineL, lineY - 2.f, lineR, lineY + 2.f));
      char rowLabel[32];
      // shortened from "C1  2 bands": at 1.5x the long form no longer fits a rail row
      std::snprintf(rowLabel, sizeof(rowLabel), "C%d  %db", circles[idx].circleId + 1, circles[idx].bandCount);
      g.DrawText(ScaledText(12.f, rowColor), rowLabel, IRECT(rowRect.L + 10.f, rowRect.T + 5.f, rowRect.R - 18.f, rowRect.B - 5.f));
      if (isCenter)
        g.DrawText(ScaledText(11.f, COLOR_WHITE), ">", IRECT(rowRect.R - 12.f, rowRect.T, rowRect.R, rowRect.B));

      // NEW THIS ROUND -- hovering a rail row shows its own avg Hz +
      // band count as a small tooltip, same idea as the per-band hover
      // tooltip on the gauge, just for a whole circle instead of one
      // band. Reuses the same mHoverValid/mHoverX/mHoverY state
      // OnMouseOver/OnMouseDrag already track regardless of where on
      // the control the cursor actually is.
      if (mHoverValid && rowRect.Contains(mHoverX, mHoverY))
      {
        char buf[48];
        const double avgHz = circles[idx].avgHz;
        char hzText[24];
        FormatHzReadout(avgHz, hzText, sizeof(hzText));
        std::snprintf(buf, sizeof(buf), "%s avg, %d band%s", hzText, circles[idx].bandCount, circles[idx].bandCount == 1 ? "" : "s");
        const IRECT tipRect(railArea.L - 150.f, rowRect.T, railArea.L, rowRect.B);
        g.FillRect(IColor(200, 24, 24, 28), tipRect);
        g.DrawText(ScaledText(10.f, COLOR_WHITE), buf, tipRect.GetPadded(-4.f));
      }
    }
    // T3-4: the bottom line describes what the pointer can do right now, instead of one fixed hint.
    const bool overRail = mHoverValid && railArea.Contains(mHoverX, mHoverY);
    const char* hint = overRail ? "click circle to select  |  wheel: scroll list"
                     : (mSelectedBand >= 0 ? "drag node  |  click tag to type  |  shift+wheel: move"
                                           : "click empty graph: new band  |  shift+wheel: move");
    g.DrawText(ScaledText(8.f, IColor(150, 160, 175, 180)), hint,
               IRECT(mRECT.R - 420.f, mRECT.B - 18.f, mRECT.R - 12.f, mRECT.B - 4.f));
  }

  int RailRowAt(const IRECT& railArea, const std::vector<CircleInfo>& circles, float y)
  {
    constexpr int kMaxVisibleRows = 8;
    const float rowH = railArea.H() / static_cast<float>(kMaxVisibleRows);
    const int row = static_cast<int>((y - railArea.T) / rowH);
    int selectedIdx = 0;
    for (size_t i = 0; i < circles.size(); ++i) if (circles[i].circleId == mSelectedCircle) selectedIdx = static_cast<int>(i);
    const int firstVisible = ClampRailScrollOffset(selectedIdx, static_cast<int>(circles.size())) + selectedIdx - kMaxVisibleRows / 2;
    const int idx = firstVisible + row;
    if (idx < 0 || idx >= static_cast<int>(circles.size())) return -1;
    return circles[idx].circleId;
  }

  // NEW THIS ROUND -- the practical slice of "rail inertia-drag (click-
  // to-select only, no momentum scroll yet)" from the original deferred
  // list: real scrolling through the rail beyond its fixed 8-row
  // centered window, via mouse wheel. NOT physics-based momentum/inertia
  // (that needs a decaying-velocity animation loop running after
  // release, which needs a timer API this project hasn't used/verified
  // anywhere yet) -- this is the direct, immediate-response version:
  // scroll the wheel while over the rail, the visible window shifts by
  // one row per wheel step, independent of which circle is selected.
  // Clamped so you can never scroll the window past having nothing left
  // to show on either end. Resets to 0 (selected row re-centers) the
  // moment a different circle gets selected -- see OnMouseDown's rail
  // branch below.
  int ClampRailScrollOffset(int selectedIdx, int circleCount) const
  {
    constexpr int kMaxVisibleRows = 8;
    if (circleCount <= kMaxVisibleRows) return 0; // everything already fits, nothing to scroll
    const int minOffset = -(kMaxVisibleRows / 2) - selectedIdx; // firstVisible can't go below 0
    const int maxOffset = (circleCount - 1) - (kMaxVisibleRows / 2) - selectedIdx; // last row can't go past circleCount-1
    return std::clamp(mRailScrollOffset, minOffset, maxOffset);
  }

  // NOTE on OnMouseWheel: standard iPlug2 IControl virtual (float x,
  // float y, const IMouseMod&, float delta) -- same can't-verify-
  // against-your-exact-headers caveat as every other UI/SDK-signature
  // note in this project; paste the error if the exact parameter order/
  // types don't match.
  void OnMouseWheel(float x, float y, const IMouseMod& mod, float delta) override
  {
    const IRECT railArea = mRECT.GetFromRight(kRailW);
    if (railArea.Contains(x, y))
    {
      if (mod.S)
      {
        const std::vector<CircleInfo> circles = GatherCircles();
        int selectedPos = -1;
        for (size_t i = 0; i < circles.size(); ++i)
          if (circles[i].circleId == mSelectedCircle) selectedPos = static_cast<int>(i);
        const int targetPos = selectedPos + ((delta > 0.f) ? -1 : 1);
        if (selectedPos >= 0 && targetPos >= 0 && targetPos < static_cast<int>(circles.size()))
        {
          PushUndoSnapshot();
          SwapCircleOrder(circles[selectedPos].circleId, circles[targetPos].circleId);
          SetDirty(false);
        }
        return;
      }
      mRailScrollOffset += (delta > 0.f) ? -1 : 1;
      SetDirty(false);
      return;
    }

    // NEW THIS ROUND -- wheel-nudge the SELECTED band's cutoff while
    // hovering anywhere over the gauge, a quick way to make a small
    // adjustment without starting a drag. Steps in normalized-position
    // space (HzToT/TToHz), not raw Hz, so one wheel notch is the same
    // perceived distance everywhere along the axis (a notch near 20Hz
    // and a notch near 15kHz feel the same size), rather than a fixed
    // Hz amount that would be a huge perceptual jump at the low end and
    // imperceptible at the high end. Deliberately NOT snapshotted for
    // undo -- same reasoning as a knob's own incremental nudge elsewhere
    // in this plugin never getting captured by this control's undo
    // stack, a flood of undo entries for "scrolled the wheel 5 times"
    // would make Undo far less useful for the gestures that actually
    // need it (creates/deletes/retunes).
    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - kRailW, mRECT.B);
    const IRECT gaugeArea = GaugeArea(mainArea);
    if (mSelectedBand < 0 || !gaugeArea.Contains(x, y)) return;
    if (GetDelegate()->GetParam(BandParamIdx(mSelectedBand, kBandActive))->Value() <= 0.5) return;
    constexpr double kWheelStepT = 0.01; // ~1% of the full 20Hz..20kHz sweep per notch
    const double currentHz = GetDelegate()->GetParam(BandParamIdx(mSelectedBand, kBandCutoff))->Value();
    const double newHz = TToHz(std::clamp(HzToT(currentHz) + (delta > 0.f ? kWheelStepT : -kWheelStepT), 0.0, 1.0));
    PushParamValue(BandParamIdx(mSelectedBand, kBandCutoff), std::clamp(newHz, 20.0, 20000.0), 20.0, 20000.0);
    SetDirty(false);
  }

  // NEW THIS ROUND -- the type-picker card's own bounds, anchored at
  // mPendingPickX/Y (the tap point) but clamped fully inside mRECT so it
  // can never draw partly off-window near an edge/corner.
  IRECT GetPickerCardRect() const
  {
    int pickerCount = 0;
    PickerItems(pickerCount);
    const float h = kPickerRowH * pickerCount;
    const float l = std::clamp(mPendingPickX, mRECT.L, mRECT.R - kPickerW);
    const float t = std::clamp(mPendingPickY, mRECT.T, mRECT.B - h);
    return IRECT(l, t, l + kPickerW, t + h);
  }

  // Normalized (0..1) response shape for picker card i (0=Bell,1=LP,2=HP,3=BP,4=Notch),
  // t = normalized log-frequency. Icon-only approximation, not the DSP curve.
  static double PickerIconResponse(int i, double t)
  {
    switch (i)
    {
      case 0: return std::exp(-std::pow((t - 0.5) / 0.12, 2.0)) * 0.9 + 0.1;
      case 1: return 1.0 / (1.0 + std::exp((t - 0.5) * 14.0));
      case 2: return 1.0 / (1.0 + std::exp((0.5 - t) * 14.0));
      case 3: return std::exp(-std::pow((t - 0.5) / 0.09, 2.0));
      default: return 1.0 - std::exp(-std::pow((t - 0.5) / 0.08, 2.0)) * 0.9;
    }
  }

  // Short label for the node HUD. Only the 5 curated picker types are named;
  // other ordinals show their number until the full InitEnum names are mirrored here.
  static const char* FilterShortName(int ordinal)
  {
    // Mirrors ExoEQ.cpp's InitEnum list for kBandFilterType, in ordinal order.
    // Keep in sync if that list changes (same rule as the constants above).
    static const char* const kNames[] = {
      "Morph (LP-BP-HP)",
      "Lowpass 6",
      "Lowpass 12",
      "Lowpass 24",
      "Lowpass 48",
      "Highpass 6",
      "Highpass 12",
      "Highpass 24",
      "Highpass 48",
      "Bandpass 12",
      "Bandpass 24",
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
    };
    constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
    if (ordinal < 0 || ordinal >= kCount) return "Unknown";
    return kNames[ordinal];
  }

  // MIDI / MTS-ESP status lamps (top-right of the graph). Read live each repaint.
  // MIDI lamp = MIDI input enabled (config.h), NOT 'notes currently held'.
  // MTS lamp = the MTS-ESP enable param, NOT a verified master connection.
  void DrawStatusLamps(IGraphics& g, const IRECT& mainArea)
  {
    using namespace iplug::igraphics;
    const bool mtsOn = GetDelegate()->GetParam(kGlobalMtsEspEnabled)->Value() > 0.5;
    const bool midiOn = true;
    const float y = mainArea.T + 12.f;
    const float x = mainArea.R - 150.f;
    auto lamp = [&](float lx, bool on, IColor onColor, const char* label)
    {
      g.FillCircle(on ? IColor(255, onColor.R, onColor.G, onColor.B) : IColor(255, 40, 44, 54), lx, y, 3.5f);
      if (on) g.FillCircle(IColor(50, onColor.R, onColor.G, onColor.B), lx, y, 7.f);
      g.DrawText(ScaledText(9.f, COLOR_GRAY), label, IRECT(lx + 8.f, y - 7.f, lx + 60.f, y + 7.f));
    };
    lamp(x, midiOn, IColor(255, 150, 200, 255), "MIDI");
    lamp(x + 64.f, mtsOn, IColor(255, 150, 255, 200), "MTS-ESP");
  }

  void DrawTypePicker(IGraphics& g)
  {
    using namespace iplug::igraphics;
    if (mPendingPickBand < 0) return;
    const IRECT cardRect = GetPickerCardRect();
    g.FillRect(IColor(248, 15, 22, 30), cardRect);
    // A plain 1px border built from 4 thin FillRect strips rather than a
    // DrawRect/DrawRoundRect outline call -- FillRect is already proven
    // to compile everywhere else in this file; an outline-specific call
    // isn't, and there's no reason to risk a new unverified primitive
    // just for a border.
    const IColor borderColor(255, 96, 118, 146);
    g.FillRect(borderColor, IRECT(cardRect.L, cardRect.T, cardRect.R, cardRect.T + 1.f));
    g.FillRect(borderColor, IRECT(cardRect.L, cardRect.B - 1.f, cardRect.R, cardRect.B));
    g.FillRect(borderColor, IRECT(cardRect.L, cardRect.T, cardRect.L + 1.f, cardRect.B));
    g.FillRect(borderColor, IRECT(cardRect.R - 1.f, cardRect.T, cardRect.R, cardRect.B));
    int pickerCount = 0;
    const PickerItem* pickerItems = PickerItems(pickerCount);
    for (int i = 0; i < pickerCount; ++i)
    {
      const IRECT rowRect(cardRect.L, cardRect.T + i * kPickerRowH, cardRect.R, cardRect.T + (i + 1) * kPickerRowH);
      if (i == 0)
        g.FillRect(IColor(255, 38, 64, 92), rowRect.GetPadded(1.f));
      // Waveform icon: sampled response shape, drawn as a dot trace (FillCircle only).
      const IRECT iconRect(rowRect.L + 8.f, rowRect.T + 4.f, rowRect.L + 46.f, rowRect.B - 4.f);
      for (int x = 0; x <= 23; ++x)
      {
        const double t = x / 23.0;
        const double m = PickerIconResponse(i, t);
        g.FillCircle(BlueGray(230), iconRect.L + static_cast<float>(t) * iconRect.W(), iconRect.B - static_cast<float>(m) * iconRect.H(), 1.1f);
      }
      g.DrawText(ScaledText(15.f, COLOR_WHITE), pickerItems[i].label, IRECT(rowRect.L + 54.f, rowRect.T, rowRect.R - 8.f, rowRect.B));
    }
  }

  // The selected circle's interaction area is inset from the graph edges.
  // The actual arc geometry is centered independently so the stack remains
  // visible as a complete, selectable object rather than a clipped curve.
  IRECT GaugeArea(const IRECT& mainArea) const
  {
    // Keep the whole centered arc inside the interaction region. The former
    // lower-third inset made the upper half of the circle visually present
    // but impossible to click, which is especially confusing when selecting
    // a newly created band.
    const float padV = mainArea.H() * 0.06f;
    const float padH = mainArea.W() * 0.05f;
    return IRECT(mainArea.L + padH, mainArea.T + padV, mainArea.R - padH, mainArea.B - padV);
  }

  // NOT const: GetDelegate() isn't const-qualified in this iPlug2 version
  // (same note BandGraphControl.h's own FindNodeNear already flagged).
  // FIXED THIS ROUND: takes `mainArea` (not `gaugeArea`) and hit-tests
  // against each band's actual ArcPointForHz position, matching where
  // DrawSelectedGauge now actually draws the dot (on the hemicircle),
  // not a flat line a few px above the gauge's bottom edge.
  int FindBandNear(const IRECT& mainArea, float x, float y)
  {
    constexpr float kHitRadiusPx = 22.f;
    int best = -1;
    float bestDist = kHitRadiusPx;
    const std::vector<CircleInfo> circles = GatherCircles();
    int selectedPos = 0;
    for (size_t i = 0; i < circles.size(); ++i)
      if (circles[i].circleId == mSelectedCircle) selectedPos = static_cast<int>(i);
    for (int b = 0; b < kMaxBands; ++b)
    {
      const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
      if (!active) continue;
      const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value());
      int circlePos = selectedPos;
      for (size_t i = 0; i < circles.size(); ++i)
        if (circles[i].circleId == circleId) circlePos = static_cast<int>(i);
      const float radius = GetArcGeom(mainArea).r * std::max(0.55f, 1.0f - std::abs(circlePos - selectedPos) * 0.08f);
      const double cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
      float bx = 0.f, by = 0.f;
      ArcPointForHzAtRadius(mainArea, cutoffHz, radius, bx, by);
      const float dist = std::sqrt((x - bx) * (x - bx) + (y - by) * (y - by));
      if (dist < bestDist) { bestDist = dist; best = b; }
    }
    return best;
  }

  // NEW -- "two points" per SINGLE band, not between different bands.
  // A wide (non-narrow-focused) filter type's resonance/width is its own
  // handle on the arc: a second point sitting on a dotted line from the
  // circle's own center out to the band's dot, at a radius that reflects
  // the CURRENT resonance (near the center = low resonance/wide slope,
  // out near the dot itself = high resonance/narrow). Bell/notch-like
  // (narrow-focused) types don't get one -- their whole character is
  // already at that one point, there's nothing a second handle would add.
  // DrawSelectedGauge draws the dotted line + handle; this is the
  // matching hit-test, checked in OnMouseDown only after the band's own
  // dot misses (the dot is still the primary, larger target).
  int FindResonanceHandleNear(const IRECT& mainArea, float x, float y)
  {
    constexpr float kHitRadiusPx = 14.f;
    int best = -1;
    float bestDist = kHitRadiusPx;
    const std::vector<CircleInfo> circles = GatherCircles();
    int selectedPos = 0;
    for (size_t i = 0; i < circles.size(); ++i)
      if (circles[i].circleId == mSelectedCircle) selectedPos = static_cast<int>(i);
    for (int b = 0; b < kMaxBands; ++b)
    {
      if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5) continue;
      const int ordinal = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandFilterType))->Value());
      const double morph = GetDelegate()->GetParam(BandParamIdx(b, kBandMorph))->Value();
      if (IsNarrowFocusType(ordinal, morph)) continue;
      const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value());
      int circlePos = selectedPos;
      for (size_t i = 0; i < circles.size(); ++i)
        if (circles[i].circleId == circleId) circlePos = static_cast<int>(i);
      const float ringRadius = GetArcGeom(mainArea).r * std::max(0.55f, 1.0f - std::abs(circlePos - selectedPos) * 0.08f);
      const double cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
      const double resonance = GetDelegate()->GetParam(BandParamIdx(b, kBandResonance))->Value();
      const float handleRadius = ResonanceHandleRadius(ringRadius, resonance);
      float hx = 0.f, hy = 0.f;
      ArcPointForHzAtRadius(mainArea, cutoffHz, handleRadius, hx, hy);
      const float dist = std::sqrt((x - hx) * (x - hx) + (y - hy) * (y - hy));
      if (dist < bestDist) { bestDist = dist; best = b; }
    }
    return best;
  }

  // Classifies a filter type as "narrow-focused" (bell/bandpass/notch/
  // formant/resonator -- character lives at one point) vs "wide" (low/
  // high-pass and the rest -- character spans a region, so the resonance
  // handle below is meaningful). Matched by substring against
  // FilterShortName so it can't silently drift out of sync with that
  // list. Ordinal 0 (Morph LP-BP-HP) is special-cased on the morph value
  // itself since that ONE ordinal can BE a bell-like bandpass (mid-morph)
  // or a plain low/high-pass (morph near either edge).
  static bool IsNarrowFocusType(int ordinal, double morph)
  {
    const char* name = FilterShortName(ordinal);
    auto has = [&](const char* needle) { return std::strstr(name, needle) != nullptr; };
    if (has("Bandpass") || has("Notch") || has("Formant") || has("Resonator")) return true;
    if (ordinal == 0) return morph > 30.0 && morph < 70.0;
    return false;
  }

  // Resonance (0.05..20, same range PushParamValue elsewhere in this file
  // already clamps it to) <-> the handle's 0..1 position along its
  // center-to-dot line, log-mapped so the handle's motion feels even
  // across the whole range instead of being squashed near one end.
  static double ResonanceToUnit(double resonance)
  {
    constexpr double kMin = 0.05, kMax = 20.0;
    const double t = (std::log(std::clamp(resonance, kMin, kMax)) - std::log(kMin)) / (std::log(kMax) - std::log(kMin));
    return std::clamp(t, 0.0, 1.0);
  }
  static double UnitToResonance(double t)
  {
    constexpr double kMin = 0.05, kMax = 20.0;
    return std::exp(std::log(kMin) + std::clamp(t, 0.0, 1.0) * (std::log(kMax) - std::log(kMin)));
  }
  // Keeps the handle a fixed minimum fraction out from the center even at
  // the lowest resonance, so it's never a dead pixel sitting exactly on
  // the center point -- always a real, separately-grabbable target. The
  // outer cap (NEW -- was 1.0, i.e. could land exactly ON the node at
  // max resonance) keeps a small gap at the other end too, so the two
  // points never fully overlap and become a single, ambiguous target.
  static constexpr float kResHandleInnerFrac = 0.22f;
  static constexpr float kResHandleOuterFrac = 0.90f;
  static float ResonanceHandleRadius(float ringRadius, double resonance)
  {
    const float t = static_cast<float>(ResonanceToUnit(resonance));
    return ringRadius * (kResHandleInnerFrac + (kResHandleOuterFrac - kResHandleInnerFrac) * t);
  }

  // NOT const, same GetDelegate() reason as FindBandNear above.
  int FirstInactiveBandSlot()
  {
    for (int b = 0; b < kMaxBands; ++b)
      if (GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() <= 0.5)
        return b;
    return -1;
  }

  void SelectBand(int bandIndex)
  {
    mSelectedBand = bandIndex;
    if (bandIndex >= 0)
    {
      // Selecting a dot also selects its circle. This makes every visible
      // band a direct target, instead of requiring the user to select the
      // rail row first and then hunt for the band again.
      mSelectedCircle = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(bandIndex, kBandCircleIndex))->Value());
      mRailScrollOffset = 0;
    }
    if (mOnBandSelected) mOnBandSelected(bandIndex);
  }

  // NEW THIS ROUND -- the automation-ghost trail's own sample history.
  // Shifts band `b`'s small ring of recent cutoff values and inserts
  // `cutoffHz` at the front. Deliberately NOT gated on "did the value
  // actually change" -- an unchanging value just means every sample in
  // the ring is identical, which draws the trail dots exactly on top of
  // the live dot (see the draw loop above), so there's no separate
  // "nothing moved" branch needed here.
  void PushGhostHistory(int b, double cutoffHz)
  {
    for (int i = kGhostTrailLen - 1; i > 0; --i)
      mGhostHistory[b][i] = mGhostHistory[b][i - 1];
    mGhostHistory[b][0] = cutoffHz;
    if (mGhostFilled[b] < kGhostTrailLen) ++mGhostFilled[b];
  }

  // REMOVED THIS ROUND -- your build error: "'CircleViewControl::
  // OnParamChange': method with override specifier 'override' did not
  // override any base class methods". That means this exact iPlug2
  // version's IControl either doesn't declare OnParamChange as a virtual
  // at all, or declares it with a different signature than the plain
  // `(int paramIdx)` I guessed -- either way, guessing again blind risks
  // the same error with a different wrong signature. Removed outright
  // rather than leaving a second guess in; the ghost trail (DrawSelected
  // Gauge/PushGhostHistory above) still updates on every repaint this
  // control gets from any OTHER cause (your own mouse interaction,
  // whatever drives this build's normal IGraphics render loop), it just
  // isn't explicitly forced to repaint on a pure host-automation-only
  // param change anymore. If you want that specific behavior back, tell
  // me the real method name/signature your IControl.h declares for "a
  // param changed, possibly from automation" and I'll wire it in exactly
  // right instead of guessing.

  // Mel/perceptual axis (the confirmed default calibration -- Concept 49):
  // mel(f) = 2595 * log10(1 + f/700), normalized across 20Hz..20kHz. Left
  // = 20Hz, right = 20kHz, matching the gauge's left-to-right convention.
  static double MelOf(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }

  // NEW THIS ROUND: plain log-frequency axis, the "switchable frequency
  // calibration beyond Mel/perceptual" deferred item -- the standard
  // axis shape most EQ plugins use (equal pixel width per octave, no
  // extra high-end compression the way Mel adds). log2(f/20), normalized
  // across the same 20Hz..20kHz range as MelOf above so the two stay
  // directly comparable/swappable.
  static double LogOf(double hz) { return std::log2(hz / 20.0); }

  // NOT const: GetDelegate() isn't const-qualified in this iPlug2
  // version (same recurring note as FindBandNear/FirstInactiveBandSlot
  // above) -- reads the new kGlobalFreqAxisMel toggle (ExoEQ.cpp's new
  // header-row IVToggleControl) so HzToT/TToHz below pick the axis the
  // user currently has selected.
  bool UseMelAxis() { return GetDelegate()->GetParam(kGlobalFreqAxisMel)->Value() > 0.5; }

  // Hz -> normalized position (0..1 across 20Hz..20kHz), Mel or log axis
  // per UseMelAxis() -- factored out of the old HzToX/XToHz (removed
  // this round) so the arc-point mapping below shares the exact same
  // axis math rather than duplicating it.
  double HzToT(double hz)
  {
    const double hzClamped = std::clamp(hz, 20.0, 20000.0);
    if (UseMelAxis())
    {
      const double melMin = MelOf(20.0), melMax = MelOf(20000.0);
      return std::clamp((MelOf(hzClamped) - melMin) / (melMax - melMin), 0.0, 1.0);
    }
    const double logMin = LogOf(20.0), logMax = LogOf(20000.0);
    return std::clamp((LogOf(hzClamped) - logMin) / (logMax - logMin), 0.0, 1.0);
  }

  double TToHz(double t)
  {
    t = std::clamp(t, 0.0, 1.0);
    if (UseMelAxis())
    {
      const double melMin = MelOf(20.0), melMax = MelOf(20000.0);
      const double mel = melMin + t * (melMax - melMin);
      return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
    }
    const double logMin = LogOf(20.0), logMax = LogOf(20000.0);
    const double logHz = logMin + t * (logMax - logMin);
    return 20.0 * std::pow(2.0, logHz);
  }

  struct ArcGeom { float cx, cy, r; };

  // Matches DrawDepthStack's rank-0 (selected circle, scale == 1.0)
  // geometry exactly -- same cx/cy/r formula, kept in one place so every
  // function that needs "where the selected circle's own arc is" agrees
  // with what's actually drawn on screen.
  ArcGeom GetArcGeom(const IRECT& mainArea) const
  {
    // Radar base near the bottom of the graph (mockup), radius fitted to
    // whichever dimension runs out first. Top of the arc: cy - r >= mainArea.T.
    const float r = std::min(mainArea.W() * 0.42f, mainArea.H() * 0.82f);
    const float cy = mainArea.T + mainArea.H() * 0.92f;
    return { mainArea.MW(), cy, r };
  }

  // Hz -> a point ON the selected circle's own hemicircle arc. THIS IS
  // THE FIX for "the line for the bands... should follow the hemicircle's
  // line instead": t=0 (20Hz) sits at the arc's left foot (angle 180deg,
  // same height as the arc's own center), t=1 (20kHz) at its right foot
  // (angle 360deg), sweeping up through the top in between -- the exact
  // same 180..360 degree range DrawDepthStack's own g.DrawArc(...) call
  // already draws, so a dot at any Hz lands exactly on the visible curve.
  void ArcPointForHz(const IRECT& mainArea, double hz, float& xOut, float& yOut)
  {
    const ArcGeom arc = GetArcGeom(mainArea);
    ArcPointForHzAtRadius(mainArea, hz, arc.r, xOut, yOut);
  }

  void ArcPointForHzAtRadius(const IRECT& mainArea, double hz, float radius, float& xOut, float& yOut)
  {
    const ArcGeom arc = GetArcGeom(mainArea);
    const double t = HzToT(hz);
    const double angleRad = (180.0 + t * 180.0) * kPi / 180.0;
    xOut = arc.cx + radius * static_cast<float>(std::cos(angleRad));
    yOut = arc.cy + radius * static_cast<float>(std::sin(angleRad));
  }

  // Inverse of ArcPointForHz -- maps a click/drag/hover point back to Hz
  // by its ANGLE around the arc's own center (not by X alone), so
  // retuning/creating/hit-testing a band near the curve tracks it
  // faithfully even where it's steep (near the two feet), rather than
  // assuming the old flat line's 1:1 X<->Hz relationship. Any point
  // below the arc's own baseline (dy > 0 -- clicked below the curve
  // entirely) is treated as if it were exactly at baseline height first,
  // which keeps the result always within the visible 180..360deg sweep
  // instead of wrapping into the lower half of the circle.
  double HzFromArcPoint(const IRECT& mainArea, float x, float y)
  {
    const ArcGeom arc = GetArcGeom(mainArea);
    const double dx = static_cast<double>(x - arc.cx);
    const double dy = std::min(0.0, static_cast<double>(y - arc.cy));
    double angleDeg = std::atan2(dy, dx) * 180.0 / kPi; // atan2 range (-180,180]; dy<=0 keeps this in [-180,180]
    if (angleDeg <= 0.0) angleDeg += 360.0; // fold into our 180..360 sweep (exact 180 edge stays 180, not wrapped)
    angleDeg = std::clamp(angleDeg, 180.0, 360.0);
    const double t = (angleDeg - 180.0) / 180.0;
    return TToHz(t);
  }

  // Same normalized-value convention as BandGraphControl::PushParamValue
  // -- see that function's own comment on the one line most likely to
  // need an SDK-signature fix if this doesn't compile as-is.
  // FIXED THIS ROUND -- the actual cause of "click at the top gives ~527 Hz,
  // should be ~3200": this used to convert real->normalized LINEARLY, but
  // the cutoff param is declared with IParam::ShapePowCurve(2.) (ExoEQ.cpp,
  // InitDouble for kBandCutoff). A linear normalized value fed into a
  // power-curve param is squared on read, so 3150 Hz (norm 0.157) landed at
  // 20 + 19980*0.157^2 = ~514 Hz. The crosshair was right because it reads
  // the mapping directly; the write path ignored the param's own curve.
  // Node drags and resonance drags were hit by the same bug -- the value
  // written didn't match the value the cursor implied. Now the parameter
  // itself converts (ToNormalized), so whatever shape it declares is honored.
  // Typed node entry, one field at a time. Frequency accepts "2.5k" or "440" (k = x1000,
  // clamped to 20 Hz-20 kHz). Gain accepts "-3.5" (dB, clamped to +-24). Unit text is ignored.
  void OnTextEntryCompletion(const char* str, int /*valIdx*/) override
  {
    if (!str || mNodeEntryBand < 0) return;
    char* end = nullptr;
    const double v = std::strtod(str, &end);
    if (end == str) { mNodeEntryBand = -1; return; }   // unreadable: keep the node as it is

    PushUndoSnapshot();
    if (mNodeEntryField == 0)
    {
      const bool kilo = (*end == 'k' || *end == 'K');
      const double hz = kilo ? v * 1000.0 : v;
      PushParamValue(BandParamIdx(mNodeEntryBand, kBandCutoff), std::clamp(hz, 20.0, 20000.0), 20.0, 20000.0);
    }
    else
    {
      PushParamValue(BandParamIdx(mNodeEntryBand, kBandGainDb), std::clamp(v, -24.0, 24.0), -24.0, 24.0);
    }
    mNodeEntryBand = -1;
    SetDirty(false);
  }

  void PushParamValue(int paramIdx, double realValue, double rangeMin, double rangeMax)
  {
    const double clamped = std::clamp(realValue, rangeMin, rangeMax);
    double normalized = (clamped - rangeMin) / (rangeMax - rangeMin);
    if (auto* p = GetDelegate()->GetParam(paramIdx))
      normalized = p->ToNormalized(clamped);
    GetDelegate()->SendParameterValueFromUI(paramIdx, normalized);
  }

  double mSampleRate;
  int mSelectedCircle = 0;
  int mSelectedBand = -1;
  // Click-to-type on the selected node's tag (see OnMouseDown / OnTextEntryCompletion).
  IRECT mSelectedTagRect;          // where the tag was last drawn; empty when not shown
  bool mSelectedTagShown = false;
  int mNodeEntryBand = -1;         // band whose tag opened the current text entry
  int mNodeEntryField = 0;         // 0 = frequency (left half of the tag), 1 = gain (right half)
  float mVisualSelectedPos = 0.f;
  iplug::igraphics::ILayerPtr mGridLayer; // T4-5 cached grid + labels
  float mGhostFade = 1.f; // 0..1, set each Draw from the circle count
  float mLoadProgress = 0.f; // T4-1: 0..1, drives the load sweep
  bool mDraggingNewBand = false;
  bool mDragIsRange = false;
  float mDragStartX = 0.f;
  float mDragStartY = 0.f;
  // NEW THIS ROUND -- existing-band retune-by-drag state, separate from
  // mDraggingNewBand/mDragStartX/Y above (those are specifically for the
  // tap-vs-range NEW band creation gesture, which still needs to wait for
  // release before deciding anything; retuning an existing band updates
  // live on every drag event instead, so it doesn't reuse that state).
  bool mDraggingExistingBand = false;
  bool mDraggingGain = false;           // NEW -- right-drag vertical gain on a node
  double mGainDragStartDb = 0.0;
  float mGainDragStartY = 0.f;
  bool mDraggingResonanceHandle = false; // NEW -- dragging a wide-type band's second (resonance) point
  int mDraggedBand = -1;
  // NEW THIS ROUND -- crosshair/hover-readout state (OnMouseOver/
  // OnMouseOut/OnMouseDrag above, DrawCrosshair below).
  bool mHoverValid = false;
  float mHoverX = 0.f;
  float mHoverY = 0.f; // NEW THIS ROUND -- paired with mHoverX so the crosshair can project onto the arc
  // NEW THIS ROUND -- rail wheel-scroll state (OnMouseWheel/
  // ClampRailScrollOffset above).
  int mRailScrollOffset = 0;
  // NEW THIS ROUND -- type-picker card state (OnMouseUp/DrawTypePicker/
  // GetPickerCardRect/OnMouseDown above). mPendingPickBand < 0 means no
  // card is open; >= 0 is the already-reserved-but-not-yet-activated
  // band slot waiting on a row choice.
  float mPendingPickX = 0.f;
  float mPendingPickY = 0.f;
  int mPendingPickBand = -1;
  // NEW THIS ROUND -- rail drag-to-scroll + momentum state (OnMouseDown/
  // OnMouseDrag/OnMouseUp's new mDraggingRail handling above).
  bool mDraggingRail = false;
  float mRailDragStartY = 0.f;
  float mRailDragTotalMovePx = 0.f;
  float mRailDragLastDeltaY = 0.f;
  // NEW THIS ROUND -- automation-ghost trail history (PushGhostHistory/
  // OnParamChange above, drawn in DrawSelectedGauge). Plain fixed-size
  // arrays rather than std::vector -- this is touched once per band per
  // repaint, not per audio sample, but a fixed-size array still avoids
  // any heap churn on the UI thread for something this small (kMaxBands
  // * kGhostTrailLen doubles, a few KB total).
  static constexpr int kGhostTrailLen = 5;
  std::vector<ResponseBand> mLastResponseBands; // filled by DrawResponseCurve, read by DrawCrosshair in the same frame
  double mGhostHistory[kMaxBands][kGhostTrailLen] = {};
  int mGhostFilled[kMaxBands] = {};
  // NEW THIS ROUND -- undo/redo stacks (PushUndoSnapshot/Undo/Redo above).
  std::vector<std::vector<double>> mUndoStack;
  std::vector<std::vector<double>> mRedoStack;
  // NEW THIS ROUND -- preset-circle tray state (OnMouseOver/OnMouseOut/
  // OnMouseDown/OnMouseDrag/OnMouseUp's new mDraggingPreset handling,
  // DrawPresetTray above).
  bool mTrayHover = false;
  bool mDraggingPreset = false;
  int mDraggedPresetIndex = -1;
  float mPresetDragX = 0.f;
  float mPresetDragY = 0.f;
  // NEW THIS ROUND -- session-only user-saved presets (SaveCurrentCircle
  // AsPreset/TotalPresetCount/GetPresetAt above).
  std::vector<PresetCircleDef> mUserPresets;
  std::function<void(int)> mOnBandSelected;
};

