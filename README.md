
# IoT Monorepo — Prod/Dev Split (Arduino + ESP32)

This layout separates **stable app code** under `apps/.../prod/` from **active development lines** under `apps/.../dev/`. Each dev/prod sketch lives in its own sketch folder so it works in the Arduino IDE *and* CI.

## Key folders
- `firmware/apps/air_monitor/prod/air_monitor_prod/` — shipping app (feature flags in `common/config/features.h`)
- `firmware/apps/air_monitor/dev/` — focused dev lines (`dev_oled`, `dev_sen66`, `dev_flash`)
- `firmware/modules/` — reusable drivers (OLED, SEN66, Flash) with headers in `include/` and optional `examples/`
- `firmware/common/` — `features.h`, `version.h`, and `util/Logging.h`
- `firmware/platform/esp32c6/` — pins & board config
- `firmware/labs/` — experimental spikes (can break)
- `.github/workflows/` — CI that compiles all sketches (prod + dev + labs) via `arduino-cli`

## Quick start
1. Open any sketch folder in Arduino IDE (e.g., `firmware/apps/air_monitor/prod/air_monitor_prod/`).
2. Copy `provisioning/arduino_secrets.h.example` ➜ `include/arduino_secrets.h` inside that sketch folder.
3. Select **ESP32C6 Dev Module** (or your target) and upload.

## CI
CI compiles all sketches on PRs. Pin library versions there to avoid surprises.

หงส์มาเยือน
