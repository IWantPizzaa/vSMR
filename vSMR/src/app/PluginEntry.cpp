// Defines the application and the stable loader/runtime ABI entry points.

#include "platform/windows/PrecompiledHeader.hpp"
#include "app/PluginEntry.hpp"
#include "bootstrap/RuntimeApi.hpp"
#include "bootstrap/RuntimeContext.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"

#include <cstring>
#include <exception>


#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// CvSMRApp

BEGIN_MESSAGE_MAP(CvSMRApp, CWinApp)
END_MESSAGE_MAP()


// CvSMRApp construction

CvSMRApp::CvSMRApp()
{

}


// The one and only CvSMRApp object

CvSMRApp theApp;


// CvSMRApp initialization

BOOL CvSMRApp::InitInstance()
{
	CWinApp::InitInstance();

	return TRUE;
}

namespace
{
	void CopyRuntimeError(
		char* destination,
		std::size_t destinationSize,
		const char* message) noexcept
	{
		if (destination == nullptr || destinationSize == 0)
			return;
		strncpy_s(
			destination,
			destinationSize,
			message != nullptr ? message : "Unknown runtime initialization error.",
			_TRUNCATE);
	}
}

extern "C" std::uint32_t __declspec(dllexport) VsmrRuntimeGetAbiVersion() noexcept
{
	return VsmrRuntimeApi::AbiVersion;
}

extern "C" bool __declspec(dllexport) VsmrRuntimeCreate(
	const VsmrRuntimeApi::BootstrapContext* context,
	EuroScopePlugIn::CPlugIn** pluginInstance,
	char* errorBuffer,
	std::size_t errorBufferSize) noexcept
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState())

	if (errorBuffer != nullptr && errorBufferSize > 0)
		errorBuffer[0] = '\0';
	if (pluginInstance == nullptr)
	{
		CopyRuntimeError(errorBuffer, errorBufferSize, "The EuroScope plug-in output pointer is null.");
		return false;
	}
	*pluginInstance = nullptr;
	if (context == nullptr || !VsmrRuntimeContext::Configure(*context))
	{
		CopyRuntimeError(errorBuffer, errorBufferSize, "The loader supplied an invalid runtime context.");
		return false;
	}

	if (theApp.gpMyPlugin != nullptr)
	{
		*pluginInstance = theApp.gpMyPlugin;
		return true;
	}

	const bool crashReporterInstalled = VsmrCrashReporter::Install(
		MY_PLUGIN_VERSION,
		VsmrRuntimeContext::InstallRoot().c_str());
	VsmrCrashRuntime::RecordEuroScopeCallback("EuroScopePlugInInit");
	try
	{
		CSMRPlugin* const createdPlugin = new CSMRPlugin();
		theApp.gpMyPlugin = createdPlugin;
		*pluginInstance = createdPlugin;
		try
		{
			Logger::info(crashReporterInstalled
				? "Crash reporter active path=" + VsmrCrashReporter::GetReportDirectory()
				: "Crash reporter could not be initialized");
		}
		catch (...)
		{
			// Diagnostics must not turn a successfully constructed runtime into a
			// failed activation that the loader could subsequently unload.
		}
		return true;
	}
	catch (const std::exception& exception)
	{
		theApp.gpMyPlugin = nullptr;
		VsmrCrashReporter::Remove();
		CopyRuntimeError(errorBuffer, errorBufferSize, exception.what());
		return false;
	}
	catch (...)
	{
		theApp.gpMyPlugin = nullptr;
		VsmrCrashReporter::Remove();
		CopyRuntimeError(errorBuffer, errorBufferSize, "vSMR runtime initialization failed unexpectedly.");
		return false;
	}
}

extern "C" bool __declspec(dllexport) VsmrRuntimeShutdown() noexcept
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState())

	try
	{
		if (!VsmrShutdownPlugin())
			return false;
	}
	catch (...)
	{
		// An exception must never cross the loader/runtime ABI during host exit.
		return false;
	}
	theApp.gpMyPlugin = nullptr;
	return true;
}
