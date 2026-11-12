/*
 * ============================================================
 *  HÅRDVARA  –  radartyp, strömkälla och ljussensor
 *  Del av Radarlogger 0.5.0
 * ============================================================
 *
 *  RADAR (byts beroende på kund):
 *    LD2451  – 5 V, 115200 baud, binärt protokoll, avstånd + vinkel, ~100 m
 *    LD2415H – 9–24 V, 9600 baud, text "V+045.3", bara hastighet, ~180 m
 *    Radarn identifieras automatiskt vid start (kan också väljas på webbsidan).
 *    Båda kopplas till samma stift: radar TX -> GPIO 18, radar RX <- GPIO 17.
 *
 *  STRÖMKÄLLA:
 *    Spänningsdelare 100 kΩ / 10 kΩ från 12 V-ingången till GPIO 3
 *    (+ 100 nF mot GND). Läge "Nät" visar bara spänningen. Läge
 *    "Batteri" sparar ström och stänger av skylten vid låg spänning –
 *    loggningen fortsätter alltid.
 *
 *  LJUSSENSOR:
 *    VEML7700 (I2C): SDA GPIO 41, SCL GPIO 42, VIN 3V3   – mäter lux
 *    eller analog (KY-054 / MH-sensor / fototransistor):  AO -> GPIO 2, 3V3
 *    Utan sensor används klockan (dag/natt) som förut.
 * ============================================================
 */
#pragma once
#include <Wire.h>

#define VOLT_PIN        3
#define LIGHT_ADC_PIN   2
#define I2C_SDA_PIN     41
#define I2C_SCL_PIN     42
#define VEML_ADDR       0x10

enum { RADAR_AUTO = 0, RADAR_LD2451 = 1, RADAR_LD2415 = 2 };
enum { LIGHT_CLOCK = 0, LIGHT_VEML = 1, LIGHT_ANALOG = 2 };

struct HwSettings {
  uint8_t  radarType;     // 0 = automatisk, 1 = LD2451, 2 = LD2415H
  uint8_t  detected;      // senast identifierade radar (1 eller 2)
  uint8_t  ld15Sens;      // LD2415H känslighet 1–15 (lägre = känsligare), standard 5
  uint8_t  ld15Angle;     // LD2415H vinkelkompensation 0–60 grader
  uint8_t  powerSource;   // 0 = nät (230 V), 1 = batteri/sol
  uint16_t vCal;          // spänningsdelarens faktor × 1000 (100k/10k = 11000)
  uint8_t  vLow;          // dV: under detta sparläge (124 = 12,4 V)
  uint8_t  vCrit;         // dV: under detta släcks skylten (120 = 12,0 V)
  uint8_t  vResume;       // dV: skylten tänds igen över detta (128 = 12,8 V)
  uint8_t  lightMode;     // 0 = klocka, 1 = VEML7700, 2 = analog sensor
  uint16_t luxDark;       // VEML7700: under detta = nattljus
  uint16_t luxBright;     // VEML7700: över detta = full dagsljusstyrka
  uint16_t anaDark;       // analog: mV i mörker
  uint16_t anaBright;     // analog: mV i fullt dagsljus
};

static const HwSettings DEFAULT_HW = {RADAR_AUTO, RADAR_LD2451, 5, 0, 0, 11000, 124, 120, 128,
                                      LIGHT_VEML, 5, 20000, 3000, 300};
HwSettings hw;

const char* radarName(uint8_t t) {
  return t == RADAR_LD2415 ? "LD2415H" : (t == RADAR_LD2451 ? "LD2451" : "okänd");
}

void clampHw(HwSettings& h) {
  if (h.radarType > 2) h.radarType = RADAR_AUTO;
  if (h.detected != RADAR_LD2451 && h.detected != RADAR_LD2415) h.detected = RADAR_LD2451;
  if (h.ld15Sens < 1 || h.ld15Sens > 15) h.ld15Sens = 5;
  if (h.ld15Angle > 60) h.ld15Angle = 0;
  if (h.powerSource > 1) h.powerSource = 0;
  if (h.vCal < 2000 || h.vCal > 30000) h.vCal = 11000;
  if (h.vCrit < 80 || h.vCrit > 250) h.vCrit = 120;
  if (h.vLow < h.vCrit) h.vLow = h.vCrit;
  if (h.vResume <= h.vCrit) h.vResume = h.vCrit + 8;
  if (h.lightMode > 2) h.lightMode = LIGHT_VEML;
  if (h.luxDark < 1) h.luxDark = 1;
  if (h.luxBright <= h.luxDark) h.luxBright = h.luxDark * 10;
}

void loadHw() {
  hw = DEFAULT_HW;
  if (prefs.getBytesLength("hw") == sizeof(HwSettings)) prefs.getBytes("hw", &hw, sizeof(hw));
  clampHw(hw);
}

void saveHw() { prefs.putBytes("hw", &hw, sizeof(hw)); }

// ------------------------------------------------------------
//  RADAR: LD2415H (text, 9600 baud)
//  Mål mot radarn: "V+045.3\r\n", från radarn: "V-045.3\r\n"
// ------------------------------------------------------------
char     ld15Line[96];
uint8_t  ld15Pos = 0;
bool     ld15SawReply = false;

void ld15HandleLine(const char* s) {
  if (s[0] == 'V' && (s[1] == '+' || s[1] == '-')) {
    float v = atof(s + 2);
    lastAckMs = millis();
    ld15SawReply = true;
    if (v >= 1.0f && v < 300.0f) {
      Target t;
      t.angle    = 0;
      t.distance = 0;                       // LD2415H mäter inte avstånd
      t.dir      = (s[1] == '+') ? TARGET_DIR_APPROACH : 0;
      t.speed    = (uint8_t)(v + 0.5f);
      t.snr      = 0;
#if DEBUG_RAW
      Serial.printf("RADAR LD2415H %s -> %u km/h %s\n", s, t.speed, t.dir ? "mot" : "från");
#endif
      handleTargets(&t, 1);
    } else {
      handleTargets(nullptr, 0);
    }
    return;
  }
  // Svar på "läs parametrar" (CF 07): "No. ..." och "X1:01 X2:00 ..."
  if (strstr(s, "X1:") || strstr(s, "No.")) {
    lastAckMs = millis();
    ld15SawReply = true;
    Serial.printf("Radar LD2415H svar: %s\n", s);
  }
}

void ld15Byte(uint8_t c) {
  if (c == '\n' || c == '\r') {
    if (ld15Pos > 0) {
      ld15Line[ld15Pos] = 0;
      ld15HandleLine(ld15Line);
      ld15Pos = 0;
    }
    return;
  }
  if (c < 0x20 || c > 0x7E) { ld15Pos = 0; return; }   // skräp: börja om
  if (ld15Pos < sizeof(ld15Line) - 1) ld15Line[ld15Pos++] = (char)c;
  else ld15Pos = 0;
}

void ld15Send(const uint8_t* b, size_t n) {
  Serial1.write(b, n);
  Serial1.flush();
}

void ld15ReadParams() {
  const uint8_t cmd[13] = {0x43, 0x46, 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  ld15Send(cmd, sizeof(cmd));
}

void ld15ApplySettings() {
  uint8_t minSpeed = settings.radarMinSpeed < 1 ? 1 : settings.radarMinSpeed;
  const uint8_t c1[8] = {0x43, 0x46, 0x01, minSpeed, hw.ld15Angle, hw.ld15Sens, 0x0D, 0x0A};
  ld15Send(c1, sizeof(c1));
  radarWaitAndParse(200);
  // Riktning: appen 0 = från, 1 = mot, 2 = båda  ->  LD2415H 2 = från, 1 = mot, 0 = båda
  static const uint8_t dirMap[3] = {2, 1, 0};
  const uint8_t c2[8] = {0x43, 0x46, 0x02, dirMap[settings.direction % 3], 0x01, 0x00, 0x0D, 0x0A};
  ld15Send(c2, sizeof(c2));
  radarWaitAndParse(200);
  Serial.printf("Radar LD2415H: min %u km/h, riktning %u, känslighet %u, vinkel %u°\n",
                minSpeed, settings.direction, hw.ld15Sens, hw.ld15Angle);
}

// ------------------------------------------------------------
//  RADAR: identifiering och byte av typ
// ------------------------------------------------------------
uint32_t lastDetectMs = 0;

void radarSerialBegin(uint32_t baud) {
  Serial1.end();
  Serial1.setRxBufferSize(1024);
  Serial1.begin(baud, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  delay(20);
  while (Serial1.available()) Serial1.read();
}

bool probeLD2451() {
  radarActive = RADAR_LD2451;
  radarSerialBegin(115200);
  uint32_t before = lastAckMs;
  const uint8_t enable[2] = {0x01, 0x00};
  radarSend(0x00FF, enable, 2);
  radarWaitAndParse(300);
  radarSend(0x00FE, nullptr, 0);
  radarWaitAndParse(200);
  return lastAckMs != before || lastFrameMs != 0;
}

bool probeLD2415() {
  radarActive = RADAR_LD2415;
  radarSerialBegin(9600);
  ld15SawReply = false;
  ld15Pos = 0;
  ld15ReadParams();
  radarWaitAndParse(900);
  return ld15SawReply;
}

uint8_t radarDetect() {
  lastDetectMs = millis();
  if (probeLD2451()) return RADAR_LD2451;
  if (probeLD2415()) return RADAR_LD2415;
  return 0;
}

// Startar rätt radar enligt inställningen och skickar inställningarna
void radarStart() {
#if !SIMULATOR
  uint8_t type = hw.radarType;
  if (type == RADAR_AUTO) {
    uint8_t found = radarDetect();
    if (found) {
      type = found;
      if (hw.detected != found) { hw.detected = found; saveHw(); }
      Serial.printf("Radar: hittade %s\n", radarName(found));
    } else {
      type = hw.detected;
      Serial.printf("Radar: svarar inte, använder senast kända (%s)\n", radarName(type));
    }
  }
  radarActive = type;
  radarHasDistance = (type == RADAR_LD2451);
  radarSerialBegin(type == RADAR_LD2415 ? 9600 : 115200);
  radarWaitAndParse(200);
  radarApplySettings(false);
#endif
}

// Automatiskt läge och ingen radar har hörts av: leta igen varje minut
void radarRedetectTick() {
#if !SIMULATOR
  if (hw.radarType != RADAR_AUTO) return;
  if (lastFrameMs != 0 || lastAckMs != 0) return;
  if (millis() - lastDetectMs < 60000) return;
  uint8_t found = radarDetect();
  if (found) {
    Serial.printf("Radar: hittade %s\n", radarName(found));
    if (hw.detected != found) { hw.detected = found; saveHw(); }
  }
  radarActive = found ? found : hw.detected;
  radarHasDistance = (radarActive == RADAR_LD2451);
  radarSerialBegin(radarActive == RADAR_LD2415 ? 9600 : 115200);
  if (found) radarApplySettings(false);
#endif
}

// ------------------------------------------------------------
//  STRÖMKÄLLA  (spänning på 12 V-ingången)
// ------------------------------------------------------------
float    supplyVolts   = -1;     // -1 = ingen mätning (delaren inte inkopplad)
float    voltAvg       = 0;
uint8_t  battState     = 0;      // 0 = OK, 1 = låg (sparläge), 2 = kritisk (skylten av)
uint32_t lastVoltMs    = 0;

float readSupplyRaw() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(VOLT_PIN);
  return (mv / 8.0f) * hw.vCal / 1000000.0f;
}

// Ungefärlig laddning för 4S LiFePO4 (vilospänning)
uint8_t battPercent(float v) {
  static const float V[] = {10.0, 12.0, 12.5, 12.8, 12.9, 13.0, 13.1, 13.2, 13.3, 13.4, 13.6};
  static const uint8_t P[] = {0, 9, 14, 17, 20, 30, 40, 70, 90, 99, 100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (v <= V[i]) return P[i - 1] + (uint8_t)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  }
  return 100;
}

void powerTick() {
  uint32_t now = millis();
  if (now - lastVoltMs < 1000) return;
  lastVoltMs = now;
  float v = readSupplyRaw();
  if (v < 1.5f) { supplyVolts = -1; voltAvg = 0; battState = 0; return; }   // ingen delare
  voltAvg = (voltAvg == 0) ? v : voltAvg * 0.9f + v * 0.1f;                 // ca 10 s medel
  supplyVolts = voltAvg;

  if (hw.powerSource != 1) { battState = 0; return; }
  float vLow = hw.vLow / 10.0f, vCrit = hw.vCrit / 10.0f, vRes = hw.vResume / 10.0f;
  uint8_t old = battState;
  if (battState == 2) {
    if (supplyVolts >= vRes) battState = (supplyVolts < vLow + 0.2f) ? 1 : 0;
  } else if (supplyVolts < vCrit) {
    battState = 2;
  } else if (battState == 1) {
    if (supplyVolts >= vLow + 0.2f) battState = 0;
  } else if (supplyVolts < vLow) {
    battState = 1;
  }
  if (battState != old) {
    static const char* names[] = {"OK", "LÅG – sparläge", "KRITISK – skylten släckt"};
    Serial.printf("Batteri: %.2f V, %s\n", supplyVolts, names[battState]);
  }
}

// Används av Skylt.h
bool powerPanelsBlocked() { return hw.powerSource == 1 && battState == 2; }
uint8_t powerSaveOverride() { return hw.powerSource == 1 ? 5 : 0; }   // batteri: släck efter 5 s

// ------------------------------------------------------------
//  LJUSSENSOR
// ------------------------------------------------------------
bool     vemlFound   = false;
uint8_t  vemlRange   = 0;        // 0 = känslig (inomhus/skymning), 1 = sol
uint32_t lastLightMs = 0;
float    luxNow      = -1;
float    anaMvNow    = -1;
float    lightFrac   = -1;       // 0 = mörkt, 1 = fullt dagsljus (utjämnat), -1 = okänt
uint8_t  vemlErrors  = 0;

bool vemlWrite(uint8_t reg, uint16_t val) {
  Wire.beginTransmission(VEML_ADDR);
  Wire.write(reg);
  Wire.write(val & 0xFF);
  Wire.write(val >> 8);
  return Wire.endTransmission() == 0;
}

bool vemlRead(uint8_t reg, uint16_t& val) {
  Wire.beginTransmission(VEML_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)VEML_ADDR, (uint8_t)2) != 2) return false;
  val = Wire.read() | (Wire.read() << 8);
  return true;
}

// Område 0: förstärkning 1, 100 ms  (0,0576 lux/steg, upp till ca 3 800 lux)
// Område 1: förstärkning 1/8, 25 ms (1,8432 lux/steg, upp till ca 120 000 lux)
bool vemlSetRange(uint8_t r) {
  vemlRange = r;
  uint16_t conf = (r == 0) ? 0x0000 : ((0b10 << 11) | (0b1100 << 6));
  return vemlWrite(0x00, conf);
}

void lightInit() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
  Wire.beginTransmission(VEML_ADDR);
  vemlFound = (Wire.endTransmission() == 0);
  if (vemlFound) {
    vemlWrite(0x03, 0x0000);          // ingen energisparfunktion
    vemlSetRange(0);
    Serial.println("Ljussensor: VEML7700 hittad");
  } else {
    Serial.println("Ljussensor: ingen VEML7700");
  }
  analogSetPinAttenuation(LIGHT_ADC_PIN, ADC_11db);
  analogSetPinAttenuation(VOLT_PIN, ADC_11db);
}

float vemlLux(uint16_t raw) {
  float res = (vemlRange == 0) ? 0.0576f : 1.8432f;
  float lux = raw * res;
  if (vemlRange == 1 && lux > 1000) {   // Vishays korrektion för höga värden
    lux = 6.0135e-13f * powf(lux, 4) - 9.3924e-9f * powf(lux, 3) + 8.1488e-5f * lux * lux + 1.0023f * lux;
  }
  return lux;
}

void lightTick() {
  uint32_t now = millis();
  if (now - lastLightMs < 500) return;
  float dt = (lastLightMs == 0) ? 0 : (now - lastLightMs) / 1000.0f;
  lastLightMs = now;

  float f = -1;
  if (hw.lightMode == LIGHT_VEML && vemlFound) {
    uint16_t raw;
    if (!vemlRead(0x04, raw)) {
      if (++vemlErrors > 10) { vemlFound = false; Serial.println("Ljussensor: VEML7700 svarar inte, använder klockan"); }
      return;
    }
    vemlErrors = 0;
    if (vemlRange == 0 && raw > 50000) { vemlSetRange(1); return; }
    float lux = vemlLux(raw);
    if (vemlRange == 1 && lux < 2000) { vemlSetRange(0); return; }
    luxNow = lux;
    float lo = log10f((float)hw.luxDark), hi = log10f((float)hw.luxBright);
    f = (log10f(lux < 0.1f ? 0.1f : lux) - lo) / (hi - lo);
  } else if (hw.lightMode == LIGHT_ANALOG) {
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(LIGHT_ADC_PIN);
    anaMvNow = mv / 8.0f;
    float d = hw.anaDark, b = hw.anaBright;
    if (fabsf(b - d) >= 50) f = (anaMvNow - d) / (b - d);   // fungerar åt båda hållen
  }

  if (f < 0) f = 0;
  if (f > 1) f = 1;
  if (hw.lightMode == LIGHT_CLOCK || (hw.lightMode == LIGHT_VEML && !vemlFound)) { lightFrac = -1; return; }
  // Utjämning ca 20 s, så att billjus och moln inte får skylten att pumpa
  if (lightFrac < 0 || dt == 0) lightFrac = f;
  else lightFrac += (f - lightFrac) * (dt / 20.0f > 1 ? 1 : dt / 20.0f);
}

// Används av Skylt.h: ljusstyrka i procent
uint8_t signBrightnessPct() {
  uint8_t pct;
  if (lightFrac >= 0) {
    pct = sign.nightBright + (uint8_t)(lightFrac * (sign.dayBright - (int)sign.nightBright) + 0.5f);
    if (sign.dayBright < sign.nightBright) pct = sign.nightBright;
  } else {
    pct = isNight() ? sign.nightBright : sign.dayBright;
  }
  if (hw.powerSource == 1 && battState >= 1 && pct > 50) pct = 50;   // sparläge
  pct = (pct / 2) * 2;                                                // undvik ständiga småändringar
  if (pct < 5) pct = 5;
  return pct;
}

const char* lightSourceText() {
  if (hw.lightMode == LIGHT_VEML) return vemlFound ? "VEML7700" : "klocka (ingen VEML7700)";
  if (hw.lightMode == LIGHT_ANALOG) return "analog sensor";
  return "klocka";
}

void hardwareInit() {
  loadHw();
  lightInit();
}

void hardwareTick() {
  powerTick();
  lightTick();
  radarRedetectTick();
}
