/// The on-disk description of a session (`session.json`). Audio lives next
/// to it, one mono WAV per track. See docs/DESIGN.md §7.
class SessionTrack {
  const SessionTrack({
    this.file,
    this.halfBars = 0,
    this.volume = 0.8,
    this.pan = 0,
    this.muted = false,
    this.input = 0,
    this.lengthMode = 0,
    this.locked = true,
    this.originBars = 0,
    this.trimEighths = 0,
  });

  factory SessionTrack.fromJson(Map<String, dynamic> json) => SessionTrack(
    file: json['file'] as String?,
    halfBars: (json['halfBars'] as num?)?.toInt() ?? 0,
    volume: (json['volume'] as num?)?.toDouble() ?? 0.8,
    pan: (json['pan'] as num?)?.toDouble() ?? 0,
    muted: json['muted'] as bool? ?? false,
    input: (json['input'] as num?)?.toInt() ?? 0,
    lengthMode: (json['lengthMode'] as num?)?.toInt() ?? 0,
    // Version 1 files hold the finished loop only, i.e. a locked trim.
    locked: json['trim'] == null,
    originBars: ((json['trim'] as Map<String, dynamic>?)?['originBars'] as num?)?.toDouble() ?? 0,
    trimEighths: ((json['trim'] as Map<String, dynamic>?)?['eighths'] as num?)?.toInt() ?? 0,
  );

  /// WAV file name inside the session folder; null for an empty track.
  final String? file;

  /// Loop length in half-bars, so it survives sample-rate changes exactly.
  final int halfBars;
  final double volume;

  /// -1 (left) .. 1 (right).
  final double pan;
  final bool muted;
  final int input;
  final int lengthMode;

  /// True: the WAV is the loop itself. False: it is the whole recording
  /// (with pre-roll), and the trim window is restored from [originBars]
  /// (where the original downbeat sits, in bars from the file start) and
  /// [trimEighths] (start shift in eighth notes).
  final bool locked;
  final double originBars;
  final int trimEighths;

  bool get hasAudio => file != null && halfBars > 0;

  Map<String, dynamic> toJson() => {
    'file': file,
    'halfBars': halfBars,
    'volume': volume,
    'pan': pan,
    'muted': muted,
    'input': input,
    'lengthMode': lengthMode,
    if (!locked) 'trim': {'originBars': originBars, 'eighths': trimEighths},
  };
}

class SessionData {
  const SessionData({
    required this.name,
    required this.createdAt,
    required this.updatedAt,
    required this.bpm,
    required this.beatsPerBar,
    required this.sampleRate,
    required this.tracks,
  });

  factory SessionData.fromJson(Map<String, dynamic> json) {
    if (json['format'] != format) {
      throw const FormatException('not a looper session');
    }
    final version = (json['version'] as num?)?.toInt() ?? 0;
    if (version > currentVersion) {
      throw FormatException('session version $version is newer than this app');
    }
    return SessionData(
      name: json['name'] as String? ?? 'Untitled',
      createdAt: DateTime.tryParse(json['createdAt'] as String? ?? '') ?? DateTime.now(),
      updatedAt: DateTime.tryParse(json['updatedAt'] as String? ?? '') ?? DateTime.now(),
      bpm: (json['bpm'] as num?)?.toDouble() ?? 120,
      beatsPerBar: (json['beatsPerBar'] as num?)?.toInt() ?? 4,
      sampleRate: (json['sampleRate'] as num?)?.toInt() ?? 48000,
      tracks: [
        for (final t in (json['tracks'] as List? ?? const []))
          SessionTrack.fromJson(t as Map<String, dynamic>),
      ],
    );
  }

  static const format = 'looper-session';
  static const currentVersion = 2;

  final String name;
  final DateTime createdAt;
  final DateTime updatedAt;
  final double bpm;
  final int beatsPerBar;
  final int sampleRate;
  final List<SessionTrack> tracks;

  int get loopCount => tracks.where((t) => t.hasAudio).length;

  SessionData copyWith({String? name, DateTime? updatedAt}) => SessionData(
    name: name ?? this.name,
    createdAt: createdAt,
    updatedAt: updatedAt ?? this.updatedAt,
    bpm: bpm,
    beatsPerBar: beatsPerBar,
    sampleRate: sampleRate,
    tracks: tracks,
  );

  Map<String, dynamic> toJson() => {
    'format': format,
    'version': currentVersion,
    'name': name,
    'createdAt': createdAt.toIso8601String(),
    'updatedAt': updatedAt.toIso8601String(),
    'bpm': bpm,
    'beatsPerBar': beatsPerBar,
    'sampleRate': sampleRate,
    'tracks': [for (final t in tracks) t.toJson()],
  };
}

/// A session as listed in the sessions browser.
class SessionInfo {
  const SessionInfo({required this.id, required this.data});

  final String id;
  final SessionData data;
}
