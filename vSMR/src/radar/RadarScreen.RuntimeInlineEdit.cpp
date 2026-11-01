#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace
{
	std::wstring FromUtf8(const std::string& value)
	{
		if (value.empty()) return {};
		const int size = ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
			static_cast<int>(value.size()), nullptr, 0);
		if (size <= 0) return {};
		std::wstring result(static_cast<size_t>(size), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, value.data(),
			static_cast<int>(value.size()), result.data(), size);
		return result;
	}

	std::string ToUtf8(const std::wstring& value)
	{
		if (value.empty()) return {};
		const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
			static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0) return {};
		std::string result(static_cast<size_t>(size), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, value.data(),
			static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
		return result;
	}

	bool IsAsciiAlphanumeric(wchar_t character)
	{
		return (character >= L'A' && character <= L'Z') ||
			(character >= L'a' && character <= L'z') ||
			(character >= L'0' && character <= L'9');
	}

	bool IsLowSurrogate(wchar_t character)
	{
		return character >= 0xDC00 && character <= 0xDFFF;
	}

	bool IsHighSurrogate(wchar_t character)
	{
		return character >= 0xD800 && character <= 0xDBFF;
	}
}

void CSMRRadar::StartRuntimeInlineEdit(RuntimeInlineEditKind kind, RECT area,
	const std::string& value)
{
	RuntimeInlineEditType = kind;
	RuntimeInlineEditArea = area;
	RuntimeInlineEditText = FromUtf8(value);
	RuntimeInlineEditAnchor = 0;
	RuntimeInlineEditCursor = RuntimeInlineEditText.size();
	RuntimeInlineEditCaretTick = ::GetTickCount64();
	RequestRefresh();
	const HWND host = AvisoRefreshHostWindow.load(std::memory_order_acquire);
	if (host != nullptr && ::IsWindow(host))
		RepaintRuntimeMenuAfterHost(host);
}

void CSMRRadar::FinishRuntimeInlineEdit(bool accept)
{
	if (RuntimeInlineEditType == RuntimeInlineEditKind::None)
		return;
	const RuntimeInlineEditKind kind = RuntimeInlineEditType;
	std::wstring value = RuntimeInlineEditText;
	if (accept)
	{
		const size_t first = value.find_first_not_of(L" \t\r\n");
		const size_t last = value.find_last_not_of(L" \t\r\n");
		value = first == std::wstring::npos ? L"" : value.substr(first, last - first + 1);
		if (kind != RuntimeInlineEditKind::Preset)
			for (wchar_t& character : value)
				if (character >= L'a' && character <= L'z')
					character = static_cast<wchar_t>(character - L'a' + L'A');
		const bool airportValid = value.size() == 4 &&
			std::all_of(value.begin(), value.end(), IsAsciiAlphanumeric);
		const bool pdcValid = !value.empty() && value.size() <= 16 &&
			std::all_of(value.begin(), value.end(), [](wchar_t character)
				{ return IsAsciiAlphanumeric(character) || character == L'_' || character == L'-'; });
		if ((kind == RuntimeInlineEditKind::Airport && !airportValid) ||
			(kind == RuntimeInlineEditKind::Preset && value.empty()) ||
			(kind == RuntimeInlineEditKind::PdcLogon && !pdcValid))
		{
			::MessageBeep(MB_ICONWARNING);
			RuntimeInlineEditCaretTick = ::GetTickCount64();
			return;
		}
	}
	RuntimeInlineEditType = RuntimeInlineEditKind::None;
	RuntimeInlineEditText.clear();
	RuntimeInlineEditCursor = 0;
	RuntimeInlineEditAnchor = 0;
	RuntimeInlineEditArea = RECT{};
	if (accept)
	{
		const std::string utf8 = ToUtf8(value);
		const int function = kind == RuntimeInlineEditKind::Airport ? RIMCAS_ACTIVE_AIRPORT_FUNC :
			kind == RuntimeInlineEditKind::Preset ? RIMCAS_AVISO_PRESET_RENAME :
			RUNTIME_DATALINK_CALLSIGN_EDIT;
		OnFunctionCall(function, utf8.c_str(), POINT{}, RECT{});
	}
	RequestRefresh();
	const HWND host = AvisoRefreshHostWindow.load(std::memory_order_acquire);
	if (host != nullptr && ::IsWindow(host))
		RepaintRuntimeMenuAfterHost(host);
}

bool CSMRRadar::HandleRuntimeInlineEditKey(WPARAM key)
{
	if (RuntimeInlineEditType == RuntimeInlineEditKind::None)
		return false;
	const bool shift = ((::GetKeyState(VK_SHIFT) | ::GetAsyncKeyState(VK_SHIFT)) & 0x8000) != 0;
	const bool control = ((::GetKeyState(VK_CONTROL) | ::GetAsyncKeyState(VK_CONTROL)) & 0x8000) != 0;
	const size_t maximum = RuntimeInlineEditType == RuntimeInlineEditKind::Airport ? 4 :
		RuntimeInlineEditType == RuntimeInlineEditKind::PdcLogon ? 16 : 128;
	auto selection = [&]()
	{
		return std::pair<size_t, size_t>{
			(std::min)(RuntimeInlineEditAnchor, RuntimeInlineEditCursor),
			(std::max)(RuntimeInlineEditAnchor, RuntimeInlineEditCursor) };
	};
	auto replace = [&](std::wstring insertion)
	{
		insertion.erase(std::remove_if(insertion.begin(), insertion.end(),
			[](wchar_t character) { return character == L'\r' || character == L'\n' || character == L'\0'; }),
			insertion.end());
		if (RuntimeInlineEditType == RuntimeInlineEditKind::Airport ||
			RuntimeInlineEditType == RuntimeInlineEditKind::PdcLogon)
		{
			for (wchar_t& character : insertion)
				if (character >= L'a' && character <= L'z')
					character = static_cast<wchar_t>(character - L'a' + L'A');
			insertion.erase(std::remove_if(insertion.begin(), insertion.end(),
				[&](wchar_t character)
				{
					return !IsAsciiAlphanumeric(character) &&
						(RuntimeInlineEditType == RuntimeInlineEditKind::Airport ||
							(character != L'_' && character != L'-'));
				}), insertion.end());
		}
		const auto [begin, end] = selection();
		const size_t retained = RuntimeInlineEditText.size() - (end - begin);
		const size_t available = retained >= maximum ? 0 : maximum - retained;
		if (insertion.size() > available)
			insertion.resize(available);
		if (!insertion.empty() && IsHighSurrogate(insertion.back()))
			insertion.pop_back();
		RuntimeInlineEditText.replace(begin, end - begin, insertion);
		RuntimeInlineEditCursor = begin + insertion.size();
		RuntimeInlineEditAnchor = RuntimeInlineEditCursor;
	};
	auto moveCursor = [&](size_t position)
	{
		RuntimeInlineEditCursor = position;
		if (!shift) RuntimeInlineEditAnchor = position;
	};
	if (key == VK_RETURN || key == VK_TAB)
	{
		FinishRuntimeInlineEdit(true);
		return true;
	}
	if (key == VK_ESCAPE)
	{
		FinishRuntimeInlineEdit(false);
		return true;
	}
	if (control && (key == 'A' || key == 'a'))
	{
		RuntimeInlineEditAnchor = 0;
		RuntimeInlineEditCursor = RuntimeInlineEditText.size();
	}
	else if (control && (key == 'V' || key == 'v'))
	{
		const HWND host = AvisoRefreshHostWindow.load(std::memory_order_acquire);
		if (::OpenClipboard(host))
		{
			HANDLE clipboard = ::GetClipboardData(CF_UNICODETEXT);
			if (clipboard != nullptr)
			{
				const wchar_t* text = static_cast<const wchar_t*>(::GlobalLock(clipboard));
				if (text != nullptr)
				{
					const size_t capacity = (std::min)(
						static_cast<size_t>(::GlobalSize(clipboard) / sizeof(wchar_t)), size_t{ 256 });
					size_t length = 0;
					while (length < capacity && text[length] != L'\0') ++length;
					replace(std::wstring(text, length));
					::GlobalUnlock(clipboard);
				}
			}
			::CloseClipboard();
		}
	}
	else if (key == VK_BACK || key == VK_DELETE)
	{
		if (RuntimeInlineEditCursor != RuntimeInlineEditAnchor)
			replace(L"");
		else if (key == VK_BACK && RuntimeInlineEditCursor > 0)
		{
			const size_t end = RuntimeInlineEditCursor;
			size_t begin = end - 1;
			if (begin > 0 && IsLowSurrogate(RuntimeInlineEditText[begin]) &&
				IsHighSurrogate(RuntimeInlineEditText[begin - 1])) --begin;
			RuntimeInlineEditText.erase(begin, end - begin);
			RuntimeInlineEditCursor = RuntimeInlineEditAnchor = begin;
		}
		else if (key == VK_DELETE && RuntimeInlineEditCursor < RuntimeInlineEditText.size())
		{
			size_t end = RuntimeInlineEditCursor + 1;
			if (end < RuntimeInlineEditText.size() &&
				IsHighSurrogate(RuntimeInlineEditText[RuntimeInlineEditCursor]) &&
				IsLowSurrogate(RuntimeInlineEditText[end])) ++end;
			RuntimeInlineEditText.erase(RuntimeInlineEditCursor, end - RuntimeInlineEditCursor);
		}
	}
	else if (key == VK_LEFT || key == VK_RIGHT)
	{
		const auto [begin, end] = selection();
		if (!shift && begin != end)
			moveCursor(key == VK_LEFT ? begin : end);
		else if (key == VK_LEFT && RuntimeInlineEditCursor > 0)
		{
			size_t next = RuntimeInlineEditCursor - 1;
			if (next > 0 && IsLowSurrogate(RuntimeInlineEditText[next]) &&
				IsHighSurrogate(RuntimeInlineEditText[next - 1])) --next;
			moveCursor(next);
		}
		else if (key == VK_RIGHT && RuntimeInlineEditCursor < RuntimeInlineEditText.size())
		{
			size_t next = RuntimeInlineEditCursor + 1;
			if (next < RuntimeInlineEditText.size() &&
				IsHighSurrogate(RuntimeInlineEditText[RuntimeInlineEditCursor]) &&
				IsLowSurrogate(RuntimeInlineEditText[next])) ++next;
			moveCursor(next);
		}
	}
	else if (key == VK_HOME)
		moveCursor(0);
	else if (key == VK_END)
		moveCursor(RuntimeInlineEditText.size());
	else if (!control)
	{
		BYTE keyboardState[256]{};
		if (::GetKeyboardState(keyboardState))
		{
			keyboardState[key & 0xFF] |= 0x80;
			if (shift) keyboardState[VK_SHIFT] |= 0x80;
			wchar_t characters[8]{};
			const UINT scanCode = ::MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
			const int count = ::ToUnicodeEx(static_cast<UINT>(key), scanCode,
				keyboardState, characters, static_cast<int>(_countof(characters)), 0,
				::GetKeyboardLayout(0));
			if (count > 0)
				replace(std::wstring(characters, characters + (std::min)(count, static_cast<int>(_countof(characters)))));
		}
	}
	RuntimeInlineEditCaretTick = ::GetTickCount64();
	RequestRefresh();
	const HWND host = AvisoRefreshHostWindow.load(std::memory_order_acquire);
	if (host != nullptr && ::IsWindow(host))
		RepaintRuntimeMenuAfterHost(host);
	return true;
}

void CSMRRadar::PlaceRuntimeInlineEditCaret(int logicalX, bool selectAll)
{
	if (RuntimeInlineEditType == RuntimeInlineEditKind::None)
		return;
	if (selectAll)
	{
		RuntimeInlineEditAnchor = 0;
		RuntimeInlineEditCursor = RuntimeInlineEditText.size();
	}
	else
	{
		const HWND host = AvisoRefreshHostWindow.load(std::memory_order_acquire);
		HDC dc = host != nullptr && ::IsWindow(host) ? ::GetDC(host) : nullptr;
		if (dc == nullptr) return;
		const HFONT font = static_cast<HFONT>((RuntimeInlineEditType == RuntimeInlineEditKind::Airport
			? RuntimeOverlayFont : RuntimeMenuActionFont).GetSafeHandle());
		const HGDIOBJ previous = font != nullptr ? ::SelectObject(dc, font) : nullptr;
		auto width = [&](size_t count)
		{
			if (count == 0) return 0;
			SIZE extent{};
			::GetTextExtentPoint32W(dc, RuntimeInlineEditText.data(),
				static_cast<int>(count), &extent);
			return static_cast<int>(extent.cx);
		};
		const int innerLeft = RuntimeInlineEditArea.left + 4;
		const int innerRight = RuntimeInlineEditArea.right - 4;
		const int fullWidth = width(RuntimeInlineEditText.size());
		int left = innerLeft + 2;
		if (RuntimeInlineEditType == RuntimeInlineEditKind::Airport &&
			fullWidth < innerRight - innerLeft)
			left = innerLeft + ((innerRight - innerLeft - fullWidth) / 2);
		else if (left + width(RuntimeInlineEditCursor) > innerRight - 2)
			left = innerRight - 2 - width(RuntimeInlineEditCursor);
		size_t closest = 0;
		int distance = (std::abs)(logicalX - left);
		for (size_t index = 1; index <= RuntimeInlineEditText.size(); ++index)
		{
			if (index < RuntimeInlineEditText.size() &&
				IsHighSurrogate(RuntimeInlineEditText[index - 1]) &&
				IsLowSurrogate(RuntimeInlineEditText[index]))
				continue;
			const int nextDistance = (std::abs)(logicalX - left - width(index));
			if (nextDistance < distance)
			{
				closest = index;
				distance = nextDistance;
			}
		}
		RuntimeInlineEditCursor = RuntimeInlineEditAnchor = closest;
		if (previous != nullptr) ::SelectObject(dc, previous);
		::ReleaseDC(host, dc);
	}
	RuntimeInlineEditCaretTick = ::GetTickCount64();
	RequestRefresh();
	const HWND host = AvisoRefreshHostWindow.load(std::memory_order_acquire);
	if (host != nullptr && ::IsWindow(host))
		RepaintRuntimeMenuAfterHost(host);
}
