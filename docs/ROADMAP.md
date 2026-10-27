# Roadmap

## v0.1 — MVP (current)
- [x] C++ engine with sample-accurate transport and 8 loop tracks
- [x] Bar-quantized record start/stop, late-press tolerance, ½/1/2/4/8-bar fixed lengths
- [x] Count-in, audible + visual metronome
- [x] Device and per-track input channel selection, latency compensation
- [x] Keyboard shortcuts

## v0.2 — playing live
- [x] Overdub (record on top of a playing loop) with one-level undo/redo per track
- [ ] Overdub feedback (fade older layers as new ones go on)
- [ ] Quantized mute/unmute (on next bar), optional
- [ ] Tiny crossfade at the loop seam to remove clicks
- [ ] Automatic latency calibration (loopback ping)
- [ ] MIDI: foot-switch / controller mapping (MIDI learn)
- [ ] Per-track pan, output routing

## v0.3 — sessions
- [x] Sessions saved automatically (WAV + JSON), reopened on launch, sessions browser
- [ ] Export stems and mixdown to a user-chosen folder
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
