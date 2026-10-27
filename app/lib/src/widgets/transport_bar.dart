import 'package:flutter/material.dart';
import 'package:looper_engine/looper_engine.dart';

import '../looper_controller.dart';
import '../theme.dart';
import 'sessions_sheet.dart';

class TransportBar extends StatelessWidget {
  const TransportBar({
    super.key,
    required this.controller,
    required this.onOpenSettings,
    required this.onOpenSessions,
  });

  final LooperController controller;
  final VoidCallback onOpenSettings;
  final VoidCallback onOpenSessions;

  @override
  Widget build(BuildContext context) {
    final s = controller.status;
    final pos = BeatPosition(s);
    final locked = controller.tempoLocked;
    final downbeatFlash = s.playing && pos.beatInBar == 0 ? (1 - pos.beatPhase * 4).clamp(0.0, 1.0) : 0.0;

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      decoration: BoxDecoration(
        color: LooperColors.surface,
        border: Border(
          bottom: BorderSide(
            color: Color.lerp(LooperColors.outline, LooperColors.downbeat, downbeatFlash)!,
            width: 2,
          ),
        ),
      ),
      child: Wrap(
        spacing: 20,
        runSpacing: 10,
        crossAxisAlignment: WrapCrossAlignment.center,
        children: [
          _PlayButton(playing: s.playing, onPressed: controller.togglePlay),
          _TempoControl(controller: controller, locked: locked),
          _MeterControl(controller: controller, locked: locked),
          BeatLamps(status: s),
          _PositionReadout(status: s, position: pos),
          _MetronomeControl(controller: controller),
          SessionButton(sessions: controller.sessions, onPressed: onOpenSessions),
          IconButton(tooltip: 'Audio settings', onPressed: onOpenSettings, icon: const Icon(Icons.tune)),
        ],
      ),
    );
  }
}

class _PlayButton extends StatelessWidget {
  const _PlayButton({required this.playing, required this.onPressed});

  final bool playing;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: playing ? 'Stop (Space)' : 'Play (Space)',
      child: FilledButton(
        style: FilledButton.styleFrom(
          backgroundColor: playing ? LooperColors.play : LooperColors.surfaceHigh,
          foregroundColor: playing ? Colors.black : LooperColors.textPrimary,
          minimumSize: const Size(56, 48),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
        ),
        onPressed: onPressed,
        child: Icon(playing ? Icons.stop_rounded : Icons.play_arrow_rounded, size: 30),
      ),
    );
  }
}

class _TempoControl extends StatelessWidget {
  const _TempoControl({required this.controller, required this.locked});

  final LooperController controller;
  final bool locked;

  @override
  Widget build(BuildContext context) {
    final bpm = controller.status.deviceRunning ? controller.status.bpm : controller.bpm;
    final lockHint = locked ? 'Clear all tracks to change tempo' : null;
    return Tooltip(
      message: lockHint ?? 'Tempo — drag, scroll or tap (T)',
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          IconButton(
            visualDensity: VisualDensity.compact,
            onPressed: locked ? null : () => controller.setBpm(bpm - 1),
            icon: const Icon(Icons.remove),
          ),
          GestureDetector(
            onVerticalDragUpdate: locked ? null : (d) => controller.setBpm(controller.bpm - d.delta.dy / 3),
            child: SizedBox(
              width: 92,
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(
                    bpm.toStringAsFixed(0),
                    style: TextStyle(
                      fontSize: 28,
                      fontWeight: FontWeight.w700,
                      fontFeatures: const [FontFeature.tabularFigures()],
                      color: locked ? LooperColors.textSecondary : LooperColors.textPrimary,
                    ),
                  ),
                  Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      if (locked) const Icon(Icons.lock, size: 11, color: LooperColors.textSecondary),
                      const Text(' BPM', style: TextStyle(fontSize: 11, color: LooperColors.textSecondary)),
                    ],
                  ),
                ],
              ),
            ),
          ),
          IconButton(
            visualDensity: VisualDensity.compact,
            onPressed: locked ? null : () => controller.setBpm(bpm + 1),
            icon: const Icon(Icons.add),
          ),
          const SizedBox(width: 4),
          OutlinedButton(onPressed: locked ? null : controller.tapTempo, child: const Text('TAP')),
        ],
      ),
    );
  }
}

class _MeterControl extends StatelessWidget {
  const _MeterControl({required this.controller, required this.locked});

  final LooperController controller;
  final bool locked;

  @override
  Widget build(BuildContext context) {
    return DropdownButton<int>(
      value: controller.beatsPerBar,
      underline: const SizedBox.shrink(),
      onChanged: locked ? null : (v) => controller.setBeatsPerBar(v!),
      items: [
        for (final n in [2, 3, 4, 5, 6, 7])
          DropdownMenuItem(
            value: n,
            child: Text('$n/4', style: const TextStyle(fontSize: 18)),
          ),
      ],
    );
  }
}

/// The visual metronome: one lamp per beat, downbeat in amber. It flashes on
/// each beat and fades over the beat, so tempo is readable without sound.
class BeatLamps extends StatelessWidget {
  const BeatLamps({super.key, required this.status});

  final EngineStatus status;

  @override
  Widget build(BuildContext context) {
    final s = status;
    final pos = BeatPosition(s);
    final countIn = s.playing && s.position < 0;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        for (var i = 0; i < s.beatsPerBar; i++)
          _Lamp(
            active: s.playing && pos.beatInBar == i,
            intensity: s.playing && pos.beatInBar == i ? 1 - pos.beatPhase * 0.7 : 0,
            color: countIn ? LooperColors.record : (i == 0 ? LooperColors.downbeat : LooperColors.beat),
            large: i == 0,
          ),
      ],
    );
  }
}

class _Lamp extends StatelessWidget {
  const _Lamp({required this.active, required this.intensity, required this.color, required this.large});

  final bool active;
  final double intensity;
  final Color color;
  final bool large;

  @override
  Widget build(BuildContext context) {
    final size = large ? 22.0 : 16.0;
    return Container(
      width: size,
      height: size,
      margin: const EdgeInsets.symmetric(horizontal: 5),
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: active ? color.withValues(alpha: intensity) : LooperColors.surfaceHigh,
        border: Border.all(color: color.withValues(alpha: 0.5), width: 1.5),
        boxShadow: active
            ? [BoxShadow(color: color.withValues(alpha: intensity * 0.8), blurRadius: 14)]
            : null,
      ),
    );
  }
}

class _PositionReadout extends StatelessWidget {
  const _PositionReadout({required this.status, required this.position});

  final EngineStatus status;
  final BeatPosition position;

  @override
  Widget build(BuildContext context) {
    final s = status;
    String text;
    Color color = LooperColors.textPrimary;
    if (!s.playing) {
      text = 'Stopped';
      color = LooperColors.textSecondary;
    } else if (s.position < 0) {
      text = 'Count-in ${s.beatsPerBar - position.beatInBar}';
      color = LooperColors.record;
    } else {
      text = 'Bar ${position.bar + 1}.${position.beatInBar + 1}';
    }
    return SizedBox(
      width: 120,
      child: Text(
        text,
        style: TextStyle(
          fontSize: 18,
          fontWeight: FontWeight.w600,
          color: color,
          fontFeatures: const [FontFeature.tabularFigures()],
        ),
      ),
    );
  }
}

class _MetronomeControl extends StatelessWidget {
  const _MetronomeControl({required this.controller});

  final LooperController controller;

  @override
  Widget build(BuildContext context) {
    final on = controller.metronomeOn;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Tooltip(
          message: on ? 'Mute click (M) — beat lamps keep flashing' : 'Unmute click (M)',
          child: IconButton.filledTonal(
            isSelected: on,
            onPressed: controller.toggleMetronome,
            icon: Icon(on ? Icons.volume_up_rounded : Icons.volume_off_rounded),
          ),
        ),
        SizedBox(
          width: 110,
          child: Slider(
            value: controller.metronomeVolume,
            onChanged: on ? (v) => controller.setMetronome(volume: v) : null,
          ),
        ),
      ],
    );
  }
}
