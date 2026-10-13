#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoViewportRenderer.hpp"

#include <chrono>

namespace
{
	bool EqualPoint(const Gdiplus::PointF& a, const Gdiplus::PointF& b) noexcept
	{
		return a.X == b.X && a.Y == b.Y;
	}
}

bool VsmrAviso::AvisoViewportRenderer::Matches(const RECT& viewport,
	const VsmrRadarTypes::AvisoRasterRenderRequest& request) const noexcept
{
	if (cache_ == nullptr || !key_.has_value() || !::EqualRect(&viewport, &viewport_))
		return false;
	const auto& key = *key_;
	if (key.preciseProjection.has_value() != request.preciseProjection.has_value() ||
		(key.preciseProjection.has_value() && !key.preciseProjection->SameMapping(*request.preciseProjection)))
		return false;
	return key.path == request.path && key.features == request.features && key.labels == request.labels &&
		key.scene == request.scene && key.groupVisibility == request.groupVisibility &&
		key.groupGeneration == request.groupGeneration && key.colorPalette == request.colorPalette &&
		key.backgroundColor.GetValue() == request.backgroundColor.GetValue() &&
		key.displayScale == request.displayScale && key.rasterScale == request.rasterScale &&
		key.viewportZoomLevel == request.viewportZoomLevel && key.labelZoomLevel == request.labelZoomLevel &&
		key.rasterWidth == request.rasterWidth && key.rasterHeight == request.rasterHeight &&
		key.displayMinLongitude == request.displayMinLongitude && key.displayMaxLongitude == request.displayMaxLongitude &&
		key.displayMinLatitude == request.displayMinLatitude && key.displayMaxLatitude == request.displayMaxLatitude &&
		key.renderMinLongitude == request.renderMinLongitude && key.renderMaxLongitude == request.renderMaxLongitude &&
		key.renderMinLatitude == request.renderMinLatitude && key.renderMaxLatitude == request.renderMaxLatitude &&
		key.scaleX == request.scaleX && key.scaleY == request.scaleY &&
		EqualPoint(key.projectedTopLeft, request.projectedTopLeft) &&
		EqualPoint(key.projectedTopRight, request.projectedTopRight) &&
		EqualPoint(key.projectedBottomLeft, request.projectedBottomLeft) &&
		EqualPoint(key.projectedBottomRight, request.projectedBottomRight);
}

VsmrAviso::AvisoViewportRenderer::Outcome VsmrAviso::AvisoViewportRenderer::Draw(
	Gdiplus::Graphics& graphics, HDC destination, const RECT& viewport,
	const VsmrRadarTypes::AvisoRasterRenderRequest& request,
	const std::function<void()>& paintBackground)
{
	Outcome outcome;
	const long long width = static_cast<long long>(viewport.right) - viewport.left;
	const long long height = static_cast<long long>(viewport.bottom) - viewport.top;
	if (destination == nullptr || width <= 0 || height <= 0 ||
		request.rasterWidth != width || request.rasterHeight != height || request.rasterScale != 1.0)
		return outcome;
	outcome.cacheHit = Matches(viewport, request);
	if (!outcome.cacheHit)
	{
		outcome.buildAttempted = true;
		const auto started = std::chrono::steady_clock::now();
		key_.reset();
		auto replacement = renderer_.Render(request, viewport.left, viewport.top, std::move(cache_));
		outcome.buildMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		if (replacement == nullptr || replacement->bitmap == nullptr)
			return outcome;
		// Publish identity and image together, including on first use after an
		// allocation/device failure. Never display the old view at a new scale.
		auto replacementKey = request;
		key_ = std::move(replacementKey);
		cache_ = std::move(replacement);
		viewport_ = viewport;
	}
	outcome.hardware = renderer_.UsesHardware();
	const bool opaque = cache_->backgroundColor.GetAlpha() == 255;
	if (!opaque && paintBackground)
		paintBackground();
	outcome.drawn = blitter_.Blend(graphics, destination, cache_->bitmap,
		RECT{ 0, 0, request.rasterWidth, request.rasterHeight }, viewport, viewport, opaque);
	return outcome;
}

void VsmrAviso::AvisoViewportRenderer::Clear() noexcept
{
	cache_.reset();
	key_.reset();
	viewport_ = {};
}

HBITMAP VsmrAviso::AvisoViewportRenderer::Bitmap() const noexcept
{
	return cache_ != nullptr ? cache_->bitmap : nullptr;
}

void VsmrAviso::AvisoViewportRenderer::ReleaseResources() noexcept
{
	Clear();
	renderer_.ReleaseResources();
}

std::uint64_t VsmrAviso::AvisoViewportRenderer::BitmapBytes() const noexcept
{
	return cache_ != nullptr ? static_cast<std::uint64_t>(cache_->rasterWidth) *
		static_cast<std::uint64_t>(cache_->rasterHeight) * 4ULL : 0ULL;
}
