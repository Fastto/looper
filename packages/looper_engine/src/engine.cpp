#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "grid.hpp"

namespace looper {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kPunchFadeSeconds = 0.004f;
constexpr float kSeamFadeSeconds = 0.008f;
constexpr float kHalfPi = 1.57079632679f;
constexpr float kPi = 3.14159265359f;
constexpr float kStopFadeSeconds = 0.010f;   // Stop, and clearing a playing track
constexpr float kStartFadeSeconds = 0.002f;  // short: keeps the downbeat's attack

// Raised-cosine envelope over `frames` steps: step 1 → just above 0, `frames` → 1.
float riseCurve(int step, int frames) {
  return 0.5f * (1.0f - std::cos(kPi * static_cast<float>(step) / static_cast<float>(frames)));
}

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

  // Four seconds of input history: the late-press tolerance and the
  // round-trip latency reach back in time, and every recording copies up to
  // a bar of pre-roll from it (see headroom()).
  historySize_ = nextPowerOfTwo(static_cast<size_t>(sr * 4.0));
  historyMask_ = static_cast<int64_t>(historySize_ - 1);
  const int usableInputs = std::min(config_.inputChannels, kMaxInputs);
  history_.assign(historySize_ * static_cast<size_t>(std::max(usableInputs, 1)), 0.0f);

  stopFadeFrames_ = std::max(1, static_cast<int>(std::lround(kStopFadeSeconds * sr)));
  startFadeFrames_ = std::max(1, static_cast<int>(std::lround(kStartFadeSeconds * sr)));

  // Calibration: 8 short windowed 2 kHz bursts, 300 ms apart, after 200 ms.
  calLead_ = static_cast<int64_t>(0.2 * sr);
  calInterval_ = static_cast<int64_t>(0.3 * sr);
  const int pulseLen = std::max(8, static_cast<int>(std::lround(0.0015 * sr)));
  const double pulseFreq = std::min(2000.0, sr / 4.0);
  calPulse_.resize(static_cast<size_t>(pulseLen));
  for (int i = 0; i < pulseLen; ++i) {
    const double hann = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (pulseLen - 1));
    calPulse_[static_cast<size_t>(i)] =
        static_cast<float>(std::sin(2.0 * 3.14159265358979 * pulseFreq * i / sr) * hann);
  }
  calCapture_.assign(static_cast<size_t>(calLead_ + calInterval_ * (kCalPulses + 1)), 0.0f);
  seamFrames_ = std::clamp(static_cast<int>(std::lround(kSeamFadeSeconds * sr)), 1, kMaxSeamFrames);
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
      if (playing_ && !stopping_) beginStop();
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
      if (!t) break;
      if (playing_ && t->state == LOOPER_TRACK_PLAYING && t->gain > 1e-4f) {
        // Audible: fade it out first (processFrame clears it at the end).
        t->clearFadeLeft = stopFadeFrames_;
        t->overdubbing = false;
        t->odWriting = false;
      } else {
        clearTrack(*t);
      }
      break;
    case CommandType::TrackOverdub:
      if (t) t->overdubbing ? stopOverdub(*t) : startOverdub(*t);
      break;
    case CommandType::TrackLoad: {
      const auto length = static_cast<int64_t>(cmd.d);
      if (t && t->state == LOOPER_TRACK_EMPTY && length > 0) {
        resetLayer(*t);
        t->writing = false;
        t->recStart = 0;  // every loop is anchored to bar 1
        t->recEnd = length;
        t->length = length;
        t->origin = cmd.a;
        t->audioEnd = cmd.b;
        t->trimEighths = static_cast<int>(cmd.c);
        t->windowStart = t->origin + eighthsToFrames(t->trimEighths);
        t->trimLocked = cmd.i != 0;
        t->windowFadeLeft = 0;
        beginPlaying(*t);
      }
      break;
    }
    case CommandType::TrackShiftStart:
      if (t && t->state == LOOPER_TRACK_PLAYING && !t->trimLocked && !t->overdubbing) {
        int minE = 0, maxE = 0, maxBars = 0;
        trimLimits(*t, minE, maxE, maxBars);
        const int target = t->trimEighths + cmd.i;
        if (cmd.i != 0 && target >= minE && target <= maxE) {
          t->trimEighths = target;
          setWindow(*t, t->origin + eighthsToFrames(target), t->length);
        }
      }
      break;
    case CommandType::TrackTrimBars:
      if (t && t->state == LOOPER_TRACK_PLAYING && !t->trimLocked && !t->overdubbing &&
          t->length >= bar_ && t->length % bar_ == 0) {
        int minE = 0, maxE = 0, maxBars = 0;
        trimLimits(*t, minE, maxE, maxBars);
        const int64_t bars = t->length / bar_ + cmd.i;
        if (cmd.i != 0 && bars >= 1 && bars <= maxBars) setWindow(*t, t->windowStart, bars * bar_);
      }
      break;
    case CommandType::TrackUndo:
      if (t) undo(*t);
      break;
    case CommandType::TrackRedo:
      if (t) redo(*t);
      break;
    case CommandType::TrackMute:
      if (t) requestMute(*t, cmd.i != 0);
      break;
    case CommandType::TrackPan:
      if (t) t->pan = std::clamp(static_cast<float>(cmd.d), -1.0f, 1.0f);
      break;
    case CommandType::SetMuteQuantize:
      muteQuantize_ = std::clamp(cmd.i, 0, 2);
      if (muteQuantize_ == 0) applyPendingMutes();
      break;
    case CommandType::SetFeedback:
      feedback_ = std::clamp(static_cast<float>(cmd.d), 0.0f, 1.0f);
      break;
    case CommandType::CalibrationStart:
      if (!playing_ && calState_ != 1) {
        calChannel_ = std::clamp(cmd.i, 0, std::max(std::min(config_.inputChannels, kMaxInputs) - 1, 0));
        calPos_ = 0;
        calState_ = 1;
      }
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
  const int64_t maxLen = std::max<int64_t>(
      (static_cast<int64_t>(capacity_) - headroom()) / bar_ * bar_, bar_ / 2);
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
      t.origin = headroom();
      t.audioEnd = 0;
      t.windowStart = t.origin;
      t.trimEighths = 0;
      t.trimLocked = false;
      t.windowFadeLeft = 0;
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
        requestMute(t, !(t.pendingMute >= 0 ? t.pendingMute != 0 : t.muted));
      }
      break;
  }
}

// Mute/unmute right away, or on the next beat/bar when quantized. A late
// press (within the timing tolerance after the line) still counts as on it,
// and asking again for the current state cancels a pending change.
void Engine::requestMute(Track& t, bool muted) {
  if (muted == t.muted) {
    t.pendingMute = -1;
    return;
  }
  if (!playing_ || stopping_ || muteQuantize_ == 0 || t.state != LOOPER_TRACK_PLAYING) {
    t.muted = muted;
    t.pendingMute = -1;
    return;
  }
  const int64_t p = position_;
  int64_t lastLine;
  if (muteQuantize_ == 2) {
    lastLine = floorDiv(p, bar_) * bar_;
  } else {
    const int64_t beat = floorDiv(p * beatsPerBar_, bar_);
    lastLine = floorDiv(beat * bar_ + beatsPerBar_ - 1, beatsPerBar_);  // first frame of that beat
  }
  if (p - lastLine <= tolerance_) {
    t.muted = muted;
    t.pendingMute = -1;
  } else {
    t.pendingMute = muted ? 1 : 0;
  }
}

void Engine::applyPendingMutes() {
  for (auto& t : tracks_) {
    if (t.pendingMute >= 0) {
      t.muted = t.pendingMute != 0;
      t.pendingMute = -1;
    }
  }
}

void Engine::beginPlaying(Track& t) {
  t.state = LOOPER_TRACK_PLAYING;
  t.fadeInLeft = startFadeFrames_;
  t.gain = t.muted ? 0.0f : t.volume;  // no fade-in: keep the downbeat's attack
  t.uncommitted = windowChunks(t);
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
  lockTrim(t);
  // The previous layer is frozen into the base (if audible) as chunks are touched.
  t.supersededActive[t.generation % kGenerationHistory] = t.layerState == LOOPER_LAYER_ACTIVE;
  t.generation += 1;
  t.uncommitted = windowChunks(t);
  t.layerState = LOOPER_LAYER_RECORDING;
  t.layerGain = 1.0f;
  t.muted = false;
  t.pendingMute = -1;
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
  const auto idx =
      static_cast<size_t>(t.windowStart + floorMod(musicalPos - t.recStart, t.length));
  commitChunk(t, idx);
  // Feedback: each pass scales what is already there. The decay lives in the
  // layer as a (negative) delta, so undo still brings back the original.
  const float existing = t.buffer[idx] + t.layer[idx];
  t.layer[idx] += fade * (sample + (feedback_ - 1.0f) * existing);
}

void Engine::startTransport(int64_t position) {
  playing_ = true;
  position_ = position;
  lastBeat_ = INT64_MIN;
  for (auto& t : tracks_) {
    if (t.state == LOOPER_TRACK_PLAYING) t.fadeInLeft = startFadeFrames_;
  }
}

void Engine::clearTrack(Track& t) {
  t.state = LOOPER_TRACK_EMPTY;
  t.writing = false;
  t.length = 0;
  t.muted = false;
  t.clearFadeLeft = 0;
  t.fadeInLeft = 0;
  t.pendingMute = -1;
  t.origin = 0;
  t.audioEnd = 0;
  t.windowStart = 0;
  t.trimEighths = 0;
  t.trimLocked = false;
  t.windowFadeLeft = 0;
  resetLayer(t);
}

// Commands wait while something is fading out, so they act on the state
// that follows (e.g. a new take after Stop, or after a clear).
bool Engine::deferCommands() const {
  if (stopping_) return true;
  for (const auto& t : tracks_) {
    if (t.clearFadeLeft > 0) return true;
  }
  return false;
}

// Cutting the output dead clicks, and cutting writes short would leave an
// overdub without its punch-out fade. So Stop fades the output out, ends
// layers right away (their latency-delayed tails still arrive), and only
// then stops the transport, as of the moment Stop was pressed.
void Engine::beginStop() {
  stopping_ = true;
  stopPressPosition_ = position_;
  stopFrame_ = 0;
  for (auto& t : tracks_) stopOverdub(t);
  const auto punchFade =
      static_cast<int64_t>(std::ceil(kPunchFadeSeconds * static_cast<float>(config_.sampleRate)));
  stopFramesLeft_ = std::max<int64_t>(stopFadeFrames_, latency_ + punchFade + 1);
}

void Engine::finishStop() {
  stopping_ = false;
  position_ = stopPressPosition_;
  clickEnv_ = 0.0f;
  applyPendingMutes();
  stopTransport();
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
  // A latency-delayed tail that never arrived is simply silence: playback
  // reads nothing past audioEnd.
  if (t.state == LOOPER_TRACK_PLAYING) t.writing = false;
}

void Engine::process(const float* input, float* output, uint32_t frames) {
  Command cmd;
  // While stopping, later commands (Play, a new recording...) wait their turn.
  while (!deferCommands() && commands_.pop(cmd)) {
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

  float mixL = 0.0f;
  float mixR = 0.0f;
  float monitor = 0.0f;
  uint32_t monitoredChannels = 0;

  if (playing_) {
    const int64_t p = position_;
    const int64_t musical = p - latency_;
    const int64_t beat = floorDiv(p * beatsPerBar_, bar_);
    const bool newBeat = beat != lastBeat_;
    if (newBeat && muteQuantize_ != 0 &&
        (muteQuantize_ == 1 || floorMod(beat, beatsPerBar_) == 0)) {
      applyPendingMutes();
    }

    for (auto& t : tracks_) {
      if (t.state == LOOPER_TRACK_ARMED && p >= t.recStart) t.state = LOOPER_TRACK_RECORDING;
      if (t.state == LOOPER_TRACK_RECORDING) {
        const int64_t maxLen = std::max<int64_t>(
            (static_cast<int64_t>(capacity_) - t.origin) / bar_ * bar_, bar_ / 2);
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
        auto sampleAt = [&](int64_t start, int64_t length) {
          const int64_t pos = floorMod(p - t.recStart, length);
          const auto idx = static_cast<size_t>(start + pos);
          commitChunk(t, idx);
          return renderBase(t, start, length, pos) + t.layer[idx] * t.layerGain;
        };
        float loopSample = sampleAt(t.windowStart, t.length);
        if (t.windowFadeLeft > 0) {
          // Trimmed while playing: crossfade from where the old window was.
          const float w = riseCurve(stopFadeFrames_ / 2 - t.windowFadeLeft + 1, stopFadeFrames_ / 2);
          loopSample = sampleAt(t.prevStart, t.prevLength) * (1.0f - w) + loopSample * w;
          --t.windowFadeLeft;
        }
        float envelope = 1.0f;
        if (t.fadeInLeft > 0) {
          envelope = riseCurve(startFadeFrames_ - t.fadeInLeft + 1, startFadeFrames_);
          --t.fadeInLeft;
        }
        bool clearNow = false;
        if (t.clearFadeLeft > 0) {
          envelope *= 1.0f - riseCurve(stopFadeFrames_ - t.clearFadeLeft + 1, stopFadeFrames_);
          clearNow = --t.clearFadeLeft == 0;
        }
        const float s = loopSample * t.gain * envelope;
        // Balance-style pan law: unity at the centre, the far side fades out.
        const float targetL = t.pan <= 0.0f ? 1.0f : std::cos(t.pan * kHalfPi);
        const float targetR = t.pan >= 0.0f ? 1.0f : std::cos(-t.pan * kHalfPi);
        t.panL += (targetL - t.panL) * gainSmoothing_;
        t.panR += (targetR - t.panR) * gainSmoothing_;
        mixL += s * t.panL;
        mixR += s * t.panR;
        t.peak = std::max(std::fabs(s), t.peak * meterDecay_);
        if (clearNow) clearTrack(t);
      } else {
        t.peak *= meterDecay_;
      }

      const bool live = t.state == LOOPER_TRACK_ARMED || t.state == LOOPER_TRACK_RECORDING ||
                        t.overdubbing;
      if (monitoring_ && live) monitoredChannels |= 1u << t.inputChannel;
    }

    if (newBeat) {
      lastBeat_ = beat;
      clickFreq_ = floorMod(beat, beatsPerBar_) == 0 ? 1760.0f : 1175.0f;
      clickEnv_ = 1.0f;
      clickPhase_ = 0.0f;
    }
    ++position_;
  } else {
    for (auto& t : tracks_) t.peak *= meterDecay_;
  }

  // Input monitoring fades in and out per channel instead of switching.
  for (int ch = 0; ch < usable; ++ch) {
    const float target = (monitoredChannels & (1u << ch)) ? 1.0f : 0.0f;
    monitorGain_[ch] += (target - monitorGain_[ch]) * gainSmoothing_;
    if (monitorGain_[ch] > 1e-5f) monitor += inputSample(ch) * monitorGain_[ch];
  }

  const float click = metronomeSample();
  float master = 1.0f;
  if (stopping_) {
    master = stopFrame_ < stopFadeFrames_
                 ? 0.5f * (1.0f + std::cos(kPi * static_cast<float>(stopFrame_ + 1) /
                                           static_cast<float>(stopFadeFrames_)))
                 : 0.0f;
    ++stopFrame_;
  }
  float left = (mixL + monitor + click) * master;
  float right = (mixR + monitor + click) * master;

  // Calibration replaces the output with test pulses and records the input.
  if (calState_ == 1) {
    const int64_t sincePulse = calPos_ - calLead_;
    float pulse = 0.0f;
    if (sincePulse >= 0 && sincePulse < calInterval_ * kCalPulses) {
      const int64_t k = sincePulse % calInterval_;
      if (k < static_cast<int64_t>(calPulse_.size())) pulse = 0.5f * calPulse_[static_cast<size_t>(k)];
    }
    left = right = pulse;
    calCapture_[static_cast<size_t>(calPos_)] = inputSample(calChannel_);
    if (++calPos_ >= static_cast<int64_t>(calCapture_.size())) calState_ = 2;
  }

  if (config_.outputChannels == 1) out[0] = 0.5f * (left + right);
  if (config_.outputChannels >= 2) {
    out[0] = left;
    out[1] = right;
  }
  for (int ch = 2; ch < config_.outputChannels; ++ch) out[ch] = 0.0f;
  ++deviceFrame_;
  if (stopping_ && --stopFramesLeft_ <= 0) finishStop();
}

void Engine::writeRecording(Track& t, int64_t musicalPos, float sample) {
  if (musicalPos < t.recStart) return;
  if (t.recEnd >= 0 && musicalPos >= t.recEnd) {
    t.writing = false;
    return;
  }
  const int64_t idx = t.origin + (musicalPos - t.recStart);  // recording frame
  if (idx >= static_cast<int64_t>(capacity_)) {
    t.writing = false;
    return;
  }
  if (!t.writeStarted) {
    t.writeStarted = true;
    // Everything before the current frame comes from the input history: the
    // pre-roll before the downbeat (for trimming earlier and for the seam)
    // and, after a late press, the part of the take that already went by.
    const int64_t available = static_cast<int64_t>(historySize_) - 1;
    const int64_t missing = std::min(idx, available);
    std::fill(t.buffer.begin(), t.buffer.begin() + (idx - missing), 0.0f);
    for (int64_t k = 0; k < missing; ++k) {
      t.buffer[static_cast<size_t>(idx - missing + k)] =
          historyAt(t.inputChannel, deviceFrame_ - missing + k);
    }
  }
  t.buffer[static_cast<size_t>(idx)] = sample;
  t.written = idx + 1 - t.origin;
  t.audioEnd = idx + 1;
}

// Headroom kept before the downbeat of every recording: up to a bar, limited
// by the input history it is copied from.
int64_t Engine::headroom() const {
  return std::max<int64_t>(seamFrames_, std::min<int64_t>(bar_, static_cast<int64_t>(historySize_) / 2));
}

int64_t Engine::eighthsToFrames(int eighths) const {
  return static_cast<int64_t>(
      std::llround(static_cast<double>(eighths) * static_cast<double>(bar_) / (2.0 * beatsPerBar_)));
}

float Engine::baseAt(const Track& t, int64_t i) const {
  return (i >= 0 && i < t.audioEnd) ? t.buffer[static_cast<size_t>(i)] : 0.0f;
}

// One sample of the loop's base at loop position `pos` of window
// [start, start + length). While the trim can still change, the seam is
// crossfaded here, at playback time, into whatever precedes the current start
// in the recording (equal-power, last 8 ms): the wrap is continuous and the
// downbeat untouched, wherever the window is.
float Engine::renderBase(const Track& t, int64_t start, int64_t length, int64_t pos) const {
  const float s = baseAt(t, start + pos);
  if (t.trimLocked) return s;
  const int64_t fade = std::min<int64_t>(seamFrames_, length / 4);
  const int64_t k = pos - (length - fade);
  if (fade <= 0 || k < 0) return s;
  const float x = (static_cast<float>(k) + 0.5f) / static_cast<float>(fade);
  return s * std::cos(x * kHalfPi) + baseAt(t, start - fade + k) * std::sin(x * kHalfPi);
}

void Engine::setWindow(Track& t, int64_t start, int64_t length) {
  if (playing_ && t.state == LOOPER_TRACK_PLAYING) {
    t.prevStart = t.windowStart;
    t.prevLength = t.length;
    t.windowFadeLeft = std::max(1, stopFadeFrames_ / 2);
  }
  t.windowStart = start;
  t.length = length;
  t.recEnd = t.recStart + length;
  t.layerVersion += 1;
}

// Layers are recorded against the current cycle, so the first one fixes the
// window: the seam is baked into the base and the trim is locked.
void Engine::lockTrim(Track& t) {
  if (t.trimLocked) return;
  const int64_t end = t.windowStart + t.length;
  if (t.audioEnd < end) {
    std::fill(t.buffer.begin() + std::max<int64_t>(t.audioEnd, 0), t.buffer.begin() + end, 0.0f);
  }
  const int64_t fade = std::min<int64_t>(seamFrames_, t.length / 4);
  for (int64_t k = 0; k < fade; ++k) {
    const int64_t pos = t.length - fade + k;
    t.buffer[static_cast<size_t>(t.windowStart + pos)] = renderBase(t, t.windowStart, t.length, pos);
  }
  t.audioEnd = std::max(t.audioEnd, end);
  t.trimLocked = true;
  t.windowFadeLeft = 0;
}

void Engine::trimLimits(const Track& t, int& minEighths, int& maxEighths, int& maxBars) const {
  const int64_t bars = t.length / bar_;
  if (t.trimLocked || t.state != LOOPER_TRACK_PLAYING) {
    minEighths = maxEighths = t.trimEighths;
    maxBars = 0;
    return;
  }
  // Start: anywhere from the beginning of the pre-roll to the last recorded frame.
  const double eighth = static_cast<double>(bar_) / (2.0 * beatsPerBar_);
  minEighths = static_cast<int>(std::ceil(-static_cast<double>(t.origin) / eighth));
  while (t.origin + eighthsToFrames(minEighths) < 0) ++minEighths;
  maxEighths = static_cast<int>(std::floor(static_cast<double>(t.audioEnd - 1 - t.origin) / eighth));
  while (maxEighths > minEighths && t.origin + eighthsToFrames(maxEighths) > t.audioEnd - 1) --maxEighths;
  minEighths = std::min(minEighths, t.trimEighths);
  maxEighths = std::max(maxEighths, t.trimEighths);
  // Length: whole bars, up to what covers the recording (or the current length).
  if (t.length < bar_ || t.length % bar_ != 0) {
    maxBars = 0;
    return;
  }
  const int64_t cover = (std::max<int64_t>(t.audioEnd - t.windowStart, 0) + bar_ - 1) / bar_;
  const int64_t fits = (static_cast<int64_t>(capacity_) - t.windowStart) / bar_;
  maxBars = static_cast<int>(std::min(std::max(bars, cover), std::max(bars, fits)));
}

int64_t Engine::windowChunks(const Track& t) const {
  if (t.length <= 0) return 0;
  return ((t.windowStart + t.length - 1) >> kChunkShift) - (t.windowStart >> kChunkShift) + 1;
}

float Engine::metronomeSample() {
  // Muting the click mid-tick fades it rather than cutting it.
  metronomeGain_ += ((metronomeOn_ ? metronomeVolume_ : 0.0f) - metronomeGain_) * gainSmoothing_;
  if (clickEnv_ < 1e-4f) return 0.0f;
  const float s = std::sin(clickPhase_) * clickEnv_;
  clickPhase_ += kTwoPi * clickFreq_ / static_cast<float>(config_.sampleRate);
  if (clickPhase_ > kTwoPi) clickPhase_ -= kTwoPi;
  clickEnv_ *= clickDecay_;
  return s * metronomeGain_;
}

void Engine::publishStatus() {
  LooperStatus& s = status_.back();
  s.device_running = 1;
  s.playing = (playing_ && !stopping_) ? 1 : 0;
  s.sample_rate = config_.sampleRate;
  s.input_channels = std::min(config_.inputChannels, kMaxInputs);
  s.output_channels = config_.outputChannels;
  s.beats_per_bar = beatsPerBar_;
  s.metronome_on = metronomeOn_ ? 1 : 0;
  s.monitoring_on = monitoring_ ? 1 : 0;
  s.count_in_bars = countInBars_;
  s.latency_frames = static_cast<int32_t>(latency_);
  s.mute_quantize = muteQuantize_;
  s.calibration = calState_;
  s.feedback = feedback_;
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
    ts.busy = (t.writing || t.odWriting || t.overdubbing || t.clearFadeLeft > 0 ||
               (playing_ && t.uncommitted > 0 && t.layerState != LOOPER_LAYER_NONE))
                  ? 1
                  : 0;
    ts.pending_mute = t.pendingMute;
    ts.trim_locked = t.trimLocked ? 1 : 0;
    ts.trim_eighths = t.trimEighths;
    int minE = 0, maxE = 0, maxBars = 0;
    trimLimits(t, minE, maxE, maxBars);
    ts.trim_min_eighths = minE;
    ts.trim_max_eighths = maxE;
    ts.trim_max_bars = maxBars;
    ts.origin = t.origin;
    ts.window_start = t.windowStart;
    ts.audio_end = t.audioEnd;
    ts.pan = t.pan;
    ts.volume = t.volume;
    ts.peak = t.peak;
  }
  status_.publish();
}

int Engine::waveform(int track, int64_t from, int64_t to, float* out, int bins) {
  if (track < 0 || track >= kMaxTracks || bins <= 0 || to <= from) return 0;
  const LooperTrackStatus& ts = status_.read().tracks[track];
  if (ts.state != LOOPER_TRACK_PLAYING || ts.length <= 0) return 0;
  // Runs on the UI thread while audio may be writing: the result is only
  // used for drawing, so an occasional torn chunk is harmless.
  const Track& t = tracks_[track];
  const auto layerState = static_cast<LooperLayerState>(ts.layer_state);
  const int64_t span = to - from;
  const auto cap = static_cast<int64_t>(capacity_);
  for (int b = 0; b < bins; ++b) {
    const int64_t lo = from + span * b / bins;
    const int64_t hi = std::max(from + span * (b + 1) / bins, lo + 1);
    float peak = 0.0f;
    for (int64_t i = std::max<int64_t>(lo, 0); i < hi && i < cap; ++i) {
      const auto u = static_cast<size_t>(i);
      const float layer = layerAudible(t, u, layerState) ? t.layer[u] : 0.0f;
      peak = std::max(peak, std::fabs(baseAt(t, i) + layer));
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
  const int64_t len = std::min(ts.length, capacity);
  for (int64_t pos = 0; pos < len; ++pos) {
    const auto i = static_cast<size_t>(t.windowStart + pos);
    out[pos] = renderBase(t, t.windowStart, ts.length, pos) +
               (layerAudible(t, i, layerState) ? t.layer[i] : 0.0f);
  }
  return len;
}

int64_t Engine::exportRecording(int track, float* out, int64_t capacity) {
  if (track < 0 || track >= kMaxTracks || !out) return 0;
  const LooperStatus& s = status_.read();
  if (static_cast<int64_t>(sent_) != s.commands_handled) return -1;
  const LooperTrackStatus& ts = s.tracks[track];
  if (ts.state == LOOPER_TRACK_ARMED || ts.state == LOOPER_TRACK_RECORDING || ts.busy) return -1;
  if (ts.state != LOOPER_TRACK_PLAYING || ts.length <= 0) return 0;
  if (ts.trim_locked) return -2;
  const Track& t = tracks_[track];
  const int64_t len = std::min(ts.audio_end, capacity);
  std::copy(t.buffer.begin(), t.buffer.begin() + len, out);
  return len;
}

// Finds each pulse in the captured input by cross-correlating with the pulse
// shape; the round trip is the median delay of the clearly detected ones.
int Engine::calibrationResult(int32_t* frames) {
  if (status_.read().calibration != 2) return 0;
  const int64_t n = static_cast<int64_t>(calPulse_.size());
  const int64_t maxLag = calInterval_ - n;
  std::vector<int64_t> lags;
  for (int k = 0; k < kCalPulses; ++k) {
    const int64_t start = calLead_ + k * calInterval_;
    double best = 0.0;
    double sumSq = 0.0;
    int64_t bestLag = -1;
    for (int64_t lag = 0; lag < maxLag; ++lag) {
      double c = 0.0;
      const float* x = calCapture_.data() + start + lag;
      for (int64_t i = 0; i < n; ++i) c += static_cast<double>(x[i]) * calPulse_[static_cast<size_t>(i)];
      sumSq += c * c;
      if (std::fabs(c) > best) {
        best = std::fabs(c);
        bestLag = lag;
      }
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(maxLag));
    if (bestLag >= 0 && best > 1e-4 && best > 6.0 * rms) lags.push_back(bestLag);
  }
  if (lags.size() < 5) return -1;
  std::sort(lags.begin(), lags.end());
  const int64_t median = lags[lags.size() / 2];
  // The detected pulses must agree within 1 ms, or it was noise.
  const int64_t spread = std::max<int64_t>(2, config_.sampleRate / 1000);
  int agreeing = 0;
  for (int64_t lag : lags) {
    if (std::llabs(lag - median) <= spread) ++agreeing;
  }
  if (agreeing < 5) return -1;
  if (frames) *frames = static_cast<int32_t>(median);
  return 1;
}

int Engine::importLoop(int track, const float* data, int64_t frames, int64_t targetFrames,
                       bool locked, int64_t origin, int trimEighths, int64_t length) {
  if (track < 0 || track >= kMaxTracks || !data || frames <= 0) return -2;
  if (targetFrames <= 0 || targetFrames > static_cast<int64_t>(capacity_)) return -2;
  if (locked) {
    origin = 0;
    trimEighths = 0;
    length = targetFrames;
  }
  const int64_t start = origin + eighthsToFrames(trimEighths);
  if (length <= 0 || origin < 0 || start < 0 || start >= targetFrames ||
      start + length > static_cast<int64_t>(capacity_)) {
    return -2;
  }
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
  cmd.d = static_cast<double>(length);
  cmd.i = locked ? 1 : 0;
  cmd.a = origin;
  cmd.b = targetFrames;
  cmd.c = trimEighths;
  return send(cmd) ? 0 : -1;
}

}  // namespace looper
