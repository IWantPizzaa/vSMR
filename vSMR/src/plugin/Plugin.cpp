#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "radar/RadarScreen.Registry.hpp"

#include <atomic>
#include <string>
#include <vector>

std::atomic<bool> Logger::ENABLED{ false };
std::string Logger::DLL_PATH;
std::atomic<Logger::Mode> Logger::CURRENT_MODE{ Logger::Mode::Normal };

std::atomic<bool> PluginShutdownRequested(false);
std::atomic<CSMRPlugin*> ActivePluginInstance{ nullptr };
std::atomic<bool> FlightDataRefreshPending(false);
bool BLINK = false;

std::vector<CSMRRadar*> RadarScreensOpened;
