#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/PluginRuntimeAudio.hpp"
#include "plugin/PluginRuntimeAudio.Internal.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "shared/logging/Logger.hpp"

#include <filesystem>
#include <mutex>
#include <string>

#include "Mmsystem.h"

namespace
{
	std::mutex AudioMutex;
	bool AudioStarted = false;

	std::filesystem::path ResolveRuntimeAudioPath(const wchar_t* fileName)
	{
		if (VsmrRuntimeContext::IsConfigured())
			return VsmrRuntimeContext::DataRoot() / L"Audio" / fileName;

		std::wstring modulePathBuffer(32768, L'\0');
		const DWORD modulePathLength = ::GetModuleFileNameW(
			HINSTANCE(&__ImageBase),
			modulePathBuffer.data(),
			static_cast<DWORD>(modulePathBuffer.size()));

		std::filesystem::path pluginDirectory;
		if (modulePathLength > 0 && modulePathLength < modulePathBuffer.size())
		{
			modulePathBuffer.resize(modulePathLength);
			pluginDirectory = std::filesystem::path(modulePathBuffer).parent_path();
		}
		else if (!Logger::DLL_PATH.empty())
		{
			pluginDirectory = std::filesystem::u8path(Logger::DLL_PATH);
		}

		if (pluginDirectory.empty())
			return {};

		return pluginDirectory / L"vSMR_Data" / L"Audio" / fileName;
	}
}

bool VsmrPluginRuntimeAudio::Play(
	const wchar_t* fileName,
	const char* description)
{
	std::lock_guard<std::mutex> lock(AudioMutex);
	if (PluginShutdownRequested.load(std::memory_order_relaxed) || fileName == nullptr)
		return false;
	const std::filesystem::path audioPath = ResolveRuntimeAudioPath(fileName);
	std::error_code ec;
	const bool fileAvailable = !audioPath.empty() && std::filesystem::is_regular_file(audioPath, ec);
	const PlaybackSource source = PlayWithFallback(
		fileAvailable ? audioPath.c_str() : nullptr,
		HINSTANCE(&__ImageBase), BuiltInSound(fileName), ::PlaySoundW);
	if (source == PlaybackSource::BuiltIn)
	{
		Logger::info(std::string(description) + " using built-in sound; external file " +
			(fileAvailable ? "could not be played: " : "missing or inaccessible: ") + audioPath.u8string());
	}
	if (source == PlaybackSource::Failed)
	{
		Logger::info(std::string(description) + " audio playback failed, including built-in fallback: " +
			audioPath.u8string() + "; check Windows audio output and EuroScope volume/mute settings");
		return false;
	}
	AudioStarted = true;
	return true;
}

void VsmrPluginRuntimeAudio::Stop()
{
	std::lock_guard<std::mutex> lock(AudioMutex);
	if (AudioStarted)
	{
		::PlaySoundW(nullptr, HINSTANCE(&__ImageBase), 0);
		AudioStarted = false;
	}
}
