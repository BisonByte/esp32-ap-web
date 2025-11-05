// ESP32 Access Point + HTTP client example for Laravel backend
// VERSIÓN CORREGIDA - Lógica de relé invertida para 5V

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <math.h>

#include "DefaultBackendConfig.h"

// ========== CONFIGURACIÓN ==========
#ifndef AP_SSID
#define AP_SSID "BisonByte-Setup"
#endif
#ifndef AP_PASS
#define AP_PASS "12345678"
#endif
#ifndef RELAY_PIN
#define RELAY_PIN 26  // GPIO26 - relé
#endif

#ifndef RELAY_ACTIVE_HIGH
// Módulo de relé alimentado con 5V (VIN) - Lógica ACTIVO-BAJO
// 0 = activo en BAJO (IN a LOW enciende el relé) ← CONFIGURACIÓN PARA RELÉ ESTÁNDAR
// 1 = activo en ALTO (IN a HIGH enciende el relé)
#define RELAY_ACTIVE_HIGH 0 // Configurado para relé activo-bajo
#endif

// ⚠️ CAMBIO IMPORTANTE: Desactivado por defecto para que el relé funcione
#ifndef FORCE_RELAY_OFF
#define FORCE_RELAY_OFF 0  // Cambiado de 1 a 0
#endif

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
#ifndef DEFAULT_SERVER_URL
#define DEFAULT_SERVER_URL "https://www.google.com"
#endif
#ifndef DEFAULT_HTTP_ACTIVATION_ENDPOINT
#define DEFAULT_HTTP_ACTIVATION_ENDPOINT ""
#endif
#ifndef DEFAULT_HTTP_STATE_ENDPOINT
#define DEFAULT_HTTP_STATE_ENDPOINT ""
#endif
#ifndef DEFAULT_HTTP_SET_ENDPOINT
#define DEFAULT_HTTP_SET_ENDPOINT ""
#endif
#ifndef DEFAULT_HTTP_TELEMETRY_ENDPOINT
#define DEFAULT_HTTP_TELEMETRY_ENDPOINT ""
#endif
#ifndef DEFAULT_ACTIVATION_KEY
#define DEFAULT_ACTIVATION_KEY ""
#endif
#ifndef DEFAULT_DEVICE_ID
#define DEFAULT_DEVICE_ID 0
#endif
#ifndef DEFAULT_HTTP_POLL_SECONDS
#define DEFAULT_HTTP_POLL_SECONDS 0
#endif
// ===================================

namespace {
  constexpr unsigned long WIFI_TIMEOUT = 20000;
  constexpr unsigned long CHECK_INTERVAL = 2000;
  constexpr unsigned long TELEMETRY_INTERVAL = 15000;
  constexpr size_t SMALL_JSON_CAPACITY = 512;
  constexpr size_t TELEMETRY_JSON_CAPACITY = 768;

  Preferences prefs;
  bool relayActiveHighRuntime = RELAY_ACTIVE_HIGH;
  WebServer server(80);
  WiFiClient plainClient;
  WiFiClientSecure secureClient;

  String wifiSsid;
  String wifiPass;
  String serverUrl;
  String deviceToken;
  String deviceName = "Bomba ESP32";
  uint32_t deviceId = 0;

  String stateEndpointOverride;
  String telemetryEndpointOverride;
  unsigned long checkIntervalMs = CHECK_INTERVAL;

  bool apMode = false;
  bool reconnectRequested = false;
  unsigned long reconnectRequestAt = 0;
  unsigned long lastCheck = 0;
  unsigned long lastTelemetry = 0;
  bool apPersist = false;
}

static String joinUrl(const String& base, const String& path) {
  if (path.startsWith("http://") || path.startsWith("https://")) return path;
  if (base.endsWith("/") && path.startsWith("/")) return base + path.substring(1);
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
}

static void httpBegin(HTTPClient& http, const String& url) {
  if (url.startsWith("https://")) {
    http.begin(secureClient, url);
  } else {
    http.begin(plainClient, url);
  }
}

static inline bool relayIsOn() {
  return relayActiveHighRuntime ? (digitalRead(RELAY_PIN) == HIGH)
                                 : (digitalRead(RELAY_PIN) == LOW);
}

static void setRelay(bool enabled) {
#if FORCE_RELAY_OFF
  Serial.println("⚠️ FORCE_RELAY_OFF está activo - relé bloqueado");
  enabled = false;
#endif
  
  if (relayActiveHighRuntime) {
    digitalWrite(RELAY_PIN, enabled ? HIGH : LOW);
  } else {
    digitalWrite(RELAY_PIN, enabled ? LOW : HIGH);
  }
  
  Serial.print("🔌 Relé ajustado a: ");
  Serial.println(enabled ? "APAGADO" : "ENCENDIDO");
}

static bool parseBooleanLike(const JsonVariantConst& value, bool fallback = false) {
  if (value.isNull()) return fallback;
  if (value.is<bool>()) return value.as<bool>();
  if (value.is<int>()) return value.as<int>() != 0;
  if (value.is<long>()) return value.as<long>() != 0;
  if (value.is<unsigned int>()) return value.as<unsigned int>() != 0;
  if (value.is<unsigned long>()) return value.as<unsigned long>() != 0;
  if (value.is<float>() || value.is<double>()) return fabs(value.as<double>()) > 0.000001;
  if (value.is<const char*>()) {
    String s = value.as<const char*>();
    s.trim();
    s.toLowerCase();
    if (s == "1" || s == "true" || s == "on" || s == "encendido") return true;
    if (s == "0" || s == "false" || s == "off" || s == "apagado") return false;
  }
  return fallback;
}

static void loadPreferences() {
  prefs.begin("bisonbyte", false);
  wifiSsid = prefs.getString("wifi_ssid", "");
  wifiPass = prefs.getString("wifi_pass", "");
  serverUrl = prefs.getString("server_url", "");
  deviceToken = prefs.getString("device_token", "");
  deviceId = prefs.getUInt("device_id", 0);
  stateEndpointOverride = prefs.getString("state_url", "");
  telemetryEndpointOverride = prefs.getString("telemetry_url", "");
  checkIntervalMs = prefs.getUInt("check_ms", CHECK_INTERVAL);
  relayActiveHighRuntime = prefs.getBool("relay_ah", RELAY_ACTIVE_HIGH);
  apPersist = prefs.getBool("ap_persist", false);

  // Asegurarse de que el valor de NVS coincida con el #define si es la primera vez
  if (!prefs.isKey("relay_ah")) {
      prefs.putBool("relay_ah", RELAY_ACTIVE_HIGH);
      relayActiveHighRuntime = RELAY_ACTIVE_HIGH;
  }


  bool changed = false;
  if (!wifiSsid.length() && String(DEFAULT_WIFI_SSID).length()) {
    wifiSsid = DEFAULT_WIFI_SSID;
    prefs.putString("wifi_ssid", wifiSsid);
    changed = true;
  }
  if (!wifiPass.length() && String(DEFAULT_WIFI_PASS).length()) {
    wifiPass = DEFAULT_WIFI_PASS;
    prefs.putString("wifi_pass", wifiPass);
    changed = true;
  }
  if (!serverUrl.length() && String(DEFAULT_SERVER_URL).length()) {
    serverUrl = DEFAULT_SERVER_URL;
    prefs.putString("server_url", serverUrl);
    changed = true;
  }
  if (deviceId == 0 && DEFAULT_DEVICE_ID > 0) {
    deviceId = DEFAULT_DEVICE_ID;
    prefs.putUInt("device_id", deviceId);
    changed = true;
  }
  if (!deviceToken.length() && String(DEFAULT_ACTIVATION_KEY).length()) {
    deviceToken = DEFAULT_ACTIVATION_KEY;
    prefs.putString("device_token", deviceToken);
    changed = true;
  }
  if (!stateEndpointOverride.length() && String(DEFAULT_HTTP_STATE_ENDPOINT).length()) {
    stateEndpointOverride = DEFAULT_HTTP_STATE_ENDPOINT;
    prefs.putString("state_url", stateEndpointOverride);
    changed = true;
  }
  if (!telemetryEndpointOverride.length() && String(DEFAULT_HTTP_TELEMETRY_ENDPOINT).length()) {
    telemetryEndpointOverride = DEFAULT_HTTP_TELEMETRY_ENDPOINT;
    prefs.putString("telemetry_url", telemetryEndpointOverride);
    changed = true;
  }
  if (!prefs.isKey("check_ms") && DEFAULT_HTTP_POLL_SECONDS > 0) {
    checkIntervalMs = DEFAULT_HTTP_POLL_SECONDS * 1000UL;
    prefs.putUInt("check_ms", checkIntervalMs);
    changed = true;
  }
  
#ifdef OVERRIDE_NVS_WITH_DEFAULTS
  bool forced = false;
  if (String(DEFAULT_WIFI_SSID).length() && wifiSsid != String(DEFAULT_WIFI_SSID)) {
    wifiSsid = DEFAULT_WIFI_SSID;
    prefs.putString("wifi_ssid", wifiSsid);
    forced = true;
  }
  if (String(DEFAULT_WIFI_PASS).length() && wifiPass != String(DEFAULT_WIFI_PASS)) {
    wifiPass = DEFAULT_WIFI_PASS;
    prefs.putString("wifi_pass", wifiPass);
    forced = true;
  }
  if (String(DEFAULT_SERVER_URL).length() && serverUrl != String(DEFAULT_SERVER_URL)) {
    serverUrl = DEFAULT_SERVER_URL;
    prefs.putString("server_url", serverUrl);
    forced = true;
  }
  if (String(DEFAULT_HTTP_STATE_ENDPOINT).length() && stateEndpointOverride != String(DEFAULT_HTTP_STATE_ENDPOINT)) {
    stateEndpointOverride = DEFAULT_HTTP_STATE_ENDPOINT;
    prefs.putString("state_url", stateEndpointOverride);
    forced = true;
  }
  if (String(DEFAULT_HTTP_TELEMETRY_ENDPOINT).length() && telemetryEndpointOverride != String(DEFAULT_HTTP_TELEMETRY_ENDPOINT)) {
    telemetryEndpointOverride = DEFAULT_HTTP_TELEMETRY_ENDPOINT;
    prefs.putString("telemetry_url", telemetryEndpointOverride);
    forced = true;
  }
  if (DEFAULT_HTTP_POLL_SECONDS > 0 && checkIntervalMs != DEFAULT_HTTP_POLL_SECONDS * 1000UL) {
    checkIntervalMs = DEFAULT_HTTP_POLL_SECONDS * 1000UL;
    prefs.putUInt("check_ms", checkIntervalMs);
    forced = true;
  }
  if (forced) {
    prefs.remove("device_token");
    prefs.putUInt("device_id", 0);
    deviceToken = "";
    deviceId = 0;
    Serial.println("Preferencias forzadas desde build (OVERRIDE_NVS_WITH_DEFAULTS).");
  }
#endif
  
  if (changed) {
    Serial.println("Preferencias iniciales aplicadas desde valores por defecto.");
  }
}

static void saveWiFiConfig(const String& ssid, const String& pass, const String& url) {
  wifiSsid = ssid;
  wifiPass = pass;
  serverUrl = url.length() ? url : serverUrl;

  prefs.putString("wifi_ssid", wifiSsid);
  prefs.putString("wifi_pass", wifiPass);
  prefs.putString("server_url", serverUrl);

  prefs.remove("device_token");
  prefs.putUInt("device_id", 0);
  deviceToken = "";
  deviceId = 0;
}

static void saveDeviceState() {
  prefs.putString("device_token", deviceToken);
  prefs.putUInt("device_id", deviceId);
}

static void saveServerDirectives(const String& stateUrl, const String& telemetryUrl, unsigned long pollSeconds) {
  if (stateUrl.length()) {
    stateEndpointOverride = stateUrl;
    prefs.putString("state_url", stateEndpointOverride);
  }
  if (telemetryUrl.length()) {
    telemetryEndpointOverride = telemetryUrl;
    prefs.putString("telemetry_url", telemetryEndpointOverride);
  }
  if (pollSeconds > 0) {
    checkIntervalMs = pollSeconds * 1000UL;
    prefs.putUInt("check_ms", checkIntervalMs);
  }
}

static void startAccessPoint() {
  if (apMode) return;

  WiFi.mode(WIFI_AP_STA);
  if (WiFi.softAP(AP_SSID, AP_PASS)) {
    apMode = true;
    IPAddress ip = WiFi.softAPIP();
    Serial.println("========================================");
    Serial.println("Modo configuración activo");
    Serial.print("Conéctate a la red: ");
    Serial.println(AP_SSID);
    Serial.print("Contraseña: ");
    Serial.println(AP_PASS[0] ? AP_PASS : "(sin contraseña)");
    Serial.print("Luego ve a: http://");
    Serial.println(ip);
    Serial.println("========================================");
  } else {
    Serial.println("✗ No se pudo iniciar el punto de acceso");
  }
}

static void stopAccessPoint() {
  if (!apMode) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apMode = false;
  Serial.println("Punto de acceso encendido (conexión WiFi activa)");
}

static bool connectToWiFi() {
  if (!wifiSsid.length()) {
    Serial.println("No hay SSID configurado, permanece el modo AP");
    return false;
  }

  Serial.printf("Conectando a WiFi \"%s\"...\n", wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✓ WiFi conectado! IP: ");
    Serial.println(WiFi.localIP());
    if (apPersist) {
      startAccessPoint();
    } else {
      stopAccessPoint();
    }
    return true;
  }

  Serial.println("✗ No se pudo conectar, manteniendo modo AP");
  startAccessPoint();
  return false;
}

static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  connectToWiFi();
}

static void ensureAccessPoint() {
  if (WiFi.status() == WL_CONNECTED) {
    if (apPersist) {
      startAccessPoint();
    } else {
      stopAccessPoint();
    }
  } else {
    startAccessPoint();
  }
}

static void registerDevice() {
  if (!serverUrl.length() || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  const String url = String(DEFAULT_HTTP_ACTIVATION_ENDPOINT).length()
                       ? DEFAULT_HTTP_ACTIVATION_ENDPOINT
                       : joinUrl(serverUrl, "/api/devices/register");

  DynamicJsonDocument doc(SMALL_JSON_CAPACITY);
  const String mac = WiFi.macAddress();
  doc["mac"] = mac;
  doc["name"] = deviceName;
  doc["connection_type"] = "http";

  String payload;
  serializeJson(doc, payload);

  Serial.println("Registrando dispositivo...");
  httpBegin(http, url);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.POST(payload);
  const String response = http.getString();

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("✓ Dispositivo registrado!");
    Serial.println(response);

    DynamicJsonDocument res(1024);
    DeserializationError err = deserializeJson(res, response);
    if (!err) {
      deviceToken = res["token"].as<String>();
      deviceId = res["device_id"] | 0;
      saveDeviceState();

      Serial.print("Device ID: ");
      Serial.println(deviceId);
      Serial.print("Token: ");
      Serial.println(deviceToken);

      if (res.containsKey("http")) {
        JsonObject httpCfg = res["http"].as<JsonObject>();
        const String stateUrl = httpCfg["state"].as<String>();
        const String telemetryUrl = httpCfg["telemetry"].as<String>();
        const unsigned long pollSeconds = httpCfg["poll_seconds"] | (checkIntervalMs / 1000UL);
        saveServerDirectives(stateUrl, telemetryUrl, pollSeconds);
        Serial.print("HTTP state endpoint: "); 
        Serial.println(stateEndpointOverride.length() ? stateEndpointOverride : "(default)");
        Serial.print("HTTP telemetry endpoint: "); 
        Serial.println(telemetryEndpointOverride.length() ? telemetryEndpointOverride : "(default)");
        Serial.print("Poll interval (ms): "); 
        Serial.println(checkIntervalMs);
      }
    } else {
      Serial.print("✗ Error parseando respuesta de registro: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.print("✗ Error al registrar: ");
    Serial.print(httpCode);
    Serial.print(" -> ");
    Serial.println(response);
  }

  http.end();
}

static void checkPumpState() {
  if (deviceId == 0 || WiFi.status() != WL_CONNECTED || !serverUrl.length()) return;

  HTTPClient http;
  auto addQuery = [](const String& base, const String& key, const String& value) -> String {
    return base + (base.indexOf('?') >= 0 ? '&' : '?') + key + "=" + value;
  };

  const String stateDefault = String(DEFAULT_HTTP_STATE_ENDPOINT).length()
                                    ? DEFAULT_HTTP_STATE_ENDPOINT
                                    : joinUrl(serverUrl, "/api/pump/state");
  String baseUrl = stateEndpointOverride.length() ? stateEndpointOverride : stateDefault;
  String url = addQuery(baseUrl, "device_id", String(deviceId));

  httpBegin(http, url);
  if (deviceToken.length()) {
    http.addHeader("Authorization", String("Bearer ") + deviceToken);
  }

  const int httpCode = http.GET();
  const String response = http.getString();

  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(SMALL_JSON_CAPACITY);
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      const bool shouldRun = parseBooleanLike(doc["should_run"], false);
      setRelay(shouldRun);
      Serial.print("📡 Estado bomba del servidor: ");
      Serial.println(shouldRun ? "APAGADA" : "ENCENDIDA");
    } else {
      Serial.print("✗ Error parseando estado: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.print("✗ Error estado (HTTP ");
    Serial.print(httpCode);
    Serial.print("): ");
    Serial.println(response);
  }

  http.end();
}

static void sendTelemetry() {
  if (deviceId == 0 || WiFi.status() != WL_CONNECTED || !serverUrl.length()) return;

  HTTPClient http;
  const String telemetryDefault = String(DEFAULT_HTTP_TELEMETRY_ENDPOINT).length()
                                        ? DEFAULT_HTTP_TELEMETRY_ENDPOINT
                                        : joinUrl(serverUrl, "/api/telemetry");
  const String url = telemetryEndpointOverride.length() ? telemetryEndpointOverride : telemetryDefault;

  DynamicJsonDocument doc(TELEMETRY_JSON_CAPACITY);
  doc["device_id"] = deviceId;
  JsonObject telemetry = doc["telemetry"].to<JsonObject>();
  telemetry["voltage"] = 220.0;
  telemetry["current"] = 3.5;
  telemetry["is_on"] = relayIsOn();

  String payload;
  serializeJson(doc, payload);

  httpBegin(http, url);
  http.addHeader("Content-Type", "application/json");
  if (deviceToken.length()) {
    http.addHeader("Authorization", String("Bearer ") + deviceToken);
  }

  const int httpCode = http.POST(payload);
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_ACCEPTED) {
    Serial.println("✓ Telemetría enviada");
  } else {
    Serial.print("✗ Error telemetría (HTTP ");
    Serial.print(httpCode);
    Serial.println(")");
  }

  http.end();
}

static String htmlEscape(const String& value) {
  String out;
  out.reserve(value.length() * 2);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

static String renderRootPage() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const IPAddress ip = wifiConnected ? WiFi.localIP() : WiFi.softAPIP();
  const String stateUrlDisplay = stateEndpointOverride.length()
                                   ? stateEndpointOverride
                                   : (String(DEFAULT_HTTP_STATE_ENDPOINT).length()
                                        ? DEFAULT_HTTP_STATE_ENDPOINT
                                        : joinUrl(serverUrl, "/api/pump/state"));
  const String telemetryUrlDisplay = telemetryEndpointOverride.length()
                                       ? telemetryEndpointOverride
                                       : (String(DEFAULT_HTTP_TELEMETRY_ENDPOINT).length()
                                            ? DEFAULT_HTTP_TELEMETRY_ENDPOINT
                                            : joinUrl(serverUrl, "/api/telemetry"));

  String html;
  html.reserve(3072);
  html += "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>BisonByte Setup</title><style>body{font-family:sans-serif;margin:2rem;background:#f7f7f7;}";
  html += "main{max-width:720px;margin:0 auto;background:#fff;padding:2rem;border-radius:1rem;box-shadow:0 1rem 2rem rgba(0,0,0,0.1);}";
  html += "label{display:block;margin-top:1rem;font-weight:600;}";
  html += "input{width:100%;padding:.75rem;margin-top:.5rem;border:1px solid #ddd;border-radius:.5rem;box-sizing:border-box;}";
  html += "button,.btn{margin-top:1.5rem;padding:.75rem 1.5rem;border:none;border-radius:.5rem;background:#2563eb;color:#fff;font-size:1rem;cursor:pointer;text-decoration:none;display:inline-block;}";
  html += "button:hover,.btn:hover{background:#1d4ed8;}";
  html += ".btn-danger{background:#dc2626;}.btn-danger:hover{background:#b91c1c;}";
  html += ".btn-success{background:#16a34a;}.btn-success:hover{background:#15803d;}";
  html += "section{margin-top:2rem;}.status{padding:1rem;background:#e0f2fe;border-radius:.75rem;}";
  html += "code{background:#e2e8f0;padding:.25rem .5rem;border-radius:.5rem;}";
  html += ".relay-status{font-size:1.5rem;font-weight:bold;padding:1rem;border-radius:.5rem;text-align:center;}";
  html += ".relay-on{background:#dcfce7;color:#16a34a;}.relay-off{background:#fee2e2;color:#dc2626;}";
  html += ".controls{display:flex;gap:1rem;margin-top:1rem;flex-wrap:wrap;}";
  html += "</style></head><body><main>";
  html += "<h1>🔧 BisonByte Setup</h1>";
  
  html += "<div class='status'><p><strong>Estado WiFi:</strong> ";
  html += wifiConnected ? "✅ Conectado" : "❌ No conectado";
  html += "</p><p><strong>IP actual:</strong> ";
  html += ip.toString();
  html += "</p><p><strong>Servidor:</strong> <code>";
  html += htmlEscape(serverUrl);
  html += "</code></p><p><strong>AP activo:</strong> ";
  html += apMode ? "✅ Sí" : "❌ No";
  html += " (persistencia: ";
  html += apPersist ? "ON" : "OFF";
  html += ")</p></div>";

  html += "<section><h2>🔌 Control del Relé</h2>";
  html += "<div class='relay-status ";
  html += relayIsOn() ? "relay-on" : "relay-off";
  html += "'>Estado: ";
  html += relayIsOn() ? "🟢 ENCENDIDO" : "🔴 APAGADO";
  html += "</div><div class='controls'>";
  html += "<a href='/relay?on=1&redirect=1' class='btn btn-success'>Apagado</a>";
  html += "<a href='/relay?on=0&redirect=1' class='btn btn-danger'>Encender</a>";
  html += "<a href='/relay?toggle=1&redirect=1' class='btn'>Toggle</a>";
  html += "</div></section>";

  html += "<section><h2>⚙️ Configurar WiFi y Servidor</h2><form method='POST' action='/configure'>";
  html += "<label for='ssid'>WiFi SSID</label><input id='ssid' name='ssid' required value='";
  html += htmlEscape(wifiSsid.isEmpty() ? DEFAULT_WIFI_SSID : wifiSsid);
  html += "'>";
  html += "<label for='pass'>WiFi Password</label><input id='pass' name='pass' type='password' value='";
  html += htmlEscape(wifiPass);
  html += "'>";
  html += "<label for='server'>URL del servidor Laravel</label><input id='server' name='server' required value='";
  html += htmlEscape(serverUrl.isEmpty() ? DEFAULT_SERVER_URL : serverUrl);
  html += "'>";
  html += "<button type='submit'>💾 Guardar y conectar</button></form></section>";

  html += "<section><h2>📱 Información del dispositivo</h2><ul>";
  html += "<li><strong>MAC:</strong> ";
  html += WiFi.macAddress();
  html += "</li><li><strong>Device ID:</strong> ";
  html += String(deviceId);
  html += "</li><li><strong>Token:</strong> ";
  html += htmlEscape(deviceToken.length() > 0 ? deviceToken.substring(0, 20) + "..." : "(no registrado)");
  html += "</li><li><strong>Estado endpoint:</strong> <code>";
  html += htmlEscape(stateUrlDisplay);
  html += "</code></li><li><strong>Telemetría endpoint:</strong> <code>";
  html += htmlEscape(telemetryUrlDisplay);
  html += "</code></li><li><strong>Poll (ms):</strong> ";
  html += String(checkIntervalMs);
  html += "</li><li><strong>Lógica relé:</strong> ";
  html += relayActiveHighRuntime ? "Activo-ALTO" : "Activo-BAJO";
  html += "</li></ul></section>";

  html += "<script>(function(){try{var e=document.querySelector('.relay-status');if(e){var t=e.textContent;e.textContent=t.replace('ENCENDIDO','__X__').replace('APAGADO','ENCENDIDO').replace('__X__','APAGADO');}}catch(_){}})();</script>";
  html += "</main></body></html>";
  return html;
}

static void handleRoot() {
  server.send(200, "text/html; charset=utf-8", renderRootPage());
}

static void handleStatus() {
  DynamicJsonDocument doc(SMALL_JSON_CAPACITY);
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["ip"] = (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString();
  doc["server_url"] = serverUrl;
  doc["device_id"] = deviceId;
  doc["relay_on"] = relayIsOn();
  doc["relay_active_high"] = relayActiveHighRuntime;
  doc["ap_active"] = apMode;
  doc["ap_persist"] = apPersist;
  doc["force_relay_off"] = FORCE_RELAY_OFF;

  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void handleRelay() {
  bool changed = false;
  if (server.hasArg("toggle")) {
    setRelay(!relayIsOn());
    changed = true;
  } else if (server.hasArg("on")) {
    const String v = server.arg("on");
    if (v == "1" || v.equalsIgnoreCase("true")) {
      setRelay(true);
      changed = true;
    } else if (v == "0" || v.equalsIgnoreCase("false")) {
      setRelay(false);
      changed = true;
    }
  } else if (server.hasArg("active_high")) {
    const String v = server.arg("active_high");
    const bool newVal = (v == "1" || v.equalsIgnoreCase("true"));
    relayActiveHighRuntime = newVal;
    prefs.putBool("relay_ah", relayActiveHighRuntime);
    setRelay(relayIsOn()); // Re-aplica el estado actual con la nueva lógica
    changed = true;
  }

  if (server.hasArg("redirect")) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }

  DynamicJsonDocument doc(SMALL_JSON_CAPACITY);
  doc["ok"] = changed;
  doc["relay_on"] = relayIsOn();
  doc["relay_active_high"] = relayActiveHighRuntime;
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static bool hasArgAndNotEmpty(const String& arg) {
  return server.hasArg(arg) && server.arg(arg).length();
}

static void handleConfigure() {
  if (!hasArgAndNotEmpty("ssid") || !hasArgAndNotEmpty("server")) {
    server.send(400, "text/plain", "Faltan campos obligatorios (ssid/server)");
    return;
  }

  const String ssid = server.arg("ssid");
  const String pass = server.hasArg("pass") ? server.arg("pass") : "";
  const String url = server.arg("server");

  saveWiFiConfig(ssid, pass, url);
  reconnectRequested = true;
  reconnectRequestAt = millis();

  String body = "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>";
  body += "<meta http-equiv='refresh' content='5;url=/'>";
  body += "<title>Configuración guardada</title></head><body>";
  body += "<p>✅ Configuración guardada. Intentando conectar a <strong>";
  body += htmlEscape(ssid);
  body += "</strong>...</p><p>Serás redirigido en 5 segundos o haz clic <a href='/'>aquí</a>.</p>";
  body += "</body></html>";

  server.send(200, "text/html; charset=utf-8", body);
}

static void handleNotFound() {
  server.send(404, "text/plain", "No encontrado");
}

static void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/relay", HTTP_GET, handleRelay);
  
  server.on("/ap", HTTP_GET, [](){
    bool changed = false;
    if (server.hasArg("on")) {
      const String v = server.arg("on");
      const bool wantOn = (v == "1" || v.equalsIgnoreCase("true"));
      apPersist = wantOn;
      prefs.putBool("ap_persist", apPersist);
      if (apPersist) startAccessPoint(); 
      else stopAccessPoint();
      changed = true;
    }
    if (server.hasArg("redirect")) { 
      server.sendHeader("Location", "/"); 
      server.send(302); 
      return; 
    }
    DynamicJsonDocument doc(SMALL_JSON_CAPACITY);
    doc["ok"] = changed; 
    doc["ap_active"] = apMode; 
    doc["ap_persist"] = apPersist;
    String body; 
    serializeJson(doc, body); 
    server.send(200, "application/json", body);
  });
  
  server.on("/configure", HTTP_POST, handleConfigure);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("✓ Servidor web iniciado");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  
  pinMode(RELAY_PIN, OUTPUT);
  
  // Para relé activo-BAJO, el estado inicial APAGADO es HIGH.
  digitalWrite(RELAY_PIN, HIGH);
  
  delay(100);
  
  Serial.println("\n\n========================================");
  Serial.println("🚀 BisonByte ESP32 - Iniciando...");
  Serial.println("🔌 Configuración: Relé con VCC=3.3V (3v3)");
  Serial.print("🔌 Pin GPIO: ");
  Serial.println(RELAY_PIN);
  Serial.println("🔌 Lógica: ACTIVO-BAJO (LOW=encendido)");
  Serial.println("========================================");
  
  secureClient.setInsecure();

  loadPreferences();

  setRelay(false); // Asegura que el estado inicial sea APAGADO
  
  Serial.print("🔌 Estado inicial del relé: ");
  Serial.println(relayIsOn() ? "ENCENDIDO ❌" : "APAGADO ✅");
  Serial.println();
  
  Serial.print("🔌 Pin del relé: GPIO");
  Serial.println(RELAY_PIN);
  Serial.print("🔌 Lógica del relé: ");
  Serial.println(relayActiveHighRuntime ? "Activo-ALTO" : "Activo-BAJO");
  
#if FORCE_RELAY_OFF
  Serial.println("⚠️ ADVERTENCIA: FORCE_RELAY_OFF está ACTIVO");
  Serial.println("⚠️ El relé permanecerá bloqueado en OFF");
  Serial.println("⚠️ Para habilitar el control, cambia FORCE_RELAY_OFF a 0");
#endif

  if (!connectToWiFi()) {
    startAccessPoint();
  }

  setupServer();
  
  Serial.println("========================================");
  Serial.println("✓ Sistema listo");
  Serial.println("========================================\n");
}

void loop() {
  server.handleClient();

  if (reconnectRequested && millis() - reconnectRequestAt > 1000) {
    reconnectRequested = false;
    if (!connectToWiFi()) {
      startAccessPoint();
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    ensureAccessPoint();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (deviceId == 0 && (millis() - lastCheck) > checkIntervalMs) {
      lastCheck = millis();
      registerDevice();
    } else if (deviceId != 0) {
      const unsigned long now = millis();
      if (now - lastCheck > checkIntervalMs) {
        lastCheck = now;
        checkPumpState();
      }
      if (now - lastTelemetry > TELEMETRY_INTERVAL) {
        lastTelemetry = now;
        sendTelemetry();
      }
    }
  }
}
