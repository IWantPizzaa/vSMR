#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/VsidBridgeClient.hpp"
#include "radar/RadarScreen.hpp"
#include "plugin/Plugin.hpp"
#include "shared/TextUtils.hpp"

extern CPoint mouseLocation;

namespace
{
	constexpr int kRailWidth = 48;
	constexpr int kDragHeight = 10;
	constexpr int kRailPadding = 3;
	constexpr int kButtonSize = 40;
	constexpr int kButtonGap = 3;
	constexpr int kAirportRowHeight = 22;
	constexpr int kRailButtonCount = 6;
	constexpr int kRailHeight =
		kDragHeight +
		(kRailPadding * 2) +
		kAirportRowHeight +
		(kButtonSize * kRailButtonCount) +
		(kButtonGap * kRailButtonCount);
	constexpr int kPopupGap = 4;
	constexpr int kPopupHeaderHeight = 23;
	constexpr int kPopupRowHeight = 28;
	constexpr int kPopupPadding = 3;
	constexpr int kPopupPagerHeight = 24;
	constexpr int kPopupControlHeight = 22;
	constexpr int kPopupActionHeight = 22;
	constexpr int kControlCornerDiameter = 6;
	constexpr int kPanelCornerDiameter = 8;
	constexpr int kInsetPopupWidth = 196;
	constexpr int kVsidPopupWidth = 280;
	constexpr int kVsidPopupHeight = 322;
	constexpr int kVsidLfpgPopupHeight = 396;
	constexpr int kStandardPopupWidth = 170;

	struct RuntimeMenuPalette
	{
		COLORREF outerBorder;
		COLORREF railBackground;
		COLORREF popupBackground;
		COLORREF titleBackground;
		COLORREF titleStripe;
		COLORREF panelTitleBackground;
		COLORREF buttonBackground;
		COLORREF listBackground;
		COLORREF cardBackground;
		COLORREF buttonHover;
		COLORREF accent;
		COLORREF accentHover;
		COLORREF text;
		COLORREF mutedText;
		COLORREF accentText;
		COLORREF divider;
		COLORREF disabledBackground;
		COLORREF disabledText;
		COLORREF dangerText;
		COLORREF dangerHover;
	};

	RuntimeMenuPalette ResolveRuntimeMenuPalette(bool dayTheme)
	{
		if (dayTheme)
		{
			return {
				RGB(63, 72, 76), RGB(115, 125, 128), RGB(171, 178, 180),
				RGB(115, 125, 128), RGB(146, 155, 158), RGB(146, 155, 158),
				RGB(173, 181, 183), RGB(182, 188, 190), RGB(165, 173, 175),
				RGB(190, 201, 204), RGB(63, 130, 159), RGB(52, 114, 141),
				RGB(23, 33, 38), RGB(70, 83, 88), RGB(247, 251, 252),
				RGB(125, 135, 138), RGB(150, 158, 161), RGB(98, 110, 114),
				RGB(141, 52, 58), RGB(201, 111, 116)
			};
		}

		return {
			RGB(5, 7, 8), RGB(30, 40, 43), RGB(32, 42, 45),
			RGB(9, 12, 13), RGB(23, 29, 31), RGB(34, 45, 48),
			RGB(41, 57, 59), RGB(41, 56, 59), RGB(39, 52, 56),
			RGB(53, 71, 75), RGB(80, 150, 180), RGB(98, 165, 193),
			RGB(208, 217, 220), RGB(143, 161, 166), RGB(244, 248, 249),
			RGB(17, 23, 25), RGB(31, 42, 45), RGB(91, 107, 112),
			RGB(229, 167, 167), RGB(112, 51, 55)
		};
	}

	enum class RuntimeIndicator
	{
		None,
		Selection,
		Visibility
	};

	struct RuntimePopupEntry
	{
		std::string id;
		std::string label;
		RuntimeIndicator indicator = RuntimeIndicator::None;
		bool active = false;
		bool enabled = true;
	};

	bool PointInside(const CRect& rect, const CPoint& point)
	{
		return
			point.x >= rect.left &&
			point.x < rect.right &&
			point.y >= rect.top &&
			point.y < rect.bottom;
	}

	void FillRectColor(HDC hdc, const CRect& rect, COLORREF color)
	{
		::SetDCBrushColor(hdc, color);
		::FillRect(hdc, &rect, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
	}

	void DrawRoundedRect(HDC hdc, const CRect& rect, COLORREF fill, COLORREF border, int diameter)
	{
		::SetDCBrushColor(hdc, fill);
		::SetDCPenColor(hdc, border);
		::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
		::SelectObject(hdc, ::GetStockObject(DC_PEN));
		::RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
	}

	void DrawSquareRect(HDC hdc, const CRect& rect, COLORREF fill, COLORREF border)
	{
		::SetDCBrushColor(hdc, fill);
		::SetDCPenColor(hdc, border);
		::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
		::SelectObject(hdc, ::GetStockObject(DC_PEN));
		::Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
	}

	void DrawRoundedBorder(HDC hdc, const CRect& rect, COLORREF border, int diameter)
	{
		::SetDCPenColor(hdc, border);
		::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
		::SelectObject(hdc, ::GetStockObject(DC_PEN));
		::RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
		::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
	}

	void DrawSquareBorder(HDC hdc, const CRect& rect, COLORREF border)
	{
		::SetDCPenColor(hdc, border);
		::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
		::SelectObject(hdc, ::GetStockObject(DC_PEN));
		::Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
		::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
	}

	void DrawTextEllipsis(HDC hdc, const CRect& sourceRect, const std::string& text, COLORREF color, UINT alignment = DT_LEFT)
	{
		CRect rect(sourceRect);
		::SetTextColor(hdc, color);
		::SetBkMode(hdc, TRANSPARENT);
		::DrawTextA(
			hdc,
			text.c_str(),
			-1,
			&rect,
			alignment | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}

	void DrawRuntimeSelectionIndicator(Gdiplus::Graphics& graphics, const CRect& rect, bool active, COLORREF color)
	{
		const Gdiplus::Color gdipColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
		Gdiplus::Pen pen(gdipColor, 1.2f);
		const int diameter = 12;
		const int left = rect.left + ((rect.Width() - diameter) / 2);
		const int top = rect.top + ((rect.Height() - diameter) / 2);
		graphics.DrawEllipse(&pen, left, top, diameter, diameter);
		if (active)
		{
			Gdiplus::SolidBrush brush(gdipColor);
			graphics.FillEllipse(&brush, left + 3, top + 3, diameter - 6, diameter - 6);
		}
	}

	void DrawRuntimeVisibilityIndicator(Gdiplus::Graphics& graphics, const CRect& rect, bool visible, COLORREF color)
	{
		const Gdiplus::Color gdipColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
		Gdiplus::Pen pen(gdipColor, 1.15f);
		const float centerX = static_cast<float>(rect.left + (rect.Width() / 2));
		const float centerY = static_cast<float>(rect.top + (rect.Height() / 2));
		Gdiplus::GraphicsPath eyePath;
		eyePath.AddBezier(centerX - 7.0f, centerY, centerX - 3.7f, centerY - 4.4f, centerX + 3.7f, centerY - 4.4f, centerX + 7.0f, centerY);
		eyePath.AddBezier(centerX + 7.0f, centerY, centerX + 3.7f, centerY + 4.4f, centerX - 3.7f, centerY + 4.4f, centerX - 7.0f, centerY);
		graphics.DrawPath(&pen, &eyePath);
		graphics.DrawEllipse(&pen, centerX - 2.0f, centerY - 2.0f, 4.0f, 4.0f);
		if (!visible)
			graphics.DrawLine(&pen, centerX - 6.2f, centerY - 6.2f, centerX + 6.2f, centerY + 6.2f);
	}

	void DrawRuntimeIcon(Gdiplus::Graphics& graphics, const std::string& kind, const CRect& rect, COLORREF color)
	{
		const Gdiplus::Color gdipColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
		Gdiplus::Pen pen(gdipColor, 1.35f);
		pen.SetStartCap(Gdiplus::LineCapSquare);
		pen.SetEndCap(Gdiplus::LineCapSquare);
		pen.SetLineJoin(Gdiplus::LineJoinMiter);

		const float centerX = static_cast<float>(rect.left + rect.Width() / 2);
		const float centerY = static_cast<float>(rect.top + rect.Height() / 2);

		if (kind == "settings")
		{
			graphics.DrawLine(&pen, centerX - 8.0f, centerY - 6.0f, centerX - 5.0f, centerY - 6.0f);
			graphics.DrawLine(&pen, centerX - 1.0f, centerY - 6.0f, centerX + 8.0f, centerY - 6.0f);
			graphics.DrawEllipse(&pen, centerX - 5.0f, centerY - 8.0f, 4.0f, 4.0f);
			graphics.DrawLine(&pen, centerX - 8.0f, centerY, centerX + 2.0f, centerY);
			graphics.DrawLine(&pen, centerX + 6.0f, centerY, centerX + 8.0f, centerY);
			graphics.DrawEllipse(&pen, centerX + 2.0f, centerY - 2.0f, 4.0f, 4.0f);
			graphics.DrawLine(&pen, centerX - 8.0f, centerY + 6.0f, centerX - 3.0f, centerY + 6.0f);
			graphics.DrawLine(&pen, centerX + 1.0f, centerY + 6.0f, centerX + 8.0f, centerY + 6.0f);
			graphics.DrawEllipse(&pen, centerX - 3.0f, centerY + 4.0f, 4.0f, 4.0f);
			return;
		}

		if (kind == "datalink")
		{
			graphics.DrawRectangle(&pen, centerX - 9.0f, centerY - 6.5f, 18.0f, 13.0f);
			graphics.DrawLine(&pen, centerX - 9.0f, centerY - 6.5f, centerX, centerY + 0.5f);
			graphics.DrawLine(&pen, centerX + 9.0f, centerY - 6.5f, centerX, centerY + 0.5f);
			graphics.DrawLine(&pen, centerX - 9.0f, centerY + 6.5f, centerX - 2.0f, centerY + 0.5f);
			graphics.DrawLine(&pen, centerX + 9.0f, centerY + 6.5f, centerX + 2.0f, centerY + 0.5f);
			return;
		}

		if (kind == "groups")
		{
			const Gdiplus::PointF topLayer[] = {
				Gdiplus::PointF(centerX, centerY - 8.0f),
				Gdiplus::PointF(centerX + 8.0f, centerY - 4.0f),
				Gdiplus::PointF(centerX, centerY),
				Gdiplus::PointF(centerX - 8.0f, centerY - 4.0f)
			};
			const Gdiplus::PointF middleLayer[] = {
				Gdiplus::PointF(centerX - 8.0f, centerY),
				Gdiplus::PointF(centerX, centerY + 4.0f),
				Gdiplus::PointF(centerX + 8.0f, centerY)
			};
			const Gdiplus::PointF bottomLayer[] = {
				Gdiplus::PointF(centerX - 8.0f, centerY + 4.0f),
				Gdiplus::PointF(centerX, centerY + 8.0f),
				Gdiplus::PointF(centerX + 8.0f, centerY + 4.0f)
			};
			graphics.DrawPolygon(&pen, topLayer, 4);
			graphics.DrawLines(&pen, middleLayer, 3);
			graphics.DrawLines(&pen, bottomLayer, 3);
			return;
		}

		if (kind == "mode")
		{
			Gdiplus::GraphicsPath eyePath;
			eyePath.AddBezier(centerX - 8.5f, centerY, centerX - 5.3f, centerY - 5.0f, centerX + 5.3f, centerY - 5.0f, centerX + 8.5f, centerY);
			eyePath.AddBezier(centerX + 8.5f, centerY, centerX + 5.3f, centerY + 5.0f, centerX - 5.3f, centerY + 5.0f, centerX - 8.5f, centerY);
			graphics.DrawPath(&pen, &eyePath);
			graphics.DrawEllipse(&pen, centerX - 2.75f, centerY - 2.75f, 5.5f, 5.5f);
			return;
		}

		if (kind == "insets")
		{
			graphics.DrawRectangle(&pen, centerX - 8.0f, centerY - 7.0f, 16.0f, 12.0f);
			graphics.DrawLine(&pen, centerX, centerY + 5.0f, centerX, centerY + 9.0f);
			graphics.DrawLine(&pen, centerX - 4.0f, centerY + 9.0f, centerX + 4.0f, centerY + 9.0f);
			return;
		}

		if (kind == "profile")
		{
			graphics.DrawRectangle(&pen, centerX - 8.0f, centerY - 7.0f, 16.0f, 14.0f);
			graphics.DrawEllipse(&pen, centerX - 5.0f, centerY - 4.0f, 4.0f, 4.0f);
			graphics.DrawArc(&pen, centerX - 6.5f, centerY, 7.0f, 6.0f, 200.0f, 140.0f);
			graphics.DrawLine(&pen, centerX + 2.0f, centerY - 3.0f, centerX + 5.0f, centerY - 3.0f);
			graphics.DrawLine(&pen, centerX + 2.0f, centerY, centerX + 5.0f, centerY);
			graphics.DrawLine(&pen, centerX + 2.0f, centerY + 3.0f, centerX + 5.0f, centerY + 3.0f);
			return;
		}

	}

}

struct CSMRRadar::RuntimeMenuPopupRenderer
{
	CSMRRadar& radar;
	HDC hdc;
	Gdiplus::Graphics& graphics;
	const RuntimeMenuPalette& palette;
	const CRect& bounds;
	const std::string& activeProfile;
	const std::string& activeMode;
	const std::vector<AvisoGroup>& groups;
	std::string title, vsidAirport;
	std::vector<RuntimePopupEntry> entries;
	std::vector<AvisoPreset> insetPresets;
	VsmrVsid::InterfaceState vsidState;
	DatalinkControlState datalinkState;
	bool insetPopup = false, vsidPopup = false, showPager = false, insetPopupTooShort = false;
	int popupWidth = 0, popupHeight = 0, visibleRows = 0, contentTop = 0;
	HFONT rowFont = nullptr, actionFont = nullptr;
	bool BuildPopup()
	{
		// ----- Building the popup -----
		if (radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Mode)
		{
			title = "Mode";
			const std::vector<DisplayModeSettings> modes = radar.GetProfileDisplayModesForEditor(activeProfile);
			for (size_t index = 0; index < modes.size(); ++index)
			{
				RuntimePopupEntry entry;
				entry.id = "runtime.mode." + std::to_string(index);
				entry.label = modes[index].name;
				entry.indicator = RuntimeIndicator::Selection;
				entry.active = AsciiCaseInsensitiveEquals(modes[index].name, activeMode);
				entries.push_back(entry);
			}
		}
		else if (radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Groups)
		{
			title = "Groups";
			for (size_t index = 0; index < groups.size(); ++index)
			{
				RuntimePopupEntry entry;
				entry.id = "runtime.group." + std::to_string(index);
				entry.label = groups[index].name;
				entry.indicator = RuntimeIndicator::Visibility;
				entry.active = groups[index].visible;
				entries.push_back(entry);
			}
		}
		else if (radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Profile)
		{
			title = "Profile";
			const std::vector<std::string> profiles = radar.GetOrderedProfileNamesForUi();
			for (size_t index = 0; index < profiles.size(); ++index)
			{
				RuntimePopupEntry entry;
				entry.id = "runtime.profile." + std::to_string(index);
				entry.label = profiles[index];
				entry.indicator = RuntimeIndicator::Selection;
				entry.active = AsciiCaseInsensitiveEquals(profiles[index], activeProfile);
				entries.push_back(entry);
			}
		}
		else if (radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Datalink)
		{
			title = "vSID / CPDLC";
		}

		insetPopup = radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Insets;
		vsidPopup = radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Datalink;
		const bool datalinkPopup = radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Datalink;
		vsidState = vsidPopup
			? VsmrVsid::GetInterfaceState(radar.getActiveAirport())
			: VsmrVsid::InterfaceState();
		vsidAirport = vsidPopup
			? VsmrVsid::NormalizeAirport(radar.getActiveAirport())
			: std::string();
		CSMRPlugin* datalinkPlugin = datalinkPopup
			? static_cast<CSMRPlugin*>(radar.GetPlugIn())
			: nullptr;
		datalinkState = datalinkPlugin != nullptr
			? datalinkPlugin->GetDatalinkControlState()
			: DatalinkControlState();
		insetPresets = insetPopup
			? radar.GetAvisoPresets()
			: std::vector<AvisoPreset>();
		popupWidth = vsidPopup ? kVsidPopupWidth
			: (insetPopup ? kInsetPopupWidth : kStandardPopupWidth);
		if (bounds.Width() < popupWidth + 8)
		{
			radar.RuntimeMenuPopupArea.SetRectEmpty();
			return false;
		}
		popupHeight = 0;
		visibleRows = 0;
		showPager = false;
		insetPopupTooShort = false;
		if (vsidPopup)
		{
			popupHeight = vsidAirport == "LFPG"
				? kVsidLfpgPopupHeight
				: kVsidPopupHeight;
		}
		else if (!insetPopup)
		{
			const int maximumHeight = (std::max)(80, bounds.Height() - 8);
			int rowCapacity = (maximumHeight - kPopupHeaderHeight - (kPopupPadding * 2)) / kPopupRowHeight;
			rowCapacity = (std::max)(1, rowCapacity);
			showPager = static_cast<int>(entries.size()) > rowCapacity;
			if (showPager)
				rowCapacity = (std::max)(1, (maximumHeight - kPopupHeaderHeight - (kPopupPadding * 2) - kPopupPagerHeight) / kPopupRowHeight);
			visibleRows = (std::min)(rowCapacity, static_cast<int>(entries.size()));
			if (entries.empty())
				popupHeight = kPopupHeaderHeight + 42;
			else
				popupHeight = kPopupHeaderHeight + (kPopupPadding * 2) + (visibleRows * kPopupRowHeight) + (showPager ? kPopupPagerHeight : 0);
		}
		else
		{
			const int presetRows = (std::min)(4, static_cast<int>(insetPresets.size()));
			const bool presetPager = insetPresets.size() > 4;
			popupHeight =
				kPopupHeaderHeight +
				kPopupPadding +
				(4 * kPopupRowHeight) +
				19 +
				(presetRows > 0 ? presetRows * kPopupRowHeight : 32) +
				(presetPager ? kPopupPagerHeight : 0) +
				(4 * kPopupActionHeight) +
				(3 * 3) +
				kPopupPadding;
			insetPopupTooShort = popupHeight > bounds.Height() - 8;
			if (insetPopupTooShort)
				popupHeight = kPopupHeaderHeight + 42;
		}

		return true;
	}

	void addPopupScreenObject(const char* id, const CRect& area, const char* tooltip)
	{
		CRect clippedArea;
		if (::IntersectRect(&clippedArea, &area, &radar.RuntimeMenuPopupArea) && !clippedArea.IsRectEmpty())
			radar.AddScreenObject(RUNTIME_MENU_POPUP, id, clippedArea, false, tooltip);
	}

	int beginRoundedClip(const CRect& area)
	{
		const int clipState = ::SaveDC(hdc);
		HRGN clipRegion = ::CreateRoundRectRgn(
			area.left,
			area.top,
			area.right + 1,
			area.bottom + 1,
			kPanelCornerDiameter,
			kPanelCornerDiameter);
		if (clipRegion != nullptr)
		{
			::ExtSelectClipRgn(hdc, clipRegion, RGN_AND);
			::DeleteObject(clipRegion);
		}
		return clipState;
	}

	void drawChoiceRow(const RuntimePopupEntry& entry, const CRect& rowArea)
	{
		const bool hover = entry.enabled && PointInside(rowArea, mouseLocation);
		COLORREF fill = entry.enabled ? palette.listBackground : palette.disabledBackground;
		if (entry.active && entry.indicator == RuntimeIndicator::Selection)
			fill = palette.accent;
		else if (hover)
			fill = palette.buttonHover;
		FillRectColor(hdc, rowArea, fill);
		CRect divider(rowArea.left, rowArea.bottom - 1, rowArea.right, rowArea.bottom);
		FillRectColor(hdc, divider, palette.divider);

		const COLORREF foreground =
			!entry.enabled ? palette.disabledText :
			(entry.active && entry.indicator == RuntimeIndicator::Selection ? palette.accentText : palette.text);
		CRect indicatorArea(rowArea.left + 3, rowArea.top, rowArea.left + 20, rowArea.bottom);
		if (entry.indicator == RuntimeIndicator::Selection)
			DrawRuntimeSelectionIndicator(graphics, indicatorArea, entry.active, foreground);
		else if (entry.indicator == RuntimeIndicator::Visibility)
			DrawRuntimeVisibilityIndicator(graphics, indicatorArea, entry.active, entry.active ? foreground : palette.mutedText);

		::SelectObject(hdc, rowFont);
		CRect labelArea(rowArea.left + 24, rowArea.top, rowArea.right - 5, rowArea.bottom);
		DrawTextEllipsis(hdc, labelArea, entry.label, foreground);
		addPopupScreenObject(entry.id.c_str(), rowArea, entry.label.c_str());
	}

	void drawRuntimeButton(
		const char* id,
		const CRect& buttonArea,
		const std::string& label,
		bool enabled,
		bool primary,
		bool danger,
		const std::string& tooltip,
		bool interactive = true)
	{
		const bool hover = enabled && PointInside(buttonArea, mouseLocation);
		COLORREF fill = enabled ? palette.buttonBackground : palette.disabledBackground;
		COLORREF foreground = enabled ? palette.text : palette.disabledText;
		if (enabled && primary)
		{
			fill = hover ? palette.accentHover : palette.accent;
			foreground = palette.accentText;
		}
		else if (enabled && danger)
		{
			fill = hover ? palette.dangerHover : palette.buttonBackground;
			foreground = hover ? RGB(255, 240, 240) : palette.dangerText;
		}
		else if (hover)
		{
			fill = palette.buttonHover;
		}
		DrawRoundedRect(hdc, buttonArea, fill, palette.outerBorder, kControlCornerDiameter);
		::SelectObject(hdc, actionFont);
		DrawTextEllipsis(hdc, buttonArea, label, foreground, DT_CENTER);
		if (enabled && interactive)
			addPopupScreenObject(id, buttonArea, tooltip.c_str());
	}

	void drawSectionLabel(const std::string& label)
	{
		::SelectObject(hdc, actionFont);
		CRect sectionArea(
			radar.RuntimeMenuPopupArea.left + 6,
			contentTop,
			radar.RuntimeMenuPopupArea.right - 6,
			contentTop + 18);
		DrawTextEllipsis(hdc, sectionArea, label, palette.mutedText);
		contentTop += 18;
	}

	void twoColumnAreas(int height, CRect& left, CRect& right)
	{
		const int gap = 3;
		const int availableWidth = radar.RuntimeMenuPopupArea.Width() - (kPopupPadding * 2) - gap;
		const int leftWidth = availableWidth / 2;
		left = CRect(
			radar.RuntimeMenuPopupArea.left + kPopupPadding,
			contentTop,
			radar.RuntimeMenuPopupArea.left + kPopupPadding + leftWidth,
			contentTop + height);
		right = CRect(
			left.right + gap,
			contentTop,
			radar.RuntimeMenuPopupArea.right - kPopupPadding,
			contentTop + height);
	}

	void DrawDatalink()
	{
		// Drawing the fixed vSID actions published by its supported command surface
		std::string statusText;
		if (vsidState.commandLineBusy)
			statusText = "EuroScope command line busy";
		else if (vsidState.providerReady)
			statusText = "vSID connected - " + std::to_string(vsidState.aircraftCount) + " active aircraft";
		else if (!vsidState.bridgeLoaded)
			statusText = "vSID - bridge not loaded";
		else if (!vsidState.bridgeCompatible)
			statusText = "vSID - incompatible bridge";
		else
			statusText = "vSID provider unavailable";

		CRect statusArea(
			radar.RuntimeMenuPopupArea.left + kPopupPadding,
			contentTop,
			radar.RuntimeMenuPopupArea.right - kPopupPadding,
			contentTop + 25);
		DrawRoundedRect(hdc, statusArea, palette.cardBackground, palette.outerBorder, kControlCornerDiameter);
		::SelectObject(hdc, actionFont);
		DrawRuntimeSelectionIndicator(graphics, CRect(statusArea.left + 4, statusArea.top, statusArea.left + 22, statusArea.bottom),
			vsidState.providerReady, vsidState.providerReady ? palette.accent : palette.mutedText);
		CRect statusTextArea(statusArea.left + 25, statusArea.top, statusArea.right - 6, statusArea.bottom);
		DrawTextEllipsis(hdc, statusTextArea, statusText, palette.text);
		contentTop += 29;

		const std::string& normalizedAirport = vsidAirport;
		const bool canSubmit = vsidState.providerReady && !vsidState.commandLineBusy;
		const bool canSubmitAirport = canSubmit && !normalizedAirport.empty();
		drawSectionLabel(normalizedAirport.empty()
			? "AIRPORT REQUIRED"
			: "AIRPORT " + normalizedAirport);

		auto drawVsidRow = [&](
			const char* leftId,
			const char* leftLabel,
			const char* leftTooltip,
			bool leftEnabled,
			const char* rightId,
			const char* rightLabel,
			const char* rightTooltip,
			bool rightEnabled)
		{
			CRect leftArea;
			CRect rightArea;
			twoColumnAreas(26, leftArea, rightArea);
			drawRuntimeButton(leftId, leftArea, leftLabel, leftEnabled, false, false, leftTooltip);
			drawRuntimeButton(rightId, rightArea, rightLabel, rightEnabled, false, false, rightTooltip);
			contentTop += 30;
		};

		auto drawVsidActions = [&](const auto& definitions, bool enabled)
		{
			std::size_t index = 0U;
			for (; index + 1U < definitions.size(); index += 2U)
			{
				const VsmrVsid::RuntimeActionDefinition& left = definitions[index];
				const VsmrVsid::RuntimeActionDefinition& right = definitions[index + 1U];
				drawVsidRow(
					left.objectId, left.label, left.tooltip, enabled,
					right.objectId, right.label, right.tooltip, enabled);
			}
			if (index < definitions.size())
			{
				const VsmrVsid::RuntimeActionDefinition& action = definitions[index];
				CRect area(
					radar.RuntimeMenuPopupArea.left + kPopupPadding,
					contentTop,
					radar.RuntimeMenuPopupArea.right - kPopupPadding,
					contentTop + 26);
				drawRuntimeButton(
					action.objectId, area, action.label, enabled,
					false, false, action.tooltip);
				contentTop += 30;
			}
		};
		const auto& automatic = VsmrVsid::AirportRuntimeActions.front();
		CRect automaticArea(radar.RuntimeMenuPopupArea.left + kPopupPadding, contentTop,
			radar.RuntimeMenuPopupArea.right - kPopupPadding, contentTop + 26);
		drawRuntimeButton(automatic.objectId, automaticArea, "", canSubmitAirport,
			false, false, "Toggle vSID automatic mode for " + normalizedAirport);
		CRect automaticLabel(automaticArea.left + 9, automaticArea.top, automaticArea.right - 95, automaticArea.bottom);
		DrawTextEllipsis(hdc, automaticLabel, "Automatic mode", canSubmitAirport ? palette.text : palette.disabledText);
		CRect automaticStatus(automaticArea.right - 77, automaticArea.top, automaticArea.right - 8, automaticArea.bottom);
		DrawRuntimeSelectionIndicator(graphics,
			CRect(automaticStatus.left - 19, automaticStatus.top, automaticStatus.left - 1, automaticStatus.bottom),
			vsidState.automaticMode.value_or(false), vsidState.automaticMode.value_or(false) ? palette.accent : palette.mutedText);
		DrawTextEllipsis(hdc, automaticStatus,
			vsidState.automaticMode.has_value() ? (*vsidState.automaticMode ? "On" : "Off") : "Unknown",
			palette.mutedText, DT_RIGHT);
		contentTop += 30;

		if (normalizedAirport == "LFPG")
		{
			drawSectionLabel("LFPG MODES");
			CRect leftArea;
			CRect rightArea;
			twoColumnAreas(26, leftArea, rightArea);
			const auto& minimum = VsmrVsid::LfpgModeActions[0];
			const auto& crossing = VsmrVsid::LfpgModeActions[1];
			const bool minimumActive =
				vsidState.lfpgMode == VsmrVsid::LfpgOperatingMode::MinimumTaxiing;
			drawRuntimeButton(
				minimum.objectId, leftArea, minimum.label, canSubmitAirport,
				minimumActive, false, minimum.tooltip, !minimumActive);
			drawRuntimeButton(
				crossing.objectId, rightArea, crossing.label, canSubmitAirport,
				!minimumActive, false, crossing.tooltip, minimumActive);
			contentTop += 30;

			twoColumnAreas(26, leftArea, rightArea);
			const auto& linked = VsmrVsid::LfpgLinkActions[0];
			const auto& unlinked = VsmrVsid::LfpgLinkActions[1];
			const bool linkedActive =
				vsidState.lfpgLinkMode == VsmrVsid::LfpgLinkMode::Linked;
			drawRuntimeButton(
				linked.objectId, leftArea, linked.label, canSubmitAirport,
				linkedActive, false, linked.tooltip, !linkedActive);
			drawRuntimeButton(
				unlinked.objectId, rightArea, unlinked.label, canSubmitAirport,
				!linkedActive, false, unlinked.tooltip, linkedActive);
			contentTop += 30;
		}

		drawSectionLabel("vSID ACTIONS");
		drawVsidActions(VsmrVsid::GeneralRuntimeActions, canSubmit);
		DrawCpdlc();
	}

	void DrawCpdlc()
	{
		// Drawing CPDLC and PDC controls
		CSMRPlugin* plugin = static_cast<CSMRPlugin*>(radar.GetPlugIn());
		const DatalinkControlState& state = datalinkState;
		contentTop += 5;
		FillRectColor(hdc, CRect(radar.RuntimeMenuPopupArea.left + 6, contentTop,
			radar.RuntimeMenuPopupArea.right - 6, contentTop + 1), palette.divider);
		contentTop += 5;
		CRect heading(radar.RuntimeMenuPopupArea.left + 7, contentTop,
			radar.RuntimeMenuPopupArea.right - 7, contentTop + 22);
		::SelectObject(hdc, actionFont);
		DrawTextEllipsis(hdc, heading, "CPDLC / PDC", palette.text);
		CRect connectionStatus(heading.right - 80, heading.top, heading.right, heading.bottom);
		DrawRuntimeSelectionIndicator(graphics,
			CRect(connectionStatus.left - 19, heading.top, connectionStatus.left - 1, heading.bottom),
			state.connected, state.connected ? palette.accent : palette.mutedText);
		DrawTextEllipsis(hdc, connectionStatus,
			state.connected ? "Connected" : (state.connecting ? "Connecting" : "Offline"),
			palette.mutedText, DT_RIGHT);
		contentTop += 26;

		auto drawCredentialRow = [&](
			const std::string& label,
			const char* id,
			const std::string& value,
			const std::string& tooltip)
		{
			CRect labelArea;
			CRect valueArea;
			twoColumnAreas(26, labelArea, valueArea);
			labelArea.left += 4;
			::SelectObject(hdc, rowFont);
			DrawTextEllipsis(hdc, labelArea, label, palette.text);
			drawRuntimeButton(
				id,
				valueArea,
				value,
				plugin != nullptr && !state.connected && !state.connecting,
				false,
				false,
				tooltip);
			contentTop += 30;
		};
		drawCredentialRow(
			"Login",
			"runtime.datalink.callsign",
			state.logonCallsign.empty() ? "Set..." : state.logonCallsign,
			"Edit the CPDLC login callsign");
		drawCredentialRow(
			"Password",
			"runtime.datalink.credentials",
			state.hasPassword ? "Change..." : "Set...",
			"Edit the Hoppie logon password");

		CRect pollArea;
		CRect connectionArea;
		twoColumnAreas(26, pollArea, connectionArea);
		drawRuntimeButton(
			"runtime.datalink.poll",
			pollArea,
			state.pollInProgress ? "Polling..." : "Poll",
			plugin != nullptr && state.connected && !state.pollInProgress,
			false,
			false,
			"Poll Hoppie messages now");
		const bool canConnect = state.controllerConnected && !state.logonCallsign.empty() && state.hasPassword;
		drawRuntimeButton(
			"runtime.datalink.connection",
			connectionArea,
			state.connected ? "Disconnect" : (state.connecting ? "Cancel" : "Connect"),
			plugin != nullptr && (state.connected || state.connecting || canConnect),
			!state.connected && !state.connecting,
			state.connected,
			state.connected || state.connecting ? "Disconnect CPDLC" : "Connect CPDLC");
		contentTop += 30;
	}

	void DrawChoices()
	{
		// Drawing mode, group, or profile choices
		if (entries.empty())
		{
		::SelectObject(hdc, actionFont);
		CRect emptyArea(
			radar.RuntimeMenuPopupArea.left + 4,
			contentTop,
			radar.RuntimeMenuPopupArea.right - 4,
			radar.RuntimeMenuPopupArea.bottom - 4);
		const std::string emptyText =
			radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Groups ? "No AVISO groups." :
			radar.ActiveRuntimeMenuPopup == RuntimeMenuPopup::Mode ? "No modes in this profile." :
			"No profiles.";
		DrawTextEllipsis(hdc, emptyArea, emptyText, palette.mutedText, DT_CENTER);
		}
		else
		{
		const int maximumOffset = (std::max)(0, static_cast<int>(entries.size()) - visibleRows);
		radar.RuntimeMenuPopupScrollOffset = std::clamp(radar.RuntimeMenuPopupScrollOffset, 0, maximumOffset);
		const int endIndex = (std::min)(
			static_cast<int>(entries.size()),
			radar.RuntimeMenuPopupScrollOffset + visibleRows);
		const CRect listArea(
			radar.RuntimeMenuPopupArea.left + kPopupPadding,
			contentTop,
			radar.RuntimeMenuPopupArea.right - kPopupPadding,
			contentTop + (visibleRows * kPopupRowHeight));
		DrawRoundedRect(
			hdc,
			listArea,
			palette.listBackground,
			palette.outerBorder,
			kPanelCornerDiameter);
		const int listClipState = beginRoundedClip(listArea);

		for (int index = radar.RuntimeMenuPopupScrollOffset; index < endIndex; ++index)
		{
			CRect rowArea(
				radar.RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				radar.RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + kPopupRowHeight);
			drawChoiceRow(entries[static_cast<size_t>(index)], rowArea);
			contentTop += kPopupRowHeight;
		}
		::RestoreDC(hdc, listClipState);
		DrawRoundedBorder(hdc, listArea, palette.outerBorder, kPanelCornerDiameter);

		if (showPager)
		{
			CRect previousArea(
				radar.RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop + 1,
				radar.RuntimeMenuPopupArea.CenterPoint().x - 1,
				contentTop + 1 + kPopupControlHeight);
			CRect nextArea(
				radar.RuntimeMenuPopupArea.CenterPoint().x + 1,
				contentTop + 1,
				radar.RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + 1 + kPopupControlHeight);
			const bool canPrevious = radar.RuntimeMenuPopupScrollOffset > 0;
			const bool canNext = radar.RuntimeMenuPopupScrollOffset < maximumOffset;
			DrawRoundedRect(hdc, previousArea, canPrevious ? palette.buttonBackground : palette.disabledBackground, palette.outerBorder, kControlCornerDiameter);
			DrawRoundedRect(hdc, nextArea, canNext ? palette.buttonBackground : palette.disabledBackground, palette.outerBorder, kControlCornerDiameter);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, previousArea, "Previous", canPrevious ? palette.text : palette.disabledText, DT_CENTER);
			DrawTextEllipsis(hdc, nextArea, "Next", canNext ? palette.text : palette.disabledText, DT_CENTER);
			if (canPrevious)
				addPopupScreenObject("runtime.page.previous", previousArea, "Previous choices");
			if (canNext)
				addPopupScreenObject("runtime.page.next", nextArea, "Next choices");
		}
		}
	}

	void DrawInsetVisibility()
	{
		// Drawing inset visibility and preset controls
		const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
		const int weatherWindowId = APPWINDOW_WEATHER - APPWINDOW_BASE;
		const int timerWindowId = APPWINDOW_TIMER - APPWINDOW_BASE;
		const struct
		{
			const char* id;
			const char* resetId;
			const char* label;
			int appWindowId;
		} insetRows[] = {
			{ "runtime.inset.aviso", "runtime.inset.reset.aviso", "AVISO", avisoWindowId },
			{ "runtime.inset.srw1", "runtime.inset.reset.srw1", "SRW 1", 1 },
			{ "runtime.inset.weather", "runtime.inset.reset.weather", "Weather", weatherWindowId },
			{ "runtime.inset.timer", "runtime.inset.reset.timer", "Timer", timerWindowId }
		};
		const CRect insetListArea(
			radar.RuntimeMenuPopupArea.left + kPopupPadding,
			contentTop,
			radar.RuntimeMenuPopupArea.right - kPopupPadding,
			contentTop + (static_cast<int>(_countof(insetRows)) * kPopupRowHeight));
		DrawRoundedRect(
			hdc,
			insetListArea,
			palette.listBackground,
			palette.outerBorder,
			kPanelCornerDiameter);
		const int insetListClipState = beginRoundedClip(insetListArea);
		for (const auto& inset : insetRows)
		{
			const auto display = radar.appWindowDisplays.find(inset.appWindowId);
			RuntimePopupEntry entry;
			entry.id = inset.id;
			entry.label = inset.label;
			entry.indicator = RuntimeIndicator::Visibility;
			entry.active = display != radar.appWindowDisplays.end() && display->second;
			CRect rowArea(
				radar.RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				radar.RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + kPopupRowHeight);
			CRect visibilityArea(rowArea);
			visibilityArea.right -= 45;
			drawChoiceRow(entry, visibilityArea);
			CRect resetArea(
				visibilityArea.right + 3,
				rowArea.top + 3,
				rowArea.right,
				rowArea.bottom - 3);
			DrawRoundedRect(
				hdc,
				resetArea,
				PointInside(resetArea, mouseLocation) ? palette.buttonHover : palette.buttonBackground,
				palette.outerBorder,
				kControlCornerDiameter);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, resetArea, "Reset", palette.text, DT_CENTER);
			addPopupScreenObject(inset.resetId, resetArea, "Reset this inset view");
			contentTop += kPopupRowHeight;
		}
		::RestoreDC(hdc, insetListClipState);
		DrawRoundedBorder(hdc, insetListArea, palette.outerBorder, kPanelCornerDiameter);
	}

	void DrawInsetPresets()
	{
		::SelectObject(hdc, actionFont);
		CRect sectionArea(
			radar.RuntimeMenuPopupArea.left + 5,
			contentTop,
			radar.RuntimeMenuPopupArea.right - 5,
			contentTop + 19);
		DrawTextEllipsis(hdc, sectionArea, "PRESET", palette.mutedText);
		contentTop += 19;

		const std::vector<AvisoPreset>& presets = insetPresets;
		const std::string activePreset = radar.GetActiveAvisoPresetName();
		const int presetRows = (std::min)(4, static_cast<int>(presets.size()));
		const int maximumPresetOffset = (std::max)(0, static_cast<int>(presets.size()) - presetRows);
		radar.RuntimeMenuPopupScrollOffset = std::clamp(radar.RuntimeMenuPopupScrollOffset, 0, maximumPresetOffset);
		if (presets.empty())
		{
			CRect emptyArea(
				radar.RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				radar.RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + 32);
			DrawTextEllipsis(hdc, emptyArea, "No inset presets.", palette.mutedText, DT_CENTER);
			contentTop += 32;
		}
		else
		{
			const CRect presetListArea(
				radar.RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				radar.RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + (presetRows * kPopupRowHeight));
			DrawRoundedRect(
				hdc,
				presetListArea,
				palette.listBackground,
				palette.outerBorder,
				kPanelCornerDiameter);
			const int presetListClipState = beginRoundedClip(presetListArea);
			for (int row = 0; row < presetRows; ++row)
			{
				const int presetIndex = radar.RuntimeMenuPopupScrollOffset + row;
				RuntimePopupEntry entry;
				entry.id = "runtime.preset." + std::to_string(presetIndex);
				entry.label = presets[static_cast<size_t>(presetIndex)].name;
				entry.indicator = RuntimeIndicator::Selection;
				entry.active = AsciiCaseInsensitiveEquals(entry.label, activePreset);
				CRect rowArea(
					radar.RuntimeMenuPopupArea.left + kPopupPadding,
					contentTop,
					radar.RuntimeMenuPopupArea.right - kPopupPadding,
					contentTop + kPopupRowHeight);
				drawChoiceRow(entry, rowArea);
				contentTop += kPopupRowHeight;
			}
			::RestoreDC(hdc, presetListClipState);
			DrawRoundedBorder(hdc, presetListArea, palette.outerBorder, kPanelCornerDiameter);
		}

		if (presets.size() > 4)
		{
			CRect previousArea(
				radar.RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop + 1,
				radar.RuntimeMenuPopupArea.CenterPoint().x - 1,
				contentTop + 1 + kPopupControlHeight);
			CRect nextArea(
				radar.RuntimeMenuPopupArea.CenterPoint().x + 1,
				contentTop + 1,
				radar.RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + 1 + kPopupControlHeight);
			const bool canPrevious = radar.RuntimeMenuPopupScrollOffset > 0;
			const bool canNext = radar.RuntimeMenuPopupScrollOffset < maximumPresetOffset;
			DrawRoundedRect(hdc, previousArea, canPrevious ? palette.buttonBackground : palette.disabledBackground, palette.outerBorder, kControlCornerDiameter);
			DrawRoundedRect(hdc, nextArea, canNext ? palette.buttonBackground : palette.disabledBackground, palette.outerBorder, kControlCornerDiameter);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, previousArea, "Previous", canPrevious ? palette.text : palette.disabledText, DT_CENTER);
			DrawTextEllipsis(hdc, nextArea, "Next", canNext ? palette.text : palette.disabledText, DT_CENTER);
			if (canPrevious)
				addPopupScreenObject("runtime.preset.page.previous", previousArea, "Previous presets");
			if (canNext)
				addPopupScreenObject("runtime.preset.page.next", nextArea, "Next presets");
			contentTop += kPopupPagerHeight;
		}
	}

	void DrawPresetActions()
	{
		const auto& presets = insetPresets;
		const std::string activePreset = radar.GetActiveAvisoPresetName();
		const std::string defaultPreset = radar.GetDefaultAvisoPresetName();

		const bool hasActivePreset =
			!activePreset.empty() &&
			std::any_of(presets.begin(), presets.end(), [&](const AvisoPreset& preset)
			{
				return AsciiCaseInsensitiveEquals(preset.name, activePreset);
			});
		const bool hasDefaultPreset = !defaultPreset.empty();
		const bool clearDefaultAction =
			hasDefaultPreset && (!hasActivePreset || AsciiCaseInsensitiveEquals(activePreset, defaultPreset));
		const bool canChangeDefault = hasActivePreset || hasDefaultPreset;
		enum class ActionTone
		{
			Normal,
			Primary,
			Danger
		};

		const struct
		{
			const char* id;
			std::string label;
			bool enabled;
			ActionTone tone;
		} actions[] = {
			{ "runtime.preset.none", "No preset", true, ActionTone::Normal },
			{ "runtime.preset.save", "Save current", true, ActionTone::Primary },
			{ "runtime.preset.update", "Update", hasActivePreset, ActionTone::Normal },
			{ "runtime.preset.rename", "Rename", hasActivePreset, ActionTone::Normal },
			{ "runtime.preset.duplicate", "Duplicate", hasActivePreset, ActionTone::Normal },
			{ "runtime.preset.default", clearDefaultAction ? "Clear default" : "Set default", canChangeDefault, ActionTone::Normal },
			{ "runtime.preset.reset", "Reload", hasActivePreset, ActionTone::Normal },
			{ "runtime.preset.delete", "Delete", hasActivePreset, ActionTone::Danger }
		};

		const int actionGap = 3;
		const int actionWidth =
			(radar.RuntimeMenuPopupArea.Width() - (kPopupPadding * 2) - actionGap) / 2;
		for (size_t index = 0; index < _countof(actions); ++index)
		{
			const int column = static_cast<int>(index % 2);
			const int row = static_cast<int>(index / 2);
			CRect actionArea(
				radar.RuntimeMenuPopupArea.left + kPopupPadding + (column * (actionWidth + actionGap)),
				contentTop + (row * (kPopupActionHeight + actionGap)),
				radar.RuntimeMenuPopupArea.left + kPopupPadding + (column * (actionWidth + actionGap)) + actionWidth,
				contentTop + (row * (kPopupActionHeight + actionGap)) + kPopupActionHeight);
			const bool enabled = actions[index].enabled;
			const bool hover = enabled && PointInside(actionArea, mouseLocation);
			COLORREF fill = palette.buttonBackground;
			COLORREF foreground = palette.text;
			if (!enabled)
			{
				fill = palette.disabledBackground;
				foreground = palette.disabledText;
			}
			else if (actions[index].tone == ActionTone::Primary)
			{
				fill = hover ? palette.accentHover : palette.accent;
				foreground = palette.accentText;
			}
			else if (actions[index].tone == ActionTone::Danger)
			{
				fill = hover ? palette.dangerHover : palette.buttonBackground;
				foreground = hover ? RGB(255, 240, 240) : palette.dangerText;
			}
			else if (hover)
			{
				fill = palette.buttonHover;
			}
			DrawRoundedRect(hdc, actionArea, fill, palette.outerBorder, kControlCornerDiameter);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, actionArea, actions[index].label, foreground, DT_CENTER);
			if (enabled)
				addPopupScreenObject(actions[index].id, actionArea, actions[index].label.c_str());
		}
	}

	void DrawPopup()
	{
		// ----- Drawing the popup -----
		const int popupRightCandidate = radar.RuntimeMenuArea.right + kPopupGap;
		int popupLeft = popupRightCandidate;
		if (popupRightCandidate + popupWidth > bounds.right - 4)
			popupLeft = radar.RuntimeMenuArea.left - kPopupGap - popupWidth;
		popupLeft = std::clamp(
			popupLeft,
			static_cast<int>(bounds.left + 4),
			static_cast<int>(bounds.right - popupWidth - 4));

		int popupTop = radar.RuntimeMenuArea.top + 8;
		if (popupTop + popupHeight > bounds.bottom - 4)
			popupTop = bounds.bottom - popupHeight - 4;
		popupTop = (std::max)(static_cast<int>(bounds.top + 4), popupTop);
		radar.RuntimeMenuPopupArea = CRect(popupLeft, popupTop, popupLeft + popupWidth, popupTop + popupHeight);

		DrawRoundedRect(hdc, radar.RuntimeMenuPopupArea, palette.popupBackground, palette.outerBorder, kPanelCornerDiameter);
		radar.AddScreenObject(RUNTIME_MENU_POPUP, "runtime.popup", radar.RuntimeMenuPopupArea, false, title.c_str());
		const int popupClipDc = ::SaveDC(hdc);
		HRGN popupClip = ::CreateRoundRectRgn(
			radar.RuntimeMenuPopupArea.left,
			radar.RuntimeMenuPopupArea.top,
			radar.RuntimeMenuPopupArea.right + 1,
			radar.RuntimeMenuPopupArea.bottom + 1,
			kPanelCornerDiameter,
			kPanelCornerDiameter);
		if (popupClip != nullptr)
		{
			::ExtSelectClipRgn(hdc, popupClip, RGN_AND);
			::DeleteObject(popupClip);
		}
		const Gdiplus::GraphicsState popupGraphicsState = graphics.Save();
		graphics.SetClip(
			Gdiplus::Rect(
				radar.RuntimeMenuPopupArea.left,
				radar.RuntimeMenuPopupArea.top,
				radar.RuntimeMenuPopupArea.Width(),
				radar.RuntimeMenuPopupArea.Height()),
			Gdiplus::CombineModeIntersect);

		CRect titleArea(
			radar.RuntimeMenuPopupArea.left + 1,
			radar.RuntimeMenuPopupArea.top + 1,
			radar.RuntimeMenuPopupArea.right - 1,
			radar.RuntimeMenuPopupArea.top + kPopupHeaderHeight);
		FillRectColor(hdc, titleArea, palette.panelTitleBackground);
		CRect titleDivider(titleArea.left, titleArea.bottom - 1, titleArea.right, titleArea.bottom);
		FillRectColor(hdc, titleDivider, palette.divider);

		HFONT headerFont = static_cast<HFONT>(radar.RuntimeOverlayFont.GetSafeHandle());
		rowFont = headerFont;
		actionFont = static_cast<HFONT>(radar.RuntimeMenuActionFont.GetSafeHandle());

		HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, headerFont));
		CRect titleText(titleArea.left + 7, titleArea.top, titleArea.right - 27, titleArea.bottom);
		DrawTextEllipsis(hdc, titleText, insetPopup ? "Insets" : title, palette.text);
		CRect closeArea(titleArea.right - 21, titleArea.top + 3, titleArea.right - 4, titleArea.bottom - 3);
		DrawRoundedRect(
			hdc,
			closeArea,
			PointInside(closeArea, mouseLocation) ? palette.buttonHover : palette.buttonBackground,
			palette.outerBorder,
			kControlCornerDiameter);
		::SelectObject(hdc, actionFont);
		DrawTextEllipsis(hdc, closeArea, "x", palette.mutedText, DT_CENTER);
		addPopupScreenObject("runtime.close", closeArea, "Close");

		contentTop = titleArea.bottom + kPopupPadding;
		if (vsidPopup) DrawDatalink();
		else if (!insetPopup) DrawChoices();
		else if (insetPopupTooShort)
		{
			::SelectObject(hdc, actionFont);
			CRect messageArea(radar.RuntimeMenuPopupArea.left + 5, contentTop,
				radar.RuntimeMenuPopupArea.right - 5, radar.RuntimeMenuPopupArea.bottom - 4);
			DrawTextEllipsis(hdc, messageArea, "Increase radar height.", palette.mutedText, DT_CENTER);
		}
		else
		{
			DrawInsetVisibility();
			DrawInsetPresets();
			DrawPresetActions();
		}
		graphics.Restore(popupGraphicsState);
		::RestoreDC(hdc, popupClipDc);
		DrawRoundedBorder(hdc, radar.RuntimeMenuPopupArea, palette.outerBorder, kPanelCornerDiameter);
		::SelectObject(hdc, oldFont);
	}
};

void CSMRRadar::RenderRuntimeMenu(HDC hdc, Gdiplus::Graphics& graphics)
{
	if (hdc == nullptr)
		return;

	const bool dayTheme = GetUiColorTheme() == "day";
	const RuntimeMenuPalette palette = ResolveRuntimeMenuPalette(dayTheme);
	const COLORREF kOuterBorder = palette.outerBorder;
	const COLORREF kRailBackground = palette.railBackground;
	const COLORREF kTitleBackground = palette.titleBackground;
	const COLORREF kTitleStripe = palette.titleStripe;
	const COLORREF kButtonBackground = palette.buttonBackground;
	const COLORREF kButtonHover = palette.buttonHover;
	const COLORREF kAccent = palette.accent;
	const COLORREF kText = palette.text;
	const COLORREF kMutedText = palette.mutedText;
	const COLORREF kAccentText = palette.accentText;
	const COLORREF kDisabledText = palette.disabledText;

	CRect bounds(GetRadarArea());
	CRect chatArea(GetChatArea());
	bounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty() && chatArea.top > bounds.top && chatArea.top < bounds.bottom)
		bounds.bottom = chatArea.top;
	const int railHeight = RuntimeMenuMinimized ? kDragHeight : kRailHeight;
	if (bounds.Width() < kRailWidth + 8 || bounds.Height() < railHeight + 8)
		return;

	if (!RuntimeMenuPositionInitialized)
	{
		RuntimeMenuPosition.x = bounds.left + 14;
		RuntimeMenuPosition.y = bounds.top + ((bounds.Height() - railHeight) / 2);
		RuntimeMenuPositionInitialized = true;
	}

	const LONG minLeft = bounds.left + 4;
	const LONG maxLeft = bounds.right - kRailWidth - 4;
	const LONG minTop = bounds.top + 4;
	const LONG maxTop = bounds.bottom - railHeight - 4;
	const LONG renderedLeft = std::clamp(RuntimeMenuPosition.x, minLeft, maxLeft);
	const LONG renderedTop = std::clamp(RuntimeMenuPosition.y, minTop, maxTop);
	RuntimeMenuArea = CRect(
		renderedLeft,
		renderedTop,
		renderedLeft + kRailWidth,
		renderedTop + railHeight);

	// ----- Drawing the rail -----
	const int savedDc = ::SaveDC(hdc);
	::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
	::SelectObject(hdc, ::GetStockObject(DC_PEN));
	if (RuntimeOverlayFont.GetSafeHandle() != nullptr)
		::SelectObject(hdc, RuntimeOverlayFont.GetSafeHandle());
	const Gdiplus::GraphicsState initialGraphicsState = graphics.Save();
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	DrawSquareRect(hdc, RuntimeMenuArea, kRailBackground, kOuterBorder);

	CRect dragArea(
		RuntimeMenuArea.left + 1,
		RuntimeMenuArea.top + 1,
		RuntimeMenuArea.right - 1,
		RuntimeMenuArea.top + kDragHeight);
	const int dragClipDc = ::SaveDC(hdc);
	HRGN railClip = ::CreateRectRgn(
		RuntimeMenuArea.left,
		RuntimeMenuArea.top,
		RuntimeMenuArea.right + 1,
		RuntimeMenuArea.bottom + 1);
	if (railClip != nullptr)
	{
		::ExtSelectClipRgn(hdc, railClip, RGN_AND);
		::DeleteObject(railClip);
	}
	FillRectColor(hdc, dragArea, kTitleBackground);
	const int stripeDc = ::SaveDC(hdc);
	if (stripeDc != 0)
	{
		::IntersectClipRect(
			hdc,
			dragArea.left,
			dragArea.top,
			dragArea.right,
			dragArea.bottom);
		::SetDCPenColor(hdc, kTitleStripe);
		for (int x = dragArea.left - dragArea.Height(); x < dragArea.right; x += 5)
		{
			::MoveToEx(hdc, x, dragArea.bottom, nullptr);
			::LineTo(hdc, x + dragArea.Height(), dragArea.top);
			::MoveToEx(hdc, x + 1, dragArea.bottom, nullptr);
			::LineTo(hdc, x + dragArea.Height() + 1, dragArea.top);
		}
		::RestoreDC(hdc, stripeDc);
	}
	::RestoreDC(hdc, dragClipDc);
	DrawSquareBorder(hdc, RuntimeMenuArea, kOuterBorder);
	AddScreenObject(
		RUNTIME_MENU_RAIL,
		"runtime.drag",
		dragArea,
		true,
		RuntimeMenuMinimized
			? "Drag vSMR runtime menu; right-click to expand"
			: "Drag vSMR runtime menu; right-click to minimize");

	if (RuntimeMenuMinimized)
	{
		ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
		RuntimeMenuPopupScrollOffset = 0;
		RuntimeMenuPopupArea.SetRectEmpty();
		graphics.Restore(initialGraphicsState);
		::RestoreDC(hdc, savedDc);
		return;
	}

	CRect airportArea(
		RuntimeMenuArea.left + 4,
		RuntimeMenuArea.top + kDragHeight + kRailPadding,
		RuntimeMenuArea.right - 4,
		RuntimeMenuArea.top + kDragHeight + kRailPadding + kAirportRowHeight);
	const bool airportHover = PointInside(airportArea, mouseLocation);
	DrawRoundedRect(
		hdc,
		airportArea,
		airportHover ? kButtonHover : kButtonBackground,
		kOuterBorder,
		kControlCornerDiameter);
	DrawTextEllipsis(hdc, airportArea, getActiveAirport(), kText, DT_CENTER);
	AddScreenObject(
		RUNTIME_MENU_RAIL,
		"runtime.airport",
		airportArea,
		false,
		"Edit active airport");

	const std::string activeProfile = GetActiveProfileNameForEditor();
	const std::string activeMode = activeProfile.empty() ? "" : GetActiveProfileDisplayModeForEditor(activeProfile);
	const std::vector<AvisoGroup> groups = GetAvisoGroups();
	size_t visibleGroupCount = 0;
	for (const AvisoGroup& group : groups)
	{
		if (group.visible)
			++visibleGroupCount;
	}

	struct RailButton
	{
		const char* id;
		const char* icon;
		const char* tooltip;
		RuntimeMenuPopup popup;
	};
	const RailButton buttons[] = {
		{ "runtime.button.mode", "mode", "Mode", RuntimeMenuPopup::Mode },
		{ "runtime.button.groups", "groups", "Groups", RuntimeMenuPopup::Groups },
		{ "runtime.button.insets", "insets", "Insets", RuntimeMenuPopup::Insets },
		{ "runtime.button.profile", "profile", "Profile", RuntimeMenuPopup::Profile },
		{ "runtime.button.datalink", "vsid", "vSID / CPDLC", RuntimeMenuPopup::Datalink },
		{ "runtime.button.control-center", "settings", "Open Control Center", RuntimeMenuPopup::None }
	};

	int buttonTop = airportArea.bottom + kButtonGap;
	for (size_t index = 0; index < _countof(buttons); ++index)
	{
		const RailButton& button = buttons[index];
		CRect buttonArea(
			RuntimeMenuArea.left + 4,
			buttonTop,
			RuntimeMenuArea.left + 4 + kButtonSize,
			buttonTop + kButtonSize);
		const bool popupButton = button.popup != RuntimeMenuPopup::None;
		const bool open = popupButton && ActiveRuntimeMenuPopup == button.popup;
		const bool hover = PointInside(buttonArea, mouseLocation);
		const COLORREF fill = open ? kAccent : (hover ? kButtonHover : kButtonBackground);
		const COLORREF foreground = open ? kAccentText : kText;
		DrawRoundedRect(hdc, buttonArea, fill, kOuterBorder, kControlCornerDiameter);
		if (std::strcmp(button.icon, "vsid") == 0)
		{
			HFONT previousFont = static_cast<HFONT>(
				::SelectObject(hdc, RuntimeMenuActionFont.GetSafeHandle()));
			CRect vsidLabel = buttonArea;
			vsidLabel.bottom = buttonArea.CenterPoint().y;
			CRect cpdlcLabel = buttonArea;
			cpdlcLabel.top = vsidLabel.bottom;
			DrawTextEllipsis(hdc, vsidLabel, "vSID", foreground, DT_CENTER);
			DrawTextEllipsis(hdc, cpdlcLabel, "CPDLC", foreground, DT_CENTER);
			if (previousFont != nullptr)
				::SelectObject(hdc, previousFont);
		}
		else
		{
			DrawRuntimeIcon(graphics, button.icon, buttonArea, foreground);
		}

		if (popupButton)
		{
			POINT triangle[] = {
				{ buttonArea.right - 2, buttonArea.bottom - 2 },
				{ buttonArea.right - 10, buttonArea.bottom - 2 },
				{ buttonArea.right - 2, buttonArea.bottom - 10 }
			};
			::SetDCBrushColor(hdc, open ? kAccentText : kMutedText);
			::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
			::SelectObject(hdc, ::GetStockObject(NULL_PEN));
			::Polygon(hdc, triangle, _countof(triangle));
			::SelectObject(hdc, ::GetStockObject(DC_PEN));
		}

		if (button.popup == RuntimeMenuPopup::Insets)
		{
			const int appWindowIds[] = {
				APPWINDOW_AVISO - APPWINDOW_BASE,
				1,
				APPWINDOW_WEATHER - APPWINDOW_BASE,
				APPWINDOW_TIMER - APPWINDOW_BASE
			};
			const int dotY = buttonArea.bottom - 6;
			for (int dot = 0; dot < static_cast<int>(_countof(appWindowIds)); ++dot)
			{
				const auto display = appWindowDisplays.find(appWindowIds[dot]);
				const bool visible = display != appWindowDisplays.end() && display->second;
				const int dotX = buttonArea.left + 11 + (dot * 5);
				::SetDCBrushColor(hdc, visible ? kAccentText : kDisabledText);
				::SetDCPenColor(hdc, kOuterBorder);
				::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
				::SelectObject(hdc, ::GetStockObject(DC_PEN));
				::Ellipse(hdc, dotX, dotY, dotX + 4, dotY + 4);
			}
		}

		std::string tooltip = button.tooltip;
		if (button.popup == RuntimeMenuPopup::Mode && !activeMode.empty())
			tooltip += ": " + activeMode;
		else if (button.popup == RuntimeMenuPopup::Groups)
			tooltip += ": " + std::to_string(visibleGroupCount) + "/" + std::to_string(groups.size()) + " visible";
		else if (button.popup == RuntimeMenuPopup::Profile && !activeProfile.empty())
			tooltip += ": " + activeProfile;
		AddScreenObject(RUNTIME_MENU_RAIL, button.id, buttonArea, false, tooltip.c_str());
		buttonTop += kButtonSize + kButtonGap;
	}

	RuntimeMenuPopupArea.SetRectEmpty();
	if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::None)
	{
		graphics.Restore(initialGraphicsState);
		::RestoreDC(hdc, savedDc);
		return;
	}

	RuntimeMenuPopupRenderer popup{ *this, hdc, graphics, palette, bounds, activeProfile, activeMode, groups };
	if (popup.BuildPopup()) popup.DrawPopup();
	graphics.Restore(initialGraphicsState);
	::RestoreDC(hdc, savedDc);
}
