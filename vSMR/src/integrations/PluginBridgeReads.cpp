#include "platform/windows/PrecompiledHeader.hpp"
#include "integrations/PluginBridgeReads.hpp"

#include <algorithm>
#include <array>

namespace
{
	using VsmrPluginBridge::ReadStatus;

	ReadStatus ClassifyStatus(ESB_Status status) noexcept
	{
		switch (status)
		{
		case ESB_OK:
			return ReadStatus::Value;
		case ESB_E_UNSET:
			return ReadStatus::Unset;
		case ESB_E_UNKNOWN_AIRCRAFT:
		case ESB_E_STALE_AIRCRAFT:
			return ReadStatus::NoAircraft;
		case ESB_E_NO_PROVIDER:
		case ESB_E_NO_FIELD:
		case ESB_E_TYPE_MISMATCH:
		case ESB_E_SHUTDOWN:
			return ReadStatus::ProviderLost;
		default:
			return ReadStatus::Failed;
		}
	}

	// Every field vSMR reads today fits the inline buffer. Larger payloads move to
	// the heap, bounded by MaximumStringBytes.
	class PayloadBuffer
	{
	public:
		explicit PayloadBuffer(std::uint32_t expectedBytes)
		{
			if (expectedBytes > m_capacity)
				(void)Grow(expectedBytes);
		}

		PayloadBuffer(const PayloadBuffer&) = delete;
		PayloadBuffer& operator=(const PayloadBuffer&) = delete;

		char* Data() noexcept { return m_data; }
		std::uint32_t Capacity() const noexcept { return m_capacity; }

		bool Grow(std::uint32_t required)
		{
			if (required <= m_capacity || required > VsmrPluginBridge::MaximumStringBytes)
				return false;
			m_heap.assign(required, '\0');
			m_data = m_heap.data();
			m_capacity = required;
			return true;
		}

	private:
		std::array<char, 256U> m_inline{};
		std::vector<char> m_heap;
		char* m_data = m_inline.data();
		std::uint32_t m_capacity = static_cast<std::uint32_t>(m_inline.size());
	};

	template<class Get, class Reresolve>
	ReadStatus ReadStringValue(
		std::uint32_t expectedBytes,
		std::string& value,
		Get&& get,
		Reresolve&& reresolve)
	{
		value.clear();
		PayloadBuffer payload(expectedBytes);
		bool resized = false;
		bool reresolved = false;
		for (;;)
		{
			ESB_Value result{};
			std::uint32_t bytes = payload.Capacity();
			const ESB_Status status = get(&result, payload.Data(), &bytes);
			if (status == ESB_OK)
			{
				// A different type behind a resolved id means the schema changed (B2.3).
				if (result.type != ESB_T_STR)
					return ReadStatus::ProviderLost;
				if (result.bytes > payload.Capacity())
					return ReadStatus::Failed;
				value.assign(payload.Data(), result.bytes);
				return ReadStatus::Value;
			}
			if (status == ESB_E_BUFFER_TOO_SMALL && !resized)
			{
				// B2.7: *io_bytes now holds the size the bridge needs.
				resized = true;
				if (!payload.Grow((std::max)(bytes, result.bytes)))
					return ReadStatus::Failed;
				continue;
			}
			if (status == ESB_E_STALE_AIRCRAFT && !reresolved)
			{
				// B2.9: the handle names an earlier connection of this callsign.
				reresolved = true;
				const ReadStatus resolved = reresolve();
				if (resolved != ReadStatus::Value)
					return resolved;
				continue;
			}
			return ClassifyStatus(status);
		}
	}

	template<class Get, class Reresolve>
	ReadStatus ReadScalarValue(
		ESB_Type type,
		std::uint32_t bytes,
		ESB_Value& value,
		Get&& get,
		Reresolve&& reresolve)
	{
		bool reresolved = false;
		for (;;)
		{
			value = ESB_Value{};
			// Scalars travel inside ESB_Value itself: no buffer and no size (B2.7).
			const ESB_Status status = get(&value, nullptr, nullptr);
			if (status == ESB_OK)
			{
				if (value.type != type)
					return ReadStatus::ProviderLost;
				return value.bytes == bytes ? ReadStatus::Value : ReadStatus::Failed;
			}
			if (status == ESB_E_STALE_AIRCRAFT && !reresolved)
			{
				reresolved = true;
				const ReadStatus resolved = reresolve();
				if (resolved != ReadStatus::Value)
					return resolved;
				continue;
			}
			return ClassifyStatus(status);
		}
	}
}

VsmrPluginBridge::ProviderBinding::ProviderBinding(
	const char* providerId,
	std::uint32_t schemaMajor,
	const FieldSpec* fields,
	std::size_t fieldCount) :
	m_providerId(providerId),
	m_schemaMajor(schemaMajor),
	m_fields(fields),
	m_fieldCount(fieldCount),
	m_fieldIds(fieldCount, ESB_FIELD_NONE),
	m_resolveStatuses(fieldCount, ESB_E_NO_PROVIDER)
{
	m_qualifiedNames.reserve(fieldCount);
	for (std::size_t index = 0U; index < fieldCount; ++index)
		m_qualifiedNames.push_back(std::string(providerId) + "/" + fields[index].name);
}

VsmrPluginBridge::ProviderState VsmrPluginBridge::ProviderBinding::Refresh(const ESB_Api_v1& api)
{
	std::uint32_t major = 0U;
	std::uint32_t minor = 0U;
	const ESB_Status version = api.provider_version != nullptr
		? api.provider_version(m_providerId, &major, &minor)
		: ESB_E_INVALID_ARG;
	if (version != ESB_OK)
	{
		// Not installed, or not loaded yet: keep working without it (B2.2).
		Reset();
		return m_state;
	}
	if (major != m_schemaMajor)
	{
		Reset();
		m_state = ProviderState::UnsupportedSchema;
		return m_state;
	}
	m_schemaMinor = minor;

	bool anyResolved = false;
	for (std::size_t index = 0U; index < m_fieldCount; ++index)
	{
		if (m_fieldIds[index] == ESB_FIELD_NONE && api.resolve != nullptr)
		{
			// Resolving with the expected type catches a schema change here rather
			// than misreading the bytes later (B2.3).
			ESB_FieldId resolved = ESB_FIELD_NONE;
			const ESB_Status status = api.resolve(
				m_qualifiedNames[index].c_str(),
				m_fields[index].type,
				&resolved);
			m_resolveStatuses[index] = status;
			if (status == ESB_OK && resolved != ESB_FIELD_NONE)
				m_fieldIds[index] = resolved;
		}
		anyResolved = anyResolved || m_fieldIds[index] != ESB_FIELD_NONE;
	}
	m_state = anyResolved ? ProviderState::Ready : ProviderState::NoMatchingFields;
	return m_state;
}

void VsmrPluginBridge::ProviderBinding::Reset() noexcept
{
	std::fill(m_fieldIds.begin(), m_fieldIds.end(), ESB_FIELD_NONE);
	std::fill(m_resolveStatuses.begin(), m_resolveStatuses.end(), ESB_E_NO_PROVIDER);
	m_schemaMinor = 0U;
	m_state = ProviderState::Absent;
}

VsmrPluginBridge::ReadStatus VsmrPluginBridge::ResolveAircraft(
	const ESB_Api_v1& api,
	const std::string& callsign,
	ESB_Aircraft& aircraft)
{
	aircraft = ESB_AIRCRAFT_NONE;
	if (api.aircraft == nullptr || callsign.empty())
		return ReadStatus::Failed;

	ESB_Aircraft resolved = ESB_AIRCRAFT_NONE;
	const ESB_Status status = api.aircraft(callsign.c_str(), &resolved);
	if (status == ESB_E_UNKNOWN_AIRCRAFT || status == ESB_E_STALE_AIRCRAFT)
		return ReadStatus::NoAircraft;
	if (status != ESB_OK)
		return ReadStatus::Failed;
	if (resolved == ESB_AIRCRAFT_NONE)
		return ReadStatus::NoAircraft;
	aircraft = resolved;
	return ReadStatus::Value;
}

VsmrPluginBridge::ReadStatus VsmrPluginBridge::ReadAircraftString(
	const ESB_Api_v1& api,
	const std::string& callsign,
	ESB_Aircraft& aircraft,
	ESB_FieldId field,
	std::uint32_t expectedBytes,
	std::string& value)
{
	if (api.get_ac == nullptr || field == ESB_FIELD_NONE)
	{
		value.clear();
		return ReadStatus::Failed;
	}
	return ReadStringValue(
		expectedBytes,
		value,
		[&](ESB_Value* out, void* buffer, std::uint32_t* bytes)
		{
			return api.get_ac(aircraft, field, out, buffer, bytes);
		},
		[&]
		{
			return ResolveAircraft(api, callsign, aircraft);
		});
}

VsmrPluginBridge::ReadStatus VsmrPluginBridge::ReadAircraftInteger(
	const ESB_Api_v1& api,
	const std::string& callsign,
	ESB_Aircraft& aircraft,
	ESB_FieldId field,
	std::int64_t& value)
{
	value = 0;
	if (api.get_ac == nullptr || field == ESB_FIELD_NONE)
		return ReadStatus::Failed;

	ESB_Value result{};
	const ReadStatus status = ReadScalarValue(
		ESB_T_I64,
		static_cast<std::uint32_t>(sizeof(std::int64_t)),
		result,
		[&](ESB_Value* out, void* buffer, std::uint32_t* bytes)
		{
			return api.get_ac(aircraft, field, out, buffer, bytes);
		},
		[&]
		{
			return ResolveAircraft(api, callsign, aircraft);
		});
	if (status == ReadStatus::Value)
		value = result.v.i64;
	return status;
}

VsmrPluginBridge::ReadStatus VsmrPluginBridge::ReadAircraftBoolean(
	const ESB_Api_v1& api,
	const std::string& callsign,
	ESB_Aircraft& aircraft,
	ESB_FieldId field,
	bool& value)
{
	value = false;
	if (api.get_ac == nullptr || field == ESB_FIELD_NONE)
		return ReadStatus::Failed;

	ESB_Value result{};
	const ReadStatus status = ReadScalarValue(
		ESB_T_BOOL,
		static_cast<std::uint32_t>(sizeof(std::int32_t)),
		result,
		[&](ESB_Value* out, void* buffer, std::uint32_t* bytes)
		{
			return api.get_ac(aircraft, field, out, buffer, bytes);
		},
		[&]
		{
			return ResolveAircraft(api, callsign, aircraft);
		});
	if (status == ReadStatus::Value)
		value = result.v.b != 0;
	return status;
}

VsmrPluginBridge::ReadStatus VsmrPluginBridge::ReadGlobalString(
	const ESB_Api_v1& api,
	ESB_FieldId field,
	std::uint32_t expectedBytes,
	std::string& value)
{
	if (api.get_global == nullptr || field == ESB_FIELD_NONE)
	{
		value.clear();
		return ReadStatus::Failed;
	}
	return ReadStringValue(
		expectedBytes,
		value,
		[&](ESB_Value* out, void* buffer, std::uint32_t* bytes)
		{
			return api.get_global(field, out, buffer, bytes);
		},
		[]
		{
			// Global values have no aircraft handle to go stale.
			return ReadStatus::Failed;
		});
}
