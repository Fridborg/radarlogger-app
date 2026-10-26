import 'package:flutter/material.dart';

import '../protocol.dart';
import '../radar_connection.dart';
import 'live_tab.dart' show speedColor;

enum Period { today, yesterday, week, month, all }

class StatsTab extends StatefulWidget {
  const StatsTab({super.key, required this.conn});

  final RadarConnection conn;

  @override
  State<StatsTab> createState() => _StatsTabState();
}

class _StatsTabState extends State<StatsTab> {
  Period _period = Period.today;
  int _limit = 50;

  (DateTime?, DateTime?) _range() {
    final now = DateTime.now();
    final today = DateTime(now.year, now.month, now.day);
    switch (_period) {
      case Period.today:
        return (today, null);
      case Period.yesterday:
        return (today.subtract(const Duration(days: 1)), today);
      case Period.week:
        return (today.subtract(const Duration(days: 6)), null);
      case Period.month:
        return (today.subtract(const Duration(days: 29)), null);
      case Period.all:
        return (null, null);
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final (from, to) = _range();

    final list = widget.conn.log.records.where((r) {
      final dt = r.dateTime;
      if (dt == null) return _period == Period.all;
      if (from != null && dt.isBefore(from)) return false;
      if (to != null && !dt.isBefore(to)) return false;
      return true;
    }).toList();

    final speeds = list.map((r) => r.speed).toList()..sort();
    final n = speeds.length;
    final maxS = n == 0 ? 0 : speeds.last;
    final avg = n == 0 ? 0.0 : speeds.reduce((a, b) => a + b) / n;
    final v85 = n == 0 ? 0 : speeds[((n * 0.85).ceil() - 1).clamp(0, n - 1)];
    final over = speeds.where((s) => s > _limit).length;
    final overPct = n == 0 ? 0.0 : over * 100 / n;

    PassRecord? fastest;
    for (final r in list) {
      if (fastest == null || r.speed > fastest.speed) fastest = r;
    }

    // Per timme
    final perHour = List<int>.filled(24, 0);
    for (final r in list) {
      final dt = r.dateTime;
      if (dt != null) perHour[dt.hour]++;
    }
    final maxHour = perHour.fold<int>(0, (a, b) => a > b ? a : b);

    // Hastighetsfördelning
    const buckets = [0, 30, 40, 50, 60, 70, 80];
    final bucketCounts = List<int>.filled(buckets.length, 0);
    for (final s in speeds) {
      var idx = 0;
      for (var i = 0; i < buckets.length; i++) {
        if (s >= buckets[i]) idx = i;
      }
      bucketCounts[idx]++;
    }
    final maxBucket = bucketCounts.fold<int>(0, (a, b) => a > b ? a : b);

    Widget tile(String value, String label, {Color? color}) => Expanded(
          child: Column(
            children: [
              Text(value,
                  style: theme.textTheme.headlineSmall
                      ?.copyWith(fontWeight: FontWeight.bold, color: color)),
              const SizedBox(height: 2),
              Text(label, style: theme.textTheme.bodySmall, textAlign: TextAlign.center),
            ],
          ),
        );

    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            for (final p in Period.values)
              ChoiceChip(
                label: Text(switch (p) {
                  Period.today => 'Idag',
                  Period.yesterday => 'Igår',
                  Period.week => '7 dagar',
                  Period.month => '30 dagar',
                  Period.all => 'Allt',
                }),
                selected: _period == p,
                onSelected: (_) => setState(() => _period = p),
              ),
          ],
        ),
        const SizedBox(height: 12),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              children: [
                Row(children: [
                  tile('$n', 'fordon'),
                  tile(n == 0 ? '–' : '$maxS', 'högsta km/h', color: n == 0 ? null : speedColor(maxS, limit: _limit)),
                  tile(n == 0 ? '–' : avg.toStringAsFixed(0), 'snitt km/h'),
                ]),
                const SizedBox(height: 16),
                Row(children: [
                  tile(n == 0 ? '–' : '$v85', 'v85 km/h\n(85 % kör långsammare)'),
                  tile(n == 0 ? '–' : '${overPct.toStringAsFixed(0)} %', 'över $_limit km/h'),
                  tile('$over', 'antal över'),
                ]),
              ],
            ),
          ),
        ),
        Row(
          children: [
            const Text('Hastighetsgräns:'),
            const SizedBox(width: 8),
            for (final l in [30, 40, 50, 60, 70, 80])
              Padding(
                padding: const EdgeInsets.only(right: 4),
                child: ChoiceChip(
                  label: Text('$l'),
                  selected: _limit == l,
                  visualDensity: VisualDensity.compact,
                  onSelected: (_) => setState(() => _limit = l),
                ),
              ),
          ],
        ),
        if (fastest != null && fastest.dateTime != null)
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Text(
              'Snabbast: ${fastest.speed} km/h  ${formatDate(fastest.dateTime!)} ${formatTime(fastest.dateTime!)}',
              style: theme.textTheme.bodyMedium,
            ),
          ),
        const SizedBox(height: 16),
        Text('Fordon per timme', style: theme.textTheme.titleSmall),
        const SizedBox(height: 8),
        SizedBox(
          height: 140,
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              for (var h = 0; h < 24; h++)
                Expanded(
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 1),
                    child: Column(
                      mainAxisAlignment: MainAxisAlignment.end,
                      children: [
                        Flexible(
                          child: FractionallySizedBox(
                            heightFactor: maxHour == 0 ? 0 : perHour[h] / maxHour,
                            child: Container(
                              decoration: BoxDecoration(
                                color: theme.colorScheme.primary,
                                borderRadius: const BorderRadius.vertical(top: Radius.circular(3)),
                              ),
                            ),
                          ),
                        ),
                        const SizedBox(height: 2),
                        Text(h % 3 == 0 ? '$h' : '', style: const TextStyle(fontSize: 9)),
                      ],
                    ),
                  ),
                ),
            ],
          ),
        ),
        const SizedBox(height: 20),
        Text('Hastighetsfördelning', style: theme.textTheme.titleSmall),
        const SizedBox(height: 8),
        for (var i = 0; i < buckets.length; i++)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 3),
            child: Row(
              children: [
                SizedBox(
                  width: 70,
                  child: Text(i == buckets.length - 1
                      ? '${buckets[i]}+'
                      : i == 0
                          ? '< ${buckets[1]}'
                          : '${buckets[i]}–${buckets[i + 1] - 1}'),
                ),
                Expanded(
                  child: LayoutBuilder(
                    builder: (_, c) => Align(
                      alignment: Alignment.centerLeft,
                      child: Container(
                        height: 18,
                        width: maxBucket == 0 ? 0 : c.maxWidth * bucketCounts[i] / maxBucket,
                        decoration: BoxDecoration(
                          color: speedColor(
                              i == 0 ? 1 : buckets[i] + 1, limit: _limit),
                          borderRadius: BorderRadius.circular(4),
                        ),
                      ),
                    ),
                  ),
                ),
                SizedBox(width: 44, child: Text('${bucketCounts[i]}', textAlign: TextAlign.right)),
              ],
            ),
          ),
        const SizedBox(height: 16),
        Text(
          'Statistiken bygger på passagerna som har hämtats till telefonen. '
          'Tryck "Hämta nya" under Passager för att uppdatera.',
          style: theme.textTheme.bodySmall,
        ),
      ],
    );
  }
}
