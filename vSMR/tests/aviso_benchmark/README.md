# AVISO rendering benchmark

This standalone Windows executable compares the original `dev` raster renderer at
`e774ac5` with the production renderer on `FPS-Killer`. Both paths use real GDI+
and the same shipped airport coordinates, styles, labels and render requests.
It also measures the production GDI presentation paths into an off-screen DIB.
It does **not** measure EuroScope FPS, input latency or end-to-end host frame time.

Run from PowerShell with Visual Studio C++ / MFC / Windows SDK installed:

```powershell
./vSMR/tests/aviso_benchmark/run_benchmark.ps1 -Samples 40
```

The script builds an x86 Release executable with the repository's compiler
hardening settings, then writes CSV statistics and reference image pairs under
`artifacts/aviso-benchmark`. Use `-OutputDirectory` for a different destination,
or `-SkipBuild` after an unchanged build. A return code of zero means all image
checks passed; image failures return a nonzero status and save the first mismatched
pair for each operation. Close other CPU-intensive workloads when collecting
performance results.

The renderer baseline is kept in `BaselineRenderer.cpp`, outside the production
DLL. Its drawing body comes from the original `CSMRRadar::RenderAvisoGeoJsonRaster`;
only the member-function signature and host cancellation check were adapted for
the standalone executable. `BenchmarkFixture.cpp` preserves the original loader's
paint resolution, geometry conversion, visibility and label policy. Its UTF-8
conversion allocates room for the terminating character before trimming it. It
reads the shipped document directly: it does not apply local user overrides,
editor validation/normalization or live runway visibility rules. Those lifecycle
paths remain covered by the main regression suite.

The workload uses LFPG, LFPO and LFML, all three palettes, a whole-map view, and a
5x detail view rotated 23 degrees. Detail centers use the median label position,
so large perimeter polygons do not put the benchmark in an empty part of the map.
The 18 standard rasters are 1920 x 1080, with original smoothing, pixel offsets,
font sizes, vertex preservation and coordinate rounding. Two additional LFPG Dark
scenes use 3840 x 2160 caches with `rasterScale=2` over the same projected display,
covering the higher raster resolutions used by cache oversampling. Native presentation
timings in those rows copy the full 3840 x 2160 bitmap; the scaled presentation rows
copy it to the same 1379 x 777 target as the standard scenes. Geographic overscan
margins and real host cache-sizing decisions are outside the harness.
The render scene is compiled outside
the rebuild timing; its cost and the first optimized render are reported separately.

The CSV reports mean, median, p95 and maximum milliseconds, plus speedup and
image differences. Timings exclude result bitmap destruction, matching the worker's
render-callback boundary. Rebuild samples use three warmups and alternate baseline
and optimized order. Presentation samples include `GdiFlush` and use four times
the requested sample count. Pan/zoom replay uses 12 deterministic, changing bounds.

| Operation | Baseline | Optimized | Image requirement |
| --- | --- | --- | --- |
| `rebuild` | Original transparent render | Warm production transparent render | Exact pixels |
| `pan_zoom_replay` | Original render at each new view | Same renderer instance at each new view | Exact pixels across all 12 views |
| `visibility_scale_projection` | Original render | Production render with changed groups, zoom, display/raster scale and skewed corners | Exact pixels |
| `native_present` | Background clear + AlphaBlend | Background clear + BitBlt of opaque cache | RGB channel difference at most 2/255 |
| `covered_native_present` | Background clear + AlphaBlend | BitBlt with cache covering entire viewport | RGB channel difference at most 2/255 |
| `scaled_present_fidelity` | Clear + scaled AlphaBlend | Clear + scaled StretchBlt | RGB channel difference at most 2/255 |
| `covered_scaled_present` | Clear + scaled AlphaBlend | StretchBlt covering a 1379 x 777 viewport | Same RGB requirement |

Opaque-cache comparisons allow the documented two-level integer rounding difference
between GDI+ source-over during raster construction and Windows AlphaBlend during
presentation. Transparent renderer and pan/zoom comparisons allow no pixel changes.
This stricter replay check caught translated cached glyph outlines changing a few
antialias samples; production caching now keys glyphs by their exact layout origin.

The benchmark prints renderer work counters alongside scene names. Hardware,
compiler version, fixture hashes and the result CSV should be retained together
when recording a run. A component speedup cannot be converted into an FPS claim:
EuroScope scheduling, radar target/tag work, UI composition, GPU/display drivers and
other plug-ins are outside this harness. A small or absent pan-rebuild improvement
is still recorded as measured; the large presentation reduction applies to cache
reuse, with the greatest gain when an opaque cache covers the viewport.

The completed [2026-10-12 measurement report](results/2026-10-12.md) retains the
actual result CSV and the machine/compiler/fixture/source hashes from that run.
