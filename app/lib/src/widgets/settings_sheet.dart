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
            const SizedBox(height: 8),
            _CalibrationPanel(controller: c),
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
            _Field(
              label: 'Quantize mute',
              child: SegmentedButton<int>(
                segments: const [
                  ButtonSegment(value: 0, label: Text('Off')),
                  ButtonSegment(value: 1, label: Text('Beat')),
                  ButtonSegment(value: 2, label: Text('Bar')),
                ],
                selected: {c.muteQuantize},
                onSelectionChanged: (v) => c.setMuteQuantize(v.first),
              ),
            ),
            const Padding(
              padding: EdgeInsets.only(left: 120, bottom: 4),
              child: Text(
                'Mute / unmute waits for the next beat or bar, so tracks drop in and out in time.',
                style: TextStyle(fontSize: 12, color: LooperColors.textSecondary),
              ),
            ),
            _Field(
              label: 'Layer feedback',
              child: Row(
                children: [
                  Expanded(
                    child: Slider(value: c.feedback, divisions: 20, onChanged: c.setFeedback),
                  ),
                  SizedBox(width: 48, child: Text('${(c.feedback * 100).round()}%')),
                ],
              ),
            ),
            const Padding(
              padding: EdgeInsets.only(left: 120, bottom: 4),
              child: Text(
                'How much of the loop survives each pass while you layer. 100% keeps everything; '
                'lower lets older parts fade away. Undo brings them back.',
                style: TextStyle(fontSize: 12, color: LooperColors.textSecondary),
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

/// Measures the real round trip through a loopback and applies it.
class _CalibrationPanel extends StatefulWidget {
  const _CalibrationPanel({required this.controller});

  final LooperController controller;

  @override
  State<_CalibrationPanel> createState() => _CalibrationPanelState();
}

class _CalibrationPanelState extends State<_CalibrationPanel> {
  int _channel = 0;
  double? _measuredMs;
  String? _error;

  @override
  Widget build(BuildContext context) {
    final c = widget.controller;
    final inputs = c.status.inputChannels;
    final channel = _channel.clamp(0, (inputs - 1).clamp(0, 63));
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(color: LooperColors.surfaceHigh, borderRadius: BorderRadius.circular(10)),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Measure latency', style: TextStyle(fontWeight: FontWeight.w600)),
          const SizedBox(height: 4),
          const Text(
            'Connect an output of your interface to an input with a cable (or hold a mic close to '
            'the speakers), turn the input gain up a little, then press Measure. You will hear 8 short beeps.',
            style: TextStyle(fontSize: 12, color: LooperColors.textSecondary),
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 12,
            runSpacing: 8,
            crossAxisAlignment: WrapCrossAlignment.center,
            children: [
              DropdownButton<int>(
                value: channel,
                onChanged: c.calibrating ? null : (v) => setState(() => _channel = v!),
                items: [
                  for (var i = 0; i < (inputs < 1 ? 1 : inputs); i++)
                    DropdownMenuItem(value: i, child: Text('Listen on In ${i + 1}')),
                ],
              ),
              FilledButton.tonalIcon(
                onPressed: c.calibrating || !c.status.deviceRunning ? null : () => _measure(channel),
                icon: c.calibrating
                    ? const SizedBox.square(dimension: 16, child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.graphic_eq),
                label: Text(c.calibrating ? 'Measuring…' : 'Measure'),
              ),
              if (_measuredMs != null) ...[
                Text('Round trip: ${_measuredMs!.toStringAsFixed(1)} ms'),
                FilledButton(
                  onPressed: () {
                    c.applyMeasuredLatency(_measuredMs!);
                    setState(() => _measuredMs = null);
                  },
                  child: const Text('Apply'),
                ),
              ],
            ],
          ),
          if (_error != null)
            Padding(
              padding: const EdgeInsets.only(top: 6),
              child: Text(_error!, style: const TextStyle(color: LooperColors.record, fontSize: 12)),
            ),
        ],
      ),
    );
  }

  Future<void> _measure(int channel) async {
    setState(() {
      _error = null;
      _measuredMs = null;
    });
    try {
      final ms = await widget.controller.measureLatency(channel);
      if (mounted) setState(() => _measuredMs = ms);
    } on StateError catch (e) {
      if (mounted) {
        setState(() => _error = 'Could not measure: ${e.message}. Check the loopback and the input level.');
      }
    }
  }
}
