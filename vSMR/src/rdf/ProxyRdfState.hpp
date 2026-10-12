#pragma once
#include "shared/ProcessSync.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <vector>

// One local TrackAudio source, one publisher; readers may reset pending calls.
// Never put EuroScope objects or STL allocations in cross-process memory.
class ProxyRdfState final
{
public:
    ProxyRdfState() = default;
    ProxyRdfState(const ProxyRdfState&) = delete;
    ProxyRdfState& operator=(const ProxyRdfState&) = delete;
    ~ProxyRdfState() { if (data_) ::UnmapViewOfFile(data_); if (mapping_) ::CloseHandle(mapping_); }
    bool Open(const std::wstring& identity = L"TrackAudio-127.0.0.1")
    {
        if (data_) return true;
        identity_ = identity;
        VsmrProcessSync::Mutex mutex;
        if (!Lock(mutex)) return false;
        mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(Shared), VsmrProcessSync::Name(L"RdfState", identity_).c_str());
        if (!mapping_) return false;
        const bool created = ::GetLastError() != ERROR_ALREADY_EXISTS;
        data_ = static_cast<Shared*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!data_) { ::CloseHandle(mapping_); mapping_ = nullptr; return false; }
        if (created) new (data_) Shared{};
        if (data_->schema != 1 || data_->size != sizeof(Shared)) {
            ::UnmapViewOfFile(data_); ::CloseHandle(mapping_); data_ = nullptr; mapping_ = nullptr; return false;
        }
        return true;
    }
    bool Active() const { return data_ != nullptr; }
    const std::wstring& Identity() const { return identity_; }
    bool BecomePublisher()
    {
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex)) return false;
        data_->publisher = ::GetCurrentProcessId(); data_->connected = false;
        for (auto& record : data_->records) record.count = 0; // retain pending acknowledgements on failover
        ++data_->revision; return true;
    }
    void SetConnected(bool connected)
    {
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex) || data_->publisher != ::GetCurrentProcessId()) return;
        data_->connected = connected;
        if (!connected) for (auto& record : data_->records) record.count = 0;
        ++data_->revision;
    }
    bool Connected() const
    {
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex) || !data_->connected || !data_->publisher) return false;
        HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, data_->publisher);
        if (!process) return false;
        const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        ::CloseHandle(process); return alive;
    }
    bool Event(const std::string& callsign, std::int64_t frequency, bool begin)
    {
        if (callsign.empty() || callsign.size() > MaximumCallsign || frequency < 0) return false;
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex) || data_->publisher != ::GetCurrentProcessId()) return false;
        auto* record = Find(callsign);
        if (!record && begin) {
            record = &*std::min_element(data_->records.begin(), data_->records.end(), [](const auto& a, const auto& b) {
                if ((a.count == 0) != (b.count == 0)) return a.count == 0;
                return a.time < b.time;
            });
            if (record->count != 0) return false; // never evict a live speaker
            *record = Record{};
            std::memcpy(record->callsign.data(), callsign.data(), callsign.size());
        }
        if (!record || record->count > MaximumFrequencies) return false;
        auto first = record->frequencies.begin(), last = first + record->count;
        const auto found = std::find(first, last, frequency);
        if (begin) {
            if (found != last || record->count == MaximumFrequencies) return false;
            if (!record->count) record->pending = true;
            record->frequencies[record->count++] = frequency;
        } else {
            if (found == last) return false;
            *found = record->frequencies[--record->count];
        }
        record->time = ::GetTickCount64(); ++data_->revision; return true;
    }
    bool Pending(const std::string& callsign) const
    {
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex)) return false;
        const auto* record = Find(callsign); return record && record->pending;
    }
    bool Reset(const std::string& callsign)
    {
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex)) return false;
        auto* record = Find(callsign);
        if (!record || !record->pending) return false;
        record->pending = false; ++data_->revision; return true; // never clears reception
    }
    void Forget(const std::string& callsign)
    {
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex) || data_->publisher != ::GetCurrentProcessId()) return;
        if (auto* record = Find(callsign)) { *record = Record{}; ++data_->revision; }
    }
    std::vector<std::string> PendingCalls() const
    {
        std::vector<std::string> result;
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex)) return result;
        for (const auto& record : data_->records) if (record.pending)
            result.emplace_back(record.callsign.data(), strnlen_s(record.callsign.data(), record.callsign.size()));
        return result;
    }
    std::uint64_t Revision() const
    {
        VsmrProcessSync::Mutex mutex;
        return data_ && Lock(mutex) ? data_->revision : 0;
    }
    bool IsPublisher() const
    {
        VsmrProcessSync::Mutex mutex;
        return data_ && Lock(mutex) && data_->publisher == ::GetCurrentProcessId();
    }
    std::vector<std::string> Transmitting() const
    {
        std::vector<std::string> result;
        VsmrProcessSync::Mutex mutex;
        if (!data_ || !Lock(mutex) || !data_->connected) return result;
        for (const auto& record : data_->records) if (record.count)
            result.emplace_back(record.callsign.data(), strnlen_s(record.callsign.data(), record.callsign.size()));
        return result;
    }
private:
    static constexpr std::size_t MaximumCallsign = 64;
    static constexpr std::uint32_t MaximumFrequencies = 16;
    struct Record {
        std::array<char, MaximumCallsign + 1> callsign{};
        std::array<std::int64_t, MaximumFrequencies> frequencies{};
        std::uint32_t count = 0;
        bool pending = false;
        std::uint64_t time = 0;
    };
    struct Shared {
        std::uint32_t schema = 1;
        std::uint32_t size = sizeof(Shared);
        DWORD publisher = 0;
        bool connected = false;
        std::uint64_t revision = 0;
        std::array<Record, 1024> records{};
    };
    bool Lock(VsmrProcessSync::Mutex& mutex) const
    { return mutex.Open(L"RdfLock", identity_) && mutex.Lock(100); }
    Record* Find(const std::string& callsign) const
    {
        for (auto& record : data_->records)
            if (callsign.size() <= MaximumCallsign && std::strncmp(record.callsign.data(), callsign.c_str(), record.callsign.size()) == 0)
                return &record;
        return nullptr;
    }
    std::wstring identity_;
    HANDLE mapping_ = nullptr;
    Shared* data_ = nullptr;
};
