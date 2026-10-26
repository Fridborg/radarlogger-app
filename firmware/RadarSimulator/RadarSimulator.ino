/*
 * ============================================================
 *  RADARSIMULATOR  –  låtsas vara en HLK-LD2451
 *  Version 0.1.0
 * ============================================================
 *
 *  Kör på den ANDRA ESP32-S3:an. Den skickar exakt samma
 *  dataramar som en riktig LD2451, så Radarloggern tror att
 *  den pratar med en riktig radar. Bra för att testa hela
 *  kedjan vid skrivbordet: tolkning, räkning, logg, webbsida
 *  och Bluetooth.
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

#define FW_VERSION    "0.1.0"
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
};
#define MAX_CARS 4
SimCar cars[MAX_CARS];

bool     autoMode   = true;
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
void addCar(float speed, bool approach, float startDist = -1, int8_t angle = 0) {
  for (auto& c : cars) {
    if (!c.active) {
      c.active   = true;
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
<div class="card"><h2>Skicka en bil</h2>
<label>Hastighet (km/h)<input id="v" type="number" value="55" min="1" max="120"></label>
<div class="row"><button class="btn" onclick="car(1)">Bil MOT radarn</button><button class="btn" onclick="car(0)">Bil FRÅN radarn</button></div>
<button class="btn" onclick="go('/two')">Två bilar tätt efter varandra</button>
<button class="btn" onclick="go('/slow')">Långsam bil (8 km/h)</button>
<button class="btn" onclick="go('/stop')">Stoppa all trafik</button></div>
<div class="card"><h2>Automatisk trafik</h2>
<label>På<input id="auto" type="checkbox" style="width:auto"></label>
<label>Lägsta hastighet<input id="amin" type="number"></label>
<label>Högsta hastighet<input id="amax" type="number"></label>
<label>Sekunder mellan bilar (min)<input id="gmin" type="number"></label>
<label>Sekunder mellan bilar (max)<input id="gmax" type="number"></label>
<button class="btn" onclick="saveA()">Spara</button></div>
</main><script>
const $=id=>document.getElementById(id);
async function go(u){await fetch(u,{method:'POST'})}
function car(a){go('/car?v='+$('v').value+'&a='+a)}
async function saveA(){const q=new URLSearchParams({on:$('auto').checked?1:0,min:$('amin').value,max:$('amax').value,gmin:$('gmin').value,gmax:$('gmax').value});await fetch('/auto?'+q,{method:'POST'})}
async function st(first){try{const s=await(await fetch('/status')).json();$('spd').textContent=s.speed;
$('st').innerHTML='Bilar i rörelse: '+s.active+' · skickade totalt: '+s.sent+'<br>Radarinställning från loggern: max '+s.maxDist+' m, riktning '+['från','mot','båda'][s.dir]+', min '+s.minSpeed+' km/h<br>Kommandon mottagna: '+s.cmds;
if(first){$('auto').checked=s.auto;$('amin').value=s.amin;$('amax').value=s.amax;$('gmin').value=s.gmin;$('gmax').value=s.gmax}}catch(e){}}
st(true);setInterval(st,300);
</script></body></html>)HTML";

void webStatus() {
  uint8_t active = 0;
  for (auto& c : cars) if (c.active) active++;
  char buf[260];
  snprintf(buf, sizeof(buf),
    "{\"speed\":%u,\"active\":%u,\"sent\":%lu,\"maxDist\":%u,\"dir\":%u,\"minSpeed\":%u,"
    "\"cmds\":%lu,\"auto\":%s,\"amin\":%u,\"amax\":%u,\"gmin\":%u,\"gmax\":%u}",
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
  for (auto& c : cars) c.active = false;
  web.send(200, "text/plain", "OK");
}

void webAuto() {
  if (web.hasArg("on"))   autoMode   = web.arg("on").toInt() == 1;
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
  web.begin();

  Serial.printf("WiFi: \"%s\", losenord \"%s\", oppna http://%s\n",
                ssid, WIFI_PASS, WiFi.softAPIP().toString().c_str());
  nextAutoMs = millis() + 3000;
}

void loop() {
  web.handleClient();

  while (Serial1.available()) parseCmdByte((uint8_t)Serial1.read());

  uint32_t now = millis();

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
    rgbLedWrite(LED_PIN, 0, lastSpeedSent ? 25 : 0, 0);
  }

  delay(1);
}
