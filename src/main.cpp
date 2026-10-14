// Weather Station v2 — scrolling forecast edition
// Board:    ESP32 DevKit V1 (ESP32-WROOM-32, D-numbered silkscreen)
// Display:  1.8" TFT, 128x160, ST7735, SPI (blue board, 8-pin header)
// Weather:  National Weather Service (api.weather.gov) for current conditions,
//           forecast and severe alerts; Open-Meteo for 15-minute rain timing.
//           Both are free and need no API key.
//
// New in v2: a scrolling marquee at the bottom of the screen with the next two
// forecast periods spelled out, e.g. "TONIGHT: Mostly Clear, 5% rain ...
// WEDNESDAY: Showers And Thunderstorms Likely, 72% rain, 1/4-1/2 in".
//
// Setup is done from a phone, no computer or code changes needed:
//   1. Plug the station into any USB phone charger.
//   2. The screen says to join the Wi-Fi network "Weather-Setup" on a phone.
//   3. A page pops up: pick the home Wi-Fi, type its password and the ZIP
//      code, tap Save. The station finds the location, city name and time
//      zone from the ZIP code.
// If the saved Wi-Fi stops working (new router/password), the setup network
// comes back on its own. Holding the BOOT button for 5 seconds erases the
// settings and starts setup over.
//
// Two builds (platformio.ini):
//   pio run -e gift -t upload   for gift units: no Home Assistant, no secrets
//   pio run -e home -t upload   for James's station: also publishes to Home
//     Assistant over MQTT (logins in secrets.h), using the same entities as
//     the original firmware (device id weather_tft_<last 3 MAC bytes>).
//
// Wiring (TFT 8-pin header -> ESP32):
//   RST->D4  CS->D15  D/C->D2  DIN->D23  CLK->D18  VCC->VIN  BL->3V3  GND->GND
// (D2 also drives the board's blue LED, so it flickers while drawing.)

#include <ctype.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Fonts/FreeSansBold18pt7b.h>  // smooth big digits for the temperature
#include <Fonts/FreeSansBold9pt7b.h>   // smooth smaller numbers (high/low, dew point, pressure)

// ---------- Settings you edit ----------
#ifdef USE_MQTT
#include "secrets.h"  // MQTT login for Home Assistant (kept out of GitHub)
#endif

const char* SETUP_NETWORK = "Weather-Setup";      // Wi-Fi name shown during setup
const uint32_t SETUP_PORTAL_TIMEOUT_S = 600;     // then retry the saved Wi-Fi
const int RESET_BUTTON = 0;                      // BOOT button
const uint32_t RESET_HOLD_MS = 5000;

const char* NWS_USER_AGENT = "weather-station-v2 (github.com/booherjr707-code)";

const uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;  // conditions + forecast
const uint32_t ALERT_REFRESH_MS   = 2UL  * 60UL * 1000UL;  // severe weather alerts
const uint32_t RAIN_REFRESH_MS    = 5UL  * 60UL * 1000UL;  // 15-minute rain timing
const uint32_t WIFI_TIMEOUT_MS = 20000;

// The header rotates between the city name and the next three days' forecast.
// While a severe alert is active, this is replaced by a flashing alert banner.
const uint32_t HEADER_SLOT_SECS = 3;

// Bottom-line forecast marquee speed.
const uint32_t MARQUEE_MS_PER_PX = 45;  // ~22 pixels per second

// TFT pins (SPI). See wiring comment above.
#define TFT_SCLK  18
#define TFT_MOSI  23
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4

// Display settings. The startup screen shows RED, GREEN, BLUE, YELLOW bars:
//   - red and blue swapped?        flip PANEL_BGR
//   - colors look like a negative? flip PANEL_INVERT (Lonely Binary panel: true)
//   - picture upside down?         change ROTATION from 1 to 3
//   - a line of junk on one edge?  set the offsets to 2 and 1
const bool PANEL_BGR    = false;
const bool PANEL_INVERT = false;
const uint8_t ROTATION  = 1;
const uint8_t COL_OFFSET = 0;
const uint8_t ROW_OFFSET = 0;
// ---------------------------------------

const int SCREEN_W = 160;
const int SCREEN_H = 128;

// Vertical barometer gauge, right edge, with a blue "H" at the top and an
// orange "L" at the bottom. A blue line + white arrow mark the current
// reading: above the middle for high pressure, below it for low, pinning at
// the ends once the reading is PRESSURE_GAUGE_RANGE inHg from center.
const float PRESSURE_GAUGE_CENTER = 29.92f;
const float PRESSURE_GAUGE_RANGE  = 0.5f;
const int   GAUGE_W      = 11;
const int   GAUGE_X      = SCREEN_W - GAUGE_W - 6;
const int   GAUGE_TOP    = 15;
const int   GAUGE_BOTTOM = 126;

// Body text stays left of the gauge and its H/L labels.
const int CONTENT_RIGHT = GAUGE_X - 9;

const int WIFI_X = SCREEN_W - 18;  // drawWiFiBars occupies [x, x+14]

// Layout rows, matching the original screen.
const int TEMP_TOP   = 16;  // big temperature
const int CLOCK_X    = 80;  // clock, high and low column
const int MARQUEE_Y  = 59;  // scrolling forecast (where "Partly Cloudy" was)
const int FEELS_Y    = 70;
const int WIND_Y     = 81;
const int RULE_Y     = 93;
const int LABELS_Y   = 97;   // "DEW PT" / "BARO inHg"
const int BIGNUM_TOP = 108;  // dew point and pressure numbers
const int BARO_X     = 48;

// Everything is drawn into a memory canvas first and sent to the screen in
// one go, so it doesn't flicker.
Arduino_DataBus* bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED, VSPI);
Arduino_GFX* panel = new Arduino_ST7735(bus, TFT_RST, ROTATION, PANEL_INVERT, 128, 160,
                                        COL_OFFSET, ROW_OFFSET, COL_OFFSET, ROW_OFFSET, PANEL_BGR);
Arduino_Canvas* gfx = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);

// Colors (RGB565)
const uint16_t C_BLACK   = 0x0000;
const uint16_t C_WHITE   = 0xFFFF;
const uint16_t C_GRAY    = 0x8410;
const uint16_t C_DKGRAY  = 0x2104;
const uint16_t C_NAVY    = 0x0010;
const uint16_t C_RED     = 0xF800;
const uint16_t C_ORANGE  = 0xFC00;
const uint16_t C_YELLOW  = 0xFFE0;
const uint16_t C_GREEN   = 0x07E0;
const uint16_t C_BLUE    = 0x001F;
const uint16_t C_CYAN    = 0x07FF;
const uint16_t C_HEADER  = 0x001F;    // top bar (blue)
const uint16_t C_TEMP    = C_YELLOW;  // big temperature
const uint16_t C_HIGH    = 0xFD20;    // high temperature (orange)
const uint16_t C_LOW     = 0x87FF;    // low temperature (light blue)
const uint16_t C_COND    = 0x9FF7;    // forecast line (pale aqua)
const uint16_t C_DEW     = 0x87E0;    // dew point (lime green)
const uint16_t C_GAUGE_H = 0x3B3F;    // gauge "H" and marker (blue)

struct Location {
  bool valid = false;
  char gridId[8] = "";
  int  gridX = 0, gridY = 0;
  char station[8] = "";
  char city[24] = "";
};

struct Weather {
  bool  valid = false;
  float temp = 0, feelsLike = 0, wind = 0, dewPoint = 0, pressureInHg = 0;
  int   high = 0, low = 0;
  int   humidity = 0;
  char  condition[24] = "";
  char  windDir[4] = "";
};

struct ForecastDay {
  char name[8] = "";
  char cond[20] = "";
  int  high = 0, low = 0;
};

struct Alert {
  bool active = false;
  int  severityRank = 0;   // 0 none, 1 unknown/minor .. 5 extreme
  int  count = 0;
  char event[36] = "";     // most urgent one
};

// Open-Meteo 15-minute rain amounts for now, +15, +30, +45, +60 minutes.
const int RAIN_SLOTS = 5;
const float RAIN_MIN_IN = 0.004f;  // per 15 min; below this counts as dry
struct Nowcast {
  bool  valid = false;
  float slot[RAIN_SLOTS] = {0};
  int   firstWet = -1;  // first slot with rain, -1 = dry for the next hour
};

// Saved settings (ESP32 flash, survive power-off). Filled in by the phone
// setup page; lat/lon, city and time zone come from the ZIP code.
struct Settings {
  char  zip[6] = "";
  float lat = 0, lon = 0;
  char  city[24] = "";
  char  tz[48] = "";
};
Settings cfg;
Preferences prefs;

Location loc;
Weather weather;
ForecastDay forecast[3];
bool forecastValid = false;
char marqueeText[360] = "";
char stormLead[64] = "";  // e.g. "STORMS STARTING ~7 AM, peak 72% ~1 PM" from the hourly forecast
bool marqueeWet = false;  // highlight the marquee when rain/storms are mentioned
Nowcast nowcast;
Alert alert;
char alertsJson[512] = "{\"alerts\":[]}";
bool lastFetchFailed = false;
bool lastAlertFetchFailed = false;
uint32_t lastFetch = 0;
uint32_t lastAlertFetch = 0;
uint32_t lastRainFetch = 0;

int severityRank(const char* sev) {
  if (strcmp(sev, "Extreme")  == 0) return 5;
  if (strcmp(sev, "Severe")   == 0) return 4;
  if (strcmp(sev, "Moderate") == 0) return 3;
  if (strcmp(sev, "Minor")    == 0) return 2;
  return 1;
}

const char* compassPoint(float deg) {
  static const char* points[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                                  "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  int i = (int)((deg / 22.5f) + 0.5f) % 16;
  if (i < 0) i += 16;
  return points[i];
}

// Case-insensitive substring check (strcasestr isn't reliably available in
// the ESP32 Arduino toolchain).
bool containsCI(const char* haystack, const char* needle) {
  char h[64], n[16];
  strlcpy(h, haystack, sizeof(h));
  strlcpy(n, needle, sizeof(n));
  for (char* c = h; *c; c++) *c = tolower(*c);
  for (char* c = n; *c; c++) *c = tolower(*c);
  return strstr(h, n) != nullptr;
}

bool mentionsRain(const char* s) {
  return containsCI(s, "rain") || containsCI(s, "shower") ||
         containsCI(s, "storm") || containsCI(s, "drizzle");
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

void drawWiFiBars(int x, int bottom) {
  int bars = wifiBars();
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 2;
    if (i < bars) gfx->fillRect(x + i * 4, bottom - h, 3, h, C_WHITE);
    else          gfx->drawFastHLine(x + i * 4, bottom - 1, 3, C_GRAY);
  }
}

int textWidth(const char* s, int size) { return strlen(s) * 6 * size; }

void drawText(int x, int y, const char* s, uint16_t color, int size = 1) {
  gfx->setFont((const GFXfont*)nullptr);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void drawCentered(int y, const char* s, uint16_t color, int size = 1) {
  drawText((SCREEN_W - textWidth(s, size)) / 2, y, s, color, size);
}

void drawRight(int right, int y, const char* s, uint16_t color, int size = 1) {
  drawText(right - textWidth(s, size), y, s, color, size);
}

void showMessage(const char* line1, const char* line2 = "", uint16_t color1 = C_WHITE) {
  gfx->fillScreen(C_BLACK);
  drawCentered(48, line1, color1, 1);
  drawCentered(64, line2, C_GRAY, 1);
  gfx->flush();
}

void showSplash() {
  gfx->fillScreen(C_BLACK);
  drawCentered(8, "WEATHER", C_YELLOW, 2);
  drawCentered(28, "STATION v2", C_WHITE, 1);
  const uint16_t bars[] = {C_RED, C_GREEN, C_BLUE, C_YELLOW};
  const char* names[] = {"RED", "GREEN", "BLUE", "YELLOW"};
  for (int i = 0; i < 4; i++) {
    gfx->fillRect(0, 44 + i * 18, SCREEN_W, 18, bars[i]);
    drawCentered(49 + i * 18, names[i], i == 3 ? C_BLACK : C_WHITE, 1);
  }
  drawCentered(118, "Starting...", C_GRAY, 1);
  gfx->flush();
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin();  // credentials saved by the setup page
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// GETs an HTTPS JSON URL into doc, keeping only the fields in filter.
bool getJson(const String& url, JsonDocument& doc, JsonDocument& filter, const char* label) {
  if (!connectWiFi()) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);
  http.addHeader("Accept", "application/geo+json");

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("%s HTTP error: %d\n", label, status);
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("%s JSON error: %s\n", label, err.c_str());
    return false;
  }
  return true;
}

// Looks up the NWS forecast grid, nearest observation station and city name
// for the saved location.
bool lookupLocation() {
  String base = String("https://api.weather.gov/points/") +
                String(cfg.lat, 4) + "," + String(cfg.lon, 4);
  JsonDocument filter;
  filter["properties"]["gridId"] = true;
  filter["properties"]["gridX"] = true;
  filter["properties"]["gridY"] = true;
  filter["properties"]["observationStations"] = true;
  filter["properties"]["relativeLocation"]["properties"]["city"] = true;
  filter["properties"]["relativeLocation"]["properties"]["state"] = true;
  JsonDocument doc;
  if (!getJson(base, doc, filter, "Points")) return false;

  JsonObject p = doc["properties"];
  strlcpy(loc.gridId, p["gridId"] | "", sizeof(loc.gridId));
  loc.gridX = p["gridX"] | 0;
  loc.gridY = p["gridY"] | 0;
  String stationsUrl = p["observationStations"] | "";
  if (cfg.city[0]) {
    strlcpy(loc.city, cfg.city, sizeof(loc.city));
  } else {
    snprintf(loc.city, sizeof(loc.city), "%s, %s",
             (const char*)(p["relativeLocation"]["properties"]["city"] | "?"),
             (const char*)(p["relativeLocation"]["properties"]["state"] | ""));
    for (char* c = loc.city; *c; c++) *c = toupper(*c);
  }
  if (!loc.gridId[0] || stationsUrl.isEmpty()) return false;

  JsonDocument sFilter;
  sFilter["features"][0]["properties"]["stationIdentifier"] = true;
  JsonDocument sDoc;
  if (!getJson(stationsUrl, sDoc, sFilter, "Stations")) return false;
  strlcpy(loc.station, sDoc["features"][0]["properties"]["stationIdentifier"] | "",
          sizeof(loc.station));
  if (!loc.station[0]) return false;

  loc.valid = true;
  Serial.printf("Location: %s, grid %s %d,%d, station %s\n",
                loc.city, loc.gridId, loc.gridX, loc.gridY, loc.station);
  return true;
}

// Live current conditions from the nearest NWS observation station.
bool fetchConditions() {
  String url = String("https://api.weather.gov/stations/") + loc.station + "/observations/latest";
  JsonDocument filter;
  JsonObject f = filter["properties"].to<JsonObject>();
  for (const char* k : {"temperature", "heatIndex", "windChill", "dewpoint", "barometricPressure",
                        "seaLevelPressure", "windSpeed", "windDirection", "relativeHumidity"}) {
    f[k]["value"] = true;
  }
  f["textDescription"] = true;
  JsonDocument doc;
  if (!getJson(url, doc, filter, "Conditions")) return false;

  JsonObject p = doc["properties"];
  float tempC = p["temperature"]["value"] | NAN;
  if (isnan(tempC)) return false;

  float feelsC = p["heatIndex"]["value"] | NAN;
  if (isnan(feelsC)) feelsC = p["windChill"]["value"] | NAN;
  if (isnan(feelsC)) feelsC = tempC;

  float dewC = p["dewpoint"]["value"] | tempC;
  float pressurePa = p["barometricPressure"]["value"] | NAN;
  if (isnan(pressurePa)) pressurePa = p["seaLevelPressure"]["value"] | NAN;
  float windKmh  = p["windSpeed"]["value"] | 0.0f;
  float windDeg  = p["windDirection"]["value"] | NAN;
  float humidity = p["relativeHumidity"]["value"] | 0.0f;
  const char* desc = p["textDescription"] | "Unknown";

  weather.temp         = tempC * 9.0f / 5.0f + 32.0f;
  weather.feelsLike    = feelsC * 9.0f / 5.0f + 32.0f;
  weather.dewPoint     = dewC * 9.0f / 5.0f + 32.0f;
  weather.pressureInHg = isnan(pressurePa) ? 0.0f : pressurePa / 3386.39f;
  weather.humidity     = (int)roundf(humidity);
  weather.wind         = windKmh * 0.621371f;
  strlcpy(weather.windDir, isnan(windDeg) ? "--" : compassPoint(windDeg), sizeof(weather.windDir));
  strlcpy(weather.condition, desc, sizeof(weather.condition));
  weather.valid = true;

  Serial.printf("%.0fF, %s, dew %.0fF, %.2f inHg\n",
                weather.temp, weather.condition, weather.dewPoint, weather.pressureInHg);
  return true;
}

// Copies the sentence mentioning rainfall amounts out of an NWS detailed
// forecast, e.g. "rainfall amounts between a quarter and half of an inch
// possible". Empty if there isn't one.
void rainfallPhrase(const char* detailed, char* out, size_t len) {
  out[0] = 0;
  const char* hit = strstr(detailed, "ainfall amounts");
  if (!hit) return;
  const char* start = hit - 1;  // the "r"/"R" of rainfall
  const char* end = strchr(hit, '.');
  size_t n = end ? (size_t)(end - start) : strlen(start);
  if (n >= len) n = len - 1;
  memcpy(out, start, n);
  out[n] = 0;
  out[0] = tolower(out[0]);
}

// Appends one forecast period to the marquee, e.g.
// "WEDNESDAY: Showers And Thunderstorms Likely, 72% rain, rainfall amounts ...".
void appendMarqueePeriod(JsonObject p) {
  char name[20], piece[200], rainfall[110];
  strlcpy(name, p["name"] | "", sizeof(name));
  for (char* c = name; *c; c++) *c = toupper(*c);
  const char* shortFc = p["shortForecast"] | "";
  int pop = p["probabilityOfPrecipitation"]["value"] | 0;
  int temp = p["temperature"] | 0;
  bool isDay = p["isDaytime"] | true;
  rainfallPhrase(p["detailedForecast"] | "", rainfall, sizeof(rainfall));

  int n = snprintf(piece, sizeof(piece), "%s: %s, %s %d, %d%% rain", name, shortFc,
                   isDay ? "high" : "low", temp, pop);
  if (rainfall[0] && n < (int)sizeof(piece)) {
    snprintf(piece + n, sizeof(piece) - n, ", %s", rainfall);
  }
  if (marqueeText[0]) strlcat(marqueeText, "   *   ", sizeof(marqueeText));
  strlcat(marqueeText, piece, sizeof(marqueeText));
  if (pop >= 30 || mentionsRain(shortFc)) marqueeWet = true;
}

// Today's high/low, the next 3 days for the header, and the marquee text for
// the next two periods, from the NWS gridpoint forecast.
bool fetchForecast() {
  String url = String("https://api.weather.gov/gridpoints/") + loc.gridId + "/" +
               loc.gridX + "," + loc.gridY + "/forecast";
  JsonDocument filter;
  JsonObject f = filter["properties"]["periods"][0].to<JsonObject>();
  f["name"] = true;
  f["startTime"] = true;
  f["isDaytime"] = true;
  f["temperature"] = true;
  f["shortForecast"] = true;
  f["detailedForecast"] = true;
  f["probabilityOfPrecipitation"]["value"] = true;
  JsonDocument doc;
  if (!getJson(url, doc, filter, "Forecast")) return false;

  JsonArray periods = doc["properties"]["periods"];

  marqueeText[0] = 0;
  marqueeWet = false;
  for (int i = 0; i < 2 && i < (int)periods.size(); i++) appendMarqueePeriod(periods[i]);
  Serial.printf("Marquee: %s\n", marqueeText);

  bool gotHigh = false, gotLow = false;
  int dayCount = -1;  // 0 = today; 1..3 = the header days
  char lastDate[11] = "";

  for (JsonObject p : periods) {
    const char* start = p["startTime"] | "";
    char date[11] = "";
    strncpy(date, start, 10);
    date[10] = 0;
    bool isDay = p["isDaytime"] | false;
    int  temp  = p["temperature"] | 0;
    const char* cond = p["shortForecast"] | "";

    if (!gotHigh && isDay)  { weather.high = temp; gotHigh = true; }
    if (!gotLow  && !isDay) { weather.low  = temp; gotLow  = true; }

    if (strcmp(date, lastDate) != 0) {
      dayCount++;
      strlcpy(lastDate, date, sizeof(lastDate));
      if (dayCount > 3) break;
      if (dayCount >= 1) {
        ForecastDay& d = forecast[dayCount - 1];
        d.high = 0;
        d.low  = 0;
        d.cond[0] = 0;
        struct tm t = {};
        int y, m, day;
        if (sscanf(date, "%d-%d-%d", &y, &m, &day) == 3) {
          t.tm_year = y - 1900;
          t.tm_mon  = m - 1;
          t.tm_mday = day;
          t.tm_hour = 12;
          mktime(&t);  // fills in the weekday
          strftime(d.name, sizeof(d.name), "%a", &t);
        } else {
          strlcpy(d.name, "---", sizeof(d.name));
        }
      }
    }

    if (dayCount >= 1 && dayCount <= 3) {
      ForecastDay& d = forecast[dayCount - 1];
      if (isDay) {
        d.high = temp;
        strlcpy(d.cond, cond, sizeof(d.cond));
      } else {
        d.low = temp;
        if (!d.cond[0]) strlcpy(d.cond, cond, sizeof(d.cond));
      }
    }
  }

  forecastValid = (dayCount >= 3);
  return true;
}

void formatHour(const char* isoTime, char* out, size_t len) {
  int hour = atoi(isoTime + 11);  // NWS times are already local, e.g. "2026-10-14T07:00:00-05:00"
  int h12 = hour % 12;
  snprintf(out, len, "%d %s", h12 == 0 ? 12 : h12, hour < 12 ? "AM" : "PM");
}

// Looks through the next 24 hours of the NWS hourly forecast for when rain
// or storms start and when the chance peaks, so the marquee can say
// "STORMS STARTING ~7 AM, peak 72% ~1 PM". Empty when nothing is coming.
bool fetchHourly() {
  String url = String("https://api.weather.gov/gridpoints/") + loc.gridId + "/" +
               loc.gridX + "," + loc.gridY + "/forecast/hourly";
  JsonDocument filter;
  JsonObject f = filter["properties"]["periods"][0].to<JsonObject>();
  f["startTime"] = true;
  f["shortForecast"] = true;
  f["probabilityOfPrecipitation"]["value"] = true;
  JsonDocument doc;
  if (!getJson(url, doc, filter, "Hourly")) return false;

  const int START_POP = 30;  // % chance that counts as "starting"
  JsonArray periods = doc["properties"]["periods"];
  int startIdx = -1, peakIdx = -1, peakPop = 0;
  bool storms = false;
  for (int i = 0; i < 24 && i < (int)periods.size(); i++) {
    JsonObject p = periods[i];
    int pop = p["probabilityOfPrecipitation"]["value"] | 0;
    const char* sf = p["shortForecast"] | "";
    if (pop >= START_POP && startIdx < 0) startIdx = i;
    if (startIdx >= 0 && containsCI(sf, "thunder")) storms = true;
    if (pop > peakPop) { peakPop = pop; peakIdx = i; }
  }

  stormLead[0] = 0;
  if (startIdx < 0) return true;
  const char* kind = storms ? "STORMS" : "RAIN";
  char startAt[8], peakAt[8];
  formatHour(periods[startIdx]["startTime"] | "", startAt, sizeof(startAt));
  formatHour(periods[peakIdx]["startTime"] | "", peakAt, sizeof(peakAt));
  if (startIdx == 0) snprintf(stormLead, sizeof(stormLead), "%s NOW, peak %d%% ~%s", kind, peakPop, peakAt);
  else snprintf(stormLead, sizeof(stormLead), "%s STARTING ~%s, peak %d%% ~%s", kind, startAt, peakPop, peakAt);
  Serial.printf("Lead: %s\n", stormLead);
  return true;
}

// Active NWS alerts (watches/warnings/advisories) for this point.
bool fetchAlerts() {
  String url = String("https://api.weather.gov/alerts/active?point=") +
               String(cfg.lat, 4) + "," + String(cfg.lon, 4);
  JsonDocument filter;
  filter["features"][0]["properties"]["event"] = true;
  filter["features"][0]["properties"]["severity"] = true;
  filter["features"][0]["properties"]["ends"] = true;
  filter["features"][0]["properties"]["expires"] = true;
  JsonDocument doc;
  if (!getJson(url, doc, filter, "Alerts")) return false;

  JsonDocument list;
  JsonArray out = list["alerts"].to<JsonArray>();
  int bestRank = 0;
  const char* bestEvent = "Alert";
  int count = 0;
  for (JsonObject f : doc["features"].as<JsonArray>()) {
    JsonObject p = f["properties"];
    count++;
    const char* sev = p["severity"] | "Unknown";
    int rank = severityRank(sev);
    if (rank > bestRank) {
      bestRank = rank;
      bestEvent = p["event"] | "Alert";
    }
    JsonObject a = out.add<JsonObject>();
    a["event"] = p["event"] | "Alert";
    a["severity"] = sev;
    a["ends"] = p["ends"].isNull() ? p["expires"] : p["ends"];
  }
  serializeJson(list, alertsJson, sizeof(alertsJson));

  bool wasActive = alert.active;
  alert.active = bestRank > 0;
  alert.severityRank = bestRank;
  alert.count = count;
  if (alert.active) {
    strlcpy(alert.event, bestEvent, sizeof(alert.event));
    if (!wasActive) Serial.printf("ALERT: %s\n", alert.event);
  } else {
    alert.event[0] = 0;
    if (wasActive) Serial.println("Alerts cleared");
  }
  return true;
}

// Open-Meteo 15-minute rain amounts for the next hour.
bool fetchNowcast() {
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&minutely_15=precipitation&forecast_minutely_15=" + RAIN_SLOTS +
               "&precipitation_unit=inch&timezone=auto";
  JsonDocument filter;
  filter["minutely_15"]["precipitation"] = true;
  JsonDocument doc;
  if (!getJson(url, doc, filter, "Rain")) return false;

  JsonArray p = doc["minutely_15"]["precipitation"];
  if (p.size() < RAIN_SLOTS) return false;
  nowcast.firstWet = -1;
  for (int i = 0; i < RAIN_SLOTS; i++) {
    nowcast.slot[i] = p[i] | 0.0f;
    if (nowcast.firstWet < 0 && nowcast.slot[i] >= RAIN_MIN_IN) nowcast.firstWet = i;
  }
  nowcast.valid = true;
  Serial.printf("Rain (in/15min): %.3f %.3f %.3f %.3f %.3f\n", nowcast.slot[0], nowcast.slot[1],
                nowcast.slot[2], nowcast.slot[3], nowcast.slot[4]);
  return true;
}

void drawDegree(int x, int y, int r = 2, uint16_t color = C_WHITE) {
  gfx->drawCircle(x, y, r, color);
}

// Scrolls text right-to-left across [0, boxW) at row y; static if it fits.
void drawMarquee(int y, const char* text, int boxW, uint16_t color) {
  int textW = textWidth(text, 1);
  if (textW <= boxW) {
    drawText((boxW - textW) / 2, y, text, color, 1);
    return;
  }
  const int gap = 40;
  int totalW = textW + gap;
  int offset = (millis() / MARQUEE_MS_PER_PX) % totalW;
  int x = boxW - offset;  // enters from the right edge
  drawText(x, y, text, color, 1);
  if (x + totalW < boxW) drawText(x + totalW, y, text, color, 1);
  if (x > 0 && x - totalW + textW > 0) drawText(x - totalW, y, text, color, 1);
}

// Replaces the header with a blinking banner naming the most urgent active
// alert, e.g. "!! SEVERE THUNDERSTORM WARNING".
void drawAlertBanner() {
  bool flash = (millis() / 500) % 2 == 0;
  gfx->fillRect(0, 0, SCREEN_W, 12, flash ? C_RED : C_YELLOW);
  char banner[40];
  snprintf(banner, sizeof(banner), "!! %s", alert.event);
  drawMarquee(2, banner, SCREEN_W, flash ? C_WHITE : C_BLACK);
}

void formatClock(char* buf, size_t len) {
  struct tm t;
  if (getLocalTime(&t, 0)) {
    int h = t.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, len, "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  } else {
    snprintf(buf, len, "--:--");
  }
}

// Header text: cycles through the city name and the next three days, e.g.
// "Tue 93/74 Sunny". The condition is dropped if it won't fit.
void drawHeaderText() {
  const char* text = loc.city[0] ? loc.city : cfg.city;
  char buf[28];
  uint32_t slot = (millis() / 1000 / HEADER_SLOT_SECS) % 4;
  if (forecastValid && slot > 0) {
    const ForecastDay& d = forecast[slot - 1];
    snprintf(buf, sizeof(buf), "%s %d/%d %s", d.name, d.high, d.low, d.cond);
    if (textWidth(buf, 1) > WIFI_X - 6) snprintf(buf, sizeof(buf), "%s %d/%d", d.name, d.high, d.low);
    text = buf;
  }
  drawText(3, 2, text, C_WHITE, 1);
}

void drawHeader() {
  if (alert.active) {
    drawAlertBanner();
  } else {
    gfx->fillRect(0, 0, SCREEN_W, 12, C_HEADER);
    drawHeaderText();
    drawWiFiBars(WIFI_X, 11);
    if (lastFetchFailed) drawText(WIFI_X - 8, 2, "!", C_YELLOW, 1);
  }
}

// Barometer gauge. See the PRESSURE_GAUGE_*/GAUGE_* constants above.
void drawPressureGauge() {
  drawText(GAUGE_X - 8, GAUGE_TOP, "H", C_GAUGE_H, 1);
  drawText(GAUGE_X - 8, GAUGE_BOTTOM - 7, "L", C_HIGH, 1);
  gfx->drawRect(GAUGE_X, GAUGE_TOP, GAUGE_W, GAUGE_BOTTOM - GAUGE_TOP, C_WHITE);
  if (weather.pressureInHg <= 0) return;

  int centerY = (GAUGE_TOP + GAUGE_BOTTOM) / 2;
  float delta = weather.pressureInHg - PRESSURE_GAUGE_CENTER;
  float frac  = constrain(delta / PRESSURE_GAUGE_RANGE, -1.0f, 1.0f);
  int halfTrack = (GAUGE_BOTTOM - GAUGE_TOP) / 2 - 3;
  int pointerY  = centerY - (int)roundf(frac * halfTrack);

  gfx->fillRect(GAUGE_X + 1, pointerY - 1, GAUGE_W - 2, 3, C_GAUGE_H);
  gfx->fillTriangle(GAUGE_X - 1, pointerY,
                    GAUGE_X - 6, pointerY - 3,
                    GAUGE_X - 6, pointerY + 3, C_WHITE);
}

const char* rainIntensity(float inPer15) {
  float perHour = inPer15 * 4;
  if (perHour < 0.1f) return "light";
  if (perHour < 0.3f) return "moderate";
  return "heavy";
}

// Full-screen rain countdown from Open-Meteo, shown when rain is due within
// the hour: a headline plus a bar for each 15-minute slot.
void drawNowcastScreen() {
  char buf[40];
  gfx->fillScreen(C_NAVY);
  drawCentered(3, "RAIN - NEXT HOUR", C_WHITE, 1);
  gfx->drawFastHLine(0, 13, SCREEN_W, C_GRAY);

  int wet = nowcast.firstWet;
  float peak = 0;
  for (int i = 0; i < RAIN_SLOTS; i++) peak = max(peak, nowcast.slot[i]);

  if (wet == 0) {
    drawCentered(20, "RAINING NOW", C_YELLOW, 2);
    int dry = -1;
    for (int i = 1; i < RAIN_SLOTS; i++) if (nowcast.slot[i] < RAIN_MIN_IN) { dry = i; break; }
    if (dry < 0) snprintf(buf, sizeof(buf), "%s, for 1 hr+", rainIntensity(peak));
    else snprintf(buf, sizeof(buf), "%s, ends in ~%d min", rainIntensity(peak), dry * 15);
  } else {
    drawCentered(20, "RAIN EXPECTED IN", C_YELLOW, 1);
    snprintf(buf, sizeof(buf), "~%d min", wet * 15);
    drawCentered(31, buf, C_YELLOW, 2);
    snprintf(buf, sizeof(buf), "%s rain", rainIntensity(peak));
  }
  drawCentered(50, buf, C_CYAN, 1);

  // Bars: height scaled so 0.10 in per 15 min (heavy) fills the box.
  const int baseY = 112, maxH = 46, barW = 22, gapW = 8;
  const int left = (SCREEN_W - (RAIN_SLOTS * barW + (RAIN_SLOTS - 1) * gapW)) / 2;
  const char* labels[] = {"now", "+15", "+30", "+45", "+60"};
  for (int i = 0; i < RAIN_SLOTS; i++) {
    int x = left + i * (barW + gapW);
    int h = (int)roundf(constrain(nowcast.slot[i] / 0.10f, 0.0f, 1.0f) * maxH);
    if (nowcast.slot[i] >= RAIN_MIN_IN && h < 2) h = 2;
    gfx->drawFastHLine(x, baseY, barW, C_GRAY);
    if (h > 0) gfx->fillRect(x, baseY - h, barW, h, C_CYAN);
    drawText(x + (barW - textWidth(labels[i], 1)) / 2, baseY + 4, labels[i], C_WHITE, 1);
  }
  gfx->flush();
}

// Draws text in a smooth font with the top of its tallest glyph at topY.
// Returns the drawn width.
int drawSmooth(const GFXfont* font, int x, int topY, const char* s, uint16_t color) {
  gfx->setFont(font);
  gfx->setTextSize(1);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds("0", 0, 0, &x1, &y1, &w, &h);  // digit height sets the baseline
  int baseline = topY - y1;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(x - x1, baseline);
  gfx->print(s);
  gfx->setFont((const GFXfont*)nullptr);
  return w;
}

// A number in the small smooth font followed by a degree mark.
void drawDegrees(int x, int topY, int value, uint16_t color) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  int w = drawSmooth(&FreeSansBold9pt7b, x, topY, buf, color);
  drawDegree(x + w + 4, topY + 2, 2, color);
}

void drawWeather() {
  char buf[40];
  gfx->fillScreen(C_BLACK);
  drawHeader();

  // Big temperature + °F, top left.
  snprintf(buf, sizeof(buf), "%d", (int)roundf(weather.temp));
  int w = drawSmooth(&FreeSansBold18pt7b, 3, TEMP_TOP, buf, C_TEMP);
  drawDegree(3 + w + 6, TEMP_TOP + 3, 3, C_TEMP);
  drawSmooth(&FreeSansBold9pt7b, 3 + w + 11, TEMP_TOP, "F", C_TEMP);

  // Clock, then high and low, in the column to the right.
  formatClock(buf, sizeof(buf));
  drawText(CLOCK_X, TEMP_TOP, buf, C_WHITE, 1);
  drawText(CLOCK_X, 32, "H", C_WHITE, 1);
  drawDegrees(CLOCK_X + 8, 28, weather.high, C_HIGH);
  drawText(CLOCK_X, 48, "L", C_WHITE, 1);
  drawDegrees(CLOCK_X + 8, 44, weather.low, C_LOW);

  // Scrolling forecast line: current condition, then tonight/tomorrow in
  // full. Yellow when rain or storms are in it, so it catches the eye.
  {
    static char line[sizeof(marqueeText) + sizeof(stormLead) + 40];
    if (stormLead[0]) snprintf(line, sizeof(line), "%s   *   Now: %s   *   %s", stormLead, weather.condition, marqueeText);
    else if (marqueeText[0]) snprintf(line, sizeof(line), "Now: %s   *   %s", weather.condition, marqueeText);
    else strlcpy(line, weather.condition, sizeof(line));
    drawMarquee(MARQUEE_Y, line, CONTENT_RIGHT, (marqueeWet || stormLead[0]) ? C_YELLOW : C_COND);
    // Keep the scrolling text out of the gauge column.
    gfx->fillRect(CONTENT_RIGHT, MARQUEE_Y - 1, SCREEN_W - CONTENT_RIGHT, 10, C_BLACK);
  }

  snprintf(buf, sizeof(buf), "Feels %d  Hum %d%%", (int)roundf(weather.feelsLike), weather.humidity);
  drawText(2, FEELS_Y, buf, C_WHITE, 1);
  snprintf(buf, sizeof(buf), "Wind %s %d mph", weather.windDir, (int)roundf(weather.wind));
  drawText(2, WIND_Y, buf, C_WHITE, 1);

  gfx->drawFastHLine(0, RULE_Y, CONTENT_RIGHT - 2, C_GRAY);

  // Bottom row: dew point and barometer.
  drawText(2, LABELS_Y, "DEW PT", C_WHITE, 1);
  drawDegrees(2, BIGNUM_TOP, (int)roundf(weather.dewPoint), C_DEW);
  drawText(BARO_X, LABELS_Y, "BARO inHg", C_WHITE, 1);
  if (weather.pressureInHg > 0) snprintf(buf, sizeof(buf), "%.2f", weather.pressureInHg);
  else strlcpy(buf, "--.--", sizeof(buf));
  drawSmooth(&FreeSansBold9pt7b, BARO_X, BIGNUM_TOP, buf, C_WHITE);

  drawPressureGauge();
  gfx->flush();
}

// ---------- Home Assistant (MQTT) ----------
#ifdef USE_MQTT
// Same device id, topics, keys and unique_ids as the original firmware, so
// Home Assistant keeps the existing entities when this replaces it.

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
char deviceId[24];
char stateTopic[48];
char availTopic[48];
char alertTopic[48];
uint32_t lastMqttTry = 0;
uint32_t lastStatePublish = 0;
bool mqttStarted = false;

bool mqttEnabled() { return MQTT_HOST[0] != 0; }

void mqttSetup() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(deviceId, sizeof(deviceId), "weather_tft_%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(stateTopic, sizeof(stateTopic), "%s/state", deviceId);
  snprintf(availTopic, sizeof(availTopic), "%s/status", deviceId);
  snprintf(alertTopic, sizeof(alertTopic), "%s/alert", deviceId);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(3);
  mqttStarted = true;
}

void publishJson(const char* topic, JsonDocument& doc, bool retain = true) {
  static char out[1024];
  size_t n = serializeJson(doc, out, sizeof(out));
  mqtt.publish(topic, (const uint8_t*)out, n, retain);
}

void addDevice(JsonDocument& d) {
  JsonObject dev = d["device"].to<JsonObject>();
  dev["identifiers"][0] = deviceId;
  dev["name"] = "Weather Alert Display";
  dev["manufacturer"] = "DIY";
  dev["model"] = "ESP32 + 1.8in TFT";
}

struct SensorDef {
  const char* key;
  const char* name;
  const char* unit;
  const char* deviceClass;
  const char* icon;
};

const SensorDef SENSORS[] = {
  {"temp",        "Temperature",    "°F",   "temperature",          nullptr},
  {"feels",       "Feels Like",     "°F",   "temperature",          nullptr},
  {"high",        "High Today",     "°F",   "temperature",          nullptr},
  {"low",         "Low Tonight",    "°F",   "temperature",          nullptr},
  {"dewpoint",    "Dew Point",      "°F",   "temperature",          nullptr},
  {"humidity",    "Humidity",       "%",    "humidity",             nullptr},
  {"pressure",    "Pressure",       "inHg", "atmospheric_pressure", nullptr},
  {"wind",        "Wind Speed",     "mph",  "wind_speed",           nullptr},
  {"wind_dir",    "Wind Direction", nullptr, nullptr,               "mdi:compass"},
  {"condition",   "Condition",      nullptr, nullptr,               "mdi:weather-partly-cloudy"},
  {"alert",       "Weather Alert",  nullptr, nullptr,               "mdi:alert"},
  {"alert_count", "Alert Count",    nullptr, nullptr,               "mdi:alert-circle"},
  {"rssi",        "WiFi Signal",    "dBm",  "signal_strength",      nullptr},
};

void publishDiscovery() {
  char topic[96];
  char uid[48];
  for (const SensorDef& sd : SENSORS) {
    JsonDocument d;
    snprintf(uid, sizeof(uid), "%s_%s", deviceId, sd.key);
    d["name"] = sd.name;
    d["unique_id"] = uid;
    d["state_topic"] = stateTopic;
    d["value_template"] = String("{{ value_json.") + sd.key + " }}";
    d["availability_topic"] = availTopic;
    if (sd.unit) d["unit_of_measurement"] = sd.unit;
    if (sd.deviceClass) {
      d["state_class"] = "measurement";
      d["device_class"] = sd.deviceClass;
    }
    if (sd.icon) d["icon"] = sd.icon;
    if (strcmp(sd.key, "rssi") == 0) d["entity_category"] = "diagnostic";
    if (strcmp(sd.key, "alert") == 0) d["json_attributes_topic"] = alertTopic;
    addDevice(d);
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", deviceId, sd.key);
    publishJson(topic, d);
  }

  JsonDocument d;
  snprintf(uid, sizeof(uid), "%s_alert_active", deviceId);
  d["name"] = "Severe Weather";
  d["unique_id"] = uid;
  d["state_topic"] = stateTopic;
  d["value_template"] = "{{ 'ON' if value_json.alert_count > 0 else 'OFF' }}";
  d["availability_topic"] = availTopic;
  d["device_class"] = "safety";
  d["json_attributes_topic"] = alertTopic;
  addDevice(d);
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s/alert_active/config", deviceId);
  publishJson(topic, d);
}

float round1(float v) { return roundf(v * 10) / 10; }

void publishState() {
  if (!mqtt.connected()) return;
  JsonDocument d;
  if (weather.valid) {
    d["temp"]     = round1(weather.temp);
    d["feels"]    = round1(weather.feelsLike);
    d["high"]     = weather.high;
    d["low"]      = weather.low;
    d["dewpoint"] = round1(weather.dewPoint);
    if (weather.pressureInHg > 0) d["pressure"] = roundf(weather.pressureInHg * 100) / 100;
    d["humidity"] = weather.humidity;
    d["wind"]     = round1(weather.wind);
    d["wind_dir"] = weather.windDir;
    d["condition"] = weather.condition;
  }
  d["alert"]       = alert.active ? alert.event : "None";
  d["alert_count"] = alert.count;
  d["rssi"]        = WiFi.RSSI();
  publishJson(stateTopic, d);
  mqtt.publish(alertTopic, alertsJson, true);
  lastStatePublish = millis();
}

void mqttLoop() {
  if (!mqttEnabled() || WiFi.status() != WL_CONNECTED) return;
  if (!mqttStarted) mqttSetup();
  if (!mqtt.connected()) {
    if (lastMqttTry != 0 && millis() - lastMqttTry < 30000) return;
    lastMqttTry = millis();
    const char* user = MQTT_USER[0] ? MQTT_USER : nullptr;
    const char* pass = MQTT_USER[0] ? MQTT_PASSWORD : nullptr;
    if (!mqtt.connect(deviceId, user, pass, availTopic, 0, true, "offline")) {
      Serial.printf("MQTT connect failed (state %d)\n", mqtt.state());
      return;
    }
    Serial.println("MQTT connected");
    mqtt.publish(availTopic, "online", true);
    publishDiscovery();
    publishState();
  }
  mqtt.loop();
  if (millis() - lastStatePublish > 60000UL) publishState();
}
#else
void mqttLoop() {}  // gift build: no Home Assistant
#endif

// ---------- Phone setup (Wi-Fi + ZIP code) ----------

void showSetupScreen() {
  gfx->fillScreen(C_BLACK);
  gfx->fillRect(0, 0, SCREEN_W, 14, C_HEADER);
  drawCentered(3, "WEATHER STATION SETUP", C_WHITE, 1);
  drawCentered(22, "On your phone, open", C_WHITE, 1);
  drawCentered(33, "Wi-Fi settings and join:", C_WHITE, 1);
  drawCentered(48, SETUP_NETWORK, C_YELLOW, 2);
  drawCentered(72, "A page will pop up.", C_WHITE, 1);
  drawCentered(85, "Pick your home Wi-Fi,", C_CYAN, 1);
  drawCentered(96, "type its password and", C_CYAN, 1);
  drawCentered(107, "your ZIP code, then Save.", C_CYAN, 1);
  gfx->flush();
}

void loadSettings() {
  prefs.begin("wx", true);
  prefs.getString("zip", cfg.zip, sizeof(cfg.zip));
  cfg.lat = prefs.getFloat("lat", 0);
  cfg.lon = prefs.getFloat("lon", 0);
  prefs.getString("city", cfg.city, sizeof(cfg.city));
  prefs.getString("tz", cfg.tz, sizeof(cfg.tz));
  prefs.end();
}

void saveSettings() {
  prefs.begin("wx", false);
  prefs.putString("zip", cfg.zip);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("city", cfg.city);
  prefs.putString("tz", cfg.tz);
  prefs.end();
}

bool haveLocation() { return cfg.zip[0] && cfg.tz[0] && (cfg.lat != 0 || cfg.lon != 0); }

// Converts an IANA time zone name (from Open-Meteo) into the POSIX rule the
// ESP32 clock needs. Covers the US zones; anything else falls back to a
// fixed offset without daylight saving.
void posixFromIana(const char* iana, long utcOffsetSec, char* out, size_t len) {
  struct Zone { const char* prefix; const char* rule; };
  static const Zone zones[] = {
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},   {"America/Detroit", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Indiana/", "EST5EDT,M3.2.0,M11.1.0"},   {"America/Kentucky/", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Indiana/Knox", "CST6CDT,M3.2.0,M11.1.0"}, {"America/Indiana/Tell_City", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},    {"America/Menominee", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/North_Dakota/", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},     {"America/Boise", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"}, {"America/Juneau", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Sitka", "AKST9AKDT,M3.2.0,M11.1.0"},    {"America/Nome", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu", "HST10"},
    {"America/Puerto_Rico", "AST4"},
  };
  // Longest matching prefix wins (so Indiana/Knox beats Indiana/).
  const Zone* best = nullptr;
  for (const Zone& z : zones) {
    if (strncmp(iana, z.prefix, strlen(z.prefix)) == 0 &&
        (!best || strlen(z.prefix) > strlen(best->prefix))) best = &z;
  }
  if (best) { strlcpy(out, best->rule, len); return; }
  // POSIX offsets are hours *west* of UTC, so the sign flips.
  snprintf(out, len, "UTC%+ld", -utcOffsetSec / 3600);
}

// Turns the ZIP code into lat/lon, a city name and a time zone.
bool lookupZip(const char* zip) {
  JsonDocument all;  // no filtering needed, these replies are tiny
  all.set(true);
  JsonDocument doc;
  if (!getJson(String("https://api.zippopotam.us/us/") + zip, doc, all, "ZIP")) return false;
  JsonObject place = doc["places"][0];
  if (place.isNull()) return false;
  float lat = atof(place["latitude"] | "0");
  float lon = atof(place["longitude"] | "0");
  if (lat == 0 && lon == 0) return false;
  char city[24];
  snprintf(city, sizeof(city), "%s, %s", (const char*)(place["place name"] | "?"),
           (const char*)(place["state abbreviation"] | ""));
  for (char* c = city; *c; c++) *c = toupper(*c);

  JsonDocument tzDoc;
  String tzUrl = String("https://api.open-meteo.com/v1/forecast?latitude=") + String(lat, 4) +
                 "&longitude=" + String(lon, 4) + "&timezone=auto&forecast_days=1";
  JsonDocument tzFilter;
  tzFilter["timezone"] = true;
  tzFilter["utc_offset_seconds"] = true;
  if (!getJson(tzUrl, tzDoc, tzFilter, "Timezone")) return false;

  strlcpy(cfg.zip, zip, sizeof(cfg.zip));
  cfg.lat = lat;
  cfg.lon = lon;
  strlcpy(cfg.city, city, sizeof(cfg.city));
  posixFromIana(tzDoc["timezone"] | "", tzDoc["utc_offset_seconds"] | 0L, cfg.tz, sizeof(cfg.tz));
  saveSettings();
  Serial.printf("ZIP %s -> %s (%.4f, %.4f), TZ %s\n", cfg.zip, cfg.city, cfg.lat, cfg.lon, cfg.tz);
  return true;
}

// Connects to the saved Wi-Fi, or runs the phone setup page when there isn't
// one (or it stops working, or no ZIP code is saved yet). Doesn't return
// until the station is online with a location.
void wifiAndLocationSetup() {
  WiFiManager wm;
  char zipDefault[6];
  strlcpy(zipDefault, cfg.zip, sizeof(zipDefault));
  WiFiManagerParameter zipField("zip", "Your 5-digit ZIP code", zipDefault, 5,
                                "inputmode='numeric' pattern='[0-9]{5}' required");
  wm.addParameter(&zipField);
  wm.setTitle("Weather Station");
  static const char* menu[] = {"wifi"};  // just the one button, keep it simple
  wm.setMenu(menu, 1);
  wm.setShowInfoUpdate(false);
  wm.setConfigPortalTimeout(SETUP_PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(20);
  wm.setAPCallback([](WiFiManager*) { showSetupScreen(); });

  for (;;) {
    bool online;
    if (haveLocation()) {
      showMessage("Connecting to Wi-Fi...");
      online = wm.autoConnect(SETUP_NETWORK);  // falls back to the setup page
    } else {
      online = wm.startConfigPortal(SETUP_NETWORK);
    }
    if (!online) {
      // Nobody finished setup within the timeout (or the router is down).
      showMessage("No Wi-Fi yet", "Trying again...", C_ORANGE);
      delay(3000);
      ESP.restart();
    }

    const char* zip = zipField.getValue();
    if (strlen(zip) == 5 && (strcmp(zip, cfg.zip) != 0 || !haveLocation())) {
      showMessage("Finding your town...", zip);
      if (!lookupZip(zip)) {
        showMessage("ZIP code not found", "Please set up again", C_ORANGE);
        cfg.zip[0] = 0;
        saveSettings();
        delay(4000);
        continue;  // back to the setup page
      }
    }
    if (haveLocation()) return;
    continue;  // connected but still no ZIP: ask again
  }
}

// Holding BOOT for RESET_HOLD_MS erases the Wi-Fi and ZIP code and restarts
// into setup, e.g. before giving the station to someone else.
void checkResetButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(RESET_BUTTON) == LOW) {
    if (pressedAt == 0) pressedAt = millis();
    if (millis() - pressedAt >= RESET_HOLD_MS) {
      showMessage("Erasing settings...", "Setup will start", C_ORANGE);
      prefs.begin("wx", false);
      prefs.clear();
      prefs.end();
      WiFi.disconnect(true, true);  // also forgets the saved Wi-Fi
      delay(2000);
      ESP.restart();
    }
  } else {
    pressedAt = 0;
  }
}

// ---------- Main ----------

void refreshWeather() {
  if (!loc.valid && !lookupLocation()) {
    lastFetchFailed = true;
    lastFetch = millis();
    return;
  }
  bool condOk = fetchConditions();
  bool fcOk   = fetchForecast();
  if (fcOk) fetchHourly();
  lastFetchFailed = !(condOk && fcOk);
  lastFetch = millis();
}

void setup() {
  Serial.begin(115200);

  if (!gfx->begin()) Serial.println("Display init failed");
  gfx->setTextWrap(false);  // the marquee draws text partly off-screen
  showSplash();
  delay(2500);

  pinMode(RESET_BUTTON, INPUT_PULLUP);
  loadSettings();
  wifiAndLocationSetup();
  showMessage("Wi-Fi connected", WiFi.localIP().toString().c_str());
  Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
  delay(1000);
  configTzTime(cfg.tz, "pool.ntp.org", "time.nist.gov");

  showMessage("Getting weather...");
  refreshWeather();
  lastAlertFetchFailed = !fetchAlerts();
  lastAlertFetch = millis();
  fetchNowcast();
  lastRainFetch = millis();

  mqttLoop();
}

void loop() {
  checkResetButton();
  uint32_t interval = lastFetchFailed ? 60UL * 1000UL : WEATHER_REFRESH_MS;
  if (millis() - lastFetch >= interval) refreshWeather();

  uint32_t alertInterval = lastAlertFetchFailed ? 30UL * 1000UL : ALERT_REFRESH_MS;
  if (millis() - lastAlertFetch >= alertInterval) {
    lastAlertFetchFailed = !fetchAlerts();
    lastAlertFetch = millis();
  }

  if (millis() - lastRainFetch >= RAIN_REFRESH_MS) {
    fetchNowcast();
    lastRainFetch = millis();
  }

  static uint32_t lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 300000UL) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }
  mqttLoop();

  // Rain due within the hour: alternate every 5s between the normal screen
  // and the rain countdown. A severe weather alert always wins.
  bool rainSoon = nowcast.valid && nowcast.firstWet >= 0;
  bool showNowcast = rainSoon && !alert.active && ((millis() / 5000) % 2 == 1);

  if (showNowcast) {
    drawNowcastScreen();
    delay(200);
  } else if (weather.valid) {
    drawWeather();  // redrawn often so the marquee scrolls smoothly
    delay(15);
  } else {
    showMessage("No weather yet", "Retrying in 1 min");
    delay(1000);
  }
}
