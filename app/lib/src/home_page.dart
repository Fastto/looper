import 'dart:ui' show AppExitResponse;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:looper_engine/looper_engine.dart';

import 'looper_controller.dart';
import 'theme.dart';
import 'widgets/export_dialog.dart';
import 'widgets/sessions_sheet.dart';
import 'widgets/settings_sheet.dart';
import 'widgets/track_card.dart';
import 'widgets/transport_bar.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key, required this.controller});

  final LooperController controller;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with SingleTickerProviderStateMixin {
  late final Ticker _ticker;
  late final AppLifecycleListener _lifecycle;
  final _focus = FocusNode();

  static const _digitKeys = [
    PhysicalKeyboardKey.digit1,
    PhysicalKeyboardKey.digit2,
    PhysicalKeyboardKey.digit3,
    PhysicalKeyboardKey.digit4,
    PhysicalKeyboardKey.digit5,
    PhysicalKeyboardKey.digit6,
    PhysicalKeyboardKey.digit7,
    PhysicalKeyboardKey.digit8,
  ];

  LooperController get _c => widget.controller;

  @override
  void initState() {
    super.initState();
    _ticker = createTicker((_) => _c.poll())..start();
    // Never lose loops: save before the app quits or goes to the background.
    _lifecycle = AppLifecycleListener(
      onExitRequested: () async {
        await _c.sessions.flush();
        return AppExitResponse.exit;
      },
      onHide: _c.sessions.flush,
    );
  }

  @override
  void dispose() {
    _lifecycle.dispose();
    _ticker.dispose();
    _focus.dispose();
    super.dispose();
  }

  Future<void> _openExport() async {
    await showExportDialog(context, _c);
    _focus.requestFocus();
  }

  // Physical keys so shortcuts work with any keyboard layout (e.g. Ukrainian).
  KeyEventResult _onKey(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent) return KeyEventResult.ignored;
    final key = event.physicalKey;
    final keyboard = HardwareKeyboard.instance;
    final track = _digitKeys.indexOf(key);
    if (track >= 0) {
      if (keyboard.isShiftPressed) {
        _c.clearTrack(track);
      } else if (keyboard.isAltPressed) {
        _c.toggleOverdub(track);
      } else {
        _c.tapTrack(track);
      }
      return KeyEventResult.handled;
    }
    if (key == PhysicalKeyboardKey.keyE && (keyboard.isMetaPressed || keyboard.isControlPressed)) {
      _openExport();
      return KeyEventResult.handled;
    }
    if (key == PhysicalKeyboardKey.keyZ && (keyboard.isMetaPressed || keyboard.isControlPressed)) {
      keyboard.isShiftPressed ? _c.redoLast() : _c.undoLast();
      return KeyEventResult.handled;
    }
    if (key == PhysicalKeyboardKey.space) {
      _c.togglePlay();
      return KeyEventResult.handled;
    }
    if (key == PhysicalKeyboardKey.keyM) {
      _c.toggleMetronome();
      return KeyEventResult.handled;
    }
    if (key == PhysicalKeyboardKey.keyT) {
      _c.tapTempo();
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  @override
  Widget build(BuildContext context) {
    return Focus(
      focusNode: _focus,
      autofocus: true,
      onKeyEvent: _onKey,
      child: Scaffold(
        body: SafeArea(
          child: ListenableBuilder(
            listenable: _c,
            builder: (context, _) => Column(
              children: [
                TransportBar(
                  controller: _c,
                  onOpenSettings: () async {
                    await showSettingsSheet(context, _c);
                    _focus.requestFocus();
                  },
                  onOpenSessions: () async {
                    final export = await showSessionsSheet(context, _c.sessions);
                    if (export == true && context.mounted) await showExportDialog(context, _c);
                    _focus.requestFocus();
                  },
                ),
                if (_c.error != null) _ErrorBanner(message: _c.error!, onRetry: () => _c.selectDevices()),
                Expanded(child: _TrackGrid(controller: _c)),
                const _HintBar(),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _TrackGrid extends StatelessWidget {
  const _TrackGrid({required this.controller});

  final LooperController controller;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final columns = constraints.maxWidth >= 1000
            ? 4
            : constraints.maxWidth >= 640
            ? 2
            : 1;
        final rows = (maxTracks / columns).ceil();
        const gap = 12.0;
        final cellHeight = (constraints.maxHeight - gap * (rows + 1)) / rows;
        final scroll = cellHeight < 340;
        final height = scroll ? 360.0 : cellHeight;
        final width = (constraints.maxWidth - gap * (columns + 1)) / columns;
        return SingleChildScrollView(
          physics: scroll ? null : const NeverScrollableScrollPhysics(),
          padding: const EdgeInsets.all(gap),
          child: Wrap(
            spacing: gap,
            runSpacing: gap,
            children: [
              for (var i = 0; i < maxTracks; i++)
                SizedBox(
                  width: width,
                  height: height,
                  child: TrackCard(controller: controller, index: i),
                ),
            ],
          ),
        );
      },
    );
  }
}

class _ErrorBanner extends StatelessWidget {
  const _ErrorBanner({required this.message, required this.onRetry});

  final String message;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    return MaterialBanner(
      backgroundColor: LooperColors.record.withValues(alpha: 0.15),
      content: Text(message),
      leading: const Icon(Icons.error_outline, color: LooperColors.record),
      actions: [TextButton(onPressed: onRetry, child: const Text('Retry'))],
    );
  }
}

class _HintBar extends StatelessWidget {
  const _HintBar();

  @override
  Widget build(BuildContext context) {
    return const Padding(
      padding: EdgeInsets.only(bottom: 6),
      child: Text(
        '1–8 record / stop / mute   ·   ⌥1–8 layer   ·   ⌘Z / ⇧⌘Z undo / redo   ·   ⌘E export   ·   '
        'Shift+1–8 clear   ·   Space play/stop   ·   M click   ·   T tap tempo',
        style: TextStyle(fontSize: 12, color: LooperColors.textSecondary),
      ),
    );
  }
}
