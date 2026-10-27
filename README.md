# Looper

An open-source live looper for musicians. Set a tempo, tap a track, play your
instrument, and layer loops that are always in time.

* **One button per track**: tap to record, tap to close the loop, tap to mute.
* **Always in time**: recordings start and end on bar lines. Late taps are
  forgiven, loops snap to ½, 1, 2, 4 or 8 bars, and nothing drifts.
* **Built for audio interfaces**: pick the device and the input channel per
  track (guitar on In 1, mic on In 2). Round-trip latency is compensated.
* **Metronome you can see**: beat lamps and a downbeat flash, so you can mute
  the click and keep it out of your sound.
* **Native audio engine** in C++ ([miniaudio](https://miniaud.io)), UI in Flutter.
  macOS first; iOS, Android, Windows and Linux are on the roadmap.

## How to play

| | |
|---|---|
| Tap an empty track | Starts recording on the next bar (with a count-in if stopped) |
| Tap again | Closes the loop on the nearest sensible bar line, playback starts seamlessly |
| Set the length first (½ … 8) | Recording stops by itself: one tap and play |
| Tap a playing track | Mute / unmute (it keeps running in time) |
| **Layer** button (or hold the big button) | Overdub on top of the loop; tap again to finish |
| Undo / redo arrows | Remove / restore the last layer of that track |
| L–R slider | Pan the track (double-click to centre) |
| Bin icon | Clear the track |
| Folder button (top bar) | Sessions: rename, new, duplicate, open, delete |
| **Export audio…** (or `⌘E`) | Stems and a mix as WAV, aligned from bar 1, into a folder you choose |

Everything is **saved automatically** (WAV + JSON) and the last session
reopens when you start the app — there is no Save button to forget.

Keyboard: `1`–`8` tap track · `⌥`+`1`–`8` layer (overdub) · `⌘Z` / `⇧⌘Z`
undo / redo layer · `⌘E` export · `Shift`+`1`–`8` clear · `Space` play/stop · `M` click
on/off · `T` tap tempo.

Tempo and time signature are locked while any track holds audio.

In **Audio settings**: quantize mute to the beat or bar, set how much of the
loop survives each layer pass (feedback), and **Measure latency** with a
loopback cable for perfectly timed recordings.

## Build & run (macOS)

Requirements: Flutter 3.38+, Xcode, CocoaPods, CMake.

```sh
make up
```

Other tasks: `make test` (engine + Dart tests, analysis),
`make test-integration` (records and reopens a session on your audio
interface), `make release` (build
`Looper.app`), `make` to list everything.

On first launch macOS asks for microphone access, which the app needs to
read your audio interface. Open **Audio settings** (the sliders icon) to pick
your interface, check the input meters and fine-tune latency.

## Project layout

```
app/                     Flutter UI
packages/looper_engine/  C++ engine + Dart FFI bindings
docs/DESIGN.md           UX and architecture
docs/ROADMAP.md          what's next
```

Engine tests (no audio hardware needed):

```sh
cd packages/looper_engine
cmake -S test_native -B build/test_native && cmake --build build/test_native --target engine_test
./build/test_native/engine_test
```

## Contributing

Issues and PRs are welcome. Read [docs/DESIGN.md](docs/DESIGN.md) first. The
audio callback must stay real-time safe: no allocations, locks or I/O.

## License

MIT. miniaudio is public domain / MIT-0.
