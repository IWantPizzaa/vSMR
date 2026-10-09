#pragma once

#include <string_view>
#include <EuroScopePlugIn.h>

namespace VsmrRadar
{
	enum class ScreenRole { Unsupported, SurfaceRadar, CoFranceInsets };

	inline bool IsInsetChromeDrag(std::string_view id) noexcept
	{
		return id == "topbar" || id == "resize_left" || id == "resize_right" ||
			id == "resize_top" || id == "resize_bottom" || id == "resize_tl" ||
			id == "resize_tr" || id == "resize_bl" || id == "resize_br";
	}

	inline int ResolveRefreshPhase(bool insetsOnly, bool needRadarContent, unsigned int phasesSeen) noexcept
	{
		using namespace EuroScopePlugIn;
		if (!insetsOnly) return REFRESH_PHASE_BEFORE_TAGS;
		// Custom renderers may provide late callbacks even with native radar
		// content disabled. Prefer the latest phase actually delivered so their
		// targets/tags stay behind our inset surfaces, independent of DLL order.
		if (needRadarContent || (phasesSeen & (1u << REFRESH_PHASE_AFTER_LISTS)) != 0)
			return REFRESH_PHASE_AFTER_LISTS;
		if ((phasesSeen & (1u << REFRESH_PHASE_AFTER_TAGS)) != 0)
			return REFRESH_PHASE_AFTER_TAGS;
		// Older custom hosts may only deliver BEFORE_TAGS. Keep them visible;
		// on the first frame a later callback upgrades the phase immediately.
		return REFRESH_PHASE_BEFORE_TAGS;
	}

	inline ScreenRole ResolveScreenRole(const char* displayName, bool geoReferenced) noexcept
	{
		if (displayName == nullptr) return ScreenRole::Unsupported;
		const std::string_view name(displayName);
		if (name == "SMR radar display") return ScreenRole::SurfaceRadar;
		// Deliberately opt in only the installed CoFrance display type. Do not
		// attach to every third-party/standard EuroScope screen.
		if (geoReferenced && name == "CoFrance radar display") return ScreenRole::CoFranceInsets;
		return ScreenRole::Unsupported;
	}
}
