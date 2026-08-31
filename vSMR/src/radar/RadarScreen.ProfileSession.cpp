#include "platform/windows/PrecompiledHeader.hpp"
#include "radar/RadarScreen.hpp"
#include "shared/TextUtils.hpp"

namespace
{
	std::mutex gSessionActiveProfileMutex;
	std::string gSessionActiveProfileName;
}

void CSMRRadar::RememberSessionActiveProfile(const std::string& profileName)
{
	const std::string trimmed = TrimAsciiWhitespaceCopy(profileName);
	if (trimmed.empty())
		return;

	std::lock_guard<std::mutex> guard(gSessionActiveProfileMutex);
	gSessionActiveProfileName = trimmed;
}

std::string CSMRRadar::GetSessionActiveProfile(const std::string& fallbackProfile)
{
	std::lock_guard<std::mutex> guard(gSessionActiveProfileMutex);
	if (!gSessionActiveProfileName.empty())
		return gSessionActiveProfileName;
	return fallbackProfile;
}

std::string CSMRRadar::ReadLastActiveProfileFromConfig() const
{
	if (CurrentConfig == nullptr)
		return "";
	return TrimAsciiWhitespaceCopy(CurrentConfig->getLastActiveProfileName());
}

void CSMRRadar::WriteLastActiveProfileToConfig(const std::string& profileName) const
{
	const std::string trimmedName = TrimAsciiWhitespaceCopy(profileName);
	if (trimmedName.empty() || CurrentConfig == nullptr)
		return;

	if (CurrentConfig->setLastActiveProfileName(trimmedName))
		CurrentConfig->saveConfig();
}
