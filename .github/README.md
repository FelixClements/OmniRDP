# OmniRDP

Standalone N:1 RDP multiplexer — one backend Windows RDP session fanned out to multiple simultaneous viewers.

Built on FreeRDP 3.25.0 client and server APIs. The process sits in the middle: it connects to one Windows target as an RDP client and accepts multiple viewer connections as an RDP server, forwarding display updates and arbitrating input between viewers.

## Features

- **N:1 multiplexing** — multiple viewers see the same remote desktop simultaneously
- **Input arbitration** — one viewer at a time holds the input lock; idle timeout releases it automatically
- **Cursor visibility** — passive viewers (without input lock) see the current mouse position via `PointerPosition` PDUs
- **Multi-monitor support** — configure 1–16 monitors (each 1920×1080) for the backend session
- **Classic SurfaceBits/NSCodec path** — stable display pipeline using encoded bitmap transport
- **Late-join** — new viewers receive a full refresh on connect
- **Slow-viewer disconnect** — viewers that can't keep up are automatically disconnected

## Architecture

```
┌──────────────┐
│  Windows RDP │
│    Server    │
└──────┬───────┘
       │ RDP client connection
       │ (FreeRDP client APIs)
┌──────┴───────┐
│   OmniRDP    │  ← standalone multiplexer process
│  (port 3389  │     backend client + viewer server
│   → backend, │     in one binary)
│   port 13389 │
│   → viewers) │
└──────┬───────┘
       │ RDP server connections
       │ (FreeRDP server APIs)
  ┌────┼────┐
  │    │    │
  V    V    V
Viewer Viewer Viewer ...
(active) (passive) (passive)
```

- **Backend side**: connects to one Windows RDP target using FreeRDP client APIs
- **Viewer side**: accepts multiple incoming viewer connections using FreeRDP server APIs on port 13389
- **Display path**: classic `SurfaceBits`/NSCodec — the multiplexer forwards encoded bitmap updates from the backend to all viewers
- **Input path**: only the viewer holding the input lock can send keyboard/mouse input to the backend; other viewers' input is silently dropped

## Project Structure

```
OmniRDP/
├── CMakeLists.txt
├── include/
│   ├── backend.h              — BackendClient API and types
│   ├── viewer_server.h         — ViewerServer, Viewer, MonitorLayout types
│   └── platform_compat.h       — Cross-platform helpers
├── src/
│   ├── main.c                 — Entrypoint, CLI parsing, event loop
│   ├── backend.c               — FreeRDP client to Windows target
│   ├── viewer_server.c         — FreeRDP server for viewer connections
│   ├── viewer_internal.c      — Pure helper logic (input policy, caps, late-join)
│   ├── viewer_internal.h       — Internal helpers header
│   ├── pointer_shape.c         — Pointer shape cache
│   ├── pointer_shape.h         — Pointer shape types
│   └── platform_compat.c       — Sleep, timestamps, signal handling
└── tests/
    ├── CMakeLists.txt
    ├── test_late_join_policy.c
    ├── test_pointer_shape.c
    └── test_viewer_state.c
```

## Build

### Prerequisites

- CMake 3.20+
- FreeRDP 3.25.0 (vendored in `freerdp-3.25.0/`, must be built first)
- C11 compiler

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

# 2. Build OmniRDP
cmake -S OmniRDP -B OmniRDP/build -G "Visual Studio 17 2022" -A x64
cmake --build OmniRDP/build --config Release -j
```

## Usage

```
OmniRDP <hostname> <port> <username> <password> [domain] [monitors]
```

| Argument   | Required | Description                                      |
|------------|----------|--------------------------------------------------|
| hostname   | Yes      | Windows RDP server hostname or IP                |
| port       | Yes      | RDP port (typically 3389)                         |
| username   | Yes      | RDP username                                      |
| password   | Yes      | RDP password                                      |
| domain     | No       | Domain (use `.` for workgroup)                   |
| monitors   | No       | Number of 1920×1080 monitors (1–16, default: 1)  |

### Examples

Single monitor (1920×1080 desktop):

```bash
# Windows
.\OmniRDP\build\Release\OmniRDP.exe 192.168.1.209 3389 localadmin mypassword
```

Dual monitor (3840×1080 desktop):

```bash
# Windows
.\OmniRDP\build\Release\OmniRDP.exe 192.168.1.209 3389 localadmin mypassword . 2
```

With domain:

```bash
OmniRDP 192.168.1.209 3389 admin password MYDOMAIN 2
```

### Connecting Viewers

Viewers connect to the multiplexer on port **13389**:

```bash
# Using xfreerdp (Linux)
xfreerdp /v:127.0.0.1:13389 /u:viewer /p:viewer /sec:rdp

# Using mstsc (Windows)
# Connect to 127.0.0.1:13389 in Remote Desktop Connection
```

> **Note:** On Windows, ensure the FreeRDP DLLs (`freerdp3.dll`, `freerdp-client3.dll`, `freerdp-server3.dll`, `winpr3.dll`) are either in the same directory as the executable or on your `PATH`.

## Input Arbitration

- The first viewer to send input automatically acquires the **input lock**
- Only the viewer holding the input lock can send keyboard/mouse events to the backend
- If the input owner is idle for the timeout period, the lock is released and another viewer can take over
- Passive viewers (without the input lock) still see the desktop and the current mouse cursor position

Unit tests cover input ownership policy, late-join strategy, and pointer shape cache logic.

## Repository Layout

```
OmniRDP/              — Active project source
freerdp-3.25.0/       — Vendored FreeRDP dependency (must be built first)
archive/              — Historical plans, architecture notes, and prior proxy-plugin work
```

## License

See [LICENSE](../LICENSE) for details.