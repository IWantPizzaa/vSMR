#pragma once

// EuroScope Plugin Bridge ABI v1. This layout follows the public contract at
// https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge/blob/main/include/esbridge.h.
// vSMR resolves it dynamically and never ships or loads the bridge DLL itself.

#include <cstdint>

namespace VsmrPluginBridgeAbi
{
	inline constexpr std::uint32_t AbiVersion = 1U;
	inline constexpr wchar_t ModuleName[] = L"EuroScopeBridge.dll";
	inline constexpr char EntrySymbol[] = "ESB_GetApi";

	using Status = std::int32_t;
	inline constexpr Status Ok = 0;
	inline constexpr Status NoProvider = -1;
	inline constexpr Status NoField = -2;
	inline constexpr Status TypeMismatch = -3;
	inline constexpr Status UnknownAircraft = -6;
	inline constexpr Status StaleAircraft = -7;
	inline constexpr Status BufferTooSmall = -8;
	inline constexpr Status Unset = -9;
	inline constexpr Status Shutdown = -15;

	using Type = std::uint32_t;
	inline constexpr Type String = 4U;

	using Aircraft = std::uint64_t;
	using FieldId = std::uint32_t;
	struct Provider;
	struct Subscription;

	struct Value
	{
		Type type;
		std::uint32_t bytes;
		union
		{
			std::int64_t integer;
			double floatingPoint;
			std::int32_t boolean;
			const void* pointer;
		} data;
	};

	struct FieldDeclaration
	{
		const char* name;
		Type type;
		std::uint32_t scope;
		std::uint32_t flags;
		std::uint32_t maxBytes;
		const char* documentation;
	};

	struct ProviderDeclaration
	{
		std::uint32_t structureSize;
		const char* providerId;
		std::uint32_t schemaMajor;
		std::uint32_t schemaMinor;
		const char* displayName;
		const char* contact;
		const FieldDeclaration* fields;
		std::uint32_t fieldCount;
		void* module;
	};

	using OnChange = void(__cdecl*)(FieldId, Aircraft, void*);

	struct Peer
	{
		char callsign[16];
		std::uint64_t lastSeenMilliseconds;
	};

	struct Origin
	{
		char peer[16];
		std::uint64_t revision;
		std::uint64_t receivedMilliseconds;
	};

	// Every pointer is retained in ABI order, including operations vSMR does
	// not call. Omitting one would shift later entries and corrupt dispatch.
	struct ApiV1
	{
		std::uint32_t structureSize;
		std::uint32_t abiVersion;
		std::uint32_t bridgeBuild;
		std::uint32_t reserved;

		Status(__cdecl* registerProvider)(const ProviderDeclaration*, Provider**);
		Status(__cdecl* unregisterProvider)(Provider*);
		Status(__cdecl* ownField)(Provider*, const char*, FieldId*);

		Status(__cdecl* resolve)(const char*, Type, FieldId*);
		Status(__cdecl* providerVersion)(const char*, std::uint32_t*, std::uint32_t*);
		Status(__cdecl* listProviders)(char*, std::uint32_t*);

		Status(__cdecl* getGlobal)(FieldId, Value*, void*, std::uint32_t*);
		Status(__cdecl* setGlobal)(Provider*, FieldId, const Value*);
		Status(__cdecl* clearGlobal)(Provider*, FieldId);

		Status(__cdecl* aircraft)(const char*, Aircraft*);
		Status(__cdecl* aircraftCallsign)(Aircraft, char*, std::uint32_t*);
		Status(__cdecl* getAircraft)(Aircraft, FieldId, Value*, void*, std::uint32_t*);
		Status(__cdecl* setAircraft)(Provider*, Aircraft, FieldId, const Value*);
		Status(__cdecl* clearAircraft)(Provider*, Aircraft, FieldId);

		std::uint64_t(__cdecl* revision)(FieldId, Aircraft);
		std::uint64_t(__cdecl* providerRevision)(const char*);
		Status(__cdecl* subscribe)(FieldId, OnChange, void*, void*, Subscription**);
		Status(__cdecl* unsubscribe)(Subscription*);

		Status(__cdecl* remotePublishers)(FieldId, Aircraft, Origin*, std::uint32_t*);
		Status(__cdecl* getRemote)(FieldId, Aircraft, const char*, Value*, void*, std::uint32_t*);
		Status(__cdecl* listPeers)(Peer*, std::uint32_t*);

		void(__cdecl* log)(Provider*, std::int32_t, const char*);
		Status(__cdecl* lastError)(char*, std::uint32_t*);
	};

	using GetApiFunction = const ApiV1* (__cdecl*)(std::uint32_t);
}
