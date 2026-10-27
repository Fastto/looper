# Looper — Design

This document describes how the looper behaves (UX) and how it is built
(architecture). It is the reference for contributors: if the code and this
document disagree, one of them is a bug.

## 1. Product goals

* **Live first.** You should be able to play with one hand on the instrument
  and the other on the keyboard / screen. Every action is one tap.
* **Never out of time.** The app owns the grid. Recordings start and end on
  bar boundaries, so loops cannot drift relative to each other.
* **Obvious state.** At any moment you can tell, from across the room, which
  track is waiting, recording, playing or muted, and where the beat is.
* **Real audio interfaces.** Pick the device and the exact input channel per
  track (guitar on In 1, mic on In 2). Low-latency native audio.
* **Open and portable.** Flutter UI + portable C++ engine. macOS is the first
  target; iOS, Android, Windows and Linux reuse the same engine.

Inspirations studied: Ableton Live Session view (clip launch quantization),
Loopy Pro (one-big-button tracks, retrospective start tolerance), Boss RC
loop stations (rec → play → overdub cycle, count-in), Elektron devices
(visual beat feedback).

## 2. Concepts

| Term | Meaning |
|------|---------|
| **Transport** | Global clock: tempo (BPM), time signature, play/stop, position. |
| **Bar / Beat** | Grid units derived from tempo. A bar is an integer number of samples (rounded to even so half a bar is also integral). |
| **Track** | A slot that holds one loop. MVP has 8 tracks. |
| **Loop length** | Always ½ bar or a whole number of bars. |
| **Input channel** | Which physical input of the audio interface a track records from. |

Tempo and time signature can only be changed while all tracks are empty
(changing them would require time-stretching — see Roadmap).

## 3. Track state machine

Each track has one big button. Tapping it does the "next obvious thing":

```
            tap                     boundary
  EMPTY ───────────▶ ARMED ───────────────────▶ RECORDING
    ▲                  │ tap (cancel)               │ tap = "stop at the next
    │                  ▼                            │ sensible bar line";
    └──────────────── EMPTY                         │ fixed-length mode stops
                                                    │ automatically
    clear (any state)                               ▼
  ◀───────────────────────────────────────────── PLAYING ◀──tap──▶ MUTED
                                                  │   ▲
                                 Layer / hold /   │   │ tap / Layer
                                 ⌥N               ▼   │
                                                 OVERDUB
```

* **EMPTY** — grey, shows "tap to record".
* **ARMED** — blinking red; a ring shows the count-down to the next bar.
* **RECORDING** — solid red; the ring fills once per bar, bar count shown.
* **PLAYING** — green; the ring is the loop's playhead; waveform shown.
* **MUTED** — dimmed; keeps playing silently so it re-enters in phase.
* **OVERDUB** — orange; you are recording a layer on top of the loop.

### 3.3 Overdub and undo

Overdub ("Layer") records on top of a playing loop. Unlike the first
recording it is **not quantized**: it punches in and out the moment you
press, with 4 ms fades so there are no clicks, and it may run over the loop
as many times as you like. Latency is compensated the same way as recording.

Each track keeps its newest layer apart from the rest of the loop, which
gives one level of **undo / redo** per track (like a loop-station pedal):

* Undo silences the last layer (5 ms fade); redo brings it back.
* Undo while overdubbing cancels the layer in progress.
* Starting a new layer freezes the previous one into the loop (if it was
  audible) and discards it if it was undone.
* `⌘Z` / `⇧⌘Z` undo / redo the most recently layered track.

How the engine does it without ever copying a whole loop on the audio thread:
the track has a `base` and a `layer` buffer, and playback mixes
`base + layer × layerGain`. Starting a new layer only bumps a *generation*
number. The layer buffer is split into 256-frame chunks, each tagged with the
generation it belongs to; the first time playback or recording touches a
chunk from an older generation, that chunk is merged into `base` (if its
layer was active) and zeroed. Playback visits every chunk once per loop
cycle, so the cost is spread evenly over one cycle at O(1) per frame.

Tapping a track while the transport is stopped starts the transport with a
one-bar count-in and records from bar 1.

### 3.1 Quantization rules (the "auto-adjust")

Let `bar` be the bar length and `tol` the timing tolerance (¼ beat).

* **Start.** If you tap within `tol` *after* a bar line, the recording starts
  at that bar line retroactively (audio is taken from an always-running input
  history buffer). Otherwise it starts at the next bar line. Late presses are
  forgiven, early presses wait.
* **Stop (Auto length).** After recording `r` samples:
  * `r ≤ ½ bar + tol` → loop is **½ bar**.
  * pressed within `tol` after a bar line → loop ends at that bar line.
  * otherwise → loop ends at the next bar line.
* **Fixed length.** A track can be set to ½, 1, 2, 4 or 8 bars. Recording
  stops by itself after that many bars: tap once and play.

### 3.2 Latency compensation

When you hear the click at time *t* and play along, the engine receives that
sound `input latency + output latency` later. The engine writes recorded
audio into the loop shifted back by that round-trip, so loops land exactly
on the grid. On macOS the round-trip is read from CoreAudio (device + stream
latency, safety offset, buffer size); a manual offset (ms) in Settings lets
you fine-tune it.

## 4. Metronome

* Audible click (accented downbeat) with its own volume; can be muted.
* **Visual metronome** is always on: a row of beat lamps in the top bar,
  downbeat in accent colour, plus a large flash on the downbeat. With the
  click muted, nothing extra goes into your audio.

## 5. Controls

### Screen

```
┌────────────────────────────────────────────────────────────────────┐
│ ▶/■   120 BPM [−][+][TAP]  4/4   ● ○ ○ ○   Bar 3.1   🔈 click   ⚙ │
├────────────────────────────────────────────────────────────────────┤
│ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐                │
│ │   (●)    │ │   (▶)    │ │   ( )    │ │   ( )    │   tracks 1–4   │
│ │ ~wave~~  │ │ ~wave~~  │ │          │ │          │                │
│ │ In1 Auto │ │ In2 2bar │ │ In1 Auto │ │ In1 Auto │                │
│ │ vol ─●── │ │ vol ──●─ │ │          │ │          │                │
│ │  M   ✕   │ │  M   ✕   │ │          │ │          │                │
│ │[Layer]↶ ↷│ │[Layer]↶ ↷│ │          │ │          │                │
│ └──────────┘ └──────────┘ └──────────┘ └──────────┘                │
│                    … tracks 5–8 …                                  │
└────────────────────────────────────────────────────────────────────┘
```

### Keyboard (desktop)

| Key | Action |
|-----|--------|
| `1`–`8` | Tap track (record / stop / stop layer / mute toggle) |
| `⌥`+`1`–`8` | Start / finish a layer (overdub) |
| `⌘Z` / `⇧⌘Z` | Undo / redo the last layer |
| `Shift`+`1`–`8` | Clear track |
| `Space` | Play / stop transport |
| `M` | Metronome click on/off |
| `T` | Tap tempo (when all tracks are empty) |

MIDI foot-switch mapping is on the roadmap.

## 6. Architecture

```
┌───────────── Flutter (Dart, UI isolate) ─────────────┐
│  LooperController (ChangeNotifier)                   │
│    • polls engine snapshot every frame (Ticker)      │
│    • sends commands                                  │
│  Widgets: TransportBar, TrackCard, SettingsSheet     │
└───────────────────────▲──────────────┬───────────────┘
                        │ dart:ffi     │
            snapshot    │              │ commands
┌───────────────────────┴──────────────▼───────────────┐
│  C API  (looper_api.h)                               │
│  Device (miniaudio duplex: CoreAudio/WASAPI/…)       │
│  Engine (pure C++, no I/O, unit-testable)            │
│   • SPSC lock-free command queue  (UI → audio)       │
│   • triple-buffer status snapshot (audio → UI)       │
│   • input history ring, 8 track buffers, metronome   │
└──────────────────────────────────────────────────────┘
```

Real-time rules for the audio callback (enforced by review):

* no allocation, no locks, no syscalls, no logging;
* all buffers are preallocated when the device opens;
* every state change happens on the audio thread at an exact sample, so
  the UI never races the engine.

`Engine` knows nothing about devices: tests drive `Engine::process()` with
synthetic input and assert on sample positions (`packages/looper_engine/test_native`).

## 7. Repository layout

```
app/                         Flutter application (UI)
packages/looper_engine/      FFI plugin
  src/                       C++ engine + C API (shared by all platforms)
  src/third_party/miniaudio  audio I/O (MIT-0 / public domain)
  lib/                       Dart bindings + typed wrapper
  test_native/               engine unit tests (CMake + ctest)
docs/                        design, roadmap
```
