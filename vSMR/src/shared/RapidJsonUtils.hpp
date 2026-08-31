#pragma once

#include "rapidjson/document.h"

#include <string>

namespace VsmrRapidJson
{
	using Allocator = rapidjson::Document::AllocatorType;

	inline void AddString(
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

	inline void SetStringMember(
		rapidjson::Value& object,
		const char* key,
		const std::string& value,
		Allocator& allocator)
	{
		if (!object.IsObject() || key == nullptr)
			return;

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

	inline void SetBoolMember(
		rapidjson::Value& object,
		const char* key,
		bool value,
		Allocator& allocator)
	{
		if (!object.IsObject() || key == nullptr)
			return;
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

	inline void CloneJsonValue(
		const rapidjson::Value& source,
		rapidjson::Value& destination,
		Allocator& allocator)
	{
		// The bundled RapidJSON predates CopyFrom, so keep its allocator-aware deep
		// copy in one place instead of serializing and parsing values again.
		if (source.IsObject())
		{
			destination.SetObject();
			for (auto member = source.MemberBegin(); member != source.MemberEnd(); ++member)
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
}
