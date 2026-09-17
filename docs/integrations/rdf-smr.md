# RDF on the SMR display

`.smr rdf on` and `.smr rdf off` control vSMR's native overlay across the main view and its insets. The off command requests a redraw immediately and does not wait for the TrackAudio WebSocket worker to stop.

The separate [KingfuChan/RDF plugin](https://github.com/KingfuChan/RDF/blob/a4bd0ae5272088286acee1c2495ed3e4a2e627c6/RDFPlugin/CRDFPlugin.cpp) creates its own overlay on every radar display. When both plugins are loaded, that overlay can make the vSMR off command appear ineffective. The supplied patch prevents RDFPlugin from creating an overlay on `SMR radar display`; its other displays and audio bridge are unaffected.

## Build and installation

1. Check out KingfuChan/RDF commit `a4bd0ae5272088286acee1c2495ed3e4a2e627c6` (1.4.3b).
2. Apply [RDF-vSMR-ground-view.patch](../../vSMR/data/Tools/RDF-vSMR-ground-view.patch).
3. Install the repository's vcpkg manifest dependencies for `x86-windows-static`.
4. Build `RDFPlugin/RDFPlugin.vcxproj` in `Release|Win32`, using MSVC with MFC and C++20 support and the installed vcpkg dependencies.
5. Back up the installed RDFPlugin DLL and replace it with the rebuilt DLL while EuroScope is closed. Install the matching vSMR runtime, then restart EuroScope.

The companion remains GPL-3.0 software; keep its source and license with any distributed binary.

## Live check

With both plugins loaded, receive a transmission on the SMR display. Verify `.smr rdf off` removes the indication from the main view and AVISO/SRW insets, and `.smr rdf on` restores an ongoing transmission. Verify the separate RDFPlugin still draws on a standard radar display. These checks require EuroScope and TrackAudio and are not covered by the standalone vSMR regression suite.
