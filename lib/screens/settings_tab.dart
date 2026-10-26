import 'package:flutter/material.dart';

import '../protocol.dart';
import '../radar_connection.dart';

class SettingsTab extends StatefulWidget {
  const SettingsTab({super.key, required this.conn});

  final RadarConnection conn;

  @override
  State<SettingsTab> createState() => _SettingsTabState();
}

class _SettingsTabState extends State<SettingsTab> {
  RadarSettings? _edit;
  bool _saving = false;

  RadarSettings? get _current => _edit ?? widget.conn.settings?.copy();

  Future<void> _save() async {
    final s = _current;
    if (s == null) return;
    setState(() => _saving = true);
    final ok = await widget.conn.writeSettings(s);
    if (!mounted) return;
    setState(() {
      _saving = false;
      if (ok) _edit = null;
    });
    ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(ok ? 'Inställningarna är sparade' : 'Kunde inte spara')));
  }

  Future<void> _clearDevice() async {
    final yes = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Rensa loggen i radarn?'),
        content: const Text(
            'Alla passager i radarloggern raderas. Telefonens kopia sparas som arkiv. Det går inte att ångra.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Avbryt')),
          FilledButton(onPressed: () => Navigator.pop(c, true), child: const Text('Rensa')),
        ],
      ),
    );
    if (yes != true) return;
    final ok = await widget.conn.clearDeviceLog();
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(ok ? 'Loggen i radarn är rensad' : 'Kunde inte rensa')));
    }
  }

  Future<void> _deleteLocal() async {
    final yes = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Radera telefonens kopia?'),
        content: const Text('Passagerna i radarn påverkas inte och kan hämtas igen.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Avbryt')),
          FilledButton(onPressed: () => Navigator.pop(c, true), child: const Text('Radera')),
        ],
      ),
    );
    if (yes != true) return;
    await widget.conn.log.deleteLocal();
    setState(() {});
  }

  Widget _number(String label, String help, int value, int min, int max,
      void Function(int) onChanged, {int step = 1}) {
    return ListTile(
      title: Text(label),
      subtitle: Text(help),
      trailing: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          IconButton(
            icon: const Icon(Icons.remove_circle_outline),
            onPressed: value > min ? () => onChanged((value - step).clamp(min, max)) : null,
          ),
          SizedBox(
            width: 40,
            child: Text('$value', textAlign: TextAlign.center,
                style: const TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
          ),
          IconButton(
            icon: const Icon(Icons.add_circle_outline),
            onPressed: value < max ? () => onChanged((value + step).clamp(min, max)) : null,
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final s = _current;

    if (s == null) {
      return Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text('Inställningarna har inte lästs in ännu.'),
            const SizedBox(height: 12),
            OutlinedButton(onPressed: widget.conn.readSettings, child: const Text('Läs in')),
          ],
        ),
      );
    }

    void set(void Function(RadarSettings) f) => setState(() {
          _edit ??= s;
          f(_edit!);
        });

    return ListView(
      padding: const EdgeInsets.symmetric(vertical: 8),
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: Text('Radarn', style: theme.textTheme.titleSmall),
        ),
        ListTile(
          title: const Text('Riktning'),
          subtitle: const Text('Vilka fordon radarn ska rapportera'),
          trailing: DropdownButton<int>(
            value: s.direction,
            items: const [
              DropdownMenuItem(value: 1, child: Text('Mot radarn')),
              DropdownMenuItem(value: 0, child: Text('Från radarn')),
              DropdownMenuItem(value: 2, child: Text('Båda')),
            ],
            onChanged: (v) => set((e) => e.direction = v ?? 1),
          ),
        ),
        _number('Max avstånd (m)', 'Hur långt bort fordon upptäcks', s.maxDistance, 10, 100,
            (v) => set((e) => e.maxDistance = v), step: 5),
        _number('Radarns min-hastighet', 'Långsammare mål ignoreras (km/h)', s.radarMinSpeed, 0,
            120, (v) => set((e) => e.radarMinSpeed = v)),
        const Divider(),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: Text('Loggen', style: theme.textTheme.titleSmall),
        ),
        if (s.hasReportDistance)
          _number(
              'Rapportavstånd (m)',
              s.reportDistance == 0
                  ? 'Av: högsta hastigheten sparas'
                  : 'Hastigheten sparas när bilen passerar ${s.reportDistance} m',
              s.reportDistance,
              0,
              100,
              (v) => set((e) => e.reportDistance = v),
              step: 5),
        _number('Spara passager över', 'Långsammare passager sparas inte (km/h)', s.logMinSpeed,
            0, 200, (v) => set((e) => e.logMinSpeed = v)),
        _number('Paus innan passage avslutas', 'Tiondels sekunder (15 = 1,5 s)', s.gapDecis, 3,
            100, (v) => set((e) => e.gapDecis = v)),
        Padding(
          padding: const EdgeInsets.all(16),
          child: FilledButton.icon(
            onPressed: _edit == null || _saving ? null : _save,
            icon: _saving
                ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.save),
            label: const Text('Spara inställningar'),
          ),
        ),
        const Divider(),
        ListTile(
          leading: const Icon(Icons.schedule),
          title: const Text('Ställ radarns klocka'),
          subtitle: const Text('Görs automatiskt vid varje anslutning'),
          onTap: () async {
            final ok = await widget.conn.syncTime();
            if (context.mounted) {
              ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(content: Text(ok ? 'Klockan är ställd' : 'Kunde inte ställa klockan')));
            }
          },
        ),
        ListTile(
          leading: Icon(Icons.delete_forever, color: theme.colorScheme.error),
          title: Text('Rensa loggen i radarn', style: TextStyle(color: theme.colorScheme.error)),
          onTap: _clearDevice,
        ),
        ListTile(
          leading: const Icon(Icons.phone_android),
          title: const Text('Radera telefonens kopia'),
          subtitle: Text('${widget.conn.log.count} passager'),
          onTap: _deleteLocal,
        ),
        if (widget.conn.info != null)
          Padding(
            padding: const EdgeInsets.all(16),
            child: Text('Radarlogger: ${widget.conn.info}', style: theme.textTheme.bodySmall),
          ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16),
          child: Text('Bluetooth-PIN vid parkoppling: 123456', style: theme.textTheme.bodySmall),
        ),
      ],
    );
  }
}
