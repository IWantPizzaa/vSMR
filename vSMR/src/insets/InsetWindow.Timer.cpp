#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "radar/RadarScreen.hpp"
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

void CInsetWindow::OnClickScreenObject(const char* sItemString, POINT Pt, int Button)
{
	UNREFERENCED_PARAMETER(Pt);
	if (!IsTimer() || sItemString == nullptr)
		return;

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

	if (Button == BUTTON_LEFT)
		StartTimer(durationMinutes);
	else if (Button == BUTTON_RIGHT)
		ResetTimer(durationMinutes);
}

void CInsetWindow::StartTimer(int durationMinutes)
{
	if (!IsTimer() || durationMinutes < 1 || durationMinutes > static_cast<int>(m_TimerDeadlineTicks.size()))
		return;
	const size_t index = static_cast<size_t>(durationMinutes - 1);
	if (m_TimerDeadlineTicks[index] != 0)
		return;
	m_TimerDeadlineTicks[index] = ::GetTickCount64() +
		(static_cast<unsigned long long>(durationMinutes) * 60ULL * 1000ULL);
	m_TimerExpired[index] = false;
}

void CInsetWindow::ResetTimer(int durationMinutes)
{
	if (!IsTimer() || durationMinutes < 1 || durationMinutes > static_cast<int>(m_TimerDeadlineTicks.size()))
		return;
	const size_t index = static_cast<size_t>(durationMinutes - 1);
	m_TimerDeadlineTicks[index] = 0;
	m_TimerExpired[index] = false;
}

bool CInsetWindow::UpdateTimerCountdowns()
{
	if (!IsTimer())
		return false;

	const unsigned long long now = ::GetTickCount64();
	bool alarmDue = false;
	for (size_t index = 0; index < m_TimerDeadlineTicks.size(); ++index)
	{
		const unsigned long long deadline = m_TimerDeadlineTicks[index];
		if (deadline == 0 || now < deadline)
			continue;

		m_TimerDeadlineTicks[index] = 0;
		m_TimerExpired[index] = true;
		alarmDue = true;
	}
	return alarmDue;
}

int CInsetWindow::GetTimerRemainingSeconds(int durationMinutes, unsigned long long now) const
{
	if (!IsTimer() || durationMinutes < 1 || durationMinutes > static_cast<int>(m_TimerDeadlineTicks.size()))
		return 0;
	const size_t index = static_cast<size_t>(durationMinutes - 1);
	const unsigned long long deadline = m_TimerDeadlineTicks[index];
	if (deadline == 0)
		return 0;
	if (now >= deadline)
		return 0;
	return static_cast<int>((deadline - now + 999ULL) / 1000ULL);
}

void CInsetWindow::renderTimer(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
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

	const COLORREF outerBorder = RGB(5, 7, 8);
	const COLORREF innerBorder = RGB(82, 96, 101);
	const COLORREF idleFill = RGB(36, 48, 51);
	const COLORREF hoverFill = RGB(48, 64, 68);
	const COLORREF runningFill = RGB(38, 79, 91);
	const COLORREF expiredFill = RGB(92, 42, 42);
	const COLORREF idleText = RGB(208, 217, 220);
	const COLORREF runningText = RGB(115, 216, 229);
	const COLORREF expiredText = RGB(255, 167, 157);

	dc.FillSolidRect(content, idleFill);
	radar_screen->AddScreenObject(m_Id, "window", content, false, "Timer");
	const int savedDc = ::SaveDC(hDC);
	if (savedDc != 0)
		::IntersectClipRect(hDC, content.left, content.top, content.right, content.bottom);
	HFONT timerFont = GetTimerFont();
	HGDIOBJ originalFont = timerFont != nullptr ? ::SelectObject(hDC, timerFont) : nullptr;
	const int oldBkMode = ::SetBkMode(hDC, TRANSPARENT);
	const unsigned long long now = ::GetTickCount64();

	const int timerCount = static_cast<int>(m_TimerDeadlineTicks.size());
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
		const int remainingSeconds = GetTimerRemainingSeconds(durationMinutes, now);
		const bool running = m_TimerDeadlineTicks[static_cast<size_t>(index)] != 0;
		const bool expired = m_TimerExpired[static_cast<size_t>(index)];
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
		false);

	dc.Detach();
}
