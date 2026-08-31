#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/PluginRuntimeAudio.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "shared/logging/Logger.hpp"

#include <filesystem>
#include <string>

#include "Mmsystem.h"

namespace
{
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
	const std::filesystem::path audioPath = ResolveRuntimeAudioPath(fileName);
	if (audioPath.empty())
	{
		Logger::info(std::string("Unable to resolve ") + description + " audio path");
		return false;
	}

	std::error_code ec;
	if (!std::filesystem::is_regular_file(audioPath, ec))
	{
		Logger::info(std::string(description) + " audio file is missing: " + audioPath.u8string());
		return false;
	}

	if (!::PlaySoundW(audioPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
	{
		Logger::info(std::string(description) + " audio playback failed: " + audioPath.u8string());
		return false;
	}

	return true;
}
