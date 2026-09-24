#pragma once
// =====================================================================
//  Train Bus Weather Signboard  -  firmware v1.0.0
//  ESP32-C3 Super Mini + 3.2" 240x320 ILI9341 SPI TFT
//  https://github.com/haldarsaurav/train_bus_weather_signboard
//
//  Copyright (c) 2026 Sam (haldarsaurav). All rights reserved.
//  No use, copying, modification or redistribution - and no reproduction
//  of this code or design with AI tools - without written permission.
//  See LICENSE.
// =====================================================================
//
//  DeskDisplay.h - the one place every tab agrees on.
//
//  The sketch is split across Arduino tabs, which the IDE concatenates in
//  its own order. Every type, constant, global and function therefore has
//  its declaration here, and every tab starts by including this file.
//  Nothing in the project relies on the prototype generator.
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

// =====================================================================
//  USER OPTIONS - the only lines most builds ever need to touch.
// =====================================================================

// Private values live in DeskSecrets.h, next to this file. That file is
// git-ignored, so your real passwords never reach GitHub. To create it,
// copy DeskSecrets.example.h to DeskSecrets.h and edit it. Without it the
// sketch still builds, using the placeholders below.
#if defined(__has_include)
#  if __has_include("DeskSecrets.h")
#    include "DeskSecrets.h"
#  endif
#endif

// Password for the setup access point (WPA2, minimum 8 characters).
// iOS refuses open ESP32 access points far more often than WPA2 ones, so
// the portal is never open. It is also what the QR code on the setup
// screen carries. Set your own in DeskSecrets.h.
#ifndef AP_PASSWORD
#define AP_PASSWORD "change-me-please"
#endif

// OPTIONAL: skip the setup access point on first boot.
// If the setup Wi-Fi will not appear on your phone, put your normal
// 2.4 GHz network in DeskSecrets.h and reflash; the board then joins it
// directly. Settings are still changed through setup mode (hold MODE 3 s).
// Leave both empty to use the access point as normal.
#ifndef WIFI_FALLBACK_SSID
#define WIFI_FALLBACK_SSID ""
#endif
#ifndef WIFI_FALLBACK_PASS
#define WIFI_FALLBACK_PASS ""
#endif

// ---------------------------------------------------------------------
// Display driver: LovyanGFX.
// TFT_eSPI 2.5.43 crashes inside init() on this ESP32-C3 (on both core
// 3.3.11 and 2.0.17), so the project uses LovyanGFX. Everything the panel
// needs is configured in the class below; there is no User_Setup.h.
//
//   SPI2, mode 0, 20 MHz write / 8 MHz read, DMA auto
//   SCK 4 | MOSI 6 | MISO 5 | DC 3 | CS 7 | RST 10
//   Panel_ILI9341 240x320, no inversion, RGB order - landscape via
//   lcd.setRotation(1) in setup(), which gives 320 x 240.
// ---------------------------------------------------------------------
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 20000000;
      cfg.freq_read   =  8000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 4;
      cfg.pin_mosi    = 6;
      cfg.pin_miso    = 5;
      cfg.pin_dc      = 3;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 7;
      cfg.pin_rst          = 10;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;   // CONFIRMED from the label on the module:
      cfg.panel_height     = 320;   // 3.2 inch, 240x320 - NOT a 480x320 panel
      cfg.memory_width     = 240;
      cfg.memory_height    = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = false;  // ILI9341: no inversion (9488 driver needed it)
      cfg.rgb_order        = false;  // CONFIRMED: R reads red
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};


// Datum codes match TFT_eSPI numbering, which LovyanGFX keeps.
static constexpr uint8_t D_TL = 0,  D_TC = 1,  D_TR = 2;
static constexpr uint8_t D_ML = 4,  D_MC = 5,  D_MR = 6;
static constexpr uint8_t D_BL = 8,  D_BC = 9,  D_BR = 10;

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// A name too wide for its column is drawn clipped, then scrolled sideways so
// the whole thing can be read. Rows register themselves here while the screen
// is drawn; the loop animates them afterwards.
struct Marquee {
  int16_t x, y, w, h;      // the window the text lives in (y is its middle)
  int16_t textW;
  int16_t offset;          // pixels scrolled, with a pause at each end
  uint8_t font;
  uint16_t fg, bg;
  String text;
};
constexpr uint8_t MAX_MARQUEE = 12;

namespace Config {
constexpr char AP_NAME[] = "Desk-Transport-Display";
constexpr char HOST_NAME[] = "desk-display";
constexpr char TRANSITOUS_URL[] = "https://api.transitous.org";
constexpr char USER_AGENT[] = "DeskTransportDisplay/1.0 (+https://github.com/haldarsaurav/train_bus_weather_signboard)";
// The pin labelled BOOT on the ESP32-C3 Super Mini. On the finished device it
// is the MODE button, and nothing about booting, so it is named for the job it
// does rather than for the silkscreen.
constexpr uint8_t MODE_BUTTON_PIN = 9;
constexpr char FIRMWARE_VERSION[] = "1.0.0";   // shown on the boot splash
constexpr uint32_t DATA_REFRESH_MS = 60000;   // live enough without hammering the APIs
constexpr uint32_t WIFI_RETRY_MS = 30000;
// Losing Wi-Fi must never turn a departure board into an access point: it
// keeps showing what it has and keeps trying. A long outage ends in a clean
// restart instead, because a fresh scan-and-join beats endless reconnects.
constexpr uint32_t WIFI_REBOOT_AFTER_MS = 900000;      // 15 min without Wi-Fi
constexpr uint32_t DATA_STALE_REBOOT_MS = 1800000;     // 30 min without data
// When the dot turns red for age. Three missed refreshes in a row: long
// enough that one slow feed does not cry wolf, short enough that a board
// showing times from ten minutes ago is never quietly green.
constexpr uint32_t DATA_STALE_MS = DATA_REFRESH_MS * 3 + 30000;   // 3.5 min
// There is deliberately no portal idle reboot. Setup mode ends on Save, on the
// portal's Restart, or on the physical RESET button - never on a timer. The
// old 10-minute reboot fired while a phone was asleep or had briefly wandered
// off the AP, which is exactly when you least want the board to restart.
constexpr uint32_t WATCHDOG_S = 60;
// MODE button. A press shorter than this is a tap (next page); holding it
// this long opens the setup portal - while running and on the boot splash.
constexpr uint32_t HOLD_FOR_SETUP_MS = 3000;
constexpr uint32_t HOLD_HINT_MS = 1000;    // "keep holding" appears here
constexpr uint32_t DEBOUNCE_MS = 40;
// How long the boot splash waits for a 3 s hold before carrying on.
constexpr uint32_t BOOT_SPLASH_MS = 1500;
// A TLS handshake that has not finished in this long is not going to. The
// library default is 120 s, twice the watchdog - one stalled handshake used
// to reboot the board.
constexpr uint32_t TLS_HANDSHAKE_S = 8;
// The stagger between the pieces of a page during a transition. Six train
// rows at 14 ms is 84 ms of cascade; ten bus rows is 140. Much more than
// this and a tap starts to feel like waiting rather than like motion.
constexpr uint8_t REVEAL_STEP_MS = 14;
constexpr uint32_t MIN_FREE_HEAP = 50000;              // refuse a fetch below this
constexpr uint32_t MIN_BLOCK = 40000;                  // ...or if no block this big is left
constexpr uint8_t ROWS_PER_PAGE = 6;   // 320x240 fits 6 rows comfortably
constexpr uint8_t MAX_TRAIN_PAGES = 4; // four pages of six

// Freising station, as Transitous knows it. Pinning this matters: a stored
// DB id sends every train refresh to v6.db.transport.rest first, which
// answers 503 far more often than not - a whole TLS handshake and up to
// eleven seconds burned before the fallback even starts. The same id serves
// the rail platforms AND the Stadtbusbahnhof bays; only the mode filter
// differs. Verified against the live API.
constexpr char TRAIN_FIXED_ID[] = "motis:de-DELFI_de:09178:2680";
constexpr char TRAIN_LABEL[] = "Freising";
constexpr uint8_t TRAIN_REQUEST_N = 30;   // 24 shown; headroom for filtered rows

// Weather fallback. The live place comes from setup (a typed town) or is
// detected from the internet connection - see detectLocation() in Net.ino.
// These are used only until that has succeeded once.
constexpr char WEATHER_LABEL[] = "Freising";
constexpr char WEATHER_LAT[] = "48.4029";
constexpr char WEATHER_LON[] = "11.7480";

// ---- 320 x 240 layout geometry (landscape, setRotation(1)) ------------
// The panel is 240x320 native, so landscape gives 320 x 240.
constexpr int16_t TR_TITLE_H = 20;   // station name + clock strip
constexpr int16_t TR_HDR_Y   = 22;   // olive column-header band
constexpr int16_t TR_HDR_H   = 20;
constexpr int16_t TR_ROW_TOP = 46;
constexpr int16_t TR_ROW_H   = 26;   // 6 rows -> 46..202

constexpr uint8_t MAX_TRAINS = 24;   // four pages of six

// Fixed bus stops, pinned to exact Transitous ids. There is no name search
// any more: an id that answers with zero departures means no buses right
// now, not a wrong stop.
struct BusStopDef {
  const char *label;     // what the screen shows
  const char *fixedId;   // exact Transitous stop
  bool cityBusOnly;      // keep StadtBus routes, drop RegionalBus and ExpressBus
  uint8_t requestN;      // departures asked for (headroom for what is filtered)
};
constexpr BusStopDef BUS_STOPS[] = {
  // Stadtbusbahnhof, bays 1-7. The same id carries regional coaches, which
  // cityBusOnly drops - hence the larger request.
  { "Bahnhof Stadtbus", "motis:de-DELFI_de:09178:2680",     true,  24 },
  { "P+R-Platz",        "motis:de-DELFI_de:09178:2831:1:1", false, 14 },
};
constexpr uint8_t BUS_STOP_COUNT = 2;

constexpr uint8_t BUS_MAX_ITEMS  = 12;   // upper bound on one page

// One stop to a page, ten departures. The fetch asks for exactly what the
// page can show, so nothing is parsed that cannot be displayed.
constexpr uint8_t BUS_ROWS_PER_PAGE = 10;
constexpr int16_t BUS_BAND_H = 26;   // white name band with the clock
constexpr int16_t BUS_ROW_TOP = 30;  // first departure
constexpr int16_t BUS_ROW_H = 19;    // 10 rows -> 30..220
// The right-hand block of a bus row, as right edges. Fixed rather than
// measured so the three columns line up down the whole board whether or not
// any given service is running late.
constexpr int16_t BUS_MIN_X  = 314;  // countdown in minutes
constexpr int16_t BUS_TIME_X = 282;  // when it will actually leave
constexpr int16_t BUS_PLAN_X = 234;  // planned time, struck, only when late

// ---- Palette sampled from the reference photographs -------------------
// Train board: deep navy body, olive-yellow header band, white rows.
constexpr uint16_t DB_BLUE      = rgb( 10,  22,  66);  // body
constexpr uint16_t DB_BAND      = rgb( 20,  38,  98);  // alternating row
constexpr uint16_t DB_RULE      = rgb( 44,  62, 120);  // hairline
constexpr uint16_t DB_HEADER    = rgb(198, 190,  84);  // header band
constexpr uint16_t DB_HEADER_HI = rgb(232, 226, 128);  // header highlight
constexpr uint16_t DB_YELLOW    = rgb(255, 214,  51);  // notices, delays
constexpr uint16_t WHITE        = rgb(255, 255, 255);
constexpr uint16_t BLACK        = rgb(  0,   0,   0);
constexpr uint16_t GREY         = rgb(140, 150, 175);
constexpr uint16_t ROW_LINE     = rgb( 34,  48,  96);
constexpr uint16_t RE_RED       = rgb(214,  48,  62);  // RE/RB badge outline
constexpr uint16_t S_BAHN_BLUE  = rgb( 58, 160, 214);  // S-Bahn filled oval
constexpr uint16_t CANCEL_RED   = rgb(235,  70,  80);
constexpr uint16_t ON_TIME_GREEN= rgb( 80, 220, 120);
constexpr uint16_t LATE_RED     = rgb(255, 105, 105);
constexpr uint16_t BADGE_WHITE  = rgb(240, 240, 240);

// Bus stop board: near-black body, amber dot-matrix, white bands.
constexpr uint16_t BUS_BLACK    = rgb(  8,   8,   8);
constexpr uint16_t BUS_AMBER    = rgb(255, 168,   0);
constexpr uint16_t BUS_BAND     = rgb(255, 255, 255);

// The weather screens tint with the time of day; WEATHER_BLUE is the daytime
// value and the neutral fallback before sunrise and sunset are known.
constexpr uint16_t WEATHER_BLUE = rgb( 10,  28,  72);   // day
constexpr uint16_t WEATHER_NIGHT = rgb(  5,  11,  32);  // after dusk
constexpr uint16_t WEATHER_TWILIGHT = rgb( 38,  22,  54); // dawn and dusk
}

struct Settings {
  String ssid;
  String password;
  // Weather location, detected from the internet connection (Net.ino,
  // detectLocation). Empty until found; Config::WEATHER_* is the fallback.
  String geoLat;
  String geoLon;
  String geoCity;
  String geoQuery;   // a town typed in setup; empty = detect automatically
  // Off by default: MODE taps through the screens and elapsed time never
  // changes the page. The NVS key stays "rotate" so a board that already has
  // a saved preference keeps it.
  bool   autoAdvance = false;
  // Long destination names sliding sideways is its own setting, independent
  // of auto-advance, and also off by default.
  bool   marqueeEnabled = false;
  uint8_t pageSeconds = 10;   // how long each screen stays up
};

struct Departure {
  time_t epoch = 0;    // actual departure, for sorting
  String line;
  String destination;
  String platform;
  String departureTime;   // when it will ACTUALLY leave (realtime if known)
  // What the timetable says. Kept separate so a late service can show both,
  // the way a real platform sign does: planned struck through, revised in
  // red. Equal to departureTime when the service is running to plan.
  String scheduledTime;
  int16_t delayMinutes = 0;
  bool cancelled = false;
  bool realtime = false;
  // Which physical stop point this one leaves from. A street stop has one
  // of these on each side of the road and the feed returns both under the
  // same query, so without this the board cannot say which side to stand on.
  float lat = 0.0f;
  float lon = 0.0f;
};

constexpr uint8_t WEATHER_HOURS = 12;   // hours in the forecast strip
constexpr uint8_t WEATHER_DAYS  = 4;    // today plus the three days asked for

// One day of the forecast. Day 0 is today; 1..3 are what the outlook page
// shows. Everything Sam asked for per day lives here: high and low, how
// likely rain is, WHEN it is expected, and that day's own sunrise and
// sunset - the old code only ever fetched today's, which is why the outlook
// had none.
struct WeatherDay {
  float   high = NAN;
  float   low = NAN;
  int16_t code = -1;
  int8_t  rainChance = -1;   // precipitation_probability_max, %
  float   rainMm = 0.0f;     // precipitation_sum, mm
  int8_t  rainFrom = -1;     // first local hour with rain, -1 = none forecast
  int8_t  rainTo = -1;       // hour after the last one with rain
  // One bit per local hour, 0..23, set when that hour is forecast wet. The
  // from/to pair above only spans the first to the last wet hour; this keeps
  // the gaps, which is what lets the bar show two separate showers as two.
  uint32_t rainMask = 0;
  String  sunrise;           // "06:45" local
  String  sunset;            // "19:32" local
  int8_t  weekday = -1;      // 0 = Sunday
  String  dayLabel;          // "16.09."
  // ---- the second line of the outlook row -------------------------------
  float   feels = NAN;       // apparent_temperature_max
  int8_t  uv = -1;           // uv_index_max, rounded
  float   windKmh = NAN;     // wind_speed_10m_max
  int16_t windDir = -1;      // wind_direction_10m_dominant, degrees FROM
  int16_t daylightMin = -1;  // sunset - sunrise, in minutes
  int16_t daylightDelta = 0; // change against the day before, in minutes
  float   moonPhase = -1.0f; // 0 new, 0.25 first quarter, 0.5 full, 0.75 last
};

struct WeatherData {
  float temperature = NAN;
  float apparentTemperature = NAN;
  float windSpeed = NAN;
  int16_t windDirection = -1;         // degrees the wind comes FROM
  int16_t humidity = -1;
  int16_t weatherCode = -1;
  String sunrise;                     // today's, for the day/night icon
  String sunset;
  // Next hours, for the strip along the bottom of the weather screen.
  float   hourTemp[WEATHER_HOURS];
  int8_t  hourRain[WEATHER_HOURS];
  int8_t  hourOfDay[WEATHER_HOURS];
  uint8_t hourCount = 0;
  WeatherDay day[WEATHER_DAYS];
  uint8_t dayCount = 0;
  String updatedAt;                   // clock time of the last good fetch
  bool valid = false;
};

// Layout columns, derived from the real panel width at draw time.
// schedX is the right edge of the small struck-through planned time, which
// is drawn only when a service is late. Reserving it always keeps the live
// times in one column down the board whether or not anything is delayed.
struct Cols { int W, H, lineX, lineW, destX, destW, platX, schedX, timeX, minX, rightX; };

struct BayInfo {
  uint8_t distinct = 0;
  float   midLat = 0.0f;
  float   midLon = 0.0f;
  bool    northSouth = true;     // true: the two sides differ N/S, else E/W
};

// Up to eight screens; the count only moves with how many trains there are:
//   trains (1-4) | Bahnhof Stadtbus | P+R-Platz | weather now | next 3 days
constexpr uint8_t EXTRA_SCREENS = 2;   // the two weather pages

// ---------------------------------------------------------------------
// Threading.
//
// Every fetch used to run inside loop(), so a single slow HTTPS call - and
// the public feeds are often slow - froze the clock, the marquees and the
// BOOT button for as long as it took. That is the stuck button and the
// frozen screen. All fetching now lives on its own task; loop() does
// nothing but draw and listen to the button, and never blocks on a socket.
//
// One recursive mutex guards everything both tasks touch: the departure
// arrays, the weather, the graphs, the settings and the status line. The
// network task fetches into its own scratch buffer with the lock OPEN, then
// takes the lock only long enough to hand the finished result over.
// ---------------------------------------------------------------------

// =====================================================================
//  Shared state.
//
//  Defined once, in DeskTransportDisplay.ino. Declaring all of it here is
//  what lets the tabs be split at all: the IDE concatenates them in its own
//  order, and nothing below depends on that order or on Arduino's
//  prototype generator.
// =====================================================================
extern LGFX lcd;

// ---------------------------------------------------------------------
// One off-screen tile, shared by everything that redraws while a screen is
// already up: the scrolling destinations, the health dot and the clock.
//
// Each of those used to paint its background straight onto the live panel
// and then draw over it, which at eighteen frames a second is a visible
// flicker. Rendered into a sprite and pushed once, they simply change.
//
// 256 x 24 at 16 bpp is 12,288 bytes, taken once at boot and never freed, so
// it cannot fail at an awkward moment - and every user falls back to drawing
// straight onto the panel if the allocation did not happen at all.
//
// Pixels a caller does not use keep SCRATCH_KEY and are pushed as
// transparent, so one tile serves regions of different sizes without
// painting over their neighbours.
//
// The size is set by the widest thing that scrolls. A bus destination gets
// whatever the countdown column leaves it, and the countdown can be a single
// digit ("0" is about 10 px), so that window reaches ~246 px. 24 is a train
// row, the tallest.
// ---------------------------------------------------------------------
constexpr int16_t  SCRATCH_W = 256;
constexpr int16_t  SCRATCH_H = 24;
constexpr uint16_t SCRATCH_KEY = rgb(255, 0, 255);   // never used by the UI
extern LGFX_Sprite scratch;
extern bool scratchReady;

extern WebServer server;
extern Preferences prefs;
extern Settings settings;
extern Departure trains[Config::MAX_TRAINS];
extern Departure busRows[Config::BUS_STOP_COUNT][Config::BUS_MAX_ITEMS];
extern WeatherData weather;

extern uint8_t  trainCount;

// The delay graph in the footer of every train page: one sample per
// refresh, newest last.
// Each sample is the average "lateness" of every fetched train, in tenths of
// a minute (a cancellation counts as 15 min, one delay is capped at 30).
// 180 samples at one refresh a minute is the last three hours.
constexpr uint8_t DELAY_HIST = 180;
extern uint8_t delayHist[DELAY_HIST];
extern uint8_t delayHistCount;
void pushDelaySample(const Departure *items, uint8_t n);
void drawDelayGraph(int x, int bottom, int h, uint16_t bg);

// The last 30 days' daily highs plus today, for the graph on the 3-day page.
// Fetched in one small extra call at boot and every 6 hours.
constexpr uint8_t TEMP_DAYS = 31;
constexpr uint32_t TEMP_HISTORY_EVERY_MS = 6UL * 3600UL * 1000UL;
extern float    tempHigh[TEMP_DAYS];
extern uint8_t  tempHighCount;
extern uint32_t tempHistoryAt;       // millis() of the last good fetch, 0 = never
bool fetchTempHistory();
void drawTempHistory(int x, int bottom, int h, uint16_t bg);
extern uint8_t  busRowCount[Config::BUS_STOP_COUNT];
extern uint16_t screenIndex;
extern uint32_t lastPageAt;
extern uint32_t lastRefreshAt;
extern uint32_t lastGoodDataAt;
extern uint32_t staleRecoveryAt;
extern uint32_t lastWifiAttemptAt;
extern uint32_t wifiLostAt;
extern String   statusText;
extern uint16_t weatherBg;
extern String   portalReason;
extern String   wifiFailReason;
extern volatile int apClients;

extern bool     setupOnlyMode;
extern bool     webServerStarted;

// Text that does not fit its column scrolls; rows register themselves here
// while a screen is drawn and the loop animates them afterwards.
extern Marquee  marquees[MAX_MARQUEE];
extern uint8_t  marqueeCount;
extern uint32_t screenShownAt;
extern uint32_t lastMarqueeStepAt;

// The health dot and the clock each register where they were drawn, so the
// loop can repaint one small rectangle instead of the whole page.
extern int16_t  healthX, healthY, healthW;
extern uint16_t healthBg, healthFg;
extern uint32_t lastHealthPaintAt;
extern uint8_t  healthPhase;
extern int16_t  clockX, clockY;
extern uint8_t  clockFont;
extern uint16_t clockFg, clockBg;
extern String   clockShown;

// Threading.
extern SemaphoreHandle_t dataMutex;
extern TaskHandle_t      netTaskHandle;
extern Departure netStage[Config::MAX_TRAINS];
extern volatile bool netBusy;
extern volatile bool netIdle;
extern volatile bool netSuspend;
// Which feeds have new data, one bit each. The loop repaints only when the
// page on screen shows one of them, so weather landing does not flash the
// train board.
constexpr uint8_t DIRTY_TRAINS = 0x01;
constexpr uint8_t DIRTY_BUS0 = 0x02;      // BUS_STOPS[0]; BUS_STOPS[i] is 0x02 << i
constexpr uint8_t DIRTY_WEATHER = 0x40;
constexpr uint8_t DIRTY_ALL = 0xFF;
extern volatile uint8_t dataDirty;
uint8_t screenFeeds();
extern volatile uint32_t cycleCount;

// MODE button, debounced inside the interrupt. btnTap is a single flag, not
// a counter: however many edges one press produces, it moves one page.
extern volatile bool     btnDown;      // currently held (debounced)
extern volatile uint32_t btnDownAt;    // when the current press began
extern volatile uint32_t btnUpAt;      // when the last press ended
extern volatile bool     btnTap;       // a short press finished, not yet used

inline void lockData() {
  if (dataMutex) xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
}
inline void unlockData() {
  if (dataMutex) xSemaphoreGiveRecursive(dataMutex);
}

// =====================================================================
//  Every function the tabs define.
// =====================================================================
// ---- Util.ino ----------------------------------------------------
String asciiGerman(const String &in);
void setFontOn(LovyanGFX &g, int n);
void setFontN(int n);
void txtScroll(const String &value, int x, int y, int w, int h, uint8_t font, uint16_t fg, uint16_t bg);
void updateMarquees();
void txtOn(LovyanGFX &g, const String &value, int x, int y, int font, uint8_t datum, uint16_t fg, uint16_t bg);
void txt(const String &value, int x, int y, int font, uint8_t datum, uint16_t fg, uint16_t bg);
void buttonIsr();   // IRAM_ATTR on the definition in Util.ino
bool modePinDown();
String htmlEscape(const String &value);
String jsonEscape(const String &value);
String urlEncode(const String &value);
String pageSecondsOptions();
int64_t daysFromCivil(int year, uint32_t month, uint32_t day);
time_t isoEpoch(const char *isoTime);
String localHhmm(int64_t utcEpoch, long utcOffset);
String hhmm(const char *isoTime);
bool isRailMode(const String &mode);
bool isBusMode(const String &mode);
uint8_t busRowsPerScreen();
uint16_t pageCount(uint8_t itemCount);
uint16_t trainPageCount();
uint16_t totalScreens();
String clipped(String text, uint16_t width, uint8_t font = 21);
String clockNow();
void sortDepartures(Departure *items, uint8_t count);
String dayPrefix(time_t epoch);
long minutesToDeparture(const Departure &item);
String countdownText(const Departure &item);
String minutesOrBlank(const Departure &item);
uint8_t dropPastDepartures(Departure *items, uint8_t count);
void feedWatchdog();
String shortenDestination(String dest, uint16_t width, uint8_t font);
String cleanPlatform(String value);
String weatherDescription(int code);
int minutesOfDay(const String &hhmmText);
String compassPoint(int16_t degrees);

// ---- Net.ino -----------------------------------------------------
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
bool connectWifi();
bool getJson(const String &url, JsonDocument &document, JsonDocument *filter = nullptr);
bool bootHoldForSetup(uint32_t forMs);
const char *firstText(JsonVariantConst preferred, JsonVariantConst fallback, const char *defaultValue);
int fetchTransitousDepartures(const char *prefixedStopId, Departure *target, uint8_t maximum, uint8_t requestN, bool trainsOnly, bool cityBusOnly);
bool fetchWeather(WeatherData &out);
bool detectLocation();
void runRefreshCycle();
void netTask(void *);
void startNetTask();
void pauseNetTask(uint32_t waitMs);

// ---- Portal.ino --------------------------------------------------
void loadSettings();
void persistSettings();
void handleScan();
void handleSave();
void sendPortalPage();
void startWebServer();
String weatherCard();
void drawSetupScreen(const String &reason);
void startSetupPortal(const String &reason);

// ---- Draw.ino ----------------------------------------------------
// What the dot is saying. Deliberately only four states - the whole point of
// dropping the "12s ago" text is that the colour alone has to be readable
// from across the desk.
enum class Health : uint8_t {
  Good,     // green  - last cycle brought everything back
  Retry,    // yellow - a feed missed; the next cycle is expected to fix it
  Stale,    // red    - nothing fresh for minutes, or no Wi-Fi. Blinks.
  Starting  // grey   - still waiting for the first data after boot
};
Health healthState();
uint16_t healthColour(Health state);
void paintHealth();
void footerHealth(int x, int y, uint16_t bg, uint16_t fg, int w = 22);
void headerClock(int x, int y, uint8_t font, uint16_t fg, uint16_t bg);
void repaintClock();
void tickHealth();
Cols columns(bool busBoard);
void drawTrainHeader(const String &title);
void drawTrainRow(const Departure &item, int y, uint8_t index);
BayInfo surveyBays(const Departure *items, uint8_t n);
String bayLabel(const Departure &item, const BayInfo &info);
void drawBusBoard(uint8_t idx);
void drawDeparturePage(Departure *items, uint8_t itemCount, uint16_t page, uint16_t pages, const String &title);
void drawWeatherIcon(int cx, int cy, int r, int code, bool night = false);
bool isNightNow();
uint16_t skyColour();
void drawHourlyStrip(int x, int y, int w, int h);
void drawWeatherHeader(const String &title);
void drawSunEvent(int x, int y, bool rising, const String &hhmmText, bool rightAligned, uint16_t bg);
String rainWindowText(const WeatherDay &d);
void drawMoon(int cx, int cy, int r, float phase, uint16_t bg);
void drawRainBar(int x, int y, int w, int h, const WeatherDay &d);
void drawOutlookRow(const WeatherDay &d, int top, int rowH);
void drawWeather();
void drawWeatherOutlook();
void drawCurrentScreen();

// ---- the page transition -----------------------------------------
// Milliseconds to pause between the pieces of a page as it is drawn. Zero
// draws at full speed, which is what every redraw that is not a page change
// wants. revealPause() is called by the page painters between rows; it
// closes the SPI transaction for the length of the pause so the panel
// actually shows what has been drawn so far.
extern uint8_t revealStep;
void revealPause();
void wipeScreen(uint16_t colour);
void renderScreen(bool animate = false);

// ---- DeskTransportDisplay.ino ------------------------------------
void handleButton();
void drawBootSplash(const char *line);
// Restarts the board on purpose, leaving the reason in NVS so the next boot
// can say why. Every deliberate ESP.restart() goes through this.
void restartFor(const char *why);
// Filled at boot: "" after a normal power-on/upload, else e.g. "brown-out
// (power dip)". lastRestartCount counts unexpected restarts since power-on.
extern String   lastRestartWhy;
extern uint16_t lastRestartCount;
void recordResetReason();
void setup();
void loop();
