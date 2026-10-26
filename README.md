# Radarlogger-appen

Android-app (Flutter) för den fristående radarloggern (HLK-LD2451 + ESP32-S3).

- **Live**: hastighet, avstånd, riktning
- **Passager**: hämtar loggen via Bluetooth, sparar i telefonen, exporterar CSV
- **Statistik**: antal, högsta, snitt, v85, andel över gränsen, per timme
- **Inställningar**: riktning, räckvidd, gränser, rensa loggen

## Installera
Öppna fliken **Releases**, ladda ner senaste `radarlogger.apk` och öppna den på telefonen.

Bluetooth-PIN vid parkoppling: `123456`.

## Bygge
GitHub Actions bygger appen automatiskt vid varje ändring (`.github/workflows/build.yml`).

## Firmware
- `firmware/Radarlogger/` – koden för radarloggern (ESP32-S3 + LD2451)
- `firmware/RadarSimulator/` – låtsasradar för test på ESP nummer två
- `firmware/bin/` – färdiga filer:
  - `radarlogger-x.y.z.bin` – hel installation via USB (ESPHome Web eller esptool-js, adress `0x0`). Raderar loggen.
  - `radarlogger-x.y.z-ota.bin` – uppdatering via WiFi på `http://192.168.4.1`. Loggen finns kvar.
