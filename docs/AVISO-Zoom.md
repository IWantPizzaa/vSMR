# AVISO native zoom rendering

AVISO now renders a changed main or inset view directly from retained vector
geometry at the current viewport's native pixel dimensions. An unchanged view
reuses that exact image. Zoom previews and later high-resolution replacements
have been removed from the visible rendering path, eliminating their sampling
loss and the transition between different coordinate transforms.

This extends the FPS-Killer work at `a76b379`. The existing geographic scene
index, ordered feature culling, immutable snapshots, opaque background baking
and direct bitmap copies remain. The old GDI+ renderer is retained for regression
comparisons and compatibility code, but main and inset presentation use the new
native renderer.

## Confirmed causes

The previous UI enlarged or shrank an already rasterized bitmap while the worker
rebuilt the view. `StretchBlt` with `COLORONCOLOR` and stretched `AlphaBlend`
operate on existing pixels. They cannot recover polygon edges, line thickness
or label details at the new zoom. The following full-resolution raster therefore
looked different even when both images represented the same geographic area.

The preview also converted geographic bounds into projected axis-aligned
rectangles, rounded the source crop and destination independently, and normalized
near-native extents by up to one pixel. The worker fit its projected geographic
AABB to integer raster dimensions. Those continuous transforms are not identical
to drawing geometry in the current viewport's native pixel grid. The replay
measures the resulting subpixel alignment error separately from bitmap sampling.

Independent inset input had additional discrepancies: integer `CenterPoint()`
discarded half a pixel for odd dimensions, rotation helpers narrowed intermediates
to float, and the longitude scale after zoom used the anchor latitude rather than
the solved new center latitude. Drawing, pan and cursor zoom now share one
double-precision geographic projection. The center calculation solves that
latitude-dependent scale consistently and rejects zooms that cannot keep the
anchor within the allowed latitude range.
Inset scale is stored as a continuous double over the existing 1..2400 range.
Fractional wheel deltas change that scale directly, including restored scales
below 20, rather than repeatedly rounding back to an unchanged integer. Existing
ASR preset scale values remain readable.

## Native rendering and cache ownership

`AvisoVectorRenderer` retains geographic path geometry and uses Direct2D over
D3D11. Straight polygon tessellation remains vector geometry, transformed for
the current view and antialiased at native pixels. Screen-space strokes and fonts
keep their configured display size as the geographic zoom changes. The complete
rotated basis is retained; a non-affine fourth-corner term uses the corresponding
bilinear projection.

Labels use their retained GDI font outlines with native analytic coverage. The
GPU computes triangle intersections with the current pixel squares, with 4 x 4
local spatial bins to limit work. Halo and fill retain source order, subpixel
translation and premultiplied alpha. The single native overlay is composited
1:1; it contains no zoomed glyph atlas or oversampled label texture. HLSL and
checked-in precompiled shader bytecode avoid runtime shader compilation. Verify
the generated headers with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File vSMR/src/aviso/shaders/compile_aviso_glyph_shaders.ps1 -Check
```

CPU triangles and bins and packed GPU triangles and bins each have 64 MiB
budgets; per-frame style, vertex and coverage work are also bounded. Native
raster dimensions are limited to 6400 per
axis and 32 million pixels; the glyph surface has its own allocation bound.
Unsupported analytic resources or workloads use native Direct2D vector glyphs
for the scene. Hardware device creation can fall back to WARP. Both fallback
paths retain the same current-view projection and native pixel dimensions.

Render requests can carry the authoritative double projection in addition to
legacy float corners. Main rendering retains the complete host viewport basis;
split layouts crop destination pixels and culling bounds rather than rebuilding
the projection from rounded geographic/pixel conversions. Insets retain their
exact rotated tangent-plane basis, including fractional centers for odd sizes.
When EuroScope's inverse projection reproduces all nine full-view pixel samples
within 1e-5 pixels of an affine fit, main rendering recovers that continuous
basis rather than deriving it from rounded integer forward projections. The
same validated inverse basis supplies inset north rotation. Nonlinear, rounded
or singular inverse mappings retain the SDK-corner projection.

`AvisoViewportRenderer` owns one native image and its exact request identity.
Changing geometry, labels, visibility, palette, background, scale, viewport,
projection or dimensions requires a current-view build. The completed native
image is drawn immediately and becomes the unchanged-view cache. Stopping zoom
does not request or apply a different-quality image. Reusing a cache cannot
stretch it, shift its origin or accept a nearby transform through pixel tolerance.

Opaque frames include their background and use the existing native `BitBlt`
path. Transparent frames retain premultiplied alpha and paint the viewport
background once over the current host underlay. A reusable DIB avoids repeated
allocation for equal-size changed views. Main and inset own separate renderer
state. Cache clear releases its DIB and key; resize replaces native image
resources while retaining the device and reusable geometry. Hide, close and
shutdown release the complete renderer's bitmap, DC, device and glyph resources.

## Validation and measurements

Recorded component timings, phase CSVs, source/binary metadata and selected
reference images are retained in the
[2026-10-13 results](../vSMR/tests/aviso_zoom_benchmark/results/2026-10-13/README.md).

The reproducible [native zoom harness](../vSMR/tests/aviso_zoom_benchmark/README.md)
replays actual bundled LFPG, LFPO and LFML geometry and labels. Its matrix covers
all three palettes, whole maps, rotated details, deliberately non-affine whole
and detail projections, and native viewports through 3840 x 2160, including
display scales 4/3 and 2. It compares each active
frame with the settling frame through the production viewport controller, and
requires identical presented pixels and cache reuse without a second build.

Additional probes verify premultiplied transparency, an offset inset with one
translucent background pass, authoritative precise projections even when float
corners disagree, precise projection cache invalidation, coverage-centroid
alignment of a geographic marker, and GDI resource ownership across repeated
resizes and renderer lifetimes. The native regression suite checks projection
round trips and cursor anchoring separately from the discrete pixel coverage.

Glyph coverage is evaluated against the physical area centroid of the actual
cached GDI font outline, with white opaque fill on transparent pixels. This
distinguishes coordinate error from different native antialias coverage. In the
recorded analytic-coverage run, 768 Arial/Calibri phases across two labels and
three display scales produced a maximum physical centroid error below 0.017 pixels,
compared with 0.821 pixels for the Direct2D realization reference. The latter's
covered alpha area for Arial 14 `Aa123 Runway` was about 1.096 times the source
outline area, while analytic coverage was about 1.000. Consequently,
old-reference RGB/contrast-centroid equality is not a physical-placement test.
The phase grid separately records edge contrast, alpha area, helper usage and
exact repeated-frame stability. Different native antialias style does not
introduce an active-to-settled transition.

The explored font bitmap atlas was rejected: it reduced edge contrast, looked
soft, added memory and did not improve timing. Native command-list replay and
GDI-compatible GPU readback also failed to improve the measured renderer.

Performance has two distinct comparisons. A complete native current-view build
and presentation replaces the previous expensive raster rebuild. An unchanged
view still uses a direct cached bitmap copy. A changed crisp vector frame does
more work than the previous stretched-image preview copy, so faster complete
rebuilds and preserved cache presentation alone cannot establish that every
EuroScope interaction frame has the same or better FPS.

The standalone timings include GPU completion/readback and GDI presentation.
They exclude host refresh scheduling, live aircraft/tag work, input dispatch,
other plug-ins and display presentation. Dense map views and initial
device/font/geometry setup remain material costs. Main zoom recovers a continuous
host projection only when its inverse samples pass validation; otherwise it
inherits the integer precision of the SDK corners. An end-to-end host
interaction trace is required to establish a 200/240 FPS result or a universal
latency guarantee.
