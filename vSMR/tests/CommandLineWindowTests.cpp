#include "platform/windows/PrecompiledHeader.hpp"

// Provider tests mock command delivery. Compile the real Win32 adapter in a
// separate namespace as well, to exercise its subclass and actual message pump.
#define VsmrEuroScopeCommandLine VsmrCommandLineWindowTests
#include "platform/windows/EuroScopeCommandLine.hpp"
#undef VsmrEuroScopeCommandLine

#include <string>
#include <vector>

namespace
{
	namespace Adapter = VsmrCommandLineWindowTests;
	int Completed = 0;
	int EnterCount = 0;
	int Remaining = 0;
	bool FollowupFailed = false;

	bool Progress()
	{
		const auto state = Adapter::Poll(Adapter::Owner::Vsid);
		if (state != Adapter::SubmissionStatus::Confirmed)
			return false;
		++Completed;
		if (--Remaining > 0)
			FollowupFailed = !Adapter::Begin(Adapter::Owner::Vsid, ".test next", nullptr, Progress);
		return true;
	}

	LRESULT CALLBACK HostEdit(HWND window, UINT message, WPARAM key, LPARAM param,
		UINT_PTR, DWORD_PTR)
	{
		if (message == WM_KEYDOWN && key == VK_RETURN)
		{
			++EnterCount;
			::SetWindowTextA(window, "");
			return 0;
		}
		return ::DefSubclassProc(window, message, key, param);
	}

	void PumpFor(DWORD milliseconds)
	{
		const ULONGLONG end = ::GetTickCount64() + milliseconds;
		do
		{
			MSG message{};
			while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
			{
				::TranslateMessage(&message);
				::DispatchMessage(&message);
			}
			::MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
		} while (::GetTickCount64() < end);
	}
}

void RunCommandLineWindowTests(std::vector<std::string>& failures)
{
	const auto check = [&](bool condition, const char* text) {
		if (!condition) failures.emplace_back(text);
	};
	// Off-screen but visible: discovery is confined to this test process, never
	// to the user's running EuroScope window. No focus or keyboard input injection.
	HWND host = ::CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "STATIC",
		"vSMR command adapter test", WS_POPUP, -20000, -20000, 800, 600,
		nullptr, nullptr, ::GetModuleHandle(nullptr), nullptr);
	HWND edit = ::CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
		10, 560, 700, 24, host, nullptr, ::GetModuleHandle(nullptr), nullptr);
	check(host && edit, "create isolated command-line test window");
	if (!host || !edit) { if (host) ::DestroyWindow(host); return; }
	::SetWindowSubclass(edit, HostEdit, 1, 0);
	::ShowWindow(host, SW_SHOWNOACTIVATE);
	for (int click = 0; click < 3; ++click)
	{
		Completed = 0;
		EnterCount = 0;
		Remaining = click == 1 ? 3 : 1;
		const int expected = Remaining;
		FollowupFailed = false;
		check(Adapter::Begin(Adapter::Owner::Vsid, ".test first", nullptr, Progress),
			"real command adapter accepts repeated button clicks");
		PumpFor(600);
		check(Completed == expected && EnterCount == expected && !FollowupFailed,
			"native progress timer completes every command in order without duplicates");
		check(!Adapter::IsBusy(), "real command adapter unlocks after completion");
	}
	check(Adapter::Begin(Adapter::Owner::Vsid, ".test cancel", nullptr, Progress), "start cancellation test");
	Adapter::Cancel(Adapter::Owner::Vsid);
	const int completedBeforeCancel = Completed;
	PumpFor(100);
	check(!Adapter::IsBusy() && Completed == completedBeforeCancel, "cancellation removes the native progress timer");
	::DestroyWindow(host);
}
