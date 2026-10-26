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
