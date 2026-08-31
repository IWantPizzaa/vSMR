#include "platform/windows/PrecompiledHeader.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/Plugin.RuntimeState.hpp"

#include "aircraft/HoldingPoint.hpp"
#include "crash/CrashRuntime.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	bool ContainsCallsign(
		const std::vector<std::string>& collection,
		const std::string& callsign)
	{
		return std::find(collection.begin(), collection.end(), callsign) != collection.end();
	}
}

void CSMRPlugin::OnGetTagItem(
	CFlightPlan FlightPlan,
	CRadarTarget RadarTarget,
	int ItemCode,
	int TagData,
	char sItemString[16],
	int* pColorCode,
	COLORREF* pRGB,
	double* pFontSize)
{
	VsmrCrashRuntime::RecordEuroScopeCallback("CSMRPlugin::OnGetTagItem");
	(void)RadarTarget;
	(void)TagData;
	(void)pFontSize;
	if (Logger::is_verbose_mode())
		Logger::info(string(__FUNCSIG__));
	if (PluginShutdownRequested.load(std::memory_order_relaxed))
	{
		strcpy_s(sItemString, 16, "");
		return;
	}

	if (ItemCode == TAG_ITEM_HOLDING_POINT)
	{
		if (pColorCode != nullptr)
			*pColorCode = TAG_COLOR_DEFAULT;
		strcpy_s(sItemString, 16, "");
		if (!FlightPlan.IsValid())
			return;

		const char* rawRemarks = FlightPlan.GetFlightPlanData().GetRemarks();
		const char* callsign = FlightPlan.GetCallsign();
		const std::string holdingPoint = VsmrHoldingPoint::Resolve(
			callsign != nullptr ? callsign : "",
			rawRemarks != nullptr ? rawRemarks : "");
		// A single space keeps an empty EuroScope list item clickable without
		// displaying the old "HP" placeholder.
		strcpy_s(sItemString, 16, holdingPoint.empty() ? " " : holdingPoint.c_str());
		return;
	}

	if (ItemCode != TAG_ITEM_DATALINK_STS)
		return;

	*pColorCode = TAG_COLOR_RGB_DEFINED;
	*pRGB = RGB(130, 130, 130);
	strcpy_s(sItemString, 16, "-");

	if (!FlightPlan.IsValid())
		return;

	const char* fpCallsign = FlightPlan.GetCallsign();
	if (fpCallsign == nullptr || fpCallsign[0] == '\0')
		return;

	const std::string callsign = fpCallsign;
	bool isDemanding = false;
	bool isStandby = false;
	bool hasMessage = false;
	bool isWilco = false;
	bool isMessageSent = false;
	{
		std::lock_guard<std::mutex> guard(DatalinkStateMutex);
		isDemanding = ContainsCallsign(AircraftDemandingClearance, callsign);
		isStandby = ContainsCallsign(AircraftStandby, callsign);
		hasMessage = ContainsCallsign(AircraftMessage, callsign);
		isWilco = ContainsCallsign(AircraftWilco, callsign);
		isMessageSent = ContainsCallsign(AircraftMessageSent, callsign);
	}

	if (isDemanding)
	{
		if (!BLINK)
			*pRGB = RGB(255, 255, 0);
		strcpy_s(sItemString, 16, isStandby ? "S" : "R");
		return;
	}
	if (hasMessage)
	{
		if (!BLINK)
			*pRGB = RGB(255, 255, 0);
		strcpy_s(sItemString, 16, "T");
		return;
	}
	if (isWilco)
	{
		*pRGB = RGB(0, 176, 0);
		strcpy_s(sItemString, 16, "V");
		return;
	}
	if (isMessageSent)
	{
		*pRGB = RGB(255, 255, 0);
		strcpy_s(sItemString, 16, "V");
	}
}
