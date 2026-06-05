# RDS Broker Backend Options Plan

## TL;DR
> **Summary**: Add OmniRDP backend config support for RDP-file-style RDS Connection Broker/session collection routing options and map them into FreeRDP backend settings.
> **Deliverables**:
> - New typed config keys for workspace ID, redirection-server-name behavior, load-balance info, and alternate full address.
> - Backend helper that applies these options to FreeRDP settings without changing viewer/service pipe behavior.
> - Config template/service-generated template examples matching the user's RDP file fields.
> - TDD tests for parsing, backend setting application, and adjacent config regressions.
> **Effort**: Short
> **Parallel**: YES - 2 waves
> **Critical Path**: config struct/parser -> backend option helper -> instance_runner wiring -> templates/docs/tests

## Context

### Original Request
Support these backend connection options so OmniRDP can be used with a Remote Desktop Connection Broker where a user needs a specific Session Collection/Farm in a load-balanced environment:

```text
full address:s:rds-broker.domain.com
workspace id:s:rds-broker.domain.com
use redirection server name:i:1
loadbalanceinfo:s:tsv://MS Terminal Services Plugin.1.Marketing_Pool
alternate full address:s:rds-broker.domain.com
```

### Research Findings
- `OmniRDP/src/svc_config.c` parses `[instance:<name>]` keys into fixed-size `InstanceConfig` fields from `OmniRDP/src/svc_config.h`.
- `OmniRDP/src/instance_runner.c` converts `InstanceConfig` into `BackendSecurityConfig` and calls `backend_configure` before `backend_connect`.
- `OmniRDP/src/backend.c:backend_configure` writes `FreeRDP_ServerHostname`, `FreeRDP_ServerPort`, credentials, security, and timeout.
- FreeRDP 3.26 command line maps `/load-balance-info:<value>` to `FreeRDP_LoadBalanceInfo` via `freerdp_settings_set_pointer_len(..., strlen(value))`.
- FreeRDP 3.26 command line maps `/server-name:<value>` to `FreeRDP_UserSpecifiedServerName`.
- FreeRDP 3.26 RDP-file parser recognizes `full address`, `alternate full address`, `loadbalanceinfo`, and `use redirection server name`; its comment says alternate full address wins like MSTSC.
- No verified FreeRDP 3.26 setting for `workspace id` was found in the explored parser path; retain it as typed config/template parity, but do not push it into FreeRDP unless a verified key is found during implementation.

### Metis Review (gaps addressed)
Metis agent tooling is unavailable in this session. Manual gap review identified and resolved:
- Ambiguity: whether alternate full address is cosmetic or connection-targeting. Resolution: follow FreeRDP/MSTSC behavior; alternate full address overrides full address when non-empty.
- Ambiguity: whether workspace ID affects FreeRDP transport settings. Resolution: parse and preserve, but no backend setting mapping without a verified FreeRDP key.
- Risk: adding raw RDP-file key names to `config.ini` would conflict with repo dotted-key conventions. Resolution: use `backend.rdp_file.*` keys and document RDP-file equivalence.

## Work Objectives

### Core Objective
A configured OmniRDP instance can connect through an RDS broker to a specific collection/farm by supplying load-balance info and related RDP-file-style backend options.

### Deliverables
- `InstanceConfig` fields:
  - `char backend_rdp_workspace_id[256];`
  - `int backend_rdp_use_redirection_server_name;`
  - `char backend_rdp_loadbalanceinfo[1024];`
  - `char backend_rdp_alternate_full_address[256];`
- Backend API type and helper in `OmniRDP/include/backend.h` / `OmniRDP/src/backend.c`:
  - `typedef struct BackendRdpFileOptions { const char *workspace_id; BOOL use_redirection_server_name; const char *loadbalanceinfo; const char *alternate_full_address; } BackendRdpFileOptions;`
  - `BOOL backend_apply_rdp_file_options(BackendClient *client, const BackendRdpFileOptions *options);`
- `instance_runner.c` wiring after `backend_configure` and before `backend_connect`.
- Template updates in `setup/config.ini.template`, `svc_service.c`, and `tray_main.c` generated examples.
- Tests proving parsing and FreeRDP settings mapping.

### Definition of Done
- `ctest --test-dir OmniRDP/build -C Debug -R "test_svc_config|test_backend_rdp_file_options" --output-on-failure` passes after RED->GREEN.
- `ctest --test-dir OmniRDP/build -C Debug -R "test_refactor_boundaries|test_public_viewer_server_facade" --output-on-failure` passes.
- Manual QA tmux transcript shows the newly built config test executable parsing a broker config successfully.
- No FreeRDP vendoring, no AAD/KRB5 dependency, no viewer listener behavior changes.

### Must Have
- Preserve existing `backend.hostname` behavior when `backend.rdp_file.alternate_full_address` is empty.
- When `backend.rdp_file.alternate_full_address` is non-empty, apply it as actual backend `FreeRDP_ServerHostname` and store it in `client->hostname`.
- When `backend.rdp_file.loadbalanceinfo` is non-empty, set `FreeRDP_LoadBalanceInfo` and `FreeRDP_LoadBalanceInfoLength` to the exact byte string and length, with no null terminator included.
- When `backend.rdp_file.use_redirection_server_name = true` and an effective host exists, set `FreeRDP_UserSpecifiedServerName` to the effective host.
- `workspace_id` must parse and round-trip in `InstanceConfig`; no FreeRDP setting mapping unless verified.

### Must NOT Have
- Do not add AAD/KRB5/RD Gateway behavior.
- Do not accept raw unsafe C library calls prohibited by root `AGENTS.md`.
- Do not modify viewer/public API boundaries except adding backend-only API in `include/backend.h`.
- Do not require a live RDS broker for automated tests.

## Verification Strategy

> ZERO HUMAN INTERVENTION - all verification is agent-executed.

- Test decision: TDD RED-GREEN-REFACTOR using existing CMake C tests.
- Manual QA policy: tmux channel for CLI/data-shaped behavior.
- Evidence paths:
  - `.omo/evidence/task-1-config-red.txt`
  - `.omo/evidence/task-1-config-green.txt`
  - `.omo/evidence/task-2-backend-red.txt`
  - `.omo/evidence/task-2-backend-green.txt`
  - `.omo/evidence/task-3-manual-qa.txt`
  - `.omo/evidence/final-ctest.txt`

## Execution Strategy

### Parallel Execution Waves
Wave 1: Task 1 and Task 2 can start as TDD scaffolds in parallel if the implementer coordinates the shared `BackendRdpFileOptions` type name first.
Wave 2: Task 3 depends on Task 1 and Task 2.
Wave 3: Task 4 depends on Task 1 names and Task 3 wiring.
Final: full verification and review.

### Dependency Matrix
| Task | Blocks | Blocked By |
|---|---|---|
| 1. Config parsing | 3, 4 | none |
| 2. Backend settings helper | 3 | none |
| 3. Instance runner wiring | final QA | 1, 2 |
| 4. Templates/docs | final QA | 1 |

## TODOs

- [ ] 1. Add typed config fields and parser support

  **What to do**: First add failing assertions in `OmniRDP/tests/test_svc_config.c` for a config containing the four new `backend.rdp_file.*` keys. Then add fields to `InstanceConfig`, defaults in `svc_config_default_instance`, and parser copies in `parse_one_instance` using existing `strcpy_safe` and `ini_get_bool` patterns.

  **Config keys**:
  ```ini
  backend.rdp_file.workspace_id = rds-broker.domain.com
  backend.rdp_file.use_redirection_server_name = true
  backend.rdp_file.loadbalanceinfo = tsv://MS Terminal Services Plugin.1.Marketing_Pool
  backend.rdp_file.alternate_full_address = rds-broker.domain.com
  ```

  **Must NOT do**: Do not add raw RDP-file key names with spaces to OmniRDP config. Do not treat `workspace_id` as required.

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: 3, 4 | Blocked By: none

  **References**:
  - Pattern: `OmniRDP/src/svc_config.h` - fixed-size `InstanceConfig` string fields.
  - Pattern: `OmniRDP/src/svc_config.c:svc_config_default_instance` - defaults for instance fields.
  - Pattern: `OmniRDP/src/svc_config.c:parse_one_instance` - `strcpy_safe` and `ini_get_bool` config parsing.
  - Test pattern: `OmniRDP/tests/test_svc_config.c` - local config writer helpers and direct `InstanceConfig` assertions.

  **Acceptance Criteria**:
  - [ ] RED captured: `ctest --test-dir OmniRDP/build -C Debug -R test_svc_config --output-on-failure` fails because new fields are absent/default.
  - [ ] GREEN captured: same command passes after parser implementation.
  - [ ] Existing invalid port and viewer auth assertions still pass.

  **QA Scenarios**:
  ```text
  Scenario: Broker config parses all RDP-file-style keys
    Tool: tmux
    Steps: tmux new-session -d -s ulw-qa-config "cd C:/Users/localadmin/.codex/worktrees/eed3/OmniRDP && ctest --test-dir OmniRDP/build -C Debug -R test_svc_config --output-on-failure"; tmux capture-pane -pt ulw-qa-config -S -200
    Expected: transcript contains "100% tests passed" or the single `test_svc_config` pass result.
    Evidence: .omo/evidence/task-1-config-green.txt

  Scenario: Empty optional RDP-file-style keys keep defaults
    Tool: tmux
    Steps: same test command after adding assertions for omitted keys.
    Expected: transcript shows `test_svc_config` pass and assertions confirm empty strings/false boolean.
    Evidence: .omo/evidence/task-1-config-defaults.txt
  ```

  **Commit**: YES | Message: `feat(config): parse backend RDP file broker options` | Files: `OmniRDP/src/svc_config.h`, `OmniRDP/src/svc_config.c`, `OmniRDP/tests/test_svc_config.c`

- [ ] 2. Add backend FreeRDP settings helper

  **What to do**: Write a new failing test executable `test_backend_rdp_file_options.c` that creates a `BackendClient`, calls `backend_configure` with baseline host `rds-broker.domain.com`, calls `backend_apply_rdp_file_options`, and reads `client->context->settings` to assert FreeRDP settings. Then add `BackendRdpFileOptions` and `backend_apply_rdp_file_options`.

  **Exact mapping**:
  - Effective host = `options->alternate_full_address` when non-empty, else existing `client->hostname`/`FreeRDP_ServerHostname`.
  - If effective host comes from alternate full address, call `freerdp_settings_set_string(settings, FreeRDP_ServerHostname, effective_host)`, update `client->hostname`, and keep `client->port` unchanged.
  - If `options->loadbalanceinfo` non-empty, call `freerdp_settings_set_pointer_len(settings, FreeRDP_LoadBalanceInfo, options->loadbalanceinfo, strlen(options->loadbalanceinfo))` and verify `FreeRDP_LoadBalanceInfoLength`.
  - If `options->use_redirection_server_name` is true and effective host non-empty, call `freerdp_settings_set_string(settings, FreeRDP_UserSpecifiedServerName, effective_host)`.
  - Ignore `workspace_id` after null/empty validation; keep parameter for future verified mapping.

  **Must NOT do**: Do not include the null terminator in `LoadBalanceInfoLength`. Do not use `strdup` without checking allocation if new ownership is introduced. Do not set `FreeRDP_RedirectionTsvUrl`; that is server-redirection runtime state, not initial load-balance info.

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: 3 | Blocked By: none

  **References**:
  - Pattern: `OmniRDP/src/backend.c:backend_configure` - settings write pattern.
  - API: FreeRDP 3.26 command-line `/load-balance-info` uses `FreeRDP_LoadBalanceInfo` pointer length with `strlen`.
  - API: FreeRDP 3.26 command-line `/server-name` uses `FreeRDP_UserSpecifiedServerName`.
  - Test CMake pattern: `OmniRDP/tests/CMakeLists.txt` existing `test_backend_gfx_decode_only` and backend include dirs.

  **Acceptance Criteria**:
  - [ ] RED captured: `ctest --test-dir OmniRDP/build -C Debug -R test_backend_rdp_file_options --output-on-failure` fails before helper implementation.
  - [ ] GREEN captured: same command passes after helper implementation.
  - [ ] Test asserts exact loadbalance byte length equals `strlen("tsv://MS Terminal Services Plugin.1.Marketing_Pool")`.
  - [ ] Test asserts alternate full address overrides `FreeRDP_ServerHostname`.
  - [ ] Test asserts `FreeRDP_UserSpecifiedServerName` is set only when boolean enabled.

  **QA Scenarios**:
  ```text
  Scenario: Backend helper applies RDS broker routing settings
    Tool: tmux
    Steps: tmux new-session -d -s ulw-qa-backend "cd C:/Users/localadmin/.codex/worktrees/eed3/OmniRDP && ctest --test-dir OmniRDP/build -C Debug -R test_backend_rdp_file_options --output-on-failure"; tmux capture-pane -pt ulw-qa-backend -S -200
    Expected: transcript contains `test_backend_rdp_file_options` pass.
    Evidence: .omo/evidence/task-2-backend-green.txt

  Scenario: No optional options preserves existing backend behavior
    Tool: tmux
    Steps: same test command; include test case with all option fields NULL/empty.
    Expected: ServerHostname remains baseline host, LoadBalanceInfoLength remains 0, UserSpecifiedServerName remains unset/empty.
    Evidence: .omo/evidence/task-2-backend-defaults.txt
  ```

  **Commit**: YES | Message: `feat(backend): apply RDS broker routing settings` | Files: `OmniRDP/include/backend.h`, `OmniRDP/src/backend.c`, `OmniRDP/tests/test_backend_rdp_file_options.c`, `OmniRDP/tests/CMakeLists.txt`

- [ ] 3. Wire parsed options into instance startup

  **What to do**: In `instance_runner.c`, build a `BackendRdpFileOptions` from `inst->backend_rdp_*` fields immediately after `backend_configure` succeeds and before `backend_set_gfx_decode_only`/`backend_connect`. Call `backend_apply_rdp_file_options`; on failure log an error, scrub password, free backend/config, and return failure like adjacent setup failures.

  **Must NOT do**: Do not log passwords or load-balance cookies. Logging the boolean and whether a load-balance string is present is acceptable; do not print the raw `loadbalanceinfo` value unless existing log policy allows config details.

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: final QA | Blocked By: 1, 2

  **References**:
  - Pattern: `OmniRDP/src/instance_runner.c` around `backend_configure` and `backend_set_gfx_decode_only` - error handling and password scrubbing.
  - Security: root `AGENTS.md` code-scanning rules.

  **Acceptance Criteria**:
  - [ ] Build succeeds for `OmniRDP` after wiring.
  - [ ] Existing `backend_configure` behavior remains intact for configs without new keys.
  - [ ] Failure path frees resources and calls `SecureZeroMemory(password, sizeof(password))` before return.

  **QA Scenarios**:
  ```text
  Scenario: Instance startup path accepts parsed broker options
    Tool: tmux
    Steps: tmux new-session -d -s ulw-qa-wire "cd C:/Users/localadmin/.codex/worktrees/eed3/OmniRDP && cmake --build OmniRDP/build --config Debug --target OmniRDP -- /m"; tmux capture-pane -pt ulw-qa-wire -S -200
    Expected: transcript contains successful target build with no compiler errors.
    Evidence: .omo/evidence/task-3-wire-build.txt

  Scenario: Adjacent no-option config path still builds/tests
    Tool: tmux
    Steps: run `ctest --test-dir OmniRDP/build -C Debug -R test_svc_config --output-on-failure`.
    Expected: transcript shows pass for existing baseline config tests.
    Evidence: .omo/evidence/task-3-regression.txt
  ```

  **Commit**: YES | Message: `feat(instance): pass broker options to backend` | Files: `OmniRDP/src/instance_runner.c`

- [ ] 4. Update templates and operator-facing examples

  **What to do**: Add commented broker/session-collection examples near backend connection settings in `setup/config.ini.template`, `svc_service.c` generated template, and `tray_main.c` default config creation. Include the user's exact RDP-file equivalents in comments and the OmniRDP key names below them.

  **Must NOT do**: Do not make broker options required. Do not alter legacy `security.*` compatibility notes.

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: final QA | Blocked By: 1

  **References**:
  - Pattern: `setup/config.ini.template` backend section.
  - Pattern: `OmniRDP/src/svc_service.c:svc_config_write_template` backend config string.
  - Pattern: `OmniRDP/src/tray_main.c` generated config string.

  **Acceptance Criteria**:
  - [ ] Templates include `backend.rdp_file.workspace_id`, `backend.rdp_file.use_redirection_server_name`, `backend.rdp_file.loadbalanceinfo`, and `backend.rdp_file.alternate_full_address`.
  - [ ] Examples show `tsv://MS Terminal Services Plugin.1.Marketing_Pool` exactly.
  - [ ] Existing `backend.security.*` examples remain preferred and unchanged.

  **QA Scenarios**:
  ```text
  Scenario: Template contains broker option block
    Tool: tmux
    Steps: tmux new-session -d -s ulw-qa-template "cd C:/Users/localadmin/.codex/worktrees/eed3/OmniRDP && rg -n 'backend\.rdp_file\.(workspace_id|use_redirection_server_name|loadbalanceinfo|alternate_full_address)|Marketing_Pool' setup/config.ini.template OmniRDP/src/svc_service.c OmniRDP/src/tray_main.c"; tmux capture-pane -pt ulw-qa-template -S -200
    Expected: transcript contains all four key names and `Marketing_Pool` in each template surface.
    Evidence: .omo/evidence/task-4-template.txt

  Scenario: No accidental raw RDP-file keys in config parser
    Tool: tmux
    Steps: run `rg -n 'full address|workspace id|use redirection server name|alternate full address' OmniRDP/src/svc_config.c OmniRDP/src/svc_config.h`.
    Expected: no matches in parser/header; raw names appear only in template comments/docs.
    Evidence: .omo/evidence/task-4-no-raw-parser-keys.txt
  ```

  **Commit**: YES | Message: `docs(config): document RDS broker backend options` | Files: `setup/config.ini.template`, `OmniRDP/src/svc_service.c`, `OmniRDP/src/tray_main.c`

## Final Verification Wave

- [ ] F1. RED->GREEN Evidence Audit
  - Confirm evidence files include failing output before production changes for `test_svc_config` and `test_backend_rdp_file_options`.
  - Confirm passing output after implementation for the same test ids.

- [ ] F2. Full Focused Test Run
  ```powershell
  ctest --test-dir OmniRDP/build -C Debug -R "test_svc_config|test_backend_rdp_file_options|test_refactor_boundaries|test_public_viewer_server_facade" --output-on-failure
  ```
  Evidence: `.omo/evidence/final-ctest.txt`

- [ ] F3. Manual QA Transcript
  ```powershell
  tmux new-session -d -s ulw-qa-final "cd C:/Users/localadmin/.codex/worktrees/eed3/OmniRDP && ctest --test-dir OmniRDP/build -C Debug -R test_backend_rdp_file_options --output-on-failure"
  tmux capture-pane -pt ulw-qa-final -S -200 > .omo/evidence/final-manual-qa.txt
  tmux kill-session -t ulw-qa-final
  ```
  PASS if transcript contains `test_backend_rdp_file_options` pass.

- [ ] F4. Cleanup Receipt
  ```powershell
  tmux ls
  git status --short --untracked-files=all
  ```
  Evidence: `.omo/evidence/final-cleanup.txt`; PASS if no `ulw-qa-*` sessions remain and only intended source/test/template files are changed.

- [ ] F5. Reviewer Gate
  Because this touches 3+ files, run a final review agent if available. If unavailable, perform a manual review using this checklist:
  - No unsafe C calls introduced.
  - No secrets/loadbalanceinfo printed in logs.
  - No FreeRDP source/vendor changes.
  - No public viewer API boundary changes.
  - New tests fail before implementation and pass after.

## Commit Strategy

Default: do not commit unless user approves. Suggested atomic commits:
1. `feat(config): parse backend RDP file broker options`
2. `feat(backend): apply RDS broker routing settings`
3. `feat(instance): pass broker options to backend`
4. `docs(config): document RDS broker backend options`

Each commit should build and pass its relevant tests before the next commit.

## Success Criteria

- [ ] A config using the user's RDS broker/session collection values is parsed into `InstanceConfig`.
- [ ] Backend settings contain the exact load-balance TSV string as `FreeRDP_LoadBalanceInfo` bytes.
- [ ] Alternate full address overrides backend host like MSTSC/FreeRDP RDP-file parsing.
- [ ] `use_redirection_server_name=true` sets `FreeRDP_UserSpecifiedServerName` to the effective host.
- [ ] Existing non-broker configs still parse and connect with `backend.hostname`.
- [ ] Templates teach operators how to translate the five RDP-file lines into OmniRDP keys.
