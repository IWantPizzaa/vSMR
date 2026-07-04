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
	virtual bool OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool released);
	bool IsAvisoViewport() const;
	void ClearAvisoViewportCache();
	
private:
	void renderAvisoViewport(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation);
	string icao;
	map<string, CPosition> AptPositions;
	std::unique_ptr<AvisoViewportState> m_AvisoState;
};
