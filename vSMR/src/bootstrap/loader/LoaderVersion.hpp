#pragma once

namespace VsmrLoaderVersion
{
	// The loader has an independent, deliberately slow-moving version. Runtime
	// releases may require a minimum loader version without forcing the loader
	// itself to change for every plug-in update.
	constexpr char Value[] = "1.0.0";
}
