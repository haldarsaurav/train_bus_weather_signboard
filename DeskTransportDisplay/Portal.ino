#include "DeskDisplay.h"

// =====================================================================
//  Train Bus Weather Signboard  -  firmware v1.0.0
//  Copyright (c) 2026 Sam (haldarsaurav). All rights reserved.
//  No use, copying, modification or redistribution - and no reproduction
//  with AI tools - without written permission. See LICENSE.
//
//  Stored settings, the setup access point, and the setup page.
//
//  The portal exists only in setup mode (MODE held 3 s, first boot, or
//  Wi-Fi not reachable). In normal running there is no web server at all:
//  serving pages from the drawing loop is what used to freeze the screen
//  and the button for seconds at a time.
//
//  What can be set: the Wi-Fi network and password, the weather place
//  (detect automatically or a typed town), auto-advance, seconds per page,
//  and scrolling long names. Stops and station are fixed in DeskDisplay.h.
//
//  Everything is stored in NVS (Preferences namespace "desk-display"):
//    ssid, pass              Wi-Fi
//    geo-q                   typed town ("" = detect automatically)
//    geo-lat/geo-lon/geo-city the place in use, cached after lookup
//    rotate, pagesec         auto-advance and its dwell (key name is legacy)
//    marquee                 scroll long destination names
//    swwhy, badwhy, badcount restart bookkeeping (DeskTransportDisplay.ino)
//    cfgver                  settings layout version
// =====================================================================

void loadSettings() {
  prefs.begin("desk-display", false);
  settings.ssid = prefs.getString("ssid", "");
  settings.password = prefs.getString("pass", "");
  settings.geoLat  = prefs.getString("geo-lat", "");
  settings.geoLon  = prefs.getString("geo-lon", "");
  settings.geoCity = prefs.getString("geo-city", "");
  settings.geoQuery = prefs.getString("geo-q", "");
  settings.autoAdvance = prefs.getBool("rotate", false); // legacy key name
  settings.marqueeEnabled = prefs.getBool("marquee", false);
  settings.pageSeconds = prefs.getUChar("pagesec", 10);
  if (settings.pageSeconds < 3)  settings.pageSeconds = 3;
  if (settings.pageSeconds > 60) settings.pageSeconds = 60;

  // Clear the keys older builds stored for configurable stops and places.
  // Nothing reads them any more; this just stops them sitting in flash.
  if (prefs.getUChar("cfgver", 0) < 6) {
    static const char *oldKeys[] = { "train", "train-id", "weather", "lat", "lon",
                                     "bstop0", "bstop1", "bstop2", "bstop3" };
    for (const char *k : oldKeys) {
      if (prefs.isKey(k)) prefs.remove(k);
    }
    prefs.putUChar("cfgver", 6);
    Serial.println("Settings: removed unused keys from older firmware");
  }
  prefs.end();
}

void persistSettings() {
  prefs.begin("desk-display", false);
  prefs.putString("ssid", settings.ssid);
  prefs.putString("pass", settings.password);
  prefs.putString("geo-lat", settings.geoLat);
  prefs.putString("geo-lon", settings.geoLon);
  prefs.putString("geo-city", settings.geoCity);
  prefs.putString("geo-q", settings.geoQuery);
  prefs.putBool("rotate", settings.autoAdvance);
  prefs.putBool("marquee", settings.marqueeEnabled);
  prefs.putUChar("pagesec", settings.pageSeconds);
  prefs.end();
}

// ---------------------------------------------------------------------
// Wi-Fi scan, asynchronously, so the page can pick a network instead of
// typing it. One wrong character in an SSID is otherwise a blank board.
// ---------------------------------------------------------------------
void handleScan() {
  const int16_t found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json", "{\"state\":\"scanning\"}");
    return;
  }
  if (found == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true, false);       // async
    server.send(200, "application/json", "{\"state\":\"scanning\"}");
    return;
  }
  String json;
  json.reserve(1500);
  json = "{\"state\":\"done\",\"nets\":[";
  uint8_t written = 0;
  for (int16_t i = 0; i < found && written < 20; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    if (written) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    written++;
  }
  json += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

// Save always restarts: setup mode ends here and the board comes back up
// on the (new) network with the new display settings.
void handleSave() {
  String newSsid = server.arg("ssid");
  const String newPass = server.arg("pass");
  newSsid.trim();
  if (newSsid.isEmpty() || newSsid.length() > 32 || newPass.length() > 64) {
    server.send(400, "application/json",
                "{\"ok\":false,\"msg\":\"Enter a network name (max 32 characters).\"}");
    return;
  }
  // An empty password means "keep it" - but only on the same network.
  if (!newPass.isEmpty() || newSsid != settings.ssid) settings.password = newPass;
  // Weather place: automatic, or a town. Any change - or, when automatic,
  // a different network (usually a different place) - drops the saved
  // coordinates so the board finds them again after the restart.
  String newQuery = server.arg("wmode") == "town" ? server.arg("wtown") : String();
  newQuery.trim();
  if (newQuery.length() > 60) newQuery = newQuery.substring(0, 60);
  const bool placeChanged = newQuery != settings.geoQuery ||
                            (newQuery.isEmpty() && newSsid != settings.ssid);
  settings.geoQuery = newQuery;
  if (placeChanged) {
    settings.geoLat = "";
    settings.geoLon = "";
    settings.geoCity = "";
  }
  settings.ssid = newSsid;
  settings.autoAdvance = server.arg("rotate") == "1";
  settings.marqueeEnabled = server.arg("marquee") == "1";
  int secs = server.arg("pagesec").toInt();
  if (secs < 3) secs = 3;
  if (secs > 60) secs = 60;
  settings.pageSeconds = (uint8_t)secs;
  persistSettings();
  Serial.printf("Saved: \"%s\", auto-advance %s (%u s), long names %s - restarting\n",
                settings.ssid.c_str(), settings.autoAdvance ? "ON" : "OFF",
                settings.pageSeconds, settings.marqueeEnabled ? "move" : "still");
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"Saved - restarting. Reconnect your phone to your normal Wi-Fi.\"}");
  delay(600);
  restartFor("saved settings");
}

String pageSecondsOptions() {
  static const uint8_t presets[] = { 5, 10, 15, 20, 30, 45, 60 };
  String out;
  bool matched = false;
  for (uint8_t p : presets) if (p == settings.pageSeconds) matched = true;
  if (!matched) {
    const String v = String(settings.pageSeconds);
    out += "<option value='" + v + "' selected>" + v + " seconds</option>";
  }
  for (uint8_t p : presets) {
    const String v = String(p);
    out += "<option value='" + v + "'";
    if (p == settings.pageSeconds) out += " selected";
    out += ">" + v + " seconds</option>";
  }
  return out;
}

// The weather card: detect automatically, or a town typed here. Built as one
// String so no empty chunk can ever be sent from it.
String weatherCard() {
  const bool town = settings.geoQuery.length() > 0;
  String inUse = settings.geoCity.length() ? settings.geoCity
                                           : String(Config::WEATHER_LABEL) + " (default)";
  String h;
  h.reserve(900);
  h = "<section class='card'><h2>Weather place</h2>"
      "<p class='hint'>In use now: <b>" + htmlEscape(inUse) + "</b></p>"
      "<label class='radio'><input type='radio' name='wmode' value='auto'";
  if (!town) h += " checked";
  h += ">Detect automatically</label>"
       "<p class='hint' style='margin:2px 0 0 30px'>from your internet connection</p>"
       "<label class='radio'><input type='radio' name='wmode' value='town' id='wmTown'";
  if (town) h += " checked";
  h += ">Use this town</label>"
       "<input type='text' name='wtown' id='wtown' maxlength='60' placeholder='e.g. Freising' value=\"" +
       htmlEscape(settings.geoQuery) + "\">"
       "<p class='hint'>Looked up after Save. The weather page shows the town it found.</p></section>";
  return h;
}

void sendPortalPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", "");
  server.sendContent(F(
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Desk Display Setup</title><style>"
    ":root{color-scheme:dark;--bg:#061126;--card:#0d2147;--line:#294573;--ink:#f4f7ff;"
    "--dim:#9fb0cf;--acc:#ffd84a;--ok:#5ee39a}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);"
    "font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "main{max-width:560px;margin:0 auto;padding:20px 16px 40px}"
    "h1{font-size:26px;margin:0 0 4px}.sub{color:var(--dim);margin:0 0 16px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin:12px 0}"
    "h2{font-size:17px;margin:0 0 8px}label{display:block;margin-top:12px;font-weight:650}"
    "input[type=text],input[type=password],select{width:100%;padding:11px;margin-top:6px;"
    "border:1px solid #49669a;border-radius:10px;background:#071934;color:var(--ink);font-size:16px}"
    ".switch{display:flex;align-items:center;gap:10px;cursor:pointer}"
    ".switch input{width:20px;height:20px;accent-color:var(--acc)}"
    ".switch small{display:block;color:var(--dim);font-weight:400}"
    ".hint{color:var(--dim);font-size:13px;margin:6px 0 0}"
    "button{font:inherit;font-weight:750;border:0;border-radius:10px;padding:11px 14px;cursor:pointer}"
    "button:disabled{opacity:.55}.ghost{background:#183765;color:var(--ink);border:1px solid #36578a;margin-top:10px}"
    ".primary{background:var(--acc);color:#07152e;width:100%;margin-top:14px}"
    ".nets{list-style:none;margin:10px 0 0;padding:0;border:1px solid var(--line);border-radius:10px;"
    "max-height:220px;overflow:auto;display:none}"
    ".nets li{display:flex;gap:10px;padding:9px 11px;border-bottom:1px solid #213c6a;cursor:pointer}"
    ".nets li:hover{background:#173867}.nets li span:last-child{margin-left:auto;color:var(--dim);font-size:12px}"
    "#msg{margin-top:10px;color:var(--dim);min-height:1.5em}"
    ".warn{background:#3a1720;border:1px solid #7b3041;color:#ffc1c6;border-radius:10px;padding:9px 12px}"
    ".radio{display:flex;align-items:center;gap:10px;margin-top:10px;font-weight:650;cursor:pointer}"
    ".radio input{width:20px;height:20px;accent-color:var(--acc)}"
    "</style></head><body><main><h1>Desk Display</h1>"
    "<p class='sub'>Setup mode. Save restarts the board.</p>"));
  if (lastRestartWhy.length()) {
    String note = "<p class='warn'>Last unexpected restart: " + htmlEscape(lastRestartWhy);
    if (lastRestartCount > 1) note += " (" + String(lastRestartCount) + "x since power-on)";
    note += "</p>";
    server.sendContent(note);
  }
  server.sendContent(F("<form id='f'>"
    "<section class='card'><h2>Wi-Fi</h2><p class='hint'>2.4 GHz networks only - the C3 has no 5 GHz radio.</p>"
    "<label>Network<input type='text' name='ssid' id='ssid' required maxlength='32' value=\""));
  if (settings.ssid.length()) server.sendContent(htmlEscape(settings.ssid));
  server.sendContent(F("\"></label><button type='button' class='ghost' id='scanBtn'>Find networks</button>"
    "<ul class='nets' id='nets'></ul>"
    "<label>Password<input type='password' name='pass' maxlength='64' autocomplete='new-password'></label>"
    "<p class='hint'>Leave empty to keep the saved password for the same network.</p></section>"));
  server.sendContent(weatherCard());
  server.sendContent(F("<section class='card'><h2>Pages</h2>"
    "<label class='switch'><input type='checkbox' name='rotate' value='1'"));
  // Never sendContent(""): in a chunked reply an empty chunk means "end of
  // page", which cut the page off here - no Save button, no script.
  server.sendContent(settings.autoAdvance ? " checked>" : ">");
  server.sendContent(F("<span>Change pages automatically<small>Off: only a MODE tap changes the page.</small>"
    "</span></label><label>Time on each page<select name='pagesec'>"));
  server.sendContent(pageSecondsOptions());
  server.sendContent(F("</select></label><label class='switch'><input type='checkbox' name='marquee' value='1'"));
  server.sendContent(settings.marqueeEnabled ? " checked>" : ">");
  server.sendContent(F("<span>Scroll long destination names<small>Off: long names are shortened.</small>"
    "</span></label><p class='hint'>MODE tap: next page &middot; hold 3 s: setup &middot; RESET: restart</p>"
    "</section></form><button type='button' class='primary' id='saveBtn'>Save and restart</button>"
    "<div id='msg'></div></main><script>"
    "const $=i=>document.getElementById(i),msg=t=>$('msg').textContent=t;"
    "async function scan(){const b=$('scanBtn');b.disabled=true;b.textContent='Scanning...';"
    "try{for(let n=0;n<25;n++){const r=await(await fetch('/scan',{method:'POST'})).json();"
    "if(r.state==='scanning'){await new Promise(ok=>setTimeout(ok,1200));continue}"
    "const u=$('nets');u.replaceChildren();u.style.display='block';r.nets.sort((a,b)=>b.rssi-a.rssi);"
    "r.nets.forEach(x=>{const li=document.createElement('li'),a=document.createElement('span'),"
    "c=document.createElement('span');a.textContent=x.ssid;c.textContent=x.rssi+' dBm';li.append(a,c);"
    "li.onclick=()=>{$('ssid').value=x.ssid;u.style.display='none'};u.append(li)});"
    "if(!r.nets.length)msg('Nothing found - try again');break}}catch(e){msg('Scan failed - try again')}"
    "finally{b.disabled=false;b.textContent='Scan again'}}$('scanBtn').onclick=scan;"
    "$('wtown').onfocus=()=>{$('wmTown').checked=true};"
    "$('saveBtn').onclick=async()=>{const p=new URLSearchParams(new FormData($('f')));"
    "$('saveBtn').disabled=true;msg('Saving...');try{const r=await(await fetch('/save',{method:'POST',body:p})).json();"
    "msg(r.msg)}catch(e){msg('Saved - the board is restarting.')}};"
    "</script></body></html>"));
  server.sendContent("");
}

void startWebServer() {
  if (webServerStarted) return;
  server.on("/",     HTTP_GET,  sendPortalPage);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/scan", HTTP_POST, handleScan);
  server.onNotFound([] {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  webServerStarted = true;
}

// The setup screen: a centred QR code with the network name and the address
// under it, and nothing else. The password is not shown - it is in the QR,
// so scanning joins without typing it.
//
// Two-stage QR. Before a phone is attached it carries the Wi-Fi credentials;
// once one is attached it becomes the settings URL, so the same scan takes
// you straight to the page.
void drawSetupScreen(const String &reason) {
  const int W = lcd.width(), H = lcd.height();
  lcd.startWrite();
  lcd.fillScreen(Config::DB_BLUE);
  lcd.fillRect(0, 0, W, 24, Config::DB_HEADER);
  lcd.fillRect(0, 0, W, 2, Config::DB_HEADER_HI);
  txt("Setup", W / 2, 12, 22, D_MC, Config::BLACK, Config::DB_HEADER);

  const int qr = 132, qx = (W - qr) / 2, qy = 32;
  lcd.fillRect(qx - 5, qy - 5, qr + 10, qr + 10, Config::WHITE);
  if (apClients > 0) {
    lcd.qrcode("http://192.168.4.1/", qx, qy, qr, 3);
  } else {
    lcd.qrcode("WIFI:S:Desk-Transport-Display;T:WPA;P:" AP_PASSWORD ";;",
               qx, qy, qr, 4);
  }

  txt(Config::AP_NAME, W / 2, qy + qr + 20, 22, D_MC,
      Config::DB_YELLOW, Config::DB_BLUE);
  txt("192.168.4.1", W / 2, qy + qr + 42, 22, D_MC,
      Config::WHITE, Config::DB_BLUE);
  // Only when joining actually failed: that is worth knowing.
  if (wifiFailReason.length()) {
    txt(clipped(wifiFailReason, W - 12, 21), W / 2, H - 9, 21,
        D_MC, Config::CANCEL_RED, Config::DB_BLUE);
  }
  lcd.endWrite();
  (void)reason;       // logged on serial, no longer printed on the panel
}

void startSetupPortal(const String &reason) {
  setupOnlyMode = true;
  // Forget everything the last page registered for live redraw.
  marqueeCount = 0;
  clockX = -1;
  healthX = -1;
  {
    const int W = lcd.width(), H = lcd.height();
    lcd.fillRect(0, H - 22, W, 22, Config::BLACK);
    txt("opening setup...", W / 2, H - 11, 22, D_MC, Config::DB_YELLOW, Config::BLACK);
  }
  // Tearing the radio down underneath a live TLS handshake is a crash, so
  // the network task is brought to a stop first.
  pauseNetTask(30000);
  portalReason = reason;

  WiFi.onEvent(onWiFiEvent);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);
  // AP plus an idle station interface: the station side is what lets the
  // page scan for networks while the access point stays up.
  WiFi.mode(WIFI_AP_STA);
  delay(300);

  // Lower transmit power. At full power the association burst can brown out
  // an ESP32-C3 Super Mini that is also driving a TFT backlight, which drops
  // the client mid-handshake and looks exactly like "unable to join".
  WiFi.setTxPower(WIFI_POWER_11dBm);

  // Configure the AP subnet and DHCP pool explicitly. Relying on the
  // defaults is the usual reason a client associates and then never gets an
  // address - iOS reports that as "unable to join".
  const IPAddress apIp(192, 168, 4, 1);
  const IPAddress apMask(255, 255, 255, 0);
  const bool cfgOk = WiFi.softAPConfig(apIp, apIp, apMask);

  // WPA2 rather than open.
  bool ok = WiFi.softAP(Config::AP_NAME, AP_PASSWORD, 1, 0, 4);
  if (!ok) {
    delay(500);
    ok = WiFi.softAP(Config::AP_NAME, AP_PASSWORD);
  }
  delay(500);

  const IPAddress ip = WiFi.softAPIP();

  Serial.println();
  Serial.printf("startSetupPortal: %s\n", reason.c_str());
  Serial.printf("  softAPConfig     -> %s\n", cfgOk ? "OK" : "FAILED");
  Serial.printf("  softAP(\"%s\") -> %s\n", Config::AP_NAME, ok ? "OK" : "FAILED");
  Serial.printf("  password         : %s\n", AP_PASSWORD);
  Serial.printf("  channel          : 1\n");
  Serial.printf("  AP IP            : %s\n", ip.toString().c_str());
  Serial.printf("  tx power         : %d\n", (int)WiFi.getTxPower());
  Serial.printf("  free heap        : %u\n", (unsigned)ESP.getFreeHeap());
  Serial.println("  waiting for a client...");
  Serial.flush();

  startWebServer();
  drawSetupScreen(reason);
}
