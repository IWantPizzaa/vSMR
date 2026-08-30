#pragma once

namespace VsmrLoaderLifecycle
{
	enum class RuntimeReleaseResult
	{
		Released,
		RuntimeRetained,
		ModuleUnloadFailed
	};

	template <typename Module, typename Shutdown, typename Plugin, typename UnloadModule>
	RuntimeReleaseResult TryReleasePublishedRuntime(
		Module& module,
		Shutdown& shutdown,
		Plugin& plugin,
		UnloadModule&& unloadModule) noexcept
	{
		bool runtimeReleased =
			!static_cast<bool>(shutdown) &&
			!static_cast<bool>(plugin);
		if (static_cast<bool>(shutdown))
		{
			try
			{
				runtimeReleased = shutdown();
			}
			catch (...)
			{
				runtimeReleased = false;
			}
		}

		if (!runtimeReleased)
			return RuntimeReleaseResult::RuntimeRetained;

		// The runtime shutdown completed, so no published object or callable may
		// remain even if Windows temporarily refuses to unmap the inert module.
		plugin = Plugin{};
		shutdown = Shutdown{};
		if (static_cast<bool>(module))
		{
			bool unloaded = false;
			try
			{
				unloaded = unloadModule(module);
			}
			catch (...)
			{
				unloaded = false;
			}
			if (!unloaded)
				return RuntimeReleaseResult::ModuleUnloadFailed;
		}

		module = Module{};
		return RuntimeReleaseResult::Released;
	}
}
