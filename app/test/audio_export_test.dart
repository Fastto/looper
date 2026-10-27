import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:looper/src/export/audio_export.dart';
import 'package:looper/src/session/wav.dart';

void main() {
  test('default length lines all loops up again', () {
    expect(defaultExportHalfBars([]), 0);
    expect(defaultExportHalfBars([4]), 4);
    expect(defaultExportHalfBars([2, 4, 8]), 8);
    expect(defaultExportHalfBars([6, 8]), 24); // 3 and 4 bars -> 12 bars
    expect(defaultExportHalfBars([1, 16]), 16); // half a bar fits in 8 bars
    // 7, 9 and 11 bars would need 693 bars: fall back to the longest loop.
    expect(defaultExportHalfBars([14, 18, 22]), 22);
  });

  test('stems repeat the loop from bar 1 and apply gain', () {
    final loop = Float32List.fromList([1, 0, -1]);
    expect(renderStem(loop, 0.5, 7), [0.5, 0, -0.5, 0.5, 0, -0.5, 0.5]);
  });

  test('mix is the stereo sum of the stems at their pans', () {
    final a = Float32List.fromList([0.25, 0.25, 0.25]);
    final b = Float32List.fromList([0.5, -0.25, 0]);
    // a centred, b hard left.
    expect(mixDown([a, b], [0, -1], 3), [0.75, 0.25, 0, 0.25, 0.25, 0.25]);
  });

  test('pan law matches the engine: unity in the centre', () {
    expect(panGains(0), (1.0, 1.0));
    expect(panGains(1).$1, closeTo(0, 1e-12));
    expect(panGains(-0.5).$2, closeTo(0.70710678, 1e-6));
  });

  test('file names are safe', () {
    expect(safeFileName('Jam: take 1/2?'), 'Jam- take 1-2-');
    expect(safeFileName('   '), 'Untitled');
  });

  test('writes stems and mix into a new sub-folder, never overwriting', () async {
    final dir = await Directory.systemTemp.createTemp('looper_export');
    addTearDown(() => dir.delete(recursive: true));
    ExportJob job({bool stems = true, bool mix = true}) => ExportJob(
      folder: dir.path,
      title: 'Jam - 120 BPM',
      sampleRate: 48000,
      frames: 8,
      loops: [
        ExportLoop(track: 0, audio: Float32List.fromList([1, 0]), gain: 1),
        ExportLoop(track: 2, audio: Float32List.fromList([0, 0, 0, 0.5]), gain: 0.5, pan: 1),
      ],
      stems: stems,
      mix: mix,
    );

    final first = writeExport(job());
    expect(first.folder, '${dir.path}/Jam - 120 BPM');
    expect(first.files.map((f) => f.split('/').last), ['01 Track 1.wav', '03 Track 3.wav', 'Mix.wav']);
    final stem3 = decodeWav(await File(first.files[1]).readAsBytes());
    expect(stem3.sampleRate, 48000);
    expect(stem3.samples, [0, 0, 0, 0.25, 0, 0, 0, 0.25]);
    final mixBytes = await File(first.files[2]).readAsBytes();
    expect(ByteData.sublistView(mixBytes).getUint16(22, Endian.little), 2); // stereo
    // Track 3 is hard right: L = track 1 only, R = both; decoding averages L/R.
    final mix = decodeWav(mixBytes);
    expect(mix.samples, [1, 0, 1, 0.125, 1, 0, 1, 0.125]);

    final second = writeExport(job(stems: false));
    expect(second.folder, '${dir.path}/Jam - 120 BPM (2)');
    expect(second.files.map((f) => f.split('/').last), ['Mix.wav']);
  });
}
