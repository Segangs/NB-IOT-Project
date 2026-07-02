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
- Also update `README.md` with a concise summary of completed work.
- Preserve previous history. Never replace the accumulated project history with only the current task.
- Commit messages must be short Korean conventional messages, for example `docs: Codex 마이그레이션`.

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
