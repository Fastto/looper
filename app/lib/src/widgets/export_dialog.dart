import 'dart:io';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';

import '../export/audio_export.dart';
import '../looper_controller.dart';
import '../theme.dart';

/// Asks what to export, lets the user pick a folder, writes the files and
/// reports where they went.
Future<void> showExportDialog(BuildContext context, LooperController controller) async {
  final messenger = ScaffoldMessenger.of(context);
  final result = await showDialog<ExportResult>(
    context: context,
    builder: (_) => ExportDialog(controller: controller),
  );
  if (result == null) return;
  final count = result.files.length;
  messenger.showSnackBar(
    SnackBar(
      content: Text('Exported ${count == 1 ? '1 file' : '$count files'} to ${result.folder}'),
      duration: const Duration(seconds: 6),
      action: Platform.isMacOS
          ? SnackBarAction(label: 'Show', onPressed: () => Process.run('open', [result.folder]))
          : null,
    ),
  );
}

class ExportDialog extends StatefulWidget {
  const ExportDialog({super.key, required this.controller});

  final LooperController controller;

  @override
  State<ExportDialog> createState() => _ExportDialogState();
}

class _ExportDialogState extends State<ExportDialog> {
  bool _stems = true;
  bool _mix = true;
  bool _includeMuted = false;
  int? _halfBars; // null = follow the default for the current selection
  bool _working = false;
  String? _error;

  LooperController get _c => widget.controller;

  Map<int, int> get _loops => _c.loopHalfBars(includeMuted: _includeMuted);

  int get _length => _halfBars ?? defaultExportHalfBars(_loops.values);

  @override
  Widget build(BuildContext context) {
    final loops = _loops;
    final length = _length;
    final status = _c.status;
    final seconds = length * (status.barFrames ~/ 2) / status.sampleRate;
    final misaligned = loops.values.any((h) => h > 0 && length % h != 0);
    final canExport = !_working && loops.isNotEmpty && (_stems || _mix) && length > 0;

    return AlertDialog(
      title: const Text('Export audio'),
      content: SizedBox(
        width: 420,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              loops.isEmpty
                  ? 'Nothing to export: record a loop first.'
                  : '${loops.length} ${loops.length == 1 ? 'loop' : 'loops'} · '
                        '${status.bpm.toStringAsFixed(0)} BPM · 32-bit float WAV, ${status.sampleRate} Hz',
              style: const TextStyle(color: LooperColors.textSecondary),
            ),
            const SizedBox(height: 16),
            Row(
              children: [
                const SizedBox(width: 72, child: Text('Length')),
                IconButton(
                  onPressed: length > 1
                      ? () => setState(() => _halfBars = length - (length.isEven ? 2 : 1))
                      : null,
                  icon: const Icon(Icons.remove),
                ),
                SizedBox(
                  width: 120,
                  child: Text(
                    '${_bars(length)} · ${_duration(seconds)}',
                    textAlign: TextAlign.center,
                    style: const TextStyle(fontWeight: FontWeight.w600),
                  ),
                ),
                IconButton(
                  onPressed: length < maxExportBars * 2
                      ? () => setState(() => _halfBars = length + (length.isEven ? 2 : 1))
                      : null,
                  icon: const Icon(Icons.add),
                ),
                if (_halfBars != null)
                  TextButton(onPressed: () => setState(() => _halfBars = null), child: const Text('Auto')),
              ],
            ),
            Text(
              misaligned
                  ? 'Some loops will stop mid-cycle at this length.'
                  : 'Every loop repeats from bar 1 and ends on a full cycle, so the files line up in a DAW.',
              style: TextStyle(
                fontSize: 12,
                color: misaligned ? LooperColors.downbeat : LooperColors.textSecondary,
              ),
            ),
            const SizedBox(height: 8),
            CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              value: _stems,
              onChanged: (v) => setState(() => _stems = v!),
              title: const Text('Stems'),
              subtitle: const Text('One file per track, at its volume'),
            ),
            CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              value: _mix,
              onChanged: (v) => setState(() => _mix = v!),
              title: const Text('Mix'),
              subtitle: const Text('All tracks together, as you hear them'),
            ),
            CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              value: _includeMuted,
              onChanged: (v) => setState(() => _includeMuted = v!),
              title: const Text('Include muted tracks'),
            ),
            if (_error != null) Text(_error!, style: const TextStyle(color: LooperColors.record)),
          ],
        ),
      ),
      actions: [
        TextButton(onPressed: _working ? null : () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton.icon(
          onPressed: canExport ? _export : null,
          icon: _working
              ? const SizedBox.square(dimension: 16, child: CircularProgressIndicator(strokeWidth: 2))
              : const Icon(Icons.folder_open),
          label: Text(_working ? 'Exporting…' : 'Choose folder…'),
        ),
      ],
    );
  }

  Future<void> _export() async {
    final folder = await getDirectoryPath(confirmButtonText: 'Export here');
    if (folder == null || !mounted) return;
    setState(() {
      _working = true;
      _error = null;
    });
    try {
      final loops = await _c.collectLoops(includeMuted: _includeMuted);
      if (loops.isEmpty) throw StateError('no loop is ready to export');
      final status = _c.status;
      final result = await runExport(
        ExportJob(
          folder: folder,
          title: '${_c.sessions.name} - ${status.bpm.toStringAsFixed(0)} BPM',
          sampleRate: status.sampleRate,
          frames: _length * (status.barFrames ~/ 2),
          loops: loops,
          stems: _stems,
          mix: _mix,
        ),
      );
      if (mounted) Navigator.pop(context, result);
    } on Object catch (e) {
      if (mounted) {
        setState(() {
          _working = false;
          _error = 'Export failed: $e';
        });
      }
    }
  }

  static String _bars(int halfBars) {
    if (halfBars == 1) return '½ bar';
    final bars = halfBars / 2;
    final text = halfBars.isEven ? bars.toStringAsFixed(0) : bars.toStringAsFixed(1);
    return halfBars == 2 ? '1 bar' : '$text bars';
  }

  static String _duration(double seconds) {
    final s = seconds.round();
    return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
  }
}
