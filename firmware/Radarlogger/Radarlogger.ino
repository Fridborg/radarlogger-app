/*
 * ============================================================
 *  RADARLOGGER  –  HLK-LD2451 + ESP32-S3 + Bluetooth + WiFi
 *  Version 0.2.1
 * ============================================================
 *
 *  Fristående hastighetslogger och fordonsräknare.
 *  Ingen Home Assistant, ingen server, inget internet.
 *
 *  - Läser LD2451-radarn via UART
 *  - Räknar fordon och sparar varje passage i flashminnet:
 *      tid, högsta hastighet, avstånd, riktning
 *  - Mobilappen ansluter via Bluetooth för att:
 *      se live-hastighet, hämta loggen, ställa in radarn,
 *      ställa klockan och rensa loggen
 *  - ESP:n startar också ett eget WiFi-nät med en webbsida:
 *      anslut till "Radarlogger-XXXX" (lösenord nedan)
 *      och öppna http://192.168.4.1 i mobilen
 *
 *  KORT:     ESP32-S3-DevKitC-1U (N16R8)
 *  BIBLIOTEK: NimBLE-Arduino 2.x (installera via Library Manager)
 *  CORE:     esp32 by Espressif 3.x
 *
 *  KOPPLING (LD2451 -> ESP32-S3):
 *      LD2451 5V   -> 5V  (5VIN)
 *      LD2451 GND  -> GND
 *      LD2451 TX   -> GPIO 18   (ESP tar emot)
 *      LD2451 RX   -> GPIO 17   (ESP skickar)
 *
 *  Arduino IDE-inställningar (Verktyg-menyn):
 *      Board:            ESP32S3 Dev Module
 *      Flash Size:       16MB
 *      PSRAM:            OPI PSRAM
 *      Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
 *      USB CDC On Boot:  Disabled  (använd porten märkt "UART")
 * ============================================================
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <sys/time.h>
#include <time.h>

// ------------------------------------------------------------
//  INSTÄLLNINGAR SOM DU KAN ÄNDRA
// ------------------------------------------------------------
#define FW_VERSION      "0.2.1"
#define DEVICE_PREFIX   "Radarlogger"   // Bluetooth-namn, följt av 4 tecken från MAC
#define BLE_PASSKEY     123456          // 6-siffrig PIN vid parkoppling

#define WIFI_ENABLED    1               // 1 = eget WiFi-nät med webbsida
#define WIFI_PASSWORD   "radar1234"     // minst 8 tecken

#define RADAR_RX_PIN    18              // ESP RX  <- radar TX
#define RADAR_TX_PIN    17              // ESP TX  -> radar RX
#define RADAR_BAUD      115200

#define LED_PIN         48              // RGB-LED på DevKitC-1 (v1.1 av kortet: 38)

// 1 = skriv ut radarns rådata i Serial Monitor (bra vid första testet)
#define DEBUG_RAW       1

// 1 = ingen radar behövs, ESP:n hittar på bilar (för att testa appen)
#define SIMULATOR       0

// Vilket värde i radarns riktningsbyte betyder "kommer MOT radarn"?
// Hi-Links manual visar 1 = mot. Ändra till 0 bara om testet visar motsatsen.
#define TARGET_DIR_APPROACH 1

// ------------------------------------------------------------
//  BLUETOOTH-ID:N (UUID)  – appen använder samma värden
// ------------------------------------------------------------
#define SERVICE_UUID    "c0de0001-5ad1-4b2a-9f00-7261646172a1"
#define LIVE_UUID       "c0de0002-5ad1-4b2a-9f00-7261646172a1"  // read + notify
#define INFO_UUID       "c0de0003-5ad1-4b2a-9f00-7261646172a1"  // read
#define SETTINGS_UUID   "c0de0004-5ad1-4b2a-9f00-7261646172a1"  // read + write (krypterad)
#define TIME_UUID       "c0de0005-5ad1-4b2a-9f00-7261646172a1"  // write (krypterad)
#define LOGCTRL_UUID    "c0de0006-5ad1-4b2a-9f00-7261646172a1"  // write (krypterad)
#define LOGDATA_UUID    "c0de0007-5ad1-4b2a-9f00-7261646172a1"  // notify

// ------------------------------------------------------------
//  DATASTRUKTURER
// ------------------------------------------------------------
#define LOG_FILE        "/passes.bin"
#define FLAG_UNSYNCED   0x01            // tiden är sekunder sedan start, inte riktig tid

struct __attribute__((packed)) PassRecord {   // 8 byte per passage
  uint32_t time;        // unix-tid (UTC), eller sekunder sedan start om FLAG_UNSYNCED
  uint8_t  speed;       // högsta hastighet km/h
  uint8_t  distance;    // avstånd (m) där högsta hastigheten mättes
  uint8_t  dir;         // 1 = mot radarn, 0 = från radarn
  uint8_t  flags;
};

struct __attribute__((packed)) Settings {      // 8 byte
  uint8_t maxDistance;   // 10–100 m
  uint8_t direction;     // 0 = bara från, 1 = bara mot, 2 = båda
  uint8_t radarMinSpeed; // 0–120 km/h, radarn ignorerar långsammare mål
  uint8_t radarDelay;    // 1–30 s (radarns larmfördröjning)
  uint8_t trigCount;     // 1–10, 0 = ändra inte
  uint8_t snr;           // 3–8 (0 = radarns standard 4), 255 = ändra inte
  uint8_t gapDecis;      // tid utan mål innan passagen avslutas, i tiondels sekunder
  uint8_t logMinSpeed;   // passager under denna hastighet sparas inte
};

struct __attribute__((packed)) LivePacket {    // 16 byte
  uint8_t  version;      // = 1
  uint8_t  speed;        // aktuell hastighet km/h (0 = inget fordon)
  uint8_t  distance;     // m
  int8_t   angle;        // grader
  uint8_t  dir;          // 1 = mot, 0 = från
  uint8_t  targets;      // antal mål i senaste bilden
  uint8_t  flags;        // bit0 tid synkad, bit1 loggen full, bit2 radar svarar, bit3 simulator
  uint8_t  lastPassSpeed;
  uint32_t recordCount;
  uint32_t now;          // unix-tid, eller sekunder sedan start
};

struct Target {
  int8_t  angle;
  uint8_t distance;
  uint8_t dir;
  uint8_t speed;
  uint8_t snr;
};

// ------------------------------------------------------------
//  GLOBALA VARIABLER
// ------------------------------------------------------------
// Samma som radarns fabriksinställning: 100 m, mot radarn, 5 km/h, 2 s fördröjning
static const Settings DEFAULT_SETTINGS = {100, 1, 5, 2, 0, 255, 15, 5};
Settings settings;
Preferences prefs;

NimBLEServer*         bleServer   = nullptr;
NimBLECharacteristic* chrLive     = nullptr;
NimBLECharacteristic* chrInfo     = nullptr;
NimBLECharacteristic* chrSettings = nullptr;
NimBLECharacteristic* chrLogData  = nullptr;

volatile bool     bleConnected  = false;
volatile uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
volatile uint16_t bleMtu        = 23;

// Begäran från Bluetooth-callbacks, hanteras i loop()
volatile bool     reqApplySettings = false;
volatile bool     reqSensitivity   = false;
volatile bool     reqTimeSync      = false;
volatile uint32_t reqTimeValue     = 0;
volatile bool     reqClearLog      = false;
volatile bool     reqDownload      = false;
volatile uint32_t reqDownloadFrom  = 0;
volatile bool     reqAbort         = false;

WebServer web(80);
char     deviceName[32]  = "";
int16_t  tzOffsetMin     = 60;   // tidszon i minuter (sätts av webbsidan)

// Logg
uint32_t recordCount     = 0;
uint32_t maxRecords      = 0;
uint32_t bootFirstIndex  = 0;    // första posten som sparades efter denna start
bool     timeSynced      = false;
bool     logFull         = false;

// Nedladdning
bool     downloading     = false;
uint32_t downloadIndex   = 0;
uint32_t lastDownloadMs  = 0;

// Live
Target   lastPrimary     = {0, 0, 0, 0, 0};
uint8_t  lastTargets     = 0;
uint32_t lastFrameMs     = 0;
uint32_t lastAckMs       = 0;    // senaste OK-svar från radarn
uint32_t lastHealthMs    = 0;
uint8_t  lastPassSpeed   = 0;
uint32_t lastLiveNotify  = 0;

// Pågående passage
bool     passActive      = false;
uint32_t passLastSeenMs  = 0;
uint8_t  passMaxSpeed    = 0;
uint8_t  passDistAtMax   = 0;
uint8_t  passLastDist    = 0;
uint8_t  passDir         = 0;
uint16_t passFrames      = 0;

// UART-parser
enum ParseState { WAIT_HEADER, READ_LEN, READ_PAYLOAD, READ_TAIL };
ParseState pState = WAIT_HEADER;
uint8_t  pHeader[4];
uint8_t  pHeaderPos = 0;
bool     pIsData    = false;
uint16_t pLen       = 0;
uint8_t  pLenPos    = 0;
uint8_t  pPayload[128];
uint16_t pPayloadPos = 0;
uint8_t  pTailPos   = 0;

static const uint8_t DATA_HEAD[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t DATA_TAIL[4] = {0xF8, 0xF7, 0xF6, 0xF5};
static const uint8_t CMD_HEAD[4]  = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t CMD_TAIL[4]  = {0x04, 0x03, 0x02, 0x01};

// ------------------------------------------------------------
//  HJÄLPFUNKTIONER
// ------------------------------------------------------------
uint32_t uptimeSeconds() {
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

uint32_t nowValue() {
  return timeSynced ? (uint32_t)time(nullptr) : uptimeSeconds();
}

void led(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(LED_PIN, r, g, b);
}

// ------------------------------------------------------------
//  INSTÄLLNINGAR (sparas i NVS och överlever omstart)
// ------------------------------------------------------------
void clampSettings(Settings& s) {
  if (s.maxDistance < 10)   s.maxDistance = 10;
  if (s.maxDistance > 100)  s.maxDistance = 100;
  if (s.direction > 2)      s.direction = 1;
  if (s.radarMinSpeed > 120) s.radarMinSpeed = 120;
  if (s.radarDelay < 1)     s.radarDelay = 1;
  if (s.radarDelay > 30)    s.radarDelay = 30;
  if (s.trigCount > 10)     s.trigCount = 10;
  if (s.gapDecis < 3)       s.gapDecis = 3;
}

void loadSettings() {
  prefs.begin("radar", false);
  if (prefs.getBytesLength("settings") == sizeof(Settings)) {
    prefs.getBytes("settings", &settings, sizeof(Settings));
  } else {
    settings = DEFAULT_SETTINGS;
  }
  clampSettings(settings);
  tzOffsetMin = prefs.getShort("tz", 60);
}

void saveSettings() {
  prefs.putBytes("settings", &settings, sizeof(Settings));
}

// ------------------------------------------------------------
//  RADARKOMMANDON
// ------------------------------------------------------------
void radarSend(uint16_t cmd, const uint8_t* value, uint8_t valueLen) {
  uint16_t len = 2 + valueLen;
  Serial1.write(CMD_HEAD, 4);
  Serial1.write((uint8_t)(len & 0xFF));
  Serial1.write((uint8_t)(len >> 8));
  Serial1.write((uint8_t)(cmd & 0xFF));
  Serial1.write((uint8_t)(cmd >> 8));
  if (valueLen) Serial1.write(value, valueLen);
  Serial1.write(CMD_TAIL, 4);
  Serial1.flush();
}

void radarWaitAndParse(uint32_t ms);   // framåtdeklaration

void radarApplySettings(bool includeSensitivity) {
  Serial.println("Radar: skickar inställningar...");
  const uint8_t enable[2] = {0x01, 0x00};
  radarSend(0x00FF, enable, 2);              // starta konfigurationsläge
  radarWaitAndParse(150);

  const uint8_t det[4] = {settings.maxDistance, settings.direction,
                          settings.radarMinSpeed, settings.radarDelay};
  radarSend(0x0002, det, 4);                 // avstånd, riktning, min-hastighet, fördröjning
  radarWaitAndParse(150);

  if (includeSensitivity && settings.trigCount > 0 && settings.snr != 255) {
    const uint8_t sens[4] = {settings.trigCount, settings.snr, 0, 0};
    radarSend(0x0003, sens, 4);
    radarWaitAndParse(150);
  }

  radarSend(0x00FE, nullptr, 0);             // avsluta konfigurationsläge
  radarWaitAndParse(150);
  Serial.printf("Radar: max %u m, riktning %u, min %u km/h\n",
                settings.maxDistance, settings.direction, settings.radarMinSpeed);
}

// ------------------------------------------------------------
//  LOGG I FLASHMINNET (LittleFS)
// ------------------------------------------------------------
void logInit() {
  if (!LittleFS.begin(true)) {
    Serial.println("FEL: LittleFS kunde inte startas");
    return;
  }
  size_t total = LittleFS.totalBytes();
  maxRecords = (total > 65536) ? (total - 65536) / sizeof(PassRecord) : 0;

  File f = LittleFS.open(LOG_FILE, "r");
  recordCount = f ? f.size() / sizeof(PassRecord) : 0;
  if (f) f.close();

  bootFirstIndex = recordCount;

  // Visa senaste passagen även efter omstart
  if (recordCount > 0) {
    File lf = LittleFS.open(LOG_FILE, "r");
    if (lf) {
      PassRecord r;
      lf.seek((recordCount - 1) * sizeof(PassRecord));
      if (lf.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) lastPassSpeed = r.speed;
      lf.close();
    }
  }
  logFull = recordCount >= maxRecords;
  Serial.printf("Logg: %lu passager sparade, plats för %lu\n", (unsigned long)recordCount, (unsigned long)maxRecords);
}

bool logAppend(const PassRecord& r) {
  if (recordCount >= maxRecords) {
    logFull = true;
    return false;
  }
  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) return false;
  size_t n = f.write((const uint8_t*)&r, sizeof(r));
  f.close();
  if (n != sizeof(r)) return false;
  recordCount++;
  return true;
}

void logClear() {
  LittleFS.remove(LOG_FILE);
  recordCount = 0;
  bootFirstIndex = 0;
  logFull = false;
  Serial.println("Logg: rensad");
}

// Rättar till tiden på passager som sparades innan klockan var ställd
void logFixUnsyncedTimes(uint32_t unixNow, uint32_t upNow) {
  if (bootFirstIndex >= recordCount) return;
  File f = LittleFS.open(LOG_FILE, "r+");
  if (!f) return;
  uint32_t fixed = 0;
  for (uint32_t i = bootFirstIndex; i < recordCount; i++) {
    PassRecord r;
    f.seek(i * sizeof(PassRecord));
    if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
    if (r.flags & FLAG_UNSYNCED) {
      r.time  = unixNow - (upNow - r.time);
      r.flags &= ~FLAG_UNSYNCED;
      f.seek(i * sizeof(PassRecord));
      f.write((const uint8_t*)&r, sizeof(r));
      fixed++;
    }
  }
  f.close();
  Serial.printf("Klocka: %lu passager fick rätt tid\n", (unsigned long)fixed);
}

// ------------------------------------------------------------
//  PASSAGER (räkning av fordon)
// ------------------------------------------------------------
void finishPass() {
  if (!passActive) return;
  passActive = false;

  if (passFrames < 2 || passMaxSpeed < settings.logMinSpeed) {
    return;   // för kort eller för långsam, troligen brus
  }

  PassRecord r;
  r.time     = nowValue();
  r.speed    = passMaxSpeed;
  r.distance = passDistAtMax;
  r.dir      = passDir;
  r.flags    = timeSynced ? 0 : FLAG_UNSYNCED;

  bool ok = logAppend(r);
  lastPassSpeed = passMaxSpeed;
  Serial.printf("PASSAGE #%lu: %u km/h vid %u m, %s%s\n", (unsigned long)recordCount, r.speed, r.distance,
                r.dir ? "MOT radarn" : "FRAN radarn", ok ? "" : "  (EJ SPARAD, loggen full)");
  led(0, 0, 40);
  delay(30);
  led(0, 0, 0);
}

void startPass(const Target& t) {
  passActive    = true;
  passMaxSpeed  = t.speed;
  passDistAtMax = t.distance;
  passLastDist  = t.distance;
  passDir       = (t.dir == TARGET_DIR_APPROACH) ? 1 : 0;
  passFrames    = 1;
  passLastSeenMs = millis();
}

void handleTargets(const Target* targets, uint8_t n) {
  lastFrameMs = millis();
  lastTargets = n;

  if (n == 0) {
    lastPrimary = {0, 0, 0, 0, 0};
    return;
  }

  // Primärt mål = närmaste målet
  uint8_t best = 0;
  for (uint8_t i = 1; i < n; i++) {
    if (targets[i].distance < targets[best].distance) best = i;
  }
  const Target& t = targets[best];
  lastPrimary = t;

  bool approaching = (t.dir == TARGET_DIR_APPROACH);

  if (!passActive) {
    startPass(t);
    return;
  }

  // Ny bil? Ett mål som kommer mot radarn och plötsligt syns långt bort igen
  if (approaching && t.distance > passLastDist + 15) {
    finishPass();
    startPass(t);
    return;
  }

  passFrames++;
  passLastSeenMs = millis();
  passLastDist   = t.distance;
  if (t.speed > passMaxSpeed) {
    passMaxSpeed  = t.speed;
    passDistAtMax = t.distance;
  }
}

void checkPassTimeout() {
  if (passActive && millis() - passLastSeenMs > (uint32_t)settings.gapDecis * 100) {
    finishPass();
  }
}

// ------------------------------------------------------------
//  UART-PARSER FÖR LD2451
//  Dataram:  F4 F3 F2 F1 | längd (2 byte) | data | F8 F7 F6 F5
//  Data:     [0] antal mål, [1] larm (1 = fordon närmar sig),
//            sedan 5 byte per mål:
//            vinkel(+0x80), avstånd m, riktning (1 = mot), hastighet km/h, SNR
//  (Bekräftat mot exemplet i Hi-Links manual V1.0, sid 11:
//   02 01 | 81 35 01 1B 13 | 81 3E 01 1B 04  =  2 mål, 53 m och 62 m, 27 km/h, riktning 1)
// ------------------------------------------------------------
void handleDataFrame(const uint8_t* p, uint16_t len) {
  Target targets[8];
  uint8_t n = 0;
  if (len >= 2) {
    uint16_t count = (len - 2) / 5;          // så många som får plats i ramen
    if (p[0] < count) count = p[0];          // ...men inte fler än radarn säger
    if (count > 8) count = 8;
    for (uint16_t i = 0; i < count; i++) {
      const uint8_t* b = p + 2 + i * 5;
      targets[n].angle    = (int8_t)((int16_t)b[0] - 0x80);
      targets[n].distance = b[1];
      targets[n].dir      = b[2];
      targets[n].speed    = b[3];
      targets[n].snr      = b[4];
      n++;
    }
  }

#if DEBUG_RAW
  if (n > 0) {
    Serial.print("RADAR ");
    for (uint16_t i = 0; i < len; i++) Serial.printf("%02X ", p[i]);
    Serial.print(" -> ");
    for (uint8_t i = 0; i < n; i++) {
      Serial.printf("[%u km/h, %u m, %d grader, riktn=%u, snr=%u] ",
                    targets[i].speed, targets[i].distance, targets[i].angle,
                    targets[i].dir, targets[i].snr);
    }
    Serial.println();
  }
#endif

  handleTargets(targets, n);
}

void handleCmdFrame(const uint8_t* p, uint16_t len) {
  if (len >= 4) {
    uint16_t cmd    = p[0] | (p[1] << 8);
    uint16_t status = p[2] | (p[3] << 8);
    if (status == 0) lastAckMs = millis();
    Serial.printf("Radar svar: kommando 0x%04X, %s\n", cmd, status == 0 ? "OK" : "FEL");
  }
}

void parseByte(uint8_t c) {
  switch (pState) {
    case WAIT_HEADER:
      pHeader[0] = pHeader[1];
      pHeader[1] = pHeader[2];
      pHeader[2] = pHeader[3];
      pHeader[3] = c;
      if (memcmp(pHeader, DATA_HEAD, 4) == 0 || memcmp(pHeader, CMD_HEAD, 4) == 0) {
        pIsData = (pHeader[0] == 0xF4);
        pState  = READ_LEN;
        pLenPos = 0;
        pLen    = 0;
        memset(pHeader, 0, 4);
      }
      break;

    case READ_LEN:
      if (pLenPos == 0) { pLen = c; pLenPos = 1; }
      else {
        pLen |= (uint16_t)c << 8;
        if (pLen > sizeof(pPayload)) { pState = WAIT_HEADER; break; }
        pPayloadPos = 0;
        pState = (pLen == 0) ? READ_TAIL : READ_PAYLOAD;
        pTailPos = 0;
      }
      break;

    case READ_PAYLOAD:
      pPayload[pPayloadPos++] = c;
      if (pPayloadPos >= pLen) { pState = READ_TAIL; pTailPos = 0; }
      break;

    case READ_TAIL: {
      const uint8_t* tail = pIsData ? DATA_TAIL : CMD_TAIL;
      if (c != tail[pTailPos]) { pState = WAIT_HEADER; break; }
      if (++pTailPos == 4) {
        if (pIsData) handleDataFrame(pPayload, pLen);
        else         handleCmdFrame(pPayload, pLen);
        pState = WAIT_HEADER;
      }
      break;
    }
  }
}

void radarPoll() {
  while (Serial1.available()) parseByte((uint8_t)Serial1.read());
}

void radarWaitAndParse(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    radarPoll();
    delay(2);
  }
}

// ------------------------------------------------------------
//  SIMULATOR (SIMULATOR = 1)
// ------------------------------------------------------------
#if SIMULATOR
uint32_t simNextCar = 0;
bool     simCarActive = false;
float    simDist = 0;
uint8_t  simSpeed = 0;
uint32_t simLastFrame = 0;

void simulatorTick() {
  uint32_t now = millis();
  if (now - simLastFrame < 100) return;   // 10 bilder per sekund
  simLastFrame = now;

  if (!simCarActive && now > simNextCar) {
    simCarActive = true;
    simDist  = settings.maxDistance;
    simSpeed = random(25, 85);
  }

  if (simCarActive) {
    simDist -= simSpeed / 3.6f * 0.1f;
    if (simDist < 3) {
      simCarActive = false;
      simNextCar = now + random(4000, 15000);
      handleTargets(nullptr, 0);
      return;
    }
    Target t;
    t.angle    = (int8_t)random(-10, 10);
    t.distance = (uint8_t)simDist;
    t.dir      = TARGET_DIR_APPROACH;
    t.speed    = simSpeed + random(-2, 3);
    t.snr      = 20;
    handleTargets(&t, 1);
  } else {
    handleTargets(nullptr, 0);
  }
}
#endif

// ------------------------------------------------------------
//  BLUETOOTH
// ------------------------------------------------------------
void updateSettingsCharacteristic() {
  chrSettings->setValue((const uint8_t*)&settings, sizeof(Settings));
}

void updateInfoCharacteristic() {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"fw\":\"%s\",\"maxRecords\":%u,\"recordSize\":%u,\"sim\":%d}",
           FW_VERSION, (unsigned)maxRecords, (unsigned)sizeof(PassRecord), SIMULATOR);
  chrInfo->setValue((const uint8_t*)buf, strlen(buf));
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected  = true;
    bleConnHandle = info.getConnHandle();
    bleMtu        = 23;
    Serial.println("Bluetooth: ansluten");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected  = false;
    bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
    reqAbort      = true;
    Serial.println("Bluetooth: frånkopplad");
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    bleMtu = mtu;
    Serial.printf("Bluetooth: MTU %u\n", mtu);
  }
  uint32_t onPassKeyDisplay() override {
    Serial.printf("Bluetooth: PIN %06u\n", (unsigned)BLE_PASSKEY);
    return BLE_PASSKEY;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("Bluetooth: parkoppling %s\n", info.isEncrypted() ? "OK" : "MISSLYCKADES");
  }
};

class SettingsCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() != sizeof(Settings)) return;
    Settings s;
    memcpy(&s, v.data(), sizeof(Settings));
    clampSettings(s);
    bool sensChanged = (s.trigCount != settings.trigCount) || (s.snr != settings.snr);
    settings = s;
    reqSensitivity   = sensChanged;
    reqApplySettings = true;
  }
};

class TimeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 4) return;
    uint32_t t;
    memcpy(&t, v.data(), 4);
    if (t < 1700000000UL) return;   // orimlig tid
    reqTimeValue = t;
    reqTimeSync  = true;
  }
};

class LogCtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 1) return;
    const uint8_t* d = v.data();
    switch (d[0]) {
      case 0x01:   // starta nedladdning: 01 + startindex (uint32)
        reqDownloadFrom = 0;
        if (v.size() >= 5) memcpy((void*)&reqDownloadFrom, d + 1, 4);
        reqDownload = true;
        break;
      case 0x02:   // rensa loggen: 02 'C' 'L' 'R'
        if (v.size() == 4 && d[1] == 'C' && d[2] == 'L' && d[3] == 'R') reqClearLog = true;
        break;
      case 0x03:   // avbryt nedladdning
        reqAbort = true;
        break;
    }
  }
};

void bleInit() {
  const char* name = deviceName;
  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);         // bonding, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);  // telefonen frågar efter PIN
  NimBLEDevice::setSecurityPasskey(BLE_PASSKEY);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  bleServer->advertiseOnDisconnect(true);

  NimBLEService* svc = bleServer->createService(SERVICE_UUID);

  chrLive = svc->createCharacteristic(LIVE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chrInfo = svc->createCharacteristic(INFO_UUID, NIMBLE_PROPERTY::READ);
  chrSettings = svc->createCharacteristic(SETTINGS_UUID,
                  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  NimBLECharacteristic* chrTime = svc->createCharacteristic(TIME_UUID,
                  NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  NimBLECharacteristic* chrLogCtrl = svc->createCharacteristic(LOGCTRL_UUID,
                  NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  chrLogData = svc->createCharacteristic(LOGDATA_UUID, NIMBLE_PROPERTY::NOTIFY);

  chrSettings->setCallbacks(new SettingsCallbacks());
  chrTime->setCallbacks(new TimeCallbacks());
  chrLogCtrl->setCallbacks(new LogCtrlCallbacks());

  updateSettingsCharacteristic();
  updateInfoCharacteristic();
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(SERVICE_UUID));
  adv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName(name);
  adv->setScanResponseData(scanData);
  adv->start();

  Serial.printf("Bluetooth: annonserar som \"%s\", PIN %06u\n", name, (unsigned)BLE_PASSKEY);
}

// LD2451 skickar bara data när den ser något. Radarn räknas som OK om
// den har skickat data eller svarat på ett kommando de senaste 15 minuterna.
bool radarIsOk() {
#if SIMULATOR
  return true;
#else
  uint32_t now = millis();
  bool frameRecent = lastFrameMs > 0 && now - lastFrameMs < 15UL * 60 * 1000;
  bool ackRecent   = lastAckMs  > 0 && now - lastAckMs  < 15UL * 60 * 1000;
  return frameRecent || ackRecent;
#endif
}

// Om inga dataramar kommit på en stund är målet borta
void clearStaleTarget() {
  if (lastTargets > 0 && millis() - lastFrameMs > 800) {
    lastPrimary = {0, 0, 0, 0, 0};
    lastTargets = 0;
  }
}

// Var 10:e minut utan trafik: fråga radarn om den lever
void radarHealthCheck() {
#if !SIMULATOR
  uint32_t now = millis();
  if (passActive || now - lastHealthMs < 10UL * 60 * 1000) return;
  lastHealthMs = now;
  if (lastFrameMs > 0 && now - lastFrameMs < 10UL * 60 * 1000) return;
  const uint8_t enable[2] = {0x01, 0x00};
  radarSend(0x00FF, enable, 2);
  radarWaitAndParse(100);
  radarSend(0x00FE, nullptr, 0);
  radarWaitAndParse(100);
#endif
}

void sendLive() {
  LivePacket p;
  p.version       = 1;
  p.speed         = lastPrimary.speed;
  p.distance      = lastPrimary.distance;
  p.angle         = lastPrimary.angle;
  p.dir           = (lastPrimary.dir == TARGET_DIR_APPROACH) ? 1 : 0;
  p.targets       = lastTargets;
  p.flags         = (timeSynced ? 0x01 : 0) | (logFull ? 0x02 : 0) |
                    (radarIsOk() ? 0x04 : 0) | (SIMULATOR ? 0x08 : 0);
  p.lastPassSpeed = lastPassSpeed;
  p.recordCount   = recordCount;
  p.now           = nowValue();
  chrLive->setValue((const uint8_t*)&p, sizeof(p));
  if (bleConnected) chrLive->notify();
}

// Skickar loggen i paket:  startindex (uint32) + antal (uint8) + N x 8 byte
// Slutpaket:  FF FF FF FF 00
void downloadTick() {
  if (!downloading) return;
  if (!bleConnected) { downloading = false; return; }
  if (millis() - lastDownloadMs < 8) return;
  lastDownloadMs = millis();

  uint8_t buf[260];
  uint16_t payload = bleMtu > 3 ? bleMtu - 3 : 20;
  if (payload > sizeof(buf)) payload = sizeof(buf);

  if (downloadIndex >= recordCount) {
    const uint8_t endPkt[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (chrLogData->notify(endPkt, 5, bleConnHandle)) {
      downloading = false;
      Serial.println("Nedladdning: klar");
    }
    return;
  }

  uint8_t maxRecs = (payload - 5) / sizeof(PassRecord);
  if (maxRecs == 0) maxRecs = 1;
  uint32_t remaining = recordCount - downloadIndex;
  uint8_t n = remaining < maxRecs ? remaining : maxRecs;

  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) { downloading = false; return; }
  f.seek(downloadIndex * sizeof(PassRecord));
  size_t got = f.read(buf + 5, n * sizeof(PassRecord));
  f.close();
  n = got / sizeof(PassRecord);
  if (n == 0) { downloadIndex = recordCount; return; }

  memcpy(buf, &downloadIndex, 4);
  buf[4] = n;
  if (chrLogData->notify(buf, 5 + n * sizeof(PassRecord), bleConnHandle)) {
    downloadIndex += n;   // gick iväg, ta nästa
  }                       // annars: bufferten full, försök igen nästa varv
}

void handleBleRequests() {
  if (reqApplySettings) {
    reqApplySettings = false;
    bool sens = reqSensitivity;
    reqSensitivity = false;
    saveSettings();
    updateSettingsCharacteristic();
#if !SIMULATOR
    radarApplySettings(sens);
#else
    (void)sens;
#endif
  }

  if (reqTimeSync) {
    reqTimeSync = false;
    uint32_t t = reqTimeValue;
    struct timeval tv = {(time_t)t, 0};
    settimeofday(&tv, nullptr);
    bool wasSynced = timeSynced;
    timeSynced = true;
    if (!wasSynced) logFixUnsyncedTimes(t, uptimeSeconds());
    Serial.printf("Klocka: ställd till %lu\n", (unsigned long)t);
  }

  if (reqClearLog) {
    reqClearLog = false;
    downloading = false;
    logClear();
  }

  if (reqAbort) {
    reqAbort = false;
    if (downloading) Serial.println("Nedladdning: avbruten");
    downloading = false;
  }

  if (reqDownload) {
    reqDownload = false;
    downloadIndex = reqDownloadFrom;
    downloading = true;
    lastDownloadMs = 0;
    Serial.printf("Nedladdning: från post %lu av %lu\n", (unsigned long)downloadIndex, (unsigned long)recordCount);
  }
}

// ------------------------------------------------------------
//  WIFI OCH WEBBSIDA  (http://192.168.4.1)
// ------------------------------------------------------------
static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html lang="sv"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Radarlogger</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--txt:#1b1d21;--mut:#6b7280;--acc:#2563eb;--ok:#16a34a;--bad:#dc2626}
@media(prefers-color-scheme:dark){:root{--bg:#111318;--card:#1c1f26;--txt:#e8eaed;--mut:#9aa0a6;--acc:#60a5fa}}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,sans-serif;background:var(--bg);color:var(--txt)}
main{max-width:520px;margin:0 auto;padding:16px}
.card{background:var(--card);border-radius:14px;padding:16px;margin-bottom:14px}
h1{font-size:18px;margin:4px 0 14px}h2{font-size:15px;margin:0 0 10px;color:var(--mut)}
.speed{font-size:96px;font-weight:700;text-align:center;line-height:1;font-variant-numeric:tabular-nums}
.unit{text-align:center;color:var(--mut);margin-bottom:10px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;text-align:center}
.grid b{display:block;font-size:20px;font-variant-numeric:tabular-nums}.grid span{font-size:12px;color:var(--mut)}
table{width:100%;border-collapse:collapse;font-size:14px;font-variant-numeric:tabular-nums}
td,th{padding:6px 4px;text-align:left;border-bottom:1px solid rgba(128,128,128,.2)}th{color:var(--mut);font-weight:500}
.btn{display:block;width:100%;padding:12px;border:0;border-radius:10px;background:var(--acc);color:#fff;font-size:15px;text-align:center;text-decoration:none;margin-top:8px}
.btn.sec{background:transparent;color:var(--bad);border:1px solid var(--bad)}
label{display:flex;justify-content:space-between;align-items:center;margin:8px 0;gap:10px}
input,select{font-size:15px;padding:6px;border-radius:8px;border:1px solid rgba(128,128,128,.4);background:var(--bg);color:var(--txt);width:130px}
.st{font-size:12px;color:var(--mut);text-align:center}
</style></head><body><main>
<h1 id="name">Radarlogger</h1>
<div class="card">
<div class="speed" id="spd">–</div><div class="unit">km/h</div>
<div class="grid">
<div><b id="dist">–</b><span>avstånd m</span></div>
<div><b id="dir">–</b><span>riktning</span></div>
<div><b id="ang">–</b><span>vinkel °</span></div>
</div></div>
<div class="card"><div class="grid">
<div><b id="cnt">0</b><span>passager totalt</span></div>
<div><b id="last">–</b><span>senaste km/h</span></div>
<div><b id="tgt">0</b><span>mål just nu</span></div>
</div><p class="st" id="st"></p></div>
<div class="card"><h2>Senaste passager</h2>
<table><thead><tr><th>Tid</th><th>km/h</th><th>m</th><th>Riktning</th></tr></thead><tbody id="rec"></tbody></table>
<a class="btn" href="/log.csv">Ladda ner hela loggen (CSV)</a></div>
<div class="card"><h2>Inställningar</h2>
<label>Riktning<select id="s_dir"><option value="1">Mot radarn</option><option value="0">Från radarn</option><option value="2">Båda</option></select></label>
<label>Max avstånd (m)<input id="s_max" type="number" min="10" max="100"></label>
<label>Radarns min-hastighet<input id="s_min" type="number" min="0" max="120"></label>
<label>Spara passager över (km/h)<input id="s_log" type="number" min="0" max="200"></label>
<label>Paus innan passage avslutas (s)<input id="s_gap" type="number" min="0.3" max="25" step="0.1"></label>
<button class="btn" onclick="saveS()">Spara inställningar</button>
<button class="btn sec" onclick="clr()">Rensa loggen</button></div>
<p class="st">Firmware <span id="fw"></span></p>
</main><script>
const $=id=>document.getElementById(id);
const tz=-new Date().getTimezoneOffset();
fetch('/settime?t='+Math.floor(Date.now()/1000)+'&tz='+tz);
function fmt(t,f){if(f&1)return'okänd tid';const d=new Date(t*1000);return d.toLocaleDateString('sv-SE')+' '+d.toLocaleTimeString('sv-SE')}
async function live(){try{const r=await(await fetch('/live')).json();
$('spd').textContent=r.speed||'–';$('dist').textContent=r.speed?r.dist:'–';
$('dir').textContent=r.speed?(r.dir?'mot':'från'):'–';$('ang').textContent=r.speed?r.angle:'–';
$('cnt').textContent=r.count;$('last').textContent=r.last||'–';$('tgt').textContent=r.targets;
$('st').textContent=(r.radar?'Radar OK':'Ingen data från radarn')+' · '+(r.synced?'klocka ställd':'klocka ej ställd')+(r.full?' · LOGGEN FULL':'');
$('name').textContent=r.name;$('fw').textContent=r.fw}catch(e){$('st').textContent='Ingen kontakt'}}
async function rec(){try{const a=await(await fetch('/recent?n=15')).json();
$('rec').innerHTML=a.map(x=>'<tr><td>'+fmt(x[0],x[4])+'</td><td>'+x[1]+'</td><td>'+x[2]+'</td><td>'+(x[3]?'mot':'från')+'</td></tr>').join('')}catch(e){}}
async function loadS(){const s=await(await fetch('/settings')).json();
$('s_dir').value=s.direction;$('s_max').value=s.maxDistance;$('s_min').value=s.radarMinSpeed;$('s_log').value=s.logMinSpeed;$('s_gap').value=s.gapDecis/10}
async function saveS(){const q=new URLSearchParams({direction:$('s_dir').value,maxDistance:$('s_max').value,radarMinSpeed:$('s_min').value,logMinSpeed:$('s_log').value,gapDecis:Math.round($('s_gap').value*10)});
await fetch('/settings?'+q,{method:'POST'});alert('Sparat')}
async function clr(){if(confirm('Rensa ALLA sparade passager?')){await fetch('/clear',{method:'POST'});rec()}}
live();rec();loadS();setInterval(live,300);setInterval(rec,3000);
</script></body></html>)HTML";

void webRoot() {
  web.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void webLive() {
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"speed\":%u,\"dist\":%u,\"angle\":%d,\"dir\":%u,\"targets\":%u,"
    "\"count\":%lu,\"last\":%u,\"synced\":%s,\"radar\":%s,\"full\":%s,"
    "\"now\":%lu,\"name\":\"%s\",\"fw\":\"%s\"}",
    lastPrimary.speed, lastPrimary.distance, lastPrimary.angle,
    (lastPrimary.dir == TARGET_DIR_APPROACH) ? 1 : 0, lastTargets,
    (unsigned long)recordCount, lastPassSpeed,
    timeSynced ? "true" : "false",
    radarIsOk() ? "true" : "false",
    logFull ? "true" : "false",
    (unsigned long)nowValue(), deviceName, FW_VERSION);
  web.send(200, "application/json", buf);
}

void webRecent() {
  uint32_t n = web.hasArg("n") ? web.arg("n").toInt() : 15;
  if (n > 100) n = 100;
  if (n > recordCount) n = recordCount;
  String out = "[";
  File f = LittleFS.open(LOG_FILE, "r");
  if (f) {
    for (uint32_t k = 0; k < n; k++) {
      uint32_t i = recordCount - 1 - k;     // nyast först
      PassRecord r;
      f.seek(i * sizeof(PassRecord));
      if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
      if (k) out += ",";
      out += "[" + String(r.time) + "," + String(r.speed) + "," + String(r.distance) + "," +
             String(r.dir) + "," + String(r.flags) + "]";
    }
    f.close();
  }
  out += "]";
  web.send(200, "application/json", out);
}

void webSetTime() {
  if (web.hasArg("tz")) {
    int tz = web.arg("tz").toInt();
    if (tz >= -720 && tz <= 840 && tz != tzOffsetMin) {
      tzOffsetMin = tz;
      prefs.putShort("tz", tzOffsetMin);
    }
  }
  if (web.hasArg("t")) {
    uint32_t t = strtoul(web.arg("t").c_str(), nullptr, 10);
    if (t >= 1700000000UL) {
      reqTimeValue = t;
      reqTimeSync  = true;
    }
  }
  web.send(200, "text/plain", "OK");
}

void webLogCsv() {
  web.sendHeader("Content-Disposition", "attachment; filename=\"radarlogg.csv\"");
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/csv; charset=utf-8", "");
  web.sendContent("\xEF\xBB\xBFnr;datum;tid;hastighet_kmh;avstand_m;riktning\r\n");  // BOM för Excel

  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) { web.sendContent(""); return; }
  String chunk;
  chunk.reserve(2048);
  PassRecord r;
  uint32_t i = 0;
  while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
    char line[80];
    char date[16], clock[12];
    if (r.flags & FLAG_UNSYNCED) {
      strcpy(date, "okand");
      strcpy(clock, "");
    } else {
      time_t lt = (time_t)r.time + tzOffsetMin * 60;
      struct tm tmv;
      gmtime_r(&lt, &tmv);
      strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
      strftime(clock, sizeof(clock), "%H:%M:%S", &tmv);
    }
    snprintf(line, sizeof(line), "%lu;%s;%s;%u;%u;%s\r\n", (unsigned long)(++i), date, clock,
             r.speed, r.distance, r.dir ? "mot" : "fran");
    chunk += line;
    if (chunk.length() > 1800) { web.sendContent(chunk); chunk = ""; radarPoll(); }
  }
  f.close();
  if (chunk.length()) web.sendContent(chunk);
  web.sendContent("");   // avslutar överföringen
}

void webGetSettings() {
  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"maxDistance\":%u,\"direction\":%u,\"radarMinSpeed\":%u,\"radarDelay\":%u,"
    "\"trigCount\":%u,\"snr\":%u,\"gapDecis\":%u,\"logMinSpeed\":%u}",
    settings.maxDistance, settings.direction, settings.radarMinSpeed, settings.radarDelay,
    settings.trigCount, settings.snr, settings.gapDecis, settings.logMinSpeed);
  web.send(200, "application/json", buf);
}

void webPostSettings() {
  Settings s = settings;
  if (web.hasArg("maxDistance"))   s.maxDistance   = web.arg("maxDistance").toInt();
  if (web.hasArg("direction"))     s.direction     = web.arg("direction").toInt();
  if (web.hasArg("radarMinSpeed")) s.radarMinSpeed = web.arg("radarMinSpeed").toInt();
  if (web.hasArg("logMinSpeed"))   s.logMinSpeed   = web.arg("logMinSpeed").toInt();
  if (web.hasArg("gapDecis"))      s.gapDecis      = web.arg("gapDecis").toInt();
  clampSettings(s);
  settings = s;
  reqApplySettings = true;
  web.send(200, "text/plain", "OK");
}

void webClear() {
  reqClearLog = true;
  web.send(200, "text/plain", "OK");
}

void wifiInit() {
#if WIFI_ENABLED
  WiFi.mode(WIFI_AP);
  WiFi.softAP(deviceName, WIFI_PASSWORD);
  web.on("/",          HTTP_GET,  webRoot);
  web.on("/live",      HTTP_GET,  webLive);
  web.on("/recent",    HTTP_GET,  webRecent);
  web.on("/settime",   HTTP_GET,  webSetTime);
  web.on("/log.csv",   HTTP_GET,  webLogCsv);
  web.on("/settings",  HTTP_GET,  webGetSettings);
  web.on("/settings",  HTTP_POST, webPostSettings);
  web.on("/clear",     HTTP_POST, webClear);
  web.begin();
  Serial.printf("WiFi: natverk \"%s\", losenord \"%s\", oppna http://%s\n",
                deviceName, WIFI_PASSWORD, WiFi.softAPIP().toString().c_str());
#endif
}

// ------------------------------------------------------------
//  SETUP OCH LOOP
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== RADARLOGGER " FW_VERSION " ===");
  led(20, 0, 0);

  uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceName, sizeof(deviceName), "%s-%02X%02X", DEVICE_PREFIX,
           (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));

  loadSettings();
  logInit();

  Serial1.setRxBufferSize(1024);
  Serial1.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

#if SIMULATOR
  Serial.println("SIMULATOR aktiv - ingen radar behövs");
  randomSeed(esp_random());
#else
  radarWaitAndParse(500);           // låt radarn starta
  radarApplySettings(false);
#endif

  bleInit();
  wifiInit();
  led(0, 0, 0);
}

void loop() {
#if SIMULATOR
  simulatorTick();
#else
  radarPoll();
#endif

  checkPassTimeout();
  clearStaleTarget();
  radarHealthCheck();
#if WIFI_ENABLED
  web.handleClient();
#endif
  handleBleRequests();
  downloadTick();

  // Live-data: 5 gånger per sekund när någon är ansluten, annars var 2:a sekund
  uint32_t interval = bleConnected ? 200 : 2000;
  if (millis() - lastLiveNotify > interval) {
    lastLiveNotify = millis();
    sendLive();
  }

  delay(1);
}
