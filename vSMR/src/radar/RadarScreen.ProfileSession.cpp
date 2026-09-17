#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"

void CSMRRadar::RestoreActiveProfileFromAsr()
{
	if (CurrentConfig == nullptr)
		return;
	const char* saved = GetDataFromAsr("ActiveProfile");
	LoadProfile(saved != nullptr ? saved : "Default", false);
	SaveActiveProfileToAsr();
}

void CSMRRadar::SaveActiveProfileToAsr()
{
	const std::string name = CurrentConfig != nullptr
		? CurrentConfig->getActiveProfileName() : "Default";
	SaveDataToAsr("ActiveProfile", "vSMR active profile", name.c_str());
}
