/*
 * ============================================================
 *  SKYLT  –  två P10-paneler (32x16, 1/4-scan, RGB, HUB75)
 *  Del av Radarlogger. Ersätter Pi Zero + radar_live.py.
 * ============================================================
 *
 *  Panelerna sitter som på Pi:n: två 32x16 i kedja, ihop 32x32,
 *  med "U-mapper" (panel 2 överst, panel 1 underst roterad 180°)
 *  och "multiplexing 4" (ZStripe 0,8).  Exakt samma pixelomräkning
 *  som hzeller-biblioteket görs här i koden, så att resultatet ska
 *  bli identiskt med Pi:n.  Blir bilden ändå fel kan man byta
 *  variant på webbsidan utan att flasha om (knappen "Nästa variant").
 *
 *  Visningen är densamma som radar_live.py:
 *    - nytt fordon: blå 50-skylt i 2 s (som högst var 30:e s)
 *    - sedan hastigheten: grön under "röd"-gränsen, röd över,
 *      blinkande röd över "blink"-gränsen
 *    - släcks när fordonet är borta
 *    - ljusstyrka dag / natt
 * ============================================================
 */
#pragma once
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <driver/gpio.h>
#include <rom/gpio.h>
#include <soc/gpio_sig_map.h>

// ------------------------------------------------------------
//  PINNAR  (ESP32-S3)  –  se kopplingsschemat i README
// ------------------------------------------------------------
#define HUB_R1   4
#define HUB_G1   5
#define HUB_B1   6
#define HUB_R2   7
#define HUB_G2   15
#define HUB_B2   16
#define HUB_A    8
#define HUB_B    9
#define HUB_C    13      // används inte av 1/4-scan, hålls låg
#define HUB_D    1       // används inte av 1/4-scan, hålls låg
#define HUB_E    -1
#define HUB_LAT  10
#define HUB_OE   11
#define HUB_CLK  12

#define PANEL_EN_PIN 21  // HÖG = panelerna får ström (MOSFET). -1 = ingen MOSFET

#define VIS_W 32         // synlig yta
#define VIS_H 32

// ------------------------------------------------------------
//  INSTÄLLNINGAR FÖR SKYLTEN (sparas i NVS)
// ------------------------------------------------------------
struct __attribute__((packed)) SignSettings {
  uint8_t enabled;       // 1 = skylten används
  uint8_t limit;         // hastighetsgräns som visas på blå skylt (20–90)
  uint8_t limitSecs;     // hur länge blå skylt visas, s (0 = aldrig)
  uint8_t limitPause;    // minst så här många s mellan blå skyltar
  uint8_t green;         // under detta visas inget
  uint8_t red;           // från detta: röd siffra
  uint8_t blink;         // från detta: blinkande röd
  uint8_t dayBright;     // % 5–100
  uint8_t nightBright;   // % 5–100
  uint8_t nightFrom;     // timme 0–23
  uint8_t nightTo;       // timme 0–23
  uint8_t holdDecis;     // siffran står kvar så här länge efter bilen är borta
  uint8_t mapCombo;      // 0–15: ZStripe-variant (bit 0–1), byt paneler (bit 2), spegla (bit 3)
  uint8_t rotate;        // 1 = vrid hela bilden 180°
  uint8_t clkPhase;      // 1 = standard, 0 = omvänd klockfas (kräver omstart)
  uint8_t powerSave;     // s utan bil innan panelerna stängs av (0 = alltid på)
  uint8_t smiley;        // 0 = av, 1 = glad gubbe vid OK fart, 2 = glad + ledsen gubbe
  uint8_t speedDecis;    // gubbläge: så länge siffran visas (tiondels s)
  uint8_t faceDecis;     // gubbläge: så länge gubben visas (tiondels s)
};
#define SIGN_SIZE_V1 16  // storlek före gubben (0.4.0)

// Pi-värdena i Home Assistant: grön 10, röd 60, blink 65, 100 % dag, 20 % natt
static const SignSettings DEFAULT_SIGN = {1, 50, 2, 30, 10, 60, 65, 100, 20, 21, 6, 10, 0, 0, 1, 0, 2, 15, 10};
SignSettings sign;

// ------------------------------------------------------------
//  TECKEN (samma bitmappar som radar_live.py)
// ------------------------------------------------------------
static const uint16_t BIG_5[20] = {0xFFF,0xFFF,0xC00,0xC00,0xC00,0xC00,0xC00,0xC00,0xC00,
                                   0xFFF,0xFFF,0x003,0x003,0x003,0x003,0x003,0x003,0x003,0xFFF,0xFFF};
static const uint16_t BIG_0[20] = {0x7FE,0xFFF,0xC03,0xC03,0xC03,0xC03,0xC03,0xC03,0xC03,0xC03,
                                   0xC03,0xC03,0xC03,0xC03,0xC03,0xC03,0xC03,0xC03,0xFFF,0x7FE};

static const uint8_t SMALL_DIG[10][14] = {
  {0x3C,0x7E,0xE7,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xE7,0x7E,0x3C}, // 0
  {0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E}, // 1
  {0x3C,0x7E,0xE7,0xC3,0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC0,0xFF,0xFF}, // 2
  {0x3C,0x7E,0xE7,0xC3,0x03,0x07,0x3E,0x3E,0x07,0x03,0xC3,0xE7,0x7E,0x3C}, // 3
  {0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xFF,0xFF,0x03,0x03,0x03,0x03,0x03,0x03}, // 4
  {0xFF,0xFF,0xC0,0xC0,0xC0,0xFC,0xFE,0x07,0x03,0x03,0x03,0xC7,0x7E,0x3C}, // 5
  {0x3C,0x7E,0xE0,0xC0,0xC0,0xFC,0xFE,0xC7,0xC3,0xC3,0xC3,0xE7,0x7E,0x3C}, // 6
  {0xFF,0xFF,0x03,0x03,0x06,0x06,0x0C,0x0C,0x18,0x18,0x30,0x30,0x60,0x60}, // 7
  {0x3C,0x7E,0xE7,0xC3,0xC3,0x7E,0x7E,0xC3,0xC3,0xC3,0xC3,0xE7,0x7E,0x3C}, // 8
  {0x3C,0x7E,0xE7,0xC3,0xC3,0xC3,0xE7,0x7F,0x3F,0x03,0x03,0xE7,0x7E,0x3C}, // 9
};
static const uint8_t CH_K[12] = {0xC3,0xC6,0xCC,0xD8,0xF0,0xF0,0xF0,0xD8,0xCC,0xC6,0xC3,0xC3};
static const uint8_t CH_M[12] = {0xC3,0xC3,0xE7,0xE7,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3};
static const uint8_t CH_H[12] = {0xC3,0xC3,0xC3,0xC3,0xFF,0xFF,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3};

// ------------------------------------------------------------
//  RAMBUFFERT  (32x32, ritas först här och skickas sedan till panelerna)
// ------------------------------------------------------------
struct RGB { uint8_t r, g, b; };
static RGB fb[VIS_H][VIS_W];

static inline void fbClear() { memset(fb, 0, sizeof(fb)); }
static inline void fbSet(int x, int y, RGB c) {
  if (x >= 0 && x < VIS_W && y >= 0 && y < VIS_H) fb[y][x] = c;
}

static const RGB C_BLUE  = {0, 60, 140};
static const RGB C_WHITE = {255, 255, 255};
static const RGB C_GREEN = {0, 255, 0};
static const RGB C_RED   = {255, 0, 0};

// Stora siffror 12x20.  5 och 0 är exakt Pi:ns, övriga ritas i samma stil.
static uint16_t bigRow(int digit, int row) {
  if (digit == 5) return BIG_5[row];
  if (digit == 0) return BIG_0[row];
  // segment:          a     b     c     d     e     f     g
  static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  uint8_t s = SEG[digit];
  bool a = s & 0x01, b = s & 0x02, c = s & 0x04, d = s & 0x08, e = s & 0x10, f = s & 0x20, g = s & 0x40;
  const uint16_t L = 0xC00, R = 0x003, FULL = 0xFFF;
  if (row <= 1)  return a ? FULL : (f ? L : 0) | (b ? R : 0);
  if (row <= 8)  return (f ? L : 0) | (b ? R : 0);
  if (row <= 10) return g ? FULL : ((f || e) ? L : 0) | ((b || c) ? R : 0);
  if (row <= 17) return (e ? L : 0) | (c ? R : 0);
  return d ? FULL : (e ? L : 0) | (c ? R : 0);
}

static void drawBig(int digit, int x0, int y0, RGB c) {
  for (int r = 0; r < 20; r++) {
    uint16_t bits = bigRow(digit, r);
    for (int i = 0; i < 12; i++) if (bits & (0x800 >> i)) fbSet(x0 + i, y0 + r, c);
  }
}

static void drawSmall8(const uint8_t* rows, int n, int x0, int y0, RGB c) {
  for (int r = 0; r < n; r++)
    for (int i = 0; i < 8; i++) if (rows[r] & (0x80 >> i)) fbSet(x0 + i, y0 + r, c);
}

// Blå skylt med vit ram och gränsen i vitt (radar_live.py: draw_50_sign)
static void renderLimitSign(uint8_t limit) {
  for (int y = 0; y < VIS_H; y++) for (int x = 0; x < VIS_W; x++) fb[y][x] = C_BLUE;
  for (int i = 0; i < VIS_W; i++) {
    fbSet(i, 0, C_WHITE); fbSet(i, 1, C_WHITE); fbSet(i, 30, C_WHITE); fbSet(i, 31, C_WHITE);
    fbSet(0, i, C_WHITE); fbSet(1, i, C_WHITE); fbSet(30, i, C_WHITE); fbSet(31, i, C_WHITE);
  }
  const int CW = 12, SP = 2;
  const int SX = (32 - (2 * CW + SP)) / 2;   // 3
  const int SY = (32 - 20) / 2;              // 6
  drawBig((limit / 10) % 10, SX, SY, C_WHITE);
  drawBig(limit % 10, SX + CW + SP, SY, C_WHITE);
}

// Hastighet + KM/H (radar_live.py: draw_speed)
static void renderSpeed(int speed, RGB c) {
  fbClear();
  char s[8];
  snprintf(s, sizeof(s), "%d", speed > 999 ? 999 : (speed < 0 ? 0 : speed));
  int n = strlen(s);
  const int cw = 8, sp = 2;
  int w = n * cw + (n - 1) * sp;
  int x = (32 - w) / 2;
  for (int i = 0; i < n; i++) {
    drawSmall8(SMALL_DIG[s[i] - '0'], 14, x, 1, c);
    x += cw + sp;
  }
  const int y = 19;
  drawSmall8(CH_K, 12, 0, y, c);
  drawSmall8(CH_M, 12, 9, y, c);
  for (int i = 1; i <= 12; i++) fbSet(18 + i / 3, 31 - i, c);   // snedstreck
  drawSmall8(CH_H, 12, 24, y, c);
}

// Gubbe: rund ram, två ögon och en mun (glad = leende, ledsen = nedåtböjd)
static void renderFace(bool happy, RGB c) {
  fbClear();
  for (int y = 0; y < VIS_H; y++)
    for (int x = 0; x < VIS_W; x++) {
      float dx = x - 15.5f, dy = y - 15.5f;
      float d2 = dx * dx + dy * dy;
      if (d2 >= 12.9f * 12.9f && d2 <= 15.2f * 15.2f) fbSet(x, y, c);
    }
  for (int y = 9; y <= 12; y++) {                // ögon 3x4
    for (int x = 10; x <= 12; x++) fbSet(x, y, c);
    for (int x = 19; x <= 21; x++) fbSet(x, y, c);
  }
  for (int x = 8; x <= 23; x++) {                // mun, 2 pixlar tjock
    float dx = x - 15.5f;
    float k = dx * dx / 14.0f;
    int y = happy ? (int)lroundf(23.0f - k) : (int)lroundf(19.0f + k);
    fbSet(x, y, c);
    fbSet(x, y + 1, c);
  }
}

// ------------------------------------------------------------
//  PIXELOMRÄKNING  synlig 32x32 -> fysisk 128x8
//  (kopia av hzeller: U-mapper + ZStripe-multiplexing)
// ------------------------------------------------------------
// variant 0 = Pi:ns ZStripe(0,8), 1 = ZStripe(8,0) (HUB75-bibliotekets egen 1/4-scan),
// 2 och 3 = samma men med speglade 8-pixelsblock
static const int8_t ZS_EVEN[4] = {0, 8, 0, 8};
static const int8_t ZS_ODD[4]  = {8, 0, 8, 0};

static void mapPixel(int x, int y, int& px, int& py) {
  if (sign.rotate) { x = VIS_W - 1 - x; y = VIS_H - 1 - y; }

  // U-mapper (hzeller, chain 2, parallel 1): 32x32 -> 64x16
  int lx, ly;
  if (y < 16) { lx = x + 32; ly = y; }
  else        { lx = 31 - x; ly = 31 - y; }

  if (sign.mapCombo & 0x04) lx = (lx + 32) % 64;        // byt plats på panelerna

  // ZStripe (hzeller multiplexing=4 är ZStripe 0,8): 32x16 -> 64x8 per panel
  int panel = lx / 32, x0 = lx % 32, y0 = ly;
  int v = sign.mapCombo & 0x03;
  int odd = (y0 / 4) % 2;
  int shift = odd ? ZS_ODD[v] : ZS_EVEN[v];
  int mx = x0 + ((x0 + shift) / 8) * 8;
  int my = (y0 % 4) + 4 * (y0 / 8);
  if (v >= 2) mx = (mx & ~7) | (7 - (mx & 7));

  px = panel * 64 + mx;
  py = my;
  if (sign.mapCombo & 0x08) px = 127 - px;               // spegla kedjan
}

static String mapComboText() {
  char b[128];
  snprintf(b, sizeof(b), "Variant %u: ZStripe(%d,%d)%s%s%s%s", sign.mapCombo,
           ZS_EVEN[sign.mapCombo & 3], ZS_ODD[sign.mapCombo & 3],
           (sign.mapCombo & 2) ? ", speglade block" : "",
           (sign.mapCombo & 4) ? ", paneler bytta" : "",
           (sign.mapCombo & 8) ? ", speglad" : "",
           sign.rotate ? ", vriden 180°" : "");
  return String(b);
}

// ------------------------------------------------------------
//  PANELDRIVARE
// ------------------------------------------------------------
MatrixPanel_I2S_DMA* hub = nullptr;
bool     panelsOn      = false;     // ström + DMA igång
bool     pinsParked    = false;
uint8_t  curBrightPct  = 0;

static const int8_t HUB_DATA_PINS[13] = {HUB_R1, HUB_G1, HUB_B1, HUB_R2, HUB_G2, HUB_B2,
                                         HUB_LAT, HUB_OE, HUB_A, HUB_B, HUB_C, HUB_D, HUB_E};

static void pushFrame() {
  if (!hub || !panelsOn) return;
  for (int y = 0; y < VIS_H; y++)
    for (int x = 0; x < VIS_W; x++) {
      int px, py;
      mapPixel(x, y, px, py);
      hub->drawPixelRGB888(px, py, fb[y][x].r, fb[y][x].g, fb[y][x].b);
    }
}

// Pinnarna låga när panelerna är strömlösa, annars matas panelen bakvägen
static void parkPins() {
  if (!hub || pinsParked) return;
  hub->stopDMAoutput();
  for (int i = 0; i < 13; i++) {
    if (HUB_DATA_PINS[i] < 0) continue;
    esp_rom_gpio_connect_out_signal(HUB_DATA_PINS[i], SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction((gpio_num_t)HUB_DATA_PINS[i], GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)HUB_DATA_PINS[i], 0);
  }
  esp_rom_gpio_connect_out_signal(HUB_CLK, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction((gpio_num_t)HUB_CLK, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)HUB_CLK, 0);
  pinsParked = true;
}

static void unparkPins() {
  if (!hub || !pinsParked) return;
  for (int i = 0; i < 13; i++) {
    if (HUB_DATA_PINS[i] < 0) continue;
    esp_rom_gpio_connect_out_signal(HUB_DATA_PINS[i], LCD_DATA_OUT0_IDX + i, false, false);
  }
  esp_rom_gpio_connect_out_signal(HUB_CLK, LCD_PCLK_IDX, sign.clkPhase ? true : false, false);
  hub->resumeDMAoutput();
  pinsParked = false;
}

static void panelPower(bool on) {
  if (on == panelsOn) return;
  if (on) {
#if PANEL_EN_PIN >= 0
    digitalWrite(PANEL_EN_PIN, HIGH);
#endif
    unparkPins();
    panelsOn = true;
    curBrightPct = 0;               // sätts om i signTick
  } else {
    if (hub) hub->clearScreen();
    parkPins();
#if PANEL_EN_PIN >= 0
    digitalWrite(PANEL_EN_PIN, LOW);
#endif
    panelsOn = false;
  }
}

void clampSign(SignSettings& s) {
  if (s.limit < 20 || s.limit > 90) s.limit = 50;
  if (s.limitSecs > 10) s.limitSecs = 10;
  if (s.dayBright < 5) s.dayBright = 5;     if (s.dayBright > 100) s.dayBright = 100;
  if (s.nightBright < 5) s.nightBright = 5; if (s.nightBright > 100) s.nightBright = 100;
  if (s.nightFrom > 23) s.nightFrom = 21;
  if (s.nightTo > 23) s.nightTo = 6;
  if (s.mapCombo > 15) s.mapCombo = 0;
  if (s.red < s.green) s.red = s.green;
  if (s.blink < s.red) s.blink = s.red;
  if (s.smiley > 2) s.smiley = 2;
  if (s.speedDecis < 5) s.speedDecis = 5;
  if (s.faceDecis < 5) s.faceDecis = 5;
  s.enabled  = s.enabled ? 1 : 0;
  s.rotate   = s.rotate ? 1 : 0;
  s.clkPhase = s.clkPhase ? 1 : 0;
}

void loadSign() {
  size_t len = prefs.getBytesLength("sign");
  sign = DEFAULT_SIGN;
  if (len == sizeof(SignSettings)) prefs.getBytes("sign", &sign, sizeof(sign));
  else if (len == SIGN_SIZE_V1) prefs.getBytes("sign", &sign, SIGN_SIZE_V1);   // behåll gamla värden
  clampSign(sign);
}

void saveSign() { prefs.putBytes("sign", &sign, sizeof(sign)); }

void signInit() {
#if PANEL_EN_PIN >= 0
  pinMode(PANEL_EN_PIN, OUTPUT);
  digitalWrite(PANEL_EN_PIN, LOW);
#endif
  if (!sign.enabled) { Serial.println("Skylt: avstängd i inställningarna"); return; }

  HUB75_I2S_CFG::i2s_pins pins = {HUB_R1, HUB_G1, HUB_B1, HUB_R2, HUB_G2, HUB_B2,
                                  HUB_A, HUB_B, HUB_C, HUB_D, HUB_E, HUB_LAT, HUB_OE, HUB_CLK};
  // Fysiskt: varje 32x16-panel med 1/4-scan är 64 pixlar lång och 8 rader hög
  HUB75_I2S_CFG cfg(64, 8, 2, pins);
  cfg.clkphase = sign.clkPhase;
  cfg.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  cfg.min_refresh_rate = 120;
  hub = new MatrixPanel_I2S_DMA(cfg);
  if (!hub->begin()) {
    Serial.println("Skylt: FEL, panelerna kunde inte startas (minne/pinnar)");
    delete hub; hub = nullptr;
    return;
  }
  hub->setBrightness8(255);
  hub->clearScreen();
  pinsParked = false;
  panelsOn = false;
#if PANEL_EN_PIN >= 0
  digitalWrite(PANEL_EN_PIN, HIGH);
#endif
  panelsOn = true;
  Serial.printf("Skylt: igång, %s\n", mapComboText().c_str());
}

// ------------------------------------------------------------
//  VISNINGSLOGIK
// ------------------------------------------------------------
enum SignState { SIGN_IDLE, SIGN_LIMIT, SIGN_SPEED };
SignState signState      = SIGN_IDLE;
uint32_t  signStateMs    = 0;
uint32_t  lastLimitMs    = 0;
uint32_t  lastSeenMs     = 0;    // senaste gången ett fordon syntes
uint32_t  lastActiveMs   = 0;    // senaste gången något visades
int       shownSpeed     = 0;
uint8_t   testMode       = 0;    // 0 = av, 1 = färgrutor, 2 = blå skylt, 3 = 88 km/h, 4 = rad för rad
uint32_t  testStartMs    = 0;
uint32_t  lastDrawMs     = 0;
uint32_t  lastFrameHash  = 0;

static uint32_t frameHash() {
  uint32_t h = 2166136261u;
  const uint8_t* p = (const uint8_t*)fb;
  for (size_t i = 0; i < sizeof(fb); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static bool isNight() {
  if (!timeSynced) return false;              // okänd tid: dag-ljus
  time_t lt = time(nullptr) + tzOffsetMin * 60;
  struct tm tmv;
  gmtime_r(&lt, &tmv);
  int h = tmv.tm_hour;
  if (sign.nightFrom == sign.nightTo) return false;
  if (sign.nightFrom < sign.nightTo) return h >= sign.nightFrom && h < sign.nightTo;
  return h >= sign.nightFrom || h < sign.nightTo;
}

static void renderTest(uint32_t now) {
  uint32_t t = now - testStartMs;
  switch (testMode) {
    case 1:   // fyra färgrutor + vit ram: röd uppe-vänster, grön uppe-höger, blå nere-vänster, gul nere-höger
      for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
        RGB c = (y < 16) ? (x < 16 ? C_RED : C_GREEN) : (x < 16 ? (RGB){0, 0, 255} : (RGB){255, 200, 0});
        if (x == 0 || y == 0 || x == 31 || y == 31) c = C_WHITE;
        fb[y][x] = c;
      }
      for (int i = 2; i < 30; i++) fbSet(i, i, C_WHITE);   // diagonal uppe-vänster -> nere-höger
      break;
    case 2:
      renderLimitSign(sign.limit);
      break;
    case 3:
      renderSpeed(88, ((t / 1000) % 2) ? C_RED : C_GREEN);
      break;
    case 5:
      renderFace(((t / 1500) % 2) == 0, ((t / 1500) % 2) == 0 ? C_GREEN : C_RED);
      break;
    case 4: { // en vit rad som vandrar nedåt, en ny rad varje halv sekund
      fbClear();
      int row = (t / 500) % 32;
      for (int x = 0; x < 32; x++) fbSet(x, row, C_WHITE);
      fbSet(0, 0, C_RED);   // markerar övre vänstra hörnet
      break;
    }
  }
}

// Anropas från loop()
void signTick() {
  if (!hub) return;
  uint32_t now = millis();
  if (now - lastDrawMs < 40) return;      // 25 bilder/s räcker gott
  lastDrawMs = now;

  // Fordon som kommer mot skylten
  bool approaching = lastPrimary.dir == TARGET_DIR_APPROACH;
  int  speed = (lastTargets > 0 && approaching) ? lastPrimary.speed : 0;
  if (speed > 0 && speed < sign.green) speed = 0;   // för långsamt, visas inte

  if (testMode) {
    renderTest(now);
    lastActiveMs = now;
  } else {
    if (speed > 0) lastSeenMs = now;

    switch (signState) {
      case SIGN_IDLE:
        if (speed > 0) {
          if (sign.limitSecs > 0 && (lastLimitMs == 0 || now - lastLimitMs >= (uint32_t)sign.limitPause * 1000)) {
            signState = SIGN_LIMIT;
            lastLimitMs = now;
          } else {
            signState = SIGN_SPEED;
          }
          signStateMs = now;
          shownSpeed = speed;
        }
        break;
      case SIGN_LIMIT:
        if (speed > shownSpeed) shownSpeed = speed;    // högsta under skyltvisningen
        if (now - signStateMs >= (uint32_t)sign.limitSecs * 1000) {
          signState = SIGN_SPEED;
          signStateMs = now;
        }
        break;
      case SIGN_SPEED:
        if (speed > 0) shownSpeed = speed;
        else if (now - lastSeenMs > (uint32_t)sign.holdDecis * 100) signState = SIGN_IDLE;
        break;
    }

    if (signState == SIGN_LIMIT) {
      renderLimitSign(sign.limit);
    } else if (signState == SIGN_SPEED && shownSpeed > 0) {
      bool ok = shownSpeed < sign.red;
      // Gubbläge: växla mellan siffran och gubben
      bool face = false;
      if (sign.smiley == 2 || (sign.smiley == 1 && ok)) {
        uint32_t period = ((uint32_t)sign.speedDecis + sign.faceDecis) * 100;
        face = ((now - signStateMs) % period) >= (uint32_t)sign.speedDecis * 100;
      }
      if (ok) {
        if (face) renderFace(true, C_GREEN); else renderSpeed(shownSpeed, C_GREEN);
      } else if (shownSpeed < sign.blink) {
        if (face) renderFace(false, C_RED); else renderSpeed(shownSpeed, C_RED);
      } else {
        bool on = ((now / 400) % 2) == 0;              // 0,4 s på / 0,4 s av
        if (!on) fbClear();
        else if (face) renderFace(false, C_RED);
        else renderSpeed(shownSpeed, C_RED);
      }
    } else {
      fbClear();
    }
    if (signState != SIGN_IDLE) lastActiveMs = now;
  }

  // Ström till panelerna
  bool wantOn = testMode || signState != SIGN_IDLE || sign.powerSave == 0 ||
                now - lastActiveMs < (uint32_t)sign.powerSave * 1000;
  if (wantOn && !panelsOn) { panelPower(true); lastFrameHash = 0; }
  if (!wantOn && panelsOn && sign.powerSave > 0) { panelPower(false); return; }

  // Ljusstyrka
  uint8_t pct = isNight() ? sign.nightBright : sign.dayBright;
  if (pct != curBrightPct) {
    curBrightPct = pct;
    hub->setBrightness8((uint8_t)((uint16_t)pct * 255 / 100));
  }

  uint32_t h = frameHash();
  if (h != lastFrameHash) {
    lastFrameHash = h;
    pushFrame();
  }
}

void signSetTest(uint8_t m) {
  testMode = m > 5 ? 0 : m;
  testStartMs = millis();
  lastFrameHash = 0;
}

void signNextCombo() {
  sign.mapCombo = (sign.mapCombo + 1) % 16;
  saveSign();
  lastFrameHash = 0;
}
