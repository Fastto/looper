import 'dart:io';
import 'dart:isolate';
import 'dart:math' as math;
import 'dart:typed_data';

import '../session/wav.dart';

/// One loop to export: its audio (one cycle) and how loud it plays.
class ExportLoop {
  const ExportLoop({required this.track, required this.audio, required this.gain, this.pan = 0});

  /// 0-based track index.
  final int track;
  final Float32List audio;
  final double gain;

  /// -1 (left) .. 1 (right); used for the mix (stems stay mono).
  final double pan;
}

/// Everything needed to write an export; plain data so it can cross isolates.
class ExportJob {
  const ExportJob({
    required this.folder,
    required this.title,
    required this.sampleRate,
    required this.frames,
    required this.loops,
    required this.stems,
    required this.mix,
  });

  /// Folder the user picked; a sub-folder named after [title] is created in it.
  final String folder;
  final String title;
  final int sampleRate;

  /// Length of every exported file, in frames.
  final int frames;
  final List<ExportLoop> loops;
  final bool stems;
  final bool mix;
}

class ExportResult {
  const ExportResult({required this.folder, required this.files});

  final String folder;
  final List<String> files;
}

/// Longest export we offer, in bars.
const maxExportBars = 64;

int _gcd(int a, int b) => b == 0 ? a : _gcd(b, a % b);

/// Default export length in half-bars: the shortest length after which all
/// loops line up again (their least common multiple), so the files loop
/// seamlessly in a DAW. Falls back to the longest loop when that would
/// exceed [maxExportBars].
int defaultExportHalfBars(Iterable<int> loopHalfBars) {
  final lengths = loopHalfBars.where((h) => h > 0).toList();
  if (lengths.isEmpty) return 0;
  var lcm = 1;
  for (final h in lengths) {
    lcm = lcm ~/ _gcd(lcm, h) * h;
    if (lcm > maxExportBars * 2) {
      return lengths.reduce((a, b) => a > b ? a : b);
    }
  }
  return lcm;
}

/// The loop repeated (from bar 1) to [frames], scaled by [gain].
Float32List renderStem(Float32List loop, double gain, int frames) {
  final out = Float32List(frames);
  if (loop.isEmpty) return out;
  for (var i = 0; i < frames; i++) {
    out[i] = loop[i % loop.length] * gain;
  }
  return out;
}

/// The engine's pan law: unity in the centre, the far side fades out.
(double, double) panGains(double pan) =>
    (pan <= 0 ? 1.0 : math.cos(pan * math.pi / 2), pan >= 0 ? 1.0 : math.cos(-pan * math.pi / 2));

/// Stereo mix (interleaved L/R) of mono stems at their pans.
Float32List mixDown(List<Float32List> stems, List<double> pans, int frames) {
  final out = Float32List(frames * 2);
  for (var s = 0; s < stems.length; s++) {
    final stem = stems[s];
    final (left, right) = panGains(pans[s]);
    for (var i = 0; i < frames && i < stem.length; i++) {
      out[2 * i] += stem[i] * left;
      out[2 * i + 1] += stem[i] * right;
    }
  }
  return out;
}

/// Makes a string safe to use as a file or folder name on every platform.
String safeFileName(String name) {
  final cleaned = name.replaceAll(RegExp(r'[\\/:*?"<>|\x00-\x1F]'), '-').trim();
  return cleaned.isEmpty ? 'Untitled' : cleaned;
}

/// Writes the export in a background isolate (rendering a long mix takes a
/// moment and must not stall the UI).
Future<ExportResult> runExport(ExportJob job) => Isolate.run(() => writeExport(job));

ExportResult writeExport(ExportJob job) {
  var dir = Directory('${job.folder}/${safeFileName(job.title)}');
  for (var n = 2; dir.existsSync(); n++) {
    dir = Directory('${job.folder}/${safeFileName(job.title)} ($n)');
  }
  dir.createSync(recursive: true);

  final files = <String>[];
  void write(String name, Float32List samples, {int channels = 1}) {
    final path = '${dir.path}/$name';
    File(path).writeAsBytesSync(encodeWavFloat32(samples, job.sampleRate, channels: channels), flush: true);
    files.add(path);
  }

  final rendered = <Float32List>[];
  for (final loop in job.loops) {
    final stem = renderStem(loop.audio, loop.gain, job.frames);
    rendered.add(stem);
    if (job.stems) {
      final number = (loop.track + 1).toString().padLeft(2, '0');
      write('$number Track ${loop.track + 1}.wav', stem);
    }
  }
  if (job.mix) {
    write('Mix.wav', mixDown(rendered, [for (final l in job.loops) l.pan], job.frames), channels: 2);
  }
  return ExportResult(folder: dir.path, files: files);
}
