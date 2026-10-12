#pragma once
#include "insets/TimerCountdownState.hpp"
#include "shared/ProcessSync.hpp"
#include <new>

// Same-computer session state. No files, network messages, passwords or FPL writes.
// All operations are on the EuroScope UI thread; deadlines use machine uptime.
class ProxyTimerState final
{
public:
    ~ProxyTimerState() { Close(); }
    ProxyTimerState() = default;
    ProxyTimerState(const ProxyTimerState&) = delete;
    ProxyTimerState& operator=(const ProxyTimerState&) = delete;
    bool Join(const std::wstring& identity)
    {
        if (identity == identity_ && data_) return true;
        Close();
        if (identity.empty()) return false;
        if (!mutex_.Open(L"Timers", identity) || !mutex_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(mutex_);
        mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(Shared), VsmrProcessSync::Name(L"TimerState", identity).c_str());
        if (!mapping_) return false;
        const bool created = ::GetLastError() != ERROR_ALREADY_EXISTS;
        data_ = static_cast<Shared*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!data_) return false;
        if (created) ::new (data_) Shared{};
        if (data_->schema != 1 || data_->size != sizeof(Shared)) return false;
        identity_ = identity;
        return true;
    }
    void Close()
    {
        if (data_) ::UnmapViewOfFile(data_);
        if (mapping_) ::CloseHandle(mapping_);
        data_ = nullptr; mapping_ = nullptr; identity_.clear(); mutex_.Close();
    }
    bool Active() const { return !identity_.empty() && data_; }
    bool Change(int minutes, bool reset, std::uint64_t now, TimerCountdownState& snapshot)
    {
        if (!Active() || !mutex_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(mutex_);
        const bool changed = reset ? data_->timers.Reset(minutes) : data_->timers.Start(minutes, now);
        snapshot = data_->timers;
        return changed;
    }
    bool Poll(std::uint64_t now, TimerCountdownState& snapshot, bool& alarm)
    {
        alarm = false;
        if (!Active() || !mutex_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(mutex_);
        alarm = data_->timers.Update(now); // one process claims each expiration
        snapshot = data_->timers;
        return true;
    }
private:
    struct Shared
    {
        std::uint32_t schema = 1;
        std::uint32_t size = sizeof(Shared);
        TimerCountdownState timers;
    };
    std::wstring identity_;
    HANDLE mapping_ = nullptr;
    Shared* data_ = nullptr;
    VsmrProcessSync::Mutex mutex_;
};
