# vSMR Project Structure

vSMR keeps first-party implementation code in a feature-oriented `vSMR/src/` tree. C++ headers live beside the implementation or feature that owns them; there is no separate flat public-header directory. This structure is for maintainers and does not change the installed plug-in layout.

## Source tree

```text
vSMR/
+-- src/
|   +-- app/                  MFC application object and EuroScope entry point
|   +-- platform/windows/     Windows SDK, GDI+, WinHTTP, PCH, resource IDs, and target-version support
|   +-- shared/               Feature-neutral text and logging helpers
|   +-- config/               Runtime profile/configuration loading and persistence
|   +-- plugin/               Process-wide EuroScope plug-in coordinator
|   +-- aircraft/             Aircraft lookup and ground-state domain support
|   +-- radar/                Main radar-screen lifecycle, rendering, menus, and interaction
|   +-- tags/                 Tag definitions, rendering, rules, and VACDM tag helpers
|   +-- insets/               AVISO, SRW 1, METAR, and Timer inset windows
|   +-- aviso/                AVISO document model, editor, presets, and radar integration
|   +-- profiles/             Profile editor and profile-specific path helpers
|   +-- control_center/       Native WebView2 host, bridge, and resource management
|   |   `-- web/              Control Center HTML, CSS, and JavaScript source
|   +-- datalink/             CPDLC settings and datalink message dialogs
|   +-- weather/              Weather parsing and process-wide cache
|   +-- safety/               RIMCAS runway monitoring and alerts
|   +-- rdf/                  Native TrackAudio RDF overlay
|   `-- crash/                WER registration, shared protocol, and crash-safe state
|       `-- handler/          Separate out-of-process WER callback DLL project
+-- resources/               Main DLL resource scripts, cursors, audio, and exports
+-- data/                    Canonical files copied into the runtime data tree
`-- tools/
    `-- crash_harness/       Isolated deterministic and real-WER validation harness
```

The root `vSMR/vSMR.vcxproj` builds the plug-in. `vSMR/src/crash/handler/vSMRCrashHandler.vcxproj` builds the x86 WER callback DLL. The crash harness remains a separate validation project at `vSMR/tools/crash_harness/vSMRCrashHarness.vcxproj` so it cannot add crash-injection paths to the production plug-in or handler.

## Feature ownership

| Area | Primary files and ownership |
| --- | --- |
| Application entry | `src/app/PluginEntry.*` owns the MFC application object and exported EuroScope initialization entry. |
| Plug-in coordinator | `src/plugin/Plugin.*` owns process-wide EuroScope callbacks, worker scheduling, external integrations, diagnostics, and radar-screen creation. |
| Radar screen | `src/radar/RadarScreen.*` owns the main screen lifecycle, refresh/render path, commands, interaction, Runtime Menu, target filtering, and ASR persistence. |
| Aircraft and tags | `src/aircraft/` owns aircraft/ground-state domain support; `src/tags/` owns tag data, definitions, rules, and rendering. |
| Insets | `src/insets/InsetWindow.*` owns inset lifetime, drawing, movement, snapping, resizing, and airport-scoped state. |
| AVISO | `src/aviso/` owns GeoJSON validation/editing, presets, and radar integration. Rendering remains integrated with the existing radar and inset paths. |
| Profiles and configuration | `src/config/RuntimeConfig.*` owns migration, validation, and persistence; `src/profiles/` owns profile editing and profile UI integration. |
| Control Center | `src/control_center/` owns the WebView2 host, native/JavaScript bridge, and managed resource files. Its web source is under `src/control_center/web/`. |
| Datalink and weather | `src/datalink/` owns CPDLC/PDC dialogs; `src/weather/` owns weather parsing and cached data. Process-wide scheduling remains in `Plugin.cpp`. |
| Safety and RDF | `src/safety/` owns RIMCAS; `src/rdf/` owns the native TrackAudio overlay. |
| Crash reporting | `src/crash/` owns normal-runtime registration and fixed-size breadcrumbs. `src/crash/handler/` owns out-of-process report generation. |
| Windows platform support | `src/platform/windows/` owns code tied directly to Windows, GDI+, WinHTTP, the SDK target, or the precompiled header. |

Feature ownership describes where code belongs, not a new runtime boundary. Existing `CSMRPlugin`, `CSMRRadar`, dialog, and helper symbols retain their established names.

## Include convention

`vSMR/src` is the single include root for first-party C++ code. Include project headers by their feature-qualified path:

```cpp
#include "radar/RadarScreen.hpp"
#include "crash/CrashRuntime.hpp"
#include "control_center/ControlCenterDialog.hpp"
```

Do not add each feature directory as a separate include path, use `..` traversal, or rely on a duplicate basename being found through include-directory order. Exact path and filename case should match Git even though normal Windows builds are case-insensitive. System and external-library headers continue to use their normal SDK or dependency include forms.

The main plug-in, crash handler, and crash harness projects each point to the same `vSMR/src` include root. The precompiled-header setting uses `platform/windows/PrecompiledHeader.hpp` explicitly. The resource script lives under `vSMR/resources/`, includes `platform/windows/ResourceIds.h` through the same source include root, and resolves cursors, audio, and its secondary resource script relative to `vSMR/resources/`.

## Stable runtime and package layout

The source reorganization does not rename installed files or directories:

| Source | Build/package destination |
| --- | --- |
| `vSMR/src/control_center/web/*` | `vSMR_Data/vSMR_webUI/*` |
| `vSMR/src/crash/handler/vSMRCrashHandler.vcxproj` output | `vSMR_Data/CrashReporter/vSMRCrashHandler.dll` |
| `vSMR/data/*` | Corresponding content under `vSMR_Data/*` |
| `vSMR/resources/*` | Compiled resources and exports in `vSMR.dll` |
| Main plug-in project output | `vSMR.dll` |

Runtime code and user documentation should continue to refer to `vSMR_Data`, `vSMR_Data/vSMR_webUI`, and `vSMR_Data/CrashReporter`. Source paths must not leak into release metadata as replacement runtime locations.

## Preserved translation-unit boundaries

The physical reorganization deliberately moved the large, stateful translation units intact instead of mixing structural cleanup with behavioral refactoring. In particular, `RadarScreen.cpp`, `Plugin.cpp`, `InsetWindow.cpp`, `ProfileEditorDialog.cpp`, `AvisoEditorDialog.cpp`, and `ControlCenterBridge.cpp` retain their existing responsibility groupings. The Control Center's large `app.js` and `styles.css` also remain intact because JavaScript initialization order and CSS cascade order are observable behavior.

These files can be decomposed later only as separate, tested changes. Anonymous-namespace state, static initialization, worker synchronization, renderer caches, MFC message routing, WebView bridge transactions, and CSS/JavaScript ordering make apparently mechanical splits capable of changing behavior.

When adding new code, prefer the owning feature directory and colocate a new `.hpp`/`.cpp` pair. Cross-feature abstractions belong in `shared/` only when they are genuinely feature-neutral; Windows-specific code belongs in `platform/windows/`.
