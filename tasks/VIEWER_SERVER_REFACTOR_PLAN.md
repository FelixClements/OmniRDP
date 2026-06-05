# Viewer Server Refactor Status and Boundary Rules

This document records the current RDPEGFX/viewer-server refactor architecture. The prerequisite extraction plan is complete through US-014; this file now describes the enforced module boundaries rather than a future extraction sequence.

## Current architecture

```text
viewer_server.c
  connection/session coordinator, viewer lifecycle, high-level dispatch

viewer_framebuffer.c/.h
  canonical pixels, dirty regions, generation tracking, snapshots

viewer_publisher.c/.h
  publish/backlog/coalescing policy decisions and metrics

viewer_classic_queue.c/.h
  mechanical classic update queue storage and ownership

viewer_classic_transport.c/.h
  classic FreeRDP BitmapUpdate/SurfaceBits/SurfaceFrameMarker sends

viewer_gfx_pipeline.c/.h
  viewer-local RDPEGFX caps, reset/surface/map, frame epochs, ACK/backpressure,
  baseline/dirty sequencing, and late-join coordinator actions

viewer_gfx_codec_uncompressed.c/.h
  uncompressed RDPEGFX surface-command payload construction only
```

The canonical framebuffer is the source of truth for viewer output. Backend RDPEGFX callbacks are decode/layout/refresh inputs only; backend GFX PDU replay to viewers has been removed and is statically blocked from returning.

## Completed refactor status

- Backend RDPEGFX callback quarantine is complete: backend code must not publish or replay viewer GFX PDUs.
- Backend replay production symbols and late-join replay behavior are removed. GFX late join uses a canonical framebuffer baseline and dirty updates.
- Viewer-local RDPEGFX FrameAcknowledge, late-join, reset/surface/map, resize, epoch, and byte-backpressure state lives in `viewer_gfx_pipeline`.
- `viewer_server.h` is a public facade. Full viewer/server internals live in private implementation headers or C files.
- Classic update queue mechanics, backlog policy, and direct FreeRDP classic sends have been extracted from `viewer_server.c`.
- RDPEGFX capability confirmation is whitelist-based for the current uncompressed-only output path.
- Late-joining GFX viewers receive pointer/cursor baseline state after a successful framebuffer baseline.
- US-014 final static boundary audit is registered with CTest.

## Ownership rules

1. `viewer_server.c` coordinates sessions only. It must not own direct RDPEGFX sends or direct classic FreeRDP update sends.
2. `viewer_framebuffer` owns pixels, dirty regions, generations, and snapshots only. It must not include FreeRDP peer/context ownership, RDPEGFX send APIs, classic send commands, or transport locks.
3. `viewer_gfx_codec_uncompressed` owns data encoding only. It may use RDPEGFX data structures/constants such as `RDPGFX_SURFACE_COMMAND` and `RDPGFX_CODECID_UNCOMPRESSED`, but must not own sessions, peers, virtual channels, or `rdpgfx->` sends.
4. `viewer_gfx_pipeline` owns viewer-local RDPEGFX protocol/session sequencing and exposes narrow coordinator results to `viewer_server.c`.
5. `viewer_classic_transport` owns classic FreeRDP sends and update locking.
6. Public headers stay narrow; internals should remain in private headers or `.c` files.
7. Backend GFX remains decode-only and must not derive viewer output from backend RDPEGFX PDUs.
8. Viewer GFX remains controlled by viewer configuration and must not mirror backend GFX caps/settings.
9. A slow or no-ACK viewer must not block backend decode or unrelated viewers.

## Static enforcement tests

The following CTest-backed static checks enforce the boundaries:

- `test_no_backend_gfx_replay` / `CheckNoBackendGfxReplay.cmake`: scans production `include/`, `src/*.c`, and `src/*.h` for removed replay/publish resurrection tokens.
- `test_backend_gfx_quarantine` / `CheckBackendGfxQuarantine.cmake`: checks `src/backend.c` for forbidden backend-to-viewer GFX publish/replay/pipeline-send tokens while still allowing decode-only RDPEGFX callbacks.
- `test_viewer_server_classic_transport_boundary` / `CheckViewerServerClassicTransportBoundary.cmake`: blocks direct classic FreeRDP send/update-lock tokens in `src/viewer_server.c`.
- `test_public_viewer_server_facade` / `CheckPublicViewerServerFacade.cmake`: blocks internal/server/RDPEGFX transport types from leaking through `include/viewer_server.h`.
- `test_refactor_boundaries` / `CheckRefactorBoundaries.cmake`: final audit covering production replay resurrection, backend quarantine, RDPEGFX and classic sends in `viewer_server.c`, framebuffer send/context ownership, codec send/session ownership, and public facade drift.

These checks intentionally scan production code only unless a check is specifically about documentation or test harness behavior.

## Remaining validation: US-015

US-015 remains pending and is limited to real-client/manual/end-to-end evidence. It should record commands, configuration, logs/screenshots where useful, pass/fail results, and follow-up stories for any failures. It should cover classic first viewer, late viewer, reconnect, resize, viewer GFX baseline/dirty updates, backend GFX decode-only behavior, no-ACK/slow-viewer isolation, late-join pointer/cursor state, and package/installer validation where available.

## Operational checklist for future changes

- Keep new production code inside the owning module listed above.
- Do not reintroduce backend replay symbols, replay late-join states/actions, or backend PDU publishing to viewers.
- Do not add RDPEGFX send calls to `viewer_server.c`.
- Do not add direct classic sends to `viewer_server.c`.
- Keep framebuffer and codec modules free of transport/session ownership.
- Run `git diff --check`, Debug build, and CTest after boundary-affecting changes.
