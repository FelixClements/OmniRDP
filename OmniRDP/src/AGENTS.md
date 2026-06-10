# SOURCE KNOWLEDGE BASE

## OVERVIEW

Production C for the multiplexer, Windows service, tray app, named-pipe protocol, config, and viewer publishing pipeline.

## WHERE TO LOOK

| Task | Files | Notes |
|---|---|---|
| Process entry points | `main.c`, `svc_main.c`, `tray_main.c` | Each maps to a separate executable in parent `CMakeLists.txt` |
| Backend RDP client | `backend.c`, `backend.h` | Connects to the real VM; backend RDPEGFX decode-only work is quarantined here |
| Viewer listener facade | `viewer_server.c`, `viewer_server_internal.h`, public `../include/viewer_server.h` | Public API must stay small; internals stay in `src` |
| Viewer state and auth | `viewer_internal.c`, `viewer_auth.c` | Input ownership, slow viewer detection, auth policy helpers |
| Framebuffer/publisher | `viewer_framebuffer.c`, `viewer_publisher.c` | Own pixels, snapshots, generations, dirty decisions |
| Classic transport | `viewer_classic_queue.c`, `viewer_classic_transport.c` | Queue policy and FreeRDP update transport are split |
| RDPEGFX path | `viewer_gfx_pipeline.c`, `viewer_gfx_codec_uncompressed.c`, `viewer_gfx_codec_rfx.c` | Pipeline owns RDPEGFX transport; codecs build payloads only |
| Pointer path | `viewer_pointer.c`, `viewer_pointer_transport.c`, `pointer_shape.c` | Keep pointer transport out of `viewer_server.c` |
| Service core | `svc_service.c`, `svc_instance_mgr.c`, `svc_pipe_server.c` | Windows SCM, worker processes, stats/events pipe |
| Config and secrets | `svc_config.c`, `ini_parser.c`, `svc_dpapi.c` | DPAPI encrypts plaintext passwords in config |
| Tray UI | `tray_icon.c`, `tray_status_dlg.c`, `tray_log_viewer.c`, `tray_pipe_client.c` | Win32 tray surfaces over service pipe protocol |
| Pipe protocol | `pipe_protocol.c`, `pipe_protocol.h` | 4-byte little-endian length prefix plus JSON payload |

## TARGET BOUNDARIES

| Executable | Source set | Link shape |
|---|---|---|
| `OmniRDP` | Backend, viewer, framebuffer, publisher, config, DPAPI | Links FreeRDP client/server/core and WinPR |
| `OmniRDP-svc` | `svc_*`, `ini_parser`, `pipe_protocol` | Service-only Windows libs; no FreeRDP dependency |
| `OmniRDP-tray` | `tray_*`, `pipe_protocol`, `svc_log` | GUI subsystem; talks to service over pipe |

## CONVENTIONS

- C standard is C11; CI runs clang-format on `src` and public `include`.
- Public headers live in `../include`; internal headers stay in `src`.
- Use `BOOL` and WinPR/Windows types consistently where adjacent code does.
- Keep new config examples on `backend.security.*`; legacy `security.*` is fallback only.
- Service/tray pipe payloads are JSON strings framed by `pipe_protocol`; update both sides together.
- Prefer small helper functions over widening already-large files such as `viewer_server.c`, `backend.c`, `svc_instance_mgr.c`, and `tray_icon.c`.

## ARCHITECTURE LOCKS

- `viewer_framebuffer.*` must not mention FreeRDP peer/context, RDPEGFX transport, update callbacks, or `IFCALL`.
- GFX codec files must not call RDPEGFX server callbacks, WTS virtual channels, or peer/context APIs.
- `viewer_server.c` must not directly use RDPEGFX transport callbacks, classic update callbacks, or pointer update callbacks; dedicated transport modules own those.
- `../include/viewer_server.h` must not expose internal structs or include FreeRDP listener/RDPEGFX/WTS headers.
- Do not restore backend replay symbols such as `ViewerGfxCompleteFrame`, `ViewerGfxFrameBuffer`, `viewer_gfx_replay_frame`, `viewer_server_publish_gfx_`, or `backend_gfx_pdu_publish_allowed`.

## CODE-SCANNING STYLE

- Avoid raw `strcpy`, `strncpy`, `strlen`, `memcpy`, `fopen`, raw `snprintf`, raw `vsnprintf`, `_snprintf`, `_vsnprintf`, `atoi`, `getenv`, and naive `fgetc` loops.
- Use `fopen_s`, `safe_string.h` `omni_format`/`omni_vformat` wrappers for formatted output, `strnlen_s`, `memcpy_s`, `strtol`/`strtoul` with range checks, or whole-struct assignment.
- Check initialization results for critical sections and Windows handles; clean up handles on every error path.

## TEST EXPECTATIONS

- Source boundary changes usually need updates or new checks in `../tests`.
- RDPEGFX, framebuffer, publisher, viewer-server, and classic-transport refactors should run the CMake boundary tests, not just executable unit tests.
