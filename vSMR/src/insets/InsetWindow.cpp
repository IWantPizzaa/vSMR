#include "platform/windows/PrecompiledHeader.hpp"
#include "insets/InsetWindow.hpp"
#include "insets/InsetWindow.Internal.hpp"

using VsmrInsetWindowInternal::IsAvisoSnappedLayout;

void CInsetWindow::ReleaseCachedFonts()
{
	for (HFONT& font : m_WeatherFonts)
	{
		if (font != nullptr)
		{
			::DeleteObject(font);
			font = nullptr;
		}
	}
	m_WeatherFontHeights.fill(0);

	if (m_TimerFont != nullptr)
	{
		::DeleteObject(m_TimerFont);
		m_TimerFont = nullptr;
	}

	m_SrwFontSource = nullptr;
	m_SrwFontSize = 0.0f;
	m_SrwFontStyle = 0;
	m_SrwFontFamily.clear();
	m_SrwBoldFont.reset();
	m_SrwBlankWidth = 0;
	m_SrwLineHeight = 0;
}

bool CInsetWindow::IsAvisoViewport() const
{
	return m_Mode == Mode::AvisoViewport;
}

bool CInsetWindow::IsSecondaryRadar() const
{
	return m_Mode == Mode::SecondaryRadar;
}

bool CInsetWindow::IsWeather() const
{
	return m_Mode == Mode::Weather;
}

bool CInsetWindow::IsTimer() const
{
	return m_Mode == Mode::Timer;
}

bool CInsetWindow::SupportsPanAndZoom() const
{
	return IsAvisoViewport() || IsSecondaryRadar();
}

bool CInsetWindow::IsSnappedLayout() const
{
	return IsAvisoSnappedLayout(m_AvisoLayoutMode);
}

double CInsetWindow::GetLastRdfRenderMilliseconds() const noexcept
{
	return m_LastRdfRenderMilliseconds;
}

double CInsetWindow::GetLastChromeRenderMilliseconds() const noexcept
{
	return m_LastChromeRenderMilliseconds;
}

void CInsetWindow::setAirport(string airportIcao)
{
	icao = airportIcao;
}
