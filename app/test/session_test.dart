import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:looper/src/session/session.dart';
import 'package:looper/src/session/session_store.dart';
import 'package:looper/src/session/wav.dart';

void main() {
  group('wav', () {
    test('float32 round trip is lossless', () {
      final samples = Float32List.fromList([0, 0.5, -0.25, 1, -1, 0.123456789]);
      final decoded = decodeWav(encodeWavFloat32(samples, 44100));
      expect(decoded.sampleRate, 44100);
      expect(decoded.samples, samples);
    });

    test('decodes 16-bit PCM stereo as mono', () {
      final bytes = BytesBuilder();
      void u32(int v) => bytes.add((ByteData(4)..setUint32(0, v, Endian.little)).buffer.asUint8List());
      void u16(int v) => bytes.add((ByteData(2)..setUint16(0, v, Endian.little)).buffer.asUint8List());
      void i16(int v) => bytes.add((ByteData(2)..setInt16(0, v, Endian.little)).buffer.asUint8List());
      bytes.add('RIFF'.codeUnits);
      u32(36 + 8);
      bytes.add('WAVE'.codeUnits);
      bytes.add('fmt '.codeUnits);
      u32(16);
      u16(1);
      u16(2);
      u32(48000);
      u32(48000 * 4);
      u16(4);
      u16(16);
      bytes.add('data'.codeUnits);
      u32(8);
      i16(16384); // L =  0.5
      i16(0); //     R =  0
      i16(-32768); // L = -1
      i16(-32768); // R = -1
      final decoded = decodeWav(bytes.toBytes());
      expect(decoded.sampleRate, 48000);
      expect(decoded.samples, [0.25, -1.0]);
    });

    test('rejects non-wav data', () {
      expect(() => decodeWav(Uint8List.fromList(List.filled(64, 0))), throwsA(isA<WavFormatException>()));
    });
  });

  group('session store', () {
    late Directory dir;
    late SessionStore store;

    setUp(() async {
      dir = await Directory.systemTemp.createTemp('looper_test');
      store = SessionStore(dir);
    });

    tearDown(() => dir.delete(recursive: true));

    SessionData session(String name, DateTime at) => SessionData(
      name: name,
      createdAt: at,
      updatedAt: at,
      bpm: 96,
      beatsPerBar: 3,
      sampleRate: 48000,
      tracks: [
        SessionTrack(
          file: store.trackFileName(0),
          halfBars: 4,
          volume: 0.6,
          pan: -0.5,
          muted: true,
          input: 1,
        ),
        const SessionTrack(lengthMode: 2),
      ],
    );

    test('writes, lists, reads back, duplicates and deletes', () async {
      final a = store.newId();
      await store.write(a, session('Morning jam', DateTime(2026, 1, 1)));
      await store.writeTrack(a, 0, Float32List.fromList([0.1, 0.2, 0.3]), 48000);
      final b = store.newId();
      expect(b, isNot(a));
      await store.write(b, session('Evening', DateTime(2026, 1, 2)));

      final list = await store.list();
      expect(list.map((s) => s.data.name), ['Evening', 'Morning jam']);

      final read = await store.read(a);
      expect(read.bpm, 96);
      expect(read.beatsPerBar, 3);
      expect(read.loopCount, 1);
      expect(read.tracks[0].muted, isTrue);
      expect(read.tracks[0].pan, -0.5);
      expect(read.tracks[1].pan, 0);
      expect(read.tracks[0].input, 1);
      expect(read.tracks[1].lengthMode, 2);
      expect(read.tracks[1].hasAudio, isFalse);

      final wav = await store.readTrack(a, read.tracks[0].file!);
      expect(wav.samples, Float32List.fromList([0.1, 0.2, 0.3]));

      final copy = await store.duplicate(a, 'Morning jam (copy)');
      expect((await store.read(copy)).name, 'Morning jam (copy)');
      expect((await store.readTrack(copy, store.trackFileName(0))).samples.length, 3);

      await store.delete(a);
      expect(store.exists(a), isFalse);
      expect((await store.list()).length, 2);
      expect(dir.listSync(recursive: true).where((e) => e.path.endsWith('.tmp')), isEmpty);
    });

    test('ignores folders that are not sessions', () async {
      await Directory('${dir.path}/junk').create();
      expect(await store.list(), isEmpty);
    });

    test('reads trim windows (v2) and treats v1 loops as locked', () {
      const trimmed = SessionTrack(
        file: 'track-1.wav',
        halfBars: 8,
        locked: false,
        originBars: 0.5,
        trimEighths: 3,
      );
      final json = trimmed.toJson();
      expect(json['trim'], {'originBars': 0.5, 'eighths': 3});
      final back = SessionTrack.fromJson(json);
      expect(back.locked, isFalse);
      expect(back.originBars, 0.5);
      expect(back.trimEighths, 3);
      final v1 = SessionTrack.fromJson({'file': 'track-1.wav', 'halfBars': 4});
      expect(v1.locked, isTrue);
      expect(const SessionTrack(file: 'x.wav', halfBars: 2).toJson().containsKey('trim'), isFalse);
    });

    test('refuses sessions from a newer app version', () {
      expect(
        () => SessionData.fromJson({'format': SessionData.format, 'version': 99}),
        throwsFormatException,
      );
    });
  });
}
