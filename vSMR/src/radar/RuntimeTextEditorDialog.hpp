#pragma once

#include <string>

namespace VsmrRuntimeTextEditor
{
	enum class Kind
	{
		Airport,
		Preset,
		PdcLogon
	};

	// Returns false for Cancel or if the dialog could not be created.
	bool Show(Kind kind, const std::string& initialValue, bool dayTheme,
		std::string& acceptedValue);
}
