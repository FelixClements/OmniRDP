# TEST KNOWLEDGE BASE

## OVERVIEW

C executable tests plus CMake text-audit tests that lock viewer/backend architecture boundaries.

## WHERE TO LOOK

| Task | Files | Notes |
|---|---|---|
| Register a new test | `CMakeLists.txt` | Add executable, include dirs/libs, `add_test`, and timeout |
| Locate config/test binaries | `RunBuiltTest.cmake` | Handles single-config and Visual Studio config subdirs |
| Viewer state/auth/pointer | `test_viewer_state.c`, `test_viewer_auth.c`, `test_viewer_pointer.c`, `test_pointer_shape.c` | Small direct unit tests |
| Framebuffer/publisher | `test_viewer_framebuffer.c`, `test_viewer_publisher.c` | Snapshot, generation, dirty-region, classic decision behavior |
| RDPEGFX pipeline/codecs | `test_viewer_gfx_pipeline.c`, `test_viewer_gfx_codec_uncompressed.c`, `test_viewer_gfx_codec_rfx.c` | Requires FreeRDP/WinPR DLLs for some tests |
| Backend quarantine | `test_backend_gfx_decode_only.c`, `CheckBackendGfxQuarantine.cmake`, `CheckNoBackendGfxReplay.cmake` | Prevents backend replay resurrection |
| Public/private boundaries | `CheckPublicViewerServerFacade.cmake`, `CheckViewerServerClassicTransportBoundary.cmake`, `CheckRefactorBoundaries.cmake` | Text audits over production sources |
| Service config | `test_svc_config.c` | Writes temporary config files with `fopen_s` |

## REGISTRATION PATTERN

1. Add `add_executable(test_name ...)`.
2. Add `target_include_directories(test_name PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ...)`.
3. Link `${WINPR_LIB}` and/or `${FREERDP_LIB}` only when the tested source requires it.
4. Set `C_STANDARD 11` and `C_STANDARD_REQUIRED ON`.
5. Register through `RunBuiltTest.cmake` so Visual Studio `Debug`/`Release` paths work.
6. Add `set_tests_properties(test_name PROPERTIES TIMEOUT 120)`.

## DLL PATH NOTES

- Tests using FreeRDP symbols need `TEST_DLL_DIR` entries for FreeRDP and WinPR build output.
- Release-artifact fallback paths appear in RDPEGFX tests because CI may stage DLLs outside the active build tree.
- Keep path lists semicolon escaped in CMake command arguments.

## BOUNDARY TESTS

- `CheckRefactorBoundaries.cmake` is the broadest architecture lock; update it only when the intended ownership boundary changes.
- `CheckNoBackendGfxReplay.cmake` forbids replay-era GFX symbols across production sources.
- `CheckPublicViewerServerFacade.cmake` keeps public headers free of internal viewer-server details.
- Text-audit tests are deliberate. Do not weaken forbidden token lists to make unrelated source changes pass.

## CONVENTIONS

- Most C tests use `assert`; larger tests use local `expect_*` helpers to keep failure context readable.
- Keep fixture setup local to the test file unless several tests genuinely share it.
- Test files may include internal `src` headers; production public headers should not include internals.
- For MSVC warning suppressions, scope them to the single target or source file.

## ANTI-PATTERNS

- Do not run FreeRDP-dependent test executables without arranging DLL search paths.
- Do not add tests that depend on generated `freerdp-3.26.0/` being committed.
- Do not mirror production unsafe-call patterns in tests unless the test is explicitly exercising the safe wrapper.
