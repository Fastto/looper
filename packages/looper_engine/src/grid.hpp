// Musical grid math: bar sizes and the quantization rules from docs/DESIGN.md §3.1.
#pragma once

#include <cmath>
#include <cstdint>

namespace looper {

inline int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

inline int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// Bar length in frames, rounded to an even number so half a bar is integral.
inline int64_t barFrames(double sampleRate, double bpm, int beatsPerBar) {
  const double exact = sampleRate * 60.0 / bpm * beatsPerBar;
  return static_cast<int64_t>(std::llround(exact / 2.0)) * 2;
}

// Late presses within this many frames after a bar line count as "on" it.
inline int64_t timingTolerance(int64_t bar, int beatsPerBar) {
  return bar / beatsPerBar / 4;  // a quarter of a beat
}

// Start of a recording requested at transport position `pos`.
inline int64_t quantizedStart(int64_t pos, int64_t bar, int64_t tolerance) {
  const int64_t last = floorDiv(pos, bar) * bar;
  return (pos - last <= tolerance) ? last : last + bar;
}

// Loop length for an auto-length recording stopped after `recorded` frames.
inline int64_t autoLoopLength(int64_t recorded, int64_t bar, int64_t tolerance) {
  const int64_t half = bar / 2;
  if (recorded <= half + tolerance) return half;
  const int64_t bars = recorded / bar;
  const int64_t rem = recorded % bar;
  if (bars >= 1 && rem <= tolerance) return bars * bar;
  return (bars + 1) * bar;
}

}  // namespace looper
