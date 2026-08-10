#include "stdafx.h"
#include "InsetWindow.h"
#include "SMRRadar.hpp"
#include "VsmrControlCenterDialog.hpp"

#include <cstdlib>
#include <limits>

extern CPoint mouseLocation;

namespace
{
	constexpr int kRailWidth = 48;
	constexpr int kDragHeight = 10;
	constexpr int kRailPadding = 3;
	constexpr int kButtonSize = 40;
	constexpr int kButtonGap = 3;
	constexpr int kAirportRowHeight = 22;
	constexpr int kRailHeight =
		kDragHeight +
		(kRailPadding * 2) +
		kAirportRowHeight +
		(kButtonSize * 5) +
		(kButtonGap * 5);
	constexpr int kPopupGap = 4;
	constexpr int kPopupHeaderHeight = 23;
	constexpr int kPopupRowHeight = 28;
	constexpr int kPopupPadding = 3;
	constexpr int kPopupPagerHeight = 24;
	constexpr int kPopupControlHeight = 22;
	constexpr int kPopupLinkedSlotHeight = 26;
	constexpr int kPopupActionHeight = 22;
	constexpr int kControlCornerDiameter = 6;
	constexpr int kPanelCornerDiameter = 8;
	constexpr int kInsetPopupWidth = 196;
	constexpr int kStandardPopupWidth = 170;

	const COLORREF kOuterBorder = RGB(5, 7, 8);
	const COLORREF kRailBackground = RGB(30, 40, 43);
	const COLORREF kPopupBackground = RGB(32, 42, 45);
	const COLORREF kTitleBackground = RGB(9, 12, 13);
	const COLORREF kTitleStripe = RGB(23, 29, 31);
	const COLORREF kPanelTitleBackground = RGB(34, 45, 48);
	const COLORREF kButtonBackground = RGB(41, 57, 59);
	const COLORREF kListBackground = RGB(41, 56, 59);
	const COLORREF kCardBackground = RGB(39, 52, 56);
	const COLORREF kButtonHover = RGB(53, 71, 75);
	const COLORREF kAccent = RGB(80, 150, 180);
	const COLORREF kAccentHover = RGB(98, 165, 193);
	const COLORREF kText = RGB(208, 217, 220);
	const COLORREF kMutedText = RGB(143, 161, 166);
	const COLORREF kAccentText = RGB(244, 248, 249);
	const COLORREF kDivider = RGB(17, 23, 25);
	const COLORREF kDisabledBackground = RGB(31, 42, 45);
	const COLORREF kDisabledText = RGB(91, 107, 112);
	const COLORREF kDangerText = RGB(229, 167, 167);
	const COLORREF kDangerHover = RGB(112, 51, 55);

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

	void DrawRoundedBorder(HDC hdc, const CRect& rect, COLORREF border, int diameter)
	{
		::SetDCPenColor(hdc, border);
		::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
		::SelectObject(hdc, ::GetStockObject(DC_PEN));
		::RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
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

		if (kind == "mode")
		{
			graphics.DrawEllipse(&pen, centerX - 8.0f, centerY - 8.0f, 16.0f, 16.0f);
			graphics.DrawLine(&pen, centerX, centerY - 8.0f, centerX, centerY - 5.0f);
			graphics.DrawLine(&pen, centerX, centerY + 5.0f, centerX, centerY + 8.0f);
			graphics.DrawLine(&pen, centerX - 8.0f, centerY, centerX - 5.0f, centerY);
			graphics.DrawLine(&pen, centerX + 5.0f, centerY, centerX + 8.0f, centerY);
			graphics.DrawLine(&pen, centerX, centerY, centerX + 4.0f, centerY - 4.0f);
			graphics.DrawEllipse(&pen, centerX - 1.5f, centerY - 1.5f, 3.0f, 3.0f);
			return;
		}

		if (kind == "groups")
		{
			const Gdiplus::PointF first(centerX - 5.0f, centerY - 4.0f);
			const Gdiplus::PointF second(centerX + 5.0f, centerY - 4.0f);
			const Gdiplus::PointF third(centerX, centerY + 5.0f);
			graphics.DrawLine(&pen, first, second);
			graphics.DrawLine(&pen, first.X + 1.5f, first.Y + 1.5f, third.X - 1.0f, third.Y - 2.0f);
			graphics.DrawLine(&pen, second.X - 1.5f, second.Y + 1.5f, third.X + 1.0f, third.Y - 2.0f);
			graphics.DrawEllipse(&pen, first.X - 2.5f, first.Y - 2.5f, 5.0f, 5.0f);
			graphics.DrawEllipse(&pen, second.X - 2.5f, second.Y - 2.5f, 5.0f, 5.0f);
			graphics.DrawEllipse(&pen, third.X - 2.5f, third.Y - 2.5f, 5.0f, 5.0f);
			return;
		}

		if (kind == "insets")
		{
			graphics.DrawRectangle(&pen, centerX - 8.0f, centerY - 7.0f, 16.0f, 14.0f);
			graphics.DrawRectangle(&pen, centerX - 4.0f, centerY - 4.0f, 8.0f, 8.0f);
			graphics.DrawLine(&pen, centerX - 8.0f, centerY - 1.0f, centerX - 4.0f, centerY - 1.0f);
			graphics.DrawLine(&pen, centerX + 4.0f, centerY + 1.0f, centerX + 8.0f, centerY + 1.0f);
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

		// Control Center/settings gear.
		graphics.DrawEllipse(&pen, centerX - 7.0f, centerY - 7.0f, 14.0f, 14.0f);
		graphics.DrawEllipse(&pen, centerX - 3.0f, centerY - 3.0f, 6.0f, 6.0f);
		for (int i = 0; i < 8; ++i)
		{
			const double angle = (static_cast<double>(i) * M_PI) / 4.0;
			const float innerX = centerX + static_cast<float>(7.0 * std::cos(angle));
			const float innerY = centerY + static_cast<float>(7.0 * std::sin(angle));
			const float outerX = centerX + static_cast<float>(9.2 * std::cos(angle));
			const float outerY = centerY + static_cast<float>(9.2 * std::sin(angle));
			graphics.DrawLine(&pen, innerX, innerY, outerX, outerY);
		}
	}

	bool ParseIndexedObjectId(const char* objectId, const char* prefix, size_t& outIndex)
	{
		if (objectId == nullptr || prefix == nullptr)
			return false;

		const size_t prefixLength = std::strlen(prefix);
		if (std::strncmp(objectId, prefix, prefixLength) != 0)
			return false;

		const char* number = objectId + prefixLength;
		if (*number == '\0')
			return false;

		char* end = nullptr;
		const unsigned long parsed = std::strtoul(number, &end, 10);
		if (end == number || *end != '\0' || parsed > static_cast<unsigned long>((std::numeric_limits<size_t>::max)()))
			return false;

		outIndex = static_cast<size_t>(parsed);
		return true;
	}

	bool EqualsNoCase(const std::string& left, const std::string& right)
	{
		return left.size() == right.size() && _stricmp(left.c_str(), right.c_str()) == 0;
	}

	std::string MakeUniquePresetName(const std::vector<CSMRRadar::AvisoPreset>& presets, const std::string& requested)
	{
		auto exists = [&](const std::string& candidate)
		{
			for (const CSMRRadar::AvisoPreset& preset : presets)
			{
				if (EqualsNoCase(preset.name, candidate))
					return true;
			}
			return false;
		};

		if (!requested.empty() && !exists(requested))
			return requested;

		const std::string base = requested.empty() ? "Inset Preset" : requested;
		for (int suffix = 2; suffix < 10000; ++suffix)
		{
			const std::string candidate = base + " " + std::to_string(suffix);
			if (!exists(candidate))
				return candidate;
		}
		return base + " " + std::to_string(::GetTickCount());
	}

	bool TryParseAsrCoordinate(const char* text, LONG& value)
	{
		if (text == nullptr || text[0] == '\0')
			return false;

		char* end = nullptr;
		const long parsed = std::strtol(text, &end, 10);
		if (end == text || *end != '\0' || parsed < -100000 || parsed > 100000)
			return false;
		value = static_cast<LONG>(parsed);
		return true;
	}
}

void CSMRRadar::RenderRuntimeMenu(HDC hdc, Gdiplus::Graphics& graphics)
{
	if (hdc == nullptr)
		return;

	CRect bounds(GetRadarArea());
	CRect chatArea(GetChatArea());
	bounds.NormalizeRect();
	chatArea.NormalizeRect();
	if (!chatArea.IsRectEmpty() && chatArea.top > bounds.top && chatArea.top < bounds.bottom)
		bounds.bottom = chatArea.top;
	if (bounds.Width() < kRailWidth + 8 || bounds.Height() < kRailHeight + 8)
		return;

	if (!RuntimeMenuPositionInitialized)
	{
		RuntimeMenuPosition.x = bounds.left + 14;
		RuntimeMenuPosition.y = bounds.top + ((bounds.Height() - kRailHeight) / 2);
		RuntimeMenuPositionInitialized = true;
	}

	const LONG minLeft = bounds.left + 4;
	const LONG maxLeft = bounds.right - kRailWidth - 4;
	const LONG minTop = bounds.top + 4;
	const LONG maxTop = bounds.bottom - kRailHeight - 4;
	const LONG renderedLeft = std::clamp(RuntimeMenuPosition.x, minLeft, maxLeft);
	const LONG renderedTop = std::clamp(RuntimeMenuPosition.y, minTop, maxTop);
	RuntimeMenuArea = CRect(
		renderedLeft,
		renderedTop,
		renderedLeft + kRailWidth,
		renderedTop + kRailHeight);

	const int savedDc = ::SaveDC(hdc);
	::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
	::SelectObject(hdc, ::GetStockObject(DC_PEN));
	if (RuntimeOverlayFont.GetSafeHandle() != nullptr)
		::SelectObject(hdc, RuntimeOverlayFont.GetSafeHandle());
	const Gdiplus::GraphicsState initialGraphicsState = graphics.Save();
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	DrawRoundedRect(hdc, RuntimeMenuArea, kRailBackground, kOuterBorder, kPanelCornerDiameter);

	CRect dragArea(
		RuntimeMenuArea.left + 1,
		RuntimeMenuArea.top + 1,
		RuntimeMenuArea.right - 1,
		RuntimeMenuArea.top + kDragHeight);
	const int dragClipDc = ::SaveDC(hdc);
	HRGN railClip = ::CreateRoundRectRgn(
		RuntimeMenuArea.left,
		RuntimeMenuArea.top,
		RuntimeMenuArea.right + 1,
		RuntimeMenuArea.bottom + 1,
		kPanelCornerDiameter,
		kPanelCornerDiameter);
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
	DrawRoundedBorder(hdc, RuntimeMenuArea, kOuterBorder, kPanelCornerDiameter);
	AddScreenObject(RUNTIME_MENU_RAIL, "runtime.drag", dragArea, true, "Drag vSMR runtime menu");

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
		const bool popupButton = index < 4;
		const bool open = popupButton && ActiveRuntimeMenuPopup == button.popup;
		const bool hover = PointInside(buttonArea, mouseLocation);
		const COLORREF fill = open ? kAccent : (hover ? kButtonHover : kButtonBackground);
		const COLORREF foreground = open ? kAccentText : kText;
		DrawRoundedRect(hdc, buttonArea, fill, kOuterBorder, kControlCornerDiameter);
		DrawRuntimeIcon(graphics, button.icon, buttonArea, foreground);

		if (popupButton)
		{
			POINT triangle[] = {
				{ buttonArea.right - 2, buttonArea.bottom - 2 },
				{ buttonArea.right - 10, buttonArea.bottom - 2 },
				{ buttonArea.right - 2, buttonArea.bottom - 10 }
			};
			::SetDCBrushColor(hdc, open ? kAccentText : RGB(129, 147, 153));
			::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
			::SelectObject(hdc, ::GetStockObject(NULL_PEN));
			::Polygon(hdc, triangle, _countof(triangle));
			::SelectObject(hdc, ::GetStockObject(DC_PEN));
		}

		if (index == 2)
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
				::SetDCBrushColor(hdc, visible ? RGB(237, 248, 251) : RGB(88, 102, 106));
				::SetDCPenColor(hdc, RGB(9, 16, 18));
				::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
				::SelectObject(hdc, ::GetStockObject(DC_PEN));
				::Ellipse(hdc, dotX, dotY, dotX + 4, dotY + 4);
			}
		}

		std::string tooltip = button.tooltip;
		if (index == 0 && !activeMode.empty())
			tooltip += ": " + activeMode;
		else if (index == 1)
			tooltip += ": " + std::to_string(visibleGroupCount) + "/" + std::to_string(groups.size()) + " visible";
		else if (index == 3 && !activeProfile.empty())
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

	std::vector<RuntimePopupEntry> entries;
	std::string title;
	if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::Mode)
	{
		title = "Mode";
		const std::vector<DisplayModeSettings> modes = GetProfileDisplayModesForEditor(activeProfile);
		for (size_t index = 0; index < modes.size(); ++index)
		{
			RuntimePopupEntry entry;
			entry.id = "runtime.mode." + std::to_string(index);
			entry.label = modes[index].name;
			entry.indicator = RuntimeIndicator::Selection;
			entry.active = EqualsNoCase(modes[index].name, activeMode);
			entries.push_back(entry);
		}
	}
	else if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::Groups)
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
	else if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::Profile)
	{
		title = "Profile";
		const std::vector<std::string> profiles = GetOrderedProfileNamesForUi();
		for (size_t index = 0; index < profiles.size(); ++index)
		{
			RuntimePopupEntry entry;
			entry.id = "runtime.profile." + std::to_string(index);
			entry.label = profiles[index];
			entry.indicator = RuntimeIndicator::Selection;
			entry.active = EqualsNoCase(profiles[index], activeProfile);
			entries.push_back(entry);
		}
	}

	const bool insetPopup = ActiveRuntimeMenuPopup == RuntimeMenuPopup::Insets;
	const std::vector<AvisoPreset> insetPresets = insetPopup
		? GetAvisoPresets()
		: std::vector<AvisoPreset>();
	const int popupWidth = insetPopup ? kInsetPopupWidth : kStandardPopupWidth;
	if (bounds.Width() < popupWidth + 8)
	{
		RuntimeMenuPopupArea.SetRectEmpty();
		graphics.Restore(initialGraphicsState);
		::RestoreDC(hdc, savedDc);
		return;
	}
	int popupHeight = 0;
	int visibleRows = 0;
	bool showPager = false;
	bool insetPopupTooShort = false;
	if (!insetPopup)
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
			kPopupLinkedSlotHeight +
			(4 * kPopupActionHeight) +
			(3 * 3) +
			kPopupPadding;
		insetPopupTooShort = popupHeight > bounds.Height() - 8;
		if (insetPopupTooShort)
			popupHeight = kPopupHeaderHeight + 42;
	}

	const int popupRightCandidate = RuntimeMenuArea.right + kPopupGap;
	int popupLeft = popupRightCandidate;
	if (popupRightCandidate + popupWidth > bounds.right - 4)
		popupLeft = RuntimeMenuArea.left - kPopupGap - popupWidth;
	popupLeft = std::clamp(
		popupLeft,
		static_cast<int>(bounds.left + 4),
		static_cast<int>(bounds.right - popupWidth - 4));

	int popupTop = RuntimeMenuArea.top + 8;
	if (popupTop + popupHeight > bounds.bottom - 4)
		popupTop = bounds.bottom - popupHeight - 4;
	popupTop = (std::max)(static_cast<int>(bounds.top + 4), popupTop);
	RuntimeMenuPopupArea = CRect(popupLeft, popupTop, popupLeft + popupWidth, popupTop + popupHeight);

	DrawRoundedRect(hdc, RuntimeMenuPopupArea, kPopupBackground, kOuterBorder, kPanelCornerDiameter);
	AddScreenObject(RUNTIME_MENU_POPUP, "runtime.popup", RuntimeMenuPopupArea, false, title.c_str());
	const int popupClipDc = ::SaveDC(hdc);
	HRGN popupClip = ::CreateRoundRectRgn(
		RuntimeMenuPopupArea.left,
		RuntimeMenuPopupArea.top,
		RuntimeMenuPopupArea.right + 1,
		RuntimeMenuPopupArea.bottom + 1,
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
			RuntimeMenuPopupArea.left,
			RuntimeMenuPopupArea.top,
			RuntimeMenuPopupArea.Width(),
			RuntimeMenuPopupArea.Height()),
		Gdiplus::CombineModeIntersect);
	auto addPopupScreenObject = [&](const char* id, const CRect& area, const char* tooltip)
	{
		CRect clippedArea;
		if (::IntersectRect(&clippedArea, &area, &RuntimeMenuPopupArea) && !clippedArea.IsRectEmpty())
			AddScreenObject(RUNTIME_MENU_POPUP, id, clippedArea, false, tooltip);
	};
	auto beginRoundedClip = [&](const CRect& area)
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
	};

	CRect titleArea(
		RuntimeMenuPopupArea.left + 1,
		RuntimeMenuPopupArea.top + 1,
		RuntimeMenuPopupArea.right - 1,
		RuntimeMenuPopupArea.top + kPopupHeaderHeight);
	FillRectColor(hdc, titleArea, kPanelTitleBackground);
	CRect titleDivider(titleArea.left, titleArea.bottom - 1, titleArea.right, titleArea.bottom);
	FillRectColor(hdc, titleDivider, RGB(17, 23, 25));

	HFONT headerFont = static_cast<HFONT>(RuntimeOverlayFont.GetSafeHandle());
	HFONT rowFont = headerFont;
	HFONT actionFont = static_cast<HFONT>(RuntimeMenuActionFont.GetSafeHandle());

	HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, headerFont));
	CRect titleText(titleArea.left + 7, titleArea.top, titleArea.right - 27, titleArea.bottom);
	DrawTextEllipsis(hdc, titleText, insetPopup ? "Insets" : title, RGB(217, 226, 228));
	CRect closeArea(titleArea.right - 21, titleArea.top + 3, titleArea.right - 4, titleArea.bottom - 3);
	DrawRoundedRect(
		hdc,
		closeArea,
		PointInside(closeArea, mouseLocation) ? kButtonHover : kButtonBackground,
		kOuterBorder,
		kControlCornerDiameter);
	::SelectObject(hdc, actionFont);
	DrawTextEllipsis(hdc, closeArea, "x", RGB(188, 200, 204), DT_CENTER);
	addPopupScreenObject("runtime.close", closeArea, "Close");

	auto drawChoiceRow = [&](const RuntimePopupEntry& entry, const CRect& rowArea)
	{
		const bool hover = entry.enabled && PointInside(rowArea, mouseLocation);
		COLORREF fill = entry.enabled ? kListBackground : kDisabledBackground;
		if (entry.active && entry.indicator == RuntimeIndicator::Selection)
			fill = kAccent;
		else if (hover)
			fill = kButtonHover;
		FillRectColor(hdc, rowArea, fill);
		CRect divider(rowArea.left, rowArea.bottom - 1, rowArea.right, rowArea.bottom);
		FillRectColor(hdc, divider, kDivider);

		const COLORREF foreground =
			!entry.enabled ? kDisabledText :
			(entry.active && entry.indicator == RuntimeIndicator::Selection ? kAccentText : kText);
		CRect indicatorArea(rowArea.left + 3, rowArea.top, rowArea.left + 20, rowArea.bottom);
		if (entry.indicator == RuntimeIndicator::Selection)
			DrawRuntimeSelectionIndicator(graphics, indicatorArea, entry.active, foreground);
		else if (entry.indicator == RuntimeIndicator::Visibility)
			DrawRuntimeVisibilityIndicator(graphics, indicatorArea, entry.active, entry.active ? foreground : kMutedText);

		::SelectObject(hdc, rowFont);
		CRect labelArea(rowArea.left + 24, rowArea.top, rowArea.right - 5, rowArea.bottom);
		DrawTextEllipsis(hdc, labelArea, entry.label, foreground);
		addPopupScreenObject(entry.id.c_str(), rowArea, entry.label.c_str());
	};

	int contentTop = titleArea.bottom + kPopupPadding;
	if (!insetPopup)
	{
		if (entries.empty())
		{
			::SelectObject(hdc, actionFont);
			CRect emptyArea(
				RuntimeMenuPopupArea.left + 4,
				contentTop,
				RuntimeMenuPopupArea.right - 4,
				RuntimeMenuPopupArea.bottom - 4);
			const std::string emptyText =
				ActiveRuntimeMenuPopup == RuntimeMenuPopup::Groups ? "No AVISO groups." :
				ActiveRuntimeMenuPopup == RuntimeMenuPopup::Mode ? "No modes in this profile." :
				"No profiles.";
			DrawTextEllipsis(hdc, emptyArea, emptyText, kMutedText, DT_CENTER);
		}
		else
		{
			const int maximumOffset = (std::max)(0, static_cast<int>(entries.size()) - visibleRows);
			RuntimeMenuPopupScrollOffset = std::clamp(RuntimeMenuPopupScrollOffset, 0, maximumOffset);
			const int endIndex = (std::min)(
				static_cast<int>(entries.size()),
				RuntimeMenuPopupScrollOffset + visibleRows);
			const CRect listArea(
				RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + (visibleRows * kPopupRowHeight));
			DrawRoundedRect(
				hdc,
				listArea,
				kListBackground,
				kOuterBorder,
				kPanelCornerDiameter);
			const int listClipState = beginRoundedClip(listArea);

			for (int index = RuntimeMenuPopupScrollOffset; index < endIndex; ++index)
			{
				CRect rowArea(
					RuntimeMenuPopupArea.left + kPopupPadding,
					contentTop,
					RuntimeMenuPopupArea.right - kPopupPadding,
					contentTop + kPopupRowHeight);
				drawChoiceRow(entries[static_cast<size_t>(index)], rowArea);
				contentTop += kPopupRowHeight;
			}
			::RestoreDC(hdc, listClipState);
			DrawRoundedBorder(hdc, listArea, kOuterBorder, kPanelCornerDiameter);

			if (showPager)
			{
				CRect previousArea(
					RuntimeMenuPopupArea.left + kPopupPadding,
					contentTop + 1,
					RuntimeMenuPopupArea.CenterPoint().x - 1,
					contentTop + 1 + kPopupControlHeight);
				CRect nextArea(
					RuntimeMenuPopupArea.CenterPoint().x + 1,
					contentTop + 1,
					RuntimeMenuPopupArea.right - kPopupPadding,
					contentTop + 1 + kPopupControlHeight);
				const bool canPrevious = RuntimeMenuPopupScrollOffset > 0;
				const bool canNext = RuntimeMenuPopupScrollOffset < maximumOffset;
				DrawRoundedRect(hdc, previousArea, canPrevious ? kButtonBackground : kDisabledBackground, kOuterBorder, kControlCornerDiameter);
				DrawRoundedRect(hdc, nextArea, canNext ? kButtonBackground : kDisabledBackground, kOuterBorder, kControlCornerDiameter);
				::SelectObject(hdc, actionFont);
				DrawTextEllipsis(hdc, previousArea, "Previous", canPrevious ? kText : kDisabledText, DT_CENTER);
				DrawTextEllipsis(hdc, nextArea, "Next", canNext ? kText : kDisabledText, DT_CENTER);
				if (canPrevious)
					addPopupScreenObject("runtime.page.previous", previousArea, "Previous choices");
				if (canNext)
					addPopupScreenObject("runtime.page.next", nextArea, "Next choices");
			}
		}
	}
	else if (insetPopupTooShort)
	{
		::SelectObject(hdc, actionFont);
		CRect messageArea(
			RuntimeMenuPopupArea.left + 5,
			contentTop,
			RuntimeMenuPopupArea.right - 5,
			RuntimeMenuPopupArea.bottom - 4);
		DrawTextEllipsis(hdc, messageArea, "Increase radar height.", kMutedText, DT_CENTER);
	}
	else
	{
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
			RuntimeMenuPopupArea.left + kPopupPadding,
			contentTop,
			RuntimeMenuPopupArea.right - kPopupPadding,
			contentTop + (static_cast<int>(_countof(insetRows)) * kPopupRowHeight));
		DrawRoundedRect(
			hdc,
			insetListArea,
			kListBackground,
			kOuterBorder,
			kPanelCornerDiameter);
		const int insetListClipState = beginRoundedClip(insetListArea);
		for (const auto& inset : insetRows)
		{
			const auto display = appWindowDisplays.find(inset.appWindowId);
			RuntimePopupEntry entry;
			entry.id = inset.id;
			entry.label = inset.label;
			entry.indicator = RuntimeIndicator::Visibility;
			entry.active = display != appWindowDisplays.end() && display->second;
			CRect rowArea(
				RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				RuntimeMenuPopupArea.right - kPopupPadding,
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
				PointInside(resetArea, mouseLocation) ? kButtonHover : kButtonBackground,
				kOuterBorder,
				kControlCornerDiameter);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, resetArea, "Reset", kText, DT_CENTER);
			addPopupScreenObject(inset.resetId, resetArea, "Reset this inset view");
			contentTop += kPopupRowHeight;
		}
		::RestoreDC(hdc, insetListClipState);
		DrawRoundedBorder(hdc, insetListArea, kOuterBorder, kPanelCornerDiameter);

		::SelectObject(hdc, actionFont);
		CRect sectionArea(
			RuntimeMenuPopupArea.left + 5,
			contentTop,
			RuntimeMenuPopupArea.right - 5,
			contentTop + 19);
		DrawTextEllipsis(hdc, sectionArea, "PRESET", RGB(159, 176, 181));
		contentTop += 19;

		const std::vector<AvisoPreset>& presets = insetPresets;
		const std::string activePreset = GetActiveAvisoPresetName();
		const std::string defaultPreset = GetDefaultAvisoPresetName();
		const int presetRows = (std::min)(4, static_cast<int>(presets.size()));
		const int maximumPresetOffset = (std::max)(0, static_cast<int>(presets.size()) - presetRows);
		RuntimeMenuPopupScrollOffset = std::clamp(RuntimeMenuPopupScrollOffset, 0, maximumPresetOffset);
		if (presets.empty())
		{
			CRect emptyArea(
				RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + 32);
			DrawTextEllipsis(hdc, emptyArea, "No inset presets.", kMutedText, DT_CENTER);
			contentTop += 32;
		}
		else
		{
			const CRect presetListArea(
				RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop,
				RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + (presetRows * kPopupRowHeight));
			DrawRoundedRect(
				hdc,
				presetListArea,
				kListBackground,
				kOuterBorder,
				kPanelCornerDiameter);
			const int presetListClipState = beginRoundedClip(presetListArea);
			for (int row = 0; row < presetRows; ++row)
			{
				const int presetIndex = RuntimeMenuPopupScrollOffset + row;
				RuntimePopupEntry entry;
				entry.id = "runtime.preset." + std::to_string(presetIndex);
				entry.label = presets[static_cast<size_t>(presetIndex)].name;
				entry.indicator = RuntimeIndicator::Selection;
				entry.active = EqualsNoCase(entry.label, activePreset);
				CRect rowArea(
					RuntimeMenuPopupArea.left + kPopupPadding,
					contentTop,
					RuntimeMenuPopupArea.right - kPopupPadding,
					contentTop + kPopupRowHeight);
				drawChoiceRow(entry, rowArea);
				contentTop += kPopupRowHeight;
			}
			::RestoreDC(hdc, presetListClipState);
			DrawRoundedBorder(hdc, presetListArea, kOuterBorder, kPanelCornerDiameter);
		}

		if (presets.size() > 4)
		{
			CRect previousArea(
				RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop + 1,
				RuntimeMenuPopupArea.CenterPoint().x - 1,
				contentTop + 1 + kPopupControlHeight);
			CRect nextArea(
				RuntimeMenuPopupArea.CenterPoint().x + 1,
				contentTop + 1,
				RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + 1 + kPopupControlHeight);
			const bool canPrevious = RuntimeMenuPopupScrollOffset > 0;
			const bool canNext = RuntimeMenuPopupScrollOffset < maximumPresetOffset;
			DrawRoundedRect(hdc, previousArea, canPrevious ? kButtonBackground : kDisabledBackground, kOuterBorder, kControlCornerDiameter);
			DrawRoundedRect(hdc, nextArea, canNext ? kButtonBackground : kDisabledBackground, kOuterBorder, kControlCornerDiameter);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, previousArea, "Previous", canPrevious ? kText : kDisabledText, DT_CENTER);
			DrawTextEllipsis(hdc, nextArea, "Next", canNext ? kText : kDisabledText, DT_CENTER);
			if (canPrevious)
				addPopupScreenObject("runtime.preset.page.previous", previousArea, "Previous presets");
			if (canNext)
				addPopupScreenObject("runtime.preset.page.next", nextArea, "Next presets");
			contentTop += kPopupPagerHeight;
		}

		const bool hasActivePreset =
			!activePreset.empty() &&
			std::any_of(presets.begin(), presets.end(), [&](const AvisoPreset& preset)
			{
				return EqualsNoCase(preset.name, activePreset);
			});
		const bool hasDefaultPreset = !defaultPreset.empty();
		const bool clearDefaultAction =
			hasDefaultPreset && (!hasActivePreset || EqualsNoCase(activePreset, defaultPreset));
		const bool canChangeDefault = hasActivePreset || hasDefaultPreset;
		CRect linkedArea(
			RuntimeMenuPopupArea.left + kPopupPadding,
			contentTop + 2,
			RuntimeMenuPopupArea.right - kPopupPadding,
			contentTop + 2 + kPopupControlHeight);
		DrawRoundedRect(
			hdc,
			linkedArea,
			hasActivePreset && PointInside(linkedArea, mouseLocation) ? kButtonHover : kCardBackground,
			RGB(17, 23, 25),
			kControlCornerDiameter);
		CRect linkedIndicator(linkedArea.left + 4, linkedArea.top, linkedArea.left + 21, linkedArea.bottom);
		DrawRuntimeSelectionIndicator(
			graphics,
			linkedIndicator,
			hasActivePreset && IsAvisoPresetLinkedMovementEnabled(),
			hasActivePreset ? kText : kDisabledText);
		::SelectObject(hdc, rowFont);
		CRect linkedText(linkedArea.left + 25, linkedArea.top, linkedArea.right - 4, linkedArea.bottom);
		DrawTextEllipsis(hdc, linkedText, "Linked movement", hasActivePreset ? kText : kDisabledText);
		if (hasActivePreset)
			addPopupScreenObject("runtime.preset.linked", linkedArea, "Toggle linked movement");
		contentTop += kPopupLinkedSlotHeight;

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
			(RuntimeMenuPopupArea.Width() - (kPopupPadding * 2) - actionGap) / 2;
		for (size_t index = 0; index < _countof(actions); ++index)
		{
			const int column = static_cast<int>(index % 2);
			const int row = static_cast<int>(index / 2);
			CRect actionArea(
				RuntimeMenuPopupArea.left + kPopupPadding + (column * (actionWidth + actionGap)),
				contentTop + (row * (kPopupActionHeight + actionGap)),
				RuntimeMenuPopupArea.left + kPopupPadding + (column * (actionWidth + actionGap)) + actionWidth,
				contentTop + (row * (kPopupActionHeight + actionGap)) + kPopupActionHeight);
			const bool enabled = actions[index].enabled;
			const bool hover = enabled && PointInside(actionArea, mouseLocation);
			COLORREF fill = kButtonBackground;
			COLORREF foreground = kText;
			if (!enabled)
			{
				fill = kDisabledBackground;
				foreground = kDisabledText;
			}
			else if (actions[index].tone == ActionTone::Primary)
			{
				fill = hover ? kAccentHover : kAccent;
				foreground = kAccentText;
			}
			else if (actions[index].tone == ActionTone::Danger)
			{
				fill = hover ? kDangerHover : kButtonBackground;
				foreground = hover ? RGB(255, 240, 240) : kDangerText;
			}
			else if (hover)
			{
				fill = kButtonHover;
			}
			DrawRoundedRect(hdc, actionArea, fill, kOuterBorder, kControlCornerDiameter);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, actionArea, actions[index].label, foreground, DT_CENTER);
			if (enabled)
				addPopupScreenObject(actions[index].id, actionArea, actions[index].label.c_str());
		}
	}

	graphics.Restore(popupGraphicsState);
	::RestoreDC(hdc, popupClipDc);
	DrawRoundedBorder(hdc, RuntimeMenuPopupArea, kOuterBorder, kPanelCornerDiameter);
	graphics.Restore(initialGraphicsState);
	::SelectObject(hdc, oldFont);
	::RestoreDC(hdc, savedDc);
}

bool CSMRRadar::HandleRuntimeMenuClick(int objectType, const char* objectId, POINT point, RECT area, int button)
{
	UNREFERENCED_PARAMETER(point);
	if (objectType != RUNTIME_MENU_RAIL && objectType != RUNTIME_MENU_POPUP)
		return false;

	auto syncControlCenter = [&](const std::string& reason = "runtime")
	{
		if (VsmrControlCenterDialog != nullptr)
			VsmrControlCenterDialog->SyncFromRadar(reason);
	};

	const char* id = objectId != nullptr ? objectId : "";
	if (button != BUTTON_LEFT)
	{
		if (objectType == RUNTIME_MENU_POPUP || std::strcmp(id, "runtime.drag") == 0)
			CloseRuntimeMenuPopup();
		return true;
	}

	if (objectType == RUNTIME_MENU_RAIL)
	{
		auto togglePopup = [&](RuntimeMenuPopup popup)
		{
			ActiveRuntimeMenuPopup = ActiveRuntimeMenuPopup == popup ? RuntimeMenuPopup::None : popup;
			RuntimeMenuPopupScrollOffset = 0;
			RequestRefresh();
		};

		if (std::strcmp(id, "runtime.airport") == 0)
		{
			CloseRuntimeMenuPopup();
			GetPlugIn()->OpenPopupEdit(area, RIMCAS_ACTIVE_AIRPORT_FUNC, getActiveAirport().c_str());
		}
		else if (std::strcmp(id, "runtime.button.mode") == 0)
			togglePopup(RuntimeMenuPopup::Mode);
		else if (std::strcmp(id, "runtime.button.groups") == 0)
			togglePopup(RuntimeMenuPopup::Groups);
		else if (std::strcmp(id, "runtime.button.insets") == 0)
			togglePopup(RuntimeMenuPopup::Insets);
		else if (std::strcmp(id, "runtime.button.profile") == 0)
			togglePopup(RuntimeMenuPopup::Profile);
		else if (std::strcmp(id, "runtime.button.control-center") == 0)
		{
			CloseRuntimeMenuPopup();
			OpenVsmrControlCenterWindow();
		}
		return true;
	}

	if (std::strcmp(id, "runtime.close") == 0)
	{
		CloseRuntimeMenuPopup();
		return true;
	}
	if (std::strcmp(id, "runtime.popup") == 0)
		return true;
	if (std::strcmp(id, "runtime.page.previous") == 0)
	{
		RuntimeMenuPopupScrollOffset = (std::max)(0, RuntimeMenuPopupScrollOffset - 5);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.page.next") == 0)
	{
		RuntimeMenuPopupScrollOffset += 5;
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.preset.page.previous") == 0)
	{
		RuntimeMenuPopupScrollOffset = (std::max)(0, RuntimeMenuPopupScrollOffset - 4);
		RequestRefresh();
		return true;
	}
	if (std::strcmp(id, "runtime.preset.page.next") == 0)
	{
		RuntimeMenuPopupScrollOffset += 4;
		RequestRefresh();
		return true;
	}

	size_t index = 0;
	if (ParseIndexedObjectId(id, "runtime.mode.", index))
	{
		const std::string activeProfile = GetActiveProfileNameForEditor();
		const std::vector<DisplayModeSettings> modes = GetProfileDisplayModesForEditor(activeProfile);
		if (index < modes.size())
		{
			if (SetProfileDisplayModeActiveForEditor(activeProfile, modes[index].name))
			{
				syncControlCenter("mode");
			}
			else
			{
				GetPlugIn()->DisplayUserMessage(
					"vSMR", "Display mode",
					"The display mode could not be saved. vSMR reloaded the current file.",
					true, true, false, false, false);
			}
		}
		CloseRuntimeMenuPopup();
		return true;
	}
	if (ParseIndexedObjectId(id, "runtime.profile.", index))
	{
		const std::vector<std::string> profiles = GetOrderedProfileNamesForUi();
		if (index < profiles.size())
		{
			if (SetActiveProfileForEditor(profiles[index], false))
			{
				syncControlCenter("profile");
			}
			else
			{
				GetPlugIn()->DisplayUserMessage(
					"vSMR", "Profile",
					"The profile could not be saved. vSMR reloaded the current file.",
					true, true, false, false, false);
			}
		}
		CloseRuntimeMenuPopup();
		return true;
	}
	if (ParseIndexedObjectId(id, "runtime.group.", index))
	{
		const std::vector<AvisoGroup> groups = GetAvisoGroups();
		if (index < groups.size())
		{
			ToggleAvisoGroupVisibility(groups[index].id);
			syncControlCenter();
		}
		RequestRefresh();
		return true;
	}
	if (ParseIndexedObjectId(id, "runtime.preset.", index))
	{
		const std::vector<AvisoPreset> presets = GetAvisoPresets();
		if (index < presets.size())
		{
			LoadAvisoPreset(presets[index].name);
			syncControlCenter("preset");
		}
		RequestRefresh();
		return true;
	}

	auto toggleAppWindow = [&](int appWindowId)
	{
		CancelInsetWindowInteractions();
		auto display = appWindowDisplays.find(appWindowId);
		if (display != appWindowDisplays.end())
		{
			display->second = !display->second;
			if (!display->second)
			{
				auto window = appWindows.find(appWindowId);
				if (window != appWindows.end() && window->second != nullptr)
					window->second->ResetAvisoInteractionState();
			}
			SaveInsetStateToAsrForAirport(getActiveAirport());
		}
		syncControlCenter();
		RequestRefresh();
	};
	auto resetInsetWindow = [&](int appWindowId)
	{
		ResetInsetWindowState(appWindowId, true);
		SaveInsetStateToAsrForAirport(getActiveAirport());
		syncControlCenter();
		RequestRefresh();
	};
	if (std::strcmp(id, "runtime.inset.aviso") == 0)
	{
		toggleAppWindow(APPWINDOW_AVISO - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.srw1") == 0)
	{
		toggleAppWindow(1);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.weather") == 0)
	{
		toggleAppWindow(APPWINDOW_WEATHER - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.timer") == 0)
	{
		toggleAppWindow(APPWINDOW_TIMER - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.aviso") == 0)
	{
		resetInsetWindow(APPWINDOW_AVISO - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.srw1") == 0)
	{
		resetInsetWindow(1);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.weather") == 0)
	{
		resetInsetWindow(APPWINDOW_WEATHER - APPWINDOW_BASE);
		return true;
	}
	if (std::strcmp(id, "runtime.inset.reset.timer") == 0)
	{
		resetInsetWindow(APPWINDOW_TIMER - APPWINDOW_BASE);
		return true;
	}

	const std::vector<AvisoPreset> presets = GetAvisoPresets();
	std::string activePreset = GetActiveAvisoPresetName();
	const auto activePresetIt = std::find_if(
		presets.begin(),
		presets.end(),
		[&](const AvisoPreset& preset) { return EqualsNoCase(preset.name, activePreset); });
	if (activePresetIt == presets.end())
		activePreset.clear();
	else
		activePreset = activePresetIt->name;
	const std::string defaultPreset = GetDefaultAvisoPresetName();
	if (std::strcmp(id, "runtime.preset.save") == 0)
	{
		const std::string name = MakeUniquePresetName(presets, "Inset Preset");
		SaveAvisoPreset(name, false, nullptr);
	}
	else if (std::strcmp(id, "runtime.preset.update") == 0)
		UpdateActiveAvisoPreset();
	else if (std::strcmp(id, "runtime.preset.rename") == 0 && !activePreset.empty())
		GetPlugIn()->OpenPopupEdit(area, RIMCAS_AVISO_PRESET_RENAME, activePreset.c_str());
	else if (std::strcmp(id, "runtime.preset.duplicate") == 0 && !activePreset.empty())
	{
		const std::string name = MakeUniquePresetName(presets, "Copy of " + activePreset);
		DuplicateAvisoPreset(activePreset, name, nullptr);
	}
	else if (std::strcmp(id, "runtime.preset.default") == 0)
	{
		if (!defaultPreset.empty() &&
			(activePreset.empty() || EqualsNoCase(activePreset, defaultPreset)))
		{
			ClearDefaultAvisoPreset();
		}
		else if (!activePreset.empty())
		{
			SetDefaultAvisoPreset(activePreset);
		}
		else
		{
			return true;
		}
	}
	else if (std::strcmp(id, "runtime.preset.reset") == 0 && !activePreset.empty())
		ResetActiveAvisoPreset();
	else if (std::strcmp(id, "runtime.preset.delete") == 0 && !activePreset.empty())
		DeleteAvisoPreset(activePreset);
	else if (std::strcmp(id, "runtime.preset.linked") == 0 && !activePreset.empty())
		SetActiveAvisoPresetLinkedMovement(!IsAvisoPresetLinkedMovementEnabled());
	else
		return true;

	syncControlCenter("preset");
	RequestRefresh();
	return true;
}

bool CSMRRadar::HandleRuntimeMenuMove(int objectType, const char* objectId, POINT point, RECT area, bool released)
{
	UNREFERENCED_PARAMETER(point);
	if (objectType != RUNTIME_MENU_RAIL ||
		objectId == nullptr ||
		std::strcmp(objectId, "runtime.drag") != 0)
	{
		return false;
	}

	// The draggable handle sits one pixel inside the framed rail.
	RuntimeMenuPosition.x = area.left - 1;
	RuntimeMenuPosition.y = area.top - 1;
	RuntimeMenuPositionInitialized = true;
	ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	RuntimeMenuPopupScrollOffset = 0;
	if (released)
		SaveRuntimeMenuPositionToAsr();
	RequestRefresh();
	return true;
}

void CSMRRadar::CloseRuntimeMenuPopup()
{
	if (ActiveRuntimeMenuPopup == RuntimeMenuPopup::None)
		return;
	ActiveRuntimeMenuPopup = RuntimeMenuPopup::None;
	RuntimeMenuPopupScrollOffset = 0;
	RequestRefresh();
}

void CSMRRadar::LoadRuntimeMenuPositionFromAsr()
{
	LONG x = 0;
	LONG y = 0;
	const char* xText = GetDataFromAsr("RuntimeMenuX");
	const bool hasX = TryParseAsrCoordinate(xText, x);
	const char* yText = GetDataFromAsr("RuntimeMenuY");
	const bool hasY = TryParseAsrCoordinate(yText, y);
	if (!hasX || !hasY)
		return;

	RuntimeMenuPosition = { x, y };
	RuntimeMenuPositionInitialized = true;
}

void CSMRRadar::SaveRuntimeMenuPositionToAsr()
{
	const std::string x = std::to_string(RuntimeMenuPosition.x);
	const std::string y = std::to_string(RuntimeMenuPosition.y);
	SaveDataToAsr("RuntimeMenuX", "vSMR runtime menu X position", x.c_str());
	SaveDataToAsr("RuntimeMenuY", "vSMR runtime menu Y position", y.c_str());
}
