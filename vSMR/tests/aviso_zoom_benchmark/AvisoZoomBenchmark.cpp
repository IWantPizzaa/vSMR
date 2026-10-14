#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>
#include "BenchmarkFixture.hpp"
#include "aviso/AvisoRasterBlitter.hpp"
#include "aviso/AvisoRasterRenderer.hpp"
#include "aviso/AvisoRenderScene.hpp"
#include "aviso/AvisoRasterSizing.hpp"
#include "aviso/AvisoVectorRenderer.hpp"
#include "aviso/AvisoViewportRenderer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>

using namespace Gdiplus;
using namespace VsmrRadarTypes;
using namespace VsmrAvisoBenchmark;
namespace fs = std::filesystem;

namespace
{
    VsmrAviso::AvisoVectorRenderer::TextMode TextMode = VsmrAviso::AvisoVectorRenderer::TextMode::NativeAnalytic;
    using Clock = std::chrono::steady_clock;
    double Milliseconds(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    struct BitmapView
    {
        DIBSECTION dib{};
        explicit BitmapView(HBITMAP bitmap)
        {
            Require(GetObject(bitmap, sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits,
                "Expected a readable 32-bit DIB");
        }
        const BYTE* Pixels() const { return static_cast<const BYTE*>(dib.dsBm.bmBits); }
    };

    struct Difference
    {
        std::uint64_t pixels = 0;
        int maximum = 0;
        double mean = 0;
        void Accumulate(const Difference& other)
        {
            pixels += other.pixels;
            maximum = (std::max)(maximum, other.maximum);
            mean += other.mean;
        }
    };

    Difference Compare(HBITMAP a, HBITMAP b, bool rgbOnly = true)
    {
        GdiFlush();
        const BitmapView left(a), right(b);
        Require(left.dib.dsBm.bmWidth == right.dib.dsBm.bmWidth && left.dib.dsBm.bmHeight == right.dib.dsBm.bmHeight,
            "Image dimensions differ");
        Difference result;
        std::uint64_t sum = 0;
        const int channels = rgbOnly ? 3 : 4;
        const std::size_t count = static_cast<std::size_t>(left.dib.dsBm.bmWidth) * left.dib.dsBm.bmHeight;
        for (std::size_t i = 0; i < count; ++i)
        {
            bool changed = false;
            for (int c = 0; c < channels; ++c)
            {
                const auto offset = i * 4 + c;
                const int delta = std::abs(static_cast<int>(left.Pixels()[offset]) - right.Pixels()[offset]);
                changed |= delta != 0;
                result.maximum = (std::max)(result.maximum, delta);
                sum += delta;
            }
            result.pixels += changed;
        }
        result.mean = static_cast<double>(sum) / (count * channels);
        return result;
    }

    void Save(HBITMAP handle, const fs::path& path)
    {
        const BitmapView view(handle);
        Bitmap bitmap(view.dib.dsBm.bmWidth, view.dib.dsBm.bmHeight, view.dib.dsBm.bmWidthBytes,
            PixelFormat32bppPARGB, const_cast<BYTE*>(view.Pixels()));
        UINT count = 0, bytes = 0;
        GetImageEncodersSize(&count, &bytes);
        std::vector<BYTE> storage(bytes);
        auto* encoders = reinterpret_cast<ImageCodecInfo*>(storage.data());
        GetImageEncoders(count, bytes, encoders);
        for (UINT i = 0; i < count; ++i)
            if (std::wstring(encoders[i].MimeType) == L"image/png")
            {
                Require(bitmap.Save(path.c_str(), &encoders[i].Clsid) == Ok, "PNG save failed");
                return;
            }
        throw std::runtime_error("No PNG encoder");
    }

    struct Surface
    {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        std::unique_ptr<Graphics> graphics;
        Surface(int width, int height)
        {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            void* pixels = nullptr;
            bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            dc = CreateCompatibleDC(nullptr);
            Require(bitmap && dc, "Cannot allocate benchmark surface");
            previous = SelectObject(dc, bitmap);
            graphics = std::make_unique<Graphics>(dc);
        }
        ~Surface()
        {
            graphics.reset();
            if (dc) { SelectObject(dc, previous); DeleteDC(dc); }
            if (bitmap) DeleteObject(bitmap);
        }
        void Clear(Color color) { Require(graphics->Clear(color) == Ok, "Surface clear failed"); }
    };

    struct Statistics
    {
        double mean = 0, median = 0, p95 = 0, maximum = 0;
        explicit Statistics(std::vector<double> values)
        {
            if (values.empty()) return;
            mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
            std::sort(values.begin(), values.end());
            median = values[values.size() / 2];
            p95 = values[static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1];
            maximum = values.back();
        }
    };

    void Row(std::ostream& csv, const std::string& scene, const char* operation,
        const std::vector<double>& oldTimes, const std::vector<double>& newTimes,
        const Difference& oldHandoff, const Difference& newHandoff, double oldAlignment,
        double cursorError, double cold, bool hardware, double oldCold = 0)
    {
        const Statistics old(oldTimes), changed(newTimes);
        csv << scene << ',' << operation << ',' << (std::max)(oldTimes.size(), newTimes.size()) << ',' << old.mean << ',' << changed.mean << ','
            << old.median << ',' << changed.median << ',' << old.p95 << ',' << changed.p95 << ',' << old.maximum << ','
            << changed.maximum << ',' << oldHandoff.pixels << ',' << oldHandoff.maximum << ',' << oldHandoff.mean << ','
            << newHandoff.pixels << ',' << newHandoff.maximum << ',' << newHandoff.mean << ',' << oldAlignment << ','
            << cursorError << ',' << cold << ',' << hardware << ',' << oldCold << '\n';
        std::cout << scene << ' ' << operation << ": " << std::fixed << std::setprecision(3) << old.mean << " -> "
            << changed.mean << " ms; old/new handoff pixels=" << oldHandoff.pixels << '/' << newHandoff.pixels
            << "; old alignment=" << oldAlignment << " px; anchor=" << cursorError << " px" << std::endl;
    }

    PointF Project(const AvisoRasterRenderRequest& r, double longitude, double latitude)
    {
        const double u = (longitude - r.displayMinLongitude) / (r.displayMaxLongitude - r.displayMinLongitude);
        const double v = (r.displayMaxLatitude - latitude) / (r.displayMaxLatitude - r.displayMinLatitude);
        const double topX = r.projectedTopLeft.X + static_cast<double>(r.projectedTopRight.X - r.projectedTopLeft.X) * u;
        const double bottomX = r.projectedBottomLeft.X + static_cast<double>(r.projectedBottomRight.X - r.projectedBottomLeft.X) * u;
        const double topY = r.projectedTopLeft.Y + static_cast<double>(r.projectedTopRight.Y - r.projectedTopLeft.Y) * u;
        const double bottomY = r.projectedBottomLeft.Y + static_cast<double>(r.projectedBottomRight.Y - r.projectedBottomLeft.Y) * u;
        return PointF(static_cast<REAL>(topX + (bottomX - topX) * v), static_cast<REAL>(topY + (bottomY - topY) * v));
    }

    VsmrAviso::PointD ProjectNative(const AvisoRasterRenderRequest& r, double longitude, double latitude)
    {
        if (r.preciseProjection.has_value()) return r.preciseProjection->Project(longitude, latitude);
        const auto point = Project(r, longitude, latitude);
        return { point.X, point.Y };
    }

    VsmrAviso::BilinearProjection Rebase(const VsmrAviso::BilinearProjection& basis,
        const VsmrAviso::GeoBounds& destinationBounds, double offsetX = 0, double offsetY = 0)
    {
        const auto& bounds = basis.Bounds();
        auto corner = [&](double lon, double lat) {
            auto point = basis.Project(lon, lat);
            point.X += offsetX; point.Y += offsetY;
            return point;
        };
        return VsmrAviso::BilinearProjection::FromCorners(destinationBounds,
            corner(bounds.minLongitude, bounds.maxLatitude), corner(bounds.maxLongitude, bounds.maxLatitude),
            corner(bounds.minLongitude, bounds.minLatitude), corner(bounds.maxLongitude, bounds.minLatitude));
    }

    RectF Bounds(const AvisoRasterRenderRequest& projection, double minLon, double minLat, double maxLon, double maxLat)
    {
        const PointF points[] = { Project(projection, minLon, maxLat), Project(projection, maxLon, maxLat),
            Project(projection, minLon, minLat), Project(projection, maxLon, minLat) };
        const REAL left = (std::min)({ points[0].X, points[1].X, points[2].X, points[3].X });
        const REAL top = (std::min)({ points[0].Y, points[1].Y, points[2].Y, points[3].Y });
        const REAL right = (std::max)({ points[0].X, points[1].X, points[2].X, points[3].X });
        const REAL bottom = (std::max)({ points[0].Y, points[1].Y, points[2].Y, points[3].Y });
        return RectF(left, top, right - left, bottom - top);
    }

    AvisoRasterRenderRequest MakeRequest(const Fixture& fixture, const fs::path& path, double zoom,
        double rotation, int width, int height)
    {
        AvisoRasterRenderRequest r;
        r.path = path.u8string(); r.features = fixture.features; r.labels = fixture.labels; r.groupVisibility = fixture.visibility;
        r.rasterWidth = width; r.rasterHeight = height;
        r.viewportZoomLevel = zoom > 1 ? 14 : 8;
        double centerLon = (fixture.minLongitude + fixture.maxLongitude) * 0.5;
        double centerLat = (fixture.minLatitude + fixture.maxLatitude) * 0.5;
        if (zoom > 1 && !fixture.labels->empty())
        {
            std::vector<double> longitudes, latitudes;
            for (const auto& label : *fixture.labels) { longitudes.push_back(label.position.longitude); latitudes.push_back(label.position.latitude); }
            std::sort(longitudes.begin(), longitudes.end()); std::sort(latitudes.begin(), latitudes.end());
            centerLon = longitudes[longitudes.size() / 2]; centerLat = latitudes[latitudes.size() / 2];
        }
        const double halfLon = (fixture.maxLongitude - fixture.minLongitude) * 0.55 / zoom;
        const double halfLat = (fixture.maxLatitude - fixture.minLatitude) * 0.55 / zoom;
        r.displayMinLongitude = r.renderMinLongitude = centerLon - halfLon;
        r.displayMaxLongitude = r.renderMaxLongitude = centerLon + halfLon;
        r.displayMinLatitude = r.renderMinLatitude = centerLat - halfLat;
        r.displayMaxLatitude = r.renderMaxLatitude = centerLat + halfLat;
        r.scaleX = width / (2 * halfLon); r.scaleY = height / (2 * halfLat);
        const double angle = rotation * 3.14159265358979323846 / 180;
        auto rotate = [&](double x, double y) {
            const double dx = x - width * 0.5, dy = y - height * 0.5;
            return VsmrAviso::PointD{ width * 0.5 + dx * std::cos(angle) - dy * std::sin(angle),
                height * 0.5 + dx * std::sin(angle) + dy * std::cos(angle) };
        };
        const auto tl = rotate(0, 0), tr = rotate(width, 0), bl = rotate(0, height), br = rotate(width, height);
        r.projectedTopLeft = PointF(static_cast<REAL>(tl.X), static_cast<REAL>(tl.Y));
        r.projectedTopRight = PointF(static_cast<REAL>(tr.X), static_cast<REAL>(tr.Y));
        r.projectedBottomLeft = PointF(static_cast<REAL>(bl.X), static_cast<REAL>(bl.Y));
        r.projectedBottomRight = PointF(static_cast<REAL>(br.X), static_cast<REAL>(br.Y));
        r.preciseProjection = VsmrAviso::BilinearProjection::FromCorners(
            {r.displayMinLongitude, r.displayMinLatitude, r.displayMaxLongitude, r.displayMaxLatitude}, tl, tr, bl, br);
        return r;
    }

    AvisoRasterRenderRequest Overscan(AvisoRasterRenderRequest r)
    {
        const double margin = VsmrAviso::NativeResolutionOverscan(r.projectedTopLeft, r.projectedTopRight,
            r.projectedBottomLeft, r.projectedBottomRight, 32000000.0);
        const double lonMargin = (r.displayMaxLongitude - r.displayMinLongitude) * margin;
        const double latMargin = (r.displayMaxLatitude - r.displayMinLatitude) * margin;
        r.renderMinLongitude = r.displayMinLongitude - lonMargin; r.renderMaxLongitude = r.displayMaxLongitude + lonMargin;
        r.renderMinLatitude = r.displayMinLatitude - latMargin; r.renderMaxLatitude = r.displayMaxLatitude + latMargin;
        const auto bounds = Bounds(r, r.renderMinLongitude, r.renderMinLatitude, r.renderMaxLongitude, r.renderMaxLatitude);
        r.renderScreenLeft = bounds.X; r.renderScreenTop = bounds.Y;
        r.rasterWidth = (std::max)(1, static_cast<int>(std::floor(bounds.Width)));
        r.rasterHeight = (std::max)(1, static_cast<int>(std::floor(bounds.Height)));
        return r;
    }

    AvisoRasterRenderRequest NativeCullingBounds(AvisoRasterRenderRequest r)
    {
        const auto projection = r.preciseProjection.has_value() ? *r.preciseProjection :
            VsmrAviso::BilinearProjection::FromCorners(
                VsmrAviso::GeoBounds{ r.displayMinLongitude, r.displayMinLatitude, r.displayMaxLongitude, r.displayMaxLatitude },
                r.projectedTopLeft, r.projectedTopRight, r.projectedBottomLeft, r.projectedBottomRight);
        double minLon = r.displayMinLongitude, maxLon = r.displayMaxLongitude;
        double minLat = r.displayMinLatitude, maxLat = r.displayMaxLatitude;
        for (const auto point : { VsmrAviso::PointD{ 0, 0 }, VsmrAviso::PointD{ double(r.rasterWidth), 0 },
            VsmrAviso::PointD{ 0, double(r.rasterHeight) }, VsmrAviso::PointD{ double(r.rasterWidth), double(r.rasterHeight) } })
        {
            VsmrAviso::GeoPoint geographic;
            if (projection.TryUnproject(point, geographic))
            {
                minLon = (std::min)(minLon, geographic.longitude); maxLon = (std::max)(maxLon, geographic.longitude);
                minLat = (std::min)(minLat, geographic.latitude); maxLat = (std::max)(maxLat, geographic.latitude);
            }
        }
        const double marginLon = (maxLon - minLon) * 0.5, marginLat = (maxLat - minLat) * 0.5;
        r.renderMinLongitude = minLon - marginLon; r.renderMaxLongitude = maxLon + marginLon;
        r.renderMinLatitude = minLat - marginLat; r.renderMaxLatitude = maxLat + marginLat;
        return r;
    }

    // Freeze the a76b379 viewport-aligned preview's continuous AABB mapping,
    // floor/ceil source crop, expanded rounded destination, and blitter extent policy.
    bool OldPreviewPlan(const AvisoRasterRenderRequest& cached, const AvisoRasterRenderRequest& current,
        RECT& source, RECT& destination)
    {
        const auto cache = Bounds(cached, cached.renderMinLongitude, cached.renderMinLatitude,
            cached.renderMaxLongitude, cached.renderMaxLatitude);
        const auto selection = Bounds(cached, current.displayMinLongitude, current.displayMinLatitude,
            current.displayMaxLongitude, current.displayMaxLatitude);
        const auto dest = Bounds(current, current.displayMinLongitude, current.displayMinLatitude,
            current.displayMaxLongitude, current.displayMaxLatitude);
        const double sourceX = (selection.X - cache.X) * cached.rasterWidth / cache.Width;
        const double sourceY = (selection.Y - cache.Y) * cached.rasterHeight / cache.Height;
        const double sourceRight = (selection.GetRight() - cache.X) * cached.rasterWidth / cache.Width;
        const double sourceBottom = (selection.GetBottom() - cache.Y) * cached.rasterHeight / cache.Height;
        if (sourceX < -1e-6 || sourceY < -1e-6 || sourceRight > cached.rasterWidth + 1e-6 || sourceBottom > cached.rasterHeight + 1e-6)
            return false;
        source = { static_cast<LONG>(std::floor(sourceX)), static_cast<LONG>(std::floor(sourceY)),
            static_cast<LONG>(std::ceil(sourceRight)), static_cast<LONG>(std::ceil(sourceBottom)) };
        source.left = std::clamp<LONG>(source.left, 0, cached.rasterWidth);
        source.top = std::clamp<LONG>(source.top, 0, cached.rasterHeight);
        source.right = std::clamp<LONG>(source.right, source.left, cached.rasterWidth);
        source.bottom = std::clamp<LONG>(source.bottom, source.top, cached.rasterHeight);
        const double xScale = dest.Width / (sourceRight - sourceX), yScale = dest.Height / (sourceBottom - sourceY);
        destination = { static_cast<LONG>(std::lround(dest.X + (source.left - sourceX) * xScale)),
            static_cast<LONG>(std::lround(dest.Y + (source.top - sourceY) * yScale)),
            static_cast<LONG>(std::lround(dest.X + (source.right - sourceX) * xScale)),
            static_cast<LONG>(std::lround(dest.Y + (source.bottom - sourceY) * yScale)) };
        return true;
    }

    double OldAlignmentError(const AvisoRasterRenderRequest& cached, const AvisoRasterRenderRequest& current,
        const RECT& source, const RECT& destination)
    {
        VsmrAviso::RasterBlitPlan plan;
        const RECT clip{ 0, 0, current.rasterWidth, current.rasterHeight };
        Require(VsmrAviso::TryBuildRasterBlitPlan(source, destination, clip, plan), "Invalid old preview plan");
        const auto cache = Bounds(cached, cached.renderMinLongitude, cached.renderMinLatitude,
            cached.renderMaxLongitude, cached.renderMaxLatitude);
        double maximum = 0;
        for (double u : { 0.15, 0.5, 0.85 })
            for (double v : { 0.15, 0.5, 0.85 })
            {
                const double lon = current.displayMinLongitude + (current.displayMaxLongitude - current.displayMinLongitude) * u;
                const double lat = current.displayMaxLatitude - (current.displayMaxLatitude - current.displayMinLatitude) * v;
                const auto old = Project(cached, lon, lat), exact = Project(current, lon, lat);
                const double rasterX = (old.X - cache.X) * cached.rasterWidth / cache.Width;
                const double rasterY = (old.Y - cache.Y) * cached.rasterHeight / cache.Height;
                const double shownX = plan.destination.left + (rasterX - source.left) * plan.destinationWidth / plan.sourceWidth;
                const double shownY = plan.destination.top + (rasterY - source.top) * plan.destinationHeight / plan.sourceHeight;
                maximum = (std::max)(maximum, std::hypot(shownX - exact.X, shownY - exact.Y));
            }
        return maximum;
    }

    void Present(VsmrAviso::AvisoRasterBlitter& blitter, Surface& surface, HBITMAP bitmap,
        const RECT& source, const RECT& destination, bool opaque, Color background, int width, int height)
    {
        const RECT clip{ 0, 0, width, height };
        VsmrAviso::RasterBlitPlan plan;
        Require(VsmrAviso::TryBuildRasterBlitPlan(source, destination, clip, plan), "Presentation plan failed");
        if (!opaque || plan.destination.left > 0 || plan.destination.top > 0 ||
            plan.destination.right < width || plan.destination.bottom < height)
            surface.Clear(background);
        Require(blitter.Blend(*surface.graphics, surface.dc, bitmap, source, destination, clip, opaque), "Bitmap presentation failed");
        GdiFlush();
    }

    bool RunScene(const Fixture& fixture, const fs::path& output, const std::string& name,
        AvisoRasterRenderRequest base, int frames, std::ostream& csv, std::ostream& stages,
        VsmrAviso::AvisoVectorRenderer::Backend backend)
    {
        const int width = base.rasterWidth, height = base.rasterHeight;
        base.scene = VsmrAviso::AvisoRenderScene::Build(*base.features, *base.labels);
        base = NativeCullingBounds(std::move(base));
        const std::size_t palette = base.colorPalette == "dark" ? 0 : base.colorPalette == "light" ? 1 : 2;
        const Color background = fixture.backgrounds[palette];
        base.backgroundColor = background;
        VsmrAviso::AvisoRasterRenderer oldRenderer;
        VsmrAviso::AvisoViewportRenderer viewport(backend, TextMode);
        Surface oldTarget(width, height), finalTarget(width, height), vectorTarget(width, height);
        VsmrAviso::AvisoRasterBlitter blitter;
        const RECT nativeRect{ 0, 0, width, height };
        auto cachedRequest = Overscan(base);
        auto started = Clock::now();
        auto cached = oldRenderer.Render(cachedRequest);
        Require(cached && cached->bitmap, "Old renderer failed");
        const double oldCold = Milliseconds(started);
        started = Clock::now();
        auto status = viewport.Draw(*vectorTarget.graphics, vectorTarget.dc, nativeRect, base, {});
        Require(status.drawn, "Vector renderer failed");
        const double cold = Milliseconds(started);
        std::cout << "  " << name << " vector hardware=" << status.hardware << ", cold=" << cold << " ms" << std::endl;
        std::cout << "  backend=" << viewport.BackendDescription() << std::endl;
        const auto coldTimings = viewport.LastTimings();
        const auto coldStats = viewport.LastStats();
        stages << name << ",-1," << status.cacheHit << ',' << coldTimings.prepareMilliseconds << ','
            << coldTimings.geometryMilliseconds << ',' << coldTimings.labelMilliseconds << ','
            << coldTimings.flushMilliseconds << ',' << coldTimings.readbackMilliseconds << ','
            << status.buildMilliseconds << ',' << cold << ',' << coldStats.glyphCacheHits << ','
            << coldStats.glyphCacheMisses << ',' << coldStats.featureCandidates << ','
            << coldStats.labelCandidates << ',' << coldStats.pathsDrawn << ','
            << coldStats.projectedVertices << ',' << viewport.CachedGlyphPoints() << ','
            << viewport.EstimatedGpuImageBytes() << ',' << viewport.CachedNativeMeshBytes() << ','
            << viewport.UsesNativeLabels() << ',' << viewport.LastVisibleLabelCount() << ','
            << coldTimings.glyphMilliseconds << ',' << coldTimings.meshSetupMilliseconds << ','
            << coldTimings.meshBuildMilliseconds << ',' << coldTimings.meshPackMilliseconds << ','
            << coldTimings.meshDrawMilliseconds << ',' << coldTimings.meshFillMilliseconds << ','
            << coldTimings.meshHaloMilliseconds << ',' << coldTimings.meshBinMilliseconds << '\n';
        std::vector<double> oldRebuild, oldPreview, changed, unchanged;
        Difference oldHandoff, newHandoff;
        double alignment = 0, cursorError = 0;
        const double initialLonSpan = base.displayMaxLongitude - base.displayMinLongitude;
        const double initialLatSpan = base.displayMaxLatitude - base.displayMinLatitude;
        // A varied off-center anchor in a rotated viewport; both directions return
        // to the same geographic bounds. Alternate anchors between scenes.
        const double anchorU = name.find("detail") == std::string::npos ? 0.2731 : 0.8173;
        const double anchorV = name.find("detail") == std::string::npos ? 0.6817 : 0.2291;
        const double anchorLon = base.displayMinLongitude + initialLonSpan * anchorU;
        const double anchorLat = base.displayMaxLatitude - initialLatSpan * anchorV;
        const auto initialAnchor = ProjectNative(base, anchorLon, anchorLat);
        AvisoRasterRenderRequest current = base;
        for (int i = 0; i < frames; ++i)
        {
            const double progress = static_cast<double>(i) / (frames - 1);
            const double zoom = std::exp(std::sin(progress * 3.14159265358979323846) * 0.85);
            current.displayMinLongitude = current.renderMinLongitude = anchorLon - initialLonSpan / zoom * anchorU;
            current.displayMaxLongitude = current.renderMaxLongitude = anchorLon + initialLonSpan / zoom * (1 - anchorU);
            current.displayMinLatitude = current.renderMinLatitude = anchorLat - initialLatSpan / zoom * (1 - anchorV);
            current.displayMaxLatitude = current.renderMaxLatitude = anchorLat + initialLatSpan / zoom * anchorV;
            current.scaleX = base.scaleX * zoom; current.scaleY = base.scaleY * zoom;
            if (base.preciseProjection.has_value())
                current.preciseProjection = Rebase(*base.preciseProjection,
                    { current.displayMinLongitude, current.displayMinLatitude, current.displayMaxLongitude, current.displayMaxLatitude });
            current = NativeCullingBounds(std::move(current));
            const auto anchor = ProjectNative(current, anchorLon, anchorLat);
            cursorError = (std::max)(cursorError, static_cast<double>(std::hypot(anchor.X - initialAnchor.X, anchor.Y - initialAnchor.Y)));
            RECT source{}, destination{};
            // If a sparse replay outruns the geographic margin, emulate the
            // completed worker catching up before the next preview is displayed.
            if (!OldPreviewPlan(cachedRequest, current, source, destination))
            {
                cachedRequest = Overscan(current);
                cached = oldRenderer.Render(cachedRequest);
                Require(cached && cached->bitmap, "Old coverage recovery failed");
                Require(OldPreviewPlan(cachedRequest, current, source, destination), "Old overscan failed to cover replay");
            }
            alignment = (std::max)(alignment, OldAlignmentError(cachedRequest, current, source, destination));
            started = Clock::now();
            Present(blitter, oldTarget, cached->bitmap, source, destination, true, background, width, height);
            oldPreview.push_back(Milliseconds(started));
            auto finalRequest = Overscan(current);
            started = Clock::now();
            auto oldFinal = oldRenderer.Render(finalRequest);
            Require(oldFinal && oldFinal->bitmap, "Old settled render failed");
            RECT finalSource{}, finalDestination{};
            Require(OldPreviewPlan(finalRequest, current, finalSource, finalDestination), "Old settled plan failed");
            Present(blitter, finalTarget, oldFinal->bitmap, finalSource, finalDestination, true, background, width, height);
            oldRebuild.push_back(Milliseconds(started));
            const auto handoff = Compare(oldTarget.bitmap, finalTarget.bitmap);
            oldHandoff.Accumulate(handoff);
            started = Clock::now();
            status = viewport.Draw(*vectorTarget.graphics, vectorTarget.dc, nativeRect, current, {});
            Require(status.drawn, "Current vector render failed");
            GdiFlush();
            changed.push_back(Milliseconds(started));
            const auto timings = status.buildAttempted ? viewport.LastTimings() : VsmrAviso::AvisoVectorRenderer::Timings{};
            const auto counters = status.buildAttempted ? viewport.LastStats() : VsmrAviso::AvisoRasterRenderStats{};
            stages << name << ',' << i << ',' << status.cacheHit << ',' << timings.prepareMilliseconds << ','
                << timings.geometryMilliseconds << ',' << timings.labelMilliseconds << ',' << timings.flushMilliseconds << ','
                << timings.readbackMilliseconds << ',' << status.buildMilliseconds << ',' << changed.back() << ','
                << counters.glyphCacheHits << ',' << counters.glyphCacheMisses << ',' << counters.featureCandidates << ','
                << counters.labelCandidates << ',' << counters.pathsDrawn << ',' << counters.projectedVertices << ','
                << viewport.CachedGlyphPoints() << ',' << viewport.EstimatedGpuImageBytes() << ','
                << viewport.CachedNativeMeshBytes() << ',' << viewport.UsesNativeLabels() << ',' << viewport.LastVisibleLabelCount() << ','
                << timings.glyphMilliseconds << ',' << timings.meshSetupMilliseconds << ','
                << timings.meshBuildMilliseconds << ',' << timings.meshPackMilliseconds << ','
                << timings.meshDrawMilliseconds << ',' << timings.meshFillMilliseconds << ','
                << timings.meshHaloMilliseconds << ',' << timings.meshBinMilliseconds << '\n';
            if (TextMode != VsmrAviso::AvisoVectorRenderer::TextMode::Vector && viewport.LastVisibleLabelCount() != 0)
                Require(viewport.UsesNativeLabels(),"A nonempty native label batch silently used vector fallback");
            if (i == frames / 3)
            {
                // Preserve the old settled presentation before the following
                // native cache draw intentionally reuses this destination.
                Save(oldTarget.bitmap, output / (name + "_old_preview.png"));
                Save(finalTarget.bitmap, output / (name + "_old_settled.png"));
                Save(vectorTarget.bitmap, output / (name + "_vector.png"));
            }
            // Simulate the settling/completion boundary: rendering the same current
            // projection must never substitute pixels from a different transform.
            const HBITMAP currentBitmap = viewport.Bitmap();
            auto settled = viewport.Draw(*finalTarget.graphics, finalTarget.dc, nativeRect, current, {});
            Require(settled.drawn && settled.cacheHit && !settled.buildAttempted && viewport.Bitmap() == currentBitmap,
                "Settled native viewport must reuse the current image without rebuilding");
            newHandoff.Accumulate(Compare(vectorTarget.bitmap, finalTarget.bitmap));
            // A 4-frame asynchronous completion lag reproduces the preview
            // sampling loss; the old raster is periodically refreshed, never held
            // for the whole zoom. Outward motion has enough geographic overscan.
            if (i % 4 == 3) { cachedRequest = std::move(finalRequest); cached = std::move(oldFinal); }
        }
        oldHandoff.mean /= frames; newHandoff.mean /= frames;
        Row(csv, name, "motion_rebuild_present", oldRebuild, changed, oldHandoff, newHandoff, alignment, cursorError, cold, status.hardware, oldCold);
        Row(csv, name, "old_preview_present", oldPreview, {}, {}, {}, alignment, cursorError, 0, status.hardware);
        std::vector<double> oldIdle;
        RECT oldSource{}, oldDestination{};
        Require(OldPreviewPlan(cachedRequest, current, oldSource, oldDestination), "Idle old cache plan failed");
        for (int i = 0; i < frames * 4; ++i)
        {
            auto oldPresent = [&]() {
                const auto begin = Clock::now();
                Require(blitter.Blend(*oldTarget.graphics, oldTarget.dc, cached->bitmap, oldSource, oldDestination, nativeRect, true), "Old idle present failed");
                GdiFlush(); oldIdle.push_back(Milliseconds(begin));
            };
            auto currentPresent = [&]() {
                const auto begin = Clock::now();
                status = viewport.Draw(*vectorTarget.graphics, vectorTarget.dc, nativeRect, current, {});
                Require(status.drawn && status.cacheHit && !status.buildAttempted, "Vector idle present failed");
                GdiFlush(); unchanged.push_back(Milliseconds(begin));
            };
            if (i % 2 == 0) { oldPresent(); currentPresent(); }
            else { currentPresent(); oldPresent(); }
        }
        Row(csv, name, "idle_cached_present", oldIdle, unchanged, {}, {}, 0, 0, 0, status.hardware);
        return newHandoff.pixels == 0 && cursorError < 0.001;
    }

    bool ProbeTransparent(AvisoRasterRenderRequest request, std::ostream& csv,
        const fs::path& output, VsmrAviso::AvisoVectorRenderer::Backend backend)
    {
        request.scene = VsmrAviso::AvisoRenderScene::Build(*request.features, *request.labels);
        VsmrAviso::AvisoVectorRenderer renderer(backend, TextMode);
        auto result = renderer.Render(request);
        auto repeat = renderer.Render(request);
        Require(result && repeat, "Transparent render failed");
        const auto diff = Compare(result->bitmap, repeat->bitmap, false);
        const BitmapView view(result->bitmap);
        bool premultiplied = true;
        for (std::size_t i = 0; i < static_cast<std::size_t>(request.rasterWidth) * request.rasterHeight; ++i)
            for (int c = 0; c < 3; ++c) premultiplied &= view.Pixels()[i * 4 + c] <= view.Pixels()[i * 4 + 3];
        Save(result->bitmap, output / "LFPG_transparent_vector.png");
        Row(csv, "LFPG_transparent", "same_projection_stability", {}, {}, {}, diff, 0, 0, 0, renderer.UsesHardware());
        // Origin translation is part of the native transform, not the render bounds.
        auto translated = request;
        for (PointF* p : { &translated.projectedTopLeft, &translated.projectedTopRight,
            &translated.projectedBottomLeft, &translated.projectedBottomRight }) { p->X += 137; p->Y += 53; }
        if (translated.preciseProjection.has_value())
            translated.preciseProjection = Rebase(*request.preciseProjection, request.preciseProjection->Bounds(), 137, 53);
        auto translatedResult = renderer.Render(translated, 137, 53);
        Require(translatedResult != nullptr, "Translated render failed");
        const auto originDiff = Compare(result->bitmap, translatedResult->bitmap, false);
        Row(csv, "LFPG_transparent", "integer_screen_origin", {}, {}, {}, originDiff, 0, 0, 0, renderer.UsesHardware());
        Surface inset(2200, 1300), settledInset(2200, 1300);
        VsmrAviso::AvisoViewportRenderer viewport(backend, TextMode);
        const RECT area{ 137, 53, 137 + request.rasterWidth, 53 + request.rasterHeight };
        unsigned backgroundCalls = 0;
        auto draw = [&](Surface& destination) {
            destination.Clear(Color(255, 20, 40, 60)); // A fresh host underlay for every frame.
            return viewport.Draw(*destination.graphics, destination.dc, area, translated, [&]() {
                ++backgroundCalls;
                SolidBrush brush(Color(128, 100, 130, 160));
                Require(destination.graphics->FillRectangle(&brush, Rect(area.left, area.top,
                    area.right - area.left, area.bottom - area.top)) == Ok, "Transparent inset background failed");
            });
        };
        const auto first = draw(inset);
        const HBITMAP firstBitmap = viewport.Bitmap();
        const auto idle = draw(settledInset);
        const auto insetDiff = Compare(inset.bitmap, settledInset.bitmap);
        Row(csv, "LFPG_transparent", "inset_origin_background_once", {}, {}, {}, insetDiff, 0, 0, 0, idle.hardware);
        return premultiplied && diff.pixels == 0 && originDiff.pixels == 0 && first.drawn && idle.drawn &&
            idle.cacheHit && !idle.buildAttempted && viewport.Bitmap() == firstBitmap && backgroundCalls == 2 && insetDiff.pixels == 0;
    }

    bool ProbeMarker(std::ostream& csv, const fs::path& output, VsmrAviso::AvisoVectorRenderer::Backend backend)
    {
        auto features = std::make_shared<std::vector<AvisoFeature>>();
        AvisoFeature marker;
        marker.polygon = true;
        marker.minLongitude = 10.003141 - 0.00023; marker.maxLongitude = 10.003141 + 0.00023;
        marker.minLatitude = 50.002718 - 0.00031; marker.maxLatitude = 50.002718 + 0.00031;
        marker.paths = { { { marker.minLongitude, marker.minLatitude }, { marker.maxLongitude, marker.minLatitude },
            { marker.maxLongitude, marker.maxLatitude }, { marker.minLongitude, marker.maxLatitude },
            { marker.minLongitude, marker.minLatitude } } };
        marker.fillColor = marker.lightFillColor = marker.realFillColor = Color(255, 255, 255, 255);
        marker.strokeColor = marker.lightStrokeColor = marker.realStrokeColor = Color(0, 0, 0, 0);
        features->push_back(marker);
        AvisoRasterRenderRequest request;
        request.features = features;
        request.labels = std::make_shared<const std::vector<AvisoLabel>>();
        request.scene = VsmrAviso::AvisoRenderScene::Build(*request.features, *request.labels);
        request.groupVisibility = std::make_shared<const std::unordered_map<std::string, bool>>();
        request.path = "synthetic marker";
        request.rasterWidth = 512; request.rasterHeight = 384; request.viewportZoomLevel = 20;
        const double originX = 137, originY = 53;
        const double radians = 17.0 * 3.14159265358979323846 / 180;
        auto corner = [&](double x, double y) {
            return PointF(static_cast<REAL>(originX + 256 + (x - 256) * std::cos(radians) - (y - 192) * std::sin(radians)),
                static_cast<REAL>(originY + 192 + (x - 256) * std::sin(radians) + (y - 192) * std::cos(radians)));
        };
        request.projectedTopLeft = corner(0, 0); request.projectedTopRight = corner(512, 0);
        request.projectedBottomLeft = corner(0, 384); request.projectedBottomRight = corner(512, 384);
        VsmrAviso::AvisoVectorRenderer renderer(backend, TextMode);
        double maximum = 0;
        Difference handoff;
        for (int step = 0; step < 40; ++step)
        {
            const double scale = std::exp(std::sin(step * 3.14159265358979323846 / 39) * 1.3);
            request.displayMinLongitude = request.renderMinLongitude = 10.003141 - 0.003141 / scale;
            request.displayMaxLongitude = request.renderMaxLongitude = 10.003141 + 0.006859 / scale;
            request.displayMinLatitude = request.renderMinLatitude = 50.002718 - 0.002718 / scale;
            request.displayMaxLatitude = request.renderMaxLatitude = 50.002718 + 0.007282 / scale;
            request.scaleX = 512 * scale / 0.01; request.scaleY = 384 * scale / 0.01;
            auto bitmap = renderer.Render(request, originX, originY);
            Require(bitmap && bitmap->bitmap, "Synthetic marker renderer failed");
            const auto expected = Project(request, 10.003141, 50.002718);
            const BitmapView view(bitmap->bitmap);
            double coverage = 0, weightedX = 0, weightedY = 0;
            for (int y = 0; y < request.rasterHeight; ++y)
                for (int x = 0; x < request.rasterWidth; ++x)
                {
                    const double alpha = view.Pixels()[(static_cast<std::size_t>(y) * request.rasterWidth + x) * 4 + 3];
                    coverage += alpha; weightedX += (x + 0.5) * alpha; weightedY += (y + 0.5) * alpha;
                }
            Require(coverage > 0, "Synthetic marker disappeared");
            maximum = (std::max)(maximum, std::hypot(weightedX / coverage - (expected.X - originX),
                weightedY / coverage - (expected.Y - originY)));
            auto repeat = renderer.Render(request, originX, originY);
            Require(repeat && repeat->bitmap, "Synthetic marker repeat failed");
            handoff.Accumulate(Compare(bitmap->bitmap, repeat->bitmap, false));
            if (step == 13) Save(bitmap->bitmap, output / "geographic_marker.png");
        }
        Row(csv, "geographic_marker", "native_centroid_error", {}, {}, {}, handoff, 0, maximum, 0, renderer.UsesHardware());
        // A full double projection is authoritative even if the legacy float
        // corners describe a different map. Its identity participates in caching.
        auto precise = request;
        const VsmrAviso::GeoBounds bounds{ request.displayMinLongitude, request.displayMinLatitude,
            request.displayMaxLongitude, request.displayMaxLatitude };
        precise.preciseProjection = VsmrAviso::BilinearProjection::FromCorners(bounds, request.projectedTopLeft,
            request.projectedTopRight, request.projectedBottomLeft, request.projectedBottomRight);
        for (PointF* point : { &precise.projectedTopLeft, &precise.projectedTopRight,
            &precise.projectedBottomLeft, &precise.projectedBottomRight }) { point->X += 31.375f; point->Y -= 13.125f; }
        auto reference = renderer.Render(request, originX, originY);
        auto preciseBitmap = renderer.Render(precise, originX, originY);
        Require(reference && preciseBitmap, "Precise projection render failed");
        const auto preciseDiff = Compare(reference->bitmap, preciseBitmap->bitmap, false);
        Row(csv, "geographic_marker", "precise_projection_overrides_float", {}, {}, {}, preciseDiff, 0, 0, 0, renderer.UsesHardware());
        bool preciseCache = false;
        {
            Surface first(800, 600), second(800, 600);
            VsmrAviso::AvisoViewportRenderer viewport(backend, TextMode);
            const RECT area{ 137, 53, 649, 437 };
            auto draw = [&](Surface& surface) {
                surface.Clear(Color(255, 0, 0, 0));
                return viewport.Draw(*surface.graphics, surface.dc, area, precise, {});
            };
            const auto initial = draw(first);
            const auto idle = draw(second);
            preciseCache = initial.drawn && idle.drawn && idle.cacheHit && !idle.buildAttempted &&
                Compare(first.bitmap, second.bitmap).pixels == 0;
            VsmrAviso::PointD tl{ request.projectedTopLeft.X + 0.375, request.projectedTopLeft.Y };
            VsmrAviso::PointD tr{ request.projectedTopRight.X + 0.375, request.projectedTopRight.Y };
            VsmrAviso::PointD bl{ request.projectedBottomLeft.X + 0.375, request.projectedBottomLeft.Y };
            VsmrAviso::PointD br{ request.projectedBottomRight.X + 0.375, request.projectedBottomRight.Y };
            precise.preciseProjection = VsmrAviso::BilinearProjection::FromCorners(bounds, tl, tr, bl, br);
            const auto changed = draw(second);
            preciseCache &= changed.drawn && !changed.cacheHit && changed.buildAttempted &&
                Compare(first.bitmap, second.bitmap).pixels != 0;
            std::cout << "Precise projection cache identity=" << preciseCache << std::endl;
        }
        // Exercise surface resizing, key invalidation and complete GDI resource
        // release independently of font/system caches and external processes.
        DWORD objectsBefore = 0;
        bool lifetimes = true;
        for (int lifetime = -1; lifetime < 8; ++lifetime)
        {
            // GDI+ initializes a process-global object on the first HDC-backed
            // Graphics. Warm that once before measuring renderer-owned lifetimes.
            if (lifetime == 0) objectsBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            Surface surface(640, 480);
            VsmrAviso::AvisoViewportRenderer viewport(backend, TextMode);
            for (int resize = 0; resize < 8; ++resize)
            {
                request.rasterWidth = 512 + resize * 7; request.rasterHeight = 384 + resize * 3;
                const RECT area{ 0, 0, request.rasterWidth, request.rasterHeight };
                const auto outcome = viewport.Draw(*surface.graphics, surface.dc, area, request, [&]() { surface.Clear(Color(255, 0, 0, 0)); });
                lifetimes &= outcome.drawn && !outcome.cacheHit && viewport.BitmapBytes() ==
                    static_cast<std::uint64_t>(request.rasterWidth) * request.rasterHeight * 4;
            }
            const auto handle = viewport.Bitmap();
            viewport.Clear();
            BITMAP bitmapInfo{};
            lifetimes &= viewport.Bitmap() == nullptr && viewport.BitmapBytes() == 0 && GetObject(handle, sizeof(bitmapInfo), &bitmapInfo) == 0;
            viewport.ReleaseResources();
        }
        GdiFlush();
        const DWORD objectsAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        std::cout << "Native resource lifetime: GDI objects " << objectsBefore << " -> " << objectsAfter
            << ", resize/cache/clear=" << lifetimes << std::endl;
        // Pixel coverage is quantized; the sampled centroid must stay within
        // one eighth of a pixel of the analytical geographic projection.
        return maximum <= 0.125 && handoff.pixels == 0 && lifetimes && objectsAfter == objectsBefore &&
            preciseDiff.pixels == 0 && preciseCache;
    }
    bool ProbeVisualStateSwaps(std::ostream& csv, VsmrAviso::AvisoVectorRenderer::Backend backend)
    {
        auto features = std::make_shared<std::vector<AvisoFeature>>();
        AvisoFeature polygon;
        polygon.polygon = true; polygon.polygonOutline = polygon.lightPolygonOutline = polygon.realPolygonOutline = true;
        polygon.paths = { { {0.2,0.2}, {0.8,0.2}, {0.8,0.8}, {0.2,0.8}, {0.2,0.2} } };
        polygon.minLongitude = polygon.minLatitude = 0.2;
        polygon.maxLongitude = polygon.maxLatitude = 0.8;
        polygon.fillColor = Color(190, 180, 30, 40); polygon.lightFillColor = Color(190, 30, 160, 40);
        polygon.realFillColor = Color(190, 40, 30, 180); polygon.groupIds = { "surface" };
        features->push_back(polygon);
        auto labels = std::make_shared<std::vector<AvisoLabel>>();
        AvisoLabel label;
        label.position = {0.5, 0.5}; label.text = L"\u00c9t\u00e9 \u03a9 \u4e2d"; label.groupIds = { "text" };
        labels->push_back(label);
        AvisoRasterRenderRequest request;
        request.path = "visual state swaps"; request.features = features; request.labels = labels;
        request.scene = VsmrAviso::AvisoRenderScene::Build(*features, *labels);
        request.rasterWidth = 384; request.rasterHeight = 256;
        request.displayMaxLongitude = request.displayMaxLatitude = request.renderMaxLongitude = request.renderMaxLatitude = 1;
        request.scaleX = 384; request.scaleY = 256; request.viewportZoomLevel = 20;
        const RECT area{ 37, 19, 421, 275 }, source{ 0, 0, 384, 256 };
        request.projectedTopLeft = PointF(37, 19); request.projectedTopRight = PointF(421, 19);
        request.projectedBottomLeft = PointF(37, 275); request.projectedBottomRight = PointF(421, 275);
        request.preciseProjection = VsmrAviso::BilinearProjection::FromCorners(VsmrAviso::GeoBounds{0,0,1,1},
            request.projectedTopLeft, request.projectedTopRight, request.projectedBottomLeft, request.projectedBottomRight);
        VsmrAviso::AvisoVectorRenderer control(backend, TextMode);
        VsmrAviso::AvisoViewportRenderer viewport(backend, TextMode);
        VsmrAviso::AvisoRasterBlitter blitter;
        Surface first(512, 384), second(512, 384), reference(512, 384);
        Difference differences;
        bool valid = true, hardware = false;
        for (int state = 0; state < 18; ++state)
        {
            request.colorPalette = state % 3 == 0 ? "dark" : state % 3 == 1 ? "light" : "real";
            request.displayScale = state / 3 % 3 == 0 ? 1.0 : state / 3 % 3 == 1 ? 4.0/3.0 : 2.0;
            request.backgroundColor = state % 2 == 0 ? Color(255, 35, 45, 55) : Color(0, 0, 0, 0);
            request.groupVisibility = std::make_shared<const std::unordered_map<std::string, bool>>(
                std::unordered_map<std::string, bool>{ { "surface", true }, { "text", state % 4 != 3 } });
            request.groupGeneration = state + 1;
            unsigned backgrounds = 0;
            auto paint = [&](Surface& surface) {
                ++backgrounds;
                SolidBrush brush(Color(133, 100, 130, 160));
                Require(surface.graphics->FillRectangle(&brush, Rect(37, 19, 384, 256)) == Ok, "State swap background failed");
            };
            first.Clear(Color(255, 20, 40, 60)); second.Clear(Color(255, 20, 40, 60)); reference.Clear(Color(255, 20, 40, 60));
            const auto active = viewport.Draw(*first.graphics, first.dc, area, request, [&]() { paint(first); });
            const auto idle = viewport.Draw(*second.graphics, second.dc, area, request, [&]() { paint(second); });
            hardware = idle.hardware;
            valid &= active.drawn && active.buildAttempted && !active.cacheHit && idle.drawn && idle.cacheHit && !idle.buildAttempted;
            valid &= backgrounds == (state % 2 == 0 ? 0U : 2U);
            const auto settledDifference = Compare(first.bitmap, second.bitmap);
            differences.Accumulate(settledDifference);
            auto expected = control.Render(request, 37, 19);
            Require(expected != nullptr, "State swap control render failed");
            if (state % 2 != 0) paint(reference);
            Require(blitter.Blend(*reference.graphics, reference.dc, expected->bitmap, source, area, area, state % 2 == 0),
                "State swap reference composition failed");
            const auto controlDifference = Compare(first.bitmap, reference.bitmap);
            differences.Accumulate(controlDifference);
            valid &= settledDifference.pixels == 0 && controlDifference.pixels == 0;
        }
        Row(csv, "visual_state_swaps", "palette_DPI_opacity_visibility", {}, {}, {}, differences, 0, 0, 0, hardware);
        viewport.ReleaseResources(); control.ReleaseResources();
        valid &= viewport.Bitmap() == nullptr && viewport.BitmapBytes() == 0 && viewport.EstimatedGpuImageBytes() == 0 &&
            control.EstimatedGpuImageBytes() == 0 && control.CachedGlyphPoints() == 0 && control.CachedNativeMeshBytes() == 0;
        return valid;
    }

    bool ProbeMembershipCopies(std::ostream& csv, VsmrAviso::AvisoVectorRenderer::Backend backend)
    {
        AvisoRasterRenderRequest request;
        request.path = "immutable membership copies";
        auto features = std::make_shared<std::vector<AvisoFeature>>();
        AvisoFeature line;
        line.paths = {{{0.1,0.3},{0.9,0.3}}}; line.groupIds = {"before"};
        line.minLongitude = 0.1; line.maxLongitude = 0.9; line.minLatitude = line.maxLatitude = 0.3;
        line.strokeWidth = 2; line.strokeColor = line.lightStrokeColor = line.realStrokeColor = Color(255,70,90,110);
        features->push_back(line);
        auto labels = std::make_shared<std::vector<AvisoLabel>>();
        AvisoLabel label;
        label.position = {0.5,0.5}; label.text = L"Shared outline"; label.groupIds = {"before"};
        labels->push_back(label);
        request.features = features; request.labels = labels;
        request.scene = VsmrAviso::AvisoRenderScene::Build(*features,*labels);
        request.groupVisibility = std::make_shared<const std::unordered_map<std::string,bool>>(
            std::unordered_map<std::string,bool>{{"before",true},{"after",true}});
        request.rasterWidth = 320; request.rasterHeight = 128;
        request.displayMaxLongitude = request.displayMaxLatitude = request.renderMaxLongitude = request.renderMaxLatitude = 1;
        request.scaleX = 320; request.scaleY = 128; request.viewportZoomLevel = 20;
        request.preciseProjection = VsmrAviso::BilinearProjection::FromCorners({0,0,1,1},
            VsmrAviso::PointD{0,0},VsmrAviso::PointD{320,0},VsmrAviso::PointD{0,128},VsmrAviso::PointD{320,128});
        VsmrAviso::AvisoVectorRenderer renderer(backend,TextMode);
        auto before = renderer.Render(request);
        Require(before != nullptr,"Membership original render failed");
        const auto points = renderer.CachedGlyphPoints();
        auto copiedFeatures = std::make_shared<std::vector<AvisoFeature>>(*features);
        auto copiedLabels = std::make_shared<std::vector<AvisoLabel>>(*labels);
        copiedFeatures->front().groupIds = {"after"};
        copiedLabels->front().groupIds = {"after"};
        request.features = copiedFeatures; request.labels = copiedLabels;
        request.scene = VsmrAviso::AvisoRenderScene::Build(*copiedFeatures,*copiedLabels);
        ++request.groupGeneration;
        auto after = renderer.Render(request);
        Require(after != nullptr,"Membership copied render failed");
        const auto difference = Compare(before->bitmap,after->bitmap,false);
        const bool retained = renderer.LastStats().glyphCacheMisses == 0 &&
            renderer.LastStats().glyphCacheHits > 0 && renderer.CachedGlyphPoints() == points && points > 0;
        Row(csv,"membership_copies","shared_native_glyph_retained",{},{},{},difference,0,0,0,renderer.UsesHardware());
        std::cout << "Membership-copy outline retained=" << retained << std::endl;
        return retained && difference.pixels == 0;
    }

    bool ProbeGlyphPhysicalCentroid(const fs::path& output, VsmrAviso::AvisoVectorRenderer::Backend backend)
    {
        std::ofstream csv(output / "glyph_physical_centroid.csv");
        csv << std::setprecision(9) << "font,text,display_scale,phase_x,phase_y,reference_error_px,native_error_px,reference_alpha_area,native_alpha_area,geometric_area,native_area_error,native_area_quantization_bound,native_used,repeat_differing_pixels\n";
        bool valid = true;
        double greatestReference = 0, greatestNative = 0;
        for (const wchar_t* font : {L"Arial",L"Calibri"})
        for (const wchar_t* text : {L"O",L"Aa123 Runway"})
        for (const double scale : {1.0,4.0/3.0,2.0})
        {
            AvisoRasterRenderRequest request;
            request.path = "physical font area centroid";
            request.features = std::make_shared<const std::vector<AvisoFeature>>();
            auto labels = std::make_shared<std::vector<AvisoLabel>>();
            AvisoLabel label;
            label.position = {0.5,0.5}; label.text = text; label.fontFamily = font; label.textSize = 14;
            label.textColor = label.lightTextColor = label.realTextColor = Color(255,255,255,255);
            label.haloColor = label.lightHaloColor = label.realHaloColor = Color(0,0,0,0);
            labels->push_back(label); request.labels = labels;
            request.scene = VsmrAviso::AvisoRenderScene::Build(*request.features,*request.labels);
            request.groupVisibility = std::make_shared<const std::unordered_map<std::string,bool>>();
            request.rasterWidth = 360; request.rasterHeight = 128; request.displayScale = scale;
            request.displayMaxLongitude = request.displayMaxLatitude = request.renderMaxLongitude = request.renderMaxLatitude = 1;
            request.scaleX = 360; request.scaleY = 128; request.viewportZoomLevel = 20;
            request.backgroundColor = Color(0,0,0,0);
            VsmrAviso::AvisoVectorRenderer reference(backend,VsmrAviso::AvisoVectorRenderer::TextMode::Vector);
            VsmrAviso::AvisoVectorRenderer renderer(backend,TextMode);
            for (int phase = 0; phase < 64; ++phase)
            {
                const double x = phase % 16 / 16.0, y = phase / 16 / 4.0;
                request.preciseProjection = VsmrAviso::BilinearProjection::FromCorners({0,0,1,1},
                    VsmrAviso::PointD{x,y},VsmrAviso::PointD{360+x,y},VsmrAviso::PointD{x,128+y},VsmrAviso::PointD{360+x,128+y});
                auto original = reference.Render(request), native = renderer.Render(request);
                Require(original && native,"Physical glyph render failed");
                VsmrAviso::PointD local{};
                double physicalArea = 0;
                Require(renderer.LabelGeometricCentroid(0,local,physicalArea),"Physical glyph oracle failed");
                const auto anchor = ProjectNative(request,0.5,0.5);
                auto centroid = [&](HBITMAP bitmap,double& area,double& quantizationBound) {
                    const BitmapView view(bitmap);
                    double sum = 0, sx = 0, sy = 0;
                    std::size_t potentiallyRounded = 0;
                    for (int py = 0; py < 128; ++py)
                    for (int px = 0; px < 360; ++px)
                    {
                        const double alpha = view.Pixels()[(static_cast<std::size_t>(py)*360+px)*4+3]/255.0;
                        sum += alpha; sx += (px+0.5)*alpha; sy += (py+0.5)*alpha;
                        // Interior full coverage is exact. Include zero-alpha
                        // neighbours because a tiny covered edge can round to 0.
                        bool edge = alpha > 0 && alpha < 1;
                        if (alpha == 0)
                            for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                if (px+dx >= 0 && px+dx < 360 && py+dy >= 0 && py+dy < 128)
                                    edge |= view.Pixels()[(static_cast<std::size_t>(py+dy)*360+px+dx)*4+3] != 0;
                        potentiallyRounded += edge;
                    }
                    Require(sum > 0,"Physical glyph disappeared");
                    area = sum;
                    quantizationBound = potentiallyRounded*0.5/255.0;
                    return std::hypot(sx/sum-anchor.X-local.X,sy/sum-anchor.Y-local.Y);
                };
                double referenceArea = 0,nativeArea = 0,referenceBound = 0,nativeBound = 0;
                const double referenceError = centroid(original->bitmap,referenceArea,referenceBound), nativeError = centroid(native->bitmap,nativeArea,nativeBound);
                greatestReference = (std::max)(greatestReference,referenceError);
                greatestNative = (std::max)(greatestNative,nativeError);
                auto repeat = renderer.Render(request);
                Require(repeat != nullptr,"Physical glyph repeat failed");
                const auto difference = Compare(native->bitmap,repeat->bitmap,false);
                const bool used = renderer.UsesNativeLabels();
                std::string fontName = std::wstring(font) == L"Arial" ? "Arial" : "Calibri";
                std::string textName = std::wstring(text) == L"O" ? "O" : "Aa123 Runway";
                csv << fontName << ',' << textName << ',' << scale << ',' << x << ',' << y << ',' << referenceError << ',' << nativeError << ','
                    << referenceArea << ',' << nativeArea << ',' << physicalArea << ',' << std::abs(nativeArea-physicalArea) << ','
                    << nativeBound << ',' << used << ',' << difference.pixels << '\n';
                valid &= difference.pixels == 0;
                if (TextMode != VsmrAviso::AvisoVectorRenderer::TextMode::Vector) valid &= used && nativeError <= 0.125;
                if (TextMode == VsmrAviso::AvisoVectorRenderer::TextMode::NativeAnalytic)
                    valid &= std::abs(nativeArea-physicalArea) <= nativeBound+1e-4;
            }
        }
        std::cout << "Physical font centroid: D2D reference max=" << greatestReference << " px, selected native max="
            << greatestNative << " px, stable/physical gate=" << valid << std::endl;
        return valid;
    }

    bool ProbeGlyphRendering(const fs::path& output, VsmrAviso::AvisoVectorRenderer::Backend backend)
    {
        std::ofstream csv(output / "glyph_fidelity.csv");
        csv << std::setprecision(9) << "palette,style,phase_x,phase_y,changed_pixels,max_rgb_difference,mean_ink_rgb_difference,native_edge_energy_ratio,centroid_delta_px\n";
        bool valid = true;
        for (int style = 0; style < 3; ++style)
        for (const char* palette : { "dark", "light", "real" })
        {
            AvisoRasterRenderRequest request;
            request.path = "glyph native fidelity"; request.colorPalette = palette;
            request.features = std::make_shared<const std::vector<AvisoFeature>>();
            auto labels = std::make_shared<std::vector<AvisoLabel>>();
            AvisoLabel label;
            label.position = { 0.5, 0.5 }; label.text = L"Aa123 Runway"; label.textSize = 14; label.haloWidth = 1;
            label.textColor = Color(221, 240, 245, 250); label.haloColor = Color(170, 0, 0, 0);
            label.lightTextColor = Color(221, 10, 15, 25); label.lightHaloColor = Color(170, 255, 255, 255);
            label.realTextColor = Color(221, 245, 185, 80); label.realHaloColor = Color(170, 0, 0, 0);
            if (style != 0)
                label.haloColor = label.lightHaloColor = label.realHaloColor = Color(0, 0, 0, 0);
            if (style == 2)
            {
                label.textColor = Color(255, 240, 245, 250);
                label.lightTextColor = Color(255, 10, 15, 25);
                label.realTextColor = Color(255, 245, 185, 80);
            }
            labels->push_back(label); request.labels = labels;
            request.groupVisibility = std::make_shared<const std::unordered_map<std::string, bool>>();
            request.scene = VsmrAviso::AvisoRenderScene::Build(*request.features, *request.labels);
            request.rasterWidth = 180; request.rasterHeight = 64;
            request.displayMaxLongitude = request.renderMaxLongitude = 1;
            request.displayMaxLatitude = request.renderMaxLatitude = 1;
            request.scaleX = 180; request.scaleY = 64; request.viewportZoomLevel = 20;
            request.backgroundColor = std::string(palette) == "light" ? Color(255, 215, 220, 225) : Color(255, 35, 40, 45);
            VsmrAviso::AvisoVectorRenderer reference(backend, VsmrAviso::AvisoVectorRenderer::TextMode::Vector);
            VsmrAviso::AvisoVectorRenderer optimized(backend, TextMode);
            Surface referenceGrid(1440, 512), optimizedGrid(1440, 512);
            referenceGrid.Clear(request.backgroundColor); optimizedGrid.Clear(request.backgroundColor);
            GdiFlush();
            BitmapView leftGrid(referenceGrid.bitmap), rightGrid(optimizedGrid.bitmap);
            double lowestEnergy = 1, greatestMean = 0, greatestCentroid = 0;
            std::uint64_t differing = 0;
            for (int i = 0; i < 64; ++i)
            {
                const double x = (i % 16) / 16.0, y = (i / 16) / 4.0;
                const VsmrAviso::PointD tl{ x, y }, tr{ 180 + x, y }, bl{ x, 64 + y }, br{ 180 + x, 64 + y };
                request.preciseProjection = VsmrAviso::BilinearProjection::FromCorners({ 0, 0, 1, 1 }, tl, tr, bl, br);
                auto leftImage = reference.Render(request), rightImage = optimized.Render(request);
                if (!leftImage || !rightImage)
                {
                    std::cerr << "Native glyph renderer failed, HRESULT=" << std::hex << optimized.LastError() << std::dec << std::endl;
                    return false;
                }
                const auto difference = Compare(leftImage->bitmap, rightImage->bitmap);
                const BitmapView left(leftImage->bitmap), right(rightImage->bitmap);
                const BYTE background[] = { request.backgroundColor.GetB(), request.backgroundColor.GetG(), request.backgroundColor.GetR() };
                std::uint64_t ink = 0, total = 0;
                double leftEnergy = 0, rightEnergy = 0;
                double leftWeight = 0, leftX = 0, leftY = 0, rightWeight = 0, rightX = 0, rightY = 0;
                for (int py = 0; py < 64; ++py)
                    for (int px = 0; px < 180; ++px)
                    {
                        const auto offset = (static_cast<std::size_t>(py) * 180 + px) * 4;
                        bool hasInk = false;
                        for (int c = 0; c < 3; ++c)
                        {
                            hasInk |= left.Pixels()[offset + c] != background[c] || right.Pixels()[offset + c] != background[c];
                            total += std::abs(static_cast<int>(left.Pixels()[offset + c]) - right.Pixels()[offset + c]);
                            const double oldWeight = std::abs(static_cast<int>(left.Pixels()[offset + c]) - background[c]);
                            const double newWeight = std::abs(static_cast<int>(right.Pixels()[offset + c]) - background[c]);
                            leftWeight += oldWeight; leftX += (px + 0.5) * oldWeight; leftY += (py + 0.5) * oldWeight;
                            rightWeight += newWeight; rightX += (px + 0.5) * newWeight; rightY += (py + 0.5) * newWeight;
                            for (const int delta : { 4, 180 * 4 })
                            {
                                if ((delta == 4 && px == 179) || (delta != 4 && py == 63)) continue;
                                leftEnergy += std::abs(static_cast<int>(left.Pixels()[offset + c]) - left.Pixels()[offset + c + delta]);
                                rightEnergy += std::abs(static_cast<int>(right.Pixels()[offset + c]) - right.Pixels()[offset + c + delta]);
                            }
                        }
                        ink += hasInk;
                        const auto grid = (static_cast<std::size_t>(i / 8 * 64 + py) * 1440 + i % 8 * 180 + px) * 4;
                        std::memcpy(const_cast<BYTE*>(leftGrid.Pixels()) + grid, left.Pixels() + offset, 4);
                        std::memcpy(const_cast<BYTE*>(rightGrid.Pixels()) + grid, right.Pixels() + offset, 4);
                    }
                const double ratio = leftEnergy ? rightEnergy / leftEnergy : 0;
                const double mean = ink ? static_cast<double>(total) / (ink * 3) : 0;
                const double centroid = leftWeight && rightWeight ? std::hypot(leftX / leftWeight - rightX / rightWeight,
                    leftY / leftWeight - rightY / rightWeight) : 1000;
                lowestEnergy = (std::min)(lowestEnergy, ratio); greatestMean = (std::max)(greatestMean, mean);
                greatestCentroid = (std::max)(greatestCentroid, centroid);
                differing += difference.pixels;
                csv << palette << ',' << style << ',' << x << ',' << y << ',' << difference.pixels << ',' << difference.maximum << ',' << mean << ',' << ratio << ',' << centroid << '\n';
                // D2D realization coverage expands thin glyphs: the independent
                // source-outline alpha oracle checks true shape and anchoring.
                // Retain native edge contrast; RGB/contrast centroids are useful
                // style diagnostics, not coordinate or alpha-mass ground truth.
                valid &= ink > 0 && ratio >= 0.90;
                auto repeat = optimized.Render(request);
                valid &= repeat && Compare(rightImage->bitmap, repeat->bitmap, false).pixels == 0;
            }
            const std::string name = std::string("glyph_") + palette + "_style" + std::to_string(style);
            Save(referenceGrid.bitmap, output / (name + "_reference.png"));
            Save(optimizedGrid.bitmap, output / (name + "_native.png"));
            std::cout << "Native glyphs " << palette << " style=" << style << ": pixels=" << differing << ", worst energy=" << lowestEnergy
                << ", greatest ink RGB mean=" << greatestMean << ", centroid delta=" << greatestCentroid
                << ", native mesh bytes=" << optimized.CachedNativeMeshBytes()
                << ", native used=" << optimized.UsesNativeLabels()
                << std::endl;
        }
        return valid;
    }
}

int main(int argc, char** argv)
{
    ULONG_PTR token = 0;
    GdiplusStartupInput startup;
    if (GdiplusStartup(&token, &startup, nullptr) != Ok) return 1;
    int exitCode = 0;
    try
    {
        const fs::path data = argc > 1 ? fs::u8path(argv[1]) : fs::path("vSMR/data/AVISO");
        const fs::path output = argc > 2 ? fs::u8path(argv[2]) : fs::path("artifacts/aviso-zoom-benchmark");
        const int frames = argc > 3 ? (std::max)(8, std::stoi(argv[3])) : 32;
        bool quick = false;
        bool qualityOnly = false;
        std::vector<double> zoomLevels{ 1.0, 5.0 };
        std::vector<std::string> airports{ "LFPG", "LFPO", "LFML" };
        std::vector<std::string> palettes{ "dark", "light", "real" };
        auto backend = VsmrAviso::AvisoVectorRenderer::Backend::Hardware;
        for (int i = 4; i < argc; ++i)
        {
            const std::string argument(argv[i]);
            if (argument == "--quick") quick = true;
            if (argument == "--software") backend = VsmrAviso::AvisoVectorRenderer::Backend::Software;
            if (argument == "--vector-reference") TextMode = VsmrAviso::AvisoVectorRenderer::TextMode::Vector;
            if (argument == "--native-analytic") TextMode = VsmrAviso::AvisoVectorRenderer::TextMode::NativeAnalytic;
            if (argument == "--quality-only") qualityOnly = true;
            auto parseList = [&](const char* prefix, std::vector<std::string>& result) {
                const std::string option(prefix);
                if (argument.rfind(option, 0) != 0) return;
                result.clear();
                std::istringstream input(argument.substr(option.size()));
                std::string value;
                while (std::getline(input, value, ',')) result.push_back(value);
                Require(!result.empty(), "Empty benchmark selection");
            };
            parseList("--airports=", airports);
            parseList("--palettes=", palettes);
            if (argument.rfind("--zoom=", 0) == 0)
            {
                const double zoom = std::stod(argument.substr(7));
                Require(std::isfinite(zoom) && zoom > 0.0, "Invalid benchmark zoom");
                zoomLevels = { zoom };
            }
        }
        for (const auto& airport : airports)
            Require(airport.size() == 4 && std::all_of(airport.begin(), airport.end(), [](char ch) { return ch >= 'A' && ch <= 'Z'; }), "Invalid airport code");
        for (const auto& palette : palettes)
            Require(palette == "dark" || palette == "light" || palette == "real", "Invalid palette");
        fs::create_directories(output);
        std::ofstream csv(output / "zoom_measurements.csv");
        std::ofstream stages(output / "zoom_stages.csv");
        stages << std::setprecision(9) << "scene,frame,cache_hit,prepare_ms,geometry_ms,label_ms,flush_ms,readback_ms,build_ms,total_frame_ms,glyph_hits,glyph_misses,feature_candidates,label_candidates,paths_drawn,projected_vertices,cached_glyph_points,estimated_gpu_image_bytes,cached_native_glyph_bytes,native_labels_used,visible_labels,glyph_ms,mesh_setup_ms,mesh_build_ms,mesh_pack_ms,mesh_draw_ms,mesh_fill_ms,mesh_halo_ms,mesh_bin_ms\n";
        csv << std::setprecision(9) << "scene,operation,frames,old_mean_ms,new_mean_ms,old_median_ms,new_median_ms,old_p95_ms,new_p95_ms,old_max_ms,new_max_ms,old_handoff_differing_pixels,old_handoff_max_channel,old_handoff_mean_channel,new_handoff_differing_pixels,new_handoff_max_channel,new_handoff_mean_channel,old_max_affine_alignment_error_px,cursor_anchor_error_px,new_cold_ms,hardware,old_cold_ms\n";
        if (!ProbeMarker(csv, output, backend)) exitCode = 2;
        if (!ProbeVisualStateSwaps(csv, backend)) exitCode = 2;
        if (!ProbeMembershipCopies(csv, backend)) exitCode = 2;
        if (!ProbeGlyphPhysicalCentroid(output, backend)) exitCode = 2;
        if (TextMode != VsmrAviso::AvisoVectorRenderer::TextMode::Vector && !ProbeGlyphRendering(output, backend)) exitCode = 2;
        for (const auto& airport : qualityOnly ? std::vector<std::string>{} : airports)
        {
            const fs::path path = data / (airport + ".geojson");
            const Fixture fixture = LoadFixture(path);
            for (double zoom : zoomLevels)
                for (const auto& palette : palettes)
                {
                    if (quick && (zoom > 1 || palette != "dark")) continue;
                    auto request = MakeRequest(fixture, path, zoom, zoom > 1 ? 23.0 : 0.0, 1920, 1080);
                    request.colorPalette = palette;
                    const std::string name = airport + (zoom > 1 ? "_detail_rotated_" : "_whole_") + palette;
                    if (!RunScene(fixture, output, name, std::move(request), frames, csv, stages, backend)) exitCode = 2;
                }
            if (airport == "LFPG")
            {
                auto request = MakeRequest(fixture, path, 1, 0, 1920, 1080);
                if (!ProbeTransparent(request, csv, output, backend)) exitCode = 2;
                request = MakeRequest(fixture, path, 5, 23, 1920, 1080);
                request.projectedBottomRight.X += 1;
                request.preciseProjection.reset();
                if (!RunScene(fixture, output, "LFPG_detail_rotated_bilinear", request, frames, csv, stages, backend)) exitCode = 2;
                request = MakeRequest(fixture, path, 1, 0, 1920, 1080);
                request.projectedBottomRight.X += 1;
                request.preciseProjection.reset();
                if (!RunScene(fixture, output, "LFPG_whole_bilinear", request, frames, csv, stages, backend)) exitCode = 2;
                if (!quick)
                    for (const auto size : { std::pair<int, int>(800, 450), std::pair<int, int>(3840, 2160) })
                    {
                        request = MakeRequest(fixture, path, 1, 0, size.first, size.second);
                        if (!RunScene(fixture, output, "LFPG_" + std::to_string(size.first) + "x" + std::to_string(size.second), request, frames, csv, stages, backend)) exitCode = 2;
                    }
                if (!quick)
                    for (const int resolution : { 2560, 3840 })
                    {
                        request = MakeRequest(fixture, path, 1, 0, resolution, resolution * 9 / 16);
                        request.displayScale = resolution == 2560 ? 4.0 / 3.0 : 2.0;
                        const std::string name = resolution == 2560 ? "LFPG_2560x1440_scale_4_3" : "LFPG_3840x2160_scale_2";
                        if (!RunScene(fixture, output, name, request, frames, csv, stages, backend)) exitCode = 2;
                    }
            }
        }
        std::cout << "Zoom CSV: " << (output / "zoom_measurements.csv").u8string() << std::endl;
    }
    catch (const std::exception& error) { std::cerr << error.what() << std::endl; exitCode = 1; }
    GdiplusShutdown(token);
    return exitCode;
}
