# qol — by Lux Cache

Quality-of-life modules for VCV Rack 2. Minimal panels, thoughtful defaults, and workflow tools that live outside the signal path.

## Modules

### grab 2

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

### jump (experimental)

![jump module](docs/jump.png)

Saved-view bookmarks for navigating large patches. Click the dot to arm, then `Cmd+1..9` saves the current scroll + zoom; not armed, `Cmd+1..9` jumps to that slot.

- 9 slots, per-patch persistence
- `Cmd+[` / `Cmd+]` back/forward through nav history
- Pulse-on-arrival so the eye can follow the jump
- One amber-pulse dot on the panel; `Esc` cancels arm

Filed as experimental: the view-restore has a known zoom drift on tight close-up saves (5% of viewport is baked into Rack's `zoomToBound` padding, which translates to ~5–17% zoom-out depending on how zoomed-in you were). Overview-scale bookmarks are near-perfect; re-saving from the landed view is an easy self-correct.

## Building

Requires the VCV Rack SDK. Set `RACK_DIR` to point at it:

```
make
make install        # copies into your local VCV plugins folder
```

## License

Proprietary — © Lux Cache.
