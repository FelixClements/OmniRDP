# Active Codebase

This file describes the **current active implementation** in `OmniRDP/`.

## Project identity

- Folder on disk: `OmniRDP/`
- CMake project name: `OmniRDP`
- Binary names: `OmniRDP.exe`, `OmniRDP-svc.exe`, `OmniRDP-tray.exe`
- Language: C11
- Main dependency: local `../freerdp-3.26.0`

## Directory layout

```text
OmniRDP/
├── CMakeLists.txt
├── compile_flags.txt
├── PLAN.md                  — Service manager architecture plan
├── include/
│   ├── backend.h            — BackendClient API and types
│   ├── platform_compat.h    — Cross-platform helpers
│   └── viewer_server.h      — ViewerServer, Viewer, MonitorLayout types
├── src/
│   ├── main.c               — Standalone CLI entrypoint + --instance dispatch
│   ├── backend.c             — FreeRDP client to Windows target
│   ├── viewer_server.c       — FreeRDP server for viewer connections
│   ├── viewer_internal.c     — Pure helper logic (input policy, caps, late-join, slow viewer)
│   ├── viewer_internal.h     — Internal helpers header
│   ├── pointer_shape.c       — Pointer shape cache
│   ├── pointer_shape.h       — Pointer shape types
│   ├── platform_compat.c     — Sleep, timestamps, signal handling, cert/key paths
│   │
│   │  Service Manager:
│   ├── svc_main.c            — Service entry point (--install/--uninstall/--run)
│   ├── svc_service.c/h       — SCM integration, install, uninstall, service lifecycle
│   ├── svc_instance_mgr.c/h  — Child process spawn, monitor, restart, reload
│   ├── svc_config.c/h        — INI config loading, validation, defaults
│   ├── svc_log.c/h           — Thread-safe file logging with rotation
│   ├── svc_dpapi.c/h         — DPAPI password encrypt/decrypt + in-file encryption
│   ├── ini_parser.c/h        — Minimal INI file parser (~220 lines)
│   ├── instance_runner.c     — --instance mode entry point for child processes
│   │
│   │  Named Pipe IPC:
│   ├── pipe_protocol.c/h     — Wire format (4-byte LE length + UTF-8 JSON), framing
│   ├── svc_pipe_server.c/h   — Service-side named pipe server with DACL
│   ├── tray_pipe_client.c/h  — Tray-side named pipe client
│   │
│   │  Tray App:
│   └── tray_main.c           — Tray app entry point, message loop, auto-start
│   └── tray_icon.c/h          — System tray icon, context menu, polling, discovery
│   └── tray_status_dlg.c/h    — Status window with ListView table and action buttons
│   └── tray_log_viewer.c/h    — Built-in log viewer window with auto-refresh
└── tests/
    ├── CMakeLists.txt
    ├── test_late_join_policy.c
    ├── test_pointer_shape.c
    └── test_viewer_state.c
```

## Runtime model

### Standalone mode (`OmniRDP.exe` without --instance)
1. Backend side: connect to one Windows RDP target using FreeRDP client APIs
2. Viewer side: accept multiple incoming viewer connections using FreeRDP server APIs
3. Process fans out a single backend desktop to many viewers

### Service mode (`OmniRDP-svc.exe` spawns `OmniRDP.exe --instance`)
1. Service loads config.ini, creates named pipe server for tray IPC
2. For each enabled instance: decrypt password via DPAPI, create anonymous pipe, spawn child
3. Child (`OmniRDP.exe --instance <name> --secrets-handle <handle> --config <path>`) reads password from pipe, connects backend, starts viewer server
4. Service monitors child health via heartbeat named pipes, auto-restarts on crash

### Tray mode (`OmniRDP-tray.exe`)
1. Enumerates SCM for `OmniRDP-*` services
2. Connects to each service's named pipe
3. Polls for instance state, updates tray icon, provides context menu and status window

## Three executables

| Binary | Purpose | Links |
|--------|---------|-------|
| `OmniRDP.exe` | Standalone multiplexer + service child instance | FreeRDP 3.26.0 (client+server) |
| `OmniRDP-svc.exe` | Windows Service manager | advapi32, crypt32, kernel32, wtsapi32 |
| `OmniRDP-tray.exe` | System tray application | comctl32, shell32, advapi32, kernel32, wtsapi32 |

## IPC protocol

- **Wire format**: 4-byte LE length prefix + UTF-8 JSON payload
- **Transport**: Named pipes (`\\.\pipe\<service>_Pipe`)
- **Commands** (tray → service): list_instances, start_instance, stop_instance, restart_instance, reload_config, get_logs
- **Push messages** (service → tray): stats, event
- **Security**: DACL restricting to SYSTEM, NetworkService, and interactive user

## Dependency and build coupling

- Source dependency: `../freerdp-3.26.0`
- Generated include dependency: `../freerdp-3.26.0/build`
- Linked libraries: freerdp3, freerdp-client3, freerdp-server3, winpr3 (OmniRDP.exe only)
- Service and tray targets have zero FreeRDP dependencies
