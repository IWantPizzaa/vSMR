#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "config/ProfileNormalization.hpp"

void CSMRRadar::EnsureTargetGroundStatusColorEntries(bool persistChanges)
{
	if (!CurrentConfig || CurrentConfig->getProfileCount() == 0) return;
	const bool changed = VsmrProfile::Normalize(CurrentConfig->getMutableActiveProfile(),
		CurrentConfig->document.GetAllocator());
	// A validated backup or a migrated read-only source may be active in memory.
	// Normalize that working copy for runtime use, but leave recovery to the
	// explicit Settings flow instead of showing a spurious startup save error.
	if (changed && persistChanges && CurrentConfig->isConfigHealthy() && !CurrentConfig->saveConfig())
	{
		GetPlugIn()->DisplayUserMessage("vSMR", "Config", "Failed to save status settings to vSMR_Profiles.json", true, true, false, false, false);
	}
}
