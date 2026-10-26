// Bluetooth-protokollet mellan appen och Radarlogger-ESP:n.
// Måste stämma med Radarlogger.ino.

import 'dart:typed_data';

class RadarUuids {
  static const service  = 'c0de0001-5ad1-4b2a-9f00-7261646172a1';
  static const live     = 'c0de0002-5ad1-4b2a-9f00-7261646172a1';
  static const info     = 'c0de0003-5ad1-4b2a-9f00-7261646172a1';
  static const settings = 'c0de0004-5ad1-4b2a-9f00-7261646172a1';
  static const time     = 'c0de0005-5ad1-4b2a-9f00-7261646172a1';
  static const logCtrl  = 'c0de0006-5ad1-4b2a-9f00-7261646172a1';
  static const logData  = 'c0de0007-5ad1-4b2a-9f00-7261646172a1';
}

/// Live-paket, 16 byte.
class LivePacket {
  final int speed;
  final int distance;
  final int angle;
  final bool approaching;
  final int targets;
  final int flags;
  final int lastPassSpeed;
  final int recordCount;
  final int now;

  const LivePacket({
    required this.speed,
    required this.distance,
    required this.angle,
    required this.approaching,
    required this.targets,
    required this.flags,
    required this.lastPassSpeed,
    required this.recordCount,
    required this.now,
  });

  bool get timeSynced => flags & 0x01 != 0;
  bool get logFull => flags & 0x02 != 0;
  bool get radarOk => flags & 0x04 != 0;
  bool get simulator => flags & 0x08 != 0;

  static LivePacket? parse(List<int> value) {
    if (value.length < 16) return null;
    final b = ByteData.sublistView(Uint8List.fromList(value));
    return LivePacket(
      speed: b.getUint8(1),
      distance: b.getUint8(2),
      angle: b.getInt8(3),
      approaching: b.getUint8(4) == 1,
      targets: b.getUint8(5),
      flags: b.getUint8(6),
      lastPassSpeed: b.getUint8(7),
      recordCount: b.getUint32(8, Endian.little),
      now: b.getUint32(12, Endian.little),
    );
  }
}

/// En sparad passage, 8 byte.
class PassRecord {
  static const size = 8;

  final int time;      // unix-sekunder (UTC), eller sekunder sedan start om tiden är okänd
  final int speed;     // km/h
  final int distance;  // m
  final bool approaching;
  final int flags;

  const PassRecord({
    required this.time,
    required this.speed,
    required this.distance,
    required this.approaching,
    required this.flags,
  });

  bool get timeKnown => flags & 0x01 == 0;

  DateTime? get dateTime =>
      timeKnown ? DateTime.fromMillisecondsSinceEpoch(time * 1000) : null;

  static PassRecord read(ByteData b, int offset) => PassRecord(
        time: b.getUint32(offset, Endian.little),
        speed: b.getUint8(offset + 4),
        distance: b.getUint8(offset + 5),
        approaching: b.getUint8(offset + 6) == 1,
        flags: b.getUint8(offset + 7),
      );

  void write(ByteData b, int offset) {
    b.setUint32(offset, time, Endian.little);
    b.setUint8(offset + 4, speed);
    b.setUint8(offset + 5, distance);
    b.setUint8(offset + 6, approaching ? 1 : 0);
    b.setUint8(offset + 7, flags);
  }
}

/// Inställningar, 8 byte.
class RadarSettings {
  int maxDistance;   // 10–100 m
  int direction;     // 0 = från, 1 = mot, 2 = båda
  int radarMinSpeed; // 0–120 km/h
  int radarDelay;    // 1–30 s
  int trigCount;     // 0 = ändra inte
  int snr;           // 255 = ändra inte
  int gapDecis;      // tiondels sekunder
  int logMinSpeed;   // km/h

  RadarSettings({
    required this.maxDistance,
    required this.direction,
    required this.radarMinSpeed,
    required this.radarDelay,
    required this.trigCount,
    required this.snr,
    required this.gapDecis,
    required this.logMinSpeed,
  });

  static RadarSettings? parse(List<int> v) {
    if (v.length < 8) return null;
    return RadarSettings(
      maxDistance: v[0],
      direction: v[1],
      radarMinSpeed: v[2],
      radarDelay: v[3],
      trigCount: v[4],
      snr: v[5],
      gapDecis: v[6],
      logMinSpeed: v[7],
    );
  }

  List<int> toBytes() => [
        maxDistance.clamp(10, 100),
        direction.clamp(0, 2),
        radarMinSpeed.clamp(0, 120),
        radarDelay.clamp(1, 30),
        trigCount.clamp(0, 10),
        snr.clamp(0, 255),
        gapDecis.clamp(3, 250),
        logMinSpeed.clamp(0, 255),
      ];

  RadarSettings copy() => RadarSettings(
        maxDistance: maxDistance,
        direction: direction,
        radarMinSpeed: radarMinSpeed,
        radarDelay: radarDelay,
        trigCount: trigCount,
        snr: snr,
        gapDecis: gapDecis,
        logMinSpeed: logMinSpeed,
      );
}

List<int> uint32le(int v) => [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF];

int readUint32le(List<int> v, int o) =>
    v[o] | (v[o + 1] << 8) | (v[o + 2] << 16) | (v[o + 3] << 24);

String two(int n) => n.toString().padLeft(2, '0');

String formatDate(DateTime d) => '${d.year}-${two(d.month)}-${two(d.day)}';

String formatTime(DateTime d) => '${two(d.hour)}:${two(d.minute)}:${two(d.second)}';
