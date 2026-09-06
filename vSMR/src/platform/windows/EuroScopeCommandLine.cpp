#include "platform/windows/PrecompiledHeader.hpp"
#include "platform/windows/EuroScopeCommandLine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <vector>

namespace
{
	using VsmrEuroScopeCommandLine::Owner;
	using VsmrEuroScopeCommandLine::SubmissionStatus;

	struct PendingSubmission
	{
		Owner owner = Owner::CdmReminder;
		HWND editControl = nullptr;
		std::string command;
		std::chrono::steady_clock::time_point startedAt;
	};

	PendingSubmission ActiveSubmission;
	bool SubmissionActive = false;
	constexpr UINT_PTR QuietVsidEnterSubclassId = 0x56534944U;

	LRESULT CALLBACK QuietVsidEnterSubclass(
		HWND window,
		UINT message,
		WPARAM wParam,
		LPARAM lParam,
		UINT_PTR subclassId,
		DWORD_PTR referenceData)
	{
		(void)referenceData;
		if (message == WM_CHAR && wParam == VK_RETURN &&
			SubmissionActive && ActiveSubmission.owner == Owner::Vsid &&
			ActiveSubmission.editControl == window)
		{
			// EuroScope consumes the posted key-down. Do not pass the translated
			// Enter character to its single-line edit, which emits a system beep.
			return 0;
		}
		if (message == WM_NCDESTROY)
			::RemoveWindowSubclass(window, QuietVsidEnterSubclass, subclassId);
		return ::DefSubclassProc(window, message, wParam, lParam);
	}

	bool IsLikelyCommandEditControl(HWND window)
	{
		if (window == nullptr || !::IsWindow(window) ||
			!::IsWindowVisible(window) || !::IsWindowEnabled(window))
		{
			return false;
		}

		char className[64] = {};
		if (::GetClassNameA(window, className, static_cast<int>(sizeof(className))) <= 0)
			return false;
		std::string classUpper(className);
		std::transform(classUpper.begin(), classUpper.end(), classUpper.begin(),
			[](unsigned char character) { return static_cast<char>(std::toupper(character)); });
		if (classUpper != "EDIT" && classUpper.find("RICHEDIT") == std::string::npos)
			return false;

		const LONG style = ::GetWindowLong(window, GWL_STYLE);
		if ((style & ES_READONLY) != 0 || (style & ES_MULTILINE) != 0)
			return false;

		RECT rect = {};
		return ::GetWindowRect(window, &rect) &&
			rect.right - rect.left >= 120 && rect.bottom - rect.top >= 12;
	}

	struct MainWindowSearch
	{
		DWORD processId = 0;
		HWND bestWindow = nullptr;
		LONG bestArea = 0;
	};

	BOOL CALLBACK FindMainWindow(HWND window, LPARAM parameter)
	{
		MainWindowSearch* search = reinterpret_cast<MainWindowSearch*>(parameter);
		if (search == nullptr)
			return TRUE;
		DWORD processId = 0;
		::GetWindowThreadProcessId(window, &processId);
		if (processId != search->processId || !::IsWindowVisible(window) ||
			::GetWindow(window, GW_OWNER) != nullptr)
		{
			return TRUE;
		}

		RECT rect = {};
		if (!::GetWindowRect(window, &rect))
			return TRUE;
		const LONG width = (std::max)(0L, rect.right - rect.left);
		const LONG height = (std::max)(0L, rect.bottom - rect.top);
		const LONG area = width * height;
		if (area > search->bestArea)
		{
			search->bestArea = area;
			search->bestWindow = window;
		}
		return TRUE;
	}

	struct EditControlSearch
	{
		RECT mainRect = {};
		HWND bestEdit = nullptr;
		LONG bestScore = LONG_MIN;
	};

	BOOL CALLBACK FindCommandEdit(HWND window, LPARAM parameter)
	{
		EditControlSearch* search = reinterpret_cast<EditControlSearch*>(parameter);
		if (search == nullptr || !IsLikelyCommandEditControl(window))
			return TRUE;

		RECT rect = {};
		if (!::GetWindowRect(window, &rect) ||
			rect.left < search->mainRect.left || rect.right > search->mainRect.right ||
			rect.bottom < search->mainRect.bottom - 120 || rect.bottom > search->mainRect.bottom)
		{
			return TRUE;
		}

		const LONG style = ::GetWindowLong(window, GWL_STYLE);
		LONG score = rect.top + ((rect.right - rect.left) / 4);
		if ((style & WS_TABSTOP) != 0) score += 1000;
		if ((style & ES_AUTOHSCROLL) != 0) score += 500;
		if (rect.bottom >= search->mainRect.bottom - 80) score += 2000;
		if (score > search->bestScore)
		{
			search->bestScore = score;
			search->bestEdit = window;
		}
		return TRUE;
	}

	HWND ResolveCommandEdit()
	{
		MainWindowSearch mainSearch;
		mainSearch.processId = ::GetCurrentProcessId();
		::EnumWindows(FindMainWindow, reinterpret_cast<LPARAM>(&mainSearch));
		if (mainSearch.bestWindow == nullptr)
			return nullptr;

		EditControlSearch editSearch;
		if (!::GetWindowRect(mainSearch.bestWindow, &editSearch.mainRect))
			return nullptr;
		::EnumChildWindows(mainSearch.bestWindow, FindCommandEdit,
			reinterpret_cast<LPARAM>(&editSearch));
		return editSearch.bestEdit;
	}

	bool ReadWindowText(HWND window, std::string& outText)
	{
		outText.clear();
		if (window == nullptr || !::IsWindow(window))
			return false;

		constexpr int MaximumCommandCharacters = 64 * 1024;
		const int length = ::GetWindowTextLengthA(window);
		if (length < 0 || length > MaximumCommandCharacters)
			return false;
		std::vector<char> buffer(static_cast<std::size_t>(length) + 1U, '\0');
		if (length > 0 && ::GetWindowTextA(
			window, buffer.data(), static_cast<int>(buffer.size())) != length)
		{
			return false;
		}
		outText.assign(buffer.data(), static_cast<std::size_t>(length));
		return true;
	}

	void ClearActiveSubmission(bool removeInsertedText) noexcept
	{
		if (SubmissionActive && ActiveSubmission.owner == Owner::Vsid &&
			ActiveSubmission.editControl != nullptr &&
			::IsWindow(ActiveSubmission.editControl))
		{
			::RemoveWindowSubclass(
				ActiveSubmission.editControl,
				QuietVsidEnterSubclass,
				QuietVsidEnterSubclassId);
		}
		if (removeInsertedText && SubmissionActive &&
			ActiveSubmission.editControl != nullptr && ::IsWindow(ActiveSubmission.editControl))
		{
			std::string currentText;
			if (ReadWindowText(ActiveSubmission.editControl, currentText) &&
				currentText == ActiveSubmission.command)
			{
				::SetWindowTextA(ActiveSubmission.editControl, "");
			}
		}
		ActiveSubmission = {};
		SubmissionActive = false;
	}
}

bool VsmrEuroScopeCommandLine::Begin(
	Owner owner,
	const std::string& command,
	std::string* outError)
{
	auto fail = [&](const char* message)
	{
		if (outError != nullptr)
			*outError = message;
		return false;
	};
	if (outError != nullptr)
		outError->clear();
	if (SubmissionActive)
		return fail("EuroScope is still processing another vSMR command.");
	if (command.empty() || command.size() > 4096U || command.front() != '.' ||
		command.find_first_of("\r\n") != std::string::npos ||
		std::find(command.begin(), command.end(), '\0') != command.end())
	{
		return fail("The generated EuroScope command is invalid.");
	}

	HWND editControl = ResolveCommandEdit();
	std::string existingText;
	if (editControl == nullptr || !ReadWindowText(editControl, existingText))
		return fail("EuroScope's command line is unavailable.");
	if (!existingText.empty())
		return fail("Clear EuroScope's command line before using this button.");
	if (!::SetWindowTextA(editControl, command.c_str()))
		return fail("EuroScope's command line could not be updated.");
	if (owner == Owner::Vsid && !::SetWindowSubclass(
		editControl,
		QuietVsidEnterSubclass,
		QuietVsidEnterSubclassId,
		0U))
	{
		::SetWindowTextA(editControl, "");
		return fail("EuroScope's command line could not be prepared.");
	}

	ActiveSubmission.owner = owner;
	ActiveSubmission.editControl = editControl;
	ActiveSubmission.command = command;
	ActiveSubmission.startedAt = std::chrono::steady_clock::now();
	SubmissionActive = true;

	const bool keyDownPosted =
		::PostMessageA(editControl, WM_KEYDOWN, VK_RETURN, 0) != FALSE;
	const bool keyUpPosted =
		::PostMessageA(editControl, WM_KEYUP, VK_RETURN, 0) != FALSE;
	if (!keyDownPosted)
	{
		ClearActiveSubmission(true);
		return fail("EuroScope did not accept the command.");
	}
	(void)keyUpPosted;
	return true;
}

VsmrEuroScopeCommandLine::SubmissionStatus
VsmrEuroScopeCommandLine::Poll(Owner owner)
{
	if (!SubmissionActive || ActiveSubmission.owner != owner)
		return SubmissionStatus::Idle;

	std::string currentText;
	if (!ReadWindowText(ActiveSubmission.editControl, currentText))
	{
		ClearActiveSubmission(false);
		return SubmissionStatus::Ambiguous;
	}
	if (currentText != ActiveSubmission.command)
	{
		ClearActiveSubmission(false);
		return SubmissionStatus::Confirmed;
	}
	if (std::chrono::steady_clock::now() - ActiveSubmission.startedAt <
		std::chrono::seconds(4))
	{
		return SubmissionStatus::Pending;
	}

	ClearActiveSubmission(true);
	return SubmissionStatus::Ambiguous;
}

bool VsmrEuroScopeCommandLine::HasPending(Owner owner) noexcept
{
	return SubmissionActive && ActiveSubmission.owner == owner;
}

bool VsmrEuroScopeCommandLine::IsBusy() noexcept
{
	return SubmissionActive;
}

void VsmrEuroScopeCommandLine::Cancel(Owner owner) noexcept
{
	if (SubmissionActive && ActiveSubmission.owner == owner)
		ClearActiveSubmission(true);
}
