# Ejemplos de PlatformIO

Este directorio contiene sketches de referencia para el firmware HTTP del ESP32.

## Cargar el firmware principal con variables del panel

1. Ajusta las claves `ESP32_WIFI_*`, `ESP32_DEVICE_ID`, `ESP32_ACTIVATION_KEY` y `ESP32_HTTP_*` en la raíz del proyecto (`.env`).
2. Conecta el módulo por USB y ejecuta:
   ```bash
   pio run -t upload
   ```
   PlatformIO ejecutará `pre_build_env.py` antes de compilar y generará `include/DefaultBackendConfig.h` con los valores anteriores.
3. El binario resultante utilizará esas credenciales como valores por defecto, por lo que el dispositivo quedará enlazado con tu panel nada más iniciar.

Consulta `simple_http/README.md` si necesitas un sketch mínimo para hacer pruebas rápidas sin el portal de configuración.
