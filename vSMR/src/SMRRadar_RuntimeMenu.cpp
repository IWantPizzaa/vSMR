#include "stdafx.h"
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
	constexpr int kRailHeight =
		kDragHeight +
		(kRailPadding * 2) +
		(kButtonSize * 5) +
		(kButtonGap * 4);
	constexpr int kPopupGap = 4;
	constexpr int kPopupHeaderHeight = 23;
	constexpr int kPopupRowHeight = 28;
	constexpr int kPopupPadding = 3;
	constexpr int kPopupPagerHeight = 21;
	constexpr int kInsetPopupWidth = 196;
	constexpr int kStandardPopupWidth = 170;

	const COLORREF kOuterBorder = RGB(5, 7, 8);
	const COLORREF kRailBackground = RGB(30, 40, 43);
	const COLORREF kTitleBackground = RGB(16, 20, 22);
	const COLORREF kButtonBackground = RGB(41, 57, 59);
	const COLORREF kButtonHover = RGB(53, 71, 75);
	const COLORREF kAccent = RGB(80, 150, 180);
	const COLORREF kText = RGB(208, 217, 220);
	const COLORREF kMutedText = RGB(143, 161, 166);
	const COLORREF kAccentText = RGB(244, 248, 249);
	const COLORREF kDivider = RGB(36, 50, 53);
	const COLORREF kDisabledBackground = RGB(31, 42, 45);
	const COLORREF kDisabledText = RGB(91, 107, 112);
	const COLORREF kDanger = RGB(112, 51, 55);

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
			point.x <= rect.right &&
			point.y >= rect.top &&
			point.y <= rect.bottom;
	}

	void FillRectColor(HDC hdc, const CRect& rect, COLORREF color)
	{
		::SetDCBrushColor(hdc, color);
		::FillRect(hdc, &rect, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
	}

	void DrawRectBorder(HDC hdc, const CRect& rect, COLORREF color)
	{
		::SetDCPenColor(hdc, color);
		::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
		::Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
		::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
	}

	void DrawRoundedRect(HDC hdc, const CRect& rect, COLORREF fill, COLORREF border, int radius)
	{
		::SetDCBrushColor(hdc, fill);
		::SetDCPenColor(hdc, border);
		::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
		::SelectObject(hdc, ::GetStockObject(DC_PEN));
		::RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
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

		const std::string base = requested.empty() ? "AVISO Preset" : requested;
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
	bounds.top += 22;
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
	RuntimeMenuPosition.x = std::clamp(RuntimeMenuPosition.x, minLeft, maxLeft);
	RuntimeMenuPosition.y = std::clamp(RuntimeMenuPosition.y, minTop, maxTop);
	RuntimeMenuArea = CRect(
		RuntimeMenuPosition.x,
		RuntimeMenuPosition.y,
		RuntimeMenuPosition.x + kRailWidth,
		RuntimeMenuPosition.y + kRailHeight);

	const int savedDc = ::SaveDC(hdc);
	::SelectObject(hdc, ::GetStockObject(DC_BRUSH));
	::SelectObject(hdc, ::GetStockObject(DC_PEN));
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	FillRectColor(hdc, RuntimeMenuArea, kRailBackground);
	DrawRectBorder(hdc, RuntimeMenuArea, kOuterBorder);

	CRect dragArea(
		RuntimeMenuArea.left + 1,
		RuntimeMenuArea.top + 1,
		RuntimeMenuArea.right - 1,
		RuntimeMenuArea.top + kDragHeight);
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
		::SetDCPenColor(hdc, RGB(46, 57, 60));
		for (int x = dragArea.left - dragArea.Height(); x < dragArea.right; x += 5)
		{
			::MoveToEx(hdc, x, dragArea.bottom, nullptr);
			::LineTo(hdc, x + dragArea.Height(), dragArea.top);
		}
		::RestoreDC(hdc, stripeDc);
	}
	AddScreenObject(RUNTIME_MENU_RAIL, "runtime.drag", dragArea, true, "Drag vSMR runtime menu");

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
		{ "runtime.button.insets", "insets", "AVISO Insets", RuntimeMenuPopup::Insets },
		{ "runtime.button.profile", "profile", "Profile", RuntimeMenuPopup::Profile },
		{ "runtime.button.control-center", "settings", "Open Control Center", RuntimeMenuPopup::None }
	};

	int buttonTop = RuntimeMenuArea.top + kDragHeight + kRailPadding;
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
		DrawRoundedRect(hdc, buttonArea, fill, kOuterBorder, 5);
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
			const int appWindowIds[] = { APPWINDOW_AVISO - APPWINDOW_BASE, 1, 2 };
			const int dotY = buttonArea.bottom - 6;
			for (int dot = 0; dot < 3; ++dot)
			{
				const auto display = appWindowDisplays.find(appWindowIds[dot]);
				const bool visible = display != appWindowDisplays.end() && display->second;
				const int dotX = buttonArea.left + 16 + (dot * 5);
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
	const int popupWidth = insetPopup ? kInsetPopupWidth : kStandardPopupWidth;
	int popupHeight = 0;
	int visibleRows = 0;
	bool showPager = false;
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
		const std::vector<AvisoPreset> presets = GetAvisoPresets();
		const int presetRows = (std::min)(4, static_cast<int>(presets.size()));
		const bool presetPager = presets.size() > 4;
		popupHeight =
			kPopupHeaderHeight +
			kPopupPadding +
			(3 * kPopupRowHeight) +
			19 +
			(presetRows > 0 ? presetRows * kPopupRowHeight : 32) +
			(presetPager ? kPopupPagerHeight : 0) +
			27 +
			(4 * 24) +
			(3 * 3) +
			kPopupPadding;
		popupHeight = (std::min)(popupHeight, bounds.Height() - 8);
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

	FillRectColor(hdc, RuntimeMenuPopupArea, kRailBackground);
	DrawRectBorder(hdc, RuntimeMenuPopupArea, kOuterBorder);
	AddScreenObject(RUNTIME_MENU_POPUP, "runtime.popup", RuntimeMenuPopupArea, false, title.c_str());

	CRect titleArea(
		RuntimeMenuPopupArea.left + 1,
		RuntimeMenuPopupArea.top + 1,
		RuntimeMenuPopupArea.right - 1,
		RuntimeMenuPopupArea.top + kPopupHeaderHeight);
	FillRectColor(hdc, titleArea, RGB(40, 54, 57));
	CRect titleDivider(titleArea.left, titleArea.bottom - 1, titleArea.right, titleArea.bottom);
	FillRectColor(hdc, titleDivider, RGB(17, 23, 25));

	HFONT headerFont = ::CreateFontA(
		-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT rowFont = ::CreateFontA(
		-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
	HFONT actionFont = ::CreateFontA(
		-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

	HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, headerFont));
	CRect titleText(titleArea.left + 7, titleArea.top, titleArea.right - 27, titleArea.bottom);
	DrawTextEllipsis(hdc, titleText, insetPopup ? "AVISO Insets" : title, RGB(217, 226, 228));
	CRect closeArea(titleArea.right - 21, titleArea.top + 3, titleArea.right - 4, titleArea.bottom - 3);
	DrawRoundedRect(hdc, closeArea, RGB(29, 38, 41), RGB(82, 96, 101), 2);
	::SelectObject(hdc, actionFont);
	DrawTextEllipsis(hdc, closeArea, "x", RGB(188, 200, 204), DT_CENTER);
	AddScreenObject(RUNTIME_MENU_POPUP, "runtime.close", closeArea, false, "Close");

	auto drawChoiceRow = [&](const RuntimePopupEntry& entry, const CRect& rowArea)
	{
		const bool hover = entry.enabled && PointInside(rowArea, mouseLocation);
		COLORREF fill = entry.enabled ? kButtonBackground : kDisabledBackground;
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
		AddScreenObject(RUNTIME_MENU_POPUP, entry.id.c_str(), rowArea, false, entry.label.c_str());
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

			if (showPager)
			{
				CRect previousArea(
					RuntimeMenuPopupArea.left + kPopupPadding,
					contentTop + 2,
					RuntimeMenuPopupArea.CenterPoint().x - 1,
					contentTop + kPopupPagerHeight - 1);
				CRect nextArea(
					RuntimeMenuPopupArea.CenterPoint().x + 1,
					contentTop + 2,
					RuntimeMenuPopupArea.right - kPopupPadding,
					contentTop + kPopupPagerHeight - 1);
				const bool canPrevious = RuntimeMenuPopupScrollOffset > 0;
				const bool canNext = RuntimeMenuPopupScrollOffset < maximumOffset;
				DrawRoundedRect(hdc, previousArea, canPrevious ? kButtonBackground : kDisabledBackground, kOuterBorder, 3);
				DrawRoundedRect(hdc, nextArea, canNext ? kButtonBackground : kDisabledBackground, kOuterBorder, 3);
				::SelectObject(hdc, actionFont);
				DrawTextEllipsis(hdc, previousArea, "Previous", canPrevious ? kText : kDisabledText, DT_CENTER);
				DrawTextEllipsis(hdc, nextArea, "Next", canNext ? kText : kDisabledText, DT_CENTER);
				if (canPrevious)
					AddScreenObject(RUNTIME_MENU_POPUP, "runtime.page.previous", previousArea, false, "Previous choices");
				if (canNext)
					AddScreenObject(RUNTIME_MENU_POPUP, "runtime.page.next", nextArea, false, "Next choices");
			}
		}
	}
	else
	{
		const int avisoWindowId = APPWINDOW_AVISO - APPWINDOW_BASE;
		const struct
		{
			const char* id;
			const char* label;
			int appWindowId;
		} insetRows[] = {
			{ "runtime.inset.aviso", "AVISO Inset", avisoWindowId },
			{ "runtime.inset.srw1", "SRW 1", 1 },
			{ "runtime.inset.srw2", "SRW 2", 2 }
		};
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
			drawChoiceRow(entry, rowArea);
			contentTop += kPopupRowHeight;
		}

		::SelectObject(hdc, actionFont);
		CRect sectionArea(
			RuntimeMenuPopupArea.left + 5,
			contentTop,
			RuntimeMenuPopupArea.right - 5,
			contentTop + 19);
		DrawTextEllipsis(hdc, sectionArea, "PRESET", RGB(159, 176, 181));
		contentTop += 19;

		const std::vector<AvisoPreset> presets = GetAvisoPresets();
		const std::string activePreset = GetActiveAvisoPresetName();
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
		}

		if (presets.size() > 4)
		{
			CRect previousArea(
				RuntimeMenuPopupArea.left + kPopupPadding,
				contentTop + 1,
				RuntimeMenuPopupArea.CenterPoint().x - 1,
				contentTop + kPopupPagerHeight - 2);
			CRect nextArea(
				RuntimeMenuPopupArea.CenterPoint().x + 1,
				contentTop + 1,
				RuntimeMenuPopupArea.right - kPopupPadding,
				contentTop + kPopupPagerHeight - 2);
			const bool canPrevious = RuntimeMenuPopupScrollOffset > 0;
			const bool canNext = RuntimeMenuPopupScrollOffset < maximumPresetOffset;
			DrawRoundedRect(hdc, previousArea, canPrevious ? kButtonBackground : kDisabledBackground, kOuterBorder, 3);
			DrawRoundedRect(hdc, nextArea, canNext ? kButtonBackground : kDisabledBackground, kOuterBorder, 3);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, previousArea, "Previous", canPrevious ? kText : kDisabledText, DT_CENTER);
			DrawTextEllipsis(hdc, nextArea, "Next", canNext ? kText : kDisabledText, DT_CENTER);
			if (canPrevious)
				AddScreenObject(RUNTIME_MENU_POPUP, "runtime.preset.page.previous", previousArea, false, "Previous presets");
			if (canNext)
				AddScreenObject(RUNTIME_MENU_POPUP, "runtime.preset.page.next", nextArea, false, "Next presets");
			contentTop += kPopupPagerHeight;
		}

		const bool hasActivePreset = !activePreset.empty();
		CRect linkedArea(
			RuntimeMenuPopupArea.left + kPopupPadding,
			contentTop + 2,
			RuntimeMenuPopupArea.right - kPopupPadding,
			contentTop + 25);
		DrawRoundedRect(
			hdc,
			linkedArea,
			hasActivePreset && PointInside(linkedArea, mouseLocation) ? kButtonHover : kButtonBackground,
			RGB(17, 23, 25),
			4);
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
			AddScreenObject(RUNTIME_MENU_POPUP, "runtime.preset.linked", linkedArea, false, "Toggle linked movement");
		contentTop += 27;

		const struct
		{
			const char* id;
			const char* label;
			bool requiresActive;
			bool danger;
		} actions[] = {
			{ "runtime.preset.save", "Save current", false, false },
			{ "runtime.preset.update", "Update", true, false },
			{ "runtime.preset.rename", "Rename", true, false },
			{ "runtime.preset.duplicate", "Duplicate", true, false },
			{ "runtime.preset.default", "Set default", true, false },
			{ "runtime.preset.reset", "Reset", true, false },
			{ "runtime.preset.delete", "Delete", true, true }
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
				contentTop + (row * (24 + actionGap)),
				RuntimeMenuPopupArea.left + kPopupPadding + (column * (actionWidth + actionGap)) + actionWidth,
				contentTop + (row * (24 + actionGap)) + 24);
			const bool enabled = !actions[index].requiresActive || hasActivePreset;
			COLORREF fill =
				!enabled ? kDisabledBackground :
				(actions[index].danger ? kDanger :
					(PointInside(actionArea, mouseLocation) ? kButtonHover : kButtonBackground));
			DrawRoundedRect(hdc, actionArea, fill, kOuterBorder, 3);
			::SelectObject(hdc, actionFont);
			DrawTextEllipsis(hdc, actionArea, actions[index].label, enabled ? kText : kDisabledText, DT_CENTER);
			if (enabled)
				AddScreenObject(RUNTIME_MENU_POPUP, actions[index].id, actionArea, false, actions[index].label);
		}
	}

	::SelectObject(hdc, oldFont);
	if (headerFont != nullptr)
		::DeleteObject(headerFont);
	if (rowFont != nullptr)
		::DeleteObject(rowFont);
	if (actionFont != nullptr)
		::DeleteObject(actionFont);
	::RestoreDC(hdc, savedDc);
}

bool CSMRRadar::HandleRuntimeMenuClick(int objectType, const char* objectId, POINT point, RECT area, int button)
{
	UNREFERENCED_PARAMETER(point);
	if (objectType != RUNTIME_MENU_RAIL && objectType != RUNTIME_MENU_POPUP)
		return false;

	auto syncControlCenter = [&]()
	{
		if (VsmrControlCenterDialog != nullptr)
			VsmrControlCenterDialog->SyncFromRadar();
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

		if (std::strcmp(id, "runtime.button.mode") == 0)
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
			SetProfileDisplayModeActiveForEditor(activeProfile, modes[index].name);
			syncControlCenter();
		}
		CloseRuntimeMenuPopup();
		return true;
	}
	if (ParseIndexedObjectId(id, "runtime.profile.", index))
	{
		const std::vector<std::string> profiles = GetOrderedProfileNamesForUi();
		if (index < profiles.size())
		{
			SetActiveProfileForEditor(profiles[index], false);
			syncControlCenter();
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
			syncControlCenter();
		}
		RequestRefresh();
		return true;
	}

	auto toggleAppWindow = [&](int appWindowId)
	{
		auto display = appWindowDisplays.find(appWindowId);
		if (display != appWindowDisplays.end())
		{
			display->second = !display->second;
			SaveDataToAsr(
				appWindowId == 1 ? "SRW1Display" :
				appWindowId == 2 ? "SRW2Display" : "AVISO1Display",
				appWindowId == 3 ? "Display AVISO viewport" : "Display Secondary Radar Window",
				display->second ? "1" : "0");
		}
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
	if (std::strcmp(id, "runtime.inset.srw2") == 0)
	{
		toggleAppWindow(2);
		return true;
	}

	const std::vector<AvisoPreset> presets = GetAvisoPresets();
	const std::string activePreset = GetActiveAvisoPresetName();
	if (std::strcmp(id, "runtime.preset.save") == 0)
	{
		const std::string name = MakeUniquePresetName(presets, "AVISO Preset");
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
	else if (std::strcmp(id, "runtime.preset.default") == 0 && !activePreset.empty())
		SetDefaultAvisoPreset(activePreset);
	else if (std::strcmp(id, "runtime.preset.reset") == 0 && !activePreset.empty())
		ResetActiveAvisoPreset();
	else if (std::strcmp(id, "runtime.preset.delete") == 0 && !activePreset.empty())
		DeleteAvisoPreset(activePreset);
	else if (std::strcmp(id, "runtime.preset.linked") == 0 && !activePreset.empty())
		SetActiveAvisoPresetLinkedMovement(!IsAvisoPresetLinkedMovementEnabled());
	else
		return true;

	syncControlCenter();
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
