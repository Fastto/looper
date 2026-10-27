import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:looper_engine/looper_engine.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../looper_controller.dart';
import 'session.dart';
import 'session_store.dart';

/// Keeps the current session saved on disk and restores it on launch.
///
/// There is no "Save" button: every change is written shortly after it
/// happens (loops once they stop being recorded/layered), and the app
/// reopens where you left off. The sessions browser opens, renames,
/// duplicates and deletes sessions. See docs/DESIGN.md §7.
class SessionManager extends ChangeNotifier {
  SessionManager(this._c, {Directory? root}) : _root = root;

  final LooperController _c;
  final Directory? _root;
  SessionStore? _store;
  SharedPreferences? _prefs;

  String id = '';
  String name = '';
  DateTime _createdAt = DateTime.now();
  List<SessionInfo> sessions = const [];
  String? lastError;
  DateTime? lastSavedAt;
  bool _ready = false; // a session is loaded and the device is running
  bool _busy = false; // loading or switching sessions

  // What is on disk for the current session: per-track audio signature
  // (null = no file) and the manifest without its timestamp.
  final List<Object?> _savedAudio = List.filled(maxTracks, null);
  final List<int> _savedLength = List.filled(maxTracks, 0);
  // How each saved track was written: null = the loop itself (locked trim),
  // otherwise the whole recording and its window.
  final List<({double originBars, int eighths})?> _savedTrim = List.filled(maxTracks, null);
  String? _savedMeta;
  Timer? _debounce;
  bool _saving = false;
  bool _existsOnDisk = false; // cached store.exists(id), checked every poll

  static const _lastSessionKey = 'lastSessionId';
  static const _saveDelay = Duration(milliseconds: 800);

  String? get folder => _store?.root.path;

  @visibleForTesting
  SessionStore get store => _store!;

  bool get onDisk => _store?.exists(id) ?? false;

  /// Finds the sessions folder and reopens the last session.
  Future<void> start() async {
    _prefs = await SharedPreferences.getInstance();
    final root = _root ?? Directory('${(await getApplicationSupportDirectory()).path}/Sessions');
    _store = SessionStore(root);
    await refreshList();
    final last = _prefs!.getString(_lastSessionKey);
    if (last != null && _store!.exists(last)) {
      await open(last, saveCurrent: false);
    } else {
      await newSession(saveCurrent: false);
    }
  }

  Future<void> refreshList() async {
    final store = _store;
    if (store == null) return;
    sessions = await store.list();
    notifyListeners();
  }

  /// Called every frame by the controller: schedules a save when something
  /// changed since the last one.
  void onPoll(EngineStatus status) {
    if (!_ready || _busy || !status.deviceRunning || _debounce != null) return;
    if (_isDirty(status)) _debounce = Timer(_saveDelay, _autosave);
  }

  /// Writes everything that can be written now. Used before quitting,
  /// switching sessions and reopening the device.
  Future<void> flush() async {
    _debounce?.cancel();
    _debounce = null;
    if (!_ready) return;
    for (var attempt = 0; attempt < 40; attempt++) {
      if (await _saveNow()) return;
      // A loop is still being recorded or merged: give it a moment.
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
  }

  Future<void> newSession({bool saveCurrent = true}) async {
    await _switch(saveCurrent, () async {
      await _c.resetForSession(bpm: _c.bpm, beatsPerBar: _c.beatsPerBar);
      id = _store!.newId();
      name = _defaultName(DateTime.now());
      _createdAt = DateTime.now();
      _markClean();
    });
  }

  Future<void> open(String sessionId, {bool saveCurrent = true}) async {
    await _switch(saveCurrent, () async {
      final store = _store!;
      final data = await store.read(sessionId);
      await _c.resetForSession(bpm: data.bpm, beatsPerBar: data.beatsPerBar, tracks: data.tracks);
      final halfBar = _c.engine.status().barFrames ~/ 2;
      final failed = <int>[];
      for (var i = 0; i < data.tracks.length && i < maxTracks; i++) {
        final t = data.tracks[i];
        if (!t.hasAudio) continue;
        try {
          final wav = await store.readTrack(sessionId, t.file!);
          int import() {
            if (t.locked) return _c.engine.trackImport(i, wav.samples, t.halfBars * halfBar);
            // A whole recording: scale it to the current bar length (sample
            // rate may differ) and restore its window.
            final bar = 2 * halfBar;
            final savedBar = barFramesFor(wav.sampleRate, data.bpm, data.beatsPerBar);
            return _c.engine.trackImport(
              i,
              wav.samples,
              (wav.samples.length * bar / savedBar).round(),
              locked: false,
              origin: (t.originBars * bar).round(),
              trimEighths: t.trimEighths,
              length: t.halfBars * halfBar,
            );
          }

          var result = import();
          if (result == -1) {
            await _c.waitIdle();
            result = import();
          }
          if (result != 0) {
            failed.add(i + 1);
          } else if (t.muted) {
            _c.engine.trackSetMuted(i, true);
          }
        } on Object {
          failed.add(i + 1);
        }
      }
      await _c.waitIdle();
      id = sessionId;
      name = data.name;
      _createdAt = data.createdAt;
      _markClean();
      if (failed.isNotEmpty) lastError = 'Could not load track ${failed.join(', ')}';
    });
  }

  /// Reloads the current session after the audio device was reopened.
  Future<void> reloadAfterDeviceChange() async {
    if (_store == null) return;
    if (onDisk) {
      await open(id, saveCurrent: false);
    } else {
      await newSession(saveCurrent: false);
    }
  }

  void rename(String newName) {
    final trimmed = newName.trim();
    if (trimmed.isEmpty || trimmed == name) return;
    name = trimmed;
    notifyListeners();
  }

  Future<void> duplicate() async {
    await flush();
    if (!onDisk) return;
    final copyId = await _store!.duplicate(id, '$name (copy)');
    await open(copyId, saveCurrent: false);
  }

  Future<void> delete(String sessionId) async {
    final store = _store;
    if (store == null) return;
    if (sessionId == id) {
      _ready = false; // don't write it back while switching away
      await store.delete(sessionId);
      await newSession(saveCurrent: false);
    } else {
      await store.delete(sessionId);
      await refreshList();
    }
  }

  // ---------------------------------------------------------------------------

  Future<void> _switch(bool saveCurrent, Future<void> Function() body) async {
    if (_store == null || _busy) return;
    if (saveCurrent) await flush();
    _debounce?.cancel();
    _debounce = null;
    _busy = true;
    lastError = null;
    notifyListeners();
    try {
      if (!_c.engine.status().deviceRunning) {
        lastError = 'Audio device is not running';
        _ready = false;
        return;
      }
      await body();
      _ready = true;
      await _prefs?.setString(_lastSessionKey, id);
    } on Object catch (e) {
      lastError = 'Could not open session: $e';
    } finally {
      _busy = false;
      await refreshList();
      notifyListeners();
    }
  }

  void _markClean() {
    final status = _c.engine.status();
    for (var i = 0; i < maxTracks; i++) {
      final t = status.tracks[i];
      _savedAudio[i] = _audioSignature(t);
      _savedLength[i] = t.state == TrackState.playing ? t.length : 0;
      _savedTrim[i] = t.state == TrackState.playing && !t.trimLocked
          ? (originBars: t.origin / status.barFrames, eighths: t.trimEighths)
          : null;
    }
    _existsOnDisk = onDisk;
    _savedMeta = _existsOnDisk ? _metaKey(_manifest(status)) : null;
  }

  Object? _audioSignature(TrackStatus t) =>
      t.state == TrackState.empty ? null : (t.state, t.length, t.layerVersion, t.layerState);

  bool _isDirty(EngineStatus status) {
    var hasLoop = false;
    for (var i = 0; i < maxTracks; i++) {
      final t = status.tracks[i];
      if (t.state == TrackState.playing) hasLoop = true;
      // Wait until a recording or layer is finished before saving it.
      if (t.state == TrackState.armed || t.state == TrackState.recording || t.overdubbing) continue;
      if (_audioSignature(t) != _savedAudio[i]) return true;
    }
    if (!hasLoop && !_existsOnDisk) return false; // nothing worth a file yet
    return _metaKey(_manifest(status)) != _savedMeta;
  }

  Future<void> _autosave() async {
    _debounce = null;
    await _saveNow();
  }

  /// Saves changed tracks and the manifest. Returns false if some track
  /// could not be saved yet (busy) and another attempt is needed.
  Future<bool> _saveNow() async {
    final store = _store;
    if (store == null || _saving || !_ready) return true;
    _saving = true;
    final sessionId = id;
    var complete = true;
    var wrote = false;
    try {
      for (var i = 0; i < maxTracks; i++) {
        // Status and export are read together, synchronously, so they match.
        final status = _c.engine.status();
        final t = status.tracks[i];
        final signature = _audioSignature(t);
        if (signature == _savedAudio[i]) continue;
        if (t.state == TrackState.empty) {
          await store.deleteTrack(sessionId, i);
          _savedAudio[i] = null;
          _savedLength[i] = 0;
          _savedTrim[i] = null;
          wrote = true;
          continue;
        }
        if (t.state != TrackState.playing) {
          complete = false;
          continue;
        }
        // Untrimmed-locked loops keep the whole recording, so the trim can
        // still be changed after reopening the session.
        final audio = t.trimLocked ? _c.engine.trackExport(i) : _c.engine.trackExportRecording(i);
        if (audio == null || audio.isEmpty) {
          complete = false;
          continue;
        }
        await store.writeTrack(sessionId, i, audio, status.sampleRate);
        _savedAudio[i] = signature;
        _savedLength[i] = t.length;
        _savedTrim[i] = t.trimLocked
            ? null
            : (originBars: t.origin / status.barFrames, eighths: t.trimEighths);
        wrote = true;
      }

      final status = _c.engine.status();
      final hasAudio = _savedAudio.any((s) => s != null);
      if (hasAudio || store.exists(sessionId)) {
        final manifest = _manifest(status);
        final key = _metaKey(manifest);
        if (wrote || key != _savedMeta) {
          await store.write(sessionId, manifest);
          _savedMeta = key;
          if (sessionId == id) _existsOnDisk = true;
          lastSavedAt = DateTime.now();
          await _prefs?.setString(_lastSessionKey, sessionId);
          await refreshList();
        }
      }
    } on Object catch (e) {
      lastError = 'Could not save session: $e';
      notifyListeners();
    } finally {
      _saving = false;
    }
    return complete;
  }

  /// The manifest for what is saved on disk plus the current settings.
  SessionData _manifest(EngineStatus status) {
    final halfBar = status.barFrames ~/ 2;
    return SessionData(
      name: name,
      createdAt: _createdAt,
      updatedAt: DateTime.now(),
      bpm: _c.bpm,
      beatsPerBar: _c.beatsPerBar,
      sampleRate: status.sampleRate,
      tracks: [
        for (var i = 0; i < maxTracks; i++)
          SessionTrack(
            file: _savedAudio[i] != null ? _store!.trackFileName(i) : null,
            halfBars: _savedAudio[i] != null && halfBar > 0 ? (_savedLength[i] / halfBar).round() : 0,
            locked: _savedTrim[i] == null,
            originBars: _savedTrim[i]?.originBars ?? 0,
            trimEighths: _savedTrim[i]?.eighths ?? 0,
            volume: _c.tracks[i].volume,
            pan: _c.tracks[i].pan,
            muted: status.tracks[i].muted,
            input: _c.tracks[i].input,
            lengthMode: _c.tracks[i].lengthMode,
          ),
      ],
    );
  }

  String _metaKey(SessionData data) => jsonEncode(data.toJson()..remove('updatedAt'));

  static String _defaultName(DateTime t) {
    String two(int v) => v.toString().padLeft(2, '0');
    return 'Session ${two(t.day)}.${two(t.month)} ${two(t.hour)}:${two(t.minute)}';
  }
}

/// Bar length in frames, rounded to even exactly as the engine does.
int barFramesFor(int sampleRate, double bpm, int beatsPerBar) =>
    (sampleRate * 60 / bpm * beatsPerBar / 2).round() * 2;
