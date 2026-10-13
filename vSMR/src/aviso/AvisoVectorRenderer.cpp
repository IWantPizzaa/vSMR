#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoVectorRenderer.hpp"
#include "aviso/AvisoRenderScene.hpp"
#include "aviso/AvisoProjection.hpp"
#include "aviso/AvisoGlyphAnalyticRenderer.hpp"
#include "radar/RadarScreen.AvisoSupport.hpp"

#include <d2d1_2.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <map>
#include <set>
#include <tuple>

using Microsoft::WRL::ComPtr;
using namespace VsmrRadarTypes;

namespace
{
	void Check(HRESULT status)
	{
		if (FAILED(status)) throw status;
	}

	D2D1_COLOR_F Color(Gdiplus::Color color)
	{
		return D2D1::ColorF(color.GetR() / 255.0f, color.GetG() / 255.0f,
			color.GetB() / 255.0f, color.GetAlpha() / 255.0f);
	}

	bool Visible(const std::vector<std::string>& groups,
		const std::unordered_map<std::string, bool>* visibility)
	{
		if (groups.empty() || visibility == nullptr) return true;
		for (const auto& group : groups)
		{
			const auto found = visibility->find(group);
			if (found == visibility->end() || found->second) return true;
		}
		return false;
	}

	struct Dib
	{
		HBITMAP bitmap = nullptr;
		void* bits = nullptr;
		~Dib() { if (bitmap != nullptr) DeleteObject(bitmap); }
		void Create(int width, int height)
		{
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (bitmap == nullptr || bits == nullptr) throw E_OUTOFMEMORY;
		}
	};

}

struct VsmrAviso::AvisoVectorRenderer::Impl
{
	struct FeatureGeometry
	{
		double longitude = 0.0;
		double latitude = 0.0;
		bool safeWorldBounds = true;
		std::vector<ComPtr<ID2D1PathGeometry>> paths;
		std::vector<ComPtr<ID2D1GeometryRealization>> fills;
	};
	struct LabelGeometry
	{
		double scale = 0.0;
		std::size_t points = 0;
		ComPtr<ID2D1PathGeometry> path;
		D2D1_RECT_F bounds{};
		ComPtr<ID2D1GeometryRealization> fill;
		ComPtr<ID2D1GeometryRealization> halo;
	};
	Backend preferred;
	TextMode textMode;
	bool hardware = false;
	std::string backendDescription;
	ComPtr<ID2D1Factory1> factory;
	AvisoGlyphAnalyticRenderer analyticGlyphs;
	std::vector<AvisoGlyphAnalyticRenderer::Instance> meshInstances;
	std::vector<std::shared_ptr<LabelGeometry>> meshGlyphs;
	ComPtr<ID3D11Texture2D> meshTexture;
	ComPtr<ID2D1Bitmap1> meshBitmap;
	bool meshFallback = false;
	bool nativeLabelsRendered = false;
	std::size_t visibleLabelCount = 0;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> immediate;
	ComPtr<ID2D1Device> d2dDevice;
	ComPtr<ID2D1DeviceContext> target;
	ComPtr<ID2D1DeviceContext1> realizationTarget;
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11Texture2D> staging;
	ComPtr<ID2D1Bitmap1> targetBitmap;
	ComPtr<ID2D1SolidColorBrush> brush;
	ComPtr<ID2D1StrokeStyle1> lineStyle;
	ComPtr<ID2D1StrokeStyle1> polygonStyle;
	int width = 0;
	int height = 0;
	std::shared_ptr<const std::vector<AvisoFeature>> features;
	std::shared_ptr<const std::vector<AvisoLabel>> labels;
	std::shared_ptr<const AvisoRenderScene> scene;
	std::vector<FeatureGeometry> geometry;
	using GlyphKey = std::tuple<std::wstring,std::wstring,std::string,float,float,double>;
	using GlyphStyleKey = std::tuple<std::wstring,std::wstring,std::string,float,float>;
	std::vector<std::shared_ptr<LabelGeometry>> glyphs;
	std::map<GlyphKey,std::shared_ptr<LabelGeometry>> sharedGlyphs;
	std::map<std::wstring,std::unique_ptr<Gdiplus::FontFamily>> fonts;
	std::vector<std::uint32_t> featureCandidates;
	std::vector<std::uint32_t> labelCandidates;
	AvisoRasterRenderStats stats;
	HRESULT error = S_OK;
	Timings timings;
	// Longitude/latitude offsets are stored around each feature's center,
	// avoiding float precision loss from absolute geographic coordinates.
	static constexpr double WorldUnitsPerDegree = 100000.0;
	static constexpr std::size_t GlyphPointBudget = 524288U;
	// Glyphs remain at native screen size while the map zooms. Their curves are
	// flattened to at most 1/16 pixel once, with no scale-dependent handoff.
	static constexpr float GlyphFlatteningTolerance = 0.0625f;
	std::size_t glyphPoints = 0;

	explicit Impl(Backend backend, TextMode text)
		: preferred(backend), textMode(text) {}

	void EnsureFactory()
	{
		if (factory) return;
		Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
		D2D1_STROKE_STYLE_PROPERTIES1 properties{};
		properties.startCap = D2D1_CAP_STYLE_FLAT;
		properties.endCap = D2D1_CAP_STYLE_FLAT;
		properties.dashCap = D2D1_CAP_STYLE_FLAT;
		properties.lineJoin = D2D1_LINE_JOIN_ROUND;
		properties.miterLimit = 10.0f;
		properties.dashStyle = D2D1_DASH_STYLE_SOLID;
		properties.transformType = D2D1_STROKE_TRANSFORM_TYPE_FIXED;
		Check(factory->CreateStrokeStyle(properties, nullptr, 0, polygonStyle.GetAddressOf()));
		properties.startCap = properties.endCap = D2D1_CAP_STYLE_ROUND;
		Check(factory->CreateStrokeStyle(properties, nullptr, 0, lineStyle.GetAddressOf()));
	}

	void DiscardDevice()
	{
		if (target) target->SetTarget(nullptr);
		brush.Reset(); targetBitmap.Reset(); staging.Reset(); texture.Reset();
		realizationTarget.Reset(); target.Reset(); d2dDevice.Reset(); immediate.Reset(); device.Reset();
		for (auto& feature : geometry) for (auto& fill : feature.fills) fill.Reset();
		for (auto& entry : sharedGlyphs) { entry.second->fill.Reset(); entry.second->halo.Reset(); }
		width = height = 0;
		meshBitmap.Reset(); meshTexture.Reset(); analyticGlyphs.ReleaseResources();
		meshInstances.clear(); meshGlyphs.clear(); nativeLabelsRendered = false; visibleLabelCount = 0;
		// Device recreation retries native coverage. Capability/budget fallback
		// stays stable while the current device and immutable scene remain valid.
		meshFallback = false;
	}

	void EnsureDevice()
	{
		if (device) return;
		EnsureFactory();
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
		HRESULT status = E_FAIL;
		if (preferred == Backend::Hardware)
		{
			// Prefer the actual high-performance GPU on hybrid systems. Older
			// Windows versions can use the normal default-adapter path below.
			ComPtr<IDXGIFactory1> dxgiFactory;
			ComPtr<IDXGIFactory6> factory6;
			if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))) && SUCCEEDED(dxgiFactory.As(&factory6)))
			{
				for (UINT index = 0; ; ++index)
				{
					ComPtr<IDXGIAdapter1> adapter;
					if (FAILED(factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) break;
					DXGI_ADAPTER_DESC1 description{};
					if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
					status = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
						D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
						device.ReleaseAndGetAddressOf(), nullptr, immediate.ReleaseAndGetAddressOf());
					if (SUCCEEDED(status)) break;
				}
			}
			if (FAILED(status))
				status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
					D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
					device.ReleaseAndGetAddressOf(), nullptr, immediate.ReleaseAndGetAddressOf());
		}
		hardware = SUCCEEDED(status);
		if (!hardware)
		{
			device.Reset(); immediate.Reset();
			Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
				device.GetAddressOf(), nullptr, immediate.GetAddressOf()));
		}
		ComPtr<IDXGIDevice> dxgiDevice;
		Check(device.As(&dxgiDevice));
		ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC description{};
		backendDescription = hardware ? "Direct2D hardware" : "Direct2D WARP software";
		if (hardware && SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) && SUCCEEDED(adapter->GetDesc(&description)))
		{
			char name[512]{};
			if (WideCharToMultiByte(CP_UTF8,0,description.Description,-1,name,ARRAYSIZE(name),nullptr,nullptr) > 0)
				backendDescription += std::string(" / ") + name;
		}
		Check(factory->CreateDevice(dxgiDevice.Get(), d2dDevice.GetAddressOf()));
		Check(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS, target.GetAddressOf()));
		target.As(&realizationTarget); // Optional on the older Direct2D runtime.
		target->SetDpi(96.0f, 96.0f);
		target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
		target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		Check(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), brush.GetAddressOf()));
	}

	void EnsureTarget(int requestedWidth, int requestedHeight)
	{
		EnsureDevice();
		if (width == requestedWidth && height == requestedHeight) return;
		target->SetTarget(nullptr);
		targetBitmap.Reset(); texture.Reset(); staging.Reset();
		width = height = 0;
		D3D11_TEXTURE2D_DESC description{};
		description.Width = requestedWidth; description.Height = requestedHeight;
		description.MipLevels = 1; description.ArraySize = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		Check(device->CreateTexture2D(&description, nullptr, texture.GetAddressOf()));
		ComPtr<IDXGISurface> surface;
		Check(texture.As(&surface));
		const auto properties = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
		Check(target->CreateBitmapFromDxgiSurface(surface.Get(), &properties, targetBitmap.GetAddressOf()));
		target->SetTarget(targetBitmap.Get());
		description.Usage = D3D11_USAGE_STAGING;
		description.BindFlags = 0;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		Check(device->CreateTexture2D(&description, nullptr, staging.GetAddressOf()));
		width = requestedWidth; height = requestedHeight;
	}

	template <typename Project>
	ComPtr<ID2D1PathGeometry> Path(const std::vector<AvisoPoint>& points, bool closed, Project&& project)
	{
		ComPtr<ID2D1PathGeometry> path;
		Check(factory->CreatePathGeometry(path.GetAddressOf()));
		ComPtr<ID2D1GeometrySink> sink;
		Check(path->Open(sink.GetAddressOf()));
		sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
		if (points.size() >= (closed ? 3U : 2U))
		{
			sink->BeginFigure(project(points.front()), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
			for (std::size_t i = 1; i < points.size(); ++i) sink->AddLine(project(points[i]));
			sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
		}
		Check(sink->Close());
		return path;
	}

	void Prepare(const AvisoRasterRenderRequest& request)
	{
		if (features == request.features && labels == request.labels) return;
		auto replacementScene = request.scene ? request.scene : AvisoRenderScene::Build(*request.features, *request.labels);
		std::vector<FeatureGeometry> replacement(request.features->size());
		for (std::size_t i = 0; i < replacement.size(); ++i)
		{
			const auto& feature = (*request.features)[i];
			auto& compiled = replacement[i];
			compiled.longitude = (feature.minLongitude + feature.maxLongitude) * 0.5;
			compiled.latitude = (feature.minLatitude + feature.maxLatitude) * 0.5;
			// Direct2D realizations have finite pre-transform coordinate limits.
			// A custom feature spanning distant airports must project its original
			// doubles into the current screen instead of clipping its retained mesh.
			compiled.safeWorldBounds = (feature.maxLongitude-feature.minLongitude)*WorldUnitsPerDegree <= 1048000.0 &&
				(feature.maxLatitude-feature.minLatitude)*WorldUnitsPerDegree <= 1048000.0;
			// A detailed view may use only a fraction of an airport's paths.
			// Compile each retained path after it survives the viewport query
			// and screen-space culling, then reuse it across later zoom frames.
			compiled.paths.resize(feature.paths.size());
			compiled.fills.resize(feature.paths.size());
		}
		std::vector<std::shared_ptr<LabelGeometry>> replacementGlyphs(request.labels->size());
		features = request.features; labels = request.labels;
		scene = std::move(replacementScene); geometry = std::move(replacement);
		glyphs = std::move(replacementGlyphs);
		// A membership edit publishes new immutable snapshots without changing
		// text outlines. Preserve every scale of each surviving style, including
		// its device realizations and native glyph tessellation.
		std::set<GlyphStyleKey> activeStyles;
		std::set<std::wstring> activeFamilies{L"Arial"};
		for (const auto& label : *labels)
		{
			activeStyles.emplace(label.text,label.fontFamily,label.textAnchor,label.textSize,label.haloWidth);
			activeFamilies.insert(label.fontFamily.empty() ? L"Arial" : label.fontFamily);
		}
		for (auto entry = sharedGlyphs.begin(); entry != sharedGlyphs.end(); )
		{
			const auto& key = entry->first;
			const GlyphStyleKey style{std::get<0>(key),std::get<1>(key),std::get<2>(key),std::get<3>(key),std::get<4>(key)};
			if (activeStyles.find(style) == activeStyles.end())
			{
				glyphPoints -= entry->second->points;
				entry = sharedGlyphs.erase(entry);
			}
			else ++entry;
		}
		for (auto font = fonts.begin(); font != fonts.end(); )
		{
			if (activeFamilies.find(font->first) == activeFamilies.end()) font = fonts.erase(font);
			else ++font;
		}
		std::vector<ID2D1PathGeometry*> survivingOutlines;
		survivingOutlines.reserve(sharedGlyphs.size());
		for (const auto& entry : sharedGlyphs) survivingOutlines.push_back(entry.second->path.Get());
		analyticGlyphs.RetainOutlines(survivingOutlines);
		meshFallback = false;
	}

	const Gdiplus::FontFamily* Font(const std::wstring& requested)
	{
		const std::wstring family = requested.empty() ? L"Arial" : requested;
		if (fonts.empty() && family != L"Arial") Font(L"Arial");
		auto found = fonts.find(family);
		if (found == fonts.end())
		{
			if (fonts.size() >= 256U) return Font(L"Arial");
			auto replacement = std::make_unique<Gdiplus::FontFamily>(family.c_str());
			found = fonts.emplace(family,std::move(replacement)).first;
		}
		if (found->second->GetLastStatus() != Gdiplus::Ok && family != L"Arial") return Font(L"Arial");
		return found->second.get();
	}

	ComPtr<ID2D1PathGeometry> Glyph(const AvisoLabel& label, double displayScale, std::size_t& pointCount)
	{
		// Keep the established font/layout semantics but compile the outline at
		// origin zero. Every frame translates the same outline in subpixel space.
		const auto* family = Font(label.fontFamily);
		Gdiplus::StringFormat format;
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
		const float em = static_cast<float>(std::lround(std::clamp(static_cast<double>(label.textSize), 6.0, 40.0) * displayScale * 10.0) / 10.0);
		const float size = static_cast<float>(label.textSize * displayScale);
		const float padding = static_cast<float>((std::max)(static_cast<double>(label.haloWidth) * displayScale, 0.0) * 3.0);
		const float layoutWidth = (std::max)(size * (std::max)(static_cast<float>(label.text.size()), 1.0f) * 0.9f + padding * 2.0f, 14.0f);
		const float layoutHeight = (std::max)(size * 1.65f + padding * 2.0f, 10.0f);
		const std::string anchor = VsmrAvisoSupport::ToUpperAscii(label.textAnchor);
		float left = -layoutWidth * 0.5f, top = -layoutHeight * 0.5f;
		if (anchor.find("LEFT") != std::string::npos) left = 0.0f;
		else if (anchor.find("RIGHT") != std::string::npos) left = -layoutWidth;
		if (anchor.find("TOP") != std::string::npos) top = 0.0f;
		else if (anchor.find("BOTTOM") != std::string::npos) top = -layoutHeight;
		Gdiplus::GraphicsPath outline;
		if (outline.AddString(label.text.c_str(), static_cast<INT>(label.text.size()), family,
			Gdiplus::FontStyleRegular, em, Gdiplus::RectF(left, top, layoutWidth, layoutHeight), &format) != Gdiplus::Ok || outline.GetPointCount() <= 0)
			return {};
		Gdiplus::PathData data;
		if (outline.GetPathData(&data) != Gdiplus::Ok) return {};
		pointCount = static_cast<std::size_t>(data.Count);
		ComPtr<ID2D1PathGeometry> path;
		Check(factory->CreatePathGeometry(path.GetAddressOf()));
		ComPtr<ID2D1GeometrySink> sink;
		Check(path->Open(sink.GetAddressOf()));
		sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
		bool figure = false;
		auto point = [&](int index) { return D2D1::Point2F(data.Points[index].X, data.Points[index].Y); };
		for (int i = 0; i < data.Count; ++i)
		{
			const BYTE type = data.Types[i] & Gdiplus::PathPointTypePathTypeMask;
			if (type == Gdiplus::PathPointTypeStart)
			{
				if (figure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
				sink->BeginFigure(point(i), D2D1_FIGURE_BEGIN_FILLED); figure = true;
			}
			else if (type == Gdiplus::PathPointTypeBezier && i + 2 < data.Count)
			{
				sink->AddBezier(D2D1::BezierSegment(point(i), point(i + 1), point(i + 2))); i += 2;
			}
			else sink->AddLine(point(i));
			if ((data.Types[i] & Gdiplus::PathPointTypeCloseSubpath) != 0 && figure)
			{
				sink->EndFigure(D2D1_FIGURE_END_CLOSED); figure = false;
			}
		}
		if (figure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
		Check(sink->Close());
		return path;
	}

};

VsmrAviso::AvisoVectorRenderer::AvisoVectorRenderer(Backend preferred, TextMode textMode)
	: impl_(std::make_unique<Impl>(preferred,textMode)) {}
VsmrAviso::AvisoVectorRenderer::~AvisoVectorRenderer() = default;
const VsmrAviso::AvisoRasterRenderStats& VsmrAviso::AvisoVectorRenderer::LastStats() const noexcept { return impl_->stats; }
bool VsmrAviso::AvisoVectorRenderer::UsesHardware() const noexcept { return impl_->hardware; }
const std::string& VsmrAviso::AvisoVectorRenderer::BackendDescription() const noexcept { return impl_->backendDescription; }
HRESULT VsmrAviso::AvisoVectorRenderer::LastError() const noexcept { return impl_->error; }
const VsmrAviso::AvisoVectorRenderer::Timings& VsmrAviso::AvisoVectorRenderer::LastTimings() const noexcept { return impl_->timings; }
std::size_t VsmrAviso::AvisoVectorRenderer::CachedGlyphPoints() const noexcept { return impl_->glyphPoints; }
std::uint64_t VsmrAviso::AvisoVectorRenderer::EstimatedGpuImageBytes() const noexcept
{
	return static_cast<std::uint64_t>(impl_->width)*impl_->height*4ULL*(impl_->staging ? 2ULL : 1ULL) +
		impl_->analyticGlyphs.GpuImageBytes() + impl_->analyticGlyphs.GpuBufferBytes();
}
std::uint64_t VsmrAviso::AvisoVectorRenderer::CachedNativeMeshBytes() const noexcept
{
	return impl_->analyticGlyphs.CachedMeshBytes();
}
bool VsmrAviso::AvisoVectorRenderer::UsesNativeLabels() const noexcept { return impl_->nativeLabelsRendered; }
std::size_t VsmrAviso::AvisoVectorRenderer::LastVisibleLabelCount() const noexcept { return impl_->visibleLabelCount; }
bool VsmrAviso::AvisoVectorRenderer::LabelGeometricCentroid(std::size_t labelIndex, PointD& centroid, double& area) noexcept
{
	if (labelIndex >= impl_->glyphs.size() || !impl_->glyphs[labelIndex] || !impl_->factory) return false;
	D2D1_POINT_2F local{};
	if (!impl_->analyticGlyphs.GlyphAreaCentroid(impl_->factory.Get(),impl_->glyphs[labelIndex]->path.Get(),local,area)) return false;
	centroid = {local.x,local.y};
	return true;
}
void VsmrAviso::AvisoVectorRenderer::ReleaseResources() noexcept
{
	impl_->DiscardDevice();
	impl_->features.reset(); impl_->labels.reset(); impl_->scene.reset();
	impl_->geometry.clear(); impl_->glyphs.clear(); impl_->sharedGlyphs.clear(); impl_->fonts.clear(); impl_->glyphPoints = 0;
	impl_->featureCandidates.clear(); impl_->labelCandidates.clear();
	impl_->lineStyle.Reset(); impl_->polygonStyle.Reset(); impl_->factory.Reset();
	impl_->hardware = false; impl_->backendDescription.clear();
}

std::unique_ptr<AvisoRasterRenderResult> VsmrAviso::AvisoVectorRenderer::Render(
	const AvisoRasterRenderRequest& request, double originX, double originY,
	std::unique_ptr<AvisoRasterRenderResult> reusable)
{
	impl_->stats = {};
	impl_->error = S_OK;
	impl_->timings = {};
	impl_->nativeLabelsRendered = false;
	impl_->visibleLabelCount = 0;
	auto checkpoint = std::chrono::steady_clock::now();
	auto elapsed = [&]() {
		const auto now = std::chrono::steady_clock::now();
		const double milliseconds = std::chrono::duration<double,std::milli>(now-checkpoint).count();
		checkpoint = now;
		return milliseconds;
	};
	const double lonSpan = request.displayMaxLongitude - request.displayMinLongitude;
	const double latSpan = request.displayMaxLatitude - request.displayMinLatitude;
	if (!request.features || !request.labels || request.rasterWidth <= 0 || request.rasterHeight <= 0 ||
		request.rasterWidth > 6400 || request.rasterHeight > 6400 ||
		static_cast<std::uint64_t>(request.rasterWidth) * request.rasterHeight > 32000000ULL ||
		!std::isfinite(lonSpan) || !std::isfinite(latSpan) || lonSpan <= 0.0 || latSpan <= 0.0 ||
		!std::isfinite(request.displayScale) || request.displayScale <= 0.0)
		return nullptr;
	try
	{
		impl_->EnsureTarget(request.rasterWidth, request.rasterHeight);
		impl_->Prepare(request);
		const auto projection = request.preciseProjection.has_value() ? *request.preciseProjection : BilinearProjection::FromCorners(
			GeoBounds{request.displayMinLongitude,request.displayMinLatitude,request.displayMaxLongitude,request.displayMaxLatitude},
			request.projectedTopLeft,request.projectedTopRight,request.projectedBottomLeft,request.projectedBottomRight);
		if (!projection.IsValid()) return nullptr;
		const double bx = projection.Horizontal().X;
		const double by = projection.Horizontal().Y;
		const double cx = projection.Vertical().X;
		const double cy = projection.Vertical().Y;
		const double projectionLonSpan = projection.LongitudeSpan();
		const double projectionLatSpan = projection.LatitudeSpan();
		const bool affine = projection.IsAffine();
		auto project = [&](double longitude, double latitude) {
			const auto point = projection.Project(longitude,latitude);
			return D2D1::Point2F(static_cast<float>(point.X - originX),static_cast<float>(point.Y - originY));
		};
		const AvisoGeoBounds bounds{request.renderMinLongitude, request.renderMinLatitude,
			request.renderMaxLongitude, request.renderMaxLatitude};
		impl_->scene->QueryFeatures(bounds, impl_->featureCandidates);
		impl_->scene->QueryLabels(bounds, impl_->labelCandidates);
		impl_->stats.featureCandidates = impl_->featureCandidates.size();
		impl_->stats.labelCandidates = impl_->labelCandidates.size();
		impl_->timings.prepareMilliseconds = elapsed();
		auto* target = impl_->target.Get();
		target->BeginDraw();
		target->SetTransform(D2D1::Matrix3x2F::Identity());
		const Gdiplus::Color background = request.backgroundColor.GetAlpha() == 255 ? request.backgroundColor : Gdiplus::Color(0,0,0,0);
		target->Clear(Color(background));
		// Geometry paths frequently share their transform and stroke color. Avoid
		// repeating state commands while keeping every primitive in source order.
		// This cache ends with the geometry pass; labels manage their own state.
		D2D1_MATRIX_3X2_F geometryTransform = D2D1::Matrix3x2F::Identity();
		static_assert(sizeof(geometryTransform) == 6U*sizeof(float), "Transform state must contain only six floats");
		auto setGeometryTransform = [&](const D2D1_MATRIX_3X2_F& next) {
			if (std::memcmp(&geometryTransform,&next,sizeof(next)) != 0)
			{
				target->SetTransform(next);
				geometryTransform = next;
			}
		};
		Gdiplus::ARGB geometryColor = 0;
		bool geometryColorKnown = false;
		auto setGeometryColor = [&](Gdiplus::Color next) {
			const auto value = next.GetValue();
			if (!geometryColorKnown || geometryColor != value)
			{
				impl_->brush->SetColor(Color(next));
				geometryColor = value;
				geometryColorKnown = true;
			}
		};
		for (const auto index : impl_->featureCandidates)
		{
			const auto& feature = (*request.features)[index];
			if (!Visible(feature.groupIds, request.groupVisibility.get()) ||
				(feature.minimumZoomLevel > 0 && request.viewportZoomLevel < feature.minimumZoomLevel)) continue;
			const auto fill = request.colorPalette == "real" ? feature.realFillColor : request.colorPalette == "light" ? feature.lightFillColor : feature.fillColor;
			const auto stroke = request.colorPalette == "real" ? feature.realStrokeColor : request.colorPalette == "light" ? feature.lightStrokeColor : feature.strokeColor;
			const bool outline = request.colorPalette == "real" ? feature.realPolygonOutline : request.colorPalette == "light" ? feature.lightPolygonOutline : feature.polygonOutline;
			const bool fillVisible = feature.polygon && fill.GetAlpha() > 0;
			const bool strokeVisible = (!feature.polygon || outline) && stroke.GetAlpha() > 0 && feature.strokeWidth > 0.0f;
			if (!fillVisible && !strokeVisible) continue;
			const float strokeWidth = static_cast<float>(feature.strokeWidth * request.displayScale);
			auto& compiled = impl_->geometry[index];
			const bool useRetainedGeometry = affine && compiled.safeWorldBounds;
			auto featureTransform = D2D1::Matrix3x2F::Identity();
			if (useRetainedGeometry)
			{
				const auto origin = project(compiled.longitude, compiled.latitude);
				featureTransform = D2D1::Matrix3x2F(static_cast<float>(bx / projectionLonSpan / Impl::WorldUnitsPerDegree),
					static_cast<float>(by / projectionLonSpan / Impl::WorldUnitsPerDegree),
					static_cast<float>(-cx / projectionLatSpan / Impl::WorldUnitsPerDegree),
					static_cast<float>(-cy / projectionLatSpan / Impl::WorldUnitsPerDegree), origin.x, origin.y);
			}
			for (std::size_t p = 0; p < feature.paths.size(); ++p)
			{
				if (feature.paths[p].size() < (feature.polygon ? 3U : 2U)) continue;
				if (!feature.polygon && feature.paths[p].size() == 2U)
				{
					const auto first = project(feature.paths[p][0].longitude,feature.paths[p][0].latitude);
					const auto second = project(feature.paths[p][1].longitude,feature.paths[p][1].latitude);
					const float margin = strokeWidth*0.5f + 2.0f;
					if ((std::max)(first.x,second.x) < -margin || (std::min)(first.x,second.x) > request.rasterWidth+margin ||
						(std::max)(first.y,second.y) < -margin || (std::min)(first.y,second.y) > request.rasterHeight+margin)
					{ ++impl_->stats.pathsCulled; continue; }
					setGeometryTransform(D2D1::Matrix3x2F::Identity());
					setGeometryColor(stroke);
					target->DrawLine(first,second,impl_->brush.Get(),strokeWidth,impl_->lineStyle.Get());
					++impl_->stats.pathsDrawn; impl_->stats.projectedVertices += 2U;
					continue;
				}
				setGeometryTransform(featureTransform);
				const auto& pathBounds = impl_->scene->PathBounds(index, p);
				const D2D1_POINT_2F corners[] = {project(pathBounds.minLongitude, pathBounds.minLatitude),
					project(pathBounds.minLongitude, pathBounds.maxLatitude), project(pathBounds.maxLongitude, pathBounds.minLatitude),
					project(pathBounds.maxLongitude, pathBounds.maxLatitude)};
				const float margin = (std::max)(strokeWidth * 0.5f, 1.0f) + 2.0f;
				const auto left = (std::min)({corners[0].x,corners[1].x,corners[2].x,corners[3].x});
				const auto right = (std::max)({corners[0].x,corners[1].x,corners[2].x,corners[3].x});
				const auto top = (std::min)({corners[0].y,corners[1].y,corners[2].y,corners[3].y});
				const auto bottom = (std::max)({corners[0].y,corners[1].y,corners[2].y,corners[3].y});
				if (right < -margin || left > request.rasterWidth + margin || bottom < -margin || top > request.rasterHeight + margin)
				{ ++impl_->stats.pathsCulled; continue; }
				ComPtr<ID2D1PathGeometry> projected;
				ID2D1PathGeometry* path = nullptr;
				if (!useRetainedGeometry)
				{
					projected = impl_->Path(feature.paths[p], feature.polygon, [&](const AvisoPoint& point){return project(point.longitude,point.latitude);});
					path = projected.Get(); impl_->stats.projectedVertices += feature.paths[p].size();
				}
				else
				{
					auto& retained = compiled.paths[p];
					if (!retained)
						retained = impl_->Path(feature.paths[p], feature.polygon, [&](const AvisoPoint& point) {
							return D2D1::Point2F(static_cast<float>((point.longitude - compiled.longitude) * Impl::WorldUnitsPerDegree),
								static_cast<float>((point.latitude - compiled.latitude) * Impl::WorldUnitsPerDegree));
						});
					path = retained.Get();
				}
				++impl_->stats.pathsDrawn;
				if (fillVisible)
				{
					setGeometryColor(fill);
					if (useRetainedGeometry && impl_->realizationTarget)
					{
						// All AVISO polygon edges are straight lines. Their retained
						// tessellation has no curve approximation that can become
						// visible when zoomed; raster antialiasing is still native.
						auto& realized = compiled.fills[p];
						if (!realized) Check(impl_->realizationTarget->CreateFilledGeometryRealization(path,0.01f,realized.GetAddressOf()));
						impl_->realizationTarget->DrawGeometryRealization(realized.Get(),impl_->brush.Get());
					}
					else target->FillGeometry(path,impl_->brush.Get());
				}
				if (strokeVisible) { setGeometryColor(stroke); target->DrawGeometry(path,impl_->brush.Get(),strokeWidth,
					feature.polygon ? impl_->polygonStyle.Get() : impl_->lineStyle.Get()); }
			}
		}
		impl_->timings.geometryMilliseconds = elapsed();
		const double centerLat = (request.displayMinLatitude + request.displayMaxLatitude) * 0.5;
		const double metersPerPixel = (std::max)(111320.0 * std::cos(centerLat * 3.14159265358979323846 / 180.0) / (std::max)(request.scaleX,1e-10),
			110540.0 / (std::max)(request.scaleY,1e-10));
		const int labelZoom = request.labelZoomLevel >= 0 ? request.labelZoomLevel : request.viewportZoomLevel;
		impl_->meshInstances.clear(); impl_->meshGlyphs.clear();
		const bool useNativeMesh = impl_->textMode != TextMode::Vector && !impl_->meshFallback;
		for (const auto index : impl_->labelCandidates)
		{
			const auto& label = (*request.labels)[index];
			if (label.text.empty() || !Visible(label.groupIds,request.groupVisibility.get()) ||
				(label.minimumZoomLevel > 0 && labelZoom < label.minimumZoomLevel) ||
				(label.maxMetersPerPixel > 0.0 && metersPerPixel > label.maxMetersPerPixel)) continue;
			const auto halo = request.colorPalette == "real" ? label.realHaloColor : request.colorPalette == "light" ? label.lightHaloColor : label.haloColor;
			const auto text = request.colorPalette == "real" ? label.realTextColor : request.colorPalette == "light" ? label.lightTextColor : label.textColor;
			if (text.GetAlpha() == 0 && (halo.GetAlpha() == 0 || label.haloWidth <= 0.0f)) continue;
			auto& slot = impl_->glyphs[index];
			std::shared_ptr<Impl::LabelGeometry> uncached;
			if (!slot || slot->scale != request.displayScale)
			{
				const Impl::GlyphKey key{label.text,label.fontFamily,label.textAnchor,label.textSize,label.haloWidth,request.displayScale};
				const auto found = impl_->sharedGlyphs.find(key);
				if (found != impl_->sharedGlyphs.end()) { slot = found->second; ++impl_->stats.glyphCacheHits; }
				else
				{
					++impl_->stats.glyphCacheMisses;
					auto replacement = std::make_shared<Impl::LabelGeometry>();
					replacement->path = impl_->Glyph(label,request.displayScale,replacement->points);
					if (replacement->path) Check(replacement->path->GetBounds(nullptr,&replacement->bounds));
					replacement->scale = request.displayScale;
					if (replacement->path && replacement->points <= Impl::GlyphPointBudget - impl_->glyphPoints)
					{
						impl_->glyphPoints += replacement->points;
						impl_->sharedGlyphs.emplace(key,replacement);
						slot = std::move(replacement);
					}
					else { slot.reset(); uncached = std::move(replacement); }
				}
			}
			else ++impl_->stats.glyphCacheHits;
			auto& glyph = slot ? *slot : *uncached;
			ID2D1PathGeometry* path = glyph.path.Get();
			if (!path) continue;
			const auto position = project(label.position.longitude,label.position.latitude);
			const float margin = static_cast<float>((std::max)(label.haloWidth*request.displayScale,0.0)) + 2.0f;
			if (glyph.bounds.right + position.x < -margin || glyph.bounds.left + position.x > request.rasterWidth + margin ||
				glyph.bounds.bottom + position.y < -margin || glyph.bounds.top + position.y > request.rasterHeight + margin)
				continue;
			++impl_->visibleLabelCount;
			if (useNativeMesh)
			{
				const float haloWidth = halo.GetAlpha() > 0 && label.haloWidth > 0.0f
					? static_cast<float>((std::max)(label.haloWidth*request.displayScale*2.0,1.0)) : 0.0f;
				impl_->meshInstances.push_back({path,position,haloWidth,Color(halo),Color(text)});
				impl_->meshGlyphs.push_back(slot ? slot : uncached);
				continue;
			}
			target->SetTransform(D2D1::Matrix3x2F::Translation(position.x,position.y));
			if (halo.GetAlpha() > 0 && label.haloWidth > 0.0f)
			{
				impl_->brush->SetColor(Color(halo));
				const float haloWidth = static_cast<float>((std::max)(label.haloWidth*request.displayScale*2.0,1.0));
				if (impl_->realizationTarget && slot)
				{
					if (!glyph.halo) Check(impl_->realizationTarget->CreateStrokedGeometryRealization(path,Impl::GlyphFlatteningTolerance,haloWidth,impl_->polygonStyle.Get(),glyph.halo.GetAddressOf()));
					impl_->realizationTarget->DrawGeometryRealization(glyph.halo.Get(),impl_->brush.Get());
				}
				else target->DrawGeometry(path,impl_->brush.Get(),haloWidth,impl_->polygonStyle.Get());
			}
			if (text.GetAlpha() > 0)
			{
				impl_->brush->SetColor(Color(text));
				if (impl_->realizationTarget && slot)
				{
					if (!glyph.fill) Check(impl_->realizationTarget->CreateFilledGeometryRealization(path,Impl::GlyphFlatteningTolerance,glyph.fill.GetAddressOf()));
					impl_->realizationTarget->DrawGeometryRealization(glyph.fill.Get(),impl_->brush.Get());
				}
				else target->FillGeometry(path,impl_->brush.Get());
			}
		}
		target->SetTransform(D2D1::Matrix3x2F::Identity());
		impl_->timings.labelMilliseconds = elapsed();
		Check(target->EndDraw());
		impl_->timings.flushMilliseconds = elapsed();
		if (useNativeMesh && !impl_->meshInstances.empty())
		{
			bool ready = false;
			ID3D11Texture2D* overlay = nullptr;
			ready = impl_->analyticGlyphs.Render(impl_->device.Get(),impl_->immediate.Get(),impl_->factory.Get(),
				impl_->polygonStyle.Get(),request.rasterWidth,request.rasterHeight,impl_->meshInstances);
			overlay = impl_->analyticGlyphs.Texture();
			target->BeginDraw();
			target->SetTransform(D2D1::Matrix3x2F::Identity());
			if (ready && overlay != nullptr)
			{
				if (overlay != impl_->meshTexture.Get())
				{
					impl_->meshTexture = overlay; impl_->meshBitmap.Reset();
					ComPtr<IDXGISurface> surface;
					const HRESULT surfaceStatus = impl_->meshTexture.As(&surface);
					const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
						D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96.0f,96.0f);
					ready = SUCCEEDED(surfaceStatus) && SUCCEEDED(target->CreateBitmapFromDxgiSurface(
						surface.Get(),&properties,impl_->meshBitmap.GetAddressOf()));
				}
			}
			else ready = false;
			if (ready && impl_->meshBitmap)
			{
				target->DrawBitmap(impl_->meshBitmap.Get(),nullptr,1.0f,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
				impl_->nativeLabelsRendered = true;
			}
			else
			{
				// A device/capability/budget failure selects one stable vector path
				// for the rest of this scene. Never publish a partial label layer.
				impl_->meshFallback = true;
				impl_->meshBitmap.Reset(); impl_->meshTexture.Reset();
				for (const auto& instance : impl_->meshInstances)
				{
					target->SetTransform(D2D1::Matrix3x2F::Translation(instance.anchor.x,instance.anchor.y));
					if (instance.haloWidth > 0.0f && instance.haloColor.a > 0.0f)
					{ impl_->brush->SetColor(instance.haloColor); target->DrawGeometry(instance.outline,impl_->brush.Get(),instance.haloWidth,impl_->polygonStyle.Get()); }
					if (instance.textColor.a > 0.0f)
					{ impl_->brush->SetColor(instance.textColor); target->FillGeometry(instance.outline,impl_->brush.Get()); }
				}
			}
			target->SetTransform(D2D1::Matrix3x2F::Identity());
			Check(target->EndDraw());
			impl_->timings.labelMilliseconds += elapsed();
		}
		Dib output;
		void* outputBits = nullptr;
		if (reusable && reusable->bitmap != nullptr)
		{
			DIBSECTION dib{};
			if (GetObject(reusable->bitmap,sizeof(dib),&dib) == sizeof(dib) && dib.dsBm.bmBits != nullptr &&
				dib.dsBm.bmWidth == request.rasterWidth && dib.dsBm.bmHeight == request.rasterHeight &&
				dib.dsBm.bmBitsPixel == 32 && dib.dsBm.bmWidthBytes == request.rasterWidth * 4)
			{
				// Complete earlier GDI source reads before writing the reused DIB.
				GdiFlush();
				outputBits = dib.dsBm.bmBits;
			}
		}
		if (outputBits == nullptr)
		{
			reusable.reset();
			output.Create(request.rasterWidth,request.rasterHeight);
			outputBits = output.bits;
		}
		impl_->immediate->CopyResource(impl_->staging.Get(),impl_->texture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(impl_->immediate->Map(impl_->staging.Get(),0,D3D11_MAP_READ,0,&mapped));
		const std::size_t stride = static_cast<std::size_t>(request.rasterWidth)*4;
		if (mapped.RowPitch == stride)
			std::memcpy(outputBits,mapped.pData,stride*static_cast<std::size_t>(request.rasterHeight));
		else
			for (int y = 0; y < request.rasterHeight; ++y)
				std::memcpy(static_cast<BYTE*>(outputBits) + y*stride,static_cast<BYTE*>(mapped.pData) + y*mapped.RowPitch,stride);
		impl_->immediate->Unmap(impl_->staging.Get(),0);
		impl_->timings.readbackMilliseconds = elapsed();
		auto result = reusable ? std::move(reusable) : std::make_unique<AvisoRasterRenderResult>();
		result->requestId=request.requestId; result->groupGeneration=request.groupGeneration; result->colorPalette=request.colorPalette;
		result->backgroundColor=background; result->displayScale=request.displayScale; result->viewportZoomLevel=request.viewportZoomLevel;
		result->labelZoomLevel=request.labelZoomLevel; result->path=request.path; result->rasterWidth=request.rasterWidth; result->rasterHeight=request.rasterHeight;
		result->displayMinLongitude=request.displayMinLongitude; result->displayMaxLongitude=request.displayMaxLongitude;
		result->displayMinLatitude=request.displayMinLatitude; result->displayMaxLatitude=request.displayMaxLatitude;
		result->renderMinLongitude=request.renderMinLongitude; result->renderMaxLongitude=request.renderMaxLongitude;
		result->renderMinLatitude=request.renderMinLatitude; result->renderMaxLatitude=request.renderMaxLatitude;
		result->projectedTopLeft=request.projectedTopLeft; result->projectedTopRight=request.projectedTopRight;
		result->projectedBottomLeft=request.projectedBottomLeft; result->projectedBottomRight=request.projectedBottomRight;
		if (output.bitmap != nullptr) { result->bitmap=output.bitmap; output.bitmap=nullptr; }
		return result;
	}
	catch (HRESULT error)
	{
		// A lost device must never publish partial pixels. Retain CPU-independent
		// paths, recreate the device on the next frame, and retry at native pixels.
		impl_->DiscardDevice();
		impl_->error = error;
		return nullptr;
	}
	catch (const std::bad_alloc&)
	{
		impl_->DiscardDevice();
		impl_->error = E_OUTOFMEMORY;
		return nullptr;
	}
}
