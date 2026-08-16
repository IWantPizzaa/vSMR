// Defines the application and EuroScope plug-in entry points.

#include "platform/windows/PrecompiledHeader.hpp"
#include "app/PluginEntry.hpp"
#include "crash/CrashReporter.hpp"
#include "crash/CrashRuntime.hpp"


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

//---EuroScopePlugInInit-----------------------------------------------

void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState())

	const bool crashReporterInstalled = VsmrCrashReporter::Install(MY_PLUGIN_VERSION);
	VsmrCrashRuntime::RecordEuroScopeCallback("EuroScopePlugInInit");
	try
	{
		// create the instance
		*ppPlugInInstance = theApp.gpMyPlugin = new CSMRPlugin();
		Logger::info(crashReporterInstalled
			? "Crash reporter active path=" + VsmrCrashReporter::GetReportDirectory()
			: "Crash reporter could not be initialized");
	}
	catch (...)
	{
		VsmrCrashReporter::Remove();
		throw;
	}
}
