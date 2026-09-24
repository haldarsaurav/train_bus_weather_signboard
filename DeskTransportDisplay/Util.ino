#include "DeskDisplay.h"

// =====================================================================
//  Train Bus Weather Signboard  -  firmware v1.0.0
//  Copyright (c) 2026 Sam (haldarsaurav). All rights reserved.
//  No use, copying, modification or redistribution - and no reproduction
//  with AI tools - without written permission. See LICENSE.
//
//  Small shared helpers: text, time, sorting, formatting, page counts.
//  Nothing in this tab touches the network or the panel.
// =====================================================================

// The classic bitmap fonts are ASCII only, so UTF-8 umlauts arrive as two
// bytes and draw as boxes. Transliterate the German way (ue/oe/ae/ss), which
// is what the rest of this sketch already does for its own strings.
String asciiGerman(const String &in) {
  String out;
  out.reserve(in.length() + 4);
  for (uint16_t i = 0; i < in.length(); i++) {
    const uint8_t c = (uint8_t)in[i];
    if (c < 0x80) { out += (char)c; continue; }
    const uint8_t d = (i + 1 < in.length()) ? (uint8_t)in[i + 1] : 0;
    if (c == 0xC3) {
      i++;
      switch (d) {
        case 0xA4: out += "ae"; break;   // a-umlaut
        case 0xB6: out += "oe"; break;   // o-umlaut
        case 0xBC: out += "ue"; break;   // u-umlaut
        case 0x84: out += "Ae"; break;
        case 0x96: out += "Oe"; break;
        case 0x9C: out += "Ue"; break;
        case 0x9F: out += "ss"; break;   // sharp s
        case 0xA9: case 0xA8: out += "e"; break;
        case 0xA0: case 0xA1: out += "a"; break;
        default:   out += '?';           break;
      }
    } else if (c == 0xC2) {
      i++;
      switch (d) {
        case 0xB3: out += '3';  break;    // superscript three, as in "1,1 m3"
        case 0xB2: out += '2';  break;
        case 0xB0: out += "deg"; break;
        case 0xA0: out += ' ';  break;    // non-breaking space
        default:   out += '?';  break;
      }
    } else if (c >= 0xC0) {
      i++;                                // skip other 2-byte sequences
      out += '?';
    }
  }
  return out;
}

// 21-26 are the real proportional GFX faces and are what the board uses now.
// The old bitmap fonts (0/1/2/4/6/8) and the seven-segment face (7) are kept
// only so the setup portal screens still compile - no board draws with them.
// Takes the target rather than assuming the panel, so the same call works
// on the scratch sprite.
void setFontOn(LovyanGFX &g, int n) {
  switch (n) {
    case 20: g.setFont(&fonts::DejaVu9);            return;   // small: delays
    case 21: g.setFont(&fonts::FreeSans9pt7b);      return;
    case 22: g.setFont(&fonts::FreeSansBold9pt7b);  return;
    case 23: g.setFont(&fonts::FreeSans12pt7b);     return;
    case 24: g.setFont(&fonts::FreeSansBold12pt7b); return;
    case 25: g.setFont(&fonts::FreeSansBold18pt7b); return;
    case 26: g.setFont(&fonts::FreeSansBold24pt7b); return;
    default: break;
  }
  switch (n) {
    case 2:  g.setFont(&fonts::Font2); break;
    case 4:  g.setFont(&fonts::Font4); break;
    case 6:  g.setFont(&fonts::Font6); break;
    case 7:  g.setFont(&fonts::Font7); break;
    case 8:  g.setFont(&fonts::Font8); break;
    default: g.setFont(&fonts::Font0); break;
  }
}

void setFontN(int n) { setFontOn(lcd, n); }

// Left-aligned text inside a fixed width. If it fits, this is an ordinary
// draw. If it does not, the full text is registered so the loop can scroll
// it, and what is drawn now is the clipped version.
void txtScroll(const String &value, int x, int y, int w, int h,
               uint8_t font, uint16_t fg, uint16_t bg) {
  const String flat = asciiGerman(value);
  setFontN(font);
  const int textW = lcd.textWidth(flat.c_str());
  if (!settings.marqueeEnabled || textW <= w || marqueeCount >= MAX_MARQUEE) {
    txt(clipped(value, w, font), x, y, font, D_ML, fg, bg);
    return;
  }
  Marquee &m = marquees[marqueeCount++];
  m.x = x; m.y = y; m.w = w; m.h = h;
  m.textW = textW;
  m.offset = 0;
  m.font = font;
  m.fg = fg; m.bg = bg;
  m.text = flat;
  txt(clipped(value, w, font), x, y, font, D_ML, fg, bg);
}

// Called from the loop. Nothing moves for the first second after a screen
// appears, so a glance at a still board is always readable.
void updateMarquees() {
  if (!settings.marqueeEnabled || !marqueeCount) return;
  const uint32_t now = millis();
  if (now - screenShownAt < 1000) return;
  // 2 px every 55 ms rather than 1 px every 33 ms: the same speed across the
  // screen for a bit under half the SPI traffic, which on a board this warm
  // is worth more than the extra smoothness.
  if (now - lastMarqueeStepAt < 55) return;
  lastMarqueeStepAt = now;

  for (uint8_t i = 0; i < marqueeCount; i++) {
    Marquee &m = marquees[i];
    const int16_t span = m.textW - m.w;
    const int16_t pause = 18;                   // ~1 s held at each end
    m.offset += 2;
    if (m.offset > span + 2 * pause) m.offset = -pause;
    int16_t shift = m.offset;
    if (shift < 0) shift = 0;
    if (shift > span) shift = span;

    const int top = m.y - m.h / 2;

    if (scratchReady && m.w <= SCRATCH_W && m.h <= SCRATCH_H) {
      // Off-screen, then one push: the row never shows its own background.
      scratch.fillScreen(SCRATCH_KEY);
      scratch.fillRect(0, 0, m.w, m.h, m.bg);
      scratch.setClipRect(0, 0, m.w, m.h);
      setFontOn(scratch, m.font);
      scratch.setTextDatum(D_ML);
      scratch.setTextColor(m.fg, m.bg);
      scratch.drawString(m.text.c_str(), -shift, m.h / 2);
      scratch.clearClipRect();
      scratch.pushSprite(&lcd, m.x, top, SCRATCH_KEY);
      continue;
    }

    lcd.setClipRect(m.x, top, m.w, m.h);
    lcd.fillRect(m.x, top, m.w, m.h, m.bg);
    setFontN(m.font);
    lcd.setTextDatum(D_ML);
    lcd.setTextColor(m.fg, m.bg);
    lcd.drawString(m.text.c_str(), m.x - shift, m.y);
    lcd.clearClipRect();
  }
}

// One call site for every string drawn, so styling stays consistent.
void txtOn(LovyanGFX &g, const String &value, int x, int y, int font,
           uint8_t datum, uint16_t fg, uint16_t bg) {
  setFontOn(g, font);
  g.setTextDatum(datum);
  g.setTextColor(fg, bg);
  g.drawString(asciiGerman(value).c_str(), x, y);
}

// One call site for every string drawn on the panel itself.
void txt(const String &value, int x, int y, int font,
         uint8_t datum, uint16_t fg, uint16_t bg) {
  txtOn(lcd, value, x, y, font, datum, fg, bg);
}

// Both edges, debounced here. Reads the pin straight from the GPIO register
// and the time from esp_timer - both safe in an interrupt, unlike
// digitalRead() and (on this core) millis().
//
//   press   : counts once the pin goes low >= DEBOUNCE_MS after the last release
//   release : counts once it goes high >= DEBOUNCE_MS after the press began;
//             a press shorter than HOLD_HINT_MS is a tap
//
// Contact bounce inside those windows is ignored, so one press is one tap.
void IRAM_ATTR buttonIsr() {
  const bool low = (REG_READ(GPIO_IN_REG) & BIT(Config::MODE_BUTTON_PIN)) == 0;
  const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if (low) {
    if (!btnDown && now - btnUpAt >= Config::DEBOUNCE_MS) {
      btnDown = true;
      btnDownAt = now;
    }
  } else if (btnDown && now - btnDownAt >= Config::DEBOUNCE_MS) {
    btnDown = false;
    btnUpAt = now;
    if (now - btnDownAt < Config::HOLD_HINT_MS) btnTap = true;
  }
}

bool modePinDown() {
  return digitalRead(Config::MODE_BUTTON_PIN) == LOW;
}

String htmlEscape(const String &value) {
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("\"", "&quot;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  return escaped;
}

String jsonEscape(const String &value) {
  String escaped = value;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  escaped.replace("\n", "\\n");
  escaped.replace("\r", "");
  return escaped;
}

String urlEncode(const String &value) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

int64_t daysFromCivil(int year, uint32_t month, uint32_t day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yearOfEra = static_cast<uint32_t>(year - era * 400);
  const uint32_t dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const uint32_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

time_t isoEpoch(const char *isoTime) {
  if (!isoTime) return 0;
  int year, month, day, hour, minute, second;
  if (sscanf(isoTime, "%d-%d-%dT%d:%d:%d", &year, &month, &day,
             &hour, &minute, &second) != 6) return 0;
  time_t epoch = static_cast<time_t>(daysFromCivil(year, month, day) * 86400LL +
                   hour * 3600LL + minute * 60LL + second);
  const char *zone = isoTime + 19;
  if ((*zone == '+' || *zone == '-') && strlen(zone) >= 6) {
    const int offsetSeconds = (String(zone + 1).substring(0, 2).toInt() * 60 +
                               String(zone + 1).substring(3, 5).toInt()) * 60;
    epoch += (*zone == '+') ? -offsetSeconds : offsetSeconds;
  }
  return epoch;
}

// A unix timestamp from Open-Meteo, rendered as local "HH:MM". The API
// returns true UTC epochs and tells us the offset, so this needs no zone
// database and cannot drift.
String localHhmm(int64_t utcEpoch, long utcOffset) {
  if (!utcEpoch) return "";
  const int64_t local = utcEpoch + utcOffset;
  const int minutes = (int)((local % 86400) / 60);
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes / 60, minutes % 60);
  return String(buffer);
}

String hhmm(const char *isoTime) {
  if (!isoTime) return "--:--";
  const time_t epoch = isoEpoch(isoTime);
  if (epoch > 0) {
    struct tm local;
    localtime_r(&epoch, &local);
    char text[6];
    snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
    return text;
  }
  const String value(isoTime);
  const int separator = value.indexOf('T');
  if (separator < 0 || value.length() < static_cast<size_t>(separator + 6)) return "--:--";
  return value.substring(separator + 1, separator + 6);
}

bool isRailMode(const String &mode) {
  return mode == "HIGHSPEED_RAIL" || mode == "LONG_DISTANCE" ||
         mode == "NIGHT_RAIL" || mode == "REGIONAL_RAIL" || mode == "SUBURBAN";
}

// A street-level stop board should show buses, coaches and trams - not just
// the single literal mode "BUS", which was dropping everything else.
bool isBusMode(const String &mode) {
  return mode == "BUS" || mode == "COACH" || mode == "TRAM" ||
         mode == "OTHER" || mode == "METRO";
}

// One stop to a page, ten departures. Fixed rather than derived, so the
// fetch asks for exactly what the page can show and not one row more.
uint8_t busRowsPerScreen() {
  return Config::BUS_ROWS_PER_PAGE;
}

uint16_t pageCount(uint8_t itemCount) {
  const uint16_t pages = (itemCount + Config::ROWS_PER_PAGE - 1) / Config::ROWS_PER_PAGE;
  return pages ? pages : 1;
}

uint16_t trainPageCount() {
  uint16_t pages = pageCount(trainCount);
  if (pages > Config::MAX_TRAIN_PAGES) pages = Config::MAX_TRAIN_PAGES;
  return pages;
}

uint16_t totalScreens() {
  return trainPageCount() + Config::BUS_STOP_COUNT + EXTRA_SCREENS;
}

String clipped(String text, uint16_t width, uint8_t font) {
  text = asciiGerman(text);
  setFontN(font);
  if (lcd.textWidth(text.c_str()) <= width) return text;
  while (text.length() > 2 && lcd.textWidth((text + "..").c_str()) > width) {
    text.remove(text.length() - 1);
  }
  return text + "..";
}

String clockNow() {
  struct tm t;
  if (!getLocalTime(&t, 5)) return "";
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

// The feeds are not reliably ordered once results span midnight.
void sortDepartures(Departure *items, uint8_t count) {
  for (uint8_t i = 1; i < count; i++) {
    Departure key = items[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && items[j].epoch > key.epoch) {
      items[j + 1] = items[j];
      j--;
    }
    items[j + 1] = key;
  }
}

// "Mo" / "Tu" / ... in front of anything that does not leave today. Without
// it a stop with no weekend service shows a bare "05:51", which reads as if
// the bus were due shortly rather than on Monday morning.
String dayPrefix(time_t epoch) {
  if (epoch <= 0) return "";
  const time_t nowEpoch = time(nullptr);
  if (nowEpoch < 100000) return "";
  struct tm departure, now;
  localtime_r(&epoch, &departure);
  localtime_r(&nowEpoch, &now);
  if (departure.tm_yday == now.tm_yday && departure.tm_year == now.tm_year) return "";
  static const char *names[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
  return String(names[departure.tm_wday % 7]) + " ";
}

// Minutes until a departure, worked out from its timestamp rather than from
// the printed "HH:MM". The string version could not tell yesterday from
// tomorrow, so a bus that had already left reappeared as a future one.
// Returns -1 when it cannot be known (no clock yet, or no timestamp).
long minutesToDeparture(const Departure &item) {
  const time_t now = time(nullptr);
  if (now < 100000) return -1;
  if (item.epoch > 0) return (long)((item.epoch - now + 30) / 60);

  // Fallback for a feed that gave a time but no timestamp.
  if (item.departureTime.length() != 5) return -1;
  struct tm nowTm;
  if (!getLocalTime(&nowTm, 10)) return -1;
  const int departureMinutes = item.departureTime.substring(0, 2).toInt() * 60 +
                               item.departureTime.substring(3, 5).toInt();
  int remaining = departureMinutes - (nowTm.tm_hour * 60 + nowTm.tm_min);
  if (remaining < -180) remaining += 1440;      // just after midnight
  return remaining;
}

// Up to an hour away the countdown in minutes is what is useful. Past that a
// countdown stops meaning anything, so the clock time is shown instead, with
// the weekday in front of it when it is not today.
String countdownText(const Departure &item) {
  const long mins = minutesToDeparture(item);
  if (mins >= 0 && mins <= 60) return String(mins);
  return dayPrefix(item.epoch) + item.departureTime;
}

// For the train board, which has its own time column: blank past the hour so
// the two columns can never print the same value on top of each other.
String minutesOrBlank(const Departure &item) {
  const long mins = minutesToDeparture(item);
  if (mins >= 0 && mins <= 60) return String(mins);
  return "";
}

// A departure that has already gone is not news. Anything more than a minute
// in the past is removed, so a failed refresh cannot leave the board showing
// buses that left an hour ago.
uint8_t dropPastDepartures(Departure *items, uint8_t count) {
  const time_t now = time(nullptr);
  if (now < 100000) return count;
  uint8_t kept = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (items[i].epoch > 0 && items[i].epoch < now - 60) continue;
    if (kept != i) items[kept] = items[i];
    kept++;
  }
  return kept;
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

// Bus headsigns arrive like "Freising (S) [P+R] ue. Isarstrasse". The bay note
// in brackets is noise on a stop board, so it always goes. The " ue. <via>"
// tail only goes when the name still will not fit.
// This works on the transliterated text: the raw feed writes the via marker
// with a u-umlaut, so the old search for " ue. " ran before the transliteration
// and never matched - nothing was ever dropped.
String shortenDestination(String dest, uint16_t width, uint8_t font) {
  dest = asciiGerman(dest);

  int open = dest.indexOf('[');
  while (open >= 0) {
    const int close = dest.indexOf(']', open);
    if (close < 0) break;
    dest = dest.substring(0, open) + dest.substring(close + 1);
    open = dest.indexOf('[');
  }
  dest.replace("  ", " ");
  dest.trim();

  setFontN(font);
  if (lcd.textWidth(dest.c_str()) <= width) return dest;
  const int via = dest.indexOf(" ue. ");
  if (via > 0) {
    dest = dest.substring(0, via);
    dest.trim();
    if (lcd.textWidth(dest.c_str()) <= width) return dest;
  }
  return clipped(dest, width, font);
}

String cleanPlatform(String value) {
  value.trim();
  const char *prefixes[] = { "Gleis ", "Gleis", "Gl. ", "Gl.", "Gl ", "Gl" };
  for (uint8_t i = 0; i < 6; i++) {
    if (value.startsWith(prefixes[i])) {
      value = value.substring(strlen(prefixes[i]));
      break;
    }
  }
  value.trim();

  // Freising reports the S-Bahn end of platform 3 as "3-S" (the feed's name
  // for a platform section). The badge already says S1, so drop any "-X" or
  // " X" letter suffix after the number: "3-S" -> "3", "3 S" -> "3".
  // A plain "3a" / "10b" is left alone - that is a real platform name.
  int cut = -1;
  for (int k = 1; k < (int)value.length(); k++) {
    const char ch = value[k];
    if ((ch == '-' || ch == ' ') && isDigit(value[k - 1])) { cut = k; break; }
  }
  if (cut > 0) {
    bool lettersOnly = true;
    for (int k = cut + 1; k < (int)value.length(); k++) {
      if (!isAlpha(value[k]) && value[k] != ' ') { lettersOnly = false; break; }
    }
    if (lettersOnly) value = value.substring(0, cut);
  }
  value.trim();
  return value;
}

String weatherDescription(int code) {
  if (code == 0) return "Clear";
  if (code <= 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 95) return "Thunderstorm";
  return "Weather";
}

// True once the clock is past sunset or before sunrise. Used for the icon.
// Minutes past local midnight, or -1 before the clock has synced.
int minutesOfDay(const String &hhmmText) {
  if (hhmmText.length() != 5) return -1;
  return hhmmText.substring(0, 2).toInt() * 60 + hhmmText.substring(3, 5).toInt();
}

// 8 points is enough to be useful and short enough to never need clipping.
String compassPoint(int16_t degrees) {
  if (degrees < 0) return "";
  static const char *points[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  const int index = ((degrees + 22) / 45) % 8;
  return points[index];
}
