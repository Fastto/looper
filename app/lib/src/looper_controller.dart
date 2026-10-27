import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:looper_engine/looper_engine.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'session/session.dart';
import 'session/session_manager.dart';

/// Per-track settings owned by the UI (re-applied whenever the device reopens).
class TrackSettings {
  TrackSettings({this.input = 0, this.lengthMode = 0, this.volume = 0.8});

  int input;
  int lengthMode;
  double volume;
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
  final List<TrackSettings> tracks = List.generate(maxTracks, (_) => TrackSettings());

  final Map<int, (int, List<double>)> _waveforms = {};
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

  /// Cached waveform of a playing loop: refetched when its audio changes,
  /// and a few times per second while an overdub is being recorded.
  List<double> waveform(int track) {
    final t = status.tracks[track];
    if (t.state != TrackState.playing) return const [];
    final live = t.overdubbing ? _frame ~/ 8 : 0;
    final key = Object.hash(t.recStart, t.length, t.layerVersion, live);
    final cached = _waveforms[track];
    if (cached != null && cached.$1 == key) return cached.$2;
    final data = _engine.waveform(track, 240);
    _waveforms[track] = (key, data);
    return data;
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
    await sessions.flush();
    _waveforms.clear();
    _layerHistory.clear();
    _open();
    notifyListeners();
    await sessions.reloadAfterDeviceChange();
  }

  /// Waits until the audio thread has applied every command sent so far.
  Future<void> waitIdle({Duration timeout = const Duration(seconds: 1)}) async {
    final deadline = DateTime.now().add(timeout);
    while (_engine.commandsPending > 0 && DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 5));
    }
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
          ..input = t.input
          ..lengthMode = t.lengthMode;
        _engine
          ..trackSetVolume(i, t.volume)
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
      ..setCountIn(countInBars);
    _applyLatency();
    for (var i = 0; i < maxTracks; i++) {
      _engine
        ..trackSetInput(i, tracks[i].input)
        ..trackSetLengthMode(i, tracks[i].lengthMode)
        ..trackSetVolume(i, tracks[i].volume);
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
    for (var i = 0; i < maxTracks; i++) {
      tracks[i]
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
      ..setInt('countInBars', countInBars);
    for (var i = 0; i < maxTracks; i++) {
      p
        ..setInt('track$i.input', tracks[i].input)
        ..setInt('track$i.lengthMode', tracks[i].lengthMode)
        ..setDouble('track$i.volume', tracks[i].volume);
    }
  }
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
