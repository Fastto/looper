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
  std::vector<float> out;   // left channel of everything rendered
  std::vector<float> outR;  // right channel

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
      outR.push_back(o[1]);
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


void testNoClicksAnywhere() {
  std::printf("no output jumps: clear, play, loop start, monitoring, click mute\n");
  // Largest sample-to-sample jump in out[from, to).
  auto maxJump = [](const std::vector<float>& out, size_t from, size_t to) {
    float worst = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < out.size(); ++i) {
      worst = std::max(worst, std::fabs(out[i] - out[i - 1]));
    }
    return worst;
  };
  auto steady = [](int64_t) { return 0.8f; };
  auto silence = [](int64_t) { return 0.0f; };

  Rig rig;
  // 1. A loop holding a steady 0.8 starts playing at the end of its take:
  //    it rises over 2 ms (2 frames here) instead of jumping to 0.8.
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, steady);                       // the tap starts the count-in
  rig.run(kBar - rig.position(), steady);   // next frame: the loop's first
  size_t mark = rig.out.size();
  rig.run(10, silence);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);
  CHECK(rig.out[mark] > 0.0f && rig.out[mark] < 0.8f);
  CHECK(std::fabs(rig.out[mark + 5] - 0.8f) < 1e-6f);
  CHECK(maxJump(rig.out, mark - 1, mark + 10) < 0.8f * 0.6f);

  // 2. Play from stopped: the same short rise.
  rig.send(CommandType::Stop);
  rig.run(kStopFrames, silence);
  rig.send(CommandType::Play);
  mark = rig.out.size();
  rig.run(10, silence);
  CHECK(rig.out[mark] < 0.8f);
  CHECK(maxJump(rig.out, mark - 1, mark + 10) < 0.8f * 0.6f);

  // 3. Clearing a playing track fades it out over 10 ms, then empties it;
  //    a tap pressed meanwhile waits and records into the cleared track.
  rig.run(100, silence);
  rig.send(CommandType::TrackClear, 0);
  rig.send(CommandType::TrackTap, 0);
  mark = rig.out.size();
  rig.run(5, silence);
  CHECK(rig.status().tracks[0].state == LOOPER_TRACK_PLAYING);  // still fading
  CHECK(rig.status().tracks[0].busy == 1);
  rig.run(10, silence);
  CHECK(std::fabs(rig.out[mark + 9]) < 1e-6f);
  CHECK(maxJump(rig.out, mark - 1, mark + 12) < 0.8f * 0.3f);
  CHECK(rig.status().tracks[0].state != LOOPER_TRACK_EMPTY);  // the queued tap re-armed it
  CHECK(rig.status().tracks[0].layer_state == LOOPER_LAYER_NONE);

  // A muted (silent) track clears at once.
  Rig quiet;
  quiet.send(CommandType::TrackLengthMode, 0, 2);
  quiet.send(CommandType::TrackTap, 0);
  quiet.run(2 * kBar + 10, steady);
  quiet.send(CommandType::TrackMute, 0, 1);
  quiet.run(200, silence);
  quiet.send(CommandType::TrackClear, 0);
  quiet.run(1, silence);
  CHECK(quiet.status().tracks[0].state == LOOPER_TRACK_EMPTY);

  // 4. Software monitoring ramps in when a track is armed and out when the
  //    take ends (the steady input is on the monitored channel).
  Rig mon;
  mon.send(CommandType::SetMonitoring, 0, 1);
  mon.send(CommandType::TrackLengthMode, 0, 2);
  mon.send(CommandType::TrackTap, 0);
  mark = mon.out.size();
  mon.run(40, steady);
  CHECK(mon.out[mark] > 0.0f && mon.out[mark] < 0.8f * 0.3f);
  CHECK(std::fabs(mon.out[mark + 39] - 0.8f) < 1e-2f);
  CHECK(maxJump(mon.out, mark - 1, mark + 40) < 0.8f * 0.3f);
  mon.run(kBar - mon.position(), steady);
  mark = mon.out.size();
  mon.run(60, steady);  // the loop takes over while the monitor fades out
  CHECK(mon.status().tracks[0].state == LOOPER_TRACK_PLAYING);
  CHECK(maxJump(mon.out, mark - 1, mark + 60) < 0.8f * 0.6f);
  CHECK(std::fabs(mon.out[mark + 59] - 0.8f) < 1e-2f);  // loop only; monitor gone

  // 5. Muting the click mid-tick fades it.
  Rig click;
  click.send(CommandType::SetMetronome, 0, 1, 1.0);
  click.send(CommandType::Play);
  click.run(30, silence);
  click.send(CommandType::SetMetronome, 0, 0, 1.0);
  mark = click.out.size();
  click.run(60, silence);
  CHECK(std::fabs(click.out[mark]) > 1e-4f);  // not cut dead
  CHECK(std::fabs(click.out[mark + 59]) < 1e-3f);
}


void testQuantizedMute() {
  std::printf("quantized mute waits for the bar/beat, forgives late presses, can be cancelled\n");
  Rig rig;
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  auto steady = [](int64_t) { return 0.8f; };
  auto runTo = [&](int64_t t) { rig.run(t - rig.position(), steady); };
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);
  runTo(kBar + 10);
  rig.send(CommandType::SetMuteQuantize, 0, 2);  // bar

  runTo(kBar + 1500);
  rig.send(CommandType::TrackMute, 0, 1);
  runTo(kBar + 1600);
  CHECK(rig.status().tracks[0].pending_mute == 1);
  CHECK(rig.status().tracks[0].muted == 0);
  CHECK(std::fabs(rig.out.back() - 0.8f) < 1e-6f);  // still playing
  runTo(2 * kBar + 100);
  CHECK(rig.status().tracks[0].muted == 1);
  CHECK(rig.status().tracks[0].pending_mute == -1);
  CHECK(std::fabs(rig.out.back()) < 1e-3f);

  // Late press (100 frames after the bar line, tolerance 250): right away.
  runTo(3 * kBar + 100);
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, steady);
  CHECK(rig.status().tracks[0].muted == 0);
  CHECK(rig.status().tracks[0].pending_mute == -1);

  // Beat mode, and cancelling: tap twice before the beat leaves it playing.
  rig.send(CommandType::SetMuteQuantize, 0, 1);
  runTo(3 * kBar + 1500);
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, steady);
  CHECK(rig.status().tracks[0].pending_mute == 1);
  rig.send(CommandType::TrackTap, 0);
  rig.run(1, steady);
  CHECK(rig.status().tracks[0].pending_mute == -1);
  runTo(3 * kBar + 2100);
  CHECK(rig.status().tracks[0].muted == 0);
  // ...and once more, well past the tolerance: mutes on the next beat (3 * kBar + 3000).
  runTo(3 * kBar + 2500);
  rig.send(CommandType::TrackTap, 0);
  runTo(3 * kBar + 2990);
  CHECK(rig.status().tracks[0].muted == 0);
  runTo(3 * kBar + 3010);
  CHECK(rig.status().tracks[0].muted == 1);
}

void testPan() {
  std::printf("pan: unity in the centre, the far side fades out\n");
  Rig rig;
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);
  rig.run(2 * kBar + 10, [](int64_t) { return 0.8f; });
  CHECK(std::fabs(rig.out.back() - 0.8f) < 1e-6f && std::fabs(rig.outR.back() - 0.8f) < 1e-6f);
  rig.send(CommandType::TrackPan, 0, 0, -1.0);
  rig.run(100, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out.back() - 0.8f) < 1e-6f);
  CHECK(std::fabs(rig.outR.back()) < 1e-3f);
  rig.send(CommandType::TrackPan, 0, 0, 0.5);
  rig.run(100, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out.back() - 0.8f * 0.70710678f) < 1e-3f);
  CHECK(std::fabs(rig.outR.back() - 0.8f) < 1e-6f);
  CHECK(rig.status().tracks[0].pan == 0.5f);
}

void testOverdubFeedback() {
  std::printf("overdub feedback decays the loop each pass; undo restores it\n");
  Rig rig;
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  auto runTo = [&](int64_t t, float in) { rig.run(t - rig.position(), [in](int64_t) { return in; }); };
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);
  runTo(2 * kBar, 0.8f);
  rig.send(CommandType::SetFeedback, 0, 0, 0.5);
  rig.send(CommandType::TrackOverdub, 0);
  runTo(4 * kBar, 0.0f);  // two silent passes
  rig.send(CommandType::TrackOverdub, 0);
  runTo(4 * kBar + 20, 0.0f);
  CHECK(rig.status().feedback == 0.5f);
  std::vector<float> buf(static_cast<size_t>(kBar));
  CHECK(rig.engine.exportLoop(0, buf.data(), kBar) == kBar);
  CHECK(std::fabs(buf[2000] - 0.2f) < 1e-6f);  // 0.8 * 0.5 * 0.5
  rig.send(CommandType::TrackUndo, 0);
  runTo(4 * kBar + 40, 0.0f);
  CHECK(rig.engine.exportLoop(0, buf.data(), kBar) == kBar);
  CHECK(std::fabs(buf[2000] - 0.8f) < 1e-6f);
  rig.send(CommandType::TrackRedo, 0);
  runTo(4 * kBar + 60, 0.0f);
  CHECK(rig.engine.exportLoop(0, buf.data(), kBar) == kBar);
  CHECK(std::fabs(buf[2000] - 0.2f) < 1e-6f);
}

void testLatencyCalibration() {
  std::printf("calibration finds the round trip through a loopback\n");
  const int64_t roundTrip = 37;
  Rig rig;
  uint32_t noise = 12345;
  auto loopback = [&](int64_t d) {
    noise = noise * 1664525u + 1013904223u;
    const float hiss = (static_cast<float>(noise >> 8) / 16777216.0f - 0.5f) * 0.01f;
    const auto i = static_cast<size_t>(d - roundTrip);
    return (d >= roundTrip && i < rig.out.size() ? rig.out[i] * 0.3f : 0.0f) + hiss;
  };
  int32_t frames = -1;
  CHECK(rig.engine.calibrationResult(&frames) == 0);  // nothing yet
  rig.send(CommandType::CalibrationStart, 0, 0);
  rig.run(100, loopback);
  CHECK(rig.status().calibration == 1);
  rig.run(4000, loopback);
  CHECK(rig.status().calibration == 2);
  CHECK(rig.engine.calibrationResult(&frames) == 1);
  CHECK(frames == roundTrip);

  // Without a loopback there is nothing to find.
  rig.send(CommandType::CalibrationStart, 0, 0);
  rig.run(4100, [&](int64_t) { return 0.0f; });
  CHECK(rig.engine.calibrationResult(&frames) == -1);

  // Refused while playing.
  rig.send(CommandType::Play);
  rig.send(CommandType::CalibrationStart, 0, 0);
  rig.run(10, [](int64_t) { return 0.0f; });
  CHECK(rig.status().calibration == 2);  // unchanged, not measuring
}


void testTrim() {
  std::printf("trim: shift the start by eighths, cut/extend whole bars, pad with silence, lock on layer\n");
  Rig rig;
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  const int64_t d0 = rig.deviceFrame - 1;  // transport t <-> device d0 + t
  const int64_t eighth = kBar / 8;         // 4/4: an eighth note is 500 frames here
  // The take: bar 1 silent (picking up the instrument), bars 2-5 played with a
  // hit on each downbeat and a softer one an eighth later, and a stray noise
  // early in bar 6 that made the loop one bar too long.
  auto player = [&](int64_t d) {
    const int64_t m = d - d0;
    if (m >= kBar && m < 5 * kBar && m % kBar == 0) return 1.0f;
    if (m >= kBar && m < 5 * kBar && m % kBar == eighth) return 0.5f;
    if (m == 5 * kBar + 100) return 0.3f;
    return 0.0f;
  };
  rig.send(CommandType::TrackTap, 0);  // late press at transport 1: starts at bar 1
  rig.run(5 * kBar + 1000 - rig.position(), player);
  rig.send(CommandType::TrackTap, 0);  // stop well into bar 6
  rig.run(6 * kBar + 10 - rig.position(), player);
  const LooperTrackStatus* ts = &rig.status().tracks[0];
  CHECK(ts->state == LOOPER_TRACK_PLAYING);
  CHECK(ts->length == 6 * kBar);
  CHECK(ts->trim_locked == 0 && ts->trim_eighths == 0);
  CHECK(ts->trim_min_eighths < 0);  // the pre-roll allows starting earlier
  CHECK(ts->trim_max_bars == 6);

  std::vector<float> loop(static_cast<size_t>(rig.engine.capacity()));
  auto exportLoop = [&] { return rig.engine.exportLoop(0, loop.data(), static_cast<int64_t>(loop.size())); };

  // Cut the silent bar: start one bar (8 eighths) later. Length stays 6 bars;
  // the end is padded with silence.
  rig.send(CommandType::TrackShiftStart, 0, 8);
  rig.run(10, player);
  CHECK(rig.status().tracks[0].trim_eighths == 8);
  CHECK(exportLoop() == 6 * kBar);
  CHECK(std::fabs(loop[0] - 1.0f) < 1e-6f);
  CHECK(std::fabs(loop[eighth] - 0.5f) < 1e-6f);
  CHECK(std::fabs(loop[3 * kBar] - 1.0f) < 1e-6f);
  CHECK(std::fabs(loop[4 * kBar + 100] - 0.3f) < 1e-6f);  // the stray, now in bar 5
  CHECK(std::fabs(loop[5 * kBar + 100]) < 1e-6f);         // bar 6: silence padding

  // Remove the two extra bars at the end.
  rig.send(CommandType::TrackTrimBars, 0, -1);
  rig.send(CommandType::TrackTrimBars, 0, -1);
  rig.run(10, player);
  CHECK(rig.status().tracks[0].length == 4 * kBar);
  CHECK(exportLoop() == 4 * kBar);
  CHECK(std::fabs(loop[0] - 1.0f) < 1e-6f && std::fabs(loop[3 * kBar] - 1.0f) < 1e-6f);
  // Nothing is lost: one bar can come back (it still holds the stray), not more.
  CHECK(rig.status().tracks[0].trim_max_bars == 5);
  rig.send(CommandType::TrackTrimBars, 0, 1);
  rig.send(CommandType::TrackTrimBars, 0, 1);  // out of range: ignored
  rig.run(10, player);
  CHECK(rig.status().tracks[0].length == 5 * kBar);
  rig.send(CommandType::TrackTrimBars, 0, -1);
  rig.run(10, player);

  // Fine shift: start an eighth earlier, so the downbeat hit plays an eighth late.
  rig.send(CommandType::TrackShiftStart, 0, -1);
  rig.run(10, player);
  CHECK(exportLoop() == 4 * kBar);
  CHECK(std::fabs(loop[0]) < 1e-6f);
  CHECK(std::fabs(loop[eighth] - 1.0f) < 1e-6f);
  rig.send(CommandType::TrackShiftStart, 0, 1);
  rig.send(CommandType::TrackShiftStart, 0, -1000);  // out of range: ignored
  rig.run(10, player);
  CHECK(rig.status().tracks[0].trim_eighths == 8);

  // Playback follows the window: the hit sounds on every downbeat.
  const int64_t t0 = (rig.position() / (4 * kBar) + 1) * 4 * kBar;  // next cycle start
  rig.run(t0 + 4 * kBar - rig.position(), [](int64_t) { return 0.0f; });
  const size_t base = rig.out.size() - static_cast<size_t>(rig.position());
  CHECK(std::fabs(rig.out[base + static_cast<size_t>(t0)] - 1.0f) < 1e-3f);
  CHECK(std::fabs(rig.out[base + static_cast<size_t>(t0 + 2 * kBar + eighth)] - 0.5f) < 1e-3f);

  // Half-bar loops cannot change length.
  Rig half;
  half.send(CommandType::TrackLengthMode, 1, 1);
  half.send(CommandType::TrackTap, 1);
  half.run(kBar + kBar / 2 + 10, [](int64_t) { return 0.1f; });
  CHECK(half.status().tracks[1].trim_max_bars == 0);

  // The first layer locks the trim (and keeps the audio exactly as heard).
  CHECK(exportLoop() == 4 * kBar);
  std::vector<float> before(loop.begin(), loop.begin() + 4 * kBar);
  rig.send(CommandType::TrackOverdub, 0);
  rig.run(5, [](int64_t) { return 0.0f; });
  rig.send(CommandType::TrackUndo, 0);
  rig.run(5, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[0].trim_locked == 1);
  rig.send(CommandType::TrackShiftStart, 0, 1);
  rig.send(CommandType::TrackTrimBars, 0, -1);
  rig.run(10, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[0].trim_eighths == 8 && rig.status().tracks[0].length == 4 * kBar);
  rig.send(CommandType::Stop);
  rig.run(kStopFrames, [](int64_t) { return 0.0f; });
  CHECK(exportLoop() == 4 * kBar);
  float worst = 0.0f;
  for (int64_t i = 0; i < 4 * kBar; ++i) worst = std::max(worst, std::fabs(loop[i] - before[i]));
  CHECK(worst < 1e-6f);
  std::vector<float> rec(loop.size());
  CHECK(rig.engine.exportRecording(0, rec.data(), static_cast<int64_t>(rec.size())) == -2);
}

void testTrimSeamAndLiveCrossfade() {
  std::printf("trim keeps the seam continuous and crossfades when trimmed while playing\n");
  Rig rig;
  const double step = 2 * 3.14159265358979 * 7.3 / kRate;
  auto sine = [&](int64_t d) { return static_cast<float>(std::sin(static_cast<double>(d) * step)); };
  rig.send(CommandType::TrackLengthMode, 0, 4);  // two bars
  rig.send(CommandType::TrackTap, 0);
  rig.run(3 * kBar + 10, sine);
  std::vector<float> loop(static_cast<size_t>(2 * kBar));
  const float maxStep = static_cast<float>(step) * 1.05f;

  for (int shift : {3, -2, 5}) {  // trims while playing
    rig.send(CommandType::TrackShiftStart, 0, shift);
    const size_t mark = rig.out.size();
    rig.run(200, sine);
    float worst = 0.0f;
    for (size_t i = mark; i < mark + 200; ++i) worst = std::max(worst, std::fabs(rig.out[i] - rig.out[i - 1]));
    CHECK(worst < 0.5f);  // a 5 ms crossfade, not a jump to another part of the wave
    CHECK(rig.engine.exportLoop(0, loop.data(), 2 * kBar) == 2 * kBar);
    CHECK(std::fabs(loop[2 * kBar - 1] - loop[0]) < maxStep);  // the seam stays smooth
  }
}

void testRecordingRoundTrip() {
  std::printf("a trimmed recording survives export + import with its window\n");
  Rig rig;
  auto content = [](int64_t d) { return static_cast<float>((d * 7919) % 1000) / 1000.0f - 0.5f; };
  rig.send(CommandType::TrackLengthMode, 0, 6);  // three bars
  rig.send(CommandType::TrackTap, 0);
  rig.run(4 * kBar + 10, content);
  rig.send(CommandType::TrackShiftStart, 0, 3);
  rig.send(CommandType::TrackTrimBars, 0, -1);
  rig.run(10, content);
  const LooperTrackStatus ts = rig.status().tracks[0];
  std::vector<float> rec(rig.engine.capacity());
  const int64_t frames = rig.engine.exportRecording(0, rec.data(), static_cast<int64_t>(rec.size()));
  CHECK(frames == ts.audio_end);
  std::vector<float> a(static_cast<size_t>(ts.length)), b(static_cast<size_t>(ts.length));
  CHECK(rig.engine.exportLoop(0, a.data(), ts.length) == ts.length);

  CHECK(rig.engine.importLoop(5, rec.data(), frames, frames, false, ts.origin, ts.trim_eighths,
                              ts.length) == 0);
  rig.run(1, content);
  const LooperTrackStatus& loaded = rig.status().tracks[5];
  CHECK(loaded.state == LOOPER_TRACK_PLAYING && loaded.trim_locked == 0);
  CHECK(loaded.trim_eighths == 3 && loaded.length == 2 * kBar);
  CHECK(rig.engine.exportLoop(5, b.data(), ts.length) == ts.length);
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::fabs(a[i] - b[i]));
  CHECK(worst < 1e-6f);
  // ...and it can still be trimmed after loading.
  rig.send(CommandType::TrackShiftStart, 5, -3);
  rig.run(10, content);
  CHECK(rig.status().tracks[5].trim_eighths == 0);
}


void testNormGain() {
  std::printf("normalization gain scales the loop smoothly; a new take or clear resets it\n");
  Rig rig;
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackVolume, 0, 0, 0.5);
  rig.send(CommandType::TrackTap, 0);
  rig.run(2 * kBar + 10, [](int64_t) { return 0.2f; });
  CHECK(std::fabs(rig.out.back() - 0.1f) < 1e-6f);  // 0.2 x volume 0.5
  rig.send(CommandType::TrackNormGain, 0, 0, 4.0);
  const size_t mark = rig.out.size();
  rig.run(100, [](int64_t) { return 0.0f; });
  CHECK(std::fabs(rig.out.back() - 0.4f) < 1e-4f);  // x 4
  float worst = 0.0f;
  for (size_t i = mark; i < rig.out.size(); ++i) worst = std::max(worst, std::fabs(rig.out[i] - rig.out[i - 1]));
  CHECK(worst < 0.1f);  // ramped, not stepped
  CHECK(rig.status().tracks[0].norm_gain == 4.0f);
  rig.send(CommandType::TrackClear, 0);
  rig.run(50, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[0].norm_gain == 1.0f);
}


void testTrimKeepsMusicInPlace() {
  std::printf("cutting bars at the start keeps the music where it was played; nudges move it; reset\n");
  Rig rig;
  rig.send(CommandType::Play);
  rig.run(1, [](int64_t) { return 0.0f; });
  const int64_t d0 = rig.deviceFrame - 1;
  const int64_t eighth = kBar / 8;
  // Recorded from bar 1 (transport 0): silent first bar, a hit on the bar-2
  // downbeat and a softer one on the bar-4 downbeat; stopped in bar 5.
  auto player = [&](int64_t d) {
    const int64_t m = d - d0;
    if (m == kBar) return 1.0f;
    if (m == 3 * kBar) return 0.5f;
    return 0.0f;
  };
  rig.send(CommandType::TrackTap, 0);
  rig.run(4 * kBar + 1000 - rig.position(), player);
  rig.send(CommandType::TrackTap, 0);
  rig.run(5 * kBar + 10 - rig.position(), player);
  CHECK(rig.status().tracks[0].length == 5 * kBar);
  CHECK(rig.status().tracks[0].rec_anchor == 0);

  // Where does the loud hit sound, over two cycles from `from`?
  auto hitTimes = [&](int64_t from, int64_t cycle) {
    rig.run(from + 2 * cycle - rig.position(), [](int64_t) { return 0.0f; });
    std::vector<int64_t> times;
    const int64_t first = rig.position() - 2 * cycle;
    const size_t base = rig.out.size() - static_cast<size_t>(2 * cycle);
    for (int64_t i = 0; i < 2 * cycle; ++i) {
      if (rig.out[base + static_cast<size_t>(i)] > 0.9f) times.push_back(first + i);
    }
    return times;
  };

  // Cut the silent bar (start boundary +1 bar), then the empty last bar.
  rig.send(CommandType::TrackShiftStartBars, 0, 1);
  rig.send(CommandType::TrackTrimBars, 0, -1);
  rig.run(10, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[0].start_bars == 1);
  CHECK(rig.status().tracks[0].length == 4 * kBar);
  CHECK(rig.status().tracks[0].rec_start == kBar);  // the loop now begins on bar 2
  // The hit still sounds on bar-2 downbeats of the 4-bar cycle: kBar mod 4 bars.
  {
    const auto hits = hitTimes(8 * kBar, 4 * kBar);
    CHECK(hits.size() == 2);
    for (int64_t t : hits) CHECK(floorMod(t, 4 * kBar) == kBar);
  }
  CHECK(hitTimes(16 * kBar, 4 * kBar).size() == 2);

  // A nudge does move the music: start an eighth earlier = the hit an eighth later.
  rig.send(CommandType::TrackShiftStart, 0, -1);
  rig.run(10, [](int64_t) { return 0.0f; });
  {
    const auto hits = hitTimes(24 * kBar, 4 * kBar);
    CHECK(hits.size() == 2);
    for (int64_t t : hits) CHECK(floorMod(t, 4 * kBar) == kBar + eighth);
  }
  rig.send(CommandType::TrackResetTrim, 0);
  rig.run(10, [](int64_t) { return 0.0f; });
  const LooperTrackStatus& r = rig.status().tracks[0];
  CHECK(r.start_bars == 0 && r.trim_eighths == 0 && r.length == 5 * kBar && r.rec_start == 0);
  {
    const auto hits = hitTimes(40 * kBar, 5 * kBar);
    CHECK(hits.size() == 2);
    for (int64_t t : hits) CHECK(floorMod(t, 5 * kBar) == kBar);
  }
  std::vector<float> loop(static_cast<size_t>(2 * kBar), 0.0f);
  loop[0] = 1.0f;
  CHECK(rig.engine.importLoop(3, loop.data(), 2 * kBar, 2 * kBar, true, 0, 0, 0, 2 * kBar) == 0);
  rig.send(CommandType::TrackMute, 0, 1);
  rig.run(10, [](int64_t) { return 0.0f; });
  CHECK(rig.status().tracks[3].rec_start == 2 * kBar);
  {
    const auto hits = hitTimes(60 * kBar, 2 * kBar);
    CHECK(hits.size() == 2);
    for (int64_t t : hits) CHECK(floorMod(t, 2 * kBar) == 0);
  }
}

void testInputMonitoring() {
  std::printf("input monitoring: off, auto (armed/recording only), on (always), with level\n");
  auto steady = [](int64_t) { return 0.4f; };
  Rig rig;
  rig.run(100, steady);
  CHECK(std::fabs(rig.out.back()) < 1e-6f);  // off by default
  rig.send(CommandType::SetInputMonitor, 0, 2, 0.5);  // In 1: on at half level
  rig.run(100, steady);
  CHECK(std::fabs(rig.out.back() - 0.2f) < 1e-3f);  // heard while stopped, no track
  CHECK(rig.status().input_monitor_mode[0] == 2);
  CHECK(std::fabs(rig.status().input_monitor_volume[0] - 0.5f) < 1e-6f);
  rig.send(CommandType::SetInputMonitor, 0, 1, 1.0);  // auto: nothing armed yet
  rig.run(100, steady);
  CHECK(std::fabs(rig.out.back()) < 1e-3f);
  rig.send(CommandType::TrackLengthMode, 0, 2);
  rig.send(CommandType::TrackTap, 0);  // armed during the count-in
  rig.run(100, steady);
  CHECK(std::fabs(rig.out.back() - 0.4f) < 1e-3f);
  // Another input on "on" doesn't hear In 1.
  rig.send(CommandType::SetInputMonitor, 0, 0, 1.0);
  rig.send(CommandType::SetInputMonitor, 1, 2, 1.0);
  rig.run(100, steady);
  CHECK(std::fabs(rig.out.back()) < 1e-3f);
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
  testNoClicksAnywhere();
  testQuantizedMute();
  testPan();
  testOverdubFeedback();
  testLatencyCalibration();
  testTrim();
  testTrimSeamAndLiveCrossfade();
  testRecordingRoundTrip();
  testNormGain();
  testTrimKeepsMusicInPlace();
  testInputMonitoring();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all engine tests passed\n");
  return 0;
}
