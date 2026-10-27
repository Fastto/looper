#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "grid.hpp"

namespace looper {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kPunchFadeSeconds = 0.004f;

size_t nextPowerOfTwo(size_t v) {
  size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

}  // namespace

Engine::Engine(const EngineConfig& config) : config_(config) {
  config_.inputChannels = std::clamp(config_.inputChannels, 0, kMaxInputs * 4);
  const double sr = config_.sampleRate;
  capacity_ = static_cast<size_t>(config_.maxLoopSeconds * sr);
  for (auto& t : tracks_) {
    t.buffer.assign(capacity_, 0.0f);
    t.layer.assign(capacity_, 0.0f);
    t.chunkGen.assign((capacity_ >> kChunkShift) + 1, 0);
  }

  // Two seconds of input history: enough for the late-press tolerance plus
  // the round-trip latency, which both reach back in time.
  historySize_ = nextPowerOfTwo(static_cast<size_t>(sr * 2.0));
  historyMask_ = static_cast<int64_t>(historySize_ - 1);
  const int usableInputs = std::min(config_.inputChannels, kMaxInputs);
  history_.assign(historySize_ * static_cast<size_t>(std::max(usableInputs, 1)), 0.0f);

  gainSmoothing_ = 1.0f - std::exp(-1.0f / (0.005f * static_cast<float>(sr)));
  meterDecay_ = std::exp(-1.0f / (0.3f * static_cast<float>(sr)));
  clickDecay_ = std::exp(-1.0f / (0.012f * static_cast<float>(sr)));
  recomputeGrid();
  publishStatus();
}

void Engine::recomputeGrid() {
  bar_ = barFrames(config_.sampleRate, bpm_, beatsPerBar_);
  tolerance_ = timingTolerance(bar_, beatsPerBar_);
}

bool Engine::anyTrackHoldsAudio() const {
  for (const auto& t : tracks_) {
    if (t.state != LOOPER_TRACK_EMPTY) return true;
  }
  return false;
}

void Engine::handle(const Command& cmd) {
  const bool validTrack = cmd.track >= 0 && cmd.track < kMaxTracks;
  Track* t = validTrack ? &tracks_[cmd.track] : nullptr;
  switch (cmd.type) {
    case CommandType::Play:
      if (!playing_) startTransport(0);
      break;
    case CommandType::Stop:
      if (playing_) stopTransport();
      break;
    case CommandType::SetTempo:
      if (!anyTrackHoldsAudio() && cmd.d >= 20.0 && cmd.d <= 300.0 && cmd.i >= 1 &&
          cmd.i <= 16) {
        bpm_ = cmd.d;
        beatsPerBar_ = cmd.i;
        recomputeGrid();
      }
      break;
    case CommandType::SetMetronome:
      metronomeOn_ = cmd.i != 0;
      metronomeVolume_ = std::clamp(static_cast<float>(cmd.d), 0.0f, 1.0f);
      break;
    case CommandType::SetMonitoring:
      monitoring_ = cmd.i != 0;
      break;
    case CommandType::SetCountIn:
      countInBars_ = std::clamp(cmd.i, 0, 4);
      break;
    case CommandType::SetLatency:
      latency_ = std::max(0, cmd.i);
      break;
    case CommandType::TrackTap:
      if (t) tap(*t);
      break;
    case CommandType::TrackClear:
      if (t) {
        t->state = LOOPER_TRACK_EMPTY;
        t->writing = false;
        t->length = 0;
        t->muted = false;
        resetLayer(*t);
      }
      break;
    case CommandType::TrackOverdub:
      if (t) t->overdubbing ? stopOverdub(*t) : startOverdub(*t);
      break;
    case CommandType::TrackLoad: {
      const auto length = static_cast<int64_t>(cmd.d);
      if (t && t->state == LOOPER_TRACK_EMPTY && length > 0 &&
          length <= static_cast<int64_t>(capacity_)) {
        resetLayer(*t);
        t->writing = false;
        t->recStart = 0;  // every loop is anchored to bar 1
        t->recEnd = length;
        t->length = length;
        beginPlaying(*t);
      }
      break;
    }
    case CommandType::TrackUndo:
      if (t) undo(*t);
      break;
    case CommandType::TrackRedo:
      if (t) redo(*t);
      break;
    case CommandType::TrackMute:
      if (t) t->muted = cmd.i != 0;
      break;
    case CommandType::TrackVolume:
      if (t) t->volume = std::clamp(static_cast<float>(cmd.d), 0.0f, 2.0f);
      break;
    case CommandType::TrackInput:
      if (t) {
        const int usable = std::min(config_.inputChannels, kMaxInputs);
        t->inputChannel = std::clamp(cmd.i, 0, std::max(usable - 1, 0));
      }
      break;
    case CommandType::TrackLengthMode:
      if (t) t->lengthMode = std::clamp(cmd.i, 0, 32);
      break;
  }
}

void Engine::tap(Track& t) {
  const int64_t maxLen = std::max<int64_t>(static_cast<int64_t>(capacity_) / bar_ * bar_, bar_ / 2);
  switch (t.state) {
    case LOOPER_TRACK_EMPTY: {
      if (!playing_) {
        startTransport(-countInBars_ * bar_);
        t.recStart = 0;
      } else {
        t.recStart = quantizedStart(position_, bar_, tolerance_);
      }
      t.recEnd = t.lengthMode > 0
                     ? t.recStart + std::min<int64_t>(t.lengthMode * (bar_ / 2), maxLen)
                     : -1;
      t.length = 0;
      resetLayer(t);
      t.writing = true;
      t.writeStarted = false;
      t.written = 0;
      t.muted = false;
      t.state = position_ >= t.recStart ? LOOPER_TRACK_RECORDING : LOOPER_TRACK_ARMED;
      break;
    }
    case LOOPER_TRACK_ARMED:
      t.state = LOOPER_TRACK_EMPTY;
      t.writing = false;
      break;
    case LOOPER_TRACK_RECORDING: {
      const int64_t len =
          std::min(autoLoopLength(position_ - t.recStart, bar_, tolerance_), maxLen);
      if (t.recEnd < 0 || t.recStart + len < t.recEnd) t.recEnd = t.recStart + len;
      if (position_ >= t.recEnd) {
        t.length = t.recEnd - t.recStart;
        beginPlaying(t);
      }
      break;
    }
    case LOOPER_TRACK_PLAYING:
      if (t.overdubbing) {
        stopOverdub(t);
      } else {
        t.muted = !t.muted;
      }
      break;
  }
}

void Engine::beginPlaying(Track& t) {
  t.state = LOOPER_TRACK_PLAYING;
  t.gain = t.muted ? 0.0f : t.volume;  // no fade-in: keep the downbeat's attack
  t.uncommitted = (t.length + (int64_t{1} << kChunkShift) - 1) >> kChunkShift;
  t.layerVersion += 1;
}

void Engine::resetLayer(Track& t) {
  // Everything currently in `layer` becomes stale; no memset needed.
  t.generation += 1;
  t.floorGeneration = t.generation;
  t.layerState = LOOPER_LAYER_NONE;
  t.layerGain = 0.0f;
  t.overdubbing = false;
  t.odWriting = false;
  t.uncommitted = 0;
  t.layerVersion += 1;
}

void Engine::startOverdub(Track& t) {
  if (!playing_ || t.state != LOOPER_TRACK_PLAYING || t.length <= 0 || t.overdubbing) return;
  // The previous layer is frozen into the base (if audible) as chunks are touched.
  t.supersededActive[t.generation % kGenerationHistory] = t.layerState == LOOPER_LAYER_ACTIVE;
  t.generation += 1;
  t.uncommitted = (t.length + (int64_t{1} << kChunkShift) - 1) >> kChunkShift;
  t.layerState = LOOPER_LAYER_RECORDING;
  t.layerGain = 1.0f;
  t.muted = false;
  t.overdubbing = true;
  t.odWriting = true;
  t.odStart = position_;
  t.odEnd = -1;
  t.layerVersion += 1;
}

void Engine::stopOverdub(Track& t) {
  if (!t.overdubbing) return;
  t.overdubbing = false;
  t.odEnd = position_;  // the latency-delayed tail keeps writing until then
  if (!playing_) t.odWriting = false;
  t.layerState = LOOPER_LAYER_ACTIVE;
  t.layerVersion += 1;
}

void Engine::undo(Track& t) {
  if (t.layerState == LOOPER_LAYER_RECORDING) {
    // Cancel the overdub in progress.
    t.overdubbing = false;
    t.odWriting = false;
    t.layerState = LOOPER_LAYER_UNDONE;
    t.layerVersion += 1;
  } else if (t.layerState == LOOPER_LAYER_ACTIVE) {
    t.layerState = LOOPER_LAYER_UNDONE;
    t.layerVersion += 1;
  }
}

void Engine::redo(Track& t) {
  if (t.layerState == LOOPER_LAYER_UNDONE) {
    t.layerState = LOOPER_LAYER_ACTIVE;
    t.layerVersion += 1;
  }
}

void Engine::commitChunk(Track& t, size_t index) {
  const size_t c = index >> kChunkShift;
  const uint32_t g = t.chunkGen[c];
  if (g == t.generation) return;
  const bool merge = g >= t.floorGeneration && t.generation - g < kGenerationHistory &&
                     t.supersededActive[g % kGenerationHistory];
  const size_t from = c << kChunkShift;
  const size_t to = std::min(from + (size_t{1} << kChunkShift), capacity_);
  float* base = t.buffer.data();
  float* layer = t.layer.data();
  for (size_t i = from; i < to; ++i) {
    if (merge) base[i] += layer[i];
    layer[i] = 0.0f;
  }
  t.chunkGen[c] = t.generation;
  if (t.uncommitted > 0) --t.uncommitted;
}

bool Engine::layerAudible(const Track& t, size_t i, LooperLayerState state) const {
  const uint32_t g = t.chunkGen[i >> kChunkShift];
  if (g == t.generation) {
    return state == LOOPER_LAYER_ACTIVE || state == LOOPER_LAYER_RECORDING;
  }
  return g >= t.floorGeneration && t.generation - g < kGenerationHistory &&
         t.supersededActive[g % kGenerationHistory];
}

void Engine::writeOverdub(Track& t, int64_t musicalPos, float sample) {
  if (musicalPos < t.odStart) return;
  if (t.odEnd >= 0 && musicalPos >= t.odEnd) {
    t.odWriting = false;
    return;
  }
  // Short fades at punch-in/out so the layer never starts with a click.
  const float fadeFrames = kPunchFadeSeconds * static_cast<float>(config_.sampleRate);
  float fade = std::min(1.0f, static_cast<float>(musicalPos - t.odStart + 1) / fadeFrames);
  if (t.odEnd >= 0) fade = std::min(fade, static_cast<float>(t.odEnd - musicalPos) / fadeFrames);
  const size_t idx = static_cast<size_t>(floorMod(musicalPos - t.recStart, t.length));
  commitChunk(t, idx);
  t.layer[idx] += sample * fade;
}

void Engine::startTransport(int64_t position) {
  playing_ = true;
  position_ = position;
  lastBeat_ = INT64_MIN;
}

void Engine::stopTransport() {
  playing_ = false;
  for (auto& t : tracks_) finishOnStop(t);
}

void Engine::finishOnStop(Track& t) {
  stopOverdub(t);
  t.odWriting = false;
  if (t.state == LOOPER_TRACK_ARMED) {
    t.state = LOOPER_TRACK_EMPTY;
    t.writing = false;
    return;
  }
  if (t.state == LOOPER_TRACK_RECORDING) {
    // Keep the whole bars recorded so far rather than throwing the take away.
    const int64_t target = t.recEnd >= 0 ? t.recEnd - t.recStart : INT64_MAX;
    const int64_t len = std::min((position_ - t.recStart) / bar_ * bar_, target);
    if (len <= 0) {
      t.state = LOOPER_TRACK_EMPTY;
      t.writing = false;
      return;
    }
    t.recEnd = t.recStart + len;
    t.length = len;
    beginPlaying(t);
  }
  if (t.state == LOOPER_TRACK_PLAYING && t.writing) {
    // The latency-delayed tail never arrived: silence it instead of
    // replaying stale audio from a previous take.
    const int64_t from = std::clamp<int64_t>(t.written, 0, t.length);
    std::fill(t.buffer.begin() + from, t.buffer.begin() + t.length, 0.0f);
    t.writing = false;
  }
}

void Engine::process(const float* input, float* output, uint32_t frames) {
  Command cmd;
  while (commands_.pop(cmd)) {
    handle(cmd);
    ++handled_;
  }

  const int inCh = config_.inputChannels;
  const int outCh = config_.outputChannels;
  for (uint32_t i = 0; i < frames; ++i) {
    processFrame(input ? input + static_cast<size_t>(i) * inCh : nullptr,
                 output + static_cast<size_t>(i) * outCh);
  }
  ++blockCounter_;
  publishStatus();
}

void Engine::processFrame(const float* in, float* out) {
  const int usable = std::min(config_.inputChannels, kMaxInputs);
  const size_t slot = static_cast<size_t>(deviceFrame_ & historyMask_);
  for (int ch = 0; ch < usable; ++ch) {
    const float x = in ? in[ch] : 0.0f;
    history_[static_cast<size_t>(ch) * historySize_ + slot] = x;
    inputPeaks_[ch] = std::max(std::fabs(x), inputPeaks_[ch] * meterDecay_);
  }
  auto inputSample = [&](int ch) { return (in && ch < usable) ? in[ch] : 0.0f; };

  float mix = 0.0f;
  float monitor = 0.0f;
  uint32_t monitoredChannels = 0;

  if (playing_) {
    const int64_t p = position_;
    const int64_t musical = p - latency_;
    const int64_t maxLen =
        std::max<int64_t>(static_cast<int64_t>(capacity_) / bar_ * bar_, bar_ / 2);

    for (auto& t : tracks_) {
      if (t.state == LOOPER_TRACK_ARMED && p >= t.recStart) t.state = LOOPER_TRACK_RECORDING;
      if (t.state == LOOPER_TRACK_RECORDING) {
        if (t.recEnd < 0 && p - t.recStart >= maxLen) t.recEnd = t.recStart + maxLen;
        if (t.recEnd >= 0 && p >= t.recEnd) {
          t.length = t.recEnd - t.recStart;
          beginPlaying(t);
        }
      }
      if (t.writing) writeRecording(t, musical, inputSample(t.inputChannel));
      if (t.odWriting) writeOverdub(t, musical, inputSample(t.inputChannel));

      if (t.state == LOOPER_TRACK_PLAYING && t.length > 0) {
        const float target = t.muted ? 0.0f : t.volume;
        t.gain += (target - t.gain) * gainSmoothing_;
        const bool layerOn =
            t.layerState == LOOPER_LAYER_ACTIVE || t.layerState == LOOPER_LAYER_RECORDING;
        t.layerGain += ((layerOn ? 1.0f : 0.0f) - t.layerGain) * gainSmoothing_;
        const size_t idx = static_cast<size_t>(floorMod(p - t.recStart, t.length));
        commitChunk(t, idx);
        const float s = (t.buffer[idx] + t.layer[idx] * t.layerGain) * t.gain;
        mix += s;
        t.peak = std::max(std::fabs(s), t.peak * meterDecay_);
      } else {
        t.peak *= meterDecay_;
      }

      const bool live = t.state == LOOPER_TRACK_ARMED || t.state == LOOPER_TRACK_RECORDING ||
                        t.overdubbing;
      if (monitoring_ && live && !(monitoredChannels & (1u << t.inputChannel))) {
        monitoredChannels |= 1u << t.inputChannel;
        monitor += inputSample(t.inputChannel);
      }
    }

    const int64_t beat = floorDiv(p * beatsPerBar_, bar_);
    if (beat != lastBeat_) {
      lastBeat_ = beat;
      clickFreq_ = floorMod(beat, beatsPerBar_) == 0 ? 1760.0f : 1175.0f;
      clickEnv_ = 1.0f;
      clickPhase_ = 0.0f;
    }
    ++position_;
  } else {
    for (auto& t : tracks_) t.peak *= meterDecay_;
  }

  const float click = metronomeSample();
  const float left = mix + monitor + click;
  if (config_.outputChannels >= 1) out[0] = left;
  if (config_.outputChannels >= 2) out[1] = left;
  for (int ch = 2; ch < config_.outputChannels; ++ch) out[ch] = 0.0f;
  ++deviceFrame_;
}

void Engine::writeRecording(Track& t, int64_t musicalPos, float sample) {
  if (musicalPos < t.recStart) return;
  if (t.recEnd >= 0 && musicalPos >= t.recEnd) {
    t.writing = false;
    return;
  }
  const int64_t idx = musicalPos - t.recStart;
  if (idx >= static_cast<int64_t>(capacity_)) {
    t.writing = false;
    return;
  }
  if (!t.writeStarted) {
    t.writeStarted = true;
    // Retroactive start (late press): the first `idx` frames already went by.
    // Pull them from the input history; the current frame is written below.
    const int64_t available = static_cast<int64_t>(historySize_) - 1;
    const int64_t missing = std::min(idx, available);
    std::fill(t.buffer.begin(), t.buffer.begin() + (idx - missing), 0.0f);
    for (int64_t k = 0; k < missing; ++k) {
      t.buffer[static_cast<size_t>(idx - missing + k)] =
          historyAt(t.inputChannel, deviceFrame_ - missing + k);
    }
  }
  t.buffer[static_cast<size_t>(idx)] = sample;
  t.written = idx + 1;
}

float Engine::metronomeSample() {
  if (clickEnv_ < 1e-4f) return 0.0f;
  const float s = std::sin(clickPhase_) * clickEnv_;
  clickPhase_ += kTwoPi * clickFreq_ / static_cast<float>(config_.sampleRate);
  if (clickPhase_ > kTwoPi) clickPhase_ -= kTwoPi;
  clickEnv_ *= clickDecay_;
  return metronomeOn_ ? s * metronomeVolume_ : 0.0f;
}

void Engine::publishStatus() {
  LooperStatus& s = status_.back();
  s.device_running = 1;
  s.playing = playing_ ? 1 : 0;
  s.sample_rate = config_.sampleRate;
  s.input_channels = std::min(config_.inputChannels, kMaxInputs);
  s.output_channels = config_.outputChannels;
  s.beats_per_bar = beatsPerBar_;
  s.metronome_on = metronomeOn_ ? 1 : 0;
  s.monitoring_on = monitoring_ ? 1 : 0;
  s.count_in_bars = countInBars_;
  s.latency_frames = static_cast<int32_t>(latency_);
  s.bpm = bpm_;
  s.position = position_;
  s.bar_frames = bar_;
  s.block_counter = blockCounter_;
  s.commands_handled = handled_;
  s.metronome_volume = metronomeVolume_;
  for (int ch = 0; ch < kMaxInputs; ++ch) s.input_peaks[ch] = inputPeaks_[ch];
  for (int i = 0; i < kMaxTracks; ++i) {
    const Track& t = tracks_[i];
    LooperTrackStatus& ts = s.tracks[i];
    ts.state = t.state;
    ts.muted = t.muted ? 1 : 0;
    ts.input_channel = t.inputChannel;
    ts.length_mode = t.lengthMode;
    ts.layer_state = t.layerState;
    ts.layer_version = t.layerVersion;
    ts.rec_start = t.recStart;
    ts.rec_end = t.recEnd;
    ts.length = t.length;
    ts.busy = (t.writing || t.odWriting || t.overdubbing ||
               (playing_ && t.uncommitted > 0 && t.layerState != LOOPER_LAYER_NONE))
                  ? 1
                  : 0;
    ts.volume = t.volume;
    ts.peak = t.peak;
  }
  status_.publish();
}

int Engine::waveform(int track, float* out, int bins) {
  if (track < 0 || track >= kMaxTracks || bins <= 0) return 0;
  const LooperTrackStatus& ts = status_.read().tracks[track];
  if (ts.state != LOOPER_TRACK_PLAYING || ts.length <= 0) return 0;
  // Runs on the UI thread while audio may be writing: the result is only
  // used for drawing, so an occasional torn chunk is harmless.
  const Track& t = tracks_[track];
  const auto layerState = static_cast<LooperLayerState>(ts.layer_state);
  const int64_t len = std::min<int64_t>(ts.length, static_cast<int64_t>(capacity_));
  for (int b = 0; b < bins; ++b) {
    const int64_t from = len * b / bins;
    const int64_t to = std::max(len * (b + 1) / bins, from + 1);
    float peak = 0.0f;
    for (int64_t k = from; k < to && k < len; ++k) {
      const size_t i = static_cast<size_t>(k);
      const float layer = layerAudible(t, i, layerState) ? t.layer[i] : 0.0f;
      peak = std::max(peak, std::fabs(t.buffer[i] + layer));
    }
    out[b] = std::min(peak, 1.0f);
  }
  return bins;
}

int64_t Engine::exportLoop(int track, float* out, int64_t capacity) {
  if (track < 0 || track >= kMaxTracks || !out) return 0;
  // Safe without locks because the only things that could change the loop
  // are commands (none pending — the UI thread is the only sender, and it
  // is busy here) and recording/layering/merging (`busy`).
  const LooperStatus& s = status_.read();
  if (static_cast<int64_t>(sent_) != s.commands_handled) return -1;
  const LooperTrackStatus& ts = s.tracks[track];
  if (ts.state == LOOPER_TRACK_ARMED || ts.state == LOOPER_TRACK_RECORDING || ts.busy) return -1;
  if (ts.state != LOOPER_TRACK_PLAYING || ts.length <= 0) return 0;
  const Track& t = tracks_[track];
  const auto layerState = static_cast<LooperLayerState>(ts.layer_state);
  const int64_t len = std::min({ts.length, capacity, static_cast<int64_t>(capacity_)});
  for (int64_t k = 0; k < len; ++k) {
    const size_t i = static_cast<size_t>(k);
    out[k] = t.buffer[i] + (layerAudible(t, i, layerState) ? t.layer[i] : 0.0f);
  }
  return len;
}

int Engine::importLoop(int track, const float* data, int64_t frames, int64_t targetFrames) {
  if (track < 0 || track >= kMaxTracks || !data || frames <= 0) return -2;
  if (targetFrames <= 0 || targetFrames > static_cast<int64_t>(capacity_)) return -2;
  const LooperStatus& s = status_.read();
  if (static_cast<int64_t>(sent_) != s.commands_handled) return -1;
  if (s.tracks[track].state != LOOPER_TRACK_EMPTY) return -1;
  // The audio thread never touches an empty track's buffer, and the TrackLoad
  // command below publishes these writes to it (release/acquire queue).
  float* dst = tracks_[track].buffer.data();
  if (frames == targetFrames) {
    std::copy(data, data + frames, dst);
  } else {
    // Linear resampling: sessions recorded at another sample rate, or a
    // length rounded differently at the current tempo.
    const double step = static_cast<double>(frames) / static_cast<double>(targetFrames);
    for (int64_t k = 0; k < targetFrames; ++k) {
      const double x = static_cast<double>(k) * step;
      const auto i = static_cast<int64_t>(x);
      const float frac = static_cast<float>(x - static_cast<double>(i));
      const float a = data[std::min(i, frames - 1)];
      const float b = data[std::min(i + 1, frames - 1)];
      dst[k] = a + (b - a) * frac;
    }
  }
  Command cmd;
  cmd.type = CommandType::TrackLoad;
  cmd.track = track;
  cmd.d = static_cast<double>(targetFrames);
  return send(cmd) ? 0 : -1;
}

}  // namespace looper
