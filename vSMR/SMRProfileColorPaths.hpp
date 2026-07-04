#pragma once

#include "rapidjson/document.h"

#include <cctype>
#include <map>
#include <string>
#include <vector>

struct ProfileColorPathToken
{
	enum class Type { Key, Index } type = Type::Key;
	std::string key;
	rapidjson::SizeType index = 0;
};

inline static char NormalizeProfileColorComponent(char component)
{
	return static_cast<char>(std::tolower(static_cast<unsigned char>(component)));
}

inline static bool IsColorConfigObject(const rapidjson::Value& value, bool* hasAlphaOut = nullptr)
{
	if (!value.IsObject())
		return false;

	auto hasIntMember = [&](const char* key) {
		return value.HasMember(key) && value[key].IsInt();
	};

	if (!hasIntMember("r") || !hasIntMember("g") || !hasIntMember("b"))
		return false;

	bool hasAlpha = value.HasMember("a") && value["a"].IsInt();
	if (value.HasMember("a") && !value["a"].IsInt())
		return false;

	if (hasAlphaOut)
		*hasAlphaOut = hasAlpha;

	return true;
}

inline static bool ShouldExposeProfileColorPath(const std::string& path)
{
	auto startsWith = [&](const char* prefix) -> bool
	{
		const std::string prefixStr = prefix;
		return path.rfind(prefixStr, 0) == 0;
	};

	if (startsWith("labels.departure.status_background_colors."))
		return false;
	if (startsWith("labels.arrival.status_background_colors."))
		return false;

	return true;
}

inline static void CollectProfileColorPaths(
	const rapidjson::Value& value,
	const std::string& path,
	std::vector<std::string>& outPaths,
	std::map<std::string, bool>& outHasAlpha)
{
	bool hasAlpha = false;
	if (!path.empty() && IsColorConfigObject(value, &hasAlpha) && ShouldExposeProfileColorPath(path))
	{
		outPaths.push_back(path);
		outHasAlpha[path] = hasAlpha;
	}

	if (value.IsObject())
	{
		for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
		{
			const std::string key = member->name.GetString();
			const std::string childPath = path.empty() ? key : path + "." + key;
			CollectProfileColorPaths(member->value, childPath, outPaths, outHasAlpha);
		}
		return;
	}

	if (value.IsArray())
	{
		for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
		{
			const std::string childPath = path + "[" + std::to_string(i) + "]";
			CollectProfileColorPaths(value[i], childPath, outPaths, outHasAlpha);
		}
	}
}

inline static std::vector<ProfileColorPathToken> ParseProfileColorPath(const std::string& path)
{
	std::vector<ProfileColorPathToken> tokens;
	if (path.empty())
		return tokens;

	std::string key;
	for (size_t i = 0; i < path.size();)
	{
		const char ch = path[i];

		if (ch == '.')
		{
			if (!key.empty())
			{
				ProfileColorPathToken token;
				token.type = ProfileColorPathToken::Type::Key;
				token.key = key;
				tokens.push_back(token);
				key.clear();
			}
			++i;
			continue;
		}

		if (ch == '[')
		{
			if (!key.empty())
			{
				ProfileColorPathToken token;
				token.type = ProfileColorPathToken::Type::Key;
				token.key = key;
				tokens.push_back(token);
				key.clear();
			}

			const size_t closePos = path.find(']', i + 1);
			if (closePos == std::string::npos || closePos == i + 1)
				return std::vector<ProfileColorPathToken>();

			rapidjson::SizeType index = 0;
			for (size_t j = i + 1; j < closePos; ++j)
			{
				unsigned char digit = static_cast<unsigned char>(path[j]);
				if (!std::isdigit(digit))
					return std::vector<ProfileColorPathToken>();

				index = static_cast<rapidjson::SizeType>(index * 10 + (path[j] - '0'));
			}

			ProfileColorPathToken token;
			token.type = ProfileColorPathToken::Type::Index;
			token.index = index;
			tokens.push_back(token);
			i = closePos + 1;
			continue;
		}

		key.push_back(ch);
		++i;
	}

	if (!key.empty())
	{
		ProfileColorPathToken token;
		token.type = ProfileColorPathToken::Type::Key;
		token.key = key;
		tokens.push_back(token);
	}

	return tokens;
}

inline static rapidjson::Value* ResolveProfilePath(rapidjson::Value& root, const std::vector<ProfileColorPathToken>& tokens)
{
	rapidjson::Value* current = &root;

	for (const auto& token : tokens)
	{
		if (token.type == ProfileColorPathToken::Type::Key)
		{
			if (!current->IsObject())
				return nullptr;

			if (!current->HasMember(token.key.c_str()))
				return nullptr;

			current = &(*current)[token.key.c_str()];
			continue;
		}

		if (!current->IsArray() || token.index >= current->Size())
			return nullptr;

		current = &(*current)[token.index];
	}

	return current;
}

inline static const char* ColorComponentKey(char component)
{
	switch (NormalizeProfileColorComponent(component))
	{
	case 'r': return "r";
	case 'g': return "g";
	case 'b': return "b";
	case 'a': return "a";
	default: return nullptr;
	}
}
