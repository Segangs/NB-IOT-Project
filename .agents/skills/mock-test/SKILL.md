---
name: mock-test
description: Use when testing the server pipeline without physical Pico hardware, running mock_pico_client.py, or validating simulated telemetry/boot ingestion.
---

# Mock Test

1. Confirm the Flask server is running or start it with the `run-server` workflow.
2. Run the mock client from `/Users/segang/Documents/NB-IOT/Segang/project`.
3. Check whether Supabase receives expected `sensorvalue`, `device_boot_logs`, or related rows.
4. Report simulated payloads, DB/API result, and any error.

Command:

```bash
cd /Users/segang/Documents/NB-IOT/Segang/project
python3 mock_pico_client.py
```
