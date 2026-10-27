// C API implementation: owns the miniaudio context/device and the Engine.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "engine.hpp"
#include "looper_api.h"
#include "third_party/miniaudio/miniaudio.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define LOOPER_COREAUDIO_LATENCY 1
#include <CoreAudio/CoreAudio.h>
#endif
#endif

namespace {

struct State {
  bool contextReady = false;
  ma_context context{};
  ma_device device{};
  bool deviceOpen = false;
  std::unique_ptr<looper::Engine> engine;
  std::vector<ma_device_info> captureDevices;
  std::vector<ma_device_info> playbackDevices;
  std::string lastError;
  int32_t reportedLatency = -1;
};

State& state() {
  static State s;
  return s;
}

void setError(const std::string& message) { state().lastError = message; }

void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
  auto* engine = static_cast<looper::Engine*>(device->pUserData);
  if (!engine) return;
  engine->process(static_cast<const float*>(input), static_cast<float*>(output), frames);
}

void appendJsonString(std::string& out, const char* s) {
  out += '"';
  for (const char* c = s; *c; ++c) {
    switch (*c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (static_cast<unsigned char>(*c) < 0x20) continue;
        out += *c;
    }
  }
  out += '"';
}

void appendDeviceList(std::string& out, const std::vector<ma_device_info>& devices) {
  out += '[';
  for (size_t i = 0; i < devices.size(); ++i) {
    if (i) out += ',';
    out += "{\"index\":" + std::to_string(i) + ",\"name\":";
    appendJsonString(out, devices[i].name);
    out += ",\"isDefault\":";
    out += devices[i].isDefault ? "true" : "false";
    out += '}';
  }
  out += ']';
}

#if LOOPER_COREAUDIO_LATENCY
UInt32 coreAudioU32(AudioObjectID object, AudioObjectPropertySelector selector,
                    AudioObjectPropertyScope scope) {
  AudioObjectPropertyAddress address = {selector, scope, 0 /* main element */};
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) != noErr) return 0;
  return value;
}

// Device latency + safety offset + buffer + first stream latency, in frames.
int64_t coreAudioLatency(AudioObjectID deviceId, bool input) {
  if (deviceId == 0) return 0;
  const AudioObjectPropertyScope scope =
      input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
  int64_t total = coreAudioU32(deviceId, kAudioDevicePropertyLatency, scope) +
                  coreAudioU32(deviceId, kAudioDevicePropertySafetyOffset, scope) +
                  coreAudioU32(deviceId, kAudioDevicePropertyBufferFrameSize, scope);
  AudioObjectPropertyAddress streamsAddress = {kAudioDevicePropertyStreams, scope, 0};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(deviceId, &streamsAddress, 0, nullptr, &size) == noErr &&
      size >= sizeof(AudioStreamID)) {
    std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
    if (AudioObjectGetPropertyData(deviceId, &streamsAddress, 0, nullptr, &size,
                                   streams.data()) == noErr) {
      total += coreAudioU32(streams[0], kAudioStreamPropertyLatency,
                            kAudioObjectPropertyScopeGlobal);
    }
  }
  return total;
}
#endif

int32_t estimateLatency(ma_device& device) {
#if LOOPER_COREAUDIO_LATENCY
  if (device.pContext->backend == ma_backend_coreaudio) {
    const int64_t in = coreAudioLatency(device.coreaudio.deviceObjectIDCapture, true);
    const int64_t out = coreAudioLatency(device.coreaudio.deviceObjectIDPlayback, false);
    // Capture and playback run as separate units joined by a ring buffer of
    // one period inside miniaudio.
    return static_cast<int32_t>(in + out + device.playback.internalPeriodSizeInFrames);
  }
#endif
  // Generic estimate: one period in, one out, one for the duplex ring buffer.
  return static_cast<int32_t>(device.capture.internalPeriodSizeInFrames +
                              2 * device.playback.internalPeriodSizeInFrames);
}

template <typename Fn>
void sendCommand(looper::CommandType type, Fn fill) {
  auto& s = state();
  if (!s.engine) return;
  looper::Command cmd;
  cmd.type = type;
  fill(cmd);
  s.engine->send(cmd);
}

void sendCommand(looper::CommandType type) {
  sendCommand(type, [](looper::Command&) {});
}

}  // namespace

extern "C" {

int32_t looper_init(void) {
  auto& s = state();
  if (s.contextReady) return 0;
  ma_context_config config = ma_context_config_init();
  if (ma_context_init(nullptr, 0, &config, &s.context) != MA_SUCCESS) {
    setError("Failed to initialize audio context");
    return -1;
  }
  s.contextReady = true;
  return 0;
}

void looper_shutdown(void) {
  auto& s = state();
  looper_close();
  if (s.contextReady) {
    ma_context_uninit(&s.context);
    s.contextReady = false;
  }
}

char* looper_list_devices(void) {
  auto& s = state();
  std::string json = "{\"capture\":[],\"playback\":[]}";
  if (s.contextReady) {
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playbackCount = 0;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&s.context, &playback, &playbackCount, &capture, &captureCount) ==
        MA_SUCCESS) {
      s.playbackDevices.assign(playback, playback + playbackCount);
      s.captureDevices.assign(capture, capture + captureCount);
      // Fill in channel counts etc. (isDefault comes from the listing itself).
      json = "{\"capture\":";
      appendDeviceList(json, s.captureDevices);
      json += ",\"playback\":";
      appendDeviceList(json, s.playbackDevices);
      json += '}';
    } else {
      setError("Failed to enumerate devices");
    }
  }
  char* out = static_cast<char*>(std::malloc(json.size() + 1));
  std::memcpy(out, json.c_str(), json.size() + 1);
  return out;
}

void looper_free_string(char* s) { std::free(s); }

int32_t looper_open(int32_t capture_index, int32_t playback_index, int32_t sample_rate,
                    int32_t buffer_frames) {
  auto& s = state();
  if (!s.contextReady && looper_init() != 0) return -1;
  looper_close();

  ma_device_config config = ma_device_config_init(ma_device_type_duplex);
  config.capture.format = ma_format_f32;
  config.capture.channels = 0;  // native: expose every input of the interface
  config.capture.shareMode = ma_share_mode_shared;
  config.playback.format = ma_format_f32;
  config.playback.channels = 2;
  config.sampleRate = static_cast<ma_uint32>(std::max(sample_rate, 0));
  config.periodSizeInFrames = static_cast<ma_uint32>(buffer_frames > 0 ? buffer_frames : 128);
  config.performanceProfile = ma_performance_profile_low_latency;
  config.noPreSilencedOutputBuffer = MA_TRUE;  // the engine writes every sample
  config.noClip = MA_FALSE;
  config.dataCallback = dataCallback;
  config.pUserData = nullptr;
  if (capture_index >= 0 && capture_index < static_cast<int32_t>(s.captureDevices.size())) {
    config.capture.pDeviceID = &s.captureDevices[capture_index].id;
  }
  if (playback_index >= 0 && playback_index < static_cast<int32_t>(s.playbackDevices.size())) {
    config.playback.pDeviceID = &s.playbackDevices[playback_index].id;
  }

  if (ma_device_init(&s.context, &config, &s.device) != MA_SUCCESS) {
    setError("Failed to open audio device (is microphone access allowed?)");
    return -2;
  }
  s.deviceOpen = true;

  looper::EngineConfig engineConfig;
  engineConfig.sampleRate = static_cast<int>(s.device.sampleRate);
  engineConfig.inputChannels = static_cast<int>(s.device.capture.channels);
  engineConfig.outputChannels = static_cast<int>(s.device.playback.channels);
  s.engine = std::make_unique<looper::Engine>(engineConfig);
  s.device.pUserData = s.engine.get();
  s.reportedLatency = estimateLatency(s.device);

  if (ma_device_start(&s.device) != MA_SUCCESS) {
    setError("Failed to start audio device");
    looper_close();
    return -3;
  }
  return 0;
}

void looper_close(void) {
  auto& s = state();
  if (s.deviceOpen) {
    ma_device_uninit(&s.device);  // blocks until the callback has finished
    s.deviceOpen = false;
  }
  s.engine.reset();
  s.reportedLatency = -1;
}

const char* looper_last_error(void) { return state().lastError.c_str(); }

int32_t looper_reported_latency_frames(void) { return state().reportedLatency; }

void looper_play(void) { sendCommand(looper::CommandType::Play); }

void looper_stop(void) { sendCommand(looper::CommandType::Stop); }

void looper_set_tempo(double bpm, int32_t beats_per_bar) {
  sendCommand(looper::CommandType::SetTempo, [&](looper::Command& c) {
    c.d = bpm;
    c.i = beats_per_bar;
  });
}

void looper_set_metronome(int32_t on, float volume) {
  sendCommand(looper::CommandType::SetMetronome, [&](looper::Command& c) {
    c.i = on;
    c.d = volume;
  });
}

void looper_set_monitoring(int32_t on) {
  sendCommand(looper::CommandType::SetMonitoring, [&](looper::Command& c) { c.i = on; });
}

void looper_set_count_in(int32_t bars) {
  sendCommand(looper::CommandType::SetCountIn, [&](looper::Command& c) { c.i = bars; });
}

void looper_set_latency_frames(int32_t frames) {
  sendCommand(looper::CommandType::SetLatency, [&](looper::Command& c) { c.i = frames; });
}

void looper_set_mute_quantize(int32_t mode) {
  sendCommand(looper::CommandType::SetMuteQuantize, [&](looper::Command& c) { c.i = mode; });
}

void looper_set_feedback(float feedback) {
  sendCommand(looper::CommandType::SetFeedback, [&](looper::Command& c) { c.d = feedback; });
}

void looper_calibrate_start(int32_t channel) {
  sendCommand(looper::CommandType::CalibrationStart, [&](looper::Command& c) { c.i = channel; });
}

int32_t looper_calibrate_result(int32_t* frames) {
  auto& s = state();
  return s.engine ? s.engine->calibrationResult(frames) : 0;
}

void looper_track_set_pan(int32_t track, float pan) {
  sendCommand(looper::CommandType::TrackPan, [&](looper::Command& c) {
    c.track = track;
    c.d = pan;
  });
}

void looper_track_tap(int32_t track) {
  sendCommand(looper::CommandType::TrackTap, [&](looper::Command& c) { c.track = track; });
}

void looper_track_clear(int32_t track) {
  sendCommand(looper::CommandType::TrackClear, [&](looper::Command& c) { c.track = track; });
}

void looper_track_set_muted(int32_t track, int32_t muted) {
  sendCommand(looper::CommandType::TrackMute, [&](looper::Command& c) {
    c.track = track;
    c.i = muted;
  });
}

void looper_track_set_volume(int32_t track, float volume) {
  sendCommand(looper::CommandType::TrackVolume, [&](looper::Command& c) {
    c.track = track;
    c.d = volume;
  });
}

void looper_track_set_input(int32_t track, int32_t channel) {
  sendCommand(looper::CommandType::TrackInput, [&](looper::Command& c) {
    c.track = track;
    c.i = channel;
  });
}

void looper_track_set_length_mode(int32_t track, int32_t half_bars) {
  sendCommand(looper::CommandType::TrackLengthMode, [&](looper::Command& c) {
    c.track = track;
    c.i = half_bars;
  });
}

void looper_get_status(LooperStatus* out) {
  auto& s = state();
  if (!out) return;
  if (!s.engine) {
    std::memset(out, 0, sizeof(*out));
    return;
  }
  *out = s.engine->status();
}

int32_t looper_get_waveform(int32_t track, int64_t from, int64_t to, float* out, int32_t bins) {
  auto& s = state();
  if (!s.engine || !out) return 0;
  return s.engine->waveform(track, from, to, out, bins);
}

int32_t looper_commands_pending(void) {
  auto& s = state();
  return s.engine ? static_cast<int32_t>(s.engine->commandsPending()) : 0;
}

int64_t looper_max_loop_frames(void) {
  auto& s = state();
  return s.engine ? static_cast<int64_t>(s.engine->capacity()) : 0;
}

int64_t looper_track_export(int32_t track, float* out, int64_t capacity) {
  auto& s = state();
  return s.engine ? s.engine->exportLoop(track, out, capacity) : 0;
}

int64_t looper_track_export_recording(int32_t track, float* out, int64_t capacity) {
  auto& s = state();
  return s.engine ? s.engine->exportRecording(track, out, capacity) : 0;
}

int32_t looper_track_import(int32_t track, const float* data, int64_t frames,
                            int64_t target_frames, int32_t locked, int64_t origin,
                            int32_t trim_eighths, int64_t length) {
  auto& s = state();
  return s.engine ? s.engine->importLoop(track, data, frames, target_frames, locked != 0, origin,
                                         trim_eighths, length)
                  : -1;
}

void looper_track_shift_start(int32_t track, int32_t eighths) {
  sendCommand(looper::CommandType::TrackShiftStart, [&](looper::Command& c) {
    c.track = track;
    c.i = eighths;
  });
}

void looper_track_trim_bars(int32_t track, int32_t bars) {
  sendCommand(looper::CommandType::TrackTrimBars, [&](looper::Command& c) {
    c.track = track;
    c.i = bars;
  });
}

}  // extern "C"
