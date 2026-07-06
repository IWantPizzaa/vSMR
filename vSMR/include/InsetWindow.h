#pragma once
#include "EuroScopePlugIn.h"
#include <string>
#include <map>
#include <memory>
#include <GdiPlus.h>
#include "Logger.h"

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
		AvisoViewport = 1
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

	virtual void render(HDC Hdc, CSMRRadar * radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation, multimap<string, string> DistanceTools);
	virtual void setAirport(string icao);
	virtual POINT projectPoint(CPosition pos);
	virtual void OnClickScreenObject(const char * sItemString, POINT Pt, int Button);
	virtual bool OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool released, const RECT* layoutBounds = nullptr);
	bool IsAvisoViewport() const;
	bool IsPointInside(POINT Pt) const;
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
	void ResetAvisoInteractionState();
	
private:
	void renderAvisoViewport(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation);
	string icao;
	map<string, CPosition> AptPositions;
	std::unique_ptr<AvisoViewportState> m_AvisoState;
};
