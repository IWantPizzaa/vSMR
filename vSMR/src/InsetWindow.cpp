#include "stdafx.h"
#include "InsetWindow.h"
#include "SMRRadar.hpp"
#include "SMRTagDefinitionUtils.hpp"
#include <chrono>
#include <future>

namespace
{
	constexpr double kAvisoMetersPerNm = 1852.0;
	constexpr double kAvisoLatMetersPerDegree = 110540.0;
	constexpr double kAvisoLonMetersPerDegree = 111320.0;

	double ClampAvisoLatitude(double latitude)
	{
		return std::clamp(latitude, -85.0, 85.0);
	}

	double AvisoCosLatitude(double latitude)
	{
		return max(0.05, std::abs(std::cos(DegToRad(latitude))));
	}

	RECT DrawInsetToolbarButton(CDC* dc, const string& letter, CRect topBar, int left, POINT mouseLocation)
	{
		POINT topLeft = { topBar.right - left, topBar.top + 2 };
		POINT bottomRight = { topBar.right - (left - 11), topBar.bottom - 2 };
		CRect rect(topLeft, bottomRight);
		rect.NormalizeRect();
		CBrush buttonBrush(RGB(60, 60, 60));
		dc->FillRect(rect, &buttonBrush);
		dc->SetTextColor(RGB(0, 0, 0));
		dc->TextOutA(rect.left + 2, rect.top, letter.c_str());

		if (mouseWithin(mouseLocation, rect))
			dc->Draw3dRect(rect, RGB(45, 45, 45), RGB(75, 75, 75));
		else
			dc->Draw3dRect(rect, RGB(75, 75, 75), RGB(45, 45, 45));

		return rect;
	}
}

struct AvisoViewportState
{
	~AvisoViewportState()
	{
		ClearCache();
	}

	void ClearCache()
	{
		if (cacheBitmap != nullptr)
		{
			::DeleteObject(cacheBitmap);
			cacheBitmap = nullptr;
		}

		cachePath.clear();
		cacheWidth = 0;
		cacheHeight = 0;
		displayMinLongitude = 0.0;
		displayMinLatitude = 0.0;
		displayMaxLongitude = 0.0;
		displayMaxLatitude = 0.0;
		renderMinLongitude = 0.0;
		renderMinLatitude = 0.0;
		renderMaxLongitude = 0.0;
		renderMaxLatitude = 0.0;
		anchorValid = false;
	}

	HBITMAP cacheBitmap = nullptr;
	string cachePath;
	int cacheWidth = 0;
	int cacheHeight = 0;
	double displayMinLongitude = 0.0;
	double displayMinLatitude = 0.0;
	double displayMaxLongitude = 0.0;
	double displayMaxLatitude = 0.0;
	double renderMinLongitude = 0.0;
	double renderMinLatitude = 0.0;
	double renderMaxLongitude = 0.0;
	double renderMaxLatitude = 0.0;
	bool anchorValid = false;
	bool renderPending = false;
	unsigned long long nextRequestId = 0;
	std::future<std::unique_ptr<CSMRRadar::AvisoRasterRenderResult>> renderFuture;
};

CInsetWindow::CInsetWindow(int Id)
{
	m_Id = Id;
	m_AvisoState = std::make_unique<AvisoViewportState>();
}

CInsetWindow::~CInsetWindow()
{
}

bool CInsetWindow::IsAvisoViewport() const
{
	return m_Mode == Mode::AvisoViewport;
}

void CInsetWindow::ClearAvisoViewportCache()
{
	if (m_AvisoState != nullptr)
		m_AvisoState->ClearCache();
}

void CInsetWindow::setAirport(string icao)
{
	this->icao = icao;
}

void CInsetWindow::OnClickScreenObject(const char * sItemString, POINT Pt, int Button)
{
	UNREFERENCED_PARAMETER(sItemString);
	UNREFERENCED_PARAMETER(Pt);
	UNREFERENCED_PARAMETER(Button);
}

bool CInsetWindow::OnMoveScreenObject(const char * sObjectId, POINT Pt, RECT Area, bool Released)
{
	if (strcmp(sObjectId, "window") == 0) {
		if (IsAvisoViewport())
		{
			if (!this->m_Grip)
			{
				m_OffsetDrag = Pt;
				m_AvisoDragStartLatitude = m_AvisoCenterLatitude;
				m_AvisoDragStartLongitude = m_AvisoCenterLongitude;
				m_Grip = true;
			}

			const int scale = max(1, m_AvisoScale);
			const double metersPerPixel = kAvisoMetersPerNm / static_cast<double>(scale);
			const double lonDegreesPerPixel = metersPerPixel / (kAvisoLonMetersPerDegree * AvisoCosLatitude(m_AvisoDragStartLatitude));
			const double latDegreesPerPixel = metersPerPixel / kAvisoLatMetersPerDegree;
			const int dragX = Pt.x - m_OffsetDrag.x;
			const int dragY = Pt.y - m_OffsetDrag.y;
			m_AvisoCenterLongitude = m_AvisoDragStartLongitude - (static_cast<double>(dragX) * lonDegreesPerPixel);
			m_AvisoCenterLatitude = ClampAvisoLatitude(m_AvisoDragStartLatitude + (static_cast<double>(dragY) * latDegreesPerPixel));

			if (Released)
				m_Grip = false;

			return true;
		}

		if (!this->m_Grip)
		{
			m_OffsetInit = m_Offset;
			m_OffsetDrag = Pt;
			m_Grip = true;
		}

		POINT maxoffset = { (m_Area.right - m_Area.left) / 2, (m_Area.bottom - (m_Area.top + 15)) / 2 };
		m_Offset.x = max(-maxoffset.x, min(maxoffset.x, m_OffsetInit.x + (Pt.x - m_OffsetDrag.x)));
		m_Offset.y = max(-maxoffset.y, min(maxoffset.y, m_OffsetInit.y + (Pt.y - m_OffsetDrag.y)));

		if (Released)
		{
			m_Grip = false;
		}
	}
	if (strcmp(sObjectId, "resize") == 0) {
		POINT TopLeft = { m_Area.left, m_Area.top };
		POINT BottomRight = { Area.right, Area.bottom };

		CRect newSize(TopLeft, BottomRight);
		newSize.NormalizeRect();

		if (newSize.Height() < 100) {
			newSize.top = m_Area.top;
			newSize.bottom = m_Area.bottom;
		}

		if (newSize.Width() < 300) {
			newSize.left = m_Area.left;
			newSize.right = m_Area.right;
		}

		m_Area = newSize;

		return Released;
	}
	if (strcmp(sObjectId, "topbar") == 0) {

		CRect appWindowRect(m_Area);
		appWindowRect.NormalizeRect();

		POINT TopLeft = { Area.left, Area.bottom + 1 };
		POINT BottomRight = { TopLeft.x + appWindowRect.Width(), TopLeft.y + appWindowRect.Height() };
		CRect newPos(TopLeft, BottomRight);
		newPos.NormalizeRect();

		m_Area = newPos;

		return Released;
	}

	if (sObjectId != nullptr && strcmp(sObjectId, "window") != 0 && strcmp(sObjectId, "resize") != 0 && strcmp(sObjectId, "topbar") != 0)
	{
		if (IsAvisoViewport())
			return true;

		string callsign = sObjectId;
		if (!callsign.empty())
		{
			POINT tagCenter{};
			const bool firstDragFrame = (!Released && m_TagBeingDragged != callsign);
			if (firstDragFrame)
			{
				POINT rectCenter{};
				auto fullRectIt = m_TagAreas.find(callsign);
				if (fullRectIt != m_TagAreas.end())
				{
					CRect fullRect = fullRectIt->second;
					fullRect.NormalizeRect();
					rectCenter = fullRect.CenterPoint();
				}
				else
				{
					CRect tagRect(Area);
					tagRect.NormalizeRect();
					rectCenter = tagRect.CenterPoint();
				}
				POINT offset = { rectCenter.x - Pt.x, rectCenter.y - Pt.y };
				m_TagDragOffsetFromCenter[callsign] = offset;
			}

			auto offsetIt = m_TagDragOffsetFromCenter.find(callsign);
			if (offsetIt != m_TagDragOffsetFromCenter.end())
			{
				tagCenter.x = Pt.x + offsetIt->second.x;
				tagCenter.y = Pt.y + offsetIt->second.y;
			}
			else
			{
				auto fullRectIt = m_TagAreas.find(callsign);
				if (fullRectIt != m_TagAreas.end())
				{
					CRect fullRect = fullRectIt->second;
					fullRect.NormalizeRect();
					tagCenter = fullRect.CenterPoint();
				}
				else
				{
					CRect tagRect(Area);
					tagRect.NormalizeRect();
					tagCenter = tagRect.CenterPoint();
				}
			}

			auto targetIt = m_TargetPoints.find(callsign);
			if (targetIt != m_TargetPoints.end())
			{
				POINT customTag = { tagCenter.x - targetIt->second.x, tagCenter.y - targetIt->second.y };
				m_TagOffsets[callsign] = customTag;

				double angle = fmod(atan2(double(customTag.y), double(customTag.x)) * 180.0 / 3.14159265358979323846, 360.0);
				if (angle < 0.0)
					angle += 360.0;
				m_TagAngles[callsign] = angle;
			}

			if (Released)
			{
				m_TagBeingDragged.clear();
				m_TagDragOffsetFromCenter.erase(callsign);
			}
			else
			{
				m_TagBeingDragged = callsign;
			}
		}
	}

	return true;
}

POINT CInsetWindow::projectPoint(CPosition pos)
{
	CRect areaRect(m_Area);
	areaRect.NormalizeRect();

	POINT refPt = areaRect.CenterPoint();
	refPt.x += m_Offset.x;
	refPt.y += m_Offset.y;

	POINT out = {0, 0};

	double dist = AptPositions[icao].DistanceTo(pos);
	double dir = TrueBearing(AptPositions[icao], pos);


	out.x = refPt.x + int(m_Scale * dist * sin(dir) + 0.5);
	out.y = refPt.y - int(m_Scale * dist * cos(dir) + 0.5);

	if (m_Rotation != 0)
	{
		return rotate_point(out, m_Rotation, refPt);
	} else
	{
		return out;
	}
}

void CInsetWindow::renderAvisoViewport(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	if (radar_screen == nullptr || gdi == nullptr || m_AvisoState == nullptr)
		return;

	CDC dc;
	dc.Attach(hDC);
	CRect viewportRect(m_Area);
	viewportRect.NormalizeRect();
	const int viewportWidth = viewportRect.Width();
	const int viewportHeight = viewportRect.Height();
	if (viewportWidth <= 0 || viewportHeight <= 0)
	{
		dc.Detach();
		return;
	}

	dc.FillSolidRect(viewportRect, RGB(10, 26, 38));
	radar_screen->AddScreenObject(m_Id, "window", m_Area, true, "");

	auto drawCenteredMessage = [&](const char* message)
	{
		COLORREF oldTextColor = dc.SetTextColor(RGB(180, 190, 195));
		const CSize messageSize = dc.GetTextExtent(message);
		dc.TextOutA(
			viewportRect.left + (viewportRect.Width() - messageSize.cx) / 2,
			viewportRect.top + (viewportRect.Height() - messageSize.cy) / 2,
			message);
		dc.SetTextColor(oldTextColor);
	};

	auto drawChrome = [&]()
	{
		POINT bottomRight = { m_Area.right, m_Area.bottom };
		POINT resizeTopLeft = { bottomRight.x - 10, bottomRight.y - 10 };
		CRect resizeArea(resizeTopLeft, bottomRight);
		resizeArea.NormalizeRect();
		CBrush resizeBrush(RGB(60, 60, 60));
		dc.FillRect(resizeArea, &resizeBrush);
		radar_screen->AddScreenObject(m_Id, "resize", resizeArea, true, "");
		dc.Draw3dRect(resizeArea, RGB(0, 0, 0), RGB(0, 0, 0));

		CBrush frameBrush(RGB(127, 122, 122));
		dc.FrameRect(viewportRect, &frameBrush);

		POINT topBarTopLeft = viewportRect.TopLeft();
		topBarTopLeft.y -= 15;
		POINT topBarBottomRight = { viewportRect.right, viewportRect.top };
		CRect topBar(topBarTopLeft, topBarBottomRight);
		topBar.NormalizeRect();
		dc.FillRect(topBar, &frameBrush);
		radar_screen->AddScreenObject(m_Id, "topbar", topBar, true, "");

		COLORREF oldTextColor = dc.SetTextColor(RGB(35, 35, 35));
		const string title = "AVISO";
		const CSize titleSize = dc.GetTextExtent(title.c_str());
		const int titleX = topBar.left + max(0, (topBar.Width() - titleSize.cx) / 2);
		const int titleY = topBar.bottom - titleSize.cy;
		dc.TextOutA(titleX, titleY, title.c_str());

		CRect rangeRect = DrawInsetToolbarButton(&dc, "Z", topBar, 29, mouseLocation);
		radar_screen->AddScreenObject(m_Id, "range", rangeRect, false, "");

		POINT closeTopLeft = { topBar.right - 16, topBar.top + 2 };
		POINT closeBottomRight = { topBar.right - 5, topBar.bottom - 2 };
		CRect closeRect(closeTopLeft, closeBottomRight);
		closeRect.NormalizeRect();
		CBrush closeBrush(RGB(60, 60, 60));
		dc.FillRect(closeRect, &closeBrush);
		CPen blackPen(PS_SOLID, 1, RGB(0, 0, 0));
		CPen* oldPen = dc.SelectObject(&blackPen);
		dc.MoveTo(closeRect.TopLeft());
		dc.LineTo(closeRect.BottomRight());
		dc.MoveTo({ closeRect.right - 1, closeRect.top });
		dc.LineTo({ closeRect.left - 1, closeRect.bottom });
		if (oldPen != nullptr)
			dc.SelectObject(oldPen);

		if (mouseWithin(mouseLocation, closeRect))
			dc.Draw3dRect(closeRect, RGB(45, 45, 45), RGB(75, 75, 75));
		else
			dc.Draw3dRect(closeRect, RGB(75, 75, 75), RGB(45, 45, 45));
		radar_screen->AddScreenObject(m_Id, "close", closeRect, false, "");
		dc.SetTextColor(oldTextColor);
	};

	const std::string airport = radar_screen->getActiveAirport();
	const std::string path = radar_screen->ResolveAvisoGeoJsonPathForAirport(airport);
	if (path.empty() ||
		!radar_screen->EnsureAvisoGeoJsonLoaded(path) ||
		(radar_screen->AvisoGeoJsonFeatures.empty() && radar_screen->AvisoGeoJsonLabels.empty()))
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}

	if (!m_AvisoViewInitialized)
	{
		CPosition airportPosition;
		if (radar_screen->TryGetActiveAirportPosition(airportPosition))
		{
			m_AvisoCenterLatitude = airportPosition.m_Latitude;
			m_AvisoCenterLongitude = airportPosition.m_Longitude;
		}
		else if (radar_screen->AvisoGeoJsonHasBounds)
		{
			m_AvisoCenterLatitude = (radar_screen->AvisoGeoJsonMinLatitude + radar_screen->AvisoGeoJsonMaxLatitude) * 0.5;
			m_AvisoCenterLongitude = (radar_screen->AvisoGeoJsonMinLongitude + radar_screen->AvisoGeoJsonMaxLongitude) * 0.5;
		}
		m_AvisoViewInitialized = true;
		m_AvisoCenterLatitude = ClampAvisoLatitude(m_AvisoCenterLatitude);
	}

	if (radar_screen->AvisoGeoJsonFeatureSnapshot == nullptr)
		radar_screen->AvisoGeoJsonFeatureSnapshot = std::make_shared<const std::vector<CSMRRadar::AvisoFeature>>(radar_screen->AvisoGeoJsonFeatures);
	if (radar_screen->AvisoGeoJsonLabelSnapshot == nullptr)
		radar_screen->AvisoGeoJsonLabelSnapshot = std::make_shared<const std::vector<CSMRRadar::AvisoLabel>>(radar_screen->AvisoGeoJsonLabels);
	if (radar_screen->AvisoGeoJsonFeatureSnapshot == nullptr || radar_screen->AvisoGeoJsonLabelSnapshot == nullptr)
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}

	if (m_AvisoState->renderPending &&
		m_AvisoState->renderFuture.valid() &&
		m_AvisoState->renderFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
	{
		std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> result = m_AvisoState->renderFuture.get();
		m_AvisoState->renderPending = false;
		if (result != nullptr && result->bitmap != nullptr)
		{
			m_AvisoState->ClearCache();
			m_AvisoState->cacheBitmap = result->bitmap;
			result->bitmap = nullptr;
			m_AvisoState->cachePath = result->path;
			m_AvisoState->cacheWidth = result->rasterWidth;
			m_AvisoState->cacheHeight = result->rasterHeight;
			m_AvisoState->displayMinLongitude = result->displayMinLongitude;
			m_AvisoState->displayMinLatitude = result->displayMinLatitude;
			m_AvisoState->displayMaxLongitude = result->displayMaxLongitude;
			m_AvisoState->displayMaxLatitude = result->displayMaxLatitude;
			m_AvisoState->renderMinLongitude = result->renderMinLongitude;
			m_AvisoState->renderMinLatitude = result->renderMinLatitude;
			m_AvisoState->renderMaxLongitude = result->renderMaxLongitude;
			m_AvisoState->renderMaxLatitude = result->renderMaxLatitude;
			m_AvisoState->anchorValid = true;
		}
	}

	const int scale = max(1, m_AvisoScale);
	const double metersPerPixel = kAvisoMetersPerNm / static_cast<double>(scale);
	const double lonDegreesPerPixel = metersPerPixel / (kAvisoLonMetersPerDegree * AvisoCosLatitude(m_AvisoCenterLatitude));
	const double latDegreesPerPixel = metersPerPixel / kAvisoLatMetersPerDegree;
	const double halfLonSpan = static_cast<double>(viewportWidth) * lonDegreesPerPixel * 0.5;
	const double halfLatSpan = static_cast<double>(viewportHeight) * latDegreesPerPixel * 0.5;
	const double displayMinLon = m_AvisoCenterLongitude - halfLonSpan;
	const double displayMaxLon = m_AvisoCenterLongitude + halfLonSpan;
	const double displayMinLat = ClampAvisoLatitude(m_AvisoCenterLatitude - halfLatSpan);
	const double displayMaxLat = ClampAvisoLatitude(m_AvisoCenterLatitude + halfLatSpan);
	const double lonSpan = displayMaxLon - displayMinLon;
	const double latSpan = displayMaxLat - displayMinLat;
	if (lonSpan <= 0.0 || latSpan <= 0.0)
	{
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}

	const double scaleX = static_cast<double>(viewportWidth) / lonSpan;
	const double scaleY = static_cast<double>(viewportHeight) / latSpan;
	auto projectPoint = [&](double longitude, double latitude) -> Gdiplus::PointF
	{
		const double x = static_cast<double>(viewportRect.left) + ((longitude - displayMinLon) * scaleX);
		const double y = static_cast<double>(viewportRect.top) + ((displayMaxLat - latitude) * scaleY);
		return Gdiplus::PointF(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y));
	};

	auto drawCache = [&]() -> bool
	{
		if (m_AvisoState->cacheBitmap == nullptr ||
			m_AvisoState->cachePath != path ||
			m_AvisoState->cacheWidth <= 0 ||
			m_AvisoState->cacheHeight <= 0 ||
			!m_AvisoState->anchorValid)
		{
			return false;
		}

		const Gdiplus::PointF destTopLeft = projectPoint(m_AvisoState->renderMinLongitude, m_AvisoState->renderMaxLatitude);
		const Gdiplus::PointF destBottomRight = projectPoint(m_AvisoState->renderMaxLongitude, m_AvisoState->renderMinLatitude);
		const double destX = min(static_cast<double>(destTopLeft.X), static_cast<double>(destBottomRight.X));
		const double destY = min(static_cast<double>(destTopLeft.Y), static_cast<double>(destBottomRight.Y));
		const double destRight = max(static_cast<double>(destTopLeft.X), static_cast<double>(destBottomRight.X));
		const double destBottom = max(static_cast<double>(destTopLeft.Y), static_cast<double>(destBottomRight.Y));
		const double destWidth = destRight - destX;
		const double destHeight = destBottom - destY;
		if (destWidth < 1.0 || destHeight < 1.0)
			return false;

		const double visibleLeft = max(destX, static_cast<double>(viewportRect.left));
		const double visibleTop = max(destY, static_cast<double>(viewportRect.top));
		const double visibleRight = min(destRight, static_cast<double>(viewportRect.right));
		const double visibleBottom = min(destBottom, static_cast<double>(viewportRect.bottom));
		const double visibleWidth = visibleRight - visibleLeft;
		const double visibleHeight = visibleBottom - visibleTop;
		if (visibleWidth < 1.0 || visibleHeight < 1.0)
			return false;

		const double sourceScaleX = static_cast<double>(m_AvisoState->cacheWidth) / destWidth;
		const double sourceScaleY = static_cast<double>(m_AvisoState->cacheHeight) / destHeight;
		const double sourceX = (visibleLeft - destX) * sourceScaleX;
		const double sourceY = (visibleTop - destY) * sourceScaleY;
		const double sourceWidth = visibleWidth * sourceScaleX;
		const double sourceHeight = visibleHeight * sourceScaleY;

		const int destLeft = static_cast<int>(std::floor(visibleLeft));
		const int destTop = static_cast<int>(std::floor(visibleTop));
		const int destRightInt = static_cast<int>(std::ceil(visibleRight));
		const int destBottomInt = static_cast<int>(std::ceil(visibleBottom));
		const int destWidthInt = destRightInt - destLeft;
		const int destHeightInt = destBottomInt - destTop;
		if (destWidthInt <= 0 || destHeightInt <= 0)
			return false;

		int sourceXInt = static_cast<int>(std::floor(sourceX));
		int sourceYInt = static_cast<int>(std::floor(sourceY));
		int sourceRightInt = static_cast<int>(std::ceil(sourceX + sourceWidth));
		int sourceBottomInt = static_cast<int>(std::ceil(sourceY + sourceHeight));
		sourceXInt = std::clamp(sourceXInt, 0, m_AvisoState->cacheWidth);
		sourceYInt = std::clamp(sourceYInt, 0, m_AvisoState->cacheHeight);
		sourceRightInt = std::clamp(sourceRightInt, sourceXInt, m_AvisoState->cacheWidth);
		sourceBottomInt = std::clamp(sourceBottomInt, sourceYInt, m_AvisoState->cacheHeight);
		const int sourceWidthInt = sourceRightInt - sourceXInt;
		const int sourceHeightInt = sourceBottomInt - sourceYInt;
		if (sourceWidthInt <= 0 || sourceHeightInt <= 0)
			return false;

		HDC sourceDc = ::CreateCompatibleDC(hDC);
		if (sourceDc == nullptr)
			return false;
		HGDIOBJ oldBitmap = ::SelectObject(sourceDc, m_AvisoState->cacheBitmap);
		if (oldBitmap == nullptr || oldBitmap == HGDI_ERROR)
		{
			::DeleteDC(sourceDc);
			return false;
		}

		gdi->Flush(Gdiplus::FlushIntentionFlush);
		const int savedDc = ::SaveDC(hDC);
		if (savedDc == 0)
		{
			::SelectObject(sourceDc, oldBitmap);
			::DeleteDC(sourceDc);
			return false;
		}

		::IntersectClipRect(hDC, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
		const bool nearNativeScale =
			std::abs(static_cast<double>(destWidthInt - sourceWidthInt)) <= 1.0 &&
			std::abs(static_cast<double>(destHeightInt - sourceHeightInt)) <= 1.0;
		const int oldStretchMode = ::SetStretchBltMode(hDC, nearNativeScale ? COLORONCOLOR : HALFTONE);
		if (!nearNativeScale)
			::SetBrushOrgEx(hDC, 0, 0, nullptr);

		BLENDFUNCTION blend = {};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;
		const BOOL blended = ::AlphaBlend(
			hDC,
			destLeft,
			destTop,
			destWidthInt,
			destHeightInt,
			sourceDc,
			sourceXInt,
			sourceYInt,
			sourceWidthInt,
			sourceHeightInt,
			blend);

		if (oldStretchMode != 0)
			::SetStretchBltMode(hDC, oldStretchMode);
		::RestoreDC(hDC, savedDc);
		::SelectObject(sourceDc, oldBitmap);
		::DeleteDC(sourceDc);
		return blended != FALSE;
	};

	auto cacheHasWorkingMargin = [&]() -> bool
	{
		if (m_AvisoState->cacheBitmap == nullptr ||
			m_AvisoState->cachePath != path ||
			!m_AvisoState->anchorValid)
		{
			return false;
		}

		const double cachedDisplayLonSpan = m_AvisoState->displayMaxLongitude - m_AvisoState->displayMinLongitude;
		const double cachedDisplayLatSpan = m_AvisoState->displayMaxLatitude - m_AvisoState->displayMinLatitude;
		if (cachedDisplayLonSpan <= 0.0 || cachedDisplayLatSpan <= 0.0)
			return false;

		const double lonScaleRatio = lonSpan / cachedDisplayLonSpan;
		const double latScaleRatio = latSpan / cachedDisplayLatSpan;
		if (lonScaleRatio < 0.985 || lonScaleRatio > 1.015 ||
			latScaleRatio < 0.985 || latScaleRatio > 1.015)
		{
			return false;
		}

		const double requiredLonMargin = lonSpan * 0.20;
		const double requiredLatMargin = latSpan * 0.20;
		return
			m_AvisoState->renderMinLongitude <= displayMinLon - requiredLonMargin &&
			m_AvisoState->renderMaxLongitude >= displayMaxLon + requiredLonMargin &&
			m_AvisoState->renderMinLatitude <= displayMinLat - requiredLatMargin &&
			m_AvisoState->renderMaxLatitude >= displayMaxLat + requiredLatMargin;
	};

	const bool cacheDrawn = drawCache();
	if (!cacheDrawn || !cacheHasWorkingMargin())
	{
		if (!m_AvisoState->renderPending)
		{
			const double overscanRatio = 0.65;
			const double renderMinLon = displayMinLon - (lonSpan * overscanRatio);
			const double renderMaxLon = displayMaxLon + (lonSpan * overscanRatio);
			const double renderMinLat = ClampAvisoLatitude(displayMinLat - (latSpan * overscanRatio));
			const double renderMaxLat = ClampAvisoLatitude(displayMaxLat + (latSpan * overscanRatio));
			const Gdiplus::PointF renderTopLeft = projectPoint(renderMinLon, renderMaxLat);
			const Gdiplus::PointF renderBottomRight = projectPoint(renderMaxLon, renderMinLat);
			const double renderScreenLeft = min(static_cast<double>(renderTopLeft.X), static_cast<double>(renderBottomRight.X));
			const double renderScreenTop = min(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderBottomRight.Y));
			const double renderScreenRight = max(static_cast<double>(renderTopLeft.X), static_cast<double>(renderBottomRight.X));
			const double renderScreenBottom = max(static_cast<double>(renderTopLeft.Y), static_cast<double>(renderBottomRight.Y));
			const double renderPixelWidth = renderScreenRight - renderScreenLeft;
			const double renderPixelHeight = renderScreenBottom - renderScreenTop;
			if (renderPixelWidth > 0.0 && renderPixelHeight > 0.0)
			{
				const double maxRasterSide = 4096.0;
				const double maxRasterPixels = 8000000.0;
				double rasterScale = 1.0;
				const double maxDimension = max(renderPixelWidth, renderPixelHeight);
				const double sideLimitedScale = maxRasterSide / maxDimension;
				if (sideLimitedScale > 0.0 && sideLimitedScale < rasterScale)
					rasterScale = sideLimitedScale;
				const double pixelLimitedScale = std::sqrt(maxRasterPixels / (renderPixelWidth * renderPixelHeight));
				if (pixelLimitedScale > 0.0 && pixelLimitedScale < rasterScale)
					rasterScale = pixelLimitedScale;
				rasterScale = std::clamp(rasterScale, 0.5, 1.0);

				CSMRRadar::AvisoRasterRenderRequest request;
				request.requestId = ++m_AvisoState->nextRequestId;
				request.path = path;
				request.features = radar_screen->AvisoGeoJsonFeatureSnapshot;
				request.labels = radar_screen->AvisoGeoJsonLabelSnapshot;
				request.rasterWidth = max(1, static_cast<int>((renderPixelWidth * rasterScale) + 0.5));
				request.rasterHeight = max(1, static_cast<int>((renderPixelHeight * rasterScale) + 0.5));
				request.rasterScale = rasterScale;
				request.displayMinLongitude = displayMinLon;
				request.displayMinLatitude = displayMinLat;
				request.displayMaxLongitude = displayMaxLon;
				request.displayMaxLatitude = displayMaxLat;
				request.renderMinLongitude = renderMinLon;
				request.renderMinLatitude = renderMinLat;
				request.renderMaxLongitude = renderMaxLon;
				request.renderMaxLatitude = renderMaxLat;
				request.renderScreenLeft = renderScreenLeft;
				request.renderScreenTop = renderScreenTop;
				request.scaleX = scaleX;
				request.scaleY = scaleY;
				request.projectedTopLeft = Gdiplus::PointF(static_cast<Gdiplus::REAL>(viewportRect.left), static_cast<Gdiplus::REAL>(viewportRect.top));
				request.projectedTopRight = Gdiplus::PointF(static_cast<Gdiplus::REAL>(viewportRect.right), static_cast<Gdiplus::REAL>(viewportRect.top));
				request.projectedBottomLeft = Gdiplus::PointF(static_cast<Gdiplus::REAL>(viewportRect.left), static_cast<Gdiplus::REAL>(viewportRect.bottom));
				request.projectedBottomRight = Gdiplus::PointF(static_cast<Gdiplus::REAL>(viewportRect.right), static_cast<Gdiplus::REAL>(viewportRect.bottom));

				m_AvisoState->renderPending = true;
				m_AvisoState->renderFuture = std::async(
					std::launch::async,
					[radar_screen, request]()
					{
						std::unique_ptr<CSMRRadar::AvisoRasterRenderResult> result = radar_screen->RenderAvisoGeoJsonRaster(request);
						try
						{
							radar_screen->RequestRefresh();
						}
						catch (...)
						{
						}
						return result;
					});
			}
		}
	}

	if (!cacheDrawn)
		drawCenteredMessage(m_AvisoState->renderPending ? "Rendering AVISO" : "AVISO unavailable");

	auto drawAircraft = [&]()
	{
		const int savedDc = ::SaveDC(hDC);
		if (savedDc == 0)
			return;

		::IntersectClipRect(hDC, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);

		auto pointInViewport = [&](const POINT& point) -> bool
		{
			return
				point.x >= viewportRect.left &&
				point.x <= viewportRect.right &&
				point.y >= viewportRect.top &&
				point.y <= viewportRect.bottom;
		};
		auto projectTargetPosition = [&](const CPosition& position) -> POINT
		{
			const Gdiplus::PointF projected = projectPoint(position.m_Longitude, position.m_Latitude);
			return {
				static_cast<LONG>(std::lround(static_cast<double>(projected.X))),
				static_cast<LONG>(std::lround(static_cast<double>(projected.Y)))
			};
		};
		auto positionNearViewport = [&](const CPosition& position) -> bool
		{
			const double lonMargin = lonSpan * 0.08;
			const double latMargin = latSpan * 0.08;
			return
				position.m_Longitude >= displayMinLon - lonMargin &&
				position.m_Longitude <= displayMaxLon + lonMargin &&
				position.m_Latitude >= displayMinLat - latMargin &&
				position.m_Latitude <= displayMaxLat + latMargin;
		};
		auto drawTrailPoint = [&](const POINT& point, COLORREF color)
		{
			if (!pointInViewport(point))
				return;
			CRect trailRect(point.x - 1, point.y - 1, point.x + 2, point.y + 2);
			dc.FillSolidRect(trailRect, color);
		};

		const Gdiplus::Color symbolColor = radar_screen->ColorManager != nullptr
			? radar_screen->ColorManager->get_corrected_color("symbol", Gdiplus::Color::White)
			: Gdiplus::Color::White;
		const COLORREF symbolColorRef = symbolColor.ToCOLORREF();
		const COLORREF selectedColorRef = RGB(0, 220, 255);
		const COLORREF stageOneColorRef = RGB(210, 130, 30);
		const COLORREF stageTwoColorRef = RGB(220, 40, 40);

		CPen symbolPen(PS_SOLID, 1, symbolColorRef);
		CPen selectedPen(PS_SOLID, 1, selectedColorRef);
		CPen stageOnePen(PS_SOLID, 1, stageOneColorRef);
		CPen stageTwoPen(PS_SOLID, 1, stageTwoColorRef);
		CBrush symbolBrush(symbolColorRef);
		CBrush stageOneBrush(stageOneColorRef);
		CBrush stageTwoBrush(stageTwoColorRef);
		CBrush hollowBrush;
		hollowBrush.CreateStockObject(HOLLOW_BRUSH);

		CRadarTarget aselTarget = radar_screen->GetPlugIn()->RadarTargetSelectASEL();
		const char* aselCallsign = aselTarget.IsValid() ? aselTarget.GetCallsign() : nullptr;
		const int maxTrailDots = max(0, min(6, max(radar_screen->Trail_Gnd, radar_screen->Trail_App)));

		for (CRadarTarget rt = radar_screen->GetPlugIn()->RadarTargetSelectFirst();
			rt.IsValid();
			rt = radar_screen->GetPlugIn()->RadarTargetSelectNext(rt))
		{
			if (!rt.IsValid() || !rt.GetPosition().IsValid() || !radar_screen->isVisible(rt))
				continue;

			const char* rtCallsignRaw = rt.GetCallsign();
			if (rtCallsignRaw == nullptr || rtCallsignRaw[0] == '\0')
				continue;
			const std::string rtCallsign = rtCallsignRaw;

			CRadarTargetPositionData rtPositionData = rt.GetPosition();
			const CPosition targetPosition = rtPositionData.GetPosition();
			if (!positionNearViewport(targetPosition))
				continue;

			const POINT targetPoint = projectTargetPosition(targetPosition);
			if (!pointInViewport(targetPoint))
				continue;

			const int reportedGs = rtPositionData.GetReportedGS();
			const int trailDots = reportedGs > 50
				? max(0, min(maxTrailDots, radar_screen->Trail_App))
				: max(0, min(maxTrailDots, radar_screen->Trail_Gnd));
			CRadarTargetPositionData previousPosition = rt.GetPreviousPosition(rtPositionData);
			for (int trailIndex = 0; trailIndex < trailDots && previousPosition.IsValid(); ++trailIndex)
			{
				const CPosition previous = previousPosition.GetPosition();
				if (positionNearViewport(previous))
					drawTrailPoint(projectTargetPosition(previous), RGB(180, 190, 190));
				previousPosition = rt.GetPreviousPosition(previousPosition);
			}

			CPen* targetPen = &symbolPen;
			CBrush* targetBrush = &symbolBrush;
			if (radar_screen->RimcasInstance != nullptr)
			{
				const CRimcas::RimcasAlertTypes alert = radar_screen->RimcasInstance->getAlert(rtCallsign);
				if (alert == CRimcas::StageTwo)
				{
					targetPen = &stageTwoPen;
					targetBrush = &stageTwoBrush;
				}
				else if (alert == CRimcas::StageOne)
				{
					targetPen = &stageOnePen;
					targetBrush = &stageOneBrush;
				}
			}

			CPen* oldPen = dc.SelectObject(targetPen);
			CBrush* oldBrush = dc.SelectObject(targetBrush);

			double headingDeg = static_cast<double>(rtPositionData.GetReportedHeadingTrueNorth());
			if (headingDeg < 0.0 || headingDeg >= 360.0)
				headingDeg = rt.GetTrackHeading();

			CPosition nosePosition = radar_screen->Haversine(targetPosition, headingDeg, 50.0);
			POINT nosePoint = projectTargetPosition(nosePosition);
			double headingRadians = atan2(
				static_cast<double>(nosePoint.y - targetPoint.y),
				static_cast<double>(nosePoint.x - targetPoint.x));
			if (!std::isfinite(headingRadians))
				headingRadians = -3.14159265358979323846 / 2.0;

			if (reportedGs > 3)
			{
				const double length = reportedGs > 50 ? 14.0 : 11.0;
				const double baseLength = length * 0.55;
				const double halfWidth = reportedGs > 50 ? 6.0 : 5.0;
				const double cosH = std::cos(headingRadians);
				const double sinH = std::sin(headingRadians);
				const double baseX = static_cast<double>(targetPoint.x) - (cosH * baseLength);
				const double baseY = static_cast<double>(targetPoint.y) - (sinH * baseLength);

				POINT aircraftPolygon[3] = {
					{
						static_cast<LONG>(std::lround(static_cast<double>(targetPoint.x) + (cosH * length))),
						static_cast<LONG>(std::lround(static_cast<double>(targetPoint.y) + (sinH * length)))
					},
					{
						static_cast<LONG>(std::lround(baseX + (sinH * halfWidth))),
						static_cast<LONG>(std::lround(baseY - (cosH * halfWidth)))
					},
					{
						static_cast<LONG>(std::lround(baseX - (sinH * halfWidth))),
						static_cast<LONG>(std::lround(baseY + (cosH * halfWidth)))
					}
				};
				dc.Polygon(aircraftPolygon, 3);
			}
			else
			{
				const int radius = 5;
				dc.Ellipse(targetPoint.x - radius, targetPoint.y - radius, targetPoint.x + radius, targetPoint.y + radius);
			}

			if (oldBrush != nullptr)
				dc.SelectObject(oldBrush);
			if (oldPen != nullptr)
				dc.SelectObject(oldPen);

			const bool isAsel = aselCallsign != nullptr && strcmp(aselCallsign, rtCallsign.c_str()) == 0;
			if (isAsel)
			{
				CPen* oldSelectedPen = dc.SelectObject(&selectedPen);
				CBrush* oldSelectedBrush = dc.SelectObject(&hollowBrush);
				dc.Ellipse(targetPoint.x - 10, targetPoint.y - 10, targetPoint.x + 10, targetPoint.y + 10);
				if (oldSelectedBrush != nullptr)
					dc.SelectObject(oldSelectedBrush);
				if (oldSelectedPen != nullptr)
					dc.SelectObject(oldSelectedPen);
			}

			const int hitSize = reportedGs > 3 ? 16 : 12;
			CRect targetArea(
				targetPoint.x - hitSize / 2,
				targetPoint.y - hitSize / 2,
				targetPoint.x + hitSize / 2,
				targetPoint.y + hitSize / 2);
			targetArea.NormalizeRect();
			radar_screen->AddScreenObject(
				DRAWING_AC_SYMBOL_APPWINDOW_BASE + (m_Id - APPWINDOW_BASE),
				rtCallsign.c_str(),
				targetArea,
				false,
				radar_screen->GetBottomLine(rtCallsign.c_str()).c_str());
		}

		::RestoreDC(hDC, savedDc);
	};

	if (cacheDrawn)
		drawAircraft();

	drawChrome();

	dc.Detach();
}

void CInsetWindow::render(HDC hDC, CSMRRadar * radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation, multimap<string, string> DistanceTools)
{
	if (this->m_Id == -1)
		return;

	if (IsAvisoViewport())
	{
		renderAvisoViewport(hDC, radar_screen, gdi, mouseLocation);
		return;
	}

	CDC dc;
	dc.Attach(hDC);

	struct Utils
	{
		static string getEnumString(CSMRRadar::TagTypes type) {
			if (type == CSMRRadar::TagTypes::Departure)
				return "departure";
			if (type == CSMRRadar::TagTypes::Arrival)
				return "arrival";
			if (type == CSMRRadar::TagTypes::Uncorrelated)
				return "uncorrelated";
			return "airborne";
		}
		static RECT GetAreaFromText(CDC * dc, string text, POINT Pos) {
			RECT Area = { Pos.x, Pos.y, Pos.x + dc->GetTextExtent(text.c_str()).cx, Pos.y + dc->GetTextExtent(text.c_str()).cy };
			return Area;
		}

		static RECT drawToolbarButton(CDC * dc, string letter, CRect TopBar, int left, POINT mouseLocation)
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

	icao = radar_screen->ActiveAirport;
	AptPositions = radar_screen->AirportPositions;
	m_TargetPoints.clear();
	m_TagAreas.clear();

	const Value& activeProfile = radar_screen->CurrentConfig->getActiveProfile();
	const auto getProfileObjectSection = [&](const char* key) -> const Value*
	{
		if (!activeProfile.IsObject() || !activeProfile.HasMember(key) || !activeProfile[key].IsObject())
			return nullptr;
		return &activeProfile[key];
	};
	const auto getSectionInt = [&](const Value* section, const char* key, int fallback) -> int
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsInt())
			return (*section)[key].GetInt();
		return fallback;
	};
	const auto getSectionDouble = [&](const Value* section, const char* key, double fallback) -> double
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsNumber())
			return (*section)[key].GetDouble();
		return fallback;
	};
	const auto getSectionBool = [&](const Value* section, const char* key, bool fallback) -> bool
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsBool())
			return (*section)[key].GetBool();
		return fallback;
	};
	const auto getSectionColorRef = [&](const Value* section, const char* key, const COLORREF fallback) -> COLORREF
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsObject())
			return radar_screen->CurrentConfig->getConfigColorRef((*section)[key]);
		return fallback;
	};
	const auto getSectionColor = [&](const Value* section, const char* key, const Color& fallback) -> Color
	{
		if (section != nullptr && section->HasMember(key) && (*section)[key].IsObject())
			return radar_screen->CurrentConfig->getConfigColor((*section)[key]);
		return fallback;
	};

	const Value* approachInsetSection = getProfileObjectSection("approach_insets");
	const Value* filterSection = getProfileObjectSection("filters");
	const Value* rimcasSection = getProfileObjectSection("rimcas");
	const Value* labelsSection = getProfileObjectSection("labels");

	const COLORREF qBackgroundColor = getSectionColorRef(approachInsetSection, "background_color", RGB(30, 30, 30));
	const COLORREF approachRunwayColor = getSectionColorRef(approachInsetSection, "runway_color", RGB(255, 255, 255));
	const COLORREF approachExtendedLineColor = getSectionColorRef(approachInsetSection, "extended_lines_color", RGB(180, 180, 180));
	const double approachExtendedLineLengthNm = max(0.1, getSectionDouble(approachInsetSection, "extended_lines_length", 15.0));
	const int approachExtendedLineTickSpacingNm = max(1, getSectionInt(approachInsetSection, "extended_lines_ticks_spacing", 1));
	const int radarRangeNm = max(1, getSectionInt(filterSection, "radar_range_nm", 999));
	const bool rimcasLabelOnlySetting = getSectionBool(rimcasSection, "rimcas_label_only", true);
	const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
	const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));
	const Color squawkErrorLabelColor = getSectionColor(labelsSection, "squawk_error_color", Color(255, 255, 0, 0));

	CRect windowAreaCRect(m_Area);
	windowAreaCRect.NormalizeRect();

	// We create the radar
	dc.FillSolidRect(windowAreaCRect, qBackgroundColor);
	radar_screen->AddScreenObject(m_Id, "window", m_Area, true, "");

	auto scale = m_Scale;

	POINT refPt = windowAreaCRect.CenterPoint();
	refPt.x += m_Offset.x;
	refPt.y += m_Offset.y;

	// Here we draw all runways for the airport
	CSectorElement rwy;
	for (rwy = radar_screen->GetPlugIn()->SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
		rwy.IsValid();
		rwy = radar_screen->GetPlugIn()->SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{

		if (startsWith(icao.c_str(), rwy.GetAirportName()))
		{

			CPen RunwayPen(PS_SOLID, 1, approachRunwayColor);
			CPen ExtendedCentreLinePen(PS_SOLID, 1, approachExtendedLineColor);
			CPen* oldPen = dc.SelectObject(&RunwayPen);

			CPosition EndOne, EndTwo;
			rwy.GetPosition(&EndOne, 0);
			rwy.GetPosition(&EndTwo, 1);

			POINT Pt1, Pt2;
			Pt1 = projectPoint(EndOne);
			Pt2 = projectPoint(EndTwo);

			POINT toDraw1, toDraw2;
			if (LiangBarsky(m_Area, Pt1, Pt2, toDraw1, toDraw2)) {
				dc.MoveTo(toDraw1);
				dc.LineTo(toDraw2);

			}

			if (rwy.IsElementActive(false, 0) || rwy.IsElementActive(false, 1))
			{
				CPosition Threshold, OtherEnd;
				if (rwy.IsElementActive(false, 0))
				{
					Threshold = EndOne; 
					OtherEnd = EndTwo;
				} else
				{
					Threshold = EndTwo; 
					OtherEnd = EndOne;
				}
					

				double reverseHeading = RadToDeg(TrueBearing(OtherEnd, Threshold));
				double lenght = approachExtendedLineLengthNm * 1852.0;

				// Drawing the extended centreline
				CPosition endExtended = BetterHarversine(Threshold, reverseHeading, lenght);

				Pt1 = projectPoint(Threshold);
				Pt2 = projectPoint(endExtended);

				if (LiangBarsky(m_Area, Pt1, Pt2, toDraw1, toDraw2)) {
					dc.SelectObject(&ExtendedCentreLinePen);
					dc.MoveTo(toDraw1);
					dc.LineTo(toDraw2);
				}

				// Drawing the ticks
				int increment = approachExtendedLineTickSpacingNm * 1852;

				for (int j = increment; j <= int(approachExtendedLineLengthNm * 1852); j += increment) {

					CPosition tickPosition = BetterHarversine(Threshold, reverseHeading, j);
					CPosition tickBottom = BetterHarversine(tickPosition, fmod(reverseHeading - 90, 360), 500);
					CPosition tickTop = BetterHarversine(tickPosition, fmod(reverseHeading + 90, 360), 500);


					Pt1 = projectPoint(tickBottom);
					Pt2 = projectPoint(tickTop);

					if (LiangBarsky(m_Area, Pt1, Pt2, toDraw1, toDraw2)) {
						dc.SelectObject(&ExtendedCentreLinePen);
						dc.MoveTo(toDraw1);
						dc.LineTo(toDraw2);
					}

				}
			} 

			dc.SelectObject(&oldPen);
		}
	}

	// Aircrafts

	vector<POINT> appAreaVect = { windowAreaCRect.TopLeft(),{ windowAreaCRect.right, windowAreaCRect.top }, windowAreaCRect.BottomRight(),{ windowAreaCRect.left, windowAreaCRect.bottom } };
	CPen WhitePen(PS_SOLID, 1, radar_screen->ColorManager->get_corrected_color("symbol", Color::White).ToCOLORREF());
	const CSMRRadar::CorrelationSettings insetCorrelationSettings = radar_screen->BuildCorrelationSettings();
	const int insetTransitionAltitude = radar_screen->GetPlugIn()->GetTransitionAltitude();
	const std::string insetActiveAirport = radar_screen->getActiveAirport();
	bool insetTagProModeEnabled = false;
	bool insetUseAspeedForGate = false;
	bool insetAirborneUseDepartureArrivalColoring = false;
	if (radar_screen->CurrentConfig != nullptr)
	{
		const Value& profile = radar_screen->CurrentConfig->getActiveProfile();
		if (profile.IsObject())
		{
			if (profile.HasMember("filters") &&
				profile["filters"].IsObject() &&
				profile["filters"].HasMember("pro_mode") &&
				profile["filters"]["pro_mode"].IsObject())
			{
				const Value& proMode = profile["filters"]["pro_mode"];
				if (proMode.HasMember("enabled") && proMode["enabled"].IsBool())
				{
					insetTagProModeEnabled = proMode["enabled"].GetBool();
				}
				else if (proMode.HasMember("enable") && proMode["enable"].IsBool())
				{
					insetTagProModeEnabled = proMode["enable"].GetBool();
				}
			}

			if (profile.HasMember("labels") &&
				profile["labels"].IsObject())
			{
				const Value& labels = profile["labels"];
				if (labels.HasMember("use_speed_for_gate") && labels["use_speed_for_gate"].IsBool())
				{
					insetUseAspeedForGate = labels["use_speed_for_gate"].GetBool();
				}
				else if (labels.HasMember("use_aspeed_for_gate") && labels["use_aspeed_for_gate"].IsBool())
				{
					insetUseAspeedForGate = labels["use_aspeed_for_gate"].GetBool();
				}

				if (labels.HasMember("use_departure_arrival_coloring") && labels["use_departure_arrival_coloring"].IsBool())
				{
					insetAirborneUseDepartureArrivalColoring = labels["use_departure_arrival_coloring"].GetBool();
				}
				else if (labels.HasMember("airborne") &&
					labels["airborne"].IsObject() &&
					labels["airborne"].HasMember("use_departure_arrival_coloring") &&
					labels["airborne"]["use_departure_arrival_coloring"].IsBool())
				{
					insetAirborneUseDepartureArrivalColoring = labels["airborne"]["use_departure_arrival_coloring"].GetBool();
				}
			}
		}
	}

	CRadarTarget aselTarget = radar_screen->GetPlugIn()->RadarTargetSelectASEL();
	CRadarTarget rt;
	for (rt = radar_screen->GetPlugIn()->RadarTargetSelectFirst();
		rt.IsValid();
		rt = radar_screen->GetPlugIn()->RadarTargetSelectNext(rt))
	{
		const char* rtCallsign = rt.GetCallsign();
		if (rtCallsign == nullptr || rtCallsign[0] == '\0')
			continue;
		const char* aselCallsign = aselTarget.IsValid() ? aselTarget.GetCallsign() : nullptr;
		bool isASEL = (aselCallsign != nullptr && strcmp(aselCallsign, rtCallsign) == 0);
		int radarRange = radarRangeNm;

		if (rt.GetGS() < 60 ||
			rt.GetPosition().GetPressureAltitude() > m_Filter ||
			!rt.IsValid() ||
			!rt.GetPosition().IsValid() ||
			rt.GetPosition().GetPosition().DistanceTo(AptPositions[icao]) > radarRange)
			continue;

		CPosition RtPos2 = rt.GetPosition().GetPosition();
		CRadarTargetPositionData RtPos = rt.GetPosition();
		auto fp = radar_screen->GetPlugIn()->FlightPlanSelect(rtCallsign);
		auto reportedGs = RtPos.GetReportedGS();
		const char* fpDestination = fp.IsValid() ? fp.GetFlightPlanData().GetDestination() : nullptr;
		const char* fpOrigin = fp.IsValid() ? fp.GetFlightPlanData().GetOrigin() : nullptr;
		const char* fpPlanType = fp.IsValid() ? fp.GetFlightPlanData().GetPlanType() : nullptr;

		// Filtering the targets

		POINT RtPoint, hPoint;

		RtPoint = projectPoint(RtPos2);

		CRadarTargetPositionData hPos = rt.GetPreviousPosition(rt.GetPosition());
		for (int i = 1; i < radar_screen->Trail_App; i++) {
			if (!hPos.IsValid())
				continue;

			hPoint = projectPoint(hPos.GetPosition());

			if (Is_Inside(hPoint, appAreaVect)) {
				dc.SetPixel(hPoint, radar_screen->ColorManager->get_corrected_color("symbol", Color::White).ToCOLORREF());
			}

			hPos = rt.GetPreviousPosition(hPos);
		}

		if (Is_Inside(RtPoint, appAreaVect)) {
			dc.SelectObject(&WhitePen);

			if (RtPos.GetTransponderC()) {
				dc.MoveTo({ RtPoint.x, RtPoint.y - 4 });
				dc.LineTo({ RtPoint.x - 4, RtPoint.y });
				dc.LineTo({ RtPoint.x, RtPoint.y + 4 });
				dc.LineTo({ RtPoint.x + 4, RtPoint.y });
				dc.LineTo({ RtPoint.x, RtPoint.y - 4 });
			}
			else {
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x - 4, RtPoint.y - 4);
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x + 4, RtPoint.y - 4);
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x - 4, RtPoint.y + 4);
				dc.MoveTo(RtPoint.x, RtPoint.y);
				dc.LineTo(RtPoint.x + 4, RtPoint.y + 4);
			}

			CRect TargetArea(RtPoint.x - 4, RtPoint.y - 4, RtPoint.x + 4, RtPoint.y + 4);
			TargetArea.NormalizeRect();
			radar_screen->AddScreenObject(DRAWING_AC_SYMBOL_APPWINDOW_BASE + (m_Id - APPWINDOW_BASE), rtCallsign, TargetArea, false, radar_screen->GetBottomLine(rtCallsign).c_str());
		}

		// Predicted Track Line
		// It starts 10 seconds away from the ac
		if (radar_screen->PredictedLength > 0) {
			double d = double(rt.GetPosition().GetReportedGS() * 0.514444) * 10;
			CPosition AwayBase = BetterHarversine(rt.GetPosition().GetPosition(), rt.GetTrackHeading(), d);

			d = double(rt.GetPosition().GetReportedGS() * 0.514444) * (radar_screen->PredictedLength * 60) - 10;
			CPosition PredictedEnd = BetterHarversine(AwayBase, rt.GetTrackHeading(), d);

			POINT liangOne, liangTwo;

			if (LiangBarsky(m_Area, projectPoint(AwayBase), projectPoint(PredictedEnd), liangOne, liangTwo))
			{
				dc.SelectObject(&WhitePen);
				dc.MoveTo(liangOne);
				dc.LineTo(liangTwo);
			}
		}

		if (mouseWithin(mouseLocation, { RtPoint.x - 4, RtPoint.y - 4, RtPoint.x + 4, RtPoint.y + 4 })) {
			dc.MoveTo(RtPoint.x, RtPoint.y - 6);
			dc.LineTo(RtPoint.x - 4, RtPoint.y - 10);
			dc.MoveTo(RtPoint.x, RtPoint.y - 6);
			dc.LineTo(RtPoint.x + 4, RtPoint.y - 10);

			dc.MoveTo(RtPoint.x, RtPoint.y + 6);
			dc.LineTo(RtPoint.x - 4, RtPoint.y + 10);
			dc.MoveTo(RtPoint.x, RtPoint.y + 6);
			dc.LineTo(RtPoint.x + 4, RtPoint.y + 10);

			dc.MoveTo(RtPoint.x - 6, RtPoint.y);
			dc.LineTo(RtPoint.x - 10, RtPoint.y - 4);
			dc.MoveTo(RtPoint.x - 6, RtPoint.y);
			dc.LineTo(RtPoint.x - 10, RtPoint.y + 4);

			dc.MoveTo(RtPoint.x + 6, RtPoint.y);
			dc.LineTo(RtPoint.x + 10, RtPoint.y - 4);
			dc.MoveTo(RtPoint.x + 6, RtPoint.y);
			dc.LineTo(RtPoint.x + 10, RtPoint.y + 4);
		}

		int lenght = 50;

		POINT TagCenter;
		m_TargetPoints[rtCallsign] = RtPoint;
		auto customTagOffsetIt = m_TagOffsets.find(rtCallsign);
		if (customTagOffsetIt != m_TagOffsets.end())
		{
			TagCenter.x = RtPoint.x + customTagOffsetIt->second.x;
			TagCenter.y = RtPoint.y + customTagOffsetIt->second.y;
		}
		else
		{
			if (m_TagAngles.find(rtCallsign) == m_TagAngles.end())
			{
				m_TagAngles[rtCallsign] = 45.0; // TODO: Not the best, ah well
			}

			TagCenter.x = long(RtPoint.x + float(lenght * cos(DegToRad(m_TagAngles[rtCallsign]))));
			TagCenter.y = long(RtPoint.y + float(lenght * sin(DegToRad(m_TagAngles[rtCallsign]))));
		}
		// Drawing the tags, what a mess

			// ----- Generating the replacing map -----
			map<string, string> TagReplacingMap = CSMRRadar::GenerateTagData(
				rt,
				fp,
				isASEL,
				radar_screen->IsCorrelatedWithSettings(fp, rt, insetCorrelationSettings),
				insetTagProModeEnabled,
				insetTransitionAltitude,
				insetUseAspeedForGate,
				icao);

		// ----- Generating the clickable map -----
		map<string, int> TagClickableMap;
		TagClickableMap[TagReplacingMap["callsign"]] = TAG_CITEM_CALLSIGN;
		TagClickableMap[TagReplacingMap["actype"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["sctype"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["sqerror"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["deprwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["seprwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["arvrwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["srvrwy"]] = TAG_CITEM_RWY;
		TagClickableMap[TagReplacingMap["gate"]] = TAG_CITEM_GATE;
		TagClickableMap[TagReplacingMap["sate"]] = TAG_CITEM_GATE;
		TagClickableMap[TagReplacingMap["flightlevel"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["gs"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["tendency"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["wake"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["tssr"]] = TAG_CITEM_NO;
		TagClickableMap[TagReplacingMap["sid"]] = TagClickableMap[TagReplacingMap["shid"]] = TAG_CITEM_SID;
		TagClickableMap[TagReplacingMap["origin"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["dest"]] = TAG_CITEM_FPBOX;
		TagClickableMap[TagReplacingMap["systemid"]] = TAG_CITEM_MANUALCORRELATE;
		TagClickableMap[TagReplacingMap["gstatus"]] = TAG_CITEM_GROUNDSTATUS;
		TagClickableMap[TagReplacingMap["uk_stand"]] = TAG_CITEM_UKSTAND;
		TagClickableMap[TagReplacingMap["remark"]] = TAG_CITEM_REMARK;
		TagClickableMap[TagReplacingMap["scratchpad"]] = TAG_CITEM_SCRATCHPAD;


		//
		// ----- Now the hard part, drawing (using gdi+) -------
		//	

		CSMRRadar::TagTypes TagType = CSMRRadar::TagTypes::Departure;
		CSMRRadar::TagTypes ColorTagType = CSMRRadar::TagTypes::Departure;

		if (fpDestination != nullptr && strcmp(fpDestination, insetActiveAirport.c_str()) == 0) {
				TagType = CSMRRadar::TagTypes::Arrival;
				ColorTagType = CSMRRadar::TagTypes::Arrival;
		}

			if (reportedGs > 50) {
				TagType = CSMRRadar::TagTypes::Airborne;

				// Is "use_departure_arrival_coloring" enabled? if not, then use the airborne colors
				const bool useDepArrColors = insetAirborneUseDepartureArrivalColoring;
				if (!useDepArrColors) {
					ColorTagType = CSMRRadar::TagTypes::Airborne;
				}
			}

		bool AcisCorrelated = radar_screen->IsCorrelatedWithSettings(radar_screen->GetPlugIn()->FlightPlanSelect(rtCallsign), rt, insetCorrelationSettings);
		if (!AcisCorrelated && reportedGs >= 3)
		{
			TagType = CSMRRadar::TagTypes::Uncorrelated;
			ColorTagType = CSMRRadar::TagTypes::Uncorrelated;
		}

		// First we need to figure out the tag size

		int TagWidth = 0, TagHeight = 0;
		RectF mesureRect;
		auto fontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
		Gdiplus::Font* tagRegularFont = (fontIt != radar_screen->customFonts.end()) ? fontIt->second.get() : nullptr;
		if (tagRegularFont == nullptr)
			continue;
		Gdiplus::Font* tagBoldFont = tagRegularFont;
		std::unique_ptr<Gdiplus::Font> tagBoldFontOwned;
		Gdiplus::FontFamily baseFamily;
		if (tagRegularFont->GetFamily(&baseFamily) == Gdiplus::Ok)
		{
			INT boldStyle = tagRegularFont->GetStyle() | Gdiplus::FontStyleBold;
			tagBoldFontOwned.reset(new Gdiplus::Font(&baseFamily, tagRegularFont->GetSize(), boldStyle, Gdiplus::UnitPixel));
			if (tagBoldFontOwned->GetLastStatus() == Gdiplus::Ok)
				tagBoldFont = tagBoldFontOwned.get();
		}

		gdi->MeasureString(L" ", wcslen(L" "), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);
		int blankWidth = (int)mesureRect.GetRight();

		mesureRect = RectF(0, 0, 0, 0);
		gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
			tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);
		int oneLineHeight = (int)mesureRect.GetBottom();
		if (tagBoldFont != nullptr && tagBoldFont != tagRegularFont)
		{
			RectF boldMeasureRect;
			gdi->MeasureString(L"AZERTYUIOPQSDFGHJKLMWXCVBN", wcslen(L"AZERTYUIOPQSDFGHJKLMWXCVBN"),
				tagBoldFont, PointF(0, 0), &Gdiplus::StringFormat(), &boldMeasureRect);
			oneLineHeight = max(oneLineHeight, (int)boldMeasureRect.GetBottom());
		}

		static const Value emptyObject(kObjectType);
		const Value& LabelsSettings = (labelsSection != nullptr) ? *labelsSection : emptyObject;
		auto hasStatusDefinition = [&](const std::string& typeKey, const char* statusKey) -> bool
		{
			if (statusKey == nullptr || statusKey[0] == '\0')
				return false;
			if (!LabelsSettings.HasMember(typeKey.c_str()) || !LabelsSettings[typeKey.c_str()].IsObject())
				return false;
			const Value& section = LabelsSettings[typeKey.c_str()];
			if (!section.HasMember("status_definitions") || !section["status_definitions"].IsObject())
				return false;
			return section["status_definitions"].HasMember(statusKey) && section["status_definitions"][statusKey].IsObject();
		};
		auto resolveDefinitionLines = [&](const std::string& typeKey, const char* statusKey) -> const Value*
		{
			if (!LabelsSettings.HasMember(typeKey.c_str()) || !LabelsSettings[typeKey.c_str()].IsObject())
				return nullptr;

			const Value& section = LabelsSettings[typeKey.c_str()];
			if (statusKey != nullptr &&
				section.HasMember("status_definitions") &&
				section["status_definitions"].IsObject() &&
				section["status_definitions"].HasMember(statusKey) &&
				section["status_definitions"][statusKey].IsObject() &&
				section["status_definitions"][statusKey].HasMember("definition") &&
				section["status_definitions"][statusKey]["definition"].IsArray())
			{
				return &section["status_definitions"][statusKey]["definition"];
			}

			if (section.HasMember("definition") && section["definition"].IsArray())
				return &section["definition"];
			return nullptr;
		};

		std::string definitionTypeKey = Utils::getEnumString(TagType);
		const char* definitionStatusKey = nullptr;
		if (TagReplacingMap["actype"] == "NoFPL" && (TagType == CSMRRadar::TagTypes::Departure || TagType == CSMRRadar::TagTypes::Arrival))
		{
			definitionStatusKey = "nofpl";
		}
		else if (TagType == CSMRRadar::TagTypes::Airborne)
		{
			bool isAirborneArrival = false;
			if (fpDestination != nullptr &&
				strcmp(fpDestination, radar_screen->getActiveAirport().c_str()) == 0)
			{
				isAirborneArrival = true;
			}
			definitionTypeKey = isAirborneArrival ? "arrival" : "departure";
			const char* airborneStatusKey = isAirborneArrival ? "airarr" : "airdep";
			const char* onRunwayStatusKey = isAirborneArrival ? "airarr_onrunway" : "airdep_onrunway";
			const bool targetOnRunway = radar_screen->RimcasInstance->isAcOnRunway(rtCallsign);
			if (targetOnRunway && hasStatusDefinition(definitionTypeKey, onRunwayStatusKey))
				definitionStatusKey = onRunwayStatusKey;
			else
				definitionStatusKey = airborneStatusKey;
		}

		const Value* labelLinesPtr = resolveDefinitionLines(definitionTypeKey, definitionStatusKey);
		if (labelLinesPtr == nullptr)
			continue;
		const Value& LabelLines = *labelLinesPtr;
		struct RenderedTagElement
		{
			std::string token;
			std::string text;
			bool bold = false;
			bool hasCustomColor = false;
			int colorR = 255;
			int colorG = 255;
			int colorB = 255;
			int measuredWidth = 0;
			int measuredHeight = 0;
			bool isClearanceToken = false;
		};
		vector<vector<RenderedTagElement>> ReplacedLabelLines;

		if (!LabelLines.IsArray())
			continue;

		for (unsigned int i = 0; i < LabelLines.Size(); i++)
		{
			const Value& line = LabelLines[i];
			vector<string> rawElements;
			if (line.IsArray())
			{
				for (unsigned int j = 0; j < line.Size(); j++)
				{
					if (line[j].IsString())
						rawElements.push_back(line[j].GetString());
				}
			}
			else if (line.IsString())
			{
				rawElements.push_back(line.GetString());
			}

			if (rawElements.empty())
				continue;

			vector<RenderedTagElement> renderedLine;
			renderedLine.reserve(rawElements.size());
			bool allEmpty = true;

			int TempTagWidth = 0;

			for (const std::string& rawElement : rawElements)
			{
				mesureRect = RectF(0, 0, 0, 0);
				DefinitionTokenStyleData styledToken = ParseDefinitionTokenStyle(rawElement);
				const std::string baseToken = styledToken.token.empty() ? rawElement : styledToken.token;
				string element;
				string clearanceNotClearedText;
				string clearanceClearedText;
				const bool isClearanceToken = TryParseClearanceTokenDisplay(baseToken, clearanceNotClearedText, clearanceClearedText);
				if (isClearanceToken)
				{
					if (fp.IsValid() && AcisCorrelated)
						element = fp.GetClearenceFlag() ? clearanceClearedText : clearanceNotClearedText;
					else
						element = "";
				}
				else
				{
					auto exactMatch = TagReplacingMap.find(baseToken);
					if (exactMatch != TagReplacingMap.end())
						element = exactMatch->second;
					else
					{
						element = baseToken;
						for (const auto& kv : TagReplacingMap)
						{
							if (element.find(kv.first) == std::string::npos)
								continue;
							replaceAll(element, kv.first, kv.second);
						}
					}
				}

				RenderedTagElement renderedElement;
				renderedElement.token = baseToken;
				renderedElement.text = element;
				renderedElement.bold = styledToken.bold;
				renderedElement.hasCustomColor = styledToken.hasCustomColor;
				renderedElement.colorR = styledToken.colorR;
				renderedElement.colorG = styledToken.colorG;
				renderedElement.colorB = styledToken.colorB;
				renderedElement.isClearanceToken = isClearanceToken;

				if (!element.empty())
				{
					allEmpty = false;
					wstring wstr = wstring(element.begin(), element.end());
					Gdiplus::Font* measureFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
					if (measureFont == nullptr)
						measureFont = tagRegularFont;
					gdi->MeasureString(wstr.c_str(), wcslen(wstr.c_str()),
						measureFont, PointF(0, 0), &Gdiplus::StringFormat(), &mesureRect);

					renderedElement.measuredWidth = (int)mesureRect.GetRight();
					renderedElement.measuredHeight = (int)mesureRect.GetBottom();
					TempTagWidth += renderedElement.measuredWidth;
				}

				renderedLine.push_back(std::move(renderedElement));
			}

			if (allEmpty)
				continue;

			if (!renderedLine.empty())
				TempTagWidth += (int)blankWidth * (int(renderedLine.size()) - 1);

			TagHeight += oneLineHeight;
			TagWidth = max(TagWidth, TempTagWidth);
			ReplacedLabelLines.push_back(std::move(renderedLine));
		}
		if (TagHeight > 0)
			TagHeight = TagHeight - 2;

		// Pfiou, done with that, now we can draw the actual rectangle.

		// We need to figure out if the tag color changes according to RIMCAS alerts, or not
		bool rimcasLabelOnly = rimcasLabelOnlySetting;

		const std::string colorTagTypeKey = Utils::getEnumString(ColorTagType);
		const Value* colorTagLabelSection = nullptr;
		if (LabelsSettings.HasMember(colorTagTypeKey.c_str()) && LabelsSettings[colorTagTypeKey.c_str()].IsObject())
			colorTagLabelSection = &LabelsSettings[colorTagTypeKey.c_str()];

		auto getColorFromSectionOrDefault = [&](const Value* section, const char* key, const Color& fallback) -> Color
		{
			if (section != nullptr && section->HasMember(key) && (*section)[key].IsObject())
				return radar_screen->CurrentConfig->getConfigColor((*section)[key]);
			return fallback;
		};
		auto getColorWithLegacy = [&](const char* preferredKey, const char* legacyKey, const Color& fallback) -> Color
		{
			if (colorTagLabelSection != nullptr &&
				colorTagLabelSection->HasMember(preferredKey) &&
				(*colorTagLabelSection)[preferredKey].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*colorTagLabelSection)[preferredKey]);
			}
			if (legacyKey != nullptr &&
				colorTagLabelSection != nullptr &&
				colorTagLabelSection->HasMember(legacyKey) &&
				(*colorTagLabelSection)[legacyKey].IsObject())
			{
				return radar_screen->CurrentConfig->getConfigColor((*colorTagLabelSection)[legacyKey]);
			}
			return fallback;
		};

		Color definedBackgroundColor = Color(255, 53, 126, 187);
		Color definedBackgroundOnRunwayColor = definedBackgroundColor;
		Color definedTextColor = Color::White;
		if (ColorTagType == CSMRRadar::TagTypes::Departure)
		{
			definedBackgroundColor = getColorWithLegacy("background_no_status_color", "gate_color", Color(255, 53, 126, 187));
			definedBackgroundOnRunwayColor = getColorWithLegacy("background_on_runway_color", "on_runway_color", definedBackgroundColor);
			definedTextColor = getColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else if (ColorTagType == CSMRRadar::TagTypes::Arrival)
		{
			definedBackgroundColor = getColorWithLegacy("background_on_ground_color", "background_color", Color(255, 191, 87, 91));
			definedBackgroundOnRunwayColor = getColorWithLegacy("background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else if (ColorTagType == CSMRRadar::TagTypes::Uncorrelated)
		{
			definedBackgroundColor = getColorWithLegacy("background_on_ground_color", "background_color", Color(255, 150, 22, 135));
			definedBackgroundOnRunwayColor = getColorWithLegacy("background_on_runway_color", "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getColorWithLegacy("text_on_ground_color", "text_color", Color::White);
		}
		else
		{
			definedBackgroundColor = getColorFromSectionOrDefault(colorTagLabelSection, "background_color", Color(255, 53, 126, 187));
			definedBackgroundOnRunwayColor = getColorFromSectionOrDefault(colorTagLabelSection, "background_color_on_runway", definedBackgroundColor);
			definedTextColor = getColorFromSectionOrDefault(colorTagLabelSection, "text_color", Color::White);
		}
		if (TagType == CSMRRadar::TagTypes::Departure) {
			if (!TagReplacingMap["sid"].empty() && radar_screen->CurrentConfig->isSidColorAvail(TagReplacingMap["sid"], radar_screen->getActiveAirport())) {
				definedBackgroundColor = radar_screen->CurrentConfig->getSidColor(TagReplacingMap["sid"], radar_screen->getActiveAirport());
			}

			if (fpPlanType != nullptr && fpPlanType[0] == 'I' && TagReplacingMap["asid"].empty()) {
				if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("background_no_sid_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["background_no_sid_color"]);
				else if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("nosid_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["nosid_color"]);
			}
		}
			if (TagReplacingMap["actype"] == "NoFPL") {
				if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("background_no_fpl_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["background_no_fpl_color"]);
				else if (LabelsSettings[Utils::getEnumString(ColorTagType).c_str()].HasMember("nofpl_color"))
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(LabelsSettings[Utils::getEnumString(ColorTagType).c_str()]["nofpl_color"]);
		}

		if (TagType == CSMRRadar::TagTypes::Airborne &&
			fp.IsValid() &&
			AcisCorrelated)
		{
			bool isAirborneDeparture = true;
			std::string originAirport = fpOrigin != nullptr ? fpOrigin : "";
			std::string activeAirportUpper = radar_screen->getActiveAirport();
			if (!originAirport.empty() && !activeAirportUpper.empty())
			{
				std::transform(originAirport.begin(), originAirport.end(), originAirport.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				std::transform(activeAirportUpper.begin(), activeAirportUpper.end(), activeAirportUpper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				isAirborneDeparture = (originAirport == activeAirportUpper);
			}

			const char* runwaySectionKey = isAirborneDeparture ? "departure" : "arrival";
			if (LabelsSettings.HasMember(runwaySectionKey) && LabelsSettings[runwaySectionKey].IsObject())
			{
				const Value& runwaySection = LabelsSettings[runwaySectionKey];
				if (runwaySection.HasMember("background_airborne_color") && runwaySection["background_airborne_color"].IsObject())
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["background_airborne_color"]);
				if (runwaySection.HasMember("text_airborne_color") && runwaySection["text_airborne_color"].IsObject())
					definedTextColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["text_airborne_color"]);

				const char* runwayColorKey = "background_on_runway_color";
				if (runwaySection.HasMember(runwayColorKey) && runwaySection[runwayColorKey].IsObject())
				{
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection[runwayColorKey]);
				}
				else if (isAirborneDeparture &&
					runwaySection.HasMember("on_runway_color") &&
					runwaySection["on_runway_color"].IsObject())
				{
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["on_runway_color"]);
				}
				else if (runwaySection.HasMember("background_color_on_runway") &&
					runwaySection["background_color_on_runway"].IsObject())
				{
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(runwaySection["background_color_on_runway"]);
				}
			}
			else if (LabelsSettings.HasMember("airborne") && LabelsSettings["airborne"].IsObject())
			{
				const Value& airborneLabel = LabelsSettings["airborne"];
				const char* bgKey = isAirborneDeparture ? "departure_background_color" : "arrival_background_color";
				const char* textKey = isAirborneDeparture ? "departure_text_color" : "arrival_text_color";
				if (airborneLabel.HasMember(bgKey) && airborneLabel[bgKey].IsObject())
					definedBackgroundColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgKey]);
				if (airborneLabel.HasMember(textKey) && airborneLabel[textKey].IsObject())
					definedTextColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[textKey]);
				const char* bgOnRunwayKey = isAirborneDeparture ? "departure_background_color_on_runway" : "arrival_background_color_on_runway";
				if (airborneLabel.HasMember(bgOnRunwayKey) && airborneLabel[bgOnRunwayKey].IsObject())
					definedBackgroundOnRunwayColor = radar_screen->CurrentConfig->getConfigColor(airborneLabel[bgOnRunwayKey]);
			}
		}

		Color TagBackgroundColor = radar_screen->RimcasInstance->GetAircraftColor(rtCallsign,
			definedBackgroundColor,
			definedBackgroundOnRunwayColor,
			rimcasStageOneColor,
			rimcasStageTwoColor);

		if (rimcasLabelOnly)
			TagBackgroundColor = radar_screen->RimcasInstance->GetAircraftColor(rtCallsign,
				definedBackgroundColor,
				definedBackgroundOnRunwayColor);

		CRect TagBackgroundRect(TagCenter.x - (TagWidth / 2), TagCenter.y - (TagHeight / 2), TagCenter.x + (TagWidth / 2), TagCenter.y + (TagHeight / 2));

		if (Is_Inside(TagBackgroundRect.TopLeft(), appAreaVect) &&
			Is_Inside(RtPoint, appAreaVect) &&
			Is_Inside(TagBackgroundRect.BottomRight(), appAreaVect)) {

			const int padding = 3;
			TagBackgroundRect = CRect(TagBackgroundRect.left - padding, TagBackgroundRect.top - padding, TagBackgroundRect.right + padding, TagBackgroundRect.bottom + padding);
			int textLeft = TagBackgroundRect.left + padding;
			int textTop = TagBackgroundRect.top + padding;
			int textWidth = max(0, TagBackgroundRect.Width() - (padding * 2));

			// Semi-transparent background to reduce clutter while keeping arrival/departure color coding (unless RIMCAS alert overrides the color).
			if (radar_screen->RimcasInstance->getAlert(rtCallsign) == CRimcas::NoAlert) {
				auto blend = [](Color a, Color b, float t) {
					auto mix = [t](BYTE c1, BYTE c2) -> BYTE {
						return static_cast<BYTE>(c1 * t + c2 * (1.0f - t));
					};
					return Color(mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
				};
				Color neutralBlue(0x6E, 0xA5, 0xA8);  // from palette
				Color neutralRed(0x4E, 0x4E, 0x68);   // from palette
				Color baseBlue(60, 120, 200);
				Color baseRed(200, 70, 80);

				if (ColorTagType == CSMRRadar::TagTypes::Departure) {
					Color mixed = blend(baseBlue, neutralBlue, 0.65f);
					TagBackgroundColor = Color(160, mixed.GetR(), mixed.GetG(), mixed.GetB());
				}
				else if (ColorTagType == CSMRRadar::TagTypes::Arrival) {
					Color mixed = blend(baseRed, neutralRed, 0.65f);
					TagBackgroundColor = Color(160, mixed.GetR(), mixed.GetG(), mixed.GetB());
				}
			}

			// Slightly enlarge tag hitbox and draw rounded background.
			auto MakeRoundedRect = [](GraphicsPath &path, Rect r, int radius) {
				path.Reset();
				int d = radius * 2;
				path.AddArc(r.X, r.Y, d, d, 180, 90);
				path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
				path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
				path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
				path.CloseFigure();
			};
			Rect RoundedRect = CopyRect(TagBackgroundRect);
			GraphicsPath roundedPath;
			MakeRoundedRect(roundedPath, RoundedRect, 4);

			SolidBrush TagBackgroundBrush(TagBackgroundColor);
			gdi->FillPath(&TagBackgroundBrush, &roundedPath);

			auto getRimcasEditorColor = [&](const char* key, const Color& fallback) -> Color
			{
				const Value& activeProfile = radar_screen->CurrentConfig->getActiveProfile();
				if (activeProfile.HasMember("rimcas") && activeProfile["rimcas"].IsObject())
				{
					const Value& rimcas = activeProfile["rimcas"];
					if (rimcas.HasMember(key) && rimcas[key].IsObject())
						return radar_screen->CurrentConfig->getConfigColor(rimcas[key]);
				}
				return fallback;
			};

			SolidBrush FontColor(radar_screen->ColorManager->get_corrected_color("label", definedTextColor));
			SolidBrush SquawkErrorColor(radar_screen->ColorManager->get_corrected_color("label",
				squawkErrorLabelColor));
			SolidBrush AlertTextColorCaution(radar_screen->ColorManager->get_corrected_color("label",
				getRimcasEditorColor("caution_alert_text_color", Color(255, 30, 30, 30))));
			SolidBrush AlertTextColorWarning(radar_screen->ColorManager->get_corrected_color("label",
				getRimcasEditorColor("warning_alert_text_color", Color(255, 255, 255, 255))));

			m_TagAreas[rtCallsign] = TagBackgroundRect;
			radar_screen->AddScreenObject(m_Id, rtCallsign, TagBackgroundRect, true, radar_screen->GetBottomLine(rtCallsign).c_str());

			int heightOffset = 0;
			for (auto&& line : ReplacedLabelLines)
			{
				int lineWidth = 0;
				for (auto&& renderedElement : line)
					lineWidth += renderedElement.measuredWidth;
				if (!line.empty())
					lineWidth += blankWidth * (int(line.size()) - 1);

				int widthOffset = max(0, (textWidth - lineWidth) / 2);
				for (auto&& renderedElement : line)
				{
					const std::string& element = renderedElement.text;
					const std::string& rawToken = renderedElement.token;
					Gdiplus::Font* drawFont = renderedElement.bold ? tagBoldFont : tagRegularFont;
					if (drawFont == nullptr)
						drawFont = tagRegularFont;

					SolidBrush* color = &FontColor;
					if (TagReplacingMap["sqerror"].size() > 0 && strcmp(element.c_str(), TagReplacingMap["sqerror"].c_str()) == 0)
						color = &SquawkErrorColor;

					CRimcas::RimcasAlertTypes rimcasStage = radar_screen->RimcasInstance->getAlert(rtCallsign);
					if (rimcasStage != CRimcas::NoAlert)
						color = (rimcasStage == CRimcas::StageTwo) ? &AlertTextColorWarning : &AlertTextColorCaution;

					std::unique_ptr<SolidBrush> tokenCustomColorBrush;
					if (renderedElement.hasCustomColor)
					{
						Color customColor = radar_screen->ColorManager->get_corrected_color("label",
							Color(255, renderedElement.colorR, renderedElement.colorG, renderedElement.colorB));
						tokenCustomColorBrush.reset(new SolidBrush(customColor));
						color = tokenCustomColorBrush.get();
					}

					wstring welement = wstring(element.begin(), element.end());
					int textOffsetY = max(0, (oneLineHeight - renderedElement.measuredHeight + 1) / 2);
					gdi->DrawString(welement.c_str(), wcslen(welement.c_str()), drawFont,
						PointF(Gdiplus::REAL(textLeft + widthOffset), Gdiplus::REAL(textTop + heightOffset + textOffsetY)),
						&Gdiplus::StringFormat(), color);

					int clickItemType = TAG_CITEM_NO;
					auto clickItemIt = TagClickableMap.find(element);
					if (clickItemIt != TagClickableMap.end())
						clickItemType = clickItemIt->second;
					if (renderedElement.isClearanceToken || IsClearanceDefinitionToken(rawToken))
						clickItemType = TAG_CITEM_CLEARANCE;

					int itemWidth = renderedElement.measuredWidth;
					int itemHeight = max(renderedElement.measuredHeight, oneLineHeight);
					if (itemWidth > 0 && itemHeight > 0)
					{
						CRect ItemRect(textLeft + widthOffset, textTop + heightOffset,
							textLeft + widthOffset + itemWidth, textTop + heightOffset + itemHeight);
						radar_screen->AddScreenObject(clickItemType, rtCallsign, ItemRect, true, radar_screen->GetBottomLine(rtCallsign).c_str());
					}

					widthOffset += renderedElement.measuredWidth;
					widthOffset += blankWidth;
				}

				heightOffset += oneLineHeight;
			}

			// Drawing the leader line
			RECT TagBackRectData = TagBackgroundRect;
			POINT toDraw1, toDraw2;
			if (LiangBarsky(TagBackRectData, RtPoint, TagBackgroundRect.CenterPoint(), toDraw1, toDraw2))
				gdi->DrawLine(&Pen(radar_screen->ColorManager->get_corrected_color("symbol", Color::White)), PointF(Gdiplus::REAL(RtPoint.x), Gdiplus::REAL(RtPoint.y)), PointF(Gdiplus::REAL(toDraw1.x), Gdiplus::REAL(toDraw1.y)));

			// If we use a RIMCAS label only, we display it, and adapt the rectangle
			CRect oldCrectSave = TagBackgroundRect;

			if (rimcasLabelOnly) {
				Color RimcasLabelColor = radar_screen->RimcasInstance->GetAircraftColor(rtCallsign, Color::AliceBlue, Color::AliceBlue,
					rimcasStageOneColor,
					rimcasStageTwoColor);

				if (RimcasLabelColor.ToCOLORREF() != Color(Color::AliceBlue).ToCOLORREF()) {
					int rimcas_height = 0;

					wstring wrimcas_height = wstring(L"ALERT");

					RectF RectRimcas_height;

					gdi->MeasureString(wrimcas_height.c_str(), wcslen(wrimcas_height.c_str()), tagRegularFont, PointF(0, 0), &Gdiplus::StringFormat(), &RectRimcas_height);
					rimcas_height = int(RectRimcas_height.GetBottom());

					// Drawing the rectangle

					CRect RimcasLabelRect(TagBackgroundRect.left, TagBackgroundRect.top - rimcas_height, TagBackgroundRect.right, TagBackgroundRect.top);
					gdi->FillRectangle(&SolidBrush(RimcasLabelColor), CopyRect(RimcasLabelRect));
					TagBackgroundRect.top -= rimcas_height;

					// Drawing the text

					wstring rimcasw = wstring(L"ALERT");
					StringFormat stformat;
					stformat.SetAlignment(StringAlignment::StringAlignmentCenter);
					SolidBrush* rimcasTextBrush = (radar_screen->RimcasInstance->getAlert(rtCallsign) == CRimcas::StageTwo)
						? &AlertTextColorWarning
						: &AlertTextColorCaution;
					gdi->DrawString(rimcasw.c_str(), wcslen(rimcasw.c_str()), tagRegularFont, PointF(Gdiplus::REAL((TagBackgroundRect.left + TagBackgroundRect.right) / 2), Gdiplus::REAL(TagBackgroundRect.top)), &stformat, rimcasTextBrush);

				}
			}

			// Adding the tag screen object

			//radar_screen->AddScreenObject(DRAWING_TAG, rt.GetCallsign(), TagBackgroundRect, true, GetBottomLine(rt.GetCallsign()).c_str());

			TagBackgroundRect = oldCrectSave;

			// Now adding the clickable zones
		}
	}

	// Distance tools here
	for (auto&& kv : DistanceTools)
	{
		CRadarTarget one = radar_screen->GetPlugIn()->RadarTargetSelect(kv.first.c_str());
		CRadarTarget two = radar_screen->GetPlugIn()->RadarTargetSelect(kv.second.c_str());

		int radarRange = radarRangeNm;

		if (one.GetGS() < 60 ||
			one.GetPosition().GetPressureAltitude() > m_Filter ||
			!one.IsValid() ||
			!one.GetPosition().IsValid() ||
			one.GetPosition().GetPosition().DistanceTo(AptPositions[icao]) > radarRange)
			continue;

		if (two.GetGS() < 60 ||
			two.GetPosition().GetPressureAltitude() > m_Filter ||
			!two.IsValid() ||
			!two.GetPosition().IsValid() ||
			two.GetPosition().GetPosition().DistanceTo(AptPositions[icao]) > radarRange)
			continue;

		CPen Pen(PS_SOLID, 1, RGB(255, 255, 255));
		CPen *oldPen = dc.SelectObject(&Pen);

		POINT onePoint = projectPoint(one.GetPosition().GetPosition());
		POINT twoPoint = projectPoint(two.GetPosition().GetPosition());

		POINT toDraw1, toDraw2;
		if (LiangBarsky(m_Area, onePoint, twoPoint, toDraw1, toDraw2)) {
			dc.MoveTo(toDraw1);
			dc.LineTo(toDraw2);
		}

		POINT TextPos = { twoPoint.x + 20, twoPoint.y };

		double Distance = one.GetPosition().GetPosition().DistanceTo(two.GetPosition().GetPosition());
		double Bearing = one.GetPosition().GetPosition().DirectionTo(two.GetPosition().GetPosition());

		string distances = std::to_string(Distance);
		size_t decimal_pos = distances.find(".");
		distances = distances.substr(0, decimal_pos + 2);

		string bearings = std::to_string(Bearing);
		decimal_pos = bearings.find(".");
		bearings = bearings.substr(0, decimal_pos + 2);

		string text = bearings;
		text += "° / ";
		text += distances;
		text += "nm";
		COLORREF old_color = dc.SetTextColor(RGB(0, 0, 0));

		CRect ClickableRect = { TextPos.x - 2, TextPos.y, TextPos.x + dc.GetTextExtent(text.c_str()).cx + 2, TextPos.y + dc.GetTextExtent(text.c_str()).cy };
		if (Is_Inside(ClickableRect.TopLeft(), appAreaVect) && Is_Inside(ClickableRect.BottomRight(), appAreaVect))
		{
			gdi->FillRectangle(&SolidBrush(Color(127, 122, 122)), CopyRect(ClickableRect));
			dc.Draw3dRect(ClickableRect, RGB(75, 75, 75), RGB(45, 45, 45));
			dc.TextOutA(TextPos.x, TextPos.y, text.c_str());

			radar_screen->AddScreenObject(RIMCAS_DISTANCE_TOOL, string(kv.first + "," + kv.second).c_str(), ClickableRect, false, "");
		}
		
		dc.SetTextColor(old_color);

		dc.SelectObject(oldPen);
	}

	// Resize square
	COLORREF resizeBackgroundColor = RGB(60, 60, 60);
	POINT BottomRight = { m_Area.right, m_Area.bottom };
	POINT TopLeft = { BottomRight.x - 10, BottomRight.y - 10 };
	CRect ResizeArea = { TopLeft, BottomRight };
	ResizeArea.NormalizeRect();
	dc.FillSolidRect(ResizeArea, resizeBackgroundColor);
	radar_screen->AddScreenObject(m_Id, "resize", ResizeArea, true, "");

	dc.Draw3dRect(ResizeArea, RGB(0, 0, 0), RGB(0, 0, 0));

	// Sides
	//CBrush FrameBrush(RGB(35, 35, 35));
	CBrush FrameBrush(RGB(127, 122, 122));
	COLORREF TopBarTextColor(RGB(35, 35, 35));
	dc.FrameRect(windowAreaCRect, &FrameBrush);

	// Topbar
	TopLeft = windowAreaCRect.TopLeft();
	TopLeft.y = TopLeft.y - 15;
	BottomRight = { windowAreaCRect.right, windowAreaCRect.top };
	CRect TopBar(TopLeft, BottomRight);
	TopBar.NormalizeRect();
	dc.FillRect(TopBar, &FrameBrush);
	POINT TopLeftText = { TopBar.left + 5, TopBar.bottom - dc.GetTextExtent("SRW 1").cy };
	COLORREF oldTextColorC = dc.SetTextColor(TopBarTextColor);

	radar_screen->AddScreenObject(m_Id, "topbar", TopBar, true, "");

	string Toptext = "SRW " + std::to_string(m_Id - APPWINDOW_BASE);
	dc.TextOutA(TopLeftText.x + (TopBar.right-TopBar.left) / 2 - dc.GetTextExtent("SRW 1").cx , TopLeftText.y, Toptext.c_str());

	// Range button
	CRect RangeRect = Utils::drawToolbarButton(&dc, "Z", TopBar, 29, mouseLocation);
	radar_screen->AddScreenObject(m_Id, "range", RangeRect, false, "");

	// Filter button
	CRect FilterRect = Utils::drawToolbarButton(&dc, "F", TopBar, 42, mouseLocation);
	radar_screen->AddScreenObject(m_Id, "filter", FilterRect, false, "");

	// Rotate button
	CRect RotateRect = Utils::drawToolbarButton(&dc, "R", TopBar, 55, mouseLocation);
	radar_screen->AddScreenObject(m_Id, "rotate", RotateRect, false, "");

	dc.SetTextColor(oldTextColorC);

	// Close
	POINT TopLeftClose = { TopBar.right - 16, TopBar.top + 2 };
	POINT BottomRightClose = { TopBar.right - 5, TopBar.bottom - 2 };
	CRect CloseRect(TopLeftClose, BottomRightClose);
	CloseRect.NormalizeRect();
	CBrush CloseBrush(RGB(60, 60, 60));
	dc.FillRect(CloseRect, &CloseBrush);
	CPen BlackPen(PS_SOLID, 1, RGB(0, 0, 0));
	dc.SelectObject(BlackPen);
	dc.MoveTo(CloseRect.TopLeft());
	dc.LineTo(CloseRect.BottomRight());
	dc.MoveTo({ CloseRect.right - 1, CloseRect.top });
	dc.LineTo({ CloseRect.left - 1, CloseRect.bottom });

	if (mouseWithin(mouseLocation, CloseRect))
		dc.Draw3dRect(CloseRect, RGB(45, 45, 45), RGB(75, 75, 75));
	else
		dc.Draw3dRect(CloseRect, RGB(75, 75, 75), RGB(45, 45, 45));

	radar_screen->AddScreenObject(m_Id, "close", CloseRect, false, "");

	dc.Detach();
}

