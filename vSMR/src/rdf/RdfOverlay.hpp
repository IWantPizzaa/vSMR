#pragma once

#include <EuroScopePlugIn.h>
#include <Windows.h>

#include <cstddef>
#include <functional>

class CSMRPlugin;
class CSMRRadar;

namespace VsmrRdf
{
	struct Status
	{
		bool enabled = false;
		bool trackAudioConnected = false;
		std::size_t activeTransmissionCount = 0;
	};

	using Projector = std::function<POINT(const EuroScopePlugIn::CPosition&)>;

	// Starts the receive-only TrackAudio client. Calls are idempotent and the
	// EuroScope plugin pointer is never used from the network worker.
	void Start(CSMRPlugin* plugin, bool enabled);
	void Stop();
	void OnTimer();
	void SetEnabled(bool enabled);
	Status GetStatus();

	// Resolves callsigns through EuroScope on the caller (UI) thread and draws
	// into the supplied viewport using that viewport's coordinate projector.
	void Draw(
		HDC dc,
		CSMRRadar* radar,
		const RECT& viewport,
		const Projector& projector);
}
