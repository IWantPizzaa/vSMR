#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "radar/RadarScreen.hpp"
#include "rendering/TagRenderer.hpp"

#include <algorithm>

#if defined(_DEBUG)
#define VSMR_REFRESH_LOG(message) Logger::info(message)
#else
#define VSMR_REFRESH_LOG(message) do { } while (0)
#endif

extern CPoint mouseLocation;
extern string TagBeingDragged;
extern int LeaderLineDefaultlenght;

namespace
{
	Gdiplus::Color ToGdiColor(const VsmrScene::Color& color)
	{
		return Gdiplus::Color(color.alpha, color.red, color.green, color.blue);
	}

	bool MouseWithinTag(const CRect& rect)
	{
		return mouseLocation.x >= rect.left + 1 &&
			mouseLocation.x <= rect.right - 1 &&
			mouseLocation.y >= rect.top + 1 &&
			mouseLocation.y <= rect.bottom - 1;
	}

	std::string ResolveMovementAlertText(CRimcas::RimcasAlerts alert)
	{
		switch (alert)
		{
		case CRimcas::RimcasAlerts::STATRPA:
			return "STAT RPA";
		case CRimcas::RimcasAlerts::NOPUSH:
			return "NO PUSH CLR";
		case CRimcas::RimcasAlerts::NOTKOF:
			return "NO TKOF CLR";
		case CRimcas::RimcasAlerts::NOTAXI:
			return "NO TAXI CLR";
		case CRimcas::RimcasAlerts::RWYINC:
			return "RWY INCURSION";
		case CRimcas::RimcasAlerts::RWYTYPE:
			return "RWY TYPE";
		case CRimcas::RimcasAlerts::RWYCLSD:
			return "RWY CLOSED";
		case CRimcas::RimcasAlerts::HIGHSPD:
			return "HIGH SPEED";
		case CRimcas::RimcasAlerts::EMERG:
			return "EMERG";
		default:
			return std::string();
		}
	}

	CRect CenteredTagRect(const POINT& center, int width, int height)
	{
		return CRect(
			center.x - width / 2,
			center.y - height / 2,
			center.x - width / 2 + width,
			center.y - height / 2 + height);
	}
}

void CSMRRadar::RenderTags(Graphics& graphics, CDC& dc)
{
	(void)dc;
	VSMR_REFRESH_LOG("Tags loop");
	if (CurrentConfig == nullptr || RimcasInstance == nullptr)
	{
		if (Logger::is_verbose_mode())
			Logger::info("RenderTags: skipped (missing config/rimcas dependency)");
		return;
	}

	const Value& activeProfile = CurrentConfig->getActiveProfile();
	if (!activeProfile.IsObject() ||
		!activeProfile.HasMember("labels") ||
		!activeProfile["labels"].IsObject())
	{
		if (Logger::is_verbose_mode())
			Logger::info("RenderTags: active profile has no valid labels object");
		return;
	}

	const Value& labels = activeProfile["labels"];
	const bool roundedCorners =
		(!labels.HasMember("rounded_corners") || !labels["rounded_corners"].IsBool()) ||
		labels["rounded_corners"].GetBool();

	auto font = customFonts.find(currentFontSize);
	Gdiplus::Font* regularFont = font != customFonts.end() ? font->second.get() : nullptr;
	VsmrTagRendering::FontContext fonts(graphics, regularFont);
	if (!fonts.IsValid())
	{
		if (Logger::is_verbose_mode())
			Logger::info("RenderTags: no font loaded for size=" + std::to_string(currentFontSize));
		return;
	}

	const auto profileColor = [&](const char* key, const Color& fallback) -> Color
	{
		if (activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
		{
			const Value& rimcas = activeProfile["rimcas"];
			if (rimcas.HasMember(key) && rimcas[key].IsObject())
				return CurrentConfig->getConfigColor(rimcas[key]);
		}
		return fallback;
	};

	const Color cautionText = profileColor("caution_alert_text_color", Color(255, 30, 30, 30));
	const Color warningText = profileColor("warning_alert_text_color", Color(255, 255, 255, 255));
	const Color cautionBackground = profileColor("caution_alert_background_color", Color(230, 255, 215, 0));
	const Color warningBackground = profileColor("warning_alert_background_color", Color(230, 200, 40, 40));

	const VsmrScene::RadarScene* scene = GetCurrentRadarScene();
	if (scene == nullptr)
		return;

	for (const VsmrScene::Target& sceneTarget : scene->targets)
	{
		if (!sceneTarget.tagVisible || !sceneTarget.position.valid)
			continue;

		const std::string& callsign = sceneTarget.callsign;
		CPosition position;
		position.m_Latitude = sceneTarget.position.latitude;
		position.m_Longitude = sceneTarget.position.longitude;
		const POINT targetPoint = ConvertCoordFromPositionToPixel(position);

		POINT tagCenter = {};
		const auto offset = TagsOffsets.find(callsign);
		if (offset != TagsOffsets.end())
		{
			tagCenter = { targetPoint.x + offset->second.x, targetPoint.y + offset->second.y };
		}
		else
		{
			if (TagAngles.find(callsign) == TagAngles.end())
				TagAngles[callsign] = 270.0f;
			int leaderLength = LeaderLineDefaultlenght;
			const auto configuredLength = TagLeaderLineLength.find(callsign);
			if (configuredLength != TagLeaderLineLength.end())
				leaderLength = configuredLength->second;
			tagCenter.x = long(targetPoint.x + float(leaderLength * cos(VsmrRadarUiSupport::DegToRad(TagAngles[callsign]))));
			tagCenter.y = long(targetPoint.y + float(leaderLength * sin(VsmrRadarUiSupport::DegToRad(TagAngles[callsign]))));
		}

		const auto previousSize = previousTagSize.find(callsign);
		const int probeWidth = previousSize != previousTagSize.end() ? previousSize->second.Width() : 0;
		const int probeHeight = previousSize != previousTagSize.end() ? previousSize->second.Height() : 0;
		const CRect hoverProbe = CenteredTagRect(tagCenter, probeWidth, probeHeight);
		const bool dragged = TagBeingDragged == callsign;
		const bool detailed = dragged || MouseWithinTag(hoverProbe);

		const VsmrScene::TagVariant& variant = detailed
			? sceneTarget.tag.detailed
			: sceneTarget.tag.normal;
		const VsmrScene::TagPalette& palette = detailed
			? sceneTarget.tag.detailedPalette
			: sceneTarget.tag.normalPalette;

		VsmrTagRendering::Layout layout;
		if (!VsmrTagRendering::MeasureLayout(fonts, variant, layout))
			continue;
		VsmrTagRendering::Layout collisionLayout;
		const VsmrTagRendering::Layout* collisionLayoutToUse = &layout;
		if (detailed && VsmrTagRendering::MeasureLayout(fonts, sceneTarget.tag.normal, collisionLayout))
			collisionLayoutToUse = &collisionLayout;

		const bool isDeparture = sceneTarget.role == VsmrScene::TargetRole::Departure;
		const CRimcas::RimcasAlerts movementAlert =
			static_cast<CRimcas::RimcasAlerts>(sceneTarget.rimcas.movementAlert);
		const std::string alertText = isDeparture
			? ResolveMovementAlertText(movementAlert)
			: std::string();
		const CRimcas::RimcasAlertSeverity severity =
			static_cast<CRimcas::RimcasAlertSeverity>(sceneTarget.rimcas.severity);
		VsmrTagRendering::TopBand alertBand;
		if (!alertText.empty())
		{
			alertBand.text = alertText;
			alertBand.action = TAG_CITEM_NO;
			alertBand.background = severity == CRimcas::RimcasAlertSeverity::WARNING
				? warningBackground
				: cautionBackground;
			alertBand.textColor = severity == CRimcas::RimcasAlertSeverity::WARNING
				? warningText
				: cautionText;
		}

		const Color background = sceneTarget.rimcas.onRunway
			? ToGdiColor(palette.backgroundOnRunway)
			: ToGdiColor(palette.background);

		VsmrTagRendering::PaintOptions options;
		options.targetPoint = targetPoint;
		options.tagCenter = tagCenter;
		options.background = background;
		options.roundedCorners = roundedCorners;
		options.extendScratchpadHit = true;
		options.scratchpadAction = TAG_CITEM_SCRATCHPAD;
		options.topBand = !alertText.empty() ? &alertBand : nullptr;
		options.topBandHitMode = VsmrTagRendering::TopBandHitMode::TextOnly;
		const CRect expectedBounds =
			VsmrTagRendering::CalculateBounds(fonts, layout, options);
		options.highlighted = dragged || MouseWithinTag(expectedBounds);

		VsmrTagRendering::PaintOptions rawBoundsOptions = options;
		rawBoundsOptions.roundedCorners = false;
		previousTagSize[callsign] =
			VsmrTagRendering::CalculateBounds(fonts, layout, rawBoundsOptions);

		const VsmrTagRendering::PaintResult painted =
			VsmrTagRendering::Paint(graphics, fonts, layout, options);
		if (painted.bounds.IsRectEmpty())
			continue;

		VsmrTagRendering::PaintOptions collisionOptions = options;
		collisionOptions.highlighted = false;
		tagAreas[callsign] = painted.bounds;
		tagCollisionAreas[callsign] =
			VsmrTagRendering::CalculateBounds(fonts, *collisionLayoutToUse, collisionOptions);
		const char* bottomLine = sceneTarget.bottomLine.c_str();
		AddScreenObject(DRAWING_TAG, callsign.c_str(), painted.bounds, true, bottomLine);
		for (const VsmrTagRendering::HitRegion& hit : painted.hitRegions)
			AddScreenObject(hit.action, callsign.c_str(), hit.area, true, bottomLine);
	}
}
