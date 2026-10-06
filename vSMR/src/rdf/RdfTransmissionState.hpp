#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace VsmrRdf
{
	// The owner serializes access. Reception and acknowledgement are separate:
	// clearing a list indication must never end a live RDF transmission.
	class TransmissionState
	{
	public:
		using FrequencyHz = std::int64_t;
		using ActiveMap = std::map<std::string, std::set<FrequencyHz>>;

		bool Begin(const std::string& callsign, FrequencyHz frequency)
		{
			auto& frequencies = Active[callsign];
			const bool newCall = frequencies.empty();
			if (!frequencies.insert(frequency).second)
				return false;
			if (newCall)
				Pending.insert(callsign);
			return true;
		}

		bool End(const std::string& callsign, FrequencyHz frequency)
		{
			const auto found = Active.find(callsign);
			if (found == Active.end() || found->second.erase(frequency) == 0)
				return false;
			if (found->second.empty())
				Active.erase(found);
			return true;
		}

		bool IsPending(const std::string& callsign) const { return Pending.count(callsign) != 0; }
		bool Acknowledge(const std::string& callsign) { return Pending.erase(callsign) != 0; }
		const ActiveMap& ActiveCalls() const { return Active; }
		const std::set<std::string>& PendingCalls() const { return Pending; }
		void ClearPending() { Pending.clear(); }
		void ClearActive() { Active.clear(); }
		void Clear() { ClearActive(); ClearPending(); }
		void Forget(const std::string& callsign) { Active.erase(callsign); Pending.erase(callsign); }

	private:
		ActiveMap Active;
		std::set<std::string> Pending;
	};
}
