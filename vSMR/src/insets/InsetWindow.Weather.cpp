#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "radar/RadarScreen.hpp"
#include "weather/WeatherStore.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

using VsmrRadarUiSupport::CopyRect;
using VsmrRadarUiSupport::DegToRad;

namespace
{
	struct WeatherPalette
	{
		COLORREF background;
		COLORREF surface;
		COLORREF compass;
		COLORREF header;
		COLORREF border;
		COLORREF divider;
		COLORREF text;
		COLORREF mutedText;
		COLORREF stale;
		COLORREF gust;
	};

	WeatherPalette ResolveWeatherPalette(bool dayTheme)
	{
		if (dayTheme)
		{
			return {
				RGB(133, 142, 145), RGB(182, 188, 190), RGB(173, 181, 183),
				RGB(150, 159, 162), RGB(63, 72, 76), RGB(125, 135, 138),
				RGB(23, 33, 38), RGB(70, 83, 88), RGB(178, 91, 38),
				RGB(161, 111, 20)
			};
		}

		return {
			RGB(43, 52, 55), RGB(41, 54, 57), RGB(36, 48, 51),
			RGB(33, 43, 46), RGB(5, 7, 8), RGB(70, 84, 88),
			RGB(201, 214, 219), RGB(143, 160, 165), RGB(230, 135, 55),
			RGB(245, 214, 122)
		};
	}

	Gdiplus::Color ToGdiColor(COLORREF color, BYTE alpha = 255)
	{
		return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
	}

	std::string FormatDegrees(int degrees)
	{
		char buffer[12] = {};
		std::snprintf(buffer, sizeof(buffer), "%03d\xB0", std::clamp(degrees, 0, 360));
		return buffer;
	}

	std::string FormatTemperature(bool available, int value)
	{
		return available ? std::to_string(value) + "\xB0 C" : "---";
	}

	std::string FormatObservationTime(std::time_t value)
	{
		if (value <= 0)
			return "----Z";
		std::tm utc = {};
		if (::gmtime_s(&utc, &value) != 0)
			return "----Z";
		char buffer[12] = {};
		std::snprintf(buffer, sizeof(buffer), "%02d%02dZ", utc.tm_hour, utc.tm_min);
		return buffer;
	}

	std::string FormatVisibility(const VsmrWeather::Snapshot& weather)
	{
		if (!weather.hasVisibility)
			return "----";
		if (weather.visibilityCavok)
			return "CAVOK";
		if (weather.visibilityMeters >= 9999)
			return "10 KM+";
		if (weather.visibilityMeters >= 1000)
		{
			if (weather.visibilityMeters % 1000 == 0)
				return std::to_string(weather.visibilityMeters / 1000) + " KM";
			char buffer[16] = {};
			std::snprintf(buffer, sizeof(buffer), "%.1f KM",
				static_cast<double>(weather.visibilityMeters) / 1000.0);
			return buffer;
		}
		return std::to_string(weather.visibilityMeters) + " M";
	}

	int WindFlowDirection(int meteorologicalDirection)
	{
		return (meteorologicalDirection + 180) % 360;
	}
}

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

void CInsetWindow::renderWeather(HDC hDC, CSMRRadar* radarScreen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	if (radarScreen == nullptr || gdi == nullptr || radarScreen->IsShutdownRequested())
		return;

	CDC dc;
	dc.Attach(hDC);
	CRect layoutBounds(radarScreen->GetRadarArea());
	CRect chatArea(radarScreen->GetChatArea());
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

	const bool dayTheme = radarScreen->GetUiColorTheme() == "day";
	const WeatherPalette palette = ResolveWeatherPalette(dayTheme);
	dc.FillSolidRect(content, palette.background);
	radarScreen->AddScreenObject(m_Id, "window", content, false, "");

	const int savedDc = ::SaveDC(hDC);
	if (savedDc != 0)
		::IntersectClipRect(hDC, content.left, content.top, content.right, content.bottom);
	const Gdiplus::GraphicsState graphicsState = gdi->Save();
	gdi->SetClip(CopyRect(content), Gdiplus::CombineModeIntersect);
	gdi->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	gdi->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

	const double scale = std::clamp(
		min(static_cast<double>(content.Width()) / 306.0,
			static_cast<double>(content.Height()) / 175.0),
		0.72,
		3.0);
	const auto fontHeight = [scale](int pixels)
	{
		return -max(1, static_cast<int>(std::lround(static_cast<double>(pixels) * scale)));
	};
	HFONT smallFont = GetWeatherFont(0, fontHeight(8), FW_NORMAL, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT labelFont = GetWeatherFont(1, fontHeight(8), FW_BOLD, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT valueFont = GetWeatherFont(2, fontHeight(10), FW_BOLD, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT directionFont = GetWeatherFont(3, fontHeight(20), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT speedFont = GetWeatherFont(4, fontHeight(14), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT detailFont = GetWeatherFont(5, fontHeight(9), FW_BOLD, FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT compassFont = GetWeatherFont(6, fontHeight(7), FW_NORMAL, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HGDIOBJ originalFont = ::GetCurrentObject(hDC, OBJ_FONT);

	const auto drawText = [&](const CRect& source, const std::string& value, HFONT font, COLORREF color, UINT flags)
	{
		CRect rect(source);
		if (font != nullptr)
			::SelectObject(hDC, font);
		::SetBkMode(hDC, TRANSPARENT);
		::SetTextColor(hDC, color);
		::DrawTextA(hDC, value.c_str(), -1, &rect,
			flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
	};

	const std::string station = VsmrWeather::NormalizeIcao(radarScreen->getActiveAirport());
	VsmrWeather::Snapshot weather;
	const bool hasSnapshot = !station.empty() && VsmrWeather::TryGet(station, weather);
	const bool hasWeather = hasSnapshot && (weather.hasWind || weather.hasQnh);
	const std::time_t now = std::time(nullptr);
	const bool stale = hasWeather && weather.updatedUtc > 0 && now > weather.updatedUtc &&
		(now - weather.updatedUtc) > (90 * 60);
	const VsmrScene::RadarScene* weatherScene = radarScreen->GetCurrentRadarScene();

	struct RunwayReference
	{
		bool valid = false;
		std::string name;
		double trueHeading = 0.0;
		int priority = 3;
		double headwindScore = -1000000.0;
	};
	RunwayReference runwayReference;
	const auto considerRunway = [&](const std::string& name, double trueHeading, bool headingValid)
	{
		if (!headingValid || name.empty() || weatherScene == nullptr)
			return;
		const auto statusIt = weatherScene->airport.runwayStatuses.find(name);
		if (statusIt == weatherScene->airport.runwayStatuses.end())
			return;
		const CRimcas::RunwayStatus status = static_cast<CRimcas::RunwayStatus>(statusIt->second);
		const int priority = (status == CRimcas::DEP || status == CRimcas::BOTH)
			? 0
			: (status == CRimcas::ARR ? 1 : 3);
		if (priority >= 3)
			return;
		const double score = weather.hasWind && !weather.windVariable
			? static_cast<double>(weather.windSpeedKnots) *
				std::cos(DegToRad(static_cast<double>(weather.windDirectionDegrees) - trueHeading))
			: 0.0;
		if (!runwayReference.valid || priority < runwayReference.priority ||
			(priority == runwayReference.priority && score > runwayReference.headwindScore))
		{
			runwayReference = { true, name, trueHeading, priority, score };
		}
	};
	for (const auto& runway : radarScreen->CachedRunwayGeometries)
	{
		considerRunway(runway.runwayNameA, runway.trueHeadingA, runway.trueHeadingAValid);
		considerRunway(runway.runwayNameB, runway.trueHeadingB, runway.trueHeadingBValid);
	}

	std::string headValue = "---";
	std::string crossValue = "---";
	if (runwayReference.valid && weather.hasWind && !weather.windVariable)
	{
		const double delta = DegToRad(
			static_cast<double>(weather.windDirectionDegrees) - runwayReference.trueHeading);
		const int head = static_cast<int>(std::lround(
			static_cast<double>(weather.windSpeedKnots) * std::cos(delta)));
		const int cross = static_cast<int>(std::lround(
			static_cast<double>(weather.windSpeedKnots) * std::sin(delta)));
		headValue = std::string(head >= 0 ? "H" : "T") + std::to_string(std::abs(head));
		crossValue = cross == 0 ? "0" : std::to_string(std::abs(cross)) + (cross > 0 ? "R" : "L");
	}

	const int peakWind = weather.hasWindGust
		? max(weather.windSpeedKnots, weather.windGustKnots)
		: weather.windSpeedKnots;
	const COLORREF windColor = peakWind < 10
		? RGB(55, 151, 96)
		: (peakWind < 20 ? RGB(214, 166, 35) :
			(peakWind < 30 ? RGB(220, 112, 36) : RGB(220, 66, 70)));
	const std::string directionValue = !hasSnapshot
		? "WAIT"
		: (!weather.hasWind ? "---" :
			(weather.windCalm ? "CALM" :
				(weather.windVariable ? "VRB" : FormatDegrees(weather.windDirectionDegrees))));
	const std::string speedValue = weather.hasWind
		? std::to_string(weather.windSpeedKnots) + " KT"
		: "-- KT";
	const std::string gustValue = weather.hasWindGust
		? "MAX " + std::to_string(weather.windGustKnots) + " KT"
		: "MAX -- KT";

	CRect inner(content);
	inner.DeflateRect(4, 4);
	dc.FillSolidRect(inner, palette.surface);
	dc.Draw3dRect(inner, palette.divider, palette.border);

	const bool compact = inner.Width() < 190 || inner.Height() < 100;
	if (compact)
	{
		CRect lines(inner);
		lines.DeflateRect(6, 3);
		const int half = lines.top + lines.Height() / 2;
		drawText(CRect(lines.left, lines.top, lines.right, half),
			directionValue + "  " + speedValue, directionFont,
			weather.hasWind ? windColor : palette.mutedText, DT_CENTER);
		std::string details = "QNH " + (weather.hasQnh ? std::to_string(weather.qnhHpa) : "----");
		if (runwayReference.valid)
			details += "  RWY " + runwayReference.name + "  " + headValue + " / " + crossValue;
		drawText(CRect(lines.left, half, lines.right, lines.bottom), details,
			detailFont, palette.text, DT_CENTER);
	}
	else
	{
		const int leftWidth = std::clamp(
			static_cast<int>(std::lround(static_cast<double>(inner.Width()) * 0.58)),
			112,
			max(112, inner.Width() - 96));
		CRect windArea(inner.left + 1, inner.top + 1, inner.left + leftWidth, inner.bottom - 1);
		CRect dataArea(windArea.right, inner.top + 1, inner.right - 1, inner.bottom - 1);
		dc.FillSolidRect(CRect(dataArea.left, dataArea.top, dataArea.left + 1, dataArea.bottom), palette.divider);

		const int footerHeight = max(16, static_cast<int>(std::lround(19.0 * scale)));
		CRect compassArea(windArea.left + 3, windArea.top + 3, windArea.right - 3, windArea.bottom - footerHeight);
		const int diameter = max(24, min(compassArea.Width(), compassArea.Height()) - 2);
		const float radius = static_cast<float>(diameter) * 0.5f;
		const float centerX = static_cast<float>(compassArea.left + compassArea.Width() / 2);
		const float centerY = static_cast<float>(compassArea.top + compassArea.Height() / 2);
		const Gdiplus::RectF roseRect(centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f);
		Gdiplus::SolidBrush roseBrush(ToGdiColor(palette.compass));
		Gdiplus::Pen roseBorder(ToGdiColor(palette.border), 1.0f);
		gdi->FillEllipse(&roseBrush, roseRect);
		gdi->DrawEllipse(&roseBorder, roseRect);

		const double pi = 3.14159265358979323846;
		const float variationInset = std::clamp(static_cast<float>(3.0 * scale), 2.0f, 7.0f);
		const Gdiplus::RectF variationRect(
			roseRect.X + variationInset,
			roseRect.Y + variationInset,
			roseRect.Width - variationInset * 2.0f,
			roseRect.Height - variationInset * 2.0f);
		Gdiplus::Pen variationPen(ToGdiColor(dayTheme ? RGB(51, 119, 147) : RGB(92, 180, 211), 220),
			std::clamp(static_cast<float>(2.0 * scale), 1.5f, 4.0f));
		variationPen.SetStartCap(Gdiplus::LineCapRound);
		variationPen.SetEndCap(Gdiplus::LineCapRound);
		if (weather.hasWindVariation)
		{
			int sweep = weather.windVariationToDegrees - weather.windVariationFromDegrees;
			if (sweep < 0)
				sweep += 360;
			if (sweep > 0)
			{
				gdi->DrawArc(&variationPen, variationRect,
					static_cast<Gdiplus::REAL>(WindFlowDirection(weather.windVariationFromDegrees) - 90),
					static_cast<Gdiplus::REAL>(sweep));
			}
		}
		else if (weather.hasWind && weather.windVariable && !weather.windCalm)
		{
			variationPen.SetDashStyle(Gdiplus::DashStyleDash);
			gdi->DrawEllipse(&variationPen, variationRect);
		}

		Gdiplus::Pen minorTick(ToGdiColor(palette.divider), 1.0f);
		Gdiplus::Pen majorTick(ToGdiColor(palette.mutedText), 1.0f);
		for (int degrees = 0; degrees < 360; degrees += 10)
		{
			const double angle = (static_cast<double>(degrees) - 90.0) * pi / 180.0;
			const float length = degrees % 30 == 0 ? 5.0f : 2.5f;
			const Gdiplus::PointF outer(
				centerX + static_cast<float>(std::cos(angle) * (radius - 2.0f)),
				centerY + static_cast<float>(std::sin(angle) * (radius - 2.0f)));
			const Gdiplus::PointF innerTick(
				centerX + static_cast<float>(std::cos(angle) * (radius - 2.0f - length)),
				centerY + static_cast<float>(std::sin(angle) * (radius - 2.0f - length)));
			gdi->DrawLine(degrees % 30 == 0 ? &majorTick : &minorTick, outer, innerTick);
			if (degrees % 30 == 0 && radius >= 42.0f)
			{
				const float labelRadius = radius - static_cast<float>(10.5 * scale);
				const int x = static_cast<int>(std::lround(centerX + std::cos(angle) * labelRadius));
				const int y = static_cast<int>(std::lround(centerY + std::sin(angle) * labelRadius));
				drawText(CRect(x - 10, y - 5, x + 10, y + 5),
					degrees == 0 ? "36" : std::to_string(degrees / 10),
					compassFont, palette.mutedText, DT_CENTER);
			}
		}

		// Draw the arrow first. The center plate then guarantees that the wind
		// vector never obscures the direction or speed values.
		if (weather.hasWind && !weather.windVariable && !weather.windCalm)
		{
			const double angle = (static_cast<double>(WindFlowDirection(weather.windDirectionDegrees)) - 90.0) * pi / 180.0;
			const float dx = static_cast<float>(std::cos(angle));
			const float dy = static_cast<float>(std::sin(angle));
			const float startRadius = radius * 0.43f;
			const float tipRadius = radius - 6.0f;
			const Gdiplus::PointF start(centerX + dx * startRadius, centerY + dy * startRadius);
			const Gdiplus::PointF tip(centerX + dx * tipRadius, centerY + dy * tipRadius);
			Gdiplus::Pen needle(ToGdiColor(windColor), std::clamp(radius * 0.05f, 2.0f, 5.0f));
			needle.SetStartCap(Gdiplus::LineCapRound);
			needle.SetEndCap(Gdiplus::LineCapRound);
			gdi->DrawLine(&needle, start, tip);
			const float arrowLength = std::clamp(radius * 0.19f, 7.0f, 17.0f);
			const float arrowWidth = std::clamp(radius * 0.095f, 4.0f, 9.0f);
			const Gdiplus::PointF base(tip.X - dx * arrowLength, tip.Y - dy * arrowLength);
			const Gdiplus::PointF perpendicular(-dy, dx);
			Gdiplus::PointF arrow[] = {
				tip,
				Gdiplus::PointF(base.X + perpendicular.X * arrowWidth, base.Y + perpendicular.Y * arrowWidth),
				Gdiplus::PointF(base.X - perpendicular.X * arrowWidth, base.Y - perpendicular.Y * arrowWidth)
			};
			Gdiplus::SolidBrush arrowBrush(ToGdiColor(windColor));
			gdi->FillPolygon(&arrowBrush, arrow, static_cast<INT>(_countof(arrow)));
		}

		const float plateHalfWidth = min(radius * 0.48f, static_cast<float>(42.0 * scale));
		const float plateHalfHeight = min(radius * 0.35f, static_cast<float>(24.0 * scale));
		Gdiplus::SolidBrush centerPlate(ToGdiColor(palette.compass));
		gdi->FillRectangle(&centerPlate, centerX - plateHalfWidth, centerY - plateHalfHeight,
			plateHalfWidth * 2.0f, plateHalfHeight * 2.0f);
		CRect directionArea(
			static_cast<int>(centerX - plateHalfWidth),
			static_cast<int>(centerY - 22.0 * scale),
			static_cast<int>(centerX + plateHalfWidth),
			static_cast<int>(centerY + 1.0 * scale));
		CRect speedArea(directionArea.left, directionArea.bottom, directionArea.right,
			static_cast<int>(centerY + 19.0 * scale));
		drawText(directionArea, directionValue, directionFont,
			weather.hasWind ? windColor : palette.mutedText, DT_CENTER);
		drawText(speedArea, speedValue, speedFont, palette.text, DT_CENTER);

		CRect footer(windArea.left + 4, windArea.bottom - footerHeight, windArea.right - 4, windArea.bottom - 2);
		const int footerMid = footer.left + footer.Width() / 2;
		drawText(CRect(footer.left, footer.top, footerMid, footer.bottom), gustValue,
			smallFont, weather.hasWindGust ? palette.gust : palette.mutedText, DT_LEFT);
		const std::string component = runwayReference.valid ? headValue + "  X" + crossValue : "H--  X--";
		drawText(CRect(footerMid, footer.top, footer.right, footer.bottom), component,
			smallFont, palette.text, DT_RIGHT);

		const int headerHeight = max(18, static_cast<int>(std::lround(21.0 * scale)));
		CRect header(dataArea.left + 1, dataArea.top, dataArea.right, dataArea.top + headerHeight);
		dc.FillSolidRect(header, palette.header);
		drawText(CRect(header.left + 5, header.top, header.right - 42, header.bottom),
			station.empty() ? "METAR" : station, labelFont, palette.text, DT_LEFT);
		drawText(CRect(header.right - 43, header.top, header.right - 4, header.bottom),
			FormatObservationTime(weather.observationUtc), smallFont,
			stale ? palette.stale : palette.mutedText, DT_RIGHT);

		struct WeatherRow
		{
			const char* label;
			std::string value;
		};
		const WeatherRow rows[] = {
			{ "VIS", FormatVisibility(weather) },
			{ "CLD", weather.cloudSummary.empty() ? "---" : weather.cloudSummary },
			{ "TEMP", FormatTemperature(weather.hasTemperature, weather.temperatureCelsius) },
			{ "DEW", FormatTemperature(weather.hasDewPoint, weather.dewPointCelsius) },
			{ "RWY", runwayReference.valid ? runwayReference.name : "---" },
			{ "HEAD", headValue + " KT" },
			{ "XWIND", crossValue + " KT" },
			{ "QNH", weather.hasQnh ? std::to_string(weather.qnhHpa) + " HPA" : "---- HPA" }
		};
		const int rowsTop = header.bottom;
		const int rowsHeight = max(1, dataArea.bottom - rowsTop);
		for (int index = 0; index < static_cast<int>(_countof(rows)); ++index)
		{
			const int top = rowsTop + rowsHeight * index / static_cast<int>(_countof(rows));
			const int bottom = rowsTop + rowsHeight * (index + 1) / static_cast<int>(_countof(rows));
			CRect row(dataArea.left + 1, top, dataArea.right, bottom);
			if (index == 4)
				dc.FillSolidRect(CRect(row.left, row.top, row.right, row.top + 1), palette.divider);
			const int labelWidth = std::clamp(static_cast<int>(std::lround(row.Width() * 0.37)), 28, 46);
			drawText(CRect(row.left + 4, row.top, row.left + labelWidth, row.bottom),
				rows[index].label, smallFont, palette.mutedText, DT_LEFT);
			drawText(CRect(row.left + labelWidth, row.top, row.right - 4, row.bottom),
				rows[index].value, valueFont, palette.text, DT_RIGHT);
		}

		if (!hasWeather)
		{
			CRect status(windArea.left + 8, windArea.top + 4, windArea.right - 8, windArea.top + headerHeight);
			drawText(status, hasSnapshot ? "NO CURRENT WEATHER" : "WAITING FOR METAR",
				smallFont, palette.mutedText, DT_CENTER);
		}
	}

	if (originalFont != nullptr)
		::SelectObject(hDC, originalFont);
	gdi->Restore(graphicsState);
	if (savedDc != 0)
		::RestoreDC(hDC, savedDc);

	CBrush frameBrush(palette.border);
	dc.FrameRect(content, &frameBrush);
	DrawWindowChrome(
		dc,
		radarScreen,
		m_AvisoLayoutMode,
		"METAR",
		false,
		mouseLocation,
		true,
		dayTheme);
	dc.Detach();
}
