// Lokal kopia av loggen i telefonen, en fil per radarlogger.

import 'dart:io';
import 'dart:typed_data';

import 'package:path_provider/path_provider.dart';

import 'protocol.dart';

class LocalLog {
  LocalLog(this.deviceId);

  final String deviceId;
  final List<PassRecord> records = [];
  File? _file;

  int get count => records.length;

  Future<File> _getFile() async {
    if (_file != null) return _file!;
    final dir = await getApplicationDocumentsDirectory();
    final safe = deviceId.replaceAll(RegExp(r'[^A-Za-z0-9]'), '_');
    _file = File('${dir.path}/radarlogg_$safe.bin');
    return _file!;
  }

  Future<void> load() async {
    records.clear();
    final f = await _getFile();
    if (!await f.exists()) return;
    final bytes = await f.readAsBytes();
    final b = ByteData.sublistView(bytes);
    final n = bytes.length ~/ PassRecord.size;
    for (var i = 0; i < n; i++) {
      records.add(PassRecord.read(b, i * PassRecord.size));
    }
  }

  Future<void> append(List<PassRecord> newRecords) async {
    if (newRecords.isEmpty) return;
    final data = ByteData(newRecords.length * PassRecord.size);
    for (var i = 0; i < newRecords.length; i++) {
      newRecords[i].write(data, i * PassRecord.size);
    }
    final f = await _getFile();
    await f.writeAsBytes(data.buffer.asUint8List(), mode: FileMode.append, flush: true);
    records.addAll(newRecords);
  }

  /// Sparar den gamla kopian under ett nytt namn och börjar om.
  /// Används när loggen i radarn har rensats.
  Future<void> archiveAndReset() async {
    final f = await _getFile();
    if (await f.exists()) {
      final stamp = DateTime.now().millisecondsSinceEpoch;
      await f.rename(f.path.replaceAll('.bin', '_arkiv_$stamp.bin'));
    }
    records.clear();
  }

  Future<void> deleteLocal() async {
    final f = await _getFile();
    if (await f.exists()) await f.delete();
    records.clear();
  }

  /// Skapar en CSV-fil (semikolon, för svensk Excel) och returnerar sökvägen.
  Future<String> exportCsv() async {
    final sb = StringBuffer('﻿nr;datum;tid;hastighet_kmh;avstand_m;riktning\r\n');
    for (var i = 0; i < records.length; i++) {
      final r = records[i];
      final dt = r.dateTime;
      final date = dt != null ? formatDate(dt) : 'okand';
      final time = dt != null ? formatTime(dt) : '';
      sb.write('${i + 1};$date;$time;${r.speed};${r.distance};${r.approaching ? 'mot' : 'fran'}\r\n');
    }
    final dir = await getTemporaryDirectory();
    final now = DateTime.now();
    final f = File('${dir.path}/radarlogg_${formatDate(now)}.csv');
    await f.writeAsString(sb.toString(), flush: true);
    return f.path;
  }
}
