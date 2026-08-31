#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterBridge.hpp"
#include "control_center/ControlCenterBridge.Internal.hpp"

#include "control_center/ControlCenterMessageProtocol.hpp"
#include "control_center/WebMessageValidation.hpp"

#include "rapidjson/document.h"

#include <utility>

using VsmrControlCenterProtocol::DecodedEnvelope;
using VsmrControlCenterProtocol::DecodeEnvelope;

VsmrControlCenterBridge::VsmrControlCenterBridge(
	CSMRRadar* owner,
	VsmrBridgeHostCallbacks callbacks)
	: State(std::make_unique<VsmrControlCenterBridgeImpl>(owner, std::move(callbacks)))
{
}

VsmrControlCenterBridge::~VsmrControlCenterBridge() = default;

void VsmrControlCenterBridge::SetOwner(CSMRRadar* owner)
{
	State->Owner = owner;
}

bool VsmrControlCenterBridge::HandleWebMessage(const std::string& messageJson)
{
	if (messageJson.empty() ||
		messageJson.size() > VsmrWebMessageValidation::MaximumInboundMessageBytes)
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
