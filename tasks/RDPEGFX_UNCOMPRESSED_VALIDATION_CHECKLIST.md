# RDPEGFX Uncompressed Runtime Validation Checklist

- Story: US-012
- Status: FAIL
- Date:
- Run ID:
- Tester:
- Build:
- Backend:
- Client/viewer:
- Config path: C:\ProgramData\OmniRDP\logs\vm2

Do not commit credentials, passwords, secrets, private hostnames, private keys, or sensitive screenshots/log excerpts. Redact sensitive values before attaching evidence.

## Required config

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
viewer.gfx.diagnostic.full_frame_dirty = false
backend.gfx.decode_only_enabled = false
codec.graphics_pipeline = false
```

## Scenario results

| Scenario | Result (PASS/FAIL/NOT_RUN) | Ghosting | Black fragments | Stale trails | Canvas fragmentation | Click responsiveness | Reconnect recovery if applicable | Log/evidence path | Notes |
|---|---|---|---|---|---|---|---|---|---|
| Initial baseline | PASS | PASS | PASS | PASS | PASS | PASS | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | Baseline rendered correctly. Logs show RDPEGFX negotiated successfully and framebuffer baseline sent. |
| Start menu open | PASS | PASS | PASS | PASS | PASS | PASS | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | No visible corruption reported. |
| Start menu close | PASS | PASS | PASS | PASS | PASS | PASS | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | No visible corruption reported. |
| Window drag | FAIL | NOT_REPORTED | NOT_REPORTED | NOT_REPORTED | NOT_REPORTED | FAIL | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | Retest after `9bc6b45`: very slow and froze at some point with a big screen change. Area-threshold storm resolved, but consecutive-defer full-frame fallback still occurred. |
| Window resize | FAIL/BLOCKED | PASS | PASS | PASS | PASS | FAIL | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | Retest after `9bc6b45`: visually passed, then froze with big screen change. |
| Overlapping windows | FAIL/BLOCKED | PASS | PASS | PASS | PASS | FAIL | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | Retest after `9bc6b45`: visually passed, then froze with big screen change. |
| Text editing | PASS | PASS | PASS | PASS | PASS | PASS | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | Retest after `9bc6b45`: passed. |
| Viewer reconnect | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  |  |
| Backend desktop resize | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  |  |

## Log evidence checklist

- [x] At least one accumulated pending dirty generation observed.
- [x] At least one sent dirty generation observed.
- [x] Confirmed no unintended diagnostic full-frame forcing occurred.

Runtime evidence summary from `C:\ProgramData\OmniRDP\logs\vm2`:

- Config applied: `viewer.gfx.enabled=true`, `viewer.gfx.codec=uncompressed`, `viewer.gfx.diagnostic.full_frame_dirty=false`.
- RDPEGFX negotiated successfully and framebuffer baseline was sent.
- During window drag, repeated `RDPEGFX threshold full-frame dirty fallback` with `reason=pending area threshold`, `width=1920`, `height=1080`.
- Later logs showed repeated fallback burst for the same generation `255` approximately every 8-16ms.
- Counts from searched logs: threshold fallback `885`, area threshold `885`, diagnostic full-frame dirty forced `0`, consecutive deferred dirty sends `0`.
- No pacing/backpressure/ACK-timeout messages were observed in searched logs.
- Diagnostic full-frame forcing was absent.

Retest evidence after mitigation commit `9bc6b45`:

- Initial baseline, Start menu open, Start menu close, and text editing passed.
- Window drag remained very slow and eventually froze on a big screen change.
- Window resize and overlapping windows were visually correct but blocked/failed because they froze with a big screen change.
- Viewer reconnect and backend desktop resize were not reported / NOT_RUN.
- Area-threshold storm was resolved: `pending area threshold` count `0`.
- `RDPEGFX threshold full-frame dirty fallback` count `3`, all with `reason=consecutive deferred dirty sends`.
- `pending rectangle count threshold` count `0`; `dirty overflow` count `0`; `diagnostic full-frame dirty forced` count `0`.
- No dirty ACK timeout, byte limit, in-flight limit, or frame-map-full logs were observed.
- Large dirty bursts occurred before the freeze, for example 143, 156, and 22 rectangle batches within approximately 48ms.
- Viewer eventually disconnected.

Suggested log patterns from current code:

- Pending/start/latest generation fields:
  - `pending_start_generation=`
  - `pending_latest_generation=`
  - `pending_dirty_rects=`
  - `pending_area=`
- Moved/sent generation fields:
  - `moved_batch_generation=`
  - `sent_generation=`
  - `generation=`
  - `last_sent_generation=`
- Diagnostic full-frame forcing should be absent unless deliberately testing the diagnostic flag:
  - Absence of `RDPEGFX diagnostic full-frame dirty forced`

## Limitations / blockers

US-012 remains pending because retest after `9bc6b45` still failed. The area-threshold full-frame fallback storm is resolved for uncompressed RDPEGFX. Additional mitigation implemented: uncompressed RDPEGFX now increments the consecutive-defer counter but does not force full-frame fallback for consecutive deferred dirty sends; RFX/non-uncompressed behavior remains unchanged. Runtime retest is required before marking US-012 complete.
