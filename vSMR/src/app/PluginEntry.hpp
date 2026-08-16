// Main application declarations for vSMR.dll.

#pragma once
#include "EuroScopePlugIn.h"
#include "plugin/Plugin.hpp"

#ifndef __AFXWIN_H__
	#error "include 'platform/windows/PrecompiledHeader.hpp' before including this file for PCH"
#endif

#include "platform/windows/ResourceIds.h"


// CvSMRApp
// See app/PluginEntry.cpp for the implementation of this class.
//

class CvSMRApp : public CWinApp
{
public:
	CvSMRApp();

// Overrides
public:
	virtual BOOL InitInstance();
	CSMRPlugin * gpMyPlugin = NULL;
	DECLARE_MESSAGE_MAP()
};
