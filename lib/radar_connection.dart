// Håller anslutningen till en Radarlogger och allt som händer över Bluetooth.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'local_log.dart';
import 'protocol.dart';

class RadarConnection extends ChangeNotifier {
  RadarConnection(this.device) : log = LocalLog(device.remoteId.str);

  final BluetoothDevice device;
  final LocalLog log;

  BluetoothConnectionState state = BluetoothConnectionState.disconnected;
  bool connecting = false;
  bool ready = false;
  String? error;
  String? info;

  LivePacket? live;
  RadarSettings? settings;

  // Nedladdning
  bool downloading = false;
  int downloadFrom = 0;
  int downloadTarget = 0;
  int downloadDone = 0;
  final List<PassRecord> _pending = [];
  int _expectedIndex = 0;
  Completer<void>? _downloadCompleter;
  Future<void> _writeChain = Future.value();
  Timer? _downloadWatchdog;

  BluetoothCharacteristic? _cLive, _cInfo, _cSettings, _cTime, _cLogCtrl, _cLogData;
  StreamSubscription? _connSub, _liveSub, _dataSub;
  bool _disposed = false;

  String get name =>
      device.platformName.isNotEmpty ? device.platformName : device.remoteId.str;

  bool get connected => state == BluetoothConnectionState.connected && ready;

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  // ------------------------------------------------------------
  //  Anslut
  // ------------------------------------------------------------
  Future<void> connect() async {
    if (connecting) return;
    connecting = true;
    ready = false;
    error = null;
    _notify();

    await log.load();

    _connSub ??= device.connectionState.listen((s) {
      state = s;
      if (s == BluetoothConnectionState.disconnected) {
        ready = false;
        if (downloading) _finishDownload(success: false);
      }
      _notify();
    });

    try {
      await device.connect(timeout: const Duration(seconds: 15), autoConnect: false);

      if (Platform.isAndroid) {
        try {
          await device.requestMtu(247);
        } catch (_) {}
      }

      final services = await device.discoverServices();
      final svc = services.firstWhere(
        (s) => s.uuid == Guid(RadarUuids.service),
        orElse: () => throw Exception('Hittar inte Radarlogger-tjänsten'),
      );
      BluetoothCharacteristic ch(String uuid) => svc.characteristics.firstWhere(
            (c) => c.uuid == Guid(uuid),
            orElse: () => throw Exception('Saknar egenskap $uuid'),
          );
      _cLive = ch(RadarUuids.live);
      _cInfo = ch(RadarUuids.info);
      _cSettings = ch(RadarUuids.settings);
      _cTime = ch(RadarUuids.time);
      _cLogCtrl = ch(RadarUuids.logCtrl);
      _cLogData = ch(RadarUuids.logData);

      await _liveSub?.cancel();
      _liveSub = _cLive!.onValueReceived.listen(_onLive);
      await _cLive!.setNotifyValue(true);
      try {
        _onLive(await _cLive!.read());
      } catch (_) {}

      await _dataSub?.cancel();
      _dataSub = _cLogData!.onValueReceived.listen(_onLogData);
      await _cLogData!.setNotifyValue(true);

      try {
        info = String.fromCharCodes(await _cInfo!.read());
      } catch (_) {}

      ready = true;
      connecting = false;
      _notify();

      // Parkoppling (PIN 123456) behövs för att skriva. Görs efter att live-data syns.
      await _ensureBonded();
      await syncTime();
      await readSettings();
    } catch (e) {
      error = 'Kunde inte ansluta: $e';
      connecting = false;
      _notify();
    }
  }

  Future<void> _ensureBonded() async {
    if (!Platform.isAndroid) return;
    try {
      final bs = await device.bondState.first;
      if (bs != BluetoothBondState.bonded) {
        await device.createBond(timeout: 60);
      }
    } catch (_) {
      // Om användaren avbryter fungerar läsning ändå.
    }
  }

  Future<void> disconnect() async {
    try {
      await device.disconnect();
    } catch (_) {}
  }

  // ------------------------------------------------------------
  //  Live
  // ------------------------------------------------------------
  void _onLive(List<int> v) {
    final p = LivePacket.parse(v);
    if (p != null) {
      live = p;
      _notify();
    }
  }

  // ------------------------------------------------------------
  //  Klocka och inställningar
  // ------------------------------------------------------------
  Future<bool> syncTime() async {
    if (_cTime == null) return false;
    try {
      final now = DateTime.now().millisecondsSinceEpoch ~/ 1000;
      await _cTime!.write(uint32le(now), withoutResponse: false);
      return true;
    } catch (e) {
      error = 'Kunde inte ställa klockan (parkoppla med PIN 123456)';
      _notify();
      return false;
    }
  }

  Future<void> readSettings() async {
    if (_cSettings == null) return;
    try {
      settings = RadarSettings.parse(await _cSettings!.read());
      _notify();
    } catch (_) {}
  }

  Future<bool> writeSettings(RadarSettings s) async {
    if (_cSettings == null) return false;
    try {
      await _ensureBonded();
      await _cSettings!.write(s.toBytes(), withoutResponse: false);
      await Future.delayed(const Duration(milliseconds: 800));
      await readSettings();
      return true;
    } catch (e) {
      error = 'Kunde inte spara: $e';
      _notify();
      return false;
    }
  }

  Future<bool> clearDeviceLog() async {
    if (_cLogCtrl == null) return false;
    try {
      await _ensureBonded();
      await _cLogCtrl!.write([0x02, 0x43, 0x4C, 0x52], withoutResponse: false); // 02 'C' 'L' 'R'
      await log.archiveAndReset();
      _notify();
      return true;
    } catch (e) {
      error = 'Kunde inte rensa: $e';
      _notify();
      return false;
    }
  }

  // ------------------------------------------------------------
  //  Hämta loggen
  // ------------------------------------------------------------
  Future<String> downloadNew() async {
    if (downloading || _cLogCtrl == null) return 'Upptagen';
    final deviceCount = live?.recordCount ?? 0;

    // Radarns logg har rensats: spara den gamla kopian och börja om
    if (deviceCount < log.count) {
      await log.archiveAndReset();
    }

    if (deviceCount == log.count) return 'Inga nya passager';

    downloading = true;
    downloadFrom = log.count;
    downloadTarget = deviceCount;
    downloadDone = 0;
    _expectedIndex = downloadFrom;
    _pending.clear();
    _downloadCompleter = Completer<void>();
    _notify();

    try {
      await _ensureBonded();
      await _cLogCtrl!.write([0x01, ...uint32le(downloadFrom)], withoutResponse: false);
      _kickWatchdog();
      await _downloadCompleter!.future;
    } catch (e) {
      await _finishDownload(success: false);
      return 'Fel vid hämtning: $e';
    }
    final got = downloadDone;
    return got > 0 ? '$got nya passager hämtade' : 'Inga nya passager';
  }

  void _kickWatchdog() {
    _downloadWatchdog?.cancel();
    _downloadWatchdog = Timer(const Duration(seconds: 6), () {
      if (downloading) _finishDownload(success: false);
    });
  }

  void _onLogData(List<int> v) {
    if (!downloading || v.length < 5) return;
    final start = readUint32le(v, 0);
    final n = v[4];

    if (start == 0xFFFFFFFF) {
      _finishDownload(success: true);
      return;
    }
    if (start != _expectedIndex) return; // dubblett eller hopp, ignoreras

    final b = ByteData.sublistView(Uint8List.fromList(v));
    for (var i = 0; i < n; i++) {
      final off = 5 + i * PassRecord.size;
      if (off + PassRecord.size > v.length) break;
      _pending.add(PassRecord.read(b, off));
    }
    _expectedIndex += n;
    downloadDone += n;
    _kickWatchdog();

    if (_pending.length >= 500) {
      final chunk = List<PassRecord>.from(_pending);
      _pending.clear();
      _writeChain = _writeChain.then((_) => log.append(chunk));
    }
    _notify();
  }

  Future<void> _finishDownload({required bool success}) async {
    _downloadWatchdog?.cancel();
    if (_pending.isNotEmpty) {
      final chunk = List<PassRecord>.from(_pending);
      _pending.clear();
      _writeChain = _writeChain.then((_) => log.append(chunk));
    }
    await _writeChain;
    downloading = false;
    if (!success) error = 'Hämtningen avbröts. Försök igen.';
    final c = _downloadCompleter;
    _downloadCompleter = null;
    if (c != null && !c.isCompleted) c.complete();
    _notify();
  }

  @override
  void dispose() {
    _disposed = true;
    _downloadWatchdog?.cancel();
    _connSub?.cancel();
    _liveSub?.cancel();
    _dataSub?.cancel();
    device.disconnect().catchError((_) {});
    super.dispose();
  }
}
