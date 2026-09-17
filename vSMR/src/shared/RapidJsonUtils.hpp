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
		destination.CopyFrom(source, allocator, true);
	}
}
