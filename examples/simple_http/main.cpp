// ESP32 Simple HTTP example for Laravel backend
// Copy this into src/main.cpp to build with PlatformIO

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ========== CONFIGURACIÓN - SOLO CAMBIA ESTO ==========
static const char* WIFI_SSID   = "TU_WIFI";           // Tu red WiFi
static const char* WIFI_PASS   = "TU_PASSWORD";       // Tu contraseña WiFi
static const char* SERVER_URL  = "http://192.168.1.100:8000";  // IP/host de tu servidor Laravel
static const char* MAC_ADDRESS = "AA:BB:CC:DD:EE:FF"; // MAC fija (o déjalo para auto)
static const int   RELAY_PIN   = 2;                    // Pin del relé (GPIO2 por defecto)
// ======================================================

static String deviceToken;
static uint32_t deviceId = 0;
static unsigned long lastCheck = 0;
static const unsigned long CHECK_INTERVAL = 2000; // Revisar cada 2 segundos

static WiFiClient wifiClient;

// Helpers
static String joinUrl(const String& base, const String& path) {
  if (path.startsWith("http://") || path.startsWith("https://")) return path;
  if (base.endsWith("/") && path.startsWith("/")) return base + path.substring(1);
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
}

static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Conectando WiFi a \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✓ WiFi conectado! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("✗ No se pudo conectar al WiFi (intentará nuevamente).");
  }
}

// Registrar el ESP32 en el servidor
static void registerDevice() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  const String url = joinUrl(SERVER_URL, "/api/devices/register");

  // Obtener MAC real si no se especificó
  String mac = MAC_ADDRESS;
  if (mac == "AA:BB:CC:DD:EE:FF") {
    mac = WiFi.macAddress();
  }

  // Construir JSON (ArduinoJson v7)
  JsonDocument doc;
  doc["mac"] = mac;
  doc["name"] = "Bomba ESP32";
  doc["connection_type"] = "http";

  String payload;
  serializeJson(doc, payload);

  Serial.println("Registrando dispositivo...");
  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);
  String response = http.getString();

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("✓ Dispositivo registrado!");
    Serial.println(response);

    JsonDocument res;
    DeserializationError err = deserializeJson(res, response);
    if (!err) {
      deviceToken = res["token"].as<String>();
      deviceId = res["device_id"] | 0;

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

// Revisar si debe encender/apagar la bomba
static void checkPumpState() {
  if (deviceId == 0 || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = joinUrl(SERVER_URL, "/api/pump/state");
  url += "?device_id=" + String(deviceId);

  http.begin(wifiClient, url);
  if (deviceToken.length()) {
    http.addHeader("Authorization", String("Bearer ") + deviceToken);
  }

  int httpCode = http.GET();
  String response = http.getString();

  if (httpCode == HTTP_CODE_OK) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      bool shouldRun = doc["should_run"] | false;
      digitalWrite(RELAY_PIN, shouldRun ? HIGH : LOW);
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

// Enviar datos de telemetría
static void sendTelemetry() {
  if (deviceId == 0 || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  const String url = joinUrl(SERVER_URL, "/api/telemetry");

  // Leer estado actual
  const bool isOn = digitalRead(RELAY_PIN) == HIGH;

  // Crear JSON de telemetría
  JsonDocument doc;
  doc["device_id"] = deviceId;
  JsonObject telemetry = doc["telemetry"].to<JsonObject>();
  telemetry["voltage"] = 220.0;   // Valores de ejemplo
  telemetry["current"] = 3.5;
  telemetry["is_on"] = isOn;

  String payload;
  serializeJson(doc, payload);

  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  if (deviceToken.length()) {
    http.addHeader("Authorization", String("Bearer ") + deviceToken);
  }

  int httpCode = http.POST(payload);
  // Acepta 200 OK o 202 Accepted
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_ACCEPTED) {
    Serial.println("✓ Telemetría enviada");
  } else {
    Serial.print("✗ Error telemetría (HTTP ");
    Serial.print(httpCode);
    Serial.println(")");
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  ensureWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    registerDevice();
  }
}

void loop() {
  // Intento simple de reconexión WiFi si se cae
  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFi();
  }

  if (millis() - lastCheck > CHECK_INTERVAL) {
    lastCheck = millis();
    if (deviceId == 0) {
      registerDevice();
    } else {
      checkPumpState();
      sendTelemetry();
    }
  }

  delay(10);
}

