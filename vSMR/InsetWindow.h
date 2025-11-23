#pragma once
#include "SMRTagTypes.hpp"
#include "EuroScopePlugIn.h"
#include <GdiPlus.h>
#include "Constant.hpp"
#include <string>
#include <map>
#include "Logger.h"

using namespace std;
using namespace EuroScopePlugIn;
using namespace Gdiplus;


class CSMRRadar;

class CInsetWindow
{
public:
	CInsetWindow(int Id, int minHeight = 100, int minWidth = 300, string windowTitle = "");
	virtual ~CInsetWindow();

	struct Utils
	{
		static string getEnumString(SMRTagType type) {
			if (type == SMRTagType::Departure)
				return "departure";
			if (type == SMRTagType::Arrival)
				return "arrival";
			if (type == SMRTagType::Uncorrelated)
				return "uncorrelated";
			return "airborne";
		}
		static RECT GetAreaFromText(CDC* dc, string text, POINT Pos) {
			RECT Area = { Pos.x, Pos.y, Pos.x + dc->GetTextExtent(text.c_str()).cx, Pos.y + dc->GetTextExtent(text.c_str()).cy };
			return Area;
		}

		static RECT drawToolbarButton(CDC* dc, string letter, CRect TopBar, int left, POINT mouseLocation)
		{
			POINT TopLeft = { TopBar.right - left, TopBar.top + 2 };
			POINT BottomRight = { TopBar.right - (left - 11), TopBar.bottom - 2 };
			CRect Rect(TopLeft, BottomRight);
			Rect.NormalizeRect();
			CBrush ButtonBrush(RGB(60, 60, 60));
			dc->FillRect(Rect, &ButtonBrush);
			dc->SetTextColor(RGB(0, 0, 0));
			dc->TextOutA(Rect.left + 2, Rect.top, letter.c_str());

			if (mouseWithin(mouseLocation, Rect))
				dc->Draw3dRect(Rect, RGB(45, 45, 45), RGB(75, 75, 75));
			else
				dc->Draw3dRect(Rect, RGB(75, 75, 75), RGB(45, 45, 45));

			return Rect;
		}
	};

	// Definition
	int m_Id = -1, m_Scale = 15, m_Filter = 5500;
	RECT m_Area = { 200, 200, 600, 500 };
	POINT m_Offset = { 0, 0 }, m_OffsetInit = { 0, 0 }, m_OffsetDrag = { 0, 0 };
	bool m_Grip = false;
	double m_Rotation = 0;
	int m_window_min_height;
	int m_window_min_width;

	virtual void render(HDC Hdc, CSMRRadar * radar_screen, Graphics* gdi, POINT mouseLocation, multimap<string, string> DistanceTools) = 0;
	virtual void OnClickScreenObject(const char * sItemString, POINT Pt, int Button) = 0;
	virtual bool OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool released) = 0;
	
protected:
	string icao;
	string m_window_title;
};

class CApproachWindow : public CInsetWindow
{
public:
	CApproachWindow(int Id);
	virtual ~CApproachWindow();

	map<string, double> m_TagAngles;

	void render(HDC Hdc, CSMRRadar* radar_screen, Graphics* gdi, POINT mouseLocation, multimap<string, string> DistanceTools);
	void setAirport(string icao);
	POINT projectPoint(CPosition pos);
	void OnClickScreenObject(const char* sItemString, POINT Pt, int Button);
	bool OnMoveScreenObject(const char* sObjectId, POINT Pt, RECT Area, bool released);

private:
	map<string, CPosition> AptPositions;
};

class CListWindow : public CInsetWindow {
public:
	CListWindow(int Id, string listTitle, int minHeight = 50, int minWidth = 150) : CInsetWindow(Id, minHeight, minWidth, listTitle) {}
	virtual ~CListWindow() {}

	void render(HDC Hdc, CSMRRadar* radar_screen, Graphics* gdi, POINT mouseLocation, multimap<string, string> DistanceTools);
	void OnClickScreenObject(const char* sItemString, POINT Pt, int Button);
	bool OnMoveScreenObject(const char* sObjectId, POINT Pt, RECT Area, bool released);

private:

};