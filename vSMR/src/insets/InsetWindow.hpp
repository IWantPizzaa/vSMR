#pragma once
#include "EuroScopePlugIn.h"
#include "diagnostics/PerformanceDiagnostics.hpp"
#include <array>
#include <string>
#include <map>
#include <memory>
#include <GdiPlus.h>

using namespace std;
using namespace EuroScopePlugIn;

class CSMRRadar;
struct AvisoViewportState;

class CInsetWindow
{
public:
	enum class Mode
	{
		SecondaryRadar = 0,
		AvisoViewport = 1,
		Weather = 2,
		Timer = 3
	};

	enum class AvisoLayoutMode
	{
		Floating = 0,
		SplitLeft = 1,
		SplitRight = 2,
		CornerTopLeft = 3,
		CornerTopRight = 4,
		CornerBottomLeft = 5,
		CornerBottomRight = 6,
		SplitTop = 7,
		SplitBottom = 8
	};

	enum class ResizeRegion
	{
		None = 0,
		Left,
		Right,
		Top,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight
	};

	CInsetWindow(int Id);
	virtual ~CInsetWindow();

	// Definition
	int m_Id = -1, m_Scale = 15, m_Filter = 5500;
	RECT m_Area = { 200, 200, 600, 500 };
	POINT m_Offset = { 0, 0 }, m_OffsetInit = { 0, 0 }, m_OffsetDrag = { 0, 0 };
	bool m_Grip = false;
	double m_Rotation = 0;
	Mode m_Mode = Mode::SecondaryRadar;
	int m_AvisoScale = 350;
	double m_AvisoCenterLatitude = 0.0;
	double m_AvisoCenterLongitude = 0.0;
	double m_AvisoDragStartLatitude = 0.0;
	double m_AvisoDragStartLongitude = 0.0;
	bool m_AvisoViewInitialized = false;
	bool m_AvisoRightPanning = false;
	bool m_AvisoScrollSelected = false;
	RECT m_AvisoScreenArea = { 0, 0, 0, 0 };
	bool m_AvisoScreenAreaValid = false;
	HWND m_AvisoRenderWindow = nullptr;
	AvisoLayoutMode m_AvisoLayoutMode = AvisoLayoutMode::Floating;

	map<string, double> m_TagAngles;
	map<string, POINT> m_TagOffsets;
	map<string, POINT> m_TagDragOffsetFromCenter;
	map<string, POINT> m_TargetPoints;
	map<string, CRect> m_TagAreas;
	string m_TagBeingDragged;

	virtual void render(HDC Hdc, CSMRRadar * radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation);
	virtual void setAirport(string icao);
	virtual POINT projectPoint(CPosition pos);
	virtual void OnClickScreenObject(const char * sItemString, POINT Pt, int Button);
	virtual bool OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool released, const RECT* layoutBounds = nullptr);
	bool IsAvisoViewport() const;
	bool IsSecondaryRadar() const;
	bool IsWeather() const;
	bool IsTimer() const;
	bool UpdateTimerCountdowns();
	bool SupportsPanAndZoom() const;
	bool IsSnappedLayout() const;
	bool IsPointInside(POINT Pt) const;
	CRect GetWindowFrameRect() const;
	CRect GetWindowContentRect() const;
	ResizeRegion HitTestResize(POINT Pt) const;
	bool HitTestTitleBar(POINT Pt) const;
	bool BeginWindowMove(POINT Pt, const RECT* layoutBounds, bool requireTitleBarHit = true);
	bool UpdateWindowMove(POINT Pt, const RECT* layoutBounds);
	bool EndWindowMove(POINT Pt, const RECT* layoutBounds);
	bool BeginWindowResize(ResizeRegion region, POINT Pt, const RECT* layoutBounds);
	bool UpdateWindowResize(POINT Pt, const RECT* layoutBounds);
	bool EndWindowResize(POINT Pt, const RECT* layoutBounds);
	bool IsWindowMoveActive() const;
	bool IsWindowResizeActive() const;
	ResizeRegion GetActiveResizeRegion() const;
	void CancelWindowInteraction();
	bool GetSnapPreviewRect(CRect& preview) const;
	void RenderSnapPreview(Gdiplus::Graphics& graphics) const;
	void ApplyAvisoLayoutBounds(const RECT* layoutBounds);
	void SnapAvisoLayoutToPoint(POINT Pt, const RECT* layoutBounds);
	void UpdateAvisoScreenArea(HWND hwnd);
	bool TryMapAvisoScreenPoint(POINT screenPoint, POINT& avisoPoint) const;
	void BeginAvisoPan(POINT Pt);
	bool UpdateAvisoPan(POINT Pt);
	void EndAvisoPan();
	void FloatAvisoViewport(POINT Pt, const RECT* layoutBounds);
	bool ZoomAvisoAtPoint(POINT Pt, double scaleMultiplier);
	void ClearAvisoViewportCache();
	void InvalidateAvisoViewportRendering();
	void CancelAvisoViewportRender();
	void ResetAvisoInteractionState();
	VsmrPerformance::AvisoQueueDepth GetAvisoPerformanceQueueDepth();
	std::size_t GetAvisoPerformanceBitmapCount(std::uint64_t* estimatedBytes = nullptr) const;
	double GetLastRdfRenderMilliseconds() const noexcept;
	double GetLastChromeRenderMilliseconds() const noexcept;
	
private:
	void renderAvisoViewport(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation);
	void renderWeather(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation);
	void renderTimer(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation);
	void StartTimer(int durationMinutes);
	void ResetTimer(int durationMinutes);
	int GetTimerRemainingSeconds(int durationMinutes, unsigned long long now) const;
	HFONT GetWeatherFont(size_t index, int height, int weight, DWORD pitchAndFamily, const char* faceName);
	HFONT GetTimerFont();
	void ReleaseCachedFonts();
	string icao;
	CPosition m_AirportPosition;
	bool m_AirportPositionValid = false;
	std::array<unsigned long long, 4> m_TimerDeadlineTicks = { 0, 0, 0, 0 };
	std::array<bool, 4> m_TimerExpired = { false, false, false, false };
	std::array<HFONT, 9> m_WeatherFonts = {};
	std::array<int, 9> m_WeatherFontHeights = {};
	HFONT m_TimerFont = nullptr;
	const Gdiplus::Font* m_SrwFontSource = nullptr;
	Gdiplus::REAL m_SrwFontSize = 0.0f;
	INT m_SrwFontStyle = 0;
	std::wstring m_SrwFontFamily;
	std::unique_ptr<Gdiplus::Font> m_SrwBoldFont;
	int m_SrwBlankWidth = 0;
	int m_SrwLineHeight = 0;
	std::unique_ptr<AvisoViewportState> m_AvisoState;
	double m_LastRdfRenderMilliseconds = 0.0;
	double m_LastChromeRenderMilliseconds = 0.0;
	bool m_WindowMoveActive = false;
	bool m_WindowMoveStartedSnapped = false;
	bool m_WindowMoveDetached = false;
	bool m_WindowInteractionMoved = false;
	POINT m_WindowInteractionStartPoint = { 0, 0 };
	RECT m_WindowInteractionStartArea = { 0, 0, 0, 0 };
	bool m_WindowResizeActive = false;
	bool m_WindowResizeDetached = false;
	ResizeRegion m_WindowResizeRegion = ResizeRegion::None;
	RECT m_WindowResizeStartFrame = { 0, 0, 0, 0 };
	bool m_SnapPreviewValid = false;
	AvisoLayoutMode m_SnapPreviewMode = AvisoLayoutMode::Floating;
	RECT m_SnapPreviewArea = { 0, 0, 0, 0 };
};
