#pragma once

#include "platform/windows/PrecompiledHeader.hpp"
#include <string>
#include "EuroScopePlugIn.h"
#define _USE_MATH_DEFINES
// ReSharper disable once CppUnusedIncludeDirective
#include <math.h>
#include <sstream>
#include <iomanip>
#include <cstring>

#define VSTRIPS_PORT 53487

const int TAG_ITEM_DATALINK_STS = 444;
const int TAG_ITEM_HOLDING_POINT = 445;
const int TAG_FUNC_DATALINK_MENU = 544;

const int TAG_FUNC_DATALINK_CONFIRM = 545;
const int TAG_FUNC_DATALINK_STBY = 546;
const int TAG_FUNC_DATALINK_VOICE = 547;
const int TAG_FUNC_DATALINK_RESET = 548;
const int TAG_FUNC_DATALINK_MESSAGE = 549;
const int TAG_FUNC_HOLDING_POINT_EDIT = 550;
const int TAG_FUNC_HOLDING_POINT_MANUAL = 551;
const int TAG_FUNC_HOLDING_POINT_SELECT = 552;
const int TAG_FUNC_HOLDING_POINT_COMMIT = 553;
const int TAG_FUNC_HOLDING_POINT_CLEAR = 554;

namespace VsmrRadarUiSupport
{
inline bool startsWith(const char* pre, const char* str)
{
	if (pre == nullptr || str == nullptr)
		return false;

	const std::size_t prefixLength = std::strlen(pre);
	const std::size_t textLength = std::strlen(str);
	return textLength >= prefixLength && std::strncmp(pre, str, prefixLength) == 0;
}

inline void replaceAll(std::string& source, const std::string& from, const std::string& to)
{
	std::string newString;
	newString.reserve(source.length());  // avoids a few memory allocations

	std::string::size_type lastPos = 0;
	std::string::size_type findPos;

	while (std::string::npos != (findPos = source.find(from, lastPos)))
	{
		newString.append(source, lastPos, findPos - lastPos);
		newString += to;
		lastPos = findPos + from.length();
	}

	// Care for the rest after last occurrence
	newString += source.substr(lastPos);

	source.swap(newString);
}

inline Gdiplus::Rect CopyRect(const CRect& rect)
{
	return Gdiplus::Rect(rect.left, rect.top, rect.Width(), rect.Height());
}

inline double TrueBearing(EuroScopePlugIn::CPosition pos1, EuroScopePlugIn::CPosition pos2)
{
	const float PI = float(atan2(0, -1));

	// returns the true bearing from pos1 to pos2
	double lat1 = pos1.m_Latitude / 180 * PI;
	double lat2 = pos2.m_Latitude / 180 * PI;
	double lon1 = pos1.m_Longitude / 180 * PI;
	double lon2 = pos2.m_Longitude / 180 * PI;
	double dir = std::fmod(std::atan2(std::sin(lon2 - lon1) * std::cos(lat2), std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(lon2 - lon1)), 2 * PI) * 180 / PI;

	return dir / 180 * PI;
}

inline POINT rotate_point(POINT p, double angle, POINT c)
{
	double sine = std::sin(angle * M_PI / 180);
	double cosi = std::cos(angle * M_PI / 180);

	// translate point back to origin:
	p.x -= c.x;
	p.y -= c.y;

	// rotate point
	double xnew = p.x * cosi - p.y * sine;
	double ynew = p.x * sine + p.y * cosi;

	// translate point back:
	p.x = LONG(xnew + c.x);
	p.y = LONG(ynew + c.y);
	return p;
}

// Liang-Barsky function by Daniel White @ http://www.skytopia.com/project/articles/compsci/clipping.html
// This function inputs 8 numbers, and outputs 4 new numbers (plus a boolean value to say whether the clipped line is drawn at all).
//
inline bool LiangBarsky(RECT Area, POINT fromSrc, POINT toSrc, POINT& ClipFrom, POINT& ClipTo)         // The output values, so declare these outside.
{

	double edgeLeft, edgeRight, edgeBottom, edgeTop, x0src, y0src, x1src, y1src;

	edgeLeft = Area.left;
	edgeRight = Area.right;
	edgeBottom = Area.top;
	edgeTop = Area.bottom;

	x0src = fromSrc.x;
	y0src = fromSrc.y;
	x1src = toSrc.x;
	y1src = toSrc.y;

	double t0 = 0.0;    double t1 = 1.0;
	double xdelta = x1src - x0src;
	double ydelta = y1src - y0src;
	double p = 0, q = 0, r;

	for (int edge = 0; edge<4; edge++) {   // Traverse through left, right, bottom, top edges.
		if (edge == 0) { p = -xdelta;    q = -(edgeLeft - x0src); }
		if (edge == 1) { p = xdelta;     q = (edgeRight - x0src); }
		if (edge == 2) { p = -ydelta;    q = -(edgeBottom - y0src); }
		if (edge == 3) { p = ydelta;     q = (edgeTop - y0src); }
		r = q / p;
		if (p == 0 && q<0) return false;   // Don't draw line at all. (parallel line outside)

		if (p<0) {
			if (r>t1) return false;         // Don't draw line at all.
			else if (r>t0) t0 = r;            // Line is clipped!
		}
		else if (p>0) {
			if (r<t0) return false;      // Don't draw line at all.
			else if (r<t1) t1 = r;         // Line is clipped!
		}
	}

	ClipFrom.x = long(x0src + t0*xdelta);
	ClipFrom.y = long(y0src + t0*ydelta);
	ClipTo.x = long(x0src + t1*xdelta);
	ClipTo.y = long(y0src + t1*ydelta);

	return true;        // (clipped) line is drawn
}

inline bool mouseWithin(POINT mouseLocation, CRect rect) {
	if (mouseLocation.x >= rect.left + 1 && mouseLocation.x <= rect.right - 1 && mouseLocation.y >= rect.top + 1 && mouseLocation.y <= rect.bottom - 1)
		return true;
	return false;
}
//---Radians-----------------------------------------

inline double DegToRad(double x)
{
	return x / 180.0 * M_PI;
}

inline double RadToDeg(double x)
{
	return x / M_PI * 180.0;
}

inline EuroScopePlugIn::CPosition BetterHarversine(
	EuroScopePlugIn::CPosition init,
	double angle,
	double meters)
{
	EuroScopePlugIn::CPosition newPos;

	double d = (meters*0.00053996) / 60 * M_PI / 180;
	double trk = DegToRad(angle);
	double lat0 = DegToRad(init.m_Latitude);
	double lon0 = DegToRad(init.m_Longitude);

	double lat = std::asin(std::sin(lat0) * std::cos(d) + std::cos(lat0) * std::sin(d) * std::cos(trk));
	double lon = std::cos(lat) == 0 ? lon0 : std::fmod(lon0 + std::asin(std::sin(trk) * std::sin(d) / std::cos(lat)) + M_PI, 2 * M_PI) - M_PI;

	newPos.m_Latitude = RadToDeg(lat);
	newPos.m_Longitude = RadToDeg(lon);

	return newPos;
}

inline std::string padWithZeros(int padding, int value)
{
	std::stringstream ss;
	ss << std::setfill('0') << std::setw(padding) << value;
	return ss.str();
}
}

//

const int DRAWING_TAG = 1211;
const int DRAWING_AC_SYMBOL = 1212;
const int DRAWING_AC_SYMBOL_APPWINDOW_BASE = 1251;
const int DRAWING_AC_SYMBOL_APPWINDOW1 = 1252;
const int DRAWING_AC_SYMBOL_APPWINDOW3 = 1254;

const int TAG_CITEM_NO = 1910;
const int TAG_CITEM_CALLSIGN = 1911;
const int TAG_CITEM_FPBOX = 1912;
const int TAG_CITEM_RWY = 1913;
const int TAG_CITEM_GATE = 1914;
const int TAG_CITEM_MANUALCORRELATE = 1915;
const int TAG_CITEM_SID = 1916;
const int TAG_CITEM_GROUNDSTATUS = 1917;
const int TAG_CITEM_UKSTAND = 1990;
const int TAG_CITEM_REMARK = 1991;
const int TAG_CITEM_SCRATCHPAD = 1992;
const int TAG_CITEM_CLEARANCE = 1993;
const int TAG_CITEM_HOLDINGPOINT = 1994;
const int TAG_CITEM_READY_STARTUP = 1995;
// RIMCAS menu and runtime callbacks
const int RIMCAS_CLOSE = EuroScopePlugIn::TAG_ITEM_FUNCTION_NO;
const int RIMCAS_ACTIVE_AIRPORT_FUNC = 8008;
const int RIMCAS_AVISO_PRESET_RENAME = 8075;
const int RUNTIME_DATALINK_DELAY_EDIT = 8078;
const int RUNTIME_DATALINK_COOLDOWN_EDIT = 8079;
const int RUNTIME_DATALINK_CALLSIGN_EDIT = 8080;
const int VSMR_GROUND_STATUS_SELECT = 8076;

const int RIMCAS_IAW = 7000;

const int RIMCAS_UPDATEFILTER = 6015;
const int RIMCAS_UPDATEFILTER1 = 6016;
const int RIMCAS_UPDATEFILTER2 = 6017;
const int RIMCAS_UPDATEFILTER3 = 6018;

const int APPWINDOW_BASE = 8887;
const int APPWINDOW_ONE = 8888;
// 8889 is intentionally left unused so existing inset identifiers stay stable.
const int APPWINDOW_AVISO = 8890;
const int APPWINDOW_WEATHER = 8891;
const int APPWINDOW_TIMER = 8892;
const int RUNTIME_MENU_RAIL = 8893;
const int RUNTIME_MENU_POPUP = 8894;
