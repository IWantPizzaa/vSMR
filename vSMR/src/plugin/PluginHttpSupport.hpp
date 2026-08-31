#pragma once

class HttpHelper;

namespace VsmrPluginRuntime
{
	// Datalink and weather requests intentionally share one HTTP client.
	HttpHelper& GetHttpHelper();
}
