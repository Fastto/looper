import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import '../session/wav.dart';

/// One loop to export: its audio (one cycle) and how loud it plays.
class ExportLoop {
  const ExportLoop({required this.track, required this.audio, required this.gain});

  /// 0-based track index.
  final int track;
  final Float32List audio;
  final double gain;
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

Float32List mixDown(List<Float32List> stems, int frames) {
  final out = Float32List(frames);
  for (final stem in stems) {
    for (var i = 0; i < frames && i < stem.length; i++) {
      out[i] += stem[i];
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
  void write(String name, Float32List samples) {
    final path = '${dir.path}/$name';
    File(path).writeAsBytesSync(encodeWavFloat32(samples, job.sampleRate), flush: true);
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
  if (job.mix) write('Mix.wav', mixDown(rendered, job.frames));
  return ExportResult(folder: dir.path, files: files);
}
