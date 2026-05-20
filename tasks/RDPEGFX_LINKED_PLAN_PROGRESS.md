# RDPEGFX linked-plan progress tracker

This tracker links `RDPEGFX_Implementation_Plan.md` to `VIEWER_SERVER_REFACTOR_PLAN.md`.

Primary rule: continue RDPEGFX work from `RDPEGFX_Implementation_Plan.md`, but every phase must obey `VIEWER_SERVER_REFACTOR_PLAN.md` module boundaries. `viewer_server.c` remains a high-level coordinator. Orchestrator agents coordinate stories/progress; bounded fixer/Ralph implementation agents may edit code/tests/CMake within a story.

## Current status

### Completed or substantially in place

- `US-001` is completed/accepted and is the current commit target.
  - Scope completed: RDPEGFX lifecycle and CapsAdvertise/CapsConfirm seam finalized with explicit server-owned fallback/join orchestration from pipeline result flags.
  - Review: oracle accepted the behavior and module-boundary direction.
  - Validation completed:
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `git diff --check`
- `US-002` is completed/accepted.
  - Scope completed: documented canonical framebuffer pixel-format, stride, orientation, alpha, and ownership invariants; added missing tests for mark-dirty accumulation, mark-dirty overflow, snapshot-free reset behavior, and generation non-increment on mark-dirty.
  - Boundary check: no live viewer traffic was routed through the framebuffer; framebuffer remains independent from FreeRDP send paths and RDPEGFX protocol context ownership.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_framebuffer.h" "OmniRDP/tests/test_viewer_framebuffer.c"`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `git diff --check`
- `US-003` is completed/accepted.
  - Scope completed: decoded GDI framebuffer ingestion validates dirty rectangles before canonical framebuffer update. Invalid/out-of-bounds rectangles are dropped; if no valid dirty rectangles remain, the update falls back to a full-frame dirty update for correctness and to preserve delivery semantics.
  - Boundary check: no classic viewer delivery behavior, backend/viewer RDPEGFX enablement, publisher/coalescing policy, backend PDU replay authority, or FreeRDP send calls in `viewer_framebuffer` were added.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_framebuffer.h" "OmniRDP/src/viewer_framebuffer.c" "OmniRDP/src/viewer_server.c" "OmniRDP/tests/test_viewer_framebuffer.c"`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `python -m json.tool "prd.json" > $null`
    - `git diff --check`
- Skeleton modules are present and wired:
  - `OmniRDP/include/viewer_framebuffer.h`
  - `OmniRDP/src/viewer_framebuffer.c`
  - `OmniRDP/include/viewer_publisher.h`
  - `OmniRDP/src/viewer_publisher.c`
  - `OmniRDP/include/viewer_gfx_pipeline.h`
  - `OmniRDP/src/viewer_gfx_pipeline.c`
  - `OmniRDP/include/viewer_gfx_codec_uncompressed.h`
  - `OmniRDP/src/viewer_gfx_codec_uncompressed.c`
- Canonical framebuffer and publisher modules exist with tests.
- RDPEGFX viewer lifecycle seam was extracted so `viewer_gfx_pipeline.c/.h` owns narrow lifecycle scaffolding including `RdpgfxServerContext` create/free/init/uninit/open and message handling wrappers.
- RDPEGFX CapsAdvertise/CapsConfirm mechanics were moved to `viewer_gfx_pipeline.c/.h`.
- The module-static orchestration callback design was cleaned up: pipeline reports explicit caps-result actions; `viewer_server.c` performs server-owned fallback/join orchestration explicitly.
- Existing validation after the latest code extraction passed:
  - `cmake --build "OmniRDP/build" --config Debug -j`
  - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
  - `git diff --check` passed with only LF-to-CRLF warnings.

### Still disabled by default

- Viewer RDPEGFX output remains disabled by default.
- Backend RDPEGFX remains disabled by default.
- No new codec behavior should be assumed active.

## Module boundary rules

- `viewer_server.c`: high-level coordination only: listener setup, peer connect/disconnect, authentication, viewer list/lifecycle, and calls into modules.
- `viewer_framebuffer.c/.h`: canonical pixels, dimensions, stride/pixel format, generation, dirty regions, snapshots. No FreeRDP send calls.
- `viewer_publisher.c/.h`: coalescing, pacing, generation tracking, queued update/byte policy, per-viewer dispatch decisions. No `RdpgfxServerContext` ownership.
- `viewer_gfx_pipeline.c/.h`: RDPEGFX session/protocol sequencing: context lifecycle, channel open/message handling, caps confirm, reset/create/map, frames, resize/reset session state, fallback state. Frame acknowledgement callback migration is not allowed until a separate reviewed story explicitly scopes it; today it remains outside the current pipeline ownership boundary.
- `viewer_gfx_codec_uncompressed.c/.h`: uncompressed RDPEGFX payload construction only. No protocol/session ownership.

## Non-goals for the linked-plan sequence

- Do not enable viewer or backend GFX by default.
- Do not mirror backend RDPEGFX capabilities to viewers.
- Do not use backend RDPEGFX PDU replay as the authoritative late-join model.
- Existing backend-PDU replay/ring code remains transitional in `viewer_server.c`; do not mix it into the canonical framebuffer/publisher path.
- Do not rewrite listener, authentication, classic lifecycle, frame queues, or backend activation outside a specific bounded story.
- Do not implement RFX, ClearCodec, H.264, AVC420, AVC444, AVC444v2, or progressive codecs before the uncompressed GFX MVP is correct.
- Do not hold framebuffer locks while encoding or sending.
- Do not allow one slow viewer to block backend decode or other viewers.

## Ralph story order

The Ralph-ready story backlog is in repository root `prd.json` because no Ralph-specific location was present in this repository. It uses the Ralph schema with `project`, `name`, `branchName`, `description`, and `userStories` fields. Story IDs use `US-001` style and include `dependsOn` arrays.

Recommended next stories:

1. `US-001` — completed/accepted; current commit target.
2. `US-002` — completed/accepted; framebuffer invariants documented and tested.
3. `US-003` — completed/accepted; decoded GDI framebuffer dirty rects are validated before canonical framebuffer update.
4. `US-004` — add publisher generation metrics without changing delivery.
5. `US-005` — publish classic full-frame baseline from framebuffer for late join.
6. `US-006` — add classic latest-state coalescing policy. Risk: split queue limits and dirty-region coalescing if it becomes too large.
7. `US-007` — add viewer RDPEGFX activation state placeholders.
8. `US-008` — build uncompressed RDPEGFX surface command from snapshot.
9. `US-009` — send disabled-by-default RDPEGFX full-frame activation baseline. Risk: split reset/surface setup, encode, and send/fallback if needed.
10. `US-010` — add RDPEGFX dirty-region incremental updates with ack pacing.
11. `US-011` — add experimental backend GFX decode-only gate.

Backend GFX (`US-011`) is intentionally deferred later than the original implementation-plan order. The current sequence prioritizes the canonical framebuffer/viewer MVP and avoids mixing backend PDU replay with the new publisher path.

## Per-story validation checklist

- Review the story's source documents and module boundary notes.
- Keep the story small enough for one Ralph iteration.
- Run `clang-format -i` on modified C/H files.
- Run `git diff --check`.
- Run `cmake --build "OmniRDP/build" --config Debug -j`; if it cannot run, document the missing local dependency/build blocker.
- Run `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`; if tests are unavailable, document the reason.
- Update this progress tracker after each completed story.

## Open risks and follow-ups

- `viewer_server.h` still exposes substantial transitional GFX structures. Prefer narrow/opaque APIs in future bounded refactors, but do not churn public internals without a concrete story.
- Existing backend-PDU replay/ring code remains transitional. Leave it isolated until the framebuffer/publisher model replaces the late-join path.
- Mid-session GFX-to-classic fallback may not be protocol-safe for all clients; prefer pre-activation fallback or per-viewer reconnect/degrade behavior unless explicitly tested.
- Backend GFX must remain decode-only and independent from viewer GFX negotiation.
