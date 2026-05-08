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



---

## Installation and First Connection Guide

This guide walks you through the complete setup — from getting the binaries to connecting your first RDP viewer.

---

### 1. Where to Download

**Pre-built releases** — if available, download the latest release zip from the [Releases](https://github.com/FelixClements/OmniRDP/releases) page. Otherwise, follow the [Build](#build) instructions above to compile from source.

The build output contains these files you need:

| File | Purpose |
|------|---------|
| `OmniRDP.exe` | Core multiplexer (standalone or child process) |
| `OmniRDP-svc.exe` | Windows service manager |
| `OmniRDP-tray.exe` | System tray app for monitoring/control |
| `freerdp3.dll` | FreeRDP client library |
| `freerdp-client3.dll` | FreeRDP client helpers |
| `freerdp-server3.dll` | FreeRDP server library |
| `winpr3.dll` | FreeRDP Windows portability layer |
| `libcrypto-3-x64.dll` | OpenSSL crypto library |
| `libssl-3-x64.dll` | OpenSSL TLS library |
| `libusb-1.0.dll` | USB redirection support |
| `zlib1.dll` | Compression library |

---

### 2. How to Install

1. **Download the installer** — get `OmniRDP-Setup.exe` from the GitHub Actions build artifacts (or from [GitHub Releases](https://github.com/OmniRDP/OmniRDP/releases) once published).

2. **Run `OmniRDP-Setup.exe`** — this is an Inno Setup installer that handles everything automatically:
   - Installs all executables and DLLs to `C:\Program Files\OmniRDP`
   - Copies `setup/config.ini.template` to `C:\ProgramData\OmniRDP\config.ini` (only if it doesn't already exist)
   - Registers the Windows service (`OmniRDP-svc.exe --install`)
   - Registers the tray app for auto-start on login (`OmniRDP-tray.exe --install`)
   - No manual file copying or `--install` commands needed — the installer does it all

On first run, if no config file exists yet, the service auto-generates a default config at `C:\ProgramData\OmniRDP\config.ini`. You'll edit this file in step 4.

> **Note on DLLs:** The installer places all DLLs in `C:\Program Files\OmniRDP` alongside the executables, so they are always found. See the [Usage note](#connecting-viewers) for details.

---

### 3. Generate SSL Certificates

Viewer connections use TLS. You need a certificate and private key pair. Two options:

**Option A — Use the built-in test certificates** (quick start, not for production):

The build generates test certs at `OmniRDP/build/Release/server.crt` and `OmniRDP/build/Release/server.key`. Copy them alongside your executables.

**Option B — Create self-signed certificates with OpenSSL**:

```powershell
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=YourServerName"
```

**Option C — Use the FreeRDP `makecert` tool**:

Located at `freerdp-3.26.0/winpr/tools/makecert/`. Build and run it to generate certificates matching FreeRDP's expected format.

Copy the resulting `server.crt` and `server.key` to a secure location (e.g., `C:\ProgramData\OmniRDP\`). You'll reference these paths in the config below.

---

### 4. Configure `config.ini`

Open `C:\ProgramData\OmniRDP\config.ini` in a text editor. It contains a `[service]` section and one or more `[instance:<name>]` sections. At minimum, edit the `[instance:Example]` section:

```ini
[instance:Example]
enabled = true

; --- Backend connection (your Windows target) ---
backend.hostname = 192.168.1.100    ; <-- Change to your RDP server IP/hostname
backend.port = 3389                  ; Default RDP port
backend.username = myuser            ; <-- Your Windows username
backend.password = mypassword        ; <-- Your Windows password

; --- Viewer listener (where RDP clients connect) ---
viewer.bind_address = 0.0.0.0        ; 0.0.0.0 = all interfaces, 127.0.0.1 = local only
viewer.port = 3390                   ; Port viewers connect to
viewer.cert_path = C:\ProgramData\OmniRDP\server.crt   ; <-- Path to your certificate
viewer.key_path  = C:\ProgramData\OmniRDP\server.key   ; <-- Path to your private key
```

**Key settings explained:**

| Setting | What to put |
|---------|-------------|
| `backend.hostname` | IP or hostname of the Windows machine you want to multiplex |
| `backend.port` | Usually `3389` (the standard RDP port) |
| `backend.username` / `backend.password` | Credentials for that Windows machine |
| `viewer.bind_address` | `0.0.0.0` to accept connections from any network, `127.0.0.1` for local-only |
| `viewer.port` | Any free port (e.g., `3390`, `3391`, etc.) |
| `viewer.cert_path` / `viewer.key_path` | Absolute paths to your TLS certificate and key files |

Save the file. For a full list of available settings, see the [Configuration Reference](#configuration-reference) section below.

---

### 5. Start It All

**Via Windows Service** (recommended — runs in Session 0, auto-starts on boot):

The service starts automatically after installation. If you need to start it manually:

```powershell
net start OmniRDP_svc
```

Or open **Services.msc**, find `OmniRDP Service`, and click **Start**.

**Via Standalone Mode** (for testing or manual use):

```powershell
OmniRDP.exe --config C:\ProgramData\OmniRDP\config.ini
```

The service (or standalone process) reads the config, spawns an instance for each enabled `[instance:<name>]` section, and begins listening on the configured viewer ports.

**Connect an RDP client** — use any standard RDP client to connect to `viewer.bind_address:viewer.port`:

```powershell
# Windows built-in Remote Desktop Connection
mstsc.exe /v:192.168.1.10:3390

# Or from another machine on the network
mstsc.exe /v:<server-ip>:3390
```

Replace `192.168.1.10` with your server's actual IP address and `3390` with your configured viewer port.

Each viewer that connects will see the same remote desktop. Only one viewer controls input at a time (see [Input Arbitration](#input-arbitration) for details).

**To uninstall**, use **Add/Remove Programs** (or **Apps & Features**) in Windows Settings — select `OmniRDP` and click **Uninstall**. This removes all files, the Windows service, and the tray auto-start entry.

---

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
