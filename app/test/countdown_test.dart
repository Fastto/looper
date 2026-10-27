import 'package:flutter_test/flutter_test.dart';
import 'package:looper/src/looper_controller.dart';
import 'package:looper_engine/looper_engine.dart';

TrackStatus _armed(int recStart) => TrackStatus(
  state: TrackState.armed,
  muted: false,
  inputChannel: 0,
  lengthMode: 0,
  layerState: LayerState.none,
  layerVersion: 0,
  busy: false,
  pendingMute: null,
  pan: 0,
  trimLocked: false,
  trimEighths: 0,
  trimMinEighths: 0,
  trimMaxEighths: 0,
  trimMaxBars: 0,
  origin: 0,
  windowStart: 0,
  audioEnd: 0,
  recStart: recStart,
  recEnd: -1,
  length: 0,
  volume: 1,
  peak: 0,
);

EngineStatus _status({required bool playing, required int position, List<TrackStatus>? tracks}) {
  final idle = EngineStatus.idle;
  return EngineStatus(
    deviceRunning: true,
    playing: playing,
    sampleRate: 48000,
    inputChannels: 2,
    beatsPerBar: 4,
    metronomeOn: true,
    metronomeVolume: 0.5,
    monitoringOn: false,
    countInBars: 1,
    latencyFrames: 0,
    muteQuantize: 0,
    calibrating: false,
    feedback: 1,
    bpm: 120,
    position: position,
    barFrames: 96000, // one beat = 24000
    blockCounter: 0,
    inputPeaks: const [],
    tracks: tracks ?? idle.tracks,
  );
}

void main() {
  test('no countdown when nothing is armed or the transport is stopped', () {
    expect(beatsUntilRecording(_status(playing: true, position: 1000)), isNull);
    expect(beatsUntilRecording(_status(playing: false, position: 0, tracks: [_armed(96000)])), isNull);
  });

  test('counts the beats left to the bar where recording starts', () {
    // Count-in from a stopped transport: starts at -1 bar, records at 0.
    expect(beatsUntilRecording(_status(playing: true, position: -96000, tracks: [_armed(0)])), 4);
    expect(beatsUntilRecording(_status(playing: true, position: -1, tracks: [_armed(0)])), 1);
    // While playing: pressed in beat 2 of bar 1, records at bar 2.
    expect(beatsUntilRecording(_status(playing: true, position: 30000, tracks: [_armed(96000)])), 3);
    expect(beatsUntilRecording(_status(playing: true, position: 80000, tracks: [_armed(96000)])), 1);
  });

  test('the nearest armed track wins', () {
    final tracks = [_armed(192000), _armed(96000)];
    expect(beatsUntilRecording(_status(playing: true, position: 50000, tracks: tracks)), 2);
  });
}
