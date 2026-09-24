#include "DeskDisplay.h"

// =====================================================================
//  Train Bus Weather Signboard  -  firmware v1.0.0
//  Copyright (c) 2026 Sam (haldarsaurav). All rights reserved.
//  No use, copying, modification or redistribution - and no reproduction
//  with AI tools - without written permission. See LICENSE.
//
//  Draw.ino - every pixel on the panel. Called only from the loop task.
//
//  Contents, top to bottom: the health dot and the clock, train board,
//  bus boards, weather now, 3-day outlook, delay and temperature graphs,
//  screen routing, and the page transition.
// =====================================================================

// The dot says one thing at a time, and it is never the age of the data in
// seconds - that number changed twice a second and told you nothing you
// could act on.
//
//   green   everything came back on the last cycle
//   yellow  one feed missed. The next cycle is expected to fix it, so this
//           is information, not a problem.
//   red     nothing fresh for minutes, or the Wi-Fi is gone. What is on the
//           screen is old and should not be trusted. Blinks, so it is the
//           one state that catches your eye from across the room.
//   grey    still waiting for the first data after a boot.
//
// A fetch in flight does NOT get its own colour. It dims whichever colour is
// already showing, so "busy" never hides "stale".
Health healthState() {
  if (WiFi.status() != WL_CONNECTED) return Health::Stale;
  if (lastGoodDataAt == 0)           return Health::Starting;
  if (millis() - lastGoodDataAt > Config::DATA_STALE_MS) return Health::Stale;
  if (statusText == "Data source offline") return Health::Stale;
  if (statusText == "Partly offline")      return Health::Retry;
  if (statusText == "Live")                return Health::Good;
  return Health::Retry;
}

uint16_t healthColour(Health state) {
  switch (state) {
    case Health::Good:  return rgb( 70, 220, 120);
    case Health::Retry: return rgb(255, 190,  70);
    case Health::Stale: return rgb(255,  80,  80);
    default:            return rgb(150, 156, 172);
  }
}

// Repaints only its own tile, so this can run several times a second
// without touching the rest of the screen.
void paintHealth() {
  if (healthX < 0) return;
  const int x = healthX, y = healthY;
  const Health state = healthState();
  uint16_t colour = healthColour(state);
  const uint8_t phase = healthPhase % 6;          // ripple out, then rest

  // Red blinks: three ticks lit, three dark. Nothing else blinks, so a
  // blinking dot always means the same thing.
  const bool blinkOff = (state == Health::Stale) && (phase >= 3);
  // A fetch in flight halves the brightness instead of changing the hue.
  const bool busy = netBusy;
  if (busy) colour = (uint16_t)((colour >> 1) & 0x7BEF);

  const uint16_t half    = (uint16_t)((colour >> 1) & 0x7BEF);
  const uint16_t quarter = (uint16_t)((colour >> 2) & 0x39E7);
  const int16_t h = 17, w = healthW > 0 ? healthW : 22;

  // Off-screen where there is room for it. This tile repaints two and a half
  // times a second, so painting its background onto the live panel first is
  // a flicker you can see from across the desk.
  const bool useSprite = scratchReady && w <= SCRATCH_W && h <= SCRATCH_H;
  LovyanGFX &g = useSprite ? (LovyanGFX &)scratch : (LovyanGFX &)lcd;
  const int cx = useSprite ? 8 : x, cy = useSprite ? 8 : y;

  if (useSprite) {
    scratch.fillScreen(SCRATCH_KEY);
    scratch.fillRect(0, 0, w, h, healthBg);
  } else {
    lcd.fillRect(x - 8, y - 8, w, h, healthBg);
  }

  if (!blinkOff) {
    g.fillCircle(cx, cy, 3, colour);
    // Only a healthy board breathes. A ring pulsing out of a red dot would
    // read as "working on it", which is the opposite of what red means.
    if (state == Health::Good && !busy) {
      if (phase == 0)      g.drawCircle(cx, cy, 5, half);
      else if (phase == 1) g.drawCircle(cx, cy, 6, quarter);
      else if (phase == 2) g.drawCircle(cx, cy, 7, quarter);
    }
  } else {
    // Not empty - a faint outline, so the dot's place on the page is still
    // visible while it is in its dark half.
    g.drawCircle(cx, cy, 3, quarter);
  }

  if (useSprite) scratch.pushSprite(&lcd, x - 8, y - 8, SCRATCH_KEY);
}

// Every page calls this once, at the spot its footer lives. It both draws
// the indicator and tells the loop where to keep redrawing it.
void footerHealth(int x, int y, uint16_t bg, uint16_t fg, int w) {
  healthX = (int16_t)x;
  healthY = (int16_t)y;
  healthW = (int16_t)w;
  healthBg = bg;
  healthFg = fg;
  paintHealth();
}

void headerClock(int x, int y, uint8_t font, uint16_t fg, uint16_t bg) {
  clockX = (int16_t)x;
  clockY = (int16_t)y;
  clockFont = font;
  clockFg = fg;
  clockBg = bg;
  clockShown = clockNow();
  txt(clockShown, x, y, font, D_MR, fg, bg);
}

void repaintClock() {
  if (clockX < 0) return;
  const String now = clockNow();
  if (!now.length() || now == clockShown) return;
  clockShown = now;

  // 54 x 19 covers "00:00" at either face, with two pixels of slack on the
  // right so the glyphs are never shaved.
  const int16_t w = 54, h = 19, left = clockX - 52;

  if (scratchReady && w <= SCRATCH_W && h <= SCRATCH_H) {
    scratch.fillScreen(SCRATCH_KEY);
    scratch.fillRect(0, 0, w, h, clockBg);
    txtOn(scratch, now, 52, h / 2, clockFont, D_MR, clockFg, clockBg);
    scratch.pushSprite(&lcd, left, clockY - 9, SCRATCH_KEY);
    return;
  }

  lcd.fillRect(left, clockY - 9, w, h, clockBg);
  txt(now, clockX, clockY, clockFont, D_MR, clockFg, clockBg);
}

// Called from the loop. This is the ticker: as long as it keeps running,
// the dot keeps breathing and the counter keeps climbing.
void tickHealth() {
  if (healthX < 0) return;
  const uint32_t now = millis();
  if (now - lastHealthPaintAt < 400) return;
  lastHealthPaintAt = now;
  healthPhase++;
  lockData();
  paintHealth();
  unlockData();
}

// ---------------------------------------------------------------------
// Column positions are derived from the real panel width at draw time.
// Nothing here assumes 480 px.
// ---------------------------------------------------------------------
Cols columns(bool busBoard) {
  Cols c;
  c.W      = lcd.width();
  c.H      = lcd.height();
  c.lineX  = 6;
  c.lineW  = 56;          // fits a four-character line like "RE22"
  c.destX  = c.lineX + c.lineW + 8;
  c.rightX = c.W - 6;
  // Right side, right-to-left: [min] [HH:MM] [plan] [platform]
  // Each of these is a RIGHT edge, with fixed room reserved, so nothing can
  // drift into its neighbour whatever the values are.
  // Widths are for the 9pt proportional face: "127" is about 26 px, "23:53"
  // about 44 px, a platform about 22 px. Each column gets its width plus a
  // 10 px gap, which is what stopped "4" and "23:53" running together.
  c.minX   = c.rightX;              // minutes, right edge
  c.timeX  = c.rightX - 28;         // the time it will actually leave
  // The planned time, small and struck through, only appears on a late row.
  // 48 px is "10:27" at the small bitmap face plus a gap either side. The
  // column is reserved on every row so the live times stay in one line down
  // the board and an empty slot is a reliable "this one is on time".
  c.schedX = c.timeX - 48;
  c.platX  = busBoard ? 0 : (c.schedX - 36);
  const int minsRoom = busBoard ? 74 : 0;
  const int destEnd  = busBoard ? (c.rightX - minsRoom) : (c.platX - 26);
  c.destW  = destEnd - c.destX;
  if (c.destW < 30) c.destW = 30;
  return c;
}

// Train board - Freising platform sign: navy body, olive header band,
// outlined RE/RB badges, filled S-Bahn ovals.
void drawTrainHeader(const String &title) {
  const Cols c = columns(false);
  lcd.fillScreen(Config::DB_BLUE);

  txt(clipped(title, c.W - 70, 22), c.lineX, Config::TR_TITLE_H / 2, 22,
      D_ML, Config::DB_YELLOW, Config::DB_BLUE);

  headerClock(c.rightX, Config::TR_TITLE_H / 2, 21,
              Config::DB_YELLOW, Config::DB_BLUE);

  // Header band with a lit top edge, as on the real sign.
  lcd.fillRect(0, Config::TR_HDR_Y, c.W, Config::TR_HDR_H, Config::DB_HEADER);
  lcd.fillRect(0, Config::TR_HDR_Y, c.W, 4, Config::DB_HEADER_HI);
  lcd.drawFastHLine(0, Config::TR_HDR_Y + Config::TR_HDR_H - 1, c.W, rgb(120, 112, 40));
  // thin yellow rule under the title strip
  lcd.drawFastHLine(0, Config::TR_TITLE_H, c.W, Config::DB_RULE);

  const int hy = Config::TR_HDR_Y + Config::TR_HDR_H / 2;
  txt("Line", c.lineX, hy, 21, D_ML, Config::BLACK, Config::DB_HEADER);
  txt("To",   c.destX, hy, 21, D_ML, Config::BLACK, Config::DB_HEADER);
  txt("Pl.",  c.platX, hy, 21, D_MR, Config::BLACK, Config::DB_HEADER);
  // "plan" and "min" ride at the small face: their columns are narrower than
  // the words are at 9pt, and a header running into its neighbour is the one
  // thing a departure board must never do.
  txt("plan", c.schedX, hy, 2,  D_MR, rgb(88, 82, 30), Config::DB_HEADER);
  txt("time", c.timeX, hy, 21, D_MR, Config::BLACK, Config::DB_HEADER);
  txt("min",  c.minX,  hy, 2,  D_MR, Config::BLACK, Config::DB_HEADER);
}

void drawTrainRow(const Departure &item, int y, uint8_t index) {
  const Cols c = columns(false);
  const int mid = y + Config::TR_ROW_H / 2;
  const bool sBahn = item.line.startsWith("S") && item.line.length() <= 3;

  // A four-character line like "RE22" is wider than the badge at bold 9pt,
  // which is what turned it into "RE2..". Measure first and step down to the
  // regular face when the bold one will not fit.
  const int badgeW = c.lineW - 6;
  uint8_t lineFont = 22;
  setFontN(lineFont);
  if (lcd.textWidth(asciiGerman(item.line).c_str()) > badgeW) lineFont = 21;

  const uint16_t bg = (index & 1) ? Config::DB_BAND : Config::DB_BLUE;
  lcd.fillRect(0, y, c.W, Config::TR_ROW_H, bg);

  // Line badge. RE/RB are a white plate with red lettering, as on the real
  // platform sign; S-Bahn is a filled blue lozenge with white lettering.
  if (sBahn) {
    lcd.fillRoundRect(c.lineX, y + 3, c.lineW, 20, 10, Config::S_BAHN_BLUE);
    txt(clipped(item.line, badgeW, lineFont), c.lineX + c.lineW / 2, mid, lineFont,
        D_MC, Config::WHITE, Config::S_BAHN_BLUE);
  } else {
    lcd.fillRoundRect(c.lineX, y + 3, c.lineW, 20, 3, Config::BADGE_WHITE);
    lcd.drawRoundRect(c.lineX, y + 3, c.lineW, 20, 3, Config::RE_RED);
    txt(clipped(item.line, badgeW, lineFont), c.lineX + c.lineW / 2, mid, lineFont,
        D_MC, Config::RE_RED, Config::BADGE_WHITE);
  }

  // Destination. A cancelled service is struck through, like a printed board.
  const uint16_t destColour = item.cancelled ? Config::CANCEL_RED : Config::WHITE;
  if (item.cancelled) {
    // A struck-through name is not scrolled - the line would be left behind.
    const String dest = clipped(item.destination, c.destW, 21);
    txt(dest, c.destX, mid, 21, D_ML, destColour, bg);
    setFontN(21);
    const int w = lcd.textWidth(asciiGerman(dest).c_str());
    lcd.drawFastHLine(c.destX, mid, w, Config::CANCEL_RED);
    lcd.drawFastHLine(c.destX, mid + 1, w, Config::CANCEL_RED);
  } else {
    txtScroll(item.destination, c.destX, mid, c.destW, Config::TR_ROW_H - 2,
              21, destColour, bg);
  }

  if (item.platform.length()) {
    txt(clipped(item.platform, 26, 21), c.platX, mid, 21, D_MR, Config::WHITE, bg);
  }

  // Green when running to plan, red when late or cancelled. The time in this
  // column is always the one to act on: when the service is late it is the
  // revised time, not the timetable's.
  const bool late = item.delayMinutes > 0 && item.scheduledTime.length() &&
                    item.scheduledTime != item.departureTime;
  const uint16_t timeColour = item.cancelled ? Config::CANCEL_RED
                            : (item.delayMinutes > 0 ? Config::LATE_RED
                                                     : Config::ON_TIME_GREEN);

  // The timetable time, struck through, the way it is printed on a platform
  // sign when the service slips. Nothing at all here means nothing is wrong,
  // so a second number down the board is always a delay.
  if (late) {
    // Small face (DejaVu 9 px): the plan and the slip are secondary to the
    // live time, and at this size they leave a clear gap after the platform.
    const uint16_t planFg = rgb(150, 158, 186);
    const int planY = mid - 5;
    txt(item.scheduledTime, c.schedX, planY, 20, D_MR, planFg, bg);
    setFontN(20);
    const int pw = lcd.textWidth(item.scheduledTime.c_str());
    lcd.drawFastHLine(c.schedX - pw, planY, pw, planFg);
    // How much late, under the struck time, so the size of the slip reads
    // without doing the subtraction in your head.
    txt("+" + String(item.delayMinutes), c.schedX, mid + 6, 20, D_MR,
        Config::LATE_RED, bg);
  }
  txt(item.departureTime, c.timeX, mid, 21, D_MR, timeColour, bg);

  if (item.cancelled) {
    txt("X", c.minX, mid, 22, D_MR, Config::CANCEL_RED, bg);
  } else {
    txt(minutesOrBlank(item), c.minX, mid, 21, D_MR, timeColour, bg);
  }

  lcd.drawFastHLine(c.lineX, y + Config::TR_ROW_H - 1,
                    c.rightX - c.lineX, Config::DB_RULE);
}

BayInfo surveyBays(const Departure *items, uint8_t n) {
  BayInfo info;
  float lats[6], lons[6];
  float minLat = 0, maxLat = 0, minLon = 0, maxLon = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (items[i].lat == 0.0f && items[i].lon == 0.0f) continue;
    bool seen = false;
    for (uint8_t j = 0; j < info.distinct; j++) {
      if (fabsf(lats[j] - items[i].lat) < 0.00002f &&
          fabsf(lons[j] - items[i].lon) < 0.00002f) { seen = true; break; }
    }
    if (seen) continue;
    if (info.distinct < 6) {
      lats[info.distinct] = items[i].lat;
      lons[info.distinct] = items[i].lon;
      if (info.distinct == 0) {
        minLat = maxLat = items[i].lat;
        minLon = maxLon = items[i].lon;
      } else {
        if (items[i].lat < minLat) minLat = items[i].lat;
        if (items[i].lat > maxLat) maxLat = items[i].lat;
        if (items[i].lon < minLon) minLon = items[i].lon;
        if (items[i].lon > maxLon) maxLon = items[i].lon;
      }
      info.distinct++;
    }
  }
  info.midLat = (minLat + maxLat) / 2.0f;
  info.midLon = (minLon + maxLon) / 2.0f;
  // Longitude degrees are about two thirds of a latitude degree here, so the
  // comparison is weighted before deciding which axis the sides differ on.
  info.northSouth = (maxLat - minLat) >= ((maxLon - minLon) * 0.663f);
  return info;
}

String bayLabel(const Departure &item, const BayInfo &info) {
  if (item.platform.length() && item.platform != "-") return item.platform;
  if (info.distinct < 2 || info.distinct > 4) return "";
  if (item.lat == 0.0f && item.lon == 0.0f) return "";
  if (info.northSouth) return item.lat >= info.midLat ? "N" : "S";
  return item.lon >= info.midLon ? "E" : "W";
}

// One stop, one page, ten departures. The street stops that used to share a
// split screen are gone; both remaining boards are at the station and each
// gets the whole panel.
void drawBusBoard(uint8_t idx) {
  if (idx >= Config::BUS_STOP_COUNT) return;
  const Config::BusStopDef &def = Config::BUS_STOPS[idx];
  const int W = lcd.width(), H = lcd.height();
  const int band = Config::BUS_BAND_H;
  const int rowH = Config::BUS_ROW_H;
  const int rowTop = Config::BUS_ROW_TOP;
  const int font = 21;
  const uint8_t n = busRowCount[idx];

  lcd.fillScreen(Config::BUS_BLACK);

  // White name band with the live clock, like the real stop sign.
  lcd.fillRect(0, 0, W, band, Config::BUS_BAND);
  const String label = clipped(def.label, W - 70, 22);
  txt(label, 6, band / 2, 22, D_ML, Config::BLACK, Config::BUS_BAND);
  headerClock(W - 6, band / 2, 21, Config::BLACK, Config::BUS_BAND);

  if (n == 0) {
    txt(cycleCount ? "No departures" : "Loading...", W / 2, H / 2 - 10, 24,
        D_MC, Config::BUS_AMBER, Config::BUS_BLACK);
    txt(clipped(statusText, W - 20, 21), W / 2, H / 2 + 20, 21,
        D_MC, rgb(120, 120, 125), Config::BUS_BLACK);
    footerHealth(10, H - 9, Config::BUS_BLACK, rgb(120, 120, 125));
    return;
  }

  const BayInfo bays = surveyBays(busRows[idx], n);

  // A marker column earns its width only if it actually distinguishes the
  // rows. At the Stadtbusbahnhof the bays differ and it is worth having; at
  // P+R every departure leaves from the same place, so a column of identical
  // "1"s would just steal room from the destinations.
  String firstSeen;
  bool anyBay = false, variedBay = false;
  for (uint8_t r = 0; r < n; r++) {
    const String bay = bayLabel(busRows[idx][r], bays);
    if (!bay.length()) continue;
    if (!anyBay) { anyBay = true; firstSeen = bay; }
    else if (bay != firstSeen) { variedBay = true; break; }
  }
  const int bayW = (anyBay && variedBay) ? 18 : 0;

  const int lineRight = 6 + bayW + 36;
  const int destX     = lineRight + 10;

  for (uint8_t r = 0; r < n && r < Config::BUS_ROWS_PER_PAGE; r++) {
    const int y = rowTop + r * rowH;
    if (y + rowH > H - 18) break;
    const Departure &item = busRows[idx][r];

    const uint16_t bg = (r & 1) ? rgb(22, 22, 22) : Config::BUS_BLACK;
    lcd.fillRect(0, y, W, rowH, bg);
    const int mid = y + rowH / 2;
    const uint16_t base = item.cancelled ? Config::CANCEL_RED : Config::BUS_AMBER;
    // Same status colours as the train board: green on time, red when late.
    const uint16_t timeColour = item.cancelled ? Config::CANCEL_RED
                              : (item.delayMinutes > 0 ? Config::LATE_RED
                                                       : Config::ON_TIME_GREEN);

    if (bayW) {
      const String bay = bayLabel(item, bays);
      if (bay.length()) {
        txt(clipped(bay, bayW - 2, 2), 4 + bayW / 2, mid, 2, D_MC,
            rgb(125, 125, 130), bg);
      }
    }

    // Line number, right-aligned so destinations line up down the board.
    txt(clipped(item.line, 36, font), lineRight, mid, font, D_MR, base, bg);

    // The right-hand block answers the actual question: when does it leave,
    // and was that the time it was meant to. A countdown on its own never
    // said what the clock time was, so a bus twenty minutes out was "20'"
    // and nothing else.
    //
    //   on time   10:34   9        (one time, green)
    //   late      10:27  10:30   3 (planned struck, revised red)
    //
    // The three columns are fixed, so nothing shifts from row to row.
    if (item.cancelled) {
      txt("X", Config::BUS_MIN_X, mid, font, D_MR, Config::CANCEL_RED, bg);
    } else {
      txt(minutesOrBlank(item), Config::BUS_MIN_X, mid, font, D_MR,
          timeColour, bg);
      txt(item.departureTime, Config::BUS_TIME_X, mid, font, D_MR,
          timeColour, bg);
      // 19 px of row is not enough to stack two readable lines, so the
      // planned time sits to the LEFT of the revised one, struck through.
      if (item.delayMinutes > 0 && item.scheduledTime.length() &&
          item.scheduledTime != item.departureTime) {
        const uint16_t planFg = rgb(148, 148, 156);
        txt(item.scheduledTime, Config::BUS_PLAN_X, mid, 20, D_MR, planFg, bg);
        setFontN(20);
        const int pw = lcd.textWidth(item.scheduledTime.c_str());
        lcd.drawFastHLine(Config::BUS_PLAN_X - pw, mid, pw, planFg);
      }
    }

    int destW = (Config::BUS_PLAN_X - 34) - destX;
    if (destW < 24) destW = 24;
    // Tidy the headsign first, then let it scroll if it is still too wide.
    const String dest = shortenDestination(item.destination, destW, font);
    if (item.cancelled) {
      txt(dest, destX, mid, font, D_ML, base, bg);
      setFontN(font);
      const int w = lcd.textWidth(asciiGerman(dest).c_str());
      lcd.drawFastHLine(destX, mid, w, Config::CANCEL_RED);
    } else {
      txtScroll(dest, destX, mid, destW, rowH - 2, font, base, bg);
    }
    revealPause();            // the rows land one after another on a swap
  }

  footerHealth(10, H - 9, Config::BUS_BLACK, rgb(120, 120, 125));
}

void drawDeparturePage(Departure *items, uint8_t itemCount, uint16_t page,
                       uint16_t pages, const String &title) {
  const Cols c = columns(false);
  drawTrainHeader(title);

  const uint8_t first = page * Config::ROWS_PER_PAGE;
  uint8_t drawn = 0;
  for (uint8_t row = 0; row < Config::ROWS_PER_PAGE; ++row) {
    const uint8_t index = first + row;
    if (index >= itemCount) break;
    const int y = Config::TR_ROW_TOP + row * Config::TR_ROW_H;
    if (y + Config::TR_ROW_H > c.H - 16) break;
    drawTrainRow(items[index], y, row);
    ++drawn;
    revealPause();            // the rows land one after another on a swap
  }

  if (drawn == 0) {
    txt(cycleCount ? "No departures" : "Loading...", c.W / 2, c.H / 2 - 16, 24,
        D_MC, Config::WHITE, Config::DB_BLUE);
    txt(clipped(statusText, c.W - 40, 21), c.W / 2, c.H / 2 + 20, 21,
        D_MC, Config::GREY, Config::DB_BLUE);
  }

  footerHealth(c.lineX + 4, c.H - 10, Config::DB_BLUE, Config::GREY);
  // Every train page: how punctual the trains have been, last 3 hours.
  drawDelayGraph(70, c.H - 4, 16, Config::DB_BLUE);
  // Small and quiet: it is a position marker, not a headline.
  if (pages > 1) {
    txt(String(page + 1) + "/" + String(pages), c.rightX, c.H - 10, 2,
        D_MR, rgb(92, 104, 140), Config::DB_BLUE);
  }
}

// Weather icon drawn with primitives - no bitmaps, no extra flash.
void drawWeatherIcon(int cx, int cy, int r, int code, bool night) {
  const uint16_t SUN   = rgb(255, 208,  64);
  const uint16_t CLOUD = rgb(226, 232, 245);
  const uint16_t DARK  = rgb(150, 162, 190);
  const uint16_t RAIN  = rgb( 96, 176, 255);
  const uint16_t SNOW  = rgb(235, 245, 255);
  const uint16_t BOLT  = rgb(255, 214,  51);

  const bool clear   = (code == 0 || code == 1);
  const bool cloudy  = (code == 2 || code == 3);
  const bool fog     = (code == 45 || code == 48);
  const bool rain    = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  const bool snow    = (code >= 71 && code <= 77) || code == 85 || code == 86;
  const bool storm   = (code >= 95);

  if (clear || cloudy) {                      // sun, or a moon after dark
    const int sx = cloudy ? cx - r / 3 : cx;
    if (night) {
      const uint16_t MOON = rgb(226, 232, 245);
      lcd.fillCircle(sx, cy - r / 6, r / 2, MOON);
      // Bite a crescent out of it with a disc in the background colour.
      lcd.fillCircle(sx + r / 4, cy - r / 3, r / 2, weatherBg);
    } else {
      lcd.fillCircle(sx, cy - r / 6, r / 2, SUN);
    }
    if (clear && !night) {
      for (int a = 0; a < 360; a += 45) {
        const float rad = a * 3.14159f / 180.0f;
        const int x1 = sx + (int)(cosf(rad) * (r * 0.62f));
        const int y1 = cy - r / 6 + (int)(sinf(rad) * (r * 0.62f));
        const int x2 = sx + (int)(cosf(rad) * (r * 0.90f));
        const int y2 = cy - r / 6 + (int)(sinf(rad) * (r * 0.90f));
        lcd.drawLine(x1, y1, x2, y2, SUN);
      }
    }
  }

  if (cloudy || rain || snow || storm || fog) {   // cloud body
    const int by = cy + r / 5;
    lcd.fillCircle(cx - r / 2, by, r / 3, CLOUD);
    lcd.fillCircle(cx + r / 4, by, r / 2.4, CLOUD);
    lcd.fillCircle(cx - r / 12, by - r / 4, r / 2.6, CLOUD);
    lcd.fillRect(cx - r / 2, by, r, r / 3, CLOUD);
    lcd.drawFastHLine(cx - r / 2, by + r / 3, r, DARK);
  }

  if (fog) {
    for (int i = 0; i < 3; i++)
      lcd.drawFastHLine(cx - r / 2, cy + r / 2 + i * 6, r, DARK);
  }
  if (rain) {
    for (int i = -1; i <= 1; i++)
      lcd.drawLine(cx + i * (r / 3), cy + r / 2,
                   cx + i * (r / 3) - 3, cy + r, RAIN);
  }
  if (snow) {
    for (int i = -1; i <= 1; i++)
      lcd.fillCircle(cx + i * (r / 3), cy + r * 0.75, 2, SNOW);
  }
  if (storm) {
    lcd.fillTriangle(cx, cy + r / 2, cx - r / 5, cy + r,
                     cx + r / 8, cy + r * 0.72, BOLT);
  }
}

bool isNightNow() {
  const int now = minutesOfDay(clockNow());
  const int up  = minutesOfDay(weather.sunrise);
  const int dn  = minutesOfDay(weather.sunset);
  if (now < 0 || up < 0 || dn < 0) return false;
  return now < up || now > dn;
}

// The sky behind both weather screens follows the day. Deep navy after dusk,
// a plum wash for the forty minutes either side of sunrise and sunset, a
// lighter blue in between. Nothing else about the pages changes - only the
// ground colour - so every foreground colour was picked to sit on all three.
uint16_t skyColour() {
  const int now = minutesOfDay(clockNow());
  const int up  = minutesOfDay(weather.sunrise);
  const int dn  = minutesOfDay(weather.sunset);
  if (now < 0 || up < 0 || dn < 0) return Config::WEATHER_BLUE;
  const int edge = 40;
  if (abs(now - up) <= edge || abs(now - dn) <= edge) return Config::WEATHER_TWILIGHT;
  if (now < up || now > dn) return Config::WEATHER_NIGHT;
  return Config::WEATHER_BLUE;
}

// The next twelve hours: rain probability as bars, temperature as a line over
// them, and a marker on the column that is happening right now.
void drawHourlyStrip(int x, int y, int w, int h) {
  const uint8_t n = weather.hourCount;
  if (n < 2) return;

  const uint16_t RAIN_BAR = rgb(38, 86, 146);
  const uint16_t TEMP_LINE = rgb(255, 190, 90);
  const uint16_t FRAME = rgb(64, 96, 150);
  const uint16_t NOW_MARK = rgb(255, 214, 51);

  float lowest = weather.hourTemp[0], highest = weather.hourTemp[0];
  for (uint8_t i = 1; i < n; i++) {
    if (weather.hourTemp[i] < lowest)  lowest  = weather.hourTemp[i];
    if (weather.hourTemp[i] > highest) highest = weather.hourTemp[i];
  }
  if (highest - lowest < 3.0f) {            // a flat day still needs a scale
    const float mid = (highest + lowest) / 2.0f;
    lowest = mid - 1.5f;
    highest = mid + 1.5f;
  }

  // The scale labels live in their own gutter on the right, so the plot can
  // use every pixel to its left without a number landing on the curve.
  const int gutter = 28;
  const int plotW = w - gutter;
  const int colW = plotW / n;

  // Index 0 is the hour we are in. A tinted column behind it, a lit left
  // edge, and its hour label picked out below: no text inside the plot, so
  // nothing can be mistaken for data.
  lcd.fillRect(x, y, colW, h, rgb(28, 42, 86));
  lcd.drawFastVLine(x, y, h + 3, NOW_MARK);

  lcd.drawFastHLine(x, y + h, plotW, FRAME);

  int previousX = 0, previousY = 0;
  for (uint8_t i = 0; i < n; i++) {
    const int cx = x + i * colW + colW / 2;

    const int barH = (int)(weather.hourRain[i] / 100.0f * (h - 6));
    if (barH > 0) {
      lcd.fillRect(cx - colW / 2 + 1, y + h - barH, colW - 2, barH, RAIN_BAR);
    }

    const float fraction = (weather.hourTemp[i] - lowest) / (highest - lowest);
    const int py = y + 10 + (int)((1.0f - fraction) * (h - 20));
    if (i > 0) lcd.drawLine(previousX, previousY, cx, py, TEMP_LINE);
    lcd.fillCircle(cx, py, 2, TEMP_LINE);
    previousX = cx; previousY = py;

    if (i % 3 == 0) {
      char label[4];
      snprintf(label, sizeof(label), "%02d", weather.hourOfDay[i]);
      txt(label, cx, y + h + 9, 2, D_MC,
          i == 0 ? NOW_MARK : rgb(124, 140, 178), weatherBg);
    }
  }

  txt(String(highest, 0) + "C", x + w, y + 6, 2, D_MR,
      rgb(255, 176, 128), weatherBg);
  txt(String(lowest, 0) + "C", x + w, y + h - 6, 2, D_MR,
      rgb(144, 202, 255), weatherBg);
}

// Shared top band for both weather screens.
void drawWeatherHeader(const String &title) {
  const int W = lcd.width();
  lcd.fillRect(0, 0, W, 24, Config::DB_HEADER);
  lcd.fillRect(0, 0, W, 2, Config::DB_HEADER_HI);
  txt(clipped(title, W - 70, 22), 6, 12, 22, D_ML, Config::BLACK, Config::DB_HEADER);
  headerClock(W - 6, 12, 21, Config::BLACK, Config::DB_HEADER);
}

// A small sun on the horizon with an arrow through it. Drawn rather than
// written, and laid out from a MEASURED text width anchored to one margin,
// which is what stops these two running off the panel.
void drawSunEvent(int x, int y, bool rising, const String &hhmmText,
                  bool rightAligned, uint16_t bg) {
  const uint16_t SUN = rgb(255, 200, 80);
  const uint16_t LINE = rgb(132, 146, 184);
  const int glyphW = 18;

  setFontN(21);
  const int textW = lcd.textWidth(asciiGerman(hhmmText).c_str());
  const int left = rightAligned ? (x - textW - glyphW - 6) : x;

  const int gx = left + glyphW / 2;
  lcd.fillCircle(gx, y - 1, 4, rising ? SUN : LINE);
  lcd.drawFastHLine(gx - 8, y + 5, 16, LINE);
  if (rising) {
    lcd.drawLine(gx + 7, y + 1, gx + 7, y - 6, SUN);
    lcd.drawLine(gx + 7, y - 6, gx + 4, y - 3, SUN);
    lcd.drawLine(gx + 7, y - 6, gx + 10, y - 3, SUN);
  } else {
    lcd.drawLine(gx + 7, y - 6, gx + 7, y + 1, LINE);
    lcd.drawLine(gx + 7, y + 1, gx + 4, y - 2, LINE);
    lcd.drawLine(gx + 7, y + 1, gx + 10, y - 2, LINE);
  }
  txt(hhmmText, left + glyphW + 6, y, 21, D_ML, rgb(198, 208, 232), bg);
}

// "12-24h", "at 15h", or "dry" - the answer to "when is it going to rain".
String rainWindowText(const WeatherDay &d) {
  if (d.rainFrom < 0) return "dry";
  if (d.rainTo - d.rainFrom <= 1) return "at " + String((int)d.rainFrom) + "h";
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%d-%dh", (int)d.rainFrom, (int)d.rainTo);
  return String(buffer);
}

// Screen one: right now, today's numbers, and the next twelve hours.
void drawWeather() {
  const int W = lcd.width(), H = lcd.height();
  const int cx = W / 2;

  weatherBg = skyColour();
  lcd.fillScreen(weatherBg);
  drawWeatherHeader(settings.geoCity.length() ? settings.geoCity
                                              : String(Config::WEATHER_LABEL));

  if (!weather.valid) {
    txt(cycleCount ? "No weather data" : "Loading...", cx, H / 2 - 10, 24, D_MC, Config::WHITE, weatherBg);
    txt(clipped(statusText, W - 24, 21), cx, H / 2 + 20, 21,
        D_MC, Config::GREY, weatherBg);
    footerHealth(10, H - 9, weatherBg, Config::GREY);
    return;
  }

  const WeatherDay &today = weather.day[0];
  const bool night = isNightNow();

  // ---- hero: the sky on the left, the number and the words on the right --
  drawWeatherIcon(38, 56, 20, weather.weatherCode, night);

  const float t = weather.temperature;
  const uint16_t tempColour =
      isnan(t) ? Config::WHITE
    : t <=  0 ? rgb(150, 200, 255)
    : t <= 10 ? rgb(200, 225, 255)
    : t <= 20 ? Config::WHITE
    : t <= 27 ? rgb(255, 210, 140)
              : rgb(255, 150, 110);

  // 18pt, and a fixed 18 px clear of the icon. A 24pt number 36 px from the
  // icon centre is what made these two touch in the first place.
  const int tempX = 78;
  const String tempText = String(t, 1);
  txt(tempText, tempX, 54, 25, D_ML, tempColour, weatherBg);
  setFontN(25);
  const int tempW = lcd.textWidth(tempText.c_str());
  const int ringX = tempX + tempW + 13;
  lcd.drawCircle(ringX, 42, 4, Config::DB_YELLOW);
  lcd.drawCircle(ringX, 42, 3, Config::DB_YELLOW);
  txt("C", ringX + 10, 46, 23, D_ML, Config::DB_YELLOW, weatherBg);

  // Description and the apparent temperature share one line - two lines of
  // small text under a big number read as clutter.
  String summary = weatherDescription(weather.weatherCode);
  if (!isnan(weather.apparentTemperature)) {
    summary += "  -  feels " + String(weather.apparentTemperature, 0) + "C";
  }
  txt(clipped(summary, W - tempX - 8, 22), tempX, 80, 22,
      D_ML, Config::DB_YELLOW, weatherBg);

  // ---- today's numbers, four evenly spaced cells ------------------------
  lcd.drawFastHLine(12, 92, W - 24, Config::ROW_LINE);
  const int valueY = 106, labelY = 122;
  const int cellX[4] = { 44, 122, 200, 278 };
  const uint16_t LABEL = rgb(150, 162, 190);

  if (!isnan(today.high) && !isnan(today.low)) {
    txt(String(today.high, 0) + " / " + String(today.low, 0), cellX[0], valueY,
        22, D_MC, Config::WHITE, weatherBg);
  }
  txt("high / low", cellX[0], labelY, 2, D_MC, LABEL, weatherBg);

  txt(today.rainChance >= 0 ? (String((int)today.rainChance) + "%") : String("-"),
      cellX[1], valueY, 22, D_MC, rgb(96, 176, 255), weatherBg);
  // The label carries the answer to "when", and the millimetres when there
  // are any - a bare percentage never told you whether to take a coat.
  String rainLabel = rainWindowText(today);
  if (today.rainFrom >= 0 && today.rainMm >= 0.1f) {
    rainLabel = String(today.rainMm, 0) + "mm " + rainLabel;
  } else if (today.rainFrom < 0) {
    rainLabel = "rain";
  }
  txt(clipped(rainLabel, 76, 2), cellX[1], labelY, 2, D_MC, LABEL, weatherBg);

  txt(String(weather.humidity) + "%", cellX[2], valueY, 22, D_MC,
      Config::WHITE, weatherBg);
  txt("humidity", cellX[2], labelY, 2, D_MC, LABEL, weatherBg);

  txt(String(weather.windSpeed, 0), cellX[3], valueY, 22, D_MC,
      Config::WHITE, weatherBg);
  // A compass point beats an arrow you have to decode. Kept to eight points
  // and no "from": the cell is 78 px wide and "km/h from SSW" does not fit.
  const String from = compassPoint(weather.windDirection);
  txt(clipped(from.length() ? ("km/h " + from) : String("km/h"), 76, 2),
      cellX[3], labelY, 2, D_MC, LABEL, weatherBg);

  revealPause();            // hero, then the numbers, then the strip

  // ---- sunrise and sunset, pinned to the two margins --------------------
  lcd.drawFastHLine(12, 132, W - 24, Config::ROW_LINE);
  if (today.sunrise.length()) drawSunEvent(14, 144, true, today.sunrise, false, weatherBg);
  if (today.sunset.length())  drawSunEvent(W - 14, 144, false, today.sunset, true, weatherBg);

  revealPause();

  // ---- the next twelve hours, given room to actually be read ------------
  drawHourlyStrip(12, 160, W - 24, 44);

  footerHealth(10, H - 9, weatherBg, Config::GREY);
  if (weather.updatedAt.length()) {
    txt(weather.updatedAt, W - 6, H - 9, 2, D_MR, rgb(104, 116, 152), weatherBg);
  }
}

// ---------------------------------------------------------------------
//  Pieces of the three-day outlook row.
// ---------------------------------------------------------------------

// A moon drawn the way the sky actually looks: a lit disc with a curved
// terminator swept across it. phase runs 0 (new) .. 0.5 (full) .. 1 (new).
void drawMoon(int cx, int cy, int r, float phase, uint16_t bg) {
  if (phase < 0.0f) return;
  const uint16_t lit  = rgb(238, 236, 214);
  const uint16_t dark = bg;
  const uint16_t rim  = rgb(118, 126, 150);

  lcd.fillCircle(cx, cy, r, lit);

  // k is +1 at new moon and -1 at full: the width of the terminator ellipse
  // and, by its sign, whether that ellipse is the shadow or the light.
  const float k = cosf(6.2831853f * phase);
  const bool waxing = phase < 0.5f;

  // Half the disc is always in shadow - the right half while waning, the
  // left half while waxing.
  for (int dy = -r; dy <= r; dy++) {
    const int half = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    if (half <= 0) continue;
    if (waxing) lcd.drawFastHLine(cx - half, cy + dy, half, dark);
    else        lcd.drawFastHLine(cx,        cy + dy, half, dark);
  }

  const int rx = (int)(r * fabsf(k) + 0.5f);
  if (rx > 0) lcd.fillEllipse(cx, cy, rx, r, (k > 0.0f) ? dark : lit);
  lcd.drawCircle(cx, cy, r, rim);
}

// Twenty-four cells, one per hour, lit where that hour is forecast wet. A
// percentage says how likely rain is; this says WHEN, which is the thing you
// actually plan around - and because it is a per-hour mask rather than a
// first-to-last span, two separate showers show up as two.
void drawRainBar(int x, int y, int w, int h, const WeatherDay &d) {
  const uint16_t dry  = rgb( 40,  48,  70);
  const uint16_t wet  = rgb( 96, 176, 255);
  const uint16_t tick = rgb( 96, 106, 134);
  // One continuous trough, then the wet hours on top of it. Drawing 24
  // separate dry cells turned a dry day into a dotted rule that read as
  // decoration rather than as an empty bar.
  lcd.fillRect(x, y, w, h, dry);
  for (uint8_t hour = 0; hour < 24; hour++) {
    if (!((d.rainMask >> hour) & 1u)) continue;
    const int x0 = x + (int)((long)w * hour / 24);
    const int x1 = x + (int)((long)w * (hour + 1) / 24);
    if (x1 > x0) lcd.fillRect(x0, y, x1 - x0, h, wet);
  }
  // Marks at 06:00, 12:00 and 18:00, so a wet block can be placed in the day
  // without counting cells.
  for (uint8_t hour = 6; hour < 24; hour += 6) {
    const int tx = x + (int)((long)w * hour / 24) - 1;
    lcd.drawFastVLine(tx, y + h, 3, tick);
  }
}

// One day of the outlook. Three bands: what it will be like, when the rain
// comes, and how the day itself is shaped.
void drawOutlookRow(const WeatherDay &d, int top, int rowH) {
  static const char *dayNames[] = { "Sun", "Mon", "Tue", "Wed",
                                    "Thu", "Fri", "Sat" };
  const int W = lcd.width();
  const uint16_t bg = weatherBg;
  const uint16_t DIM  = rgb(132, 144, 180);
  const uint16_t SUN_COL  = rgb(255, 200,  80);
  const uint16_t DUSK_COL = rgb(140, 154, 194);

  // ---- band one: the day, the sky, the temperatures --------------------
  const int mid1 = top + 12;
  const int8_t wd = d.weekday;
  txt(dayNames[wd >= 0 && wd < 7 ? wd : 0], 8, mid1, 22, D_ML,
      Config::DB_YELLOW, bg);
  txt(d.dayLabel, 50, mid1 + 1, 2, D_ML, DIM, bg);

  drawWeatherIcon(104, mid1, 11, d.code, false);

  if (!isnan(d.high)) {
    txt(String(d.high, 0) + "C", 164, mid1, 24, D_MR, rgb(255, 190, 130), bg);
  }
  if (!isnan(d.low)) {
    txt("/" + String(d.low, 0) + "C", 168, mid1 + 1, 21, D_ML,
        rgb(150, 205, 255), bg);
  }
  // Feels-like earns its place only when it disagrees with the real number.
  if (!isnan(d.feels) && !isnan(d.high) && fabsf(d.feels - d.high) >= 1.0f) {
    txt("feels " + String(d.feels, 0), 216, mid1 + 1, 2, D_ML, DIM, bg);
  }
  if (d.uv >= 0) {
    const uint16_t uvCol = d.uv <= 2 ? rgb(120, 210, 140)
                         : d.uv <= 5 ? rgb(240, 210,  90)
                         : d.uv <= 7 ? rgb(255, 160,  70)
                                     : rgb(255,  95,  95);
    txt("UV " + String((int)d.uv), W - 8, mid1 + 1, 2, D_MR, uvCol, bg);
  }

  // ---- band two: when it rains, and how much ---------------------------
  const int barY = top + 25;
  drawRainBar(8, barY, 216, 6, d);
  // "40% 1.2mm" when there is something to say, plain "dry" when there is
  // not. "0% dry" is two ways of saying nothing.
  String rainText;
  if (d.rainMask) {
    rainText = (d.rainChance >= 0 ? String((int)d.rainChance) + "%" : String(""));
    if (d.rainMm >= 0.1f) rainText += "  " + String(d.rainMm, 1) + "mm";
  } else {
    if (d.rainChance >= 15) rainText = String((int)d.rainChance) + "%  ";
    rainText += "dry";
  }
  txt(rainText, W - 8, barY + 3, 2, D_MR,
      d.rainMask ? rgb(150, 195, 240) : DIM, bg);

  // ---- band three: the shape of the day --------------------------------
  const int mid3 = top + 46;
  if (d.sunrise.length()) {
    lcd.fillCircle(13, mid3, 3, SUN_COL);
    lcd.drawLine(13, mid3 - 5, 13, mid3 - 8, SUN_COL);
    txt(d.sunrise, 22, mid3, 21, D_ML, rgb(214, 204, 176), bg);
  }
  if (d.sunset.length()) {
    lcd.fillCircle(79, mid3, 3, DUSK_COL);
    lcd.drawFastHLine(74, mid3 + 5, 11, DUSK_COL);
    txt(d.sunset, 88, mid3, 21, D_ML, rgb(172, 184, 216), bg);
  }
  int lightEnd = 146;
  if (d.daylightMin > 0) {
    String light = String(d.daylightMin / 60) + "h" +
                   (d.daylightMin % 60 < 10 ? "0" : "") +
                   String(d.daylightMin % 60);
    if (d.daylightDelta != 0) {
      light += d.daylightDelta > 0 ? " +" : " ";
      light += String((int)d.daylightDelta) + "m";
    }
    txt(light, 146, mid3 + 1, 2, D_ML, DIM, bg);
    // The wind block starts where this text actually ends, so "-3m" can
    // never run into the arrow.
    setFontN(2);
    lightEnd = 146 + lcd.textWidth(asciiGerman(light).c_str());
  }
  // Wind speed only. The direction arrow is gone from the outlook - it was
  // the one thing too many on this row.
  if (!isnan(d.windKmh)) {
    int wx = lightEnd + 12;
    if (wx < 218) wx = 218;
    txt(String(d.windKmh, 0) + " km/h", wx, mid3 + 1, 2, D_ML, DIM, bg);
  }
  drawMoon(300, mid3, 8, d.moonPhase, bg);
}

// Screen two: the three days after today. One row each - the sky and the
// temperatures, a twenty-four hour bar saying when the rain arrives, and the
// shape of the day underneath: first light, last light, how long that is and
// which way it is going, the wind, and the moon.
void drawWeatherOutlook() {
  const int W = lcd.width(), H = lcd.height();

  weatherBg = skyColour();
  lcd.fillScreen(weatherBg);
  drawWeatherHeader("Next 3 days");

  if (!weather.valid || weather.dayCount < 2) {
    txt("No forecast", W / 2, H / 2, 24, D_MC, Config::WHITE, weatherBg);
    footerHealth(10, H - 9, weatherBg, Config::GREY);
    return;
  }

  const int rowH = 62;

  for (uint8_t i = 1; i < weather.dayCount && i <= 3; i++) {
    const int top = 30 + (i - 1) * rowH;
    drawOutlookRow(weather.day[i], top, rowH);
    if (i < weather.dayCount - 1 && i < 3) {
      lcd.drawFastHLine(10, top + rowH - 4, W - 20, Config::ROW_LINE);
    }
    revealPause();            // one day at a time on a page change
  }

  footerHealth(10, H - 9, weatherBg, Config::GREY);
  drawTempHistory(44, H - 3, 14, weatherBg);   // the last 30 days' highs
  if (weather.updatedAt.length()) {
    txt(weather.updatedAt, W - 6, H - 9, 2, D_MR, rgb(104, 116, 152), weatherBg);
  }
}

// ---------------------------------------------------------------------
// The delay graph. Called under the data lock (the net task pushes, the
// loop draws).
// ---------------------------------------------------------------------
void pushDelaySample(const Departure *items, uint8_t n) {
  uint32_t pain = 0;                 // tenths of a minute, summed
  for (uint8_t i = 0; i < n; i++) {
    if (items[i].cancelled) { pain += 150; continue; }
    int16_t d = items[i].delayMinutes;
    if (d < 0) d = 0;
    if (d > 30) d = 30;
    pain += (uint32_t)d * 10;
  }
  uint32_t avg = n ? pain / n : 0;
  if (avg > 255) avg = 255;
  if (delayHistCount < DELAY_HIST) {
    delayHist[delayHistCount++] = (uint8_t)avg;
  } else {
    memmove(delayHist, delayHist + 1, DELAY_HIST - 1);
    delayHist[DELAY_HIST - 1] = (uint8_t)avg;
  }
}

// One pixel per refresh, newest on the right, growing leftwards as the
// history fills. Height follows the average delay (full height at 8 min);
// colour says how bad: green under 1.5 min, amber under 4, red above.
// Deliberately dim - it is a background trend, not a headline.
void drawDelayGraph(int x, int bottom, int h, uint16_t bg) {
  const int w = DELAY_HIST;
  lcd.fillRect(x, bottom - h + 1, w, h, bg);
  lcd.drawFastHLine(x, bottom, w, rgb(40, 56, 104));        // the baseline
  const int start = x + w - delayHistCount;
  for (uint8_t i = 0; i < delayHistCount; i++) {
    const uint8_t v = delayHist[i];                        // tenths of a min
    int bh = 1 + (int)v * (h - 1) / 80;
    if (bh > h) bh = h;
    const uint16_t col = v < 15 ? rgb(56, 150, 96)
                       : v < 40 ? rgb(200, 150, 60)
                                : rgb(214, 72, 72);
    lcd.drawFastVLine(start + i, bottom - bh + 1, bh, col);
  }
}

// The 30-day temperature graph on the 3-day page. One bar per day, oldest
// left, today on the right. Height is that day's high, scaled to the
// coldest and warmest day in the window.
//
// Colour follows the temperature smoothly rather than in five hard steps:
// a gradient through these stops, interpolated per degree.
//   -10 deep blue | 0 blue | 8 cyan | 15 green | 20 yellow | 25 orange | 32 red
// Each bar fades from its full colour at the top towards the sky at the
// bottom, faint lines mark each week back from today, and the warmest and
// coldest high of the window are printed on the left in warm/cool colours.
struct TempStop { int8_t t; uint8_t r, g, b; };
static const TempStop TEMP_STOPS[] = {
  { -10,  90, 110, 235 }, {  0,  80, 150, 240 }, {  8,  70, 200, 220 },
  {  15,  90, 205, 120 }, { 20, 215, 205,  80 }, { 25, 240, 150,  60 },
  {  32, 230,  70,  70 },
};

// The gradient colour for a temperature, as 8-bit channels.
static void tempRgb(float t, uint8_t &r, uint8_t &g, uint8_t &b) {
  const uint8_t n = sizeof(TEMP_STOPS) / sizeof(TEMP_STOPS[0]);
  if (t <= TEMP_STOPS[0].t) { r = TEMP_STOPS[0].r; g = TEMP_STOPS[0].g; b = TEMP_STOPS[0].b; return; }
  for (uint8_t i = 1; i < n; i++) {
    if (t <= TEMP_STOPS[i].t) {
      const TempStop &lo = TEMP_STOPS[i - 1], &hi = TEMP_STOPS[i];
      const float f = (t - lo.t) / (float)(hi.t - lo.t);
      r = (uint8_t)(lo.r + (hi.r - lo.r) * f);
      g = (uint8_t)(lo.g + (hi.g - lo.g) * f);
      b = (uint8_t)(lo.b + (hi.b - lo.b) * f);
      return;
    }
  }
  r = TEMP_STOPS[n - 1].r; g = TEMP_STOPS[n - 1].g; b = TEMP_STOPS[n - 1].b;
}

// RGB565 back to 8-bit channels, so the fade can blend towards the sky.
static void unpack565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = (uint8_t)(((c >> 11) & 0x1F) << 3);
  g = (uint8_t)(((c >> 5) & 0x3F) << 2);
  b = (uint8_t)((c & 0x1F) << 3);
}

void drawTempHistory(int x, int bottom, int h, uint16_t bg) {
  const int barW = 6, gap = 1, w = TEMP_DAYS * (barW + gap);
  lcd.fillRect(x, bottom - h + 1, w, h, bg);
  if (!tempHighCount) return;
  float lo = 1000, hi = -1000;
  for (uint8_t i = 0; i < tempHighCount; i++) {
    if (isnan(tempHigh[i])) continue;
    if (tempHigh[i] < lo) lo = tempHigh[i];
    if (tempHigh[i] > hi) hi = tempHigh[i];
  }
  if (hi < lo) return;

  // Warmest and coldest high of the window, right-aligned just left of the
  // bars (small face, so they clear the health dot).
  txt(String(hi, 0), x - 3, bottom - h + 4, 20, D_MR, rgb(255, 176, 128), bg);
  txt(String(lo, 0), x - 3, bottom - 2,     20, D_MR, rgb(144, 202, 255), bg);

  const float floorT = lo - 2.0f;          // the coldest day still shows a bar
  const float span = (hi - floorT) < 1.0f ? 1.0f : (hi - floorT);
  const int start = x + w - tempHighCount * (barW + gap);
  uint8_t br, bgG, bb;
  unpack565(bg, br, bgG, bb);

  for (uint8_t i = 0; i < tempHighCount; i++) {
    if (isnan(tempHigh[i])) continue;
    int bh = 2 + (int)((tempHigh[i] - floorT) * (h - 2) / span);
    if (bh > h) bh = h;
    const int bx = start + i * (barW + gap);
    uint8_t r, g, b;
    tempRgb(tempHigh[i], r, g, b);
    // Full colour at the top, 35 % of the way to the sky at the bottom.
    for (int k = 0; k < bh; k++) {
      const float f = 0.35f + 0.65f * (1.0f - (float)k / (float)(bh > 1 ? bh - 1 : 1));
      lcd.drawFastHLine(bx, bottom - bh + 1 + k, barW,
                        rgb((uint8_t)(br  + (r - br)  * f),
                            (uint8_t)(bgG + (g - bgG) * f),
                            (uint8_t)(bb  + (b - bb)  * f)));
    }
    // Today, the last bar, gets a white cap so "now" is findable.
    if (i == tempHighCount - 1) lcd.drawFastHLine(bx, bottom - bh + 1, barW, Config::WHITE);
  }

  // A faint line one week, two weeks, three and four weeks back from today.
  for (uint8_t wk = 1; wk <= 4; wk++) {
    const int lx = x + w - wk * 7 * (barW + gap) - 1;
    if (lx > x) lcd.drawFastVLine(lx, bottom - h + 1, h, rgb(28, 44, 90));
  }
}

// The feeds the page on screen is drawn from, as DIRTY_* bits.
uint8_t screenFeeds() {
  const uint16_t trainPages = trainPageCount();
  const uint16_t idx = screenIndex % totalScreens();
  if (idx < trainPages) return DIRTY_TRAINS;
  const uint16_t rest = idx - trainPages;
  if (rest < Config::BUS_STOP_COUNT) return (uint8_t)(DIRTY_BUS0 << rest);
  return DIRTY_WEATHER;
}

// Up to eight screens: 1-4 train pages, one per bus stop, two of weather.
void drawCurrentScreen() {
  marqueeCount = 0;                 // this screen registers its own
  const uint16_t trainPages = trainPageCount();
  screenIndex %= totalScreens();
  if (screenIndex < trainPages) {
    drawDeparturePage(trains, trainCount, screenIndex, trainPages,
                      Config::TRAIN_LABEL);
    return;
  }
  switch (screenIndex - trainPages) {
    case 0:  drawBusBoard(0);         break;   // Bahnhof Stadtbus
    case 1:  drawBusBoard(1);         break;   // P+R-Platz
    case 2:  drawWeather();           break;
    default: drawWeatherOutlook();    break;
  }
}

// ---------------------------------------------------------------------
//  The page transition.
//
//  There is no spare RAM on a C3 for a second frame buffer - a full 320x240
//  one is 150 kB - so nothing can be slid or cross-faded. What IS free is
//  the order and the timing of the drawing that has to happen anyway, and
//  that turns out to be enough:
//
//    1. the old page is wiped off top to bottom behind a moving edge
//    2. the new page is drawn in its normal single pass, but each row hands
//       control back for a few milliseconds, so the rows land one after
//       another instead of the whole page appearing at once
//
//  Total is about 200 ms, it allocates nothing, and it draws every pixel
//  exactly once - the old version of this redrew the whole page twenty
//  times for one tap.
// ---------------------------------------------------------------------

// Called by the page painters between rows. Closing the transaction is what
// makes the panel show what has been drawn so far; without it everything is
// still queued and the pause buys nothing.
void revealPause() {
  if (!revealStep) return;
  lcd.endWrite();
  delay(revealStep);
  lcd.startWrite();
}

void wipeScreen(uint16_t colour) {
  const int W = lcd.width(), H = lcd.height();
  const int band = 15;
  // A lit edge running ahead of the wipe. Without it the screen just gets
  // shorter; with it, something is clearly moving.
  const uint16_t edge = rgb(120, 132, 160);
  for (int y = 0; y < H; y += band) {
    const int h = (y + band > H) ? (H - y) : band;
    lcd.startWrite();
    lcd.fillRect(0, y, W, h, colour);
    if (y + h < H) lcd.drawFastHLine(0, y + h, W, edge);
    lcd.endWrite();
    delay(5);
  }
  // Take the last edge line off again.
  lcd.fillRect(0, H - 1, W, 1, colour);
}

void renderScreen(bool animate) {
  if (animate) wipeScreen(Config::BLACK);

  lockData();
  // One SPI transaction for the whole page. Without this every fill and every
  // string re-acquires the bus and re-asserts CS, which is a surprising share
  // of the time a page takes to appear.
  lcd.startWrite();
  clockX = -1;                       // the new page registers its own
  healthX = -1;
  revealStep = animate ? Config::REVEAL_STEP_MS : 0;
  drawCurrentScreen();
  revealStep = 0;
  lcd.endWrite();
  unlockData();
  screenShownAt = millis();
}
