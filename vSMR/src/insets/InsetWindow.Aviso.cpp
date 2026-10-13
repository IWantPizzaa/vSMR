#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoRasterSizing.hpp"
#include "insets/InsetWindow.hpp"
#include "insets/AvisoViewportCenter.hpp"
#include "insets/InsetWindow.Internal.hpp"
#include "aviso/AvisoViewportRenderer.hpp"
#include "aviso/AvisoProjection.hpp"
#include "radar/RadarScreen.hpp"
#include "rendering/TagRenderer.hpp"
#include "rendering/TargetSymbolRenderer.hpp"
#include "rendering/NorthIndicator.hpp"
#include "rdf/RdfOverlay.hpp"
#include "shared/logging/Logger.hpp"
#include <chrono>

using VsmrRadarUiSupport::CopyRect;
using VsmrRadarUiSupport::DegToRad;
using VsmrRadarUiSupport::mouseWithin;
using VsmrInsetWindowInternal::AvisoCosLatitude;
using VsmrInsetWindowInternal::AvisoRectIntersects;
using VsmrInsetWindowInternal::ClampAvisoLatitude;
using VsmrInsetWindowInternal::DrawInsetWindowChrome;
using VsmrInsetWindowInternal::DrawRadarInsetBorder;
using VsmrInsetWindowInternal::ResolveAvisoViewportScreenRotationDeg;
using VsmrInsetWindowInternal::SceneColorToGdi;
using VsmrInsetWindowInternal::kAvisoLatMetersPerDegree;
using VsmrInsetWindowInternal::kAvisoLonMetersPerDegree;
using VsmrInsetWindowInternal::kAvisoMetersPerNm;

struct AvisoViewportState
{
	VsmrAviso::AvisoViewportRenderer native;
	double screenRotationDeg = 0.0;
	void ClearCache() { native.Clear(); }
	void ReleaseResources() { native.ReleaseResources(); }
	VsmrPerformance::AvisoQueueDepth PerformanceQueueDepth() const { return {}; }
};

CInsetWindow::CInsetWindow(int Id)
{
	m_Id = Id;
	m_AvisoState = std::make_unique<AvisoViewportState>();
}

CInsetWindow::~CInsetWindow()
{
	CancelAvisoViewportRender();
	ReleaseCachedFonts();
}

void CInsetWindow::ClearAvisoViewportCache()
{
	if (m_AvisoState != nullptr)
		m_AvisoState->ClearCache();
}

void CInsetWindow::InvalidateAvisoViewportRendering()
{
	ClearAvisoViewportCache();
}

VsmrPerformance::AvisoQueueDepth CInsetWindow::GetAvisoPerformanceQueueDepth()
{
	return m_AvisoState != nullptr
		? m_AvisoState->PerformanceQueueDepth()
		: VsmrPerformance::AvisoQueueDepth{};
}

std::size_t CInsetWindow::GetAvisoPerformanceBitmapCount(
	std::uint64_t* estimatedBytes) const
{
	if (estimatedBytes != nullptr)
		*estimatedBytes = 0;
	if (m_AvisoState == nullptr || m_AvisoState->native.Bitmap() == nullptr)
		return 0;
	if (estimatedBytes != nullptr)
		*estimatedBytes = m_AvisoState->native.BitmapBytes();
	return 1;
}

std::uint64_t CInsetWindow::GetAvisoPerformanceGpuImageBytes() const noexcept
{
	return m_AvisoState != nullptr ? m_AvisoState->native.EstimatedGpuImageBytes() : 0ULL;
}

void CInsetWindow::CancelAvisoViewportRender()
{
	if (m_AvisoState == nullptr)
		return;

	m_AvisoState->ReleaseResources();
}

double CInsetWindow::GetAvisoViewportScreenRotationDeg() const noexcept
{
	return m_AvisoState != nullptr ? m_AvisoState->screenRotationDeg : 0.0;
}

struct CInsetWindow::InsetAvisoView
{
	const CRect& viewportRect;
	const double& displayMinLon;
	const double& displayMaxLon;
	const double& displayMinLat;
	const double& displayMaxLat;
	const double& lonSpan;
	const double& latSpan;
	const VsmrAviso::InsetProjection& projection;
	Gdiplus::PointF projectPoint(double longitude, double latitude) const
	{
		const auto point = projection.Project({ longitude, latitude });
		return { static_cast<Gdiplus::REAL>(point.X), static_cast<Gdiplus::REAL>(point.Y) };
	}
};

struct CInsetWindow::AvisoTagTarget
{
	const VsmrScene::Target* target = nullptr;
	POINT point = {};
};

struct CInsetWindow::AvisoTagPass
{
	CSMRRadar* radar_screen;
	Gdiplus::Graphics* gdi;
	const CRect& viewportRect;
	VsmrTagRendering::FontContext& tagFonts;
	const Value& activeProfile;
	const std::set<std::string>& previouslyDetailed;
	POINT mouseLocation;
	bool hoverAllowed;
	bool roundedTagCornersEnabled;
	Color rimcasStageOneColor;
	Color rimcasStageTwoColor;
};

void CInsetWindow::renderAvisoTags(std::vector<AvisoTagTarget>& visibleTagTargets, const AvisoTagPass& pass)
{
	const auto& radar_screen = pass.radar_screen;
	const auto& gdi = pass.gdi;
	const auto& viewportRect = pass.viewportRect;
	const auto& previouslyDetailed = pass.previouslyDetailed;
	const auto& mouseLocation = pass.mouseLocation;
	const auto& hoverAllowed = pass.hoverAllowed;
	const auto& roundedTagCornersEnabled = pass.roundedTagCornersEnabled;
	const auto& rimcasStageOneColor = pass.rimcasStageOneColor;
	const auto& rimcasStageTwoColor = pass.rimcasStageTwoColor;
	auto& tagFonts = pass.tagFonts;
	auto clipToViewport = [&](CRect rect) -> CRect
	{
		rect.NormalizeRect();
		CRect clipped;
		clipped.IntersectRect(rect, viewportRect);
		return clipped;
	};
	auto pointInViewport = [&](const POINT& point, int margin = 0) -> bool
	{
		return
			point.x >= viewportRect.left - margin &&
			point.x <= viewportRect.right + margin &&
			point.y >= viewportRect.top - margin &&
			point.y <= viewportRect.bottom + margin;
	};
	auto rectIntersectsViewport = [&](const CRect& rect) -> bool
	{
		return AvisoRectIntersects(rect, viewportRect);
	};
	// Symbols are complete before tag layout begins. This makes tag z-order
	// independent of the scene target order and keeps every tag above aircraft.
	std::sort(
		visibleTagTargets.begin(),
		visibleTagTargets.end(),
		[](const AvisoTagTarget& left, const AvisoTagTarget& right)
		{
			return left.target->callsign < right.target->callsign;
		});

	bool autoDeconflictionEnabled = true;
	const Value* labelsSection = (pass.activeProfile.IsObject() && pass.activeProfile.HasMember("labels") && pass.activeProfile["labels"].IsObject() ? &pass.activeProfile["labels"] : nullptr);
	if (labelsSection != nullptr &&
		labelsSection->HasMember("auto_deconfliction") &&
		(*labelsSection)["auto_deconfliction"].IsBool())
	{
		autoDeconflictionEnabled = (*labelsSection)["auto_deconfliction"].GetBool();
	}

	struct PreparedTag
	{
		const VsmrScene::Target* target = nullptr;
		VsmrTagRendering::Layout layout;
		VsmrTagRendering::PaintOptions options;
	};
	std::vector<PreparedTag> preparedTags;
	std::vector<CRect> occupiedTagBounds;
	const int leaderLength = static_cast<int>(std::lround(50 * radar_screen->GetDisplayScale()));
	constexpr double angleStep = 22.5;
	for (const AvisoTagTarget& visible : visibleTagTargets)
	{
		const VsmrScene::Target& sceneTarget = *visible.target;
		const std::string& callsign = sceneTarget.callsign;
		VsmrTagRendering::Layout layout;
		if (!VsmrTagRendering::MeasureLayout(tagFonts, sceneTarget.tag.normal, layout))
		{
			VsmrScene::TagVariant fallback;
			VsmrScene::TagLine line;
			VsmrScene::TagElement element;
			const auto token = sceneTarget.tag.tokens.find("callsign");
			element.text = token != sceneTarget.tag.tokens.end() && !token->second.empty()
				? token->second
				: callsign;
			element.action = TAG_CITEM_NO;
			element.effectiveColor = sceneTarget.tag.normalPalette.text;
			line.elements.push_back(std::move(element));
			fallback.lines.push_back(std::move(line));
			if (!VsmrTagRendering::MeasureLayout(tagFonts, fallback, layout))
				continue;
		}

		VsmrTagRendering::PaintOptions options;
		options.displayScale = radar_screen->GetDisplayScale();
		options.targetPoint = visible.point;
		const VsmrScene::TagPalette& palette = sceneTarget.tag.normalPalette;
		options.background = SceneColorToGdi(
			sceneTarget.rimcas.onRunway ? palette.backgroundOnRunway : palette.background);
		options.leaderColor = Gdiplus::Color(255, 255, 255, 255);
		options.roundedCorners = roundedTagCornersEnabled;
		options.fitBackgroundToText = radar_screen->GetTagFitBackgroundToText();
		options.extendScratchpadHit = true;
		options.scratchpadAction = TAG_CITEM_SCRATCHPAD;
		options.centerLines = true;
		options.symmetricBounds = true;

		const auto customOffset = m_TagOffsets.find(callsign);
		const bool hasCustomOffset = customOffset != m_TagOffsets.end();
		double baseAngle = m_TagAngles.emplace(callsign, 45.0).first->second;
		int placementLeaderLength = leaderLength;
		if (hasCustomOffset)
		{
			placementLeaderLength = (std::max)(
				1,
				static_cast<int>(std::lround(std::hypot(
					static_cast<double>(customOffset->second.x),
					static_cast<double>(customOffset->second.y)))));
			baseAngle = VsmrRadarUiSupport::RadToDeg(std::atan2(
				static_cast<double>(customOffset->second.y),
				static_cast<double>(customOffset->second.x)));
			options.tagCenter = {
				visible.point.x + customOffset->second.x,
				visible.point.y + customOffset->second.y };
		}
		auto centerAtAngle = [&](double angle) -> POINT
		{
			return {
				static_cast<LONG>(visible.point.x + placementLeaderLength * cos(DegToRad(angle))),
				static_cast<LONG>(visible.point.y + placementLeaderLength * sin(DegToRad(angle))) };
		};
		if (!hasCustomOffset)
			options.tagCenter = centerAtAngle(baseAngle);
		auto collides = [&](const CRect& candidate) -> bool
		{
			CRect paddedCandidate(candidate);
			paddedCandidate.InflateRect(2, 2);
			for (const CRect& occupied : occupiedTagBounds)
			{
				CRect overlap;
				if (overlap.IntersectRect(paddedCandidate, occupied))
					return true;
			}
			return false;
		};

		if (autoDeconflictionEnabled)
		{
			double selectedAngle = baseAngle;
			for (int candidateIndex = 0; candidateIndex < 32; ++candidateIndex)
			{
				const int step = candidateIndex == 0
					? 0
					: ((candidateIndex + 1) / 2) * (candidateIndex % 2 == 1 ? 1 : -1);
				const double candidateAngle = baseAngle + step * angleStep;
				options.tagCenter = centerAtAngle(candidateAngle);
				const CRect candidateBounds =
					VsmrTagRendering::CalculateBounds(tagFonts, layout, options);
				if (!collides(candidateBounds))
				{
					selectedAngle = candidateAngle;
					break;
				}
			}
			selectedAngle = std::fmod(selectedAngle + 360.0, 360.0);
			if (!hasCustomOffset)
				m_TagAngles[callsign] = selectedAngle;
			options.tagCenter = centerAtAngle(selectedAngle);
		}

		const CRect expectedBounds =
			VsmrTagRendering::CalculateBounds(tagFonts, layout, options);
		if (!rectIntersectsViewport(expectedBounds) && !pointInViewport(visible.point, 20))
			continue;
		CRect occupiedBounds(expectedBounds);
		occupiedBounds.InflateRect(2, 2);
		occupiedTagBounds.push_back(occupiedBounds);
		// Use the normal footprint for placement so expansion cannot move the tag.
		const VsmrTagRendering::Layout normalLayout = layout;
		const bool dragged = m_TagBeingDragged == callsign;
		const bool detailed = VsmrTagRendering::SelectHoveredLayout(
			tagFonts, sceneTarget.tag, options, mouseLocation, hoverAllowed, dragged,
			previouslyDetailed.count(callsign) != 0, normalLayout, layout);
		if (detailed) m_DetailedTagCallsigns.insert(callsign);
		const auto& selectedPalette = detailed ? sceneTarget.tag.detailedPalette : sceneTarget.tag.normalPalette;
		options.background = SceneColorToGdi(sceneTarget.rimcas.onRunway ? selectedPalette.backgroundOnRunway : selectedPalette.background);
		options.highlighted = dragged || (hoverAllowed &&
			VsmrTagRendering::CalculateBounds(tagFonts, layout, options).PtInRect(mouseLocation));
		preparedTags.push_back({ &sceneTarget, std::move(layout), options });
	}

	for (const PreparedTag& prepared : preparedTags)
	{
		const VsmrScene::Target& sceneTarget = *prepared.target;
		const VsmrTagRendering::PaintResult painted =
			VsmrTagRendering::Paint(*gdi, tagFonts, prepared.layout, prepared.options);
		if (painted.bounds.IsRectEmpty())
			continue;

		m_TagAreas[sceneTarget.callsign] = painted.bounds;
		m_AvisoTagHits.Add(sceneTarget.callsign, DRAWING_TAG, painted.bounds, viewportRect);
		const CRect clippedTag = clipToViewport(painted.bounds);
		if (!clippedTag.IsRectEmpty())
		{
			radar_screen->AddScreenObject(
				m_Id,
				sceneTarget.callsign.c_str(),
				clippedTag,
				true,
				sceneTarget.bottomLine.c_str());
		}
		for (const VsmrTagRendering::HitRegion& hit : painted.hitRegions)
		{
			m_AvisoTagHits.Add(sceneTarget.callsign, hit.action, hit.area, viewportRect);
			const CRect clippedHit = clipToViewport(hit.area);
			if (!clippedHit.IsRectEmpty())
			{
				radar_screen->AddScreenObject(
					hit.action,
					sceneTarget.callsign.c_str(),
					clippedHit,
					true,
					sceneTarget.bottomLine.c_str());
			}
		}

		const CRimcas::RimcasAlertTypes stage =
			static_cast<CRimcas::RimcasAlertTypes>(sceneTarget.rimcas.alertStage);
		if (stage == CRimcas::StageOne || stage == CRimcas::StageTwo)
		{
			VsmrTagRendering::DetachedTopBand alertBand;
			alertBand.text = "ALERT";
			alertBand.background =
				stage == CRimcas::StageOne ? rimcasStageOneColor : rimcasStageTwoColor;
			alertBand.textColor = stage == CRimcas::StageTwo
				? Gdiplus::Color(255, 255, 255, 255)
				: Gdiplus::Color(255, 30, 30, 30);
			VsmrTagRendering::PaintDetachedTopBand(
				*gdi,
				tagFonts,
				painted.bounds,
				alertBand);
		}
	}

}

void CInsetWindow::renderAvisoAircraft(HDC hDC, CDC& dc, CSMRRadar* radar_screen,
	Gdiplus::Graphics* gdi, POINT mouseLocation, const InsetAvisoView& view)
{
	const auto& viewportRect = view.viewportRect;
	// A rotated viewport can show geographic points outside its unrotated
	// latitude/longitude rectangle. Invert the same expanded screen rectangle
	// used by the final point test before applying this cheap geographic filter.
	const auto targetBounds = view.projection.VisibleBounds(viewportRect.Width(), viewportRect.Height(), 180.0);

	const int savedDc = ::SaveDC(hDC);
	if (savedDc == 0)
		return;

	::IntersectClipRect(hDC, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
	Gdiplus::GraphicsState graphicsState = gdi->Save();
	gdi->SetClip(CopyRect(viewportRect), Gdiplus::CombineModeIntersect);
	m_TargetPoints.clear();
	m_TagAreas.clear();
	const auto previouslyDetailed = std::move(m_DetailedTagCallsigns);
	m_DetailedTagCallsigns.clear();
	const bool hoverAllowed = radar_screen->CanHoverTags(mouseLocation, this);

	auto pointInViewport = [&](const POINT& point, int margin = 0) -> bool
	{
		return
			point.x >= viewportRect.left - margin &&
			point.x <= viewportRect.right + margin &&
			point.y >= viewportRect.top - margin &&
			point.y <= viewportRect.bottom + margin;
	};
	auto clipToViewport = [&](CRect rect) -> CRect
	{
		rect.NormalizeRect();
		CRect clipped;
		clipped.IntersectRect(rect, viewportRect);
		return clipped;
	};
	auto projectTargetPosition = [&](const CPosition& position) -> POINT
	{
		const Gdiplus::PointF projected = view.projectPoint(position.m_Longitude, position.m_Latitude);
		return {
			static_cast<LONG>(std::lround(static_cast<double>(projected.X))),
			static_cast<LONG>(std::lround(static_cast<double>(projected.Y)))
		};
	};
	auto positionNearViewport = [&](const CPosition& position) -> bool
	{
		return
			position.m_Longitude >= targetBounds.minLongitude &&
			position.m_Longitude <= targetBounds.maxLongitude &&
			position.m_Latitude >= targetBounds.minLatitude &&
			position.m_Latitude <= targetBounds.maxLatitude;
	};

	static const Value emptyObject(kObjectType);
	const Value& activeProfile = (radar_screen->CurrentConfig != nullptr)
		? radar_screen->CurrentConfig->getActiveProfile()
		: emptyObject;
	auto getProfileObjectSection = [&](const char* key) -> const Value*
	{
		if (!activeProfile.IsObject() || !activeProfile.HasMember(key) || !activeProfile[key].IsObject())
			return nullptr;
		return &activeProfile[key];
	};
	auto getSectionColor = [&](const Value* section, const char* key, const Color& fallback) -> Color
	{
		if (radar_screen->CurrentConfig != nullptr &&
			section != nullptr &&
			section->HasMember(key) &&
			(*section)[key].IsObject())
		{
			return radar_screen->CurrentConfig->getConfigColor((*section)[key]);
		}
		return fallback;
	};
	const Value* rimcasSection = getProfileObjectSection("rimcas");
	const Color rimcasStageOneColor = getSectionColor(rimcasSection, "background_color_stage_one", Color(255, 160, 90, 30));
	const Color rimcasStageTwoColor = getSectionColor(rimcasSection, "background_color_stage_two", Color(255, 150, 0, 0));
	const VsmrScene::RadarScene* targetScene = radar_screen->GetCurrentRadarScene();
	const VsmrScene::TargetPresentation defaultTargetPresentation;
	const VsmrScene::TargetPresentation& targetPresentation = targetScene != nullptr
		? targetScene->targetPresentation
		: defaultTargetPresentation;
	const double pixPerMeter = (std::max)(
		0.0,
		(std::max)(1.0, m_AvisoScale) / kAvisoMetersPerNm);

	VsmrTargetRendering::FrameSettings targetSettings;
	targetSettings.presentation = targetPresentation;
	targetSettings.pixelsPerMeter = pixPerMeter;
	const auto targetSettingsProjectPoint = [&](const VsmrScene::GeoPoint& point) -> POINT
	{
		CPosition position;
		position.m_Latitude = point.latitude;
		position.m_Longitude = point.longitude;
		return projectTargetPosition(position);
	};
	const auto targetSettingsPointVisible = [&](const POINT& point, int margin) -> bool
	{
		return pointInViewport(point, margin);
	};
	targetSettings.iconCache = radar_screen->CreateTargetIconCacheCallbacks();
	VsmrTargetRendering::Frame targetRenderer(*gdi, std::move(targetSettings));
	VsmrTargetRendering::DrawOptions targetDrawOptions;
	// The shared renderer applies the presentation scale to this baseline hit size.
	targetDrawOptions.minimumHitSize = 18;

	CPen symbolPen(PS_SOLID, 1, RGB(255, 255, 255));

	auto tagFontIt = radar_screen->customFonts.find(radar_screen->currentFontSize);
	Gdiplus::Font* tagRegularFont =
		tagFontIt != radar_screen->customFonts.end() ? tagFontIt->second.get() : nullptr;
	VsmrTagRendering::FontContext tagFonts(*gdi, tagRegularFont, 2, &m_TagTextCache);
	const bool roundedTagCornersEnabled = radar_screen->GetTagRoundedCornersEnabledForEditor();

	const VsmrScene::RadarScene* radarScene = radar_screen->GetCurrentRadarScene();
	std::vector<AvisoTagTarget> visibleTagTargets;
	if (radarScene != nullptr)
	for (const VsmrScene::Target& sceneTarget : radarScene->targets)
	{
		if (!sceneTarget.iconVisible || !sceneTarget.position.valid)
			continue;
		const std::string& rtCallsign = sceneTarget.callsign;
		CPosition targetPosition;
		targetPosition.m_Latitude = sceneTarget.position.latitude;
		targetPosition.m_Longitude = sceneTarget.position.longitude;
		if (!positionNearViewport(targetPosition))
			continue;

		const POINT targetPoint = projectTargetPosition(targetPosition);
		if (!pointInViewport(targetPoint, 180))
			continue;

		const VsmrTargetRendering::DrawResult renderedTarget =
			targetRenderer.DrawTarget(sceneTarget, targetSettingsProjectPoint, targetSettingsPointVisible, targetDrawOptions);
		if (!renderedTarget.drawn)
			continue;

		if (mouseWithin(mouseLocation, { targetPoint.x - 5, targetPoint.y - 5, targetPoint.x + 5, targetPoint.y + 5 }))
		{
			CPen* oldPen = dc.SelectObject(&symbolPen);
			dc.MoveTo(targetPoint.x, targetPoint.y - 8);
			dc.LineTo(targetPoint.x - 6, targetPoint.y - 12);
			dc.MoveTo(targetPoint.x, targetPoint.y - 8);
			dc.LineTo(targetPoint.x + 6, targetPoint.y - 12);
			dc.MoveTo(targetPoint.x, targetPoint.y + 8);
			dc.LineTo(targetPoint.x - 6, targetPoint.y + 12);
			dc.MoveTo(targetPoint.x, targetPoint.y + 8);
			dc.LineTo(targetPoint.x + 6, targetPoint.y + 12);
			dc.MoveTo(targetPoint.x - 8, targetPoint.y);
			dc.LineTo(targetPoint.x - 12, targetPoint.y - 6);
			dc.MoveTo(targetPoint.x - 8, targetPoint.y);
			dc.LineTo(targetPoint.x - 12, targetPoint.y + 6);
			dc.MoveTo(targetPoint.x + 8, targetPoint.y);
			dc.LineTo(targetPoint.x + 12, targetPoint.y - 6);
			dc.MoveTo(targetPoint.x + 8, targetPoint.y);
			dc.LineTo(targetPoint.x + 12, targetPoint.y + 6);
			dc.SelectObject(oldPen);
		}

		CRect targetArea(renderedTarget.hitBounds);
		targetArea.NormalizeRect();
		const CRect clippedTargetArea = clipToViewport(targetArea);
		if (!clippedTargetArea.IsRectEmpty())
		{
			radar_screen->AddScreenObject(
				DRAWING_AC_SYMBOL_APPWINDOW_BASE + (m_Id - APPWINDOW_BASE),
				rtCallsign.c_str(),
				clippedTargetArea,
				false,
				sceneTarget.bottomLine.c_str());
		}

		m_TargetPoints[rtCallsign] = targetPoint;
		if (sceneTarget.tagVisible && tagFonts.IsValid())
			visibleTagTargets.push_back({ &sceneTarget, targetPoint });
	}


	// Keep the symbol frame and font context alive until tag painting completes.
	const AvisoTagPass tagPass{ radar_screen, gdi, viewportRect, tagFonts, activeProfile,
		previouslyDetailed, mouseLocation, hoverAllowed, roundedTagCornersEnabled,
		rimcasStageOneColor, rimcasStageTwoColor };
	renderAvisoTags(visibleTagTargets, tagPass);

	gdi->Restore(graphicsState);
	::RestoreDC(hDC, savedDc);
}

void CInsetWindow::renderAvisoViewport(HDC hDC, CSMRRadar* radar_screen, Gdiplus::Graphics* gdi, POINT mouseLocation)
{
	m_AvisoTagHits.Clear();
	if (radar_screen == nullptr || gdi == nullptr || m_AvisoState == nullptr)
		return;
	if (radar_screen->IsShutdownRequested())
		return;

	// ----- Preparing the viewport -----
	CDC dc;
	dc.Attach(hDC);
	CRect layoutBounds(radar_screen->GetRadarArea());
	CRect chatArea(radar_screen->GetChatArea());
	layoutBounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty())
		layoutBounds.bottom = chatArea.top;
	ApplyAvisoLayoutBounds(&layoutBounds);

	CRect viewportRect = GetWindowContentRect();
	viewportRect.NormalizeRect();
	const int viewportWidth = viewportRect.Width();
	const int viewportHeight = viewportRect.Height();
	const double labelVisibilityRatio = VsmrAviso::InsetLabelVisibilityRatio(
		viewportWidth, viewportHeight, layoutBounds.Width(), layoutBounds.Height());
	if (viewportWidth <= 0 || viewportHeight <= 0)
	{
		dc.Detach();
		return;
	}
	HWND renderWindow = ::WindowFromDC(hDC);
	if (renderWindow == nullptr || !::IsWindow(renderWindow))
		renderWindow = ::GetActiveWindow();
	UpdateAvisoScreenArea(renderWindow);

	radar_screen->AddScreenObject(m_Id, "window", viewportRect, false, "");
	radar_screen->AddScreenObject(m_Id, "viewport", viewportRect, false, "");

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
		CBrush frameBrush(RGB(5, 7, 8));
		dc.FrameRect(viewportRect, &frameBrush);
		DrawInsetWindowChrome(
			dc,
			radar_screen,
			m_Id,
			m_AvisoLayoutMode,
			m_Area,
			"AVISO",
			false,
			mouseLocation,
			true,
			radar_screen->GetUiColorTheme() == "day",
			&m_LastChromeRenderMilliseconds);
		DrawRadarInsetBorder(dc, m_AvisoLayoutMode, m_Area);
	};

	// ----- Loading the AVISO data -----
	const std::string airport = radar_screen->getActiveAirport();
	const std::string path = radar_screen->ResolveAvisoGeoJsonPathForAirport(airport);
	if (path.empty() ||
		!radar_screen->EnsureAvisoGeoJsonLoaded(path) ||
		(radar_screen->AvisoGeoJsonFeatureCount == 0 && radar_screen->AvisoGeoJsonLabelCount == 0))
	{
		radar_screen->DrawAvisoBackground(*gdi, viewportRect);
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}
	std::shared_ptr<const std::vector<CSMRRadar::AvisoFeature>> featureSnapshot;
	std::shared_ptr<const std::vector<CSMRRadar::AvisoLabel>> labelSnapshot;
	std::shared_ptr<const VsmrAviso::AvisoRenderScene> renderScene;
	std::shared_ptr<const std::unordered_map<std::string, bool>> groupVisibility;
	unsigned long long groupGeneration = 0;
	if (!radar_screen->GetAvisoRenderSnapshots(
		featureSnapshot,
		labelSnapshot,
		groupVisibility,
		groupGeneration,
		&renderScene))
	{
		radar_screen->DrawAvisoBackground(*gdi, viewportRect);
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}
	// Validate a restored center once per airport/source, not while the user pans.
	const std::string centerContext = airport + "|" + path;
	if (m_AvisoCenterContext != centerContext && radar_screen->AvisoGeoJsonHasBounds)
	{
		const double metersPerPixel = kAvisoMetersPerNm / (std::max)(1.0, m_AvisoScale);
		const double viewportRadiusMeters = std::hypot(
			static_cast<double>(viewportWidth), static_cast<double>(viewportHeight)) * metersPerPixel * 0.5;
		if (!VsmrAvisoViewportCenter::OverlapsAirport(
			m_AvisoCenterLatitude, m_AvisoCenterLongitude,
			viewportRadiusMeters / kAvisoLatMetersPerDegree,
			viewportRadiusMeters / (kAvisoLonMetersPerDegree * AvisoCosLatitude(m_AvisoCenterLatitude)),
			radar_screen->AvisoGeoJsonMinLatitude, radar_screen->AvisoGeoJsonMaxLatitude,
			radar_screen->AvisoGeoJsonMinLongitude, radar_screen->AvisoGeoJsonMaxLongitude))
		{
			m_AvisoViewInitialized = false;
			ClearAvisoViewportCache();
		}
	}
	// Initializing the view from the airport or dataset bounds
	if (!m_AvisoViewInitialized)
	{
		CPosition airportPosition;
		if (radar_screen->TryGetActiveAirportPosition(airportPosition) &&
			(!radar_screen->AvisoGeoJsonHasBounds ||
			 VsmrAvisoViewportCenter::OverlapsAirport(
				airportPosition.m_Latitude, airportPosition.m_Longitude, 0.0, 0.0,
				radar_screen->AvisoGeoJsonMinLatitude, radar_screen->AvisoGeoJsonMaxLatitude,
				radar_screen->AvisoGeoJsonMinLongitude, radar_screen->AvisoGeoJsonMaxLongitude)))
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
	m_AvisoCenterContext = centerContext;

	// The same double-precision transform drives geometry, aircraft and input.
	const double scale = (std::max)(1.0, m_AvisoScale);
	const double screenRotationDeg = ResolveAvisoViewportScreenRotationDeg(radar_screen,
		m_AvisoCenterLatitude, m_AvisoCenterLongitude);
	m_AvisoState->screenRotationDeg = screenRotationDeg;
	const VsmrAviso::InsetProjection projection{
		{ m_AvisoCenterLongitude, m_AvisoCenterLatitude },
		{ viewportRect.left + viewportWidth * 0.5, viewportRect.top + viewportHeight * 0.5 },
		scale, screenRotationDeg };
	const auto bounds = projection.Bounds(viewportWidth, viewportHeight);
	const double displayMinLon = bounds.minLongitude, displayMaxLon = bounds.maxLongitude;
	const double displayMinLat = bounds.minLatitude, displayMaxLat = bounds.maxLatitude;
	const double lonSpan = displayMaxLon - displayMinLon;
	const double latSpan = displayMaxLat - displayMinLat;
	if (lonSpan <= 0.0 || latSpan <= 0.0)
	{
		radar_screen->DrawAvisoBackground(*gdi, viewportRect);
		drawCenteredMessage("AVISO unavailable");
		drawChrome();
		dc.Detach();
		return;
	}
	const double scaleX = static_cast<double>(viewportWidth) / lonSpan;
	const double scaleY = static_cast<double>(viewportHeight) / latSpan;
	auto point = [&](double longitude, double latitude) -> Gdiplus::PointF {
		const auto projected = projection.Project({ longitude, latitude });
		return { static_cast<Gdiplus::REAL>(projected.X), static_cast<Gdiplus::REAL>(projected.Y) };
	};
	const auto projectedTopLeft = point(displayMinLon, displayMaxLat);
	const auto projectedTopRight = point(displayMaxLon, displayMaxLat);
	const auto projectedBottomLeft = point(displayMinLon, displayMinLat);
	const auto projectedBottomRight = point(displayMaxLon, displayMinLat);

	CPosition insetDownLeft;
	insetDownLeft.m_Latitude = displayMinLat;
	insetDownLeft.m_Longitude = displayMinLon;
	CPosition insetUpRight;
	insetUpRight.m_Latitude = displayMaxLat;
	insetUpRight.m_Longitude = displayMaxLon;
	const double crossDistance = SMRGeometry::DistanceMeters(insetDownLeft, insetUpRight);
	// Normalize the visible ground distance to the current host dimensions.
	// A small inset must not reveal dense text merely because its diagonal is shorter.
	const int labelZoomLevel = SMRGeometry::ZoomLevelFromCrossDistance(crossDistance / labelVisibilityRatio);
	const int viewportZoomLevel = SMRGeometry::ZoomLevelFromCrossDistance(crossDistance);
	InsetAvisoView view{ viewportRect, displayMinLon, displayMaxLon,
		displayMinLat, displayMaxLat, lonSpan, latSpan, projection };
	double renderMinLon = displayMinLon, renderMaxLon = displayMaxLon;
	double renderMinLat = displayMinLat, renderMaxLat = displayMaxLat;
	for (const auto& corner : { VsmrAviso::PointD{ double(viewportRect.left), double(viewportRect.top) },
		VsmrAviso::PointD{ double(viewportRect.right), double(viewportRect.top) },
		VsmrAviso::PointD{ double(viewportRect.left), double(viewportRect.bottom) },
		VsmrAviso::PointD{ double(viewportRect.right), double(viewportRect.bottom) } })
	{
		const auto geographic = projection.Unproject(corner);
		renderMinLon = (std::min)(renderMinLon, geographic.longitude);
		renderMaxLon = (std::max)(renderMaxLon, geographic.longitude);
		renderMinLat = (std::min)(renderMinLat, geographic.latitude);
		renderMaxLat = (std::max)(renderMaxLat, geographic.latitude);
	}
	const double longitudeMargin = (renderMaxLon - renderMinLon) * 0.5;
	const double latitudeMargin = (renderMaxLat - renderMinLat) * 0.5;
	CSMRRadar::AvisoRasterRenderRequest request;
	request.groupGeneration = groupGeneration;
	request.path = path;
	request.features = featureSnapshot;
	request.labels = labelSnapshot;
	request.scene = renderScene;
	request.preciseProjection = VsmrAviso::BilinearProjection::FromCorners(bounds,
		projection.Project({ displayMinLon, displayMaxLat }), projection.Project({ displayMaxLon, displayMaxLat }),
		projection.Project({ displayMinLon, displayMinLat }), projection.Project({ displayMaxLon, displayMinLat }));
	request.groupVisibility = groupVisibility;
	request.colorPalette = radar_screen->AvisoColorPalette;
	request.backgroundColor = radar_screen->GetAvisoRasterBackgroundColor();
	request.rasterWidth = viewportWidth;
	request.rasterHeight = viewportHeight;
	request.rasterScale = 1.0;
	request.displayScale = radar_screen->GetDisplayScale();
	request.labelZoomLevel = labelZoomLevel;
	request.displayMinLongitude = displayMinLon;
	request.displayMinLatitude = displayMinLat;
	request.displayMaxLongitude = displayMaxLon;
	request.displayMaxLatitude = displayMaxLat;
	request.renderMinLongitude = renderMinLon - longitudeMargin;
	request.renderMinLatitude = renderMinLat - latitudeMargin;
	request.renderMaxLongitude = renderMaxLon + longitudeMargin;
	request.renderMaxLatitude = renderMaxLat + latitudeMargin;
	request.scaleX = scaleX;
	request.scaleY = scaleY;
	request.viewportZoomLevel = viewportZoomLevel;
	request.projectedTopLeft = projectedTopLeft;
	request.projectedTopRight = projectedTopRight;
	request.projectedBottomLeft = projectedBottomLeft;
	request.projectedBottomRight = projectedBottomRight;
	bool backgroundSatisfied = false;
	const auto outcome = m_AvisoState->native.Draw(*gdi, hDC, viewportRect, request, [&]() {
		radar_screen->DrawAvisoBackground(*gdi, viewportRect);
		backgroundSatisfied = true;
	});
	const bool cacheDrawn = outcome.drawn;
	if (outcome.buildAttempted)
	{
		radar_screen->PerformanceDiagnostics.RecordAvisoRasterBuild(VsmrPerformance::AvisoViewport::Inset,
			outcome.buildMilliseconds, 0.0, outcome.drawn);
		if (outcome.drawn)
			radar_screen->PerformanceDiagnostics.RecordAvisoResultApplied(VsmrPerformance::AvisoViewport::Inset);
	}
	radar_screen->PerformanceDiagnostics.RecordAvisoCacheOutcome(VsmrPerformance::AvisoViewport::Inset,
		outcome.cacheHit && outcome.drawn ? VsmrPerformance::AvisoCacheOutcome::Exact : VsmrPerformance::AvisoCacheOutcome::Miss,
		false, !outcome.drawn);
	if (!backgroundSatisfied && !cacheDrawn)
		radar_screen->DrawAvisoBackground(*gdi, viewportRect);
	if (!cacheDrawn)
		drawCenteredMessage("AVISO unavailable");

	// ----- Drawing aircraft and tags -----


	if (cacheDrawn)
		renderAvisoAircraft(hDC, dc, radar_screen, gdi, mouseLocation, view);

	// Use the AVISO viewport's own pan/zoom/rotation projection.  The external
	// RDF plugin only knows the parent radar transform, which is why its marker
	// cannot be reused for this inset.
	gdi->Flush(Gdiplus::FlushIntentionSync);
	const auto rdfStarted = std::chrono::steady_clock::now();
	VsmrRdf::Draw(
		hDC,
		radar_screen,
		viewportRect,
		[&](const CPosition& position) -> POINT
		{
			const Gdiplus::PointF projected = view.projectPoint(
				position.m_Longitude,
				position.m_Latitude);
			return {
				static_cast<LONG>(std::lround(static_cast<double>(projected.X))),
				static_cast<LONG>(std::lround(static_cast<double>(projected.Y)))
			};
		});
	m_LastRdfRenderMilliseconds += std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - rdfStarted).count();

	if (radar_screen->ShowNorthIndicator && VsmrRendering::DrawNorthIndicator(*gdi, viewportRect, screenRotationDeg,
		radar_screen->GetAvisoBackgroundColor(), m_NorthIndicatorOffset))
		radar_screen->AddScreenObject(m_Id, "north.drag",
			VsmrRendering::NorthIndicatorBounds(viewportRect, m_NorthIndicatorOffset), true, "Drag north indicator");
	gdi->Flush(Gdiplus::FlushIntentionSync);
	drawChrome();

	dc.Detach();
}
