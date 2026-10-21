#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

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
			{
				Pending.insert(callsign);
				PendingFrequencies[callsign] = { frequency };
			}
			else if (Pending.count(callsign) != 0)
			{
				// A second receiver hears the same call. Keep both frequencies
				// until acknowledgement, even if one receiver ends first.
				PendingFrequencies[callsign].insert(frequency);
			}
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
		bool IsPending(const std::string& callsign, const std::set<FrequencyHz>& receivedFrequencies) const
		{
			const auto found = PendingFrequencies.find(callsign);
			return found != PendingFrequencies.end() && Intersects(found->second, receivedFrequencies);
		}
		bool Acknowledge(const std::string& callsign)
		{
			PendingFrequencies.erase(callsign);
			return Pending.erase(callsign) != 0;
		}
		const ActiveMap& ActiveCalls() const { return Active; }
		const std::set<std::string>& PendingCalls() const { return Pending; }
		std::vector<std::string> PendingCalls(const std::set<FrequencyHz>& receivedFrequencies) const
		{
			std::vector<std::string> result;
			for (const auto& entry : PendingFrequencies)
				if (Intersects(entry.second, receivedFrequencies)) result.push_back(entry.first);
			return result;
		}
		std::vector<std::string> Transmitting(const std::set<FrequencyHz>& receivedFrequencies) const
		{
			std::vector<std::string> result;
			for (const auto& entry : Active)
				if (Intersects(entry.second, receivedFrequencies)) result.push_back(entry.first);
			return result;
		}
		void ClearPending() { Pending.clear(); PendingFrequencies.clear(); }
		void ClearActive() { Active.clear(); }
		void Clear() { ClearActive(); ClearPending(); }
		void Forget(const std::string& callsign) { Active.erase(callsign); Acknowledge(callsign); }

	private:
		static bool Intersects(const std::set<FrequencyHz>& first, const std::set<FrequencyHz>& second)
		{
			for (const FrequencyHz frequency : first)
				if (second.count(frequency) != 0) return true;
			return false;
		}
		ActiveMap Active;
		std::set<std::string> Pending;
		std::map<std::string, std::set<FrequencyHz>> PendingFrequencies;
	};
}
