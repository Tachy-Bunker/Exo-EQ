#pragma once

#include "IControl.h"
#include "ExoEQ.h" // BandParamIdx, kMaxBands, EBandParamOffset, kMaxCircles
#include "DSP/CircleChain.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>
#include <cstdio>

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

  void Draw(IGraphics& g) override
  {
    using namespace iplug::igraphics;
    g.FillRect(IColor(255, 21, 21, 26), mRECT);

    // Split: rail docked on the right (per the rail-right round confirmed
    // earlier), everything else (depth stack + selected-circle gauge) in
    // the remaining area to its left.
    const float railW = 70.f;
    const IRECT railArea = mRECT.GetFromRight(railW);
    // Built directly rather than via a "reduced by" helper (not confident
    // that exact helper name exists in the vendored IRECT -- GetFromRight/
    // GetFromLeft/GetPadded etc. above are the ones already proven to
    // compile elsewhere in this project's controls, so this one spot
    // avoids guessing at an unproven name).
    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - railW, mRECT.B);

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

    DrawDepthStack(g, mainArea, circles);
    DrawScrim(g, mainArea, circles);
    DrawSelectedGauge(g, mainArea, circles);
    DrawRail(g, railArea, circles);
    // NEW THIS ROUND: live crosshair + Hz readout -- another item off the
    // original deferred list. Drawn last so it sits on top of everything
    // else.
    DrawCrosshair(g, mainArea);
    // NEW THIS ROUND: the tap-creates-a-band filter-type popover --
    // drawn last/on top of everything, same as the crosshair above.
    DrawTypePicker(g);
    // NEW THIS ROUND: undo/redo (top-left corner) and the preset-circle
    // tray (left edge, hover to reveal) -- see DrawUndoRedo/
    // DrawPresetTray's own comments below for what each one does.
    DrawUndoRedo(g);
    DrawPresetTray(g);
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
          const IRECT mainAreaForPick(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);
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

    const IRECT railArea = mRECT.GetFromRight(70.f);
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
      return;
    }

    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);
    const IRECT gaugeArea = GaugeArea(mainArea);
    if (!gaugeArea.Contains(x, y)) return;

    // Existing-band hit test first -- clicking an existing band selects
    // it (for the detail panel), same split BandGraphControl already
    // used, AND now starts a RETUNE drag (move its own cutoff by
    // dragging), fixed this round -- previously this just selected the
    // band and returned, with no way to move or delete a band already on
    // the graph at all once created (only the detail panel's own knobs
    // could touch it). Empty space is still what creates a brand new band.
    const int hitBand = FindBandNear(mainArea, x, y);
    if (hitBand >= 0)
    {
      // NEW THIS ROUND: shift-click a band to toggle its Mute instantly,
      // without opening the detail panel or starting a retune drag --
      // `mod.S` is iPlug2's standard IMouseMod shift-key flag (same
      // can't-verify-against-your-exact-headers caveat as every other
      // UI/SDK-signature note in this project; paste the error if this
      // specific field name doesn't compile).
      if (mod.S)
      {
        PushUndoSnapshot();
        const bool currentlyMuted = GetDelegate()->GetParam(BandParamIdx(hitBand, kBandMute))->Value() > 0.5;
        PushParamValue(BandParamIdx(hitBand, kBandMute), currentlyMuted ? 0.0 : 1.0, 0.0, 1.0);
        SelectBand(hitBand);
        mDraggingNewBand = false;
        mDraggingExistingBand = false;
        SetDirty(false);
        return;
      }
      SelectBand(hitBand);
      mDraggingNewBand = false;
      mDraggingExistingBand = true;
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

    mDragStartX = x;
    mDragStartY = y;
    mDraggingNewBand = true;
    mDraggingExistingBand = false;
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
      const IRECT railArea = mRECT.GetFromRight(70.f);
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
      const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);
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

      const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);
      const IRECT railArea = mRECT.GetFromRight(70.f);
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
        const IRECT railArea = mRECT.GetFromRight(70.f);
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

    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);

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

    // A plain TAP (not a drag -- see mDragIsRange's gesture-direction
    // branch above for drags) opens a small filter-type popover card
    // right at the tap point instead of force-creating a fixed type: a
    // short list, click a row to commit, click anywhere else to cancel.
    // Drawn/hit-tested with this
    // file's own existing FillRect/DrawText/Contains primitives rather
    // than IGraphics' native popup-menu API (CreatePopupMenu/
    // IPopupMenu/OnPopupMenuSelection) -- deliberately, since this file
    // has an established pattern of avoiding SDK calls it can't verify
    // against your exact vendored headers, and a native popup menu would
    // be 3 more of those at once. Nothing is created yet -- the actual
    // PushParamValue calls happen in OnMouseDown below once a row (or
    // nothing) is picked.
    mPendingPickX = x;
    mPendingPickY = y;
    mPendingPickBand = freeBand;
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
    const IRECT railArea = mRECT.GetFromRight(70.f);
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

    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);
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
  static constexpr int kNumFilterTypeOptions = 46; // length of that same InitEnum list (counted from ExoEQ.cpp directly) -- update together if it changes

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
  static constexpr float kPickerRowH = 26.f;
  static constexpr float kPickerW = 130.f;

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
        g.DrawText(IText(9.f, IColor(190, 217, 164, 76)), kLetters[i], letterRect);
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
        const IRECT nameRect(rowRect.L, rowRect.T, rowRect.R, rowRect.T + 20.f);
        const IRECT previewRect(rowRect.L, rowRect.T + 20.f, rowRect.R, rowRect.B);
        g.DrawText(IText(11.f, COLOR_WHITE), preset.name, nameRect.GetPadded(-6.f));
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
          if (pb.cutoffHz >= 1000.0)
            std::snprintf(buf, sizeof(buf), "%s @ %.1fkHz", typeName, pb.cutoffHz / 1000.0);
          else
            std::snprintf(buf, sizeof(buf), "%s @ %.0fHz", typeName, pb.cutoffHz);
          g.DrawText(IText(9.f, COLOR_GRAY), buf, previewRect.GetPadded(-6.f));
        }
      }

      // NEW THIS ROUND -- the "+ Save Current" action row, right after
      // every real preset. Not draggable (OnMouseDown's tray hit-test
      // treats this exact row index, == presetCount, as a click-to-save
      // instead of a drag start) -- captures mSelectedCircle's own
      // bands (up to 3) as a new entry at the end of this same list.
      {
        const IRECT saveRowRect(trayRect.L, trayRect.T + presetCount * kPresetRowH, trayRect.R, trayRect.T + (presetCount + 1) * kPresetRowH);
        g.DrawText(IText(11.f, IColor(255, 217, 164, 76)), "+ Save Current", saveRowRect.GetPadded(-6.f));
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
        g.DrawText(IText(11.f, COLOR_WHITE), preset.name, IRECT(mPresetDragX + 8.f, mPresetDragY - 8.f, mPresetDragX + 140.f, mPresetDragY + 8.f));
      }
    }

    // NEW THIS ROUND -- live drop-target feedback while a preset is
    // being dragged: highlights whichever outcome WOULD happen if you
    // released right now (replace/insert/add-new), same three outcomes
    // OnMouseUp's own drop resolution already implements -- this is
    // purely visual, it doesn't change what actually happens on release.
    if (mDraggingPreset)
    {
      const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);
      const IRECT railArea = mRECT.GetFromRight(70.f);
      const ArcGeom arc = GetArcGeom(mainArea);
      constexpr float kDeadCenterRadiusPx = 42.f; // same radius OnMouseUp's own drop resolution uses
      const float distFromCenter = std::sqrt((mPresetDragX - arc.cx) * (mPresetDragX - arc.cx) + (mPresetDragY - arc.cy) * (mPresetDragY - arc.cy));
      if (distFromCenter <= kDeadCenterRadiusPx)
      {
        // REPLACE -- ring around the whole selected-circle arc.
        g.DrawArc(IColor(220, 217, 76, 76), arc.cx, arc.cy, arc.r, 180.f, 360.f, nullptr, 4.f);
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
        g.DrawText(IText(16.f, IColor(220, 217, 164, 76)), "+", IRECT(mPresetDragX - 10.f, mPresetDragY - 10.f, mPresetDragX + 10.f, mPresetDragY + 10.f));
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
    g.FillRect(mUndoStack.empty() ? inactiveBg : activeBg, undoRect);
    g.FillRect(mRedoStack.empty() ? inactiveBg : activeBg, redoRect);
    g.DrawText(IText(11.f, mUndoStack.empty() ? inactiveText : activeText), "UNDO", undoRect);
    g.DrawText(IText(11.f, mRedoStack.empty() ? inactiveText : activeText), "REDO", redoRect);
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
  iplug::igraphics::IColor AccentColor() const { return iplug::igraphics::IColor(255, 217, 164, 76); }

  void DrawDepthStack(IGraphics& g, const IRECT& area, const std::vector<CircleInfo>& circles)
  {
    using namespace iplug::igraphics;
    // Depth rank: distance from the selected circle in the (already
    // signal-flow-ordered) `circles` list -- rank 0 is the selected
    // circle itself (drawn largest, full opacity), rank N dims and
    // shrinks further out, same rolodex falloff as the mockups.
    int selectedPos = 0;
    for (size_t i = 0; i < circles.size(); ++i) if (circles[i].circleId == mSelectedCircle) selectedPos = static_cast<int>(i);

    const float cx = area.MW();
    const float cy = area.B;
    const float baseRadius = std::min(area.W(), area.H()) * 0.42f;

    for (size_t i = 0; i < circles.size(); ++i)
    {
      const int rank = std::abs(static_cast<int>(i) - selectedPos);
      const float scale = std::max(0.55f, 1.0f - rank * 0.08f);
      const float opacity = std::max(0.12f, 1.0f - rank * 0.16f);
      const IColor hue = CircleHueColor(circles[i].avgHz);
      const IColor faded(static_cast<int>(opacity * 255), hue.R, hue.G, hue.B);
      const float r = baseRadius * scale;
      g.DrawArc(faded, cx, cy, r, 180.f, 360.f, nullptr, 2.5f + (rank == 0 ? 1.f : 0.f));
    }
  }

  void DrawScrim(IGraphics& g, const IRECT& area, const std::vector<CircleInfo>& /*circles*/)
  {
    using namespace iplug::igraphics;
    // 80%-opacity black specifically behind the SELECTED circle's own
    // footprint (not the whole background) -- ghosts peeking out past
    // its edges keep their own, lesser depth-dim level from DrawDepthStack.
    const float cx = area.MW();
    const float cy = area.B;
    const float r = std::min(area.W(), area.H()) * 0.42f;
    g.FillRect(IColor(204, 10, 10, 12), IRECT(cx - r, cy - r, cx + r, cy));
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
      g.DrawArc(IColor(140, 90, 90, 100), arc.cx, arc.cy, arc.r, 180.f, 360.f, nullptr, 2.f);
    }

    // NEW THIS ROUND -- which band (if any) the cursor is currently
    // close enough to for a hover tooltip, found during the same loop
    // that draws every dot below rather than a second pass over
    // kMaxBands. Drawn AFTER the loop (see the bottom of this function)
    // so the tooltip box always sits on top of every dot, not just the
    // hovered one's.
    int hoveredBand = -1;
    float hoveredBandX = 0.f, hoveredBandY = 0.f;
    float hoveredBandDist = 12.f; // hover radius -- a little more forgiving than FindBandNear's own 10px click-hit radius, since this is just a readout, not a mutation

    if (sel)
    {
      for (int b = 0; b < kMaxBands; ++b)
      {
        const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
        if (!active) continue;
        const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value());
        if (circleId != mSelectedCircle) continue;

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
          ArcPointForHz(area, mGhostHistory[b][h], gx, gy);
          const float fade = 1.f - static_cast<float>(h) / static_cast<float>(kGhostTrailLen);
          g.FillCircle(IColor(static_cast<int>(fade * 110.f), accent.R, accent.G, accent.B), gx, gy, 2.f + fade * 2.f);
        }

        // FIXED THIS ROUND: dot position is now a point ON the hemicircle
        // arc (ArcPointForHz, below), not a flat x/fixed-y pair.
        float x = 0.f, y = 0.f;
        ArcPointForHz(area, cutoffHz, x, y);
        const bool selected = (b == mSelectedBand);
        // NEW THIS ROUND -- muted bands now read as muted ON THE GRAPH
        // itself (dimmed + smaller), not just in the new hover tooltip --
        // before this, a muted band looked completely identical to an
        // active one until you opened the detail panel or hovered it.
        const bool muted = GetDelegate()->GetParam(BandParamIdx(b, kBandMute))->Value() > 0.5;
        IColor dotColor = selected ? COLOR_WHITE : accent;
        float dotRadius = selected ? 6.f : 4.5f;
        if (muted)
        {
          dotColor = IColor(140, dotColor.R, dotColor.G, dotColor.B); // same hue, lower alpha -- "still here, just not doing anything right now"
          dotRadius *= 0.75f;
        }
        g.FillCircle(dotColor, x, y, dotRadius);

        // NEW THIS ROUND -- hover-tooltip candidacy check, same spot the
        // dot itself is drawn so there's no second coordinate lookup.
        if (mHoverValid)
        {
          const float dist = std::sqrt((mHoverX - x) * (mHoverX - x) + (mHoverY - y) * (mHoverY - y));
          if (dist < hoveredBandDist) { hoveredBandDist = dist; hoveredBand = b; hoveredBandX = x; hoveredBandY = y; }
        }
      }
    }

    g.DrawText(IText(10.f, COLOR_GRAY), "20Hz", IRECT(gaugeArea.L, gaugeArea.B + 2.f, gaugeArea.L + 40.f, gaugeArea.B + 16.f));
    g.DrawText(IText(10.f, COLOR_GRAY), "20kHz", IRECT(gaugeArea.R - 44.f, gaugeArea.B + 2.f, gaugeArea.R, gaugeArea.B + 16.f));

    // NEW THIS ROUND -- Pro-Q-style "hover a node, see its own numbers"
    // tooltip: Hz + Q (resonance) + Mute state, right above whichever
    // dot the cursor is closest to (within hoveredBandDist, set above).
    // Separate from DrawCrosshair's own general "wherever you are on the
    // arc" readout -- this one is about a SPECIFIC existing band, not an
    // arbitrary hover position, so it only appears right at a real dot.
    if (hoveredBand >= 0)
    {
      const double hz = GetDelegate()->GetParam(BandParamIdx(hoveredBand, kBandCutoff))->Value();
      const double q = GetDelegate()->GetParam(BandParamIdx(hoveredBand, kBandResonance))->Value();
      const bool muted = GetDelegate()->GetParam(BandParamIdx(hoveredBand, kBandMute))->Value() > 0.5;
      char buf[48];
      if (hz >= 1000.0)
        std::snprintf(buf, sizeof(buf), "%.2fkHz  Q %.2f%s", hz / 1000.0, q, muted ? "  (muted)" : "");
      else
        std::snprintf(buf, sizeof(buf), "%.0fHz  Q %.2f%s", hz, q, muted ? "  (muted)" : "");
      const float labelHalfW = 60.f;
      const float labelX = std::clamp(hoveredBandX, area.L + labelHalfW, area.R - labelHalfW);
      const IRECT tipRect(labelX - labelHalfW, hoveredBandY - 28.f, labelX + labelHalfW, hoveredBandY - 12.f);
      g.FillRect(IColor(200, 24, 24, 28), tipRect);
      g.DrawText(IText(10.f, COLOR_WHITE), buf, tipRect);
    }
  }

  // NEW THIS ROUND -- paired with OnMouseOver/OnMouseOut/OnMouseDrag's
  // mHoverValid/mHoverX tracking above. A no-op whenever the cursor
  // isn't over the gauge area at all (outside the control, or over the
  // rail/depth-stack background).
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

    char buf[32];
    if (hz >= 1000.0)
      std::snprintf(buf, sizeof(buf), "%.2f kHz", hz / 1000.0);
    else
      std::snprintf(buf, sizeof(buf), "%.0f Hz", hz);
    const float labelHalfW = 40.f;
    const float labelX = std::clamp(px, gaugeArea.L + labelHalfW, gaugeArea.R - labelHalfW);
    g.DrawText(IText(11.f, COLOR_WHITE), buf, IRECT(labelX - labelHalfW, gaugeArea.T - 16.f, labelX + labelHalfW, gaugeArea.T));
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
      const float alpha = (isCenter ? 1.0f : std::max(0.15f, 1.0f - distFromCenter * 0.85f)) * muteDim;
      const IColor hue = CircleHueColor(circles[idx].avgHz);
      const IColor rowColor(static_cast<int>(alpha * 255), hue.R, hue.G, hue.B);
      g.FillRect(IColor(static_cast<int>(alpha * 40), hue.R, hue.G, hue.B), rowRect);
      g.DrawText(IText(11.f, rowColor), std::to_string(circles[idx].bandCount).c_str(), rowRect);
      if (isCenter)
        g.DrawText(IText(11.f, COLOR_WHITE), ">", IRECT(rowRect.R - 12.f, rowRect.T, rowRect.R, rowRect.B));

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
        if (avgHz >= 1000.0)
          std::snprintf(buf, sizeof(buf), "%.2fkHz avg, %d band%s", avgHz / 1000.0, circles[idx].bandCount, circles[idx].bandCount == 1 ? "" : "s");
        else
          std::snprintf(buf, sizeof(buf), "%.0fHz avg, %d band%s", avgHz, circles[idx].bandCount, circles[idx].bandCount == 1 ? "" : "s");
        const IRECT tipRect(railArea.L - 150.f, rowRect.T, railArea.L, rowRect.B);
        g.FillRect(IColor(200, 24, 24, 28), tipRect);
        g.DrawText(IText(10.f, COLOR_WHITE), buf, tipRect.GetPadded(-4.f));
      }
    }
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
  void OnMouseWheel(float x, float y, const IMouseMod& /*mod*/, float delta) override
  {
    const IRECT railArea = mRECT.GetFromRight(70.f);
    if (railArea.Contains(x, y))
    {
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
    const IRECT mainArea(mRECT.L, mRECT.T, mRECT.R - 70.f, mRECT.B);
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

  void DrawTypePicker(IGraphics& g)
  {
    using namespace iplug::igraphics;
    if (mPendingPickBand < 0) return;
    const IRECT cardRect = GetPickerCardRect();
    g.FillRect(IColor(240, 32, 32, 38), cardRect);
    // A plain 1px border built from 4 thin FillRect strips rather than a
    // DrawRect/DrawRoundRect outline call -- FillRect is already proven
    // to compile everywhere else in this file; an outline-specific call
    // isn't, and there's no reason to risk a new unverified primitive
    // just for a border.
    const IColor borderColor(255, 90, 90, 100);
    g.FillRect(borderColor, IRECT(cardRect.L, cardRect.T, cardRect.R, cardRect.T + 1.f));
    g.FillRect(borderColor, IRECT(cardRect.L, cardRect.B - 1.f, cardRect.R, cardRect.B));
    g.FillRect(borderColor, IRECT(cardRect.L, cardRect.T, cardRect.L + 1.f, cardRect.B));
    g.FillRect(borderColor, IRECT(cardRect.R - 1.f, cardRect.T, cardRect.R, cardRect.B));
    int pickerCount = 0;
    const PickerItem* pickerItems = PickerItems(pickerCount);
    for (int i = 0; i < pickerCount; ++i)
    {
      const IRECT rowRect(cardRect.L, cardRect.T + i * kPickerRowH, cardRect.R, cardRect.T + (i + 1) * kPickerRowH);
      g.DrawText(IText(12.f, COLOR_WHITE), pickerItems[i].label, rowRect.GetPadded(-6.f));
    }
  }

  // The selected circle's gauge occupies the lower-center ~70% of the
  // main area, matching the mockups' "semicircle sits low, labels below
  // it" framing -- the depth stack above still shows through behind it.
  IRECT GaugeArea(const IRECT& mainArea) const
  {
    const float padV = mainArea.H() * 0.3f;
    const float padH = mainArea.W() * 0.08f;
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
    constexpr float kHitRadiusPx = 10.f;
    int best = -1;
    float bestDist = kHitRadiusPx;
    for (int b = 0; b < kMaxBands; ++b)
    {
      const bool active = GetDelegate()->GetParam(BandParamIdx(b, kBandActive))->Value() > 0.5;
      if (!active) continue;
      const int circleId = static_cast<int>(GetDelegate()->GetParam(BandParamIdx(b, kBandCircleIndex))->Value());
      if (circleId != mSelectedCircle) continue;
      const double cutoffHz = GetDelegate()->GetParam(BandParamIdx(b, kBandCutoff))->Value();
      float bx = 0.f, by = 0.f;
      ArcPointForHz(mainArea, cutoffHz, bx, by);
      const float dist = std::sqrt((x - bx) * (x - bx) + (y - by) * (y - by));
      if (dist < bestDist) { bestDist = dist; best = b; }
    }
    return best;
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
    return { mainArea.MW(), mainArea.B, std::min(mainArea.W(), mainArea.H()) * 0.42f };
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
    const double t = HzToT(hz);
    const double angleRad = (180.0 + t * 180.0) * kPi / 180.0;
    xOut = arc.cx + arc.r * static_cast<float>(std::cos(angleRad));
    yOut = arc.cy + arc.r * static_cast<float>(std::sin(angleRad));
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
  void PushParamValue(int paramIdx, double realValue, double rangeMin, double rangeMax)
  {
    const double normalized = std::clamp((realValue - rangeMin) / (rangeMax - rangeMin), 0.0, 1.0);
    GetDelegate()->SendParameterValueFromUI(paramIdx, normalized);
  }

  double mSampleRate;
  int mSelectedCircle = 0;
  int mSelectedBand = -1;
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
