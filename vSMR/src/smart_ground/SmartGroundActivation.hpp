#pragma once

#include <atomic>
#include <string>

namespace VsmrSmartGround
{
    enum class ActivationCommand { Unrecognized, Toggle, Enable, Disable, Status };

    // The caller supplies the trimmed, case-normalized EuroScope command.
    inline ActivationCommand ParseActivationCommand(const std::string& command)
    {
        for (const char* prefix : { ".smr sg", ".smr smart ground" })
        {
            if (command == prefix) return ActivationCommand::Toggle;
            if (command == std::string(prefix) + " on") return ActivationCommand::Enable;
            if (command == std::string(prefix) + " off") return ActivationCommand::Disable;
            if (command == std::string(prefix) + " status") return ActivationCommand::Status;
        }
        return ActivationCommand::Unrecognized;
    }

    // Session-only: never restore this switch from settings or an ASR file.
    class Activation
    {
    public:
        bool IsEnabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
        bool SetEnabled(bool enabled) noexcept { return enabled_.exchange(enabled, std::memory_order_acq_rel) != enabled; }
    private:
        std::atomic<bool> enabled_{ false };
    };
}
