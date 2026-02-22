#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#include "secrets.h"

TFT_eSPI tft = TFT_eSPI();
WiFiMulti wifiMulti;

enum Location { LOC_HOME, LOC_RC };
static Location currentLocation = LOC_HOME;

/*static void drawUmbrellaIcon(int x, int y, uint16_t color) {
  // x,y = top-left of a ~14x14 icon
  // canopy
  tft.drawLine(x+1, y+6,  x+12, y+6,  color);
  tft.drawLine(x+2, y+5,  x+11, y+5,  color);
  tft.drawLine(x+3, y+4,  x+10, y+4,  color);
  tft.drawLine(x+4, y+3,  x+9,  y+3,  color);
  tft.drawLine(x+5, y+2,  x+8,  y+2,  color);

  // little scallops (3 bumps)
  tft.drawPixel(x+3,  y+7, color);
  tft.drawPixel(x+6,  y+7, color);
  tft.drawPixel(x+9,  y+7, color);

  // handle
  tft.drawLine(x+7, y+6,  x+7, y+12, color);
  tft.drawLine(x+7, y+12, x+5, y+12, color);
  tft.drawPixel(x+5, y+11, color);
}*/

static void drawUmbrellaIcon(int x, int y, int s, uint16_t color) {
  // x,y = top-left, s = scale (1,2,3...)
  auto px = [&](int dx, int dy) {
    if (s <= 1) {
      tft.drawPixel(x + dx, y + dy, color);
    } else {
      tft.fillRect(x + dx*s, y + dy*s, s, s, color);
    }
  };

  auto hline = [&](int dx0, int dx1, int dy) {
    if (dx1 < dx0) { int t = dx0; dx0 = dx1; dx1 = t; }
    if (s <= 1) {
      tft.drawLine(x + dx0, y + dy, x + dx1, y + dy, color);
    } else {
      tft.fillRect(x + dx0*s, y + dy*s, (dx1 - dx0 + 1)*s, s, color);
    }
  };

  auto vline = [&](int dx, int dy0, int dy1) {
    if (dy1 < dy0) { int t = dy0; dy0 = dy1; dy1 = t; }
    if (s <= 1) {
      tft.drawLine(x + dx, y + dy0, x + dx, y + dy1, color);
    } else {
      tft.fillRect(x + dx*s, y + dy0*s, s, (dy1 - dy0 + 1)*s, color);
    }
  };

  // canopy (a little triangle-ish dome)
  hline(2, 13, 7);
  hline(3, 12, 6);
  hline(4, 11, 5);
  hline(5, 10, 4);
  hline(6, 9,  3);

  // scallops
  px(4, 8);
  px(7, 8);
  px(10,8);

  // handle
  vline(8, 7, 14);
  hline(8, 6, 14);
  px(6, 13);
}

static void drawSunIcon(int x, int y, int s, uint16_t color) {
  int cx = x + 7*s, cy = y + 7*s;
  // center circle
  tft.drawCircle(cx, cy, 3*s, color);
  // cardinal rays
  tft.drawLine(cx,        y + 1*s, cx,        y + 3*s, color);  // top
  tft.drawLine(cx,        y + 11*s, cx,       y + 13*s, color); // bottom
  tft.drawLine(x + 1*s,  cy,       x + 3*s,  cy,       color);  // left
  tft.drawLine(x + 11*s, cy,       x + 13*s, cy,       color);  // right
  // diagonal rays
  tft.drawLine(x + 3*s,  y + 3*s,  x + 4*s,  y + 4*s,  color);
  tft.drawLine(x + 10*s, y + 10*s, x + 11*s, y + 11*s, color);
  tft.drawLine(x + 10*s, y + 4*s,  x + 11*s, y + 3*s,  color);
  tft.drawLine(x + 3*s,  y + 11*s, x + 4*s,  y + 10*s, color);
}

// Poll the Worker (safe to do faster than 511 because Worker caches upstream)
static const uint32_t POLL_MS        = 30 * 1000;
static const uint32_t TOUCH_WAKE_MS  = 10 * 1000;
#define TOUCH_IRQ 36

static uint32_t manualWakeUntil = 0;

static bool isInDisplayWindow() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int minuteOfDay = lt.tm_hour * 60 + lt.tm_min;
  return minuteOfDay >= (8 * 60) && minuteOfDay < (9 * 60);
}

struct Departure {
  int mins;
  time_t depEpoch;
  String dest;   // "LCL" / "EXP" / etc
};

/*struct Weather {
  int tempF;        // -999 if missing
  int precipProb;   // -1 if missing
};*/

struct Weather {
  int tempF;        // -999 if missing
  int precipProb;   // -1 if missing (still useful for logs/debug)
  String precipIcon; // "sun" or "umb"
  String precipText; // "3h+" / "now" / "60m" / "2h0.08\"" etc
};

static String formatTimeLocal(time_t epochUtc) {
  if (epochUtc <= 0) return "--:--";

  struct tm lt;
  localtime_r(&epochUtc, &lt);

  int h = lt.tm_hour % 12;
  if (h == 0) h = 12;

  char buf[8];
  snprintf(buf, sizeof(buf), "%d:%02d%c", h, lt.tm_min, (lt.tm_hour < 12) ? 'a' : 'p');
  return String(buf);
}

static void drawDepartures(const Departure& a, const Departure& b, const Weather& w) {
  const int W = tft.width();
  const int H = tft.height();

  const int PAD = 10;
  const int RESERVED_BOTTOM = 70;      // keep this space free for weather band
  const int META_FONT = 4;
  const int MIN_FONT  = 7;

  // Header geometry
  const int headerY = 10;
  const int headerH = 34;              // approx height for font 4 header
  const int topLineY = headerY + headerH;

  // Content area (everything above reserved bottom)
  const int contentTop    = topLineY + 10;
  const int contentBottom = H - RESERVED_BOTTOM - 10;
  const int contentH      = contentBottom - contentTop;

  // Two fixed rows
  const int rowH  = contentH / 2;
  const int ROW_PAD_Y = 4;

  const int row1Y = contentTop + ROW_PAD_Y;
  const int row2Y = contentTop + rowH + ROW_PAD_Y;

  // Divider exactly between rows (consistent every time)
  const int midDividerY = contentTop + rowH;

  // Minutes column width
  const int MIN_COL_W = 110;
  const int metaX = PAD + MIN_COL_W + 12;

  tft.fillScreen(TFT_BLACK);

  // Header
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  const char* header = (currentLocation == LOC_RC) ? "Caltrain  Redwood City" : "Caltrain  4th/King";
  tft.drawString(header, PAD, headerY, META_FONT);

  // Dividers
  tft.drawFastHLine(PAD, topLineY, W - 2 * PAD, TFT_DARKGREY);
  tft.drawFastHLine(PAD, midDividerY, W - 2 * PAD, TFT_DARKGREY);
  tft.drawFastHLine(PAD, H - RESERVED_BOTTOM, W - 2 * PAD, TFT_DARKGREY);

  auto drawRow = [&](int y, const Departure& d) {
    // Big minutes
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(d.mins) + "m", PAD, y + 2, MIN_FONT);

    // Service label (LCL/LTD/EXP/etc)
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(d.dest, metaX, y + 10, META_FONT);

    // Departure time (right aligned)
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawRightString(formatTimeLocal(d.depEpoch), W - PAD, y + 10, META_FONT);
  };

  drawRow(row1Y, a);
  drawRow(row2Y, b);
  
  // --- Weather in reserved bottom band ---
  const int wxTop = H - RESERVED_BOTTOM;     // 240-70 = 170
  const int wxBottom = H;
  const int wxH = wxBottom - wxTop;

  tft.setTextFont(2);       // has letters
  tft.setTextSize(3);       // big, but still legible
  tft.setTextDatum(TL_DATUM);

  // Vertically center the text within the band
  // Font 2 @ size 3 is ~24px tall (varies a bit); center safely.
  const int textH = 24;
  const int wxY = wxTop + (wxH - textH) / 2;   // centered, should be ~193

  // LEFT: "55°F"
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  String tempNum = (w.tempF <= -900) ? String("--") : String(w.tempF);
  int x = PAD;

  tft.drawString(tempNum, x, wxY);

  int numW = tft.textWidth(tempNum);
  int degCx = x + numW + 6;         // degree circle center x
  int degCy = wxY + 7;              // degree circle center y for size 3
  tft.drawCircle(degCx, degCy, 3, TFT_WHITE);
  tft.drawString("F", degCx + 7, wxY);

  // RIGHT: "0% Rain"
  /*tft.setTextColor(TFT_CYAN, TFT_BLACK);
  String rain = (w.precipProb >= 0) ? (String(w.precipProb) + "% Rain") : String("--% Rain");
  int rainW = tft.textWidth(rain);
  tft.drawString(rain, W - PAD - rainW, wxY);*/

    // RIGHT: icon + short precip summary (e.g. "U now", "S 3h+")
  /*tft.setTextColor(TFT_CYAN, TFT_BLACK);

  char iconChar = ' ';
  if (w.precipIcon == "umb") iconChar = 'U';   // umbrella
  else if (w.precipIcon == "sun") iconChar = 'S'; // sun

  String ptxt = (w.precipText.length() ? w.precipText : String("--"));
  String right = String(iconChar) + " " + ptxt;   // uses your “one extra char” efficiently

  int rightW = tft.textWidth(right);
  tft.drawString(right, W - PAD - rightW, wxY);*/

    // RIGHT: tiny drawn icon + precip text (e.g. [☂] now, [☀] 3h+)
  tft.setTextColor(TFT_CYAN, TFT_BLACK);

  String ptxt = (w.precipText.length() ? w.precipText : String("--"));

  //const int ICON_W = 16;     // reserved horizontal space for icon + a little gap
  //const int ICON_H = 14;
  const int ICON_S = 2;                  // 1=old tiny, 2=nice, 3=chunky
  const int ICON_W = 16 * ICON_S;        // reserve space scaled

  int textW = tft.textWidth(ptxt);

  // Right-align the whole "icon + text" group
  int groupW = ICON_W + textW;
  int groupX = W - PAD - groupW;

  int iconX = groupX;
  int iconY = wxY + 2;       // tweak if you want it slightly higher/lower

  //if (w.precipIcon == "umb")      drawUmbrellaIcon(iconX, iconY, TFT_CYAN);
  //else if (w.precipIcon == "sun") drawSunIcon(iconX, iconY, TFT_CYAN);

  if (w.precipIcon == "umb") {
    drawUmbrellaIcon(iconX, iconY, ICON_S, TFT_CYAN);
  } else if (w.precipIcon == "sun") {
    drawSunIcon(iconX, iconY, ICON_S, TFT_CYAN);
  }

  // text sits to the right of the icon
  tft.drawString(ptxt, iconX + ICON_W, wxY);

  // Restore for the rest of the screen if needed
  tft.setTextSize(1);

}

static bool fetch_next2(Departure &d1, Departure &d2, Weather &w) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const char* url =
    "https://caltrain-board.murphy-ciaran-n.workers.dev/?agency=CT&stopCode=70012";

  if (!http.begin(client, url)) {
    Serial.println("http.begin failed");
    return false;
  }

  int code = http.GET();
  Serial.printf("Worker HTTP %d, bytes=%d\n", code, http.getSize());

  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  const char* trainsKey  = (currentLocation == LOC_RC) ? "trainsRC"  : "trains";
  const char* weatherKey = (currentLocation == LOC_RC) ? "weatherRC" : "weather";

  JsonArray trains = doc[trainsKey].as<JsonArray>();
  if (trains.isNull() || trains.size() < 2) {
    Serial.println("Missing/short trains array");
    return false;
  }

  JsonObject weather = doc[weatherKey].as<JsonObject>();
  w.tempF = weather["tempF"] | -999;
  w.precipProb = weather["precipProb"] | -1;
  w.precipIcon = (const char*)(weather["precipIcon"] | "");
  w.precipText = (const char*)(weather["precipText"] | "");

  auto fill = [&](JsonVariant v, Departure &out) {
    out.mins = v["mins"] | 0;
    out.depEpoch = (time_t)(v["depEpoch"] | 0);

    const char* type = v["type"] | "—";
    if      (!strcmp(type, "Local"))   out.dest = "LCL";
    else if (!strcmp(type, "Limited")) out.dest = "LTD";
    else if (!strcmp(type, "Express")) out.dest = "EXP";
    else if (!strcmp(type, "SCC"))     out.dest = "SCC";
    else                               out.dest = type;
  };

  fill(trains[0], d1);
  fill(trains[1], d2);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID,    WIFI_PASS);
  wifiMulti.addAP(WIFI_SSID_RC, WIFI_PASS_RC);

  Serial.print("WiFi connecting");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  currentLocation = (WiFi.SSID() == WIFI_SSID_RC) ? LOC_RC : LOC_HOME;
  Serial.println("\nWiFi connected to: " + WiFi.SSID());
  Serial.println(currentLocation == LOC_RC ? "Location: Redwood City" : "Location: Home");

  // Pacific timezone (DST aware) + NTP
  configTzTime("PST8PDT,M3.2.0/2,M11.1.0/2",
               "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  // Wait for NTP (up to ~8s)
  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 1700000000 && millis() - start < 8000) {
    delay(200);
    now = time(nullptr);
  }

  pinMode(TOUCH_IRQ, INPUT);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("BOOT", 10, 10, 2);
  delay(250);
}

void loop() {
  // Reconnect if dropped (location was already determined at boot)
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    while (wifiMulti.run() != WL_CONNECTED) {
      Serial.print(".");
      delay(500);
    }
    Serial.println("\nReconnected to: " + WiFi.SSID());
  }

  bool inWindow = (currentLocation != LOC_HOME) || isInDisplayWindow();

  if (!inWindow && millis() >= manualWakeUntil) {
    // Screen off — block here polling for a touch every 100ms
    digitalWrite(TFT_BL, LOW);
    while (digitalRead(TOUCH_IRQ) == HIGH && !isInDisplayWindow()) {
      delay(100);
    }
    manualWakeUntil = millis() + TOUCH_WAKE_MS;
    digitalWrite(TFT_BL, HIGH);
  }

  // Arriving here: screen is (or should be) on
  // Reset the timer if currently being touched
  if (!inWindow && digitalRead(TOUCH_IRQ) == LOW) {
    manualWakeUntil = millis() + TOUCH_WAKE_MS;
  }

  Departure a, b;
  Weather w;

  if (fetch_next2(a, b, w)) {
    Serial.printf("%dm — %s\n", a.mins, a.dest.c_str());
    Serial.printf("%dm — %s\n", b.mins, b.dest.c_str());
    Serial.printf("Weather: %dF, %d%%\n", w.tempF, w.precipProb);
    Serial.println("---");

    drawDepartures(a, b, w);
  }

  // Poll delay — check touch every 100ms so we can reset the timer or turn off promptly
  uint32_t pollEnd = millis() + POLL_MS;
  while (millis() < pollEnd) {
    if (!inWindow) {
      if (digitalRead(TOUCH_IRQ) == LOW) {
        manualWakeUntil = millis() + TOUCH_WAKE_MS;
      }
      if (millis() >= manualWakeUntil) {
        digitalWrite(TFT_BL, LOW);
        break;
      }
    }
    delay(100);
  }
}