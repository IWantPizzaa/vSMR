#pragma once

namespace VsmrPluginRuntimeAudio
{
	bool Play(const wchar_t* fileName, const char* description);
	// Stop asynchronous resource playback before the runtime DLL is unloaded.
	void Stop();
}
