---
name: modem-debug
description: Use when debugging HL7811 modem, AT commands, LTE-M/NB-IoT attach, MQTTS, CME ERRORs, UART noise, TLS certificates, or Pico modem communication.
---

# Modem Debug

Check the known fragile points before changing code:

1. `PWR_ON_N` pulse sequence is HIGH -> LOW -> HIGH.
2. `pico_enable_stdio_uart` stays disabled for modem UART.
3. AT commands end with `\r` only.
4. Response reads keep the 256-byte guard.
5. Modem communication uses `is_modem_busy`.
6. SSL verification is `AT+KSSLCFG=0,3` unless the user approves a temporary diagnostic exception.
7. MQTTS payloads stay under 80 bytes.

Known symptoms:

- `CME ERROR: 0`: certificate slot/TLS initialization issue.
- `CME ERROR: 907`: EMQX/Supabase auth parameter mismatch; keep `username/password`.
- `CME ERROR: 931`: certificate format or chunking issue.
- Repeating `ERROR`: UART stdio interference or debug task race.

After edits, run a firmware build.
