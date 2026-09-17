#include "PluginBridgeTests.hpp"

#include "integrations/PluginBridgeReads.hpp"
#include "integrations/RampAgentBridgeData.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using VsmrPluginBridge::FieldSpec;
	using VsmrPluginBridge::ProviderBinding;
	using VsmrPluginBridge::ProviderState;
	using VsmrPluginBridge::ReadStatus;

	void Check(
		bool condition,
		const char* message,
		std::vector<std::string>& failures)
	{
		if (!condition)
			failures.emplace_back(message);
	}

	constexpr ESB_FieldId StandFieldId = 1U;
	constexpr ESB_FieldId CountFieldId = 2U;
	constexpr ESB_FieldId FlagFieldId = 3U;
	constexpr ESB_FieldId ModeFieldId = 4U;

	// Scripted provider "demo". Status order and size reporting mirror the bridge's
	// Registry and AircraftTable implementation.
	struct FakeBridgeState
	{
		bool providerLive = true;
		std::uint32_t schemaMajor = 1U;
		bool aircraftKnown = true;
		std::uint32_t generation = 1U;
		bool standSet = true;
		std::string stand = "K12";
		std::int64_t count = 42;
		bool flag = true;
		std::string mode;
		int aircraftReads = 0;
		bool scalarBufferPassed = false;
	};

	FakeBridgeState Fake;

	ESB_Aircraft CurrentHandle() noexcept
	{
		return (static_cast<ESB_Aircraft>(Fake.generation) << 32U) | 1U;
	}

	ESB_Status ReadFakeString(
		const std::string& text,
		ESB_Value* out,
		void* buffer,
		std::uint32_t* bytes)
	{
		const std::uint32_t need = static_cast<std::uint32_t>(text.size());
		out->type = ESB_T_STR;
		out->bytes = need;
		out->v.ptr = nullptr;
		if (bytes == nullptr)
			return ESB_E_INVALID_ARG;
		const std::uint32_t have = *bytes;
		*bytes = need;
		if (buffer == nullptr || have < need)
			return ESB_E_BUFFER_TOO_SMALL;
		if (need != 0U)
			std::memcpy(buffer, text.data(), need);
		out->v.ptr = buffer;
		return ESB_OK;
	}

	ESB_Status __cdecl FakeResolve(const char* qualifiedName, ESB_Type expect, ESB_FieldId* out)
	{
		if (qualifiedName == nullptr || out == nullptr)
			return ESB_E_INVALID_ARG;
		if (!Fake.providerLive)
			return ESB_E_NO_PROVIDER;

		struct DeclaredField
		{
			const char* name;
			ESB_Type type;
			ESB_FieldId id;
		};
		static constexpr DeclaredField declared[] = {
			{ "demo/stand", ESB_T_STR, StandFieldId },
			{ "demo/count", ESB_T_I64, CountFieldId },
			{ "demo/flag", ESB_T_BOOL, FlagFieldId },
			{ "demo/mode", ESB_T_STR, ModeFieldId }
		};
		for (const DeclaredField& field : declared)
		{
			if (std::strcmp(qualifiedName, field.name) != 0)
				continue;
			if (expect != 0U && expect != field.type)
				return ESB_E_TYPE_MISMATCH;
			*out = field.id;
			return ESB_OK;
		}
		return ESB_E_NO_FIELD;
	}

	ESB_Status __cdecl FakeProviderVersion(
		const char* providerId,
		std::uint32_t* major,
		std::uint32_t* minor)
	{
		if (providerId == nullptr || major == nullptr || minor == nullptr)
			return ESB_E_INVALID_ARG;
		if (!Fake.providerLive || std::strcmp(providerId, "demo") != 0)
			return ESB_E_NO_PROVIDER;
		*major = Fake.schemaMajor;
		*minor = 2U;
		return ESB_OK;
	}

	ESB_Status __cdecl FakeAircraft(const char* callsign, ESB_Aircraft* out)
	{
		if (callsign == nullptr || out == nullptr)
			return ESB_E_INVALID_ARG;
		if (!Fake.aircraftKnown || std::strcmp(callsign, "AFR123") != 0)
			return ESB_E_UNKNOWN_AIRCRAFT;
		*out = CurrentHandle();
		return ESB_OK;
	}

	ESB_Status __cdecl FakeGetAircraft(
		ESB_Aircraft aircraft,
		ESB_FieldId field,
		ESB_Value* out,
		void* buffer,
		std::uint32_t* bytes)
	{
		++Fake.aircraftReads;
		if (out == nullptr)
			return ESB_E_INVALID_ARG;
		if (aircraft != CurrentHandle())
			return ESB_E_STALE_AIRCRAFT;
		if (field != StandFieldId && field != CountFieldId && field != FlagFieldId)
			return ESB_E_NO_FIELD;
		if (!Fake.providerLive)
			return ESB_E_NO_PROVIDER;
		if (field == StandFieldId)
			return Fake.standSet ? ReadFakeString(Fake.stand, out, buffer, bytes) : ESB_E_UNSET;

		Fake.scalarBufferPassed = Fake.scalarBufferPassed || buffer != nullptr || bytes != nullptr;
		if (field == CountFieldId)
		{
			out->type = ESB_T_I64;
			out->bytes = static_cast<std::uint32_t>(sizeof(std::int64_t));
			out->v.i64 = Fake.count;
			return ESB_OK;
		}
		out->type = ESB_T_BOOL;
		out->bytes = static_cast<std::uint32_t>(sizeof(std::int32_t));
		out->v.b = Fake.flag ? 1 : 0;
		return ESB_OK;
	}

	ESB_Status __cdecl FakeGetGlobal(
		ESB_FieldId field,
		ESB_Value* out,
		void* buffer,
		std::uint32_t* bytes)
	{
		if (out == nullptr)
			return ESB_E_INVALID_ARG;
		if (field != ModeFieldId)
			return ESB_E_NO_FIELD;
		if (!Fake.providerLive)
			return ESB_E_NO_PROVIDER;
		return Fake.mode.empty() ? ESB_E_UNSET : ReadFakeString(Fake.mode, out, buffer, bytes);
	}

	ESB_Api_v1 MakeFakeBridge()
	{
		ESB_Api_v1 api{};
		api.struct_size = static_cast<std::uint32_t>(sizeof(ESB_Api_v1));
		api.abi_version = ESB_ABI_VERSION;
		api.resolve = &FakeResolve;
		api.provider_version = &FakeProviderVersion;
		api.aircraft = &FakeAircraft;
		api.get_ac = &FakeGetAircraft;
		api.get_global = &FakeGetGlobal;
		return api;
	}

	void TestProviderBinding(const ESB_Api_v1& api, std::vector<std::string>& failures)
	{
		enum : std::size_t { Stand, Count, Flag, Mode, Retyped, Undeclared, FieldCount };
		static constexpr FieldSpec fields[FieldCount] = {
			{ "stand", ESB_T_STR, 32U },
			{ "count", ESB_T_I64, 0U },
			{ "flag", ESB_T_BOOL, 0U },
			{ "mode", ESB_T_STR, 64U },
			{ "count", ESB_T_STR, 32U },
			{ "gate", ESB_T_STR, 16U }
		};

		Fake = FakeBridgeState{};
		ProviderBinding provider("demo", 1U, fields, FieldCount);
		Check(
			provider.Refresh(api) == ProviderState::Ready &&
				provider.Field(Stand) == StandFieldId &&
				provider.Field(Count) == CountFieldId &&
				provider.Field(Flag) == FlagFieldId &&
				provider.Field(Mode) == ModeFieldId &&
				provider.SchemaMinor() == 2U,
			"bridge bindings resolve declared fields with their expected types",
			failures);
		Check(
			provider.Field(Retyped) == ESB_FIELD_NONE &&
				provider.ResolveStatus(Retyped) == ESB_E_TYPE_MISMATCH &&
				provider.Field(Undeclared) == ESB_FIELD_NONE &&
				provider.ResolveStatus(Undeclared) == ESB_E_NO_FIELD,
			"a retyped or undeclared bridge field is isolated without disabling its provider",
			failures);

		Fake.providerLive = false;
		Check(
			provider.Refresh(api) == ProviderState::Absent &&
				provider.Field(Stand) == ESB_FIELD_NONE,
			"a missing bridge provider drops resolved fields instead of failing",
			failures);
		Fake.providerLive = true;
		Check(
			provider.Refresh(api) == ProviderState::Ready &&
				provider.Field(Stand) == StandFieldId,
			"a bridge provider that loads later is resolved on a later tick",
			failures);

		Fake.schemaMajor = 2U;
		Check(
			provider.Refresh(api) == ProviderState::UnsupportedSchema &&
				provider.Field(Stand) == ESB_FIELD_NONE,
			"an unsupported bridge schema major is never read",
			failures);
		Fake.schemaMajor = 1U;

		static constexpr FieldSpec undeclaredOnly[] = { { "gate", ESB_T_STR, 16U } };
		ProviderBinding unmatched("demo", 1U, undeclaredOnly, 1U);
		Check(
			unmatched.Refresh(api) == ProviderState::NoMatchingFields,
			"a bridge provider without any expected field is distinguished from an absent one",
			failures);
	}

	void TestReads(const ESB_Api_v1& api, std::vector<std::string>& failures)
	{
		Fake = FakeBridgeState{};
		const std::string callsign = "AFR123";
		ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
		Check(
			VsmrPluginBridge::ResolveAircraft(api, callsign, aircraft) == ReadStatus::Value &&
				aircraft == CurrentHandle(),
			"bridge aircraft handles are resolved from the callsign",
			failures);
		ESB_Aircraft unknownAircraft = ESB_AIRCRAFT_NONE;
		Check(
			VsmrPluginBridge::ResolveAircraft(api, "DLH456", unknownAircraft) == ReadStatus::NoAircraft &&
				unknownAircraft == ESB_AIRCRAFT_NONE,
			"an aircraft the bridge has not seen has no handle",
			failures);

		std::string value;
		Check(
			VsmrPluginBridge::ReadAircraftString(api, callsign, aircraft, StandFieldId, 32U, value) == ReadStatus::Value &&
				value == "K12",
			"bridge strings are copied with their explicit length",
			failures);

		Fake.standSet = false;
		Check(
			VsmrPluginBridge::ReadAircraftString(api, callsign, aircraft, StandFieldId, 32U, value) == ReadStatus::Unset &&
				value.empty(),
			"an unset bridge field is an empty value, not a missing provider",
			failures);
		Fake.standSet = true;

		Fake.stand.assign(300U, 'S');
		Fake.aircraftReads = 0;
		Check(
			VsmrPluginBridge::ReadAircraftString(api, callsign, aircraft, StandFieldId, 32U, value) == ReadStatus::Value &&
				value == Fake.stand &&
				Fake.aircraftReads == 2,
			"a bridge string larger than the buffer is read again at the reported size",
			failures);

		Fake.stand.assign(VsmrPluginBridge::MaximumStringBytes + 1U, 'S');
		Check(
			VsmrPluginBridge::ReadAircraftString(api, callsign, aircraft, StandFieldId, 32U, value) == ReadStatus::Failed &&
				value.empty(),
			"bridge strings beyond the hard size limit are rejected",
			failures);
		Fake.stand = "K12";

		const ESB_Aircraft staleHandle = aircraft;
		++Fake.generation;
		Check(
			VsmrPluginBridge::ReadAircraftString(api, callsign, aircraft, StandFieldId, 32U, value) == ReadStatus::Value &&
				value == "K12" &&
				aircraft != staleHandle &&
				aircraft == CurrentHandle(),
			"a stale bridge aircraft handle is re-resolved from the callsign",
			failures);

		++Fake.generation;
		Fake.aircraftKnown = false;
		Check(
			VsmrPluginBridge::ReadAircraftString(api, callsign, aircraft, StandFieldId, 32U, value) == ReadStatus::NoAircraft &&
				aircraft == ESB_AIRCRAFT_NONE,
			"an aircraft that disconnected behind a stale handle yields no value",
			failures);
		Fake.aircraftKnown = true;
		(void)VsmrPluginBridge::ResolveAircraft(api, callsign, aircraft);

		Fake.providerLive = false;
		Check(
			VsmrPluginBridge::ReadAircraftString(api, callsign, aircraft, StandFieldId, 32U, value) == ReadStatus::ProviderLost,
			"a bridge provider unloaded after resolution is reported for re-resolution",
			failures);
		Fake.providerLive = true;

		std::int64_t count = 0;
		bool flag = false;
		Fake.scalarBufferPassed = false;
		Check(
			VsmrPluginBridge::ReadAircraftInteger(api, callsign, aircraft, CountFieldId, count) == ReadStatus::Value &&
				count == 42 &&
				VsmrPluginBridge::ReadAircraftBoolean(api, callsign, aircraft, FlagFieldId, flag) == ReadStatus::Value &&
				flag &&
				!Fake.scalarBufferPassed,
			"scalar bridge fields are read without a buffer",
			failures);

		std::string mode;
		Check(
			VsmrPluginBridge::ReadGlobalString(api, ModeFieldId, 64U, mode) == ReadStatus::Unset &&
				mode.empty(),
			"an unset global bridge field reads as unset",
			failures);
		Fake.mode = "LFPG=1;";
		Check(
			VsmrPluginBridge::ReadGlobalString(api, ModeFieldId, 64U, mode) == ReadStatus::Value &&
				mode == "LFPG=1;",
			"global bridge strings follow the same read procedure",
			failures);
	}

	void TestRampAgentData(std::vector<std::string>& failures)
	{
		using VsmrRampAgent::NormalizeText;

		Check(
			NormalizeText("  K12 ", VsmrRampAgent::StandMaximumBytes) == "K12",
			"Ramp Agent stands trim surrounding whitespace",
			failures);
		Check(
			NormalizeText(std::string(40U, 'A'), VsmrRampAgent::StandMaximumBytes) == std::string(32U, 'A'),
			"Ramp Agent values are clipped to the declared field size",
			failures);
		Check(
			NormalizeText("ABC\xC3\xA9", 4U) == "ABC" &&
				NormalizeText("Parking \xC3\xA9loign\xC3\xA9", VsmrRampAgent::RemarkMaximumBytes) ==
					"Parking \xC3\xA9loign\xC3\xA9",
			"a character split by publisher clipping is dropped while complete ones are kept",
			failures);
		Check(
			NormalizeText("Gate\t12\r\n", VsmrRampAgent::RemarkMaximumBytes) == "Gate 12" &&
				NormalizeText("A\xFF" "B", VsmrRampAgent::RemarkMaximumBytes) == "A?B" &&
				NormalizeText("\xED\xA0\x80", VsmrRampAgent::RemarkMaximumBytes) == "???",
			"control characters become spaces and invalid UTF-8 becomes '?'",
			failures);
		const char embeddedNull[] = { 'K', '1', '\0', '2' };
		Check(
			NormalizeText(std::string_view(embeddedNull, sizeof(embeddedNull)), VsmrRampAgent::StandMaximumBytes).empty(),
			"Ramp Agent values with embedded nulls are rejected",
			failures);

		VsmrRampAgent::AircraftData data;
		data.remark = "Contact apron";
		Check(
			!VsmrRampAgent::HasPublishedAircraftData(data),
			"a Ramp Agent remark without a stand is not a stand assignment",
			failures);
		data.stand = "K12";
		Check(
			VsmrRampAgent::HasPublishedAircraftData(data),
			"a published Ramp Agent stand identifies an assigned aircraft",
			failures);
	}
}

std::vector<std::string> RunPluginBridgeTests()
{
	std::vector<std::string> failures;
	const ESB_Api_v1 api = MakeFakeBridge();
	TestProviderBinding(api, failures);
	TestReads(api, failures);
	TestRampAgentData(failures);
	RunPluginBridgePollingTests(failures);
	Fake = FakeBridgeState{};
	return failures;
}
