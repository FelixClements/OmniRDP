# RDPEGFX Uncompressed Runtime Validation Checklist

- Story: US-012
- Status: NOT_RUN
- Date:
- Run ID:
- Tester:
- Build:
- Backend:
- Client/viewer:
- Config path:

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
| Initial baseline | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Start menu open | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Start menu close | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Window drag | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Window resize | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Overlapping windows | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Text editing | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Viewer reconnect | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  |  |
| Backend desktop resize | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  |  |

## Log evidence checklist

- [ ] At least one accumulated pending dirty generation observed.
- [ ] At least one sent dirty generation observed.
- [ ] Confirmed no unintended diagnostic full-frame forcing occurred.

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

US-012 remains pending until runtime validation is executed against a real backend VM and real RDP viewer/client lab using the required config above. This checklist records the required evidence but does not itself validate runtime behavior.
