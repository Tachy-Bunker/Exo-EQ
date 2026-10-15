ExoEQ update: real rail drag-to-scroll + momentum, closing out the last
buildable-without-more-spec-from-you deferred item. Everything from
every prior round (filter-type popover, config.h 4:3, rail wheel-scroll,
Mel/log axis toggle, everything before that) is unchanged and still in
here. Read "GUI FEATURE, ROUND 14" first.

GUI FEATURE, ROUND 14 -- rail drag-to-scroll + momentum
Pressing and dragging on the rail now scrolls it live (1 row per rail-
row-height of pixel movement), same as the wheel-scroll from last round
but continuous instead of step-at-a-time. A small press+release with
barely any movement still counts as a tap-to-select, exactly like
before -- only a REAL drag scrolls. Release while still moving fast and
the rail keeps scrolling for a moment and decelerates (ease-out, ~450ms,
a fixed glide distance in the flick's direction) -- this is the actual
momentum/inertia half of the deferred item; last round's wheel-scroll
was real scrolling but stopped the instant the wheel did.
NOTE, the one real uncertainty in this round: the momentum glide uses
IControl's own animation-timer API (SetAnimation + GetAnimationProgress)
-- standard iPlug2 calls, but the one API in this entire project I have
the least confidence is exactly right without seeing your vendored
IControl.h directly (every other flagged call this project has hit so
far has been a dropdown/menu/text-entry/mouse-event signature; this is
the first one that's a whole different subsystem -- animation ticking).
If this specific block in CircleViewControl.h's OnMouseUp doesn't
compile, paste the error -- nothing else in this round depends on it;
drag-to-scroll itself (the non-momentum part) doesn't touch this API at
all and will keep working even if the momentum block needs fixing.
Still open (same reasons as last round): automation-ghost rendering and
the first-timer tutorial tour both still need your own description of
what they should actually show/walk through before I'd be building the
right thing instead of guessing.

GUI FEATURE, ROUND 13 -- tap now opens a filter-type popover card
Previously, tapping empty gauge space always force-created a Bell band
immediately. Now: tap opens a small card right at the tap point with 5
choices (Bell, Lowpass 12, Highpass 12, Bandpass 12, Notch 12) -- click
a row to create the band as that type, click anywhere else to cancel
(nothing gets created). Drag-release still immediately creates a Band-
Pass 12 range band exactly as before -- the card is specifically for the
tap gesture. This is the "connected gesture-sorted filter-type popover
card" from the original deferred list, with one honest simplification:
"gesture-sorted" specifically meant picking a type by swipe DIRECTION,
which needs more design detail than I have to get right (which
direction maps to which type, how many directions, etc.) -- what's
built instead is tap-opens-a-list, which is the same end result (quick
type choice without opening the full detail panel) through a plain,
unambiguous gesture. Say if you want true directional gesture-sorting
and the exact mapping, and I'll build that specifically.
Built with this file's own existing FillRect/DrawText/Contains
primitives, not IGraphics' native popup-menu API -- deliberately, since
that would be 2-3 more unverified-signature calls at once in a file
that already has several flagged; this keeps the card's own risk
surface to zero new SDK dependencies.
Still open, and why:
 - True physics-based momentum/inertia rail scroll (vs. last round's
   wheel-scroll, which IS real scrolling, just not momentum) -- needs a
   decaying-velocity animation loop running after mouse-release, which
   needs a timer/animation API this project has never used or verified
   anywhere. Buildable, but it's the one piece here I'd want to confirm
   the right iPlug2 API for rather than guess blind.
 - Automation-ghost / audio-rate-particle rendering -- this was always
   the vaguest item on the original list (no concrete visual spec was
   ever pinned down for what this actually looks like). Needs your own
   description of what you want drawn before I build the wrong thing.
 - First-timer tutorial tour -- same as above, needs your call on what
   it actually walks through and how many steps, before this is worth
   building.

GUI FEATURE, ROUND 12
1. config.h -- NOW FIXED DIRECTLY, no manual edit needed. You pasted
   your real file, so this zip includes the actual fixed config.h at
   Projects/ExoEQ/config.h -- let it overwrite yours. Only change:
   PLUG_WIDTH/PLUG_HEIGHT went from 600x600 (1:1) to 1000x750, exactly
   4:3, width the larger dimension as you asked. 750 tall was chosen to
   give the now-3-section layout (graph/detail panel/tab strip + header
   row) comfortable room -- 600px was already tight for that stack.
   AttachCornerResizer's EUIResizerMode::Scale mode (already in
   OnLayout, unchanged) scales width/height together on a manual drag-
   resize, so this starting ratio is what's preserved at any size you
   drag the window to -- nothing else needed.
2. Rail wheel-scroll -- the practical slice of "rail inertia-drag, no
   momentum scroll yet" from the original deferred list. Scroll your
   mouse wheel while over the right-hand rail to browse through circles
   beyond the fixed 8-row centered window, without changing which
   circle is selected. Clamped so you can't scroll past having nothing
   left to show. Re-centers on the selected row the moment you click a
   different circle. NOT physics-based momentum (a decaying-velocity
   scroll after you release a drag) -- that needs a timer/animation-loop
   API this project hasn't used or verified anywhere yet; this is the
   direct, immediate-response version instead. NOTE on OnMouseWheel:
   standard iPlug2 IControl virtual, same can't-verify-against-your-
   exact-headers caveat as every other UI/SDK-signature note in this
   project -- paste the error if its exact signature doesn't match.

ABOUT THE 4:3 ASPECT RATIO ASK -- I need your config.h
PLUG_WIDTH/PLUG_HEIGHT (the plugin window's starting size) live in your
project's config.h, which -- same situation as CMakeLists.txt a few
rounds back -- I don't have a synced copy of in this sandbox, so I can't
set it blind. Paste your config.h (or just the PLUG_WIDTH/PLUG_HEIGHT/
PLUG_MIN_WIDTH/PLUG_MIN_HEIGHT/PLUG_MAX_WIDTH/PLUG_MAX_HEIGHT lines) and
I'll set a real 4:3 ratio (e.g. 1000x750 or 900x675) and fix it there
directly next round. One thing already true regardless: OnLayout's own
`pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false)` call
already preserves whatever starting aspect ratio config.h sets when the
user drags-resizes the window -- EUIResizerMode::Scale scales width and
height together, it doesn't let them drift independently -- so setting
the ratio once in config.h is really the only piece missing here.

GUI FEATURE, ROUND 11 -- Mel vs. log frequency axis, switchable
Added a real "Mel Axis" toggle (header row, next to Mute/Solo Circle) --
the last deferred-list item that was still just a hardcoded choice: ON
(default, matches every existing project exactly) uses the Mel/
perceptual axis already in place; OFF switches the graph to a standard
log-frequency axis (equal pixel width per octave -- no extra high-end
compression, the shape most EQ plugins use). New global param
kGlobalFreqAxisMel, real/automatable/saved like everything else, read
directly by CircleViewControl.h's HzToX/XToHz every time it converts
between screen position and Hz -- so every band already on the graph,
the crosshair readout, and both band-creation/retune-drag gestures all
immediately respect whichever axis is selected, with no separate code
path for each. Pure display setting -- doesn't touch DSP, doesn't change
what any band actually sounds like, only where its dot is drawn/clicked.

GUI FEATURES + FIX, ROUND 10
1. Live crosshair + Hz readout. Hover (or drag) anywhere over the gauge
   and a thin vertical guide + the exact Hz value (e.g. "1.25 kHz" or
   "440 Hz") now follows your cursor -- "the crosshair + live Hz/cents
   readout" from the original deferred list (cents specifically left
   out -- nothing in this plugin's current params gives a note/cents
   reference to measure against; Hz alone matches every other readout
   already in this file). Uses OnMouseOver/OnMouseOut, iPlug2's standard
   hover virtuals -- same can't-verify-against-your-exact-headers caveat
   as the rest of this project's UI-signature notes.
2. Double-click a rail row to delete that WHOLE circle (deactivates
   every band assigned to it) -- the "preset-circle tray/drag-to-delete-
   a-circle" deferred item, built as a double-click since there's no
   tray/trash UI yet to actually drag something onto.
3. REAL FIX, not just a feature: retuning an existing band by dragging
   (built last round) was sending a raw SendParameterValueFromUI on
   every single mouse-move with NO Begin/EndInformHostOfParamChangeFromUI
   bracketing around the whole drag gesture. That's not cosmetic -- most
   hosts use that bracketing to know "this is ONE continuous automation
   gesture" and "this is ONE undo step"; without it, a drag could record
   as a flood of disconnected automation points, or not undo cleanly as
   a single action, depending on the host. Fixed by opening the bracket
   when the drag starts (OnMouseDown) and closing it when it ends
   (OnMouseUp) -- the same pattern iPlug2's own built-in knob/slider
   controls already use internally, just made explicit here since this
   control talks to params directly instead of going through one of
   those base classes. This is part of what "undo/redo" meant on the
   original deferred list -- the plugin relies on the HOST's own
   undo system (iPlug2 has no separate undo stack of its own), so making
   every gesture register as one clean host-level action IS the fix,
   not a custom undo stack built from scratch.
Still outstanding (say which's next): the connected gesture-sorted
filter-type popover card, rail inertia-drag/momentum, automation-ghost
rendering, first-timer tutorial tour, switchable frequency calibration
beyond Mel.

GUI FEATURE, ROUND 9 -- shift-click mute, selected-circle Solo/Mute
(still in this zip, unchanged from last round)

GUI FEATURE, ROUND 9 -- shift-click mute, selected-circle Solo/Mute
Two more items off the original design's "explicitly not in this round"
deferred list, now built:
 1. Shift-click an existing band on the graph to toggle its Mute
    instantly (CircleViewControl.h's OnMouseDown) -- no need to open the
    detail panel or hunt for the Mute toggle for a quick A/B. Uses
    IMouseMod's shift flag (`mod.S`) -- same can't-verify-against-your-
    exact-headers caveat as every other UI/SDK-signature note in this
    project; paste the error if that field name doesn't compile.
 2. "Mute Circle" / "Solo Circle" buttons, new header strip above the
    graph (ExoEQ.cpp). Mute Circle is a toggle: if any band in the
    currently-selected circle is unmuted, mutes all of them; if they're
    already all muted, unmutes all of them. Solo Circle is one-way
    (standard mixing-console solo): unmutes every band in the selected
    circle, mutes every band in every OTHER circle. Both read "which
    circle is selected" via a new CircleViewControl::GetSelectedCircle()
    getter -- the control's own selection state was private before this,
    with no way for ExoEQ.cpp's layout code to ask it anything.
Still outstanding from the original deferred list (say which one's
next): the connected gesture-sorted filter-type popover card, rail
inertia-drag/momentum, crosshair+live Hz/cents readout, automation-ghost
rendering, undo/redo+trash, preset-circle tray/drag-to-delete-a-circle,
first-timer tutorial tour, switchable frequency calibration beyond Mel.

GUI FEATURE, ROUND 8 -- bands on the graph were create-only, now you can
move and delete them too
What was missing: clicking an existing band on the gauge only selected
it (routed the detail panel to it) -- there was no drag-to-retune and no
delete gesture anywhere in CircleViewControl.h. The only way to change
an already-placed band's frequency was the detail panel's Cutoff knob,
and the only way to "remove" one was its Mute toggle, which silences it
but leaves it permanently occupying one of the 256 band slots.
Added, in CircleViewControl.h:
 1. Drag an existing band to retune it. Click-and-hold a band (instead
    of empty space) and drag -- its cutoff now tracks your cursor live,
    same Mel-axis XToHz conversion every other coordinate<->Hz spot in
    this file already uses. Release anywhere to stop; nothing else
    changes (type/resonance/etc. untouched, only cutoff).
 2. Double-click an existing band to delete it. Sets its kBandActive
    param back to 0 -- the exact same "inert slot" state an unused band
    already sits in, so this doesn't need any new DSP concept, just
    flips the one param that already governs it. Double-clicking empty
    gauge space, or the rail, does nothing (no accidental deletes from a
    stray double-click on blank area).
NOTE on OnMouseDblClick: this is iPlug2's standard double-click virtual
override (same name/pattern as OnMouseDown/Drag/Up, which all already
compile fine in this file) -- same can't-verify-against-your-exact-
vendored-headers caveat as every other UI/SDK-signature note in this
project. If this one specific override doesn't compile, paste the error
and I'll match whatever the real signature is.

GUI FIX, ROUND 7 -- dot doesn't land where the mouse points (still in
this zip, unchanged from last round)

GUI FIX, ROUND 7 -- dot appeared away from where you clicked/dragged
Root cause, found in CircleViewControl.h's OnMouseUp: for a click-DRAG
(creating a Band-Pass range band), the new band's cutoff was set to the
GEOMETRIC MEAN of your press point and your release point -- some
frequency BETWEEN the two, never exactly under either one. For a plain
tap (no drag) this was invisible (press and release are the same point,
so there's no gap to notice), but any real drag put the dot visibly
away from your cursor at release -- exactly what your screenshot showed.
Fixed: the dot's cutoff now always equals the RELEASE point directly,
both for a tap (no change there, start==end already) and for a drag
(previously the geometric mean, now the literal release x). Bandwidth/
resonance for a drag-created band still derives from how far apart your
press and release were, same as before -- only which Hz value becomes
the dot's own position changed.
One more thing worth knowing even with this fixed: the gauge's
frequency axis is MEL-scaled (your own earlier confirmed choice, "Mel/
perceptual" over a plain log axis), which compresses the upper end of
the range harder than a standard log-frequency EQ display does -- so
near the right edge, a small mouse movement can still correspond to a
bigger Hz jump than a linear or plain-log axis would give you. That's
the frequency AXIS's intentional shape, not a bug, and is separate from
the geometric-mean bug just fixed -- if positioning still feels
imprecise specifically in the top end after this update, that's the
axis curve to revisit (say so and we can swap it for plain log), not
another instance of this same bug.

GUI FIX, ROUND 6 -- clicking the black graph did nothing, now fixed
Root cause, found in CircleViewControl.h: a fresh/empty project has
ZERO active bands (every band defaults to inactive -- see ExoEQ.h's own
note on kBandActive), so GatherCircles() returns an empty list and
EnsureSelectionValid(circles) returns false. Both Draw() and
OnMouseDown() used to BAIL OUT ENTIRELY whenever that happened -- Draw()
drew nothing at all (not even the gauge's axis labels), and OnMouseDown
returned before ever reaching the drag-to-create-a-band code. Net
effect: on a brand new plugin instance there was no visible target AND
no way to click your way out of the empty state -- a real deadlock, not
a cosmetic issue. Round 5's layout fix (tabs) just made the dead graph
area visible/full-size, which is why this only showed up now.
Fixed 3 things in CircleViewControl.h:
 1. Draw() no longer returns early on an empty project -- every helper
    it calls (DrawDepthStack/DrawScrim/DrawSelectedGauge/DrawRail) was
    already safe with zero circles (plain for-loops over size 0), so the
    early return was pure UI dead-weight, not a real safety check.
 2. OnMouseDown() no longer bails either, for the same reason -- the
    rail hit-test, existing-band hit-test, and the drag-start code that
    follows all work fine with an empty project (the hit-tests just find
    nothing, which is correct).
 3. DrawSelectedGauge() now always draws the "20Hz"/"20kHz" axis labels
    and a new thin baseline guide bar across the gauge, even with zero
    bands in the selected circle -- previously it returned before
    drawing ANYTHING if the selected circle had no bands yet, so even
    once (1) and (2) are fixed, there'd have been no visible cue for
    where to click. Now there's a visible line to click/drag on from the
    very first launch.
What to expect now: open the plugin fresh, you should see a thin
horizontal guide line across the lower-center of the graph (between the
20Hz/20kHz labels) even with nothing created yet. Click it once = new
Bell band (point). Click-drag across it, release = new Band-Pass band
(range), release point sets its width. Either way a dot should appear
on the gauge and the detail panel below should retarget to it.

LAYOUT FIX, ROUND 5 -- your screenshot, global controls overlapping the
circle graph
What you saw: a cluster of small boxes (Group #, Grp Mute, Grp Gain,
Conn #, LFO1-4 Hz/Wave, Formula..., etc.) floating on top of the black
circle-graph area, plus a big flat grid of per-band knobs underneath
(Type/Cutoff/Reso/.../Energy2 Mix). The bottom grid is CORRECT and
intentional -- it's the per-band detail panel, confirmed with you
earlier ("one retargeting panel" design), it just retargets to whichever
band you click/select on the graph above it; nothing wrong there.
The overlapping boxes were the bug: Groups/Mod Matrix/LFOs/Fractal
Generator/Transform/Image-to-EQ controls were still positioned in a
fixed top-right stack from BEFORE the circle graph existed. Once the
circle graph started filling the whole top half of the window, that old
stack sat directly on top of it.
Fix: moved all 6 of those sections into a tab strip below the per-band
detail panel (you picked this over "shrink the graph for a side column"
and "collapsible drawer" when asked). The window is now 3 stacked
sections: circle graph (42% height) / per-band detail panel (30%) / tab
strip (28%, buttons on top, the active tab's content below). Click a
tab button (Groups / Mod Matrix / LFOs / Fractal / Transform / Image) to
show its controls; only one tab's content is visible at a time, exactly
like the 1-of-128-bands detail panel already worked. MTS-ESP's toggle
stays always-visible in the tab button row itself (top-right of that
row) since it's one single global switch, not worth its own tab.
Mechanically: every control in each tab section is collected into one
of 6 lists as it's created (ExoEQ.h's new mTabControls array), and each
tab button's click just calls IControl::Hide(true) on every OTHER tab's
list and Hide(false) on its own -- Hide() is a plain, already-proven
call in this project (TextEntryCallbackControl already used it), so
this doesn't lean on any new/unverified IGraphics API the way a
grouped-control show/hide helper would have.
Nothing in this round touches DSP, params, or any file outside
ExoEQ.cpp/.h -- if your project loads/sounds fine today, it still will
after this; this is purely where controls are drawn and shown/hidden.

BUILD FIX, ROUND 4 -- fixed two rounds ago, all 3 were LINK errors
Compiling succeeded this time (no more .cpp/.h errors) -- these are all
link-stage problems:
1. stbi_* "already defined in ExoEQ.obj" (conflicting with
   IGraphicsWin.obj) -- iPlug2's own IGraphicsWin.cpp ALREADY defines
   STB_IMAGE_IMPLEMENTATION and compiles the real stb_image
   implementation in for its own bitmap loading. ExoEQ.cpp was ALSO
   defining it (that was already in the project before this round,
   from the image-to-EQ round, and nobody had hit the actual double-
   implementation collision until now because the code never actually
   reached your disk before -- see BUILD FIX ROUND 2 above). Fixed by
   removing that #define from ExoEQ.cpp entirely -- ImageToEq.h's
   stbi_load/etc. calls now link against IGraphicsWin.obj's existing
   implementation instead of compiling a second, colliding one.
2 & 3. `pffft_*` and `MTS_*` unresolved externals -- NOT something I can
   fix from a code change. Vendor/pffft/pffft.c and Vendor/MTS-ESP/
   libMTSClient.cpp both exist in your project tree already (they
   compile fine as headers/declarations, which is why the .cpp files
   reached the LINK stage instead of erroring earlier) but neither .c/
   .cpp source file is actually in your CMakeLists.txt's SOURCES list,
   so nothing ever compiles their real function bodies in. This is a
   CMakeLists.txt edit, not a code fix -- see below.

CMAKELISTS.TXT -- NOW FIXED DIRECTLY, NO MANUAL EDIT NEEDED
You pasted your real CMakeLists.txt, so this round includes the actual
fixed file at Projects/ExoEQ/CMakeLists.txt -- just let it overwrite
yours. What changed, exactly:
  1. Added to the `SOURCES` list inside `iplug_add_plugin(...)`:
       Vendor/pffft/pffft.c
       Vendor/MTS-ESP/libMTSClient.cpp
     This is the actual, direct cause of every pffft_*/MTS_* unresolved-
     external error above -- both files were already #included fine as
     headers, but their .c/.cpp bodies were never in SOURCES, so their
     real function implementations never got compiled into any object
     file at all.
  2. Added a new block right after `iplug_add_plugin(...)`: on Linux
     (UNIX AND NOT APPLE), links `dl` into every module target (vst3/
     vst2/clap/aax/app) that exists, since MTS-ESP's libMTSClient.cpp
     dlopen()s a system library at runtime and dlopen/dlsym/dlclose live
     in libdl there. macOS gets an explicit no-op branch (those symbols
     are in libSystem by default, already linked). Windows needs nothing
     extra -- LoadLibrary is in the system libs CMake already links for
     it, which is why your current error list has no Windows-side
     dl-equivalent complaint.
  Your existing "Upstream gap fix (2026-10-14)" main.rc block (wiring
  resources/main.rc into VST3/VST2/CLAP/AAX for font/icon embedding) is
  preserved completely unchanged below this new block.
  For the record: the MTS-ESP round's own README already called out
  needing `dl` on non-Windows when that feature first shipped, and it
  looks like it never got applied -- and the pffft SOURCES omission was
  never flagged with its own note at all when the FFT stage shipped.
  Both are my oversight from earlier rounds, not yours; fixed here
  directly now that I actually have your real file instead of guessing
  at its shape.

BUILD FIX, ROUND 3 -- your latest error, real SDK-signature mismatches
These are actual compiler errors against your real vendored iPlug2
headers (not missing files) -- some are in CircleViewControl.h (new
this round, my mistake), some are in ExoEQ.cpp's existing fractal/
image-to-EQ UI code (pre-existing, flagged as unverified when first
written, now actually caught by your compiler):
1. CircleViewControl.h's FindBandNear/FirstInactiveBandSlot were marked
   `const` but call GetDelegate(), which isn't const-qualified in your
   iPlug2 version (BandGraphControl.h's own FindNodeNear already hit
   this exact issue once before -- I made the same mistake in the new
   file). Fixed by dropping `const` from both.
2. `pGraphics->CreateTextEntry(...)` in your iPlug2 version does NOT
   take a completion lambda as its 5th argument -- the real signature's
   5th param is an `int` (a max-length), and completion is delivered via
   IControl::OnTextEntryCompletion(), a virtual called on whichever
   control is passed as CreateTextEntry's FIRST argument. Fixed with a
   small reusable proxy control (TextEntryCallbackControl, new, in
   ExoEQ.cpp) that exists purely to receive that callback and forward it
   to an ordinary std::function -- used by both the Formula button and
   each of the 9 variable-range buttons.
3. `imageStatusText->SetStr(...)` failed because your iPlug2 version's
   AttachControl returns a plain IControl*, not a templated/derived
   return -- so `auto*` off of it silently narrowed imageStatusText to
   IControl*, which doesn't have SetStr (only ITextControl, a subclass,
   does). Fixed by declaring it as ITextControl* before attaching,
   instead of taking AttachControl's own return type.

BUILD FIX, ROUND 2 -- your latest error, two separate problems
1. "LFO.h(75,41): error C2065: 'M_PI': undeclared identifier" -- M_PI
   isn't standard C++; glibc/macOS expose it as a convenience extension
   but MSVC only defines it when _USE_MATH_DEFINES is set before <cmath>
   is included anywhere. Fixed by not depending on it at all -- LFO.h now
   defines its own exoeq::kPi constant instead.
2. "ExoEQ.h(13,10): Cannot open include file: 'DSP/ImageToEq.h'" -- same
   class of problem as BandGroups.h last round, different file: the
   image_to_eq round's delivery (DSP/ImageToEq.h + Vendor/stb/stb_image.h)
   evidently never actually landed on your disk either, even though
   that round's README described it as shipped and it IS sitting in this
   sandbox's own history (unlike BandGroups.h, I have the REAL original
   file this time, not a recreation) -- pulled directly from that round's
   zip, byte-for-byte unchanged, plus its vendored stb_image.h dependency
   (both missing, since ImageToEq.h's first line includes it).

Given TWO previously-"shipped" rounds have now turned out to not
actually be on your disk, it's worth checking your project folder
against this project's full file list next time something fails to
find an include, rather than assuming everything up to the last zip you
applied is actually there -- something about how these zips have been
unpacked/applied may be dropping files silently.

BUILD FIX, ROUND 1 (previous message) -- still applies, included again
Your error then -- "BandChain.h(10,10): Cannot open include file:
'BandGroups.h'" -- meant DSP/BandGroups.h never actually existed on
disk either, despite the groups_modmatrix round's README claiming that
DSP (BandGroups.h + ModMatrix.h + LFO.h) was "already fully built and
tested in an earlier session." Unlike ImageToEq.h, I don't have a copy
of these 3 anywhere in this sandbox's own history (confirmed by
checking every update zip ever produced here), so they're RECREATED,
not restored -- shaped to exactly match how ExoEQ.h/.cpp already calls
them, not verified against whatever the original internals did beyond
that. Each file's own header comment spells this out.

HOW TO APPLY
Unzip directly over your project root. Changed: ExoEQ.h, ExoEQ.cpp.
New: DSP/CircleChain.h, DSP/BandGroups.h, DSP/ModMatrix.h, DSP/LFO.h,
DSP/ImageToEq.h, Vendor/stb/stb_image.h, CircleViewControl.h. Unchanged
from the energy2 round (included again so this zip is self-contained):
DSP/BandChain.h.

IMPORTANT, READ FIRST: WHAT I COULD AND COULDN'T SEE
This sandbox only ever gets the files that changed in each round's zip --
it does not hold a synced copy of your whole project. Reconstructing
"the current file" meant chaining through every update zip ever produced
here by timestamp. Everything that touches DSP/BandChain.h, FilterCore.h,
FilterTypes.h, KeyTrack.h, MtsEspClient.h, FftContainer.h, FractalGenerator.h,
TransformFilter.h, and ImageToEq.h, I have current copies of (pulled from
whichever round most recently shipped each one) and verified ExoEQ.cpp's
#includes/usages against them before touching anything.

DSP/BandGroups.h, DSP/ModMatrix.h, and DSP/LFO.h were NOT recoverable
from this sandbox's own history -- per the groups_modmatrix round's own
README, that DSP was "already fully built and tested in an earlier
session," before this sandbox's history starts, and was never shipped
as its own zip since only ExoEQ.h/.cpp changed that round. Your C1083
error is the proof that whatever that earlier session built never
actually reached your disk. See the BUILD FIX section above -- all 3 are
recreated this round, from what ExoEQ.h/.cpp's own existing code already
requires of them.

YOUR 3 CALLS FROM THIS ROUND, APPLIED
1. Circles run in SERIES, not just visually -- circle 0's output feeds
   circle 1, circle 1 feeds circle 2, etc. Bands inside one circle still
   sum in PARALLEL exactly as before (DSP/CircleChain.h, full rationale
   in its header comment).
2. Band/circle ceiling raised 128 -> 256 (your call, tied to the future
   zoom feature making 128 feel tight once it exists).
3. A circle's color commits once a drag/edit is released, not live
   during the drag -- this round doesn't yet have per-frame drag-time
   recoloring to commit on vs. not (the gauge always just reads current
   param values when it draws), so this is naturally already how it
   behaves; flagging it as answered rather than silently assumed.

DSP: DSP/CircleChain.h (new)
- A circle IS a BandChain (same parallel-band-sum object this project
  already had) -- CircleChain just owns up to 256 of them and runs them
  one after another. BandChain.h itself is completely untouched.
- New per-band param: Circle N (kBandCircleIndex, int 0..255, default 0).
  Every band starts in circle 0 -- an existing project loading this is
  one circle holding every band, a series chain of length 1, byte-for-
  byte the old flat parallel sum. Zero behavior change until you actually
  use more than one circle.
- No new "circle order" param needed or added -- a band's circle ID IS
  its series position (circle 0 always runs first). Reordering circles
  in the future GUI means reassigning which bands' Circle N values point
  at which ID, not maintaining a separate order list that could drift.
- Flagged tradeoff, not fixed this round: group mute/gain envelopes
  (BandChain.h's existing 8-group feature) now run one independent copy
  per circle that has a group member in it, instead of one shared copy
  plugin-wide -- computes the identical value (no audible difference),
  just redundant CPU if many circles share one active group's envelope
  at once. See CircleChain.h's own comment for why this is a "measure
  before optimizing further" call, not an oversight.

GUI: CircleViewControl.h (new), replaces BandGraphControl in the layout
Per your confirmation that this view replaces the band list entirely.
BandGraphControl.h itself is left in the project tree, just unattached,
in case you want to roll back.

WHAT'S ACTUALLY WIRED THIS ROUND
- Depth-stacked circle overview: rolodex scale/opacity falloff by depth
  rank from whichever circle is selected.
- Per-circle hue from that circle's bands' average cutoff frequency
  (red->magenta->violet->blue->cyan, routed around yellow-green), ONE
  flat color per circle -- not per-band, not per-position. One fixed
  amber accent for every band inside any circle.
- 80%-black scrim specifically behind the selected circle's own
  footprint.
- Right-docked rail, one row per circle that has at least one band in
  it, max 8 visible rows with an alpha fade toward the edges, click a
  row to select that circle.
- Selected circle unrolled to a 180-degree gauge, Mel/perceptual
  frequency axis (20Hz..20kHz left to right, the confirmed default
  calibration), that circle's own bands drawn on the arc.
- Tap empty space on the gauge = new Bell-shaped band (Morph filter
  forced to its BP/bell midpoint). Click-drag-release = new Band-Pass 12
  band, with bandwidth set from how far apart the drag traveled. Both
  land in the currently selected circle, first free inactive band slot,
  and both call the same onBandSelected callback the old band-list
  detail panel already used -- that panel retargets exactly as before,
  no changes needed on its side.

RECREATED FILES, WHAT EACH ONE ACTUALLY DOES
- DSP/BandGroups.h: just the GroupSettings struct (mute, gainLinear, an
  EnvelopeFollowerParams) and kNumGroups=8 -- BandChain.h's own logic
  (accumulative gain, any-member mute, shared per-group envelope) was
  ALREADY fully written and untouched; this file only had to supply the
  type those lines read.
- DSP/LFO.h: LfoWaveform (Sine/Triangle/Saw/Square/Sample&Hold, matching
  the dropdown order), LfoParams, and LFO::Tick() returning -1..1. Ticked
  once per block (per ExoEQ.cpp's own existing call site) using an
  assumed 512-sample block for phase advance -- an approximation, not
  sample-accurate against your host's real block size; say if you want
  that tightened up.
- DSP/ModMatrix.h: ModDestination (6 entries matching the Mod Conn Dest
  dropdown exactly), ModMatrix::Connection, SetSourceValue/SetConnections/
  Apply. Apply accumulates every enabled connection's (source*depth) per
  destination, then applies each destination once -- Cutoff as an octave
  multiplier, Gain as a dB multiplier, Resonance/Morph/Drive/FreqShift as
  additive offsets, matching each one's own param semantics.

WHAT'S DELIBERATELY NOT IN THIS ROUND (flagged, not forgotten)
- The connected, gesture-sorted filter-type popover card (Concepts
  50/51) -- tap and drag both just create one fixed type each for now.
- Rail inertia-drag/momentum scrolling -- click-to-select only.
- The crosshair + live Hz/cents readout (Concept 52).
- Automation ghost + audio-rate particle rendering (Concept 53).
- Undo/redo history + trash (Concept 54).
- Preset-circle tray and drag-a-circle-out-to-delete-it (Concept 55).
- Shift-click mute/bypass on non-selected circles, and the selected
  circle's own Solo/Mute buttons.
- The first-timer tutorial tour.
- Switchable frequency calibration -- Mel/perceptual only, hardcoded;
  the other 3 options from Concept 49 aren't reachable from the GUI yet.
- Creating/renaming/deleting a circle as its own deliberate action --
  right now a circle only exists because a band points at it, and only
  stops existing once nothing does. The "Empty" preset-driven way to add
  one on purpose (Concept 55) isn't built yet either.

TESTING
No new automated tests this round -- CircleChain.h is new, untested DSP
glue code (it calls BandChain.h's own already-tested public interface,
it doesn't reimplement any filter math), and CircleViewControl.h is UI,
same "not unit-tested, not verified against the real SDK" category every
other GUI control in this project is already in. Say if you want a
regression test written for CircleChain.h specifically (its series-
feeds-forward behavior and the empty-circle skip are both straightforward
to test without the real iPlug2 SDK, same as BandChain's own tests).

NEXT
The connected type-picker card and the rail's inertia-drag are the two
most-requested-feeling pieces still missing from what's already designed
on the mockup canvas -- say if you want those next, or if presets/undo-
redo/mute-solo/tutorial should jump the queue instead.

ROUND 15: BAND DOTS NOW FOLLOW THE HEMICIRCLE ARC
You reported: "the line for the bands is a horizontal line, but it
should follow the hemicircle's line instead." It was -- DrawSelectedGauge
placed every band dot at HzToX(hz) for its X, but a FIXED y
(gaugeArea.B - 10.f), so the dots sat on a flat line while
DrawDepthStack drew a real curved arc behind them. Rewrote the geometry
in CircleViewControl.h so every band dot, and every mouse interaction
that creates/moves/deletes one, actually uses the SAME arc
DrawDepthStack already draws for the selected circle (its rank-0,
scale==1.0 case: cx = mainArea.MW(), cy = mainArea.B,
r = min(W,H)*0.42):

- ArcPointForHz(hz) -> (x,y): sweeps 20Hz at the arc's left foot
  (180deg) to 20kHz at its right foot (360deg), through the top in
  between -- the exact angle range DrawDepthStack's own
  g.DrawArc(..., 180.f, 360.f, ...) already draws, so a band dot now
  lands exactly on the visible curve, not near it.
- HzFromArcPoint(x,y) -> hz: the inverse, by the point's ANGLE around
  the arc's own center rather than its raw X -- needed because X alone
  no longer determines Hz once dots live on a curve instead of a line.
  Clicks below the arc's own baseline are folded up to baseline height
  first, so dragging below the curve still resolves to a Hz value
  instead of wrapping into the lower half of the circle.
- DrawSelectedGauge's old flat FillRect "baseline guide" is now a
  g.DrawArc call with the identical geometry -- so even an empty
  circle (zero bands yet) shows the real curve as its guide track, not
  a straight line that never matched what bands would be drawn on.
- FindBandNear (hit-testing for click-to-select/retune/delete),
  OnMouseDrag's live retune, and OnMouseUp's new-band creation (both the
  tap-opens-picker and drag-creates-Band-Pass-12 paths) all switched
  from XToHz(gaugeArea, x) to HzFromArcPoint(mainArea, x, y) -- using
  the FULL click/drag point, not just its X.
- The crosshair (DrawCrosshair) now tracks mHoverY in addition to
  mHoverX (OnMouseOver/OnMouseDrag both updated) and projects the raw
  cursor onto the arc the same way, so the live Hz readout and its dot
  sit on the curve too, not on the old flat line -- kept visually
  consistent with where a click would actually land a band.
- HzToX/XToHz (the old flat-line mapping) are gone, replaced by
  HzToT/TToHz (Hz <-> normalized 0..1 position, same Mel/log axis math,
  just without baking in a flat-line X) which ArcPointForHz/
  HzFromArcPoint build on top of -- removed rather than left dead, since
  nothing calls them anymore.

Everything here reuses primitives already proven to compile in this
file (DrawArc, FillCircle, FillRect, std::atan2/cos/sin) -- no new,
unverified IGraphics/IControl API surface this round. One simplification
worth flagging: HzFromArcPoint treats the arc as a perfect semicircle at
a FIXED radius (the selected circle's own, same one DrawDepthStack uses)
regardless of how far the cursor actually is from the center -- a click
far outside the arc still resolves to a Hz value (by angle alone, not
distance), which is intentional (lets a drag starting anywhere still
retune/create smoothly) but means there's no "too far from the curve,
ignore this click" radius check beyond FindBandNear's own 10px hit
radius for selecting an EXISTING dot specifically.

ROUND 16: BUILD FIX (MSVC C3493)
Your error: "'kMomentumExtraRows' cannot be implicitly captured because
no default capture mode has been specified" in CircleViewControl.h's
rail-momentum SetAnimation lambda. A constexpr local is allowed to be
used in a non-capturing lambda per the standard, but MSVC enforces this
more strictly in some contexts -- fixed by just capturing it explicitly
([this, direction, startOffset, kMomentumExtraRows]) instead of relying
on it being implicitly usable. No behavior change, pure build fix.

ROUND 17: GESTURE-DIRECTION FILTER-TYPE PICKING
The last real piece of the original "connected gesture-sorted filter-
type popover card" deferred item: dragging to create a band (not
tapping) now picks its filter type by the DRAG'S OWN DIRECTION, off the
same PickerItems() list the tap-popover shows (Bell, Lowpass 12,
Highpass 12, Bandpass 12, Notch 12) -- not always Band-Pass 12 anymore.

How it works: angle of the vector from press-point to release-point,
full 360 degrees split into 5 equal 72-degree slices (one per
PickerItems() entry), slice 0 centered on straight-right (angle 0),
proceeding counter-clockwise. No visible menu/overlay appears while
dragging -- there was never a concrete visual spec for one, so this
ships as the functional mapping only (same pattern as every other item
this project has shipped without a spec to guess from). Cutoff (release
point) and resonance (press/release octave span) still compute exactly
as before; only the TYPE is now direction-dependent.

This mapping (which direction -> which type) is an arbitrary but fixed
assignment, not something you specified -- say if you want specific
directions pinned to specific types (e.g. "up = boost/Bell, down = cut/
Notch, right = low end, left = high end") and I'll rewire the slice
order to match instead of the current straight-right-then-CCW default.

Still not built, because each needs a spec from you, not a guessed
default (see the updated top-of-file comment in CircleViewControl.h for
the full current list): automation ghost/audio-rate-particle rendering;
an in-plugin undo/redo history + trash UI (host-level undo already
works via the existing Begin/EndInformHostOfParamChangeFromUI gesture
brackets); the first-timer tutorial tour; a preset-circle tray to drag a
circle out of (deleting one by double-click already works, there's just
no tray).

ROUND 18: AUTOMATION-GHOST TRAIL
The last deferred item with no visual spec to guess from: "automation
ghost/audio-rate-particle rendering." Built the most literal reasonable
reading of "automation ghost" -- each band's dot now trails a short,
fading history of its own recent cutoff values (5 samples, sampled once
per repaint) along the SAME hemicircle arc the dot itself sits on. When
a host automates a band's cutoff, the trail visibly stretches out
behind the moving dot; when nothing's moving, every sample in the ring
is identical and the trail collapses invisibly back onto the dot (no
separate "idle" state needed -- it's just overdraw at the same point).

Added CircleViewControl::OnParamChange(int) override (SetDirty on every
param change) so the trail actually animates during host automation
playback, not just on the user's own mouse-driven SetDirty calls
elsewhere in the file -- NOTE: standard iPlug2 IControl virtual, but
can't verify the exact signature against your vendored headers; paste
the error if it doesn't compile, same caveat as everywhere else in this
project.

This is "audio-RATE" only in the sense that it updates every repaint (up
to PLUG_FPS=60/sec) driven by param changes, not a literal per-sample
particle system -- an actual audio-rate (44.1kHz+) visualization isn't
something a GUI repaint loop can show meaningfully anyway (far faster
than any display refreshes), so this is the practical ceiling for what
"ghost trail" can mean on a GUI thread. Say if you had something more
specific in mind (e.g. a persistent waveform-style history plot rather
than a short fading trail) and this can be reworked toward that instead.

Remaining deferred items, all still genuinely waiting on a spec from
you rather than a guessed default: an in-plugin undo/redo history + trash
UI (distinct from host-level undo, which already works via the existing
param-change gesture brackets); the first-timer tutorial tour (needs
your own steps/content); a preset-circle tray to drag a circle out of
(deleting one by double-click on its rail row already works).

ROUND 19: UNDO/REDO, PRESET-CIRCLE TRAY, AND A FABFILTER-STYLE "GRAPH IS
THE WHOLE WINDOW" LAYOUT

Three things from your last message, each addressed:

1) UNDO/REDO ("just some arrows"). Built as two small text buttons
("UNDO"/"REDO", not literal arrow glyphs -- a Unicode arrow's glyph
coverage depends on whatever font this build actually embeds, which
isn't something I can verify from here; plain ASCII text always
renders) in the graph's top-left corner, always visible, dimmed when
that direction has nothing to undo/redo. This is a WHOLE-PROJECT
snapshot stack (every band param's current value), not a per-action
diff -- simpler and far less likely to miss a mutation site than hand-
tracking "what exactly changed" for every one of this file's many
PushParamValue call sites. Capped at 24 steps. Snapshots are taken
before: creating a band (tap-popover commit and drag-direction commit),
retuning an existing band (once, at drag START, not per pixel moved),
shift-click-mute, deleting a band, and deleting a circle -- i.e. every
mutating action already in this file. NOTE: leans on
IParam::GetNormalized() (capture) + SendParameterValueFromUI (restore)
to round-trip any param's value without needing to know its real-value
range -- standard iPlug2 IParam method, but, same caveat as everywhere
else in this project, unverified against your exact headers.

2) PRESET-CIRCLE TRAY, per your spec: "a library on the left which pops
up when you hover over it, and then you can drag and drop a circle to
the bands, either replacing current circle if dropping at dead center,
or placing at a specific order between the Depth Rail lines, or adding
it as new and selecting if i drop it anywhere else." Built exactly that:
hovering near the left edge reveals a small panel listing 4 starter
circle templates (Clean Vocal Shelf, Warm Low Boost, Air Lift Top,
Notch Cleanup -- a deliberately simple starting set, say if you want
specific ones instead, exact types/Hz/Q). Drag one out:
  - Drop near the DEAD CENTER of the selected circle's own arc ->
    REPLACES it (every existing band in it is deactivated first).
  - Drop on the rail, between two rows -> INSERTS at that specific
    order (every circle at/after that point shifts up one slot --
    series order IS circle-ID order, so this is a real reorder).
  - Drop anywhere else -> ADDS as a brand new circle (first free ID)
    and selects it.
SIMPLIFICATION flagged in the code: the insert-at-order reorder clamps
rather than errors if a band is already sitting in the very last of
the 256 circle slots (nowhere left to shift up to) -- a real edge case,
not something a realistic project is likely to hit.

3) THE BIG ONE -- "the plugin should definitely be fully what the black
screen is right now, and all the parameters will appear and disappear
based on the actions, a bit like on Fabfilter Pro-Q." Changed ExoEQ.cpp
so graphArea is now the ENTIRE window below the header -- the circle
graph IS the whole plugin by default, exactly like Pro-Q's own curve
view. The per-band detail panel and the global-FX tab strip (Groups/
Mod Matrix/LFOs/Fractal/Transform/Image) keep their exact same bottom-
anchored sizing/internal layouts (nothing about their own GetGridCell
calls needed to change) but now FLOAT ON TOP of the graph instead of
pushing it up out of the way, and BOTH START HIDDEN:
  - The detail panel appears the moment you select a band, and stays
    up until you select a different one (there's no explicit "deselect
    to hide it again" action yet -- clicking empty graph space doesn't
    clear the selection. Say if you want that, e.g. an Escape key or a
    click on empty space, and it's a quick follow-up).
  - The tab strip is behind a new small "Global FX" button in the
    header (it needed a trigger OUTSIDE itself, since its own tab
    buttons would otherwise be hidden along with everything else it
    controls) -- click it to reveal/hide the whole strip, same single
    on/off switch either way.

This is the FIRST pass at the Pro-Q-style request, not the complete
version of it -- what this round does NOT yet do: no smooth slide/fade
transition when the panel or tab strip show/hide (it's an instant
Hide()/show, same mechanism this file already used for the tab-switch
itself); no inline, click-the-dot-and-a-tiny-popup-appears-right-there
micro-editing the way Pro-Q's own per-node popup works (the detail
panel is still one fixed panel at the bottom, just now hidden by
default rather than always-on); the header row itself (Mute Circle/
Solo Circle/Mel Axis/Global FX) is still always visible, not itself
contextual. Say which of these matters most and that's the next
concrete step on this.

NOTE on the "Global FX" button's lambda: captures a std::shared_ptr<int>
(the currently-active tab index) rather than a plain int by reference --
a few of this file's existing per-tab-button lambdas already captured
things by value correctly, but the ACTIVE TAB tracking variable itself
is new this round and needed to outlive OnLayout's own stack frame
(every button's SetActionFunction closure is stored and called much
later, long after OnLayout returns) -- flagging this explicitly since
it's exactly the kind of dangling-reference bug that compiles fine and
only breaks at runtime, not something a compiler error would catch for
you.

ROUND 20: BUILD FIXES (MSVC C2065/C2923/C2976/etc. cascade, and C3668)
Two real problems in last round's CircleViewControl.h, both now fixed:

1) FirstFreeCircleId and RailInsertionPoint (added for the preset tray)
were declared with `const std::vector<CircleInfo>&` parameters ABOVE
where CircleInfo itself gets declared in the file. A member function's
BODY gets "complete-class" lookup (can reference anything declared
anywhere in the class, in any order) but its PARAMETER TYPES don't --
those are resolved immediately at the point they're written, so a
nested type used as a parameter type has to already be declared further
up, textually. Fixed by moving both functions down to right after
CircleInfo/GatherCircles/EnsureSelectionValid, where CircleInfo is
already in scope -- everything else that used CircleInfo in this file
was already past that point, which is why only these two new functions
hit it.

2) OnParamChange(int) override: your build said it didn't override
anything in this exact iPlug2 version's IControl -- meaning either it's
not declared as a virtual there at all, or its real signature is
something other than the plain `(int paramIdx)` I guessed. Removed
rather than guessing a second signature blind. Net effect: the
automation-ghost trail (added two rounds ago) still updates on every
repaint this control gets from any other cause, it's just no longer
explicitly forced to repaint on a pure automation-only param change with
nothing else happening. If you want that back, the real method
name/signature from your vendored IControl.h would let me wire it in
exactly instead of guessing.

No behavior changes beyond these two fixes.

ROUND 21: DESELECT/CLOSE BUTTON FOR THE DETAIL PANEL
The follow-up flagged two rounds ago: once you select a band, the
detail panel showed and then had no way to hide itself again (clicking
empty graph space is already "create a new band," so it can't double
as deselect). Added a small "X" button pinned to the detail panel's own
top-right corner, visible only while the panel itself is -- click it to
clear the selection and hide the panel (and itself) again, same Hide()
pattern already used everywhere else this round for contextual show/
hide.

New CircleViewControl::ClearSelection() (public, alongside the existing
GetSelectedCircle() getter) does the actual mSelectedBand = -1; the
button's own action function in ExoEQ.cpp calls that AND hides
panelControls/itself in the same action, since neither side alone can
reach what it needs (the control can't touch the panel's own Hide()
calls; the button can't touch the control's private selection state
without the new getter-style method).

One real wrinkle worth flagging: the button needs `circleView`, which
doesn't exist yet at the point onBandSelected is defined (onBandSelected
is itself passed INTO circleView's constructor) -- so onBandSelected
captures a std::shared_ptr<IControl*> "slot" that starts null and gets
filled in once the button is actually created a few lines later. Same
shared_ptr-for-anything-a-long-lived-closure-needs-later pattern as
fxVisible/activeTabIdx from two rounds ago, now a third time -- flagging
in case a FOURTH case comes up later, since missing this pattern once is
exactly the kind of dangling-reference bug that compiles clean and only
breaks at runtime.

NOTE: GetFromTRHC (top-right-hand-corner IRECT helper) is not yet proven
to compile in your build -- GetFromBRHC already is (this file's own
build-info text at the bottom uses it), and IRECT's usual corner-helper
quartet almost certainly includes TRHC too, but paste the error if it
doesn't.

ROUND 22: THREE MORE FEATURES -- HOVER TOOLTIP, DISCOVERABLE PRESET TAB,
RAIL-SPACE DESELECT (PLUS A REAL BUG FIX THAT CAME OUT OF IT)

1) Per-band hover tooltip, Pro-Q style: hovering near an existing band's
own dot now shows a small readout right above it -- Hz, Q (resonance),
and "(muted)" when muted. Separate from the existing crosshair readout
(which tracks the arc generically, anywhere you hover); this one only
appears when you're actually close to a real band, same radius idea as
the click hit-test but a little more forgiving (12px vs. FindBandNear's
10px) since it's just a readout, not a mutation.

2) The preset tray's left-edge tab was too subtle to find on its own --
"pops up when you hover over it" doesn't work if nothing invites the
hover. Added a stacked "PRESETS" label down the tab's own height (one
letter per line -- simpler/safer than guessing at a text-rotation API
this file has never used or proven) and widened the actual hover zone
from 14px to 18px to match it.

3) A second way to close the detail panel, besides its own "X" button:
tapping empty RAIL space (below the last circle row, or an otherwise-
empty rail) now deselects too -- that tap used to be a pure no-op.
Tapping empty GAUGE space couldn't do this (already means "create a new
band there"), but the rail had nothing else going on in that spot.

REAL BUG CAUGHT WHILE WIRING #3: CircleViewControl now calls its own
mOnBandSelected(-1) callback to mean "deselected" (same callback
ExoEQ.cpp's close button already drives indirectly). But onBandSelected
in ExoEQ.cpp unconditionally ran `BandParamIdx(bandIndex, offset)` for
EVERY detail-panel control before this fix -- with bandIndex == -1, that
underflows into a garbage param index, and every one of those ~25
SetParamIdx calls would have retargeted a hidden control at an invalid
index. Added an early `if (bandIndex < 0) return;` right after the
Hide() calls, so deselecting only ever hides the panel, never retargets
it at garbage. The existing "X" button from last round was NOT affected
(it never went through onBandSelected at all), but this new rail-space
path would have hit it immediately without the guard -- caught before
shipping, not from a build error this time, just reading through what
the new code actually does with that -1.

ROUND 23: FINE-TUNE DRAG, WHEEL-NUDGE, MUTED-BAND VISUAL STATE

1) Fine-tune retuning: hold Shift while dragging an existing band and
it follows your cursor's target position DAMPED (12% per move) instead
of jumping straight there -- for placing a band precisely near a
specific Hz. This control maps drag position to Hz by absolute angle
around the arc (HzFromArcPoint), not by accumulated mouse delta the way
a knob's own fine-drag usually works, so damping toward the target is
the natural fit here rather than scaling a delta that doesn't exist in
this mapping.

2) Scroll wheel over the gauge now nudges the SELECTED band's cutoff
(previously the wheel only did anything over the rail). Steps in
normalized-position space, not raw Hz, so one notch feels the same size
everywhere along the axis instead of being huge near 20Hz and invisible
near 15kHz. Deliberately NOT captured by Undo -- same as a knob's own
incremental nudges never going through this control's undo stack;
snapshotting every wheel tick would flood Undo with noise instead of
the gestures that actually benefit from it (creates/deletes/retunes).

3) Muted bands now visibly read as muted ON THE GRAPH itself -- dimmed
(lower alpha, same hue) and slightly smaller dot -- instead of looking
identical to an active band until you opened the detail panel or
hovered it (last round's new tooltip). Small but real: "is anything
here muted" used to require checking each band individually.

ROUND 24: RAIL MUTE-DIM + HOVER TOOLTIP, PRESET-TRAY ROW PREVIEWS

1) A circle whose every active band is muted now dims its own rail row
too -- same visual language the per-band dots got two rounds ago ("still
here, just not doing anything"). Before this a fully-muted circle looked
identical to an active one on the rail.

2) Hovering a rail row now shows a small tooltip to its left: average
Hz + band count for that circle. Same hover state (mHoverValid/X/Y)
every other readout in this file already tracks, just checked against
each row's own rect this time.

3) Preset-tray rows now show a one-line preview under each name (first
band's type + Hz -- e.g. "Bell @ 3.0kHz") instead of just a bare name --
widened each row from 28px to 38px to fit it (GetPresetTrayRect sizes
off the same constant, so nothing else needed adjusting). Only the
preset's FIRST band is previewed even though PresetCircleDef supports
up to 3 -- none of the current 4 templates use more than one, and a
short label beats a full dump if that changes later.

ROUND 25: SAVE-YOUR-OWN PRESETS + LIVE DROP-TARGET FEEDBACK
Two more pieces of the preset-tray spec:

1) The tray was 4 fixed built-ins forever -- now there's a "+ Save
Current" row right after them. Click it to capture the currently
selected circle (up to 3 of its active bands -- same fixed-size cap
every built-in preset already lives under) as a new "Custom N" entry,
appended to the SAME draggable list the built-ins live in. Session-only
(not written into the plugin's saved project state -- no param slot
exists for "an arbitrary number of named circle templates", and adding
one is bigger than this round's scope), capped at 8 -- saving a 9th
drops the oldest. PresetCircleDef's `name` field changed from a string-
literal pointer to a small fixed buffer to make this possible without
a dangling pointer once a preset is user-generated rather than compiled
in.

2) Dragging a preset now shows which of the three outcomes would
actually happen if you let go right now: an amber ring around the whole
arc for REPLACE (dead center), a highlighted line at the exact gap for
INSERT (hovering the rail between two rows), or a small "+" at the
cursor for ADD NEW (anywhere else) -- purely visual, doesn't change
what OnMouseUp's own drop resolution actually does, just tells you
which of its three branches you're currently aimed at before you
commit.

====================================================================
ROUND 26 -- REAL BUG FIX from your screenshot: detail panel invisible
====================================================================

Good news first: that build log is the first fully clean build this
session -- VST3 and standalone app both compiled, only one unrelated
warning deep in iPlug2's own font code. Not our code's fault.

The bug in the screenshot: the detail panel's knobs were just black
empty space, even with a band selected (one dot shown larger/white).
Only the X button, header buttons (Global FX/Mute/Solo/Mel Axis),
UNDO/REDO, and the PRESETS tab were visible.

Root cause: iPlug2 draws attached controls in the order you attach
them -- later attach = drawn ON TOP, fully covering anything under it
if it paints an opaque background. CircleViewControl now fills its
ENTIRE bounds with an opaque backdrop every frame (since last round's
"graph is the whole window" change). circleView was being attached
AFTER the panel knobs in ExoEQ.cpp -- so its own backdrop painted
directly over the knobs every single frame, regardless of their
Hide(false) state. This exact bug was invisible before last round,
because the graph only used to cover the top 42% of the screen; the
panel used to live entirely below it, never under it.

Fix: moved circleView's construction and AttachControl() call to the
very top of OnLayout's control-building section, BEFORE any panel
knob, tab button, or header button is created -- so everything else
now naturally draws on top of it, same as the X button and header
buttons already did by accident. Since circleView's onBandSelected
callback needs knob pointers that don't exist yet at that point in the
function, used the same forward-reference-slot trick already proven
elsewhere in this file (panelCloseButtonSlot/fxVisible/activeTabIdx):
a std::shared_ptr<std::function<void(int)>> that circleView's own
constructor captures a tiny one-line forwarding lambda around, filled
in with the real onBandSelected lambda once it's built further down
(right after its own definition, where panelCloseButtonSlot used to be
filled in).

Nothing else about onBandSelected, panelCloseButtonSlot, or any knob
changed -- this is a pure reordering-plus-indirection fix, nothing in
CircleViewControl.h needed to change at all.

Only ExoEQ.cpp changed this round.

====================================================================
ROUND 27 -- user presets now actually persist (were session-only)
====================================================================

Your screenshot confirmed Round 26's z-order fix works: the rail,
UNDO/REDO, header buttons, and the X button all render correctly now
with nothing bleeding through or getting hidden. (Panel knobs weren't
visible in that particular screenshot because no band was selected --
that's correct, intended behavior, not the bug.)

Picked up the next flagged item without waiting to be asked: user-saved
preset circles (the "+ Save Current" tray row from a couple rounds ago)
were explicitly called out as session-only, "bigger task for later" --
this round is that task.

- CircleViewControl.h: added GetUserPresets()/SetUserPresets() on top
  of the existing mUserPresets vector.
- ExoEQ.h: a forward-declared CircleViewControl* mCircleView (set once
  in OnLayout), plus a small POD PendingPresetDef staging struct for
  the case below.
- ExoEQ.cpp: SerializeState/UnserializeState now read/write your saved
  presets field-by-field (not as a raw struct blob -- padding/layout
  can differ across builds/compilers, so each int/double/bool goes
  through chunk.Put/Get individually, same approach already used for
  the fractal custom-var ranges).

The one real wrinkle: most hosts restore plugin state BEFORE you ever
open the editor window (OnLayout only runs on first open), so
mCircleView is often still null exactly when UnserializeState needs to
hand it your presets. Handled with a staging buffer (mPendingUserPresets)
that OnLayout drains into the freshly-built circleView the moment one
exists -- so presets survive a saved project being reloaded whether or
not you had the UI open when it loaded.

Old projects saved before this feature existed just read 0 presets and
move on, same backward-compat pattern the fractal-formula string
already uses above it in the same two functions.

Changed this round: CircleViewControl.h, ExoEQ.h, ExoEQ.cpp.

====================================================================
ROUND 28 -- build fix: PresetCircleDef/GetUserPresets were private
====================================================================

Your error log, all C2248 "cannot access private member/struct": the
`private:` label partway through CircleViewControl.h (added at some
earlier round, unrelated to last round's work) put PresetCircleDef,
PresetBand, and GetUserPresets/SetUserPresets out of reach of
ExoEQ.cpp's SerializeState/UnserializeState, which is exactly what
Round 27's persistence code needs to call.

Fix: removed that `private:` label. CircleViewControl only ever had
one access block before (everything public, matching the rest of this
file's style) -- nothing below that point actually needs real
encapsulation from ExoEQ.cpp, so this is a straight removal, not a
re-split. Only CircleViewControl.h changed this round.
