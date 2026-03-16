// /beacon-supermini/src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>  // from the ESP32 core (not external lib)
#include <esp_random.h>     // preferred on 3.x cores

#include <Adafruit_NeoPixel.h>
#include "battery_icons.h"
#include <Wire.h>
//#include <SPI.h>        // <-- add this line
//#define U8X8_USE_PINS 1
//#define U8X8_HAVE_HW_SPI 1
#include <U8g2lib.h>    // <-- must come after SPI


#define EFBOARD_SERIAL_DEVICE Serial    //!< Serial device to use (Serial or USBSerial)
#define EFBOARD_SERIAL_BAUD 115200         //!< Baudrate for the serial device


// ---------- OLED (SSD1306 128x64 I2C) ----------
#define OLED_ENABLE 1       // set 0 to compile without OLED
#define OLED_I2C_ADDR 0x3C  // common cheap-o SSD1306
#define OLED_SDA_PIN 12     // <<< set these to your Supermini’s I2C pins
#define OLED_SCL_PIN 13     // (if you don’t know, keep defaults & try)
#define OLED_I2C_HZ 400000

#if OLED_ENABLE
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE, /* clock=*/OLED_SCL_PIN, /* data=*/OLED_SDA_PIN);
#endif

// ---------- board wiring ----------
#define LED_PIN_WS2812 48
#define LED_COUNT 1

Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN_WS2812, NEO_GRB + NEO_KHZ800);
// BOOT button for “randomize ID at boot” (hold while powering)
#define PIN_BTN_BOOT 0  // typical on ESP32-S3 Supermini. change if needed.

// ---------- optional battery sense (off by default) ----------
#define PIN_VBAT_ADC 8  // set to an ADC1 pin if you add a divider, else -1
#define VBAT_RTOP_OHMS 100000.0f
#define VBAT_RBOT_OHMS 100000.0f
#define ADC_VREF 3.30f
#define ADC_MAX_COUNTS 4095.0f
#define VBAT_LOW_THRESHOLD_V 3.50f
#define VBAT_SAMPLE_INTERVAL_MS 10000UL

// ---------- beacon behavior ----------
#define BEACON_IS_STATIONARY 1
#define BEACON_TX_POWER_DBM 7
#define ADV_INTERVAL_MS 700
#define BEACON_ID_FIXED 0x00000000  // 0=derive from MAC

#define EF_BLEFH_MFGID 0x28EF
#define EF_BLEFH_VERSION 0x02
#define EF_BLEFH_TYPE_BADGE 'D'
#define EF_BLEFH_TYPE_BEACON 'B'
#define EF_BLEFH_V2_MINLEN 10
#define EF_BLEFH_F_CONNECTABLE (1u << 0)
#define EF_BLEFH_F_STATIONARY (1u << 1)
#define EF_BLEFH_F_LOWBATT (1u << 2)
#define EF_BLEFH_F_HINT_NAME (1u << 3)

#define BEACON_NAME_PREFIX "FOXHUNT"  // used for BLE name & OLED
#define BEACON_NAME "Test1"           // used for BLE name & OLED currently not used

// ---------- peer scan config for OLED HUD ----------
#define FH_MAX_PEERS 16
#define FH_STALE_MS 7000
#define FH_EMA_A 0.30f
#define FH_RSSI_MIN -90
#define FH_RSSI_MAX -40



// ---------- globals ----------
static uint32_t g_devId = 0;
static bool g_lowbatt = false;

// ---------- helpers ----------

struct FHPeer {
  bool used = false;
  uint32_t id = 0;
  int rssi = -127;  // smoothed
  int lastRaw = -127;
  uint32_t lastSeen = 0;  // millis()
  char type = 0;          // 'D' badge, 'B' beacon, '?' unknown
};

static FHPeer s_peers[FH_MAX_PEERS];

static bool fhFresh(const FHPeer& p) {
  return p.used && (millis() - p.lastSeen) <= FH_STALE_MS;
}

static int fhFindById(uint32_t id) {
  for (int i = 0; i < FH_MAX_PEERS; i++)
    if (s_peers[i].used && s_peers[i].id == id) return i;
  return -1;
}

static int fhFindOrAlloc(uint32_t id) {
  int i = fhFindById(id);
  if (i >= 0) return i;
  for (int k = 0; k < FH_MAX_PEERS; k++)
    if (!s_peers[k].used) {
      s_peers[k].used = true;
      s_peers[k].id = id;
      return k;
    }
  // replace stalest
  int worst = 0;
  uint32_t oldest = UINT32_MAX;
  for (int k = 0; k < FH_MAX_PEERS; k++)
    if (s_peers[k].lastSeen < oldest) {
      oldest = s_peers[k].lastSeen;
      worst = k;
    }
  s_peers[worst] = FHPeer{};
  s_peers[worst].used = true;
  s_peers[worst].id = id;
  return worst;
}

static uint8_t rssiToPx(int rssi, uint8_t maxW = 127) {
  if (rssi < FH_RSSI_MIN) rssi = FH_RSSI_MIN;
  if (rssi > FH_RSSI_MAX) rssi = FH_RSSI_MAX;
  float pct = (rssi - FH_RSSI_MIN) * (1.0f / (FH_RSSI_MAX - FH_RSSI_MIN));
  int w = (int)(pct * maxW + 0.5f);
  if (w < 0) w = 0;
  if (w > (int)maxW) w = maxW;
  return (uint8_t)w;
}

static uint32_t idFromMac() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  uint32_t id = (uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5];
  return id ? id : 0xEF28C0DE;
}

static String shortTail(uint32_t id) {
  char tail[5];
  snprintf(tail, sizeof(tail), "%04X", (uint16_t)(id & 0xFFFF));
  return String(tail);
}

static String makeName(uint32_t id) {
  String name = BEACON_NAME_PREFIX;
  name += "-";
  name += shortTail(id);
  return name;
}

class FHScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveManufacturerData()) return;

    // Arduino BLE returns Arduino String
    String md = dev.getManufacturerData();
    int mlen = md.length();
    if (mlen < 6) return;

    // Safe byte pointer (binary data ok) + use mlen for bounds
    const uint8_t* m = (const uint8_t*)md.c_str();

    // EF manufacturer?
    uint16_t mid = (uint16_t)m[0] | ((uint16_t)m[1] << 8);
    if (mid != EF_BLEFH_MFGID) return;

    uint32_t id = 0;
    char type = '?';

    // v2: [0..1]=mfg, [2]=ver, [3]=type, [4..7]=id
    if (mlen >= EF_BLEFH_V2_MINLEN && m[2] == EF_BLEFH_VERSION) {
      type = (char)m[3];
      memcpy(&id, m + 4, 4);
    } else {
      // v1 fallback: [2..5]=id
      memcpy(&id, m + 2, 4);
      type = '?';
    }

    // ignore self
    if (id == g_devId) return;

    int idx = fhFindOrAlloc(id);
    int rssi = dev.getRSSI();
    s_peers[idx].type = type;
    s_peers[idx].lastRaw = rssi;
    s_peers[idx].rssi = (s_peers[idx].rssi == -127)
                          ? rssi
                          : (int)(FH_EMA_A * rssi + (1.0f - FH_EMA_A) * s_peers[idx].rssi);
    s_peers[idx].lastSeen = millis();
  }
};

static FHScanCb s_scanCb;

static std::string buildV2(uint32_t id, char type, uint8_t flags, int8_t txpwr) {
  std::string md;
  md.push_back((char)(EF_BLEFH_MFGID & 0xFF));
  md.push_back((char)((EF_BLEFH_MFGID >> 8) & 0xFF));
  md.push_back((char)EF_BLEFH_VERSION);
  md.push_back((char)type);
  md.append(reinterpret_cast<const char*>(&id), 4);
  md.push_back((char)flags);
  md.push_back((char)txpwr);
  return md;
}

static float readVBAT() {
#if PIN_VBAT_ADC < 0
  return NAN;
#else
  uint16_t raw = analogRead(PIN_VBAT_ADC);
  float vadc = (raw / ADC_MAX_COUNTS) * ADC_VREF;
  float vbat = vadc * ((VBAT_RTOP_OHMS + VBAT_RBOT_OHMS) / VBAT_RBOT_OHMS);
  return vbat;
#endif
}

static int vbatToPercent(float v) {
  if (isnan(v) || v <= 0) return -1;  // unknown
  const float V_EMPTY = 3.30f;
  const float V_FULL = 4.20f;
  if (v <= V_EMPTY) return 0;
  if (v >= V_FULL) return 100;
  float pct = 100.0f * (v - V_EMPTY) / (V_FULL - V_EMPTY);
  return (int)(pct + 0.5f);
}
static const unsigned char* pickBatteryIcon(int pct, bool charging) {
  if (charging && pct >= 0) return image_battery_charging_bits;

  if (pct < 0) return image_no_battery_bits;  // unknown → show empty (or make a “?” icon)
  if (pct < 10) return image_battery_0_bits;
  if (pct < 20) return image_battery_10_bits;
  if (pct < 30) return image_battery_20_bits;
  if (pct < 40) return image_battery_30_bits;
  if (pct < 60) return image_battery_40_bits;
  if (pct < 70) return image_battery_60_bits;
  if (pct < 80) return image_battery_70_bits;
  if (pct < 90) return image_battery_80_bits;
  if (pct < 100) return image_battery_90_bits;
  return image_battery_full_2_bits;
}


static void wsLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t br = 24) {
  pixel.setBrightness(br);
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

static void startAdvertising() {
  uint8_t flags = 0;
  if (BEACON_IS_STATIONARY) flags |= EF_BLEFH_F_STATIONARY;
  flags |= EF_BLEFH_F_HINT_NAME;
  if (g_lowbatt) flags |= EF_BLEFH_F_LOWBATT;

  BLEAdvertising* adv = BLEDevice::getAdvertising();

  BLEAdvertisementData ad;

  // Build Manufacturer Specific Data payload
  std::string md = buildV2(g_devId, EF_BLEFH_TYPE_BEACON, flags, (int8_t)BEACON_TX_POWER_DBM);

  // Build full AD structure: [len][type=0xFF][payload...]
  std::string msd;
  msd.reserve(md.size() + 2);
  msd.push_back((char)(md.size() + 1));  // length = payload + type
  msd.push_back((char)0xFF);             // AD type: Manufacturer Specific (0xFF)
  msd.append(md);

  // Use the raw-bytes overload (binary-safe, no truncation on '\0')
  ad.addData(const_cast<char*>(msd.data()), msd.size());

  adv->setAdvertisementData(ad);

  BLEAdvertisementData sr;
  String nm = makeName(g_devId);
  sr.setName(nm.c_str());
  adv->setScanResponseData(sr);

  adv->start();
}

#if OLED_ENABLE
static void oledSplash() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x13_tf);
  u8g2.drawStr(0, 12, "EF28 Fox-Hunt Beacon");
  u8g2.drawHLine(0, 14, 127);
  u8g2.setFont(u8g2_font_7x14B_tf);
  String idLine = "ID 0x" + shortTail(g_devId);
  u8g2.drawStr(0, 32, idLine.c_str());
  u8g2.setFont(u8g2_font_6x13_tf);
  u8g2.drawStr(0, 48, "ADV v2  Type: B");
  u8g2.drawStr(60, 32, "TX:+7dBm");
  if (g_lowbatt) u8g2.drawStr(0, 64, "LOW BAT!");
  u8g2.sendBuffer();
}

static void oledStatus(float vbat) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_t0_12b_tf);
  u8g2.drawStr(0, 12, "EF28-BEACON");
  u8g2.setFont(u8g2_font_6x13_tf);
  String id = "ID: 0x" + shortTail(g_devId);
  u8g2.drawStr(0, 28, id.c_str());
#if PIN_VBAT_ADC >= 0
  char vb[24];
  snprintf(vb, sizeof(vb), "VBAT: %.2f V", isnan(vbat) ? 0.0f : vbat);
  u8g2.drawStr(0, 44, vb);
#endif
  u8g2.drawStr(0, 60, g_lowbatt ? "STATUS: LOWBATT" : "STATUS: OK");
  u8g2.sendBuffer();
}


#endif

void setup() {
  EFBOARD_SERIAL_DEVICE.begin(EFBOARD_SERIAL_BAUD);
  delay(50);
  WiFi.mode(WIFI_OFF);

  // WS2812
  //FastLED.addLeds<NEOPIXEL, LED_PIN_WS2812>(leds, LED_COUNT);
  wsLedColor(0, 0, 0, 0);

  // BOOT button
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  // I2C / OLED
#if OLED_ENABLE
  //Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_HZ);
  u8g2.begin();
  //u8g2.setContrast(255);
#endif

#if PIN_VBAT_ADC >= 0
  // analogSetPinAttenuation(PIN_VBAT_ADC, ADC_11db); // optional
#endif

  // Device ID
  g_devId = (BEACON_ID_FIXED != 0) ? (uint32_t)BEACON_ID_FIXED : idFromMac();
  if (digitalRead(PIN_BTN_BOOT) == LOW) { g_devId ^= esp_random(); }

  // Battery initial
#if PIN_VBAT_ADC >= 0
  float v0 = readVBAT();
  g_lowbatt = (v0 > 0 && v0 < VBAT_LOW_THRESHOLD_V);
  EFBOARD_SERIAL_DEVICE.printf("[BEACON] VBAT init: %.3f V low=%d\n", v0, g_lowbatt);
#else
  g_lowbatt = false;
#endif

  // BLE
  BLEDevice::init(makeName(g_devId).c_str());
  startAdvertising();

  EFBOARD_SERIAL_DEVICE.printf("[BEACON] ID=0x%08lX name=%s\n",
                (unsigned long)g_devId, makeName(g_devId).c_str());

  // LED & OLED splash
  if (g_lowbatt) wsLedColor(32, 0, 0, 24);
  else wsLedColor(0, 28, 32, 24);
  // Start very light, continuous scan (coexists with advertising)
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&s_scanCb, /*wantDuplicates=*/true);
  scan->setActiveScan(true);
  scan->setInterval(160);
  scan->setWindow(160);
  scan->start(1, false);  // non-blocking 1s slice; we’ll restart in loop()

#if OLED_ENABLE
  oledSplash();
#endif
}

void loop() {
  static uint32_t tBlink = 0, tBat = 0, tOled = 0;
  uint32_t now = millis();

  // heartbeat blink
  if (now - tBlink >= 1000) {
    tBlink = now;
    static bool hi = false;
    hi = !hi;
    if (g_lowbatt) wsLedColor(32, 0, 0, hi ? 28 : 10);
    else wsLedColor(0, 28, 32, hi ? 28 : 10);
  }

#if PIN_VBAT_ADC >= 0
  if (now - tBat >= VBAT_SAMPLE_INTERVAL_MS) {
    tBat = now;
    float v = readVBAT();
    bool low = (v > 0 && v < VBAT_LOW_THRESHOLD_V);
    if (low != g_lowbatt) {
      g_lowbatt = low;
      BLEDevice::getAdvertising()->stop();
      startAdvertising();  // update LOWBATT flag
      EFBOARD_SERIAL_DEVICE.printf("[BEACON] VBAT=%.3f V low=%d -> flags updated\n", v, g_lowbatt);
    }
  }
#endif

  // keep scan running in 1s slices without blocking
  if (!BLEDevice::getScan()->isScanning()) {
    BLEDevice::getScan()->start(1, false);
  }

#if OLED_ENABLE
  if (now - tOled >= 500) {
    tOled = now;

    // compute fresh peers & pick top 5 by RSSI
    int idxs[FH_MAX_PEERS];
    int n = 0;
    for (int i = 0; i < FH_MAX_PEERS; i++)
      if (fhFresh(s_peers[i])) idxs[n++] = i;
    // selection sort by RSSI (descending), but cap to 5
    int top = n < 16 ? n : 16;
    for (int a = 0; a < top; ++a) {
      int best = a;
      for (int b = a + 1; b < n; ++b)
        if (s_peers[idxs[b]].rssi > s_peers[idxs[best]].rssi) best = b;
      int t = idxs[a];
      idxs[a] = idxs[best];
      idxs[best] = t;
    }

    // count BADGEs (type == 'D')
    int badgeCount = 0;
    for (int i = 0; i < n; i++)
      if (s_peers[idxs[i]].type == EF_BLEFH_TYPE_BADGE) badgeCount++;

    u8g2.clearBuffer();
    // header
    u8g2.setFont(u8g2_font_t0_12b_tf);
    u8g2.drawStr(0, 12, "EF28-BEACON HUD");

    // line with counts
    u8g2.setFont(u8g2_font_6x13_tf);
    char line[32];
    snprintf(line, sizeof(line), "BADGES:%d  PEERS:%d", badgeCount, n);
    u8g2.drawStr(0, 26, line);

    // 1px bars for top 16 at y=34..42?
    uint8_t y = 34;
    for (int i = 0; i < top; ++i, ++y) {
      int idx = idxs[i];
      uint8_t w = rssiToPx(s_peers[idx].rssi, 127);
      // draw bar
      if (w) u8g2.drawHLine(0, y, w);
      // optional tiny type tag at right edge
      //char t[2] = { s_peers[idx].type ? s_peers[idx].type : '?', 0 };
      //u8g2.drawStr(10, y + 10, t);
    }

    // status line at bottom
    //u8g2.drawStr(0, 62, g_lowbatt ? "LOW BAT" : "OK");

    // Top-right battery indicator (or "OK" if no VBAT measurement)
#if PIN_VBAT_ADC >= 0
    // After drawing "EF28-BEACON HUD"
    u8g2.setFontMode(1);    // text alpha (keep, harmless)
    u8g2.setBitmapMode(1);  // transparent bitmap draw
    u8g2.setDrawColor(1);   // draw pixels as ON

#if PIN_VBAT_ADC >= 0
    float v_now = readVBAT();
    int pct = vbatToPercent(v_now);
    bool charging = false;  // TODO: set from a GPIO/charger IC if available
    const unsigned char* bmp = pickBatteryIcon(pct, charging);

    // Top-right: x = 128 - 24 = 104; y = 0
    u8g2.drawXBM(128 - BAT_ICON_W, 0, BAT_ICON_W, BAT_ICON_H, bmp);
#else
    // No VBAT measurement: show no battery on top-right
    u8g2.drawXBM(0, 0, 24, 16, image_no_battery_bits);
#endif
#else
    // No battery sensing: show USB Symbol
    u8g2.drawXBM(0, 2, 24, 11, image_UsbTree_bits);
#endif

    u8g2.sendBuffer();
  }
#endif

  delay(10);
}
