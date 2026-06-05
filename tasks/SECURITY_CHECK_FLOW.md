# Security Check Fix Flow

Use this flow when fixing security/static-analysis findings in OmniRDP. The goal is to use GitHub as the source of truth, fix the root cause locally, then let CI confirm the alert is closed.

## 1. Fetch alerts from GitHub

Find the repository name:

```powershell
git remote -v
```

Fetch all open code-scanning alerts:

```powershell
gh api --paginate `
  -H "Accept: application/vnd.github+json" `
  -H "X-GitHub-Api-Version: 2022-11-28" `
  "/repos/OWNER/REPO/code-scanning/alerts?per_page=100&state=open" `
  > code-scanning-warnings.json
```

Useful filters:

```text
severity=critical|high|medium|low|warning|note|error
state=open|closed|dismissed|fixed
tool_name=CodeQL
ref=refs/heads/main
```

For OmniRDP, focus on alerts in committed project code:

```text
OmniRDP/src
OmniRDP/include
OmniRDP/tests
```

Do not fix generated/external code such as `freerdp-3.26.0/`; FreeRDP changes belong in small reproducible patches under `patches/freerdp/`.

## 2. Group alerts before editing

Do not fix alerts one-by-one blindly. Group them by:

- rule id/name
- file path
- unsafe API or repeated pattern
- shared helper/function/root cause

Example jq summary:

```powershell
Get-Content code-scanning-warnings.json | ConvertFrom-Json |
  Group-Object { "$($_.rule.id) :: $($_.most_recent_instance.location.path)" } |
  Sort-Object Count -Descending |
  Select-Object Count, Name
```

Typical warning classes in this repo:

- unsafe C calls: `strcpy`, `strncpy`, `strlen`, `memcpy`, `fopen`, `atoi`, `getenv`, direct `fgetc` loops
- unchecked lock setup: `InitializeCriticalSection`
- cppcheck cleanup findings: unread variables, redundant assignments, overly broad variable scope

## 3. Inspect the code and choose the real fix

Read the surrounding function, not just the flagged line. Check:

- buffer sizes and truncation behavior
- whether the input is config/user/pipe/file data
- ownership and cleanup paths for allocated memory
- whether an init failure can leave an object partially usable
- whether a warning is a true bug, unsafe pattern, or low-value false positive

Prefer a real safe replacement over suppression. Suppress only when the warning is clearly wrong and the justification is durable.

## 4. Implement safe patterns

Use repo-approved patterns from `AGENTS.md`:

- string copy: `snprintf` with return-value/truncation checks
- bounded string length: `strnlen_s(value, max)`
- numeric parsing: `strtol`/`strtoul` with `errno`, end-pointer, and range checks
- file open: `fopen_s`
- memory copy: `memcpy_s`, or whole-struct assignment when copying a complete struct
- critical sections: `InitializeCriticalSectionAndSpinCount` or `InitializeCriticalSectionEx` with failure handling

Important: if an initialization step can now fail, propagate that failure and make cleanup safe. Do not leave code that later calls `EnterCriticalSection` or `DeleteCriticalSection` on an uninitialized lock.

## 5. Run local checks

At minimum:

```powershell
git diff --check
cmake --build OmniRDP/build --config Debug -j 2
ctest --test-dir OmniRDP/build -C Debug --output-on-failure
cmake --build OmniRDP/build --config Release -j 2
```

Run narrower targets when appropriate, but use full Debug tests and Release build before committing broad C changes.

## 6. Re-check banned APIs locally

The security workflow rejects direct uses of the calls that created most warning noise. Check before pushing:

```powershell
rg "\b(strcpy|strncpy|strlen|memcpy|fopen|atoi|getenv|fgetc|InitializeCriticalSection)\s*\(" OmniRDP/src OmniRDP/include OmniRDP/tests
```

Expected result after cleanup is no matches. If a remaining match is truly required, document why and adjust the security check deliberately rather than sneaking it in.

## 7. Commit and let GitHub confirm closure

After local validation, commit the fix and push/PR. GitHub alerts do not close immediately when code changes locally. They close only after the relevant GitHub scan runs on the updated commit or branch.

After CI completes, re-fetch alerts:

```powershell
gh api --paginate "/repos/OWNER/REPO/code-scanning/alerts?per_page=100&state=open"
```

If alerts remain open, compare the alert location and commit SHA. It may be from an old branch/scan, or the pattern may still exist through a wrapper/macro/new location.

## 8. Current security CI intent

OmniRDP uses GitHub-native security checks as the main signal:

- CodeQL C/C++ for semantic security scanning
- Flawfinder scoped to OmniRDP source/include/tests
- Cppcheck with low-noise bug/security settings
- banned-API grep for fast regression detection

Codacy clang-tidy is intentionally not used as a required gate because it needs build context and was failing as tooling noise rather than reporting useful security issues.
