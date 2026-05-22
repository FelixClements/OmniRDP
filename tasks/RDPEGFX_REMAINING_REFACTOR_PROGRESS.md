# RDPEGFX remaining refactor progress tracker

This tracker belongs to the Ralph loop in root `prd.json` with branch `ralph/rdpegfx-remaining-refactor-work`.

Source PRD: `tasks/prd-rdpegfx-remaining-refactor-work.md`

Archived prior loop evidence:

- `tasks/archived-loops/2026-05-22-rdpegfx-framebuffer-publisher/prd.json`
- `tasks/archived-loops/2026-05-22-rdpegfx-framebuffer-publisher/RDPEGFX_LINKED_PLAN_PROGRESS.md`

## Current status

All stories are pending. Do not mark a story complete until its implementation, tests, and validation checklist pass and `prd.json` is updated with `passes: true` for that story.

## Story backlog

1. `US-001` — pending — Quarantine backend RDPEGFX callbacks.
2. `US-002` — pending — Remove backend GFX replay production code.
3. `US-003` — pending — Move FrameAcknowledge handling into pipeline.
4. `US-004` — pending — Move late-join GFX state/actions into pipeline.
5. `US-005` — pending — Move viewer-local RDPEGFX surface/reset state into pipeline.
6. `US-006` — pending — Shrink `viewer_server.h` public surface.
7. `US-007` — pending — Extract classic queue data structures.
8. `US-008` — pending — Move classic backlog policy out of viewer server.
9. `US-009` — pending — Move classic FreeRDP sends into transport module.
10. `US-010` — pending — Implement strict RDPEGFX capabilities whitelist.
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
