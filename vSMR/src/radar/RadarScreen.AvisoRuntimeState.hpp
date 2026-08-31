#pragma once

#include <windows.h>

#include <map>
#include <vector>

class CSMRRadar;

// AVISO window and hook state is defined alongside the radar window procedures.
extern std::map<HWND, std::vector<CSMRRadar*>> gInsetWindowRadarScreens;
extern HHOOK gThreadMouseHook;
extern HHOOK gThreadKeyboardHook;

UINT AvisoWorkerRefreshMessage();
