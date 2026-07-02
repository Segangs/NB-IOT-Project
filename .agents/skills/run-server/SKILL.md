---
name: run-server
description: Use when starting, checking, or troubleshooting the integrated Flask dashboard server, local web app, or PyWebView app under Segang/project.
---

# Run Server

The integrated server lives at `/Users/segang/Documents/NB-IOT/Segang/project`.

1. Work from the server directory.
2. Confirm `.env` exists and includes required keys by name only; never print secret values.
3. Install dependencies only when needed and approved if network access is required.
4. Run `python3 app.py` for the Flask dashboard or `python3 desktop_app.py` for the desktop wrapper.
5. Report the local URL/port and any startup error.

Useful commands:

```bash
cd /Users/segang/Documents/NB-IOT/Segang/project
python3 app.py
python3 desktop_app.py
```
