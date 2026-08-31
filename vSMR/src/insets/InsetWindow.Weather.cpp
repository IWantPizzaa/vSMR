#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "radar/RadarScreen.hpp"
#include "weather/WeatherStore.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>

HFONT CInsetWindow::GetWeatherFont(
	size_t index,
	int height,
	int weight,
	DWORD pitchAndFamily,
	const char* faceName)
{
	if (index >= m_WeatherFonts.size())
		return nullptr;

	if (m_WeatherFonts[index] != nullptr && m_WeatherFontHeights[index] == height)
		return m_WeatherFonts[index];

	if (m_WeatherFonts[index] != nullptr)
	{
		::DeleteObject(m_WeatherFonts[index]);
		m_WeatherFonts[index] = nullptr;
	}

	HFONT font = ::CreateFontA(
		height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, pitchAndFamily, faceName);
	if (font != nullptr)
	{
		m_WeatherFonts[index] = font;
		m_WeatherFontHeights[index] = height;
	}
	return font;
}

void CInsetWindow::renderWeather(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	if (radar_screen == nullptr || gdi == nullptr || radar_screen->IsShutdownRequested())
		return;

	CDC dc;
	dc.Attach(hDC);
	CRect layoutBounds(radar_screen->GetRadarArea());
	CRect chatArea(radar_screen->GetChatArea());
	layoutBounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		layoutBounds.bottom = chatArea.top;
	ApplyAvisoLayoutBounds(&layoutBounds);

	CRect content = GetWindowContentRect();
	content.NormalizeRect();
	if (content.Width() <= 0 || content.Height() <= 0)
	{
		dc.Detach();
		return;
	}

	HWND renderWindow = ::WindowFromDC(hDC);
	if (renderWindow == nullptr || !::IsWindow(renderWindow))
		renderWindow = ::GetActiveWindow();
	UpdateAvisoScreenArea(renderWindow);

	const COLORREF background = RGB(43, 52, 55);
	const COLORREF panel = RGB(41, 54, 57);
	const COLORREF panelHeader = RGB(33, 43, 46);
	const COLORREF panelSoft = RGB(47, 62, 66);
	const COLORREF control = RGB(36, 48, 51);
	const COLORREF outerBorder = RGB(5, 7, 8);
	const COLORREF innerBorder = RGB(17, 23, 25);
	const COLORREF text = RGB(201, 214, 219);
	const COLORREF mutedText = RGB(143, 160, 165);
	const COLORREF cyan = RGB(92, 180, 211);

	dc.FillSolidRect(content, background);
	radar_screen->AddScreenObject(m_Id, "window", content, false, "");

	const int savedDc = ::SaveDC(hDC);
	if (savedDc != 0)
		::IntersectClipRect(hDC, content.left, content.top, content.right, content.bottom);
	const Gdiplus::GraphicsState graphicsState = gdi->Save();
	gdi->SetClip(CopyRect(content), Gdiplus::CombineModeIntersect);
	gdi->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	gdi->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

	const double weatherScale = std::clamp(
		min(static_cast<double>(content.Width()) / 306.0,
			static_cast<double>(content.Height()) / 175.0),
		0.75,
		3.0);
	auto scaledFontHeight = [weatherScale](int pixels) -> int
	{
		return -max(1, static_cast<int>(std::lround(static_cast<double>(pixels) * weatherScale)));
	};
	HFONT labelFont = GetWeatherFont(0, scaledFontHeight(9), FW_BOLD, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT valueFont = GetWeatherFont(1, scaledFontHeight(13), FW_BOLD, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT directionFont = GetWeatherFont(2, scaledFontHeight(22), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT speedFont = GetWeatherFont(3, scaledFontHeight(16), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT gustFont = GetWeatherFont(4, scaledFontHeight(12), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT qnhFont = GetWeatherFont(5, scaledFontHeight(19), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT compassFont = GetWeatherFont(6, scaledFontHeight(8), FW_NORMAL, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	const double rawTextScale = std::clamp(weatherScale, 0.95, 1.65);
	HFONT rawFont = nullptr;
	HFONT rawBoldFont = nullptr;
	HGDIOBJ originalFont = ::GetCurrentObject(hDC, OBJ_FONT);

	auto drawText = [&](const CRect& source, const std::string& value, HFONT font, COLORREF color, UINT flags)
	{
		CRect rect(source);
		if (font != nullptr)
			::SelectObject(hDC, font);
		::SetBkMode(hDC, TRANSPARENT);
		::SetTextColor(hDC, color);
		::DrawTextA(hDC, value.c_str(), -1, &rect, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
	};
	auto fillAndBorder = [&](const CRect& rect, COLORREF fill)
	{
		dc.FillSolidRect(rect, fill);
		dc.Draw3dRect(rect, innerBorder, outerBorder);
	};
	auto formatDegrees = [](int degrees) -> std::string
	{
		char buffer[12] = {};
		std::snprintf(buffer, sizeof(buffer), "%03d\xB0", std::clamp(degrees, 0, 360));
		return buffer;
	};
	auto formatVariation = [](int from, int to) -> std::string
	{
		char buffer[20] = {};
		std::snprintf(buffer, sizeof(buffer), "%03d-%03d", std::clamp(from, 0, 360), std::clamp(to, 0, 360));
		return buffer;
	};
	auto windFlowDirection = [](int meteorologicalDirection) -> int
	{
		return (meteorologicalDirection + 180) % 360;
	};
	auto formatObservationTime = [](std::time_t value) -> std::string
	{
		if (value <= 0)
			return "----Z";
		std::tm utc = {};
		if (::gmtime_s(&utc, &value) != 0)
			return "----Z";
		char buffer[12] = {};
		std::snprintf(buffer, sizeof(buffer), "%02d%02dZ", utc.tm_hour, utc.tm_min);
		return buffer;
	};

	// ----- Resolving weather and runway components -----
	const std::string station = VsmrWeather::NormalizeIcao(radar_screen->getActiveAirport());
	VsmrWeather::Snapshot weather;
	const bool hasSnapshot = !station.empty() && VsmrWeather::TryGet(station, weather);
	const bool hasWeather = hasSnapshot && (weather.hasWind || weather.hasQnh);
	const std::time_t now = std::time(nullptr);
	const bool stale = hasWeather && weather.updatedUtc > 0 && now > weather.updatedUtc &&
		(now - weather.updatedUtc) > (90 * 60);
	const VsmrScene::RadarScene* weatherScene = radar_screen->GetCurrentRadarScene();

	struct RunwayReference
	{
		bool valid = false;
		std::string name;
		double trueHeading = 0.0;
		int priority = 3;
		double headwindScore = -1000000.0;
	};
	RunwayReference referenceRunway;
	// Preferring active departures, then the strongest headwind among equals
	const auto considerRunway = [&](const std::string& runwayName, double trueHeading, bool headingValid)
	{
		if (!headingValid || runwayName.empty() || weatherScene == nullptr)
			return;
		const auto statusIt = weatherScene->airport.runwayStatuses.find(runwayName);
		if (statusIt == weatherScene->airport.runwayStatuses.end())
			return;
		const CRimcas::RunwayStatus status = static_cast<CRimcas::RunwayStatus>(statusIt->second);
		const int priority = (status == CRimcas::DEP || status == CRimcas::BOTH)
			? 0
			: (status == CRimcas::ARR ? 1 : 3);
		if (priority >= 3)
			return;

		const double headwindScore = weather.hasWind && !weather.windVariable
			? static_cast<double>(weather.windSpeedKnots) *
				std::cos(DegToRad(static_cast<double>(weather.windDirectionDegrees) - trueHeading))
			: 0.0;
		if (!referenceRunway.valid || priority < referenceRunway.priority ||
			(priority == referenceRunway.priority && headwindScore > referenceRunway.headwindScore))
		{
			referenceRunway.valid = true;
			referenceRunway.name = runwayName;
			referenceRunway.trueHeading = trueHeading;
			referenceRunway.priority = priority;
			referenceRunway.headwindScore = headwindScore;
		}
	};
	for (const auto& runway : radar_screen->CachedRunwayGeometries)
	{
		considerRunway(runway.runwayNameA, runway.trueHeadingA, runway.trueHeadingAValid);
		considerRunway(runway.runwayNameB, runway.trueHeadingB, runway.trueHeadingBValid);
	}

	std::string qnhValue = weather.hasQnh ? std::to_string(weather.qnhHpa) : "----";
	std::string variationValue = "---";
	if (weather.hasWindVariation)
		variationValue = formatVariation(weather.windVariationFromDegrees, weather.windVariationToDegrees);
	else if (weather.hasWind && weather.windVariable)
		variationValue = "VRB";

	std::string headValue = "---";
	std::string crossValue = "---";
	if (referenceRunway.valid && weather.hasWind && !weather.windVariable)
	{
		const double delta = DegToRad(
			static_cast<double>(weather.windDirectionDegrees) - referenceRunway.trueHeading);
		const int head = static_cast<int>(std::lround(static_cast<double>(weather.windSpeedKnots) * std::cos(delta)));
		const int cross = static_cast<int>(std::lround(static_cast<double>(weather.windSpeedKnots) * std::sin(delta)));
		headValue = std::string(head >= 0 ? "H" : "T") + std::to_string(std::abs(head));
		crossValue = cross == 0
			? "0"
			: std::to_string(std::abs(cross)) + (cross > 0 ? "R" : "L");
	}

	const int peakWind = weather.hasWindGust
		? max(weather.windSpeedKnots, weather.windGustKnots)
		: weather.windSpeedKnots;
	const COLORREF windColor = peakWind < 10
		? RGB(94, 193, 137)
		: (peakWind < 20
			? RGB(224, 189, 71)
			: (peakWind < 30 ? RGB(230, 135, 55) : RGB(235, 90, 90)));
	std::string directionValue = hasSnapshot ? "NO WIND" : "WAIT";
	std::string speedValue;
	std::string gustValue;
	if (weather.hasWind)
	{
		directionValue = weather.windCalm
			? "CALM"
			: (weather.windVariable ? "VRB" : formatDegrees(weather.windDirectionDegrees));
		speedValue = std::to_string(weather.windSpeedKnots) + " KT";
		if (weather.hasWindGust)
			gustValue = "G" + std::to_string(weather.windGustKnots);
	}

	// ----- Arranging the responsive panels -----
	const int padding = 4;
	const int gap = 4;
	CRect inner(content);
	inner.DeflateRect(padding, padding);
	const bool hasRawReport = hasSnapshot && !weather.rawReport.empty();
	int rawHeight = 0;
	if (hasRawReport && inner.Height() >= 58)
	{
		const int minimumRawHeight = max(34, static_cast<int>(std::lround(42.0 * rawTextScale)));
		const int maximumRawHeight = max(minimumRawHeight, static_cast<int>(std::lround(92.0 * rawTextScale)));
		rawHeight = std::clamp(
			static_cast<int>(std::lround(static_cast<double>(inner.Height()) * 0.42)),
			minimumRawHeight,
			min(maximumRawHeight, max(minimumRawHeight, inner.Height() - 38)));
	}
	CRect rawPanel;
	CRect primary(inner);
	if (rawHeight > 0)
	{
		rawPanel = CRect(inner.left, inner.bottom - rawHeight, inner.right, inner.bottom);
		primary.bottom = max(primary.top, rawPanel.top - gap);

		const int availableRawWidth = max(1, rawPanel.Width() - 8);
		const int availableRawHeight = max(1, rawPanel.Height() - 6);
		const int maximumRawPixels = std::clamp(
			static_cast<int>(std::lround(19.0 * rawTextScale)),
			12,
			28);
		int selectedRawPixels = 12;
		for (int candidatePixels = maximumRawPixels; candidatePixels >= 12; --candidatePixels)
		{
			const int estimatedCharacterWidth = max(1, static_cast<int>(std::lround(candidatePixels * 0.61)));
			const int estimatedSpaceWidth = estimatedCharacterWidth;
			const int candidateLineHeight = max(candidatePixels + 4, static_cast<int>(std::lround(candidatePixels * 1.42)));
			int estimatedLines = 1;
			int lineWidth = 0;
			std::istringstream report(weather.rawReport);
			std::string reportToken;
			while (report >> reportToken)
			{
				const int tokenWidth = max(estimatedCharacterWidth, static_cast<int>(reportToken.size()) * estimatedCharacterWidth);
				const int requiredWidth = lineWidth == 0 ? tokenWidth : estimatedSpaceWidth + tokenWidth;
				if (lineWidth > 0 && lineWidth + requiredWidth > availableRawWidth)
				{
					++estimatedLines;
					lineWidth = tokenWidth;
				}
				else
				{
					lineWidth += requiredWidth;
				}
			}
			if (estimatedLines * candidateLineHeight <= availableRawHeight)
			{
				selectedRawPixels = candidatePixels;
				break;
			}
		}
		rawFont = GetWeatherFont(7, -selectedRawPixels, FW_NORMAL, FIXED_PITCH | FF_MODERN, "Consolas");
		rawBoldFont = GetWeatherFont(8, -selectedRawPixels, FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	}
	const int primaryWidth = max(1, primary.Width());
	const int primaryHeight = max(1, primary.Height());
	const bool compactLayout = primaryWidth < 165 || primaryHeight < 78;
	const bool stackedLayout = !compactLayout && primaryWidth < 230 && primaryHeight >= 142;
	CRect windPanel;
	CRect statsPanel;
	if (compactLayout)
	{
		windPanel = primary;
	}
	else if (stackedLayout)
	{
		const int availableHeight = max(1, primaryHeight - gap);
		const int maximumWindHeight = max(1, availableHeight - 52);
		const int minimumWindHeight = min(82, maximumWindHeight);
		const int windHeight = std::clamp(
			min(primaryWidth, maximumWindHeight),
			minimumWindHeight,
			maximumWindHeight);
		windPanel = CRect(primary.left, primary.top, primary.right, primary.top + windHeight);
		statsPanel = CRect(primary.left, windPanel.bottom + gap, primary.right, primary.bottom);
	}
	else
	{
		const int minimumStatsWidth = 104;
		const int availableWidth = max(1, primaryWidth - gap);
		const int maximumWindWidth = max(1, availableWidth - minimumStatsWidth);
		const int minimumWindWidth = min(116, maximumWindWidth);
		const int windWidth = std::clamp(
			static_cast<int>(std::lround(static_cast<double>(availableWidth) * 0.55)),
			minimumWindWidth,
			maximumWindWidth);
		windPanel = CRect(primary.left, primary.top, primary.left + windWidth, primary.bottom);
		statsPanel = CRect(windPanel.right + gap, primary.top, primary.right, primary.bottom);
	}
	fillAndBorder(windPanel, panel);
	if (!statsPanel.IsRectEmpty())
		fillAndBorder(statsPanel, panel);
	if (!rawPanel.IsRectEmpty())
		fillAndBorder(rawPanel, control);

	// ----- Drawing the wind display -----
	if (!compactLayout)
	{
	CRect roseBounds(
		windPanel.left + 4,
		windPanel.top + 4,
		windPanel.right - 4,
		windPanel.bottom - 4);
	const int roseDiameter = max(20, min(roseBounds.Width(), roseBounds.Height()) - 4);
	const float radius = static_cast<float>(roseDiameter) * 0.5f;
	const float centerX = static_cast<float>(roseBounds.left + roseBounds.Width() / 2);
	const float centerY = static_cast<float>(roseBounds.top + roseBounds.Height() / 2);
	const Gdiplus::RectF roseRect(centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f);
	Gdiplus::SolidBrush roseBrush(Gdiplus::Color(255, 36, 48, 51));
	Gdiplus::Pen roseBorder(Gdiplus::Color(255, 17, 23, 25), 1.0f);
	gdi->FillEllipse(&roseBrush, roseRect);
	gdi->DrawEllipse(&roseBorder, roseRect);

	const double pi = 3.14159265358979323846;
	const Gdiplus::Color variationColor(230, 92, 180, 211);
	const float variationOuterRadius = max(2.0f, radius - 3.0f);
	const float variationBandWidth = std::clamp(static_cast<float>(5.0 * weatherScale), 4.0f, 9.0f);
	const float variationInnerRadius = max(1.0f, variationOuterRadius - variationBandWidth);
	const Gdiplus::RectF variationOuterRect(
		centerX - variationOuterRadius,
		centerY - variationOuterRadius,
		variationOuterRadius * 2.0f,
		variationOuterRadius * 2.0f);
	const Gdiplus::RectF variationInnerRect(
		centerX - variationInnerRadius,
		centerY - variationInnerRadius,
		variationInnerRadius * 2.0f,
		variationInnerRadius * 2.0f);
	if (weather.hasWindVariation)
	{
		int sweep = weather.windVariationToDegrees - weather.windVariationFromDegrees;
		if (sweep < 0)
			sweep += 360;
		if (sweep > 0)
		{
			const Gdiplus::REAL startAngle = static_cast<Gdiplus::REAL>(
				windFlowDirection(weather.windVariationFromDegrees) - 90);
			Gdiplus::GraphicsPath variationBand;
			variationBand.AddArc(variationOuterRect, startAngle, static_cast<Gdiplus::REAL>(sweep));
			variationBand.AddArc(
				variationInnerRect,
				startAngle + static_cast<Gdiplus::REAL>(sweep),
				-static_cast<Gdiplus::REAL>(sweep));
			variationBand.CloseFigure();

			Gdiplus::SolidBrush variationFill(Gdiplus::Color(72, 92, 180, 211));
			Gdiplus::Pen variationOutline(Gdiplus::Color(170, 92, 180, 211), 1.0f);
			variationOutline.SetLineJoin(Gdiplus::LineJoinRound);
			gdi->FillPath(&variationFill, &variationBand);
			gdi->DrawPath(&variationOutline, &variationBand);

			Gdiplus::Pen variationEdge(variationColor, 2.0f);
			variationEdge.SetStartCap(Gdiplus::LineCapRound);
			variationEdge.SetEndCap(Gdiplus::LineCapRound);
			gdi->DrawArc(
				&variationEdge,
				variationOuterRect,
				startAngle,
				static_cast<Gdiplus::REAL>(sweep));

			Gdiplus::SolidBrush endpointBrush(variationColor);
			const float endpointRadius = (variationOuterRadius + variationInnerRadius) * 0.5f;
			const float endpointDotRadius = std::clamp(static_cast<float>(1.7 * weatherScale), 1.5f, 3.0f);
			for (int endpoint : { weather.windVariationFromDegrees, weather.windVariationToDegrees })
			{
				const double endpointAngle = (static_cast<double>(windFlowDirection(endpoint)) - 90.0) * pi / 180.0;
				const Gdiplus::PointF endpointPoint(
					centerX + static_cast<float>(std::cos(endpointAngle) * endpointRadius),
					centerY + static_cast<float>(std::sin(endpointAngle) * endpointRadius));
				gdi->FillEllipse(
					&endpointBrush,
					endpointPoint.X - endpointDotRadius,
					endpointPoint.Y - endpointDotRadius,
					endpointDotRadius * 2.0f,
					endpointDotRadius * 2.0f);
			}
		}
	}
	else if (weather.hasWind && weather.windVariable && !weather.windCalm)
	{
		Gdiplus::GraphicsPath variableBand(Gdiplus::FillModeAlternate);
		variableBand.AddEllipse(variationOuterRect);
		variableBand.AddEllipse(variationInnerRect);
		Gdiplus::SolidBrush variableFill(Gdiplus::Color(45, 92, 180, 211));
		gdi->FillPath(&variableFill, &variableBand);

		Gdiplus::Pen variableEdge(variationColor, 1.8f);
		variableEdge.SetDashStyle(Gdiplus::DashStyleDash);
		variableEdge.SetDashCap(Gdiplus::DashCapRound);
		gdi->DrawEllipse(&variableEdge, variationOuterRect);
	}

	Gdiplus::Pen minorTick(Gdiplus::Color(255, 92, 108, 113), 1.0f);
	Gdiplus::Pen majorTick(Gdiplus::Color(255, 151, 169, 174), 1.0f);
	for (int degrees = 0; degrees < 360; degrees += 10)
	{
		const double angle = (static_cast<double>(degrees) - 90.0) * pi / 180.0;
		const float tickLength = degrees % 30 == 0 ? 5.0f : 2.5f;
		const Gdiplus::PointF outer(
			centerX + static_cast<float>(std::cos(angle) * (radius - 2.0f)),
			centerY + static_cast<float>(std::sin(angle) * (radius - 2.0f)));
		const Gdiplus::PointF innerTick(
			centerX + static_cast<float>(std::cos(angle) * (radius - 2.0f - tickLength)),
			centerY + static_cast<float>(std::sin(angle) * (radius - 2.0f - tickLength)));
		gdi->DrawLine(degrees % 30 == 0 ? &majorTick : &minorTick, outer, innerTick);

		if (degrees % 30 == 0 && radius >= 46.0f)
		{
			const float labelRadius = radius - static_cast<float>(11.0 * weatherScale);
			const int labelX = static_cast<int>(std::lround(centerX + std::cos(angle) * labelRadius));
			const int labelY = static_cast<int>(std::lround(centerY + std::sin(angle) * labelRadius));
			const int labelHalfWidth = max(10, static_cast<int>(std::lround(10.0 * weatherScale)));
			const int labelHalfHeight = max(5, static_cast<int>(std::lround(6.0 * weatherScale)));
			CRect labelArea(
				labelX - labelHalfWidth,
				labelY - labelHalfHeight,
				labelX + labelHalfWidth,
				labelY + labelHalfHeight);
			drawText(labelArea, degrees == 0 ? "36" : std::to_string(degrees / 10), compassFont, mutedText, DT_CENTER);
		}
	}

	const float centerTextHalfWidth = min(
		max(10.0f, radius * 0.68f),
		static_cast<float>(42.0 * weatherScale));
	const float centerTextHalfHeight = min(
		max(10.0f, radius * 0.62f),
		static_cast<float>(29.0 * weatherScale));
	if (weather.hasWind && !weather.windVariable && !weather.windCalm)
	{
		const double angle = (static_cast<double>(windFlowDirection(weather.windDirectionDegrees)) - 90.0) * pi / 180.0;
		const float directionX = static_cast<float>(std::cos(angle));
		const float directionY = static_cast<float>(std::sin(angle));
		const float horizontalBoundary = std::abs(directionX) > 0.001f
			? centerTextHalfWidth / std::abs(directionX)
			: radius;
		const float verticalBoundary = std::abs(directionY) > 0.001f
			? centerTextHalfHeight / std::abs(directionY)
			: radius;
		const float startRadius = min(horizontalBoundary, verticalBoundary) + max(1.5f, radius * 0.035f);
		const float lineRadius = max(startRadius + 2.0f, radius - max(4.0f, radius * 0.12f));
		const Gdiplus::PointF start(
			centerX + directionX * startRadius,
			centerY + directionY * startRadius);
		const Gdiplus::PointF tip(
			centerX + directionX * lineRadius,
			centerY + directionY * lineRadius);
		const Gdiplus::Color needleColor(255, GetRValue(windColor), GetGValue(windColor), GetBValue(windColor));
		const float needleWidth = std::clamp(radius * 0.052f, 1.8f, 5.5f);
		const float arrowLength = std::clamp(radius * 0.20f, 7.0f, 19.0f);
		const float arrowHalfWidth = std::clamp(radius * 0.105f, 3.8f, 10.0f);
		const Gdiplus::PointF arrowBase(
			tip.X - directionX * arrowLength,
			tip.Y - directionY * arrowLength);
		const Gdiplus::PointF perpendicular(-directionY, directionX);
		const float shaftEndRadius = max(
			startRadius,
			lineRadius - arrowLength + needleWidth * 0.4f);
		const Gdiplus::PointF shaftEnd(
			centerX + directionX * shaftEndRadius,
			centerY + directionY * shaftEndRadius);
		Gdiplus::Pen needlePen(needleColor, needleWidth);
		needlePen.SetStartCap(Gdiplus::LineCapRound);
		needlePen.SetEndCap(Gdiplus::LineCapRound);
		gdi->DrawLine(&needlePen, start, shaftEnd);
		Gdiplus::PointF arrow[] = {
			tip,
			Gdiplus::PointF(
				arrowBase.X + perpendicular.X * arrowHalfWidth,
				arrowBase.Y + perpendicular.Y * arrowHalfWidth),
			Gdiplus::PointF(
				arrowBase.X - perpendicular.X * arrowHalfWidth,
				arrowBase.Y - perpendicular.Y * arrowHalfWidth)
		};
		Gdiplus::SolidBrush arrowBrush(needleColor);
		gdi->FillPolygon(&arrowBrush, arrow, static_cast<INT>(_countof(arrow)));
	}

	// Keep the center values readable when a compact compass leaves too little
	// room for the full arrowhead outside the text area.
	Gdiplus::SolidBrush centerPlate(Gdiplus::Color(255, 36, 48, 51));
	gdi->FillRectangle(
		&centerPlate,
		centerX - centerTextHalfWidth,
		centerY - centerTextHalfHeight,
		centerTextHalfWidth * 2.0f,
		centerTextHalfHeight * 2.0f);

	CRect directionArea(
		static_cast<int>(centerX - centerTextHalfWidth),
		static_cast<int>(centerY - 25.0 * weatherScale),
		static_cast<int>(centerX + centerTextHalfWidth),
		static_cast<int>(centerY - 3.0 * weatherScale));
	CRect speedArea(
		directionArea.left,
		directionArea.bottom,
		directionArea.right,
		static_cast<int>(centerY + 16.0 * weatherScale));
	CRect gustArea(
		directionArea.left,
		speedArea.bottom - 1,
		directionArea.right,
		static_cast<int>(centerY + 29.0 * weatherScale));
	drawText(directionArea, directionValue, radius >= 35.0f ? directionFont : valueFont, weather.hasWind ? windColor : mutedText, DT_CENTER);
	if (radius >= 35.0f)
		drawText(speedArea, speedValue, speedFont, text, DT_CENTER);
	if (radius >= 45.0f)
		drawText(gustArea, gustValue, gustFont, RGB(245, 214, 122), DT_CENTER);

	std::string footer;
	if (!hasWeather)
	{
		footer = hasSnapshot ? "NO CURRENT WEATHER" : "WAITING FOR METAR";
	}
	else if (stale)
	{
		footer = "STALE  " + formatObservationTime(weather.observationUtc);
	}
	if (!footer.empty())
	{
		CRect footerArea(
			windPanel.left + 4,
			windPanel.bottom - max(14, static_cast<int>(16.0 * weatherScale)),
			windPanel.right - 4,
			windPanel.bottom - 2);
		drawText(footerArea, footer, labelFont, stale ? RGB(230, 135, 55) : mutedText, DT_CENTER);
	}
	}
	else
	{
		CRect compactTop(windPanel);
		compactTop.DeflateRect(5, 3);
		const int split = compactTop.top + max(18, compactTop.Height() / 2);
		CRect windLine(compactTop.left, compactTop.top, compactTop.right, min(compactTop.bottom, split));
		CRect detailLine(compactTop.left, windLine.bottom, compactTop.right, compactTop.bottom);
		std::string windSummary = directionValue;
		if (!speedValue.empty())
			windSummary += "  " + speedValue;
		if (!gustValue.empty())
			windSummary += " " + gustValue;
		drawText(windLine, windSummary, valueFont, weather.hasWind ? windColor : mutedText, DT_LEFT);
		std::string detailSummary = "QNH " + qnhValue;
		if (weather.hasWindVariation)
			detailSummary += "  VAR " + variationValue;
		if (referenceRunway.valid)
			detailSummary += "  " + headValue + "/" + crossValue;
		drawText(detailLine, detailSummary, labelFont, text, DT_LEFT);
	}

	// Drawing QNH, variation and runway components
	const struct StatCell
	{
		const char* label;
		std::string value;
	} cells[] = {
		{ "QNH", qnhValue },
		{ "VAR", variationValue },
		{ "HEAD", headValue },
		{ "XWIND", crossValue }
	};
	if (!statsPanel.IsRectEmpty())
	{
		const int statsWidth = max(1, statsPanel.Width());
		const int statsHeight = max(1, statsPanel.Height());
		const bool priorityRows = statsWidth < static_cast<int>(std::lround(160.0 * weatherScale));
		if (priorityRows)
		{
			struct PriorityRow
			{
				std::string label;
				std::string value;
				HFONT font = nullptr;
			};
			const std::string runwayLabel = referenceRunway.valid
				? "RWY" + referenceRunway.name
				: "COMP";
			const std::string componentValue = referenceRunway.valid
				? headValue + " / X" + crossValue
				: "---";
			const PriorityRow rows[] = {
				{ "QNH", qnhValue, qnhFont },
				{ "VAR", variationValue, valueFont },
				{ runwayLabel, componentValue, valueFont }
			};
			const int rowWeights[] = { 40, 30, 30 };
			int rowTop = statsPanel.top;
			int accumulatedWeight = 0;
			for (int row = 0; row < 3; ++row)
			{
				accumulatedWeight += rowWeights[row];
				const int rowBottom = row == 2
					? statsPanel.bottom
					: statsPanel.top + (statsHeight * accumulatedWeight) / 100;
				CRect cell(statsPanel.left, rowTop, statsPanel.right, rowBottom);
				dc.FillSolidRect(cell, panelSoft);
				dc.Draw3dRect(cell, innerBorder, outerBorder);
				const int labelWidth = std::clamp(
					static_cast<int>(std::lround(static_cast<double>(statsWidth) * 0.37)),
					38,
					max(38, statsWidth - 42));
				CRect labelArea(cell.left + 1, cell.top + 1, min(cell.right - 1, cell.left + labelWidth), cell.bottom - 1);
				CRect valueArea(labelArea.right, cell.top + 1, cell.right - 2, cell.bottom - 1);
				dc.FillSolidRect(labelArea, panelHeader);
				labelArea.left += 3;
				drawText(labelArea, rows[row].label, labelFont, mutedText, DT_LEFT);
				drawText(valueArea, rows[row].value, rows[row].font, text, DT_CENTER);
				rowTop = rowBottom;
			}
		}
		else
		{
			const int firstColumnWidth = statsWidth / 2;
			for (int row = 0; row < 2; ++row)
			{
				const int rowTop = statsPanel.top + (statsHeight * row) / 2;
				const int rowBottom = statsPanel.top + (statsHeight * (row + 1)) / 2;
				for (int column = 0; column < 2; ++column)
				{
					const int index = row * 2 + column;
					const int left = column == 0 ? statsPanel.left : statsPanel.left + firstColumnWidth;
					const int right = column == 0 ? statsPanel.left + firstColumnWidth : statsPanel.right;
					CRect cell(left, rowTop, right, rowBottom);
					dc.FillSolidRect(cell, panelSoft);
					dc.Draw3dRect(cell, innerBorder, outerBorder);
					const int desiredHeaderHeight = max(
						11,
						static_cast<int>(std::lround(15.0 * weatherScale)));
					const int headerHeight = min(desiredHeaderHeight, max(10, cell.Height() / 3));
					CRect header(cell.left + 1, cell.top + 1, cell.right - 1, min(cell.bottom - 1, cell.top + headerHeight));
					dc.FillSolidRect(header, panelHeader);
					CRect labelArea(header.left + 3, header.top, header.right - 2, header.bottom);
					CRect valueArea(cell.left + 2, header.bottom, cell.right - 2, cell.bottom - 1);
					drawText(labelArea, cells[index].label, labelFont, mutedText, DT_LEFT);
					drawText(valueArea, cells[index].value, index == 0 ? qnhFont : valueFont, text, DT_CENTER);
				}
			}
		}
	}

	// ----- Drawing the raw METAR -----
	if (!rawPanel.IsRectEmpty() && hasRawReport)
	{
		auto allDigits = [](const std::string& token, size_t first, size_t count) -> bool
		{
			if (first > token.size() || count > token.size() - first)
				return false;
			for (size_t index = first; index < first + count; ++index)
			{
				if (std::isdigit(static_cast<unsigned char>(token[index])) == 0)
					return false;
			}
			return true;
		};
		auto beginsWith = [](const std::string& token, const char* prefix) -> bool
		{
			const size_t length = std::strlen(prefix);
			return token.size() >= length && token.compare(0, length, prefix) == 0;
		};
		auto containsWeatherCode = [](const std::string& token) -> bool
		{
			static const char* codes[] = {
				"RA", "SN", "DZ", "TS", "FG", "BR", "HZ", "SH", "FZ", "GR", "GS", "SQ"
			};
			for (const char* code : codes)
			{
				if (token.find(code) != std::string::npos)
					return true;
			}
			return false;
		};

		CRect rawBounds(rawPanel);
		rawBounds.DeflateRect(4, 3);
		::SetBkMode(hDC, TRANSPARENT);
		int cursorX = rawBounds.left;
		int cursorY = rawBounds.top;
		TEXTMETRICA rawMetrics = {};
		if (rawFont != nullptr)
			::SelectObject(hDC, rawFont);
		::GetTextMetricsA(hDC, &rawMetrics);
		const int lineHeight = max(16, rawMetrics.tmHeight + max(3, rawMetrics.tmExternalLeading + 2));
		std::istringstream report(weather.rawReport);
		std::string token;
		size_t tokenIndex = 0;
		while (report >> token)
		{
			const bool qnhToken = token.size() == 5 &&
				(token[0] == 'Q' || token[0] == 'A') && allDigits(token, 1, 4);
			const bool windToken =
				(token.size() >= 7 && token.compare(token.size() - 2, 2, "KT") == 0) ||
				(token.size() >= 8 && token.compare(token.size() - 3, 3, "MPS") == 0) ||
				(token.size() == 7 && token[3] == 'V' && allDigits(token, 0, 3) && allDigits(token, 4, 3));
			const bool visibilityToken = token == "CAVOK" ||
				(token.size() == 4 && allDigits(token, 0, 4));
			const bool cloudToken = beginsWith(token, "FEW") || beginsWith(token, "SCT") ||
				beginsWith(token, "BKN") || beginsWith(token, "OVC") || beginsWith(token, "VV");
			const size_t temperatureSeparator = token.find('/');
			const bool temperatureToken = temperatureSeparator != std::string::npos &&
				temperatureSeparator > 0 && temperatureSeparator + 1 < token.size() &&
				token.size() <= 7;
			const bool weatherToken = tokenIndex > 2 && !windToken && !cloudToken &&
				!temperatureToken && containsWeatherCode(token);
			COLORREF tokenColor = text;
			COLORREF tokenFill = control;
			bool highlighted = false;
			if (qnhToken)
			{
				tokenColor = RGB(245, 214, 122);
				tokenFill = RGB(67, 58, 35);
				highlighted = true;
			}
			else if (windToken)
			{
				tokenColor = windColor;
				tokenFill = RGB(40, 58, 61);
				highlighted = true;
			}
			else if (visibilityToken)
			{
				tokenColor = token == "CAVOK" || std::atoi(token.c_str()) >= 5000
					? RGB(116, 207, 151)
					: (std::atoi(token.c_str()) >= 1500 ? RGB(245, 214, 122) : RGB(241, 116, 116));
				tokenFill = RGB(38, 59, 52);
				highlighted = true;
			}
			else if (weatherToken)
			{
				tokenColor = RGB(241, 164, 92);
				tokenFill = RGB(64, 47, 35);
				highlighted = true;
			}
			else if (cloudToken)
			{
				tokenColor = cyan;
			}
			else if (temperatureToken)
			{
				tokenColor = RGB(205, 190, 230);
			}

			HFONT tokenFont = highlighted ? rawBoldFont : rawFont;
			if (tokenFont != nullptr)
				::SelectObject(hDC, tokenFont);
			SIZE tokenSize = {};
			::GetTextExtentPoint32A(hDC, token.c_str(), static_cast<int>(token.size()), &tokenSize);
			const int tokenWidth = max(1, tokenSize.cx);
			if (cursorX > rawBounds.left && cursorX + tokenWidth > rawBounds.right)
			{
				cursorX = rawBounds.left;
				cursorY += lineHeight;
			}
			if (cursorY + lineHeight > rawBounds.bottom)
				break;
			if (highlighted)
				dc.FillSolidRect(CRect(cursorX - 1, cursorY, min(rawBounds.right, cursorX + tokenWidth + 2), cursorY + lineHeight), tokenFill);
			::SetTextColor(hDC, tokenColor);
			::TextOutA(hDC, cursorX, cursorY + max(0, (lineHeight - tokenSize.cy) / 2), token.c_str(), static_cast<int>(token.size()));
			SIZE spaceSize = {};
			::GetTextExtentPoint32A(hDC, " ", 1, &spaceSize);
			cursorX += tokenWidth + max(3, spaceSize.cx);
			++tokenIndex;
		}
	}

	if (originalFont != nullptr)
		::SelectObject(hDC, originalFont);

	gdi->Restore(graphicsState);
	if (savedDc != 0)
		::RestoreDC(hDC, savedDc);

	CBrush frameBrush(outerBorder);
	dc.FrameRect(content, &frameBrush);
	DrawWindowChrome(
		dc,
		radar_screen,
		m_AvisoLayoutMode,
		"Metar",
		false,
		mouseLocation,
		true);

	dc.Detach();
}
