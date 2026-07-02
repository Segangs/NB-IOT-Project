# AGENTS.md - NB-IOT Codex Project Guide

## Project Overview

This repository contains the integrated NB-IoT firmware and monitoring server project.

- Firmware root: `/Users/segang/Documents/NB-IOT`
- Server root: `/Users/segang/Documents/NB-IOT/Segang/project`
- Git remote: `https://github.com/Segangs/NB-IOT-Project.git`
- Supabase project: `NB_IOT` (`yzorfvgpmkwnjpdfyqsk`)
- Dashboard/MQTT host: `segang.local`, currently also connected to `zxcx.io`
- Pico MQTT target host: `p.zxcx.io`

Do not refer to the old `/Users/segang/Documents/PicoTeam` path. The server has been integrated under this repository's `Segang/` directory.

## Language And Records

- Respond to the user in Korean unless they ask otherwise.
- Keep code identifiers, commands, file paths, API names, and model names in English.
- After every completed work item, update `project_history.md` using the existing top-appended date format.
- Also update `README.md` with a concise summary of completed work, grouped under one section per date. If a date already exists, append the new work item inside that date section instead of creating another same-date heading.
- Use Korean noun-ending style for new `README.md` and `project_history.md` entries, avoiding explanatory sentence endings such as `했습니다`, `합니다`, or `됩니다`.
- Preserve previous history. Never replace the accumulated project history with only the current task.
- Do not automatically commit or push at the end of each work item. Leave git changes uncommitted unless the user explicitly asks to commit, sync, or push.
- When the user explicitly asks for a commit, commit messages must be short Korean conventional messages, for example `docs: Codex 마이그레이션`.

## Safety

- Do not commit secrets, tokens, `.env` values, Supabase keys, EMQX API keys, or OAuth credentials.
- Ask before external side effects such as push, deploy, DB schema changes, EMQX changes, or destructive file operations.
- Supabase schema changes require explicit user approval after inspecting current tables, logs, and advisors.
- Do not auto-apply RLS or SECURITY DEFINER remediation SQL. Enabling RLS without policies can break the app.

## Firmware Workflow

Use the `build-firmware` skill for firmware build requests.

```bash
cd /Users/segang/Documents/NB-IOT
mkdir -p build
cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

Important firmware constraints:

- Board: `pico2_w`, RP2350, C11/C++17.
- `uart0` on GP0/GP1 is modem-only. Keep `pico_enable_stdio_uart(... 0)`.
- Debug through USB stdio only.
- HL7811 AT terminator is `\r` only, not `\r\n`.
- Keep the UART read guard at 256 bytes per response loop.
- Keep MQTTS payloads within the 80-byte modem limit.
- Telemetry payload format: `[sensorId, temperature]`.
- Boot payload uses compact JSON array fields.
- Keep Flash log entries 32-byte aligned.
- Use `is_modem_busy` around modem communication.
- Known FreeRTOS task names include `vBootTask`, `vSensorTask`, `vLCDTask`, `vPeriodicModemTask`, and `vBuzzerTask`.

## Hardware And PCB Context

Hardware design files are under `DOCS/PCB/`. Read `DOCS/PCB/pico2w_rm78_sensor_pcb_design_portfolio.md` before changing GPIO assignments, power behavior, sensor cabling, modem wiring, or PCB/manufacturing notes.

Current PCB design summary:

- EasyEDA schematic/PCB export date: `2026-07-02`, editor `6.5.57`.
- Core modules: Raspberry Pi Pico 2 W, RM78-1 LTE-M, DS1129-04 dual RJ45, DS18B20, SPH0645LM4H, LCD1602 I2C, 8002A speaker amp, IP5306, MP1584EN, LTC2954CTS8-1, 4-pin electronic power switch module.
- Power nets: `+5V_IP5306` is always-on IP5306 output for LTC2954 and switch input only; `+5V_SYS` is switched system power for Pico, RM78-1, LCD, sensors, and amplifier; `+3V3OUT` is Pico 3.3V for sensors/pull-ups.
- Modem GPIO map: GP0 LTE TXD, GP1 LTE RXD, GP2 WAKEUP, GP3 RESET, GP4 PWRON, GP5 RM78-1 TXON input, GP28 TXON display LED.
- Power-management GPIO map: GP14 LTC2954 INT, GP15 LTC2954 KILL.
- LCD GPIO map: GP16 SDA and GP17 SCL through BSS138 level shifter to 5V LCD1602 I2C.
- Sensor/audio GPIO map: GP18 I2S BCLK shared, GP19 I2S LRCLK shared, GP20 MIC1 DOUT, GP21 MIC2 DOUT, GP22 TEMP1 DATA, GP26 TEMP2 DATA.
- SPH0645LM4H design intent: not voice recording or simple sound logging, but long-term machine acoustic pattern collection for Edge AI/TinyML inputs, including normal operation, abnormal operation, and predictive-abnormal state classification.
- Audio data handling direction: collect raw PCM or derived features such as FFT, RMS, band energy, or MFCC-like features; keep 3m UTP stability in mind by preferring moderate sample rates such as 8-16 kHz, or 24 kHz only when needed.
- LED/speaker GPIO map: GP6 speaker PWM through series resistor, GP8 status red, GP9 status green, GP10-GP13 RJ45 LEDs.
- DS1129-04 RJ45 UTP pairing: BCLK with GND on pair 4-5; DOUT and TEMP DATA on pair 3-6; LRCLK on pair 7; pin 8/16 NC.
- DS18B20 pull-ups: 5.1kΩ to `+3V3OUT`; I2S series damping: 47Ω on BCLK/LRCLK/DOUT lines.

Known PCB review follow-ups:

- Change speaker input resistor `R6` from 100kΩ to 1kΩ for `GP6 -> 8002A SIG`.
- Confirm whether GP7 external-power sensing sees regulated 5V or raw DC jack voltage; keep Pico input below 3.3V.
- Confirm LTC2954 `C4` PDT 22µF matches desired shutdown delay.
- Keep I2S 47Ω resistors close to the driving side and preserve GND return around BCLK/LRCLK/DOUT.
- Keep RM78-1 and 8002A `+5V_SYS` paths wide, with the 1000µF modem capacitor near RM78-1.
- Treat `DOCS/PCB` as the source of truth for PCB/manufacturing context unless the user provides a newer EasyEDA export.

## Server Workflow

Use the `run-server`, `mock-test`, and `emqx-setup` skills for server operation, simulated Pico testing, and EMQX setup work.

```bash
cd /Users/segang/Documents/NB-IOT/Segang/project
python3 app.py
python3 mock_pico_client.py
```

Server notes:

- Backend: Flask and Supabase Python SDK.
- Supabase Realtime drives live dashboard updates.
- Google OAuth uses the local loopback/PyWebView flow.
- `auth_device` RPC arguments must remain `username text, password text`.
- `get_device_sensors` returns sensor mapping plus `tempUpperLimitValue` and `tempLowerLimitValue`.
- EMQX rule SQL uses `json_decode(payload)`, `nth`, and `cast(col, type)` comma syntax.

## Supabase

Use the `supabase-inspect` skill before DB inspection and `db-migrate` before DB changes.

Current verified project:

- Project name: `NB_IOT`
- Project ref: `yzorfvgpmkwnjpdfyqsk`
- Region: `ap-northeast-1`
- API URL: `https://yzorfvgpmkwnjpdfyqsk.supabase.co`

Known advisory state as of 2026-07-02:

- Supabase MCP OAuth access works in Codex.
- `public` tables currently report RLS disabled.
- Several public functions report mutable `search_path`.
- Several `SECURITY DEFINER` functions are executable by `anon`/`authenticated` roles.

Treat these as follow-up security work that needs a policy design and explicit approval.

## Codex Skills

Repository-scoped skills live under `.agents/skills/`.

- `build-firmware`: CMake firmware build.
- `run-server`: Flask dashboard server run.
- `supabase-inspect`: Supabase project, table, log, and advisor inspection.
- `db-migrate`: approved Supabase DB migration workflow.
- `commit-and-log`: history, README, and git commit finish flow.
- `modem-debug`: HL7811 AT/MQTT debugging.
- `emqx-setup`: EMQX/Supabase setup script workflow.
- `mock-test`: Pico mock client test.
- `project-history-update`: top-appended history updates.

Invoke these when the user request matches their description.

## Verification

Before claiming completion:

- Run the smallest relevant validation command.
- For config/documentation changes, validate TOML/Markdown shape where practical.
- For firmware changes, run CMake/build unless blocked by environment.
- For server changes, run targeted Python syntax/tests or a local smoke test.
- Report anything that could not be verified.
