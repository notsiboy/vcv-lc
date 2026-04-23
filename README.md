# qol — by Lux Cache

Quality-of-life modules for VCV Rack 2. Minimal panels, thoughtful defaults, and workflow tools that live outside the signal path.

## Modules

### grab 2

![grab 2 module](docs/grab2.png)

One recorder, every mode. Builds on `grab` (auto-triggered one-shot) and `take` (rolling retrospective buffer), stitches them into a single 4 HP panel, and adds a few tricks neither has alone. Both DSP paths run off one shared stereo input pair.

**Mode cycle** — the top LED cycles three modes. Click acts on a 2-second delay, softly flashing the pending colour, so an accidental click can't trip a one-shot mid-jam; cycle further during the countdown or wait for the commit.

- **off** — no automation; you drive recording manually
- **grab** (yellow) — auto-triggered one-shot. Opens a take on signal, closes on silence
- **snip** (pink) — silence-gated rolling buffer. When audio stops, the ring freezes (the waveform literally stops scrolling) and resumes on the next non-silent sample. Saves come out with quiet stretches elided, no editor required

**Rec button** — big dual-action centre button.
- Short click → toggles a manual force-record. Light turns red while recording (whether manual or mode-triggered)
- Long click (≥ 450 ms) → saves a take from the rolling buffer. Amber flash on fire, plus a hold-progress amber ramp so you know when it's about to trigger
- Outer ring goes red in sync with the LED core

**Peak meters + waveform** — L peak column on the left, take's vertical voice-memos waveform in the middle, R peak column on the right. A mono source lights both peak columns (signal mirrors into both sides) so it's visually clear you can record with just one cable patched.

**Other conveniences**
- **Save to sub folder** — one-click toggle that routes saves into `<outputDir>/<patch>_<dd>_<mm>_<yyyy>/`. Flipped off-on later gets a fresh date
- Separate filename prefixes for grab (`grab_NN.wav`) and take (`take_NN.wav`), shared output dir
- Mono recording: if only L (or only R) is patched, the file is written as a true mono WAV — not a stereo WAV with duplicate channels
- All of grab + take + snip settings (threshold, hangover, pre-roll, fade in/out, bit depth, normalise, buffer length, silence threshold…) available in the right-click menu

4 HP. Both standalone `grab` and `take` modules remain available — `grab 2` is the unified flagship.

### journal

![journal module](docs/journal.png)

Resizable text canvas with a real rich-text editor underneath. Document model follows ProseMirror's bones: blocks have a type (paragraph / heading / bullet / ordered / HR), formatting is metadata on bytes, and list markers are structural — they're rendered from block type + depth, never stored inside the text.

- Theme-aware canvas (light / dark / grey), shared across all Lux Cache modules
- Drag the bottom-right corner to resize (3–128 HP)
- Centred title field between the top screws
- Cmd+B / Cmd+I / Cmd+E for bold / italic / inline code — with **pending-mark** semantics (no selection = next character picks up the style, no markers to trip over)
- Cmd+Shift+] / [ cycles heading level (paragraph ↔ H3 … H1)
- Type `# ` at line start for a heading; `- `, `* `, `+ `, `→ `, `— `, `– ` for a bullet; `1. ` for ordered; `---` alone for a horizontal rule
- Bullet markers **preserve whatever character you typed** — lists can mix `-`, `*`, `+`, `→`, `—`, `–` freely, and each item remembers its own
- New lists auto-indent one level for visual breathing room; Shift+Tab to bring flush with the margin, Shift+Tab again to exit the list
- Enter on a non-empty list item continues at the same depth; Enter on an empty item outdents one level, and at depth 0 exits the list
- Ordered list numbers auto-derive from position — delete / reorder items and the numbering updates itself
- Tab / Shift+Tab indent and outdent inside a list; in a paragraph Tab inserts a literal tab, Shift+Tab falls through to Rack's focus-prev
- Visual-line arrow nav, word-level Alt+arrows, line Home/End, doc Cmd+↑/↓
- Cmd+A / C / X / V clipboard, Cmd+Z / Cmd+Shift+Z undo/redo with typing coalescing
- Click / shift-click / drag to select. Double-click selects a word, triple-click a block; dragging after either snaps the selection to word / block boundaries (Google Docs feel)
- Right-click menu: export as `.md` or `.txt`, insert horizontal rule, hide logo, theme picker
- Round-trips cleanly through markdown on save/load

### tidy

![tidy module](docs/tidy.png)

Selectively hide or fade individual modules and cables without touching global cable opacity.

- Picker mode — click any module in the rack to hide it or darken its panel
- Per-rule controls: cable opacity, module brightness, hide-connected-cables
- Per-cable-colour opacity so you can fade entire colour classes
- Preset slots that capture the current rule set
- Dark-mode overlay for "force this module to look dark"

### grab

![grab module](docs/grab.png)

Auto-triggered one-shot recorder. Listens for signal, captures the take, writes WAV. No arming cables, no gate inputs — it just records when audio's coming in.

- Stereo L/R inputs; captures mono if only one is connected
- Arm button on panel; records only when armed
- Threshold + hangover + pre-roll so attacks aren't clipped and small gaps don't end a take
- Min-take filter suppresses spurious click-triggered micro-files
- Right-click menu: threshold (dB), hangover (ms), pre-roll (ms), fade in/out (ms), max take length (s), normalise to 0 dB, bit depth (16 / 24 / 32-bit float), filename prefix, output directory, reveal folder
- Filenames auto-increment: `<prefix><NN>.wav`
- Written asynchronously on a background thread so the audio thread never touches disk

### take

![take module](docs/take.png)

Session-aware retrospective recorder. Pairs with `grab` as its opposite — `grab` starts recording when audio arrives; `take` is always quietly rolling a ring buffer of the last N seconds, so you can capture something *after* the fact. Solves the "that thing I played 30 seconds ago was perfect" problem.

- Stereo continuous ring buffer, 60 s default (adjustable 10–300 s in right-click)
- One panel button — click to freeze the last N seconds to WAV
- Voice-memos style vertical waveform on the panel: newest audio at the top, scrolls down, centred silhouette of the stereo peak
- Auto-named `<prefix><NN>.wav` files, asynchronous writer thread so the audio path never touches disk
- Right-click: buffer length, fade in/out, normalise to 0 dB, bit depth (16 / 24 / 32-bit float), filename prefix, output directory + picker, reveal folder
- 4 HP

### qmap

![qmap module](docs/qmap.png)

14 aux CV inputs that touch-map to any parameter in the rack — uMap-style. Arm a slot, click a knob anywhere in the patch, and that knob follows the CV on the matching jack.

- Touch-to-assign uses Rack's own touched-param mechanism: once a slot is armed, the next param you click on another module becomes its target. No dragging cables to phantom inputs.
- **qmap** master button at the top walks the arming cursor through slots 1 → 14 automatically — hit it once, touch-assign every jack in order. Clicking it again mid-sweep cancels. Each successful assign pushes an undo entry.
- Per-slot arm buttons flash amber while armed; when a slot is bound the dot tracks the incoming CV — the LED visibly breathes with whatever is driving the param. Right-click an arm button to clear its mapping.
- Right-click a jack for per-slot **Unipolar (0..10V) / Bipolar (-5..5V)** polarity plus draggable **Attenuator** (±2×) and **Offset** (±10V) sliders so you can trim or bias a CV without patching an inline scaler.
- Drop a **qmod** next to this module and every unconnected aux input auto-feeds from the matching qmod output. A centre dot appears on both modules' jacks while the link is active. A real cable always overrides the auto-feed. When flanked by qmods on both sides, a radio in the menu lets you favour left or right.
- Module context menu: mappings list (click a bound entry to clear, click an empty entry to arm), **Arm all (sequential)**, **Clear all mappings**, **Copy/Paste mappings** to clone a whole bank to another qmap, adjacency status, theme picker.
- Bindings persist across patch save/load via VCV's `ParamHandle` system, so reordering or duplicating target modules doesn't silently break the map.

4 HP, laid out as 7 rows × 2 columns of jacks with arm buttons tucked above each.

### qmod

![qmod module](docs/qmod.png)

14-channel modulation source with the same layout as qmap — drop them side-by-side and every qmod output lines up with the matching qmap aux input.

**Modes** — the master button at the top cycles five modes, each with its own LED colour. Picking a mode broadcasts it to every slot; per-slot LED buttons can be clicked to diverge so any mix of modes can run on the same bank.

- **Random triggers** (red) — stochastic trigger bursts, each slot fires at its own rate with ±50% jitter
- **Triggered S+H** (orange) — no internal clock; every trigger input rising edge resamples every slot, VCV Random-style
- **Smooth random** (cyan, default) — smootherstep-slewed random-target wander
- **Sample & hold** (purple) — classic free-running S+H at each slot's rate
- **LFO** (green) — selectable sine / triangle / square / saw

**Stagger** — Ochd-style log-spread of rates from slot 1 (fastest) to slot 14 (slowest). Toggle on/off, and dial the **Stagger spread** slider to tighten or stretch the slow-end multiplier. Turn stagger off and every slot runs at the global rate.

**Global controls** (inline in the right-click menu)
- **Global rate** — 0.01–10 Hz base speed
- **Smoothness** — double duty: shapes the random-target slew in smooth-random mode, and acts as a one-pole output slew limiter on S+H / triggered S+H / LFO so step changes glide. Random triggers bypass it so pulses stay crisp.

**Trigger / CV input** — single jack at the top; the right-click menu picks how it's read:
- **Trigger / resync** — rising edge resets phases, resamples S+H slots, fires random triggers
- **Gate (run/freeze)** — high = run, low = freeze and hold every output voltage
- **CV → rate / smoothness / mode** — accepts unipolar or bipolar (±5V) signals; while the cable drives the value, your menu-set base is preserved and restored the moment you unplug

**Per-jack right-click** — output range (unipolar 0..10V / 0..5V / 0..1V, bipolar ±10V / ±5V / ±1V), plus **Attenuator** (±2×) and **Offset** (±10V) for post-range conditioning.

**Other**: per-slot LED brightness tracks each output's CV (triggers flash and decay), **Copy/Paste settings** to clone a whole qmod to another, undo on mode cycles and paste, and a live status line showing whether a qmap is adjacent and which side is auto-feeding. Per-slot state persists in JSON.

4 HP. 14 outputs in 7×2 columns, title + master button + trigger input up top.

### capture

![capture module](docs/capture.png)

Two exports off one panel: **capture** takes a PNG of the rack, **scan** writes a markdown dependency inventory of the current patch. Fills two long-standing community gaps — the only built-in PNG path was a CLI-only `--screenshot` flag, and Rack buries plugin dependencies in the patch JSON as slugs only.

**capture** — one-click PNG of what's on screen

- Amber flash on successful save
- **Fit whole rack** by default — zooms out to frame every module before the shot, so you don't have to pre-navigate
- Brief settle delay so module panels redraw at the new zoom before capture (no stale cached renders)
- Hides its own module during the shot so the capture button isn't in the frame
- High-DPI native — retina framebuffer → retina PNG
- Files: `<prefix><NN>.png`, auto-indexed

**scan** — markdown dependency report for the current patch

- Walks every module, groups by plugin, writes `plugin name + version + per-module count`
- Install list with direct links to each plugin's page on library.vcvrack.com so recipients can go straight to the subscribe button
- Optionally copies the report to the clipboard on press (on by default) — "copied" confirmation fades in under the button
- Files: `<prefix><NN>.md`, auto-indexed, shares the output directory with capture

Right-click covers both sides: fit-all / hide-self / viewport-only toggles for capture, run-now and clipboard-copy for scan, separate filename prefixes, shared output directory + picker + reveal, dark mode.

3 HP.

## Building

Requires the VCV Rack SDK. Set `RACK_DIR` to point at it:

```
make
make install        # copies into your local VCV plugins folder
```

## License

Proprietary — © Lux Cache.
