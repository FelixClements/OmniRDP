// test_utils.h — suppress blocking CRT dialogs on headless CI
#pragma once

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <stdlib.h>

static inline void test_suppress_crt_dialogs(void) {
    /* Default report mode in Debug CRT is _CRTDBG_MODE_WINDOW (modal dialog).
     * Switch to _CRTDBG_MODE_DEBUG which sends output to OutputDebugString
     * — silent when no debugger is attached. */
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_WARN,  _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);

    /* Suppress the Windows Error Reporting dialog on abort(). */
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
}
#else
static inline void test_suppress_crt_dialogs(void) {}
#endif
