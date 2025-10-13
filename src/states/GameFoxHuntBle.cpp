// MIT License
//
// Fox-hunt (BLE RSSI) state for EF28 badge.
//
// This file implements the methods declared for `struct GameFoxHuntBle` in FSMState.h.
// No BLE headers are pulled into FSMState.h to keep the core header clean.

#include <Config.h>
#include <EFLed.h>
#include <EFBoard.h>
#include <EFLogging.h>
#include "FSMState.h"
#include <EFTouch.h>
#ifdef HasDisplay
    #include <EFDisplay.h>
#endif


#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEAdvertisedDevice.h>
#include <BLEUtils.h>
#include "esp_bt.h"

// ------------------ Config ------------------
#define EF_BLEFH_MFGID       0x28EF   // little-endian in ManufacturerData
#define EF_BLEFH_MAX_PEERS   16
#define EF_BLEFH_STALE_MS    7000 //1500
#define EF_BLEFH_PURGE_MS    30000
#define EF_BLEFH_RSSI_MIN    -90
#define EF_BLEFH_RSSI_MAX    -40
#define EF_BLEFH_EMA_A       0.30f    // RSSI smoothing

// derive a 32-bit Badge ID from MAC (or change to your own global)
static uint32_t ef_defaultBadgeIdFromMac() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  uint32_t id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
              | ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
  if (id == 0) id = 0xEF28C0DE;
  return id;
}

static TaskHandle_t s_scanTask = nullptr;
static volatile bool s_scanTaskRun = false;
// --- debug counters / timing ---
static volatile uint32_t s_seenCallbacks = 0; // already there (keeps incrementing in callback)
static volatile uint32_t s_scanCycles    = 0; // counts 5s scan windows completed
static uint32_t s_lastHudMs  = 0;             // last time we refreshed HUD
static uint32_t s_lastCbMs   = 0;             // last time a callback fired

// --- modes (menus) ---
enum ViewMode : uint8_t { VIEW_TRACK = 0, VIEW_COUNT = 1 };
static ViewMode s_view = VIEW_TRACK;

// quick colors
//static inline CRGB C_BLUE()  { return CRGB(0, 0, 180); }
static inline CRGB C_TEAL(uint8_t v=80) { return CRGB(0, v, 100); }
static inline CRGB C_GREEN(uint8_t v=180){ return CRGB(0, v, 0); }

// blink bookkeeping
static uint32_t s_lastMuzzleBlinkMs = 0;


// ---------------- internal model ----------------
struct FHPeer {
  bool used = false;
  uint32_t id = 0;
  BLEAddress addr = BLEAddress((uint8_t*)"\0\0\0\0\0\0");
  int rssi = -127;       // smoothed
  int lastRaw = -127;    // last raw
  uint32_t lastSeen = 0; // millis()
};

// File-scope state (keeps header simple)
static uint32_t s_myBadgeId = 0;
static bool     s_lockActive = false;
static uint32_t s_lockedBadgeId = 0;
static int      s_cursor = -1;

static FHPeer   s_peers[EF_BLEFH_MAX_PEERS];
static int      s_sortedIdx[EF_BLEFH_MAX_PEERS];

bool GameFoxHuntBle::shouldBeRemembered() {
	return true;
}

// ---------------- helpers ----------------
static bool fresh(const FHPeer& p) {
  return p.used && (millis() - p.lastSeen) <= EF_BLEFH_STALE_MS;
}
static void prune() {
  uint32_t now = millis();
  for (auto &p : s_peers) {
    if (p.used && now - p.lastSeen > EF_BLEFH_PURGE_MS) p.used = false;
  }
}
static int findById(uint32_t id) {
  for (int i=0;i<EF_BLEFH_MAX_PEERS;i++)
    if (s_peers[i].used && s_peers[i].id == id) return i;
  return -1;
}
static int findOrAlloc(uint32_t id, const BLEAddress& addr) {
  int i = findById(id);
  if (i >= 0) return i;

  for (int k=0;k<EF_BLEFH_MAX_PEERS;k++) if (!s_peers[k].used) {
    s_peers[k].used = true; s_peers[k].id = id; s_peers[k].addr = addr;
    s_peers[k].rssi = -127; s_peers[k].lastRaw = -127; s_peers[k].lastSeen = millis();
    return k;
  }
  // replace stalest
  int worst = 0; uint32_t oldest = UINT32_MAX;
  for (int k=0;k<EF_BLEFH_MAX_PEERS;k++) if (s_peers[k].lastSeen < oldest) {
    oldest = s_peers[k].lastSeen; worst = k;
  }
  s_peers[worst] = FHPeer{};
  s_peers[worst].used = true; s_peers[worst].id = id; s_peers[worst].addr = addr;
  s_peers[worst].rssi = -127; s_peers[worst].lastRaw = -127; s_peers[worst].lastSeen = millis();
  return worst;
}
static int strongestFresh() {
  int best=-1, bestR=-999;
  for (int i=0;i<EF_BLEFH_MAX_PEERS;i++)
    if (fresh(s_peers[i]) && s_peers[i].rssi > bestR) { best = i; bestR = s_peers[i].rssi; }
  return best;
}
static int freshCount() {
  int c=0; for (int i=0;i<EF_BLEFH_MAX_PEERS;i++) if (fresh(s_peers[i])) c++;
  return c;
}
static int fillSortedByRssi(int outIdx[], int maxOut) {
  int n=0;
  for (int i=0;i<EF_BLEFH_MAX_PEERS && n<maxOut;i++) if (fresh(s_peers[i])) outIdx[n++] = i;
  for (int a=0;a<n;a++) for (int b=a+1;b<n;b++)
    if (s_peers[outIdx[b]].rssi > s_peers[outIdx[a]].rssi) { int t=outIdx[a]; outIdx[a]=outIdx[b]; outIdx[b]=t; }
  return n;
}
static uint8_t rssiToPercent(int rssi) {
  if (rssi < EF_BLEFH_RSSI_MIN) rssi = EF_BLEFH_RSSI_MIN;
  if (rssi > EF_BLEFH_RSSI_MAX) rssi = EF_BLEFH_RSSI_MAX;
  float pct = (rssi - EF_BLEFH_RSSI_MIN) * (100.0f / (EF_BLEFH_RSSI_MAX - EF_BLEFH_RSSI_MIN));
  return (uint8_t)(pct + 0.5f);
}

static void showPercent(uint8_t pct) {
  uint8_t N = (pct * EFLED_EFBAR_NUM) / 100;
  for (uint8_t i=0;i<EFLED_EFBAR_NUM;i++)
    EFLed.setEFBar(i, (i < N) ? CRGB(0,100,0) : CRGB(25,0,0));
}

static void showCount(uint8_t count) {
  uint8_t N = (count > EFLED_EFBAR_NUM) ? EFLED_EFBAR_NUM : count;
  for (uint8_t i=0;i<EFLED_EFBAR_NUM;i++)
    EFLed.setEFBar(i, (i < N) ? CRGB(0,50,100) : CRGB(25,0,0));
}

// show state via dragon LEDs (no display needed)
static void applyStateIndicators(bool targetFresh) {
  // clear first
  EFLed.setDragonMuzzle(CRGB::Black);
  EFLed.setDragonCheek(CRGB::Black);
  EFLed.setDragonEarTop(CRGB::Black);
  EFLed.setDragonEarBottom(CRGB::Black);

  if (s_lockActive) {
    // LOCKED: eye green (bright if fresh), nose solid teal
    EFLed.setDragonEye(targetFresh ? C_GREEN(180) : C_GREEN(60));
    EFLed.setDragonNose(C_TEAL(80));
  } else if (s_view == VIEW_TRACK) {
    // TRACK: eye blue, nose pulse to show scanning
    EFLed.setDragonEye(CRGB(0, 0, 180));
    uint8_t v = 40 + (uint8_t)((sinf(millis()/600.0f)*0.5f+0.5f)*60);
    EFLed.setDragonNose(C_TEAL(v));
  } else { // VIEW_COUNT
    // COUNT: ears on as a “mode flag”, nose slow pulse
    EFLed.setDragonEarTop(CRGB(0, 0, 180));
    EFLed.setDragonEarBottom(CRGB(200,0,0));
    uint8_t v = 30 + (uint8_t)((sinf(millis()/900.0f)*0.5f+0.5f)*30);
    EFLed.setDragonNose(C_TEAL(v));
    EFLed.setDragonEye(CRGB::Black);
  }

  // auto-clear muzzle blink after ~120 ms
  if (millis() - s_lastMuzzleBlinkMs > 120) {
    EFLed.setDragonMuzzle(CRGB::Black);
  }
}

// call this when a (re)lock happens
static void blinkMuzzle() {
  s_lastMuzzleBlinkMs = millis();
  EFLed.setDragonMuzzle(CRGB::White);
}


// Adapter so we don’t need BLE callbacks in the header
class FHScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveManufacturerData()) return;
    std::string md = dev.getManufacturerData();
    if (md.size() < 6) return;

    uint16_t mid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
    if (mid != EF_BLEFH_MFGID) return;

    uint32_t id; memcpy(&id, md.data()+2, 4);
    if (id == s_myBadgeId) return; // skip self

    int rssi = dev.getRSSI();
    BLEAddress addr = dev.getAddress();

    int idx = findOrAlloc(id, addr);
    uint32_t now = millis();

    s_peers[idx].lastRaw = rssi;
    s_peers[idx].rssi = (s_peers[idx].rssi == -127)
                        ? rssi
                        : (int)(EF_BLEFH_EMA_A * rssi + (1.0f - EF_BLEFH_EMA_A) * s_peers[idx].rssi);
    s_peers[idx].lastSeen = now;
    s_seenCallbacks++;
    s_lastCbMs = millis();
  }
};
static FHScanCb s_scanCb;

static void startAdvertising() {
  std::string md;
  md.push_back((char)(EF_BLEFH_MFGID & 0xFF));
  md.push_back((char)((EF_BLEFH_MFGID >> 8) & 0xFF));
  md.append((char*)&s_myBadgeId, 4);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  BLEAdvertisementData ad;
  ad.setManufacturerData(md);
  adv->setAdvertisementData(ad);
  adv->setScanResponse(false);
  adv->start();
}
static void bleScanTask(void*){
  BLEScan* scan = BLEDevice::getScan();
  // setup once
  scan->setActiveScan(true);
  scan->setInterval(160);
  scan->setWindow(160);
  //scan->setAdvertisedDeviceCallbacks(&s_scanCb);
  scan->setAdvertisedDeviceCallbacks(&s_scanCb, /*wantDuplicates=*/true);


  // loop short blocking scans so we can stop promptly
  while (s_scanTaskRun) {
  scan->start(1 /*seconds*/, false /*blocking*/); // was 5
  s_scanCycles++;
  scan->clearResults();
  vTaskDelay(pdMS_TO_TICKS(5));
}
  vTaskDelete(nullptr);
}

static void startScanning() {
  if (s_scanTask) return; // already running
  s_seenCallbacks = 0;
  s_scanTaskRun = true;
  xTaskCreatePinnedToCore(
    bleScanTask, "BLEScanTask",
    4096, nullptr, 1, &s_scanTask,
    0 /* Core 0 with BT controller */
  );
  LOG_INFO("[FoxHunt] scan task spawned\r\n");
}

static void stopBLE() {
  auto adv  = BLEDevice::getAdvertising(); if (adv)  adv->stop();
  // stop the scan task cleanly
  s_scanTaskRun = false;
  if (s_scanTask) {
    // let the 5s scan window finish; worst-case wait 5s
    vTaskDelete(s_scanTask);  // if you prefer immediate, uncomment; otherwise let it self-delete
    s_scanTask = nullptr;
  }
  // BLEDevice::deinit(true); // keep disabled unless you must reclaim BT RAM
}

// ---------------- GameFoxHuntBle method implementations ----------------
const char* GameFoxHuntBle::getName() { return "GameFoxHuntBle"; }
bool GameFoxHuntBle::shouldBeRemembered() { return true; }

void GameFoxHuntBle::entry() {
  LOG_INFO("[FoxHunt] enter\r\n");
  WiFi.mode(WIFI_OFF);
  this->tick = 0;
  EFLed.clear();
  EFLed.setDragonNose(CRGB(0,50,100));
  delay(60);
  EFLed.setDragonNose(CRGB(0,0,0));
  delay(60);
  EFLed.setDragonNose(CRGB(0,50,100));
  delay(60);
  EFLed.setDragonNose(CRGB(0,0,0));
  delay(60);
  EFLed.setDragonNose(CRGB(0,50,100));
  delay(60);
  EFLed.setDragonNose(CRGB(0,0,0));
  delay(60);

  if (s_myBadgeId == 0) s_myBadgeId = ef_defaultBadgeIdFromMac();
  LOGF_INFO("[FoxHunt] myBadgeId=0x%08lX\r\n", (unsigned long)s_myBadgeId);

  BLEDevice::init("EF28");
  LOG_INFO("[FoxHunt] BLE inited\r\n");

  startAdvertising();
  LOG_INFO("[FoxHunt] advertising\r\n");

  startScanning();
  LOG_INFO("[FoxHunt] scan task requested\r\n");
}

void GameFoxHuntBle::exit() {
  stopBLE();
  EFLed.clear();
  LOG_INFO("[FoxHunt] exit\r\n");
}

void GameFoxHuntBle::run() {
  prune();

  // --- 1 Hz HUD + serial snapshot ---
  uint32_t now = millis();
  if (now - s_lastHudMs >= 1000) {
    int idxStrong = strongestFresh();
    int freshCnt  = freshCount();

    // pull & reset callback counter atomically-enough for our purposes
    uint32_t cb = s_seenCallbacks;
    s_seenCallbacks = 0;

    // build a short, dense HUD line
    // ex: "P:3 RSSI:-58 L:ABCD Cb/s:12"
    String hud = "P:" + String(freshCnt) + " ";

    if (idxStrong >= 0) {
      hud += "RSSI:" + String(s_peers[idxStrong].rssi);
    } else {
      hud += "RSSI:--";
    }

    if (s_lockActive) {
      // show last 16 bits of locked badge id to keep it short
      uint16_t tail = (uint16_t)(s_lockedBadgeId & 0xFFFF);
      char buf[8]; snprintf(buf, sizeof(buf), "%04X", tail);
      hud += " L:" + String(buf);
    } else {
      hud += " L:--";
    }

    hud += " Cb/s:" + String(cb);

    // on-screen (uses your new helper)
    #ifdef HasDisplay
      EFDisplay.setHUDEnabled(true);

      String line0 = s_lockActive ? "LOCKED" : "TRACK";
      line0 += String(" P:") + String(freshCnt);

      String line1;
      if (idxStrong >= 0) line1 = "RSSI:" + String(s_peers[idxStrong].rssi);
      else                line1 = "RSSI:--";

      String line2;
      if (s_lockActive) {//tracking locked on specifig tag
        char buf[8]; snprintf(buf, sizeof(buf), "%04X", (uint16_t)(s_lockedBadgeId & 0xFFFF));
        line2 = "TGT:" + String(buf);
      } else {
        line2 = "TGT:--";
      }

      String line3 = "Cb/s:" + String(cb);

      EFDisplay.setHUDLine(0, line0);
      EFDisplay.setHUDLine(1, line1);
      EFDisplay.setHUDLine(2, line2);
      EFDisplay.setHUDLine(3, line3);
    #endif

    // serial snapshot
    LOGF_INFO("[FoxHunt] peers=%d strongest=%d rssi=%d locked=%s cbps=%lu scans=%lu\n",
              freshCnt,
              idxStrong,
              (idxStrong >= 0 ? s_peers[idxStrong].rssi : -127),
              (s_lockActive ? "yes" : "no"),
              (unsigned long)cb,
              (unsigned long)s_scanCycles);

    s_lastHudMs = now;
  }

  // (optional) tiny heartbeat on the nose so you see run() ticking
  EFLed.setDragonNose(CRGB(0,50,50));

  // --- existing LED mapping (mode-aware) ---
  bool targetFresh = false;
  int idx = -1;

  if (s_lockActive) {
    int j = findById(s_lockedBadgeId);
    targetFresh = (j >= 0 && fresh(s_peers[j]));
    if (targetFresh) idx = j;
  }

  if (!s_lockActive) {
    if (s_view == VIEW_TRACK) {
      // default: show strongest proximity (what you wanted)
      int s = strongestFresh();
      if (s >= 0) showPercent(rssiToPercent(s_peers[s].rssi));
      else        showCount((uint8_t)freshCount());
    } else { // VIEW_COUNT
      showCount((uint8_t)freshCount());
    }
  } else {
    // LOCKED: always show locked proximity (or 0 if stale)
    if (idx >= 0) showPercent(rssiToPercent(s_peers[idx].rssi));
    else          showPercent(0);
  }

// State indicators on dragon LEDs
applyStateIndicators(targetFresh);


  // keep your display animations alive
  #ifdef HasDisplay
    EFDisplay.loop();
  #endif

  this->tick++;
}

// touch handlers

// Quick tap = lock next target (cycle & lock)
std::unique_ptr<FSMState> GameFoxHuntBle::touchEventFingerprintRelease() {
  int n = fillSortedByRssi(s_sortedIdx, EF_BLEFH_MAX_PEERS);
  if (n > 0) {
    s_cursor = (s_cursor + 1) % n;
    s_lockedBadgeId = s_peers[s_sortedIdx[s_cursor]].id;
    s_lockActive = true;
    s_view = VIEW_TRACK;   // show proximity immediately
    LOGF_INFO("[FoxHunt] Locked 0x%08lX\r\n", (unsigned long)s_lockedBadgeId);
  } else {
    LOG_INFO("[FoxHunt] No peers to lock\r\n");
  }
  return nullptr;
}

// Shortpress: unlocking lock track strongest again (keeps UX clean)
std::unique_ptr<FSMState> GameFoxHuntBle::touchEventFingerprintShortpress() {
  s_lockActive = false;
  LOG_INFO("[FoxHunt] Unlocked");
  return nullptr;
}

// Hold = exit to main menu
std::unique_ptr<FSMState> GameFoxHuntBle::touchEventFingerprintLongpress() {
  s_lockActive = false;
  s_cursor = -1;
  #ifdef HasDisplay
    EFDisplay.loop();
    EFDisplay.setHUDEnabled(false);
    EFDisplay.setHUDLine(0, "");
    EFDisplay.setHUDLine(1, "");
    EFDisplay.setHUDLine(2, "");
    EFDisplay.setHUDLine(3, "");
  #endif
  return std::make_unique<MenuMain>();
}

// -------- Nose --------

// Quick tap = toggle view (TRACK <-> COUNT)
std::unique_ptr<FSMState> GameFoxHuntBle::touchEventNoseRelease() {
  s_view = (s_view == VIEW_TRACK) ? VIEW_COUNT : VIEW_TRACK;
  LOGF_INFO("[FoxHunt] View -> %s\r\n", (s_view == VIEW_TRACK) ? "TRACK" : "COUNT");
  return nullptr;
}

// Shortpress: treat al long press
std::unique_ptr<FSMState> GameFoxHuntBle::touchEventNoseShortpress() {
  return this->touchEventNoseLongpress();
}

std::unique_ptr<FSMState> GameFoxHuntBle::touchEventNoseLongpress() {
// Hold = unlock (stay in current view)
  s_lockActive = false;
  s_cursor = -1;
  LOG_INFO("[FoxHunt] Unlock\r\n");
  return nullptr;
}
// -------- All --------

// Hold both = toggle lock
std::unique_ptr<FSMState> GameFoxHuntBle::touchEventAllLongpress() {
  s_lockActive = !s_lockActive;
  if (!s_lockActive) s_lockedBadgeId = 0;
  LOGF_INFO("[FoxHunt] Toggle lock -> %d\r\n", (int)s_lockActive);
  return nullptr;
}
