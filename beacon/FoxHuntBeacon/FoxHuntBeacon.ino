// /beacon-supermini/src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>   // from the ESP32 core (not external lib)

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <U8g2lib.h>

// ---------- board wiring ----------
#define LED_PIN_WS2812    48
#define LED_COUNT         1


Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN_WS2812, NEO_GRB + NEO_KHZ800);
// BOOT button for “randomize ID at boot” (hold while powering)
#define PIN_BTN_BOOT      0     // typical on ESP32-S3 Supermini. change if needed.

// ---------- OLED (SSD1306 128x64 I2C) ----------
#define OLED_ENABLE       1     // set 0 to compile without OLED
#define OLED_I2C_ADDR     0x3C  // common cheap-o SSD1306
#define OLED_SDA_PIN      12     // <<< set these to your Supermini’s I2C pins
#define OLED_SCL_PIN      13     // (if you don’t know, keep defaults & try)
#define OLED_I2C_HZ       400000

#if OLED_ENABLE
// Full-frame buffer renderer, SSD1306 128x64, hardware I2C, no reset line
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ OLED_SCL_PIN, /* data=*/ OLED_SDA_PIN
);
#endif

// ---------- optional battery sense (off by default) ----------
#define PIN_VBAT_ADC            -1   // set to an ADC1 pin if you add a divider, else -1
#define VBAT_RTOP_OHMS          100000.0f
#define VBAT_RBOT_OHMS          100000.0f
#define ADC_VREF                3.30f
#define ADC_MAX_COUNTS          4095.0f
#define VBAT_LOW_THRESHOLD_V    3.50f
#define VBAT_SAMPLE_INTERVAL_MS 10000UL

// ---------- beacon behavior ----------
#define BEACON_IS_STATIONARY    1
#define BEACON_TX_POWER_DBM     7
#define ADV_INTERVAL_MS         700
#define BEACON_ID_FIXED         0x00000000   // 0=derive from MAC

#define EF_BLEFH_MFGID          0x28EF
#define EF_BLEFH_VERSION        0x02
#define EF_BLEFH_TYPE_BADGE     'D'
#define EF_BLEFH_TYPE_BEACON    'B'
#define EF_BLEFH_V2_MINLEN      10
#define EF_BLEFH_F_CONNECTABLE  (1u<<0)
#define EF_BLEFH_F_STATIONARY   (1u<<1)
#define EF_BLEFH_F_LOWBATT      (1u<<2)
#define EF_BLEFH_F_HINT_NAME    (1u<<3)

// ---------- globals ----------
static uint32_t g_devId = 0;
static bool g_lowbatt = false;

// ---------- helpers ----------
static uint32_t idFromMac() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  uint32_t id = (uint32_t)mac[2]<<24 | (uint32_t)mac[3]<<16 | (uint32_t)mac[4]<<8 | mac[5];
  return id ? id : 0xEF28C0DE;
}

static String shortTail(uint32_t id) {
  char tail[5]; snprintf(tail, sizeof(tail), "%04X", (uint16_t)(id & 0xFFFF));
  return String(tail);
}

static String makeName(uint32_t id) {
  return "EF28-B-" + shortTail(id);
}

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

static void wsLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t br=24) {
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
  u8g2.drawStr(0, 48, "ADV v2  Type: B  TX:+7dBm");
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
  Serial.begin(115200);
  delay(50);
  WiFi.mode(WIFI_OFF);

  // WS2812
  //FastLED.addLeds<NEOPIXEL, LED_PIN_WS2812>(leds, LED_COUNT);
  wsLedColor(0,0,0,0);

  // BOOT button
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  // I2C / OLED
#if OLED_ENABLE
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_HZ);
  u8g2.begin();
  u8g2.setContrast(255);
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
  Serial.printf("[BEACON] VBAT init: %.3f V low=%d\n", v0, g_lowbatt);
#else
  g_lowbatt = false;
#endif

  // BLE
  BLEDevice::init(makeName(g_devId).c_str());
  startAdvertising();

  Serial.printf("[BEACON] ID=0x%08lX name=%s\n",
                (unsigned long)g_devId, makeName(g_devId).c_str());

  // LED & OLED splash
  if (g_lowbatt) wsLedColor(32,0,0,24); else wsLedColor(0,28,32,24);
#if OLED_ENABLE
  oledSplash();
#endif
}

void loop() {
  static uint32_t tBlink=0, tBat=0, tOled=0;
  uint32_t now = millis();

  // heartbeat blink
  if (now - tBlink >= 1000) {
    tBlink = now;
    static bool hi=false; hi = !hi;
    if (g_lowbatt) wsLedColor(32,0,0, hi?28:10);
    else           wsLedColor(0,28,32, hi?28:10);
  }

#if PIN_VBAT_ADC >= 0
  if (now - tBat >= VBAT_SAMPLE_INTERVAL_MS) {
    tBat = now;
    float v = readVBAT();
    bool low = (v > 0 && v < VBAT_LOW_THRESHOLD_V);
    if (low != g_lowbatt) {
      g_lowbatt = low;
      BLEDevice::getAdvertising()->stop();
      startAdvertising(); // update LOWBATT flag
      Serial.printf("[BEACON] VBAT=%.3f V low=%d -> flags updated\n", v, g_lowbatt);
    }
  }
#endif

#if OLED_ENABLE
  if (now - tOled >= 500) {
    tOled = now;
    float v = NAN;
  #if PIN_VBAT_ADC >= 0
    v = readVBAT();
  #endif
    oledStatus(v);
  }
#endif

  delay(10);
}
