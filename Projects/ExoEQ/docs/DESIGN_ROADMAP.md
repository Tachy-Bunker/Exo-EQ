# ExoEQ — Design Roadmap (working list)

Status key: [ ] todo  [~] in progress  [x] done  [?] needs decision

Rule: work top to bottom inside each tier. Each item gets a README.md entry and its own zip (changed files only).

## Decisions made
- Ghost curve: show the OTHER CIRCLES' curves as faint dashed lines while editing. A/B is NOT previewed; A/B is an easy toggle only.
- Keep the mixed visual treatment as-is (not forced to be uniform).
- Number format: one format everywhere, same precision. (Proposed, see item T2-1 — confirm before building.)

## Tier 1 — Fix collisions and label bugs (visible now)
- [x] T1-1 Header overlap: "Band FX" button covers "Lab" text at top-left. Fix tab row spacing or shorten labels.
- [x] T1-2 Circle title "CIRCLE 2" is hidden under the floating node readout. Dock readout to a fixed strip.
- [~] T1-3 Type dropdown truncates "Morph (LP-BP-HP)". Full-width row, or ellipsis + tooltip.
- [x] T1-4 "Bypass" label overlaps the right panel slider. Separate bypass toggle from A/B block.
- [x] T1-5 Morph knob label shows "LP-BP-HP" while the value reads "50.00 %". Label should be "Morph"; active stage name shown as a caption under the knob.

## Tier 2 — Number formatting + discoverability of built features
- [x] T2-1 One frequency format everywhere (band editor, node readout, numeric entry hints).
      Proposal: below 1000 Hz -> "440 Hz" (0 dp); 1000 Hz and up -> "3.22 kHz" (2 dp).
      Gain: "+0.0 dB" everywhere (1 dp). Percent: 0 dp for Morph/Mix unless value is stepped.
      CONFIRMED. Tweak: Freq/Cutoff always shows 2 decimals ("440.00 Hz", "3.22 kHz").
- [x] T2-2 Numeric-entry cue: value text underline + pointer on hover. Touch: long-press opens entry. One-time first-launch tooltip "click value to type".
- [x] T2-3 Stepped knobs: notch ticks around the ring, active notch filled. Readout "Stage n / N".
- [x] T2-4 Morph strip under the Morph knob: three-segment LP | BP | HP bar, fills with position.
- [x] T2-5 Node-to-list color link: each circle node gets a hue; matching row in right stack and curve segment use it. Selecting a row highlights its node.
- [x] T2-6 Node tag entry: when a node is selected, two fixed fields (Freq, Gain) side by side, each clickable for typing.
- [x] T2-7 Band FX badge: dot on the Band FX button when any band effect is active.
- [x] T2-8 Global bypass: dim graph to ~40%, dry response as dashed line.
- [x] T2-9 A/B segmented control "A | B". Dot on the non-active letter when the other slot differs. (No preview.)

## Tier 3 — Comparison and preview
- [x] T3-1 Ghost curves for OTHER CIRCLES: faint dashed curve per other circle while editing. One extra path each; redraw only when that circle changes. Cap at a sane count (e.g. 4) for weak machines.
- [x] T3-2 Morph preview sparklines: four precomputed bitmaps (one per stage) shown beside the Morph knob. [?] Prototype first; IGraphics bitmap caching needed.
- [~] T3-3 Arc notches: superseded by the T3-2 morph stage preview, which shows each stage's curve. Stage dots on the selected node stay. Drop unless revisited.
- [x] T3-4 Status hint line: replace static "SHIFT + WHEEL MOVE" with a line describing the element under the cursor. Also suggestion: show the active A/B slot name here.

## Tier 4 — Motion + performance
- [x] T4-1 Load sequence: grid fades in, then curve draws from lowest node outward. Under 400 ms total.
- [ ] T4-2 Spectral depth animates only when audio is present. Silence = static.
- [ ] T4-3 Stepped parameters snap with no easing.
- [ ] T4-4 Two transition durations only: fast 120 ms, normal 250 ms.
- [x] T4-5 Static layer cache: grid, frequency labels, dB scale rendered once to bitmap; redraw only curve, active nodes, readout.
- [x] T4-6 Redraw cap 30 fps when audio is idle; redraw on parameter change only otherwise.
- [-] T4-7 Cache text strings/layout (no per-frame measuring).
- [x] T4-8 "Low motion" option in settings: disables spectral depth and transitions. (Not OS reduce-motion; DAW hosts don't reliably report it.)
- [-] T4-9 Tabular (fixed-width) digits for all readouts so values don't jitter.

## Tier 5 — Rare / experimental (prototype before committing)
- [x] T5-1 Hover-preview on the arc: hovering the circle shows frequency tick before click.
- [x] T5-2 Node move snaps to the stepped grid when the band's type is stepped.

## Tier 4 findings (checked against code, not yet changed)
- T4-3 stepped snap: already instant. ExoStepKnob rounds on SetValue; nothing eases. Treat as done.
- T4-4 durations: only one transition exists (depth-stack ease, 0.18 per redraw, CircleViewControl ~line 1857). Nothing to unify yet.
- T4-1 load sequence, T4-2 spectral depth motion, T4-6 30 fps cap: all need a steady UI tick. The only hook is IPlugAPIBase::OnIdle (IPlugAPIBase.cpp:191). Not verified on the FL/Windows UI thread. Decision needed before building these.
- T4-8 low motion: needs a new bool parameter and a settings control. The header row has no free cell left. Decision needed on where it lives.
- T4-5 static layer cache: needs an offscreen bitmap in IGraphics. Not started; biggest performance win on weak machines.
- T4-7 text caching, T4-9 tabular digits: low value on current profile; tabular digits not supported by the font path.

## Open questions
- [?] T2-1 number format (see above).
- [?] T3-2 sparkline bitmaps: cost on weak machines vs. benefit. Prototype and measure.
- [?] Ghost curve cap: how many other circles to show at once.

## Log
- 2026-10-20 Crash on GUI reopen fixed (dangling mCircleView). Open: user presets not saved while GUI is closed.
- 2026-10-20 T4-1 done (curve sweep, self-driven). OnIdle not needed: self-dirty pattern works.
- 2026-10-20 Tier 4 audited: T4-3 done by design; T4-4 n/a for now; T4-1, T4-2, T4-6 blocked on a verified UI tick; T4-8 needs a param and a placement decision; T4-5 is the biggest perf item.
- 2026-10-20 T3-4 done (context hint). T3-3 trial: stage dots not visible in the 16:49 screenshot; stepped knob ticks are visible. Needs a check.
- 2026-10-20 T3-3 trial: stage dots on selected node halo. Needs visual judgement.
- 2026-10-20 T3-1 done: ghost curves for other circles (cap 4, dashed).
- 2026-10-20 T2-5 and T2-9 done (curve hue, differ marker). T2-8 dashed dry line still open.
- 2026-10-20 T2-7 done: Band FX button shows * when any band FX send is on.
- 2026-10-20 T2-8 done (dim only, dashed dry line pending). T2-9 partial (bracket marker). Morph label text removed after stack overflow; LP/BP/HP ticks remain. Build confirmed no crash.
- 2026-10-20 Stack overflow persisted; morph label writes removed. Need PDB if it still crashes.
- 2026-10-20 Crash fixed: SetValue -> SetLabelStr -> SetDirty -> SetValue recursion (morph strip label). Caused stack overflow on band add.
- 2026-10-20 Screenshot fix pass: doubled units removed; morph strip replaced by ring ticks + label name (T2-4 revised).
- 2026-10-20 T2-5 partial: rail rows and active-row bar use the circle's hue (CircleHueColor). Curve colour link not done yet.
- 2026-10-20 T2-4 caption added (LP/BP/HP under strip). T2-6 marked done: node tag split into Freq/Gain entry was built earlier (node entry zip). Host check still needed.
- 2026-10-20 T2-2, T2-3 done; T2-4 strip done, caption pending. Visual offsets need a screenshot check.
- 2026-10-20 T1-4 done: Bypass/A-B moved to header cells 2-3. Tier 1 complete pending host test.
- 2026-10-20 T1-1, T1-2, T1-3 (partial: enum name), T1-5 done. T1-4 open (layout). Not host-tested.
- 2026-10-20 T2-1 done: FreqFormat.h + ExoEQ.cpp + CircleViewControl.h. Not yet host-tested.
- 2026-10-20 Palette: 8 fixed circle colours (CircleColorFor, ghost = 35% over bg). Nodes/curves/rail use circle colour; ghosted circles dim.
- 2026-10-20 Alt-click moves a band to the next circle (follows it). Shift-click still toggles mute.
- 2026-10-20 Presets cached on GUI close (saves correctly with GUI closed).
- 2026-10-20 T2-8 dashed dry 0 dB line during bypass. T4-6 30 fps cap (PLUG_FPS). T4-8 Low Motion param + toggle (kGlobalLowMotion, appended last). T5-1 arc hover tick. T5-2 1/3-octave snap for stepped types (Shift = free).
- 2026-10-20 Crash root cause: stale build output, not the source. Clean build fixed it. Lesson: check PDB source MD5 vs current file when offsets do not change.
- 2026-10-20 T3-2 morph preview for stepped + continuous types; redraws only after params settle (150 ms). T4-5 grid + dB rings cached in an IGraphics layer. T4-7 and T4-9 dropped (no IGraphics support without custom text layout). T3-3 superseded by T3-2.
