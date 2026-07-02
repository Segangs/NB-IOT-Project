---
name: emqx-setup
description: Use when configuring, verifying, or troubleshooting EMQX MQTT broker setup, Supabase HTTP actions, EMQX rules, or p.zxcx.io MQTT ingestion.
---

# EMQX Setup

The broker is on the dashboard server (`segang.local` / `zxcx.io`), and Pico devices publish to `p.zxcx.io`.

1. Work from `/Users/segang/Documents/NB-IOT/Segang/project`.
2. Confirm required `.env` keys by name only.
3. Review `emqx_setup.sh` before running it.
4. Confirm EMQX API credentials are current without exposing them.
5. Preserve the Docker data volume unless the user explicitly approves a migration.
6. Verify rules use `json_decode(payload)`, `nth`, and EMQX comma-style `cast(col, type)`.

Ask before changing production broker settings.
