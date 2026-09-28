#pragma once

#include <string_view>

namespace VsmrRadar
{
	enum class ScreenRole { Unsupported, SurfaceRadar, CoFranceInsets };

	inline bool IsInsetChromeDrag(std::string_view id) noexcept
	{
		return id == "topbar" || id == "resize_left" || id == "resize_right" ||
			id == "resize_top" || id == "resize_bottom" || id == "resize_tl" ||
			id == "resize_tr" || id == "resize_bl" || id == "resize_br";
	}

	inline bool UsesAfterListsPhase(bool insetsOnly, bool needRadarContent) noexcept
	{
		// Do not depend on native TAG/list phases for custom displays that
		// disable EuroScope's native radar content (including CoFrance).
		return insetsOnly && needRadarContent;
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
