#pragma once

#include "rapidjson/document.h"

// Configuration layers are deliberately not JSON Merge Patch: null is a value,
// never a deletion instruction. Arrays are indivisible values.
namespace VsmrLayeredConfig
{
	using Allocator = rapidjson::Document::AllocatorType;
	using Value = rapidjson::Value;

	inline const Value* Member(const Value& object, const char* name)
	{
		if (!object.IsObject()) return nullptr;
		const auto found = object.FindMember(name);
		return found == object.MemberEnd() ? nullptr : &found->value;
	}

	inline Value* Member(Value& object, const char* name)
	{
		if (!object.IsObject()) return nullptr;
		const auto found = object.FindMember(name);
		return found == object.MemberEnd() ? nullptr : &found->value;
	}

	inline void Put(Value& object, const char* name, const Value& value, Allocator& allocator)
	{
		if (!object.IsObject()) object.SetObject();
		if (Value* existing = Member(object, name)) existing->CopyFrom(value, allocator);
		else
		{
			Value key;
			key.SetString(name, allocator);
			Value copy;
			copy.CopyFrom(value, allocator);
			object.AddMember(key, copy, allocator);
		}
	}

	inline void Merge(Value& destination, const Value& overrides, Allocator& allocator)
	{
		if (!destination.IsObject() || !overrides.IsObject())
		{
			destination.CopyFrom(overrides, allocator);
			return;
		}
		for (auto item = overrides.MemberBegin(); item != overrides.MemberEnd(); ++item)
		{
			Value* target = Member(destination, item->name.GetString());
			if (target != nullptr) Merge(*target, item->value, allocator);
			else Put(destination, item->name.GetString(), item->value, allocator);
		}
	}

	// Produces an override relative to a known matching baseline. Callers decide
	// whether absent keys mean inheritance or a domain-specific hidden item.
	inline void Difference(const Value* defaults, const Value& edited, Value& result, Allocator& allocator)
	{
		if (edited.IsObject() && (defaults == nullptr || defaults->IsObject()))
		{
			result.SetObject();
			for (auto item = edited.MemberBegin(); item != edited.MemberEnd(); ++item)
			{
				const Value* base = defaults ? Member(*defaults, item->name.GetString()) : nullptr;
				if (base != nullptr && *base == item->value) continue;
				Value child;
				Difference(base, item->value, child, allocator);
				Put(result, item->name.GetString(), child, allocator);
			}
		}
		else result.CopyFrom(edited, allocator);
	}

	// Applies only changes made since the previous effective snapshot. Existing
	// untouched overrides remain pinned, even when a release happens to make a
	// default equal to them. Removing an edited key resets that key to inheritance.
	inline void ApplyEdits(const Value* defaults, const Value* previous, const Value& edited,
		Value& overrides, Allocator& allocator)
	{
		if (previous != nullptr && *previous == edited) return;
		if (!edited.IsObject() || (defaults != nullptr && !defaults->IsObject()) ||
			(previous != nullptr && !previous->IsObject()))
		{
			overrides.CopyFrom(edited, allocator);
			return;
		}
		if (!overrides.IsObject()) overrides.SetObject();
		if (previous != nullptr && previous->IsObject())
		{
			for (auto item = previous->MemberBegin(); item != previous->MemberEnd(); ++item)
				if (!edited.HasMember(item->name)) overrides.RemoveMember(item->name);
		}
		for (auto item = edited.MemberBegin(); item != edited.MemberEnd(); ++item)
		{
			const char* key = item->name.GetString();
			const Value* before = previous ? Member(*previous, key) : nullptr;
			if (before != nullptr && *before == item->value) continue;
			const Value* base = defaults ? Member(*defaults, key) : nullptr;
			if (base != nullptr && *base == item->value)
			{
				overrides.RemoveMember(key);
				continue;
			}
			Value patch;
			if (const Value* saved = Member(overrides, key)) patch.CopyFrom(*saved, allocator);
			else patch.SetObject();
			ApplyEdits(base, before, item->value, patch, allocator);
			if (patch.IsObject() && patch.ObjectEmpty() && base != nullptr && base->IsObject())
				overrides.RemoveMember(key);
			else Put(overrides, key, patch, allocator);
		}
	}
}
