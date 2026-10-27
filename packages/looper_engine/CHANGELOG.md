## 0.6.0

* Trim: loops are windows over the recording (with up to a bar of pre-roll); shift the start by eighths, cut/extend whole bars, silence padding, live crossfade; locked by the first layer.
* Seam crossfade at playback time while trimmable; recordings export/import with their window.
* Per-track normalization gain (smoothed; reset by a new take or clear).

## 0.5.0

* Quantized mute/unmute (beat or bar) with late-press tolerance and cancel.
* Per-track pan with a balance-style law; stereo output.
* Overdub feedback, undoable (decay stored in the layer).
* Latency calibration through a loopback: pulses + cross-correlation.

## 0.4.0

* Loop seam crossfade: the last 8 ms fade into the audio played just before the downbeat.
* Stop fades the output out over 10 ms and waits for latency-delayed layer tails; queued commands run after.
* Clearing a playing track fades it out; loops fade in over 2 ms when they start; software monitoring and the click are smoothed.

## 0.3.0

* Export/import of loops for sessions; per-track `busy` flag and command acknowledgement counter.

## 0.2.0

* Overdub with punch fades and one-level undo/redo per track (lazy, chunked layer merge).

## 0.1.0

* Initial engine: transport, 8 quantized loop tracks, metronome, latency compensation.
