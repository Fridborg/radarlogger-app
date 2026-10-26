import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../radar_connection.dart';
import 'live_tab.dart';
import 'log_tab.dart';
import 'settings_tab.dart';
import 'stats_tab.dart';

class DeviceScreen extends StatefulWidget {
  const DeviceScreen({super.key, required this.device, required this.title});

  final BluetoothDevice device;
  final String title;

  @override
  State<DeviceScreen> createState() => _DeviceScreenState();
}

class _DeviceScreenState extends State<DeviceScreen> {
  late final RadarConnection conn;
  int _tab = 0;

  @override
  void initState() {
    super.initState();
    conn = RadarConnection(widget.device);
    conn.connect();
  }

  @override
  void dispose() {
    conn.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: conn,
      builder: (context, _) {
        final theme = Theme.of(context);
        Widget body;
        if (!conn.connected) {
          body = Center(
            child: Padding(
              padding: const EdgeInsets.all(32),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  if (conn.connecting) ...[
                    const CircularProgressIndicator(),
                    const SizedBox(height: 16),
                    const Text('Ansluter...'),
                  ] else ...[
                    Icon(Icons.bluetooth_disabled, size: 48, color: theme.colorScheme.outline),
                    const SizedBox(height: 12),
                    Text(conn.error ?? 'Inte ansluten', textAlign: TextAlign.center),
                    const SizedBox(height: 16),
                    FilledButton.icon(
                      onPressed: conn.connect,
                      icon: const Icon(Icons.refresh),
                      label: const Text('Anslut igen'),
                    ),
                  ],
                  if (conn.log.count > 0) ...[
                    const SizedBox(height: 24),
                    Text('${conn.log.count} passager finns sparade i telefonen.',
                        style: theme.textTheme.bodySmall),
                    TextButton(
                      onPressed: () => setState(() => _tab = 2),
                      child: const Text('Visa statistik utan anslutning'),
                    ),
                  ],
                ],
              ),
            ),
          );
          // Statistik och passager kan visas från telefonens kopia även utan anslutning
          if (_tab == 1) body = LogTab(conn: conn);
          if (_tab == 2) body = StatsTab(conn: conn);
        } else {
          body = IndexedStack(
            index: _tab,
            children: [
              LiveTab(conn: conn),
              LogTab(conn: conn),
              StatsTab(conn: conn),
              SettingsTab(conn: conn),
            ],
          );
        }

        return Scaffold(
          appBar: AppBar(
            title: Text(widget.title),
            actions: [
              Padding(
                padding: const EdgeInsets.only(right: 16),
                child: Icon(
                  conn.connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
                  color: conn.connected ? Colors.green : theme.colorScheme.outline,
                ),
              ),
            ],
          ),
          body: SafeArea(child: body),
          bottomNavigationBar: NavigationBar(
            selectedIndex: _tab,
            onDestinationSelected: (i) => setState(() => _tab = i),
            destinations: const [
              NavigationDestination(icon: Icon(Icons.speed), label: 'Live'),
              NavigationDestination(icon: Icon(Icons.list_alt), label: 'Passager'),
              NavigationDestination(icon: Icon(Icons.bar_chart), label: 'Statistik'),
              NavigationDestination(icon: Icon(Icons.tune), label: 'Inställningar'),
            ],
          ),
        );
      },
    );
  }
}
