# RDPEGFX remaining refactor progress tracker

This tracker belongs to the Ralph loop in root `prd.json` with branch `ralph/rdpegfx-remaining-refactor-work`.

Source PRD: `tasks/prd-rdpegfx-remaining-refactor-work.md`

Archived prior loop evidence:

- `tasks/archived-loops/2026-05-22-rdpegfx-framebuffer-publisher/prd.json`
- `tasks/archived-loops/2026-05-22-rdpegfx-framebuffer-publisher/RDPEGFX_LINKED_PLAN_PROGRESS.md`

## Current status

US-001 through US-014 are complete. Do not mark another story complete until its implementation, tests, and validation checklist pass and `prd.json` is updated with `passes: true` for that story.

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
11. `US-011` — complete — Add frame epoch and backpressure-safe ACK handling. Viewer dirty frame mappings now include epochs and payload bytes; reset/invalidation/baseline/wrap boundaries clear in-flight frame/byte state and advance epochs; nonzero ACKs only update accepted ACK and dirty pacing on current-epoch matches; frame ID wrap skips zero; and dirty sends are gated by an internal 4 MiB per-viewer byte limit. Tests cover stale ACK protection, baseline/invalidation epoch clearing, byte backpressure and ACK release, zero/unknown ACK compatibility, and wrap behavior. Debug build and CTest passed on 2026-05-23.
12. `US-012` — complete — Harden resize/reset GFX sequencing. Resize/surface invalidation now acts as a strict per-viewer GFX epoch boundary, clearing dirty/ACK/surface state and disabling dirty updates until a fresh canonical framebuffer ResetGraphics/CreateSurface/Map/full-frame baseline succeeds. Already-live RDPEGFX viewers with an invalidated surface now schedule that baseline through `viewer_gfx_pipeline_step_join`; stale ACKs and dimension-mismatch dirty snapshots are deferred/ignored without recording frames. Tests cover idle live resize, in-flight dirty resize with stale ACK, late join during resize, and dimension mismatch deferral. Debug build and CTest passed on 2026-05-23.
13. `US-013` — complete — Add pointer and cursor late-join baseline. Pending RDPEGFX late-join framebuffer baseline success now returns a pointer-baseline action and `viewer_server.c` sends a forced pointer snapshot only after that framebuffer baseline succeeds; live resize/canonical refresh baselines and failed baselines do not request pointer baseline. RDPEGFX pending activation suppresses the immediate forced pointer send while classic activation remains unchanged. Backend pointer snapshot copy API deep-copies active shape data under the backend pointer lock before viewer transport sends it. Tests cover pending-vs-live baseline action split plus forced custom/default/hidden pointer transport planning and forced position despite matching generations. Debug build and CTest passed on 2026-05-23.
14. `US-014` — complete — Add final boundary audit enforcement. Added CTest `test_refactor_boundaries` covering production replay resurrection, backend GFX quarantine, RDPEGFX/classic sends in `viewer_server.c`, framebuffer send/context ownership, codec send/session ownership, and public facade drift. Existing backend replay, backend quarantine, and classic transport static checks were strengthened. Debug build and CTest passed on 2026-05-24.
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
- `viewer_server.c` must not contain direct RDPEGFX send calls or direct classic FreeRDP send/update-lock calls.
- `viewer_framebuffer` and `viewer_gfx_codec_uncompressed` must remain free of transport/session/context ownership; CTest `test_refactor_boundaries` enforces these module boundaries.
