# ExoEQ

ExoEQ is a node-based, multi-band EQ and experimental filter instrument built with C++ and iPlug2. It targets a professional everyday workflow while making microtonal tracking, unusual filters, per-band effects, modulation, and generative sound design first-class features.

This file is the project source of truth. Update it when implementation, product decisions, build targets, or known limitations change. `README_this_update.txt` is historical handover material and is not authoritative when it conflicts with this document.

## Product promise

ExoEQ should be as dependable and quick to use as a modern professional EQ, while offering capabilities that normal EQs do not:

- microtonal per-band pitch tracking;
- SCL/TUN tuning and eventual MTS-ESP support;
- circle-based organization of bands;
- per-band delay, distortion, frequency shifting, and sidechain modulation;
- experimental filter types and morphable filter behavior;
- modulation and generative tools that can build whole filter series.

The product has two audiences: people who need a precise, useful EQ and sound designers who want unusual spectral behavior. The normal EQ workflow must remain understandable when experimental systems are unused.

## Current implementation

### Audio and DSP

The following systems are present in `DSP/` and have been unit-tested during development:

- parallel multi-band processing with up to 256 band slots;
- state-variable filtering with LP/BP/HP outputs and morphing;
- filter type selection, gain, resonance, cutoff, mute, solo, and active state;
- delay, distortion, and single-sideband frequency-shift sends;
- sidechain envelope following and per-band cutoff modulation;
- LFO and modulation-matrix infrastructure;
- Scala `.scl` parsing, tuning tables, latch, and portamento;
- band groups and circle assignment;
- frequency-response calculation;
- FFT containers, image-to-EQ, transform, fractal, and related generative components.

MTS-ESP is scaffolded but requires the real external client SDK to be vendored and verified. `.kbm` keyboard mapping is not implemented yet. FFT and generative systems remain optional until their latency, CPU cost, and workflow are verified in the plugin.

### GUI

The main GUI is implemented in `CircleViewControl.h` and `BandGraphControl.h`:

- 180-degree hemicircle arc layout for band nodes;
- live crosshair and Hz readout, with Mel/log frequency-axis switching;
- draggable band retuning, shift fine-tuning, wheel nudging, and double-click deletion;
- gesture-based creation with filter-type popover;
- right-hand Depth Rail with circle selection, hover information, scrolling, drag scrolling, and momentum;
- dimming for fully muted circles and per-band/per-circle mute and solo;
- contextual detail panel exposing core, delay, distortion, frequency-shift, sidechain, and Energy2 controls;
- preset-circle tray with replace, insert, add, save, drop-target feedback, and project persistence;
- undo/redo project snapshots;
- automation ghost trail infrastructure.

Known GUI limitations are listed under [Open work](#open-work). Some iPlug2 signatures were written from general SDK knowledge and still need verification against the exact vendored headers, even where the current build accepted them.

## Build targets

The intended first release targets are Windows VST3, macOS VST3, standalone development apps, Audio Unit on macOS, and CLAP after its SDK is fetched. AAX is later because it requires Avid developer access and signing.

The project expects an iPlug2 checkout beside `Projects/` and uses `CMakeLists.txt` to locate it. The source ZIP intentionally excludes iPlug2 and `.git`.

## Quality bar

Before calling a release shippable, verify that audio stays finite and stable at parameter extremes; parameter changes do not click, zipper, denormalize, or stall the audio thread; gestures create sensible host automation and undo steps; save/reload restores every audible setting and custom preset; no-active-band, many-band, sidechain, MIDI, offline-render, and sample-rate-change cases work; the UI remains legible at practical sizes; and CPU, latency, and memory are measured in representative sessions.

## Roadmap

### M0 — Baseline and contracts

- restore the matching iPlug2 checkout and build a clean Windows VST3 and standalone app;
- audit every parameter's range, unit, default, smoothing, automation, and serialization;
- verify inactive-band behavior and reset behavior;
- establish a repeatable host test matrix.

### M1 — Professional core

- finish the everyday EQ workflow and contextual layout;
- add precise numeric editing and clear filter semantics;
- verify response rendering against the actual DSP;
- complete state, preset, undo, bypass, A/B, and automation behavior;
- test stereo, sidechain, offline rendering, and sample-rate changes.

### M2 — Microtonal workflow

- finish per-band tuning UX;
- support `.scl` and `.tun` cleanly;
- add `.kbm` mapping;
- integrate and verify MTS-ESP;
- make latch, portamento, MIDI note display, and tuning reference understandable.

### M3 — Experimental controls

- expose the modulation matrix without overwhelming the basic workflow;
- expose advanced filter types and per-band sends with useful defaults;
- add group modulation and circle-level controls;
- make experimental modes discoverable through presets and tooltips.

### M4 — Generative and FFT features

- verify FFT latency and CPU behavior;
- integrate image-to-EQ, transform, fractal, and spectral capture as optional workflows;
- keep the time-domain core usable when those systems are disabled;
- document which features are suitable for live use and which are render-oriented.

### M5 — Release engineering

- macOS VST3 and AU builds, then CLAP;
- signing, packaging, installation, crash reporting, and update strategy;
- compatibility passes in several DAWs;
- release presets, manual, and regression checklist.

## Open work

- tutorial tour;
- fade/slide transitions for contextual panels and tabs;
- contextual header visibility;
- inline micro-editing where it improves speed without reducing clarity;
- repaint behavior for host-only automation changes (`OnParamChange` needs the exact iPlug2 virtual signature);
- verification of `SetParamIdx`, `IVMenuButtonControl`, `GetFromTRHC`, animation, and related SDK calls;
- full regression in real DAWs, including offline render paths and sidechain layouts;
- MTS-ESP SDK integration and `.kbm` support;
- macOS/AU/CLAP build verification and release packaging.

## Architecture rules

- Keep the audio callback allocation-free, lock-free, and finite for all reachable parameter values.
- Keep filter-type selection and expensive setup at block or parameter-change boundaries, never in the per-sample hot loop.
- Keep the time-domain path latency-free where possible; FFT features remain optional.
- Treat parameter definitions, serialization, UI mapping, and DSP units as one contract.
- Prefer reusable systems over one-off implementations for each experimental feature.
- Do not call a feature complete until it has a clear UI path, saved state, and host evidence.

## Development log

### 2026-10-15 — Phase 1 interaction pass

- centered the circle stack and made the complete arc selectable;
- made empty-space clicks create an immediate Bell band;
- improved band selection and dragging with a larger hitbox;
- added hover halos, selected-band halo, and persistent frequency/Q readout;
- reduced the default selected-band panel to essential controls;
- widened and labeled the Depth Rail and central active circle;
- verified Windows VST3 and standalone Release builds.

### 2026-10-15 — Depth transition pass

- added eased movement between selected circle layers;
- synchronized arc outlines, band dots, and ghost trails during transitions;
- widened the rail and added central active-circle labeling;
- verified Windows VST3 and standalone Release builds again.

### 2026-10-15 — Circle order control

- added Shift + rail-wheel reordering for the selected circle;
- swaps band assignments atomically and records one undo step;
- leaves ordinary rail scrolling unchanged;
- verified Windows VST3 and standalone Release builds.

Record each meaningful change with its date, files changed, build or host evidence, tests run, and remaining uncertainty. Distinguish “implemented in source” from “verified in a real host.” The root repository README and `README_this_update.txt` retain the detailed historical log.

### 2026-10-16 — Airwindows parameter pass (batches 1–6)

Implemented in source; not yet built or verified in a host.

- AW Z-Lowpass: morph is a continuous 1→2 stage crossfade (was a threshold at 0.5). DSP/FilterTypes.h.
- AW X-Lowpass: extra is the source input gain, `pow(extra+0.9, 4)`; default 0.1 = unity. DSP/FilterTypes.h.
- AW Y family: morph is the source dry/wet mix (`dry = morph`; default 0 = fully processed). DSP/FilterTypes.h.
- Morph and Extra moved into the always-visible compact row (8 cells).
- Per-type knob labels rebuilt from a sweep of every filter type (ordinals 0–44). A knob that does nothing for the selected type is hidden, and the compact row reflows to close the gap.
- Fixed: comb/ring/FM/formant/phaser/resonator/resample labels were keyed to ordinals 49–57 and never applied; now 36–44.
- AW AverMatrix: morph mapping reversed so the default (morph 0) is full wet (+1); morph 1 is the complement (−1). Measured at 1 kHz: morph 0 −2.08 dB, morph 1 +3.31 dB.
- Stepped knobs (int/enum params) snap to their discrete positions via ExoStepKnob: slot order ×3, Group #, Conn #, Src, Target, Fractal Branch, Fractal Depth, Image Nodes. Drag path through SetValue is unverified in the SDK.
- Per-type Morph steps (MorphStepsForType): AW LP2/HP2, X family, Holt 4; Z-HP/BP/Notch 2; SlewSonic 4; Comb FF/FB 2 (labelled Polarity); Phaser 8; Phys Odd/Even 8 (labelled Partials). Continuous, left as-is: Z-Lowpass crossfade, Vocal Formant vowel interpolation, Ring/FM shape, Hermepass slope, Resample smooth, Y family, BiquadNonLin, AverMatrix, Z-Acid, Capacitor, AirLift.

Remaining uncertainty: `SetTargetRECT` reflow and the compact-row layout at small window sizes are unverified in a host build.

- Morph readout (FormatMorphReadout via IParam::SetDisplayFunc): stepped types show their real step ("3 stages", "+"/"-" polarity, "4 partials"); continuous types keep the % readout. Z-Lowpass stays continuous.

### 2026-10-16 — M1: numeric editing (first pass)

- Click the value text under any compact-row knob (Cutoff, Reso, Gain, Morph, Extra) to type a value.
- Parses "440", "2.5k", "2.5 kHz", "-3.5 dB", "50%"; clamps to the parameter range.
- Replaces iPlug2's own entry for these knobs: it parses with atof, so "2.5k" read as 2.5 Hz.
- Implemented in source; not yet verified in a host build.
- Still open: numeric entry for circle nodes, Band FX knobs, and the reso/gain values shown on nodes.
- Stepped Morph text box shows the step count (stages/partials 1..N, SlewSonic level 0..3, Comb polarity 0/1) and converts typed counts back to the stored percent.
- Numeric entry extended to the Band FX knobs (KeyTrk, Delay, Distortion, Shift, SC Depth, Energy2). Click the value text under each knob.
- Still open for numeric editing: circle nodes (typing a Hz/reso/gain directly on a node).

### 2026-10-16 — M1: global bypass

- Added kGlobalBypass (appended last in the global enum so saved state keeps its indices). Default off.
- ProcessBlock passes input straight to output when on; no DSP runs.
- Header row is now 8 cells: 6 tabs, MTS-ESP, Bypass.
- Latency: the FFT stage still reports its latency to the host while bypassed, so toggling bypass shifts timing by that latency. Needs a host check.
- Implemented in source; not yet verified in a host build.

### 2026-10-16 — M1: A/B compare

- Header A/B button (9-cell header row: 6 tabs, MTS-ESP, Bypass, A/B). The label shows the active slot.
- First press: A is saved, B starts as a copy of A. Each press saves the slot you leave and recalls the other.
- Recall sends only the parameters that differ, through SendParameterValueFromUI, so host automation sees the change.
- Session-only: A and B are not saved with the project.
- Implemented in source; not yet verified in a host build.

### 2026-10-16 — M1: circle-node numeric entry

- Click the tag above the selected node (Hz and dB) to type "freq" or "freq gain", e.g. "2.5k" or "2.5k -3.5".
- Frequency clamps to 20 Hz-20 kHz; gain clamps to +-24 dB. Unit text such as "Hz" is ignored.
- Records one undo step. Double-click on a node still deletes it.
- Implemented in source; not yet verified in a host build.

### 2026-10-16 — Crash report (FL Studio, ExoEQ.vst3 access violation)

- Log: access violation at ExoEQ.vst3 offset 0x1970A, reading 0x0000000200000137, after a series of knob tweaks (cutoff, resonance, filter type).
- Root cause not yet confirmed. The log has no symbols, so the function at that offset is unknown.
- Defensive changes: null checks on every parameter lookup in the new display, text-entry and knob code paths.
- Still needed: a Debug or PDB-enabled build and a fresh crash log, to name the function at offset 0x1970A.
- Likely cause (from the report: crash after opening the wrapper's routing tab and returning): ProcessBlock read inputs[2+c] (sidechain) unconditionally. When the host routes only the main pair, those pointers are invalid. Fixed: sidechain is read only when at least four input channels are connected; otherwise it is silence.
- Also: OnUIClose clears the pointers into the editor's controls (mRefreshKnobNames, mAbButton), so a parameter change while the editor is closed cannot call into destroyed controls.

### 2026-10-16 — Crash: routing-tab return (second attempt)

- Repro: open FL's wrapper routing tab and return, with no sidechain or MIDI touched. Closing and reopening the plugin normally does not crash.
- That points at the host changing channel routing while the plugin is live, not at the editor.
- ProcessBlock now checks the connected channel counts first. If the main stereo pair is not connected, or the buffers are missing, it outputs silence and returns, before any input or output buffer is read.
- Tradeoff: with a mono or unrouted input the plugin outputs silence instead of processing.
- Not yet verified in FL Studio.


## T2-1 — One Hz/kHz readout format (2026-10-20)
- New FreqFormat.h: FormatHzReadout(hz). Below 1 kHz "440.00 Hz", 1 kHz and up "3.22 kHz", always 2 decimals.
- Cutoff param now uses that format via SetDisplayFunc (band editor knob + any display of the param).
- Node tag, hover label, band-list preview, circle average label all use the same helper. Gain stays at 1 decimal.
- Changed files: ExoEQ.cpp, CircleViewControl.h, FreqFormat.h (new).


## T1-1, T1-2, T1-5 — header overlap, circle label, morph label (2026-10-20)
- T1-1: removed the "Dirty Hardware Lab" header label that overlapped Band FX. Re-place it elsewhere if wanted.
- T1-2: "CIRCLE n" layer label moved to the arc's left shoulder so the node tag no longer covers it.
- T1-3 (partial): filter type entry renamed "Morph (LP-BP-HP)" -> "Morph" so the dropdown fits.
- T1-5: morph knob label for type 0 is "Morph" (was "LP-BP-HP").
- T1-4 not done: Bypass / A-B row needs a layout pass against the right-hand node stack.
- Changed files: ExoEQ.cpp, CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.


## T1-4 — Bypass and A/B moved to header (2026-10-20)
- Bypass toggle now header cell 2 of 7, A/B button header cell 3 of 7. Both always visible, no longer inside the Global FX drawer.
- Changed files: ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.
- Not compiled. If the build complains that headerRow is out of scope at this point, the layout lambda split is the cause; tell me the error.


## T2-2, T2-3, T2-4 — hover cue, stepped notches, morph strip (2026-10-20)
- T2-2: ExoStepKnob draws a short underline under the value text while the mouse is over it (click-to-type cue). Touch long-press not done yet.
- T2-3: stepped knobs draw notch ticks around the ring; active step is brighter and longer. Geometry uses iPlug's own angle convention (0 = top).
- T2-4: LP | BP | HP marker strip under the Morph knob, shown only for filter type 0. Drawn only if there is room under the knob; otherwise skipped. Caption with stage name still to do.
- Changed files: ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.
- Visual positions (notch radius, strip offset) are estimates. Check against a screenshot and tell me what to nudge.


## T2-4 caption — stage name under the morph strip (2026-10-20)
- LP / BP / HP caption under the morph strip, nearest of 0 / 50 / 100 %. Hidden if no room. Roadmap T2-6 marked done (built earlier).
- Changed files: ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.


## T2-5 (partial) — rail row hue (2026-10-20)
- Right-rail rows and the active row's left bar take the circle's own hue, via the existing CircleHueColor(avgHz). Previously neutral.
- Not done: the curve/arc colour per circle, and selecting a row highlighting its node. Next pass.
- Changed files: CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.


## Fix pass after screenshot (2026-10-20)
- Doubled units fixed: Cutoff read "1.00 kHz Hz" and Morph read "0.00% %". The display functions already print their own unit, so the param unit is now "" for both.
- Morph strip under the knob overlapped the value text. Replaced by LP / BP / HP ticks on the outer ring (0, 50, 100 %). The stage name now shows in the knob label ("Morph: LP" / "BP" / "HP"), updated on every value change. The caption under the strip is gone.
- Changed files: ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.


## Crash fix — stack overflow when adding a band (2026-10-20)
- Cause: ExoStepKnob::SetValue() called UpdateStripLabel(), which called SetLabelStr(). SetLabelStr() calls SetDirty(), and IControl::SetDirty() calls SetValue() again. Infinite recursion, reported as a stack overflow in ExoEQ.vst3 (crash log 2026-10-20_164134).
- Fix: label is only set when its text changes, and UpdateStripLabel never re-enters itself (mInStripLabel guard).
- Changed files: ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.


## Crash follow-up (2026-10-20)
- Stack overflow still reproduced after the label guard. The label write is removed from SetValue and SetStrip entirely. Morph knob label is back to "Morph".
- Stage name is no longer shown as text; LP / BP / HP ticks on the ring remain.
- If the overflow still occurs with this build, the cause is elsewhere. Send ExoEQ.pdb from the same build as the crash log so the offsets can be mapped to functions.
- Changed files: ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.


## T2-8 bypass dim, T2-9 A/B active marker (2026-10-20)
- T2-8: when Bypass is on, the whole circle view is dimmed (translucent overlay). The dashed dry-response line is not done yet.
- T2-9 (partial): the A/B button shows the active slot in brackets: "[A]  B" or "A  [B]". The dot for "slots differ" is not done yet.
- T2-7 (Band FX badge) not done: needs the list of band FX send params to decide when a badge shows.
- Changed files: ExoEQ.cpp, CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.


## T2-7 Band FX badge (2026-10-20)
- "Band FX" button reads "Band FX *" while any active band has delay, distortion, freq shift, sidechain or energy2 switched on. Updated on every band param change and on layout.
- Changed files: ExoEQ.cpp, ExoEQ.h, README.md, docs/DESIGN_ROADMAP.md.
- Uses a label change on a button only (no knob), so the knob recursion from earlier does not apply here.


## T2-5 curve hue, T2-9 differ marker (2026-10-20)
- T2-5: the selected circle's response curve and its spokes take the circle's hue (CircleHueColor of the mean band frequency). Row-to-node highlight was already in place; not changed.
- T2-9: A/B button adds " *" when the current settings differ from the other slot. Only checked when the other slot has been filled.
- T2-8 dashed dry line still not done.
- Changed files: CircleViewControl.h, ExoEQ.h, ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.


## T3-1 ghost curves (2026-10-20)
- Other circles' response curves are drawn faint and dashed behind the selected circle. Capped at 4 ghosts.
- Ghost draws do not touch the crosshair data (mLastResponseBands), so the readout still reads the selected circle.
- Weak-device note: each ghost re-gathers and samples 140 points per redraw. If the graph lags with 4 ghosts, the next step is caching ghost paths until those circles change.
- Changed files: CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.


## T3-3 (trial) stage marks on the selected node (2026-10-20)
- For stepped morph types (phaser, stage filters, comb, partials), the selected node gets n small dots fanned out along its radial line, 20 px out. Active stage is white.
- MorphStepsForType moved from ExoEQ.cpp to the new MorphSteps.h so the circle view can read it. Same logic.
- Trial item: judge it on screen. If it's noise, drop it and I'll remove the block.
- Changed files: MorphSteps.h (new), ExoEQ.cpp, CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.


## T3-4 status hint line (2026-10-20)
- Bottom-right hint now changes with the pointer: over the circle rail it says how to select and scroll; with a node selected it lists drag, tag typing and move; otherwise it says click empty graph for a new band.
- Changed files: CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.
- Hints describe behaviour already in the code. If a hint is wrong, tell me which gesture it misdescribes.


## T4-1 load sweep (2026-10-20)
- The selected circle's curve draws outward on open, in about 15 redraws. Self-driven: the control re-marks itself dirty until the sweep finishes. No timer or OnIdle used.
- Finding: the depth-stack ease already works this way, so a framework timer is not needed for motion.
- Ghost curves are not swept; they show at once.
- Changed files: CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.


## T4-1 load sweep fix (2026-10-20)
- The sweep now advances in IsDirty(), which IGraphics calls once per display refresh before collecting dirty rects. The earlier version re-dirtied from inside Draw(), which the redraw loop could drop, so the sweep never showed.
- Changed files: CircleViewControl.h, README.md, docs/DESIGN_ROADMAP.md.
- Not seen on screen yet. If it still doesn't show, the next check is whether the control is created after the editor opens or kept from before.


## Crash on reopening the GUI (2026-10-20)
- Access violation at ExoEQ.vst3 offset 0x5CB2A (read of 0x138) after closing and reopening the editor.
- Cause: mCircleView kept pointing at the destroyed circle view. OnParamChangeUI and state save/load then used the freed control.
- Fix: mCircleView is cleared in OnUIClose and at the start of layout.
- Known gap, not fixed here: user presets live inside the circle view, so saving plugin state while the GUI is closed stores no user presets. Needs a cache. Tell me if you see preset loss.
- Changed files: ExoEQ.cpp, README.md, docs/DESIGN_ROADMAP.md.
