import 'dart:ui' show FontFeature;

import 'package:flutter/material.dart';

import '../radar_connection.dart';

Color speedColor(int speed, {int limit = 50}) {
  if (speed == 0) return Colors.grey;
  if (speed <= limit) return const Color(0xFF16A34A);
  if (speed <= limit + 10) return const Color(0xFFF59E0B);
  return const Color(0xFFDC2626);
}

class LiveTab extends StatelessWidget {
  const LiveTab({super.key, required this.conn});

  final RadarConnection conn;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final p = conn.live;
    final speed = p?.speed ?? 0;
    final active = speed > 0;

    Widget stat(String value, String label) => Expanded(
          child: Column(
            children: [
              Text(value,
                  style: theme.textTheme.headlineSmall?.copyWith(fontWeight: FontWeight.bold)),
              const SizedBox(height: 2),
              Text(label, style: theme.textTheme.bodySmall),
            ],
          ),
        );

    Widget chip(bool ok, String okText, String badText) => Chip(
          avatar: Icon(ok ? Icons.check_circle : Icons.error_outline,
              size: 18, color: ok ? Colors.green : theme.colorScheme.error),
          label: Text(ok ? okText : badText),
        );

    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Card(
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: 28, horizontal: 16),
            child: Column(
              children: [
                Text(
                  active ? '$speed' : '–',
                  style: TextStyle(
                    fontSize: 120,
                    height: 1,
                    fontWeight: FontWeight.w800,
                    color: speedColor(speed),
                    fontFeatures: const [FontFeature.tabularFigures()],
                  ),
                ),
                Text('km/h', style: theme.textTheme.titleMedium),
                const SizedBox(height: 20),
                Row(
                  children: [
                    stat(active ? '${p!.distance}' : '–', 'avstånd m'),
                    stat(active ? (p!.approaching ? 'mot' : 'från') : '–', 'riktning'),
                    stat(active ? '${p!.angle}°' : '–', 'vinkel'),
                  ],
                ),
              ],
            ),
          ),
        ),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Row(
              children: [
                stat('${p?.recordCount ?? 0}', 'passager i radarn'),
                stat(p == null || p.lastPassSpeed == 0 ? '–' : '${p.lastPassSpeed}', 'senaste km/h'),
                stat('${p?.targets ?? 0}', 'mål just nu'),
              ],
            ),
          ),
        ),
        const SizedBox(height: 8),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          alignment: WrapAlignment.center,
          children: [
            chip(p?.radarOk ?? false, 'Radar OK', 'Ingen radardata'),
            chip(p?.timeSynced ?? false, 'Klocka ställd', 'Klocka ej ställd'),
            if (p?.logFull ?? false) chip(false, '', 'Loggen är full'),
            if (p?.simulator ?? false)
              const Chip(avatar: Icon(Icons.science, size: 18), label: Text('Simulator')),
          ],
        ),
        if (conn.error != null) ...[
          const SizedBox(height: 16),
          Text(conn.error!, textAlign: TextAlign.center,
              style: TextStyle(color: theme.colorScheme.error)),
        ],
      ],
    );
  }
}
