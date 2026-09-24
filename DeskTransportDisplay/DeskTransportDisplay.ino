#include "DeskDisplay.h"

// =====================================================================
//  Train Bus Weather Signboard  -  firmware v1.0.0
//  Copyright (c) 2026 Sam (haldarsaurav). All rights reserved.
//  No use, copying, modification or redistribution - and no reproduction
//  with AI tools - without written permission. See LICENSE.
//
//  DeskTransportDisplay.ino - globals, boot, setup(), loop(), MODE button.
//
//  Live Freising departures, city buses and weather on a desk-sized
//  departure board. One MODE button, no cloud service of its own.
//
//  Screens (MODE tap = next):
//    1-4  trains from Freising station, 6 per page (as many pages as needed)
//    5    Bahnhof Stadtbus (town buses only), 10 rows
//    6    P+R-Platz, 10 rows
//    7    weather now + next 12 hours
//    8    next 3 days + 30-day temperature graph
//
//  How it fits together
//    loop()    draws and reads the button. Priority 2. Never blocks on a
//              socket, never serves web pages in normal running.
//    netTask   fetches the four sources on its own FreeRTOS task at
//              priority 1, so it only ever gets the CPU the loop leaves idle.
//    portal    only exists in setup mode (MODE held 3 s). Then the loop
//              serves it and nothing else runs.
//
//  Tabs: DeskDisplay.h (types, constants, declarations), Util, Net, Draw,
//  Portal. See README.md for wiring, board settings and troubleshooting.
// =====================================================================

// ---- the shared state declared extern in DeskDisplay.h ----------------
LGFX lcd;
LGFX_Sprite scratch(&lcd);
bool scratchReady = false;

WebServer server(80);
Preferences prefs;
Settings settings;
Departure trains[Config::MAX_TRAINS];
Departure busRows[Config::BUS_STOP_COUNT][Config::BUS_MAX_ITEMS];
WeatherData weather;

uint8_t trainCount = 0;
uint8_t delayHist[DELAY_HIST] = {0};
uint8_t delayHistCount = 0;
float    tempHigh[TEMP_DAYS];
uint8_t  tempHighCount = 0;
uint32_t tempHistoryAt = 0;
uint8_t busRowCount[Config::BUS_STOP_COUNT] = {0};
uint16_t screenIndex = 0;
uint32_t lastPageAt = 0;
uint32_t lastRefreshAt = 0;
uint32_t lastGoodDataAt = 0;    // last cycle that got anything at all
uint32_t staleRecoveryAt = 0;   // when the soft Wi-Fi re-join was tried
uint32_t lastWifiAttemptAt = 0;
uint32_t wifiLostAt = 0;
String statusText = "Fetching live data";
// The sky behind the weather screens, set before either one draws. The icon
// punches its crescent in this colour, so it cannot be a local.
uint16_t weatherBg = Config::WEATHER_BLUE;
String portalReason;
String wifiFailReason;
volatile int apClients = 0;

Marquee marquees[MAX_MARQUEE];
uint8_t marqueeCount = 0;
uint32_t screenShownAt = 0;
uint32_t lastMarqueeStepAt = 0;

bool setupOnlyMode = false;
bool webServerStarted = false;
bool setupHeldAtBoot = false;   // MODE held 3 s while setup() was running

int16_t  healthX = -1, healthY = -1, healthW = 152;
uint16_t healthBg = 0, healthFg = 0;
uint32_t lastHealthPaintAt = 0;
uint8_t  healthPhase = 0;
// Non-zero only for the length of a page transition. See renderScreen().
uint8_t  revealStep = 0;

int16_t  clockX = -1, clockY = -1;
uint8_t  clockFont = 21;
uint16_t clockFg = 0, clockBg = 0;
String   clockShown;

SemaphoreHandle_t dataMutex = nullptr;
TaskHandle_t      netTaskHandle = nullptr;

Departure netStage[Config::MAX_TRAINS];   // scratch, network task only
volatile bool netBusy    = false;   // a fetch is in flight right now
volatile bool netIdle    = true;    // the task is provably not in a call
volatile bool netSuspend = false;   // portal asked the task to stand down
volatile uint8_t dataDirty = 0;    // DIRTY_* bits: which feeds landed
volatile uint32_t cycleCount = 0;   // completed refresh cycles since boot

volatile bool     btnDown   = false;
volatile uint32_t btnDownAt = 0;
volatile uint32_t btnUpAt   = 0;
volatile bool     btnTap    = false;

// ---------------------------------------------------------------------
// The boot splash. Kept deliberately static: one title, one status line,
// and the MODE hint. Nothing on it animates, so nothing on it flickers.
// ---------------------------------------------------------------------
void drawBootSplash(const char *line) {
  const int W = lcd.width(), H = lcd.height();
  lcd.startWrite();
  lcd.fillScreen(Config::DB_BLUE);
  lcd.fillRect(0, 0, W, 30, Config::DB_HEADER);
  lcd.fillRect(0, 0, W, 2, Config::DB_HEADER_HI);
  txt("Departure Board", W / 2, 15, 22, D_MC, Config::BLACK, Config::DB_HEADER);
  txt(line, W / 2, H / 2 - 6, 24, D_MC, Config::DB_YELLOW, Config::DB_BLUE);
  txt("hold MODE 3 s for setup", W / 2, H - 14, 21,
      D_MC, Config::GREY, Config::DB_BLUE);
  txt(String("v") + Config::FIRMWARE_VERSION, W - 6, 15, 21,
      D_MR, Config::BLACK, Config::DB_HEADER);
  lcd.endWrite();
}

// Changes only the status line of the splash.
static void bootStatus(const char *line) {
  const int W = lcd.width(), H = lcd.height();
  lcd.fillRect(0, H / 2 - 22, W, 32, Config::DB_BLUE);
  txt(line, W / 2, H / 2 - 6, 24, D_MC, Config::DB_YELLOW, Config::DB_BLUE);
}

// Watches MODE while setup() is busy, for up to forMs (0 = one look).
// Returns true once the button has been held down continuously for 3 s.
// The hold is tracked across calls, so it keeps counting through the Wi-Fi
// join. Feeds the watchdog while it waits.
bool bootHoldForSetup(uint32_t forMs) {
  static uint32_t downSince = 0;
  static bool hinted = false;
  const int W = lcd.width(), H = lcd.height();
  const uint32_t started = millis();
  do {
    if (modePinDown()) {
      if (!downSince) downSince = millis() | 1;
      const uint32_t held = millis() - downSince;
      if (!hinted && held >= Config::HOLD_HINT_MS) {
        hinted = true;
        lcd.fillRect(0, H - 26, W, 26, Config::DB_BLUE);
        txt("keep holding for setup...", W / 2, H - 14, 22,
            D_MC, Config::DB_YELLOW, Config::DB_BLUE);
      }
      if (held >= Config::HOLD_FOR_SETUP_MS) return true;
    } else if (downSince) {
      downSince = 0;
      if (hinted) {
        hinted = false;
        lcd.fillRect(0, H - 26, W, 26, Config::DB_BLUE);
        txt("hold MODE 3 s for setup", W / 2, H - 14, 21,
            D_MC, Config::GREY, Config::DB_BLUE);
      }
    }
    feedWatchdog();
    if (forMs) delay(10);
  } while (millis() - started < forMs);
  return false;
}

// ---------------------------------------------------------------------
// Why did the board restart? The chip keeps a reset reason across a reboot;
// deliberate restarts add their own reason in NVS first. Normal starts
// (plug, upload, RESET button, Save in setup) clear the record; anything else
// is remembered and counted until the next normal start, shown on the boot
// splash and on the setup page, and printed on serial in words.
// ---------------------------------------------------------------------
String   lastRestartWhy;
uint16_t lastRestartCount = 0;

void restartFor(const char *why) {
  prefs.begin("desk-display", false);
  prefs.putString("swwhy", why);
  prefs.end();
  Serial.printf("Restarting: %s\n", why);
  Serial.flush();
  delay(100);
  ESP.restart();
}

void recordResetReason() {
  const esp_reset_reason_t rr = esp_reset_reason();
  prefs.begin("desk-display", false);
  const String swWhy = prefs.getString("swwhy", "");
  if (swWhy.length()) prefs.remove("swwhy");

  String why;
  switch (rr) {
    case ESP_RST_PANIC:    why = "crash (panic)"; break;
    case ESP_RST_INT_WDT:  why = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT: why = "watchdog - something blocked"; break;
    case ESP_RST_WDT:      why = "watchdog"; break;
    case ESP_RST_BROWNOUT: why = "brown-out (power dip)"; break;
    case ESP_RST_SW:
      // Our own restarts. Save in setup is a normal one.
      if (swWhy.length() && swWhy != "saved settings") why = swWhy;
      break;
    default: break;   // power-on, RESET button, USB upload: normal
  }

  if (why.length()) {
    lastRestartCount = prefs.getUShort("badcount", 0) + 1;
    prefs.putUShort("badcount", lastRestartCount);
    prefs.putString("badwhy", why);
    lastRestartWhy = why;
  } else if (rr == ESP_RST_POWERON || rr == ESP_RST_EXT || rr == ESP_RST_USB) {
    prefs.remove("badcount");         // fresh start: forget the old record
    prefs.remove("badwhy");
  } else {
    // A normal software restart keeps showing the last problem, if any.
    lastRestartWhy = prefs.getString("badwhy", "");
    lastRestartCount = prefs.getUShort("badcount", 0);
  }
  prefs.end();
  Serial.printf("Reset reason %d%s%s\n", (int)rr,
                why.length() ? " - UNEXPECTED: " : " (normal)",
                why.c_str());
}

// ---------------------------------------------------------------------
// MODE button, running.
//
// The interrupt does the debouncing and hands over at most one tap per
// press (see buttonIsr in Util.ino). This only acts on it:
//
//   tap (released within 1 s)  -> next screen, exactly one
//   hold 1 s                   -> "keep holding for setup..." on the footer;
//                                 letting go now changes nothing
//   hold 3 s                   -> setup portal, straight away
//
// Why it jumped several pages before: the old version latched any falling
// edge and, if the loop was busy drawing when it looked, measured the "press"
// from that stale edge - so contact bounce or a glitch on the pin became
// extra taps. Now a press has to be really down for 40 ms, and it is timed
// where it happens, in the interrupt.
// ---------------------------------------------------------------------
void handleButton() {
  static bool hinted = false;
  static bool spent = false;          // this hold already opened setup
  static uint32_t highSince = 0;
  const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

  bool down, tap;
  uint32_t downAt;
  noInterrupts();
  down = btnDown;
  downAt = btnDownAt;
  tap = btnTap;
  btnTap = false;
  interrupts();

  // A glitch shorter than the debounce can leave "down" set with no release
  // edge to clear it. If the pin has really been up for 80 ms, it is up.
  if (down && !modePinDown()) {
    if (!highSince) highSince = now | 1;
    else if (now - highSince >= 80) {
      noInterrupts();
      btnDown = false;
      btnUpAt = now;
      interrupts();
      down = false;
    }
  } else {
    highSince = 0;
  }

  if (spent) {                        // wait for the release after a hold
    if (!down) spent = false;
    return;
  }

  if (tap && !setupOnlyMode) {
    hinted = false;
    screenIndex = (screenIndex + 1) % totalScreens();
    lastPageAt = millis();            // a tap restarts the dwell interval
    Serial.printf("MODE tap - screen %u/%u\n", screenIndex + 1, totalScreens());
    renderScreen(true);
    return;
  }

  if (down && !setupOnlyMode) {
    const uint32_t held = now - downAt;
    const int W = lcd.width(), H = lcd.height();
    static int barShown = 0;
    if (!hinted && held >= Config::HOLD_HINT_MS) {
      hinted = true;
      barShown = 0;
      lcd.fillRect(0, H - 24, W, 24, Config::BLACK);
      txt("keep holding for setup...", W / 2, H - 13, 22,
          D_MC, Config::DB_YELLOW, Config::BLACK);
      lcd.drawFastHLine(0, H - 2, W, rgb(60, 60, 60));
      healthX = -1;                   // the hint is sitting on the footer
      marqueeCount = 0;               // nothing scrolls across it
    }
    // A bar along the bottom fills from 1 s to 3 s, so the hold is visibly
    // counting rather than just sitting there.
    if (hinted) {
      uint32_t p = held - Config::HOLD_HINT_MS;
      const uint32_t span = Config::HOLD_FOR_SETUP_MS - Config::HOLD_HINT_MS;
      if (p > span) p = span;
      const int bw = (int)(p * (uint32_t)W / span);
      if (bw > barShown) {
        lcd.fillRect(barShown, H - 3, bw - barShown, 3, Config::DB_YELLOW);
        barShown = bw;
      }
    }
    if (held >= Config::HOLD_FOR_SETUP_MS) {
      spent = true;
      hinted = false;
      Serial.println("MODE held 3 s - opening setup portal");
      startSetupPortal("Opened with the MODE button");
    }
  } else if (hinted) {
    hinted = false;                   // let go between 1 and 3 s: no change
    renderScreen(false);              // put the footer back
  }
}

// ---------------------------------------------------------------------
// Boot order: watchdog -> panel + sprite -> splash -> settings -> reset
// reason -> 1.5 s window for a MODE hold -> Wi-Fi join (MODE still
// watched) -> NTP + time zone -> button interrupt -> network task ->
// first page. Any failure to join Wi-Fi ends in setup mode instead.
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // Drawing and the button outrank fetching. The network task runs at 1;
  // this puts the loop at 2, so a TLS handshake can never starve the screen.
  vTaskPrioritySet(NULL, 2);

  // The API changed shape between ESP-IDF 4 (Arduino core 2.x) and ESP-IDF 5
  // (core 3.x): two arguments became a config struct. On core 3.x the system
  // has usually started the watchdog already, so a second init answers
  // ESP_ERR_INVALID_STATE and reconfigure is the way in.
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtConfig = {};
  wdtConfig.timeout_ms     = Config::WATCHDOG_S * 1000;
  wdtConfig.idle_core_mask = 0;          // the idle tasks are not subscribed
  wdtConfig.trigger_panic  = true;
  if (esp_task_wdt_init(&wdtConfig) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdtConfig);
  }
#else
  esp_task_wdt_init(Config::WATCHDOG_S, true);
#endif
  esp_task_wdt_add(NULL);
  pinMode(Config::MODE_BUTTON_PIN, INPUT_PULLUP);
  dataMutex = xSemaphoreCreateRecursiveMutex();

  lcd.init();
  lcd.setRotation(1);                    // ILI9341 landscape, 320 x 240
  lcd.setTextWrap(false);
  // Taken once, here, while the heap is still untouched by Wi-Fi and TLS.
  scratch.setColorDepth(16);
  scratchReady = (scratch.createSprite(SCRATCH_W, SCRATCH_H) != nullptr);

  drawBootSplash("Starting");
  Serial.printf("\nDesk display v%s | panel %d x %d | sprite %s\n",
                Config::FIRMWARE_VERSION, (int)lcd.width(), (int)lcd.height(),
                scratchReady ? "ok" : "UNAVAILABLE");
  loadSettings();
  recordResetReason();
  if (lastRestartWhy.length()) {
    // Stays on the splash through the Wi-Fi join, so it can be read.
    const int W = lcd.width(), H = lcd.height();
    String line = "last restart: " + lastRestartWhy;
    if (lastRestartCount > 1) line += " (" + String(lastRestartCount) + "x)";
    txt(clipped(line, W - 12, 21), W / 2, H / 2 + 26, 21,
        D_MC, Config::CANCEL_RED, Config::DB_BLUE);
  }

  // A short window to hold MODE for setup before anything else happens.
  // (Holding it BEFORE power-on starts the C3's own flash-download mode
  // instead - that is the chip's strapping pin, not something code decides.)
  if (bootHoldForSetup(Config::BOOT_SPLASH_MS)) {
    startSetupPortal("Opened with the MODE button");
    return;
  }

  bootStatus(settings.ssid.length() ? "Joining Wi-Fi" : "No Wi-Fi saved");
  if (!connectWifi()) {
    if (setupHeldAtBoot) {
      startSetupPortal("Opened with the MODE button");
    } else {
      startSetupPortal(settings.ssid.isEmpty() ? "First-time setup"
                                               : "Wi-Fi not reachable");
    }
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  attachInterrupt(digitalPinToInterrupt(Config::MODE_BUTTON_PIN), buttonIsr, CHANGE);
  btnTap = false;

  // No waiting for the first cycle any more: the board goes up now and each
  // page fills in as its feed lands.
  startNetTask();
  renderScreen(false);
  lastPageAt = millis();
}

// ---------------------------------------------------------------------
// The loop owns the panel. In order: setup mode (serve the portal only),
// the MODE button, marquees and the health dot, repaint when a feed this
// page shows has landed, the clock on the minute, Wi-Fi-loss bookkeeping,
// and auto-advance. It never blocks on the network.
// ---------------------------------------------------------------------
void loop() {
  feedWatchdog();

  if (setupOnlyMode) {
    // Setup mode: serve the portal, nothing else. It ends on Save or RESET.
    server.handleClient();
    static uint32_t lastStatusAt = 0;
    static int lastClients = -1;
    if (millis() - lastStatusAt > 1000) {
      lastStatusAt = millis();
      const int n = WiFi.softAPgetStationNum();
      if (n != lastClients) {
        const bool crossedZero = (n > 0) != (lastClients > 0);
        lastClients = n;
        apClients = n;
        // Crossing 0<->1 swaps the QR between "join" and "open setup".
        if (crossedZero) drawSetupScreen(portalReason);
      }
    }
    delay(2);
    return;
  }

  handleButton();
  // A 3 s hold just opened setup: stop here. Carrying on with this pass is
  // what drew scrolling names and the clock over the QR code.
  if (setupOnlyMode) return;

  // While MODE is held nothing else touches the screen, so the "keep
  // holding" hint and its progress bar stay put. New data waits until the
  // button is let go.
  if (btnDown) { delay(2); return; }

  updateMarquees();
  tickHealth();               // the ticker: proof the board is still alive

  const uint32_t now = millis();

  // New data landed on the network task: repaint once, here, on the task
  // that owns the panel.
  if (dataDirty) {
    const uint8_t landed = dataDirty;
    dataDirty = 0;
    // A change in the number of train pages shifts every page after them,
    // so that always redraws whatever the page is.
    static uint16_t lastTrainPages = 0;
    const uint16_t trainPages = trainPageCount();
    const bool shifted = trainPages != lastTrainPages;
    lastTrainPages = trainPages;
    if (shifted || (landed & screenFeeds())) renderScreen(false);
  }

  // The minute change redraws the clock's own rectangle and nothing else.
  static uint32_t lastClockCheck = 0;
  if (now - lastClockCheck > 1000) {
    lastClockCheck = now;
    lockData();
    repaintClock();
    unlockData();
  }

  // Wi-Fi loss. Re-joining is the network task's job; all the loop does is
  // note the outage and decide when it has gone on long enough to restart.
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostAt == 0) {
      wifiLostAt = now | 1;
      Serial.println("Wi-Fi lost - network task will re-join");
    }
    if (now - wifiLostAt >= Config::WIFI_REBOOT_AFTER_MS) {
      Serial.println("Wi-Fi gone for 15 min - restarting");
      restartFor("no Wi-Fi for 15 min");
    }
  } else if (wifiLostAt != 0) {
    wifiLostAt = 0;
    lastRefreshAt = 0;          // back online: refresh at once
    Serial.println("Wi-Fi back");
  }

  // With auto-advance off, elapsed time never changes the page.
  if (settings.autoAdvance &&
      now - lastPageAt >= (uint32_t)settings.pageSeconds * 1000UL) {
    lastPageAt = now;
    screenIndex = (screenIndex + 1) % totalScreens();
    renderScreen(true);
  }
  delay(2);
}
