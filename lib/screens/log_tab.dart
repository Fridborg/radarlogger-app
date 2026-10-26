import 'package:flutter/material.dart';
import 'package:share_plus/share_plus.dart';

import '../protocol.dart';
import '../radar_connection.dart';
import 'live_tab.dart' show speedColor;

class LogTab extends StatelessWidget {
  const LogTab({super.key, required this.conn});

  final RadarConnection conn;

  Future<void> _download(BuildContext context) async {
    final msg = await conn.downloadNew();
    if (context.mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
    }
  }

  Future<void> _export(BuildContext context) async {
    if (conn.log.count == 0) return;
    final path = await conn.log.exportCsv();
    await Share.shareXFiles([XFile(path)], text: 'Radarlogg från ${conn.name}');
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final records = conn.log.records;
    final onDevice = conn.live?.recordCount;
    final newCount = onDevice == null ? null : (onDevice - records.length);

    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 8),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Text(
                '${records.length} passager i telefonen'
                '${onDevice != null ? ' · $onDevice i radarn' : ''}',
                style: theme.textTheme.titleSmall,
              ),
              const SizedBox(height: 8),
              if (conn.downloading) ...[
                LinearProgressIndicator(
                  value: conn.downloadTarget > conn.downloadFrom
                      ? conn.downloadDone / (conn.downloadTarget - conn.downloadFrom)
                      : null,
                ),
                const SizedBox(height: 4),
                Text('Hämtar ${conn.downloadDone} av ${conn.downloadTarget - conn.downloadFrom}...'),
              ] else
                Row(
                  children: [
                    Expanded(
                      child: FilledButton.icon(
                        onPressed: conn.connected ? () => _download(context) : null,
                        icon: const Icon(Icons.download),
                        label: Text(newCount != null && newCount > 0
                            ? 'Hämta $newCount nya'
                            : 'Hämta nya'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: OutlinedButton.icon(
                        onPressed: records.isEmpty ? null : () => _export(context),
                        icon: const Icon(Icons.ios_share),
                        label: const Text('Exportera CSV'),
                      ),
                    ),
                  ],
                ),
            ],
          ),
        ),
        const Divider(height: 1),
        Expanded(
          child: records.isEmpty
              ? const Center(child: Text('Inga passager hämtade ännu.'))
              : ListView.builder(
                  itemCount: records.length,
                  itemBuilder: (_, i) {
                    final r = records[records.length - 1 - i]; // nyast först
                    final dt = r.dateTime;
                    return ListTile(
                      dense: true,
                      leading: CircleAvatar(
                        backgroundColor: speedColor(r.speed),
                        foregroundColor: Colors.white,
                        child: Text('${r.speed}', style: const TextStyle(fontWeight: FontWeight.bold)),
                      ),
                      title: Text(dt != null ? '${formatDate(dt)}  ${formatTime(dt)}' : 'Okänd tid'),
                      subtitle: Text('${r.distance} m · ${r.approaching ? 'mot radarn' : 'från radarn'}'),
                      trailing: Text('#${records.length - i}', style: theme.textTheme.bodySmall),
                    );
                  },
                ),
        ),
      ],
    );
  }
}
