#pragma once

#include "rapidjson/document.h"

#include <string>

namespace VsmrControlCenterUpdater
{
	void BuildStatePayload(
		rapidjson::Value& payload,
		rapidjson::Document::AllocatorType& allocator);

	bool ApplySettings(
		const rapidjson::Value* payload,
		std::string& error);

	bool QueueAction(
		const rapidjson::Value* payload,
		const std::string& requestId,
		std::string& action,
		std::string& error);

	bool OpenRelease(
		const rapidjson::Value* payload,
		std::string& error);
}
