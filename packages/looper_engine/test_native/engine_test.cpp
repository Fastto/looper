// Offline tests for the looper engine: drive Engine::process() with synthetic
// input and check sample positions exactly. Run: see test_native/README.md.
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "../src/engine.hpp"
#include "../src/grid.hpp"

using namespace looper;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

// Tiny sample rate keeps the numbers readable: 60 BPM, 4/4 → bar = 4000.
constexpr int kRate = 1000;
constexpr int64_t kBar = 4000;

struct Rig {
  Engine engine;
  int64_t deviceFrame = 0;
  std::vector<float> out;  // mono copy of everything rendered

  explicit Rig(int64_t latency = 0) : engine(makeConfig()) {
    send(CommandType::SetTempo, 0, 4, 60.0);
    send(CommandType::SetMetronome, 0, 0, 0.0);
    send(CommandType::SetLatency, 0, static_cast<int>(latency));
    run(1, [](int64_t) { return 0.0f; });
  }

  static EngineConfig makeConfig() {
    EngineConfig c;
    c.sampleRate = kRate;
    c.inputChannels = 2;
    c.outputChannels = 2;
    c.maxLoopSeconds = 60.0;
    return c;
  }

  void send(CommandType type, int track = 0, int i = 0, double d = 0.0) {
    Command c;
    c.type = type;
    c.track = track;
    c.i = i;
    c.d = d;
    engine.send(c);
  }

  // Renders `frames` frames one at a time so commands land on exact frames.
  void run(int64_t frames, const std::function<float(int64_t)>& input) {
    for (int64_t n = 0; n < frames; ++n) {
      float in[2] = {input(deviceFrame), 0.0f};
      float o[2] = {0.0f, 0.0f};
      engine.process(in, o, 1);
      out.push_back(o[0]);
      ++deviceFrame;
    }
  }

  const LooperStatus& status() { return engine.status(); }
  int64_t position() { return status().position; }
};

void testGrid() {
  std::printf("grid math\n");
  CHECK(barFrames(48000, 120, 4) == 96000);
  CHECK(barFrames(44100, 113, 4) % 2 == 0);
  CHECK(floorDiv(-1, 4000) == -1);
  CHECK(floorMod(-1, 4000) == 3999);
  const int64_t tol = timingTolerance(kBar, 4);  // 250
  CHECK(tol == 250);
  CHECK(quantizedStart(0, kBar, tol) == 0);
  CHECK(quantizedStart(200, kBar, tol) == 0);      // late press, forgiven
  CHECK(quantizedStart(300, kBar, tol) == kBar);   // waits for next bar
  CHECK(quantizedStart(3900, kBar, tol) == kBar);  // early press waits
  CHECK(autoLoopLength(1000, kBar, tol) == kBar / 2);
  CHECK(autoLoopLength(2200, kBar, tol) == kBar / 2);
  CHECK(autoLoopLength(2300, kBar, tol) == kBar);
  CHECK(autoLoopLength(4100, kBar, tol) == kBar);      // just after bar 1
  CHECK(autoLoopLength(4300, kBar, tol) == 2 * kBar);  // clearly into bar 2
  CHECK(autoLoopLength(11000, kBar, tol) == 3 * kBar);
}

void testRecordFromStoppedWithCountIn() {
  std::printf("record from stopped transport with count-in\n");
  Rig rig;
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, [](int64_t) { return 0.0f; });
  CHECK(rig.status().playing == 1);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_ARMED);
  CHECK(rig.status().tracks[0].rec_start == 0);
  CHECK(rig.position() == -kBar + 1);

  // Play an impulse on beat 2 of the first recorded bar (transport 1000).
  const int64_t startDevice = rig.deviceFrame - 1 + kBar;  // device frame at transport 0
  auto impulse = [&](int64_t d) { return d == startDevice + 1000 ? 1.0f : 0.0f; };
  rig.run(kBar - 1, impulse);
  CHECK(rig.position() == 0);
  rig.run(10, impulse);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_RECORDING);

  // Stop half-way through bar 2 → rounds up to 2 bars.
  rig.run(kBar + 1500 - 10, impulse);
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, impulse);
  CHECK(rig.status().tracks[0].rec_end == 2 * kBar);
  rig.run(2 * kBar + 1 - rig.position(), impulse);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.status().tracks[0].length == 2 * kBar);

  // The impulse must come back at transport 2*kBar + 1000 and 4*kBar + 1000.
  const size_t base = rig.out.size() - static_cast<size_t>(rig.position());  // index of transport 0
  rig.run(2 * kBar + 1500, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out[base + 2 * kBar + 1000] - 1.0f) < 1e-4f);
  CHECK(std::fabs(rig.out[base + 2 * kBar + 999]) < 1e-6f);
  CHECK(std::fabs(rig.out[base + 4 * kBar + 1000] - 1.0f) < 1e-4f);
}

void testLatencyCompensation() {
  std::printf("latency compensation\n");
  const int64_t latency = 37;
  Rig rig(latency);
  rig.send(CommandType::TrackLengthMode, 0, 2);  // fixed 1 bar
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, [](int64_t) { return 0.0f; });
  const int64_t startDevice = rig.deviceFrame - 1 + kBar;
  // The player hits on beat 3 (transport 2000); the engine hears it
  // `latency` frames later.
  auto impulse = [&](int64_t d) { return d == startDevice + 2000 + latency ? 1.0f : 0.0f; };
  rig.run(kBar + kBar + 100, impulse);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.status().tracks[0].length == kBar);
  const size_t base = rig.out.size() - static_cast<size_t>(rig.position());
  rig.run(2 * kBar, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out[base + kBar + 2000] - 1.0f) < 1e-4f);
  CHECK(std::fabs(rig.out[base + 2 * kBar + 2000] - 1.0f) < 1e-4f);
}

void testLatePressStartsRetroactively() {
  std::printf("late press records retroactively from the bar line\n");
  Rig rig;
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  // Transport t ↔ device frame d0 + t.
  const int64_t d0 = rig.deviceFrame - 1;
  auto impulse = [&](int64_t d) { return d == d0 + kBar + 50 ? 1.0f : 0.0f; };
  rig.run(kBar + 200 - rig.position(), impulse);  // 200 frames into bar 2
  rig.send(CommandType::TrackLengthMode, 1, 2);
  rig.send(CommandType::TrackTap, 1);
  rig.run(1, impulse);
  CHECK(rig.status().tracks[1].state == LOOPER_TRACK_RECORDING);
  CHECK(rig.status().tracks[1].rec_start == kBar);
  rig.run(2 * kBar + 10 - rig.position(), impulse);
  CHECK(rig.status().tracks[1].state == LOOPER_TRACK_PLAYING);
  const size_t base = rig.out.size() - static_cast<size_t>(rig.position());
  rig.run(kBar, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out[base + 2 * kBar + 50] - 1.0f) < 1e-4f);
}

void testMuteAndClear() {
  std::printf("mute toggles, clear empties, tempo locked while holding audio\n");
  Rig rig;
  rig.send(CommandType::TrackLengthMode, 0, 1);  // half bar
  rig.send(CommandType::TrackTap, 0);
  rig.run(kBar + kBar / 2 + 10, [](int64_t) { return 0.5f; });
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.status().tracks[0].length == kBar / 2);
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[0].muted == 1);
  rig.run(200, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out.back()) < 1e-3f);  // faded out

  rig.send(CommandType::SetTempo, 0, 4, 90.0);
  rig.run(1, [](int64_t) { return 0.0f; });
  CHECK(rig.status().bpm == 60.0);  // locked
  rig.send(CommandType::TrackClear, 0);
  rig.send(CommandType::SetTempo, 0, 4, 90.0);
  rig.run(1, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_EMPTY);
  CHECK(rig.status().bpm == 90.0);
}

void testStopKeepsWholeBars() {
  std::printf("stopping transport mid-recording keeps whole bars\n");
  Rig rig;
  rig.send(CommandType::TrackTap, 2);
  rig.run(kBar + 2 * kBar + 700, [](int64_t) { return 0.25f; });
  CHECK(rig.status().tracks[2].state == LOOPER_TRACK_RECORDING);
  rig.send(CommandType::Stop);
  rig.run(1, [](int64_t) { return 0.0f; });
  CHECK(rig.status().playing == 0);
  CHECK(rig.status().tracks[2].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.status().tracks[2].length == 2 * kBar);

  // Stopping during the first bar throws the take away.
  rig.send(CommandType::TrackTap, 3);
  rig.run(kBar + 500, [](int64_t) { return 0.25f; });
  rig.send(CommandType::Stop);
  rig.run(1, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[3].state == LOOPER_TRACK_EMPTY);
}

}  // namespace

int main() {
  testGrid();
  testRecordFromStoppedWithCountIn();
  testLatencyCompensation();
  testLatePressStartsRetroactively();
  testMuteAndClear();
  testStopKeepsWholeBars();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all engine tests passed\n");
  return 0;
}
