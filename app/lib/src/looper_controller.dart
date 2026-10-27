import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:looper_engine/looper_engine.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'export/audio_export.dart';
import 'session/session.dart';
import 'session/session_manager.dart';

/// Per-track settings owned by the UI (re-applied whenever the device reopens).
class TrackSettings {
  TrackSettings({this.input = 0, this.lengthMode = 0, this.volume = 0.8, this.pan = 0});

  int input;
  int lengthMode;
  double volume;
  double pan;
}

/// Allowed fixed loop lengths, in half-bars. 0 = auto.
const List<int> lengthModes = [0, 1, 2, 4, 8, 16];

String lengthModeLabel(int halfBars) => switch (halfBars) {
  0 => 'Auto',
  1 => '½',
  _ => '${halfBars ~/ 2}',
};

/// Bridges the native engine and the widgets. Widgets call actions; the
/// home page calls [poll] once per frame to refresh [status].
class LooperController extends ChangeNotifier {
  /// [sessionsRoot] overrides where sessions are stored (tests).
  LooperController({LooperEngine? engine, Directory? sessionsRoot})
    : _engine = engine ?? LooperEngine(),
      _sessionsRoot = sessionsRoot;

  final Directory? _sessionsRoot;

  final LooperEngine _engine;
  SharedPreferences? _prefs;

  /// The current session: autosave, restore, sessions browser.
  late final SessionManager sessions = SessionManager(this, root: _sessionsRoot);

  LooperEngine get engine => _engine;

  EngineStatus status = EngineStatus.idle;
  AudioDevices devices = const AudioDevices(capture: [], playback: []);
  String? error;
  bool started = false;

  // Settings (persisted).
  String? captureName;
  String? playbackName;
  int bufferFrames = 128;
  double latencyOffsetMs = 0;
  double bpm = 120;
  int beatsPerBar = 4;
  bool metronomeOn = true;
  double metronomeVolume = 0.5;
  bool monitoring = false;
  int countInBars = 1;

  /// Mute/unmute waits for: 0 = nothing, 1 = beat, 2 = bar.
  int muteQuantize = 0;

  /// Share of the loop that survives each overdub pass (1 = everything).
  double feedback = 1;
  final List<TrackSettings> tracks = List.generate(maxTracks, (_) => TrackSettings());

  final Map<int, (int, WaveformView)> _waveforms = {};

  /// Trim step for shifting a loop's start, in eighth notes; 0 = one bar.
  int trimStep = 1;
  final List<DateTime> _taps = [];
  int _frame = 0;

  /// Tracks in the order they were overdubbed, for global undo/redo.
  final List<int> _layerHistory = [];

  Future<void> start() async {
    _prefs = await SharedPreferences.getInstance();
    _load();
    if (!_engine.init()) {
      error = 'Could not initialize audio.';
      notifyListeners();
      return;
    }
    devices = _engine.listDevices();
    _open();
    started = true;
    notifyListeners();
    await sessions.start();
  }

  @override
  void dispose() {
    sessions.dispose();
    _engine.dispose();
    super.dispose();
  }

  // ---------------------------------------------------------------- polling

  void poll() {
    _frame++;
    final next = _engine.status();
    // Without an open device the engine reports zeros; keep a sane grid.
    status = next.deviceRunning ? next : EngineStatus.idle;
    if (_frame % 10 == 0) sessions.onPoll(status);
    notifyListeners();
  }

  /// Cached waveform of a playing track: while the trim can change it shows
  /// the whole recording with the loop window marked; once locked, the loop.
  /// Refetched when the audio changes, and a few times per second while an
  /// overdub is being recorded.
  WaveformView waveform(int track) {
    final t = status.tracks[track];
    if (t.state != TrackState.playing) return WaveformView.empty;
    final live = t.overdubbing ? _frame ~/ 8 : 0;
    final key = Object.hash(t.windowStart, t.length, t.audioEnd, t.trimLocked, t.layerVersion, live);
    final cached = _waveforms[track];
    if (cached != null && cached.$1 == key) return cached.$2;
    final windowEnd = t.windowStart + t.length;
    final from = t.trimLocked ? t.windowStart : math.min(0, t.windowStart);
    final to = t.trimLocked ? windowEnd : math.max(t.audioEnd, windowEnd);
    final span = math.max(1, to - from);
    final view = WaveformView(
      peaks: _engine.waveform(track, from, to, 240),
      windowFrom: (t.windowStart - from) / span,
      windowTo: (windowEnd - from) / span,
    );
    _waveforms[track] = (key, view);
    return view;
  }

  // ---------------------------------------------------------------- device

  void refreshDevices() {
    devices = _engine.listDevices();
    notifyListeners();
  }

  /// Reopens the audio device. The session is saved first and reloaded
  /// afterwards, so loops survive a device or sample-rate change.
  Future<void> selectDevices({String? capture, String? playback, int? buffer}) async {
    captureName = capture ?? captureName;
    playbackName = playback ?? playbackName;
    bufferFrames = buffer ?? bufferFrames;
    _save();
    await stopAndSettle();
    await sessions.flush();
    _waveforms.clear();
    _layerHistory.clear();
    _open();
    notifyListeners();
    await sessions.reloadAfterDeviceChange();
  }

  /// Stops the transport and waits for its fade-out, so closing the audio
  /// device (device change, quit) never cuts a sound off mid-wave.
  Future<void> stopAndSettle() async {
    if (!status.deviceRunning || !_engine.status().playing) return;
    _engine.stop();
    await waitIdle();
    await Future<void>.delayed(const Duration(milliseconds: 80));
  }

  /// Waits until the audio thread has applied every command sent so far.
  Future<void> waitIdle({Duration timeout = const Duration(seconds: 1)}) async {
    final deadline = DateTime.now().add(timeout);
    while (_engine.commandsPending > 0 && DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 5));
    }
  }

  /// Loop lengths of the playing tracks, in half-bars (export length hints).
  Map<int, int> loopHalfBars({bool includeMuted = true}) {
    final halfBar = status.barFrames ~/ 2;
    return {
      for (var i = 0; i < maxTracks; i++)
        if (status.tracks[i].state == TrackState.playing &&
            (includeMuted || !status.tracks[i].muted) &&
            halfBar > 0)
          i: (status.tracks[i].length / halfBar).round(),
    };
  }

  /// The audible loops, one cycle each, waiting briefly for tracks that are
  /// still being recorded into or merged.
  Future<List<ExportLoop>> collectLoops({required bool includeMuted}) async {
    final loops = <ExportLoop>[];
    for (var i = 0; i < maxTracks; i++) {
      final deadline = DateTime.now().add(const Duration(seconds: 4));
      while (true) {
        final t = _engine.status().tracks[i];
        if (t.state == TrackState.empty || (t.muted && !includeMuted)) break;
        final audio = t.state == TrackState.playing ? _engine.trackExport(i) : null;
        if (audio != null) {
          if (audio.isNotEmpty) {
            loops.add(
              ExportLoop(track: i, audio: audio, gain: tracks[i].volume * t.normGain, pan: tracks[i].pan),
            );
          }
          break;
        }
        if (DateTime.now().isAfter(deadline)) break;
        await Future<void>.delayed(const Duration(milliseconds: 50));
      }
    }
    return loops;
  }

  /// Stops, clears every track and applies session-level settings.
  Future<void> resetForSession({
    required double bpm,
    required int beatsPerBar,
    List<SessionTrack>? tracks,
  }) async {
    _engine.stop();
    for (var i = 0; i < maxTracks; i++) {
      _engine.trackClear(i);
    }
    this.bpm = bpm.clamp(40, 240).toDouble();
    this.beatsPerBar = beatsPerBar;
    _engine.setTempo(this.bpm, this.beatsPerBar);
    if (tracks != null) {
      for (var i = 0; i < maxTracks; i++) {
        final t = i < tracks.length ? tracks[i] : const SessionTrack();
        this.tracks[i]
          ..volume = t.volume
          ..pan = t.pan
          ..input = t.input
          ..lengthMode = t.lengthMode;
        _engine
          ..trackSetVolume(i, t.volume)
          ..trackSetPan(i, t.pan)
          ..trackSetInput(i, t.input)
          ..trackSetLengthMode(i, t.lengthMode);
      }
    }
    _waveforms.clear();
    _layerHistory.clear();
    _save();
    await waitIdle();
    status = _engine.status();
    notifyListeners();
  }

  void _open() {
    int indexOf(List<AudioDevice> list, String? name) =>
        name == null ? -1 : list.indexWhere((d) => d.name == name);
    error = _engine.open(
      captureIndex: indexOf(devices.capture, captureName),
      playbackIndex: indexOf(devices.playback, playbackName),
      bufferFrames: bufferFrames,
    );
    if (error != null) return;
    status = _engine.status(); // sample rate is needed by _applyLatency
    _applyAll();
  }

  void _applyAll() {
    _engine
      ..setTempo(bpm, beatsPerBar)
      ..setMetronome(on: metronomeOn, volume: metronomeVolume)
      ..setMonitoring(monitoring)
      ..setCountIn(countInBars)
      ..setMuteQuantize(muteQuantize)
      ..setFeedback(feedback);
    _applyLatency();
    for (var i = 0; i < maxTracks; i++) {
      _engine
        ..trackSetInput(i, tracks[i].input)
        ..trackSetLengthMode(i, tracks[i].lengthMode)
        ..trackSetVolume(i, tracks[i].volume)
        ..trackSetPan(i, tracks[i].pan);
    }
  }

  int get reportedLatencyFrames => math.max(0, _engine.reportedLatencyFrames);

  void _applyLatency() {
    final offset = (latencyOffsetMs / 1000 * status.sampleRate).round();
    _engine.setLatencyFrames(math.max(0, reportedLatencyFrames + offset));
  }

  void setLatencyOffsetMs(double ms) {
    latencyOffsetMs = ms;
    _applyLatency();
    _save();
    notifyListeners();
  }

  // ---------------------------------------------------------------- transport

  void togglePlay() => status.playing ? _engine.stop() : _engine.play();

  bool get tempoLocked => status.anyTrackHoldsAudio;

  void setBpm(double value) {
    if (tempoLocked) return;
    bpm = value.clamp(40, 240).roundToDouble();
    _engine.setTempo(bpm, beatsPerBar);
    _save();
    notifyListeners();
  }

  void setBeatsPerBar(int value) {
    if (tempoLocked) return;
    beatsPerBar = value;
    _engine.setTempo(bpm, beatsPerBar);
    _save();
    notifyListeners();
  }

  /// Tap tempo: average of the recent intervals.
  void tapTempo() {
    final now = DateTime.now();
    if (_taps.isNotEmpty && now.difference(_taps.last).inMilliseconds > 2000) _taps.clear();
    _taps.add(now);
    if (_taps.length > 5) _taps.removeAt(0);
    if (_taps.length < 2) return;
    final ms = _taps.last.difference(_taps.first).inMilliseconds / (_taps.length - 1);
    setBpm(60000 / ms);
  }

  void toggleMetronome() => setMetronome(on: !metronomeOn);

  void setMetronome({bool? on, double? volume}) {
    metronomeOn = on ?? metronomeOn;
    metronomeVolume = volume ?? metronomeVolume;
    _engine.setMetronome(on: metronomeOn, volume: metronomeVolume);
    _save();
    notifyListeners();
  }

  void setMonitoring(bool on) {
    monitoring = on;
    _engine.setMonitoring(on);
    _save();
    notifyListeners();
  }

  void setCountIn(int bars) {
    countInBars = bars;
    _engine.setCountIn(bars);
    _save();
    notifyListeners();
  }

  void setMuteQuantize(int mode) {
    muteQuantize = mode;
    _engine.setMuteQuantize(mode);
    _save();
    notifyListeners();
  }

  void setFeedback(double value) {
    feedback = value.clamp(0, 1).toDouble();
    _engine.setFeedback(feedback);
    _save();
    notifyListeners();
  }

  // ---------------------------------------------------------------- calibration

  bool calibrating = false;

  /// Measures the round trip through a loopback on [channel] and returns it
  /// in milliseconds. Throws [StateError] if nothing clear was heard.
  Future<double> measureLatency(int channel) async {
    if (calibrating) throw StateError('already measuring');
    calibrating = true;
    notifyListeners();
    try {
      await stopAndSettle();
      _engine.calibrateStart(channel);
      await waitIdle();
      final deadline = DateTime.now().add(const Duration(seconds: 6));
      while (DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
        final frames = _engine.calibrateResult(); // throws if nothing was heard
        if (frames != null && !_engine.status().calibrating) {
          return frames / status.sampleRate * 1000;
        }
      }
      throw StateError('measurement timed out');
    } finally {
      calibrating = false;
      notifyListeners();
    }
  }

  /// Uses a measured round trip: the offset becomes whatever the system
  /// report was missing.
  void applyMeasuredLatency(double ms) {
    final reportedMs = reportedLatencyFrames / status.sampleRate * 1000;
    setLatencyOffsetMs((ms - reportedMs).roundToDouble());
  }

  // ---------------------------------------------------------------- normalize

  /// Peak level a normalized loop is brought to: -1 dBFS.
  static const normalizeTarget = 0.891;

  /// Most a quiet loop is boosted: +24 dB (more would mostly raise noise).
  static const maxNormGain = 15.85;

  /// Normalizes a playing loop so its loudest peak hits -1 dBFS, or switches
  /// normalization off if it is on. Returns the new gain in dB, or null if
  /// the loop was silent or not ready.
  Future<double?> toggleNormalize(int i) async {
    final t = _engine.status().tracks[i];
    if (t.state != TrackState.playing) return null;
    if (t.normalized) {
      _engine.trackSetNormGain(i, 1);
      return 0;
    }
    final deadline = DateTime.now().add(const Duration(seconds: 3));
    Float32List? audio;
    while ((audio = _engine.trackExport(i)) == null && DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    if (audio == null || audio.isEmpty) return null;
    var peak = 0.0;
    for (final x in audio) {
      final a = x.abs();
      if (a > peak) peak = a;
    }
    if (peak < 1e-4) return null;
    final gain = (normalizeTarget / peak).clamp(0.0, maxNormGain).toDouble();
    _engine.trackSetNormGain(i, gain);
    return 20 * math.log(gain) / math.ln10;
  }

  // ---------------------------------------------------------------- trim

  int get _stepEighths => trimStep == 0 ? 2 * status.beatsPerBar : trimStep;

  /// Cycles the trim step: ⅛ → ¼ → bar.
  void cycleTrimStep() {
    trimStep = switch (trimStep) {
      1 => 2,
      2 => 0,
      _ => 1,
    };
    _save();
    notifyListeners();
  }

  String get trimStepLabel => switch (trimStep) {
    1 => '⅛',
    2 => '¼',
    _ => 'bar',
  };

  bool _canTrim(TrackStatus t) => t.state == TrackState.playing && !t.trimLocked && !t.overdubbing;

  /// Whether the start can move by one step in [direction] (-1 earlier, +1 later).
  bool canShiftStart(int i, int direction) {
    final t = status.tracks[i];
    final target = t.trimEighths + direction * _stepEighths;
    return _canTrim(t) && target >= t.trimMinEighths && target <= t.trimMaxEighths;
  }

  void shiftStart(int i, int direction) {
    if (canShiftStart(i, direction)) _engine.trackShiftStart(i, direction * _stepEighths);
  }

  /// Whether a bar can be removed (-1) or added back (+1) at the end.
  bool canTrimBars(int i, int delta) {
    final t = status.tracks[i];
    if (!_canTrim(t) || t.trimMaxBars == 0) return false;
    final bars = t.length ~/ status.barFrames + delta;
    return bars >= 1 && bars <= t.trimMaxBars;
  }

  void trimBars(int i, int delta) {
    if (canTrimBars(i, delta)) _engine.trackTrimBars(i, delta);
  }

  // ---------------------------------------------------------------- tracks

  void tapTrack(int i) => _engine.trackTap(i);

  /// Starts or stops layering on top of a playing loop.
  void toggleOverdub(int i) {
    final t = status.tracks[i];
    if (t.state != TrackState.playing || (!status.playing && !t.overdubbing)) return;
    if (!t.overdubbing) {
      _layerHistory
        ..remove(i)
        ..add(i);
    }
    _engine.trackOverdub(i);
  }

  void undoTrack(int i) => _engine.trackUndo(i);

  void redoTrack(int i) => _engine.trackRedo(i);

  /// Undo the most recent layer that can be undone, on any track.
  void undoLast() {
    for (final i in _layerHistory.reversed) {
      if (status.tracks[i].canUndo) return undoTrack(i);
    }
  }

  /// Redo the most recently overdubbed track whose layer is undone.
  void redoLast() {
    for (final i in _layerHistory.reversed) {
      if (status.tracks[i].canRedo) return redoTrack(i);
    }
  }

  void clearTrack(int i) {
    _engine.trackClear(i);
    _waveforms.remove(i);
    _layerHistory.remove(i);
  }

  void toggleMute(int i) => _engine.trackSetMuted(i, !status.tracks[i].muted);

  void setTrackVolume(int i, double v) {
    tracks[i].volume = v;
    _engine.trackSetVolume(i, v);
    _save();
    notifyListeners();
  }

  void setTrackPan(int i, double pan) {
    tracks[i].pan = pan.clamp(-1, 1).toDouble();
    _engine.trackSetPan(i, tracks[i].pan);
    _save();
    notifyListeners();
  }

  void setTrackInput(int i, int channel) {
    tracks[i].input = channel;
    _engine.trackSetInput(i, channel);
    _save();
    notifyListeners();
  }

  void setTrackLengthMode(int i, int halfBars) {
    tracks[i].lengthMode = halfBars;
    _engine.trackSetLengthMode(i, halfBars);
    _save();
    notifyListeners();
  }

  // ---------------------------------------------------------------- persistence

  void _load() {
    final p = _prefs!;
    captureName = p.getString('captureName');
    playbackName = p.getString('playbackName');
    bufferFrames = p.getInt('bufferFrames') ?? bufferFrames;
    latencyOffsetMs = p.getDouble('latencyOffsetMs') ?? latencyOffsetMs;
    bpm = p.getDouble('bpm') ?? bpm;
    beatsPerBar = p.getInt('beatsPerBar') ?? beatsPerBar;
    metronomeOn = p.getBool('metronomeOn') ?? metronomeOn;
    metronomeVolume = p.getDouble('metronomeVolume') ?? metronomeVolume;
    monitoring = p.getBool('monitoring') ?? monitoring;
    countInBars = p.getInt('countInBars') ?? countInBars;
    muteQuantize = p.getInt('muteQuantize') ?? muteQuantize;
    feedback = p.getDouble('feedback') ?? feedback;
    trimStep = p.getInt('trimStep') ?? trimStep;
    for (var i = 0; i < maxTracks; i++) {
      tracks[i]
        ..pan = p.getDouble('track$i.pan') ?? 0
        ..input = p.getInt('track$i.input') ?? 0
        ..lengthMode = p.getInt('track$i.lengthMode') ?? 0
        ..volume = p.getDouble('track$i.volume') ?? 0.8;
    }
  }

  void _save() {
    final p = _prefs;
    if (p == null) return;
    void setString(String k, String? v) => v == null ? p.remove(k) : p.setString(k, v);
    setString('captureName', captureName);
    setString('playbackName', playbackName);
    p
      ..setInt('bufferFrames', bufferFrames)
      ..setDouble('latencyOffsetMs', latencyOffsetMs)
      ..setDouble('bpm', bpm)
      ..setInt('beatsPerBar', beatsPerBar)
      ..setBool('metronomeOn', metronomeOn)
      ..setDouble('metronomeVolume', metronomeVolume)
      ..setBool('monitoring', monitoring)
      ..setInt('countInBars', countInBars)
      ..setInt('muteQuantize', muteQuantize)
      ..setDouble('feedback', feedback)
      ..setInt('trimStep', trimStep);
    for (var i = 0; i < maxTracks; i++) {
      p
        ..setDouble('track$i.pan', tracks[i].pan)
        ..setInt('track$i.input', tracks[i].input)
        ..setInt('track$i.lengthMode', tracks[i].lengthMode)
        ..setDouble('track$i.volume', tracks[i].volume);
    }
  }
}

/// Beats left until an armed track starts recording (the count-in from a
/// stopped transport, or the wait for the next bar while playing); null
/// when nothing is waiting.
int? beatsUntilRecording(EngineStatus s) {
  if (!s.playing) return null;
  int? wait;
  for (final t in s.tracks) {
    if (t.state == TrackState.armed && t.recStart > s.position) {
      final frames = t.recStart - s.position;
      if (wait == null || frames < wait) wait = frames;
    }
  }
  return wait == null ? null : (wait / s.beatFrames).ceil();
}

/// Musical position derived from an engine snapshot.
class BeatPosition {
  BeatPosition(EngineStatus s)
    : bar = (s.position / s.barFrames).floor(),
      beatInBar = ((s.position / s.beatFrames).floor() % s.beatsPerBar),
      beatPhase = (s.position / s.beatFrames) - (s.position / s.beatFrames).floor();

  /// 0-based bar index; negative during the count-in.
  final int bar;
  final int beatInBar;

  /// 0..1 progress through the current beat.
  final double beatPhase;
}

/// What the track waveform shows: peaks over a span of the recording and
/// where the loop window sits in it (fractions 0..1).
class WaveformView {
  const WaveformView({required this.peaks, required this.windowFrom, required this.windowTo});

  static const empty = WaveformView(peaks: [], windowFrom: 0, windowTo: 1);

  final List<double> peaks;
  final double windowFrom;
  final double windowTo;
}
