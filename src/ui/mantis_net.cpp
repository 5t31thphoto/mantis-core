// MantisCore networking: background Wi-Fi connect, NTP sync and weather (Open-Meteo, ZIP via zippopotam.us).
// Weather uses the same approach as Mantis Pinball's weather mode: Open-Meteo geocoding (ZIP -> lat/lon)
// and forecast over HTTPS with WiFiClientSecure.
#include "mantis_internal.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

volatile bool mcNetBusy = false;
volatile bool mcWifiUp = false;
volatile bool mcNtpPending = false;
McWeather mcWeather;

static TaskHandle_t netTask = nullptr;
static volatile bool netRestart = false;
static volatile bool netPaused = false;

const char *mcWeatherLabel(int code) {
    if (code == 0) return "Clear";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Snow";
    if (code >= 95) return "Storm";
    return "";
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
    uint32_t dl = millis() + 14000;
    while (c.connected() && millis() < dl) {
        String ln = c.readStringUntil('\n');
        if (ln == "\r" || ln == "") break;
        if (ln.startsWith("HTTP/") && ln.indexOf(" 200") < 0) {
            c.stop();
            return false;
        }
    }
    body = "";
    while ((c.connected() || c.available()) && millis() < dl) {
        int ch = c.read();
        if (ch < 0) {
            delay(1);
            continue;
        }
        if (body.length() < 8192) body += (char)ch;
    }
    c.stop();
    return body.length() > 0;
}

static bool fetchWeather(const String &zip) {
    static String cachedZip;
    static float lat = 0, lon = 0;
    String body;
    if (zip != cachedZip) {
        if (!httpsGet("geocoding-api.open-meteo.com", "/v1/search?name=" + zip + "&count=1&language=en&format=json", body))
            return false;
        JsonDocument doc;
        if (deserializeJson(doc, body)) return false;
        JsonVariant place = doc["results"][0];
        if (place.isNull()) return false;
        lat = place["latitude"] | 0.0f;
        lon = place["longitude"] | 0.0f;
        cachedZip = zip;
    }
    char path[160];
    snprintf(path, sizeof(path),
             "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&temperature_unit=fahrenheit",
             lat, lon);
    if (!httpsGet("api.open-meteo.com", path, body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonVariant cur = doc["current"];
    if (cur.isNull()) return false;
    mcWeather.tempF = (int)lroundf(cur["temperature_2m"] | 0.0f);
    mcWeather.code = cur["weather_code"] | -1;
    mcWeather.valid = true;
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
        if (!netPaused && ssid.length()) {
            mcNetBusy = true;
            if (wifiConnect(ssid, pass, 15000)) {
                mcWifiUp = true;
                configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
                for (int i = 0; i < 40 && time(nullptr) < 1704067200; i++) vTaskDelay(pdMS_TO_TICKS(250));
                if (time(nullptr) >= 1704067200) mcNtpPending = true; // UI thread writes the RTC
                if (zip.length() == 5) fetchWeather(zip);
            }
            mcNetBusy = false;
        }
        // refresh weather every 30 minutes, or sooner when settings change
        for (int s = 0; s < 1800 && !netRestart; s++) vTaskDelay(pdMS_TO_TICKS(1000));
        if (!netRestart && mcWifiUp && !netPaused && zip.length() == 5) {
            mcNetBusy = true;
            fetchWeather(zip);
            mcNetBusy = false;
            netRestart = false;
            for (int s = 0; s < 1800 && !netRestart; s++) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void mcNetStart() {
    netPaused = false;
    mcWeather.valid = false;
    if (netTask) {
        netRestart = true;
        return;
    }
    xTaskCreatePinnedToCore(netLoop, "mcNet", 16384, nullptr, 1, &netTask, 0);
}

void mcNetStop() {
    netPaused = true;
    netRestart = true;
    for (int i = 0; i < 100 && mcNetBusy; i++) delay(100); // let an in-flight request finish
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    mcWifiUp = false;
}
