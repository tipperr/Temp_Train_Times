/**
 * Caltrain Departure Display
 *
 * Fetches the next two Caltrain departures and local weather from a Cloudflare
 * Worker and renders them on a CYD (ESP32-2432S028) TFT display.
 *
 * Location is determined once at boot by checking which WiFi network the device
 * connects to. The worker returns data for both home (4th & King, SF) and
 * Redwood City in a single response; the correct set is selected here.
 *
 * Screen behaviour (home only):
 *   - Automatically on between 8:00am and 9:00am.
 *   - Touch-to-wake outside that window (stays on for TOUCH_WAKE_MS per touch).
 *   - At Redwood City the screen is always on.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#include "secrets.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

TFT_eSPI tft = TFT_eSPI();
WiFiMulti wifiMulti;

enum Location { LOC_HOME, LOC_RC };
static Location currentLocation = LOC_HOME;

// How often to fetch fresh data from the worker
static const uint32_t POLL_MS = 30 * 1000;

// How long a touch keeps the screen on outside the display window
static const uint32_t TOUCH_WAKE_MS = 10 * 1000;

// XPT2046 touch controller IRQ pin (active LOW when screen is pressed)
#define TOUCH_IRQ 36

// Timestamp (millis) until which the screen should stay on after a touch
static uint32_t manualWakeUntil = 0;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct Departure {
  int    mins;      // minutes until departure
  time_t depEpoch;  // departure time as UTC epoch
  String dest;      // abbreviated service type: "LCL", "LTD", "EXP", "SCC"
};

struct Weather {
  int    tempF;       // current temperature in °F; -999 if unavailable
  int    precipProb;  // precipitation probability 0–100; -1 if unavailable
  String precipIcon;  // "sun" or "umb"
  String precipText;  // short summary e.g. "3h+", "now", "60m"
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * Returns true if the current local time falls within the automatic display
 * window (8:00am – 9:00am).
 */
static bool isInDisplayWindow() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int minuteOfDay = lt.tm_hour * 60 + lt.tm_min;
  return minuteOfDay >= (8 * 60) && minuteOfDay < (9 * 60);
}

/**
 * Formats a UTC epoch as a 12-hour local time string, e.g. "8:04a", "12:30p".
 * Returns "--:--" if the epoch is zero or negative.
 */
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

// ---------------------------------------------------------------------------
// Icon drawing
// ---------------------------------------------------------------------------

/**
 * Draws a scalable umbrella icon with the top-left corner at (x, y).
 *
 * Uses lambdas for pixel, horizontal line, and vertical line primitives so
 * that at scale 1 they map to single-pixel TFT calls, and at scale > 1 they
 * map to filled rectangles for crisp scaling.
 *
 * @param x      Top-left x coordinate
 * @param y      Top-left y coordinate
 * @param s      Scale factor (1 = 16×16px, 2 = 32×32px, …)
 * @param color  TFT colour value
 */
static void drawUmbrellaIcon(int x, int y, int s, uint16_t color) {
  auto px = [&](int dx, int dy) {
    if (s <= 1) tft.drawPixel(x + dx, y + dy, color);
    else        tft.fillRect(x + dx*s, y + dy*s, s, s, color);
  };

  auto hline = [&](int dx0, int dx1, int dy) {
    if (dx1 < dx0) { int t = dx0; dx0 = dx1; dx1 = t; }
    if (s <= 1) tft.drawLine(x + dx0, y + dy, x + dx1, y + dy, color);
    else        tft.fillRect(x + dx0*s, y + dy*s, (dx1 - dx0 + 1)*s, s, color);
  };

  auto vline = [&](int dx, int dy0, int dy1) {
    if (dy1 < dy0) { int t = dy0; dy0 = dy1; dy1 = t; }
    if (s <= 1) tft.drawLine(x + dx, y + dy0, x + dx, y + dy1, color);
    else        tft.fillRect(x + dx*s, y + dy0*s, s, (dy1 - dy0 + 1)*s, color);
  };

  // Canopy
  hline(2, 13, 7);
  hline(3, 12, 6);
  hline(4, 11, 5);
  hline(5, 10, 4);
  hline(6, 9,  3);

  // Scallops along the canopy edge
  px(4, 8);
  px(7, 8);
  px(10, 8);

  // Handle
  vline(8, 7, 14);
  hline(8, 6, 14);
  px(6, 13);
}

/**
 * Draws a scalable sun icon with the top-left corner at (x, y).
 *
 * Coordinates are designed on a 14×14 pixel grid and multiplied by s.
 *
 * @param x      Top-left x coordinate
 * @param y      Top-left y coordinate
 * @param s      Scale factor (1 = 14×14px, 2 = 28×28px, …)
 * @param color  TFT colour value
 */
static void drawSunIcon(int x, int y, int s, uint16_t color) {
  int cx = x + 7*s, cy = y + 7*s;

  tft.drawCircle(cx, cy, 3*s, color);

  // Cardinal rays
  tft.drawLine(cx,        y + 1*s,  cx,        y + 3*s,  color); // top
  tft.drawLine(cx,        y + 11*s, cx,        y + 13*s, color); // bottom
  tft.drawLine(x + 1*s,  cy,       x + 3*s,   cy,       color); // left
  tft.drawLine(x + 11*s, cy,       x + 13*s,  cy,       color); // right

  // Diagonal rays
  tft.drawLine(x + 3*s,  y + 3*s,  x + 4*s,  y + 4*s,  color); // top-left
  tft.drawLine(x + 10*s, y + 10*s, x + 11*s, y + 11*s, color); // bottom-right
  tft.drawLine(x + 10*s, y + 4*s,  x + 11*s, y + 3*s,  color); // top-right
  tft.drawLine(x + 3*s,  y + 11*s, x + 4*s,  y + 10*s, color); // bottom-left
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

/**
 * Redraws the entire screen with the two upcoming departures and weather.
 *
 * Layout (320×240, landscape):
 *   - Header bar:  station name
 *   - Row 1:       next departure — large minutes, service type, clock time
 *   - Row 2:       following departure
 *   - Weather band (bottom 70px): temperature left, precip icon + text right
 */
static void drawDepartures(const Departure& a, const Departure& b, const Weather& w) {
  const int W = tft.width();
  const int H = tft.height();

  const int PAD             = 10;
  const int RESERVED_BOTTOM = 70;
  const int META_FONT       = 4;
  const int MIN_FONT        = 7;

  // Header
  const int headerY  = 10;
  const int headerH  = 34;
  const int topLineY = headerY + headerH;

  // Departure rows
  const int contentTop    = topLineY + 10;
  const int contentBottom = H - RESERVED_BOTTOM - 10;
  const int contentH      = contentBottom - contentTop;
  const int rowH          = contentH / 2;
  const int ROW_PAD_Y     = 4;
  const int row1Y         = contentTop + ROW_PAD_Y;
  const int row2Y         = contentTop + rowH + ROW_PAD_Y;
  const int midDividerY   = contentTop + rowH;

  // Minutes column
  const int MIN_COL_W = 110;
  const int metaX     = PAD + MIN_COL_W + 12;

  tft.fillScreen(TFT_BLACK);

  // --- Header ---
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  const char* header = (currentLocation == LOC_RC) ? "Caltrain  Redwood City" : "Caltrain  4th/King";
  tft.drawString(header, PAD, headerY, META_FONT);

  // --- Dividers ---
  tft.drawFastHLine(PAD, topLineY,            W - 2 * PAD, TFT_DARKGREY);
  tft.drawFastHLine(PAD, midDividerY,         W - 2 * PAD, TFT_DARKGREY);
  tft.drawFastHLine(PAD, H - RESERVED_BOTTOM, W - 2 * PAD, TFT_DARKGREY);

  // --- Departure rows ---
  auto drawRow = [&](int y, const Departure& d) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(d.mins) + "m", PAD, y + 2, MIN_FONT);

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(d.dest, metaX, y + 10, META_FONT);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawRightString(formatTimeLocal(d.depEpoch), W - PAD, y + 10, META_FONT);
  };

  drawRow(row1Y, a);
  drawRow(row2Y, b);

  // --- Weather band ---
  const int wxTop  = H - RESERVED_BOTTOM;
  const int wxH    = H - wxTop;
  const int textH  = 24; // approx height of font 2 @ size 3
  const int wxY    = wxTop + (wxH - textH) / 2;

  tft.setTextFont(2);
  tft.setTextSize(3);
  tft.setTextDatum(TL_DATUM);

  // Temperature (left)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String tempNum = (w.tempF <= -900) ? String("--") : String(w.tempF);
  int x = PAD;
  tft.drawString(tempNum, x, wxY);
  int numW  = tft.textWidth(tempNum);
  int degCx = x + numW + 6;
  int degCy = wxY + 7;
  tft.drawCircle(degCx, degCy, 3, TFT_WHITE);
  tft.drawString("F", degCx + 7, wxY);

  // Precipitation icon + text (right)
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  String ptxt = w.precipText.length() ? w.precipText : String("--");

  const int ICON_S = 2;
  const int ICON_W = 16 * ICON_S;

  int groupW = ICON_W + tft.textWidth(ptxt);
  int iconX  = W - PAD - groupW;
  int iconY  = wxY + 2;

  if      (w.precipIcon == "umb") drawUmbrellaIcon(iconX, iconY, ICON_S, TFT_CYAN);
  else if (w.precipIcon == "sun") drawSunIcon(iconX, iconY, ICON_S, TFT_CYAN);

  tft.drawString(ptxt, iconX + ICON_W, wxY);

  tft.setTextSize(1);
}

// ---------------------------------------------------------------------------
// Data fetching
// ---------------------------------------------------------------------------

/**
 * Fetches the next two departures and weather from the Cloudflare Worker.
 *
 * The worker returns data for both locations in one payload. The correct
 * keys ("trains" vs "trainsRC", "weather" vs "weatherRC") are chosen based
 * on currentLocation.
 *
 * @param d1  Output: next departure
 * @param d2  Output: departure after that
 * @param w   Output: current weather
 * @return    true on success, false on any network or parse error
 */
static bool fetch_next2(Departure &d1, Departure &d2, Weather &w) {
  WiFiClientSecure client;
  client.setInsecure(); // worker uses TLS but cert validation is skipped on-device
                        // no API key needed here — auth lives in the Worker

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
  w.tempF      = weather["tempF"]      | -999;
  w.precipProb = weather["precipProb"] | -1;
  w.precipIcon = (const char*)(weather["precipIcon"] | "");
  w.precipText = (const char*)(weather["precipText"] | "");

  // Map full service type names to short display labels
  auto fill = [&](JsonVariant v, Departure &out) {
    out.mins     = v["mins"]     | 0;
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

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  // Connect to whichever known network is in range.
  // Location is fixed for the lifetime of this boot.
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

  // Pacific time with DST, synced via NTP
  configTzTime("PST8PDT,M3.2.0/2,M11.1.0/2",
               "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  // Wait for NTP sync (up to 8s)
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
  // Reconnect if WiFi dropped (location is not re-evaluated)
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    while (wifiMulti.run() != WL_CONNECTED) {
      Serial.print(".");
      delay(500);
    }
    Serial.println("\nReconnected to: " + WiFi.SSID());
  }

  // At home, the screen is only on during the display window or after a touch.
  // At RC the screen is always on, so inWindow is always true.
  bool inWindow = (currentLocation != LOC_HOME) || isInDisplayWindow();

  if (!inWindow && millis() >= manualWakeUntil) {
    // Screen off — block in a 100ms poll loop until a touch or the window opens
    digitalWrite(TFT_BL, LOW);
    while (digitalRead(TOUCH_IRQ) == HIGH && !isInDisplayWindow()) {
      delay(100);
    }
    manualWakeUntil = millis() + TOUCH_WAKE_MS;
    digitalWrite(TFT_BL, HIGH);
  }

  // If the screen is on due to a touch, reset the timer on each new press
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

  // Wait POLL_MS before the next fetch, checking touch every 100ms so the
  // screen can turn off promptly when the manual wake timer expires.
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
