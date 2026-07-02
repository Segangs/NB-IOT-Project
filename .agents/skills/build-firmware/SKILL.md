---
name: build-firmware
description: Use when building, rebuilding, checking, or producing the NB-IOT Pico firmware UF2 with CMake, make, ninja, or firmware build requests.
---

# Build Firmware

Build the NB-IOT firmware from `/Users/segang/Documents/NB-IOT`.

1. Confirm the repository root and `.env` exist. If `.env` is missing, do not invent secrets; point to `.env.example`.
2. Configure with CMake from `build/`.
3. Build with `make -j$(sysctl -n hw.ncpu)` or the repo's existing Ninja setup.
4. Report success/failure and the UF2 path, usually `build/nb_iot_project.uf2`.
5. If the build fails, preserve the first relevant compiler/configuration error and inspect the touched source before proposing a fix.

Use:

```bash
cd /Users/segang/Documents/NB-IOT
mkdir -p build
cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```
