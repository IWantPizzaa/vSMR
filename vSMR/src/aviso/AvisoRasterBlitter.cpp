#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterBlitter.hpp"

#include <limits>

namespace
{
	class SelectedObject final
	{
	public:
		SelectedObject(HDC dc, HGDIOBJ object) noexcept : dc_(dc)
		{
			if (dc_ != nullptr && object != nullptr)
				previous_ = ::SelectObject(dc_, object);
		}

		~SelectedObject()
		{
			if (IsValid())
				::SelectObject(dc_, previous_);
		}

		bool IsValid() const noexcept
		{
			return previous_ != nullptr && previous_ != HGDI_ERROR;
		}

	private:
		HDC dc_ = nullptr;
		HGDIOBJ previous_ = nullptr;
	};

	class SavedDc final
	{
	public:
		explicit SavedDc(HDC dc) noexcept : dc_(dc), state_(dc != nullptr ? ::SaveDC(dc) : 0)
		{
		}

		~SavedDc()
		{
			if (state_ != 0)
				::RestoreDC(dc_, state_);
		}

		bool IsValid() const noexcept
		{
			return state_ != 0;
		}

	private:
		HDC dc_ = nullptr;
		int state_ = 0;
	};

	class StretchMode final
	{
	public:
		StretchMode(HDC dc, int mode) noexcept : dc_(dc)
		{
			if (dc_ != nullptr)
				previous_ = ::SetStretchBltMode(dc_, mode);
		}

		~StretchMode()
		{
			if (previous_ != 0)
				::SetStretchBltMode(dc_, previous_);
		}

	private:
		HDC dc_ = nullptr;
		int previous_ = 0;
	};

	bool TryGetExtent(LONG start, LONG end, int& extent) noexcept
	{
		const long long value = static_cast<long long>(end) - static_cast<long long>(start);
		if (value <= 0 || value > (std::numeric_limits<int>::max)())
			return false;
		extent = static_cast<int>(value);
		return true;
	}

	bool IsNearNative(int destinationExtent, int sourceExtent) noexcept
	{
		const long long difference =
			static_cast<long long>(destinationExtent) - static_cast<long long>(sourceExtent);
		return difference >= -1 && difference <= 1;
	}
}

bool VsmrAviso::TryBuildRasterBlitPlan(
	const RECT& source,
	const RECT& destination,
	const RECT& clip,
	RasterBlitPlan& plan) noexcept
{
	RasterBlitPlan candidate;
	if (!TryGetExtent(source.left, source.right, candidate.sourceWidth) ||
		!TryGetExtent(source.top, source.bottom, candidate.sourceHeight))
	{
		return false;
	}

	int requestedDestinationWidth = 0;
	int requestedDestinationHeight = 0;
	if (!TryGetExtent(destination.left, destination.right, requestedDestinationWidth) ||
		!TryGetExtent(destination.top, destination.bottom, requestedDestinationHeight))
	{
		return false;
	}

	candidate.source = source;
	candidate.destination = destination;
	candidate.clip = clip;
	candidate.destinationWidth = IsNearNative(
		requestedDestinationWidth,
		candidate.sourceWidth)
		? candidate.sourceWidth
		: requestedDestinationWidth;
	candidate.destinationHeight = IsNearNative(
		requestedDestinationHeight,
		candidate.sourceHeight)
		? candidate.sourceHeight
		: requestedDestinationHeight;

	const long long destinationRight =
		static_cast<long long>(candidate.destination.left) + candidate.destinationWidth;
	const long long destinationBottom =
		static_cast<long long>(candidate.destination.top) + candidate.destinationHeight;
	if (destinationRight > (std::numeric_limits<LONG>::max)() ||
		destinationRight < (std::numeric_limits<LONG>::min)() ||
		destinationBottom > (std::numeric_limits<LONG>::max)() ||
		destinationBottom < (std::numeric_limits<LONG>::min)())
	{
		return false;
	}
	candidate.destination.right = static_cast<LONG>(destinationRight);
	candidate.destination.bottom = static_cast<LONG>(destinationBottom);
	candidate.scaled =
		candidate.destinationWidth != candidate.sourceWidth ||
		candidate.destinationHeight != candidate.sourceHeight;
	plan = candidate;
	return true;
}

VsmrAviso::AvisoRasterBlitter::~AvisoRasterBlitter() noexcept
{
	if (sourceDc_ != nullptr)
		::DeleteDC(sourceDc_);
}

bool VsmrAviso::AvisoRasterBlitter::EnsureSourceDc(HDC destinationDc) noexcept
{
	if (sourceDc_ == nullptr && destinationDc != nullptr)
		sourceDc_ = ::CreateCompatibleDC(destinationDc);
	return sourceDc_ != nullptr;
}

bool VsmrAviso::AvisoRasterBlitter::Blend(
	Gdiplus::Graphics& graphics,
	HDC destinationDc,
	HBITMAP sourceBitmap,
	const RECT& source,
	const RECT& destination,
	const RECT& clip) noexcept
{
	if (destinationDc == nullptr || sourceBitmap == nullptr)
		return false;

	RasterBlitPlan plan;
	if (!TryBuildRasterBlitPlan(source, destination, clip, plan))
		return false;

	if (!EnsureSourceDc(destinationDc))
		return false;
	SelectedObject selectedBitmap(sourceDc_, sourceBitmap);
	if (!selectedBitmap.IsValid())
		return false;

	graphics.Flush(Gdiplus::FlushIntentionFlush);
	SavedDc savedDestination(destinationDc);
	if (!savedDestination.IsValid())
		return false;

	::IntersectClipRect(
		destinationDc,
		plan.clip.left,
		plan.clip.top,
		plan.clip.right,
		plan.clip.bottom);
	StretchMode stretchMode(destinationDc, plan.scaled ? HALFTONE : COLORONCOLOR);
	if (plan.scaled)
		::SetBrushOrgEx(destinationDc, 0, 0, nullptr);

	BLENDFUNCTION blend{};
	blend.BlendOp = AC_SRC_OVER;
	blend.SourceConstantAlpha = 255;
	blend.AlphaFormat = AC_SRC_ALPHA;
	return ::AlphaBlend(
		destinationDc,
		plan.destination.left,
		plan.destination.top,
		plan.destinationWidth,
		plan.destinationHeight,
		sourceDc_,
		plan.source.left,
		plan.source.top,
		plan.sourceWidth,
		plan.sourceHeight,
		blend) != FALSE;
}
