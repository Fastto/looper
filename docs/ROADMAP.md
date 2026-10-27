# Roadmap

Status as of 2026-10-27. Design details for everything marked done are in
[DESIGN.md](DESIGN.md).

## Done

### v0.1 — MVP
- [x] C++ engine (miniaudio) with a sample-accurate transport and 8 loop tracks
- [x] Bar-quantized record start/stop, late-press tolerance, ½/1/2/4/8-bar fixed lengths
- [x] Count-in, audible + visual metronome; "Rec in 3, 2, 1" countdown also while playing
- [x] Device and per-track input channel selection, round-trip latency compensation
- [x] Keyboard shortcuts

### v0.2 — playing live
- [x] Overdub with one-level undo/redo per track; overdub feedback (undoable)
- [x] Quantized mute/unmute (next beat or bar), optional
- [x] Per-track pan (stereo output) and normalize (non-destructive, −1 dBFS peak)
- [x] Trim: cut/add bars at the start (music stays in place), ⅛ / ¼ timing nudge,
      cut/add bars at the end, reset; live crossfade; locked by the first layer
- [x] Hearing yourself: per-input monitoring (off / on / auto) with level, in the top bar
- [x] Automatic latency calibration (loopback pulses + cross-correlation)
- [x] No clicks anywhere: loop seam crossfade into the pre-roll, fades on stop, clear,
      loop start, monitoring and click mute; safe device change and quit

### v0.3 — sessions (part)
- [x] Sessions saved automatically (WAV + JSON, format v2), reopened on launch, browser
      (rename, new, duplicate, open, delete); trims and anchors survive reopening
- [x] Export stems and a stereo mix as WAV into a chosen folder

### Tooling
- [x] `make up / test / test-integration / release`; CI (engine tests, analysis, macOS build)
- [x] Engine unit tests (sample-exact, ASan/UBSan), Dart unit tests, end-to-end test on
      a real audio interface

## Next

### Before the first release
- [ ] Manual QA pass on a real instrument (see checklist below)
- [ ] Screenshots and a short GIF in the README (record → layer → trim)
- [ ] Release build of `Looper.app` attached to a GitHub Release (zip/dmg); signing and
      notarization (Apple Developer Program) so it opens without Gatekeeper warnings
- [ ] CONTRIBUTING.md, issue templates, a few `good first issue`s
- [ ] Social preview image; GitHub Discussions
- [ ] App icon

### v0.2 — remaining
- [ ] MIDI: foot-switch / controller mapping (MIDI learn) for hands-free play
- [ ] Output routing to more than two channels

### v0.3 — remaining
- [ ] Import samples (WAV reader already supports 16/24/32-bit and stereo)
- [ ] Free-tempo mode: the first loop sets the tempo
- [ ] Tempo change with time-stretching (needs a real-time stretcher; mind its licence)

### v0.4 — arranging
- [ ] Scenes (groups of loops) and a song timeline built from recorded clips
- [ ] Effects per track (filter, reverb, delay)
- [ ] Polymeter (loops of different lengths drifting on purpose) as an opt-in

### Platforms
- [x] macOS
- [ ] iOS / iPadOS (AVAudioSession setup, Bluetooth/USB interfaces)
- [ ] Android (AAudio / Oboe-class latency)
- [ ] Windows (WASAPI exclusive / ASIO), Linux (ALSA/JACK/PipeWire)

## Manual QA checklist

Things the automated tests can't judge by ear or by eye:

- [ ] Record → close loop → layer → undo/redo on a real instrument; no clicks at the seam
- [ ] Trim: cut a silent first bar with step `bar` — the loop stays in time with the others;
      nudge with `⅛`; cut a stray last bar; `↺` reset; trim while playing
- [ ] Two loops recorded from different bars stay aligned after quitting and reopening
- [ ] Input monitoring On / Auto / Off and its level; latency feels acceptable at buffer 64/128
- [ ] Measure latency with a loopback cable; loops land on the grid afterwards
- [ ] Quantized mute (beat/bar), pan, normalize (`N`), feedback
- [ ] Export stems + mix and open them in a DAW: aligned from bar 1, correct pans
- [ ] Change the audio device while loops play: they come back after the switch
- [ ] Layout at small window sizes (track cards, top bar wrapping)
