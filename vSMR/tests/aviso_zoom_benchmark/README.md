# AVISO native zoom benchmark

This Windows Release Win32 executable compares the `a76b379` FPS-Killer GDI+
raster/preview strategy with the production native vector viewport renderer.
It measures complete off-screen rendering and GDI presentation, including the
GPU wait and readback. It does not measure EuroScope FPS or input latency.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File vSMR/tests/aviso_zoom_benchmark/run_zoom_benchmark.ps1 -Frames 32
```

Visual Studio C++, MFC and the Windows SDK are required. The script uses the
repository compiler hardening settings and writes `zoom_measurements.csv`,
per-frame `zoom_stages.csv`, and PNG reference images to
`artifacts/aviso-zoom-benchmark`. Use `-OutputDirectory` to retain separate runs,
`-SkipBuild` for an unchanged binary, `-Quick` for the three whole-map dark scenes
and LFPG whole/detail bilinear scenes, or `-Software` to force Direct2D WARP.
`-QualityOnly` runs the independent synthetic probes without airport timings.
The default uses production analytic native glyph coverage. `-VectorReference`
selects the Direct2D vector reference for controlled antialias/performance
comparisons; `-NativeAnalytic` explicitly selects the default.
Stop other CPU-intensive work while measuring.
Each run also records source, fixture and binary SHA256 hashes, options, exit
status, CPU/GPU/driver and OS details in `run_metadata.json`.

The old path calls the unchanged `AvisoRasterRenderer`, builds the original
native-resolution overscan, and reproduces the prior viewport-aligned preview
mapping: geographic corners to projected AABBs, floor/ceil source cropping,
expanded rounded destination, then the real `AvisoRasterBlitter` including its
near-native extent rule. It periodically refreshes the cached raster after four
replay frames. If a sparse replay exceeds its margin, it catches the cache up
before displaying the next frame. The comparison measures the sampled-image
handoff and rounding error, rather than attempting to reproduce the host worker
scheduler. Opaque caches that cover the viewport skip background painting in
both paths, matching FPS-Killer.

The new path calls the production `AvisoViewportRenderer::Draw`. It rasterizes
the current view at its exact native pixel dimensions and copies the completed
image. For every replay step, the settling frame must reuse the same HBITMAP,
report a cache hit without a build, and reproduce all presented RGB pixels.
There is no later higher-resolution replacement to compare against.

The default matrix contains 24 scenes and 32 steps per scene:

- LFPG, LFPO and LFML; dark, light and real palettes; whole map and 5x detail
  rotated 23 degrees at 1920 x 1080.
- LFPG whole and rotated detail projections with the bottom-right corner displaced
  by one pixel, exercising the host SDK's possible non-affine corner rounding.
- LFPG whole map at 800 x 450 and 3840 x 2160 native pixels.
- LFPG at 2560 x 1440 with display scale 4/3, and 3840 x 2160 with display
  scale 2, exercising the configured 2K and 4K font and stroke sizes.

Each sequence zooms in and out by a smooth factor up to approximately 2.34,
anchored at varied off-center geographic points. The request corner basis stays
fixed, as it does during a cursor-centered geographic zoom. The native request
uses the authoritative double-precision corner basis. Its culling bounds match
production: include the inverse projection of all native viewport corners and
add a half-span geographic margin for offscreen strokes and labels. These margins
do not allocate extra raster pixels. The old renderer uses the original float
corner basis and raster overscan. The first frame and
the duplicate central zoom value can hit the new cache; timings record this in
`zoom_stages.csv`. Parsed immutable fixtures and spatial scene compilation are
outside the timing. Initial device/geometry/font setup is reported separately.
The fixture loader is shared with the original AVISO benchmark: it reads bundled
data and styles directly, without local overrides or live runway policy.

Additional probes check transparent premultiplied output, integer screen-origin
invariance, a translucent inset background painted once per frame, and a
geographic marker across 40 zoom steps at 17 degrees with a nonzero origin. The
marker checks the coverage-weighted pixel centroid against the geographic
projection within one eighth of a pixel, allowing discrete antialias coverage.
Its repeated images must match all BGRA bytes. Eight renderer lifetimes each
perform eight surface resizes; bitmap byte counts, deleted HBITMAPs, and unchanged
process GDI-object counts check ownership and release.

Palette, display scale, opacity, group visibility and Unicode font changes are
compared against independently rendered current-view images. Replacing immutable
feature/label arrays with identical style and text but changed group membership
must retain shared font outlines without a miss or pixel change.

Native glyph alternatives also have two independent quality measurements. A
64-phase grid records differences from Direct2D vector reference coverage for
three palettes, with halo, translucent fill and opaque fill. The native edge-energy
ratio must remain at least 0.90; this rejects the discarded bitmap-atlas experiment
(approximately 0.67–0.78). RGB differences and contrast-weighted centroid differences
are recorded as antialias-style diagnostics, not used as coordinate ground truth.
Separately, 768 white-on-transparent phases cover Arial/Calibri, `O`/`Aa123 Runway`
and display scales 1, 4/3 and 2. The alpha centroid is compared with the area-weighted
centroid of the actual cached GDI font outline's triangles, within 0.125 pixels.
Analytic coverage must also preserve source alpha area within an 8-bit edge
quantization bound. Repeated frames must match every BGRA byte, and helper usage
is checked so fallback to the reference cannot silently satisfy these probes.

These measurements explain why reference RGB equality is inappropriate for a new
native antialias implementation: the Direct2D realization can increase the covered
alpha area of thin font strokes, altering contrast and its weighted centroid.
The source-outline oracle tests actual physical placement without that bias.

| Operation | Meaning |
| --- | --- |
| `motion_rebuild_present` | Old current-view raster rebuild and presentation versus new current-view vector build and presentation; includes active-to-settled image differences |
| `old_preview_present` | The old stretched cached image copy alone; its quality differs from a current-view vector render |
| `idle_cached_present` | Exact unchanged-view cache presentation in both paths |
| `native_centroid_error` | Geographic-marker coverage-centroid error; recorded in the cursor error column |
| `same_projection_stability` | Two native transparent renders with the same request, compared over all BGRA channels |
| `integer_screen_origin` | Translate request corners and origin equally; native transparent pixels must remain identical |
| `inset_origin_background_once` | Actual offset viewport presentation and settled cache reuse over a fresh underlay with one translucent background pass |

`old_handoff_differing_pixels` accumulates changed pixels across the replay,
so a pixel can be counted on several frames. `old_max_affine_alignment_error_px`
is the continuous mapping error at nine geographic markers before discrete
bitmap sampling. It isolates the rounded rectangle transform. Native renderer
antialiasing can differ from GDI+; old-versus-new pixels are not asserted equal.
The stability requirement compares the new active and settled frame at the
same current view.

The per-frame stage CSV reports preparation, geometry submission, labels,
EndDraw/flush, GPU readback, total build and total frame milliseconds. Direct2D
can execute work asynchronously, so stage boundaries identify CPU and wait
costs rather than independent GPU execution durations.
It also records candidate, path and glyph counters, cached outline points, and
estimated working/staging GPU image bytes. Cached presentation order alternates
between old and new to reduce ordering bias.
Exact cache-hit rows report zero build work. Cache memory, visible-label count
and glyph implementation describe the reused current-view image. Every nonempty
analytic label batch must report helper usage; a zero usage flag with zero
visible labels represents an empty batch rather than fallback.

Success returns zero. The test fails for stale viewport cache reuse, nonzero
active-to-settled image changes, lost marker alignment, invalid alpha data,
repeated background painting, renderer failure, or GDI resource leakage.
Timing comparisons are retained as measurements rather than hard thresholds:
driver scheduling and other workloads can affect individual samples.

Current-view vector frames have more work than copying an already rasterized,
scaled preview. Compare those costs explicitly when assessing interaction FPS;
a faster complete rebuild does not prove that every host frame gets faster.
