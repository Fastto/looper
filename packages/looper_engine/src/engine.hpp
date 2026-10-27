// The looper engine: transport, tracks, metronome. Pure DSP, no device I/O,
// so it can be driven offline by tests. See docs/DESIGN.md.
#pragma once

#include <array>
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
  TrackOverdub,
  TrackUndo,
  TrackRedo,
  TrackMute,
  TrackVolume,
  TrackInput,
  TrackLengthMode,
  TrackLoad,  // d = loop length; audio was written by importLoop()
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
  // Each track holds two buffers (base + overdub layer) of this length.
  double maxLoopSeconds = 48.0;
};

class Engine {
 public:
  static constexpr int kMaxTracks = LOOPER_MAX_TRACKS;
  static constexpr int kMaxInputs = LOOPER_MAX_INPUTS;

  // Allocates all buffers. Not real-time safe; call before audio starts.
  explicit Engine(const EngineConfig& config);

  // UI thread: queue a command. Returns false if the queue is full.
  bool send(const Command& cmd) {
    if (!commands_.push(cmd)) return false;
    ++sent_;
    return true;
  }

  // UI thread: commands not yet applied, according to the latest status.
  int64_t commandsPending() { return static_cast<int64_t>(sent_) - status_.read().commands_handled; }

  size_t capacity() const { return capacity_; }

  // UI thread: flattened copy of a loop. Returns frames, 0 if none, -1 if busy.
  int64_t exportLoop(int track, float* out, int64_t capacity);

  // UI thread: loads audio into an empty track. 0 = ok, -1 busy, -2 bad length.
  int importLoop(int track, const float* data, int64_t frames, int64_t targetFrames);

  // Audio thread: interleaved float buffers. `input` may be null.
  void process(const float* input, float* output, uint32_t frames);

  // UI thread: latest published status.
  const LooperStatus& status() { return status_.read(); }

  // UI thread: loop peaks for display. Only meaningful for PLAYING tracks.
  int waveform(int track, float* out, int bins);

  const EngineConfig& config() const { return config_; }

 private:
  // Overdub layers are merged into the base lazily, one chunk at a time,
  // the first time playback or recording touches a chunk (see commitChunk).
  static constexpr int kChunkShift = 8;
  // Upper bound for the loop-seam crossfade (8 ms even at 192 kHz).
  static constexpr int kMaxSeamFrames = 2048;
  static constexpr int kGenerationHistory = 256;

  struct Track {
    std::vector<float> buffer;  // base: the recording plus merged layers
    std::vector<float> layer;   // the latest overdub, kept apart for undo
    LooperTrackState state = LOOPER_TRACK_EMPTY;
    int64_t recStart = 0;
    int64_t recEnd = -1;   // -1 while open-ended
    int64_t length = 0;
    // Recording writes lag the transport by the latency, so writing has its
    // own lifetime that can outlast the RECORDING state by a few ms.
    bool writing = false;
    bool writeStarted = false;
    int64_t written = 0;
    // The audio just before the downbeat, captured when recording starts and
    // crossfaded into the end of the loop when it is complete (closeSeam).
    std::array<float, kMaxSeamFrames> preroll{};
    int prerollFrames = 0;
    int inputChannel = 0;
    int lengthMode = 0;  // 0 = auto, N = fixed N half-bars
    bool muted = false;
    float volume = 1.0f;
    float gain = 0.0f;
    float peak = 0.0f;

    // overdub
    LooperLayerState layerState = LOOPER_LAYER_NONE;
    float layerGain = 0.0f;
    bool overdubbing = false;  // between the start and stop presses
    bool odWriting = false;    // writes lag by the latency, like recording
    int64_t odStart = 0;       // transport frames
    int64_t odEnd = -1;
    int32_t layerVersion = 0;
    // Layer generations: chunkGen[c] is the generation whose audio chunk c
    // of `layer` holds. A chunk from an older generation is merged into the
    // base (if that layer was active when superseded) and zeroed on first use.
    uint32_t generation = 0;
    uint32_t floorGeneration = 0;  // older chunks hold stale audio
    std::vector<uint32_t> chunkGen;
    bool supersededActive[kGenerationHistory] = {};
    int64_t uncommitted = 0;  // chunks of the loop not yet on the current generation
  };

  void handle(const Command& cmd);
  void tap(Track& t);
  void resetLayer(Track& t);
  void beginPlaying(Track& t);
  // Whether sample `i` of `layer` is audible (generation-aware, see commitChunk).
  bool layerAudible(const Track& t, size_t i, LooperLayerState state) const;
  void startOverdub(Track& t);
  void stopOverdub(Track& t);
  void undo(Track& t);
  void redo(Track& t);
  void commitChunk(Track& t, size_t index);
  void writeOverdub(Track& t, int64_t musicalPos, float sample);
  void startTransport(int64_t position);
  void stopTransport();
  void finishOnStop(Track& t);
  bool anyTrackHoldsAudio() const;
  void recomputeGrid();
  void processFrame(const float* in, float* out);
  void writeRecording(Track& t, int64_t musicalPos, float sample);
  void closeSeam(Track& t);
  float metronomeSample();
  void publishStatus();

  float historyAt(int channel, int64_t deviceFrame) const {
    return history_[static_cast<size_t>(channel) * historySize_ +
                    static_cast<size_t>(deviceFrame & historyMask_)];
  }

  EngineConfig config_;
  SpscQueue<Command, 256> commands_;
  uint64_t sent_ = 0;     // UI thread
  int64_t handled_ = 0;   // audio thread, published in the status
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

  int seamFrames_ = 0;
  float gainSmoothing_ = 0.0f;
  float meterDecay_ = 0.0f;
  float inputPeaks_[kMaxInputs] = {};
  int64_t blockCounter_ = 0;
};

}  // namespace looper
