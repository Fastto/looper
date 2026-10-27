import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'session.dart';
import 'wav.dart';

/// Sessions on disk: `<root>/<id>/session.json` plus `track-N.wav` files.
/// Every write goes to a temporary file first and is renamed into place, so
/// a crash never leaves a half-written session behind.
class SessionStore {
  SessionStore(this.root);

  final Directory root;

  static const _manifest = 'session.json';

  Directory _dir(String id) => Directory('${root.path}/$id');

  String trackFileName(int index) => 'track-${index + 1}.wav';

  /// A new, unused session id (sortable timestamp).
  String newId() {
    final now = DateTime.now();
    String two(int v) => v.toString().padLeft(2, '0');
    final base =
        '${now.year}-${two(now.month)}-${two(now.day)}_${two(now.hour)}-${two(now.minute)}-${two(now.second)}';
    var id = base;
    for (var n = 2; _dir(id).existsSync(); n++) {
      id = '$base-$n';
    }
    return id;
  }

  bool exists(String id) => File('${_dir(id).path}/$_manifest').existsSync();

  /// All readable sessions, most recently updated first.
  Future<List<SessionInfo>> list() async {
    if (!await root.exists()) return [];
    final sessions = <SessionInfo>[];
    await for (final entry in root.list()) {
      if (entry is! Directory) continue;
      final id = entry.uri.pathSegments.where((s) => s.isNotEmpty).last;
      try {
        sessions.add(SessionInfo(id: id, data: await read(id)));
      } on Object {
        // Skip folders that are not (valid) sessions.
      }
    }
    sessions.sort((a, b) => b.data.updatedAt.compareTo(a.data.updatedAt));
    return sessions;
  }

  Future<SessionData> read(String id) async {
    final text = await File('${_dir(id).path}/$_manifest').readAsString();
    return SessionData.fromJson(jsonDecode(text) as Map<String, dynamic>);
  }

  Future<void> write(String id, SessionData data) async {
    await _dir(id).create(recursive: true);
    const encoder = JsonEncoder.withIndent('  ');
    await _atomicWrite('${_dir(id).path}/$_manifest', utf8.encode(encoder.convert(data.toJson())));
  }

  Future<void> writeTrack(String id, int index, Float32List samples, int sampleRate) async {
    await _dir(id).create(recursive: true);
    await _atomicWrite('${_dir(id).path}/${trackFileName(index)}', encodeWavFloat32(samples, sampleRate));
  }

  Future<WavData> readTrack(String id, String file) async {
    return decodeWav(await File('${_dir(id).path}/$file').readAsBytes());
  }

  Future<void> deleteTrack(String id, int index) async {
    final f = File('${_dir(id).path}/${trackFileName(index)}');
    if (await f.exists()) await f.delete();
  }

  Future<void> delete(String id) async {
    final dir = _dir(id);
    if (await dir.exists()) await dir.delete(recursive: true);
  }

  /// Copies a session to a new id under a new name. Returns the new id.
  Future<String> duplicate(String id, String name) async {
    final copyId = newId();
    final target = _dir(copyId);
    await target.create(recursive: true);
    await for (final entry in _dir(id).list()) {
      if (entry is File && !entry.path.endsWith('.tmp')) {
        await entry.copy('${target.path}/${entry.uri.pathSegments.last}');
      }
    }
    final data = await read(copyId);
    final now = DateTime.now();
    await write(
      copyId,
      SessionData(
        name: name,
        createdAt: now,
        updatedAt: now,
        bpm: data.bpm,
        beatsPerBar: data.beatsPerBar,
        sampleRate: data.sampleRate,
        tracks: data.tracks,
      ),
    );
    return copyId;
  }

  Future<void> _atomicWrite(String path, List<int> bytes) async {
    final tmp = File('$path.tmp');
    await tmp.writeAsBytes(bytes, flush: true);
    await tmp.rename(path);
  }
}
