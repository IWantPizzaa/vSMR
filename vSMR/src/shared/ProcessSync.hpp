#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>
#include <sstream>
#include <cstdint>

namespace VsmrProcessSync
{
    inline std::wstring Name(const wchar_t* purpose, const std::wstring& identity)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (wchar_t c : identity) { hash ^= static_cast<std::uint16_t>(c); hash *= 1099511628211ULL; }
        std::wostringstream name;
        name << L"Local\\vSMR." << purpose << L".v1." << std::hex << hash;
        return name.str();
    }

    inline std::wstring PathIdentity(const std::filesystem::path& path)
    {
        std::error_code error;
        auto canonical = std::filesystem::weakly_canonical(path, error);
        if (error) {
            error.clear(); canonical = std::filesystem::absolute(path, error);
            canonical = (error ? path : canonical).lexically_normal();
        }
        auto result = canonical.wstring();
        if (!result.empty()) ::CharUpperBuffW(result.data(), static_cast<DWORD>(result.size()));
        return result;
    }

    class Mutex final
    {
    public:
        Mutex() = default;
        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;
        ~Mutex() { Close(); }
        bool Open(const wchar_t* purpose, const std::wstring& identity)
        {
            Close();
            handle_ = ::CreateMutexW(nullptr, FALSE, Name(purpose, identity).c_str());
            return handle_ != nullptr;
        }
        bool Lock(DWORD timeout = 100)
        {
            if (!handle_ || owned_) return owned_;
            const DWORD result = ::WaitForSingleObject(handle_, timeout);
            owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            return owned_;
        }
        void Unlock()
        {
            if (owned_) { ::ReleaseMutex(handle_); owned_ = false; }
        }
        void Close()
        {
            Unlock();
            if (handle_) ::CloseHandle(handle_);
            handle_ = nullptr;
        }
        bool Owned() const { return owned_; }
    private:
        HANDLE handle_ = nullptr;
        bool owned_ = false;
    };

    class ScopedUnlock final
    {
    public:
        explicit ScopedUnlock(Mutex& mutex) : mutex_(mutex) {}
        ~ScopedUnlock() { mutex_.Unlock(); }
        ScopedUnlock(const ScopedUnlock&) = delete;
        ScopedUnlock& operator=(const ScopedUnlock&) = delete;
    private:
        Mutex& mutex_;
    };

    class PathMutex final
    {
    public:
        explicit PathMutex(const std::string& path)
        {
            locked_ = mutex_.Open(L"Config", PathIdentity(std::filesystem::u8path(path))) && mutex_.Lock(1000);
        }
        explicit operator bool() const { return locked_; }
    private:
        Mutex mutex_;
        bool locked_ = false;
    };
}
