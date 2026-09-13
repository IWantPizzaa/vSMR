#pragma once

// Consumer procedures for EuroScope Plugin Bridge ABI v1 (lib/include/esbridge.h),
// following phase B of the bridge's integration checklist. The API table is passed
// in explicitly so the regression tests run these exact procedures against a
// scripted bridge. Every call must happen on the EuroScope main thread (A8).

#include <esbridge.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VsmrPluginBridge
{
	// Hard bound for one STR payload after following ESB_E_BUFFER_TOO_SMALL (B2.7).
	// Providers declare far smaller max_bytes; this only contains a faulty bridge.
	inline constexpr std::uint32_t MaximumStringBytes = 64U * 1024U;

	enum class ReadStatus : std::uint8_t
	{
		Value,        // ESB_OK with the expected type
		Unset,        // ESB_E_UNSET: declared, but no value is held (B2.8)
		NoAircraft,   // unknown to the bridge, or still stale after re-resolving (B2.9)
		ProviderLost, // provider gone or its schema changed: re-resolve (B2.2, B2.3)
		Failed        // anything else: ignore this value and retry on a later tick
	};

	struct FieldSpec
	{
		const char* name;            // without the "provider/" prefix
		ESB_Type type;
		std::uint32_t expectedBytes; // STR: first buffer size, normally the declared max_bytes
	};

	enum class ProviderState : std::uint8_t
	{
		Absent,            // ESB_E_NO_PROVIDER: not installed, or not loaded yet
		UnsupportedSchema, // schema_major is not the one vSMR understands (B2.4)
		NoMatchingFields,  // loaded, but none of the expected fields resolve
		Ready              // at least one field resolved with its expected type
	};

	class ProviderBinding
	{
	public:
		ProviderBinding(
			const char* providerId,
			std::uint32_t schemaMajor,
			const FieldSpec* fields,
			std::size_t fieldCount);

		// Call once per timer tick. Unresolved fields are retried on every tick
		// because the provider may load after vSMR (B2.1).
		ProviderState Refresh(const ESB_Api_v1& api);
		void Reset() noexcept;

		const char* ProviderId() const noexcept { return m_providerId; }
		std::uint32_t SchemaMajor() const noexcept { return m_schemaMajor; }
		std::uint32_t SchemaMinor() const noexcept { return m_schemaMinor; }
		ProviderState State() const noexcept { return m_state; }
		std::size_t FieldCount() const noexcept { return m_fieldCount; }
		const FieldSpec& Spec(std::size_t index) const noexcept { return m_fields[index]; }
		ESB_FieldId Field(std::size_t index) const noexcept { return m_fieldIds[index]; }
		// Last resolve result of an unresolved field: ESB_E_NO_FIELD when this provider
		// version does not declare it, ESB_E_TYPE_MISMATCH when its type changed.
		ESB_Status ResolveStatus(std::size_t index) const noexcept { return m_resolveStatuses[index]; }

	private:
		const char* m_providerId;
		std::uint32_t m_schemaMajor;
		const FieldSpec* m_fields;
		std::size_t m_fieldCount;
		std::vector<std::string> m_qualifiedNames;
		std::vector<ESB_FieldId> m_fieldIds;
		std::vector<ESB_Status> m_resolveStatuses;
		std::uint32_t m_schemaMinor = 0U;
		ProviderState m_state = ProviderState::Absent;
	};

	// Aircraft handles come from the callsign on every scan and never outlive a tick.
	ReadStatus ResolveAircraft(
		const ESB_Api_v1& api,
		const std::string& callsign,
		ESB_Aircraft& aircraft);

	// Aircraft reads re-resolve `aircraft` from `callsign` once on ESB_E_STALE_AIRCRAFT.
	ReadStatus ReadAircraftString(
		const ESB_Api_v1& api,
		const std::string& callsign,
		ESB_Aircraft& aircraft,
		ESB_FieldId field,
		std::uint32_t expectedBytes,
		std::string& value);
	ReadStatus ReadAircraftInteger(
		const ESB_Api_v1& api,
		const std::string& callsign,
		ESB_Aircraft& aircraft,
		ESB_FieldId field,
		std::int64_t& value);
	ReadStatus ReadAircraftBoolean(
		const ESB_Api_v1& api,
		const std::string& callsign,
		ESB_Aircraft& aircraft,
		ESB_FieldId field,
		bool& value);
	ReadStatus ReadGlobalString(
		const ESB_Api_v1& api,
		ESB_FieldId field,
		std::uint32_t expectedBytes,
		std::string& value);
}
