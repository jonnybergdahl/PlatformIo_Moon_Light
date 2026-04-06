/// Moon Phase Light
///
/// Drives a 3-LED SK6812 WWA (Cool White / Warm White / Amber) strip
/// to display the current moon phase. Uses WiFiManager for provisioning
/// and optionally publishes a Home Assistant light via MQTT discovery.

#include <Arduino.h>
#ifdef ESP8266
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
#ifdef ESP8266
  #define LED_PIN       D1   // D1 on D1 Mini
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  #define LED_PIN       1   // GPIO1 on ESP32-S3 Super Mini
#else
  #define LED_PIN       5   // GPIO5 on ESP32 D1 Mini
#endif
#define LED_COUNT       5

// ---------------------------------------------------------------------------
// SK6812 WWA channel mapping (GRB byte order on the wire)
//   R position -> Cool White
//   G position -> Amber
//   B position -> Warm White
//
// Adafruit NeoPixel Color(r,g,b) maps r->R, g->G, b->B
// so: Color(coolWhite, amber, warmWhite)
// ---------------------------------------------------------------------------
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
#define WIFI_AP_NAME    "MoonPhase-Setup"
#define CONFIG_FILE     "/config.json"

// MQTT topics (chip-id is appended at runtime)
#define MQTT_BASE_TOPIC     "moonphase"
#define HA_DISCOVERY_PREFIX "homeassistant"

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);
WiFiManager  wm;

// ---------------------------------------------------------------------------
// Configuration stored in LittleFS
// ---------------------------------------------------------------------------
struct Config {
    bool     mqttEnabled  = false;
    char     mqttHost[64] = "";
    uint16_t mqttPort     = 1883;
    char     mqttUser[32] = "";
    char     mqttPass[64] = "";
    bool     haDiscovery  = false;
    uint8_t  brightness   = 255;
} config;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
enum StartupState { STARTUP_WIFI, STARTUP_NTP, STARTUP_READY };
StartupState startupState = STARTUP_WIFI;

bool     lightOn            = true;
uint8_t  brightness         = 255;  // capped to config.brightness at startup
double   currentPhase       = 0.0;
double   currentIllumination = 0.0;

unsigned long lastMoonUpdate      = 0;
unsigned long lastMqttReconnect   = 0;
bool          mqttWasConnected    = false;

// Derived at startup from ESP.getChipId()
String chipId;
String mqttClientId;
String mqttStateTopic;
String mqttCmdTopic;
String mqttAvailTopic;
String haDiscoveryTopic;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void loadConfig();
void saveConfig();
void buildTopics();
double getMoonPhase();
double getMoonIllumination(double phase);
const char* getMoonPhaseName(double phase);
void updateLEDs();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void publishState();
void publishDiscovery();

// ===================================================================
//  Configuration persistence (LittleFS + JSON)
// ===================================================================

void loadConfig() {
    if (!LittleFS.exists(CONFIG_FILE)) return;
    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) return;

    JsonDocument doc;
    if (deserializeJson(doc, file) == DeserializationError::Ok) {
        config.mqttEnabled = doc["mqtt_enabled"] | false;
        strlcpy(config.mqttHost, doc["mqtt_host"] | "", sizeof(config.mqttHost));
        config.mqttPort = doc["mqtt_port"] | 1883;
        strlcpy(config.mqttUser, doc["mqtt_user"] | "", sizeof(config.mqttUser));
        strlcpy(config.mqttPass, doc["mqtt_pass"] | "", sizeof(config.mqttPass));
        config.haDiscovery = doc["ha_discovery"] | false;
        config.brightness = doc["brightness"] | 255;
    }
    file.close();
}

void saveConfig() {
    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) return;

    JsonDocument doc;
    doc["mqtt_enabled"] = config.mqttEnabled;
    doc["mqtt_host"]    = config.mqttHost;
    doc["mqtt_port"]    = config.mqttPort;
    doc["mqtt_user"]    = config.mqttUser;
    doc["mqtt_pass"]    = config.mqttPass;
    doc["ha_discovery"] = config.haDiscovery;
    doc["brightness"]   = config.brightness;
    serializeJson(doc, file);
    file.close();
}

// ===================================================================
//  MQTT topic helpers
// ===================================================================

void buildTopics() {
#ifdef ESP8266
    chipId            = String(ESP.getChipId(), HEX);
#else
    uint32_t efuse = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
    chipId            = String(efuse, HEX);
#endif
    mqttClientId      = "moonphase-" + chipId;
    mqttStateTopic    = String(MQTT_BASE_TOPIC) + "/" + chipId + "/state";
    mqttCmdTopic      = String(MQTT_BASE_TOPIC) + "/" + chipId + "/set";
    mqttAvailTopic    = String(MQTT_BASE_TOPIC) + "/" + chipId + "/available";
    haDiscoveryTopic  = String(HA_DISCOVERY_PREFIX) + "/light/moonphase_" + chipId + "/config";
}

// ===================================================================
//  Moon-phase calculation
// ===================================================================

/// Returns the moon phase as a value in [0, 1).
/// 0.0 = new moon, ~0.5 = full moon, approaching 1.0 = next new moon.
double getMoonPhase() {
    time_t now;
    time(&now);
    if (now < 1000000000L) return 0.0;   // NTP not synced yet

    // Reference new moon: 2000-01-06 18:14 UTC  (Unix 947182440)
    const double refNewMoon   = 947182440.0;
    const double synodicMonth = 29.53058868 * 86400.0;   // seconds

    double elapsed = (double)now - refNewMoon;
    double phase   = fmod(elapsed, synodicMonth) / synodicMonth;
    if (phase < 0.0) phase += 1.0;

    double illum = 0.5 * (1.0 - cos(2.0 * M_PI * phase));
    Serial.printf("[moon] time=%lu phase=%.4f illum=%.4f name=%s\n",
                  (unsigned long)now, phase, illum, getMoonPhaseName(phase));
    return phase;
}

/// Illumination fraction [0, 1] derived from phase angle.
double getMoonIllumination(double phase) {
    return 0.5 * (1.0 - cos(2.0 * M_PI * phase));
}

/// Phase name matching Home Assistant's Moon integration.
const char* getMoonPhaseName(double phase) {
    if (phase < 0.0625)  return "new_moon";
    if (phase < 0.1875)  return "waxing_crescent";
    if (phase < 0.3125)  return "first_quarter";
    if (phase < 0.4375)  return "waxing_gibbous";
    if (phase < 0.5625)  return "full_moon";
    if (phase < 0.6875)  return "waning_gibbous";
    if (phase < 0.8125)  return "last_quarter";
    if (phase < 0.9375)  return "waning_crescent";
    return "new_moon";
}

// ===================================================================
//  LED control
// ===================================================================

static inline void setPixelWWA(uint8_t pixel,
                               uint8_t warmWhite,
                               uint8_t coolWhite,
                               uint8_t amber) {
    strip.setPixelColor(pixel, strip.Color(coolWhite, amber, warmWhite));
}

/// Pulsing effect for startup states. Uses a sine wave for smooth breathing.
void pulseLEDs(uint8_t warmWhite, uint8_t coolWhite, uint8_t amber) {
    double t = (millis() % 2000) / 2000.0;          // 0..1 over 2 seconds
    double pulse = 0.5 * (1.0 - cos(2.0 * M_PI * t)); // smooth 0..1..0
    double maxScale = config.brightness / 255.0;
    uint8_t ww = (uint8_t)(warmWhite * pulse * maxScale);
    uint8_t cw = (uint8_t)(coolWhite * pulse * maxScale);
    uint8_t am = (uint8_t)(amber * pulse * maxScale);
    for (uint8_t i = 0; i < LED_COUNT; i++) {
        setPixelWWA(i, ww, cw, am);
    }
    strip.show();
}

void updateLEDs() {
    if (!lightOn) {
        strip.clear();
        strip.show();
        return;
    }

    double phase = getMoonPhase();
    double illum = getMoonIllumination(phase);
    currentPhase        = phase;
    currentIllumination = illum;

    double scale = brightness / 255.0;

    // Colour mapping
    //   New  moon (illum ~0):   dim amber glow
    //   Quarter    (illum ~0.5): warm white dominant, amber receding
    //   Full moon  (illum ~1):   bright cool white + some warm white
    uint8_t cw, ww, am;

    if (illum < 0.5) {
        double t = illum * 2.0;                     // 0 .. 1
        am = (uint8_t)(scale * (30.0 + 100.0 * (1.0 - t)));
        ww = (uint8_t)(scale * 200.0 * t);
        cw = 0;
    } else {
        double t = (illum - 0.5) * 2.0;            // 0 .. 1
        am = (uint8_t)(scale * 30.0 * (1.0 - t));
        ww = (uint8_t)(scale * (200.0 - 80.0 * t));
        cw = (uint8_t)(scale * 255.0 * t);
    }

    for (uint8_t i = 0; i < LED_COUNT; i++) {
        setPixelWWA(i, ww, cw, am);
    }
    strip.show();
}

// ===================================================================
//  MQTT  –  Home Assistant discovery + state
// ===================================================================

void publishDiscovery() {
    if (!config.haDiscovery || !mqttClient.connected()) return;

    JsonDocument doc;
    doc["name"]               = "Moon Phase " + chipId;
    doc["unique_id"]          = "moonphase_" + chipId;
    doc["object_id"]          = "moonphase_" + chipId;
    doc["command_topic"]      = mqttCmdTopic;
    doc["state_topic"]        = mqttStateTopic;
    doc["availability_topic"] = mqttAvailTopic;
    doc["schema"]             = "json";
    doc["brightness"]         = true;
    doc["brightness_scale"]   = 100;

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0]  = "moonphase_" + chipId;
    device["name"]            = "Moon Phase Light " + chipId;
    device["model"]           = "Moon Phase Light";
    device["manufacturer"]    = "DIY";
    device["sw_version"]      = __DATE__;

    char buf[600];
    serializeJson(doc, buf, sizeof(buf));
    Serial.printf("[mqtt] discovery -> %s\n", haDiscoveryTopic.c_str());
    mqttClient.publish(haDiscoveryTopic.c_str(), buf, true);
}

void publishState() {
    if (!config.mqttEnabled || !mqttClient.connected()) return;

    JsonDocument doc;
    doc["state"]        = lightOn ? "ON" : "OFF";
    doc["brightness"]   = (int)(brightness * 100.0 / config.brightness + 0.5);
    doc["moon_phase"]   = getMoonPhaseName(currentPhase);
    doc["illumination"] = ((int)(currentIllumination * 100.0 + 0.5)) / 100.0;

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    Serial.printf("[mqtt] state -> %s: %s\n", mqttStateTopic.c_str(), buf);
    mqttClient.publish(mqttStateTopic.c_str(), buf, true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length) != DeserializationError::Ok) return;

    Serial.printf("[mqtt] cmd received on %s\n", topic);

    if (doc["state"].is<const char*>()) {
        lightOn = (strcmp(doc["state"], "ON") == 0);
        Serial.printf("[mqtt] state -> %s\n", lightOn ? "ON" : "OFF");
    }
    if (doc["brightness"].is<int>()) {
        int pct = doc["brightness"];
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        brightness = (uint8_t)(pct * config.brightness / 100);
        Serial.printf("[mqtt] brightness -> %d%% = %d (max %d)\n", pct, brightness, config.brightness);
    }

    updateLEDs();
    publishState();
}

void mqttReconnect() {
    if (!config.mqttEnabled || strlen(config.mqttHost) == 0) return;
    if (mqttClient.connected()) return;

    unsigned long now = millis();
    if (now - lastMqttReconnect < 5000) return;
    lastMqttReconnect = now;

    Serial.print("MQTT connecting to ");
    Serial.print(config.mqttHost);
    Serial.print(":");
    Serial.println(config.mqttPort);

    bool ok;
    if (strlen(config.mqttUser) > 0) {
        ok = mqttClient.connect(mqttClientId.c_str(),
                                config.mqttUser, config.mqttPass,
                                mqttAvailTopic.c_str(), 0, true, "offline");
    } else {
        ok = mqttClient.connect(mqttClientId.c_str(),
                                mqttAvailTopic.c_str(), 0, true, "offline");
    }

    if (ok) {
        Serial.println("MQTT connected");
        mqttClient.publish(mqttAvailTopic.c_str(), "online", true);
        mqttClient.subscribe(mqttCmdTopic.c_str());
        publishDiscovery();
        publishState();
        mqttWasConnected = true;
    } else {
        Serial.print("MQTT failed, rc=");
        Serial.println(mqttClient.state());
    }
}

// ===================================================================
//  WiFiManager  –  portal with MQTT custom parameters
// ===================================================================

// Custom parameters allocated in setupWiFi, persist for non-blocking portal
static WiFiManagerParameter* pMqttHeader;
static WiFiManagerParameter* pMqttEnabled;
static WiFiManagerParameter* pMqttHost;
static WiFiManagerParameter* pMqttPort;
static WiFiManagerParameter* pMqttUser;
static WiFiManagerParameter* pMqttPass;
static WiFiManagerParameter* pHaDiscovery;
static WiFiManagerParameter* pBrightnessHeader;
static WiFiManagerParameter* pBrightness;

void saveParamsCallback() {
    config.mqttEnabled = (strncmp(pMqttEnabled->getValue(), "T", 1) == 0);
    strlcpy(config.mqttHost, pMqttHost->getValue(), sizeof(config.mqttHost));
    config.mqttPort = atoi(pMqttPort->getValue());
    if (config.mqttPort == 0) config.mqttPort = 1883;
    strlcpy(config.mqttUser, pMqttUser->getValue(), sizeof(config.mqttUser));
    strlcpy(config.mqttPass, pMqttPass->getValue(), sizeof(config.mqttPass));
    config.haDiscovery = (strncmp(pHaDiscovery->getValue(), "T", 1) == 0);
    int bri = atoi(pBrightness->getValue());
    config.brightness = (bri < 0) ? 0 : (bri > 255) ? 255 : (uint8_t)bri;
    brightness = min(brightness, config.brightness);
    saveConfig();
    Serial.println("Config saved");
}

void setupWiFi() {
    static char portStr[6];
    snprintf(portStr, sizeof(portStr), "%u", config.mqttPort);

    pMqttHeader  = new WiFiManagerParameter("<h3>MQTT Settings</h3>");
    pMqttEnabled = new WiFiManagerParameter(
        "mqtt_enabled", "Enable MQTT", "T", 2,
        config.mqttEnabled ? "type=\"checkbox\" checked" : "type=\"checkbox\"",
        WFM_LABEL_AFTER);
    pMqttHost = new WiFiManagerParameter("mqtt_host", "<br>MQTT Host",
                                          config.mqttHost, sizeof(config.mqttHost));
    pMqttPort = new WiFiManagerParameter("mqtt_port", "MQTT Port",
                                          portStr, sizeof(portStr));
    pMqttUser = new WiFiManagerParameter("mqtt_user", "MQTT Username",
                                          config.mqttUser, sizeof(config.mqttUser));
    pMqttPass = new WiFiManagerParameter("mqtt_pass", "MQTT Password",
                                          config.mqttPass, sizeof(config.mqttPass));
    pHaDiscovery = new WiFiManagerParameter(
        "ha_discovery", "Use HA Discovery", "T", 2,
        config.haDiscovery ? "type=\"checkbox\" checked" : "type=\"checkbox\"",
        WFM_LABEL_AFTER);

    static char briStr[4];
    snprintf(briStr, sizeof(briStr), "%u", config.brightness);
    pBrightnessHeader = new WiFiManagerParameter("<hr><h3>LED Settings</h3>");
    pBrightness = new WiFiManagerParameter("brightness", "Max Brightness (0-255)",
                                            briStr, 4);

    wm.setSaveParamsCallback(saveParamsCallback);
    wm.setConfigPortalBlocking(false);

    wm.addParameter(pMqttHeader);
    wm.addParameter(pMqttEnabled);
    wm.addParameter(pMqttHost);
    wm.addParameter(pMqttPort);
    wm.addParameter(pMqttUser);
    wm.addParameter(pMqttPass);
    wm.addParameter(pHaDiscovery);
    wm.addParameter(pBrightnessHeader);
    wm.addParameter(pBrightness);

    // Connect to WiFi (non-blocking), then start the web portal on
    // the STA interface so it stays reachable at the device IP.
    wm.autoConnect(WIFI_AP_NAME);
    wm.startWebPortal();
}

// ===================================================================
//  Arduino entry points
// ===================================================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== Moon Phase ===");

    Serial.println("[setup] LED init...");
    strip.begin();
    strip.clear();
    strip.show();

    Serial.println("[setup] LittleFS...");
#ifdef ESP8266
    LittleFS.begin();
#else
    if (!LittleFS.begin(true)) {
        Serial.println("[setup] LittleFS mount FAILED");
    }
#endif

    Serial.println("[setup] loadConfig...");
    loadConfig();
    brightness = config.brightness;

    Serial.println("[setup] buildTopics...");
    buildTopics();

    String hostname = "moonphase-" + chipId;
#ifdef ESP8266
    WiFi.hostname(hostname);
#else
    WiFi.setHostname(hostname.c_str());
#endif

    Serial.println("[setup] setupWiFi...");
    setupWiFi();
    Serial.println("[setup] setupWiFi done");
}

void loop() {
    wm.process();
    unsigned long now = millis();

    // Heartbeat every 10 seconds
    static unsigned long lastHeartbeat = 0;
    if (now - lastHeartbeat >= 10000UL) {
        lastHeartbeat = now;
        Serial.printf("[loop] alive, heap=%u, wifi=%d, state=%d\n",
                      ESP.getFreeHeap(), WiFi.status(), startupState);
    }

    // -- Startup state machine ------------------------------------------
    if (startupState == STARTUP_WIFI) {
        pulseLEDs(0, 200, 0);   // Cool White pulse
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("[startup] WiFi connected, starting NTP...");
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            startupState = STARTUP_NTP;
        }
        return;
    }

    if (startupState == STARTUP_NTP) {
        pulseLEDs(0, 0, 200);   // Amber pulse
        time_t t;
        time(&t);
        if (t > 1000000000L) {
            Serial.println("[startup] NTP synced, starting normal operation");
            // Init MQTT now that we have network + time
            if (config.mqttEnabled && strlen(config.mqttHost) > 0) {
                mqttClient.setServer(config.mqttHost, config.mqttPort);
                mqttClient.setCallback(mqttCallback);
                mqttClient.setBufferSize(1024);
                Serial.printf("[startup] MQTT enabled -> %s:%u\n",
                              config.mqttHost, config.mqttPort);
            }
            updateLEDs();
            startupState = STARTUP_READY;
        }
        return;
    }

    // -- Normal operation -----------------------------------------------

    // MQTT housekeeping
    if (config.mqttEnabled && strlen(config.mqttHost) > 0) {
        if (!mqttClient.connected()) {
            mqttReconnect();
        }
        mqttClient.loop();
    }

    // Recalculate moon phase every 60 seconds
    if (now - lastMoonUpdate >= 60000UL) {
        lastMoonUpdate = now;
        double prevIllum = currentIllumination;
        updateLEDs();

        // Publish updated state if illumination changed
        if (config.mqttEnabled && mqttClient.connected()) {
            if (fabs(currentIllumination - prevIllum) > 0.005) {
                publishState();
            }
        }
    }
}
