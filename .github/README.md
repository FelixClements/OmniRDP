# OmniRDP

Standalone N:1 RDP multiplexer with Windows service management — one backend Windows RDP session fanned out to multiple simultaneous viewers.

Built on FreeRDP 3.26.0 client and server APIs. The process sits in the middle: it connects to one Windows target as an RDP client and accepts multiple viewer connections as an RDP server, forwarding display updates and arbitrating input between viewers.

New in v2: Windows service manager (`OmniRDP-svc.exe`) with system tray app (`OmniRDP-tray.exe`) for managing multiple backend instances. Multi-service support with isolated configs, per-instance logging, and named pipe IPC.

## Features

### Core Multiplexer
- **N:1 multiplexing** — multiple viewers see the same remote desktop simultaneously
- **Input arbitration** — one viewer at a time holds the input lock; idle timeout releases it automatically
- **Cursor visibility** — passive viewers see the current mouse position
- **Multi-monitor support** — configure 1–16 monitors for the backend session
- **Classic SurfaceBits/NSCodec path** — stable display pipeline using encoded bitmap transport
- **Late-join** — new viewers receive a full refresh on connect
- **Slow-viewer disconnect** — viewers that can't keep up are automatically disconnected

### Service Manager (NEW)
- **Windows Service** — OmniRDP runs as a proper Windows service in Session 0
- **Multiple instances** — one service manages multiple backend+viewer pairs from a single config file
- **Auto-recovery** — crashed instances auto-restart with exponential backoff
- **DPAPI encryption** — passwords stored encrypted in config, never on command line
- **Heartbeat monitoring** — child process health tracked via named pipes
- **Config hot-reload** — add/remove instances without restarting the service

### System Tray App (NEW)
- **Status icon** — colored tray icon (green/yellow/red/gray) reflects overall health
- **Status window** — ListView table showing all instances across all services
- **Instance controls** — Start/Stop/Restart instances from the tray menu
- **Log viewer** — built-in window for viewing service logs
- **First-time setup** — prompts to install service and configure instances
- **Session change handling** — graceful shutdown on user logoff

### Multi-Service Support (NEW)
- **Named services** — install multiple isolated OmniRDP services (`OmniRDP-Prod`, `OmniRDP-Test`, etc.)
- **Per-service configs** — each service has its own `config.ini`
- **Per-service logs** — isolated log directories per service
- **Tray discovery** — tray app auto-discovers all `OmniRDP-*` services via SCM enumeration

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  User Session (Session 1+)                                  │
│  ┌─────────────────────────┐                                │
│  │  OmniRDP-tray.exe       │  ← System tray + status window │
│  │  - Tray icon (colored)  │                                │
│  │  - Status window        │                                │
│  │  - Service discovery    │                                │
│  └───────────┬─────────────┘                                │
│              │ Named Pipe IPC (\.\pipe\OmniRDP_Pipe)        │
└──────────────┼──────────────────────────────────────────────┘
               │
┌──────────────┼──────────────────────────────────────────────┐
│              │  Session 0 (Windows Service)                 │
│  ┌───────────▼───────────┐                                  │
│  │  OmniRDP-svc.exe      │  ← SCM-registered service        │
│  │  - Instance manager   │                                  │
│  │  - Config watcher     │                                  │
│  │  - Named pipe server  │                                  │
│  └──┬───────┬────────────┘                                  │
│     │       │                                               │
│  ┌──▼──┐ ┌──▼──┐                                            │
│  │Inst1│ │Inst2│  ← Child processes (OmniRDP.exe)           │
│  │     │ │     │     Each: backend client + viewer server   │
│  └─────┘ └─────┘                                            │
└─────────────────────────────────────────────────────────────┘
```

### Three Executables

| Binary | Purpose | Dependencies |
|--------|---------|-------------|
| `OmniRDP.exe` | Standalone multiplexer or service child instance | FreeRDP 3.26.0 (client+server) |
| `OmniRDP-svc.exe` | Windows Service manager | advapi32, crypt32, kernel32 |
| `OmniRDP-tray.exe` | System tray application | comctl32, shell32, wtsapi32 |

## Project Structure

```
OmniRDP/
├── CMakeLists.txt
├── include/
│   ├── backend.h              — BackendClient API and types
│   ├── viewer_server.h         — ViewerServer, Viewer, MonitorLayout types
│   └── platform_compat.h       — Cross-platform helpers
├── src/
│   ├── main.c                 — Standalone CLI entrypoint
│   ├── backend.c               — FreeRDP client to Windows target
│   ├── viewer_server.c         — FreeRDP server for viewer connections
│   ├── viewer_internal.c/h    — Pure helper logic (input policy, caps, late-join)
│   ├── pointer_shape.c/h      — Pointer shape cache
│   ├── platform_compat.c       — Sleep, timestamps, signal handling
│   │
│   │  Service Manager (NEW):
│   ├── svc_main.c             — Service entry point (--install/--uninstall/--run)
│   ├── svc_service.c/h        — SCM integration, install/uninstall
│   ├── svc_instance_mgr.c/h   — Child process spawn/monitor/restart
│   ├── svc_config.c/h         — INI config loading and validation
│   ├── svc_log.c/h            — Thread-safe file logging with rotation
│   ├── svc_dpapi.c/h          — DPAPI password encrypt/decrypt
│   ├── ini_parser.c/h         — Minimal INI file parser
│   ├── instance_runner.c      — --instance mode entry point for child processes
│   │
│   │  Named Pipe IPC (NEW):
│   ├── pipe_protocol.c/h      — Wire format, framing, message types
│   ├── svc_pipe_server.c/h    — Service-side named pipe server
│   ├── tray_pipe_client.c/h   — Tray-side named pipe client
│   │
│   │  Tray App (NEW):
│   ├── tray_main.c            — Tray app entry point
│   ├── tray_icon.c/h          — System tray icon, context menu, polling
│   ├── tray_status_dlg.c/h    — Status window with ListView table
│   └── tray_log_viewer.c/h    — Built-in log viewer window
└── tests/
    ├── test_late_join_policy.c
    ├── test_pointer_shape.c
    └── test_viewer_state.c
```

## Build

### Prerequisites

- CMake 3.20+
- FreeRDP 3.26.0 (vendored in `freerdp-3.26.0/`, must be built first)
- C11 compiler
- Windows SDK (for service manager and tray app)

### Windows (Visual Studio 2022)

Prerequisites: CMake, Visual Studio 2022 BuildTools/IDE, vcpkg with `openssl`, `libjpeg-turbo`, `libpng`, `zlib`, `libusb` installed for `x64-windows`.

```powershell
# 1. Build FreeRDP
cmake -S freerdp-3.26.0 -B freerdp-3.26.0/build `
  -DWITH_SERVER=ON -DWITH_CLIENT=ON -DWITH_SHADOW=OFF -DWITH_PROXY=OFF `
  -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=ON `
  -DWITH_X11=OFF -DWITH_WAYLAND=OFF -DWITH_PULSE=OFF -DWITH_ALSA=OFF `
  -DWITH_FFMPEG=OFF -DWITH_CAIRO=OFF -DWITH_SDL2=OFF `
  -DWITH_SWSCALE=OFF -DWITH_DSP_FFMPEG=OFF -DWITH_VIDEO_FFMPEG=OFF `
  -DWITH_OPENH264=OFF -DWITH_MEDIA_FOUNDATION=OFF `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -G "Visual Studio 17 2022" -A x64

cmake --build freerdp-3.26.0/build --config Release -j

# 2. Build OmniRDP (all 3 targets)
cmake -S OmniRDP -B OmniRDP/build -G "Visual Studio 17 2022" -A x64
cmake --build OmniRDP/build --config Release --target OmniRDP OmniRDP-svc OmniRDP-tray -j
```

## Usage

### Standalone Mode (original)

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

### Service Manager Mode (NEW)

#### Installation

```powershell
# Install the service (runs as Administrator)
OmniRDP-svc.exe --install

# Install with custom name and config
OmniRDP-svc.exe --install --service-name "OmniRDP-Prod" --config "C:\OmniRDP\prod\config.ini"
```

#### Managing Instances

Edit `C:\ProgramData\OmniRDP\config.ini`:

```ini
[service]
log_level = debug
log_dir = C:\ProgramData\OmniRDP\logs

[instances]
names = office-desktop, lab-server

[instance:office-desktop]
enabled = true
backend.hostname = 192.168.1.209
backend.port = 3389
backend.username = localadmin
backend.password = changeme
viewer.bind_address = 0.0.0.0
viewer.port = 3390

[instance:lab-server]
enabled = true
backend.hostname = lab-rdp.company.com
backend.port = 3389
backend.username = admin
backend.password = changeme
viewer.bind_address = 127.0.0.1
viewer.port = 3391
```

After editing, reload the config from the tray app or restart the service.

#### Tray App

```powershell
# Run the tray app
OmniRDP-tray.exe

# Register for auto-start on login
OmniRDP-tray.exe --install
```

### Instance Mode (spawned by service, not for direct use)

```
OmniRDP --instance <name> --secrets-handle <handle> [--config <path>]
```

### Connecting Viewers

Viewers connect to the instance's configured viewer port:

```bash
# Using mstsc (Windows) — connect to the viewer port from the config
mstsc /v:127.0.0.1:3390

# Using xfreerdp (Linux)
xfreerdp /v:127.0.0.1:3390 /u:viewer /p:viewer /sec:rdp
```

> **Note:** On Windows, ensure the FreeRDP DLLs (`freerdp3.dll`, `freerdp-client3.dll`, `freerdp-server3.dll`, `winpr3.dll`) are either in the same directory as the executable or on your `PATH`.

## Configuration Reference

### `[service]` Section

| Key | Default | Description |
|-----|---------|-------------|
| `log_level` | `info` | Log level: debug, info, warn, error |
| `log_dir` | `C:\ProgramData\OmniRDP\logs` | Log directory |
| `log_max_size_mb` | `10` | Max log file size before rotation |
| `log_max_files` | `5` | Max old log files to keep |
| `pipe_name` | `OmniRDP_ServicePipe` | Named pipe for tray IPC |
| `heartbeat_timeout_sec` | `10` | Child process heartbeat timeout |
| `graceful_shutdown_sec` | `10` | Graceful shutdown wait time |

### `[instance:<name>]` Section

| Key | Default | Required | Description |
|-----|---------|----------|-------------|
| `enabled` | `true` | No | Enable/disable this instance |
| `backend.hostname` | — | **Yes** | Backend RDP server hostname/IP |
| `backend.port` | `3389` | No | Backend RDP port |
| `backend.username` | — | **Yes** | Login username |
| `backend.password` | — | **Yes** | Password (plaintext auto-encrypted with DPAPI) |
| `backend.domain` | — | No | Domain (empty for workgroup) |
| `viewer.bind_address` | `127.0.0.1` | No | Viewer listener address |
| `viewer.port` | — | **Yes** | Viewer listener port |
| `viewer.max_viewers` | `10` | No | Max simultaneous viewers |
| `viewer.cert_path` | — | No | TLS certificate path |
| `viewer.key_path` | — | No | TLS key path |
| `display.monitor_count` | `1` | No | Number of monitors |
| `display.monitor_width` | `1920` | No | Monitor width |
| `display.monitor_height` | `1080` | No | Monitor height |
| `reconnect.enabled` | `true` | No | Auto-reconnect on disconnect |
| `reconnect.max_attempts` | `10` | No | Max reconnect attempts |
| `codec.nscodec` | `true` | No | Enable NSCodec |
| `codec.remote_fx` | `true` | No | Enable RemoteFX |
| `security.tls_enabled` | `true` | No | Enable TLS security |
| `security.nla_enabled` | `true` | No | Enable NLA security |

## Input Arbitration

- The first viewer to send input automatically acquires the **input lock**
- Only the viewer holding the input lock can send keyboard/mouse events to the backend
- If the input owner is idle for the timeout period, the lock is released
- Passive viewers still see the desktop and current mouse cursor position

## Security

- **DPAPI encryption**: Passwords in `config.ini` are auto-encrypted with `CryptProtectData` (machine-local)
- **Password transfer**: Never on command line — passed via anonymous pipe with handle inheritance
- **Config file ACL**: SYSTEM/Admin Full Control, Authenticated Users Read-only
- **Named pipe ACL**: Restricted to SYSTEM, NetworkService, and interactive user
- **Service account**: Defaults to `NT AUTHORITY\NetworkService` (least privilege)

## Repository Layout

```
OmniRDP/              — Active project source (multiplexer + service manager + tray app)
freerdp-3.26.0/       — Vendored FreeRDP dependency (must be built first)
archive/              — Historical plans, architecture notes, and prior proxy-plugin work
```

## License

See [LICENSE](../LICENSE) for details.
