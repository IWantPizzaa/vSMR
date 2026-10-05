#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "radar/RadarScreen.hpp"
#include "plugin/Plugin.hpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	constexpr int kTimerColumnCount = 2;
	constexpr int kTimerRowCount = 2;
}

HFONT CInsetWindow::GetTimerFont()
{
	if (m_TimerFont == nullptr)
	{
		m_TimerFont = ::CreateFontA(
			-10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	}
	return m_TimerFont;
}

void CInsetWindow::OnClickScreenObject(const char* sItemString, POINT Pt, int Button, CSMRRadar* radarScreen)
{
	UNREFERENCED_PARAMETER(Pt);
	if (sItemString == nullptr || radarScreen == nullptr || radarScreen->IsShutdownRequested())
		return;
	if (IsWeather())
	{
		if (Button != BUTTON_LEFT) return;
		const int step = strcmp(sItemString, "weather.previous") == 0 ? -1 :
			strcmp(sItemString, "weather.next") == 0 ? 1 : 0;
		if (step == 0) return;
		if (radarScreen->WeatherAllAirports)
			m_WeatherPage = (m_WeatherPage + step + m_WeatherPageCount) % m_WeatherPageCount;
		else
		{
			const auto stations = radarScreen->GetOpenWeatherAirports();
			if (stations.empty()) return;
			const auto selected = std::find(stations.begin(), stations.end(), m_WeatherSelectedStation);
			const int index = selected != stations.end() ? static_cast<int>(selected - stations.begin()) : 0;
			m_WeatherSelectedStation = stations[(index + step + stations.size()) % stations.size()];
		}
		radarScreen->RequestRefresh();
		return;
	}
	if (!IsTimer()) return;

	int durationMinutes = 0;
	if (strcmp(sItemString, "timer.1m") == 0)
		durationMinutes = 1;
	else if (strcmp(sItemString, "timer.2m") == 0)
		durationMinutes = 2;
	else if (strcmp(sItemString, "timer.3m") == 0)
		durationMinutes = 3;
	else if (strcmp(sItemString, "timer.4m") == 0)
		durationMinutes = 4;
	if (durationMinutes == 0)
		return;

	auto* plugin = static_cast<CSMRPlugin*>(radarScreen->GetPlugIn());
	if (plugin != nullptr && (Button == BUTTON_LEFT || Button == BUTTON_RIGHT))
		plugin->ChangeTimerCountdown(durationMinutes, Button == BUTTON_RIGHT);
}

void CInsetWindow::renderTimer(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	if (radar_screen == nullptr || gdi == nullptr || radar_screen->IsShutdownRequested())
		return;
	const auto* plugin = static_cast<CSMRPlugin*>(radar_screen->GetPlugIn());
	if (plugin == nullptr)
		return;
	const auto& timers = plugin->GetTimerCountdowns();

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

	// Resolve the live interface theme on every frame, just like the title bar.
	// Changing Day/Night must not restart or reset any countdown.
	const bool dayTheme = radar_screen->GetUiColorTheme() == "day";
	const COLORREF outerBorder = dayTheme ? RGB(63, 72, 76) : RGB(5, 7, 8);
	const COLORREF innerBorder = dayTheme ? RGB(125, 135, 138) : RGB(82, 96, 101);
	const COLORREF idleFill = dayTheme ? RGB(173, 181, 183) : RGB(36, 48, 51);
	const COLORREF hoverFill = dayTheme ? RGB(190, 201, 204) : RGB(48, 64, 68);
	const COLORREF runningFill = dayTheme ? RGB(153, 194, 204) : RGB(38, 79, 91);
	const COLORREF expiredFill = dayTheme ? RGB(218, 174, 176) : RGB(92, 42, 42);
	const COLORREF idleText = dayTheme ? RGB(23, 33, 38) : RGB(208, 217, 220);
	const COLORREF runningText = dayTheme ? RGB(26, 68, 83) : RGB(115, 216, 229);
	const COLORREF expiredText = dayTheme ? RGB(111, 34, 40) : RGB(255, 167, 157);

	dc.FillSolidRect(content, idleFill);
	radar_screen->AddScreenObject(m_Id, "window", content, false, "Timer");
	const int savedDc = ::SaveDC(hDC);
	if (savedDc != 0)
		::IntersectClipRect(hDC, content.left, content.top, content.right, content.bottom);
	HFONT timerFont = GetTimerFont();
	HGDIOBJ originalFont = timerFont != nullptr ? ::SelectObject(hDC, timerFont) : nullptr;
	const int oldBkMode = ::SetBkMode(hDC, TRANSPARENT);
	const unsigned long long now = ::GetTickCount64();

	const int timerCount = TimerCountdownState::Count;
	for (int durationMinutes = 1; durationMinutes <= timerCount; ++durationMinutes)
	{
		const int index = durationMinutes - 1;
		const int column = index % kTimerColumnCount;
		const int row = index / kTimerColumnCount;
		CRect cell(
			content.left + (content.Width() * column) / kTimerColumnCount,
			content.top + (content.Height() * row) / kTimerRowCount,
			content.left + (content.Width() * (column + 1)) / kTimerColumnCount,
			content.top + (content.Height() * (row + 1)) / kTimerRowCount);
		const int remainingSeconds = timers.RemainingSeconds(durationMinutes, now);
		const bool running = timers.Running(durationMinutes);
		const bool expired = timers.Expired(durationMinutes);
		COLORREF fill = running ? runningFill : (expired ? expiredFill : idleFill);
		if (!running && !expired && cell.PtInRect(mouseLocation))
			fill = hoverFill;
		dc.FillSolidRect(cell, fill);
		dc.Draw3dRect(cell, innerBorder, outerBorder);

		char label[16] = {};
		if (running)
		{
			std::snprintf(label, sizeof(label), "%d:%02d", remainingSeconds / 60, remainingSeconds % 60);
		}
		else if (expired)
		{
			std::snprintf(label, sizeof(label), "0:00");
		}
		else
		{
			std::snprintf(label, sizeof(label), "%dM", durationMinutes);
		}
		::SetTextColor(hDC, running ? runningText : (expired ? expiredText : idleText));
		CRect textRect(cell);
		::DrawTextA(hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		const std::string objectId = "timer." + std::to_string(durationMinutes) + "m";
		radar_screen->AddScreenObject(
			m_Id,
			objectId.c_str(),
			cell,
			false,
			"Left click to start; right click to reset");
	}

	::SetBkMode(hDC, oldBkMode);
	if (originalFont != nullptr)
		::SelectObject(hDC, originalFont);
	if (savedDc != 0)
		::RestoreDC(hDC, savedDc);

	CBrush frameBrush(outerBorder);
	dc.FrameRect(content, &frameBrush);
	DrawWindowChrome(
		dc,
		radar_screen,
		AvisoLayoutMode::Floating,
		"Timer",
		false,
		mouseLocation,
		false,
		dayTheme);

	dc.Detach();
}
