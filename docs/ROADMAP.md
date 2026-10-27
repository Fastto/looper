# Roadmap

## v0.1 — MVP (current)
- [x] C++ engine with sample-accurate transport and 8 loop tracks
- [x] Bar-quantized record start/stop, late-press tolerance, ½/1/2/4/8-bar fixed lengths
- [x] Count-in, audible + visual metronome
- [x] Device and per-track input channel selection, latency compensation
- [x] Keyboard shortcuts

## v0.2 — playing live
- [x] Overdub (record on top of a playing loop) with one-level undo/redo per track
- [x] Overdub feedback (fade older layers as new ones go on)
- [x] Quantized mute/unmute (next beat or bar), optional
- [x] Crossfade at the loop seam into the pre-roll (no clicks, downbeat untouched)
- [x] Stop fades the output out and lets layers punch out cleanly
- [x] Trim: shift the loop start by ⅛ / ¼ / bar, cut or add whole bars at the end
- [x] No clicks anywhere: fades on clear, loop start, monitoring, click mute, device change and quit
- [x] Automatic latency calibration (loopback pulses + cross-correlation)
- [ ] MIDI: foot-switch / controller mapping (MIDI learn)
- [x] Per-track pan (stereo output, stereo mix export)
- [ ] Output routing to more than two channels

## v0.3 — sessions
- [x] Sessions saved automatically (WAV + JSON), reopened on launch, sessions browser
- [x] Export stems and mixdown to a user-chosen folder
- [ ] Import samples
- [ ] Free-tempo mode: the first loop sets the tempo
- [ ] Tempo change with time-stretching

## v0.4 — arranging
- [ ] Scenes (groups of loops) and a song timeline built from recorded clips
- [ ] Effects per track (filter, reverb, delay)

## Platforms
- [x] macOS
- [ ] iOS / iPadOS (AVAudioSession setup, Bluetooth/USB interfaces)
- [ ] Android (AAudio / Oboe-class latency)
- [ ] Windows (WASAPI exclusive / ASIO), Linux (ALSA/JACK/PipeWire)
