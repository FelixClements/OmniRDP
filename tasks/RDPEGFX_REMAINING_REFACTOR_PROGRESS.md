# RDPEGFX remaining refactor progress tracker

This tracker belongs to the Ralph loop in root `prd.json` with branch `ralph/rdpegfx-remaining-refactor-work`.

Source PRD: `tasks/prd-rdpegfx-remaining-refactor-work.md`

Archived prior loop evidence:

- `tasks/archived-loops/2026-05-22-rdpegfx-framebuffer-publisher/prd.json`
- `tasks/archived-loops/2026-05-22-rdpegfx-framebuffer-publisher/RDPEGFX_LINKED_PLAN_PROGRESS.md`

## Current status

US-001 through US-010 are complete. Do not mark another story complete until its implementation, tests, and validation checklist pass and `prd.json` is updated with `passes: true` for that story.

## Story backlog

1. `US-001` — complete — Quarantine backend RDPEGFX callbacks. Backend RDPEGFX callbacks no longer publish viewer GFX PDUs; CTest `test_backend_gfx_quarantine` enforces this boundary. Debug build and CTest passed on 2026-05-22.
2. `US-002` — complete — Remove backend GFX replay production code. Production replay symbols, backend GFX publish APIs, replay-dependent late-join states/actions, replay ACK release behavior, and replay policy tests were removed; late-joining GFX viewers use the canonical framebuffer baseline and dirty-update path only. CTest `test_no_backend_gfx_replay` enforces forbidden production replay patterns. Debug build and CTest passed on 2026-05-22.
3. `US-003` — complete — Move remaining FrameAcknowledge handling into pipeline. The RDPEGFX FrameAcknowledge callback and remaining ACK bookkeeping now live in `viewer_gfx_pipeline`; `viewer_server.c` no longer writes ACK/presented timestamp state. Debug build and CTest passed on 2026-05-22.
4. `US-004` — complete — Move canonical late-join GFX state/actions into pipeline. Remaining viewer-local canonical late-join activation/live/fallback transitions and coordinator action decisions now live in `viewer_gfx_pipeline`; `viewer_server.c` coordinates framebuffer baseline sends and classic fallback side effects from narrow pipeline actions. Tests cover activation actions, baseline step/finish, and fallback boundaries without replay ACK release. Debug build and CTest passed on 2026-05-22.
5. `US-005` — complete — Move viewer-local RDPEGFX surface/reset state into pipeline. Activation no longer copies negotiated/backend dimensions into viewer GFX surface state; canonical framebuffer snapshots drive ResetGraphics/CreateSurface/MapSurfaceToOutput/full-frame baseline dimensions and successful baselines enable dirty updates. Backend layout changes now invalidate pipeline surface/dirty state and wait for the next canonical snapshot. Tests cover activation, baseline dimensions/recording, dirty denial before baseline and dimension mismatch, and invalidation. Debug build and CTest passed on 2026-05-23.
6. `US-006` — complete — Shrink `viewer_server.h` public surface. `viewer_server.h` is now a public facade for lifecycle/config/backend-publish APIs with opaque server/backend declarations; internal viewer, RDPEGFX negotiation/pipeline state, queues, and full server storage moved to `src/viewer_server_internal.h`. `MonitorLayout` moved to `include/monitor_layout.h` for backend/main compatibility. CTest `test_public_viewer_server_facade` enforces forbidden internal tokens in the public facade. Debug build and CTest passed on 2026-05-23.
7. `US-007` — complete — Extract classic queue data structures.
8. `US-008` — complete — Move classic backlog policy out of viewer server. Classic BitmapUpdate publish, SurfaceBits publish, pump full-refresh/drop, and latest-state threshold decisions now use pure `viewer_publisher` decision APIs; `viewer_server.c` applies side effects, queue operations, refresh requests, counters/logging, snapshots, and classic sends. `viewer_classic_queue` remains mechanical. Debug build and CTest passed on 2026-05-23.
9. `US-009` — complete — Move classic FreeRDP sends into transport module. Classic BitmapUpdate chunking/validation, SurfaceBits sends, SurfaceFrameMarker sends, FreeRDP update batching, and send counters/timing now live in `viewer_classic_transport`; `viewer_server.c` coordinates queues/policy/pump only through a narrow transport context. CTest `test_viewer_server_classic_transport_boundary` enforces that direct classic FreeRDP send/update-lock tokens do not return to `viewer_server.c`. Debug build and CTest passed on 2026-05-23.
10. `US-010` — complete — Implement strict RDPEGFX capabilities whitelist. Caps selection now admits only known-good official uncompressed-compatible RDPEGFX versions with an explicit safe flag mask, rejects unknown/future/AVC and other unsupported flags, downgrades to lower supported advertised caps, and avoids canonical poisoning from unsupported viewers. Unit tests cover supported selection, unsupported versions/flags, downgrade, canonical whitelist/mismatch behavior, and unsupported-first isolation. Debug build and CTest passed on 2026-05-23.
11. `US-011` — pending — Add frame epoch and backpressure-safe ACK handling.
12. `US-012` — pending — Harden resize/reset GFX sequencing.
13. `US-013` — pending — Add pointer and cursor late-join baseline.
14. `US-014` — pending — Add final boundary audit enforcement.
15. `US-015` — pending — Record end-to-end RDPEGFX validation evidence.

## Per-story validation checklist

- Keep the story small and ordered according to `prd.json`.
- Do not enable viewer or backend GFX by default.
- Do not route backend RDPEGFX PDU replay to viewer output.
- Do not move backend replay into another production module.
- Do not link deprecated/test-only backend replay paths into production binaries.
- Do not add RFX, ClearCodec, H.264, AVC, AVC444, AVC420, or progressive codec work.
- Run `clang-format -i` on modified C/H files.
- Run `git diff --check`.
- Run `cmake --build "OmniRDP/build" --config Debug -j`; if it cannot run, document the missing local dependency/build blocker.
- Run `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`; if tests are unavailable, document the reason.
- Run `python -m json.tool "prd.json" > $null` when `prd.json` changes.
- Do not commit generated/build artifacts.
- Update this tracker after each completed story.

## Boundary rules

- Canonical framebuffer remains the viewer source of truth.
- `viewer_server.c` should coordinate only; RDPEGFX protocol/session/ACK/surface/reset state belongs in `viewer_gfx_pipeline` or narrow internal modules.
- `viewer_framebuffer` and `viewer_gfx_codec_uncompressed` must not own send/context behavior.
- Backend GFX remains default-off and decode-only.
- Viewer GFX remains controlled only by `viewer.gfx.enabled` and must not derive caps/settings from backend GFX.
- Backend RDPEGFX callbacks must not call any `viewer_server_publish_gfx_*` function.
- Production code must not contain `ViewerGfxCompleteFrame`, `ViewerGfxFrameBuffer`, or `viewer_gfx_replay_frame` after `US-002`.
- Replay-dependent late-join states/actions and replay ACK release behavior were removed with `US-002`; later ACK/late-join stories must cover only canonical framebuffer baseline and dirty-update responsibilities.
