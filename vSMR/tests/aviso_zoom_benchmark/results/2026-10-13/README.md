# Recorded native AVISO zoom validation — 2026-10-13

Both Release Win32 runs returned zero. The hardware matrix compared 768 active
and settled frames across 24 airport/palette/projection/resolution cases; every
new presented pixel was identical. WARP repeated five cases with 16 steps each,
also with zero changed pixels. Native image identity and exact viewport cache
reuse were asserted at every settling step.

This is an off-screen component test. It does not establish EuroScope interaction
FPS or input latency. Native current-view frames cost more than the old scaled
preview copy; first-view native initialization also costs more. The expensive
complete raster rebuild and unchanged-view bitmap copy are separate comparisons.

## Reproduction and provenance

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File vSMR/tests/aviso_zoom_benchmark/run_zoom_benchmark.ps1 -Frames 32
powershell -NoProfile -ExecutionPolicy Bypass -File vSMR/tests/aviso_zoom_benchmark/run_zoom_benchmark.ps1 -Frames 16 -Quick -SkipBuild -Software
```

See the [harness method](../../README.md) and
[implementation notes](../../../../../docs/AVISO-Zoom.md). The before path is
the unchanged `a76b379` FPS-Killer raster renderer with its original overscan,
rounded viewport preview mapping and bitmap presenter. The after path uses the
production native viewport controller and analytic glyph renderer, including
the final exact geometry-state cache. Parsed fixtures and spatial scene setup
are outside timed frames; GPU completion/readback and GDI presentation are included.

The machine was a Ryzen 7 9800X3D, Windows 11 Home build 26200, NVIDIA RTX 5070
driver 32.0.16.1088. Hardware selection was confirmed by the renderer. MSVC
19.51.36256.0/v145 built x86 Release with `/O2 /W4 /WX`. Builds and regression
tests were idle during measurement. Separate
[hardware metadata](hardware_metadata.json) and [WARP metadata](warp_metadata.json)
retain source, project, shader, fixture and executable SHA256 hashes and run options.
Both published runs use the same executable hash. Timestamps in metadata are UTC.

After measurement, trailing whitespace was normalized in the two generated
shader headers and their generator, and an extra HLSL EOF blank was removed.
The four original measured source hashes remain in `sources.sha256`; added
`final_sha256` and `change_note` fields record the final formatting. Both compiled
shader byte arrays were verified identical before and after, and shader
generation `-Check` passed. Their payload hashes are retained in
`post_run_formatting`. The measured benchmark executable and all other measured
inputs are unchanged; no timing rerun was required for this formatting cleanup.

Full results are retained in [hardware measurements](hardware_measurements.csv),
[hardware stages](hardware_stages.csv), [WARP measurements](warp_measurements.csv)
and [WARP stages](warp_stages.csv). Exact cache-hit rows contain zero build work;
memory, visible-label count and renderer provenance describe the reused image.

## Hardware current-view costs

Representative dark-palette means and new p95 values, in milliseconds:

| Scene | Old full rebuild + present | Native build + present | Native p95 | Old scaled preview copy |
| --- | ---: | ---: | ---: | ---: |
| LFPG whole, 1920 × 1080 | 46.847 | 4.485 | 7.401 | 1.515 |
| LFPG detail, 23° rotation | 35.036 | 2.913 | 4.148 | 1.389 |
| LFPG whole, fourth-corner +1 px | 45.934 | 7.616 | 13.957 | 1.431 |
| LFPG 2560 × 1440, display scale 4/3 | 57.407 | 6.824 | 10.909 | 2.417 |
| LFPG 3840 × 2160, display scale 2 | 75.485 | 10.763 | 15.128 | 5.408 |
| LFPO whole | 15.807 | 2.494 | 3.665 | 1.355 |
| LFML whole | 6.628 | 1.582 | 3.627 | 1.360 |

Replay includes the initial exact cache hit and the duplicate central zoom value;
the per-frame CSV identifies those hits. It zooms in and out around varied
off-center geographic anchors, with production culling margins and precise double
projection. The non-affine case projects and rebuilds screen paths exactly and
therefore has a higher CPU geometry cost. Main host inverse recovery can avoid
integer-corner cross terms only when the actual inverse passes its validation.
This benchmark does not prove that a live host will pass that gate.

Unchanged LFPG whole presentation remained a direct bitmap copy: 0.142 ms before,
0.135 ms after. At 4K/display scale 2 it was 2.049 ms before, 1.927 ms after.
First-view native initialization was 267 ms for LFPG whole, compared with 50 ms
for the old initial raster, and 356 ms at 4K/display scale 2 versus 76 ms before.
These cold measurements create a fresh renderer per scene after process warmup;
they include device, retained geometry and font preparation, not fixture parsing.
They do not represent every palette or membership change in a retained renderer.

The largest known GPU image/buffer estimate was 108.71 MiB at 4K/display scale 2.
The native CPU DIB adds 31.64 MiB, and retained CPU glyph triangles/bins peaked
at 9.92 MiB. The estimate includes working/staging textures, the single native
glyph overlay and explicit GPU buffers. It excludes driver and Direct2D internal
allocations, other scene/CPU data and other applications, so it is not total VRAM.

## Stability, coordinates and glyph coverage

- Hardware: all 768 active-to-settled pairs had zero changed presented RGB pixels.
  WARP: all 80 pairs also had zero changed pixels. Repeated raw synthetic/native
  transparent frames matched every BGRA byte.
- The geographic marker's raster alpha centroid stayed within 0.0602 pixels of
  its projected center across 40 zoom steps at 17° rotation and a nonzero origin.
  Its projection and precise cache-key probes passed, including deliberately
  conflicting legacy float corners and a 0.375-pixel precise-view change.
- Cursor anchors in the airport replay stayed within the 0.001-pixel requirement.
  The old rectangle/crop transform showed up to 0.732 pixels of continuous
  alignment error before bitmap sampling. Its preview-to-settled images differed
  on millions of accumulated pixels; the native path has no quality replacement.
- Offset inset transparency, premultiplied channels and exactly one translucent
  background pass per frame passed. Eighteen palette/display-scale/opacity/Unicode/
  visibility states matched independently rendered current-view images.
- Immutable geometry/label copies with changed group membership reused shared
  font outlines: zero glyph cache misses, unchanged retained points and pixels.
- Eight renderer lifetimes with eight resizes each released native bitmaps and
  left process GDI counts unchanged at 6. Resource-clear probes reported zero
  known GPU images, CPU glyph cache and bitmap ownership afterward.
- Every nonempty label batch used the actual analytic helper in both matrices;
  no hidden vector fallback satisfied the test. The 94 hardware and 14 WARP frames
  reporting no helper usage had zero visible labels.

The independent physical font oracle covered 768 fractional phases: Arial and
Calibri, `O` and `Aa123 Runway`, display scales 1, 4/3 and 2. Maximum selected
alpha-centroid error against the actual source outline's area centroid was
below 0.017 pixels on both hardware and WARP, below the 0.125-pixel gate.
Source alpha area stayed within the 8-bit edge quantization bound; all repeated
BGRA comparisons were identical. Complete values are in the
[hardware physical CSV](hardware_glyph_physical_centroid.csv) and
[WARP physical CSV](warp_glyph_physical_centroid.csv).

Native antialias coverage differs from Direct2D realizations. The reference's
physical centroid error reached 0.821 pixels, and at one Arial 14 word phase its
alpha area was approximately 1.096 times the source geometry, versus 1.000 for
analytic coverage. Thus a contrast-weighted reference centroid can differ without
a geometric position error. The 576-sample palette/halo/transparency grid records
that style difference in [hardware fidelity](hardware_glyph_fidelity.csv) and
[WARP fidelity](warp_glyph_fidelity.csv). Minimum edge-energy ratios were 0.919
and 0.918 respectively, above the 0.90 gate; bitmap-atlas exploration had reduced
contrast to about 0.67–0.78 and was rejected. The native images were inspected at
native size and remain crisp. No antialias-style switch occurs on settling.

Selected images show [old preview](LFPG_whole_dark_old_preview.png),
[old settled](LFPG_whole_dark_old_settled.png), and
[native current view](LFPG_whole_dark_native.png) at the same replay step.
The correctly exported old settled image is distinct from the native image.
Native font phase grids are retained with [halo](glyph_dark_style0_native.png)
and [opaque fill](glyph_dark_style2_native.png), alongside their Direct2D references
([halo](glyph_dark_style0_reference.png), [fill](glyph_dark_style2_reference.png)).

## WARP and practical limits

The forced software path preserves native quality and stability but is slower.
LFPG whole averaged 36.946 ms, whole fourth-corner +1 px 40.976 ms, detail
bilinear 6.096 ms, LFPO whole 9.019 ms and LFML whole 5.891 ms. These are software
render/readback/presentation costs, not hardware FPS guarantees.

Warm hardware complete rebuilds are substantially faster than the old raster
rebuilds, and idle bitmap presentation is preserved. A crisp changed native
frame still costs more than the old stretched preview copy, and cold creation,
dense non-affine views and high DPI retain material latency. Live host aircraft,
tags, input, other plug-ins and display scheduling are outside this harness.
End-to-end EuroScope zoom traces remain necessary to establish overall interaction
FPS, particularly a 200/240 FPS claim or universal absence of performance regression.
