#include "stdafx.h"
#include "VsmrControlCenterBridge.hpp"

#include "AvisoDocumentModel.hpp"
#include "InsetWindow.h"
#include "SMRPlugin.hpp"
#include "SMRRadar.hpp"

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace
{
	constexpr int kBridgeProtocolVersion = 1;
	constexpr size_t kMaximumBridgeMessageBytes = 32u * 1024u * 1024u;

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

	bool EqualsNoCase(const std::string& left, const std::string& right)
	{
		return LowerAscii(left) == LowerAscii(right);
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
		if (type == "alerts.update") return VsmrBridgeAction::AlertsUpdate;
		if (type == "settings.update") return VsmrBridgeAction::SettingsUpdate;
		if (type == "datalink.state.request") return VsmrBridgeAction::DatalinkStateRequest;
		if (type == "datalink.settings.update") return VsmrBridgeAction::DatalinkSettingsUpdate;
		if (type == "datalink.connection.connect") return VsmrBridgeAction::DatalinkConnect;
		if (type == "datalink.connection.disconnect") return VsmrBridgeAction::DatalinkDisconnect;
		if (type == "datalink.poll") return VsmrBridgeAction::DatalinkPoll;
		if (type == "cdm.scan") return VsmrBridgeAction::CdmScan;
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
		if (!profiles.IsArray())
		{
			error = "Profiles state must be an array.";
			return false;
		}

		std::set<std::string> names;
		size_t profileCount = 0;
		for (rapidjson::SizeType i = 0; i < profiles.Size(); ++i)
		{
			const rapidjson::Value& item = profiles[i];
			if (!IsProfileEntry(item))
				continue;
			++profileCount;
			const std::string name = LowerAscii(TrimAscii(item["name"].GetString()));
			if (!names.insert(name).second)
			{
				error = "Profile names must be unique.";
				return false;
			}
		}
		if (profileCount == 0)
		{
			error = "At least one named profile is required.";
			return false;
		}
		return true;
	}

	bool RestoreBackupFileAtomically(const std::string& destination)
	{
		if (destination.empty())
			return false;

		const std::string backupPath = destination + ".bak";
		const DWORD backupAttributes = ::GetFileAttributesA(backupPath.c_str());
		if (backupAttributes == INVALID_FILE_ATTRIBUTES ||
			(backupAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			return false;

		std::string temporaryPath;
		for (int attempt = 0; attempt < 128; ++attempt)
		{
			std::ostringstream candidate;
			candidate << destination
				<< ".rollback."
				<< ::GetCurrentProcessId()
				<< "."
				<< ::GetTickCount()
				<< "."
				<< attempt;
			temporaryPath = candidate.str();
			if (::CopyFileA(backupPath.c_str(), temporaryPath.c_str(), TRUE))
				break;

			const DWORD copyError = ::GetLastError();
			temporaryPath.clear();
			if (copyError != ERROR_FILE_EXISTS &&
				copyError != ERROR_ALREADY_EXISTS)
				return false;
		}
		if (temporaryPath.empty())
			return false;

		HANDLE temporaryFile = ::CreateFileA(
			temporaryPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		const bool flushed =
			temporaryFile != INVALID_HANDLE_VALUE &&
			::FlushFileBuffers(temporaryFile) != FALSE;
		if (temporaryFile != INVALID_HANDLE_VALUE)
			::CloseHandle(temporaryFile);
		if (!flushed ||
			!::MoveFileExA(
				temporaryPath.c_str(),
				destination.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			::DeleteFileA(temporaryPath.c_str());
			return false;
		}
		return true;
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
		const std::string lowered = LowerAscii(TrimAscii(value));
		return lowered.rfind("https://github.com/", 0) == 0 ||
			lowered.rfind("https://www.github.com/", 0) == 0 ||
			lowered.rfind("https://raw.githubusercontent.com/", 0) == 0;
	}

	std::string NormalizeGithubRawUrl(const std::string& value)
	{
		std::string url = TrimAscii(value);
		const std::string lowered = LowerAscii(url);
		const std::string rawPrefix = "https://raw.githubusercontent.com/";
		if (lowered.rfind(rawPrefix, 0) == 0)
			return url;

		const std::string githubPrefix = lowered.rfind("https://www.github.com/", 0) == 0
			? "https://www.github.com/"
			: "https://github.com/";
		if (lowered.rfind(githubPrefix, 0) != 0)
			return "";

		std::string path = url.substr(githubPrefix.size());
		const size_t blob = LowerAscii(path).find("/blob/");
		if (blob == std::string::npos)
			return "";
		const std::string repository = path.substr(0, blob);
		const std::string file = path.substr(blob + 6);
		if (repository.empty() || file.empty())
			return "";
		return rawPrefix + repository + "/" + file;
	}
}

struct VsmrControlCenterBridge::Impl
{
	CSMRRadar* Owner = nullptr;
	VsmrBridgeHostCallbacks Callbacks;
	unsigned long long NativeMessageSequence = 0;

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
		settings.AddMember("approachWindows", true, allocator);

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
		insets.AddMember("srw2", insetVisible(2), allocator);
		insets.AddMember("weather", insetVisible(APPWINDOW_WEATHER - APPWINDOW_BASE), allocator);
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
			for (const auto& runwayEntry : Owner->RimcasInstance->RunwayAreas)
			{
				const std::string& runway = runwayEntry.first;
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
		std::string source;
		rapidjson::Document aviso;
		if (!ReadFileText(path, source) ||
			aviso.Parse<0>(source.c_str()).HasParseError() ||
			!aviso.IsObject() ||
			!aviso.HasMember("features") ||
			!aviso["features"].IsArray())
		{
			aviso.SetObject();
			Allocator& allocator = aviso.GetAllocator();
			AddString(aviso, "type", "FeatureCollection", allocator);
			rapidjson::Value features(rapidjson::kArrayType);
			aviso.AddMember("features", features, allocator);
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
		const std::string& requestId = "")
	{
		if (Owner == nullptr || Owner->CurrentConfig == nullptr)
		{
			SendError(requestId, "vSMR configuration is not available.");
			return;
		}
		const std::string avisoPath =
			Owner->ResolveAvisoGeoJsonPathForAirport(Owner->getActiveAirport());
		if (!avisoPath.empty())
			Owner->EnsureAvisoGeoJsonLoaded(avisoPath);

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
		AddString(payload, "reason", reason, allocator);
		message.AddMember("payload", payload, allocator);
		Send(message);
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

		const rapidjson::Value& stagedState = (*payload)["state"];
		const std::string stagedAirport = TrimAscii(ReadString(stagedState, "airport"));
		if (stagedState.HasMember("aviso") &&
			(stagedAirport.empty() ||
				!EqualsNoCase(stagedAirport, TrimAscii(Owner->getActiveAirport()))))
		{
			error = "The active airport changed while these edits were staged. Reload the Control Center before continuing.";
			return false;
		}
		if (!stagedState.HasMember("profiles"))
		{
			error = "Undo/redo state is missing profiles.";
			return false;
		}
		const rapidjson::Value& profiles = stagedState["profiles"];
		if (!ValidateProfileArray(profiles, error))
			return false;

		std::string activeProfile = ReadString(stagedState, "activeProfile");
		if (activeProfile.empty())
			activeProfile = Owner->GetActiveProfileNameForEditor();

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
		Owner->LoadProfile(activeProfile);

		// LoadProfile's session bookkeeping is intentionally mutable. Restore
		// the exact editor document after the live renderer has consumed it.
		if (!Owner->CurrentConfig->replaceInMemoryConfig(
			profiles,
			activeProfile,
			error))
			return false;

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
				return false;
			}

			const rapidjson::Value& settings = stagedState["settings"];
			if (settings.HasMember("showFps") && settings["showFps"].IsBool())
			{
				Owner->ShowFps = settings["showFps"].GetBool();
				Owner->SaveDataToAsr(
					"ShowFps",
					"Show FPS counter",
					Owner->ShowFps ? "1" : "0");
			}
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
			applyInsetVisibility(2, "srw2");
			applyInsetVisibility(APPWINDOW_WEATHER - APPWINDOW_BASE, "weather");
		}

		if (stagedState.HasMember("aviso"))
		{
			const rapidjson::Value& stagedAviso = stagedState["aviso"];
			if (!stagedAviso.IsObject() ||
				!stagedAviso.HasMember("features") ||
				!stagedAviso["features"].IsArray())
			{
				error = "Undo/redo AVISO state must be a GeoJSON FeatureCollection.";
				return false;
			}

			std::vector<CSMRRadar::AvisoGroup> stagedGroups;
			bool hasStagedGroups = false;
			if (stagedAviso.HasMember("vsmr_groups"))
			{
				if (!stagedAviso["vsmr_groups"].IsArray())
				{
					error = "Undo/redo AVISO groups must be an array.";
					return false;
				}

				hasStagedGroups = true;
				const rapidjson::Value& groupValues = stagedAviso["vsmr_groups"];
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
					group.visible =
						existing != existingVisibility.end()
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

			const std::string avisoPath =
				Owner->ResolveAvisoGeoJsonPathForAirport(Owner->getActiveAirport());
			if (!avisoPath.empty())
				Owner->EnsureAvisoGeoJsonLoaded(avisoPath);
			if (!Owner->ApplyAvisoGroupMembershipSnapshot(stagedAviso, &error))
			{
				if (error.empty())
					error = "Unable to apply undo/redo AVISO group membership.";
				return false;
			}
			if (hasStagedGroups && !Owner->UpdateAvisoGroups(stagedGroups))
			{
				error = "Unable to apply undo/redo AVISO groups.";
				return false;
			}
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
		const std::string& requestId,
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
			avisoModel = std::make_unique<AvisoDocumentModel>();
			std::string loadError;
			if (!avisoModel->LoadFromFile(avisoPath, loadError))
			{
				error = loadError.empty() ? "Unable to load current AVISO data." : loadError;
				return false;
			}
			MergeAvisoPreservingCoordinates(
				avisoModel->MutableDocument(),
				incomingAviso);
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

		CloneJsonValue(
			mergedProfiles,
			Owner->CurrentConfig->document,
			Owner->CurrentConfig->document.GetAllocator());

		bool avisoExistedBeforeSave = false;
		if (avisoModel != nullptr)
		{
			const DWORD attributes = ::GetFileAttributesA(avisoPath.c_str());
			avisoExistedBeforeSave =
				attributes != INVALID_FILE_ATTRIBUTES &&
				(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
			if (!avisoModel->SaveAtomically(avisoPath, error))
			{
				CloneJsonValue(
					previousProfiles,
					Owner->CurrentConfig->document,
					Owner->CurrentConfig->document.GetAllocator());
				if (error.empty())
					error = "Unable to save AVISO GeoJSON atomically.";
				return false;
			}
		}

		if (!Owner->CurrentConfig->saveConfig(profileIdentities))
		{
			CloneJsonValue(
				previousProfiles,
				Owner->CurrentConfig->document,
				Owner->CurrentConfig->document.GetAllocator());
			bool avisoRollbackOk = true;
			if (avisoModel != nullptr)
			{
				if (avisoExistedBeforeSave)
					avisoRollbackOk =
						RestoreBackupFileAtomically(avisoPath);
				else
					avisoRollbackOk =
						::DeleteFileA(avisoPath.c_str()) != FALSE ||
						::GetLastError() == ERROR_FILE_NOT_FOUND;
			}
			error = "Unable to save vSMR_Profiles.json atomically.";
			if (!avisoRollbackOk)
				error += " The AVISO rollback also failed; restore its .bak file before reloading.";
			return false;
		}

		Owner->ReloadConfig();
		if (!activeProfileBefore.empty() &&
			Owner->CurrentConfig->isItActiveProfile(activeProfileBefore) != 0)
			Owner->LoadProfile(activeProfileBefore);
		if (avisoModel != nullptr)
			Owner->ForceReloadAvisoGeoJson();
		Owner->RequestRefresh();
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
		else if (window == "srw2") id = 2;
		else if (!srwOnly && window == "weather") id = APPWINDOW_WEATHER - APPWINDOW_BASE;
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
			const bool configReloaded = Owner->ReloadConfig();
			const bool avisoReloaded = Owner->ForceReloadAvisoGeoJson();
			if (!configReloaded || !avisoReloaded)
			{
				error =
					!configReloaded && !avisoReloaded
						? "Profile/map and AVISO reload failed; the previously loaded data remains active."
						: !configReloaded
							? "Profile or map reload failed; the previously loaded data remains active."
							: "AVISO reload failed; the previously loaded overlay remains active.";
				return false;
			}
			SendAuthoritativeState("reload", envelope.id);
			SendAck(envelope.id, "state.reload", "Configuration reloaded");
			return true;
		}
		case VsmrBridgeAction::StateReset:
			if (!Callbacks.requestResetDefaults)
			{
				error = "Bundled defaults are not available in this host.";
				return false;
			}
			Callbacks.requestResetDefaults(envelope.id);
			return true;
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
			!parsed.HasMember("features") ||
			!parsed["features"].IsArray())
		{
			error = "The selected AVISO file is not a GeoJSON FeatureCollection.";
			return false;
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
	const std::string& jsonText)
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

	rapidjson::Document message;
	MakeEnvelope(message, "resource.loaded", requestId);
	Allocator& allocator = message.GetAllocator();
	rapidjson::Value payload(rapidjson::kObjectType);
	AddString(payload, "resource", normalizedResource, allocator);
	AddString(payload, "source", source, allocator);
	rapidjson::Value data;
	CloneJsonValue(parsed, data, allocator);
	payload.AddMember("data", data, allocator);
	message.AddMember("payload", payload, allocator);
	State->Send(message);
	return true;
}
