import os
import json
from pathlib import Path

# PlatformIO provides `env` and `projenv` via SCons
try:
    Import("env", "projenv")  # type: ignore # noqa: F821
except Exception:
    # Fallback for static analysis
    env = None
    projenv = None


def parse_dotenv(dotenv_path: Path) -> dict:
    data: dict[str, str] = {}
    if not dotenv_path.exists():
        return data
    for line in dotenv_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        # strip quotes if present
        if (v.startswith('"') and v.endswith('"')) or (v.startswith("'") and v.endswith("'")):
            v = v[1:-1]
        data[k] = v
    return data


def quote_define(value: str) -> str:
    # Use JSON to safely quote/escape for C/C++ defines
    return json.dumps(value)


def main() -> None:
    project_dir = Path(__file__).resolve().parent
    repo_root = project_dir.parent
    dotenv = parse_dotenv(repo_root / ".env")

    ssid = os.environ.get("ESP32_WIFI_SSID") or dotenv.get("ESP32_WIFI_SSID")
    pwd = os.environ.get("ESP32_WIFI_PASSWORD") or dotenv.get("ESP32_WIFI_PASSWORD")

    # Server URL priority: explicit ESP32_DEFAULT_SERVER_URL, then APP_URL, then default
    server_url = (
        os.environ.get("ESP32_DEFAULT_SERVER_URL")
        or dotenv.get("ESP32_DEFAULT_SERVER_URL")
        or os.environ.get("APP_URL")
        or dotenv.get("APP_URL")
        or "https://proyecto.bisonbyte.io"
    )

    cppdefines = []

    if ssid:
        cppdefines.append(("DEFAULT_WIFI_SSID", quote_define(ssid)))
    if pwd:
        cppdefines.append(("DEFAULT_WIFI_PASS", quote_define(pwd)))

    # Always pass server URL to ensure HTTPS/host is aligned with backend
    if server_url:
        cppdefines.append(("DEFAULT_SERVER_URL", quote_define(server_url)))

    if cppdefines and env is not None:
        env.Append(CPPDEFINES=cppdefines)


main()

