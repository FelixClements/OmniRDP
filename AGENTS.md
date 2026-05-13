# AGENTS.md

Repository guidance for AI agents and developers working in the OmniRDP codebase.

## Repository overview

```
OmniRDP/              Main application (C, CMake)
setup/                Inno Setup installer script + license files
patches/freerdp/      Patches applied to upstream FreeRDP at build/CI time
.github/workflows/    GitHub Actions CI
freerdp-3.26.0/       NOT committed — generated locally or checked out by CI
```

OmniRDP links against FreeRDP libraries (freerdp3.dll, winpr3.dll, freerdp-client3.dll, freerdp-server3.dll) and ships them alongside its own executables.

## FreeRDP source and patch workflow

FreeRDP 3.26.0 is an **external dependency**. The full source tree is **not committed** to this repo. Instead:

1. **CI** checks out `FreeRDP/FreeRDP` ref `3.26.0` into `freerdp-3.26.0/` at build time.
2. CI applies patches from `patches/freerdp/` using `git -C freerdp-3.26.0 apply`.
3. CI then configures and builds FreeRDP with standard flags.

### Current patches

| Patch | Purpose |
|---|---|
| `0001-winpr-skip-legacy-provider-when-internal-md4.patch` | Skip loading OpenSSL legacy provider when `WITH_INTERNAL_MD4` is enabled |

### Why a patch?

OpenSSL 3.x requires the legacy provider for MD4, which is needed for NTLM/CredSSP authentication. Instead of requiring end-users to install OpenSSL with legacy provider enabled, we compile FreeRDP with internal MD4 support and skip the legacy provider load entirely. This is a small, targeted source change to upstream FreeRDP.

### Local development with FreeRDP

You need a local `freerdp-3.26.0/` checkout for building, but **do not commit it**. Instead:

```powershell
git clone https://github.com/FreeRDP/FreeRDP.git freerdp-3.26.0
Set-Location freerdp-3.26.0
git checkout 3.26.0
git apply ..\patches\freerdp\0001-winpr-skip-legacy-provider-when-internal-md4.patch
Set-Location ..
```

Then build:

```powershell
cmake -S freerdp-3.26.0 -B freerdp-3.26.0/build `
  -DWITH_INTERNAL_MD4=ON `
  -DWITH_INTERNAL_MD5=ON `
  -DWITH_INTERNAL_RC4=ON `
  -DWITH_NATIVE_SSPI=ON `
  -DWITH_AAD=OFF `
  -DWITH_KRB5=OFF
cmake --build freerdp-3.26.0/build --config Release -j
cmake --build OmniRDP/build --config Release -j
```

## FreeRDP build flags

| Flag | Value | Reason |
|---|---|---|
| `WITH_INTERNAL_MD4` | ON | NTLM/CredSSP needs MD4; avoids OpenSSL 3.x legacy provider dependency |
| `WITH_INTERNAL_MD5` | ON | NTLM/CredSSP needs MD5 |
| `WITH_INTERNAL_RC4` | ON | Legacy RDP security and licensing need RC4 |
| `WITH_NATIVE_SSPI` | ON | Windows SSPI handles Kerberos/NTLM natively for domain auth |
| `WITH_AAD` | OFF | Not needed for on-prem domain-joined VMs |
| `WITH_KRB5` | OFF | Not needed when native SSPI is enabled |

## Configuration model

OmniRDP config lives in `config.ini` with `[instance:<name>]` sections. Key groups:

### Backend VM connection (OmniRDP connects to real RDP server)

```ini
backend.hostname = 192.168.1.10
backend.port = 3389
backend.username = alice
backend.password = password
backend.domain = CONTOSO
backend.connect_timeout_ms = 30000
```

### Backend security (OmniRDP -> backend VM)

```ini
backend.security.nla_enabled = true
backend.security.tls_enabled = true
backend.security.rdp_enabled = true
backend.security.server_authentication = true
backend.security.ignore_certificate = false
```

### Viewer listener/security (clients connect to OmniRDP)

```ini
viewer.bind_address = 192.168.1.207
viewer.port = 3390
viewer.cert_path = C:\server.crt
viewer.key_path = C:\server.key
viewer.max_viewers = 10
```

### Legacy compatibility keys

```ini
security.tls_enabled = true
security.nla_enabled = false
security.server_authentication = true
security.ignore_certificate = false
```

These are retained for backward compatibility. If `backend.security.*` keys are absent, these fill in. Prefer `backend.security.*` for new configs.

### Credential handling

- `backend.username = alice` + `backend.domain = CONTOSO` — preferred
- `backend.username = alice@contoso.local` — UPN, domain left empty
- `backend.username = CONTOSO\alice` — auto-split into domain + username

## CI workflow

`.github/workflows/ci.yml` has two jobs:

1. **format-check** — clang-format on C sources
2. **windows-build-test** — one job that:
   - Checks out repo
   - Checks out upstream FreeRDP 3.26.0
   - Applies patches
   - Installs vcpkg once
   - Builds FreeRDP Debug + OmniRDP Debug
   - Builds OmniRDP ASan + runs tests on push
   - Builds FreeRDP Release + OmniRDP Release
   - Uploads Release artifacts
3. **build-installer** — downloads Release artifact, builds Inno Setup installer

### Troubleshooting CI

| Symptom | Likely cause |
|---|---|
| `CMakeLists.txt` not found in `freerdp-3.26.0` | FreeRDP checkout step failed or was removed |
| Patch fails to apply | Upstream FreeRDP changed; patch needs update for new version |
| `backend.c` format check fails | Run `clang-format -i OmniRDP/src/backend.c` |
| Installer skipped | Previous job failed; check `windows-build-test` |
| vcpkg errors | Check vcpkg cache key or dependency list |

## Preventing code-scanning warning alerts

- Avoid unsafe C library calls that GitHub code scanning flags in this repo: `strcpy`, `strncpy`, `strlen`, `memcpy`, `fopen`, `atoi`, `getenv`, and direct `fgetc` loops without robust EOF/error handling.
- Prefer bounded, checked patterns: `snprintf` with return-value/truncation checks, `strnlen_s`, `memcpy_s` or whole-struct assignment where appropriate, `fopen_s`, `strtol`/`strtoul` with `errno`, end-pointer, and range checks, and `InitializeCriticalSectionAndSpinCount`/`InitializeCriticalSectionEx` with failure handling.
- Do not commit generated `freerdp-3.26.0/` source trees, build directories, artifacts, logs, or archives; keep FreeRDP changes as small patches under `patches/freerdp/`.
- Fix code-scanning, compiler, clang-format, and relevant build/test warnings before opening PRs.
- Use `backend.security.*` config keys for new examples and changes; keep legacy `security.*` keys only for compatibility notes.

## Build outputs

| What | Local path |
|---|---|
| OmniRDP EXEs | `OmniRDP/build/Release/OmniRDP.exe` etc. |
| FreeRDP DLLs | `freerdp-3.26.0/build/libfreerdp/Release/freerdp3.dll` etc. |
| Installer | `setup/Output/OmniRDP-Setup.exe` |

## Licensing

| Component | License |
|---|---|
| OmniRDP | GNU AGPLv3 |
| FreeRDP | Apache License 2.0 |
| Installer license files | `setup/license/AGPLv3.txt`, `setup/license/Apache-2.0.txt`, `setup/license/NOTICE.txt` |

When modifying FreeRDP source via patches, add modification notices per Apache 2.0 §4(b).

## Agent rules

- Do not commit build artifacts, logs, or archives.
- Do not commit the full `freerdp-3.26.0/` source tree unless intentionally vendoring.
- Keep FreeRDP modifications as small reproducible patches in `patches/freerdp/`.
- Run or mention relevant build checks when changing C/CMake/CI files.
- Prefer `backend.security.*` over legacy `security.*` keys in new config.
- For on-prem domain VMs, do not add AAD/KRB5 dependencies.
