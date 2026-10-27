// End-to-end on real audio hardware: record a loop, autosave it, switch
// sessions, reopen it. Run: `make test-integration`.
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:looper/src/export/audio_export.dart';
import 'package:looper/src/looper_controller.dart';
import 'package:looper/src/session/wav.dart';
import 'package:looper_engine/looper_engine.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('a recorded loop survives saving and reopening its session', (tester) async {
    SharedPreferences.setMockInitialValues({});
    final root = await Directory.systemTemp.createTemp('looper_sessions');
    final c = LooperController(sessionsRoot: root);
    addTearDown(() async {
      c.dispose();
      await root.delete(recursive: true);
    });

    await tester.runAsync(() async {
      await c.start();
      c.poll();
      if (!c.status.deviceRunning) {
        markTestSkipped('no audio device: ${c.error}');
        return;
      }

      Future<void> pollUntil(bool Function() done, {int seconds = 10}) async {
        final deadline = DateTime.now().add(Duration(seconds: seconds));
        while (!done() && DateTime.now().isBefore(deadline)) {
          await Future<void>.delayed(const Duration(milliseconds: 20));
          c.poll();
        }
        expect(done(), isTrue, reason: 'timed out');
      }

      // 240 BPM: one bar = one second.
      await c.resetForSession(bpm: 240, beatsPerBar: 4);
      c.setMetronome(on: false);
      c.setTrackLengthMode(0, 2); // 1 bar
      c.setTrackVolume(0, 0.7);
      c.tapTrack(0);
      // Playing starts on the bar line; the latency-delayed tail lands just after.
      await pollUntil(() => c.status.tracks[0].state == TrackState.playing && !c.status.tracks[0].busy);
      final bar = c.status.barFrames;
      expect(c.status.tracks[0].length, bar);
      final original = c.engine.trackExport(0)!;
      expect(original.length, bar);

      await c.sessions.flush();
      final id = c.sessions.id;
      final manifest =
          jsonDecode(await File('${root.path}/$id/session.json').readAsString()) as Map<String, dynamic>;
      final track = (manifest['tracks'] as List).first as Map<String, dynamic>;
      expect(track['halfBars'], 2);
      expect(track['volume'], closeTo(0.7, 1e-9));
      final wav = decodeWav(await File('${root.path}/$id/track-1.wav').readAsBytes());
      expect(wav.samples, original);

      await c.sessions.newSession();
      c.poll();
      expect(c.sessions.id, isNot(id));
      expect(c.status.tracks[0].state, TrackState.empty);

      await c.sessions.open(id);
      await pollUntil(() => c.status.tracks[0].state == TrackState.playing);
      expect(c.sessions.lastError, isNull);
      expect(c.status.tracks[0].length, bar);
      expect(c.tracks[0].volume, closeTo(0.7, 1e-9));
      expect(c.engine.trackExport(0), original);
      expect((await c.sessions.store.list()).length, 1); // the empty session was never written

      // Export: two cycles of the loop as a stem and a mix.
      final loops = await c.collectLoops(includeMuted: false);
      expect(loops.single.track, 0);
      final result = await runExport(
        ExportJob(
          folder: root.path,
          title: 'export',
          sampleRate: c.status.sampleRate,
          frames: 2 * bar,
          loops: loops,
          stems: true,
          mix: true,
        ),
      );
      expect(result.files.map((f) => f.split('/').last), ['01 Track 1.wav', 'Mix.wav']);
      final stem = decodeWav(await File(result.files.first).readAsBytes());
      expect(stem.samples.length, 2 * bar);
      for (final i in [0, bar ~/ 3, bar - 1]) {
        expect(stem.samples[i], closeTo(original[i] * 0.7, 1e-6));
        expect(stem.samples[bar + i], closeTo(original[i] * 0.7, 1e-6));
      }
    });
  });
}
