// ESP32 Access Point + HTTP client example for Laravel backend

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ========== CONFIGURACIÓN ==========
#define AP_SSID "BisonByte-Setup"   // ✅ Cambia el nombre si quieres
#define AP_PASS "12345678"           // ✅ Cambia la contraseña si deseas
#define RELAY_PIN 2                   // ⚠️ Pon aquí el pin de tu relé
#define LED_PIN 4                     // ⚠️ Pon aquí el pin de tu LED indicador
// ===================================

namespace {
  constexpr unsigned long WIFI_TIMEOUT = 20000;   // 20 segundos para conectar
  constexpr unsigned long CHECK_INTERVAL = 2000;  // Consultar estado cada 2 s
  constexpr unsigned long TELEMETRY_INTERVAL = 15000; // Telemetría cada 15 s
  constexpr size_t SMALL_JSON_CAPACITY = 512;
  constexpr size_t TELEMETRY_JSON_CAPACITY = 768;

  Preferences prefs;
  WebServer server(80);
  WiFiClient wifiClient;

  String wifiSsid;
  String wifiPass;
  String serverUrl;
  String deviceToken;
  String deviceName = "Bomba ESP32";
  uint32_t deviceId = 0;

  bool apMode = false;
  bool reconnectRequested = false;
  unsigned long reconnectRequestAt = 0;
  unsigned long lastCheck = 0;
  unsigned long lastTelemetry = 0;
}

static String joinUrl(const String& base, const String& path) {
  if (path.startsWith("http://") || path.startsWith("https://")) return path;
  if (base.endsWith("/") && path.startsWith("/")) return base + path.substring(1);
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
}

static void setRelay(bool enabled) {
  digitalWrite(RELAY_PIN, enabled ? HIGH : LOW);
  digitalWrite(LED_PIN, enabled ? HIGH : LOW);
}

static void loadPreferences() {
  prefs.begin("bisonbyte", false);
  wifiSsid = prefs.getString("wifi_ssid", "");
  wifiPass = prefs.getString("wifi_pass", "");
  serverUrl = prefs.getString("server_url", "http://192.168.1.100:8000");
  deviceToken = prefs.getString("device_token", "");
  deviceId = prefs.getUInt("device_id", 0);
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
  Serial.println("Punto de acceso apagado (conexión WiFi activa)");
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
    stopAccessPoint();
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
    stopAccessPoint();
  } else {
    startAccessPoint();
  }
}

static void registerDevice() {
  if (!serverUrl.length() || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  const String url = joinUrl(serverUrl, "/api/devices/register");

  DynamicJsonDocument doc(SMALL_JSON_CAPACITY);
  const String mac = WiFi.macAddress();
  doc["mac"] = mac;
  doc["name"] = deviceName;
  doc["connection_type"] = "http";

  String payload;
  serializeJson(doc, payload);

  Serial.println("Registrando dispositivo...");
  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.POST(payload);
  const String response = http.getString();

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("✓ Dispositivo registrado!");
    Serial.println(response);

    DynamicJsonDocument res(SMALL_JSON_CAPACITY);
    DeserializationError err = deserializeJson(res, response);
    if (!err) {
      deviceToken = res["token"].as<String>();
      deviceId = res["device_id"] | 0;
      saveDeviceState();

      Serial.print("Device ID: ");
      Serial.println(deviceId);
      Serial.print("Token: ");
      Serial.println(deviceToken);
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
  String url = joinUrl(serverUrl, "/api/pump/state");
  url += "?device_id=" + String(deviceId);

  http.begin(wifiClient, url);
  if (deviceToken.length()) {
    http.addHeader("Authorization", String("Bearer ") + deviceToken);
  }

  const int httpCode = http.GET();
  const String response = http.getString();

  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(SMALL_JSON_CAPACITY);
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      const bool shouldRun = doc["should_run"] | false;
      setRelay(shouldRun);
      Serial.print("Estado bomba: ");
      Serial.println(shouldRun ? "ENCENDIDA" : "APAGADA");
    } else {
      Serial.print("✗ Error parseando estado: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.print("✗ Error estado (HTTP ");
    Serial.print(httpCode);
    Serial.println(")");
  }

  http.end();
}

static void sendTelemetry() {
  if (deviceId == 0 || WiFi.status() != WL_CONNECTED || !serverUrl.length()) return;

  HTTPClient http;
  const String url = joinUrl(serverUrl, "/api/telemetry");

  DynamicJsonDocument doc(TELEMETRY_JSON_CAPACITY);
  doc["device_id"] = deviceId;
  JsonObject telemetry = doc["telemetry"].to<JsonObject>();
  telemetry["voltage"] = 220.0;   // Ejemplo
  telemetry["current"] = 3.5;     // Ejemplo
  telemetry["is_on"] = digitalRead(RELAY_PIN) == HIGH;

  String payload;
  serializeJson(doc, payload);

  http.begin(wifiClient, url);
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

  String html;
  html.reserve(2048);
  html += "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>";
  html += "<title>BisonByte Setup</title><style>body{font-family:sans-serif;margin:2rem;background:#f7f7f7;}";
  html += "main{max-width:720px;margin:0 auto;background:#fff;padding:2rem;border-radius:1rem;box-shadow:0 1rem 2rem rgba(0,0,0,0.1);}label{display:block;margin-top:1rem;font-weight:600;}";
  html += "input{width:100%;padding:.75rem;margin-top:.5rem;border:1px solid #ddd;border-radius:.5rem;}button{margin-top:1.5rem;padding:.75rem 1.5rem;border:none;border-radius:.5rem;background:#2563eb;color:#fff;font-size:1rem;cursor:pointer;}button:hover{background:#1d4ed8;}section{margin-top:2rem;}";
  html += ".status{padding:1rem;background:#e0f2fe;border-radius:.75rem;}code{background:#e2e8f0;padding:.25rem .5rem;border-radius:.5rem;}</style></head><body><main>";
  html += "<h1>BisonByte Setup</h1>";
  html += "<div class='status'><p><strong>Estado WiFi:</strong> ";
  html += wifiConnected ? "Conectado" : "No conectado";
  html += "</p><p><strong>IP actual:</strong> ";
  html += ip.toString();
  html += "</p><p><strong>Servidor:</strong> <code>";
  html += htmlEscape(serverUrl);
  html += "</code></p></div>";

  html += "<section><h2>Configurar WiFi y servidor</h2><form method='POST' action='/configure'>";
  html += "<label for='ssid'>WiFi SSID</label><input id='ssid' name='ssid' required value='";
  html += htmlEscape(wifiSsid);
  html += "'>";
  html += "<label for='pass'>WiFi Password</label><input id='pass' name='pass' type='password' value='";
  html += htmlEscape(wifiPass);
  html += "'>";
  html += "<label for='server'>URL del servidor Laravel</label><input id='server' name='server' required value='";
  html += htmlEscape(serverUrl);
  html += "'>";
  html += "<button type='submit'>Guardar y conectar</button></form></section>";

  html += "<section><h2>Información del dispositivo</h2><ul>";
  html += "<li><strong>MAC:</strong> ";
  html += WiFi.macAddress();
  html += "</li><li><strong>Device ID:</strong> ";
  html += String(deviceId);
  html += "</li><li><strong>Token:</strong> ";
  html += htmlEscape(deviceToken);
  html += "</li></ul></section>";

  html += "<section><h2>Estado del relé</h2><p>Actualmente: <strong>";
  html += digitalRead(RELAY_PIN) == HIGH ? "ENCENDIDO" : "APAGADO";
  html += "</strong></p></section>";

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
  doc["relay_on"] = digitalRead(RELAY_PIN) == HIGH;

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

  String body = "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'><title>Configuración guardada</title></head><body><p>Configuración guardada. Intentando conectar a <strong>";
  body += htmlEscape(ssid);
  body += "</strong>. Regresa a <a href='/'>inicio</a> para ver el estado.</p></body></html>";

  server.send(200, "text/html; charset=utf-8", body);
}

static void handleNotFound() {
  server.send(404, "text/plain", "No encontrado");
}

static void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/configure", HTTP_POST, handleConfigure);
  server.onNotFound(handleNotFound);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  setRelay(false);

  loadPreferences();
  setupServer();

  if (!connectToWiFi()) {
    startAccessPoint();
  }
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
    if (deviceId == 0 && (millis() - lastCheck) > CHECK_INTERVAL) {
      lastCheck = millis();
      registerDevice();
    } else if (deviceId != 0) {
      const unsigned long now = millis();
      if (now - lastCheck > CHECK_INTERVAL) {
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

