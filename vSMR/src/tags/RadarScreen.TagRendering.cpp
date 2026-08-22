#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/ResourceIds.h"
#include "radar/RadarScreen.hpp"
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

}

void CSMRRadar::RenderTags(Graphics& graphics, CDC& dc)
{
	(void)dc;
	// Drawing the Tags
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

	const Value& LabelsSettings = activeProfile["labels"];
	const auto verboseTargetStep = [&](const std::string& callsign, const std::string& step)
	{
		if (!Logger::is_verbose_mode())
			return;
		Logger::info("RenderTags: " + callsign + " " + step);
	};
	const bool roundedTagCornersEnabled =
		(!LabelsSettings.HasMember("rounded_corners") || !LabelsSettings["rounded_corners"].IsBool()) ||
		LabelsSettings["rounded_corners"].GetBool();
	const auto isTagBeingDragged = [](const string& callsign) -> bool
	{
		return TagBeingDragged == callsign;
	};
	const auto isMouseWithinRect = [](const CRect& rect) -> bool
	{
		return mouseLocation.x >= rect.left + 1 &&
			mouseLocation.x <= rect.right - 1 &&
			mouseLocation.y >= rect.top + 1 &&
			mouseLocation.y <= rect.bottom - 1;
	};
	struct TagHoverState
	{
		CRect probeRect;
		bool isDragged = false;
		bool pointerInProbe = false;
		bool useDetailedLayout = false;
	};
	const auto buildHoverProbeRect = [&](const std::string& callsign, const POINT& tagCenter) -> CRect
	{
		auto previousSizeIt = previousTagSize.find(callsign);
		const int probeWidth = (previousSizeIt != previousTagSize.end()) ? previousSizeIt->second.Width() : 0;
		const int probeHeight = (previousSizeIt != previousTagSize.end()) ? previousSizeIt->second.Height() : 0;
		const int probeLeft = tagCenter.x - (probeWidth / 2);
		const int probeTop = tagCenter.y - (probeHeight / 2);
		return CRect(probeLeft, probeTop, probeLeft + probeWidth, probeTop + probeHeight);
	};
	const auto resolveTagHoverState = [&](const std::string& callsign, const POINT& tagCenter) -> TagHoverState
	{
		TagHoverState hoverState;
		hoverState.probeRect = buildHoverProbeRect(callsign, tagCenter);
		hoverState.isDragged = isTagBeingDragged(callsign);
		hoverState.pointerInProbe = isMouseWithinRect(hoverState.probeRect);
		hoverState.useDetailedLayout = hoverState.isDragged || hoverState.pointerInProbe;
		return hoverState;
	};

	auto fontIt = customFonts.find(currentFontSize);
	Gdiplus::Font* tagRegularFont = (fontIt != customFonts.end()) ? fontIt->second.get() : nullptr;
	if (tagRegularFont == nullptr)
	{
		if (Logger::is_verbose_mode())
			Logger::info("RenderTags: no font loaded for size=" + std::to_string(currentFontSize));
		return;
	}

	Gdiplus::Font* tagBoldFont = tagRegularFont;
	std::unique_ptr<Gdiplus::Font> tagBoldFontOwned;
	if (tagRegularFont != nullptr)
	{
		Gdiplus::FontFamily baseFamily;
		if (tagRegularFont->GetFamily(&baseFamily) == Gdiplus::Ok)
		{
			INT boldStyle = tagRegularFont->GetStyle() | Gdiplus::FontStyleBold;
			tagBoldFontOwned.reset(new Gdiplus::Font(&baseFamily, tagRegularFont->GetSize(), boldStyle, Gdiplus::UnitPixel));
			if (tagBoldFontOwned->GetLastStatus() == Gdiplus::Ok)
				tagBoldFont = tagBoldFontOwned.get();
		}
	}

	const Gdiplus::StringFormat tagStringFormat;
	RectF tagMeasureRect;
	graphics.MeasureString(L" ", wcslen(L" "), tagRegularFont, PointF(0, 0), &tagStringFormat, &tagMeasureRect);
	const int tagBlankWidth = static_cast<int>(tagMeasureRect.GetRight());
	tagMeasureRect = RectF(0, 0, 0, 0);
	graphics.MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
		tagRegularFont, PointF(0, 0), &tagStringFormat, &tagMeasureRect);
	int tagOneLineHeight = static_cast<int>(tagMeasureRect.GetBottom());
	if (tagBoldFont != nullptr && tagBoldFont != tagRegularFont)
	{
		RectF boldMeasure;
		graphics.MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
			tagBoldFont, PointF(0, 0), &tagStringFormat, &boldMeasure);
		tagOneLineHeight = max(tagOneLineHeight, static_cast<int>(boldMeasure.GetBottom()));
	}

	const VsmrScene::RadarScene* scene = GetCurrentRadarScene();
	if (scene == nullptr)
		return;
	for (const VsmrScene::Target& sceneTarget : scene->targets)
	{
		if (!sceneTarget.tagVisible || !sceneTarget.position.valid)
			continue;
		const std::string& rtCallsign = sceneTarget.callsign;
		CPosition targetPosition;
		targetPosition.m_Latitude = sceneTarget.position.latitude;
		targetPosition.m_Longitude = sceneTarget.position.longitude;
		const POINT acPosPix = ConvertCoordFromPositionToPixel(targetPosition);

		// Getting the tag center/offset

		POINT TagCenter;
		map<string, POINT>::iterator it = TagsOffsets.find(rtCallsign);
		if (it != TagsOffsets.end()) {
			TagCenter = { acPosPix.x + it->second.x, acPosPix.y + it->second.y };
		}
		else {
			// Use angle:

			if (TagAngles.find(rtCallsign) == TagAngles.end())
				TagAngles[rtCallsign] = 270.0f;

			int lenght = LeaderLineDefaultlenght;
			if (TagLeaderLineLength.find(rtCallsign) != TagLeaderLineLength.end())
				lenght = TagLeaderLineLength[rtCallsign];

			TagCenter.x = long(acPosPix.x + float(lenght * cos(DegToRad(TagAngles[rtCallsign]))));
			TagCenter.y = long(acPosPix.y + float(lenght * sin(DegToRad(TagAngles[rtCallsign]))));
		}

		TagTypes TagType = TagTypes::Departure;
		switch (sceneTarget.role)
		{
		case VsmrScene::TargetRole::Arrival:
			TagType = TagTypes::Arrival;
			break;
		case VsmrScene::TargetRole::AirborneArrival:
			TagType = TagTypes::Airborne;
			break;
		case VsmrScene::TargetRole::AirborneDeparture:
			TagType = TagTypes::Airborne;
			break;
		case VsmrScene::TargetRole::Uncorrelated:
			TagType = TagTypes::Uncorrelated;
			break;
		default:
			break;
		}


		verboseTargetStep(rtCallsign, "after_tag_data");

		verboseTargetStep(rtCallsign, "after_clickable_map");

		//
		// ----- Now the hard part, drawing (using gdi+) -------
		//

		// First we need to figure out the tag size
		int TagWidth = 0, TagHeight = 0;
		const int blankWidth = tagBlankWidth;
		const int oneLineHeight = tagOneLineHeight;
		RectF mesureRect;

		const TagHoverState hoverState = resolveTagHoverState(rtCallsign, TagCenter);
		bool isTagDetailled = hoverState.useDetailedLayout;
		verboseTargetStep(rtCallsign, std::string("detail_mode=") + (isTagDetailled ? "1" : "0"));

		const VsmrScene::TagPalette& tagPalette = isTagDetailled
			? sceneTarget.tag.detailedPalette
			: sceneTarget.tag.normalPalette;

		struct RenderedTagElement
		{
			std::string text;
			int action = TAG_CITEM_NO;
			bool bold = false;
			VsmrScene::Color effectiveColor;
			int measuredWidth = 0;
			int measuredHeight = 0;
		};
		vector<vector<RenderedTagElement>> ReplacedLabelLines;
		int CollisionTagWidth = 0;
		int CollisionTagHeight = 0;
		auto measureSceneTagLines = [&](const VsmrScene::TagVariant& variant, int& outTagWidth, int& outTagHeight, vector<vector<RenderedTagElement>>* renderedLines) -> bool
		{
			if (variant.lines.empty())
				return false;
			for (const VsmrScene::TagLine& sceneLine : variant.lines)
			{
				if (sceneLine.elements.empty())
					continue;
				vector<RenderedTagElement> renderedLine;
				renderedLine.reserve(sceneLine.elements.size());
				int lineWidth = 0;
				for (const VsmrScene::TagElement& sceneElement : sceneLine.elements)
				{
					RenderedTagElement renderedElement;
					renderedElement.text = sceneElement.text;
					renderedElement.action = sceneElement.action;
					renderedElement.bold = sceneElement.bold;
					renderedElement.effectiveColor = sceneElement.effectiveColor;
					if (!renderedElement.text.empty())
					{
						wstring text(renderedElement.text.begin(), renderedElement.text.end());
						mesureRect = RectF(0, 0, 0, 0);
						Gdiplus::Font* measureFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
						graphics.MeasureString(text.c_str(), static_cast<INT>(text.size()), measureFont, PointF(0, 0), &tagStringFormat, &mesureRect);
						renderedElement.measuredWidth = static_cast<int>(mesureRect.GetRight());
						renderedElement.measuredHeight = static_cast<int>(mesureRect.GetBottom());
					}
					lineWidth += renderedElement.measuredWidth;
					renderedLine.push_back(std::move(renderedElement));
				}
				if (renderedLine.empty())
					continue;
				lineWidth += blankWidth * (static_cast<int>(renderedLine.size()) - 1);
				outTagWidth = max(outTagWidth, lineWidth);
				outTagHeight += oneLineHeight;
				if (renderedLines != nullptr)
					renderedLines->push_back(std::move(renderedLine));
			}
			return outTagWidth > 0 && outTagHeight > 0;
		};

		const VsmrScene::TagVariant& displayedVariant = isTagDetailled
			? sceneTarget.tag.detailed
			: sceneTarget.tag.normal;
		if (!measureSceneTagLines(displayedVariant, TagWidth, TagHeight, &ReplacedLabelLines))
			continue;
		verboseTargetStep(rtCallsign, "after_tag_measurement");

		if (!measureSceneTagLines(sceneTarget.tag.normal, CollisionTagWidth, CollisionTagHeight, nullptr))
		{
			CollisionTagWidth = TagWidth;
			CollisionTagHeight = TagHeight;
		}
		if (CollisionTagWidth <= 0 || CollisionTagHeight <= 0)
		{
			CollisionTagWidth = TagWidth;
			CollisionTagHeight = TagHeight;
		}

		string alertStr;
		int alertTextWidth = 0;
		int alertTextHeight = 0;
		const CRimcas::RimcasAlertTypes rimcasStage =
			static_cast<CRimcas::RimcasAlertTypes>(sceneTarget.rimcas.alertStage);
		const CRimcas::RimcasAlerts alert =
			static_cast<CRimcas::RimcasAlerts>(sceneTarget.rimcas.movementAlert);
		if (CRimcas::NONE != alert && TagType == TagTypes::Departure)
		{
			switch (alert)
			{
			case CRimcas::RimcasAlerts::STATRPA:
				alertStr = "STAT RPA";
				break;
			case CRimcas::RimcasAlerts::NOPUSH:
				alertStr = "NO PUSH CLR";
				break;
			case CRimcas::RimcasAlerts::NOTKOF:
				alertStr = "NO TKOF CLR";
				break;
			case CRimcas::RimcasAlerts::NOTAXI:
				alertStr = "NO TAXI CLR";
				break;
			case CRimcas::RimcasAlerts::RWYINC:
				alertStr = "RWY INCURSION";
				break;
			case CRimcas::RimcasAlerts::RWYTYPE:
				alertStr = "RWY TYPE";
				break;
			case CRimcas::RimcasAlerts::RWYCLSD:
				alertStr = "RWY CLOSED";
				break;
			case CRimcas::RimcasAlerts::HIGHSPD:
				alertStr = "HIGH SPEED";
				break;
			case CRimcas::RimcasAlerts::EMERG:
				alertStr = "EMERG";
				break;
			default:
				break;
			}

			if (!alertStr.empty())
			{
				wstring wstr = wstring(alertStr.begin(), alertStr.end());
				graphics.MeasureString(wstr.c_str(), wcslen(wstr.c_str()),
					tagRegularFont, PointF(0, 0), &tagStringFormat, &mesureRect);

				alertTextWidth = static_cast<int>(mesureRect.GetRight());
				alertTextHeight = static_cast<int>(mesureRect.GetBottom());
				int TempTagWidth = alertTextWidth;
				TagWidth = max(TagWidth, TempTagWidth);
				TagHeight += oneLineHeight;
				CollisionTagWidth = max(CollisionTagWidth, TempTagWidth);
				CollisionTagHeight += oneLineHeight;
			}
		}

		const int tagRectLeft = TagCenter.x - (TagWidth / 2);
		const int tagRectTop = TagCenter.y - (TagHeight / 2);
		previousTagSize[rtCallsign] = CRect(tagRectLeft, tagRectTop, tagRectLeft + TagWidth, tagRectTop + TagHeight);

		const Color definedBackgroundColor = ToGdiColor(tagPalette.background);
		const Color definedBackgroundOnRunwayColor = ToGdiColor(tagPalette.backgroundOnRunway);
		verboseTargetStep(rtCallsign, "after_color_resolution");

		const Color TagBackgroundColor = sceneTarget.rimcas.onRunway
			? definedBackgroundOnRunwayColor
			: definedBackgroundColor;

		verboseTargetStep(rtCallsign, "after_background_color");

		// Drawing the tag background

		// Keep the tag hitbox compact while retaining a small text inset.
		const int padding = 1;
		const int tagBackgroundLeft = TagCenter.x - (TagWidth / 2) - padding;
		const int tagBackgroundTop = TagCenter.y - (TagHeight / 2) - padding;
		CRect TagBackgroundRect(tagBackgroundLeft, tagBackgroundTop, tagBackgroundLeft + TagWidth + (padding * 2), tagBackgroundTop + TagHeight + (padding * 2));
		const int tagCollisionLeft = TagCenter.x - (CollisionTagWidth / 2) - padding;
		const int tagCollisionTop = TagCenter.y - (CollisionTagHeight / 2) - padding;
		CRect TagCollisionRect(tagCollisionLeft, tagCollisionTop, tagCollisionLeft + CollisionTagWidth + (padding * 2), tagCollisionTop + CollisionTagHeight + (padding * 2));
		int textLeft = TagBackgroundRect.left + padding;
		int textTop = TagBackgroundRect.top + padding;
		auto MakeRoundedRect = [](GraphicsPath &path, Rect r, int radius) {
			path.Reset();
			int d = radius * 2;
			path.AddArc(r.X, r.Y, d, d, 180, 90);
			path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
			path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
			path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
			path.CloseFigure();
		};

		Rect RoundedRect = CopyRect(TagBackgroundRect);
		SolidBrush TagBackgroundBrush(TagBackgroundColor);
		const bool pointerInTagRect = isMouseWithinRect(TagBackgroundRect);
		GraphicsPath roundedPath;
		bool hasRoundedTagClipPath = false;
		if (roundedTagCornersEnabled)
		{
			MakeRoundedRect(roundedPath, RoundedRect, 4);
			hasRoundedTagClipPath = true;
			graphics.FillPath(&TagBackgroundBrush, &roundedPath);
			if (pointerInTagRect || hoverState.isDragged)
			{
				Pen pw(Color(static_cast<Gdiplus::ARGB>(Color::White)));
				graphics.DrawPath(&pw, &roundedPath);
			}
		}
		else
		{
			graphics.FillRectangle(&TagBackgroundBrush, RoundedRect);
			if (pointerInTagRect || hoverState.isDragged)
			{
				Pen pw(Color(static_cast<Gdiplus::ARGB>(Color::White)));
				graphics.DrawRectangle(&pw, RoundedRect);
			}
		}

		// Drawing the tag text

		auto getRimcasEditorColor = [&](const char* key, const Color& fallback) -> Color
		{
			if (activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
			{
				const Value& rimcas = activeProfile["rimcas"];
				if (rimcas.HasMember(key) && rimcas[key].IsObject())
					return CurrentConfig->getConfigColor(rimcas[key]);
			}
			return fallback;
		};
		SolidBrush AlertTextColorCaution(
			getRimcasEditorColor("caution_alert_text_color", Color(255, 30, 30, 30)));
		SolidBrush AlertTextColorWarning(
			getRimcasEditorColor("warning_alert_text_color", Color(255, 255, 255, 255)));
		SolidBrush AlertColorCaution(
			getRimcasEditorColor("caution_alert_background_color", Color(230, 255, 215, 0)));
		SolidBrush AlertColorWarning(
			getRimcasEditorColor("warning_alert_background_color", Color(230, 200, 40, 40)));
		const Color leaderLineColor(static_cast<Gdiplus::ARGB>(Color::White));


		// Drawing the leader line
		RECT TagBackRectData = TagBackgroundRect;
		POINT toDraw1, toDraw2;
		if (LiangBarsky(TagBackRectData, acPosPix, TagBackgroundRect.CenterPoint(), toDraw1, toDraw2))
		{
			Pen leaderLinePen(leaderLineColor);
			graphics.DrawLine(&leaderLinePen, PointF(Gdiplus::REAL(acPosPix.x), Gdiplus::REAL(acPosPix.y)), PointF(Gdiplus::REAL(toDraw1.x), Gdiplus::REAL(toDraw1.y)));
		}

		// If we use a RIMCAS label only, we display it, and adapt the rectangle
		CRect oldCrectSave = TagBackgroundRect;
		const std::string& tagBottomLine = sceneTarget.bottomLine;
		const char* tagBottomLineText = tagBottomLine.c_str();

		// Adding the tag screen object
		tagAreas[rtCallsign] = TagBackgroundRect;
		tagCollisionAreas[rtCallsign] = TagCollisionRect;
		AddScreenObject(DRAWING_TAG, rtCallsign.c_str(), TagBackgroundRect, true, tagBottomLineText);

		TagBackgroundRect = oldCrectSave;

		// Clickable zones
		int heightOffset = 0;
		// Drawing Alert
		if (alert != CRimcas::NONE &&
			TagType == TagTypes::Departure &&
			!alertStr.empty())
		{
			wstring welement = wstring(alertStr.begin(), alertStr.end());
			const int alertLineHeight = max(alertTextHeight, oneLineHeight);
			const int alertTop = TagBackgroundRect.top + heightOffset;
			const int alertBottom = TagBackgroundRect.top + padding + heightOffset + alertLineHeight;
			CRect ItemRect(TagBackgroundRect.left,
				alertTop,
				TagBackgroundRect.right,
				alertBottom);
			const CRimcas::RimcasAlertSeverity severity =
				static_cast<CRimcas::RimcasAlertSeverity>(sceneTarget.rimcas.severity);
			SolidBrush* AlertColor = (severity == CRimcas::RimcasAlertSeverity::WARNING) ? &AlertColorWarning : &AlertColorCaution;
			SolidBrush* RimcasTextColor = (severity == CRimcas::RimcasAlertSeverity::WARNING) ? &AlertTextColorWarning : &AlertTextColorCaution;
			if (roundedTagCornersEnabled && hasRoundedTagClipPath)
			{
				graphics.SetClip(&roundedPath, CombineModeReplace);
			}
			graphics.FillRectangle(AlertColor, CopyRect(ItemRect));
			if (roundedTagCornersEnabled && hasRoundedTagClipPath)
			{
				graphics.ResetClip();
			}

			wstring walertStr = wstring(alertStr.begin(), alertStr.end());
			const int alertTextOffsetY = max(0, (alertLineHeight - alertTextHeight + 1) / 2);
			graphics.DrawString(walertStr.c_str(), wcslen(walertStr.c_str()), tagRegularFont,
				PointF(Gdiplus::REAL(textLeft), Gdiplus::REAL(textTop + heightOffset + alertTextOffsetY)),
				&tagStringFormat, RimcasTextColor);

			CRect alertRect(TagBackgroundRect.left, TagBackgroundRect.top + heightOffset,
				TagBackgroundRect.left + alertTextWidth, TagBackgroundRect.top + heightOffset + max(alertTextHeight, oneLineHeight));

			AddScreenObject(TAG_CITEM_NO, rtCallsign.c_str(), alertRect, true, tagBottomLineText);
			heightOffset += oneLineHeight;
		}
		for (auto&& line : ReplacedLabelLines)
		{

			int widthOffset = 0;
			for (auto&& renderedElement : line)
			{
				const std::string& element = renderedElement.text;
				Gdiplus::Font* drawFont = renderedElement.bold ? tagBoldFont : tagRegularFont;

				SolidBrush elementColor(ToGdiColor(renderedElement.effectiveColor));

				wstring welement = wstring(element.begin(), element.end());
				const int textOffsetY = max(0, (oneLineHeight - renderedElement.measuredHeight + 1) / 2);
				graphics.DrawString(welement.c_str(), wcslen(welement.c_str()), drawFont,
					PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset + textOffsetY)),
					&tagStringFormat, &elementColor);

				int clickItemType = renderedElement.action;

				int itemWidth = renderedElement.measuredWidth;
				if (clickItemType == TAG_CITEM_SCRATCHPAD) {
					// Extend scratchpad hit area to the tag border without growing the drawn tag.
					int rightBound = TagBackgroundRect.right - padding;
					int extendedWidth = rightBound - (textLeft + widthOffset);
					if (extendedWidth > itemWidth)
						itemWidth = extendedWidth;
				}
				int itemHeight = max(renderedElement.measuredHeight, oneLineHeight);

				CRect ItemRect(textLeft + widthOffset, textTop + heightOffset,
					textLeft + widthOffset + itemWidth, textTop + heightOffset + itemHeight);

				AddScreenObject(clickItemType, rtCallsign.c_str(), ItemRect, true, tagBottomLineText);

				widthOffset += renderedElement.measuredWidth;
				widthOffset += blankWidth;
			}

			heightOffset += oneLineHeight;
		}


	}

}
