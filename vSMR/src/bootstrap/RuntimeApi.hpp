#pragma once

#include <cstddef>
#include <cstdint>

namespace EuroScopePlugIn
{
	class CPlugIn;
}

namespace VsmrRuntimeApi
{
	constexpr std::uint32_t AbiVersion = 1U;

	// This is deliberately a plain C-compatible structure. The loader owns the
	// pointed-to strings and keeps them alive for the lifetime of the runtime.
	struct BootstrapContext
	{
		std::uint32_t structureSize;
		std::uint32_t abiVersion;
		const wchar_t* loaderPath;
		const wchar_t* installRoot;
		const wchar_t* dataRoot;
		const wchar_t* canonicalRuntimePath;
		const wchar_t* loadedRuntimePath;
	};

	using GetAbiVersionFunction = std::uint32_t(__cdecl*)() noexcept;
	using CreateFunction = bool(__cdecl*)(
		const BootstrapContext* context,
		EuroScopePlugIn::CPlugIn** pluginInstance,
		char* errorBuffer,
		std::size_t errorBufferSize) noexcept;
	// Returns true only when every runtime object has been released and the
	// loader may safely unload the runtime DLL. A false result asks the loader
	// to retain the module for the remainder of the process.
	using ShutdownFunction = bool(__cdecl*)() noexcept;
}
