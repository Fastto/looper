import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../session/session.dart';
import '../session/session_manager.dart';
import '../theme.dart';

Future<void> showSessionsSheet(BuildContext context, SessionManager sessions) {
  sessions.refreshList();
  return showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    backgroundColor: LooperColors.surface,
    constraints: const BoxConstraints(maxWidth: 720),
    builder: (_) => ListenableBuilder(
      listenable: sessions,
      builder: (context, _) => SessionsSheet(sessions: sessions),
    ),
  );
}

/// Button in the transport bar: the session name plus a saved indicator.
class SessionButton extends StatelessWidget {
  const SessionButton({super.key, required this.sessions, required this.onPressed});

  final SessionManager sessions;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: sessions,
      builder: (context, _) {
        final saved = sessions.lastSavedAt;
        final error = sessions.lastError;
        return Tooltip(
          message:
              error ??
              (saved != null
                  ? 'Saved automatically at ${_time(saved)} — open sessions'
                  : 'Saved automatically once you record a loop — open sessions'),
          child: TextButton.icon(
            onPressed: onPressed,
            icon: Icon(
              error != null ? Icons.error_outline : Icons.folder_open,
              size: 20,
              color: error != null ? LooperColors.record : null,
            ),
            label: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 180),
              child: Text(sessions.name, overflow: TextOverflow.ellipsis),
            ),
          ),
        );
      },
    );
  }
}

class SessionsSheet extends StatefulWidget {
  const SessionsSheet({super.key, required this.sessions});

  final SessionManager sessions;

  @override
  State<SessionsSheet> createState() => _SessionsSheetState();
}

class _SessionsSheetState extends State<SessionsSheet> {
  late final TextEditingController _name = TextEditingController(text: widget.sessions.name);

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  bool _nameFocused = false;

  @override
  Widget build(BuildContext context) {
    final s = widget.sessions;
    if (!_nameFocused && _name.text != s.name) _name.text = s.name;
    final others = s.sessions.where((i) => i.id != s.id).toList();

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(24, 16, 24, 16),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Session', style: Theme.of(context).textTheme.headlineSmall),
            const SizedBox(height: 8),
            Focus(
              onFocusChange: (f) {
                _nameFocused = f;
                if (!f) s.rename(_name.text);
              },
              child: TextField(
                controller: _name,
                decoration: const InputDecoration(labelText: 'Name', isDense: true),
                onSubmitted: s.rename,
              ),
            ),
            const SizedBox(height: 6),
            Text(
              s.lastSavedAt != null
                  ? 'Saved automatically · ${_time(s.lastSavedAt!)}'
                  : 'Saved automatically once you record a loop.',
              style: const TextStyle(color: LooperColors.textSecondary, fontSize: 12),
            ),
            if (s.lastError != null)
              Padding(
                padding: const EdgeInsets.only(top: 6),
                child: Text(s.lastError!, style: const TextStyle(color: LooperColors.record)),
              ),
            const SizedBox(height: 12),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                FilledButton.icon(
                  onPressed: () => _run(context, s.newSession),
                  icon: const Icon(Icons.add),
                  label: const Text('New session'),
                ),
                OutlinedButton.icon(
                  onPressed: s.onDisk ? () => _run(context, s.duplicate) : null,
                  icon: const Icon(Icons.copy),
                  label: const Text('Duplicate'),
                ),
              ],
            ),
            const Divider(height: 28),
            Text('Saved sessions', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 4),
            if (others.isEmpty)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 12),
                child: Text('No other sessions yet.', style: TextStyle(color: LooperColors.textSecondary)),
              )
            else
              ConstrainedBox(
                constraints: BoxConstraints(maxHeight: MediaQuery.sizeOf(context).height * 0.45),
                child: ListView(
                  shrinkWrap: true,
                  children: [
                    for (final info in others)
                      _SessionTile(
                        info: info,
                        onOpen: () => _run(context, () => s.open(info.id)),
                        onDelete: () => _confirmDelete(context, info),
                      ),
                  ],
                ),
              ),
            if (s.folder != null)
              Padding(
                padding: const EdgeInsets.only(top: 12),
                child: Row(
                  children: [
                    Expanded(
                      child: Text(
                        s.folder!,
                        style: const TextStyle(fontSize: 11, color: LooperColors.textSecondary),
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                    IconButton(
                      tooltip: 'Copy folder path',
                      iconSize: 16,
                      onPressed: () => Clipboard.setData(ClipboardData(text: s.folder!)),
                      icon: const Icon(Icons.content_copy),
                    ),
                  ],
                ),
              ),
          ],
        ),
      ),
    );
  }

  Future<void> _run(BuildContext context, Future<void> Function() action) async {
    widget.sessions.rename(_name.text);
    final navigator = Navigator.of(context);
    await action();
    if (mounted && widget.sessions.lastError == null) navigator.pop();
  }

  Future<void> _confirmDelete(BuildContext context, SessionInfo info) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Delete "${info.data.name}"?'),
        content: const Text('Its loops will be deleted from disk. This cannot be undone.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: LooperColors.record),
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
    if (ok == true) await widget.sessions.delete(info.id);
  }
}

class _SessionTile extends StatelessWidget {
  const _SessionTile({required this.info, required this.onOpen, required this.onDelete});

  final SessionInfo info;
  final VoidCallback onOpen;
  final VoidCallback onDelete;

  @override
  Widget build(BuildContext context) {
    final d = info.data;
    final loops = d.loopCount == 1 ? '1 loop' : '${d.loopCount} loops';
    return ListTile(
      contentPadding: EdgeInsets.zero,
      leading: const Icon(Icons.queue_music),
      title: Text(d.name, overflow: TextOverflow.ellipsis),
      subtitle: Text(
        '${d.bpm.toStringAsFixed(0)} BPM · ${d.beatsPerBar}/4 · $loops · ${_date(d.updatedAt)}',
        style: const TextStyle(color: LooperColors.textSecondary),
      ),
      onTap: onOpen,
      trailing: IconButton(
        tooltip: 'Delete session',
        onPressed: onDelete,
        icon: const Icon(Icons.delete_outline),
      ),
    );
  }
}

String _two(int v) => v.toString().padLeft(2, '0');

String _time(DateTime t) => '${_two(t.hour)}:${_two(t.minute)}';

String _date(DateTime t) => '${_two(t.day)}.${_two(t.month)}.${t.year} ${_time(t)}';
