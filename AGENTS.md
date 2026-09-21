# JeevesRobot — Hermes2StackChan

Operating manual for any agent working in this directory. Facts and hard rules —
not task state. Current status lives in `SESSION_HANDOVER.md` / `PLANS.md` in the repo root.

## Environment
- ESP-IDF v5.5.1 at `~/esp/esp-idf/` — run `export IDF_PATH=~/esp/esp-idf && source ~/esp/esp-idf/export.sh` before any build.
- Firmware build dir: `firmware/build/` (gitignored).
- USB port: `/dev/cu.usbmodem4101`.
- WiFi: `Tobermory` — credentials in `firmware/.env`.
- Mac LAN IP: `10.10.40.50`.

## Build & Flash
```bash
cd <this directory>
export IDF_PATH=~/esp/esp-idf && source ~/esp/esp-idf/export.sh
python3 scripts/apply_firmware_env.py --env firmware/.env --sdkconfig firmware/sdkconfig
cd firmware && idf.py build && idf.py -p /dev/cu.usbmodem4101 flash
```
- **Check the exit code explicitly.** A wrapper that parses stdout for "Build succeeded"
  can report success while `make`/`idf.py` exited non-zero. Never flash a failed build.

## Bridge (Mac side)
```bash
cd <this directory>
python3.11 -m bridge.hermes2stackchan_bridge run --pair jeeves --no-life --no-info \
  --no-idle-sleep --no-touch-lamp --no-touch-emotions --no-power
```
- ESP-IDF's `export.sh` hijacks Python to 3.9.6. The bridge needs 3.11+.
  Use `python3.11` (resolves to the Hermes venv) or the full venv path.

## MQTT verification
```bash
mosquitto_sub -h 127.0.0.1 -t "hermes-stackchan/jeeves/status" -C 1 -W 3
mosquitto_pub -h 127.0.0.1 -t "hermes-stackchan/jeeves/cmd/face" \
  -m '{"emotion":"happy","intensity_pct":100}'
```

## Key files
- `firmware/main/main.cpp` — firmware source
- `firmware/.env` — firmware config (WiFi credentials)
- `bridge/hermes2stackchan_bridge.py` — bridge service
- `tests/` — Python tests

## HARD RULES (violations have crashed the device before)
1. `firmware/main/main.cpp` line ~7769: the WPA3 `sae_pwe_h2e` line MUST stay
   commented out. Uncommenting it causes a crash loop.
2. SPI clock for the LCD must be ≤10MHz. 40MHz corrupts DMA transfers.
3. Client isolation on the "Tobermory" WiFi must remain DISABLED, or the bridge
   cannot reach the device.

## Known traps
- **Clean rebuild (`rm -rf build/`)** can cause `apply_firmware_env.py` to sync only
  2 of 10 keys. After a clean rebuild, verify the sdkconfig has all expected keys:
  `grep -c CONFIG_ firmware/sdkconfig`. Incomplete config = display may fail to init.
- **`display_ready=true` ≠ screen showing anything.** Blank screen is usually
  (a) draw function early-returns before `begin_frame()`/`flush_frame()`,
  (b) colour inversion wrong, or (c) something clears the screen after drawing.
  Serial must show `begin_frame:` and `flush_frame:` lines for the face to render.
  Fix for stuck-first-frame: set `g_force_face_redraw = true` in `display_boot()`.
- **Colour inversion:** ILI9341 hardware inversion is ON after vendor init.
  `esp_lcd_panel_invert_color()` may be overridden by the driver's vendor init
  sequence. If inversion misbehaves, compensate by changing drawn colours or
  send a raw ILI9341 command after init.
- **`esp_lcd_panel_invert_color(g_panel, false)`** with black background / white lines
  is the working configuration. Uniform white screen = inversion double-applied.

## Working style (enforced)
- **Definition of done = build exit code 0 AND tests pass.** Nothing else counts.
- If the user says the device shows X, accept that observation — verify only via
  MQTT status or serial logs, never by binary/hash analysis.
- If the user says "stalled" or "loop": stop analysing, state ONE corrective
  action, execute it. No circular debugging.
- Commit per task with a descriptive message; one concern per commit.
- Task-based subagents: each task is one subagent, gated by build+test, reviewed
  for spec compliance then quality before the next task starts.
