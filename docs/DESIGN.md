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

### 3.3 The loop seam

When a loop wraps, its last sample is followed by its first, and the two
were never next to each other in the performance, so the wrap clicks. The
fix used by hardware loopers: when recording starts, the engine keeps the
few milliseconds played **just before** the downbeat (the pre-roll, taken
from the input history, so it works for late presses too). The last 8 ms of the
loop are crossfaded (equal-power) into that pre-roll — at playback time
while the loop can still be trimmed (so the seam follows the trim), and
baked into the audio when the trim locks (§3.5). The loop now ends exactly the way the performance led into
the downbeat, so the wrap is continuous. The downbeat itself is never
touched, so the attack of the first hit keeps its punch.

Overdub layers need nothing extra: a layer that runs across the seam is
recorded continuously in real time, and punch-in/out have their own fades.

**Stop** doesn't cut the output dead either. The output fades out over
10 ms (raised cosine), and the transport keeps running a little longer —
the round-trip latency plus the punch fade — so that a layer ended by Stop
still receives its latency-delayed tail and its punch-out fade. The
recordings are then closed as of the moment Stop was pressed. The UI shows
"stopped" at once; commands sent in those few tens of ms (Play, a new
recording) wait in the queue and run right after.

### 3.4 No clicks, anywhere

Every change that could make the output jump has an envelope. The rule for
new features: nothing may switch audio on or off in a single sample.

| Event | Envelope |
|-------|----------|
| Loop wraps | 8 ms crossfade of the tail into the pre-roll (§3.3) |
| Loop starts (take closed, Play, count-in, session load) | 2 ms raised-cosine fade-in — short, so the downbeat keeps its attack |
| Stop | 10 ms fade-out of the whole output; transport runs on until layer tails land |
| Clear a playing track | 10 ms fade-out, then clear |
| Mute / unmute, volume, undo / redo layer | 5 ms one-pole smoothing |
| Overdub punch-in / punch-out | 4 ms fades on the recorded layer |
| Software monitoring on / off (arm, take end, toggle) | 5 ms per input channel |
| Click mute / volume | 5 ms smoothing |
| Audio device change, quit | the app stops (with its fade) and waits before closing the device |

While a fade-out is running (Stop, clear), new commands wait in the queue
for those few ms, so they act on the state that follows.

### 3.5 Trim

Pressing record and then reaching for the instrument leaves a silent bar at
the start; a stray sound after the last bar makes the loop a bar too long.
Trim fixes both, on the waveform of each track:

| Control | Effect |
|---------|--------|
| left `‹` / `›` | Start the loop one step earlier / later. The length stays; the loop's new start plays on the downbeat. Past the end of the recording it is padded with silence. |
| step chip (`⅛` / `¼` / `bar`) | Step for the left pair (eighth note, quarter note, whole bar) |
| right `‹` / `›` | Remove / add back a whole bar at the end (only for loops of 2+ bars) |

Loop lengths stay whole bars, so trimmed loops never drift against the
others (no polymeter). Nothing is destroyed: the loop is a **window over the
recording** (`windowStart`, `length`), and the recording keeps up to a bar
of **pre-roll** before the original downbeat, copied from the input history
when recording starts — so `‹` can even recover a note played slightly
early. Frames past the recorded audio read as silence. Trimming while
playing crossfades from the old window over 5 ms.

The first layer **locks the trim**: layers are recorded against the loop
cycle as it is, so moving the window afterwards would split them at the
seam. On locking, the seam crossfade is baked into the audio and the
window is fixed. Sessions store the whole recording plus the window
(`trim: {originBars, eighths}`), so an untouched loop can still be trimmed
after reopening.

### 3.6 Overdub and undo

Overdub ("Layer") records on top of a playing loop. Unlike the first
recording it is **not quantized**: it punches in and out the moment you
press, with 4 ms fades so there are no clicks, and it may run over the loop
as many times as you like. Latency is compensated the same way as recording.

**Layer feedback** (Settings, 0–100 %, default 100 %) scales what is already
in the loop on every pass while layering, so older parts fade away the
longer you layer. The decay is written into the layer as a negative delta
(`layer += fade × (input + (feedback − 1) × (base + layer))`) rather than
into the base, so undo still restores the loop exactly as it was.

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

**Measure latency** (Settings → Timing) finds the real round trip: with a
cable from an output to an input (or a mic near the speakers), the engine
plays 8 short windowed 2 kHz bursts 300 ms apart and records the chosen
input. Each burst is located by cross-correlation with its known shape; a
burst counts only if its peak stands well above the correlation noise, and
at least 5 must agree within 1 ms. The median is the round trip, and
**Apply** sets the offset to what the system report was missing.

### 3.2.1 Quantized mute

With *Quantize mute* set to Beat or Bar, muting or unmuting a playing track
waits for the next beat or bar line (the button blinks meanwhile), so parts
drop in and out in time. The same late-press tolerance as recording
applies, and pressing again before the line cancels the change.

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
| `⌘E` | Export audio (stems / mix) |
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

## 7. Sessions

A session is everything on screen: tempo, time signature, per-track settings
and the loops. There is **no Save button** — like a hardware looper, nothing
you record is ever lost:

* Changes are written to disk shortly after they happen. A loop is written
  once it stops being recorded or layered (the engine reports a track as
  `busy` until then, including the one loop cycle in which older layers are
  merged).
* Quitting the app, hiding it, switching sessions and changing the audio
  device save first; changing the device then reloads the session.
* On launch the last session is reopened.
* A new session is only written once it holds a loop, so the list does not
  fill with empty sessions.

The sessions browser (folder button in the top bar) renames, duplicates,
deletes and opens sessions.

### On disk

```
<Application Support>/Sessions/2026-10-27_15-04-12/
  session.json      tempo, time signature, track settings, loop lengths, trim
  track-1.wav       mono 32-bit float: the whole recording (pre-roll included)
                    while the trim is open, else the loop with its layers mixed in
  track-3.wav
```

`session.json` (`"format": "looper-session"`, `"version": 2`; version 1 files
load as locked loops) stores each loop
length in **half-bars**, not frames, so a session recorded at 44.1 kHz loads
exactly in time on a 48 kHz interface: the engine resamples each loop to
`halfBars × bar / 2` frames. Undo history is not saved — layers are merged
into the loop. Files are written to `*.tmp` and renamed, so a crash never
leaves a half-written session. The WAV reader also accepts 16/24/32-bit PCM
and stereo files (mixed to mono), which is the basis for importing samples.

### Exporting audio

**Export audio…** (sessions browser, or `⌘E`) writes WAV files into a folder
the user picks, inside a new sub-folder named `<session> - <BPM> BPM`
(never overwriting an earlier export):

* **Stems** — `01 Track 1.wav`, … one per track at its volume.
* **Mix** — `Mix.wav`, all tracks summed as you hear them.
* Muted tracks are left out unless asked for.

Every file starts at bar 1 and has the same length, so they line up when
dropped into a DAW. The default length is the least common multiple of the
loop lengths — the shortest span after which every loop ends on a full
cycle (a 3-bar and a 4-bar loop give 12 bars) — falling back to the longest
loop when that would exceed 64 bars. Files are 32-bit float (no clipping
in the mix) at the session's sample rate; rendering runs in a background
isolate.

### Threading

Export and import run on the UI thread without locks. They are safe because
the UI thread is the only sender of commands: the engine compares the number
of commands sent with the number the audio thread reports as applied, and a
track's audio can otherwise only change while it is `busy`. An empty track's
buffer is never touched by the audio thread; the `TrackLoad` command that
follows an import publishes the written samples to it (release/acquire queue).

## 8. Repository layout

```
app/                         Flutter application (UI)
packages/looper_engine/      FFI plugin
  src/                       C++ engine + C API (shared by all platforms)
  src/third_party/miniaudio  audio I/O (MIT-0 / public domain)
  lib/                       Dart bindings + typed wrapper
  test_native/               engine unit tests (CMake + ctest)
docs/                        design, roadmap
```
