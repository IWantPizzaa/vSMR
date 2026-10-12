#pragma once
#include "shared/ProcessSync.hpp"
#include <algorithm>
#include <cstring>
#include <new>

// Bounded same-machine transport, not an arbitrary command/FPL relay. A single
// publisher owns the channel on the UI thread; readers cannot publish its data.
class ProxySessionChannel final
{
public:
    static constexpr std::size_t MaximumPayload = 4U * 1024U * 1024U;
    static constexpr ULONGLONG FreshnessMs = 5000;
    struct Command { std::uint64_t id = 0; int action = 0; std::string airport; };
    ~ProxySessionChannel() { Close(); }
    ProxySessionChannel() = default;
    ProxySessionChannel(const ProxySessionChannel&) = delete;
    ProxySessionChannel& operator=(const ProxySessionChannel&) = delete;
    bool Open(const std::wstring& identity, const wchar_t* purpose = L"Bridge")
    {
        if (identity == identity_ && purpose_ == purpose && data_) return true;
        Close();
        if (identity.empty()) return false;
        purpose_ = purpose;
        const auto lockName = purpose_ + L"Lock", ownerName = purpose_ + L"Publisher";
        if (!lock_.Open(lockName.c_str(), identity) || !publisher_.Open(ownerName.c_str(), identity) || !lock_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(lock_);
        mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(Shared)), VsmrProcessSync::Name(purpose, identity).c_str());
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
        if (Active() && publisher_.Owned() && lock_.Lock()) {
            data_->pid = 0; data_->bytes = 0; ++data_->revision;
            lock_.Unlock();
        }
        if (data_) ::UnmapViewOfFile(data_);
        if (mapping_) ::CloseHandle(mapping_);
        data_ = nullptr; mapping_ = nullptr; identity_.clear(); purpose_.clear();
        lock_.Close(); publisher_.Close();
    }
    bool Active() const { return data_ && !identity_.empty(); }
    bool IsPublisher() const { return Active() && publisher_.Owned(); }
    bool Publish(const std::string& payload)
    {
        if (!Active() || payload.size() > MaximumPayload || !publisher_.Lock(0) || !lock_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(lock_);
        const bool changedOwner = data_->pid != ::GetCurrentProcessId();
        if (changedOwner && (data_->commandState == 1 || data_->commandState == 2)) {
            data_->commandState = 3; data_->success = false;
            strcpy_s(data_->error, "Primary instance changed; command was not retried. Verify its state before clicking again.");
        }
        if (changedOwner || data_->bytes != payload.size() ||
            std::memcmp(data_->payload, payload.data(), payload.size()) != 0) {
            std::memcpy(data_->payload, payload.data(), payload.size());
            data_->bytes = static_cast<std::uint32_t>(payload.size()); ++data_->revision;
        }
        data_->pid = ::GetCurrentProcessId(); data_->heartbeat = ::GetTickCount64();
        return true;
    }
    bool Read(std::string& payload, std::uint64_t& revision)
    {
        if (!Active() || !lock_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(lock_);
        if (!Fresh() || data_->bytes > MaximumPayload) return false;
        payload.assign(data_->payload, data_->bytes); revision = data_->revision;
        return true;
    }
    bool Submit(int action, const std::string& airport, std::uint64_t& id, std::string& error)
    {
        if (action < 0 || action > 14 || (airport.size() != 4 && !airport.empty()) ||
            !std::all_of(airport.begin(), airport.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); })) {
            error = "Invalid Proxy vSID action or airport."; return false;
        }
        if (!Active() || !lock_.Lock()) { error = "Proxy command channel unavailable."; return false; }
        VsmrProcessSync::ScopedUnlock unlock(lock_);
        if (!Fresh() || data_->pid == ::GetCurrentProcessId()) { error = "Primary instance is not available."; return false; }
        if ((data_->commandState == 1 || data_->commandState == 2) && ::GetTickCount64() - data_->commandTime < FreshnessMs) {
            error = "Another Proxy vSID action is being processed."; return false;
        }
        id = ++data_->nextCommand;
        data_->commandId = id; data_->sender = ::GetCurrentProcessId(); data_->action = action;
        strcpy_s(data_->airport, airport.c_str()); data_->commandTime = ::GetTickCount64(); data_->commandState = 1;
        return true;
    }
    bool Take(Command& command)
    {
        if (!IsPublisher() || !lock_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(lock_);
        if (data_->pid != ::GetCurrentProcessId() || data_->commandState != 1) return false;
        if (::GetTickCount64() - data_->commandTime >= FreshnessMs) {
            data_->commandState = 3; data_->success = false; strcpy_s(data_->error, "Proxy command expired without execution."); return false;
        }
        command = {data_->commandId, data_->action, data_->airport};
        data_->commandState = 2; // at most once, including when the owner exits
        return true;
    }
    void Complete(std::uint64_t id, bool success, const std::string& error)
    {
        if (!IsPublisher() || !lock_.Lock()) return;
        VsmrProcessSync::ScopedUnlock unlock(lock_);
        if (data_->commandId != id || data_->commandState != 2 || data_->pid != ::GetCurrentProcessId()) return;
        data_->commandState = 3; data_->success = success;
        strncpy_s(data_->error, error.c_str(), _TRUNCATE);
    }
    bool Result(std::uint64_t id, bool& success, std::string& error)
    {
        if (!Active() || !lock_.Lock()) return false;
        VsmrProcessSync::ScopedUnlock unlock(lock_);
        if (data_->commandId != id || data_->sender != ::GetCurrentProcessId()) {
            success = false; error = "Proxy command result unavailable; verify vSID state. Command was not retried."; return true;
        }
        if (data_->commandState == 3) { success = data_->success; error = data_->error; return true; }
        if (!Fresh() || ::GetTickCount64() - data_->commandTime >= FreshnessMs) {
            success = false; error = "Primary did not confirm the Proxy action; verify vSID state. Command was not retried."; return true;
        }
        return false;
    }
private:
    bool Fresh() const
    {
        if (!data_->pid || !data_->bytes || ::GetTickCount64() - data_->heartbeat >= FreshnessMs) return false;
        const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, data_->pid);
        if (!process) return false;
        const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        ::CloseHandle(process); return alive;
    }
    struct Shared
    {
        std::uint32_t schema = 1, size = sizeof(Shared), pid = 0, bytes = 0;
        std::uint64_t heartbeat = 0, revision = 0, nextCommand = 0, commandId = 0, commandTime = 0;
        DWORD sender = 0;
        int commandState = 0, action = 0;
        bool success = false;
        char airport[5]{}, error[256]{}, payload[MaximumPayload]{};
    };
    std::wstring identity_, purpose_;
    HANDLE mapping_ = nullptr;
    Shared* data_ = nullptr;
    VsmrProcessSync::Mutex lock_, publisher_;
};
