# FPS-Killer: AVISO implementation and measurements

Work started from `dev` commit `e774ac5` on branch `FPS-Killer`.
Measurements were collected on 12 October 2026 with the local Ryzen 7 9800X3D,
RTX 5070, and a Release Win32 MSVC v145 executable. The machine also exposes a
virtual display adapter. No live EuroScope session was available.

The substantial measured improvement is **cached AVISO presentation**. This is
a component benchmark, not a claim of 200/240 FPS in EuroScope. Motion-time
raster rebuilding remains expensive on detailed maps.

## Original architecture

`RadarScreen.AvisoData.cpp` reads and validates GeoJSON, resolves styles and
palettes, converts polygons, lines and labels into geographic coordinates,
and publishes immutable snapshots for workers. It previously also retained an
unused duplicate of those complete vectors. User edits and runway visibility
produce new snapshots/generations; the editor continues to operate on JSON.

Each main/inset viewport owns an `AvisoRasterPipeline` worker. Requests carry
geographic bounds, four host-projected corners, resolution, visibility and
palette. The worker interpolates the four corners, projects vertices, builds
GDI+ geometry/text, and draws an antialiased premultiplied 32-bit DIB. It draws
polygon rings independently in document order; this behavior is preserved.

The UI stores an overscanned raster and projects/crops it during pan/zoom while
new work runs. Coalescing, latest-request cancellation, content debounce and
worker-to-UI refresh messages already existed. Main raster allocation is capped
at 6,400 pixels per axis and 32 million pixels; inset sizing uses an 18 million
pixel budget. These existing limits remain.

Even a cache hit previously filled the complete viewport background, waited
for its GDI+ drawing to complete, then used GDI `AlphaBlend` for the AVISO
overlay. Dynamic aircraft, tags, alerts, RDF and inset chrome are separate passes.

## Findings and evidence

| Finding | Evidence/status |
| --- | --- |
| Background repaint and alpha compositing cost persisted on every cached frame | Confirmed in main/inset code and measured presentation benchmarks |
| Rebuilds recreated brushes, pens, font families and glyph paths and visited all features/labels | Confirmed in original renderer; warm rebuild comparisons quantify resource reuse |
| A feature-level bound can include many invisible component paths | Confirmed; new path culling has synthetic correctness coverage |
| Loading retained two full copies of geometry/text | Confirmed by all uses of the old vectors; removed the unused copy |
| Translating cached glyph outlines can change antialias samples | Confirmed by pan replay image comparisons; cache now requires exact layout origins |
| Diagnostics reset created an approximately 377 KB stack temporary | Confirmed by object disassembly and a native test stack overflow; reset now fills in place |
| AVISO caused the reported approximately 100 FPS limit | **Unproven** without a running host and an identical traffic/display workload |
| GPU, synchronization, other plug-ins or host work impose a particular FPS ceiling | **Unproven**; no host/GPU trace or display presentation measurements were available |

Scene capture still enumerates controllers/targets and computes tag/alert state
on each selected refresh callback. Full scene diagnostics and mixed GDI/GDI+
ordering are other possible frame costs. Their contribution to the user's FPS
has not been measured, so they were not speculatively rewritten.

## New pipeline

1. Move parsed vectors into immutable snapshots without a second complete copy.
2. Compile shared geographic feature/label BVHs and per-path bounds once per
   loaded geometry. Group membership changes reuse the bounds because geometry
   and ordering remain unchanged.
3. Query ordered viewport candidates in the worker, retaining the original
   painter order. Cull distant component paths with projected stroke/antialias
   padding. Preserve the original coordinate rounding and vertex policy.
4. Reuse worker-local projection buffers, brushes, pens and exact-origin glyph
   paths. Changed label snapshots or resolution invalidate the relevant caches.
5. Bake a fully opaque palette background into the worker raster. With full
   viewport coverage, the UI copies it directly and skips background repaint.
   Partial coverage still paints the background first. Translucent backgrounds
   retain their original transparent overlay and UI alpha composition.
6. Keep existing geographic previews, worker cancellation, supersession,
   editing, palette and runway/group behavior. Cache keys now explicitly include
   baked background color and geometry zoom thresholds; applied caches carry
   resolution and zoom metadata.

Native opaque display uses `BitBlt`; scaled opaque previews use `StretchBlt`
with `COLORONCOLOR`, matching the prior AlphaBlend sampling policy. Microsoft
documents that AlphaBlend converts stretch modes to COLORONCOLOR, including
HALFTONE. [AlphaBlend documentation](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-alphablend)

No additional raster copy is allocated for the opaque path. Glyph outline
storage is limited to 524,288 points (approximately 5 MB plus container
overhead per worker); each brush/pen table is capped at 2,048 entries. Worker
termination destroys its caches before GDI+ shutdown. Original DIB limits,
selected-object restoration and result ownership remain enforced.

Direct2D can interoperate with a supplied GDI DC, so GPU rendering is a possible
future experiment. Its interoperation and raster differences require separate
measurement and compatibility work; no GPU speedup was assumed or claimed.
[Direct2D/GDI interoperation](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-and-gdi-interoperation-overview)

## Reproducible measurements

The standalone harness freezes the original renderer from `e774ac5` and calls
the production renderer with the same converted bundled LFPG/LFPO/LFML data.
It bypasses local overrides, editor normalization, live runway rules and
EuroScope scheduling. See the
[benchmark method and retained results](../vSMR/tests/aviso_benchmark/README.md).

Three warmups precede 40 rebuild samples. Presentation has 160 samples and
includes `GdiFlush`; baseline/optimized timing order alternates. Standard
rasters are 1920 × 1080. Two extra LFPG cases use 3840 × 2160 caches. Geographic
overscan and production cache-sizing decisions are outside the harness.
Compilation cost and the first optimized render are reported separately.

| Operation, LFPG whole map / Dark | Original mean | Optimized mean | Reduction |
| --- | ---: | ---: | ---: |
| Warm transparent rebuild | 43.758 ms | 36.266 ms | 17.1% |
| Changing pan/zoom rebuild | 42.657 ms | 42.547 ms | 0.3% |
| Native presentation, including background fill | 1.400 ms | 0.239 ms | 82.9% |
| Fully covered native presentation | 1.401 ms | 0.132 ms | 90.5% |
| Fully covered scaled preview | 1.923 ms | 0.436 ms | 77.3% |

Across the 18 standard scenes, covered native presentation improved
10.05–10.72× (90.05–90.67% lower mean time). Warm rebuild time decreased
13.73–21.23%. Pan/zoom rebuild results ranged from about 3% slower to about 3%
faster: **there is no substantial measured motion-time rebuild improvement**.
LFPG's 3840 × 2160 whole-map warm rebuild changed from 67.590 to 59.586 ms.

All 20 primary transparent rasters, 240 changing pan/zoom renders and 20
visibility/resolution/projection variants matched the baseline pixel for pixel.
Opaque native/scaled output differs by at most 2/255 per RGB channel because
GDI+ background composition and final Windows alpha composition round differently.
This is a small color rounding difference, not a geometry/placement change.

These timings do not include host presentation, target/tag work, real mouse
input latency or other plug-ins. They cannot establish overall FPS, CPU
utilization or 1% low FPS improvements. Cold costs and p95/max values are kept
in the result CSV instead of being hidden by the mean.

## Diagnostics and host limitations

Performance exports now include sub-millisecond callback entry intervals,
timing p99 and callback cadence 1% low. The latter is 1000 divided by the mean
of the slowest `ceil(N × 0.01)` positive callback intervals in milliseconds.
It includes idle time and UI scheduling; it is explicitly **not display FPS**.

Measured plugin work now includes callback preparation and graphics teardown,
but excludes diagnostics reporting and host presentation. This definition is
broader than the original frame metric, so old/new frame exports are not
directly comparable without accounting for the changed scope. History remains
bounded to 2,048 samples; at 240 callbacks/sec it holds roughly 8.5 seconds.

The bundled SDK documents `RequestRefresh` as posting/coalescing requests until
the UI thread finishes other tasks (`lib/include/EuroScopePlugIn.h`, RequestRefresh).
Its standard one-second refresh and once-per-second `OnTimer` do not prove a
100 FPS cap during user input. The plugin has no discovered fixed 100 Hz render
timer. No artificial high-frequency invalidation loop was added.

To measure actual host behavior, run matching baseline/new builds with the same
airport, viewport, palettes/opacity, resolution, traffic, inset arrangement and
other plug-ins. Capture sustained pan/zoom separately from idle operation.
Export Performance diagnostics immediately after each interval, compare work
stages against callback intervals, and retain sample counts/cache outcomes/build
cancellation counts. Use a host/display trace for presented FPS and input latency.

## Changed files and validation

| Files | Purpose |
| --- | --- |
| `src/aviso/AvisoRenderScene.*` | Immutable spatial indexes and component bounds |
| `src/aviso/AvisoRasterRenderer.*` | Reusable standalone worker renderer and bounded resource/glyph caches |
| `src/aviso/AvisoRasterBlitter.*` | Opaque native/scaled copy paths, retained alpha path and host DC restoration |
| `src/aviso/AvisoRasterPipeline.*`, `src/radar/RadarScreenTypes.hpp` | Background/zoom cache keys and immutable render metadata |
| `src/radar/RadarScreen.AvisoData.cpp`, `.AvisoGroups.cpp`, `RadarScreen.hpp` | Snapshot ownership, index sharing, removal of duplicate vectors |
| `src/radar/RadarScreen.AvisoRendering.cpp`, `src/insets/InsetWindow.Aviso.cpp` | Main/inset background coverage, cache validity and new renderer integration |
| `src/diagnostics/PerformanceDiagnostics.*`, `src/radar/RadarScreen.Refresh.cpp`, `src/control_center/ControlCenterPerformance.cpp` | Callback/work distributions, p99/1% low export and stack-safe reset |
| Renderer/diagnostics/pipeline/shared rendering tests and projects | Regression coverage and build integration |
| `tests/aviso_benchmark/*` | Frozen baseline, actual map fixtures, timing/image harness and recorded results |
| `.gitattributes`, `README.md` | Pin template line endings to preserve the existing migration fingerprint and link this report |

Initial `dev` Release Win32 build and native regressions passed. Final validation:

- Clean solution rebuild: Release Win32, MSVC v145, successful. Loader, runtime,
  apply helper, crash handler and native test executable were produced.
- `vSMR/tests/run_tests.ps1`: exit 0. Native regression tests and the Control
  Center browser tests passed, including new renderer, blitter, queue and
  diagnostics coverage. Release/default/feed/installer/JSON/audio/assets/style
  and naming checks passed; installer/migration fixtures passed 368 checks.
- Release input verification confirmed all 161 bundled AVISO file hashes.
  No map, palette or configuration content changed.
- Standalone benchmark: exit 0; 280 exact transparent image comparisons and
  native/scaled opaque checks passed. Recorded CSV and metadata are committed
  with the reproduction harness.
- `git diff --check`: passed.

Validation exposed an existing template line-ending/hash inconsistency. Pinning
`profile_templates.json` to CRLF preserves its stored migration fingerprint
across checkouts without regenerating `default.json` or changing configuration.

Live EuroScope frame pacing, host display appearance and prolonged in-host
resource behavior remain unverified.

## Further work

The next useful measurement is a live host trace to identify whether target/tag
work, host scheduling or raster rebuild latency limits interaction. If motion
rebuilds dominate, prototype a GPU raster backend against these frozen image
fixtures and measure complete DC transfer cost. For especially large scenes,
investigate bounded tiles or incremental pan strips while preserving labels,
transparency order and the 32-bit address-space budget. These remain future
experiments, not implemented features or promised gains.
