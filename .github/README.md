# RDP Multiplexer

This repository is centered on `multiplexer-v2/`: a standalone true N:1 RDP multiplexer built on FreeRDP client and server APIs.

## Active direction

- one backend Windows RDP session
- multiple simultaneous viewers
- standalone server process, not a FreeRDP proxy plugin
- true N:1 based on FreeRDP implementation so the project does not have to keep rebuilding protocol state by hand where a native FreeRDP path exists

The active roadmap is:

- `.sisyphus/plans/n1-multiplexer-v2-plan.md`

## Repository layout

- `multiplexer-v2/` — active standalone project
- `freerdp-3.25.0/` — FreeRDP dependency used by `multiplexer-v2`
- `FreeRDP-upstream/` — upstream reference tree for comparison only
- `archive/legacy-proxy-plugin/` — archived proxy-module implementation and superseded proxy-era plans/docs

## Why the project restarted as standalone

The clearest local rationale is preserved in:

- `archive/legacy-proxy-plugin/.sisyphus/plans/rdp-multiplexer-plan-adjusted.md`

That archived history captures the earlier proxy-module direction and why the project eventually moved to a standalone v2 path.

## Current v2 status

- backend FreeRDP client connection works
- viewer listener works
- multi-viewer latest-frame fanout exists
- takeover-style keyboard/mouse input arbitration exists
- per-viewer RDPEGFX negotiation and delivery path now exists in first-pass form, with classic fallback still preserved
- real-environment hardening is still in progress

## Build

### Linux

```bash
cmake -S freerdp-3.25.0 -B freerdp-3.25.0/build -DWITH_SERVER=ON -DWITH_SHADOW=ON -DWITH_PROXY=OFF
cmake --build freerdp-3.25.0/build -j$(nproc)
cmake -S multiplexer-v2 -B multiplexer-v2/build
cmake --build multiplexer-v2/build -j$(nproc)
```

### Windows (Visual Studio 2022)

Prerequisites: CMake, Visual Studio 2022 BuildTools/IDE, vcpkg with `openssl`, `libjpeg-turbo`, `libpng`, `zlib`, `libusb` installed for `x64-windows`.

```powershell
# 1. Build FreeRDP
cmake -S freerdp-3.25.0 -B freerdp-3.25.0/build `
  -DWITH_SERVER=ON -DWITH_CLIENT=ON -DWITH_SHADOW=OFF -DWITH_PROXY=OFF `
  -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=ON `
  -DWITH_X11=OFF -DWITH_WAYLAND=OFF -DWITH_PULSE=OFF -DWITH_ALSA=OFF `
  -DWITH_FFMPEG=OFF -DWITH_CAIRO=OFF -DWITH_SDL2=OFF `
  -DWITH_SWSCALE=OFF -DWITH_DSP_FFMPEG=OFF -DWITH_VIDEO_FFMPEG=OFF `
  -DWITH_OPENH264=OFF -DWITH_MEDIA_FOUNDATION=OFF `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -G "Visual Studio 17 2022" -A x64

cmake --build freerdp-3.25.0/build --config Release -j

# 2. Build multiplexer-v2
cmake -S multiplexer-v2 -B multiplexer-v2/build -G "Visual Studio 17 2022" -A x64
cmake --build multiplexer-v2/build --config Release -j
```

## Run

### Linux

```bash
./multiplexer-v2/build/multiplexer-v2 <backend-host> 3389 <username> <password> [domain]
```

Example:

```bash
./multiplexer-v2/build/multiplexer-v2 192.168.1.209 3389 localadmin localadmin .
```

### Windows

```powershell
.\multiplexer-v2\build\Release\multiplexer-v2.exe <backend-host> 3389 <username> <password> [domain]
```

Example:

```powershell
.\multiplexer-v2\build\Release\multiplexer-v2.exe 192.168.1.209 3389 localadmin localadmin .
```

> **Note:** On Windows, ensure the FreeRDP DLLs (`freerdp3.dll`, `freerdp-client3.dll`, `freerdp-server3.dll`, `winpr3.dll`) are either in the same directory as the executable or on your `PATH`.

## Local smoke tests

Plain viewer:

```bash
timeout 10 xvfb-run -a ./freerdp-3.25.0/build/client/X11/xfreerdp /v:127.0.0.1:13389 /u:viewer /p:viewer /sec:rdp +clipboard /log-level:warn
```

RDPEGFX viewer:

```bash
timeout 15 xvfb-run -a ./freerdp-3.25.0/build/client/X11/xfreerdp /v:127.0.0.1:13389 /u:viewer /p:viewer /sec:rdp /gfx +clipboard /log-level:warn
```

## Notes

- `build/`, `build-review/`, `build-review-ci/`, `build-review-ci2/`, and `build-review-test/` are local build artifact directories, not active project source.
- `CERT-C-QUICKREF.md` remains active as a coding reference.
