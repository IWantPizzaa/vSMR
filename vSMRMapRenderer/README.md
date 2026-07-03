# vSMR MapLibre Renderer

This directory is the sidecar build area for the future x64 helper process.
It intentionally does not modify the existing Win32 `vSMR.vcxproj` plugin
build.

## Phase 1 POC

`vSMRMapRendererPoc` is a standalone x64 proof-of-concept executable that:

- loads a local airport GeoJSON through a MapLibre GeoJSON source
- creates a minimal offline style with no basemap
- renders camera bounds into a complete BGRA frame
- optionally exports PNG and raw BGRA output
- reports first-frame, average, p95, export, and memory metrics

The target is disabled by default because MapLibre Native is not vendored in
this repository. Build it only after producing x64 MapLibre Native artifacts.

Example configure command:

```powershell
cmake -S vSMRMapRenderer -B build-maprenderer-x64 -A x64 `
  -DVSMR_BUILD_MAPLIBRE_POC=ON `
  -DMAPLIBRE_NATIVE_ROOT=C:\deps\maplibre-native `
  -DMAPLIBRE_NATIVE_LIBRARIES="C:\deps\maplibre-native\build-windows-opengl\lib\mbgl-core.lib;..."
```

Example run command:

```powershell
.\build-maprenderer-x64\Release\vSMRMapRendererPoc.exe `
  --geojson C:\data\LFPG.geojson `
  --bounds 49.05 2.45 48.98 2.62 `
  --width 1920 --height 1080 `
  --frames 120 `
  --output lfpg.png `
  --bgra lfpg.bgra
```

The bounds order is `north west south east`, matching MapLibre Native's
`mbgl-render` tool.

## Dependency Boundary

MapLibre Native remains an x64-only dependency of this sidecar build. Do not add
MapLibre include paths or libraries to the Win32 EuroScope plugin project.

Recommended initial backend: MapLibre Native `windows-opengl`.

## Current Limits

This POC does not yet open a continuously interactive window. It exercises the
persistent MapLibre render path and frame export first; the interactive mouse
pan/zoom window should be added after the dependency is available locally and
the static LFPG render has been validated.
