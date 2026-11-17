#pragma once

#include <map>
#include <string>

namespace VsmrSmartGroundList
{
    inline std::string DisplayRoute(const std::string& route, bool enabled = true)
    {
        if (!enabled) return "No Data";
        if (route.empty()) return " "; // Empty EuroScope list cells remain clickable.
        if (route.size() <= 15) return route;
        const auto separator = route.rfind(' ', 12);
        const auto length = separator == std::string::npos ? 12 : separator;
        return route.substr(0, length) + "...";
    }

    enum class Action { Edit, Commit, Reset, Close };
    struct PendingAction { std::string callsign; Action action = Action::Close; };

    // Callback IDs are never reused in a plugin instance. A cancelled/expired
    // popup cannot accidentally target a later selected aircraft.
    class Actions
    {
    public:
        int Add(const std::string& callsign, Action action)
        {
            if (callsign.empty() || next_ >= 700000000) return 0;
            const int id = next_++;
            pending_.emplace(id, PendingAction{ callsign, action });
            while (pending_.size() > 128) pending_.erase(pending_.begin());
            return id;
        }
        bool Take(int id, PendingAction& result)
        {
            const auto found = pending_.find(id);
            if (found == pending_.end()) return false;
            result = found->second; pending_.erase(found); return true;
        }
        void Forget(const std::string& callsign)
        {
            for (auto item = pending_.begin(); item != pending_.end();)
                if (item->second.callsign == callsign) item = pending_.erase(item);
                else ++item;
        }
        void Clear() { pending_.clear(); }
    private:
        int next_ = 700000;
        std::map<int, PendingAction> pending_;
    };
}
