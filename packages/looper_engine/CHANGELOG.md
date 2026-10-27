## 0.4.0

* Loop seam crossfade: the last 8 ms fade into the audio played just before the downbeat.
* Stop fades the output out over 10 ms and waits for latency-delayed layer tails; queued commands run after.

## 0.3.0

* Export/import of loops for sessions; per-track `busy` flag and command acknowledgement counter.

## 0.2.0

* Overdub with punch fades and one-level undo/redo per track (lazy, chunked layer merge).

## 0.1.0

* Initial engine: transport, 8 quantized loop tracks, metronome, latency compensation.
