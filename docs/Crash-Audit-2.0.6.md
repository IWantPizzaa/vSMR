# vSMR 2.0.6 crash audit

Date: 2026-11-01. Scope: the `dev` branch, Win32 Release plugin, bootstrap/update helpers, rendering, Runtime Menu, Control Center, audio, network workers, and JSON inputs.

## Findings and changes

- The PDC login crash path was fixed in the preceding `dev` change (`663bc46`): Runtime Menu actions that open EuroScope windows now run in the screen-object callback rather than the mouse hook. This audit added exception boundaries to the mouse and keyboard hooks, Runtime Menu clicks, and the timer callback. An unexpected C++ or MFC exception is recorded before returning to EuroScope.
- Nine production functions allocated roughly 64 KiB of local stack space for path or network buffers. These buffers now use heap storage with allocation failure handling. The follow-up MSVC code analysis reports no C6262 stack warning in production code. Its remaining C6262 is in the standalone test executable.
- Tag output now checks nullable SDK output pointers before writing. The holding-point tag's eight-character validation was traced through to its 16-byte SDK output buffer; this copy is bounded.
- A malformed short PE image could underflow a length check in the crash reporter while reading PDB identity. The minimum image-size check now covers the NT headers.
- The background AVISO sync function is `noexcept`; its error-message assignment is now guarded so a second allocation failure in the catch handler cannot call `std::terminate`.
- Exception logging in the AVISO refresh callback and the timer audio worker is guarded so a logging failure cannot turn a recoverable exception into a process exit. Refresh exception log throttling is now thread-local to avoid a data race across radar views.

## Verification

- MSVC v145 Win32 Release solution build with `/analyze`: completed. Remaining null-pointer diagnostics in production were checked against their preceding guards, and the WinHTTP WebSocket upgrade API intentionally receives a null option buffer with size zero. Third-party RapidJSON style diagnostics and tick-count guidance remain.
- Full project build and regression scripts: passed, including Control Center browser tests, installer/merge checks (373 checks), JSON entry-point bounded-validation check, audio-resource check, and distribution checks.
- AddressSanitizer Win32 regression executable: passed.
- AddressSanitizer JSON parser smoke/fuzz executable: passed on 10,014 inputs.

## Limits of this audit

No crash dump or reproduction from the affected EuroScope 3.2.9 installation was available. The real PDC login click path, WebView2/EuroScope interaction, graphics drivers, and other plugins cannot be fully exercised by the standalone tests. Exception boundaries do not recover from invalid memory access or a process abort inside EuroScope, the C runtime, a driver, or another plugin. Therefore these checks reduce known crash risks but cannot establish a literal zero-crash guarantee; any new CTD should be investigated from its minidump and exact installed build.
