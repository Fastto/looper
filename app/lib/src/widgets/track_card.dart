import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:looper_engine/looper_engine.dart';

import '../looper_controller.dart';
import '../theme.dart';

/// One loop slot: a big state button plus its settings.
class TrackCard extends StatelessWidget {
  const TrackCard({super.key, required this.controller, required this.index});

  final LooperController controller;
  final int index;

  @override
  Widget build(BuildContext context) {
    final s = controller.status;
    final t = s.tracks[index];
    final settings = controller.tracks[index];
    final view = _TrackView.of(s, t);

    return Container(
      decoration: BoxDecoration(
        color: LooperColors.surface,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: view.color.withValues(alpha: view.active ? 0.7 : 0.15), width: 1.5),
      ),
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
      child: Column(
        children: [
          _Header(controller: controller, index: index, status: s, track: t),
          Expanded(
            child: Center(
              child: _TrackButton(
                view: view,
                index: index,
                onTap: () => controller.tapTrack(index),
                onLongPress: () => controller.toggleOverdub(index),
              ),
            ),
          ),
          SizedBox(
            height: 34,
            child: _TrimmableWaveform(
              controller: controller,
              index: index,
              playhead: view.loopPhase,
              color: t.overdubbing
                  ? LooperColors.overdub
                  : (t.muted ? LooperColors.muted : LooperColors.play),
            ),
          ),
          const SizedBox(height: 6),
          _LengthSelector(
            value: settings.lengthMode,
            enabled: t.state == TrackState.empty,
            onChanged: (v) => controller.setTrackLengthMode(index, v),
          ),
          Row(
            children: [
              const Tooltip(
                message: 'Volume',
                child: Icon(Icons.volume_down, size: 16, color: LooperColors.textSecondary),
              ),
              Expanded(
                flex: 3,
                child: Slider(
                  value: settings.volume,
                  max: 1.5,
                  onChanged: (v) => controller.setTrackVolume(index, v),
                ),
              ),
              _NormalizeButton(controller: controller, index: index),
              Expanded(
                flex: 2,
                child: _PanSlider(controller: controller, index: index),
              ),
            ],
          ),
          _LayerControls(controller: controller, index: index, status: s, track: t),
        ],
      ),
    );
  }
}

/// Everything the button needs, derived from the engine state.
class _TrackView {
  const _TrackView({
    required this.color,
    required this.progress,
    required this.label,
    required this.sublabel,
    required this.icon,
    required this.active,
    required this.blink,
    this.loopPhase,
  });

  factory _TrackView.of(EngineStatus s, TrackStatus t) {
    final pos = s.position;
    final bar = s.barFrames;
    String bars(int frames) {
      final b = frames / bar;
      if (b == 0.5) return '½ bar';
      final n = b.round();
      return n == 1 ? '1 bar' : '$n bars';
    }

    switch (t.state) {
      case TrackState.empty:
        return const _TrackView(
          color: LooperColors.outline,
          progress: 0,
          label: 'REC',
          sublabel: 'tap to record',
          icon: Icons.fiber_manual_record,
          active: false,
          blink: false,
        );
      case TrackState.armed:
        final wait = t.recStart - pos;
        final beats = (wait / s.beatFrames).ceil();
        final beatPhase = (pos / s.beatFrames) % 1;
        return _TrackView(
          color: LooperColors.record,
          progress: (1 - wait / bar).clamp(0.0, 1.0),
          label: '$beats',
          sublabel: 'starts on the bar',
          icon: Icons.hourglass_top,
          active: true,
          blink: beatPhase < 0.5,
        );
      case TrackState.recording:
        final elapsed = pos - t.recStart;
        final hasEnd = t.recEnd >= 0;
        final progress = hasEnd ? elapsed / (t.recEnd - t.recStart) : (elapsed % bar) / bar;
        return _TrackView(
          color: LooperColors.record,
          progress: progress.clamp(0.0, 1.0),
          label: 'Bar ${elapsed ~/ bar + 1}',
          sublabel: hasEnd ? 'stops at ${bars(t.recEnd - t.recStart)}' : 'tap to close loop',
          icon: Icons.fiber_manual_record,
          active: true,
          blink: false,
        );
      case TrackState.playing:
        final phase = t.length > 0 ? ((pos - t.recStart) % t.length) / t.length : 0.0;
        if (t.overdubbing) {
          return _TrackView(
            color: LooperColors.overdub,
            progress: phase,
            label: 'Layer',
            sublabel: 'tap to finish layer',
            icon: Icons.layers,
            active: true,
            blink: false,
            loopPhase: s.playing ? phase : null,
          );
        }
        final pending = t.pendingMute;
        final grid = s.muteQuantize == 2 ? 'bar' : 'beat';
        return _TrackView(
          color: t.muted ? LooperColors.muted : LooperColors.play,
          progress: phase,
          label: bars(t.length),
          sublabel: pending != null
              ? '${pending ? 'mutes' : 'plays'} on the $grid'
              : (t.muted ? 'muted · tap to play' : 'tap to mute · hold to layer'),
          icon: t.muted ? Icons.volume_off : Icons.graphic_eq,
          active: !t.muted,
          // A change waiting for the grid blinks with the beat.
          blink: pending != null && (s.position / s.beatFrames) % 1 < 0.5,
          loopPhase: s.playing ? phase : null,
        );
    }
  }

  final Color color;
  final double progress;
  final String label;
  final String sublabel;
  final IconData icon;
  final bool active;
  final bool blink;
  final double? loopPhase;
}

class _TrackButton extends StatelessWidget {
  const _TrackButton({
    required this.view,
    required this.onTap,
    required this.onLongPress,
    required this.index,
  });

  final _TrackView view;
  final VoidCallback onTap;
  final VoidCallback onLongPress;
  final int index;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final size = math.max(48.0, math.min(constraints.maxWidth, constraints.maxHeight) - 4);
        final fill = view.blink
            ? view.color.withValues(alpha: 0.35)
            : view.color.withValues(alpha: view.active ? 0.16 : 0.0);
        return Tooltip(
          message: 'Key ${index + 1}',
          child: Material(
            color: Colors.transparent,
            shape: const CircleBorder(),
            child: InkWell(
              customBorder: const CircleBorder(),
              onTap: onTap,
              onLongPress: onLongPress,
              child: CustomPaint(
                painter: _RingPainter(progress: view.progress, color: view.color, fill: fill),
                child: SizedBox.square(
                  dimension: size,
                  child: FittedBox(
                    fit: BoxFit.scaleDown,
                    child: Padding(
                      padding: const EdgeInsets.all(16),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Icon(
                            view.icon,
                            size: 26,
                            color: view.color == LooperColors.outline ? LooperColors.record : view.color,
                          ),
                          Text(view.label, style: const TextStyle(fontSize: 22, fontWeight: FontWeight.w700)),
                          Text(
                            view.sublabel,
                            style: const TextStyle(fontSize: 11, color: LooperColors.textSecondary),
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

class _RingPainter extends CustomPainter {
  _RingPainter({required this.progress, required this.color, required this.fill});

  final double progress;
  final Color color;
  final Color fill;

  @override
  void paint(Canvas canvas, Size size) {
    final center = size.center(Offset.zero);
    final radius = size.shortestSide / 2 - 5;
    canvas.drawCircle(center, radius, Paint()..color = fill);
    canvas.drawCircle(
      center,
      radius,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 6
        ..color = LooperColors.surfaceHigh,
    );
    if (progress > 0) {
      canvas.drawArc(
        Rect.fromCircle(center: center, radius: radius),
        -math.pi / 2,
        2 * math.pi * progress,
        false,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 6
          ..strokeCap = StrokeCap.round
          ..color = color,
      );
    }
  }

  @override
  bool shouldRepaint(_RingPainter old) => old.progress != progress || old.color != color || old.fill != fill;
}

class _Header extends StatelessWidget {
  const _Header({required this.controller, required this.index, required this.status, required this.track});

  final LooperController controller;
  final int index;
  final EngineStatus status;
  final TrackStatus track;

  @override
  Widget build(BuildContext context) {
    final inputs = math.max(status.inputChannels, 1);
    final input = controller.tracks[index].input.clamp(0, inputs - 1);
    final inputPeak = input < status.inputPeaks.length ? status.inputPeaks[input] : 0.0;
    return Row(
      children: [
        Container(
          width: 24,
          height: 24,
          alignment: Alignment.center,
          decoration: BoxDecoration(color: LooperColors.surfaceHigh, borderRadius: BorderRadius.circular(6)),
          child: Text('${index + 1}', style: const TextStyle(fontWeight: FontWeight.w700)),
        ),
        const SizedBox(width: 8),
        Tooltip(
          message: 'Input channel to record from',
          child: DropdownButton<int>(
            value: input,
            isDense: true,
            underline: const SizedBox.shrink(),
            onChanged: (v) => controller.setTrackInput(index, v!),
            items: [
              for (var c = 0; c < inputs; c++)
                DropdownMenuItem(
                  value: c,
                  child: Text('In ${c + 1}', style: const TextStyle(fontSize: 13)),
                ),
            ],
          ),
        ),
        if (controller.inputHeard(input))
          const Tooltip(
            message: 'You hear this input through the app (top bar)',
            child: Icon(Icons.headphones, size: 14, color: LooperColors.play),
          ),
        const SizedBox(width: 8),
        Expanded(
          child: _Meter(level: inputPeak, color: LooperColors.record),
        ),
        const SizedBox(width: 8),
        SizedBox(
          width: 36,
          child: _Meter(level: track.peak, color: LooperColors.play),
        ),
      ],
    );
  }
}

class _Meter extends StatelessWidget {
  const _Meter({required this.level, required this.color});

  final double level;
  final Color color;

  @override
  Widget build(BuildContext context) {
    // Map roughly -48..0 dBFS onto the bar.
    final db = level > 0 ? 20 * math.log(level) / math.ln10 : -96.0;
    final f = ((db + 48) / 48).clamp(0.0, 1.0);
    return ClipRRect(
      borderRadius: BorderRadius.circular(2),
      child: SizedBox(
        height: 4,
        child: Stack(
          children: [
            Container(color: LooperColors.surfaceHigh),
            FractionallySizedBox(
              widthFactor: f,
              child: Container(color: level >= 0.99 ? LooperColors.downbeat : color),
            ),
          ],
        ),
      ),
    );
  }
}

/// The waveform with trim controls: the left pair moves the loop start by
/// the trim step, the right pair removes or adds whole bars at the end.
class _TrimmableWaveform extends StatelessWidget {
  const _TrimmableWaveform({
    required this.controller,
    required this.index,
    required this.playhead,
    required this.color,
  });

  final LooperController controller;
  final int index;
  final double? playhead;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final c = controller;
    final t = c.status.tracks[index];
    final view = c.waveform(index);
    final playing = t.state == TrackState.playing;
    final showTrim = playing && !t.trimLocked;
    final step = c.trimStepLabel;

    Widget button(IconData icon, String tip, bool enabled, VoidCallback onPressed) => Tooltip(
      message: tip,
      child: SizedBox(
        width: 22,
        child: IconButton(
          padding: EdgeInsets.zero,
          iconSize: 18,
          visualDensity: VisualDensity.compact,
          onPressed: enabled ? onPressed : null,
          icon: Icon(icon),
        ),
      ),
    );

    final wave = CustomPaint(
      size: Size.infinite,
      painter: _WaveformPainter(
        data: view.peaks,
        windowFrom: view.windowFrom,
        windowTo: view.windowTo,
        playhead: playhead,
        color: color,
      ),
    );

    if (!showTrim) {
      return Stack(
        children: [
          Positioned.fill(child: wave),
          if (playing && t.trimLocked)
            const Positioned(
              right: 4,
              top: 2,
              child: Tooltip(
                message: 'Trim is fixed once you layer on a loop (layers follow the loop as recorded)',
                child: Icon(Icons.lock_outline, size: 12, color: LooperColors.textSecondary),
              ),
            ),
        ],
      );
    }
    return Row(
      children: [
        button(
          Icons.chevron_left,
          'Start earlier by $step',
          c.canShiftStart(index, -1),
          () => c.shiftStart(index, -1),
        ),
        button(
          Icons.chevron_right,
          'Start later by $step (cut the beginning)',
          c.canShiftStart(index, 1),
          () => c.shiftStart(index, 1),
        ),
        Expanded(
          child: Stack(
            children: [
              Positioned.fill(child: wave),
              Positioned(
                left: 2,
                top: 1,
                child: Tooltip(
                  message: 'Trim step — click to change (⅛ / ¼ / bar)',
                  child: InkWell(
                    onTap: c.cycleTrimStep,
                    borderRadius: BorderRadius.circular(4),
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 4),
                      decoration: BoxDecoration(
                        color: LooperColors.surfaceHigh.withValues(alpha: 0.85),
                        borderRadius: BorderRadius.circular(4),
                      ),
                      child: Text(step, style: const TextStyle(fontSize: 10, fontWeight: FontWeight.w700)),
                    ),
                  ),
                ),
              ),
              if (c.canResetTrim(index))
                Positioned(
                  right: 2,
                  top: 1,
                  child: Tooltip(
                    message: 'Undo all trimming: back to the take as recorded',
                    child: InkWell(
                      onTap: () => c.resetTrim(index),
                      borderRadius: BorderRadius.circular(4),
                      child: Container(
                        padding: const EdgeInsets.symmetric(horizontal: 3),
                        decoration: BoxDecoration(
                          color: LooperColors.surfaceHigh.withValues(alpha: 0.85),
                          borderRadius: BorderRadius.circular(4),
                        ),
                        child: const Icon(Icons.restart_alt, size: 12),
                      ),
                    ),
                  ),
                ),
            ],
          ),
        ),
        button(
          Icons.chevron_left,
          'Remove the last bar',
          c.canTrimBars(index, -1),
          () => c.trimBars(index, -1),
        ),
        button(Icons.chevron_right, 'Add a bar back', c.canTrimBars(index, 1), () => c.trimBars(index, 1)),
      ],
    );
  }
}

class _WaveformPainter extends CustomPainter {
  _WaveformPainter({
    required this.data,
    required this.windowFrom,
    required this.windowTo,
    required this.playhead,
    required this.color,
  });

  final List<double> data;

  /// Where the loop sits in the shown span (0..1); outside is dimmed.
  final double windowFrom;
  final double windowTo;

  /// 0..1 within the loop window.
  final double? playhead;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final mid = size.height / 2;
    final rrect = RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(6));
    canvas.drawRRect(rrect, Paint()..color = LooperColors.background);
    if (data.isEmpty) return;
    final inside = Paint()
      ..color = color.withValues(alpha: 0.75)
      ..strokeWidth = math.max(1, size.width / data.length * 0.7);
    final outside = Paint()
      ..color = LooperColors.muted.withValues(alpha: 0.5)
      ..strokeWidth = inside.strokeWidth;
    for (var i = 0; i < data.length; i++) {
      final f = (i + 0.5) / data.length;
      final x = f * size.width;
      final h = math.max(0.5, math.sqrt(data[i]) * mid * 0.95);
      final inWindow = f >= windowFrom && f <= windowTo;
      canvas.drawLine(Offset(x, mid - h), Offset(x, mid + h), inWindow ? inside : outside);
    }
    // Brackets around the loop when the recording extends beyond it.
    if (windowFrom > 0.001 || windowTo < 0.999) {
      final bracket = Paint()
        ..color = LooperColors.textPrimary.withValues(alpha: 0.8)
        ..strokeWidth = 1.5
        ..style = PaintingStyle.stroke;
      for (final f in [windowFrom, windowTo]) {
        final x = f * size.width;
        canvas.drawLine(Offset(x, 2), Offset(x, size.height - 2), bracket);
      }
    }
    final p = playhead;
    if (p != null) {
      final x = (windowFrom + p * (windowTo - windowFrom)) * size.width;
      canvas.drawLine(
        Offset(x, 0),
        Offset(x, size.height),
        Paint()
          ..color = LooperColors.textPrimary
          ..strokeWidth = 2,
      );
    }
  }

  @override
  bool shouldRepaint(_WaveformPainter old) =>
      old.playhead != playhead ||
      !identical(old.data, data) ||
      old.color != color ||
      old.windowFrom != windowFrom ||
      old.windowTo != windowTo;
}

class _LengthSelector extends StatelessWidget {
  const _LengthSelector({required this.value, required this.enabled, required this.onChanged});

  final int value;
  final bool enabled;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: 'Loop length in bars. Auto = stop when you tap again',
      child: Row(
        children: [
          for (final m in lengthModes)
            Expanded(
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 1.5),
                child: _Chip(
                  label: lengthModeLabel(m),
                  selected: m == value,
                  onTap: enabled ? () => onChanged(m) : null,
                ),
              ),
            ),
        ],
      ),
    );
  }
}

class _Chip extends StatelessWidget {
  const _Chip({required this.label, required this.selected, required this.onTap});

  final String label;
  final bool selected;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final enabled = onTap != null;
    return InkWell(
      borderRadius: BorderRadius.circular(6),
      onTap: onTap,
      child: Container(
        height: 24,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(6),
          color: selected ? LooperColors.surfaceHigh : Colors.transparent,
          border: Border.all(color: selected ? LooperColors.textSecondary : LooperColors.outline),
        ),
        child: Text(
          label,
          style: TextStyle(
            fontSize: 12,
            fontWeight: selected ? FontWeight.w700 : FontWeight.w400,
            color: enabled || selected ? LooperColors.textPrimary : LooperColors.textSecondary,
          ),
        ),
      ),
    );
  }
}

/// Overdub toggle plus one level of undo/redo for the last layer.
class _LayerControls extends StatelessWidget {
  const _LayerControls({
    required this.controller,
    required this.index,
    required this.status,
    required this.track,
  });

  final LooperController controller;
  final int index;
  final EngineStatus status;
  final TrackStatus track;

  @override
  Widget build(BuildContext context) {
    final t = track;
    final canLayer = t.state == TrackState.playing && (status.playing || t.overdubbing);
    return Row(
      children: [
        Expanded(
          child: Tooltip(
            message: t.overdubbing
                ? 'Finish layer (⌥${index + 1})'
                : 'Record a layer on top of this loop (⌥${index + 1}, or hold the big button)',
            child: FilledButton.tonalIcon(
              style: FilledButton.styleFrom(
                visualDensity: VisualDensity.compact,
                backgroundColor: t.overdubbing ? LooperColors.overdub : null,
                foregroundColor: t.overdubbing ? Colors.black : null,
              ),
              onPressed: canLayer ? () => controller.toggleOverdub(index) : null,
              icon: Icon(t.overdubbing ? Icons.check : Icons.layers, size: 18),
              label: Text(t.overdubbing ? 'Finish layer' : 'Layer'),
            ),
          ),
        ),
        const SizedBox(width: 4),
        Tooltip(
          message: t.overdubbing ? 'Cancel this layer' : 'Undo last layer (⌘Z)',
          child: IconButton(
            visualDensity: VisualDensity.compact,
            onPressed: t.canUndo ? () => controller.undoTrack(index) : null,
            icon: const Icon(Icons.undo, size: 20),
          ),
        ),
        Tooltip(
          message: 'Redo layer (⇧⌘Z)',
          child: IconButton(
            visualDensity: VisualDensity.compact,
            onPressed: t.canRedo ? () => controller.redoTrack(index) : null,
            icon: const Icon(Icons.redo, size: 20),
          ),
        ),
        Tooltip(
          message: t.muted ? 'Unmute' : 'Mute',
          child: IconButton(
            visualDensity: VisualDensity.compact,
            isSelected: t.muted,
            onPressed: t.state == TrackState.playing ? () => controller.toggleMute(index) : null,
            icon: Icon(t.muted ? Icons.volume_off : Icons.volume_up, size: 20),
          ),
        ),
        Tooltip(
          message: 'Clear (Shift+${index + 1})',
          child: IconButton(
            visualDensity: VisualDensity.compact,
            onPressed: t.state == TrackState.empty ? null : () => controller.clearTrack(index),
            icon: const Icon(Icons.delete_outline, size: 20),
          ),
        ),
      ],
    );
  }
}

/// Pan from L to R; double-click (or double-tap) centres it.
class _PanSlider extends StatelessWidget {
  const _PanSlider({required this.controller, required this.index});

  final LooperController controller;
  final int index;

  @override
  Widget build(BuildContext context) {
    final pan = controller.tracks[index].pan;
    final label = pan.abs() < 0.01 ? 'C' : '${pan < 0 ? 'L' : 'R'}${(pan.abs() * 100).round()}';
    return Tooltip(
      message: 'Pan $label — double-click to centre',
      child: GestureDetector(
        onDoubleTap: () => controller.setTrackPan(index, 0),
        child: Row(
          children: [
            const Text('L', style: TextStyle(fontSize: 11, color: LooperColors.textSecondary)),
            Expanded(
              child: Slider(value: pan, min: -1, onChanged: (v) => controller.setTrackPan(index, v)),
            ),
            const Text('R', style: TextStyle(fontSize: 11, color: LooperColors.textSecondary)),
          ],
        ),
      ),
    );
  }
}

/// One press brings the loop's peak to -1 dBFS (non-destructive gain on top
/// of the volume); the button then shows the boost, and a second press
/// switches it off.
class _NormalizeButton extends StatelessWidget {
  const _NormalizeButton({required this.controller, required this.index});

  final LooperController controller;
  final int index;

  @override
  Widget build(BuildContext context) {
    final t = controller.status.tracks[index];
    final on = t.normalized;
    final db = on ? 20 * math.log(t.normGain) / math.ln10 : 0.0;
    final label = on ? '${db >= 0 ? '+' : ''}${db.toStringAsFixed(db.abs() < 10 ? 1 : 0)}' : 'N';
    return Tooltip(
      message: on
          ? 'Normalized (${label}dB) — click to switch off'
          : 'Normalize: bring the loudest peak to -1 dBFS',
      child: SizedBox(
        width: 42,
        height: 26,
        child: TextButton(
          style: TextButton.styleFrom(
            padding: EdgeInsets.zero,
            backgroundColor: on ? LooperColors.play.withValues(alpha: 0.2) : null,
            foregroundColor: on ? LooperColors.play : LooperColors.textSecondary,
            textStyle: const TextStyle(fontSize: 11, fontWeight: FontWeight.w700),
          ),
          onPressed: t.state == TrackState.playing
              ? () async {
                  final messenger = ScaffoldMessenger.of(context);
                  final result = await controller.toggleNormalize(index);
                  if (result == null) {
                    messenger.showSnackBar(
                      const SnackBar(content: Text('Nothing to normalize: the loop is silent.')),
                    );
                  }
                }
              : null,
          child: Text(label),
        ),
      ),
    );
  }
}
