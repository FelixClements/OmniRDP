# ULW Notepad: RDS Broker Backend Options

## Skills
- omo:ulw-loop: requested execution mode; full workflow read. CLI/subagent tools unavailable, so evidence is recorded in files directly.
- omo:programming: inspected; not directly applicable to C files, but TDD/post-write discipline retained.

## Scope
- Surfaces: service config parser, backend API/settings, instance runner wiring, tests CMake, setup/service/tray templates.
- Files expected: svc_config.h/c, backend.h/c, instance_runner.c, tests/test_svc_config.c, tests/test_backend_rdp_file_options.c, tests/CMakeLists.txt, setup/config.ini.template, svc_service.c, tray_main.c.
- Existing dirty files before work: .gitignore, AGENTS.md, OmniRDP/src/AGENTS.md, OmniRDP/tests/AGENTS.md, .omo/plans/rds-broker-backend-options.md.
- Build dirs absent: OmniRDP/build and OmniRDP/build-debug not present. Verification may need fresh configure or be blocked by missing local FreeRDP checkout.

## Binding Criteria
1. Config parsing: test_svc_config covers populated and omitted backend.rdp_file.* keys; RED then GREEN evidence.
2. Backend settings: test_backend_rdp_file_options covers loadbalanceinfo bytes/length, alternate hostname override, UserSpecifiedServerName toggle, empty defaults; RED then GREEN evidence.
3. Wiring/templates: instance_runner passes parsed options; templates contain operator-facing keys; focused tests and tmux transcript evidence.
4. Regression: refactor/public facade checks run if build environment exists; otherwise record exact blocker.

## Evidence Ledger
- Pending.
