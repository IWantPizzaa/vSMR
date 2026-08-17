#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.hpp"

#include "aviso/AvisoDocumentModel.hpp"
#include "platform/windows/network/HttpHelper.hpp"
#include "insets/InsetWindow.hpp"
#include "plugin/Plugin.hpp"
#include "radar/RadarScreen.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/RuntimeResourceFiles.hpp"
#include "crash/CrashReportSupport.hpp"
#include "diagnostics/PerformanceDiagnostics.hpp"

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

extern std::vector<CSMRRadar*> RadarScreensOpened;

namespace
{
	constexpr int kBridgeProtocolVersion = 1;
	constexpr size_t kMaximumBridgeMessageBytes = 32u * 1024u * 1024u;
	std::mutex gBridgeSaveTransactionMutex;

	using Allocator = rapidjson::Document::AllocatorType;

	std::string TrimAscii(std::string value)
	{
		auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
			return !isSpace(static_cast<unsigned char>(c));
		}));
		value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
			return !isSpace(static_cast<unsigned char>(c));
		}).base(), value.end());
		return value;
	}

	std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	}

	std::string UpperAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
	}

	bool EqualsNoCase(const std::string& left, const std::string& right)
	{
		return LowerAscii(left) == LowerAscii(right);
	}

	std::string NormalizeAirportCandidate(std::string value)
	{
		value = UpperAscii(TrimAscii(value));
		if (value.size() != 4 || value == "JSON" || value == "AVIS" || value == "GEOJ")
			return "";
		for (const unsigned char character : value)
		{
			if (std::isalnum(character) == 0)
				return "";
		}
		return value;
	}

	std::string DetectAvisoAirport(
		const rapidjson::Value& document,
		std::string sourceHint)
	{
		const char* keys[] = {
			"icao", "icao_code", "airport_icao", "airport_code", "airport", "active_airport"
		};
		auto findInObject = [&](const rapidjson::Value& object) -> std::string
		{
			if (!object.IsObject())
				return "";
			for (const char* key : keys)
			{
				if (!object.HasMember(key) || !object[key].IsString())
					continue;
				const std::string candidate = NormalizeAirportCandidate(object[key].GetString());
				if (!candidate.empty())
					return candidate;
			}
			return "";
		};

		std::string airport = findInObject(document);
		if (!airport.empty())
			return airport;
		if (document.IsObject() && document.HasMember("metadata"))
		{
			airport = findInObject(document["metadata"]);
			if (!airport.empty())
				return airport;
		}
		if (document.IsObject() && document.HasMember("properties"))
		{
			airport = findInObject(document["properties"]);
			if (!airport.empty())
				return airport;
		}

		std::replace(sourceHint.begin(), sourceHint.end(), '\\', '/');
		const size_t suffix = sourceHint.find_first_of("?#");
		if (suffix != std::string::npos)
			sourceHint.resize(suffix);
		const size_t slash = sourceHint.find_last_of('/');
		if (slash != std::string::npos)
			sourceHint = sourceHint.substr(slash + 1);

		// Work only with the basename stem so URL path segments and extensions
		// such as blob/json/geojson can never be mistaken for an airport. Accept
		// either a bare ICAO filename (LFPO.geojson) or the ICAO token directly
		// beside AVISO (LFPO_AVISO.geojson / AVISO_LFPO.geojson).
		const size_t extension = sourceHint.find_last_of('.');
		if (extension != std::string::npos)
			sourceHint.resize(extension);
		sourceHint = UpperAscii(sourceHint);

		std::vector<std::string> tokens;
		std::string token;
		for (const unsigned char character : sourceHint)
		{
			if (std::isalnum(character) != 0)
			{
				token.push_back(static_cast<char>(character));
				continue;
			}
			if (!token.empty())
			{
				tokens.push_back(std::move(token));
				token.clear();
			}
		}
		if (!token.empty())
			tokens.push_back(std::move(token));

		if (tokens.size() == 1)
			return NormalizeAirportCandidate(tokens.front());
		for (size_t index = 0; index < tokens.size(); ++index)
		{
			if (tokens[index] != "AVISO")
				continue;
			if (index > 0)
			{
				airport = NormalizeAirportCandidate(tokens[index - 1]);
				if (!airport.empty())
					return airport;
			}
			if (index + 1 < tokens.size())
			{
				airport = NormalizeAirportCandidate(tokens[index + 1]);
				if (!airport.empty())
					return airport;
			}
		}
		return "";
	}

	std::string ReadString(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsString())
			return "";
		return object[key].GetString();
	}

	bool ReadBool(const rapidjson::Value& object, const char* key, bool fallback)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsBool())
			return fallback;
		return object[key].GetBool();
	}

	int ReadInt(const rapidjson::Value& object, const char* key, int fallback)
	{
		if (!object.IsObject() || key == nullptr || !object.HasMember(key) || !object[key].IsInt())
			return fallback;
		return object[key].GetInt();
	}

	std::uint32_t NormalizePerformanceWindowSeconds(int requested)
	{
		switch (requested)
		{
		case 30:
		case 120:
		case 600:
			return static_cast<std::uint32_t>(requested);
		default:
			return 120;
		}
	}

	std::size_t NormalizePerformanceSeriesPoints(int requested)
	{
		if (requested <= 0)
			return 120;
		return static_cast<std::size_t>((std::clamp)(requested, 1, 600));
	}

	std::string FormatUtcMilliseconds(std::uint64_t utcMilliseconds)
	{
		if (utcMilliseconds == 0)
			return {};
		const std::time_t seconds = static_cast<std::time_t>(utcMilliseconds / 1000ULL);
		std::tm utc = {};
		if (gmtime_s(&utc, &seconds) != 0)
			return {};
		char formatted[40] = {};
		_snprintf_s(
			formatted,
			_TRUNCATE,
			"%04d-%02d-%02dT%02d:%02d:%02d.%03lluZ",
			utc.tm_year + 1900,
			utc.tm_mon + 1,
			utc.tm_mday,
			utc.tm_hour,
			utc.tm_min,
			utc.tm_sec,
			static_cast<unsigned long long>(utcMilliseconds % 1000ULL));
		return formatted;
	}

	std::filesystem::path EnvironmentDirectory(const wchar_t* variable)
	{
		if (variable == nullptr || variable[0] == L'\0')
			return {};
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD required = ::GetEnvironmentVariableW(
				variable,
				buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (required == 0 || required > 32768U)
				return {};
			if (required < buffer.size())
				return std::filesystem::path(buffer.data());
			buffer.resize(static_cast<std::size_t>(required) + 1U);
		}
	}

	std::filesystem::path TemporaryDirectory()
	{
		std::vector<wchar_t> buffer(512U);
		for (;;)
		{
			const DWORD required = ::GetTempPathW(
				static_cast<DWORD>(buffer.size()),
				buffer.data());
			if (required == 0 || required > 32768U)
				return {};
			if (required < buffer.size())
				return std::filesystem::path(buffer.data());
			buffer.resize(static_cast<std::size_t>(required) + 1U);
		}
	}

	bool WritePerformanceReportAtomically(
		const std::string& reportJson,
		const std::filesystem::path& dataDirectory,
		std::string& reportPath,
		std::string& error)
	{
		reportPath.clear();
		error.clear();
		if (reportJson.empty())
		{
			error = "The performance report is empty.";
			return false;
		}

		const std::filesystem::path localAppData = EnvironmentDirectory(L"LOCALAPPDATA");
		const std::filesystem::path temporaryBase = TemporaryDirectory();
		const std::array<std::filesystem::path, 3> candidates = {
			dataDirectory.empty() ? std::filesystem::path{} : dataDirectory / L"Diagnostics",
			localAppData.empty()
				? std::filesystem::path{}
				: localAppData / L"vSMR" / L"Diagnostics",
			temporaryBase.empty()
				? std::filesystem::path{}
				: temporaryBase / L"vSMR" / L"Diagnostics"
		};
		const std::filesystem::path directory =
			VsmrCrashSupport::SelectFirstWritableDirectory(candidates);
		if (directory.empty())
		{
			error = "No writable performance diagnostics folder is available.";
			return false;
		}

		SYSTEMTIME utc = {};
		::GetSystemTime(&utc);
		char timestamp[40] = {};
		_snprintf_s(
			timestamp,
			_TRUNCATE,
			"%04u%02u%02u_%02u%02u%02u_%03u",
			utc.wYear,
			utc.wMonth,
			utc.wDay,
			utc.wHour,
			utc.wMinute,
			utc.wSecond,
			utc.wMilliseconds);

		static volatile LONG temporarySequence = 0;
		const LONG sequence = ::InterlockedIncrement(&temporarySequence);
		const std::wstring temporaryName =
			L".vsmr-performance-" + std::to_wstring(::GetCurrentProcessId()) +
			L"-" + std::to_wstring(::GetTickCount64()) +
			L"-" + std::to_wstring(sequence) + L".tmp";
		const std::filesystem::path temporary = directory / temporaryName;
		const std::wstring nativeTemporary = VsmrCrashSupport::MakeNativePath(temporary);
		HANDLE output = ::CreateFileW(
			nativeTemporary.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (output == INVALID_HANDLE_VALUE)
		{
			error = "Unable to create the temporary performance report.";
			return false;
		}

		bool writeSucceeded = true;
		std::size_t writtenTotal = 0;
		while (writtenTotal < reportJson.size())
		{
			const DWORD requested = static_cast<DWORD>((std::min)(
				reportJson.size() - writtenTotal,
				static_cast<std::size_t>(1024U * 1024U)));
			DWORD written = 0;
			if (::WriteFile(
				output,
				reportJson.data() + writtenTotal,
				requested,
				&written,
				nullptr) == FALSE || written != requested)
			{
				writeSucceeded = false;
				break;
			}
			writtenTotal += written;
		}
		if (writeSucceeded)
			writeSucceeded = ::FlushFileBuffers(output) != FALSE;
		::CloseHandle(output);
		if (!writeSucceeded)
		{
			::DeleteFileW(nativeTemporary.c_str());
			error = "Unable to write the performance report.";
			return false;
		}

		for (unsigned int suffix = 0; suffix < 1000; ++suffix)
		{
			std::string filename = "vSMR_performance_" + std::string(timestamp);
			if (suffix > 0)
				filename += "_" + std::to_string(suffix + 1);
			filename += ".json";
			const std::filesystem::path target = directory / filename;
			const std::wstring nativeTarget = VsmrCrashSupport::MakeNativePath(target);
			if (::MoveFileExW(
				nativeTemporary.c_str(),
				nativeTarget.c_str(),
				MOVEFILE_WRITE_THROUGH) != FALSE)
			{
				reportPath = std::filesystem::path(
					VsmrCrashSupport::DisplayPath(nativeTarget)).u8string();
				return true;
			}
			const DWORD moveError = ::GetLastError();
			if (moveError != ERROR_FILE_EXISTS && moveError != ERROR_ALREADY_EXISTS)
				break;
		}

		::DeleteFileW(nativeTemporary.c_str());
		error = "Unable to finalize the performance report.";
		return false;
	}

	void AddString(
		rapidjson::Value& object,
		const char* key,
		const std::string& value,
		Allocator& allocator)
	{
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value stringValue;
		stringValue.SetString(
			value.c_str(),
			static_cast<rapidjson::SizeType>(value.size()),
			allocator);
		object.AddMember(keyValue, stringValue, allocator);
	}

	void AddUint64(
		rapidjson::Value& object,
		const char* key,
		std::uint64_t value,
		Allocator& allocator)
	{
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value number;
		number.SetUint64(value);
		object.AddMember(keyValue, number, allocator);
	}

	void AddSize(
		rapidjson::Value& object,
		const char* key,
		std::size_t value,
		Allocator& allocator)
	{
		AddUint64(object, key, static_cast<std::uint64_t>(value), allocator);
	}

	void AddDistribution(
		rapidjson::Value& timings,
		const char* name,
		const VsmrPerformance::Distribution& distribution,
		double latest,
		bool hasLatest,
		Allocator& allocator)
	{
		rapidjson::Value item(rapidjson::kObjectType);
		AddSize(item, "sampleCount", distribution.sampleCount, allocator);
		if (distribution.sampleCount > 0)
		{
			if (hasLatest)
				item.AddMember("latest", latest, allocator);
			item.AddMember("average", distribution.average, allocator);
			item.AddMember("median", distribution.median, allocator);
			item.AddMember("p95", distribution.p95, allocator);
			item.AddMember("max", distribution.maximum, allocator);
		}
		rapidjson::Value key;
		key.SetString(name, allocator);
		timings.AddMember(key, item, allocator);
	}

	std::vector<std::string> PerformanceRefreshReasonLabels(std::uint32_t mask)
	{
		std::vector<std::string> labels = VsmrPerformance::RefreshReasonNames(mask);
		if (labels.empty())
			labels.emplace_back("unspecified");
		return labels;
	}

	std::string JoinPerformanceRefreshReasons(std::uint32_t mask)
	{
		const std::vector<std::string> labels = PerformanceRefreshReasonLabels(mask);
		std::ostringstream output;
		for (std::size_t index = 0; index < labels.size(); ++index)
		{
			if (index != 0)
				output << " + ";
			output << labels[index];
		}
		return output.str();
	}

	void AddCacheItem(
		rapidjson::Value& caches,
		const char* id,
		const char* name,
		std::uint64_t exactHits,
		std::uint64_t previewHits,
		std::uint64_t misses,
		std::size_t entries,
		Allocator& allocator)
	{
		rapidjson::Value item(rapidjson::kObjectType);
		AddString(item, "id", id, allocator);
		AddString(item, "name", name, allocator);
		AddUint64(item, "hits", exactHits, allocator);
		AddUint64(item, "previewHits", previewHits, allocator);
		AddUint64(item, "misses", misses, allocator);
		const std::uint64_t accesses = exactHits + previewHits + misses;
		if (accesses > 0)
			item.AddMember(
				"hitRate",
				static_cast<double>(exactHits + previewHits) /
					static_cast<double>(accesses),
				allocator);
		else
		{
			rapidjson::Value nullValue;
			nullValue.SetNull();
			item.AddMember("hitRate", nullValue, allocator);
		}
		AddSize(item, "entries", entries, allocator);
		caches.PushBack(item, allocator);
	}

	void SetStringMember(
		rapidjson::Value& object,
		const char* key,
		const std::string& value,
		Allocator& allocator)
	{
		rapidjson::Value stringValue;
		stringValue.SetString(
			value.c_str(),
			static_cast<rapidjson::SizeType>(value.size()),
			allocator);
		if (object.HasMember(key))
			object[key] = stringValue;
		else
		{
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			object.AddMember(keyValue, stringValue, allocator);
		}
	}

	void SetBoolMember(
		rapidjson::Value& object,
		const char* key,
		bool value,
		Allocator& allocator)
	{
		if (object.HasMember(key))
			object[key].SetBool(value);
		else
		{
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value boolValue;
			boolValue.SetBool(value);
			object.AddMember(keyValue, boolValue, allocator);
		}
	}

	void CloneJsonValue(
		const rapidjson::Value& source,
		rapidjson::Value& destination,
		Allocator& allocator)
	{
		if (source.IsObject())
		{
			destination.SetObject();
			for (rapidjson::Value::ConstMemberIterator member = source.MemberBegin();
				member != source.MemberEnd();
				++member)
			{
				rapidjson::Value key;
				key.SetString(
					member->name.GetString(),
					member->name.GetStringLength(),
					allocator);
				rapidjson::Value value;
				CloneJsonValue(member->value, value, allocator);
				destination.AddMember(key, value, allocator);
			}
			return;
		}
		if (source.IsArray())
		{
			destination.SetArray();
			for (rapidjson::SizeType index = 0; index < source.Size(); ++index)
			{
				rapidjson::Value value;
				CloneJsonValue(source[index], value, allocator);
				destination.PushBack(value, allocator);
			}
			return;
		}
		if (source.IsString())
		{
			destination.SetString(
				source.GetString(),
				source.GetStringLength(),
				allocator);
			return;
		}
		if (source.IsBool()) { destination.SetBool(source.GetBool()); return; }
		if (source.IsInt()) { destination.SetInt(source.GetInt()); return; }
		if (source.IsUint()) { destination.SetUint(source.GetUint()); return; }
		if (source.IsInt64()) { destination.SetInt64(source.GetInt64()); return; }
		if (source.IsUint64()) { destination.SetUint64(source.GetUint64()); return; }
		if (source.IsDouble()) { destination.SetDouble(source.GetDouble()); return; }
		destination.SetNull();
	}

	rapidjson::Value& EnsureObjectMember(
		rapidjson::Value& object,
		const char* key,
		Allocator& allocator)
	{
		if (!object.IsObject())
			object.SetObject();
		if (!object.HasMember(key) || !object[key].IsObject())
		{
			if (object.HasMember(key))
				object.RemoveMember(key);
			rapidjson::Value keyValue;
			keyValue.SetString(key, allocator);
			rapidjson::Value member(rapidjson::kObjectType);
			object.AddMember(keyValue, member, allocator);
		}
		return object[key];
	}

	void CopyOrReplaceMember(
		rapidjson::Value& destination,
		const char* key,
		const rapidjson::Value& source,
		Allocator& allocator)
	{
		if (destination.HasMember(key))
			destination.RemoveMember(key);
		rapidjson::Value keyValue;
		keyValue.SetString(key, allocator);
		rapidjson::Value copy;
		CloneJsonValue(source, copy, allocator);
		destination.AddMember(keyValue, copy, allocator);
	}

	std::string SerializeCompact(const rapidjson::Value& value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		value.Accept(writer);
		return std::string(buffer.GetString(), buffer.Size());
	}

	std::string SerializePretty(const rapidjson::Value& value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.SetIndent('\t', 1);
		value.Accept(writer);
		return std::string(buffer.GetString(), buffer.Size());
	}

	VsmrBridgeAction ActionFromType(const std::string& requestedType)
	{
		const std::string type = LowerAscii(TrimAscii(requestedType));
		if (type == "ui.ready") return VsmrBridgeAction::UiReady;
		if (type == "window.close") return VsmrBridgeAction::WindowClose;
		if (type == "window.drag" || type == "window.drag.start") return VsmrBridgeAction::WindowDragStart;
		if (type == "state.save" || type == "save.all") return VsmrBridgeAction::StateSave;
		if (type == "state.reload" || type == "reload.all") return VsmrBridgeAction::StateReload;
		if (type == "state.reset") return VsmrBridgeAction::StateReset;
		if (type == "state.restore.backup") return VsmrBridgeAction::StateRestoreBackup;
		if (type == "state.undo" || type == "undo") return VsmrBridgeAction::StateUndo;
		if (type == "state.redo" || type == "redo") return VsmrBridgeAction::StateRedo;
		if (type == "runtime.profile.change") return VsmrBridgeAction::RuntimeProfileChange;
		if (type == "runtime.mode.change") return VsmrBridgeAction::RuntimeModeChange;
		if (type == "aviso.group.visibility") return VsmrBridgeAction::RuntimeGroupVisibility;
		if (type == "aviso.groups.visibility") return VsmrBridgeAction::RuntimeGroupsVisibility;
		if (type == "aviso.groups.update") return VsmrBridgeAction::RuntimeGroupsUpdate;
		if (type == "aviso.inset.toggle") return VsmrBridgeAction::RuntimeInsetToggle;
		if (type == "display.srw.toggle") return VsmrBridgeAction::RuntimeSrwToggle;
		if (type == "aviso.inset.preset.load") return VsmrBridgeAction::InsetPresetLoad;
		if (type == "aviso.inset.preset.capture") return VsmrBridgeAction::InsetPresetCapture;
		if (type == "aviso.inset.preset.update") return VsmrBridgeAction::InsetPresetUpdate;
		if (type == "aviso.inset.preset.rename") return VsmrBridgeAction::InsetPresetRename;
		if (type == "aviso.inset.preset.duplicate") return VsmrBridgeAction::InsetPresetDuplicate;
		if (type == "aviso.inset.preset.default") return VsmrBridgeAction::InsetPresetDefault;
		if (type == "aviso.inset.preset.reset") return VsmrBridgeAction::InsetPresetReset;
		if (type == "aviso.inset.preset.delete") return VsmrBridgeAction::InsetPresetDelete;
		if (type == "aviso.inset.preset.linked") return VsmrBridgeAction::InsetPresetLinked;
		if (type == "aviso.inset.preset.legacy.assign") return VsmrBridgeAction::InsetPresetLegacyAssign;
		if (type == "alerts.update") return VsmrBridgeAction::AlertsUpdate;
		if (type == "settings.update") return VsmrBridgeAction::SettingsUpdate;
		if (type == "datalink.state.request") return VsmrBridgeAction::DatalinkStateRequest;
		if (type == "datalink.settings.update") return VsmrBridgeAction::DatalinkSettingsUpdate;
		if (type == "datalink.connection.connect") return VsmrBridgeAction::DatalinkConnect;
		if (type == "datalink.connection.disconnect") return VsmrBridgeAction::DatalinkDisconnect;
		if (type == "datalink.poll") return VsmrBridgeAction::DatalinkPoll;
		if (type == "cdm.scan") return VsmrBridgeAction::CdmScan;
		if (type == "performance.state.request") return VsmrBridgeAction::PerformanceStateRequest;
		if (type == "performance.reset") return VsmrBridgeAction::PerformanceReset;
		if (type == "performance.report.export") return VsmrBridgeAction::PerformanceReportExport;
		if (type == "resource.computer.load" ||
			type == "profiles.load.computer" ||
			type == "aviso.load.computer" ||
			type == "browse.profiles" ||
			type == "browse.aviso")
			return VsmrBridgeAction::ResourceComputerLoad;
		if (type == "resource.github.load" ||
			type == "profiles.load.github" ||
			type == "aviso.load.github")
			return VsmrBridgeAction::ResourceGithubLoad;
		return VsmrBridgeAction::Unknown;
	}

	struct DecodedEnvelope
	{
		int version = 0;
		std::string id;
		std::string type;
		VsmrBridgeAction action = VsmrBridgeAction::Unknown;
		const rapidjson::Value* payload = nullptr;
	};

	bool DecodeEnvelope(
		const rapidjson::Document& document,
		DecodedEnvelope& envelope,
		std::string& error)
	{
		error.clear();
		if (!document.IsObject())
		{
			error = "Bridge message must be a JSON object.";
			return false;
		}

		envelope.version = 1;
		if (document.HasMember("version"))
		{
			if (!document["version"].IsInt())
			{
				error = "Bridge message version must be an integer.";
				return false;
			}
			envelope.version = document["version"].GetInt();
		}
		if (envelope.version != kBridgeProtocolVersion)
		{
			error = "Unsupported bridge protocol version.";
			return false;
		}

		envelope.id = ReadString(document, "id");
		envelope.type = ReadString(document, "type");
		if (envelope.type.empty())
			envelope.type = ReadString(document, "action");
		if (envelope.type.empty())
		{
			error = "Bridge message type is required.";
			return false;
		}

		envelope.action = ActionFromType(envelope.type);
		if (document.HasMember("payload"))
			envelope.payload = &document["payload"];
		return true;
	}

	void MakeEnvelope(
		rapidjson::Document& document,
		const std::string& type,
		const std::string& requestId)
	{
		document.SetObject();
		Allocator& allocator = document.GetAllocator();
		document.AddMember("version", kBridgeProtocolVersion, allocator);
		if (!requestId.empty())
			AddString(document, "id", requestId, allocator);
		AddString(document, "type", type, allocator);
	}

	bool IsProfileEntry(const rapidjson::Value& value)
	{
		return value.IsObject() &&
			value.HasMember("name") &&
			value["name"].IsString() &&
			!TrimAscii(value["name"].GetString()).empty();
	}

	bool IsMetadataEntry(const rapidjson::Value& value)
	{
		return value.IsObject() &&
			value.HasMember("_vsmr") &&
			value["_vsmr"].IsObject() &&
			!value.HasMember("name");
	}

	bool ValidateProfileArray(const rapidjson::Value& profiles, std::string& error)
	{
		rapidjson::Document candidate;
		candidate.Parse<0>(SerializeCompact(profiles).c_str());
		if (candidate.HasParseError())
		{
			error = "Profiles state could not be parsed.";
			return false;
		}
		bool migrated = false;
		return CConfig::validateAndMigrateProfilesDocument(candidate, error, migrated);
	}

	bool CreateRollbackSnapshot(
		const std::string& source,
		std::string& snapshotPath)
	{
		snapshotPath.clear();
		if (source.empty())
			return false;

		for (int attempt = 0; attempt < 128; ++attempt)
		{
			std::ostringstream candidate;
			candidate << source
				<< ".transaction-rollback."
				<< ::GetCurrentProcessId()
				<< "."
				<< ::GetTickCount()
				<< "."
				<< attempt;
			const std::string candidatePath = candidate.str();
			if (::CopyFileA(source.c_str(), candidatePath.c_str(), TRUE))
			{
				snapshotPath = candidatePath;
				break;
			}

			const DWORD copyError = ::GetLastError();
			if (copyError != ERROR_FILE_EXISTS &&
				copyError != ERROR_ALREADY_EXISTS)
			{
				return false;
			}
		}
		if (snapshotPath.empty())
			return false;

		HANDLE snapshotFile = ::CreateFileA(
			snapshotPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		const bool flushed =
			snapshotFile != INVALID_HANDLE_VALUE &&
			::FlushFileBuffers(snapshotFile) != FALSE;
		if (snapshotFile != INVALID_HANDLE_VALUE)
			::CloseHandle(snapshotFile);
		if (!flushed)
		{
			::DeleteFileA(snapshotPath.c_str());
			snapshotPath.clear();
			return false;
		}
		return true;
	}

	bool RestoreRollbackSnapshotAtomically(
		const std::string& snapshotPath,
		const std::string& destination)
	{
		return !snapshotPath.empty() &&
			!destination.empty() &&
			::MoveFileExA(
				snapshotPath.c_str(),
				destination.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	}

	bool DeleteRollbackSnapshot(std::string& snapshotPath)
	{
		if (snapshotPath.empty())
			return true;
		const bool deleted =
			::DeleteFileA(snapshotPath.c_str()) != FALSE ||
			::GetLastError() == ERROR_FILE_NOT_FOUND;
		if (deleted)
			snapshotPath.clear();
		return deleted;
	}

	void MergeProfileArrayPreservingTopLevelUnknowns(
		const rapidjson::Value& original,
		const rapidjson::Value& incoming,
		rapidjson::Document& output)
	{
		output.SetArray();
		Allocator& allocator = output.GetAllocator();

		const rapidjson::Value* incomingMetadata = nullptr;
		bool hasIncomingUnknownEntries = false;
		for (rapidjson::SizeType i = 0; i < incoming.Size(); ++i)
		{
			const rapidjson::Value& item = incoming[i];
			if (IsMetadataEntry(item))
			{
				incomingMetadata = &item;
				continue;
			}

			hasIncomingUnknownEntries =
				hasIncomingUnknownEntries || !IsProfileEntry(item);
			rapidjson::Value copy;
			CloneJsonValue(item, copy, allocator);
			output.PushBack(copy, allocator);
		}

		const rapidjson::Value* originalMetadata = nullptr;
		if (original.IsArray())
		{
			for (rapidjson::SizeType i = 0; i < original.Size(); ++i)
			{
				const rapidjson::Value& item = original[i];
				if (IsMetadataEntry(item))
				{
					originalMetadata = &item;
					continue;
				}
				if (IsProfileEntry(item))
					continue;
				if (hasIncomingUnknownEntries)
					continue;

				rapidjson::Value copy;
				CloneJsonValue(item, copy, allocator);
				output.PushBack(copy, allocator);
			}
		}

		const rapidjson::Value* metadata = incomingMetadata != nullptr
			? incomingMetadata
			: originalMetadata;
		if (metadata != nullptr)
		{
			rapidjson::Value copy;
			CloneJsonValue(*metadata, copy, allocator);
			output.PushBack(copy, allocator);
		}
	}

	bool SamePersistedFeatureIdentity(
		const rapidjson::Value& left,
		const rapidjson::Value& right)
	{
		auto readId = [](const rapidjson::Value& feature) -> std::string
		{
			if (feature.IsObject() && feature.HasMember("id") && feature["id"].IsString())
				return feature["id"].GetString();
			if (feature.IsObject() &&
				feature.HasMember("properties") &&
				feature["properties"].IsObject() &&
				feature["properties"].HasMember("id") &&
				feature["properties"]["id"].IsString())
				return feature["properties"]["id"].GetString();
			return "";
		};

		const std::string leftId = readId(left);
		const std::string rightId = readId(right);
		if (leftId.empty() || rightId.empty())
			return true;
		return leftId == rightId;
	}

	void MergeAvisoPreservingCoordinates(
		rapidjson::Document& destination,
		const rapidjson::Value& incoming)
	{
		if (!destination.IsObject() || !incoming.IsObject())
		{
			CloneJsonValue(incoming, destination, destination.GetAllocator());
			return;
		}

		Allocator& allocator = destination.GetAllocator();
		for (auto member = incoming.MemberBegin(); member != incoming.MemberEnd(); ++member)
		{
			if (std::string(member->name.GetString()) == "features")
				continue;
			CopyOrReplaceMember(destination, member->name.GetString(), member->value, allocator);
		}

		if (!incoming.HasMember("features") || !incoming["features"].IsArray())
			return;
		if (!destination.HasMember("features") || !destination["features"].IsArray())
		{
			CopyOrReplaceMember(destination, "features", incoming["features"], allocator);
			return;
		}

		rapidjson::Value& currentFeatures = destination["features"];
		const rapidjson::Value& newFeatures = incoming["features"];
		if (currentFeatures.Size() != newFeatures.Size())
		{
			CopyOrReplaceMember(destination, "features", newFeatures, allocator);
			return;
		}

		for (rapidjson::SizeType index = 0; index < newFeatures.Size(); ++index)
		{
			rapidjson::Value& current = currentFeatures[index];
			const rapidjson::Value& updated = newFeatures[index];
			if (!current.IsObject() || !updated.IsObject() ||
				!SamePersistedFeatureIdentity(current, updated))
			{
				rapidjson::Value replacement;
				CloneJsonValue(updated, replacement, allocator);
				current = replacement;
				continue;
			}

			std::vector<std::string> keysToRemove;
			for (auto member = current.MemberBegin(); member != current.MemberEnd(); ++member)
			{
				const std::string key = member->name.GetString();
				if (key != "geometry" && !updated.HasMember(member->name.GetString()))
					keysToRemove.push_back(key);
			}
			for (const std::string& key : keysToRemove)
				current.RemoveMember(key.c_str());

			for (auto member = updated.MemberBegin(); member != updated.MemberEnd(); ++member)
			{
				const std::string key = member->name.GetString();
				if (key == "geometry")
					continue;
				CopyOrReplaceMember(current, key.c_str(), member->value, allocator);
			}
		}
	}

	bool ReadFileText(const std::string& path, std::string& text)
	{
		text.clear();
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
			return false;
		std::ostringstream buffer;
		buffer << input.rdbuf();
		text = buffer.str();
		return static_cast<bool>(input) || input.eof();
	}

	std::string RuntimeResourceFromType(
		const std::string& requestedType,
		const rapidjson::Value* payload)
	{
		if (payload != nullptr && payload->IsObject())
		{
			std::string resource = LowerAscii(ReadString(*payload, "resource"));
			if (resource.empty())
				resource = LowerAscii(ReadString(*payload, "kind"));
			if (resource == "profiles" || resource == "aviso")
				return resource;
		}
		const std::string type = LowerAscii(requestedType);
		return type.find("profile") != std::string::npos ? "profiles" : "aviso";
	}

	bool IsAllowedGithubUrl(const std::string& value)
	{
		const std::string url = TrimAscii(value);
		return HttpHelper::IsHttpsUrlForHost(url, "github.com") ||
			HttpHelper::IsHttpsUrlForHost(url, "www.github.com") ||
			HttpHelper::IsHttpsUrlForHost(url, "raw.githubusercontent.com");
	}

	std::string NormalizeGithubRawUrl(const std::string& value)
	{
		std::string url = TrimAscii(value);
		const std::string rawPrefix = "https://raw.githubusercontent.com/";
		if (HttpHelper::IsHttpsUrlForHost(url, "raw.githubusercontent.com"))
			return url;
		if (!HttpHelper::IsHttpsUrlForHost(url, "github.com") &&
			!HttpHelper::IsHttpsUrlForHost(url, "www.github.com"))
			return "";

		const size_t authorityEnd = url.find('/', url.find("://") + 3);
		if (authorityEnd == std::string::npos)
			return "";
		std::string path = url.substr(authorityEnd + 1);
		const size_t suffix = path.find_first_of("?#");
		if (suffix != std::string::npos)
			path.resize(suffix);
		const size_t blob = LowerAscii(path).find("/blob/");
		if (blob == std::string::npos)
			return "";
		const std::string repository = path.substr(0, blob);
		const std::string file = path.substr(blob + 6);
		if (repository.empty() || repository.find('/') == std::string::npos || file.empty())
			return "";
		return rawPrefix + repository + "/" + file;
	}
}

struct VsmrControlCenterBridge::Impl
{
	CSMRRadar* Owner = nullptr;
	VsmrBridgeHostCallbacks Callbacks;
	unsigned long long NativeMessageSequence = 0;
	mutable std::string AvisoHealthCachePath;
	mutable std::filesystem::file_time_type AvisoHealthCacheWriteTime{};
	mutable std::uintmax_t AvisoHealthCacheSize = 0;
	mutable bool AvisoHealthCacheExists = false;
	mutable bool AvisoHealthCacheHealthy = false;
	mutable std::string AvisoHealthCacheMessage;
	mutable std::string AvisoHealthCacheDocumentJson;
	std::uint32_t PeakProcessGdiObjects = 0;
	std::size_t PeakVsmrCachedBitmaps = 0;
	std::uint64_t PeakEstimatedBitmapBytes = 0;
	std::size_t PeakAvisoPendingDepth = 0;
	std::uint64_t LastPerformanceGeneration = 0;

	explicit Impl(CSMRRadar* owner, VsmrBridgeHostCallbacks callbacks)
		: Owner(owner), Callbacks(std::move(callbacks))
	{
	}

	std::string NextNativeId()
	{
		return "native-" + std::to_string(++NativeMessageSequence);
	}

	void Send(rapidjson::Document& message)
	{
		if (Callbacks.sendJson)
			Callbacks.sendJson(SerializeCompact(message));
	}

	void SendAck(
		const std::string& requestId,
		const std::string& action,
		const std::string& messageText = "")
	{
		rapidjson::Document message;
		MakeEnvelope(message, "state.ack", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload(rapidjson::kObjectType);
		AddString(payload, "action", action, allocator);
		if (!messageText.empty())
			AddString(payload, "message", messageText, allocator);
		message.AddMember("payload", payload, allocator);
		Send(message);
	}

	void SendError(const std::string& requestId, const std::string& messageText)
	{
		rapidjson::Document message;
		MakeEnvelope(message, "state.error", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload(rapidjson::kObjectType);
		AddString(payload, "message", messageText, allocator);
		message.AddMember("payload", payload, allocator);
		AddString(message, "message", messageText, allocator);
		Send(message);
	}

	std::string RadarIdentifier() const
	{
		if (Owner == nullptr)
			return {};
		const auto found = std::find(RadarScreensOpened.begin(), RadarScreensOpened.end(), Owner);
		if (found == RadarScreensOpened.end())
			return "radar-current";
		return "radar-" + std::to_string(
			static_cast<std::size_t>(std::distance(RadarScreensOpened.begin(), found)) + 1U);
	}

	static std::uint64_t MonotonicToUtc(
		const VsmrPerformance::Snapshot& snapshot,
		std::uint64_t monotonicMilliseconds)
	{
		if (snapshot.collectionStartedUtcMilliseconds == 0 ||
			snapshot.collectionStartedMonotonicMilliseconds == 0)
		{
			return 0;
		}
		if (monotonicMilliseconds >= snapshot.collectionStartedMonotonicMilliseconds)
		{
			return snapshot.collectionStartedUtcMilliseconds +
				(monotonicMilliseconds - snapshot.collectionStartedMonotonicMilliseconds);
		}
		const std::uint64_t difference =
			snapshot.collectionStartedMonotonicMilliseconds - monotonicMilliseconds;
		return difference <= snapshot.collectionStartedUtcMilliseconds
			? snapshot.collectionStartedUtcMilliseconds - difference
			: 0;
	}

	void AddTargetSummary(
		rapidjson::Value& targets,
		const char* name,
		const VsmrPerformance::Snapshot& snapshot,
		bool visible,
		Allocator& allocator) const
	{
		std::uint64_t total = 0;
		std::size_t maximum = 0;
		for (const VsmrPerformance::FrameSample& sample : snapshot.series)
		{
			const std::size_t value = visible
				? sample.visibleTargets
				: sample.processedTargets;
			total += static_cast<std::uint64_t>(value);
			maximum = (std::max)(maximum, value);
		}

		rapidjson::Value summary(rapidjson::kObjectType);
		if (!snapshot.series.empty())
		{
			const std::size_t latest = visible
				? snapshot.latestFrame.visibleTargets
				: snapshot.latestFrame.processedTargets;
			AddSize(summary, "latest", latest, allocator);
			summary.AddMember(
				"average",
				static_cast<double>(total) / static_cast<double>(snapshot.series.size()),
				allocator);
			AddSize(summary, "max", maximum, allocator);
		}
		rapidjson::Value key;
		key.SetString(name, allocator);
		targets.AddMember(key, summary, allocator);
	}

	void BuildPerformancePayload(
		const VsmrPerformance::Snapshot& snapshot,
		std::size_t maximumSeriesPoints,
		rapidjson::Value& payload,
		Allocator& allocator)
	{
		payload.SetObject();
		payload.AddMember("schemaVersion", 1, allocator);
		payload.AddMember("available", Owner != nullptr, allocator);
		AddString(
			payload,
			"generatedAtUtc",
			FormatUtcMilliseconds(VsmrPerformance::PerformanceDiagnostics::UtcMilliseconds()),
			allocator);
		if (Owner == nullptr)
			return;
		if (LastPerformanceGeneration != snapshot.generation)
		{
			PeakProcessGdiObjects = 0;
			PeakVsmrCachedBitmaps = 0;
			PeakEstimatedBitmapBytes = 0;
			PeakAvisoPendingDepth = 0;
			LastPerformanceGeneration = snapshot.generation;
		}

		rapidjson::Value source(rapidjson::kObjectType);
		AddString(source, "airport", Owner->getActiveAirport(), allocator);
		AddString(source, "profile", Owner->GetActiveProfileNameForEditor(), allocator);
		AddString(source, "radarId", RadarIdentifier(), allocator);
		payload.AddMember("source", source, allocator);

		rapidjson::Value window(rapidjson::kObjectType);
		window.AddMember("seconds", snapshot.windowSeconds, allocator);
		AddSize(window, "samples", snapshot.frame.sampleCount, allocator);
		const std::uint64_t retainedFrames = (std::min)(
			snapshot.totalFrames,
			static_cast<std::uint64_t>(VsmrPerformance::MaximumFrameSamples));
		const std::uint64_t overwrittenFrames = snapshot.totalFrames - retainedFrames;
		AddUint64(window, "overwritten", overwrittenFrames, allocator);
		AddUint64(window, "dropped", overwrittenFrames, allocator);
		const std::uint64_t observedStart = snapshot.series.empty()
			? 0
			: snapshot.series.front().timestampMilliseconds;
		const std::uint64_t observedEnd = snapshot.series.empty()
			? 0
			: snapshot.series.back().timestampMilliseconds;
		window.AddMember(
			"observedSeconds",
			observedEnd >= observedStart && observedStart != 0
				? static_cast<double>(observedEnd - observedStart) / 1000.0
				: 0.0,
			allocator);
		AddString(
			window,
			"fromUtc",
			FormatUtcMilliseconds(MonotonicToUtc(snapshot, observedStart)),
			allocator);
		AddString(
			window,
			"toUtc",
			FormatUtcMilliseconds(MonotonicToUtc(snapshot, observedEnd)),
			allocator);
		payload.AddMember("window", window, allocator);

		rapidjson::Value timings(rapidjson::kObjectType);
		const bool hasLatest = snapshot.hasLatestFrame;
		AddDistribution(timings, "frame", snapshot.frame, snapshot.latestFrame.frameMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "scene", snapshot.scene, snapshot.latestFrame.sceneMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "aviso", snapshot.aviso, snapshot.latestFrame.avisoMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "targets", snapshot.targets, snapshot.latestFrame.targetsMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "rimcas", snapshot.rimcas, snapshot.latestFrame.rimcasMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "tags", snapshot.tags, snapshot.latestFrame.tagsMilliseconds, hasLatest, allocator);
		AddDistribution(timings, "srw", snapshot.srw, snapshot.latestFrame.srwMilliseconds, hasLatest, allocator);
		AddDistribution(
			timings,
			"avisoInset",
			snapshot.avisoInset,
			snapshot.latestFrame.avisoInsetMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(timings, "rdf", snapshot.rdf, snapshot.latestFrame.rdfMilliseconds, hasLatest, allocator);
		AddDistribution(
			timings,
			"insetChrome",
			snapshot.insetChrome,
			snapshot.latestFrame.insetChromeMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneAvisoLoad",
			snapshot.sceneAvisoLoad,
			snapshot.latestFrame.sceneAvisoLoadMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneControllerOwnership",
			snapshot.sceneControllerOwnership,
			snapshot.latestFrame.sceneControllerOwnershipMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneTargetCapture",
			snapshot.sceneTargetCapture,
			snapshot.latestFrame.sceneTargetCaptureMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"sceneFinalize",
			snapshot.sceneFinalize,
			snapshot.latestFrame.sceneFinalizeMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"euroScopeLookups",
			snapshot.euroScopeLookups,
			snapshot.latestFrame.euroScopeLookupMilliseconds,
			hasLatest,
			allocator);
		AddDistribution(
			timings,
			"avisoRasterRebuild",
			snapshot.avisoRasterRebuild,
			0.0,
			false,
			allocator);
		payload.AddMember("timings", timings, allocator);

		rapidjson::Value caches(rapidjson::kArrayType);
		AddCacheItem(
			caches,
			"aviso-main",
			"AVISO raster (main)",
			snapshot.mainAviso.exactHits,
			snapshot.mainAviso.previewHits,
			snapshot.mainAviso.misses,
			snapshot.resources.mainAvisoBitmapCount,
			allocator);
		AddCacheItem(
			caches,
			"aviso-insets",
			"AVISO raster (insets)",
			snapshot.insetAviso.exactHits,
			snapshot.insetAviso.previewHits,
			snapshot.insetAviso.misses,
			snapshot.resources.insetAvisoBitmapCount,
			allocator);
		AddCacheItem(
			caches,
			"aircraft-source",
			"Aircraft source bitmap",
			snapshot.aircraftSourceCache.hits,
			0,
			snapshot.aircraftSourceCache.misses,
			snapshot.aircraftSourceCache.entries,
			allocator);
		AddCacheItem(
			caches,
			"realistic-scaled",
			"Realistic icon scale",
			snapshot.realisticScaledCache.hits,
			0,
			snapshot.realisticScaledCache.misses,
			snapshot.realisticScaledCache.entries,
			allocator);
		AddCacheItem(
			caches,
			"realistic-rotated",
			"Realistic icon rotation",
			snapshot.realisticRotatedCache.hits,
			0,
			snapshot.realisticRotatedCache.misses,
			snapshot.realisticRotatedCache.entries,
			allocator);
		payload.AddMember("caches", caches, allocator);

		rapidjson::Value targets(rapidjson::kObjectType);
		AddTargetSummary(targets, "processed", snapshot, false, allocator);
		AddTargetSummary(targets, "visible", snapshot, true, allocator);
		payload.AddMember("targets", targets, allocator);

		rapidjson::Value refresh(rapidjson::kObjectType);
		if (snapshot.hasLatestFrame)
		{
			AddString(
				refresh,
				"latestReason",
				JoinPerformanceRefreshReasons(snapshot.latestFrame.refreshReasonMask),
				allocator);
			rapidjson::Value latestReasons(rapidjson::kArrayType);
			for (const std::string& label : PerformanceRefreshReasonLabels(snapshot.latestFrame.refreshReasonMask))
			{
				rapidjson::Value value;
				value.SetString(label.c_str(), static_cast<rapidjson::SizeType>(label.size()), allocator);
				latestReasons.PushBack(value, allocator);
			}
			refresh.AddMember("latestReasons", latestReasons, allocator);
		}
		rapidjson::Value reasonCounts(rapidjson::kArrayType);
		auto addRefreshReasonCount = [&](const char* id, const char* label, std::uint64_t count)
		{
			if (count == 0)
				return;
			rapidjson::Value item(rapidjson::kObjectType);
			AddString(item, "id", id, allocator);
			AddString(item, "reason", label, allocator);
			AddUint64(item, "count", count, allocator);
			reasonCounts.PushBack(item, allocator);
		};
		addRefreshReasonCount("unspecified", "Unspecified", snapshot.refresh.reasons.unspecified);
		addRefreshReasonCount("initial", "Initial frame", snapshot.refresh.reasons.initial);
		addRefreshReasonCount("mainViewChanged", "Main view changed", snapshot.refresh.reasons.mainViewChanged);
		addRefreshReasonCount("insetPanZoom", "Inset pan or zoom", snapshot.refresh.reasons.insetPanZoom);
		addRefreshReasonCount("insetMoveResize", "Inset move or resize", snapshot.refresh.reasons.insetMoveResize);
		addRefreshReasonCount("hover", "Hover", snapshot.refresh.reasons.hover);
		addRefreshReasonCount(
			"targetOrFlightPlanUpdate",
			"Target or flight-plan update",
			snapshot.refresh.reasons.targetOrFlightPlanUpdate);
		addRefreshReasonCount("controllerUpdate", "Controller update", snapshot.refresh.reasons.controllerUpdate);
		addRefreshReasonCount("profileUpdate", "Profile update", snapshot.refresh.reasons.profileUpdate);
		addRefreshReasonCount("airportUpdate", "Airport update", snapshot.refresh.reasons.airportUpdate);
		addRefreshReasonCount("avisoWorkerUpdate", "AVISO worker update", snapshot.refresh.reasons.avisoWorkerUpdate);
		addRefreshReasonCount("userActionExternal", "User or external action", snapshot.refresh.reasons.userActionExternal);
		addRefreshReasonCount("avisoDataChanged", "AVISO data changed", snapshot.refresh.reasons.avisoDataChanged);
		refresh.AddMember("reasonCounts", reasonCounts, allocator);
		AddString(refresh, "reasonScope", "selectedRetainedFrameWindow", allocator);
		refresh.AddMember("reasonCountsMayOverlap", true, allocator);
		refresh.AddMember("spikeThresholdMilliseconds", snapshot.refresh.spikeThresholdMilliseconds, allocator);
		AddString(refresh, "spikeComparison", ">=", allocator);
		AddUint64(refresh, "spikeCount", snapshot.refresh.spikeCount, allocator);
		auto addSpike = [&](const char* name, bool present, const VsmrPerformance::FrameSample& sample)
		{
			if (!present)
				return;
			rapidjson::Value spike(rapidjson::kObjectType);
			AddUint64(spike, "frameId", sample.frameId, allocator);
			spike.AddMember("frameMilliseconds", sample.frameMilliseconds, allocator);
			AddUint64(
				spike,
				"ageMilliseconds",
				observedEnd >= sample.timestampMilliseconds
					? observedEnd - sample.timestampMilliseconds
					: 0,
				allocator);
			AddString(spike, "reason", JoinPerformanceRefreshReasons(sample.refreshReasonMask), allocator);
			AddString(spike, "primaryReason", VsmrPerformance::PrimaryRefreshReasonName(sample.refreshReasonMask), allocator);
			AddSize(spike, "processedTargets", sample.processedTargets, allocator);
			AddSize(spike, "visibleTargets", sample.visibleTargets, allocator);
			spike.AddMember("avisoMilliseconds", sample.avisoMilliseconds, allocator);
			spike.AddMember("avisoInsetMilliseconds", sample.avisoInsetMilliseconds, allocator);

			const std::pair<const char*, double> stages[] = {
				{ "Scene capture", sample.sceneMilliseconds },
				{ "Main AVISO", sample.avisoMilliseconds },
				{ "AVISO inset", sample.avisoInsetMilliseconds },
				{ "Targets", sample.targetsMilliseconds },
				{ "RIMCAS", sample.rimcasMilliseconds },
				{ "Tags", sample.tagsMilliseconds },
				{ "SRW", sample.srwMilliseconds },
				{ "RDF", sample.rdfMilliseconds },
				{ "Inset chrome", sample.insetChromeMilliseconds }
			};
			const std::pair<const char*, double>* dominantStage = &stages[0];
			for (const auto& stage : stages)
			{
				if (stage.second > dominantStage->second)
					dominantStage = &stage;
			}
			std::ostringstream context;
			context.imbue(std::locale::classic());
			context << "Largest measured slice: " << dominantStage->first << " "
				<< std::fixed << std::setprecision(2) << dominantStage->second << " ms";
			AddString(spike, "context", context.str(), allocator);
			rapidjson::Value key;
			key.SetString(name, allocator);
			refresh.AddMember(key, spike, allocator);
		};
		addSpike("worstSpike", snapshot.refresh.hasWorstSpike, snapshot.refresh.worstSpike);
		addSpike("latestSpike", snapshot.refresh.hasLatestSpike, snapshot.refresh.latestSpike);
		payload.AddMember("refresh", refresh, allocator);

		PeakProcessGdiObjects = (std::max)(
			PeakProcessGdiObjects,
			snapshot.resources.processGdiObjects);
		PeakVsmrCachedBitmaps = (std::max)(
			PeakVsmrCachedBitmaps,
			snapshot.resources.ownedBitmapCount);
		PeakEstimatedBitmapBytes = (std::max)(
			PeakEstimatedBitmapBytes,
			snapshot.resources.estimatedBitmapBytes);
		rapidjson::Value graphics(rapidjson::kObjectType);
		graphics.AddMember("processGdiObjects", snapshot.resources.processGdiObjects, allocator);
		AddSize(graphics, "vsmrCachedBitmaps", snapshot.resources.ownedBitmapCount, allocator);
		graphics.AddMember("peakProcessGdiObjects", PeakProcessGdiObjects, allocator);
		AddSize(graphics, "peakVsmrCachedBitmaps", PeakVsmrCachedBitmaps, allocator);
		AddUint64(graphics, "estimatedBitmapBytes", snapshot.resources.estimatedBitmapBytes, allocator);
		AddUint64(graphics, "peakEstimatedBitmapBytes", PeakEstimatedBitmapBytes, allocator);
		AddSize(graphics, "aircraftBitmapCount", snapshot.resources.aircraftBitmapCount, allocator);
		AddSize(graphics, "realisticIconBitmapCount", snapshot.resources.realisticIconBitmapCount, allocator);
		AddSize(graphics, "mainAvisoBitmapCount", snapshot.resources.mainAvisoBitmapCount, allocator);
		AddSize(graphics, "insetAvisoBitmapCount", snapshot.resources.insetAvisoBitmapCount, allocator);
		payload.AddMember("graphics", graphics, allocator);

		const std::size_t avisoPendingDepth =
			snapshot.mainAviso.queue.pending + snapshot.insetAviso.queue.pending;
		const std::size_t avisoInFlight =
			snapshot.mainAviso.queue.inFlight + snapshot.insetAviso.queue.inFlight;
		const std::size_t avisoCompleted =
			snapshot.mainAviso.queue.completed + snapshot.insetAviso.queue.completed;
		PeakAvisoPendingDepth = (std::max)(PeakAvisoPendingDepth, avisoPendingDepth);
		CSMRPlugin* const plugin = DatalinkPlugin();
		const WorkerQueueSnapshot pluginQueues = plugin != nullptr
			? plugin->GetWorkerQueueSnapshot()
			: WorkerQueueSnapshot{};
		rapidjson::Value worker(rapidjson::kObjectType);
		worker.AddMember("active", avisoInFlight > 0, allocator);
		AddSize(worker, "pendingDepth", avisoPendingDepth, allocator);
		AddSize(worker, "inFlight", avisoInFlight, allocator);
		AddSize(worker, "maxDepth", PeakAvisoPendingDepth, allocator);
		AddUint64(
			worker,
			"supersededRequests",
			snapshot.mainAviso.requestsSuperseded + snapshot.insetAviso.requestsSuperseded,
			allocator);
		rapidjson::Value queues(rapidjson::kArrayType);
		auto addQueue = [&](const char* name, bool active, std::size_t pending, std::size_t inFlight, std::size_t workers, std::size_t completed)
		{
			rapidjson::Value queue(rapidjson::kObjectType);
			AddString(queue, "name", name, allocator);
			queue.AddMember("active", active, allocator);
			AddSize(queue, "pendingDepth", pending, allocator);
			AddSize(queue, "inFlight", inFlight, allocator);
			AddSize(queue, "workers", workers, allocator);
			AddSize(queue, "completedWaiting", completed, allocator);
			queues.PushBack(queue, allocator);
		};
		addQueue(
			"AVISO",
			avisoInFlight > 0,
			avisoPendingDepth,
			avisoInFlight,
			snapshot.mainAviso.queue.workers + snapshot.insetAviso.queue.workers,
			avisoCompleted);
		addQueue(
			"Network",
			pluginQueues.networkInFlight > 0,
			pluginQueues.networkQueued,
			pluginQueues.networkInFlight,
			pluginQueues.networkWorkers,
			0);
		addQueue(
			"Weather",
			pluginQueues.weatherInFlight > 0,
			pluginQueues.weatherQueued,
			pluginQueues.weatherInFlight,
			pluginQueues.weatherWorkerRunning ? 1U : 0U,
			0);
		worker.AddMember("queues", queues, allocator);
		payload.AddMember("worker", worker, allocator);

		rapidjson::Value aviso(rapidjson::kObjectType);
		AddUint64(
			aviso,
			"framesDelayed",
			snapshot.avisoDelayedFrames,
			allocator);
		AddUint64(
			aviso,
			"framesUsingFallback",
			snapshot.avisoFallbackFrames,
			allocator);
		const std::uint64_t totalBuilds =
			snapshot.mainAviso.rasterBuilds + snapshot.insetAviso.rasterBuilds;
		const std::uint64_t failedBuilds =
			snapshot.mainAviso.rasterBuildFailures + snapshot.insetAviso.rasterBuildFailures;
		AddUint64(
			aviso,
			"rebuildsCompleted",
			totalBuilds >= failedBuilds ? totalBuilds - failedBuilds : 0,
			allocator);
		AddUint64(
			aviso,
			"blankDelayedFrames",
			snapshot.avisoBlankDelayedFrames,
			allocator);
		AddUint64(
			aviso,
			"requestsQueued",
			snapshot.mainAviso.requestsQueued + snapshot.insetAviso.requestsQueued,
			allocator);
		AddUint64(
			aviso,
			"requestsCoalesced",
			snapshot.mainAviso.requestsCoalesced + snapshot.insetAviso.requestsCoalesced,
			allocator);
		AddUint64(
			aviso,
			"requestsSuperseded",
			snapshot.mainAviso.requestsSuperseded + snapshot.insetAviso.requestsSuperseded,
			allocator);
		AddUint64(
			aviso,
			"requestsDebounced",
			snapshot.mainAviso.requestsDebounced + snapshot.insetAviso.requestsDebounced,
			allocator);
		AddUint64(
			aviso,
			"rasterBuilds",
			totalBuilds,
			allocator);
		AddUint64(
			aviso,
			"rasterBuildFailures",
			failedBuilds,
			allocator);
		AddUint64(
			aviso,
			"rasterBuildsCancelled",
			snapshot.mainAviso.rasterBuildsCancelled + snapshot.insetAviso.rasterBuildsCancelled,
			allocator);
		AddUint64(
			aviso,
			"resultsApplied",
			snapshot.mainAviso.resultsApplied + snapshot.insetAviso.resultsApplied,
			allocator);
		AddUint64(
			aviso,
			"resultsDiscarded",
			snapshot.mainAviso.resultsDiscarded + snapshot.insetAviso.resultsDiscarded,
			allocator);
		auto addAvisoViewport = [&](const char* name, const VsmrPerformance::AvisoSnapshot& source)
		{
			rapidjson::Value viewport(rapidjson::kObjectType);
			AddUint64(viewport, "requestsQueued", source.requestsQueued, allocator);
			AddUint64(viewport, "requestsCoalesced", source.requestsCoalesced, allocator);
			AddUint64(viewport, "requestsSuperseded", source.requestsSuperseded, allocator);
			AddUint64(viewport, "requestsDebounced", source.requestsDebounced, allocator);
			AddUint64(viewport, "rasterBuilds", source.rasterBuilds, allocator);
			AddUint64(viewport, "rasterBuildFailures", source.rasterBuildFailures, allocator);
			AddUint64(viewport, "rasterBuildsCancelled", source.rasterBuildsCancelled, allocator);
			AddUint64(viewport, "resultsApplied", source.resultsApplied, allocator);
			AddUint64(viewport, "resultsDiscarded", source.resultsDiscarded, allocator);
			rapidjson::Value key;
			key.SetString(name, allocator);
			aviso.AddMember(key, viewport, allocator);
		};
		addAvisoViewport("main", snapshot.mainAviso);
		addAvisoViewport("inset", snapshot.insetAviso);
		payload.AddMember("aviso", aviso, allocator);

		rapidjson::Value series(rapidjson::kArrayType);
		const std::size_t availableSeries = snapshot.series.size();
		const std::size_t wantedSeries = (std::min)(availableSeries, maximumSeriesPoints);
		for (std::size_t index = 0; index < wantedSeries; ++index)
		{
			const std::size_t sourceIndex = wantedSeries <= 1
				? availableSeries - 1
				: (index * (availableSeries - 1)) / (wantedSeries - 1);
			const VsmrPerformance::FrameSample& sample = snapshot.series[sourceIndex];
			rapidjson::Value point(rapidjson::kObjectType);
			AddUint64(
				point,
				"offsetMs",
				sample.timestampMilliseconds >= observedStart
					? sample.timestampMilliseconds - observedStart
					: 0,
				allocator);
			point.AddMember("frameMs", sample.frameMilliseconds, allocator);
			point.AddMember("avisoMs", sample.avisoMilliseconds, allocator);
			point.AddMember("avisoInsetMs", sample.avisoInsetMilliseconds, allocator);
			point.AddMember("sceneMs", sample.sceneMilliseconds, allocator);
			point.AddMember("srwMs", sample.srwMilliseconds, allocator);
			point.AddMember("rdfMs", sample.rdfMilliseconds, allocator);
			point.AddMember("insetChromeMs", sample.insetChromeMilliseconds, allocator);
			point.AddMember("refreshReasonMask", sample.refreshReasonMask, allocator);
			series.PushBack(point, allocator);
		}
		payload.AddMember("series", series, allocator);
	}

	void SendPerformanceState(
		const std::string& requestId,
		std::uint32_t windowSeconds,
		std::size_t maximumSeriesPoints)
	{
		rapidjson::Document message;
		MakeEnvelope(message, "performance.state", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload(rapidjson::kObjectType);
		if (Owner == nullptr)
		{
			payload.AddMember("schemaVersion", 1, allocator);
			payload.AddMember("available", false, allocator);
		}
		else
		{
			const VsmrPerformance::Snapshot snapshot = Owner->GetPerformanceSnapshot(
				windowSeconds,
				VsmrPerformance::MaximumFrameSamples);
			BuildPerformancePayload(snapshot, maximumSeriesPoints, payload, allocator);
		}
		message.AddMember("payload", payload, allocator);
		Send(message);
	}

	bool AddWorkerQueuesToPerformanceReport(
		const std::string& nativeReport,
		std::string& report,
		std::string& error)
	{
		report.clear();
		error.clear();
		rapidjson::Document document;
		document.Parse<0>(nativeReport.c_str());
		if (document.HasParseError() || !document.IsObject())
		{
			error = "The native performance report could not be serialized.";
			return false;
		}

		Allocator& allocator = document.GetAllocator();
		if (!document.HasMember("type"))
			AddString(document, "type", "vSMR.performance-report", allocator);
		CSMRPlugin* const plugin = DatalinkPlugin();
		const WorkerQueueSnapshot queues = plugin != nullptr
			? plugin->GetWorkerQueueSnapshot()
			: WorkerQueueSnapshot{};
		rapidjson::Value workerQueues(rapidjson::kObjectType);
		rapidjson::Value network(rapidjson::kObjectType);
		AddSize(network, "workers", queues.networkWorkers, allocator);
		AddSize(network, "queued", queues.networkQueued, allocator);
		AddSize(network, "inFlight", queues.networkInFlight, allocator);
		workerQueues.AddMember("network", network, allocator);
		rapidjson::Value weather(rapidjson::kObjectType);
		weather.AddMember("workerRunning", queues.weatherWorkerRunning, allocator);
		AddSize(weather, "queued", queues.weatherQueued, allocator);
		AddSize(weather, "inFlight", queues.weatherInFlight, allocator);
		workerQueues.AddMember("weather", weather, allocator);
		if (document.HasMember("workerQueues"))
			document.RemoveMember("workerQueues");
		document.AddMember("workerQueues", workerQueues, allocator);
		report = SerializePretty(document);
		return !report.empty();
	}

	void SendPerformanceExportAck(
		const std::string& requestId,
		const std::string& path)
	{
		rapidjson::Document message;
		MakeEnvelope(message, "state.ack", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload(rapidjson::kObjectType);
		AddString(payload, "action", "performance.report.export", allocator);
		AddString(payload, "message", "Performance report exported", allocator);
		AddString(payload, "path", path, allocator);
		payload.AddMember("cancelled", false, allocator);
		message.AddMember("payload", payload, allocator);
		Send(message);
	}

	std::string ContentRevision(const std::string& contents)
	{
		std::uint64_t hash = 14695981039346656037ULL;
		for (const unsigned char byte : contents)
		{
			hash ^= static_cast<std::uint64_t>(byte);
			hash *= 1099511628211ULL;
		}
		std::ostringstream output;
		output << std::hex << std::setfill('0') << std::setw(16) << hash;
		return output.str();
	}

	std::string FileRevision(const std::string& path)
	{
		std::string contents;
		return ReadFileText(path, contents) ? ContentRevision(contents) : "missing";
	}

	void EvaluateAvisoHealth(
		const std::string& path,
		bool& healthy,
		std::string& message) const
	{
		std::error_code fileError;
		const std::filesystem::path filePath(path);
		const bool exists = !path.empty() &&
			std::filesystem::is_regular_file(filePath, fileError) &&
			!fileError;
		const std::uintmax_t size = exists
			? std::filesystem::file_size(filePath, fileError)
			: 0;
		const std::filesystem::file_time_type writeTime = exists && !fileError
			? std::filesystem::last_write_time(filePath, fileError)
			: std::filesystem::file_time_type{};
		const bool stampValid = !fileError;
		if (stampValid &&
			AvisoHealthCachePath == path &&
			AvisoHealthCacheExists == exists &&
			AvisoHealthCacheSize == size &&
			AvisoHealthCacheWriteTime == writeTime)
		{
			healthy = AvisoHealthCacheHealthy;
			message = AvisoHealthCacheMessage;
			return;
		}

		healthy = false;
		message = "The active airport AVISO source is missing or invalid; the previous overlay remains active when available.";
		std::string validatedDocumentJson;
		if (exists && stampValid && size <= kMaximumBridgeMessageBytes)
		{
			std::string avisoJson;
			rapidjson::Document parsed;
			if (ReadFileText(path, avisoJson) &&
				!parsed.Parse<0>(avisoJson.c_str()).HasParseError() &&
				parsed.IsObject() &&
				parsed.HasMember("type") &&
				parsed["type"].IsString() &&
				std::strcmp(parsed["type"].GetString(), "FeatureCollection") == 0)
			{
				bool schemaSupported = true;
				if (parsed.HasMember("metadata"))
				{
					const rapidjson::Value& metadata = parsed["metadata"];
					schemaSupported = metadata.IsObject();
					if (schemaSupported && metadata.HasMember("schema_version"))
					{
						const rapidjson::Value& schemaVersion = metadata["schema_version"];
						schemaSupported = schemaVersion.IsInt() &&
							schemaVersion.GetInt() >= 1 &&
							schemaVersion.GetInt() <= 2;
					}
				}
				if (schemaSupported)
				{
					AvisoDocumentModel validationModel;
					CloneJsonValue(
						parsed,
						validationModel.MutableDocument(),
						validationModel.MutableDocument().GetAllocator());
					validationModel.MarkIndexesDirty();
					const AvisoValidationResult validation =
						validationModel.ValidateAndRecalculate();
					healthy = validation.ok;
					if (healthy)
						validatedDocumentJson = SerializeCompact(
							validationModel.GetDocument());
					if (!healthy && !validation.errorText.empty())
						message = validation.errorText;
				}
			}
		}
		else if (exists && size > kMaximumBridgeMessageBytes)
		{
			message = "The active airport AVISO source exceeds the supported 32 MB limit.";
		}

		AvisoHealthCachePath = path;
		AvisoHealthCacheExists = exists;
		AvisoHealthCacheSize = size;
		AvisoHealthCacheWriteTime = writeTime;
		AvisoHealthCacheHealthy = healthy;
		AvisoHealthCacheMessage = healthy ? std::string() : message;
		AvisoHealthCacheDocumentJson = healthy
			? std::move(validatedDocumentJson)
			: std::string();
		if (healthy)
			message.clear();
	}

	CSMRPlugin* DatalinkPlugin() const
	{
		if (Owner == nullptr)
			return nullptr;
		return static_cast<CSMRPlugin*>(Owner->GetPlugIn());
	}

	void BuildDatalinkState(
		rapidjson::Value& datalink,
		Allocator& allocator) const
	{
		datalink.SetObject();
		CSMRPlugin* plugin = DatalinkPlugin();
		datalink.AddMember("available", plugin != nullptr, allocator);
		if (plugin == nullptr)
			return;

		const DatalinkControlState state = plugin->GetDatalinkControlState();
		datalink.AddMember("connected", state.connected, allocator);
		datalink.AddMember("connecting", state.connecting, allocator);
		datalink.AddMember("pollInProgress", state.pollInProgress, allocator);
		datalink.AddMember("controllerConnected", state.controllerConnected, allocator);
		AddString(datalink, "logonCallsign", state.logonCallsign, allocator);
		datalink.AddMember("hasPassword", state.hasPassword, allocator);
		datalink.AddMember("playSound", state.playSound, allocator);
		datalink.AddMember("cdmAutoEnabled", state.cdmAutoEnabled, allocator);
		datalink.AddMember("cdmDelayMinutes", state.cdmDelayMinutes, allocator);
		datalink.AddMember("cdmCooldownMinutes", state.cdmCooldownMinutes, allocator);
		datalink.AddMember("vacdmConfigured", state.vacdmConfigured, allocator);
		AddString(datalink, "activeAirport", state.activeAirport, allocator);
		AddString(datalink, "cdmAliasPath", state.cdmAliasPath, allocator);
		datalink.AddMember("cdmAliasReady", state.cdmAliasReady, allocator);
		AddString(datalink, "statusMessage", state.statusMessage, allocator);
	}

	void SendDatalinkState(
		const std::string& requestId = "",
		const std::string& messageText = "")
	{
		rapidjson::Document message;
		MakeEnvelope(message, "datalink.state", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload(rapidjson::kObjectType);
		rapidjson::Value datalink;
		BuildDatalinkState(datalink, allocator);
		payload.AddMember("datalink", datalink, allocator);
		if (!messageText.empty())
			AddString(payload, "message", messageText, allocator);
		message.AddMember("payload", payload, allocator);
		Send(message);
	}

	void BuildSettings(
		rapidjson::Value& settings,
		Allocator& allocator) const
	{
		settings.SetObject();
		if (Owner == nullptr)
			return;

		AddString(settings, "profileFile", Owner->ConfigPath, allocator);
		AddString(
			settings,
			"avisoFile",
			Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport()),
			allocator);
		settings.AddMember("watchFiles", true, allocator);
		AddString(settings, "bridgeMode", "Native WebView2", allocator);
		settings.AddMember("updateInterval", 250, allocator);
		AddString(
			settings,
			"resolutionPreset",
			Owner->GetSmallTargetIconBoostResolutionPreset(),
			allocator);
		settings.AddMember("showFps", Owner->ShowFps, allocator);
		settings.AddMember("runtimeSync", true, allocator);
		settings.AddMember("confirmDelete", true, allocator);

		rapidjson::Value dataHealth(rapidjson::kObjectType);
		const bool configHealthy =
			Owner->CurrentConfig != nullptr && Owner->CurrentConfig->isConfigHealthy();
		dataHealth.AddMember("profilesHealthy", configHealthy, allocator);
		dataHealth.AddMember(
			"profilesUsingBackup",
			Owner->CurrentConfig != nullptr && Owner->CurrentConfig->isUsingBackup(),
			allocator);
		dataHealth.AddMember(
			"profilesBackupAvailable",
			Owner->CurrentConfig != nullptr && Owner->CurrentConfig->isBackupAvailable(),
			allocator);
		AddString(
			dataHealth,
			"profilesMessage",
			Owner->CurrentConfig != nullptr
				? Owner->CurrentConfig->getLastLoadMessage()
				: "vSMR configuration is not available.",
			allocator);
		const std::string avisoPath =
			Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
		bool avisoHealthy = false;
		std::string avisoHealthMessage;
		EvaluateAvisoHealth(avisoPath, avisoHealthy, avisoHealthMessage);
		dataHealth.AddMember("avisoHealthy", avisoHealthy, allocator);
		AddString(
			dataHealth,
			"avisoMessage",
			avisoHealthy ? "" : avisoHealthMessage,
			allocator);
		settings.AddMember("dataHealth", dataHealth, allocator);

		bool rimcasEnabled = true;
		if (Owner->CurrentConfig != nullptr)
		{
			const rapidjson::Value& profile = Owner->CurrentConfig->getActiveProfile();
			if (profile.IsObject() &&
				profile.HasMember("rimcas") &&
				profile["rimcas"].IsObject())
				rimcasEnabled = ReadBool(profile["rimcas"], "enabled", true);
		}
		settings.AddMember("rimcas", rimcasEnabled, allocator);
		settings.AddMember(
			"vacdm",
			Owner->CurrentConfig != nullptr &&
				!Owner->CurrentConfig->getVacdmServerUrl().empty(),
			allocator);
		rapidjson::Value capabilities(rapidjson::kObjectType);
		capabilities.AddMember("nativeBridge", true, allocator);
		capabilities.AddMember("atomicSave", true, allocator);
		capabilities.AddMember("githubLoad", true, allocator);
		capabilities.AddMember("groups", true, allocator);
		capabilities.AddMember("datalink", true, allocator);
		capabilities.AddMember("maps", false, allocator);
		settings.AddMember("capabilities", capabilities, allocator);
	}

	void BuildRuntimeState(
		rapidjson::Value& runtime,
		Allocator& allocator) const
	{
		runtime.SetObject();
		if (Owner == nullptr)
			return;

		rapidjson::Value insets(rapidjson::kObjectType);
		auto insetVisible = [&](int id) -> bool
		{
			const auto found = Owner->appWindowDisplays.find(id);
			return found != Owner->appWindowDisplays.end() && found->second;
		};
		insets.AddMember("aviso", insetVisible(3), allocator);
		insets.AddMember("srw1", insetVisible(1), allocator);
		insets.AddMember("weather", insetVisible(APPWINDOW_WEATHER - APPWINDOW_BASE), allocator);
		insets.AddMember("timer", insetVisible(APPWINDOW_TIMER - APPWINDOW_BASE), allocator);
		runtime.AddMember("insets", insets, allocator);
		runtime.AddMember("avisoInsetVisible", insetVisible(3), allocator);
		AddString(
			runtime,
			"activeAvisoPreset",
			Owner->GetActiveAvisoPresetName(),
			allocator);
		rapidjson::Value groups(rapidjson::kArrayType);
		for (const CSMRRadar::AvisoGroup& group : Owner->GetAvisoGroups())
		{
			rapidjson::Value item(rapidjson::kObjectType);
			AddString(item, "id", group.id, allocator);
			AddString(item, "name", group.name, allocator);
			item.AddMember("visible", group.visible, allocator);
			groups.PushBack(item, allocator);
		}
		runtime.AddMember("groups", groups, allocator);

		rapidjson::Value alerts(rapidjson::kObjectType);
		AddString(alerts, "visibility", Owner->isLVP ? "lvp" : "normal", allocator);
		rapidjson::Value runways(rapidjson::kArrayType);
		if (Owner->RimcasInstance != nullptr)
		{
			// Geometry can be temporarily invalidated while the active airport is
			// changing. Build the runtime list from the union of geometry and all
			// configured monitoring maps so the Control Center never receives a
			// false empty runway list during that transition.
			std::set<std::string> runwayNames;
			for (const auto& entry : Owner->RimcasInstance->RunwayAreas)
				runwayNames.insert(entry.first);
			for (const auto& entry : Owner->RimcasInstance->MonitoredRunwayArr)
				runwayNames.insert(entry.first);
			for (const auto& entry : Owner->RimcasInstance->MonitoredRunwayDep)
				runwayNames.insert(entry.first);
			for (const auto& entry : Owner->RimcasInstance->ClosedRunway)
				runwayNames.insert(entry.first);

			for (const std::string& runway : runwayNames)
			{
				rapidjson::Value item(rapidjson::kObjectType);
				AddString(item, "id", runway, allocator);
				const auto arr = Owner->RimcasInstance->MonitoredRunwayArr.find(runway);
				const auto dep = Owner->RimcasInstance->MonitoredRunwayDep.find(runway);
				const auto closed = Owner->RimcasInstance->ClosedRunway.find(runway);
				item.AddMember(
					"arrival",
					arr != Owner->RimcasInstance->MonitoredRunwayArr.end() && arr->second,
					allocator);
				item.AddMember(
					"departure",
					dep != Owner->RimcasInstance->MonitoredRunwayDep.end() && dep->second,
					allocator);
				item.AddMember(
					"closed",
					closed != Owner->RimcasInstance->ClosedRunway.end() && closed->second,
					allocator);
				runways.PushBack(item, allocator);
			}
		}
		alerts.AddMember("runways", runways, allocator);
		runtime.AddMember("alerts", alerts, allocator);
	}

	void SendAvisoState(const std::string& requestId)
	{
		if (Owner == nullptr)
			return;

		const std::string path =
			Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
		rapidjson::Document aviso;
		bool healthy = false;
		std::string validationError;
		EvaluateAvisoHealth(path, healthy, validationError);
		const bool valid = healthy &&
			!AvisoHealthCacheDocumentJson.empty() &&
			!aviso.Parse<0>(AvisoHealthCacheDocumentJson.c_str()).HasParseError() &&
			aviso.IsObject();
		if (!valid)
		{
			aviso.SetObject();
			Allocator& allocator = aviso.GetAllocator();
			AddString(aviso, "type", "FeatureCollection", allocator);
			rapidjson::Value features(rapidjson::kArrayType);
			aviso.AddMember("features", features, allocator);
			if (!validationError.empty())
			{
				Logger::info(
					"Control Center withheld invalid AVISO GeoJSON: " +
					validationError);
			}
		}
		const std::vector<CSMRRadar::AvisoGroup> runtimeGroups = Owner->GetAvisoGroups();
		if (!runtimeGroups.empty())
		{
			const rapidjson::Value* persistedGroups =
				aviso.HasMember("vsmr_groups") && aviso["vsmr_groups"].IsArray()
				? &aviso["vsmr_groups"]
				: nullptr;
			rapidjson::Value groups(rapidjson::kArrayType);
			for (const CSMRRadar::AvisoGroup& group : runtimeGroups)
			{
				rapidjson::Value item(rapidjson::kObjectType);
				if (persistedGroups != nullptr)
				{
					for (rapidjson::SizeType index = 0; index < persistedGroups->Size(); ++index)
					{
						const rapidjson::Value& candidate = (*persistedGroups)[index];
						if (!candidate.IsObject())
							continue;
						std::string candidateId = ReadString(candidate, "id");
						if (candidateId.empty())
							candidateId = ReadString(candidate, "group_id");
						if (candidateId == group.id)
						{
							CloneJsonValue(candidate, item, aviso.GetAllocator());
							break;
						}
					}
				}
				if (!item.HasMember("id") && !item.HasMember("group_id"))
					AddString(item, "id", group.id, aviso.GetAllocator());
				SetStringMember(item, "name", group.name, aviso.GetAllocator());
				SetBoolMember(item, "visible", group.visible, aviso.GetAllocator());
				groups.PushBack(item, aviso.GetAllocator());
			}
			CopyOrReplaceMember(aviso, "vsmr_groups", groups, aviso.GetAllocator());
		}

		rapidjson::Document message;
		MakeEnvelope(message, "state.aviso", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload;
		CloneJsonValue(aviso, payload, allocator);
		message.AddMember("payload", payload, allocator);
		Send(message);
	}

	void SendAuthoritativeState(
		const std::string& reason,
		const std::string& requestId = "",
		bool includeAviso = true)
	{
		if (Owner == nullptr || Owner->CurrentConfig == nullptr)
		{
			SendError(requestId, "vSMR configuration is not available.");
			return;
		}
		if (includeAviso)
		{
			const std::string avisoPath =
				Owner->ResolveAvisoGeoJsonPathForAirport(Owner->getActiveAirport());
			if (!avisoPath.empty())
				Owner->EnsureAvisoGeoJsonLoaded(avisoPath);
		}

		rapidjson::Document message;
		MakeEnvelope(message, "state.authoritative", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload(rapidjson::kObjectType);
		rapidjson::Value profiles;
		CloneJsonValue(Owner->CurrentConfig->document, profiles, allocator);
		payload.AddMember("profiles", profiles, allocator);
		rapidjson::Value settings;
		BuildSettings(settings, allocator);
		payload.AddMember("settings", settings, allocator);
		rapidjson::Value runtime;
		BuildRuntimeState(runtime, allocator);
		payload.AddMember("runtime", runtime, allocator);
		rapidjson::Value datalink;
		BuildDatalinkState(datalink, allocator);
		payload.AddMember("datalink", datalink, allocator);
		AddString(
			payload,
			"activeProfile",
			Owner->GetActiveProfileNameForEditor(),
			allocator);
		AddString(payload, "airport", Owner->getActiveAirport(), allocator);
		AddString(
			payload,
			"configRevision",
			Owner->CurrentConfig->getConfigRevision(),
			allocator);
		AddString(
			payload,
			"avisoRevision",
			FileRevision(Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport())),
			allocator);
		payload.AddMember("avisoFollows", includeAviso, allocator);
		AddString(payload, "reason", reason, allocator);
		message.AddMember("payload", payload, allocator);
		Send(message);
		if (includeAviso)
			SendAvisoState(requestId);
	}

	void SendStagedAuthoritativeState(
		const rapidjson::Value& stagedState,
		const std::string& reason,
		const std::string& requestId)
	{
		if (Owner == nullptr || Owner->CurrentConfig == nullptr)
		{
			SendError(requestId, "vSMR configuration is not available.");
			return;
		}

		rapidjson::Document message;
		MakeEnvelope(message, "state.authoritative", requestId);
		Allocator& allocator = message.GetAllocator();
		rapidjson::Value payload(rapidjson::kObjectType);

		rapidjson::Value profiles;
		CloneJsonValue(Owner->CurrentConfig->document, profiles, allocator);
		payload.AddMember("profiles", profiles, allocator);

		if (stagedState.IsObject() &&
			stagedState.HasMember("aviso") &&
			stagedState["aviso"].IsObject())
		{
			rapidjson::Value aviso;
			CloneJsonValue(stagedState["aviso"], aviso, allocator);
			payload.AddMember("aviso", aviso, allocator);
		}

		rapidjson::Value settings;
		if (stagedState.IsObject() &&
			stagedState.HasMember("settings") &&
			stagedState["settings"].IsObject())
		{
			CloneJsonValue(stagedState["settings"], settings, allocator);
		}
		else
		{
			BuildSettings(settings, allocator);
		}
		payload.AddMember("settings", settings, allocator);

		rapidjson::Value runtime;
		BuildRuntimeState(runtime, allocator);
		payload.AddMember("runtime", runtime, allocator);
		rapidjson::Value datalink;
		BuildDatalinkState(datalink, allocator);
		payload.AddMember("datalink", datalink, allocator);
		AddString(
			payload,
			"activeProfile",
			Owner->GetActiveProfileNameForEditor(),
			allocator);
		const std::string stagedAirport = ReadString(stagedState, "airport");
		AddString(
			payload,
			"airport",
			stagedAirport.empty() ? Owner->getActiveAirport() : stagedAirport,
			allocator);
		// Staged editor content must never be paired with a revision observed
		// after that content was captured. Echo only the caller's exact tokens;
		// omitting absent tokens keeps unrelated group-only updates from blessing
		// stale editor data with a newer disk revision.
		const std::string stagedConfigRevision =
			TrimAscii(ReadString(stagedState, "configRevision"));
		const std::string stagedAvisoRevision =
			TrimAscii(ReadString(stagedState, "avisoRevision"));
		if (!stagedConfigRevision.empty())
			AddString(
				payload,
				"configRevision",
				stagedConfigRevision,
				allocator);
		if (!stagedAvisoRevision.empty())
			AddString(
				payload,
				"avisoRevision",
				stagedAvisoRevision,
				allocator);
		AddString(payload, "reason", reason, allocator);
		message.AddMember("payload", payload, allocator);
		Send(message);
	}

	bool ApplyHistoryState(
		const rapidjson::Value* payload,
		const std::string& reason,
		const std::string& requestId,
		std::string& error)
	{
		if (Owner == nullptr || Owner->CurrentConfig == nullptr)
		{
			error = "vSMR configuration is not available.";
			return false;
		}
		if (payload == nullptr ||
			!payload->IsObject() ||
			!payload->HasMember("state") ||
			!(*payload)["state"].IsObject())
		{
			error = "Undo/redo payload is missing staged state.";
			return false;
		}
		std::lock_guard<std::mutex> transactionLock(
			gBridgeSaveTransactionMutex);

		const rapidjson::Value& stagedState = (*payload)["state"];
		const std::string stagedAirport = TrimAscii(ReadString(stagedState, "airport"));
		const std::string expectedConfigRevision =
			TrimAscii(ReadString(stagedState, "configRevision"));
		const std::string persistedConfigRevision =
			Owner->CurrentConfig->getPersistedConfigRevision();
		if (expectedConfigRevision.empty() ||
			expectedConfigRevision != persistedConfigRevision)
		{
			error = "The profiles file changed after this history entry was created. Reload the Control Center before continuing.";
			return false;
		}
		if (stagedState.HasMember("aviso") &&
			(stagedAirport.empty() ||
				!EqualsNoCase(stagedAirport, TrimAscii(Owner->getActiveAirport()))))
		{
			error = "The active airport changed while these edits were staged. Reload the Control Center before continuing.";
			return false;
		}
		if (stagedState.HasMember("aviso"))
		{
			const std::string expectedAvisoRevision =
				TrimAscii(ReadString(stagedState, "avisoRevision"));
			const std::string currentAvisoRevision = FileRevision(
				Owner->GetAvisoGeoJsonEditorPathForAirport(
					Owner->getActiveAirport()));
			if (expectedAvisoRevision.empty() ||
				expectedAvisoRevision != currentAvisoRevision)
			{
				error = "The AVISO file changed after this history entry was created. Reload the Control Center before continuing.";
				return false;
			}
		}
		if (!stagedState.HasMember("profiles"))
		{
			error = "Undo/redo state is missing profiles.";
			return false;
		}
		const rapidjson::Value& profiles = stagedState["profiles"];
		if (!ValidateProfileArray(profiles, error))
			return false;

		// Prepare every fallible AVISO operation before changing the live profile,
		// settings, or inset state. Undo/redo must be one logical transaction: a
		// malformed group near the end of the payload cannot leave the earlier
		// profile and UI changes applied.
		const rapidjson::Value* stagedAviso = nullptr;
		std::vector<CSMRRadar::AvisoGroup> stagedGroups;
		bool hasStagedGroups = false;
		if (stagedState.HasMember("aviso"))
		{
			stagedAviso = &stagedState["aviso"];
			if (!stagedAviso->IsObject() ||
				!stagedAviso->HasMember("features") ||
				!(*stagedAviso)["features"].IsArray())
			{
				error = "Undo/redo AVISO state must be a GeoJSON FeatureCollection.";
				return false;
			}

			if (stagedAviso->HasMember("vsmr_groups"))
			{
				if (!(*stagedAviso)["vsmr_groups"].IsArray())
				{
					error = "Undo/redo AVISO groups must be an array.";
					return false;
				}

				hasStagedGroups = true;
				const rapidjson::Value& groupValues = (*stagedAviso)["vsmr_groups"];
				std::unordered_set<std::string> seenIds;
				std::unordered_map<std::string, bool> existingVisibility;
				for (const CSMRRadar::AvisoGroup& existing : Owner->GetAvisoGroups())
					existingVisibility[existing.id] = existing.visible;

				stagedGroups.reserve(groupValues.Size());
				for (rapidjson::SizeType index = 0; index < groupValues.Size(); ++index)
				{
					const rapidjson::Value& item = groupValues[index];
					if (!item.IsObject())
					{
						error = "Each undo/redo AVISO group must be an object.";
						return false;
					}

					CSMRRadar::AvisoGroup group;
					group.id = ReadString(item, "id");
					if (group.id.empty())
						group.id = ReadString(item, "group_id");
					if (group.id.empty())
					{
						error = "Each undo/redo AVISO group requires an id.";
						return false;
					}
					if (!seenIds.insert(group.id).second)
					{
						error = "Undo/redo AVISO group ids must be unique.";
						return false;
					}

					group.name = ReadString(item, "name");
					if (group.name.empty())
						group.name = group.id;
					const auto existing = existingVisibility.find(group.id);
					group.visible = existing != existingVisibility.end()
						? existing->second
						: true;
					if (item.HasMember("visible"))
					{
						if (!item["visible"].IsBool())
						{
							error = "Undo/redo AVISO group visible values must be boolean.";
							return false;
						}
						group.visible = item["visible"].GetBool();
					}
					stagedGroups.push_back(std::move(group));
				}
			}
		}

		std::string activeProfile = ReadString(stagedState, "activeProfile");
		if (activeProfile.empty())
			activeProfile = Owner->GetActiveProfileNameForEditor();

		rapidjson::Document previousProfiles;
		CloneJsonValue(
			Owner->CurrentConfig->document,
			previousProfiles,
			previousProfiles.GetAllocator());
		const std::string previousActiveProfile = Owner->GetActiveProfileNameForEditor();
		const std::array<std::pair<int, bool>, 4> previousInsetVisibility = {{
			{ 3, Owner->appWindowDisplays[3] },
			{ 1, Owner->appWindowDisplays[1] },
			{ APPWINDOW_WEATHER - APPWINDOW_BASE,
				Owner->appWindowDisplays[APPWINDOW_WEATHER - APPWINDOW_BASE] },
			{ APPWINDOW_TIMER - APPWINDOW_BASE,
				Owner->appWindowDisplays[APPWINDOW_TIMER - APPWINDOW_BASE] }
		}};
		auto rollbackLiveState = [&]()
		{
			std::string ignored;
			if (Owner->CurrentConfig->replaceInMemoryConfig(
				previousProfiles,
				previousActiveProfile,
				ignored))
			{
				if (Owner->RimcasInstance != nullptr)
					Owner->RimcasInstance->setInactiveAlerts(
						Owner->CurrentConfig->getInactiveAlert());
				Owner->LoadProfile(previousActiveProfile, false, false);
				// LoadProfile records mutable session state; put the exact history
				// document back after the renderer has consumed it.
				Owner->CurrentConfig->replaceInMemoryConfig(
					previousProfiles,
					previousActiveProfile,
					ignored);
			}
			for (const auto& visibility : previousInsetVisibility)
				Owner->appWindowDisplays[visibility.first] = visibility.second;
			Owner->CancelInsetWindowInteractions();
			Owner->RequestRefresh();
		};

		if (!Owner->CurrentConfig->replaceInMemoryConfig(
			profiles,
			activeProfile,
			error))
			return false;

		// LoadProfile normally records the old RIMCAS selection first. Seed it
		// with the staged selection so applying history cannot overwrite the
		// profile that is being restored.
		if (Owner->RimcasInstance != nullptr)
			Owner->RimcasInstance->setInactiveAlerts(
				Owner->CurrentConfig->getInactiveAlert());
		Owner->LoadProfile(activeProfile, false, false);

		// LoadProfile's session bookkeeping is intentionally mutable. Restore
		// the exact editor document after the live renderer has consumed it.
		if (!Owner->CurrentConfig->replaceInMemoryConfig(
			profiles,
			activeProfile,
			error))
		{
			rollbackLiveState();
			return false;
		}

		if (stagedState.HasMember("settings") &&
			stagedState["settings"].IsObject())
		{
			const std::string resolution =
				ReadString(stagedState["settings"], "resolutionPreset");
			if (!resolution.empty() &&
				!Owner->SetSmallTargetIconBoostResolutionPreset(
					resolution,
					false))
			{
				error = "The staged resolution preset is invalid.";
				rollbackLiveState();
				return false;
			}
			// ShowFps remains staged in the editor response below. Unlike profile
			// data, it is ASR state and is applied only by a successful SaveAll.
		}

		if (stagedAviso != nullptr)
		{
			const std::string avisoPath =
				Owner->ResolveAvisoGeoJsonPathForAirport(Owner->getActiveAirport());
			if (!avisoPath.empty() &&
				!Owner->EnsureAvisoGeoJsonLoaded(avisoPath, false))
			{
				error = "Unable to load the active AVISO source for undo/redo.";
				rollbackLiveState();
				return false;
			}
			if (!Owner->ApplyAvisoGroupMembershipSnapshot(*stagedAviso, &error))
			{
				if (error.empty())
					error = "Unable to apply undo/redo AVISO group membership.";
				rollbackLiveState();
				return false;
			}
			// UpdateAvisoGroups normalizes an already validated vector and cannot
			// fail; keep all remaining UI mutations after this final fallible step.
			if (hasStagedGroups)
				Owner->UpdateAvisoGroups(stagedGroups);
		}

		if (stagedState.HasMember("runtime") &&
			stagedState["runtime"].IsObject() &&
			stagedState["runtime"].HasMember("insets") &&
			stagedState["runtime"]["insets"].IsObject())
		{
			const rapidjson::Value& insets = stagedState["runtime"]["insets"];
			Owner->CancelInsetWindowInteractions();
			const auto applyInsetVisibility = [&](int id, const char* key)
			{
				if (!insets.HasMember(key) || !insets[key].IsBool())
					return;
				const bool visible = ReadBool(insets, key, false);
				Owner->appWindowDisplays[id] = visible;
				if (!visible)
				{
					auto windowIt = Owner->appWindows.find(id);
					if (windowIt != Owner->appWindows.end() && windowIt->second != nullptr)
						windowIt->second->ResetAvisoInteractionState();
				}
			};
			applyInsetVisibility(3, "aviso");
			applyInsetVisibility(1, "srw1");
			applyInsetVisibility(APPWINDOW_WEATHER - APPWINDOW_BASE, "weather");
			applyInsetVisibility(APPWINDOW_TIMER - APPWINDOW_BASE, "timer");
		}

		Owner->InvalidateStructuredTagRuleCache();
		Owner->RequestRefresh();
		SendStagedAuthoritativeState(
			stagedState,
			reason,
			requestId);
		return true;
	}

	bool SaveAll(
		const rapidjson::Value* payload,
		const std::string& /*requestId*/,
		std::string& error)
	{
		error.clear();
		if (Owner == nullptr || Owner->CurrentConfig == nullptr)
		{
			error = "vSMR configuration is not available.";
			return false;
		}
		if (payload == nullptr || !payload->IsObject())
		{
			error = "Save payload must be an object.";
			return false;
		}
		if (!payload->HasMember("profiles"))
		{
			error = "Save payload is missing profiles.";
			return false;
		}

		bool hasStagedShowFps = false;
		bool stagedShowFps = Owner->ShowFps;
		if (payload->HasMember("settings"))
		{
			const rapidjson::Value& settings = (*payload)["settings"];
			if (!settings.IsObject())
			{
				error = "Save settings must be an object.";
				return false;
			}
			if (settings.HasMember("showFps"))
			{
				if (!settings["showFps"].IsBool())
				{
					error = "Show FPS must be a boolean setting.";
					return false;
				}
				hasStagedShowFps = true;
				stagedShowFps = settings["showFps"].GetBool();
			}
		}
		// Revision checks, both file writes, and rollback form one process-wide
		// transaction.  Without this lock two Control Centers can both pass the
		// AVISO revision check and then corrupt each other's backup/rollback.
		std::lock_guard<std::mutex> transactionLock(gBridgeSaveTransactionMutex);
		const std::string stagedAirport = TrimAscii(ReadString(*payload, "airport"));
		if (payload->HasMember("aviso") &&
			(stagedAirport.empty() ||
				!EqualsNoCase(stagedAirport, TrimAscii(Owner->getActiveAirport()))))
		{
			error = "The active airport changed while these edits were staged. Reload the Control Center before saving.";
			return false;
		}

		const rapidjson::Value& incomingProfiles = (*payload)["profiles"];
		if (!ValidateProfileArray(incomingProfiles, error))
			return false;
		const std::string expectedConfigRevision =
			TrimAscii(ReadString(*payload, "configRevision"));
		const std::string expectedAvisoRevision =
			TrimAscii(ReadString(*payload, "avisoRevision"));
		if (expectedConfigRevision.empty())
		{
			error = "The Control Center has not received an authoritative profiles revision. Reload before saving.";
			return false;
		}
		if (payload->HasMember("aviso") && expectedAvisoRevision.empty())
		{
			error = "The Control Center has not received an authoritative AVISO revision. Reload before saving.";
			return false;
		}
		const bool recoveryConfirmed =
			ReadBool(*payload, "recoveryConfirmed", false);
		const bool avisoRecoveryConfirmed =
			ReadBool(*payload, "avisoRecoveryConfirmed", false);

		std::vector<CConfig::ProfileSaveIdentity> profileIdentities;
		if (payload->HasMember("profileIdentities"))
		{
			const rapidjson::Value& identities = (*payload)["profileIdentities"];
			if (!identities.IsArray())
			{
				error = "Profile identity state must be an array.";
				return false;
			}

			std::set<std::string> seenCurrentNames;
			for (rapidjson::SizeType index = 0; index < identities.Size(); ++index)
			{
				const rapidjson::Value& identity = identities[index];
				if (!identity.IsObject())
				{
					error = "Each profile identity must be an object.";
					return false;
				}
				const std::string currentName = TrimAscii(ReadString(identity, "currentName"));
				const std::string persistedName = TrimAscii(ReadString(identity, "persistedName"));
				if (currentName.empty())
				{
					error = "Each profile identity must name its current profile.";
					return false;
				}

				bool currentProfileExists = false;
				for (rapidjson::SizeType profileIndex = 0; profileIndex < incomingProfiles.Size(); ++profileIndex)
				{
					const rapidjson::Value& profile = incomingProfiles[profileIndex];
					if (IsProfileEntry(profile) &&
						EqualsNoCase(TrimAscii(profile["name"].GetString()), currentName))
					{
						currentProfileExists = true;
						break;
					}
				}
				if (!currentProfileExists ||
					!seenCurrentNames.insert(LowerAscii(currentName)).second)
				{
					error = "Profile identity state does not match the profiles being saved.";
					return false;
				}

				profileIdentities.push_back({ currentName, persistedName });
			}

			size_t incomingProfileCount = 0;
			for (rapidjson::SizeType index = 0; index < incomingProfiles.Size(); ++index)
				incomingProfileCount += IsProfileEntry(incomingProfiles[index]) ? 1u : 0u;
			if (profileIdentities.size() != incomingProfileCount)
			{
				error = "Profile identity state must contain exactly one entry per profile.";
				return false;
			}
		}

		rapidjson::Document mergedProfiles;
		MergeProfileArrayPreservingTopLevelUnknowns(
			Owner->CurrentConfig->document,
			incomingProfiles,
			mergedProfiles);
		if (!ValidateProfileArray(mergedProfiles, error))
			return false;

		std::unique_ptr<AvisoDocumentModel> avisoModel;
		std::string avisoPath;
		bool avisoSaveIsRecovery = false;
		if (payload->HasMember("aviso"))
		{
			const rapidjson::Value& incomingAviso = (*payload)["aviso"];
			if (!incomingAviso.IsObject() ||
				!incomingAviso.HasMember("features") ||
				!incomingAviso["features"].IsArray())
			{
				error = "AVISO state must be a GeoJSON FeatureCollection.";
				return false;
			}

			avisoPath =
				Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport());
			if (!expectedAvisoRevision.empty() &&
				expectedAvisoRevision != FileRevision(avisoPath))
			{
				error =
					"The active AVISO file changed in another vSMR window. Reload before saving so those changes are not overwritten.";
				return false;
			}
			avisoModel = std::make_unique<AvisoDocumentModel>();
			std::string loadError;
			if (!avisoModel->LoadFromFile(avisoPath, loadError))
			{
				if (!avisoRecoveryConfirmed)
				{
					error = loadError.empty() ? "Unable to load current AVISO data." : loadError;
					return false;
				}
				CloneJsonValue(
					incomingAviso,
					avisoModel->MutableDocument(),
					avisoModel->MutableDocument().GetAllocator());
				avisoSaveIsRecovery = true;
			}
			else
			{
				MergeAvisoPreservingCoordinates(
					avisoModel->MutableDocument(),
					incomingAviso);
			}
			avisoModel->MarkIndexesDirty();
			if (!avisoModel->ValidateLoadedFeatureCollection(error))
				return false;
		}

		rapidjson::Document previousProfiles;
		CloneJsonValue(
			Owner->CurrentConfig->document,
			previousProfiles,
			previousProfiles.GetAllocator());
		const std::string activeProfileBefore = Owner->GetActiveProfileNameForEditor();

		bool avisoExistedBeforeSave = false;
		std::string avisoRollbackSnapshotPath;
		bool avisoBackupExistedBeforeSave = false;
		std::string avisoBackupRollbackSnapshotPath;
		if (avisoModel != nullptr)
		{
			const DWORD attributes = ::GetFileAttributesA(avisoPath.c_str());
			avisoExistedBeforeSave =
				attributes != INVALID_FILE_ATTRIBUTES &&
				(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
			if (avisoExistedBeforeSave &&
				!CreateRollbackSnapshot(avisoPath, avisoRollbackSnapshotPath))
			{
				error =
					"Unable to create an exact AVISO rollback snapshot; no files were changed.";
				return false;
			}

			// A normal AVISO save rotates the previous primary into .bak. Snapshot
			// the pre-transaction backup too, so a later profiles failure restores
			// the exact two-file recovery state rather than losing an older version.
			if (!avisoSaveIsRecovery)
			{
				const std::string backupPath = avisoPath + ".bak";
				const DWORD backupAttributes = ::GetFileAttributesA(backupPath.c_str());
				avisoBackupExistedBeforeSave =
					backupAttributes != INVALID_FILE_ATTRIBUTES &&
					(backupAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
				if (avisoBackupExistedBeforeSave &&
					!CreateRollbackSnapshot(
						backupPath,
						avisoBackupRollbackSnapshotPath))
				{
					DeleteRollbackSnapshot(avisoRollbackSnapshotPath);
					error =
						"Unable to snapshot the existing AVISO backup; no files were changed.";
					return false;
				}
			}
		}

		CloneJsonValue(
			mergedProfiles,
			Owner->CurrentConfig->document,
			Owner->CurrentConfig->document.GetAllocator());

		if (avisoModel != nullptr)
		{
			if (!avisoModel->SaveAtomically(
				avisoPath,
				error,
				!avisoSaveIsRecovery))
			{
				CloneJsonValue(
					previousProfiles,
					Owner->CurrentConfig->document,
					Owner->CurrentConfig->document.GetAllocator());
				bool primaryRestored = true;
				if (avisoExistedBeforeSave)
				{
					primaryRestored = RestoreRollbackSnapshotAtomically(
						avisoRollbackSnapshotPath,
						avisoPath);
					if (primaryRestored)
						avisoRollbackSnapshotPath.clear();
				}
				else
				{
					primaryRestored =
						::DeleteFileA(avisoPath.c_str()) != FALSE ||
						::GetLastError() == ERROR_FILE_NOT_FOUND;
				}

				bool backupRestored = true;
				if (!avisoSaveIsRecovery)
				{
					const std::string backupPath = avisoPath + ".bak";
					if (avisoBackupExistedBeforeSave)
					{
						backupRestored = RestoreRollbackSnapshotAtomically(
							avisoBackupRollbackSnapshotPath,
							backupPath);
						if (backupRestored)
							avisoBackupRollbackSnapshotPath.clear();
					}
					else
					{
						backupRestored =
							::DeleteFileA(backupPath.c_str()) != FALSE ||
							::GetLastError() == ERROR_FILE_NOT_FOUND;
					}
				}
				if (error.empty())
					error = "Unable to save AVISO GeoJSON atomically.";
				if (!primaryRestored)
					error += " The exact old primary remains at " +
						avisoRollbackSnapshotPath + ".";
				if (!backupRestored)
					error += " The exact old backup remains at " +
						avisoBackupRollbackSnapshotPath + ".";
				return false;
			}
		}

		std::string profileSaveError;
		if (!Owner->CurrentConfig->saveConfig(
			profileIdentities,
			expectedConfigRevision,
			&profileSaveError,
			recoveryConfirmed))
		{
			CloneJsonValue(
				previousProfiles,
				Owner->CurrentConfig->document,
				Owner->CurrentConfig->document.GetAllocator());
			bool avisoRollbackOk = true;
			bool avisoBackupRollbackOk = true;
			if (avisoModel != nullptr)
			{
				if (avisoExistedBeforeSave)
				{
					avisoRollbackOk = RestoreRollbackSnapshotAtomically(
						avisoRollbackSnapshotPath,
						avisoPath);
					if (avisoRollbackOk)
						avisoRollbackSnapshotPath.clear();
				}
				else
					avisoRollbackOk =
						::DeleteFileA(avisoPath.c_str()) != FALSE ||
						::GetLastError() == ERROR_FILE_NOT_FOUND;

				if (!avisoSaveIsRecovery)
				{
					const std::string backupPath = avisoPath + ".bak";
					if (avisoBackupExistedBeforeSave)
					{
						avisoBackupRollbackOk = RestoreRollbackSnapshotAtomically(
							avisoBackupRollbackSnapshotPath,
							backupPath);
						if (avisoBackupRollbackOk)
							avisoBackupRollbackSnapshotPath.clear();
					}
					else
					{
						avisoBackupRollbackOk =
							::DeleteFileA(backupPath.c_str()) != FALSE ||
							::GetLastError() == ERROR_FILE_NOT_FOUND;
					}
				}
			}
			error = profileSaveError.empty()
				? "Unable to save vSMR_Profiles.json atomically."
				: profileSaveError;
			if (!avisoRollbackOk)
			{
				error += " The AVISO rollback also failed.";
				if (!avisoRollbackSnapshotPath.empty())
						error += " The exact pre-save file remains at " +
							avisoRollbackSnapshotPath + ".";
			}
			if (!avisoBackupRollbackOk)
			{
				error += " The AVISO backup rollback also failed.";
				if (!avisoBackupRollbackSnapshotPath.empty())
					error += " The exact pre-save backup remains at " +
						avisoBackupRollbackSnapshotPath + ".";
			}
			return false;
		}

		if (!DeleteRollbackSnapshot(avisoRollbackSnapshotPath))
		{
			Logger::info(
				"Warning: unable to remove completed AVISO transaction snapshot " +
				avisoRollbackSnapshotPath);
		}
		if (!DeleteRollbackSnapshot(avisoBackupRollbackSnapshotPath))
		{
			Logger::info(
				"Warning: unable to remove completed AVISO backup transaction snapshot " +
				avisoBackupRollbackSnapshotPath);
		}

		// Display settings are staged with the rest of the Control Center draft,
		// but ShowFps is ASR state rather than profile JSON. Commit it only after
		// the profiles/AVISO transaction succeeds so a failed Save changes no
		// live or persisted display state.
		if (hasStagedShowFps)
		{
			Owner->ShowFps = stagedShowFps;
			Owner->SaveDataToAsr(
				"ShowFps",
				"Show FPS counter",
				Owner->ShowFps ? "1" : "0");
		}

		bool reloadFailed = false;
		bool avisoReloadFailed = false;
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr || radar->CurrentConfig == nullptr ||
				!radar->CurrentConfig->sharesConfigFileWith(*Owner->CurrentConfig))
			{
				continue;
			}

			if (!radar->ReloadConfig())
				reloadFailed = true;
			if (avisoModel != nullptr &&
				EqualsNoCase(
					radar->GetAvisoGeoJsonEditorPathForAirport(radar->getActiveAirport()),
					avisoPath))
			{
				if (!radar->ForceReloadAvisoGeoJson())
					avisoReloadFailed = true;
			}
			radar->RequestRefresh();
			if (radar != Owner && radar->VsmrControlCenterDialog != nullptr)
				radar->VsmrControlCenterDialog->SyncFromRadar("external-save");
		}
		if (!activeProfileBefore.empty() &&
			Owner->CurrentConfig->isItActiveProfile(activeProfileBefore) != 0)
			Owner->LoadProfile(activeProfileBefore);
		if (reloadFailed || avisoReloadFailed)
		{
			const std::string warning = reloadFailed && avisoReloadFailed
				? "The files were saved, but one or more radar windows could not reload their configuration or AVISO renderer. Reload vSMR before editing again."
				: avisoReloadFailed
					? "The files were saved, but one or more radar windows could not reload the AVISO renderer. Reload vSMR before editing again."
					: "The files were saved, but one or more radar windows could not reload them. Reload vSMR before editing again.";
			Logger::info(warning);
			Owner->GetPlugIn()->DisplayUserMessage(
				"vSMR",
				"Configuration reload",
				warning.c_str(),
				true, true, false, false, false);
		}
		return true;
	}

	bool HandleProfileChange(
		const rapidjson::Value* payload,
		std::string& error)
	{
		const std::string profile =
			payload != nullptr ? ReadString(*payload, "profile") : "";
		if (profile.empty())
		{
			error = "Profile name is required.";
			return false;
		}
		if (!Owner->SetActiveProfileForEditor(profile, true))
		{
			error = "The selected profile could not be activated.";
			return false;
		}
		return true;
	}

	bool HandleModeChange(
		const rapidjson::Value* payload,
		std::string& error)
	{
		if (payload == nullptr || !payload->IsObject())
		{
			error = "Mode payload must be an object.";
			return false;
		}
		std::string profile = ReadString(*payload, "profile");
		if (profile.empty())
			profile = Owner->GetActiveProfileNameForEditor();
		const std::string mode = ReadString(*payload, "mode");
		if (profile.empty() || mode.empty())
		{
			error = "Profile and mode names are required.";
			return false;
		}
		if (!Owner->SetProfileDisplayModeActiveForEditor(profile, mode))
		{
			error = "The selected display mode could not be activated.";
			return false;
		}
		return true;
	}

	bool HandleInsetToggle(
		const rapidjson::Value* payload,
		bool srwOnly,
		std::string& error)
	{
		if (payload == nullptr || !payload->IsObject())
		{
			error = "Inset payload must be an object.";
			return false;
		}
		const std::string airport = TrimAscii(ReadString(*payload, "airport"));
		if (!airport.empty() && !EqualsNoCase(airport, TrimAscii(Owner->getActiveAirport())))
		{
			error = "Inset request belongs to a different airport.";
			return false;
		}
		const std::string profile = TrimAscii(ReadString(*payload, "profile"));
		if (!profile.empty() && !EqualsNoCase(profile, TrimAscii(Owner->GetActiveProfileNameForEditor())))
		{
			error = "Inset request belongs to a different profile.";
			return false;
		}
		const std::string window = LowerAscii(ReadString(*payload, "window"));
		int id = 0;
		if (window == "srw1") id = 1;
		else if (!srwOnly && window == "weather") id = APPWINDOW_WEATHER - APPWINDOW_BASE;
		else if (!srwOnly && window == "timer") id = APPWINDOW_TIMER - APPWINDOW_BASE;
		else if (!srwOnly && (window == "aviso" || window.empty())) id = 3;
		if (id == 0)
		{
			error = "Unknown inset window.";
			return false;
		}
		const bool visible = ReadBool(*payload, "visible", false);
		Owner->CancelInsetWindowInteractions();
		Owner->appWindowDisplays[id] = visible;
		if (!visible)
		{
			auto windowIt = Owner->appWindows.find(id);
			if (windowIt != Owner->appWindows.end() && windowIt->second != nullptr)
				windowIt->second->ResetAvisoInteractionState();
		}
		Owner->SaveInsetStateToAsrForAirport(Owner->getActiveAirport());
		Owner->RequestRefresh();
		return true;
	}

	bool HandlePreset(
		VsmrBridgeAction action,
		const rapidjson::Value* payload,
		std::string& error)
	{
		if (payload == nullptr || !payload->IsObject())
		{
			error = "Inset preset payload must be an object.";
			return false;
		}
		const std::string airport = TrimAscii(ReadString(*payload, "airport"));
		if (!airport.empty() && !EqualsNoCase(airport, TrimAscii(Owner->getActiveAirport())))
		{
			error = "Inset preset request belongs to a different airport.";
			return false;
		}
		std::string presetName = ReadString(*payload, "preset");
		if (payload->HasMember("preset") && (*payload)["preset"].IsObject())
			presetName = ReadString((*payload)["preset"], "name");
		const std::string oldName = ReadString(*payload, "oldName");
		const std::string sourceName = ReadString(*payload, "source");
		const bool linked = ReadBool(
			*payload,
			"linked_movement",
			payload->HasMember("preset") && (*payload)["preset"].IsObject()
				? ReadBool((*payload)["preset"], "linked_movement", false)
				: false);

		bool ok = false;
		switch (action)
		{
		case VsmrBridgeAction::InsetPresetLoad:
			ok = Owner->LoadAvisoPreset(presetName);
			break;
		case VsmrBridgeAction::InsetPresetCapture:
		{
			std::string savedName;
			ok = Owner->SaveAvisoPreset(presetName, false, &savedName, linked);
			break;
		}
		case VsmrBridgeAction::InsetPresetUpdate:
			ok = Owner->UpdateActiveAvisoPreset();
			break;
		case VsmrBridgeAction::InsetPresetRename:
			ok = Owner->RenameAvisoPreset(
				oldName.empty() ? Owner->GetActiveAvisoPresetName() : oldName,
				presetName,
				linked);
			break;
		case VsmrBridgeAction::InsetPresetDuplicate:
		{
			std::string savedName;
			ok = Owner->DuplicateAvisoPreset(
				sourceName.empty() ? Owner->GetActiveAvisoPresetName() : sourceName,
				presetName,
				&savedName);
			break;
		}
		case VsmrBridgeAction::InsetPresetDefault:
			ok = Owner->SetDefaultAvisoPreset(presetName);
			break;
		case VsmrBridgeAction::InsetPresetReset:
			ok = Owner->ResetActiveAvisoPreset();
			break;
		case VsmrBridgeAction::InsetPresetDelete:
			ok = Owner->DeleteAvisoPreset(presetName);
			break;
		case VsmrBridgeAction::InsetPresetLinked:
			ok = Owner->SetActiveAvisoPresetLinkedMovement(linked);
			break;
		default:
			break;
		}

		if (!ok)
			error = "Inset preset operation failed.";
		Owner->RequestRefresh();
		return ok;
	}

	bool HandleLegacyPresetAssignment(
		const rapidjson::Value* payload,
		std::string& error)
	{
		if (Owner == nullptr || Owner->CurrentConfig == nullptr ||
			payload == nullptr || !payload->IsObject())
		{
			error = "Legacy inset preset assignment is not available.";
			return false;
		}
		const std::string airport = TrimAscii(ReadString(*payload, "airport"));
		if (airport.empty() ||
			!EqualsNoCase(airport, TrimAscii(Owner->getActiveAirport())))
		{
			error = "The active airport changed before the legacy presets were assigned.";
			return false;
		}

		size_t assignedPresetCount = 0;
		if (!Owner->CurrentConfig->assignUnscopedAvisoPresetsToAirport(
			Owner->GetActiveProfileNameForEditor(),
			airport,
			assignedPresetCount,
			error))
		{
			return false;
		}

		bool reloadFailed = false;
		for (CSMRRadar* radar : RadarScreensOpened)
		{
			if (radar == nullptr || radar->CurrentConfig == nullptr ||
				!radar->CurrentConfig->sharesConfigFileWith(*Owner->CurrentConfig))
			{
				continue;
			}
			if (!radar->ReloadConfig())
				reloadFailed = true;
			radar->RequestRefresh();
			if (radar != Owner && radar->VsmrControlCenterDialog != nullptr)
				radar->VsmrControlCenterDialog->SyncFromRadar("external-save");
		}
		if (reloadFailed)
		{
			Owner->GetPlugIn()->DisplayUserMessage(
				"vSMR", "Inset preset migration",
				"The assignment was saved, but one or more radar windows must be reloaded.",
				true, true, false, false, false);
		}
		return true;
	}

	bool HandleAlerts(
		const rapidjson::Value* payload,
		std::string& error)
	{
		if (Owner->CurrentConfig == nullptr ||
			Owner->RimcasInstance == nullptr ||
			payload == nullptr ||
			!payload->IsObject())
		{
			error = "Alert state is not available.";
			return false;
		}

		rapidjson::Value& activeProfile =
			const_cast<rapidjson::Value&>(Owner->CurrentConfig->getActiveProfile());
		if (!activeProfile.IsObject())
		{
			error = "The active profile is invalid.";
			return false;
		}

		Allocator& allocator = Owner->CurrentConfig->document.GetAllocator();
		rapidjson::Value& rimcas =
			EnsureObjectMember(activeProfile, "rimcas", allocator);
		if (payload->HasMember("rimcas") && (*payload)["rimcas"].IsObject())
			CloneJsonValue((*payload)["rimcas"], rimcas, allocator);
		SetBoolMember(
			rimcas,
			"enabled",
			ReadBool(*payload, "enabled", true),
			allocator);

		std::unordered_set<std::string> inactiveAlerts;
		if (rimcas.HasMember("inactive_alerts") &&
			rimcas["inactive_alerts"].IsArray())
		{
			const rapidjson::Value& alerts = rimcas["inactive_alerts"];
			for (rapidjson::SizeType index = 0; index < alerts.Size(); ++index)
			{
				const rapidjson::Value& alert = alerts[index];
				if (alert.IsString())
					inactiveAlerts.insert(alert.GetString());
			}
		}
		Owner->RimcasInstance->setInactiveAlerts(inactiveAlerts);

		const std::string visibility = LowerAscii(ReadString(*payload, "visibility"));
		Owner->isLVP = visibility == "lvp" || visibility == "low";

		if (payload->HasMember("runways") && (*payload)["runways"].IsArray())
		{
			Owner->RimcasInstance->MonitoredRunwayArr.clear();
			Owner->RimcasInstance->MonitoredRunwayDep.clear();
			Owner->RimcasInstance->ClosedRunway.clear();
			const rapidjson::Value& runways = (*payload)["runways"];
			for (rapidjson::SizeType index = 0; index < runways.Size(); ++index)
			{
				const rapidjson::Value& runway = runways[index];
				if (!runway.IsObject())
					continue;
				const std::string name = ReadString(runway, "id");
				if (name.empty())
					continue;
				Owner->RimcasInstance->MonitoredRunwayArr[name] =
					ReadBool(runway, "arrival", false);
				Owner->RimcasInstance->MonitoredRunwayDep[name] =
					ReadBool(runway, "departure", false);
				Owner->RimcasInstance->ClosedRunway[name] =
					ReadBool(runway, "closed", false);
			}
		}

		Owner->LoadProfile(Owner->GetActiveProfileNameForEditor());
		Owner->RequestRefresh();
		return true;
	}

	bool HandleSettings(
		const rapidjson::Value* payload,
		std::string& error)
	{
		if (payload == nullptr || !payload->IsObject())
		{
			error = "Settings payload must be an object.";
			return false;
		}

		const std::string resolution = ReadString(*payload, "resolutionPreset");
		if (!resolution.empty() &&
			!Owner->SetSmallTargetIconBoostResolutionPreset(resolution, false))
		{
			error = "The selected resolution preset is invalid.";
			return false;
		}

		if (payload->HasMember("showFps") && (*payload)["showFps"].IsBool())
		{
			Owner->ShowFps = (*payload)["showFps"].GetBool();
			Owner->SaveDataToAsr(
				"ShowFps",
				"Show FPS counter",
				Owner->ShowFps ? "1" : "0");
		}

		if (Owner->CurrentConfig != nullptr &&
			payload->HasMember("rimcas") &&
			(*payload)["rimcas"].IsBool())
		{
			rapidjson::Value& activeProfile =
				const_cast<rapidjson::Value&>(Owner->CurrentConfig->getActiveProfile());
			rapidjson::Value& rimcas = EnsureObjectMember(
				activeProfile,
				"rimcas",
				Owner->CurrentConfig->document.GetAllocator());
			SetBoolMember(
				rimcas,
				"enabled",
				(*payload)["rimcas"].GetBool(),
				Owner->CurrentConfig->document.GetAllocator());
		}
		Owner->RequestRefresh();
		return true;
	}

	bool HandleDatalinkSettings(
		const rapidjson::Value* payload,
		std::string& error)
	{
		CSMRPlugin* plugin = DatalinkPlugin();
		if (plugin == nullptr)
		{
			error = "The vSMR datalink service is not available.";
			return false;
		}
		if (payload == nullptr || !payload->IsObject())
		{
			error = "Datalink settings payload must be an object.";
			return false;
		}

		const DatalinkControlState current = plugin->GetDatalinkControlState();
		const std::string callsign = payload->HasMember("logonCallsign")
			? ReadString(*payload, "logonCallsign")
			: current.logonCallsign;
		const bool replacePassword = ReadBool(*payload, "replacePassword", false);
		const std::string password = replacePassword
			? ReadString(*payload, "password")
			: "";
		if (replacePassword && password.empty())
		{
			error = "Enter a Hoppie code before replacing the saved code.";
			return false;
		}

		return plugin->UpdateDatalinkControlSettings(
			callsign,
			password,
			replacePassword,
			ReadBool(*payload, "playSound", current.playSound),
			ReadBool(*payload, "cdmAutoEnabled", current.cdmAutoEnabled),
			ReadInt(*payload, "cdmDelayMinutes", current.cdmDelayMinutes),
			ReadInt(*payload, "cdmCooldownMinutes", current.cdmCooldownMinutes),
			error);
	}

	bool HandleAvisoGroups(
		VsmrBridgeAction action,
		const rapidjson::Value* payload,
		std::string& error)
	{
		if (Owner == nullptr)
		{
			error = "vSMR radar state is not available.";
			return false;
		}
		if (payload == nullptr || !payload->IsObject())
		{
			error = "AVISO group payload must be an object.";
			return false;
		}

		const std::string avisoPath =
			Owner->ResolveAvisoGeoJsonPathForAirport(Owner->getActiveAirport());
		if (!avisoPath.empty())
			Owner->EnsureAvisoGeoJsonLoaded(avisoPath);

		if (action == VsmrBridgeAction::RuntimeGroupVisibility)
		{
			std::string groupId = ReadString(*payload, "id");
			if (groupId.empty())
				groupId = ReadString(*payload, "group_id");
			if (groupId.empty())
			{
				error = "AVISO group id is required.";
				return false;
			}
			if (!payload->HasMember("visible") || !(*payload)["visible"].IsBool())
			{
				error = "AVISO group visibility must be a boolean.";
				return false;
			}
			if (!Owner->SetAvisoGroupVisibility(groupId, (*payload)["visible"].GetBool()))
			{
				error = "Unknown AVISO group id.";
				return false;
			}
			return true;
		}

		if (!payload->HasMember("groups") || !(*payload)["groups"].IsArray())
		{
			error = "AVISO groups must be an array.";
			return false;
		}
		const rapidjson::Value& groupValues = (*payload)["groups"];
		std::unordered_set<std::string> seenIds;

		if (action == VsmrBridgeAction::RuntimeGroupsVisibility)
		{
			std::vector<std::pair<std::string, bool>> visibility;
			visibility.reserve(groupValues.Size());
			for (rapidjson::SizeType index = 0; index < groupValues.Size(); ++index)
			{
				const rapidjson::Value& item = groupValues[index];
				if (!item.IsObject())
				{
					error = "Each AVISO group visibility entry must be an object.";
					return false;
				}
				std::string groupId = ReadString(item, "id");
				if (groupId.empty())
					groupId = ReadString(item, "group_id");
				if (groupId.empty())
				{
					error = "Each AVISO group visibility entry requires an id.";
					return false;
				}
				if (!seenIds.insert(groupId).second)
				{
					error = "AVISO group ids must be unique.";
					return false;
				}
				if (!item.HasMember("visible") || !item["visible"].IsBool())
				{
					error = "Each AVISO group visibility entry requires a boolean visible value.";
					return false;
				}
				visibility.push_back(std::make_pair(groupId, item["visible"].GetBool()));
			}

			if (!Owner->SetAvisoGroupVisibilities(visibility))
			{
				error = "One or more AVISO group ids are unknown.";
				return false;
			}
			return true;
		}

		if (action != VsmrBridgeAction::RuntimeGroupsUpdate)
		{
			error = "Unsupported AVISO group action.";
			return false;
		}

		std::unordered_map<std::string, bool> existingVisibility;
		for (const CSMRRadar::AvisoGroup& existing : Owner->GetAvisoGroups())
			existingVisibility[existing.id] = existing.visible;

		std::vector<CSMRRadar::AvisoGroup> groups;
		groups.reserve(groupValues.Size());
		for (rapidjson::SizeType index = 0; index < groupValues.Size(); ++index)
		{
			const rapidjson::Value& item = groupValues[index];
			if (!item.IsObject())
			{
				error = "Each AVISO group definition must be an object.";
				return false;
			}

			CSMRRadar::AvisoGroup group;
			group.id = ReadString(item, "id");
			if (group.id.empty())
				group.id = ReadString(item, "group_id");
			if (group.id.empty())
			{
				error = "Each AVISO group definition requires an id.";
				return false;
			}
			if (!seenIds.insert(group.id).second)
			{
				error = "AVISO group ids must be unique.";
				return false;
			}

			group.name = ReadString(item, "name");
			if (group.name.empty())
				group.name = group.id;
			const auto existing = existingVisibility.find(group.id);
			group.visible =
				existing != existingVisibility.end()
				? existing->second
				: true;
			if (item.HasMember("visible"))
			{
				if (!item["visible"].IsBool())
				{
					error = "AVISO group visible values must be boolean.";
					return false;
				}
				group.visible = item["visible"].GetBool();
			}
			groups.push_back(std::move(group));
		}

		if (payload->HasMember("aviso"))
		{
			const rapidjson::Value& stagedAviso = (*payload)["aviso"];
			if (!stagedAviso.IsObject() ||
				!stagedAviso.HasMember("features") ||
				!stagedAviso["features"].IsArray())
			{
				error = "Staged AVISO state must be a GeoJSON FeatureCollection.";
				return false;
			}
			if (!Owner->ApplyAvisoGroupMembershipSnapshot(stagedAviso, &error))
			{
				if (error.empty())
					error = "Unable to apply staged AVISO group membership.";
				return false;
			}
		}

		return Owner->UpdateAvisoGroups(groups);
	}

	bool Dispatch(
		const DecodedEnvelope& envelope,
		std::string& error)
	{
		error.clear();
		switch (envelope.action)
		{
		case VsmrBridgeAction::UiReady:
			SendAuthoritativeState("initial", envelope.id);
			return true;
		case VsmrBridgeAction::WindowClose:
			if (Callbacks.closeWindow)
				Callbacks.closeWindow();
			return true;
		case VsmrBridgeAction::WindowDragStart:
			if (Callbacks.beginWindowDrag)
				Callbacks.beginWindowDrag();
			return true;
		case VsmrBridgeAction::StateSave:
			if (!SaveAll(envelope.payload, envelope.id, error))
				return false;
			{
				rapidjson::Document saved;
				MakeEnvelope(saved, "state.saved", envelope.id);
				Allocator& allocator = saved.GetAllocator();
				rapidjson::Value payload(rapidjson::kObjectType);
				AddString(payload, "message", "Saved and reloaded", allocator);
				AddString(
					payload,
					"configRevision",
					Owner->CurrentConfig->getConfigRevision(),
					allocator);
				AddString(
					payload,
					"avisoRevision",
					FileRevision(Owner->GetAvisoGeoJsonEditorPathForAirport(Owner->getActiveAirport())),
					allocator);
				rapidjson::Value settings;
				BuildSettings(settings, allocator);
				payload.AddMember("settings", settings, allocator);
				saved.AddMember("payload", payload, allocator);
				Send(saved);
			}
			SendAuthoritativeState("save", envelope.id);
			return true;
		case VsmrBridgeAction::StateReload:
		{
			if (Owner == nullptr)
			{
				error = "vSMR radar state is not available.";
				return false;
			}
			if (Callbacks.cancelPendingResources)
				Callbacks.cancelPendingResources();
			const bool configReloaded = Owner->ReloadConfig();
			const bool avisoReloaded = Owner->ForceReloadAvisoGeoJson();
			if (!configReloaded || !avisoReloaded)
			{
				// ReloadConfig may intentionally activate a validated backup or a
				// read-only migrated document while returning false. Publish that exact
				// recovery state before reporting the warning so the Web UI and native
				// revision/health state cannot diverge. Keep the last rendered AVISO in
				// the UI when its replacement failed validation.
				SendAuthoritativeState("reload", envelope.id, avisoReloaded);
				error =
					!configReloaded && !avisoReloaded
						? "Profiles did not reload normally and AVISO validation failed. The safest available profiles state and previous rendered overlay remain active; review Settings."
						: !configReloaded
							? "Profiles did not reload normally. The safest available in-memory state is active; review the recovery status in Settings."
							: "AVISO validation failed; the previous rendered overlay remains active.";
				return false;
			}
			SendAuthoritativeState("reload", envelope.id);
			SendAck(envelope.id, "state.reload", "Configuration reloaded");
			return true;
		}
		case VsmrBridgeAction::StateReset:
			if (Callbacks.cancelPendingResources)
				Callbacks.cancelPendingResources();
			if (!Callbacks.requestResetDefaults)
			{
				error = "Bundled defaults are not available in this host.";
				return false;
			}
			Callbacks.requestResetDefaults(envelope.id);
			return true;
		case VsmrBridgeAction::StateRestoreBackup:
		{
			if (Owner == nullptr || Owner->CurrentConfig == nullptr)
			{
				error = "vSMR configuration is not available.";
				return false;
			}
			if (Callbacks.cancelPendingResources)
				Callbacks.cancelPendingResources();
			if (!Owner->CurrentConfig->restoreBackup(error))
				return false;

			bool reloadFailed = false;
			for (CSMRRadar* radar : RadarScreensOpened)
			{
				if (radar == nullptr || radar->CurrentConfig == nullptr ||
					!radar->CurrentConfig->sharesConfigFileWith(*Owner->CurrentConfig))
					continue;
				if (!radar->ReloadConfig())
					reloadFailed = true;
				radar->RequestRefresh();
				if (radar != Owner && radar->VsmrControlCenterDialog != nullptr)
					radar->VsmrControlCenterDialog->SyncFromRadar("backup-restored");
			}
			if (reloadFailed)
			{
				SendAuthoritativeState("backup-restored", envelope.id);
				error = "The profiles backup was restored, but one or more radar windows could not reload it. Reload vSMR.";
				return false;
			}
			SendAuthoritativeState("backup-restored", envelope.id);
			SendAck(envelope.id, envelope.type, "Profiles backup restored");
			return true;
		}
		case VsmrBridgeAction::StateUndo:
			return ApplyHistoryState(
				envelope.payload,
				"undo",
				envelope.id,
				error);
		case VsmrBridgeAction::StateRedo:
			return ApplyHistoryState(
				envelope.payload,
				"redo",
				envelope.id,
				error);
		case VsmrBridgeAction::RuntimeProfileChange:
			if (!HandleProfileChange(envelope.payload, error))
				return false;
			SendAuthoritativeState("profile", envelope.id);
			return true;
		case VsmrBridgeAction::RuntimeModeChange:
			if (!HandleModeChange(envelope.payload, error))
				return false;
			SendAuthoritativeState("mode", envelope.id);
			return true;
		case VsmrBridgeAction::RuntimeInsetToggle:
			if (!HandleInsetToggle(envelope.payload, false, error))
				return false;
			SendAuthoritativeState("inset", envelope.id);
			return true;
		case VsmrBridgeAction::RuntimeSrwToggle:
			if (!HandleInsetToggle(envelope.payload, true, error))
				return false;
			SendAuthoritativeState("inset", envelope.id);
			return true;
		case VsmrBridgeAction::InsetPresetLoad:
		case VsmrBridgeAction::InsetPresetCapture:
		case VsmrBridgeAction::InsetPresetUpdate:
		case VsmrBridgeAction::InsetPresetRename:
		case VsmrBridgeAction::InsetPresetDuplicate:
		case VsmrBridgeAction::InsetPresetDefault:
		case VsmrBridgeAction::InsetPresetReset:
		case VsmrBridgeAction::InsetPresetDelete:
		case VsmrBridgeAction::InsetPresetLinked:
			if (!HandlePreset(envelope.action, envelope.payload, error))
				return false;
			SendAuthoritativeState("preset", envelope.id);
			return true;
		case VsmrBridgeAction::InsetPresetLegacyAssign:
			if (!HandleLegacyPresetAssignment(envelope.payload, error))
				return false;
			SendAuthoritativeState("legacy-preset-assigned", envelope.id);
			return true;
		case VsmrBridgeAction::AlertsUpdate:
			if (!HandleAlerts(envelope.payload, error))
				return false;
			SendAck(envelope.id, envelope.type, "Alert settings applied");
			return true;
		case VsmrBridgeAction::SettingsUpdate:
			if (!HandleSettings(envelope.payload, error))
				return false;
			SendAck(envelope.id, envelope.type, "Settings applied");
			return true;
		case VsmrBridgeAction::DatalinkStateRequest:
			SendDatalinkState(envelope.id);
			return true;
		case VsmrBridgeAction::DatalinkSettingsUpdate:
			if (!HandleDatalinkSettings(envelope.payload, error))
				return false;
			SendDatalinkState(envelope.id, "Datalink settings applied");
			SendAck(envelope.id, envelope.type, "Datalink settings applied");
			return true;
		case VsmrBridgeAction::DatalinkConnect:
		{
			CSMRPlugin* plugin = DatalinkPlugin();
			if (plugin == nullptr)
			{
				error = "The vSMR datalink service is not available.";
				return false;
			}
			if (!plugin->ConnectDatalink(error))
				return false;
			SendDatalinkState(envelope.id, "Connecting to Hoppie");
			return true;
		}
		case VsmrBridgeAction::DatalinkDisconnect:
		{
			CSMRPlugin* plugin = DatalinkPlugin();
			if (plugin == nullptr)
			{
				error = "The vSMR datalink service is not available.";
				return false;
			}
			if (!plugin->DisconnectDatalink(error))
				return false;
			SendDatalinkState(envelope.id, "Disconnected from Hoppie");
			return true;
		}
		case VsmrBridgeAction::DatalinkPoll:
		{
			CSMRPlugin* plugin = DatalinkPlugin();
			if (plugin == nullptr)
			{
				error = "The vSMR datalink service is not available.";
				return false;
			}
			if (!plugin->PollDatalink(error))
				return false;
			SendDatalinkState(envelope.id, "Polling Hoppie messages");
			return true;
		}
		case VsmrBridgeAction::CdmScan:
		{
			CSMRPlugin* plugin = DatalinkPlugin();
			if (plugin == nullptr)
			{
				error = "The vSMR datalink service is not available.";
				return false;
			}
			std::string result;
			if (!plugin->RunCdmReminderScan(result, error))
				return false;
			SendDatalinkState(envelope.id, result);
			SendAck(envelope.id, envelope.type, result);
			return true;
		}
		case VsmrBridgeAction::PerformanceStateRequest:
		{
			const int requestedWindow = envelope.payload != nullptr
				? ReadInt(*envelope.payload, "windowSeconds", 120)
				: 120;
			const int requestedPoints = envelope.payload != nullptr
				? ReadInt(*envelope.payload, "maxSeriesPoints", 120)
				: 120;
			SendPerformanceState(
				envelope.id,
				NormalizePerformanceWindowSeconds(requestedWindow),
				NormalizePerformanceSeriesPoints(requestedPoints));
			return true;
		}
		case VsmrBridgeAction::PerformanceReset:
			if (Owner == nullptr)
			{
				error = "vSMR performance diagnostics are not available.";
				return false;
			}
			Owner->ResetPerformanceDiagnostics();
			PeakProcessGdiObjects = 0;
			PeakVsmrCachedBitmaps = 0;
			PeakEstimatedBitmapBytes = 0;
			PeakAvisoPendingDepth = 0;
			LastPerformanceGeneration = 0;
			SendAck(envelope.id, envelope.type, "Performance sample reset");
			return true;
		case VsmrBridgeAction::PerformanceReportExport:
		{
			if (Owner == nullptr)
			{
				error = "vSMR performance diagnostics are not available.";
				return false;
			}
			const std::string format = envelope.payload != nullptr
				? LowerAscii(ReadString(*envelope.payload, "format"))
				: "json";
			if (!format.empty() && format != "json")
			{
				error = "Only JSON performance reports are supported.";
				return false;
			}
			const std::uint32_t windowSeconds = NormalizePerformanceWindowSeconds(
				envelope.payload != nullptr
					? ReadInt(*envelope.payload, "windowSeconds", 120)
					: 120);
			const std::string nativeReport = Owner->BuildPerformanceReportJson(
				windowSeconds,
				VsmrPerformance::MaximumFrameSamples);
			std::string report;
			if (!AddWorkerQueuesToPerformanceReport(nativeReport, report, error))
				return false;
			std::string reportPath;
			if (!WritePerformanceReportAtomically(
				report,
				std::filesystem::path(Owner->DataPath),
				reportPath,
				error))
			{
				return false;
			}
			SendPerformanceExportAck(envelope.id, reportPath);
			Logger::info("Performance diagnostics report written path=" + reportPath);
			return true;
		}
		case VsmrBridgeAction::ResourceComputerLoad:
		{
			const std::string resource =
				RuntimeResourceFromType(envelope.type, envelope.payload);
			if (Callbacks.requestComputerLoad)
				Callbacks.requestComputerLoad(resource, envelope.id);
			return true;
		}
		case VsmrBridgeAction::ResourceGithubLoad:
		{
			const std::string resource =
				RuntimeResourceFromType(envelope.type, envelope.payload);
			const std::string url = envelope.payload != nullptr
				? ReadString(*envelope.payload, "url")
				: "";
			if (!IsAllowedGithubUrl(url))
			{
				error = "Only github.com and raw.githubusercontent.com file URLs are allowed.";
				return false;
			}
			const std::string normalizedUrl = NormalizeGithubRawUrl(url);
			if (normalizedUrl.empty())
			{
				error = "The GitHub URL must point to a file.";
				return false;
			}
			if (Callbacks.requestGithubLoad)
				Callbacks.requestGithubLoad(
					resource,
					normalizedUrl,
					envelope.id);
			return true;
		}
		case VsmrBridgeAction::RuntimeGroupVisibility:
		case VsmrBridgeAction::RuntimeGroupsVisibility:
		case VsmrBridgeAction::RuntimeGroupsUpdate:
			if (!HandleAvisoGroups(envelope.action, envelope.payload, error))
				return false;
			if (envelope.payload != nullptr &&
				envelope.payload->IsObject() &&
				envelope.payload->HasMember("aviso") &&
				(*envelope.payload)["aviso"].IsObject())
			{
				SendStagedAuthoritativeState(
					*envelope.payload,
					"group",
					envelope.id);
			}
			else
			{
				SendAuthoritativeState("group", envelope.id);
			}
			return true;
		default:
			error = "Unsupported bridge action: " + envelope.type;
			return false;
		}
	}
};

VsmrControlCenterBridge::VsmrControlCenterBridge(
	CSMRRadar* owner,
	VsmrBridgeHostCallbacks callbacks)
	: State(std::make_unique<Impl>(owner, std::move(callbacks)))
{
}

VsmrControlCenterBridge::~VsmrControlCenterBridge() = default;

void VsmrControlCenterBridge::SetOwner(CSMRRadar* owner)
{
	State->Owner = owner;
}

bool VsmrControlCenterBridge::HandleWebMessage(const std::string& messageJson)
{
	if (messageJson.empty() || messageJson.size() > kMaximumBridgeMessageBytes)
	{
		State->SendError("", "Bridge message is empty or exceeds the 32 MB limit.");
		return false;
	}

	rapidjson::Document document;
	document.Parse<0>(messageJson.c_str());
	if (document.HasParseError())
	{
		State->SendError("", "Bridge message contains invalid JSON.");
		return false;
	}

	DecodedEnvelope envelope;
	std::string error;
	if (!DecodeEnvelope(document, envelope, error))
	{
		State->SendError(envelope.id, error);
		return false;
	}

	if (!State->Dispatch(envelope, error))
	{
		State->SendError(envelope.id, error);
		return false;
	}
	return true;
}

void VsmrControlCenterBridge::PushAuthoritativeState(const std::string& reason)
{
	State->SendAuthoritativeState(reason);
}

void VsmrControlCenterBridge::PushError(
	const std::string& requestId,
	const std::string& message)
{
	State->SendError(requestId, message);
}

bool VsmrControlCenterBridge::ValidateLoadedResource(
	const std::string& resource,
	const std::string& jsonText,
	std::string& error) const
{
	error.clear();
	if (jsonText.empty())
	{
		error = "The selected resource is empty.";
		return false;
	}

	rapidjson::Document parsed;
	parsed.Parse<0>(jsonText.c_str());
	if (parsed.HasParseError())
	{
		error = "The selected resource contains invalid JSON.";
		return false;
	}

	const std::string normalizedResource = LowerAscii(resource);
	if (normalizedResource == "profiles")
		return ValidateProfileArray(parsed, error);
	if (normalizedResource == "aviso")
	{
		if (!parsed.IsObject() ||
			!parsed.HasMember("type") ||
			!parsed["type"].IsString() ||
			std::strcmp(parsed["type"].GetString(), "FeatureCollection") != 0 ||
			!parsed.HasMember("features") ||
			!parsed["features"].IsArray())
		{
			error = "The selected AVISO file is not a GeoJSON FeatureCollection.";
			return false;
		}
		if (parsed.HasMember("metadata") && !parsed["metadata"].IsObject())
		{
			error = "AVISO metadata must be an object.";
			return false;
		}
		if (parsed.HasMember("metadata") && parsed["metadata"].IsObject() &&
			parsed["metadata"].HasMember("schema_version"))
		{
			const rapidjson::Value& schemaVersion = parsed["metadata"]["schema_version"];
			if (!schemaVersion.IsInt() || schemaVersion.GetInt() < 1)
			{
				error = "AVISO metadata.schema_version must be a positive integer.";
				return false;
			}
			if (schemaVersion.GetInt() > 2)
			{
				error = "The AVISO file uses a future schema version that this build does not support.";
				return false;
			}
		}

		AvisoDocumentModel validationModel;
		CloneJsonValue(
			parsed,
			validationModel.MutableDocument(),
			validationModel.MutableDocument().GetAllocator());
		validationModel.MarkIndexesDirty();
		const AvisoValidationResult result =
			validationModel.ValidateAndRecalculate();
		if (!result.ok)
		{
			error = result.errorText.empty()
				? "The selected AVISO file failed validation."
				: result.errorText;
			return false;
		}
		return true;
	}

	error = "Unknown resource type.";
	return false;
}

bool VsmrControlCenterBridge::HandleLoadedResource(
	const std::string& resource,
	const std::string& source,
	const std::string& requestId,
	const std::string& jsonText,
	const std::string& effectivePath)
{
	std::string validationError;
	if (!ValidateLoadedResource(resource, jsonText, validationError))
	{
		State->SendError(requestId, validationError);
		return false;
	}

	rapidjson::Document parsed;
	parsed.Parse<0>(jsonText.c_str());
	const std::string normalizedResource = LowerAscii(resource);
	if (normalizedResource == "profiles")
	{
		bool migrated = false;
		std::string migrationError;
		if (!CConfig::validateAndMigrateProfilesDocument(parsed, migrationError, migrated))
		{
			State->SendError(requestId, migrationError);
			return false;
		}
	}
	std::string normalizedEffectivePath;
	std::string activatedAvisoRevision;
	if (!effectivePath.empty())
	{
		std::string pathError;
		if (!VsmrResourceFiles::NormalizeExistingFilePath(
			effectivePath,
			normalizedEffectivePath,
			pathError))
		{
			State->SendError(
				requestId,
				pathError.empty() ? "The selected resource path is unavailable." : pathError);
			return false;
		}

		// The file picker/download result is validated before this method runs.
		// Reject a source that changed between that read and activation instead of
		// pairing stale JSON with a newer revision token.
		std::string activationJson;
		if (!ReadFileText(normalizedEffectivePath, activationJson) ||
			State->ContentRevision(activationJson) !=
				State->ContentRevision(jsonText))
		{
			State->SendError(
				requestId,
				"The selected resource changed while it was loading. Select it again.");
			return false;
		}

		if (State->Owner == nullptr)
		{
			State->SendError(requestId, "vSMR radar state is not available.");
			return false;
		}

		std::string activationError;
		if (normalizedResource == "profiles")
		{
			if (!State->Owner->SetProfilesConfigPath(
				normalizedEffectivePath,
				&activationError,
				true))
			{
				State->SendError(
					requestId,
					activationError.empty()
						? "Unable to activate the selected profiles file."
						: activationError);
				return false;
			}
			// Return exactly what the native runtime activated, not the earlier
			// picker buffer. Its revision now describes this same document.
			CloneJsonValue(
				State->Owner->CurrentConfig->document,
				parsed,
				parsed.GetAllocator());
		}
		else if (normalizedResource == "aviso")
		{
			const std::string activeAirport = NormalizeAirportCandidate(
				State->Owner->getActiveAirport());
			if (activeAirport.empty())
			{
				State->SendError(requestId, "Select an active airport before loading AVISO GeoJSON.");
				return false;
			}

			const std::string detectedAirport = DetectAvisoAirport(parsed, source);
			if (detectedAirport.empty())
			{
				State->SendError(
					requestId,
					"Could not determine the AVISO airport. Add metadata.icao or use a filename such as LFPO.geojson, LFPO_AVISO.geojson, or AVISO_LFPO.geojson.");
				return false;
			}
			if (detectedAirport != activeAirport)
			{
				State->SendError(
					requestId,
					"This AVISO file is for " + detectedAirport +
					". Select that airport before loading it; the active airport is " +
					activeAirport + ".");
				return false;
			}

			const auto previousOverride =
				State->Owner->AvisoGeoJsonOverridePaths.find(activeAirport);
			const bool hadPreviousOverride =
				previousOverride != State->Owner->AvisoGeoJsonOverridePaths.end();
			const std::string previousOverridePath = hadPreviousOverride
				? previousOverride->second
				: std::string();
			State->Owner->SetAvisoGeoJsonOverrideForAirport(
				activeAirport,
				normalizedEffectivePath);
			const std::string activatedPath =
				State->Owner->ResolveAvisoGeoJsonPathForAirport(activeAirport);
			if (!EqualsNoCase(activatedPath, normalizedEffectivePath) ||
				!State->Owner->ForceReloadAvisoGeoJson())
			{
				State->Owner->SetAvisoGeoJsonOverrideForAirport(
					activeAirport,
					hadPreviousOverride ? previousOverridePath : std::string());
				State->Owner->ForceReloadAvisoGeoJson();
				State->SendError(requestId, "Unable to activate the selected AVISO GeoJSON file.");
				return false;
			}

			std::string activatedJson;
			std::string activatedValidationError;
			if (!ReadFileText(normalizedEffectivePath, activatedJson) ||
				!ValidateLoadedResource(
					"aviso",
					activatedJson,
					activatedValidationError) ||
				State->ContentRevision(activatedJson) !=
					State->FileRevision(normalizedEffectivePath))
			{
				State->Owner->SetAvisoGeoJsonOverrideForAirport(
					activeAirport,
					hadPreviousOverride ? previousOverridePath : std::string());
				State->Owner->ForceReloadAvisoGeoJson();
				State->SendError(
					requestId,
					"The selected AVISO file changed during activation. Select it again.");
				return false;
			}
			activatedAvisoRevision =
				State->ContentRevision(activatedJson);
			parsed.Parse<0>(activatedJson.c_str());
			if (parsed.HasParseError() ||
				DetectAvisoAirport(parsed, source) != activeAirport)
			{
				State->Owner->SetAvisoGeoJsonOverrideForAirport(
					activeAirport,
					hadPreviousOverride ? previousOverridePath : std::string());
				State->Owner->ForceReloadAvisoGeoJson();
				State->SendError(
					requestId,
					"The activated AVISO no longer matches the active airport.");
				return false;
			}

			// The override is process-wide across radar screens. Do not publish it
			// to other Control Centers until validation and renderer activation have
			// both succeeded, otherwise a failed load briefly exposes the path that
			// is about to be rolled back.
			for (CSMRRadar* radar : RadarScreensOpened)
			{
				if (radar == nullptr || radar == State->Owner ||
					radar->VsmrControlCenterDialog == nullptr)
				{
					continue;
				}
				radar->VsmrControlCenterDialog->SyncFromRadar("resource-source");
			}
		}
	}

	rapidjson::Document message;
	MakeEnvelope(message, "resource.loaded", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	AddString(payload, "resource", normalizedResource, allocator);
	AddString(payload, "source", source, allocator);
	if (State->Owner != nullptr && State->Owner->CurrentConfig != nullptr)
	{
		AddString(
			payload,
			"configRevision",
			State->Owner->CurrentConfig->getConfigRevision(),
			allocator);
		AddString(
			payload,
			"avisoRevision",
			activatedAvisoRevision.empty()
				? State->FileRevision(State->Owner->GetAvisoGeoJsonEditorPathForAirport(State->Owner->getActiveAirport()))
				: activatedAvisoRevision,
			allocator);
		rapidjson::Value settings;
		State->BuildSettings(settings, allocator);
		payload.AddMember("settings", settings, allocator);
	}
	if (!normalizedEffectivePath.empty())
		AddString(payload, "path", normalizedEffectivePath, allocator);
	rapidjson::Value data;
	CloneJsonValue(parsed, data, allocator);
	payload.AddMember("data", data, allocator);
	message.AddMember("payload", payload, allocator);
	State->Send(message);
	return true;
}
