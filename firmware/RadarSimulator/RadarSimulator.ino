/*
 * ============================================================
 *  RADARSIMULATOR  –  låtsas vara en HLK-LD2451
 *  Version 0.2.0
 * ============================================================
 *
 *  Kör på den ANDRA ESP32-S3:an. Den skickar exakt samma
 *  dataramar som en riktig LD2451, så Radarloggern tror att
 *  den pratar med en riktig radar. Bra för att testa hela
 *  kedjan vid skrivbordet: tolkning, räkning, logg, webbsida
 *  och Bluetooth.
 *
 *  STARTAR AUTOMATISKT: så fort den får ström kör den ett
 *  program med olika trafiksituationer (fortkörare, inbromsning,
 *  kö, möte, cykel, lång paus …) om och om igen tills du stoppar.
 *  Stoppa/starta: knappen BOOT på kortet, eller webbsidan.
 *
 *  Styrs från mobilen:
 *    WiFi "RadarSim-XXXX", lösenord "simulator1"
 *    öppna http://192.168.4.1
 *
 *  KOPPLING (simulator-ESP -> logger-ESP):
 *      GPIO 17 (TX)  ->  GPIO 18 (RX)  på loggern
 *      GPIO 18 (RX)  <-  GPIO 17 (TX)  på loggern
 *      GND           ->  GND
 *  (Båda korten får ström via var sin USB-kabel.)
 *
 *  Arduino IDE: samma inställningar som Radarlogger
 *  (ESP32S3 Dev Module, 16MB, OPI PSRAM, Huge APP).
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#define FW_VERSION    "0.2.0"
#define BUTTON_PIN    0           // BOOT-knappen: start/stopp
#define SIM_TX_PIN    17          // -> loggerns RX (GPIO 18)
#define SIM_RX_PIN    18          // <- loggerns TX (GPIO 17)
#define SIM_BAUD      115200
#define FRAME_HZ      10          // dataramar per sekund
#define WIFI_PASS     "simulator1"
#define WIFI_CHANNEL  6           // annan kanal än loggern
#define LED_PIN       48

// ------------------------------------------------------------
//  RADARNS INSTÄLLNINGAR (ändras av loggern via kommandon)
// ------------------------------------------------------------
uint8_t cfgMaxDist  = 100;   // m
uint8_t cfgDir      = 2;     // 0 från, 1 mot, 2 båda
uint8_t cfgMinSpeed = 5;     // km/h
uint8_t cfgDelay    = 2;
uint8_t cfgTrig     = 1;
uint8_t cfgSnr      = 4;
bool    configMode  = false;
uint32_t cmdCount   = 0;

// ------------------------------------------------------------
//  SIMULERADE FORDON
// ------------------------------------------------------------
struct SimCar {
  bool   active;
  float  dist;      // m
  float  speed;     // km/h
  int8_t angle;     // grader
  bool   approach;  // true = mot radarn
  float  endDist;
  float  accel;     // km/h per sekund (minus = bromsar)
  float  target;    // hastighet där accelerationen slutar
};
#define MAX_CARS 4
SimCar cars[MAX_CARS];

// Läge: 0 = stopp, 1 = scenarier (startar vid ström på), 2 = slumpad trafik
uint8_t  mode       = 1;
bool     autoMode   = false;   // slumpad trafik (läge 2)
uint8_t  autoMin    = 30;
uint8_t  autoMax    = 80;
uint16_t autoGapMin = 4;     // sekunder mellan bilar
uint16_t autoGapMax = 15;
uint32_t nextAutoMs = 0;
uint32_t carsSent   = 0;
uint32_t lastFrameMs = 0;
uint8_t  lastSpeedSent = 0;

WebServer web(80);
char ssid[32];

// ------------------------------------------------------------
void addCar(float speed, bool approach, float startDist = -1, int8_t angle = 0,
            float accel = 0, float target = 0) {
  for (auto& c : cars) {
    if (!c.active) {
      c.active   = true;
      c.accel    = accel;
      c.target   = target;
      c.speed    = speed;
      c.approach = approach;
      c.angle    = angle;
      if (approach) {
        c.dist    = startDist > 0 ? startDist : 110;
        c.endDist = 2;
      } else {
        c.dist    = startDist > 0 ? startDist : 2;
        c.endDist = 110;
      }
      carsSent++;
      Serial.printf("Ny bil: %.0f km/h, %s\n", speed, approach ? "MOT" : "FRAN");
      return;
    }
  }
}

void stepCars(float dt) {
  for (auto& c : cars) {
    if (!c.active) continue;
    if (c.accel != 0) {
      c.speed += c.accel * dt;
      if ((c.accel > 0 && c.speed >= c.target) || (c.accel < 0 && c.speed <= c.target)) {
        c.speed = c.target;
        c.accel = 0;
      }
    }
    float move = c.speed / 3.6f * dt;
    if (c.approach) {
      c.dist -= move;
      if (c.dist <= c.endDist) c.active = false;
    } else {
      c.dist += move;
      if (c.dist >= c.endDist) c.active = false;
    }
  }
}

// ------------------------------------------------------------
//  SKICKA DATARAM I LD2451-FORMAT
//  F4 F3 F2 F1 | len | antal | larm | 5 byte/mål | F8 F7 F6 F5
// ------------------------------------------------------------
void sendFrame() {
  uint8_t payload[2 + MAX_CARS * 5];
  uint8_t n = 0;
  bool alarm = false;

  for (auto& c : cars) {
    if (!c.active) continue;
    if (c.dist > cfgMaxDist || c.dist < 1) continue;            // utanför räckvidd
    if (c.speed < cfgMinSpeed) continue;                        // för långsam
    if (cfgDir == 0 && c.approach) continue;                    // bara från
    if (cfgDir == 1 && !c.approach) continue;                   // bara mot

    uint8_t* b = payload + 2 + n * 5;
    int spd = (int)roundf(c.speed + random(-1, 2));             // lite mätbrus
    if (spd < 0) spd = 0;
    b[0] = (uint8_t)(0x80 + c.angle + random(-1, 2));
    b[1] = (uint8_t)c.dist;
    b[2] = c.approach ? 1 : 0;
    b[3] = (uint8_t)spd;
    b[4] = (uint8_t)random(40, 200);
    if (c.approach) alarm = true;
    lastSpeedSent = spd;
    n++;
  }
  if (n == 0) lastSpeedSent = 0;

  payload[0] = n;
  payload[1] = alarm ? 1 : 0;
  uint16_t len = n ? 2 + n * 5 : 0;          // riktig radar skickar längd 0 utan mål

  const uint8_t head[4] = {0xF4, 0xF3, 0xF2, 0xF1};
  const uint8_t tail[4] = {0xF8, 0xF7, 0xF6, 0xF5};
  Serial1.write(head, 4);
  Serial1.write((uint8_t)(len & 0xFF));
  Serial1.write((uint8_t)(len >> 8));
  if (len) Serial1.write(payload, len);
  Serial1.write(tail, 4);
}

// ------------------------------------------------------------
//  TA EMOT KONFIGURATIONSKOMMANDON FRÅN LOGGERN OCH SVARA (ACK)
//  FD FC FB FA | len | cmd (2) | värde | 04 03 02 01
// ------------------------------------------------------------
void sendAck(uint16_t cmd, const uint8_t* extra, uint8_t extraLen) {
  uint16_t ackCmd = cmd | 0x0100;
  uint16_t len = 4 + extraLen;
  const uint8_t head[4] = {0xFD, 0xFC, 0xFB, 0xFA};
  const uint8_t tail[4] = {0x04, 0x03, 0x02, 0x01};
  Serial1.write(head, 4);
  Serial1.write((uint8_t)(len & 0xFF));
  Serial1.write((uint8_t)(len >> 8));
  Serial1.write((uint8_t)(ackCmd & 0xFF));
  Serial1.write((uint8_t)(ackCmd >> 8));
  Serial1.write((uint8_t)0x00);   // status OK
  Serial1.write((uint8_t)0x00);
  if (extraLen) Serial1.write(extra, extraLen);
  Serial1.write(tail, 4);
}

void handleCommand(const uint8_t* p, uint16_t len) {
  if (len < 2) return;
  uint16_t cmd = p[0] | (p[1] << 8);
  const uint8_t* v = p + 2;
  cmdCount++;
  switch (cmd) {
    case 0x00FF: {
      configMode = true;
      const uint8_t extra[4] = {0x01, 0x00, 0x40, 0x00};
      sendAck(cmd, extra, 4);
      Serial.println("Kommando: starta konfiguration");
      break;
    }
    case 0x00FE:
      configMode = false;
      sendAck(cmd, nullptr, 0);
      Serial.println("Kommando: avsluta konfiguration");
      break;
    case 0x0002:
      if (len >= 6) {
        cfgMaxDist = v[0]; cfgDir = v[1]; cfgMinSpeed = v[2]; cfgDelay = v[3];
      }
      sendAck(cmd, nullptr, 0);
      Serial.printf("Kommando: max %u m, riktning %u, min %u km/h, fordrojning %u s\n",
                    cfgMaxDist, cfgDir, cfgMinSpeed, cfgDelay);
      break;
    case 0x0003:
      if (len >= 4) { cfgTrig = v[0]; cfgSnr = v[1]; }
      sendAck(cmd, nullptr, 0);
      Serial.printf("Kommando: trigger %u, SNR %u\n", cfgTrig, cfgSnr);
      break;
    case 0x0012: {
      const uint8_t extra[4] = {cfgMaxDist, cfgDir, cfgMinSpeed, cfgDelay};
      sendAck(cmd, extra, 4);
      break;
    }
    default:
      sendAck(cmd, nullptr, 0);
      Serial.printf("Kommando: okant 0x%04X (svarar OK)\n", cmd);
      break;
  }
}

// Enkel parser för kommandoramar
uint8_t  cHead[4];
uint8_t  cBuf[64];
uint16_t cLen = 0, cPos = 0;
uint8_t  cState = 0, cLenPos = 0, cTail = 0;
const uint8_t CMD_HEAD[4] = {0xFD, 0xFC, 0xFB, 0xFA};
const uint8_t CMD_TAIL[4] = {0x04, 0x03, 0x02, 0x01};

void parseCmdByte(uint8_t c) {
  switch (cState) {
    case 0:
      memmove(cHead, cHead + 1, 3);
      cHead[3] = c;
      if (memcmp(cHead, CMD_HEAD, 4) == 0) { cState = 1; cLenPos = 0; memset(cHead, 0, 4); }
      break;
    case 1:
      if (cLenPos == 0) { cLen = c; cLenPos = 1; }
      else {
        cLen |= (uint16_t)c << 8;
        if (cLen > sizeof(cBuf)) { cState = 0; break; }
        cPos = 0; cTail = 0;
        cState = cLen ? 2 : 3;
      }
      break;
    case 2:
      cBuf[cPos++] = c;
      if (cPos >= cLen) { cState = 3; cTail = 0; }
      break;
    case 3:
      if (c != CMD_TAIL[cTail]) { cState = 0; break; }
      if (++cTail == 4) { handleCommand(cBuf, cLen); cState = 0; }
      break;
  }
}

// ------------------------------------------------------------
//  SCENARIER  –  körs i tur och ordning, sedan börjar det om
//  Varje händelse: efter X tiondels sekunder, en bil med
//  hastighet, riktning, start-avstånd, acceleration och sluthastighet.
//  (Skylten med standardinställningar: under 10 inget, grön till 59,
//   röd 60–64, blinkande röd från 65, blå 50-skylt högst var 30:e s.)
// ------------------------------------------------------------
struct Ev { uint16_t atDs; uint8_t speed; bool approach; uint8_t startDist; int8_t accel; uint8_t target; };
struct Scenario { const char* name; uint8_t pauseS; uint8_t n; Ev ev[4]; };

static const Scenario SCEN[] = {
  {"Lugn bil 35 km/h (grön)",                 8, 1, {{0, 35, true, 0, 0, 0}}},
  {"Bil precis under gränsen 48 km/h (grön)", 8, 1, {{0, 48, true, 0, 0, 0}}},
  {"Fortkörare 62 km/h (röd)",                8, 1, {{0, 62, true, 0, 0, 0}}},
  {"Kraftig fortkörare 82 km/h (blinkar)",    8, 1, {{0, 82, true, 0, 0, 0}}},
  {"Bromsar för skylten 70 -> 40 km/h",       8, 1, {{0, 70, true, 0, -12, 40}}},
  {"Gasar på 40 -> 75 km/h",                  8, 1, {{0, 40, true, 0, 10, 75}}},
  {"Två bilar tätt efter varandra 45 + 58",   8, 2, {{0, 45, true, 0, 0, 0}, {30, 58, true, 0, 0, 0}}},
  {"Kö: fyra bilar 30 km/h",                  8, 4, {{0, 30, true, 0, 0, 0}, {30, 32, true, 0, 0, 0},
                                                     {60, 29, true, 0, 0, 0}, {90, 31, true, 0, 0, 0}}},
  {"Möte: bil mot 55 + bil från 60",          8, 2, {{0, 55, true, 0, 0, 0}, {20, 60, false, 0, 0, 0}}},
  {"Bil FRÅN radarn 70 km/h (ska inte visas)",8, 1, {{0, 70, false, 0, 0, 0}}},
  {"Cykel 8 km/h (ska inte visas)",           5, 1, {{0, 8, true, 40, 0, 0}}},
  {"Moped 25 km/h nära (grön)",               8, 1, {{0, 25, true, 50, 0, 0}}},
  {"Lång paus 40 s (skylten släcks)",        40, 0, {}},
};
static const uint8_t SCEN_COUNT = sizeof(SCEN) / sizeof(SCEN[0]);

uint8_t  scenIdx     = 0;
uint8_t  scenEvNext  = 0;
uint32_t scenStartMs = 0;
uint32_t scenDoneMs  = 0;      // när sista bilen försvann (0 = inte än)
uint32_t scenRounds  = 0;

void stopAllCars() { for (auto& c : cars) c.active = false; }

void scenStart(uint8_t i) {
  scenIdx = i % SCEN_COUNT;
  scenEvNext = 0;
  scenStartMs = millis();
  scenDoneMs = 0;
  Serial.printf("Scenario %u/%u: %s\n", scenIdx + 1, SCEN_COUNT, SCEN[scenIdx].name);
}

bool anyCarActive() { for (auto& c : cars) if (c.active) return true; return false; }

void scenTick() {
  uint32_t now = millis();
  const Scenario& sc = SCEN[scenIdx];
  while (scenEvNext < sc.n && now - scenStartMs >= (uint32_t)sc.ev[scenEvNext].atDs * 100) {
    const Ev& e = sc.ev[scenEvNext++];
    addCar(e.speed, e.approach, e.startDist ? e.startDist : -1, (int8_t)random(-6, 7),
           e.accel, e.target);
  }
  if (scenEvNext < sc.n || anyCarActive()) return;
  if (scenDoneMs == 0) scenDoneMs = now;
  if (now - scenDoneMs >= (uint32_t)sc.pauseS * 1000) {
    if (scenIdx + 1 >= SCEN_COUNT) scenRounds++;
    scenStart(scenIdx + 1);
  }
}

void setMode(uint8_t m) {
  mode = m > 2 ? 0 : m;
  autoMode = (mode == 2);
  stopAllCars();
  if (mode == 1) scenStart(0);
  nextAutoMs = millis() + 1000;
  Serial.printf("Läge: %s\n", mode == 0 ? "STOPP" : mode == 1 ? "scenarier" : "slumpad trafik");
}

// ------------------------------------------------------------
//  WEBBSIDA FÖR ATT STYRA SIMULATORN
// ------------------------------------------------------------
static const char PAGE[] = R"HTML(<!doctype html>
<html lang="sv"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Radarsimulator</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--txt:#1b1d21;--mut:#6b7280;--acc:#7c3aed}
@media(prefers-color-scheme:dark){:root{--bg:#111318;--card:#1c1f26;--txt:#e8eaed;--mut:#9aa0a6;--acc:#a78bfa}}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,sans-serif;background:var(--bg);color:var(--txt)}
main{max-width:520px;margin:0 auto;padding:16px}.card{background:var(--card);border-radius:14px;padding:16px;margin-bottom:14px}
h1{font-size:18px;margin:4px 0 14px}h2{font-size:15px;margin:0 0 10px;color:var(--mut)}
.big{font-size:64px;font-weight:700;text-align:center;font-variant-numeric:tabular-nums}
.btn{display:block;width:100%;padding:12px;border:0;border-radius:10px;background:var(--acc);color:#fff;font-size:15px;margin-top:8px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:8px}
label{display:flex;justify-content:space-between;align-items:center;margin:8px 0;gap:10px}
input{font-size:15px;padding:6px;border-radius:8px;border:1px solid rgba(128,128,128,.4);background:var(--bg);color:var(--txt);width:110px}
.st{font-size:13px;color:var(--mut);line-height:1.6}
</style></head><body><main>
<h1>Radarsimulator (LD2451)</h1>
<div class="card"><div class="big" id="spd">0</div><p class="st" style="text-align:center">km/h skickas just nu</p>
<p class="st" id="st"></p></div>
<div class="card"><h2>Automatik</h2>
<div class="big" style="font-size:22px" id="mode">–</div>
<p class="st" id="scen" style="text-align:center"></p>
<div class="row"><button class="btn" onclick="md(1)">Kör scenarier</button><button class="btn" onclick="md(2)">Slumpad trafik</button></div>
<button class="btn" style="background:#dc2626" onclick="md(0)">STOPP</button>
<p class="st">Startar själv med scenarier när simulatorn får ström. BOOT-knappen på kortet växlar stopp / start.</p></div>
<div class="card"><h2>Skicka en bil</h2>
<label>Hastighet (km/h)<input id="v" type="number" value="55" min="1" max="120"></label>
<div class="row"><button class="btn" onclick="car(1)">Bil MOT radarn</button><button class="btn" onclick="car(0)">Bil FRÅN radarn</button></div>
<button class="btn" onclick="go('/two')">Två bilar tätt efter varandra</button>
<button class="btn" onclick="go('/slow')">Långsam bil (8 km/h)</button>
<button class="btn" onclick="go('/stop')">Stoppa all trafik</button></div>
<div class="card"><h2>Slumpad trafik</h2>
<label>Lägsta hastighet<input id="amin" type="number"></label>
<label>Högsta hastighet<input id="amax" type="number"></label>
<label>Sekunder mellan bilar (min)<input id="gmin" type="number"></label>
<label>Sekunder mellan bilar (max)<input id="gmax" type="number"></label>
<button class="btn" onclick="saveA()">Spara</button></div>
</main><script>
const $=id=>document.getElementById(id);
async function go(u){await fetch(u,{method:'POST'})}
function car(a){go('/car?v='+$('v').value+'&a='+a)}
async function md(m){await fetch('/mode?m='+m,{method:'POST'})}
async function saveA(){const q=new URLSearchParams({min:$('amin').value,max:$('amax').value,gmin:$('gmin').value,gmax:$('gmax').value});await fetch('/auto?'+q,{method:'POST'})}
async function st(first){try{const s=await(await fetch('/status')).json();$('spd').textContent=s.speed;
$('st').innerHTML='Bilar i rörelse: '+s.active+' · skickade totalt: '+s.sent+'<br>Radarinställning från loggern: max '+s.maxDist+' m, riktning '+['från','mot','båda'][s.dir]+', min '+s.minSpeed+' km/h<br>Kommandon mottagna: '+s.cmds;
$('mode').textContent=['STOPPAD','Kör scenarier','Slumpad trafik'][s.mode];
$('scen').textContent=s.mode==1?('Scenario '+s.si+' av '+s.sn+': '+s.scen+' · varv '+s.rounds):'';
if(first){$('amin').value=s.amin;$('amax').value=s.amax;$('gmin').value=s.gmin;$('gmax').value=s.gmax}}catch(e){}}
st(true);setInterval(st,300);
</script></body></html>)HTML";

void webStatus() {
  uint8_t active = 0;
  for (auto& c : cars) if (c.active) active++;
  char buf[420];
  snprintf(buf, sizeof(buf),
    "{\"mode\":%u,\"si\":%u,\"sn\":%u,\"scen\":\"%s\",\"rounds\":%lu,"
    "\"speed\":%u,\"active\":%u,\"sent\":%lu,\"maxDist\":%u,\"dir\":%u,\"minSpeed\":%u,"
    "\"cmds\":%lu,\"auto\":%s,\"amin\":%u,\"amax\":%u,\"gmin\":%u,\"gmax\":%u}",
    mode, scenIdx + 1, SCEN_COUNT, SCEN[scenIdx].name, (unsigned long)scenRounds,
    lastSpeedSent, active, (unsigned long)carsSent, cfgMaxDist, cfgDir > 2 ? 2 : cfgDir,
    cfgMinSpeed, (unsigned long)cmdCount, autoMode ? "true" : "false",
    autoMin, autoMax, autoGapMin, autoGapMax);
  web.send(200, "application/json", buf);
}

void webCar() {
  float v = web.hasArg("v") ? web.arg("v").toFloat() : 50;
  bool a  = web.hasArg("a") ? web.arg("a").toInt() == 1 : true;
  if (v < 1) v = 1;
  if (v > 200) v = 200;
  addCar(v, a, -1, (int8_t)random(-8, 9));
  web.send(200, "text/plain", "OK");
}

void webTwo() {
  // Två bilar ca 15 m efter varandra i samma riktning
  addCar(50, true, 110, -3);
  addCar(52, true, 125, 3);
  web.send(200, "text/plain", "OK");
}

void webSlow() {
  addCar(8, true, 40, 0);
  web.send(200, "text/plain", "OK");
}

void webStop() {
  setMode(0);
  web.send(200, "text/plain", "OK");
}

void webMode() {
  setMode(web.hasArg("m") ? web.arg("m").toInt() : 0);
  web.send(200, "text/plain", "OK");
}

void webAuto() {
  if (web.hasArg("min"))  autoMin    = constrain(web.arg("min").toInt(), 1, 200);
  if (web.hasArg("max"))  autoMax    = constrain(web.arg("max").toInt(), autoMin, 200);
  if (web.hasArg("gmin")) autoGapMin = constrain(web.arg("gmin").toInt(), 1, 3600);
  if (web.hasArg("gmax")) autoGapMax = constrain(web.arg("gmax").toInt(), autoGapMin, 3600);
  nextAutoMs = millis() + 1000;
  web.send(200, "text/plain", "OK");
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== RADARSIMULATOR " FW_VERSION " ===");
  randomSeed(esp_random());

  Serial1.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);

  uint64_t mac = ESP.getEfuseMac();
  snprintf(ssid, sizeof(ssid), "RadarSim-%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, WIFI_PASS, WIFI_CHANNEL);

  web.on("/",       HTTP_GET,  []() { web.send(200, "text/html; charset=utf-8", PAGE); });
  web.on("/status", HTTP_GET,  webStatus);
  web.on("/car",    HTTP_POST, webCar);
  web.on("/two",    HTTP_POST, webTwo);
  web.on("/slow",   HTTP_POST, webSlow);
  web.on("/stop",   HTTP_POST, webStop);
  web.on("/auto",   HTTP_POST, webAuto);
  web.on("/mode",   HTTP_POST, webMode);
  web.begin();

  Serial.printf("WiFi: \"%s\", losenord \"%s\", oppna http://%s\n",
                ssid, WIFI_PASS, WiFi.softAPIP().toString().c_str());
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(3000);                 // ge loggern tid att starta
  setMode(1);                  // kör scenarier direkt
}

void loop() {
  web.handleClient();

  while (Serial1.available()) parseCmdByte((uint8_t)Serial1.read());

  uint32_t now = millis();

  // BOOT-knappen: stopp <-> scenarier
  static bool lastBtn = true;
  static uint32_t btnMs = 0;
  bool btn = digitalRead(BUTTON_PIN);
  if (!btn && lastBtn && now - btnMs > 300) {
    btnMs = now;
    setMode(mode == 0 ? 1 : 0);
  }
  lastBtn = btn;

  if (mode == 1) scenTick();

  if (autoMode && now > nextAutoMs) {
    addCar(random(autoMin, autoMax + 1), true, -1, (int8_t)random(-8, 9));
    nextAutoMs = now + 1000UL * random(autoGapMin, autoGapMax + 1);
  }

  if (now - lastFrameMs >= 1000 / FRAME_HZ) {
    float dt = (now - lastFrameMs) / 1000.0f;
    if (dt > 0.5f) dt = 0.1f;
    lastFrameMs = now;
    stepCars(dt);
    if (!configMode) sendFrame();   // riktig radar skickar inga mål i konfig-läge
    // LED: grön = bil skickas, svagt blå = kör men ingen bil just nu, röd = stoppad
    if (mode == 0)          rgbLedWrite(LED_PIN, 20, 0, 0);
    else if (lastSpeedSent) rgbLedWrite(LED_PIN, 0, 25, 0);
    else                    rgbLedWrite(LED_PIN, 0, 0, 6);
  }

  delay(1);
}
