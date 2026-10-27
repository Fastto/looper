// The looper engine: transport, tracks, metronome. Pure DSP, no device I/O,
// so it can be driven offline by tests. See docs/DESIGN.md.
#pragma once

#include <cstdint>
#include <vector>

#include "looper_api.h"
#include "lockfree.hpp"

namespace looper {

enum class CommandType : uint8_t {
  Play,
  Stop,
  SetTempo,
  SetMetronome,
  SetMonitoring,
  SetCountIn,
  SetLatency,
  TrackTap,
  TrackClear,
  TrackMute,
  TrackVolume,
  TrackInput,
  TrackLengthMode,
};

struct Command {
  CommandType type = CommandType::Play;
  int32_t track = 0;
  int32_t i = 0;
  double d = 0.0;
};

struct EngineConfig {
  int sampleRate = 48000;
  int inputChannels = 2;
  int outputChannels = 2;
  double maxLoopSeconds = 60.0;
};

class Engine {
 public:
  static constexpr int kMaxTracks = LOOPER_MAX_TRACKS;
  static constexpr int kMaxInputs = LOOPER_MAX_INPUTS;

  // Allocates all buffers. Not real-time safe; call before audio starts.
  explicit Engine(const EngineConfig& config);

  // UI thread: queue a command. Returns false if the queue is full.
  bool send(const Command& cmd) { return commands_.push(cmd); }

  // Audio thread: interleaved float buffers. `input` may be null.
  void process(const float* input, float* output, uint32_t frames);

  // UI thread: latest published status.
  const LooperStatus& status() { return status_.read(); }

  // UI thread: loop peaks for display. Only meaningful for PLAYING tracks.
  int waveform(int track, float* out, int bins);

  const EngineConfig& config() const { return config_; }

 private:
  struct Track {
    std::vector<float> buffer;
    LooperTrackState state = LOOPER_TRACK_EMPTY;
    int64_t recStart = 0;
    int64_t recEnd = -1;   // -1 while open-ended
    int64_t length = 0;
    // Recording writes lag the transport by the latency, so writing has its
    // own lifetime that can outlast the RECORDING state by a few ms.
    bool writing = false;
    bool writeStarted = false;
    int64_t written = 0;
    int inputChannel = 0;
    int lengthMode = 0;  // 0 = auto, N = fixed N half-bars
    bool muted = false;
    float volume = 1.0f;
    float gain = 0.0f;
    float peak = 0.0f;
  };

  void handle(const Command& cmd);
  void tap(Track& t);
  void startTransport(int64_t position);
  void stopTransport();
  void finishOnStop(Track& t);
  bool anyTrackHoldsAudio() const;
  void recomputeGrid();
  void processFrame(const float* in, float* out);
  void writeRecording(Track& t, int64_t musicalPos, float sample);
  float metronomeSample();
  void publishStatus();

  float historyAt(int channel, int64_t deviceFrame) const {
    return history_[static_cast<size_t>(channel) * historySize_ +
                    static_cast<size_t>(deviceFrame & historyMask_)];
  }

  EngineConfig config_;
  SpscQueue<Command, 256> commands_;
  TripleBuffer<LooperStatus> status_;

  Track tracks_[kMaxTracks];
  size_t capacity_ = 0;  // frames per track buffer

  std::vector<float> history_;  // planar input history ring, per channel
  size_t historySize_ = 0;
  int64_t historyMask_ = 0;

  // transport
  bool playing_ = false;
  int64_t position_ = 0;
  int64_t deviceFrame_ = 0;
  double bpm_ = 120.0;
  int beatsPerBar_ = 4;
  int64_t bar_ = 0;
  int64_t tolerance_ = 0;
  int countInBars_ = 1;
  int64_t latency_ = 0;
  bool monitoring_ = false;

  // metronome
  bool metronomeOn_ = true;
  float metronomeVolume_ = 0.5f;
  int64_t lastBeat_ = INT64_MIN;
  float clickPhase_ = 0.0f;
  float clickFreq_ = 0.0f;
  float clickEnv_ = 0.0f;
  float clickDecay_ = 0.0f;

  float gainSmoothing_ = 0.0f;
  float meterDecay_ = 0.0f;
  float inputPeaks_[kMaxInputs] = {};
  int64_t blockCounter_ = 0;
};

}  // namespace looper
