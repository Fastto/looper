import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../looper_controller.dart';
import '../theme.dart';

Future<void> showSettingsSheet(BuildContext context, LooperController controller) {
  return showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    backgroundColor: LooperColors.surface,
    constraints: const BoxConstraints(maxWidth: 720),
    builder: (_) => ListenableBuilder(
      listenable: controller,
      builder: (context, _) => SettingsSheet(controller: controller),
    ),
  );
}

class SettingsSheet extends StatelessWidget {
  const SettingsSheet({super.key, required this.controller});

  final LooperController controller;

  @override
  Widget build(BuildContext context) {
    final c = controller;
    final s = c.status;
    final rate = s.sampleRate;
    final reportedMs = c.reportedLatencyFrames / rate * 1000;
    final totalMs = s.latencyFrames / rate * 1000;
    final holdsAudio = s.anyTrackHoldsAudio;

    return SafeArea(
      child: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(24, 16, 24, 24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Text('Audio', style: Theme.of(context).textTheme.headlineSmall),
                const Spacer(),
                IconButton(
                  tooltip: 'Rescan devices',
                  onPressed: c.refreshDevices,
                  icon: const Icon(Icons.refresh),
                ),
              ],
            ),
            if (holdsAudio)
              const Padding(
                padding: EdgeInsets.only(bottom: 8),
                child: Text(
                  'Changing the device saves and reloads your loops (layers are merged, undo is reset).',
                  style: TextStyle(color: LooperColors.textSecondary),
                ),
              ),
            _Field(
              label: 'Input device',
              child: _DeviceDropdown(
                names: [for (final d in c.devices.capture) d.name],
                value: c.captureName,
                onChanged: (v) => c.selectDevices(capture: v),
              ),
            ),
            _Field(
              label: 'Output device',
              child: _DeviceDropdown(
                names: [for (final d in c.devices.playback) d.name],
                value: c.playbackName,
                onChanged: (v) => c.selectDevices(playback: v),
              ),
            ),
            _Field(
              label: 'Buffer size',
              child: DropdownButton<int>(
                value: c.bufferFrames,
                onChanged: (v) => c.selectDevices(buffer: v),
                items: [
                  for (final n in [64, 128, 256, 512])
                    DropdownMenuItem(
                      value: n,
                      child: Text('$n frames  (${(n / rate * 1000).toStringAsFixed(1)} ms)'),
                    ),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: Text(
                s.deviceRunning
                    ? '$rate Hz · ${s.inputChannels} inputs'
                    : (c.error ?? 'Audio device not running'),
                style: TextStyle(color: s.deviceRunning ? LooperColors.textSecondary : LooperColors.record),
              ),
            ),
            const SizedBox(height: 8),
            Text('Inputs', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 8),
            _InputMeters(peaks: s.inputPeaks),
            const Divider(height: 32),
            Text('Timing', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 4),
            Text(
              'Recordings are shifted back by the round-trip latency so they land on the grid. '
              'Reported by the system: ${reportedMs.toStringAsFixed(1)} ms. '
              'If loops sound late, increase the offset; if early, decrease it.',
              style: const TextStyle(color: LooperColors.textSecondary),
            ),
            Row(
              children: [
                const SizedBox(width: 120, child: Text('Offset')),
                Expanded(
                  child: Slider(
                    value: c.latencyOffsetMs.clamp(-50, 100),
                    min: -50,
                    max: 100,
                    divisions: 150,
                    label: '${c.latencyOffsetMs.toStringAsFixed(0)} ms',
                    onChanged: c.setLatencyOffsetMs,
                  ),
                ),
                SizedBox(
                  width: 150,
                  child: Text(
                    '${c.latencyOffsetMs >= 0 ? '+' : ''}${c.latencyOffsetMs.toStringAsFixed(0)} ms '
                    '(total ${totalMs.toStringAsFixed(1)})',
                  ),
                ),
              ],
            ),
            _Field(
              label: 'Count-in',
              child: SegmentedButton<int>(
                segments: const [
                  ButtonSegment(value: 0, label: Text('Off')),
                  ButtonSegment(value: 1, label: Text('1 bar')),
                  ButtonSegment(value: 2, label: Text('2 bars')),
                ],
                selected: {c.countInBars},
                onSelectionChanged: (v) => c.setCountIn(v.first),
              ),
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Software input monitoring'),
              subtitle: const Text(
                'Hear the input of armed / recording tracks through the app. '
                'Leave off if your interface has direct monitoring.',
              ),
              value: c.monitoring,
              onChanged: c.setMonitoring,
            ),
          ],
        ),
      ),
    );
  }
}

class _Field extends StatelessWidget {
  const _Field({required this.label, required this.child});

  final String label;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        children: [
          SizedBox(width: 120, child: Text(label)),
          Flexible(child: child),
        ],
      ),
    );
  }
}

class _DeviceDropdown extends StatelessWidget {
  const _DeviceDropdown({required this.names, required this.value, required this.onChanged});

  final List<String> names;
  final String? value;
  final ValueChanged<String?> onChanged;

  @override
  Widget build(BuildContext context) {
    return DropdownButton<String?>(
      isExpanded: true,
      value: names.contains(value) ? value : null,
      onChanged: onChanged,
      items: [
        const DropdownMenuItem<String?>(value: null, child: Text('System default')),
        for (final n in names.toSet())
          DropdownMenuItem<String?>(
            value: n,
            child: Text(n, overflow: TextOverflow.ellipsis),
          ),
      ],
    );
  }
}

class _InputMeters extends StatelessWidget {
  const _InputMeters({required this.peaks});

  final List<double> peaks;

  @override
  Widget build(BuildContext context) {
    if (peaks.isEmpty) {
      return const Text('No inputs', style: TextStyle(color: LooperColors.textSecondary));
    }
    return Wrap(
      spacing: 12,
      runSpacing: 8,
      children: [
        for (var i = 0; i < peaks.length; i++)
          SizedBox(
            width: 150,
            child: Row(
              children: [
                SizedBox(width: 40, child: Text('In ${i + 1}')),
                Expanded(
                  child: LinearProgressIndicator(
                    value: ((20 * math.log(math.max(peaks[i], 1e-5)) / math.ln10 + 48) / 48).clamp(0.0, 1.0),
                    color: peaks[i] >= 0.99 ? LooperColors.downbeat : LooperColors.record,
                    backgroundColor: LooperColors.surfaceHigh,
                    minHeight: 6,
                    borderRadius: BorderRadius.circular(3),
                  ),
                ),
              ],
            ),
          ),
      ],
    );
  }
}
