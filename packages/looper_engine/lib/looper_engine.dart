/// Dart API for the native looper engine.
///
/// All methods are synchronous and cheap: commands are queued for the audio
/// thread, and [status] copies the latest snapshot published by it.
library;

import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

import 'looper_engine_bindings_generated.dart';

const int maxTracks = LOOPER_MAX_TRACKS;
const int maxInputs = LOOPER_MAX_INPUTS;

enum TrackState { empty, armed, recording, playing }

/// The last overdub layer of a track (one level of undo/redo).
enum LayerState { none, recording, active, undone }

class AudioDevice {
  const AudioDevice({required this.index, required this.name, required this.isDefault});

  factory AudioDevice.fromJson(Map<String, dynamic> json) => AudioDevice(
    index: json['index'] as int,
    name: json['name'] as String,
    isDefault: json['isDefault'] as bool,
  );

  final int index;
  final String name;
  final bool isDefault;
}

class AudioDevices {
  const AudioDevices({required this.capture, required this.playback});

  final List<AudioDevice> capture;
  final List<AudioDevice> playback;
}

class TrackStatus {
  const TrackStatus({
    required this.state,
    required this.muted,
    required this.inputChannel,
    required this.lengthMode,
    required this.layerState,
    required this.layerVersion,
    required this.recStart,
    required this.recEnd,
    required this.length,
    required this.volume,
    required this.peak,
  });

  static const empty = TrackStatus(
    state: TrackState.empty,
    muted: false,
    inputChannel: 0,
    lengthMode: 0,
    layerState: LayerState.none,
    layerVersion: 0,
    recStart: 0,
    recEnd: -1,
    length: 0,
    volume: 1,
    peak: 0,
  );

  final TrackState state;
  final bool muted;
  final int inputChannel;

  /// 0 = auto, otherwise a fixed length in half-bars.
  final int lengthMode;
  final LayerState layerState;

  /// Changes whenever the loop's audio changes (new take, layer, undo/redo).
  final int layerVersion;

  bool get overdubbing => layerState == LayerState.recording;
  bool get canUndo => layerState == LayerState.active || layerState == LayerState.recording;
  bool get canRedo => layerState == LayerState.undone;

  /// Transport frame where the loop starts (always on a bar line).
  final int recStart;

  /// Transport frame where recording ends; -1 while open-ended.
  final int recEnd;

  /// Loop length in frames (only while playing).
  final int length;
  final double volume;
  final double peak;
}

class EngineStatus {
  const EngineStatus({
    required this.deviceRunning,
    required this.playing,
    required this.sampleRate,
    required this.inputChannels,
    required this.beatsPerBar,
    required this.metronomeOn,
    required this.metronomeVolume,
    required this.monitoringOn,
    required this.countInBars,
    required this.latencyFrames,
    required this.bpm,
    required this.position,
    required this.barFrames,
    required this.blockCounter,
    required this.inputPeaks,
    required this.tracks,
  });

  static final idle = EngineStatus(
    deviceRunning: false,
    playing: false,
    sampleRate: 48000,
    inputChannels: 0,
    beatsPerBar: 4,
    metronomeOn: true,
    metronomeVolume: 0.5,
    monitoringOn: false,
    countInBars: 1,
    latencyFrames: 0,
    bpm: 120,
    position: 0,
    barFrames: 96000,
    blockCounter: 0,
    inputPeaks: const [],
    tracks: List.filled(maxTracks, TrackStatus.empty),
  );

  final bool deviceRunning;
  final bool playing;
  final int sampleRate;
  final int inputChannels;
  final int beatsPerBar;
  final bool metronomeOn;
  final double metronomeVolume;
  final bool monitoringOn;
  final int countInBars;
  final int latencyFrames;
  final double bpm;

  /// Transport position in frames; negative during the count-in.
  final int position;
  final int barFrames;
  final int blockCounter;
  final List<double> inputPeaks;
  final List<TrackStatus> tracks;

  double get beatFrames => barFrames / beatsPerBar;

  bool get anyTrackHoldsAudio => tracks.any((t) => t.state != TrackState.empty);
}

class LooperEngine {
  LooperEngine() : _status = calloc<LooperStatus>(), _wave = calloc<Float>(_maxWaveBins);

  static const int _maxWaveBins = 1024;

  final Pointer<LooperStatus> _status;
  final Pointer<Float> _wave;

  bool init() => _bindings.looper_init() == 0;

  void dispose() {
    _bindings.looper_shutdown();
    calloc.free(_status);
    calloc.free(_wave);
  }

  AudioDevices listDevices() {
    final ptr = _bindings.looper_list_devices();
    try {
      final json = jsonDecode(ptr.cast<Utf8>().toDartString()) as Map<String, dynamic>;
      List<AudioDevice> parse(String key) =>
          (json[key] as List).map((e) => AudioDevice.fromJson(e as Map<String, dynamic>)).toList();
      return AudioDevices(capture: parse('capture'), playback: parse('playback'));
    } finally {
      _bindings.looper_free_string(ptr);
    }
  }

  /// Opens the device. Indices come from [listDevices]; -1 = system default.
  /// Returns null on success, or an error message.
  String? open({int captureIndex = -1, int playbackIndex = -1, int sampleRate = 0, int bufferFrames = 128}) {
    final result = _bindings.looper_open(captureIndex, playbackIndex, sampleRate, bufferFrames);
    if (result == 0) return null;
    return _bindings.looper_last_error().cast<Utf8>().toDartString();
  }

  void close() => _bindings.looper_close();

  /// Round-trip latency reported by the OS, in frames (-1 if unknown).
  int get reportedLatencyFrames => _bindings.looper_reported_latency_frames();

  void play() => _bindings.looper_play();
  void stop() => _bindings.looper_stop();
  void setTempo(double bpm, int beatsPerBar) => _bindings.looper_set_tempo(bpm, beatsPerBar);
  void setMetronome({required bool on, required double volume}) =>
      _bindings.looper_set_metronome(on ? 1 : 0, volume);
  void setMonitoring(bool on) => _bindings.looper_set_monitoring(on ? 1 : 0);
  void setCountIn(int bars) => _bindings.looper_set_count_in(bars);
  void setLatencyFrames(int frames) => _bindings.looper_set_latency_frames(frames);

  void trackTap(int track) => _bindings.looper_track_tap(track);
  void trackClear(int track) => _bindings.looper_track_clear(track);
  void trackOverdub(int track) => _bindings.looper_track_overdub(track);
  void trackUndo(int track) => _bindings.looper_track_undo(track);
  void trackRedo(int track) => _bindings.looper_track_redo(track);
  void trackSetMuted(int track, bool muted) => _bindings.looper_track_set_muted(track, muted ? 1 : 0);
  void trackSetVolume(int track, double volume) => _bindings.looper_track_set_volume(track, volume);
  void trackSetInput(int track, int channel) => _bindings.looper_track_set_input(track, channel);
  void trackSetLengthMode(int track, int halfBars) => _bindings.looper_track_set_length_mode(track, halfBars);

  EngineStatus status() {
    _bindings.looper_get_status(_status);
    final s = _status.ref;
    return EngineStatus(
      deviceRunning: s.device_running != 0,
      playing: s.playing != 0,
      sampleRate: s.sample_rate,
      inputChannels: s.input_channels,
      beatsPerBar: s.beats_per_bar,
      metronomeOn: s.metronome_on != 0,
      metronomeVolume: s.metronome_volume,
      monitoringOn: s.monitoring_on != 0,
      countInBars: s.count_in_bars,
      latencyFrames: s.latency_frames,
      bpm: s.bpm,
      position: s.position,
      barFrames: s.bar_frames,
      blockCounter: s.block_counter,
      inputPeaks: List.generate(s.input_channels.clamp(0, maxInputs), (i) => s.input_peaks[i]),
      tracks: List.generate(maxTracks, (i) {
        final t = s.tracks[i];
        return TrackStatus(
          state: TrackState.values[t.state.clamp(0, TrackState.values.length - 1)],
          muted: t.muted != 0,
          inputChannel: t.input_channel,
          lengthMode: t.length_mode,
          layerState: LayerState.values[t.layer_state.clamp(0, LayerState.values.length - 1)],
          layerVersion: t.layer_version,
          recStart: t.rec_start,
          recEnd: t.rec_end,
          length: t.length,
          volume: t.volume,
          peak: t.peak,
        );
      }),
    );
  }

  /// Peak envelope of a playing track's loop, [bins] values in 0..1.
  List<double> waveform(int track, int bins) {
    final n = _bindings.looper_get_waveform(track, _wave, bins.clamp(1, _maxWaveBins));
    return List.generate(n, (i) => _wave[i]);
  }
}

const String _libName = 'looper_engine';

final DynamicLibrary _dylib = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.open('$_libName.framework/$_libName');
  }
  if (Platform.isAndroid || Platform.isLinux) {
    return DynamicLibrary.open('lib$_libName.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('$_libName.dll');
  }
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}();

final LooperEngineBindings _bindings = LooperEngineBindings(_dylib);
