// MantisCore networking: background Wi-Fi connect, NTP sync and weather.
// Weather uses the same approach as Mantis Pinball's weather mode: Open-Meteo geocoding (ZIP -> place)
// then the Open-Meteo forecast and air-quality APIs over HTTPS (WiFiClientSecure, HTTP/1.0).
#include "mantis_internal.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

volatile bool mcNetBusy = false;
volatile bool mcWifiUp = false;
volatile bool mcNtpPending = false;

static McWeather wxShared;                                // written by the net task
static portMUX_TYPE wxMux = portMUX_INITIALIZER_UNLOCKED; // guards wxShared (plain data, memcpy only)
static TaskHandle_t netTask = nullptr;
static volatile bool netRestart = false;
static volatile bool netPaused = false;
static volatile bool netFetchNow = false;

void mcWeatherSnapshot(McWeather &out) {
    taskENTER_CRITICAL(&wxMux);
    memcpy(&out, &wxShared, sizeof(McWeather));
    taskEXIT_CRITICAL(&wxMux);
}
static void publish(const McWeather &w) {
    taskENTER_CRITICAL(&wxMux);
    memcpy(&wxShared, &w, sizeof(McWeather));
    taskEXIT_CRITICAL(&wxMux);
}

// HTTPS GET into a String (pattern from Mantis Pinball wxGetS).
static bool httpsGet(const char *host, const String &path, String &body) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(12000); // Arduino core 3: milliseconds (Stream semantics)
    if (!c.connect(host, 443)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: mantiscore/1\r\n"
             "Accept: application/json\r\nConnection: close\r\n\r\n",
             path.c_str(), host);
    uint32_t dl = millis() + 15000;
    while (c.connected() && millis() < dl) {
        String ln = c.readStringUntil('\n');
        if (ln == "\r" || ln == "") break;
        if (ln.startsWith("HTTP/") && ln.indexOf(" 200") < 0) {
            c.stop();
            return false;
        }
    }
    body = "";
    body.reserve(6144);
    while ((c.connected() || c.available()) && millis() < dl) {
        int ch = c.read();
        if (ch < 0) {
            delay(1);
            continue;
        }
        if (body.length() < 16384) body += (char)ch;
    }
    c.stop();
    return body.length() > 0;
}

static void copyTime(char *dst, const char *iso) { // "2026-10-20T07:41" -> "07:41"
    dst[0] = 0;
    if (!iso) return;
    const char *t = strchr(iso, 'T');
    if (t && strlen(t) >= 6) {
        memcpy(dst, t + 1, 5);
        dst[5] = 0;
    }
}

static bool fetchWeather(const String &zip, bool metric) {
    static String cachedZip;
    static float lat = 0, lon = 0;
    static char place[28] = "";
    String body;
    if (zip != cachedZip) {
        if (!httpsGet("geocoding-api.open-meteo.com", "/v1/search?name=" + zip + "&count=1&language=en&format=json", body))
            return false;
        JsonDocument doc;
        if (deserializeJson(doc, body)) return false;
        JsonVariant p = doc["results"][0];
        if (p.isNull()) return false;
        lat = p["latitude"] | 0.0f;
        lon = p["longitude"] | 0.0f;
        snprintf(place, sizeof(place), "%s", (const char *)(p["name"] | ""));
        cachedZip = zip;
    }
    McWeather w;
    w.metric = metric;
    snprintf(w.place, sizeof(w.place), "%s", place);
    char path[700];
    snprintf(path, sizeof(path),
             "/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,"
             "cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m,uv_index"
             "&hourly=temperature_2m,precipitation_probability&forecast_hours=12"
             "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,precipitation_probability_max,uv_index_max"
             "&forecast_days=1&timezone=auto&temperature_unit=%s&wind_speed_unit=%s&precipitation_unit=%s",
             lat, lon, metric ? "celsius" : "fahrenheit", metric ? "kmh" : "mph", metric ? "mm" : "inch");
    if (!httpsGet("api.open-meteo.com", path, body)) return false;
    {
        JsonDocument doc;
        if (deserializeJson(doc, body)) return false;
        JsonVariant cur = doc["current"];
        if (cur.isNull()) return false;
        w.temp = cur["temperature_2m"] | 0.0f;
        w.feels = cur["apparent_temperature"] | w.temp;
        w.humidity = cur["relative_humidity_2m"] | 0;
        w.isDay = (cur["is_day"] | 1) != 0;
        w.precip = cur["precipitation"] | 0.0f;
        w.code = cur["weather_code"] | -1;
        w.cloud = cur["cloud_cover"] | 0;
        w.pressure = cur["pressure_msl"] | 0.0f;
        w.wind = cur["wind_speed_10m"] | 0.0f;
        w.windDir = cur["wind_direction_10m"] | 0;
        w.gust = cur["wind_gusts_10m"] | 0.0f;
        w.uv = cur["uv_index"] | 0.0f;
        JsonVariant day = doc["daily"];
        w.hi = day["temperature_2m_max"][0] | w.temp;
        w.lo = day["temperature_2m_min"][0] | w.temp;
        w.popMax = day["precipitation_probability_max"][0] | 0;
        w.uvMax = day["uv_index_max"][0] | 0.0f;
        copyTime(w.sunrise, day["sunrise"][0] | "");
        copyTime(w.sunset, day["sunset"][0] | "");
        JsonArray ht = doc["hourly"]["temperature_2m"];
        JsonArray hp = doc["hourly"]["precipitation_probability"];
        for (JsonVariant v : ht) {
            if (w.hours >= 12 || v.isNull()) break;
            w.hourT[w.hours] = v.as<float>();
            w.hourPop[w.hours] = hp[w.hours] | 0;
            w.hours++;
        }
    }
    snprintf(path, sizeof(path),
             "/v1/air-quality?latitude=%.4f&longitude=%.4f&current=us_aqi,pm2_5,pm10,carbon_monoxide,ozone&timezone=auto",
             lat, lon);
    if (httpsGet("air-quality-api.open-meteo.com", path, body)) {
        JsonDocument doc;
        if (!deserializeJson(doc, body) && !doc["current"].isNull()) {
            JsonVariant cur = doc["current"];
            w.aqi = cur["us_aqi"] | -1;
            w.pm25 = cur["pm2_5"] | 0.0f;
            w.pm10 = cur["pm10"] | 0.0f;
            w.co = cur["carbon_monoxide"] | 0.0f;
            w.o3 = cur["ozone"] | 0.0f;
            w.aqiValid = w.aqi >= 0;
        }
    }
    w.updated = time(nullptr);
    w.valid = true;
    publish(w);
    return true;
}

// Same connect sequence as Mantis AI's connectWiFi().
static bool wifiConnect(const String &ssid, const String &pass, uint32_t timeoutMs) {
    WiFi.disconnect(true);
    delay(80);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
    return WiFi.status() == WL_CONNECTED;
}

static void netLoop(void *) {
    for (;;) {
        netRestart = false;
        mcWifiUp = false;
        // snapshot: the UI only edits these while the task is stopped (mcNetStop) or idle
        String ssid = mcCfg.ssid, pass = mcCfg.pass, zip = mcCfg.zip, tz = mcTimeZones[mcCfg.tzIndex].posix;
        bool metric = mcCfg.metric;
        if (!netPaused && ssid.length()) {
            mcNetBusy = true;
            if (wifiConnect(ssid, pass, 15000)) {
                mcWifiUp = true;
                configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
                for (int i = 0; i < 40 && time(nullptr) < 1704067200; i++) vTaskDelay(pdMS_TO_TICKS(250));
                if (time(nullptr) >= 1704067200) mcNtpPending = true; // UI thread writes the RTC
                if (zip.length() == 5) fetchWeather(zip, metric);
            }
            mcNetBusy = false;
        }
        // refresh every 15 minutes, on request, or reconnect when settings change
        while (!netRestart) {
            netFetchNow = false;
            for (int s = 0; s < 900 && !netRestart && !netFetchNow; s++) vTaskDelay(pdMS_TO_TICKS(1000));
            if (netRestart) break;
            if (mcWifiUp && !netPaused && zip.length() == 5 && WiFi.status() == WL_CONNECTED) {
                mcNetBusy = true;
                fetchWeather(zip, metric);
                mcNetBusy = false;
            } else if (!netPaused && ssid.length()) {
                break; // Wi-Fi dropped: reconnect
            }
        }
    }
}

void mcNetStart() {
    netPaused = false;
    if (netTask) {
        netRestart = true;
        return;
    }
    xTaskCreatePinnedToCore(netLoop, "mcNet", 16384, nullptr, 1, &netTask, 0);
}

void mcNetRefreshWeather() { netFetchNow = true; }

void mcNetStop() {
    netPaused = true;
    netRestart = true;
    for (int i = 0; i < 100 && mcNetBusy; i++) delay(100); // let an in-flight request finish
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    mcWifiUp = false;
}
