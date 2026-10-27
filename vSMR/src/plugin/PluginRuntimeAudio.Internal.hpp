#pragma once

#include <Windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>
#include "platform/windows/ResourceIds.h"

namespace VsmrPluginRuntimeAudio
{
	struct PcmWave
	{
		WAVEFORMATEX format{};
		std::vector<char> samples;
	};

	inline bool ParsePcmWave(const std::vector<char>& bytes, PcmWave& output)
	{
		const auto byte = [&](std::size_t offset) { return static_cast<std::uint8_t>(bytes[offset]); };
		const auto word = [&](std::size_t offset) {
			return static_cast<std::uint16_t>(byte(offset) | (byte(offset + 1) << 8));
		};
		const auto dword = [&](std::size_t offset) {
			return static_cast<std::uint32_t>(byte(offset)) |
				(static_cast<std::uint32_t>(byte(offset + 1)) << 8) |
				(static_cast<std::uint32_t>(byte(offset + 2)) << 16) |
				(static_cast<std::uint32_t>(byte(offset + 3)) << 24);
		};
		if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
			std::memcmp(bytes.data() + 8, "WAVE", 4) != 0 ||
			dword(4) > bytes.size() - 8)
			return false;
		const std::size_t end = static_cast<std::size_t>(dword(4)) + 8;
		bool hasFormat = false;
		bool hasSamples = false;
		PcmWave parsed;
		for (std::size_t offset = 12; offset + 8 <= end;)
		{
			const std::size_t length = dword(offset + 4);
			const std::size_t payload = offset + 8;
			if (length > end - payload)
				return false;
			if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && !hasFormat)
			{
				if (length < 16 || word(payload) != WAVE_FORMAT_PCM)
					return false;
				parsed.format.wFormatTag = WAVE_FORMAT_PCM;
				parsed.format.nChannels = word(payload + 2);
				parsed.format.nSamplesPerSec = dword(payload + 4);
				parsed.format.nAvgBytesPerSec = dword(payload + 8);
				parsed.format.nBlockAlign = word(payload + 12);
				parsed.format.wBitsPerSample = word(payload + 14);
				if (parsed.format.nChannels < 1 || parsed.format.nChannels > 2 ||
					parsed.format.nSamplesPerSec < 8000 || parsed.format.nSamplesPerSec > 192000 ||
					(parsed.format.wBitsPerSample != 8 && parsed.format.wBitsPerSample != 16 &&
						parsed.format.wBitsPerSample != 24 && parsed.format.wBitsPerSample != 32) ||
					parsed.format.nBlockAlign != parsed.format.nChannels * parsed.format.wBitsPerSample / 8 ||
					parsed.format.nAvgBytesPerSec != parsed.format.nSamplesPerSec * parsed.format.nBlockAlign)
					return false;
				hasFormat = true;
			}
			else if (std::memcmp(bytes.data() + offset, "data", 4) == 0 && !hasSamples)
			{
				if (length == 0 || length > 20 * 1024 * 1024)
						return false;
				parsed.samples.assign(bytes.begin() + payload, bytes.begin() + payload + length);
				hasSamples = true;
			}
			offset = payload + length + (length & 1U);
		}
		if (!hasFormat || !hasSamples || parsed.samples.size() % parsed.format.nBlockAlign != 0)
			return false;
		output = std::move(parsed);
		return true;
	}

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
