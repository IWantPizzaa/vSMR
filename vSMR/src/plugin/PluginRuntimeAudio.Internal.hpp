#pragma once

#include <Windows.h>
#include <mmsystem.h>
#include <cwchar>
#include "platform/windows/ResourceIds.h"

namespace VsmrPluginRuntimeAudio
{
	enum class PlaybackSource { Failed, File, BuiltIn };

	inline WORD BuiltInSound(const wchar_t* fileName)
	{
		if (fileName != nullptr && _wcsicmp(fileName, L"Alarm.wav") == 0)
			return IDR_TIMER_ALARM_WAVE;
		if (fileName != nullptr && _wcsicmp(fileName, L"Ding.wav") == 0)
			return IDR_CPDLC_DING_WAVE;
		return 0;
	}

	// Injectable playback keeps regression tests silent and independent of audio
	// hardware. Keep custom files first, then use the DLL's own WAVE resource.
	template<typename Player>
	PlaybackSource PlayWithFallback(const wchar_t* path, HMODULE module, WORD resource, Player&& player)
	{
		constexpr DWORD flags = SND_ASYNC | SND_NODEFAULT;
		if (path != nullptr && *path != L'\0' && player(path, nullptr, SND_FILENAME | flags))
			return PlaybackSource::File;
		if (module != nullptr && resource != 0 &&
			player(MAKEINTRESOURCEW(resource), module, SND_RESOURCE | flags))
			return PlaybackSource::BuiltIn;
		return PlaybackSource::Failed;
	}
}
