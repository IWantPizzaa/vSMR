#pragma once

#include "control_center/ControlCenterBridge.hpp"

#include "rapidjson/document.h"

#include <string>

namespace VsmrControlCenterProtocol
{
	constexpr int Version = 1;

	struct DecodedEnvelope
	{
		int version = 0;
		std::string id;
		std::string type;
		VsmrBridgeAction action = VsmrBridgeAction::Unknown;
		const rapidjson::Value* payload = nullptr;
	};

	VsmrBridgeAction ActionFromType(const std::string& requestedType);
	bool DecodeEnvelope(
		const rapidjson::Document& document,
		DecodedEnvelope& envelope,
		std::string& error);
	void MakeEnvelope(
		rapidjson::Document& document,
		const std::string& type,
		const std::string& requestId);
}
