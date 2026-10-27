#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/PluginRuntimeAudio.hpp"
#include "plugin/PluginRuntimeAudio.Internal.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "bootstrap/RuntimeContext.hpp"
#include "shared/logging/Logger.hpp"

#include <filesystem>
#include <fstream>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Mmsystem.h"

namespace
{
	std::mutex AudioMutex;
	bool AudioStarted = false;
	struct AlarmPlayback
	{
		VsmrPluginRuntimeAudio::PcmWave wave;
		HWAVEOUT device = nullptr;
		HANDLE event = nullptr;
		WAVEHDR header{};
	};
	std::unique_ptr<AlarmPlayback> ActiveAlarm;
	std::thread AlarmWorker;
	std::atomic_bool AlarmStopRequested{ false };

	void StopAlarmUnderLock()
	{
		AlarmStopRequested.store(true, std::memory_order_release);
		if (AlarmWorker.joinable()) AlarmWorker.join();
		ActiveAlarm.reset();
		AlarmStopRequested.store(false, std::memory_order_release);
	}

	bool ReadWaveFile(const std::filesystem::path& path, VsmrPluginRuntimeAudio::PcmWave& wave)
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec || size < 44 || size > 21 * 1024 * 1024)
			return false;
		std::ifstream input(path, std::ios::binary);
		if (!input) return false;
		std::vector<char> bytes(static_cast<std::size_t>(size));
		input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		return input && VsmrPluginRuntimeAudio::ParsePcmWave(bytes, wave);
	}

	bool ReadBuiltInAlarm(VsmrPluginRuntimeAudio::PcmWave& wave)
	{
		const HINSTANCE module = HINSTANCE(&__ImageBase);
		const HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(IDR_TIMER_ALARM_WAVE), L"WAVE");
		if (resource == nullptr) return false;
		const DWORD size = ::SizeofResource(module, resource);
		const HGLOBAL loaded = ::LoadResource(module, resource);
		const void* data = loaded != nullptr ? ::LockResource(loaded) : nullptr;
		if (data == nullptr || size < 44 || size > 21 * 1024 * 1024) return false;
		const auto* first = static_cast<const char*>(data);
		return VsmrPluginRuntimeAudio::ParsePcmWave(
			std::vector<char>(first, first + size), wave);
	}

	bool StartIndependentAlarm(const std::filesystem::path& path, bool fileAvailable)
	{
		StopAlarmUnderLock();
		auto playback = std::make_unique<AlarmPlayback>();
		const bool fromFile = fileAvailable && ReadWaveFile(path, playback->wave);
		if (!fromFile && !ReadBuiltInAlarm(playback->wave))
			return false;
		playback->event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (playback->event == nullptr)
			return false;
		MMRESULT result = ::waveOutOpen(&playback->device, WAVE_MAPPER,
			&playback->wave.format, reinterpret_cast<DWORD_PTR>(playback->event), 0, CALLBACK_EVENT);
		if (result != MMSYSERR_NOERROR)
		{
			::CloseHandle(playback->event);
			Logger::info("Timer alarm independent output could not open: WinMM " + std::to_string(result));
			return false;
		}
		playback->header.lpData = playback->wave.samples.data();
		playback->header.dwBufferLength = static_cast<DWORD>(playback->wave.samples.size());
		result = ::waveOutPrepareHeader(playback->device, &playback->header, sizeof(playback->header));
		if (result == MMSYSERR_NOERROR)
			result = ::waveOutWrite(playback->device, &playback->header, sizeof(playback->header));
		if (result != MMSYSERR_NOERROR)
		{
			::waveOutReset(playback->device);
			if (playback->header.dwFlags & WHDR_PREPARED)
				::waveOutUnprepareHeader(playback->device, &playback->header, sizeof(playback->header));
			::waveOutClose(playback->device);
			::CloseHandle(playback->event);
			Logger::info("Timer alarm independent output could not start: WinMM " + std::to_string(result));
			return false;
		}
		AlarmPlayback* const active = playback.get();
		try
		{
			AlarmWorker = std::thread([active]
			{
				bool finished = false;
				while (!AlarmStopRequested.load(std::memory_order_acquire))
				{
					if (active->header.dwFlags & WHDR_DONE)
					{
						finished = true;
						break;
					}
					if (::WaitForSingleObject(active->event, 100) == WAIT_FAILED)
						break;
				}
				if (!finished) ::waveOutReset(active->device);
				::waveOutUnprepareHeader(active->device, &active->header, sizeof(active->header));
				::waveOutClose(active->device);
				::CloseHandle(active->event);
				Logger::info(finished ? "Timer alarm playback completed on independent output" :
					"Timer alarm playback interrupted on independent output");
			});
		}
		catch (...)
		{
			::waveOutReset(playback->device);
			::waveOutUnprepareHeader(playback->device, &playback->header, sizeof(playback->header));
			::waveOutClose(playback->device);
			::CloseHandle(playback->event);
			return false;
		}
		ActiveAlarm = std::move(playback);
		Logger::info(std::string("Timer alarm playback started on independent output from ") +
			(fromFile ? "file: " + path.u8string() : "built-in WAV resource"));
		return true;
	}

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
	if (_wcsicmp(fileName, L"Alarm.wav") == 0 && StartIndependentAlarm(audioPath, fileAvailable))
		return true;
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
	Logger::info(std::string(description) + " playback started from " +
		(source == PlaybackSource::File ? "file: " + audioPath.u8string() : "built-in WAV resource"));
	AudioStarted = true;
	return true;
}

void VsmrPluginRuntimeAudio::Stop()
{
	std::lock_guard<std::mutex> lock(AudioMutex);
	StopAlarmUnderLock();
	if (AudioStarted)
	{
		::PlaySoundW(nullptr, HINSTANCE(&__ImageBase), 0);
		AudioStarted = false;
	}
}
