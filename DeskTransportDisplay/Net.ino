#include "DeskDisplay.h"

// =====================================================================
//  Train Bus Weather Signboard  -  firmware v1.0.0
//  Copyright (c) 2026 Sam (haldarsaurav). All rights reserved.
//  No use, copying, modification or redistribution - and no reproduction
//  with AI tools - without written permission. See LICENSE.
//
//  Everything that talks to the outside world, and the task it runs on.
//
//  No function here draws anything. The panel belongs to the loop task
//  alone, which is the whole reason a slow feed no longer freezes it.
// =====================================================================

// Logs the association handshake step by step. If STACONNECTED appears but
// STAIPASSIGNED never does, the association worked and DHCP is the failure.
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      apClients = WiFi.softAPgetStationNum();
      Serial.printf("[AP] client ASSOCIATED   (now %d)\n", apClients);
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.println("[AP] client GOT AN IP  <- join succeeded");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      apClients = WiFi.softAPgetStationNum();
      Serial.printf("[AP] client LEFT         (now %d)\n", apClients);
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[AP] started");
      break;
    default:
      break;
  }
}

bool connectWifi() {
  if (settings.ssid.isEmpty() && strlen(WIFI_FALLBACK_SSID) > 0) {
    settings.ssid = WIFI_FALLBACK_SSID;
    settings.password = WIFI_FALLBACK_PASS;
    Serial.printf("Using hard-coded Wi-Fi: %s\n", WIFI_FALLBACK_SSID);
  }
  if (settings.ssid.isEmpty()) {
    wifiFailReason = "no network saved";
    return false;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                    // modem sleep hurts weak antennas
  WiFi.setTxPower(WIFI_POWER_11dBm);       // steadier than full power here
  WiFi.setHostname(Config::HOST_NAME);
  delay(200);

  // Scan first. The ESP32-C3 has NO 5 GHz radio, so a 5 GHz-only SSID can
  // never be joined - and that failure is otherwise indistinguishable from a
  // wrong password. Scanning says which it is.
  Serial.println();
  Serial.printf("Looking for \"%s\" on 2.4 GHz...\n", settings.ssid.c_str());
  feedWatchdog();
  const int found = WiFi.scanNetworks(false, true);
  feedWatchdog();
  int best = -1;
  for (int i = 0; i < found; i++) {
    Serial.printf("   %-32s ch%-3d %4d dBm %s\n",
                  WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "");
    if (WiFi.SSID(i) == settings.ssid &&
        (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
  }

  if (found <= 0) {
    wifiFailReason = "radio sees no networks";
    Serial.println("   >>> the radio sees NOTHING - antenna/RF fault");
    WiFi.scanDelete();
    return false;
  }

  if (best < 0) {
    wifiFailReason = "\"" + settings.ssid + "\" not on 2.4 GHz";
    Serial.printf("   >>> \"%s\" IS NOT among the %d networks found.\n",
                  settings.ssid.c_str(), found);
    Serial.println("   >>> Most likely it is a 5 GHz-only SSID. The ESP32-C3");
    Serial.println("   >>> has no 5 GHz radio. Use the 2.4 GHz network name.");
    WiFi.scanDelete();
    return false;
  }

  const int rssi = WiFi.RSSI(best);
  const int chan = WiFi.channel(best);
  Serial.printf("   found it: ch%d, %d dBm\n", chan, rssi);
  WiFi.scanDelete();

  // Up to three attempts, 20 s each.
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("   connect attempt %d...\n", attempt);
    WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
    const uint32_t startedAt = millis();
    while (millis() - startedAt < 20000) {
      const wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) {
        Serial.printf("   CONNECTED  ip=%s  rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        wifiFailReason = "";
        return true;
      }
      if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
        Serial.printf("   status=%d (likely wrong password)\n", (int)st);
        break;
      }
      // Joining can take a full minute across three attempts. MODE is
      // watched the whole time, so a 3 s hold opens setup without waiting
      // for Wi-Fi to give up first. (bootHoldForSetup feeds the watchdog.)
      if (bootHoldForSetup(250)) {
        setupHeldAtBoot = true;
        WiFi.disconnect(true);
        return false;
      }
    }
    WiFi.disconnect(true);
    if (bootHoldForSetup(500)) { setupHeldAtBoot = true; return false; }
  }

  wifiFailReason = "wrong password? (" + String(rssi) + " dBm)";
  Serial.println("   FAILED after 3 attempts. Signal was fine, so the");
  Serial.println("   password is the most likely cause.");
  return false;
}

// ---------------------------------------------------------------------
// ArduinoJson reads a Stream by calling read(), and WiFiClientSecure answers
// -1 the moment its buffer runs dry - even while the server is still
// sending. ArduinoJson takes that for end-of-input and stops mid-document,
// which is the "IncompleteInput" that emptied every transit board.
//
// Open-Meteo never showed it because its reply is small enough to arrive in
// one burst. Transitous sends no Content-Length, so the body ends only when
// the connection closes, and its reply arrives in several TLS records with
// gaps between them - exactly the case this breaks on.
//
// So: wait for the next byte rather than assume there isn't one. EOF is
// reported only when the socket is genuinely closed and drained, or the
// deadline passes.
// ---------------------------------------------------------------------
class PatientStream : public Stream {
public:
  PatientStream(WiFiClient &client, uint32_t timeoutMs)
    : _client(client), _timeout(timeoutMs) {}

  int available() override { return _client.available(); }
  int peek() override      { return _client.peek(); }
  void flush() override    {}
  size_t write(uint8_t) override { return 0; }

  int read() override {
    const uint32_t startedAt = millis();
    uint32_t lastFed = startedAt;
    for (;;) {
      const int c = _client.read();
      if (c >= 0) return c;
      // Genuinely finished: the peer closed and there is nothing buffered.
      if (!_client.connected() && _client.available() <= 0) return -1;
      const uint32_t waited = millis() - startedAt;
      if (waited >= _timeout) return -1;
      if (millis() - lastFed > 500) { lastFed = millis(); feedWatchdog(); }
      delay(1);
    }
  }

private:
  WiFiClient &_client;
  uint32_t    _timeout;
};

bool getJson(const String &url, JsonDocument &document, JsonDocument *filter) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Every call builds its own TLS context, which is the largest allocation
  // this firmware makes. Refusing the call beats an out-of-memory reset.
  // Total free heap is not enough on its own: TLS needs one big block, and
  // after hours of Strings the heap can be plenty but in small pieces. An
  // allocation failure there is a crash, a skipped fetch is not.
  if (ESP.getFreeHeap() < Config::MIN_FREE_HEAP ||
      ESP.getMaxAllocHeap() < Config::MIN_BLOCK) {
    Serial.printf("Skipping fetch: heap %u free, largest block %u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return false;
  }

  for (int attempt = 1; attempt <= 2; attempt++) {
    feedWatchdog();
    // https for the feeds; plain http only for the one-off location lookup
    // (ip-api.com's free tier is http-only).
    const bool tls = url.startsWith("https://");
    WiFiClientSecure secure;
    WiFiClient plain;
    if (tls) {
      // A desk prototype can use this without storing a changing CA bundle.
      // Do not reuse this helper for secrets or authenticated calls.
      secure.setInsecure();
      secure.setHandshakeTimeout(Config::TLS_HANDSHAKE_S);
    }
    WiFiClient &client = tls ? static_cast<WiFiClient &>(secure) : plain;

    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(7000);
    if (!http.begin(client, url)) return false;

    // HTTP/1.0 stops the server using chunked transfer encoding. ArduinoJson
    // reads the socket directly and cannot decode chunk framing, which shows
    // up as "InvalidInput" even though the request succeeded.
    http.useHTTP10(true);
    http.addHeader("User-Agent", Config::USER_AGENT);
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");   // never gzip

    const char *wanted[] = { "Content-Type", "Content-Encoding", "Transfer-Encoding" };
    http.collectHeaders(wanted, 3);

    const uint32_t startedAt = millis();
    const int responseCode = http.GET();

    if (responseCode != HTTP_CODE_OK) {
      Serial.printf("HTTP %d (try %d): %s\n", responseCode, attempt, url.c_str());
      http.end();
      // 429/5xx are usually the public API being busy, not a bad request.
      const bool worthRetrying = responseCode == 429 || responseCode >= 500;
      if (worthRetrying && attempt == 1) { delay(1200); continue; }
      return false;
    }

    // Read through PatientStream, never the raw client - see the note above.
    PatientStream body(*http.getStreamPtr(), 6000);
    const DeserializationError error = filter
      ? deserializeJson(document, body, DeserializationOption::Filter(*filter))
      : deserializeJson(document, body);

    if (error) {
      Serial.printf("JSON error: %s\n", error.c_str());
      Serial.printf("   content-type: %s\n", http.header("Content-Type").c_str());
      Serial.printf("   encoding    : %s\n", http.header("Content-Encoding").c_str());
      Serial.printf("   transfer    : %s\n", http.header("Transfer-Encoding").c_str());
      Serial.printf("   length      : %d\n", http.getSize());
      Serial.printf("   url         : %s\n", url.c_str());
    }
    http.end();
    Serial.printf("   fetch %s in %lu ms\n", error ? "FAILED" : "ok",
                  (unsigned long)(millis() - startedAt));
    if (!error) return true;
    if (attempt == 1) { delay(600); continue; }
    return false;
  }
  return false;
}

const char *firstText(JsonVariantConst preferred, JsonVariantConst fallback,
                      const char *defaultValue) {
  const char *value = preferred.as<const char *>();
  if (value && *value) return value;
  value = fallback.as<const char *>();
  return (value && *value) ? value : defaultValue;
}

// cityBusOnly keeps the Freising town network and drops the regional and
// express coaches. Transitous has no server-side filter for the product, but
// it does put it in the route id: a StadtBus route reads
//   de-DELFI_de:mvv-muenchen:19-640|640|StadtBus:1081_3
// against RegionalBus and ExpressBus for the others. Verified against the
// live feed for Freising Bahnhof. If that ever stops matching, the caller
// falls back to the unfiltered list rather than showing an empty board.
int fetchTransitousDepartures(const char *prefixedStopId, Departure *target,
                              uint8_t maximum, uint8_t requestN,
                              bool trainsOnly, bool cityBusOnly) {
  if (strncmp(prefixedStopId, "motis:", 6) != 0) return -1;
  const String stopId(prefixedStopId + 6);
  JsonDocument document;
  JsonDocument filter;
  filter["stopTimes"][0]["place"]["departure"] = true;
  filter["stopTimes"][0]["place"]["scheduledDeparture"] = true;
  filter["stopTimes"][0]["place"]["track"] = true;
  filter["stopTimes"][0]["place"]["scheduledTrack"] = true;
  filter["stopTimes"][0]["place"]["cancelled"] = true;
  filter["stopTimes"][0]["place"]["lat"] = true;
  filter["stopTimes"][0]["place"]["lon"] = true;
  filter["stopTimes"][0]["mode"] = true;
  filter["stopTimes"][0]["realTime"] = true;
  filter["stopTimes"][0]["headsign"] = true;
  filter["stopTimes"][0]["routeShortName"] = true;
  filter["stopTimes"][0]["displayName"] = true;
  filter["stopTimes"][0]["cancelled"] = true;
  filter["stopTimes"][0]["tripCancelled"] = true;
  // Needed only where regional buses have to be told apart from town ones.
  // It is a long string, so it is fetched for the one stop that needs it.
  if (cityBusOnly) filter["stopTimes"][0]["routeId"] = true;

  // Each board asks for a fixed, small number: what it shows plus headroom
  // for what gets filtered out. See Config::TRAIN_REQUEST_N and BUS_STOPS.
  const uint8_t requested = requestN;
  // Filter server-side as well. A rail station and its bus stops often share
  // one stop id, so without this the rail departures use up the whole page.
  const String modeParam = trainsOnly
    ? "&mode=HIGHSPEED_RAIL,LONG_DISTANCE,NIGHT_RAIL,REGIONAL_RAIL,SUBURBAN"
    : "&mode=BUS,COACH,TRAM,METRO";
  const String url = String(Config::TRANSITOUS_URL) + "/api/v5/stoptimes?stopId=" +
    urlEncode(stopId) + "&n=" + String(requested) + modeParam;
  if (!getJson(url, document, &filter)) return -1;
  JsonArray list = document["stopTimes"].as<JsonArray>();
  if (list.isNull()) return -1;

  Serial.printf("Transitous %s: %d stopTimes returned\n",
                trainsOnly ? "rail" : "bus", (int)list.size());

  uint8_t count = 0;
  uint8_t skipped = 0;
  String seenModes;
  for (JsonObject stopTime : list) {
    const String mode = stopTime["mode"] | "";
    if (seenModes.indexOf(mode) < 0) { seenModes += mode; seenModes += " "; }
    bool keep = trainsOnly ? isRailMode(mode) : isBusMode(mode);
    if (keep && cityBusOnly) {
      const char *route = stopTime["routeId"].as<const char *>();
      keep = route && strstr(route, "StadtBus") != nullptr;
    }
    if (!keep) { skipped++; continue; }
    if (count >= maximum) break;
    Departure &item = target[count++];
    item.line = firstText(stopTime["routeShortName"], stopTime["displayName"], "?");
    item.destination = stopTime["headsign"] | "Unknown";
    item.platform = cleanPlatform(firstText(stopTime["place"]["track"],
                              stopTime["place"]["scheduledTrack"], "-"));
    const char *actual = stopTime["place"]["departure"].as<const char *>();
    const char *planned = stopTime["place"]["scheduledDeparture"].as<const char *>();
    item.departureTime = hhmm((actual && *actual) ? actual : planned);
    // Both times are kept. The board shows the planned one struck through
    // beside the revised one when they differ, which is the only way to see
    // that a 10:30 departure was meant to be 10:27.
    item.scheduledTime = hhmm((planned && *planned) ? planned : actual);
    const time_t actualEpoch = isoEpoch(actual);
    const time_t plannedEpoch = isoEpoch(planned);
    item.epoch = actualEpoch ? actualEpoch : plannedEpoch;
    item.delayMinutes = (actualEpoch && plannedEpoch)
      ? static_cast<int16_t>((actualEpoch - plannedEpoch) / 60) : 0;
    item.cancelled = (stopTime["cancelled"] | false) ||
                     (stopTime["tripCancelled"] | false) ||
                     (stopTime["place"]["cancelled"] | false);
    item.realtime = stopTime["realTime"] | false;
    item.lat = stopTime["place"]["lat"] | 0.0f;
    item.lon = stopTime["place"]["lon"] | 0.0f;
  }

  sortDepartures(target, count);
  Serial.printf("   kept %d, skipped %d, modes seen: %s\n",
                (int)count, (int)skipped, seenModes.c_str());
  for (uint8_t d = 0; d < count && d < 4; d++) {
    Serial.printf("      %s  epoch=%ld\n",
                  target[d].departureTime.c_str(), (long)target[d].epoch);
  }
  return count;
}

// ---------------------------------------------------------------------
// Where the board is, for the weather: a town typed in setup, or else
// looked up once from the internet connection's public address and saved;
// looked up again only after the place or the Wi-Fi network is changed in
// setup. City-level accuracy, which is what a
// forecast needs. Two services, so one being down does not matter.
// ---------------------------------------------------------------------
bool detectLocation() {
  JsonDocument doc;
  String lat, lon, city;
  String query;
  lockData();
  query = settings.geoQuery;
  unlockData();
  if (query.length()) {
    // A town typed in setup. Open-Meteo's own geocoder, the same service
    // the forecast comes from; German names, first match.
    const String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
      urlEncode(query) + "&count=1&language=de&format=json";
    if (getJson(url, doc) && !doc["results"][0].isNull()) {
      JsonObject r = doc["results"][0];
      lat  = String(r["latitude"].as<float>(), 4);
      lon  = String(r["longitude"].as<float>(), 4);
      city = r["name"] | query.c_str();
    } else {
      Serial.printf("Town \"%s\" not found - using the built-in default\n",
                    query.c_str());
      return false;
    }
  } else if (getJson("http://ip-api.com/json/?fields=status,city,lat,lon", doc) &&
      doc["status"] == "success") {
    lat  = String(doc["lat"].as<float>(), 4);
    lon  = String(doc["lon"].as<float>(), 4);
    city = doc["city"] | "";
  } else {
    doc.clear();
    if (getJson("https://ipwho.is/?fields=success,city,latitude,longitude", doc) &&
        (doc["success"] | false)) {
      lat  = String(doc["latitude"].as<float>(), 4);
      lon  = String(doc["longitude"].as<float>(), 4);
      city = doc["city"] | "";
    }
  }
  if (lat.isEmpty() || lon.isEmpty() || (lat == "0.0000" && lon == "0.0000")) {
    Serial.println("Location lookup failed - using the built-in default");
    return false;
  }
  lockData();
  settings.geoLat = lat;
  settings.geoLon = lon;
  settings.geoCity = asciiGerman(city);
  unlockData();
  persistSettings();
  Serial.printf("Location detected: %s (%s, %s)\n",
                settings.geoCity.c_str(), lat.c_str(), lon.c_str());
  dataDirty |= DIRTY_WEATHER;        // the weather header shows the town
  tempHistoryAt = 0;                 // a new place needs its own history
  return true;
}

// Daily highs for the last 30 days and today. Asked for on its own so the
// main forecast call does not grow by 30 days of hourly data.
bool fetchTempHistory() {
  String lat, lon;
  lockData();
  lat = settings.geoLat.length() ? settings.geoLat : String(Config::WEATHER_LAT);
  lon = settings.geoLon.length() ? settings.geoLon : String(Config::WEATHER_LON);
  unlockData();
  JsonDocument doc;
  const String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
    lat + "&longitude=" + lon +
    "&daily=temperature_2m_max&past_days=30&forecast_days=1&timezone=auto";
  if (!getJson(url, doc)) return false;
  JsonArrayConst highs = doc["daily"]["temperature_2m_max"];
  if (highs.isNull() || highs.size() == 0) return false;
  float fresh[TEMP_DAYS];
  uint8_t n = 0;
  for (JsonVariantConst v : highs) {
    if (n >= TEMP_DAYS) break;
    fresh[n++] = v.isNull() ? NAN : v.as<float>();
  }
  lockData();
  memcpy(tempHigh, fresh, sizeof(float) * n);
  tempHighCount = n;
  tempHistoryAt = millis() | 1;
  unlockData();
  Serial.printf("Temperature history: %u days, today high %.1f\n",
                n, n ? (double)fresh[n - 1] : 0.0);
  return true;
}

// ---------------------------------------------------------------------
// The forecast. Everything comes from one Open-Meteo call, asked for as
// unix timestamps so no date string has to be parsed on the device and
// every local hour is simple arithmetic.
//
// Verified against the live API: hourly runs 00:00 of day 0 to 23:00 of
// day 3, daily carries sunrise and sunset for all four days, and
// "precipitation" is the millimetres in that hour - which is what gives the
// rain window a start and an end rather than just a percentage.
// ---------------------------------------------------------------------
bool fetchWeather(WeatherData &out) {
  String lat, lon;
  lockData();
  lat = settings.geoLat.length() ? settings.geoLat : String(Config::WEATHER_LAT);
  lon = settings.geoLon.length() ? settings.geoLon : String(Config::WEATHER_LON);
  unlockData();
  JsonDocument document;
  const String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
    lat + "&longitude=" + lon +
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
    "weather_code,wind_speed_10m,wind_direction_10m"
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
    "precipitation_sum,weather_code,sunrise,sunset,"
    // The second line of each outlook row. All four ride along on the call
    // that was already being made - no extra request, no extra handshake.
    "apparent_temperature_max,uv_index_max,wind_speed_10m_max,"
    "wind_direction_10m_dominant"
    "&hourly=temperature_2m,precipitation_probability,precipitation"
    "&forecast_days=4&timezone=auto&timeformat=unixtime";
  if (!getJson(url, document)) return false;

  const long utcOffset = document["utc_offset_seconds"] | 0L;

  out.temperature         = document["current"]["temperature_2m"] | NAN;
  out.apparentTemperature = document["current"]["apparent_temperature"] | NAN;
  out.humidity            = document["current"]["relative_humidity_2m"] | -1;
  out.weatherCode         = document["current"]["weather_code"] | -1;
  out.windSpeed           = document["current"]["wind_speed_10m"] | NAN;
  out.windDirection       = document["current"]["wind_direction_10m"] | -1;

  // ---- the four days ----------------------------------------------------
  JsonArrayConst dayTimes = document["daily"]["time"];
  JsonArrayConst sunrises = document["daily"]["sunrise"];
  JsonArrayConst sunsets  = document["daily"]["sunset"];
  int64_t day0Local = 0;
  out.dayCount = 0;
  for (uint8_t d = 0; d < WEATHER_DAYS && d < dayTimes.size(); d++) {
    const int64_t midnightUtc = dayTimes[d] | (int64_t)0;
    if (!midnightUtc) continue;
    const int64_t localMidnight = midnightUtc + utcOffset;
    if (d == 0) day0Local = localMidnight;

    WeatherDay &wd = out.day[d];
    wd = WeatherDay();
    wd.high       = document["daily"]["temperature_2m_max"][d] | NAN;
    wd.low        = document["daily"]["temperature_2m_min"][d] | NAN;
    wd.code       = document["daily"]["weather_code"][d] | -1;
    wd.rainChance = (int8_t)(document["daily"]["precipitation_probability_max"][d] | -1);
    wd.rainMm     = document["daily"]["precipitation_sum"][d] | 0.0f;
    wd.feels      = document["daily"]["apparent_temperature_max"][d] | NAN;
    wd.windKmh    = document["daily"]["wind_speed_10m_max"][d] | NAN;
    wd.windDir    = (int16_t)(document["daily"]["wind_direction_10m_dominant"][d] | -1);
    {
      const float uv = document["daily"]["uv_index_max"][d] | NAN;
      wd.uv = isnan(uv) ? -1 : (int8_t)(uv + 0.5f);
    }
    const int64_t riseUtc = sunrises[d] | (int64_t)0;
    const int64_t setUtc  = sunsets[d]  | (int64_t)0;
    wd.sunrise    = localHhmm(riseUtc, utcOffset);
    wd.sunset     = localHhmm(setUtc,  utcOffset);

    // Day length straight off the two timestamps that were already fetched,
    // and how it compares with the day before. In September in Freising that
    // delta is about four minutes a day, and watching it is half the point
    // of having the number at all.
    if (riseUtc && setUtc && setUtc > riseUtc) {
      wd.daylightMin = (int16_t)((setUtc - riseUtc) / 60);
      if (d > 0 && out.day[d - 1].daylightMin > 0) {
        wd.daylightDelta = (int16_t)(wd.daylightMin - out.day[d - 1].daylightMin);
      }
    }
    // Moon phase. Open-Meteo does not carry one, but it is pure arithmetic
    // from the date: the synodic month is 29.530588 days and 2000-01-06
    // 18:14 UTC was a new moon. 0 = new, 0.5 = full.
    {
      const double sinceNewMoon = (double)(localMidnight + 43200 - 947182440LL);
      double cycles = sinceNewMoon / 2551442.9;
      cycles -= floor(cycles);
      if (cycles < 0) cycles += 1.0;
      wd.moonPhase = (float)cycles;
    }

    // Day of the week and the date, straight off the local midnight.
    const int64_t daysSinceEpoch = localMidnight / 86400;
    wd.weekday = (int8_t)((daysSinceEpoch + 4) % 7);        // 1970-01-01 = Thu
    time_t noon = (time_t)(midnightUtc + 43200);
    struct tm asDay;
    gmtime_r(&noon, &asDay);
    char label[8];
    snprintf(label, sizeof(label), "%02d.%02d.", asDay.tm_mday, asDay.tm_mon + 1);
    wd.dayLabel = label;
    out.dayCount = d + 1;
  }
  out.sunrise = out.dayCount ? out.day[0].sunrise : String();
  out.sunset  = out.dayCount ? out.day[0].sunset  : String();

  // ---- the hourly series ------------------------------------------------
  // Two jobs at once: the twelve-hour strip on the today screen, and the
  // per-day rain window. Both are driven off the same walk.
  JsonArrayConst hourTimes = document["hourly"]["time"];
  JsonArrayConst hourTemps = document["hourly"]["temperature_2m"];
  JsonArrayConst hourProbs = document["hourly"]["precipitation_probability"];
  JsonArrayConst hourMm    = document["hourly"]["precipitation"];

  const time_t nowUtc = time(nullptr);
  out.hourCount = 0;
  for (size_t i = 0; i < hourTimes.size(); i++) {
    const int64_t stampUtc = hourTimes[i] | (int64_t)0;
    if (!stampUtc) continue;
    const int64_t local = stampUtc + utcOffset;
    const int8_t hourOfDay = (int8_t)((local % 86400) / 3600);
    const int dayIndex = (int)((local - day0Local) / 86400);

    const float mm   = hourMm[i] | 0.0f;
    const int   prob = hourProbs[i] | 0;

    // Rain window for the day this hour belongs to.
    if (dayIndex >= 0 && dayIndex < (int)out.dayCount) {
      const bool wet = mm >= 0.1f || prob >= 60;
      if (wet) {
        WeatherDay &wd = out.day[dayIndex];
        if (wd.rainFrom < 0) wd.rainFrom = hourOfDay;
        wd.rainTo = (int8_t)(hourOfDay + 1);
        // One bit per hour, so the bar on the outlook page can show two
        // separate showers as two rather than as one long wet afternoon.
        if (hourOfDay >= 0 && hourOfDay < 24) {
          wd.rainMask |= (uint32_t)1 << hourOfDay;
        }
      }
    }

    // The strip: twelve hours from the current one onwards.
    if (out.hourCount < WEATHER_HOURS &&
        (nowUtc < 100000 || stampUtc + 3600 > (int64_t)nowUtc)) {
      const float t = hourTemps[i] | NAN;
      if (!isnan(t)) {
        const uint8_t k = out.hourCount++;
        out.hourTemp[k]  = t;
        out.hourRain[k]  = (int8_t)prob;
        out.hourOfDay[k] = hourOfDay;
      }
    }
  }

  out.valid = !isnan(out.temperature) && out.dayCount > 0;
  if (out.valid) out.updatedAt = clockNow();
  Serial.printf("Weather: %.1fC, %u days, %u hours, today %.0f/%.0f %d%% rain\n",
                (double)out.temperature, out.dayCount, out.hourCount,
                (double)out.day[0].high, (double)out.day[0].low,
                (int)out.day[0].rainChance);
  for (uint8_t d = 0; d < out.dayCount; d++) {
    const WeatherDay &wd = out.day[d];
    Serial.printf("   day %u %s  %.0f/%.0f (feels %.0f)  rain %d%% %.1fmm  "
                  "window %d-%d mask %06lX  sun %s-%s  %dh%02dm %+dm  "
                  "UV %d  wind %.0fkm/h %d deg  moon %.2f\n",
                  d, wd.dayLabel.c_str(),
                  (double)wd.high, (double)wd.low, (double)wd.feels,
                  (int)wd.rainChance, (double)wd.rainMm,
                  (int)wd.rainFrom, (int)wd.rainTo, (unsigned long)wd.rainMask,
                  wd.sunrise.c_str(), wd.sunset.c_str(),
                  wd.daylightMin / 60, wd.daylightMin % 60,
                  (int)wd.daylightDelta, (int)wd.uv,
                  (double)wd.windKmh, (int)wd.windDir, (double)wd.moonPhase);
  }
  return out.valid;
}

// ---------------------------------------------------------------------
// The refresh cycle. This runs on the network task, never on the loop, so
// however long a feed takes to answer the screen keeps drawing and the
// button keeps working.
//
// Each source is fetched into netStage with the lock OPEN - that is the slow
// part - and only the handover is locked, which takes microseconds. Every
// source marks the screen dirty as it lands, so after boot the pages fill
// in one by one instead of waiting for the slowest feed.
//
// Four HTTPS calls per cycle, always: trains, the two bus stops, weather.
// Plus, rarely: the location lookup (once, or after a change in setup) and
// the 30-day temperature history (every 6 h). There is no name search and
// no fallback walk: every stop is a pinned id, and zero departures is an
// answer.
// ---------------------------------------------------------------------
void runRefreshCycle() {
  const uint32_t cycleStart = millis();
  uint8_t trainOk = 0, busOk = 0;
  bool weatherOk = false;

  // ---- trains -----------------------------------------------------------
  feedWatchdog();
  {
    const int n = fetchTransitousDepartures(Config::TRAIN_FIXED_ID, netStage,
                                            Config::MAX_TRAINS,
                                            Config::TRAIN_REQUEST_N, true, false);
    if (n >= 0) {
      lockData();
      const uint8_t kept = dropPastDepartures(netStage, (uint8_t)n);
      for (uint8_t i = 0; i < kept; i++) trains[i] = netStage[i];
      trainCount = kept;
      pushDelaySample(trains, kept);
      unlockData();
      trainOk = 1;
      dataDirty |= DIRTY_TRAINS;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(10));

  // ---- bus stops ------------------------------------------------------------
  for (uint8_t i = 0; i < Config::BUS_STOP_COUNT; i++) {
    if (netSuspend || setupOnlyMode) return;
    const Config::BusStopDef &def = Config::BUS_STOPS[i];
    feedWatchdog();
    // Zero is a real answer (late evening, Sundays) and is shown as such.
    // It used to set off a name search and up to eight more requests.
    const int n = fetchTransitousDepartures(def.fixedId, netStage,
                                            Config::BUS_ROWS_PER_PAGE,
                                            def.requestN, false,
                                            def.cityBusOnly);
    if (n >= 0) {
      lockData();
      const uint8_t kept = dropPastDepartures(netStage, (uint8_t)n);
      for (uint8_t r = 0; r < kept; r++) busRows[i][r] = netStage[r];
      busRowCount[i] = kept;
      unlockData();
      busOk++;
      dataDirty |= (uint8_t)(DIRTY_BUS0 << i);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // ---- weather ----------------------------------------------------------
  if (netSuspend || setupOnlyMode) return;
  feedWatchdog();
  // No saved location yet: find it first. Tried at most every 10 minutes, so
  // a lookup service being down never costs more than one call per cycle.
  {
    static uint32_t lastGeoTry = 0;
    if (settings.geoLat.isEmpty() &&
        (lastGeoTry == 0 || millis() - lastGeoTry > 600000UL)) {
      lastGeoTry = millis() | 1;
      detectLocation();
    }
  }
  {
    // Static: WeatherData is a few hundred bytes of floats and Strings, and
    // this task's stack is better spent on TLS.
    static WeatherData fresh;
    fresh = WeatherData();
    if (fetchWeather(fresh)) {
      lockData();
      weather = fresh;
      unlockData();
      weatherOk = true;
      dataDirty |= DIRTY_WEATHER;
    }
    // A failed fetch keeps the last good forecast on screen; the dot says
    // it is old.
  }

  // ---- 30-day temperature history (every 6 hours) -------------------------
  if (!(netSuspend || setupOnlyMode) &&
      (tempHistoryAt == 0 || millis() - tempHistoryAt >= TEMP_HISTORY_EVERY_MS)) {
    static uint32_t lastTry = 0;
    // A failure retries after 10 minutes, not every cycle.
    if (lastTry == 0 || millis() - lastTry >= 600000UL) {
      lastTry = millis() | 1;
      feedWatchdog();
      if (fetchTempHistory()) dataDirty |= DIRTY_WEATHER;
    }
  }

  // ---- settle the status line ------------------------------------------
  const uint8_t successful = trainOk + (busOk > 0 ? 1 : 0) + (weatherOk ? 1 : 0);
  lockData();
  if (successful == 3)      statusText = "Live";
  else if (successful > 0)  statusText = "Partly offline";
  else                      statusText = "Data source offline";
  unlockData();
  if (successful > 0) lastGoodDataAt = millis();

  Serial.printf("Refresh: %s in %lu ms | trains %s (%u), buses %u/%u "
                "(%u, %u), weather %s | heap=%u\n",
                statusText.c_str(), (unsigned long)(millis() - cycleStart),
                trainOk ? "ok" : "FAILED", trainCount,
                busOk, (unsigned)Config::BUS_STOP_COUNT,
                busRowCount[0], busRowCount[1],
                weatherOk ? "ok" : "FAILED", (unsigned)ESP.getFreeHeap());
}

// The network task itself. It owns Wi-Fi recovery too, because reconnecting
// blocks and blocking is exactly what must stay off the loop.
void netTask(void *) {
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();

    if (netSuspend || setupOnlyMode) {
      netIdle = true;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      netIdle = true;
      // A long outage gets a re-join attempt every 30 s from here; the loop
      // decides when to give up and restart.
      if (wifiLostAt != 0 && millis() - lastWifiAttemptAt >= Config::WIFI_RETRY_MS) {
        lastWifiAttemptAt = millis();
        netIdle = false;
        WiFi.reconnect();
        netIdle = true;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    // Nothing has come back for half an hour. That is usually the APIs being
    // down rather than this board being stuck, so first re-join Wi-Fi once,
    // and restart only if that changed nothing. This lives here, not in the
    // loop, because re-joining blocks - and the loop must never block.
    if (lastGoodDataAt != 0 &&
        millis() - lastGoodDataAt >= Config::DATA_STALE_REBOOT_MS) {
      if (staleRecoveryAt == 0 || staleRecoveryAt < lastGoodDataAt) {
        Serial.printf("No data for %lu s - re-joining Wi-Fi (heap=%u)\n",
                      (unsigned long)((millis() - lastGoodDataAt) / 1000),
                      (unsigned)ESP.getFreeHeap());
        staleRecoveryAt = millis();
        netIdle = false;
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
        netIdle = true;
        lastRefreshAt = 0;
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }
      if (millis() - staleRecoveryAt >= Config::DATA_STALE_REBOOT_MS) {
        restartFor("no data for an hour");
      }
    }

    if (lastRefreshAt != 0 &&
        millis() - lastRefreshAt < Config::DATA_REFRESH_MS) {
      netIdle = true;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // One more look after the idle window: the radio may have been taken
    // away while this task was sleeping.
    if (netSuspend) { netIdle = true; continue; }

    netIdle = false;
    netBusy = true;
    runRefreshCycle();
    netBusy = false;
    netIdle = true;
    // Stamped non-zero even if millis() happens to be 0, so "never" and
    // "just now" cannot be confused.
    lastRefreshAt = millis() | 1;
    cycleCount++;
    // After the first cycle every page swaps "Loading..." for its real
    // content or "No departures", whichever feed it shows.
    if (cycleCount == 1) dataDirty = DIRTY_ALL;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void startNetTask() {
  if (netTaskHandle) return;
  // TLS wants a generous stack. 16 kB (was 12): running out of stack here
  // is a crash-and-reboot, and 4 kB of heap is cheap insurance.
  // Priority 1 - BELOW the loop, which setup() raises to 2 - so drawing and
  // the button always win the CPU and a TLS handshake can only ever use the
  // time the loop leaves idle.
  xTaskCreateUniversal(netTask, "net", 16384, nullptr, 1, &netTaskHandle,
                       ARDUINO_RUNNING_CORE);
}

// Bring the network task to a complete stop before anything reconfigures
// Wi-Fi. Tearing the radio down underneath a TLS handshake is a crash.
void pauseNetTask(uint32_t waitMs) {
  netSuspend = true;
  // Rollover-safe: comparing elapsed against the limit works across the
  // millis() wrap, where comparing against a precomputed deadline does not.
  const uint32_t started = millis();
  while (!netIdle && (millis() - started) < waitMs) {
    feedWatchdog();
    delay(20);
  }
}
