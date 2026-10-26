import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../protocol.dart';
import 'device_screen.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key});

  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<ScanScreen> {
  List<ScanResult> _results = [];
  bool _scanning = false;
  String? _message;
  StreamSubscription<List<ScanResult>>? _resSub;
  StreamSubscription<bool>? _scanSub;

  @override
  void initState() {
    super.initState();
    _resSub = FlutterBluePlus.scanResults.listen((list) {
      if (!mounted) return;
      setState(() => _results = list.where(_isRadar).toList()
        ..sort((a, b) => b.rssi.compareTo(a.rssi)));
    });
    _scanSub = FlutterBluePlus.isScanning.listen((s) {
      if (mounted) setState(() => _scanning = s);
    });
    _scan();
  }

  @override
  void dispose() {
    _resSub?.cancel();
    _scanSub?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  bool _isRadar(ScanResult r) {
    final name = r.advertisementData.advName.isNotEmpty
        ? r.advertisementData.advName
        : r.device.platformName;
    return name.startsWith('Radarlogger') ||
        r.advertisementData.serviceUuids.contains(Guid(RadarUuids.service));
  }

  String _nameOf(ScanResult r) {
    if (r.advertisementData.advName.isNotEmpty) return r.advertisementData.advName;
    if (r.device.platformName.isNotEmpty) return r.device.platformName;
    return r.device.remoteId.str;
  }

  Future<void> _scan() async {
    setState(() => _message = null);
    try {
      if (await FlutterBluePlus.isSupported == false) {
        setState(() => _message = 'Telefonen saknar Bluetooth LE');
        return;
      }
      if (Platform.isAndroid &&
          FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
        try {
          await FlutterBluePlus.turnOn();
        } catch (_) {}
      }
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 10));
    } catch (e) {
      setState(() => _message = 'Kunde inte söka: $e');
    }
  }

  Future<void> _open(ScanResult r) async {
    await FlutterBluePlus.stopScan();
    if (!mounted) return;
    await Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => DeviceScreen(device: r.device, title: _nameOf(r)),
    ));
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(title: const Text('Radarlogger')),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _scanning ? FlutterBluePlus.stopScan : _scan,
        icon: Icon(_scanning ? Icons.stop : Icons.search),
        label: Text(_scanning ? 'Sluta söka' : 'Sök'),
      ),
      body: Column(
        children: [
          if (_scanning) const LinearProgressIndicator(),
          if (_message != null)
            Padding(
              padding: const EdgeInsets.all(16),
              child: Text(_message!, style: TextStyle(color: theme.colorScheme.error)),
            ),
          Expanded(
            child: _results.isEmpty
                ? Center(
                    child: Padding(
                      padding: const EdgeInsets.all(32),
                      child: Text(
                        _scanning
                            ? 'Söker efter radarloggrar...'
                            : 'Ingen radarlogger hittades.\n\nKontrollera att den har ström och att Bluetooth är på, och tryck Sök.',
                        textAlign: TextAlign.center,
                        style: theme.textTheme.bodyLarge,
                      ),
                    ),
                  )
                : ListView.builder(
                    itemCount: _results.length,
                    itemBuilder: (_, i) {
                      final r = _results[i];
                      return ListTile(
                        leading: const Icon(Icons.radar, size: 36),
                        title: Text(_nameOf(r)),
                        subtitle: Text('Signal: ${r.rssi} dBm'),
                        trailing: const Icon(Icons.chevron_right),
                        onTap: () => _open(r),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}
