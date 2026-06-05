# PROJECT KNOWLEDGE BASE

**Generated:** 2026-06-04
**Commit:** 51d56ee
**Branch:** detached HEAD

## OVERVIEW

OmniRDP is a C11/CMake Windows RDP multiplexer that links against FreeRDP 3.26.0 and ships three executables: `OmniRDP`, `OmniRDP-svc`, and `OmniRDP-tray`.

## STRUCTURE

```text
OmniRDP/
|-- OmniRDP/              # CMake project, production C sources, public headers, tests
|   |-- include/          # Public API headers only; keep internals out
|   |-- src/              # Multiplexer, service, tray, pipe, config, RDPEGFX code
|   `-- tests/            # C executable tests plus CMake boundary audits
|-- patches/freerdp/      # Reproducible patches applied to upstream FreeRDP
|-- setup/                # Inno Setup installer and config template
|-- tasks/                # RDPEGFX plans, evidence, validation notes
`-- freerdp-3.26.0/       # External local checkout only; never commit
```

## WHERE TO LOOK

| Task | Location | Notes |
|---|---|---|
| Main multiplexer executable | `OmniRDP/src/main.c`, `backend.c`, `viewer_server.c` | FreeRDP client-to-backend plus listener-to-viewers path |
| Viewer publishing and framebuffer | `OmniRDP/src/viewer_publisher.c`, `viewer_framebuffer.c` | Generation, snapshots, dirty regions, classic/GFX publish decisions |
| RDPEGFX viewer path | `OmniRDP/src/viewer_gfx_pipeline.c`, `viewer_gfx_codec_*.c` | Pipeline owns FreeRDP RDPEGFX transport; codecs stay transport-free |
| Classic viewer path | `viewer_classic_queue.c`, `viewer_classic_transport.c` | Queue policy separated from FreeRDP update calls |
| Service executable | `svc_main.c`, `svc_service.c`, `svc_instance_mgr.c`, `svc_pipe_server.c` | No FreeRDP link dependency |
| Tray executable | `tray_main.c`, `tray_icon.c`, `tray_pipe_client.c` | Talks to service over named pipe protocol |
| Config model | `svc_config.c`, `ini_parser.c`, `setup/config.ini.template` | Prefer `backend.security.*` over legacy `security.*` |
| Installer | `setup/OmniRDP.iss`, `setup/build-installer-local.ps1` | Pulls built EXEs/DLLs into Inno Setup output |
| CI build logic | `.github/workflows/ci.yml` | Checks out FreeRDP, applies patches, builds Debug/ASan/Release |
| Source-specific guidance | `OmniRDP/src/AGENTS.md` | Boundaries and anti-patterns for production C |
| Test-specific guidance | `OmniRDP/tests/AGENTS.md` | Test registration, DLL PATH handling, architecture audits |

## FREERDP DEPENDENCY

FreeRDP 3.26.0 is external. CI checks out `FreeRDP/FreeRDP` ref `3.26.0` into `freerdp-3.26.0/`, applies `patches/freerdp/*.patch`, then builds shared libraries.

| Patch | Purpose |
|---|---|
| `0001-winpr-skip-legacy-provider-when-internal-md4.patch` | Skip OpenSSL legacy provider loading when internal MD4 is enabled |

Local setup:

```powershell
git clone https://github.com/FreeRDP/FreeRDP.git freerdp-3.26.0
Set-Location freerdp-3.26.0
git checkout 3.26.0
git apply ..\patches\freerdp\0001-winpr-skip-legacy-provider-when-internal-md4.patch
Set-Location ..
```

FreeRDP build flags: `WITH_INTERNAL_MD4=ON`, `WITH_INTERNAL_MD5=ON`, `WITH_INTERNAL_RC4=ON`, `WITH_NATIVE_SSPI=ON`, `WITH_AAD=OFF`, `WITH_KRB5=OFF`.

When modifying FreeRDP source via patches, keep patches small and add Apache 2.0 section 4(b) modification notices.

## CONFIGURATION MODEL

OmniRDP reads `config.ini` with `[instance:<name>]` sections.

```ini
backend.hostname = 192.168.1.10
backend.port = 3389
backend.username = alice
backend.password = password
backend.domain = CONTOSO
backend.connect_timeout_ms = 30000

backend.security.nla_enabled = true
backend.security.tls_enabled = true
backend.security.rdp_enabled = true
backend.security.server_authentication = true
backend.security.ignore_certificate = false

viewer.bind_address = 192.168.1.207
viewer.port = 3390
viewer.cert_path = C:\server.crt
viewer.key_path = C:\server.key
viewer.max_viewers = 10
```

Legacy `security.*` keys are compatibility fallback only. New examples and code paths should use `backend.security.*`.

Credential handling: `alice` plus `CONTOSO` is preferred; `alice@contoso.local` stays UPN with empty domain; `CONTOSO\alice` is auto-split.

## COMMANDS

```powershell
cmake -S freerdp-3.26.0 -B freerdp-3.26.0/build `
  -DWITH_INTERNAL_MD4=ON `
  -DWITH_INTERNAL_MD5=ON `
  -DWITH_INTERNAL_RC4=ON `
  -DWITH_NATIVE_SSPI=ON `
  -DWITH_AAD=OFF `
  -DWITH_KRB5=OFF `
  -DWITH_SERVER=ON `
  -DBUILD_SHARED_LIBS=ON

cmake --build freerdp-3.26.0/build --config Release -j

cmake -S OmniRDP -B OmniRDP/build `
  -DFREERDP_BUILD="$PWD/freerdp-3.26.0/build"
cmake --build OmniRDP/build --config Release -j
ctest --test-dir OmniRDP/build -C Release --output-on-failure
```

Format check mirrors CI:

```powershell
Get-ChildItem -Path "OmniRDP/src", "OmniRDP/include" -Recurse -Include "*.c", "*.h" |
  ForEach-Object { clang-format --dry-run --Werror $_.FullName }
```

## CI

`.github/workflows/ci.yml` runs `format-check`, `windows-build-test`, and `build-installer`. The build job checks out FreeRDP, applies patches, installs vcpkg deps, builds Debug/ASan/Release, and uploads release artifacts.

Common failures: missing `freerdp-3.26.0/CMakeLists.txt` means checkout failed; patch failures mean patch drift; format failures need clang-format; installer skips when `windows-build-test` fails.

## CODE-SCANNING RULES

Avoid patterns GitHub code scanning flags in this repo: `strcpy`, `strncpy`, raw `strlen`, raw `memcpy`, `fopen`, `atoi`, `getenv`, and direct `fgetc` loops without robust EOF/error handling.

Prefer checked patterns already used locally: `snprintf` with truncation checks, `strnlen_s`, `memcpy_s`, whole-struct assignment, `fopen_s`, `strtol`/`strtoul` with `errno`, end-pointer, and range checks, and checked `InitializeCriticalSectionAndSpinCount`/`InitializeCriticalSectionEx`.

## ANTI-PATTERNS

- Do not commit `freerdp-3.26.0/`, build directories, artifacts, logs, archives, or installer outputs.
- Do not add AAD or KRB5 dependencies for on-prem domain VM scenarios.
- Do not move FreeRDP source edits into the repo; keep reproducible patch files under `patches/freerdp/`.
- Do not expose internal viewer transport or GFX pipeline types through `OmniRDP/include`.
- Do not resurrect backend RDPEGFX replay symbols forbidden by the CMake boundary tests.

## BUILD OUTPUTS

| What | Local path |
|---|---|
| OmniRDP EXEs | `OmniRDP/build/Release/OmniRDP.exe`, `OmniRDP-svc.exe`, `OmniRDP-tray.exe` |
| FreeRDP DLLs | `freerdp-3.26.0/build/libfreerdp/Release/freerdp3.dll` and peers |
| Installer | `setup/Output/OmniRDP-Setup.exe` |

## LICENSING

OmniRDP is GNU AGPLv3. FreeRDP is Apache License 2.0. Installer license files live under `setup/license/`.
