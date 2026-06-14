# OmniRDP

OmniRDP is a Windows RDP multiplexer: it connects to one backend Windows RDP
session and exposes a viewer listener that multiple RDP clients can connect to.
All viewers see the same desktop, while input is arbitrated so only one viewer
controls the backend at a time.

The project builds three executables:

| Binary | Purpose |
| --- | --- |
| `OmniRDP.exe` | Multiplexer process. Runs standalone or as a service child instance. |
| `OmniRDP-svc.exe` | Windows service manager for one or more configured instances. |
| `OmniRDP-tray.exe` | System tray app for service status, instance control, and logs. |

OmniRDP uses FreeRDP client and server libraries. CI builds FreeRDP from the
default branch of the [`FelixClements/FreeRDP`](https://github.com/FelixClements/FreeRDP)
fork into a neutral `freerdp` checkout path, applies the OmniRDP FreeRDP patch,
then builds OmniRDP Debug, ASan, and Release configurations.

## Latest Release

Download the current installer or portable package from
[OmniRDP v0.8](https://github.com/FelixClements/OmniRDP/releases/tag/v0.8).

v0.8 includes:

- ClearCodec viewer GFX support.
- RDPEGFX capability downgrade handling for viewer compatibility.
- CI builds against the `FelixClements/FreeRDP` fork default branch.
- CMake support for deriving the FreeRDP source root from `FREERDP_BUILD`, so
  builds can use any external FreeRDP checkout path.

## Features

### Multiplexer

- Multiple viewers can watch the same backend RDP session.
- One viewer at a time owns input; idle timeout releases the input lock.
- Passive viewers still see the current pointer position.
- Multi-monitor backend sessions are supported.
- Late-joining viewers receive a current-state refresh.
- Slow viewers can be disconnected or, when configured, resynchronized from the
  latest framebuffer baseline.

### Display Paths

- Classic RDP bitmap path using SurfaceBits/BitmapUpdate style publishing.
- Canonical framebuffer used for snapshots, late join, and viewer publishing.
- Optional viewer RDPEGFX path.
- Viewer GFX codecs: `uncompressed`, `rfx`, and `clearcodec`.
- RDPEGFX capability selection supports downgrading to compatible viewer caps.

Viewer GFX remains experimental and is controlled by `viewer.gfx.enabled`.
Backend RDPEGFX decode-only settings are independent from viewer GFX.

### Service And Tray

- `OmniRDP-svc.exe` runs as a Windows service in Session 0.
- A single service can manage multiple backend/viewer instances from one config.
- Child instance health is monitored over named pipe heartbeat messages.
- Crashed instances can restart with backoff.
- `OmniRDP-tray.exe` discovers OmniRDP services and shows status, controls, and
  logs from the user session.
- Passwords are passed to child processes without putting them on the command
  line and are persisted with DPAPI encryption after first load.

## Download And Install

### Installer

1. Download `OmniRDP-Setup.exe` from
   [GitHub Releases](https://github.com/FelixClements/OmniRDP/releases).
2. Run the installer as Administrator.
3. Edit `C:\ProgramData\OmniRDP\config.ini`.
4. Start or restart the `OmniRDP_svc` service.
5. Run `OmniRDP-tray.exe` from the Start menu or startup entry.

The installer places the executables and required DLLs in
`C:\Program Files\OmniRDP`, installs the service, installs the tray app startup
entry, and creates a default config if one does not already exist.

### Portable Zip

The `windows-OmniRDP.zip` release asset contains the three executables and
runtime DLLs. Keep the DLLs next to the executables or add their directory to
`PATH`.

## First Configuration

Edit `C:\ProgramData\OmniRDP\config.ini`. The default template contains
`[service]`, `[instances]`, and `[instance:<name>]` sections.

Minimal example:

```ini
[service]
log_level = info
log_dir = C:\ProgramData\OmniRDP\logs
pipe_name = OmniRDP_ServicePipe

[instances]
names = Example

[instance:Example]
enabled = true

backend.hostname = 192.168.1.10
backend.port = 3389
backend.username = localadmin
backend.password = password
backend.domain =
backend.connect_timeout_ms = 30000

backend.security.nla_enabled = true
backend.security.tls_enabled = true
backend.security.rdp_enabled = true
backend.security.server_authentication = true
backend.security.ignore_certificate = false

viewer.bind_address = 0.0.0.0
viewer.port = 3390
viewer.max_viewers = 10
viewer.cert_path = C:\ProgramData\OmniRDP\server.crt
viewer.key_path = C:\ProgramData\OmniRDP\server.key

viewer.security.nla_enabled = true
viewer.security.tls_enabled = true
viewer.security.rdp_enabled = true
viewer.auth.mode = backend_credentials

display.monitor_count = 1
display.monitor_width = 1920
display.monitor_height = 1080
display.color_depth = 32
```

Use `backend.security.*` for backend VM security settings. Legacy
`security.*` keys are still accepted as compatibility fallback, but new configs
should prefer `backend.security.*`.

### Viewer Authentication

Viewer-side authentication is separate from backend authentication.

| `viewer.auth.mode` | Behavior |
| --- | --- |
| `none` | Accept viewer sessions without checking MSTSC credentials in OmniRDP. |
| `backend_credentials` | Require MSTSC credentials to match `backend.username` and `backend.password`. |

NLA is performed by the OmniRDP listener host before OmniRDP's application
logon callback runs. If viewers authenticate with backend-local credentials
from another VM, use:

```ini
viewer.security.nla_enabled = false
viewer.security.tls_enabled = true
viewer.auth.mode = backend_credentials
```

## Certificates

Viewer connections use TLS when `viewer.security.tls_enabled = true`. Provide a
certificate and key and reference them with `viewer.cert_path` and
`viewer.key_path`.

Create a self-signed certificate with OpenSSL:

```powershell
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=OmniRDP"
```

FreeRDP also includes a `makecert` tool under
`<freerdp-source>\winpr\tools\makecert\`.

## Running

### Windows Service

The installer registers the service automatically. To manage it manually:

```powershell
net start OmniRDP_svc
net stop OmniRDP_svc
```

Install manually from a portable build:

```powershell
OmniRDP-svc.exe --install
OmniRDP-svc.exe --uninstall
```

Install a named service with a custom config:

```powershell
OmniRDP-svc.exe --install --service-name "OmniRDP-Prod" --config "C:\OmniRDP\prod\config.ini"
```

### Tray App

```powershell
OmniRDP-tray.exe
OmniRDP-tray.exe --install
OmniRDP-tray.exe --uninstall
```

### Standalone Multiplexer

For manual testing:

```powershell
OmniRDP.exe --config C:\ProgramData\OmniRDP\config.ini
```

Legacy positional mode is also available:

```text
OmniRDP <hostname> <port> <username> <password> [domain] [monitors]
```

## Connecting Viewers

Connect any RDP client to the configured viewer listener:

```powershell
mstsc.exe /v:<omni-host>:3390
```

Example:

```powershell
mstsc.exe /v:192.168.1.50:3390
```

Only the viewer holding the input lock can send keyboard and mouse input to the
backend. Other viewers remain passive until the lock is released.

## Configuration Reference

### `[service]`

| Key | Default | Description |
| --- | --- | --- |
| `log_level` | `info` | `debug`, `info`, `warn`, or `error`. |
| `log_dir` | `C:\ProgramData\OmniRDP\logs` | Base log directory. |
| `log_max_size_mb` | `10` | Max log file size before rotation. |
| `log_max_files` | `5` | Number of rotated log files to keep. |
| `pipe_name` | `OmniRDP_ServicePipe` | Named pipe for tray/service IPC. |
| `heartbeat_timeout_sec` | `10` | Child heartbeat timeout. |
| `graceful_shutdown_sec` | `10` | Graceful shutdown wait. |
| `health_poll_interval_sec` | `2` | Instance health poll interval. |
| `instance_startup_delay_ms` | `500` | Delay between starting instances. |

### `[instances]`

| Key | Description |
| --- | --- |
| `names` | Comma-separated list of instance names. Each name needs a matching `[instance:<name>]` section. |

### `[instance:<name>]`

| Key | Default | Description |
| --- | --- | --- |
| `enabled` | `true` | Enable this instance. |
| `backend.hostname` | Required | Backend RDP host/IP. |
| `backend.port` | `3389` | Backend RDP port. |
| `backend.username` | Required | Backend username. |
| `backend.password` | Required | Backend password. Plaintext is DPAPI-encrypted after load. |
| `backend.domain` | empty | Backend domain. `DOMAIN\user` is auto-split; UPNs stay intact. |
| `backend.connect_timeout_ms` | `30000` | Backend connection timeout. |
| `backend.gfx.decode_only_enabled` | `false` | Experimental backend RDPEGFX decode-only mode. |
| `backend.gfx.rfx_enabled` | `false` | Experimental backend RemoteFX candidate mode. |
| `backend.rdp_file.workspace_id` | empty | Optional RDS routing workspace ID. |
| `backend.rdp_file.use_redirection_server_name` | `false` | Optional RDS redirection setting. |
| `backend.rdp_file.loadbalanceinfo` | empty | Optional RDS collection routing value. |
| `backend.rdp_file.alternate_full_address` | empty | Optional RDS alternate full address. |
| `backend.security.nla_enabled` | `true` | Enable NLA to backend. |
| `backend.security.tls_enabled` | `true` | Enable TLS to backend. |
| `backend.security.rdp_enabled` | `true` | Enable standard RDP security to backend. |
| `backend.security.server_authentication` | `true` | Validate backend server identity. |
| `backend.security.ignore_certificate` | `false` | Ignore backend certificate errors. |
| `reconnect.enabled` | `true` | Reconnect backend after disconnect. |
| `reconnect.max_attempts` | `10` | Max reconnect attempts. |
| `reconnect.initial_delay_ms` | `1000` | Initial reconnect delay. |
| `reconnect.max_delay_ms` | `60000` | Max reconnect delay. |
| `reconnect.backoff_multiplier` | `2.0` | Reconnect backoff multiplier. |
| `viewer.bind_address` | `127.0.0.1` | Listener address for viewers. |
| `viewer.port` | Required | Listener port for viewers. |
| `viewer.max_viewers` | `10` | Max simultaneous viewers. |
| `viewer.cert_path` | empty | TLS certificate path. |
| `viewer.key_path` | empty | TLS private key path. |
| `viewer.security.nla_enabled` | `true` | Enable NLA on viewer listener. |
| `viewer.security.tls_enabled` | `true` | Enable TLS on viewer listener. |
| `viewer.security.rdp_enabled` | `true` | Enable standard RDP security on viewer listener. |
| `viewer.auth.mode` | `backend_credentials` | `none` or `backend_credentials`. |
| `viewer.slow_disconnect_enabled` | `true` | Disconnect slow viewers. |
| `viewer.slow_lag_interval_ms` | `5000` | Slow-viewer lag check interval. |
| `viewer.slow_disconnect_after_ms` | `30000` | Slow-viewer disconnect threshold. |
| `viewer.late_join_timeout_ms` | `15000` | Late-join refresh timeout. |
| `viewer.late_join_refresh_deadline_ms` | `5000` | Deadline for backend refresh during late join. |
| `viewer.late_join_replay_max_frames` | `4` | Max replay frames for late join. |
| `viewer.throttle_max_updates_per_sec` | `0` | Classic update throttle; `0` disables. |
| `viewer.gfx.enabled` | `false` | Enable experimental viewer RDPEGFX path. |
| `viewer.gfx.codec` | `uncompressed` | `uncompressed`, `rfx`, or `clearcodec`. |
| `viewer.gfx.rfx_threading_enabled` | `false` | Enable threaded RFX encoding. |
| `viewer.gfx.dirty_max_in_flight_frames` | `1` | Viewer GFX dirty frame pacing limit. |
| `viewer.gfx.dirty_max_in_flight_bytes` | `4194304` | Viewer GFX dirty byte pacing limit. |
| `viewer.gfx.diagnostic.full_frame_dirty` | `false` | Send every GFX dirty update as full-frame dirty for diagnostics. |
| `viewer.classic_latest_state_enabled` | `false` | Allow stale classic queue entries to be replaced by latest framebuffer state. |
| `viewer.classic_latest_state_max_queue_depth` | `0` | Queue-depth trigger for latest-state replacement; `0` disables. |
| `viewer.classic_latest_state_max_queue_bytes` | `0` | Queue-byte trigger for latest-state replacement; `0` disables. |
| `display.monitor_count` | `1` | Number of monitors. |
| `display.monitor_width` | `1920` | Monitor width. |
| `display.monitor_height` | `1080` | Monitor height. |
| `display.color_depth` | `32` | Color depth. |
| `codec.nscodec` | `true` | Backend/client NSCodec setting. |
| `codec.remote_fx` | `true` | Backend/client RemoteFX setting. |
| `codec.graphics_pipeline` | `false` | Reserved backend graphics pipeline toggle. |
| `codec.h264` | `false` | H.264 setting. |
| `codec.avc444` | `false` | AVC444 setting. |
| `codec.avc444v2` | `false` | AVC444v2 setting. |
| `codec.frame_acknowledge` | `4` | Frame acknowledge setting. |

## Build From Source

### Prerequisites

- Windows with Visual Studio 2022 Build Tools or IDE.
- CMake 3.20 or newer.
- vcpkg with `openssl`, `libjpeg-turbo`, `libpng`, `zlib`, and `libusb` for
  `x64-windows`.
- A FreeRDP checkout built before OmniRDP.

### Build FreeRDP

```powershell
git clone https://github.com/FelixClements/FreeRDP.git freerdp

cmake -S freerdp -B freerdp/build `
  -DWITH_INTERNAL_MD4=ON `
  -DWITH_INTERNAL_MD5=ON `
  -DWITH_INTERNAL_RC4=ON `
  -DWITH_NATIVE_SSPI=ON `
  -DWITH_AAD=OFF `
  -DWITH_KRB5=OFF `
  -DWITH_SERVER=ON `
  -DWITH_SHADOW=OFF `
  -DWITH_PROXY=OFF `
  -DBUILD_TESTING=OFF `
  -DBUILD_SHARED_LIBS=ON `
  -DWITH_X11=OFF `
  -DWITH_WAYLAND=OFF `
  -DWITH_SDL2=OFF `
  -DWITH_CAIRO=OFF `
  -DWITH_FFMPEG=OFF `
  -DWITH_SWSCALE=OFF `
  -DWITH_MEDIA_FOUNDATION=OFF `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -G "Visual Studio 17 2022" `
  -A x64

cmake --build freerdp/build --config Release -j
```

### Build OmniRDP

```powershell
cmake -S OmniRDP -B OmniRDP/build `
  -DFREERDP_BUILD="$PWD/freerdp/build" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -G "Visual Studio 17 2022" `
  -A x64

cmake --build OmniRDP/build --config Release -j
ctest --test-dir OmniRDP/build -C Release --output-on-failure
```

### Build Installer Locally

After a Release build, use the installer helper:

```powershell
.\setup\build-installer-local.ps1
```

The installer is written to `setup\Output\OmniRDP-Setup.exe`.

## CI

The main workflow is `.github/workflows/ci.yml`.

It runs:

- `format-check`
- `windows-build-test`
- `build-installer`

The Windows build job:

1. Checks out this repository.
2. Checks out `FelixClements/FreeRDP` default branch into `freerdp`.
3. Applies `patches/freerdp/*.patch`.
4. Installs vcpkg dependencies.
5. Builds FreeRDP Debug and Release.
6. Builds OmniRDP Debug, ASan, and Release.
7. Runs ASan app smoke checks on push builds.
8. Uploads the portable app artifact.
9. Builds and uploads the Inno Setup installer.

## Architecture

```text
User Session (interactive desktop)
|-- OmniRDP-tray.exe
|   |-- tray icon and status window
|   |-- service discovery
|   |-- instance start/stop/restart commands
|   `-- log viewer
|
`-- named pipe IPC
    `-- \\.\pipe\<configured pipe_name>

Session 0 (Windows service)
|-- OmniRDP-svc.exe
|   |-- SCM integration
|   |-- config loading and hot reload
|   |-- instance process manager
|   |-- heartbeat monitoring
|   `-- named pipe server
|
|-- OmniRDP.exe --instance office-desktop
|   |-- backend FreeRDP client -> Windows target
|   |-- viewer FreeRDP server -> RDP viewers
|   |-- input arbitration
|   |-- canonical framebuffer
|   |-- classic viewer publishing
|   `-- optional viewer RDPEGFX publishing
|
`-- OmniRDP.exe --instance lab-server
    |-- backend FreeRDP client -> Windows target
    `-- viewer FreeRDP server -> RDP viewers
```

Each enabled `[instance:<name>]` runs in its own child `OmniRDP.exe` process.
The service owns process lifetime and health monitoring; the tray app never
talks directly to child instances. Viewer clients connect to each instance's
configured `viewer.bind_address` and `viewer.port`.

Display state flows through a canonical framebuffer. The classic path publishes
bitmap updates to viewers, while the optional viewer RDPEGFX path publishes from
the same current-state framebuffer using the configured `viewer.gfx.codec`.
Backend RDPEGFX decode-only support is separate from viewer RDPEGFX publishing.

## Repository Layout

```text
OmniRDP/
|-- OmniRDP/              # CMake project, production sources, public headers, tests
|   |-- include/          # Public API headers
|   |-- src/              # Multiplexer, service, tray, pipe, config, GFX code
|   `-- tests/            # C executable tests and CMake boundary audits
|-- patches/freerdp/      # Reproducible patches applied to FreeRDP in CI
|-- setup/                # Inno Setup installer and config template
|-- scripts/              # Smoke, benchmark, and validation helpers
|-- tasks/                # RDPEGFX plans and validation notes
`-- freerdp/              # External FreeRDP checkout used by CI/local builds; do not commit
```

## Security Notes

- Passwords in config are encrypted with Windows DPAPI after first load.
- Backend credentials are passed to child processes over inherited handles, not
  on the command line.
- The service and tray communicate over a named pipe.
- Viewer TLS depends on the certificate and key configured for each instance.
- Use `backend.security.ignore_certificate = false` for production backends
  unless you intentionally accept backend certificate errors.

## License

OmniRDP is licensed under the GNU AGPLv3. FreeRDP is Apache License 2.0.
