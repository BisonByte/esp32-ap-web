Simple HTTP example for ESP32 (Arduino/PlatformIO)

How to use

- Open `examples/simple_http/main.cpp`.
- Replace `WIFI_SSID`, `WIFI_PASS`, and `SERVER_URL` with your values.
- Optionally set `MAC_ADDRESS` or leave the default to auto-detect.
- Copy the entire file content into `src/main.cpp` (replace existing) to build and upload.

Laravel backend

- In your Laravel `.env` file add:
  ESP32_ENABLED=true
  Then run: `php artisan config:clear`

Notes

- Uses ArduinoJson 7.x API (`JsonDocument`).
- Accepts HTTP 200 or 201 for registration and 200 or 202 for telemetry.
- `RELAY_PIN` defaults to GPIO2; adjust for your board/relay module.
