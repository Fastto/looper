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
// Stop fades out and waits for latency-delayed writes; this covers it.
constexpr int64_t kStopFrames = 64;

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
  rig.run(kStopFrames, [](int64_t) { return 0.0f; });
  CHECK(rig.status().playing == 0);
  CHECK(rig.status().tracks[2].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.status().tracks[2].length == 2 * kBar);

  // Stopping during the first bar throws the take away.
  rig.send(CommandType::TrackTap, 3);
  rig.run(kBar + 500, [](int64_t) { return 0.25f; });
  rig.send(CommandType::Stop);
  rig.run(kStopFrames, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[3].state == LOOPER_TRACK_EMPTY);
}


void testOverdubUndoRedo() {
  std::printf("overdub layers, undo/redo, lazy merge of older layers\n");
  const int64_t latency = 20;
  Rig rig(latency);
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  const int64_t d0 = rig.deviceFrame - 1;  // transport t <-> device d0 + t
  std::vector<int64_t> hits;               // musical positions of impulses played
  auto input = [&](int64_t d) {
    for (int64_t m : hits) {
      if (d == d0 + m + latency) return 1.0f;
    }
    return 0.0f;
  };
  auto runTo = [&](int64_t t) { rig.run(t - rig.position(), input); };
  auto outAt = [&](int64_t t) {
    return rig.out[rig.out.size() - static_cast<size_t>(rig.position()) + static_cast<size_t>(t)];
  };
  auto heard = [&](int64_t t) { return std::fabs(outAt(t) - 1.0f) < 1e-3f; };
  auto silent = [&](int64_t t) { return std::fabs(outAt(t)) < 1e-3f; };

  // Base loop: one bar with a hit on 1000.
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);
  hits.push_back(1000);
  runTo(2 * kBar);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);

  // Layer 1 adds a hit on 2000.
  rig.send(CommandType::TrackOverdub, 0);
  hits.push_back(2 * kBar + 2000);
  runTo(2 * kBar + 10);
  CHECK(rig.status().tracks[0].layer_state == LOOPER_LAYER_RECORDING);
  runTo(3 * kBar);
  rig.send(CommandType::TrackOverdub, 0);
  runTo(4 * kBar);
  CHECK(rig.status().tracks[0].layer_state == LOOPER_LAYER_ACTIVE);
  CHECK(heard(3 * kBar + 1000) && heard(3 * kBar + 2000));

  rig.send(CommandType::TrackUndo, 0);
  runTo(5 * kBar);
  CHECK(rig.status().tracks[0].layer_state == LOOPER_LAYER_UNDONE);
  CHECK(heard(4 * kBar + 1000) && silent(4 * kBar + 2000));

  rig.send(CommandType::TrackRedo, 0);
  runTo(6 * kBar);
  CHECK(heard(5 * kBar + 1000) && heard(5 * kBar + 2000));

  // Layer 2 (hit on 3000) freezes layer 1 into the base.
  rig.send(CommandType::TrackOverdub, 0);
  hits.push_back(6 * kBar + 3000);
  runTo(7 * kBar);
  rig.send(CommandType::TrackOverdub, 0);
  runTo(8 * kBar);
  CHECK(heard(7 * kBar + 1000) && heard(7 * kBar + 2000) && heard(7 * kBar + 3000));
  rig.send(CommandType::TrackUndo, 0);
  runTo(9 * kBar);
  CHECK(heard(8 * kBar + 1000) && heard(8 * kBar + 2000) && silent(8 * kBar + 3000));

  // A new overdub discards the undone layer 2 for good.
  rig.send(CommandType::TrackOverdub, 0);
  hits.push_back(9 * kBar + 500);
  runTo(10 * kBar);
  rig.send(CommandType::TrackTap, 0);  // the big button also ends an overdub
  runTo(11 * kBar);
  CHECK(rig.status().tracks[0].muted == 0);
  CHECK(heard(10 * kBar + 500) && heard(10 * kBar + 1000) && heard(10 * kBar + 2000) &&
        silent(10 * kBar + 3000));
  rig.send(CommandType::TrackUndo, 0);
  runTo(12 * kBar);
  CHECK(silent(11 * kBar + 500) && heard(11 * kBar + 1000) && heard(11 * kBar + 2000) &&
        silent(11 * kBar + 3000));

  // Undo during an overdub cancels it.
  rig.send(CommandType::TrackRedo, 0);
  rig.send(CommandType::TrackOverdub, 0);
  hits.push_back(12 * kBar + 1500);
  runTo(12 * kBar + 3000);
  rig.send(CommandType::TrackUndo, 0);
  runTo(14 * kBar);
  CHECK(rig.status().tracks[0].layer_state == LOOPER_LAYER_UNDONE);
  CHECK(heard(13 * kBar + 500) && silent(13 * kBar + 1500));

  // Re-recording the track starts clean.
  rig.send(CommandType::TrackClear, 0);
  rig.send(CommandType::TrackTap, 0);
  runTo(16 * kBar);
  CHECK(rig.status().tracks[0].layer_state == LOOPER_LAYER_NONE);
  CHECK(silent(15 * kBar + 500) && silent(15 * kBar + 1000) && silent(15 * kBar + 2000));
}


void testExportImport() {
  std::printf("export flattens audible layers; import loads in time; busy is respected\n");
  Rig rig;
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  const int64_t d0 = rig.deviceFrame - 1;
  std::vector<int64_t> hits;
  auto input = [&](int64_t d) {
    for (int64_t m : hits) {
      if (d == d0 + m) return 1.0f;
    }
    return 0.0f;
  };
  auto runTo = [&](int64_t t) { rig.run(t - rig.position(), input); };
  std::vector<float> buf(static_cast<size_t>(rig.engine.capacity()));
  auto exportTrack = [&](int track) {
    return rig.engine.exportLoop(track, buf.data(), static_cast<int64_t>(buf.size()));
  };

  CHECK(exportTrack(0) == 0);  // empty
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);
  CHECK(exportTrack(0) == -1);  // commands pending
  hits.push_back(1000);
  runTo(kBar / 2);
  CHECK(exportTrack(0) == -1);  // recording
  runTo(kBar + 10);
  CHECK(exportTrack(0) == kBar);
  CHECK(std::fabs(buf[1000] - 1.0f) < 1e-6f && std::fabs(buf[2000]) < 1e-6f);

  // Layer on 2000: busy while layering and until older chunks are merged.
  runTo(2 * kBar);
  rig.send(CommandType::TrackOverdub, 0);
  hits.push_back(2 * kBar + 2000);
  runTo(3 * kBar);
  CHECK(exportTrack(0) == -1);
  rig.send(CommandType::TrackOverdub, 0);
  runTo(3 * kBar + 100);
  CHECK(rig.status().tracks[0].busy == 0);  // a whole cycle passed while layering
  CHECK(exportTrack(0) == kBar);
  CHECK(std::fabs(buf[1000] - 1.0f) < 1e-6f && std::fabs(buf[2000] - 1.0f) < 1e-6f);

  rig.send(CommandType::TrackUndo, 0);
  runTo(3 * kBar + 200);
  CHECK(exportTrack(0) == kBar);
  CHECK(std::fabs(buf[1000] - 1.0f) < 1e-6f && std::fabs(buf[2000]) < 1e-6f);

  // A new layer stopped with the transport inside its first cycle: chunks are
  // left unmerged, and export must still see layer 1 (redone) plus layer 2.
  rig.send(CommandType::TrackRedo, 0);
  runTo(4 * kBar);
  rig.send(CommandType::TrackOverdub, 0);
  hits.push_back(4 * kBar + 500);
  runTo(4 * kBar + 800);
  rig.send(CommandType::Stop);
  rig.run(kStopFrames, input);
  CHECK(rig.status().playing == 0);
  CHECK(exportTrack(0) == kBar);
  CHECK(std::fabs(buf[500] - 1.0f) < 1e-6f && std::fabs(buf[1000] - 1.0f) < 1e-6f &&
        std::fabs(buf[2000] - 1.0f) < 1e-6f);

  // Import into track 1: two bars with a hit on 3000, plays from bar 1.
  std::vector<float> loop(static_cast<size_t>(2 * kBar), 0.0f);
  loop[3000] = 0.5f;
  CHECK(rig.engine.importLoop(0, loop.data(), 2 * kBar, 2 * kBar) == -1);  // not empty
  CHECK(rig.engine.importLoop(1, loop.data(), 2 * kBar, 2 * kBar) == 0);
  CHECK(rig.engine.importLoop(2, loop.data(), 2 * kBar, 2 * kBar) == -1);  // load pending
  rig.run(1, input);
  CHECK(rig.status().tracks[1].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.status().tracks[1].length == 2 * kBar);
  CHECK(rig.status().tracks[1].layer_state == LOOPER_LAYER_NONE);
  rig.send(CommandType::TrackMute, 0, 1);
  rig.send(CommandType::Play);
  rig.run(1, input);
  const size_t base = rig.out.size() - 1;  // transport 0
  rig.run(4 * kBar, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out[base + 3000] - 0.5f) < 1e-6f);
  CHECK(std::fabs(rig.out[base + 2 * kBar + 3000] - 0.5f) < 1e-6f);

  // Resampling: a 1-bar loop recorded at half the rate fills 1 bar here.
  std::vector<float> half(static_cast<size_t>(kBar / 2), 0.25f);
  CHECK(rig.engine.importLoop(2, half.data(), kBar / 2, kBar) == 0);
  rig.run(1, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[2].length == kBar);
  CHECK(exportTrack(2) == kBar);
  CHECK(std::fabs(buf[kBar - 1] - 0.25f) < 1e-6f);
  CHECK(rig.engine.importLoop(3, half.data(), kBar / 2, 0) == -2);
}


void testSeamCrossfade() {
  std::printf("loop seam is crossfaded into the pre-roll; the downbeat is untouched\n");
  const int64_t latency = 13;
  Rig rig(latency);
  // A sine whose period does not divide the loop: without a crossfade the
  // wrap from the last sample to the first would jump.
  const double freq = 7.3;  // Hz at the 1 kHz test rate
  const double step = 2 * 3.14159265358979 * freq / kRate;
  const int64_t d0 = rig.deviceFrame + kBar;  // device frame of transport 0 (after count-in)
  auto sine = [&](int64_t d) {
    // What the player plays at musical time m arrives `latency` frames later.
    return static_cast<float>(std::sin(static_cast<double>(d - latency - d0) * step));
  };
  rig.send(CommandType::TrackLengthMode, 0, 2);  // one bar
  rig.send(CommandType::TrackTap, 0);
  rig.run(kBar + kBar + 50, sine);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.status().tracks[0].busy == 0);

  std::vector<float> loop(static_cast<size_t>(kBar));
  CHECK(rig.engine.exportLoop(0, loop.data(), kBar) == kBar);
  auto played = [&](int64_t m) { return static_cast<float>(std::sin(static_cast<double>(m) * step)); };
  const float maxStep = static_cast<float>(step) * 1.05f;

  CHECK(std::fabs(loop[0] - played(0)) < 1e-5f);              // downbeat untouched
  CHECK(std::fabs(loop[kBar / 2] - played(kBar / 2)) < 1e-5f);  // body untouched
  const float rawJump = std::fabs(played(kBar - 1) - played(0));
  CHECK(rawJump > 10 * maxStep);                               // the test would catch a click
  CHECK(std::fabs(loop[kBar - 1] - loop[0]) < maxStep);        // ...but the seam is smooth
  // The last 8 ms (8 frames here) follow equal-power curves from the take
  // into the pre-roll, i.e. what was played just before the downbeat.
  const int fade = 8;
  for (int k = 0; k < fade; ++k) {
    const double x = (k + 0.5) / fade * 3.14159265358979 / 2;
    const float expected = static_cast<float>(played(kBar - fade + k) * std::cos(x) +
                                              played(-fade + k) * std::sin(x));
    CHECK(std::fabs(loop[kBar - fade + k] - expected) < 1e-5f);
  }
  CHECK(std::fabs(loop[kBar - fade - 1] - played(kBar - fade - 1)) < 1e-5f);  // fade starts there
  for (int64_t i = kBar - 20; i < kBar - 1; ++i) {
    CHECK(std::fabs(loop[i + 1] - loop[i]) < 3 * maxStep);     // and the fade itself is smooth
  }
}


void testStopFadesOut() {
  std::printf("stop fades the output out, lets layers punch out, then runs queued commands\n");
  const int64_t latency = 20;
  Rig rig(latency);
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  const int64_t d0 = rig.deviceFrame - 1;
  // Base loop: one bar of a steady 0.5.
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);
  rig.run(2 * kBar - rig.position(), [](int64_t) { return 0.5f; });
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);

  // Layer a steady 0.25 from bar 3, then press Stop 2000 frames in.
  rig.send(CommandType::TrackOverdub, 0);
  auto layer = [&](int64_t d) { return d >= d0 + 2 * kBar + latency ? 0.25f : 0.0f; };
  rig.run(2000, layer);
  CHECK(std::fabs(rig.out.back() - 0.5f) < 1e-6f);  // the base plays; the layer is not heard yet
  rig.send(CommandType::Stop);
  rig.send(CommandType::TrackTap, 1);  // pressed right after Stop: must wait for it
  rig.run(1, layer);
  CHECK(rig.status().playing == 0);   // the UI sees "stopped" at once
  const size_t fadeStart = rig.out.size() - 1;
  rig.run(kStopFrames, layer);

  // Output: a smooth fade over 10 ms (10 frames here), then silence.
  float previous = 0.5f;
  for (size_t i = fadeStart; i < fadeStart + 10; ++i) {
    CHECK(rig.out[i] <= previous + 1e-6f);
    CHECK(previous - rig.out[i] < 0.5f * 0.2f);
    previous = rig.out[i];
  }
  CHECK(std::fabs(rig.out[fadeStart + 9]) < 1e-6f);
  CHECK(std::fabs(rig.out[fadeStart + 20]) < 1e-6f);  // silent while the layer tail lands

  // The tap queued during the fade ran afterwards: a fresh recording with a
  // count-in, rather than one that the stop would have cancelled.
  CHECK(rig.status().tracks[1].state == LOOPER_TRACK_ARMED);
  CHECK(rig.status().playing == 1);
  CHECK(rig.status().position < 0);
  rig.send(CommandType::Stop);
  rig.run(kStopFrames, [](int64_t) { return 0.0f; });

  // The layer ends exactly at the Stop press with its punch-out fade, even
  // though its last 20 frames arrived after the press.
  std::vector<float> buf(static_cast<size_t>(kBar));
  CHECK(rig.engine.exportLoop(0, buf.data(), kBar) == kBar);
  CHECK(std::fabs(buf[1500] - 0.75f) < 1e-6f);
  CHECK(std::fabs(buf[1990] - 0.75f) < 1e-6f);
  CHECK(buf[1999] < 0.6f && buf[1999] > 0.5f);  // fading out
  CHECK(std::fabs(buf[2000] - 0.5f) < 1e-6f);   // base only from the press on
}

}  // namespace

int main() {
  testGrid();
  testRecordFromStoppedWithCountIn();
  testLatencyCompensation();
  testLatePressStartsRetroactively();
  testMuteAndClear();
  testStopKeepsWholeBars();
  testOverdubUndoRedo();
  testExportImport();
  testSeamCrossfade();
  testStopFadesOut();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all engine tests passed\n");
  return 0;
}
